#include "pmm.h"

#define PAGE_SIZE 4096
#define MAX_PHYS_MEM (256 * 1024 * 1024) /* 256 MiB cap for now */
#define MAX_FRAMES (MAX_PHYS_MEM / PAGE_SIZE)

static uint32_t bitmap[MAX_FRAMES / 32];
static uint32_t total_frames;

static void bitmap_set(uint32_t frame)
{
    bitmap[frame / 32] |= (1u << (frame % 32));
}

static void bitmap_clear(uint32_t frame)
{
    bitmap[frame / 32] &= ~(1u << (frame % 32));
}

static int bitmap_test(uint32_t frame)
{
    return (bitmap[frame / 32] >> (frame % 32)) & 1u;
}

void pmm_init(struct memory_map *memmap)
{
    for (uint32_t i = 0; i < MAX_FRAMES / 32; ++i)
        bitmap[i] = 0xFFFFFFFFu;

    uint64_t max_addr = 0;
    for (uint32_t i = 0; i < memmap->region_count; ++i)
    {
        if (memmap->regions[i].type == 1)
        {
            uint64_t end = memmap->regions[i].base + memmap->regions[i].length;
            if (end > max_addr)
                max_addr = end;
        }
    }

    if (max_addr > MAX_PHYS_MEM)
        max_addr = MAX_PHYS_MEM;

    total_frames = (uint32_t)(max_addr / PAGE_SIZE);

    for (uint32_t i = 0; i < total_frames; ++i)
        bitmap_clear(i);

    for (uint32_t i = 0; i < memmap->region_count; ++i)
    {
        if (memmap->regions[i].type != 1)
        {
            uint64_t start = memmap->regions[i].base / PAGE_SIZE;
            uint64_t end = (memmap->regions[i].base + memmap->regions[i].length + PAGE_SIZE - 1) / PAGE_SIZE;
            if (end > total_frames)
                end = total_frames;
            for (uint64_t f = start; f < end; ++f)
                bitmap_set((uint32_t)f);
        }
    }

    /* Reserve first few frames (including zero page) */
    for (uint32_t f = 0; f < 16 && f < total_frames; ++f)
        bitmap_set(f);
}

uint64_t pmm_alloc_page(void)
{
    for (uint32_t f = 0; f < total_frames; ++f)
    {
        if (!bitmap_test(f))
        {
            bitmap_set(f);
            return (uint64_t)f * PAGE_SIZE;
        }
    }
    return 0; /* out of memory */
}

void pmm_free_page(uint64_t addr)
{
    uint32_t frame = (uint32_t)(addr / PAGE_SIZE);
    if (frame < total_frames)
        bitmap_clear(frame);
}
