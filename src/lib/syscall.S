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

.section .note.GNU-stack,"",@progbits
