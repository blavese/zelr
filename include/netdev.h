#pragma once
#include "types.h"

/* Whichever network card was found, behind one interface. */
bool netdev_init(void);
bool netdev_up(void);
bool netdev_send(const void *data, u16 len);
void netdev_poll(void);
/* Whether the card hands frames over from an interrupt of its own, rather than
   only when netdev_poll asks. */
bool netdev_interrupts(void);
const u8 *netdev_mac(void);
u32  netdev_rx_count(void);
u32  netdev_tx_count(void);
const char *netdev_name(void);

/* The wireless station's Ethernet frames, into the stack (wlan_on_eth). */
void netdev_wlan_rx(const u8 *eth, u32 len);

/* When none of the drivers matched: the first ethernet controller on the
   bus, if there is one. "There is no card" and "there is a card nothing
   here can drive" are different problems with the same symptom, and only
   one of them is fixed by plugging something in. */
bool netdev_undriven(u16 *vendor, u16 *device);
