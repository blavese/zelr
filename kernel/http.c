/* Just enough HTTP to ask a server for a page and keep the answer. */
#include "http.h"
#include "tcp.h"
#include "net.h"
#include "fs.h"
#include "heap.h"
#include "printf.h"
#include "string.h"

/* Bigger than the receive buffer underneath, on purpose: a page that fits
   in one is not a test of anything. */
#define BODY_CAP (256 * 1024)

/* Appends if the whole string fits, and reports whether it did. Refusing to
   truncate matters: half a request line is a request the server will answer
   in some other way, which is worse than not sending one. */
static bool append(char *buf, u32 cap, u32 *n, const char *s) {
    for (; *s; s++) {
        if (*n >= cap) return false;
        buf[(*n)++] = *s;
    }
    return true;
}

/* Splits "name" or "name:port" apart. A URL is allowed to carry a port and
   a server is allowed to listen on one, so a client that can only reach 80
   cannot be pointed at anything but a public website. */
static u16 split_port(const char *in, char *host, u32 cap) {
    u32 n = 0;
    u16 port = 0;
    const char *p = in;
    for (; *p && *p != ':'; p++)
        if (n + 1 < cap) host[n++] = *p;
    host[n] = 0;
    if (*p == ':') {
        u32 v = 0;
        for (p++; *p >= '0' && *p <= '9'; p++) v = v * 10 + (u32)(*p - '0');
        if (v > 0 && v < 65536) port = (u16)v;
    }
    return port ? port : 80;
}

int http_get(const char *spec, const char *path, const char *save_as) {
    char host[128];
    u16 port = split_port(spec, host, sizeof(host));

    ipv4_t ip = net_parse_ip(host);
    if (!ip && !net_resolve(host, &ip, 5000)) return HTTP_ERR_RESOLVE;

    char addr[20];
    net_format_ip(ip, addr);
    kprintf("connecting to %s (%s) port %d\n", host, addr, port);

    if (!tcp_connect(ip, port, 6000)) return HTTP_ERR_CONNECT;

    /* HTTP/1.0 with an explicit close, so the server ends the body by
       closing the connection and we do not have to parse chunked encoding.
       Every piece goes through the same bounded append, because guarding the
       variable parts and not the fixed ones still overflows: the trailer is
       fifty bytes that have to fit after whatever the caller supplied. */
    char req[512];
    u32 n = 0;
    bool fits = true;
    fits &= append(req, sizeof(req), &n, "GET ");
    fits &= append(req, sizeof(req), &n, path);
    fits &= append(req, sizeof(req), &n, " HTTP/1.0\r\nHost: ");
    fits &= append(req, sizeof(req), &n, spec);   /* name and port, as asked for */
    fits &= append(req, sizeof(req), &n,
                   "\r\nUser-Agent: zelr/" KERNEL_VERSION
                   "\r\nConnection: close\r\n\r\n");
    if (!fits) { tcp_close(); return HTTP_ERR_TOOLONG; }

    if (!tcp_send(req, (u16)n)) { tcp_close(); return HTTP_ERR_SEND; }

    u8 *buf = (u8 *)kmalloc(BODY_CAP);
    if (!buf) { tcp_close(); return HTTP_ERR_MEMORY; }

    /* Read until the server closes, rather than once. One read is whatever
       happened to have arrived by then, which for anything bigger than a
       segment or two is the front of the page and no sign that the rest is
       missing. */
    u32 got = 0;
    while (got < BODY_CAP) {
        u32 n = tcp_recv(buf + got, BODY_CAP - got, 10000);
        got += n;
        if (!n && tcp_ended()) break;
        if (!n) break;                     /* nothing in ten seconds */
    }
    tcp_close();

    if (got == 0) { kfree(buf); return HTTP_ERR_EMPTY; }

    /* Split the headers from the body at the blank line. */
    u32 body = 0;
    for (u32 i = 0; i + 3 < got; i++) {
        if (buf[i] == 13 && buf[i + 1] == 10 && buf[i + 2] == 13 && buf[i + 3] == 10) {
            body = i + 4;
            break;
        }
    }

    int status = 0;
    if (got > 12 && buf[0] == 'H') {
        status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
    }

    u32 blen = got - body;
    if (save_as && blen) fs_write(save_as, buf + body, blen);

    kprintf("status %d, %d bytes of headers, %d bytes of body\n", status, body, blen);
    if (save_as && blen) kprintf("saved to %s\n", save_as);
    else if (blen) {
        u32 show = blen < 400 ? blen : 400;
        kprintf("---\n");
        for (u32 i = 0; i < show; i++) kputc((char)buf[body + i]);
        if (blen > show) kprintf("\n... %d more bytes\n", blen - show);
        kprintf("\n---\n");
    }

    kfree(buf);
    return status ? status : HTTP_ERR_EMPTY;
}
