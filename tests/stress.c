/* SPDX-License-Identifier: MIT */
/*
 * heap-stress: several threads doing random malloc/calloc/realloc/free.
 * Used to check that heapfdr's bookkeeping stays exact under contention.
 *
 *   heap-stress        everything is freed, expect "no leaks"
 *   heap-stress N      also leaks exactly N blocks of 100 bytes
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define THREADS 4
#define ROUNDS  20000
#define SLOTS   64

static void *volatile sink;

static unsigned next_rand(unsigned *state)
{
    *state = *state * 1103515245u + 12345u;
    return *state >> 8;
}

static void *worker(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u + 1u;
    void *slot[SLOTS] = { 0 };

    for (int i = 0; i < ROUNDS; i++) {
        unsigned r = next_rand(&seed);
        int k = (int)(r % SLOTS);
        size_t size = 1 + (r >> 6) % 512;
        void *q;

        switch ((r >> 3) % 4) {
        case 0:
            free(slot[k]);
            slot[k] = malloc(size);
            break;
        case 1:
            free(slot[k]);
            slot[k] = calloc(1, size);
            break;
        case 2:
            q = realloc(slot[k], size);
            if (q)
                slot[k] = q;
            break;
        default:
            free(slot[k]);
            slot[k] = NULL;
            break;
        }
        if (slot[k])
            *(volatile char *)slot[k] = (char)r;
    }
    for (int k = 0; k < SLOTS; k++)
        free(slot[k]);
    return NULL;
}

__attribute__((noinline)) static void keep_forever(int count)
{
    for (int i = 0; i < count; i++)
        sink = malloc(100);
}

int main(int argc, char **argv)
{
    pthread_t threads[THREADS];

    for (uintptr_t i = 0; i < THREADS; i++)
        pthread_create(&threads[i], NULL, worker, (void *)i);
    for (int i = 0; i < THREADS; i++)
        pthread_join(threads[i], NULL);

    if (argc > 1)
        keep_forever(atoi(argv[1]));

    printf("heap-stress: %d threads x %d operations\n", THREADS, ROUNDS);
    return 0;
}
