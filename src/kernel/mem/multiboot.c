#include "multiboot.h"

#define VGA_MEMORY 0xB8000

static void debug_char(int pos, char c, uint8_t color) {
    volatile uint16_t *video = (uint16_t *)VGA_MEMORY;
    video[pos] = (color << 8) | c;
}

void multiboot_parse(struct multiboot_info *mb_info, struct memory_map *out)
{
    debug_char(160, 'A', 0x0E); // Yellow A - entered function
    
    out->region_count = 0;

    debug_char(161, 'B', 0x0E); // Yellow B - initialized region_count
    
    uint8_t *ptr = (uint8_t *)mb_info + sizeof(struct multiboot_info);
    uint8_t *end = (uint8_t *)mb_info + mb_info->total_size;

    debug_char(162, 'C', 0x0E); // Yellow C - calculated pointers
    
    while (ptr < end)
    {
        debug_char(163, 'D', 0x0E); // Yellow D - in loop
        
        struct multiboot_tag *tag = (struct multiboot_tag *)ptr;
        if (tag->type == 0)
            break;

        debug_char(164, 'E', 0x0E); // Yellow E - checked type
        
        if (tag->type == 6) /* Multiboot2 memory map tag */
        {
            debug_char(165, 'F', 0x0E); // Yellow F - found mmap tag
            
            struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)tag;
            uint8_t *entry_ptr = (uint8_t *)(mmap_tag + 1);
            uint8_t *mmap_end = (uint8_t *)tag + tag->size;

            while (entry_ptr + mmap_tag->entry_size <= mmap_end && out->region_count < MAX_MEMORY_REGIONS)
            {
                debug_char(166, 'G', 0x0E); // Yellow G - in entry loop
                
                struct multiboot_mmap_entry *entry = (struct multiboot_mmap_entry *)entry_ptr;

                out->regions[out->region_count].base = entry->addr;
                out->regions[out->region_count].length = entry->len;
                out->regions[out->region_count].type = entry->type;
                out->region_count++;

                entry_ptr += mmap_tag->entry_size;
            }
        }

        debug_char(167, 'H', 0x0E); // Yellow H - about to advance ptr
        
        /* tags are 8-byte aligned */
        ptr += (tag->size + 7) & ~7U;
    }
    
    debug_char(168, 'I', 0x0E); // Yellow I - done
}
