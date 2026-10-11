/* SPDX-License-Identifier: MIT */
/*
 * telemetry-demo: a small, deliberately buggy telemetry pipeline.
 *
 * Packets are encoded, decoded, routed to a channel and logged. Each
 * scenario feeds input that walks into one real bug in the code below,
 * so heapfdr has something to find. Run without arguments for the list.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct packet {
    uint16_t apid;          /* which subsystem sent it */
    uint16_t seq;
    uint16_t len;
    uint8_t *payload;
};

struct channel {
    uint16_t    apid;
    const char *name;
    unsigned    received;
};

static struct channel channels[] = {
    { 0x101, "power",    0 },
    { 0x102, "thermal",  0 },
    { 0x103, "attitude", 0 },
};

/* ------------------------------------------------------------ codec */

/* Wire format: apid(2) seq(2) len(2) payload(len) checksum(1) */
static uint8_t checksum(const uint8_t *p, size_t n)
{
    uint8_t c = 0;

    while (n--)
        c ^= *p++;
    return c;
}

static size_t encode(uint8_t *out, uint16_t apid, uint16_t seq, const char *text, int corrupt)
{
    uint16_t len = (uint16_t)strlen(text);

    out[0] = (uint8_t)(apid >> 8);
    out[1] = (uint8_t)apid;
    out[2] = (uint8_t)(seq >> 8);
    out[3] = (uint8_t)seq;
    out[4] = (uint8_t)(len >> 8);
    out[5] = (uint8_t)len;
    memcpy(out + 6, text, len);
    out[6 + len] = checksum(out, 6u + len) ^ (corrupt ? 0x5a : 0);
    return 7u + len;
}

static struct packet *decode(const uint8_t *raw, size_t n)
{
    struct packet *pkt;

    if (n < 7)
        return NULL;
    pkt = malloc(sizeof *pkt);
    if (!pkt)
        return NULL;
    pkt->apid = (uint16_t)(raw[0] << 8 | raw[1]);
    pkt->seq = (uint16_t)(raw[2] << 8 | raw[3]);
    pkt->len = (uint16_t)(raw[4] << 8 | raw[5]);
    pkt->payload = malloc(pkt->len + 1u);
    if (!pkt->payload) {
        free(pkt);
        return NULL;
    }
    memcpy(pkt->payload, raw + 6, pkt->len);
    pkt->payload[pkt->len] = '\0';

    if (checksum(raw, n - 1) != raw[n - 1]) {
        free(pkt);          /* BUG: the payload is left behind on this path */
        return NULL;
    }
    return pkt;
}

static void release(struct packet *pkt)
{
    if (!pkt)
        return;
    free(pkt->payload);
    free(pkt);
}

/* ---------------------------------------------------------- routing */

static struct channel *find_channel(uint16_t apid)
{
    for (size_t i = 0; i < sizeof channels / sizeof channels[0]; i++)
        if (channels[i].apid == apid)
            return &channels[i];
    return NULL;
}

static void route(const struct packet *pkt)
{
    struct channel *ch = find_channel(pkt->apid);

    ch->received++;         /* BUG: assumes every apid is registered */
}

static char *describe(const struct packet *pkt)
{
    char line[96];

    snprintf(line, sizeof line, "apid=0x%03x seq=%u \"%s\"",
             pkt->apid, pkt->seq, (const char *)pkt->payload);
    return strdup(line);
}

static void log_packet(const struct packet *pkt, int verbose)
{
    char *text = describe(pkt);

    if (verbose) {
        printf("  %s\n", text);
        return;             /* BUG: early return skips the free below */
    }
    free(text);
}

static uint16_t last_seq;

static void check_sequence(const struct packet *pkt)
{
    if (pkt->seq < last_seq) {
        fprintf(stderr, "fatal: sequence counter went backwards (%u -> %u)\n",
                last_seq, pkt->seq);
        abort();
    }
    last_seq = pkt->seq;
}

/* ---------------------------------------------------- frame handling */

struct uplink_job {
    uint8_t *frame;
    size_t   len;
};

static void transmit(struct uplink_job *job)
{
    /* ...hand the bytes to the radio... */
    free(job->frame);       /* done with it once it is on the wire */
}

static void cancel(struct uplink_job *job)
{
    free(job->frame);       /* BUG: transmit() already freed it */
}

#define FRAME_BYTES (1u << 20)

struct frame_cache {
    uint8_t *last;
    size_t   used;
};

static uint8_t *assemble(struct frame_cache *cache)
{
    uint8_t *frame = calloc(1, FRAME_BYTES);

    cache->last = frame;
    cache->used = 0;
    return frame;
}

static void flush_frame(uint8_t *frame)
{
    /* ...write it to storage... */
    free(frame);            /* the cache still points at it */
}

static void annotate(struct frame_cache *cache, const char *note)
{
    /* BUG: the cached frame was flushed already */
    memcpy(cache->last + cache->used, note, strlen(note) + 1);
}

/* Counts nesting in a command script such as "((a)(b))". */
static int nesting(const char *s, int depth)
{
    char note[512];

    if (*s == '\0')
        return depth;
    if (*s == '(')
        return nesting(s + 1, depth + 1);
    if (*s == ')')
        return nesting(s + 1, depth - 1);

    /* BUG: other characters are "skipped" without moving forward */
    snprintf(note, sizeof note, "skipping '%c' at depth %d", *s, depth);
    return nesting(s, depth) + (note[0] == '\0');
}

/* -------------------------------------------------------- scenarios */

static const char *const samples[] = { "bus 28.1V", "panel +41C", "rate 0.02deg/s" };

static void run_clean(void)
{
    uint8_t raw[128];

    for (uint16_t i = 0; i < 6; i++) {
        size_t n = encode(raw, (uint16_t)(0x101 + i % 3), i, samples[i % 3], 0);
        struct packet *pkt = decode(raw, n);

        if (!pkt)
            continue;
        route(pkt);
        log_packet(pkt, 0);
        release(pkt);
    }
    printf("  routed 6 packets\n");
}

static void run_leak(void)
{
    uint8_t raw[128];

    for (uint16_t i = 0; i < 6; i++) {
        int corrupt = (i == 1 || i == 4);
        size_t n = encode(raw, (uint16_t)(0x101 + i % 3), i, samples[i % 3], corrupt);
        struct packet *pkt = decode(raw, n);

        if (!pkt) {
            printf("  dropped packet %u: bad checksum\n", i);
            continue;
        }
        route(pkt);
        log_packet(pkt, i % 2 == 0);
        release(pkt);
    }
}

static void run_double_free(void)
{
    struct uplink_job job = { .frame = malloc(48), .len = 48 };

    memset(job.frame, 0x55, job.len);
    transmit(&job);
    cancel(&job);
    printf("  uplink job finished\n");
}

/* GCC can spot this one at compile time; let it through, it's the point. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
static void run_bad_free(void)
{
    uint8_t raw[128];
    size_t n = encode(raw, 0x102, 7, "panel +41C", 0);
    uint8_t *copy = malloc(n);
    uint8_t *body;

    memcpy(copy, raw, n);
    body = copy + 6;        /* skip the header */
    printf("  payload starts with '%c'\n", body[0]);
    free(body);             /* BUG: frees from the middle of the block */
}
#pragma GCC diagnostic pop

static void run_null(void)
{
    uint8_t raw[128];
    size_t n = encode(raw, 0x1f0, 1, "who am I", 0);    /* nobody registered 0x1f0 */
    struct packet *pkt = decode(raw, n);

    route(pkt);
    release(pkt);
}

static void run_use_after_free(void)
{
    struct frame_cache cache = { 0 };
    uint8_t *frame = assemble(&cache);

    flush_frame(frame);
    annotate(&cache, "eclipse entry");
}

static void run_stack_overflow(void)
{
    printf("  depth %d\n", nesting("((x))", 0));
}

static void run_abort(void)
{
    static const uint16_t seqs[] = { 10, 11, 12, 9 };
    uint8_t raw[128];

    for (size_t i = 0; i < sizeof seqs / sizeof seqs[0]; i++) {
        size_t n = encode(raw, 0x103, seqs[i], samples[2], 0);
        struct packet *pkt = decode(raw, n);

        check_sequence(pkt);
        release(pkt);
    }
}

static const struct {
    const char *name;
    void (*run)(void);
    const char *what;
} scenarios[] = {
    { "clean",          run_clean,          "valid traffic, everything is freed" },
    { "leak",           run_leak,           "an error path and an early return leak memory" },
    { "double-free",    run_double_free,    "an uplink frame is freed twice" },
    { "bad-free",       run_bad_free,       "free() gets a pointer into the middle of a block" },
    { "null",           run_null,           "an unknown apid leads to a NULL dereference" },
    { "use-after-free", run_use_after_free, "a frame is written after it was flushed" },
    { "stack-overflow", run_stack_overflow, "a parser recurses forever on bad input" },
    { "abort",          run_abort,          "a sanity check calls abort()" },
};

#define NSCENARIOS (sizeof scenarios / sizeof scenarios[0])

static void usage(void)
{
    printf("usage: telemetry-demo <scenario>\n\nscenarios:\n");
    for (size_t i = 0; i < NSCENARIOS; i++)
        printf("  %-15s %s\n", scenarios[i].name, scenarios[i].what);
}

int main(int argc, char **argv)
{
    /* line buffering keeps our output in order with heapfdr's reports */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc != 2) {
        usage();
        return 2;
    }
    for (size_t i = 0; i < NSCENARIOS; i++) {
        if (strcmp(argv[1], scenarios[i].name) == 0) {
            printf("telemetry-demo: %s\n", scenarios[i].name);
            scenarios[i].run();
            return 0;
        }
    }
    usage();
    return 2;
}
