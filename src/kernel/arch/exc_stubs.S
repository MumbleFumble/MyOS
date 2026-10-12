.code64
.intel_syntax noprefix

.extern page_fault_handler

/*
 * Page-fault entry stub (IDT vector 14).
 *
 * Unlike IRQs, the CPU pushes an 8-byte error code before entering a
 * #PF handler. Stack layout on entry (low address -> high):
 *   error_code, RIP, CS, RFLAGS, RSP, SS
 *
 * We save the GPRs, pass the error code in RDI to the C handler, then
 * discard the error code before iretq. The handler currently halts
 * (no demand pager yet), so the epilogue below is for shape.
 */
.global pf_stub
pf_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    /* Error code sits above the 15 pushed registers: 15 * 8 = 120 */
    mov rdi, [rsp + 120]
    call page_fault_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 8          /* discard the CPU-pushed error code */
    iretq
