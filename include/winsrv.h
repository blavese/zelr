#pragma once
#include "types.h"
#include "wm.h"
#include "paging.h"

/* The window server: the half of the window manager that ring 3 can reach.
   It hands out window handles rather than pointers, and maps a window's
   pixels into the calling program's address space. */

#define WINSRV_MAX 8

/* Where a program's first surface appears in its own address space. Inside
   user space, well clear of where a program's own code and stack go.

   The step has to be at least as large as the biggest surface, or one
   window's pages would run into the next window's base. The largest this
   accepts is 2560x1600, 15.6 MiB, so sixteen covers it; eight windows take
   128 MiB of a program's address space, above everything else in it.

   It was 1600x1200 in 8 MiB, which is smaller than the 1920x1080 screen
   Settings offers: maximising a program's window there was refused. */
#define WINSRV_MAX_W 2560
#define WINSRV_MAX_H 1600
#define WINSRV_SURFACE_BASE (USER_SPACE_BASE + 0x60000000ull)
#define WINSRV_SURFACE_STEP 0x01000000ull

void winsrv_init(void);

int  winsrv_create(u32 pid, const char *title, int cw, int ch);

/* Gives a window a different size. The surface is reallocated and remapped
   at the same address the program already has, so its pointer stays good,
   but everything it drew is gone and it is told to draw again.

   Only windows whose owner asked to be resizable are ever resized, because
   a program that does not expect it would carry on writing to the size it
   last saw. */
bool winsrv_resize(u32 pid, int handle, int cw, int ch);
bool winsrv_allow_resize(u32 pid, int handle);

/* The window's program has a use for Escape (SYS_WIN_ESCAPE). */
bool winsrv_want_escape(u32 pid, int handle);

/* The window manager's way in, for a window it is resizing itself. This
   only records the size; see the note on want_cw in wm.h. */
bool winsrv_resize_window(window_t *w, int cw, int ch);

/* Frees surfaces that have been replaced. Called by the window manager at a
   point where it is holding no pointer into one. */
void winsrv_reap_retired(void);
u64  winsrv_surface(u32 pid, int handle, u64 dir);   /* user address, or 0 */
int  winsrv_size(u32 pid, int handle);               /* cw << 16 | ch, or -1 */
bool winsrv_poll(u32 pid, int handle, wm_event_t *out);
bool winsrv_commit(u32 pid, int handle);
bool winsrv_close(u32 pid, int handle);

/* The window behind a handle, for kernel-side callers. Null if the handle is
   not this program's, or the window has already gone. */
window_t *winsrv_window(u32 pid, int handle);

/* Drops every window a task owned. Called when the task goes away, so a
   program that crashes does not leave a dead window on the desktop. */
void winsrv_release(u32 pid);
