#pragma once

#include <stdint.h>

/*
 * ELF64 header and program header structures (just what we need).
 */

#define ELF_MAGIC      0x464C457F   /* "\x7FELF" little-endian */
#define ELF_CLASS64    2
#define ELF_DATA_LE    1
#define ELF_TYPE_EXEC  2
#define ELF_MACH_X64   62
#define PT_LOAD        1

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;   /* offset in file */
    uint64_t p_vaddr;    /* virtual address to load at */
    uint64_t p_paddr;
    uint64_t p_filesz;   /* bytes in file image */
    uint64_t p_memsz;    /* bytes in memory (may be > filesz; rest = zero) */
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

/*
 * Result returned by elf_load.
 */
typedef struct {
    uint64_t cr3;       /* physical address of the new PML4 */
    uint64_t entry;     /* virtual entry point (from ELF header) */
    uint64_t ustack;    /* virtual address of top of user stack */
} elf_result_t;

/*
 * Load an ELF64 executable from a byte buffer embedded in the kernel.
 * Creates a new address space, maps all PT_LOAD segments, allocates a
 * user-mode stack at USER_STACK_TOP.
 *
 * Returns 0 on success, -1 on error.
 * On success, *out is filled.
 */
int elf_load(const uint8_t *elf_data, uint64_t elf_size, elf_result_t *out);
