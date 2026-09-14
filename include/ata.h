#pragma once
#include "types.h"

#include "blockdev.h"   /* SECTOR_SIZE lives with the block layer */

bool ata_init(void);
bool ata_present(void);
u32  ata_sectors(void);                 /* total addressable sectors */
u32  ata_max_run(void);                 /* sectors this will take at once */
const char *ata_model(void);
bool ata_read(u32 lba, u32 count, void *buf);
bool ata_write(u32 lba, u32 count, const void *buf);
bool ata_flush(void);
