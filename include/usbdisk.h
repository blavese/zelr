#pragma once
#include "types.h"

/* A USB stick.
 *
 * Mass storage is not a protocol of its own so much as a way of posting SCSI
 * commands through two bulk endpoints. A command goes out wrapped in thirty
 * one bytes, the data goes whichever way the command said, and thirteen
 * bytes come back saying whether it worked. That is the whole of it, and it
 * is the same on every stick ever made, which is why one driver covers all
 * of them.
 *
 * Kept apart from blockdev.c on purpose. That picks one disk at boot and
 * hides which it was; this is a disk that arrives later, may be pulled out
 * mid sector, and is never the one the system booted from. */

bool usbdisk_present(void);
u32  usbdisk_sectors(void);
u32  usbdisk_block_size(void);
const char *usbdisk_model(void);

/* Both take and return whole sectors, and both are synchronous. */
bool usbdisk_read(u32 lba, u32 count, void *buf);
bool usbdisk_write(u32 lba, u32 count, const void *buf);

/* Called by the enumerator when it finds a bulk only mass storage interface
   it has already opened both endpoints on. */
bool usbdisk_attach(u8 slot, u8 in_dci, u8 out_dci);

/* And when whatever it was hanging off went away. */
void usbdisk_detach(u8 slot);

/* Which disk number it was given, or BLK_NONE when there is no stick. */
u32 usbdisk_blk_id(void);
