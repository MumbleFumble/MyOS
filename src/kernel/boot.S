/* Multiboot2 header + 64-bit long mode entry */

    .section .multiboot_header,"a"
    .align 8

    .set MB2_MAGIC,      0xE85250D6
    .set MB2_ARCH_I386,  0
    .set MB2_HEADER_LEN, multiboot_header_end - multiboot_header_start

multiboot_header_start:
    .long MB2_MAGIC
    .long MB2_ARCH_I386
    .long MB2_HEADER_LEN
    .long -(MB2_MAGIC + MB2_ARCH_I386 + MB2_HEADER_LEN)

    /* End tag: type=0, size=8 */
    .short 0
    .short 0
    .long  8

multiboot_header_end:

    .section .text
    .code32
    .globl _start
    .extern kernel_main

_start:
    cli
    
    /* Save multiboot pointer IMMEDIATELY - it's in EBX */
    mov %ebx, %esi
    
    /* Write 'B' to show we started */
    mov $0xB8000, %edi
    movw $0x0F42, (%edi)
    
    /* Debug: print multiboot pointer in 32-bit mode */
    /* Print "MB=" at position 1 */
    movw $0x0F4D, 2(%edi)   /* 'M' */
    movw $0x0F42, 4(%edi)   /* 'B' */
    movw $0x0F3D, 6(%edi)   /* '=' */
    
    /* Print EBX value as 8 hex digits */
    mov %esi, %eax          /* EBX value (saved in ESI) */
    mov $8, %cl
.print_hex_32:
    rol $4, %eax
    push %eax
    and $0xF, %eax
    cmp $10, %al
    jl .is_digit_32
    add $('A'-'0'-10), %al
.is_digit_32:
    add $'0', %al
    mov %al, %ah
    mov $0x0F, %al
    mov %ah, %al
    mov $8, %bl
    sub %cl, %bl
    movzx %bl, %ebx
    shl $1, %ebx
    movw %ax, 8(%edi,%ebx)
    movw %ax, 8(%edi,%ebx)
    pop %eax
    dec %cl
    jnz .print_hex_32
    
    /* Set up page tables in statically allocated space */
    /* Zero them first */
    mov $boot_pml4, %edi
    mov $12288, %ecx
    xor %eax, %eax
    rep stosb
    
    /* Write 'C' to show page tables zeroed */
    mov $0xB8000, %edi
    movw $0x0F43, 2(%edi)
    
    /* PML4[0] -> PDPT */
    mov $boot_pdpt, %eax
    or $0x3, %eax
    mov %eax, boot_pml4
    
    /* PDPT[0] -> PD */
    mov $boot_pd, %eax
    or $0x3, %eax
    mov %eax, boot_pdpt
    
    /* Map first 16MB with 2MB huge pages (8 entries) */
    mov $boot_pd, %edi
    mov $0x83, %eax         /* Present, writable, huge page */
    mov $8, %ecx            /* 8 entries = 16MB */
.map_page:
    mov %eax, (%edi)
    add $0x200000, %eax     /* Next 2MB */
    add $8, %edi            /* Next PD entry */
    loop .map_page
    
    /* Write 'D' to show page tables set up */
    mov $0xB8000, %edi
    movw $0x0F44, 4(%edi)
    
    /* Load CR3 with PML4 address */
    mov $boot_pml4, %eax
    mov %eax, %cr3
    
    /* Write 'E' */
    mov $0xB8000, %edi
    movw $0x0F45, 6(%edi)
    
    /* Enable PAE in CR4 */
    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4
    
    /* Write 'F' */
    mov $0xB8000, %edi
    movw $0x0F46, 8(%edi)
    
    /* Set LME bit in EFER MSR */
    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr
    
    /* Write 'G' */
    mov $0xB8000, %edi
    movw $0x0F47, 10(%edi)
    
    /* Enable paging in CR0 (this activates long mode) */
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0
    
    /* Write 'H' - if we see this, we're in long mode! */
    mov $0xB8000, %edi
    movw $0x0F48, 12(%edi)
    
    /* Load 64-bit GDT */
    lgdt gdt64_ptr
    
    /* Write 'I' */
    mov $0xB8000, %edi
    movw $0x0F49, 14(%edi)
    
    /* Far jump to 64-bit code segment */
    ljmp $0x08, $long_mode_start

.code64
long_mode_start:
    /* Set up 64-bit segment registers */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    
    /* Set up stack */
    mov $kernel_stack_top, %rsp
    xor %rbp, %rbp
    
    /* ESI has the 32-bit multiboot pointer - zero-extend to RDI (first arg in x86-64 calling convention) */
    mov %esi, %edi
    
    /* Call kernel_main */
    call kernel_main
    
.hang:
    hlt
    jmp .hang

    .section .rodata
    .align 16
gdt64:
    .quad 0x0000000000000000    /* Null descriptor */
    .quad 0x00209A0000000000    /* 64-bit code segment */
    .quad 0x0000920000000000    /* 64-bit data segment */
gdt64_end:

gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

    .section .bss
    .align 4096
boot_pml4:
    .space 4096
boot_pdpt:
    .space 4096
boot_pd:
    .space 4096

    .align 16
kernel_stack:
    .space 16384
kernel_stack_top:
