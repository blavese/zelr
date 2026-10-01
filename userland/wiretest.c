/* What the browser's own fetching does with a real server.
 *
 * The `fetch` command in the shell is the kernel's http client and is a
 * different program from this one: what the browser uses is userland's
 * fetch.h, and nothing but the browser used it, so nothing but a screenshot
 * could check it. A screenshot is a poor way to ask whether a body arrived
 * compressed, whether a cookie came back, or whether two requests shared a
 * connection.
 *
 * So this runs in the machine, over the same stack, against the same test
 * server, and says what it found. What it cannot see from in here -- how
 * many connections were opened -- the server counts instead.
 *
 *   exec /bin/wiretest http://10.0.2.2:8000
 */
#include "zelr.h"
#include "alloc.h"
#include "web.h"
#include "fetch.h"

static int failed;

static void ok(const char *what, int cond) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    putc('\n');
    if (!cond) failed++;
}

static char buf[128 * 1024];
static response_t reply;

static int get(const char *base, const char *path) {
    char full[URL_TEXT];
    int n = 0;
    for (const char *p = base; *p && n < (int)sizeof(full) - 1; p++)
        full[n++] = *p;
    for (const char *p = path; *p && n < (int)sizeof(full) - 1; p++)
        full[n++] = *p;
    full[n] = 0;

    url_t u;
    if (!url_parse(full, &u)) return -1;
    reply.len = 0;
    int rc = web_get(&u, buf, (int)sizeof(buf), &reply);
    return rc < 0 ? rc : reply.len;
}

/* Whether a block of text holds a word. web.h compares whole strings and
   folds case; what is wanted here is a search, so here it is. */
static int holds(const char *hay, const char *needle) {
    for (int i = 0; hay[i]; i++) {
        int k = 0;
        while (needle[k] && hay[i + k] == needle[k]) k++;
        if (!needle[k]) return 1;
    }
    return 0;
}

static int body_has(const char *want) {
    for (int i = 0; i < reply.len; i++) {
        int j = 0;
        while (want[j] && i + j < reply.len && reply.body[i + j] == want[j]) j++;
        if (!want[j]) return 1;
    }
    return 0;
}

/* Asks a secure socket for the site's front page and says whether an HTTP
   answer came back on it. */
static int answers(int sock, const char *host) {
    char req[256];
    int n = 0;
    const char *parts[] = { "GET / HTTP/1.1\r\nHost: ", host,
                            "\r\nConnection: close\r\n\r\n" };
    for (int p = 0; p < 3; p++)
        for (const char *c = parts[p]; *c && n < (int)sizeof(req) - 1; c++) req[n++] = *c;
    if (send(sock, req, n) != n) return 0;
    static char head[512];
    int got = 0;
    for (int tries = 0; tries < 8 && got < 12; tries++) {
        int r = recv(sock, head + got, (int)sizeof(head) - 1 - got);
        if (r < 0) break;                 /* NET_EOF or an error */
        got += r;
    }
    head[got] = 0;
    return got >= 12 && head[0] == 'H' && holds(head, "HTTP/1.");
}

/* Two encrypted connections at once, to two sites. There was one on the
   whole machine: a second was refused with NET_ERR_BUSY while the first was
   open, which is also what every other program got while the browser sat on
   an https page. Each is asked for its page with the other still open, the
   second first. Only for tlscheck, which reaches the real web. */
static int two_secure(const char *a, const char *b) {
    int s1 = connect_tls(a, 443);
    ok("a secure connection opens", s1 >= 0);
    int s2 = connect_tls(b, 443);
    ok("and a second opens while the first is still open", s2 >= 0);
    if (s1 >= 0 && s2 >= 0) {
        ok("the second answers", answers(s2, b));
        ok("and so does the first, which was waiting all that time", answers(s1, a));
    }
    if (s1 >= 0) disconnect(s1);
    if (s2 >= 0) disconnect(s2);
    puts(failed ? "WIRETEST_FAIL\n" : "WIRETEST_PASS\n");
    return failed;
}

int main(int argc, char **argv) {
    const char *base = argc > 1 ? argv[1] : "";
    if (!strcmp(base, "two-secure") && argc > 3) return two_secure(argv[2], argv[3]);
    if (!base[0]) {
        puts("wiretest needs an address to ask\nWIRETEST_FAIL\n");
        return 1;
    }

    /* --- compressed ------------------------------------------------------
     *
     * The length is the point as much as the words are: a client that
     * handed the packed bytes straight to the parser would find no words in
     * them, and one that never asked for compression would find the words
     * and prove nothing. */
    int n = get(base, "/gz");
    ok("a page the server compressed comes back at all", n > 0);
    ok("and is the words rather than the packed bytes",
       body_has("Sent compressed"));
    ok("and the whole of them", body_has("existed."));

    /* --- cookies ---------------------------------------------------------- */
    get(base, "/setcookie");
    get(base, "/whoami");
    ok("a cookie the server set is sent back to it", body_has("sid=abc123"));
    ok("and so is a second one from the same answer", body_has("pref=dark"));

    get(base, "/bye");
    get(base, "/whoami");
    ok("one the server deleted is not sent again",
       !body_has("sid=abc123"));
    ok("and deleting it left the others alone", body_has("pref=dark"));

    /* --- who is asking ------------------------------------------------------
     *
     * The Referer the server is sent, under the default policy: the asking
     * page whole when it is on the same site, its origin when it is not, and
     * nothing when it was encrypted and this is not. */
    {
        char from[URL_TEXT], want[URL_TEXT];
        w_copy(from, sizeof(from), base, sizeof(from));
        w_copy(from + w_len(from), (int)sizeof(from) - w_len(from), "/from/page?q=1#frag", (int)sizeof(from) - w_len(from));
        w_copy(web_referrer_from, sizeof(web_referrer_from), from, sizeof(web_referrer_from));
        get(base, "/referer");
        w_copy(want, sizeof(want), "referer=", sizeof(want));
        w_copy(want + 8, (int)sizeof(want) - 8, from, (int)sizeof(want) - 8);
        want[w_len(want) - 5] = 0;                       /* less the fragment */
        ok("a request from a page on the same site says which page, less its fragment", body_has(want));
        w_copy(web_referrer_from, sizeof(web_referrer_from), "http://elsewhere.test:8080/a/b?c", sizeof(web_referrer_from));
        get(base, "/referer");
        ok("one from another site says only that site", body_has("referer=http://elsewhere.test:8080/<"));
        w_copy(web_referrer_from, sizeof(web_referrer_from), "https://secure.test/private", sizeof(web_referrer_from));
        get(base, "/referer");
        ok("and one from an encrypted page to a plain one says nothing", body_has("referer=nothing"));
        web_referrer_from[0] = 0;
        get(base, "/referer");
        ok("nor does one from no page", body_has("referer=nothing"));
    }

    /* --- other methods, bytes and a page's headers -----------------------
     *
     * What a page's fetch sends (jsnet.h, through browser.c): a method other
     * than GET and POST, a body of bytes with a NUL in it and no type, the
     * header lines the page set; and an answer to HEAD, which says how long
     * a body is that never comes. */
    {
        char full[URL_TEXT];
        w_copy(full, sizeof(full), base, sizeof(full));
        w_copy(full + w_len(full), (int)sizeof(full) - w_len(full), "/asked", (int)sizeof(full) - w_len(full));
        url_t u;
        url_parse(full, &u);
        static const char bytes[4] = { 1, 0, 2, (char)255 };
        web_method = "PUT";
        web_body_len = 4;
        web_body_type = "";
        web_extra = "X-Page: set by the page\r\nAccept: application/json\r\n";
        reply.len = 0;
        web_send(&u, bytes, buf, (int)sizeof(buf), &reply);
        web_method = 0; web_body_len = -1; web_body_type = 0; web_extra = 0;
        ok("PUT arrives as PUT with every byte of its body, a NUL among them, and no type",
           body_has("method=PUT bytes=1,0,2,255 type=none"));
        ok("with the page's own header, and its Accept in place of the browser's",
           body_has("x-page=set by the page") && body_has("accept=application/json") && !body_has("accept=text"));
        int has_answer = 0;
        for (int i = 0; reply.head && i + 8 < reply.hlen; i++)
            if (reply.head[i] == 'X' && reply.head[i + 1] == '-' && reply.head[i + 2] == 'A') has_answer = 1;
        ok("and its answer's headers are kept for the page to read", has_answer);
        web_method = "DELETE";
        url_parse(full, &u);
        web_send(&u, 0, buf, (int)sizeof(buf), &reply);
        web_method = 0;
        ok("DELETE arrives as DELETE", body_has("method=DELETE bytes= type=none"));
        web_method = "HEAD";
        url_parse(full, &u);
        int began = ticks();
        int hr = web_send(&u, 0, buf, (int)sizeof(buf), &reply);
        int took = ticks() - began;
        web_method = 0;
        ok("an answer to HEAD is not waited on for the body its length promises",
           hr == 200 && reply.len == 0 && !reply.cut && took < 300);
    }

    /* --- an answer that stops short --------------------------------------
     *
     * The server says 2000 bytes, sends 500 and goes quiet for longer than a
     * fetch waits; then it sends the rest, which is made to look like an
     * answer of its own. The fetch has to say the page stopped short, and
     * must not keep the connection: the next request down it read the rest
     * of this answer as its own. */
    int stalled = get(base, "/stalls");
    ok("an answer that stops before its length says so",
       stalled == 500 && reply.cut && !reply.truncated);
    get(base, "/one");
    ok("and the answer after it is its own, not the rest of that one",
       body_has("marker-alpha") && !body_has("stale-tail"));
    get(base, "/two");
    ok("and so is the one after that", body_has("marker-beta") && !reply.cut);

    /* An early answer (103) ahead of the real one was taken for the answer. */
    int early = get(base, "/early-hints");
    ok("an early answer ahead of the real one is passed over",
       early > 0 && reply.status == 200 && body_has("marker-early"));

    /* No Content-Length on a 204, which has no body: it was waited on for
       three quiet reads, twelve seconds. */
    int began = ticks();
    get(base, "/no-body");
    int took = ticks() - began;
    ok("an answer with no body is not waited on for one",
       reply.status == 204 && reply.len == 0 && !reply.cut && took < 300);

    /* --- three at once ----------------------------------------------------
     *
     * This is the one that could not be written before. The stack held a
     * single connection in file level variables: a second connect had
     * nowhere to put itself, and an arriving segment had nothing to be
     * matched against except the only connection there was.
     *
     * So all three are opened first, and only then is anything sent. Doing
     * them one at a time would pass against a stack that can only manage
     * one, because each would be finished before the next began. What this
     * asks is whether three can be open at the same moment, be told apart
     * when their answers arrive, and each end up with its own bytes.
     *
     * It uses the sockets directly rather than fetch.h, which keeps one
     * connection at a time by design. */
    {
        int host_len = 0;
        char host[URL_HOST];
        int port = 80;
        url_t u;
        if (url_parse(base, &u)) {
            for (const char *p = u.host; *p && host_len < URL_HOST - 1; p++)
                host[host_len++] = *p;
            port = u.port;
        }
        host[host_len] = 0;

        int s1 = connect(host, port);
        int s2 = connect(host, port);
        int s3 = connect(host, port);
        ok("three connections open at once", s1 >= 0 && s2 >= 0 && s3 >= 0);
        ok("and they are three different sockets",
           s1 != s2 && s2 != s3 && s1 != s3);

        if (s1 >= 0 && s2 >= 0 && s3 >= 0) {
            /* Different paths, so the answers can be told apart by what is
               in them rather than by the order they arrive in. */
            static const char *req[3] = {
                "GET /one HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                "GET /two HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                "GET /gz HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
            };
            int sk[3] = { s1, s2, s3 };
            int sent_ok = 1;
            for (int i = 0; i < 3; i++) {
                int n2 = 0;
                while (req[i][n2]) n2++;
                if (send(sk[i], req[i], n2) < 0) sent_ok = 0;
            }
            ok("a request goes out on each of them", sent_ok);

            /* Read them round robin, so no connection is drained before
               another is touched. A stack that mixed two streams together
               would show up here and nowhere else. */
            static char got[3][4096];
            int len[3] = { 0, 0, 0 }, done[3] = { 0, 0, 0 };
            for (int round = 0; round < 400; round++) {
                int live = 0;
                for (int i = 0; i < 3; i++) {
                    if (done[i]) continue;
                    live = 1;
                    int room = (int)sizeof(got[i]) - 1 - len[i];
                    if (room <= 0) { done[i] = 1; continue; }
                    int n2 = recv(sk[i], got[i] + len[i], room);
                    if (n2 == NET_EOF || n2 < 0) { done[i] = 1; continue; }
                    len[i] += n2;
                }
                if (!live) break;
            }
            for (int i = 0; i < 3; i++) got[i][len[i]] = 0;

            ok("all three answers arrive", len[0] > 0 && len[1] > 0 && len[2] > 0);

            /* And each socket has its own answer rather than a share of one
               stream. Every reply carries the path it was for. */
            ok("and each one is the answer to its own request",
               holds(got[0], "marker-alpha") && holds(got[1], "marker-beta")
               && holds(got[2], "Sent compressed"));

            for (int i = 0; i < 3; i++) disconnect(sk[i]);
        }

        /* --- how long a read waits ------------------------------------------
         *
         * sock_wait (syscall 68): a read of a connection nothing is coming
         * on takes as long as the socket was told to wait. A WebSocket is
         * asked on every pass of the browser's loop, and the four seconds a
         * read waited otherwise froze the browser for as long as the server
         * was quiet. The server here waits for a request this never sends. */
        int s = connect(host, port);
        ok("a connection to read from", s >= 0);
        if (s >= 0) {
            char b[16];
            int set0 = sock_wait(s, 0);
            int t0 = ticks();
            int r0 = recv(s, b, (int)sizeof(b));
            int quick = ticks() - t0;
            ok("a socket told not to wait gives nothing back at once", set0 == 0 && r0 == 0 && quick < 10);
            sock_wait(s, 300);
            t0 = ticks();
            int r1 = recv(s, b, (int)sizeof(b));
            int waited = ticks() - t0;
            ok("and one told to wait 300 ms waits that long", r1 == 0 && waited >= 25 && waited < 200);
            ok("and a wait past a minute is refused", sock_wait(s, 60001) < 0);
            disconnect(s);
            ok("and so is a socket that is not open", sock_wait(s, 0) < 0);
        }
    }

    puts(failed ? "WIRETEST_FAIL\n" : "WIRETEST_PASS\n");
    return failed;
}
