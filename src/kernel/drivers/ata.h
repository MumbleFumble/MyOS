#pragma once
#include <stdint.h>

/* Read `count` 512-byte sectors starting at LBA `lba` from the primary
 * master ATA device into `buf`.
 * Returns 0 on success, -1 if the drive is absent or a read error occurs. */
int ata_read(uint32_t lba, uint32_t count, uint8_t *buf);

/* Returns 1 if a primary master ATA disk is detected, 0 otherwise. */
int ata_detect(void);
