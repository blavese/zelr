/* Interactive shell. Reads from the PS/2 keyboard or the serial line,
   whichever produces a character first, so it can be driven by a person at
   the console or by a script piping into QEMU. */
#include "shell.h"
#include "printf.h"
#include "string.h"
#include "keyboard.h"
#include "vga.h"
#include "heap.h"
#include "pmm.h"
#include "timer.h"
#include "sched.h"
#include "smp.h"
#include "fs.h"
#include "vfs.h"
#include "blockdev.h"
#include "diskfs.h"
#include "fat.h"
#include "net.h"
#include "netdev.h"
#include "http.h"
#include "mouse.h"
#include "user.h"
#include "wm.h"
#include "apps.h"
#include "fb.h"
#include "elf.h"
#include "usbdisk.h"
#include "sound.h"
#include "power.h"
#include "io.h"
#include "welcome.h"
#include "serial.h"
#include "theme.h"
#include "fbcon.h"

#define LINE_MAX 256
#define ARG_MAX  16

static char line[LINE_MAX];

static u32 num(const char *s) {
    u32 v = 0;
    for (u32 i = 0; s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (u32)(s[i] - '0');
    return v;
}

/* A line into words, taken apart where it lies.
 *
 * Double quotes hold a word together and are removed; there is no escaping
 * and no single quote, which is what userland/args.h decided and this now
 * matches. The two shells splitting a line differently is worse than either
 * rule: it means the same command means two things depending on which one
 * is in front of you.
 *
 * It mattered less when the words were glued back into a single string on
 * the way to a program. They are handed over as words now, so where a word
 * ends is the program's business and not just the shell's. */
static u32 split(char *s, char **argv, u32 max) {
    u32 n = 0;
    while (*s && n < max) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;

        if (*s == '"') {
            *s++ = 0;
            argv[n++] = s;
            while (*s && *s != '"') s++;
            if (*s) *s++ = 0;
            continue;
        }

        argv[n++] = s;
        while (*s && *s != ' ') s++;
    }
    return n;
}

static void cmd_help(void) {
    kprintf("commands:\n"
            "  guide           a short tour, start here\n"
            "  desktop         windows, a mouse and a paint program\n"
            "  help            this text\n"
            "  ls [PATH]       list a directory\n"
            "  cd [PATH]       change directory\n"
            "  pwd             print the current directory\n"
            "  mkdir NAME      make a directory\n"
            "  rmdir NAME      remove an empty directory\n"
            "  cat NAME        print a file\n"
            "  write NAME TEXT create or overwrite a file\n"
            "  append NAME TXT add a line to a file\n"
            "  cp SRC DST      copy a file\n"
            "  rm NAME         delete a file\n"
            "  disk            show the attached disk\n"
            "  sync            force a write to disk\n"
            "  format          erase the disk and start clean\n"
            "  net             network status\n"
            "  dhcp            ask the network for an address\n"
            "  ping ADDRESS    ping a host by ip or name\n"
            "  resolve HOST    look up a hostname\n"
            "  fetch HOST [PATH] [FILE]   download a page over http\n"
            "  exec PROGRAM    run an elf program and wait for it\n"
            "  bg PROGRAM      run one in the background\n"
            "  ring3           run the built-in ring 3 test\n"
            "  ps              list tasks\n"
            "  mem             memory usage\n"
            "  cpus            processors found and started\n"
            "  uptime          time since boot\n"
            "  uname           kernel identity\n"
            "  echo TEXT       print text\n"
            "  clear           clear the screen\n"
            "  spawn           start a background counter task\n"
            "  fault           deliberately divide by zero\n"
            "  reboot          reset the machine\n");
}

static void cmd_ls(const char *path) {
    char where[VFS_PATH_MAX];
    if (!vfs_resolve(path ? path : ".", where, sizeof(where))) {
        kprintf("ls: bad path\n");
        return;
    }
    bool is_dir = false;
    if (!(where[0] == '/' && where[1] == 0)) {
        if (!vfs_stat(where, 0, &is_dir)) { kprintf("ls: %s: not found\n", where); return; }
        if (!is_dir) { kprintf("ls: %s: not a directory\n", where); return; }
    }

    u32 files = 0, dirs = 0, bytes = 0;
    for (u32 i = 0; ; i++) {
        char name[VFS_NAME_MAX];
        u32 size = 0;
        bool sub = false;
        if (vfs_list(where, i, name, &size, &sub) != 1) break;
        if (sub) { kprintf("       <dir>  %s/\n", name); dirs++; }
        else     { kprintf("  %10d  %s\n", size, name); files++; bytes += size; }
    }

    if (!files && !dirs) { kprintf("(empty)\n"); return; }
    kprintf("%d file(s) in %d bytes", files, bytes);
    if (dirs) kprintf(", %d director%s", dirs, dirs == 1 ? "y" : "ies");
    kputc('\n');
}

static void cmd_cat(const char *name) {
    u32 size = 0;
    bool is_dir = false;
    if (!vfs_stat(name, &size, &is_dir)) { kprintf("cat: %s: no such file\n", name); return; }
    if (is_dir) { kprintf("cat: %s: is a directory\n", name); return; }

    u8 *data = vfs_slurp(name, &size);
    if (!data) { kprintf("cat: %s: cannot read\n", name); return; }
    for (u32 i = 0; i < size; i++) kputc((char)data[i]);
    if (size && data[size - 1] != '\n') kputc('\n');
    kfree(data);
}

static void join_from(char **argv, u32 argc, u32 start, char *out, u32 cap) {
    u32 o = 0;
    for (u32 i = start; i < argc && o < cap - 1; i++) {
        if (i > start && o < cap - 1) out[o++] = ' ';
        for (const char *p = argv[i]; *p && o < cap - 1; p++) out[o++] = *p;
    }
    out[o] = 0;
}

static void cmd_ps(void) {
    task_t *t = task_list();
    if (!t) { kprintf("(no tasks)\n"); return; }
    /* Slices is every tick the scheduler handed this task. Waiting is the
       ones it spent halted, so the difference is what it actually did. */
    kprintf("  PID  STATE     SLICES  WAITING  NAME\n");
    task_t *p = t;
    do {
        kprintf("  %3d  %-8s  %6d  %7d  %s\n", p->pid,
                task_state_name(p->state),
                p->slices, (u32)p->idle_ticks, p->name);
        p = p->next;
    } while (p != t);
}

static void cmd_mem(void) {
    kprintf("physical: %d KiB total, %d KiB used, %d KiB free\n",
            pmm_total_frames() * 4, pmm_used_frames() * 4, pmm_free_frames() * 4);
    kprintf("heap:     %d KiB total, %d bytes used\n", heap_total() / 1024, heap_used());
    if (vfs_disk_backed()) kprintf("files:    on disk, %d KiB free\n", fat_free_bytes() / 1024);
    else                   kprintf("files:    %d in memory using %d bytes\n", fs_count(), fs_bytes_used());
    kprintf("serial:   irqs=%d got=%d read=%d dropped=%d\n",
            serial_isr_calls(), serial_isr_bytes(), serial_read_bytes(), serial_overruns());
}

static void cmd_uptime(void) {
    u64 t = timer_ticks();
    u32 secs = (u32)(t / timer_hz());
    kprintf("up %d.%02d seconds (%d ticks at %d Hz)\n",
            secs, (u32)((t % timer_hz()) * 100 / timer_hz()), (u32)t, timer_hz());
}

static volatile u32 spawn_n = 0;
static void counter_task(void) {
    for (int i = 0; i < 3; i++) { spawn_n++; task_sleep(400); }
    task_exit();
}

/* The prompt carries the working directory, because a shell with
   directories and no way to see where you are is worse than one without. */
static void prompt(void) {
    const char *at = vfs_cwd();
    if (at[0] == '/' && at[1] == 0) kprintf("zelr> ");
    else                            kprintf("zelr:%s> ", at);
}

/* Opens the desktop, and comes back when it is left.

   A function rather than four lines inside the command, because the
   machine now does this by itself at startup as well. */
static void enter_desktop(void) {
    /* Open with a terminal, which is the most useful thing to have
       there. Everything else is on the launcher. It is an ordinary
       ring 3 program and draws on its own, through the window server,
       while this task runs the compositor. */
    u32 psize = 0;
    u8 *pimg = vfs_slurp("/bin/term", &psize);
    if (pimg) {
        int rc = user_spawn_elf("/bin/term", pimg, psize);
        if (rc < 0) kprintf("desktop: term: %s\n", elf_error(rc));
        kfree(pimg);
    }

    wm_run();

    /* The desktop owned the screen; give the console its own back.
     *
     * Both, because which one is in front depends on whether there is a
     * framebuffer, and clearing the text console on a machine that has one
     * leaves the console's own output arriving on top of the desktop that
     * was there a moment ago. That barely mattered when the desktop was
     * something you typed a command to get into. It is the way out of one
     * the machine opens by itself. */
    if (fb_active()) fbcon_clear();
    vga_clear();
    kprintf("back at the shell\n");
}

/* A name on its own, looked for as a program.
 *
 * The console shell had no such path at all: anything that was not one of
 * its own commands was "not found", and a program had to be started with
 * `exec`. The kernel could always load an ELF off the disk -- every spawn
 * here reads through the same VFS as `cat` -- so what was missing was never
 * the loading, it was the looking, and it was missing in both shells.
 *
 * Here first, then a stick, then /bin. The working directory comes first
 * because a program somebody has just downloaded or copied is the one they
 * mean, and /bin is last so that a name in both runs the one in front of
 * you rather than the one that shipped. */
static bool run_by_name(u32 argc, char **argv) {
    static const char *const where[] = { 0, "/usb", "/bin" };
    char path[VFS_PATH_MAX];

    for (u32 i = 0; i < sizeof(where) / sizeof(where[0]); i++) {
        u32 n = 0;
        if (where[i]) {
            for (const char *p = where[i]; *p && n < sizeof(path) - 2; p++) path[n++] = *p;
            path[n++] = '/';
        }
        /* The name as given covers both a path somebody typed and a bare
           name in the directory the shell is in. */
        for (const char *p = argv[0]; *p && n < sizeof(path) - 1; p++) path[n++] = *p;
        path[n] = 0;

        u32 size = 0;
        u8 *img = vfs_slurp(path, &size);
        if (!img) continue;

        /* Every word, the program's own name first, which is what the
           line already is. */
        int rc = user_spawn_elf_argv(path, img, size, (int)argc,
                                     (const char *const *)argv);
        kfree(img);
        if (rc > 0) { task_wait((u32)rc); return true; }

        /* It was there and it is not a program. Saying so beats carrying on
           to the next directory and then reporting that nothing of that
           name exists, which is the opposite of what was found. */
        kprintf("%s: %s\n", path, elf_error(rc));
        return true;
    }
    return false;
}

static void execute(char *buf) {
    char *argv[ARG_MAX];
    u32 argc = split(buf, argv, ARG_MAX);
    if (!argc) return;
    const char *c = argv[0];

    if (!strcmp(c, "help")) cmd_help();
    else if (!strcmp(c, "guide")) guide_print();
    else if (!strcmp(c, "desktop")) {
        if (!fb_active()) { kprintf("the desktop needs a framebuffer" "\n"); return; }
        enter_desktop();
    }
    else if (!strcmp(c, "bg")) {
        if (argc < 2) { kprintf("usage: bg PROGRAM [ARG ...]" "\n"); return; }
        u32 size = 0;
        u8 *img = vfs_slurp(argv[1], &size);
        if (!img) { kprintf("bg: %s: no such file" "\n", argv[1]); return; }
        int rc = user_spawn_elf_argv(argv[1], img, size, (int)argc - 1,
                                     (const char *const *)(argv + 1));
        kfree(img);
        if (rc > 0) kprintf("[%d] %s running in the background" "\n", rc, argv[1]);
        else kprintf("bg: %s: %s" "\n", argv[1], elf_error(rc));
    } else if (!strcmp(c, "exec")) {
        if (argc < 2) { kprintf("usage: exec PROGRAM [ARG ...]" "\n" "e.g. exec hello" "\n"); return; }
        u32 size = 0;
        u8 *img = vfs_slurp(argv[1], &size);
        if (!img) { kprintf("exec: %s: no such file" "\n", argv[1]); return; }
        /* And everything that followed it. This used to hand over the
           first word only, and before that nothing at all: a program that
           takes a file name could be started from the desktop's shell,
           which passed one, and not from here, so the same program
           behaved differently depending on which shell ran it. */
        int rc = user_spawn_elf_argv(argv[1], img, size, (int)argc - 1,
                                     (const char *const *)(argv + 1));
        kfree(img);
        if (rc > 0) {
            /* Wait for it, the way a shell does, so its output is not
               interleaved with the next prompt. */
            task_wait((u32)rc);
        } else {
            kprintf("exec: %s: %s" "\n", argv[1], elf_error(rc));
        }
    } else if (!strcmp(c, "ring3")) {
        int pid = user_spawn_stub("ring3");
        if (pid > 0) kprintf("started pid %d in ring 3\n", pid);
        else kprintf("could not start it (%d)\n", pid);
    }
    else if (!strcmp(c, "mouse")) {
        if (!mouse_present()) { kprintf("no mouse\n"); return; }
        kprintf("pointer  %d,%d\n", mouse_x(), mouse_y());
        kprintf("buttons  %s%s%s\n",
                (mouse_buttons() & 1) ? "left " : "",
                (mouse_buttons() & 2) ? "right " : "",
                (mouse_buttons() & 4) ? "middle" : "");
        kprintf("moves    %d\n", mouse_moves());
    }
    else if (!strcmp(c, "sync")) {
        /* Writes already go straight through; this only pushes whatever the
           drive is still holding in its own cache. */
        kprintf(diskfs_flush() ? "flushed to disk\n" : "sync: no disk\n");
    } else if (!strcmp(c, "format")) {
        if (!diskfs_available()) { kprintf("format: no disk attached\n"); return; }
        kprintf(diskfs_format() ? "disk formatted\n" : "format failed\n");
    } else if (!strcmp(c, "disk")) {
        if (!blk_present()) { kprintf("no disk attached\n"); return; }
        kprintf("model    %s\n", blk_model());
        kprintf("size     %d sectors (%d MiB)\n", blk_sectors(), blk_sectors() / 2048);
        kprintf("state    %s\n", diskfs_mounted() ? "mounted" : "not mounted");
        if (fat_mounted()) {
            kprintf("format   FAT16, %d clusters of %d bytes\n",
                    fat_total_clusters(), fat_cluster_bytes());
            kprintf("free     %d KiB\n", fat_free_bytes() / 1024);
        }
    }    else if (!strcmp(c, "net")) {
        if (!net_up()) {
            /* A machine with a card nothing here drives is a different
               problem from a machine with no card, and only one of them is
               fixed by plugging something in. */
            u16 uv = 0, ud = 0;
            if (netdev_undriven(&uv, &ud))
                kprintf("card     %04x:%04x on the bus, no driver for it\n", uv, ud);
            else
                kprintf("no network card\n");
            return;
        }
        const u8 *m = net_mac();
        char b[20];
        /* Which card, because there can now be more than one kind and a usb
           adapter arriving is the sort of thing worth being able to see. */
        kprintf("card     %s\n", netdev_name());
        kprintf("mac      %02x:%02x:%02x:%02x:%02x:%02x\n", m[0],m[1],m[2],m[3],m[4],m[5]);
        if (net_ip()) {
            net_format_ip(net_ip(), b);      kprintf("address  %s\n", b);
            net_format_ip(net_netmask(), b); kprintf("netmask  %s\n", b);
            net_format_ip(net_gateway(), b); kprintf("gateway  %s\n", b);
            net_format_ip(net_dns(), b);     kprintf("dns      %s\n", b);
        } else {
            kprintf("address  none, run: dhcp\n");
        }
        kprintf("packets  %d in, %d out\n", net_rx_packets(), net_tx_packets());
    } else if (!strcmp(c, "dhcp")) {
        if (!net_up()) { kprintf("no network card\n"); return; }
        kprintf("asking for an address...\n");
        if (net_dhcp(6000)) {
            char b[20]; net_format_ip(net_ip(), b);
            kprintf("got %s\n", b);
        } else kprintf("no answer\n");
    } else if (!strcmp(c, "ping")) {
        if (argc < 2) { kprintf("usage: ping ADDRESS\n"); return; }
        if (!net_ip()) { kprintf("no address yet, run: dhcp\n"); return; }
        ipv4_t target = net_parse_ip(argv[1]);
        if (!target) {
            if (!net_resolve(argv[1], &target, 4000)) { kprintf("cannot resolve %s\n", argv[1]); return; }
        }
        char b[20]; net_format_ip(target, b);
        for (int i = 0; i < 4; i++) {
            int ms = net_ping(target, 2000);
            if (ms >= 0) kprintf("reply from %s: seq=%d time=%dms\n", b, i + 1, ms);
            else         kprintf("no reply from %s: seq=%d\n", b, i + 1);
        }
    } else if (!strcmp(c, "fetch")) {
        if (argc < 2) { kprintf("usage: fetch [https://]HOST [PATH] [SAVEAS]\n"); return; }
        if (!net_ip()) { kprintf("no address yet, run: dhcp\n"); return; }
        const char *path = argc > 2 ? argv[2] : "/";
        const char *save = argc > 3 ? argv[3] : 0;
        int rc = http_get(argv[1], path, save);
        if (rc == HTTP_ERR_RESOLVE) kprintf("cannot resolve %s\n", argv[1]);
        else if (rc == HTTP_ERR_CONNECT) kprintf("could not connect\n");
        else if (rc == HTTP_ERR_TOOLONG) kprintf("the host and path do not fit in a request\n");
        else if (rc == HTTP_ERR_TLS) { /* http_get already said which check failed */ }
        else if (rc == HTTP_ERR_SEND) kprintf("the request could not be sent\n");
        else if (rc == HTTP_ERR_EMPTY) kprintf("nothing came back\n");
        else if (rc == HTTP_ERR_MEMORY) kprintf("not enough memory for the answer\n");
        else if (rc < 0) kprintf("fetch failed\n");
    } else if (!strcmp(c, "resolve")) {
        if (argc < 2) { kprintf("usage: resolve HOSTNAME\n"); return; }
        ipv4_t ip;
        if (net_resolve(argv[1], &ip, 4000)) {
            char b[20]; net_format_ip(ip, b);
            kprintf("%s is %s\n", argv[1], b);
        } else kprintf("cannot resolve %s\n", argv[1]);
    }
    else if (!strcmp(c, "ls")) cmd_ls(argc > 1 ? argv[1] : 0);
    else if (!strcmp(c, "cat")) {
        if (argc < 2) kprintf("usage: cat NAME\n"); else cmd_cat(argv[1]);
    } else if (!strcmp(c, "write") || !strcmp(c, "append")) {
        if (argc < 3) { kprintf("usage: %s NAME TEXT\n", c); return; }
        char text[LINE_MAX];
        join_from(argv, argc, 2, text, sizeof(text));
        u32 len = (u32)strlen(text);
        text[len++] = '\n';
        bool ok = (c[0] == 'w') ? vfs_write(argv[1], text, len)
                                : vfs_append(argv[1], text, len);
        kprintf(ok ? "ok\n" : "failed\n");
    } else if (!strcmp(c, "cp")) {
        /* The terminal had this and the shell did not, which stopped
           mattering the moment there was a second volume to copy
           between. Whole file at a time: the heap is the limit, and
           anything that does not fit in it does not fit on a floppy
           sized volume either. */
        if (argc < 3) { kprintf("usage: cp SRC DST\n"); return; }

        u32 size = 0;
        if (!vfs_stat(argv[1], &size, 0)) {
            kprintf("cp: %s: no such file\n", argv[1]);
            return;
        }

        u8 *buf = (u8 *)kmalloc(size ? size : 1);
        if (!buf) { kprintf("cp: out of memory\n"); return; }

        int got = vfs_read(argv[1], buf, size);
        if (got < 0) kprintf("cp: %s: cannot read\n", argv[1]);
        else if (!vfs_write(argv[2], buf, (u32)got))
            kprintf("cp: %s: cannot write\n", argv[2]);
        else kprintf("ok\n");
        kfree(buf);
    } else if (!strcmp(c, "rm")) {
        if (argc < 2) kprintf("usage: rm NAME\n");
        else kprintf(vfs_delete(argv[1]) ? "ok\n" : "rm: no such file\n");
    } else if (!strcmp(c, "cd")) {
        const char *where = argc > 1 ? argv[1] : "/";
        if (!vfs_chdir(where)) kprintf("cd: %s: not a directory\n", where);
    } else if (!strcmp(c, "pwd")) {
        kprintf("%s\n", vfs_cwd());
    } else if (!strcmp(c, "mkdir")) {
        if (argc < 2) kprintf("usage: mkdir NAME\n");
        else kprintf(vfs_mkdir(argv[1]) ? "ok\n" : "mkdir: failed\n");
    } else if (!strcmp(c, "rmdir")) {
        if (argc < 2) kprintf("usage: rmdir NAME\n");
        else kprintf(vfs_rmdir(argv[1]) ? "ok\n" : "rmdir: not empty, or not a directory\n");
    } else if (!strcmp(c, "ps")) cmd_ps();
    else if (!strcmp(c, "cpus")) {
        kprintf("found    %d\n", smp_cpu_count());
        kprintf("started  %d\n", smp_started());
        kprintf("  CPU  APIC  ROLE     JOBS\n");
        for (u32 i = 0; i < smp_cpu_count(); i++) {
            const cpu_t *p = smp_cpu(i);
            if (!p) continue;
            kprintf("  %3d  %4d  %-7s  %d%s\n", i, p->apic_id,
                    i == 0 ? "kernel" : "worker", p->jobs,
                    p->started ? "" : "  (did not start)");
        }
        if (smp_cpu_count() > 1)
            kprintf("the kernel runs on cpu 0; the others take work handed to them\n");
    }
    else if (!strcmp(c, "mem")) cmd_mem();
    else if (!strcmp(c, "uptime")) cmd_uptime();
    else if (!strcmp(c, "uname")) kprintf("%s %s x86_64\n", KERNEL_NAME, KERNEL_VERSION);
    else if (!strcmp(c, "clear")) vga_clear();
    else if (!strcmp(c, "echo")) {
        char text[LINE_MAX];
        join_from(argv, argc, 1, text, sizeof(text));
        kprintf("%s\n", text);
    } else if (!strcmp(c, "spawn")) {
        task_t *t = task_create("counter", counter_task);
        kprintf(t ? "spawned pid %d\n" : "spawn failed\n", t ? t->pid : 0);
    } else if (!strcmp(c, "fault")) {
        kprintf("dividing by zero...\n");
        volatile int z = 0;
        volatile int x = 1 / z;
        (void)x;
    } else if (!strcmp(c, "beep")) {
        /* A note, which is the smallest thing that proves the whole
           path works: a converter, a pin, a running stream, and a
           buffer refilled in time. */
        if (!sound_present()) { kprintf("no sound device\n"); return; }

        u32 hz = argc > 1 ? num(argv[1]) : 440;
        u32 ms = argc > 2 ? num(argv[2]) : 400;
        if (hz < 20) hz = 20;
        if (hz > 20000) hz = 20000;      /* past hearing either way */
        if (ms > 10000) ms = 10000;

        kprintf("%d Hz for %d ms\n", hz, ms);
        sound_tone(hz, ms);
        kprintf("done\n");
    } else if (!strcmp(c, "stick")) {
        /* The USB disk, which is not the one the system booted from and so
           is not what `disk` reports on.
         *
           Reading and writing a raw sector is the only way to tell whether
           the driver underneath actually works. A device will happily report
           a capacity it cannot move a single byte of, and enumerating one is
           not the same as talking to it. */
        if (!usbdisk_present()) { kprintf("no usb disk\n"); return; }

        if (argc < 2) {
            kprintf("model    %s\n", usbdisk_model());
            kprintf("size     %d sectors of %d bytes\n",
                    usbdisk_sectors(), usbdisk_block_size());
            return;
        }

        u32 lba = argc > 2 ? num(argv[2]) : 0;
        u32 bytes = usbdisk_block_size();
        u8 *buf = (u8 *)kmalloc(bytes);
        if (!buf) { kprintf("out of memory\n"); return; }

        if (!strcmp(argv[1], "read")) {
            if (!usbdisk_read(lba, 1, buf)) {
                kprintf("read failed at %d\n", lba);
            } else {
                kprintf("sector %d:", lba);
                for (u32 i = 0; i < 16; i++) kprintf(" %x", buf[i]);
                kprintf("\n");
            }
        } else if (!strcmp(argv[1], "write")) {
            u8 fill = argc > 3 ? (u8)num(argv[3]) : 0;
            for (u32 i = 0; i < bytes; i++) buf[i] = fill;
            if (usbdisk_write(lba, 1, buf)) kprintf("wrote sector %d\n", lba);
            else                            kprintf("write failed at %d\n", lba);
        } else {
            kprintf("stick [read|write] <lba> [byte]\n");
        }
        kfree(buf);
    } else if (!strcmp(c, "shutdown") || !strcmp(c, "poweroff")) {
        /* Anything not yet on the disk goes first. A machine that is
           switched off does not come back to finish writing. */
        if (!power_can_off()) {
            kprintf("cannot power off: %s\n", power_describe());
            return;
        }
        kprintf("powering off\n");
        diskfs_flush();
        power_off();
        kprintf("the firmware did not take it\n");
    } else if (!strcmp(c, "reboot")) {
        kprintf("rebooting\n");
        power_reboot();
    } else if (!run_by_name(argc, argv)) {
        kprintf("%s: not a command, and no program of that name here, "
                "on a stick or in /bin\n", c);
    }
}

static bool console_only;

void shell_console_only(void) { console_only = true; }

void shell_task(void) {
    welcome_print();

    /* A machine with a screen opens the desktop by itself, because that is
       what switching a computer on is supposed to do. Escape leaves it and
       lands here, so the console is still one key away.
     *
     * Two things turn it off: the setting, and the word console on the
     * kernel command line. The second exists because a machine being driven
     * down a serial line by something expecting a prompt should not have to
     * press escape to get one, and because there has to be a way back in
     * when the setting is wrong. */
    theme_init();
    if (fb_active() && !console_only && theme()->autodesktop) enter_desktop();

    u32 len = 0;
    prompt();
    for (;;) {
        int ch = kbd_trygetchar();
        if (ch < 0) { task_idle_wait(); continue; }  /* the timer, or a key */
        /* The console has no shortcuts, so a chord is just its character. */
        char c = (char)KEY_CODE(ch);

        if (c == '\n') {
            kputc('\n');
            line[len] = 0;
            execute(line);
            len = 0;
            prompt();
        } else if (c == '\b') {
            if (len) { len--; kputc('\b'); }
        } else if (c >= ' ' && c < 127 && len < LINE_MAX - 2) {
            line[len++] = c;
            kputc(c);
        }
    }
}
