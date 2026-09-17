/* Boot-time self test. `run.sh -T` boots with this enabled, so the whole
   kernel can be checked from a script without a human watching a screen. */
#include "selftest.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "pmm.h"
#include "paging.h"
#include "fs.h"
#include "vfs.h"
#include "layout.h"
#include "sysfs.h"
#include "timer.h"
#include "synaptics.h"
#include "sched.h"
#include "wait.h"
#include "syscall.h"
#include "idt.h"
#include "blockdev.h"
#include "diskfs.h"
#include "fat.h"
#include "elf.h"
#include "user.h"
#include "paging.h"
#include "sched.h"
#include "syscall.h"
#include "net.h"
#include "netdev.h"
#include "fb.h"
#include "fbcon.h"
#include "mouse.h"
#include "gfx.h"
#include "wm.h"
#include "font.h"
#include "winsrv.h"
#include "theme.h"
#include "pins.h"
#include "smp.h"
#include "builtin.h"
#include "blackbox.h"
#include "pci.h"
#include "acpi.h"
#include "io.h"
#include "ioapic.h"
#include "lapic.h"
#include "pic.h"
#include "clipboard.h"
#include "rtc.h"

static int passed, failed;

static void ok(const char *what, bool cond) {
    if (cond) { passed++; kprintf("  PASS  %s\n", what); }
    else      { failed++; kprintf("  FAIL  %s\n", what); }
}

static void test_string(void) {
    char b[32];
    ok("strlen", strlen("hello") == 5);
    ok("strcmp equal", strcmp("abc", "abc") == 0);
    ok("strcmp order", strcmp("abc", "abd") < 0);
    strcpy(b, "copy me");
    ok("strcpy", strcmp(b, "copy me") == 0);
    memset(b, 'x', 4); b[4] = 0;
    ok("memset", strcmp(b, "xxxx") == 0);
    char s[8] = "source";
    char d[8];
    memcpy(d, s, 7);
    ok("memcpy", strcmp(d, "source") == 0);
    ok("memcmp", memcmp("ab", "ab", 2) == 0 && memcmp("ab", "ac", 2) != 0);
    char ov[16] = "abcdef";
    memmove(ov + 1, ov, 5); ov[6] = 0;
    ok("memmove overlap", strcmp(ov, "aabcde") == 0);
}

static void test_heap(void) {
    u32 before = heap_used();
    void *a = kmalloc(64), *b = kmalloc(128), *c = kmalloc(32);
    ok("kmalloc returns distinct blocks", a && b && c && a != b && b != c);
    memset(a, 0xAA, 64);
    ok("heap memory is writable", *(u8 *)a == 0xAA);
    kfree(a); kfree(b); kfree(c);
    ok("heap coalesces back to baseline", heap_used() == before);

    void *big = kmalloc(1024 * 512);
    ok("large allocation", big != 0);
    kfree(big);

    u32 *z = (u32 *)kcalloc(64);
    bool zeroed = z != 0;
    for (int i = 0; i < 16 && zeroed; i++) if (z[i]) zeroed = false;
    ok("kcalloc zeroes", zeroed);
    kfree(z);
}

/* The identity map, now that it covers whatever the machine has rather than
   a fixed 64 MiB. */
static void test_identity_map(void) {
    u64 low = KERNEL_LOW_MB * 1024ull * 1024ull;
    u64 mapped = paging_mapped_bytes();

    ok("the identity map covers at least the low region", mapped >= low);

    /* One to one, which is the whole reason it exists: a pointer the kernel
       holds is a physical address a device can be given. */
    bool one_to_one = true;
    for (u64 a = 0x100000; a < low; a += 4ull * 1024 * 1024)
        if (virt_to_phys(a) != a) one_to_one = false;
    ok("and every address in it translates to itself", one_to_one);

    /* Memory above the low region is mapped in 2 MiB pages, and the point of
       mapping it is being able to use it. Written through a pointer and read
       back, because a page table entry that is present and wrong looks
       exactly like one that is right until something touches it. */
    if (mapped > low + 4ull * 1024 * 1024) {
        u64 a = low + 2ull * 1024 * 1024;
        ok("memory above it is mapped too", virt_to_phys(a) == a);

        volatile u64 *p = (volatile u64 *)a;
        u64 keep = *p;
        *p = 0x5A5AC3C3A5A53C3Cull;
        ok("and can be written and read back", *p == 0x5A5AC3C3A5A53C3Cull);
        *p = keep;

        /* A single page inside one of those large ones can still be mapped
           on its own, which means the large one came apart correctly and
           everything around it survived. */
        u64 probe = a + 8ull * 1024 * 1024;
        if (probe + PAGE_SIZE < mapped) {
            u64 frame = pmm_alloc_frame();
            if (frame) {
                u64 neighbour = probe + PAGE_SIZE;
                bool remapped = map_page(probe, frame, PTE_PRESENT | PTE_RW);
                ok("a 2 MiB page can be split to remap one page inside it",
                   remapped && virt_to_phys(probe) == frame);
                ok("and the pages beside it are undisturbed",
                   virt_to_phys(neighbour) == neighbour);
                map_page(probe, probe, PTE_PRESENT | PTE_RW);
                pmm_free_frame(frame);
            }
        }
    }

    /* The gap between the memory below 4 GiB and the memory above it is
       where devices live, and it must not be mapped as ordinary memory: a
       driver that found it already mapped would use it cached, and a cached
       write to a device register does not reach the device. Only checked on
       a machine big enough to have the gap at all. */
    if (mapped > 4ull * 1024 * 1024 * 1024) {
        ok("the hole below 4 GiB is left for devices to claim",
           virt_to_phys(0xC0000000ull) == 0);
    }
}

static void test_pmm(void) {
    u32 free_before = pmm_free_frames();
    u32 f1 = pmm_alloc_frame(), f2 = pmm_alloc_frame();
    ok("frames allocate", f1 && f2 && f1 != f2);
    ok("frames are page aligned", (f1 & 0xFFF) == 0 && (f2 & 0xFFF) == 0);
    ok("free count dropped", pmm_free_frames() == free_before - 2);
    pmm_free_frame(f1); pmm_free_frame(f2);
    ok("free count restored", pmm_free_frames() == free_before);
}

static void test_paging(void) {
    u64 phys = pmm_alloc_frame();
    const u64 v = 0x00D00000;
    ok("map_page", map_page(v, phys, PTE_PRESENT | PTE_RW));
    volatile u32 *p = (volatile u32 *)v;
    *p = 0xDEADBEEF;
    ok("write through mapping", *p == 0xDEADBEEF);
    ok("virt_to_phys agrees", (virt_to_phys(v) & ~0xFFFu) == phys);
    unmap_page(v);
    ok("unmap clears translation", virt_to_phys(v) == 0);
    pmm_free_frame(phys);
}

/* The rule a system call leans on when it validates a pointer: being mapped
   in the caller's address space is not the same as being reachable from ring
   3. Every space inherits the kernel's high mappings, so a check that only
   asked whether a page was present would accept the framebuffer and the
   controllers' register windows. */
static void test_user_access(void) {
    u32 dir = paging_new_directory();
    if (!dir) { ok("scratch address space", false); return; }

    u32 kp = pmm_alloc_frame(), up = pmm_alloc_frame(), kp2 = pmm_alloc_frame();
    if (!kp || !up || !kp2) { ok("scratch frames", false); return; }

    /* A kernel mapping above where user space begins, which is exactly what
       the framebuffer aperture is. */
    map_page_in(dir, 0x38000000, kp, PTE_PRESENT | PTE_RW);
    ok("a kernel page above user space is mapped",
       virt_to_phys_in(dir, 0x38000000) != 0);
    ok("but ring 3 cannot reach it", !virt_is_user_in(dir, 0x38000000));

    /* Two pages sharing one table: mapping the user one widens the directory
       entry, and its kernel neighbour must stay out of reach anyway. */
    map_page_in(dir, 0x39000000, up,  PTE_PRESENT | PTE_RW | PTE_USER);
    map_page_in(dir, 0x39001000, kp2, PTE_PRESENT | PTE_RW);
    ok("a user page is reachable", virt_is_user_in(dir, 0x39000000));
    ok("its kernel neighbour in the same table is not",
       !virt_is_user_in(dir, 0x39001000));
    ok("an unmapped address is not", !virt_is_user_in(dir, 0x3A000000));

    paging_free_directory(dir);
    pmm_free_frame(kp);
    pmm_free_frame(kp2);
}

static void test_fs(void) {
    char buf[64];
    u32 size = 0;

    vfs_delete("/t.txt");
    ok("write file", vfs_write("/t.txt", "hello", 5));
    ok("read it back", vfs_read("/t.txt", buf, sizeof(buf)) == 5 && memcmp(buf, "hello", 5) == 0);
    ok("stat reports the size", vfs_stat("/t.txt", &size, 0) && size == 5);
    ok("append", vfs_append("/t.txt", "!!", 2));
    ok("append grew the file",
       vfs_read("/t.txt", buf, sizeof(buf)) == 7 && memcmp(buf, "hello!!", 7) == 0);

    u32 n = vfs_count("/");
    ok("create second file", vfs_write("/u.txt", "x", 1) && vfs_count("/") == n + 1);
    ok("delete", vfs_delete("/t.txt") && !vfs_stat("/t.txt", 0, 0));
    vfs_delete("/u.txt");
}

static void test_paths(void) {
    char out[VFS_PATH_MAX];

    ok("an absolute path is left alone",
       vfs_resolve("/a/b", out, sizeof(out)) && strcmp(out, "/a/b") == 0);
    ok("repeated slashes collapse",
       vfs_resolve("//a///b", out, sizeof(out)) && strcmp(out, "/a/b") == 0);
    ok("a dot goes nowhere",
       vfs_resolve("/a/./b", out, sizeof(out)) && strcmp(out, "/a/b") == 0);
    ok("dot dot climbs one",
       vfs_resolve("/a/b/..", out, sizeof(out)) && strcmp(out, "/a") == 0);
    ok("dot dot in the middle",
       vfs_resolve("/a/../b", out, sizeof(out)) && strcmp(out, "/b") == 0);
    ok("dot dot stops at the root",
       vfs_resolve("/../../..", out, sizeof(out)) && strcmp(out, "/") == 0);
    ok("the root resolves to itself",
       vfs_resolve("/", out, sizeof(out)) && strcmp(out, "/") == 0);

    /* Relative paths are joined to wherever the caller is. */
    ok("a directory can be entered", vfs_mkdir("/sub") && vfs_chdir("/sub"));
    ok("the working directory follows", strcmp(vfs_cwd(), "/sub") == 0);
    ok("a relative name resolves inside it",
       vfs_resolve("f.txt", out, sizeof(out)) && strcmp(out, "/sub/f.txt") == 0);
    ok("and dot dot leaves it",
       vfs_resolve("../g.txt", out, sizeof(out)) && strcmp(out, "/g.txt") == 0);
    vfs_chdir("/");
    vfs_rmdir("/sub");
}

static void test_directories(void) {
    vfs_delete("/d/inner.txt");
    vfs_rmdir("/d/deep");
    vfs_rmdir("/d");

    ok("mkdir creates one", vfs_mkdir("/d"));
    bool is_dir = false;
    ok("it stats as a directory", vfs_stat("/d", 0, &is_dir) && is_dir);
    ok("making it twice fails", !vfs_mkdir("/d"));

    ok("a file can be written inside it", vfs_write("/d/inner.txt", "nested", 6));
    char buf[16];
    ok("and read back out", vfs_read("/d/inner.txt", buf, sizeof(buf)) == 6 &&
                            memcmp(buf, "nested", 6) == 0);
    ok("it lists inside, not outside", vfs_count("/d") == 1);

    ok("directories nest", vfs_mkdir("/d/deep") && vfs_write("/d/deep/x", "y", 1));
    ok("the nested file reads back", vfs_read("/d/deep/x", buf, sizeof(buf)) == 1);
    ok("a path through two levels resolves",
       vfs_stat("/d/deep/../deep/x", 0, 0));

    ok("rmdir refuses a directory with things in it", !vfs_rmdir("/d"));
    ok("emptying it lets rmdir work",
       vfs_delete("/d/deep/x") && vfs_rmdir("/d/deep") &&
       vfs_delete("/d/inner.txt") && vfs_rmdir("/d"));
    ok("and it is gone", !vfs_stat("/d", 0, 0));
}

static void test_timer(void) {
    u64 a = timer_ticks();
    sleep_ms(60);
    u64 b = timer_ticks();
    ok("timer advances", b > a);
    ok("timer roughly matches the requested delay", (b - a) >= 4 && (b - a) <= 20);
}

static volatile int bp_hits = 0;
static void on_breakpoint(registers_t *r) { (void)r; bp_hits++; }

static void test_interrupts(void) {
    /* If the IDT were broken this would have triple faulted long ago, but
       take an explicit software interrupt to be sure the whole path still
       works: stub, dispatcher, handler, and the return. */
    register_interrupt_handler(3, on_breakpoint);
    int before = bp_hits;
    __asm__ volatile ("int $3");
    ok("software interrupt reached its handler", bp_hits == before + 1);
    __asm__ volatile ("int $3");
    __asm__ volatile ("int $3");
    ok("handler is re-entrant", bp_hits == before + 3);
}


static void test_disk(void) {
    if (!blk_present()) { kprintf("  SKIP  no disk attached\n"); return; }
    ok("disk reports a size", blk_sectors() > 0);

    /* Use a sector well past the filesystem so nothing real is disturbed,
       and put back whatever was there. */
    u32 lba = blk_sectors() - 4;
    u8 original[SECTOR_SIZE], probe[SECTOR_SIZE], back[SECTOR_SIZE];
    ok("read a sector", blk_read(lba, 1, original));

    for (u32 i = 0; i < SECTOR_SIZE; i++) probe[i] = (u8)(i * 7 + 3);
    ok("write a sector", blk_write(lba, 1, probe));
    ok("read it back", blk_read(lba, 1, back));
    ok("what came back is what went out", memcmp(probe, back, SECTOR_SIZE) == 0);

    blk_write(lba, 1, original);
    ok("original contents restored", blk_read(lba, 1, back) && memcmp(original, back, SECTOR_SIZE) == 0);

    /* More sectors than any one driver command can carry.
     *
     * The AHCI driver stops at eight and the ATA one at 255, so a request
     * for sixteen is a single command on one of them and has to be split on
     * the other. Nothing above this layer should be able to tell which,
     * and until the block layer split them, nothing above it could: the
     * boot log asked for 32 and quietly wrote nothing on every AHCI
     * machine while passing on every ATA one. */
    if (blk_sectors() > 64) {
        static u8 big_out[16 * SECTOR_SIZE], big_in[16 * SECTOR_SIZE];
        static u8 big_keep[16 * SECTOR_SIZE];
        u32 at = blk_sectors() - 24;

        ok("a run longer than one command reads", blk_read(at, 16, big_keep));

        /* A pattern that differs between sectors, so a split that repeats or
           drops one is visible rather than averaging out. */
        for (u32 i = 0; i < sizeof(big_out); i++)
            big_out[i] = (u8)((i / SECTOR_SIZE) * 31 + (i % SECTOR_SIZE) * 7 + 11);

        ok("a run longer than one command writes", blk_write(at, 16, big_out));
        ok("and reads back", blk_read(at, 16, big_in));
        ok("every sector of it survived the round trip",
           memcmp(big_out, big_in, sizeof(big_out)) == 0);

        blk_write(at, 16, big_keep);
        ok("the long run was put back",
           blk_read(at, 16, big_in) && memcmp(big_keep, big_in, sizeof(big_keep)) == 0);

        /* A count of zero is not a short request, it is a caller mistake. */
        ok("a zero length request is refused", blk_read(at, 0, big_in) == false);
    }
}

static void test_net(void) {
    if (!net_up()) { kprintf("  SKIP  no network card\n"); return; }
    const u8 *m = net_mac();
    bool nonzero = false;
    for (int i = 0; i < 6; i++) if (m[i]) nonzero = true;
    ok("card has a mac address", nonzero);

    ok("dhcp obtained a lease", net_dhcp(8000));
    if (net_ip()) {
        ok("address is not zero", net_ip() != 0);
        ok("gateway was supplied", net_gateway() != 0);
        ok("resolver was supplied", net_dns() != 0);
        ok("gateway answers icmp", net_ping(net_gateway(), 3000) >= 0);
        ipv4_t ip = 0;
        ok("dns resolves a name", net_resolve("example.com", &ip, 6000) && ip != 0);
    }
}


static void test_video(void) {
    if (!fb_active()) { kprintf("  SKIP  no framebuffer\n"); return; }
    ok("mode is the one that was asked for", fb_width() == 1024 && fb_height() == 768);
    ok("pitch matches the width", fb_pitch() == fb_width() * 4);
    ok("text grid derives from the font", fbcon_cols() == fb_width() / FONT_W);

    /* Write a pixel and read it back out of the back buffer. */
    u32 probe = RGB(0x12, 0x34, 0x56);
    u32 keep = fb_get(900, 700);
    fb_put(900, 700, probe);
    ok("pixel round trips", fb_get(900, 700) == probe);

    fb_rect(880, 690, 20, 20, RGB(1, 2, 3));
    ok("rect fills its interior", fb_get(890, 700) == RGB(1, 2, 3));
    ok("rect stops at its edge", fb_get(905, 700) != RGB(1, 2, 3));
    fb_put(900, 700, keep);

    bool inked = false;
    for (u32 y = 0; y < FONT_H; y++) if (font8x16[(u8)'A' - FONT_FIRST][y]) inked = true;
    ok("font has glyph data", inked);
}

static void test_mouse(void) {
    if (!mouse_present()) { kprintf("  SKIP  no mouse\n"); return; }
    ok("pointer starts on screen",
       mouse_x() >= 0 && mouse_x() < (i32)fb_width() &&
       mouse_y() >= 0 && mouse_y() < (i32)fb_height());
}


static void test_fat(void) {
    if (!blk_present()) { kprintf("  SKIP  no disk attached\n"); return; }
    ok("volume is mounted", fat_mounted());
    ok("cluster count is in the FAT16 range",
       fat_total_clusters() >= 4085 && fat_total_clusters() <= 65524);
    ok("clusters are a sensible size", fat_cluster_bytes() >= 512);

    /* A file that spans more than one cluster exercises chain following,
       which a single sector write would not. */
    u32 big = fat_cluster_bytes() * 2 + 137;
    u8 *out = (u8 *)kmalloc(big);
    u8 *in  = (u8 *)kmalloc(big);
    if (!out || !in) { ok("scratch buffers", false); return; }
    for (u32 i = 0; i < big; i++) out[i] = (u8)(i * 31 + 7);

    ok("write a multi-cluster file", fat_write_file("/sptest.bin", out, big));
    int got = fat_read_file("/sptest.bin", in, big);
    ok("read back the same length", got == (int)big);
    ok("read back the same bytes", got == (int)big && memcmp(out, in, big) == 0);
    ok("it appears in the directory", fat_count("/") > 0);
    ok("delete removes it", fat_delete_file("/sptest.bin"));
    ok("and it is gone", fat_read_file("/sptest.bin", in, big) < 0);

    /* A subdirectory is a cluster chain rather than the fixed root area, so
       it exercises a different path through the same code. */
    fat_delete_file("/sub/deep.bin");
    fat_rmdir("/sub");
    ok("a subdirectory can be made", fat_mkdir("/sub"));
    ok("a file spanning clusters fits in it", fat_write_file("/sub/deep.bin", out, big));
    ok("and reads back byte for byte",
       fat_read_file("/sub/deep.bin", in, big) == (int)big && memcmp(out, in, big) == 0);
    ok("the root does not show what is inside it", fat_count("/sub") == 1);
    ok("cleaning up works",
       fat_delete_file("/sub/deep.bin") && fat_rmdir("/sub"));

    kfree(out);
    kfree(in);
}


/* Builds a minimal but structurally valid ELF32 header in a caller supplied
   buffer, so individual fields can then be corrupted one at a time. */
/* A minimal but well formed ELF64 executable, which each test then damages
   in one specific way. The offsets are the ones in the specification rather
   than a struct, so that a mistake in the loader's own struct cannot hide
   here as well. */
#define ELF_PHOFF 64
static void make_elf(u8 *buf, u64 vaddr) {
    memset(buf, 0, 192);
    buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 2;                       /* 64 bit */
    buf[5] = 1;                       /* little endian */
    *(u16 *)(buf + 16) = 2;           /* ET_EXEC */
    *(u16 *)(buf + 18) = 62;          /* EM_X86_64 */
    *(u64 *)(buf + 24) = vaddr;       /* entry */
    *(u64 *)(buf + 32) = ELF_PHOFF;   /* phoff */
    *(u16 *)(buf + 54) = 56;          /* phentsize */
    *(u16 *)(buf + 56) = 1;           /* phnum */

    u8 *ph = buf + ELF_PHOFF;
    *(u32 *)(ph + 0)  = 1;            /* PT_LOAD */
    *(u32 *)(ph + 4)  = 5;            /* read and execute */
    *(u64 *)(ph + 8)  = 0;            /* offset */
    *(u64 *)(ph + 16) = vaddr;        /* vaddr */
    *(u64 *)(ph + 32) = 16;           /* filesz */
    *(u64 *)(ph + 40) = 16;           /* memsz */
}

static void test_elf(void) {
    u8 buf[192];
    u64 entry = 0;
    u64 dir = paging_new_directory();
    if (!dir) { ok("scratch address space", false); return; }

    ok("rejects a buffer too short to hold a header",
       elf_load(dir, buf, 8, &entry) == ELF_ERR_SHORT);

    make_elf(buf, USER_SPACE_BASE + 0x40000000ull);
    buf[1] = 'X';
    ok("rejects a bad magic number", elf_load(dir, buf, sizeof(buf), &entry) == ELF_ERR_MAGIC);

    make_elf(buf, USER_SPACE_BASE + 0x40000000ull);
    buf[4] = 1;                                     /* claims 32 bit */
    ok("rejects the wrong class", elf_load(dir, buf, sizeof(buf), &entry) == ELF_ERR_CLASS);

    make_elf(buf, USER_SPACE_BASE + 0x40000000ull);
    *(u16 *)(buf + 18) = 40;                        /* ARM */
    ok("rejects another machine", elf_load(dir, buf, sizeof(buf), &entry) == ELF_ERR_TYPE);

    /* The important one: a program must not be able to ask to be mapped
       over the kernel. */
    make_elf(buf, 0x00100000);
    ok("rejects a segment inside kernel memory",
       elf_load(dir, buf, sizeof(buf), &entry) == ELF_ERR_RANGE);

    make_elf(buf, USER_SPACE_BASE + 0x40000000ull);
    *(u64 *)(buf + ELF_PHOFF + 32) = 4096;          /* filesz past the end */
    ok("rejects a segment that runs off the end of the file",
       elf_load(dir, buf, sizeof(buf), &entry) == ELF_ERR_OVERFLOW);

    make_elf(buf, USER_SPACE_BASE + 0x40000000ull);
    ok("accepts a well formed header",
       elf_load(dir, buf, sizeof(buf), &entry) == ELF_OK && entry == USER_SPACE_BASE + 0x40000000ull);

    paging_free_directory(dir);
}

static void test_userspace(void) {
    /* The stub makes six putc calls and then exits, so the syscall counter
       moving is direct evidence that ring 3 code ran and crossed back in.
       Counting tasks would race: it can finish before the check. */
    u32 before = syscall_count();
    u32 free_before = pmm_free_frames();
    int pid = user_spawn_stub("selftest-ring3");
    ok("a ring 3 task can be created", pid > 0);
    if (pid <= 0) return;

    task_wait((u32)pid);
    ok("it reached exit on its own", !task_alive((u32)pid));
    ok("ring 3 code issued system calls", syscall_count() >= before + 7);
    /* Waited for rather than asserted outright.
     *
     * task_wait returns as soon as the task is dead, and the address space
     * is freed afterwards by the collector, which runs on a scheduler tick.
     * Those are not the same moment. Under software emulation a tick almost
     * always landed in between and this passed as a plain equality; with
     * hardware acceleration the check outruns the collector and it failed
     * about one run in three. The memory is reclaimed either way, so what
     * this should say is that it comes back, not that it comes back before
     * anything else has had a turn. */
    u32 reclaimed = 0;
    for (int i = 0; i < 50 && !reclaimed; i++) {
        if (pmm_free_frames() >= free_before) reclaimed = 1;
        else sleep_ms(10);
    }
    ok("its address space was reclaimed", reclaimed);
}


static void test_gfx(void) {
    const int W = 32, H = 24;
    u32 *px = (u32 *)kmalloc((u32)(W * H) * 4);
    if (!px) { ok("scratch surface", false); return; }

    surf_clear(px, W, H, 0x111111);
    ok("clear fills every pixel", px[0] == 0x111111 && px[W * H - 1] == 0x111111);

    surf_rect(px, W, H, 4, 4, 8, 8, 0x222222);
    ok("rect fills its interior", px[6 * W + 6] == 0x222222);
    ok("rect leaves the outside alone", px[2 * W + 2] == 0x111111);

    /* A rectangle hanging off the edge must clip rather than write past the
       end of the surface. */
    surf_rect(px, W, H, -4, -4, 8, 8, 0x333333);
    ok("rect clips at the top left", px[0] == 0x333333 && px[3 * W + 3] == 0x333333);
    surf_rect(px, W, H, W - 4, H - 4, 8, 8, 0x444444);
    ok("rect clips at the bottom right", px[(H - 1) * W + (W - 1)] == 0x444444);

    surf_clear(px, W, H, 0);
    surf_line(px, W, H, 2, 2, 20, 12, 1, 0x555555);
    ok("line marks its start", px[2 * W + 2] == 0x555555);
    ok("line marks its end", px[12 * W + 20] == 0x555555);

    surf_clear(px, W, H, 0);
    surf_disc(px, W, H, 16, 12, 4, 0x666666);
    ok("disc fills its centre", px[12 * W + 16] == 0x666666);
    ok("disc stays inside its radius", px[12 * W + 25] == 0);

    surf_clear(px, W, H, 0);
    surf_text(px, W, H, 1, 1, "A", 0x777777);
    bool inked = false;
    for (int i = 0; i < W * H; i++) if (px[i] == 0x777777) inked = true;
    ok("text puts ink on the surface", inked);

    char buf[32];
    kformat(buf, sizeof(buf), "%d/%s/%x", 42, "ok", 255);
    ok("kformat formats", strcmp(buf, "42/ok/ff") == 0);
    kformat(buf, sizeof(buf), "%-4s/%-3d", "x", 7);
    ok("kformat left aligns", strcmp(buf, "x   /7  ") == 0);
    kformat(buf, 6, "abcdefghij");
    ok("kformat respects the buffer size", strlen(buf) == 5);

    kfree(px);
}

static void test_wm(void) {
    if (!fb_active()) { kprintf("  SKIP  no framebuffer\n"); return; }

    window_t *a = wm_create("a", 10, 10, 120, 80);
    window_t *b = wm_create("b", 40, 40, 120, 80);
    ok("windows can be created", a && b);
    if (!a || !b) return;

    ok("outer size allows for the chrome",
       wm_outer_w(a) == 120 + WM_BORDER * 2 &&
       wm_outer_h(a) == 80 + WM_TITLE_H + WM_BORDER);

    /* Closing must also drop the manager's reference, or the next composite
       walks freed memory. */
    wm_close(a);
    wm_close(b);
    ok("windows can be closed", true);
}

static void test_winsrv(void) {
    const u32 PID = 4242, OTHER = 4243;

    int h = winsrv_create(PID, "selftest", 64, 48);
    ok("a window can be created for a program", h >= 0);
    if (h < 0) return;

    ok("its size comes back", winsrv_size(PID, h) == ((64 << 16) | 48));
    ok("another program cannot see the handle", winsrv_size(OTHER, h) == -1);

    u64 ua = winsrv_surface(PID, h, paging_current_directory());
    ok("the surface maps into the caller", ua == WINSRV_SURFACE_BASE);

    /* Writing through the address the program was given must land in the
       pixels the window manager composites from. Those pages are identity
       mapped for the kernel, so the physical address is readable here. */
    if (ua) {
        *(volatile u32 *)ua = 0xDEADBEEF;
        u64 phys = virt_to_phys(ua);
        ok("it aliases the window pixels", phys && *(volatile u32 *)phys == 0xDEADBEEF);

        /* Pixel zero for the program has to be pixel zero for the window.
           Mapping the page before it puts every row out by a fixed amount,
           which draws a recognisable but wrong picture. */
        window_t *win = winsrv_window(PID, h);
        ok("and starts exactly where the window does", win && phys == (u64)win->canvas);

        /* The last pixel must be inside the mapping too. */
        u64 last = ua + (64ull * 48ull - 1) * 4;
        *(volatile u32 *)last = 0xFEEDFACE;
        ok("the whole surface is mapped",
           win && win->canvas[64 * 48 - 1] == 0xFEEDFACE);
        ok("asking again returns the same address",
           winsrv_surface(PID, h, paging_current_directory()) == ua);
    }

    ok("a foreign program cannot map it", winsrv_surface(OTHER, h, paging_current_directory()) == 0);
    ok("commit is accepted", winsrv_commit(PID, h));
    ok("a foreign commit is not", !winsrv_commit(OTHER, h));

    /* Closing goes through wm_close, which must not release a surface the
       server carved out of a larger allocation: that pointer is not one
       kmalloc returned, and the server frees the real one itself. A heap
       that still balances afterwards is the evidence. */
    u32 heap_before_close = heap_used();
    ok("the window closes", winsrv_close(PID, h));
    ok("the handle is dead afterwards", winsrv_surface(PID, h, paging_current_directory()) == 0);
    ok("closing it gave the heap back rather than corrupting it",
       heap_used() < heap_before_close);

    /* Everything a task owned goes away with it. */
    int a = winsrv_create(PID, "one", 40, 40);
    int b = winsrv_create(PID, "two", 40, 40);
    ok("a program can hold more than one window", a >= 0 && b >= 0 && a != b);
    winsrv_release(PID);
    ok("its windows go when the program does", winsrv_size(PID, a) == -1 && winsrv_size(PID, b) == -1);
}

static void test_builtin(void) {
    ok("programs ship with the kernel", builtin_count_programs() >= 3);

    u32 size = 0;
    ok("paint is one of them", vfs_stat("/bin/paint", &size, 0) && size > 1024);

    u8 head[4] = { 0, 0, 0, 0 };
    vfs_read("/bin/paint", head, sizeof(head));
    ok("and it is a real ELF",
       head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F');

    ok("they cannot be overwritten", !vfs_write("/bin/paint", "x", 1));
    ok("nor deleted", !vfs_delete("/bin/paint"));

    /* The disk must not be holding its own copy, or rebuilding the kernel
       would change nothing on a machine that had already booted once. */
    if (blk_present() && fat_mounted()) {
        char name[VFS_NAME_MAX];
        bool on_disk = false;
        for (u32 i = 0; ; i++) {
            if (fat_list("/", i, name, 0, 0) != 1) break;
            if (strcmp(name, "paint") == 0) on_disk = true;
        }
        ok("and are not written to the disk", !on_disk);
    }
}

static void test_open_files(void) {
    vfs_delete("/fd.txt");

    int fd = vfs_open("/fd.txt", O_WRITE | O_CREATE);
    ok("a file can be opened for writing", fd >= 0);
    if (fd < 0) return;

    ok("writing reports what it took", vfs_fd_write(fd, "abcdefgh", 8) == 8);
    ok("seeking back works", vfs_fd_seek(fd, 0, 0) == 0);
    ok("overwriting in place works", vfs_fd_write(fd, "ABC", 3) == 3);
    ok("the size is what was written", vfs_fd_size(fd) == 8);
    ok("closing writes it out", vfs_close(fd));

    char buf[16];
    ok("and the file has the edit",
       vfs_read("/fd.txt", buf, sizeof(buf)) == 8 && memcmp(buf, "ABCdefgh", 8) == 0);

    fd = vfs_open("/fd.txt", O_READ);
    ok("reading a chunk at a time works", fd >= 0 && vfs_fd_read(fd, buf, 3) == 3);
    ok("it starts where it left off", vfs_fd_read(fd, buf, 3) == 3 && memcmp(buf, "def", 3) == 0);
    ok("seeking to the end reports the size", vfs_fd_seek(fd, 0, 2) == 8);
    ok("reading past the end gives nothing", vfs_fd_read(fd, buf, 4) == 0);
    vfs_close(fd);

    ok("a missing file will not open without create", vfs_open("/nope.txt", O_READ) < 0);
    vfs_delete("/fd.txt");
}

static void test_pins(void) {
    vfs_delete(PIN_FILE);
    pins_init();

    /* A machine that has never been told otherwise starts with the programs
       it ships, which is also what makes the taskbar worth looking at the
       first time somebody opens the desktop. */
    ok("a machine with no list starts with one", pins_count() > 0);
    ok("and the terminal is on it", pins_find("/bin/term") >= 0);
    ok("something that is not a program is not", pins_find("/bin/nope") < 0);

    int had = pins_count();
    ok("an app can be pinned", pins_add("Count", "/bin/count"));
    ok("and is then on the list", pins_count() == had + 1);
    ok("pinning the same one twice does nothing", !pins_add("Count", "/bin/count"));

    /* The order is the list, because the order is what a person drags an
       icon along the panel to change. */
    int from = pins_find("/bin/count");
    pins_move(from, 0);
    ok("an app can be moved to the front", pins_find("/bin/count") == 0);
    ok("and what was in front moved along", pins_find("/bin/term") == 1);
    pins_move(0, pins_count() - 1);
    ok("and to the back", pins_find("/bin/count") == pins_count() - 1);

    /* Out of range is a thing a drag does at both ends of the panel. */
    pins_move(0, -4);
    ok("moving past the front stays at the front", pins_find("/bin/term") == 0);
    pins_move(0, 99);
    ok("and past the back stays at the back", pins_find("/bin/term") == pins_count() - 1);

    /* What survives a reboot is what is in the file. */
    int want = pins_count();
    ok("the list saves", pins_save());
    pins_init();
    ok("and comes back the same length", pins_count() == want);
    ok("in the same order", pins_find("/bin/term") == want - 1);
    ok("with the names it was given",
       !strcmp(pin_at(pins_find("/bin/count"))->label, "Count"));

    /* Emptying it has to stick. A list somebody cleared that came back full
       on the next boot would be the taskbar refusing to be configured. */
    while (pins_count()) pins_remove(0);
    ok("everything can be unpinned", pins_count() == 0);
    pins_init();
    ok("and an emptied list stays empty", pins_count() == 0);

    /* There is a limit and it is the array's. */
    for (int i = 0; i < PIN_MAX + 4; i++) {
        char path[16];
        kformat(path, sizeof(path), "/bin/p%d", i);
        pins_add("X", path);
    }
    ok("the list stops at what it can hold", pins_count() == PIN_MAX);

    vfs_delete(PIN_FILE);
    pins_init();
}

static void test_theme(void) {
    vfs_delete(THEME_FILE);
    theme_init();

    ok("there is a default accent", theme()->accent != 0);
    ok("and it is one of the presets", theme_current_preset() >= 0);

    /* A preset changes the palette without touching anything else. */
    int corner_before = theme()->corner;
    theme_apply_preset(3);
    ok("a preset changes the accent", theme()->accent == theme_preset_accent(3));
    ok("and leaves the rest alone", theme()->corner == corner_before);

    /* Saving and reloading has to round trip, or the settings window would
       appear to work and then forget. */
    theme_apply_preset(4);
    ok("the theme saves", theme_save());
    theme_apply_preset(0);
    ok("reloading reports a change", theme_reload());
    ok("and brings the saved accent back", theme()->accent == theme_preset_accent(4));
    ok("reloading again reports no change", !theme_reload());

    /* The file is the interface a ring 3 program writes, so hand-written
       text has to work exactly as well as what theme_save produces. */
    const char *hand = "# by hand\npreset 2\nwallpaper 1\ncorner 14\nshadows 0\n";
    ok("a hand written config writes", vfs_write(THEME_FILE, hand, (u32)strlen(hand)));
    ok("and is picked up", theme_reload());
    ok("preset applied", theme()->accent == theme_preset_accent(2));
    ok("wallpaper applied", theme()->wallpaper == WALLPAPER_GRID);
    ok("corner applied", theme()->corner == 14);
    ok("shadows applied", !theme()->shadows);

    /* A value out of range must be clamped rather than believed. */
    const char *bad = "wallpaper 99\ncorner 900\n";
    vfs_write(THEME_FILE, bad, (u32)strlen(bad));
    theme_reload();
    ok("a silly wallpaper falls back", theme()->wallpaper < WALLPAPER_COUNT);
    ok("a silly corner is clamped", theme()->corner <= 20);

    vfs_delete(THEME_FILE);
}

/* --- the other processors -------------------------------------------------

   These have to prove three separate things, because a processor that
   started but never runs anything looks exactly like one that works:

     - it executes code we gave it, and the code sees the right argument
     - it runs at the same time as this one rather than instead of it
     - the lock between them actually excludes

   The counter test does the last two together. Every participant adds the
   same number of times under the lock, and the total has to be exact. A
   broken lock loses increments; a processor that never ran loses all of
   them at once. */

#define SMP_ADDS 20000

static spinlock_t test_lock;
static volatile u32 shared_counter;
static volatile u32 seen_arg[SMP_MAX_CPUS];

static void smp_add_work(void *arg) {
    u64 who = (u64)arg;
    if (who < SMP_MAX_CPUS) seen_arg[who] = who + 1;
    for (u32 i = 0; i < SMP_ADDS; i++) {
        spin_lock(&test_lock);
        shared_counter++;
        spin_unlock(&test_lock);
    }
}

static void test_smp(void) {
    ok("the firmware described at least one processor", smp_cpu_count() >= 1);
    ok("this one is running", smp_cpu(0) && smp_cpu(0)->started);

    if (smp_cpu_count() < 2) {
        kprintf("  SKIP  only one processor on this machine\n");
        return;
    }

    ok("every processor found was started", smp_started() == smp_cpu_count());

    u32 helpers = 0;
    for (u32 i = 1; i < smp_cpu_count(); i++)
        if (smp_cpu(i)->started) helpers++;
    ok("at least one other processor came up", helpers > 0);

    /* An idle processor should be asleep, not burning a core.
     *
     * This used to check the opposite: that the spin counter was climbing,
     * which it was, because the idle loop never stopped running. That is
     * exactly the thing that stopped zelr booting on a machine given more
     * than one processor, and the check said it was working as intended.
     * A counter that moves a handful of times is one that woke, looked and
     * went back to sleep; one that moves thousands of times is spinning. */
    u64 spins_before = smp_cpu(1)->spins;
    sleep_ms(50);
    u64 moved = smp_cpu(1)->spins - spins_before;
    ok("an idle processor is asleep rather than spinning", moved < 100);

    /* Hand the same job to all of them and join in. */
    shared_counter = 0;
    test_lock = 0;
    for (u32 i = 0; i < SMP_MAX_CPUS; i++) seen_arg[i] = 0;

    u32 dispatched = 0;
    for (u64 i = 1; i < smp_cpu_count(); i++)
        if (smp_run((u32)i, smp_add_work, (void *)i)) dispatched++;
    ok("work was accepted by every other processor", dispatched == helpers);

    smp_add_work((void *)0);              /* this processor does a share too */

    bool joined = true;
    for (u32 i = 1; i < smp_cpu_count(); i++)
        if (!smp_wait(i, 8000)) joined = false;
    ok("they all finished", joined);

    ok("the count is exact, so the lock held",
       shared_counter == SMP_ADDS * (helpers + 1));

    bool args_ok = (seen_arg[0] == 1);
    for (u32 i = 1; i < smp_cpu_count(); i++)
        if (smp_cpu(i)->started && seen_arg[i] != i + 1) args_ok = false;
    ok("each one was handed its own argument", args_ok);

    bool counted = true;
    for (u32 i = 1; i < smp_cpu_count(); i++)
        if (smp_cpu(i)->started && smp_cpu(i)->jobs != 1) counted = false;
    ok("each one recorded exactly one job", counted);

    /* Nothing should be left holding the lock. */
    spin_lock(&test_lock);
    spin_unlock(&test_lock);
    ok("the lock is free afterwards", true);
}

/* --- waiting --------------------------------------------------------------

   The point of a wait queue is that a waiting task costs nothing. Proving it
   works needs two things shown separately: that a blocked task really is
   woken by somebody else, and that while it is blocked it is not being
   scheduled at all. The second is what a spin loop would fail. */

static volatile int waiter_state;   /* 0 not started, 1 blocked, 2 woken */
static volatile u32 waiter_slices;
static int wait_channel;

static void waiter_task(void) {
    waiter_state = 1;
    wait_on(&wait_channel, 0);          /* no deadline: only a wake ends this */
    task_t *me = task_current();
    waiter_slices = me ? me->slices : 0;
    waiter_state = 2;
    task_exit_with(7);
}

static void test_waiting(void) {
    waiter_state = 0;
    waiter_slices = 0;

    task_t *t = task_create("waiter", waiter_task);
    ok("a task can be created to wait", t != 0);
    if (!t) return;
    u32 pid = t->pid;

    /* Let it reach the wait. */
    for (int i = 0; i < 50 && waiter_state == 0; i++) sleep_ms(10);
    ok("it got as far as blocking", waiter_state == 1);
    ok("and the scheduler knows it is blocked", task_blocked_count() >= 1);

    /* While blocked it must not be running. A spin loop would climb here. */
    u32 before = t->slices;
    sleep_ms(150);
    ok("a blocked task is not scheduled at all", t->slices == before);
    ok("and it has not woken by itself", waiter_state == 1);

    wake_all(&wait_channel);
    for (int i = 0; i < 50 && waiter_state != 2; i++) sleep_ms(10);
    ok("waking it lets it run again", waiter_state == 2);

    /* And the status it exited with comes back to whoever asks. */
    ok("its exit status is collected", task_wait(pid) == 7);
    ok("waiting on a task that never existed says so", task_wait(999999) == -1);
}

/* --- work against waiting -------------------------------------------------
 *
 * A slice is a tick the scheduler handed a task. It is not a tick the task
 * used: this kernel is round robin, so a loop waiting for a key is picked
 * every tick and spends the slice halted. Counting slices as work made an
 * idle machine read a hundred per cent, and the system monitor said so for
 * a whole release before anyone worked out that it could not be right.
 *
 * So both directions are checked here. Spinning has to show up as work, and
 * waiting has to show up as waiting, and neither is allowed to look like the
 * other. Getting either backwards fails this. */
static void test_idle_accounting(void) {
    task_t *me = task_current();
    ok("there is a task to measure", me != 0);
    if (!me) return;

    /* Spinning. The tick count is read straight from the timer, so the loop
       ends after a known stretch of time whatever the processor's speed. */
    u32 slices = me->slices;
    u64 idle = me->idle_ticks;
    u64 until = timer_ticks() + 30;
    while (timer_ticks() < until) { /* busy on purpose */ }

    u32 spun_slices = me->slices - slices;
    u64 spun_idle = me->idle_ticks - idle;
    ok("a spinning task is given slices", spun_slices > 10);
    ok("and none of them count as waiting", spun_idle == 0);

    /* Waiting. The same stretch of time, spent halted. */
    slices = me->slices;
    idle = me->idle_ticks;
    until = timer_ticks() + 30;
    while (timer_ticks() < until) task_idle_wait();

    u32 waited_slices = me->slices - slices;
    u64 waited_idle = me->idle_ticks - idle;
    ok("a waiting task is given slices too", waited_slices > 10);
    ok("and nearly all of them count as waiting",
       waited_idle * 10 >= (u64)waited_slices * 8);

    /* Which is the whole point: the two are told apart. */
    ok("so waiting and working do not look the same",
       waited_idle > spun_idle + 10);
}

/* --- the trackpad ---------------------------------------------------------
 *
 * No emulator has a Synaptics pad, so nothing here can show that one is
 * found: that has to happen on a real laptop. What can be shown is
 * everything after a report arrives, by building reports and feeding them
 * to the decoder.
 *
 * Which is the half where the mistakes are. A pad reports where the finger
 * is and a pointer needs how far it moved, and every one of the checks below
 * that is not about arithmetic is about a way that subtraction goes wrong:
 * a finger that lands somewhere else, a finger moving too slowly to divide,
 * a second finger arriving. */

/* A report, assembled the way the pad assembles one. The bits of x, y and w
   are scattered across four of the six bytes, which is the sort of layout
   that comes of adding fields to a three byte packet for twenty years. */
static void syn_pack(u8 out[6], i32 x, i32 y, u8 z, u8 w, u8 btns) {
    out[0] = (u8)(0x80 | (((w >> 2) & 3) << 4) | (((w >> 1) & 1) << 2)
                  | (btns & 3));
    out[1] = (u8)((((y >> 8) & 0x0F) << 4) | ((x >> 8) & 0x0F));
    out[2] = z;
    out[3] = (u8)(0xC0 | (((y >> 12) & 1) << 5) | (((x >> 12) & 1) << 4)
                  | ((w & 1) << 2) | (btns & 3));
    out[4] = (u8)(x & 0xFF);
    out[5] = (u8)(y & 0xFF);
}

static void syn_feed(i32 x, i32 y, u8 z, u8 w, u8 btns) {
    u8 p[6];
    syn_pack(p, x, y, z, w, btns);
    for (int i = 0; i < 6; i++) syn_byte(p[i]);
}

static void syn_lift(void) { syn_feed(0, 0, 0, 0, 0); }

/* Somewhere in the middle of the screen, so that nothing below is measuring
   the edge the pointer is clamped against. */
static bool park_pointer(void) {
    mouse_inject(-20000, 20000, 0);       /* into the corner */
    mouse_inject(400, -300, 0);           /* and out to a known place */
    return mouse_x() == 400 && mouse_y() == 300;
}

static void test_trackpad(void) {
    /* The knock must not mistake an ordinary mouse for a pad, which is what
       is on the other end of this in every emulator and on most desks.
       Asked at boot and answered wrongly, this machine would be driving a
       mouse through six byte reports and the pointer would be dead.
 
       Not by calling syn_detect again: that talks to the hardware, and by
       now the timer is draining the controller on every tick and would eat
       the answer. What is asked is what the machine concluded at boot. */
    ok("this machine did not conclude it has a trackpad", !syn_present());

    /* And the decision itself, which is the part an emulator can exercise:
       a pad's answer, a mouse's answer, and one bit between them. */
    {
        static const u8 pad[3]   = { 0x02, 0x47, 0x07 };
        static const u8 mouse[3] = { 0x00, 0x02, 0x64 };
        static const u8 near[3]  = { 0x02, 0x46, 0x07 };
        ok("a pad's answer is recognised", syn_answer_is_pad(pad));
        ok("a mouse's answer is not", !syn_answer_is_pad(mouse));
        ok("and neither is one bit away from a pad's",
           !syn_answer_is_pad(near));
    }

    syn_reset_state(true);
    ok("the pointer can be parked away from the edges", park_pointer());

    /* --- a position, turned into movement ------------------------------- */
    i32 x0 = mouse_x(), y0 = mouse_y();
    syn_feed(3000, 3000, 60, 4, 0);
    ok("the first report of a contact moves nothing",
       mouse_x() == x0 && mouse_y() == y0);

    syn_feed(3600, 3000, 60, 4, 0);       /* 600 units right, six to a pixel */
    ok("a finger moving right moves the pointer right",
       mouse_x() == x0 + 100);

    /* The pad counts y upward and the screen counts it downward. Getting
       this backwards is the one bug in a trackpad everybody notices. */
    syn_feed(3600, 3600, 60, 4, 0);
    ok("a finger moving up the pad moves the pointer up",
       mouse_y() == y0 - 100);
    syn_lift();

    /* --- slowly ---------------------------------------------------------- */
    /* Two units at a time, divided by six. Each one on its own is nothing,
       and three of them are a pixel only if the remainder is kept. Without
       that, a finger moved slowly does not move the pointer at all. */
    syn_reset_state(true);
    x0 = mouse_x();
    syn_feed(3000, 3000, 60, 4, 0);
    syn_feed(3002, 3000, 60, 4, 0);
    syn_feed(3004, 3000, 60, 4, 0);
    ok("two of six pixels is still nothing", mouse_x() == x0);
    syn_feed(3006, 3000, 60, 4, 0);
    ok("but the remainder is kept, so a slow finger still moves it",
       mouse_x() == x0 + 1);
    syn_lift();

    /* --- and too far ----------------------------------------------------- */
    /* A second finger landing changes which contact is being reported, and
       the difference looks like a flick across the whole pad. */
    syn_reset_state(true);
    x0 = mouse_x();
    syn_feed(3000, 3000, 60, 4, 0);
    syn_feed(4500, 3000, 60, 4, 0);
    ok("a jump no finger could make is not a movement", mouse_x() == x0);
    syn_lift();

    /* --- two fingers scroll ---------------------------------------------- */
    syn_reset_state(true);
    mouse_take_scroll();                  /* whatever was pending, cleared */
    x0 = mouse_x(); y0 = mouse_y();
    syn_feed(3000, 3000, 60, 0, 0);       /* width 0 is two fingers */
    syn_feed(3000, 2760, 60, 0, 0);       /* 240 units down the pad */
    ok("two fingers do not move the pointer",
       mouse_x() == x0 && mouse_y() == y0);
    ok("two fingers moved down the pad scroll down",
       mouse_take_scroll() == 2);
    syn_lift();

    /* The second finger landing is the real reason a position jumps, and it
       is caught by the count changing rather than by the distance. Without
       this, starting a scroll throws the pointer across the screen first. */
    syn_reset_state(true);
    mouse_take_scroll();
    x0 = mouse_x(); y0 = mouse_y();
    syn_feed(3000, 3000, 60, 4, 0);       /* one finger, the origin */
    syn_feed(3300, 3000, 60, 4, 0);       /* moving: 300 units, 50 pixels */
    ok("one finger moves the pointer", mouse_x() == x0 + 50);
    /* Away in both directions, because a second finger that lands at the
       same height would go straight down the scrolling path and the jump
       would never show as movement. This check missed exactly that until
       the decoder was broken on purpose to see whether it would. */
    syn_feed(3900, 2400, 60, 0, 0);       /* a second lands, far away */
    ok("a second finger landing does not move the pointer",
       mouse_x() == x0 + 50 && mouse_y() == y0);
    ok("nor does it scroll", mouse_take_scroll() == 0);

    /* And lifting one of them is the same problem the other way round. */
    syn_feed(3500, 2400, 60, 4, 0);       /* back to one, somewhere else */
    ok("a finger leaving does not move the pointer either",
       mouse_x() == x0 + 50 && mouse_y() == y0);
    syn_lift();

    /* --- tapping --------------------------------------------------------- */
    syn_reset_state(true);
    syn_feed(3000, 3000, 60, 4, 0);
    syn_lift();
    ok("a quick touch that went nowhere is a left click",
       mouse_buttons() == 0x01);

    /* And is held, because the window manager reads the buttons once a pass
       and a click released before the next one never happened. */
    syn_tick();
    ok("and the click is still down a moment later", mouse_buttons() == 0x01);
    sleep_ms(120);
    syn_tick();
    ok("and is let go after that", mouse_buttons() == 0);

    /* Two fingers tap the other button. */
    syn_reset_state(true);
    syn_feed(3000, 3000, 60, 0, 0);
    syn_lift();
    ok("two fingers tapping is a right click", mouse_buttons() == 0x02);
    sleep_ms(120);
    syn_tick();

    /* A finger resting is not a tap, or the pointer clicks wherever it is
       left. */
    syn_reset_state(true);
    syn_feed(3000, 3000, 60, 4, 0);
    sleep_ms(300);
    syn_lift();
    ok("a finger held down is not a tap", mouse_buttons() == 0);

    /* Neither is one that moved: that was a drag of the pointer. */
    syn_reset_state(true);
    syn_feed(3000, 3000, 60, 4, 0);
    syn_feed(3150, 3000, 60, 4, 0);
    syn_lift();
    ok("a touch that travelled is not a tap", mouse_buttons() == 0);

    /* --- finding the start of a report ----------------------------------- */
    /* Six bytes with no length prefix, read from a stream that can be joined
       half way through. The first byte of a report has its top two bits 10
       and the fourth has 11, and both are checked, so a stream out of step
       costs one report rather than decoding into a pointer that flies off. */
    syn_reset_state(true);
    x0 = mouse_x();
    syn_byte(0x40); syn_byte(0x00); syn_byte(0xFF);   /* none of them a start */
    syn_feed(3000, 3000, 60, 4, 0);                   /* the origin */
    syn_feed(3600, 3000, 60, 4, 0);
    ok("a report found mid stream is still decoded", mouse_x() == x0 + 100);

    /* And the second anchor: three good bytes then a fourth that cannot be
       one, which has to throw the report away rather than shift into it. */
    syn_reset_state(true);
    x0 = mouse_x();
    syn_feed(3000, 3000, 60, 4, 0);
    syn_byte(0x80); syn_byte(0x00); syn_byte(0x3C);
    syn_byte(0x00);                                   /* not 11 at the top */
    syn_feed(3600, 3000, 60, 4, 0);
    ok("a fourth byte that cannot be one throws the report away",
       mouse_x() == x0 + 100);

    syn_lift();
    syn_reset_state(false);
    mouse_inject(0, 0, 0);
}

static volatile int timeout_reached;

static void timeout_task(void) {
    /* Nothing ever wakes this address, so only the deadline can end it. */
    static int never;
    bool woken = wait_on(&never, 120);
    timeout_reached = woken ? 1 : 2;
    task_exit_with(0);
}

static void test_wait_timeout(void) {
    timeout_reached = 0;
    task_t *t = task_create("timeout", timeout_task);
    if (!t) { ok("scratch task", false); return; }

    u64 start = timer_ticks();
    for (int i = 0; i < 100 && timeout_reached == 0; i++) sleep_ms(10);
    u64 waited = timer_ticks() - start;

    ok("a wait with a deadline comes back", timeout_reached != 0);
    ok("and says it timed out rather than being woken", timeout_reached == 2);
    ok("after about the time it was given", waited >= 10 && waited <= 60);
    task_wait(t->pid);
}

/* --- the live tree --------------------------------------------------------

   /sys is generated on every read, which is the property worth checking:
   the same file read twice while something changes must not give the same
   answer twice. Everything else here is that nothing can write to it. */

static bool contains(const char *hay, const char *needle) {
    u32 n = strlen(needle);
    for (u32 i = 0; hay[i]; i++)
        if (strncmp(hay + i, needle, n) == 0) return true;
    return false;
}

static void test_live_tree(void) {
    char buf[SYSFS_MAX];
    bool is_dir = false;
    u32 size = 0;

    ok("/sys is a directory", vfs_stat("/sys", 0, &is_dir) && is_dir);
    ok("/bin is a directory", vfs_stat("/bin", 0, &is_dir) && is_dir);

    /* Both show up in a listing of the root, so ls finds them without them
       existing on any volume. */
    bool saw_sys = false, saw_bin = false;
    char name[VFS_NAME_MAX];
    for (u32 i = 0; vfs_list("/", i, name, 0, 0) == 1 && i < 64; i++) {
        if (strcmp(name, "sys") == 0) saw_sys = true;
        if (strcmp(name, "bin") == 0) saw_bin = true;
    }
    ok("and both are listed in the root", saw_sys && saw_bin);

    int n = vfs_read("/sys/version", buf, sizeof(buf) - 1);
    ok("a generated file reads", n > 0);
    if (n > 0) buf[n] = 0; else buf[0] = 0;
    ok("and says what this is", contains(buf, KERNEL_NAME) && contains(buf, KERNEL_VERSION));

    ok("its size is known before reading it",
       vfs_stat("/sys/version", &size, 0) && size == (u32)n);

    n = vfs_read("/sys/memory", buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = 0; else buf[0] = 0;
    ok("memory reports frames", contains(buf, "frames"));
    ok("and the heap", contains(buf, "heap"));

    n = vfs_read("/sys/tasks", buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = 0; else buf[0] = 0;
    ok("tasks lists this one", contains(buf, "selftest"));

    n = vfs_read("/sys/devices", buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = 0; else buf[0] = 0;
    ok("devices reports the video mode", contains(buf, "video"));

    /* The point of the whole thing: nothing is cached. Read the clock twice
       with time passing in between and the two must differ. */
    char first[SYSFS_MAX];
    int a = vfs_read("/sys/uptime", first, sizeof(first) - 1);
    if (a > 0) first[a] = 0; else first[0] = 0;
    sleep_ms(60);
    int b = vfs_read("/sys/uptime", buf, sizeof(buf) - 1);
    if (b > 0) buf[b] = 0; else buf[0] = 0;
    ok("reading the same file twice gives what is true now",
       a > 0 && b > 0 && strcmp(first, buf) != 0);

    /* And it is genuinely read-only, through every door. */
    ok("a generated file cannot be written", !vfs_write("/sys/memory", "x", 1));
    ok("nor deleted", !vfs_delete("/sys/memory"));
    ok("nor opened for writing", vfs_open("/sys/memory", O_WRITE) < 0);
    ok("a directory cannot be made inside it", !vfs_mkdir("/sys/mine"));
    ok("and /bin is the same", !vfs_write("/bin/paint", "x", 1));
    ok("but it can be opened for reading", vfs_open("/sys/memory", O_READ) >= 0);
    vfs_close(vfs_open("/sys/memory", O_READ));

    ok("something that is not there says so", vfs_read("/sys/nothing", buf, 16) < 0);
    ok("and does not stat", !vfs_stat("/sys/nothing", 0, 0));
}

static void test_layout(void) {
    layout_init();

    bool is_dir = false;
    ok("/home exists", vfs_stat("/home", 0, &is_dir) && is_dir);
    ok("/doc exists", vfs_stat("/doc", 0, &is_dir) && is_dir);
    ok("/cfg exists", vfs_stat("/cfg", 0, &is_dir) && is_dir);
    ok("/tmp exists", vfs_stat("/tmp", 0, &is_dir) && is_dir);

    u32 size = 0;
    ok("the documentation shipped with it", vfs_stat("/doc/readme", &size, 0) && size > 100);

    /* Seeding is once per disk, not once per boot: a second pass must not
       put back a file somebody deleted. */
    vfs_delete("/home/notes");
    vfs_write("/home/kept", "mine", 4);
    layout_init();
    ok("running it again does not restore a deleted file", !vfs_stat("/home/notes", 0, 0));
    ok("and leaves what the user wrote alone", vfs_stat("/home/kept", &size, 0) && size == 4);

    /* /tmp is emptied, which is the only thing that makes it a scratch
       directory rather than another place files accumulate. */
    vfs_write("/tmp/scratch", "gone next boot", 14);
    ok("a file can be put in /tmp", vfs_stat("/tmp/scratch", 0, 0));
    layout_init();
    ok("and starting up empties it", !vfs_stat("/tmp/scratch", 0, 0));

    vfs_delete("/home/kept");
}

/* The black box has to hold up in exactly the conditions where nothing else
   is working, so the parts worth checking are the ones that only matter
   then: that a long boot does not push the fault off the end, that a torn
   write is not read back as a good record, and that it refuses a disk it
   did not make. That last one is the dangerous direction: a false positive
   there overwrites somebody's partition table. */
static char bb_scratch[BB_BYTES];

static bool bb_contains(const char *needle) {
    const char *hay = bb_text();
    u32 n = bb_len(), m = strlen(needle);
    if (m > n) return false;
    for (u32 i = 0; i + m <= n; i++) {
        u32 j = 0;
        while (j < m && hay[i + j] == needle[j]) j++;
        if (j == m) return true;
    }
    return false;
}

static void test_blackbox(void) {
    bb_log("selftest marker alpha");
    ok("a line reaches the log", bb_contains("selftest marker alpha"));

    u32 before = bb_len();
    bb_mark("selftest phase");
    ok("a mark is written as a phase", bb_contains("== selftest phase"));
    ok("the log grew", bb_len() > before);

    /* A fault, without taking one: the register dump is the part of this
       that a real fault cannot be relied on to reach. */
    registers_t r;
    memset(&r, 0, sizeof(r));
    r.int_no = 14; r.err_code = 2; r.rip = 0xDEAD1000; r.rsp = 0x7FF0;
    bb_fault(&r, "a test fault");
    ok("a fault names itself", bb_contains("!! a test fault"));
    ok("a fault records the vector", bb_contains("vec=14"));
    ok("a fault records rip", bb_contains("0xdead1000"));

    /* Overflow. What must survive is the end, because the end is the fault. */
    for (int i = 0; i < 400; i++)
        bb_log("filler %d ........................................", i);
    bb_log("selftest marker omega");
    ok("the log stays inside its buffer", bb_len() < BB_BYTES);
    ok("the newest line survives overflow", bb_contains("selftest marker omega"));
    ok("the oldest line was dropped", !bb_contains("selftest marker alpha"));

    if (!blk_present()) { kprintf("  SKIP  no disk, cannot test the record\n"); return; }

    u8 boot[SECTOR_SIZE];
    if (!blk_read(0, 1, boot)) { ok("read the boot sector", false); return; }

    bool ours = boot[510] == 0x55 && boot[511] == 0xAA &&
                memcmp(boot + 3, "ZELR    ", 8) == 0 &&
                *(u16 *)(boot + 14) >= BB_LBA + BB_SECTORS;
    if (!ours) { kprintf("  SKIP  not a zelr volume with room reserved\n"); return; }

    /* Write, then recover, because bb_prev answers out of what the last
       recover found rather than off the disk. That indirection is the whole
       point: this boot's own flush lands on the sectors the last boot left,
       so the record has to be lifted into memory before that happens. */
    ok("the record is written", bb_flush());
    u32 want = bb_len();
    bb_recover();
    u32 n = bb_prev(bb_scratch, sizeof(bb_scratch));
    ok("the record reads back", n > 0);
    ok("what came back is what went out",
       n == want && memcmp(bb_scratch, bb_text(), n) == 0);

    /* A half written record must not be trusted. Flip one byte of the text
       and leave the checksum alone, which is what a power cut looks like. */
    u8 sec[SECTOR_SIZE];
    ok("read the record", blk_read(BB_LBA, 1, sec));
    u8 keep = sec[64];
    sec[64] = (u8)(keep ^ 0xFF);
    blk_write(BB_LBA, 1, sec);
    bb_recover();
    ok("a torn record is refused", bb_prev(bb_scratch, sizeof(bb_scratch)) == 0);
    sec[64] = keep;
    blk_write(BB_LBA, 1, sec);
    bb_recover();
    ok("the good record is readable again",
       bb_prev(bb_scratch, sizeof(bb_scratch)) > 0);

    /* Somebody else's disk. The name is the cheapest of the four checks to
       break, and breaking any one of them has to be enough. */
    u8 saved_name[8];
    memcpy(saved_name, boot + 3, 8);
    memcpy(boot + 3, "MSWIN4.1", 8);
    blk_write(0, 1, boot);
    ok("a volume we did not format is refused", bb_flush() == false);
    bb_recover();
    ok("and it is not read from either",
       bb_prev(bb_scratch, sizeof(bb_scratch)) == 0);

    memcpy(boot + 3, saved_name, 8);
    blk_write(0, 1, boot);
    blk_flush();
    ok("our own volume is accepted again", bb_flush());

    /* Something else in the reserved sectors, which is what a second stage
       bootloader would look like. The volume still passes every check about
       who formatted it, so this is the only thing standing between the log
       and whatever else came to live there. */
    u8 keep_first[SECTOR_SIZE], intruder[SECTOR_SIZE];
    ok("read the reserved sector", blk_read(BB_LBA, 1, keep_first));
    memset(intruder, 0, sizeof(intruder));
    memcpy(intruder, "NOT A BLACK BOX", 15);
    blk_write(BB_LBA, 1, intruder);
    ok("data we do not recognise is not written over", bb_flush() == false);

    /* And a blank region is fine, which is what a fresh format leaves. */
    memset(intruder, 0, sizeof(intruder));
    blk_write(BB_LBA, 1, intruder);
    ok("a blank region is written to", bb_flush());

    blk_write(BB_LBA, 1, keep_first);
    blk_flush();
}

/* The two ways of reaching configuration space have to agree.
 *
 * This is the check worth having, because the mapped path is arithmetic on a
 * base address and arithmetic is exactly what goes wrong silently: a wrong
 * shift gives you a different device's registers, which read as plausible
 * numbers rather than as an error. The port pair is implemented here rather
 * than called, so the comparison is against something this file computes
 * itself and not against the code being tested. */
static u32 legacy_read32(u8 bus, u8 slot, u8 func, u8 offset) {
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)slot << 11)
             | ((u32)func << 8) | (offset & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void test_pcie(void) {
    const acpi_info_t *a = acpi();
    ok("the acpi tables were found", a->found);
    if (a->found) {
        ok("a directory of tables was read", a->ntables > 0);
        ok("at least one processor is described", a->ncpus > 0);
    }

    if (!pci_ecam_active()) {
        kprintf("  SKIP  no mcfg on this machine, legacy ports only\n");
        /* The fallback still has to behave. Past 256 there is no answer and
           the caller must be told so rather than handed a wrapped offset. */
        ok("extended space reads as absent without a mapping",
           pci_read32(0, 0, 0, 0x100) == 0xFFFFFFFFu);
        return;
    }

    ok("a base address was published", pci_ecam_base() != 0);
    ok("the base is page aligned", (pci_ecam_base() & 0xFFF) == 0);

    /* Walk what is actually on the bus and compare every function both ways.
       Anything present is a real test; an empty slot reads as all ones down
       both paths and proves nothing, so those are counted and not asserted. */
    u32 compared = 0, disagreed = 0;
    for (u16 bus = 0; bus <= pci_ecam_last_bus() && bus < 4; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                u32 viaport = legacy_read32((u8)bus, slot, func, 0x00);
                if ((u16)(viaport & 0xFFFF) == 0xFFFF) continue;

                u32 viamap = pci_read32((u8)bus, slot, func, 0x00);
                u32 cls_p  = legacy_read32((u8)bus, slot, func, 0x08);
                u32 cls_m  = pci_read32((u8)bus, slot, func, 0x08);
                compared++;
                if (viaport != viamap || cls_p != cls_m) disagreed++;
            }
        }
    }
    ok("there was something on the bus to compare", compared > 0);
    ok("both paths report the same ids and classes", disagreed == 0);

    /* The host bridge is function zero of slot zero and is on every machine
       that has a bus at all. Its extended space is only reachable one way. */
    pci_dev_t host;
    bool have_host = pci_find_class(0x06, 0x00, 0x00, &host);
    if (have_host) {
        u32 lo = pci_read32(host.bus, host.slot, host.func, 0x00);
        ok("the bridge answers through the mapping", (u16)(lo & 0xFFFF) != 0xFFFF);

        /* Reading past 256 must not alias back to the start of the function,
           which is what a missing offset in the address arithmetic does. */
        u32 ext = pci_read32(host.bus, host.slot, host.func, 0x100);
        ok("extended space does not alias the header", ext != lo);
    }

    /* An offset nothing can satisfy is refused rather than wrapped. */
    ok("an out of range offset reads as absent",
       pci_read32(0, 0, 0, 0x1000) == 0xFFFFFFFFu);
    ok("an impossible slot reads as absent",
       pci_read32(0, 32, 0, 0x00) == 0xFFFFFFFFu);

    if (a->nmcfg) {
        bool sane = true;
        for (u32 i = 0; i < a->nmcfg; i++)
            if (!a->mcfg[i].base || a->mcfg[i].start_bus > a->mcfg[i].end_bus)
                sane = false;
        ok("every mcfg entry describes a real range", sane);
    }
}

/* Interrupt routing.
 *
 * The thing worth checking is not that the IOAPIC was found, it is that the
 * timer still ticks through it. Almost every machine wires IRQ 0 to input 2
 * rather than input 0, and a kernel that ignores the firmware's list of such
 * moves routes a line nothing is connected to. Nothing reports an error:
 * interrupts are simply never delivered, the scheduler never preempts
 * anything, and the machine appears to hang at whatever it was doing. So the
 * clock is measured rather than assumed.
 */
static void test_irqs(void) {
    if (!ioapic_active()) {
        kprintf("  SKIP  no ioapic on this machine, 8259 only\n");
        ok("the clock still runs on the 8259", timer_hz() > 0);
        return;
    }

    ok("the local apic is up", lapic_present());
    ok("the controller reports its inputs", ioapic_inputs() >= 16);

    /* Every override the firmware listed has to name a real input, or
       routing through it writes to a register that is not there. */
    const acpi_info_t *a = acpi();
    bool sane = true;
    for (u32 i = 0; i < a->noverride; i++)
        if (a->override[i].gsi >= ioapic_inputs()) sane = false;
    ok("every override names an input that exists", sane);

    /* The timer is the one that matters, and it is the one that moves. */
    u32 timer_gsi = ioapic_gsi_for_irq(0);
    ok("the timer's line is known", timer_gsi < ioapic_inputs());
    kprintf("        irq0 arrives on input %d\n", timer_gsi);

    /* And it is ticking. Interrupts are on by now, so a tick count that
       moves is proof that a routed line is delivering to a handler and that
       the handler's acknowledgement is reaching the right controller: miss
       the EOI and exactly one interrupt is ever delivered. */
    u64 before = timer_ticks();
    sleep_ms(60);
    u64 after = timer_ticks();
    ok("the clock advances through the ioapic", after > before);

    /* Masking has to actually stop it, or the mask bit is being written to
       the wrong half of the entry and everything above is a coincidence.
     *
     * Nothing in here may wait on the clock, because the clock is the thing
     * being switched off. sleep_ms waits for a tick count to move and so
     * never returns while the timer is masked, which is a hang rather than
     * a failure. (Which is exactly what the first version of this test did.)
     * So the wait is a spin, and how long to spin is measured against the
     * running clock first rather than guessed at. */
    u32 per_tick = 0;
    u64 mark = timer_ticks();
    while (timer_ticks() == mark) { }            /* to a tick boundary */
    mark = timer_ticks();
    while (timer_ticks() == mark && per_tick < 100000000) per_tick++;
    ok("the spin was calibrated against the clock", per_tick > 0);

    u32 spin = per_tick * 4;                     /* four ticks' worth */

    ioapic_mask_irq(0);
    u64 a1 = timer_ticks();
    for (volatile u32 i = 0; i < spin; i++) { }
    u64 a2 = timer_ticks();
    ioapic_unmask_irq(0);
    ok("masking the timer stops it", a2 == a1);

    u64 b1 = timer_ticks();
    for (volatile u32 i = 0; i < spin * 4 && timer_ticks() == b1; i++) { }
    ok("unmasking starts it again", timer_ticks() > b1);

    /* A line no controller owns is refused rather than written somewhere. */
    ok("an input past the end is refused", ioapic_route_irq(200, 100) == false);
}

/* The clipboard. Small enough that the only things worth checking are the
   edges: that it round trips, that it refuses more than it can hold rather
   than truncating, and that asking with no room answers the length. */
static void test_clipboard(void) {
    clip_init();
    ok("a fresh clipboard is empty", clip_len() == 0);

    const char *msg = "copied between programs";
    ok("text goes in", clip_set(msg, strlen(msg)));
    ok("the length is what went in", clip_len() == strlen(msg));

    char back[64];
    u32 n = clip_get(back, sizeof(back));
    ok("it comes back the same length", n == strlen(msg));
    ok("and the same bytes", strcmp(back, msg) == 0);

    /* A caller sizing a buffer asks with no room at all. */
    ok("asking with no buffer answers the length", clip_len() == strlen(msg));

    /* Short buffers truncate and still terminate, because the alternative
       is a caller reading past the end of its own array. */
    char small[8];
    n = clip_get(small, sizeof(small));
    ok("a short buffer gets what fits", n == sizeof(small) - 1);
    ok("and is still terminated", small[sizeof(small) - 1] == 0);

    /* More than the buffer holds is refused outright. A paste that silently
       loses the end of a file is worse than one that does not happen. */
    static char huge[CLIP_MAX + 16];
    memset(huge, 'x', sizeof(huge));
    ok("more than it can hold is refused", clip_set(huge, sizeof(huge)) == false);
    ok("and what was there is untouched", clip_len() == strlen(msg));

    /* Exactly the maximum is accepted, so the refusal is off by nothing. */
    ok("the largest it can hold is accepted", clip_set(huge, CLIP_MAX - 1));
    ok("at its full length", clip_len() == CLIP_MAX - 1);

    ok("setting it again replaces rather than appends",
       clip_set(msg, strlen(msg)) && clip_len() == strlen(msg));

    u32 g = clip_generation();
    clip_set("again", 5);
    ok("every set is visible to a watcher", clip_generation() == g + 1);

    clip_init();
}

/* The clock.
 *
 * What can be checked without knowing what time it actually is: that the
 * fields are in range, that reading it twice does not go backwards, and that
 * it advances. The last one is the interesting one, because the failure this
 * chip invites is reading during an update and getting a mixture of the old
 * time and the new. That produces a value that is in range, looks entirely
 * reasonable, and is an hour wrong.
 */
static void test_clock(void) {
    if (!rtc_present()) {
        kprintf("  SKIP  no cmos clock on this machine\n");
        return;
    }

    rtc_time_t t;
    ok("the clock reads", rtc_read(&t));
    ok("the month is a month", t.month >= 1 && t.month <= 12);
    ok("the day is a day", t.day >= 1 && t.day <= 31);
    ok("the hour is an hour", t.hour <= 23);
    ok("the minute is a minute", t.minute <= 59);
    ok("the second is a second", t.second <= 59);

    /* Four digits, and this century. A two digit year that was never widened
       reads as 26, which passes every range check above. */
    ok("the year is a four digit year", t.year >= 1970 && t.year < 2200);

    /* Read it again straight away. Nothing should have gone backwards, and a
       read that lands mid update usually does. */
    rtc_time_t again;
    ok("it reads a second time", rtc_read(&again));
    ok("the date did not change under us",
       again.year == t.year && again.month == t.month && again.day == t.day);

    int moved_back = 0;
    if (again.hour < t.hour) moved_back = 1;
    else if (again.hour == t.hour && again.minute < t.minute) moved_back = 1;
    else if (again.hour == t.hour && again.minute == t.minute &&
             again.second < t.second) moved_back = 1;
    ok("time did not run backwards", !moved_back);

    /* And it moves. Waiting for the second to turn over proves the chip is
       running rather than returning one frozen value. */
    u8 started = again.second;
    int changed = 0;
    for (int i = 0; i < 30 && !changed; i++) {
        sleep_ms(100);
        rtc_time_t now;
        if (rtc_read(&now) && now.second != started) changed = 1;
    }
    ok("the clock is running", changed);

    /* Hammer it across several update boundaries.
     *
     * The chip updates in place once a second, and a read taken during that
     * update returns a mixture of the old time and the new: 11:59:59
     * becoming 12:00:00 can be read as 11:00:00, which is an hour wrong and
     * passes every range check above. One read cannot see this, because the
     * window is about two milliseconds in every thousand. Several thousand
     * reads across a few seconds cross it repeatedly, and any torn value
     * shows up as time going backwards.
     *
     * It has never caught anything here and probably cannot: removing the
     * double read entirely still passes, because QEMU updates its emulated
     * chip atomically as far as the guest is concerned and so never produces
     * a torn value at all. The protection in rtc.c is against what the real
     * part does, and this check is left in to catch a regression on hardware
     * that does it. Treat it as unverified rather than as passing. */
    rtc_time_t prev;
    int back = 0, reads = 0;
    if (rtc_read(&prev)) {
        for (int i = 0; i < 4000; i++) {
            rtc_time_t now;
            if (!rtc_read(&now)) continue;
            reads++;
            u32 a = (u32)prev.hour * 3600 + (u32)prev.minute * 60 + prev.second;
            u32 b = (u32)now.hour * 3600 + (u32)now.minute * 60 + now.second;
            /* Midnight is the one legitimate way round. */
            if (b + 60 < a && !(prev.hour == 23 && now.hour == 0)) back++;
            prev = now;
        }
    }
    ok("several thousand reads were taken", reads > 1000);
    ok("none of them read a time earlier than the one before", back == 0);

    char text[24];
    rtc_format(text, sizeof(text));
    ok("it formats to the full width", strlen(text) == 19);
    ok("with dashes where a date has them", text[4] == '-' && text[7] == '-');
    ok("and colons where a time has them", text[13] == ':' && text[16] == ':');

    char shortform[8];
    rtc_format_short(shortform, sizeof(shortform));
    ok("the short form is hh:mm", strlen(shortform) == 5 && shortform[2] == ':');

    /* A buffer too small must be refused rather than written past. */
    char tiny[4];
    tiny[3] = '#';
    rtc_format(tiny, 3);
    ok("a buffer too small is not written past", tiny[3] == '#');
}

int selftest_run(void) {
    passed = failed = 0;
    kprintf("\n=== zelr self test ===\n");
    kprintf("[string]\n");     test_string();
    kprintf("[the identity map]\n"); test_identity_map();
    kprintf("[physical memory]\n"); test_pmm();
    kprintf("[paging]\n");     test_paging();
    kprintf("[user access]\n"); test_user_access();
    kprintf("[heap]\n");       test_heap();
    kprintf("[filesystem]\n"); test_fs();
    kprintf("[paths]\n");      test_paths();
    kprintf("[directories]\n"); test_directories();
    kprintf("[open files]\n");  test_open_files();
    kprintf("[timer]\n");      test_timer();
    kprintf("[interrupts]\n"); test_interrupts();
    kprintf("[disk]\n");       test_disk();
    kprintf("[fat]\n");        test_fat();
    kprintf("[network]\n");    test_net();
    kprintf("[elf]\n");        test_elf();
    kprintf("[userspace]\n");  test_userspace();
    kprintf("[video]\n");      test_video();
    kprintf("[mouse]\n");      test_mouse();
    kprintf("[graphics]\n");   test_gfx();
    kprintf("[windows]\n");    test_wm();
    kprintf("[window server]\n"); test_winsrv();
    kprintf("[built-in programs]\n"); test_builtin();
    kprintf("[theme]\n");      test_theme();
    kprintf("[taskbar]\n");    test_pins();
    kprintf("[live tree]\n"); test_live_tree();
    kprintf("[layout]\n");    test_layout();
    kprintf("[waiting]\n");    test_waiting();
    test_idle_accounting();
    kprintf("[trackpad]\n");   test_trackpad();
    kprintf("[wait timeouts]\n"); test_wait_timeout();
    kprintf("[processors]\n"); test_smp();
    kprintf("[black box]\n"); test_blackbox();
    kprintf("[acpi and pcie]\n"); test_pcie();
    kprintf("[interrupt routing]\n"); test_irqs();
    kprintf("[clipboard]\n"); test_clipboard();
    kprintf("[clock]\n"); test_clock();
    kprintf("\n%d passed, %d failed\n", passed, failed);
    kprintf(failed ? "SELFTEST_FAIL\n" : "SELFTEST_PASS\n");
    return failed;
}
