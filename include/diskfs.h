#pragma once
#include "types.h"

/* Bringing the disk up. The filesystem itself is FAT and lives in fat.c;
   this is only the part that decides whether there is a usable volume and
   prepares one if there is not. */

bool diskfs_available(void);

/* A stick, mounted at /usb. Tries a partition table first and then the whole
   device, because sticks come both ways. */
bool diskfs_mount_removable(u32 dev);
void diskfs_unmount_removable(void);
bool diskfs_removable_mounted(void);
bool diskfs_mounted(void);
bool diskfs_format(void);

/* Mounts the volume and recovers anything a crash left stranded. Returns the
   number of entries in the root, -1 with no disk, -2 if it is not FAT16. */
int  diskfs_mount(void);

/* Pushes anything the drivers are still holding out to the platter. */
bool diskfs_flush(void);
