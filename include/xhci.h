#pragma once
#include "types.h"

/* The USB host controller every machine built this decade actually has.
 *
 * There were three before it, one per USB generation, and a laptop from the
 * last ten years has none of them: the ports are wired to xHCI whatever
 * speed the thing plugged into them runs at, and the firmware hands the
 * controller over in whatever state it left it. Without this driver zelr has
 * no keyboard on any such machine, because the PS/2 controller it does have
 * is either emulated by the firmware until an operating system takes over,
 * or is not there at all.
 *
 * What this exposes is the little that the USB layer above it needs: bring
 * the controller up, find out which ports have something on them, address a
 * device, ask it a question through its control endpoint, and set up an
 * interrupt endpoint that reports on its own. Everything specific to a kind
 * of device is in usb.c and nothing about the rings is. */

#define XHCI_MAX_PORTS  32
#define XHCI_MAX_SLOTS  16

/* USB speeds, as the controller numbers them in a port's status register. */
typedef enum {
    XHCI_SPEED_NONE = 0,
    XHCI_SPEED_FULL = 1,        /* 12 Mbit  */
    XHCI_SPEED_LOW  = 2,        /* 1.5 Mbit */
    XHCI_SPEED_HIGH = 3,        /* 480 Mbit */
    XHCI_SPEED_SUPER = 4,       /* 5 Gbit   */
} xhci_speed_t;

/* The eight bytes every control transfer starts with. */
typedef struct {
    u8  type;
    u8  request;
    u16 value;
    u16 index;
    u16 length;
} __attribute__((packed)) usb_setup_t;

/* Finds the controller, resets it and starts it. False when there is none,
   which is the normal answer on an emulated machine that was not given one
   and not an error. */
bool xhci_init(void);
bool xhci_present(void);

/* How many root ports the controller has, and whether one has something
   plugged into it. */
u32  xhci_ports(void);
bool xhci_port_connected(u32 port);

/* Where a device sits, which the controller has to be told because it is
   the thing routing packets to it.
 *
 * A device plugged straight into the machine is on a root port with nothing
 * in between. One behind a hub needs the path as well: four bits per tier,
 * lowest tier first, which is what a route string is. And a slow device
 * behind a fast hub needs to say which hub is doing the conversion, because
 * the controller talks to that hub at the fast speed and the hub does the
 * slow part on its own. */
typedef struct {
    u32 root_port;              /* the port on the controller */
    u32 route;                  /* four bits per hub below it, 0 on a root */
    xhci_speed_t speed;
    u8  tt_slot;                /* the hub translating, or 0 for none */
    u8  tt_port;
} xhci_where_t;

/* Resets a root port. False when there is nothing on it, or it would not
   come up. */
bool xhci_reset_root_port(u32 port);

/* Gives a device an address of its own. Returns the slot the controller
   assigned, or 0. */
u8   xhci_attach(const xhci_where_t *where);
xhci_speed_t xhci_speed(u32 port);

/* Tells the controller that a slot it has already addressed is a hub, and
   how many ports it has. Until it knows, it will not route anything to
   anything plugged into that hub. */
bool xhci_mark_hub(u8 slot, u8 ports, u8 think_time, bool multi_tt);

/* Gives a slot back, for a device that has been unplugged. What the slot was
   built out of is kept and used again by whatever is plugged in next. */
void xhci_detach(u8 slot);

/* Whether a root port has reported a change since this was last asked.
   Reading it clears it. */
bool xhci_took_port_change(void);

/* One control transfer, start to finish. data may be null for a transfer
   with no data stage. */
bool xhci_control(u8 slot, const usb_setup_t *setup, void *data, u16 len);

/* Corrects how big the control endpoint's packets are.
 *
 * A full speed device may use 8, 16, 32 or 64 bytes and there is no way to
 * know which before asking it, and asking it is itself a control transfer.
 * So the first one is done with 8, which every device accepts, and this puts
 * the real number in afterwards. Skipping it works right up until a device
 * that wanted 64 is read 8 bytes at a time: the controller sends a packet
 * shorter than the endpoint's size, the device takes that as the end of the
 * transfer, and the descriptor comes back truncated. */
bool xhci_set_packet_size(u8 slot, u16 max_packet);

/* Tells the controller about an interrupt-in endpoint, so it can be polled.
   dci is the endpoint's index in the device context: 2*number+1 for an in
   endpoint. */
bool xhci_open_interrupt_in(u8 slot, u8 dci, u16 max_packet, u8 interval);

/* Hands the controller a buffer to fill the next time the endpoint has
   something to say. One report per call: it is queued again when it comes
   back, which is what usb.c does with it. */
bool xhci_listen(u8 slot, u8 dci, void *buf, u16 len);

/* Drains the event ring. Safe from an interrupt handler and from ordinary
   code; the ring has one consumer and this is it. on_report is called for
   each interrupt transfer that completed, with the slot, the endpoint and
   how many bytes arrived. */
void xhci_poll(void);
void xhci_on_report(void (*fn)(u8 slot, u8 dci, u32 len));

/* What to show in the system information window. */
const char *xhci_describe(void);
