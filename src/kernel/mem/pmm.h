#pragma once

#include <stdint.h>
#include "multiboot.h"

void pmm_init(struct memory_map *memmap);
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t addr);
