#pragma once

#include <stdint.h>

#define VFS_TYPE_FILE  0
#define VFS_TYPE_DIR   1
#define VFS_NAME_MAX   64

typedef struct vfs_node vfs_node_t;

typedef struct {
    /* Read up to len bytes starting at offset into buf.
     * Returns bytes read, 0 at EOF, negative on error. */
    int64_t (*read)   (vfs_node_t *node, uint64_t offset, uint64_t len, uint8_t *buf);
    /* Release the node (typically kfree's it). */
    void    (*close)  (vfs_node_t *node);
    /* Enumerate directory entries by 0-based index.
     * Writes null-terminated name into name_out[VFS_NAME_MAX].
     * Returns 0 on success, -1 if index out of range. */
    int     (*readdir)(vfs_node_t *dir, uint32_t index, char name_out[VFS_NAME_MAX]);
} vfs_ops_t;

struct vfs_node {
    char       name[VFS_NAME_MAX];
    uint32_t   type;   /* VFS_TYPE_FILE or VFS_TYPE_DIR */
    uint64_t   size;
    vfs_ops_t *ops;
    void      *priv;   /* fs-specific private data */
};

/*
 * Initialise the VFS and attach the ramfs as the root filesystem.
 * Must be called after ramfs_init().
 */
void vfs_init(void);

/*
 * Open a file by path.  Leading '/' is stripped; the rest is treated as a
 * flat filename in the root ramfs.
 * Returns a kmalloc'd node on success, NULL if not found.
 * Caller must call vfs_close() when done.
 */
vfs_node_t *vfs_open   (const char *path);
int64_t     vfs_read   (vfs_node_t *node, uint64_t offset, uint64_t len, uint8_t *buf);
void        vfs_close  (vfs_node_t *node);

/*
 * Enumerate the root directory by index.
 * name_out must be at least VFS_NAME_MAX bytes.
 * Returns 0 on success, -1 if index is past the last entry.
 */
int vfs_readdir(uint32_t index, char name_out[VFS_NAME_MAX]);
