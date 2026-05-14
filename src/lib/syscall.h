#pragma once

/* Syscall numbers — must match kernel's sys/syscall.h */
#define SYS_WRITE  1
#define SYS_EXIT   2
#define SYS_READ   3
#define SYS_SBRK   4
#define SYS_CLEAR  5
#define SYS_GETPID 6
#define SYS_WAIT   7
#define SYS_EXEC   8
#define SYS_OPEN   9
#define SYS_CLOSE  10
#define SYS_READDIR   11
#define SYS_TIME      12
#define SYS_TELEMETRY 13
#define SYS_SETPOLICY 14

/* Policy indices for sys_setpolicy() */
#define SCHED_POLICY_RR  0   /* round-robin  */
#define SCHED_POLICY_WF  1   /* weighted-fair */

/* Matches kernel rtc_time_t exactly */
typedef struct {
    unsigned char  seconds;
    unsigned char  minutes;
    unsigned char  hours;
    unsigned char  day;
    unsigned char  month;
    unsigned short year;
} sys_time_t;

/*
 * Per-task telemetry snapshot — mirrors kernel task_stat_t exactly.
 * Returned in bulk by sys_telemetry().
 */
typedef struct {
    unsigned int   pid;
    unsigned char  state;     /* 0=READY 1=RUNNING 2=DEAD 3=WAITING */
    unsigned char  _pad[3];
    char           name[16];
    unsigned long  total_ticks;
    unsigned long  wait_ticks;
    unsigned long  syscall_count;
    unsigned long  io_block_count;
} task_stat_t;               /* 56 bytes */

/* File descriptors */
#define STDIN   0
#define STDOUT  1
#define STDERR  2

/* Raw syscall stubs (implemented in syscall.S) */
long sys_write(long fd, const void *buf, long count);
long sys_read(long fd, void *buf, long count);
void sys_exit(long status) __attribute__((noreturn));
void *sys_sbrk(long increment);
void  sys_clear(void);
long  sys_getpid(void);
long  sys_wait(long pid);
long  sys_exec(const char *name, const char *cmdline);
long  sys_open(const char *path);
long  sys_close(long fd);
long  sys_readdir(long index, char *name_buf);
long  sys_time(sys_time_t *buf);
long  sys_telemetry(task_stat_t *buf, long max_tasks);
long  sys_setpolicy(long index);
