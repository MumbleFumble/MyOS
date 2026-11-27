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
    cld                 /* DF is user-controlled state; kernel C assumes DF=0 */
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

/* ------------------------------------------------------------------
 * Generic exception stubs (Phase 17 inc.2) for the vectors
 * that share the kill-the-task path: #DE(0), #DB(1), #UD(6)
 * push no error code (a dummy 0 is pushed so the frame is
 * uniform); #SS(12) and #GP(13) use the CPU-pushed code.
 * Both forms then push the vector number and join
 * exc_common, which saves the same 15 GPRs as pf_stub and
 * calls exception_handler(vector, err, cs) with the frame
 * fields at fixed offsets from rsp at the call:
 *   [rsp+120] vector, [rsp+128] err, [rsp+136] RIP,
 *   [rsp+144] CS.
 * The handler never returns for a user-mode exception (it
 * kills the task) nor for a kernel-mode one (it halts); the
 * epilogue exists for shape.
 * ------------------------------------------------------------------ */
.extern exception_handler

.global exc0_stub
exc0_stub:
    cld
    push 0
    push 0
    jmp exc_common

.global exc1_stub
exc1_stub:
    cld
    push 0
    push 1
    jmp exc_common

.global exc6_stub
exc6_stub:
    cld
    push 0
    push 6
    jmp exc_common

.global exc12_stub
exc12_stub:
    cld
    push 12
    jmp exc_common

.global exc13_stub
exc13_stub:
    cld
    push 13
    jmp exc_common

exc_common:
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

    mov rdi, [rsp + 120]
    mov rsi, [rsp + 128]
    mov rdx, [rsp + 144]
    call exception_handler

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
    add rsp, 16         /* discard vector + error code */
    iretq
