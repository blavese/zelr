/* Intl: numbers, dates, plurals, relative times, lists, comparison and
 * segmenting written the way a page's reader expects them. GitHub and
 * Spotify stopped at "Intl is not defined", and a page that formats a price
 * or a date with it is most pages.
 *
 * There is one locale here, English as the United States writes it, and
 * every object says so: whatever a page asks for, resolvedOptions().locale is
 * en-US, and supportedLocalesOf keeps only the English tags it was given.
 * Formatting 1234.5 the German way while claiming to is not an option, and
 * pretending to be German while writing it the American way is worse.
 *
 * Numbers are rounded from the shortest decimal that reads back as the
 * double (jsnum.h, js_shortest), as the browsers' ICU rounds them, half away
 * from nought as the standard's default mode has it. Dates are in the machine's time, which is
 * UTC, or in one of the time zones in INTL_ZONES, whose offsets and summer
 * rules are the ones in force in 2026; a zone this does not know is shown in
 * UTC and resolvedOptions says so. Each object keeps what it resolved as
 * bytes in a string under a key no script can name. */
#pragma once

/* --- the locale ------------------------------------------------------------------------ */

static jstr *intl_k_state;

static u32 intl_len(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static int intl_same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

enum { INTL_NF = 0x4E464D54, INTL_DTF, INTL_PR, INTL_RTF, INTL_COL, INTL_LF, INTL_SEG, INTL_DN, INTL_LOC };

static void intl_store(jctx *J, jobj *o, const void *st, u32 size) {
    if (!o) return;
    jstr *s = js_str_n(J, (const char *)st, size);
    jprop *p = s ? js_put_prop(J, o, intl_k_state, js_from_str(s)) : 0;
    if (p) p->flags = 0;
}

/* The state of this kind kept on t, or 0: a TypeError is thrown for
   anything else, as the standard's methods do. */
static int intl_load(jctx *J, jval t, u32 kind, void *st, u32 size) {
    jprop *p = js_is_obj(t) ? js_find(t.obj, intl_k_state) : 0;
    if (!p || p->v.t != JS_STR || p->v.str->len != size) {
        js_throw(J, JS_ERR_TYPE, "this is not the Intl object the method belongs to", J->error_line);
        return 0;
    }
    const char *b = p->v.str->s;
    u32 k;
    char *d = (char *)st;
    for (u32 i = 0; i < size; i++) d[i] = b[i];
    k = *(const u32 *)st;
    if (k != kind) {
        js_throw(J, JS_ERR_TYPE, "this is not the Intl object the method belongs to", J->error_line);
        return 0;
    }
    return 1;
}

static int intl_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int intl_alnum(char c) { return intl_alpha(c) || (c >= '0' && c <= '9'); }
static char intl_low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static char intl_up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

/* A language tag in its canonical case -- language lower, script title,
   region upper -- or 0 when it is not one. */
static int intl_canon(const char *s, u32 n, char *out, int cap) {
    int w = 0, part = 0;
    u32 i = 0;
    if (n == 0) return 0;
    while (i < n) {
        u32 st = i;
        while (i < n && s[i] != '-' && s[i] != '_') {
            if (!intl_alnum(s[i])) return 0;
            i++;
        }
        u32 len = i - st;
        if (len == 0 || len > 8) return 0;
        if (part == 0 && (len < 2 || len > 8 || len == 4)) return 0;
        if (part == 0) for (u32 k = st; k < i; k++) if (!intl_alpha(s[k])) return 0;
        if (w + (int)len + 2 >= cap) return 0;
        if (part) out[w++] = '-';
        for (u32 k = st; k < i; k++) {
            char c = intl_low(s[k]);
            if (part && len == 4 && intl_alpha(s[st]) && k == st) c = intl_up(c);        /* script */
            else if (part && len == 2 && intl_alpha(s[st])) c = intl_up(c);              /* region */
            out[w++] = c;
        }
        part++;
        if (i < n) i++;
        if (i == n && (s[n - 1] == '-' || s[n - 1] == '_')) return 0;
    }
    out[w] = 0;
    return w;
}

/* The locales a script asked for, canonical and without repeats: a string,
   a Locale, or a list of either. A malformed one is a RangeError. */
static jobj *intl_requested(jctx *J, jval locales) {
    jobj *out = js_array(J);
    if (!out || locales.t == JS_UNDEF) return out;
    jargs A;
    js_args_init(&A);
    if (locales.t == JS_STR) js_args_push(J, &A, locales);
    else if (js_is_obj(locales) && js_find(locales.obj, intl_k_state)) js_args_push(J, &A, locales);
    else {
        jval len = js_get_str(J, locales, "length");
        u32 nl = len.t == JS_NUM && len.num > 0 ? (u32)len.num : 0;
        for (u32 i = 0; i < nl && J->sig == JS_OK; i++) js_args_push(J, &A, js_get(J, locales, js_to_key(J, js_num(i))));
    }
    for (int i = 0; i < A.n && J->sig == JS_OK; i++) {
        jval v = A.v[i];
        if (v.t != JS_STR && !js_is_obj(v)) { js_throw(J, JS_ERR_TYPE, "a locale is a string", J->error_line); break; }
        jstr *s = js_to_str(J, v);
        if (!s || J->sig != JS_OK) break;
        char c[64];
        if (!intl_canon(s->s, s->len, c, (int)sizeof c)) {
            js_throw(J, JS_ERR_RANGE, "that is not a language tag", J->error_line);
            break;
        }
        int dup = 0;
        for (u32 k = 0; k < out->len; k++)
            if (out->items[k].t == JS_STR && intl_same(out->items[k].str->s, c)) dup = 1;
        if (!dup) js_arr_push(J, out, js_from_str(js_str(J, c)));
    }
    js_args_free(&A);
    return out;
}

static jval nat_intl_canonical(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *l = intl_requested(J, js_arg(a, n, 0));
    return J->sig == JS_OK && l ? js_from_obj(l) : js_undef();
}

/* supportedLocalesOf: the English ones of those asked for. */
static jval nat_intl_supported(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jobj *l = intl_requested(J, js_arg(a, n, 0));
    jobj *out = js_array(J);
    if (J->sig != JS_OK || !l || !out) return js_undef();
    for (u32 i = 0; i < l->len; i++) {
        const char *s = l->items[i].str->s;
        if (s[0] == 'e' && s[1] == 'n' && (s[2] == 0 || s[2] == '-')) js_arr_push(J, out, l->items[i]);
    }
    return js_from_obj(out);
}

/* --- options ------------------------------------------------------------------------------ */

static jval intl_options(jctx *J, jval o) {
    if (o.t == JS_UNDEF) return js_undef();
    if (o.t == JS_NULL) { js_throw(J, JS_ERR_TYPE, "options cannot be null", J->error_line); return js_undef(); }
    return o;
}

/* Which of `allowed` the option is (a list ending in 0), dflt when it is not
   given; a value not on the list is a RangeError. */
static int intl_opt(jctx *J, jval opts, const char *name, const char *const *allowed, int dflt) {
    if (!js_is_obj(opts) || J->sig != JS_OK) return dflt;
    jval v = js_get_str(J, opts, name);
    if (J->sig != JS_OK || v.t == JS_UNDEF) return dflt;
    jstr *s = js_to_str(J, v);
    if (!s || J->sig != JS_OK) return dflt;
    for (int i = 0; allowed[i]; i++) if (intl_same(s->s, allowed[i])) return i;
    js_throw_named(J, JS_ERR_RANGE, "", s, " is not a value this option takes");
    return dflt;
}

static int intl_opt_num(jctx *J, jval opts, const char *name, int lo, int hi, int dflt) {
    if (!js_is_obj(opts) || J->sig != JS_OK) return dflt;
    jval v = js_get_str(J, opts, name);
    if (J->sig != JS_OK || v.t == JS_UNDEF) return dflt;
    double d = js_to_num(J, v);
    if (d != d || d < lo || d > hi) {
        js_throw(J, JS_ERR_RANGE, "a number option out of its range", J->error_line);
        return dflt;
    }
    return (int)js_floor(d);
}

/* -1 when not given. */
static int intl_opt_bool(jctx *J, jval opts, const char *name) {
    if (!js_is_obj(opts) || J->sig != JS_OK) return -1;
    jval v = js_get_str(J, opts, name);
    if (J->sig != JS_OK || v.t == JS_UNDEF) return -1;
    return js_to_bool(v);
}

static int intl_opt_text(jctx *J, jval opts, const char *name, char *out, int cap) {
    out[0] = 0;
    if (!js_is_obj(opts) || J->sig != JS_OK) return 0;
    jval v = js_get_str(J, opts, name);
    if (J->sig != JS_OK || v.t == JS_UNDEF) return 0;
    jstr *s = js_to_str(J, v);
    if (!s || J->sig != JS_OK) return 0;
    int w = 0;
    for (u32 i = 0; i < s->len && w < cap - 1; i++) out[w++] = s->s[i];
    out[w] = 0;
    return 1;
}

static void intl_set(jctx *J, jobj *o, const char *name, jval v) { if (o) js_set(J, o, name, v); }
static void intl_set_str(jctx *J, jobj *o, const char *name, const char *v) { intl_set(J, o, name, js_from_str(js_str(J, v))); }

/* The format getter of NumberFormat and DateTimeFormat, and Collator's
   compare: a function bound to its object, made once, so it can be handed
   round (list.map(nf.format)). */
static jval intl_bound(jctx *J, jval t, const char *key, jnative fn, int arity) {
    if (!js_is_obj(t)) return js_throw(J, JS_ERR_TYPE, "this is not an Intl object", J->error_line);
    jstr *k = js_intern(J, key, (u32)intl_len(key));
    jprop *p = js_find(t.obj, k);
    if (p && p->v.t == JS_OBJ) return p->v;
    jobj *f = js_native_n(J, "", fn, arity);
    if (!f) return js_undef();
    f->flags |= JOF_NOCTOR;
    f->data = t;
    jprop *q = js_put_prop(J, t.obj, k, js_from_obj(f));
    if (q) q->flags = 0;
    return js_from_obj(f);
}

/* Output as pieces with their types, for format and formatToParts alike. */
typedef struct {
    jtext text;
    int n;
    struct { const char *type; u32 at, len; const char *unit; } p[520];
} intl_parts;

static void ip_add(jctx *J, intl_parts *P, const char *type, const char *s, u32 len) {
    if (!len) return;
    if (P->n > 0 && P->n >= 519) { jt_put(J, &P->text, s, len); P->p[P->n - 1].len += len; return; }
    P->p[P->n].type = type;
    P->p[P->n].at = P->text.n;
    P->p[P->n].len = len;
    P->p[P->n].unit = 0;
    P->n++;
    jt_put(J, &P->text, s, len);
}

static void ip_adds(jctx *J, intl_parts *P, const char *type, const char *s) { ip_add(J, P, type, s, (u32)intl_len(s)); }

static jval ip_string(jctx *J, intl_parts *P) {
    jstr *s = jt_done(J, &P->text);
    return s ? js_from_str(s) : js_undef();
}

static jval ip_array(jctx *J, intl_parts *P, const char *extra_key, jval extra) {
    jobj *out = js_array(J);
    for (int i = 0; out && i < P->n; i++) {
        jobj *o = js_object(J, JO_PLAIN);
        if (!o) break;
        js_set(J, o, "type", js_from_str(js_str(J, P->p[i].type)));
        js_set(J, o, "value", js_from_str(js_str_n(J, P->text.b + P->p[i].at, P->p[i].len)));
        if (P->p[i].unit) js_set(J, o, "unit", js_from_str(js_str(J, P->p[i].unit)));
        if (extra_key) js_set(J, o, extra_key, extra);
        js_arr_push(J, out, js_from_obj(o));
    }
    free(P->text.b);
    P->text.b = 0;
    return out ? js_from_obj(out) : js_undef();
}

/* --- numbers ------------------------------------------------------------------------------ */

enum { NFS_DECIMAL, NFS_PERCENT, NFS_CURRENCY, NFS_UNIT };
enum { NFN_STANDARD, NFN_SCIENTIFIC, NFN_ENGINEERING, NFN_COMPACT };
enum { NFG_OFF, NFG_AUTO, NFG_ALWAYS, NFG_MIN2 };
enum { NFSD_AUTO, NFSD_ALWAYS, NFSD_NEVER, NFSD_EXCEPTZERO, NFSD_NEGATIVE };

typedef struct {
    u32 kind;
    u8 style, cur_display, accounting, unit_display, notation, compact_long, grouping, sign;
    u8 use_sig, compact_round, min_int, min_frac, max_frac, min_sig, max_sig, pad;
    char currency[4];
    char unit[48];
} intl_nf;

typedef struct { const char *code, *symbol, *narrow, *one, *other; int digits; } intl_currency;

static const intl_currency INTL_CURRENCIES[] = {
    { "USD", "$", "$", "US dollar", "US dollars", 2 }, { "EUR", "\xE2\x82\xAC", "\xE2\x82\xAC", "euro", "euros", 2 },
    { "GBP", "\xC2\xA3", "\xC2\xA3", "British pound", "British pounds", 2 },
    { "JPY", "\xC2\xA5", "\xC2\xA5", "Japanese yen", "Japanese yen", 0 },
    { "CNY", "CN\xC2\xA5", "\xC2\xA5", "Chinese yuan", "Chinese yuan", 2 },
    { "INR", "\xE2\x82\xB9", "\xE2\x82\xB9", "Indian rupee", "Indian rupees", 2 },
    { "CAD", "CA$", "$", "Canadian dollar", "Canadian dollars", 2 },
    { "AUD", "A$", "$", "Australian dollar", "Australian dollars", 2 },
    { "NZD", "NZ$", "$", "New Zealand dollar", "New Zealand dollars", 2 },
    { "HKD", "HK$", "$", "Hong Kong dollar", "Hong Kong dollars", 2 },
    { "SGD", "SGD", "$", "Singapore dollar", "Singapore dollars", 2 },
    { "TWD", "NT$", "$", "New Taiwan dollar", "New Taiwan dollars", 2 },
    { "MXN", "MX$", "$", "Mexican peso", "Mexican pesos", 2 },
    { "BRL", "R$", "R$", "Brazilian real", "Brazilian reals", 2 },
    { "CHF", "CHF", "CHF", "Swiss franc", "Swiss francs", 2 },
    { "SEK", "SEK", "kr", "Swedish krona", "Swedish kronor", 2 },
    { "NOK", "NOK", "kr", "Norwegian krone", "Norwegian kroner", 2 },
    { "DKK", "DKK", "kr", "Danish krone", "Danish kroner", 2 },
    { "PLN", "PLN", "z\xC5\x82", "Polish zloty", "Polish zlotys", 2 },
    { "CZK", "CZK", "K\xC4\x8D", "Czech koruna", "Czech korunas", 2 },
    { "HUF", "HUF", "Ft", "Hungarian forint", "Hungarian forints", 2 },
    { "RUB", "RUB", "\xE2\x82\xBD", "Russian ruble", "Russian rubles", 2 },
    { "TRY", "TRY", "\xE2\x82\xBA", "Turkish lira", "Turkish Lira", 2 },
    { "ILS", "\xE2\x82\xAA", "\xE2\x82\xAA", "Israeli new shekel", "Israeli new shekels", 2 },
    { "KRW", "\xE2\x82\xA9", "\xE2\x82\xA9", "South Korean won", "South Korean won", 0 },
    { "VND", "\xE2\x82\xAB", "\xE2\x82\xAB", "Vietnamese dong", "Vietnamese dong", 0 },
    { "THB", "THB", "\xE0\xB8\xBF", "Thai baht", "Thai baht", 2 },
    { "PHP", "\xE2\x82\xB1", "\xE2\x82\xB1", "Philippine peso", "Philippine pesos", 2 },
    { "IDR", "IDR", "Rp", "Indonesian rupiah", "Indonesian rupiahs", 2 },
    { "MYR", "MYR", "RM", "Malaysian ringgit", "Malaysian ringgits", 2 },
    { "ZAR", "ZAR", "R", "South African rand", "South African rand", 2 },
    { "NGN", "NGN", "\xE2\x82\xA6", "Nigerian naira", "Nigerian nairas", 2 },
    { "EGP", "EGP", "E\xC2\xA3", "Egyptian pound", "Egyptian pounds", 2 },
    { "AED", "AED", "AED", "UAE dirham", "UAE dirhams", 2 },
    { "SAR", "SAR", "SAR", "Saudi riyal", "Saudi riyals", 2 },
    { "ARS", "ARS", "$", "Argentine peso", "Argentine pesos", 2 },
    { "CLP", "CLP", "$", "Chilean peso", "Chilean pesos", 0 },
    { "COP", "COP", "$", "Colombian peso", "Colombian pesos", 2 },
    { "UAH", "UAH", "\xE2\x82\xB4", "Ukrainian hryvnia", "Ukrainian hryvnias", 2 },
    { "BTC", "BTC", "BTC", "BTC", "BTC", 2 },
    { 0, 0, 0, 0, 0, 0 },
};

static const intl_currency *intl_currency_of(const char *code) {
    for (int i = 0; INTL_CURRENCIES[i].code; i++) if (intl_same(INTL_CURRENCIES[i].code, code)) return &INTL_CURRENCIES[i];
    return 0;
}

/* Units as English writes them: short and narrow for one and more, long
   for one and more. */
typedef struct { const char *id, *s1, *sn, *n1, *nn, *l1, *ln; int tight; } intl_unit;

static const intl_unit INTL_UNITS[] = {
    { "percent", "%", "%", "%", "%", "percent", "percent", 1 },
    { "second", "sec", "sec", "s", "s", "second", "seconds", 0 },
    { "minute", "min", "min", "m", "m", "minute", "minutes", 0 },
    { "hour", "hr", "hr", "h", "h", "hour", "hours", 0 },
    { "day", "day", "days", "d", "d", "day", "days", 0 },
    { "week", "wk", "wks", "w", "w", "week", "weeks", 0 },
    { "month", "mth", "mths", "m", "m", "month", "months", 0 },
    { "year", "yr", "yrs", "y", "y", "year", "years", 0 },
    { "millisecond", "ms", "ms", "ms", "ms", "millisecond", "milliseconds", 0 },
    { "microsecond", "\xCE\xBCs", "\xCE\xBCs", "\xCE\xBCs", "\xCE\xBCs", "microsecond", "microseconds", 0 },
    { "nanosecond", "ns", "ns", "ns", "ns", "nanosecond", "nanoseconds", 0 },
    { "bit", "bit", "bit", "bit", "bit", "bit", "bits", 0 },
    { "byte", "byte", "byte", "B", "B", "byte", "bytes", 0 },
    { "kilobit", "kb", "kb", "kb", "kb", "kilobit", "kilobits", 0 },
    { "kilobyte", "kB", "kB", "kB", "kB", "kilobyte", "kilobytes", 0 },
    { "megabit", "Mb", "Mb", "Mb", "Mb", "megabit", "megabits", 0 },
    { "megabyte", "MB", "MB", "MB", "MB", "megabyte", "megabytes", 0 },
    { "gigabit", "Gb", "Gb", "Gb", "Gb", "gigabit", "gigabits", 0 },
    { "gigabyte", "GB", "GB", "GB", "GB", "gigabyte", "gigabytes", 0 },
    { "terabit", "Tb", "Tb", "Tb", "Tb", "terabit", "terabits", 0 },
    { "terabyte", "TB", "TB", "TB", "TB", "terabyte", "terabytes", 0 },
    { "petabyte", "PB", "PB", "PB", "PB", "petabyte", "petabytes", 0 },
    { "millimeter", "mm", "mm", "mm", "mm", "millimeter", "millimeters", 0 },
    { "centimeter", "cm", "cm", "cm", "cm", "centimeter", "centimeters", 0 },
    { "meter", "m", "m", "m", "m", "meter", "meters", 0 },
    { "kilometer", "km", "km", "km", "km", "kilometer", "kilometers", 0 },
    { "inch", "in", "in", "\xE2\x80\xB3", "\xE2\x80\xB3", "inch", "inches", 0 },
    { "foot", "ft", "ft", "\xE2\x80\xB2", "\xE2\x80\xB2", "foot", "feet", 0 },
    { "yard", "yd", "yd", "yd", "yd", "yard", "yards", 0 },
    { "mile", "mi", "mi", "mi", "mi", "mile", "miles", 0 },
    { "gram", "g", "g", "g", "g", "gram", "grams", 0 },
    { "kilogram", "kg", "kg", "kg", "kg", "kilogram", "kilograms", 0 },
    { "ounce", "oz", "oz", "oz", "oz", "ounce", "ounces", 0 },
    { "pound", "lb", "lb", "lb", "lb", "pound", "pounds", 0 },
    { "stone", "st", "st", "st", "st", "stone", "stones", 0 },
    { "liter", "L", "L", "L", "L", "liter", "liters", 0 },
    { "milliliter", "mL", "mL", "mL", "mL", "milliliter", "milliliters", 0 },
    { "gallon", "gal", "gal", "gal", "gal", "gallon", "gallons", 0 },
    { "fluid-ounce", "fl oz", "fl oz", "fl oz", "fl oz", "fluid ounce", "fluid ounces", 0 },
    { "celsius", "\xC2\xB0" "C", "\xC2\xB0" "C", "\xC2\xB0" "C", "\xC2\xB0" "C", "degree Celsius", "degrees Celsius", 1 },
    { "fahrenheit", "\xC2\xB0" "F", "\xC2\xB0" "F", "\xC2\xB0", "\xC2\xB0", "degree Fahrenheit", "degrees Fahrenheit", 1 },
    { "degree", "deg", "deg", "\xC2\xB0", "\xC2\xB0", "degree", "degrees", 0 },
    { "acre", "ac", "ac", "ac", "ac", "acre", "acres", 0 },
    { "hectare", "ha", "ha", "ha", "ha", "hectare", "hectares", 0 },
    { "kilometer-per-hour", "km/h", "km/h", "km/h", "km/h", "kilometer per hour", "kilometers per hour", 0 },
    { "mile-per-hour", "mph", "mph", "mph", "mph", "mile per hour", "miles per hour", 0 },
    { "meter-per-second", "m/s", "m/s", "m/s", "m/s", "meter per second", "meters per second", 0 },
    { "liter-per-kilometer", "L/km", "L/km", "L/km", "L/km", "liter per kilometer", "liters per kilometer", 0 },
    { "mile-per-gallon", "mpg", "mpg", "mpg", "mpg", "mile per gallon", "miles per gallon", 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 },
};

static const intl_unit *intl_unit_of(const char *id) {
    for (int i = 0; INTL_UNITS[i].id; i++) if (intl_same(INTL_UNITS[i].id, id)) return &INTL_UNITS[i];
    return 0;
}

static const char *const NF_STYLE[] = { "decimal", "percent", "currency", "unit", 0 };
static const char *const NF_CURDISP[] = { "symbol", "narrowSymbol", "code", "name", 0 };
static const char *const NF_CURSIGN[] = { "standard", "accounting", 0 };
static const char *const NF_UNITDISP[] = { "short", "narrow", "long", 0 };
static const char *const NF_NOTATION[] = { "standard", "scientific", "engineering", "compact", 0 };
static const char *const NF_COMPACT[] = { "short", "long", 0 };
static const char *const NF_SIGN[] = { "auto", "always", "never", "exceptZero", "negative", 0 };
static const char *const NF_GROUPING[] = { "false", "auto", "always", "min2", 0 };

static int intl_nf_setup(jctx *J, intl_nf *f, jval opts, int dflt_min_frac, int dflt_max_frac) {
    for (u32 i = 0; i < sizeof *f; i++) ((char *)f)[i] = 0;
    f->kind = INTL_NF;
    opts = intl_options(J, opts);
    if (J->sig != JS_OK) return 0;
    f->style = (u8)intl_opt(J, opts, "style", NF_STYLE, NFS_DECIMAL);
    char cur[16];
    if (intl_opt_text(J, opts, "currency", cur, (int)sizeof cur)) {
        if (intl_len(cur) != 3 || !intl_alpha(cur[0]) || !intl_alpha(cur[1]) || !intl_alpha(cur[2])) {
            js_throw(J, JS_ERR_RANGE, "a currency is three letters", J->error_line);
            return 0;
        }
        for (int i = 0; i < 3; i++) f->currency[i] = intl_up(cur[i]);
    }
    if (f->style == NFS_CURRENCY && !f->currency[0]) {
        js_throw(J, JS_ERR_TYPE, "a currency style needs a currency", J->error_line);
        return 0;
    }
    f->cur_display = (u8)intl_opt(J, opts, "currencyDisplay", NF_CURDISP, 0);
    f->accounting = (u8)intl_opt(J, opts, "currencySign", NF_CURSIGN, 0);
    if (intl_opt_text(J, opts, "unit", f->unit, (int)sizeof f->unit) && !intl_unit_of(f->unit)) {
        /* One of the standard's that this has no words for is written by its
           name; anything not shaped like one is refused. */
        for (const char *p = f->unit; *p; p++)
            if (!(intl_alpha(*p) || *p == '-')) { js_throw(J, JS_ERR_RANGE, "that is not a unit", J->error_line); return 0; }
    }
    if (f->style == NFS_UNIT && !f->unit[0]) {
        js_throw(J, JS_ERR_TYPE, "a unit style needs a unit", J->error_line);
        return 0;
    }
    f->unit_display = (u8)intl_opt(J, opts, "unitDisplay", NF_UNITDISP, 0);
    f->notation = (u8)intl_opt(J, opts, "notation", NF_NOTATION, NFN_STANDARD);
    f->compact_long = (u8)intl_opt(J, opts, "compactDisplay", NF_COMPACT, 0);
    f->sign = (u8)intl_opt(J, opts, "signDisplay", NF_SIGN, NFSD_AUTO);

    int cd = 2;
    if (f->style == NFS_CURRENCY) {
        const intl_currency *c = intl_currency_of(f->currency);
        cd = c ? c->digits : 2;
    }
    int mnfd_d = f->style == NFS_CURRENCY ? cd : dflt_min_frac;
    int mxfd_d = f->style == NFS_CURRENCY ? cd : f->style == NFS_PERCENT ? 0 : dflt_max_frac;
    f->min_int = (u8)intl_opt_num(J, opts, "minimumIntegerDigits", 1, 21, 1);
    int mnfd = intl_opt_num(J, opts, "minimumFractionDigits", 0, 100, -1);
    int mxfd = intl_opt_num(J, opts, "maximumFractionDigits", 0, 100, -1);
    int mnsd = intl_opt_num(J, opts, "minimumSignificantDigits", 1, 21, -1);
    int mxsd = intl_opt_num(J, opts, "maximumSignificantDigits", 1, 21, -1);
    if (J->sig != JS_OK) return 0;
    if (mnsd >= 0 || mxsd >= 0) {
        f->use_sig = 1;
        f->min_sig = (u8)(mnsd < 0 ? 1 : mnsd);
        f->max_sig = (u8)(mxsd < 0 ? 21 : mxsd);
        if (f->min_sig > f->max_sig) { js_throw(J, JS_ERR_RANGE, "more significant digits at least than at most", J->error_line); return 0; }
    } else if (mnfd >= 0 || mxfd >= 0) {
        if (mnfd < 0) mnfd = mnfd_d < mxfd ? mnfd_d : mxfd;
        if (mxfd < 0) mxfd = mxfd_d > mnfd ? mxfd_d : mnfd;
        if (mnfd > mxfd) { js_throw(J, JS_ERR_RANGE, "more fraction digits at least than at most", J->error_line); return 0; }
        f->min_frac = (u8)mnfd;
        f->max_frac = (u8)mxfd;
    } else if (f->notation == NFN_COMPACT) {
        f->compact_round = 1;
        f->min_frac = 0;
        f->max_frac = 0;
    } else {
        f->min_frac = (u8)mnfd_d;
        f->max_frac = (u8)mxfd_d;
    }
    /* useGrouping: a boolean, or one of the words; compact writes 1000 as
       1K and groups only from five digits. */
    int dflt_group = f->notation == NFN_COMPACT ? NFG_MIN2 : NFG_AUTO;
    f->grouping = (u8)dflt_group;
    if (js_is_obj(opts)) {
        jval g = js_get_str(J, opts, "useGrouping");
        if (J->sig != JS_OK) return 0;
        if (g.t == JS_BOOL) f->grouping = (u8)(g.b ? NFG_ALWAYS : NFG_OFF);
        else if (g.t == JS_STR) {
            int k = -1;
            for (int i = 0; NF_GROUPING[i]; i++) if (intl_same(g.str->s, NF_GROUPING[i])) k = i;
            if (k < 0) { js_throw(J, JS_ERR_RANGE, "useGrouping takes always, auto, min2 or a boolean", J->error_line); return 0; }
            f->grouping = (u8)(k == 0 ? NFG_OFF : k);
        } else if (g.t != JS_UNDEF) f->grouping = (u8)(js_to_bool(g) ? NFG_ALWAYS : NFG_OFF);
    }
    return J->sig == JS_OK;
}

/* A positive finite number's digits, the first at 10^*first: the shortest
   that read back as it, which is what the browsers round (ICU), so 1.005 to
   two places is 1.01 as a page expects, where the double's exact value,
   1.00499..., would round down. */
static const char *intl_big_digits;   /* a BigInt's digits, while one is formatted */

static int intl_digits(double x, char *dg, int *first) {
    if (intl_big_digits) {
        int n = 0;
        while (intl_big_digits[n] && n < 780) { dg[n] = intl_big_digits[n]; n++; }
        while (n > 1 && dg[n - 1] == '0') n--;
        int len = 0;
        while (intl_big_digits[len]) len++;
        *first = len - 1;
        return n;
    }
    return js_shortest(x, dg, first);
}

/* A non-negative finite number's decimal digits, rounded as f says, after
   dividing by 10^shift: the digits dg[0..*nd) with the first at 10^*first.
   Returns whether the value came to nought. */
static int intl_round(const intl_nf *f, double x, int shift, int frac_digits, int sig_mode, char *dg, int *nd, int *first) {
    if (x == 0) { dg[0] = '0'; *nd = 1; *first = 0; return 1; }
    *nd = intl_digits(x, dg, first);
    *first -= shift;
    int keep = sig_mode ? f->max_sig : *first + 1 + frac_digits;
    if (keep <= 0 && !(keep == 0 && dg[0] >= '5')) { dg[0] = '0'; *nd = 1; *first = 0; return 1; }
    if (js_round_digits(dg, nd, keep)) (*first)++;
    while (*nd > 1 && dg[*nd - 1] == '0') (*nd)--;
    if (*nd == 1 && dg[0] == '0') { *first = 0; return 1; }
    return 0;
}

/* The digits as integer and fraction parts, grouped and padded. */
static void intl_emit_digits(jctx *J, intl_parts *P, const intl_nf *f, const char *dg, int nd, int first,
                             int min_frac, int max_frac_shown, int grouping) {
    char ip[800], fp[240];
    int ni = 0, nf = 0;
    if (first < 0) ip[ni++] = '0';
    else for (int p = first; p >= 0 && ni < 790; p--) { int k = first - p; ip[ni++] = k < nd ? dg[k] : '0'; }
    if (ni < f->min_int) {
        int pad = f->min_int - ni;
        for (int i = ni - 1; i >= 0; i--) ip[i + pad] = ip[i];
        for (int i = 0; i < pad; i++) ip[i] = '0';
        ni += pad;
    }
    for (int p = -1; p >= -max_frac_shown && nf < 230; p--) { int k = first - p; fp[nf++] = (k >= 0 && k < nd) ? dg[k] : '0'; }
    while (nf > min_frac && fp[nf - 1] == '0') nf--;
    int group = grouping == NFG_OFF ? 0 : grouping == NFG_MIN2 ? ni >= 5 : ni >= 4;
    if (!group) ip_add(J, P, "integer", ip, (u32)ni);
    else {
        int lead = ni % 3 ? ni % 3 : 3;
        ip_add(J, P, "integer", ip, (u32)lead);
        for (int i = lead; i < ni; i += 3) {
            ip_add(J, P, "group", ",", 1);
            ip_add(J, P, "integer", ip + i, 3);
        }
    }
    if (nf) {
        ip_add(J, P, "decimal", ".", 1);
        ip_add(J, P, "fraction", fp, (u32)nf);
    }
}

static const char *const INTL_COMPACT_S[5] = { "", "K", "M", "B", "T" };
static const char *const INTL_COMPACT_L[5] = { "", " thousand", " million", " billion", " trillion" };

/* The number part: digits, exponent or compact suffix; *zero says whether
   it came to nought. */
static void intl_number_body(jctx *J, intl_parts *P, const intl_nf *f, double x, int *zero, int *plural_one) {
    char dg[800];
    int nd, first;
    if (f->notation == NFN_SCIENTIFIC || f->notation == NFN_ENGINEERING) {
        int e = 0;
        if (x != 0) {
            int d0;
            char t[800];
            int tn = intl_digits(x, t, &d0);
            (void)tn;
            e = d0;
            if (f->notation == NFN_ENGINEERING) e = (e >= 0 ? e / 3 : -((-e + 2) / 3)) * 3;
        }
        int maxf = f->use_sig ? 0 : f->max_frac, minf = f->use_sig ? 0 : f->min_frac;
        if (!f->use_sig && f->max_frac == 0 && f->min_frac == 0 && f->style != NFS_CURRENCY) maxf = 3;
        *zero = intl_round(f, x, e, maxf, f->use_sig, dg, &nd, &first);
        if (!*zero && first - 0 >= (f->notation == NFN_ENGINEERING ? 3 : 1)) {
            e += f->notation == NFN_ENGINEERING ? 3 : 1;
            *zero = intl_round(f, x, e, maxf, f->use_sig, dg, &nd, &first);
        }
        int shown = f->use_sig ? (nd - 1 - first > 0 ? nd - 1 - first : 0) : maxf;
        if (f->use_sig && nd < f->min_sig) shown = f->min_sig - 1 - first;
        intl_emit_digits(J, P, f, dg, nd, first, f->use_sig ? shown : minf, shown, NFG_OFF);
        ip_add(J, P, "exponentSeparator", "E", 1);
        if (e < 0) ip_add(J, P, "exponentMinusSign", "-", 1);
        char eb[8];
        int w = 0, ev = e < 0 ? -e : e;
        char rev[8];
        int r = 0;
        do { rev[r++] = (char)('0' + ev % 10); ev /= 10; } while (ev);
        while (r) eb[w++] = rev[--r];
        ip_add(J, P, "exponentInteger", eb, (u32)w);
        *plural_one = 0;
        return;
    }
    int shift = 0, k = 0;
    if (f->notation == NFN_COMPACT && x >= 1000) {
        int d0;
        char t[800];
        intl_digits(x, t, &d0);
        k = d0 / 3;
        if (k > 4) k = 4;
        shift = 3 * k;
    }
    for (int pass = 0; pass < 2; pass++) {
        int frac = f->max_frac;
        int sig = f->use_sig;
        if (f->compact_round) {
            /* Two significant digits for a value under ten, none after the
               point otherwise: 1.2K, 12K, 123K. */
            int d0 = 0;
            if (x != 0) { char t[800]; intl_digits(x, t, &d0); }
            frac = (d0 - shift) < 1 ? 1 : 0;
            sig = 0;
        }
        *zero = intl_round(f, x, shift, frac, sig, dg, &nd, &first);
        if (f->notation == NFN_COMPACT && k < 4 && !*zero && first >= 3 && (shift || x >= 1000)) {
            k++;
            shift += 3;
            continue;
        }
        break;
    }
    int shown, minf;
    if (f->use_sig) {
        int total = nd < f->min_sig ? f->min_sig : nd;
        shown = total - 1 - first;
        if (shown < 0) shown = 0;
        minf = shown;
        if (nd < f->min_sig) minf = shown;
        else minf = 0;
    } else {
        shown = f->compact_round ? ((first < 1) ? 1 : 0) : f->max_frac;
        minf = f->min_frac;
    }
    intl_emit_digits(J, P, f, dg, nd, first, minf, shown, f->grouping);
    *plural_one = !*zero && nd == 1 && first == 0 && dg[0] == '1' && minf == 0 && k == 0;
    if (k) ip_adds(J, P, "compact", f->compact_long ? INTL_COMPACT_L[k] : INTL_COMPACT_S[k]);
}

static int intl_symbol_ends_in_letter(const char *s) {
    int n = (int)intl_len(s);
    return n && intl_alpha(s[n - 1]);
}

/* A whole number, with its sign, currency, percent or unit. */
static void intl_format_number(jctx *J, intl_parts *P, const intl_nf *f, double x) {
    int neg = x < 0 || (x == 0 && 1 / x < 0);
    if (x != x) {
        if (f->sign == NFSD_ALWAYS) ip_add(J, P, "plusSign", "+", 1);
        ip_adds(J, P, "nan", "NaN");
        return;
    }
    double ax = neg ? -x : x;
    if (f->style == NFS_PERCENT) ax *= 100;
    intl_parts body;
    body.text.b = 0; body.text.n = 0; body.text.cap = 0; body.text.full = 0;
    body.n = 0;
    int zero = 0, one = 0;
    if (ax - ax != 0) ip_adds(J, &body, "infinity", "\xE2\x88\x9E");
    else intl_number_body(J, &body, f, ax, &zero, &one);

    int show_minus = 0, show_plus = 0;
    switch (f->sign) {
        case NFSD_AUTO: show_minus = neg; break;
        case NFSD_ALWAYS: show_minus = neg; show_plus = !neg; break;
        case NFSD_NEVER: break;
        case NFSD_EXCEPTZERO: show_minus = neg && !zero; show_plus = !neg && !zero; break;
        case NFSD_NEGATIVE: show_minus = neg && !zero; break;
    }
    int paren = f->style == NFS_CURRENCY && f->accounting && show_minus;
    const intl_currency *c = f->style == NFS_CURRENCY ? intl_currency_of(f->currency) : 0;
    char code[4] = { f->currency[0], f->currency[1], f->currency[2], 0 };
    const char *sym = c ? (f->cur_display == 1 ? c->narrow : c->symbol) : code;
    if (f->cur_display == 2) sym = code;

    if (paren) ip_add(J, P, "literal", "(", 1);
    else if (show_minus) ip_add(J, P, "minusSign", "-", 1);
    else if (show_plus) ip_add(J, P, "plusSign", "+", 1);
    if (f->style == NFS_CURRENCY && f->cur_display != 3) {
        ip_adds(J, P, "currency", sym);
        if (intl_symbol_ends_in_letter(sym)) ip_add(J, P, "literal", "\xC2\xA0", 2);
    }
    for (int i = 0; i < body.n; i++) ip_add(J, P, body.p[i].type, body.text.b + body.p[i].at, body.p[i].len);
    free(body.text.b);
    if (f->style == NFS_PERCENT) ip_add(J, P, "percentSign", "%", 1);
    if (f->style == NFS_CURRENCY && f->cur_display == 3) {
        ip_add(J, P, "literal", " ", 1);
        ip_adds(J, P, "currency", c ? (one ? c->one : c->other) : code);
    }
    if (f->style == NFS_UNIT) {
        const intl_unit *u = intl_unit_of(f->unit);
        const char *word = f->unit;
        int tight = 0;
        if (u) {
            if (f->unit_display == 2) word = one ? u->l1 : u->ln;
            else if (f->unit_display == 1) { word = one ? u->n1 : u->nn; tight = 1; }
            else { word = one ? u->s1 : u->sn; tight = u->tight; }
        }
        if (!tight) ip_add(J, P, "literal", " ", 1);
        ip_adds(J, P, "unit", word);
    }
    if (paren) ip_add(J, P, "literal", ")", 1);
}

static jval nat_intl_nf_ctor(jctx *J, jval t, jval *a, int n) {
    intl_requested(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    intl_nf f;
    if (!intl_nf_setup(J, &f, js_arg(a, n, 1), 0, 3)) return js_undef();
    jobj *o = js_is_obj(t) && J->new_target.t != JS_UNDEF ? t.obj : 0;
    if (!o) {
        jval pv = js_get(J, js_from_obj(J->callee), J->s_prototype);
        o = js_object_with(J, JO_PLAIN, js_is_obj(pv) ? pv.obj : J->p_object);
        if (!o) return js_undef();
    }
    intl_store(J, o, &f, (u32)sizeof f);
    return js_from_obj(o);
}

/* A number, or its text read as one, as format takes it. A BigInt is
   written from its digits, which formatting then reads (intl_digits): *big
   holds them, or is 0. */
static double intl_arg_num(jctx *J, jval v, jstr **big) {
    if (big) *big = 0;
    if (v.t == JS_BIG) {
        jstr *s = jsb_to_str(J, v.big, 10);
        if (big && s) *big = s;
        /* Its size, for the choices made by size (compact's K and M). */
        double d = jsb_to_double(J, v.big);
        if (d - d != 0) d = d < 0 ? -1e308 : 1e308;
        return d;
    }
    if (v.t == JS_STR) return js_str_to_num(v.str->s, v.str->len);
    return js_to_num(J, v);
}

/* Formats x, or the BigInt whose text is big. */
static void intl_format_any(jctx *J, intl_parts *P, const intl_nf *f, double x, jstr *big) {
    char *buf = 0;
    if (big) {
        const char *d = big->s[0] == '-' ? big->s + 1 : big->s;
        if (d[0] == '0') x = 0;
        else if (f->style == NFS_PERCENT) {
            /* A percent is a hundred times the number: two more digits. */
            u32 n = 0;
            while (d[n]) n++;
            buf = (char *)malloc(n + 3);
            if (buf) {
                for (u32 i = 0; i < n; i++) buf[i] = d[i];
                buf[n] = buf[n + 1] = '0';
                buf[n + 2] = 0;
                intl_big_digits = buf;
            }
        } else intl_big_digits = d;
    }
    intl_format_number(J, P, f, x);
    intl_big_digits = 0;
    free(buf);
}

static jval nat_intl_nf_format_fn(jctx *J, jval t, jval *a, int n) {
    (void)t;
    intl_nf f;
    if (!intl_load(J, J->callee->data, INTL_NF, &f, (u32)sizeof f)) return js_undef();
    jstr *big;
    double x = intl_arg_num(J, js_arg(a, n, 0), &big);
    if (J->sig != JS_OK) return js_undef();
    intl_parts *P = (intl_parts *)malloc(sizeof(intl_parts));
    if (!P) return js_undef();
    P->text.b = 0; P->text.n = 0; P->text.cap = 0; P->text.full = 0;
    P->n = 0;
    intl_format_any(J, P, &f, x, big);
    jval r = ip_string(J, P);
    free(P);
    return r;
}

static jval nat_intl_nf_format(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_nf f;
    if (!intl_load(J, t, INTL_NF, &f, (u32)sizeof f)) return js_undef();
    return intl_bound(J, t, "*format*", nat_intl_nf_format_fn, 1);
}

static jval nat_intl_nf_parts(jctx *J, jval t, jval *a, int n) {
    intl_nf f;
    if (!intl_load(J, t, INTL_NF, &f, (u32)sizeof f)) return js_undef();
    jstr *big;
    double x = intl_arg_num(J, js_arg(a, n, 0), &big);
    if (J->sig != JS_OK) return js_undef();
    intl_parts *P = (intl_parts *)malloc(sizeof(intl_parts));
    if (!P) return js_undef();
    P->text.b = 0; P->text.n = 0; P->text.cap = 0; P->text.full = 0;
    P->n = 0;
    intl_format_any(J, P, &f, x, big);
    jval r = ip_array(J, P, 0, js_undef());
    free(P);
    return r;
}

static jval nat_intl_nf_range(jctx *J, jval t, jval *a, int n) {
    intl_nf f;
    if (!intl_load(J, t, INTL_NF, &f, (u32)sizeof f)) return js_undef();
    double x = intl_arg_num(J, js_arg(a, n, 0), 0), y = intl_arg_num(J, js_arg(a, n, 1), 0);
    if (J->sig != JS_OK) return js_undef();
    if (x != x || y != y) return js_throw(J, JS_ERR_RANGE, "a range of numbers needs numbers", J->error_line);
    intl_parts *P = (intl_parts *)malloc(sizeof(intl_parts));
    if (!P) return js_undef();
    P->text.b = 0; P->text.n = 0; P->text.cap = 0; P->text.full = 0;
    P->n = 0;
    intl_format_number(J, P, &f, x);
    u32 first_len = P->text.n;
    ip_add(J, P, "literal", "\xE2\x80\x93", 3);
    intl_format_number(J, P, &f, y);
    /* The same both ends: the one, with an approximately sign. */
    if (P->text.n - first_len - 3 == first_len) {
        int same = 1;
        for (u32 i = 0; i < first_len; i++) if (P->text.b[i] != P->text.b[first_len + 3 + i]) same = 0;
        if (same) {
            jtext q = { 0, 0, 0, 0 };
            jt_put(J, &q, "~", 1);
            jt_put(J, &q, P->text.b, first_len);
            free(P->text.b);
            free(P);
            return js_from_str(jt_done(J, &q));
        }
    }
    jval r = ip_string(J, P);
    free(P);
    return r;
}

static jval nat_intl_nf_resolved(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_nf f;
    if (!intl_load(J, t, INTL_NF, &f, (u32)sizeof f)) return js_undef();
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return js_undef();
    intl_set_str(J, o, "locale", "en-US");
    intl_set_str(J, o, "numberingSystem", "latn");
    intl_set_str(J, o, "style", NF_STYLE[f.style]);
    if (f.style == NFS_CURRENCY) {
        char code[4] = { f.currency[0], f.currency[1], f.currency[2], 0 };
        intl_set_str(J, o, "currency", code);
        intl_set_str(J, o, "currencyDisplay", NF_CURDISP[f.cur_display]);
        intl_set_str(J, o, "currencySign", NF_CURSIGN[f.accounting]);
    }
    if (f.style == NFS_UNIT) {
        intl_set_str(J, o, "unit", f.unit);
        intl_set_str(J, o, "unitDisplay", NF_UNITDISP[f.unit_display]);
    }
    intl_set(J, o, "minimumIntegerDigits", js_num(f.min_int));
    if (f.use_sig) {
        intl_set(J, o, "minimumSignificantDigits", js_num(f.min_sig));
        intl_set(J, o, "maximumSignificantDigits", js_num(f.max_sig));
    } else {
        intl_set(J, o, "minimumFractionDigits", js_num(f.min_frac));
        intl_set(J, o, "maximumFractionDigits", js_num(f.compact_round ? 0 : f.max_frac));
    }
    if (f.grouping == NFG_OFF) intl_set(J, o, "useGrouping", js_bool(0));
    else intl_set_str(J, o, "useGrouping", NF_GROUPING[f.grouping]);
    intl_set_str(J, o, "notation", NF_NOTATION[f.notation]);
    if (f.notation == NFN_COMPACT) intl_set_str(J, o, "compactDisplay", NF_COMPACT[f.compact_long]);
    intl_set_str(J, o, "signDisplay", NF_SIGN[f.sign]);
    intl_set_str(J, o, "roundingMode", "halfExpand");
    intl_set_str(J, o, "roundingPriority", f.compact_round ? "morePrecision" : "auto");
    intl_set_str(J, o, "trailingZeroDisplay", "auto");
    return js_from_obj(o);
}

/* BigInt.prototype.toLocaleString. */
static jval intl_bigint_text(jctx *J, jval v, jval locales, jval opts) {
    intl_requested(J, locales);
    if (J->sig != JS_OK) return js_undef();
    intl_nf f;
    if (!intl_nf_setup(J, &f, opts, 0, 3)) return js_undef();
    jstr *big;
    double x = intl_arg_num(J, v, &big);
    intl_parts *P = (intl_parts *)malloc(sizeof(intl_parts));
    if (!P) return js_undef();
    P->text.b = 0; P->text.n = 0; P->text.cap = 0; P->text.full = 0;
    P->n = 0;
    intl_format_any(J, P, &f, x, big);
    jval r = ip_string(J, P);
    free(P);
    return r;
}

/* Number.prototype.toLocaleString, through a NumberFormat of its options. */
static jval intl_number_text(jctx *J, double x, jval locales, jval opts) {
    intl_requested(J, locales);
    if (J->sig != JS_OK) return js_undef();
    intl_nf f;
    if (!intl_nf_setup(J, &f, opts, 0, 3)) return js_undef();
    intl_parts *P = (intl_parts *)malloc(sizeof(intl_parts));
    if (!P) return js_undef();
    P->text.b = 0; P->text.n = 0; P->text.cap = 0; P->text.full = 0;
    P->n = 0;
    intl_format_number(J, P, &f, x);
    jval r = ip_string(J, P);
    free(P);
    return r;
}

/* --- time zones -------------------------------------------------------------------------- */

enum { TZR_NONE, TZR_US, TZR_EU, TZR_AU, TZR_NZ };

typedef struct {
    const char *name;
    int offset;                 /* minutes east of UTC, outside summer */
    int rule;
    const char *s_std, *s_dst;  /* short names, or 0 for GMT+N */
    const char *l_std, *l_dst;  /* long names, or 0 for GMT+NN:NN */
} intl_zone;

static const intl_zone INTL_ZONES[] = {
    { "UTC", 0, TZR_NONE, "UTC", "UTC", "Coordinated Universal Time", "Coordinated Universal Time" },
    { "America/New_York", -300, TZR_US, "EST", "EDT", "Eastern Standard Time", "Eastern Daylight Time" },
    { "America/Detroit", -300, TZR_US, "EST", "EDT", "Eastern Standard Time", "Eastern Daylight Time" },
    { "America/Toronto", -300, TZR_US, "EST", "EDT", "Eastern Standard Time", "Eastern Daylight Time" },
    { "America/Chicago", -360, TZR_US, "CST", "CDT", "Central Standard Time", "Central Daylight Time" },
    { "America/Winnipeg", -360, TZR_US, "CST", "CDT", "Central Standard Time", "Central Daylight Time" },
    { "America/Denver", -420, TZR_US, "MST", "MDT", "Mountain Standard Time", "Mountain Daylight Time" },
    { "America/Edmonton", -420, TZR_US, "MST", "MDT", "Mountain Standard Time", "Mountain Daylight Time" },
    { "America/Phoenix", -420, TZR_NONE, "MST", "MST", "Mountain Standard Time", "Mountain Standard Time" },
    { "America/Los_Angeles", -480, TZR_US, "PST", "PDT", "Pacific Standard Time", "Pacific Daylight Time" },
    { "America/Vancouver", -480, TZR_US, "PST", "PDT", "Pacific Standard Time", "Pacific Daylight Time" },
    { "America/Anchorage", -540, TZR_US, "AKST", "AKDT", "Alaska Standard Time", "Alaska Daylight Time" },
    { "Pacific/Honolulu", -600, TZR_NONE, "HST", "HST", "Hawaii-Aleutian Standard Time", "Hawaii-Aleutian Standard Time" },
    { "America/Halifax", -240, TZR_US, "AST", "ADT", "Atlantic Standard Time", "Atlantic Daylight Time" },
    { "America/Mexico_City", -360, TZR_NONE, "CST", "CST", "Central Standard Time", "Central Standard Time" },
    { "America/Bogota", -300, TZR_NONE, 0, 0, "Colombia Standard Time", "Colombia Standard Time" },
    { "America/Lima", -300, TZR_NONE, 0, 0, "Peru Standard Time", "Peru Standard Time" },
    { "America/Sao_Paulo", -180, TZR_NONE, 0, 0, "Brasilia Standard Time", "Brasilia Standard Time" },
    { "America/Argentina/Buenos_Aires", -180, TZR_NONE, 0, 0, "Argentina Standard Time", "Argentina Standard Time" },
    { "Europe/London", 0, TZR_EU, "GMT", 0, "Greenwich Mean Time", "British Summer Time" },
    { "Europe/Dublin", 0, TZR_EU, "GMT", 0, "Greenwich Mean Time", "Irish Standard Time" },
    { "Europe/Lisbon", 0, TZR_EU, 0, 0, "Western European Standard Time", "Western European Summer Time" },
    { "Europe/Paris", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Berlin", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Madrid", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Rome", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Amsterdam", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Brussels", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Vienna", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Zurich", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Stockholm", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Oslo", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Copenhagen", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Warsaw", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Prague", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Budapest", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Athens", 120, TZR_EU, 0, 0, "Eastern European Standard Time", "Eastern European Summer Time" },
    { "Europe/Helsinki", 120, TZR_EU, 0, 0, "Eastern European Standard Time", "Eastern European Summer Time" },
    { "Europe/Kyiv", 120, TZR_EU, 0, 0, "Eastern European Standard Time", "Eastern European Summer Time" },
    { "Europe/Kiev", 120, TZR_EU, 0, 0, "Eastern European Standard Time", "Eastern European Summer Time" },
    { "Europe/Bucharest", 120, TZR_EU, 0, 0, "Eastern European Standard Time", "Eastern European Summer Time" },
    { "Europe/Istanbul", 180, TZR_NONE, 0, 0, 0, 0 },
    { "Europe/Moscow", 180, TZR_NONE, 0, 0, "Moscow Standard Time", "Moscow Standard Time" },
    { "Africa/Lagos", 60, TZR_NONE, 0, 0, "West Africa Standard Time", "West Africa Standard Time" },
    { "Africa/Johannesburg", 120, TZR_NONE, 0, 0, "South Africa Standard Time", "South Africa Standard Time" },
    { "Africa/Nairobi", 180, TZR_NONE, 0, 0, "East Africa Time", "East Africa Time" },
    { "Asia/Dubai", 240, TZR_NONE, 0, 0, "Gulf Standard Time", "Gulf Standard Time" },
    { "Asia/Karachi", 300, TZR_NONE, 0, 0, "Pakistan Standard Time", "Pakistan Standard Time" },
    { "Asia/Kolkata", 330, TZR_NONE, 0, 0, "India Standard Time", "India Standard Time" },
    { "Asia/Calcutta", 330, TZR_NONE, 0, 0, "India Standard Time", "India Standard Time" },
    { "Asia/Kathmandu", 345, TZR_NONE, 0, 0, "Nepal Time", "Nepal Time" },
    { "Asia/Dhaka", 360, TZR_NONE, 0, 0, "Bangladesh Standard Time", "Bangladesh Standard Time" },
    { "Asia/Bangkok", 420, TZR_NONE, 0, 0, "Indochina Time", "Indochina Time" },
    { "Asia/Jakarta", 420, TZR_NONE, 0, 0, "Western Indonesia Time", "Western Indonesia Time" },
    { "Asia/Ho_Chi_Minh", 420, TZR_NONE, 0, 0, "Indochina Time", "Indochina Time" },
    { "Asia/Shanghai", 480, TZR_NONE, 0, 0, "China Standard Time", "China Standard Time" },
    { "Asia/Hong_Kong", 480, TZR_NONE, 0, 0, "Hong Kong Standard Time", "Hong Kong Standard Time" },
    { "Asia/Singapore", 480, TZR_NONE, 0, 0, "Singapore Standard Time", "Singapore Standard Time" },
    { "Asia/Taipei", 480, TZR_NONE, 0, 0, "Taipei Standard Time", "Taipei Standard Time" },
    { "Asia/Manila", 480, TZR_NONE, 0, 0, "Philippine Standard Time", "Philippine Standard Time" },
    { "Australia/Perth", 480, TZR_NONE, 0, 0, "Australian Western Standard Time", "Australian Western Standard Time" },
    { "Asia/Seoul", 540, TZR_NONE, 0, 0, "Korean Standard Time", "Korean Standard Time" },
    { "Asia/Tokyo", 540, TZR_NONE, 0, 0, "Japan Standard Time", "Japan Standard Time" },
    { "Australia/Darwin", 570, TZR_NONE, 0, 0, "Australian Central Standard Time", "Australian Central Standard Time" },
    { "Australia/Adelaide", 570, TZR_AU, 0, 0, "Australian Central Standard Time", "Australian Central Daylight Time" },
    { "Australia/Brisbane", 600, TZR_NONE, 0, 0, "Australian Eastern Standard Time", "Australian Eastern Standard Time" },
    { "Australia/Sydney", 600, TZR_AU, 0, 0, "Australian Eastern Standard Time", "Australian Eastern Daylight Time" },
    { "Australia/Melbourne", 600, TZR_AU, 0, 0, "Australian Eastern Standard Time", "Australian Eastern Daylight Time" },
    { "Australia/Canberra", 600, TZR_AU, 0, 0, "Australian Eastern Standard Time", "Australian Eastern Daylight Time" },
    { "Australia/Hobart", 600, TZR_AU, 0, 0, "Australian Eastern Standard Time", "Australian Eastern Daylight Time" },
    { "Pacific/Auckland", 720, TZR_NZ, 0, 0, "New Zealand Standard Time", "New Zealand Daylight Time" },
    { 0, 0, 0, 0, 0, 0, 0 },
};

/* The UTC time of the nth (1-based; 0 the last) Sunday of a month, at
   `hour` in a clock `off` minutes east of UTC. */
static double intl_sunday(int year, int month, int nth, int hour, int off) {
    double first = js_make_time(year, month, 1, 0, 0, 0, 0);
    jdate p;
    js_date_parts(first, &p);
    int day = 1 + (7 - p.wd) % 7;
    if (nth > 0) day += 7 * (nth - 1);
    else {
        static const int DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int len = DAYS[month] + (month == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0));
        while (day + 7 <= len) day += 7;
    }
    return js_make_time(year, month, day, hour, 0, 0, 0) - off * 60000.0;
}

static int intl_in_summer(const intl_zone *z, double t) {
    if (z->rule == TZR_NONE) return 0;
    jdate p;
    js_date_parts(t, &p);
    int y = p.y, o = z->offset;
    switch (z->rule) {
        case TZR_US: return t >= intl_sunday(y, 2, 2, 2, o) && t < intl_sunday(y, 10, 1, 1, o);
        case TZR_EU: return t >= intl_sunday(y, 2, 0, 1, 0) && t < intl_sunday(y, 9, 0, 1, 0);
        case TZR_AU: return t < intl_sunday(y, 3, 1, 2, o) || t >= intl_sunday(y, 9, 1, 2, o);
        case TZR_NZ: return t < intl_sunday(y, 3, 1, 2, o) || t >= intl_sunday(y, 8, 0, 2, o);
    }
    return 0;
}

static int intl_zone_offset(const intl_zone *z, double t) { return z->offset + (intl_in_summer(z, t) ? 60 : 0); }

/* A zone by name, as IANA writes it or in any case; -1 when it is none
   this knows. UTC's other names are UTC. */
static int intl_zone_find(const char *name) {
    static const char *const UTCS[] = { "utc", "etc/utc", "gmt", "etc/gmt", "etc/universal", "universal", "zulu", "etc/zulu", 0 };
    char low[64];
    int k = 0;
    for (; name[k] && k < 63; k++) low[k] = intl_low(name[k]);
    low[k] = 0;
    for (int i = 0; UTCS[i]; i++) if (intl_same(low, UTCS[i])) return 0;
    for (int i = 0; INTL_ZONES[i].name; i++) {
        const char *z = INTL_ZONES[i].name;
        int j = 0;
        while (z[j] && low[j] && intl_low(z[j]) == low[j]) j++;
        if (!z[j] && !low[j]) return i;
    }
    return -1;
}

/* GMT+1, GMT-5:30; long: GMT+01:00. */
static void intl_gmt_text(char *out, int off, int longform) {
    int w = 0;
    const char *g = "GMT";
    for (int i = 0; g[i]; i++) out[w++] = g[i];
    if (off == 0 && !longform) { out[w] = 0; return; }
    out[w++] = off < 0 ? '-' : '+';
    int a = off < 0 ? -off : off, h = a / 60, m = a % 60;
    if (longform || h >= 10) { if (longform && h < 10) out[w++] = '0'; if (h >= 10) out[w++] = (char)('0' + h / 10); out[w++] = (char)('0' + h % 10); }
    else out[w++] = (char)('0' + h);
    if (m || longform) {
        out[w++] = ':';
        out[w++] = (char)('0' + m / 10);
        out[w++] = (char)('0' + m % 10);
    }
    out[w] = 0;
}

/* --- dates ------------------------------------------------------------------------------------ */

enum { DC_NONE, DC_NUMERIC, DC_2DIGIT, DC_LONG, DC_SHORT, DC_NARROW };
enum { DS_NONE, DS_FULL, DS_LONG, DS_MEDIUM, DS_SHORT };

typedef struct {
    u32 kind;
    u8 weekday, era, year, month, day, hour, minute, second, fsd, tzname, hour12, date_style, time_style, pad;
    int zone;                       /* into INTL_ZONES */
} intl_dtf;

static const char *const DT_TEXT3[] = { "long", "short", "narrow", 0 };
static const char *const DT_NUM2[] = { "numeric", "2-digit", 0 };
static const char *const DT_MONTH[] = { "numeric", "2-digit", "long", "short", "narrow", 0 };
static const char *const DT_STYLE[] = { "full", "long", "medium", "short", 0 };
static const char *const DT_TZNAME[] = { "short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric", 0 };
static const char *const DT_CYCLE[] = { "h11", "h12", "h23", "h24", 0 };

static const char *const INTL_MONTHS_L[12] = { "January", "February", "March", "April", "May", "June", "July",
                                               "August", "September", "October", "November", "December" };
static const char *const INTL_DAYS_L[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

/* required and defaults as the standard's ToDateTimeOptions names them:
   0 any/date (the constructor and toLocaleDateString), 1 time, 2 all. */
static int intl_dtf_setup(jctx *J, intl_dtf *f, jval opts, int required, int defaults) {
    for (u32 i = 0; i < sizeof *f; i++) ((char *)f)[i] = 0;
    f->kind = INTL_DTF;
    opts = intl_options(J, opts);
    if (J->sig != JS_OK) return 0;
    int h12 = intl_opt_bool(J, opts, "hour12");
    int cycle = intl_opt(J, opts, "hourCycle", DT_CYCLE, -1);
    char tz[64];
    f->zone = 0;
    if (intl_opt_text(J, opts, "timeZone", tz, (int)sizeof tz)) {
        int z = intl_zone_find(tz);
        /* An offset written as +HH:MM is a zone of its own, which this
           does not keep: it is written in UTC, and says so. */
        if (z < 0) {
            int area = 0;
            for (const char *p = tz; *p; p++) if (*p == '/') area = 1;
            if (!area && !(tz[0] == '+' || tz[0] == '-')) {
                js_throw(J, JS_ERR_RANGE, "that is not a time zone", J->error_line);
                return 0;
            }
            z = 0;
        }
        f->zone = z;
    }
    f->weekday = (u8)(intl_opt(J, opts, "weekday", DT_TEXT3, -1) + 3);
    f->era = (u8)(intl_opt(J, opts, "era", DT_TEXT3, -1) + 3);
    f->year = (u8)(intl_opt(J, opts, "year", DT_NUM2, -1) + 1);
    f->month = (u8)(intl_opt(J, opts, "month", DT_MONTH, -1) + 1);
    f->day = (u8)(intl_opt(J, opts, "day", DT_NUM2, -1) + 1);
    f->hour = (u8)(intl_opt(J, opts, "hour", DT_NUM2, -1) + 1);
    f->minute = (u8)(intl_opt(J, opts, "minute", DT_NUM2, -1) + 1);
    f->second = (u8)(intl_opt(J, opts, "second", DT_NUM2, -1) + 1);
    f->fsd = (u8)intl_opt_num(J, opts, "fractionalSecondDigits", 1, 3, 0);
    f->tzname = (u8)(intl_opt(J, opts, "timeZoneName", DT_TZNAME, -1) + 1);
    f->date_style = (u8)(intl_opt(J, opts, "dateStyle", DT_STYLE, -1) + 1);
    f->time_style = (u8)(intl_opt(J, opts, "timeStyle", DT_STYLE, -1) + 1);
    if (J->sig != JS_OK) return 0;
    if (f->weekday == 2) f->weekday = 0;
    if (f->era == 2) f->era = 0;
    /* -1 + 3 is 2, which no text style is: nothing was asked. */
    int any_date = f->weekday || f->year || f->month || f->day;
    int any_time = f->hour || f->minute || f->second || f->fsd;
    if ((f->date_style || f->time_style) && (any_date || any_time || f->era || f->tzname)) {
        js_throw(J, JS_ERR_TYPE, "dateStyle and timeStyle cannot be used with the parts", J->error_line);
        return 0;
    }
    if (!f->date_style && !f->time_style) {
        int need_date = 1, need_time = 1;
        if (required == 0) need_date = need_time = !(any_date || any_time);
        if (required == 1) { need_date = 0; need_time = !(any_time || any_date); }
        if (required == 2) need_date = need_time = !(any_date || any_time);
        if (defaults == 0 && need_date && need_time) need_time = 0;
        if (defaults == 1 && need_date && need_time) need_date = 0;
        if (need_date) f->year = f->month = f->day = DC_NUMERIC;
        if (need_time) f->hour = f->minute = f->second = DC_NUMERIC;
        (void)required;
    }
    f->hour12 = 1;
    if (cycle >= 2) f->hour12 = 0;
    if (cycle >= 0 && cycle < 2) f->hour12 = 1;
    if (h12 >= 0) f->hour12 = (u8)h12;
    return 1;
}

static void intl_num_into(char *out, int *w, int v, int width) {
    char rev[12];
    int r = 0;
    if (v < 0) { out[(*w)++] = '-'; v = -v; }
    do { rev[r++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (r < width) rev[r++] = '0';
    while (r) out[(*w)++] = rev[--r];
}

static void ip_num(jctx *J, intl_parts *P, const char *type, int v, int width) {
    char b[16];
    int w = 0;
    intl_num_into(b, &w, v, width);
    ip_add(J, P, type, b, (u32)w);
}

/* The date part of a pattern, English order. */
static void intl_date_part(jctx *J, intl_parts *P, const intl_dtf *f, const jdate *d, int wd, int month, int day, int year) {
    char nb[8];
    if (wd) {
        const char *name = INTL_DAYS_L[d->wd];
        if (wd == DC_LONG) ip_adds(J, P, "weekday", name);
        else ip_add(J, P, "weekday", name, wd == DC_NARROW ? 1 : 3);
        if (month || day || year) ip_add(J, P, "literal", ", ", 2);
    }
    int y = d->y, era_bc = y <= 0;
    if (era_bc) y = 1 - y;
    if (month >= DC_LONG) {
        const char *name = INTL_MONTHS_L[d->mon];
        if (month == DC_LONG) ip_adds(J, P, "month", name);
        else ip_add(J, P, "month", name, month == DC_NARROW ? 1 : 3);
        if (day) {
            ip_add(J, P, "literal", " ", 1);
            ip_num(J, P, "day", d->d, day == DC_2DIGIT ? 2 : 1);
        }
        if (year) {
            ip_add(J, P, "literal", day ? ", " : " ", day ? 2 : 1);
            ip_num(J, P, "year", year == DC_2DIGIT ? y % 100 : y, year == DC_2DIGIT ? 2 : 1);
        }
    } else if (month) {
        ip_num(J, P, "month", d->mon + 1, month == DC_2DIGIT ? 2 : 1);
        if (day) { ip_add(J, P, "literal", "/", 1); ip_num(J, P, "day", d->d, day == DC_2DIGIT ? 2 : 1); }
        if (year) { ip_add(J, P, "literal", "/", 1); ip_num(J, P, "year", year == DC_2DIGIT ? y % 100 : y, year == DC_2DIGIT ? 2 : 1); }
    } else {
        if (day) ip_num(J, P, "day", d->d, day == DC_2DIGIT ? 2 : 1);
        if (year) {
            if (day) ip_add(J, P, "literal", " ", 1);
            ip_num(J, P, "year", year == DC_2DIGIT ? y % 100 : y, year == DC_2DIGIT ? 2 : 1);
        }
    }
    if (f->era) {
        ip_add(J, P, "literal", " ", 1);
        ip_adds(J, P, "era", f->era == DC_NARROW ? (era_bc ? "B" : "A") : f->era == DC_LONG ? (era_bc ? "Before Christ" : "Anno Domini")
                                                                : (era_bc ? "BC" : "AD"));
    }
    (void)nb;
}

static void intl_zone_name(jctx *J, intl_parts *P, const intl_dtf *f, int style, double t) {
    const intl_zone *z = &INTL_ZONES[f->zone];
    int summer = intl_in_summer(z, t), off = intl_zone_offset(z, t);
    char buf[24];
    const char *name = 0;
    if (style == 1 || style == 5) name = summer ? z->s_dst : z->s_std;
    if (style == 2 || style == 6) name = summer ? z->l_dst : z->l_std;
    if (!name) {
        intl_gmt_text(buf, off, style == 4 || style == 2 || style == 6);
        name = buf;
    }
    ip_adds(J, P, "timeZoneName", name);
}

static void intl_time_part(jctx *J, intl_parts *P, const intl_dtf *f, const jdate *d, int ms, int hour, int minute, int second, int fsd,
                           int tzname, double t) {
    int h = d->h;
    if (hour) {
        if (f->hour12) {
            int h12 = h % 12 ? h % 12 : 12;
            ip_num(J, P, "hour", h12, hour == DC_2DIGIT ? 2 : 1);
        } else ip_num(J, P, "hour", h, 2);
    }
    if (minute) {
        if (hour) ip_add(J, P, "literal", ":", 1);
        ip_num(J, P, "minute", d->mi, hour || minute == DC_2DIGIT ? 2 : 1);
    }
    if (second) {
        if (hour || minute) ip_add(J, P, "literal", ":", 1);
        ip_num(J, P, "second", d->s, hour || minute || second == DC_2DIGIT ? 2 : 1);
    }
    if (fsd) {
        ip_add(J, P, "literal", ".", 1);
        char b[4];
        int v = ms, w = 0;
        b[w++] = (char)('0' + v / 100);
        if (fsd > 1) b[w++] = (char)('0' + v / 10 % 10);
        if (fsd > 2) b[w++] = (char)('0' + v % 10);
        ip_add(J, P, "fractionalSecond", b, (u32)w);
    }
    if (hour && f->hour12) {
        ip_add(J, P, "literal", " ", 1);
        ip_adds(J, P, "dayPeriod", h < 12 ? "AM" : "PM");
    }
    if (tzname) {
        ip_add(J, P, "literal", " ", 1);
        intl_zone_name(J, P, f, tzname, t);
    }
}

static int intl_format_date(jctx *J, intl_parts *P, const intl_dtf *f, double t) {
    if (t != t) { js_throw(J, JS_ERR_RANGE, "an invalid date cannot be written", J->error_line); return 0; }
    const intl_zone *z = &INTL_ZONES[f->zone];
    double local = t + intl_zone_offset(z, t) * 60000.0;
    jdate d;
    js_date_parts(local, &d);
    int ms = d.ms;
    if (f->date_style || f->time_style) {
        if (f->date_style) {
            switch (f->date_style) {
                case DS_FULL: intl_date_part(J, P, f, &d, DC_LONG, DC_LONG, DC_NUMERIC, DC_NUMERIC); break;
                case DS_LONG: intl_date_part(J, P, f, &d, 0, DC_LONG, DC_NUMERIC, DC_NUMERIC); break;
                case DS_MEDIUM: intl_date_part(J, P, f, &d, 0, DC_SHORT, DC_NUMERIC, DC_NUMERIC); break;
                default: intl_date_part(J, P, f, &d, 0, DC_NUMERIC, DC_NUMERIC, DC_2DIGIT); break;
            }
        }
        if (f->date_style && f->time_style) {
            if (f->date_style <= DS_LONG) ip_add(J, P, "literal", " at ", 4);
            else ip_add(J, P, "literal", ", ", 2);
        }
        if (f->time_style) {
            int tz = f->time_style == DS_FULL ? 2 : f->time_style == DS_LONG ? 1 : 0;
            intl_time_part(J, P, f, &d, ms, DC_NUMERIC, DC_2DIGIT, f->time_style <= DS_MEDIUM ? DC_2DIGIT : 0, 0, tz, t);
        }
        return 1;
    }
    int any_date = f->weekday || f->year || f->month || f->day;
    int any_time = f->hour || f->minute || f->second || f->fsd;
    if (any_date) intl_date_part(J, P, f, &d, f->weekday, f->month, f->day, f->year);
    if (any_date && (any_time || f->tzname)) ip_add(J, P, "literal", ", ", 2);
    if (any_time || f->tzname) {
        if (!any_time && f->tzname) { intl_zone_name(J, P, f, f->tzname, t); return 1; }
        intl_time_part(J, P, f, &d, ms, f->hour, f->minute, f->second, f->fsd, f->tzname, t);
    }
    return 1;
}

/* A date's time value: a Date, a number, or now for nothing. */
static double intl_arg_time(jctx *J, jval v) {
    if (v.t == JS_UNDEF) return js_now(J);
    if (js_is_obj(v) && v.obj->kind == JO_DATE) return v.obj->ival.num;
    return js_time_clip(js_to_num(J, v));
}

static jval nat_intl_dtf_ctor(jctx *J, jval t, jval *a, int n) {
    intl_requested(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    intl_dtf f;
    if (!intl_dtf_setup(J, &f, js_arg(a, n, 1), 0, 0)) return js_undef();
    jobj *o = js_is_obj(t) && J->new_target.t != JS_UNDEF ? t.obj : 0;
    if (!o) {
        jval pv = js_get(J, js_from_obj(J->callee), J->s_prototype);
        o = js_object_with(J, JO_PLAIN, js_is_obj(pv) ? pv.obj : J->p_object);
        if (!o) return js_undef();
    }
    intl_store(J, o, &f, (u32)sizeof f);
    return js_from_obj(o);
}

static jval intl_date_string(jctx *J, const intl_dtf *f, double t) {
    intl_parts *P = (intl_parts *)malloc(sizeof(intl_parts));
    if (!P) return js_undef();
    P->text.b = 0; P->text.n = 0; P->text.cap = 0; P->text.full = 0;
    P->n = 0;
    jval r = js_undef();
    if (intl_format_date(J, P, f, t)) r = ip_string(J, P);
    else free(P->text.b);
    free(P);
    return r;
}

static jval nat_intl_dtf_format_fn(jctx *J, jval t, jval *a, int n) {
    (void)t;
    intl_dtf f;
    if (!intl_load(J, J->callee->data, INTL_DTF, &f, (u32)sizeof f)) return js_undef();
    double tv = intl_arg_time(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    return intl_date_string(J, &f, tv);
}

static jval nat_intl_dtf_format(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_dtf f;
    if (!intl_load(J, t, INTL_DTF, &f, (u32)sizeof f)) return js_undef();
    return intl_bound(J, t, "*format*", nat_intl_dtf_format_fn, 1);
}

static jval nat_intl_dtf_parts(jctx *J, jval t, jval *a, int n) {
    intl_dtf f;
    if (!intl_load(J, t, INTL_DTF, &f, (u32)sizeof f)) return js_undef();
    double tv = intl_arg_time(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    intl_parts *P = (intl_parts *)malloc(sizeof(intl_parts));
    if (!P) return js_undef();
    P->text.b = 0; P->text.n = 0; P->text.cap = 0; P->text.full = 0;
    P->n = 0;
    jval r = js_undef();
    if (intl_format_date(J, P, &f, tv)) r = ip_array(J, P, 0, js_undef());
    else free(P->text.b);
    free(P);
    return r;
}

/* formatRange: one when both ends write the same, else both with an en
   dash between thin spaces. */
static jval nat_intl_dtf_range(jctx *J, jval t, jval *a, int n) {
    intl_dtf f;
    if (!intl_load(J, t, INTL_DTF, &f, (u32)sizeof f)) return js_undef();
    if (n < 2 || a[0].t == JS_UNDEF || a[1].t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "formatRange needs two dates", J->error_line);
    double x = intl_arg_time(J, a[0]), y = intl_arg_time(J, a[1]);
    if (J->sig != JS_OK) return js_undef();
    jval s1 = intl_date_string(J, &f, x);
    if (J->sig != JS_OK) return js_undef();
    jval s2 = intl_date_string(J, &f, y);
    if (J->sig != JS_OK) return js_undef();
    if (js_str_eq(s1.str, s2.str)) return s1;
    jtext q = { 0, 0, 0, 0 };
    jt_put(J, &q, s1.str->s, s1.str->len);
    jt_put(J, &q, "\xE2\x80\x89\xE2\x80\x93\xE2\x80\x89", 9);
    jt_put(J, &q, s2.str->s, s2.str->len);
    return js_from_str(jt_done(J, &q));
}

static jval nat_intl_dtf_resolved(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_dtf f;
    if (!intl_load(J, t, INTL_DTF, &f, (u32)sizeof f)) return js_undef();
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return js_undef();
    intl_set_str(J, o, "locale", "en-US");
    intl_set_str(J, o, "calendar", "gregory");
    intl_set_str(J, o, "numberingSystem", "latn");
    intl_set_str(J, o, "timeZone", INTL_ZONES[f.zone].name);
    if (f.hour || f.time_style) {
        intl_set_str(J, o, "hourCycle", f.hour12 ? "h12" : "h23");
        intl_set(J, o, "hour12", js_bool(f.hour12));
    }
    static const char *const V[] = { "", "numeric", "2-digit", "long", "short", "narrow" };
    if (f.weekday) intl_set_str(J, o, "weekday", V[f.weekday]);
    if (f.era) intl_set_str(J, o, "era", V[f.era]);
    if (f.year) intl_set_str(J, o, "year", V[f.year]);
    if (f.month) intl_set_str(J, o, "month", V[f.month]);
    if (f.day) intl_set_str(J, o, "day", V[f.day]);
    if (f.hour) intl_set_str(J, o, "hour", f.hour12 ? V[f.hour] : "2-digit");
    if (f.minute) intl_set_str(J, o, "minute", "2-digit");
    if (f.second) intl_set_str(J, o, "second", "2-digit");
    if (f.fsd) intl_set(J, o, "fractionalSecondDigits", js_num(f.fsd));
    if (f.tzname) intl_set_str(J, o, "timeZoneName", DT_TZNAME[f.tzname - 1]);
    if (f.date_style) intl_set_str(J, o, "dateStyle", DT_STYLE[f.date_style - 1]);
    if (f.time_style) intl_set_str(J, o, "timeStyle", DT_STYLE[f.time_style - 1]);
    return js_from_obj(o);
}

/* Date.prototype.toLocaleString and its date and time forms. */
static jval intl_date_text(jctx *J, double t, jval locales, jval opts, int shape) {
    intl_requested(J, locales);
    if (J->sig != JS_OK) return js_undef();
    if (t != t) return js_from_str(js_str(J, "Invalid Date"));
    intl_dtf f;
    int req = shape == 5 ? 0 : shape == 6 ? 1 : 2, def = shape == 5 ? 0 : shape == 6 ? 1 : 2;
    if (!intl_dtf_setup(J, &f, opts, req, def)) return js_undef();
    return intl_date_string(J, &f, t);
}

/* --- plurals ------------------------------------------------------------------------------------ */

typedef struct { u32 kind; u8 ordinal, pad[3]; intl_nf nf; } intl_pr;

static const char *const PR_TYPE[] = { "cardinal", "ordinal", 0 };

static jval nat_intl_pr_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "PluralRules is made with new", J->error_line);
    intl_requested(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    intl_pr p;
    for (u32 i = 0; i < sizeof p; i++) ((char *)&p)[i] = 0;
    p.kind = INTL_PR;
    jval opts = intl_options(J, js_arg(a, n, 1));
    p.ordinal = (u8)intl_opt(J, opts, "type", PR_TYPE, 0);
    if (J->sig != JS_OK || !intl_nf_setup(J, &p.nf, opts, 0, 3)) return js_undef();
    p.nf.style = NFS_DECIMAL;
    p.nf.grouping = NFG_OFF;
    intl_store(J, t.obj, &p, (u32)sizeof p);
    return t;
}

/* English: one for 1 written with nothing after the point, else other;
   ordinals by their last digits. */
static const char *intl_plural(jctx *J, const intl_pr *p, double x) {
    if (x != x || x - x != 0) return "other";
    double ax = x < 0 ? -x : x;
    char dg[800];
    int nd, first;
    intl_round(&p->nf, ax, 0, p->nf.max_frac, p->nf.use_sig, dg, &nd, &first);
    int frac_shown = nd - 1 - first;
    if (frac_shown < 0) frac_shown = 0;
    if (!p->nf.use_sig && frac_shown < p->nf.min_frac) frac_shown = p->nf.min_frac;
    /* The whole part, as far as its last two digits. */
    long long i = 0;
    for (int pos = first; pos >= 0; pos--) {
        int k = first - pos;
        i = (i * 10 + (k < nd ? dg[k] - '0' : 0)) % 1000;
    }
    (void)J;
    if (p->ordinal) {
        if (frac_shown) return "other";
        int m10 = (int)(i % 10), m100 = (int)(i % 100);
        if (m10 == 1 && m100 != 11) return "one";
        if (m10 == 2 && m100 != 12) return "two";
        if (m10 == 3 && m100 != 13) return "few";
        return "other";
    }
    int whole_one = nd >= 1 && first == 0 && dg[0] == '1' && nd == 1;
    return whole_one && frac_shown == 0 ? "one" : "other";
}

static jval nat_intl_pr_select(jctx *J, jval t, jval *a, int n) {
    intl_pr p;
    if (!intl_load(J, t, INTL_PR, &p, (u32)sizeof p)) return js_undef();
    double x = js_to_num(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    return js_from_str(js_str(J, intl_plural(J, &p, x)));
}

static jval nat_intl_pr_select_range(jctx *J, jval t, jval *a, int n) {
    intl_pr p;
    if (!intl_load(J, t, INTL_PR, &p, (u32)sizeof p)) return js_undef();
    double x = js_to_num(J, js_arg(a, n, 0)), y = js_to_num(J, js_arg(a, n, 1));
    if (J->sig != JS_OK) return js_undef();
    if (x != x || y != y) return js_throw(J, JS_ERR_RANGE, "a range needs numbers", J->error_line);
    return js_from_str(js_str(J, intl_plural(J, &p, y)));
}

static jval nat_intl_pr_resolved(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_pr p;
    if (!intl_load(J, t, INTL_PR, &p, (u32)sizeof p)) return js_undef();
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return js_undef();
    intl_set_str(J, o, "locale", "en-US");
    intl_set_str(J, o, "type", PR_TYPE[p.ordinal]);
    intl_set(J, o, "minimumIntegerDigits", js_num(p.nf.min_int));
    if (p.nf.use_sig) {
        intl_set(J, o, "minimumSignificantDigits", js_num(p.nf.min_sig));
        intl_set(J, o, "maximumSignificantDigits", js_num(p.nf.max_sig));
    } else {
        intl_set(J, o, "minimumFractionDigits", js_num(p.nf.min_frac));
        intl_set(J, o, "maximumFractionDigits", js_num(p.nf.max_frac));
    }
    jobj *cats = js_array(J);
    static const char *const CARD[] = { "one", "other", 0 };
    static const char *const ORD[] = { "few", "one", "two", "other", 0 };
    for (const char *const *c = p.ordinal ? ORD : CARD; cats && *c; c++) js_arr_push(J, cats, js_from_str(js_str(J, *c)));
    if (cats) intl_set(J, o, "pluralCategories", js_from_obj(cats));
    intl_set_str(J, o, "roundingMode", "halfExpand");
    return js_from_obj(o);
}

/* --- relative times ------------------------------------------------------------------------------ */

typedef struct { u32 kind; u8 numeric_auto, style, pad[2]; } intl_rtf;

static const char *const RTF_NUMERIC[] = { "always", "auto", 0 };
static const char *const RTF_STYLE[] = { "long", "short", "narrow", 0 };
static const char *const RTF_UNITS[] = { "second", "minute", "hour", "day", "week", "month", "quarter", "year", 0 };
/* short: one, more; narrow is written against the number. */
static const char *const RTF_SHORT1[] = { "sec.", "min.", "hr.", "day", "wk.", "mo.", "qtr.", "yr." };
static const char *const RTF_SHORTN[] = { "sec.", "min.", "hr.", "days", "wk.", "mo.", "qtrs.", "yr." };
static const char *const RTF_NARROW[] = { "s", "m", "h", "d", "w", "mo", "q", "y" };

static jval nat_intl_rtf_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "RelativeTimeFormat is made with new", J->error_line);
    intl_requested(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    intl_rtf r = { INTL_RTF, 0, 0, { 0, 0 } };
    jval opts = intl_options(J, js_arg(a, n, 1));
    r.numeric_auto = (u8)intl_opt(J, opts, "numeric", RTF_NUMERIC, 0);
    r.style = (u8)intl_opt(J, opts, "style", RTF_STYLE, 0);
    if (J->sig != JS_OK) return js_undef();
    intl_store(J, t.obj, &r, (u32)sizeof r);
    return t;
}

static int intl_rtf_unit(jctx *J, jval u) {
    jstr *s = js_to_str(J, u);
    if (!s || J->sig != JS_OK) return -1;
    char b[16];
    u32 n = s->len < 15 ? s->len : 15;
    for (u32 i = 0; i < n; i++) b[i] = s->s[i];
    b[n] = 0;
    if (n > 1 && b[n - 1] == 's') b[n - 1] = 0;           /* days is day */
    for (int i = 0; RTF_UNITS[i]; i++) if (intl_same(b, RTF_UNITS[i])) return i;
    js_throw_named(J, JS_ERR_RANGE, "", s, " is not a unit of time");
    return -1;
}

static void intl_rtf_format(jctx *J, intl_parts *P, const intl_rtf *r, double v, int unit) {
    int neg = v < 0 || (v == 0 && 1 / v < 0);
    if (r->numeric_auto && (v == -1 || v == 0 || v == 1) && !(v == 0 && neg)) {
        static const char *const DAY[3] = { "yesterday", "today", "tomorrow" };
        static const char *const REL[3] = { "last ", "this ", "next " };
        int k = (int)v + 1;
        char buf[40];
        int w = 0;
        const char *word = 0;
        if (unit == 3) word = DAY[k];
        else if (unit == 0 && v == 0) word = "now";
        else if (unit >= 4 || (unit <= 2 && v == 0)) {
            for (const char *q = REL[k]; *q; q++) buf[w++] = *q;
            const char *u = r->style == 0 ? RTF_UNITS[unit] : RTF_SHORT1[unit];
            for (const char *q = u; *q; q++) buf[w++] = *q;
            buf[w] = 0;
            word = buf;
        }
        if (word) { ip_adds(J, P, "literal", word); return; }
    }
    double av = neg ? -v : v;
    intl_nf nf;
    for (u32 i = 0; i < sizeof nf; i++) ((char *)&nf)[i] = 0;
    nf.kind = INTL_NF;
    nf.min_int = 1;
    nf.max_frac = 3;
    nf.grouping = NFG_AUTO;
    intl_parts num;
    num.text.b = 0; num.text.n = 0; num.text.cap = 0; num.text.full = 0;
    num.n = 0;
    int zero = 0, one = 0;
    intl_number_body(J, &num, &nf, av, &zero, &one);
    const char *word;
    int tight = 0;
    if (r->style == 0) word = RTF_UNITS[unit];
    else if (r->style == 1) word = one ? RTF_SHORT1[unit] : RTF_SHORTN[unit];
    else { word = RTF_NARROW[unit]; tight = 1; }
    char tail[40];
    int w = 0;
    if (!tight) tail[w++] = ' ';
    for (const char *q = word; *q; q++) tail[w++] = *q;
    if (r->style == 0 && !one) tail[w++] = 's';
    tail[w] = 0;
    if (!neg) ip_add(J, P, "literal", "in ", 3);
    for (int i = 0; i < num.n; i++) {
        ip_add(J, P, num.p[i].type, num.text.b + num.p[i].at, num.p[i].len);
        P->p[P->n - 1].unit = RTF_UNITS[unit];
    }
    free(num.text.b);
    if (neg) {
        char t2[48];
        int k = 0;
        for (int i = 0; tail[i]; i++) t2[k++] = tail[i];
        for (const char *q = " ago"; *q; q++) t2[k++] = *q;
        t2[k] = 0;
        ip_adds(J, P, "literal", t2);
    } else ip_adds(J, P, "literal", tail);
}

static jval intl_rtf_run(jctx *J, jval t, jval *a, int n, int parts) {
    intl_rtf r;
    if (!intl_load(J, t, INTL_RTF, &r, (u32)sizeof r)) return js_undef();
    double v = js_to_num(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    if (v != v || v - v != 0) return js_throw(J, JS_ERR_RANGE, "a relative time needs a finite number", J->error_line);
    int unit = intl_rtf_unit(J, js_arg(a, n, 1));
    if (unit < 0) return js_undef();
    intl_parts *P = (intl_parts *)malloc(sizeof(intl_parts));
    if (!P) return js_undef();
    P->text.b = 0; P->text.n = 0; P->text.cap = 0; P->text.full = 0;
    P->n = 0;
    intl_rtf_format(J, P, &r, v, unit);
    jval res = parts ? ip_array(J, P, 0, js_undef()) : ip_string(J, P);
    free(P);
    return res;
}

static jval nat_intl_rtf_format(jctx *J, jval t, jval *a, int n) { return intl_rtf_run(J, t, a, n, 0); }
static jval nat_intl_rtf_parts(jctx *J, jval t, jval *a, int n) { return intl_rtf_run(J, t, a, n, 1); }

static jval nat_intl_rtf_resolved(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_rtf r;
    if (!intl_load(J, t, INTL_RTF, &r, (u32)sizeof r)) return js_undef();
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return js_undef();
    intl_set_str(J, o, "locale", "en-US");
    intl_set_str(J, o, "style", RTF_STYLE[r.style]);
    intl_set_str(J, o, "numeric", RTF_NUMERIC[r.numeric_auto]);
    intl_set_str(J, o, "numberingSystem", "latn");
    return js_from_obj(o);
}

/* --- comparing ------------------------------------------------------------------------------------ */

typedef struct { u32 kind; u8 usage, sensitivity, numeric, ignore_punct, case_first, pad[3]; } intl_col;

static const char *const COL_USAGE[] = { "sort", "search", 0 };
static const char *const COL_SENS[] = { "base", "accent", "case", "variant", 0 };
static const char *const COL_CASE[] = { "false", "upper", "lower", 0 };

/* Latin letters U+00C0 to U+017F as the letter they are written on, in its
   case; '*' for the ones that are two letters (Æ, ß, Ĳ, Œ) and '.' for the
   ones that are not letters (×, ÷). */
static const char INTL_LATIN[] =
    "AAAAAA*CEEEEIIIIDNOOOOO.OUUUUYT*aaaaaa*ceeeeiiiidnooooo.ouuuuyty"
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi**JjKkkLlLlLlLlLlNnNnNnnNnOoOoOo**RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

static int intl_utf8(const char *s, u32 n, u32 *at) {
    u8 c = (u8)s[*at];
    int cp, len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { (*at)++; return 0xFFFD; }
    if (*at + (u32)len > n) { *at = n; return 0xFFFD; }
    for (int i = 1; i < len; i++) cp = (cp << 6) | ((u8)s[*at + i] & 0x3F);
    *at += (u32)len;
    return cp;
}

/* The lower-case form of an upper-case Latin letter from U+00C0 on: its
   accent weighs the same whichever case it is in, so é and É differ only
   by case, which is weighed after. */
static int intl_latin_lower(int cp) {
    if (cp >= 0xC0 && cp <= 0xDE) return cp + 0x20;
    if (cp == 0x178) return 0xFF;
    if (cp >= 0x100 && cp < 0x17F) {
        char b = INTL_LATIN[cp - 0xC0];
        char next = INTL_LATIN[cp + 1 - 0xC0];
        if (next == intl_low(b)) return cp + 1;
    }
    return cp;
}

/* A character's weights: primary (what letter), secondary (which accent)
   and tertiary (which case, lower first); an expansion (ß, Æ) gives two
   primaries. 0 primaries for what is ignored. */
typedef struct { int p[2]; int np; int s, t; } intl_w;

static int intl_is_punct(int cp) {
    return (cp < 0x80 && !intl_alnum((char)cp)) || (cp >= 0x2000 && cp <= 0x206F) || (cp >= 0xA0 && cp <= 0xBF) || cp == 0xD7 || cp == 0xF7;
}

static void intl_weigh(int cp, intl_w *w) {
    w->np = 1;
    w->s = 0;
    w->t = 0;
    /* Classes, low to high: white space and punctuation, symbols, digits,
       Latin letters, then everything else by its number. */
    if (cp >= '0' && cp <= '9') { w->p[0] = 0x10000 + cp; return; }
    if (cp >= 'a' && cp <= 'z') { w->p[0] = 0x20000 + (cp - 'a') * 4; return; }
    if (cp >= 'A' && cp <= 'Z') { w->p[0] = 0x20000 + (cp - 'A') * 4; w->t = 1; return; }
    if (cp >= 0xC0 && cp <= 0x17F) {
        char b = INTL_LATIN[cp - 0xC0];
        if (b == '*') {
            const char *two = cp == 0xC6 || cp == 0xE6 ? "ae" : cp == 0xDF ? "ss" : cp == 0x132 || cp == 0x133 ? "ij" : "oe";
            w->p[0] = 0x20000 + (two[0] - 'a') * 4;
            w->p[1] = 0x20000 + (two[1] - 'a') * 4;
            w->np = 2;
            w->t = cp == 0xC6 || cp == 0x132 || cp == 0x152;
            w->s = w->t ? intl_latin_lower(cp) : cp;
            return;
        }
        if (b != '.') {
            int up = b >= 'A' && b <= 'Z';
            w->p[0] = 0x20000 + (intl_low(b) - 'a') * 4;
            /* Þ and þ come after z, as English dictionaries have them. */
            if (cp == 0xDE || cp == 0xFE) w->p[0] = 0x20000 + 26 * 4;
            w->s = up ? intl_latin_lower(cp) : cp;
            w->t = up;
            return;
        }
    }
    /* Greek and Cyrillic, one case for both. */
    if (cp >= 0x391 && cp <= 0x3A9) { w->p[0] = 0x30000 + cp + 0x20; w->t = 1; return; }
    if (cp >= 0x410 && cp <= 0x42F) { w->p[0] = 0x30000 + cp + 0x20; w->t = 1; return; }
    if (cp >= 0x400 && cp <= 0x40F) { w->p[0] = 0x30000 + cp + 0x50; w->t = 1; return; }
    if (cp >= 0x300 && cp <= 0x36F) { w->np = 0; w->s = cp; return; }        /* a combining accent */
    if (intl_is_punct(cp)) { w->p[0] = cp; return; }
    w->p[0] = 0x30000 + cp;
}

typedef struct { int *p, *s, *t; int np, ns, nt, cap; } intl_keys;

static void ik_push(int **arr, int *n, int *cap, int v) {
    if (*n >= *cap) return;
    (*arr)[(*n)++] = v;
}

static void intl_keys_of(const intl_col *c, const char *s, u32 n, intl_keys *k) {
    u32 at = 0;
    int capp = k->cap, caps = k->cap, capt = k->cap;
    k->np = k->ns = k->nt = 0;
    while (at < n) {
        u32 start = at;
        int cp = intl_utf8(s, n, &at);
        if (c->numeric && cp >= '0' && cp <= '9') {
            /* A run of digits is its number: leading noughts dropped, then
               its length, then its digits. */
            u32 e = start;
            while (e < n && s[e] >= '0' && s[e] <= '9') e++;
            u32 b = start;
            while (b + 1 < e && s[b] == '0') b++;
            ik_push(&k->p, &k->np, &capp, 0x10000 + 0x100 + (int)(e - b));
            for (u32 i = b; i < e; i++) ik_push(&k->p, &k->np, &capp, 0x10000 + s[i]);
            at = e;
            continue;
        }
        intl_w w;
        intl_weigh(cp, &w);
        if (c->ignore_punct && w.np == 1 && w.p[0] < 0x10000) continue;
        for (int i = 0; i < w.np; i++) ik_push(&k->p, &k->np, &capp, w.p[i]);
        if (w.np == 0 && k->ns > 0) { k->s[k->ns - 1] = k->s[k->ns - 1] * 2 + w.s; continue; }
        if (w.np) {
            ik_push(&k->s, &k->ns, &caps, w.s);
            ik_push(&k->t, &k->nt, &capt, w.t);
        }
    }
}

static int intl_cmp_arr(const int *a, int na, const int *b, int nb, int invert) {
    int m = na < nb ? na : nb;
    for (int i = 0; i < m; i++) if (a[i] != b[i]) return (a[i] < b[i]) != invert ? -1 : 1;
    return na == nb ? 0 : na < nb ? -1 : 1;
}

static int intl_collate(const intl_col *c, const char *a, u32 na, const char *b, u32 nb) {
    int cap = (int)(na > nb ? na : nb) + 8;
    int *mem = (int *)malloc((u64)cap * 6 * sizeof(int));
    if (!mem) return 0;
    intl_keys ka = { mem, mem + cap, mem + 2 * cap, 0, 0, 0, cap };
    intl_keys kb = { mem + 3 * cap, mem + 4 * cap, mem + 5 * cap, 0, 0, 0, cap };
    intl_keys_of(c, a, na, &ka);
    intl_keys_of(c, b, nb, &kb);
    int r = intl_cmp_arr(ka.p, ka.np, kb.p, kb.np, 0);
    if (!r && (c->sensitivity == 1 || c->sensitivity == 3)) r = intl_cmp_arr(ka.s, ka.ns, kb.s, kb.ns, 0);
    if (!r && (c->sensitivity == 2 || c->sensitivity == 3)) r = intl_cmp_arr(ka.t, ka.nt, kb.t, kb.nt, c->case_first == 1);
    free(mem);
    return r;
}

static int intl_col_setup(jctx *J, intl_col *c, jval opts) {
    for (u32 i = 0; i < sizeof *c; i++) ((char *)c)[i] = 0;
    c->kind = INTL_COL;
    opts = intl_options(J, opts);
    if (J->sig != JS_OK) return 0;
    c->usage = (u8)intl_opt(J, opts, "usage", COL_USAGE, 0);
    c->sensitivity = (u8)intl_opt(J, opts, "sensitivity", COL_SENS, 3);
    int num = intl_opt_bool(J, opts, "numeric");
    c->numeric = (u8)(num > 0);
    int ip = intl_opt_bool(J, opts, "ignorePunctuation");
    c->ignore_punct = (u8)(ip > 0);
    c->case_first = (u8)intl_opt(J, opts, "caseFirst", COL_CASE, 0);
    return J->sig == JS_OK;
}

static jval nat_intl_col_ctor(jctx *J, jval t, jval *a, int n) {
    intl_requested(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    intl_col c;
    if (!intl_col_setup(J, &c, js_arg(a, n, 1))) return js_undef();
    jobj *o = js_is_obj(t) && J->new_target.t != JS_UNDEF ? t.obj : 0;
    if (!o) {
        jval pv = js_get(J, js_from_obj(J->callee), J->s_prototype);
        o = js_object_with(J, JO_PLAIN, js_is_obj(pv) ? pv.obj : J->p_object);
        if (!o) return js_undef();
    }
    intl_store(J, o, &c, (u32)sizeof c);
    return js_from_obj(o);
}

static jval nat_intl_col_compare_fn(jctx *J, jval t, jval *a, int n) {
    (void)t;
    intl_col c;
    if (!intl_load(J, J->callee->data, INTL_COL, &c, (u32)sizeof c)) return js_undef();
    jstr *x = js_to_str(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    jstr *y = js_to_str(J, js_arg(a, n, 1));
    if (J->sig != JS_OK || !x || !y) return js_undef();
    return js_num(intl_collate(&c, x->s, x->len, y->s, y->len));
}

static jval nat_intl_col_compare(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_col c;
    if (!intl_load(J, t, INTL_COL, &c, (u32)sizeof c)) return js_undef();
    return intl_bound(J, t, "*compare*", nat_intl_col_compare_fn, 2);
}

static jval nat_intl_col_resolved(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_col c;
    if (!intl_load(J, t, INTL_COL, &c, (u32)sizeof c)) return js_undef();
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return js_undef();
    intl_set_str(J, o, "locale", "en-US");
    intl_set_str(J, o, "usage", COL_USAGE[c.usage]);
    intl_set_str(J, o, "sensitivity", COL_SENS[c.sensitivity]);
    intl_set(J, o, "ignorePunctuation", js_bool(c.ignore_punct));
    intl_set_str(J, o, "collation", "default");
    intl_set(J, o, "numeric", js_bool(c.numeric));
    intl_set_str(J, o, "caseFirst", COL_CASE[c.case_first]);
    return js_from_obj(o);
}

/* String.prototype.localeCompare. */
static jval intl_locale_compare(jctx *J, jstr *x, jstr *y, jval locales, jval opts) {
    intl_requested(J, locales);
    if (J->sig != JS_OK) return js_undef();
    intl_col c;
    if (!intl_col_setup(J, &c, opts)) return js_undef();
    return js_num(intl_collate(&c, x->s, x->len, y->s, y->len));
}

/* --- lists -------------------------------------------------------------------------------------- */

typedef struct { u32 kind; u8 type, style, pad[2]; } intl_lf;

static const char *const LF_TYPE[] = { "conjunction", "disjunction", "unit", 0 };
static const char *const LF_STYLE[] = { "long", "short", "narrow", 0 };

static jval nat_intl_lf_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "ListFormat is made with new", J->error_line);
    intl_requested(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    intl_lf l = { INTL_LF, 0, 0, { 0, 0 } };
    jval opts = intl_options(J, js_arg(a, n, 1));
    l.type = (u8)intl_opt(J, opts, "type", LF_TYPE, 0);
    l.style = (u8)intl_opt(J, opts, "style", LF_STYLE, 0);
    if (J->sig != JS_OK) return js_undef();
    intl_store(J, t.obj, &l, (u32)sizeof l);
    return t;
}

static jval intl_lf_run(jctx *J, jval t, jval *a, int n, int parts) {
    intl_lf l;
    if (!intl_load(J, t, INTL_LF, &l, (u32)sizeof l)) return js_undef();
    jargs A;
    js_args_init(&A);
    jval list = js_arg(a, n, 0);
    if (list.t != JS_UNDEF) js_iter_collect(J, list, &A);
    for (int i = 0; i < A.n && J->sig == JS_OK; i++)
        if (A.v[i].t != JS_STR) js_throw(J, JS_ERR_TYPE, "a list to write is a list of strings", J->error_line);
    if (J->sig != JS_OK) { js_args_free(&A); return js_undef(); }
    intl_parts *P = (intl_parts *)malloc(sizeof(intl_parts));
    if (!P) { js_args_free(&A); return js_undef(); }
    P->text.b = 0; P->text.n = 0; P->text.cap = 0; P->text.full = 0;
    P->n = 0;
    const char *word = l.type == 1 ? "or" : l.style == 1 ? "&" : "and";
    for (int i = 0; i < A.n; i++) {
        if (i) {
            char sep[16];
            int w = 0;
            int last = i == A.n - 1;
            if (l.type == 2) {
                if (l.style == 2) sep[w++] = ' ';
                else { sep[w++] = ','; sep[w++] = ' '; }
            } else if (l.type == 0 && l.style == 2) {
                sep[w++] = ','; sep[w++] = ' ';
            } else if (last) {
                if (A.n > 2) sep[w++] = ',';
                sep[w++] = ' ';
                for (const char *q = word; *q; q++) sep[w++] = *q;
                sep[w++] = ' ';
            } else { sep[w++] = ','; sep[w++] = ' '; }
            ip_add(J, P, "literal", sep, (u32)w);
        }
        ip_add(J, P, "element", A.v[i].str->s, A.v[i].str->len);
        if (A.v[i].str->len == 0 && P->n < 519) {
            P->p[P->n].type = "element"; P->p[P->n].at = P->text.n; P->p[P->n].len = 0; P->p[P->n].unit = 0; P->n++;
        }
    }
    js_args_free(&A);
    jval r = parts ? ip_array(J, P, 0, js_undef()) : ip_string(J, P);
    if (!parts && r.t == JS_UNDEF) r = js_from_str(js_str(J, ""));
    free(P);
    return r;
}

static jval nat_intl_lf_format(jctx *J, jval t, jval *a, int n) { return intl_lf_run(J, t, a, n, 0); }
static jval nat_intl_lf_parts(jctx *J, jval t, jval *a, int n) { return intl_lf_run(J, t, a, n, 1); }

static jval nat_intl_lf_resolved(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_lf l;
    if (!intl_load(J, t, INTL_LF, &l, (u32)sizeof l)) return js_undef();
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return js_undef();
    intl_set_str(J, o, "locale", "en-US");
    intl_set_str(J, o, "type", LF_TYPE[l.type]);
    intl_set_str(J, o, "style", LF_STYLE[l.style]);
    return js_from_obj(o);
}

/* --- segments ------------------------------------------------------------------------------------ */

typedef struct { u32 kind; u8 granularity, pad[3]; } intl_seg;

static const char *const SEG_GRAN[] = { "grapheme", "word", "sentence", 0 };

static jval nat_intl_seg_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "Segmenter is made with new", J->error_line);
    intl_requested(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    intl_seg s = { INTL_SEG, 0, { 0, 0, 0 } };
    s.granularity = (u8)intl_opt(J, intl_options(J, js_arg(a, n, 1)), "granularity", SEG_GRAN, 0);
    if (J->sig != JS_OK) return js_undef();
    intl_store(J, t.obj, &s, (u32)sizeof s);
    return t;
}

static int intl_is_extender(int cp) {
    return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF)
        || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F)
        || (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0100 && cp <= 0xE01EF) || cp == 0x200C;
}

static int intl_wordish(int cp) {
    if (cp < 0x80) return intl_alnum((char)cp) || cp == '_';
    if (intl_is_punct(cp)) return 0;
    if (cp >= 0x2E80 && cp <= 0x9FFF) return 0;                /* ideographs: one a word */
    return 1;
}

static int intl_cjk(int cp) { return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF); }

/* Where the segment starting at `at` ends, and whether it is word-like. */
static u32 intl_seg_next(const intl_seg *g, const char *s, u32 n, u32 at, int *wordlike) {
    *wordlike = 0;
    u32 p = at;
    int cp = intl_utf8(s, n, &p);
    if (g->granularity == 0) {
        if (cp == '\r' && p < n && s[p] == '\n') return p + 1;
        int ri = cp >= 0x1F1E6 && cp <= 0x1F1FF;
        while (p < n) {
            u32 q = p;
            int nx = intl_utf8(s, n, &q);
            if (intl_is_extender(nx)) { p = q; continue; }
            if (nx == 0x200D && q < n) { intl_utf8(s, n, &q); p = q; continue; }
            if (ri && nx >= 0x1F1E6 && nx <= 0x1F1FF) { p = q; ri = 0; continue; }
            break;
        }
        return p;
    }
    if (g->granularity == 1) {
        if (cp == ' ' || cp == '\t') {
            while (p < n && (s[p] == ' ' || s[p] == '\t')) p++;
            return p;
        }
        if (intl_cjk(cp)) { *wordlike = 1; return p; }
        if (!intl_wordish(cp)) {
            while (p < n) { u32 q = p; int nx = intl_utf8(s, n, &q); if (!intl_is_extender(nx)) break; p = q; }
            return p;
        }
        *wordlike = 1;
        while (p < n) {
            u32 q = p;
            int nx = intl_utf8(s, n, &q);
            if (intl_wordish(nx) || intl_is_extender(nx)) { if (intl_cjk(nx)) break; p = q; continue; }
            /* An apostrophe or a point between letters or digits keeps the
               word: don't, 3.14. */
            if ((nx == '\'' || nx == '.' || nx == ',' || nx == 0x2019) && q < n) {
                u32 r = q;
                int after = intl_utf8(s, n, &r);
                int digit_run = cp >= '0' && cp <= '9';
                if (intl_wordish(after) && !intl_cjk(after) && (nx != ',' || digit_run) && (nx != '.' || digit_run || nx == '.'))
                    if (nx != '.' || (after >= '0' && after <= '9' && s[p - 1] >= '0' && s[p - 1] <= '9')
                        || nx == '\'' || nx == 0x2019) { p = r; continue; }
            }
            break;
        }
        return p;
    }
    /* Sentences: to the end of the stop and the space after it. */
    while (p < n) {
        char ch = s[p - 1];
        if (ch == '.' || ch == '!' || ch == '?') {
            while (p < n && (s[p] == '.' || s[p] == '!' || s[p] == '?' || s[p] == '"' || s[p] == '\'' || s[p] == ')')) p++;
            if (p >= n || s[p] == ' ' || s[p] == '\n' || s[p] == '\t') {
                while (p < n && (s[p] == ' ' || s[p] == '\n' || s[p] == '\t' || s[p] == '\r')) p++;
                return p;
            }
        }
        if (ch == '\n') return p;
        intl_utf8(s, n, &p);
    }
    return p;
}

static jstr *intl_k_seg_input, *intl_k_seg_at;

static jval intl_seg_data(jctx *J, const intl_seg *g, jstr *input, u32 at, u32 end, int wordlike) {
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return js_undef();
    js_set(J, o, "segment", js_from_str(js_str_n(J, input->s + at, end - at)));
    js_set(J, o, "index", js_num(at));
    js_set(J, o, "input", js_from_str(input));
    if (g->granularity == 1) js_set(J, o, "isWordLike", js_bool(wordlike));
    return js_from_obj(o);
}

static jobj *intl_p_segments, *intl_p_seg_iter;

static jval nat_intl_seg_segment(jctx *J, jval t, jval *a, int n) {
    intl_seg g;
    if (!intl_load(J, t, INTL_SEG, &g, (u32)sizeof g)) return js_undef();
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s || J->sig != JS_OK) return js_undef();
    jobj *o = js_object_with(J, JO_PLAIN, intl_p_segments);
    if (!o) return js_undef();
    intl_store(J, o, &g, (u32)sizeof g);
    jprop *p = js_put_prop(J, o, intl_k_seg_input, js_from_str(s));
    if (p) p->flags = 0;
    return js_from_obj(o);
}

static jval nat_intl_segments_containing(jctx *J, jval t, jval *a, int n) {
    intl_seg g;
    if (!intl_load(J, t, INTL_SEG, &g, (u32)sizeof g)) return js_undef();
    jprop *ip = js_find(t.obj, intl_k_seg_input);
    if (!ip || ip->v.t != JS_STR) return js_undef();
    jstr *s = ip->v.str;
    double want = n > 0 && a[0].t != JS_UNDEF ? js_to_num(J, a[0]) : 0;
    if (want != want || want < 0 || want >= s->len) return js_undef();
    u32 at = 0;
    while (at < s->len) {
        int wl;
        u32 end = intl_seg_next(&g, s->s, s->len, at, &wl);
        if (end <= at) end = at + 1;
        if (want < end) return intl_seg_data(J, &g, s, at, end, wl);
        at = end;
    }
    return js_undef();
}

static jval nat_intl_segments_iter(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_seg g;
    if (!intl_load(J, t, INTL_SEG, &g, (u32)sizeof g)) return js_undef();
    jprop *ip = js_find(t.obj, intl_k_seg_input);
    jobj *it = js_object_with(J, JO_PLAIN, intl_p_seg_iter);
    if (!it || !ip) return js_undef();
    intl_store(J, it, &g, (u32)sizeof g);
    jprop *p = js_put_prop(J, it, intl_k_seg_input, ip->v);
    if (p) p->flags = 0;
    p = js_put_prop(J, it, intl_k_seg_at, js_num(0));
    if (p) p->flags = JP_WRITE;
    return js_from_obj(it);
}

static jval nat_intl_seg_iter_next(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_seg g;
    if (!intl_load(J, t, INTL_SEG, &g, (u32)sizeof g)) return js_undef();
    jprop *ip = js_find(t.obj, intl_k_seg_input), *ap = js_find(t.obj, intl_k_seg_at);
    if (!ip || !ap || ip->v.t != JS_STR) return js_iter_result(J, js_undef(), 1);
    jstr *s = ip->v.str;
    u32 at = (u32)ap->v.num;
    if (at >= s->len) return js_iter_result(J, js_undef(), 1);
    int wl;
    u32 end = intl_seg_next(&g, s->s, s->len, at, &wl);
    if (end <= at) end = at + 1;
    ap->v = js_num(end);
    return js_iter_result(J, intl_seg_data(J, &g, s, at, end, wl), 0);
}

static jval nat_intl_seg_iter_self(jctx *J, jval t, jval *a, int n) { (void)J; (void)a; (void)n; return t; }

static jval nat_intl_seg_resolved(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_seg g;
    if (!intl_load(J, t, INTL_SEG, &g, (u32)sizeof g)) return js_undef();
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return js_undef();
    intl_set_str(J, o, "locale", "en-US");
    intl_set_str(J, o, "granularity", SEG_GRAN[g.granularity]);
    return js_from_obj(o);
}

/* --- names ------------------------------------------------------------------------------------ */

typedef struct { u32 kind; u8 type, style, fallback, pad; } intl_dn;

static const char *const DN_TYPE[] = { "language", "region", "script", "currency", "calendar", "dateTimeField", 0 };
static const char *const DN_STYLE[] = { "long", "short", "narrow", 0 };
static const char *const DN_FALLBACK[] = { "code", "none", 0 };

static const char *const INTL_LANGS[] = {
    "ar", "Arabic", "bn", "Bangla", "cs", "Czech", "da", "Danish", "de", "German", "el", "Greek", "en", "English",
    "es", "Spanish", "fa", "Persian", "fi", "Finnish", "fr", "French", "he", "Hebrew", "hi", "Hindi",
    "hu", "Hungarian", "id", "Indonesian", "it", "Italian", "ja", "Japanese", "ko", "Korean", "ms", "Malay",
    "nl", "Dutch", "no", "Norwegian", "nb", "Norwegian Bokm\xC3\xA5l", "pl", "Polish", "pt", "Portuguese",
    "ro", "Romanian", "ru", "Russian", "sv", "Swedish", "sw", "Swahili", "ta", "Tamil", "th", "Thai",
    "tr", "Turkish", "uk", "Ukrainian", "ur", "Urdu", "vi", "Vietnamese", "zh", "Chinese", "ca", "Catalan",
    "ga", "Irish", "cy", "Welsh", "la", "Latin", "eo", "Esperanto", 0,
};
static const char *const INTL_REGIONS[] = {
    "US", "United States", "GB", "United Kingdom", "CA", "Canada", "AU", "Australia", "NZ", "New Zealand",
    "IE", "Ireland", "IN", "India", "DE", "Germany", "FR", "France", "ES", "Spain", "IT", "Italy", "NL", "Netherlands",
    "BE", "Belgium", "CH", "Switzerland", "AT", "Austria", "SE", "Sweden", "NO", "Norway", "DK", "Denmark",
    "FI", "Finland", "PL", "Poland", "PT", "Portugal", "GR", "Greece", "TR", "T\xC3\xBCrkiye", "RU", "Russia",
    "UA", "Ukraine", "CN", "China", "JP", "Japan", "KR", "South Korea", "TW", "Taiwan", "HK", "Hong Kong SAR China",
    "SG", "Singapore", "MY", "Malaysia", "ID", "Indonesia", "TH", "Thailand", "VN", "Vietnam", "PH", "Philippines",
    "BR", "Brazil", "MX", "Mexico", "AR", "Argentina", "CL", "Chile", "CO", "Colombia", "PE", "Peru",
    "ZA", "South Africa", "NG", "Nigeria", "EG", "Egypt", "KE", "Kenya", "IL", "Israel", "AE", "United Arab Emirates",
    "SA", "Saudi Arabia", "PK", "Pakistan", "BD", "Bangladesh", "CZ", "Czechia", "HU", "Hungary", "RO", "Romania",
    "001", "world", "150", "Europe", "419", "Latin America", 0,
};
static const char *const INTL_SCRIPTS[] = {
    "Latn", "Latin", "Cyrl", "Cyrillic", "Arab", "Arabic", "Grek", "Greek", "Hebr", "Hebrew", "Deva", "Devanagari",
    "Hans", "Simplified Han", "Hant", "Traditional Han", "Jpan", "Japanese", "Kore", "Korean", "Thai", "Thai",
    "Hira", "Hiragana", "Kana", "Katakana", "Beng", "Bangla", 0,
};

static const char *intl_name_in(const char *const *table, const char *code) {
    for (int i = 0; table[i]; i += 2) if (intl_same(table[i], code)) return table[i + 1];
    return 0;
}

static jval nat_intl_dn_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "DisplayNames is made with new", J->error_line);
    intl_requested(J, js_arg(a, n, 0));
    if (J->sig != JS_OK) return js_undef();
    jval opts = js_arg(a, n, 1);
    if (!js_is_obj(opts)) return js_throw(J, JS_ERR_TYPE, "DisplayNames needs options with a type", J->error_line);
    intl_dn d = { INTL_DN, 0, 0, 0, 0 };
    d.style = (u8)intl_opt(J, opts, "style", DN_STYLE, 0);
    d.type = (u8)intl_opt(J, opts, "type", DN_TYPE, 99);
    d.fallback = (u8)intl_opt(J, opts, "fallback", DN_FALLBACK, 0);
    if (J->sig != JS_OK) return js_undef();
    if (d.type == 99) return js_throw(J, JS_ERR_TYPE, "DisplayNames needs a type", J->error_line);
    intl_store(J, t.obj, &d, (u32)sizeof d);
    return t;
}

static jval nat_intl_dn_of(jctx *J, jval t, jval *a, int n) {
    intl_dn d;
    if (!intl_load(J, t, INTL_DN, &d, (u32)sizeof d)) return js_undef();
    jstr *s = js_to_str(J, js_arg(a, n, 0));
    if (!s || J->sig != JS_OK) return js_undef();
    char code[64];
    const char *name = 0;
    char buf[128];
    if (d.type == 0) {
        if (!intl_canon(s->s, s->len, code, (int)sizeof code)) return js_throw(J, JS_ERR_RANGE, "that is not a language tag", J->error_line);
        if (intl_same(code, "en-US")) name = "American English";
        else if (intl_same(code, "en-GB")) name = "British English";
        else if (intl_same(code, "en-AU")) name = "Australian English";
        else if (intl_same(code, "en-CA")) name = "Canadian English";
        else if (intl_same(code, "es-MX")) name = "Mexican Spanish";
        else if (intl_same(code, "pt-BR")) name = "Brazilian Portuguese";
        else if (intl_same(code, "fr-CA")) name = "Canadian French";
        else if (intl_same(code, "zh-Hans")) name = "Simplified Chinese";
        else if (intl_same(code, "zh-Hant")) name = "Traditional Chinese";
        else {
            char lang[12];
            int k = 0;
            while (code[k] && code[k] != '-' && k < 11) { lang[k] = code[k]; k++; }
            lang[k] = 0;
            const char *ln = intl_name_in(INTL_LANGS, lang);
            if (ln && code[k] == '-') {
                const char *reg = intl_name_in(INTL_REGIONS, code + k + 1);
                if (!reg) reg = intl_name_in(INTL_SCRIPTS, code + k + 1);
                if (reg) {
                    int w = 0;
                    for (const char *q = ln; *q && w < 100; q++) buf[w++] = *q;
                    buf[w++] = ' '; buf[w++] = '(';
                    for (const char *q = reg; *q && w < 125; q++) buf[w++] = *q;
                    buf[w++] = ')'; buf[w] = 0;
                    name = buf;
                }
            } else if (ln && !code[k]) name = ln;
        }
    } else {
        u32 k = 0;
        for (; k < s->len && k < 63; k++) code[k] = d.type == 1 || d.type == 3 ? intl_up(s->s[k]) : s->s[k];
        code[k] = 0;
        if (d.type == 2 && k == 4) { code[0] = intl_up(code[0]); for (int i = 1; i < 4; i++) code[i] = intl_low(code[i]); }
        if (d.type == 1) name = intl_name_in(INTL_REGIONS, code);
        else if (d.type == 2) name = intl_name_in(INTL_SCRIPTS, code);
        else if (d.type == 3) {
            const intl_currency *c = intl_currency_of(code);
            if (c) {
                int w = 0;
                buf[w++] = intl_up(c->one[0]);
                for (const char *q = c->one + 1; *q && w < 120; q++) buf[w++] = *q;
                buf[w] = 0;
                /* The name of a currency is written with capitals in a list
                   of them: US Dollar, Euro. */
                for (int i = 1; i < w; i++) if (buf[i - 1] == ' ') buf[i] = intl_up(buf[i]);
                name = buf;
            }
        } else if (d.type == 4) name = intl_same(code, "gregory") ? "Gregorian Calendar" : 0;
        else if (d.type == 5) {
            static const char *const F[] = { "era", "era", "year", "year", "quarter", "quarter", "month", "month",
                "weekOfYear", "week", "weekday", "day of the week", "day", "day", "dayPeriod", "AM/PM", "hour", "hour",
                "minute", "minute", "second", "second", "timeZoneName", "time zone", 0 };
            name = intl_name_in(F, code);
        }
    }
    if (name) return js_from_str(js_str(J, name));
    if (d.fallback == 1) return js_undef();
    return js_from_str(js_str(J, d.type == 0 ? code : code));
}

static jval nat_intl_dn_resolved(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_dn d;
    if (!intl_load(J, t, INTL_DN, &d, (u32)sizeof d)) return js_undef();
    jobj *o = js_object(J, JO_PLAIN);
    if (!o) return js_undef();
    intl_set_str(J, o, "locale", "en-US");
    intl_set_str(J, o, "style", DN_STYLE[d.style]);
    intl_set_str(J, o, "type", DN_TYPE[d.type]);
    intl_set_str(J, o, "fallback", DN_FALLBACK[d.fallback]);
    return js_from_obj(o);
}

/* --- Intl.Locale ---------------------------------------------------------------------------------- */

typedef struct { u32 kind; char tag[64]; } intl_loc;

static jval nat_intl_loc_ctor(jctx *J, jval t, jval *a, int n) {
    if (J->new_target.t == JS_UNDEF) return js_throw(J, JS_ERR_TYPE, "Locale is made with new", J->error_line);
    jval v = js_arg(a, n, 0);
    if (v.t != JS_STR && !js_is_obj(v)) return js_throw(J, JS_ERR_TYPE, "a Locale is made from a tag", J->error_line);
    jstr *s = js_to_str(J, v);
    if (!s || J->sig != JS_OK) return js_undef();
    intl_loc l;
    for (u32 i = 0; i < sizeof l; i++) ((char *)&l)[i] = 0;
    l.kind = INTL_LOC;
    if (!intl_canon(s->s, s->len, l.tag, (int)sizeof l.tag)) return js_throw(J, JS_ERR_RANGE, "that is not a language tag", J->error_line);
    /* Options may set the language, script and region. */
    jval opts = js_arg(a, n, 1);
    if (js_is_obj(opts)) {
        char lang[12] = { 0 }, script[8] = { 0 }, region[8] = { 0 };
        const char *p = l.tag;
        int k = 0;
        while (*p && *p != '-' && k < 11) lang[k++] = *p++;
        while (*p == '-') {
            p++;
            char part[12];
            int m = 0;
            while (*p && *p != '-' && m < 11) part[m++] = *p++;
            part[m] = 0;
            if (m == 4 && !script[0]) for (int i = 0; i <= m; i++) script[i] = part[i];
            else if ((m == 2 || m == 3) && !region[0]) for (int i = 0; i <= m; i++) region[i] = part[i];
        }
        char o[16];
        if (intl_opt_text(J, opts, "language", o, (int)sizeof o)) for (int i = 0; i < 12; i++) lang[i] = i < 11 ? intl_low(o[i]) : 0;
        if (intl_opt_text(J, opts, "script", o, (int)sizeof o)) { for (int i = 0; i < 5; i++) script[i] = i ? intl_low(o[i]) : intl_up(o[0]); script[4] = 0; }
        if (intl_opt_text(J, opts, "region", o, (int)sizeof o)) { for (int i = 0; i < 4; i++) region[i] = intl_up(o[i]); region[3] = 0; }
        int w = 0;
        for (const char *q = lang; *q; q++) l.tag[w++] = *q;
        if (script[0]) { l.tag[w++] = '-'; for (const char *q = script; *q; q++) l.tag[w++] = *q; }
        if (region[0]) { l.tag[w++] = '-'; for (const char *q = region; *q; q++) l.tag[w++] = *q; }
        l.tag[w] = 0;
    }
    intl_store(J, t.obj, &l, (u32)sizeof l);
    return t;
}

/* Which part of the tag: 0 language, 1 script, 2 region, 3 the whole. */
static jval intl_loc_part(jctx *J, jval t, int which) {
    intl_loc l;
    if (!intl_load(J, t, INTL_LOC, &l, (u32)sizeof l)) return js_undef();
    if (which == 3) return js_from_str(js_str(J, l.tag));
    const char *p = l.tag;
    char part[12];
    int m = 0;
    while (*p && *p != '-' && m < 11) part[m++] = *p++;
    part[m] = 0;
    if (which == 0) return js_from_str(js_str(J, part));
    while (*p == '-') {
        p++;
        m = 0;
        while (*p && *p != '-' && m < 11) part[m++] = *p++;
        part[m] = 0;
        if (which == 1 && m == 4) return js_from_str(js_str(J, part));
        if (which == 2 && (m == 2 || (m == 3 && part[0] >= '0' && part[0] <= '9'))) return js_from_str(js_str(J, part));
    }
    return js_undef();
}

static jval nat_intl_loc_language(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return intl_loc_part(J, t, 0); }
static jval nat_intl_loc_script(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return intl_loc_part(J, t, 1); }
static jval nat_intl_loc_region(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return intl_loc_part(J, t, 2); }
static jval nat_intl_loc_tostring(jctx *J, jval t, jval *a, int n) { (void)a; (void)n; return intl_loc_part(J, t, 3); }

/* maximize: the likely script and region, for the languages this knows. */
static jval nat_intl_loc_maximize(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    intl_loc l;
    if (!intl_load(J, t, INTL_LOC, &l, (u32)sizeof l)) return js_undef();
    static const char *const LIKELY[] = { "en", "Latn-US", "fr", "Latn-FR", "de", "Latn-DE", "es", "Latn-ES", "it", "Latn-IT",
        "pt", "Latn-BR", "nl", "Latn-NL", "sv", "Latn-SE", "pl", "Latn-PL", "ru", "Cyrl-RU", "uk", "Cyrl-UA", "ja", "Jpan-JP",
        "ko", "Kore-KR", "zh", "Hans-CN", "ar", "Arab-EG", "hi", "Deva-IN", "tr", "Latn-TR", "el", "Grek-GR", "he", "Hebr-IL", 0 };
    jval lang = intl_loc_part(J, t, 0), scr = intl_loc_part(J, t, 1), reg = intl_loc_part(J, t, 2);
    const char *likely = lang.t == JS_STR ? intl_name_in(LIKELY, lang.str->s) : 0;
    char tag[64];
    int w = 0;
    for (const char *q = lang.t == JS_STR ? lang.str->s : "und"; *q; q++) tag[w++] = *q;
    char ls[8] = { 0 }, lr[8] = { 0 };
    if (likely) { for (int i = 0; i < 4; i++) ls[i] = likely[i]; for (int i = 0; likely[5 + i]; i++) lr[i] = likely[5 + i]; }
    const char *s1 = scr.t == JS_STR ? scr.str->s : ls[0] ? ls : 0;
    const char *r1 = reg.t == JS_STR ? reg.str->s : lr[0] ? lr : 0;
    if (s1) { tag[w++] = '-'; for (const char *q = s1; *q; q++) tag[w++] = *q; }
    if (r1) { tag[w++] = '-'; for (const char *q = r1; *q; q++) tag[w++] = *q; }
    tag[w] = 0;
    jval arg = js_from_str(js_str(J, tag));
    jval ctor = js_get(J, t, J->s_constructor);
    return js_construct(J, ctor, &arg, 1, ctor);
}

static jval nat_intl_loc_minimize(jctx *J, jval t, jval *a, int n) {
    (void)a; (void)n;
    jval lang = intl_loc_part(J, t, 0);
    if (lang.t != JS_STR) return js_undef();
    jval ctor = js_get(J, t, J->s_constructor);
    return js_construct(J, ctor, &lang, 1, ctor);
}

/* --- supportedValuesOf, and the whole of it put together ------------------------------------------- */

static jval nat_intl_values_of(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jstr *k = js_to_str(J, js_arg(a, n, 0));
    if (!k || J->sig != JS_OK) return js_undef();
    jobj *out = js_array(J);
    if (!out) return js_undef();
    if (js_str_is(k, "calendar")) js_arr_push(J, out, js_from_str(js_str(J, "gregory")));
    else if (js_str_is(k, "collation")) js_arr_push(J, out, js_from_str(js_str(J, "default")));
    else if (js_str_is(k, "numberingSystem")) js_arr_push(J, out, js_from_str(js_str(J, "latn")));
    else if (js_str_is(k, "currency")) { for (int i = 0; INTL_CURRENCIES[i].code; i++) js_arr_push(J, out, js_from_str(js_str(J, INTL_CURRENCIES[i].code))); }
    else if (js_str_is(k, "timeZone")) { for (int i = 0; INTL_ZONES[i].name; i++) js_arr_push(J, out, js_from_str(js_str(J, INTL_ZONES[i].name))); }
    else if (js_str_is(k, "unit")) { for (int i = 0; INTL_UNITS[i].id; i++) js_arr_push(J, out, js_from_str(js_str(J, INTL_UNITS[i].id))); }
    else return js_throw_named(J, JS_ERR_RANGE, "", k, " is not a kind of value Intl lists");
    return js_from_obj(out);
}

static jobj *intl_class(jctx *J, jobj *intl, const char *name, jnative ctor, int arity, const char *tag) {
    jobj *proto = js_object_with(J, JO_PLAIN, J->p_object);
    jobj *c = js_native_n(J, name, ctor, arity);
    if (!proto || !c) return 0;
    js_put_prop_flags(J, c, J->s_prototype, js_from_obj(proto), 0);
    js_put_prop_flags(J, proto, J->s_constructor, js_from_obj(c), JP_WRITE | JP_CONF);
    js_put_prop_flags(J, intl, js_str(J, name), js_from_obj(c), JP_WRITE | JP_CONF);
    js_tag(J, proto, tag);
    js_method(J, c, "supportedLocalesOf", nat_intl_supported, 1);
    return proto;
}

static void js_setup_intl(jctx *J) {
    intl_k_state = js_sym_new(J, "intl", 4);
    intl_k_seg_input = js_sym_new(J, "input", 5);
    intl_k_seg_at = js_sym_new(J, "at", 2);
    jobj *intl = js_object(J, JO_PLAIN);
    if (!intl) return;
    js_tag(J, intl, "Intl");
    js_method(J, intl, "getCanonicalLocales", nat_intl_canonical, 1);
    js_method(J, intl, "supportedValuesOf", nat_intl_values_of, 1);

    jobj *p = intl_class(J, intl, "NumberFormat", nat_intl_nf_ctor, 0, "Intl.NumberFormat");
    js_getter(J, p, "format", nat_intl_nf_format);
    js_method(J, p, "formatToParts", nat_intl_nf_parts, 1);
    js_method(J, p, "formatRange", nat_intl_nf_range, 2);
    js_method(J, p, "resolvedOptions", nat_intl_nf_resolved, 0);

    p = intl_class(J, intl, "DateTimeFormat", nat_intl_dtf_ctor, 0, "Intl.DateTimeFormat");
    js_getter(J, p, "format", nat_intl_dtf_format);
    js_method(J, p, "formatToParts", nat_intl_dtf_parts, 1);
    js_method(J, p, "formatRange", nat_intl_dtf_range, 2);
    js_method(J, p, "resolvedOptions", nat_intl_dtf_resolved, 0);

    p = intl_class(J, intl, "PluralRules", nat_intl_pr_ctor, 0, "Intl.PluralRules");
    js_method(J, p, "select", nat_intl_pr_select, 1);
    js_method(J, p, "selectRange", nat_intl_pr_select_range, 2);
    js_method(J, p, "resolvedOptions", nat_intl_pr_resolved, 0);

    p = intl_class(J, intl, "RelativeTimeFormat", nat_intl_rtf_ctor, 0, "Intl.RelativeTimeFormat");
    js_method(J, p, "format", nat_intl_rtf_format, 2);
    js_method(J, p, "formatToParts", nat_intl_rtf_parts, 2);
    js_method(J, p, "resolvedOptions", nat_intl_rtf_resolved, 0);

    p = intl_class(J, intl, "Collator", nat_intl_col_ctor, 0, "Intl.Collator");
    js_getter(J, p, "compare", nat_intl_col_compare);
    js_method(J, p, "resolvedOptions", nat_intl_col_resolved, 0);

    p = intl_class(J, intl, "ListFormat", nat_intl_lf_ctor, 0, "Intl.ListFormat");
    js_method(J, p, "format", nat_intl_lf_format, 1);
    js_method(J, p, "formatToParts", nat_intl_lf_parts, 1);
    js_method(J, p, "resolvedOptions", nat_intl_lf_resolved, 0);

    p = intl_class(J, intl, "Segmenter", nat_intl_seg_ctor, 0, "Intl.Segmenter");
    js_method(J, p, "segment", nat_intl_seg_segment, 1);
    js_method(J, p, "resolvedOptions", nat_intl_seg_resolved, 0);
    intl_p_segments = js_object_with(J, JO_PLAIN, J->p_object);
    intl_p_seg_iter = js_object_with(J, JO_PLAIN, J->p_object);
    if (intl_p_segments) {
        js_method(J, intl_p_segments, "containing", nat_intl_segments_containing, 1);
        js_method_key(J, intl_p_segments, J->sym_iterator, "[Symbol.iterator]", nat_intl_segments_iter, 0);
    }
    if (intl_p_seg_iter) {
        js_method(J, intl_p_seg_iter, "next", nat_intl_seg_iter_next, 0);
        js_method_key(J, intl_p_seg_iter, J->sym_iterator, "[Symbol.iterator]", nat_intl_seg_iter_self, 0);
        js_tag(J, intl_p_seg_iter, "Segmenter String Iterator");
    }

    p = intl_class(J, intl, "DisplayNames", nat_intl_dn_ctor, 2, "Intl.DisplayNames");
    js_method(J, p, "of", nat_intl_dn_of, 1);
    js_method(J, p, "resolvedOptions", nat_intl_dn_resolved, 0);

    p = intl_class(J, intl, "Locale", nat_intl_loc_ctor, 1, "Intl.Locale");
    js_getter(J, p, "language", nat_intl_loc_language);
    js_getter(J, p, "script", nat_intl_loc_script);
    js_getter(J, p, "region", nat_intl_loc_region);
    js_getter(J, p, "baseName", nat_intl_loc_tostring);
    js_method(J, p, "toString", nat_intl_loc_tostring, 0);
    js_method(J, p, "maximize", nat_intl_loc_maximize, 0);
    js_method(J, p, "minimize", nat_intl_loc_minimize, 0);

    js_declare_flags(J, J->global, js_str(J, "Intl"), js_from_obj(intl), JP_WRITE | JP_CONF);
}
