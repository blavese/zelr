/* A TCP client, with retransmission, and more than one of it.
 *
 * It used to be one connection, and that was not a simplification of the
 * interface -- it was file level state. `tcp_connect` had nowhere to put a
 * second connection, and `tcp_input` had nothing to match an arriving
 * segment against except the only connection there was. A page and its
 * pictures were fetched strictly one after another, nothing could ask the
 * network for anything while a page loaded, and two programs could not both
 * be online: the second was told the machine was busy.
 *
 * Everything that was a file level variable is a field of a connection now,
 * and the segment that arrives is matched to one of them by the port it is
 * addressed to. The rest of the file is what it was.
 *
 * The send window is still one segment per connection: this waits for each
 * piece to be acknowledged before sending the next. That is slow on a fat
 * link and perfectly correct on any of them, and it is a per connection
 * limit now rather than a limit on the machine.
 *
 * Every segment is sent at a sequence number worked out before it goes on
 * the wire, and nothing here advances a counter afterwards. That is not
 * style. The receive side can run between the instruction that sends a
 * segment and the next one; a `send, then add what it used` pair puts the
 * addition on top of a number the handshake has already settled, and the
 * connection is then one byte ahead of itself for the rest of its life. The
 * server answers every segment with the same acknowledgement, asking for a
 * byte that has already gone past, and the page never arrives.
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

/* Big enough to hold what arrives while nobody is reading. The window
   advertised is whatever is free in here, so a peer is told to stop rather
   than having its data thrown away. */
#define RXCAP 65536

typedef struct {
    u16 sport, dport;
    u32 seq, ack;
    u8  offset, flags;
    u16 window, csum, urgent;
} __attribute__((packed)) tcp_t;

typedef enum { T_CLOSED, T_SYNSENT, T_OPEN, T_CLOSING, T_DONE } tstate_t;

typedef struct {
    bool used;                    /* this slot belongs to somebody */
    volatile tstate_t state;

    ipv4_t peer_ip;
    u16    peer_port, local_port;

    volatile u32  snd_nxt, snd_una, rcv_nxt;
    volatile bool got_fin;

    /* The one segment in flight, kept so it can be sent again. */
    u8   rt_data[1400];
    u16  rt_len;
    u32  rt_seq;
    u8   rt_flags;
    bool rt_pending;
    u64  rt_sent_at;
    u32  rt_timeout_ms;
    int  rt_tries;

    u8   *rxbuf;                  /* kept between connections on this slot */
    volatile u32 rxlen;
} tcpc;

static tcpc conns[TCP_MAX];

/* Walked rather than picked, so no two connections close together share one.
   Started off the clock so that two boots of the same machine do not open
   with the same number. One counter for the machine, because a port has to
   be unique across every connection this machine has rather than within
   one of them. */
static u16 next_port;

/* Totals since boot, across every connection. A count that went with the
   connection would answer "how is this transfer going" and /sys/net is
   asking "how is this machine's link". */
static u32 rst_seen, ooo_seen, rt_total;

u32 tcp_resets(void)       { return rst_seen; }
u32 tcp_out_of_order(void) { return ooo_seen; }
u32 tcp_retransmits(void)  { return rt_total; }

u32 tcp_open_count(void) {
    u32 n = 0;
    for (int i = 0; i < TCP_MAX; i++)
        if (conns[i].used && conns[i].state == T_OPEN) n++;
    return n;
}

/* A handle is an index, and every entry point checks it. A caller that has
   closed a connection and kept the number is the ordinary case rather than
   an exotic one, so this has to be cheap and it has to be everywhere. */
static tcpc *slot(int h) {
    if (h < 0 || h >= TCP_MAX) return 0;
    if (!conns[h].used) return 0;
    return &conns[h];
}

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

/* What is free in this connection's receive buffer, which is what its peer
   is told it may send. Advertising a number that is not the free space is
   how a stack ends up dropping data it has already promised to take. */
static u16 window_now(const tcpc *c) {
    u32 free = RXCAP - c->rxlen;
    return (u16)(free > 65535 ? 65535 : free);
}

/* Puts one segment on the wire. Does not touch the retransmit slot. */
static bool emit(tcpc *c, u32 seq, u8 flags, const void *data, u16 dlen) {
    u8 seg[1500];
    if (sizeof(tcp_t) + dlen > sizeof(seg)) return false;

    tcp_t *t = (tcp_t *)seg;
    t->sport = np_hs(c->local_port);
    t->dport = np_hs(c->peer_port);
    t->seq = np_hl(seq);
    t->ack = np_hl(c->rcv_nxt);
    t->offset = 5 << 4;
    t->flags = flags;
    t->window = np_hs(window_now(c));
    t->csum = 0;
    t->urgent = 0;
    if (dlen) memcpy(seg + sizeof(tcp_t), data, dlen);

    u16 total = (u16)(sizeof(tcp_t) + dlen);
    t->csum = np_hs(tcp_checksum(net_ip(), c->peer_ip, seg, total));
    return np_ip_send(c->peer_ip, 6, seg, total);
}

/* Sends a segment and remembers it until it is acknowledged.
 *
 * The sequence number is passed in rather than read from snd_nxt, so a
 * caller settles what the segment occupies before it goes out. See the top
 * of the file for what happens when it is settled afterwards. */
static bool send_reliable(tcpc *c, u32 seq, u8 flags, const void *data, u16 dlen) {
    if (dlen > sizeof(c->rt_data)) return false;

    c->rt_seq = seq;
    c->rt_flags = flags;
    c->rt_len = dlen;
    if (dlen) memcpy(c->rt_data, data, dlen);
    c->rt_pending = true;
    c->rt_tries = 0;
    c->rt_timeout_ms = RTO_MIN_MS;
    c->rt_sent_at = timer_ticks();

    return emit(c, c->rt_seq, flags, data, dlen);
}

/* A bare acknowledgement carries no sequence space, so it is never resent:
   if it is lost the peer simply sends its data again. */
static bool send_ack(tcpc *c) { return emit(c, c->snd_nxt, TH_ACK, 0, 0); }

static void pump_one(tcpc *c) {
    if (!c->rt_pending || c->state == T_CLOSED) return;

    u64 elapsed_ticks = timer_ticks() - c->rt_sent_at;
    u32 elapsed_ms = (u32)(elapsed_ticks * 1000u / timer_hz());
    if (elapsed_ms < c->rt_timeout_ms) return;

    if (c->rt_tries >= MAX_RETRIES) {
        c->state = T_DONE;                    /* the peer is not answering */
        c->rt_pending = false;
        return;
    }

    c->rt_tries++;
    rt_total++;
    c->rt_timeout_ms *= 2;
    if (c->rt_timeout_ms > RTO_MAX_MS) c->rt_timeout_ms = RTO_MAX_MS;
    c->rt_sent_at = timer_ticks();
    emit(c, c->rt_seq, c->rt_flags, c->rt_len ? c->rt_data : 0, c->rt_len);
}

/* Every open connection, because a loop waiting on one of them is the only
   thing running and the others still have timers. A download on one
   connection used to be the only thing keeping any of them alive; now it
   keeps all of them alive, which is the point of having more than one. */
void tcp_pump(void) {
    for (int i = 0; i < TCP_MAX; i++)
        if (conns[i].used) pump_one(&conns[i]);
}

static void ack_arrived(tcpc *c, u32 ack) {
    /* Sequence numbers wrap, so compare as a signed difference. */
    if ((i32)(ack - c->snd_una) > 0) c->snd_una = ack;
    if (c->rt_pending) {
        u32 covers = c->rt_seq + c->rt_len;
        if (c->rt_flags & (TH_SYN | TH_FIN)) covers++;
        if ((i32)(ack - covers) >= 0) c->rt_pending = false;
    }
}

/* Which connection a segment belongs to.
 *
 * By the port it is addressed to, which is the field this machine chose and
 * made unique. The peer's address is checked as well rather than trusted:
 * a segment from somewhere else carrying a local port that happens to match
 * is not this connection, and acting on it would be taking a stranger's
 * word for where a stream is up to. */
static tcpc *demux(ipv4_t src, const tcp_t *t) {
    u16 dport = np_hs(t->dport);
    u16 sport = np_hs(t->sport);
    for (int i = 0; i < TCP_MAX; i++) {
        tcpc *c = &conns[i];
        if (!c->used || c->state == T_CLOSED) continue;
        if (c->local_port != dport) continue;
        if (c->peer_ip != src || c->peer_port != sport) continue;
        return c;
    }
    return 0;
}

void tcp_input(ipv4_t src, const u8 *p, u16 len) {
    if (len < sizeof(tcp_t)) return;
    const tcp_t *t = (const tcp_t *)p;

    tcpc *c = demux(src, t);
    if (!c) return;

    u16 hlen = (u16)((t->offset >> 4) * 4);
    if (hlen < sizeof(tcp_t) || hlen > len) return;
    const u8 *data = p + hlen;
    u16 dlen = (u16)(len - hlen);
    u32 seq = np_nl(t->seq);

    if (t->flags & TH_RST) { rst_seen++; c->state = T_DONE; c->rt_pending = false; return; }

    if (c->state == T_SYNSENT) {
        if ((t->flags & (TH_SYN | TH_ACK)) == (TH_SYN | TH_ACK)) {
            c->rcv_nxt = np_nl(t->seq) + 1;
            c->snd_nxt = np_nl(t->ack);
            c->snd_una = c->snd_nxt;
            c->rt_pending = false;             /* the SYN is acknowledged */
            c->state = T_OPEN;
            send_ack(c);
        }
        return;
    }

    if (t->flags & TH_ACK) ack_arrived(c, np_nl(t->ack));

    if (dlen) {
        if (seq == c->rcv_nxt) {
            /* Take what there is room for, and acknowledge only that.
               Acknowledging the whole segment and keeping part of it told
               the peer the rest had arrived, so it was never sent again and
               the hole was never filled: the body came back with a piece
               missing out of the middle and nothing said so. A peer is
               allowed to be told a prefix was taken; it is not allowed to be
               told bytes arrived that were thrown away.

               How full the buffer is and what is in it are one fact and are
               changed together, with nothing else running. tcp_recv does the
               same on its side. Both of them are tasks now rather than one
               task and one interrupt handler, so neither is atomic against
               the other by construction, and the timer is what would come
               between them. */
            bool were_on = interrupts_enabled();
            if (were_on) cli();
            u32 room = RXCAP - c->rxlen;
            u32 n = dlen < room ? dlen : room;
            if (n) { memcpy(c->rxbuf + c->rxlen, data, n); c->rxlen += n; c->rcv_nxt += n; }
            if (were_on) sti();
            send_ack(c);
        } else {
            /* Out of order or already seen. Repeat the acknowledgement so the
               peer learns which byte we are actually waiting for. */
            ooo_seen++;
            send_ack(c);
        }
    }

    /* A FIN is the end of the data, and it is only the end once the data
     * is all here.
     *
     * It sits at the sequence number after whatever the segment carried, so
     * it counts only when that was the byte this end was waiting for. This
     * used to be acted on whichever segment it arrived in and whether or not
     * anything before it was missing, which is a connection ending with a
     * hole in the middle of what was being read -- and nothing says so. The
     * reader is told the answer is complete and hands back a body that stops
     * partway through, at a different length every time, with no error
     * anywhere. A two hundred kilobyte page came back whole about half the
     * time and short the rest.
     *
     * Out of order is not rare enough to ignore. One segment overtaking
     * another inside the host's own network stack is all it takes, and the
     * last segment of a body is the one carrying the FIN.
     *
     * Refusing it is enough on its own: the acknowledgement this end keeps
     * repeating names the byte it is still waiting for, the peer sends it
     * again, and the FIN that follows arrives in order. */
    if ((t->flags & TH_FIN) && !c->got_fin && seq + dlen == c->rcv_nxt) {
        c->rcv_nxt++;
        c->got_fin = true;
        send_ack(c);
        if (c->state == T_OPEN) {
            u32 ours = c->snd_nxt;             /* not seq: that is the peer's */
            c->snd_nxt = ours + 1;             /* the FIN takes one */
            c->state = T_CLOSING;
            send_reliable(c, ours, TH_FIN | TH_ACK, 0, 0);
        } else {
            c->state = T_DONE;
        }
    }
}

/* A slot nobody is using, with its buffer allocated.
 *
 * The buffer is kept once it exists rather than freed with the connection:
 * sixty four kilobytes is a real allocation and a page that opens six
 * connections one after another would otherwise take it from the heap six
 * times over. */
static int take_slot(void) {
    for (int i = 0; i < TCP_MAX; i++) {
        tcpc *c = &conns[i];
        if (c->used) continue;
        if (!c->rxbuf) {
            c->rxbuf = (u8 *)kmalloc(RXCAP);
            if (!c->rxbuf) return -1;
        }
        c->used = true;
        return i;
    }
    return -1;
}

int tcp_open(ipv4_t ip, u16 port, u32 timeout_ms) {
    if (!next_port) next_port = (u16)(45000 + (timer_ticks() & 0x0FFF));

    int h = take_slot();
    if (h < 0) return -1;
    tcpc *c = &conns[h];

    c->peer_ip = ip;
    c->peer_port = port;

    /* A port no open connection is using.
     *
     * It used to be worked out from the clock, which gives a different one
     * every time as long as the clock has moved. Two connections inside the
     * same tick got the same port, and the peer still has the last one in
     * its books: the new handshake looks like an old connection turning up
     * again and is dropped, so connecting takes the full timeout and then
     * fails.
     *
     * Nothing did that by hand. A redirect does: the answer arrives, the
     * connection closes and the next one opens in the same instant. So
     * following a redirect worked or did not depending on where in a tick
     * it happened to land.
     *
     * With several connections at once the counter walking on is no longer
     * enough on its own, because it wraps: a machine that has opened
     * sixteen thousand connections comes back round to one that is still
     * up. So the ones in use are checked, and the counter walks past them. */
    for (int guard = 0; guard < 65536; guard++) {
        if (next_port < 45000 || next_port >= 61000) next_port = 45000;
        u16 want = next_port++;
        bool taken = false;
        for (int i = 0; i < TCP_MAX; i++)
            if (conns[i].used && i != h && conns[i].local_port == want &&
                conns[i].state != T_CLOSED) taken = true;
        if (!taken) { c->local_port = want; break; }
    }

    c->snd_nxt = 0x5A4C5200u ^ (u32)(timer_ticks() * 2654435761u) ^ (u32)(h * 0x9E3779B9u);
    c->snd_una = c->snd_nxt;
    c->rcv_nxt = 0;
    c->rxlen = 0;
    c->got_fin = false;
    c->rt_pending = false;
    c->state = T_SYNSENT;

    /* The SYN occupies one sequence number, and that is settled here rather
       than after the send: the SYN and ACK can come back inside the call
       below, and the handler then sets snd_nxt from what the peer
       acknowledged. An increment after the fact lands on top of that. */
    u32 iss = c->snd_nxt;
    c->snd_nxt = iss + 1;
    if (!send_reliable(c, iss, TH_SYN, 0, 0)) {
        c->state = T_CLOSED;
        c->used = false;
        return -1;
    }

    u64 deadline = timer_ticks() + (timeout_ms * timer_hz()) / 1000u;
    while (c->state == T_SYNSENT && timer_ticks() < deadline) {
        net_poll();
        tcp_pump();                            /* resends the SYN if needed */
    }

    if (c->state != T_OPEN) {
        c->state = T_CLOSED;
        c->used = false;
        return -1;
    }
    return h;
}

bool tcp_send(int h, const void *data, u16 len) {
    tcpc *c = slot(h);
    if (!c || c->state != T_OPEN) return false;

    u32 seq = c->snd_nxt;
    c->snd_nxt = seq + len;
    if (!send_reliable(c, seq, TH_PSH | TH_ACK, data, len)) {
        c->snd_nxt = seq;
        return false;
    }

    /* One segment in flight: wait for it before returning, so a caller that
       sends twice cannot overwrite the copy kept for retransmission. */
    u64 deadline = timer_ticks() + (timer_hz() * 8);
    while (c->rt_pending && c->state == T_OPEN && timer_ticks() < deadline) {
        net_poll();
        tcp_pump();
    }
    return !c->rt_pending;
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
u32 tcp_recv(int h, u8 *out, u32 cap, u32 timeout_ms) {
    tcpc *c = slot(h);
    if (!c) return 0;

    u64 deadline = timer_ticks() + (timeout_ms * timer_hz()) / 1000u;

    /* Once before the buffer is looked at, always.
     *
     * The loop below only runs when there is nothing waiting, which during a
     * download is almost never: the reader takes what has arrived and comes
     * straight back for more. So on a fast transfer this function used to
     * return without ever touching the card, and the only thing left moving
     * frames out of the queue and acknowledgements onto the wire was the
     * background task, ten times a second. The peer fills the window it was
     * last told about and waits, and a body that should have taken a second
     * arrives in pieces or not at all. */
    net_poll();

    while (c->rxlen == 0 && timer_ticks() < deadline) {
        net_poll();
        tcp_pump();
        if (c->got_fin || c->state == T_DONE || c->state == T_CLOSED) break;
    }

    /* The receive side appends to this buffer, so taking from the front and
       shifting the rest down has to happen with nothing else running. */
    bool were_on = interrupts_enabled();
    if (were_on) cli();

    u32 n = c->rxlen < cap ? c->rxlen : cap;
    if (n) {
        memcpy(out, c->rxbuf, n);
        c->rxlen -= n;
        if (c->rxlen) memmove(c->rxbuf, c->rxbuf + n, c->rxlen);
    }

    if (were_on) sti();

    /* Taking bytes out opens the window again. Say so, or a peer that filled
       it sits waiting for a probe it need never have sent. */
    if (n && c->state == T_OPEN) send_ack(c);
    return n;
}

/* Whether anything more can arrive. A read of nothing means the answer is
   over only when this is true; otherwise it means nothing came in time. */
bool tcp_ended(int h) {
    const tcpc *c = slot(h);
    if (!c) return true;
    return c->rxlen == 0 && (c->got_fin || c->state == T_DONE || c->state == T_CLOSED);
}

void tcp_close(int h) {
    tcpc *c = slot(h);
    if (!c) return;

    if (c->state == T_OPEN) {
        u32 seq = c->snd_nxt;
        c->snd_nxt = seq + 1;
        c->state = T_CLOSING;
        send_reliable(c, seq, TH_FIN | TH_ACK, 0, 0);
        u64 deadline = timer_ticks() + timer_hz() * 2;
        while (c->state == T_CLOSING && timer_ticks() < deadline) {
            net_poll();
            tcp_pump();
        }
    }
    c->state = T_CLOSED;
    c->rt_pending = false;

    /* The slot goes back, and the buffer stays with it. A handle held past
       this point finds a closed slot rather than somebody else's
       connection, because `used` is what a handle is checked against. */
    c->used = false;
    c->rxlen = 0;
}

bool tcp_connected(int h) {
    const tcpc *c = slot(h);
    return c && c->state == T_OPEN;
}

int tcp_state_code(int h) {
    const tcpc *c = slot(h);
    return c ? (int)c->state : (int)T_CLOSED;
}
