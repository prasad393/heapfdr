/* SPDX-License-Identifier: MIT */
/*
 * heap-api: calls every allocation function heapfdr wraps.
 *
 *   heap-api        everything is freed again, expect "no leaks"
 *   heap-api leak   leaves one block behind per allocator
 */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *volatile keep[8];

static void round_trip(void)
{
    void *p, *q;
    char *s;
    FILE *f;

    p = malloc(10);
    p = realloc(p, 100);
    p = realloc(p, 5);
    free(p);

    free(calloc(4, 25));

    p = reallocarray(NULL, 8, 16);
    p = reallocarray(p, 16, 16);
    free(p);

    if (posix_memalign(&p, 64, 200) == 0)
        free(p);
    free(aligned_alloc(32, 64));
    free(memalign(128, 300));
    free(valloc(1000));

    p = malloc(50);
    q = realloc(p, 0);          /* glibc frees p and returns NULL */
    free(q);

    s = strdup("telemetry");
    free(s);

    f = fopen("/dev/null", "r");
    if (f)
        fclose(f);
}

static void leave_one_of_each(void)
{
    void *p = NULL;

    keep[0] = malloc(11);
    keep[1] = calloc(1, 22);
    keep[2] = realloc(NULL, 33);
    keep[3] = aligned_alloc(64, 64);
    if (posix_memalign(&p, 128, 128) == 0)
        keep[4] = p;
}

int main(int argc, char **argv)
{
    round_trip();
    if (argc > 1 && strcmp(argv[1], "leak") == 0)
        leave_one_of_each();
    puts("heap-api: done");
    return 0;
}
