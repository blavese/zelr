#pragma once
#include "types.h"

/* A wireless station: what a laptop's 802.11 does between its radio and the
 * network stack, written from IEEE 802.11-2016.
 *
 * Finding networks: on each channel in turn a probe request goes out and
 * every beacon and probe response heard is kept, with what its RSN element
 * says it protects with. Joining one: open authentication, association
 * (offering CCMP and a pre-shared key in the station's own RSN element),
 * and for a protected network the 4-way handshake (wpa.h has the key
 * arithmetic): the access point's nonce, ours and the keys from both, the
 * third message checked (its MIC, its replay counter, the nonce, the RSN
 * element against the beacon's, so nobody can talk us down) and its group
 * key unwrapped, the fourth sent, the keys put in. Then data frames carry
 * Ethernet frames (an LLC/SNAP header in place of the type), encrypted with
 * CCMP (ccmp.h) once there are keys: the pairwise one for what goes out and
 * what comes to us, the group one for what the access point sends to all.
 * A frame whose packet number is not past the last taken is dropped, and so
 * is one whose MIC does not hold.
 *
 * It is a state machine moved on by frames (wlan_rx) and by time
 * (wlan_poll), and talks to the hardware through a radio: send a frame, go
 * to a channel. A radio is anything that can do those two things: a card's
 * driver, or the simulated access point the self test joins (wlansim.c).
 *
 * Not done: open networks are joined without encryption but WEP, TKIP,
 * WPA3 (SAE), enterprise (802.1X), 802.11n/ac rates and power saving are
 * not; nor roaming, nor hidden networks.
 */

#define WLAN_SSID_MAX 32
#define WLAN_BSS_MAX  16

typedef struct {
    u8   bssid[6];
    char ssid[WLAN_SSID_MAX + 1];
    int  channel;
    int  signal;                 /* as the radio gave it: larger is nearer */
    int  secure;                 /* 0 open, 1 WPA2 with CCMP and a pre-shared key, -1 protected another way */
    u8   rsn[64];                /* its RSN element as it sent it */
    int  rsn_len;
} wlan_bss;

typedef struct {
    void (*tx)(const u8 *frame, u32 len);     /* a whole frame, no FCS */
    void (*channel)(int ch);
    void (*poll)(void);                       /* hands what it has heard to wlan_rx */
    const char *name;
    u8   mac[6];
} wlan_radio;

typedef enum {
    WLAN_IDLE, WLAN_SCANNING, WLAN_AUTH, WLAN_ASSOC, WLAN_HANDSHAKE, WLAN_CONNECTED, WLAN_FAILED
} wlan_state_t;

void wlan_attach(const wlan_radio *r);

/* A frame the radio heard, and how strong. */
void wlan_rx(const u8 *frame, u32 len, int signal);

/* Time passing: retries and giving up. Called once a tick (ten ms) by
   whoever drives the radio. */
void wlan_poll(void);

/* Every channel in turn; wlan_state is WLAN_SCANNING until it is done. */
void wlan_scan(void);
int  wlan_networks(wlan_bss *out, int max);

/* One of the networks heard, joined; 0 when joining has begun, -1 when there
   is no such network or it is protected in a way this does not do. */
int  wlan_join(const char *ssid, const char *passphrase);
void wlan_leave(void);

wlan_state_t wlan_state(void);
const char  *wlan_why(void);          /* what it is doing, or why it stopped */

/* Ethernet frames to and from the network once joined. */
bool wlan_send_eth(const u8 *eth, u32 len);
void wlan_on_eth(void (*fn)(const u8 *eth, u32 len));

/* How many frames came in and were dropped as replayed or not genuine. */
u32  wlan_dropped(void);

/* For the network stack (netdev.c): the radio, if one is attached (none
   detaches it); whether it is the machine's network (a card's driver or the
   "wlansim" boot word say so; the self test's is not); the network joined;
   Ethernet frames carried each way. */
const wlan_radio *wlan_radio_of(void);
void wlan_for_network(bool yes);
bool wlan_is_network(void);
const char *wlan_ssid(void);
u32  wlan_rx_count(void);
u32  wlan_tx_count(void);
