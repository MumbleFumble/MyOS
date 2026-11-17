#include "../fs/vfs.h"
#include "../fs/myfs.h"
#include "../drivers/ata.h"
#include "bcache.h"
#include "../mem/kheap.h"

/* Cache the superblock and per-file sector offsets at mount time */
#define DISKFS_MAX_FILES 64

typedef struct {
    char     name[MYFS_NAME_MAX];
    uint64_t size;
    uint32_t data_lba;   /* first sector of file data on disk */
    uint8_t  deleted;    /* latest record is a tombstone (inc.3) */
} diskfs_entry_t;

static diskfs_entry_t disk_table[DISKFS_MAX_FILES];
static int            disk_count   = 0;
static int            disk_mounted = 0;

/* Append-only write state (Phase 6 inc.3). Records are appended at
 * disk_append_lba; the tail of the disk (2048 sectors, matching
 * RAW_SCRATCH_SECTORS in syscall.c) is reserved as test scratch
 * and never handed to the filesystem. */
static uint32_t disk_append_lba   = 0;
static uint32_t disk_record_count = 0;  /* records on disk (superblock) */
static uint32_t disk_append_limit = 0;

static int diskfs_name_eq(const char *a, const char *b)
{
    for (int j = 0; j < MYFS_NAME_MAX; j++) {
        if (a[j] != b[j]) return 0;
        if (!a[j]) return 1;
    }
    return 1;
}

static int diskfs_find(const char *name)
{
    for (int i = 0; i < disk_count; i++)
        if (diskfs_name_eq(disk_table[i].name, name)) return i;
    return -1;
}

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

    /* Read whole sectors straight into the caller's buffer; use a small
     * stack bounce buffer only for partial head/tail sectors. This avoids
     * the per-read kmalloc that used to leak the kernel heap (kfree is
     * currently a no-op bump allocator), which failed reads past ~7 MiB. */
    uint8_t bounce[512];
    uint64_t done = 0;
    for (uint32_t s = first_sect; s <= last_sect; s++) {
        uint64_t sect_start = (uint64_t)s * 512;
        uint64_t copy_start = disk_off > sect_start ? disk_off : sect_start;
        uint64_t sect_end   = sect_start + 512;
        uint64_t copy_end   = disk_off + len < sect_end ? disk_off + len : sect_end;
        uint64_t chunk = copy_end - copy_start;
        /* File context for the prefetch policy (Phase 3 inc.9):
         * file_id = directory entry index + 1 (0 = no file), file_off =
         * sector index within the file's record. */
        if (chunk == 512) {
            if (bcache_read_ctx(priv->record_lba + s, buf + done,
                                (uint32_t)priv->entry_idx + 1, s) != 0)
                return -1;
        } else {
            if (bcache_read_ctx(priv->record_lba + s, bounce,
                                (uint32_t)priv->entry_idx + 1, s) != 0)
                return -1;
            uint64_t b_off = copy_start - sect_start;
            for (uint64_t i = 0; i < chunk; i++)
                buf[done + i] = bounce[b_off + i];
        }
        done += chunk;
    }
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

/* -----------------------------------------------------------------------
 * Writes (Phase 6 inc.3): append-only log over the packed format.
 * Rewriting a file tombstones its old record and appends the new
 * full content at disk_append_lba; the superblock record count is
 * rewritten on every append. Every write ends with bcache_sync(),
 * so a completed write is durable. Whole-file read-modify-append
 * in a kmalloc buffer caps files at DISKFS_MAX_FILE_BYTES (the
 * bump-allocator heap leaks the buffer per write: fine at shell
 * scale, a real allocator is future work).
 * ----------------------------------------------------------------------- */
#define DISKFS_MAX_FILE_BYTES (256 * 1024)

static int diskfs_write_super(void)
{
    uint8_t sec[512];
    if (bcache_read(0, sec) != 0) return -1;
    myfs_super_t *sb = (myfs_super_t *)sec;
    sb->magic      = MYFS_MAGIC;
    sb->file_count = disk_record_count;
    return bcache_write(0, sec);
}

static int diskfs_tombstone(uint32_t record_lba)
{
    uint8_t sec[512];
    if (bcache_read(record_lba, sec) != 0) return -1;
    sec[72] |= MYFS_FLAG_DELETED;   /* _pad[0]: 64 name + 8 size */
    return bcache_write(record_lba, sec);
}

/* Append one record (header + data) at disk_append_lba. Returns
 * the record LBA, or -1 (no space / write error). */
static int64_t diskfs_append_record(const char *name,
                                    const uint8_t *data, uint64_t size)
{
    uint32_t sects = (uint32_t)((80 + size + 511) / 512);
    if (disk_append_lba + sects > disk_append_limit) return -1;
    uint32_t start = disk_append_lba;
    uint8_t bounce[512];
    for (uint32_t s = 0; s < sects; s++) {
        uint64_t rstart = (uint64_t)s * 512;
        if (s == 0) {
            for (int i = 0; i < 512; i++) bounce[i] = 0;
            myfs_file_hdr_t *fh = (myfs_file_hdr_t *)bounce;
            int j = 0;
            while (name[j] && j < MYFS_NAME_MAX - 1) {
                fh->name[j] = name[j]; j++;
            }
            fh->size = size;
            uint64_t n = size < 432 ? size : 432;
            for (uint64_t i = 0; i < n; i++) bounce[80 + i] = data[i];
            if (bcache_write(start + s, bounce) != 0) return -1;
        } else {
            uint64_t doff = rstart - 80;
            uint64_t n = (80 + size) - rstart;
            if (n > 512) n = 512;
            if (n == 512) {
                if (bcache_write(start + s, data + doff) != 0)
                    return -1;
            } else {
                for (int i = 0; i < 512; i++) bounce[i] = 0;
                for (uint64_t i = 0; i < n; i++)
                    bounce[i] = data[doff + i];
                if (bcache_write(start + s, bounce) != 0) return -1;
            }
        }
    }
    disk_append_lba += sects;
    disk_record_count++;
    if (diskfs_write_super() != 0) return -1;
    return (int64_t)start;
}

static int64_t diskfs_write_op(vfs_node_t *node, uint64_t offset,
                               uint64_t len, uint8_t *buf)
{
    diskfs_priv_t  *priv = (diskfs_priv_t *)node->priv;
    diskfs_entry_t *e    = &disk_table[priv->entry_idx];
    if (e->deleted || !disk_mounted) return -1;

    uint64_t new_size = e->size;
    if (offset + len > new_size) new_size = offset + len;
    if (new_size > DISKFS_MAX_FILE_BYTES) return -1;

    uint8_t *content = (uint8_t *)kmalloc(new_size ? new_size : 1);
    if (!content) return -1;
    for (uint64_t i = 0; i < new_size; i++) content[i] = 0;
    if (e->size) {
        /* Current content comes from the OLD record, before it is
         * replaced below. */
        if (diskfs_read_op(node, 0, e->size, content) < 0) return -1;
    }
    for (uint64_t i = 0; i < len; i++) content[offset + i] = buf[i];

    uint32_t old_lba = priv->record_lba;
    int64_t nl = diskfs_append_record(e->name, content, new_size);
    if (nl < 0) return -1;
    if (old_lba) diskfs_tombstone(old_lba);  /* new record wins anyway */
    e->size          = new_size;
    e->data_lba      = (uint32_t)nl;
    priv->record_lba = (uint32_t)nl;
    node->size       = new_size;
    if (bcache_sync() != 0) return -1;
    return (int64_t)len;
}

static vfs_ops_t diskfs_ops = {
    .read    = diskfs_read_op,
    .write   = diskfs_write_op,
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

    /* Walk ALL records (the superblock count includes tombstones
     * and superseded records). The table holds one entry per name;
     * the latest live record wins. */
    uint32_t count = sb->file_count;
    disk_record_count = count;
    uint32_t total = ata_total_sectors();
    disk_append_limit = (total > 2048 + 1024) ? total - 2048 : 0;

    uint32_t cur_lba = 1;
    uint8_t  hdr_buf[512];

    for (uint32_t i = 0; i < count; i++) {
        if (disk_append_limit && cur_lba >= disk_append_limit) break;
        if (ata_read(cur_lba, 1, hdr_buf) != 0) break;
        myfs_file_hdr_t *fh = (myfs_file_hdr_t *)hdr_buf;
        uint32_t record_sects = (80 + (uint32_t)fh->size + 511) / 512;

        if (fh->_pad[0] & MYFS_FLAG_DELETED) {
            int idx = diskfs_find(fh->name);
            if (idx >= 0) disk_table[idx].deleted = 1;
        } else {
            int idx = diskfs_find(fh->name);
            if (idx < 0 && disk_count < DISKFS_MAX_FILES) {
                idx = disk_count++;
                int nlen = 0;
                while (nlen < MYFS_NAME_MAX - 1 && fh->name[nlen]) nlen++;
                for (int j = 0; j <= nlen; j++)
                    disk_table[idx].name[j] = fh->name[j];
            }
            if (idx >= 0) {
                disk_table[idx].size     = fh->size;
                disk_table[idx].data_lba = cur_lba;
                disk_table[idx].deleted  = 0;
            }
        }
        cur_lba += record_sects;
    }
    disk_append_lba = cur_lba;
    if (!disk_append_limit) disk_append_limit = cur_lba;

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
        if (!match || disk_table[i].deleted) continue;

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

static vfs_node_t *diskfs_make_node(int entry_idx)
{
    vfs_node_t    *n    = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    diskfs_priv_t *priv = (diskfs_priv_t *)kmalloc(sizeof(diskfs_priv_t));
    if (!n || !priv) { kfree(n); kfree(priv); return 0; }
    int len = 0;
    while (disk_table[entry_idx].name[len]) len++;
    for (int j = 0; j <= len; j++) n->name[j] = disk_table[entry_idx].name[j];
    n->type          = VFS_TYPE_FILE;
    n->size          = disk_table[entry_idx].size;
    n->ops           = &diskfs_ops;
    priv->entry_idx  = entry_idx;
    priv->record_lba = disk_table[entry_idx].data_lba;
    n->priv          = priv;
    return n;
}

/* Create (or truncate) a file: append an empty record for it and
 * open it. Truncation tombstones the previous record. */
vfs_node_t *diskfs_create(const char *name)
{
    if (!disk_mounted) return 0;
    int nlen = 0;
    while (name[nlen]) nlen++;
    if (nlen == 0 || nlen >= MYFS_NAME_MAX) return 0;

    uint8_t empty = 0;
    int idx = diskfs_find(name);
    if (idx < 0) {
        if (disk_count >= DISKFS_MAX_FILES) return 0;
        int64_t nl = diskfs_append_record(name, &empty, 0);
        if (nl < 0) return 0;
        idx = disk_count++;
        for (int j = 0; j <= nlen; j++) disk_table[idx].name[j] = name[j];
        disk_table[idx].size     = 0;
        disk_table[idx].data_lba = (uint32_t)nl;
        disk_table[idx].deleted  = 0;
    } else {
        int64_t nl = diskfs_append_record(disk_table[idx].name, &empty, 0);
        if (nl < 0) return 0;
        diskfs_tombstone(disk_table[idx].data_lba);
        disk_table[idx].size     = 0;
        disk_table[idx].data_lba = (uint32_t)nl;
        disk_table[idx].deleted  = 0;
    }
    if (bcache_sync() != 0) return 0;
    return diskfs_make_node(idx);
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
