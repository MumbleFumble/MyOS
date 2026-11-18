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
    puts_u(" rx="); puts_u(itoa((long)o.rx_frames, buf));
    puts_u(" tx="); puts_u(itoa((long)o.tx_frames, buf));
    puts_u("\r\n");
}

static void cmd_netsend(void)
{
    if (sys_netsend() != 0) { puts_u("netsend: failed\r\n"); return; }
    puts_u("netsent\r\n");
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
        if (streq(cmd, "sched"))   { cmd_sched();   puts_u("> "); continue; }
        if (streq(cmd, "trace")) {
            if (streq(arg, "full")) cmd_trace_full();
            else cmd_trace();
            puts_u("> "); continue;
        }
        if (streq(cmd, "pftrace")) { cmd_pftrace(); puts_u("> "); continue; }
        if (streq(cmd, "iotrace")) { cmd_iotrace(); puts_u("> "); continue; }
        if (streq(cmd, "acctrace")) { cmd_acctrace(); puts_u("> "); continue; }
        if (streq(cmd, "bcachestat")) { cmd_bcachestat(); puts_u("> "); continue; }
        if (streq(cmd, "dmastat")) { cmd_dmastat(); puts_u("> "); continue; }
        if (streq(cmd, "wstat")) { cmd_wstat(); puts_u("> "); continue; }
        if (streq(cmd, "sync")) { cmd_sync(); puts_u("> "); continue; }
        if (streq(cmd, "write")) { cmd_write(arg); puts_u("> "); continue; }
        if (streq(cmd, "netstat")) { cmd_netstat(); puts_u("> "); continue; }
        if (streq(cmd, "netsend")) { cmd_netsend(); puts_u("> "); continue; }
        if (streq(cmd, "append")) { cmd_append(arg); puts_u("> "); continue; }
        if (streq(cmd, "prefetch")) {
            long pi = -1;
            if (arg[0] >= '0' && arg[0] <= '9') pi = arg[0] - '0';
            if (pi < 0 || pi > 3) { puts_u("usage: prefetch <0|1|2|3>  (0=none 1=seq 2=stride 3=learned)\r\n> "); continue; }
            long r = sys_prefetch_policy(pi);
            puts_u(r == 0 ? "prefetch policy set\r\n> " : "prefetch: failed\r\n> ");
            continue;
        }

        if (streq(cmd, "pfpolicy")) {
            long pi = -1;
            if (arg[0] >= '0' && arg[0] <= '9') pi = arg[0] - '0';
            if (pi != 0 && pi != 1) { puts_u("usage: pfpolicy <0|1>  (0=fifo  1=learned)\r\n> "); continue; }
            long r = sys_pf_setpolicy(pi);
            if (r < 0) puts_u("pfpolicy: unknown index\r\n> ");
            else { puts_u("pf policy set to "); puts_u(pi ? "learned\r\n> " : "fifo\r\n> "); }
            continue;
        }
        if (streq(cmd, "policy")) {
            if (!arg[0]) {
                puts_u("usage: policy <0|1|2>  (0=round-robin  1=weighted-fair  2=learned)\r\n> ");
                continue;
            }
            long idx = arg[0] - '0';
            long r   = sys_setpolicy(idx);
            if (r < 0) {
                puts_u("policy: unknown index\r\n> ");
            } else {
                puts_u("policy set to ");
                puts_u(idx == SCHED_POLICY_ML ? "learned" :
                        idx == SCHED_POLICY_WF ? "weighted-fair" : "round-robin");
                puts_u("\r\n> ");
            }
            continue;
        }

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

        if (streq(cmd, "pf")) {
            /* Deliberately fault from ring 3: write to an unmapped
             * address. Expect the #PF handler to fire with
             * cr2=0x50000000000, err=0x6 (not-present, write, user). */
            puts_u("pf: writing to unmapped 0x50000000000...\r\n");
            *(volatile char *)0x50000000000UL = 42;
            puts_u("pf: survived?!\r\n> ");
            continue;
        }

        if (streq(cmd, "help")) {
            puts_u("commands: ls, cat <file>, date, exec <prog> [arg...], getpid, sched, trace, policy <0|1|2>, pftrace, pfpolicy <0|1>, iotrace, acctrace, bcachestat, dmastat, wstat, sync, write, append, netstat, netsend, prefetch <0|1|2|3>, memtest, pf, clear, exit\r\n> ");
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

