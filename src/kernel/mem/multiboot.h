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

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};

struct multiboot_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};

struct multiboot_info {
    uint32_t total_size;
    uint32_t reserved;
};

void multiboot_parse(struct multiboot_info *mb_info, struct memory_map *out);
