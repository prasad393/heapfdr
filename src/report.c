/* SPDX-License-Identifier: MIT */
/* Reports: heap misuse as it happens, and whatever is left at exit. */
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "heapfdr.h"

#define UNKNOWN_FREES_SHOWN 3

/* Keeps reports from two threads from interleaving. If both locks are
 * needed, this one comes first: demangling a name inside a report can
 * allocate, which takes the ledger lock. */
static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
static int unknown_frees_shown;

/* A thread that was reporting during fork() does not exist in the child. */
void report_child_after_fork(void)
{
    pthread_mutex_init(&report_lock, NULL);
}

static const char *call_name(int op)
{
    return op == OP_REALLOC ? "realloc" : "free";
}

static void begin(struct fdr_out *o, const char *title)
{
    pthread_mutex_lock(&report_lock);
    out_open(o, STDERR_FILENO);
    stack_forget_modules();
    out_tag(o);
    out_str(o, "======== ");
    out_str(o, title);
    out_str(o, " ========");
    out_nl(o);
}

static void end(struct fdr_out *o)
{
    stack_print_modules(o);
    pthread_mutex_unlock(&report_lock);
}

static void heading(struct fdr_out *o, const char *text)
{
    out_tag(o);
    out_str(o, "  ");
    out_str(o, text);
    out_nl(o);
}

static void call_line(struct fdr_out *o, int op, uintptr_t addr)
{
    out_tag(o);
    out_str(o, call_name(op));
    out_chr(o, '(');
    out_hex(o, addr, 1);
    out_str(o, "): ");
}

static void outcome_line(struct fdr_out *o, int op)
{
    out_tag(o);
    if (fdr_cfg.abort_on_error)
        out_str(o, "stopping here because HEAPFDR_ABORT=1");
    else if (op == OP_REALLOC)
        out_str(o, "realloc() returned NULL instead of touching the heap");
    else
        out_str(o, "the call was skipped so the program can keep running");
    out_nl(o);
}

void report_double_free(uintptr_t addr, const struct fdr_freed *prev,
                        const uintptr_t *frames, int depth, int op)
{
    struct fdr_out o;

    begin(&o, op == OP_REALLOC ? "REALLOC OF FREED BLOCK" : "DOUBLE FREE");
    call_line(&o, op, addr);
    out_str(&o, "this ");
    out_size(&o, prev->size);
    out_str(&o, " block was already freed ");
    out_ms(&o, fdr_now_ns() - prev->when_ns);
    out_str(&o, " earlier");
    out_nl(&o);

    heading(&o, op == OP_REALLOC ? "realloc() called here:" : "freed again here:");
    stack_print(&o, frames, depth, 0);
    heading(&o, "first freed here:");
    stack_print_site(&o, prev->free_site);
    heading(&o, "allocated here:");
    stack_print_site(&o, prev->alloc_site);
    outcome_line(&o, op);
    end(&o);
}

void report_bad_free(uintptr_t addr, const struct fdr_block *owner,
                     const uintptr_t *frames, int depth, int op)
{
    struct fdr_out o;

    begin(&o, "INVALID FREE");
    call_line(&o, op, addr);
    out_str(&o, "pointer is ");
    out_udec(&o, addr - owner->addr);
    out_str(&o, " bytes inside a live ");
    out_size(&o, owner->size);
    out_str(&o, " block, not at its start");
    out_nl(&o);

    heading(&o, "called from:");
    stack_print(&o, frames, depth, 0);
    heading(&o, "the block it points into was allocated here:");
    stack_print_site(&o, owner->site);
    outcome_line(&o, op);
    end(&o);
}

void report_unknown_free(uintptr_t addr, const uintptr_t *frames, int depth, int op)
{
    struct fdr_out o;

    if (__atomic_fetch_add(&unknown_frees_shown, 1, __ATOMIC_RELAXED) >= UNKNOWN_FREES_SHOWN)
        return;

    begin(&o, "UNKNOWN POINTER");
    call_line(&o, op, addr);
    out_str(&o, "heapfdr never saw this pointer allocated; passing it to glibc");
    out_nl(&o);
    heading(&o, "called from:");
    stack_print(&o, frames, depth, 0);
    end(&o);
}

/* -------------------------------------------------------- leak report */

struct leak_row {
    uint32_t site;
    uint64_t bytes;
    uint64_t blocks;
};

struct leak_totals {
    uint64_t bytes, blocks, sites;
    uint64_t hidden_bytes, hidden_blocks;
};

/* Insertion sort, largest first. Leak lists are short, and qsort() may
 * allocate, which is the one thing we can't do here. */
static void sort_rows(struct leak_row *rows, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        struct leak_row r = rows[i];
        uint32_t j = i;
        while (j > 0 && rows[j - 1].bytes < r.bytes) {
            rows[j] = rows[j - 1];
            j--;
        }
        rows[j] = r;
    }
}

/* Snapshot the per-site counters under the lock. Blocks allocated before
 * main() are left out unless HEAPFDR_STARTUP=1 (or main() was never seen). */
static uint32_t collect(struct leak_row *rows, size_t cap, struct leak_totals *t)
{
    uint32_t n = 0, nsites;
    int with_boot = fdr_cfg.include_startup || !fdr_saw_main;

    ledger_lock();
    nsites = ledger_site_count();
    for (uint32_t i = 0; i < nsites && n < cap; i++) {
        const struct fdr_site *s = ledger_site_at(i);
        uint64_t bytes = s->live_bytes, blocks = s->live_blocks;

        if (with_boot) {
            bytes += s->boot_bytes;
            blocks += s->boot_blocks;
        } else {
            t->hidden_bytes += s->boot_bytes;
            t->hidden_blocks += s->boot_blocks;
        }
        if (blocks) {
            rows[n].site = i;
            rows[n].bytes = bytes;
            rows[n].blocks = blocks;
            n++;
        }
    }
    ledger_unlock();
    return n;
}

/* Drop runtime noise and suppressed sites. Runs without the ledger lock
 * because it calls dladdr(). */
static uint32_t filter(struct leak_row *rows, uint32_t n, struct leak_totals *t)
{
    uint32_t kept = 0;

    for (uint32_t i = 0; i < n; i++) {
        const struct fdr_site *s = ledger_site_at(rows[i].site);
        int depth = (int)s->depth;

        if (rows[i].site != 0 &&
            (stack_is_runtime_noise(s->frames, depth) ||
             stack_matches(s->frames, depth, fdr_cfg.suppress))) {
            t->hidden_bytes += rows[i].bytes;
            t->hidden_blocks += rows[i].blocks;
            continue;
        }
        t->bytes += rows[i].bytes;
        t->blocks += rows[i].blocks;
        t->sites++;
        rows[kept++] = rows[i];
    }
    return kept;
}

static void print_totals(struct fdr_out *o, const struct fdr_stats *st)
{
    out_tag(o);
    out_str(o, "totals: ");
    out_count(o, st->allocs, "allocation");
    out_str(o, ", ");
    out_count(o, st->frees, "free");
    out_str(o, ", peak ");
    out_size(o, st->peak_bytes);
    out_str(o, " in use, ");
    out_count(o, st->errors, "error");
    out_nl(o);
}

void report_leaks(void)
{
    struct leak_totals t = { 0 };
    struct fdr_stats st;
    struct fdr_out o;
    struct leak_row *rows;
    size_t rows_bytes = (size_t)ledger_site_count() * sizeof *rows + 4096;
    uint32_t n;
    int show;

    rows = mmap(NULL, rows_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (rows == MAP_FAILED)
        return;

    n = collect(rows, rows_bytes / sizeof *rows, &t);
    n = filter(rows, n, &t);
    sort_rows(rows, n);
    ledger_stats(&st);

    pthread_mutex_lock(&report_lock);
    out_open(&o, STDERR_FILENO);
    stack_forget_modules();

    out_tag(&o);
    out_str(&o, "======== LEAK REPORT (pid ");
    out_udec(&o, (unsigned long long)getpid());
    out_str(&o, ") ========");
    out_nl(&o);

    if (t.blocks == 0) {
        out_tag(&o);
        out_str(&o, fdr_saw_main ? "no leaks: everything allocated by the program was freed"
                                 : "no leaks: everything allocated was freed");
        out_nl(&o);
    } else {
        out_tag(&o);
        out_count(&o, t.blocks, "block");
        out_str(&o, " (");
        out_size(&o, t.bytes);
        out_str(&o, ") never freed, from ");
        out_count(&o, t.sites, "call site");
        out_nl(&o);

        show = (int)n < fdr_cfg.top_sites ? (int)n : fdr_cfg.top_sites;
        for (int i = 0; i < show; i++) {
            const struct fdr_site *s = ledger_site_at(rows[i].site);

            out_tag(&o);
            out_nl(&o);
            out_tag(&o);
            out_str(&o, "leak ");
            out_udec(&o, (unsigned)i + 1);
            out_str(&o, " of ");
            out_udec(&o, n);
            out_str(&o, ": ");
            out_size(&o, rows[i].bytes);
            out_str(&o, " in ");
            out_count(&o, rows[i].blocks, "block");
            out_str(&o, " (this site allocated ");
            out_count(&o, s->allocs, "time");
            out_str(&o, ")");
            out_nl(&o);
            stack_print_site(&o, rows[i].site);
        }
        if ((int)n > show) {
            uint64_t rest = 0;
            for (uint32_t i = (uint32_t)show; i < n; i++)
                rest += rows[i].bytes;
            out_tag(&o);
            out_nl(&o);
            out_tag(&o);
            out_str(&o, "... and ");
            out_count(&o, n - (uint32_t)show, "more site");
            out_str(&o, " (");
            out_size(&o, rest);
            out_str(&o, "), raise HEAPFDR_TOP to see them");
            out_nl(&o);
        }
        out_tag(&o);
        out_nl(&o);
    }

    if (t.hidden_blocks) {
        out_tag(&o);
        out_str(&o, "not counted: ");
        out_count(&o, t.hidden_blocks, "block");
        out_str(&o, " (");
        out_size(&o, t.hidden_bytes);
        out_str(&o, ") held by the C runtime, startup code or suppressed sites");
        out_nl(&o);
    }
    if (st.unknown_frees) {
        out_tag(&o);
        out_count(&o, st.unknown_frees, "free");
        out_str(&o, " of pointers heapfdr never saw allocated");
        out_nl(&o);
    }
    print_totals(&o, &st);
    stack_print_modules(&o);
    pthread_mutex_unlock(&report_lock);

    munmap(rows, rows_bytes);

    if (t.blocks && fdr_cfg.leak_exit_code) {
        fflush(NULL);
        _exit(fdr_cfg.leak_exit_code);
    }
}
