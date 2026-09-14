#include "types.h"
#include "vga.h"
#include "serial.h"
#include "printf.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"
#include "multiboot.h"
#include "handoff.h"
#include "pmm.h"
#include "paging.h"
#include "heap.h"
#include "sched.h"
#include "fs.h"
#include "blockdev.h"
#include "diskfs.h"
#include "pci.h"
#include "netdev.h"
#include "net.h"
#include "fb.h"
#include "fbcon.h"
#include "mouse.h"
#include "syscall.h"
#include "shell.h"
#include "winsrv.h"
#include "smp.h"
#include "acpi.h"
#include "vfs.h"
#include "builtin.h"
#include "layout.h"
#include "selftest.h"
#include "string.h"
#include "io.h"
#include "blackbox.h"

#define HEAP_BASE (8u * 1024 * 1024)
#define HEAP_SIZE (16u * 1024 * 1024)

static bool want_selftest = false;

/* QEMU's isa-debug-exit device: writing here ends the VM with (code<<1)|1,
   which is how the test script gets a real exit status out of the kernel. */
static void machine_exit(u32 code) {
    outl(0xF4, code);
    for (;;) hlt();
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

    for (const char *p = h->cmdline; *p; p++)
        if (!strncmp(p, "selftest", 8)) { want_selftest = true; break; }

    banner();

    bb_mark("gdt");
    gdt_init();      kprintf("  gdt     flat segments, tss installed\n");
    bb_mark("idt");
    idt_init();      kprintf("  idt     256 vectors\n");
    bb_mark("pic");
    pic_init();      kprintf("  pic     irqs remapped to 32..47\n");
    bb_mark("memory");
    pmm_init(h);     kprintf("  memory  %d KiB usable, via %s\n",
                             (u32)(pmm_free_frames() * 4), h->loader);
    bb_log("memory %d KiB usable, loader %s",
           (u32)(pmm_free_frames() * 4), h->loader);
    /* The heap lives in identity mapped memory, so the frame allocator
       has to be told about it or it will hand the same pages out twice. */
    pmm_reserve(HEAP_BASE, HEAP_SIZE);

    bb_mark("paging");
    paging_init();   kprintf("  paging  enabled\n");
    bb_mark("heap");
    heap_init(HEAP_BASE, HEAP_SIZE);
    kprintf("  heap    %d KiB\n", HEAP_SIZE / 1024);
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
        bb_log("video %dx%d 32bpp, %s", fb_width(), fb_height(),
               h->fb_base ? "adopted from the loader" : "set through vbe");
    } else {
        kprintf("  video   no vbe, vga text mode\n");
        bb_log("video none, vga text mode only");
    }
    bb_mark("filesystem");
    fs_init();
    vfs_init();
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
        if (n >= 0)      { kprintf("  fs      fat16 mounted, %d entries in the root\n", n);
                           bb_log("fs fat16 mounted, %d entries in the root", n); }
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

    bb_mark("timer");
    timer_init(100); kprintf("  timer   100 Hz\n");

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
    bb_mark("input");
    keyboard_init();
    if (fb_active() && mouse_init())
        kprintf("  mouse   ps/2, pointer at %d,%d\n", mouse_x(), mouse_y());
    else
        bb_log("mouse none: no ps/2 pointer answered");
    serial_enable_irq();
    kprintf("  input   ps/2 keyboard + serial (irq driven)\n");

    bb_mark("syscalls, window server, scheduler");
    syscall_init();
    winsrv_init();
    sched_init();
    if (want_selftest) task_create("selftest", selftest_task);
    else               task_create("init", init_task);

    kprintf("  sched   %d task(s)\n", task_count());

    /* The last thing written before control leaves this function. A log
       that ends here booted; one that ends at an earlier mark names the
       thing that did not finish. */
    bb_mark("handing over to the scheduler");
    bb_flush();
    sched_start();
}
