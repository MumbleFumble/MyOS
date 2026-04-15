#pragma once

#include <stdint.h>

/* Syscall numbers */
#define SYS_WRITE   1   /* write(fd, buf, count)  -> bytes written */
#define SYS_EXIT    2   /* exit(status)           -> does not return */

/* fd values */
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
