#pragma once
#include "types.h"
#include "net.h"

bool tcp_connect(ipv4_t ip, u16 port, u32 timeout_ms);
bool tcp_send(const void *data, u16 len);

/* Hands over what has arrived and takes it out of the buffer, so reading in
   a loop walks through the answer rather than returning its first piece over
   and over. Returns as soon as there is anything. */
u32  tcp_recv(u8 *out, u32 cap, u32 timeout_ms);

/* Nothing more is coming. A read of nothing means the end of the answer only
   when this is true, and otherwise means nothing arrived in time. */
bool tcp_ended(void);
int  tcp_state_code(void);
u32  tcp_resets(void);
u32  tcp_out_of_order(void);
u32  tcp_retransmits(void);

void tcp_close(void);
bool tcp_connected(void);
void tcp_input(ipv4_t src, const u8 *p, u16 len);
