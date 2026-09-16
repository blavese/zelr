/* See include/usb.h.
 *
 * What a device is asked, in order, and why:
 *
 *   the device descriptor, first eight bytes    to learn how big its control
 *                                               transfers may be, which is
 *                                               needed before a longer one
 *                                               can be asked for at all
 *   the configuration descriptor, first nine    to learn how long the whole
 *                                               thing is
 *   the configuration descriptor, all of it     the interfaces and their
 *                                               endpoints are inside it
 *   set configuration                           nothing works before this
 *   set protocol, boot                          so the reports are the fixed
 *                                               shape this file understands
 *   set idle, never                             so a key held down does not
 *                                               arrive over and over
 *
 * and then one interrupt endpoint is opened and a buffer handed to it. Every
 * time that comes back it is decoded and handed to the same ring buffer the
 * PS/2 keyboard fills, so nothing above here knows the difference. */
#include "usb.h"
#include "xhci.h"
#include "keyboard.h"
#include "mouse.h"
#include "heap.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "usbdisk.h"
#include "diskfs.h"
#include "sched.h"
#include "io.h"
#include "blackbox.h"

/* --- the requests ---------------------------------------------------------- */
#define REQ_GET_DESCRIPTOR  6
#define REQ_SET_CONFIG      9
#define REQ_SET_IDLE        0x0A
#define REQ_SET_PROTOCOL    0x0B

#define DESC_DEVICE     1
#define DESC_CONFIG     2
#define DESC_INTERFACE  4
#define DESC_ENDPOINT   5

#define CLASS_HID       3
#define CLASS_HUB       9
/* Mass storage, speaking SCSI, over two bulk endpoints. Every stick is this
   and almost nothing else: the other transports in the specification were
   for floppy drives. */
#define CLASS_MSC       8
#define SUB_SCSI        6
#define PROTO_BBB       0x50
#define SUB_BOOT        1
#define PROTO_KEYBOARD  1
#define PROTO_MOUSE     2

typedef struct {
    u8  length;
    u8  type;
    u16 usb;
    u8  device_class;
    u8  device_subclass;
    u8  device_protocol;
    u8  max_packet0;
    u16 vendor;
    u16 product;
} __attribute__((packed)) device_desc_t;

typedef struct {
    u8  length;
    u8  type;
    u16 total_length;
    u8  interfaces;
    u8  value;
} __attribute__((packed)) config_desc_t;

typedef struct {
    u8 length;
    u8 type;
    u8 number;
    u8 alternate;
    u8 endpoints;
    u8 iclass;
    u8 subclass;
    u8 protocol;
} __attribute__((packed)) interface_desc_t;

typedef struct {
    u8  length;
    u8  type;
    u8  address;
    u8  attributes;
    u16 max_packet;
    u8  interval;
} __attribute__((packed)) endpoint_desc_t;

/* --- what has been found --------------------------------------------------- */
#define MAX_DEVICES 8
#define REPORT_MAX  16

typedef struct {
    bool used;
    bool keyboard;              /* otherwise a mouse */
    u8   slot;
    u8   dci;
    u32  root;                  /* the root port its tree hangs off */
    u16  length;                /* how much of the report to expect */
    u8  *buf;                   /* the controller writes into this */
    u8   last[REPORT_MAX];      /* the previous one, to tell presses apart */
} device_t;

static device_t devices[MAX_DEVICES];
static u32 nkeyboards, nmice, nhubs, ndisks;
static volatile u32 nreports;
static bool started;
static char description[128];

/* --- the boot keyboard's numbering ------------------------------------------ */
/*
 * A USB keyboard does not send scancodes. It sends usage identifiers, which
 * are the same on every keyboard in the world regardless of what is printed
 * on the keys, and are nothing like the PS/2 set. 0x04 is the key where a
 * keyboard sold in an English speaking country has A on it.
 */
static const char PLAIN[] = {
    /* 0x00 */ 0, 0, 0, 0,
    /* 0x04 */ 'a','b','c','d','e','f','g','h','i','j','k','l','m',
               'n','o','p','q','r','s','t','u','v','w','x','y','z',
    /* 0x1E */ '1','2','3','4','5','6','7','8','9','0',
    /* 0x28 */ '\n', 27, '\b', '\t', ' ',
    /* 0x2D */ '-','=','[',']','\\',
    /* 0x32 */ 0,                     /* the non-US key next to Enter */
    /* 0x33 */ ';','\'','`',',','.','/',
};

static const char SHIFTED[] = {
    0, 0, 0, 0,
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n', 27, '\b', '\t', ' ',
    '_','+','{','}','|',
    0,
    ':','"','~','<','>','?',
};

/* The keys that are not characters, which is why they have numbers of their
   own above anything a byte can hold. */
static int special(u8 usage) {
    switch (usage) {
    case 0x4F: return KEY_RIGHT;
    case 0x50: return KEY_LEFT;
    case 0x51: return KEY_DOWN;
    case 0x52: return KEY_UP;
    case 0x4A: return KEY_HOME;
    case 0x4D: return KEY_END;
    case 0x4B: return KEY_PAGE_UP;
    case 0x4E: return KEY_PAGE_DOWN;
    case 0x4C: return KEY_DELETE;
    case 0x49: return KEY_INSERT;
    default: break;
    }
    if (usage >= 0x3A && usage <= 0x45) return KEY_F1 + (usage - 0x3A);
    return 0;
}

static int translate(u8 usage, bool shift) {
    int key = special(usage);
    if (key) return key;
    if (usage < sizeof PLAIN) {
        char c = shift ? SHIFTED[usage] : PLAIN[usage];
        if (c) return (int)(u8)c;
    }
    return 0;
}

/* --- a report arrived -------------------------------------------------------- */
static void on_keyboard(device_t *d, u32 len) {
    if (len < 3) return;
    u8 mods = d->buf[0];
    bool shift = (mods & 0x22) != 0;
    bool ctrl  = (mods & 0x11) != 0;
    bool alt   = (mods & 0x44) != 0;

    u32 stamp = 0;
    if (alt) stamp |= KEY_MOD_ALT;
    if (ctrl) stamp |= KEY_MOD_CTRL;
    if (shift) stamp |= KEY_MOD_SHIFT;

    /* The report is the list of keys held right now, not what changed. A key
       is a press if it is in this one and was not in the last, which is also
       what makes holding one down send it once rather than sixty times a
       second. */
    for (u32 i = 2; i < len && i < REPORT_MAX; i++) {
        u8 usage = d->buf[i];
        if (usage < 4) continue;              /* 1..3 are error markers */

        bool was_held = false;
        for (u32 j = 2; j < REPORT_MAX; j++)
            if (d->last[j] == usage) { was_held = true; break; }
        if (was_held) continue;

        int key = translate(usage, shift);
        if (!key) continue;
        /* Control folds a letter to the character it names, the same way the
           PS/2 driver does it, because that is what a program comparing a
           key against 3 for copy is expecting to see. */
        if (ctrl && key >= 'a' && key <= 'z') key = key - 'a' + 1;
        else if (ctrl && key >= 'A' && key <= 'Z') key = key - 'A' + 1;
        keyboard_inject(key | stamp);
    }

    keyboard_set_mods(alt, ctrl, shift);
    /* Cleared first, not just overwritten: a report shorter than the last
       one would otherwise leave the tail of the old one behind, and a key
       that is still sitting in that tail reads as being held down forever,
       so it never registers as pressed again. */
    memset(d->last, 0, sizeof d->last);
    memcpy(d->last, d->buf, len < REPORT_MAX ? len : REPORT_MAX);
}

static void on_mouse(device_t *d, u32 len) {
    if (len < 3) return;
    /* Buttons in the first byte, then two signed bytes of movement. The
       vertical one counts the same way a PS/2 mouse does, upward, and the
       screen counts downward, so the mouse driver is handed it the way it
       already expects. */
    u8 buttons = d->buf[0] & 0x07;
    i32 dx = (i8)d->buf[1];
    i32 dy = (i8)d->buf[2];
    mouse_inject(dx, -dy, buttons);
}

static void on_report(u8 slot, u8 dci, u32 residual) {
    for (u32 i = 0; i < MAX_DEVICES; i++) {
        device_t *d = &devices[i];
        if (!d->used || d->slot != slot || d->dci != dci) continue;

        /* The controller reports how much of the buffer it did *not* fill. */
        u32 len = d->length > residual ? d->length - residual : 0;
        nreports++;
        if (d->keyboard) on_keyboard(d, len);
        else             on_mouse(d, len);

        /* And it is handed back immediately, because an endpoint with
           nothing queued on it is one the device cannot report to: the next
           key press would be silently dropped. */
        xhci_listen(slot, dci, d->buf, d->length);
        return;
    }
}

/* --- asking a device about itself --------------------------------------------- */
/*
 * Answers land here first and are copied out afterwards. The controller is
 * given a physical address, and the only memory this kernel can hand it
 * without translating is the heap, which is identity mapped. A descriptor
 * read straight into a local would be asking it to write to a stack address,
 * and whether that happens to be the same number is not something worth
 * depending on.
 */
static u8 *bounce;
#define BOUNCE_MAX 512

/* Every request in this file goes through here: the eight bytes saying what
   is being asked, and a buffer for whatever comes back. */
static bool request(u8 slot, u8 type, u8 req, u16 value, u16 index,
                    void *out, u16 len) {
    if (len > BOUNCE_MAX) return false;
    if (len && !bounce) return false;
    if (len) memset(bounce, 0, len);
    usb_setup_t s = { type, req, value, index, len };
    if (!xhci_control(slot, &s, len ? bounce : 0, len)) return false;
    if (len && out) memcpy(out, bounce, len);
    return true;
}

static bool get_descriptor(u8 slot, u8 type, u8 index, void *out, u16 len) {
    return request(slot, 0x80, REQ_GET_DESCRIPTOR,
                   (u16)((u16)type << 8 | index), 0, out, len);
}

/* Waiting, in two very different worlds.
 *
 * Enumeration runs twice: once from main, before interrupts have ever been
 * switched on, and again from a task every time something is plugged in.
 * task_sleep falls back to halting until a timer interrupt when there is no
 * task, and in the first of those worlds that interrupt cannot arrive, so
 * the machine would stop there and never finish booting. A write to a port
 * that goes nowhere takes about a microsecond and works in both. */
static void wait_ms(u32 ms) {
    if (task_current()) { task_sleep(ms); return; }
    while (ms--) for (u32 i = 0; i < 1000; i++) io_wait();
}

static bool set_configuration(u8 slot, u8 value) {
    return request(slot, 0x00, REQ_SET_CONFIG, value, 0, 0, 0);
}

static bool set_boot_protocol(u8 slot, u8 interface) {
    return request(slot, 0x21, REQ_SET_PROTOCOL, 0, interface, 0, 0);
}

static bool set_idle(u8 slot, u8 interface) {
    /* Zero means report only when something changes. Without it a keyboard
       with a key held down repeats it as fast as its interval allows. */
    return request(slot, 0x21, REQ_SET_IDLE, 0, interface, 0, 0);
}

/* --- hubs --------------------------------------------------------------- */
/*
 * A hub is an ordinary USB device that happens to have ports. It is asked
 * about itself the same way as anything else, and then each of its ports is
 * powered, looked at and reset, which is exactly what the controller does for
 * its own root ports. What comes out the other side is another device to
 * enumerate, and sometimes another hub.
 *
 * This matters more on a laptop than it looks. A built-in keyboard and
 * touchpad are often behind a hub that is part of the chipset rather than
 * plugged into a port anybody can see, so a driver that walks only root ports
 * finds an empty machine and reports, correctly and uselessly, that there is
 * no keyboard.
 */
#define DESC_HUB        0x29
#define DESC_HUB_SS     0x2A

typedef struct {
    u8  length;
    u8  type;
    u8  ports;
    u16 characteristics;
    u8  power_on_delay;         /* in units of 2 ms */
    u8  current_ma;
} __attribute__((packed)) hub_desc_t;

/* What a port says about itself. */
#define PS_CONNECTED    0x0001
#define PS_ENABLED      0x0002
#define PS_RESETTING    0x0010
#define PS_LOW_SPEED    0x0200
#define PS_HIGH_SPEED   0x0400

/* And what can be asked of it. */
#define FEAT_PORT_RESET         4
#define FEAT_PORT_POWER         8
#define FEAT_C_PORT_CONNECTION  16
#define FEAT_C_PORT_RESET       20

static bool hub_descriptor(u8 slot, bool super, hub_desc_t *out) {
    u16 type = super ? DESC_HUB_SS : DESC_HUB;
    return request(slot, 0xA0, REQ_GET_DESCRIPTOR, (u16)(type << 8), 0,
                   out, sizeof *out);
}

static bool port_feature(u8 slot, u8 port, u16 feature, bool set) {
    return request(slot, 0x23, set ? 3 : 1, feature, port, 0, 0);
}

static bool port_status(u8 slot, u8 port, u16 *status, u16 *change) {
    u8 b[4] = { 0, 0, 0, 0 };
    if (!request(slot, 0xA3, 0, 0, port, b, 4)) return false;
    *status = (u16)(b[0] | (b[1] << 8));
    *change = (u16)(b[2] | (b[3] << 8));
    return true;
}

/* Brings one downstream port up, and says what speed answered. */
static xhci_speed_t reset_hub_port(u8 slot, u8 port) {
    u16 status = 0, change = 0;
    if (!port_status(slot, port, &status, &change)) return XHCI_SPEED_NONE;
    if (!(status & PS_CONNECTED)) return XHCI_SPEED_NONE;

    if (!port_feature(slot, port, FEAT_PORT_RESET, true))
        return XHCI_SPEED_NONE;

    /* A reset takes tens of milliseconds and the hub says when it is done by
       dropping the bit. Twenty tries at ten milliseconds is comfortably more
       than the specification allows it to take. */
    for (int i = 0; i < 20; i++) {
        wait_ms(10);
        if (!port_status(slot, port, &status, &change)) return XHCI_SPEED_NONE;
        if (!(status & PS_RESETTING) && (status & PS_ENABLED)) break;
    }
    if (!(status & PS_ENABLED)) return XHCI_SPEED_NONE;

    /* Acknowledged, or the hub keeps reporting the same change forever and
       the port reads as though it had only just been plugged in. */
    port_feature(slot, port, FEAT_C_PORT_RESET, false);
    port_feature(slot, port, FEAT_C_PORT_CONNECTION, false);

    if (status & PS_LOW_SPEED)  return XHCI_SPEED_LOW;
    if (status & PS_HIGH_SPEED) return XHCI_SPEED_HIGH;
    return XHCI_SPEED_FULL;
}

/* --- setting one up ------------------------------------------------------------ */
static device_t *free_device(void) {
    for (u32 i = 0; i < MAX_DEVICES; i++)
        if (!devices[i].used) return &devices[i];
    return 0;
}

/* Walks the configuration descriptor looking for a boot keyboard or mouse,
   and the interrupt-in endpoint that goes with it. Everything in there is a
   run of records, each starting with its own length, so walking it means
   stepping by that length and never by the size of the structure being
   looked for: there are records in here this file has never heard of and
   stepping over them correctly is the whole trick. */
static bool claim_interface(u8 slot, u32 root_port,
                            const u8 *cfg, u16 total) {
    const interface_desc_t *want = 0;
    const interface_desc_t *disk = 0;
    u8  bulk_in = 0, bulk_out = 0;
    u16 bulk_mps = 0;
    u16 at = 0;

    while (at + 2 <= total) {
        const u8 *rec = cfg + at;
        u8 len = rec[0];
        u8 type = rec[1];
        if (len < 2) break;

        if (type == DESC_INTERFACE && len >= sizeof(interface_desc_t)) {
            const interface_desc_t *i = (const interface_desc_t *)rec;
            want = (i->iclass == CLASS_HID && i->subclass == SUB_BOOT
                    && (i->protocol == PROTO_KEYBOARD
                        || i->protocol == PROTO_MOUSE)) ? i : 0;
            disk = (i->iclass == CLASS_MSC && i->subclass == SUB_SCSI
                    && i->protocol == PROTO_BBB) ? i : 0;
        } else if (type == DESC_ENDPOINT && disk
                   && len >= sizeof(endpoint_desc_t)) {
            /* Two bulk endpoints, one each way. Collected rather than acted
               on, because both are needed before either is any use. */
            const endpoint_desc_t *e = (const endpoint_desc_t *)rec;
            if ((e->attributes & 0x03) == 2) {
                bool in = (e->address & 0x80) != 0;
                u8 number = e->address & 0x0F;
                u8 dci = (u8)(number * 2 + (in ? 1 : 0));
                if (in) bulk_in = dci; else bulk_out = dci;
                u16 mps = e->max_packet & 0x7FF;
                if (mps) bulk_mps = mps;
            }
        } else if (type == DESC_ENDPOINT && want
                   && len >= sizeof(endpoint_desc_t)) {
            const endpoint_desc_t *e = (const endpoint_desc_t *)rec;
            bool in = (e->address & 0x80) != 0;
            bool interrupt = (e->attributes & 0x03) == 3;
            if (in && interrupt) {
                device_t *d = free_device();
                if (!d) return false;

                u8 number = e->address & 0x0F;
                u8 dci = (u8)(number * 2 + 1);
                u16 mps = e->max_packet & 0x7FF;
                u8 interval = e->interval ? e->interval : 1;

                if (!xhci_open_interrupt_in(slot, dci, mps, interval))
                    return false;

                d->buf = (u8 *)kmalloc(REPORT_MAX * 2);
                if (!d->buf) return false;
                memset(d->buf, 0, REPORT_MAX * 2);
                memset(d->last, 0, sizeof d->last);

                d->used = true;
                d->keyboard = (want->protocol == PROTO_KEYBOARD);
                d->slot = slot;
                d->dci = dci;
                d->root = root_port;
                d->length = mps < REPORT_MAX ? mps : REPORT_MAX;

                set_boot_protocol(slot, want->number);
                set_idle(slot, want->number);

                if (d->keyboard) nkeyboards++; else nmice++;
                bb_log("usb %s on slot %d ep %d",
                       d->keyboard ? "keyboard" : "mouse", slot, dci);
                xhci_listen(slot, dci, d->buf, d->length);
                return true;
            }
        }
        at = (u16)(at + len);
    }

    /* A stick, if both halves of it turned up.
     *
     * The lower numbered endpoint is opened first. Each Configure Endpoint
     * says how far down the context array the controller should look, and
     * opening the higher one second means that number only ever rises. */
    if (bulk_in && bulk_out && bulk_mps) {
        u8 lo = bulk_in < bulk_out ? bulk_in : bulk_out;
        u8 hi = bulk_in < bulk_out ? bulk_out : bulk_in;
        u8 lo_kind = (lo == bulk_in) ? XHCI_EP_BULK_IN : XHCI_EP_BULK_OUT;
        u8 hi_kind = (hi == bulk_in) ? XHCI_EP_BULK_IN : XHCI_EP_BULK_OUT;

        if (!xhci_open_endpoint(slot, lo, lo_kind, bulk_mps, 0)) return false;
        if (!xhci_open_endpoint(slot, hi, hi_kind, bulk_mps, 0)) return false;

        if (!usbdisk_attach(slot, bulk_in, bulk_out)) return false;
        ndisks++;
        bb_log("usb disk on slot %d, in ep %d out ep %d", slot, bulk_in, bulk_out);

        /* And mounted, if there is anything on it to mount. A stick that is
           blank, or formatted as something this kernel does not read, is
           still a disk: it just has no files under /usb. */
        diskfs_mount_removable(usbdisk_blk_id());
        return true;
    }

    return false;
}

/* How far down a chain of hubs to follow. USB allows five tiers below the
   root, which is also as many as the route string has room for: four bits
   each and twenty bits of it. */
#define MAX_TIER 5

static void enumerate(const xhci_where_t *where, u8 tier);

/* Everything plugged into one hub, and everything plugged into those. */
static void walk_hub(u8 slot, const xhci_where_t *hub, u8 tier,
                     xhci_speed_t hub_speed) {
    if (tier >= MAX_TIER) return;

    hub_desc_t hd;
    memset(&hd, 0, sizeof hd);
    if (!hub_descriptor(slot, hub_speed == XHCI_SPEED_SUPER, &hd)) return;
    if (!hd.ports || hd.ports > 15) return;

    /* The controller will not route anything to a device behind this until
       it knows it is a hub and how many ports it has. */
    u8 think_time = (u8)((hd.characteristics >> 5) & 3);
    if (!xhci_mark_hub(slot, hd.ports, think_time, false)) return;

    for (u8 p = 1; p <= hd.ports; p++)
        port_feature(slot, p, FEAT_PORT_POWER, true);

    /* The hub says how long its ports take to come up, in units of two
       milliseconds, and a port read before then reads as empty. */
    wait_ms((u32)hd.power_on_delay * 2u + 20u);

    for (u8 p = 1; p <= hd.ports; p++) {
        xhci_speed_t speed = reset_hub_port(slot, p);
        if (speed == XHCI_SPEED_NONE) continue;

        xhci_where_t child = *hub;
        child.speed = speed;
        /* The path, one nibble per tier, this hub's port in this tier's. */
        child.route = hub->route | ((u32)(p & 0xF) << (4 * tier));

        /* A slow device behind a fast hub is reached by the controller
           talking to the hub at the fast speed and letting the hub do the
           slow part. It has to be told which hub, and which of its ports. */
        if (hub_speed == XHCI_SPEED_HIGH
            && (speed == XHCI_SPEED_LOW || speed == XHCI_SPEED_FULL)) {
            child.tt_slot = slot;
            child.tt_port = p;
        }
        enumerate(&child, (u8)(tier + 1));
    }
}

/* One device, wherever it is in the tree. */
static void enumerate(const xhci_where_t *where, u8 tier) {
    u8 slot = xhci_attach(where);
    if (!slot) return;

    /* Eight bytes first, because until the device has said how big its
       control transfers may be, eight is the only size that is safe to ask
       for on every speed there is. */
    device_desc_t dev;
    memset(&dev, 0, sizeof dev);
    if (!get_descriptor(slot, DESC_DEVICE, 0, &dev, 8)) return;

    /* Now that it has said, tell the controller, or every descriptor read
       after this one comes back cut short at eight bytes. */
    if (dev.max_packet0 && dev.max_packet0 != 8)
        xhci_set_packet_size(slot, dev.max_packet0);

    config_desc_t head;
    memset(&head, 0, sizeof head);
    if (!get_descriptor(slot, DESC_CONFIG, 0, &head, sizeof head)) return;
    u16 total = head.total_length;
    if (!total || total > BOUNCE_MAX) return;

    u8 *cfg = (u8 *)kmalloc(total);
    if (!cfg) return;
    memset(cfg, 0, total);
    if (get_descriptor(slot, DESC_CONFIG, 0, cfg, total)
        && set_configuration(slot, head.value)) {
        /* A hub says so in its device descriptor rather than in one of its
           interfaces, which is the one place a class code is about the whole
           device rather than about one thing it does. */
        if (dev.device_class == CLASS_HUB) {
            nhubs++;
            bb_log("usb hub on slot %d, tier %d", slot, tier);
            walk_hub(slot, where, tier, where->speed);
        } else {
            claim_interface(slot, where->root_port, cfg, total);
        }
    }
    kfree(cfg);
}

static void setup_port(u32 port) {
    if (!xhci_reset_root_port(port)) return;
    xhci_where_t w = { port, 0, xhci_speed(port), 0, 0 };
    enumerate(&w, 0);
}

/* --- something was plugged in, or pulled out -------------------------------------- */
/*
 * The controller reports a port changing on its event ring, and that ring is
 * drained inside an interrupt handler, where none of the work can be done:
 * enumerating a device is a series of control transfers, each of which waits
 * for the controller to answer. So the handler records that something
 * happened and this task does the work.
 *
 * Only root ports are watched. A hub reports changes on its own interrupt
 * endpoint, and following that would mean keeping every hub's endpoint
 * listening and decoding its bitmap; what is here notices anything plugged
 * into the machine itself and finds whatever is behind it at that point,
 * which is the case that matters on a laptop.
 */
static bool claimed[XHCI_MAX_PORTS];
static void describe(void);

/* Drops everything that was hanging off a root port that is now empty. */
static void forget_root(u32 port) {
    for (u32 i = 0; i < MAX_DEVICES; i++) {
        device_t *d = &devices[i];
        if (!d->used || d->root != port) continue;
        if (d->keyboard) { if (nkeyboards) nkeyboards--; }
        else             { if (nmice) nmice--; }
        /* Before the disk goes, so nothing is left pointing at it. */
        diskfs_unmount_removable();
        usbdisk_detach(d->slot);
        xhci_detach(d->slot);
        d->used = false;
        bb_log("usb device on port %d unplugged", port);
    }
}

static void rescan(void) {
    u32 ports = xhci_ports();
    for (u32 p = 0; p < ports && p < XHCI_MAX_PORTS; p++) {
        bool there = xhci_port_connected(p);
        if (there && !claimed[p]) {
            claimed[p] = true;
            setup_port(p);
            describe();
        } else if (!there && claimed[p]) {
            claimed[p] = false;
            forget_root(p);
            describe();
        }
    }
}

static void service(void) {
    for (;;) {
        task_sleep(300);
        if (xhci_took_port_change()) rescan();
    }
}

void usb_start_service(void) {
    if (started) task_create("usb", service);
}

/* --- the outside ---------------------------------------------------------------- */
static void describe(void) {
    kformat(description, sizeof description,
            "%s, %d hub(s), %d keyboard(s), %d mouse, %d disk(s)",
            xhci_describe(), nhubs, nkeyboards, nmice, ndisks);
}

void usb_init(void) {
    bounce = (u8 *)kmalloc(BOUNCE_MAX);
    if (!bounce) return;
    if (!xhci_init()) {
        kformat(description, sizeof description, "no controller");
        return;
    }
    xhci_on_report(on_report);

    u32 ports = xhci_ports();
    for (u32 p = 0; p < ports; p++) {
        if (!xhci_port_connected(p)) continue;
        if (p < XHCI_MAX_PORTS) claimed[p] = true;
        setup_port(p);
    }

    started = true;
    describe();
}

void usb_poll(void) {
    if (started) xhci_poll();
}

bool usb_present(void) { return started; }
u32  usb_keyboards(void) { return nkeyboards; }
u32  usb_mice(void) { return nmice; }
u32  usb_hubs(void) { return nhubs; }
u32  usb_disks(void) { return ndisks; }
u32  usb_reports(void) { return nreports; }
const char *usb_describe(void) {
    return description[0] ? description : "not started";
}
