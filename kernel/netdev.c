/* Picks a network card and hides which one it was from the stack above.
 *
 * e1000 is tried first because it is what VirtualBox presents and what
 * VMware presents to a guest it recognises. The PCnet is what VMware gives
 * a guest it does not, which is any system written from scratch, so it is
 * the one that decides whether this works on somebody's own laptop. The
 * RTL8139 is QEMU's older default and is kept as the last of the three.
 *
 * A USB adapter is not chosen at boot because it cannot be: the bus has not
 * been walked yet when this runs, and the thing may well be plugged in an
 * hour later. So it is not picked, it is fallen back to, whenever there is
 * no card on the PCI bus and one has turned up on the USB one. That is the
 * case that matters on a laptop whose wireless will not start without a
 * vendor binary: a cable, an adapter, or a phone with tethering turned on. */
#include "netdev.h"
#include "pci.h"
#include "e1000.h"
#include "pcnet.h"
#include "rtl8139.h"
#include "usbnet.h"

typedef enum { NIC_NONE, NIC_E1000, NIC_PCNET, NIC_RTL8139, NIC_USB } nic_t;
static nic_t nic = NIC_NONE;

/* Asked every time rather than remembered, because the answer changes when
   something is plugged in. */
static nic_t current(void) {
    if (nic != NIC_NONE) return nic;
    return usbnet_present() ? NIC_USB : NIC_NONE;
}

bool netdev_init(void) {
    if (e1000_init())  { nic = NIC_E1000;   return true; }
    if (pcnet_init())  { nic = NIC_PCNET;   return true; }
    if (rtl_init())    { nic = NIC_RTL8139; return true; }
    nic = NIC_NONE;
    return false;
}

bool netdev_up(void) {
    switch (current()) {
        case NIC_E1000:   return e1000_up();
        case NIC_PCNET:   return pcnet_up();
        case NIC_RTL8139: return rtl_up();
        case NIC_USB:     return true;
        default:          return false;
    }
}

bool netdev_send(const void *data, u16 len) {
    switch (current()) {
        case NIC_E1000:   return e1000_send(data, len);
        case NIC_PCNET:   return pcnet_send(data, len);
        case NIC_RTL8139: return rtl_send(data, len);
        case NIC_USB:     return usbnet_send(data, len);
        default:          return false;
    }
}

void netdev_poll(void) {
    switch (current()) {
        case NIC_E1000:   e1000_poll(); break;
        case NIC_PCNET:   pcnet_poll(); break;
        case NIC_RTL8139: rtl_poll();   break;
        case NIC_USB:     usbnet_poll(); break;
        default: break;
    }
}

const u8 *netdev_mac(void) {
    static const u8 zero[6] = { 0, 0, 0, 0, 0, 0 };
    switch (current()) {
        case NIC_E1000:   return e1000_mac();
        case NIC_PCNET:   return pcnet_mac();
        case NIC_RTL8139: return rtl_mac();
        case NIC_USB:     return usbnet_mac();
        default:          return zero;
    }
}

u32 netdev_rx_count(void) {
    switch (current()) {
        case NIC_E1000:   return e1000_rx_count();
        case NIC_PCNET:   return pcnet_rx_count();
        case NIC_RTL8139: return rtl_rx_count();
        case NIC_USB:     return usbnet_rx_count();
        default:          return 0;
    }
}

u32 netdev_tx_count(void) {
    switch (current()) {
        case NIC_E1000:   return e1000_tx_count();
        case NIC_PCNET:   return pcnet_tx_count();
        case NIC_RTL8139: return rtl_tx_count();
        case NIC_USB:     return usbnet_tx_count();
        default:          return 0;
    }
}

bool netdev_undriven(u16 *vendor, u16 *device) {
    if (current() != NIC_NONE) return false;
    /* Worked out once. Walking the bus is tens of thousands of
       configuration reads, this is asked by a panel that redraws itself
       while it is open, and nothing appears on the PCI bus while the
       machine is running. What can appear is a USB adapter, and that is
       above, in current(). */
    static bool looked, present;
    static u16 saw_vendor, saw_device;

    if (!looked) {
        looked = true;
        /* By class rather than by programming interface, because that byte
           means something different on every card and matching it would
           miss the ones this is for. */
        pci_dev_t found;
        if (pci_list_class(0x02, 0x00, &found, 1) > 0) {
            present = true;
            saw_vendor = found.vendor;
            saw_device = found.device;
        }
    }

    if (!present) return false;
    if (vendor) *vendor = saw_vendor;
    if (device) *device = saw_device;
    return true;
}

const char *netdev_name(void) {
    switch (current()) {
        case NIC_E1000:   return "e1000";
        case NIC_PCNET:   return "pcnet";
        case NIC_RTL8139: return "rtl8139";
        case NIC_USB:     return usbnet_name();
        default:          return "none";
    }
}
