#include "types.h"
#include "vga.h"
#include "serial.h"
#include "printf.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"
#include "ps2.h"
#include "sound.h"
#include "usb.h"
#include "multiboot.h"
#include "handoff.h"
#include "pmm.h"
#include "paging.h"
#include "heap.h"
#include "sched.h"
#include "fs.h"
#include "blockdev.h"
#include "diskfs.h"
#include "fat.h"
#include "pci.h"
#include "ioapic.h"
#include "lapic.h"
#include "netdev.h"
#include "wifi.h"
#include "net.h"
#include "fb.h"
#include "fbcon.h"
#include "mouse.h"
#include "synaptics.h"
#include "syscall.h"
#include "shell.h"
#include "winsrv.h"
#include "clipboard.h"
#include "rtc.h"
#include "rng.h"
#include "fpu.h"
#include "smp.h"
#include "acpi.h"
#include "vfs.h"
#include "fd.h"
#include "builtin.h"
#include "layout.h"
#include "selftest.h"
#include "string.h"
#include "io.h"
#include "blackbox.h"

extern u8 __kernel_end[];

/* Where the kernel's own memory starts.
 *
 * It was eight megabytes, fixed, with the kernel image and the frame bitmap
 * that describes all of memory squeezed in underneath. The image carries
 * every user program inside it and had grown to seven and a half, which left
 * four hundred kilobytes for the bitmap and no warning at all about what
 * happens when the next program is added: the bitmap gets no room, decides
 * it can describe nothing, and the machine reports no memory and stops.
 *
 * So it follows the image instead, with two megabytes left for the bitmap,
 * which is what it takes to describe the sixty four gigabytes this kernel
 * will map. */
#define BITMAP_ROOM (2ull * 1024 * 1024)

static u64 heap_base(void) {
    u64 a = (u64)__kernel_end + BITMAP_ROOM;
    return (a + 0xFFFFFull) & ~0xFFFFFull;      /* to the next megabyte */
}

/* The floor. The compositor's back buffer and the copy of it the flush
   compares against are the two largest things this kernel ever allocates,
   and how large is decided by the panel: 3 MiB each at 1024x768, 8 at
   1920x1080, 32 at 3840x2160. What too small a heap looks like is worth
   spelling out, because it is not an error message: the allocation fails,
   the framebuffer is never adopted, and the fallback for having no
   framebuffer is VGA text mode, which a machine that booted through UEFI
   does not have. So it boots, and the screen stays black, and nothing says
   why. */
#define HEAP_MIN  (24ull * 1024 * 1024)

/* And the ceiling, and the share.
 *
 * The old answer was sixteen megabytes plus room for the screen, on a
 * machine with 256 MiB and on one with 4 GiB alike. That is the literal
 * reason giving zelr four gigabytes changed nothing whatsoever: nothing in
 * it would ever ask for the difference. A quarter, to half a gigabyte, is
 * the kernel taking a share of what it was given and leaving the rest for
 * the programs, which get theirs a page at a time from the frame allocator
 * and can use all of it. */
#define HEAP_SHARE 4
#define HEAP_CEIL  (512ull * 1024 * 1024)

static u64 heap_bytes = HEAP_MIN;

static u64 usable_total(const handoff_t *h) {
    u64 n = 0;
    for (u64 i = 0; i < h->region_count; i++)
        if (h->regions[i].type == MEM_USABLE) n += h->regions[i].len;
    return n;
}

/* Usable memory that runs without a break from here.
 *
 * The heap is one block, so this is the honest ceiling: memory on the far
 * side of a hole the firmware reserved is still memory and is still handed
 * out a page at a time, but it is not part of this. Regions are not required
 * to be in order, so this grows the end until nothing extends it. */
static u64 usable_run_from(const handoff_t *h, u64 from) {
    u64 end = from;
    for (u64 pass = 0; pass < HANDOFF_MAX_REGIONS; pass++) {
        u64 was = end;
        for (u64 i = 0; i < h->region_count; i++) {
            const mem_region_t *r = &h->regions[i];
            if (r->type != MEM_USABLE) continue;
            if (r->base > end || r->base + r->len <= end) continue;
            end = r->base + r->len;
        }
        if (end == was) break;
    }
    return end > from ? end - from : 0;
}

static u64 heap_size_for(const handoff_t *h, u64 base) {
    u64 screen = (h->fb_base && h->fb_pitch && h->fb_height)
               ? (u64)h->fb_pitch * 4ull * (u64)h->fb_height : 0;
    u64 need = HEAP_MIN + ((screen * 2 + 0xFFFFFull) & ~0xFFFFFull);

    u64 share = usable_total(h) / HEAP_SHARE;
    u64 want = share > need ? share : need;
    if (want > HEAP_CEIL) want = HEAP_CEIL;

    /* Never past the end of the run it sits in, and leave a megabyte of it
       so that the frame allocator is not handed a heap with nothing beside
       it. */
    u64 run = usable_run_from(h, base);
    run = run > (1024ull * 1024) ? run - (1024ull * 1024) : 0;
    if (want > run) want = run;

    /* Below the line the kernel maps a page at a time, anything goes. Above
       it, memory is mapped two megabytes at a time and only where a whole
       one of them is usable, so a heap that crosses the line has to end on
       one of those boundaries to be sure of being mapped at all. */
    u64 low = KERNEL_LOW_MB * 1024ull * 1024ull;
    if (base + want > low) {
        u64 end = (base + want) & ~(2ull * 1024 * 1024 - 1);
        want = end > base ? end - base : 0;
    }

    return want;
}

static bool want_selftest = false;

/* QEMU's isa-debug-exit device: writing here ends the VM with (code<<1)|1,
   which is how the test script gets a real exit status out of the kernel. */
static void machine_exit(u32 code) {
    outl(0xF4, code);
    for (;;) hlt();
}

/* Every network controller on the bus, driven or not.
 *
 * Which matters more than it looks. A wireless card is not one kind of
 * thing: most of them run the 802.11 MAC as firmware on a processor of
 * their own, and that firmware is a binary from the vendor, so a kernel
 * that will not carry one cannot make those cards transmit at all. It is
 * not a question of writing more code. Atheros parts are the exception,
 * because their MAC is in hardware and the driver talks to it directly,
 * and they can therefore be driven by a kernel written from scratch.
 *
 * So the useful thing a machine can say about a card it cannot drive is
 * who made it, because that decides whether driving it is possible. */
static const char *pci_vendor_name(u16 vendor) {
    switch (vendor) {
        case 0x8086: return "intel";
        case 0x168C: return "atheros";
        case 0x10EC: return "realtek";
        case 0x14E4: return "broadcom";
        case 0x1814: return "ralink";
        case 0x14C3: return "mediatek";
        case 0x1969: return "qualcomm";
        case 0x1AF4: return "virtio";
        case 0x1022: return "amd";
        default:     return "unknown";
    }
}

static void net_survey(void) {
    pci_dev_t found[6];
    u32 n = pci_list_class(0x02, 0x00, found, 6);            /* ethernet */
    u32 room = n < 6 ? 6 - n : 0;
    u32 other = pci_list_class(0x02, 0x80, found + (n < 6 ? n : 6), room);

    u32 total = n + other;
    if (total > 6) total = 6;

    for (u32 i = 0; i < total; i++) {
        bool wireless = i >= n;
        const char *maker = pci_vendor_name(found[i].vendor);

        /* Atheros is called out by name because it is the one answer that
           means a radio this kernel could drive on its own terms. */
        const char *note = !wireless            ? "ethernet"
                         : found[i].vendor == 0x168C
                           ? "wireless, no firmware needed"
                           : "wireless, needs vendor firmware";

        kprintf("  card    %s %04x:%04x at %d:%d.%d, %s\n",
                maker, found[i].vendor, found[i].device,
                found[i].bus, found[i].slot, found[i].func, note);
        bb_log("card %04x:%04x %s", found[i].vendor, found[i].device, note);
    }
}

static void banner(void) {
    vga_set_color(VGA_LCYAN, VGA_BLACK);
    kprintf("\n  +--------------------------------+\n");
    kprintf("  |  %s %-25s|\n", KERNEL_NAME, KERNEL_VERSION);
    kprintf("  |  x86-64 long mode              |\n");
    kprintf("  +--------------------------------+\n\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);
}

static void selftest_task(void) {
    int failures = selftest_run();
    machine_exit(failures ? 2 : 0);
}

static void init_task(void) {
    layout_init();
    vfs_chdir(layout_home());
    shell_task();
}

/* Where a multiboot loader arrives, once boot.S has put the processor into
   long mode. It has no handoff structure of its own, so one is built out of
   what it left behind and the machine carries on through the same door as
   everything else.

   The handoff lives here rather than on the stack because the stack this is
   called on belongs to boot.S and is not very large. */
static handoff_t multiboot_handoff;

void kmain(handoff_t *h);

void kmain_multiboot(u32 magic, u32 mbi_addr) {
    handoff_t *h = &multiboot_handoff;
    memset(h, 0, sizeof(*h));
    h->magic = HANDOFF_MAGIC;
    strncpy(h->loader, "multiboot", sizeof(h->loader) - 1);

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        /* Nothing has been initialised yet, so say it the only way there is
           and stop. */
        serial_init();
        panic("not booted by a multiboot loader (magic=%x)", magic);
    }

    multiboot_info_t *mbi = (multiboot_info_t *)(u64)mbi_addr;

    if ((mbi->flags & (1 << 2)) && mbi->cmdline)
        strncpy(h->cmdline, (const char *)(u64)mbi->cmdline, sizeof(h->cmdline) - 1);

    /* Multiboot describes memory in its own format; boil it down to the
       three kinds the kernel understands. */
    if (mbi->flags & (1 << 6)) {
        u64 p = mbi->mmap_addr;
        u64 end = (u64)mbi->mmap_addr + mbi->mmap_length;
        while (p < end && h->region_count < HANDOFF_MAX_REGIONS) {
            mb_mmap_entry_t *e = (mb_mmap_entry_t *)p;
            mem_region_t *r = &h->regions[h->region_count++];
            r->base = e->addr;
            r->len = e->len;
            r->type = (e->type == 1) ? MEM_USABLE : MEM_RESERVED;
            r->pad = 0;
            p += e->size + 4;
        }
    }

    if (!h->region_count) {
        /* A loader that described nothing. Assume what the fallback used to:
           everything above the first megabyte, up to what it claimed. */
        mem_region_t *r = &h->regions[h->region_count++];
        r->base = 0x100000;
        r->len = ((u64)mbi->mem_upper + 1024) * 1024;
        r->type = MEM_USABLE;
        r->pad = 0;
    }

    /* No framebuffer: fb_init sets a mode itself through the VBE ports,
       which only exists on a machine that has a BIOS. */
    h->fb_base = 0;

    kmain(h);
}

void kmain(handoff_t *h) {
    serial_init();
    /* First, so that everything below it is recorded. The log needs
       nothing but the serial port and its own static buffer, and both
       exist by now. */
    bb_init();
    bb_mark("serial, vga");
    vga_init();

    if (!h || h->magic != HANDOFF_MAGIC)
        panic("started without a handoff structure");

    for (const char *p = h->cmdline; *p; p++) {
        if (!strncmp(p, "selftest", 8)) want_selftest = true;
        if (!strncmp(p, "console", 7))  shell_console_only();
    }

    banner();

    bb_mark("gdt");
    gdt_init();      kprintf("  gdt     flat segments, tss installed\n");
    bb_mark("idt");
    idt_init();      kprintf("  idt     256 vectors\n");
    bb_mark("pic");
    pic_init();      kprintf("  pic     irqs remapped to 32..47\n");
    bb_mark("memory");
    u64 hbase = heap_base();
    pmm_init(h, hbase);
                     kprintf("  memory  %d MiB usable, via %s\n",
                             (u32)(pmm_free_frames() / 256), h->loader);
    bb_log("memory %d KiB usable, loader %s",
           (u32)(pmm_free_frames() * 4), h->loader);
    /* The heap lives in identity mapped memory, so the frame allocator
       has to be told about it or it will hand the same pages out twice. */
    heap_bytes = heap_size_for(h, hbase);
    pmm_reserve(hbase, heap_bytes);

    /* Before anything runs a program, because a program compiled with the
       vector instructions takes an invalid opcode on its first one until
       this has happened. Cheap, and per processor: the others do the same
       for themselves when they start. */
    fpu_init();
    kprintf("  fpu     sse enabled, %d bytes of state per task\n", FPU_AREA);

    bb_mark("paging");
    paging_init(h);
    /* Before anything is mapped, because it decides what a mapping means. */
    paging_init_pat();
    kprintf("  paging  enabled, %d MiB mapped\n",
            (u32)(paging_mapped_bytes() / (1024 * 1024)));
    bb_mark("heap");
    heap_init(hbase, heap_bytes);
    /* Said as a share rather than as a number, because the number on its own
       does not answer the question anybody actually has, which is whether
       giving the machine more memory did anything. */
    kprintf("  heap    %d MiB at %d MiB, %d MiB left for programs\n",
            (u32)(heap_bytes / (1024 * 1024)),
            (u32)(hbase / (1024 * 1024)),
            (u32)(pmm_free_frames() * 4096ull / (1024 * 1024)));
    bb_log("heap %d MiB at %p", (u32)(heap_bytes / (1024 * 1024)), (void *)hbase);
    /* Needs paging to map the aperture and the heap for the back
       buffer, so this is the earliest it can come up. Anything
       printed before now is only in the serial log. */
    /* A UEFI loader has already chosen a mode and there is no way to ask
       for another once the firmware is gone, so take what it gave. Only a
       machine with a BIOS gets to pick. */
    /* Before anything probes the bus. The disk and network drivers below
       both walk PCI, and on a machine where the mapped path is available it
       is the one that can see all of a device's configuration space. Needs
       paging, which is why it is not earlier. */
    bb_mark("acpi, pcie");
    acpi_use_rsdp(h->rsdp);
    acpi_init();
    if (pci_ecam_init()) {
        kprintf("  pcie    ecam at %p, buses 0..%d\n",
                (void *)pci_ecam_base(), pci_ecam_last_bus());
        bb_log("pcie ecam at %p, buses 0..%d, tables via %s",
               (void *)pci_ecam_base(), pci_ecam_last_bus(),
               acpi()->used_xsdt ? "xsdt" : "rsdt");
    } else {
        kprintf("  pcie    none, using the legacy config ports\n");
        bb_log("pcie no mcfg, legacy config ports only");
    }

    bb_mark("video");
    bool have_screen = h->fb_base
        ? fb_adopt(h->fb_base, h->fb_width, h->fb_height, h->fb_pitch)
        : fb_init(1024, 768);

    if (have_screen) {
        fbcon_init();
        vga_set_color(VGA_LCYAN, VGA_BLACK);
        kprintf("  %s %s\n", KERNEL_NAME, KERNEL_VERSION);
        vga_set_color(VGA_LGREY, VGA_BLACK);
        kprintf("  video   %dx%d 32bpp, %dx%d text\n",
                fb_width(), fb_height(), fbcon_cols(), fbcon_rows());
        bb_log("video %dx%d 32bpp, %s, full flush %d kcycles, %s",
               fb_width(), fb_height(), fb_backend(),
               (u32)(fb_flush_cycles() / 1000),
               paging_wc_ready() ? "write combining" : "uncached");
        bb_log("video %s", fb_double_buffered()
               ? "double buffered" : "NO BACK BUFFER, drawing straight at the screen");
    } else {
        kprintf("  video   no adapter found, vga text mode\n");
        bb_log("video none: no vbe and no vmware adapter, vga text only");
    }
    bb_mark("filesystem");
    fs_init();
    vfs_init();
    fd_init();
    bb_mark("disk");
    if (blk_init()) {
        kprintf("  disk    %s via %s, %d MiB\n", blk_model(), blk_driver(), blk_sectors() / 2048);
        bb_log("disk %s via %s, %d MiB", blk_model(), blk_driver(), blk_sectors() / 2048);
        int n = diskfs_mount();
        /* After the mount, because the log lives inside whichever volume was
           mounted and until then there is no way to know where that is; and
           before anything writes a new record, because this boot is about to
           land on the sectors holding the last one. */
        bb_recover();
        if (n >= 0)      { kprintf("  fs      fat%d mounted, %d entries in the root\n",
                                   fat_type(), n);
                           bb_log("fs fat%d mounted, %d entries in the root", fat_type(), n); }
        else if (n == -2) {
            /* A brand new disk should just work rather than telling
               someone to run a command they have never heard of. */
            if (diskfs_format()) { kprintf("  fs      new disk prepared\n");
                                   bb_log("fs new disk prepared"); }
            else                 { kprintf("  fs      could not prepare the disk\n");
                                   bb_log("fs could not prepare the disk"); }
        }
        else              { kprintf("  fs      disk unreadable, using memory only\n");
                            bb_log("fs disk unreadable, memory only"); }
    } else {
        kprintf("  disk    none, files will not persist\n");
        bb_log("disk none: no controller this kernel can drive");
    }
    builtin_install();
    kprintf("  progs   %d built in\n", builtin_count_programs());

    bb_mark("clock");
    if (rtc_init()) {
        char now[24];
        rtc_format(now, sizeof(now));
        kprintf("  clock   %s\n", now);
        bb_log("clock %s", now);
    } else {
        kprintf("  clock   none, uptime only\n");
        bb_log("clock none: no usable cmos clock");
    }

    bb_mark("timer");
    timer_init(100); kprintf("  timer   100 Hz\n");

    /* After the clock and the timer, both of which it mixes in. What it can
       say here is only what the processor offered: the timing source is
       collected in the timer interrupt and does not exist yet, because
       interrupts are not enabled until the scheduler starts. */
    bb_mark("entropy");
    rng_init();
    if (rng_ready()) {
        kprintf("  random  %s\n", rng_sources());
        bb_log("random %s", rng_sources());
    } else {
        kprintf("  random  no hardware source, collecting timing jitter\n");
        bb_log("random no rdseed and no rdrand, timing jitter only");
    }

    /* Needs the timer: the startup sequence is defined in microseconds and
       there is nothing to measure them with before it. */
    bb_mark("smp");
    smp_init();
    if (smp_cpu_count() > 1)
        kprintf("  cpu     %d processors, %d started\n",
                smp_cpu_count(), smp_started());
    else
        kprintf("  cpu     1 processor\n");
    bb_log("cpu %d found, %d started", smp_cpu_count(), smp_started());
    bb_mark("network");
    if (netdev_init()) {
        net_init();
        const u8 *m = net_mac();
        kprintf("  net     %s %02x:%02x:%02x:%02x:%02x:%02x\n",
                netdev_name(), m[0], m[1], m[2], m[3], m[4], m[5]);
        bb_log("net %s", netdev_name());
    } else {
        kprintf("  net     no card found\n");
        bb_log("net no card this kernel can drive");
    }
    wifi_init();
    net_survey();
    bb_mark("input");
    /* Before either driver, because both arrive through it and neither can
       be trusted to leave it in a state the other one needs. */
    ps2_init();
    keyboard_init();
    if (fb_active() && mouse_init())
        kprintf("  mouse   ps/2%s, pointer at %d,%d\n",
                mouse_has_wheel() ? " with a wheel" : "",
                mouse_x(), mouse_y());
    else
        bb_log("mouse none: no ps/2 pointer answered");
    /* After the keyboard and the mouse, because what it finds is handed
       to them. A machine with no xHCI controller passes straight through
       this, which is every emulated machine not given one on purpose, and
       almost no real one. */
    usb_init();
    if (usb_present()) kprintf("  usb     %s\n", usb_describe());

    bb_mark("sound");
    if (sound_init()) {
        kprintf("  sound   %s, %d Hz\n", sound_describe(), sound_rate());
        bb_log("sound %s, %d Hz", sound_describe(), sound_rate());
    } else {
        /* What is on the bus and not being driven, rather than nothing at
           all. A machine with a sound controller this kernel does not know
           and a machine with no sound hardware at all used to look
           identical from here -- both said nothing -- and they need
           completely different answers from whoever is reading. */
        pci_dev_t audio[4];
        u32 n = pci_list_class(0x04, 0x01, audio, 4);          /* multimedia */
        if (n < 4) n += pci_list_class(0x04, 0x03, audio + n, 4 - n);  /* hda */

        for (u32 i = 0; i < n; i++)
            kprintf("  sound   %04x:%04x at %d:%d.%d, no driver\n",
                    audio[i].vendor, audio[i].device,
                    audio[i].bus, audio[i].slot, audio[i].func);
        if (!n) kprintf("  sound   no controller found\n");
        bb_log("sound none, %d undriven controller(s) on the bus", n);
    }

    serial_enable_irq();
    kprintf("  input   ps/2 keyboard + serial (irq driven)\n");

    /* What the pointer turned out to be. Said out loud because a trackpad
       that was found and is doing nothing and a trackpad that was never
       found look identical from the far side of the screen, and on a laptop
       this is the first thing worth knowing. */
    if (syn_present())
        kprintf("  pointer synaptics %d.%d, caps %06x%s\n",
                syn_major(), syn_minor(), syn_capabilities(),
                mouse_has_wheel() ? ", two finger scroll" : "");
    else if (mouse_present())
        kprintf("  pointer ps/2 mouse%s\n",
                mouse_has_wheel() ? " with a wheel" : "");
    else
        kprintf("  pointer none found\n");

    /* And what a pointer might be hiding behind, on a machine where it is
       not on the 8042. A laptop trackpad that is not a ps/2 device is on an
       i2c controller, and those turn up under one of two classes depending
       on how the vendor felt that year. There is no driver for either yet,
       so this only says what is there, which is the thing that has to be
       known before there is any point writing one. */
    {
        pci_dev_t serial[4];
        u32 n = pci_list_class(0x0C, 0x80, serial, 4);          /* other bus */
        u32 dsp = pci_list_class(0x11, 0x80, serial + (n < 4 ? n : 4),
                                 n < 4 ? 4 - n : 0);            /* or here */
        u32 total = n + dsp;
        for (u32 i = 0; i < total && i < 4; i++)
            kprintf("  i2c     %04x:%04x at %d:%d.%d, no driver\n",
                    serial[i].vendor, serial[i].device,
                    serial[i].bus, serial[i].slot, serial[i].func);
    }

    /* Interrupt routing, after every driver has registered its handler and
       before interrupts are ever enabled, which does not happen until
       sched_start. Doing it here rather than inside each driver means one
       place decides, and the decision can see which lines are actually
       claimed: routing one whose vector has no handler would deliver an
       interrupt nothing acknowledges. */
    bb_mark("interrupt routing");
    if (ioapic_init()) {
        if (acpi()->has_8259) pic_disable();
        u32 routed = 0;
        for (u8 irq = 0; irq < 16; irq++) {
            if (!idt_has_handler((u8)(32 + irq))) continue;
            if (ioapic_route_irq(irq, (u8)(32 + irq))) routed++;
        }
        kprintf("  irqs    ioapic, %d input(s), %d routed\n",
                ioapic_inputs(), routed);
        bb_log("irqs ioapic, %d inputs, %d routed, 8259 %s",
               ioapic_inputs(), routed,
               acpi()->has_8259 ? "masked" : "absent");
    } else {
        kprintf("  irqs    8259, no ioapic described\n");
        bb_log("irqs 8259 only, no ioapic in the tables");
    }

    bb_mark("syscalls, window server, scheduler");
    syscall_init();
    clip_init();
    winsrv_init();
    sched_init();
    /* After the scheduler exists, because it is a task, and the task is how
       anything plugged in later gets noticed at all. */
    usb_start_service();
    /* Same reason, and the same place: frames arrive whether or not somebody
       is blocked waiting for one, and since the card's interrupt stopped
       running the stack there has to be something that does. */
    net_start_service();
    if (want_selftest) {
        task_create("selftest", selftest_task);
    } else {
        task_create("init", init_task);

        /* And ask for an address, rather than waiting to be told to.
         *
         * This was a button on the panel and nothing else, which meant a
         * machine that had just booted had a working card, a working stack
         * and no address, so everything that used the network failed while
         * saying something else about itself. Nobody reads "no address" as
         * "press the button in the corner first".
         *
         * Here rather than beside the card, because it is a task and there
         * was no scheduler to put one on yet at that point: written there
         * first, it silently created nothing and the machine still came up
         * with no address. Not during the self test, because that runs its
         * own exchange on the same socket, and two at once is a race. */
        net_dhcp_start();
    }

    kprintf("  sched   %d task(s)\n", task_count());

    /* The last thing written before control leaves this function. A log
       that ends here booted; one that ends at an earlier mark names the
       thing that did not finish. */
    bb_mark("handing over to the scheduler");
    bb_flush();
    sched_start();
}
