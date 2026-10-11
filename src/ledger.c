/* SPDX-License-Identifier: MIT */
/*
 * The ledger: every live heap block, the call sites that allocated them,
 * a ring of recently freed blocks and a short history of heap events.
 *
 * Storage comes straight from mmap(), so nothing in here can call back
 * into the allocator we are watching.
 */
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "heapfdr.h"

#define BLOCKS_INITIAL  (1u << 16)
#define SITE_SLOTS      (1u << 16)   /* distinct call sites we can remember */
#define SITE_INDEX      (1u << 17)   /* hash index over them, kept half empty */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static struct fdr_block *blocks;     /* open addressing, addr 0 = empty */
static size_t block_cap, block_count;

static struct fdr_site *sites;       /* site 0 = "no stack recorded" */
static uint32_t site_count;
static uint32_t *site_index;         /* 0 = empty, else index into sites */

static struct fdr_freed freed[FDR_FREED_SLOTS];
static uint64_t freed_head;

static struct fdr_event events[FDR_EVENT_SLOTS];
static uint64_t event_head;

static struct fdr_stats stats;
static uint64_t t0_ns;

volatile int fdr_phase;
volatile int fdr_saw_main;

static FDR_TLS pid_t cached_tid;

/* ------------------------------------------------------------- helpers */

static void *map_zeroed(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static uint64_t mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t fdr_now_ns(void)
{
    return mono_ns() - t0_ns;
}

pid_t fdr_tid(void)
{
    if (!cached_tid)
        cached_tid = (pid_t)syscall(SYS_gettid);
    return cached_tid;
}

/* murmur3 finalizer: cheap and spreads pointer bits well */
static inline uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

static inline size_t home_slot(uintptr_t addr, size_t cap)
{
    return (size_t)mix64(addr) & (cap - 1);
}

/* ---------------------------------------------------------- lifecycle */

int ledger_init(void)
{
    t0_ns = mono_ns();
    block_cap = BLOCKS_INITIAL;
    blocks = map_zeroed(block_cap * sizeof *blocks);
    sites = map_zeroed(SITE_SLOTS * sizeof *sites);
    site_index = map_zeroed(SITE_INDEX * sizeof *site_index);
    if (!blocks || !sites || !site_index)
        return 0;
    site_count = 1;
    return 1;
}

void ledger_lock(void)
{
    pthread_mutex_lock(&lock);
}

void ledger_unlock(void)
{
    pthread_mutex_unlock(&lock);
}

/* The forking thread held the lock; in the child it is the only thread left. */
void ledger_child_after_fork(void)
{
    pthread_mutex_init(&lock, NULL);
    cached_tid = 0;
}

/* -------------------------------------------------------- block table */

static void block_put(struct fdr_block *tab, size_t cap, const struct fdr_block *b)
{
    size_t i = home_slot(b->addr, cap);

    while (tab[i].addr)
        i = (i + 1) & (cap - 1);
    tab[i] = *b;
}

static int grow_blocks(void)
{
    size_t ncap = block_cap * 2;
    struct fdr_block *ntab = map_zeroed(ncap * sizeof *ntab);

    if (!ntab)
        return 0;
    for (size_t i = 0; i < block_cap; i++)
        if (blocks[i].addr)
            block_put(ntab, ncap, &blocks[i]);
    munmap(blocks, block_cap * sizeof *blocks);
    blocks = ntab;
    block_cap = ncap;
    return 1;
}

static size_t block_find(uintptr_t addr)
{
    size_t mask = block_cap - 1;

    for (size_t i = home_slot(addr, block_cap); blocks[i].addr; i = (i + 1) & mask)
        if (blocks[i].addr == addr)
            return i;
    return SIZE_MAX;
}

/* Linear probing delete without tombstones: pull later entries back into
 * the hole unless that would move them in front of their home slot. */
static void block_erase(size_t hole)
{
    size_t mask = block_cap - 1;
    size_t j = hole;

    for (;;) {
        j = (j + 1) & mask;
        if (!blocks[j].addr)
            break;
        size_t home = home_slot(blocks[j].addr, block_cap);
        int stays = (hole < j) ? (hole < home && home <= j)
                               : (hole < home || home <= j);
        if (!stays) {
            blocks[hole] = blocks[j];
            hole = j;
        }
    }
    blocks[hole].addr = 0;
}

static void count_in(const struct fdr_block *b)
{
    struct fdr_site *s = &sites[b->site];

    if (b->flags & BLOCK_BOOT) {
        s->boot_blocks++;
        s->boot_bytes += b->size;
    } else {
        s->live_blocks++;
        s->live_bytes += b->size;
    }
    stats.live_blocks++;
    stats.live_bytes += b->size;
    if (stats.live_bytes > stats.peak_bytes)
        stats.peak_bytes = stats.live_bytes;
}

static void count_out(const struct fdr_block *b)
{
    struct fdr_site *s = &sites[b->site];

    if (b->flags & BLOCK_BOOT) {
        s->boot_blocks--;
        s->boot_bytes -= b->size;
    } else {
        s->live_blocks--;
        s->live_bytes -= b->size;
    }
    stats.live_blocks--;
    stats.live_bytes -= b->size;
}

static void push_event(int op, uintptr_t addr, size_t size)
{
    struct fdr_event *e = &events[event_head % FDR_EVENT_SLOTS];

    e->when_ns = fdr_now_ns();
    e->addr = addr;
    e->size = size;
    e->tid = fdr_tid();
    e->op = (uint32_t)op;
    event_head++;
}

static void insert_block(const struct fdr_block *b)
{
    size_t slot = block_find(b->addr);

    if (slot != SIZE_MAX) {
        /* We missed a free somewhere; forget the stale entry. */
        count_out(&blocks[slot]);
        blocks[slot] = *b;
        count_in(b);
        return;
    }
    if ((block_count + 1) * 10 > block_cap * 7 && !grow_blocks())
        return;
    block_put(blocks, block_cap, b);
    block_count++;
    count_in(b);
}

void ledger_add(uintptr_t addr, size_t size, uint32_t site, int op)
{
    struct fdr_block b = {
        .addr = addr,
        .size = size,
        .site = site,
        .flags = fdr_phase == 0 ? BLOCK_BOOT : 0,
    };

    insert_block(&b);
    sites[site].allocs++;
    stats.allocs++;
    push_event(op, addr, size);
}

int ledger_take(uintptr_t addr, struct fdr_block *out)
{
    size_t slot = block_find(addr);

    if (slot == SIZE_MAX)
        return 0;
    *out = blocks[slot];
    block_erase(slot);
    block_count--;
    count_out(out);
    return 1;
}

/* Put back a block we took out, e.g. when realloc() failed. */
void ledger_restore(const struct fdr_block *b)
{
    insert_block(b);
}

void ledger_freed(const struct fdr_block *b, uint32_t free_site, int op)
{
    struct fdr_freed *f = &freed[freed_head % FDR_FREED_SLOTS];

    f->addr = b->addr;
    f->size = b->size;
    f->alloc_site = b->site;
    f->free_site = free_site;
    f->when_ns = fdr_now_ns();
    freed_head++;

    stats.frees++;
    if (op == OP_FREE)
        push_event(OP_FREE, b->addr, b->size);
}

void ledger_count_stray(int is_error)
{
    if (is_error)
        stats.errors++;
    else
        stats.unknown_frees++;
}

/* Slow on purpose: only used when free() is handed a pointer we don't know. */
int ledger_find_live_containing(uintptr_t addr, struct fdr_block *out)
{
    for (size_t i = 0; i < block_cap; i++) {
        const struct fdr_block *b = &blocks[i];
        if (b->addr && addr > b->addr && addr < b->addr + b->size) {
            *out = *b;
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------- call sites */

static uint64_t hash_frames(const uintptr_t *f, int n)
{
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)n;

    for (int i = 0; i < n; i++)
        h = mix64(h ^ f[i]);
    return h | 1;
}

uint32_t ledger_site(const uintptr_t *frames, int depth)
{
    uint64_t h;
    uint32_t mask = SITE_INDEX - 1;

    if (depth <= 0)
        return 0;
    h = hash_frames(frames, depth);
    for (uint32_t i = (uint32_t)h & mask;; i = (i + 1) & mask) {
        uint32_t idx = site_index[i];

        if (!idx) {
            struct fdr_site *s;

            if (site_count >= SITE_SLOTS)
                return 0;           /* full: account it as "no stack" */
            idx = site_count++;
            s = &sites[idx];
            memcpy(s->frames, frames, (size_t)depth * sizeof *frames);
            s->depth = (uint32_t)depth;
            s->hash = h;
            site_index[i] = idx;
            return idx;
        }
        if (sites[idx].hash == h && sites[idx].depth == (uint32_t)depth &&
            memcmp(sites[idx].frames, frames, (size_t)depth * sizeof *frames) == 0)
            return idx;
    }
}

/* ---------------------------------------------------- lock-free reads */

int ledger_find_freed(uintptr_t addr, int containing, struct fdr_freed *out)
{
    uint64_t head = freed_head;
    uint64_t n = head < FDR_FREED_SLOTS ? head : FDR_FREED_SLOTS;

    /* newest first, so a reused address matches its latest free */
    for (uint64_t k = 0; k < n; k++) {
        const struct fdr_freed *f = &freed[(head - 1 - k) % FDR_FREED_SLOTS];
        int hit = containing ? (addr >= f->addr && addr < f->addr + (f->size ? f->size : 1))
                             : (addr == f->addr);
        if (hit) {
            *out = *f;
            return 1;
        }
    }
    return 0;
}

const struct fdr_site *ledger_site_at(uint32_t idx)
{
    return &sites[idx];
}

uint32_t ledger_site_count(void)
{
    return site_count;
}

void ledger_stats(struct fdr_stats *out)
{
    *out = stats;
}

/* Copies the most recent events, oldest first. */
int ledger_events(struct fdr_event *out, int max)
{
    uint64_t head = event_head;
    uint64_t n = head < FDR_EVENT_SLOTS ? head : FDR_EVENT_SLOTS;

    if (n > (uint64_t)max)
        n = (uint64_t)max;
    for (uint64_t k = 0; k < n; k++)
        out[k] = events[(head - n + k) % FDR_EVENT_SLOTS];
    return (int)n;
}
