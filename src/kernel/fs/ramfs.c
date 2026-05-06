#include "ramfs.h"
#include "../mem/kheap.h"

#define RAMFS_MAX_FILES 32

typedef struct {
    char           name[VFS_NAME_MAX];
    const uint8_t *data;
    uint64_t       size;
    int            hidden;   /* 1 = skip in readdir (ls), still openable */
} ramfs_entry_t;

static ramfs_entry_t fs_table[RAMFS_MAX_FILES];
static int           fs_count = 0;

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

static int rf_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void rf_memcpy(void *dst, const void *src, int n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

/* -----------------------------------------------------------------------
 * vfs_ops implementations
 * ----------------------------------------------------------------------- */

static int64_t ramfs_read_op(vfs_node_t *node, uint64_t offset,
                              uint64_t len, uint8_t *buf)
{
    ramfs_entry_t *e = (ramfs_entry_t *)node->priv;
    if (offset >= e->size) return 0;
    uint64_t avail = e->size - offset;
    if (len > avail) len = avail;
    rf_memcpy(buf, e->data + offset, (int)len);
    return (int64_t)len;
}

static void ramfs_close_op(vfs_node_t *node)
{
    kfree(node);   /* node was kmalloc'd by ramfs_open */
}

static int ramfs_readdir_op(vfs_node_t *dir, uint32_t index,
                             char name_out[VFS_NAME_MAX])
{
    (void)dir;
    return ramfs_readdir(index, name_out);
}

static vfs_ops_t ramfs_ops = {
    .read    = ramfs_read_op,
    .close   = ramfs_close_op,
    .readdir = ramfs_readdir_op,
};

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

static int ramfs_add_internal(const char *name, const uint8_t *data,
                               uint64_t size, int hidden)
{
    if (fs_count >= RAMFS_MAX_FILES) return -1;
    int len = rf_strlen(name);
    if (len >= VFS_NAME_MAX) return -1;

    ramfs_entry_t *e = &fs_table[fs_count++];
    rf_memcpy(e->name, name, len + 1);
    e->data   = data;
    e->size   = size;
    e->hidden = hidden;
    return 0;
}

int ramfs_add(const char *name, const uint8_t *data, uint64_t size)
{
    return ramfs_add_internal(name, data, size, 0);
}

int ramfs_add_hidden(const char *name, const uint8_t *data, uint64_t size)
{
    return ramfs_add_internal(name, data, size, 1);
}

vfs_node_t *ramfs_open(const char *name)
{
    int len = rf_strlen(name);
    for (int i = 0; i < fs_count; i++) {
        const char *a = fs_table[i].name;
        int match = 1;
        for (int j = 0; j <= len; j++) {
            if (a[j] != name[j]) { match = 0; break; }
        }
        if (!match) continue;

        vfs_node_t *n = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
        if (!n) return 0;
        rf_memcpy(n->name, fs_table[i].name, len + 1);
        n->type = VFS_TYPE_FILE;
        n->size = fs_table[i].size;
        n->ops  = &ramfs_ops;
        n->priv = &fs_table[i];
        return n;
    }
    return 0;
}

int ramfs_readdir(uint32_t index, char name_out[VFS_NAME_MAX])
{
    /* Skip hidden entries; caller uses sequential 0-based visible index */
    uint32_t visible = 0;
    for (int i = 0; i < fs_count; i++) {
        if (fs_table[i].hidden) continue;
        if (visible == index) {
            int len = rf_strlen(fs_table[i].name);
            rf_memcpy(name_out, fs_table[i].name, len + 1);
            return 0;
        }
        visible++;
    }
    return -1;
}
