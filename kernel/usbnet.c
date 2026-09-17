/* See include/usbnet.h.
 *
 * Two things make this driver look stranger than it is.
 *
 * The setup is a conversation rather than a few register writes: a message
 * is posted to the device through the control endpoint, and the answer is
 * fetched with a second control transfer. Every step is send, then ask for
 * the reply, and the reply has to be matched to the request by an id
 * because nothing else distinguishes them.
 *
 * And every frame carries a forty four byte header, of which this reads two
 * fields: where the payload starts and how long it is. The rest describes
 * out of band data that nothing sends.
 */
#include "usbnet.h"
#include "xhci.h"
#include "net.h"
#include "heap.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "blackbox.h"

/* --- the messages --------------------------------------------------------- */

#define MSG_PACKET        0x00000001u
#define MSG_INITIALIZE    0x00000002u
#define MSG_INIT_CMPLT    0x80000002u
#define MSG_QUERY         0x00000004u
#define MSG_QUERY_CMPLT   0x80000004u
#define MSG_SET           0x00000005u
#define MSG_SET_CMPLT     0x80000005u

#define STATUS_SUCCESS    0x00000000u

/* The two facts worth asking a network adapter for. */
#define OID_PERMANENT_ADDRESS 0x01010101u
#define OID_PACKET_FILTER     0x0001010Eu

/* Everything addressed to us, everything addressed to everyone, and the
   multicast in between. Without this the adapter is open and silent, which
   is the single easiest way to spend an afternoon on this driver. */
#define FILTER_DIRECTED   0x01
#define FILTER_MULTICAST  0x02
#define FILTER_BROADCAST  0x08
#define FILTER_WANTED     (FILTER_DIRECTED | FILTER_MULTICAST | FILTER_BROADCAST)

#define PACKET_HEADER  44
#define CONTROL_MAX    256
#define FRAME_MAX      1536
#define TRANSFER_MAX   (PACKET_HEADER + FRAME_MAX)

/* Class requests on the communications interface: one to post a message,
   one to collect the answer. */
#define SEND_ENCAPSULATED  0x00
#define GET_ENCAPSULATED   0x01

static bool present;
static u8   slot_used, ep_in, ep_out, comm_iface;
static u8   mac[6];
static u32  request_id = 1;
static u32  rx_count, tx_count;

static u8  *tx;          /* one transfer out, header and frame together */
static u8  *rx;          /* and one in */

/* And the control conversation, which cannot use the stack.
 *
 * The controller is given a physical address and this kernel hands it the
 * pointer it has, so anything it is pointed at has to be somewhere the two
 * are the same. The heap is; a kernel stack is not always, and a buffer
 * that happens to be in the wrong place produces a refused transfer rather
 * than a wrong one. That is how this was found: the first message went and
 * the second did not, and the only difference between them was which
 * function's frame they sat in. */
static u8  *cmd;         /* the message going out */
static u8  *answer;      /* and the one coming back */

#define CMD_MAX 64

/* Little endian on the wire, and the compiler's idea of a struct is not
   something to post to a device, so the words go in and come out by hand. */
static void put32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static u32 get32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static bool post(const u8 *msg, u16 len) {
    usb_setup_t s;
    s.type = 0x21;              /* to the device, class, interface */
    s.request = SEND_ENCAPSULATED;
    s.value = 0;
    s.index = comm_iface;
    s.length = len;
    return xhci_control(slot_used, &s, (void *)msg, len);
}

static bool collect(u8 *out, u16 len) {
    usb_setup_t s;
    s.type = 0xA1;              /* from the device, class, interface */
    s.request = GET_ENCAPSULATED;
    s.value = 0;
    s.index = comm_iface;
    s.length = len;
    return xhci_control(slot_used, &s, out, len);
}

/* Post a message and read what came back, retrying the read because the
   answer is not necessarily ready the instant the question was accepted. */
static bool ask(const u8 *msg, u16 len, u8 *reply, u16 reply_len, u32 want) {
    if (!post(msg, len)) return false;

    for (int tries = 0; tries < 20; tries++) {
        memset(reply, 0, reply_len);
        if (collect(reply, reply_len) && get32(reply) == want)
            return true;
        sleep_ms(5);
    }
    return false;
}

static bool initialize(void) {
    put32(cmd + 0, MSG_INITIALIZE);
    put32(cmd + 4, 24);
    put32(cmd + 8, request_id++);
    put32(cmd + 12, 1);                 /* the version this speaks */
    put32(cmd + 16, 0);
    put32(cmd + 20, TRANSFER_MAX);

    if (!ask(cmd, 24, answer, CONTROL_MAX, MSG_INIT_CMPLT)) return false;
    return get32(answer + 12) == STATUS_SUCCESS;
}

static bool query(u32 oid, u8 *value, u32 want_len) {
    put32(cmd + 0, MSG_QUERY);
    put32(cmd + 4, 28);
    put32(cmd + 8, request_id++);
    put32(cmd + 12, oid);
    /* No information buffer goes with a query, so its length is zero and
       its offset is zero with it. Naming an offset for a buffer that is not
       there puts it one past the end of the message, and a device that
       checks refuses the whole thing: the transfer is stalled, and what
       that looks like from here is a message that would not send. */
    put32(cmd + 16, 0);
    put32(cmd + 20, 0);
    put32(cmd + 24, 0);

    if (!ask(cmd, 28, answer, CONTROL_MAX, MSG_QUERY_CMPLT)) return false;
    if (get32(answer + 12) != STATUS_SUCCESS) return false;

    u32 len = get32(answer + 16);
    u32 off = get32(answer + 20);

    /* The offset counts from the request id, eight bytes in, which is the
       one arithmetic mistake this protocol invites. */
    u32 at = off + 8;
    if (len < want_len || at + want_len > CONTROL_MAX) return false;

    memcpy(value, answer + at, want_len);
    return true;
}

static bool set_filter(u32 filter) {
    put32(cmd + 0, MSG_SET);
    put32(cmd + 4, 32);
    put32(cmd + 8, request_id++);
    put32(cmd + 12, OID_PACKET_FILTER);
    put32(cmd + 16, 4);
    put32(cmd + 20, 20);                /* again, counted from byte eight */
    put32(cmd + 24, 0);
    put32(cmd + 28, filter);

    if (!ask(cmd, 32, answer, CONTROL_MAX, MSG_SET_CMPLT)) return false;
    return get32(answer + 12) == STATUS_SUCCESS;
}

/* --- the driver ----------------------------------------------------------- */

bool usbnet_attach(u8 slot, u8 in_dci, u8 out_dci, u8 ctrl_iface) {
    if (present) return false;          /* one is enough */

    rx_count = tx_count = 0;
    slot_used = slot;
    ep_in = in_dci;
    ep_out = out_dci;
    comm_iface = ctrl_iface;
    request_id = 1;

    if (!tx) tx = (u8 *)kmalloc(TRANSFER_MAX);
    if (!rx) rx = (u8 *)kmalloc(TRANSFER_MAX);
    if (!cmd) cmd = (u8 *)kmalloc(CMD_MAX);
    if (!answer) answer = (u8 *)kmalloc(CONTROL_MAX);
    if (!tx || !rx || !cmd || !answer) return false;
    memset(cmd, 0, CMD_MAX);

    if (!initialize()) {
        bb_log("usbnet: it would not initialise");
        return false;
    }
    if (!query(OID_PERMANENT_ADDRESS, mac, 6)) {
        bb_log("usbnet: no address came back");
        return false;
    }
    if (!set_filter(FILTER_WANTED)) {
        bb_log("usbnet: the filter was refused, nothing would arrive");
        return false;
    }

    present = true;
    bb_log("usbnet %02x:%02x:%02x:%02x:%02x:%02x on slot %d",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], slot);
    return true;
}

void usbnet_detach(u8 slot) {
    if (present && slot == slot_used) present = false;
}

bool usbnet_present(void) { return present; }
const u8 *usbnet_mac(void) { return mac; }
const char *usbnet_name(void) { return "usb ethernet"; }

bool usbnet_send(const void *frame, u16 len) {
    if (!present || !tx || len > FRAME_MAX) return false;

    memset(tx, 0, PACKET_HEADER);
    put32(tx + 0, MSG_PACKET);
    put32(tx + 4, PACKET_HEADER + len);
    put32(tx + 8, PACKET_HEADER - 8);   /* the payload, from byte eight */
    put32(tx + 12, len);
    memcpy(tx + PACKET_HEADER, frame, len);

    if (xhci_bulk(slot_used, ep_out, tx, PACKET_HEADER + len, false) < 0)
        return false;
    tx_count++;
    return true;
}

void usbnet_poll(void) {
    if (!present || !rx) return;

    int got = xhci_bulk(slot_used, ep_in, rx, TRANSFER_MAX, true);
    if (got < PACKET_HEADER) return;

    if (get32(rx) != MSG_PACKET) return;

    u32 off = get32(rx + 8) + 8;
    u32 len = get32(rx + 12);

    /* Both of these come off a wire, so both are checked against what
       actually arrived rather than against what the header claims came. */
    if (!len || len > FRAME_MAX) return;
    if (off + len > (u32)got) return;

    rx_count++;
    net_receive(rx + off, (u16)len);
}

u32 usbnet_rx_count(void) { return rx_count; }
u32 usbnet_tx_count(void) { return tx_count; }
