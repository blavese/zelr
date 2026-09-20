/* Building and launching ring 3 processes. */
#include "user.h"
#include "io.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "printf.h"
#include "string.h"
#include "fs.h"
#include "heap.h"
#include "elf.h"
#include "user.h"

#define USER_CODE_BASE  (USER_SPACE_BASE + 0x40000000ull)

/* Where a program's stack pointer starts, which is eight bytes below a
   sixteen byte boundary and not on one.
 *
 * That looks wrong and is not. A compiled function assumes it was reached by
 * a call, and a call has pushed a return address, so at the first
 * instruction of any function the stack pointer is eight past alignment and
 * the prologue is written to match. A program entered by an interrupt
 * return has had nothing pushed, so handing it an aligned pointer leaves
 * every sixteen byte slot in its frame eight bytes out.
 *
 * With the vector instructions off nothing noticed. With them on, the first
 * aligned move to one of those slots is a general protection fault, and it
 * lands in whatever function happened to be complicated enough for the
 * compiler to want a vector spill. The allocator test found it; the same
 * allocator ran twenty thousand rounds on the host without complaint,
 * which is what said the fault was not in the allocator at all. */
/* Sixteen pages, which is sixty four kilobytes.
 *
 * Four was enough for a program that draws a window and reads a key. It is
 * not enough for one that lays out a web page: a style sheet is matched
 * against a tree, and a tree is walked by descending it, so the depth of
 * the page is the depth of the walk. Nesting a hundred divs deep is not
 * unusual on the web and it is not the page being strange.
 *
 * Unlike the kernel's own stacks there is nothing underneath this one: only
 * these pages are mapped, so running off the end is a page fault naming the
 * program rather than silent damage to whatever was below. That is the
 * better failure of the two and it is still a failure, which is why the
 * number is chosen against what the work needs rather than left at what the
 * first program happened to use. */
#define USER_STACK_PAGES 16

extern u8 user_stub_start[], user_stub_end[];

/* Maps a fresh, zeroed frame into a user address space. Frames come from the
   identity mapped pool, so the kernel can still reach them by their physical
   address to fill them in. */
static u64 alloc_user_page(u64 dir, u64 virt) {
    u64 frame = pmm_alloc_frame();
    if (!frame) return 0;
    memset((void *)frame, 0, PAGE_SIZE);
    if (!map_page_in(dir, virt, frame, PTE_PRESENT | PTE_RW | PTE_USER)) {
        pmm_free_frame(frame);
        return 0;
    }
    return frame;
}

bool user_build_stack(u64 dir) {
    for (u64 i = 1; i <= USER_STACK_PAGES; i++)
        if (!alloc_user_page(dir, USER_STACK_TOP - i * PAGE_SIZE)) return false;
    return true;
}

/* Copies a blob into consecutive user pages starting at base. */
static bool load_flat(u64 dir, u64 base, const u8 *data, u64 size) {
    u64 done = 0;
    while (done < size) {
        u64 frame = alloc_user_page(dir, base + done);
        if (!frame) return false;
        u64 n = size - done;
        if (n > PAGE_SIZE) n = PAGE_SIZE;
        memcpy((void *)frame, data + done, n);
        done += PAGE_SIZE;
    }
    return true;
}

/* --- the heap a program grows for itself ---------------------------------
 *
 * One page at a time, mapped into the calling program's own address space.
 * There is no shared pool and no reclaiming: a page handed to a program
 * belongs to it until the program exits, when the whole directory goes.
 * That is the right trade here, because the alternative is a kernel that
 * tracks every allocation twice, once down here and once in the allocator
 * in ring 3 that is already tracking them.
 */
u64 user_sbrk(i64 delta) {
    task_t *t = task_current();
    if (!t || !t->dir) return 0;

    if (!t->brk_base) {
        t->brk_base = USER_HEAP_BASE;
        t->brk = USER_HEAP_BASE;
    }
    u64 was = t->brk;
    if (delta == 0) return was;

    if (delta < 0) {
        /* Giving it back is accepted and does nothing. Unmapping would mean
           freeing frames that the allocator above may still have a free
           list threaded through, and the saving is a page. */
        u64 back = (u64)(-delta);
        if (back > t->brk - t->brk_base) t->brk = t->brk_base;
        else t->brk -= back;
        return was;
    }

    u64 want = was + (u64)delta;
    if (want < was) return 0;                     /* wrapped */
    if (want > USER_HEAP_MAX) return 0;

    u64 first = (was + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
    u64 last  = (want + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
    for (u64 va = first; va < last; va += PAGE_SIZE) {
        if (!alloc_user_page(t->dir, va)) {
            /* What was mapped stays mapped; the break does not move, so the
               pages are simply unused until the program asks again. Telling
               the caller no is the whole of the failure. */
            return 0;
        }
    }

    t->brk = want;
    return was;
}

int user_spawn_stub(const char *name) {
    u64 dir = paging_new_directory();
    if (!dir) return -1;

    u64 size = (u64)(user_stub_end - user_stub_start);
    if (!load_flat(dir, USER_CODE_BASE, user_stub_start, size) || !user_build_stack(dir)) {
        paging_free_directory(dir);
        return -2;
    }

    task_t *t = task_create_user(name, dir, USER_CODE_BASE, USER_STACK_START);
    if (!t) { paging_free_directory(dir); return -3; }
    return (int)t->pid;
}

int user_spawn_flat(const char *name, const u8 *image, u32 size) {
    if (!image || size == 0) return -1;

    u64 dir = paging_new_directory();
    if (!dir) return -1;

    if (!load_flat(dir, USER_CODE_BASE, image, size) || !user_build_stack(dir)) {
        paging_free_directory(dir);
        return -2;
    }

    task_t *t = task_create_user(name, dir, USER_CODE_BASE, USER_STACK_START);
    if (!t) { paging_free_directory(dir); return -3; }
    return (int)t->pid;
}

int user_spawn_elf(const char *name, const u8 *image, u32 size) {
    return user_spawn_elf_arg(name, image, size, 0);
}

int user_spawn_elf_arg(const char *name, const u8 *image, u32 size,
                       const char *arg) {
    u64 dir = paging_new_directory();
    if (!dir) return ELF_ERR_MEMORY;

    u64 entry = 0;
    int rc = elf_load(dir, image, size, &entry);
    if (rc != ELF_OK) { paging_free_directory(dir); return rc; }

    if (!user_build_stack(dir)) { paging_free_directory(dir); return ELF_ERR_MEMORY; }

    /* Interrupts off across the creation and the argument together: the
       task is runnable the moment it is on the list, and a program that
       reads its argument in its first instructions would otherwise find
       the field still empty. */
    bool were_on = interrupts_enabled();
    cli();
    task_t *t = task_create_user(name, dir, entry, USER_STACK_START);
    if (t && arg) {
        strncpy(t->arg, arg, sizeof(t->arg) - 1);
        t->arg[sizeof(t->arg) - 1] = 0;
    }
    if (were_on) sti();

    if (!t) { paging_free_directory(dir); return ELF_ERR_MEMORY; }
    return (int)t->pid;
}
