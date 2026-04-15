.code64
.intel_syntax noprefix

.extern syscall_dispatch

/*
 * int 0x80 syscall entry point.
 *
 * Calling convention (kernel and future ring-3):
 *   rax = syscall number
 *   rdi = arg1,  rsi = arg2,  rdx = arg3
 *
 * Return value in rax.
 *
 * We save rdi/rsi/rdx *before* overwriting them for the C call,
 * then pass the originals as args to syscall_dispatch(nr, arg1, arg2, arg3).
 */
.global isr80

isr80:
    /* Save callee-saved and the registers we'll clobber for the C call */
    push r15
    push r14
    push r13
    push r12
    push rbp
    push rbx
    push r11
    push r10
    push r9
    push r8
    push rcx

    /* Save the three argument registers before clobbering them */
    push rdx        /* arg3 (original rdx) */
    push rsi        /* arg2 (original rsi) */
    push rdi        /* arg1 (original rdi) */

    /* syscall_dispatch(nr=rax, arg1=original_rdi, arg2=original_rsi, arg3=original_rdx) */
    mov  rdi, rax           /* nr */
    mov  rsi, [rsp]         /* arg1 = original rdi */
    mov  rdx, [rsp + 8]     /* arg2 = original rsi */
    mov  rcx, [rsp + 16]    /* arg3 = original rdx */
    call syscall_dispatch
    /* rax now holds the return value */

    /* Discard the three saved arg regs (return value stays in rax) */
    add  rsp, 24

    pop  rcx
    pop  r8
    pop  r9
    pop  r10
    pop  r11
    pop  rbx
    pop  rbp
    pop  r12
    pop  r13
    pop  r14
    pop  r15

    iretq
