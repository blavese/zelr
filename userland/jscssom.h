/* Style sheets as objects (the CSSOM), and what the browser asks of them:
 *
 * - A style or link element's sheet, document.styleSheets, and sheets a
 *   script makes itself (new CSSStyleSheet), with their rules as objects:
 *   style rules with a selector and a style of their own, the rules that
 *   hold rules (@media, @supports, @container, @layer, @scope,
 *   @starting-style), @keyframes with its frames, @font-face, @import,
 *   @page, @namespace, @property and @counter-style.
 * - insertRule, deleteRule and the rest that change a sheet. What they
 *   change is what the page looks like: a sheet a script has changed is
 *   written out again (jcs_publish), and the browser reads that in place of
 *   what the element holds (jsdom_sheet_override). emotion and
 *   styled-components, built for production, put all of a React page's
 *   rules in this way, and none of them had reached the screen.
 * - document.adoptedStyleSheets, read after every other sheet
 *   (jsdom_adopted).
 * - The load and error events of a stylesheet link and of a preloaded
 *   sheet, fired once the browser has fetched it (jsdom_link_loaded):
 *   webpack waits for one before it runs the code the sheet came with, and
 *   a preloaded sheet's onload is what makes it one of the page's.
 *
 * A list of rules, a sheet's or a rule's, is two arrays kept on its holder:
 * each rule's text, and the rule's object once a script has asked for it.
 * A sheet nobody reads costs nothing, and one only added to is never taken
 * apart.
 *
 * A shadow root's adopted sheets are read for its tree (jsdom_shadow_adopted,
 * browser.c). Not done: an @import rule's own styleSheet is null; a style is the text it was written as, so a shorthand is not
 * expanded into its longhands as a browser's is; and nested rules inside a
 * style rule are kept as written, not made into rules of their own. */
#pragma once

enum { JK_STYLE, JK_IMPORT, JK_MEDIA, JK_FONTFACE, JK_PAGE, JK_KEYFRAMES, JK_KEYFRAME, JK_NAMESPACE,
       JK_SUPPORTS, JK_LAYERBLOCK, JK_LAYERSTMT, JK_CONTAINER, JK_PROPERTY, JK_SCOPE, JK_STARTING,
       JK_COUNTER, JK_FONTFEAT, JK_COUNT };

/* What each kind is to a script: its interface, its number in the old
   CSSRule.type (0 for the kinds that came after that was frozen), the
   at-keyword it is written with, and what it holds. */
enum { JH_DECLS, JH_RULES, JH_NONE };
static const struct { const char *iface; int type; const char *at; int holds; } JCS_KIND[JK_COUNT] = {
    { "CSSStyleRule", 1, 0, JH_DECLS },
    { "CSSImportRule", 3, "import", JH_NONE },
    { "CSSMediaRule", 4, "media", JH_RULES },
    { "CSSFontFaceRule", 5, "font-face", JH_DECLS },
    { "CSSPageRule", 6, "page", JH_DECLS },
    { "CSSKeyframesRule", 7, "keyframes", JH_RULES },
    { "CSSKeyframeRule", 8, 0, JH_DECLS },
    { "CSSNamespaceRule", 10, "namespace", JH_NONE },
    { "CSSSupportsRule", 12, "supports", JH_RULES },
    { "CSSLayerBlockRule", 0, "layer", JH_RULES },
    { "CSSLayerStatementRule", 0, "layer", JH_NONE },
    { "CSSContainerRule", 0, "container", JH_RULES },
    { "CSSPropertyRule", 0, "property", JH_DECLS },
    { "CSSScopeRule", 0, "scope", JH_RULES },
    { "CSSStartingStyleRule", 0, "starting-style", JH_RULES },
    { "CSSCounterStyleRule", 11, "counter-style", JH_DECLS },
    { "CSSFontFeatureValuesRule", 14, "font-feature-values", JH_NONE },
};

/* Where a list of rules is being read: a sheet's own, inside a rule that
   holds rules, or inside @keyframes, whose rules are its frames. */
enum { JC_SHEET, JC_GROUP, JC_FRAMES };

enum { JF_MOD = 1, JF_OFF = 2, JF_MADE = 4, JF_READ = 8, JF_LINK = 16 };

static jstr *jcs_k_texts, *jcs_k_objs, *jcs_k_owner, *jcs_k_hash, *jcs_k_flags, *jcs_k_href, *jcs_k_list,
            *jcs_k_sheet, *jcs_k_parent, *jcs_k_kind, *jcs_k_head, *jcs_k_body, *jcs_k_style, *jcs_k_media,
            *jcs_k_mof, *jcs_k_sheetobj, *jcs_k_adopted, *jcs_k_mtext;
static jobj *jcs_p_stylesheet, *jcs_p_sheet, *jcs_p_rulelist, *jcs_p_medialist, *jcs_p_sheetlist,
            *jcs_p_rule[JK_COUNT];
static jobj *jcs_rstyles;              /* rules whose style exists, by host number (JD_RSTYLE + n) */
static jobj *jcs_rlists;               /* what each live list lists, by host number (JD_RLIST + n) */
static jobj *jcs_sheetlist;            /* document.styleSheets, as it was at jcs_sheetlist_at */
static u32 jcs_sheetlist_at;

/* What the browser reads in place of an element's own sheet, one for each
   sheet a script has changed, and for each sheet a script made. Written out
   when the browser asks and not at each change (stale): emotion puts rules
   in one at a time, thousands on a large page, and writing the whole sheet
   after each was the square of that. */
#define JCS_RECS 128
static struct { jobj *sheet; int node, stale; u32 hash; char *text; int len; } jcs_rec[JCS_RECS];
static int jcs_nrec;
static u32 jcs_version;

static int (*jcs_sheet_text)(const char *url, const char **text);

/* --- reading CSS -------------------------------------------------------------------------- */

static int jcs_after_string(const char *s, int len, int p) {
    char q = s[p++];
    while (p < len && s[p] != q && s[p] != '\n') p += s[p] == '\\' ? 2 : 1;
    return p < len ? p + 1 : len;
}

static int jcs_after_comment(const char *s, int len, int p) {
    for (p += 2; p + 1 < len; p++)
        if (s[p] == '*' && s[p + 1] == '/') return p + 2;
    return len;
}

/* Past white space and comments, and at a sheet's top level the <!-- and
   --> a sheet inside a style element may still be wrapped in. */
static int jcs_skip(const char *s, int len, int p, int top) {
    for (;;) {
        while (p < len && css_space(s[p])) p++;
        if (p + 1 < len && s[p] == '/' && s[p + 1] == '*') { p = jcs_after_comment(s, len, p); continue; }
        if (top && p + 3 < len && s[p] == '<' && s[p + 1] == '!' && s[p + 2] == '-' && s[p + 3] == '-') { p += 4; continue; }
        if (top && p + 2 < len && s[p] == '-' && s[p + 1] == '-' && s[p + 2] == '>') { p += 3; continue; }
        return p;
    }
}

/* The first { (or ; when semi) outside brackets and strings from p, or a
   stray }, or the end. */
static int jcs_scan(const char *s, int len, int p, int semi) {
    int depth = 0;
    while (p < len) {
        char c = s[p];
        if (c == '\\') { p += 2; continue; }
        if (c == '"' || c == '\'') { p = jcs_after_string(s, len, p); continue; }
        if (c == '/' && p + 1 < len && s[p + 1] == '*') { p = jcs_after_comment(s, len, p); continue; }
        if (c == '(' || c == '[') depth++;
        else if ((c == ')' || c == ']') && depth) depth--;
        else if (!depth && (c == '{' || c == '}' || (semi && c == ';'))) return p;
        p++;
    }
    return len;
}

/* The } that closes a block opened just before p, or the end, which closes
   every block still open. */
static int jcs_block_end(const char *s, int len, int p) {
    int depth = 1;
    while (p < len) {
        char c = s[p];
        if (c == '\\') { p += 2; continue; }
        if (c == '"' || c == '\'') { p = jcs_after_string(s, len, p); continue; }
        if (c == '/' && p + 1 < len && s[p + 1] == '*') { p = jcs_after_comment(s, len, p); continue; }
        if (c == '{') depth++;
        else if (c == '}' && !--depth) return p;
        p++;
    }
    return len;
}

static int jcs_same_fold(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (w_lower(a[i]) != w_lower(b[i])) return 0;
    return 1;
}

static int jcs_word(const char *s, int n, const char *w) {
    int i = 0;
    for (; i < n && w[i]; i++) if (w_lower(s[i]) != w[i]) return 0;
    return i == n && !w[i];
}

static int jcs_at_kind(const char *name, int n, int block) {
    static const struct { const char *w; int kind, block; } AT[] = {
        { "import", JK_IMPORT, 0 }, { "namespace", JK_NAMESPACE, 0 }, { "layer", JK_LAYERSTMT, 0 },
        { "layer", JK_LAYERBLOCK, 1 }, { "media", JK_MEDIA, 1 }, { "supports", JK_SUPPORTS, 1 },
        { "container", JK_CONTAINER, 1 }, { "scope", JK_SCOPE, 1 }, { "starting-style", JK_STARTING, 1 },
        { "keyframes", JK_KEYFRAMES, 1 }, { "-webkit-keyframes", JK_KEYFRAMES, 1 },
        { "-moz-keyframes", JK_KEYFRAMES, 1 }, { "font-face", JK_FONTFACE, 1 }, { "page", JK_PAGE, 1 },
        { "property", JK_PROPERTY, 1 }, { "counter-style", JK_COUNTER, 1 },
        { "font-feature-values", JK_FONTFEAT, 1 },
    };
    for (u32 i = 0; i < sizeof AT / sizeof AT[0]; i++)
        if (AT[i].block == block && jcs_word(name, n, AT[i].w)) return AT[i].kind;
    return -1;
}

/* A rule as written: its kind, all of it, its head (the selector, or what
   follows the at-keyword) and inside its braces (bs -1 for none). */
typedef struct { int kind, start, end, hs, he, bs, be; } jcs_raw;

/* The rule at *at: 1, or 0 at the end, or -1 for something read past and
   dropped, as a browser drops what it cannot use. */
static int jcs_read(const char *s, int len, int *at, jcs_raw *r, int ctx) {
    int p = jcs_skip(s, len, *at, ctx == JC_SHEET);
    if (p >= len) { *at = len; return 0; }
    r->start = p;
    r->bs = r->be = -1;
    if (s[p] == '}' || s[p] == ';') { *at = p + 1; return -1; }
    int kind;
    if (s[p] == '@') {
        int ne = p + 1;
        while (ne < len && (jd_alnum(s[ne]) || s[ne] == '-' || s[ne] == '_')) ne++;
        int q = jcs_scan(s, len, ne, 1), block = q < len && s[q] == '{';
        kind = jcs_at_kind(s + p + 1, ne - p - 1, block);
        r->hs = ne;
        r->he = q;
        if (block) {
            r->bs = q + 1;
            r->be = jcs_block_end(s, len, q + 1);
            r->end = r->be < len ? r->be + 1 : len;
        } else r->end = q < len && s[q] == ';' ? q + 1 : q;
        if (r->end == r->start) r->end++;
    } else {
        int q = jcs_scan(s, len, p, 0);
        if (q >= len || s[q] != '{') { *at = q < len ? q + 1 : len; return -1; }
        r->hs = p;
        r->he = q;
        r->bs = q + 1;
        r->be = jcs_block_end(s, len, q + 1);
        r->end = r->be < len ? r->be + 1 : len;
        kind = ctx == JC_FRAMES ? JK_KEYFRAME : JK_STYLE;
    }
    *at = r->end;
    while (r->hs < r->he && css_space(s[r->hs])) r->hs++;
    while (r->he > r->hs && css_space(s[r->he - 1])) r->he--;
    r->kind = kind;
    if (kind < 0) return -1;
    if (ctx == JC_FRAMES && kind != JK_KEYFRAME) return -1;
    if (ctx == JC_GROUP && (kind == JK_IMPORT || kind == JK_NAMESPACE)) return -1;
    return 1;
}

/* Whether the style sheets' own parser reads a selector list, all of it
   (css.h, as querySelector does, the sheet put back as it was after). A
   shadow tree's :host and ::slotted() are read as its sheet is written for
   it (css_scope): the parser knows them only so, and a component's adopted
   sheet lost every ::slotted rule. */
static int jcs_selector_ok(const char *s, int len) {
    if (len <= 0) return 0;
    if (!jd_sheet) return 1;
    static char scoped[1024];
    int shadowy = 0;
    for (int i = 0; i + 5 <= len && !shadowy; i++)
        shadowy = s[i] == ':' && (w_starts_fold(s + i, ":host") || w_starts_fold(s + i, "::slotted("));
    if (shadowy && len < (int)sizeof(scoped) / 2) {
        len = css_scope_selectors(s, len, scoped, (int)sizeof(scoped), 0, "x");
        s = scoped;
    }
    int sels = jd_sheet->nsels, used = jd_sheet->used, over = jd_sheet->overflowed, negs = jd_sheet->nnegs;
    int at = 0, ok = 1, parts = 0;
    for (;;) {
        while (at < len && css_space(s[at])) at++;
        int spec = 0, from = at;
        int k = at < len ? css_parse_selector(jd_sheet, s, len, &at, &spec) : 0;
        if (k <= 0 || jd_sheet->overflowed || at == from) { ok = 0; break; }
        parts++;
        while (at < len && css_space(s[at])) at++;
        if (at >= len) break;
        if (s[at] != ',') { ok = 0; break; }
        at++;
    }
    jd_sheet->nsels = sels;
    jd_sheet->used = used;
    jd_sheet->overflowed = over;
    jd_sheet->nnegs = negs;
    return ok && parts > 0;
}

/* A keyframe's keys: from, to and percentages, with commas between. */
static int jcs_keys_ok(const char *s, int len) {
    int at = 0, keys = 0;
    while (at < len) {
        while (at < len && (css_space(s[at]) || s[at] == ',')) at++;
        if (at >= len) break;
        int st = at;
        while (at < len && s[at] != ',' && !css_space(s[at])) at++;
        int n = at - st;
        if (!jcs_word(s + st, n, "from") && !jcs_word(s + st, n, "to")) {
            if (n < 2 || s[st + n - 1] != '%') return 0;
            for (int i = st; i < st + n - 1; i++)
                if (!((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) return 0;
        }
        keys++;
    }
    return keys > 0;
}

static int jcs_valid(const char *s, const jcs_raw *r) {
    int n = r->he - r->hs;
    if (r->kind == JK_STYLE) return jcs_selector_ok(s + r->hs, n);
    if (r->kind == JK_KEYFRAME) return jcs_keys_ok(s + r->hs, n);
    if (r->kind == JK_IMPORT || r->kind == JK_NAMESPACE || r->kind == JK_KEYFRAMES) return n > 0;
    return 1;
}

/* Text with every run of white space made one space. */
static jval jcs_collapsed(const char *s, int n) {
    jtext t = { 0, 0, 0, 0 };
    int gap = 0;
    for (int i = 0; i < n; i++) {
        if (css_space(s[i])) { gap = t.n > 0; continue; }
        if (gap) jd_putc(&t, ' ');
        gap = 0;
        jd_putc(&t, s[i]);
    }
    return js_from_str(jt_done(&jd_J, &t));
}

/* Declarations as a browser writes them back: "name: value;" with a space
   between, names in lower case but for custom properties. Nested rules are
   left as they were written. */
static jval jcs_decls(const char *s, int n) {
    char *st = (char *)malloc((u64)n + 1);
    if (!st) return jd_str("");
    int nested = 0;
    for (int i = 0; i < n; i++) {
        st[i] = s[i];
        if (s[i] == '{') nested = 1;
    }
    st[n] = 0;
    if (nested) {
        jval v = jcs_collapsed(st, n);
        free(st);
        return v;
    }
    jtext t = { 0, 0, 0, 0 };
    jd_decl d;
    int at = 0;
    while (jd_decl_next(st, &at, &d)) {
        if (!d.nl) continue;
        if (t.n) jd_putc(&t, ' ');
        int custom = d.nl > 2 && st[d.ns] == '-' && st[d.ns + 1] == '-';
        for (int i = 0; i < d.nl; i++) jd_putc(&t, custom ? st[d.ns + i] : w_lower(st[d.ns + i]));
        jd_put(&t, ": ");
        jt_put(&jd_J, &t, st + d.vs, (u32)d.vl);
        if (d.imp) jd_put(&t, " !important");
        jd_putc(&t, ';');
    }
    free(st);
    return js_from_str(jt_done(&jd_J, &t));
}

/* --- holders, rules and their text ------------------------------------------------------ */

static jobj *jcs_obj(jobj *o, jstr *key) {
    jval v = o ? jd_kept(o, key) : js_undef();
    return v.t == JS_OBJ ? v.obj : 0;
}

static jstr *jcs_str(jobj *o, jstr *key) {
    jval v = o ? jd_kept(o, key) : js_undef();
    return v.t == JS_STR ? v.str : js_str(&jd_J, "");
}

static int jcs_num(jobj *o, jstr *key, int none) {
    jval v = o ? jd_kept(o, key) : js_undef();
    return v.t == JS_NUM ? (int)v.num : none;
}

/* A sheet's hash, kept as a number: all 32 bits of it. */
static u32 jcs_hash_of(jobj *sh) {
    jval v = sh ? jd_kept(sh, jcs_k_hash) : js_undef();
    return v.t == JS_NUM ? (u32)v.num : 0;
}

static int jcs_is_rule(jobj *o) { return jcs_num(o, jcs_k_kind, -1) >= 0; }
static int jcs_flags(jobj *sh) { return jcs_num(sh, jcs_k_flags, 0); }
static void jcs_set_flags(jobj *sh, int f) { jd_keep(sh, jcs_k_flags, js_num(f)); }

static jobj *jcs_sheet_of(jobj *h) { return !h ? 0 : jcs_is_rule(h) ? jcs_obj(h, jcs_k_sheet) : h; }

/* A holder's rules from text, all of them the kind of rule it can hold. */
static void jcs_fill(jctx *J, jobj *h, const char *s, int len, int ctx) {
    jobj *texts = js_array(J), *objs = js_array(J);
    if (!texts || !objs) return;
    jd_keep(h, jcs_k_texts, js_from_obj(texts));
    jd_keep(h, jcs_k_objs, js_from_obj(objs));
    int at = 0, got, past_imports = 0;
    jcs_raw r;
    while ((got = jcs_read(s, len, &at, &r, ctx)) != 0) {
        if (got < 0 || !jcs_valid(s, &r)) continue;
        /* An @import after any other rule is not one. */
        if (r.kind == JK_IMPORT && past_imports) continue;
        if (r.kind != JK_IMPORT && r.kind != JK_LAYERSTMT && r.kind != JK_NAMESPACE) past_imports = 1;
        js_arr_push(J, texts, jd_str_n(s + r.start, r.end - r.start));
        js_arr_push(J, objs, js_undef());
    }
}

/* A sheet's rules, read from where the sheet came from the first time
   they are wanted: the style element's text, or the linked file, which the
   browser fetches (jsdom_sheet_text_with). */
static void jcs_ready(jctx *J, jobj *sh) {
    int f = jcs_flags(sh);
    if (f & JF_READ) return;
    jcs_set_flags(sh, f | JF_READ);
    int node = jcs_num(sh, jcs_k_owner, -1);
    if (node >= 0 && (f & JF_LINK)) {
        const char *text = 0;
        jstr *href = jcs_str(sh, jcs_k_href);
        int n = jcs_sheet_text && href->len ? jcs_sheet_text(href->s, &text) : -1;
        jcs_fill(J, sh, n > 0 && text ? text : "", n > 0 && text ? n : 0, JC_SHEET);
        return;
    }
    jtext t = { 0, 0, 0, 0 };
    if (node >= 0 && node < jd_doc->count)
        for (int c = jd_doc->nodes[node].first; c >= 0; c = jd_doc->nodes[c].next)
            if (jd_kind(c) == JN_TEXT) jd_put(&t, jd_text_of(c));
    jcs_fill(J, sh, t.b ? t.b : "", (int)t.n, JC_SHEET);
    free(t.b);
}

static jobj *jcs_texts(jctx *J, jobj *h) {
    if (!jcs_is_rule(h)) jcs_ready(J, h);
    return jcs_obj(h, jcs_k_texts);
}

static jobj *jcs_objs(jctx *J, jobj *h) {
    if (!jcs_is_rule(h)) jcs_ready(J, h);
    return jcs_obj(h, jcs_k_objs);
}

static jstr *jcs_text(jctx *J, jobj *r);

static jobj *jcs_rule_new(jctx *J, const char *s, const jcs_raw *r, jobj *sheet, jobj *parent) {
    jobj *o = js_object_with(J, JO_PLAIN, jcs_p_rule[r->kind]);
    if (!o) return 0;
    jd_keep(o, jcs_k_kind, js_num(r->kind));
    if (sheet) jd_keep(o, jcs_k_sheet, js_from_obj(sheet));
    if (parent) jd_keep(o, jcs_k_parent, js_from_obj(parent));
    jd_keep(o, jcs_k_head, jcs_collapsed(s + r->hs, r->he - r->hs));
    int holds = JCS_KIND[r->kind].holds;
    if (holds == JH_DECLS && r->bs >= 0) jd_keep(o, jcs_k_body, jcs_decls(s + r->bs, r->be - r->bs));
    else if (holds == JH_RULES) jcs_fill(J, o, r->bs >= 0 ? s + r->bs : "", r->bs >= 0 ? r->be - r->bs : 0,
                                         r->kind == JK_KEYFRAMES ? JC_FRAMES : JC_GROUP);
    else if (r->bs >= 0) jd_keep(o, jcs_k_body, jcs_collapsed(s + r->bs, r->be - r->bs));
    return o;
}

/* The object for rule i of a holder, made the first time it is asked for. */
static jobj *jcs_rule_at(jctx *J, jobj *h, u32 i) {
    jobj *texts = jcs_texts(J, h), *objs = jcs_objs(J, h);
    if (!texts || !objs || i >= texts->len) return 0;
    if (i < objs->len && objs->items[i].t == JS_OBJ) return objs->items[i].obj;
    jstr *src = texts->items[i].t == JS_STR ? texts->items[i].str : 0;
    if (!src) return 0;
    int ctx = !jcs_is_rule(h) ? JC_SHEET : jcs_num(h, jcs_k_kind, 0) == JK_KEYFRAMES ? JC_FRAMES : JC_GROUP;
    int at = 0;
    jcs_raw r;
    int got;
    while ((got = jcs_read(src->s, (int)src->len, &at, &r, ctx)) < 0) {}
    if (got != 1) return 0;
    jobj *o = jcs_rule_new(J, src->s, &r, jcs_sheet_of(h), jcs_is_rule(h) ? h : 0);
    if (o) js_arr_set(J, objs, i, js_from_obj(o));
    return o;
}

/* cssText, as a browser writes it. */
static jstr *jcs_text(jctx *J, jobj *r) {
    int k = jcs_num(r, jcs_k_kind, 0);
    jstr *head = jcs_str(r, jcs_k_head), *body = jcs_str(r, jcs_k_body);
    jtext t = { 0, 0, 0, 0 };
    if (JCS_KIND[k].at) {
        jd_putc(&t, '@');
        jd_put(&t, JCS_KIND[k].at);
        if (head->len) jd_putc(&t, ' ');
    }
    jt_put(J, &t, head->s, head->len);
    int holds = JCS_KIND[k].holds;
    if (holds == JH_DECLS || (holds == JH_NONE && k == JK_FONTFEAT)) {
        jd_put(&t, t.n ? " { " : "{ ");
        jt_put(J, &t, body->s, body->len);
        jd_put(&t, body->len ? " }" : "}");
    } else if (holds == JH_RULES) {
        jd_put(&t, " {\n");
        jobj *texts = jcs_obj(r, jcs_k_texts), *objs = jcs_obj(r, jcs_k_objs);
        for (u32 i = 0; texts && i < texts->len; i++) {
            jstr *c = objs && i < objs->len && objs->items[i].t == JS_OBJ ? jcs_text(J, objs->items[i].obj)
                    : texts->items[i].t == JS_STR ? texts->items[i].str : 0;
            if (!c) continue;
            jd_put(&t, "  ");
            jt_put(J, &t, c->s, c->len);
            jd_putc(&t, '\n');
        }
        jd_putc(&t, '}');
    } else jd_putc(&t, ';');
    return jt_done(J, &t);
}

static void jcs_publish(jctx *J, jobj *sh);

/* A rule, or a list, was changed: its text goes into its holder's, and so
   on up to the sheet, which is written out again for the browser. A rule
   that has been taken out of its sheet changes nothing. */
static void jcs_changed(jctx *J, jobj *h) {
    for (int guard = 0; h && guard < 64; guard++) {
        if (!jcs_is_rule(h)) { jcs_publish(J, h); return; }
        jobj *up = jcs_obj(h, jcs_k_parent);
        if (!up) up = jcs_obj(h, jcs_k_sheet);
        jobj *objs = up ? jcs_obj(up, jcs_k_objs) : 0, *texts = up ? jcs_obj(up, jcs_k_texts) : 0;
        if (!objs || !texts) return;
        u32 i = 0;
        while (i < objs->len && !(objs->items[i].t == JS_OBJ && objs->items[i].obj == h)) i++;
        if (i >= objs->len || i >= texts->len) return;
        texts->items[i] = js_from_str(jcs_text(J, h));
        h = up;
    }
}

static void jcs_arr_insert(jctx *J, jobj *a, u32 at, jval v) {
    js_arr_push(J, a, js_undef());
    for (u32 k = a->len - 1; k > at; k--) a->items[k] = a->items[k - 1];
    a->items[at] = v;
}

static void jcs_arr_remove(jobj *a, u32 at) {
    if (at >= a->len) return;
    for (u32 k = at; k + 1 < a->len; k++) a->items[k] = a->items[k + 1];
    a->len--;
}

/* insertRule on a sheet or a rule that holds rules: one rule, of a kind
   this holder can hold, put in at index. */
static jval jcs_insert(jctx *J, jobj *h, jstr *text, jval where) {
    jobj *texts = jcs_texts(J, h), *objs = jcs_objs(J, h);
    if (!texts || !objs) return jd_illegal(J);
    double d = where.t == JS_UNDEF ? 0 : js_to_num(J, where);
    if (J->sig != JS_OK) return js_undef();
    if (!(d >= 0 && d <= (double)texts->len))
        return js_throw_dom(J, "IndexSizeError", "there is no place in the list at that index");
    u32 at = (u32)d;
    int ctx = !jcs_is_rule(h) ? JC_SHEET : jcs_num(h, jcs_k_kind, 0) == JK_KEYFRAMES ? JC_FRAMES : JC_GROUP;
    jcs_raw r;
    int pos = 0, got = jcs_read(text->s, (int)text->len, &pos, &r, ctx);
    if (got != 1 || !jcs_valid(text->s, &r))
        return js_throw_dom(J, "SyntaxError", "that is not a rule this can hold");
    jcs_raw more;
    int after = pos;
    if (jcs_read(text->s, (int)text->len, &after, &more, ctx) != 0)
        return js_throw_dom(J, "SyntaxError", "that is more than one rule");
    if (r.kind == JK_IMPORT && (jcs_flags(jcs_sheet_of(h)) & JF_MADE))
        return js_throw_dom(J, "SyntaxError", "a sheet a script made cannot import another");
    jcs_arr_insert(J, texts, at, jd_str_n(text->s + r.start, r.end - r.start));
    jcs_arr_insert(J, objs, at, js_undef());
    jcs_changed(J, h);
    return js_num(at);
}

static jval jcs_delete(jctx *J, jobj *h, jval where) {
    jobj *texts = jcs_texts(J, h), *objs = jcs_objs(J, h);
    if (!texts || !objs) return jd_illegal(J);
    double d = js_to_num(J, where);
    if (J->sig != JS_OK) return js_undef();
    if (!(d >= 0 && d < (double)texts->len))
        return js_throw_dom(J, "IndexSizeError", "there is no rule at that index");
    u32 at = (u32)d;
    if (at < objs->len && objs->items[at].t == JS_OBJ) {
        jd_keep(objs->items[at].obj, jcs_k_sheet, js_undef());
        jd_keep(objs->items[at].obj, jcs_k_parent, js_undef());
    }
    jcs_arr_remove(texts, at);
    jcs_arr_remove(objs, at);
    jcs_changed(J, h);
    return js_undef();
}

/* --- what the browser reads -------------------------------------------------------------- */

/* What a sheet was read from, to tell when it has changed under it: a style
   element's text, or a link's address. */
static u32 jcs_node_hash(int node) {
    if (node < 0 || node >= jd_doc->count) return 0;
    if (jd_doc->nodes[node].tag == T_LINK) {
        const char *h = jd_attr(node, "href");
        u32 x = 2166136261u;
        for (; h && *h; h++) { x ^= (u8)*h; x *= 16777619u; }
        return x;
    }
    return dom_text_hash(jd_doc, node);
}

static int jcs_rec_of(jobj *sh, int make) {
    for (int k = 0; k < jcs_nrec; k++) if (jcs_rec[k].sheet == sh) return k;
    if (!make || jcs_nrec >= JCS_RECS) return -1;
    jcs_rec[jcs_nrec].sheet = sh;
    jcs_rec[jcs_nrec].text = 0;
    jcs_rec[jcs_nrec].stale = 1;
    jcs_rec[jcs_nrec].len = 0;
    return jcs_nrec++;
}

static void jcs_rec_drop(jobj *sh) {
    int k = jcs_rec_of(sh, 0);
    if (k < 0) return;
    free(jcs_rec[k].text);
    jcs_rec[k] = jcs_rec[--jcs_nrec];
    jcs_version++;
    jd_touched();
}

static void jcs_publish(jctx *J, jobj *sh) {
    jcs_ready(J, sh);
    jcs_set_flags(sh, jcs_flags(sh) | JF_MOD);
    int k = jcs_rec_of(sh, 1);
    if (k < 0) return;
    jcs_rec[k].stale = 1;
    jcs_rec[k].node = jcs_num(sh, jcs_k_owner, -1);
    jcs_rec[k].hash = jcs_hash_of(sh);
    jcs_version++;
    jd_touched();
}

/* A record's text, written out now if the sheet has changed since. */
static void jcs_rec_write(jctx *J, int k) {
    if (!jcs_rec[k].stale) return;
    jcs_rec[k].stale = 0;
    jobj *sh = jcs_rec[k].sheet;
    int f = jcs_flags(sh);
    jtext t = { 0, 0, 0, 0 };
    jobj *texts = jcs_obj(sh, jcs_k_texts), *objs = jcs_obj(sh, jcs_k_objs);
    for (u32 i = 0; !(f & JF_OFF) && texts && i < texts->len; i++) {
        jstr *c = objs && i < objs->len && objs->items[i].t == JS_OBJ ? jcs_text(J, objs->items[i].obj)
                : texts->items[i].t == JS_STR ? texts->items[i].str : 0;
        if (!c) continue;
        jt_put(J, &t, c->s, c->len);
        jd_putc(&t, '\n');
    }
    jd_putc(&t, 0);
    free(jcs_rec[k].text);
    jcs_rec[k].text = t.b;
    jcs_rec[k].len = t.n ? (int)t.n - 1 : 0;
}

static jobj *jcs_adopted_list(void) {
    return jd_document_obj ? jcs_obj(jd_document_obj, jcs_k_adopted) : 0;
}

/* The adopted list of the s-th shadow root a script attached (jsdom.h). */
static jobj *jcs_shadow_adopted_list(int s) {
    if (s < 0 || s >= jd_nshadow) return 0;
    jobj *r = jd_element(&jd_J, jd_shadow_root[s]);
    return r ? jcs_obj(r, jcs_k_adopted) : 0;
}

static u32 jcs_list_print(u32 v, jobj *ad) {
    for (u32 i = 0; ad && i < ad->len; i++)
        v = v * 31u + (ad->items[i].t == JS_OBJ ? (u32)(u64)ad->items[i].obj : 7u);
    return ad ? v * 31u + ad->len : v;
}

/* Changes whenever what the browser should read for the page's sheets may
   have: a sheet written out, one dropped, or an adopted list rearranged --
   the document's or a shadow root's -- which a script can do by pushing onto
   the array it was handed. */
__attribute__((unused)) static u32 jsdom_css_version(void) {
    if (!jd_doc || !jd_open) return 0;
    u32 v = jcs_list_print(jcs_version, jcs_adopted_list());
    for (int s = 0; s < jd_nshadow; s++) v = jcs_list_print(v * 31u + (u32)s, jcs_shadow_adopted_list(s));
    return v;
}

/* How often the tree has changed, for the browser to tell a
   getComputedStyle in a loop that nothing has since the last. */
__attribute__((unused)) static u32 jsdom_dom_version(void) { return jd_doc && jd_open ? jd_version : 0; }

/* A script's version of an element's sheet, when it has changed the one
   the element still gives; disabled, it is empty. */
__attribute__((unused)) static int jsdom_sheet_override(int node, const char **text, int *len) {
    if (!jd_doc || !jd_open) return 0;
    for (int k = 0; k < jcs_nrec; k++) {
        if (jcs_rec[k].node != node) continue;
        if (jcs_rec[k].hash != jcs_node_hash(node)) return 0;
        if (text) jcs_rec_write(&jd_J, k);
        if (text) *text = jcs_rec[k].text ? jcs_rec[k].text : "";
        if (len) *len = jcs_rec[k].len;
        return 1;
    }
    return 0;
}

/* The i-th sheet of an adopted list, or 0 past the last: what it holds,
   which is nothing while it is disabled. */
static int jcs_adopted_at(jobj *ad, int i, const char **text, int *len, u32 *id) {
    if (!ad || i < 0 || (u32)i >= ad->len) return 0;
    *text = "";
    *len = 0;
    jobj *sh = ad->items[i].t == JS_OBJ ? ad->items[i].obj : 0;
    int k = sh ? jcs_rec_of(sh, 0) : -1;
    if (k < 0 && sh && (jcs_flags(sh) & JF_MADE)) {
        jcs_publish(&jd_J, sh);
        k = jcs_rec_of(sh, 0);
    }
    if (k >= 0) jcs_rec_write(&jd_J, k);
    if (k >= 0 && jcs_rec[k].text) { *text = jcs_rec[k].text; *len = jcs_rec[k].len; }
    if (id) *id = sh ? (u32)(u64)sh : 0;
    return 1;
}

/* The i-th of document.adoptedStyleSheets. */
__attribute__((unused)) static int jsdom_adopted(int i, const char **text, int *len) {
    return jd_doc && jd_open && jcs_adopted_at(jcs_adopted_list(), i, text, len, 0);
}

/* The i-th adopted sheet of the s-th shadow root a script attached, with a
   number that is the sheet's own, so trees that adopt the same sheets can
   share one reading of them (browser.c, trees_gather). */
__attribute__((unused)) static int jsdom_shadow_adopted(int s, int i, const char **text, int *len, u32 *id) {
    return jd_doc && jd_open && jcs_adopted_at(jcs_shadow_adopted_list(s), i, text, len, id);
}

__attribute__((unused)) static void jsdom_sheet_text_with(int (*fn)(const char *url, const char **text)) {
    jcs_sheet_text = fn;
}

static void jcs_fire_link(jval arg) {
    int v = (int)arg.num, node = v >> 1;
    if (jd_doc && node >= 0 && node < jd_doc->count) jd_fire_simple(node, (v & 1) ? "load" : "error", 0, 0);
}

/* A stylesheet or preloaded link has been fetched, or could not be: its
   load or error event, as a task of its own. */
__attribute__((unused)) static void jsdom_link_loaded(int node, int ok) {
    if (!jd_doc || !jd_open || node < 0 || node >= jd_doc->count) return;
    jd_later_native(jcs_fire_link, js_num(node * 2 + (ok ? 1 : 0)), 0);
}

/* --- sheets ------------------------------------------------------------------------------ */

static int jcs_is_sheet_link(int node) {
    const char *rel = jd_attr(node, "rel");
    return rel && jd_word_in(rel, "stylesheet") && !jd_word_in(rel, "alternate");
}

static jobj *jcs_sheet_new(jctx *J, int node, int flags) {
    jobj *sh = js_object_with(J, JO_PLAIN, jcs_p_sheet);
    if (!sh) return 0;
    jcs_set_flags(sh, flags);
    if (node >= 0) {
        jd_keep(sh, jcs_k_owner, js_num(node));
        jd_keep(sh, jcs_k_hash, js_num(jcs_node_hash(node)));
        if (flags & JF_LINK) {
            const char *h = jd_attr(node, "href");
            jd_keep(sh, jcs_k_href, h && *h ? js_from_str(jd_resolve_str(J, h)) : jd_str(""));
        }
    }
    return sh;
}

/* A style or link element's sheet: none while it is not in the page, or a
   link is not a stylesheet's; a new one when what it holds has changed,
   and the old one then belongs to nobody. */
static jobj *jcs_sheet_for(jctx *J, int node) {
    if (node < 0 || node >= jd_doc->count || !jd_connected(node)) return 0;
    int link = jd_doc->nodes[node].tag == T_LINK;
    if (link ? !jcs_is_sheet_link(node) : jd_doc->nodes[node].tag != T_STYLE) return 0;
    if (link && !jd_attr(node, "href")) return 0;
    jobj *el = jd_element(J, node);
    jobj *sh = jcs_obj(el, jcs_k_sheetobj);
    if (sh && jcs_hash_of(sh) == jcs_node_hash(node)) return sh;
    if (sh) {
        jd_keep(sh, jcs_k_owner, js_undef());
        jcs_rec_drop(sh);
    }
    sh = jcs_sheet_new(J, node, link ? JF_LINK : 0);
    if (sh) jd_keep(el, jcs_k_sheetobj, js_from_obj(sh));
    return sh;
}

static jobj *jcs_this_sheet(jval t) {
    return js_is_obj(t) && t.obj->proto && jcs_num(t.obj, jcs_k_flags, -1) >= 0 && !jcs_is_rule(t.obj) ? t.obj : 0;
}

static jval nat_cssom_sheet(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    jobj *sh = jcs_sheet_for(J, x);
    return sh ? js_from_obj(sh) : js_null();
}

/* A live list: its host number names what it lists, a holder's rules or
   an array of sheets. */
static jobj *jcs_live_list(jctx *J, jobj *of, jobj *proto) {
    jobj *l = js_object_with(J, JO_PLAIN, proto);
    if (!l || !jcs_rlists) return l;
    l->host = JD_RLIST + (int)jcs_rlists->len;
    js_arr_push(J, jcs_rlists, js_from_obj(of));
    return l;
}

static jobj *jcs_list_holder(jval t) {
    if (!js_is_obj(t) || t.obj->host < JD_RLIST || !jcs_rlists) return 0;
    u32 k = (u32)(t.obj->host - JD_RLIST);
    return k < jcs_rlists->len && jcs_rlists->items[k].t == JS_OBJ ? jcs_rlists->items[k].obj : 0;
}

static u32 jcs_list_len(jctx *J, jobj *of) {
    if (of->kind == JO_ARRAY) return of->len;
    jobj *texts = jcs_texts(J, of);
    return texts ? texts->len : 0;
}

static jval jcs_list_item(jctx *J, jobj *of, u32 i) {
    if (of->kind == JO_ARRAY) return i < of->len ? of->items[i] : js_undef();
    jobj *r = jcs_rule_at(J, of, i);
    return r ? js_from_obj(r) : js_undef();
}

static int jcs_list_get(jctx *J, jobj *o, const char *name, jval *out) {
    jobj *of = jcs_list_holder(js_from_obj(o));
    if (!of) return 0;
    if (w_same(name, "length")) { *out = js_num(jcs_list_len(J, of)); return 1; }
    if (name[0] < '0' || name[0] > '9') return 0;
    u32 i = 0;
    for (const char *p = name; *p; p++) {
        if (*p < '0' || *p > '9' || i > 100000000) return 0;
        i = i * 10 + (u32)(*p - '0');
    }
    *out = jcs_list_item(J, of, i);
    return 1;
}

static jval nat_cssom_list_length(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *of = jcs_list_holder(t);
    return of ? js_num(jcs_list_len(J, of)) : jd_illegal(J);
}

static jval nat_cssom_list_item(jctx *J, jval t, jval *a, int n) {
    jobj *of = jcs_list_holder(t);
    if (!of) return jd_illegal(J);
    double d = js_to_num(J, js_arg(a, n, 0));
    if (!(d >= 0 && d < (double)jcs_list_len(J, of))) return js_null();
    jval v = jcs_list_item(J, of, (u32)d);
    return v.t == JS_UNDEF ? js_null() : v;
}

static jval nat_cssom_list_values(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *of = jcs_list_holder(t);
    if (!of) return jd_illegal(J);
    jobj *arr = js_array(J);
    u32 len = jcs_list_len(J, of);
    for (u32 i = 0; arr && i < len; i++) js_arr_push(J, arr, jcs_list_item(J, of, i));
    return jd_array_iter(J, arr, "values");
}

static jval jcs_rules_of(jctx *J, jobj *h) {
    jobj *l = jcs_obj(h, jcs_k_list);
    if (!l) {
        l = jcs_live_list(J, h, jcs_p_rulelist);
        if (l) jd_keep(h, jcs_k_list, js_from_obj(l));
    }
    return l ? js_from_obj(l) : js_null();
}

/* document.styleSheets: the style elements and stylesheet links in the
   page, in its order, made again only when the page has changed. */
static jval nat_cssom_sheets(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    if (jcs_sheetlist && jcs_sheetlist_at == jd_version) return js_from_obj(jcs_sheetlist);
    jobj *arr = js_array(J);
    if (!arr) return js_null();
    for (int i = jd_walk_first(-1); i >= 0; i = jd_walk_next(i, jd_top())) {
        if (jd_kind(i) != JN_ELEMENT) continue;
        int tag = jd_doc->nodes[i].tag;
        if (tag != T_STYLE && tag != T_LINK) continue;
        jobj *sh = jcs_sheet_for(J, i);
        if (sh) js_arr_push(J, arr, js_from_obj(sh));
    }
    jcs_sheetlist = jcs_live_list(J, arr, jcs_p_sheetlist);
    jcs_sheetlist_at = jd_version;
    return jcs_sheetlist ? js_from_obj(jcs_sheetlist) : js_null();
}

static jval nat_cssom_sheet_rules(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *sh = jcs_this_sheet(t);
    return sh ? jcs_rules_of(J, sh) : jd_illegal(J);
}

static jval nat_cssom_sheet_insert(jctx *J, jval t, jval *a, int n) {
    jobj *sh = jcs_this_sheet(t);
    if (!sh) return jd_illegal(J);
    return jcs_insert(J, sh, jd_arg_str(J, a, n, 0), js_arg(a, n, 1));
}

static jval nat_cssom_sheet_delete(jctx *J, jval t, jval *a, int n) {
    jobj *sh = jcs_this_sheet(t);
    if (!sh) return jd_illegal(J);
    return jcs_delete(J, sh, js_arg(a, n, 0));
}

/* The old names: addRule(selector, style, index), which answers -1, and
   removeRule(index), from the first rule. */
static jval nat_cssom_sheet_add(jctx *J, jval t, jval *a, int n) {
    jobj *sh = jcs_this_sheet(t);
    if (!sh) return jd_illegal(J);
    jstr *sel = jd_arg_str(J, a, n, 0), *st = jd_arg_str(J, a, n, 1);
    jtext tx = { 0, 0, 0, 0 };
    jt_put(J, &tx, sel->s, sel->len);
    jd_put(&tx, " { ");
    jt_put(J, &tx, st->s, st->len);
    jd_put(&tx, " }");
    jstr *rule = jt_done(J, &tx);
    jobj *texts = jcs_texts(J, sh);
    jval at = n > 2 && a[2].t != JS_UNDEF ? a[2] : js_num(texts ? texts->len : 0);
    jcs_insert(J, sh, rule, at);
    return js_num(-1);
}

static jval nat_cssom_sheet_remove(jctx *J, jval t, jval *a, int n) {
    jobj *sh = jcs_this_sheet(t);
    if (!sh) return jd_illegal(J);
    return jcs_delete(J, sh, n > 0 ? a[0] : js_num(0));
}

/* replaceSync and replace, only on a sheet a script made; an @import in
   what it is given is left out. */
static jval jcs_replace(jctx *J, jval t, jval *a, int n) {
    jobj *sh = jcs_this_sheet(t);
    if (!sh) return jd_illegal(J);
    if (!(jcs_flags(sh) & JF_MADE))
        return js_throw_dom(J, "NotAllowedError", "only a sheet a script made can be replaced");
    jstr *s = jd_arg_str(J, a, n, 0);
    jobj *objs = jcs_obj(sh, jcs_k_objs);
    for (u32 i = 0; objs && i < objs->len; i++)
        if (objs->items[i].t == JS_OBJ) jd_keep(objs->items[i].obj, jcs_k_sheet, js_undef());
    jcs_fill(J, sh, s->s, (int)s->len, JC_SHEET);
    jobj *texts = jcs_obj(sh, jcs_k_texts);
    objs = jcs_obj(sh, jcs_k_objs);
    for (u32 i = texts ? texts->len : 0; i > 0; i--) {
        jstr *c = texts->items[i - 1].t == JS_STR ? texts->items[i - 1].str : 0;
        int at = 0;
        jcs_raw r;
        if (c && jcs_read(c->s, (int)c->len, &at, &r, JC_SHEET) == 1 && r.kind == JK_IMPORT) {
            jcs_arr_remove(texts, i - 1);
            jcs_arr_remove(objs, i - 1);
        }
    }
    jcs_set_flags(sh, jcs_flags(sh) | JF_READ);
    jcs_publish(J, sh);
    return t;
}

static jval nat_cssom_replace_sync(jctx *J, jval t, jval *a, int n) {
    jval r = jcs_replace(J, t, a, n);
    return J->sig == JS_OK ? js_undef() : r;
}

static jval nat_cssom_replace(jctx *J, jval t, jval *a, int n) {
    jval r = jcs_replace(J, t, a, n);
    jobj *p = js_promise_new(J);
    if (!p) return js_undef();
    if (J->sig == JS_THROWN) {
        jval err = J->ret;
        J->sig = JS_OK;
        js_promise_settle(J, p, 0, err);
    } else js_promise_settle(J, p, 1, r);
    return js_from_obj(p);
}

static jval nat_cssom_sheet_ctor(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "CSSStyleSheet is made with new", J->error_line);
    jobj *sh = jcs_sheet_new(J, -1, JF_MADE | JF_READ);
    if (!sh) return js_undef();
    jcs_fill(J, sh, "", 0, JC_SHEET);
    jval opt = js_arg(a, n, 0);
    if (js_is_obj(opt)) {
        jval m = js_get(J, opt, js_str(J, "media")), off = js_get(J, opt, js_str(J, "disabled"));
        if (J->sig != JS_OK) return js_undef();
        if (m.t != JS_UNDEF) jd_keep(sh, jcs_k_mtext, js_from_str(js_to_str(J, m)));
        if (js_to_bool(off)) jcs_set_flags(sh, jcs_flags(sh) | JF_OFF);
    }
    return js_from_obj(sh);
}

static jval nat_cssom_owner(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *sh = jcs_this_sheet(t);
    if (!sh) return jd_illegal(J);
    int node = jcs_num(sh, jcs_k_owner, -1);
    return node >= 0 ? jd_el_value(J, node) : js_null();
}

static jval nat_cssom_href(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *sh = jcs_this_sheet(t);
    if (!sh) return jd_illegal(J);
    return (jcs_flags(sh) & JF_LINK) ? js_from_str(jcs_str(sh, jcs_k_href)) : js_null();
}

static jval nat_cssom_type(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    return jcs_this_sheet(t) ? jd_str("text/css") : jd_illegal(J);
}

static jval nat_cssom_title(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *sh = jcs_this_sheet(t);
    if (!sh) return jd_illegal(J);
    int node = jcs_num(sh, jcs_k_owner, -1);
    const char *v = node >= 0 ? jd_attr(node, "title") : 0;
    return v ? jd_str(v) : js_null();
}

static jval nat_cssom_disabled(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *sh = jcs_this_sheet(t);
    return sh ? js_bool((jcs_flags(sh) & JF_OFF) != 0) : jd_illegal(J);
}

static jval nat_cssom_set_disabled(jctx *J, jval t, jval *a, int n) {
    jobj *sh = jcs_this_sheet(t);
    if (!sh) return jd_illegal(J);
    int off = js_to_bool(js_arg(a, n, 0)), f = jcs_flags(sh);
    if (off == ((f & JF_OFF) != 0)) return js_undef();
    jcs_set_flags(sh, off ? f | JF_OFF : f & ~JF_OFF);
    jcs_publish(J, sh);
    return js_undef();
}

static jval nat_cssom_null(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_null();
}

/* --- media lists ---------------------------------------------------------------------------
 *
 * A sheet's (its element's media attribute, or what a made one was given)
 * or an @media rule's (its condition), read from there each time. */
static jobj *jcs_media_of(jctx *J, jobj *of) {
    jobj *m = jcs_obj(of, jcs_k_media);
    if (m) return m;
    m = js_object_with(J, JO_PLAIN, jcs_p_medialist);
    if (!m) return 0;
    jd_keep(m, jcs_k_mof, js_from_obj(of));
    jd_keep(of, jcs_k_media, js_from_obj(m));
    return m;
}

static jstr *jcs_media_text(jctx *J, jval t, jobj **of) {
    *of = js_is_obj(t) ? jcs_obj(t.obj, jcs_k_mof) : 0;
    if (!*of) return 0;
    if (jcs_is_rule(*of)) return jcs_str(*of, jcs_k_head);
    int node = jcs_num(*of, jcs_k_owner, -1);
    if (node >= 0) {
        const char *v = jd_attr(node, "media");
        return js_str(J, v ? v : "");
    }
    return jcs_str(*of, jcs_k_mtext);
}

static void jcs_media_store(jctx *J, jobj *of, const char *text) {
    if (jcs_is_rule(of)) {
        jd_keep(of, jcs_k_head, jcs_collapsed(text, w_len(text)));
        jcs_changed(J, of);
        return;
    }
    int node = jcs_num(of, jcs_k_owner, -1);
    if (node >= 0) jd_attr_set(node, "media", text);
    else jd_keep(of, jcs_k_mtext, jd_str(text));
}

/* The media in a list, one by one, commas outside brackets between. */
static int jcs_medium(const jstr *s, int *at, int *st, int *len) {
    int p = *at;
    while (p < (int)s->len && (css_space(s->s[p]) || s->s[p] == ',')) p++;
    if (p >= (int)s->len) { *at = p; return 0; }
    int depth = 0, q = p;
    while (q < (int)s->len && (depth || s->s[q] != ',')) {
        if (s->s[q] == '(') depth++;
        else if (s->s[q] == ')' && depth) depth--;
        q++;
    }
    *at = q;
    *st = p;
    while (q > p && css_space(s->s[q - 1])) q--;
    *len = q - p;
    return 1;
}

static jval nat_cssom_media(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return jd_illegal(J);
    jobj *m = jcs_media_of(J, t.obj);
    return m ? js_from_obj(m) : js_null();
}

static jval nat_cssom_media_text(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *of;
    jstr *s = jcs_media_text(J, t, &of);
    return s ? js_from_str(s) : jd_illegal(J);
}

static jval nat_cssom_media_set(jctx *J, jval t, jval *a, int n) {
    jobj *of;
    if (!jcs_media_text(J, t, &of)) return jd_illegal(J);
    jcs_media_store(J, of, jd_arg_str(J, a, n, 0)->s);
    return js_undef();
}

static jval nat_cssom_media_length(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *of;
    jstr *s = jcs_media_text(J, t, &of);
    if (!s) return jd_illegal(J);
    int at = 0, st, len, k = 0;
    while (jcs_medium(s, &at, &st, &len)) k++;
    return js_num(k);
}

static jval nat_cssom_media_item(jctx *J, jval t, jval *a, int n) {
    jobj *of;
    jstr *s = jcs_media_text(J, t, &of);
    if (!s) return jd_illegal(J);
    double want = js_to_num(J, js_arg(a, n, 0));
    int at = 0, st, len, k = 0;
    while (jcs_medium(s, &at, &st, &len))
        if (k++ == (int)want && want >= 0) return jcs_collapsed(s->s + st, len);
    return js_null();
}

/* appendMedium and deleteMedium (data 0, 1). */
static jval nat_cssom_medium(jctx *J, jval t, jval *a, int n) {
    jobj *of;
    jstr *s = jcs_media_text(J, t, &of);
    if (!s) return jd_illegal(J);
    jstr *m = jd_arg_str(J, a, n, 0);
    int del = (int)J->callee->data.num, at = 0, st, len, found = 0;
    jtext out = { 0, 0, 0, 0 };
    while (jcs_medium(s, &at, &st, &len)) {
        if (len == (int)m->len && jcs_same_fold(s->s + st, m->s, len)) {
            found = 1;
            if (del) continue;
        }
        if (out.n) jd_put(&out, ", ");
        jt_put(J, &out, s->s + st, (u32)len);
    }
    if (del && !found) { free(out.b); return js_throw_dom(J, "NotFoundError", "that medium is not in the list"); }
    if (!del && !found) {
        if (out.n) jd_put(&out, ", ");
        jt_put(J, &out, m->s, m->len);
    }
    jd_putc(&out, 0);
    jcs_media_store(J, of, out.b ? out.b : "");
    free(out.b);
    return js_undef();
}

/* --- rules --------------------------------------------------------------------------------- */

static jobj *jcs_this_rule(jval t, int kind) {
    if (!js_is_obj(t) || !jcs_is_rule(t.obj)) return 0;
    return kind < 0 || jcs_num(t.obj, jcs_k_kind, -1) == kind ? t.obj : 0;
}

static jval nat_cssom_rule_text(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *r = jcs_this_rule(t, -1);
    return r ? js_from_str(jcs_text(J, r)) : jd_illegal(J);
}

static jval nat_cssom_rule_type(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *r = jcs_this_rule(t, -1);
    return r ? js_num(JCS_KIND[jcs_num(r, jcs_k_kind, 0)].type) : jd_illegal(J);
}

static jval nat_cssom_rule_sheet(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *r = jcs_this_rule(t, -1);
    if (!r) return jd_illegal(J);
    jobj *sh = jcs_obj(r, jcs_k_sheet);
    return sh ? js_from_obj(sh) : js_null();
}

static jval nat_cssom_rule_parent(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *r = jcs_this_rule(t, -1);
    if (!r) return jd_illegal(J);
    jobj *p = jcs_obj(r, jcs_k_parent);
    return p ? js_from_obj(p) : js_null();
}

/* The head, whatever a kind calls it: selectorText, keyText, a name, a
   condition. Written, it is checked as the kind would read it. */
static jval nat_cssom_head(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *r = jcs_this_rule(t, -1);
    return r ? js_from_str(jcs_str(r, jcs_k_head)) : jd_illegal(J);
}

static jval nat_cssom_set_head(jctx *J, jval t, jval *a, int n) {
    jobj *r = jcs_this_rule(t, -1);
    if (!r) return jd_illegal(J);
    jstr *s = jd_arg_str(J, a, n, 0);
    int k = jcs_num(r, jcs_k_kind, 0);
    if (k == JK_STYLE && !jcs_selector_ok(s->s, (int)s->len)) return js_undef();
    if (k == JK_KEYFRAME && !jcs_keys_ok(s->s, (int)s->len))
        return js_throw_dom(J, "SyntaxError", "that is not a keyframe's keys");
    jd_keep(r, jcs_k_head, jcs_collapsed(s->s, (int)s->len));
    jcs_changed(J, r);
    return js_undef();
}

/* The part of the head before the first space: a container's or layer's
   name, a @scope's start. */
static jval nat_cssom_head_word(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *r = jcs_this_rule(t, -1);
    if (!r) return jd_illegal(J);
    jstr *h = jcs_str(r, jcs_k_head);
    u32 e = 0;
    while (e < h->len && h->s[e] != ' ' && h->s[e] != '(') e++;
    return jd_str_n(h->s, (int)e);
}

static jval nat_cssom_head_rest(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *r = jcs_this_rule(t, -1);
    if (!r) return jd_illegal(J);
    jstr *h = jcs_str(r, jcs_k_head);
    u32 e = 0;
    while (e < h->len && h->s[e] != ' ' && h->s[e] != '(') e++;
    while (e < h->len && h->s[e] == ' ') e++;
    return jd_str_n(h->s + e, (int)(h->len - e));
}

static jval nat_cssom_rule_style(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *r = jcs_this_rule(t, -1);
    if (!r || JCS_KIND[jcs_num(r, jcs_k_kind, 0)].holds != JH_DECLS) return jd_illegal(J);
    jobj *st = jcs_obj(r, jcs_k_style);
    if (st) return js_from_obj(st);
    st = js_object_with(J, JO_PLAIN, jd_p[JI_STYLEDECL]);
    if (!st || !jcs_rstyles) return js_null();
    st->host = JD_RSTYLE + (int)jcs_rstyles->len;
    js_arr_push(J, jcs_rstyles, js_from_obj(r));
    jd_keep(r, jcs_k_style, js_from_obj(st));
    return js_from_obj(st);
}

static jval nat_cssom_set_rule_style(jctx *J, jval t, jval *a, int n) {
    jobj *r = jcs_this_rule(t, -1);
    if (!r || JCS_KIND[jcs_num(r, jcs_k_kind, 0)].holds != JH_DECLS) return jd_illegal(J);
    jcs_rule_set_decls(r, jd_arg_str(J, a, n, 0)->s);
    return js_undef();
}

static jobj *jcs_style_rule(jval t) {
    if (!js_is_obj(t) || t.obj->host < JD_RSTYLE || t.obj->host >= JD_RLIST || !jcs_rstyles) return 0;
    u32 k = (u32)(t.obj->host - JD_RSTYLE);
    return k < jcs_rstyles->len && jcs_rstyles->items[k].t == JS_OBJ ? jcs_rstyles->items[k].obj : 0;
}

static const char *jcs_rule_decls(jobj *rule) { return jcs_str(rule, jcs_k_body)->s; }

static void jcs_rule_set_decls(jobj *rule, const char *text) {
    jd_keep(rule, jcs_k_body, jcs_decls(text, w_len(text)));
    jcs_changed(&jd_J, rule);
}

static jval nat_cssom_parent_rule(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    jobj *r = jcs_style_rule(t);
    return r ? js_from_obj(r) : js_null();
}

static jval nat_cssom_group_rules(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *r = jcs_this_rule(t, -1);
    if (!r || JCS_KIND[jcs_num(r, jcs_k_kind, 0)].holds != JH_RULES) return jd_illegal(J);
    return jcs_rules_of(J, r);
}

static jval nat_cssom_group_insert(jctx *J, jval t, jval *a, int n) {
    jobj *r = jcs_this_rule(t, -1);
    if (!r || JCS_KIND[jcs_num(r, jcs_k_kind, 0)].holds != JH_RULES) return jd_illegal(J);
    return jcs_insert(J, r, jd_arg_str(J, a, n, 0), js_arg(a, n, 1));
}

static jval nat_cssom_group_delete(jctx *J, jval t, jval *a, int n) {
    jobj *r = jcs_this_rule(t, -1);
    if (!r || JCS_KIND[jcs_num(r, jcs_k_kind, 0)].holds != JH_RULES) return jd_illegal(J);
    return jcs_delete(J, r, js_arg(a, n, 0));
}

/* @keyframes: frames added at the end, and found or taken out by their
   keys, the last that matches. */
static int jcs_frame_index(jctx *J, jobj *r, jstr *key) {
    jobj *texts = jcs_texts(J, r);
    jval want = jcs_collapsed(key->s, (int)key->len);
    for (u32 i = texts ? texts->len : 0; i > 0; i--) {
        jobj *f = jcs_rule_at(J, r, i - 1);
        jstr *k = f ? jcs_str(f, jcs_k_head) : 0;
        if (k && want.t == JS_STR && k->len == want.str->len && jcs_same_fold(k->s, want.str->s, (int)k->len))
            return (int)i - 1;
    }
    return -1;
}

static jval nat_cssom_frames_append(jctx *J, jval t, jval *a, int n) {
    jobj *r = jcs_this_rule(t, JK_KEYFRAMES);
    if (!r) return jd_illegal(J);
    jobj *texts = jcs_texts(J, r);
    jcs_insert(J, r, jd_arg_str(J, a, n, 0), js_num(texts ? texts->len : 0));
    return js_undef();
}

static jval nat_cssom_frames_delete(jctx *J, jval t, jval *a, int n) {
    jobj *r = jcs_this_rule(t, JK_KEYFRAMES);
    if (!r) return jd_illegal(J);
    int i = jcs_frame_index(J, r, jd_arg_str(J, a, n, 0));
    if (i >= 0) jcs_delete(J, r, js_num(i));
    return js_undef();
}

static jval nat_cssom_frames_find(jctx *J, jval t, jval *a, int n) {
    jobj *r = jcs_this_rule(t, JK_KEYFRAMES);
    if (!r) return jd_illegal(J);
    int i = jcs_frame_index(J, r, jd_arg_str(J, a, n, 0));
    jobj *f = i >= 0 ? jcs_rule_at(J, r, (u32)i) : 0;
    return f ? js_from_obj(f) : js_null();
}

/* @import's address, from url("...") or a bare string. */
static jval nat_cssom_import_href(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *r = jcs_this_rule(t, JK_IMPORT);
    if (!r) return jd_illegal(J);
    jstr *h = jcs_str(r, jcs_k_head);
    u32 p = 0;
    if (h->len > 4 && jcs_same_fold(h->s, "url(", 4)) p = 4;
    while (p < h->len && css_space(h->s[p])) p++;
    char q = p < h->len && (h->s[p] == '"' || h->s[p] == '\'') ? h->s[p++] : 0;
    u32 e = p;
    while (e < h->len && (q ? h->s[e] != q : (h->s[e] != ')' && !css_space(h->s[e])))) e++;
    return jd_str_n(h->s + p, (int)(e - p));
}

/* A @property's descriptors, from its declarations. */
static jval nat_cssom_descriptor(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *r = jcs_this_rule(t, -1);
    if (!r) return jd_illegal(J);
    static const char *const NAMES[] = { "syntax", "inherits", "initial-value" };
    int which = (int)J->callee->data.num;
    jd_decls src = { -1, r };
    jval v = jd_style_value(&src, NAMES[which], 0);
    if (which == 1) return js_bool(v.t == JS_STR && js_str_is(v.str, "true"));
    if (which == 2 && v.t == JS_STR && !v.str->len) return js_null();
    if (which == 0 && v.t == JS_STR && v.str->len >= 2 && (v.str->s[0] == '"' || v.str->s[0] == '\''))
        return jd_str_n(v.str->s + 1, (int)v.str->len - 2);
    return v;
}

/* A @layer statement's names, as a frozen list. */
static jval nat_cssom_layer_names(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *r = jcs_this_rule(t, JK_LAYERSTMT);
    if (!r) return jd_illegal(J);
    jstr *h = jcs_str(r, jcs_k_head);
    jobj *arr = js_array(J);
    int at = 0, st, len;
    while (arr && jcs_medium(h, &at, &st, &len)) js_arr_push(J, arr, jd_str_n(h->s + st, len));
    if (arr) arr->flags |= JOF_FROZEN;
    return arr ? js_from_obj(arr) : js_null();
}

/* --- adopted sheets ------------------------------------------------------------------------ */

static jval nat_cssom_adopted(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!js_is_obj(t)) return jd_illegal(J);
    jobj *arr = jcs_obj(t.obj, jcs_k_adopted);
    if (!arr) {
        arr = js_array(J);
        if (!arr) return js_null();
        jd_keep(t.obj, jcs_k_adopted, js_from_obj(arr));
    }
    return js_from_obj(arr);
}

/* Only sheets a script made may be adopted. What is kept is a copy of the
   list, the one the getter then hands out, so pushing onto that is seen
   and changing the array that was given is not, as in a browser. */
static jval nat_cssom_set_adopted(jctx *J, jval t, jval *a, int n) {
    if (!js_is_obj(t)) return jd_illegal(J);
    jargs A;
    js_args_init(&A);
    js_iter_collect(J, js_arg(a, n, 0), &A);
    if (J->sig != JS_OK) { js_args_free(&A); return js_undef(); }
    for (int i = 0; i < A.n; i++) {
        jobj *sh = jcs_this_sheet(A.v[i]);
        if (!sh || !(jcs_flags(sh) & JF_MADE)) {
            js_args_free(&A);
            return js_throw_dom(J, "NotAllowedError", "only sheets a script made can be adopted");
        }
    }
    jobj *arr = js_array(J);
    for (int i = 0; arr && i < A.n; i++) js_arr_push(J, arr, A.v[i]);
    js_args_free(&A);
    if (arr) jd_keep(t.obj, jcs_k_adopted, js_from_obj(arr));
    jcs_version++;
    jd_touched();
    return js_undef();
}

/* --- setting it up ------------------------------------------------------------------------- */

static void jd_setup_cssom(jctx *J) {
    static const char *const KEYS[] = { "texts", "rules", "owner", "hash", "flags", "href", "list", "sheet",
                                        "parent", "kind", "head", "body", "style", "media", "mediaOf",
                                        "sheetObject", "adopted", "mediaText" };
    jstr **keys[] = { &jcs_k_texts, &jcs_k_objs, &jcs_k_owner, &jcs_k_hash, &jcs_k_flags, &jcs_k_href,
                      &jcs_k_list, &jcs_k_sheet, &jcs_k_parent, &jcs_k_kind, &jcs_k_head, &jcs_k_body,
                      &jcs_k_style, &jcs_k_media, &jcs_k_mof, &jcs_k_sheetobj, &jcs_k_adopted, &jcs_k_mtext };
    for (u32 i = 0; i < sizeof keys / sizeof keys[0]; i++) *keys[i] = js_sym_new(J, KEYS[i], (u32)w_len(KEYS[i]));
    for (int k = 0; k < jcs_nrec; k++) free(jcs_rec[k].text);
    jcs_nrec = 0;
    jcs_version = 0;
    jcs_sheetlist = 0;
    jcs_rstyles = js_array(J);
    jcs_rlists = js_array(J);

    jcs_p_stylesheet = jd_interface(J, "StyleSheet", 0, 0, 0);
    jobj *ss = jcs_p_stylesheet;
    jd_accessor(J, ss, "type", nat_cssom_type, 0);
    jd_accessor(J, ss, "href", nat_cssom_href, 0);
    jd_accessor(J, ss, "ownerNode", nat_cssom_owner, 0);
    jd_accessor(J, ss, "parentStyleSheet", nat_cssom_null, 0);
    jd_accessor(J, ss, "title", nat_cssom_title, 0);
    jd_accessor(J, ss, "media", nat_cssom_media, 0);
    jd_accessor(J, ss, "disabled", nat_cssom_disabled, nat_cssom_set_disabled);
    jcs_p_sheet = jd_interface(J, "CSSStyleSheet", ss, nat_cssom_sheet_ctor, 0);
    jobj *cs = jcs_p_sheet;
    jd_accessor(J, cs, "cssRules", nat_cssom_sheet_rules, 0);
    jd_accessor(J, cs, "rules", nat_cssom_sheet_rules, 0);
    jd_accessor(J, cs, "ownerRule", nat_cssom_null, 0);
    jd_method(J, cs, "insertRule", nat_cssom_sheet_insert, 1);
    jd_method(J, cs, "deleteRule", nat_cssom_sheet_delete, 1);
    jd_method(J, cs, "addRule", nat_cssom_sheet_add, 0);
    jd_method(J, cs, "removeRule", nat_cssom_sheet_remove, 0);
    jd_method(J, cs, "replace", nat_cssom_replace, 1);
    jd_method(J, cs, "replaceSync", nat_cssom_replace_sync, 1);

    jcs_p_rulelist = jd_interface(J, "CSSRuleList", 0, 0, 0);
    jd_accessor(J, jcs_p_rulelist, "length", nat_cssom_list_length, 0);
    jd_method(J, jcs_p_rulelist, "item", nat_cssom_list_item, 1);
    js_method_key(J, jcs_p_rulelist, J->sym_iterator, "[Symbol.iterator]", nat_cssom_list_values, 0);
    jcs_p_sheetlist = jd_interface(J, "StyleSheetList", 0, 0, 0);
    jd_accessor(J, jcs_p_sheetlist, "length", nat_cssom_list_length, 0);
    jd_method(J, jcs_p_sheetlist, "item", nat_cssom_list_item, 1);
    js_method_key(J, jcs_p_sheetlist, J->sym_iterator, "[Symbol.iterator]", nat_cssom_list_values, 0);

    jcs_p_medialist = jd_interface(J, "MediaList", 0, 0, 0);
    jobj *ml = jcs_p_medialist;
    jd_accessor(J, ml, "mediaText", nat_cssom_media_text, nat_cssom_media_set);
    jd_accessor(J, ml, "length", nat_cssom_media_length, 0);
    jd_method(J, ml, "item", nat_cssom_media_item, 1);
    jd_method(J, ml, "toString", nat_cssom_media_text, 0);
    jd_fn(J, ml, "appendMedium", nat_cssom_medium, 1, 0);
    jd_fn(J, ml, "deleteMedium", nat_cssom_medium, 1, 1);

    /* The rules, parents first, as the standard has them. */
    jobj *rule = jd_interface(J, "CSSRule", 0, 0, 0);
    jd_accessor(J, rule, "cssText", nat_cssom_rule_text, nat_nothing_js);
    jd_accessor(J, rule, "type", nat_cssom_rule_type, 0);
    jd_accessor(J, rule, "parentStyleSheet", nat_cssom_rule_sheet, 0);
    jd_accessor(J, rule, "parentRule", nat_cssom_rule_parent, 0);
    static const struct { const char *name; int n; } TYPES[] = {
        { "STYLE_RULE", 1 }, { "CHARSET_RULE", 2 }, { "IMPORT_RULE", 3 }, { "MEDIA_RULE", 4 },
        { "FONT_FACE_RULE", 5 }, { "PAGE_RULE", 6 }, { "KEYFRAMES_RULE", 7 }, { "KEYFRAME_RULE", 8 },
        { "MARGIN_RULE", 9 }, { "NAMESPACE_RULE", 10 }, { "COUNTER_STYLE_RULE", 11 }, { "SUPPORTS_RULE", 12 },
        { "FONT_FEATURE_VALUES_RULE", 14 },
    };
    jobj *rc = jd_ctor_of(rule);
    for (u32 i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++) {
        js_const_prop(J, rule, TYPES[i].name, js_num(TYPES[i].n));
        if (rc) js_const_prop(J, rc, TYPES[i].name, js_num(TYPES[i].n));
    }
    jobj *group = jd_interface(J, "CSSGroupingRule", rule, 0, 0);
    jd_accessor(J, group, "cssRules", nat_cssom_group_rules, 0);
    jd_method(J, group, "insertRule", nat_cssom_group_insert, 1);
    jd_method(J, group, "deleteRule", nat_cssom_group_delete, 1);
    jobj *cond = jd_interface(J, "CSSConditionRule", group, 0, 0);
    jd_accessor(J, cond, "conditionText", nat_cssom_head, 0);
    for (int k = 0; k < JK_COUNT; k++) {
        int holds = JCS_KIND[k].holds;
        jobj *parent = k == JK_MEDIA || k == JK_SUPPORTS || k == JK_CONTAINER ? cond
                     : holds == JH_RULES && k != JK_KEYFRAMES ? group : rule;
        jcs_p_rule[k] = jd_interface(J, JCS_KIND[k].iface, parent, 0, 0);
        jobj *p = jcs_p_rule[k];
        if (holds == JH_DECLS) jd_accessor(J, p, "style", nat_cssom_rule_style, nat_cssom_set_rule_style);
    }
    jd_accessor(J, jcs_p_rule[JK_STYLE], "selectorText", nat_cssom_head, nat_cssom_set_head);
    jd_accessor(J, jcs_p_rule[JK_PAGE], "selectorText", nat_cssom_head, nat_cssom_set_head);
    jd_accessor(J, jcs_p_rule[JK_KEYFRAME], "keyText", nat_cssom_head, nat_cssom_set_head);
    jd_accessor(J, jcs_p_rule[JK_MEDIA], "media", nat_cssom_media, 0);
    jobj *kf = jcs_p_rule[JK_KEYFRAMES];
    jd_accessor(J, kf, "name", nat_cssom_head, nat_cssom_set_head);
    jd_accessor(J, kf, "cssRules", nat_cssom_group_rules, 0);
    jd_accessor(J, kf, "length", nat_cssom_list_length, 0);
    jd_method(J, kf, "appendRule", nat_cssom_frames_append, 1);
    jd_method(J, kf, "deleteRule", nat_cssom_frames_delete, 1);
    jd_method(J, kf, "findRule", nat_cssom_frames_find, 1);
    jobj *im = jcs_p_rule[JK_IMPORT];
    jd_accessor(J, im, "href", nat_cssom_import_href, 0);
    jd_accessor(J, im, "media", nat_cssom_media, 0);
    jd_accessor(J, im, "styleSheet", nat_cssom_null, 0);
    jd_accessor(J, im, "layerName", nat_cssom_null, 0);
    jd_accessor(J, im, "supportsText", nat_cssom_null, 0);
    jd_accessor(J, jcs_p_rule[JK_NAMESPACE], "prefix", nat_cssom_head_word, 0);
    jd_accessor(J, jcs_p_rule[JK_NAMESPACE], "namespaceURI", nat_cssom_head_rest, 0);
    jd_accessor(J, jcs_p_rule[JK_LAYERBLOCK], "name", nat_cssom_head, 0);
    jd_accessor(J, jcs_p_rule[JK_LAYERSTMT], "nameList", nat_cssom_layer_names, 0);
    jd_accessor(J, jcs_p_rule[JK_CONTAINER], "containerName", nat_cssom_head_word, 0);
    jd_accessor(J, jcs_p_rule[JK_CONTAINER], "containerQuery", nat_cssom_head_rest, 0);
    jd_accessor(J, jcs_p_rule[JK_COUNTER], "name", nat_cssom_head, 0);
    jobj *pr = jcs_p_rule[JK_PROPERTY];
    jd_accessor(J, pr, "name", nat_cssom_head, 0);
    jd_getter_data(J, pr, "syntax", nat_cssom_descriptor, 0);
    jd_getter_data(J, pr, "inherits", nat_cssom_descriptor, 1);
    jd_getter_data(J, pr, "initialValue", nat_cssom_descriptor, 2);

    jd_accessor(J, jd_p[JI_STYLEDECL], "parentRule", nat_cssom_parent_rule, 0);
    jd_accessor(J, jd_p[JI_DOCUMENT], "styleSheets", nat_cssom_sheets, 0);
    jd_accessor(J, jd_p[JI_DOCUMENT], "adoptedStyleSheets", nat_cssom_adopted, nat_cssom_set_adopted);
    if (jd_p_shadowroot)
        jd_accessor(J, jd_p_shadowroot, "adoptedStyleSheets", nat_cssom_adopted, nat_cssom_set_adopted);
}
