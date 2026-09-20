#pragma once
#include "types.h"

#define ETH_ALEN 6

typedef u32 ipv4_t;      /* stored in host byte order */

void  net_init(void);
void  net_receive(const u8 *frame, u16 len);
void  net_poll(void);

/* The task that runs the stack when nothing else is waiting on it. Started
   once the scheduler exists, because it is a task. */
void  net_start_service(void);

/* What the queue between the card and the stack is doing. Dropped frames are
   frames the card delivered that nothing came to collect, which is the one
   number that says the machine is behind rather than merely busy. */
u32   net_rx_queued(void);
u32   net_rx_dropped(void);
u32   net_rx_deepest(void);

bool  net_up(void);
ipv4_t net_ip(void);
ipv4_t net_gateway(void);
ipv4_t net_netmask(void);
ipv4_t net_dns(void);
const u8 *net_mac(void);

/* Runs the full DHCP handshake. Returns true once an address is bound.
   Blocks for up to the timeout, so anything with a screen wants the pair
   below instead. */
bool  net_dhcp(u32 timeout_ms);

/* Asks for an address in a task of its own, and says whether one is already
   being asked for.

   Two things want to ask: the machine, once, as soon as it has a card, and
   somebody pressing the button on the panel when that did not work. Both go
   through here so that they cannot both be in the middle of an exchange at
   once, which they would be sharing one UDP socket and one transaction id to
   do. Doing nothing when an ask is already running is the whole point rather
   than an optimisation. */
void  net_dhcp_start(void);
bool  net_dhcp_busy(void);

/* Sends an echo request and waits for the reply. Returns round trip in
   milliseconds, or -1 on timeout. */
int   net_ping(ipv4_t dst, u32 timeout_ms);

bool  net_udp_send(ipv4_t dst, u16 sport, u16 dport, const void *data, u16 len);

/* Resolves a name through the DHCP-supplied resolver. */
bool  net_resolve(const char *host, ipv4_t *out, u32 timeout_ms);

ipv4_t net_parse_ip(const char *s);
void   net_format_ip(ipv4_t ip, char *out);

u32   net_rx_packets(void);
u32   net_tx_packets(void);
