/* Minimal first-fit malloc/free for user-space programs.
 * Backed by SYS_SBRK — allocates from the kernel-managed heap.
 *
 * Block layout (each allocation is preceded by a header):
 *
 *   ┌──────────────────┐
 *   │ size  (8 bytes)  │  ← total payload size (not including header)
 *   │ next  (8 bytes)  │  ← pointer to next free block, or NULL if used
 *   │ free  (8 bytes)  │  ← 1 = free, 0 = allocated
 *   ├──────────────────┤
 *   │ user data        │
 *   └──────────────────┘
 */
#include "malloc.h"
#include "syscall.h"

typedef unsigned long  ulong;
typedef unsigned char  u8;

#define ALIGN8(x)  (((x) + 7UL) & ~7UL)

typedef struct block_hdr {
    ulong            size;  /* payload bytes (not including this header) */
    struct block_hdr *next; /* next block in the free list (NULL if allocated) */
    ulong            free;  /* 1 = free, 0 = in use */
} block_hdr_t;

#define HDR_SIZE  sizeof(block_hdr_t)

static block_hdr_t *free_list = (void *)0;

/* -----------------------------------------------------------------------
 * Request `sz` bytes from the kernel via sbrk and return a fresh block.
 * ----------------------------------------------------------------------- */
static block_hdr_t *request_block(ulong sz)
{
    block_hdr_t *blk = (block_hdr_t *)sys_sbrk((long)(HDR_SIZE + sz));
    if ((long)blk == -1) return (void *)0;
    blk->size = sz;
    blk->next = (void *)0;
    blk->free = 0;
    return blk;
}

/* -----------------------------------------------------------------------
 * malloc
 * ----------------------------------------------------------------------- */
void *malloc(ulong size)
{
    if (size == 0) return (void *)0;
    size = ALIGN8(size);

    /* Walk the free list for a block that fits */
    block_hdr_t *prev = (void *)0;
    block_hdr_t *cur  = free_list;

    while (cur) {
        if (cur->free && cur->size >= size) {
            /* Split if there's room for another header + at least 8 bytes */
            if (cur->size >= size + HDR_SIZE + 8) {
                block_hdr_t *split = (block_hdr_t *)((u8 *)(cur + 1) + size);
                split->size = cur->size - size - HDR_SIZE;
                split->next = cur->next;
                split->free = 1;
                cur->size   = size;
                cur->next   = split;
            }
            cur->free = 0;
            return (void *)(cur + 1);
        }
        prev = cur;
        cur  = cur->next;
    }

    /* No suitable free block — grow the heap */
    block_hdr_t *blk = request_block(size);
    if (!blk) return (void *)0;

    if (prev)
        prev->next = blk;
    else
        free_list = blk;

    return (void *)(blk + 1);
}

/* -----------------------------------------------------------------------
 * free
 * ----------------------------------------------------------------------- */
void free(void *ptr)
{
    if (!ptr) return;
    block_hdr_t *blk = (block_hdr_t *)ptr - 1;
    blk->free = 1;

    /* Coalesce adjacent free blocks */
    block_hdr_t *cur = free_list;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            /* Merge cur and cur->next */
            cur->size += HDR_SIZE + cur->next->size;
            cur->next  = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

/* -----------------------------------------------------------------------
 * calloc
 * ----------------------------------------------------------------------- */
void *calloc(ulong nmemb, ulong size)
{
    ulong total = nmemb * size;
    void *ptr = malloc(total);
    if (!ptr) return (void *)0;
    u8 *p = (u8 *)ptr;
    for (ulong i = 0; i < total; i++) p[i] = 0;
    return ptr;
}

/* -----------------------------------------------------------------------
 * realloc
 * ----------------------------------------------------------------------- */
void *realloc(void *ptr, ulong size)
{
    if (!ptr)   return malloc(size);
    if (!size) { free(ptr); return (void *)0; }

    block_hdr_t *blk = (block_hdr_t *)ptr - 1;
    if (blk->size >= size) return ptr;  /* already big enough */

    void *new = malloc(size);
    if (!new) return (void *)0;

    /* Copy old data */
    u8 *src = (u8 *)ptr;
    u8 *dst = (u8 *)new;
    ulong n = blk->size < size ? blk->size : size;
    for (ulong i = 0; i < n; i++) dst[i] = src[i];
    free(ptr);
    return new;
}
