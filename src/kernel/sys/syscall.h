#pragma once

#include <stdint.h>

/* EFAULT: a syscall's user pointer was refused by the
 * validated copy layer (Phase 17 inc.3): the range is not
 * wholly present, user-accessible memory in the caller's
 * own address space. -3 collides with no existing return
 * in the tree (flat -1; E_WOULDBLOCK is -2). Mirrored for
 * user programs in src/lib/syscall.h. */
#define EFAULT (-3)

/* Syscall numbers */
#define SYS_WRITE   1   /* write(fd, buf, count)  -> bytes written */
#define SYS_EXIT    2   /* exit(status)           -> does not return */
#define SYS_READ    3   /* read(fd, buf, count)   -> bytes read     */
#define SYS_SBRK    4   /* sbrk(increment)        -> old break addr */
#define SYS_CLEAR   5   /* clear()                -> 0              */
#define SYS_GETPID  6   /* getpid()               -> current pid    */
#define SYS_WAIT    7   /* wait(pid)              -> child exit code */
#define SYS_EXEC    8   /* exec(name)             -> child pid      */
#define SYS_OPEN    9   /* open(path)             -> fd             */
#define SYS_CLOSE   10  /* close(fd)              -> 0              */
#define SYS_READDIR 11  /* readdir(index, namebuf)-> 0 or -1        */
#define SYS_TIME      12  /* time(rtc_time_t *buf)       -> 0          */
#define SYS_TELEMETRY 13  /* telemetry(buf, max_tasks)   -> task count */
#define SYS_SETPOLICY 14  /* setpolicy(index)            -> 0 or -1   */
#define SYS_TELEMETRY_DRAIN 15  /* telemetry_drain(buf, max) -> records  */
#define SYS_PF_DRAIN 16  /* pf_drain(buf, max) -> fault records      */
#define SYS_PF_SETPOLICY 17  /* pf_setpolicy(index) -> 0 or -1      */
#define SYS_IO_DRAIN 18  /* io_drain(buf, max) -> I/O records       */
#define SYS_BCACHE_STAT 19  /* bcache_stat(hits_out, misses_out) */
#define SYS_PREFETCH_POLICY 20  /* prefetch_policy(idx) -> 0/-1 */
#define SYS_ACCESS_DRAIN 21  /* access_drain(buf, max) -> demand access records */
#define SYS_DMA_STAT 22  /* dma_stat(dma_out, err_out, pio_out) */
#define SYS_WSTAT 23  /* wstat(dma_out, err_out, pio_out) - write path */
#define SYS_RAW_SECTOR 24  /* raw_sector(idx, buf, is_write) - scratch-area test hook */
#define SYS_BCACHE_SYNC 25  /* bcache_sync() -> 0 or -1 */
#define SYS_FCREATE 26     /* fcreate(path) -> fd (create/truncate) */
#define SYS_NETSTAT 27     /* netstat(out) -> 0 (Phase 7) */
#define SYS_NETSEND 28     /* netsend() -> 0 or -1 (Phase 7 test frame) */
#define SYS_PING 29        /* ping(ip_ptr, seq) -> rtt ticks or -1 (Phase 7) */
#define SYS_DHCP 30        /* dhcp(out_ptr) -> 0 or -1 (Phase 8) */
#define SYS_NSLOOKUP 31    /* nslookup(name, server_or_0, out4) -> 0 or -1 (Phase 9) */
#define SYS_TCPECHO 32     /* tcpecho(ip, port, args) -> bytes received or -1 (Phase 9) */
#define SYS_PFWEIGHTS 33   /* pfweights(mode, out) -> 0 or -1 (Phase 10) */
#define SYS_FUNLINK 34     /* funlink(path) -> 0 or -1 (Phase 11) */
#define SYS_FRENAME 35     /* frename(old, new) -> 0 or -1 (Phase 11) */
#define SYS_TCPLISTEN 36   /* tcplisten(port) -> 0 or -1 (Phase 12) */
#define SYS_TCPACCEPT 37   /* tcpaccept(out) -> 0 or -1 (Phase 12) */
#define SYS_TCPSRV_CLOSE 38 /* tcpserve_close() -> 0 or -1 (Phase 12) */
#define SYS_TCPSRV_SEND 39 /* tcpsrv_send(data, len) -> len or -1 (Phase 12) */
#define SYS_TCPSRV_RECV 40 /* tcpsrv_recv(out, cap) -> n, 0=EOF, -1 (Phase 12) */        /* ping(ip_ptr, seq) -> rtt ticks or -1 (Phase 7) */     /* netsend() -> 0 or -1 (Phase 7 test frame) */     /* fcreate(path) -> fd (create/truncate) */  /* bcache_sync() -> 0 or -1 */
#define SYS_DF 41          /* df(out) -> 0 or -1 (Phase 13) */
#define SYS_BCACHE_PROBE 42 /* bcache_probe(lba) -> state bits (Phase 13) */
#define SYS_DUP2 43        /* dup2(oldfd, newfd) -> newfd or -1 (Phase 14) */
#define SYS_EXEC_INHERIT 45 /* exec_inherit(name, cmdline, map) -> child pid */
#define SYS_PIPE 44        /* pipe(fds_out) -> 0 or -1 (Phase 14) */
#define SYS_KILL 46        /* kill(pid) -> 0 or -1 (Phase 18) */

/* Scheduling policy indices for SYS_SETPOLICY */
#define SCHED_POLICY_RR  0   /* round-robin (default)   */
#define SCHED_POLICY_WF  1   /* weighted-fair           */
#define SCHED_POLICY_ML  2   /* learned (linear model)  */

/* fd values */
#define FD_STDIN    0
#define FD_STDOUT   1
#define FD_STDERR   2

/*
 * Install int 0x80 trap gate in the IDT (DPL=3 so ring-3 code can invoke it).
 */
void syscall_init(void);

/*
 * Called from isr80.S with the syscall number and first three arguments.
 * Returns the syscall return value in RAX.
 */
int64_t syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3);
