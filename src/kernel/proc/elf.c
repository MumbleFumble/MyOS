#include "elf.h"
#include "../mem/vmm.h"
#include "../mem/pmm.h"

/* User stack: 16KB mapped just below 0x10001000000 (1TB + 16MB)
 * Both code and stack live in PML4[2] (1TB–2TB range), away from the boot
 * identity map in PML4[0] which covers 0–512MB with huge pages. */
#define USER_STACK_TOP    0x10001000000UL
#define USER_STACK_PAGES  4           /* 4 × 4KB = 16 KB */

/* Flags used for segment mappings */
#define SEG_FLAGS  (PAGE_PRESENT | PAGE_RW | PAGE_USER)

/* Zero n bytes at a virtual address in the current address space.
 * Safe because the kernel identity-maps the first 512MB, and all frames
 * we allocate via PMM are in that range. */
static void memzero(uint64_t vaddr, uint64_t n)
{
    uint8_t *p = (uint8_t *)vaddr;
    for (uint64_t i = 0; i < n; i++)
        p[i] = 0;
}

static void memcopy(uint64_t dst, const uint8_t *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++)
        d[i] = src[i];
}

int elf_load(const uint8_t *data, uint64_t size, elf_result_t *out)
{
    if (size < sizeof(Elf64_Ehdr))
        return -1;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;

    /* Validate magic and class */
    if (*(uint32_t *)eh->e_ident != ELF_MAGIC)   return -1;
    if (eh->e_ident[4] != ELF_CLASS64)            return -1;
    if (eh->e_ident[5] != ELF_DATA_LE)            return -1;
    if (eh->e_type    != ELF_TYPE_EXEC)            return -1;
    if (eh->e_machine != ELF_MACH_X64)            return -1;

    /* Create a new address space */
    uint64_t cr3 = vmm_create_address_space();
    if (!cr3) return -1;

    /* Process PT_LOAD segments */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff
                                                     + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0)      continue;

        /* Align vaddr down to page boundary */
        uint64_t vaddr  = ph->p_vaddr & ~(PAGE_SIZE - 1);
        uint64_t offset = ph->p_vaddr - vaddr;
        uint64_t map_sz = ph->p_memsz + offset;
        uint64_t npages = (map_sz + PAGE_SIZE - 1) / PAGE_SIZE;

        /* Allocate and map pages for this segment */
        for (uint64_t p = 0; p < npages; p++) {
            uint64_t frame = pmm_alloc_page();
            if (!frame) return -1;

            /* Zero the frame (it lives in the identity-mapped region) */
            memzero(frame, PAGE_SIZE);

            vmm_map_page_in(cr3,
                            frame,
                            vaddr + p * PAGE_SIZE,
                            SEG_FLAGS);

            /* Copy file data into the first filesz bytes */
            uint64_t page_virt  = vaddr + p * PAGE_SIZE;
            uint64_t page_start = page_virt;           /* virtual == physical here */
            uint64_t page_end   = page_start + PAGE_SIZE;

            uint64_t seg_file_start = ph->p_vaddr;
            uint64_t seg_file_end   = ph->p_vaddr + ph->p_filesz;

            /* Clamp copy range to this page */
            uint64_t copy_start = seg_file_start > page_start ? seg_file_start : page_start;
            uint64_t copy_end   = seg_file_end   < page_end   ? seg_file_end   : page_end;

            if (copy_start < copy_end) {
                uint64_t dst_frame_off = copy_start - page_start;
                uint64_t src_off       = copy_start - ph->p_vaddr + ph->p_offset;
                memcopy(frame + dst_frame_off,
                        data + src_off,
                        copy_end - copy_start);
            }
        }
    }

    /* Allocate user stack pages */
    uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    for (uint32_t p = 0; p < USER_STACK_PAGES; p++) {
        uint64_t frame = pmm_alloc_page();
        if (!frame) return -1;
        memzero(frame, PAGE_SIZE);
        vmm_map_page_in(cr3,
                        frame,
                        stack_base + p * PAGE_SIZE,
                        SEG_FLAGS);
    }

    out->cr3    = cr3;
    out->entry  = eh->e_entry;
    out->ustack = USER_STACK_TOP;
    return 0;
}
