/* SPDX-License-Identifier: MIT */
/*
 * Fatal signal handling.
 *
 * The handler runs on a separate stack so a stack overflow can still be
 * reported, and all output goes through write(2). backtrace() and dladdr()
 * are not on the async-signal-safe list: backtrace() is warmed up at
 * startup so it won't allocate here, and in practice both are fine for a
 * last-words report. The case where they can hang is a crash inside the
 * dynamic loader while it holds its own lock.
 */
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "heapfdr.h"

#define ALT_STACK_BYTES (128 * 1024)
#define CRASH_DEPTH     32
#define CRASH_EVENTS    16

static const int fatal_signals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
#define NFATAL ((int)(sizeof fatal_signals / sizeof fatal_signals[0]))

static struct sigaction previous[NFATAL];
volatile int fdr_crashing;
static char log_path[320];

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGFPE:  return "SIGFPE";
    case SIGILL:  return "SIGILL";
    case SIGABRT: return "SIGABRT";
    default:      return "signal";
    }
}

static const char *signal_reason(int sig, int code)
{
    if (sig == SIGABRT)
        return "abort() was called";
    if (code == SI_USER)
        return "sent by kill()";
    if (code == SI_TKILL)
        return "sent by raise() or tgkill()";

    switch (sig) {
    case SIGSEGV:
        if (code == SEGV_MAPERR) return "address not mapped";
        if (code == SEGV_ACCERR) return "no permission for this access";
        break;
    case SIGBUS:
        if (code == BUS_ADRALN) return "misaligned address";
        if (code == BUS_ADRERR) return "address does not exist";
        break;
    case SIGFPE:
        if (code == FPE_INTDIV) return "integer divide by zero";
        if (code == FPE_INTOVF) return "integer overflow";
        if (code == FPE_FLTDIV) return "floating point divide by zero";
        break;
    case SIGILL:
        if (code == ILL_ILLOPC) return "illegal opcode";
        break;
    }
    return "unknown cause";
}

static void read_registers(const ucontext_t *uc, uintptr_t *pc, uintptr_t *sp, uintptr_t *fp)
{
    *pc = *sp = *fp = 0;
    if (!uc)
        return;
#if defined(__x86_64__)
    *pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    *sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    *fp = (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
#elif defined(__aarch64__)
    *pc = (uintptr_t)uc->uc_mcontext.pc;
    *sp = (uintptr_t)uc->uc_mcontext.sp;
    *fp = (uintptr_t)uc->uc_mcontext.regs[29];
#endif
}

/* ------------------------------------------------- report sections */

static void line_start(struct fdr_out *o, const char *label)
{
    out_tag(o);
    out_str(o, label);
    out_pad(o, sizeof FDR_TAG + 8);
}

static void print_hint(struct fdr_out *o, int sig, uintptr_t addr, uintptr_t sp)
{
    struct fdr_freed f;
    uintptr_t gap = addr > sp ? addr - sp : sp - addr;

    if (sig != SIGSEGV && sig != SIGBUS)
        return;

    if (addr < 4096) {
        line_start(o, "hint");
        out_str(o, "address is near NULL, most likely a NULL pointer dereference");
        out_nl(o);
    } else if (sp && gap < 64 * 1024) {
        line_start(o, "hint");
        out_str(o, "address is right next to the stack pointer, most likely a stack overflow");
        out_nl(o);
    } else if (ledger_find_freed(addr, 1, &f)) {
        line_start(o, "hint");
        out_str(o, "address is ");
        out_udec(o, addr - f.addr);
        out_str(o, " bytes into a ");
        out_size(o, f.size);
        out_str(o, " block freed ");
        out_ms(o, fdr_now_ns() - f.when_ns);
        out_str(o, " earlier: use-after-free");
        out_nl(o);
        out_tag(o);
        out_str(o, "  that block was allocated at:");
        out_nl(o);
        stack_print_site(o, f.alloc_site);
        out_tag(o);
        out_str(o, "  and freed at:");
        out_nl(o);
        stack_print_site(o, f.free_site);
    }
}

static void print_backtrace(struct fdr_out *o, uintptr_t pc)
{
    void *raw[CRASH_DEPTH + 16];
    uintptr_t frames[CRASH_DEPTH];
    int n = backtrace(raw, CRASH_DEPTH + 16);
    int start = -1, k = 0;

    /* The unwinder walks through our handler and the kernel's signal frame
     * first. Start the report at the instruction that actually faulted. */
    for (int i = 0; i < n; i++) {
        if ((uintptr_t)raw[i] == pc) {
            start = i;
            break;
        }
    }
    if (start >= 0)
        k = stack_trim((const void *const *)raw, n, start, frames, CRASH_DEPTH);
    else if (pc)
        frames[k++] = pc;   /* could not unwind past the signal frame */

    out_tag(o);
    out_str(o, "backtrace:");
    out_nl(o);
    stack_print(o, frames, k, 1);
}

static const char *op_name(uint32_t op)
{
    switch (op) {
    case OP_MALLOC:   return "malloc";
    case OP_CALLOC:   return "calloc";
    case OP_REALLOC:  return "realloc";
    case OP_MEMALIGN: return "memalign";
    case OP_FREE:     return "free";
    default:          return "?";
    }
}

static void print_recent_events(struct fdr_out *o)
{
    struct fdr_event ev[CRASH_EVENTS];
    int n = ledger_events(ev, CRASH_EVENTS);

    out_tag(o);
    out_str(o, "last ");
    out_count(o, (uint64_t)n, "heap event");
    out_str(o, ", oldest first:");
    out_nl(o);
    for (int i = 0; i < n; i++) {
        out_tag(o);
        out_str(o, "    t+");
        out_ms(o, ev[i].when_ns);
        out_pad(o, sizeof FDR_TAG + 18);
        out_str(o, op_name(ev[i].op));
        out_pad(o, sizeof FDR_TAG + 28);
        out_hex(o, ev[i].addr, 12);
        out_str(o, "  ");
        out_size(o, ev[i].size);
        out_pad(o, sizeof FDR_TAG + 56);
        out_str(o, "tid ");
        out_udec(o, (unsigned long long)ev[i].tid);
        out_nl(o);
    }
}

static void print_heap_summary(struct fdr_out *o)
{
    struct fdr_stats st;

    ledger_stats(&st);
    line_start(o, "heap");
    out_count(o, st.live_blocks, "live block");
    out_str(o, ", ");
    out_size(o, st.live_bytes);
    out_str(o, " in use, peak ");
    out_size(o, st.peak_bytes);
    out_str(o, ", ");
    out_count(o, st.allocs, "allocation");
    out_nl(o);
}

/* "<dir>/heapfdr-crash-<pid>.log", built by hand: no snprintf in here. */
static int open_log(void)
{
    size_t n = 0, dir_len = strlen(fdr_cfg.log_dir);
    char digits[16];
    int nd = 0;
    unsigned long pid = (unsigned long)getpid();
    static const char stem[] = "/heapfdr-crash-";
    static const char ext[] = ".log";

    if (dir_len == 0 || dir_len + sizeof stem + sizeof ext + sizeof digits > sizeof log_path)
        return -1;
    memcpy(log_path, fdr_cfg.log_dir, dir_len);
    n = dir_len;
    memcpy(log_path + n, stem, sizeof stem - 1);
    n += sizeof stem - 1;
    do {
        digits[nd++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid);
    while (nd)
        log_path[n++] = digits[--nd];
    memcpy(log_path + n, ext, sizeof ext);

    return open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
}

static void restore_previous(int sig)
{
    for (int i = 0; i < NFATAL; i++)
        if (fatal_signals[i] == sig)
            sigaction(sig, &previous[i], NULL);
}

static void on_fatal_signal(int sig, siginfo_t *si, void *ctx)
{
    int saved_errno = errno;
    uintptr_t pc, sp, fp, addr = (uintptr_t)si->si_addr;
    struct fdr_out o;
    int logfd;

    if (fdr_crashing) {
        /* Something went wrong while reporting. Don't try again. */
        restore_previous(sig);
        raise(sig);
        return;
    }
    fdr_crashing = 1;

    read_registers(ctx, &pc, &sp, &fp);
    logfd = open_log();
    out_open(&o, STDERR_FILENO);
    out_also(&o, logfd);
    stack_forget_modules();

    out_tag(&o);
    out_str(&o, "======== CRASH ========");
    out_nl(&o);

    line_start(&o, "signal");
    out_str(&o, signal_name(sig));
    out_str(&o, " (");
    out_str(&o, signal_reason(sig, si->si_code));
    out_str(&o, ")");
    out_nl(&o);

    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL) {
        line_start(&o, "address");
        out_hex(&o, addr, 1);
        out_nl(&o);
    }
    print_hint(&o, sig, addr, sp);

    line_start(&o, "process");
    out_str(&o, "pid ");
    out_udec(&o, (unsigned long long)getpid());
    out_str(&o, ", tid ");
    out_udec(&o, (unsigned long long)fdr_tid());
    out_str(&o, ", ");
    out_ms(&o, fdr_now_ns());
    out_str(&o, " after start");
    out_nl(&o);

    if (pc) {
        line_start(&o, "regs");
        out_str(&o, "pc ");
        out_hex(&o, pc, 12);
        out_str(&o, "  sp ");
        out_hex(&o, sp, 12);
        out_str(&o, "  fp ");
        out_hex(&o, fp, 12);
        out_nl(&o);
    }

    print_backtrace(&o, pc);
    print_recent_events(&o);
    print_heap_summary(&o);
    stack_print_modules(&o);

    if (logfd >= 0) {
        out_tag(&o);
        out_str(&o, "this report was also written to ");
        out_str(&o, log_path);
        out_nl(&o);
        close(logfd);
    }

    /* Hand the signal back to whoever had it before us (normally the
     * default action, so the process still dies with the right status
     * and can leave a core dump). A real fault fires again as soon as we
     * return; a signal sent with kill()/raise() has to be re-raised. */
    restore_previous(sig);
    if (si->si_code <= 0)
        raise(sig);
    errno = saved_errno;
}

void crash_install(void)
{
    struct sigaction sa;
    void *alt = mmap(NULL, ALT_STACK_BYTES, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (alt != MAP_FAILED) {
        stack_t ss = { .ss_sp = alt, .ss_size = ALT_STACK_BYTES, .ss_flags = 0 };
        sigaltstack(&ss, NULL);
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fatal_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigfillset(&sa.sa_mask);
    for (int i = 0; i < NFATAL; i++)
        sigaction(fatal_signals[i], &sa, &previous[i]);
}
