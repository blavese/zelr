#pragma once
#include "types.h"

/* NVMe.
 *
 * The third disk driver, and the one that matters on a machine bought this
 * decade. AHCI was SATA's way of talking to a controller that was designed
 * around a spinning platter and a cable; NVMe is what a solid state disk
 * bolted to PCIe looks like when nobody is pretending there is a cable.
 *
 * Structurally it is closer to the network cards here than to AHCI. There
 * are no ports and no device registers to poke per command: there are queues
 * in ordinary memory, the driver writes a 64 byte command into one and rings
 * a doorbell, and the controller writes a 16 byte completion into another
 * when it is done. Everything else is which queue and which command.
 *
 * Two pairs of queues are needed. The admin pair is created by hand through
 * controller registers and is used to ask what the device is and to create
 * the second pair; the I/O pair does the actual reading and writing. */

bool nvme_init(void);
bool nvme_present(void);
u32  nvme_sectors(void);
u32  nvme_max_run(void);             /* sectors this will take at once */
const char *nvme_model(void);

bool nvme_read(u32 lba, u32 count, void *buf);
bool nvme_write(u32 lba, u32 count, const void *buf);
bool nvme_flush(void);
