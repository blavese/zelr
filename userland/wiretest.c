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

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

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

int main(void) {
    char base[URL_TEXT];
    if (getarg(base, sizeof(base)) <= 0 || !base[0]) {
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
    }

    puts(failed ? "WIRETEST_FAIL\n" : "WIRETEST_PASS\n");
    return failed;
}
