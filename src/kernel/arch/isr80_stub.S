    .section .text
    .globl isr80

isr80:
    pusha                 # save all general registers
    call isr80_handler    # C function, no args
    popa                  # restore registers
    iretd                 # return from interrupt
