#pragma once
#include "types.h"

/* Ethernet over USB.
 *
 * This is the answer to a laptop whose wireless card will not start without
 * a vendor binary: a socket, or a phone. Enable USB tethering on a phone and
 * it presents itself as a network adapter, and what it presents is RNDIS,
 * which is two bulk endpoints and a handful of control messages. The same
 * driver covers the adapters that speak it.
 *
 * RNDIS is Microsoft's, and it is not elegant: every frame carries a forty
 * four byte header of which two fields are ever read, and the setup is a
 * conversation of encapsulated messages posted through the control endpoint
 * because the people who designed it were wrapping an existing Windows
 * interface rather than designing a protocol. But it is published, it is
 * what phones speak, and it has nothing in it that has to be taken on trust
 * from a binary.
 */

/* Called by the enumerator once both bulk endpoints of a CDC data interface
   are open. ctrl_iface is the interface number of the communications half,
   which is where the encapsulated messages are posted. */
bool usbnet_attach(u8 slot, u8 in_dci, u8 out_dci, u8 ctrl_iface);

/* And when it was unplugged. */
void usbnet_detach(u8 slot);

bool usbnet_present(void);
const u8 *usbnet_mac(void);
const char *usbnet_name(void);

/* Both the same shape as every other card here, so netdev.c can hold it
   behind the same door. */
bool usbnet_send(const void *frame, u16 len);
void usbnet_poll(void);
u32  usbnet_rx_count(void);
u32  usbnet_tx_count(void);
