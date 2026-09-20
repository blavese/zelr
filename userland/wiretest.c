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

    puts(failed ? "WIRETEST_FAIL\n" : "WIRETEST_PASS\n");
    return failed;
}
