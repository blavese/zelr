#pragma once
#include "types.h"

/* What wireless hardware this machine has, and whether it can be used.
 *
 * Three answers rather than two, because "no wireless" and "wireless this
 * kernel will not drive" are different facts and only one of them is about
 * the machine. Most wireless cards run the 802.11 MAC as firmware on a
 * processor of their own, and that firmware is a binary from the vendor.
 * A kernel written from scratch cannot make such a card transmit at all,
 * and no amount of further code changes that: the logic is not missing, it
 * is on the other side of a chip that will not start without its own
 * software.
 *
 * Atheros parts are the exception. Their MAC is in hardware and a driver
 * talks to it directly, which is why they are the only ones named here.
 *
 * So a machine with no wireless says so, and a machine with wireless it
 * cannot use says which card and why, which is more useful than an empty
 * list of networks and considerably more honest.
 */

typedef enum {
    WIFI_NONE,        /* nothing on the bus that is a wireless controller */
    WIFI_NEEDS_BLOB,  /* one is there and will not start without a vendor
                         binary, so this kernel cannot drive it */
    WIFI_DRIVABLE,    /* one is there whose MAC is in hardware */
} wifi_state_t;

void wifi_init(void);

wifi_state_t wifi_state(void);

/* Who made it and what it is, for saying why it is not working. Zero and an
   empty string when there is nothing. */
u16  wifi_vendor(void);
u16  wifi_device(void);
const char *wifi_maker(void);

/* One line about the situation, for the panel and the boot log. Always
   something sayable, whatever the state. */
const char *wifi_describe(void);
