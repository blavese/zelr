/* Picks a network card and hides which one it was from the stack above.
 *
 * e1000 is tried first because it is what VirtualBox and VMware present;
 * the RTL8139 is QEMU's older default and is kept as the fallback.
 *
 * A USB adapter is not chosen at boot because it cannot be: the bus has not
 * been walked yet when this runs, and the thing may well be plugged in an
 * hour later. So it is not picked, it is fallen back to, whenever there is
 * no card on the PCI bus and one has turned up on the USB one. That is the
 * case that matters on a laptop whose wireless will not start without a
 * vendor binary: a cable, an adapter, or a phone with tethering turned on. */
#include "netdev.h"
#include "e1000.h"
#include "rtl8139.h"
#include "usbnet.h"

typedef enum { NIC_NONE, NIC_E1000, NIC_RTL8139, NIC_USB } nic_t;
static nic_t nic = NIC_NONE;

/* Asked every time rather than remembered, because the answer changes when
   something is plugged in. */
static nic_t current(void) {
    if (nic != NIC_NONE) return nic;
    return usbnet_present() ? NIC_USB : NIC_NONE;
}

bool netdev_init(void) {
    if (e1000_init())  { nic = NIC_E1000;   return true; }
    if (rtl_init())    { nic = NIC_RTL8139; return true; }
    nic = NIC_NONE;
    return false;
}

bool netdev_up(void) {
    switch (current()) {
        case NIC_E1000:   return e1000_up();
        case NIC_RTL8139: return rtl_up();
        case NIC_USB:     return true;
        default:          return false;
    }
}

bool netdev_send(const void *data, u16 len) {
    switch (current()) {
        case NIC_E1000:   return e1000_send(data, len);
        case NIC_RTL8139: return rtl_send(data, len);
        case NIC_USB:     return usbnet_send(data, len);
        default:          return false;
    }
}

void netdev_poll(void) {
    switch (current()) {
        case NIC_E1000:   e1000_poll(); break;
        case NIC_RTL8139: rtl_poll();   break;
        case NIC_USB:     usbnet_poll(); break;
        default: break;
    }
}

const u8 *netdev_mac(void) {
    static const u8 zero[6] = { 0, 0, 0, 0, 0, 0 };
    switch (current()) {
        case NIC_E1000:   return e1000_mac();
        case NIC_RTL8139: return rtl_mac();
        case NIC_USB:     return usbnet_mac();
        default:          return zero;
    }
}

u32 netdev_rx_count(void) {
    switch (current()) {
        case NIC_E1000:   return e1000_rx_count();
        case NIC_RTL8139: return rtl_rx_count();
        case NIC_USB:     return usbnet_rx_count();
        default:          return 0;
    }
}

u32 netdev_tx_count(void) {
    switch (current()) {
        case NIC_E1000:   return e1000_tx_count();
        case NIC_RTL8139: return rtl_tx_count();
        case NIC_USB:     return usbnet_tx_count();
        default:          return 0;
    }
}

const char *netdev_name(void) {
    switch (current()) {
        case NIC_E1000:   return "e1000";
        case NIC_RTL8139: return "rtl8139";
        case NIC_USB:     return usbnet_name();
        default:          return "none";
    }
}
