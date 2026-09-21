#pragma once
#include "types.h"
#include "handoff.h"

#define PAGE_SIZE 4096ull

/* Fed from the handoff rather than from multiboot, because a machine that
   booted through UEFI has no multiboot information at all. */
/* bitmap_limit is the address the bitmap must stay below, which is where
   the heap begins. A machine with more memory than fits in that gap gets as
   much of it as can be described. */
void pmm_init(const handoff_t *h, u64 bitmap_limit);

/* Marks a physical range as spoken for, so it is never handed out. */
void pmm_reserve(u64 start, u64 size);

u64  pmm_alloc_frame(void);        /* physical address, or 0 */

/* Gives a frame back. A shared one is only really given back by the last
   address space holding it; see below. */
void pmm_free_frame(u64 addr);

/* --- frames held by more than one address space --------------------------
 *
 * What copy on write needs, and the only part of it the allocator has to
 * know about. Two address spaces pointing at one frame is fine right up
 * until one of them is torn down, at which point the frame goes back to the
 * free list while the other is still reading it -- and what that looks like
 * is one program's memory turning into another program's, later, somewhere
 * else.
 *
 * So a count of the extra holders. Zero is the ordinary case and means one
 * owner, so a frame nothing ever shared behaves exactly as it did.
 *
 * The table is a byte a frame and comes from the heap, which means it
 * cannot exist until the heap does -- pmm_init runs long before that. Until
 * pmm_share_ready() is true, sharing is refused and the caller copies
 * instead, which is slower and always correct. */
void pmm_share_init(void);
bool pmm_share_ready(void);

/* Another address space is using this frame. False means it could not be
   recorded, and the caller must copy rather than share. */
bool pmm_share(u64 addr);

/* How many hold it: 1 for an ordinary frame, more for a shared one. */
u32  pmm_holders(u64 addr);

/* How much sharing there is, for anything that reports on the machine. */
u64  pmm_shared_frames(void);
u64  pmm_total_frames(void);
u64  pmm_used_frames(void);
u64  pmm_free_frames(void);
