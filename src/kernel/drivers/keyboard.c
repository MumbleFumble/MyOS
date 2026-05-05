#include "keyboard.h"
#include "vga.h"
#include "../arch/irq.h"
#include "../arch/port_io.h"

/* -----------------------------------------------------------------------
 * US QWERTY scancode set 1 → ASCII tables
 * Index = scancode (make code).  0 = no printable character.
 * ----------------------------------------------------------------------- */
static const char sc_lower[128] = {
/*00*/  0,   0,  '1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t',
/*10*/ 'q','w','e','r','t','y','u','i','o','p','[',']','\n',  0, 'a','s',
/*20*/ 'd','f','g','h','j','k','l',';','\'','`',  0, '\\','z','x','c','v',
/*30*/ 'b','n','m',',','.','/',  0, '*',  0, ' ',  0,   0,   0,   0,   0,   0,
/*40*/   0,  0,  0,  0,  0,  0,  0, '7','8','9','-','4','5','6','+','1',
/*50*/ '2','3','0','.',  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
/*60*/   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
/*70*/   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};

static const char sc_upper[128] = {
/*00*/  0,   0,  '!','@','#','$','%','^','&','*','(',')','_','+', '\b','\t',
/*10*/ 'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',  0, 'A','S',
/*20*/ 'D','F','G','H','J','K','L',':','"','~',   0, '|', 'Z','X','C','V',
/*30*/ 'B','N','M','<','>','?',  0, '*',  0, ' ',  0,   0,   0,   0,   0,   0,
/*40*/   0,  0,  0,  0,  0,  0,  0, '7','8','9','-','4','5','6','+','1',
/*50*/ '2','3','0','.',  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
/*60*/   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
/*70*/   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};

/* -----------------------------------------------------------------------
 * Ring buffer (power-of-two size for easy masking)
 * ----------------------------------------------------------------------- */
#define KB_BUF_SIZE 256
#define KB_BUF_MASK (KB_BUF_SIZE - 1)

static volatile char kb_buf[KB_BUF_SIZE];
static volatile uint32_t kb_head = 0; /* producer (IRQ writes here) */
static volatile uint32_t kb_tail = 0; /* consumer (reads here)       */

static inline int kb_buf_full(void)  { return ((kb_head + 1) & KB_BUF_MASK) == (kb_tail & KB_BUF_MASK); }
static inline int kb_buf_empty(void) { return (kb_head & KB_BUF_MASK) == (kb_tail & KB_BUF_MASK); }

static void kb_push(char c)
{
    if (!kb_buf_full()) {
        kb_buf[kb_head & KB_BUF_MASK] = c;
        /* Ensure character is visible before advancing head */
        __asm__ volatile("" ::: "memory");
        kb_head++;
    }
}

/* -----------------------------------------------------------------------
 * IRQ1 handler
 * ----------------------------------------------------------------------- */
static int shift_held = 0;
static int caps_lock  = 0;

static void keyboard_irq_handler(void)
{
    uint8_t sc = inb(0x60); /* read scancode from PS/2 data port */

    if (sc & 0x80) {
        /* Key release — only care about shift */
        uint8_t rel = sc & 0x7F;
        if (rel == 0x2A || rel == 0x36)
            shift_held = 0;
        return;
    }

    /* Key press */
    switch (sc) {
    case 0x2A: case 0x36: shift_held = 1; return;   /* left/right shift  */
    case 0x3A: caps_lock ^= 1; return;               /* caps lock toggle  */
    default:   break;
    }

    if (sc >= 128) return;

    int upper = shift_held ^ caps_lock;
    char c = upper ? sc_upper[sc] : sc_lower[sc];
    if (!c) return;

    /* Echo to VGA */
    vga_putchar(c);

    kb_push(c);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void keyboard_init(void)
{
    irq_install_handler(1, keyboard_irq_handler);
}

char keyboard_getchar(void)
{
    if (kb_buf_empty()) return 0;
    char c = kb_buf[kb_tail & KB_BUF_MASK];
    __asm__ volatile("" ::: "memory");
    kb_tail++;
    return c;
}

char keyboard_getchar_wait(void)
{
    while (kb_buf_empty())
        __asm__ volatile("hlt");
    return keyboard_getchar();
}
