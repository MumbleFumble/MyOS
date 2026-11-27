/*
 * wc.elf - Phase 17 inc.6 standalone wc.
 *
 * Reads fd 0 until EOF (stdin only, no file argument),
 * counts lines (number of '\n' bytes), words (maximal
 * runs of non-whitespace; whitespace = space, tab, '\r',
 * '\n'), and bytes (total read), then writes exactly one
 * line to fd 1: "<lines> <words> <bytes>\n" (single
 * spaces, decimal, no leading spaces, no filename).
 *
 * E_WOULDBLOCK is retried, never treated as EOF.
 *
 * Exit codes (distinct per failure class):
 *   0 success
 *   2 read error
 *   3 write error
 *   4 close failure on the success path
 *
 * Close discipline (the inc-005 contract: task exit
 * tears down NOTHING): fd 1 is closed on every exit
 * path - that close delivers EOF to whoever reads wc's
 * output pipe. As with cat.elf, a standalone run's fd 1
 * is the pinned console slot and its close refuses by
 * design (exit 4 after printing; the shell does not
 * collect the code).
 */
#include "../src/lib/syscall.h"

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void finish(int code) __attribute__((noreturn));
static void finish(int code)
{
    if (sys_close(1) != 0 && code == 0) code = 4;
    sys_exit(code);
}

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[256];
    long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    for (;;) {
        long r = sys_read(0, buf, sizeof buf);
        if (r == E_WOULDBLOCK) continue;
        if (r == 0) break;
        if (r < 0) finish(2);
        for (long i = 0; i < r; i++) {
            char c = buf[i];
            bytes++;
            if (c == '\n') lines++;
            if (is_ws(c)) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }

    /* Format "<lines> <words> <bytes>\n" by hand. */
    char out[64];
    int n = 0;
    long vals[3];
    vals[0] = lines; vals[1] = words; vals[2] = bytes;
    for (int v = 0; v < 3; v++) {
        char dig[20];
        int d = 0;
        long x = vals[v];
        if (x == 0) dig[d++] = '0';
        while (x > 0) { dig[d++] = (char)('0' + x % 10); x /= 10; }
        while (d > 0) out[n++] = dig[--d];
        out[n++] = (v < 2) ? ' ' : '\n';
    }

    long off = 0;
    while (off < n) {
        long w = sys_write(1, out + off, n - off);
        if (w == E_WOULDBLOCK) continue;
        if (w < 0) finish(3);
        off += w;
    }
    finish(0);
}
