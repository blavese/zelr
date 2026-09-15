#pragma once
#include "types.h"
#include "handoff.h"

#define PTE_PRESENT  0x001
#define PTE_RW       0x002
#define PTE_USER     0x004
#define PTE_NOCACHE  0x018      /* write-through and cache-disable together */
#define PTE_HUGE     0x080      /* this entry is the page, not a table */

/* The physical address inside an entry. The top bits are flags, and bit 63
   is the no-execute bit, so a plain complement of the low twelve is wrong. */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

/* Identity mapped, shared by every address space. Everything the kernel
   allocates lives below this line, so a physical address is a valid pointer
   and drivers can hand one to a device without translation.
 *
 * The bottom of it is mapped a page at a time, because that is the part
 * whose mappings get changed: the kernel itself, the frame bitmap, the heap,
 * the trampoline the other processors start on, and the text mode buffer at
 * 0xB8000 which is not memory at all and has to be reachable anyway. */
#define KERNEL_LOW_MB 64ull

/* Above that, memory is mapped in 2 MiB pages, and only where the firmware
   said there is memory.
 *
 * Mapping everything instead would be simpler and wrong. The space between
 * the memory below 4 GiB and the memory above it is where devices put their
 * registers, and a device mapped as ordinary cacheable memory does not work:
 * a write sits in the cache instead of reaching the hardware. Those holes
 * are left unmapped here so that paging_map_device maps them properly, with
 * caching turned off, when a driver asks for one.
 *
 * The ceiling is what the frame bitmap can describe before it runs into the
 * heap, and is enforced rather than assumed. */
#define KERNEL_SPACE_MAX_GB 64ull

/* Where user space begins, and why it is so far up.
 *
 * One entry of the top level table covers 512 GiB. The kernel's mappings are
 * shared into every address space by copying that entry, so anything sharing
 * the entry shares the tables underneath it. In 32-bit each entry covered
 * only 4 MiB and a program at 1 GiB was naturally separate; here it would sit
 * in the same entry as the kernel, and its page tables would be added to the
 * kernel's own. Every address space would then see every other one's
 * memory, and none of it could be freed when a program exited.
 *
 * So user space gets an entry of its own: the second, at 512 GiB. Everything
 * below stays the kernel's, everything in that entry belongs to one program
 * and is freed with it. */
#define USER_SPACE_BASE   0x0000008000000000ull
#define USER_SPACE_END    0x000000FFFFFFF000ull

/* Takes the memory map because what gets mapped is what the firmware said
   is memory, rather than a fixed span that happens to be big enough. */
void paging_init(const handoff_t *h);

/* How much memory ended up identity mapped. */
u64  paging_mapped_bytes(void);

/* Operate on whichever address space is loaded. */
bool map_page(u64 virt, u64 phys, u64 flags);
void unmap_page(u64 virt);

/* The same, in an address space that is not necessarily the live one. */
void unmap_page_in(u64 pml4_phys, u64 virt);
u64  virt_to_phys(u64 virt);

/* Operate on a named address space, which need not be the active one. */
bool map_page_in(u64 pml4_phys, u64 virt, u64 phys, u64 flags);
u64  virt_to_phys_in(u64 pml4_phys, u64 virt);

/* True only if ring 3 could reach this address itself, which is a stricter
   question than whether it is mapped. */
bool virt_is_user_in(u64 pml4_phys, u64 virt);

/* A fresh address space sharing the kernel's mappings. Returns the physical
   address of the top level table, or 0. */
u64  paging_new_directory(void);
void paging_free_directory(u64 pml4_phys);
void paging_switch(u64 pml4_phys);
u64  paging_current_directory(void);
u64  paging_kernel_directory(void);

/* Maps a device's registers where they already are, uncached. */
void *paging_map_device(u64 phys, u64 bytes);
