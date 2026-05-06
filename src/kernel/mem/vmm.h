#pragma once

#include <stdint.h>

#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000UL

#define PAGE_SIZE 4096UL

#define PAGE_PRESENT 0x001UL
#define PAGE_RW      0x002UL
#define PAGE_USER    0x004UL
#define PAGE_NX      (1UL << 63)

/*
 * Initialise VMM: record the boot PML4 physical address from CR3.
 */
void vmm_init(void);

/*
 * Get/set the active address space (CR3 physical address).
 */
uint64_t vmm_current_cr3(void);
void     vmm_switch(uint64_t cr3_phys);

/*
 * Map a single 4KB page.
 *   vmm_map_page       — maps in the CURRENT address space
 *   vmm_map_page_in    — maps in an arbitrary address space
 */
void vmm_map_page(uint64_t phys, uint64_t virt, uint64_t flags);
void vmm_map_page_in(uint64_t cr3_phys, uint64_t phys, uint64_t virt, uint64_t flags);
void vmm_unmap_page(uint64_t virt);

/*
 * Allocate n_pages physical frames and map them contiguously at virt
 * in the given address space.  Returns 0 on success, -1 on OOM.
 */
int vmm_alloc_pages(uint64_t cr3_phys, uint64_t virt, uint64_t n_pages, uint64_t flags);

/*
 * Translate a virtual address in an arbitrary address space to its physical
 * address by walking the page tables.  Returns 0 if the mapping is absent.
 */
uint64_t vmm_virt_to_phys(uint64_t cr3_phys, uint64_t virt);

/*
 * Create a new address space (PML4).
 * The boot kernel mapping (PML4[0]) is shared so kernel code stays reachable.
 * Returns the physical address of the new PML4, or 0 on OOM.
 */
uint64_t vmm_create_address_space(void);
