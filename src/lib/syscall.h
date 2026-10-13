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
#define SYS_TELEMETRY_DRAIN 15
#define SYS_PF_DRAIN 16

/* Policy indices for sys_setpolicy() */
#define SCHED_POLICY_RR  0   /* round-robin  */
#define SCHED_POLICY_WF  1   /* weighted-fair */
#define SCHED_POLICY_ML  2   /* learned */

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


/*
 * Per-context-switch trace record -- mirrors kernel struct switch_record.
 * Drained in bulk by sys_telemetry_drain().
 */
typedef struct {
    unsigned int   pid;
    unsigned char  state;     /* 0=READY 1=RUNNING 2=DEAD 3=WAITING */
    unsigned char  _pad[3];
    unsigned long  total_ticks;
    unsigned long  wait_ticks;
    unsigned long  syscall_count;
    unsigned long  io_block_count;
} trace_task_snapshot_t;      /* 40 bytes */

typedef struct {
    unsigned long  tick;
    unsigned int   prev_idx;
    unsigned int   next_idx;
    unsigned int   task_count;
    unsigned int   _pad;
    trace_task_snapshot_t tasks[32];
} switch_record_t;            /* 1304 bytes */

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
long  sys_telemetry_drain(switch_record_t *buf, long max_records);

/* Page-fault record -- mirrors kernel struct pf_record exactly. */
typedef struct {
    unsigned long  tick;
    unsigned long  cr2;
    unsigned long  err_code;
    unsigned int   pid;
    unsigned int   resolved;
} pf_record_t;               /* 32 bytes */

long sys_pf_drain(pf_record_t *buf, long max_records);
