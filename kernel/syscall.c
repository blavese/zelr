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
#include "vfs.h"
#include "wait.h"
#include "elf.h"
#include "user.h"
#include "net.h"
#include "tcp.h"
#include "heap.h"
#include "smp.h"
#include "pmm.h"
#include "fb.h"
#include "fat.h"

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

    /* The name shown in the task list is the file's, not the whole path. */
    const char *name = path;
    for (const char *p = path; *p; p++) if (*p == '/') name = p + 1;

    int rc = user_spawn_elf(name, image, size);
    kfree(image);
    return rc;
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

    /* Everything it holds goes back, exactly as if it had exited. */
    winsrv_release(pid);
    vfs_release(pid);
    syscall_release(pid);
    t->exit_status = -1;
    t->died_at = timer_ticks();
    t->state = TASK_DEAD;
    wake_all(t);
    return 0;
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
    kputc((char)(r->rbx & 0xFF));
    return 1;
}

static i64 sys_write(registers_t *r) {
    u64 buf = r->rcx;
    u64 len = r->rdx;
    if (len > 65536) return -1;
    if (!user_range_ok(buf, len)) return -1;
    const char *p = (const char *)buf;
    for (u32 i = 0; i < len; i++) kputc(p[i]);
    return (i32)len;
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
    return vfs_open(path, r->rcx);
}

static i64 sys_close(registers_t *r) {
    return vfs_close((int)r->rbx) ? 0 : -1;
}

static i64 sys_fread(registers_t *r) {
    u64 buf = r->rcx, len = r->rdx;
    if (len > 1024 * 1024) return -1;
    if (!user_range_ok(buf, len)) return -1;
    return vfs_fd_read((int)r->rbx, (void *)buf, len);
}

static i64 sys_fwrite(registers_t *r) {
    u64 buf = r->rcx, len = r->rdx;
    if (len > 1024 * 1024) return -1;
    if (!user_range_ok(buf, len)) return -1;
    return vfs_fd_write((int)r->rbx, (const void *)buf, len);
}

static i64 sys_seek(registers_t *r) {
    return vfs_fd_seek((int)r->rbx, (i32)r->rcx, r->rdx);
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

   The TCP stack handles one connection at a time, so there is one socket and
   it belongs to whoever opened it. That is a real limit rather than a
   simplification of the interface: two programs cannot both be connected. */

static u32 sock_owner;
static bool sock_open;

static i64 sys_connect(registers_t *r) {
    char host[128];
    if (!copy_path(r->rbx, host, sizeof(host))) return -1;
    u16 port = (u16)r->rcx;
    if (!port) return -1;
    if (!net_up()) return -1;
    if (sock_open) return -1;              /* already in use */

    ipv4_t ip = net_parse_ip(host);
    if (!ip && !net_resolve(host, &ip, 6000)) return -1;
    if (!tcp_connect(ip, port, 6000)) return -1;

    sock_owner = caller_pid();
    sock_open = true;
    return 0;
}

static i64 sys_send(registers_t *r) {
    if (!sock_open || sock_owner != caller_pid()) return -1;
    u64 buf = r->rcx, len = r->rdx;
    if (len == 0 || len > 1400) return -1;
    if (!user_range_ok(buf, len)) return -1;
    return tcp_send((const void *)buf, (u16)len) ? (i32)len : -1;
}

static i64 sys_recv(registers_t *r) {
    if (!sock_open || sock_owner != caller_pid()) return -1;
    u64 buf = r->rcx, len = r->rdx;
    if (len == 0 || len > 65536) return -1;
    if (!user_range_ok(buf, len)) return -1;
    return (i32)tcp_recv((u8 *)buf, len, 4000);
}

static i64 sys_disconnect(registers_t *r) {
    (void)r;
    if (!sock_open || sock_owner != caller_pid()) return -1;
    tcp_close();
    sock_open = false;
    return 0;
}

/* Frees the socket when its owner dies, so a crashed program does not lock
   the only connection the machine has. */
void syscall_release(u32 pid) {
    if (sock_open && sock_owner == pid) { tcp_close(); sock_open = false; }
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
