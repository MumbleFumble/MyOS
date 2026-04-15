.code64
.intel_syntax noprefix

/*
 * gdt_load(gdtr_addr, cs, ds)
 *   rdi = address of the packed GDTR struct (limit + base)
 *   si  = new code segment selector
 *   dx  = new data/stack segment selector
 *
 * We must do a far return (lretq) to reload CS, because a simple
 * mov to CS is not allowed in 64-bit mode.
 */
.global gdt_load

gdt_load:
    lgdt [rdi]

    /* Reload data segment registers with the new DS (rdx) */
    mov  ax, dx
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    /* Far return to reload CS:
     * Push CS selector then the return address, then lretq. */
    pop  rax                /* return address */
    push rsi                /* new CS (passed as 64-bit reg, only low 16 matter) */
    push rax                /* return address */
    lretq

/* Load the TSS selector into TR */
.global tss_load
tss_load:
    ltr  di
    ret
