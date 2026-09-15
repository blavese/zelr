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
void pmm_free_frame(u64 addr);
u64  pmm_total_frames(void);
u64  pmm_used_frames(void);
u64  pmm_free_frames(void);
