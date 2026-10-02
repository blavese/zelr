#pragma once
/* Sound and video for the page: MediaSource, SourceBuffer and TimeRanges,
 * and the <video> and <audio> elements playing what is appended, on the
 * pipeline in media.h (fragmented MP4 holding H.264 and AAC).
 *
 * Included from jsdom.h. A page plays the way the players on the web do it:
 * it makes a MediaSource, hands its blob: address to a <video> as its src,
 * waits for sourceopen, adds a SourceBuffer for each stream and appends the
 * segments it fetched itself. The element is then the standard's: play()
 * gives a promise, readyState climbs as what is buffered reaches past the
 * playing position, timeupdate comes four times a second, and seeking,
 * waiting and ended are told as they happen. The browser draws the latest
 * picture in the element's box (jsdom_media_picture) and the sound goes to
 * the card, which is the clock the pictures are shown by.
 *
 * Everything an event is told for is queued and told on the browser's next
 * pass (jd_media_pump, from jsdom_timers), as a browser queues it, never in
 * the middle of the script that caused it; appends and removals happen
 * there too, between updatestart and updateend.
 *
 * A page has JD_MEDIA elements with a pipeline at once (YouTube's watch page
 * makes seven) and JD_SBUFS source buffers among them. Only the first element to play sound has the card;
 * another plays on the clock with its sound left out.
 *
 * A src that names a file (MP4, not fragmented) is played from it: asked
 * for by ranges, a stretch a pass, as the pipeline wants it (media.h,
 * media_file_want), from the network through the browser, or from what a
 * blob: or data: address holds.
 *
 * Not done: sequence mode (taken, and played as segments), append windows
 * (kept, not applied), a playbackRate other than 1 (kept, not applied), and
 * text tracks. */
#include "aac.h"
#include "h264.h"
#include "media.h"

#define JD_MEDIA 16
#define JD_SBUFS 8
#define JD_MQ 96

enum { JM_NOTHING, JM_METADATA, JM_CURRENT, JM_FUTURE, JM_ENOUGH };
enum { JM_NET_EMPTY, JM_NET_IDLE, JM_NET_LOADING, JM_NET_NO_SOURCE };
enum { JM_MS_CLOSED, JM_MS_OPEN, JM_MS_ENDED };

typedef struct {
    jobj   *el;                  /* the element's object; 0 when the slot is free */
    int     node;
    jobj   *ms;                  /* the MediaSource attached, or 0 */
    int     ms_state;
    media_t m;
    int     ready, net, paused, seeking, waiting, ended_told, loaded_told;
    double  seek_to;
    int     last_update;         /* the tick of the last timeupdate */
    int     last_progress;
    int     vw, vh;              /* the size last told by resize */
    int     error;               /* MediaError's code, 0 for none */
    jobj   *promises[8];         /* play()s waiting for playing */
    int     npromises;
    int     pic_w, pic_h;        /* the latest frame, lent to the browser */
    u8     *pic;
    /* A file named as the src: its address, or the bytes a blob: or data:
       address held (in the region, reached from here). */
    char    url[URL_TEXT];
    jstr   *bytes;
    /* The stretch under way (the browser's id for it, plus one; 0 for
       none), and where it starts. */
    int     ask;
    long long ask_from;
} jdmedia;

typedef struct {
    jobj   *self;                /* 0 when free */
    jobj   *ms;
    int     slot, k;
    int     updating;
    u8     *data;                /* an append waiting for the next pass */
    long long n;
    int     removing;
    double  from, to;
    int     sequence;            /* mode is "sequence" */
    double  win_start, win_end;
} jdsbuf;

static jdmedia jd_media[JD_MEDIA];
static jdsbuf jd_sb[JD_SBUFS];

/* What is to be told on the next pass: an event for an element (node) or an
   object (to). Static, so what it holds is seen by the collector. */
static struct { jobj *to; int node; const char *type; } jd_mq[JD_MQ];
static int jd_mq_n;
static int jd_media_frames_new;     /* a frame the browser has not drawn */
static int jd_media_resized;        /* a size the layout has not had */

static jstr *jd_k_msslot, *jd_k_mslist, *jd_k_msactive, *jd_k_msdur, *jd_k_sbslot, *jd_k_trlist,
            *jd_k_mvol, *jd_k_mmuted, *jd_k_mrate, *jd_k_mdrate;
static jobj *jd_p_ms, *jd_p_sb, *jd_p_sblist, *jd_p_ranges, *jd_p_mediaerr;

static void jd_media_say(const char *why) {
    if (!js_print_hook) return;
    js_print_hook("media: ", 7);
    js_print_hook(why, (u32)w_len(why));
    js_print_hook("\n", 1);
}

static void jd_mq_add(jobj *to, int node, const char *type) {
    if (jd_mq_n >= JD_MQ) return;
    jd_mq[jd_mq_n].to = to;
    jd_mq[jd_mq_n].node = node;
    jd_mq[jd_mq_n].type = type;
    jd_mq_n++;
}

static void jd_media_tell(int s, const char *type) { jd_mq_add(0, jd_media[s].node, type); }

static void jd_obj_simple(jobj *o, const char *type) {
    jobj *ev = jd_new_event(0, type, 0, 0);
    if (!ev) return;
    js_set(&jd_J, ev, "isTrusted", js_bool(1));
    jd_dispatch_to(ev, js_from_obj(o), js_from_obj(o));
}

/* --- which element, which buffer ---------------------------------------------------------- */

static int jd_media_of_node(int node) {
    for (int s = 0; s < JD_MEDIA; s++) if (jd_media[s].el && jd_media[s].node == node) return s;
    return -1;
}

static int jd_media_of(jval t) {
    int node = jd_el_of(t);
    return node < 0 ? -1 : jd_media_of_node(node);
}

/* A slot for an element, made when it first needs one. */
static int jd_media_slot(int node) {
    int s = jd_media_of_node(node);
    if (s >= 0) return s;
    for (s = 0; s < JD_MEDIA; s++) if (!jd_media[s].el) break;
    if (s == JD_MEDIA) return -1;
    jdmedia *d = &jd_media[s];
    for (int i = 0; i < (int)sizeof(*d); i++) ((volatile u8 *)d)[i] = 0;
    d->el = jd_element(&jd_J, node);
    if (!d->el) return -1;
    d->node = node;
    d->paused = 1;
    media_open(&d->m);
    return s;
}

static int jd_ms_slot(jobj *ms) {
    jval v = jd_kept(ms, jd_k_msslot);
    int s = v.t == JS_NUM ? (int)v.num : -1;
    return s >= 0 && s < JD_MEDIA && jd_media[s].el && jd_media[s].ms == ms ? s : -1;
}

static int jd_sb_of(jval t) {
    if (!js_is_obj(t)) return -1;
    jval v = jd_kept(t.obj, jd_k_sbslot);
    int b = v.t == JS_NUM ? (int)v.num : -1;
    return b >= 0 && b < JD_SBUFS && jd_sb[b].self == t.obj ? b : -1;
}

static int jd_ms_is(jval v) {
    if (!js_is_obj(v) || !jd_p_ms) return 0;
    for (jobj *p = v.obj->proto; p; p = p->proto) if (p == jd_p_ms) return 1;
    return 0;
}

/* The MediaSource a blob: address was made for, or 0. */
static jobj *jd_ms_lookup(const char *url) {
    if (!jd_blob_urls || !jd_is_blob_url(url)) return 0;
    int len = 0;
    while (url[len] && url[len] != '#') len++;
    for (u32 i = 0; i + 1 < jd_blob_urls->len; i += 2) {
        jval u = jd_blob_urls->items[i];
        if (u.t != JS_STR || (int)u.str->len != len) continue;
        int same = 1;
        for (int k = 0; k < len && same; k++) if (u.str->s[k] != url[k]) same = 0;
        if (same && jd_ms_is(jd_blob_urls->items[i + 1])) return jd_blob_urls->items[i + 1].obj;
    }
    return 0;
}

/* --- types ---------------------------------------------------------------------------------- */

/* Whether a type is one this plays: MP4 holding H.264 at 8 bits in 4:2:0
   (avc1/avc3 with the baseline, main, extended or high profile) and AAC-LC.
   2 for a type with codecs that are all ours, 1 for MP4 named without
   them, 0 for anything else. */
static int jd_media_type(const char *t, int len) {
    int i = 0;
    while (i < len && t[i] == ' ') i++;
    int video = 0;
    if (len - i >= 9 && w_starts_fold(t + i, "video/mp4")) { video = 1; i += 9; }
    else if (len - i >= 9 && w_starts_fold(t + i, "audio/mp4")) i += 9;
    else if (len - i >= 9 && w_starts_fold(t + i, "audio/aac")) return 2;
    else return 0;
    while (i < len && t[i] == ' ') i++;
    if (i >= len) return 1;
    if (t[i] != ';') return 0;
    int codecs = 0;
    while (i < len) {
        while (i < len && (t[i] == ';' || t[i] == ' ')) i++;
        int ns = i;
        while (i < len && t[i] != '=' && t[i] != ';') i++;
        int is_codecs = i - ns == 6 && w_starts_fold(t + ns, "codecs");
        if (i >= len || t[i] != '=') continue;
        i++;
        int quoted = i < len && t[i] == '"';
        if (quoted) i++;
        while (i < len && (quoted ? t[i] != '"' : t[i] != ';')) {
            while (i < len && (t[i] == ' ' || t[i] == ',')) i++;
            int cs = i;
            while (i < len && t[i] != ',' && t[i] != '"' && t[i] != ';' && t[i] != ' ') i++;
            int cl = i - cs;
            if (!cl || !is_codecs) continue;
            const char *c = t + cs;
            int good = 0;
            if (cl >= 6 && (w_starts_fold(c, "avc1.") || w_starts_fold(c, "avc3."))) {
                /* The profile is the first two hex digits: 42 baseline, 4D
                   main, 58 extended, 64 high. */
                int p = (w_lower(c[5]) << 8) | w_lower(c[6]);
                good = video && (p == ('4' << 8 | '2') || p == ('4' << 8 | 'd') || p == ('5' << 8 | '8') ||
                                 p == ('6' << 8 | '4'));
            } else if (cl == 4 && (w_starts_fold(c, "avc1") || w_starts_fold(c, "avc3"))) good = video;
            else if (cl == 9 && w_starts_fold(c, "mp4a.40.2")) good = 1;
            else if (cl == 4 && w_starts_fold(c, "mp4a")) good = 1;
            if (!good) return 0;
            codecs++;
        }
        if (quoted && i < len) i++;
    }
    return codecs ? 2 : 1;
}

static int jd_media_type_str(jstr *s) { return s ? jd_media_type(s->s, (int)s->len) : 0; }

/* --- time ranges ---------------------------------------------------------------------------- */

static jval jd_ranges(jctx *J, const double *r, int n) {
    jobj *o = js_object_with(J, JO_PLAIN, jd_p_ranges);
    jobj *list = js_array(J);
    if (!o || !list) return js_undef();
    for (int i = 0; i < 2 * n; i++) js_arr_push(J, list, js_num(r[i]));
    jd_keep(o, jd_k_trlist, js_from_obj(list));
    return js_from_obj(o);
}

static jobj *jd_ranges_list(jval t) {
    if (!js_is_obj(t)) return 0;
    jval v = jd_kept(t.obj, jd_k_trlist);
    return js_is_obj(v) && v.obj->kind == JO_ARRAY ? v.obj : 0;
}

static jval nat_ranges_length(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    jobj *l = jd_ranges_list(t);
    return js_num(l ? l->len / 2 : 0);
}

static jval jd_ranges_at(jctx *J, jval t, jval *a, int n, int end) {
    jobj *l = jd_ranges_list(t);
    if (!l) return jd_illegal(J);
    double i = js_trunc(js_to_num(J, js_arg(a, n, 0)));
    if (!(i >= 0) || i >= l->len / 2) return js_throw_dom(J, "IndexSizeError", "there is no range of that number");
    return l->items[2 * (u32)i + (u32)end];
}

static jval nat_ranges_start(jctx *J, jval t, jval *a, int n) { return jd_ranges_at(J, t, a, n, 0); }
static jval nat_ranges_end(jctx *J, jval t, jval *a, int n) { return jd_ranges_at(J, t, a, n, 1); }

/* What every source of an element holds, the ranges they all cover. */
static int jd_media_buffered(int s, double *r, int max) {
    media_t *m = &jd_media[s].m;
    if (m->file) return media_buffered(m, 0, r, max);
    int n = -1;
    double acc[64];
    for (int k = 0; k < MEDIA_SOURCES; k++) {
        int used = 0;
        for (int b = 0; b < JD_SBUFS; b++) if (jd_sb[b].self && jd_sb[b].slot == s && jd_sb[b].k == k) used = 1;
        if (!used) continue;
        double mine[64];
        int c = media_buffered(m, k, mine, 32);
        if (n < 0) {
            for (int i = 0; i < 2 * c; i++) acc[i] = mine[i];
            n = c;
            continue;
        }
        double out[64];
        int w = 0;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < c && w < 32; j++) {
                double lo = acc[2 * i] > mine[2 * j] ? acc[2 * i] : mine[2 * j];
                double hi = acc[2 * i + 1] < mine[2 * j + 1] ? acc[2 * i + 1] : mine[2 * j + 1];
                if (hi > lo) { out[2 * w] = lo; out[2 * w + 1] = hi; w++; }
            }
        for (int i = 0; i < 2 * w; i++) acc[i] = out[i];
        n = w;
    }
    if (n < 0) n = 0;
    if (n > max) n = max;
    for (int i = 0; i < 2 * n; i++) r[i] = acc[i];
    return n;
}

/* --- the element's state, told -------------------------------------------------------------- */

static void jd_media_settle(int s, int ok, const char *name, const char *why) {
    jdmedia *d = &jd_media[s];
    for (int i = 0; i < d->npromises; i++) {
        jobj *p = d->promises[i];
        d->promises[i] = 0;
        if (!p) continue;
        if (ok) js_promise_settle(&jd_J, p, 1, js_undef());
        else {
            jobj *e = js_domexc_new(&jd_J, 0, js_str(&jd_J, why), js_str(&jd_J, name));
            js_promise_settle(&jd_J, p, 0, e ? js_from_obj(e) : js_undef());
        }
    }
    d->npromises = 0;
}

/* Whether the clock should run, and making it so. */
static void jd_media_clock(int s) {
    jdmedia *d = &jd_media[s];
    int run = !d->paused && d->ready >= JM_FUTURE && !d->seeking && !d->m.ended && !d->error;
    if (run && !d->m.playing) media_play(&d->m);
    else if (!run && d->m.playing) media_pause(&d->m);
}

static double jd_media_now(int s) {
    jdmedia *d = &jd_media[s];
    if (d->seeking) return d->seek_to;
    double t = d->m.playing ? media_clock(&d->m) : d->m.position;
    if (d->m.duration == d->m.duration && t > d->m.duration) t = d->m.duration;
    return t;
}

static void jd_media_fail(int s, int code, const char *why) {
    jdmedia *d = &jd_media[s];
    if (d->error) return;
    jd_media_say(why);
    d->error = code;
    d->net = code == 4 ? JM_NET_NO_SOURCE : JM_NET_IDLE;
    jd_media_settle(s, 0, code == 4 ? "NotSupportedError" : "AbortError", why);
    jd_media_tell(s, "error");
    jd_media_clock(s);
}

/* --- MediaSource ---------------------------------------------------------------------------- */

static void jd_list_put(jobj *list, jobj *sb, int add) {
    if (!list) return;
    jval v = jd_kept(list, jd_k_mslist);
    jobj *arr = js_is_obj(v) ? v.obj : 0;
    if (!arr) return;
    u32 n = arr->len;
    if (add) js_arr_push(&jd_J, arr, js_from_obj(sb));
    else {
        u32 w = 0;
        for (u32 i = 0; i < n; i++) if (!(js_is_obj(arr->items[i]) && arr->items[i].obj == sb)) arr->items[w++] = arr->items[i];
        arr->len = w;
    }
    /* Its members as indexed properties too, as a list's are. */
    for (u32 i = 0; i < n + 1; i++) {
        jstr *key = js_to_key(&jd_J, js_num(i));
        if (i < arr->len) js_put_prop_flags(&jd_J, list, key, arr->items[i], JP_ENUM);
        else js_delete_prop(list, key);
    }
}

static void jd_sb_drop(int b) {
    jdsbuf *x = &jd_sb[b];
    if (x->data) free(x->data);
    x->data = 0;
    x->n = 0;
    if (x->slot >= 0 && x->slot < JD_MEDIA && jd_media[x->slot].el) {
        media_source *src = &jd_media[x->slot].m.src[x->k];
        media_free_samples(src);
        if (src->pend) free(src->pend);
        for (int i = 0; i < (int)sizeof(*src); i++) ((volatile u8 *)src)[i] = 0;
    }
    x->self = 0;
    x->ms = 0;
}

/* The MediaSource let go of its element: closed, and its buffers gone. */
static void jd_ms_detach(int s) {
    jdmedia *d = &jd_media[s];
    jobj *ms = d->ms;
    if (!ms) return;
    for (int b = 0; b < JD_SBUFS; b++)
        if (jd_sb[b].self && jd_sb[b].ms == ms) {
            jval l = jd_kept(ms, jd_k_msactive);
            if (js_is_obj(l)) jd_list_put(l.obj, jd_sb[b].self, 0);
            l = jd_kept(ms, jd_k_mslist);
            if (js_is_obj(l)) { jd_list_put(l.obj, jd_sb[b].self, 0); jd_mq_add(l.obj, -1, "removesourcebuffer"); }
            jd_sb_drop(b);
        }
    d->ms = 0;
    d->ms_state = JM_MS_CLOSED;
    jd_keep(ms, jd_k_msslot, js_num(-1));
    jd_mq_add(ms, -1, "sourceclose");
}

static jval nat_ms_ctor(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (J->new_target.t == JS_UNDEF || !js_is_obj(t))
        return js_throw(J, JS_ERR_TYPE, "a MediaSource is made with new", J->error_line);
    jobj *list = js_object_with(J, JO_PLAIN, jd_p_sblist), *active = js_object_with(J, JO_PLAIN, jd_p_sblist);
    jobj *arr = js_array(J), *arr2 = js_array(J);
    if (!list || !active || !arr || !arr2) return js_undef();
    jd_keep(list, jd_k_mslist, js_from_obj(arr));
    jd_keep(active, jd_k_mslist, js_from_obj(arr2));
    jd_keep(t.obj, jd_k_mslist, js_from_obj(list));
    jd_keep(t.obj, jd_k_msactive, js_from_obj(active));
    jd_keep(t.obj, jd_k_msslot, js_num(-1));
    jd_keep(t.obj, jd_k_msdur, js_num(js_nan()));
    return t;
}

static jval nat_ms_supported(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *ty = jd_arg_str(J, a, n, 0);
    return js_bool(jd_media_type_str(ty) != 0);
}

static jval nat_ms_state(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return jd_illegal(J);
    int s = jd_ms_slot(t.obj);
    int st = s < 0 ? JM_MS_CLOSED : jd_media[s].ms_state;
    return jd_str(st == JM_MS_OPEN ? "open" : st == JM_MS_ENDED ? "ended" : "closed");
}

static jval nat_ms_buffers(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return js_is_obj(t) ? jd_kept(t.obj, jd_k_mslist) : js_undef();
}

static jval nat_ms_active(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    return js_is_obj(t) ? jd_kept(t.obj, jd_k_msactive) : js_undef();
}

static jval nat_ms_duration(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (!js_is_obj(t)) return js_undef();
    int s = jd_ms_slot(t.obj);
    if (s < 0) return js_num(js_nan());
    return js_num(jd_media[s].m.duration);
}

static int jd_ms_busy(jobj *ms) {
    for (int b = 0; b < JD_SBUFS; b++) if (jd_sb[b].self && jd_sb[b].ms == ms && jd_sb[b].updating) return 1;
    return 0;
}

static jval nat_ms_set_duration(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    double v = js_to_num(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    if (v < 0 || v != v) return js_throw(J, JS_ERR_TYPE, "a duration is a number, 0 or more", J->error_line);
    int s = jd_ms_slot(t.obj);
    if (s < 0 || jd_media[s].ms_state != JM_MS_OPEN)
        return js_throw_dom(J, "InvalidStateError", "the MediaSource is not open");
    if (jd_ms_busy(t.obj)) return js_throw_dom(J, "InvalidStateError", "a SourceBuffer is still updating");
    media_t *m = &jd_media[s].m;
    if (m->duration == v) return js_undef();
    m->duration = v;
    jd_media_tell(s, "durationchange");
    return js_undef();
}

static void jd_ms_open_again(int s) {
    jdmedia *d = &jd_media[s];
    if (d->ms_state != JM_MS_ENDED) return;
    d->ms_state = JM_MS_OPEN;
    d->m.eos = 0;
    d->m.flushed = 0;
    jd_mq_add(d->ms, -1, "sourceopen");
}

static jval nat_ms_add(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    jstr *ty = jd_arg_str(J, a, n, 0);
    if (!ty->len) return js_throw(J, JS_ERR_TYPE, "a SourceBuffer needs a type", J->error_line);
    if (!jd_media_type_str(ty)) return js_throw_dom(J, "NotSupportedError", "that type is not one this plays");
    int s = jd_ms_slot(t.obj);
    if (s < 0 || jd_media[s].ms_state != JM_MS_OPEN)
        return js_throw_dom(J, "InvalidStateError", "the MediaSource is not open");
    int k = -1, b = -1;
    for (int kk = 0; kk < MEDIA_SOURCES && k < 0; kk++) {
        int used = 0;
        for (int i = 0; i < JD_SBUFS; i++) if (jd_sb[i].self && jd_sb[i].slot == s && jd_sb[i].k == kk) used = 1;
        if (!used) k = kk;
    }
    for (int i = 0; i < JD_SBUFS && b < 0; i++) if (!jd_sb[i].self) b = i;
    if (k < 0 || b < 0) return js_throw_dom(J, "QuotaExceededError", "no room for another SourceBuffer");
    jobj *sb = js_object_with(J, JO_PLAIN, jd_p_sb);
    if (!sb) return js_undef();
    jdsbuf *x = &jd_sb[b];
    for (int i = 0; i < (int)sizeof(*x); i++) ((volatile u8 *)x)[i] = 0;
    x->self = sb;
    x->ms = t.obj;
    x->slot = s;
    x->k = k;
    x->win_end = 1.0 / 0.0;
    jd_keep(sb, jd_k_sbslot, js_num(b));
    jval l = jd_kept(t.obj, jd_k_mslist);
    if (js_is_obj(l)) { jd_list_put(l.obj, sb, 1); jd_mq_add(l.obj, -1, "addsourcebuffer"); }
    l = jd_kept(t.obj, jd_k_msactive);
    if (js_is_obj(l)) { jd_list_put(l.obj, sb, 1); jd_mq_add(l.obj, -1, "addsourcebuffer"); }
    return js_from_obj(sb);
}

static jval nat_ms_remove(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    int b = jd_sb_of(js_arg(a, n, 0));
    if (b < 0 || jd_sb[b].ms != t.obj) return js_throw_dom(J, "NotFoundError", "that SourceBuffer is not this MediaSource's");
    jobj *sb = jd_sb[b].self;
    if (jd_sb[b].updating) { jd_mq_add(sb, -1, "abort"); jd_mq_add(sb, -1, "updateend"); }
    jval l = jd_kept(t.obj, jd_k_msactive);
    if (js_is_obj(l)) { jd_list_put(l.obj, sb, 0); jd_mq_add(l.obj, -1, "removesourcebuffer"); }
    l = jd_kept(t.obj, jd_k_mslist);
    if (js_is_obj(l)) { jd_list_put(l.obj, sb, 0); jd_mq_add(l.obj, -1, "removesourcebuffer"); }
    jd_sb_drop(b);
    return js_undef();
}

static jval nat_ms_end(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    int s = jd_ms_slot(t.obj);
    if (s < 0 || jd_media[s].ms_state != JM_MS_OPEN)
        return js_throw_dom(J, "InvalidStateError", "the MediaSource is not open");
    if (jd_ms_busy(t.obj)) return js_throw_dom(J, "InvalidStateError", "a SourceBuffer is still updating");
    jstr *err = n > 0 && a[0].t != JS_UNDEF ? js_to_str(J, a[0]) : 0;
    if (err && !js_str_is(err, "network") && !js_str_is(err, "decode"))
        return js_throw(J, JS_ERR_TYPE, "endOfStream takes \"network\" or \"decode\"", J->error_line);
    jdmedia *d = &jd_media[s];
    d->ms_state = JM_MS_ENDED;
    jd_mq_add(t.obj, -1, "sourceended");
    if (err) {
        jd_media_fail(s, js_str_is(err, "network") ? 2 : 3, "the page ended its stream with an error");
        return js_undef();
    }
    double was = d->m.duration;
    media_end_of_stream(&d->m);
    if (d->m.duration != was) jd_media_tell(s, "durationchange");
    return js_undef();
}

static jval nat_ms_live_range(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return jd_illegal(J);
    int s = jd_ms_slot(t.obj);
    if (s < 0 || jd_media[s].ms_state != JM_MS_OPEN)
        return js_throw_dom(J, "InvalidStateError", "the MediaSource is not open");
    return js_undef();
}

static jval nat_sblist_length(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (!js_is_obj(t)) return js_num(0);
    jval v = jd_kept(t.obj, jd_k_mslist);
    return js_num(js_is_obj(v) ? v.obj->len : 0);
}

/* --- SourceBuffer --------------------------------------------------------------------------- */

static jval jd_sb_check(jctx *J, jval t, int *bp) {
    int b = jd_sb_of(t);
    *bp = b;
    if (b < 0) return js_throw_dom(J, "InvalidStateError", "this SourceBuffer has been removed");
    return js_undef();
}

static jval nat_sb_append(jctx *J, jval t, jval *a, int n) {
    int b;
    jd_sb_check(J, t, &b);
    if (b < 0) return js_undef();
    jdsbuf *x = &jd_sb[b];
    if (x->updating) return js_throw_dom(J, "InvalidStateError", "an append or a removal is still going on");
    jdmedia *d = &jd_media[x->slot];
    if (d->error) return js_throw_dom(J, "InvalidStateError", "the element has an error");
    const u8 *p;
    u32 len;
    if (!tx_bytes_of(js_arg(a, n, 0), &p, &len))
        return js_throw(J, JS_ERR_TYPE, "appendBuffer takes an ArrayBuffer or a view of one", J->error_line);
    jd_ms_open_again(x->slot);
    x->data = (u8 *)malloc(len ? len : 1);
    if (!x->data) return js_throw_dom(J, "QuotaExceededError", "no memory for that append");
    for (u32 i = 0; i < len; i++) x->data[i] = p[i];
    x->n = len;
    x->updating = 1;
    return js_undef();
}

static jval nat_sb_remove(jctx *J, jval t, jval *a, int n) {
    int b;
    jd_sb_check(J, t, &b);
    if (b < 0) return js_undef();
    jdsbuf *x = &jd_sb[b];
    if (x->updating) return js_throw_dom(J, "InvalidStateError", "an append or a removal is still going on");
    double from = js_to_num(J, js_arg(a, n, 0)), to = js_to_num(J, js_arg(a, n, 1));
    if (J->sig != JS_OK) return js_undef();
    media_t *m = &jd_media[x->slot].m;
    if (!(from >= 0) || (m->duration == m->duration && from > m->duration) || !(to > from))
        return js_throw(J, JS_ERR_TYPE, "a removal is from a time to a later one, inside the duration", J->error_line);
    jd_ms_open_again(x->slot);
    x->removing = 1;
    x->from = from;
    x->to = to;
    x->updating = 1;
    return js_undef();
}

static jval nat_sb_abort(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int b;
    jd_sb_check(J, t, &b);
    if (b < 0) return js_undef();
    jdsbuf *x = &jd_sb[b];
    if (jd_media[x->slot].ms_state != JM_MS_OPEN)
        return js_throw_dom(J, "InvalidStateError", "the MediaSource is not open");
    if (x->removing) return js_throw_dom(J, "InvalidStateError", "a removal cannot be aborted");
    if (x->updating) {
        if (x->data) free(x->data);
        x->data = 0;
        x->n = 0;
        x->updating = 0;
        jd_mq_add(x->self, -1, "abort");
        jd_mq_add(x->self, -1, "updateend");
    }
    /* Whatever was cut short is thrown away, and the parser starts again at
       the beginning of a segment. */
    jd_media[x->slot].m.src[x->k].pend_n = 0;
    x->win_start = 0;
    x->win_end = 1.0 / 0.0;
    return js_undef();
}

static jval nat_sb_updating(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int b = jd_sb_of(t);
    return js_bool(b >= 0 && jd_sb[b].updating);
}

static jval nat_sb_buffered(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int b;
    jd_sb_check(J, t, &b);
    if (b < 0) return js_undef();
    double r[64];
    int c = media_buffered(&jd_media[jd_sb[b].slot].m, jd_sb[b].k, r, 32);
    return jd_ranges(J, r, c);
}

static jval nat_sb_offset(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int b = jd_sb_of(t);
    return js_num(b < 0 ? 0 : jd_media[jd_sb[b].slot].m.src[jd_sb[b].k].offset);
}

static jval nat_sb_set_offset(jctx *J, jval t, jval *a, int n) {
    int b;
    jd_sb_check(J, t, &b);
    if (b < 0) return js_undef();
    if (jd_sb[b].updating) return js_throw_dom(J, "InvalidStateError", "an append or a removal is still going on");
    double v = js_to_num(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    if (v != v || v - v != 0) return js_throw(J, JS_ERR_TYPE, "an offset is a finite number", J->error_line);
    jd_ms_open_again(jd_sb[b].slot);
    jd_media[jd_sb[b].slot].m.src[jd_sb[b].k].offset = v;
    return js_undef();
}

static jval nat_sb_mode(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int b = jd_sb_of(t);
    return jd_str(b >= 0 && jd_sb[b].sequence ? "sequence" : "segments");
}

static jval nat_sb_set_mode(jctx *J, jval t, jval *a, int n) {
    int b;
    jd_sb_check(J, t, &b);
    if (b < 0) return js_undef();
    jstr *v = jd_arg_str(J, a, n, 0);
    if (!js_str_is(v, "segments") && !js_str_is(v, "sequence")) return js_undef();
    if (jd_sb[b].updating) return js_throw_dom(J, "InvalidStateError", "an append or a removal is still going on");
    jd_sb[b].sequence = js_str_is(v, "sequence");
    return js_undef();
}

static jval nat_sb_win_start(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int b = jd_sb_of(t);
    return js_num(b < 0 ? 0 : jd_sb[b].win_start);
}

static jval nat_sb_set_win_start(jctx *J, jval t, jval *a, int n) {
    int b;
    jd_sb_check(J, t, &b);
    if (b < 0) return js_undef();
    double v = js_to_num(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    if (!(v >= 0) || v >= jd_sb[b].win_end) return js_throw(J, JS_ERR_TYPE, "that is not a start for the window", J->error_line);
    jd_sb[b].win_start = v;
    return js_undef();
}

static jval nat_sb_win_end(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int b = jd_sb_of(t);
    return js_num(b < 0 ? 1.0 / 0.0 : jd_sb[b].win_end);
}

static jval nat_sb_set_win_end(jctx *J, jval t, jval *a, int n) {
    int b;
    jd_sb_check(J, t, &b);
    if (b < 0) return js_undef();
    double v = js_to_num(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    if (v != v || v <= jd_sb[b].win_start) return js_throw(J, JS_ERR_TYPE, "that is not an end for the window", J->error_line);
    jd_sb[b].win_end = v;
    return js_undef();
}

static jval nat_sb_change_type(jctx *J, jval t, jval *a, int n) {
    int b;
    jd_sb_check(J, t, &b);
    if (b < 0) return js_undef();
    jstr *ty = jd_arg_str(J, a, n, 0);
    if (!ty->len) return js_throw(J, JS_ERR_TYPE, "changeType needs a type", J->error_line);
    if (!jd_media_type_str(ty)) return js_throw_dom(J, "NotSupportedError", "that type is not one this plays");
    if (jd_sb[b].updating) return js_throw_dom(J, "InvalidStateError", "an append or a removal is still going on");
    return js_undef();
}

static jval nat_sb_tracks(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jobj *o = js_array(J);
    return o ? js_from_obj(o) : js_undef();
}

/* --- the element ---------------------------------------------------------------------------- */

/* The stretch under way given up on, or finished with. */
static void jd_media_ask_end(jdmedia *d) {
    if (d->ask && jd_ask_end) jd_ask_end(d->ask - 1);
    d->ask = 0;
}

/* The element let go of what it played: told emptied, at the start again. */
static void jd_media_reset_slot(int s, int tell) {
    jdmedia *d = &jd_media[s];
    jd_media_ask_end(d);
    jd_ms_detach(s);
    if (tell && (d->net == JM_NET_LOADING || d->net == JM_NET_IDLE)) jd_media_tell(s, "abort");
    jd_media_settle(s, 0, "AbortError", "the element was given something else to play");
    if (tell && d->net != JM_NET_EMPTY) {
        jd_media_tell(s, "emptied");
        d->paused = 1;
    }
    media_close(&d->m);
    media_open(&d->m);
    d->ready = JM_NOTHING;
    d->net = JM_NET_EMPTY;
    d->seeking = d->waiting = d->ended_told = d->loaded_told = 0;
    d->error = 0;
    d->vw = d->vh = 0;
    d->pic = 0;
    d->pic_w = d->pic_h = 0;
    d->url[0] = 0;
    d->bytes = 0;
}

/* The source an element names: its src, or the first <source> child of a
   type it plays (or of no type). */
static const char *jd_media_source(int node) {
    const char *src = jd_attr(node, "src");
    if (src) return src;
    for (int c = jd_doc->nodes[node].first; c >= 0; c = jd_doc->nodes[c].next) {
        if (!jd_is_element(c) || !w_same_fold(dom_tag_name(jd_doc, c), "source")) continue;
        const char *s = jd_attr(c, "src");
        if (!s) continue;
        const char *ty = jd_attr(c, "type");
        if (ty && !jd_media_type(ty, w_len(ty))) continue;
        return s;
    }
    return 0;
}

/* The standard's load algorithm, as far as this goes: let go of what was
   there, find the source, and attach its MediaSource. */
static void jd_media_load(int node) {
    int s = jd_media_slot(node);
    if (s < 0) { jd_media_say("a page plays sixteen elements at most"); return; }
    jdmedia *d = &jd_media[s];
    int keep_playing = !d->paused && d->net == JM_NET_EMPTY;
    jd_media_reset_slot(s, 1);
    if (keep_playing) d->paused = 0;
    d->m.muted = jd_attr(node, "muted") != 0;
    jval vol = jd_kept(d->el, jd_k_mvol), mu = jd_kept(d->el, jd_k_mmuted);
    if (vol.t == JS_NUM) d->m.volume = vol.num;
    if (mu.t == JS_BOOL) d->m.muted = mu.b;
    const char *src = jd_media_source(node);
    if (!src || !src[0]) return;
    d->net = JM_NET_LOADING;
    jd_media_tell(s, "loadstart");
    jstr *whole = jd_resolve_str(&jd_J, src);
    const char *addr = whole ? whole->s : src;
    jobj *ms = jd_ms_lookup(addr);
    if (!ms) {
        /* A file: what a blob: or data: address holds, or the network's. */
        jstr *bb, *bt;
        if (jd_blob_lookup(addr, &bb, &bt)) d->bytes = bb;
        else if (jd_is_data_url(addr)) {
            char *data = 0;
            u32 len = jd_data_url(addr, &data, 0, 0);
            if (data) d->bytes = js_str_n(&jd_J, data, len);
            free(data);
            if (!d->bytes) { jd_media_fail(s, 4, "a data: address that does not read"); return; }
        } else if (!w_starts_fold(addr, "http://") && !w_starts_fold(addr, "https://")) {
            jd_media_fail(s, 4, "nothing a src of that kind can name is played here");
            return;
        }
        w_copy(d->url, sizeof(d->url), addr, sizeof(d->url));
        media_file_open(&d->m);
        return;
    }
    int was = jd_ms_slot(ms);
    if (was >= 0) { jd_media_fail(s, 4, "that MediaSource is already attached to an element"); return; }
    d->ms = ms;
    d->ms_state = JM_MS_OPEN;
    jd_keep(ms, jd_k_msslot, js_num(s));
    jd_mq_add(ms, -1, "sourceopen");
}

/* Every <video> and <audio> the page was written with that names
   something, loaded (jsdom_open). */
static void jd_media_scan(void) {
    for (int i = 0; i < jd_doc->count; i++) {
        int tag = jd_doc->nodes[i].tag;
        if (jd_doc->nodes[i].kind != DN_ELEMENT || (tag != T_VIDEO && tag != T_AUDIO) || !jd_connected(i)) continue;
        if (jd_media_source(i)) jd_media_load(i);
    }
}

/* A src set or changed (jd_attr_set): the element loads again. */
static void jd_media_src_set(int el, const char *name) {
    if (!w_same(name, "src")) return;
    const char *tag = dom_tag_name(jd_doc, el);
    if (w_same_fold(tag, "video") || w_same_fold(tag, "audio")) jd_media_load(el);
}

static jval nat_media_load(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int node = jd_el_of(t);
    if (node < 0) return jd_illegal(J);
    jd_media_load(node);
    return js_undef();
}

static jval nat_media_can_play(jctx *J, jval t, jval *a, int n) {
    (void)t;
    int v = jd_media_type_str(jd_arg_str(J, a, n, 0));
    return jd_str(v == 2 ? "probably" : v == 1 ? "maybe" : "");
}

static jval nat_media_play(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int node = jd_el_of(t);
    if (node < 0) return jd_illegal(J);
    int s = jd_media_slot(node);
    if (s < 0) return jd_rejected_dom(J, "NotSupportedError", "a page plays sixteen elements at most");
    jdmedia *d = &jd_media[s];
    if (d->error == 4) return jd_rejected_dom(J, "NotSupportedError", "the element has nothing it can play");
    if (d->net == JM_NET_EMPTY && jd_media_source(node)) { jd_media_load(node); d = &jd_media[s]; }
    jobj *p = js_promise_new(J);
    if (!p) return js_undef();
    if (d->m.ended && !d->seeking) {
        d->seeking = 1;
        d->seek_to = 0;
        media_seek(&d->m, 0);
        d->ended_told = 0;
        jd_media_tell(s, "seeking");
    }
    int was_paused = d->paused;
    if (d->paused) {
        d->paused = 0;
        jd_media_tell(s, "play");
        if (d->ready <= JM_CURRENT) jd_media_tell(s, "waiting");
        else jd_media_tell(s, "playing");
    }
    if (d->npromises < 8) d->promises[d->npromises++] = p;
    else js_promise_settle(J, p, 1, js_undef());
    /* Already playing: kept on the next pass, with nothing told. */
    if (!d->seeking && d->ready >= JM_FUTURE && !was_paused) jd_media_tell(s, "");
    jd_media_clock(s);
    return js_from_obj(p);
}

static jval nat_media_pause(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int node = jd_el_of(t);
    if (node < 0) return jd_illegal(J);
    int s = jd_media_of_node(node);
    if (s < 0) {
        if (!jd_media_source(node)) return js_undef();
        jd_media_load(node);
        s = jd_media_of_node(node);
        if (s < 0) return js_undef();
    }
    jdmedia *d = &jd_media[s];
    if (!d->paused) {
        d->paused = 1;
        jd_media_tell(s, "timeupdate");
        jd_media_tell(s, "pause");
        jd_media_settle(s, 0, "AbortError", "the element was paused before it played");
    }
    jd_media_clock(s);
    return js_undef();
}

static jval nat_media_paused(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int s = jd_media_of(t);
    return js_bool(s < 0 || jd_media[s].paused);
}

static jval nat_media_ended(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int s = jd_media_of(t);
    int node = jd_el_of(t);
    return js_bool(s >= 0 && jd_media[s].m.ended && !jd_attr(node, "loop"));
}

static jval nat_media_seeking(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int s = jd_media_of(t);
    return js_bool(s >= 0 && jd_media[s].seeking);
}

static jval nat_media_time(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int s = jd_media_of(t);
    return js_num(s < 0 ? 0 : jd_media_now(s));
}

static jval nat_media_set_time(jctx *J, jval t, jval *a, int n) {
    int node = jd_el_of(t);
    if (node < 0) return jd_illegal(J);
    double v = js_to_num(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    if (v != v || v - v != 0) return js_throw(J, JS_ERR_TYPE, "a time is a finite number", J->error_line);
    int s = jd_media_slot(node);
    if (s < 0) return js_undef();
    jdmedia *d = &jd_media[s];
    if (v < 0) v = 0;
    if (d->m.duration == d->m.duration && v > d->m.duration) v = d->m.duration;
    if (d->ready == JM_NOTHING) {
        /* Before there is anything: where it will start. */
        d->m.position = v;
        d->m.clock_pos = v;
        return js_undef();
    }
    d->seeking = 1;
    d->seek_to = v;
    d->ended_told = 0;
    media_seek(&d->m, v);
    if (d->ready > JM_METADATA) d->ready = JM_METADATA;
    jd_media_tell(s, "seeking");
    jd_media_clock(s);
    return js_undef();
}

static jval nat_media_duration(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int s = jd_media_of(t);
    if (s < 0 || jd_media[s].ready == JM_NOTHING) return js_num(js_nan());
    return js_num(jd_media[s].m.duration);
}

static jval nat_media_ready(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int s = jd_media_of(t);
    return js_num(s < 0 ? 0 : jd_media[s].ready);
}

static jval nat_media_network(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int s = jd_media_of(t);
    return js_num(s < 0 ? JM_NET_EMPTY : jd_media[s].net);
}

static jval nat_media_error(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int s = jd_media_of(t);
    if (s < 0 || !jd_media[s].error) return js_null();
    jobj *e = js_object_with(J, JO_PLAIN, jd_p_mediaerr);
    if (!e) return js_null();
    static const char *const WHY[] = { "", "aborted", "the network failed",
        "the stream could not be decoded", "nothing the element names can be played here" };
    js_set(J, e, "code", js_num(jd_media[s].error));
    js_set(J, e, "message", jd_str(WHY[jd_media[s].error & 3 ? jd_media[s].error & 3 : 4]));
    return js_from_obj(e);
}

static jval nat_media_buffered(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int s = jd_media_of(t);
    double r[64];
    int c = s < 0 ? 0 : jd_media_buffered(s, r, 32);
    return jd_ranges(J, r, c);
}

static jval nat_media_seekable(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int s = jd_media_of(t);
    double r[2] = { 0, 0 };
    if (s >= 0 && jd_media[s].ready > JM_NOTHING && jd_media[s].m.duration == jd_media[s].m.duration) {
        r[1] = jd_media[s].m.duration;
        if (r[1] - r[1] != 0) {               /* live: what is buffered */
            double b[64];
            int c = jd_media_buffered(s, b, 32);
            if (!c) return jd_ranges(J, r, 0);
            r[0] = b[0];
            r[1] = b[2 * c - 1];
        }
        return jd_ranges(J, r, 1);
    }
    return jd_ranges(J, r, 0);
}

static jval nat_media_played(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int s = jd_media_of(t);
    double r[2] = { 0, s < 0 ? 0 : jd_media_now(s) };
    return jd_ranges(J, r, r[1] > 0);
}

static jval nat_media_volume(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (!js_is_obj(t)) return js_num(1);
    jval v = jd_kept(t.obj, jd_k_mvol);
    return v.t == JS_NUM ? v : js_num(1);
}

static jval nat_media_set_volume(jctx *J, jval t, jval *a, int n) {
    if (jd_el_of(t) < 0) return jd_illegal(J);
    double v = js_to_num(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    if (!(v >= 0 && v <= 1)) return js_throw_dom(J, "IndexSizeError", "a volume is from 0 to 1");
    jval was = jd_kept(t.obj, jd_k_mvol);
    if (was.t == JS_NUM && was.num == v) return js_undef();
    jd_keep(t.obj, jd_k_mvol, js_num(v));
    int s = jd_media_of(t);
    if (s >= 0) { jd_media[s].m.volume = v; jd_media_tell(s, "volumechange"); }
    else jd_mq_add(0, jd_el_of(t), "volumechange");
    return js_undef();
}

static jval nat_media_muted(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    if (!js_is_obj(t)) return js_bool(0);
    jval v = jd_kept(t.obj, jd_k_mmuted);
    if (v.t == JS_BOOL) return v;
    int node = jd_el_of(t);
    return js_bool(node >= 0 && jd_attr(node, "muted"));
}

static jval nat_media_set_muted(jctx *J, jval t, jval *a, int n) {
    int node = jd_el_of(t);
    if (node < 0) return jd_illegal(J);
    int v = js_to_bool(js_arg(a, n, 0));
    jval was = nat_media_muted(J, t, a, n);
    jd_keep(t.obj, jd_k_mmuted, js_bool(v));
    if (was.b == v) return js_undef();
    int s = jd_media_of(t);
    if (s >= 0) { jd_media[s].m.muted = v; jd_media_tell(s, "volumechange"); }
    else jd_mq_add(0, node, "volumechange");
    return js_undef();
}

static jval jd_media_rate(jval t, jstr *key) {
    if (!js_is_obj(t)) return js_num(1);
    jval v = jd_kept(t.obj, key);
    return v.t == JS_NUM ? v : js_num(1);
}

static jval jd_media_set_rate(jctx *J, jval t, jval *a, int n, jstr *key) {
    int node = jd_el_of(t);
    if (node < 0) return jd_illegal(J);
    double v = js_to_num(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    if (v != v || v - v != 0) return js_throw(J, JS_ERR_TYPE, "a rate is a finite number", J->error_line);
    jval was = jd_media_rate(t, key);
    jd_keep(t.obj, key, js_num(v));
    if (was.num != v) jd_mq_add(0, node, "ratechange");
    return js_undef();
}

static jval nat_media_rate(jctx *J, jval t, jval *a, int n) { (void)J; (void)a; (void)n; return jd_media_rate(t, jd_k_mrate); }
static jval nat_media_drate(jctx *J, jval t, jval *a, int n) { (void)J; (void)a; (void)n; return jd_media_rate(t, jd_k_mdrate); }
static jval nat_media_set_rate(jctx *J, jval t, jval *a, int n) { return jd_media_set_rate(J, t, a, n, jd_k_mrate); }
static jval nat_media_set_drate(jctx *J, jval t, jval *a, int n) { return jd_media_set_rate(J, t, a, n, jd_k_mdrate); }

static jval nat_media_vw(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int s = jd_media_of(t);
    return js_num(s < 0 || jd_media[s].ready == JM_NOTHING ? 0 : jd_media[s].vw);
}

static jval nat_media_vh(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int s = jd_media_of(t);
    return js_num(s < 0 || jd_media[s].ready == JM_NOTHING ? 0 : jd_media[s].vh);
}

static jval nat_media_quality(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int s = jd_media_of(t);
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return js_undef();
    int shown = s < 0 ? 0 : jd_media[s].m.frames_shown, dropped = s < 0 ? 0 : jd_media[s].m.frames_dropped;
    js_set(J, o, "creationTime", js_num(jd_now_ms()));
    js_set(J, o, "totalVideoFrames", js_num(shown + dropped));
    js_set(J, o, "droppedVideoFrames", js_num(dropped));
    js_set(J, o, "corruptedVideoFrames", js_num(0));
    return js_from_obj(o);
}

static jval nat_media_src_object(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_null();
}

/* --- each pass ------------------------------------------------------------------------------ */

/* Where an element stands now that more is buffered or the clock moved:
   readyState, and what that change tells. */
static void jd_media_judge(int s) {
    jdmedia *d = &jd_media[s];
    if ((!d->ms && !d->m.file) || d->error) return;
    int any = 0, all = 1;
    if (d->m.file) any = all = d->m.file_ready;
    for (int b = 0; b < JD_SBUFS; b++)
        if (jd_sb[b].self && jd_sb[b].slot == s) {
            any = 1;
            if (!d->m.src[jd_sb[b].k].have_init) all = 0;
        }
    if (!any || !all) return;
    media_t *m = &d->m;
    if (d->ready == JM_NOTHING) {
        for (int k = 0; k < MEDIA_SOURCES; k++)
            for (int i = 0; i < m->src[k].init.n; i++)
                if (m->src[k].init.t[i].kind == MP4_VIDEO && !d->vw) {
                    d->vw = m->src[k].init.t[i].width;
                    d->vh = m->src[k].init.t[i].height;
                }
        d->ready = JM_METADATA;
        if (m->duration == m->duration) jd_media_tell(s, "durationchange");
        jd_media_tell(s, "loadedmetadata");
        if (d->vw) { jd_media_tell(s, "resize"); jd_media_resized = 1; }
    }
    double r[64];
    int c = jd_media_buffered(s, r, 32);
    double at = d->seeking ? d->seek_to : jd_media_now(s);
    int now = 0, ahead = 0;
    for (int i = 0; i < c; i++)
        if (at >= r[2 * i] - 0.15 && at < r[2 * i + 1]) {
            now = 1;
            double end = r[2 * i + 1];
            if (end >= at + 1.0 || (m->eos && i == c - 1) ||
                (m->duration == m->duration && end >= m->duration - 0.1)) ahead = 1;
        }
    if (m->ended || (m->eos && c && at >= r[2 * c - 1] - 0.05)) now = ahead = 1;
    int want = ahead ? JM_ENOUGH : now ? JM_CURRENT : JM_METADATA;
    int was = d->ready;
    if (d->seeking && now) {
        d->seeking = 0;
        jd_media_tell(s, "timeupdate");
        jd_media_tell(s, "seeked");
    }
    if (want == was) { jd_media_clock(s); return; }
    d->ready = want;
    if (want >= JM_CURRENT && !d->loaded_told) { d->loaded_told = 1; jd_media_tell(s, "loadeddata"); }
    if (want >= JM_FUTURE && was < JM_FUTURE) {
        jd_media_tell(s, "canplay");
        if (!d->paused) jd_media_tell(s, "playing");
        else if (jd_attr(d->node, "autoplay") && d->m.position == 0) {
            d->paused = 0;
            jd_media_tell(s, "play");
            jd_media_tell(s, "playing");
        }
        d->waiting = 0;
    }
    if (want == JM_ENOUGH && was < JM_ENOUGH) jd_media_tell(s, "canplaythrough");
    if (want < JM_FUTURE && was >= JM_FUTURE && !d->paused && !m->ended) {
        d->waiting = 1;
        jd_media_tell(s, "timeupdate");
        jd_media_tell(s, "waiting");
    }
    jd_media_clock(s);
}

/* An append or a removal a SourceBuffer was asked for, done now. */
static void jd_sb_work(int b) {
    jdsbuf *x = &jd_sb[b];
    jobj *sb = x->self;
    int s = x->slot;
    jd_obj_simple(sb, "updatestart");
    if (!jd_sb[b].self || jd_sb[b].self != sb) return;     /* removed by a listener */
    int failed = 0;
    if (x->removing) {
        media_remove(&jd_media[s].m, x->k, x->from, x->to);
        x->removing = 0;
    } else if (x->data) {
        failed = media_append(&jd_media[s].m, x->k, x->data, x->n) < 0;
        if (failed) jd_media_say(jd_media[s].m.src[x->k].why[0] ? jd_media[s].m.src[x->k].why : "an append was not MP4 this reads");
        free(x->data);
        x->data = 0;
        x->n = 0;
    }
    x->updating = 0;
    if (failed) {
        /* The standard's append error: the stream ended with a decode error
           before the buffer's error and updateend are told. */
        jd_media[s].m.src[x->k].pend_n = 0;
        if (jd_media[s].ms_state == JM_MS_OPEN) {
            jd_media[s].ms_state = JM_MS_ENDED;
            jd_mq_add(jd_media[s].ms, -1, "sourceended");
        }
        jd_media_fail(s, 3, "a segment could not be read");
        jd_obj_simple(sb, "error");
        jd_obj_simple(sb, "updateend");
        return;
    }
    /* What arrived is judged before updateend, so a page that asks then
       finds the readyState and size it brought. */
    jd_media_judge(s);
    jd_obj_simple(sb, "update");
    jd_obj_simple(sb, "updateend");
    int now = ticks();
    if (now - jd_media[s].last_progress >= 35) { jd_media[s].last_progress = now; jd_media_tell(s, "progress"); }
}

/* The whole length a 206's Content-Range says ("bytes a-b/total"), or 0. */
static long long jd_range_total(const jd_reply *rp) {
    const char *h = rp->head;
    for (int i = 0; h && i + 14 < rp->hlen; i++) {
        if ((i && h[i - 1] != '\n') || !w_starts_fold(h + i, "content-range:")) continue;
        int k = i + 14;
        while (k < rp->hlen && h[k] != '/' && h[k] != '\n') k++;
        if (k >= rp->hlen || h[k] != '/') return 0;
        long long v = 0;
        for (k++; k < rp->hlen && h[k] >= '0' && h[k] <= '9'; k++) v = v * 10 + (h[k] - '0');
        return v;
    }
    return 0;
}

/* One stretch of a file the element wants, fetched and fed: from the bytes
   a blob: or data: address held, or by a range from the network -- started
   on one pass and taken on a later one where the browser can, so the page
   and what is already buffered go on playing while it comes. 1 when
   something was asked for or came. */
static int jd_media_file_step(int s) {
    jdmedia *d = &jd_media[s];
    long long from = 0;
    int len = 0;
    int r;
    jd_reply rp = { 0, 0, 0, 0, 0, 0, 0 };
    if (d->ask) {
        int st = jd_ask_poll(d->ask - 1, &rp);
        if (st == 0) return 0;
        if (st < 0) rp.status = st;
        from = d->ask_from;
    } else if (!media_file_want(&d->m, &from, &len)) return 0;
    if (d->bytes && !d->ask) {
        long long n = d->bytes->len;
        if (from >= n) { jd_media_fail(s, 4, "the file ends before what it says it holds"); return 1; }
        long long l = from + len > n ? n - from : len;
        r = media_file_feed(&d->m, from, (const u8 *)d->bytes->s + from, l, n);
    } else {
        if (!d->ask && !jd_do_request && !jd_ask_start) return 0;
        char range[64];
        int w = 0;
        const char *pre = "Range: bytes=";
        for (const char *p = pre; *p; p++) range[w++] = *p;
        long long nums[2] = { from, from + len - 1 };
        for (int k = 0; k < 2; k++) {
            char tmp[24];
            int t = 0;
            long long v = nums[k];
            do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (t) range[w++] = tmp[--t];
            if (!k) range[w++] = '-';
        }
        range[w++] = '\r';
        range[w++] = '\n';
        range[w] = 0;
        if (!d->ask && jd_ask_start) {
            int id = jd_ask_start("GET", d->url, 0, 0, 0, range, len + 65536);
            if (id == -1) return 0;              /* every way out busy: a later pass */
            if (id >= 0) { d->ask = id + 1; d->ask_from = from; return 1; }
            rp.status = id;
        } else if (!d->ask) {
            jd_do_request("GET", d->url, 0, 0, 0, range, &rp);
        }
        if (rp.status != 200 && rp.status != 206) {
            jd_media_ask_end(d);
            jd_media_fail(s, d->ready ? 2 : 4, rp.status > 0 ? "the server would not give the file" : "the file could not be fetched");
            return 1;
        }
        /* A server that does not do ranges sends the whole file from the
           start, as far as it fits. */
        long long at = rp.status == 206 ? from : 0;
        long long total = rp.status == 206 ? jd_range_total(&rp) : rp.len;
        r = rp.len > 0 ? media_file_feed(&d->m, at, (const u8 *)rp.body, rp.len, total) : -1;
        jd_media_ask_end(d);
    }
    if (r < 0) jd_media_fail(s, d->ready ? 3 : 4, d->m.src[0].why[0] ? d->m.src[0].why : "the file is not MP4 this plays");
    else {
        int now = ticks();
        if (now - d->last_progress >= 35) { d->last_progress = now; jd_media_tell(s, "progress"); }
    }
    return 1;
}

/* Lends the slot that has the card's sound, so two elements never write
   into it at once. */
static int jd_media_sound_owner = -1;

static int jd_media_pump(void) {
    int told = 0;
    /* What was queued, in order; what that queues waits for the next pass. */
    int count = jd_mq_n;
    for (int i = 0; i < count && !jd_spent(); i++) {
        jobj *to = jd_mq[i].to;
        int node = jd_mq[i].node;
        const char *type = jd_mq[i].type;
        jd_mq[i].to = 0;
        if (node >= 0) {
            if (type[0]) jd_fire_simple(node, type, 0, 0);
            /* The play()s waiting are kept when playing is told (the
               standard's "notify about playing"). */
            int s = jd_media_of_node(node);
            if (s >= 0 && (!type[0] || w_same(type, "playing"))) jd_media_settle(s, 1, 0, 0);
        } else if (to) jd_obj_simple(to, type);
        js_drain(&jd_J);
        told++;
    }
    for (int i = count; i < jd_mq_n; i++) { jd_mq[i - count] = jd_mq[i]; jd_mq[i].to = 0; }
    jd_mq_n -= count;
    for (int b = 0; b < JD_SBUFS && !jd_spent(); b++)
        if (jd_sb[b].self && jd_sb[b].updating) { jd_sb_work(b); js_drain(&jd_J); told++; }
    for (int s = 0; s < JD_MEDIA; s++) {
        jdmedia *d = &jd_media[s];
        if (!d->el || (!d->ms && !d->m.file) || d->error) continue;
        /* A file's next stretch, one a pass: each holds the browser while
           it comes. */
        if (d->m.file && jd_media_file_step(s)) told++;
        if (d->error) continue;
        jd_media_judge(s);
        if (d->ready < JM_METADATA) continue;
        if (jd_media_sound_owner < 0 && d->m.playing) jd_media_sound_owner = s;
        d->m.no_sound = jd_media_sound_owner != s;
        int fresh = media_step(&d->m);
        if (fresh) {
            d->pic_w = d->m.vw;
            d->pic_h = d->m.vh;
            d->pic = d->m.frame;
            jd_media_frames_new = 1;
            if (d->m.vw != d->vw || d->m.vh != d->vh) {
                d->vw = d->m.vw;
                d->vh = d->m.vh;
                jd_media_tell(s, "resize");
                jd_media_resized = 1;
            }
        }
        int now = ticks();
        if (d->m.playing && now - d->last_update >= 25) { d->last_update = now; jd_media_tell(s, "timeupdate"); }
        if (d->m.ended && !d->ended_told) {
            d->ended_told = 1;
            if (jd_attr(d->node, "loop")) {
                d->ended_told = 0;
                media_seek(&d->m, 0);
                d->seeking = 1;
                d->seek_to = 0;
                jd_media_tell(s, "seeking");
            } else {
                jd_media_tell(s, "timeupdate");
                if (!d->paused) { d->paused = 1; jd_media_tell(s, "pause"); }
                jd_media_tell(s, "ended");
            }
            jd_media_clock(s);
        }
        if (!d->m.playing && jd_media_sound_owner == s) jd_media_sound_owner = -1;
    }
    return told;
}

/* How soon the page wants the next pass for its media: -1 for nothing. */
static int jd_media_due(void) {
    if (jd_mq_n) return 0;
    for (int b = 0; b < JD_SBUFS; b++) if (jd_sb[b].self && jd_sb[b].updating) return 0;
    int best = -1;
    for (int s = 0; s < JD_MEDIA; s++) {
        jdmedia *d = &jd_media[s];
        if (!d->el || (!d->ms && !d->m.file) || d->error) continue;
        long long from;
        int len;
        if (d->ask) best = 1;
        else if (d->m.file && media_file_want(&d->m, &from, &len)) return 0;
        if (d->m.playing) return 1;
        if (d->ready >= JM_METADATA && !d->m.frames_shown) best = 2;
    }
    return best;
}

static void jd_media_reset(void) {
    for (int s = 0; s < JD_MEDIA; s++) {
        jd_media_ask_end(&jd_media[s]);
        if (jd_media[s].el) media_close(&jd_media[s].m);
        for (int i = 0; i < (int)sizeof(jd_media[s]); i++) ((volatile u8 *)&jd_media[s])[i] = 0;
    }
    for (int b = 0; b < JD_SBUFS; b++) {
        if (jd_sb[b].data) free(jd_sb[b].data);
        for (int i = 0; i < (int)sizeof(jd_sb[b]); i++) ((volatile u8 *)&jd_sb[b])[i] = 0;
    }
    for (int i = 0; i < JD_MQ; i++) jd_mq[i].to = 0;
    jd_mq_n = 0;
    jd_media_frames_new = 0;
    jd_media_resized = 0;
    jd_media_sound_owner = -1;
}

/* --- for the browser ------------------------------------------------------------------------ */

/* The latest picture of a <video>, three bytes a pixel, or 0 while it has
   none. */
const u8 *jsdom_media_picture(int node, int *w, int *h) {
    int s = jd_media_of_node(node);
    if (s < 0 || !jd_media[s].pic) return 0;
    *w = jd_media[s].pic_w;
    *h = jd_media[s].pic_h;
    return jd_media[s].pic;
}

/* Each element that knows its picture's size, for the layout: the i-th's
   node and size, or 0 past the last. */
int jsdom_media_size(int i, int *node, int *w, int *h) {
    for (int s = 0; s < JD_MEDIA; s++) {
        if (!jd_media[s].el || !jd_media[s].vw || !jd_media[s].vh) continue;
        if (i-- > 0) continue;
        *node = jd_media[s].node;
        *w = jd_media[s].vw;
        *h = jd_media[s].vh;
        return 1;
    }
    return 0;
}

/* Whether a picture has come that has not been drawn, and whether a size
   changed (which the layout must hear); each is answered once. */
int jsdom_media_frames(int *resized) {
    int f = jd_media_frames_new;
    jd_media_frames_new = 0;
    if (resized) { *resized = jd_media_resized; jd_media_resized = 0; }
    return f;
}

/* For a test: every element's clock set to t (negative: the real one). */
void jsdom_media_test_clock(double t) {
    for (int s = 0; s < JD_MEDIA; s++) if (jd_media[s].el) jd_media[s].m.test_clock = t;
}

/* For a test: each decoded picture of every element, before it is shown. */
void jsdom_media_on_picture(void (*fn)(void *, const h264_picture *)) {
    for (int s = 0; s < JD_MEDIA; s++) if (jd_media[s].el) jd_media[s].m.on_picture = fn;
}

/* new Audio(src): an <audio> to play by script, its src set (and so loaded)
   when one is given, preload auto as the standard has it. */
static jval nat_audio_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "Audio is made with new", J->error_line);
    int o = dom_create_element(jd_doc, "audio", 5);
    if (o < 0) return js_undef();
    jval v = jd_el_value(J, o);
    dom_attr_set(jd_doc, o, "preload", "auto");
    if (n > 0 && a[0].t != JS_UNDEF) {
        jstr *src = js_to_str(J, a[0]);
        if (J->sig != JS_OK) return js_undef();
        jd_attr_set(o, "src", src->s);
    }
    return v;
}

/* --- setting up ----------------------------------------------------------------------------- */

static void jd_setup_media(jctx *J) {
    jd_k_msslot = js_sym_new(J, "element", 7);
    jd_k_mslist = js_sym_new(J, "buffers", 7);
    jd_k_msactive = js_sym_new(J, "active", 6);
    jd_k_msdur = js_sym_new(J, "duration", 8);
    jd_k_sbslot = js_sym_new(J, "buffer", 6);
    jd_k_trlist = js_sym_new(J, "ranges", 6);
    jd_k_mvol = js_sym_new(J, "volume", 6);
    jd_k_mmuted = js_sym_new(J, "muted", 5);
    jd_k_mrate = js_sym_new(J, "playbackRate", 12);
    jd_k_mdrate = js_sym_new(J, "defaultPlaybackRate", 19);
    jd_media_reset();

    jd_p_ranges = jd_interface(J, "TimeRanges", 0, 0, 0);
    jd_accessor(J, jd_p_ranges, "length", nat_ranges_length, 0);
    jd_method(J, jd_p_ranges, "start", nat_ranges_start, 1);
    jd_method(J, jd_p_ranges, "end", nat_ranges_end, 1);

    jd_p_mediaerr = jd_interface(J, "MediaError", 0, 0, 0);
    static const char *const ERRS[] = { "MEDIA_ERR_ABORTED", "MEDIA_ERR_NETWORK", "MEDIA_ERR_DECODE",
        "MEDIA_ERR_SRC_NOT_SUPPORTED", 0 };
    jd_consts(J, jd_p_mediaerr, ERRS, 1);
    jd_consts(J, jd_ctor_of(jd_p_mediaerr), ERRS, 1);

    jd_p_sblist = jd_interface(J, "SourceBufferList", jd_p[JI_EVENTTARGET], 0, 0);
    jd_accessor(J, jd_p_sblist, "length", nat_sblist_length, 0);

    jd_p_sb = jd_interface(J, "SourceBuffer", jd_p[JI_EVENTTARGET], 0, 0);
    jd_method(J, jd_p_sb, "appendBuffer", nat_sb_append, 1);
    jd_method(J, jd_p_sb, "remove", nat_sb_remove, 2);
    jd_method(J, jd_p_sb, "abort", nat_sb_abort, 0);
    jd_method(J, jd_p_sb, "changeType", nat_sb_change_type, 1);
    jd_accessor(J, jd_p_sb, "updating", nat_sb_updating, 0);
    jd_accessor(J, jd_p_sb, "buffered", nat_sb_buffered, 0);
    jd_accessor(J, jd_p_sb, "timestampOffset", nat_sb_offset, nat_sb_set_offset);
    jd_accessor(J, jd_p_sb, "mode", nat_sb_mode, nat_sb_set_mode);
    jd_accessor(J, jd_p_sb, "appendWindowStart", nat_sb_win_start, nat_sb_set_win_start);
    jd_accessor(J, jd_p_sb, "appendWindowEnd", nat_sb_win_end, nat_sb_set_win_end);
    jd_accessor(J, jd_p_sb, "audioTracks", nat_sb_tracks, 0);
    jd_accessor(J, jd_p_sb, "videoTracks", nat_sb_tracks, 0);
    jd_accessor(J, jd_p_sb, "textTracks", nat_sb_tracks, 0);

    jd_p_ms = jd_interface(J, "MediaSource", jd_p[JI_EVENTTARGET], nat_ms_ctor, 0);
    jd_accessor(J, jd_p_ms, "readyState", nat_ms_state, 0);
    jd_accessor(J, jd_p_ms, "sourceBuffers", nat_ms_buffers, 0);
    jd_accessor(J, jd_p_ms, "activeSourceBuffers", nat_ms_active, 0);
    jd_accessor(J, jd_p_ms, "duration", nat_ms_duration, nat_ms_set_duration);
    jd_method(J, jd_p_ms, "addSourceBuffer", nat_ms_add, 1);
    jd_method(J, jd_p_ms, "removeSourceBuffer", nat_ms_remove, 1);
    jd_method(J, jd_p_ms, "endOfStream", nat_ms_end, 0);
    jd_method(J, jd_p_ms, "setLiveSeekableRange", nat_ms_live_range, 2);
    jd_method(J, jd_p_ms, "clearLiveSeekableRange", nat_ms_live_range, 0);
    jobj *ctor = jd_ctor_of(jd_p_ms);
    if (ctor) {
        js_method(J, ctor, "isTypeSupported", nat_ms_supported, 1);
        js_set(J, ctor, "canConstructInDedicatedWorker", js_bool(0));
    }

    jobj *p = jd_iface("HTMLMediaElement");
    if (p) {
        jd_accessor(J, p, "paused", nat_media_paused, 0);
        jd_accessor(J, p, "ended", nat_media_ended, 0);
        jd_accessor(J, p, "seeking", nat_media_seeking, 0);
        jd_accessor(J, p, "currentTime", nat_media_time, nat_media_set_time);
        jd_accessor(J, p, "duration", nat_media_duration, 0);
        jd_accessor(J, p, "volume", nat_media_volume, nat_media_set_volume);
        jd_accessor(J, p, "muted", nat_media_muted, nat_media_set_muted);
        jd_accessor(J, p, "playbackRate", nat_media_rate, nat_media_set_rate);
        jd_accessor(J, p, "defaultPlaybackRate", nat_media_drate, nat_media_set_drate);
        jd_accessor(J, p, "readyState", nat_media_ready, 0);
        jd_accessor(J, p, "networkState", nat_media_network, 0);
        jd_accessor(J, p, "error", nat_media_error, 0);
        jd_accessor(J, p, "buffered", nat_media_buffered, 0);
        jd_accessor(J, p, "seekable", nat_media_seekable, 0);
        jd_accessor(J, p, "played", nat_media_played, 0);
        jd_accessor(J, p, "srcObject", nat_media_src_object, nat_nothing_js);
        jd_method(J, p, "canPlayType", nat_media_can_play, 1);
        jd_method(J, p, "play", nat_media_play, 0);
        jd_method(J, p, "pause", nat_media_pause, 0);
        jd_method(J, p, "load", nat_media_load, 0);
        static const char *const NET[] = { "NETWORK_EMPTY", "NETWORK_IDLE", "NETWORK_LOADING", "NETWORK_NO_SOURCE", 0 };
        static const char *const READY[] = { "HAVE_NOTHING", "HAVE_METADATA", "HAVE_CURRENT_DATA",
            "HAVE_FUTURE_DATA", "HAVE_ENOUGH_DATA", 0 };
        jd_consts(J, p, NET, 0);
        jd_consts(J, p, READY, 0);
        jd_consts(J, jd_ctor_of(p), NET, 0);
        jd_consts(J, jd_ctor_of(p), READY, 0);
    }
    jobj *audio = js_native_n(J, "Audio", nat_audio_ctor, 0);
    if (audio) {
        js_put_prop_flags(J, audio, J->s_prototype, js_from_obj(jd_iface("HTMLAudioElement")), 0);
        js_declare_flags(J, J->global, js_str(J, "Audio"), js_from_obj(audio), JP_WRITE | JP_CONF);
    }
    if ((p = jd_iface("HTMLVideoElement"))) {
        jd_accessor(J, p, "videoWidth", nat_media_vw, 0);
        jd_accessor(J, p, "videoHeight", nat_media_vh, 0);
        jd_method(J, p, "getVideoPlaybackQuality", nat_media_quality, 0);
    }
}
