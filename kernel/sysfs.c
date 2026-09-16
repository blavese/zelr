/* The live tree.
 *
 * Every file here is a function. Reading /sys/memory runs the code that asks
 * the frame allocator how much is left and formats the answer; nothing is
 * stored between reads, so what comes back is what is true now.
 *
 * The alternative was a pile of shell commands that each print their own
 * thing, which is what this replaces. A file can be read by any program, not
 * just the shell, and it can be piped, copied and searched with the same
 * tools as anything else. That is the whole reason for putting kernel state
 * in the filesystem rather than behind commands.
 *
 * /bin is here for a different reason: the programs built into the kernel
 * image have to live somewhere, and the root is where the user's files are.
 */
#include "sysfs.h"
#include "usb.h"
#include "usbdisk.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "heap.h"
#include "sched.h"
#include "timer.h"
#include "smp.h"
#include "blockdev.h"
#include "netdev.h"
#include "net.h"
#include "fb.h"
#include "vfs.h"
#include "wait.h"
#include "blackbox.h"
#include "clipboard.h"
#include "rtc.h"

/* --- built-in programs, which are what /bin holds ----------------------- */

#define MAX_PROGRAMS 16

typedef struct {
    const char *name;
    const u8   *data;
    u32         size;
} program_t;

static program_t programs[MAX_PROGRAMS];
static u32 n_programs;

void sysfs_add_program(const char *name, const u8 *data, u32 size) {
    if (n_programs >= MAX_PROGRAMS) return;
    programs[n_programs].name = name;
    programs[n_programs].data = data;
    programs[n_programs].size = size;
    n_programs++;
}

u32 sysfs_program_count(void) { return n_programs; }

static const program_t *program_named(const char *name) {
    for (u32 i = 0; i < n_programs; i++)
        if (strcmp(programs[i].name, name) == 0) return &programs[i];
    return 0;
}

/* --- the generated files ------------------------------------------------ */

/* A tiny appender, because every one of these builds a few lines and
   checking the remaining space at each step by hand would bury what the
   file actually says. */
typedef struct { char *buf; u32 cap; u32 len; } out_t;

static void put(out_t *o, const char *fmt, ...) {
    if (o->len >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    char line[256];
    kvformat(line, sizeof(line), fmt, ap);
    va_end(ap);
    for (u32 i = 0; line[i] && o->len < o->cap - 1; i++) o->buf[o->len++] = line[i];
    o->buf[o->len] = 0;
}

static u32 render_version(char *b, u32 cap) {
    out_t o = { b, cap, 0 };
    put(&o, "name      %s\n", KERNEL_NAME);
    put(&o, "version   %s\n", KERNEL_VERSION);
    put(&o, "mode      x86-64 long mode\n");
    put(&o, "programs  %d built in\n", n_programs);
    return o.len;
}

static u32 render_memory(char *b, u32 cap) {
    out_t o = { b, cap, 0 };
    u64 total = pmm_total_frames(), used = pmm_used_frames();
    put(&o, "frames    %d total, %d used, %d free\n",
        (u32)total, (u32)used, (u32)(total - used));
    put(&o, "physical  %d KiB total, %d KiB free\n",
        (u32)(total * 4), (u32)((total - used) * 4));
    put(&o, "heap      %d KiB of %d KiB used\n",
        heap_used() / 1024, heap_total() / 1024);
    return o.len;
}

static const char *state_name(int s) {
    switch (s) {
        case TASK_READY:    return "ready";
        case TASK_RUNNING:  return "running";
        case TASK_SLEEPING: return "sleeping";
        case TASK_BLOCKED:  return "blocked";
        default:            return "dead";
    }
}

static u32 render_tasks(char *b, u32 cap) {
    out_t o = { b, cap, 0 };
    put(&o, "pid  state     ring  slices  name\n");

    task_t *head = task_list();
    if (!head) return o.len;
    task_t *p = head;
    do {
        put(&o, "%-4d %-9s %-5s %-7d %s\n",
            p->pid, state_name(p->state), p->user ? "3" : "0",
            p->slices, p->name);
        p = p->next;
    } while (p != head);
    return o.len;
}

static u32 render_uptime(char *b, u32 cap) {
    out_t o = { b, cap, 0 };
    u64 ticks = timer_ticks();
    u32 hz = timer_hz();
    u32 secs = hz ? (u32)(ticks / hz) : 0;
    put(&o, "ticks     %d at %d Hz\n", (u32)ticks, hz);
    put(&o, "uptime    %dh %dm %ds\n", secs / 3600, (secs / 60) % 60, secs % 60);
    put(&o, "tasks     %d, %d of them blocked\n",
        task_count(), task_blocked_count());
    put(&o, "wakeups   %d, %d blocked now\n", wait_wakeups(), wait_blocked_now());
    return o.len;
}

static u32 render_cpu(char *b, u32 cap) {
    out_t o = { b, cap, 0 };
    put(&o, "described %d\n", smp_cpu_count());
    put(&o, "started   %d\n", smp_started());
    for (u32 i = 0; i < smp_cpu_count(); i++) {
        const cpu_t *c = smp_cpu(i);
        if (!c) continue;
        put(&o, "cpu%-6d apic %d, %-8s %d jobs\n",
            i, c->apic_id, c->started ? "running" : "halted", c->jobs);
    }
    return o.len;
}

static u32 render_devices(char *b, u32 cap) {
    out_t o = { b, cap, 0 };
    if (blk_present())
        put(&o, "disk      %s via %s, %d MiB\n",
            blk_model(), blk_driver(), blk_sectors() / 2048);
    else
        put(&o, "disk      none\n");

    if (netdev_up()) {
        const u8 *m = netdev_mac();
        put(&o, "network   %s %02x:%02x:%02x:%02x:%02x:%02x\n",
            netdev_name(), m[0], m[1], m[2], m[3], m[4], m[5]);
    } else {
        put(&o, "network   none\n");
    }

    if (fb_active()) put(&o, "video     %dx%d 32bpp\n", fb_width(), fb_height());
    else             put(&o, "video     vga text\n");

    if (usb_present())
        put(&o, "usb       %s, %d report(s)\n", usb_describe(), usb_reports());
    else
        put(&o, "usb       no controller\n");
    put(&o, "keyboard  ps/2%s\n", usb_keyboards() ? " and usb" : "");
    put(&o, "storage   %s\n", vfs_disk_backed() ? "fat16 on disk" : "memory only");
    if (usbdisk_present())
        put(&o, "usbdisk   %s, %d sector(s) of %d bytes\n",
            usbdisk_model(), usbdisk_sectors(), usbdisk_block_size());
    return o.len;
}

static u32 render_net(char *b, u32 cap) {
    out_t o = { b, cap, 0 };
    if (!netdev_up()) { put(&o, "no network card\n"); return o.len; }

    char ip[20];
    net_format_ip(net_ip(), ip);      put(&o, "address   %s\n", ip);
    net_format_ip(net_netmask(), ip); put(&o, "netmask   %s\n", ip);
    net_format_ip(net_gateway(), ip); put(&o, "gateway   %s\n", ip);
    net_format_ip(net_dns(), ip);     put(&o, "dns       %s\n", ip);
    put(&o, "state     %s\n", net_up() ? "configured" : "no address yet");
    put(&o, "packets   %d in, %d out\n", net_rx_packets(), net_tx_packets());
    return o.len;
}

static u32 render_programs(char *b, u32 cap) {
    out_t o = { b, cap, 0 };
    for (u32 i = 0; i < n_programs; i++)
        put(&o, "%-16s %d bytes\n", programs[i].name, programs[i].size);
    return o.len;
}

/* --- the tree ----------------------------------------------------------- */

typedef struct {
    const char     *path;
    sysfs_render_t  render;
} node_t;

/* This boot's log, as the black box has it. Copied rather than rendered:
   the text is already formatted, and reformatting it here would mean two
   places that decide what a log line looks like. */
static u32 render_boot(char *b, u32 cap) {
    const char *t = bb_text();
    u32 n = bb_len();
    if (n > cap - 1) {
        /* Keep the end. The last line is the one being looked for. */
        t += n - (cap - 1);
        n = cap - 1;
    }
    memcpy(b, t, n);
    b[n] = 0;
    return n;
}

/* The boot before this one, off the disk. This is the file that matters
   after a machine has failed to come up: reboot it from something that
   works, read this, and the last mark says where it died. */
static u32 render_lastboot(char *b, u32 cap) {
    u32 n = bb_prev(b, cap);
    if (n) return n;
    out_t o = { b, cap, 0 };
    put(&o, "no record\n");
    put(&o, "\n");
    put(&o, "There is one only after a boot that reached the scheduler, or\n");
    put(&o, "that panicked, on a volume this kernel formatted.\n");
    return o.len;
}

/* What is on the clipboard. Reading it from a shell is how you find out
   whether a copy in one window actually reached the thing a paste in
   another would read, which is not a question the programs can answer
   about each other. */
static u32 render_clipboard(char *b, u32 cap) {
    u32 n = clip_get(b, cap);
    if (n == 0) {
        out_t o = { b, cap, 0 };
        put(&o, "(empty)\n");
        return o.len;
    }
    return n;
}

/* The wall clock, which is the one thing here that does not come from
   counting since boot. */
static u32 render_time(char *b, u32 cap) {
    out_t o = { b, cap, 0 };
    char now[24];
    rtc_format(now, sizeof(now));
    put(&o, "now       %s\n", now);
    put(&o, "source    %s\n", rtc_present() ? "cmos clock" : "none");
    put(&o, "uptime    %d s\n", (u32)(timer_ticks() / (timer_hz() ? timer_hz() : 100)));
    return o.len;
}

/* What the screen is, written for a program to read rather than for a
   person. The settings window offers a list of sizes, and whether
   offering one is honest depends on how this screen was obtained: a
   mode this kernel set can be set again, and one the firmware handed
   over cannot be changed at all once its boot services are gone. */
static u32 render_screen(char *b, u32 cap) {
    out_t o = { b, cap, 0 };
    put(&o, "width %d\n", fb_active() ? fb_width() : 0);
    put(&o, "height %d\n", fb_active() ? fb_height() : 0);
    put(&o, "settable %d\n", fb_mode_settable() ? 1 : 0);
    put(&o, "source %s\n", fb_backend());
    return o.len;
}

static const node_t nodes[] = {
    { "/sys/version",  render_version  },
    { "/sys/memory",   render_memory   },
    { "/sys/tasks",    render_tasks    },
    { "/sys/uptime",   render_uptime   },
    { "/sys/cpu",      render_cpu      },
    { "/sys/devices",  render_devices  },
    { "/sys/net",      render_net      },
    { "/sys/programs", render_programs },
    { "/sys/boot",     render_boot     },
    { "/sys/lastboot", render_lastboot },
    { "/sys/clipboard", render_clipboard },
    { "/sys/time",     render_time      },
    { "/sys/screen",   render_screen    },
};
#define N_NODES (sizeof(nodes) / sizeof(nodes[0]))

void sysfs_init(void) { n_programs = 0; }

/* Rendering into a scratch buffer is how the size of a generated file is
   found, because there is no other way to know it without producing it. One
   buffer is enough: nothing here is re-entrant, and it is only ever touched
   with the caller's own stack live. */
static char scratch[SYSFS_MAX];

bool sysfs_owns(const char *abs) {
    return strncmp(abs, "/sys", 4) == 0 && (abs[4] == 0 || abs[4] == '/');
}

/* True for the directories the tree provides. */
static bool is_dir_path(const char *abs) {
    return strcmp(abs, "/sys") == 0 || strcmp(abs, "/bin") == 0;
}

static const node_t *node_at(const char *abs) {
    for (u32 i = 0; i < N_NODES; i++)
        if (strcmp(nodes[i].path, abs) == 0) return &nodes[i];
    return 0;
}

/* "/bin/name" with no further slashes. */
static const char *bin_name(const char *abs) {
    if (strncmp(abs, "/bin/", 5) != 0) return 0;
    const char *name = abs + 5;
    if (!*name) return 0;
    for (const char *p = name; *p; p++) if (*p == '/') return 0;
    return name;
}

bool sysfs_stat(const char *abs, u32 *size_out, bool *dir_out) {
    if (is_dir_path(abs)) {
        if (size_out) *size_out = 0;
        if (dir_out)  *dir_out = true;
        return true;
    }

    const node_t *n = node_at(abs);
    if (n) {
        if (size_out) *size_out = n->render(scratch, sizeof(scratch));
        if (dir_out)  *dir_out = false;
        return true;
    }

    const char *name = bin_name(abs);
    if (name) {
        const program_t *p = program_named(name);
        if (!p) return false;
        if (size_out) *size_out = p->size;
        if (dir_out)  *dir_out = false;
        return true;
    }
    return false;
}

int sysfs_read(const char *abs, void *buf, u32 cap) {
    const node_t *n = node_at(abs);
    if (n) {
        u32 len = n->render(scratch, sizeof(scratch));
        if (len > cap) len = cap;
        memcpy(buf, scratch, len);
        return (int)len;
    }

    const char *name = bin_name(abs);
    if (name) {
        const program_t *p = program_named(name);
        if (!p) return -1;
        u32 len = p->size < cap ? p->size : cap;
        memcpy(buf, p->data, len);
        return (int)len;
    }
    return -1;
}

int sysfs_list(const char *abs, u32 index, char *name_out, u32 *size_out, bool *dir_out) {
    if (strcmp(abs, "/sys") == 0) {
        if (index >= N_NODES) return 0;
        const char *leaf = nodes[index].path + 5;    /* past "/sys/" */
        if (name_out) { strncpy(name_out, leaf, VFS_NAME_MAX - 1); name_out[VFS_NAME_MAX - 1] = 0; }
        if (size_out) *size_out = nodes[index].render(scratch, sizeof(scratch));
        if (dir_out)  *dir_out = false;
        return 1;
    }

    if (strcmp(abs, "/bin") == 0) {
        if (index >= n_programs) return 0;
        if (name_out) { strncpy(name_out, programs[index].name, VFS_NAME_MAX - 1); name_out[VFS_NAME_MAX - 1] = 0; }
        if (size_out) *size_out = programs[index].size;
        if (dir_out)  *dir_out = false;
        return 1;
    }
    return 0;
}
