#pragma once
#include "types.h"

bool pcnet_init(void);
bool pcnet_up(void);
bool pcnet_send(const void *data, u16 len);
void pcnet_poll(void);
const u8 *pcnet_mac(void);
u32  pcnet_rx_count(void);
u32  pcnet_tx_count(void);
