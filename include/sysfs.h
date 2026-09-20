#pragma once
#include "types.h"

/* The live tree: files that are not stored anywhere.
 *
 * Reading one of these calls a function that writes the answer at that
 * moment. There is nothing on disk and nothing in memory holding a copy, so
 * a file here is never stale and never needs updating when the thing it
 * describes changes.
 *
 * This is what /sys is. It is also how /bin works: the built-in programs are
 * files in the kernel image rather than on the volume, and putting them in a
 * directory of their own keeps them out of the root where the user's files
 * live.
 *
 * The VFS checks here before either of the real backends, so these paths
 * cannot be shadowed by a file somebody writes on the disk. */

/* How many built-in programs there is room for.
 *
 * Public, and asserted against in kernel/builtin.c, because this number was
 * private and the table outgrew it: registering the seventeenth program
 * returned quietly, the last one in the list was the browser, and /bin
 * simply did not have a browser in it any more. Nothing said so — not the
 * build, not the boot, not the kernel's own checks. A screenshot did, two
 * hundred checks later.
 *
 * So the number is here, where the table that has to fit in it can see it,
 * and the fit is now a compile error rather than a program that vanishes. */
#define SYSFS_MAX_PROGRAMS 48

/* Fills out with text and returns how many bytes it wrote. Called with a
   buffer of at least SYSFS_MAX bytes. */
#define SYSFS_MAX 4096
typedef u32 (*sysfs_render_t)(char *out, u32 cap);

void sysfs_init(void);

/* True when this absolute path is inside the live tree, whether or not
   anything is actually there. The VFS uses this to decide that a write must
   be refused rather than falling through to the disk. */
bool sysfs_owns(const char *abs);

bool sysfs_stat(const char *abs, u32 *size_out, bool *dir_out);
int  sysfs_read(const char *abs, void *buf, u32 cap);
int  sysfs_list(const char *abs, u32 index, char *name_out, u32 *size_out, bool *dir_out);

/* Registers a program that lives in the kernel image, so it appears in
   /bin and can be read and run like anything else. */
void sysfs_add_program(const char *name, const u8 *data, u32 size);
u32  sysfs_program_count(void);
