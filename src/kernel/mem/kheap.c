#include "kheap.h"
#include "pmm.h"
#include "vmm.h"

#define HEAP_START   0x01000000UL  /* 16 MiB, within identity-mapped range */
#define HEAP_INITIAL_SIZE (4 * 1024 * 1024UL) /* 4 MiB */

static uint64_t heap_start = HEAP_START;
static uint64_t heap_end   = HEAP_START;
static uint64_t heap_limit = HEAP_START + HEAP_INITIAL_SIZE;

static uint64_t align_up(uint64_t val, uint64_t align)
{
    return (val + align - 1) & ~(align - 1);
}

static void heap_grow_to(uint64_t new_end)
{
    new_end = align_up(new_end, PAGE_SIZE);
    while (heap_end < new_end && heap_end < heap_limit)
    {
        uint64_t phys = pmm_alloc_page();
        vmm_map_page(phys, heap_end, PAGE_RW);
        heap_end += PAGE_SIZE;
    }
}

void kheap_init(struct memory_map *memmap)
{
    (void)memmap; /* unused for now; heap range is fixed */
    heap_end = heap_start;
    heap_limit = HEAP_START + HEAP_INITIAL_SIZE;
}

void *kmalloc(uint64_t size)
{
    if (size == 0)
        return 0;

    heap_start = align_up(heap_start, 8);
    uint64_t new_end = heap_start + size;

    if (new_end > heap_end)
        heap_grow_to(new_end);

    if (new_end > heap_limit)
        return 0; /* out of heap memory */

    void *ptr = (void *)heap_start;
    heap_start = new_end;
    return ptr;
}

void kfree(void *ptr)
{
    (void)ptr; /* bump allocator cannot free individual blocks yet */
}
