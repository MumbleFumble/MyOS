/* uaccess.h — validated user-pointer access for syscalls
 * (Phase 17 inc.3). The decision logic lives in
 * uaccess_check.h (host-tested); uaccess.c supplies the
 * page-table predicate and the copy helpers.
 *
 * INVARIANT the validate-then-use pattern rests on: a
 * validated page cannot become invalid before the caller
 * uses it. This kernel has no unmap and no page
 * replacement, mappings in an address space only grow,
 * each space is used by its owning task alone, syscalls
 * run on the caller's CR3, and every fd op dereferences
 * its buffer synchronously in the caller's context (IRQ
 * handlers only wake; they never copy to user buffers).
 * If any of that changes (munmap, swap, shared mappings,
 * deferred completion), this layer must move to bounce
 * buffers.
 */
#pragma once

#include <stdint.h>

/* Nonzero iff the whole range is validated user memory in
 * the caller's address space. need_write additionally
 * requires every page to be writable. */
int user_range_ok(uint64_t va, uint64_t len, int need_write);

/* Copy len bytes between kernel memory and a validated
 * user range. Return 0 on success, EFAULT if the range is
 * refused (nothing is copied in either direction). */
int copy_from_user(void *kdst, uint64_t usrc, uint64_t len);
int copy_to_user(uint64_t udst, const void *ksrc, uint64_t len);

/* Copy a NUL-terminated string from user memory into a
 * kernel buffer of max bytes (including the NUL), checking
 * each page before reading any byte in it. Returns the
 * string length on success, EFAULT if a page is bad before
 * the NUL or no NUL appears within max bytes. */
int copy_string_from_user(char *kdst, uint64_t usrc, uint64_t max);
