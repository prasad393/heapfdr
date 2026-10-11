/* SPDX-License-Identifier: MIT */
/* Runtime options, read once from HEAPFDR_* environment variables. */
#include <stdlib.h>
#include <string.h>

#include "heapfdr.h"

struct fdr_config fdr_cfg;

static int env_int(const char *name, int def, int lo, int hi)
{
    const char *s = getenv(name);
    char *end;
    long v;

    if (!s || !*s)
        return def;
    v = strtol(s, &end, 10);
    if (*end != '\0')
        return def;
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return (int)v;
}

static void env_str(const char *name, char *dst, size_t cap, const char *def)
{
    const char *s = getenv(name);
    size_t n;

    if (!s)
        s = def;
    n = strlen(s);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

/* Used until the constructor runs; the environment may not be readable yet. */
void fdr_config_defaults(void)
{
    fdr_cfg.depth = 12;
    fdr_cfg.top_sites = 10;
    fdr_cfg.abort_on_error = 0;
    fdr_cfg.leak_exit_code = 0;
    fdr_cfg.include_startup = 0;
    fdr_cfg.trace_children = 0;
    fdr_cfg.quiet = 0;
    strcpy(fdr_cfg.log_dir, ".");
    fdr_cfg.suppress[0] = '\0';
}

void fdr_config_load(void)
{
    fdr_cfg.depth           = env_int("HEAPFDR_DEPTH", 12, 1, FDR_MAX_DEPTH);
    fdr_cfg.top_sites       = env_int("HEAPFDR_TOP", 10, 1, 1000);
    fdr_cfg.abort_on_error  = env_int("HEAPFDR_ABORT", 0, 0, 1);
    fdr_cfg.leak_exit_code  = env_int("HEAPFDR_EXITCODE", 0, 0, 255);
    fdr_cfg.include_startup = env_int("HEAPFDR_STARTUP", 0, 0, 1);
    fdr_cfg.trace_children  = env_int("HEAPFDR_CHILDREN", 0, 0, 1);
    fdr_cfg.quiet           = env_int("HEAPFDR_QUIET", 0, 0, 1);
    env_str("HEAPFDR_LOG_DIR", fdr_cfg.log_dir, sizeof fdr_cfg.log_dir, ".");
    env_str("HEAPFDR_SUPPRESS", fdr_cfg.suppress, sizeof fdr_cfg.suppress, "");

    if (strcmp(fdr_cfg.log_dir, "none") == 0)
        fdr_cfg.log_dir[0] = '\0';
}
