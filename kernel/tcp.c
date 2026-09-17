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
 * slow on a fat link and perfectly correct on any of them.
 *
 * Every segment is sent at a sequence number worked out before it goes on
 * the wire, and nothing here advances a counter afterwards. That is not
 * style. The card's interrupt runs this file's receive side, so the reply
 * to a segment can be handled between the instruction that sends it and the
 * next one; a `send, then add what it used` pair puts the addition on top of
 * a number the handshake has already settled, and the connection is then one
 * byte ahead of itself for the rest of its life. The server answers every
 * segment with the same acknowledgement, asking for a byte that has already
 * gone past, and the page never arrives.
 *
 * It only happens against a server that replies before the sending code
 * reaches its next line, which is any server on the same machine and no
 * server across the internet. So it looked like the loopback being odd. */
#include "tcp.h"
#include "net.h"
#include "netpriv.h"
#include "timer.h"
#include "printf.h"
#include "string.h"
#include "io.h"
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

/* Big enough to hold what arrives while nobody is reading. The window
   advertised is whatever is free in here, so a peer is told to stop rather
   than having its data thrown away. */
#define RXCAP 65536
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

/* What is free in the receive buffer, which is what the peer is told it may
   send. Advertising a number that is not the free space is how a stack ends
   up dropping data it has already promised to take. */
static u16 window_now(void) {
    u32 free = RXCAP - rxlen;
    return (u16)(free > 65535 ? 65535 : free);
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
    t->window = np_hs(window_now());
    t->csum = 0;
    t->urgent = 0;
    if (dlen) memcpy(seg + sizeof(tcp_t), data, dlen);

    u16 total = (u16)(sizeof(tcp_t) + dlen);
    t->csum = np_hs(tcp_checksum(net_ip(), peer_ip, seg, total));
    return np_ip_send(peer_ip, 6, seg, total);
}

/* Sends a segment and remembers it until it is acknowledged.
 *
 * The sequence number is passed in rather than read from snd_nxt, so a
 * caller settles what the segment occupies before it goes out. See the top
 * of the file for what happens when it is settled afterwards. */
static bool send_reliable(u32 seq, u8 flags, const void *data, u16 dlen) {
    if (dlen > sizeof(rt_data)) return false;

    rt_seq = seq;
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
            /* Take what there is room for, and acknowledge only that.
               Acknowledging the whole segment and keeping part of it told
               the peer the rest had arrived, so it was never sent again and
               the hole was never filled: the body came back with a piece
               missing out of the middle and nothing said so. A peer is
               allowed to be told a prefix was taken; it is not allowed to be
               told bytes arrived that were thrown away. */
            u32 room = RXCAP - rxlen;
            u32 n = dlen < room ? dlen : room;
            if (n) { memcpy(rxbuf + rxlen, data, n); rxlen += n; rcv_nxt += n; }
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
            u32 seq = snd_nxt;
            snd_nxt = seq + 1;                 /* the FIN takes one */
            state = T_CLOSING;
            send_reliable(seq, TH_FIN | TH_ACK, 0, 0);
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

    /* The SYN occupies one sequence number, and that is settled here rather
       than after the send: the SYN and ACK can come back inside the call
       below, and the handler then sets snd_nxt from what the peer
       acknowledged. An increment after the fact lands on top of that. */
    u32 iss = snd_nxt;
    snd_nxt = iss + 1;
    if (!send_reliable(iss, TH_SYN, 0, 0)) { state = T_CLOSED; return false; }

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

    u32 seq = snd_nxt;
    snd_nxt = seq + len;
    if (!send_reliable(seq, TH_PSH | TH_ACK, data, len)) {
        snd_nxt = seq;
        return false;
    }

    /* One segment in flight: wait for it before returning, so a caller that
       sends twice cannot overwrite the copy kept for retransmission. */
    u64 deadline = timer_ticks() + (timer_hz() * 8);
    while (rt_pending && state == T_OPEN && timer_ticks() < deadline) {
        net_poll();
        tcp_pump();
    }
    return !rt_pending;
}

/* Hands over what has arrived and takes it out of the buffer.
 *
 * It used to copy from the front and leave the buffer as it was, so the
 * second call returned the same bytes as the first. A caller that loops
 * until it has the whole body got that body repeated until it ran out of
 * room, which is what the terminal's own fetch was doing, and a caller that
 * reads once was limited to whatever fitted in here. Neither could carry a
 * page of any size.
 *
 * It returns as soon as there is anything rather than waiting for the answer
 * to go quiet, because the caller is the one that knows how much it wants. */
u32 tcp_recv(u8 *out, u32 cap, u32 timeout_ms) {
    u64 deadline = timer_ticks() + (timeout_ms * timer_hz()) / 1000u;

    while (rxlen == 0 && timer_ticks() < deadline) {
        net_poll();
        tcp_pump();
        if (got_fin || state == T_DONE || state == T_CLOSED) break;
    }

    /* The card's interrupt appends to this buffer, so taking from the front
       and shifting the rest down has to happen with nothing else running. */
    bool were_on = interrupts_enabled();
    if (were_on) cli();

    u32 n = rxlen < cap ? rxlen : cap;
    if (n) {
        memcpy(out, rxbuf, n);
        rxlen -= n;
        if (rxlen) memmove(rxbuf, rxbuf + n, rxlen);
    }

    if (were_on) sti();

    /* Taking bytes out opens the window again. Say so, or a peer that filled
       it sits waiting for a probe it need never have sent. */
    if (n && state == T_OPEN) send_ack();
    return n;
}

/* Whether anything more can arrive. A read of nothing means the answer is
   over only when this is true; otherwise it means nothing came in time. */
bool tcp_ended(void) {
    return rxlen == 0 && (got_fin || state == T_DONE || state == T_CLOSED);
}

void tcp_close(void) {
    if (state == T_OPEN) {
        u32 seq = snd_nxt;
        snd_nxt = seq + 1;
        state = T_CLOSING;
        send_reliable(seq, TH_FIN | TH_ACK, 0, 0);
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
