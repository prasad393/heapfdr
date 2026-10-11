/* SPDX-License-Identifier: MIT */
/*
 * The allocator entry points the program actually calls. Each one forwards
 * to glibc and then records what happened in the ledger.
 *
 * Two rules keep this safe:
 *   - stacks are captured before taking the ledger lock, because the
 *     unwinder takes the dynamic loader's lock and dlopen() calls malloc();
 *   - a block leaves the ledger before glibc frees it, so another thread
 *     can never be handed the same address while it is still recorded.
 */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "heapfdr.h"

enum { ST_COLD, ST_RESOLVING, ST_PASSTHRU, ST_READY };

static int state = ST_COLD;
static volatile int stacks_ready;
static FDR_TLS int in_capture;

static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static void  (*real_free)(void *);
static int   (*real_posix_memalign)(void **, size_t, size_t);
static void *(*real_aligned_alloc)(size_t, size_t);
static void *(*real_memalign)(size_t, size_t);
static void *(*real_valloc)(size_t);
static void *(*real_pvalloc)(size_t);

/*
 * dlsym() itself may call calloc() before we know where the real one is.
 * Those few early requests are served from this static buffer and never
 * given back.
 */
static char early_heap[8192] __attribute__((aligned(16)));
static size_t early_used;

static void *early_alloc(size_t n)
{
    size_t at;

    n = (n + 15) & ~(size_t)15;
    at = __atomic_fetch_add(&early_used, n, __ATOMIC_RELAXED);
    if (at + n > sizeof early_heap)
        return NULL;
    return early_heap + at;
}

static int from_early_heap(const void *p)
{
    return (const char *)p >= early_heap && (const char *)p < early_heap + sizeof early_heap;
}

#define RESOLVE(var, name) ((var) = (__typeof__(var))dlsym(RTLD_NEXT, name))

static void resolve_real_functions(void)
{
    RESOLVE(real_malloc, "malloc");
    RESOLVE(real_calloc, "calloc");
    RESOLVE(real_realloc, "realloc");
    RESOLVE(real_free, "free");
    RESOLVE(real_posix_memalign, "posix_memalign");
    RESOLVE(real_aligned_alloc, "aligned_alloc");
    RESOLVE(real_memalign, "memalign");
    RESOLVE(real_valloc, "valloc");
    RESOLVE(real_pvalloc, "pvalloc");
}

static void early_init(void)
{
    int expected = ST_COLD;

    if (!__atomic_compare_exchange_n(&state, &expected, ST_RESOLVING, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;     /* already under way; callers cope with ST_RESOLVING */

    resolve_real_functions();
    if (!real_malloc || !real_calloc || !real_realloc || !real_free) {
        static const char msg[] = FDR_TAG "cannot find the real allocator, giving up\n";
        if (write(STDERR_FILENO, msg, sizeof msg - 1) < 0) {
            /* nothing sensible left to do */
        }
        _exit(127);
    }
    __atomic_store_n(&state, ST_PASSTHRU, __ATOMIC_RELEASE);

    fdr_config_defaults();
    if (ledger_init())
        __atomic_store_n(&state, ST_READY, __ATOMIC_RELEASE);
}

static inline int current_state(void)
{
    int s = __atomic_load_n(&state, __ATOMIC_ACQUIRE);

    if (likely(s == ST_READY))
        return s;
    if (s == ST_COLD) {
        early_init();
        s = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
    }
    return s;
}

static int capture(uintptr_t *frames)
{
    int n;

    if (!stacks_ready || in_capture)
        return 0;
    in_capture = 1;
    n = stack_capture(frames, fdr_cfg.depth);
    in_capture = 0;
    return n;
}

static void track(void *p, size_t size, int op)
{
    uintptr_t frames[FDR_MAX_DEPTH];
    int depth = capture(frames);

    ledger_lock();
    ledger_add((uintptr_t)p, size, ledger_site(frames, depth), op);
    ledger_unlock();
}

/*
 * free() or realloc() got a pointer that is not a live block. Work out
 * what went wrong and report it. Returns 1 if glibc should still see the
 * pointer (we simply never saw it allocated), 0 if passing it on would
 * corrupt the heap.
 */
static int handle_stray(uintptr_t addr, const uintptr_t *frames, int depth, int op)
{
    struct fdr_freed prev;
    struct fdr_block owner;
    int twice, inside;

    ledger_lock();
    twice = ledger_find_freed(addr, 0, &prev);
    inside = !twice && ledger_find_live_containing(addr, &owner);
    ledger_count_stray(twice || inside);
    ledger_unlock();

    if (twice) {
        report_double_free(addr, &prev, frames, depth, op);
    } else if (inside) {
        report_bad_free(addr, &owner, frames, depth, op);
    } else {
        report_unknown_free(addr, frames, depth, op);
        return 1;
    }
    if (fdr_cfg.abort_on_error)
        abort();
    return 0;
}

/* ------------------------------------------------------ entry points */

FDR_EXPORT void *malloc(size_t size)
{
    int s = current_state();
    void *p;

    if (unlikely(s == ST_RESOLVING))
        return early_alloc(size);
    p = real_malloc(size);
    if (p && s == ST_READY)
        track(p, size, OP_MALLOC);
    return p;
}

FDR_EXPORT void *calloc(size_t count, size_t size)
{
    int s = current_state();
    size_t bytes;
    void *p;

    if (__builtin_mul_overflow(count, size, &bytes)) {
        errno = ENOMEM;
        return NULL;
    }
    if (unlikely(s == ST_RESOLVING))
        return early_alloc(bytes);      /* static storage, already zero */
    p = real_calloc(count, size);
    if (p && s == ST_READY)
        track(p, bytes, OP_CALLOC);
    return p;
}

FDR_EXPORT void free(void *p)
{
    uintptr_t frames[FDR_MAX_DEPTH];
    struct fdr_block b;
    int s, depth;

    if (!p || from_early_heap(p))
        return;
    s = current_state();
    if (s != ST_READY) {
        if (real_free)
            real_free(p);
        return;
    }

    depth = capture(frames);
    ledger_lock();
    if (ledger_take((uintptr_t)p, &b)) {
        ledger_freed(&b, ledger_site(frames, depth), OP_FREE);
        ledger_unlock();
        real_free(p);
        return;
    }
    ledger_unlock();

    if (handle_stray((uintptr_t)p, frames, depth, OP_FREE))
        real_free(p);
}

static void *realloc_early(void *p, size_t size, int s)
{
    size_t avail;
    void *q;

    if (s == ST_RESOLVING)
        q = early_alloc(size);
    else
        q = malloc(size);
    if (q && p) {
        avail = (size_t)(early_heap + sizeof early_heap - (char *)p);
        memcpy(q, p, size < avail ? size : avail);
    }
    return q;
}

FDR_EXPORT void *realloc(void *p, size_t size)
{
    uintptr_t frames[FDR_MAX_DEPTH];
    struct fdr_block old;
    uint32_t site;
    int s = current_state();
    int known, depth;
    void *q;

    if (unlikely(s == ST_RESOLVING || (p && from_early_heap(p))))
        return realloc_early(p, size, s);
    if (s != ST_READY)
        return real_realloc(p, size);
    if (!p) {
        q = real_realloc(NULL, size);
        if (q)
            track(q, size, OP_REALLOC);
        return q;
    }

    depth = capture(frames);
    ledger_lock();
    known = ledger_take((uintptr_t)p, &old);
    ledger_unlock();

    if (!known && !handle_stray((uintptr_t)p, frames, depth, OP_REALLOC)) {
        errno = ENOMEM;
        return NULL;
    }

    q = real_realloc(p, size);

    ledger_lock();
    site = ledger_site(frames, depth);
    if (!q && size != 0) {
        if (known)
            ledger_restore(&old);       /* failed: the old block is still valid */
    } else {
        if (known && q != p)
            ledger_freed(&old, site, OP_REALLOC);
        if (q)
            ledger_add((uintptr_t)q, size, site, OP_REALLOC);
    }
    ledger_unlock();
    return q;
}

FDR_EXPORT void *reallocarray(void *p, size_t count, size_t size)
{
    size_t bytes;

    if (__builtin_mul_overflow(count, size, &bytes)) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(p, bytes);
}

FDR_EXPORT int posix_memalign(void **out, size_t align, size_t size)
{
    int s = current_state();
    int rc;

    if (unlikely(s == ST_RESOLVING))
        return ENOMEM;
    rc = real_posix_memalign(out, align, size);
    if (rc == 0 && *out && s == ST_READY)
        track(*out, size, OP_MEMALIGN);
    return rc;
}

static void *aligned_common(void *(*fn)(size_t, size_t), size_t align, size_t size)
{
    int s = current_state();
    void *p;

    if (unlikely(s == ST_RESOLVING))
        return NULL;
    p = fn ? fn(align, size) : real_memalign(align, size);
    if (p && s == ST_READY)
        track(p, size, OP_MEMALIGN);
    return p;
}

FDR_EXPORT void *aligned_alloc(size_t align, size_t size)
{
    return aligned_common(real_aligned_alloc, align, size);
}

FDR_EXPORT void *memalign(size_t align, size_t size)
{
    return aligned_common(real_memalign, align, size);
}

FDR_EXPORT void *valloc(size_t size)
{
    int s = current_state();
    void *p;

    if (unlikely(s == ST_RESOLVING))
        return NULL;
    p = real_valloc ? real_valloc(size) : real_memalign((size_t)getpagesize(), size);
    if (p && s == ST_READY)
        track(p, size, OP_MEMALIGN);
    return p;
}

FDR_EXPORT void *pvalloc(size_t size)
{
    size_t page = (size_t)getpagesize();
    size_t rounded = (size + page - 1) & ~(page - 1);
    int s = current_state();
    void *p;

    if (unlikely(s == ST_RESOLVING))
        return NULL;
    p = real_pvalloc ? real_pvalloc(size) : real_memalign(page, rounded);
    if (p && s == ST_READY)
        track(p, rounded ? rounded : page, OP_MEMALIGN);
    return p;
}

/* ------------------------------------------- knowing when main() runs */

/*
 * Wrapping __libc_start_main tells us when main() begins and ends, so
 * memory the runtime sets up before main() (locale data, C++ runtime
 * pools, ...) can be kept out of the leak report by default.
 */
typedef int (*main_fn)(int, char **, char **);
static main_fn user_main;

/* Lives in its own section so stack.c can recognise it in a backtrace:
 * everything below this frame is libc start-up code. */
__attribute__((noinline, section("heapfdr_main")))
static int main_trampoline(int argc, char **argv, char **envp)
{
    int rc;

    fdr_saw_main = 1;
    fdr_phase = 1;
    rc = user_main(argc, argv, envp);
    fdr_phase = 2;
    return rc;
}

FDR_EXPORT int __libc_start_main(main_fn main_func, int argc, char **argv,
                                 void (*init)(void), void (*fini)(void),
                                 void (*rtld_fini)(void), void *stack_end)
{
    int (*real_start)(main_fn, int, char **, void (*)(void), void (*)(void),
                      void (*)(void), void *);

    RESOLVE(real_start, "__libc_start_main");
    user_main = main_func;
    return real_start(main_trampoline, argc, argv, init, fini, rtld_fini, stack_end);
}

/* ---------------------------------------------------- start and stop */

static int is_heapfdr_entry(const char *s, size_t len)
{
    const char *name = s;

    for (size_t i = 0; i < len; i++)
        if (s[i] == '/')
            name = s + i + 1;
    return (size_t)(s + len - name) >= 10 && memcmp(name, "libheapfdr", 10) == 0;
}

/* Child processes (shells from system(), helpers...) are not traced unless asked. */
static void drop_from_ld_preload(void)
{
    const char *cur = getenv("LD_PRELOAD");
    char keep[1024];
    size_t k = 0;

    if (!cur)
        return;
    for (const char *p = cur; *p;) {
        const char *start;
        size_t len;

        while (*p == ':' || *p == ' ')
            p++;
        start = p;
        while (*p && *p != ':' && *p != ' ')
            p++;
        len = (size_t)(p - start);
        if (!len || is_heapfdr_entry(start, len))
            continue;
        if (k + len + 2 > sizeof keep)
            return;     /* unusually long; leave it alone */
        if (k)
            keep[k++] = ':';
        memcpy(keep + k, start, len);
        k += len;
    }
    keep[k] = '\0';
    if (k)
        setenv("LD_PRELOAD", keep, 1);
    else
        unsetenv("LD_PRELOAD");
}

static void banner(void)
{
    struct fdr_out o;

    out_open(&o, STDERR_FILENO);
    out_tag(&o);
    out_str(&o, "recording heap activity of pid ");
    out_udec(&o, (unsigned long long)getpid());
    out_str(&o, " (stack depth ");
    out_udec(&o, (unsigned)fdr_cfg.depth);
    out_str(&o, ")");
    out_nl(&o);
}

/* fork() copies our locks in whatever state other threads left them. */
static void child_after_fork(void)
{
    ledger_child_after_fork();
    report_child_after_fork();
    elfsym_child_after_fork();
}

__attribute__((constructor)) static void heapfdr_start(void)
{
    if (current_state() != ST_READY) {
        static const char msg[] = FDR_TAG "could not set up bookkeeping, not tracking\n";
        if (write(STDERR_FILENO, msg, sizeof msg - 1) < 0) {
            /* nothing to do */
        }
        return;
    }

    fdr_config_load();

    in_capture = 1;
    stack_init();
    in_capture = 0;
    stacks_ready = 1;

    crash_install();
    pthread_atfork(ledger_lock, ledger_unlock, child_after_fork);
    if (!fdr_cfg.trace_children)
        drop_from_ld_preload();
    if (!fdr_cfg.quiet)
        banner();
}

__attribute__((destructor)) static void heapfdr_stop(void)
{
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == ST_READY)
        report_leaks();
}
