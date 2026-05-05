#include "vga.h"
#include "../arch/port_io.h"

#define VGA_BASE    0xB8000
#define VGA_COLS    80
#define VGA_ROWS    25

/* Hardware cursor I/O ports */
#define VGA_CTRL    0x3D4
#define VGA_DATA    0x3D5

static volatile uint16_t *const vga_buf = (uint16_t *)VGA_BASE;
static int cur_row  = 0;
static int cur_col  = 0;
static uint8_t cur_attr = (VGA_BLACK << 4) | VGA_LIGHT_GREY; /* light-grey on black */

/* -----------------------------------------------------------------------
 * Hardware cursor
 * ----------------------------------------------------------------------- */
static void hw_cursor_update(void)
{
    uint16_t pos = (uint16_t)(cur_row * VGA_COLS + cur_col);
    outb(VGA_CTRL, 14);
    outb(VGA_DATA, (uint8_t)(pos >> 8));
    outb(VGA_CTRL, 15);
    outb(VGA_DATA, (uint8_t)(pos & 0xFF));
}

/* Enable blinking underline cursor (scan lines 13-15) */
static void hw_cursor_enable(void)
{
    outb(VGA_CTRL, 0x0A);
    outb(VGA_DATA, (inb(VGA_DATA) & 0xC0) | 13);
    outb(VGA_CTRL, 0x0B);
    outb(VGA_DATA, (inb(VGA_DATA) & 0xE0) | 15);
}

/* -----------------------------------------------------------------------
 * Scroll: move every row up by one, blank the last row.
 * ----------------------------------------------------------------------- */
static void scroll(void)
{
    for (int r = 0; r < VGA_ROWS - 1; r++)
        for (int c = 0; c < VGA_COLS; c++)
            vga_buf[r * VGA_COLS + c] = vga_buf[(r + 1) * VGA_COLS + c];

    uint16_t blank = ((uint16_t)cur_attr << 8) | ' ';
    for (int c = 0; c < VGA_COLS; c++)
        vga_buf[(VGA_ROWS - 1) * VGA_COLS + c] = blank;

    cur_row = VGA_ROWS - 1;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void vga_init(void)
{
    hw_cursor_enable();
    vga_clear();
}

void vga_clear(void)
{
    uint16_t blank = ((uint16_t)cur_attr << 8) | ' ';
    for (int i = 0; i < VGA_ROWS * VGA_COLS; i++)
        vga_buf[i] = blank;
    cur_row = 0;
    cur_col = 0;
    hw_cursor_update();
}

void vga_set_color(uint8_t fg, uint8_t bg)
{
    cur_attr = (uint8_t)((bg << 4) | (fg & 0x0F));
}

void vga_putchar(char c)
{
    if (c == '\n') {
        cur_col = 0;
        cur_row++;
    } else if (c == '\r') {
        cur_col = 0;
    } else if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            vga_buf[cur_row * VGA_COLS + cur_col] = ((uint16_t)cur_attr << 8) | ' ';
        }
    } else if (c == '\t') {
        /* Advance to next 8-column tab stop */
        cur_col = (cur_col + 8) & ~7;
        if (cur_col >= VGA_COLS) {
            cur_col = 0;
            cur_row++;
        }
    } else {
        vga_buf[cur_row * VGA_COLS + cur_col] = ((uint16_t)cur_attr << 8) | (uint8_t)c;
        cur_col++;
        if (cur_col >= VGA_COLS) {
            cur_col = 0;
            cur_row++;
        }
    }

    if (cur_row >= VGA_ROWS)
        scroll();

    hw_cursor_update();
}

void vga_puts(const char *s)
{
    while (*s)
        vga_putchar(*s++);
}

void vga_write(const char *buf, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++)
        vga_putchar(buf[i]);
}

void vga_get_cursor(int *row, int *col)
{
    if (row) *row = cur_row;
    if (col) *col = cur_col;
}
