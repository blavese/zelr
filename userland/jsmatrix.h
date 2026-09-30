/* DOMMatrix, DOMMatrixReadOnly, DOMPoint and DOMPointReadOnly: the page's
 * own geometry, for the scripts that work a transform out themselves.
 * Apple's animations make a DOMMatrix of each element's computed transform
 * and read its sixteen numbers back, and stopped at "DOMMatrix is not a
 * constructor".
 *
 * A matrix is sixteen numbers in the standard's order, m11 m12 m13 m14 m21
 * ... m44, which is column after column of
 *
 *     m11 m21 m31 m41
 *     m12 m22 m32 m42
 *     m13 m23 m33 m43
 *     m14 m24 m34 m44
 *
 * applied to a point as a column (x' = m11 x + m21 y + m31 z + m41 w), and a
 * mark saying whether it is still two-dimensional. They are kept under a key
 * no script can name, and a, b ... f and m11 ... m44 are accessors on the
 * prototype that read them (and, on a DOMMatrix, write them). The methods
 * that do not end in Self give back a new DOMMatrix, as the standard has it;
 * a transform written as text is read as CSS writes one, in pixels and
 * angles, one function after another, and a length relative to something
 * (%, em) is refused, since a matrix has nothing to measure it against. */
#pragma once

static jstr *jd_k_mat, *jd_k_mat2d;
static jobj *jd_p_matro, *jd_p_mat, *jd_p_ptro, *jd_p_pt;

typedef struct { double m[16]; int is2d; } dmat;

static const char *const DMX_NAMES[16] = {
    "m11", "m12", "m13", "m14", "m21", "m22", "m23", "m24",
    "m31", "m32", "m33", "m34", "m41", "m42", "m43", "m44",
};
/* a b c d e f are m11 m12 m21 m22 m41 m42. */
static const int DMX_2D[6] = { 0, 1, 4, 5, 12, 13 };

static void dmx_identity(dmat *x) {
    for (int i = 0; i < 16; i++) x->m[i] = (i % 5 == 0) ? 1 : 0;
    x->is2d = 1;
}

/* r = a * b: b is applied to a point first. */
static void dmx_mul(dmat *r, const dmat *a, const dmat *b) {
    double out[16];
    for (int c = 0; c < 4; c++)
        for (int w = 0; w < 4; w++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a->m[4 * k + w] * b->m[4 * c + k];
            out[4 * c + w] = s;
        }
    for (int i = 0; i < 16; i++) r->m[i] = out[i];
    r->is2d = a->is2d && b->is2d;
}

static void dmx_post(dmat *x, const dmat *by) { dmx_mul(x, x, by); }

static double dmx_rad(double deg) { return deg * 3.141592653589793 / 180; }

static void dmx_translate(dmat *x, double tx, double ty, double tz) {
    dmat t;
    dmx_identity(&t);
    t.m[12] = tx;
    t.m[13] = ty;
    t.m[14] = tz;
    t.is2d = tz == 0;
    dmx_post(x, &t);
}

static void dmx_scale(dmat *x, double sx, double sy, double sz, double ox, double oy, double oz) {
    dmat s;
    dmx_translate(x, ox, oy, oz);
    dmx_identity(&s);
    s.m[0] = sx;
    s.m[5] = sy;
    s.m[10] = sz;
    s.is2d = sz == 1;
    dmx_post(x, &s);
    dmx_translate(x, -ox, -oy, -oz);
    if (sz != 1 || oz != 0) x->is2d = 0;
}

/* A turn of `deg` degrees about the axis (ax, ay, az), as CSS's rotate3d. */
static void dmx_rotate_axis(dmat *x, double ax, double ay, double az, double deg) {
    double len = js_sqrt(ax * ax + ay * ay + az * az);
    dmat r;
    dmx_identity(&r);
    if (len == 0 || deg == 0) { r.is2d = ax == 0 && ay == 0; dmx_post(x, &r); return; }
    ax /= len; ay /= len; az /= len;
    double h = dmx_rad(deg) / 2;
    double sc = js_circle(h, 0) * js_circle(h, 1), sq = js_circle(h, 0) * js_circle(h, 0);
    r.m[0] = 1 - 2 * (ay * ay + az * az) * sq;
    r.m[1] = 2 * (ax * ay * sq + az * sc);
    r.m[2] = 2 * (ax * az * sq - ay * sc);
    r.m[4] = 2 * (ax * ay * sq - az * sc);
    r.m[5] = 1 - 2 * (ax * ax + az * az) * sq;
    r.m[6] = 2 * (ay * az * sq + ax * sc);
    r.m[8] = 2 * (ax * az * sq + ay * sc);
    r.m[9] = 2 * (ay * az * sq - ax * sc);
    r.m[10] = 1 - 2 * (ax * ax + ay * ay) * sq;
    r.is2d = ax == 0 && ay == 0;
    dmx_post(x, &r);
}

static void dmx_skew(dmat *x, double degx, double degy) {
    dmat s;
    dmx_identity(&s);
    s.m[4] = js_circle(dmx_rad(degx), 0) / js_circle(dmx_rad(degx), 1);
    s.m[1] = js_circle(dmx_rad(degy), 0) / js_circle(dmx_rad(degy), 1);
    dmx_post(x, &s);
}

/* The inverse, by cofactors; 0 when there is none. */
static int dmx_invert(dmat *x) {
    const double *m = x->m;
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0 || !js_isfin(det)) {
        for (int i = 0; i < 16; i++) x->m[i] = js_nan();
        x->is2d = 0;
        return 0;
    }
    for (int i = 0; i < 16; i++) x->m[i] = inv[i] / det;
    return 1;
}

/* --- a transform written as text ------------------------------------------------------- */

static const char *dmx_space(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') p++;
    return p;
}

/* A number with its unit: what kind it is (0 a plain number, 1 a length in
   pixels, 2 an angle in degrees), or -1 when it is not one this can use. */
static const char *dmx_value(const char *p, double *v, int *kind) {
    p = dmx_space(p);
    char buf[48];
    int k = 0;
    if (*p == '+' || *p == '-') buf[k++] = *p++;
    while (((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E'
            || ((*p == '+' || *p == '-') && k && (buf[k - 1] == 'e' || buf[k - 1] == 'E'))) && k < 47) {
        if ((*p == 'e' || *p == 'E') && !(p[1] >= '0' && p[1] <= '9') && p[1] != '+' && p[1] != '-') break;
        buf[k++] = *p++;
    }
    buf[k] = 0;
    if (!k || (k == 1 && (buf[0] == '+' || buf[0] == '-'))) { *kind = -1; return p; }
    *v = js_str_to_num(buf, (u32)k);
    char unit[8];
    int u = 0;
    while (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '%') && u < 7) unit[u++] = w_lower(*p++);
    unit[u] = 0;
    if (!u) *kind = 0;
    else if (w_same(unit, "px")) *kind = 1;
    else if (w_same(unit, "deg")) *kind = 2;
    else if (w_same(unit, "rad")) { *kind = 2; *v = *v * 180 / 3.141592653589793; }
    else if (w_same(unit, "grad")) { *kind = 2; *v = *v * 0.9; }
    else if (w_same(unit, "turn")) { *kind = 2; *v = *v * 360; }
    else *kind = -1;
    if (*v != *v) *kind = -1;
    return p;
}

/* The transform list in `s` applied to x, one function after another. 0
   when it is not one. */
static int dmx_parse(dmat *x, const char *s) {
    dmx_identity(x);
    const char *p = dmx_space(s);
    if (!*p || w_starts_fold(p, "none")) {
        if (!*p) return 1;
        return !*dmx_space(p + 4);
    }
    while (*p) {
        char fn[16];
        int f = 0;
        while (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) && f < 15)
            fn[f++] = w_lower(*p++);
        fn[f] = 0;
        if (!f || *p != '(') return 0;
        p++;
        double v[16];
        int kind[16], nv = 0;
        for (;;) {
            p = dmx_space(p);
            if (*p == ')') { p++; break; }
            if (nv >= 16) return 0;
            p = dmx_value(p, &v[nv], &kind[nv]);
            if (kind[nv] < 0) return 0;
            nv++;
            p = dmx_space(p);
            if (*p == ',') p++;
        }
        /* What each function takes: lengths (a 0 may be written bare),
           angles, or numbers. */
        int len_ok = 1, ang_ok = 1, num_ok = 1;
        for (int i = 0; i < nv; i++) {
            if (!(kind[i] == 1 || (kind[i] == 0 && v[i] == 0))) len_ok = 0;
            if (!(kind[i] == 2 || (kind[i] == 0 && v[i] == 0))) ang_ok = 0;
            if (kind[i] != 0) num_ok = 0;
        }
        if (w_same(fn, "matrix") && nv == 6 && num_ok) {
            dmat t;
            dmx_identity(&t);
            for (int i = 0; i < 6; i++) t.m[DMX_2D[i]] = v[i];
            dmx_post(x, &t);
        } else if (w_same(fn, "matrix3d") && nv == 16 && num_ok) {
            dmat t;
            for (int i = 0; i < 16; i++) t.m[i] = v[i];
            t.is2d = 0;
            dmx_post(x, &t);
        } else if (w_same(fn, "translate") && (nv == 1 || nv == 2) && len_ok) dmx_translate(x, v[0], nv > 1 ? v[1] : 0, 0);
        else if (w_same(fn, "translatex") && nv == 1 && len_ok) dmx_translate(x, v[0], 0, 0);
        else if (w_same(fn, "translatey") && nv == 1 && len_ok) dmx_translate(x, 0, v[0], 0);
        else if (w_same(fn, "translatez") && nv == 1 && len_ok) dmx_translate(x, 0, 0, v[0]), x->is2d = 0;
        else if (w_same(fn, "translate3d") && nv == 3 && len_ok) dmx_translate(x, v[0], v[1], v[2]), x->is2d = 0;
        else if (w_same(fn, "scale") && (nv == 1 || nv == 2) && num_ok) dmx_scale(x, v[0], nv > 1 ? v[1] : v[0], 1, 0, 0, 0);
        else if (w_same(fn, "scalex") && nv == 1 && num_ok) dmx_scale(x, v[0], 1, 1, 0, 0, 0);
        else if (w_same(fn, "scaley") && nv == 1 && num_ok) dmx_scale(x, 1, v[0], 1, 0, 0, 0);
        else if (w_same(fn, "scalez") && nv == 1 && num_ok) dmx_scale(x, 1, 1, v[0], 0, 0, 0), x->is2d = 0;
        else if (w_same(fn, "scale3d") && nv == 3 && num_ok) dmx_scale(x, v[0], v[1], v[2], 0, 0, 0), x->is2d = 0;
        else if ((w_same(fn, "rotate") || w_same(fn, "rotatez")) && nv == 1 && ang_ok) {
            int flat = x->is2d;
            dmx_rotate_axis(x, 0, 0, 1, v[0]);
            if (w_same(fn, "rotatez")) x->is2d = 0;
            else x->is2d = flat;
        } else if (w_same(fn, "rotatex") && nv == 1 && ang_ok) dmx_rotate_axis(x, 1, 0, 0, v[0]), x->is2d = 0;
        else if (w_same(fn, "rotatey") && nv == 1 && ang_ok) dmx_rotate_axis(x, 0, 1, 0, v[0]), x->is2d = 0;
        else if (w_same(fn, "rotate3d") && nv == 4 && kind[0] == 0 && kind[1] == 0 && kind[2] == 0
                 && (kind[3] == 2 || (kind[3] == 0 && v[3] == 0)))
            dmx_rotate_axis(x, v[0], v[1], v[2], v[3]), x->is2d = 0;
        else if (w_same(fn, "skew") && (nv == 1 || nv == 2) && ang_ok) dmx_skew(x, v[0], nv > 1 ? v[1] : 0);
        else if (w_same(fn, "skewx") && nv == 1 && ang_ok) dmx_skew(x, v[0], 0);
        else if (w_same(fn, "skewy") && nv == 1 && ang_ok) dmx_skew(x, 0, v[0]);
        else if (w_same(fn, "perspective") && nv == 1 && len_ok) {
            dmat t;
            dmx_identity(&t);
            if (v[0] != 0) t.m[11] = -1 / v[0];
            t.is2d = 0;
            dmx_post(x, &t);
        } else return 0;
        p = dmx_space(p);
        if (*p == ',') return 0;
    }
    return 1;
}

/* --- the objects --------------------------------------------------------------------------- */

static int dmx_of(jval v, dmat *out) {
    if (!js_is_obj(v)) return 0;
    jval a = jd_kept(v.obj, jd_k_mat);
    if (a.t != JS_OBJ || a.obj->kind != JO_ARRAY || a.obj->len < 16) return 0;
    for (int i = 0; i < 16; i++) out->m[i] = a.obj->items[i].t == JS_NUM ? a.obj->items[i].num : js_nan();
    jval f = jd_kept(v.obj, jd_k_mat2d);
    out->is2d = f.t == JS_BOOL ? f.b : 1;
    return 1;
}

static void dmx_store(jobj *o, const dmat *x) {
    jctx *J = &jd_J;
    jval a = jd_kept(o, jd_k_mat);
    jobj *arr = a.t == JS_OBJ && a.obj->kind == JO_ARRAY ? a.obj : js_array(J);
    if (!arr) return;
    for (int i = 0; i < 16; i++) js_arr_set(J, arr, (u32)i, js_num(x->m[i]));
    jd_keep(o, jd_k_mat, js_from_obj(arr));
    jd_keep(o, jd_k_mat2d, js_bool(x->is2d));
}

static jval dmx_new(jctx *J, const dmat *x, jobj *proto) {
    jobj *o = js_object_with(J, JO_PLAIN, proto ? proto : jd_p_mat);
    if (!o) return js_undef();
    dmx_store(o, x);
    return js_from_obj(o);
}

/* A matrix from what a script handed over: another matrix, a dictionary
   of its numbers (fromMatrix), or nothing. */
static int dmx_from_value(jctx *J, jval v, dmat *out) {
    if (v.t == JS_UNDEF || v.t == JS_NULL) { dmx_identity(out); return 1; }
    if (dmx_of(v, out)) return 1;
    if (!js_is_obj(v)) return 0;
    dmx_identity(out);
    int any3d = 0;
    for (int i = 0; i < 16; i++) {
        jval m = js_get(J, v, js_str(J, DMX_NAMES[i]));
        if (J->sig != JS_OK) return 0;
        if (m.t != JS_UNDEF) {
            out->m[i] = js_to_num(J, m);
            if ((i == 10 || i == 15) ? out->m[i] != 1 : (i % 4 >= 2 || i >= 8) && i != 12 && i != 13 && out->m[i] != 0)
                any3d = 1;
        }
    }
    static const char *const AF[6] = { "a", "b", "c", "d", "e", "f" };
    for (int i = 0; i < 6; i++) {
        jval m = js_get(J, v, js_str(J, AF[i]));
        if (J->sig != JS_OK) return 0;
        if (m.t != JS_UNDEF) out->m[DMX_2D[i]] = js_to_num(J, m);
    }
    jval flat = js_get(J, v, js_str(J, "is2D"));
    out->is2d = flat.t == JS_UNDEF ? !any3d : js_to_bool(flat);
    if (out->is2d && any3d) {
        js_throw(J, JS_ERR_TYPE, "a two-dimensional matrix with three-dimensional numbers", J->error_line);
        return 0;
    }
    return 1;
}

static jval nat_matrix_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "DOMMatrix is made with new", J->error_line);
    dmat x;
    dmx_identity(&x);
    jval init = js_arg(a, n, 0);
    if (init.t == JS_STR) {
        if (!dmx_parse(&x, init.str->s)) return js_throw_dom(J, "SyntaxError", "a transform this cannot read");
    } else if (js_is_obj(init)) {
        u32 len = 0;
        jval l = js_get(J, init, js_str(J, "length"));
        if (J->sig != JS_OK) return js_undef();
        len = l.t == JS_NUM ? (u32)l.num : 0;
        if (len != 6 && len != 16)
            return js_throw(J, JS_ERR_TYPE, "a matrix is made of 6 numbers or 16", J->error_line);
        for (u32 i = 0; i < len; i++) {
            jval e = js_get(J, init, js_to_key(J, js_num(i)));
            if (J->sig != JS_OK) return js_undef();
            if (len == 6) x.m[DMX_2D[i]] = js_to_num(J, e);
            else x.m[i] = js_to_num(J, e);
        }
        x.is2d = len == 6;
    } else if (init.t != JS_UNDEF) {
        return js_throw(J, JS_ERR_TYPE, "a matrix is made from text or numbers", J->error_line);
    }
    if (!js_is_obj(t)) return dmx_new(J, &x, 0);
    dmx_store(t.obj, &x);
    return t;
}

static jval nat_matrix_field(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    dmat x;
    if (!dmx_of(t, &x)) return jd_illegal(J);
    return js_num(x.m[(int)J->callee->data.num]);
}

static jval nat_matrix_set_field(jctx *J, jval t, jval *a, int n) {
    dmat x;
    if (!dmx_of(t, &x)) return jd_illegal(J);
    int i = (int)J->callee->data.num;
    double v = js_to_num(J, js_arg(a, n, 0));
    x.m[i] = v;
    /* A number off the flat part made other than it is: no longer 2D. */
    if (i != 0 && i != 1 && i != 4 && i != 5 && i != 12 && i != 13 && v != ((i == 10 || i == 15) ? 1 : 0)) x.is2d = 0;
    dmx_store(t.obj, &x);
    return js_undef();
}

static jval nat_matrix_is2d(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    dmat x;
    if (!dmx_of(t, &x)) return jd_illegal(J);
    return js_bool(x.is2d);
}

static jval nat_matrix_isid(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    dmat x;
    if (!dmx_of(t, &x)) return jd_illegal(J);
    for (int i = 0; i < 16; i++) if (x.m[i] != ((i % 5 == 0) ? 1 : 0)) return js_bool(0);
    return js_bool(1);
}

static double dmx_argd(jctx *J, jval *a, int n, int i, double dflt) {
    return i < n && a[i].t != JS_UNDEF ? js_to_num(J, a[i]) : dflt;
}

/* What each method does to a matrix, by number: the Self forms change it,
   the others change a copy. */
enum { DMXO_TRANSLATE, DMXO_SCALE, DMXO_SCALE3D, DMXO_ROTATE, DMXO_ROTATE_VEC, DMXO_ROTATE_AXIS, DMXO_SKEWX,
       DMXO_SKEWY, DMXO_MULTIPLY, DMXO_PREMULTIPLY, DMXO_INVERT, DMXO_FLIPX, DMXO_FLIPY };

static int dmx_apply(jctx *J, dmat *x, int op, jval *a, int n) {
    switch (op) {
        case DMXO_TRANSLATE: {
            double tz = dmx_argd(J, a, n, 2, 0);
            dmx_translate(x, dmx_argd(J, a, n, 0, 0), dmx_argd(J, a, n, 1, 0), tz);
            if (tz != 0) x->is2d = 0;
            break;
        }
        case DMXO_SCALE: {
            double sx = dmx_argd(J, a, n, 0, 1);
            dmx_scale(x, sx, dmx_argd(J, a, n, 1, sx), dmx_argd(J, a, n, 2, 1), dmx_argd(J, a, n, 3, 0),
                     dmx_argd(J, a, n, 4, 0), dmx_argd(J, a, n, 5, 0));
            break;
        }
        case DMXO_SCALE3D: {
            double s = dmx_argd(J, a, n, 0, 1);
            dmx_scale(x, s, s, s, dmx_argd(J, a, n, 1, 0), dmx_argd(J, a, n, 2, 0), dmx_argd(J, a, n, 3, 0));
            if (s != 1) x->is2d = 0;
            break;
        }
        case DMXO_ROTATE: {
            double rx = dmx_argd(J, a, n, 0, 0), ry = dmx_argd(J, a, n, 1, 0), rz = dmx_argd(J, a, n, 2, 0);
            if ((n < 2 || a[1].t == JS_UNDEF) && (n < 3 || a[2].t == JS_UNDEF)) { rz = rx; rx = ry = 0; }
            int flat = x->is2d && rx == 0 && ry == 0;
            if (rz != 0) dmx_rotate_axis(x, 0, 0, 1, rz);
            if (ry != 0) dmx_rotate_axis(x, 0, 1, 0, ry);
            if (rx != 0) dmx_rotate_axis(x, 1, 0, 0, rx);
            x->is2d = flat;
            break;
        }
        case DMXO_ROTATE_VEC: {
            double vx = dmx_argd(J, a, n, 0, 0), vy = dmx_argd(J, a, n, 1, 0);
            int flat = x->is2d;
            if (vx != 0 || vy != 0) dmx_rotate_axis(x, 0, 0, 1, js_atan2(vy, vx) * 180 / 3.141592653589793);
            x->is2d = flat;
            break;
        }
        case DMXO_ROTATE_AXIS: {
            double ax = dmx_argd(J, a, n, 0, 0), ay = dmx_argd(J, a, n, 1, 0), az = dmx_argd(J, a, n, 2, 0);
            int flat = x->is2d && ax == 0 && ay == 0;
            dmx_rotate_axis(x, ax, ay, az, dmx_argd(J, a, n, 3, 0));
            x->is2d = flat;
            break;
        }
        case DMXO_SKEWX: dmx_skew(x, dmx_argd(J, a, n, 0, 0), 0); break;
        case DMXO_SKEWY: dmx_skew(x, 0, dmx_argd(J, a, n, 0, 0)); break;
        case DMXO_MULTIPLY: case DMXO_PREMULTIPLY: {
            dmat o;
            if (!dmx_from_value(J, js_arg(a, n, 0), &o)) {
                if (J->sig == JS_OK) js_throw(J, JS_ERR_TYPE, "that is not a matrix", J->error_line);
                return 0;
            }
            if (op == DMXO_MULTIPLY) dmx_mul(x, x, &o);
            else dmx_mul(x, &o, x);
            break;
        }
        case DMXO_INVERT: dmx_invert(x); break;
        case DMXO_FLIPX: case DMXO_FLIPY: {
            dmat f;
            dmx_identity(&f);
            f.m[op == DMXO_FLIPX ? 0 : 5] = -1;
            dmx_post(x, &f);
            break;
        }
    }
    return J->sig == JS_OK;
}

/* data: the operation; spare: 1 for the Self form, which changes this one. */
static jval nat_matrix_op(jctx *J, jval t, jval *a, int n) {
    dmat x;
    if (!dmx_of(t, &x)) return jd_illegal(J);
    int op = (int)J->callee->data.num, self = J->callee->spare == 1;
    if (!dmx_apply(J, &x, op, a, n)) return js_undef();
    if (self) { dmx_store(t.obj, &x); return t; }
    return dmx_new(J, &x, 0);
}

static jval nat_matrix_set_value(jctx *J, jval t, jval *a, int n) {
    dmat x;
    if (!dmx_of(t, &x)) return jd_illegal(J);
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s || J->sig != JS_OK) return js_undef();
    if (!dmx_parse(&x, s->s)) return js_throw_dom(J, "SyntaxError", "a transform this cannot read");
    dmx_store(t.obj, &x);
    return t;
}

static jval dmx_point(jctx *J, double x, double y, double z, double w, jobj *proto);

static jval nat_matrix_transform_point(jctx *J, jval t, jval *a, int n) {
    dmat x;
    if (!dmx_of(t, &x)) return jd_illegal(J);
    jval p = js_arg(a, n, 0);
    double v[4] = { 0, 0, 0, 1 };
    static const char *const K[4] = { "x", "y", "z", "w" };
    for (int i = 0; js_is_obj(p) && i < 4; i++) {
        jval e = js_get(J, p, js_str(J, K[i]));
        if (J->sig != JS_OK) return js_undef();
        if (e.t != JS_UNDEF) v[i] = js_to_num(J, e);
    }
    double r[4];
    for (int w = 0; w < 4; w++) r[w] = x.m[w] * v[0] + x.m[4 + w] * v[1] + x.m[8 + w] * v[2] + x.m[12 + w] * v[3];
    return dmx_point(J, r[0], r[1], r[2], r[3], jd_p_pt);
}

static void dmx_put_num(jctx *J, jtext *t, double v) {
    jstr *s = js_to_str(J, js_num(v));
    if (s) jt_put(J, t, s->s, s->len);
}

static jval nat_matrix_tostring(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    dmat x;
    if (!dmx_of(t, &x)) return jd_illegal(J);
    for (int i = 0; i < 16; i++)
        if (!js_isfin(x.m[i])) return js_throw_dom(J, "InvalidStateError", "a matrix with a number that is not finite has no text");
    jtext out = { 0, 0, 0, 0 };
    jt_put(J, &out, x.is2d ? "matrix(" : "matrix3d(", x.is2d ? 7 : 9);
    for (int i = 0; i < (x.is2d ? 6 : 16); i++) {
        if (i) jt_put(J, &out, ", ", 2);
        dmx_put_num(J, &out, x.m[x.is2d ? DMX_2D[i] : i]);
    }
    jt_put(J, &out, ")", 1);
    return js_from_str(jt_done(J, &out));
}

static jval nat_matrix_json(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    dmat x;
    if (!dmx_of(t, &x)) return jd_illegal(J);
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return js_undef();
    static const char *const AF[6] = { "a", "b", "c", "d", "e", "f" };
    for (int i = 0; i < 6; i++) js_set(J, o, AF[i], js_num(x.m[DMX_2D[i]]));
    for (int i = 0; i < 16; i++) js_set(J, o, DMX_NAMES[i], js_num(x.m[i]));
    js_set(J, o, "is2D", js_bool(x.is2d));
    js_set(J, o, "isIdentity", nat_matrix_isid(J, t, 0, 0));
    return js_from_obj(o);
}

/* toFloat32Array and toFloat64Array: spare is the element size. */
static jval nat_matrix_to_array(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    dmat x;
    if (!dmx_of(t, &x)) return jd_illegal(J);
    jobj *arr = js_array(J);
    for (int i = 0; arr && i < 16; i++) js_arr_push(J, arr, js_num(x.m[i]));
    jval ctor = js_get_str(J, js_from_obj(J->global_obj), J->callee->spare == 4 ? "Float32Array" : "Float64Array");
    if (!js_is_constructor(J, ctor) || !arr) return arr ? js_from_obj(arr) : js_undef();
    jval arg = js_from_obj(arr);
    return js_construct(J, ctor, &arg, 1, ctor);
}

static jval nat_matrix_from(jctx *J, jval t, jval *a, int n) {
    (void)t;
    dmat x;
    int which = J->callee->spare;                   /* 0 fromMatrix, 1 from an array */
    if (which == 0) {
        if (!dmx_from_value(J, js_arg(a, n, 0), &x)) {
            if (J->sig == JS_OK) js_throw(J, JS_ERR_TYPE, "that is not a matrix", J->error_line);
            return js_undef();
        }
    } else {
        jval ctor = js_from_obj(jd_ctor_of(jd_p_mat));
        jval arg = js_arg(a, n, 0);
        return js_construct(J, ctor, &arg, 1, ctor);
    }
    return dmx_new(J, &x, J->callee->data.t == JS_OBJ ? J->callee->data.obj : jd_p_mat);
}

/* --- points ---------------------------------------------------------------------------------- */

static jval dmx_point(jctx *J, double x, double y, double z, double w, jobj *proto) {
    jobj *o = js_object_with(J, JO_PLAIN, proto);
    if (!o) return js_undef();
    int flags = proto == jd_p_ptro ? JP_ENUM : JP_PLAIN;
    js_put_prop_flags(J, o, js_str(J, "x"), js_num(x), flags);
    js_put_prop_flags(J, o, js_str(J, "y"), js_num(y), flags);
    js_put_prop_flags(J, o, js_str(J, "z"), js_num(z), flags);
    js_put_prop_flags(J, o, js_str(J, "w"), js_num(w), flags);
    return js_from_obj(o);
}

static jval nat_point_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "DOMPoint is made with new", J->error_line);
    double v[4] = { dmx_argd(J, a, n, 0, 0), dmx_argd(J, a, n, 1, 0), dmx_argd(J, a, n, 2, 0), dmx_argd(J, a, n, 3, 1) };
    jobj *proto = js_is_obj(t) && t.obj->proto ? t.obj->proto : jd_p_pt;
    int ro = proto == jd_p_ptro;
    if (!js_is_obj(t)) return dmx_point(J, v[0], v[1], v[2], v[3], proto);
    static const char *const K[4] = { "x", "y", "z", "w" };
    for (int i = 0; i < 4; i++) js_put_prop_flags(J, t.obj, js_str(J, K[i]), js_num(v[i]), ro ? JP_ENUM : JP_PLAIN);
    return t;
}

static jval nat_point_transform(jctx *J, jval t, jval *a, int n) {
    jval m = js_arg(a, n, 0);
    dmat x;
    if (!dmx_from_value(J, m, &x)) {
        if (J->sig == JS_OK) js_throw(J, JS_ERR_TYPE, "that is not a matrix", J->error_line);
        return js_undef();
    }
    jval mv = dmx_new(J, &x, 0);
    return nat_matrix_transform_point(J, mv, &t, 1);
}

static jval nat_point_json(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jobj *o = js_object(J, JO_PLAIN);
    static const char *const K[4] = { "x", "y", "z", "w" };
    for (int i = 0; o && i < 4; i++) js_set(J, o, K[i], js_get(J, t, js_str(J, K[i])));
    return js_from_obj(o);
}

static jval nat_point_from(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval p = js_arg(a, n, 0);
    double v[4] = { 0, 0, 0, 1 };
    static const char *const K[4] = { "x", "y", "z", "w" };
    for (int i = 0; js_is_obj(p) && i < 4; i++) {
        jval e = js_get(J, p, js_str(J, K[i]));
        if (J->sig != JS_OK) return js_undef();
        if (e.t != JS_UNDEF) v[i] = js_to_num(J, e);
    }
    return dmx_point(J, v[0], v[1], v[2], v[3], J->callee->data.t == JS_OBJ ? J->callee->data.obj : jd_p_pt);
}

/* --- setting it up --------------------------------------------------------------------------- */

static void dmx_method(jctx *J, jobj *on, const char *name, jnative fn, int arity, double data, int spare) {
    jobj *f = js_native_n(J, name, fn, arity);
    if (!f || !on) return;
    f->flags |= JOF_NOCTOR;
    f->data = js_num(data);
    f->spare = (u16)spare;
    js_put_prop_flags(J, on, js_str(J, name), js_from_obj(f), JP_ENUM | JP_WRITE | JP_CONF);
}

static void jd_setup_matrix(jctx *J) {
    jd_k_mat = js_sym_new(J, "matrix", 6);
    jd_k_mat2d = js_sym_new(J, "is2D", 4);
    jd_p_matro = jd_interface(J, "DOMMatrixReadOnly", 0, nat_matrix_ctor, 0);
    jd_p_mat = jd_interface(J, "DOMMatrix", jd_p_matro, nat_matrix_ctor, 0);
    if (!jd_p_matro || !jd_p_mat) return;

    static const char *const AF[6] = { "a", "b", "c", "d", "e", "f" };
    for (int i = 0; i < 22; i++) {
        const char *name = i < 6 ? AF[i] : DMX_NAMES[i - 6];
        int idx = i < 6 ? DMX_2D[i] : i - 6;
        jobj *g = js_native(J, name, nat_matrix_field);
        jobj *s = js_native(J, name, nat_matrix_set_field);
        if (!g || !s) continue;
        g->flags |= JOF_NOCTOR;
        s->flags |= JOF_NOCTOR;
        g->data = s->data = js_num(idx);
        js_define_accessor(J, jd_p_matro, js_str(J, name), js_from_obj(g), js_undef(), JP_ENUM | JP_CONF);
        js_define_accessor(J, jd_p_mat, js_str(J, name), js_from_obj(g), js_from_obj(s), JP_ENUM | JP_CONF);
    }
    jd_accessor(J, jd_p_matro, "is2D", nat_matrix_is2d, 0);
    jd_accessor(J, jd_p_matro, "isIdentity", nat_matrix_isid, 0);

    static const struct { const char *name; int op, arity; } OPS[] = {
        { "translate", DMXO_TRANSLATE, 0 }, { "scale", DMXO_SCALE, 0 }, { "scale3d", DMXO_SCALE3D, 0 },
        { "rotate", DMXO_ROTATE, 0 }, { "rotateFromVector", DMXO_ROTATE_VEC, 0 },
        { "rotateAxisAngle", DMXO_ROTATE_AXIS, 0 }, { "skewX", DMXO_SKEWX, 0 }, { "skewY", DMXO_SKEWY, 0 },
        { "multiply", DMXO_MULTIPLY, 0 }, { "inverse", DMXO_INVERT, 0 }, { "flipX", DMXO_FLIPX, 0 },
        { "flipY", DMXO_FLIPY, 0 },
    };
    for (u32 i = 0; i < sizeof OPS / sizeof OPS[0]; i++)
        dmx_method(J, jd_p_matro, OPS[i].name, nat_matrix_op, OPS[i].arity, OPS[i].op, 0);
    /* The Self forms, on DOMMatrix only; inverse's is invertSelf. */
    static const struct { const char *name; int op; } SELF[] = {
        { "translateSelf", DMXO_TRANSLATE }, { "scaleSelf", DMXO_SCALE }, { "scale3dSelf", DMXO_SCALE3D },
        { "rotateSelf", DMXO_ROTATE }, { "rotateFromVectorSelf", DMXO_ROTATE_VEC },
        { "rotateAxisAngleSelf", DMXO_ROTATE_AXIS }, { "skewXSelf", DMXO_SKEWX }, { "skewYSelf", DMXO_SKEWY },
        { "multiplySelf", DMXO_MULTIPLY }, { "preMultiplySelf", DMXO_PREMULTIPLY }, { "invertSelf", DMXO_INVERT },
    };
    for (u32 i = 0; i < sizeof SELF / sizeof SELF[0]; i++)
        dmx_method(J, jd_p_mat, SELF[i].name, nat_matrix_op, 0, SELF[i].op, 1);
    dmx_method(J, jd_p_mat, "setMatrixValue", nat_matrix_set_value, 1, 0, 0);
    dmx_method(J, jd_p_matro, "transformPoint", nat_matrix_transform_point, 0, 0, 0);
    dmx_method(J, jd_p_matro, "toFloat32Array", nat_matrix_to_array, 0, 0, 4);
    dmx_method(J, jd_p_matro, "toFloat64Array", nat_matrix_to_array, 0, 0, 8);
    dmx_method(J, jd_p_matro, "toJSON", nat_matrix_json, 0, 0, 0);
    dmx_method(J, jd_p_matro, "toString", nat_matrix_tostring, 0, 0, 0);

    jobj *cro = jd_ctor_of(jd_p_matro), *cm = jd_ctor_of(jd_p_mat);
    static const char *const FROM[3] = { "fromMatrix", "fromFloat32Array", "fromFloat64Array" };
    for (int k = 0; k < 2; k++) {
        jobj *c = k ? cm : cro;
        for (int i = 0; c && i < 3; i++) {
            jobj *f = js_native_n(J, FROM[i], nat_matrix_from, 1);
            if (!f) continue;
            f->flags |= JOF_NOCTOR;
            f->spare = (u16)(i ? 1 : 0);
            f->data = js_from_obj(k ? jd_p_mat : jd_p_matro);
            js_put_prop_flags(J, c, js_str(J, FROM[i]), js_from_obj(f), JP_WRITE | JP_CONF);
        }
    }
    /* WebKit's name for it, which Apple's scripts look for too; in Safari
       and Chrome it is the same constructor. */
    if (cm) js_declare(J, J->global, js_str(J, "WebKitCSSMatrix"), js_from_obj(cm));

    jd_p_ptro = jd_interface(J, "DOMPointReadOnly", 0, nat_point_ctor, 0);
    jd_p_pt = jd_interface(J, "DOMPoint", jd_p_ptro, nat_point_ctor, 0);
    dmx_method(J, jd_p_ptro, "matrixTransform", nat_point_transform, 0, 0, 0);
    dmx_method(J, jd_p_ptro, "toJSON", nat_point_json, 0, 0, 0);
    jobj *pro = jd_ctor_of(jd_p_ptro), *pp = jd_ctor_of(jd_p_pt);
    for (int k = 0; k < 2; k++) {
        jobj *c = k ? pp : pro;
        jobj *f = c ? js_native_n(J, "fromPoint", nat_point_from, 1) : 0;
        if (!f) continue;
        f->flags |= JOF_NOCTOR;
        f->data = js_from_obj(k ? jd_p_pt : jd_p_ptro);
        js_put_prop_flags(J, c, js_str(J, "fromPoint"), js_from_obj(f), JP_WRITE | JP_CONF);
    }
}
