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
    u16  length;                /* how much of the report to expect */
    u8  *buf;                   /* the controller writes into this */
    u8   last[REPORT_MAX];      /* the previous one, to tell presses apart */
} device_t;

static device_t devices[MAX_DEVICES];
static u32 nkeyboards, nmice;
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

static bool get_descriptor(u8 slot, u8 type, u8 index, void *out, u16 len) {
    if (!bounce || len > BOUNCE_MAX) return false;
    memset(bounce, 0, len);
    usb_setup_t s = { 0x80, REQ_GET_DESCRIPTOR,
                      (u16)((u16)type << 8 | index), 0, len };
    if (!xhci_control(slot, &s, bounce, len)) return false;
    memcpy(out, bounce, len);
    return true;
}

static bool set_configuration(u8 slot, u8 value) {
    usb_setup_t s = { 0x00, REQ_SET_CONFIG, value, 0, 0 };
    return xhci_control(slot, &s, 0, 0);
}

static bool set_boot_protocol(u8 slot, u8 interface) {
    usb_setup_t s = { 0x21, REQ_SET_PROTOCOL, 0, interface, 0 };
    return xhci_control(slot, &s, 0, 0);
}

static bool set_idle(u8 slot, u8 interface) {
    /* Zero means report only when something changes. Without it a keyboard
       with a key held down repeats it as fast as its interval allows. */
    usb_setup_t s = { 0x21, REQ_SET_IDLE, 0, interface, 0 };
    return xhci_control(slot, &s, 0, 0);
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
static bool claim_interface(u8 slot, const u8 *cfg, u16 total) {
    const interface_desc_t *want = 0;
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
    return false;
}

static void setup_port(u32 port) {
    u8 slot = xhci_attach(port);
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
    if (!total || total > 512) return;

    u8 *cfg = (u8 *)kmalloc(total);
    if (!cfg) return;
    memset(cfg, 0, total);
    if (get_descriptor(slot, DESC_CONFIG, 0, cfg, total)
        && set_configuration(slot, head.value))
        claim_interface(slot, cfg, total);
    kfree(cfg);
}

/* --- the outside ---------------------------------------------------------------- */
void usb_init(void) {
    bounce = (u8 *)kmalloc(BOUNCE_MAX);
    if (!bounce) return;
    if (!xhci_init()) {
        kformat(description, sizeof description, "no controller");
        return;
    }
    xhci_on_report(on_report);

    u32 ports = xhci_ports();
    for (u32 p = 0; p < ports; p++)
        if (xhci_port_connected(p)) setup_port(p);

    started = true;
    kformat(description, sizeof description, "%s, %d keyboard(s), %d mouse",
              xhci_describe(), nkeyboards, nmice);
}

void usb_poll(void) {
    if (started) xhci_poll();
}

bool usb_present(void) { return started; }
u32  usb_keyboards(void) { return nkeyboards; }
u32  usb_mice(void) { return nmice; }
u32  usb_reports(void) { return nreports; }
const char *usb_describe(void) {
    return description[0] ? description : "not started";
}
