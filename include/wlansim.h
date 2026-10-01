#pragma once
#include "types.h"
#include "wlan.h"

/* A simulated WPA2 access point for the self test (kernel/wlansim.c): a
   radio whose far end is a network to join. */

/* A fresh access point, and the radio that reaches it for a station whose
   address is mac. */
const wlan_radio *wlansim_reset(const u8 mac[6]);

/* What is waiting for the station, delivered (wlan_rx). */
void wlansim_pump(void);

const char *wlansim_ssid(void);
const char *wlansim_password(void);
const u8   *wlansim_bssid(void);

/* How many second or fourth messages did not hold; whether the access point
   has the keys in. */
int  wlansim_mic_failures(void);
int  wlansim_keys(void);

/* For the test to make happen: a beacon, a frame to all under the group key,
   the last frame again, the last frame with a byte changed, and being sent
   away with a reason. */
void wlansim_beacon(void);
void wlansim_broadcast(void);
void wlansim_replay(void);
void wlansim_bend(void);
void wlansim_deauth(int code);

/* The next third message wrong: 1 offering less than the beacon (TKIP),
   2 with another nonce than the first message's, 3 with the first
   message's replay counter; 0 right again. And a frame in the clear. */
void wlansim_misbehave(int how);
void wlansim_plain(void);
