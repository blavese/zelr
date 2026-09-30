/* What forms, ranges and serialising scripts expect of a document:
 *
 * - Constraint validation. checkValidity answered true for everything, so a
 *   page asking whether its empty required field could be sent was told yes.
 *   Now a field's ValidityState is worked out from what it holds and its
 *   attributes -- required, the email and url types, pattern, min, max and
 *   step -- with setCustomValidity, validationMessage in English as Chrome
 *   words it, willValidate, and checkValidity and reportValidity firing
 *   invalid, for fields, fieldsets and forms. Mozilla's consent manager
 *   validates addresses through a url field's validity.
 * - DocumentType, whose prototype that manager reads, and document.doctype
 *   when the page said <!DOCTYPE html>.
 * - XMLSerializer, which writes a node as markup.
 * - Range, made by document.createRange or new Range, with its boundaries,
 *   createContextualFragment (the fragment's scripts run when it is put in
 *   the page, as innerHTML's do not), insertNode, the three contents
 *   methods, comparisons and boxes; and a Selection that holds the ranges a
 *   script adds to it, since nobody can select anything here by hand.
 * - Attr nodes (createAttribute, setAttributeNode and the rest), namespace
 *   lookups, DOMImplementation's createDocument and createDocumentType, and
 *   editing commands that say no: what Mozilla's consent manager and NPR's
 *   scripts take off the prototypes before they do anything.
 *
 * Not done: tooLong and tooShort, which the standard reports only after a
 * person has typed, and the partly contained elements a range's contents
 * copy the shells of -- whole nodes and cut text are copied, the shells
 * round them are not. A document createDocument makes is not told it is
 * XML, so its elements are HTML's. */
#pragma once

static jstr *jd_k_custom_msg, *jd_k_validity, *jd_k_vs_el, *jd_k_rstart, *jd_k_roff, *jd_k_rend, *jd_k_reoff;
static jobj *jd_p_validity, *jd_p_range, *jd_p_doctype, *jd_p_serializer, *jd_p_selection;

enum { JV_MISSING = 1, JV_TYPE = 2, JV_PATTERN = 4, JV_LONG = 8, JV_SHORT = 16, JV_UNDER = 32, JV_OVER = 64,
       JV_STEP = 128, JV_BAD = 256, JV_CUSTOM = 512 };

/* --- which fields are checked -------------------------------------------------------- */

static const char *jd_field_type(int x) {
    static char low[24];
    const char *t = jd_attr(x, "type");
    int k = 0;
    for (; t && t[k] && k < 23; k++) low[k] = w_lower(t[k]);
    low[k] = 0;
    static const char *const KNOWN = "text search tel url email password date month week time datetime-local number range "
                                     "color checkbox radio file submit image reset button hidden";
    if (!k || !jd_word_in(KNOWN, low)) return "text";
    return low;
}

static int jd_has_attr(int x, const char *name) { return jd_attr(x, name) != 0; }

/* willValidate: an input, select, textarea or submitting button that is
   not disabled, not read-only, and not inside a datalist. */
static int jd_will_validate(int x) {
    if (!jd_is_element(x)) return 0;
    int tag = jd_doc->nodes[x].tag;
    if (tag != T_INPUT && tag != T_SELECT && tag != T_TEXTAREA && tag != T_BUTTON) return 0;
    if (jd_has_attr(x, "disabled")) return 0;
    for (int p = jd_doc->nodes[x].parent; p >= 0; p = jd_doc->nodes[p].parent) {
        if (!jd_is_element(p)) continue;
        const char *nm = dom_tag_name(jd_doc, p);
        if (w_same(nm, "datalist")) return 0;
        if (w_same(nm, "fieldset") && jd_has_attr(p, "disabled")) return 0;
    }
    if (tag == T_BUTTON) {
        const char *ty = jd_attr(x, "type");
        return !ty || w_same_fold(ty, "submit");
    }
    if (tag == T_INPUT) {
        const char *ty = jd_field_type(x);
        if (w_same(ty, "hidden") || w_same(ty, "reset") || w_same(ty, "button")) return 0;
    }
    if ((tag == T_INPUT || tag == T_TEXTAREA) && jd_has_attr(x, "readonly")) return 0;
    return 1;
}

static jstr *jd_field_value(jctx *J, int x) {
    jval v = nat_value(J, jd_el_value(J, x), 0, 0);
    return v.t == JS_STR ? v.str : js_str(J, "");
}

static int jd_is_checked(int x) {
    const char *v = jd_attr(x, "checked");
    return v && !w_same(v, "0");
}

/* The form a field belongs to: its form attribute's, else the nearest one
   round it. */
static int jd_form_owner(int x) {
    const char *id = jd_attr(x, "form");
    if (id && *id) {
        int f = dom_by_id(jd_doc, id);
        return f >= 0 && jd_doc->nodes[f].tag == T_FORM ? f : -1;
    }
    for (int p = jd_doc->nodes[x].parent; p >= 0; p = jd_doc->nodes[p].parent)
        if (jd_doc->nodes[p].tag == T_FORM) return p;
    return -1;
}

static int jd_alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

static int jd_in_set(char c, const char *set) {
    for (; c && *set; set++) if (*set == c) return 1;
    return 0;
}

/* An address a person could type: local@domain, the domain in labels. */
static int jd_valid_email(const char *s, int n) {
    int at = -1;
    for (int i = 0; i < n; i++) if (s[i] == '@') { if (at >= 0) return 0; at = i; }
    if (at <= 0 || at >= n - 1) return 0;
    for (int i = 0; i < at; i++) {
        char c = s[i];
        if (!(jd_alnum(c) || jd_in_set(c, ".!#$%&'*+/=?^_`{|}~-"))) return 0;
    }
    int label = 0;
    for (int i = at + 1; i <= n; i++) {
        char c = i < n ? s[i] : '.';
        if (c == '.') {
            if (!label || s[i - 1] == '-' || s[i - label] == '-') return 0;
            label = 0;
        } else if (jd_alnum(c) || c == '-') label++;
        else return 0;
    }
    return 1;
}

/* An absolute address: a scheme, a colon, and something after it. */
static int jd_valid_url(const char *s, int n) {
    if (!n || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) return 0;
    int i = 1;
    while (i < n && (jd_alnum(s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')) i++;
    if (i >= n || s[i] != ':' || i + 1 >= n) return 0;
    for (int k = 0; k < n; k++) if (s[k] == ' ' || s[k] == '\t' || s[k] == '\n') return 0;
    return 1;
}

/* A valid floating-point number as the standard writes one; 0 when not. */
static int jd_float_of(const char *s, double *out) {
    int i = 0, digits = 0;
    if (s[i] == '-') i++;
    while (s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    if (s[i] == '.') { i++; int f = 0; while (s[i] >= '0' && s[i] <= '9') { i++; f++; } if (!f) return 0; digits += f; }
    if (!digits) return 0;
    if (s[i] == 'e' || s[i] == 'E') {
        i++;
        if (s[i] == '+' || s[i] == '-') i++;
        int e = 0;
        while (s[i] >= '0' && s[i] <= '9') { i++; e++; }
        if (!e) return 0;
    }
    if (s[i]) return 0;
    *out = js_str_to_num(s, (u32)i);
    return 1;
}

/* Whether the whole of the value matches the pattern, as ^(?:pattern)$. An
   unreadable pattern matches everything, as browsers treat it. */
static int jd_pattern_matches(jctx *J, const char *pattern, jstr *value) {
    jtext t = { 0, 0, 0, 0 };
    jd_put(&t, "^(?:");
    jd_put(&t, pattern);
    jd_put(&t, ")$");
    jstr *src = jt_done(J, &t);
    jsignal outer = J->sig;
    jobj *re = src ? js_regex_new(J, src->s, src->len, RXF_U) : 0;
    int ok = 1;
    if (re && J->sig == JS_OK) {
        jval test = js_get(J, js_from_obj(re), js_str(J, "test"));
        jval arg = js_from_str(value);
        jval r = js_callable(test) ? js_call(J, test, js_from_obj(re), &arg, 1) : js_bool(1);
        if (J->sig == JS_OK) ok = js_to_bool(r);
    }
    J->sig = outer;
    return ok;
}

static int jd_radio_group_checked(int x) {
    const char *name = jd_attr(x, "name");
    if (!name || !*name) return jd_is_checked(x);
    int form = jd_form_owner(x);
    for (int i = 0; i < jd_doc->count; i++) {
        if (jd_doc->nodes[i].kind != DN_ELEMENT || jd_doc->nodes[i].tag != T_INPUT) continue;
        if (!w_same(jd_field_type(i), "radio")) continue;
        const char *n2 = jd_attr(i, "name");
        if (!n2 || !w_same(n2, name) || jd_form_owner(i) != form || !jd_connected(i)) continue;
        if (jd_is_checked(i)) return 1;
    }
    return 0;
}

static int jd_radio_group_required(int x) {
    const char *name = jd_attr(x, "name");
    if (!name || !*name) return jd_has_attr(x, "required");
    int form = jd_form_owner(x);
    for (int i = 0; i < jd_doc->count; i++) {
        if (jd_doc->nodes[i].kind != DN_ELEMENT || jd_doc->nodes[i].tag != T_INPUT) continue;
        if (!w_same(jd_field_type(i), "radio")) continue;
        const char *n2 = jd_attr(i, "name");
        if (n2 && w_same(n2, name) && jd_form_owner(i) == form && jd_has_attr(i, "required")) return 1;
    }
    return 0;
}

static jstr *jd_custom_msg(int x) {
    jobj *o = jd_element(&jd_J, x);
    jval m = o ? jd_kept(o, jd_k_custom_msg) : js_undef();
    return m.t == JS_STR ? m.str : 0;
}

/* Everything wrong with a field's value, as JV_ bits. */
static int jd_validity_flags(jctx *J, int x) {
    int flags = 0;
    jstr *custom = jd_custom_msg(x);
    if (custom && custom->len) flags |= JV_CUSTOM;
    int tag = jd_doc->nodes[x].tag;
    int required = jd_has_attr(x, "required");
    if (tag == T_SELECT) {
        if (required && !jd_field_value(J, x)->len) flags |= JV_MISSING;
        return flags;
    }
    if (tag == T_TEXTAREA) {
        if (required && !jd_field_value(J, x)->len) flags |= JV_MISSING;
        return flags;
    }
    if (tag != T_INPUT) return flags;
    const char *ty = jd_field_type(x);
    if (w_same(ty, "checkbox")) {
        if (required && !jd_is_checked(x)) flags |= JV_MISSING;
        return flags;
    }
    if (w_same(ty, "radio")) {
        if (jd_radio_group_required(x) && !jd_radio_group_checked(x)) flags |= JV_MISSING;
        return flags;
    }
    if (w_same(ty, "submit") || w_same(ty, "image") || w_same(ty, "reset") || w_same(ty, "button")
        || w_same(ty, "hidden") || w_same(ty, "color"))
        return flags;
    jstr *v = jd_field_value(J, x);
    if (required && !v->len && !w_same(ty, "range")) flags |= JV_MISSING;
    if (!v->len) return flags;
    if (w_same(ty, "email")) {
        int multiple = jd_has_attr(x, "multiple"), bad = 0, start = 0;
        for (int i = 0; i <= (int)v->len; i++) {
            if (i < (int)v->len && !(multiple && v->s[i] == ',')) continue;
            int a = start, b = i;
            while (a < b && (v->s[a] == ' ' || v->s[a] == '\t')) a++;
            while (b > a && (v->s[b - 1] == ' ' || v->s[b - 1] == '\t')) b--;
            if (!jd_valid_email(v->s + a, b - a)) bad = 1;
            start = i + 1;
        }
        if (bad) flags |= JV_TYPE;
    }
    if (w_same(ty, "url") && !jd_valid_url(v->s, (int)v->len)) flags |= JV_TYPE;
    const char *pattern = jd_attr(x, "pattern");
    if (pattern && (w_same(ty, "text") || w_same(ty, "search") || w_same(ty, "tel") || w_same(ty, "url")
                    || w_same(ty, "email") || w_same(ty, "password")) && !jd_pattern_matches(J, pattern, v))
        flags |= JV_PATTERN;
    if (w_same(ty, "number") || w_same(ty, "range")) {
        double d, lo, hi, step = 1, base = 0;
        if (!jd_float_of(v->s, &d)) return flags;
        const char *mn = jd_attr(x, "min"), *mx = jd_attr(x, "max"), *st = jd_attr(x, "step");
        int has_lo = mn && jd_float_of(mn, &lo), has_hi = mx && jd_float_of(mx, &hi);
        if (has_lo && d < lo) flags |= JV_UNDER;
        if (has_hi && d > hi) flags |= JV_OVER;
        int any = st && w_same_fold(st, "any");
        if (st && !any && (!jd_float_of(st, &step) || step <= 0)) step = 1;
        if (has_lo) base = lo;
        else { const char *dv = jd_attr(x, "value"); double b; if (dv && jd_float_of(dv, &b)) base = b; }
        if (!any) {
            double q = (d - base) / step, r = q - js_floor(q + 0.5);
            if (r < 0) r = -r;
            if (r > 1e-9 * (q < 0 ? -q : q) + 1e-9) flags |= JV_STEP;
        }
        return flags;
    }
    if (w_same(ty, "date") || w_same(ty, "month") || w_same(ty, "week") || w_same(ty, "time") || w_same(ty, "datetime-local")) {
        /* The same shape compares as text in the same order as in time. */
        const char *mn = jd_attr(x, "min"), *mx = jd_attr(x, "max");
        if (mn && *mn && strcmp(v->s, mn) < 0) flags |= JV_UNDER;
        if (mx && *mx && strcmp(v->s, mx) > 0) flags |= JV_OVER;
    }
    return flags;
}

static int jd_field_valid(jctx *J, int x) { return !jd_will_validate(x) || !jd_validity_flags(J, x); }

/* validationMessage: Chrome's English for the first thing wrong. */
static jval jd_validation_message(jctx *J, int x) {
    if (!jd_will_validate(x)) return jd_str("");
    int f = jd_validity_flags(J, x);
    if (!f) return jd_str("");
    if (f & JV_CUSTOM) return js_from_str(jd_custom_msg(x));
    int tag = jd_doc->nodes[x].tag;
    const char *ty = tag == T_INPUT ? jd_field_type(x) : "";
    if (f & JV_MISSING) {
        if (tag == T_SELECT) return jd_str("Please select an item in the list.");
        if (w_same(ty, "checkbox")) return jd_str("Please check this box if you want to proceed.");
        if (w_same(ty, "radio")) return jd_str("Please select one of these options.");
        if (w_same(ty, "file")) return jd_str("Please select a file.");
        return jd_str("Please fill out this field.");
    }
    if (f & JV_TYPE) return jd_str(w_same(ty, "email") ? "Please enter an email address." : "Please enter a URL.");
    if (f & JV_PATTERN) return jd_str("Please match the requested format.");
    jtext t = { 0, 0, 0, 0 };
    if (f & JV_UNDER) { jd_put(&t, "Value must be greater than or equal to "); jd_put(&t, jd_attr(x, "min")); jd_putc(&t, '.'); }
    else if (f & JV_OVER) { jd_put(&t, "Value must be less than or equal to "); jd_put(&t, jd_attr(x, "max")); jd_putc(&t, '.'); }
    else jd_put(&t, "Please enter a valid value.");
    return js_from_str(jt_done(J, &t));
}

/* --- the methods -------------------------------------------------------------------------- */

static jval nat_will_validate(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)a; (void)n;
    int x = jd_el_of(t);
    return js_bool(x >= 0 && jd_will_validate(x));
}

static jval nat_validation_message(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    return x < 0 ? jd_str("") : jd_validation_message(J, x);
}

static jval nat_set_custom_validity(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    jobj *o = x >= 0 ? jd_element(J, x) : 0;
    if (!o) return js_undef();
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (s) jd_keep(o, jd_k_custom_msg, js_from_str(s));
    return js_undef();
}

/* checkValidity: true, or false after an invalid event at the field. */
static int jd_check_field(jctx *J, int x) {
    if (jd_field_valid(J, x)) return 1;
    jd_fire_simple(x, "invalid", 0, 1);
    return 0;
}

static int jd_check_under(jctx *J, int top, int form) {
    int ok = 1;
    for (int i = jd_walk_first(top); i >= 0; i = jd_walk_next(i, top)) {
        if (!jd_is_element(i)) continue;
        int tag = jd_doc->nodes[i].tag;
        if (tag != T_INPUT && tag != T_SELECT && tag != T_TEXTAREA && tag != T_BUTTON) continue;
        if (form >= 0 && jd_form_owner(i) != form) continue;
        if (!jd_check_field(J, i)) ok = 0;
    }
    /* Fields outside the form that name it with form="". */
    if (form >= 0) {
        const char *id = jd_attr(form, "id");
        for (int i = 0; id && *id && i < jd_doc->count; i++) {
            if (!jd_is_element(i) || !jd_attr(i, "form") || jd_form_owner(i) != form) continue;
            int inside = 0;
            for (int p = jd_doc->nodes[i].parent; p >= 0; p = jd_doc->nodes[p].parent) if (p == form) inside = 1;
            if (!inside && !jd_check_field(J, i)) ok = 0;
        }
    }
    return ok;
}

static jval nat_check_validity(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    if (x < 0) return js_bool(1);
    int tag = jd_doc->nodes[x].tag;
    if (tag == T_FORM) return js_bool(jd_check_under(J, x, x));
    const char *nm = dom_tag_name(jd_doc, x);
    if (w_same(nm, "fieldset")) return js_bool(jd_check_under(J, x, -1));
    return js_bool(jd_check_field(J, x));
}

/* ValidityState: its field kept on it, its answers worked out when asked. */
static jval nat_validity(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    int x = jd_el_of(t);
    jobj *o = x >= 0 ? jd_element(J, x) : 0;
    if (!o) return js_undef();
    jval kept = jd_kept(o, jd_k_validity);
    if (kept.t == JS_OBJ) return kept;
    jobj *v = js_object_with(J, JO_PLAIN, jd_p_validity);
    if (!v) return js_undef();
    jd_keep(v, jd_k_vs_el, js_num(x));
    jd_keep(o, jd_k_validity, js_from_obj(v));
    return js_from_obj(v);
}

static jval nat_validity_flag(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jval el = js_is_obj(t) ? jd_kept(t.obj, jd_k_vs_el) : js_undef();
    if (el.t != JS_NUM) return jd_illegal(J);
    int x = (int)el.num, bit = (int)J->callee->data.num;
    int f = jd_validity_flags(J, x);
    return js_bool(bit ? (f & bit) != 0 : f == 0);
}

/* --- DocumentType and XMLSerializer ------------------------------------------------------ */

static jobj *jd_doctype_obj;

static jval nat_doctype(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!jd_is_doc(t) || !jd_doc->standards) return js_null();
    if (!jd_doctype_obj) {
        jd_doctype_obj = js_object_with(J, JO_PLAIN, jd_p_doctype);
        if (!jd_doctype_obj) return js_null();
        js_set(J, jd_doctype_obj, "name", jd_str("html"));
        js_set(J, jd_doctype_obj, "publicId", jd_str(""));
        js_set(J, jd_doctype_obj, "systemId", jd_str(""));
        js_set(J, jd_doctype_obj, "nodeType", js_num(10));
        js_set(J, jd_doctype_obj, "nodeName", jd_str("html"));
    }
    return js_from_obj(jd_doctype_obj);
}

static jval nat_nothing_node(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return js_undef(); }

static jval nat_serializer_ctor(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "XMLSerializer is made with new", J->error_line);
    return t;
}

static jval nat_serialize(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval v = js_arg(a, n, 0);
    jtext out = { 0, 0, 0, 0 };
    if (js_is_obj(v) && v.obj == jd_doctype_obj) jd_put(&out, "<!DOCTYPE html>");
    else if (jd_is_doc(v)) {
        if (jd_doc->standards) jd_put(&out, "<!DOCTYPE html>");
        if (jd_doc->root >= 0) jd_serialise(jd_doc->root, &out, 1);
    } else {
        int x = jd_node_of(v);
        if (x < 0) return js_throw(J, JS_ERR_TYPE, "serializeToString needs a node", J->error_line);
        if (jd_kind(x) == JN_FRAGMENT) jd_serialise_children(x, &out);
        else jd_serialise(x, &out, 1);
    }
    jstr *s = jt_done(J, &out);
    return s ? js_from_str(s) : jd_str("");
}

/* --- Range --------------------------------------------------------------------------------- */

/* A boundary is a node and an offset: children for an element, a document
   or a fragment, characters (bytes here) for text. The document, which has
   no node of its own, is -2. */
#define JD_DOCNODE -2

static int jd_node_len(int x) {
    if (x == JD_DOCNODE) return jd_doc->root >= 0 ? 1 : 0;
    int k = jd_kind(x);
    if (k == JN_TEXT || k == JN_COMMENT) return (int)w_len(jd_text_of(x));
    int c = 0;
    for (int ch = jd_doc->nodes[x].first; ch >= 0; ch = jd_doc->nodes[ch].next) c++;
    return c;
}

static int jd_parent_of(int x) {
    if (x == JD_DOCNODE) return -3;
    int p = jd_doc->nodes[x].parent;
    if (p < 0 && x == jd_doc->root) return JD_DOCNODE;
    return p;
}

static int jd_child_index(int x) {
    int p = jd_doc->nodes[x].parent, i = 0;
    if (p < 0) return 0;
    for (int c = jd_doc->nodes[p].first; c >= 0 && c != x; c = jd_doc->nodes[c].next) i++;
    return i;
}

static int jd_child_at(int x, int k) {
    if (x == JD_DOCNODE) return k == 0 ? jd_doc->root : -1;
    int c = jd_doc->nodes[x].first;
    while (c >= 0 && k-- > 0) c = jd_doc->nodes[c].next;
    return c;
}

static int jd_is_ancestor(int a, int b) {
    if (a == JD_DOCNODE) return b != JD_DOCNODE && jd_connected(b);
    for (int p = b == JD_DOCNODE ? -3 : jd_parent_of(b); p != -3 && p >= JD_DOCNODE; p = jd_parent_of(p)) {
        if (p == a) return 1;
        if (p == JD_DOCNODE) break;
    }
    return 0;
}

static jval jd_node_value(jctx *J, int x) {
    if (x == JD_DOCNODE) return jd_document_obj ? js_from_obj(jd_document_obj) : js_null();
    return jd_el_value(J, x);
}

static int jd_range_node(jval v) {
    if (jd_is_doc(v)) return JD_DOCNODE;
    return jd_node_of(v);
}

/* The standard's comparison of two boundary points: -1 before, 0 equal, 1
   after. */
static int jd_cmp_points(int na, int oa, int nb, int ob) {
    if (na == nb) return oa < ob ? -1 : oa > ob ? 1 : 0;
    if (jd_is_ancestor(nb, na)) return -jd_cmp_points(nb, ob, na, oa);
    if (jd_is_ancestor(na, nb)) {
        int c = nb;
        while (jd_parent_of(c) != na) c = jd_parent_of(c);
        return jd_child_index(c) < oa ? 1 : -1;
    }
    /* Neither holds the other: the order of the nodes in the document. */
    for (int i = jd_walk_first(jd_doc->root); i >= 0; i = jd_walk_next(i, jd_doc->root)) {
        if (i == na) return -1;
        if (i == nb) return 1;
    }
    return na == jd_doc->root ? -1 : 1;
}

typedef struct { int sn, so, en, eo; } jrange;

static int jd_range_get(jctx *J, jval t, jrange *r) {
    if (!js_is_obj(t)) { jd_illegal(J); return 0; }
    jval a = jd_kept(t.obj, jd_k_rstart), b = jd_kept(t.obj, jd_k_roff), c = jd_kept(t.obj, jd_k_rend), d = jd_kept(t.obj, jd_k_reoff);
    if (a.t != JS_NUM || b.t != JS_NUM || c.t != JS_NUM || d.t != JS_NUM) { jd_illegal(J); return 0; }
    r->sn = (int)a.num; r->so = (int)b.num; r->en = (int)c.num; r->eo = (int)d.num;
    return 1;
}

static void jd_range_put(jobj *o, const jrange *r) {
    jd_keep(o, jd_k_rstart, js_num(r->sn));
    jd_keep(o, jd_k_roff, js_num(r->so));
    jd_keep(o, jd_k_rend, js_num(r->en));
    jd_keep(o, jd_k_reoff, js_num(r->eo));
}

static jobj *jd_range_new(jctx *J, jobj *proto) {
    jobj *o = js_object_with(J, JO_PLAIN, proto ? proto : jd_p_range);
    if (!o) return 0;
    jrange r = { JD_DOCNODE, 0, JD_DOCNODE, 0 };
    jd_range_put(o, &r);
    return o;
}

static jval nat_range_ctor(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "Range is made with new", J->error_line);
    if (!js_is_obj(t)) return js_undef();
    jrange r = { JD_DOCNODE, 0, JD_DOCNODE, 0 };
    jd_range_put(t.obj, &r);
    return t;
}

static jval nat_create_range(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jobj *o = jd_range_new(J, 0);
    return o ? js_from_obj(o) : js_undef();
}

/* Which boundary a getter or setter is about: 0 start, 1 end. */
static jval nat_range_container(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    return jd_node_value(J, J->callee->data.num ? r.en : r.sn);
}

static jval nat_range_offset(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    return js_num(J->callee->data.num ? r.eo : r.so);
}

static jval nat_range_collapsed(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    return js_bool(r.sn == r.en && r.so == r.eo);
}

static int jd_common_ancestor(int a, int b) {
    for (int p = a; p != -3; p = jd_parent_of(p)) {
        if (p == b || jd_is_ancestor(p, b)) return p;
        if (p == JD_DOCNODE) break;
    }
    return JD_DOCNODE;
}

static jval nat_range_common(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    return jd_node_value(J, jd_common_ancestor(r.sn, r.en));
}

/* setStart and setEnd (data 0, 1), and the Before/After forms (data 2..5). */
static jval nat_range_set(jctx *J, jval t, jval *a, int n) {
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    int which = (int)J->callee->data.num;
    int node = jd_range_node(js_arg(a, n, 0));
    if (node < JD_DOCNODE) return js_throw(J, JS_ERR_TYPE, "a range's boundary is in a node", J->error_line);
    int off;
    if (which <= 1) {
        double d = js_trunc(js_to_num(J, js_arg(a, n, 1)));
        if (J->sig != JS_OK) return js_undef();
        if (d < 0 || d > jd_node_len(node)) return js_throw_dom(J, "IndexSizeError", "that offset is past the node's end");
        off = (int)d;
    } else {
        int p = jd_parent_of(node);
        if (p < JD_DOCNODE) return js_throw_dom(J, "InvalidNodeTypeError", "that node has no parent");
        off = node == jd_doc->root && p == JD_DOCNODE ? 0 : jd_child_index(node);
        if (which == 3 || which == 5) off++;
        node = p;
        which = which <= 3 ? 0 : 1;
    }
    if (which == 0) {
        r.sn = node; r.so = off;
        if (jd_cmp_points(r.sn, r.so, r.en, r.eo) > 0) { r.en = node; r.eo = off; }
    } else {
        r.en = node; r.eo = off;
        if (jd_cmp_points(r.sn, r.so, r.en, r.eo) > 0) { r.sn = node; r.so = off; }
    }
    jd_range_put(t.obj, &r);
    return js_undef();
}

static jval nat_range_select(jctx *J, jval t, jval *a, int n) {
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    int node = jd_range_node(js_arg(a, n, 0));
    if (node < JD_DOCNODE) return js_throw(J, JS_ERR_TYPE, "a range selects a node", J->error_line);
    if (J->callee->data.num) {                      /* selectNodeContents */
        r.sn = r.en = node;
        r.so = 0;
        r.eo = jd_node_len(node);
    } else {
        int p = jd_parent_of(node);
        if (p < JD_DOCNODE) return js_throw_dom(J, "InvalidNodeTypeError", "that node has no parent");
        int i = node == jd_doc->root && p == JD_DOCNODE ? 0 : jd_child_index(node);
        r.sn = r.en = p;
        r.so = i;
        r.eo = i + 1;
    }
    jd_range_put(t.obj, &r);
    return js_undef();
}

static jval nat_range_collapse(jctx *J, jval t, jval *a, int n) {
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    if (js_to_bool(js_arg(a, n, 0))) { r.en = r.sn; r.eo = r.so; }
    else { r.sn = r.en; r.so = r.eo; }
    jd_range_put(t.obj, &r);
    return js_undef();
}

static jval nat_range_clone(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    jobj *o = jd_range_new(J, t.obj->proto);
    if (!o) return js_undef();
    jd_range_put(o, &r);
    return js_from_obj(o);
}

/* Whether node x lies wholly inside the range. */
static int jd_range_contains(const jrange *r, int x) {
    int p = jd_parent_of(x);
    if (p < JD_DOCNODE) return 0;
    int i = x == jd_doc->root && p == JD_DOCNODE ? 0 : jd_child_index(x);
    return jd_cmp_points(p, i, r->sn, r->so) >= 0 && jd_cmp_points(p, i + 1, r->en, r->eo) <= 0;
}

static jval nat_range_tostring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    jtext out = { 0, 0, 0, 0 };
    if (r.sn == r.en && r.sn >= 0 && jd_kind(r.sn) == JN_TEXT) {
        const char *s = jd_text_of(r.sn);
        jt_put(J, &out, s + r.so, (u32)(r.eo - r.so));
    } else {
        int top = jd_common_ancestor(r.sn, r.en);
        int root = top == JD_DOCNODE ? jd_doc->root : top;
        for (int i = root; i >= 0; i = jd_walk_next(i, root)) {
            if (jd_kind(i) != JN_TEXT) continue;
            const char *s = jd_text_of(i);
            int len = (int)w_len(s), from = 0, to = len;
            if (i == r.sn) from = r.so;
            else if (i == r.en) to = r.eo;
            else if (!jd_range_contains(&r, i)) continue;
            if (i == r.en && i != r.sn) to = r.eo;
            if (to > from) jt_put(J, &out, s + from, (u32)(to - from));
        }
    }
    jstr *s = jt_done(J, &out);
    return s ? js_from_str(s) : jd_str("");
}

static void jd_unmark_scripts(int top) {
    for (int i = jd_walk_first(top); i >= 0; i = jd_walk_next(i, top))
        if (jd_is_element(i) && jd_doc->nodes[i].tag == T_SCRIPT && i < DOM_NODES)
            jd_started[i >> 3] &= (u8)~(1 << (i & 7));
}

static jval nat_range_fragment(jctx *J, jval t, jval *a, int n) {
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s || J->sig != JS_OK) return js_undef();
    int f = jd_parse_fragment(s->s, (int)s->len);
    if (f < 0) return js_null();
    /* Unlike innerHTML's, these scripts run when the fragment is put in the
       page. */
    jd_unmark_scripts(f);
    return jd_el_value(J, f);
}

/* Text split at k: x keeps the first part, a new node after it the rest. */
static int jd_split_text(int x, int k) {
    const char *s = jd_text_of(x);
    int len = (int)w_len(s);
    if (k <= 0 || k >= len) return -1;
    int tail = jd_new_text(s + k, len - k);
    char *head = (char *)malloc((u64)k + 1);
    if (tail < 0 || !head) { free(head); return -1; }
    for (int i = 0; i < k; i++) head[i] = s[i];
    head[k] = 0;
    jd_set_data(x, head, k);
    free(head);
    int p = jd_doc->nodes[x].parent;
    if (p >= 0) jd_insert(p, tail, jd_doc->nodes[x].next);
    return tail;
}

static jval nat_range_insert(jctx *J, jval t, jval *a, int n) {
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    int node = jd_node_of(js_arg(a, n, 0));
    if (node < 0) return js_throw(J, JS_ERR_TYPE, "insertNode needs a node", J->error_line);
    int parent, before;
    if (r.sn >= 0 && jd_kind(r.sn) == JN_TEXT) {
        parent = jd_doc->nodes[r.sn].parent;
        int tail = jd_split_text(r.sn, r.so);
        before = r.so == 0 ? r.sn : tail >= 0 ? tail : jd_doc->nodes[r.sn].next;
    } else {
        parent = r.sn;
        before = jd_child_at(r.sn, r.so);
    }
    if (parent < 0) return js_throw_dom(J, "HierarchyRequestError", "the node cannot be put there");
    if (!jd_can_insert(J, parent, node)) return js_undef();
    jd_insert(parent, node, before);
    return js_undef();
}

/* Text node x's bytes from..to as a new text node. */
static int jd_text_piece(int x, int from, int to) {
    const char *s = jd_text_of(x);
    return to > from ? jd_new_text(s + from, to - from) : -1;
}

/* Text node x keeping only its bytes outside from..to. */
static void jd_text_cut(int x, int from, int to) {
    const char *s = jd_text_of(x);
    int len = (int)w_len(s);
    char *keep = (char *)malloc((u64)len + 1);
    if (!keep) return;
    int w = 0;
    for (int i = 0; i < from; i++) keep[w++] = s[i];
    for (int i = to; i < len; i++) keep[w++] = s[i];
    keep[w] = 0;
    jd_set_data(x, keep, w);
    free(keep);
}

/* The next node after x's subtree, within top. */
static int jd_after_subtree(int x, int top) {
    while (x >= 0 && x != top) {
        if (jd_doc->nodes[x].next >= 0) return jd_doc->nodes[x].next;
        x = jd_doc->nodes[x].parent;
    }
    return -1;
}

/* delete, extract and clone (data 0, 1, 2). Every position is worked out
   before anything changes: the text a boundary falls inside is cut in
   place, and the nodes wholly between the two are moved, copied or
   removed, outermost first. */
static jval nat_range_contents(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    int op = (int)J->callee->data.num;
    int frag = op ? jd_new_fragment() : -1;
    if (r.sn == r.en && r.so == r.eo) return frag >= 0 ? jd_el_value(J, frag) : js_undef();
    if (r.sn == r.en && r.sn >= 0 && jd_kind(r.sn) == JN_TEXT) {
        int c = op ? jd_text_piece(r.sn, r.so, r.eo) : -1;
        if (c >= 0 && frag >= 0) jd_insert(frag, c, -1);
        if (op != 2) {
            jd_text_cut(r.sn, r.so, r.eo);
            r.eo = r.so;
            jd_range_put(t.obj, &r);
        }
        return frag >= 0 ? jd_el_value(J, frag) : js_undef();
    }
    int st = r.sn >= 0 && jd_kind(r.sn) == JN_TEXT ? r.sn : -1;
    int et = r.en >= 0 && jd_kind(r.en) == JN_TEXT ? r.en : -1;
    /* The boundaries between nodes: after the start's text, before the
       end's. */
    jrange inner = r;
    if (st >= 0) { inner.sn = jd_doc->nodes[st].parent; inner.so = jd_child_index(st) + 1; }
    if (et >= 0) { inner.en = jd_doc->nodes[et].parent; inner.eo = jd_child_index(et); }
    int top = jd_common_ancestor(r.sn, r.en);
    int root = top == JD_DOCNODE ? jd_doc->root : top;
    int *list = (int *)malloc(sizeof(int) * 4096), nl = 0;
    if (!list) return js_undef();
    for (int i = jd_walk_first(root); i >= 0 && nl < 4096; ) {
        if (i != st && i != et && jd_range_contains(&inner, i)) {
            list[nl++] = i;
            i = jd_after_subtree(i, root);
        } else i = jd_walk_next(i, root);
    }
    if (st >= 0 && op) {
        int c = jd_text_piece(st, r.so, (int)w_len(jd_text_of(st)));
        if (c >= 0 && frag >= 0) jd_insert(frag, c, -1);
    }
    for (int k = 0; k < nl; k++) {
        int x = list[k];
        if (op == 2) {
            int c = jd_clone(x, 1);
            if (c >= 0 && frag >= 0) jd_insert(frag, c, -1);
        } else if (op == 1) {
            if (frag >= 0) jd_insert(frag, x, -1);
        } else jd_remove(x);
    }
    free(list);
    if (et >= 0 && op) {
        int c = jd_text_piece(et, 0, r.eo);
        if (c >= 0 && frag >= 0) jd_insert(frag, c, -1);
    }
    if (op != 2) {
        if (st >= 0) jd_text_cut(st, r.so, (int)w_len(jd_text_of(st)));
        if (et >= 0) jd_text_cut(et, 0, r.eo);
        r.en = r.sn;
        r.eo = r.so;
        jd_range_put(t.obj, &r);
    }
    return frag >= 0 ? jd_el_value(J, frag) : js_undef();
}

static jval nat_range_surround(jctx *J, jval t, jval *a, int n) {
    int wrap = jd_node_of(js_arg(a, n, 0));
    if (wrap < 0) return js_throw(J, JS_ERR_TYPE, "surroundContents needs a node", J->error_line);
    jobj *cf = js_native_n(J, "", nat_range_contents, 0);
    if (!cf) return js_undef();
    cf->data = js_num(1);
    jobj *saved = J->callee;
    J->callee = cf;
    jval frag = nat_range_contents(J, t, 0, 0);
    J->callee = saved;
    int f = jd_node_of(frag);
    if (J->sig != JS_OK || f < 0) return js_undef();
    while (jd_doc->nodes[wrap].first >= 0) jd_remove(jd_doc->nodes[wrap].first);
    jd_insert(wrap, f, -1);
    jval w = js_arg(a, n, 0);
    nat_range_insert(J, t, &w, 1);
    jrange r;
    if (jd_range_get(J, t, &r)) {
        int p = jd_doc->nodes[wrap].parent, i = jd_child_index(wrap);
        r.sn = r.en = p;
        r.so = i;
        r.eo = i + 1;
        if (p >= 0) jd_range_put(t.obj, &r);
    }
    return js_undef();
}

static jval nat_range_compare(jctx *J, jval t, jval *a, int n) {
    jrange r, o;
    if (!jd_range_get(J, t, &r)) return js_undef();
    int how = (int)js_to_num(J, js_arg(a, n, 0));
    jval other = js_arg(a, n, 1);
    if (!jd_range_get(J, other, &o)) return js_undef();
    if (how < 0 || how > 3) return js_throw_dom(J, "NotSupportedError", "that is not a way to compare ranges");
    /* START_TO_START, START_TO_END, END_TO_END, END_TO_START. */
    int tn = how == 0 || how == 3 ? r.sn : r.en, to = how == 0 || how == 3 ? r.so : r.eo;
    int on = how == 0 || how == 1 ? o.sn : o.en, oo = how == 0 || how == 1 ? o.so : o.eo;
    return js_num(jd_cmp_points(tn, to, on, oo));
}

/* comparePoint (data 0) and isPointInRange (data 1). */
static jval nat_range_point(jctx *J, jval t, jval *a, int n) {
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    int node = jd_range_node(js_arg(a, n, 0));
    if (node < JD_DOCNODE) return js_throw(J, JS_ERR_TYPE, "that is not a node", J->error_line);
    int off = (int)js_to_num(J, js_arg(a, n, 1));
    if (off < 0 || off > jd_node_len(node)) return js_throw_dom(J, "IndexSizeError", "that offset is past the node's end");
    int c = jd_cmp_points(node, off, r.sn, r.so) < 0 ? -1 : jd_cmp_points(node, off, r.en, r.eo) > 0 ? 1 : 0;
    return J->callee->data.num ? js_bool(c == 0) : js_num(c);
}

static jval nat_range_intersects(jctx *J, jval t, jval *a, int n) {
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    int node = jd_node_of(js_arg(a, n, 0));
    if (node < 0) return js_bool(0);
    int p = jd_parent_of(node);
    if (p < JD_DOCNODE) return js_bool(1);
    int i = node == jd_doc->root && p == JD_DOCNODE ? 0 : jd_child_index(node);
    return js_bool(jd_cmp_points(p, i, r.en, r.eo) < 0 && jd_cmp_points(p, i + 1, r.sn, r.so) > 0);
}

/* The box of what the range holds: the element round it, as the layout
   placed it. */
static int jd_range_box(const jrange *r, int *x, int *y, int *w, int *h) {
    int top = jd_common_ancestor(r->sn, r->en);
    while (top >= 0 && !jd_is_element(top)) top = jd_doc->nodes[top].parent;
    if (top < 0) top = jd_doc->body;
    *x = *y = *w = *h = 0;
    return top >= 0 && jd_box(top, x, y, w, h);
}

static jval nat_range_rect(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    int x, y, w, h;
    jd_range_box(&r, &x, &y, &w, &h);
    return jd_rect(J, x, y, w, h);
}

static jval nat_range_rects(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jrange r;
    if (!jd_range_get(J, t, &r)) return js_undef();
    jobj *list = js_array(J);
    int x, y, w, h;
    if (list && !(r.sn == r.en && r.so == r.eo) && jd_range_box(&r, &x, &y, &w, &h)) js_arr_push(J, list, jd_rect(J, x, y, w, h));
    return list ? js_from_obj(list) : js_undef();
}

/* --- Selection --------------------------------------------------------------------------------- */

static jobj *jd_selection, *jd_sel_range;

static jval nat_get_selection(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    if (!jd_selection) jd_selection = js_object_with(J, JO_PLAIN, jd_p_selection);
    return jd_selection ? js_from_obj(jd_selection) : js_null();
}

static jval nat_sel_count(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; return js_num(jd_sel_range ? 1 : 0); }

static jval nat_sel_add(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jrange r;
    if (!jd_range_get(J, js_arg(a, n, 0), &r)) return js_undef();
    if (!jd_sel_range) jd_sel_range = a[0].obj;
    return js_undef();
}

static jval nat_sel_remove_all(jctx *J, jval t, jval *a, int n) { (void)J; (void)t; (void)a; (void)n; jd_sel_range = 0; return js_undef(); }

static jval nat_sel_range_at(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (!jd_sel_range || js_to_num(J, js_arg(a, n, 0)) != 0) return js_throw_dom(J, "IndexSizeError", "the selection has no range there");
    return js_from_obj(jd_sel_range);
}

/* anchorNode, focusNode (data 0, 1) and their offsets (data 2, 3). */
static jval nat_sel_point(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jrange r;
    if (!jd_sel_range || !jd_range_get(J, js_from_obj(jd_sel_range), &r)) {
        int k = (int)J->callee->data.num;
        return k < 2 ? js_null() : js_num(0);
    }
    switch ((int)J->callee->data.num) {
        case 0: return jd_node_value(J, r.sn);
        case 1: return jd_node_value(J, r.en);
        case 2: return js_num(r.so);
        default: return js_num(r.eo);
    }
}

static jval nat_sel_collapsed(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jrange r;
    if (!jd_sel_range || !jd_range_get(J, js_from_obj(jd_sel_range), &r)) return js_bool(1);
    return js_bool(r.sn == r.en && r.so == r.eo);
}

static jval nat_sel_type(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jrange r;
    if (!jd_sel_range || !jd_range_get(J, js_from_obj(jd_sel_range), &r)) return jd_str("None");
    return jd_str(r.sn == r.en && r.so == r.eo ? "Caret" : "Range");
}

static jval nat_sel_tostring(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    if (!jd_sel_range) return jd_str("");
    return nat_range_tostring(J, js_from_obj(jd_sel_range), 0, 0);
}

/* collapse(node, offset) and selectAllChildren(node) (data 0, 1). */
static jval nat_sel_place(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval nd = js_arg(a, n, 0);
    if (nd.t == JS_NULL) { jd_sel_range = 0; return js_undef(); }
    int node = jd_range_node(nd);
    if (node < JD_DOCNODE) return js_throw(J, JS_ERR_TYPE, "that is not a node", J->error_line);
    jobj *o = jd_range_new(J, 0);
    if (!o) return js_undef();
    jrange r;
    if (J->callee->data.num) { r.sn = r.en = node; r.so = 0; r.eo = jd_node_len(node); }
    else {
        int off = (int)js_to_num(J, js_arg(a, n, 1));
        if (off < 0 || off > jd_node_len(node)) return js_throw_dom(J, "IndexSizeError", "that offset is past the node's end");
        r.sn = r.en = node;
        r.so = r.eo = off;
    }
    jd_range_put(o, &r);
    jd_sel_range = o;
    return js_undef();
}

static jval nat_sel_contains(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (!jd_sel_range) return js_bool(0);
    return nat_range_intersects(J, js_from_obj(jd_sel_range), a, n);
}

/* --- setting it up ------------------------------------------------------------------------------ */

static void jd_fn(jctx *J, jobj *on, const char *name, jnative fn, int arity, double data) {
    jobj *f = js_native_n(J, name, fn, arity);
    if (!f || !on) return;
    f->flags |= JOF_NOCTOR;
    f->data = js_num(data);
    js_put_prop_flags(J, on, js_str(J, name), js_from_obj(f), JP_ENUM | JP_WRITE | JP_CONF);
}

/* --- attribute nodes, namespaces and commands --------------------------------------------- */

/* An Attr keeps its value under a key no script can name and, while an
   element holds it, reads and writes the value through that element, so
   setting a held attribute's value sets the element's attribute, as the
   standard has it. Mozilla's consent manager makes one with
   createAttribute and hands it to setAttributeNode; so does NPR's. */
static int jd_is_attr(jval v) {
    if (!js_is_obj(v)) return 0;
    for (jobj *p = v.obj->proto; p; p = p->proto)
        if (p == jd_p[JI_ATTR]) return 1;
    return 0;
}

static int jd_attr_owner(jctx *J, jval at) { return jd_el_of(js_get(J, at, js_str(J, "ownerElement"))); }

static jstr *jd_attr_name_of(jctx *J, jval at) {
    jval v = js_get(J, at, js_str(J, "name"));
    return v.t == JS_STR ? v.str : 0;
}

static jval nat_attr_value(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    if (!jd_is_attr(t)) return jd_illegal(J);
    int el = jd_attr_owner(J, t);
    jstr *nm = jd_attr_name_of(J, t);
    const char *v = el >= 0 && nm ? jd_attr(el, nm->s) : 0;
    if (v) return jd_str(v);
    jval kept = jd_kept(t.obj, jd_k_attr_value);
    return kept.t == JS_STR ? kept : jd_str("");
}

static jval nat_attr_set_value(jctx *J, jval t, jval *a, int n) {
    if (!jd_is_attr(t)) return jd_illegal(J);
    jstr *v = js_to_str(J, js_arg(a, n, 0));
    if (!v) return js_undef();
    jd_keep(t.obj, jd_k_attr_value, js_from_str(v));
    int el = jd_attr_owner(J, t);
    jstr *nm = jd_attr_name_of(J, t);
    if (el >= 0 && nm && nm->len) jd_attr_set(el, nm->s, v->s);
    return js_undef();
}

/* One nothing holds yet. A prefixed name is split into its prefix and
   local name. */
static jval jd_attr_new(jctx *J, jstr *name, jval ns) {
    jobj *o = js_object_with(J, JO_PLAIN, jd_p[JI_ATTR]);
    if (!o) return js_null();
    u32 colon = 0;
    for (u32 i = 0; i < name->len && !colon; i++)
        if (name->s[i] == ':') colon = i + 1;
    jval nm = js_from_str(name);
    js_set(J, o, "name", nm);
    js_set(J, o, "nodeName", nm);
    js_set(J, o, "localName", colon ? js_from_str(js_str_n(J, name->s + colon, name->len - colon)) : nm);
    js_set(J, o, "prefix", colon ? js_from_str(js_str_n(J, name->s, colon - 1)) : js_null());
    js_set(J, o, "nodeType", js_num(2));
    js_set(J, o, "specified", js_bool(1));
    js_set(J, o, "namespaceURI", ns);
    js_set(J, o, "ownerElement", js_null());
    jd_keep(o, jd_k_attr_value, jd_str(""));
    return js_from_obj(o);
}

/* A name the markup could have written: no spaces, quotes, slashes,
   equals signs or angle brackets, and something there. */
static int jd_attr_name_ok(const jstr *s) {
    if (!s || !s->len) return 0;
    for (u32 i = 0; i < s->len; i++) {
        char c = s->s[i];
        if (c <= ' ' || c == '"' || c == '\'' || c == '/' || c == '=' || c == '<' || c == '>') return 0;
    }
    return 1;
}

static jval nat_doc_create_attr(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *s = jd_arg_str(J, a, n, 0);
    if (!jd_attr_name_ok(s)) return js_throw_dom(J, "InvalidCharacterError", "that is not a name an attribute can have");
    /* An HTML document's attribute names are lower case. */
    char *low = (char *)malloc((u64)s->len + 1);
    if (!low) return js_null();
    for (u32 i = 0; i < s->len; i++) low[i] = s->s[i] >= 'A' && s->s[i] <= 'Z' ? (char)(s->s[i] + 32) : s->s[i];
    jval r = jd_attr_new(J, js_str_n(J, low, s->len), js_null());
    free(low);
    return r;
}

static jval nat_doc_create_attr_ns(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval nv = js_arg(a, n, 0);
    jstr *ns = nv.t == JS_NULL || nv.t == JS_UNDEF ? 0 : js_to_str(J, nv);
    jstr *s = jd_arg_str(J, a, n, 1);
    if (!jd_attr_name_ok(s)) return js_throw_dom(J, "InvalidCharacterError", "that is not a name an attribute can have");
    return jd_attr_new(J, s, ns && ns->len ? js_from_str(ns) : js_null());
}

/* setAttributeNode and setNamedItem: the element takes the attribute's
   name and value, and hands back the one it had by that name. */
static jval jd_attr_node_put(jctx *J, int el, jval at) {
    if (el < 0) return jd_illegal(J);
    if (!jd_is_attr(at)) return js_throw(J, JS_ERR_TYPE, "that is not an Attr", J->error_line);
    int ow = jd_attr_owner(J, at);
    if (ow >= 0 && ow != el) return js_throw_dom(J, "InUseAttributeError", "that attribute belongs to another element");
    jstr *nm = jd_attr_name_of(J, at);
    if (!nm || !nm->len) return js_null();
    if (ow == el && jd_attr(el, nm->s)) return at;
    jval v = jd_kept(at.obj, jd_k_attr_value);
    int i = jd_attr_index(el, nm->s);
    jval old = i >= 0 ? jd_attr_obj(J, el, i) : js_null();
    /* The one handed back keeps the value it had and belongs to nobody. */
    if (js_is_obj(old)) js_set(J, old.obj, "ownerElement", js_null());
    jd_attr_set(el, nm->s, v.t == JS_STR ? v.str->s : "");
    js_set(J, at.obj, "ownerElement", jd_el_value(J, el));
    return old;
}

static jval jd_attr_node_take(jctx *J, int el, jval at) {
    if (el < 0) return jd_illegal(J);
    if (!jd_is_attr(at)) return js_throw(J, JS_ERR_TYPE, "that is not an Attr", J->error_line);
    jstr *nm = jd_attr_name_of(J, at);
    const char *v = nm && jd_attr_owner(J, at) == el ? jd_attr(el, nm->s) : 0;
    if (!v) return js_throw_dom(J, "NotFoundError", "that attribute is not on this element");
    jd_keep(at.obj, jd_k_attr_value, jd_str(v));
    js_set(J, at.obj, "ownerElement", js_null());
    jd_attr_remove(el, nm->s);
    return at;
}

/* The attribute whose local name this is, prefixed or not. */
static int jd_attr_index_ns(int el, const char *local) {
    int i = jd_attr_index(el, local);
    if (i >= 0) return i;
    const dnode *d = &jd_doc->nodes[el];
    for (i = 0; i < d->attr_n; i++) {
        const char *nm = jd_doc->arena + jd_doc->attrs[d->attr_at + i].name, *colon = 0;
        for (const char *p = nm; *p; p++) if (*p == ':') colon = p;
        if (colon && w_same_fold(colon + 1, local)) return i;
    }
    return -1;
}

static jval nat_set_attr_node(jctx *J, jval t, jval *a, int n) { return jd_attr_node_put(J, jd_el_of(t), js_arg(a, n, 0)); }
static jval nat_remove_attr_node(jctx *J, jval t, jval *a, int n) { return jd_attr_node_take(J, jd_el_of(t), js_arg(a, n, 0)); }
static jval nat_attrs_set(jctx *J, jval t, jval *a, int n) { return jd_attr_node_put(J, jd_attrs_node(t), js_arg(a, n, 0)); }

static jval nat_get_attr_node(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    char nm[128];
    jd_attr_name(x, jd_arg_str(J, a, n, 0), nm, (int)sizeof(nm));
    return jd_attr_obj(J, x, jd_attr_index(x, nm));
}

static jval nat_get_attr_node_ns(jctx *J, jval t, jval *a, int n) {
    int x = jd_el_of(t);
    if (x < 0) return jd_illegal(J);
    return jd_attr_obj(J, x, jd_attr_index_ns(x, jd_arg_str(J, a, n, 1)->s));
}

static jval nat_attrs_named_ns(jctx *J, jval t, jval *a, int n) {
    int el = jd_attrs_node(t);
    if (el < 0) return jd_illegal(J);
    return jd_attr_obj(J, el, jd_attr_index_ns(el, jd_arg_str(J, a, n, 1)->s));
}

static jval nat_attrs_remove_ns(jctx *J, jval t, jval *a, int n) {
    int el = jd_attrs_node(t);
    if (el < 0) return jd_illegal(J);
    jval at = jd_attr_obj(J, el, jd_attr_index_ns(el, jd_arg_str(J, a, n, 1)->s));
    return js_is_obj(at) ? jd_attr_node_take(J, el, at) : js_throw_dom(J, "NotFoundError", "there is no such attribute");
}

/* The element a namespace is looked up from: the node itself, a text's
   or comment's parent, or a document's root element. */
static int jd_ns_element(jval t) {
    int x = jd_is_doc(t) ? jd_top() : jd_node_of(t);
    if (x < 0) return -1;
    int k = jd_kind(x);
    if (k == JN_DOCUMENT) x = jd_inert_element(x);
    else if (k == JN_TEXT || k == JN_COMMENT) x = jd_doc->nodes[x].parent;
    return jd_is_element(x) ? x : -1;
}

static jstr *jd_ns_arg(jctx *J, jval *a, int n) {
    jval v = js_arg(a, n, 0);
    jstr *s = v.t == JS_NULL || v.t == JS_UNDEF ? 0 : js_to_str(J, v);
    return s && s->len ? s : 0;
}

/* Every element here is HTML's or SVG's and has no prefix, so no prefix
   finds the element's own namespace; a prefix is looked for in the xmlns
   attributes of the element and those round it. */
static jval nat_lookup_ns(jctx *J, jval t, jval *a, int n) {
    jstr *p = jd_ns_arg(J, a, n);
    int x = jd_ns_element(t);
    if (x < 0) return js_null();
    if (!p) return nat_namespace(J, jd_el_value(J, x), 0, 0);
    if (w_same(p->s, "xml")) return jd_str("http://www.w3.org/XML/1998/namespace");
    if (w_same(p->s, "xmlns")) return jd_str("http://www.w3.org/2000/xmlns/");
    if (p->len > 100) return js_null();
    char nm[112] = "xmlns:";
    for (u32 i = 0; i <= p->len; i++) nm[6 + i] = p->s[i];
    for (; jd_is_element(x); x = jd_doc->nodes[x].parent) {
        const char *v = jd_attr(x, nm);
        if (v) return *v ? jd_str(v) : js_null();
    }
    return js_null();
}

static jval nat_lookup_prefix(jctx *J, jval t, jval *a, int n) {
    jstr *ns = jd_ns_arg(J, a, n);
    if (!ns) return js_null();
    for (int x = jd_ns_element(t); jd_is_element(x); x = jd_doc->nodes[x].parent) {
        const dnode *d = &jd_doc->nodes[x];
        for (int i = 0; i < d->attr_n; i++) {
            const char *nm = jd_doc->arena + jd_doc->attrs[d->attr_at + i].name;
            const char *v = jd_doc->arena + jd_doc->attrs[d->attr_at + i].value;
            int k = 0;
            while (k < 6 && nm[k] == "xmlns:"[k]) k++;
            if (k == 6 && nm[6] && w_same(v, ns->s)) return jd_str(nm + 6);
        }
    }
    return js_null();
}

static jval nat_is_default_ns(jctx *J, jval t, jval *a, int n) {
    jstr *ns = jd_ns_arg(J, a, n);
    jval def = nat_lookup_ns(J, t, 0, 0);
    if (def.t != JS_STR) return js_bool(!ns);
    return js_bool(ns && w_same(def.str->s, ns->s));
}

/* Editing commands: nothing here is editable, so none is supported,
   and a page that copies with execCommand("copy") is told it did not. */
static jval nat_exec_command(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return js_bool(0);
}

static jval nat_command_value(jctx *J, jval t, jval *a, int n) {
    (void)J; (void)t; (void)a; (void)n;
    return jd_str("");
}

/* An empty document, with a root element when a name is given. It is
   not told it is XML, and a doctype handed to it is not put in it. */
static jval nat_impl_create_doc(jctx *J, jval t, jval *a, int n) {
    (void)t;
    int d = jd_new_document(0, 0);
    if (d < 0) return js_throw_dom(J, "NotSupportedError", "the document is full");
    jval q = js_arg(a, n, 1);
    jstr *qn = q.t == JS_NULL ? 0 : js_to_str(J, q);
    if (qn && qn->len) {
        jstr *ns = jd_ns_arg(J, a, n);
        int el = dom_create_element(jd_doc, qn->s, (int)qn->len);
        if (el < 0) return js_throw_dom(J, "NotSupportedError", "the document is full");
        if (ns && js_str_is(ns, "http://www.w3.org/2000/svg") && el < DOM_NODES) jd_svg_made[el >> 3] |= (u8)(1 << (el & 7));
        dom_append(jd_doc, d, el);
    }
    return jd_el_value(J, d);
}

static jval nat_impl_create_doctype(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *name = jd_arg_str(J, a, n, 0);
    jobj *o = js_object_with(J, JO_PLAIN, jd_p_doctype);
    if (!o) return js_null();
    js_set(J, o, "name", js_from_str(name));
    js_set(J, o, "nodeName", js_from_str(name));
    js_set(J, o, "publicId", js_from_str(jd_arg_str(J, a, n, 1)));
    js_set(J, o, "systemId", js_from_str(jd_arg_str(J, a, n, 2)));
    js_set(J, o, "nodeType", js_num(10));
    return js_from_obj(o);
}

static void jd_getter_data(jctx *J, jobj *on, const char *name, jnative fn, double data) {
    jobj *g = js_native(J, name, fn);
    if (!g || !on) return;
    g->flags |= JOF_NOCTOR;
    g->data = js_num(data);
    js_define_accessor(J, on, js_str(J, name), js_from_obj(g), js_undef(), JP_ENUM | JP_CONF);
}

static void jd_setup_form(jctx *J) {
    jd_k_custom_msg = js_sym_new(J, "validationMessage", 17);
    jd_k_validity = js_sym_new(J, "validity", 8);
    jd_k_vs_el = js_sym_new(J, "element", 7);
    jd_k_rstart = js_sym_new(J, "start", 5);
    jd_k_roff = js_sym_new(J, "startOffset", 11);
    jd_k_rend = js_sym_new(J, "end", 3);
    jd_k_reoff = js_sym_new(J, "endOffset", 9);
    jd_k_attr_value = js_sym_new(J, "value", 5);
    jd_doctype_obj = 0;
    jd_selection = 0;
    jd_sel_range = 0;

    jd_p_validity = jd_interface(J, "ValidityState", 0, 0, 0);
    static const struct { const char *name; int bit; } FLAGS[] = {
        { "valueMissing", JV_MISSING }, { "typeMismatch", JV_TYPE }, { "patternMismatch", JV_PATTERN },
        { "tooLong", JV_LONG }, { "tooShort", JV_SHORT }, { "rangeUnderflow", JV_UNDER }, { "rangeOverflow", JV_OVER },
        { "stepMismatch", JV_STEP }, { "badInput", JV_BAD }, { "customError", JV_CUSTOM }, { "valid", 0 },
    };
    for (u32 i = 0; i < sizeof FLAGS / sizeof FLAGS[0]; i++)
        jd_getter_data(J, jd_p_validity, FLAGS[i].name, nat_validity_flag, FLAGS[i].bit);

    static const char *const FIELDS[] = { "HTMLInputElement", "HTMLSelectElement", "HTMLTextAreaElement", "HTMLButtonElement",
                                          "HTMLFieldSetElement", "HTMLOutputElement", "HTMLObjectElement", 0 };
    for (int i = 0; FIELDS[i]; i++) {
        jobj *p = jd_iface(FIELDS[i]);
        if (!p) continue;
        jd_accessor(J, p, "validity", nat_validity, 0);
        jd_accessor(J, p, "willValidate", nat_will_validate, 0);
        jd_accessor(J, p, "validationMessage", nat_validation_message, 0);
        jd_method(J, p, "checkValidity", nat_check_validity, 0);
        jd_method(J, p, "reportValidity", nat_check_validity, 0);
        jd_method(J, p, "setCustomValidity", nat_set_custom_validity, 1);
    }
    jobj *fp = jd_iface("HTMLFormElement");
    if (fp) {
        jd_method(J, fp, "checkValidity", nat_check_validity, 0);
        jd_method(J, fp, "reportValidity", nat_check_validity, 0);
    }

    jd_p_doctype = jd_interface(J, "DocumentType", jd_p[JI_NODE], 0, 0);
    jd_method(J, jd_p_doctype, "before", nat_nothing_node, 0);
    jd_method(J, jd_p_doctype, "after", nat_nothing_node, 0);
    jd_method(J, jd_p_doctype, "replaceWith", nat_nothing_node, 0);
    jd_method(J, jd_p_doctype, "remove", nat_nothing_node, 0);

    jobj *ap = jd_p[JI_ATTR];
    jd_accessor(J, ap, "value", nat_attr_value, nat_attr_set_value);
    jd_accessor(J, ap, "nodeValue", nat_attr_value, nat_attr_set_value);
    jd_accessor(J, ap, "textContent", nat_attr_value, nat_attr_set_value);
    jobj *ep = jd_p[JI_ELEMENT];
    jd_method(J, ep, "getAttributeNode", nat_get_attr_node, 1);
    jd_method(J, ep, "getAttributeNodeNS", nat_get_attr_node_ns, 2);
    jd_method(J, ep, "setAttributeNode", nat_set_attr_node, 1);
    jd_method(J, ep, "setAttributeNodeNS", nat_set_attr_node, 1);
    jd_method(J, ep, "removeAttributeNode", nat_remove_attr_node, 1);
    jobj *mp = jd_p[JI_NAMEDNODEMAP];
    jd_method(J, mp, "getNamedItemNS", nat_attrs_named_ns, 2);
    jd_method(J, mp, "setNamedItem", nat_attrs_set, 1);
    jd_method(J, mp, "setNamedItemNS", nat_attrs_set, 1);
    jd_method(J, mp, "removeNamedItemNS", nat_attrs_remove_ns, 2);
    jobj *np = jd_p[JI_NODE];
    jd_method(J, np, "lookupNamespaceURI", nat_lookup_ns, 1);
    jd_method(J, np, "lookupPrefix", nat_lookup_prefix, 1);
    jd_method(J, np, "isDefaultNamespace", nat_is_default_ns, 1);
    jobj *dp = jd_p[JI_DOCUMENT];
    jd_method(J, dp, "createAttribute", nat_doc_create_attr, 1);
    jd_method(J, dp, "createAttributeNS", nat_doc_create_attr_ns, 2);
    jd_method(J, dp, "execCommand", nat_exec_command, 1);
    jd_method(J, dp, "queryCommandSupported", nat_exec_command, 1);
    jd_method(J, dp, "queryCommandEnabled", nat_exec_command, 1);
    jd_method(J, dp, "queryCommandState", nat_exec_command, 1);
    jd_method(J, dp, "queryCommandIndeterminate", nat_exec_command, 1);
    jd_method(J, dp, "queryCommandValue", nat_command_value, 1);

    jd_p_serializer = jd_interface(J, "XMLSerializer", 0, nat_serializer_ctor, 0);
    jd_method(J, jd_p_serializer, "serializeToString", nat_serialize, 1);

    jd_p_range = jd_interface(J, "Range", 0, nat_range_ctor, 0);
    jobj *rp = jd_p_range;
    jd_getter_data(J, rp, "startContainer", nat_range_container, 0);
    jd_getter_data(J, rp, "endContainer", nat_range_container, 1);
    jd_getter_data(J, rp, "startOffset", nat_range_offset, 0);
    jd_getter_data(J, rp, "endOffset", nat_range_offset, 1);
    jd_accessor(J, rp, "collapsed", nat_range_collapsed, 0);
    jd_accessor(J, rp, "commonAncestorContainer", nat_range_common, 0);
    jd_fn(J, rp, "setStart", nat_range_set, 2, 0);
    jd_fn(J, rp, "setEnd", nat_range_set, 2, 1);
    jd_fn(J, rp, "setStartBefore", nat_range_set, 1, 2);
    jd_fn(J, rp, "setStartAfter", nat_range_set, 1, 3);
    jd_fn(J, rp, "setEndBefore", nat_range_set, 1, 4);
    jd_fn(J, rp, "setEndAfter", nat_range_set, 1, 5);
    jd_fn(J, rp, "selectNode", nat_range_select, 1, 0);
    jd_fn(J, rp, "selectNodeContents", nat_range_select, 1, 1);
    jd_fn(J, rp, "collapse", nat_range_collapse, 0, 0);
    jd_fn(J, rp, "cloneRange", nat_range_clone, 0, 0);
    jd_fn(J, rp, "detach", nat_nothing_node, 0, 0);
    jd_fn(J, rp, "toString", nat_range_tostring, 0, 0);
    jd_fn(J, rp, "createContextualFragment", nat_range_fragment, 1, 0);
    jd_fn(J, rp, "insertNode", nat_range_insert, 1, 0);
    jd_fn(J, rp, "deleteContents", nat_range_contents, 0, 0);
    jd_fn(J, rp, "extractContents", nat_range_contents, 0, 1);
    jd_fn(J, rp, "cloneContents", nat_range_contents, 0, 2);
    jd_fn(J, rp, "surroundContents", nat_range_surround, 1, 0);
    jd_fn(J, rp, "compareBoundaryPoints", nat_range_compare, 2, 0);
    jd_fn(J, rp, "comparePoint", nat_range_point, 2, 0);
    jd_fn(J, rp, "isPointInRange", nat_range_point, 2, 1);
    jd_fn(J, rp, "intersectsNode", nat_range_intersects, 1, 0);
    jd_fn(J, rp, "getBoundingClientRect", nat_range_rect, 0, 0);
    jd_fn(J, rp, "getClientRects", nat_range_rects, 0, 0);
    static const char *const HOW[] = { "START_TO_START", "START_TO_END", "END_TO_END", "END_TO_START", 0 };
    jobj *rc = jd_ctor_of(rp);
    for (int i = 0; HOW[i]; i++) {
        js_const_prop(J, rp, HOW[i], js_num(i));
        if (rc) js_const_prop(J, rc, HOW[i], js_num(i));
    }
    jd_method(J, jd_p[JI_DOCUMENT], "createRange", nat_create_range, 0);

    jd_p_selection = jd_interface(J, "Selection", 0, 0, 0);
    jobj *sp = jd_p_selection;
    jd_accessor(J, sp, "rangeCount", nat_sel_count, 0);
    jd_accessor(J, sp, "isCollapsed", nat_sel_collapsed, 0);
    jd_accessor(J, sp, "type", nat_sel_type, 0);
    jd_getter_data(J, sp, "anchorNode", nat_sel_point, 0);
    jd_getter_data(J, sp, "focusNode", nat_sel_point, 1);
    jd_getter_data(J, sp, "anchorOffset", nat_sel_point, 2);
    jd_getter_data(J, sp, "focusOffset", nat_sel_point, 3);
    jd_method(J, sp, "addRange", nat_sel_add, 1);
    jd_method(J, sp, "removeAllRanges", nat_sel_remove_all, 0);
    jd_method(J, sp, "empty", nat_sel_remove_all, 0);
    jd_method(J, sp, "removeRange", nat_sel_remove_all, 1);
    jd_method(J, sp, "getRangeAt", nat_sel_range_at, 1);
    jd_method(J, sp, "toString", nat_sel_tostring, 0);
    jd_fn(J, sp, "collapse", nat_sel_place, 2, 0);
    jd_fn(J, sp, "setPosition", nat_sel_place, 2, 0);
    jd_fn(J, sp, "selectAllChildren", nat_sel_place, 1, 1);
    jd_method(J, sp, "containsNode", nat_sel_contains, 1);
    jd_method(J, sp, "collapseToStart", nat_nothing_node, 0);
    jd_method(J, sp, "collapseToEnd", nat_nothing_node, 0);
    jd_method(J, sp, "extend", nat_nothing_node, 2);
    jd_method(J, sp, "setBaseAndExtent", nat_nothing_node, 4);
    jd_method(J, sp, "deleteFromDocument", nat_nothing_node, 0);
    jd_method(J, sp, "modify", nat_nothing_node, 3);
    js_declare(J, J->global, js_str(J, "getSelection"), js_from_obj(js_native_n(J, "getSelection", nat_get_selection, 0)));
    jd_method(J, jd_p[JI_DOCUMENT], "getSelection", nat_get_selection, 0);
}
