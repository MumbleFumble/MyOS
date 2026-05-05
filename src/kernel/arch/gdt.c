#include "gdt.h"

/* GDT: null + kernel code/data + user code/data + TSS (2 slots) = 7 entries */
#define GDT_ENTRIES 7

static uint64_t gdt[GDT_ENTRIES];
static struct tss  kernel_tss;

extern void gdt_load(uint64_t gdtr_addr, uint16_t cs, uint16_t ds);
extern void tss_load(uint16_t sel);

/* -----------------------------------------------------------------------
 * Helpers to build GDT descriptors from raw fields.
 * See Intel SDM Vol.3 3.4.5 for the layout.
 * ----------------------------------------------------------------------- */

static uint64_t make_code_descriptor(uint8_t dpl)
{
    /*
     * In 64-bit mode the only meaningful bits are:
     *   P=1, DPL, S=1, type=0xA (code, execute/read), L=1 (64-bit), D=0
     * Base and limit are ignored by hardware in 64-bit mode.
     */
    uint64_t desc = 0;
    desc |= (uint64_t)1      << 47;   /* P */
    desc |= (uint64_t)dpl    << 45;   /* DPL */
    desc |= (uint64_t)1      << 44;   /* S (code/data) */
    desc |= (uint64_t)0xA    << 40;   /* type: code, execute/read */
    desc |= (uint64_t)1      << 53;   /* L (64-bit code) */
    return desc;
}

static uint64_t make_data_descriptor(uint8_t dpl)
{
    /* type=0x2 (data, read/write), P=1, S=1, D/B=1 */
    uint64_t desc = 0;
    desc |= (uint64_t)1      << 47;   /* P */
    desc |= (uint64_t)dpl    << 45;   /* DPL */
    desc |= (uint64_t)1      << 44;   /* S */
    desc |= (uint64_t)0x2    << 40;   /* type: data, read/write */
    desc |= (uint64_t)1      << 54;   /* D/B */
    return desc;
}

static void install_tss_descriptor(int slot, uint64_t base, uint32_t limit)
{
    /*
     * TSS descriptor is 16 bytes.  We store it in two consecutive 8-byte
     * GDT slots.
     *
     * Low 8 bytes layout:
     *   [15: 0]  limit[15:0]
     *   [39:16]  base[23:0]
     *   [43:40]  type = 0x9 (64-bit TSS available)
     *   [44]     S    = 0
     *   [46:45]  DPL  = 0
     *   [47]     P    = 1
     *   [51:48]  limit[19:16]
     *   [52]     AVL  = 0
     *   [54:53]  00
     *   [55]     G    = 0
     *   [63:56]  base[31:24]
     *
     * High 8 bytes:
     *   [31: 0]  base[63:32]
     *   [63:32]  reserved (must be 0)
     */
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFF);
    low |= (uint64_t)(base  & 0xFFFFFF)   << 16;
    low |= (uint64_t)0x9                   << 40;  /* type */
    low |= (uint64_t)1                     << 47;  /* P */
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;
    low |= (uint64_t)((base >> 24) & 0xFF) << 56;

    uint64_t high = (base >> 32) & 0xFFFFFFFF;

    gdt[slot]     = low;
    gdt[slot + 1] = high;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void gdt_init(void)
{
    /* 0: null */
    gdt[0] = 0;

    /* 1: kernel code  (ring 0) -> selector 0x08 */
    gdt[1] = make_code_descriptor(0);

    /* 2: kernel data  (ring 0) -> selector 0x10 */
    gdt[2] = make_data_descriptor(0);

    /* 3: user code    (ring 3) -> selector 0x18 (use 0x1B with RPL=3) */
    gdt[3] = make_code_descriptor(3);

    /* 4: user data    (ring 3) -> selector 0x20 (use 0x23 with RPL=3) */
    gdt[4] = make_data_descriptor(3);

    /* 5+6: TSS descriptor (16 bytes, two slots) -> selector 0x28 */
    /* Zero the TSS first */
    for (uint32_t i = 0; i < sizeof(struct tss); i++)
        ((uint8_t *)&kernel_tss)[i] = 0;

    kernel_tss.iomap_base = sizeof(struct tss); /* no I/O port access from ring 3 */
    /* rsp0 left as 0 for now; set by tss_set_rsp0() once the kernel stack is known */

    install_tss_descriptor(5, (uint64_t)&kernel_tss, sizeof(struct tss) - 1);

    /* Build GDTR and reload all segment registers */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr;

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)gdt;

    gdt_load((uint64_t)&gdtr, GDT_KERNEL_CODE, GDT_KERNEL_DATA);

    /* Load the TSS into TR */
    tss_load(GDT_TSS_SEL);
}

void tss_set_rsp0(uint64_t rsp0)
{
    kernel_tss.rsp0 = rsp0;
}
