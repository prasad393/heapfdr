/* SPDX-License-Identifier: MIT */
/* Capturing call stacks and turning addresses into readable frames. */
#include <dlfcn.h>
#include <execinfo.h>
#include <link.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "heapfdr.h"

#define MAX_MODULES 16

static uintptr_t self_lo, self_hi;          /* where libheapfdr itself is mapped */
static char exe_path[256];                  /* absolute path of the main program */

static const char *modules_seen[MAX_MODULES];
static int nmodules_seen;

struct where {
    const char *path;
    const char *module;     /* basename of path */
    const char *symbol;
    uintptr_t   sym_off;
    uintptr_t   file_addr;  /* address as addr2line sees it */
};

static int find_self(struct dl_phdr_info *info, size_t size, void *arg)
{
    uintptr_t probe = (uintptr_t)arg;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    int mine = 0;

    (void)size;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t start, end;

        if (ph->p_type != PT_LOAD)
            continue;
        start = info->dlpi_addr + ph->p_vaddr;
        end = start + ph->p_memsz;
        if (start < lo)
            lo = start;
        if (end > hi)
            hi = end;
        if (probe >= start && probe < end)
            mine = 1;
    }
    if (!mine)
        return 0;
    self_lo = lo;
    self_hi = hi;
    return 1;
}

void stack_init(void)
{
    void *warmup[4];
    ssize_t n = readlink("/proc/self/exe", exe_path, sizeof exe_path - 1);

    exe_path[n > 0 ? n : 0] = '\0';
    dl_iterate_phdr(find_self, (void *)(uintptr_t)&stack_init);
    /* The first backtrace() loads libgcc_s and allocates. Do it now,
     * not in the middle of someone's malloc() or a crash. */
    backtrace(warmup, 4);
}

/* Start and end of the section holding our main() wrapper (see hooks.c);
 * the linker defines these for any section named like a C identifier. */
extern const char __start_heapfdr_main[] __attribute__((visibility("hidden")));
extern const char __stop_heapfdr_main[] __attribute__((visibility("hidden")));

static int is_self(uintptr_t pc)
{
    return pc >= self_lo && pc < self_hi;
}

static int is_main_wrapper(uintptr_t pc)
{
    return pc >= (uintptr_t)__start_heapfdr_main && pc < (uintptr_t)__stop_heapfdr_main;
}

/*
 * Copy raw[start..n) into frames without heapfdr's own frames (our hooks
 * show up when glibc aborts inside them). Stop at our main() wrapper:
 * below it is only libc start-up code.
 */
int stack_trim(const void *const *raw, int n, int start, uintptr_t *frames, int max)
{
    int k = 0;

    for (int i = start; i < n && k < max; i++) {
        uintptr_t pc = (uintptr_t)raw[i];

        if (is_main_wrapper(pc))
            break;
        if (!is_self(pc))
            frames[k++] = pc;
    }
    return k;
}

int stack_capture(uintptr_t *frames, int max)
{
    void *raw[FDR_MAX_DEPTH + 8];
    int n = backtrace(raw, max + 8);

    return stack_trim((const void *const *)raw, n, 0, frames, max);
}

static const char *base_name(const char *path)
{
    const char *b = path;

    for (const char *p = path; *p; p++)
        if (*p == '/')
            b = p + 1;
    return b;
}

static int locate(uintptr_t addr, struct where *w)
{
    Dl_info info;
    struct link_map *map = NULL;

    memset(w, 0, sizeof *w);
    if (!dladdr1((void *)addr, &info, (void **)&map, RTLD_DL_LINKMAP) || !info.dli_fname)
        return 0;

    /* The main program has an empty name in the link map and dladdr()
     * reports argv[0]; an absolute path is more useful for symbolizing. */
    if (map && map->l_name[0] == '\0' && exe_path[0])
        w->path = exe_path;
    else if (*info.dli_fname)
        w->path = info.dli_fname;
    else
        return 0;

    w->module = base_name(w->path);
    w->file_addr = addr - (map ? map->l_addr : 0);
    if (info.dli_sname) {
        w->symbol = info.dli_sname;
        w->sym_off = addr - (uintptr_t)info.dli_saddr;
    } else {
        w->symbol = elf_symbol(w->path, w->file_addr, &w->sym_off);
    }
    return 1;
}

static void remember_module(const char *path)
{
    for (int i = 0; i < nmodules_seen; i++)
        if (strcmp(modules_seen[i], path) == 0)
            return;
    if (nmodules_seen < MAX_MODULES)
        modules_seen[nmodules_seen++] = path;
}

void stack_forget_modules(void)
{
    nmodules_seen = 0;
}

/* The module list at the end of a report is what heapfdr-symbolize reads. */
void stack_print_modules(struct fdr_out *o)
{
    for (int i = 0; i < nmodules_seen; i++) {
        out_tag(o);
        out_str(o, "  module ");
        out_str(o, base_name(modules_seen[i]));
        out_str(o, " = ");
        out_str(o, modules_seen[i]);
        out_nl(o);
    }
}

typedef char *(*demangle_fn)(const char *, char *, size_t *, int *);

/*
 * C++ names, demangled with the C++ runtime's own __cxa_demangle() when
 * the program has it loaded. It allocates, so not during a crash; the
 * crash report keeps the raw name and heapfdr-symbolize fixes it later.
 */
static const char *readable(const char *sym, char *buf, size_t cap)
{
    static demangle_fn demangle;
    static int looked_up;
    char *out;
    size_t len;
    int status = -1;

    if (fdr_crashing || strncmp(sym, "_Z", 2) != 0)
        return sym;
    if (!looked_up) {
        demangle = (demangle_fn)dlsym(RTLD_DEFAULT, "__cxa_demangle");
        looked_up = 1;
    }
    if (!demangle)
        return sym;
    out = demangle(sym, NULL, NULL, &status);
    if (status != 0 || !out)
        return sym;

    len = strlen(out);
    if (len < cap) {
        memcpy(buf, out, len + 1);
    } else {
        memcpy(buf, out, cap - 4);
        memcpy(buf + cap - 4, "...", 4);
    }
    free(out);
    return buf;
}

void stack_print(struct fdr_out *o, const uintptr_t *frames, int n, int first_is_pc)
{
    char name[160];

    for (int i = 0; i < n; i++) {
        uintptr_t pc = frames[i];
        /* Return addresses point past the call. Step back one byte so the
         * lookup lands on the call instruction itself. */
        uintptr_t at = (i == 0 && first_is_pc) ? pc : pc - 1;
        struct where w;
        int repeats = 0;

        /* deep recursion: print the frame once and say how often it repeats */
        while (i + 1 + repeats < n && frames[i + 1 + repeats] == pc)
            repeats++;

        out_tag(o);
        out_str(o, "    #");
        out_udec(o, (unsigned)i);
        out_pad(o, sizeof FDR_TAG + 8);
        out_hex(o, pc, 12);
        out_str(o, "  ");
        if (locate(at, &w)) {
            if (w.symbol) {
                out_str(o, readable(w.symbol, name, sizeof name));
                out_chr(o, '+');
                out_hex(o, w.sym_off, 1);
            } else {
                out_str(o, "??");
            }
            out_str(o, "  (");
            out_str(o, w.module);
            out_chr(o, ':');
            out_hex(o, w.file_addr, 1);
            out_chr(o, ')');
            remember_module(w.path);
        } else {
            out_str(o, "??");
        }
        out_nl(o);

        if (repeats) {
            out_tag(o);
            out_pad(o, sizeof FDR_TAG + 8);
            out_str(o, "... same frame ");
            out_count(o, (uint64_t)repeats, "more time");
            out_nl(o);
            i += repeats;
        }
    }
}

void stack_print_site(struct fdr_out *o, uint32_t site)
{
    const struct fdr_site *s = ledger_site_at(site);

    if (site == 0 || s->depth == 0) {
        out_tag(o);
        out_str(o, "    (no stack recorded)");
        out_nl(o);
        return;
    }
    stack_print(o, s->frames, (int)s->depth, 0);
}

/* Memory the C runtime keeps for the life of the process: stdio buffers
 * and the dynamic loader's own bookkeeping. Not the program's leaks. */
int stack_is_runtime_noise(const uintptr_t *frames, int n)
{
    struct where w;

    if (n == 0 || !locate(frames[0] - 1, &w))
        return 0;
    if (strncmp(w.module, "ld-linux", 8) == 0 || strncmp(w.module, "ld.so", 5) == 0)
        return 1;
    return w.symbol && strcmp(w.symbol, "_IO_file_doallocate") == 0;
}

static int pattern_hit(const char *patterns, const char *text)
{
    const char *p = patterns;

    if (!text)
        return 0;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        if (len && memmem(text, strlen(text), p, len))
            return 1;
        if (!comma)
            break;
        p = comma + 1;
    }
    return 0;
}

/* HEAPFDR_SUPPRESS: any frame whose symbol or module contains a pattern. */
int stack_matches(const uintptr_t *frames, int n, const char *patterns)
{
    if (!patterns || !*patterns)
        return 0;
    for (int i = 0; i < n; i++) {
        struct where w;
        if (!locate(frames[i] - 1, &w))
            continue;
        if (pattern_hit(patterns, w.symbol) || pattern_hit(patterns, w.module))
            return 1;
    }
    return 0;
}
