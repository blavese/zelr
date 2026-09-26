# 11 -- The web browser (everything except JS-engine internals and image decoders)

Source tree: the repository root (zelr main, 2026-09-22, two commits after v0.37.0). Static reading only: nothing was built or run. Every `file:line` below was checked against the tree. "Verified" in section 10 means verified by reading the code, not by running it.

---

## 1. Scope

| File | Lines | Role |
|---|---|---|
| `userland/browser.c` | 1746 | The browser program: window, chrome, main loop, page build (parse → sheets → index → pictures → scripts → layout), drawing, forms, history, find, status line. |
| `userland/web.h` | 299 | `url_t`, bounded string helpers, URL parse/print/join, the address-or-search rule, query escaping. |
| `userland/fetch.h` | 760 | HTTP/1.1 client over the kernel's sockets: request building, the three body-end modes, dechunking, gzip, keep-alive with retry-once, redirects, an in-memory cookie jar. |
| `userland/html.h` | 445 | Tag table (`T_*`, `HTML_TAGS`), void tags, entity + UTF-8 decoding with ASCII transliteration, raw-text end finder. Also a flat tokenizer (`html_parse`, `hdoc`) that nothing uses any more. |
| `userland/dom.h` | 698 | The document tree (`ddoc`/`dnode`), arena, attributes, tree-building parser with implied-end rules and a whitespace rule, tree mutation used by scripts. |
| `userland/css.h` | 1468 | CSS: colours, lengths, property table, selector and declaration parser, shorthands, at-rules, matching, cascade (specificity then order), inheritance, rule index, and the UA stylesheet `CSS_UA`. |
| `userland/layout.h` | 1416 | Styled tree → flat display list (`ldoc`): block/inline/flex layout, positioned boxes, line breaking against the 33 browser faces, form-control sizing, hit testing. |
| `userland/layouttest.c` | 515 | Headless ring-3 test of layout coordinates (flex, box-sizing, min/max, absolute/relative, `<center>`, pseudo-element rules, big lengths, vw, % height). Prints `LAYOUTTEST_PASS/FAIL`. |
| `userland/pagetest.c` | 639 | Headless ring-3 test of dom.h + jsdom.h: parse, attributes, whitespace, scripts, events, tree mutation, classList, timers, fields, querySelector. Prints `PAGETEST_PASS/FAIL`. |
| `tools/browsercheck.py` | 404 | QEMU + desktop + real HTTP server: draws pages, follows a link, goes back, chunked vs measured, sheets, PNG, missing picture, SVG, script, redirect, 404, https-to-a-plain-server. Asserts by pixel colour and by render equality. |
| `tools/formcheck.py` | 341 | QEMU: finds controls by colour, types, ticks, submits GET and POST; the server records what arrived. Checks rounded field corners. |
| `tools/webcheck.py` | 170 | QEMU: the **kernel shell's** `fetch` command (kernel/http.c) against the test server on e1000, pcnet, an undriven ne2k, and an instant-answer path. Does **not** exercise userland `fetch.h`. |
| `tools/livecheck.py` | 154 | QEMU: the `/live` page -- timer, external script, XHR, click listener -- each proven by a colour; `/live-quiet` is the control. |

Read for the interface only (or as test infrastructure):

| File | Lines | Why |
|---|---|---|
| `userland/jsdom.h` | 1314 | Read fully: the DOM bindings, events, timers, XHR, script running -- the whole browser↔engine interface. |
| `userland/js.h` | 447 | Read lines 1–260: header (what the engine has/has not), limits, value/object types, `jctx` with the `host_get`/`host_set` hooks, region allocator. |
| `tools/webserver.py` | 633 | The test server every browser check talks to (routes listed in §8). |
| `userland/wiretest.c` 185 + `tools/wirecheck.py` 90 | -- | The only tests that drive `fetch.h` directly (gzip, cookies, keep-alive counted by the server, three sockets at once). |
| `tools/findcheck.py` 167, `tools/piccheck.py` 80, `tools/ring3check.py` 108 | -- | Find highlight check; runners for layouttest/pagetest. |
| `userland/facetext.h` (generated, 1.7 MB), `userland/face.h` | -- | Only the face table (`TFACE_SIZES 33`, `face_t`, `face_glyph`, `FACE_FIRST 32`..`FACE_LAST 126`). |
| `userland/ui.h`, `userland/draw.h` | -- | Widgets/primitives the browser calls (`ui_field`, `ui_field_key`, `ui_field_draw`, `ui_button*`, `ui_toolbar`, `ui_statusbar`, `ui_well`, `ui_sunken`, `ui_scrollbar`, `ui_round`, `rect`, `round_rect`, `disc`, `mix`). |
| `sdk/zelr.h` 120–200, 415–475, 820–930 | -- | Socket, TLS-status, window, find and key syscalls. |
| `kernel/syscall.c` 700–900 | -- | What `connect`/`connect_tls`/`send`/`recv`/`disconnect`/`tls_status` actually do (socket table, one-TLS-socket rule, 1400/8192-byte send cap, 4 s recv timeout). |

---

## 2. Big picture

### 2.1 What the area is

`browser.c` is a ring-3 program (`/bin/browser`, embedded in the kernel image via `kernel/builtin.c:31,87`, launcher entry `kernel/wm.c:400`). Everything else in scope is header-only (`static inline`) code compiled into it (and into the test programs). Built by `userland/build.sh` with `zig cc -target x86_64-freestanding-none -O2 -std=gnu11 -ffreestanding -nostdlib -fno-builtin -mcmodel=large -mno-red-zone`; unlike the kernel, ring 3 may use floating point (the JS engine needs doubles).

The end-to-end pipeline (one page):

```
address bar text
  └─ go_or_search (browser.c:1308)
       url_looks_like_address (web.h:127) ? set_address(typed)
                                          : set_search → "https://lite.duckduckgo.com/lite/?q=" + url_escape(typed)
  └─ load(address) (browser.c:865)
       url_parse (web.h:173)           bare name ⇒ https:443; "http://" ⇒ :80
       web_get/web_post (fetch.h:753/757) → web_send: ≤6 hops of redirects
         web_fetch: one retry on a fresh socket if a *reused* socket gave nothing back
           web_fetch_once: connect / connect_tls (kernel), request, recv loop,
                           length | chunked | close, dechunk, gunzip (inflate.h), cookies
       text/plain or application/json ⇒ show_plain (one <pre>)
       otherwise ⇒ build (browser.c:769)
         jsdom_close; dom_parse (dom.h:486)      tree, implied ends, whitespace rule
         css_init; CSS_UA; accent_sheet; every <style>; ≤6 <link rel=stylesheet>;
         every style="" (gather_inline_styles); css_index
         gather_pictures (≤24 <img>; PNG/JPEG by magic bytes, else SVG)
         jsdom_open → scripts in node order (external ones fetched synchronously) →
                      DOMContentLoaded, load (to document listeners)
         relayout → lay_run (layout.h:1348); if no words came out but the DOM has words,
                    lay out again ignoring visibility (lay_show_hidden)
         publish_text (for the desktop's find)
       status line: counts + ", encrypted" / ", NOT encrypted"; maybe follow <meta refresh>
main loop (browser.c:1350): events → keys → timers → one XHR → form navigation →
  scroll → (if dirty) load / hover relayout / JS click / controls / links / chrome / draw_page
```

### 2.2 Design decisions and their stated reasons

- **Fixed buffers, decided once, reused for every page** (browser.c:40-57): "a browser that allocated per page would be a browser whose failure to show one depended on which one it showed before". Sized from measured pages (a 670 KB encyclopaedia article; 240 KB ordinary article): `SRC_MAX` became 1 MiB after a 320 KB buffer turned a compressed page into "the fetch failed". The JS engine does allocate (quarter-megabyte region chunks), as do gunzip and the image decoders.
- **One buffer for the whole response** (fetch.h:426-431): headers and body share `buf`; the body is a pointer into it. (The comment's reason, "a program here has no allocator", is stale -- see §10.)
- **Three body-end modes decided by headers, not by sniffing** (fetch.h:1-14): Content-Length, chunked, or connection close; "a client that only knows one of them loses the end of about half the pages on the web".
- **Keep-alive with retry-once** (fetch.h:432-450): a page with a dozen pictures used to pay a dozen (TLS) handshakes. The connection is kept only when the response was self-delimiting and not `Connection: close`. A failure on a reused connection is retried once, "only when nothing came back at all, because a request the server answered and then dropped may have been acted on, and asking again is how somebody orders twice."
- **https by default for a bare name** (web.h:97-106): guessing http would send the typed address in the clear before the redirect to https. Scheme-relative links keep the page's scheme so an encrypted page's links are never "quietly taken down to http" (web.h:229-232).
- **Search goes to DuckDuckGo Lite, not Google** (browser.c:1270-1291): Google's `/search` result page arrives with no results in it and is built by script; DDG Lite is plain HTML that renders. "A limitation stated rather than a preference."
- **ASCII transliteration** (html.h:96-106): the typefaces cover printable ASCII only; curly quotes, dashes, accents are written the way they were before Unicode ("a reader loses the shape of the mark rather than the sense of it"). Unknown code points become `?`.
- **A real tree with browser-style implied-end rules** (dom.h:1-26, 362-368): `<p>` ends at a block, `<li>` at the next `<li>`, etc.; a stray close tag for something not open is dropped "because that is how one stray `</div>` in a footer swallows the rest of a document."
- **Whitespace is content only between inline things** (dom.h:502-520): dropped at the start of a container and after a block; otherwise one `" "` text node is kept (the Google nav read "GmailImages" when all whitespace was dropped).
- **The UA stylesheet is a stylesheet** (css.h:1416-1422): parsed first so every page rule beats it by ordinary cascade. A second tiny sheet `a{color:#<theme accent>}` sits between UA and page (browser.c:681-702).
- **Rule index by rightmost key** (css.h:1293-1304): rules filed under the id, first class, or tag of their rightmost compound (or "universal"), so an element only tests rules that could match -- "turns the tens of millions into tens".
- **Lengths kept as written, resolved at layout** (css.h:82-85), in hundredths in an `int` (css.h:99-107: shorts overflowed at 327.67 and broke `width:960px`).
- **Transparency composited against white** (css.h:213-219), since nothing composites.
- **One-pass layout into a flat display list in document coordinates** (layout.h:1-20); drawing and scrolling are "a subtraction". Floats, grid, table column widths are deliberately absent ("a browser that does half of them puts things in places nobody chose"). (The same comments also still list positioning and flex as absent -- they are now implemented; see §10.)
- **Inline boxes have left/right edges but not top/bottom** (layout.h:544-562): left/right padding/border/margin separate links; vertical padding "does not move the line it is on"; a box that wraps across lines draws nothing rather than a band.
- **Flex by measure-then-lay-out** (layout.h:825-847): measure each child by laying it out and throwing that away -- "exactly right, because the thing being measured is the thing that will be drawn".
- **Positioned boxes** (layout.h:1055-1078): relative = lay out then shift items; absolute = from nearest positioned ancestor, takes no space; fixed = treated like absolute against the page ("the closer of the two wrong answers"); bottom-only boxes stay where the flow put them.
- **Pages that hide themselves until a framework runs** (layout.h:48-64; browser.c:666-678): if a layout produced no words but the DOM has words, lay out again ignoring `visibility`/`opacity`, and say so.
- **Form control values live in the DOM as attributes** (browser.c:149-158): layout, drawing, scripts and submission read the same place ("two answers that agree until somebody types").
- **A click goes to the page first** (browser.c:1594-1601): handlers can `preventDefault`, and then neither control nor link acts.
- **The script world lives as long as the page** (jsdom.h:26-42), costs nothing for a page with no script and no `on*` attribute; XHR is performed on the browser's *next pass* so code after `send()` runs before `onload` (jsdom.h:759-764).
- **"encrypted" / "NOT encrypted" in words** (browser.c:20-25, 1006-1010): a padlock reads as a promise about the site; the code only promises the bytes came from whoever holds the typed name. Marking only the encrypted case "trains people to read a missing mark as nothing in particular".
- **Find reads what was laid out** (browser.c:221-236): the desktop cannot read pixels, so the browser publishes the laid-out words (`win_set_text`) and re-finds the n-th match itself to scroll and highlight.


---

## 3. File-by-file detail

### 3.1 `userland/web.h` (299 lines) -- URLs

Purpose (web.h:1-6): "everything here is bytes in and bytes out, with no screen anywhere in it."

**Constants**: `URL_HOST 128` (10), `URL_PATH 640` (11), `URL_TEXT (URL_HOST + URL_PATH + 24)` = 792 (12). `URL_HOST` matches the kernel's 128-byte host copy in `sys_connect` (kernel/syscall.c:748).

**Type** `url_t` (14-19): `char host[URL_HOST]` (no port), `char path[URL_PATH]` (path + query, never a fragment; always starts with `/` except the bug in §10), `int port`, `int secure` (1 = https/TLS).

**String helpers** (21-60, "written out rather than pulled in" so bounds are obvious):
- `w_len(s)` (29); `w_copy(dst, cap, src, n)` (35) copies ≤ min(n, cap-1) chars, stops at NUL, always terminates; `w_same(a,b)` (41) exact equality; `w_lower(c)` (46) ASCII; `w_same_fold(a,b)` (51) case-insensitive **whole-string** equality; `w_starts_fold(s,prefix)` (56) case-insensitive prefix.
- `url_copy(dst, src)` (68): field-by-field, because "a struct assignment of this size becomes a call to memcpy, and there is no library here to have one in" (62-67). The same reasoning recurs in layout.h:855-859.

**`url_text(u, out, cap)`** (80): `http(s)://` + host + `:port` only when not the scheme default (and `n < cap-8`) + path. Used for the address bar, history, hover status, meta-refresh comparison.

**`url_looks_like_address(in)`** (127) -- the address-or-search rule (107-126), in order:
1. skip leading spaces; empty → search (0);
2. starts with `http://`, `https://` or `//` → address;
3. contains a space or tab anywhere → search;
4. starts with `/` → address; starts with `localhost` (prefix test) → address;
5. the **last** `.` before any `/`, `?`, `#` must not be the first character and must be followed by `[A-Za-z0-9]` → address; otherwise search.
So `zelr` → search, `zelr.com` → address, `10.0.2.2:8000/form` → address, but also `3.14` or `e.g` → address, and a trailing space turns `example.com ` into a search.

**`url_escape(in, out, cap)`** (157): keeps `A-Z a-z 0-9 - _ . ~`, everything else `%XX` (uppercase hex); space is `%20` ("one rule rather than two", 152-156). Stops when `w >= cap-4`. Used only for the search query. (Form submission uses a different encoder with `+` for space: browser.c:353.)

**`url_parse(in, out)`** (173):
1. skip spaces; `secure = 1` by default; strip `https://` (8), `http://` (7, `secure = 0`) or `//` (2);
2. host = everything up to `/`, `?`, `#` (stored up to 127 chars; scanning continues past); empty host → return 0;
3. port: at the first `:` in the host, digits only, accepted if 1..65535 else the scheme default; the host is cut at the `:` regardless (so IPv6 literals and `user:pass@` break);
4. path: a `/` is prepended only if the next char is not `/` **and not `?`**; copied up to `#` (≤ 639 chars); empty → `/`. Returns 1.

**`url_join(base, href, out)`** (222) -- link resolution:
- leading spaces skipped; empty or `#...` → a copy of `base` (so a fragment link re-fetches the same page; there is no in-page anchor navigation);
- `http://`/`https://` → `url_parse`;
- `//host/...` → parsed, then `secure = base->secure`; the port is reset to the scheme default unless a `:` appeared in the authority (229-243);
- any `:` in the first 12 chars before a `/` → return 0 ("a scheme this cannot speak", 245-249): rejects `javascript:`, `mailto:`, `data:`, `tel:`, but also relative references with an early colon;
- `/abs/path` → copied verbatim up to `#` (no dot-segment removal);
- otherwise: base path up to and including its last `/` (the scan covers the whole `path`, **including the query**), plus `href` up to `#`; then `/./` removed and `/../` pops a segment (276-296); empty → `/`.

### 3.2 `userland/fetch.h` (760 lines) -- HTTP client

Includes `zelr.h`, `alloc.h`, `inflate.h`, `web.h` (15-19).

**Errors** (21-30): `WEB_ERR_SCHEME -1`, `WEB_ERR_CONNECT -2`, `WEB_ERR_SEND -3`, `WEB_ERR_EMPTY -4`, `WEB_ERR_HEADERS -5` (no blank line), `WEB_ERR_TLS -6`, `WEB_ERR_DOWN -7`, `WEB_ERR_RESOLVE -8`, `WEB_ERR_BUSY -9`, `WEB_ERR_ENCODING -10`. `web_err_from(rc)` (35) maps kernel `NET_ERR_DOWN/RESOLVE/TLS/BUSY` (sdk/zelr.h:189-193: -2,-3,-5,-6) and folds everything else (incl. `NET_ERR_CONNECT -4`, `-1`) into `WEB_ERR_CONNECT`. `WEB_ERR_SCHEME` is never returned by fetch.h itself.

**`response_t`** (45-54): `int status`; `char *body` (points into the caller's buffer); `int len`; `int truncated` (buffer filled, or gunzip ran out of room); `char location[URL_TEXT]`; `char ctype[64]`; `int secure`; `char how[64]` (TLS parameters agreed, or why the handshake failed).

**Request helpers**: `wh_add(buf,cap,n,s)` (58) / `wh_add_num` (67) append or return -1 when full.

**Header readers**: `wh_header(head, hlen, name, out, cap)` (84): case-insensitive name at a line start followed by `:`, value with leading blanks skipped, up to CR/LF; first occurrence only. `wh_header_nth(..., nth)` (111): the nth occurrence (for `Set-Cookie`). `wh_number` (363) decimal or -1; `wh_hex` (370) hex prefix or -1.

**`wh_split(buf, len, *skip)`** (387): first `\n\n` (skip 2) or `\r\n\r\n` (skip 4); bare-LF servers accepted ("not broken enough to refuse to talk to").

**`wh_dechunk(body, len)`** (399): in place ("every byte written is behind the byte being read"); chunk extensions ignored (skips to LF); size 0 ends; a short last chunk is kept ("keep what there is"); returns length, or -1 if the first size did not parse (the caller treats ≤ 0 as 0). Trailers are not parsed (irrelevant: size 0 returns immediately).

**`wh_gunzip(body, len, cap, *cut)`** (151): checks `1F 8B 08`, skips the 10-byte header, `FEXTRA` (flag 4), `FNAME` (8), `FCOMMENT` (16), `FHCRC` (2); the CRC/ISIZE trailer is ignored "because what a checksum would catch the transport has already checked" (137-150). `malloc(cap)` for output, `inf_run` (inflate.h:279), copy back, `free`. `INF_FULL` ⇒ keep the front and set `*cut`; **any other error, including `INF_TRUNCATED`, ⇒ -1** (176). Only `Content-Encoding` values starting with `gzip` are decoded (687-688); the request advertises `gzip` only (532). No `deflate`, no `br`.

**Cookie jar** (184-361). Limits (196-200): `CK_MAX 64`, `CK_NAME 96`, `CK_VALUE 768`, `CK_DOMAIN = URL_HOST (128)`, `CK_PATH 160`. `cookie_t` (202-209): `name, value, domain, path, secure, used`. Global `ck_jar[CK_MAX]` (211) -- memory only, gone when the browser exits ("a decision and not half a job", 190-192). `ck_forget_all()` (213) exists but is never called.
- `ck_domain_ok(host, domain)` (219): case-insensitive suffix on a label boundary; empty domain never matches.
- `ck_path_ok(path, cpath)` (228): empty or `/` matches all; else prefix with a boundary (`\0`, `/`, or cpath ending in `/`).
- `ck_take_one(u, line)` (243): `name=value` (name ≤ 95, value ≤ 767, trailing spaces trimmed; no name or no `=` ⇒ ignored). Defaults: domain = request host, path = `/`. Attributes: `Secure`; `Domain` (leading `.` stripped; accepted only if `ck_domain_ok(host, d)` -- so a host can widen to any parent, even a bare TLD); `Path` (only if it starts with `/`); `Max-Age` starting with `0` or `-` ⇒ delete; `Expires` containing `1970` ⇒ delete (the only date check, 303-308). `HttpOnly`/`SameSite` ignored. Same (name, domain, path) ⇒ update value or delete; else take the first free slot; a full jar silently drops new cookies.
- `ck_take(u, head, hlen)` (332): up to 16 `Set-Cookie` headers per response (on every response, including redirects and errors).
- `ck_header(u, out, cap)` (342): all matching cookies (secure only over https) as `a=1; b=2`, stopping when full.

**Keep-alive state** (451-469): `ka_host[URL_HOST]`, `ka_port`, `ka_secure`, `ka_live`, `ka_sock = -1`. Exactly one kept connection, by design ("what the several [sockets] are for is the browser asking for a page and its pictures at once, which is a layer above this one", 454-458 -- not implemented). `web_drop()` (461) disconnects; `ka_matches(u)` (466) = live && same port, scheme, host (case-folded).

**`web_fetch_once(u, body, buf, cap, r)`** (474) -- one exchange:
1. Reset `*r`.
2. Connection: reuse if `ka_matches` (for TLS, `r->how = tls_what`); else `web_drop()` then `connect_tls(host, port)` (the handshake verifies the certificate against `u->host`, 491-493) or `connect(host, port)`. On `NET_ERR_TLS` only, `r->how = tls_why` (496-499).
3. Request (512-551) in `req[URL_PATH + URL_HOST + CK_VALUE + 512]` (2048 B):
   ```
   GET|POST <path> HTTP/1.1\r\n
   Host: <host>[:<port> only if not the scheme default]\r\n     (518-524: "Host: www.google.com:443" loops some servers)
   User-Agent: zelr\r\n
   Accept: text/html,text/plain,*/*\r\n
   Accept-Encoding: gzip\r\n
   Connection: keep-alive\r\n
   [Cookie: <jar>\r\n]
   [Content-Type: application/x-www-form-urlencoded\r\nContent-Length: <strlen(body)>\r\n]
   \r\n
   ```
   Overflow ⇒ `WEB_ERR_SEND`. Sent in ≤ 1400-byte pieces (554-562; the kernel refuses plain sends > 1400 and TLS sends > 8192, kernel/syscall.c:840), then the body in the same pieces (564-577).
4. Receive loop (579-630): reads ≤ 32768 bytes per `recv` into `buf + total`, always leaving room for a NUL; `room <= 0` ⇒ `truncated = 1`, stop. `NET_EOF` (-2) or any negative ⇒ stop. `0` (the kernel's 4000 ms recv timeout expired, kernel/syscall.c:860,869) ⇒ count; **three in a row** ⇒ stop ("a server that has stopped talking without saying so"). Once headers are complete: `Transfer-Encoding` starting `chunked` ⇒ chunked; else `Content-Length` ⇒ `want`. Stop as soon as `total - hlen - skip >= want`, or when a chunked buffer **ends with** `0\r\n\r\n` (617-629).
5. Keep decision (635-651): keep iff headers were found, the body was self-delimiting (`want >= 0 || chunked`) and `Connection` does not start with `close`; else `disconnect`.
6. Parse (653-682): `total == 0` ⇒ `WEB_ERR_EMPTY`; no blank line ⇒ `WEB_ERR_HEADERS`; status = digits at bytes 9-11 when `buf[0] == 'H'`; `Location`, `Content-Type`, cookies; body = after headers; dechunk or trim to Content-Length; NUL-terminate.
7. gzip (686-699) as above; failure ⇒ `web_drop()`, `WEB_ERR_ENCODING`.
8. Return `status`, or `WEB_ERR_EMPTY` if no status was parsed.

**`web_fetch`** (706): `reused = ka_matches(u)`; on failure *of a reused connection* with `WEB_ERR_SEND`, `WEB_ERR_EMPTY` or `WEB_ERR_CONNECT`, drop and try **once** more on a fresh connection.

**`web_send(u, body, buf, cap, r)`** (722): up to `WEB_MAX_HOPS 6` (720) fetches. Returns immediately on an error or a non-redirect status. For 301/302/303/307/308: no `Location` ⇒ return the 3xx; `url_join` failure ⇒ return; a Location equal to the current URL (host, path, port **and scheme**) ⇒ return the 3xx (a loop is "reported rather than followed"; http→https on the same path is not a loop, 733-736); else `*u = next`. After a 301/302/303 a POST becomes a GET; 307/308 keep the body (742-748). After 6 redirects returns the last 3xx. `*u` ends as the final URL (the browser uses this for the address bar and relative links).

**`web_get(u, buf, cap, r)`** (753) = `web_send(u, 0, ...)`; **`web_post(u, body, ...)`** (757) = `web_send(u, body ? body : "", ...)`.

**Syscalls used** (sdk/zelr.h): `connect` (425), `connect_tls` (443), `tls_what`/`tls_why` (450-451, via `SYS_TLS_STATUS`, machine-wide "last handshake"), `send` (452), `recv` (462, `NET_EOF -2`), `disconnect` (465); plus `malloc`/`free` (alloc.h). Kernel side (kernel/syscall.c:712-879): `SOCK_MAX = TCP_MAX` sockets checked against the owner pid; `sys_connect_tls` refuses with `NET_ERR_BUSY` while **any** secure socket is open machine-wide (786-787); a failed handshake closes the TCP connection (798-802); DNS and TCP connect time out at 6000 ms.

### 3.3 `userland/html.h` (445 lines) -- tags and characters

**Tags** (23-42): `T_OTHER 0`, then 64 known tags in this order: `html head body title h1..h6 p br hr div span a ul ol li dl dt dd b strong i em code pre kbd samp tt blockquote table tr td th caption script style img form input button textarea select option nav header footer section article main aside figure figcaption small noscript iframe svg label meta link hgroup center`; `T_COUNT = 65`. `HTML_TAGS[T_COUNT]` (65-77) holds the names in the same order (it is consistent, although the promised check "at the bottom of this file", 62-64, does not exist). Everything else (`tbody`, `thead`, `u`, `s`, `sup`, `sub`, `mark`, `abbr`, `picture`, `source`, `video`, `canvas`, `details`, `summary`, `template`, `fieldset`, `address`, `wbr`, …) is `T_OTHER`.

**Node kinds** `N_TEXT=1, N_OPEN, N_CLOSE` (21) -- only for the dead flat tokenizer.

- `html_tag_of(name, len)` (79): case-insensitive exact match by linear scan; `T_OTHER` if none.
- `html_void(t)` (91): `br hr img input meta link` only.
- `html_fold_cp(cp, out)` (107): code point → ASCII (writes 1-4 bytes). `< 0x80` itself; `0xC0-0xFF` via the table at 108-110 (`AAAAAAACEEEEIIIIDNOOOOOx0UUUUYPB` / `aaaaaaaceeeeiiiionooooo/0uuuuypy`: é→e, ñ→n, ß→B, Ø→0, ×→x, ÷→/); specific mappings (116-141): NBSP→space, ©→`(c)`, «→`<<`, ®→`(R)`, °→` deg`, ·→`-`, »→`>>`, ½→`1/2`, U+2010..2013→`-`, U+2014/2015→`--`, U+2018/2019/201B→`'`, U+201C/201D/201F→`"`, U+2022/25CF/25AA→`*`, U+2026→`...`, U+2039/203A→`<`/`>`, U+2122→`(TM)`, U+2190/2192→`<-`/`->`, U+2212→`-`, U+20AC→`EUR`, U+00A3→`GBP`, U+00A5→`JPY`; anything else `?`.
- `html_char(p, left, *used, out)` (153): one source character. `&` (if `left > 2`): numeric `&#123;`/`&#x7B;` (≤ 8 digits; none ⇒ literal `&`) or one of 36 named entities (178-195: `amp lt gt quot apos nbsp mdash ndash hellip lsquo rsquo ldquo rdquo copy reg trade bull middot laquo raquo times deg euro pound yen eacute egrave agrave ccedil uuml ouml auml szlig ntilde rarr larr`, matched exactly and case-insensitively on an alphanumeric run ≤ 12; unknown ⇒ literal `&`); the `;` is optional (212-214). UTF-8 2/3/4-byte sequences decoded (no overlong/surrogate checks); a bad lead or continuation byte ⇒ `?` consuming one byte. Everything then goes through `html_fold_cp`.
- `html_raw_end(p, len, at, name)` (298): offset of the first `</name` (case-insensitive, no check of what follows the name) at or after `at`, or `len`.
- **Dead code**: `hnode` (44-50), `hdoc` (52-59), `HTML_NODES 9000`, `HTML_ARENA 192K`, `ha_*` (245-260), `html_attr_value` (266), `html_parse` (309) -- a flat open/text/close tokenizer from before dom.h; no `.c` or other header references them (grep).

### 3.4 `userland/dom.h` (698 lines) -- the tree

**Limits** (32-40, "measured against real pages rather than picked": 5600 elements, 14000 attributes, 670 KB of HTML): `DOM_NODES 20000`, `DOM_ATTRS 40000`, `DOM_ARENA (1536*1024)`, `DOM_DEPTH 64` (parser stack).

**Types**:
- `DN_ELEMENT = 1, DN_TEXT` (42).
- `dattr {int name, value}` (47-49): arena offsets; names folded to lower case on the way in.
- `dnode` (51-58): `short kind, tag; int text` (DN_TEXT: its string; DN_ELEMENT with `T_OTHER`: its lower-cased name; else -1); `int attr_at, attr_n` (a contiguous run in `attrs`); `int parent, first, last, next, prev` (-1 = none).
- `ddoc` (60-73): `nodes[DOM_NODES], count, attrs[DOM_ATTRS], nattrs, arena[DOM_ARENA], used, root` (a synthetic `<html>` element, always index 0), `body` (first `<body>`, else `root`), `head`, `title` (arena offset or -1), `overflowed`. About 2.6 MB; the browser holds one static `doc`.

Nothing is freed: removed nodes keep their index and data; the whole arena is discarded when the next page is parsed (108-117).

**Arena**: `dom_put` (77, sets `overflowed` when full), `dom_str` (82), `dom_get` (89, unused).

**Nodes and mutation**: `dom_new(d, kind, tag)` (95); `dom_unlink` (118); `dom_contains(a, b)` (136: b is a or under a); `dom_append(parent, child)` (144: refuses `parent == child` and cycles -- "a ring is not a tree that terminates" -- and moves a node that is already attached); `dom_insert_before(parent, child, ref)` (161: a `ref` that is not a child of `parent` means append, as the DOM says); `dom_create_element(name, len)` (181: unknown names kept as text so `tagName` answers).

**Attributes**: `dom_attr_add` (196, only valid while the element is the newest run); `dom_attr(el, name)` (204: exact, first match, or 0); `dom_attr_fold` (225: case-insensitive name, added for SVG `viewBox`, 214-224); `dom_attr_set(el, name, value)` (254): rewrite in place if the new value fits, else a fresh arena string; if absent and the element's run is not last, **copy the run to the end** of `attrs` (old copy abandoned) and append -- needed for `el.className = ...` on an element without a class (249-253).

**Walking and text**: `dom_next(i, top)` (293) iterative pre-order within `top` ("a recursive walk on a fixed stack is how a browser meets a page built out of nested divs and stops", 288-292); `dom_text_content(el, out, cap)` (304) concatenates all text descendants (including script/style text); `dom_set_text(el, s, len)` (325) creates one new text node and sets `first = last = -1` **without unlinking** the old children; `dom_has_class(el, want, len)` (341) whole-word class test; `dom_has_words(d)` (407) any non-space text under body outside `script/style/textarea/title` (drives the hidden-page fallback).

**Tree-building rules**:
- `dom_is_block(t)` (369): `p div ul ol li dl dt dd h1-h6 hr blockquote table tr td th form nav header footer section article main aside figure figcaption pre caption hgroup body center`.
- `dom_closes(open, now)` (385): `P` by any block; `LI` by `LI`; `DT`/`DD` by `DT`/`DD`; `TD`/`TH` by `TD`/`TH`/`TR`; `TR` by `TR`; `OPTION` by `OPTION`. Only the **top** of the stack is tested (repeatedly).
- `dom_raw(t)` (399): `script style textarea title` -- content is one text node, decoded only for title/textarea (650).

**Parser state** `dparse {ddoc *d; int stack[DOM_DEPTH]; int depth}` (434-438); `dp_top` (440, `root` when empty), `dp_tag_top` (444), `dp_push` (449: beyond 64 sets `overflowed` and the element is simply not pushed), `dp_pop_to(tag)` (457: to the nearest open element with that tag; nothing if none), `dp_text(p, len, decode)` (465), `dp_all_space` (480).

**`dom_parse(d, p, len)`** (486):
1. Reset; `root = dom_new(DN_ELEMENT, T_HTML)`.
2. Text up to `<`: decoded. If all whitespace (502-536): dropped when the parent has no children yet or its last child is a block element; otherwise one `" "` text node. Else a text node on `dp_top`.
3. `<!-- ... -->` skipped; `<!...>`, `<?...>` skipped to `>`.
4. Close tag: name up to `>`/space; a known tag ⇒ `dp_pop_to`; **unknown ⇒ ignored**.
5. Open tag: name up to `>`, whitespace or `/`; unknown ⇒ lower-cased name stored; pop while `dom_closes(top, tag)`; create; every attribute (name lower-cased; value `"..."`, `'...'` or bare; entity-decoded; a bare attribute gets `""`); `self_closed` iff the byte before `>` is `/`; append to `dp_top`; remember the first `<body>`/`<head>`; raw elements consume to `</name` and skip past `>`; `<title>` sets `d->title` once; otherwise push unless void or self-closed.
6. `body = root` if there was no `<body>`.

Note: the synthetic `root` is a `T_HTML`; a page's own `<html>` becomes a second `T_HTML` child of it.

**Finding**: `dom_by_id(id)` (675) and `dom_by_tag(tag, nth)` (686) scan node indices 0..count (document order for parsed nodes; detached and script-created nodes included); `dom_tag_name(el)` (694).


### 3.5 `userland/css.h` (1468 lines) -- style sheets

Includes `zelr.h`, `web.h`, `dom.h` (20-23).

**Limits** (25-35, measured: "two sheets ... two hundred and seventeen kilobytes and eleven hundred rules"): `CSS_RULES 4000`, `CSS_SELS 9000`, `CSS_DECLS 16000`, `CSS_TEXT (512*1024)`, `CSS_MAXCLS 4` (classes/attributes per compound; extras are ignored, which makes the compound match more broadly). Also `CSS_BUCKETS 512` (1306), `CSS_HITS 192` (1344).

**Enums**: `P_*` properties (39-52: `COLOR, BACKGROUND, DISPLAY, FONT_SIZE, FONT_WEIGHT, FONT_STYLE, FONT_FAMILY, TEXT_ALIGN, TEXT_DECORATION, MARGIN_T/R/B/L, PADDING_T/R/B/L, BORDER_T/R/B/L, BORDER_COLOR, WIDTH, MAX_WIDTH, HEIGHT, LINE_HEIGHT, WHITE_SPACE, LIST_STYLE, RADIUS, TEXT_INDENT, VISIBILITY, OPACITY, FLEX_DIR, JUSTIFY, ALIGN_ITEMS, FLEX_WRAP, GAP, FLEX_GROW, MIN_WIDTH, MIN_HEIGHT, MAX_HEIGHT, BOX_SIZING, POSITION, TOP, RIGHT, BOTTOM, LEFT`); `POS_STATIC/RELATIVE/ABSOLUTE/FIXED` (60); `CSS_AUTO_OFF ((short)-32768)` (64); `D_INLINE 0, D_BLOCK, D_INLINE_BLOCK, D_LIST_ITEM, D_NONE, D_TABLE_CELL, D_FLEX` (66-67); `FD_ROW, FD_ROW_REVERSE, FD_COLUMN, FD_COLUMN_REVERSE` (75); `JC_START, JC_CENTER, JC_END, JC_BETWEEN, JC_AROUND, JC_EVENLY` (76); `AI_STRETCH, AI_START, AI_CENTER, AI_END, AI_BASELINE` (77); `A_LEFT, A_CENTER, A_RIGHT, A_JUSTIFY` (78); `WS_NORMAL, WS_PRE, WS_NOWRAP` (79); `LS_DISC, LS_DECIMAL, LS_NONE, LS_CIRCLE, LS_SQUARE` (80); units `U_PX, U_EM, U_REM, U_PCT, U_VW, U_VH, U_AUTO` (86); combinators `CB_FIRST, CB_DESC, CB_CHILD` (160); pseudo-classes `PS_NONE, PS_HOVER, PS_LINK, PS_VISITED, PS_FIRST_CHILD, PS_ROOT` (161).

**Globals**: `css_view_w`, `css_view_h` (96-97) -- the window for `vw`/`vh`; `lay_run` sets `css_view_w = width` (layout.h:1357), `browser.c` sets `css_view_h = view_h` each loop pass (1327, 1362). Nothing sets `css_view_h` in layouttest (so `vh` resolves to auto there).

**Types**:
- `clen {int v; uchar unit}` (99-109): hundredths of the unit.
- `cstyle` (111-144): `u32 color, background, border_color; short font_px; uchar bold, italic, mono, underline, strike, display, align, white, list, visible, has_bg; short mt, mr, mb, ml, pt, pr, pb, pl, bt, br, bb, bl; short width, max_width, height, min_width, min_height, max_height` (-1 = auto/none); `uchar border_box, position; short top, right_off, bottom, left` (`CSS_AUTO_OFF` = auto); `short line_h` (percent of font size); `short radius, indent; uchar flex_dir, justify, align_items, flex_wrap; short gap, grow`.
- `csel` (151-158): `short tag` (-1 any), `int id` (text offset or -1), `int cls[CSS_MAXCLS]` (class text offsets; an attribute-presence test is stored as `-offset-2`), `short ncls, combinator` (how this part joins the part to its left), `short pseudo`.
- `cdecl {short prop; int value}` (163-166), `crule {sel_at, sel_n, decl_at, decl_n, spec, order}` (168-173; parts leftmost first).
- `csheet` (175-185): `rules[4000], nrules, sels[9000], nsels, decls[16000], ndecls, text[512K], used, overflowed` (~1 MB).
- `cinline {int at, n}` (193-195): a node's `style=""` declarations, one per DOM node (parsed once per page, not per layout, 187-192).
- `cmatch {int hover; int visited_links}` (935-938).
- `cindex {by_tag[T_COUNT], by_key[CSS_BUCKETS], universal, next[CSS_RULES]}` (1308-1313); `chit {rule, spec, order}` (1346-1350).

**Text arena**: `css_put` (197), `css_put_lower` (205, unused).

**Colours** (213-314): `CSS_NAMES` (223-249): 53 names -- `black white red green blue gray grey silver maroon yellow olive lime aqua cyan teal navy fuchsia magenta purple orange transparent(=FFFFFF) lightgray/lightgrey darkgray/darkgrey whitesmoke gainsboro dimgray/dimgrey darkblue darkred darkgreen steelblue royalblue dodgerblue crimson tomato gold beige ivory linen salmon khaki indigo violet pink brown coral turquoise slategray/slategrey midnightblue rebeccapurple`. `css_color(s, *out)` (259): `#rgb`/`#rrggbb` (4/8-digit forms use the first 3/6 digits; alpha ignored); `rgb(...)`/`rgba(...)` with commas, spaces or `/`, `%` channels, alpha 0..1 or %, **composited over white** (303-308); otherwise an exact case-insensitive name. No `hsl()`, no `currentColor`.

**Lengths**: `css_len(s)` (318): `auto` or no digits ⇒ `U_AUTO`; optional sign; integer part capped at 10000; two fractional digits; suffix `em`, `rem`, `vw`, `vh`, `%`, `pt` (×4/3 → px); anything else (px, ex, ch, cm, vmin…) is px. `css_px(L, font_px, root_px, pct_of)` (359): em × font_px, rem × root_px, % of `pct_of` (0 if `pct_of < 0`), vw/vh of `css_view_w/h` (-1 if unset), auto = -1.

**Property names**: `CSS_PROPS` (373-425) maps: `color`; `background-color`, `background` → P_BACKGROUND; `display`; `flex-direction`; `justify-content`; `align-items`; `flex-wrap`; `gap`, `column-gap`, `row-gap` → P_GAP; `flex-grow`, `flex` → P_FLEX_GROW; `font-size/-weight/-style/-family`; `text-align`; `text-decoration`, `text-decoration-line`; `margin-*`, `padding-*`, `border-*-width`; `border-color`; `width`, `min-width`, `min-height`, `max-height`, `box-sizing`, `position`, `top`, `right`, `bottom`, `left`, `max-width`, `height`, `line-height`, `white-space`, `list-style-type`, `border-radius`, `text-indent`, `visibility`, `opacity`. `css_prop_of(name)` (427) exact lower-case.

**Parsing**:
- `css_skip(p, len, i)` (452) whitespace and `/* */` together (so a comment inside a selector list is not a descendant combinator).
- `css_parse_selector(s, p, len, *at, *spec)` (479): one selector up to `,` or `{`. Returns the number of compound parts, or **0 = drop this selector** for: `+`/`~` combinators (490-498); any pseudo-element (`::x`, or `:before/:after/:first-line/:first-letter`, 533-554 -- the `::-webkit-scrollbar{width:6px}` bug); any pseudo-class with an argument (`:not()`, `:nth-child()`, `:is()`…, 563-573); `[attr=value]`-style attribute tests (583-589); an unknown element name (606-613); running out of `CSS_SELS`. Supported: `*`, tag, `#id` (spec `1<<20`), `.class` (spec `1<<10`, max 4), `[attr]` presence (spec `1<<10`), pseudo-classes recognised **by length and first letter only** (556-560): 5+`h` → `:hover`, 4+`l` → `:link`, 7+`v` → `:visited`, 4+`r` → `:root`, any 11-letter name → `:first-child`; any other argument-less pseudo-class is **accepted and ignored** (spec +`1<<10`, no test). Descendant (space) and child (`>`) combinators. Tag spec 1.
- `css_add` (639), `css_parts` (649: up to N space-separated parts, parentheses kept together; parts are slices, not NUL-terminated), `css_shorthand4` (665: 1/2/3/4-value box expansion).
- `css_declare(s, name, nlen, v, vlen)` (681): trims; strips a trailing `!important` (only when the value is longer than 10 chars; the priority is **not** honoured); lower-cases the name (≤ 39 chars). Shorthands: `margin`, `padding` (4-value); `border` and `border-width` → one width for all four sides (last length, or `thin`=1/`medium`=2/`thick`=4, 1 if none, 0 if `none`/`hidden`) plus `border-color` if a part parses as a colour (711-746); `border-top/-bottom/-left/-right` → the whole value into that side (width and colour extracted at apply time); `font` → the first part that parses as a positive length becomes `font-size`, `bold`/`italic` before it are taken (751-765); `list-style` → P_LIST_STYLE. `background` (not `-color`) keeps only the first part that parses as a colour, else nothing (774-787: "a gradient or an image ... paints panels black"). Everything else via `CSS_PROPS`; unknown properties dropped.
- `css_parse_block` (792): `name: value;` pairs to `}`; `;`/`}` inside parentheses do not end a value.
- `css_at_rule(p, len, i, *open_body)` (828): `@x ...;` skipped (so `@import` is never fetched); `@media` (5 letters starting `m`) and `@supports` (8 letters starting `s`) are **opened** unless the prelude contains `print` (841-852: min-width queries are opened on purpose; so are max-width and `prefers-color-scheme`); every other block at-rule (`@font-face`, `@keyframes`, `@layer`, `@container`, …) is skipped whole.
- `css_parse(s, p, len)` (867): loop of at-rules, stray `}` (closing an opened media block), and rules: a selector list (≤ 32 selectors) then one declaration block shared by one `crule` per selector (`order = nrules`). A rule whose block is not directly after the selectors is discarded.

**Matching**: `css_part_matches(s, d, el, c, m)` (940): element; tag; `id` exact; each class via `dom_has_class`, each `[attr]` via `dom_attr`; pseudo: `:hover` ⇔ `el == m->hover`; `:link` **and** `:visited` ⇔ an `<a>` with `href` (`visited_links` is never read); `:first-child` ⇔ first element child; `:root` ⇔ `el == d->root` (the synthetic root). `css_matches(s, d, el, r, m)` (977): rightmost part first; child ⇒ parent must match; descendant ⇒ the **nearest** matching ancestor is taken (no backtracking).

**Cascade support**:
- `css_default_style(st, root_px)` (1003): colour `0x1A1A1A`, background `0xFFFFFF`, border colour `0xD0D0D0`, font `root_px`, display inline, flex row/start/stretch, align left, white normal, list disc, visible, no bg, zero boxes, sizes -1, static, offsets auto, `line_h 145`, radius/indent 0. It does **not** set `border_box` (see §10).
- `css_inherit(child, parent)` (1037): defaults, then inherits `color, font_px, bold, italic, mono, align, white, list, line_h, visible, underline, background` (with `has_bg = 0`). Not inherited: margins, strike, sizes, display, flex settings, position.
- `css_apply(s, dcl, st, root_px, pct_of)` (1054) per property: colours via `css_color` (background sets `has_bg`); `display` (`none`, `inline-block`, `inline-flex`→inline-block, `inline`, `list-item`, `table-cell`, `flex`, everything else → block, 1063-1072); flex keywords (1074-1103; `flex-wrap` stored but unused by layout); `gap` px ≥ 0; `flex`/`flex-grow` leading integer (`none` 0, `auto` 1); `font-size` (`smaller` ×5/6, `larger` ×6/5, else a length resolved against the **current** `st->font_px`, clamped 7..96, 1126-1133); `font-weight` (`bold*`, `normal`, digit ≥ 6); `font-style` italic/oblique; `font-family` mono iff the value contains `mono`; `text-align`; `text-decoration` (first word: `underline`/`line-through`/`none`); `white-space` (`pre*` → pre, `nowrap`); `list-style(-type)`; `box-sizing`; `position` (`sticky` → relative); `visibility:hidden`; `opacity` `0`/`0.0`/`0.1…` → invisible; `line-height` (% → percent, px > 4 → percent of font size, a number → ×100, clamped 90..300, 1195-1204); lengths (margins, paddings, border sides, sizes, offsets, radius, indent): `height/min-height/max-height` in % → auto (1238-1252), `top/bottom` in % → auto; border sides: `none/hidden` → 0, no length → 1, any colour part sets `border_color`; offsets keep sign, clamp ±4000, auto; **all other lengths clamp to [-1, 4000]** (1285-1287), so a negative margin becomes -1, the value that means `auto`.

**Index and collection**: `css_hash` (1315, FNV-1a masked to 512); `css_index(s, x)` (1321): files each rule (walking backwards so chains are in source order) under its rightmost part's id, else first real class, else tag, else universal. `css_collect_chain` (1352) appends matching rules (≤ `CSS_HITS`). `css_collect(s, x, d, el, m, hits)` (1369): universal chain, the element's tag chain, its id's bucket, and each class's bucket (class tokens copied into a 64-byte buffer), then a stable insertion sort by `(spec, order)`. A bucket shared by the id and a class, by two classes (hash collision), or by a repeated class token is walked twice, producing duplicate hits.

**UA stylesheet** `CSS_UA` (1423-1468), verbatim rules:
```
html,body{display:block;margin:0;padding:0;color:#1a1a1a}
body{padding:0 2px}
div,section,article,main,aside,nav,header,footer,figure,figcaption,hgroup,form,dl,dt,fieldset,address{display:block}
p{display:block;margin:0.85em 0}
center{display:block;text-align:center}
h1{display:block;font-size:2em;font-weight:bold;margin:0.55em 0 0.4em}
h2{display:block;font-size:1.5em;font-weight:bold;margin:0.7em 0 0.35em}
h3{display:block;font-size:1.22em;font-weight:bold;margin:0.8em 0 0.3em}
h4{display:block;font-size:1.05em;font-weight:bold;margin:0.9em 0 0.3em}
h5,h6{display:block;font-weight:bold;margin:1em 0 0.3em}
h6{font-size:0.9em}
ul,ol{display:block;margin:0.7em 0;padding-left:1.8em}
li{display:list-item;margin:0.25em 0}
ol{list-style-type:decimal}
dd{display:block;margin-left:1.8em}
blockquote{display:block;margin:0.9em 0;padding-left:1em;border-left:3px #d6d6d6;color:#555}
pre{display:block;white-space:pre;font-family:monospace;margin:0.9em 0;padding:10px 12px;background:#f6f7f9;border-radius:6px;color:#222}
code,kbd,samp,tt{font-family:monospace;font-size:0.93em}
b,strong{font-weight:bold}
i,em{font-style:italic}
small{font-size:0.86em}
a{color:#0b5ed7;text-decoration:underline}
hr{display:block;margin:1.1em 0;border-top:1px #dcdcdc}
table{display:block;margin:0.8em 0}
tr{display:block}
td,th{display:table-cell;padding:4px 10px 4px 0}
th{font-weight:bold;text-align:left}
caption{display:block;font-style:italic;margin:0.3em 0}
head,script,style,title,meta,link,noscript{display:none}
button{display:inline-block;padding:5px 12px;background:#f2f3f5;border:1px #c9ccd1;border-radius:6px}
input,textarea,select{display:inline-block;padding:4px 8px;background:#ffffff;border:1px #c9ccd1;border-radius:6px}
img{display:inline-block}
center{display:block;text-align:center}
iframe,svg{display:none}
```
(`fieldset`/`address` are unknown tags, so those two selectors are dropped; `center` appears twice; the `a` colour is always overridden by the accent sheet.)

### 3.6 `userland/layout.h` (1416 lines) -- boxes and lines

Includes `web.h`, `dom.h`, `css.h`, `facetext.h` (21-26).

**Limits** (28-44, measured on an article with 1600 anchors that produced 46 000 items and 6 500 links): `LAY_ITEMS 96000`, `LAY_TEXT (1024*1024)`, `LAY_LINKS 12000`, `LAY_LINE 400` (unused), `LAY_DEPTH = DOM_DEPTH (64)`, `LAY_FLEX_MAX 32` (849). A link past `LAY_LINKS` is drawn as plain words (not clickable).

**Item kinds** `LK_BOX 1, LK_TEXT, LK_BULLET, LK_IMAGE, LK_FIELD` (46). **Control kinds** `CTL_NONE 0, CTL_TEXT, CTL_PASSWORD, CTL_BUTTON, CTL_CHECK, CTL_RADIO, CTL_AREA, CTL_SELECT, CTL_HIDDEN` (168-169), shared by layout (size) and browser (look and behaviour).

**Global** `lay_show_hidden` (65): set by the browser for the second layout pass of a page that produced no words (ignores `visibility`/`opacity` checks; `display:none` still applies).

**Types**:
- `litem` (67-77): `int x, y, w, h` (document coordinates; y down the document); `int at` (text offset for LK_TEXT and numbered bullets, else -1); `u32 color, bg, border`; `short face`; `int node` (the element; **text runs leave it -1**); `int link` (index into `links` or -1); `uchar kind, under, strike, radius, bt, br, bb, bl, has_bg`.
- `llink {int node; int href}` (79-82); `limage {int node; int w, h}` (87-90: size only -- pixels belong to the browser).
- `ldoc` (92-101): `items[96000], nitems, text[1 MiB], used, links[12000], nlinks, height, overflowed` (~6.5 MB).
- `lctx` (243-278): `d, s, x, m, inl, imgs, nimgs, out, root_px`; line state `line_at, line_n, pen, line_top, line_h, line_base, line_left, line_width, align, pending_space, line_started`; `cur_link`; `list_depth, list_count[LAY_DEPTH]`; containing block for positioned boxes `pos_x, pos_y, pos_w`.

**Faces** (103-153): `face_pick(px, bold, mono)` (109) nearest size among faces with the same bold and mono flags, falling back to nearest size alone. The 33 faces (`facetext.h:6, 26255-26289`): regular and bold at 11, 12, 13, 14, 15, 16, 17, 19, 21, 24, 28, 34, 42 px; monospaced regular 12, 13, 14, 15, 17; monospaced bold 13, 15. **No italic face** (italic is parsed and dropped). `tface_of` (131), `tface_h` (136, = face size), `tface_wn` (138: sum of glyph advances; bytes outside 32..126 measured as space), `tface_w` (149).

**Controls**: `lay_control_kind(d, el)` (181): `textarea` → AREA, `select` → SELECT, `button` → BUTTON; `input` by `type` (case-insensitive): missing/empty → TEXT, `hidden`, `password`, `checkbox`, `radio`, `submit`/`button`/`reset`/`image` → BUTTON, anything else → TEXT. `lay_control_label(d, el, kind)` (207): button: `value`, else `<button>` text (≤ 255) or `Button`, else `Reset`/`Submit`; select: text of the first direct `<option>` child ("nothing here opens one"); textarea: `value` attribute or text content; others: `value` or `""`. Returns a shared static 256-byte buffer for computed labels.

**Helpers**: `lay_put` (280), `lay_number` (291: leading digits, capped 100000, so `100%` reads as 100), `lay_image_of` (302), `lay_item` (308: defaults colour `0x1A1A1A`, bg `0xFFFFFF`, border `0xD0D0D0`, kind LK_TEXT, node/link -1).

**Lines**: `lay_line_end(L, *y)` (331): shift the line's items by the slack for centre/right (justify is left); drop each item onto the baseline (`y = line_top + line_base - height`, never above `line_top`); `*y = line_top + line_h`. `lay_line_start` (359). `lay_line_fit(L, h, pct)` (376): line box `h*pct/100`, baseline at `h*4/5 + (box-h)/2`, both maxima over the line.

**Text**: `lay_word(L, s, n, st, *y, face)` (389): wraps when the word (plus a pending space measured in its own face) passes the right edge, the line is not empty and `white-space` is not `nowrap`; one LK_TEXT item per word (colour, underline, strike, `link = cur_link`). Long words are never broken. `lay_text_run(L, s, st, *y)` (423): `pre` → one item per source line, newlines end lines, spaces kept, no wrapping (and no underline/strike set); otherwise split on space/tab/CR/LF/FF into words with collapsing.

**Style**: `lay_style(L, el, parent, out, pct_of)` (470): `css_inherit`, `css_collect`, apply every declaration of every hit in order, then the element's inline declarations (always last).

**Inline layout** `lay_inline(L, node, parent, *y)` (563): iterative walk of `node`'s subtree with an explicit 64-entry style stack (`stack`, `stack_node`, `stack_x`, `stack_top`, `stack_slot`). For each node: pop styles whose element has been left; text ⇒ `lay_text_run` (if visible); element ⇒ `lay_style`; `display:none` ⇒ skip the subtree (600-609); `<br>` ⇒ end the line; `<img>` ⇒ if its picture arrived, size = attributes (`width`+`height`, or one of them scaling the other) else intrinsic, capped at the line width, wrapped like a tall word, LK_IMAGE carrying `link`, line fit at 100 % (a fix for `ih*ih/100`, 660-665); else its `alt` text in grey `0x6B6B6B`; controls ⇒ sized (check/radio: face+2 square; button: label width + 20 by face+10; textarea: `cols` (28) × width of `0` + 10 by `rows` (3) × (face+4) + 8; others: `size` (20) × `0` + 10), clamped to [8, line width], hidden controls take no room, LK_FIELD carrying face, colour, bg, radius, border colour and borders (≤ 8), `pen += fw + 2`, then the control's subtree is skipped (762-768); any other element ⇒ pushed (`lay_inline_open`), and an `<a href>` registers a link and sets `cur_link` (775-783). At the end remaining boxes are closed and `cur_link = -1`. Block-level descendants inside an inline run are flattened into the line.
- `lay_inline_open` (501): reserves an LK_BOX slot if the element has a background or border; `pen += max(ml,0) + bl + pl`.
- `lay_inline_close` (521): `pen += pr + br + max(mr,0)`; fills the slot (x after the left margin, y = line top, h = line height) only if the box began on this line -- "a box that started on one line and ended on another is drawn as nothing" (558-561); radius ≤ 40.

**`lay_is_block_node(L, n, parent)`** (811): element whose display is BLOCK, LIST_ITEM or FLEX. Inline-block, table-cell, inline and none are all laid out inline.

**Flex**: `lay_measure(L, node, parent, avail, *height)` (853): saves every line/list field and the output counters, lays `node` out at x = 0, returns the rightmost item edge and the height, restores everything (including `overflowed`, 872-877). `lay_flex(L, node, st, cx, cw, *y)` (908): items = up to 32 **element** children (text children are not laid out); column directions stack children as blocks with `gap` (931-938); rows: each child measured at `3*cw` (or 2000), clamped to `cw`, an explicit `width` wins; too wide ⇒ shrink proportionally with a floor of 8; spare room ⇒ given to `flex-grow` children (remainder to the last grower), otherwise distributed by `justify-content`; the row is as tall as the tallest **measured** child; each child laid out at its position and width, then shifted down for `align-items: center/end` (stretch/start/baseline stay at the top). `flex-wrap` is ignored.

**`lay_block(L, node, parent, x, avail, *y)`** (1079): style probe; none or invisible ⇒ nothing. Absolute/fixed (1086-1117): containing width `pos_w` (if > 16) else `avail`; x from `left` (`pos_x + left`) or `right` (`pos_x + aw - right - width`) else the flow x; y from `top` (`pos_y + top`) else the flow y (bottom-only stays in flow); laid out with `lay_block_placed` and `*y` restored (takes no space). Relative (1119-1133): laid out in flow, then every item it produced shifted by `left`/`-right`, `top`/`-bottom`. Otherwise `lay_block_placed`.

**`lay_block_placed(L, node, parent, x, avail, *y)`** (1138): style again; negative margins treated as 0; `box_w = avail - ml - mr`; `width`, `max-width`, `min-width` (content-box adds the horizontal frame, 1150-1169); auto left and right margins centre a box narrowed by width/max-width; min beats max; `box_w >= 16`; `*y += mt`; LK_BOX slot if background or border; content box `cx = x + ml + bl + pl`, `cw = box_w - frame` (≥ 16); list-item marker (1208-1236: decimal `N.` right-aligned 6 px left of the content, else a disc glyph slot at `cx - 18`; circle/square draw as discs); `<ul>`/`<ol>` push a list counter; a non-static box becomes the containing block (`pos_x = cx`, `pos_y = box_top`, `pos_w = cw`) for its descendants; flex containers go to `lay_flex`; otherwise children: block children via `lay_block`, runs of inline/text children share one line context (text-indent at each run start); `height`/`min-height` grow the box, `max-height` shrinks it (content not clipped, 1317-1321); `<hr>` at least its border; the slot is filled with the final border box; `*y += mb`. Vertical margins never collapse.

**`lay_run(out, d, s, x, m, inl, imgs, nimgs, width, root_px)`** (1348): resets the output, `css_view_w = width`, containing block = the page, root style = `css_default_style(root_px)`, lays out from `d->body` (or `d->root`) at x = 0. Styles on the page's `<html>` element therefore do not reach `<body>` (its parent style is the default), and `rem` always means the browser's `root_px`.

**Queries**: `lay_words(o)` (1386) counts LK_TEXT items; `lay_link_at(o, x, y)` (1397): the last **LK_TEXT** item with a link under the point (±2 px vertically); `lay_node_at(o, x, y)` (1408): the last item with `node >= 0` under the point (boxes, images, fields, bullets -- never text runs).


### 3.7 `userland/browser.c` (1746 lines) -- the program

Includes (27-38): `zelr.h draw.h ui.h web.h fetch.h dom.h css.h layout.h jsdom.h png.h jpeg.h svg.h`.

**Limits and buffers** (40-92): `SRC_MAX (1024*1024)` -- the page response (headers + body) and pages the browser writes itself; `CSS_MAX (512*1024)` -- each linked sheet and **each picture** (the same `cssbuf`, reused once sheets are parsed, 704-709); `SHEETS_MAX 6`; `SCRIPT_MAX (128*1024)`, `SCRIPTS_MAX 8` (external scripts); `REPLY_MAX (128*1024)` (XHR replies, separate "because a reply arriving must not write over the text of the script that asked for it"); `ASKS_MAX 64` (XHRs per page); `HIST_MAX 40`; `PICS_MAX 24` (123); `REFRESH_MAX 3` (312); `post_body[4096]` (349); `focus_buf[1024]` (168); `title[160]` (101); `status[URL_TEXT + 96]` (102).

**Global state**:
- page data: `src, cssbuf, scriptbuf, replybuf` (80-83); `doc` (ddoc), `sheet` (csheet), `index_` (cindex), `page` (ldoc), `match` (cmatch), `inl[DOM_NODES]` (cinline) (87-92); roughly 12 MB of static arrays in total;
- navigation: `hist_t {char text[URL_TEXT]; int scroll}` (94), `hist[40], hist_n, hist_at = -1` (95-96); `here` (url_t of the current page, 98); `reply` (response_t, 99); `go_to[URL_TEXT], load_post` (297-298); `want_go, go_is_post` (350-351); `refreshes, go_is_refresh` (313-314); `scripts_outside, asks_made` (84-85);
- view: `scroll` (103), `over_link = -1` (104), `hover_node = -1` (105), `root_px = 16` (109, the zoom), `find_item = -1` (237), `page_unhidden` (657), `title`, `status`;
- pictures: `shown {int node; picture pic}` (125-128), `pics[24], npics, pic_sizes[24] (limage), npic_sizes, pics_skipped` (130-134);
- controls: `browser_win` (160), `focus_node = -1` (162), `focus_field` (169, a `ui_field` over `focus_buf`); address bar `address[URL_TEXT]`, `bar` (1245-1246), `bar_fresh` (1262);
- scripts: `scripts_ran, scripts_changed, script_err[128], said_script_err` (448-453).

**Pictures**: `pics_drop()` (136) frees every decoded picture; `pic_of(node)` (143).

**Controls** (149-219): values live in the DOM. `field_checked(el)` (171): `checked` present and not `"0"`; `field_set_checked(el, on)` (179) writes `"1"`/`"0"` ("unchecking one has to leave something behind"); `field_value(el, kind)` (183) = `lay_control_label`; `field_set_value(el, v)` (187) writes `value`; `form_of(el)` (194) nearest `<form>` ancestor; `focus_control(el)` (203): sets `focus_node`, and for TEXT/PASSWORD/AREA copies the current value into `focus_buf` and focuses the editor (other kinds get `focus_node` but no editable field).

**Find** (221-284): `publish_text()` (243) concatenates the text of every LK_TEXT item, each followed by a space, into a static 4096-byte buffer and calls `win_set_text` (the kernel keeps `WM_TEXT_MAX 4096`, include/wm.h:77). `find_show(which, view_h)` (258): `win_find_query` (≤ 63 chars), then walks LK_TEXT items counting case-insensitive, non-overlapping matches **inside each item**; the `which`-th sets `find_item` and scrolls to `item.y - view_h/3`. The kernel counts matches in the published text the same way (kernel/wm.c:3266-3275) and sends `which = -1` to clear marks (3334-3341).

**Meta refresh** `meta_refresh(out, cap)` (316): first `<meta http-equiv="refresh">` whose `content` has `;` then optional spaces, `u`/`U`, then `=`; the URL is taken with quotes stripped. The delay is ignored; `content="5"` (no URL) is ignored (327-329).

**Forms** (286-445):
- `url_encode_into(out, cap, *at, v)` (353): unreserved kept, space → `+`, else `%XX`.
- `form_query(form, out, cap)` (372): every element in **node-index order** whose control kind is not NONE or BUTTON, belongs to `form` (`form_of`), and has a non-empty `name`; checkboxes/radios only when checked (value or `on`); others via `field_value` (so a `<select>` sends its first option's *text*); `name=value` pairs joined by `&`. Buttons never contribute (not even the one clicked).
- `submit_form(form)` (405): action resolved against `here` (empty ⇒ this page, "what a search box on a site relies on"); `method` `post` (case-insensitive) ⇒ POST; `go_to = url_text(target)` with any existing query removed (426-429); POST ⇒ `post_body = query`; GET ⇒ `go_to + "?" + query`; `want_go = 1` (navigation happens later in the loop).

**Status text**: `say(a, b)` (457), `say_more(s)` (464), `why(rc)` (471) human reasons per `WEB_ERR_*`, `why_heading(rc)` (497): `No network address` (DOWN), `That name did not resolve` (RESOLVE), `This connection was refused` (TLS), else `Cannot show this page`.

**Building a page** (506-815):
- `gather_inline_styles()` (517): for each element (indices `< doc.count` at call time) with a `style` attribute, parses `name: value;` pairs (parentheses respected) through `css_declare` into `sheet.decls`, recording `inl[i] = {at, n}`.
- `gather_inline_sheets()` (556): every `<style>` element's text children in node order ⇒ `css_parse` (no `media` check).
- `gather_linked_sheets(*fetched, *skipped)` (578): `<link>` whose `rel` is exactly `stylesheet` (case-insensitive whole string) with a non-empty `href`; at most 6 fetched (`web_get` into `cssbuf`, 2xx with a body ⇒ `css_parse`); the rest counted as skipped. No `media` check.
- `fetch_script(src, *out)` (611): the `jsdom_fetch_with` hook; ≤ 8 per page; resolved against `here`; `web_get` into `scriptbuf`; returns the length for 2xx (a truncated script is still returned).
- `do_request(method, url, body, *out, *status)` (636): the `jsdom_request_with` hook; ≤ 64 per page; resolved against `here`; a method starting with `P`/`p` ⇒ `web_post` (so PUT/PATCH are POST), else `web_get`; `*status = rc` (negative on network errors). The comment says no cookies are sent -- they are (see §10).
- `relayout(width)` (659): `match.hover = hover_node`, `match.visited_links = 0`; `lay_run`; if `lay_words == 0 && dom_has_words`, a second `lay_run` with `lay_show_hidden = 1`; `page_unhidden` = the second pass produced words.
- `accent_sheet()` (688): `a{color:#<ui_load_theme().accent>}` parsed after the UA sheet.
- `gather_pictures()` (710): up to 24 `<img src>` in node order; `web_get` into `cssbuf`; accepted if `0 <= rc < 400` with a body; format from the bytes: `89 'P' 'N' 'G'` ⇒ `png_decode(body, len, &pic, 0xFFFFFF)`; `FF D8` ⇒ `jpeg_decode`; otherwise (after whitespace) `<` ⇒ `svg_render(body, len, width attr, height attr, &pic, 0xFFFFFF)`; failures counted in `pics_skipped` (the element falls back to alt text).
- `build(html, len, width, want_sheets, *fetched, *skipped)` (769): `jsdom_close`; `dom_parse`; `css_init`; UA; accent; `<style>` sheets; linked sheets (if `want_sheets`); inline styles; `css_index`; `pics_drop`; pictures (if `want_sheets`); reset focus, script counters and `asks_made`; `jsdom_open(&doc, &sheet)` ⇒ `jsdom_fetch_with`, `jsdom_request_with`, `jsdom_scripts`, `jsdom_loaded`, `jsdom_changed`; `hover_node = -1`; `relayout`; `find_item = -1`; `publish_text`; `title` from `doc.title`.
- `show_message(heading, body, width)` (820): a small HTML page (`body{padding:28px 32px;max-width:640px}...`) through `build(..., want_sheets = 0)` -- "the one path that draws anything is the path that is used".
- `show_plain(body, len, width)` (837): `<style>body{padding:16px}pre{font-size:14px;line-height:1.45}</style><pre>` + body with `<` and `&` escaped + `</pre>`, written into `src`.
- `number_into` (855).

**`load(address, width, keep_scroll)`** (865):
1. `url_parse` fails ⇒ "Not an address" page, status `nothing to go to`.
2. `here = u`; status `fetching <address>`.
3. `web_post(&here, post_body, src, SRC_MAX, &reply)` if `load_post` (then cleared: "going back to it afterwards asks again with GET"), else `web_get`. `here` becomes the post-redirect URL.
4. Error ⇒ an error page: for `WEB_ERR_TLS` with a reason, heading `This connection was refused` and the kernel's reason, status `refused: <reason>`; else `why_heading`/`why`. Title cleared. Return.
5. `Content-Type` starting `text/plain` or `application/json` ⇒ `show_plain` (title = path); anything else ⇒ `build` (binary types are parsed as HTML).
6. `scroll = 0` (`keep_scroll` is always passed 0).
7. Status, first match wins: `rc >= 400` ⇒ `the server said this page is not there`; `page_unhidden` ⇒ `this page hides itself until its own script rebuilds it; shown as it arrived`; `reply.truncated` ⇒ `shown as far as it fits: more page arrived than this can hold`; `doc.overflowed` ⇒ `... more markup than this can hold`; `page.overflowed` ⇒ `... more on the page than this can lay out`; else `N link(s) on this page`. Then `, N style sheet(s)`, ` (more were not read)`, `, N picture(s)` / ` (more would not show)` / `, no picture on it would show`, then `, a script stopped: <error>` or `, N script(s) ran` + ` (N from a file/files)` + `, one would not come`.
8. If `200 <= rc < 400`, `refreshes < 3` and a meta refresh names a different URL: `go_to` = it, `go_is_refresh = 1`, GET, `want_go = 1`, `, following the page's own redirect`.
9. Always last: `, encrypted` or `, NOT encrypted` (from `reply.secure`, i.e. the final hop).

**History** `push_history(address)` (1013): saves the current scroll into the current entry, drops the oldest when full (shifting 39 entries), truncates forward history, appends. Entries' `scroll` is saved but never read back.

**Drawing**:
- `tface_draw(s, x, y, str, fg, which)` (1035): the 33-face blitter (draw.h's own covers only 6 faces); baseline `y + size*4/5`; coverage 255 ⇒ `fg`, else `mix(under, fg, a)`; clipped to the surface.
- `draw_page(s, ox, oy, vw, vh)` (1060): for each item with `y = it->y - scroll` inside `[-8 - h, vh + 8]`:
  - LK_BOX: width clamped to `vw`; `round_rect` or `rect` for the background if `has_bg`; each non-zero border side as a solid `rect` in `border` colour (no styles, one colour).
  - LK_IMAGE: nearest-neighbour scaling from the RGB picture, clipped to the view on all sides.
  - LK_FIELD: checkbox/radio = filled square with a 1 px edge (`0x3B6FD6` when focused, else `0xA9A9A9`), ticked = inner square or a disc; others: inside colour = page background if set, else `0xE6E6EA` for buttons / `0xFFFFFF`; rounded (`ui_round` twice) when the page gave a radius; border colour = page's if it set any border, else the edge colour; value from the DOM **at draw time** (passwords as `*`, CR/LF as spaces), showing the tail that fits; buttons centred; text always `0x1A1A1A`; caret after the visible text when focused.
  - LK_BULLET: numbered text or a radius-3 disc.
  - LK_TEXT: find highlight `0xFFE58F` behind the whole item (1206-1212); hovered link colour `0x0842A0`; underline if `under` or hovered (extended across the space to the next word of the same link on the same line, 1225-1235); strike-through line.
- Chrome constants: `TOOLBAR_H = UI_PAD + UI_BTN_H + UI_PAD` = 46 (1243; `UI_PAD 8`, `UI_BTN_H 30`, `UI_ROW 26`, `UI_SCROLL_W 10`, ui.h:25-31).

**Address bar and search** (1245-1311): `set_address(s)` (1264) copies into the bar and puts the cursor at the end; `bar_fresh` (1248-1262) makes the first printable key after a click replace the text ("a selection in everything but drawing"); `SEARCH_PREFIX "https://lite.duckduckgo.com/lite/?q="` (1291); `set_search(what)` (1293) = prefix + `url_escape(what)`; `go_or_search(typed)` (1308).

**`main(argc, argv)`** (1313): `win_create("Browser", 860, 620)`, `win_allow_resize`; arguments joined with spaces and passed to `go_or_search` (so `browser how an operating system boots` searches), else `https://example.com/`; `push_history`; `css_view_h = 600` until the first frame; `want_load = 1`. The loop (1350-1742) is detailed in §4.2. Geometry per pass: `view_x = 3`, `view_y = TOOLBAR_H + 3` (49), `view_w = w - 6 - UI_SCROLL_W`, `view_h = h - view_y - UI_ROW - 3` (minimums 80/40); layout width `view_w - 2*UI_PAD`; for the default window: 844×542 view, layout width 828. Toolbar: `<` (x 8, w 34), `>` (x 46, w 34), `Reload` (x 86, w 62), the address field from x 154 to `w - 54`, `Go` (primary, w 40). Status bar: left = status, or the resolved URL of the link under the pointer; right = page title or `zelr`. The page is drawn into a surface cut off at the bottom of the well (1732) and the bevel redrawn afterwards; `ui_scrollbar` (display only) when the page is taller than the view.

**Keyboard** (1411-1482):

| Where the keyboard is | Key | Effect |
|---|---|---|
| Address bar (`bar.focused`) | Return | `go_or_search(address)`, push history, load |
| | Esc | leave the bar, restore the current history entry's text |
| | anything else | `ui_field_key` (Backspace, Delete, Left, Right, Home, End, printable; the first printable key after a click replaces the text) |
| A page control (`focus_node >= 0`) | Esc | unfocus |
| | Return (not a textarea) | submit the control's form |
| | Space on checkbox/radio | toggle `checked` |
| | anything else | `ui_field_key` on `focus_field`, then write `value` back to the DOM |
| The page | Down / Up | scroll ±40 px |
| | PageDown / PageUp, Space | ±(view_h − 40) |
| | Home / End | top / `page.height` |
| | `+` or `=` / `-` | `root_px` ±2 within 10..28, relayout |
| | Left / Right | history back / forward (re-fetch) |

Mouse: wheel ±48 px per step (`scroll -= steps*48`, 1511); click on a link, a control, the toolbar buttons, or the bar. There is no Ctrl+L, no Backspace-for-back, no F5, no draggable scrollbar.

### 3.8 The JS interface (`userland/jsdom.h`, 1314 lines; `userland/js.h` 1-260)

The engine (js.h) runs a language and knows nothing of pages; its `jctx` has two hooks, `host_get(J, obj, name, *out)` and `host_set(J, obj, name, v)` (js.h:214-219), and every `jobj` carries an `int host` (js.h:133-135). jsdom.h fills them in. Engine limits relevant to pages: `JS_MEM_CAP 24 MiB`, `JS_STEP_CAP 40 000 000`, `JS_DEPTH_CAP 160` (js.h:61-63), region allocation in `JS_CHUNK 256 KiB` pieces via `malloc`, no garbage collector (js.h:38-53). (The js.h header's "no regular expressions" is contradicted by jsregex.h -- the engine agent's area.)

**What browser.c calls** (all static in jsdom.h unless noted):

| Function | Line | Contract |
|---|---|---|
| `jsdom_close()` | 1125 | drop pending XHRs; if open, `js_done`, forget doc/sheet/wrappers/listeners/timers. Called first thing in `build`. |
| `jsdom_open(ddoc*, csheet*)` | 1139 | opens a world only if the page has a `<script>` or any `on*` attribute (`jd_page_scripts`, 1109); installs hooks, `document`, `window` (= the global object), `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`, `XMLHttpRequest`; allocates `jd_wrap[DOM_NODES]`. |
| `jsdom_fetch_with(fn)` / `jsdom_request_with(fn)` (non-static) | 1229 / 781 | the browser's `fetch_script` and `do_request`: jsdom knows nothing about the network or the page address. |
| `jsdom_scripts(err, cap)` | 1240 | every `<script>` element in **node-index order** (re-reading `d->count`, so script elements created meanwhile are run too), `src` ⇒ fetched synchronously at its position (in-page text ignored), else its text child; `type`, `async`, `defer` are not examined; returns how many ran; `err` = the first error (with "at line N"). |
| `jsdom_loaded()` | 1293 | `DOMContentLoaded` then `load`, dispatched with target -1: only `document` listeners run (no `window` listeners, no `window.onload`, no `<body onload>`). |
| `jsdom_changed()` | 1308 | read-and-clear of `jd_dirty` (set by any DOM write, `document.title` write, class/value/checked writes, tree mutations). |
| `jsdom_live()` | 1303 | a world is open. |
| `jsdom_timers()` | 894 | run every due timer once (repeaters rebooked from now); returns how many ran. Clock = `ticks()` at `JD_HZ 100`. |
| `jsdom_requests()` | 847 | perform **one** pending XHR via `do_request`, set `status`, `readyState = 4`, `responseText`, call `onload` (always, even on failure); returns 1 if one was made. |
| `jsdom_click(node)` | 1301 | `jd_dispatch(node, "click")`: from `node` up the parent chain running the `on<type>` attribute (wrapped as `(function(event){...})`, compiled on demand, ≤ 4096 bytes) then listeners, then `document` listeners; returns whether `preventDefault()` was called. No capture phase (208-218). |
| `jsdom_outside()` / `jsdom_outside_failed()` | 1237 / 1238 | external scripts fetched / not fetched. |
| `jsdom_error()` | 1314 | the first error text. |

**DOM surface for scripts**: `document.getElementById`, `getElementsByTagName` (known tags only; a snapshot array), `createElement`, `createTextNode`, `querySelector(All)` (selectors parsed onto the browser's own sheet and rolled back, 564-619 -- same parser and matcher as CSS), `addEventListener`/`removeEventListener`, `document.title` (read/write), `document.body`. Elements: `getAttribute`, `setAttribute`, `addEventListener`, `removeEventListener`, `appendChild`, `insertBefore`, `removeChild`, `remove`, `querySelector(All)` (descendants only); properties `textContent` (r/w; read up to 4096 bytes), `tagName`, `id`, `className` (r/w), `value` (r/w; the attribute, or a textarea's text), `checked` (r/w), `classList` (`add/remove/contains/toggle`), `parentNode/parentElement`, `firstChild`, `lastChild`, `nextSibling`, `previousSibling`, `children` (snapshot). Event object: `type`, `target`, `preventDefault`, `stopPropagation`. XHR: `open(method, url)`, `send(body)`, `setRequestHeader` (accepted and dropped), `onload`, `status`, `readyState`, `responseText`; no `onreadystatechange`, `onerror`, `getResponseHeader`. Not present: `el.style`, `innerHTML`, `location`, `window.addEventListener`, `document.cookie`, `localStorage`, `fetch`/Promises.

**Limits**: `JD_LISTENERS 256`, `JD_TYPE_MAX 24` (event-type name), `JD_TIMERS 64`, `JD_REQUESTS 8` pending (a ninth `send` is silently dropped, 821), `JD_WRAPS = DOM_NODES`, host numbers `JD_DOCUMENT 0x1000000`, `JD_CLASSLIST 0x2000000` (+ node).

### 3.9 Test programs in scope

- **`userland/layouttest.c`** (515): builds pages like the browser minus fetching -- `dom_parse`, `CSS_UA`, `<style>` sheets, zeroed `inl`, `css_index`, `lay_run(width, 16)` (56-83) -- and asserts on `litem` coordinates found via `box_of(node)` (LK_BOX with that node) or LK_IMAGE items; `picture_at(id, w, h)` fakes a decoded picture (51-92). No accent sheet, no linked sheets, `css_view_h` never set. Ends `LAYOUTTEST_PASS`/`LAYOUTTEST_FAIL`, exit status = failures. About 46 assertions (listed in §8).
- **`userland/pagetest.c`** (639): dom.h + jsdom.h only (no css parsing beyond query selectors, no layout): `run_scripts` = `jsdom_open(d, &qsheet)` + `jsdom_scripts` + `jsdom_loaded` + `jsdom_changed` (65-73); `pump_until(want, ms)` polls `jsdom_timers()` against `ticks()` (82-87). About 71 assertions. Ends `PAGETEST_PASS`/`PAGETEST_FAIL`.
- **`tools/browsercheck.py`**, **`tools/formcheck.py`**, **`tools/livecheck.py`** drive a real guest with the desktop (`vm.type("desktop\n")`), start `browser http://10.0.2.2:<port>/...` and compare screenshots; **`tools/webcheck.py`** drives the kernel shell's `fetch`. Details in §8.


---

## 4. Control flow and lifecycles

### 4.1 Start-up (browser.c:1313-1348)
1. `win_create("Browser", 860, 620)`; exit(1) on failure; `win_allow_resize`.
2. `argv[1..]` joined with spaces → `go_or_search` (address or DDG search); none → `https://example.com/`.
3. `push_history(address)` (hist_at 0); `css_view_h = 600`; `want_load = 1`, `dirty = 1`.

### 4.2 One pass of the main loop (browser.c:1350-1742)
1. Window size and surface; compute the view rectangle; `css_view_h = view_h`; if `view_w != laid_for` and nothing is loading, `want_width = 1` (1351-1366).
2. `ui_begin`; drain `win_poll`: `WIN_EV_CLOSE` ⇒ exit loop; `WIN_EV_FIND` ⇒ `find_show(ev.y, view_h)`; `WIN_EV_SCROLL` accumulates `scrolled`; everything goes through `ui_feed`; stop draining once 16 keys are held so the rest stay queued for the next pass (1382-1391).
3. Deferred reflow: if `want_width` and no button is down ⇒ `relayout(view_w - 16)`, `laid_for = view_w`, `publish_text` (1397-1403).
4. Every key of the pass (1411-1482; table in §3.7).
5. `jsdom_timers()`; if any ran and `jsdom_changed()` ⇒ relayout (1487-1490).
6. `jsdom_requests()` (at most one XHR, blocking); if one ran and changed the DOM ⇒ relayout (1495-1498).
7. `want_go` (form submission or meta refresh) ⇒ `load_post = go_is_post`, `set_address(go_to)`, `push_history(go_to)`, `want_load = 1` (1502-1509).
8. `scroll -= scrolled * 48`; clamp to `[0, page.height - view_h]` (1511-1516).
9. Dirty check: pointer moved, keys, press/release, wheel, scroll changed or a load pending ⇒ draw; else `sleep_ms(16)` and loop (1530-1538). Timers and XHRs (steps 5-6) run even when idle.
10. `ui_load_theme()` (reads the config from disk on drawn frames).
11. If `want_load`: reset the refresh counter unless this load was the page's own refresh; draw a "fetching..." frame and `win_commit`; `load(address, view_w - 16, 0)` (blocking); put the landed URL (after redirects) in the bar and in the current history entry (1544-1569).
12. `fill` the window; hit-test the pointer if it is inside the view: `over_link = lay_link_at`, `node_under = lay_node_at` (1571-1580).
13. Hover: if `node_under` changed ⇒ `hover_node = node_under`, `relayout`, recompute `over_link` (1587-1592) -- a full relayout whenever the pointer crosses between box-bearing elements, whether or not any `:hover` rule exists.
14. JS click: on release over a node with a live world ⇒ `jsdom_click(node_under)`; if the DOM changed ⇒ relayout; the first handler error goes to the status line once; `preventDefault` ⇒ the release is consumed (1602-1619).
15. Controls (1625-1658): checkbox toggles; radio clears every radio of the same name in the same form (`form_of`) and sets itself; button ⇒ `submit_form(form_of(node))` unless `type=reset` (reset does nothing); text-like controls (and select) ⇒ `focus_control`; `bar.focused = 0`.
16. A release elsewhere (no node, or a non-control node) unfocuses the page control (1663-1666).
17. Link: release over a link ⇒ `url_join(here, href)` ⇒ bar, history, `want_load` (1668-1678).
18. Chrome: toolbar, `<`/`>` (history, re-fetch), `Reload` (`want_load` with whatever is in the bar), the address field (`ui_field_draw`; a click into it sets `bar_fresh`), `Go` (`push_history(address)` + load, **without** `go_or_search`) (1681-1710).
19. Status bar (hovered link URL overrides the status text) (1713-1724).
20. Page: `ui_well`, `draw_page` into a surface cut at the well's bottom, `ui_sunken` (bevel back on top), `ui_scrollbar` (1727-1738); `win_commit`; `sleep_ms(16)`.

### 4.3 Navigation state
Flags: `want_load` (fetch `address` on the next drawn frame), `want_go` (take `go_to` next pass), `load_post` (the next load is a POST of `post_body`; consumed by that load), `go_is_refresh` (the next load came from a meta refresh; keeps `refreshes`), `want_width`/`laid_for` (reflow when the width changed and the mouse is up).

| Trigger | What happens |
|---|---|
| Return in the bar | `go_or_search(address)`, push, load |
| `Go` button | push, load (no search detection) |
| `Reload` | load the bar's current text (no new history entry) |
| `<` `>` or Left/Right keys | save scroll into the entry (never restored), move `hist_at`, load (GET, full re-fetch) |
| Link click | `url_join`, push, load |
| Return in a field / submit button | `submit_form` ⇒ `want_go` ⇒ push, load (POST if the form says so) |
| Meta refresh | ≤ 3 in a row, to a different URL ⇒ `want_go` (GET) |
| Redirects | followed inside `web_send` (≤ 6), invisible to history except that the entry is rewritten to the landed URL |

### 4.4 Building a page (build, browser.c:769-815)
`jsdom_close` → `dom_parse` → `css_init` → UA sheet → accent sheet → all `<style>` → linked sheets (≤ 6) → all `style=""` → `css_index` → pictures (≤ 24, fetched and decoded) → scripts (open world, run all, DOMContentLoaded, load) → `relayout` (possibly twice) → `publish_text` → title. Sheets, the index and `inl[]` are **not** rebuilt afterwards; script changes to classes are seen (the cascade reads the DOM), changes to `style` attributes, new `<style>`/`<link>`/`<img>` elements are not.

### 4.5 One HTTP exchange
`web_get/web_post` → `web_send` (hop loop) → `web_fetch` (retry rule) → `web_fetch_once`:

```
[ka_live && same host/port/scheme] --reuse--> send request
[else] web_drop -> connect / connect_tls --fail--> WEB_ERR_* (TLS: r->how = tls_why)
send in 1400-byte pieces --fail--> web_drop, WEB_ERR_SEND
recv loop: until Content-Length satisfied | buffer ends with 0\r\n\r\n (chunked) |
           NET_EOF | error | 3 consecutive 4-second silences | buffer full (truncated)
keep = headers found && (Content-Length || chunked) && !Connection: close
       keep -> ka_live=1 (socket stays open, including after a truncated body)
       else -> disconnect
parse status, Location, Content-Type, Set-Cookie; dechunk / trim; gunzip
```

Keep-alive is a single slot: any request to another host/port/scheme closes it first. Because the kernel allows only one TLS socket on the whole machine (kernel/syscall.c:786-787) and the browser keeps its TLS socket open between requests, other programs' `connect_tls` get `NET_ERR_BUSY` while the browser idles on an https page.

### 4.6 The script world
Opened in `build` (only if the page has a `<script>` or an `on*` attribute), lives until the next `build` (any navigation, including error pages). Scripts run synchronously during `build` (external ones fetched at their position), then `DOMContentLoaded` and `load` go to document listeners. Afterwards, each main-loop pass runs due timers and at most one queued XHR; clicks are delivered on release. After each of these, `jsdom_changed()` decides whether to relayout. A handler error is reported once per page on the status line (`said_script_err`).

### 4.7 Keyboard focus
Three mutually exclusive destinations, tested in order: the address bar (`bar.focused`, toggled by `ui_field_draw` on releases inside/outside it), a page control (`focus_node >= 0`, set by clicking a control, cleared by Esc, by Return-submit, by clicking a non-control, or by a new page), otherwise the page (scroll/zoom/history keys). Clicking a control clears `bar.focused`.

### 4.8 Hover, find and the display list
- Hover: `hover_node` feeds `cmatch.hover`; only the exact element under the pointer matches `:hover` (not its ancestors), and only elements that produced a box/field/image/bullet can be "under" the pointer.
- Find: `WIN_EV_FIND(n)` ⇒ `find_show(n)` ⇒ `find_item` (an index into `page.items`) + scroll; `draw_page` paints `0xFFE58F` behind that item; `n = -1` clears. `publish_text` runs on build and on width reflows only.
- Relayout triggers: build; width change (deferred until the button is up); zoom; hover change; timers/XHR/clicks that changed the DOM. Each relayout re-runs the whole cascade for every element (style is computed up to three times per block element: `lay_is_block_node`, `lay_block`, `lay_block_placed`), and every flex level measures its children with a full throw-away layout.

---

## 5. Interfaces

### 5.1 What this area provides (and to whom)

| Header / symbol | Used by |
|---|---|
| `web.h` (`url_t`, `url_parse`, `url_join`, `url_text`, `url_looks_like_address`, `url_escape`, `w_*` helpers) | `fetch.h`, `html.h`, `dom.h`, `css.h`, `layout.h`, `jsdom.h`, `browser.c`, `wiretest.c`, `jsprobe.c` |
| `fetch.h` (`web_get`, `web_post`, `response_t`, `WEB_ERR_*`, cookie jar, `web_drop`) | `browser.c`, `wiretest.c`, `jsprobe.c` |
| `html.h` (`T_*`, `HTML_TAGS`, `html_tag_of`, `html_void`, `html_char`, `html_raw_end`) | `dom.h`, `css.h` (`html_tag_of` for tag selectors), `jsdom.h` (`getElementsByTagName`) |
| `dom.h` (`ddoc`, `dom_parse`, `dom_attr*`, mutation, `dom_next`, `dom_by_*`) | `css.h`, `layout.h`, `jsdom.h`, `svg.h` (parses SVG files with `dom_parse` into a malloc'd `ddoc`, svg.h:687-699, and uses `dom_attr`, `dom_attr_fold`, `dom_tag_name`, `dom_next`), `browser.c`, `layouttest.c`, `pagetest.c`, `jsprobe.c` |
| `css.h` (`csheet`, `css_parse`, `css_declare`, `css_parse_selector`, `css_matches`, `css_color`, `CSS_UA`, …) | `layout.h`, `jsdom.h` (query selectors), `svg.h` (`css_color`, svg.h:523), `browser.c`, `layouttest.c`, `pagetest.c` |
| `layout.h` (`lay_run`, `ldoc`, `litem`, `lay_link_at`, `lay_node_at`, `lay_control_kind/label`, `CTL_*`, `lay_show_hidden`, `lay_number`) | `browser.c`, `layouttest.c` |
| The browser program | launched as `/bin/browser` from the desktop launcher (kernel/wm.c:400) or a shell (`browser <address or words>`); publishes its words to the desktop's find via `win_set_text` and receives `WIN_EV_FIND` |

### 5.2 What this area depends on

- **Kernel via sdk/zelr.h**: sockets `connect` (SYS_CONNECT), `connect_tls` (SYS_TLS_CONNECT 45), `tls_what`/`tls_why` (SYS_TLS_STATUS 46), `send`, `recv`, `disconnect`; windows `win_create`, `win_allow_resize`, `win_surface`, `win_width/height`, `win_poll`, `win_commit`, `win_close`, `win_set_text` (SYS_WIN_TEXT 56), `win_find_query` (SYS_WIN_FIND 57); `ticks()` (timers), `sleep_ms`, `exit`, `memset`; `KEY_*` codes, `KEY_CODE`, `KEY_IS_SPECIAL`.
- **Kernel behaviour relied on**: `recv` returns 0 after 4000 ms of silence and `NET_EOF (-2)` at the end (kernel/syscall.c:852-872); plain sends ≤ 1400 bytes, TLS sends ≤ 8192 (840); one TLS socket machine-wide (786-787); sockets owned per pid and released at exit (883-886); certificate verified against the host name inside `connect_tls` (802); DHCP/DNS/TCP from the network area; `JD_HZ 100` must equal the kernel timer rate (jsdom.h:680-686).
- **Userland libraries**: `alloc.h` (`malloc`/`free` for gunzip, the JS region, decoders), `inflate.h` (`inf_run`, `INF_FULL`), `png.h` (`png_decode(data, n, &picture, bg)`, `PNG_OK`, `picture {w, h, u8 *rgb}` 3 bytes/pixel, `picture_free`), `jpeg.h` (`jpeg_decode(data, n, &picture)`, `JPG_OK`), `svg.h` (`svg_render(xml, len, want_w, want_h, &picture, bg)`, `SVG_OK`), `facetext.h`/`face.h` (33 faces), `draw.h` (`surface`, `fill`, `rect`, `round_rect`, `disc`, `mix`), `ui.h` (`ui_input`, `ui_field` + `ui_field_key` + `ui_field_draw`, `ui_button`, `ui_button_primary`, `ui_label`, `ui_toolbar`, `ui_statusbar`, `ui_well`, `ui_sunken`, `ui_scrollbar`, `ui_round`, `ui_load_theme` → `accent`, `bg`, …; `UI_PAD 8`, `UI_GAP 6`, `UI_ROW 26`, `UI_BTN_H 30`, `UI_SCROLL_W 10`, `UI_KEYS 16`), `jsdom.h`/`js.h`/`jsparse.h`/`jsrun.h` (the engine).
- **Network services**: `lite.duckduckgo.com` for searches; the theme's accent colour from the settings file (via `ui_load_theme`).

---

## 6. Concurrency, locking, memory ownership, invariants

- **Single-threaded.** One ring-3 process, one loop; every network operation (page, sheets, pictures, scripts, XHR) blocks the loop. No locks. The kernel's socket table is shared with other programs; the browser holds at most one socket at a time (the kept connection) -- and it may be the machine's only TLS slot (§4.5).
- **Static ownership.** All page state lives in static arrays sized at compile time (`src`, `cssbuf`, `scriptbuf`, `replybuf`, `doc`, `sheet`, `index_`, `page`, `inl`, `hist`, `ck_jar`, `pics`). They are overwritten by the next page. Heap: decoded pictures (`picture.rgb`, freed by `pics_drop` at the next build), gunzip's temporary output buffer (freed before return), the JS region (freed by `jsdom_close`), SVG's temporary `ddoc` (svg.h).
- **Borrowed pointers**: `response_t.body` points into the caller's buffer and is valid until that buffer is reused; `fetch_script`'s returned text lives in `scriptbuf` until the next script fetch (jsdom runs it immediately); `do_request`'s text lives in `replybuf` until the next request (jsdom copies it into a JS string before calling `onload`); `lay_control_label` returns a static buffer shared by all callers (hence `form_query`'s copy into `keep`, 386-394).
- **Arenas only grow**: DOM text/attributes, CSS text/decls, layout text. Removing a DOM node never frees anything; a node count consumed by script churn is not recovered until the next page.
- **Invariants relied upon**:
  - `doc.nodes[0]` is the synthetic root `T_HTML`; `doc.body >= 0` after `dom_parse`.
  - An element's attributes are one contiguous run `attrs[attr_at .. attr_at+attr_n)`; `dom_attr_set` preserves this by moving runs (dom.h:271-279).
  - The tree has no cycles (`dom_append`/`dom_insert_before` refuse them) -- layout and `dom_next` would not terminate otherwise.
  - Rules in each `cindex` chain are in source order (built backwards, css.h:1326-1339), and `css_collect`'s insertion sort is stable, so equal specificity resolves by order.
  - `page.items` is in paint order: a box's slot is reserved before its contents (layout.h:1189-1200, 497-500), so backgrounds are drawn under text and hit tests walk backwards to find the topmost item.
  - Item coordinates are document coordinates; drawing subtracts `scroll` and adds the view origin.
  - `lay_measure` restores every piece of layout state it touches (including `overflowed`) -- any new field added to `lctx` or `ldoc` must be saved/restored there too.
  - Selector queries from scripts append to the live sheet and are rolled back (`nsels`, `used`, `overflowed`) -- the index never references those temporary selectors.
- **Stack**: the user stack is 64 KiB (`USER_STACK_PAGES 16`, kernel/user.c:35-49, sized "against what the work needs" for deep pages). Layout recursion depth follows the DOM depth of block elements (`lay_block` ↔ `lay_block_placed`, plus `lay_flex`/`lay_measure` frames); the parser caps that at 64, but scripts can nest deeper. `lay_inline` alone keeps ~7 KB of arrays; `lay_style` a 2.3 KB hit array per call.

---

## 7. Limits and magic numbers

| Name / value | Where | Meaning |
|---|---|---|
| `URL_HOST 128`, `URL_PATH 640`, `URL_TEXT 792` | web.h:10-12 | host, path+query, printed URL |
| `WEB_MAX_HOPS 6` | fetch.h:720 | redirect fetches per request |
| 1400 | fetch.h:554-576 | send piece (kernel plain-send cap) |
| 32768 | fetch.h:585 | recv piece |
| 3 × 4000 ms | fetch.h:590-595; kernel/syscall.c:860,869 | silence before giving up (~12 s) |
| request buffer 2048 | fetch.h:512 | `URL_PATH + URL_HOST + CK_VALUE + 512` |
| `CK_MAX 64`, `CK_NAME 96`, `CK_VALUE 768`, `CK_DOMAIN 128`, `CK_PATH 160` | fetch.h:196-200 | cookie jar |
| 16 | fetch.h:334 | Set-Cookie headers read per response |
| `HTML_TAGS` 64 known + `T_OTHER`; `T_COUNT 65` | html.h:23-77 | tag table |
| 36 named entities; ≤ 8 digits numeric; name ≤ 12 | html.h:165-201 | entity decoding |
| `DOM_NODES 20000`, `DOM_ATTRS 40000`, `DOM_ARENA 1.5 MiB`, `DOM_DEPTH 64` | dom.h:37-40 | document |
| `CSS_RULES 4000`, `CSS_SELS 9000`, `CSS_DECLS 16000`, `CSS_TEXT 512 KiB`, `CSS_MAXCLS 4` | css.h:31-35 | sheet |
| 32 | css.h:885 | selectors per rule |
| `CSS_BUCKETS 512`, `CSS_HITS 192` | css.h:1306, 1344 | index buckets, matched rules per element |
| spec `1<<20` / `1<<10` / 1 | css.h:517-606 | id / class, attribute, pseudo / tag |
| length clamp [-1, 4000]; offsets ±4000; integer part ≤ 10000 | css.h:1279-1287, 346 | lengths |
| font-size 7..96 px; `smaller` ×5/6, `larger` ×6/5 | css.h:1127-1131 | font size |
| `line_h` default 145 %, clamp 90..300 | css.h:1029, 1201-1202 | line height |
| `pt` × 4/3 | css.h:353 | points to px |
| `CSS_AUTO_OFF -32768` | css.h:64 | auto offset |
| default colours 0x1A1A1A text, 0xFFFFFF background, 0xD0D0D0 border | css.h:1004-1006; layout.h:314 | |
| `LAY_ITEMS 96000`, `LAY_TEXT 1 MiB`, `LAY_LINKS 12000`, `LAY_LINE 400` (unused), `LAY_DEPTH 64`, `LAY_FLEX_MAX 32` | layout.h:40-44, 849 | display list |
| 33 faces: 11,12,13,14,15,16,17,19,21,24,28,34,42 (regular, bold); mono 12,13,14,15,17; mono bold 13,15 | facetext.h:26255-26289 | typefaces |
| baseline 4/5 of height | layout.h:379; browser.c:1038 | text placement |
| `3*cw` or 2000 | layout.h:948 | flex measuring width |
| 8 px | layout.h:973 | flex shrink floor |
| 16 px | layout.h:1184, 1204 | minimum block and content width |
| radius ≤ 40 (boxes), ≤ 255 (fields); field borders ≤ 8 | layout.h:541, 742-749, 1340 | |
| textarea 28 cols × 3 rows; input size 20; check/radio face+2; button label+20; field height face+10 | layout.h:685-705 | control sizes |
| bullet at content x − 18, 14 wide; decimal 6 px left | layout.h:1215-1231 | list markers |
| `SRC_MAX 1 MiB`, `CSS_MAX 512 KiB`, `SHEETS_MAX 6`, `SCRIPT_MAX 128 KiB`, `SCRIPTS_MAX 8`, `REPLY_MAX 128 KiB`, `ASKS_MAX 64`, `HIST_MAX 40`, `PICS_MAX 24`, `REFRESH_MAX 3` | browser.c:58-78, 123, 312 | per-page limits |
| `post_body 4096`, `focus_buf 1024`, `title 160`, `status 888` | browser.c:349, 168, 101, 102 | |
| publish buffer 4096 (= `WM_TEXT_MAX`) | browser.c:244; include/wm.h:77 | find text |
| find query ≤ 63 chars | browser.c:261 | |
| window 860×620; toolbar 46 px; view at (3, 49) | browser.c:1314, 1243, 1356-1359 | chrome |
| scroll: arrows 40 px; page = view_h − 40; wheel 48 px/step | browser.c:1458-1465, 1511 | |
| zoom `root_px` 16, step 2, range 10..28 | browser.c:109, 1466-1469 | |
| find highlight 0xFFE58F; hovered link 0x0842A0; field edge 0x3B6FD6 focused / 0xA9A9A9; button face 0xE6E6EA; field text 0x1A1A1A; alt text 0x6B6B6B | browser.c:1111-1118, 1211, 1221; layout.h:677 | colours |
| frame sleep 16 ms | browser.c:1537, 1741 | ~60 Hz loop |
| `SEARCH_PREFIX "https://lite.duckduckgo.com/lite/?q="` | browser.c:1291 | search |
| default page `https://example.com/` | browser.c:1346 | |
| `JD_LISTENERS 256`, `JD_TYPE_MAX 24`, `JD_TIMERS 64`, `JD_HZ 100`, `JD_REQUESTS 8`, handler source ≤ 4096 | jsdom.h:219-220, 685-686, 766, 311 | script bindings |
| `JS_MEM_CAP 24 MiB`, `JS_STEP_CAP 40 000 000`, `JS_DEPTH_CAP 160`, `JS_CHUNK 256 KiB` | js.h:61-63, 224 | engine caps |
| timers clamped to 1 h, never 0 ticks | jsdom.h:699-707 | |


---

## 8. Tests

The kernel selftest (`-append selftest`) does not touch the browser: everything here is ring 3. (The `[layout] 9 checks` line in the README's selftest listing is kernel/layout.c, the on-disk directory layout -- unrelated.)

### 8.1 `userland/layouttest.c` (ring 3, `LAYOUTTEST_PASS`)
Run by `tools/piccheck.py` (PROGRAMS, line 38) and `tools/ring3check.py` (SUITES, line 40); gate step `pictest`. About 46 assertions:
- flex row of three 60 px boxes: exist; x strictly increasing; equal y (114-136);
- the same markup without flex stacks (same x, increasing y) (142-156);
- `gap:20px` ⇒ 18..22 px between boxes (159-172);
- `justify-content:space-between` ⇒ first x < 40, second x > 400 in 600 px (178-191);
- `justify-content:center` ⇒ single box x in (200, 340) (194-203);
- `flex:1` child takes > 400 px (209-222);
- `flex-direction:column; gap` ⇒ stacked, same x (225-238);
- five 100 px boxes in 300 px ⇒ last box right edge ≤ 320 (245-259);
- `/drawn` page exactly as webserver.py serves it: `<img width=200 height=120>` with a faked 100×60 picture ⇒ LK_IMAGE 200×120 (271-304); no attributes ⇒ 100×60 (307-326);
- `box-sizing` content-box 200+2×20+2×5 = 250, border-box = 200 (335-349);
- `min-width` raises to 180, `max-width` lowers to 120, min beats max = 200 (352-368);
- absolute box at ancestor + (30, 10), and following content closes up at +40 (377-405);
- relative box moved to x = 25, next block still at y = 30 (412-424);
- `<center>` centres an inline-block's box: x in (200, 300) (434-448);
- `::-webkit-scrollbar{width:6px}` and `div:before{width:6px}` do not shrink boxes (461-476);
- `width:960px` is 960 (483-488); `width:50vw` in 600 is 300 (491-496); `height:100%` with nothing to resolve against is auto (505-511).
Not covered: text wrapping, inline edges, line heights, fonts, tables, lists, floats (absent), anything with `inl[]`, the hidden-page fallback, hit testing.

### 8.2 `userland/pagetest.c` (ring 3, `PAGETEST_PASS`)
Run by `ring3check.py` (line 36) and `piccheck.py` (line 39, whose label "a whole page parses, styles and lays out" overstates it). About 71 assertions: `dom_by_id`/tag name/text (93-100); attribute read/missing/change/longer/add-to-older-element/neighbour untouched (102-131); `dom_set_text` (133-134); whitespace rule "Gmail Images", dropped after a block, runs left for the line breaker (137-154); a script that sums 1..10, writes textContent/className/title and reports `changed` (157-191); `getElementsByTagName().length` (194-206); writing to a missing element is an error (209-223); a syntax error is reported (225-232); a page with no script opens nothing (234-240); `onclick` attribute-only page opens a world, fires on `jsdom_click`, sets dirty (249-268); listeners outlive the script, bubble from child to parent, `e.target` is the hit element (272-312); `preventDefault` ⇒ `jsdom_click` returns 1, a passive listener ⇒ 0 (316-334); `stopPropagation` (336-352); `DOMContentLoaded` on document (354-365); a throwing handler is reported and the world survives (369-382); `createElement` + `appendChild`, `removeChild`, `insertBefore`, move-on-append, cycle refused (389-461); `classList` add/remove/contains/toggle (463-477); `parentNode`/`children` (479-490); `setTimeout` 300 ms runs once, not early (498-515); `setInterval` 10 ms repeats (517-531); `clearTimeout` (533-546); `value`/`checked` read/write (550-568); `querySelector(All)` with descendant/compound selectors, element-scoped query, selector lists, and 400 queries not exhausting the sheet (575-635). Clicks are delivered by node index, so the browser's hit test (text runs have no node) is not exercised.

### 8.3 `tools/browsercheck.py` (gate step `browsertest`, 600 s)
Boots with `memory=256` and `-nic user,model=e1000`, runs `dhcp`, starts the desktop, then `browser http://10.0.2.2:<port>/`. Window geometry is recomputed from the WM's rules (lines 33-88: `WIN_X = CASCADE_X + 48`, `WIN_Y = 74`, clamped to the work area; `BACK`, `BAR`, `PAGE` rectangles from `UI_PAD 8`, `UI_GAP 6`, `UI_BTN_H 30`, `UI_ROW 26`, `UI_SCROLL_W 10`). `page_settled` waits for three identical frames 0.4 s apart. Checks (by colour counts in `PAGE` or byte-equality of the page region):
1. first page drawn (> 8 colours) on paper (> 200 000 white pixels);
2. the one link is drawn in the accent `LINK = (0x6E,0x8A,0xE8)` (the default theme preset), found by `centre_of`;
3. clicking it changes the page; the new page differs;
4. Back lands on the first page **pixel for pixel**;
5. `/measured` renders; `/framed` (same `SAMPLE` HTML, chunked in awkward 97-byte chunks) renders **identically** (dechunker check);
6. `/styled` shows the `BAND (0x1D,0x4E,0xD8)` > 3000 px; `/bare` (same markup, no sheets) differs and has < 200 band pixels;
7. `/picture` shows > 10 000 `LOGO (0xE1,0x1D,0x48)` pixels (PNG 160×90); `/missing-picture` < 100 and differs;
8. `/drawn` (SVG at 200×120) > 10 000 logo pixels;
9. `/scripted` shows the band (script set the class); `/unscripted` does not;
10. `/redirect` (302 → `/second`) renders identically to the page reached by the link;
11. `/nothing-here` (404) differs from the previous page;
12. `https://<test server>/` differs from the 404 page (the plain-HTTP server cannot complete a TLS handshake, so an error page appears).

### 8.4 `tools/formcheck.py` (gate `formtest`)
Opens `/form` (webserver.py:314-338): `#q` green `00A000`, `#spare` (no name) `00C0C0`, `#deep` checkbox `C000C0`, `#off` checkbox `808000`, a hidden `from=zelr`, submit `#go` `E08000`, and a rounded `#round` `00A0FF` (`border-radius:14px`). Checks: field, button, checkbox found by colour; hidden field not drawn (it looks for colour `0x000001`, which nothing paints -- vacuous); the rounded field's four corner pixels are not its colour; typing `hello world` changes the page; ticking `#deep` and pressing Search makes the **server** receive a GET with `q=hello+world`, `deep=yes`, `from=zelr`, no `off=`, no `spare=`/`=ignored`; the landing page has a link (≥ 60 accent pixels) to `/posts`; typing `over the wire` there and pressing Send arrives as a POST body `q=over+the+wire`. Typing uses a 0.18 s gap because the guest drops serial input it cannot drain (the docstring blames a kernel sleep that returns early).

### 8.5 `tools/livecheck.py` (gate `livetest`)
`/live-quiet` (scripts stripped): blue bands > 8000 px and < 200 px of each other colour. Then (Alt+Q closes the window; a new browser) `/live`: timer (900 ms) ⇒ ORANGE `FF8000` > 8000; external `/live.js` ⇒ VIOLET `8000FF` > 8000; XHR to `/live.txt` whose `onload` is assigned **after** `send()` ⇒ TEAL `00A0A0` > 8000 (proves the request runs on a later pass); nothing green yet; clicking the first blue band (a div with a background, so it has a box and a node) ⇒ GREEN `00B050` > 8000 (listener ran and the page was re-laid out). `PAGE = (200,120,900,620)` is a fixed screen rectangle.

### 8.6 `tools/webcheck.py` (gate `webtest`)
Uses the kernel shell's `fetch <host> <path>` (kernel/http.c, output `status %d, %d bytes of headers, %d bytes of body`, kernel/http.c:160) -- **not** fetch.h: `/size/1000` twice, `/size/200000` three times (receive-buffer walk, FIN ordering), `/size/40000`, `/slow` (2000 bytes in dribs), 404; the same on a PCnet card including 200 000 bytes; an `ne2k_pci` card is named by its ids `10ec:8029`; one fetch via the instant `guestfwd` path to 10.0.2.100. It validates the TCP stack under the browser, not the browser.

### 8.7 Tests of fetch.h itself: `userland/wiretest.c` + `tools/wirecheck.py` (gate `wiretest`)
`exec /bin/wiretest http://10.0.2.2:<port>`: `/gz` comes back decompressed ("Sent compressed" … "existed."); `/setcookie` then `/whoami` returns `sid=abc123` and `pref=dark`; `/bye` (Max-Age=0) removes `sid` but keeps `pref`; three raw sockets open at once, each gets its own answer (kernel multi-socket check). Server side: ≥ 5 GET requests over ≤ half as many connections (keep-alive), first request carried no cookie, a later one carried `sid=abc123`.

### 8.8 `tools/findcheck.py` (gate `findtest`)
Ctrl+F, type `connection` (a word on the test server's `/` page): > 60 pixels of `0xFFE58F` in the page (the browser's highlight), the find bar shows a count; `zzzqqq` lights nothing; Esc closes the bar. Single-word queries only.

### 8.9 The test server `tools/webserver.py`
`ThreadingHTTPServer` on 127.0.0.1:0, HTTP/1.1, reached from the guest as `10.0.2.2:<port>` through slirp (`HOST_IP`, line 27); `instant_args` uses `guestfwd` to `10.0.2.100:80` (one connection only). Routes (do_GET 432-560): `/`, `/index.html` (PAGE with one link to `/second`), `/gz` (gzip only if the client sent `Accept-Encoding: gzip`), `/one`, `/two`, `/setcookie` (two Set-Cookie), `/whoami` (echo Cookie), `/bye` (`sid=; Max-Age=0`), `/form`, `/posts`, `/said` (GET echo; POST at do_POST 406-413), `/second`, `/styled`, `/style.css`, `/bare`, `/logo.svg`, `/drawn`, `/logo.png`, `/picture`, `/missing-picture`, `/scripted`, `/unscripted`, `/live`, `/live.js`, `/live.txt` ("teal"), `/live-quiet`, `/big` (200 000 bytes), `/size/N`, `/framed` (chunked `SAMPLE`), `/measured`, `/chunked` (50 000 bytes in 4000-byte chunks), `/close` (no length, `Connection: close`), `/redirect` (302 → `/second`), `/redirect-relative` (301 → `second`), `/loop` (302 → itself), `/slow`, else 404. Server-side records: `COUNTS` (connections, GET requests), `COOKIES` (per GET), `RECEIVED` (forms).

### 8.10 Not covered by any test
https inside the browser (no TLS test server; `tlscheck.py` uses the kernel `fetch` against the real web), redirect method rules (307/308, POST→GET), `/redirect-relative`, `/loop`, `/close`, `/chunked`, `/big` in the browser, meta refresh, the retry-once rule, truncation paths, gzip truncation, cookie domain/path/secure rules, the cascade (specificity ties, `!important`, media queries, pseudo-classes, attribute selectors), inline edges and wrapping, lists, tables, `<select>`, radio groups, textareas, keyboard scrolling, the wheel, zoom, history scroll, hover, clicks on text, multi-word find, script `type`, the hidden-page fallback.

---

## 9. How to extend

- **A new HTML tag**: add it to the `T_*` enum and to `HTML_TAGS` at the same position (html.h:23-77; nothing checks the order despite the comment), then decide `html_void` (html.h:91), `dom_is_block`/`dom_closes`/`dom_raw` (dom.h:369-401), give it UA CSS in `CSS_UA` (a selector naming an unknown tag drops the whole selector), and, if it is a control, `lay_control_kind`. `T_COUNT` sizes `cindex.by_tag` automatically.
- **A new CSS property**: `P_*` before `P_COUNT` (css.h:39-52), a `CSS_PROPS` row (373-425) or a shorthand branch in `css_declare` (681), a `cstyle` field, **its default in `css_default_style`** (1003 -- `border_box` shows what happens otherwise), `css_inherit` if it inherits (1037), a `css_apply` case (1054), and its use in layout. Lengths should stay `clen` until layout resolves them.
- **Selectors**: recognise pseudo-classes by full name (the current length/first-letter test at css.h:556-560 lets every unknown pseudo-class match); sibling combinators need a new `CB_*`, parsing at 489-498 and a previous-element walk in `css_matches` (977); attribute value tests need a second list in `csel` (currently attribute presence is encoded as negative class offsets, 591-595). Anything not understood must return 0 (drop), never approximate -- the `::-webkit-scrollbar` and `div:before` regressions are the reason (layouttest 461-476).
- **Layout modes**: `lay_is_block_node` (811) decides block vs inline; `lay_block_placed` (1138) owns block children; an inline-block or table would need a measuring pass like `lay_flex`'s `lay_measure` (853). Every new `lctx`/`ldoc` field touched during layout must be saved and restored in `lay_measure` (field by field: struct copies are `memcpy` calls the freestanding link cannot resolve, layout.h:855-859).
- **Items and drawing**: a new `LK_*` needs a `draw_page` branch (browser.c:1060) and a decision about hit testing (`lay_link_at` only considers LK_TEXT; `lay_node_at` any item with `node >= 0`; text runs currently carry no node). Reserve background slots before contents so paint order stays correct.
- **Form controls**: `CTL_*` + `lay_control_kind`/`lay_control_label` (layout.h:168-239) + sizing in `lay_inline` (682-768) + drawing (browser.c:1108-1196) + click/keyboard handling (1411-1482, 1625-1658) + `form_query` (372).
- **Script bindings**: properties in `jd_host_get`/`jd_host_set` (jsdom.h:921-1092, set `jd_dirty` on any write the layout must see); methods via `js_set` in `jd_element` (105) or on `document` in `jsdom_open` (1162-1189); new global functions via `js_declare` on `jd_J.global`. Anything that changes styles must also refresh what the browser caches (`inl[]`, the sheet index).
- **Networking**: fetch.h is the only userland HTTP client (also used by wiretest and jsprobe). The comment at 454-458 anticipates several kept connections; the kernel still allows only one TLS socket machine-wide (kernel/syscall.c:775-787). New encodings need both `Accept-Encoding` (532) and a decoder branch (686-699).
- **Pitfalls stated in comments**: never re-send a POST after 301/302/303 ("how somebody orders twice", fetch.h:742-748) or retry a request that got any answer (446-449); http→https on the same path is not a loop (733-736); omit the default port from `Host` (518-520); keep `publish_text`'s buffer equal to `WM_TEXT_MAX`; `JD_HZ` must match kernel/timer.c (jsdom.h:680-686); keep at most 16 keys per frame and leave the rest queued (browser.c:1382-1391); reflow only once dragging stops (1395-1396); draw the chrome before the page so a torn frame still has its toolbar (1518-1529); XHR must complete on a later pass (jsdom.h:759-764); a relayout per frame for hover must be avoided (browser.c:1582-1586).

---

## 10. Doc drift and suspicious code

### 10.1 Likely bugs (verified by reading; not run)

1. **Inline skip escapes its subtree -- content laid out twice** (layout.h:600-608 and 762-767). `lay_block_placed` calls `lay_inline(child)` for every non-block child (layout.h:1286-1287), including `display:none` elements (`<script>`, `<style>`, `<noscript>`, `<svg>`, `<iframe>`, hidden elements) and form controls (including `<input type=hidden>`). Inside `lay_inline`, when `at == node` and `node` has no next sibling, the climb `while (skip >= 0 && next < 0 && parent != node) skip = parent` rises above `node` (its parent is never `node`), `at` becomes the next sibling of some ancestor, and the walk continues through the rest of the document, laying it out inline on the current line; the ordinary block walk then lays it out again. The `if (skip == node) at = -1;` guard only covers the case where `node` has a next sibling. Pretty-printed HTML hides this (a trailing whitespace text node follows the element, dom.h:521-535); tight or minified markup such as `<div><b>x</b><script>…</script></div><p>…` or `<form><input name=q><input type=submit></form><p>…` triggers it. Fix: `if (at == node) { at = -1; continue; }` before the climb.
2. **Block-level and flex-item links are not links** (layout.h:775-783 is the only place a link is registered, and only when `lay_inline` pushes an `<a>`). An `<a>` with `display:block`, or any `<a>` that is a flex item (flex items are laid out through `lay_block`, layout.h:1029), produces text with `link = cur_link = -1`: not clickable, no hover URL. Common in navigation bars and cards.
3. **`cstyle.border_box` is never initialised** (css.h:1003-1032 omits it; `css_inherit` does not copy it). `lay_block_placed` reads it (layout.h:1165-1169, 1308) from a stack `cstyle`, so for any element without an explicit `box-sizing` declaration the width/height/min/max frame adjustment depends on stale stack contents. layouttest sets `box-sizing` explicitly in the one test that measures it (336-348).
4. **Relative font sizes compound across the cascade** (css.h:1126-1132 resolves `em`/`%`/`smaller`/`larger` against the current `st->font_px`, which earlier-applied rules have already changed; layout.h:475-480 applies every matching rule in order). UA `h1{font-size:2em}` plus a page `h1{font-size:1.5em}` gives 3em. Duplicate hits make it worse: an element whose `id` and a class hash to the same bucket, two classes colliding in the 512 buckets, or a repeated class token, walks that chain twice (css.h:1377-1399) and applies the same relative size twice.
5. **`rgba(…, .5)` is opaque** (css.h:287-298): `any` is set only by integer digits, so an alpha written without a leading zero (the minified form) breaks out before `c[3]` is stored and alpha stays 255 -- `rgba(0,0,0,.1)` borders and overlays draw solid black.
6. **`font:` shorthand takes a numeric weight as the size** (css.h:753-760): the first part that `css_len` reads as a positive length wins, and a unitless `400`/`700` is a length in px, so `font: 700 14px/1.4 sans-serif` sets `font-size:700` (clamped to 96 px).
7. **Named colours are only found as the last token of a multi-part value** (css.h:311-312 uses `w_same_fold`, a whole-string compare, on slices from `css_parts` at 723, 783 and 1267 that are not NUL-terminated): `border: solid black 1px` loses its colour; `background: white url(x.png)` is dropped. Hex and `rgb()` are unaffected.
8. **Unknown pseudo-classes match everything** (css.h:556-576): anything argument-less that is not hover/link/visited/root/11-letters is accepted with no test, so `li:last-child{border:0}`, `a:focus{…}`, `input:checked{…}` apply unconditionally. And **`:visited` matches every link** (css.h:959-962): `cmatch.visited_links` (937) is never read, although browser.c:661 sets it to 0 expecting `:visited` to match nothing -- `a:link{…} a:visited{purple}` colours every link purple.
9. **Negative lengths become `-1`, which means auto** (css.h:1285-1287): negative margins are lost (`lay_block_placed` treats `< 0` as 0, layout.h:1146-1147), and a box with both side margins negative and a width is centred as if `margin: auto` (1174, 1178).
10. **The page is not clipped at the top of the view** (browser.c:1060-1080, 1727-1735): `draw_page` draws every item overlapping `[-8, vh+8]` into a surface cut only at the bottom; `rect`/`round_rect`/`tface_draw` clip to the window, not the well. The toolbar is drawn before the page in the same frame, so a background box (e.g. a body or wrapper with a background colour) scrolled partly above the view paints over the toolbar, and text straddling the top edge leaves fragments above the well. `ui_sunken` only restores the bevel. Images are clipped correctly (1092-1097).
11. **Flex rows are as tall as a measurement taken at triple width** (layout.h:948, 1021, 1037, 1048): children are measured at `3*cw`, laid out at their final (narrower) widths, but the row height is the tallest *measured* height; a child whose text wraps more at its real width overflows into what follows. Also in measurement: a child with a background or border reports the full `3*cw` width (its LK_BOX spans `avail`, 1329-1331), so it asks for the whole row; a child without one reports its right-most *content* edge, excluding its right padding/border.
12. **Flex drops text children and items past 32** (layout.h:914-917): `display:flex; …>Hello <b>x</b>` loses "Hello"; children after the 32nd are never laid out. `display:none` items still take 1 px plus a gap. `flex-wrap` is parsed (css.h:1101-1103) and ignored.
13. **Text runs have no node, so clicks and `:hover` target the nearest box-bearing element** (`lay_word` never sets `it->node`, layout.h:405-418; `lay_node_at` skips `node < 0`, 1408-1416). `jsdom_click(node_under)` bubbles *up* from that element (jsdom.h:360-366), so an `onclick` on a `<div>`/`<span>`/`<a>` without a background or border is never reached when its text is clicked (the tests click by node or on a coloured band). `:hover` likewise applies only to box-bearing elements, and only to the element itself, not its ancestors (css.h:959).
14. **Image links are not clickable** (layout.h:1400: `lay_link_at` ignores LK_IMAGE, although images carry `link`, 658). Logos wrapped in `<a>` do nothing.
15. **Every button submits** (browser.c:1649-1653): `<button type="button">` and `<input type="button">` (all mapped to `CTL_BUTTON`, layout.h:187, 199-201) call `submit_form` unless a click handler prevented it; `type=reset` does nothing.
16. **The Go button skips search detection** (browser.c:1706-1710): only Return runs `go_or_search` (1421); Go loads the raw bar text, so words typed then Go try DNS on "operating system".
17. **Stale inline styles** (browser.c:517-553): `inl[]` is reset only for indices `< doc.count` at build time, before scripts run. Nodes created by scripts later (higher indices) read `inl[]` entries left from a previous, larger page -- declaration ranges into the current sheet's arrays, i.e. wrong styles. Style attribute changes made by scripts (`setAttribute('style', …)`) are never re-parsed.
18. **`<style>` sheets always precede linked sheets** (browser.c:778-779): source order is not document order, so a page's override `<style>` placed after its `<link>` loses equal-specificity ties.
19. **`show_plain` writes into the buffer it reads** (browser.c:837-853): `reply.body` points into `src` (load() fetched into `src`, 883-885), and the expanded output (a 74-byte header, `<`→`&lt;`, `&`→`&amp;`) is written to `src[0…]`. The writer gains 3-4 bytes per escaped character, so once those gains exceed the slack (header length − 74) it overwrites input not yet read: text/plain or JSON bodies with many `<`/`&` display garbled. (Bounds are respected; no crash.)
20. **`host?query` produces a request target without `/`** (web.h:205): the `/` is prepended only if the next char is neither `/` nor `?`, so `http://example.com?x=1` requests `GET ?x=1 HTTP/1.1`.
21. **Query-only and query-bearing relative links resolve wrongly** (web.h:263-274): the "directory" is up to the last `/` anywhere in `path`, query included, and `?page=2` replaces the last path segment rather than just the query: base `/w/index.php?title=A` + `?title=B` ⇒ `/w/?title=B`; base `/a/b?next=/c/d` + `e` ⇒ `/a/b?next=/c/e`. Pagination links (`?page=2`) break unless the path ends in `/`.
22. **Relative links with an early colon are rejected** (web.h:246-249): any `:` in the first 12 chars before a `/` is taken as a scheme (`?t=10:30`, `a:b.html`).
23. **A truncated response keeps its connection** (fetch.h:584, 635-646): when the buffer fills before the body ends (`truncated`), a Content-Length or chunked response is still kept alive; the unread remainder is what the next request on that connection reads first. The retry rule rescues it only when that garbage produces `WEB_ERR_EMPTY` (no status); `WEB_ERR_HEADERS` is returned as is.
24. **A truncated gzip stream discards everything** (fetch.h:176): only `INF_FULL` keeps the decoded front; `INF_TRUNCATED` (stream cut short) fails the whole fetch with `WEB_ERR_ENCODING`, contrary to the "keep the front of the page" philosophy stated at 170-173.
25. **Chunked end detection is a tail heuristic** (fetch.h:617-629): it stops when the buffer *ends* with `0\r\n\r\n` -- a chunk whose data ends in `0\r\n` arriving exactly at a read boundary looks like the terminator (truncated page); a bare-LF terminator is never seen (waits ~12 s).
26. **Cookie scope and script access** (fetch.h:219-226, 294-298, 536-541; browser.c:626-635): `Domain=com` is accepted from `example.com` (no public-suffix check); cookies are attached to every request including sheets, pictures, scripts and XHR; there is no same-origin check on XHR and responses are readable by the page -- so a page's script can read another site's responses made with that site's cookies. The comment claiming "no cookies are sent with it" is false.
27. **Wheel direction looks inverted** (browser.c:1511 `scroll -= scrolled * 48`): the kernel documents wheel steps as positive downward (include/wm.h:43-45; selftest "two fingers moved down the pad scroll down" expects +2, kernel/selftest.c:2804-2805), and every other app adds (`music.c:388`, `monitor.c:302`, `settings.c:790`, `files.c:509`, `notes.c:282`; `term.c:1835` subtracts only because its `view` counts from the end).
28. **Field caret and textarea** (browser.c:1190-1194): the caret is drawn after the visible text even when `ui_field_key` moved the cursor (Left/Home); a textarea shows one line (newlines drawn as spaces, 1169) and Return cannot insert a newline (`ui_field_key` ignores keys < 32).
29. **Radio via keyboard** (browser.c:1448-1451): Space toggles a radio on or off without clearing the others of its name.
30. **`<select>`** : it shows and submits its first option's *text* (layout.h:220-229; `form_query` via `field_value`), `value` attributes of options are ignored, and it cannot be changed; focusing one and typing writes `value=""` onto the select (browser.c:1452-1455, since `focus_field` is not focused and `field_set_value` runs anyway).
31. **Find only works for single words in the first 4 KB** (browser.c:243-284): the published text is capped at 4096 bytes (so is the kernel's copy), and `find_show` matches inside individual word items, while the desktop counts matches in the space-joined text -- a query with a space is counted by the desktop but never highlighted or scrolled to. `publish_text` is not re-run after script-driven relayouts.
32. **History does not restore scroll** (browser.c:1014-1026, 1471, 1476, 1684, 1691 save `hist[].scroll`; nothing reads it; `load()`'s `keep_scroll` is always 0).
33. **Media handling** (browser.c:589 and 556-569 ignore `media` on `<link>`/`<style>`, so print sheets apply; css.h:841-852 opens every non-print `@media`, including `max-width` mobile overrides and `prefers-color-scheme: dark`, which then win by source order; `@import` is never fetched; `@layer` blocks are skipped whole).
34. **Unknown elements never close** (dom.h:566 ignores close tags of `T_OTHER`): `<my-el>a</my-el> b` puts `b` inside `my-el`; `<tbody>`, `<u>`, `<sup>`, custom elements accumulate following siblings as children until a known ancestor closes. This also feeds svg.h, which parses SVG with `dom_parse` and composes transforms up the parent chain (svg.h:753-762): after `<g transform=…>…</g>`, later siblings become children of the `<g>` and inherit its transform.
35. **Unquoted attribute ending in `/` self-closes** (dom.h:640): `<a href=/>Home</a>` or `<a href=https://x.com/>` is treated as `<a/>` and its text is not a link.
36. **`<` not followed by a letter starts a tag** (dom.h:571-591): text like `a < b` or `<3` creates a nameless/odd element that swallows text up to the next `>` and stays open.
37. **`&#0;` writes a NUL into the text arena** (html.h:176, 216 → `html_fold_cp(0)`), truncating that text node; other control code points are passed through.
38. **Script `type` is ignored** (jsdom.h:1248-1281): `application/ld+json`, templates and `type=module` are run as classic scripts; the first error recorded (`jd_note_error` keeps only the first) can mask a real one.
39. **`load` events go to `document` only** (jsdom.h:1293-1297 dispatch with node -1): `window.addEventListener('load', …)` throws (window is the global object without that method), `window.onload` and `<body onload>` never run; XHR supports only `onload`.
40. **`visibility:hidden` removes the box and its space** (layout.h:1084, 1144), unlike CSS; `opacity` below 0.2 does the same (css.h:1189-1194).
41. **`transparent` paints opaque white** (css.h:231, 1059-1060 set `has_bg`): a transparent button on a coloured header becomes a white box.
42. **`rem` and `html` styles** (layout.h:1374-1379): layout starts at `<body>` with a default parent style, so `html{font-size;color;background}` never reaches the body, and `rem` is always the browser's `root_px` (16 by default), e.g. `html{font-size:62.5%} body{font-size:1.6rem}` renders at 25.6 px.
43. **Greedy descendant matching** (css.h:991-995): the nearest matching ancestor is taken without backtracking, so `A > B C` fails when the nearest `B` is not a child of `A` but a farther one is.
44. **The kept TLS connection blocks other programs** (fetch.h:635-646 keep; kernel/syscall.c:786-787): after the browser loads an https page it holds the machine's single TLS slot until it navigates elsewhere or exits; `get https://…` in a shell then fails with BUSY.
45. **Dead or no-op code**: `int ax = *y >= 0 ? x : x;` (layout.h:1091); `L->list_count[d] = L->list_count[d];` (1237-1238); `case 0x00D7` unreachable (html.h:138); `LAY_LINE` (layout.h:43), `css_put_lower` (css.h:205), `dom_get` (dom.h:89), `ck_forget_all` (fetch.h:213 -- cookies cannot be cleared); the whole flat parser in html.h (44-59, 245-445); UA selectors `fieldset,address` (unknown tags ⇒ dropped) and the duplicate `center` rule (css.h:1427, 1432/1467); `WEB_ERR_CONNECT` in the retry set cannot occur on a reused connection (fetch.h:711).
46. **Other gaps worth knowing**: `inline-block` and `table-cell` are laid out as plain inline (width/height/vertical padding ignored; layout.h:811-822); block elements inside an inline run are flattened; `!important` is stripped but not honoured (css.h:683-695); no margin collapsing; `text-align:justify` is left; italic, `LS_CIRCLE`/`LS_SQUARE` and `pre` underlines are not drawn; binary content types are parsed as HTML (browser.c:904-913); 5xx pages say "not there" (920); images > 512 KB fail (they share `cssbuf`); external scripts > 128 KB run truncated (611-624, `truncated` not checked); the window title is not updated when a script changes `document.title` after load; a page nested deeper than 64 reports "more markup than this can hold" although kernel/user.c:40-41 calls 100-deep nesting ordinary.

### 10.2 Comments and docs that disagree with the code

- fetch.h:426-431 -- "a program here has no allocator": alloc.h is included (17) and `wh_gunzip` mallocs (167).
- fetch.h:525-528 -- "identity, because there is no decompressor here … Close, because this makes one request per connection": the code sends `Accept-Encoding: gzip` and `Connection: keep-alive` (532-533).
- css.h:14-16 -- positioning listed as absent (it is implemented: css.h:1178-1184, layout.h:1086-1133); css.h:18 -- "Nothing is allocated, here or anywhere in this program" (the browser allocates for JS, gunzip, pictures).
- layout.h:9-16 -- "absolute and fixed positioning, flex and grid tracks" listed as deliberately absent; flex (908-1049) and positioning are implemented and tested.
- README.md:1150-1153 -- "What is still absent … floats, positioned boxes, table column widths and grid": positioned boxes exist.
- README.md:787-788 -- lists forms support; `<select>` is drawn but inert. README.md:1155-1158 says bodies are "asked for compressed" -- gzip only (the brief's "gzip/deflate" is not what the code does).
- dom.h:117 and jsdom.h:46-47 -- "twelve thousand" nodes; `DOM_NODES` is 20000 (dom.h:37).
- dom.h:288-292 -- "Everything that traverses the tree uses this rather than recursion": `lay_block`/`lay_block_placed`/`lay_flex`/`lay_measure` recurse.
- html.h:1-13 -- "Not a tree … the layout below needs": describes the dead flat parser; html.h:62-64 -- the tag-table check "at the bottom of this file" does not exist.
- web.h:97-106 -- the https-default comment sits above `url_looks_like_address` but describes `url_parse`.
- browser.c:115-117 and tools/browsercheck.py:207-209, tools/wirecheck.py:6-8 -- "one connection at a time is all this kernel's TCP does": the kernel has a socket table (kernel/syscall.c:700-745); only TLS is single. browser.c:481-482 (`WEB_ERR_BUSY` text "the one connection this machine has") has the same drift.
- browser.c:628-632 -- XHR "no cookies are sent with it": they are (fetch.h:536-541).
- browser.c:865 -- `keep_scroll` parameter is always 0; the history `scroll` field is write-only.
- sdk/zelr.h:429-431 -- `connect_tls`: "Zero means the connection is open": it returns the socket handle (kernel/syscall.c:808), which fetch.h relies on.
- tools/browsercheck.py:387-392 -- "an address it cannot speak … https says it cannot do that": https is supported; the check passes only because the test server has no TLS.
- tools/formcheck.py:184-186 -- the hidden-field check searches for colour `0x000001` and can never fail.
- tools/piccheck.py:39 -- pagetest "parses, styles and lays out": it does neither styling nor layout.

---

## 11. Open questions

1. Does the wheel really scroll backwards in QEMU/hardware (10.1 #27), or does the PS/2 path deliver the opposite sign from the documented one?
2. Has the uninitialised `border_box` (10.1 #3) shown up as layout that changes between relayouts (e.g. on hover)? A one-line fix in `css_default_style` would settle it.
3. Does the top-edge overdraw (10.1 #10) appear on real pages? Nothing in the WM or `ui_well` clips it as far as the code shows.
4. How often does the inline-skip escape (10.1 #1) fire on real (minified) pages? It would show as duplicated text and a doubled page height.
5. Is the flat tokenizer in html.h meant to be removed, or kept for something planned?
6. fetch.h:454-458 plans several connections "a layer above this one" (a page and its pictures at once). Is that still intended, given the kernel's single TLS session state?
7. Should XHR/sub-resource requests carry cookies at all without an origin check (10.1 #26)? The code comment assumes they don't.
8. `JD_HZ 100` is asserted to match kernel/timer.c; the kernel area should confirm the tick rate.
9. The search engine is an external dependency (`lite.duckduckgo.com`); is there a fallback if it changes its markup or blocks the user agent `zelr`?
10. What exactly `tls_what`/`tls_why` print, and whether `tls_status`'s machine-wide "last handshake" can report another program's handshake to the browser (kernel TLS area).
11. Whether `svg.h` compensates for dom.h's never-closing unknown elements (10.1 #34) -- the SVG agent should confirm the `<g>` transform leak.
