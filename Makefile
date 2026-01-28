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

CFLAGS     := -ffreestanding -O2 -Wall -Wextra -m64 -std=gnu11 -fno-pic -fno-pie -fno-asynchronous-unwind-tables -mcmodel=kernel -mno-red-zone
LDFLAGS    := -nostdlib -z max-page-size=0x1000 -m elf_x86_64

SRC_DIR    := src
# First collect all C files
KERNEL_SRC_ALL := $(wildcard $(SRC_DIR)/kernel/*.c $(SRC_DIR)/kernel/arch/*.c $(SRC_DIR)/kernel/mem/*.c)
# Then exclude isr80.c if it exists (we use isr80.S instead)
KERNEL_SRC := $(filter-out %/isr80.c, $(KERNEL_SRC_ALL))
KERNEL_ASM := $(SRC_DIR)/kernel/boot.S $(SRC_DIR)/kernel/arch/idt_load.S $(SRC_DIR)/kernel/arch/irq_stubs.S $(SRC_DIR)/kernel/arch/isr80.S
KERNEL_OBJ := $(KERNEL_SRC:.c=.o) $(KERNEL_ASM:.S=.o)

.PHONY: all clean run iso

all: $(KERNEL_ELF)

$(KERNEL_ELF): $(KERNEL_OBJ)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T boot/linker.ld -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

iso: $(KERNEL_ELF)
	@mkdir -p $(BOOT_DIR)/grub
	cp $(KERNEL_ELF) $(BOOT_DIR)/kernel.elf
	cp boot/grub.cfg $(BOOT_DIR)/grub/grub.cfg
	grub-mkrescue -d /usr/lib/grub/i386-pc -o $(ISO_IMAGE) $(ISO_DIR) 2>&1 | grep -v "xorriso"

run: iso
	qemu-system-$(ARCH) -cdrom $(ISO_IMAGE) -boot d -m 512M

run-hd: iso
	qemu-system-$(ARCH) -hda $(ISO_IMAGE) -m 512M

clean:
	rm -rf build
	find src -name '*.o' -delete
