#pragma once

#include <stdint.h>

#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000UL

#define PAGE_SIZE 4096UL

#define PAGE_PRESENT 0x001UL
#define PAGE_RW      0x002UL
#define PAGE_USER    0x004UL

void vmm_init(void);

void vmm_map_page(uint64_t phys, uint64_t virt, uint64_t flags);
void vmm_unmap_page(uint64_t virt);
