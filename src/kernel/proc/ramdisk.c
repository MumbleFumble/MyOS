#include "ramdisk.h"

static ramdisk_entry_t rd_table[RAMDISK_MAX_ENTRIES];
static int             rd_count = 0;

static int rd_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void rd_memcpy(void *dst, const void *src, int n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

int ramdisk_register(const char *name, const uint8_t *data, uint64_t size)
{
    if (rd_count >= RAMDISK_MAX_ENTRIES) return -1;
    int len = rd_strlen(name);
    if (len >= RAMDISK_MAX_NAME) return -1;

    ramdisk_entry_t *e = &rd_table[rd_count++];
    rd_memcpy(e->name, name, len + 1);
    e->data = data;
    e->size = size;
    return 0;
}

const ramdisk_entry_t *ramdisk_find(const char *name)
{
    int len = rd_strlen(name);
    for (int i = 0; i < rd_count; i++) {
        /* strcmp */
        const char *a = rd_table[i].name;
        const char *b = name;
        int match = 1;
        for (int j = 0; j <= len; j++) {
            if (a[j] != b[j]) { match = 0; break; }
        }
        if (match) return &rd_table[i];
    }
    return 0;
}
