#pragma once
#include "types.h"
#include "idt.h"

/* What the machine was doing when it stopped.
 *
 * On a laptop there is no serial port and no debugger. If the kernel dies
 * three seconds into boot the screen goes black and that is the whole of the
 * evidence. Everything else in this project can be tested under QEMU, where
 * the serial line carries the story out; the first boot on real hardware is
 * the one run where none of that is available, and it is also the run most
 * likely to fail.
 *
 * So the kernel narrates itself into a buffer that survives what killed it,
 * and there are three ways to get the buffer back:
 *
 *   serial   mirrored as it is written, free, works wherever a port exists
 *   screen   painted on panic, large enough to photograph
 *   disk     written to reserved sectors, survives the power cycle
 *
 * The screen is the one that matters on a laptop, and the disk is the one
 * that matters when the screen never came up at all.
 *
 * This has to work before paging, before the heap, and before any driver, so
 * the buffer is static and nothing here allocates. */

#define BB_SECTORS 32                    /* 16 KiB, in the reserved area */
/* Where in the reserved area it starts depends on the width of the volume,
   because FAT32 keeps three things of its own in there at addresses other
   readers expect. fat_boot_log_lba answers it from the boot sector; this is
   the FAT16 answer and the smallest it can be. */
#define BB_LBA_MIN 1
#define BB_BYTES   (BB_SECTORS * 512)
#define BB_MAGIC   0x5842594Eu           /* "NYBX" */

void bb_init(void);

/* A line in the log. Mirrored to serial as it is written, so a machine with
   a port does not have to survive to the end to tell you anything. */
void bb_log(const char *fmt, ...);

/* A boot phase. Separate from bb_log because these are what you read first
   when the machine hangs: the last mark is the thing that did not finish. */
void bb_mark(const char *phase);

/* Registers, at the point a fault was taken. */
void bb_fault(registers_t *r, const char *what);

/* Paints the tail of the log where a camera can see it. Uses the
   framebuffer if one came up, the text console if not. */
void bb_screen(void);

/* Lifts the previous boot's record off the disk and into memory. Must run
   before anything writes a new one, and so before bb_flush: this boot is
   about to overwrite the sectors the last boot left behind, and the last
   boot's record is the entire reason someone rebooted. Call it as soon as
   there is a disk to read. */
void bb_recover(void);

/* Writes the log to the reserved sectors, and only ever to a volume this
   kernel formatted. Returns false if it refused, which is the normal answer
   on somebody else's disk. */
bool bb_flush(void);

/* The log this boot has produced so far. */
const char *bb_text(void);
u32         bb_len(void);

/* The log from the boot before this one, as bb_recover found it. Returns 0
   if there was none, or if the disk holds someone else's data. */
u32 bb_prev(char *out, u32 cap);

/* Which boot this is, counted on the volume. */
u32 bb_boot_number(void);
