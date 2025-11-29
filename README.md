# MyOS

MyOS is a 64-bit microkernel-style operating system built from scratch as a systems programming project.
The goal is to implement:

- 64-bit bootloader (GRUB)
- Paging + virtual memory
- Interrupt handling + driver layer
- System calls
- Process scheduler
- User-space ELF loader
- Minimal libc
- Simple shell

## Build & Run

Requirements:
- GCC cross-compiler (x86_64-elf-gcc)
- GNU binutils
- QEMU
- Make

To build:


