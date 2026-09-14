/* A single-connection TCP client, with retransmission.
 *
 * The earlier version assumed nothing was ever lost, which is true of an
 * emulated link and false of a real one. It now keeps the last unacknowledged
 * segment, times it, and sends it again if the acknowledgement does not
 * arrive. The timeout doubles on each attempt, so a link that is briefly
 * congested is not made worse by a burst of retries.
 *
 * Still one connection at a time, and the send window is one segment: this
 * waits for each piece to be acknowledged before sending the next. That is
 * slow on a fat link and perfectly correct on any of them. */
#include "tcp.h"
#include "net.h"
#include "netpriv.h"
#include "timer.h"
#include "printf.h"
#include "string.h"
#include "heap.h"

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10

#define RTO_MIN_MS   400
#define RTO_MAX_MS   4000
#define MAX_RETRIES  6

typedef struct {
    u16 sport, dport;
    u32 seq, ack;
    u8  offset, flags;
    u16 window, csum, urgent;
} __attribute__((packed)) tcp_t;

typedef enum { T_CLOSED, T_SYNSENT, T_OPEN, T_CLOSING, T_DONE } tstate_t;

static volatile tstate_t state = T_CLOSED;
static ipv4_t peer_ip;
static u16    peer_port, local_port;
static volatile u32 snd_nxt, snd_una, rcv_nxt;
static volatile bool got_fin;

/* The one segment in flight, kept so it can be sent again. */
static u8  rt_data[1400];
static u16 rt_len;
static u32 rt_seq;
static u8  rt_flags;
static bool rt_pending;
static u64 rt_sent_at;
static u32 rt_timeout_ms;
static int rt_tries;
static u32 rt_total;              /* retransmissions this connection */

#define RXCAP 16384
static u8 *rxbuf;
static volatile u32 rxlen;

u32 tcp_retransmits(void) { return rt_total; }

static u16 tcp_checksum(ipv4_t src, ipv4_t dst, const u8 *seg, u16 len) {
    u8 ph[12];
    ph[0] = (u8)(src >> 24); ph[1] = (u8)(src >> 16); ph[2] = (u8)(src >> 8); ph[3] = (u8)src;
    ph[4] = (u8)(dst >> 24); ph[5] = (u8)(dst >> 16); ph[6] = (u8)(dst >> 8); ph[7] = (u8)dst;
    ph[8] = 0; ph[9] = 6;
    ph[10] = (u8)(len >> 8); ph[11] = (u8)len;

    u32 sum = 0;
    for (int i = 0; i < 12; i += 2) sum += (u32)((ph[i] << 8) | ph[i + 1]);
    u16 i = 0;
    for (; i + 1 < len; i += 2) sum += (u32)((seg[i] << 8) | seg[i + 1]);
    if (i < len) sum += (u32)(seg[i] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

/* Puts one segment on the wire. Does not touch the retransmit slot. */
static bool emit(u32 seq, u8 flags, const void *data, u16 dlen) {
    u8 seg[1500];
    if (sizeof(tcp_t) + dlen > sizeof(seg)) return false;

    tcp_t *t = (tcp_t *)seg;
    t->sport = np_hs(local_port);
    t->dport = np_hs(peer_port);
    t->seq = np_hl(seq);
    t->ack = np_hl(rcv_nxt);
    t->offset = 5 << 4;
    t->flags = flags;
    t->window = np_hs(8192);
    t->csum = 0;
    t->urgent = 0;
    if (dlen) memcpy(seg + sizeof(tcp_t), data, dlen);

    u16 total = (u16)(sizeof(tcp_t) + dlen);
    t->csum = np_hs(tcp_checksum(net_ip(), peer_ip, seg, total));
    return np_ip_send(peer_ip, 6, seg, total);
}

/* Sends a segment and remembers it until it is acknowledged. */
static bool send_reliable(u8 flags, const void *data, u16 dlen) {
    if (dlen > sizeof(rt_data)) return false;

    rt_seq = snd_nxt;
    rt_flags = flags;
    rt_len = dlen;
    if (dlen) memcpy(rt_data, data, dlen);
    rt_pending = true;
    rt_tries = 0;
    rt_timeout_ms = RTO_MIN_MS;
    rt_sent_at = timer_ticks();

    return emit(rt_seq, flags, data, dlen);
}

/* A bare acknowledgement carries no sequence space, so it is never resent:
   if it is lost the peer simply sends its data again. */
static bool send_ack(void) { return emit(snd_nxt, TH_ACK, 0, 0); }

/* Called from the receive loops. Resends whatever is still outstanding once
   its timer expires. */
void tcp_pump(void) {
    if (!rt_pending || state == T_CLOSED) return;

    u64 elapsed_ticks = timer_ticks() - rt_sent_at;
    u32 elapsed_ms = (u32)(elapsed_ticks * 1000u / timer_hz());
    if (elapsed_ms < rt_timeout_ms) return;

    if (rt_tries >= MAX_RETRIES) {
        state = T_DONE;                       /* the peer is not answering */
        rt_pending = false;
        return;
    }

    rt_tries++;
    rt_total++;
    rt_timeout_ms *= 2;
    if (rt_timeout_ms > RTO_MAX_MS) rt_timeout_ms = RTO_MAX_MS;
    rt_sent_at = timer_ticks();
    emit(rt_seq, rt_flags, rt_len ? rt_data : 0, rt_len);
}

static void ack_arrived(u32 ack) {
    /* Sequence numbers wrap, so compare as a signed difference. */
    if ((i32)(ack - snd_una) > 0) snd_una = ack;
    if (rt_pending) {
        u32 covers = rt_seq + rt_len;
        if (rt_flags & (TH_SYN | TH_FIN)) covers++;
        if ((i32)(ack - covers) >= 0) rt_pending = false;
    }
}

void tcp_input(ipv4_t src, const u8 *p, u16 len) {
    if (state == T_CLOSED || len < sizeof(tcp_t)) return;
    const tcp_t *t = (const tcp_t *)p;
    if (src != peer_ip) return;
    if (np_hs(t->dport) != local_port) return;

    u16 hlen = (u16)((t->offset >> 4) * 4);
    if (hlen < sizeof(tcp_t) || hlen > len) return;
    const u8 *data = p + hlen;
    u16 dlen = (u16)(len - hlen);

    if (t->flags & TH_RST) { state = T_DONE; rt_pending = false; return; }

    if (state == T_SYNSENT) {
        if ((t->flags & (TH_SYN | TH_ACK)) == (TH_SYN | TH_ACK)) {
            rcv_nxt = np_nl(t->seq) + 1;
            snd_nxt = np_nl(t->ack);
            snd_una = snd_nxt;
            rt_pending = false;                /* the SYN is acknowledged */
            state = T_OPEN;
            send_ack();
        }
        return;
    }

    if (t->flags & TH_ACK) ack_arrived(np_nl(t->ack));

    if (dlen) {
        if (np_nl(t->seq) == rcv_nxt) {
            u32 room = RXCAP - rxlen;
            u32 n = dlen < room ? dlen : room;
            if (n) { memcpy(rxbuf + rxlen, data, n); rxlen += n; }
            rcv_nxt += dlen;
            send_ack();
        } else {
            /* Out of order or already seen. Repeat the acknowledgement so the
               peer learns which byte we are actually waiting for. */
            send_ack();
        }
    }

    if (t->flags & TH_FIN) {
        rcv_nxt++;
        got_fin = true;
        send_ack();
        if (state == T_OPEN) {
            snd_nxt++;
            send_reliable(TH_FIN | TH_ACK, 0, 0);
            state = T_CLOSING;
        } else {
            state = T_DONE;
        }
    }
}

bool tcp_connect(ipv4_t ip, u16 port, u32 timeout_ms) {
    if (!rxbuf) {
        rxbuf = (u8 *)kmalloc(RXCAP);
        if (!rxbuf) return false;
    }
    peer_ip = ip;
    peer_port = port;
    local_port = (u16)(45000 + (timer_ticks() & 0x0FFF));
    snd_nxt = 0x5A4C5200u ^ (u32)(timer_ticks() * 2654435761u);
    snd_una = snd_nxt;
    rcv_nxt = 0;
    rxlen = 0;
    got_fin = false;
    rt_pending = false;
    rt_total = 0;
    state = T_SYNSENT;

    if (!send_reliable(TH_SYN, 0, 0)) { state = T_CLOSED; return false; }
    snd_nxt++;

    u64 deadline = timer_ticks() + (timeout_ms * timer_hz()) / 1000u;
    while (state == T_SYNSENT && timer_ticks() < deadline) {
        net_poll();
        tcp_pump();                            /* resends the SYN if needed */
    }

    if (state != T_OPEN) { state = T_CLOSED; return false; }
    return true;
}

bool tcp_send(const void *data, u16 len) {
    if (state != T_OPEN) return false;
    if (!send_reliable(TH_PSH | TH_ACK, data, len)) return false;
    snd_nxt += len;

    /* One segment in flight: wait for it before returning, so a caller that
       sends twice cannot overwrite the copy kept for retransmission. */
    u64 deadline = timer_ticks() + (timer_hz() * 8);
    while (rt_pending && state == T_OPEN && timer_ticks() < deadline) {
        net_poll();
        tcp_pump();
    }
    return !rt_pending;
}

u32 tcp_recv(u8 *out, u32 cap, u32 timeout_ms) {
    u64 deadline = timer_ticks() + (timeout_ms * timer_hz()) / 1000u;
    u64 quiet = timer_ticks() + (timer_hz() * 2);

    while (timer_ticks() < deadline) {
        u32 before = rxlen;
        net_poll();
        tcp_pump();
        if (rxlen > before) quiet = timer_ticks() + (timer_hz() * 2);
        if (got_fin) break;
        if (state == T_DONE) break;
        if (rxlen && timer_ticks() > quiet) break;
    }

    u32 n = rxlen < cap ? rxlen : cap;
    if (n) memcpy(out, rxbuf, n);
    return n;
}

void tcp_close(void) {
    if (state == T_OPEN) {
        snd_nxt++;
        send_reliable(TH_FIN | TH_ACK, 0, 0);
        state = T_CLOSING;
        u64 deadline = timer_ticks() + timer_hz() * 2;
        while (state == T_CLOSING && timer_ticks() < deadline) {
            net_poll();
            tcp_pump();
        }
    }
    state = T_CLOSED;
    rt_pending = false;
}

bool tcp_connected(void) { return state == T_OPEN; }
