OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
  . = 1M;

  .multiboot_header :
  {
    KEEP(*(.multiboot_header))
  }

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
  }
}
