#include <stdint.h>
#include "arch/idt.h"
#include "arch/irq.h"
#include "arch/timer.h"
#include "arch/port_io.h"
#include "mem/multiboot.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "mem/kheap.h"

/* COM1 serial debug output - survives triple faults, readable in QEMU -serial stdio */
static void serial_init(void)
{
    outb(0x3F8 + 1, 0x00); /* disable interrupts */
    outb(0x3F8 + 3, 0x80); /* enable DLAB */
    outb(0x3F8 + 0, 0x03); /* 38400 baud low */
    outb(0x3F8 + 1, 0x00); /* 38400 baud high */
    outb(0x3F8 + 3, 0x03); /* 8N1 */
    outb(0x3F8 + 2, 0xC7); /* FIFO on */
    outb(0x3F8 + 4, 0x0B); /* RTS/DSR */
}

static void serial_putchar(char c)
{
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, (uint8_t)c);
}

static void serial_puts(const char *s)
{
    while (*s) serial_putchar(*s++);
}

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

    serial_init();
    serial_puts("[serial] kernel_main entered\r\n");

    idt_init();
    serial_puts("[serial] idt_init done\r\n");
    irq_init();
    serial_puts("[serial] irq_init done\r\n");
    timer_init();
    serial_puts("[serial] timer_init done\r\n");

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
    
    // Memory management init - with per-step progress on row 4
    volatile uint16_t *mrow = video + VGA_COLS * 4;
    mrow[0] = (0x0E << 8) | 'M'; // yellow M = starting mem init
    serial_puts("[serial] pmm_init start\r\n");

    pmm_init(&memmap);
    mrow[1] = (0x0A << 8) | 'P'; // green P = pmm ok
    serial_puts("[serial] pmm_init done\r\n");

    vmm_init();
    mrow[2] = (0x0A << 8) | 'V'; // green V = vmm ok
    serial_puts("[serial] vmm_init done\r\n");

    kheap_init(&memmap);
    mrow[3] = (0x0A << 8) | 'H'; // green H = heap ok
    serial_puts("[serial] kheap_init done\r\n");

    void *heap_test = kmalloc(64);
    mrow[4] = heap_test ? ((0x0A << 8) | 'K') : ((0x0C << 8) | 'X'); // green K = kmalloc ok, red X = fail
    serial_puts(heap_test ? "[serial] kmalloc ok\r\n" : "[serial] kmalloc FAILED\r\n");
    serial_puts("[serial] entering timer loop\r\n");

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
