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
#include "io.h"
#include "welcome.h"
#include "serial.h"

#define LINE_MAX 256
#define ARG_MAX  16

static void reboot_now(void);

static char line[LINE_MAX];

static u32 split(char *s, char **argv, u32 max) {
    u32 n = 0;
    while (*s && n < max) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
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
    kprintf("  PID  STATE     SLICES  NAME\n");
    const char *st[] = { "ready", "running", "sleeping", "dead" };
    task_t *p = t;
    do {
        kprintf("  %3d  %-8s  %6d  %s\n", p->pid, st[p->state], p->slices, p->name);
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

static void execute(char *buf) {
    char *argv[ARG_MAX];
    u32 argc = split(buf, argv, ARG_MAX);
    if (!argc) return;
    const char *c = argv[0];

    if (!strcmp(c, "help")) cmd_help();
    else if (!strcmp(c, "guide")) guide_print();
    else if (!strcmp(c, "desktop")) {
        if (!fb_active()) { kprintf("the desktop needs a framebuffer" "\n"); return; }

        /* Open with a terminal, which is the most useful thing to have
           there. Everything else is on the launcher. It is an ordinary ring
           3 program and draws on its own, through the window server, while
           this task runs the compositor. */
        u32 psize = 0;
        u8 *pimg = vfs_slurp("/bin/term", &psize);
        if (pimg) {
            int rc = user_spawn_elf("term", pimg, psize);
            if (rc < 0) kprintf("desktop: term: %s" "\n", elf_error(rc));
            kfree(pimg);
        }
        wm_run();
        /* the desktop owned the screen; give the console its own back */
        vga_clear();
        kprintf("back at the shell" "\n");
    }
    else if (!strcmp(c, "bg")) {
        if (argc < 2) { kprintf("usage: bg PROGRAM" "\n"); return; }
        u32 size = 0;
        u8 *img = vfs_slurp(argv[1], &size);
        if (!img) { kprintf("bg: %s: no such file" "\n", argv[1]); return; }
        int rc = user_spawn_elf(argv[1], img, size);
        kfree(img);
        if (rc > 0) kprintf("[%d] %s running in the background" "\n", rc, argv[1]);
        else kprintf("bg: %s: %s" "\n", argv[1], elf_error(rc));
    } else if (!strcmp(c, "exec")) {
        if (argc < 2) { kprintf("usage: exec PROGRAM" "\n" "e.g. exec hello" "\n"); return; }
        u32 size = 0;
        u8 *img = vfs_slurp(argv[1], &size);
        if (!img) { kprintf("exec: %s: no such file" "\n", argv[1]); return; }
        int rc = user_spawn_elf(argv[1], img, size);
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
        if (!net_up()) { kprintf("no network card\n"); return; }
        const u8 *m = net_mac();
        char b[20];
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
        if (argc < 2) { kprintf("usage: fetch HOST [PATH] [SAVEAS]\n"); return; }
        if (!net_ip()) { kprintf("no address yet, run: dhcp\n"); return; }
        const char *path = argc > 2 ? argv[2] : "/";
        const char *save = argc > 3 ? argv[3] : 0;
        int rc = http_get(argv[1], path, save);
        if (rc == HTTP_ERR_RESOLVE) kprintf("cannot resolve %s\n", argv[1]);
        else if (rc == HTTP_ERR_CONNECT) kprintf("could not connect\n");
        else if (rc == HTTP_ERR_TOOLONG) kprintf("the host and path do not fit in a request\n");
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
    } else if (!strcmp(c, "reboot")) {
        kprintf("rebooting\n");
        reboot_now();
    } else {
        kprintf("%s: not found (try help)\n", c);
    }
}

/* Restarting the machine, in the three ways there are.
 *
 * The keyboard controller is the old way and the one every emulator answers
 * to. A laptop often has no keyboard controller at all, and a port that
 * nothing answers reads back 0xFF: waiting for a bit in it to clear waits
 * for a bit that is always set, which is a machine that locks up on the word
 * reboot and comes back only on the power button. The loop below is bounded
 * for that reason rather than for tidiness.
 *
 * After it, the reset control register, which is where firmware without an
 * 8042 puts the same function. And if neither answered, a triple fault:
 * taking an interrupt with no table to look it up in is not a request the
 * machine can decline.
 */
static void reboot_now(void) {
    for (u32 i = 0; i < 100000; i++)
        if (!(inb(0x64) & 0x02)) break;
    outb(0x64, 0xFE);
    io_wait();

    outb(0xCF9, 0x02);                  /* reset control, prepared */
    outb(0xCF9, 0x06);                  /* and asked for */
    io_wait();

    struct { u16 limit; u64 base; } __attribute__((packed)) none = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" :: "m"(none));
    for (;;) hlt();
}

void shell_task(void) {
    welcome_print();
    u32 len = 0;
    prompt();
    for (;;) {
        int ch = kbd_trygetchar();
        if (ch < 0) { hlt(); continue; }   /* woken by the timer or a key */
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
