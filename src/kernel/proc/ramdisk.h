#pragma once

#include <stdint.h>

/*
 * Simple in-kernel ramdisk: a table of named ELF blobs embedded at link time.
 * Programs are registered at boot via ramdisk_register() and looked up by name
 * via ramdisk_find().
 */

#define RAMDISK_MAX_ENTRIES  16
#define RAMDISK_MAX_NAME     32

typedef struct {
    char           name[RAMDISK_MAX_NAME];
    const uint8_t *data;
    uint64_t       size;
} ramdisk_entry_t;

/*
 * Register an ELF blob under a name.  Returns 0 on success, -1 if the table
 * is full or name is too long.
 */
int ramdisk_register(const char *name, const uint8_t *data, uint64_t size);

/*
 * Look up a program by name.  Returns a pointer to the entry, or NULL if not
 * found.
 */
const ramdisk_entry_t *ramdisk_find(const char *name);
