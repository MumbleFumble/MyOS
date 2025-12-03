#include "vmm.h"
#include "pmm.h"

/* 64-bit 4-level paging structures */
static uint64_t *current_pml4 = 0;

/* For now, just use the boot-time identity mapping */

void vmm_map_page(uint64_t phys, uint64_t virt, uint64_t flags)
{
    /* TODO: Implement 4-level page table mapping */
    /* For now, rely on boot.S identity map of first 2MB */
    (void)phys;
    (void)virt;
    (void)flags;
}

void vmm_unmap_page(uint64_t virt)
{
    /* TODO: Implement page unmapping */
    (void)virt;
}

void vmm_init(void)
{
    /* Boot.S already set up identity mapping for first 2MB */
    /* Just read CR3 to get the current PML4 */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    current_pml4 = (uint64_t *)(cr3 & ~0xFFFUL);
}
