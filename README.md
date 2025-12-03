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

### Requirements

- GCC cross-compiler (`x86_64-elf-gcc`)
- GNU binutils for `x86_64-elf`
- QEMU (`qemu-system-x86_64`)
- `grub-mkrescue` and `xorriso`
- `make`

### Quick start (via WSL/Ubuntu)

On Windows, the simplest setup is using WSL with Ubuntu.

In PowerShell (once):

```powershell
wsl --install -d Ubuntu
```

Then, inside Ubuntu:

```bash
sudo apt update
sudo apt install build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo libisl-dev
sudo apt install qemu-system-x86 grub-pc-bin xorriso
sudo apt install nasm
```

Build a cross-compiler (one-time):

```bash
export PREFIX="$HOME/opt/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

mkdir -p $HOME/src
cd $HOME/src

# binutils
wget https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.xz
tar xf binutils-2.42.tar.xz
mkdir binutils-build && cd binutils-build
../binutils-2.42/configure --target=$TARGET --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror
make -j"$(nproc)"
make install

# gcc (C only)
cd $HOME/src
wget https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz
tar xf gcc-14.2.0.tar.xz
mkdir gcc-build && cd gcc-build
../gcc-14.2.0/configure --target=$TARGET --prefix="$PREFIX" --disable-nls --enable-languages=c --without-headers
make all-gcc -j"$(nproc)"
make all-target-libgcc -j"$(nproc)"
make install-gcc
make install-target-libgcc
```

Ensure the cross tools are on your `PATH` (e.g. in `~/.bashrc`):

```bash
export PATH="$HOME/opt/cross/bin:$PATH"
```

### Building MyOS

From WSL/Ubuntu, in this repo (e.g. `/mnt/c/Users/<you>/Documents/Projects/MyOS`):

```bash
cd /mnt/c/Users/<you>/Documents/Projects/MyOS
make
make iso
```

This produces `build/myos.elf` and `build/myos.iso`.

### Running under QEMU

Still inside WSL/Ubuntu:

```bash
make run
```

You should see a QEMU window with a text message from the kernel.


PHASE 0 — Repo Setup (Day 0)
0.1 Repository

 Create GitHub repo

 Initialize folder structure

 Add starter README

 Add Makefile skeleton

 Set up QEMU run command

 Add .gitignore

0.2 Toolchain

 Install x86_64-elf binutils

 Install x86_64-elf gcc cross compiler

 Install QEMU

 Install NASM

⭐ PHASE 1 — Bootloader & Entering Long Mode

(Goal: get the CPU into a known state and running your C kernel code.)

1.1 GRUB Bootloader + Multiboot Header

GRUB loads your kernel into memory and jumps to it — without a bootloader your CPU has no idea where your OS is.

1.2 Early stack setup

The CPU starts with no stack; you must create one immediately so any C code (which uses a stack) can function.

1.3 VGA text write (“Hello”)

A minimal visual output proves your kernel is actually executing before you move on to complex features.

1.4 Build minimal GDT (Global Descriptor Table)

Entering 64-bit mode requires a 64-bit code segment — the GDT provides this. Without it, the CPU faults.

1.5 Enable PAE + Long Mode + Paging

The CPU cannot enter 64-bit mode without paging enabled; paging cannot be enabled without PAE; so these steps must occur in strict order.

1.6 Far jump into 64-bit mode

The CPU enters long mode only on a far jump into the 64-bit code segment defined in your GDT, so this transition must happen after paging.

⭐ PHASE 2 — Memory Management

(Goal: control memory safely so the rest of the OS has a stable foundation.)

2.1 Parse physical memory map

You can’t allocate memory until you know which RAM regions are usable — this is why we gather the memory map first.

2.2 Build physical page allocator (bitmap/buddy)

Higher-level memory management depends on the ability to hand out raw physical pages, so this must come first.

2.3 Set up virtual memory (paging structures)

Without mapping memory virtually, user/kernel isolation and advanced memory features are impossible — paging is the backbone of the OS.

2.4 Implement kmalloc() (kernel heap)

Kernel data structures need dynamic memory; you cannot build drivers, processes, or anything flexible without a heap.

2.5 Higher-half kernel mapping (optional but recommended)

Mapping the kernel high in memory avoids collisions with user memory and simplifies the design of future memory isolation.

⭐ PHASE 3 — Interrupts & Basic Drivers

(Goal: let hardware talk to the OS.)

3.1 Build IDT (Interrupt Descriptor Table)

Interrupt handling is required before timers, keyboard input, system calls, and exceptions; so the IDT is foundational.

3.2 PIC/APIC initialization

Hardware IRQ numbers overlap CPU exceptions by default; remapping them prevents random crashes when enabling interrupts.

3.3 Enable timer interrupts (PIT or APIC timer)

Multitasking and scheduling depend on periodic timer signals; without a timer, you cannot switch processes deterministically.

3.4 Keyboard driver

A simple driver proves interrupts, IRQ routing, and device I/O are all working before moving into more advanced kernel features.

⭐ PHASE 4 — System Calls & User/Kernel Boundary

(Goal: safely switch between unprivileged user code and privileged kernel code.)

4.1 Set up user mode (ring 3 segments + TSS)

You must configure CPU privilege levels before running user programs; otherwise user code could crash or control the CPU.

4.2 Implement syscall entry point (int 0x80 or syscall)

This is the controlled “doorway” between user programs and the kernel; it must exist before any user program can do I/O or allocation.

4.3 Create syscall table (write, exit, sleep, etc.)

Each kernel service needs an ID and handler — without a syscall table, user programs have no abilities and cannot interact with hardware.

4.4 Return-to-user with iretq

You must verify safe transitions back to unprivileged mode before you can run any user-space program.

⭐ PHASE 5 — Processes & Scheduling

(Goal: run multiple programs, switching between them safely.)

5.1 Define process_t structure

You need a structure to represent each process (registers, memory space, PID) before you can switch between them or create new ones.

5.2 Build context switch mechanism

The scheduler can’t function until you define how to save and restore CPU registers and stacks; context switch is the prerequisite.

5.3 Implement Round-Robin scheduler

With context switching working, the scheduler decides which process runs next; this is the smallest viable scheduler.

5.4 Implement process creation (spawn() or fork())

A scheduler is useless without processes; so once scheduling is stable, you can create new user programs.

5.5 Test two processes running alternately

Successful alternating output verifies timer interrupts, kernel stack switching, and CR3 switching all work correctly together.

⭐ PHASE 6 — ELF Loading (Running Real Programs)

(Goal: execute compiled C user programs.)

6.1 Parse ELF file header

You must know where executable code and data live before you can load them into memory.

6.2 Map ELF segments into new process address space

The process must have correct memory layout or execution will fault instantly; mapping must occur before setting RIP.

6.3 Set up user stack

User mode cannot run without a valid stack pointer; this must be created before returning to user mode.

6.4 Switch to user mode and jump to entry point

This is the moment the OS becomes “real”: executing actual user binaries.

⭐ PHASE 7 — File System

(Goal: persistent storage and user programs you can load by name.)

7.1 Create simple RAMFS or TarFS

Before implementing a disk filesystem, an in-memory filesystem gives you file abstraction quickly and safely.

7.2 Implement open, read, write, close syscalls

User programs need these to access any files; without these syscalls, the filesystem is unusable.

7.3 Add path resolution logic

Commands like ls depend on navigating directories and paths like /bin/app.

⭐ PHASE 8 — Shell & Userland

(Goal: provide a user interface to run programs on your OS.)

8.1 Build a simple shell

The shell is the simplest interactive test for syscalls, the filesystem, ELF loading, and process creation.

8.2 Add basic utilities (ls, cat, echo, ps)

These confirm multiple kernel components (FS, syscalls, scheduler) work under real-world usage.

8.3 Add error handling

After userland is running, you need robust kernel panics and error paths so debugging becomes manageable.

⭐ PHASE 9 — (Optional) Advanced Kernel Features

(Goal: improve capabilities once OS is stable.)

9.1 Threads & synchronization primitives

Once multiple user processes work, you can support multiple threads within a process.

9.2 Virtual memory improvements (mmap, shared memory)

Enables modern applications, dynamic libraries, and advanced IPC.

9.3 Graphics / framebuffer mode

Move beyond VGA to a graphical OS.

9.4 Networking stack

The last major subsystem; requires stable memory, interrupts, and userland tools first.

Milestone:
✔ Boot OS → greet → type commands → run user programs.

PHASE 9 — Advanced Features (Optional but impressive)
Threads

 Thread creation

 Mutexes

 Condition variables

Memory

 mmap

 shared memory

Drivers

 Framebuffer graphics

 Mouse

 Storage driver

Networking

 RTL8139 driver

 Minimal TCP/IP stack

PHASE 10 — Documentation + GitHub Polish (Final Week)
10.1 Documentation

 Memory map diagram

 Boot sequence diagram

 ISR flow diagram

 Scheduler timeline

 Syscall table

 VFS diagram

10.2 GitHub polish

 Add build badges

 Add GIFs (screen recordings)

 Complete README

 Add CONTRIBUTING.md

 Add architecture overview in docs/

 Tag releases (v0.1, v1.0, etc.)