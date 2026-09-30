/* Modules: <script type="module">, import and export, import() and
 * import.meta, for the pages that are nothing else -- GitHub, MDN, Reddit,
 * Cloudflare and BBC Sport ran none of their scripts.
 *
 * A module is fetched and read as a module (js_parse_module), and what it
 * imports is fetched and read the same way, each address once: the module
 * map, jd_mods. Then linked: each module gets a scope of its own under the
 * global one (jscope.module is its record, which import() and import.meta
 * written inside it look for); its exports become getters on a namespace
 * object that read its bindings when asked, so a binding the exporter
 * changes later is seen changed, as the standard's live bindings are; and
 * each import becomes a getter in the importer's scope reading the
 * exporter's namespace. Then run: the modules it imports first, each once.
 *
 * An address is resolved against the module's own (the page's, for one
 * written in the page), and a bare name ("react") through the page's import
 * map, the first <script type="importmap">: its "imports", exact names and
 * prefixes ending in a slash. Module scripts run after the page's classic
 * ones, in the order they were written, as defer has them; a nomodule
 * script does not run, since this browser has modules.
 *
 * A module that awaits at its top level (NF_TLA) runs as an async body does
 * (js_run_module_async) and is JM_ASYNC until its promise settles; one that
 * imports a module still pending is JM_WAITING, counting what it waits for,
 * and runs when the last of them has finished. Each has a promise, done, that
 * settles when it has finished, which is what those that import it and an
 * import() of it wait on. The next module script of the page does not wait,
 * as the standard has it.
 *
 * A data: address is read from itself and a blob: one from its Blob, never
 * asked of the network; nothing is relative to either, as the standard has it.
 *
 * Not done: an import map's scopes; import attributes,
 * which are read and set aside, so a JSON module is refused as the script it
 * is not; and a cycle is only as right as reading bindings late makes it. */
#pragma once

#define JD_MODS_MAX 256
#define JD_URL_MAX  1024

enum { JM_NEW, JM_PARSED, JM_LINKED, JM_RUNNING, JM_WAITING, JM_ASYNC, JM_DONE, JM_FAILED };

typedef struct {
    char   *key;          /* its address, or inline:N for one in the page */
    char   *url;          /* what its imports are resolved against, and import.meta.url */
    int     state;
    int     prog;
    jscope *scope;
    jobj   *ns;           /* the namespace: a getter for each export */
    jobj   *meta;
    jval    error;        /* what it threw, for an import() of it */
    jobj   *done;         /* settles when it has finished, for one that did not at once */
    int     waiting;      /* how many of what it imports it is still waiting for */
} jmod;

static jmod jd_mods[JD_MODS_MAX];
static int jd_nmods;
static jobj *jd_imports;             /* the import map's "imports" */

static char *jd_mod_dup(const char *s) {
    int n = 0;
    while (s[n]) n++;
    char *d = (char *)malloc((u64)n + 1);
    if (!d) return 0;
    for (int i = 0; i <= n; i++) d[i] = s[i];
    return d;
}

static void jd_mods_reset(void) {
    for (int i = 0; i < jd_nmods; i++) {
        free(jd_mods[i].key);
        free(jd_mods[i].url);
    }
    jd_nmods = 0;
    jd_imports = 0;
}

static int jd_mod_find(const char *key) {
    for (int i = 0; i < jd_nmods; i++)
        if (jd_mods[i].key && w_same(jd_mods[i].key, key)) return i;
    return -1;
}

static int jd_mod_add(const char *key, const char *url) {
    if (jd_nmods >= JD_MODS_MAX) return -1;
    jmod *m = &jd_mods[jd_nmods];
    m->key = jd_mod_dup(key);
    m->url = jd_mod_dup(url);
    if (!m->key || !m->url) { free(m->key); free(m->url); return -1; }
    m->state = JM_NEW;
    m->prog = -1;
    m->scope = 0;
    m->ns = 0;
    m->meta = 0;
    m->error = js_undef();
    m->done = 0;
    m->waiting = 0;
    return jd_nmods++;
}

/* --- where a name leads ------------------------------------------------------------ */

/* Neither a path (/, ./, ../) nor an address with a scheme: a name only an
   import map can say the place of. */
static int jd_is_bare(const char *s) {
    if (s[0] == '/') return 0;
    if (s[0] == '.' && (s[1] == '/' || (s[1] == '.' && s[2] == '/'))) return 0;
    char c = s[0];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return 1;
    int i = 1;
    while ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9')
           || s[i] == '+' || s[i] == '-' || s[i] == '.') i++;
    return s[i] != ':';
}

static int jd_importmap_lookup(const char *spec, char *out, int cap) {
    if (!jd_imports) return 0;
    jctx *J = &jd_J;
    jval v = js_get(J, js_from_obj(jd_imports), js_str(J, spec));
    const char *to = 0, *rest = "";
    if (v.t == JS_STR) {
        to = v.str->s;
    } else {
        /* The longest prefix that ends in a slash. */
        jprop **own;
        u32 n = js_keys_of(J, jd_imports, &own, JK_STR);
        u32 best = 0;
        for (u32 i = 0; i < n; i++) {
            jstr *k = own[i]->key;
            if (!k->len || k->s[k->len - 1] != '/' || k->len <= best || own[i]->v.t != JS_STR) continue;
            u32 j = 0;
            while (j < k->len && spec[j] == k->s[j]) j++;
            if (j == k->len) { best = k->len; to = own[i]->v.str->s; rest = spec + k->len; }
        }
    }
    if (!to) return 0;
    int w = 0;
    for (const char *p = to; *p && w < cap - 1; p++) out[w++] = *p;
    for (const char *p = rest; *p && w < cap - 1; p++) out[w++] = *p;
    out[w] = 0;
    return 1;
}

/* A module's name made its address: against `base`, or, for a bare name,
   what the import map gives it, against the page. A data: or blob: address
   is its own, whatever its length: out is not used for one, and what comes
   back is the name itself. 0 when it leads nowhere. */
static const char *jd_mod_resolve(const char *spec, const char *base, char *out, int cap) {
    if (jd_is_data_url(spec) || jd_is_blob_url(spec)) return spec;
    char mapped[JD_URL_MAX];
    const char *target = spec, *against = base;
    if (jd_is_bare(spec)) {
        if (!jd_importmap_lookup(spec, mapped, (int)sizeof(mapped))) return 0;
        if (jd_is_data_url(mapped) || jd_is_blob_url(mapped)) {
            w_copy(out, cap, mapped, cap);
            return out;
        }
        target = mapped;
        against = jd_address;
    }
    jurl *b = (jurl *)malloc(sizeof(jurl));
    jurl *u = (jurl *)malloc(sizeof(jurl));
    int ok = 0;
    if (b && u && ju_parse(against, 0, b) && ju_parse(target, b, u)) {
        jtext t = { 0, 0, 0, 0 };
        ju_text(u, &t, 0);
        int k = 0;
        for (; k < (int)t.n && k < cap - 1; k++) out[k] = t.b[k];
        out[k] = 0;
        free(t.b);
        ok = k > 0;
    }
    free(b);
    free(u);
    return ok ? out : 0;
}

/* The page's import map, read once as the scripts start. */
static void jd_importmap_read(void) {
    jd_imports = 0;
    for (int i = 0; i < jd_parsed && i < jd_doc->count; i++) {
        if (jd_doc->nodes[i].kind != DN_ELEMENT || jd_doc->nodes[i].tag != T_SCRIPT) continue;
        const char *ty = dom_attr(jd_doc, i, "type");
        if (!ty || !w_same(ty, "importmap") || !jd_connected(i)) continue;
        jtext t = { 0, 0, 0, 0 };
        jd_text_content(i, &t);
        jctx *J = &jd_J;
        jsignal outer = J->sig;
        jval json = js_get_str(J, js_from_obj(J->global_obj), "JSON");
        jval parse = js_get_str(J, json, "parse");
        jval text = js_from_str(js_str_n(J, t.b ? t.b : "", t.n));
        free(t.b);
        jval map = js_callable(parse) ? js_call(J, parse, json, &text, 1) : js_undef();
        if (J->sig == JS_OK && js_is_obj(map)) {
            jval imports = js_get_str(J, map, "imports");
            if (js_is_obj(imports)) jd_imports = imports.obj;
        }
        J->sig = outer;
        return;
    }
}

/* --- reading -------------------------------------------------------------------------- */

static void jd_mod_failed(jmod *m, const char *why) {
    m->state = JM_FAILED;
    char msg[160];
    int w = 0;
    for (const char *p = why; *p && w < 80; p++) msg[w++] = *p;
    msg[w++] = ':';
    msg[w++] = ' ';
    for (const char *p = m->url; *p && w < (int)sizeof(msg) - 1; p++) msg[w++] = *p;
    msg[w] = 0;
    jd_note_text(msg);
    jctx *J = &jd_J;
    jobj *e = js_error_with(J, J->err_proto[JS_ERR_TYPE], js_str(J, "TypeError"), js_str(J, msg));
    m->error = e ? js_from_obj(e) : js_undef();
}

static void jd_mod_parse(int i, const char *text, u32 len) {
    jmod *m = &jd_mods[i];
    jctx *J = &jd_J;
    jsignal outer = J->sig;
    jval outer_ret = J->ret;
    J->sig = JS_OK;
    int prog = js_parse_module(J, text, len);
    if (prog < 0) {
        char why[140];
        int w = 0;
        for (const char *p = "a module could not be read ("; *p; p++) why[w++] = *p;
        for (const char *p = J->error; *p && w < (int)sizeof(why) - 2; p++) why[w++] = *p;
        why[w++] = ')';
        why[w] = 0;
        J->sig = outer;
        J->ret = outer_ret;
        jd_mod_failed(m, why);
        return;
    }
    J->sig = outer;
    J->ret = outer_ret;
    m->prog = prog;
    m->state = JM_PARSED;
}

/* The address of what a declaration of module m names, as an index into the
   map, or -1. */
static int jd_mod_child(jmod *m, int st) {
    jstr *s = jd_J.nodes[st].str;
    char url[JD_URL_MAX];
    const char *to = s ? jd_mod_resolve(s->s, m->url, url, (int)sizeof(url)) : 0;
    return to ? jd_mod_find(to) : -1;
}

static int jd_mod_is_request(int st) {
    int k = jd_J.nodes[st].kind;
    return k == N_IMPORTDECL || (k == N_EXPORTDECL && jd_J.nodes[st].op >= 3);
}

/* Everything the modules from `first` on import, fetched and read, each
   address once. Breadth first, so no recursion: what order they come in does
   not matter, only the order they run in, which follows the imports. */
static void jd_mod_fetch_all(int first) {
    jctx *J = &jd_J;
    for (int i = first; i < jd_nmods; i++) {
        jmod *m = &jd_mods[i];
        if (m->state == JM_NEW) {
            /* A data: address carries its text and a blob: one names it; only
               the rest are asked for. */
            const char *text = 0;
            char *owned = 0;
            int len = 0;
            if (jd_is_data_url(m->url)) {
                len = (int)jd_data_url(m->url, &owned, 0, 0);
                text = owned;
            } else if (jd_is_blob_url(m->url)) {
                jstr *bb, *bt;
                if (jd_blob_lookup(m->url, &bb, &bt)) { text = bb->s; len = (int)bb->len; }
            } else if (jd_get_script) {
                len = jd_get_script(m->url, &text);
            }
            if (len <= 0 || !text) { free(owned); jd_mod_failed(m, "a module would not come"); continue; }
            if (!jd_room_for((u32)len)) { free(owned); jd_mod_failed(m, "a module too large for this machine's memory"); continue; }
            jd_mod_parse(i, text, (u32)len);
            free(owned);
            m = &jd_mods[i];
        }
        if (m->state != JM_PARSED) continue;
        for (int cell = J->nodes[m->prog].a; cell >= 0; cell = J->nodes[cell].b) {
            int st = J->nodes[cell].a;
            if (st < 0 || !jd_mod_is_request(st)) continue;
            jstr *s = J->nodes[st].str;
            char url[JD_URL_MAX];
            const char *to = s ? jd_mod_resolve(s->s, m->url, url, (int)sizeof(url)) : 0;
            if (!to) continue;
            if (jd_mod_find(to) < 0 && jd_mod_add(to, to) < 0) break;
            m = &jd_mods[i];
        }
    }
}

/* --- linking ------------------------------------------------------------------------- */

/* A binding of a module's, read when asked: the callee carries the scope's
   table and the name. */
static jval nat_mod_binding(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jobj *f = J->callee;
    if (!js_is_obj(f->data) || f->data2.t != JS_STR) return js_undef();
    jprop *p = js_find(f->data.obj, f->data2.str);
    if (!p) return js_undef();
    if (p->v.t == JS_HOLE)
        return js_throw_named(J, JS_ERR_REFERENCE, "", f->data2.str, " cannot be used before its declaration");
    if (p->v.t == JS_ACC) return js_prop_read(J, p, js_undef());
    return p->v;
}

/* One name read from another module's namespace. */
static jval nat_mod_forward(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jobj *f = J->callee;
    if (!js_is_obj(f->data) || f->data2.t != JS_STR) return js_undef();
    return js_get(J, f->data, f->data2.str);
}

static void jd_mod_getter(jobj *on, jstr *name, jnative fn, jval data, jstr *what) {
    if (!on || !name) return;
    jobj *g = js_native_n(&jd_J, "", fn, 0);
    if (!g) return;
    g->data = data;
    g->data2 = js_from_str(what);
    js_define_accessor(&jd_J, on, name, js_from_obj(g), js_undef(), JP_ENUM);
}

static void jd_mod_export_names(jmod *m, int t) {
    jctx *J = &jd_J;
    if (t < 0) return;
    int k = J->nodes[t].kind;
    if (k == N_IDENT && J->nodes[t].str) {
        jd_mod_getter(m->ns, J->nodes[t].str, nat_mod_binding, js_from_obj(m->scope->vars), J->nodes[t].str);
    } else if (k == N_APAT || k == N_OPAT) {
        for (int cell = J->nodes[t].a; cell >= 0; cell = J->nodes[cell].b)
            jd_mod_export_names(m, J->nodes[cell].a);
    }
}

/* 1 when module i and everything it imports is ready to run. */
static int jd_mod_link(int i) {
    jmod *m = &jd_mods[i];
    if (m->state == JM_FAILED) return 0;
    if (m->state != JM_PARSED) return 1;             /* linked, or being linked */
    jctx *J = &jd_J;
    m->state = JM_LINKED;

    jscope *sc = js_scope(J, J->global_lex);
    jobj *ns = js_object_with(J, JO_PLAIN, 0);
    if (!sc || !ns) { jd_mod_failed(m, "no memory for a module"); return 0; }
    sc->escaped = 1;
    sc->module = m;
    m->scope = sc;
    m->ns = ns;
    js_put_prop_flags(J, ns, J->sym_to_string_tag, js_from_str(js_str(J, "Module")), 0);
    js_hoist_body(J, m->prog, sc, sc);
    jval vars = js_from_obj(sc->vars);

    int ok = 1;
    for (int cell = J->nodes[m->prog].a; cell >= 0; cell = J->nodes[cell].b) {
        int st = J->nodes[cell].a;
        if (st < 0 || !jd_mod_is_request(st)) continue;
        int c = jd_mod_child(&jd_mods[i], st);
        if (c < 0) {
            /* A name nothing gives an address, said rather than failed in
               silence. */
            jstr *s = J->nodes[st].str;
            char msg[160];
            int w = 0;
            for (const char *p = "a module name this cannot resolve: "; *p; p++) msg[w++] = *p;
            for (u32 k = 0; s && k < s->len && w < (int)sizeof(msg) - 1; k++) msg[w++] = s->s[k];
            msg[w] = 0;
            jd_note_text(msg);
            ok = 0;
        } else if (!jd_mod_link(c)) {
            ok = 0;
        }
    }
    m = &jd_mods[i];

    /* Its exports. */
    for (int cell = J->nodes[m->prog].a; cell >= 0; cell = J->nodes[cell].b) {
        int st = J->nodes[cell].a;
        if (st < 0 || J->nodes[st].kind != N_EXPORTDECL) continue;
        int op = J->nodes[st].op, d = J->nodes[st].a;
        if (op == 0 && d >= 0) {
            int k = J->nodes[d].kind;
            if ((k == N_FUNCDECL || k == N_CLASSDECL) && J->nodes[d].str)
                jd_mod_getter(ns, J->nodes[d].str, nat_mod_binding, vars, J->nodes[d].str);
            else if (k == N_VAR)
                for (int c = J->nodes[d].a; c >= 0; c = J->nodes[c].b) jd_mod_export_names(m, J->nodes[c].c);
        } else if (op == 1) {
            jd_mod_getter(ns, js_intern(J, "default", 7), nat_mod_binding, vars, js_intern(J, "*default*", 9));
        } else if (op == 2) {
            for (int c = d; c >= 0; c = J->nodes[c].b) {
                int s = J->nodes[c].a;
                if (s < 0 || J->nodes[s].a < 0) continue;
                jd_mod_getter(ns, J->nodes[s].str, nat_mod_binding, vars, J->nodes[J->nodes[s].a].str);
            }
        } else {
            int ch = jd_mod_child(m, st);
            if (ch < 0 || !jd_mods[ch].ns) continue;
            jval cns = js_from_obj(jd_mods[ch].ns);
            if (op == 3) {
                for (int c = d; c >= 0; c = J->nodes[c].b) {
                    int s = J->nodes[c].a;
                    if (s < 0 || J->nodes[s].a < 0) continue;
                    jd_mod_getter(ns, J->nodes[s].str, nat_mod_forward, cns, J->nodes[J->nodes[s].a].str);
                }
            } else if (op == 4) {
                jprop **own;
                u32 n = js_keys_of(J, jd_mods[ch].ns, &own, JK_STR);
                for (u32 k = 0; k < n; k++)
                    if (!js_str_eq(own[k]->key, js_intern(J, "default", 7)) && !js_find(ns, own[k]->key))
                        jd_mod_getter(ns, own[k]->key, nat_mod_forward, cns, own[k]->key);
            } else if (op == 5 && d >= 0) {
                js_put_prop_flags(J, ns, J->nodes[d].str, cns, JP_ENUM);
            }
        }
    }

    /* Its imports: getters in its scope reading the other module. */
    for (int cell = J->nodes[m->prog].a; cell >= 0; cell = J->nodes[cell].b) {
        int st = J->nodes[cell].a;
        if (st < 0 || J->nodes[st].kind != N_IMPORTDECL) continue;
        int ch = jd_mod_child(m, st);
        if (ch < 0 || !jd_mods[ch].ns) continue;
        jval cns = js_from_obj(jd_mods[ch].ns);
        for (int c = J->nodes[st].a; c >= 0; c = J->nodes[c].b) {
            int s = J->nodes[c].a;
            if (s < 0) continue;
            int op = J->nodes[s].op;
            jstr *local = J->nodes[s].str;
            if (op == 2) js_declare_flags(J, sc, local, cns, JP_ENUM);
            else if (op == 1) jd_mod_getter(sc->vars, local, nat_mod_forward, cns, js_intern(J, "default", 7));
            else if (J->nodes[s].a >= 0)
                jd_mod_getter(sc->vars, local, nat_mod_forward, cns, J->nodes[J->nodes[s].a].str);
        }
    }

    if (!ok) {
        m->state = JM_FAILED;
        m->error = js_undef();
    }
    return ok;
}

/* --- running -------------------------------------------------------------------------- */

/* What a module that did not finish at once threw, on the page's error line:
   nothing else sees a rejected promise. */
static void jd_mod_note_value(jval v) {
    jctx *J = &jd_J;
    jsignal outer = J->sig;
    jval outer_ret = J->ret;
    J->sig = JS_OK;
    jstr *s = js_to_str(J, v);
    if (J->sig == JS_OK && s) {
        w_copy(J->error, (int)sizeof(J->error), s->s, (int)s->len + 1);
        J->error_line = 0;
        jd_note_error();
    }
    J->sig = outer;
    J->ret = outer_ret;
}

static jobj *jd_mod_done_promise(jmod *m) {
    if (!m->done) m->done = js_promise_new(&jd_J);
    return m->done;
}

/* Module i finished, one way or the other, after it was left waiting. */
static void jd_mod_finish(int i, int good, jval err, const char *why) {
    jmod *m = &jd_mods[i];
    if (m->state == JM_DONE || m->state == JM_FAILED) return;
    if (good) {
        m->state = JM_DONE;
    } else {
        m->state = JM_FAILED;
        m->error = err;
    }
    if (jd_script_done) jd_script_done(-1, m->url, 0, good ? "" : why);
    if (m->done) js_promise_settle(&jd_J, m->done, good, good ? js_undef() : err);
}

static jval nat_mod_body_ok(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jd_mod_finish((int)J->callee->data.num, 1, js_undef(), "");
    return js_undef();
}

static jval nat_mod_body_err(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval e = js_arg(a, n, 0);
    jd_mod_note_value(e);
    jd_mod_finish((int)J->callee->data.num, 0, e, J->error);
    return js_undef();
}

static void jd_mod_when(jobj *p, int i, jnative ok, jnative err) {
    jctx *J = &jd_J;
    jobj *f = js_native_n(J, "", ok, 1);
    jobj *g = js_native_n(J, "", err, 1);
    if (!p || !f || !g) return;
    f->data = js_num((double)i);
    g->data = js_num((double)i);
    js_promise_then(J, p, js_from_obj(f), js_from_obj(g));
}

/* Module i's own body, what it imports having finished. 1 when it finished,
   0 when it threw, 2 when it awaits at its top level and has not yet. */
static int jd_mod_body(int i) {
    jmod *m = &jd_mods[i];
    jctx *J = &jd_J;
    int nested = J->nest > 0;
    jsignal outer = J->sig;
    jval outer_ret = J->ret;
    int was = jd_current_script;
    jd_current_script = -1;                          /* currentScript is null in a module */
    int good, pending = 0;
    if (J->nodes[m->prog].flags & NF_TLA) {
        jobj *p = js_run_module_async(J, m->prog, m->scope);
        jpromise *pr = p ? (jpromise *)p->internal : 0;
        m = &jd_mods[i];
        if (!pr) {
            good = 0;
            J->ret = js_undef();
            w_copy(J->error, (int)sizeof(J->error), "no memory for a module", 64);
        } else if (pr->state == PR_PENDING) {
            m->state = JM_ASYNC;
            jd_mod_done_promise(m);
            jd_mod_when(p, i, nat_mod_body_ok, nat_mod_body_err);
            pending = 1;
            good = 1;
        } else {
            pr->handled = 1;
            good = pr->state == PR_OK;
            if (!good) {
                J->ret = pr->value;
                jsignal s = J->sig;
                J->sig = JS_OK;
                jstr *t = js_to_str(J, pr->value);
                if (J->sig == JS_OK && t) w_copy(J->error, (int)sizeof(J->error), t->s, (int)t->len + 1);
                J->sig = s;
            }
        }
    } else {
        good = js_run_module(J, m->prog, m->scope);
    }
    jd_current_script = was;
    m = &jd_mods[i];
    int r = pending ? 2 : good;
    if (!pending) {
        if (!good && !nested) jd_note_error();
        jd_mod_finish(i, good, J->ret, J->error);
    }
    J->sig = outer;
    J->ret = outer_ret;
    return r;
}

/* One module that module i was waiting for has finished: when it was the
   last, i runs. */
static jval nat_mod_child_ok(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    int i = (int)J->callee->data.num;
    if (i < 0 || i >= jd_nmods || jd_mods[i].state != JM_WAITING) return js_undef();
    if (--jd_mods[i].waiting > 0) return js_undef();
    jd_mods[i].state = JM_RUNNING;
    jd_mod_body(i);
    return js_undef();
}

static jval nat_mod_child_err(jctx *J, jval t, jval *a, int n) {
    (void)t;
    int i = (int)J->callee->data.num;
    if (i < 0 || i >= jd_nmods || jd_mods[i].state != JM_WAITING) return js_undef();
    jd_mod_finish(i, 0, js_arg(a, n, 0), "what it imports threw");
    return js_undef();
}

/* Module i, after what it imports; each once. 1 when it finished, 0 when it
   or something it imports threw, 2 when it will finish later (done). */
static int jd_mod_run(int i) {
    jmod *m = &jd_mods[i];
    if (m->state == JM_FAILED) return 0;
    if (m->state == JM_WAITING || m->state == JM_ASYNC) return 2;
    if (m->state != JM_LINKED) return 1;             /* running now (a cycle), or done */
    m->state = JM_RUNNING;
    jctx *J = &jd_J;
    int wait = 0;
    for (int cell = J->nodes[m->prog].a; cell >= 0; cell = J->nodes[cell].b) {
        int st = J->nodes[cell].a;
        if (st < 0 || !jd_mod_is_request(st)) continue;
        int c = jd_mod_child(&jd_mods[i], st);
        if (c < 0) continue;
        int r = jd_mod_run(c);
        if (r == 0) {
            jd_mods[i].state = JM_FAILED;
            jd_mods[i].error = jd_mods[c].error;
            return 0;
        }
        if (r == 2) {
            /* Counted once however many times it is named: its done is
               waited on once. */
            int seen = 0;
            for (int k = J->nodes[m->prog].a; k != cell && k >= 0; k = J->nodes[k].b) {
                int s2 = J->nodes[k].a;
                if (s2 >= 0 && jd_mod_is_request(s2) && jd_mod_child(&jd_mods[i], s2) == c) seen = 1;
            }
            if (seen) continue;
            wait++;
            jd_mod_when(jd_mod_done_promise(&jd_mods[c]), i, nat_mod_child_ok, nat_mod_child_err);
        }
        m = &jd_mods[i];
    }
    m = &jd_mods[i];
    if (wait) {
        m->state = JM_WAITING;
        m->waiting = wait;
        jd_mod_done_promise(m);
        return 2;
    }
    return jd_mod_body(i);
}

/* --- the page's module scripts, import() and import.meta ---------------------------- */

static int jd_module_type(const char *ty) {
    if (!ty) return 0;
    while (*ty == ' ') ty++;
    return w_same(ty, "module");
}

static int jd_module_wanted(int i) {
    if (jd_doc->nodes[i].kind != DN_ELEMENT || jd_doc->nodes[i].tag != T_SCRIPT) return 0;
    if (jd_is_started(i)) return 0;
    if (!jd_module_type(dom_attr(jd_doc, i, "type"))) return 0;
    if (!jd_connected(i)) return 0;
    if (jd_has_ancestor_tag(i, T_OTHER, "template")) return 0;
    return 1;
}

/* A module script of the page: from its src, or written in it. */
static int jd_run_module_el(int node) {
    const char *src = dom_attr(jd_doc, node, "src");
    int first = jd_nmods, idx;
    if (src && *src) {
        char url[JD_URL_MAX];
        const char *to = src;
        if (!jd_is_data_url(src) && !jd_is_blob_url(src)) {
            if (!jd_resolve(src, url, (int)sizeof(url))) return 0;
            to = url;
        }
        idx = jd_mod_find(to);
        if (idx < 0) idx = jd_mod_add(to, to);
    } else {
        char key[32];
        int w = 0;
        for (const char *p = "inline:"; *p; p++) key[w++] = *p;
        char digits[12];
        int nd = 0, v = node;
        do { digits[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 11);
        while (nd) key[w++] = digits[--nd];
        key[w] = 0;
        idx = jd_mod_add(key, jd_address[0] ? jd_address : "about:blank");
        if (idx >= 0) {
            jtext t = { 0, 0, 0, 0 };
            jd_text_content(node, &t);
            jd_mod_parse(idx, t.b ? t.b : "", t.n);
            free(t.b);
        }
    }
    if (idx < 0) return 0;
    jd_mod_fetch_all(first < idx ? first : idx);
    return jd_mod_link(idx) && jd_mod_run(idx) != 0;
}

static void jd_inserted_module_due(jval arg) {
    int i = jd_node_of(arg);
    if (i < 0) return;
    int ran = jd_run_module_el(i);
    jd_fire_simple(i, ran ? "load" : "error", 0, 0);
}

/* An import() of a module that finishes later, settled when it does. */
static jval nat_import_ok(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jobj *f = J->callee;
    int i = f->data2.t == JS_NUM ? (int)f->data2.num : -1;
    if (js_is_obj(f->data) && i >= 0 && i < jd_nmods && jd_mods[i].ns)
        js_promise_settle(J, f->data.obj, 1, js_from_obj(jd_mods[i].ns));
    return js_undef();
}

static jval nat_import_err(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *f = J->callee;
    if (js_is_obj(f->data)) js_promise_settle(J, f->data.obj, 0, js_arg(a, n, 0));
    return js_undef();
}

/* import(name): the module, fetched, linked and run if it was not, as a
   promise of its namespace. Relative to the module the call was written in,
   or the page. */
static jval jd_import(jctx *J, jval spec, void *module) {
    jobj *p = js_promise_new(J);
    if (!p) return js_undef();
    jstr *s = js_to_str(J, spec);
    if (J->sig != JS_OK) return js_undef();
    jmod *from = (jmod *)module;
    char url[JD_URL_MAX];
    const char *base = from ? from->url : (jd_address[0] ? jd_address : "about:blank");
    int idx = -1;
    const char *to = s ? jd_mod_resolve(s->s, base, url, (int)sizeof(url)) : 0;
    if (to) {
        idx = jd_mod_find(to);
        if (idx < 0) idx = jd_mod_add(to, to);
    }
    if (idx < 0) {
        jobj *e = js_error_with(J, J->err_proto[JS_ERR_TYPE], js_str(J, "TypeError"),
                                js_str(J, "a module name this cannot resolve"));
        js_promise_settle(J, p, 0, e ? js_from_obj(e) : js_undef());
        return js_from_obj(p);
    }
    jd_mod_fetch_all(idx);
    int good = jd_mod_link(idx) ? jd_mod_run(idx) : 0;
    jmod *m = &jd_mods[idx];
    if (good == 2 && m->done) {
        jobj *f = js_native_n(J, "", nat_import_ok, 1);
        jobj *g = js_native_n(J, "", nat_import_err, 1);
        if (f && g) {
            f->data = g->data = js_from_obj(p);
            f->data2 = js_num((double)idx);
            js_promise_then(J, m->done, js_from_obj(f), js_from_obj(g));
        }
    } else if (good && m->ns) {
        js_promise_settle(J, p, 1, js_from_obj(m->ns));
    } else {
        jval err = m->error;
        if (err.t == JS_UNDEF) {
            jobj *e = js_error_with(J, J->err_proto[JS_ERR_TYPE], js_str(J, "TypeError"),
                                    js_str(J, "a module would not load"));
            err = e ? js_from_obj(e) : js_undef();
        }
        J->sig = JS_OK;
        js_promise_settle(J, p, 0, err);
    }
    return js_from_obj(p);
}

/* import.meta.resolve(name): the address import() would take, as text. */
static jval nat_meta_resolve(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *f = J->callee;
    int idx = f->data.t == JS_NUM ? (int)f->data.num : -1;
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    char url[JD_URL_MAX];
    const char *to = idx >= 0 && idx < jd_nmods && s ? jd_mod_resolve(s->s, jd_mods[idx].url, url, (int)sizeof(url)) : 0;
    if (!to) return js_throw(J, JS_ERR_TYPE, "a module name this cannot resolve", J->error_line);
    return js_from_str(js_str(J, to));
}

static jval jd_import_meta(jctx *J, void *module) {
    jmod *m = (jmod *)module;
    if (!m) return js_undef();
    if (!m->meta) {
        m->meta = js_object_with(J, JO_PLAIN, 0);
        if (!m->meta) return js_undef();
        js_set(J, m->meta, "url", js_from_str(js_str(J, m->url)));
        jobj *r = js_native_n(J, "resolve", nat_meta_resolve, 1);
        if (r) {
            r->data = js_num((double)(m - jd_mods));
            js_set(J, m->meta, "resolve", js_from_obj(r));
        }
    }
    return js_from_obj(m->meta);
}
