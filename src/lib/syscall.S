/* Minimal syscall stubs for ring-3 user programs.
 * Calling convention: standard SysV x86-64 (rdi, rsi, rdx for args).
 * Kernel ABI:  rax = syscall nr,  rdi/rsi/rdx = arg1/2/3.
 * Return value in rax.
 */

.code64
.intel_syntax noprefix

/* Process entry point (Phase 11 inc.4). The kernel ELF loader
 * builds a System V initial stack (argc at [rsp], argv pointers
 * above it), but a C _start expects argc/argv in rdi/rsi. _entry
 * bridges the two and is the ELF entry of every user program.
 * If _start returns, exit(0). */
.global _entry
_entry:
    mov  rdi, [rsp]         /* argc */
    lea  rsi, [rsp + 8]     /* argv */
    xor  rbp, rbp
    call _start
    xor  rdi, rdi
    mov  rax, 2             /* SYS_EXIT */
    int  0x80
    ud2

/* long sys_write(long fd, const void *buf, long count) */
.global sys_write
sys_write:
    mov  rax, 1         /* SYS_WRITE */
    int  0x80
    ret

/* void sys_exit(long status) — does not return */
.global sys_exit
sys_exit:
    mov  rax, 2         /* SYS_EXIT */
    int  0x80
.hang:
    hlt
    jmp .hang

/* long sys_read(long fd, void *buf, long count) */
.global sys_read
sys_read:
    mov  rax, 3         /* SYS_READ */
    int  0x80
    ret

/* void *sys_sbrk(long increment) */
.global sys_sbrk
sys_sbrk:
    mov  rax, 4         /* SYS_SBRK */
    int  0x80
    ret

/* void sys_clear(void) */
.global sys_clear
sys_clear:
    mov  rax, 5         /* SYS_CLEAR */
    int  0x80
    ret

/* long sys_getpid(void) */
.global sys_getpid
sys_getpid:
    mov  rax, 6         /* SYS_GETPID */
    int  0x80
    ret

/* long sys_wait(long pid) */
.global sys_wait
sys_wait:
    mov  rax, 7         /* SYS_WAIT */
    int  0x80
    ret

/* long sys_open(const char *path) */
.global sys_open
sys_open:
    mov  rax, 9         /* SYS_OPEN */
    int  0x80
    ret

/* long sys_close(long fd) */
.global sys_close
sys_close:
    mov  rax, 10        /* SYS_CLOSE */
    int  0x80
    ret

/* long sys_readdir(long index, char *name_buf) */
.global sys_readdir
sys_readdir:
    mov  rax, 11        /* SYS_READDIR */
    int  0x80
    ret

/* long sys_time(sys_time_t *buf) */
.global sys_time
sys_time:
    mov  rax, 12        /* SYS_TIME */
    int  0x80
    ret

/* long sys_telemetry(task_stat_t *buf, long max_tasks) */
.global sys_telemetry
sys_telemetry:
    mov  rax, 13        /* SYS_TELEMETRY */
    int  0x80
    ret

/* long sys_telemetry_drain(switch_record_t *buf, long max_records) */
.global sys_telemetry_drain
sys_telemetry_drain:
    mov  rax, 15        /* SYS_TELEMETRY_DRAIN */
    int  0x80
    ret

/* long sys_pf_drain(pf_record_t *buf, long max_records) */
.global sys_pf_drain
sys_pf_drain:
    mov  rax, 16        /* SYS_PF_DRAIN */
    int  0x80
    ret

/* long sys_pf_setpolicy(long index) */
.global sys_pf_setpolicy
sys_pf_setpolicy:
    mov  rax, 17        /* SYS_PF_SETPOLICY */
    int  0x80
    ret

/* long sys_io_drain(io_record_t *buf, long max_records) */
.global sys_io_drain
sys_io_drain:
    mov  rax, 18        /* SYS_IO_DRAIN */
    int  0x80
    ret

/* long sys_bcache_stat(unsigned long *hits, unsigned long *misses) */
.global sys_bcache_stat
sys_bcache_stat:
    mov  rax, 19        /* SYS_BCACHE_STAT */
    int  0x80
    ret

/* long sys_dma_stat(unsigned long *dma, unsigned long *err, unsigned long *pio) */
.global sys_dma_stat
sys_dma_stat:
    mov  rax, 22        /* SYS_DMA_STAT */
    int  0x80
    ret

/* long sys_wstat(unsigned long *dma, unsigned long *err, unsigned long *pio) */
.global sys_wstat
sys_wstat:
    mov  rax, 23        /* SYS_WSTAT */
    int  0x80
    ret

/* long sys_raw_sector(unsigned long idx, void *buf, long is_write) */
.global sys_raw_sector
sys_raw_sector:
    mov  rax, 24        /* SYS_RAW_SECTOR */
    int  0x80
    ret
/* long sys_bcache_sync(void) */
.global sys_bcache_sync
sys_bcache_sync:
    mov  rax, 25        /* SYS_BCACHE_SYNC */
    int  0x80
    ret
/* long sys_fcreate(const char *path) */
.global sys_fcreate
sys_fcreate:
    mov  rax, 26        /* SYS_FCREATE */
    int  0x80
    ret
/* long sys_netstat(struct netstat_out *out) */
.global sys_netstat
sys_netstat:
    mov  rax, 27        /* SYS_NETSTAT */
    int  0x80
    ret

/* long sys_netsend(void) */
.global sys_netsend
sys_netsend:
    mov  rax, 28        /* SYS_NETSEND */
    int  0x80
    ret
/* long sys_ping(const unsigned char *ip, unsigned short seq) */
.global sys_ping
sys_ping:
    mov  rax, 29        /* SYS_PING */
    int  0x80
    ret
/* long sys_dhcp(struct dhcp_out *out) */
.global sys_dhcp
sys_dhcp:
    mov  rax, 30        /* SYS_DHCP */
    int  0x80
    ret
/* long sys_nslookup(const char *name, const unsigned char *server,
 *                   unsigned char out[4]) */
.global sys_nslookup
sys_nslookup:
    mov  rax, 31        /* SYS_NSLOOKUP */
    int  0x80
    ret
/* long sys_tcpecho(const unsigned char *ip, unsigned short port,
 *                  struct tcpecho_args *args) */
.global sys_tcpecho
sys_tcpecho:
    mov  rax, 32        /* SYS_TCPECHO */
    int  0x80
    ret
/* long sys_pfweights(unsigned int mode, struct pfweights_out *out) */
.global sys_pfweights
sys_pfweights:
    mov  rax, 33        /* SYS_PFWEIGHTS */
    int  0x80
    ret
/* long sys_funlink(const char *path) */
.global sys_funlink
sys_funlink:
    mov  rax, 34        /* SYS_FUNLINK */
    int  0x80
    ret

/* long sys_frename(const char *old_path, const char *new_path) */
.global sys_frename
sys_frename:
    mov  rax, 35        /* SYS_FRENAME */
    int  0x80
    ret

/* long sys_prefetch_policy(long index) */
.global sys_prefetch_policy
sys_prefetch_policy:
    mov  rax, 20        /* SYS_PREFETCH_POLICY */
    int  0x80
    ret

/* long sys_access_drain(access_record_t *buf, long max_records) */
.global sys_access_drain
sys_access_drain:
    mov  rax, 21        /* SYS_ACCESS_DRAIN */
    int  0x80
    ret

/* long sys_setpolicy(long index) */
.global sys_setpolicy
sys_setpolicy:
    mov  rax, 14        /* SYS_SETPOLICY */
    int  0x80
    ret

/* long sys_exec(const char *name, const char *cmdline) */
/* rdi = name, rsi = cmdline — already in correct regs for arg1/arg2 */
.global sys_exec
sys_exec:
    mov  rax, 8         /* SYS_EXEC */
    int  0x80
    ret

.section .note.GNU-stack,"",@progbits
