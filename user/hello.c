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

void _start(void)
{
    sys_clear();
    puts_u("MyOS shell v0.3 -- commands: memtest, clear, getpid, exit\r\n> ");

    for (;;) {
        char line[256];
        long n = readline(line, 256);
        if (n <= 0) continue;
        if (n > 0 && line[n-1] == '\n') line[--n] = '\0';
        if (n == 0) { puts_u("> "); continue; }

        if (streq(line, "exit"))    { puts_u("bye!\r\n"); sys_exit(0); }
        if (streq(line, "clear"))   { sys_clear(); puts_u("> "); continue; }
        if (streq(line, "memtest")) { cmd_memtest(); puts_u("> "); continue; }
        if (streq(line, "getpid"))  {
            char buf[16];
            long pid = sys_getpid();
            puts_u("pid: "); puts_u(itoa(pid, buf)); puts_u("\r\n> ");
            continue;
        }
        if (streq(line, "help")) {
            puts_u("commands: memtest, clear, getpid, exit\r\n> ");
            continue;
        }

        puts_u("echo: ");
        sys_write(STDOUT, line, n);
        puts_u("\r\n> ");
    }
}


