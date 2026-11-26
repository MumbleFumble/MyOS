#include "../src/lib/syscall.h"
#include "../src/lib/malloc.h"
#include "glob.h"

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

static void cmd_sched(void)
{
    /* State names matching task_state_t order */
    static const char * const state_names[] = { "READY", "RUN  ", "DEAD ", "WAIT " };
    task_stat_t stats[32];
    long count = sys_telemetry(stats, 32);
    if (count < 0) { puts_u("sched: telemetry failed\r\n"); return; }
    char buf[24];
    puts_u("PID  STATE  SYSCALLS  IO_BLKS  TICKS     WAIT      NAME\r\n");
    puts_u("---  -----  --------  -------  --------  --------  ----\r\n");
    for (long i = 0; i < count; i++) {
        task_stat_t *s = &stats[i];
        /* PID */
        itoa(s->pid, buf);
        int pad = 5 - (int)slen(buf); while (pad-- > 0) puts_u(" ");
        puts_u(buf); puts_u("  ");
        /* State */
        unsigned char st = s->state < 4 ? s->state : 0;
        puts_u(state_names[st]); puts_u("  ");
        /* syscall_count */
        itoa((long)s->syscall_count, buf);
        pad = 8 - (int)slen(buf); while (pad-- > 0) puts_u(" ");
        puts_u(buf); puts_u("  ");
        /* io_block_count */
        itoa((long)s->io_block_count, buf);
        pad = 7 - (int)slen(buf); while (pad-- > 0) puts_u(" ");
        puts_u(buf); puts_u("  ");
        /* total_ticks */
        itoa((long)s->total_ticks, buf);
        pad = 8 - (int)slen(buf); while (pad-- > 0) puts_u(" ");
        puts_u(buf); puts_u("  ");
        /* wait_ticks */
        itoa((long)s->wait_ticks, buf);
        pad = 8 - (int)slen(buf); while (pad-- > 0) puts_u(" ");
        puts_u(buf); puts_u("  ");
        /* name */
        puts_u(s->name); puts_u("\r\n");
    }
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

/* Static drain buffer: 8 records x 1304 bytes = ~10.2 KiB (static, not stack). */
/* Static drain buffer: 8 records x 1304 bytes = ~10.2 KiB (static, not stack).
 * The kernel ring holds 256 records, so cmd_trace drains in a loop. */
static switch_record_t trace_buf[8];
static pf_record_t pf_buf[8];
static io_record_t io_buf[8];
static access_record_t acc_buf[16];

static void cmd_trace(void)
{
    char buf[24];
    long total = 0;
    /* Loop until a drain comes back empty. Cap at 32 iterations (256 records
     * = full ring) so we cannot spin forever on a live system that keeps
     * appending records while we print. */
    for (long iter = 0; iter < 32; iter++) {
        long n = sys_telemetry_drain(trace_buf, 8);
        if (n < 0) { puts_u("trace: drain failed\r\n"); return; }
        if (n == 0) break;
        for (long i = 0; i < n; i++) {
            switch_record_t *r = &trace_buf[i];
            puts_u("["); puts_u(itoa(total + i, buf)); puts_u("] tick=");
            puts_u(itoa((long)r->tick, buf)); puts_u(" ");
            puts_u(itoa((long)r->prev_idx, buf)); puts_u(" -> ");
            puts_u(itoa((long)r->next_idx, buf)); puts_u("  tasks=");
            puts_u(itoa((long)r->task_count, buf)); puts_u("\r\n");
        }
        total += n;
    }
    puts_u("drained "); puts_u(itoa(total, buf)); puts_u(" switch records\r\n");
}

/* Page-fault telemetry: drains the kernel pf ring and prints records. */
static void cmd_pftrace(void)
{
    char buf[24];
    char hex[19];
    long total = 0;
    for (long iter = 0; iter < 32; iter++) {
        long n = sys_pf_drain(pf_buf, 8);
        if (n < 0) { puts_u("pftrace: drain failed\r\n"); return; }
        if (n == 0) break;
        for (long i = 0; i < n; i++) {
            pf_record_t *r = &pf_buf[i];
            puts_u("["); puts_u(itoa(total + i, buf)); puts_u("] tick=");
            puts_u(itoa((long)r->tick, buf));
            puts_u(" cr2=0x");
            unsigned long v = (unsigned long)r->cr2;
            for (int h = 0; h < 16; h++) {
                int nib = (v >> (60 - h*4)) & 0xF;
                hex[h] = nib < 10 ? '0'+nib : 'a'+nib-10;
            }
            hex[16] = '\0';
            puts_u(hex);
            puts_u(" err="); puts_u(itoa((long)r->err_code, buf));
            puts_u(" pid="); puts_u(itoa((long)r->pid, buf));
            puts_u(" resolved="); puts_u(itoa((long)r->resolved, buf));
            puts_u("\r\n");
        }
        total += n;
    }
    puts_u("drained "); puts_u(itoa(total, buf)); puts_u(" fault records\r\n");
}

/* Block cache stats: shows hits/misses. */
static void cmd_bcachestat(void)
{
    char buf[24];
    unsigned long hits = 0, misses = 0, dirty = 0;
    if (sys_bcache_stat(&hits, &misses, &dirty) != 0) {
        puts_u("bcachestat: failed\r\n");
        return;
    }
    puts_u("hits="); puts_u(itoa((long)hits, buf));
    puts_u(" misses="); puts_u(itoa((long)misses, buf));
    puts_u(" dirty="); puts_u(itoa((long)dirty, buf));
    unsigned long total = hits + misses;
    if (total > 0) {
        /* hit rate as integer percent */
        unsigned long pct = (hits * 100) / total;
        puts_u(" hit_rate="); puts_u(itoa((long)pct, buf)); puts_u("%");
    }
    puts_u("\r\n");
}

/* ATA DMA stats: shows sectors moved by DMA vs PIO. */
static void cmd_dmastat(void)
{
    char buf[24];
    unsigned long dma = 0, err = 0, pio = 0;
    if (sys_dma_stat(&dma, &err, &pio) != 0) {
        puts_u("dmastat: failed\r\n");
        return;
    }
    puts_u("dma_sectors="); puts_u(itoa((long)dma, buf));
    puts_u(" dma_errors="); puts_u(itoa((long)err, buf));
    puts_u(" pio_sectors="); puts_u(itoa((long)pio, buf));
    puts_u("\r\n");
}

static void print_ip(const unsigned char ip[4]);

/* Network status (Phase 7): MAC, link, frame counters. */
static void puthex2(unsigned char v)
{
    static const char h[] = "0123456789abcdef";
    char b[3];
    b[0] = h[(v >> 4) & 0xF]; b[1] = h[v & 0xF]; b[2] = 0;
    puts_u(b);
}

static void cmd_netstat(void)
{
    char buf[32];
    struct netstat_out o;
    if (sys_netstat(&o) != 0 || !o.present) {
        puts_u("net: no NIC\r\n");
        return;
    }
    puts_u("mac=");
    for (int i = 0; i < 6; i++) {
        if (i) puts_u(":");
        puthex2(o.mac[i]);
    }
    puts_u(o.link ? " link=up" : " link=down");
    puts_u(" ip="); print_ip(o.ip);
    puts_u(" rx="); puts_u(itoa((long)o.rx_frames, buf));
    puts_u(" tx="); puts_u(itoa((long)o.tx_frames, buf));
    puts_u(" irq="); puts_u(itoa((long)o.irq_rx, buf));
    puts_u("\r\n");
    puts_u("arp rx="); puts_u(itoa((long)o.arp_rx, buf));
    puts_u(" tx="); puts_u(itoa((long)o.arp_tx, buf));
    puts_u(" icmp rx="); puts_u(itoa((long)o.icmp_rx, buf));
    puts_u(" tx="); puts_u(itoa((long)o.icmp_tx, buf));
    puts_u(" udp rx="); puts_u(itoa((long)o.udp_rx, buf));
    puts_u(" tx="); puts_u(itoa((long)o.udp_tx, buf));
    puts_u(" badck="); puts_u(itoa((long)o.bad_cksum, buf));
    puts_u("\r\n");
}

static void cmd_netsend(void)
{
    if (sys_netsend() != 0) { puts_u("netsend: failed\r\n"); return; }
    puts_u("netsent\r\n");
}

/* dhcp: obtain address/gateway/DNS via DHCP (Phase 8). */
static void cmd_dhcp(void)
{
    char buf[16];
    struct dhcp_out o;
    if (sys_dhcp(&o) != 0) { puts_u("dhcp: failed\r\n"); return; }
    puts_u("dhcp: bound "); print_ip(o.ip);
    puts_u(" gw "); print_ip(o.gw);
    puts_u(" dns "); print_ip(o.dns);
    puts_u(" lease="); puts_u(itoa((long)o.lease_secs, buf));
    puts_u("s renewals="); puts_u(itoa((long)o.renewals, buf));
    puts_u("\r\n");
}
/* Parse "a.b.c.d" into ip[4]; 1 on success. */
static int parse_ip(const char *s, unsigned char ip[4])
{
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        int v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
        if (v > 255) return 0;
        ip[part] = (unsigned char)v;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    return *s == 0;
}

static void print_ip(const unsigned char ip[4])
{
    char buf[32];
    for (int i = 0; i < 4; i++) {
        if (i) puts_u(".");
        puts_u(itoa((long)ip[i], buf));
    }
}

/* ping <ip>: four ICMP echo requests via SYS_PING. */
static void cmd_ping(const char *arg)
{
    char buf[32];
    unsigned char ip[4];
    if (!parse_ip(arg, ip)) { puts_u("ping: bad address\r\n"); return; }
    for (int seq = 1; seq <= 4; seq++) {
        long r = sys_ping(ip, (unsigned short)seq);
        if (r < 0) {
            puts_u("ping: no reply seq=");
            puts_u(itoa((long)seq, buf));
            puts_u("\r\n");
        } else {
            puts_u("reply from ");
            print_ip(ip);
            puts_u(": seq="); puts_u(itoa((long)seq, buf));
            puts_u(" time="); puts_u(itoa(r * 10, buf));
            puts_u("ms\r\n");
        }
    }
}

/* write <file> <text>: create/truncate a disk file with text + LF.
 * append <file> <text>: read-modify-write in the shell (no seek). */
static char fbuf[4096];

static int split_name_text(const char *arg, char *name_out,
                            const char **text_out)
{
    int ni = 0;
    while (arg[ni] && arg[ni] != ' ' && ni < 63) {
        name_out[ni] = arg[ni]; ni++;
    }
    name_out[ni] = 0;
    const char *t = arg + ni;
    while (*t == ' ') t++;
    *text_out = t;
    return ni > 0;
}

static long slen_u(const char *s) { long n = 0; while (s[n]) n++; return n; }

static void cmd_write(const char *arg)
{
    char name[64]; const char *text;
    if (!split_name_text(arg, name, &text)) {
        puts_u("usage: write <file> <text>\r\n"); return;
    }
    long fd = sys_fcreate(name);
    if (fd < 0) { puts_u("write: failed\r\n"); return; }
    sys_write(fd, text, slen_u(text));
    sys_write(fd, "\n", 1);
    sys_close(fd);
    puts_u("written\r\n");
}

static void cmd_append(const char *arg)
{
    char name[64]; const char *text;
    if (!split_name_text(arg, name, &text)) {
        puts_u("usage: append <file> <text>\r\n"); return;
    }
    long fd = sys_open(name);
    if (fd < 0) { puts_u("append: no such file\r\n"); return; }
    long n = sys_read(fd, fbuf, (long)sizeof(fbuf) - 1);
    sys_close(fd);
    if (n < 0) n = 0;
    fd = sys_fcreate(name);
    if (fd < 0) { puts_u("append: failed\r\n"); return; }
    if (n) sys_write(fd, fbuf, n);
    sys_write(fd, text, slen_u(text));
    sys_write(fd, "\n", 1);
    sys_close(fd);
    puts_u("appended\r\n");
}

/* Flush dirty cache entries to disk. */
static void cmd_sync(void)
{
    if (sys_bcache_sync() != 0) {
        puts_u("sync: failed\r\n");
        return;
    }
    puts_u("synced\r\n");
}

/* ATA write-path stats: sectors written by DMA vs PIO. */
static void cmd_wstat(void)
{
    char buf[24];
    unsigned long dma = 0, err = 0, pio = 0;
    if (sys_wstat(&dma, &err, &pio) != 0) {
        puts_u("wstat: failed\r\n");
        return;
    }
    puts_u("dma_write_sectors="); puts_u(itoa((long)dma, buf));
    puts_u(" dma_write_errors="); puts_u(itoa((long)err, buf));
    puts_u(" pio_write_sectors="); puts_u(itoa((long)pio, buf));
    puts_u("\r\n");
}

/* I/O telemetry: drains the kernel I/O ring and prints records. */
static void cmd_iotrace(void)
{
    char buf[24];
    long total = 0;
    for (long iter = 0; iter < 32; iter++) {
        long n = sys_io_drain(io_buf, 8);
        if (n < 0) { puts_u("iotrace: drain failed\r\n"); return; }
        if (n == 0) break;
        for (long i = 0; i < n; i++) {
            io_record_t *r = &io_buf[i];
            puts_u("["); puts_u(itoa(total + i, buf)); puts_u("] ");
            puts_u("lba="); puts_u(itoa((long)r->lba, buf));
            puts_u(" count="); puts_u(itoa((long)r->count, buf));
            puts_u(" lat="); puts_u(itoa((long)r->latency_us, buf));
            puts_u("us result="); puts_u(itoa((long)r->result, buf));
            puts_u("\r\n");
        }
        total += n;
    }
    puts_u("drained "); puts_u(itoa(total, buf)); puts_u(" I/O records\r\n");
}

/* Demand-access trace: drains the bcache access ring and prints
 * machine-readable ACC lines for offline prefetch training:
 *   ACC seq=<n> lba=<n> hit=<0|1> */
static void cmd_acctrace(void)
{
    char buf[24];
    long total = 0;
    for (long iter = 0; iter < 512; iter++) {
        long n = sys_access_drain(acc_buf, 16);
        if (n < 0) { puts_u("acctrace: drain failed\r\n"); return; }
        if (n == 0) break;
        for (long i = 0; i < n; i++) {
            access_record_t *r = &acc_buf[i];
            puts_u("ACC seq="); puts_u(itoa((long)r->seq, buf));
            puts_u(" lba="); puts_u(itoa((long)r->lba, buf));
            puts_u(" hit="); puts_u(itoa((long)r->hit, buf));
            puts_u(" file="); puts_u(itoa((long)r->file_id, buf));
            puts_u(" off="); puts_u(itoa((long)r->file_off, buf));
            puts_u("\r\n");
        }
        total += n;
    }
    puts_u("drained "); puts_u(itoa(total, buf)); puts_u(" access records\r\n");
}

/* Machine-readable full trace dump for offline ML training data.
 * Prints every drained record with all per-task snapshots, one REC line
 * followed by one TASK line per task slot:
 *   REC tick=<tick> prev=<prev_idx> next=<next_idx> ntasks=<n>
 *   TASK idx=<i> pid=<pid> state=<s> total=<total_ticks> wait=<wait_ticks>
 *        syscalls=<syscall_count> ioblocks=<io_block_count>
 * Parseable on the host with scripts/parse_trace.py. */
static void cmd_trace_full(void)
{
    char buf[24];
    long total = 0;
    for (long iter = 0; iter < 32; iter++) {
        long n = sys_telemetry_drain(trace_buf, 8);
        if (n < 0) { puts_u("trace: drain failed\r\n"); return; }
        if (n == 0) break;
        for (long i = 0; i < n; i++) {
            switch_record_t *r = &trace_buf[i];
            puts_u("REC tick="); puts_u(itoa((long)r->tick, buf));
            puts_u(" prev=");   puts_u(itoa((long)r->prev_idx, buf));
            puts_u(" next=");   puts_u(itoa((long)r->next_idx, buf));
            puts_u(" ntasks="); puts_u(itoa((long)r->task_count, buf));
            puts_u("\r\n");
            for (long t = 0; t < (long)r->task_count; t++) {
                trace_task_snapshot_t *s = &r->tasks[t];
                puts_u("TASK idx=");     puts_u(itoa(t, buf));
                puts_u(" pid=");         puts_u(itoa((long)s->pid, buf));
                puts_u(" state=");       puts_u(itoa((long)s->state, buf));
                puts_u(" total=");       puts_u(itoa((long)s->total_ticks, buf));
                puts_u(" wait=");        puts_u(itoa((long)s->wait_ticks, buf));
                puts_u(" syscalls=");    puts_u(itoa((long)s->syscall_count, buf));
                puts_u(" ioblocks=");    puts_u(itoa((long)s->io_block_count, buf));
                puts_u("\r\n");
            }
        }
        total += n;
    }
    puts_u("drained "); puts_u(itoa(total, buf)); puts_u(" switch records\r\n");
}

/* nslookup <name> [server]: resolve a DNS A record (Phase 9). */
static void cmd_nslookup(const char *arg)
{
    char name[128];
    int i = 0;
    while (arg[i] && arg[i] != ' ' && i < 127) { name[i] = arg[i]; i++; }
    name[i] = 0;
    unsigned char server[4];
    const unsigned char *sv = 0;
    if (arg[i] == ' ' && parse_ip(arg + i + 1, server))
        sv = server;
    unsigned char out[4];
    if (sys_nslookup(name, sv, out) != 0) {
        puts_u("nslookup: failed\r\n");
        return;
    }
    puts_u(name); puts_u(" = "); print_ip(out); puts_u("\r\n");
}
/* tcpecho <ip> <port> <text>: one TCP round trip (Phase 9). */
static void cmd_tcplisten(const char *arg)
{
    char buf[16];
    int port = 0;
    int i = 0;
    while (arg[i] >= '0' && arg[i] <= '9') { port = port * 10 + (arg[i] - '0'); i++; }
    if (i == 0 || port == 0 || port > 65535) {
        puts_u("usage: tcplisten <port>\r\n");
        return;
    }
    if (sys_tcplisten(port) != 0) { puts_u("tcplisten: listen failed\r\n"); return; }
    struct tcpaccept_out o;
    if (sys_tcpaccept(&o) != 0) { sys_tcpsrv_close(); puts_u("tcplisten: accept timed out\r\n"); return; }
    puts_u("tcplisten: accepted ");
    print_ip(o.ip);
    puts_u(":");
    puts_u(itoa((long)o.port, buf));
    puts_u("\r\n");
    sys_tcpsrv_close();
    puts_u("tcplisten: closed\r\n");
}

static void cmd_tcpechosrv(const char *arg)
{
    char buf[16];
    int port = 0;
    int i = 0;
    while (arg[i] >= '0' && arg[i] <= '9') { port = port * 10 + (arg[i] - '0'); i++; }
    if (i == 0 || port == 0 || port > 65535) {
        puts_u("usage: tcpechosrv <port>\r\n");
        return;
    }
    if (sys_tcplisten(port) != 0) { puts_u("tcpechosrv: listen failed\r\n"); return; }
    struct tcpaccept_out o;
    if (sys_tcpaccept(&o) != 0) { sys_tcpsrv_close(); puts_u("tcpechosrv: accept timed out\r\n"); return; }
    puts_u("tcpechosrv: accepted ");
    print_ip(o.ip);
    puts_u(":");
    puts_u(itoa((long)o.port, buf));
    puts_u("\r\n");
    static unsigned char ebuf[512];
    for (;;) {
        long n = sys_tcpsrv_recv(ebuf, 512);
        if (n <= 0) break;
        if (sys_tcpsrv_send(ebuf, n) < 0) break;
    }
    sys_tcpsrv_close();
    puts_u("tcpechosrv: closed\r\n");
}

/* df (Phase 13 inc.5): MyFS capacity / free-space report.
 * Figures come from SYS_DF (the filesystem's own live
 * bookkeeping) and agree with scripts/fsck_myfs.py on the
 * same image: records walked == live + tombstones. */
static void cmd_bcprobe(const char *arg)
{
    char buf[16];
    long lba = 0;
    if (!arg || !*arg) { puts_u("bcprobe: usage bcprobe <lba>\r\n"); return; }
    while (*arg >= '0' && *arg <= '9') { lba = lba * 10 + (*arg - '0'); arg++; }
    long bits = sys_bcache_probe(lba);
    puts_u("bcprobe: lba ");
    puts_u(itoa(lba, buf));
    puts_u(" valid=");
    puts_u(itoa((bits & 1) ? 1 : 0, buf));
    puts_u(" dirty=");
    puts_u(itoa((bits & 2) ? 1 : 0, buf));
    puts_u(" prefetched=");
    puts_u(itoa((bits & 4) ? 1 : 0, buf));
    puts_u("\r\n");
}
static void cmd_df(const char *arg)
{
    (void)arg;
    struct df_out o;
    char buf[16];
    if (sys_df(&o) != 0) { puts_u("df: not mounted\r\n"); return; }
    puts_u("df: capacity ");
    puts_u(itoa((long)o.cap_sectors, buf));
    puts_u(" sectors, free ");
    puts_u(itoa((long)o.free_sectors, buf));
    puts_u("\r\n");
    puts_u("df: live ");
    puts_u(itoa((long)o.live_records, buf));
    puts_u(" records, ");
    puts_u(itoa((long)o.live_sectors, buf));
    puts_u(" sectors\r\n");
    puts_u("df: tombstones ");
    puts_u(itoa((long)o.dead_records, buf));
    puts_u(" records, ");
    puts_u(itoa((long)o.dead_sectors, buf));
    puts_u(" sectors\r\n");
}

/* httpd <port> <file> (Phase 12 inc.5): serve one connection.
 * Reads the request line (up to the first CRLF, bounded), then
 * replies HTTP/1.0 200 with the named MyFS file as the body,
 * or 404 with a short body when the file does not exist. Files
 * are served up to an 8192-byte cap. */
static int httpd_send(const unsigned char *p, long n)
{
    while (n > 0) {
        long chunk = n > 512 ? 512 : n;
        if (sys_tcpsrv_send(p, chunk) < 0) return -1;
        p += chunk; n -= chunk;
    }
    return 0;
}

static void cmd_httpd(const char *arg)
{
    char buf[16];
    int port = 0;
    int i = 0;
    while (arg[i] >= '0' && arg[i] <= '9') { port = port * 10 + (arg[i] - '0'); i++; }
    while (arg[i] == ' ') i++;
    const char *fname = arg + i;
    if (port == 0 || port > 65535 || !fname[0]) {
        puts_u("usage: httpd <port> <file>\r\n");
        return;
    }
    if (sys_tcplisten(port) != 0) { puts_u("httpd: listen failed\r\n"); return; }
    struct tcpaccept_out o;
    if (sys_tcpaccept(&o) != 0) { sys_tcpsrv_close(); puts_u("httpd: accept timed out\r\n"); return; }
    puts_u("httpd: accepted ");
    print_ip(o.ip);
    puts_u(":");
    puts_u(itoa((long)o.port, buf));
    puts_u("\r\n");
    static unsigned char rbuf[512];
    long rlen = 0;
    while (rlen < 512) {
        long n = sys_tcpsrv_recv(rbuf + rlen, 512 - rlen);
        if (n <= 0) break;
        rlen += n;
        if (rlen >= 2 && rbuf[rlen - 2] == '\r' && rbuf[rlen - 1] == '\n') break;
        if (rlen >= 1 && rbuf[rlen - 1] == '\n') break;
    }
    if (rlen > 0) {
        static unsigned char fbuf[8192];
        static char hdr[96];
        long fd = sys_open(fname);
        if (fd < 0) {
            const char *h = "HTTP/1.0 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found";
            httpd_send((const unsigned char *)h, slen_u(h));
        } else {
            long flen = 0;
            long r;
            while (flen < 8192 && (r = sys_read(fd, fbuf + flen, 8192 - flen)) > 0)
                flen += r;
            sys_close(fd);
            int hp = 0;
            const char *pre = "HTTP/1.0 200 OK\r\nContent-Length: ";
            while (pre[hp]) { hdr[hp] = pre[hp]; hp++; }
            const char *num = itoa(flen, buf);
            int k = 0;
            while (num[k]) hdr[hp++] = num[k++];
            hdr[hp++] = '\r'; hdr[hp++] = '\n'; hdr[hp++] = '\r'; hdr[hp++] = '\n';
            hdr[hp] = 0;
            if (httpd_send((const unsigned char *)hdr, hp) == 0)
                httpd_send(fbuf, flen);
        }
    }
    sys_tcpsrv_close();
    puts_u("httpd: closed\r\n");
}

static void cmd_tcpecho(const char *arg)
{
    char ipbuf[24];
    int i = 0;
    while (arg[i] && arg[i] != ' ' && i < 23) { ipbuf[i] = arg[i]; i++; }
    ipbuf[i] = 0;
    unsigned char ip[4];
    if (!parse_ip(ipbuf, ip) || arg[i] != ' ') {
        puts_u("usage: tcpecho <ip> <port> <text>\r\n");
        return;
    }
    const char *p = arg + i + 1;
    int port = 0;
    while (*p >= '0' && *p <= '9') { port = port * 10 + (*p - '0'); p++; }
    if (*p != ' ' || port == 0 || port > 65535) {
        puts_u("usage: tcpecho <ip> <port> <text>\r\n");
        return;
    }
    p++;
    unsigned int tlen = 0;
    while (p[tlen]) tlen++;
    if (tlen == 0) { puts_u("usage: tcpecho <ip> <port> <text>\r\n"); return; }
    static char out[2048];
    struct tcpecho_args a;
    a.data = p; a.len = tlen; a.cap = 2047; a.out = out;
    long n = sys_tcpecho(ip, (unsigned short)port, &a);
    if (n < 0) { puts_u("tcpecho: failed\r\n"); return; }
    out[n] = 0;
    puts_u(out); puts_u("\r\n");
}
/* httpget <host> <path>: HTTP/1.0 GET over TCP (Phase 9 inc.3).
 * Resolves the host with DNS when it is not a dotted quad, then
 * runs the whole exchange through SYS_TCPECHO. */
static int sput(char *dst, int o, const char *s)
{
    while (*s) dst[o++] = *s++;
    return o;
}

static void cmd_httpget(const char *arg)
{
    char host[128];
    int i = 0;
    while (arg[i] && arg[i] != ' ' && i < 127) { host[i] = arg[i]; i++; }
    host[i] = 0;
    if (arg[i] != ' ') { puts_u("usage: httpget <host> <path>\r\n"); return; }
    const char *path = arg + i + 1;
    if (!*path) { puts_u("usage: httpget <host> <path>\r\n"); return; }
    unsigned char ip[4];
    if (!parse_ip(host, ip)) {
        if (sys_nslookup(host, 0, ip) != 0) {
            puts_u("httpget: cannot resolve host\r\n");
            return;
        }
    }
    static char req[512];
    int o = 0;
    o = sput(req, o, "GET ");
    o = sput(req, o, path);
    o = sput(req, o, " HTTP/1.0\r\nHost: ");
    o = sput(req, o, host);
    o = sput(req, o, "\r\n\r\n");
    req[o] = 0;
    static char out[4096];
    struct tcpecho_args a;
    a.data = req; a.len = (unsigned int)o; a.cap = 4095; a.out = out;
    long n = sys_tcpecho(ip, 80, &a);
    if (n < 0) { puts_u("httpget: failed\r\n"); return; }
    out[n] = 0;
    puts_u(out);
    puts_u("\r\n");
}
/* Print one fixed-point (x1024) weight as sNNN.NNN. */
static void print_fp(int v)
{
    char buf[16];
    if (v < 0) { puts_u("-"); v = -v; }
    puts_u(itoa(v / 1024, buf));
    puts_u(".");
    int frac = (v % 1024) * 1000 / 1024;
    if (frac < 100) puts_u("0");
    if (frac < 10) puts_u("0");
    puts_u(itoa(frac, buf));
}

/* pfweights [zero|seed]: inspect or reset the learned prefetch
 * weights (Phase 10 online learning). */
static const char *pfw_names[13] = {
    "bias", "is_next", "stride_conf", "stride_unconf", "seq_run",
    "dist", "recent_acc", "reuse", "jump", "stream_cont",
    "file_seq", "file_seq_other", "file_stride"
};

static void cmd_pfweights(const char *arg)
{
    if (streq(arg, "zero")) {
        sys_pfweights(1, 0);
        puts_u("pfweights: zeroed\r\n");
        return;
    }
    if (streq(arg, "seed")) {
        sys_pfweights(2, 0);
        puts_u("pfweights: reseeded\r\n");
        return;
    }
    if (streq(arg, "save")) {
        long r = sys_pfweights(3, 0);
        puts_u(r == 0 ? "pfweights: saved\r\n" : "pfweights: save failed\r\n");
        return;
    }
    struct pfweights_out o;
    if (sys_pfweights(0, &o) != 0) { puts_u("pfweights: failed\r\n"); return; }
    for (int i = 0; i < 13; i++) {
        puts_u(pfw_names[i]); puts_u("="); print_fp(o.w[i]);
        puts_u(i % 3 == 2 || i == 12 ? "\r\n" : "  ");
    }
    char buf[16];
    puts_u("updates: +"); puts_u(itoa((long)o.pos, buf));
    puts_u(" -"); puts_u(itoa((long)o.neg, buf));
    puts_u("\r\n");
}
/* rm <file>: delete a disk file. */
static void cmd_rm(const char *arg)
{
    if (!arg[0]) { puts_u("usage: rm <file>\r\n"); return; }
    puts_u(sys_funlink(arg) == 0 ? "rm: deleted\r\n" : "rm: failed\r\n");
}

/* mv <old> <new>: rename a disk file. */
static void cmd_mv(const char *arg)
{
    char oldn[64];
    int i = 0;
    while (arg[i] && arg[i] != ' ' && i < 63) { oldn[i] = arg[i]; i++; }
    oldn[i] = 0;
    while (arg[i] == ' ') i++;
    if (!oldn[0] || !arg[i]) { puts_u("usage: mv <old> <new>\r\n"); return; }
    puts_u(sys_frename(oldn, arg + i) == 0 ? "mv: renamed\r\n" : "mv: failed\r\n");
}
/* ------------------------------------------------------------------
 * Shell parser (Phase 14 inc.4). Tokenises a command line into
 * pipeline stages (an argv array per stage, NULL-terminated)
 * plus an optional redirection target. Separators are spaces
 * and tabs (runs collapse; leading/trailing ignored for the
 * token structure). '|' and '>' are operator tokens even when
 * adjacent to words. Quoting (Phase 16 inc.4): a ' or "
 * region may open anywhere in a word; inside a region,
 * separators do not split and '|' / '>' are word characters.
 * Token slices stay RAW (quote characters included); cooking
 * happens in expand_str, driven by the same qstep state
 * machine, so the two cannot disagree. A line that ends
 * inside a region is the parse error "unterminated quote".
 * There is no escaping: a backslash is ordinary, and '<',
 * ';' and '&' are ordinary characters. The
 * grammar follows POSIX placement: the word after '>' is the
 * target and other words stay arguments of their stage.
 * Arrays are sized to the 255-char line, so no line can
 * overflow them and there is no "too complex" error.
 * ------------------------------------------------------------------ */
#define TOK_WORD 0
#define TOK_PIPE 1
#define TOK_GT   2

#define PARSE_OK          0
#define PARSE_EMPTY       1   /* no tokens: just re-prompt */
#define PARSE_ERROR       2   /* parse_err names it */
#define PARSE_UNSUPPORTED 3   /* valid, but uses | or > */

static char        tokbuf[256];
static int         tok_kind[256];
static int         tok_start[256];
static char       *tok_ptr[256];
static int         ntok_g;
static char       *stg_argv[384];
static int         stg_argc[128];
static int         nstages_g;
static char       *redirect_g;
static const char *parse_err;
static int         word_toks[256];  /* token index of each stage word */
static int         nword_toks;
static int         gt_tok;          /* token index of '>', or -1 */
static int         gt_stage;        /* stage index holding '>' */
static int         prompt_nl;
static int         stdin_pipe;      /* fd 0 wired to a pipe for this dispatch (inc.7) */
static long        cat_stdin_err;
static int         run_depth;       /* active run frames (inc.8) */   /* negative stdin read recorded by bare cat (inc.7) */       /* set by dispatch_command (inc.5) */
static long        redir_save;      /* saved console fd slot, or -1 */

/* ---- shell variables (Phase 16 inc.1) ----
 * A fixed-capacity store of name/value pairs in insertion
 * order. This increment is the store and its mutation surface
 * only: NO expansion anywhere yet ($NAME stays a literal word
 * until inc.2). Limits are hard: a violation prints a named
 * error and changes nothing (a failed reassignment leaves the
 * old value intact) — never truncation, never overflow. */
#define VAR_MAX       32   /* variables in the store */
#define VAR_NAME_MAX  31   /* chars, excluding the NUL */
#define VAR_VALUE_MAX 127  /* chars, excluding the NUL */
static struct { char name[VAR_NAME_MAX + 1];
                char value[VAR_VALUE_MAX + 1]; } vars[VAR_MAX];
static int nvars;

/* NAME = [A-Za-z_][A-Za-z0-9_]* over s[0..len). Length-based:
 * the name part of an assignment word is not NUL-terminated.
 * Shared by the assignment recogniser and unset. */
static int var_name_ok(const char *s, long len)
{
    if (len < 1) return 0;
    for (long i = 0; i < len; i++) {
        char c = s[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
              || c == '_' || (i > 0 && c >= '0' && c <= '9');
        if (!ok) return 0;
    }
    return 1;
}

static int var_find(const char *name, long len)
{
    for (int i = 0; i < nvars; i++) {
        long j = 0;
        while (j < len && vars[i].name[j] && vars[i].name[j] == name[j])
            j++;
        if (j == len && vars[i].name[j] == '\0') return i;
    }
    return -1;
}

/* Status of the last executed line (0 or 1), read by $?
 * expansion. Written ONLY by the shell's own layers (parser,
 * assignment, expander, executor, dispatcher), never from
 * inside a builtin: see the contract on dispatch_command. */
static int last_status;

/* One quote state machine for the whole shell (Phase 16
 * inc.4): the parser's scanner uses it to find word
 * boundaries and expand_str uses it to cook words, so the
 * two can never disagree about what is quoted. *qs is 0
 * outside quotes, or the opening quote character inside a
 * region. Returns 1 when c is a quote delimiter in the
 * current state (an opener outside quotes, the matching
 * closer inside) and updates *qs; any other character
 * returns 0. Inside single quotes only ' delimits; inside
 * double quotes only " does. */
static int qstep(int *qs, char c)
{
    if (*qs) {
        if (c == *qs) { *qs = 0; return 1; }
        return 0;
    }
    if (c == '\'' || c == '"') { *qs = c; return 1; }
    return 0;
}

/* Expand and cook a raw string from a parsed line into out
 * (cap bytes including the NUL), in ONE pass over the
 * input: $NAME / ${NAME} / $? substitute from the store
 * (undefined names expand to nothing) and quote regions
 * are honoured -- inside single quotes everything is
 * literal, inside double quotes substitution still
 * applies, and quote delimiters are dropped. Substituted
 * text is written straight to out and never re-scanned,
 * so a stored value containing '$', '|' or '>' is inert
 * data, and quoting on the line never carries into a
 * value. '$$' passes through as a unit (a special for a
 * later step, like '$1', which stays literal because the
 * character after the '$' cannot start a name). A '$'
 * before any other character, or at the end, is a literal
 * '$'. A backslash is an ordinary character throughout.
 * The ${...} scan never consults quote state. Returns 0,
 * -1 (bad substitution: '${' with no '}' or an invalid
 * name inside braces; also an unbalanced quote tail,
 * which a parsed line cannot produce), or -2 (the result
 * would exceed cap). The scan continues after an overflow,
 * so a bad substitution later in the string still wins:
 * badness is a property of the input text alone.
 * mask, when non-NULL, receives one byte per output
 * character (Phase 16 inc.5): 1 when the character is
 * glob-eligible -- copied from unquoted input, or produced
 * by a substitution whose reference appeared in unquoted
 * text -- and 0 when it came from quoted text or a quoted
 * expansion. Callers that never glob pass NULL. */
static int expand_str(const char *in, char *out, long cap,
                      unsigned char *mask)
{
    long o = 0;
    int bad = 0, toolong = 0;
    int qs = 0;
    for (long i = 0; in[i]; ) {
        char c = in[i];
        if (c != '$' || qs == '\'') {
            if (qstep(&qs, c)) { i++; continue; }
            if (o < cap - 1) {
                out[o] = c;
                if (mask) mask[o] = (unsigned char)(qs == 0);
                o++;
            } else toolong = 1;
            i++;
            continue;
        }
        char n = in[i + 1];
        if (n == '$') {
            if (o < cap - 2) {
                int e = (qs == 0);
                out[o] = '$'; if (mask) mask[o] = (unsigned char)e; o++;
                out[o] = '$'; if (mask) mask[o] = (unsigned char)e; o++;
            }
            else toolong = 1;
            i += 2;
            continue;
        }
        if (n == '?') {
            if (o < cap - 1) {
                out[o] = last_status ? '1' : '0';
                if (mask) mask[o] = (unsigned char)(qs == 0);
                o++;
            }
            else toolong = 1;
            i += 2;
            continue;
        }
        if (n == '{' || (n >= 'A' && n <= 'Z') || (n >= 'a' && n <= 'z')
            || n == '_') {
            const char *ns;
            long nl, adv;
            if (n == '{') {
                long j = i + 2;
                while (in[j] && in[j] != '}') j++;
                if (!in[j] || !var_name_ok(in + i + 2, j - (i + 2))) {
                    bad = 1;
                    i = in[j] ? j + 1 : j;
                    continue;
                }
                ns = in + i + 2; nl = j - (i + 2); adv = j + 1 - i;
            } else {
                long j = i + 1;
                while ((in[j] >= 'A' && in[j] <= 'Z')
                       || (in[j] >= 'a' && in[j] <= 'z')
                       || (in[j] >= '0' && in[j] <= '9') || in[j] == '_')
                    j++;
                ns = in + i + 1; nl = j - (i + 1); adv = j - i;
            }
            int at = var_find(ns, nl);
            if (at >= 0) {
                const char *v = vars[at].value;
                int ve = (qs == 0);
                for (long k = 0; v[k]; k++) {
                    if (o < cap - 1) {
                        out[o] = v[k];
                        if (mask) mask[o] = (unsigned char)ve;
                        o++;
                    }
                    else toolong = 1;
                }
            }
            i += adv;
            continue;
        }
        if (o < cap - 1) {
            out[o] = '$';
            if (mask) mask[o] = (unsigned char)(qs == 0);
            o++;
        } else toolong = 1;
        i++;
    }
    out[o] = '\0';
    if (qs) bad = 1;
    if (bad) return -1;
    if (toolong) return -2;
    return 0;
}

/* A raw string needs the expansion path when it contains a
 * '$' or a quote character; anything else takes the legacy
 * capped copy byte-for-byte, exactly as before quoting. */
static int str_needs_expand(const char *s)
{
    for (long i = 0; s[i]; i++)
        if (s[i] == '$' || s[i] == '\'' || s[i] == '"') return 1;
    return 0;
}

/* Try to perform the assignment in the single word w (already
 * known to be a whole line's only word). Returns 1 if w was an
 * assignment line (performed, or refused with a named error),
 * 0 if w is not assignment-shaped at all and the line must be
 * dispatched as usual. Check order: name length, value length,
 * then capacity (new names only) — all before any mutation. */
static int var_try_assign(const char *w)
{
    long eq = -1;
    for (long i = 0; w[i]; i++)
        if (w[i] == '=') { eq = i; break; }
    if (eq < 0 || !var_name_ok(w, eq)) return 0;
    const char *val = w + eq + 1;
    if (eq > VAR_NAME_MAX) {
        puts_u("shell: variable name too long\r\n");
        return 2;
    }
    /* The NAME portion never expands. The value expands
     * against the CURRENT store before storing (inc.2), so
     * X=$Y copies Y's present value. A '$'-free value takes
     * the inc.1 raw length check; a '$' value is expanded
     * first (its raw length says nothing about the result),
     * and the expanded text must fit the same value cap —
     * an overflow names that cap, a bad substitution names
     * itself, and either failure stores nothing. */
    char ebuf[VAR_VALUE_MAX + 1];
    const char *store_val = val;
    if (str_needs_expand(val)) {
        int st = expand_str(val, ebuf, VAR_VALUE_MAX + 1, 0);
        if (st == -1) {
            puts_u("shell: bad substitution\r\n");
            return 2;
        }
        if (st == -2) {
            puts_u("shell: variable value too long\r\n");
            return 2;
        }
        store_val = ebuf;
    } else {
        long vlen = 0;
        while (val[vlen]) vlen++;
        if (vlen > VAR_VALUE_MAX) {
            puts_u("shell: variable value too long\r\n");
            return 2;
        }
    }
    int at = var_find(w, eq);
    if (at < 0) {
        if (nvars >= VAR_MAX) {
            puts_u("shell: too many variables\r\n");
            return 2;
        }
        at = nvars++;
        for (long i = 0; i < eq; i++) vars[at].name[i] = w[i];
        vars[at].name[eq] = '\0';
    }
    long si = 0;
    while (store_val[si]) { vars[at].value[si] = store_val[si]; si++; }
    vars[at].value[si] = '\0';
    return 1;
}

static int shell_parse(const char *line)
{
    /* Tokenise a scratch copy; the original line is kept for
     * the raw-remainder arg and the echo fallback. A word's
     * terminating delimiter is read BEFORE it is overwritten
     * with a NUL, so an adjacent operator is never destroyed. */
    int li = 0;
    while (line[li] && li < 255) { tokbuf[li] = line[li]; li++; }
    tokbuf[li] = '\0';

    ntok_g = 0;
    int i = 0;
    while (tokbuf[i]) {
        char c = tokbuf[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '|' || c == '>') {
            tok_kind[ntok_g] = (c == '|') ? TOK_PIPE : TOK_GT;
            tok_ptr[ntok_g] = (char *)((c == '|') ? "|" : ">");
            tok_start[ntok_g] = i;
            ntok_g++; i++; continue;
        }
        int st = i;
        int qs = 0;
        while (tokbuf[i]) {
            char c = tokbuf[i];
            if (!qs && (c == ' ' || c == '\t' || c == '|' || c == '>'))
                break;
            qstep(&qs, c);
            i++;
        }
        if (qs) {
            parse_err = "unterminated quote";
            return PARSE_ERROR;
        }
        char d = tokbuf[i];
        tokbuf[i] = '\0';
        tok_kind[ntok_g] = TOK_WORD;
        tok_ptr[ntok_g] = tokbuf + st;
        tok_start[ntok_g] = st;
        ntok_g++;
        if (!d) break;
        if (d == '|' || d == '>') {
            tok_kind[ntok_g] = (d == '|') ? TOK_PIPE : TOK_GT;
            tok_ptr[ntok_g] = (char *)((d == '|') ? "|" : ">");
            tok_start[ntok_g] = i;
            ntok_g++;
        }
        i++;
    }
    if (ntok_g == 0) return PARSE_EMPTY;

    /* Grammar scan, left to right; the first error wins. */
    int na = 0, nst = 0, argc = 0;
    redirect_g = 0; parse_err = 0;
    nword_toks = 0; gt_tok = -1;
    for (int k = 0; k < ntok_g; k++) {
        if (tok_kind[k] == TOK_PIPE) {
            if (argc == 0) {
                parse_err = "empty command in pipeline";
                return PARSE_ERROR;
            }
            stg_argv[na++] = 0; stg_argc[nst++] = argc; argc = 0;
        } else if (tok_kind[k] == TOK_GT) {
            gt_tok = k; gt_stage = nst;
            if (redirect_g) {
                parse_err = "multiple redirections";
                return PARSE_ERROR;
            }
            if (k + 1 < ntok_g && tok_kind[k+1] == TOK_GT &&
                tok_start[k+1] == tok_start[k] + 1) {
                parse_err = "'>>' (append) not supported";
                return PARSE_ERROR;
            }
            if (k + 1 >= ntok_g || tok_kind[k+1] != TOK_WORD) {
                parse_err = "missing redirect target";
                return PARSE_ERROR;
            }
            redirect_g = tok_ptr[k+1];
            k++;
        } else {
            stg_argv[na++] = tok_ptr[k]; argc++;
            word_toks[nword_toks++] = k;
        }
    }
    if (argc == 0) {
        parse_err = nst ? "empty command in pipeline" : "missing command";
        return PARSE_ERROR;
    }
    stg_argv[na++] = 0; stg_argc[nst++] = argc;
    nstages_g = nst;
    return PARSE_OK;
}

/* Build arg for one stage of a redirected or pipelined line
 * (Phase 14 inc.5, generalised in inc.6). The plain-line feed
 * (raw remainder from the second token) would include the
 * "> target" text and other stages, so a stage arg is the
 * raw line span from the stage's first argument word to its
 * last (word = a word token of the stage; the command word
 * and the redirect target are excluded). w0 is the stage's
 * first word index in word_toks, wargc its word count. If
 * '>' sits between argument words (POSIX placement), the
 * span is two raw segments joined with one space, the first
 * segment's trailing separators stripped. Inner spacing is
 * raw, so write's free text keeps its double spaces. A
 * word's raw end is exactly start + its raw length, quote
 * characters included (Phase 16 inc.4): every token is
 * quote-balanced and the join point sits between tokens,
 * so no quoted region can span it; cooking the joined
 * text is expand_str's job, downstream. */
static void build_stage_arg(int w0, int wargc, const char *line,
                            char *arg, int cap)
{
    int ai = 0;
    if (wargc >= 2) {
        int first = word_toks[w0 + 1];
        int last = word_toks[w0 + wargc - 1];
        int last_end = tok_start[last] + (int)slen_u(tok_ptr[last]);
        int split = -1;
        if (gt_tok > first && gt_tok < last) {
            for (int wi = w0 + 2; wi < w0 + wargc; wi++) {
                if (word_toks[wi] > gt_tok) { split = word_toks[wi]; break; }
            }
        }
        if (split < 0) {
            for (int q = tok_start[first]; q < last_end && ai < cap; q++)
                arg[ai++] = line[q];
        } else {
            int e1 = tok_start[gt_tok];
            while (e1 > tok_start[first] &&
                   (line[e1-1] == ' ' || line[e1-1] == '\t')) e1--;
            for (int q = tok_start[first]; q < e1 && ai < cap; q++)
                arg[ai++] = line[q];
            if (ai && ai < cap) arg[ai++] = ' ';
            for (int q = tok_start[split]; q < last_end && ai < cap; q++)
                arg[ai++] = line[q];
        }
    }
    arg[ai] = '\0';
}

/* Restore fd 1 from redir_save, if a redirected dispatch is
 * somehow still active at the top of the main loop. */
static void redir_restore(void)
{
    if (redir_save >= 0) {
        sys_dup2(redir_save, 1);
        sys_close(redir_save);
        redir_save = -1;
    }
}

static void dispatch_command(const char *cmd, const char *arg,
                             const char *line, long n);
/* Execute a pipeline (Phase 14 inc.6): N >= 2 stages run
 * sequentially, stage s's stdout wired to a fresh pipe whose
 * read end becomes stage s+1's stdin; the last stage writes
 * to the console, or to the line's redirect target (which
 * the parser must place in the last stage). Interleaving
 * the dispatches is impossible — dispatch is a synchronous
 * call — so the pipe bound applies: only the first 1024
 * bytes of an intermediate stage's output are retained, and
 * a producer writing more drops the excess (its writes
 * return short counts / -2, and no builtin retries writes),
 * so no stage can hang. EOF arms automatically: a pipe's
 * write end lives only as fd 1's description once its slot
 * is closed, and wiring the next stage displaces fd 1,
 * dropping the last writer reference before the consumer
 * dispatches. No builtin reads fd 0 today, so intermediate
 * output is discarded when the ends close; the wiring is
 * what the test observes (console exactly-once, restore).
 * fd 0 and fd 1 are saved before stage 0 and restored after
 * the last stage, on the failure path too. */
static int is_session_cmd(const char *nm)
{
    return streq(nm, "exec") || streq(nm, "exit") ||
           streq(nm, "pf");
}

static int stage_needs_expand(int stage)
{
    int aoff = 0;
    for (int s = 0; s < stage; s++) aoff += stg_argc[s] + 1;
    for (int k = 0; k < stg_argc[stage]; k++)
        if (str_needs_expand(stg_argv[aoff + k])) return 1;
    return 0;
}

/* ------------------------------------------------------------------
 * Globbing (Phase 16 inc.5), the third step of the fixed
 * order parse -> expand -> glob -> dispatch. The matcher
 * is glob_match() in glob.h (shared with the host test).
 * Eligibility is per-character, from expand_str's mask: a
 * '*', '?' or '[' globs only where its mask byte is 1
 * (unquoted original text, or an unquoted expansion's
 * value). Globbing is per WORD, before the flat arg string
 * is finalised, against the same listing ls uses
 * (sys_readdir over the flat MyFS directory). Match order
 * is pinned SORTED (byte order), not listing order.
 *
 * Buffers are file-scope statics under the same discipline
 * as exp_cmd/exp_arg: every glob result is fully consumed
 * (copied into cmd/arg or the target) before dispatch, so
 * a nested execute_line (via run) can never observe a
 * half-used store. glob_collect clobbers glob_store on
 * every call BY DESIGN: callers consume a list completely
 * before collecting again (the command word's extra
 * matches are copied OUT to glob_extras before any
 * argument word is collected).
 * ------------------------------------------------------------------ */
#define GLOB_MAX 64   /* == DISKFS_MAX_FILES: the whole directory */
static char glob_store[GLOB_MAX][64];
static char glob_extras[GLOB_MAX - 1][64];
static int glob_extras_n;
static char gwbuf[256];
static unsigned char gwmask[256];
static char grbuf[256];

static int gstrcmp(const char *a, const char *b)
{
    long i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return (unsigned char)a[i] - (unsigned char)b[i];
}

static void gcopy(char *dst, const char *srcw)
{
    int i = 0;
    while (srcw[i]) { dst[i] = srcw[i]; i++; }
    dst[i] = '\0';
}

/* Collect the sorted matches for a cooked word + mask into
 * glob_store. Returns the count, or -1 if the matches
 * exceed the store (impossible against a 64-file
 * directory; the caller turns it into the too-long error
 * rather than dropping a match silently). */
static int glob_collect(const char *pat, const unsigned char *mask)
{
    int n = 0;
    char name[64];
    long idx = 0;
    while (sys_readdir(idx, name) == 0) {
        idx++;
        if (!glob_match(pat, mask, name)) continue;
        if (n >= GLOB_MAX) return -1;
        gcopy(glob_store[n], name);
        n++;
    }
    for (int i = 1; i < n; i++) {
        char tmp[64];
        gcopy(tmp, glob_store[i]);
        int j = i - 1;
        while (j >= 0 && gstrcmp(glob_store[j], tmp) > 0) {
            gcopy(glob_store[j + 1], glob_store[j]);
            j--;
        }
        gcopy(glob_store[j + 1], tmp);
    }
    return n;
}

static int glob_candidate(const char *w, const unsigned char *mask)
{
    for (long i = 0; w[i]; i++)
        if (mask[i] && (w[i] == '*' || w[i] == '?' || w[i] == '['))
            return 1;
    return 0;
}

/* Glob one stage's command word (w = its raw word). On one
 * or more matches, cmd becomes the first and the rest are
 * copied to glob_extras for glob_arg_part. Returns -1 on
 * store overflow, else 0. A cooking failure cannot happen
 * (the word already expanded once); it is treated as
 * no-glob defensively. */
static int glob_cmd_part(const char *w, char *cmd)
{
    glob_extras_n = 0;
    if (expand_str(w, gwbuf, 256, gwmask) != 0) return 0;
    if (!glob_candidate(gwbuf, gwmask)) return 0;
    int n = glob_collect(gwbuf, gwmask);
    if (n < 0) return -1;
    if (n == 0) return 0;
    gcopy(cmd, glob_store[0]);
    for (int i = 1; i < n; i++)
        gcopy(glob_extras[glob_extras_n++], glob_store[i]);
    return 0;
}

static void gr_append(const char *s, int *first, int *overflow)
{
    long len = slen_u(grbuf);
    if (!*first) {
        if (len < 191) grbuf[len++] = ' ';
        else *overflow = 1;
        grbuf[len] = '\0';
    }
    *first = 0;
    for (long i = 0; s[i]; i++) {
        if (len < 191) grbuf[len++] = s[i];
        else *overflow = 1;
    }
    grbuf[len] = '\0';
}

/* Glob one stage's argument words into arg (the phase-one
 * string, cap 192). The command extras (glob_cmd_part)
 * prepend. If nothing matched, or the rebuilt arg equals
 * the phase-one arg (a no-op glob, e.g. a pattern whose
 * only match is its own literal name), arg is left
 * untouched byte-for-byte. Returns -1 when something
 * matched but the rebuilt arg would exceed the cap: the
 * caller raises "shell: expansion too long" and dispatches
 * nothing (never a silently truncated match list). */
static int glob_arg_part(int stage, char *arg)
{
    int aoff = 0;
    for (int s = 0; s < stage; s++) aoff += stg_argc[s] + 1;
    int any = glob_extras_n > 0;
    int overflow = 0, first = 1;
    grbuf[0] = '\0';
    for (int i = 0; i < glob_extras_n; i++)
        gr_append(glob_extras[i], &first, &overflow);
    for (int k = 1; k < stg_argc[stage]; k++) {
        const char *w = stg_argv[aoff + k];
        if (expand_str(w, gwbuf, 256, gwmask) != 0) {
            gr_append(w, &first, &overflow);
            continue;
        }
        if (!glob_candidate(gwbuf, gwmask)) {
            gr_append(gwbuf, &first, &overflow);
            continue;
        }
        int n = glob_collect(gwbuf, gwmask);
        if (n < 0) return -1;
        if (n == 0) {
            gr_append(gwbuf, &first, &overflow);
            continue;
        }
        any = 1;
        for (int i = 0; i < n; i++)
            gr_append(glob_store[i], &first, &overflow);
    }
    if (!any) return 0;
    if (overflow) return -1;
    if (streq(grbuf, arg)) return 0;
    gcopy(arg, grbuf);
    return 0;
}

static void expand_fail(int st)
{
    if (st == -1) puts_u("shell: bad substitution\r\n");
    else puts_u("shell: expansion too long\r\n");
}

/* Expanded per-stage cmd/arg, filled by run_pipeline's
 * expansion pass before anything is dispatched and consumed
 * by its main loop, so a stage that mutates the store (unset)
 * cannot change a later stage's expansion — the pipeline is
 * expanded as a whole, the way subshells would see it.
 * run_pipeline cannot re-enter (a stage can never be `run`),
 * so statics are safe here. */
static char exp_cmd[128][64];
static char exp_arg[128][192];

static int run_pipeline(const char *line)
{
    /* Pre-checks: nothing has run yet. */
    int aoff = 0;
    for (int s = 0; s < nstages_g; s++) {
        const char *nm = stg_argv[aoff];
        if (is_session_cmd(nm) || streq(nm, "run")) {
            puts_u("shell: "); puts_u(nm);
            puts_u(" not supported in pipelines\r\n");
            last_status = 1;
            return 0;
        }
        aoff += stg_argc[s] + 1;
    }
    if (redirect_g && gt_stage != nstages_g - 1) {
        puts_u("shell: redirection in a pipeline must be on the last stage\r\n");
        last_status = 1;
        return 0;
    }
    /* Expansion pass (Phase 16 inc.2): expand every stage's
     * cmd/arg ONCE, here — before the target is created and
     * before anything is dispatched — and refuse session
     * commands by their EXPANDED names (the pre-check above
     * sees raw words; a stage $X with X=exit must not smuggle
     * exit past it). On any error: nothing dispatched, no
     * target created. */
    {
        int aoff = 0, woff = 0, xerr = 0;
        for (int s = 0; s < nstages_g; s++) {
            const char *w = stg_argv[aoff];
            if (str_needs_expand(w)) {
                int st = expand_str(w, exp_cmd[s], 64, 0);
                if (st && (!xerr || st == -1)) xerr = st;
            } else {
                int ci = 0;
                while (w[ci] && ci < 63) { exp_cmd[s][ci] = w[ci]; ci++; }
                exp_cmd[s][ci] = '\0';
            }
            if (stage_needs_expand(s)) {
                char raw[256];
                build_stage_arg(woff, stg_argc[s], line, raw, 255);
                int st = expand_str(raw, exp_arg[s], 192, 0);
                if (st && (!xerr || st == -1)) xerr = st;
            } else {
                build_stage_arg(woff, stg_argc[s], line, exp_arg[s], 191);
            }
            /* Glob this stage (inc.5) before the refusal
             * checks below read its name: cmd first match,
             * extras prepend its arg words. */
            if (glob_cmd_part(w, exp_cmd[s]) < 0 ||
                glob_arg_part(s, exp_arg[s]) < 0) {
                if (!xerr) xerr = -2;
            }
            aoff += stg_argc[s] + 1;
            woff += stg_argc[s];
        }
        if (xerr) { expand_fail(xerr); last_status = 1; return 0; }
        for (int s = 0; s < nstages_g; s++) {
            if (!exp_cmd[s][0]) {
                puts_u("shell: empty command\r\n");
                last_status = 1;
                return 0;
            }
            if (is_session_cmd(exp_cmd[s]) || streq(exp_cmd[s], "run")) {
                puts_u("shell: "); puts_u(exp_cmd[s]);
                puts_u(" not supported in pipelines\r\n");
                last_status = 1;
                return 0;
            }
        }
    }
    long tfd = -1;
    if (redirect_g) {
        tfd = sys_fcreate(redirect_g);
        if (tfd < 0) {
            puts_u("shell: cannot create "); puts_u(redirect_g);
            puts_u("\r\n"); last_status = 1; return 0;
        }
    }
    long sv0 = -1, sv1 = -1;
    for (long k = 15; k >= 3; k--)
        if (sys_dup2(k, k) < 0) { sv0 = k; break; }
    if (sv0 >= 0 && sys_dup2(0, sv0) < 0) sv0 = -1;
    if (sv0 >= 0) {
        for (long k = 15; k >= 3; k--)
            if (sys_dup2(k, k) < 0) { sv1 = k; break; }
        if (sv1 >= 0 && sys_dup2(1, sv1) < 0) sv1 = -1;
    }
    if (sv0 < 0 || sv1 < 0) {
        if (sv0 >= 0) { sys_dup2(sv0, 0); sys_close(sv0); }
        if (tfd >= 0) sys_close(tfd);
        puts_u("shell: pipeline failed\r\n");
        last_status = 1;
        return 0;
    }

    long prev_read = -1;
    int failed = 0;
    aoff = 0;
    int woff = 0;
    /* Dispatch-entry reset (see the contract on
     * dispatch_command): a stage fallback may raise this to
     * 1; it is never lowered again inside the pipeline. */
    last_status = 0;
    for (int s = 0; s < nstages_g; s++) {
        if (s > 0) {
            sys_dup2(prev_read, 0);
            sys_close(prev_read);
            prev_read = -1;
        }
        if (s < nstages_g - 1) {
            long pfds[2];
            if (sys_pipe(pfds) < 0) { failed = 1; break; }
            sys_dup2(pfds[1], 1);
            sys_close(pfds[1]);
            prev_read = pfds[0];
        } else if (tfd >= 0) {
            sys_dup2(tfd, 1);
            sys_close(tfd);
            tfd = -1;
        } else {
            sys_dup2(sv1, 1);
        }
        char *scmd = exp_cmd[s];
        char *sarg = exp_arg[s];
        char rline[256];
        int ri = 0;
        for (int j = 0; scmd[j] && ri < 250; j++) rline[ri++] = scmd[j];
        if (sarg[0]) {
            rline[ri++] = ' ';
            for (int j = 0; sarg[j] && ri < 250; j++) rline[ri++] = sarg[j];
        }
        rline[ri] = '\0';
        stdin_pipe = (s > 0);
        dispatch_command(scmd, sarg, rline, ri);
        aoff += stg_argc[s] + 1;
        woff += stg_argc[s];
    }
    if (prev_read >= 0) sys_close(prev_read);
    if (tfd >= 0) sys_close(tfd);
    sys_dup2(sv0, 0); sys_close(sv0);
    sys_dup2(sv1, 1); sys_close(sv1);
    stdin_pipe = 0;
    if (cat_stdin_err) {
        char nb[24];
        puts_u("shell: cat: stdin read error ");
        puts_u(itoa(cat_stdin_err, nb));
        puts_u("\r\n");
        cat_stdin_err = 0;
        last_status = 1;
        if (failed) { puts_u("shell: pipeline failed\r\n"); last_status = 1; }
        return 0;
    }
    if (failed) { puts_u("shell: pipeline failed\r\n"); last_status = 1; return 0; }
    return redirect_g ? -1 : prompt_nl;
}


/* Execute one line (Phase 14 inc.8: the main loop's body,
 * extracted behaviour-identically so `run` can drive it).
 * Pipelines, redirection and plain dispatch, plus the
 * script refusals. Returns the prompt flavour for the
 * interactive caller: 1 = "\r\n> ", 0 = "> ", -1 = the
 * line produced no console output (the caller still prints
 * "> "; cmd_run's flavour accounting ignores -1). Nothing
 * in here prints a prompt. */
static int execute_line(char *line, long n)
{
    redir_restore();

    /* Parse the line (Phase 14 inc.4). A plain command
     * feeds the dispatch chain below with cmd/arg as the
     * old first-space split produced them, with ONE
     * change: the separator run after the command word
     * no longer leaks into arg. arg is the raw remainder
     * of the line starting at the second token (inner and
     * trailing bytes byte-identical to before), and cmd
     * is the first token; both keep their old copy caps. */
    int pr = shell_parse(line);
    if (pr == PARSE_EMPTY) return -1;
    if (pr == PARSE_ERROR) {
        puts_u("shell: parse error: "); puts_u(parse_err);
        puts_u("\r\n");
        last_status = 1;
        return 0;
    }

    /* Assignment lines (Phase 16 inc.1): a line that is a
     * single NAME=value word — one stage, no redirection, one
     * word — sets the variable and prints nothing. Recognised
     * here, post-parse and before dispatch, so the guards are
     * structural: X=1 | cat is a pipeline (nstages > 1) and
     * X=1 > f is a redirect line, and neither assigns; the
     * redirect branch and run_pipeline call dispatch_command
     * directly and never re-enter this function, so a stage or
     * rebuilt line can never become an assignment either.
     * Placed before the script session-command check: exit=1
     * is a variable, not the exit command (that check compares
     * whole names, and this word is not one). */
    if (nstages_g == 1 && !redirect_g && stg_argc[0] == 1) {
        int ar = var_try_assign(stg_argv[0]);
        if (ar) {
            /* An assignment line's status is the assignment's
             * own outcome (tri-state: 1 stored, 2 refused). */
            last_status = (ar == 2) ? 1 : 0;
            return -1;
        }
    }

    /* Expansion (Phase 16 inc.2): the redirect target and the
     * command word expand here, before anything is created or
     * dispatched. A '$'-free string takes the legacy capped
     * copy byte-for-byte; a '$' string is expanded from its
     * UNCAPPED raw source, so a cap can never cut a reference
     * in half and silently change the line's meaning —
     * overflow of the destination cap is a named error. arg
     * is NOT touched yet: on a pipeline line the raw remainder
     * spans every stage, and expanding it here would report
     * later stages' errors in the wrong place (and false-fire
     * on length); run_pipeline expands per stage itself. */
    char cmd[64], arg[192];
    char tbuf[256];
    unsigned char tmask[256];
    int cmd_exp = 0;
    {
        int xerr = 0;
        if (redirect_g) {
            /* Cook the target into tbuf/tmask: through
             * expand_str when it needs expansion, else the
             * raw word with an all-eligible mask -- globbing
             * (inc.5) applies either way. */
            int tst = 0;
            if (str_needs_expand(redirect_g)) {
                tst = expand_str(redirect_g, tbuf, 256, tmask);
                if (tst) xerr = tst;
                else if (!tbuf[0]) {
                    puts_u("shell: empty redirect target\r\n");
                    last_status = 1;
                    return 0;
                }
            } else {
                int ti = 0;
                while (redirect_g[ti] && ti < 255) {
                    tbuf[ti] = redirect_g[ti];
                    tmask[ti] = 1;
                    ti++;
                }
                tbuf[ti] = '\0';
            }
            if (!tst) {
                /* Glob the target: exactly one match replaces
                 * it; zero leave it literal; more than one is
                 * an ambiguous redirect -- no file created,
                 * nothing dispatched. */
                if (glob_candidate(tbuf, tmask)) {
                    int gn = glob_collect(tbuf, tmask);
                    if (gn < 0) xerr = -2;
                    else if (gn > 1) {
                        puts_u("shell: ambiguous redirect\r\n");
                        last_status = 1;
                        return 0;
                    }
                    else if (gn == 1) gcopy(tbuf, glob_store[0]);
                }
                if (!xerr) redirect_g = tbuf;
            }
        }
        const char *w = stg_argv[0];
        if (str_needs_expand(w)) {
            cmd_exp = 1;
            int st = expand_str(w, cmd, 64, 0);
            if (st && (!xerr || st == -1)) xerr = st;
        } else {
            int ci = 0;
            while (w[ci] && ci < 63) { cmd[ci] = w[ci]; ci++; }
            cmd[ci] = '\0';
        }
        if (xerr) { expand_fail(xerr); last_status = 1; return 0; }
    }
    /* Glob the command word (inc.5), single-stage lines
     * only -- pipelines glob per stage in run_pipeline. The
     * session-command check below and dispatch both read
     * the globbed name; extra matches wait in glob_extras
     * for the arg pass. */
    if (nstages_g == 1 && glob_cmd_part(stg_argv[0], cmd) < 0) {
        expand_fail(-2);
        last_status = 1;
        return 0;
    }
    if (cmd_exp && !cmd[0]) {
        puts_u("shell: empty command\r\n");
        last_status = 1;
        return 0;
    }

    /* Inside a script, the session commands are refused:
     * a script must never end the shell session. Placed
     * before the pipeline and redirect branches, so an
     * `exit > log` line creates nothing. (Pipelines refuse
     * them in run_pipeline's own pre-check.) */
    if (run_depth > 0 && nstages_g == 1 && is_session_cmd(cmd)) {
        puts_u("run: "); puts_u(cmd);
        puts_u(" not allowed in a script\r\n");
        last_status = 1;
        return 0;
    }

    if (nstages_g > 1) return run_pipeline(line);

    /* Single-stage arg, built exactly as before — except that
     * a '$' anywhere in the stage's words makes the raw text
     * be built uncapped (255) and expanded into arg, for the
     * same never-cut-a-reference reason as cmd. */
    if (redirect_g) {
        if (stage_needs_expand(0)) {
            char raw[256];
            build_stage_arg(0, stg_argc[0], line, raw, 255);
            int st = expand_str(raw, arg, 192, 0);
            if (st) { expand_fail(st); last_status = 1; return 0; }
        } else {
            build_stage_arg(0, stg_argc[0], line, arg, 191);
        }
    } else {
        const char *a = (ntok_g > 1) ? line + tok_start[1]
                                     : line + n;
        if (str_needs_expand(a)) {
            int st = expand_str(a, arg, 192, 0);
            if (st) { expand_fail(st); last_status = 1; return 0; }
        } else {
            int ai = 0;
            while (a[ai] && ai < 191) { arg[ai] = a[ai]; ai++; }
            arg[ai] = '\0';
        }
    }
    /* Glob the argument words (inc.5): per word, after
     * expansion, before dispatch -- and before the redirect
     * branch creates the target file. */
    if (glob_arg_part(0, arg) < 0) {
        expand_fail(-2);
        last_status = 1;
        return 0;
    }
    if (redirect_g) {
        if (streq(cmd, "exec")) {
            puts_u("shell: redirection not supported for exec\r\n");
            last_status = 1;
            return 0;
        }
        if (streq(cmd, "run")) {
            puts_u("shell: redirection not supported for run\r\n");
            last_status = 1;
            return 0;
        }
        long tfd = sys_fcreate(redirect_g);
        if (tfd < 0) {
            puts_u("shell: cannot create "); puts_u(redirect_g);
            puts_u("\r\n"); last_status = 1; return 0;
        }
        long sv = -1;
        for (long k = 15; k >= 3; k--)
            if (sys_dup2(k, k) < 0) { sv = k; break; }
        if (sv < 0 || sys_dup2(1, sv) < 0) {
            sys_close(tfd);
            puts_u("shell: redirection failed\r\n"); last_status = 1; return 0;
        }
        redir_save = sv;
        sys_dup2(tfd, 1);
        sys_close(tfd);
        /* The echo fallback must not see "> target":
         * dispatch gets a rebuilt line (cmd + arg). */
        char rline[256];
        int ri = 0;
        for (int j = 0; cmd[j] && ri < 250; j++) rline[ri++] = cmd[j];
        if (arg[0]) {
            rline[ri++] = ' ';
            for (int j = 0; arg[j] && ri < 250; j++) rline[ri++] = arg[j];
        }
        rline[ri] = '\0';
        last_status = 0;
        dispatch_command(cmd, arg, rline, ri);
        redir_restore();
        return -1;
    }
    if (str_needs_expand(line)) {
        /* The echo fallback must show the cooked text. The
         * whole line is expanded (a plain line has no
         * operators, and its pieces already expanded cleanly
         * into tighter caps, so this cannot fail); the
         * original spacing is preserved. 512 bytes provably
         * suffice: expanded cmd <= 63, expanded arg <= 191,
         * and the separators between them are bounded by the
         * 255-char line; cooking only ever removes bytes. */
        char xline[512];
        expand_str(line, xline, 512, 0);
        /* Dispatch-entry reset (see the contract on
         * dispatch_command): AFTER every expansion of this
         * line has read the old status -- the fallback line
         * above is one of those expansions -- and before
         * dispatch. The fallback may raise it to 1; nothing
         * after dispatch lowers it again, so a `run` line
         * leaves its last script line's status standing. */
        last_status = 0;
        dispatch_command(cmd, arg, xline, (long)slen_u(xline));
    } else {
        last_status = 0;
        dispatch_command(cmd, arg, line, n);
    }
    return prompt_nl;
}


void _start(long argc, char **argv)
{
    (void)argc; (void)argv;   /* unused for now; shell is interactive */
    sys_clear();
    puts_u("MyOS shell v0.3\r\nType 'help' for commands.\r\n> ");

    redir_save = -1;
    for (;;) {
        char line[256];
        long n = readline(line, 256);
        if (n <= 0) continue;
        if (n > 0 && line[n-1] == '\n') line[--n] = '\0';
        if (n == 0) { puts_u("> "); continue; }
        int fl = execute_line(line, n);
        puts_u(fl == 1 ? "\r\n> " : "> ");
    }
}


/* run <file>: execute a script (Phase 14 inc.8). Each line
 * goes through execute_line, the same path as an
 * interactive line; no prompt is printed per line — the
 * lines' flavours are folded into prompt_nl, so the caller
 * prints exactly one prompt when the script ends, with the
 * flavour of the last line that produced console output.
 * Errors (parse, not found, refusals) print and the script
 * continues with the next line. Nesting is capped at 4
 * active runs. The reader streams the file in 256-byte
 * chunks, so there is no file-size cap; lines are capped
 * at 255 chars — a longer line (after stripping one
 * trailing CR) is skipped with a message, never truncated
 * and executed, and a line containing a NUL byte is
 * skipped the same way. A final line without a newline
 * still executes. Script lines start with stdin_pipe
 * cleared: they do not inherit an outer stage's wiring
 * (and `run` is refused in pipelines and under
 * redirection, so this is belt and braces). */
static void cmd_run(const char *arg)
{
    if (!arg[0]) { puts_u("usage: run <file>\r\n"); return; }
    if (run_depth >= 4) { puts_u("run: nesting too deep\r\n"); return; }
    long fd = sys_open(arg);
    if (fd < 0) { puts_u("run: not found: "); puts_u(arg); prompt_nl = 1; return; }
    int saved_pipe = stdin_pipe; stdin_pipe = 0;
    run_depth++;
    int last = -1;
    char rbuf[256]; long rlen = 0, rpos = 0;
    int eof = 0;
    for (;;) {
        char lbuf[257];
        int llen = 0, toolong = 0, has_nul = 0, any = 0, rderr = 0;
        int hit_eof = 0;
        for (;;) {
            if (rpos >= rlen) {
                rlen = sys_read(fd, rbuf, 256); rpos = 0;
                if (rlen < 0) { rderr = 1; hit_eof = 1; break; }
                if (rlen == 0) { eof = 1; hit_eof = 1; break; }
            }
            char c = rbuf[rpos++]; any = 1;
            if (c == '\n') break;
            if (c == '\0') { has_nul = 1; continue; }
            if (llen < 256) lbuf[llen++] = c; else toolong = 1;
        }
        if (rderr) { puts_u("run: read error\r\n"); last = 0; break; }
        if (!any && llen == 0 && !toolong && !has_nul) break;
        if (llen > 0 && lbuf[llen - 1] == '\r') llen--;
        if (toolong || llen > 255) {
            puts_u("run: line too long, skipped\r\n"); last = 0;
        } else if (has_nul) {
            puts_u("run: line contains NUL, skipped\r\n"); last = 0;
        } else {
            lbuf[llen] = '\0';
            int fl = execute_line(lbuf, llen);
            if (fl >= 0) last = fl;
        }
        if (hit_eof || eof) break;
    }
    run_depth--;
    stdin_pipe = saved_pipe;
    sys_close(fd);
    prompt_nl = last;
}


/* The builtin dispatch chain, extracted from the shell's
 * main loop in Phase 14 inc.5 so a redirected run can swap
 * fd 1 around exactly one dispatch. Branches no longer
 * print the prompt: the caller prints it after dispatch
 * returns (and after fd 1 is restored, when redirected).
 * prompt_nl records the branches whose prompt used to be
 * "\r\n> " because their output does not end with a
 * newline; for them the caller prints "\r\n> ".
 *
 * Status contract (Phase 16 inc.3): last_status is owned
 * by the shell's layers, never by a builtin. Every caller
 * sets last_status = 0 at the moment it commits to
 * dispatching (execute_line's plain and redirect paths,
 * run_pipeline before its stage loop), and NOTHING
 * assigns it after dispatch returns -- so a `run` line
 * leaves its last script line's status standing, and a
 * pipeline keeps 1 if ANY stage failed. The one write
 * inside this function is the echo fallback's: an unknown
 * command is the dispatch layer's own failure, so it
 * raises last_status to 1. Builtin-reported failures
 * (cat: not found, usage: ..., run: not found) do not
 * touch it: with no exit-code ABI they are
 * indistinguishable from normal output without
 * pattern-matching text, which the model refuses. */
static void dispatch_command(const char *cmd, const char *arg,
                             const char *line, long n)
{
    prompt_nl = 0;
    if (streq(cmd, "exit"))    { puts_u("bye!\r\n"); sys_exit(0); }
    if (streq(cmd, "clear"))   { sys_clear(); return; }
    if (streq(cmd, "memtest")) { cmd_memtest(); return; }
    if (streq(cmd, "sched"))   { cmd_sched();   return; }
    if (streq(cmd, "trace")) {
        if (streq(arg, "full")) cmd_trace_full();
        else cmd_trace();
        return;
    }
    if (streq(cmd, "pftrace")) { cmd_pftrace(); return; }
    if (streq(cmd, "iotrace")) { cmd_iotrace(); return; }
    if (streq(cmd, "acctrace")) { cmd_acctrace(); return; }
    if (streq(cmd, "bcachestat")) { cmd_bcachestat(); return; }
    if (streq(cmd, "dmastat")) { cmd_dmastat(); return; }
    if (streq(cmd, "wstat")) { cmd_wstat(); return; }
    if (streq(cmd, "sync")) { cmd_sync(); return; }
    if (streq(cmd, "write")) { cmd_write(arg); return; }
    if (streq(cmd, "netstat")) { cmd_netstat(); return; }
    if (streq(cmd, "netsend")) { cmd_netsend(); return; }
    if (streq(cmd, "ping")) { cmd_ping(arg); return; }
    if (streq(cmd, "dhcp")) { cmd_dhcp(); return; }
    if (streq(cmd, "nslookup")) { cmd_nslookup(arg); return; }
    if (streq(cmd, "tcpecho")) { cmd_tcpecho(arg); return; }
    if (streq(cmd, "tcplisten")) { cmd_tcplisten(arg); return; }
    if (streq(cmd, "tcpechosrv")) { cmd_tcpechosrv(arg); return; }
    if (streq(cmd, "httpd")) { cmd_httpd(arg); return; }
    if (streq(cmd, "httpget")) { cmd_httpget(arg); return; }
    if (streq(cmd, "pfweights")) { cmd_pfweights(arg); return; }
    if (streq(cmd, "df")) { cmd_df(arg); return; }
    if (streq(cmd, "bcprobe")) { cmd_bcprobe(arg); return; }
    if (streq(cmd, "rm")) { cmd_rm(arg); return; }
    if (streq(cmd, "mv")) { cmd_mv(arg); return; }
    if (streq(cmd, "append")) { cmd_append(arg); return; }
    if (streq(cmd, "prefetch")) {
        long pi = -1;
        if (arg[0] >= '0' && arg[0] <= '9') pi = arg[0] - '0';
        if (pi < 0 || pi > 4) { puts_u("usage: prefetch <0|1|2|3|4>  (0=none 1=seq 2=stride 3=learned 4=learned-online)\r\n"); return; }
        long r = sys_prefetch_policy(pi);
        puts_u(r == 0 ? "prefetch policy set\r\n" : "prefetch: failed\r\n");
        return;
    }

    if (streq(cmd, "pfpolicy")) {
        long pi = -1;
        if (arg[0] >= '0' && arg[0] <= '9') pi = arg[0] - '0';
        if (pi != 0 && pi != 1) { puts_u("usage: pfpolicy <0|1>  (0=fifo  1=learned)\r\n"); return; }
        long r = sys_pf_setpolicy(pi);
        if (r < 0) puts_u("pfpolicy: unknown index\r\n");
        else { puts_u("pf policy set to "); puts_u(pi ? "learned\r\n" : "fifo\r\n"); }
        return;
    }
    if (streq(cmd, "policy")) {
        if (!arg[0]) {
            puts_u("usage: policy <0|1|2>  (0=round-robin  1=weighted-fair  2=learned)\r\n");
            return;
        }
        long idx = arg[0] - '0';
        long r   = sys_setpolicy(idx);
        if (r < 0) {
            puts_u("policy: unknown index\r\n");
        } else {
            puts_u("policy set to ");
            puts_u(idx == SCHED_POLICY_ML ? "learned" :
                    idx == SCHED_POLICY_WF ? "weighted-fair" : "round-robin");
            prompt_nl = 1;
        }
        return;
    }

    if (streq(cmd, "getpid")) {
        char buf[16];
        long pid = sys_getpid();
        puts_u("pid: "); puts_u(itoa(pid, buf)); prompt_nl = 1;
        return;
    }

    if (streq(cmd, "ls")) {
        char name[64];
        long idx = 0;
        while (sys_readdir(idx, name) == 0) {
            puts_u(name); puts_u("\r\n");
            idx++;
        }
       
        return;
    }

    if (streq(cmd, "run")) { cmd_run(arg); return; }

    if (streq(cmd, "set")) {
        /* Phase 16 inc.1: list the store, insertion order.
         * An argument is a named error (before today, `set`
         * was an unknown command and hit the echo fallback).
         * Like ls, each line ends with \r\n and the prompt
         * follows plainly; an empty store prints nothing. */
        if (arg[0]) {
            puts_u("set: takes no arguments"); prompt_nl = 1;
            return;
        }
        for (int i = 0; i < nvars; i++) {
            puts_u(vars[i].name); puts_u("=");
            puts_u(vars[i].value); puts_u("\r\n");
        }
        return;
    }

    if (streq(cmd, "unset")) {
        /* Phase 16 inc.1: remove one variable. arg is the raw
         * remainder and keeps any trailing separators (the
         * inc.4 rule), so trailing spaces/tabs are trimmed
         * before the name is validated; a name with inner
         * spaces then fails validation like any bad name. */
        char nm[192];
        int nl = 0;
        while (arg[nl] && nl < 191) { nm[nl] = arg[nl]; nl++; }
        while (nl > 0 && (nm[nl - 1] == ' ' || nm[nl - 1] == '\t'))
            nl--;
        nm[nl] = '\0';
        if (!nl) { puts_u("usage: unset <name>"); prompt_nl = 1; return; }
        if (!var_name_ok(nm, nl)) {
            puts_u("unset: invalid variable name"); prompt_nl = 1;
            return;
        }
        int at = var_find(nm, nl);
        if (at < 0) {
            puts_u("unset: no such variable: "); puts_u(nm);
            prompt_nl = 1;
            return;
        }
        for (int i = at; i + 1 < nvars; i++) {
            int j = 0;
            while ((vars[i].name[j] = vars[i + 1].name[j]) != '\0') j++;
            j = 0;
            while ((vars[i].value[j] = vars[i + 1].value[j]) != '\0') j++;
        }
        nvars--;
        return;
    }

    if (streq(cmd, "cat")) {
        if (!arg[0]) {
            if (!stdin_pipe) { puts_u("usage: cat <file>\r\n"); return; }
            /* Bare cat as a filter (Phase 14 inc.7): the
             * shell wired fd 0 to a pipe for this stage.
             * Copy fd 0 to fd 1 until a read returns <= 0:
             * 0 is EOF (under sequential wiring the writers
             * are gone before a consumer dispatches); a
             * negative value (-2 would-block, -1) also ends
             * the copy and is recorded for the caller to
             * report on the console after restore, since
             * nothing inside one dispatch can add data and
             * retrying would spin. A write returning other
             * than the full chunk (full downstream ring,
             * closed reader) stops the copy and drops the
             * rest; no write is ever retried. When fd 0 is
             * the console stdin_pipe is 0 and the usage
             * branch above runs, exactly as before, so
             * interactive bare cat cannot hang the shell. */
            char buf[128];
            long r;
            while ((r = sys_read(STDIN, buf, 127)) > 0) {
                if (sys_write(STDOUT, buf, r) != r) break;
            }
            if (r < 0) cat_stdin_err = r;
            prompt_nl = 1;
            return;
        }
        long fd = sys_open(arg);
        if (fd < 0) { puts_u("cat: not found: "); puts_u(arg); prompt_nl = 1; return; }
        char buf[128];
        long r;
        while ((r = sys_read(fd, buf, 127)) > 0)
            sys_write(STDOUT, buf, r);
        sys_close(fd);
        prompt_nl = 1;
        return;
    }

    if (streq(cmd, "pf")) {
        /* Deliberately fault from ring 3: write to an unmapped
         * address. Expect the #PF handler to fire with
         * cr2=0x50000000000, err=0x6 (not-present, write, user). */
        puts_u("pf: writing to unmapped 0x50000000000...\r\n");
        *(volatile char *)0x50000000000UL = 42;
        puts_u("pf: survived?!\r\n");
        return;
    }

    if (streq(cmd, "help")) {
        puts_u("commands: ls, cat <file>, date, exec <prog> [arg...], getpid, sched, trace, policy <0|1|2>, pftrace, pfpolicy <0|1>, iotrace, acctrace, bcachestat, dmastat, wstat, sync, write, append, rm, mv, netstat, netsend, ping, dhcp, nslookup, tcpecho, httpget, prefetch <0|1|2|3|4>, pfweights, memtest, pf, clear, exit\r\n");
        return;
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
        puts_u(itoa(t.seconds, buf)); prompt_nl = 1;
        return;
    }

    if (streq(cmd, "exec")) {
        if (!arg[0]) { puts_u("usage: exec <prog> [args]\r\n"); return; }
        /* Program name is the first token; the full line
         * (Phase 11 inc.4) becomes the child argv. */
        char prog[64];
        int pi2 = 0;
        while (arg[pi2] && arg[pi2] != ' ' && pi2 < 63) { prog[pi2] = arg[pi2]; pi2++; }
        prog[pi2] = 0;
        long pid = sys_exec(prog, arg);
        if (pid < 0) { puts_u("exec: failed\r\n"); return; }
        sys_wait(pid);
       
        return;
    }

    /* Unknown command: the dispatch layer's one failure
     * write (see the status contract above). */
    last_status = 1;
    puts_u("echo: ");
    sys_write(STDOUT, line, n);
    prompt_nl = 1;
}
