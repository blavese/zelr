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

/* A program is entered with the stack pointer on a sixteen byte boundary,
 * which is what System V says and what the startup code in sdk/zelr.h
 * is written against.
 *
 * It used to be eight below one, because every program's entry point was an
 * ordinary compiled function and a compiled function assumes a call pushed
 * a return address for it. Handing such a function an aligned pointer put
 * every sixteen byte slot in its frame eight bytes out, which nothing
 * noticed until the vector instructions went on: then the first aligned
 * move to one of those slots was a general protection fault, landing in
 * whatever function happened to be complicated enough for the compiler to
 * want a vector spill. The allocator test found it, and the same allocator
 * running twenty thousand rounds on the host without complaint is what said
 * the fault was not in the allocator at all.
 *
 * The entry point is startup code now rather than a function, and it does
 * its own aligning before it calls anything, which is how every other
 * machine arranges this.
 */
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

/* The stack a program starts on, and the words it starts with.
 *
 * System V says what is there when a process is entered, and this builds
 * exactly that, from the top of the last stack page downward:
 *
 *     the strings, argv[0] last so argv[0] is highest
 *     a null word            the environment, which is empty here
 *     a null word            the end of the vector
 *     argv[argc-1] .. argv[0]
 *     argc                   <- the stack pointer, on a 16 byte boundary
 *
 * It is built here rather than handed over through a system call because a
 * program's words have to be there before its first instruction runs. The
 * old arrangement was a single string in the task, copied out by a call the
 * program made at startup, and it needed interrupts off across creating the
 * task and filling the field in -- a task is runnable the moment it is on
 * the list, and a program that asked early enough would have found it empty.
 * Memory that is already on the stack cannot lose that race.
 *
 * It also means a program built somewhere else, by a toolchain that has
 * never heard of this kernel, finds its arguments where its startup code
 * already looks for them.
 */
bool user_build_stack(u64 dir, int argc, const char *const *argv,
                      u64 *rsp_out) {
    u64 top = 0;
    for (u64 i = 1; i <= USER_STACK_PAGES; i++) {
        u64 f = alloc_user_page(dir, USER_STACK_TOP - i * PAGE_SIZE);
        if (!f) return false;
        if (i == 1) top = f;                  /* the page the block goes in */
    }

    if (argc < 0) argc = 0;
    if (argc > USER_ARGV_MAX) return false;

    u8 *page = (u8 *)top;
    u64 at = PAGE_SIZE;                       /* an offset inside that page */
    u64 ptr[USER_ARGV_MAX];

    for (int i = argc - 1; i >= 0; i--) {
        const char *w = argv && argv[i] ? argv[i] : "";
        u64 n = strlen(w) + 1;
        if (n > at) return false;
        at -= n;
        memcpy(page + at, w, n);
        ptr[i] = USER_STACK_TOP - (PAGE_SIZE - at);
    }

    /* argc, the vector, and the two nulls that end the vector and the
       environment. Rounded down to sixteen so the entry point is reached on
       a boundary, which is what the ABI promises and what a compiler's
       aligned vector stores depend on. */
    u64 need = ((u64)argc + 3) * 8;
    if (need > at) return false;
    at = (at - need) & ~15ull;

    u64 *block = (u64 *)(page + at);
    block[0] = (u64)argc;
    for (int i = 0; i < argc; i++) block[1 + i] = ptr[i];
    block[1 + argc] = 0;                      /* the end of argv */
    block[2 + argc] = 0;                      /* and of an empty environ */

    if (rsp_out) *rsp_out = USER_STACK_TOP - (PAGE_SIZE - at);
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
    u64 rsp = 0;
    if (!load_flat(dir, USER_CODE_BASE, user_stub_start, size) ||
        !user_build_stack(dir, 1, &name, &rsp)) {
        paging_free_directory(dir);
        return -2;
    }

    task_t *t = task_create_user(name, dir, USER_CODE_BASE, rsp);
    if (!t) { paging_free_directory(dir); return -3; }
    return (int)t->pid;
}

int user_spawn_flat(const char *name, const u8 *image, u32 size) {
    if (!image || size == 0) return -1;

    u64 dir = paging_new_directory();
    if (!dir) return -1;

    u64 rsp = 0;
    if (!load_flat(dir, USER_CODE_BASE, image, size) ||
        !user_build_stack(dir, 1, &name, &rsp)) {
        paging_free_directory(dir);
        return -2;
    }

    task_t *t = task_create_user(name, dir, USER_CODE_BASE, rsp);
    if (!t) { paging_free_directory(dir); return -3; }
    return (int)t->pid;
}

int user_spawn_elf(const char *name, const u8 *image, u32 size) {
    /* Started on its own name, which is what argv[0] is everywhere. */
    return user_spawn_elf_argv(name, image, size, 1, &name);
}

int user_spawn_elf_argv(const char *name, const u8 *image, u32 size,
                        int argc, const char *const *argv) {
    u64 dir = paging_new_directory();
    if (!dir) return ELF_ERR_MEMORY;

    u64 entry = 0;
    int rc = elf_load(dir, image, size, &entry);
    if (rc != ELF_OK) { paging_free_directory(dir); return rc; }

    u64 rsp = 0;
    if (!user_build_stack(dir, argc, argv, &rsp)) {
        paging_free_directory(dir);
        return ELF_ERR_MEMORY;
    }

    task_t *t = task_create_user(name, dir, entry, rsp);
    if (!t) { paging_free_directory(dir); return ELF_ERR_MEMORY; }
    return (int)t->pid;
}
