/* See include/blackbox.h for what this is for.
 *
 * The only subtle part is the refusal to write. A black box is worth having
 * because it writes to the disk, and writing to the disk is exactly what you
 * must not do on a machine you were only asked to boot on. Somebody running
 * this off a USB stick on their own laptop has a partition table and a
 * filesystem at the front of their internal drive, and putting a log there
 * would destroy it.
 *
 * So bb_flush checks four things before it touches a sector, and every one of
 * them has to be a volume this kernel made: the boot signature, the OEM name
 * fat_format writes, the volume serial it writes, and enough reserved
 * sectors to hold the log. Three of those would be coincidence. Four is not.
 * A disk that fails any of them gets nothing, and the serial and screen
 * routes still work, so the refusal costs only the one output. */
#include "blackbox.h"
#include "blockdev.h"
#include "serial.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "vga.h"
#include "fb.h"
#include "gfx.h"
#include "fat.h"

/* On disk, and in memory, the same layout: the header is written as part of
   the first sector, so a read gives back everything in one go. */
typedef struct {
    u32  magic;
    u32  boot;                  /* which boot produced this */
    u32  len;                   /* bytes of text */
    u32  sum;                   /* of the text, so a torn write is not trusted */
    char build[32];
} bb_head_t;

#define BB_TEXT_MAX (BB_BYTES - sizeof(bb_head_t))

/* BSS, not the heap: this has to work before heap_init runs. */
static u8   image[BB_BYTES];
static u32  len;
static u32  boot_number;
static bool wrapped;

static char *text(void) { return (char *)(image + sizeof(bb_head_t)); }

static u32 checksum(const char *p, u32 n) {
    u32 h = 2166136261u;                 /* FNV-1a, for spotting a torn write */
    for (u32 i = 0; i < n; i++) { h ^= (u8)p[i]; h *= 16777619u; }
    return h;
}

/* --- writing --------------------------------------------------------------
 *
 * When the buffer fills, the front goes. What killed the machine is at the
 * end, and the first thing anyone reads is the last thing that happened. */

static void drop_oldest_half(void) {
    u32 keep = BB_TEXT_MAX / 2;
    char *t = text();
    /* Start at a line boundary so the log does not resume mid-sentence. */
    u32 from = len - keep;
    while (from < len && t[from] != '\n') from++;
    if (from < len) from++;
    memmove(t, t + from, len - from);
    len = len - from;
    wrapped = true;
}

static void append(const char *s) {
    char *t = text();
    for (; *s; s++) {
        if (len + 1 >= BB_TEXT_MAX) drop_oldest_half();
        t[len++] = *s;
        serial_putc(*s);
    }
    t[len] = 0;
}

/* Milliseconds since the timer started, which reads as zero until it does.
   That is useful rather than a defect: a line stamped zero happened before
   the timer, which places it in the boot without needing anything else.
   After that the numbers are what makes a slow probe visible, and a probe
   that takes thirty seconds on real hardware looks exactly like a hang. */
static void stamp(void) {
    u32 hz = timer_hz();
    u32 ms = hz ? (u32)(timer_ticks() * 1000 / hz) : 0;
    char b[24];
    kformat(b, sizeof(b), "[%5d] ", ms);
    append(b);
}

void bb_init(void) {
    memset(image, 0, sizeof(image));
    len = 0;
    wrapped = false;
    boot_number = 0;
    append("nyx ");
    append(KERNEL_VERSION);
    append(" boot log\n");
}

void bb_log(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    kvformat(line, sizeof(line), fmt, ap);
    va_end(ap);
    stamp();
    append(line);
    append("\n");
}

void bb_mark(const char *phase) {
    stamp();
    append("== ");
    append(phase);
    append("\n");
}

void bb_fault(registers_t *r, const char *what) {
    u64 cr2 = 0, cr3 = 0;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

    bb_log("!! %s", what);
    bb_log("   vec=%d err=%x rip=%p cs=%x flags=%x",
           (u32)r->int_no, (u32)r->err_code, (void *)r->rip,
           (u32)r->cs, (u32)r->rflags);
    bb_log("   rsp=%p ss=%x cr2=%p cr3=%p",
           (void *)r->rsp, (u32)r->ss, (void *)cr2, (void *)cr3);
    bb_log("   rax=%p rbx=%p rcx=%p rdx=%p",
           (void *)r->rax, (void *)r->rbx, (void *)r->rcx, (void *)r->rdx);
    bb_log("   rsi=%p rdi=%p rbp=%p r8 =%p",
           (void *)r->rsi, (void *)r->rdi, (void *)r->rbp, (void *)r->r8);
}

const char *bb_text(void) { return text(); }
u32         bb_len(void)  { return len; }
u32 bb_boot_number(void)  { return boot_number; }

/* --- getting it off the machine ------------------------------------------ */

/* How many lines fit, and where they start, for whichever display exists. */
static void screen_lines(const char **start, u32 *count, u32 rows) {
    const char *t = text();
    u32 lines = 1;
    for (u32 i = 0; i < len; i++) if (t[i] == '\n') lines++;

    u32 skip = lines > rows ? lines - rows : 0;
    const char *p = t;
    for (u32 i = 0; i < skip; i++) { while (*p && *p != '\n') p++; if (*p) p++; }
    *start = p;
    *count = lines - skip;
}

/* Every line starts "[ticks] ", so what the line is saying begins after
   that. Worth picking out: a fault should be findable in a photograph of a
   screen without reading all sixty rows. */
static u32 line_colour(const char *line) {
    const char *m = line;
    if (*m == '[') { while (*m && *m != ']') m++; if (*m) m++; while (*m == ' ') m++; }
    if (m[0] == '!' && m[1] == '!') return 0xFF6060;   /* the fault */
    if (m[0] == '=' && m[1] == '=') return 0x80D0FF;   /* a phase */
    if (m[0] == ' ') return 0xFF9090;                  /* a fault's registers */
    return 0xC8C8C8;
}

void bb_screen(void) {
    if (fb_active()) {
        u32 w = fb_width(), h = fb_height();
        u32 rows = (h - 40) / 16;
        if (rows > 60) rows = 60;

        fb_clear(0x120008);
        fb_rect(0, 0, w, 22, 0xA01020);
        gfx_text(8, 4, "nyx stopped. this is the boot log.", 0xFFFFFF);

        const char *p; u32 n;
        screen_lines(&p, &n, rows);

        char line[128];
        for (u32 row = 0; row < n; row++) {
            u32 i = 0;
            while (*p && *p != '\n' && i < sizeof(line) - 1) line[i++] = *p++;
            line[i] = 0;
            if (*p) p++;
            gfx_text(8, (int)(28 + row * 16), line, line_colour(line));
        }
        fb_flush();
        return;
    }

    /* No framebuffer, so the text console is all there is. It holds 25 rows
       and the last of them is what matters. */
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_clear();
    const char *p; u32 n;
    screen_lines(&p, &n, 23);
    vga_write("nyx stopped. boot log:\n");
    for (u32 row = 0; row < n; row++) {
        while (*p && *p != '\n') vga_putc(*p++);
        vga_putc('\n');
        if (*p) p++;
    }
}

/* --- the disk, carefully -------------------------------------------------- */

/* Where the volume this belongs to starts.
 *
 * On a disk image the filesystem is the disk and this is zero, which is what
 * it always was. On a partitioned disk it is the start of whichever
 * partition got mounted, and that matters more than it looks: sector 1 of a
 * GPT disk is the GPT header itself. Writing a boot log over a partition
 * table would be a spectacular way to destroy a machine, and while the
 * checks below already refuse it (a protective MBR carries neither the name
 * nor the serial fat_format writes), refusing by luck is not the same as
 * addressing the right sector. */
static u32 volume_base(void) {
    return fat_mounted() ? fat_base() : 0;
}

/* True only for a volume fat_format made, with room reserved for the log.
   Anything else is somebody's real disk and is not written to. */
static bool volume_is_ours(u8 *sec) {
    if (!blk_present()) return false;
    if (!blk_read(volume_base(), 1, sec)) return false;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return false;
    if (memcmp(sec + 3, "NYX     ", 8) != 0) return false;
    if (*(u32 *)(sec + 39) != 0x4E595800u) return false;
    u16 reserved = *(u16 *)(sec + 14);
    if (reserved < BB_LBA + BB_SECTORS) return false;
    return true;
}

/* And the sectors themselves have to be either blank or a log already.
 *
 * The four checks above say the volume came from this kernel. They do not
 * say that nothing else has since been put where the log goes, and the
 * obvious thing to put in a volume's reserved sectors is a second stage
 * bootloader. Nothing does that today. Rather than leave that as something
 * to remember later, the rule here is that unrecognised data is never
 * written over, which holds whatever else the reserved area grows into. */
static bool region_is_free(void) {
    u8 first[512];
    if (!blk_read(volume_base() + BB_LBA, 1, first)) return false;
    if (*(u32 *)first == BB_MAGIC) return true;
    for (u32 i = 0; i < sizeof(first); i++)
        if (first[i]) return false;
    return true;
}

/* One sector of scratch, static because the fault path has no stack to spare
   and no allocator to ask. */
static u8 scratch[512];

bool bb_flush(void) {
    if (!volume_is_ours(scratch)) return false;
    if (!region_is_free()) return false;

    /* The count comes off the previous record, so it survives a reboot. */
    bb_head_t prev;
    u32 n = 0;
    if (blk_read(volume_base() + BB_LBA, 1, scratch)) {
        memcpy(&prev, scratch, sizeof(prev));
        if (prev.magic == BB_MAGIC) n = prev.boot;
    }
    boot_number = n + 1;

    bb_head_t *h = (bb_head_t *)image;
    h->magic = BB_MAGIC;
    h->boot  = boot_number;
    h->len   = len;
    h->sum   = checksum(text(), len);
    kformat(h->build, sizeof(h->build), "%s", KERNEL_VERSION);

    u32 used = (u32)sizeof(bb_head_t) + len;
    u32 secs = (used + 511) / 512;
    if (secs > BB_SECTORS) secs = BB_SECTORS;

    if (!blk_write(volume_base() + BB_LBA, secs, image)) return false;
    blk_flush();
    return true;
}

/* The previous boot's record, held in memory because the sectors it came
   from are about to be written over by this one. Reading it lazily instead
   would mean that rebooting to find out why the machine failed is the act
   that destroys the answer. */
static u8   saved[BB_BYTES];
static u32  saved_len;
static bool saved_valid;

void bb_recover(void) {
    saved_valid = false;
    saved_len = 0;

    if (!volume_is_ours(scratch)) return;
    if (!blk_read(volume_base() + BB_LBA, BB_SECTORS, saved)) return;

    bb_head_t *h = (bb_head_t *)saved;
    if (h->magic != BB_MAGIC) return;
    if (h->len > BB_TEXT_MAX) return;

    char *t = (char *)(saved + sizeof(bb_head_t));
    if (checksum(t, h->len) != h->sum) return;   /* torn, or never finished */

    saved_len = h->len;
    saved_valid = true;
    bb_log("black box: recovered boot %d, %d bytes", h->boot, h->len);
}

u32 bb_prev(char *out, u32 cap) {
    if (!out || cap == 0 || !saved_valid) return 0;
    u32 n = saved_len < cap - 1 ? saved_len : cap - 1;
    memcpy(out, saved + sizeof(bb_head_t), n);
    out[n] = 0;
    return n;
}
