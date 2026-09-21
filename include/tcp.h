#pragma once
#include "types.h"
#include "net.h"

/* How many connections this machine can hold open at once.
 *
 * It was one. Not one per program -- one, for the whole machine, in file
 * level variables: `tcp_connect` had nowhere to put a second connection and
 * `tcp_input` had nothing to match an arriving segment against except the
 * only connection there was. A page and its pictures were fetched strictly
 * one after another, nothing could ask the network for anything while a
 * page was loading, and two programs could not both be online.
 *
 * Six because a page with a stylesheet and a handful of images is the case
 * this is for, and because every one of them holds a receive buffer of
 * sixty four kilobytes. */
#define TCP_MAX 6

/* Opens one and waits for the handshake. Returns a handle, or -1: there is
   no connection to talk about when it fails, which is why this does not
   return a bool and take the handle separately. */
int  tcp_open(ipv4_t ip, u16 port, u32 timeout_ms);

bool tcp_send(int h, const void *data, u16 len);

/* Hands over what has arrived and takes it out of the buffer, so reading in
   a loop walks through the answer rather than returning its first piece over
   and over. Returns as soon as there is anything. */
u32  tcp_recv(int h, u8 *out, u32 cap, u32 timeout_ms);

/* Nothing more is coming. A read of nothing means the end of the answer only
   when this is true, and otherwise means nothing arrived in time. */
bool tcp_ended(int h);
int  tcp_state_code(int h);

void tcp_close(int h);
bool tcp_connected(int h);

/* Totals since boot across every connection, which is what /sys/net is
   about: a count that resets when a connection closes says nothing about a
   machine's link. */
u32  tcp_resets(void);
u32  tcp_out_of_order(void);
u32  tcp_retransmits(void);

/* How many are open, for anything that reports on the machine. */
u32  tcp_open_count(void);

/* Resends whatever is outstanding on every open connection once its timer
   expires. Called from any loop that is waiting on the network. */
void tcp_pump(void);

void tcp_input(ipv4_t src, const u8 *p, u16 len);
