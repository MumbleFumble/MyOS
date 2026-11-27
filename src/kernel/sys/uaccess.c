/* uaccess.c — the page-table predicate and copy helpers
 * behind uaccess.h (Phase 17 inc.3). The predicate walks
 * the CURRENT CR3 (the caller's address space; syscalls
 * run on it) with the same identity-cast conventions as
 * vmm.c's walker. Every level must be PRESENT and USER;
 * write-direction checks additionally require RW at every
 * level. A PS (huge page) entry at PDPT or PD level is
 * refused outright: user slots in this kernel are mapped
 * exclusively with 4K pages, and refusing is the safe
 * direction.
 */
#include <stdint.h>
#include "uaccess.h"
#include "uaccess_check.h"
#include "syscall.h"          /* EFAULT */
#include "../mem/vmm.h"       /* PAGE_PRESENT, PAGE_RW, PAGE_USER */

#define PTE_PS 0x80ULL        /* page-size bit (vmm.h has no name) */

static int kernel_page_user_ok(void *ctx, uint64_t page_va,
                               int need_write)
{
    (void)ctx;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t need = PAGE_PRESENT | PAGE_USER |
                    (need_write ? PAGE_RW : 0);

    uint64_t *pml4 = (uint64_t *)(cr3 & ~0xFFFUL);
    uint64_t e = pml4[(page_va >> 39) & 0x1FF];
    if ((e & need) != need) return 0;

    uint64_t *pdpt = (uint64_t *)(e & ~0xFFFUL);
    e = pdpt[(page_va >> 30) & 0x1FF];
    if ((e & need) != need) return 0;
    if (e & PTE_PS) return 0;

    uint64_t *pd = (uint64_t *)(e & ~0xFFFUL);
    e = pd[(page_va >> 21) & 0x1FF];
    if ((e & need) != need) return 0;
    if (e & PTE_PS) return 0;

    uint64_t *pt = (uint64_t *)(e & ~0xFFFUL);
    e = pt[(page_va >> 12) & 0x1FF];
    if ((e & need) != need) return 0;
    return 1;
}

int user_range_ok(uint64_t va, uint64_t len, int need_write)
{
    return uaccess_validate(va, len, need_write,
                            kernel_page_user_ok, 0);
}

int copy_from_user(void *kdst, uint64_t usrc, uint64_t len)
{
    if (!user_range_ok(usrc, len, 0)) return EFAULT;
    uint8_t *d = (uint8_t *)kdst;
    const uint8_t *s = (const uint8_t *)usrc;
    for (uint64_t i = 0; i < len; i++) d[i] = s[i];
    return 0;
}

int copy_to_user(uint64_t udst, const void *ksrc, uint64_t len)
{
    if (!user_range_ok(udst, len, 1)) return EFAULT;
    uint8_t *d = (uint8_t *)udst;
    const uint8_t *s = (const uint8_t *)ksrc;
    for (uint64_t i = 0; i < len; i++) d[i] = s[i];
    return 0;
}

int copy_string_from_user(char *kdst, uint64_t usrc, uint64_t max)
{
    if (max == 0) return EFAULT;
    if (!uaccess_shape_ok(usrc, 1)) return EFAULT;
    /* Clamp so usrc + i can never step onto or past the
     * canonical limit mid-scan. */
    uint64_t room = UACCESS_USER_LIMIT - usrc;
    if (max > room) max = room;

    uint64_t checked_page = ~(uint64_t)0;
    for (uint64_t i = 0; i < max; i++) {
        uint64_t addr = usrc + i;
        uint64_t page = addr & ~(UACCESS_PAGE_SIZE - 1);
        if (page != checked_page) {
            if (!kernel_page_user_ok(0, page, 0)) return EFAULT;
            checked_page = page;
        }
        char c = *(const char *)addr;
        kdst[i] = c;
        if (c == 0) return (int)i;
    }
    return EFAULT;   /* no NUL within max bytes */
}
