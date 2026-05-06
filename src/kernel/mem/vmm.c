#include "vmm.h"
#include "pmm.h"

/* -----------------------------------------------------------------------
 * Physical-to-virtual conversion.
 * The kernel identity-maps the first 16MB in boot.S, so for any
 * frame allocated by PMM (which lives in that range) physical == virtual.
 * ----------------------------------------------------------------------- */
static inline uint64_t *pa2va(uint64_t phys)
{
    return (uint64_t *)phys;
}

/* Allocate a page-table frame from PMM and zero it. */
static uint64_t alloc_pt_frame(void)
{
    uint64_t frame = pmm_alloc_page();
    if (!frame) return 0;
    uint64_t *p = pa2va(frame);
    for (int i = 0; i < 512; i++)
        p[i] = 0;
    return frame;
}

/*
 * Walk one level of the page-table hierarchy.  If the entry is absent,
 * allocate and install a new table.  Returns a pointer to the next-level
 * table, or NULL if the entry is a huge page or allocation fails.
 */
static uint64_t *get_or_create_table(uint64_t *parent, uint64_t idx, uint64_t user_bit)
{
    if (!(parent[idx] & PAGE_PRESENT)) {
        uint64_t frame = alloc_pt_frame();
        if (!frame) return (void *)0;
        parent[idx] = frame | PAGE_PRESENT | PAGE_RW | user_bit;
    }
    /* Refuse to split an existing huge page (PS bit = bit 7). */
    if (parent[idx] & 0x80)
        return (void *)0;
    return pa2va(parent[idx] & ~0xFFFUL);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void vmm_init(void)
{
    /* boot.S already installed the identity map — nothing more to do. */
}

uint64_t vmm_current_cr3(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void vmm_switch(uint64_t cr3_phys)
{
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3_phys) : "memory");
}

void vmm_map_page_in(uint64_t cr3_phys, uint64_t phys, uint64_t virt, uint64_t flags)
{
    uint64_t *pml4 = pa2va(cr3_phys & ~0xFFFUL);
    uint64_t user_bit = (flags & PAGE_USER) ? PAGE_USER : 0;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_or_create_table(pml4, pml4_idx, user_bit);
    if (!pdpt) return;
    uint64_t *pd = get_or_create_table(pdpt, pdpt_idx, user_bit);
    if (!pd) return;
    uint64_t *pt = get_or_create_table(pd, pd_idx, user_bit);
    if (!pt) return;

    pt[pt_idx] = (phys & ~0xFFFUL) | (flags | PAGE_PRESENT);
}

void vmm_map_page(uint64_t phys, uint64_t virt, uint64_t flags)
{
    vmm_map_page_in(vmm_current_cr3(), phys, virt, flags);
}

void vmm_unmap_page(uint64_t virt)
{
    uint64_t *pml4 = pa2va(vmm_current_cr3() & ~0xFFFUL);

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PAGE_PRESENT)) return;
    uint64_t *pdpt = pa2va(pml4[pml4_idx] & ~0xFFFUL);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) return;
    uint64_t *pd = pa2va(pdpt[pdpt_idx] & ~0xFFFUL);
    if (!(pd[pd_idx] & PAGE_PRESENT) || (pd[pd_idx] & 0x80)) return;
    uint64_t *pt = pa2va(pd[pd_idx] & ~0xFFFUL);
    pt[pt_idx] = 0;

    /* Invalidate the TLB entry for this page. */
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

int vmm_alloc_pages(uint64_t cr3_phys, uint64_t virt, uint64_t n_pages, uint64_t flags)
{
    for (uint64_t i = 0; i < n_pages; i++) {
        uint64_t frame = pmm_alloc_page();
        if (!frame) return -1;
        vmm_map_page_in(cr3_phys, frame, virt + i * PAGE_SIZE, flags);
    }
    return 0;
}

uint64_t vmm_virt_to_phys(uint64_t cr3_phys, uint64_t virt)
{
    uint64_t *pml4 = pa2va(cr3_phys & ~0xFFFUL);
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;
    uint64_t off      = virt & 0xFFFUL;

    if (!(pml4[pml4_idx] & PAGE_PRESENT)) return 0;
    uint64_t *pdpt = pa2va(pml4[pml4_idx] & ~0xFFFUL);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) return 0;
    uint64_t *pd = pa2va(pdpt[pdpt_idx] & ~0xFFFUL);
    if (!(pd[pd_idx] & PAGE_PRESENT)) return 0;
    if (pd[pd_idx] & 0x80) /* huge page */
        return (pd[pd_idx] & ~0x1FFFFFUL) | (virt & 0x1FFFFFUL);
    uint64_t *pt = pa2va(pd[pd_idx] & ~0xFFFUL);
    if (!(pt[pt_idx] & PAGE_PRESENT)) return 0;
    return (pt[pt_idx] & ~0xFFFUL) | off;
}

uint64_t vmm_create_address_space(void)
{
    uint64_t new_pml4_phys = alloc_pt_frame();
    if (!new_pml4_phys) return 0;

    uint64_t *new_pml4  = pa2va(new_pml4_phys);
    uint64_t *boot_pml4 = pa2va(vmm_current_cr3() & ~0xFFFUL);

    /*
     * Share PML4[0]: points to the boot PDPT which identity-maps 0-16MB
     * (the kernel).  All address spaces must be able to execute kernel code
     * and access kernel data.  User pages go into other PML4 entries.
     */
    new_pml4[0] = boot_pml4[0];
    /* Entries 1-511 are already zeroed by alloc_pt_frame. */

    return new_pml4_phys;
}
