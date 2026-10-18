/* See include/timesync.h. */
#include "timesync.h"
#include "net.h"
#include "zone.h"
#include "rng.h"
#include "printf.h"
#include "string.h"

/* NTP counts seconds from 1900, and its 32 bits run out in February 2036.
   A stamp with the top bit clear is past that (RFC 4330's rule, good until
   2104); with it set, between 1968 and 2036. */
#define NTP_UNIX_GAP 2208988800LL

static char said[112] = "not asked yet";

const char *timesync_said(void) { return said; }

static u32 be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

bool timesync_read(const u8 *p, u32 len, const u8 nonce[8], i64 *unix_out, const char **why) {
    if (len < 48) { *why = "the answer was short"; return false; }
    u8 leap = (u8)(p[0] >> 6), version = (u8)((p[0] >> 3) & 7), mode = (u8)(p[0] & 7);
    if (mode != 4) { *why = "the answer was not a server's"; return false; }
    if (version < 3 || version > 4) { *why = "the answer was in a version this does not read"; return false; }
    for (int i = 0; i < 8; i++)
        if (p[24 + i] != nonce[i]) { *why = "the answer was not to this question"; return false; }
    /* Stratum nought is a refusal ("kiss of death"): the server asks not to
       be asked, and gives no time. */
    if (p[1] == 0) { *why = "the server refused"; return false; }
    if (p[1] > 15 || leap == 3) { *why = "the server's own clock is not set"; return false; }
    u32 secs = be32(p + 40), frac = be32(p + 44);
    if (!secs && !frac) { *why = "the answer had no time in it"; return false; }
    i64 t = (i64)secs + ((secs & 0x80000000u) ? 0 : 0x100000000LL);
    *unix_out = t - NTP_UNIX_GAP + (frac >= 0x80000000u ? 1 : 0);
    return true;
}

bool timesync_now(const char *server) {
    char host[64];
    u16 port = 123;
    u32 n = 0;
    while (server[n] && server[n] != ':' && n < sizeof(host) - 1) { host[n] = server[n]; n++; }
    host[n] = 0;
    if (server[n] == ':') {
        u32 v = 0;
        for (const char *q = server + n + 1; *q >= '0' && *q <= '9'; q++) v = v * 10 + (u32)(*q - '0');
        if (v && v < 65536) port = (u16)v;
    }

    if (!net_up()) { kformat(said, sizeof(said), "not set: no network"); return false; }
    ipv4_t ip = net_parse_ip(host);
    if (!ip && !net_resolve(host, &ip, 4000)) {
        kformat(said, sizeof(said), "not set: %s was not found", host);
        return false;
    }

    u8 nonce[8], reply[48];
    rng_bytes(nonce, sizeof(nonce));
    if (!net_sntp(ip, port, nonce, reply, 4000)) {
        kformat(said, sizeof(said), "not set: no answer from %s", server);
        return false;
    }
    i64 network;
    const char *why = "";
    if (!timesync_read(reply, sizeof(reply), nonce, &network, &why)) {
        kformat(said, sizeof(said), "not set: %s (%s)", why, server);
        return false;
    }

    /* How far the clock is out, as it reads now with what it was already
       corrected by, and the correction moved by that much. */
    zone_time_t z;
    if (!zone_now(&z)) { kformat(said, sizeof(said), "not set: no clock to set"); return false; }
    i64 off = network - z.utc;
    zone_set_adjust(zone_adjust() + off);
    i64 a = off < 0 ? -off : off;
    if (a < 2) kformat(said, sizeof(said), "set from %s, the clock was right", server);
    else kformat(said, sizeof(said), "set from %s, the clock was %d s %s", server,
                 (i32)(a > 0x7fffffff ? 0x7fffffff : a), off < 0 ? "fast" : "slow");
    return true;
}
