#include "multiboot.h"

/*
 * Multiboot1 mmap entry.
 * The 4-byte `size` field at the start of each entry gives the byte count
 * of the data that follows it (NOT including the `size` field itself).
 * A standard E820 entry has size=20: base(8) + length(8) + type(4).
 */
struct mb1_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;           /* 1 = available RAM */
} __attribute__((packed));

void multiboot_parse(struct multiboot_info *mb, struct memory_map *out)
{
    out->region_count = 0;

    /* Bit 6 of flags: memory map is present */
    if (!(mb->flags & (1u << 6)))
        return;

    uint8_t *ptr = (uint8_t *)(uint64_t)mb->mmap_addr;
    uint8_t *end = ptr + mb->mmap_length;

    while (ptr < end && out->region_count < MAX_MEMORY_REGIONS) {
        /* First 4 bytes = entry data size (does not count itself) */
        uint32_t entry_size = *(uint32_t *)ptr;
        struct mb1_mmap_entry *e = (struct mb1_mmap_entry *)(ptr + 4);

        out->regions[out->region_count].base   = e->base_addr;
        out->regions[out->region_count].length = e->length;
        out->regions[out->region_count].type   = e->type;
        out->region_count++;

        ptr += entry_size + 4;  /* +4: advance past the size field */
    }
}
