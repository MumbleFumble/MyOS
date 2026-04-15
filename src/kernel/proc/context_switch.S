.code64
.intel_syntax noprefix

/*
 * context_switch(old_rsp_ptr, new_rsp)
 *   rdi = pointer to where we save the old task's RSP  (uint64_t *)
 *   rsi = new task's RSP to restore
 *
 * We save all callee-saved registers onto the current (old) stack,
 * save RSP, swap to the new stack, restore registers, and ret —
 * which pops the new task's saved RIP and resumes it.
 *
 * Callee-saved registers in the SysV x86-64 ABI:
 *   rbp, rbx, r12, r13, r14, r15
 * (rsp is saved/restored explicitly)
 */
.global context_switch

context_switch:
    /* ---- save old task ---- */
    push r15
    push r14
    push r13
    push r12
    push rbx
    push rbp

    /* Save current RSP into *old_rsp_ptr */
    mov  [rdi], rsp

    /* ---- restore new task ---- */
    mov  rsp, rsi

    pop  rbp
    pop  rbx
    pop  r12
    pop  r13
    pop  r14
    pop  r15

    /* ret pops the saved RIP of the new task */
    ret

/* Mark stack as non-executable */
.section .note.GNU-stack, "", @progbits
