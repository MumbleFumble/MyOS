#include "vfs.h"
#include "ramfs.h"
#include "diskfs.h"

void vfs_init(void)
{
    /* ramfs is always present; diskfs is mounted separately if ATA is detected */
}

vfs_node_t *vfs_open(const char *path)
{
    if (path[0] == '/') path++;
    /* ramfs first (in-kernel blobs), then disk */
    vfs_node_t *n = ramfs_open(path);
    if (n) return n;
    return diskfs_open(path);
}

int64_t vfs_read(vfs_node_t *node, uint64_t offset, uint64_t len, uint8_t *buf)
{
    if (!node || !node->ops || !node->ops->read) return -1;
    return node->ops->read(node, offset, len, buf);
}

int64_t vfs_write(vfs_node_t *node, uint64_t offset, uint64_t len, uint8_t *buf)
{
    if (!node || !node->ops || !node->ops->write) return -1;
    return node->ops->write(node, offset, len, buf);
}

int vfs_unlink(const char *path)
{
    if (!path) return -1;
    if (path[0] == '/') path++;
    return diskfs_delete(path);
}

int vfs_rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return -1;
    if (old_path[0] == '/') old_path++;
    if (new_path[0] == '/') new_path++;
    return diskfs_rename(old_path, new_path);
}

vfs_node_t *vfs_create(const char *path)
{
    if (path[0] == '/') path++;
    /* Only diskfs is writable; ramfs holds in-kernel blobs. */
    return diskfs_create(path);
}

void vfs_close(vfs_node_t *node)
{
    if (!node) return;
    if (node->ops && node->ops->close) node->ops->close(node);
}

int vfs_readdir(uint32_t index, char name_out[VFS_NAME_MAX])
{
    /* Return ramfs entries first, then diskfs entries */
    uint32_t ramfs_count = 0;
    char tmp[VFS_NAME_MAX];
    while (ramfs_readdir(ramfs_count, tmp) == 0) ramfs_count++;

    if (index < ramfs_count)
        return ramfs_readdir(index, name_out);
    return diskfs_readdir(index - ramfs_count, name_out);
}
