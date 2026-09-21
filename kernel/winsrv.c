/* The window server.
 *
 * A graphical program in ring 3 cannot touch the framebuffer and cannot be
 * handed a kernel pointer, so the surface it draws into has to be mapped
 * into its own address space. That is the whole job here:
 *
 *   - a window's pixels are allocated page aligned, so whole pages can be
 *     handed out without exposing anything else that shares them
 *   - those pages are mapped into the caller's directory with PTE_USER, at a
 *     fixed address the program is told about
 *   - the program draws into that memory directly, then asks for a repaint
 *
 * The kernel keeps the window; the program only ever gets a small integer
 * handle and a pointer to its own pixels. Nothing else crosses over.
 */
#include "winsrv.h"
#include "wm.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "sched.h"
#include "string.h"
#include "printf.h"
#include "io.h"
#include "fb.h"

typedef struct {
    bool      used;
    u32       pid;             /* who owns it */
    window_t *win;
    u32      *raw;             /* what kmalloc returned, for kfree */
    u32      *pixels;          /* page aligned inside raw, and what is mapped */

    /* --- the one the desktop reads ---------------------------------------
     *
     * There was one surface. The program drew into it and the compositor
     * read it, the same memory, at whatever moment the compositor happened
     * to run -- and win_commit did not swap anything, it only said the
     * window had changed. So a program whose frame begins by painting over
     * everything genuinely contained, for a while, a background and nothing
     * else. The compositor is a task like any other and it ran in that gap.
     *
     * What that looked like was a maximised card game with no cards, no
     * seats and no buttons in it, which reads as a drawing fault and was a
     * timing one. Two programs carried a shadow buffer of their own to get
     * round it, which is a workaround every graphical program would have
     * had to repeat.
     *
     * So the window server keeps the second one, and every program gets it.
     * The program draws into `pixels`, which nothing else ever reads;
     * commit copies that into `shown`, which is what the window points at.
     *
     * The copy is not atomic -- three megabytes with the interrupts off
     * would stall the timer for milliseconds, which is a worse fault than
     * the one being fixed. What the compositor can still catch is part of
     * one finished frame and part of another, which for two frames of the
     * same window is nothing at all, and is a different thing from catching
     * a frame that has been erased and not yet redrawn. */
    u32      *shown_raw;
    u32      *shown;

    u64       bytes;           /* rounded up to whole pages */
    u64       user_addr;       /* where the owner sees it, 0 until mapped */
    u64       dir;             /* the address space it was mapped into */
} slot_t;

/* What a frame costs to publish, counted so /sys can be asked. */
static u64 published_frames, published_bytes;

u64 winsrv_frames(void)  { return published_frames; }
u64 winsrv_bytes(void)   { return published_bytes; }

/* The copy itself.
 *
 * A word at a time through volatile pointers, because at -O2 clang
 * recognises this loop and replaces it with a call to memcpy -- which
 * exists here, so it would link, and would then be a call the kernel makes
 * several hundred times a second into a byte mover. The word loop is what
 * is wanted and the volatile is what keeps it. */
static void publish(slot_t *s) {
    if (!s->shown || !s->pixels) return;
    volatile u32 *d = (volatile u32 *)s->shown;
    const volatile u32 *b = (const volatile u32 *)s->pixels;
    u64 n = s->bytes / 4;
    for (u64 i = 0; i < n; i++) d[i] = b[i];
    published_frames++;
    published_bytes += s->bytes;
}

static slot_t slots[WINSRV_MAX];

/* Surfaces that have been swapped out and are waiting to be freed.
 *
 * The swap happens in the owning program's context, and the window manager
 * runs in another task that may be part way through compositing the very
 * buffer being replaced. Freeing it there and then would pull it out from
 * under that read, so it is parked here and released by the manager itself
 * at the top of a frame, where it is holding no pointer into one. */
#define RETIRED_MAX 8
static u32 *retired[RETIRED_MAX];

static void retire(u32 *raw) {
    if (!raw) return;
    for (int i = 0; i < RETIRED_MAX; i++)
        if (!retired[i]) { retired[i] = raw; return; }
    /* Nowhere to park it. Freeing now risks a torn read of one frame;
       leaking would be worse, and eight outstanding is already far more
       than a single resize can produce. */
    kfree(raw);
}

void winsrv_reap_retired(void) {
    for (int i = 0; i < RETIRED_MAX; i++)
        if (retired[i]) { kfree(retired[i]); retired[i] = 0; }
}

void winsrv_init(void) {
    memset(slots, 0, sizeof(slots));
    memset(retired, 0, sizeof(retired));
}

/* Defined below, but every door into the server calls it first. */
static void apply_pending(slot_t *s);

static slot_t *lookup(u32 pid, int handle) {
    if (handle < 0 || handle >= WINSRV_MAX) return 0;
    slot_t *s = &slots[handle];
    if (!s->used || s->pid != pid) return 0;
    return s;
}

/* The window manager calls this when the close button is used. The program
   still owns its handle; it finds out through a WM_EV_CLOSE event. */
static void on_wm_close(window_t *w) {
    for (int i = 0; i < WINSRV_MAX; i++)
        if (slots[i].used && slots[i].win == w) slots[i].win = 0;
}

static void free_slot(slot_t *s) {
    if (s->user_addr && s->dir) {
        /* Named rather than assumed to be the live one. A program killed by
           something else has its windows released from the killer's context,
           and leaving those pages mapped while freeing them would point a
           task at memory that had been handed back. The address space itself
           is still there either way: it is freed when the task is reaped,
           which is always after this. */
        for (u64 off = 0; off < s->bytes; off += PAGE_SIZE)
            unmap_page_in(s->dir, s->user_addr + off);
    }
    if (s->win) { s->win->on_close = 0; wm_close(s->win); }
    if (s->raw) kfree(s->raw);
    if (s->shown_raw) kfree(s->shown_raw);
    memset(s, 0, sizeof(*s));
}

int winsrv_create(u32 pid, const char *title, int cw, int ch) {
    if (cw < 32 || ch < 32 || cw > 1600 || ch > 1200) return -1;

    int handle = -1;
    for (int i = 0; i < WINSRV_MAX; i++)
        if (!slots[i].used) { handle = i; break; }
    if (handle < 0) return -1;

    slot_t *s = &slots[handle];
    u64 bytes = ((u64)(cw * ch) * 4 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Over-allocate and align up. The heap is inside the identity mapped
       region, so an aligned virtual address is an aligned physical one, and
       the pages can be handed to a user directory as they are. */
    u32 *raw = (u32 *)kmalloc(bytes + PAGE_SIZE);
    if (!raw) return -1;
    u32 *pixels = (u32 *)(((u64)raw + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    memset(pixels, 0, bytes);

    /* And the one the desktop reads. Not page aligned for any reason of
       its own -- nothing maps it -- but allocated the same way so the two
       are the same size and the copy between them is one loop. */
    u32 *shown_raw = (u32 *)kmalloc(bytes + PAGE_SIZE);
    if (!shown_raw) { kfree(raw); return -1; }
    u32 *shown = (u32 *)(((u64)shown_raw + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    memset(shown, 0, bytes);

    /* Cascade windows so two programs do not open on top of each other. The
       vertical step has to clear a title bar, or the window underneath is
       there but cannot be clicked on.
     *
     * Starting clear of the icon column, because the desktop icons are what
     * is there to look at when nothing is running and the first window used
     * to open square on top of them.
     *
     * Then pulled back on to the screen. The cascade walks right and down,
     * and a window wide enough to begin with walks off the edge by the
     * fifth one: what opens is a title bar whose buttons are past the end
     * of the screen, so the window cannot be closed. */
    int step = handle % 5;
    int x = wm_icons_right() + 14 + step * 48, y = 36 + step * 38;

    int maxx = (int)fb_width() - (cw + WM_BORDER * 2);
    int maxy = wm_work_height() - (ch + WM_TOP + WM_BORDER);
    if (x > maxx) x = maxx;
    if (y > maxy) y = maxy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    window_t *w = wm_create(title, x, y, cw, ch);
    if (!w) { kfree(raw); kfree(shown_raw); return -1; }

    /* Which program this is. The task was named after the path it was
       loaded from, which is what the taskbar matches a pinned app against. */
    task_t *owner = task_by_pid(pid);
    if (owner) {
        strncpy(w->app, owner->name, sizeof(w->app) - 1);
        w->app[sizeof(w->app) - 1] = 0;
    }

    /* Point the window at the pages the program will get, and let the one
       the manager allocated go. */
    kfree(w->canvas);
    w->canvas = shown;            /* what the desktop reads, not what is drawn into */
    w->owned_by_user = true;
    w->on_close = on_wm_close;

    s->used = true;
    s->pid = pid;
    s->win = w;
    s->raw = raw;                 /* kfree wants the address kmalloc returned */
    s->pixels = pixels;           /* the mapping starts here, not at raw */
    s->shown_raw = shown_raw;
    s->shown = shown;
    s->bytes = bytes;
    s->user_addr = 0;
    s->dir = 0;
    return handle;
}

u64 winsrv_surface(u32 pid, int handle, u64 dir) {
    slot_t *s = lookup(pid, handle);
    if (!s) return 0;
    apply_pending(s);
    if (s->user_addr) return s->user_addr;

    u64 base = WINSRV_SURFACE_BASE + (u64)handle * WINSRV_SURFACE_STEP;
    for (u64 off = 0; off < s->bytes; off += PAGE_SIZE) {
        u64 phys = (u64)s->pixels + off;      /* identity mapped, so this is it */
        if (!map_page_in(dir, base + off, phys, PTE_PRESENT | PTE_RW | PTE_USER)) {
            for (u64 back = 0; back < off; back += PAGE_SIZE) unmap_page(base + back);
            return 0;
        }
    }

    s->user_addr = base;
    s->dir = dir;
    return base;
}

int winsrv_size(u32 pid, int handle) {
    slot_t *s = lookup(pid, handle);
    if (!s || !s->win) return -1;
    apply_pending(s);
    return (s->win->cw << 16) | (s->win->ch & 0xFFFF);
}

bool winsrv_poll(u32 pid, int handle, wm_event_t *out) {
    slot_t *s = lookup(pid, handle);
    if (!s) return false;
    apply_pending(s);
    if (!s->win) {
        /* The window is gone but the program has not noticed yet. */
        out->type = WM_EV_CLOSE;
        out->x = out->y = 0;
        out->buttons = out->key = 0;
        return true;
    }
    return wm_pop_event(s->win, out);
}

/* Swaps a window's pixels for a new block of a different size.
 *
 * The mapping has to end up at the same user address, because the program
 * was given that address once and has no reason to ask again. So the old
 * pages are unmapped from under it and the new ones put in their place,
 * which is only safe because nothing else can run in that address space
 * while this is happening. */
static bool resize_slot(slot_t *s, int cw, int ch) {
    if (cw < 32 || ch < 32 || cw > 1600 || ch > 1200) return false;
    if (!s->win) return false;
    if (s->win->cw == cw && s->win->ch == ch) return true;

    /* This must be the owning program's own address space, because the
       mapping is about to be pulled apart and rebuilt underneath it. Called
       from anywhere else it would be doing that to a program that might be
       part way through a draw. */
    if (s->dir && paging_current_directory() != s->dir) return false;

    u64 bytes = ((u64)((u64)cw * (u64)ch) * 4 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (bytes > WINSRV_SURFACE_STEP) return false;   /* would reach the next window */

    u32 *raw = (u32 *)kmalloc(bytes + PAGE_SIZE);
    if (!raw) return false;
    u32 *pixels = (u32 *)(((u64)raw + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    memset(pixels, 0, bytes);

    u32 *shown_raw = (u32 *)kmalloc(bytes + PAGE_SIZE);
    if (!shown_raw) { kfree(raw); return false; }
    u32 *shown = (u32 *)(((u64)shown_raw + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    memset(shown, 0, bytes);

    /* Remap in the owner's address space, which is almost never the live
       one: the resize is usually the window manager acting on a window
       belonging to a program that is not currently running. That is why
       every page here names its directory rather than assuming it.
       Dropping the bookkeeping and freeing the old pages instead would
       leave that program mapped to memory that had been handed back. */
    if (s->user_addr && s->dir) {
        for (u64 off = 0; off < s->bytes; off += PAGE_SIZE)
            unmap_page_in(s->dir, s->user_addr + off);

        for (u64 off = 0; off < bytes; off += PAGE_SIZE) {
            if (map_page_in(s->dir, s->user_addr + off, (u64)pixels + off,
                            PTE_PRESENT | PTE_RW | PTE_USER)) continue;
            /* Out of frames partway through. Put the old pages back rather
               than leaving the program with half a surface. */
            for (u64 back = 0; back < off; back += PAGE_SIZE)
                unmap_page_in(s->dir, s->user_addr + back);
            for (u64 old = 0; old < s->bytes; old += PAGE_SIZE)
                map_page_in(s->dir, s->user_addr + old, (u64)s->pixels + old,
                            PTE_PRESENT | PTE_RW | PTE_USER);
            kfree(raw);
            kfree(shown_raw);
            return false;
        }
    }

    /* Only the one the compositor reads has to be parked rather than
       freed: the manager may be part way through a pass over it. The one
       the program draws into is not read by anything else, and the program
       is the caller here, so it can go now. */
    if (s->raw) kfree(s->raw);
    retire(s->shown_raw);
    s->raw = raw;
    s->pixels = pixels;
    s->shown_raw = shown_raw;
    s->shown = shown;
    s->bytes = bytes;

    /* The three fields the compositor reads together, changed together, so
       it cannot catch a new surface paired with the old dimensions and read
       past the end of it. */
    bool were_on = interrupts_enabled();
    cli();
    s->win->canvas = shown;
    s->win->cw = cw;
    s->win->ch = ch;
    s->win->want_cw = 0;
    s->win->want_ch = 0;
    if (were_on) sti();

    s->win->dirty = true;
    wm_invalidate(s->win);
    return true;
}

/* Takes up a size the desktop asked for, if one is waiting. Every way into
   the server goes through here first, so the swap always happens on a call
   the program made rather than at a moment chosen for it. */
static void apply_pending(slot_t *s) {
    if (!s->win || !s->win->want_cw || !s->win->want_ch) return;
    int cw = s->win->want_cw, ch = s->win->want_ch;
    s->win->want_cw = s->win->want_ch = 0;
    resize_slot(s, cw, ch);
}

bool winsrv_resize(u32 pid, int handle, int cw, int ch) {
    slot_t *s = lookup(pid, handle);
    return s ? resize_slot(s, cw, ch) : false;   /* the owner is asking */
}

bool winsrv_allow_resize(u32 pid, int handle) {
    slot_t *s = lookup(pid, handle);
    if (!s || !s->win) return false;
    s->win->resizable = true;
    return true;
}

/* What the window manager calls when it is the one doing the resizing.
   It only writes down the size and tells the program; the surface is left
   exactly as it is until the program next calls in. */
bool winsrv_resize_window(window_t *w, int cw, int ch) {
    if (!w) return false;
    if (cw < 32 || ch < 32 || cw > 1600 || ch > 1200) return false;
    if (w->cw == cw && w->ch == ch) return true;

    w->want_cw = cw;
    w->want_ch = ch;

    wm_event_t ev = { WM_EV_RESIZE, cw, ch, 0, 0 };
    wm_push_event(w, &ev);
    return true;
}

/* Commit now means what its name says.
 *
 * It used to mark the window dirty and nothing else: the pixels the
 * compositor read were the pixels the program was still writing, so there
 * was no moment a frame became finished. Now the finished frame is copied
 * where the desktop is looking, and only then is the window marked. */
bool winsrv_commit(u32 pid, int handle) {
    slot_t *s = lookup(pid, handle);
    if (!s || !s->win) return false;
    apply_pending(s);
    publish(s);
    wm_invalidate(s->win);
    return true;
}

bool winsrv_close(u32 pid, int handle) {
    slot_t *s = lookup(pid, handle);
    if (!s) return false;
    free_slot(s);
    return true;
}

window_t *winsrv_window(u32 pid, int handle) {
    slot_t *s = lookup(pid, handle);
    return s ? s->win : 0;
}

void winsrv_release(u32 pid) {
    for (int i = 0; i < WINSRV_MAX; i++)
        if (slots[i].used && slots[i].pid == pid) free_slot(&slots[i]);
}
