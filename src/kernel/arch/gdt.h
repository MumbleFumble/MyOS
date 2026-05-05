#pragma once

#include <stdint.h>

/*
 * 64-bit GDT descriptor (8 bytes)
 * We build each entry manually as a uint64_t so we don't fight bitfield
 * layout rules across compilers.
 */

/*
 * TSS descriptor is 16 bytes in 64-bit mode (occupies two GDT slots).
 */
struct tss_descriptor {
    uint32_t limit_low  : 16;
    uint32_t base_low   : 24;
    uint32_t type       : 4;    /* 0x9 = 64-bit TSS available */
    uint32_t zero0      : 1;
    uint32_t dpl        : 2;
    uint32_t present    : 1;
    uint32_t limit_high : 4;
    uint32_t avl        : 1;
    uint32_t zero1      : 2;
    uint32_t granularity: 1;
    uint32_t base_mid   : 8;
    uint32_t base_high  : 32;
    uint32_t reserved   : 32;
} __attribute__((packed));

/*
 * x86-64 TSS.  We only need rsp0 and the iomap offset.
 * The rest must be zero.
 */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;          /* kernel stack for ring-0 entry from ring-3 */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];        /* interrupt stack table entries */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    /* offset to I/O permission bitmap */
} __attribute__((packed));

/*
 * GDT layout (indices / selectors):
 *   0x00  null
 *   0x08  kernel code  (ring 0)
 *   0x10  kernel data  (ring 0)
 *   0x18  user code    (ring 3)
 *   0x20  user data    (ring 3)
 *   0x28  TSS low      (ring 0, 16-byte entry spanning 0x28 + 0x30)
 */
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_CODE    0x1B   /* 0x18 | RPL 3 */
#define GDT_USER_DATA    0x23   /* 0x20 | RPL 3 */
#define GDT_TSS_SEL      0x28

void gdt_init(void);

/* Call this after kmalloc is ready to set rsp0 (kernel stack for ring-3 tasks). */
void tss_set_rsp0(uint64_t rsp0);
