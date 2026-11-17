#pragma once

#include <stdint.h>

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
