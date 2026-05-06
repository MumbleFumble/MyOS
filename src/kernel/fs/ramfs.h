#pragma once

#include "vfs.h"

/*
 * In-memory read-only filesystem backed by statically registered blobs.
 * ELF executables registered here are also accessible via vfs_open() so
 * user programs can read their own binary if needed.
 */

/* Register a named blob.  name must be < VFS_NAME_MAX chars.
 * Returns 0 on success, -1 if the table is full or name is too long. */
int ramfs_add(const char *name, const uint8_t *data, uint64_t size);

/* Same as ramfs_add but the entry is hidden from readdir (ls).
 * Use for ELF executables that should be exec-able but not listed. */
int ramfs_add_hidden(const char *name, const uint8_t *data, uint64_t size);

/* Open a file by exact name.  Returns a kmalloc'd vfs_node, or NULL. */
vfs_node_t *ramfs_open(const char *name);

/* Enumerate entries by 0-based index into name_out[VFS_NAME_MAX].
 * Returns 0 on success, -1 if index is out of range. */
int ramfs_readdir(uint32_t index, char name_out[VFS_NAME_MAX]);
