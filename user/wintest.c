/* wintest: proof program for Phase 15 inc.4 (receive window).
 * Accepts one connection and reads it on a schedule the host
 * peer can observe through the advertised window field:
 *   1. a blocking recv of exactly 1 byte (returns when the
 *      peer's first 1024-byte burst lands, leaving 1023 held);
 *   2. a spin, while the peer's second burst fills the
 *      receive buffer to a zero advertised window;
 *   3. a 512-byte read, which must trigger the guest's
 *      window-update ACK (observed by the peer, which then
 *      probes the reopened window with an old-sequence
 *      zero-length segment before sending more);
 *   4. a second spin, then reads to the end of the stream,
 *      verifying every byte against the pattern, and replies
 *      "WIN-OK".
 * Prints "wintest: ok <name>" per check and "wintest: ALL
 * PASS" at the end; any failure prints "wintest: FAIL <name>".
 */
#include "../src/lib/syscall.h"

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts_u(const char *s) { sys_write(1, s, slen(s)); }

static int fails = 0;
static void check(int ok, const char *name)
{
    if (ok) {
        puts_u("wintest: ok ");
    } else {
        puts_u("wintest: FAIL ");
        fails++;
    }
    puts_u(name);
    puts_u("\r\n");
}

static unsigned char pat(long o) { return (unsigned char)((o * 7 + 3) % 256); }

static void spin(unsigned long n)
{
    volatile unsigned long i;
    for (i = 0; i < n; i++) { }
}

static unsigned char buf[2048];
static long total = 0;

static int verify(long start, long n)
{
    long i;
    for (i = 0; i < n; i++)
        if (buf[i] != pat(start + i))
            return 0;
    return 1;
}

void _start(void)
{
    struct tcpaccept_out out;
    long r;

    puts_u("wintest: start\r\n");
    if (sys_tcplisten(8080) != 0) {
        check(0, "listen");
        sys_exit(1);
    }
    if (sys_tcpaccept(&out) != 0) {
        check(0, "accept");
        sys_exit(1);
    }
    puts_u("wintest: accepted\r\n");

    r = sys_tcpsrv_recv(buf, 1);
    check(r == 1 && verify(0, 1), "first byte");
    total = 1;

    spin(60000000UL);

    r = sys_tcpsrv_recv(buf, 512);
    check(r == 512 && verify(total, r), "held read 512");
    if (r > 0)
        total += r;

    spin(150000000UL);

    while (total < 2561) {
        r = sys_tcpsrv_recv(buf, 2048);
        if (r <= 0)
            break;
        if (!verify(total, r)) {
            check(0, "pattern in rest");
            break;
        }
        total += r;
    }
    check(total == 2561, "received 2561 bytes");

    if (sys_tcpsrv_send("WIN-OK", 6) != 6)
        check(0, "reply");
    sys_tcpsrv_close();

    if (fails == 0)
        puts_u("wintest: ALL PASS\r\n");
    else
        puts_u("wintest: FAILURES\r\n");
    sys_exit(0);
}
