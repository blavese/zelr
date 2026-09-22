/* System calls.
 *
 * int 0x80, arguments in registers, result in eax. The gate is the only door
 * from ring 3 into the kernel, so every pointer that arrives through it comes
 * from code that is not trusted and has to be checked against the caller's
 * own address space before it is touched. */
#include "syscall.h"
#include "idt.h"
#include "printf.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "fs.h"
#include "string.h"
#include "timer.h"
#include "winsrv.h"
#include "clipboard.h"
#include "sound.h"
#include "vfs.h"
#include "fd.h"
#include "signal.h"
#include "wait.h"
#include "io.h"
#include "gdt.h"
#include "user.h"
#include "elf.h"
#include "heap.h"
#include "elf.h"
#include "user.h"
#include "net.h"
#include "tcp.h"
#include "tls.h"
#include "heap.h"
#include "smp.h"
#include "pmm.h"
#include "fb.h"
#include "fat.h"
#include "power.h"
#include "diskfs.h"

/* A pointer from ring 3 has to be inside user space to begin with. Being
   mapped is checked separately, and being reachable from ring 3 after that. */
#define USER_MIN USER_SPACE_BASE

/* Confirms a user buffer is one the caller could have reached on its own:
   above the kernel, not wrapped, and mapped user-accessible for its whole
   length. A missing check here is how a kernel gets talked into reading or
   writing wherever it is asked to.

   Merely being mapped is not enough. Every address space inherits the
   kernel's high mappings, so the framebuffer aperture and the register
   windows of the disk and network controllers are all present in a user
   directory; they are simply not reachable from ring 3 because their pages
   lack the user bit. Checking presence alone would let a program name one of
   those addresses and have the kernel write to it on the program's behalf. */
static bool user_range_ok(u64 addr, u64 len) {
    if (len == 0) return true;
    if (addr < USER_MIN) return false;
    if (addr + len < addr) return false;                 /* wrapped */

    u64 dir = paging_current_directory();
    for (u64 a = addr & ~0xFFFull; a < addr + len; a += PAGE_SIZE) {
        if (!virt_is_user_in(dir, a)) return false;
    }
    return true;
}

/* Whose call this is. Used everywhere a handle has to be checked against
   its owner, so one program cannot drive another's window or socket. */
static u32 caller_pid(void) {
    task_t *t = task_current();
    return t ? t->pid : 0;
}

/* Copies a path in before anything looks at it. A path still in user memory
   can be changed by another thread between the check and the use. */
static bool copy_path(u64 addr, char *out, u64 cap) {
    for (u32 i = 0; i < cap; i++) {
        if (!user_range_ok(addr + i, 1)) return false;
        out[i] = ((const char *)addr)[i];
        if (!out[i]) return true;
    }
    return false;                       /* no terminator inside the limit */
}

static i64 sys_exit(registers_t *r) {
    /* The status is whatever the program returned from main, which its
       start code puts in the first argument. */
    task_exit_with((int)(i32)r->rbx);
    return 0;
}

/* --- starting other programs --------------------------------------------

   A program naming another program is the one place a system call takes a
   path that leads to something being run, so the path is copied in and the
   image is read through the same VFS everything else uses: there is no way
   to ask for a program that could not also be read with `cat`. */

static i64 sys_spawn(registers_t *r) {
    char path[VFS_PATH_MAX];
    if (!copy_path(r->rbx, path, sizeof(path))) return -1;

    u32 size = 0;
    u8 *image = vfs_slurp(path, &size);
    if (!image) return -1;

    /* Named after the path, because that is what the taskbar matches a
       window against to know it is the app whose icon is pinned. */
    int rc = user_spawn_elf(path, image, size);
    kfree(image);
    return rc;
}

/* --- the words a program is started on -----------------------------------
 *
 * A vector of pointers in the caller's address space, each pointing at a
 * string in the same place. Both levels belong to ring 3, so both are
 * checked, and everything is copied rather than pointed at: exec throws the
 * caller's address space away while these are still needed, and a pointer
 * into it would then be a pointer into memory that had stopped existing.
 *
 * One block holds every string, because what has to be bounded is the total
 * and not the longest.
 */
#define ARGV_BYTES 2048

typedef struct {
    char       *store;
    const char *word[USER_ARGV_MAX];
    int         count;
} argv_t;

static void argv_drop(argv_t *a) {
    if (a->store) kfree(a->store);
    a->store = 0;
    a->count = 0;
}

/* No vector at all is no words rather than a failure: that is what a
   program started by something with nothing to say to it gets. */
static bool argv_take(argv_t *a, u64 vec, i64 count) {
    a->store = 0;
    a->count = 0;
    if (count <= 0 || !vec) return true;
    if (count > USER_ARGV_MAX) return false;
    if (!user_range_ok(vec, (u64)count * sizeof(u64))) return false;

    a->store = (char *)kmalloc(ARGV_BYTES);
    if (!a->store) return false;

    const u64 *from = (const u64 *)vec;
    u64 used = 0;
    for (i64 i = 0; i < count; i++) {
        if (used >= ARGV_BYTES || !copy_path(from[i], a->store + used,
                                             ARGV_BYTES - used)) {
            argv_drop(a);
            return false;
        }
        a->word[i] = a->store + used;
        used += strlen(a->store + used) + 1;
    }
    a->count = (int)count;
    return true;
}

static i64 sys_spawn_argv(registers_t *r) {
    char path[VFS_PATH_MAX];
    if (!copy_path(r->rbx, path, sizeof(path))) return -1;

    argv_t a;
    if (!argv_take(&a, r->rcx, (i64)r->rdx)) return -1;

    u32 size = 0;
    u8 *image = vfs_slurp(path, &size);
    if (!image) { argv_drop(&a); return -1; }

    /* Its own name when the caller named nothing, so a program can always
       read argv[0] and always find something. */
    const char *self = path;
    int rc = a.count ? user_spawn_elf_argv(path, image, size, a.count, a.word)
                     : user_spawn_elf_argv(path, image, size, 1, &self);
    kfree(image);
    argv_drop(&a);
    return rc;
}

/* --- fork ----------------------------------------------------------------
 *
 * Copy the address space, then copy the frame. The child is told nothing:
 * it comes back from this very call with zero in it, on a stack that is a
 * copy of the parent's, at the instruction after the one that asked.
 *
 * The frame this is handed is the one the interrupt return path will use for
 * the parent, so the child's copy of it is already correct in every register
 * except the one the answer arrives in.
 */
static i64 sys_fork(registers_t *r) {
    task_t *parent = task_current();
    if (!parent || !parent->dir) return -1;

    u64 dir = paging_clone_directory(parent->dir);
    if (!dir) return -1;

    /* Interrupts off across the copy and the naming together: the child is
       runnable the instant it is on the list, and a scheduler that ran it
       before its name was set would print an empty one. */
    bool were_on = interrupts_enabled();
    cli();
    task_t *child = task_fork(parent->name, dir, r, 0);
    if (were_on) sti();

    if (!child) { paging_free_directory(dir); return -1; }
    return (i64)child->pid;
}

/* --- exec ----------------------------------------------------------------
 *
 * The other half. This does not make a process: it throws away the one
 * running in this process and puts another in its place, which is why a
 * successful exec never returns.
 *
 * The old address space cannot be freed while this code is still running on
 * it, and it is not: the kernel stack this is executing on belongs to the
 * task rather than to the address space, and the switch below moves to the
 * new space before the old one goes.
 */
static i64 sys_exec(registers_t *r) {
    task_t *t = task_current();
    if (!t || !t->dir) return -1;

    char path[VFS_PATH_MAX];
    if (!copy_path(r->rbx, path, sizeof(path))) return -1;

    /* Taken before anything is torn down. These live in the address space
       this call is about to replace. */
    argv_t a;
    if (!argv_take(&a, r->rcx, (i64)r->rdx)) return -1;

    u32 size = 0;
    u8 *image = vfs_slurp(path, &size);
    if (!image) { argv_drop(&a); return -1; }

    u64 dir = paging_new_directory();
    if (!dir) { kfree(image); argv_drop(&a); return -1; }

    u64 entry = 0;
    int rc = elf_load(dir, image, size, &entry);
    kfree(image);
    if (rc != ELF_OK) { paging_free_directory(dir); argv_drop(&a); return rc; }

    const char *self = path;
    u64 rsp = 0;
    bool built = a.count ? user_build_stack(dir, a.count, a.word, &rsp)
                         : user_build_stack(dir, 1, &self, &rsp);
    argv_drop(&a);
    if (!built) { paging_free_directory(dir); return -1; }

    u64 old = t->dir;

    bool were_on = interrupts_enabled();
    cli();

    t->dir = dir;
    paging_switch(dir);

    /* The heap belonged to the program that is being replaced. */
    t->brk = 0;
    t->brk_base = 0;

    /* So did every handler. An address that meant something in the old
       program means whatever happens to be there in the new one, and a
       signal arriving afterwards would jump into it. */
    signal_forget_handlers(t->pid);

    /* And every mapping. They described the address space that is being
       thrown away; the new program's are its own to ask for. */
    user_drop_mappings();

    /* A name is what `ps` shows, and a process that became something else
       should say what it became. */
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    strncpy(t->name, base, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = 0;

    /* Rewriting the frame is how this never returns: the interrupt return
       path is about to unwind it, and it now describes the new program at
       its first instruction rather than this call at its last. */
    memset(r, 0, sizeof(*r));
    r->rip = entry;
    r->cs = USER_CODE_SEL;
    r->ss = USER_DATA_SEL;
    r->rsp = rsp;
    r->rflags = 0x202;
    r->int_no = 32;

    if (were_on) sti();

    paging_free_directory(old);
    return 0;
}

static i64 sys_getppid(registers_t *r) {
    (void)r;
    task_t *t = task_current();
    return t ? (i64)t->parent_pid : 0;
}

static i64 sys_sbrk(registers_t *r) {
    return (i64)user_sbrk((i64)r->rbx);
}

static i64 sys_wait(registers_t *r) {
    return task_wait((u32)r->rbx);
}

/* Ends another task. Only what this program started, which for now means any
   task other than the one asking: there is no parent to check against yet,
   and the alternative is not being able to stop anything at all. */
static i64 sys_kill(registers_t *r) {
    u32 pid = (u32)r->rbx;
    task_t *me = task_current();
    if (me && me->pid == pid) return -1;

    task_t *t = task_by_pid(pid);
    if (!t || t->state == TASK_DEAD) return -1;

    /* Everything it holds goes back, exactly as if it had exited. One copy
       of that, in kernel/signal.c, because there are three ways to end a
       task now and the risk is that one of them forgets the windows. */
    signal_end_task(pid, -1);
    return 0;
}

/* --- signals -------------------------------------------------------------
 *
 * What a program does with one, and how one is raised. There is no handler:
 * see include/signal.h for why the honest version of that is absence. */
static i64 sys_signal(registers_t *r) {
    task_t *t = task_current();
    if (!t) return -1;
    /* The number, what to do with it, and -- when that is a handler -- the
       address in ring 3 the handler returns into. The kernel cannot supply
       the third, having no code mapped there. */
    return signal_disposition(t->pid, (int)r->rbx, r->rcx, r->rdx) ? 0 : -1;
}

/* Asked for by a handler's return, and by nothing else.
 *
   It does not return to its caller. It puts back the frame the program was
   interrupted at, so what comes out of the interrupt gate is the program
   carrying on where the signal found it -- which is why the value handed
   back is the rax that was saved rather than a result of anything. */
static i64 sys_poll(registers_t *r) {
    u32 n = (u32)r->rcx;
    if (n > POLL_MAX) return -1;

    u64 bytes = (u64)n * sizeof(pollfd_t);
    if (n && !user_range_ok(r->rbx, bytes)) return -1;

    /* Copied in and back out rather than worked on where it lies. This
       sleeps, and a program that is asleep can be forked or can have
       another of its threads -- when there are threads -- change what is
       under that pointer. */
    pollfd_t set[POLL_MAX];
    for (u32 i = 0; i < n; i++) set[i] = ((const pollfd_t *)r->rbx)[i];

    int got = fd_poll(set, n, (int)(i32)r->rdx);

    if (!user_range_ok(r->rbx, bytes)) return -1;   /* it may have gone */
    for (u32 i = 0; i < n; i++) ((pollfd_t *)r->rbx)[i].revents = set[i].revents;
    return got;
}

static i64 sys_rename(registers_t *r) {
    char from[VFS_PATH_MAX], to[VFS_PATH_MAX];
    if (!copy_path(r->rbx, from, sizeof(from))) return -1;
    if (!copy_path(r->rcx, to, sizeof(to))) return -1;
    return vfs_rename(from, to) ? 0 : -1;
}

static i64 sys_fsync(registers_t *r) {
    return fd_sync((int)r->rbx) ? 0 : -1;
}

static i64 sys_mmap(registers_t *r) {
    /* The length and what it may be used for. No address hint: the
       kernel picks, because a program that picks is a program that can
       pick something already in use. */
    return (i64)user_mmap(r->rbx, (int)r->rcx);
}

static i64 sys_munmap(registers_t *r) {
    return user_munmap(r->rbx, r->rcx) ? 0 : -1;
}

static i64 sys_sigreturn(registers_t *r) {
    if (signal_return(r)) return (i64)r->rax;

    /* The frame was not one this kernel wrote, or the stack it was on has
       gone. Either way the program cannot be put back, and carrying on from
       a frame that failed its checks is the one thing not to do. */
    task_exit_with(139);
    return -1;
}

static i64 sys_sigsend(registers_t *r) {
    return signal_send((u32)r->rbx, (int)r->rcx) ? 0 : -1;
}

static i64 sys_win_resizable(registers_t *r) {
    task_t *t = task_current();
    return winsrv_allow_resize(t ? t->pid : 0, (int)r->rbx) ? 0 : -1;
}

static i64 sys_win_resize(registers_t *r) {
    task_t *t = task_current();
    return winsrv_resize(t ? t->pid : 0, (int)r->rbx,
                         (int)r->rcx, (int)r->rdx) ? 0 : -1;
}

/* The length is passed rather than inferred, so a program can copy text that
   is not terminated, which is what a selection out of a terminal's grid is. */
static i64 sys_clip_set(registers_t *r) {
    u32 len = (u32)r->rcx;
    if (len > CLIP_MAX) return -1;
    if (!user_range_ok(r->rbx, len ? len : 1)) return -1;
    return clip_set((const char *)r->rbx, len) ? (i64)len : -1;
}

static i64 sys_clip_get(registers_t *r) {
    u32 cap = (u32)r->rcx;
    if (cap == 0) return (i64)clip_len();      /* asking how much there is */
    if (!user_range_ok(r->rbx, cap)) return -1;
    return (i64)clip_get((char *)r->rbx, cap);
}

static i64 sys_sound_info(registers_t *r) {
    if (!user_range_ok(r->rbx, sizeof(sound_info_t))) return -1;
    sound_info_t *out = (sound_info_t *)r->rbx;
    out->present = sound_present() ? 1 : 0;
    out->rate = sound_rate();
    out->channels = sound_channels();
    out->reserved = 0;
    return 0;
}

static i64 sys_sound_write(registers_t *r) {
    u32 count = (u32)r->rcx;
    if (!sound_present()) return -1;
    if (!count) return 0;

    /* Capped so that one call cannot ask the kernel to sit in a write for
       longer than the sound it is writing. The caller loops. */
    if (count > 4096) count = 4096;

    u64 bytes = (u64)count * sound_channels() * 2;
    if (!user_range_ok(r->rbx, bytes)) return -1;
    return (i64)sound_write((const i16 *)r->rbx, count);
}

static i64 sys_power(registers_t *r) {
    /* Anything not yet on the disk goes first. A machine that is switched
       off does not come back to finish writing. */
    diskfs_flush();

    if ((u32)r->rbx == POWER_REBOOT) {
        power_reboot();
        return -1;
    }
    if (!power_can_off()) return -1;
    power_off();
    return -1;                  /* only reached when the firmware declined */
}

static i64 sys_tasks(registers_t *r) {
    u32 index = (u32)r->rbx;
    if (!user_range_ok(r->rcx, sizeof(zelr_task_t))) return -1;

    task_t *head = task_list();
    if (!head) return 0;

    u32 i = 0;
    task_t *p = head;
    do {
        if (i == index) {
            zelr_task_t out;
            memset(&out, 0, sizeof(out));
            out.pid = p->pid;
            out.state = (u32)p->state;
            out.slices = p->slices;
            out.idle = (u32)p->idle_ticks;
            out.user = p->user ? 1 : 0;
            strncpy(out.name, p->name, sizeof(out.name) - 1);
            memcpy((void *)r->rcx, &out, sizeof(out));
            return 1;
        }
        i++;
        p = p->next;
    } while (p != head);

    return 0;
}

static i64 sys_putc(registers_t *r) {
    char c = (char)(r->rbx & 0xFF);
    return fd_write(FD_STDOUT, &c, 1);
}

/* This used to put the bytes on the screen itself. Now it puts them wherever
 * this program's descriptor 1 points, which is the screen unless somebody
 * arranged otherwise — and the somebody is a shell, between the fork and the
 * exec, with the program none the wiser.
 *
 * Every program in the system writes through this call, so every program in
 * the system became redirectable the moment this line changed, without one
 * of them being recompiled to know about it. */
static i64 sys_write(registers_t *r) {
    u64 buf = r->rcx;
    u64 len = r->rdx;
    if (len > 65536) return -1;
    if (!user_range_ok(buf, len)) return -1;
    return fd_write(FD_STDOUT, (const void *)buf, (u32)len);
}

static i64 sys_getpid(registers_t *r) {
    (void)r;
    task_t *t = task_current();
    return t ? (i32)t->pid : -1;
}

static i64 sys_ticks(registers_t *r) {
    (void)r;
    return (i32)timer_ticks();
}

static i64 sys_sleep(registers_t *r) {
    task_sleep(r->rbx);
    return 0;
}

/* Reads a file into a user buffer. Returns the byte count, or -1. */
static i64 sys_read_file(registers_t *r) {
    u64 name_addr = r->rbx;
    u64 buf = r->rcx;
    u64 cap = r->rdx;

    if (!user_range_ok(name_addr, 1) || !user_range_ok(buf, cap)) return -1;

    char name[FS_NAME_MAX];
    const char *src = (const char *)name_addr;
    u32 i = 0;
    for (; i < FS_NAME_MAX - 1; i++) {
        if (!user_range_ok(name_addr + i, 1)) return -1;
        name[i] = src[i];
        if (!name[i]) break;
    }
    name[FS_NAME_MAX - 1] = 0;

    return vfs_read(name, (void *)buf, cap);
}

/* --- files ---------------------------------------------------------------

   Every path arrives as a user pointer, so it is copied into the kernel
   before anything looks at it. A path that is still in user memory can be
   changed by another thread between the check and the use. */

static i64 sys_open(registers_t *r) {
    char path[VFS_PATH_MAX];
    if (!copy_path(r->rbx, path, sizeof(path))) return -1;
    return fd_open(path, r->rcx);
}

static i64 sys_close(registers_t *r) {
    return fd_close((int)r->rbx) ? 0 : -1;
}

static i64 sys_fread(registers_t *r) {
    u64 buf = r->rcx, len = r->rdx;
    if (len > 1024 * 1024) return -1;
    if (!user_range_ok(buf, len)) return -1;
    return fd_read((int)r->rbx, (void *)buf, len);
}

static i64 sys_fwrite(registers_t *r) {
    u64 buf = r->rcx, len = r->rdx;
    if (len > 1024 * 1024) return -1;
    if (!user_range_ok(buf, len)) return -1;
    return fd_write((int)r->rbx, (const void *)buf, len);
}

/* --- duplicating a descriptor -------------------------------------------
 *
 * Redirection is this and nothing else. A shell forks, and in the child says
 * dup2(file, 1): descriptor 1 now refers to the file, the old 1 is closed,
 * and the program that follows writes to its output exactly as it always
 * did. Nothing in the program knows, which is the entire point — `sort`
 * would need a flag for every place its output could ever go, and instead
 * it needs none. */
static i64 sys_dup(registers_t *r) {
    return fd_dup((int)r->rbx);
}

static i64 sys_dup2(registers_t *r) {
    return fd_dup2((int)r->rbx, (int)r->rcx);
}

/* Hands back two numbers, into an array of two the caller owns. */
static i64 sys_pipe(registers_t *r) {
    if (!user_range_ok(r->rbx, sizeof(int) * 2)) return -1;
    int ends[2];
    if (!fd_pipe(ends)) return -1;
    int *out = (int *)r->rbx;
    out[0] = ends[0];
    out[1] = ends[1];
    return 0;
}

static i64 sys_seek(registers_t *r) {
    return fd_seek((int)r->rbx, (i32)r->rcx, r->rdx);
}

static i64 sys_unlink(registers_t *r) {
    char path[VFS_PATH_MAX];
    if (!copy_path(r->rbx, path, sizeof(path))) return -1;
    return vfs_delete(path) ? 0 : -1;
}

static i64 sys_mkdir(registers_t *r) {
    char path[VFS_PATH_MAX];
    if (!copy_path(r->rbx, path, sizeof(path))) return -1;
    return vfs_mkdir(path) ? 0 : -1;
}

static i64 sys_rmdir(registers_t *r) {
    char path[VFS_PATH_MAX];
    if (!copy_path(r->rbx, path, sizeof(path))) return -1;
    return vfs_rmdir(path) ? 0 : -1;
}

/* What the kernel writes into a name, and what a program has room for, are
   the same number or this is a stack overflow on every listing. It was one
   for a release: names were eight and three, the field was 32 bytes and
   that was enough, and then long filenames made VFS_NAME_MAX 64 and nothing
   said so. The write goes upward, into the saved registers the syscall
   returns through, so what ring 3 got back was a zero from the padding and
   every directory looked empty. */
_Static_assert(sizeof(((zelr_stat_t *)0)->name) >= VFS_NAME_MAX,
               "readdir writes VFS_NAME_MAX bytes into zelr_stat_t.name");

static i64 sys_readdir(registers_t *r) {
    char path[VFS_PATH_MAX];
    if (!copy_path(r->rbx, path, sizeof(path))) return -1;
    if (!user_range_ok(r->rdx, sizeof(zelr_stat_t))) return -1;

    zelr_stat_t st;
    memset(&st, 0, sizeof(st));
    bool is_dir = false;
    int rc = vfs_list(path, r->rcx, st.name, &st.size, &is_dir);
    if (rc != 1) return rc < 0 ? -1 : 0;
    st.is_dir = is_dir ? 1 : 0;
    memcpy((void *)r->rdx, &st, sizeof(st));
    return 1;
}

static i64 sys_stat(registers_t *r) {
    char path[VFS_PATH_MAX];
    if (!copy_path(r->rbx, path, sizeof(path))) return -1;
    if (!user_range_ok(r->rcx, sizeof(zelr_stat_t))) return -1;

    zelr_stat_t st;
    memset(&st, 0, sizeof(st));
    bool is_dir = false;
    if (!vfs_stat(path, &st.size, &is_dir)) return -1;
    st.is_dir = is_dir ? 1 : 0;
    memcpy((void *)r->rcx, &st, sizeof(st));
    return 0;
}

static i64 sys_chdir(registers_t *r) {
    char path[VFS_PATH_MAX];
    if (!copy_path(r->rbx, path, sizeof(path))) return -1;
    return vfs_chdir(path) ? 0 : -1;
}

static i64 sys_getcwd(registers_t *r) {
    u64 buf = r->rbx, cap = r->rcx;
    if (cap == 0 || cap > VFS_PATH_MAX) return -1;
    if (!user_range_ok(buf, cap)) return -1;
    const char *at = vfs_cwd();
    u32 n = (u32)strlen(at);
    if (n + 1 > cap) return -1;
    memcpy((void *)buf, at, n + 1);
    return (i32)n;
}

/* --- sockets -------------------------------------------------------------

   There used to be one, for the whole machine, and it belonged to whoever
   opened it: a second program asking to connect was told the machine was
   busy. That was not a simplification of this interface, it was the stack
   underneath having exactly one connection in file level variables.

   It holds several now, so this does too. A socket is a small number, the
   caller gets it back from connect and hands it to everything afterwards,
   and it is checked against the caller on every call -- a program must not
   be able to read another program's connection by guessing a number, and
   with one socket that question could not even be asked. */

#define SOCK_MAX TCP_MAX

typedef struct {
    bool open;
    bool secure;
    u32  owner;
    int  tcp;
} sock_t;

static sock_t socks[SOCK_MAX];

/* Shuts one down in the right order: the TLS close notification has to go
   out over a connection that is still up, so it goes first. */
static void sock_drop(int h) {
    if (h < 0 || h >= SOCK_MAX || !socks[h].open) return;
    if (socks[h].secure) { tls_close(socks[h].tcp); socks[h].secure = false; }
    tcp_close(socks[h].tcp);
    socks[h].open = false;
}

/* The caller's socket, or nothing. Both halves matter: a number outside the
   table is a mistake, and a number inside it that belongs to somebody else
   is the thing this check exists for. */
static sock_t *sock_of(u64 raw) {
    if (raw >= SOCK_MAX) return 0;
    sock_t *s = &socks[raw];
    if (!s->open || s->owner != caller_pid()) return 0;
    return s;
}

static int sock_take(void) {
    for (int i = 0; i < SOCK_MAX; i++) if (!socks[i].open) return i;
    return -1;
}

static i64 sys_connect(registers_t *r) {
    char host[128];
    if (!copy_path(r->rbx, host, sizeof(host))) return -1;
    u16 port = (u16)r->rcx;
    if (!port) return -1;
    if (!net_up()) return NET_ERR_DOWN;

    int h = sock_take();
    if (h < 0) return NET_ERR_BUSY;          /* every socket is in use */

    ipv4_t ip = net_parse_ip(host);
    if (!ip && !net_resolve(host, &ip, 6000)) return NET_ERR_RESOLVE;

    int t = tcp_open(ip, port, 6000);
    if (t < 0) return NET_ERR_CONNECT;

    socks[h].tcp = t;
    socks[h].owner = caller_pid();
    socks[h].open = true;
    socks[h].secure = false;
    return h;
}

/* The same connection, with the handshake done on it before the caller gets
   it back. The name is needed twice over and for different things: to find
   the address, and to check that the certificate at the other end is for the
   site that was asked for rather than merely for whoever answered.

   Only one of these at a time. The stack holds several connections but the
   TLS session state in kernel/tls.c is still single, so a machine can have
   one encrypted connection and the rest plain. Said out loud rather than
   discovered: a second handshake would quietly take the first one's keys. */
static i64 sys_connect_tls(registers_t *r) {
    char host[128];
    if (!copy_path(r->rbx, host, sizeof(host))) return -1;
    u16 port = (u16)r->rcx;
    if (!port) port = 443;
    if (!net_up()) return NET_ERR_DOWN;

    for (int i = 0; i < SOCK_MAX; i++)
        if (socks[i].open && socks[i].secure) return NET_ERR_BUSY;

    int h = sock_take();
    if (h < 0) return NET_ERR_BUSY;

    ipv4_t ip = net_parse_ip(host);
    if (!ip && !net_resolve(host, &ip, 6000)) return NET_ERR_RESOLVE;

    int t = tcp_open(ip, port, 6000);
    if (t < 0) return NET_ERR_CONNECT;

    /* A handshake that fails takes the connection with it. Leaving the TCP
       side open after a certificate was refused would let a caller that
       ignored the return value carry on and send the request in the clear,
       to the machine that just failed to prove who it was. */
    if (!tls_connect(t, host)) { tcp_close(t); return NET_ERR_TLS; }

    socks[h].tcp = t;
    socks[h].owner = caller_pid();
    socks[h].open = true;
    socks[h].secure = true;
    return h;
}

/* Why the last handshake on this socket failed, or what its open one
   agreed on. The socket is named now rather than assumed: there is a
   session per connection, so "the last handshake" is no longer a thing the
   machine has one of. */
static i64 sys_tls_status(registers_t *r) {
    u64 buf = r->rbx, cap = r->rcx;
    if (cap == 0 || cap > 256) return -1;
    if (!user_range_ok(buf, cap)) return -1;

    /* The last one, machine wide, which is what this call has always
       meant and is what the asking program can still answer with: a
       handshake that failed has already closed its socket, so there is no
       handle left to name. Every session here agrees the same suite, so
       what was agreed is the same answer whichever one is asked. */
    const char *s = r->rdx == TLS_WHAT ? tls_describe(-1) : tls_error(-1);
    u32 n = (u32)strlen(s);
    if (n + 1 > cap) n = (u32)cap - 1;
    memcpy((void *)buf, s, n);
    ((char *)buf)[n] = 0;
    return (i32)n;
}

static i64 sys_send(registers_t *r) {
    sock_t *s = sock_of(r->rbx);
    if (!s) return -1;
    u64 buf = r->rcx, len = r->rdx;
    /* A plain send is one segment, because that is what the stack writes in
       one go. TLS makes its own records and splits them itself, so the limit
       there is the record size rather than the segment. */
    if (len == 0 || len > (s->secure ? 8192u : 1400u)) return -1;
    if (!user_range_ok(buf, len)) return -1;
    if (s->secure)
        return tls_send(s->tcp, (const void *)buf, (u32)len) ? (i32)len : -1;
    return tcp_send(s->tcp, (const void *)buf, (u16)len) ? (i32)len : -1;
}

/* Reads what has arrived, and says which kind of nothing it got.
 *
 * Zero used to mean both "not yet" and "that was all", which a program
 * reading in a loop cannot tell apart: it either stops early on a slow
 * server or waits forever on a finished one. NET_EOF is the second. */
static i64 sys_recv(registers_t *r) {
    sock_t *s = sock_of(r->rbx);
    if (!s) return -1;
    u64 buf = r->rcx, len = r->rdx;
    if (len == 0 || len > 65536) return -1;
    if (!user_range_ok(buf, len)) return -1;

    if (s->secure) {
        u32 n = tls_recv(s->tcp, (u8 *)buf, len, 4000);
        if (n) return (i32)n;
        /* A finished TLS connection is one that said so in an alert, or one
           whose carrier stopped. The second is not a clean ending and is
           reported the same way, because a caller can do nothing different
           about it and the alternative is waiting forever. */
        return (tls_ended(s->tcp) || tcp_ended(s->tcp)) ? -2 : 0;
    }

    u32 n = tcp_recv(s->tcp, (u8 *)buf, len, 4000);
    if (n) return (i32)n;
    return tcp_ended(s->tcp) ? -2 : 0;
}

static i64 sys_disconnect(registers_t *r) {
    sock_t *s = sock_of(r->rbx);
    if (!s) return -1;
    sock_drop((int)r->rbx);
    return 0;
}

/* Frees whatever a program still held when it died, so a crash does not
   leave connections open that nothing will ever close. */
void syscall_release(u32 pid) {
    for (int i = 0; i < SOCK_MAX; i++)
        if (socks[i].open && socks[i].owner == pid) sock_drop(i);
}

static i64 sys_resolve(registers_t *r) {
    char host[128];
    if (!copy_path(r->rbx, host, sizeof(host))) return -1;
    if (!user_range_ok(r->rcx, 4)) return -1;
    if (!net_up()) return -1;

    ipv4_t ip = net_parse_ip(host);
    if (!ip && !net_resolve(host, &ip, 6000)) return -1;
    *(u32 *)r->rcx = ip;
    return 0;
}

static i64 sys_netinfo(registers_t *r) {
    if (!user_range_ok(r->rbx, sizeof(zelr_netinfo_t))) return -1;
    zelr_netinfo_t info;
    memset(&info, 0, sizeof(info));
    info.up = net_up() ? 1 : 0;
    if (info.up) {
        info.ip = net_ip();
        info.gateway = net_gateway();
        info.netmask = net_netmask();
        info.dns = net_dns();
        memcpy(info.mac, net_mac(), 6);
    }
    memcpy((void *)r->rbx, &info, sizeof(info));
    return 0;
}

/* --- the window server ---------------------------------------------------

   Everything below is reached only through these calls. A program never sees
   a window_t, only a handle it was given, and the handle is checked against
   the caller's pid every time so one program cannot drive another's window. */

static i64 sys_win_create(registers_t *r) {
    u64 name_addr = r->rbx;
    int cw = (int)r->rcx, ch = (int)r->rdx;

    char title[32];
    const char *src = (const char *)name_addr;
    u32 i = 0;
    for (; i < sizeof(title) - 1; i++) {
        if (!user_range_ok(name_addr + i, 1)) return -1;
        title[i] = src[i];
        if (!title[i]) break;
    }
    title[sizeof(title) - 1] = 0;

    return winsrv_create(caller_pid(), title, cw, ch);
}

static i64 sys_win_surface(registers_t *r) {
    /* This is the one system call whose result is a pointer, which is why
       the whole table returns a machine word rather than an int. */
    u64 addr = winsrv_surface(caller_pid(), (int)r->rbx,
                              paging_current_directory());
    return (i64)addr;
}

static i64 sys_win_size(registers_t *r) {
    return winsrv_size(caller_pid(), (int)r->rbx);
}

static i64 sys_win_poll(registers_t *r) {
    u64 out = r->rcx;
    if (!user_range_ok(out, sizeof(wm_event_t))) return -1;

    wm_event_t ev;
    if (!winsrv_poll(caller_pid(), (int)r->rbx, &ev)) return 0;
    memcpy((void *)out, &ev, sizeof(ev));
    return 1;
}

/* What a window is showing, as words, so the desktop's find can look
   through it. A window that never calls this is not searched and is not
   pretended to be: find says how many windows it could look in. */
static i64 sys_win_text(registers_t *r) {
    u64 addr = r->rcx;
    int len = (int)r->rdx;
    if (len < 0) len = 0;
    if (len > WM_TEXT_MAX) len = WM_TEXT_MAX;
    if (len && !user_range_ok(addr, (u64)len)) return -1;

    window_t *w = winsrv_window(caller_pid(), (int)r->rbx);
    if (!w) return -1;
    wm_set_text(w, (const char *)addr, len);
    return len;
}

/* And what is being looked for, for a window that has just been told that
   somebody is. */
static i64 sys_win_find(registers_t *r) {
    u64 addr = r->rbx;
    int cap = (int)r->rcx;
    if (cap <= 0 || !user_range_ok(addr, (u64)cap)) return -1;
    return wm_find_query((char *)addr, cap);
}

static i64 sys_win_commit(registers_t *r) {
    return winsrv_commit(caller_pid(), (int)r->rbx) ? 0 : -1;
}

static i64 sys_win_close(registers_t *r) {
    return winsrv_close(caller_pid(), (int)r->rbx) ? 0 : -1;
}

static i64 sys_sysinfo(registers_t *r) {
    if (!user_range_ok(r->rbx, sizeof(zelr_sysinfo_t))) return -1;

    zelr_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    info.cpus_found = smp_cpu_count();
    info.cpus_started = smp_started();
    info.mem_total_kb = pmm_total_frames() * 4;
    info.mem_used_kb = pmm_used_frames() * 4;
    info.mem_free_kb = pmm_free_frames() * 4;
    info.heap_total_kb = heap_total() / 1024;
    info.uptime_seconds = (u32)(timer_ticks() / timer_hz());
    info.tasks = task_count();
    info.screen_w = fb_active() ? fb_width() : 0;
    info.screen_h = fb_active() ? fb_height() : 0;
    info.syscalls = syscall_count();
    info.disk_kb_free = fat_mounted() ? fat_free_bytes() / 1024 : 0;

    memcpy((void *)r->rbx, &info, sizeof(info));
    return 0;
}

static u32 served;
u32 syscall_count(void) { return served; }

typedef i64 (*syscall_fn)(registers_t *);

static const syscall_fn TABLE[] = {
    [SYS_EXIT]      = sys_exit,
    [SYS_PUTC]      = sys_putc,
    [SYS_WRITE]     = sys_write,
    [SYS_GETPID]    = sys_getpid,
    [SYS_TICKS]     = sys_ticks,
    [SYS_SLEEP]     = sys_sleep,
    [SYS_READ_FILE] = sys_read_file,
    [SYS_WIN_CREATE]  = sys_win_create,
    [SYS_WIN_SURFACE] = sys_win_surface,
    [SYS_WIN_SIZE]    = sys_win_size,
    [SYS_WIN_POLL]    = sys_win_poll,
    [SYS_WIN_COMMIT]  = sys_win_commit,
    [SYS_WIN_TEXT]    = sys_win_text,
    [SYS_WIN_FIND]    = sys_win_find,
    [SYS_WIN_CLOSE]   = sys_win_close,
    [SYS_OPEN]        = sys_open,
    [SYS_CLOSE]       = sys_close,
    [SYS_FREAD]       = sys_fread,
    [SYS_FWRITE]      = sys_fwrite,
    [SYS_SEEK]        = sys_seek,
    [SYS_UNLINK]      = sys_unlink,
    [SYS_MKDIR]       = sys_mkdir,
    [SYS_RMDIR]       = sys_rmdir,
    [SYS_READDIR]     = sys_readdir,
    [SYS_STAT]        = sys_stat,
    [SYS_CHDIR]       = sys_chdir,
    [SYS_GETCWD]      = sys_getcwd,
    [SYS_CONNECT]     = sys_connect,
    [SYS_TLS_CONNECT] = sys_connect_tls,
    [SYS_TLS_STATUS]  = sys_tls_status,
    [SYS_SEND]        = sys_send,
    [SYS_RECV]        = sys_recv,
    [SYS_DISCONNECT]  = sys_disconnect,
    [SYS_RESOLVE]     = sys_resolve,
    [SYS_NETINFO]     = sys_netinfo,
    [SYS_SYSINFO]     = sys_sysinfo,
    [SYS_SPAWN]       = sys_spawn,
    [SYS_WAIT]        = sys_wait,
    [SYS_KILL]        = sys_kill,
    [SYS_TASKS]       = sys_tasks,
    [SYS_WIN_RESIZABLE] = sys_win_resizable,
    [SYS_WIN_RESIZE]    = sys_win_resize,
    [SYS_CLIP_SET]  = sys_clip_set,
    [SYS_CLIP_GET]  = sys_clip_get,
    [SYS_SOUND_INFO]  = sys_sound_info,
    [SYS_SOUND_WRITE] = sys_sound_write,
    [SYS_POWER]       = sys_power,
    [SYS_SPAWN_ARGV]  = sys_spawn_argv,
    [SYS_SBRK]        = sys_sbrk,
    [SYS_FORK]        = sys_fork,
    [SYS_EXEC]        = sys_exec,
    [SYS_GETPPID]     = sys_getppid,
    [SYS_DUP]         = sys_dup,
    [SYS_DUP2]        = sys_dup2,
    [SYS_PIPE]        = sys_pipe,
    [SYS_SIGNAL]      = sys_signal,
    [SYS_SIGSEND]     = sys_sigsend,
    [SYS_SIGRETURN]   = sys_sigreturn,
    [SYS_MMAP]        = sys_mmap,
    [SYS_MUNMAP]      = sys_munmap,
    [SYS_FSYNC]       = sys_fsync,
    [SYS_RENAME]      = sys_rename,
    [SYS_POLL]        = sys_poll,
};

#define N_SYSCALLS (sizeof(TABLE) / sizeof(TABLE[0]))

static void syscall_handler(registers_t *r) {
    served++;
    u64 n = r->rax;
    if (n >= N_SYSCALLS || !TABLE[n]) {
        r->rax = (u64)-1;
        return;
    }
    r->rax = (u64)TABLE[n](r);
}

void syscall_init(void) {
    register_interrupt_handler(0x80, syscall_handler);
}
