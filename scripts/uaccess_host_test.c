/* Host test for the pure user-range validator
 * (src/kernel/sys/uaccess_check.h), in the pattern of
 * scripts/glob_host_test.c: the same header the kernel
 * compiles, driven here by a fake sparse page table so the
 * mapped/unmapped decisions are testable without a guest.
 * The kernel's real predicate (a 4-level CR3 walk in
 * uaccess.c) is exercised only in the guest; everything
 * about range shape and page-span iteration is tested
 * here.
 *
 * Build: gcc -std=c11 -Wall -Wextra \
 *          -o /tmp/uaccess_host_test scripts/uaccess_host_test.c
 */
#include <stdio.h>
#include <stdint.h>
#include "../src/kernel/sys/uaccess_check.h"

/* Fake table: three mapped user pages at 0x10000000000
 * (the ELF-image slot), one of them read-only; one mapped
 * page at the demand base 0x20000000000. Everything else
 * is unmapped. */
static const uint64_t mapped[] = {
    0x10000000000ULL, 0x10000001000ULL, 0x10000002000ULL,
    0x20000000000ULL,
};
static const uint64_t readonly_page = 0x10000001000ULL;
static int calls;
static int last_need_write;

static int fake_page_ok(void *ctx, uint64_t page_va, int need_write)
{
    (void)ctx;
    calls++;
    last_need_write = need_write;
    for (unsigned i = 0; i < sizeof mapped / sizeof mapped[0]; i++)
        if (mapped[i] == page_va) {
            if (need_write && page_va == readonly_page) return 0;
            return 1;
        }
    return 0;
}

static int fails;
static void expect(const char *name, uint64_t va, uint64_t len,
                   int need_write, int want)
{
    calls = 0;
    int got = uaccess_validate(va, len, need_write, fake_page_ok, 0);
    if (got != want) {
        printf("FAIL %s: validate(0x%llx, %llu, w=%d) = %d, want %d\n",
               name, (unsigned long long)va,
               (unsigned long long)len, need_write, got, want);
        fails++;
    }
}
static void expect_calls(const char *name, int want)
{
    if (calls != want) {
        printf("FAIL %s: predicate called %d times, want %d\n",
               name, calls, want);
        fails++;
    }
}

int main(void)
{
    /* Shape: kernel slot 0 is never user memory. */
    expect("kernel-range start", 0x1000, 16, 0, 0);
    expect("kernel-range interior", 0x400000, 8, 0, 0);
    expect("null page", 0, 1, 0, 0);
    /* Shape: straddling the PML4 slot-0 boundary. */
    expect("straddle slot-0 end", 0x8000000000ULL - 8, 16, 0, 0);
    /* At the boundary exactly: shape passes, walk refuses
     * (the page is unmapped in the fake table). */
    expect("at slot-0 end, unmapped", 0x8000000000ULL, 16, 0, 0);
    /* Shape: wrap-around and the canonical limit. */
    expect("wrap-around", 0xFFFFFFFFFFFFF000ULL, 0x2000, 0, 0);
    expect("straddle canonical limit",
           UACCESS_USER_LIMIT - 8, 16, 0, 0);
    expect("above canonical limit", UACCESS_USER_LIMIT, 16, 0, 0);
    /* Zero length: always valid, no page is consulted. */
    expect("zero length at null", 0, 0, 0, 1);
    expect_calls("zero length at null", 0);
    expect("zero length at kernel ptr", 0x1000, 0, 1, 1);
    expect_calls("zero length at kernel ptr", 0);
    expect("zero length at unmapped", 0x50000000000ULL, 0, 0, 1);
    /* Valid ranges over the fake table. */
    expect("one mapped page", 0x10000000000ULL, 100, 0, 1);
    expect("multi-page all mapped", 0x10000000F00ULL, 0x200, 0, 1);
    expect_calls("multi-page all mapped", 2);
    expect("three pages exactly", 0x10000000000ULL, 0x3000, 0, 1);
    expect_calls("three pages exactly", 3);
    expect("demand page", 0x20000000000ULL, 4096, 0, 1);
    /* Mapped/unmapped straddles: refused, whole range
     * checked in order, stops at the first bad page. */
    expect("straddle mapped/unmapped", 0x10000002000ULL, 0x2000, 0, 0);
    expect("first page unmapped", 0x10000003000ULL, 0x2000, 0, 0);
    expect_calls("first page unmapped", 1);
    expect("unmapped user range", 0x50000000000ULL, 4096, 0, 0);
    /* Direction: the read-only page refuses writes only. */
    expect("read from ro page", 0x10000001000ULL, 16, 0, 1);
    expect("write to ro page", 0x10000001000ULL, 16, 1, 0);
    if (last_need_write != 1) {
        printf("FAIL need_write not passed through\n");
        fails++;
    }
    expect("write spanning ro page", 0x10000000000ULL, 0x2000, 1, 0);
    expect("read spanning ro page", 0x10000000000ULL, 0x2000, 0, 1);

    if (fails) {
        printf("uaccess host test: %d FAILURES\n", fails);
        return 1;
    }
    printf("uaccess host test: all cases PASS\n");
    return 0;
}
