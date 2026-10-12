#include "../src/lib/syscall.h"

/*
 * demand_test -- Phase 2 demand-paging test (increment 2).
 *
 * Writes a distinct pattern to NPAGES pages inside the kernel's demand
 * region (0x20000000000, 16 pages), then reads everything back and
 * verifies it. Each page's first touch raises one #PF, serviced by the
 * demand pager (allocate + map + resume); no halt is expected.
 * Prints "[demand] PASS" on success, "[demand] FAIL ..." otherwise.
 *
 * Build: wired into the Makefile next to the benchmarks; the ELF is
 * packed into disk.img by the `disk` target. Run from the shell with:
 *     exec demand_test.elf
 */

#define DEMAND_BASE 0x20000000000UL
#define NPAGES      4

static long slen(const char *s) {
    long n = 0; while (s[n]) n++; return n;
}
static void puts_u(const char *s) { sys_write(STDOUT, s, slen(s)); }

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

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[32];
    volatile unsigned char *base = (volatile unsigned char *)DEMAND_BASE;

    puts_u("demand_test: writing ");
    puts_u(itoa(NPAGES, buf));
    puts_u(" pages in the demand region\r\n");

    /* Write phase: first touch of each page faults, demand pager maps it. */
    for (long p = 0; p < NPAGES; p++) {
        volatile unsigned char *pg = base + p * 4096;
        for (long i = 0; i < 4096; i++)
            pg[i] = (unsigned char)((p * 31 + i * 7) & 0xFF);
    }

    /* Verify phase: pages are mapped now, no faults expected here. */
    for (long p = 0; p < NPAGES; p++) {
        volatile unsigned char *pg = base + p * 4096;
        for (long i = 0; i < 4096; i++) {
            unsigned char want = (unsigned char)((p * 31 + i * 7) & 0xFF);
            if (pg[i] != want) {
                puts_u("[demand] FAIL: page ");
                puts_u(itoa(p, buf));
                puts_u(" offset ");
                puts_u(itoa(i, buf));
                puts_u("\r\n");
                sys_exit(1);
            }
        }
    }

    puts_u("[demand] PASS: ");
    puts_u(itoa(NPAGES, buf));
    puts_u(" demand pages written and verified\r\n");
    sys_exit(0);
}
