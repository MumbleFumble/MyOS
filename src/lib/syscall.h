#pragma once

/* Syscall numbers — must match kernel's sys/syscall.h */
#define SYS_WRITE  1
#define SYS_EXIT   2
#define SYS_READ   3
#define SYS_SBRK   4
#define SYS_CLEAR  5

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
