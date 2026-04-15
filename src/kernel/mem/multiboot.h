#pragma once

#include <stdint.h>

struct memory_region {
    uint64_t base;
    uint64_t length;
    uint32_t type; /* 1 = usable, others reserved */
};

#define MAX_MEMORY_REGIONS 32

struct memory_map {
    struct memory_region regions[MAX_MEMORY_REGIONS];
    uint32_t region_count;
};

/*
 * Multiboot1 info structure.
 * GRUB places this in memory and passes its physical address in EBX.
 * All pointer fields are 32-bit physical addresses (identity-mapped).
 */
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;      /* KB below 1MB   */
    uint32_t mem_upper;      /* KB above 1MB   */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;    /* byte length of mmap buffer */
    uint32_t mmap_addr;      /* physical address of first mmap entry */
} __attribute__((packed));

void multiboot_parse(struct multiboot_info *mb_info, struct memory_map *out);
