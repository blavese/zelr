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

#define USER_CODE_BASE  (USER_SPACE_BASE + 0x40000000ull)
#define USER_STACK_TOP  (USER_SPACE_BASE + 0x50000000ull)
#define USER_STACK_PAGES 4

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

static bool build_stack(u64 dir) {
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

int user_spawn_stub(const char *name) {
    u64 dir = paging_new_directory();
    if (!dir) return -1;

    u64 size = (u64)(user_stub_end - user_stub_start);
    if (!load_flat(dir, USER_CODE_BASE, user_stub_start, size) || !build_stack(dir)) {
        paging_free_directory(dir);
        return -2;
    }

    task_t *t = task_create_user(name, dir, USER_CODE_BASE, USER_STACK_TOP - 16);
    if (!t) { paging_free_directory(dir); return -3; }
    return (int)t->pid;
}

int user_spawn_flat(const char *name, const u8 *image, u32 size) {
    if (!image || size == 0) return -1;

    u64 dir = paging_new_directory();
    if (!dir) return -1;

    if (!load_flat(dir, USER_CODE_BASE, image, size) || !build_stack(dir)) {
        paging_free_directory(dir);
        return -2;
    }

    task_t *t = task_create_user(name, dir, USER_CODE_BASE, USER_STACK_TOP - 16);
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

    if (!build_stack(dir)) { paging_free_directory(dir); return ELF_ERR_MEMORY; }

    /* Interrupts off across the creation and the argument together: the
       task is runnable the moment it is on the list, and a program that
       reads its argument in its first instructions would otherwise find
       the field still empty. */
    bool were_on = interrupts_enabled();
    cli();
    task_t *t = task_create_user(name, dir, entry, USER_STACK_TOP - 16);
    if (t && arg) {
        strncpy(t->arg, arg, sizeof(t->arg) - 1);
        t->arg[sizeof(t->arg) - 1] = 0;
    }
    if (were_on) sti();

    if (!t) { paging_free_directory(dir); return ELF_ERR_MEMORY; }
    return (int)t->pid;
}
