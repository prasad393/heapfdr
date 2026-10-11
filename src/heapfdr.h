/* SPDX-License-Identifier: MIT */
/*
 * heapfdr - a heap "flight data recorder" for Linux processes.
 *
 * Internal interfaces shared by the library's source files. Nothing in
 * here is exported from libheapfdr.so; only the allocator entry points
 * in hooks.c are visible to the program.
 */
#ifndef HEAPFDR_H
#define HEAPFDR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define FDR_EXPORT  __attribute__((visibility("default")))
#define FDR_TLS     __thread __attribute__((tls_model("initial-exec")))
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define FDR_TAG          "[heapfdr] "
#define FDR_MAX_DEPTH    24     /* frames kept per call site */
#define FDR_EVENT_SLOTS  64     /* heap events kept for crash reports */
#define FDR_FREED_SLOTS  4096   /* recently freed blocks, for double free / UAF */

/* ---------------------------------------------------------------- config.c */

struct fdr_config {
    int  depth;             /* frames recorded per allocation */
    int  top_sites;         /* leak sites printed at exit */
    int  abort_on_error;    /* abort() on double or invalid free */
    int  leak_exit_code;    /* exit with this code when leaks are found */
    int  include_startup;   /* also report blocks allocated before main() */
    int  trace_children;    /* keep LD_PRELOAD for child processes */
    int  quiet;             /* no banner */
    char log_dir[256];      /* where crash logs go, "" = nowhere */
    char suppress[512];     /* comma separated symbol/module substrings */
};

extern struct fdr_config fdr_cfg;
void fdr_config_defaults(void);
void fdr_config_load(void);

/* ------------------------------------------------------------------- out.c */

/* Output that never allocates, so it works inside malloc and in a crash. */
struct fdr_out {
    int    fds[2];
    int    nfds;
    size_t len;
    size_t col;
    char   buf[512];
};

void out_open(struct fdr_out *o, int fd);
void out_also(struct fdr_out *o, int fd);
void out_chr(struct fdr_out *o, char c);
void out_str(struct fdr_out *o, const char *s);
void out_udec(struct fdr_out *o, unsigned long long v);
void out_hex(struct fdr_out *o, uintptr_t v, int min_digits);
void out_size(struct fdr_out *o, uint64_t bytes);
void out_ms(struct fdr_out *o, uint64_t ns);
void out_count(struct fdr_out *o, uint64_t n, const char *noun);
void out_pad(struct fdr_out *o, size_t col);
void out_tag(struct fdr_out *o);
void out_nl(struct fdr_out *o);
void out_flush(struct fdr_out *o);

/* ---------------------------------------------------------------- ledger.c */

enum fdr_op { OP_MALLOC = 1, OP_CALLOC, OP_REALLOC, OP_MEMALIGN, OP_FREE };

#define BLOCK_BOOT 1u   /* allocated before main() started */

struct fdr_block {
    uintptr_t addr;
    size_t    size;
    uint32_t  site;
    uint32_t  flags;
};

struct fdr_site {
    uintptr_t frames[FDR_MAX_DEPTH];
    uint64_t  hash;
    uint32_t  depth;
    uint64_t  live_blocks, live_bytes;
    uint64_t  boot_blocks, boot_bytes;
    uint64_t  allocs;
};

struct fdr_freed {
    uintptr_t addr;
    size_t    size;
    uint32_t  alloc_site;
    uint32_t  free_site;
    uint64_t  when_ns;
};

struct fdr_event {
    uint64_t  when_ns;
    uintptr_t addr;
    size_t    size;
    pid_t     tid;
    uint32_t  op;
};

struct fdr_stats {
    uint64_t allocs, frees;
    uint64_t live_blocks, live_bytes, peak_bytes;
    uint64_t errors, unknown_frees;
};

extern volatile int fdr_phase;      /* 0 before main(), 1 in main(), 2 after */
extern volatile int fdr_saw_main;   /* the __libc_start_main hook ran */

int       ledger_init(void);
void      ledger_lock(void);
void      ledger_unlock(void);
void      ledger_child_after_fork(void);

/* The functions below expect the ledger lock to be held. */
uint32_t  ledger_site(const uintptr_t *frames, int depth);
void      ledger_add(uintptr_t addr, size_t size, uint32_t site, int op);
int       ledger_take(uintptr_t addr, struct fdr_block *out);
void      ledger_restore(const struct fdr_block *b);
void      ledger_freed(const struct fdr_block *b, uint32_t free_site, int op);
int       ledger_find_live_containing(uintptr_t addr, struct fdr_block *out);
void      ledger_count_stray(int is_error);

/* Lock-free readers, also used from the crash handler. */
int       ledger_find_freed(uintptr_t addr, int containing, struct fdr_freed *out);
const struct fdr_site *ledger_site_at(uint32_t idx);
uint32_t  ledger_site_count(void);
void      ledger_stats(struct fdr_stats *out);
int       ledger_events(struct fdr_event *out, int max);
uint64_t  fdr_now_ns(void);
pid_t     fdr_tid(void);

/* ---------------------------------------------------------------- elfsym.c */

const char *elf_symbol(const char *path, uintptr_t file_addr, uintptr_t *offset);
void        elfsym_child_after_fork(void);

/* ----------------------------------------------------------------- stack.c */

void stack_init(void);
int  stack_capture(uintptr_t *frames, int max);
int  stack_trim(const void *const *raw, int n, int start, uintptr_t *frames, int max);
void stack_print(struct fdr_out *o, const uintptr_t *frames, int n, int first_is_pc);
void stack_print_site(struct fdr_out *o, uint32_t site);
int  stack_is_runtime_noise(const uintptr_t *frames, int n);
int  stack_matches(const uintptr_t *frames, int n, const char *patterns);
void stack_forget_modules(void);
void stack_print_modules(struct fdr_out *o);

/* ---------------------------------------------------------------- report.c */

void report_double_free(uintptr_t addr, const struct fdr_freed *prev,
                        const uintptr_t *frames, int depth, int op);
void report_bad_free(uintptr_t addr, const struct fdr_block *owner,
                     const uintptr_t *frames, int depth, int op);
void report_unknown_free(uintptr_t addr, const uintptr_t *frames, int depth, int op);
void report_leaks(void);
void report_child_after_fork(void);

/* ----------------------------------------------------------------- crash.c */

extern volatile int fdr_crashing;
void crash_install(void);

#endif /* HEAPFDR_H */
