/* Linker script for user-space programs.
 * Load address 0x400000 (4MB) — well above the kernel at 1MB,
 * and above the kernel heap (4-12MB)... actually use 0x1000000 (16MB)
 * since the kernel heap sits at 4-12MB.  PMM hands out frames from
 * 16MB onwards so we map user ELFs starting there.
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x10000000000;

    .text : ALIGN(4K)
    {
        *(.text*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata*)
    }

    .data : ALIGN(4K)
    {
        *(.data*)
    }

    .bss : ALIGN(4K)
    {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.note.GNU-stack)
        *(.comment)
    }
}
