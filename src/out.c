/* SPDX-License-Identifier: MIT */
/*
 * Report output. Only write(2) and a fixed buffer are used here, which
 * keeps it usable from inside malloc() and from the crash handler, where
 * printf() and friends are off limits.
 */
#include <errno.h>
#include <unistd.h>

#include "heapfdr.h"

void out_open(struct fdr_out *o, int fd)
{
    o->nfds = 0;
    o->len = 0;
    o->col = 0;
    if (fd >= 0)
        o->fds[o->nfds++] = fd;
}

void out_also(struct fdr_out *o, int fd)
{
    if (fd >= 0 && o->nfds < 2)
        o->fds[o->nfds++] = fd;
}

static void write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

void out_flush(struct fdr_out *o)
{
    for (int i = 0; i < o->nfds; i++)
        write_all(o->fds[i], o->buf, o->len);
    o->len = 0;
}

void out_chr(struct fdr_out *o, char c)
{
    if (o->len == sizeof o->buf)
        out_flush(o);
    o->buf[o->len++] = c;
    o->col = (c == '\n') ? 0 : o->col + 1;
}

void out_str(struct fdr_out *o, const char *s)
{
    if (!s)
        s = "(null)";
    while (*s)
        out_chr(o, *s++);
}

void out_udec(struct fdr_out *o, unsigned long long v)
{
    char tmp[24];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        out_chr(o, tmp[--n]);
}

void out_hex(struct fdr_out *o, uintptr_t v, int min_digits)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[2 * sizeof v];
    int n = 0;

    do {
        tmp[n++] = digits[v & 0xf];
        v >>= 4;
    } while (v);
    while (n < min_digits && n < (int)sizeof tmp)
        tmp[n++] = '0';

    out_str(o, "0x");
    while (n)
        out_chr(o, tmp[--n]);
}

/* 1536 -> "1.5 KiB". One decimal place is enough for a report. */
void out_size(struct fdr_out *o, uint64_t bytes)
{
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    uint64_t whole = bytes, rem = 0;
    int u = 0;

    if (bytes < 1024) {
        out_udec(o, bytes);
        out_str(o, " B");
        return;
    }
    while (whole >= 1024 && u < 4) {
        rem = whole % 1024;
        whole /= 1024;
        u++;
    }
    out_udec(o, whole);
    out_chr(o, '.');
    out_udec(o, rem * 10 / 1024);
    out_chr(o, ' ');
    out_str(o, units[u]);
}

/* Nanoseconds as milliseconds with three decimals: "12.345 ms". */
void out_ms(struct fdr_out *o, uint64_t ns)
{
    uint64_t us = ns / 1000;
    uint64_t frac = us % 1000;

    out_udec(o, us / 1000);
    out_chr(o, '.');
    out_chr(o, (char)('0' + frac / 100));
    out_chr(o, (char)('0' + frac / 10 % 10));
    out_chr(o, (char)('0' + frac % 10));
    out_str(o, " ms");
}

/* "1 block", "3 blocks" */
void out_count(struct fdr_out *o, uint64_t n, const char *noun)
{
    out_udec(o, n);
    out_chr(o, ' ');
    out_str(o, noun);
    if (n != 1)
        out_chr(o, 's');
}

/* Pad with spaces up to a column, always leaving at least one. */
void out_pad(struct fdr_out *o, size_t col)
{
    do
        out_chr(o, ' ');
    while (o->col < col);
}

void out_tag(struct fdr_out *o)
{
    out_str(o, FDR_TAG);
}

void out_nl(struct fdr_out *o)
{
    out_chr(o, '\n');
    out_flush(o);
}
