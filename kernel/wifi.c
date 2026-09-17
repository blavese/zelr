/* See include/wifi.h. Finding the card, and being straight about it. */
#include "wifi.h"
#include "pci.h"

#define VENDOR_ATHEROS 0x168C

static wifi_state_t state = WIFI_NONE;
static u16 vendor, device;

static const char *maker_of(u16 v) {
    switch (v) {
        case 0x8086:         return "intel";
        case VENDOR_ATHEROS: return "atheros";
        case 0x10EC:         return "realtek";
        case 0x14E4:         return "broadcom";
        case 0x1814:         return "ralink";
        case 0x14C3:         return "mediatek";
        case 0x1969:         return "qualcomm";
        default:             return "an unknown maker";
    }
}

void wifi_init(void) {
    state = WIFI_NONE;
    vendor = device = 0;

    /* Class 2 is a network controller; subclass 0x80 is the one that is not
       ethernet, which is where wireless lands. A card is taken as the one
       to report if it is the first found, and an Atheros is preferred over
       anything else because it is the only one that could be driven. */
    pci_dev_t found[4];
    u32 n = pci_list_class(0x02, 0x80, found, 4);
    if (!n) return;
    if (n > 4) n = 4;

    u32 pick = 0;
    for (u32 i = 0; i < n; i++)
        if (found[i].vendor == VENDOR_ATHEROS) { pick = i; break; }

    vendor = found[pick].vendor;
    device = found[pick].device;
    state = (vendor == VENDOR_ATHEROS) ? WIFI_DRIVABLE : WIFI_NEEDS_BLOB;
}

wifi_state_t wifi_state(void) { return state; }
u16 wifi_vendor(void) { return vendor; }
u16 wifi_device(void) { return device; }

const char *wifi_maker(void) {
    return state == WIFI_NONE ? "" : maker_of(vendor);
}

const char *wifi_describe(void) {
    switch (state) {
        case WIFI_NONE:
            return "no wireless card";
        case WIFI_DRIVABLE:
            /* The MAC is in hardware on these, so there is nothing to load
               and nothing stopping a driver written from scratch. */
            return "wireless, drivable without firmware";
        default:
            return "wireless, needs vendor firmware";
    }
}
