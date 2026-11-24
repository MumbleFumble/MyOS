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
 * adjacent to words. There is no quoting or escaping: a quote
 * character is ordinary, and '|' / '>' anywhere on the line is
 * an operator; '<', ';' and '&' are ordinary characters.
 * Pipelines and redirection are parsed but NOT executed yet:
 * a valid line using them reports PARSE_UNSUPPORTED. The
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
static int         prompt_nl;       /* set by dispatch_command (inc.5) */
static long        redir_save;      /* saved console fd slot, or -1 */

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
        while (tokbuf[i] && tokbuf[i] != ' ' && tokbuf[i] != '\t' &&
               tokbuf[i] != '|' && tokbuf[i] != '>')
            i++;
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
 * raw, so write's free text keeps its double spaces. With
 * no quoting in the lexer, a word's raw end is exactly
 * start + its length. */
static void build_stage_arg(int w0, int wargc, const char *line,
                            char *arg)
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
            for (int q = tok_start[first]; q < last_end && ai < 191; q++)
                arg[ai++] = line[q];
        } else {
            int e1 = tok_start[gt_tok];
            while (e1 > tok_start[first] &&
                   (line[e1-1] == ' ' || line[e1-1] == '\t')) e1--;
            for (int q = tok_start[first]; q < e1 && ai < 191; q++)
                arg[ai++] = line[q];
            if (ai && ai < 191) arg[ai++] = ' ';
            for (int q = tok_start[split]; q < last_end && ai < 191; q++)
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
static void run_pipeline(const char *line)
{
    /* Pre-checks: nothing has run yet. */
    int aoff = 0;
    for (int s = 0; s < nstages_g; s++) {
        const char *nm = stg_argv[aoff];
        if (streq(nm, "exec") || streq(nm, "exit") ||
            streq(nm, "pf")) {
            puts_u("shell: "); puts_u(nm);
            puts_u(" not supported in pipelines\r\n> ");
            return;
        }
        aoff += stg_argc[s] + 1;
    }
    if (redirect_g && gt_stage != nstages_g - 1) {
        puts_u("shell: redirection in a pipeline must be on the last stage\r\n> ");
        return;
    }
    long tfd = -1;
    if (redirect_g) {
        tfd = sys_fcreate(redirect_g);
        if (tfd < 0) {
            puts_u("shell: cannot create "); puts_u(redirect_g);
            puts_u("\r\n> "); return;
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
        puts_u("shell: pipeline failed\r\n> ");
        return;
    }

    long prev_read = -1;
    int failed = 0;
    aoff = 0;
    int woff = 0;
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
        char scmd[64], sarg[192];
        const char *w = stg_argv[aoff];
        int ci = 0;
        while (w[ci] && ci < 63) { scmd[ci] = w[ci]; ci++; }
        scmd[ci] = '\0';
        build_stage_arg(woff, stg_argc[s], line, sarg);
        char rline[256];
        int ri = 0;
        for (int j = 0; scmd[j] && ri < 250; j++) rline[ri++] = scmd[j];
        if (sarg[0]) {
            rline[ri++] = ' ';
            for (int j = 0; sarg[j] && ri < 250; j++) rline[ri++] = sarg[j];
        }
        rline[ri] = '\0';
        dispatch_command(scmd, sarg, rline, ri);
        aoff += stg_argc[s] + 1;
        woff += stg_argc[s];
    }
    if (prev_read >= 0) sys_close(prev_read);
    if (tfd >= 0) sys_close(tfd);
    sys_dup2(sv0, 0); sys_close(sv0);
    sys_dup2(sv1, 1); sys_close(sv1);
    if (failed) { puts_u("shell: pipeline failed\r\n> "); return; }
    puts_u(redirect_g ? "> " : (prompt_nl ? "\r\n> " : "> "));
}


void _start(long argc, char **argv)
{
    (void)argc; (void)argv;   /* unused for now; shell is interactive */
    sys_clear();
    puts_u("MyOS shell v0.3\r\nType 'help' for commands.\r\n> ");

    redir_save = -1;
    for (;;) {
        redir_restore();
        char line[256];
        long n = readline(line, 256);
        if (n <= 0) continue;
        if (n > 0 && line[n-1] == '\n') line[--n] = '\0';
        if (n == 0) { puts_u("> "); continue; }

        /* Parse the line (Phase 14 inc.4). A plain command
         * feeds the dispatch chain below with cmd/arg as the
         * old first-space split produced them, with ONE
         * change: the separator run after the command word
         * no longer leaks into arg. arg is the raw remainder
         * of the line starting at the second token (inner and
         * trailing bytes byte-identical to before), and cmd
         * is the first token; both keep their old copy caps. */
        int pr = shell_parse(line);
        if (pr == PARSE_EMPTY) { puts_u("> "); continue; }
        if (pr == PARSE_ERROR) {
            puts_u("shell: parse error: "); puts_u(parse_err);
            puts_u("\r\n> "); continue;
        }

        char cmd[64], arg[192];
        {
            const char *w = stg_argv[0];
            int ci = 0;
            while (w[ci] && ci < 63) { cmd[ci] = w[ci]; ci++; }
            cmd[ci] = '\0';
            if (redirect_g) {
                build_stage_arg(0, stg_argc[0], line, arg);
            } else {
                const char *a = (ntok_g > 1) ? line + tok_start[1]
                                             : line + n;
                int ai = 0;
                while (a[ai] && ai < 191) { arg[ai] = a[ai]; ai++; }
                arg[ai] = '\0';
            }
        }

        if (nstages_g > 1) { run_pipeline(line); continue; }
        if (redirect_g) {
            if (streq(cmd, "exec")) {
                puts_u("shell: redirection not supported for exec\r\n> ");
                continue;
            }
            long tfd = sys_fcreate(redirect_g);
            if (tfd < 0) {
                puts_u("shell: cannot create "); puts_u(redirect_g);
                puts_u("\r\n> "); continue;
            }
            long sv = -1;
            for (long k = 15; k >= 3; k--)
                if (sys_dup2(k, k) < 0) { sv = k; break; }
            if (sv < 0 || sys_dup2(1, sv) < 0) {
                sys_close(tfd);
                puts_u("shell: redirection failed\r\n> "); continue;
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
            dispatch_command(cmd, arg, rline, ri);
            redir_restore();
            puts_u("> ");
            continue;
        }
        dispatch_command(cmd, arg, line, n);
        puts_u(prompt_nl ? "\r\n> " : "> ");
    }
}


/* The builtin dispatch chain, extracted from the shell's
 * main loop in Phase 14 inc.5 so a redirected run can swap
 * fd 1 around exactly one dispatch. Branches no longer
 * print the prompt: the caller prints it after dispatch
 * returns (and after fd 1 is restored, when redirected).
 * prompt_nl records the branches whose prompt used to be
 * "\r\n> " because their output does not end with a
 * newline; for them the caller prints "\r\n> ". */
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

    if (streq(cmd, "cat")) {
        if (!arg[0]) { puts_u("usage: cat <file>\r\n"); return; }
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

    puts_u("echo: ");
    sys_write(STDOUT, line, n);
    prompt_nl = 1;
}
