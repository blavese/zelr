/* sh — a shell, in ring 3.
 *
 * There has been a shell in this system for a long time, and it lives in the
 * kernel. It works by calling kernel functions directly, which means every
 * command it has is a function somebody added to a switch inside the kernel,
 * and it can only ever do things the kernel already knows how to do.
 *
 * This one is a program. It knows three system calls and nothing else:
 *
 *   fork  makes a copy of itself
 *   dup2  points a copy's descriptors somewhere before it becomes anything
 *   exec  turns the copy into the program that was typed
 *
 * Everything below is those three in different arrangements. Redirection is
 * a dup2 between the fork and the exec. A pipeline is a pipe, two forks, and
 * a dup2 on each side. Running in the background is not waiting. There is no
 * fourth mechanism, and nothing here needs the kernel's permission for any
 * particular command: a program that did not exist when this was written
 * runs exactly as well as one that did.
 *
 * What it does not do, and says so rather than half doing: no variables, no
 * globbing, no job control, no quoting beyond double quotes, no `&&`. It
 * also passes one argument string to a program rather than a vector, because
 * that is what this kernel's exec carries; the program splits it again with
 * args.h.
 */
#include "zelr.h"
#include "alloc.h"
#include "args.h"

#define LINE_MAX    512
#define WORDS_MAX    32
#define STAGES_MAX    4

/* One command in a pipeline: the words of it, and where its input and
   output were told to go. */
typedef struct {
    char *argv[WORDS_MAX];
    int   argc;
    const char *in;          /* a path from <, or nothing */
    const char *out;         /* a path from > or >>, or nothing */
    int   append;            /* it was >> rather than > */
} stage_t;

static char line[LINE_MAX];

/* --- small things -------------------------------------------------------- */

static void say(const char *s) { puts(s); }

static void say_err(const char *a, const char *b) {
    /* Errors go to descriptor 2, which is what 2 is for: a shell whose
       complaints went down the pipe would feed them to the next program. */
    char msg[256];
    int n = 0;
    for (const char *s = "sh: "; *s; s++) msg[n++] = *s;
    for (const char *s = a; *s && n < 200; s++) msg[n++] = *s;
    if (b) {
        for (const char *s = ": "; *s; s++) msg[n++] = *s;
        for (const char *s = b; *s && n < 250; s++) msg[n++] = *s;
    }
    msg[n++] = '\n';
    fwrite(2, msg, n);
}

static int has_slash(const char *s) {
    for (; *s; s++) if (*s == '/') return 1;
    return 0;
}

/* --- parsing ------------------------------------------------------------- */

/* Pulls the redirections out of a stage's words, leaving the rest. A
   redirection is two words — the arrow and the path — and both come out. */
static int take_redirections(stage_t *st) {
    int keep = 0;
    for (int i = 0; i < st->argc; i++) {
        char *w = st->argv[i];
        int is_in  = w[0] == '<' && w[1] == 0;
        int is_out = w[0] == '>' && w[1] == 0;
        int is_app = w[0] == '>' && w[1] == '>' && w[2] == 0;

        if (is_in || is_out || is_app) {
            if (i + 1 >= st->argc) {
                say_err("a redirection needs somewhere to go", 0);
                return 0;
            }
            if (is_in) st->in = st->argv[i + 1];
            else { st->out = st->argv[i + 1]; st->append = is_app; }
            i++;                                  /* and its path */
            continue;
        }
        st->argv[keep++] = w;
    }
    st->argc = keep;
    st->argv[keep] = 0;
    return 1;
}

/* Cuts the line at every | and turns each piece into a stage. */
static int parse(char *text, stage_t *stages, int *count, int *background) {
    *count = 0;
    *background = 0;

    /* The & goes first, because it belongs to the whole line rather than to
       the last command in it. */
    int len = strlen(text);
    while (len && (text[len - 1] == ' ' || text[len - 1] == '\n')) text[--len] = 0;
    if (len && text[len - 1] == '&') { *background = 1; text[--len] = 0; }

    char *piece = text;
    for (;;) {
        char *bar = piece;
        while (*bar && *bar != '|') bar++;
        int more = *bar == '|';
        if (more) *bar = 0;

        if (*count >= STAGES_MAX) {
            say_err("that is more stages than this shell has", 0);
            return 0;
        }
        stage_t *st = &stages[*count];
        st->in = st->out = 0;
        st->append = 0;
        st->argc = args_split(piece, st->argv, WORDS_MAX - 1);
        st->argv[st->argc] = 0;
        if (!take_redirections(st)) return 0;

        if (st->argc == 0) {
            /* An empty stage is `ls |` or `| wc`, which is a typing mistake
               rather than a command, and running half of it would be worse
               than saying so. */
            if (*count > 0 || more) { say_err("there is nothing on one side of the |", 0); return 0; }
        } else {
            (*count)++;
        }

        if (!more) break;
        piece = bar + 1;
    }
    return 1;
}

/* --- built in commands ---------------------------------------------------
 *
 * These have to be the shell itself rather than programs, because each of
 * them changes the shell. A `cd` that ran as a child would change the
 * child's directory and then the child would exit. */

static int builtin(stage_t *st) {
    const char *c = st->argv[0];

    if (!strcmp(c, "exit")) exit(0);

    if (!strcmp(c, "cd")) {
        const char *where = st->argc > 1 ? st->argv[1] : "/";
        if (chdir(where) != 0) say_err("cannot go to", where);
        return 1;
    }

    if (!strcmp(c, "pwd")) {
        char cwd[128];
        if (getcwd(cwd, sizeof(cwd)) >= 0) { say(cwd); say("\n"); }
        return 1;
    }

    if (!strcmp(c, "help")) {
        say("this shell runs programs from /bin, and arranges them:\n"
            "  cmd > file      send its output to a file\n"
            "  cmd >> file     add to the end of one instead\n"
            "  cmd < file      take its input from a file\n"
            "  a | b           b reads what a wrote\n"
            "  cmd &           do not wait for it\n"
            "built in, because each of them changes this shell:\n"
            "  cd pwd exit help jobs\n"
            "there are no variables, no globbing and no &&.\n");
        return 1;
    }

    if (!strcmp(c, "jobs")) {
        /* Nothing keeps a list yet. Saying so is better than printing an
           empty one and letting it look like there is nothing running. */
        say("this shell does not keep track of background work yet\n");
        return 1;
    }
    return 0;
}

/* --- running ------------------------------------------------------------- */

/* Becomes the program this stage names, on the words it was given.

   take_redirections has already put a null after the last one, which is
   what execv counts up to, so the line as typed is the vector and there
   is nothing to rebuild. It used to be glued back into one string here
   and taken apart again by the program, which meant `grep "two words"`
   arrived as two. */
static void become(stage_t *st) {
    char path[160];
    if (has_slash(st->argv[0])) {
        strncpy(path, st->argv[0], sizeof(path) - 1);
    } else {
        strcpy(path, "/bin/");
        strncpy(path + 5, st->argv[0], sizeof(path) - 6);
    }

    execv(path, st->argv);
    /* Only here if it would not run. */
    say_err("not found", st->argv[0]);
    exit(127);
}

static int open_for(stage_t *st, int which) {
    if (which == 0) return open(st->in, O_READ);
    return open(st->out, st->append ? (O_WRITE | O_CREATE | O_APPEND)
                                    : (O_WRITE | O_CREATE | O_TRUNC));
}

static void run(stage_t *stages, int count, int background) {
    int pids[STAGES_MAX];
    int made = 0;
    int carried = -1;               /* the reading end left by the last stage */

    for (int i = 0; i < STAGES_MAX; i++) pids[i] = 0;

    for (int i = 0; i < count; i++) {
        stage_t *st = &stages[i];
        int ends[2] = { -1, -1 };
        int last = i == count - 1;

        if (!last && pipe(ends) != 0) {
            say_err("no pipe to be had", 0);
            break;
        }

        int pid = fork();
        if (pid < 0) { say_err("cannot make another process", 0); break; }

        if (pid == 0) {
            /* The child, arranging itself before it becomes anything.
               Everything here is undone by the exec except the descriptors,
               which is exactly why it is done here.

               And the interrupt, which is not undone by it. This shell
               ignores SIGINT so that ctrl-C reaches the program rather than
               ending the session; a child inherits that, and exec keeps an
               ignored signal ignored, which is the rule everywhere and is
               there so a parent can start something deliberately immune.
               What it means here is that the program would be immune too,
               and the one thing a shell must be able to do is stop the
               thing it started. So it is put back before the exec.

               Measured, by leaving it out: `spin` could not be interrupted
               and the shell sat waiting for it, which is the machine lost. */
            signal(SIGINT, SIG_DFL);

            if (carried >= 0) { dup2(carried, 0); close(carried); }
            if (!last) {
                close(ends[0]);
                dup2(ends[1], 1);
                close(ends[1]);
            }
            /* A file named on the line beats the pipe, because that is what
               the person typing it said. */
            if (st->in) {
                int fd = open_for(st, 0);
                if (fd < 0) { say_err("cannot read", st->in); exit(1); }
                dup2(fd, 0);
                close(fd);
            }
            if (st->out) {
                int fd = open_for(st, 1);
                if (fd < 0) { say_err("cannot write", st->out); exit(1); }
                dup2(fd, 1);
                close(fd);
            }
            become(st);
        }

        /* The shell holds no end of any pipe once the children have them:
           a reader waits for the last writer to go, and a shell that kept
           its copy open would be that writer forever.

           Cleared as well as closed, so the tidying up after the loop does
           not close the same number twice — and cannot close a number that
           something else has been handed in the meantime. */
        if (carried >= 0) { close(carried); carried = -1; }
        if (!last) { close(ends[1]); carried = ends[0]; }
        pids[made++] = pid;
    }

    if (carried >= 0) close(carried);
    if (!made) return;

    if (background) {
        say("[");
        putn(pids[made - 1]);
        say("]\n");
        return;
    }
    for (int i = 0; i < made; i++)
        if (pids[i] > 0) wait_for(pids[i]);
}

/* --- the loop ------------------------------------------------------------ */

static void prompt(void) {
    char cwd[128];
    if (getcwd(cwd, sizeof(cwd)) >= 0) say(cwd);
    say(" $ ");
}

int main(void) {
    /* The interrupt belongs to whatever this shell started, not to the
       shell that is waiting for it. Without this, stopping a program would
       close the session it was started from. */
    signal(SIGINT, SIG_IGN);

    say("zelr shell. type help for what it can do, exit to leave.\n");

    stage_t stages[STAGES_MAX];
    for (;;) {
        prompt();

        int n = fread(0, line, LINE_MAX - 1);
        if (n <= 0) { say("\n"); return 0; }      /* end of input */
        line[n] = 0;

        int count = 0, background = 0;
        if (!parse(line, stages, &count, &background)) continue;
        if (count == 0) continue;

        /* A built in only makes sense on its own: `cd x | wc` would run the
           cd in a child and change nothing. */
        if (count == 1 && !stages[0].out && !stages[0].in && builtin(&stages[0]))
            continue;

        run(stages, count, background);
    }
}
