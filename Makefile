TARGET     := myos
ARCH       := x86_64
CROSS_COMPILE ?= x86_64-elf-

CC         := $(CROSS_COMPILE)gcc
LD         := $(CROSS_COMPILE)ld
AS         := $(CROSS_COMPILE)gcc

ISO_DIR    := build/iso
BOOT_DIR   := $(ISO_DIR)/boot
KERNEL_ELF := build/$(TARGET).elf
ISO_IMAGE  := build/$(TARGET).iso

CFLAGS     := -ffreestanding -O2 -Wall -Wextra -m64 -std=gnu11 -fno-pic -fno-pie -fno-asynchronous-unwind-tables -mcmodel=kernel -mno-red-zone -mgeneral-regs-only
LDFLAGS    := -nostdlib -z max-page-size=0x1000 -m elf_x86_64

# User-space build flags (ring-3, no kernel restrictions)
USER_CFLAGS  := -ffreestanding -O2 -Wall -m64 -std=gnu11 -fno-pic -fno-pie \
                -fno-asynchronous-unwind-tables -mno-red-zone -mcmodel=large -I.
USER_LDFLAGS := -nostdlib -z max-page-size=0x1000 -m elf_x86_64

USER_HELLO_ELF := build/user/hello.elf
USER_BENCH_CPU_ELF := build/user/bench_cpu.elf
USER_BENCH_IO_ELF  := build/user/bench_io.elf
USER_BENCH_MIX_ELF := build/user/bench_mix.elf
USER_DEMAND_ELF    := build/user/demand_test.elf
USER_EVICT_ELF     := build/user/evict_test.elf
USER_BENCH_PF_ELF   := build/user/bench_pf.elf
IO_TEST_BIN        := build/io_test.bin
IO_TEST2_BIN       := build/io_test2.bin
USER_BENCH_FILES_ELF := build/user/bench_files.elf
USER_WTEST_ELF     := build/user/wtest.elf
USER_WTESTW_ELF    := build/user/wtestw.elf
USER_WTESTV_ELF    := build/user/wtestv.elf
USER_ARGTEST_ELF   := build/user/argtest.elf
USER_FDTEST_ELF    := build/user/fdtest.elf

SRC_DIR    := src
# First collect all C files
KERNEL_SRC_ALL := $(wildcard $(SRC_DIR)/kernel/*.c $(SRC_DIR)/kernel/arch/*.c $(SRC_DIR)/kernel/mem/*.c $(SRC_DIR)/kernel/proc/*.c $(SRC_DIR)/kernel/sys/*.c $(SRC_DIR)/kernel/drivers/*.c $(SRC_DIR)/kernel/fs/*.c)
# Then exclude isr80.c if it exists (we use isr80.S instead)
KERNEL_SRC := $(filter-out %/isr80.c, $(KERNEL_SRC_ALL))
KERNEL_ASM := $(SRC_DIR)/kernel/boot.S $(SRC_DIR)/kernel/arch/idt_load.S $(SRC_DIR)/kernel/arch/gdt_load.S $(SRC_DIR)/kernel/arch/irq_stubs.S $(SRC_DIR)/kernel/arch/exc_stubs.S $(SRC_DIR)/kernel/arch/isr80.S $(SRC_DIR)/kernel/proc/context_switch.S
KERNEL_OBJ := $(KERNEL_SRC:.c=.o) $(KERNEL_ASM:.S=.o)

.PHONY: all clean run iso

all: $(KERNEL_ELF)

# User programs
$(USER_HELLO_ELF): user/hello.c src/lib/syscall.S src/lib/malloc.c user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c user/hello.c       -o build/user/hello.o
	$(CC) $(USER_CFLAGS) -c src/lib/syscall.S  -o build/user/syscall_u.o
	$(CC) $(USER_CFLAGS) -c src/lib/malloc.c   -o build/user/malloc_u.o
	$(LD) $(USER_LDFLAGS) -T user/user.ld -o $@ build/user/hello.o build/user/syscall_u.o build/user/malloc_u.o

$(USER_BENCH_CPU_ELF): user/bench_cpu.c src/lib/syscall.S src/lib/malloc.c user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c user/bench_cpu.c  -o build/user/bench_cpu.o
	$(CC) $(USER_CFLAGS) -c src/lib/syscall.S -o build/user/syscall_u.o
	$(CC) $(USER_CFLAGS) -c src/lib/malloc.c  -o build/user/malloc_u.o
	$(LD) $(USER_LDFLAGS) -T user/user.ld -o $@ build/user/bench_cpu.o build/user/syscall_u.o build/user/malloc_u.o

$(USER_BENCH_IO_ELF): user/bench_io.c src/lib/syscall.S src/lib/malloc.c user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c user/bench_io.c   -o build/user/bench_io.o
	$(CC) $(USER_CFLAGS) -c src/lib/syscall.S -o build/user/syscall_u.o
	$(CC) $(USER_CFLAGS) -c src/lib/malloc.c  -o build/user/malloc_u.o
	$(LD) $(USER_LDFLAGS) -T user/user.ld -o $@ build/user/bench_io.o build/user/syscall_u.o build/user/malloc_u.o

$(USER_ARGTEST_ELF): user/argtest.c src/lib/syscall.S src/lib/malloc.c user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c user/argtest.c  -o build/user/argtest.o
	$(CC) $(USER_CFLAGS) -c src/lib/syscall.S -o build/user/syscall_u.o
	$(CC) $(USER_CFLAGS) -c src/lib/malloc.c  -o build/user/malloc_u.o
	$(LD) $(USER_LDFLAGS) -T user/user.ld -o $@ build/user/argtest.o build/user/syscall_u.o build/user/malloc_u.o

$(USER_FDTEST_ELF): user/fdtest.c src/lib/syscall.S src/lib/malloc.c user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c user/fdtest.c  -o build/user/fdtest.o
	$(CC) $(USER_CFLAGS) -c src/lib/syscall.S -o build/user/syscall_u.o
	$(CC) $(USER_CFLAGS) -c src/lib/malloc.c  -o build/user/malloc_u.o
	$(LD) $(USER_LDFLAGS) -T user/user.ld -o $@ build/user/fdtest.o build/user/syscall_u.o build/user/malloc_u.o

$(USER_WTEST_ELF): user/wtest.c src/lib/syscall.S src/lib/malloc.c user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c user/wtest.c   -o build/user/wtest.o
	$(CC) $(USER_CFLAGS) -c src/lib/syscall.S -o build/user/syscall_u.o
	$(CC) $(USER_CFLAGS) -c src/lib/malloc.c  -o build/user/malloc_u.o
	$(LD) $(USER_LDFLAGS) -T user/user.ld -o $@ build/user/wtest.o build/user/syscall_u.o build/user/malloc_u.o

$(USER_WTESTW_ELF): user/wtest.c src/lib/syscall.S src/lib/malloc.c user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -DWTEST_MODE=1 -c user/wtest.c -o build/user/wtestw.o
	$(CC) $(USER_CFLAGS) -c src/lib/syscall.S -o build/user/syscall_u.o
	$(CC) $(USER_CFLAGS) -c src/lib/malloc.c  -o build/user/malloc_u.o
	$(LD) $(USER_LDFLAGS) -T user/user.ld -o $@ build/user/wtestw.o build/user/syscall_u.o build/user/malloc_u.o

$(USER_WTESTV_ELF): user/wtest.c src/lib/syscall.S src/lib/malloc.c user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -DWTEST_MODE=2 -c user/wtest.c -o build/user/wtestv.o
	$(CC) $(USER_CFLAGS) -c src/lib/syscall.S -o build/user/syscall_u.o
	$(CC) $(USER_CFLAGS) -c src/lib/malloc.c  -o build/user/malloc_u.o
	$(LD) $(USER_LDFLAGS) -T user/user.ld -o $@ build/user/wtestv.o build/user/syscall_u.o build/user/malloc_u.o

$(USER_BENCH_MIX_ELF): user/bench_mix.c src/lib/syscall.S src/lib/malloc.c user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c user/bench_mix.c  -o build/user/bench_mix.o
	$(CC) $(USER_CFLAGS) -c src/lib/syscall.S -o build/user/syscall_u.o
	$(CC) $(USER_CFLAGS) -c src/lib/malloc.c  -o build/user/malloc_u.o
	$(LD) $(USER_LDFLAGS) -T user/user.ld -o $@ build/user/bench_mix.o build/user/syscall_u.o build/user/malloc_u.o

$(USER_DEMAND_ELF): user/demand_test.c src/lib/syscall.S src/lib/malloc.c user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c user/demand_test.c -o build/user/demand_test.o
	$(CC) $(USER_CFLAGS) -c src/lib/syscall.S -o build/user/syscall_u.o
	$(CC) $(USER_CFLAGS) -c src/lib/malloc.c   -o build/user/malloc_u.o
	$(LD) $(USER_LDFLAGS) -T user/user.ld -o $@ build/user/demand_test.o build/user/syscall_u.o build/user/malloc_u.o

$(USER_EVICT_ELF): user/evict_test.c src/lib/syscall.S src/lib/malloc.c user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c user/evict_test.c  -o build/user/evict_test.o
	$(CC) $(USER_CFLAGS) -c src/lib/syscall.S -o build/user/syscall_u.o
	$(CC) $(USER_CFLAGS) -c src/lib/malloc.c   -o build/user/malloc_u.o
	$(LD) $(USER_LDFLAGS) -T user/user.ld -o $@ build/user/evict_test.o build/user/syscall_u.o build/user/malloc_u.o

$(USER_BENCH_PF_ELF): user/bench_pf.c src/lib/syscall.S src/lib/malloc.c user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c user/bench_pf.c    -o build/user/bench_pf.o
	$(CC) $(USER_CFLAGS) -c src/lib/syscall.S -o build/user/syscall_u.o
	$(CC) $(USER_CFLAGS) -c src/lib/malloc.c   -o build/user/malloc_u.o
	$(LD) $(USER_LDFLAGS) -T user/user.ld -o $@ build/user/bench_pf.o build/user/syscall_u.o build/user/malloc_u.o

$(USER_BENCH_FILES_ELF): user/bench_files.c src/lib/syscall.S src/lib/malloc.c user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c user/bench_files.c -o build/user/bench_files.o
	$(CC) $(USER_CFLAGS) -c src/lib/syscall.S -o build/user/syscall_u.o
	$(CC) $(USER_CFLAGS) -c src/lib/malloc.c  -o build/user/malloc_u.o
	$(LD) $(USER_LDFLAGS) -T user/user.ld -o $@ build/user/bench_files.o build/user/syscall_u.o build/user/malloc_u.o

$(IO_TEST2_BIN): scripts/gen_io_test.py
	@mkdir -p $(dir $@)
	python3 scripts/gen_io_test.py $(IO_TEST2_BIN) 0x87654321

$(IO_TEST_BIN): scripts/gen_io_test.py
	@mkdir -p $(dir $@)
	python3 scripts/gen_io_test.py $(IO_TEST_BIN)

$(KERNEL_ELF): $(KERNEL_OBJ) $(USER_HELLO_ELF)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T boot/linker.ld -o $@ $(KERNEL_OBJ) \
		--format=binary $(USER_HELLO_ELF) --format=default

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Header dependencies for kernel objects (generated by -MMD above).
# Without these, editing a header silently leaves stale .o files whose
# struct layouts disagree with the rest of the kernel.
-include $(KERNEL_SRC:.c=.d)

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

DISK_IMAGE  := build/disk.img
DISK_FILES_DIR := disk_files

iso: $(KERNEL_ELF) disk
	@mkdir -p $(BOOT_DIR)/grub
	cp $(KERNEL_ELF) $(BOOT_DIR)/kernel.elf
	cp boot/grub.cfg $(BOOT_DIR)/grub/grub.cfg
	grub-mkrescue -d /usr/lib/grub/i386-pc -o $(ISO_IMAGE) $(ISO_DIR)

# Build the MyFS disk image from all files in disk_files/
disk: $(USER_BENCH_CPU_ELF) $(USER_BENCH_IO_ELF) $(USER_BENCH_MIX_ELF) $(USER_DEMAND_ELF) $(USER_EVICT_ELF) $(USER_BENCH_PF_ELF) $(IO_TEST_BIN) $(IO_TEST2_BIN) $(USER_BENCH_FILES_ELF) $(USER_WTEST_ELF) $(USER_WTESTW_ELF) $(USER_WTESTV_ELF) $(USER_ARGTEST_ELF) $(USER_FDTEST_ELF)
	@mkdir -p $(dir $(DISK_IMAGE))
	@if [ -d $(DISK_FILES_DIR) ] && [ "$$(ls -A $(DISK_FILES_DIR) 2>/dev/null)" ]; then \
		python3 scripts/mkfs_myfs.py $(DISK_IMAGE) $(DISK_FILES_DIR)/* $(USER_BENCH_CPU_ELF) $(USER_BENCH_IO_ELF) $(USER_BENCH_MIX_ELF) $(USER_DEMAND_ELF) $(USER_EVICT_ELF) $(USER_BENCH_PF_ELF) $(IO_TEST_BIN) $(IO_TEST2_BIN) $(USER_BENCH_FILES_ELF) $(USER_WTEST_ELF) $(USER_WTESTW_ELF) $(USER_WTESTV_ELF) $(USER_ARGTEST_ELF) $(USER_FDTEST_ELF); \
	else \
		python3 scripts/mkfs_myfs.py $(DISK_IMAGE) $(USER_BENCH_CPU_ELF) $(USER_BENCH_IO_ELF) $(USER_BENCH_MIX_ELF) $(USER_DEMAND_ELF) $(USER_EVICT_ELF) $(USER_BENCH_PF_ELF) $(IO_TEST_BIN) $(IO_TEST2_BIN) $(USER_BENCH_FILES_ELF) $(USER_WTEST_ELF) $(USER_WTESTW_ELF) $(USER_WTESTV_ELF) $(USER_ARGTEST_ELF) $(USER_FDTEST_ELF); \
	fi

run: iso
	qemu-system-$(ARCH) -cdrom $(ISO_IMAGE) -drive file=$(DISK_IMAGE),format=raw,if=ide,index=0 -boot d -m 512M -serial stdio -no-reboot -no-shutdown

run-hd: iso
	qemu-system-$(ARCH) -hda $(ISO_IMAGE) -m 512M

clean:
	rm -rf build
	find src -name '*.o' -delete
	find src -name '*.d' -delete
