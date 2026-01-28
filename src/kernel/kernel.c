#include <stdint.h>
#include "arch/idt.h"
#include "arch/irq.h"
#include "arch/timer.h"
#include "mem/multiboot.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "mem/kheap.h"

#define VGA_MEMORY 0xB8000
#define VGA_COLS   80
#define VGA_ROWS   25

static void clear_screen(uint8_t color) {
    volatile uint16_t* vga = (uint16_t*) VGA_MEMORY;
    uint16_t blank = ((uint16_t)color << 8) | ' ';
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        vga[i] = blank;
    }
}

static void print_hex64(volatile uint16_t *video, int offset, uint64_t value) {
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (value >> (i * 4)) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        video[offset + (15 - i)] = (0x0F << 8) | c;
    }
}

__attribute__((noreturn))
void kernel_main(struct multiboot_info *mb_info)
{
    volatile uint16_t *video = (uint16_t *)0xB8000;
    
    // Don't clear screen yet - just overwrite BIOS text
    const char *msg = "MyOS 64-bit | MB ptr=";
    int pos = 0;
    for (int i = 0; msg[i] != '\0'; ++i, ++pos) {
        video[pos] = (0x0F << 8) | msg[i];
    }
    
    /* Print multiboot pointer value */
    print_hex64(video, pos, (uint64_t)mb_info);
    pos += 16;
    
    /* Check if pointer looks valid (should be < 16MB based on our identity map) */
    video[pos++] = (0x0F << 8) | ' ';
    if ((uint64_t)mb_info < 0x1000000) {
        const char *status = "OK";
        for (int i = 0; status[i] != '\0'; ++i, ++pos) {
            video[pos] = (0x0A << 8) | status[i];  /* Green */
        }
    } else {
        const char *status = "OUT_OF_RANGE";
        for (int i = 0; status[i] != '\0'; ++i, ++pos) {
            video[pos] = (0x0C << 8) | status[i];  /* Red */
        }
    }

    idt_init();
    irq_init();
    timer_init();

    // Test extended identity mapping first
    video[VGA_COLS+13] = (0x0A << 8) | 'I';
    video[VGA_COLS+14] = (0x0A << 8) | 'd';
    video[VGA_COLS+15] = (0x0A << 8) | 'e';
    video[VGA_COLS+16] = (0x0A << 8) | 'n';
    video[VGA_COLS+17] = (0x0A << 8) | 't';
    video[VGA_COLS+18] = (0x0A << 8) | 'i';
    video[VGA_COLS+19] = (0x0A << 8) | 't';
    video[VGA_COLS+20] = (0x0A << 8) | 'y';
    video[VGA_COLS+21] = (0x0A << 8) | ' ';
    video[VGA_COLS+22] = (0x0A << 8) | 'm';
    video[VGA_COLS+23] = (0x0A << 8) | 'a';
    video[VGA_COLS+24] = (0x0A << 8) | 'p';
    video[VGA_COLS+25] = (0x0A << 8) | ' ';
    video[VGA_COLS+26] = (0x0A << 8) | '1';
    video[VGA_COLS+27] = (0x0A << 8) | '6';
    video[VGA_COLS+28] = (0x0A << 8) | 'M';
    video[VGA_COLS+29] = (0x0A << 8) | 'B';
    
    // Parse multiboot info
    struct memory_map memmap;
    multiboot_parse(mb_info, &memmap);
    
    video[VGA_COLS+18] = (0x0F << 8) | 'r';
    video[VGA_COLS+19] = (0x0F << 8) | 'e';
    video[VGA_COLS+20] = (0x0F << 8) | 'g';
    video[VGA_COLS+21] = (0x0F << 8) | 'i';
    video[VGA_COLS+22] = (0x0F << 8) | 'o';
    video[VGA_COLS+23] = (0x0F << 8) | 'n';
    video[VGA_COLS+24] = (0x0F << 8) | 's';
    video[VGA_COLS+25] = (0x0F << 8) | '=';
    char digit = '0' + (memmap.region_count % 10);
    video[VGA_COLS+26] = (0x0A << 8) | digit;
    
    // Memory management disabled for now - causes crash
    // pmm_init(&memmap);
    // vmm_init();
    // kheap_init(&memmap);

    // Display timer ticks to show interrupts are working
    video[VGA_COLS*2] = (0x0F << 8) | 'T';
    video[VGA_COLS*2+1] = (0x0F << 8) | 'i';
    video[VGA_COLS*2+2] = (0x0F << 8) | 'm';
    video[VGA_COLS*2+3] = (0x0F << 8) | 'e';
    video[VGA_COLS*2+4] = (0x0F << 8) | 'r';
    video[VGA_COLS*2+5] = (0x0F << 8) | ':';
    video[VGA_COLS*2+6] = (0x0F << 8) | ' ';

    for (;;)
    {
        uint32_t ticks = timer_ticks();
        
        // Display tick count in hex (8 digits)
        for (int i = 7; i >= 0; i--) {
            uint8_t nibble = (ticks >> (i * 4)) & 0xF;
            char c = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
            video[VGA_COLS*2 + 7 + (7-i)] = (0x0A << 8) | c;
        }
        
        __asm__ volatile("hlt");
    }
}
