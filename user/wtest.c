#include "../src/lib/syscall.h"

/*
 * wtest — Phase 6 write-path test program.
 *
 * Writes a deterministic pattern to 8 scratch sectors at the end of
 * the disk via SYS_RAW_SECTOR (which routes to ata_write in inc.1),
 * reads them back via ata_read, and verifies byte-for-byte. Prints
 * "wtest: write/readback OK" on success, a FAIL line otherwise.
 * Run from the shell with:  exec wtest.elf
 */

#define NSECT 8

static unsigned char wbuf[512];
static unsigned char rbuf[512];

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts_u(const char *s) { sys_write(STDOUT, s, slen(s)); }

static char *itoa(long n, char *buf)
{
    char tmp[24]; int i = 0;
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }
    while (n) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
    int j = 0;
    while (i--) buf[j++] = tmp[i];
    buf[j] = '\0';
    return buf;
}

static void fill_pattern(long idx)
{
    for (long j = 0; j < 512; j++)
        wbuf[j] = (unsigned char)(idx * 31 + j * 7 + (j >> 3));
}

static void fail(const char *what, long idx)
{
    char buf[24];
    puts_u("wtest: ");
    puts_u(what);
    puts_u(" FAIL idx=");
    puts_u(itoa(idx, buf));
    puts_u("\r\n");
}

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;
    for (long i = 0; i < NSECT; i++) {
        fill_pattern(i);
        if (sys_raw_sector((unsigned long)i, wbuf, 1) != 0) {
            fail("write", i);
            sys_exit(1);
        }
    }
    for (long i = 0; i < NSECT; i++) {
        fill_pattern(i);
        if (sys_raw_sector((unsigned long)i, rbuf, 0) != 0) {
            fail("read", i);
            sys_exit(1);
        }
        for (long j = 0; j < 512; j++) {
            if (rbuf[j] != wbuf[j]) {
                fail("verify", i);
                sys_exit(1);
            }
        }
    }
    puts_u("wtest: write/readback OK (8 sectors)\r\n");
    sys_exit(0);
}
