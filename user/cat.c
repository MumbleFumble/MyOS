/*
 * cat.elf - Phase 17 inc.6 standalone cat.
 *
 * With a file argument (argv[1]): open it and copy its
 * bytes to fd 1 exactly. With no argument: copy fd 0 to
 * fd 1 until EOF. Written against fds/argv only; no shell
 * state. Byte-exact in both modes, including content
 * larger than the 1024-byte pipe ring and content with
 * no trailing newline.
 *
 * E_WOULDBLOCK on any fd is retried, never treated as
 * EOF or error (the pipe layer is non-blocking).
 *
 * Exit codes (distinct per failure class):
 *   0 success
 *   2 open failed (file argument names no file)
 *   3 read error
 *   4 write error
 *   5 close failure on the success path
 *
 * Close discipline (the inc-005 contract: task exit
 * tears down NOTHING): the opened file and fd 1 are
 * closed on EVERY exit path, error paths included - the
 * close of fd 1 is what delivers EOF downstream when the
 * slot holds the last alias of a pipe write end. On the
 * success path a failed close is its own class (5); note
 * a standalone run's fd 1 is the pinned console slot, so
 * its close refuses by design and a shell-run cat exits
 * 5 after printing - the shell does not collect the code,
 * and in the wired case the close succeeds and the code
 * is 0.
 */
#include "../src/lib/syscall.h"

/* Copy in_fd -> fd 1. Returns 0, or 3 (read) / 4 (write). */
static int copy_to_stdout(long in_fd)
{
    char buf[256];
    for (;;) {
        long r = sys_read(in_fd, buf, sizeof buf);
        if (r == E_WOULDBLOCK) continue;
        if (r == 0) return 0;
        if (r < 0) return 3;
        long off = 0;
        while (off < r) {
            long w = sys_write(1, buf + off, r - off);
            if (w == E_WOULDBLOCK) continue;
            if (w < 0) return 4;
            off += w;
        }
    }
}

static void finish(long file_fd, int code) __attribute__((noreturn));
static void finish(long file_fd, int code)
{
    int close_bad = 0;
    if (file_fd >= 0 && sys_close(file_fd) != 0) close_bad = 1;
    if (sys_close(1) != 0) close_bad = 1;
    if (code == 0 && close_bad) code = 5;
    sys_exit(code);
}

void _start(long argc, char **argv)
{
    long in_fd = 0;
    long file_fd = -1;
    if (argc >= 2) {
        file_fd = sys_open(argv[1]);
        if (file_fd < 0) finish(-1, 2);
        in_fd = file_fd;
    }
    finish(file_fd, copy_to_stdout(in_fd));
}
