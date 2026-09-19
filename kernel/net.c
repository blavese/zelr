/* A small IPv4 stack: ethernet, ARP, IP, ICMP, UDP, DHCP and DNS.
 *
 * Everything here is polled or interrupt driven from the NIC and processed
 * inline; there are no sockets and no buffering beyond one frame at a time,
 * which keeps the whole path short enough to follow. */
#include "net.h"
#include "netdev.h"
#include "timer.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "netpriv.h"
#include "tcp.h"
#include "sched.h"

#define ETH_P_IP   0x0800
#define ETH_P_ARP  0x0806

#define IP_ICMP 1
#define IP_UDP  17

static inline u16 hs(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u32 hl(u32 v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}

typedef struct {
    u8  dst[6], src[6];
    u16 type;
} __attribute__((packed)) eth_t;

typedef struct {
    u16 htype, ptype;
    u8  hlen, plen;
    u16 oper;
    u8  sha[6]; u32 spa;
    u8  tha[6]; u32 tpa;
} __attribute__((packed)) arp_t;

typedef struct {
    u8  ver_ihl, tos;
    u16 len, id, frag;
    u8  ttl, proto;
    u16 csum;
    u32 src, dst;
} __attribute__((packed)) ip_t;

typedef struct {
    u8  type, code;
    u16 csum, id, seq;
} __attribute__((packed)) icmp_t;

typedef struct {
    u16 sport, dport, len, csum;
} __attribute__((packed)) udp_t;

typedef struct {
    u8  op, htype, hlen, hops;
    u32 xid;
    u16 secs, flags;
    u32 ciaddr, yiaddr, siaddr, giaddr;
    u8  chaddr[16];
    u8  sname[64], file[128];
    u32 cookie;
    u8  options[312];
} __attribute__((packed)) dhcp_t;

static u8    my_mac[6];
static ipv4_t my_ip, my_mask, my_gw, my_dns;
static bool  bound = false;

static const u8 BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* --- arp cache ---------------------------------------------------------- */

#define ARP_N 16
static struct { ipv4_t ip; u8 mac[6]; bool used; } arp_cache[ARP_N];

static void arp_store(ipv4_t ip, const u8 *mac) {
    for (int i = 0; i < ARP_N; i++)
        if (arp_cache[i].used && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    for (int i = 0; i < ARP_N; i++)
        if (!arp_cache[i].used) {
            arp_cache[i].used = true;
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    arp_cache[0].ip = ip;                       /* evict the first */
    memcpy(arp_cache[0].mac, mac, 6);
}

static bool arp_get(ipv4_t ip, u8 *out) {
    for (int i = 0; i < ARP_N; i++)
        if (arp_cache[i].used && arp_cache[i].ip == ip) {
            memcpy(out, arp_cache[i].mac, 6);
            return true;
        }
    return false;
}

/* --- checksums ---------------------------------------------------------- */

static u16 checksum(const void *data, u32 len, u32 start) {
    const u8 *p = (const u8 *)data;
    u32 sum = start;
    while (len > 1) { sum += (u32)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (u32)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

/* --- transmit ----------------------------------------------------------- */

static u8 txbuf[1600];

static void eth_send(const u8 *dst, u16 type, const void *payload, u16 len) {
    if ((u32)sizeof(eth_t) + len > sizeof(txbuf)) return;
    eth_t *e = (eth_t *)txbuf;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, my_mac, 6);
    e->type = hs(type);
    memcpy(txbuf + sizeof(eth_t), payload, len);
    netdev_send(txbuf, (u16)(sizeof(eth_t) + len));
}

static void arp_send(u16 oper, const u8 *target_mac, ipv4_t target_ip) {
    arp_t a;
    a.htype = hs(1);
    a.ptype = hs(ETH_P_IP);
    a.hlen = 6; a.plen = 4;
    a.oper = hs(oper);
    memcpy(a.sha, my_mac, 6);
    a.spa = hl(my_ip);
    memcpy(a.tha, target_mac, 6);
    a.tpa = hl(target_ip);
    eth_send(oper == 1 ? BCAST : target_mac, ETH_P_ARP, &a, sizeof(a));
}

static bool resolve_mac(ipv4_t ip, u8 *out, u32 timeout_ms) {
    /* Anything off our subnet goes via the gateway. */
    if (my_mask && ((ip & my_mask) != (my_ip & my_mask)) && my_gw) ip = my_gw;

    if (arp_get(ip, out)) return true;

    u64 deadline = timer_ticks() + (timeout_ms * timer_hz()) / 1000u;
    for (int tries = 0; tries < 4; tries++) {
        arp_send(1, BCAST, ip);
        u64 wait = timer_ticks() + (timer_hz() / 4);
        while (timer_ticks() < wait) {
            netdev_poll();
            if (arp_get(ip, out)) return true;
            if (timer_ticks() > deadline) return false;
        }
        if (timer_ticks() > deadline) break;
    }
    return arp_get(ip, out);
}

static u16 ip_id = 1;

static bool ip_send(ipv4_t dst, u8 proto, const void *payload, u16 len) {
    u8 pkt[1500];
    /* Checked before the ARP, so an oversized payload costs nothing and can
       never be copied past the end of the staging buffer. */
    if ((u32)sizeof(ip_t) + len > sizeof(pkt)) return false;

    u8 mac[6];
    if (!resolve_mac(dst, mac, 2000)) return false;

    ip_t *ip = (ip_t *)pkt;
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->len = hs((u16)(sizeof(ip_t) + len));
    ip->id = hs(ip_id++);
    ip->frag = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum = 0;
    ip->src = hl(my_ip);
    ip->dst = hl(dst);
    ip->csum = hs(checksum(ip, sizeof(ip_t), 0));

    memcpy(pkt + sizeof(ip_t), payload, len);
    eth_send(mac, ETH_P_IP, pkt, (u16)(sizeof(ip_t) + len));
    return true;
}

u16  np_hs(u16 v) { return hs(v); }
u32  np_hl(u32 v) { return hl(v); }
u32  np_nl(u32 v) { return hl(v); }
bool np_ip_send(ipv4_t dst, u8 proto, const void *payload, u16 len) {
    return ip_send(dst, proto, payload, len);
}

/* --- receive ------------------------------------------------------------ */

static volatile bool  ping_got;
static volatile u16   ping_seq;

static volatile bool  dhcp_offer_got, dhcp_ack_got;
static volatile ipv4_t dhcp_offer_ip, dhcp_server_ip;
static volatile ipv4_t dhcp_mask, dhcp_gw, dhcp_dns;
static u32 dhcp_xid;

static volatile bool  dns_got;
static volatile ipv4_t dns_result;
static volatile u16   dns_id;

static void handle_arp(const u8 *p, u16 len) {
    if (len < sizeof(arp_t)) return;
    const arp_t *a = (const arp_t *)p;
    ipv4_t spa = hl(a->spa), tpa = hl(a->tpa);

    arp_store(spa, a->sha);

    if (hs(a->oper) == 1 && my_ip && tpa == my_ip)
        arp_send(2, a->sha, spa);            /* somebody is asking for us */
}

static void handle_icmp(const ip_t *ip, const u8 *p, u16 len) {
    if (len < sizeof(icmp_t)) return;
    const icmp_t *ic = (const icmp_t *)p;

    if (ic->type == 8) {                     /* echo request: answer it */
        u8 reply[1500 - sizeof(ip_t)];
        if (len > sizeof(reply)) return;
        memcpy(reply, p, len);
        icmp_t *r = (icmp_t *)reply;
        r->type = 0;
        r->csum = 0;
        r->csum = hs(checksum(reply, len, 0));
        ip_send(hl(ip->src), IP_ICMP, reply, len);
    } else if (ic->type == 0) {              /* echo reply to ours */
        if (hs(ic->seq) == ping_seq) ping_got = true;
    }
}

static const u8 *dhcp_option(const dhcp_t *d, u8 want, u8 *len_out) {
    const u8 *o = d->options;
    const u8 *end = d->options + sizeof(d->options);
    while (o < end && *o != 0xFF) {
        u8 code = *o++;
        if (code == 0) continue;
        if (o >= end) break;
        u8 l = *o++;
        if (o + l > end) break;
        if (code == want) { if (len_out) *len_out = l; return o; }
        o += l;
    }
    return 0;
}

static void handle_dhcp(const u8 *p, u16 len) {
    if (len < 240) return;
    const dhcp_t *d = (const dhcp_t *)p;
    if (d->xid != dhcp_xid) return;

    u8 l = 0;
    const u8 *t = dhcp_option(d, 53, &l);
    if (!t || l < 1) return;

    if (t[0] == 2) {                          /* OFFER */
        dhcp_offer_ip = hl(d->yiaddr);
        const u8 *sid = dhcp_option(d, 54, &l);
        dhcp_server_ip = (sid && l >= 4)
            ? ((ipv4_t)sid[0] << 24 | (ipv4_t)sid[1] << 16 | (ipv4_t)sid[2] << 8 | sid[3])
            : hl(d->siaddr);
        dhcp_offer_got = true;
    } else if (t[0] == 5) {                   /* ACK */
        dhcp_offer_ip = hl(d->yiaddr);
        const u8 *m = dhcp_option(d, 1, &l);
        if (m && l >= 4) dhcp_mask = (ipv4_t)m[0] << 24 | (ipv4_t)m[1] << 16 | (ipv4_t)m[2] << 8 | m[3];
        const u8 *g = dhcp_option(d, 3, &l);
        if (g && l >= 4) dhcp_gw = (ipv4_t)g[0] << 24 | (ipv4_t)g[1] << 16 | (ipv4_t)g[2] << 8 | g[3];
        const u8 *n = dhcp_option(d, 6, &l);
        if (n && l >= 4) dhcp_dns = (ipv4_t)n[0] << 24 | (ipv4_t)n[1] << 16 | (ipv4_t)n[2] << 8 | n[3];
        dhcp_ack_got = true;
    }
}

static void handle_dns_reply(const u8 *p, u16 len);

static void handle_udp(const ip_t *ip, const u8 *p, u16 len) {
    if (len < sizeof(udp_t)) return;
    const udp_t *u = (const udp_t *)p;
    u16 dport = hs(u->dport);
    const u8 *body = p + sizeof(udp_t);
    u16 blen = (u16)(len - sizeof(udp_t));

    if (dport == 68) handle_dhcp(body, blen);
    else if (dport >= 40000) handle_dns_reply(body, blen);
}

static void handle_ip(const u8 *p, u16 len) {
    if (len < sizeof(ip_t)) return;
    const ip_t *ip = (const ip_t *)p;
    if ((ip->ver_ihl >> 4) != 4) return;

    u16 ihl = (u16)((ip->ver_ihl & 0x0F) * 4);
    u16 total = hs(ip->len);
    if (ihl < sizeof(ip_t) || total < ihl || total > len) return;

    ipv4_t dst = hl(ip->dst);
    /* Accept our own address, broadcast, and anything while unbound so the
       DHCP reply is not thrown away before we have an address. */
    if (my_ip && dst != my_ip && dst != 0xFFFFFFFFu && bound) return;

    const u8 *body = p + ihl;
    u16 blen = (u16)(total - ihl);

    if (ip->proto == IP_ICMP) handle_icmp(ip, body, blen);
    else if (ip->proto == IP_UDP) handle_udp(ip, body, blen);
    else if (ip->proto == 6) tcp_input(hl(ip->src), body, blen);
}

void net_receive(const u8 *frame, u16 len) {
    if (len < sizeof(eth_t)) return;
    const eth_t *e = (const eth_t *)frame;
    const u8 *body = frame + sizeof(eth_t);
    u16 blen = (u16)(len - sizeof(eth_t));

    switch (hs(e->type)) {
        case ETH_P_ARP: handle_arp(body, blen); break;
        case ETH_P_IP:  handle_ip(body, blen);  break;
        default: break;
    }
}

void net_poll(void) { netdev_poll(); }

/* --- udp ---------------------------------------------------------------- */

static bool udp_send_to(ipv4_t dst, u16 sport, u16 dport,
                        const void *data, u16 len, bool broadcast) {
    u8 pkt[1400];
    if (len + sizeof(udp_t) > sizeof(pkt)) return false;

    udp_t *u = (udp_t *)pkt;
    u->sport = hs(sport);
    u->dport = hs(dport);
    u->len   = hs((u16)(sizeof(udp_t) + len));
    u->csum  = 0;                            /* optional in IPv4, left off */
    memcpy(pkt + sizeof(udp_t), data, len);

    u16 total = (u16)(sizeof(udp_t) + len);

    if (broadcast) {
        u8 ipbuf[1500];
        ip_t *ip = (ip_t *)ipbuf;
        ip->ver_ihl = 0x45; ip->tos = 0;
        ip->len = hs((u16)(sizeof(ip_t) + total));
        ip->id = hs(ip_id++); ip->frag = 0;
        ip->ttl = 64; ip->proto = IP_UDP; ip->csum = 0;
        ip->src = hl(my_ip);
        ip->dst = hl(0xFFFFFFFFu);
        ip->csum = hs(checksum(ip, sizeof(ip_t), 0));
        memcpy(ipbuf + sizeof(ip_t), pkt, total);
        eth_send(BCAST, ETH_P_IP, ipbuf, (u16)(sizeof(ip_t) + total));
        return true;
    }
    return ip_send(dst, IP_UDP, pkt, total);
}

bool net_udp_send(ipv4_t dst, u16 sport, u16 dport, const void *data, u16 len) {
    return udp_send_to(dst, sport, dport, data, len, false);
}

/* --- dhcp --------------------------------------------------------------- */

static void dhcp_build(dhcp_t *d, u8 type, ipv4_t req_ip, ipv4_t server) {
    memset(d, 0, sizeof(*d));
    d->op = 1; d->htype = 1; d->hlen = 6;
    d->xid = dhcp_xid;
    d->flags = hs(0x8000);                   /* please broadcast the reply */
    memcpy(d->chaddr, my_mac, 6);
    d->cookie = hl(0x63825363u);

    u8 *o = d->options;
    *o++ = 53; *o++ = 1; *o++ = type;        /* message type */
    if (req_ip) {
        *o++ = 50; *o++ = 4;
        *o++ = (u8)(req_ip >> 24); *o++ = (u8)(req_ip >> 16);
        *o++ = (u8)(req_ip >> 8);  *o++ = (u8)req_ip;
    }
    if (server) {
        *o++ = 54; *o++ = 4;
        *o++ = (u8)(server >> 24); *o++ = (u8)(server >> 16);
        *o++ = (u8)(server >> 8);  *o++ = (u8)server;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;   /* mask, router, dns */
    *o++ = 0xFF;
}

bool net_dhcp(u32 timeout_ms) {
    if (!netdev_up()) return false;

    dhcp_xid = 0x5A4C5200u ^ (u32)timer_ticks();
    dhcp_offer_got = dhcp_ack_got = false;
    dhcp_mask = dhcp_gw = dhcp_dns = 0;
    my_ip = 0; bound = false;

    dhcp_t d;
    dhcp_build(&d, 1, 0, 0);                 /* DISCOVER */
    udp_send_to(0xFFFFFFFFu, 68, 67, &d, sizeof(d), true);

    u64 deadline = timer_ticks() + (timeout_ms * timer_hz()) / 1000u;
    while (!dhcp_offer_got && timer_ticks() < deadline) net_poll();
    if (!dhcp_offer_got) return false;

    dhcp_build(&d, 3, dhcp_offer_ip, dhcp_server_ip);   /* REQUEST */
    udp_send_to(0xFFFFFFFFu, 68, 67, &d, sizeof(d), true);

    while (!dhcp_ack_got && timer_ticks() < deadline) net_poll();
    if (!dhcp_ack_got) return false;

    my_ip   = dhcp_offer_ip;
    my_mask = dhcp_mask ? dhcp_mask : 0xFFFFFF00u;
    my_gw   = dhcp_gw;
    my_dns  = dhcp_dns;
    bound = true;
    return true;
}

/* --- asking for one without stopping to wait ----------------------------- */

/* An exchange takes seconds, and there are two places that start one: the
   machine at startup, and the button on the panel for when that did not
   work. Neither can afford to block, and both sharing one socket and one
   transaction id means they must not overlap, so the flag lives here with
   the socket rather than in either caller. */
static bool dhcp_asking;

bool net_dhcp_busy(void) { return dhcp_asking; }

static void dhcp_task(void) {
    net_dhcp(6000);
    dhcp_asking = false;
    task_exit();
}

void net_dhcp_start(void) {
    if (dhcp_asking || !netdev_up()) return;
    dhcp_asking = true;
    if (!task_create("dhcp", dhcp_task)) dhcp_asking = false;
}

/* --- icmp echo ---------------------------------------------------------- */

int net_ping(ipv4_t dst, u32 timeout_ms) {
    if (!netdev_up() || !my_ip) return -1;

    static u16 seq = 0;
    seq++;
    ping_seq = seq;
    ping_got = false;

    u8 buf[64];
    icmp_t *ic = (icmp_t *)buf;
    ic->type = 8; ic->code = 0; ic->csum = 0;
    ic->id = hs(0x4E59); ic->seq = hs(seq);
    for (u32 i = sizeof(icmp_t); i < sizeof(buf); i++) buf[i] = (u8)i;
    ic->csum = hs(checksum(buf, sizeof(buf), 0));

    u64 start = timer_ticks();
    if (!ip_send(dst, IP_ICMP, buf, sizeof(buf))) return -1;

    u64 deadline = start + (timeout_ms * timer_hz()) / 1000u;
    while (!ping_got && timer_ticks() < deadline) net_poll();
    if (!ping_got) return -1;

    u64 elapsed = timer_ticks() - start;
    return (int)((elapsed * 1000u) / timer_hz());
}

/* --- dns ---------------------------------------------------------------- */

static void handle_dns_reply(const u8 *p, u16 len) {
    if (len < 12) return;
    u16 id = (u16)((p[0] << 8) | p[1]);
    if (id != dns_id) return;

    u16 qd = (u16)((p[4] << 8) | p[5]);
    u16 an = (u16)((p[6] << 8) | p[7]);
    if (an == 0) { dns_got = true; dns_result = 0; return; }

    u16 off = 12;
    for (u16 q = 0; q < qd && off < len; q++) {          /* skip questions */
        while (off < len && p[off] != 0) off = (u16)(off + p[off] + 1);
        off = (u16)(off + 5);
    }
    for (u16 a = 0; a < an && off + 12 <= len; a++) {
        if ((p[off] & 0xC0) == 0xC0) off = (u16)(off + 2);
        else { while (off < len && p[off] != 0) off = (u16)(off + p[off] + 1); off++; }
        if (off + 10 > len) break;
        u16 type = (u16)((p[off] << 8) | p[off + 1]);
        u16 rdlen = (u16)((p[off + 8] << 8) | p[off + 9]);
        off = (u16)(off + 10);
        if (type == 1 && rdlen == 4 && off + 4 <= len) {
            dns_result = (ipv4_t)p[off] << 24 | (ipv4_t)p[off + 1] << 16 |
                         (ipv4_t)p[off + 2] << 8 | p[off + 3];
            dns_got = true;
            return;
        }
        off = (u16)(off + rdlen);
    }
    dns_got = true;
    dns_result = 0;
}

bool net_resolve(const char *host, ipv4_t *out, u32 timeout_ms) {
    if (!netdev_up() || !my_dns) return false;

    static u16 next_id = 0x1234;
    dns_id = next_id++;
    dns_got = false;
    dns_result = 0;

    u8 q[512];
    u32 n = 0;
    q[n++] = (u8)(dns_id >> 8); q[n++] = (u8)dns_id;
    q[n++] = 0x01; q[n++] = 0x00;            /* recursion desired */
    q[n++] = 0; q[n++] = 1;                  /* one question */
    q[n++] = 0; q[n++] = 0;
    q[n++] = 0; q[n++] = 0;
    q[n++] = 0; q[n++] = 0;

    /* name, as length-prefixed labels */
    const char *s = host;
    while (*s) {
        const char *dot = s;
        while (*dot && *dot != '.') dot++;
        u32 l = (u32)(dot - s);
        if (l == 0 || l > 63 || n + l + 1 >= sizeof(q)) return false;
        q[n++] = (u8)l;
        for (u32 i = 0; i < l; i++) q[n++] = (u8)s[i];
        s = *dot ? dot + 1 : dot;
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;                  /* A */
    q[n++] = 0; q[n++] = 1;                  /* IN */

    u16 sport = (u16)(40000 + (timer_ticks() & 0x3FF));

    /* Asked more than once, because this is one UDP datagram each way and
       either of them is allowed to go missing. A resolver client that sends
       one query and gives up when nothing comes back reports that a name
       does not exist, which is a different and much more alarming thing
       than a packet having been dropped, and it is the answer everything
       above this gets told. The ARP above does the same for the same
       reason.
     *
       The identifier stays the same across the attempts: a late answer to
       the first query is a perfectly good answer to the second. */
    u64 deadline = timer_ticks() + (timeout_ms * timer_hz()) / 1000u;
    u64 gap = (timer_hz() * 3) / 4;             /* three quarters of a second */

    for (int tries = 0; tries < 4 && !dns_got; tries++) {
        if (!net_udp_send(my_dns, sport, 53, q, (u16)n)) return false;

        u64 until = timer_ticks() + gap;
        if (until > deadline) until = deadline;
        while (!dns_got && timer_ticks() < until) net_poll();

        if (timer_ticks() >= deadline) break;
        gap *= 2;
    }

    if (!dns_got || !dns_result) return false;

    *out = dns_result;
    return true;
}

/* --- helpers ------------------------------------------------------------ */

ipv4_t net_parse_ip(const char *s) {
    u32 parts[4] = { 0, 0, 0, 0 };
    int idx = 0;
    bool any = false;
    for (const char *p = s; *p; p++) {
        if (*p >= '0' && *p <= '9') { parts[idx] = parts[idx] * 10 + (u32)(*p - '0'); any = true; }
        else if (*p == '.') { if (++idx > 3) return 0; }
        else return 0;
    }
    if (!any || idx != 3) return 0;
    for (int i = 0; i < 4; i++) if (parts[i] > 255) return 0;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

void net_format_ip(ipv4_t ip, char *out) {
    u32 a = (ip >> 24) & 0xFF, b = (ip >> 16) & 0xFF, c = (ip >> 8) & 0xFF, d = ip & 0xFF;
    u32 n = 0;
    u32 vals[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) {
        u32 v = vals[i];
        if (v >= 100) out[n++] = (char)('0' + v / 100);
        if (v >= 10)  out[n++] = (char)('0' + (v / 10) % 10);
        out[n++] = (char)('0' + v % 10);
        if (i < 3) out[n++] = '.';
    }
    out[n] = 0;
}

void net_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
    my_ip = my_mask = my_gw = my_dns = 0;
    bound = false;
    if (netdev_up()) memcpy(my_mac, netdev_mac(), 6);
}

bool  net_up(void)        { return netdev_up(); }
ipv4_t net_ip(void)       { return my_ip; }
ipv4_t net_gateway(void)  { return my_gw; }
ipv4_t net_netmask(void)  { return my_mask; }
ipv4_t net_dns(void)      { return my_dns; }
const u8 *net_mac(void)   { return my_mac; }
u32 net_rx_packets(void)  { return netdev_rx_count(); }
u32 net_tx_packets(void)  { return netdev_tx_count(); }
