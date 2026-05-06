#pragma once
#include "vfs.h"

/* Mount the MyFS filesystem from the primary ATA disk.
 * Returns 0 on success, -1 if no disk or wrong magic. */
int diskfs_mount(void);

/* Open a file from the disk filesystem. Returns NULL if not found. */
vfs_node_t *diskfs_open(const char *name);

/* Enumerate disk filesystem files by index. Returns 0 or -1. */
int diskfs_readdir(uint32_t index, char name_out[VFS_NAME_MAX]);

/* Returns 1 if a disk is mounted. */
int diskfs_is_mounted(void);
