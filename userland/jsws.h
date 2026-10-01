#pragma once
/* The page's WebSocket, on the protocol in wsock.h.
 *
 * Included from jsdom.h. Made with new WebSocket(address, protocols): it
 * says CONNECTING at once and connects on the browser's next pass, as a
 * browser's does after the code that made it has run; then open, a message
 * for each that comes (text as a string, binary as a Blob or an ArrayBuffer
 * as binaryType says), error when it failed, and close with the code, the
 * reason and whether it ended cleanly. send() and close() as the standard
 * has them. The browser asks every socket on each pass, from jsdom_timers,
 * and wakes for it a twentieth of a second at most while one is open.
 *
 * A page has JD_SOCKETS at once: the machine has six sockets for every
 * program together, and one held open is one fewer for everything else. A
 * page past that gets one that fails, as a server that cannot be reached.
 * The cookies for the address go with the upgrade (the browser's jar,
 * jsdom_websockets_with), and the page's origin as Origin.
 *
 * Not done: bufferedAmount is always 0 (a send goes out in the call), and
 * extensions and protocol are what the server agreed, never more. */
#include "wsock.h"

#define JD_SOCKETS 4

typedef struct {
    jobj  *self;                 /* the page's WebSocket; null when the slot is free */
    wsock  w;
    char   offered[256];         /* the subprotocols asked for, comma separated */
    char   address[URL_TEXT];
    int    started;              /* the connection has been tried */
} jdws;

static jdws jd_ws[JD_SOCKETS];
static jstr *jd_k_wsslot, *jd_k_wsstate, *jd_k_wsbinary, *jd_k_wsproto;
static jobj *jd_p_ws;

static int (*jd_ws_cookies)(const char *address, char *out, int cap);

/* Why one failed, on the page's console, where a browser says it too. */
static void jd_ws_say(const char *why) {
    if (!js_print_hook) return;
    js_print_hook("WebSocket: ", 11);
    js_print_hook(why, (u32)w_len(why));
    js_print_hook("\n", 1);
}

void jsdom_websockets_with(int (*fn)(const char *, char *, int)) { jd_ws_cookies = fn; }

static int jd_ws_slot(jobj *o) {
    jval v = jd_kept(o, jd_k_wsslot);
    int s = v.t == JS_NUM ? (int)v.num : -1;
    return s >= 0 && s < JD_SOCKETS && jd_ws[s].self == o ? s : -1;
}

static void jd_ws_state(jobj *o, int st) { jd_keep(o, jd_k_wsstate, js_num(st)); }

static void jd_ws_simple(jobj *o, const char *type) {
    jobj *ev = jd_new_event(0, type, 0, 0);
    if (!ev) return;
    js_set(&jd_J, ev, "isTrusted", js_bool(1));
    jd_dispatch_to(ev, js_from_obj(o), js_from_obj(o));
}

static void jd_ws_closed(jobj *o, int code, const char *reason, int clean) {
    jd_ws_state(o, WS_CLOSED);
    jobj *ev = jd_new_event(jd_evkind("CloseEvent"), "close", 0, 0);
    if (!ev) return;
    js_set(&jd_J, ev, "isTrusted", js_bool(1));
    js_set(&jd_J, ev, "wasClean", js_bool(clean));
    js_set(&jd_J, ev, "code", js_num(code));
    js_set(&jd_J, ev, "reason", js_from_str(js_str(&jd_J, reason ? reason : "")));
    jd_dispatch_to(ev, js_from_obj(o), js_from_obj(o));
}

/* The slot given back, and the page told it is over: error first when it
   failed, then close. */
static void jd_ws_end(int s, int failed, int code, const char *reason, int clean) {
    jobj *o = jd_ws[s].self;
    jd_ws[s].self = 0;
    ws_drop(&jd_ws[s].w);
    if (!o) return;
    if (failed) jd_ws_simple(o, "error");
    jd_ws_closed(o, code, reason, clean);
}

/* The connection itself, on the pass after the page asked for it. */
static void jd_ws_begin(jval arg) {
    int s = arg.t == JS_NUM ? (int)arg.num : -1;
    if (s < 0 || s >= JD_SOCKETS || !jd_ws[s].self || jd_ws[s].started) return;
    jdws *d = &jd_ws[s];
    d->started = 1;
    char cookies[CK_VALUE];
    cookies[0] = 0;
    if (jd_ws_cookies) jd_ws_cookies(d->address, cookies, sizeof(cookies));
    jstr *origin = jd_page_origin(&jd_J);
    if (!ws_start(&d->w, d->address, d->offered, origin ? origin->s : "", cookies)) {
        jd_ws_say(d->w.why);
        jd_ws_end(s, 1, 1006, "", 0);
    }
}

/* Each socket asked what has happened since the last pass: how many things
   the page was told. */
static int jd_ws_pump(void) {
    int told = 0;
    for (int s = 0; s < JD_SOCKETS; s++) {
        jdws *d = &jd_ws[s];
        if (!d->self || !d->started) continue;
        for (int k = 0; k < 64 && d->self; k++) {
            ws_event ev;
            if (!ws_pump(&d->w, &ev, d->offered)) break;
            jobj *o = d->self;
            told++;
            if (ev.type == WSE_OPEN) {
                jd_ws_state(o, WS_OPEN);
                jd_keep(o, jd_k_wsproto, js_from_str(js_str(&jd_J, d->w.protocol)));
                jd_ws_simple(o, "open");
            } else if (ev.type == WSE_MESSAGE) {
                jval data;
                if (!ev.binary) {
                    data = js_from_str(js_str_n(&jd_J, (const char *)ev.data, ev.len));
                } else {
                    jval bt = jd_kept(o, jd_k_wsbinary);
                    jstr *bytes = js_str_n(&jd_J, (const char *)ev.data, ev.len);
                    if (bt.t == JS_STR && js_str_is(bt.str, "arraybuffer")) {
                        jobj *buf = ta_new_buffer(&jd_J, jd_J.p_buffer, ev.len);
                        if (buf) {
                            u8 *p = ta_bytes(buf);
                            for (u32 i = 0; i < ev.len; i++) p[i] = ev.data[i];
                        }
                        data = buf ? js_from_obj(buf) : js_null();
                    } else {
                        jobj *b = jd_blob_make(&jd_J, 0, bytes, js_str(&jd_J, ""));
                        data = b ? js_from_obj(b) : js_null();
                    }
                }
                jobj *me = jd_new_event(jd_evkind("MessageEvent"), "message", 0, 0);
                if (!me) continue;
                js_set(&jd_J, me, "isTrusted", js_bool(1));
                js_set(&jd_J, me, "data", data);
                jstr *origin = 0;
                const char *a = d->address;
                int n = 0;
                if (w_starts_fold(a, "wss://")) n = 6;
                else if (w_starts_fold(a, "ws://")) n = 5;
                while (a[n] && a[n] != '/' && a[n] != '?') n++;
                origin = js_str_n(&jd_J, a, (u32)n);
                js_set(&jd_J, me, "origin", js_from_str(origin));
                js_set(&jd_J, me, "lastEventId", js_from_str(js_str(&jd_J, "")));
                js_set(&jd_J, me, "source", js_null());
                js_set(&jd_J, me, "ports", js_from_obj(js_array(&jd_J)));
                jd_dispatch_to(me, js_from_obj(o), js_from_obj(o));
            } else if (ev.type == WSE_ERROR) {
                jd_ws_say(d->w.why);
                jd_ws_end(s, 1, 1006, "", 0);
            } else if (ev.type == WSE_CLOSE) {
                jd_ws_end(s, 0, ev.code, ev.reason, ev.clean);
            }
            js_drain(&jd_J);
        }
    }
    return told;
}

static int jd_ws_open_count(void) {
    int n = 0;
    for (int s = 0; s < JD_SOCKETS; s++) if (jd_ws[s].self) n++;
    return n;
}

/* Every socket closed with the page: nothing is told, as nothing is left
   to tell. */
static void jd_ws_reset(void) {
    for (int s = 0; s < JD_SOCKETS; s++) {
        if (jd_ws[s].self && jd_ws[s].w.state == WS_OPEN) ws_close(&jd_ws[s].w, 1001, "");
        ws_drop(&jd_ws[s].w);
        jd_ws[s].self = 0;
        jd_ws[s].started = 0;
    }
}

/* A subprotocol name is a token: printable ASCII without separators. */
static int jd_ws_token(const jstr *s) {
    if (!s || !s->len) return 0;
    for (u32 i = 0; i < s->len; i++) {
        u8 c = (u8)s->s[i];
        if (c <= 0x20 || c >= 0x7F) return 0;
        for (const char *p = "()<>@,;:\\\"/[]?={}"; *p; p++) if (c == (u8)*p) return 0;
    }
    return 1;
}

static void jd_ws_failed_later(jval arg) {
    if (!js_is_obj(arg)) return;
    jd_ws_simple(arg.obj, "error");
    jd_ws_closed(arg.obj, 1006, "", 0);
}

static jval nat_ws_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "a WebSocket is made with new", J->error_line);
    if (n < 1) return js_throw(J, JS_ERR_TYPE, "a WebSocket needs an address", J->error_line);
    jstr *given = js_to_str(J, a[0]);
    if (J->sig != JS_OK) return js_undef();
    jstr *whole = jd_resolve_str(J, given->s);
    if (!whole) whole = given;
    /* http: and https: are taken as ws: and wss:, as the standard now has
       it; anything else is not an address a WebSocket can have. */
    char addr[URL_TEXT];
    int w = 0;
    const char *p = whole->s;
    if (w_starts_fold(p, "https://")) { w_copy(addr, sizeof(addr), "wss://", sizeof(addr)); w = 6; p += 8; }
    else if (w_starts_fold(p, "http://")) { w_copy(addr, sizeof(addr), "ws://", sizeof(addr)); w = 5; p += 7; }
    else if (!w_starts_fold(p, "ws://") && !w_starts_fold(p, "wss://"))
        return js_throw_dom(J, "SyntaxError", "a WebSocket address is ws: or wss:");
    for (; *p && *p != '#' && w < URL_TEXT - 1; p++) addr[w++] = *p;
    addr[w] = 0;

    char offered[256];
    int o = 0;
    offered[0] = 0;
    if (n > 1 && a[1].t != JS_UNDEF) {
        jval list = a[1];
        u32 len = 1;
        int arr = js_is_obj(list) && list.obj->kind == JO_ARRAY;
        if (arr) len = js_len_of(J, list);
        for (u32 i = 0; i < len && J->sig == JS_OK; i++) {
            jstr *pr = js_to_str(J, arr ? js_get_index(J, list, i) : list);
            if (J->sig != JS_OK) return js_undef();
            if (!jd_ws_token(pr)) return js_throw_dom(J, "SyntaxError", "that is not a subprotocol name");
            for (u32 j = 0; j < i; j++) {
                jstr *q = js_to_str(J, js_get_index(J, list, j));
                if (js_str_eq(q, pr)) return js_throw_dom(J, "SyntaxError", "a subprotocol named twice");
            }
            if (o + (int)pr->len + 3 >= (int)sizeof(offered)) break;
            if (o) { offered[o++] = ','; offered[o++] = ' '; }
            for (u32 k = 0; k < pr->len; k++) offered[o++] = pr->s[k];
            offered[o] = 0;
        }
    }

    jobj *me = t.obj;
    jd_keep(me, jd_k_url, js_from_str(js_str(J, addr)));
    jd_keep(me, jd_k_wsbinary, js_from_str(js_str(J, "blob")));
    jd_keep(me, jd_k_wsproto, js_from_str(js_str(J, "")));
    jd_ws_state(me, WS_CONNECTING);
    int slot = -1;
    for (int s = 0; s < JD_SOCKETS; s++) if (!jd_ws[s].self) { slot = s; break; }
    if (slot < 0) {
        jd_ws_say("a page has four WebSockets at most");
        jd_later_native(jd_ws_failed_later, t, 1);
        return t;
    }
    jdws *d = &jd_ws[slot];
    ws_init(&d->w);
    d->self = me;
    d->started = 0;
    w_copy(d->address, sizeof(d->address), addr, sizeof(d->address));
    w_copy(d->offered, sizeof(d->offered), offered, sizeof(d->offered));
    jd_keep(me, jd_k_wsslot, js_num(slot));
    jd_later_native(jd_ws_begin, js_num(slot), 1);
    return t;
}

static jval nat_ws_state(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (!js_is_obj(t)) return js_undef();
    jval v = jd_kept(t.obj, jd_k_wsstate);
    return v.t == JS_NUM ? v : js_num(WS_CLOSED);
}

static jval nat_ws_url(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return js_is_obj(t) ? jd_kept(t.obj, jd_k_url) : js_undef();
}

static jval nat_ws_protocol(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return js_is_obj(t) ? jd_kept(t.obj, jd_k_wsproto) : js_undef();
}

static jval nat_ws_extensions(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    return js_from_str(js_str(J, ""));
}

static jval nat_ws_buffered(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_num(0);
}

static jval nat_ws_binary(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return js_is_obj(t) ? jd_kept(t.obj, jd_k_wsbinary) : js_undef();
}

static jval nat_ws_set_binary(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return js_undef();
    jstr *v = js_to_str(J, js_arg(a, n, 0));
    if (v && (js_str_is(v, "blob") || js_str_is(v, "arraybuffer"))) jd_keep(t.obj, jd_k_wsbinary, js_from_str(v));
    return js_undef();
}

static jval nat_ws_send(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    jval st = jd_kept(t.obj, jd_k_wsstate);
    if (st.t == JS_NUM && (int)st.num == WS_CONNECTING)
        return js_throw_dom(J, "InvalidStateError", "a WebSocket cannot send before it is open");
    int s = jd_ws_slot(t.obj);
    if (s < 0 || jd_ws[s].w.state != WS_OPEN) return js_undef();
    jval v = js_arg(a, n, 0);
    if (v.t == JS_STR || !js_is_obj(v)) {
        jstr *text = js_to_str(J, v);
        if (J->sig != JS_OK) return js_undef();
        ws_send(&jd_ws[s].w, 0, (const u8 *)text->s, text->len);
        return js_undef();
    }
    jobj *parts = js_array(J);
    if (!parts) return js_undef();
    js_arr_push(J, parts, v);
    jstr *bytes = jd_blob_join(J, js_from_obj(parts));
    if (J->sig != JS_OK || !bytes) return js_undef();
    ws_send(&jd_ws[s].w, 1, (const u8 *)bytes->s, bytes->len);
    return js_undef();
}

static jval nat_ws_close(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    int code = 0;
    jstr *reason = 0;
    if (n > 0 && a[0].t != JS_UNDEF) {
        double c = js_trunc(js_to_num(J, a[0]));
        if (c != 1000 && (c < 3000 || c > 4999))
            return js_throw_dom(J, "InvalidAccessError", "a close code is 1000 or from 3000 to 4999");
        code = (int)c;
    }
    if (n > 1 && a[1].t != JS_UNDEF) {
        reason = js_to_str(J, a[1]);
        if (J->sig != JS_OK) return js_undef();
        if (reason->len > 123) return js_throw_dom(J, "SyntaxError", "a close reason is 123 bytes at most");
    }
    jval st = jd_kept(t.obj, jd_k_wsstate);
    int state = st.t == JS_NUM ? (int)st.num : WS_CLOSED;
    if (state == WS_CLOSING || state == WS_CLOSED) return js_undef();
    int s = jd_ws_slot(t.obj);
    if (state == WS_CONNECTING || s < 0 || jd_ws[s].w.state != WS_OPEN) {
        /* Not yet open: it fails, and says so afterwards. */
        jd_ws_state(t.obj, WS_CLOSING);
        if (s >= 0) { jd_ws[s].self = 0; ws_drop(&jd_ws[s].w); }
        jd_later_native(jd_ws_failed_later, t, 1);
        return js_undef();
    }
    char why[124];
    w_copy(why, sizeof(why), reason ? reason->s : "", sizeof(why));
    ws_close(&jd_ws[s].w, code, why);
    jd_ws_state(t.obj, WS_CLOSING);
    return js_undef();
}

static void jd_setup_ws(jctx *J) {
    jd_k_wsslot = js_sym_new(J, "socket", 6);
    jd_k_wsstate = js_sym_new(J, "readyState", 10);
    jd_k_wsbinary = js_sym_new(J, "binaryType", 10);
    jd_k_wsproto = js_sym_new(J, "protocol", 8);
    jd_p_ws = jd_interface(J, "WebSocket", jd_p[JI_EVENTTARGET], nat_ws_ctor, 1);
    jd_accessor(J, jd_p_ws, "readyState", nat_ws_state, 0);
    jd_accessor(J, jd_p_ws, "url", nat_ws_url, 0);
    jd_accessor(J, jd_p_ws, "protocol", nat_ws_protocol, 0);
    jd_accessor(J, jd_p_ws, "extensions", nat_ws_extensions, 0);
    jd_accessor(J, jd_p_ws, "bufferedAmount", nat_ws_buffered, 0);
    jd_accessor(J, jd_p_ws, "binaryType", nat_ws_binary, nat_ws_set_binary);
    jd_method(J, jd_p_ws, "send", nat_ws_send, 1);
    jd_method(J, jd_p_ws, "close", nat_ws_close, 0);
    static const char *const WS[] = { "CONNECTING", "OPEN", "CLOSING", "CLOSED", 0 };
    jd_consts(J, jd_p_ws, WS, 0);
    jd_consts(J, jd_ctor_of(jd_p_ws), WS, 0);
}
