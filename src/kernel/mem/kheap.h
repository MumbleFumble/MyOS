#pragma once

#include <stdint.h>
#include "multiboot.h"

void kheap_init(struct memory_map *memmap);
void *kmalloc(uint64_t size);
void kfree(void *ptr);
