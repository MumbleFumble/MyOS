#include "../fs/vfs.h"
#include "../fs/myfs.h"
#include "../drivers/ata.h"
#include "../mem/kheap.h"

/* Cache the superblock and per-file sector offsets at mount time */
#define DISKFS_MAX_FILES 64

typedef struct {
    char     name[MYFS_NAME_MAX];
    uint64_t size;
    uint32_t data_lba;   /* first sector of file data on disk */
} diskfs_entry_t;

static diskfs_entry_t disk_table[DISKFS_MAX_FILES];
static int            disk_count   = 0;
static int            disk_mounted = 0;

/* -----------------------------------------------------------------------
 * vfs_ops for a disk file
 * ----------------------------------------------------------------------- */

typedef struct {
    int      entry_idx;
    uint32_t record_lba;    /* first sector of this file's record */
    /* data starts at byte 80 within sector record_lba */
} diskfs_priv_t;

static int64_t diskfs_read_op(vfs_node_t *node, uint64_t offset,
                               uint64_t len, uint8_t *buf)
{
    diskfs_priv_t  *priv = (diskfs_priv_t *)node->priv;
    diskfs_entry_t *e    = &disk_table[priv->entry_idx];

    if (offset >= e->size) return 0;
    uint64_t avail = e->size - offset;
    if (len > avail) len = avail;

    /* Data starts at byte 80 within sector record_lba (after the file header).
     * Map file offset -> disk byte offset: disk_off = 80 + offset */
    uint64_t disk_off = 80 + offset;

    uint32_t first_sect = (uint32_t)(disk_off / 512);
    uint32_t last_sect  = (uint32_t)((disk_off + len - 1) / 512);
    uint32_t n_sects    = last_sect - first_sect + 1;

    uint8_t *tmp = (uint8_t *)kmalloc(n_sects * 512);
    if (!tmp) return -1;

    if (ata_read(priv->record_lba + first_sect, n_sects, tmp) != 0) {
        kfree(tmp);
        return -1;
    }

    uint64_t src_off = disk_off - (uint64_t)first_sect * 512;
    for (uint64_t i = 0; i < len; i++)
        buf[i] = tmp[src_off + i];

    kfree(tmp);
    return (int64_t)len;
}

static void diskfs_close_op(vfs_node_t *node)
{
    kfree(node->priv);
    kfree(node);
}

static int diskfs_readdir_op(vfs_node_t *dir, uint32_t index,
                              char name_out[VFS_NAME_MAX])
{
    (void)dir;
    if ((int)index >= disk_count) return -1;
    int len = 0;
    while (disk_table[index].name[len]) len++;
    for (int i = 0; i <= len; i++) name_out[i] = disk_table[index].name[i];
    return 0;
}

static vfs_ops_t diskfs_ops = {
    .read    = diskfs_read_op,
    .close   = diskfs_close_op,
    .readdir = diskfs_readdir_op,
};

/* -----------------------------------------------------------------------
 * Mount: read superblock + build in-memory directory
 * ----------------------------------------------------------------------- */

int diskfs_mount(void)
{
    if (!ata_detect()) return -1;

    /* Read superblock (sector 0) */
    uint8_t sector[512];
    if (ata_read(0, 1, sector) != 0) return -1;

    myfs_super_t *sb = (myfs_super_t *)sector;
    if (sb->magic != MYFS_MAGIC) return -1;

    uint32_t count = sb->file_count;
    if (count > DISKFS_MAX_FILES) count = DISKFS_MAX_FILES;

    /* Walk file records starting at sector 1 */
    uint32_t cur_lba = 1;
    uint8_t  hdr_buf[512];

    for (uint32_t i = 0; i < count; i++) {
        if (ata_read(cur_lba, 1, hdr_buf) != 0) break;
        myfs_file_hdr_t *fh = (myfs_file_hdr_t *)hdr_buf;

        int nlen = 0;
        while (nlen < MYFS_NAME_MAX - 1 && fh->name[nlen]) nlen++;
        for (int j = 0; j <= nlen; j++) disk_table[i].name[j] = fh->name[j];
        disk_table[i].size     = fh->size;
        disk_table[i].data_lba = cur_lba;   /* data is at byte 80 within this LBA */

        /* Advance: record = 80-byte hdr + data, padded to 512-byte boundary */
        uint32_t record_sects = (80 + (uint32_t)fh->size + 511) / 512;
        cur_lba += record_sects;
        disk_count++;
    }

    disk_mounted = 1;
    return 0;
}

/* -----------------------------------------------------------------------
 * Open a file from the disk filesystem
 * ----------------------------------------------------------------------- */

vfs_node_t *diskfs_open(const char *name)
{
    if (!disk_mounted) return 0;

    int nlen = 0;
    while (name[nlen]) nlen++;

    for (int i = 0; i < disk_count; i++) {
        const char *a = disk_table[i].name;
        int match = 1;
        for (int j = 0; j <= nlen; j++) {
            if (a[j] != name[j]) { match = 0; break; }
        }
        if (!match) continue;

        vfs_node_t    *n    = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
        diskfs_priv_t *priv = (diskfs_priv_t *)kmalloc(sizeof(diskfs_priv_t));
        if (!n || !priv) { kfree(n); kfree(priv); return 0; }

        for (int j = 0; j <= nlen; j++) n->name[j] = disk_table[i].name[j];
        n->type         = VFS_TYPE_FILE;
        n->size         = disk_table[i].size;
        n->ops          = &diskfs_ops;
        priv->entry_idx = i;
        priv->record_lba = disk_table[i].data_lba;
        n->priv         = priv;
        return n;
    }
    return 0;
}

int diskfs_readdir(uint32_t index, char name_out[VFS_NAME_MAX])
{
    if (!disk_mounted || (int)index >= disk_count) return -1;
    int len = 0;
    while (disk_table[index].name[len]) len++;
    for (int i = 0; i <= len; i++) name_out[i] = disk_table[index].name[i];
    return 0;
}

int diskfs_is_mounted(void) { return disk_mounted; }
