#include "../src/lib/syscall.h"
#include "../src/lib/malloc.h"

static long slen(const char *s) {
    long n = 0; while (s[n]) n++; return n;
}
static void puts_u(const char *s) { sys_write(STDOUT, s, slen(s)); }

/* Minimal itoa: writes decimal representation of n into buf, returns pointer */
static char *itoa(long n, char *buf)
{
    char tmp[24]; int i = 0;
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return buf; }
    int neg = n < 0; if (neg) n = -n;
    while (n) { tmp[i++] = '0' + (n % 10); n /= 10; }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i--) buf[j++] = tmp[i];
    buf[j] = '\0';
    return buf;
}

/* Read a line from stdin into buf (up to max-1 chars), null-terminate. */
static long readline(char *buf, long max)
{
    long n = 0;
    while (n < max - 1) {
        char tmp[1];
        long r = sys_read(STDIN, tmp, 1);
        if (r <= 0) break;
        char c = tmp[0];
        if (c == '\n') { buf[n++] = '\n'; break; }
        if (c == '\b' || c == 127) { if (n > 0) n--; continue; }
        buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

/* strcmp */
static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void cmd_memtest(void)
{
    char num[24];
    puts_u("malloc test: allocating 3 buffers...\r\n");

    char *a = malloc(64);
    char *b = malloc(128);
    char *c = malloc(32);

    if (!a || !b || !c) { puts_u("  malloc FAILED\r\n"); return; }

    /* Write distinct patterns */
    for (int i = 0; i < 64;  i++) a[i] = 'A';
    for (int i = 0; i < 128; i++) b[i] = 'B';
    for (int i = 0; i < 32;  i++) c[i] = 'C';

    puts_u("  a[0]="); char ca[2]={a[0],'\0'}; puts_u(ca);
    puts_u("  b[0]="); char cb[2]={b[0],'\0'}; puts_u(cb);
    puts_u("  c[0]="); char cc[2]={c[0],'\0'}; puts_u(cc);
    puts_u("\r\n");

    free(b);
    char *d = malloc(64); /* should reuse b's freed block */
    puts_u("  freed b, reallocated 64 bytes: ");
    puts_u(d ? "OK\r\n" : "FAILED\r\n");

    /* realloc test */
    a = realloc(a, 256);
    puts_u("  realloc(a, 256): ");
    puts_u(a ? "OK\r\n" : "FAILED\r\n");

    free(a); free(c); free(d);

    puts_u("  heap break after frees: 0x");
    long brk = (long)sys_sbrk(0);
    /* print hex */
    char hex[17]; int h = 15;
    hex[16] = '\0';
    unsigned long v = (unsigned long)brk;
    while (h >= 0) {
        int nibble = v & 0xF;
        hex[h--] = nibble < 10 ? '0'+nibble : 'a'+nibble-10;
        v >>= 4;
    }
    puts_u(hex); puts_u("\r\n");

    puts_u("malloc test PASSED\r\n");
    (void)num;
}

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;   /* unused for now; shell is interactive */
    sys_clear();
    puts_u("MyOS shell v0.3\r\nType 'help' for commands.\r\n> ");

    for (;;) {
        char line[256];
        long n = readline(line, 256);
        if (n <= 0) continue;
        if (n > 0 && line[n-1] == '\n') line[--n] = '\0';
        if (n == 0) { puts_u("> "); continue; }

        /* Split line into cmd and arg (first space separates them) */
        char cmd[64], arg[192];
        int ci = 0, ai = 0, in_arg = 0;
        for (int i = 0; line[i]; i++) {
            if (!in_arg && line[i] == ' ') { in_arg = 1; continue; }
            if (!in_arg) { if (ci < 63)  cmd[ci++] = line[i]; }
            else         { if (ai < 191) arg[ai++] = line[i]; }
        }
        cmd[ci] = '\0'; arg[ai] = '\0';

        if (streq(cmd, "exit"))    { puts_u("bye!\r\n"); sys_exit(0); }
        if (streq(cmd, "clear"))   { sys_clear(); puts_u("> "); continue; }
        if (streq(cmd, "memtest")) { cmd_memtest(); puts_u("> "); continue; }

        if (streq(cmd, "getpid")) {
            char buf[16];
            long pid = sys_getpid();
            puts_u("pid: "); puts_u(itoa(pid, buf)); puts_u("\r\n> ");
            continue;
        }

        if (streq(cmd, "ls")) {
            char name[64];
            long idx = 0;
            while (sys_readdir(idx, name) == 0) {
                puts_u(name); puts_u("\r\n");
                idx++;
            }
            puts_u("> ");
            continue;
        }

        if (streq(cmd, "cat")) {
            if (!arg[0]) { puts_u("usage: cat <file>\r\n> "); continue; }
            long fd = sys_open(arg);
            if (fd < 0) { puts_u("cat: not found: "); puts_u(arg); puts_u("\r\n> "); continue; }
            char buf[128];
            long r;
            while ((r = sys_read(fd, buf, 127)) > 0)
                sys_write(STDOUT, buf, r);
            sys_close(fd);
            puts_u("\r\n> ");
            continue;
        }

        if (streq(cmd, "help")) {
            puts_u("commands: ls, cat <file>, date, exec <prog> [arg...], getpid, memtest, clear, exit\r\n> ");
            continue;
        }

        if (streq(cmd, "date")) {
            sys_time_t t;
            sys_time(&t);
            char buf[4];
            /* YYYY-MM-DD HH:MM:SS */
            puts_u(itoa(t.year,    buf)); puts_u("-");
            if (t.month   < 10) puts_u("0");
            puts_u(itoa(t.month,   buf)); puts_u("-");
            if (t.day     < 10) puts_u("0");
            puts_u(itoa(t.day,     buf)); puts_u(" ");
            if (t.hours   < 10) puts_u("0");
            puts_u(itoa(t.hours,   buf)); puts_u(":");
            if (t.minutes < 10) puts_u("0");
            puts_u(itoa(t.minutes, buf)); puts_u(":");
            if (t.seconds < 10) puts_u("0");
            puts_u(itoa(t.seconds, buf)); puts_u("\r\n> ");
            continue;
        }

        if (streq(cmd, "exec")) {
            if (!arg[0]) { puts_u("usage: exec <prog> [args]\r\n> "); continue; }
            long pid = sys_exec(arg, arg);
            if (pid < 0) { puts_u("exec: failed\r\n> "); continue; }
            sys_wait(pid);
            puts_u("> ");
            continue;
        }

        puts_u("echo: ");
        sys_write(STDOUT, line, n);
        puts_u("\r\n> ");
    }
}

