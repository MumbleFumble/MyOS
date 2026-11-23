#pragma once
#include "vfs.h"

/* Mount the MyFS filesystem from the primary ATA disk.
 * Returns 0 on success, -1 if no disk or wrong magic. */
int diskfs_mount(void);

/* Open a file from the disk filesystem. Returns NULL if not found. */
vfs_node_t *diskfs_open(const char *name);
/* Create (or truncate) a file and open it (Phase 6 inc.3). */
vfs_node_t *diskfs_create(const char *name);

/* Delete a file by tombstoning its record (Phase 11). 0 or -1. */
int diskfs_delete(const char *name);
/* Rename a file (Phase 11). 0 or -1. */
int diskfs_rename(const char *old_name, const char *new_name);

/* Enumerate disk filesystem files by index. Returns 0 or -1. */
int diskfs_readdir(uint32_t index, char name_out[VFS_NAME_MAX]);

/* Returns 1 if a disk is mounted. */
int diskfs_is_mounted(void);

/* Capacity snapshot for the shell `df` command (Phase 13
 * inc.5). All figures come from the filesystem's live
 * bookkeeping: the append pointer, the superblock record
 * count and the in-memory live-file table. Read-only. */
struct diskfs_df {
    uint32_t cap_sectors;   /* sectors available to the log */
    uint32_t free_sectors;  /* append_limit - append_lba */
    uint32_t records;       /* total log records (superblock) */
    uint32_t live_records;  /* == live files (one record each) */
    uint32_t live_sectors;  /* sectors held by live records */
    uint32_t dead_records;  /* tombstones / superseded records */
    uint32_t dead_sectors;  /* sectors they hold (compaction) */
};
/* Fill *out with the capacity snapshot. 0 or -1. */
int diskfs_df(struct diskfs_df *out);
