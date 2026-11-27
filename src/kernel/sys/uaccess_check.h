/* uaccess_check.h — pure range-validation logic for user
 * pointers (Phase 17 inc.3). No page tables, no kernel
 * state: the shape arithmetic plus a page-span walk driven
 * by a caller-supplied predicate. This header is the single
 * source consumed by the kernel (src/kernel/sys/uaccess.c
 * supplies a predicate that walks the caller's real page
 * tables) and by the host test (scripts/uaccess_host_test.c
 * supplies a fake sparse table), in the pattern of
 * user/glob.h.
 *
 * The decision for a range (va, len):
 *   - len == 0 is always valid, for any va: no byte is
 *     touched, so no page needs checking. (Preserves the
 *     long-standing count==0 syscall behaviour.)
 *   - va must lie wholly outside PML4 slot 0's span
 *     [0, UACCESS_KERNEL_SLOT_END): every user address
 *     space shares that slot with the identity-mapped
 *     kernel, so nothing in it is ever user memory.
 *   - va + len must not wrap and must not pass
 *     UACCESS_USER_LIMIT (the canonical low half, end
 *     exclusive).
 *   - every page the range spans must satisfy the
 *     predicate (present + user-accessible, plus writable
 *     when need_write is set).
 * The whole range is checked before the caller moves any
 * byte, so a refused copy_to_user writes nothing and a
 * refused copy_from_user has no side effects.
 */
#ifndef MYOS_UACCESS_CHECK_H
#define MYOS_UACCESS_CHECK_H

#include <stdint.h>

#define UACCESS_KERNEL_SLOT_END 0x8000000000ULL   /* 1 << 39 */
#define UACCESS_USER_LIMIT      0x800000000000ULL /* 1 << 47 */
#define UACCESS_PAGE_SIZE       4096ULL

/* Predicate: return nonzero iff the page containing
 * page_va (page-aligned) is present and user-accessible
 * (and writable when need_write is set). */
typedef int (*uaccess_page_fn)(void *ctx, uint64_t page_va,
                               int need_write);

static inline int uaccess_shape_ok(uint64_t va, uint64_t len)
{
    if (len == 0) return 1;
    if (va < UACCESS_KERNEL_SLOT_END) return 0;
    if (va >= UACCESS_USER_LIMIT) return 0;
    if (len > UACCESS_USER_LIMIT - va) return 0;  /* wrap + limit */
    return 1;
}

static inline int uaccess_validate(uint64_t va, uint64_t len,
                                   int need_write,
                                   uaccess_page_fn page_ok,
                                   void *ctx)
{
    if (!uaccess_shape_ok(va, len)) return 0;
    if (len == 0) return 1;
    uint64_t first = va & ~(UACCESS_PAGE_SIZE - 1);
    uint64_t last  = (va + len - 1) & ~(UACCESS_PAGE_SIZE - 1);
    for (uint64_t p = first;; p += UACCESS_PAGE_SIZE) {
        if (!page_ok(ctx, p, need_write)) return 0;
        if (p == last) break;
    }
    return 1;
}

#endif /* MYOS_UACCESS_CHECK_H */
