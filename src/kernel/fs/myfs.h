#pragma once
#include <stdint.h>

/*
 * MyFS — simple flat disk filesystem (read-only through Phase 5;
 * append-only writes since Phase 6 inc.3).
 *
 * Disk layout:
 *   Sector 0:  struct myfs_super (512 bytes)
 *   Sector 1+: file records, each padded to a multiple of 512 bytes
 *
 * File record layout (contiguous on disk):
 *   struct myfs_file_hdr  (80 bytes, sector-aligned header at start of record)
 *   uint8_t data[size]
 *   padding to next 512-byte boundary
 */

#define MYFS_MAGIC  0x4D594653UL   /* "MYFS" */
#define MYFS_NAME_MAX 64

/* Record flag in myfs_file_hdr_t._pad[0] (Phase 6 inc.3): the record
 * is a tombstone. MyFS updates are append-only: rewriting a file
 * tombstones its old record and appends the new content at the end
 * of the log; the mount walk lets the latest live record win. */
#define MYFS_FLAG_DELETED 0x01

typedef struct {
    uint32_t magic;
    uint32_t file_count;
    uint8_t  _pad[504];            /* pad to 512 bytes */
} __attribute__((packed)) myfs_super_t;

typedef struct {
    char     name[MYFS_NAME_MAX];
    uint64_t size;                 /* data size in bytes */
    uint8_t  _pad[8];              /* pad header to 80 bytes */
} __attribute__((packed)) myfs_file_hdr_t;
