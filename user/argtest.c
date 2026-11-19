/* argtest: print argc and every argv string, then exit.
 * Proof program for Phase 11 inc.4 (real argv passing): the shell
 * runs `exec argtest.elf one two three` and this prints
 *   argc=4
 *   argv[0]=argtest.elf
 *   argv[1]=one
 *   ...
 * Before inc.4, argc/argv were register garbage and every program
 * ignored them. */
#include "../src/lib/syscall.h"

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts_u(const char *s) { sys_write(1, s, slen(s)); }

static char *itoa(long n, char *buf)
{
    char tmp[24];
    int i = 0, j = 0;
    if (n == 0) { buf[0] = '0'; buf[1] = 0; return buf; }
    int neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    while (n) { tmp[i++] = (char)('0' + (n % 10)); n /= 10; }
    if (neg) tmp[i++] = '-';
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}

void _start(long argc, char **argv)
{
    char buf[32];
    puts_u("argc=");
    puts_u(itoa(argc, buf));
    puts_u("\r\n");
    for (long i = 0; i < argc; i++) {
        puts_u("argv[");
        puts_u(itoa(i, buf));
        puts_u("]=");
        puts_u(argv[i]);
        puts_u("\r\n");
    }
    sys_exit(0);
}
