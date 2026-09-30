/* Proxy: an object that asks its handler what to do each time something is
 * done to it, and does it to its target when the handler has nothing to say.
 *
 * Libraries use them for what they cannot do otherwise -- a store that knows
 * which of its values were read, an object whose every name has a value --
 * and scripts ask whether there is one before anything else: Ars Technica's
 * and Yahoo's stopped at "Proxy is not defined".
 *
 * A proxy is an ordinary object with JOF_PROXY set, its target in `data` and
 * its handler in `data2` -- the fields a native keeps what it carries in -- so
 * a proxy of a function is a native, and calling it or `new` on it goes
 * through the natives' door (js_proxy_call). Everything else looks for the
 * flag: reading, writing, `in`, delete, listing keys, a property's
 * descriptor, defining one, the prototype, and Array.isArray.
 *
 * Not done: the invariants the standard holds a handler to (a trap may say a
 * property that cannot change is not there, and be believed); the
 * isExtensible, preventExtensions and setPrototypeOf traps, which are the
 * target's own; and a proxy as an ordinary object's prototype, when written
 * through, goes on up past it unless it has a set trap. Keys are listed with
 * their values read through the get trap, which a trap that counts what is
 * read will see. */
#pragma once

static jval jp_key(jstr *key) { return js_is_sym_key(key) ? js_from_sym(key) : js_from_str(key); }

/* The handler's trap by this name, or undefined when it has none. A revoked
   proxy has no handler, and anything done to it is an error. */
static jval jp_trap(jctx *J, jobj *p, const char *name, u32 len, int *revoked) {
    *revoked = 0;
    if (p->data2.t != JS_OBJ || p->data.t != JS_OBJ) {
        *revoked = 1;
        js_throw(J, JS_ERR_TYPE, "a proxy that has been revoked", J->error_line);
        return js_undef();
    }
    jval t = js_get(J, p->data2, js_intern(J, name, len));
    if (J->sig != JS_OK) { *revoked = 1; return js_undef(); }
    return js_callable(t) ? t : js_undef();
}

static jval js_proxy_get(jctx *J, jobj *p, jstr *key, jval receiver) {
    int rv;
    jval trap = jp_trap(J, p, "get", 3, &rv);
    if (rv) return js_undef();
    if (trap.t == JS_UNDEF) return js_getv(J, p->data, key, receiver);
    jval args[3] = { p->data, jp_key(key), receiver };
    return js_call(J, trap, p->data2, args, 3);
}

static void js_proxy_set(jctx *J, jobj *p, jstr *key, jval v, jval receiver) {
    int rv;
    jval trap = jp_trap(J, p, "set", 3, &rv);
    if (rv) return;
    if (trap.t == JS_UNDEF) {
        /* Written on the target; a setter there sees the proxy as `this`. */
        js_putv(J, p->data, key, v, (receiver.t == JS_OBJ && receiver.obj == p) ? receiver : p->data);
        return;
    }
    jval args[4] = { p->data, jp_key(key), v, receiver };
    js_call(J, trap, p->data2, args, 4);
}

/* A proxy met on the way up from an object being written: its set trap, if
   it has one, decides; without one the walk goes on past it. */
static int js_proxy_set_inherited(jctx *J, jobj *p, jstr *key, jval v, jval receiver) {
    int rv;
    jval trap = jp_trap(J, p, "set", 3, &rv);
    if (rv) return 1;
    if (trap.t == JS_UNDEF) return 0;
    jval args[4] = { p->data, jp_key(key), v, receiver };
    js_call(J, trap, p->data2, args, 4);
    return 1;
}

static int js_proxy_has(jctx *J, jobj *p, jstr *key) {
    int rv;
    jval trap = jp_trap(J, p, "has", 3, &rv);
    if (rv) return 0;
    if (trap.t == JS_UNDEF) return js_has(J, p->data.obj, key);
    jval args[2] = { p->data, jp_key(key) };
    jval r = js_call(J, trap, p->data2, args, 2);
    return J->sig == JS_OK && js_to_bool(r);
}

static int js_proxy_delete(jctx *J, jobj *p, jstr *key) {
    int rv;
    jval trap = jp_trap(J, p, "deleteProperty", 14, &rv);
    if (rv) return 0;
    if (trap.t == JS_UNDEF) return js_delete(J, p->data, key);
    jval args[2] = { p->data, jp_key(key) };
    jval r = js_call(J, trap, p->data2, args, 2);
    return J->sig == JS_OK && js_to_bool(r);
}

/* A property of the proxy's own, as getOwnPropertyDescriptor says it is:
   the value, or the accessor, and what may be done with it. */
static int js_proxy_get_own(jctx *J, jobj *p, jstr *key, jval *v, int *flags) {
    int rv;
    jval trap = jp_trap(J, p, "getOwnPropertyDescriptor", 24, &rv);
    if (rv) return 0;
    if (trap.t == JS_UNDEF) return js_get_own(J, p->data.obj, key, v, flags);
    jval args[2] = { p->data, jp_key(key) };
    jval d = js_call(J, trap, p->data2, args, 2);
    if (J->sig != JS_OK || !js_is_obj(d)) return 0;

    int fl = 0;
    if (js_to_bool(js_get(J, d, J->s_enumerable))) fl |= JP_ENUM;
    if (js_to_bool(js_get(J, d, J->s_configurable))) fl |= JP_CONF;
    jval get = js_get(J, d, J->s_get), set = js_get(J, d, J->s_set);
    if (get.t != JS_UNDEF || set.t != JS_UNDEF) {
        jacc *a = (jacc *)js_alloc(J, (u32)sizeof(jacc));
        if (!a) return 0;
        a->get = get;
        a->set = set;
        v->t = JS_ACC;
        v->acc = a;
    } else {
        *v = js_get(J, d, J->s_value);
        if (js_to_bool(js_get(J, d, J->s_writable))) fl |= JP_WRITE;
    }
    *flags = fl;
    return J->sig == JS_OK;
}

/* The keys, as ownKeys gives them or as the target has them, each made a
   property the lists that ask for them can read: its value through the get
   trap, and whether it is listed from its descriptor. */
static u32 js_proxy_keys_of(jctx *J, jobj *p, jprop ***out, int want) {
    *out = 0;
    int rv;
    jval trap = jp_trap(J, p, "ownKeys", 7, &rv);
    if (rv) return 0;

    jobj *keys = js_array(J);
    if (!keys) return 0;
    if (trap.t != JS_UNDEF) {
        jval r = js_call(J, trap, p->data2, &p->data, 1);
        if (J->sig != JS_OK) return 0;
        jval len = js_get(J, r, J->s_length);
        u32 n = (u32)js_to_num(J, len);
        for (u32 i = 0; i < n && J->sig == JS_OK; i++) {
            jval k = js_get_index(J, r, i);
            if (k.t == JS_STR || k.t == JS_SYM) js_arr_push(J, keys, k);
        }
    } else {
        jobj *t = p->data.obj;
        if (t->kind == JO_ARRAY || t->kind == JO_ARGS || t->kind == JO_TYPED) {
            u32 len = t->kind == JO_TYPED ? js_ta_length(t) : t->len;
            for (u32 i = 0; i < len; i++) js_arr_push(J, keys, js_from_str(js_to_key(J, js_num(i))));
        }
        jprop **own;
        u32 nown = js_keys_of(J, t, &own, JK_STR | JK_SYM);
        for (u32 i = 0; i < nown; i++) js_arr_push(J, keys, jp_key(own[i]->key));
    }
    if (!keys->len || J->sig != JS_OK) return 0;

    jprop *props = (jprop *)js_alloc(J, (u32)sizeof(jprop) * keys->len);
    jprop **list = (jprop **)js_alloc(J, (u32)sizeof(jprop *) * keys->len);
    if (!props || !list) return 0;
    u32 at = 0;
    for (u32 i = 0; i < keys->len && J->sig == JS_OK; i++) {
        jval k = keys->items[i];
        jstr *key = k.str;
        if (js_is_sym_key(key) ? !(want & JK_SYM) : !(want & JK_STR)) continue;
        /* What getOwnPropertyDescriptor says it is, getter and all: reading
           each value through the get trap ran getters a listing must not,
           Alpine's warnings among them. Keys asked for without their
           enumerability (Reflect.ownKeys) are listed whether or not there
           is a descriptor, as the trap gave them. */
        jval dv = js_undef();
        int fl = 0;
        int have = js_proxy_get_own(J, p, key, &dv, &fl);
        if (J->sig != JS_OK) break;
        if ((want & JK_ENUM) && (!have || !(fl & JP_ENUM))) continue;
        props[at].key = key;
        props[at].v = have ? dv : js_undef();
        props[at].flags = have ? fl : 0;
        props[at].next = props[at].onext = 0;
        list[at] = &props[at];
        at++;
    }
    *out = list;
    return at;
}

static int js_proxy_define(jctx *J, jobj *p, jstr *key, jval desc) {
    int rv;
    jval trap = jp_trap(J, p, "defineProperty", 14, &rv);
    if (rv) return 0;
    if (trap.t == JS_UNDEF) return js_define_from_desc(J, p->data.obj, key, desc);
    jval args[3] = { p->data, jp_key(key), desc };
    jval r = js_call(J, trap, p->data2, args, 3);
    return J->sig == JS_OK && js_to_bool(r);
}

static jval js_proxy_getproto(jctx *J, jobj *p) {
    int rv;
    jval trap = jp_trap(J, p, "getPrototypeOf", 14, &rv);
    if (rv) return js_undef();
    if (trap.t == JS_UNDEF) {
        jobj *t = p->data.obj;
        if (t->flags & JOF_PROXY) return js_proxy_getproto(J, t);
        return t->proto ? js_from_obj(t->proto) : js_null();
    }
    return js_call(J, trap, p->data2, &p->data, 1);
}

/* Whether this is an array, as Array.isArray asks: a proxy is one when what
   it stands for is. */
static int js_proxy_is_array(jctx *J, jobj *p) {
    int depth = 0;
    while (p && (p->flags & JOF_PROXY) && depth++ < 1000) {
        if (p->data.t != JS_OBJ) {
            js_throw(J, JS_ERR_TYPE, "a proxy that has been revoked", J->error_line);
            return 0;
        }
        p = p->data.obj;
    }
    return p && p->kind == JO_ARRAY;
}

/* A proxy of a function, called or given to `new`. */
static jval js_proxy_call(jctx *J, jval this_val, jval *argv, int argc) {
    jobj *p = J->callee;
    jval nt = J->new_target;
    int rv;
    jval trap = jp_trap(J, p, nt.t != JS_UNDEF ? "construct" : "apply",
                        nt.t != JS_UNDEF ? 9 : 5, &rv);
    if (rv) return js_undef();
    jval target = p->data;
    if (trap.t == JS_UNDEF) {
        if (nt.t != JS_UNDEF) return js_construct(J, target, argv, argc, nt);
        return js_call(J, target, this_val, argv, argc);
    }
    jobj *list = js_array(J);
    if (!list) return js_undef();
    for (int i = 0; i < argc; i++) js_arr_push(J, list, argv[i]);
    if (nt.t != JS_UNDEF) {
        jval args[3] = { target, js_from_obj(list), nt };
        jval r = js_call(J, trap, p->data2, args, 3);
        if (J->sig == JS_OK && !js_is_obj(r))
            return js_throw(J, JS_ERR_TYPE, "a proxy's construct gave back something that is not an object",
                            J->error_line);
        return r;
    }
    jval args[3] = { target, this_val, js_from_obj(list) };
    return js_call(J, trap, p->data2, args, 3);
}

static jobj *js_proxy_new(jctx *J, jval target, jval handler) {
    jobj *p;
    if (js_callable(target)) {
        p = js_native_n(J, "", js_proxy_call, 0);
        if (!p) return 0;
        if (!js_is_constructor(J, target)) p->flags |= JOF_NOCTOR;
    } else {
        p = js_object_with(J, JO_PLAIN, 0);
        if (!p) return 0;
    }
    p->flags |= JOF_PROXY;
    p->data = target;
    p->data2 = handler;
    p->proto = target.obj->proto;
    return p;
}

static jval nat_proxy_make(jctx *J, jval t, jval *a, int n) {
    (void)t;
    if (J->new_target.t == JS_UNDEF)
        return js_throw(J, JS_ERR_TYPE, "Proxy has to be called with new", J->error_line);
    jval target = js_arg(a, n, 0), handler = js_arg(a, n, 1);
    if (!js_is_obj(target) || !js_is_obj(handler))
        return js_throw(J, JS_ERR_TYPE, "a proxy needs an object for its target and one for its handler",
                        J->error_line);
    jobj *p = js_proxy_new(J, target, handler);
    return p ? js_from_obj(p) : js_undef();
}

static jval nat_proxy_revoke(jctx *J, jval t, jval *a, int n) {
    (void)t; (void)a; (void)n;
    jobj *self = J->callee;
    if (js_is_obj(self->data)) {
        jobj *p = self->data.obj;
        p->data = js_null();
        p->data2 = js_null();
        self->data = js_null();
    }
    return js_undef();
}

static jval nat_proxy_revocable(jctx *J, jval t, jval *a, int n) {
    (void)t;
    jval target = js_arg(a, n, 0), handler = js_arg(a, n, 1);
    if (!js_is_obj(target) || !js_is_obj(handler))
        return js_throw(J, JS_ERR_TYPE, "a proxy needs an object for its target and one for its handler",
                        J->error_line);
    jobj *p = js_proxy_new(J, target, handler);
    jobj *revoke = js_native_n(J, "", nat_proxy_revoke, 0);
    jobj *r = js_object(J, JO_PLAIN);
    if (!p || !revoke || !r) return js_undef();
    revoke->data = js_from_obj(p);
    js_put_prop(J, r, js_str(J, "proxy"), js_from_obj(p));
    js_put_prop(J, r, js_str(J, "revoke"), js_from_obj(revoke));
    return js_from_obj(r);
}

static void js_proxy_init(jctx *J) {
    jobj *proxy = js_ctor(J, "Proxy", nat_proxy_make, 2, 0);
    if (proxy) js_method(J, proxy, "revocable", nat_proxy_revocable, 2);
}
