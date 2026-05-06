/* Minimal syscall stubs for ring-3 user programs.
 * Calling convention: standard SysV x86-64 (rdi, rsi, rdx for args).
 * Kernel ABI:  rax = syscall nr,  rdi/rsi/rdx = arg1/2/3.
 * Return value in rax.
 */

.code64
.intel_syntax noprefix

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

/* long sys_exec(const char *name, const char *cmdline) */
/* rdi = name, rsi = cmdline — already in correct regs for arg1/arg2 */
.global sys_exec
sys_exec:
    mov  rax, 8         /* SYS_EXEC */
    int  0x80
    ret

.section .note.GNU-stack,"",@progbits
