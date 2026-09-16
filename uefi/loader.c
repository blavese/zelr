/* The UEFI bootloader.
 *
 * A machine with UEFI firmware never enters real mode and never runs a boot
 * sector, so bootloader/cdboot.S cannot start it. This can. It is an EFI
 * application: a PE executable the firmware loads, calls, and gives a table
 * of services to.
 *
 * That makes it a much easier job than the BIOS loader in one way and a
 * harder one in another. Easier because the machine is already in 64-bit
 * long mode with paging on and memory identity mapped, so there is no A20
 * gate, no unreal mode and no descriptor tables to build. Harder because
 * there is no BIOS: no INT 13h to read a disc, no INT 10h to set a video
 * mode, no INT 15h to ask about memory. Every one of those is a firmware
 * protocol that has to be located and called.
 *
 * The last thing it does is ExitBootServices, after which none of the
 * firmware's services exist any more and the machine belongs to the kernel.
 */
#include "efi.h"
#include "../include/handoff.h"

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

/* --- output ------------------------------------------------------------- */

static void print(const CHAR16 *s) {
    ST->con_out->output_string(ST->con_out, (CHAR16 *)s);
}

static void print_hex(u64 v) {
    CHAR16 buf[19];
    const CHAR16 *digits = u"0123456789abcdef";
    buf[0] = u'0';
    buf[1] = u'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = 0;
    print(buf);
}

static void print_dec(u64 v) {
    CHAR16 buf[21];
    int n = 0;
    if (!v) { print(u"0"); return; }
    while (v) { buf[n++] = (CHAR16)(u'0' + v % 10); v /= 10; }
    CHAR16 out[21];
    for (int i = 0; i < n; i++) out[i] = buf[n - 1 - i];
    out[n] = 0;
    print(out);
}

/* Anything that goes wrong here is fatal: there is nowhere to fall back to
   and no operating system yet to report it. */
static void die(const CHAR16 *why, EFI_STATUS status) {
    print(u"\r\nzelr: ");
    print(why);
    print(u" (");
    print_hex(status);
    print(u")\r\n");
    for (;;) __asm__ volatile ("hlt");
}

/* --- small helpers, since there is no libc ------------------------------ */

static void *mem_set(void *p, int v, u64 n) {
    u8 *b = (u8 *)p;
    for (u64 i = 0; i < n; i++) b[i] = (u8)v;
    return p;
}

static void str_copy(char *d, const char *s, int cap) {
    int i = 0;
    for (; i < cap - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

/* --- the screen --------------------------------------------------------- */

/* Picks the largest mode that is a plain 32-bit pixel format. The firmware
   often starts in something small, and a mode with a bit mask or one that
   only supports blitting has no linear framebuffer to draw into. */
static void setup_graphics(handoff_t *h) {
    EFI_GUID gop_guid = EFI_GOP_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;

    EFI_STATUS s = BS->locate_protocol(&gop_guid, 0, (void **)&gop);
    if (EFI_ERROR(s) || !gop) {
        print(u"  no graphics protocol; running without a screen\r\n");
        return;
    }

    u32 best = gop->mode->mode;
    u32 best_pixels = 0;
    u32 smallest = gop->mode->mode;
    u32 smallest_pixels = 0xFFFFFFFFu;

    for (u32 i = 0; i < gop->mode->max_mode; i++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN size;
        if (EFI_ERROR(gop->query_mode(gop, i, &size, &info))) continue;
        if (info->pixel_format != PixelRedGreenBlueReserved8BitPerColor &&
            info->pixel_format != PixelBlueGreenRedReserved8BitPerColor) continue;

        u32 pixels = info->horizontal_resolution * info->vertical_resolution;
        if (pixels < smallest_pixels) { smallest_pixels = pixels; smallest = i; }

        /* Bigger is better, up to what the kernel can afford a back buffer
           for. The heap grows to fit the mode chosen here, so this is the
           ceiling on that rather than a property of any particular panel. */
        if (info->horizontal_resolution > 3840) continue;
        if (info->vertical_resolution > 2160) continue;

        if (pixels > best_pixels) { best_pixels = pixels; best = i; }
    }

    /* Nothing this machine offers is small enough. Keeping the mode the
       firmware happens to be in is the wrong answer, because that is usually
       the panel's own resolution and therefore the largest of the lot: the
       one that was just rejected. Take the smallest instead. */
    if (!best_pixels && smallest_pixels != 0xFFFFFFFFu) {
        best = smallest;
        best_pixels = smallest_pixels;
    }

    if (best_pixels && best != gop->mode->mode) {
        EFI_STATUS r = gop->set_mode(gop, best);
        if (EFI_ERROR(r)) print(u"  could not change mode, keeping the current one\r\n");
    }

    h->fb_base   = gop->mode->frame_buffer_base;
    h->fb_width  = gop->mode->info->horizontal_resolution;
    h->fb_height = gop->mode->info->vertical_resolution;
    h->fb_pitch  = gop->mode->info->pixels_per_scan_line;
    h->fb_bpp    = 32;

    print(u"  screen  ");
    print_dec(h->fb_width);
    print(u"x");
    print_dec(h->fb_height);
    print(u" at ");
    print_hex(h->fb_base);
    print(u"\r\n");
}

/* --- the ACPI tables ---------------------------------------------------- */

/* On a UEFI machine the pointer to the ACPI tables is in the configuration
   table, not lying in low memory where the kernel's own search looks. Handing
   it over is what lets the kernel find the other processors. */
static void find_acpi(handoff_t *h) {
    EFI_GUID acpi2 = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi1 = EFI_ACPI_10_TABLE_GUID;

    for (UINTN i = 0; i < ST->number_of_table_entries; i++) {
        EFI_CONFIGURATION_TABLE *e = &ST->configuration_table[i];
        if (guid_eq(&e->vendor_guid, &acpi2)) {
            h->rsdp = (u64)e->vendor_table;
            return;                        /* 2.0 wins if both are present */
        }
        if (guid_eq(&e->vendor_guid, &acpi1) && !h->rsdp)
            h->rsdp = (u64)e->vendor_table;
    }
}

/* --- reading the kernel ------------------------------------------------- */

/* Opens a file in the same directory this loader was loaded from, which is
   how it finds the kernel without being told where it is. */
static EFI_FILE_PROTOCOL *open_boot_file(EFI_HANDLE image, const CHAR16 *name) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_GUID;

    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    if (EFI_ERROR(BS->handle_protocol(image, &li_guid, (void **)&li)) || !li)
        die(u"cannot ask which image this is", 0);

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    if (EFI_ERROR(BS->handle_protocol(li->device_handle, &fs_guid, (void **)&fs)) || !fs)
        die(u"the volume this booted from has no filesystem", 0);

    EFI_FILE_PROTOCOL *root = 0;
    if (EFI_ERROR(fs->open_volume(fs, &root)) || !root)
        die(u"cannot open the boot volume", 0);

    EFI_FILE_PROTOCOL *file = 0;
    EFI_STATUS s = root->open(root, &file, (CHAR16 *)name, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s) || !file) die(u"cannot open the kernel", s);

    root->close(root);
    return file;
}

/* The kernel is a flat image that belongs at a fixed physical address. Asking
   the firmware for that exact address rather than any free pages is the point:
   the kernel is linked to run there. */
static u64 load_kernel(EFI_HANDLE image, u64 load_at, u64 *size_out) {
    EFI_FILE_PROTOCOL *file = open_boot_file(image, u"zelr.bin");

    /* Seek to the end to learn the size, since asking for file information
       means another structure and another buffer. */
    if (EFI_ERROR(file->set_position(file, 0xFFFFFFFFFFFFFFFFull)))
        die(u"cannot measure the kernel", 0);
    u64 size = 0;
    if (EFI_ERROR(file->get_position(file, &size))) die(u"cannot measure the kernel", 0);
    if (EFI_ERROR(file->set_position(file, 0))) die(u"cannot rewind the kernel", 0);
    if (!size) die(u"the kernel is empty", 0);

    u64 pages = (size + 0xFFF) / 0x1000;
    EFI_PHYSICAL_ADDRESS at = load_at;
    EFI_STATUS s = BS->allocate_pages(AllocateAddress, EfiLoaderData, pages, &at);
    if (EFI_ERROR(s)) {
        /* Some firmware has already put something at 1 MiB. Nothing can be
           done about that here; the kernel is linked to run at that address. */
        die(u"cannot reserve the address the kernel is linked at", s);
    }

    UINTN want = size;
    if (EFI_ERROR(file->read(file, &want, (void *)at))) die(u"cannot read the kernel", 0);
    if (want != size) die(u"the kernel read short", 0);
    file->close(file);

    print(u"  kernel  ");
    print_dec(size);
    print(u" bytes at ");
    print_hex(at);
    print(u"\r\n");

    *size_out = size;
    return at;
}

/* --- memory ------------------------------------------------------------- */

static int usable_after_exit(u32 type) {
    /* Once boot services are gone, the memory they were using is ours. The
       firmware's own runtime code and its ACPI tables are not. */
    return type == EfiConventionalMemory ||
           type == EfiBootServicesCode ||
           type == EfiBootServicesData ||
           type == EfiLoaderCode;
}

/* Reads the firmware's memory map, boils it down to the three kinds the
   kernel cares about, and leaves boot services behind.
 *
 * The awkward part is that asking for the map allocates, and allocating
 * changes the map, which invalidates the key ExitBootServices wants. The
 * loop is the documented way through: get the map, try to exit, and if the
 * firmware says the key is stale, get it again. */
static void take_the_machine(EFI_HANDLE image, handoff_t *h) {
    static u8 map_buffer[32 * 1024];

    for (int attempt = 0; attempt < 8; attempt++) {
        UINTN size = sizeof(map_buffer);
        UINTN key = 0, desc_size = 0;
        u32 desc_version = 0;

        EFI_STATUS s = BS->get_memory_map((UINTN *)&size,
                                          (EFI_MEMORY_DESCRIPTOR *)map_buffer,
                                          &key, &desc_size, &desc_version);
        if (EFI_ERROR(s)) die(u"cannot read the memory map", s);

        /* Boil it down before exiting, because afterwards there is no way to
           report a problem and no services to call. */
        h->region_count = 0;
        for (UINTN off = 0; off + desc_size <= size; off += desc_size) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(map_buffer + off);
            if (!d->pages) continue;

            u32 kind;
            if (usable_after_exit(d->type)) kind = MEM_USABLE;
            else if (d->type == EfiACPIReclaimMemory) kind = MEM_ACPI;
            else kind = MEM_RESERVED;

            /* Merge with the previous entry when they touch and match, which
               keeps a fragmented firmware map inside our fixed array. */
            if (h->region_count) {
                mem_region_t *last = &h->regions[h->region_count - 1];
                if (last->type == kind && last->base + last->len == d->physical_start) {
                    last->len += d->pages * 0x1000;
                    continue;
                }
            }
            if (h->region_count >= HANDOFF_MAX_REGIONS) break;

            mem_region_t *r = &h->regions[h->region_count++];
            r->base = d->physical_start;
            r->len = d->pages * 0x1000;
            r->type = kind;
            r->pad = 0;
        }

        s = BS->exit_boot_services(image, key);
        if (!EFI_ERROR(s)) return;
        /* The map changed underneath us. Ask again. */
    }

    die(u"the firmware would not hand over the machine", 0);
}

/* --- entry -------------------------------------------------------------- */

/* Where the kernel is linked. The BIOS loader puts it here too, so the same
   image works either way. */
#define KERNEL_PHYS 0x100000ull

/* The handoff lives at a fixed low address rather than in firmware memory,
   because everything the firmware allocated stops being described the moment
   boot services end. */
#define HANDOFF_PHYS 0x70000ull

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table) {
    ST = system_table;
    BS = ST->boot_services;

    ST->con_out->clear_screen(ST->con_out);
    print(u"zelr uefi loader\r\n");

    /* The firmware will reset the machine if it thinks we have hung. */
    BS->set_watchdog_timer(0, 0, 0, 0);

    handoff_t *h = (handoff_t *)HANDOFF_PHYS;
    EFI_PHYSICAL_ADDRESS at = HANDOFF_PHYS;
    EFI_STATUS s = BS->allocate_pages(AllocateAddress, EfiLoaderData,
                                      (sizeof(handoff_t) + 0xFFF) / 0x1000, &at);
    if (EFI_ERROR(s)) die(u"cannot reserve the handoff page", s);

    mem_set(h, 0, sizeof(*h));
    h->magic = HANDOFF_MAGIC;
    str_copy(h->loader, "uefi", sizeof(h->loader));

    find_acpi(h);
    if (h->rsdp) {
        print(u"  acpi    tables at ");
        print_hex(h->rsdp);
        print(u"\r\n");
    } else {
        print(u"  acpi    the firmware did not say where\r\n");
    }

    setup_graphics(h);

    u64 size = 0;
    u64 base = load_kernel(image, KERNEL_PHYS, &size);
    h->kernel_base = base;
    h->kernel_size = size;

    print(u"  leaving the firmware\r\n");
    take_the_machine(image, h);

    /* Nothing above this line can be called again. The machine is already in
       long mode with memory identity mapped, so the kernel simply continues
       where the firmware left off. */
    /* The first bytes of the image are a header, not code: a multiboot
       header sits near the front and cannot be jumped to. The entry point
       is stated in the first sixteen bytes. */
    const char *sig = (const char *)base;
    for (int i = 0; i < 8; i++)
        if (sig[i] != "ZELRKRN"[i]) {
            /* Nothing can be printed: boot services are gone. */
            for (;;) __asm__ volatile ("hlt");
        }
    u64 entry_point = *(u64 *)(base + 8);

    /* Everything in this file is compiled for the firmware's calling
       convention. The kernel is not: it is an ordinary System V binary and
       expects its argument in a different register, so the type says so. */
    void (__attribute__((sysv_abi)) *enter)(handoff_t *) =
        (void (__attribute__((sysv_abi)) *)(handoff_t *))entry_point;
    enter(h);

    for (;;) __asm__ volatile ("hlt");
}
