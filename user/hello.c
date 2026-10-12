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

        if (streq(cmd, "help")) {
            puts_u("commands: ls, cat <file>, date, exec <prog> [arg...], getpid, sched, trace, policy <0|1|2>, memtest, clear, exit\r\n> ");
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

