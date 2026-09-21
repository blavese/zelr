#include "fd.h"
#include "diskfs.h"
#include "heap.h"
#include "io.h"
#include "keyboard.h"
#include "pipe.h"
#include "timer.h"
#include "printf.h"
#include "sched.h"
#include "signal.h"
#include "string.h"
#include "vfs.h"

/* See include/fd.h for why there are two layers here rather than one. This
 * file is both of them: the table of open file descriptions, which is the
 * machine's, and the per task arrays of numbers pointing into it, which are
 * each process's own.
 */

/* The two numbers have to agree or a task's array is a different length from
   the one every function here indexes. They are spelled out separately
   because the scheduler holds the array and has no other reason to know what
   a file descriptor is. */
_Static_assert(TASK_MAX_FD == FD_MAX,
               "sched.h's per task array and fd.h's limit have drifted apart");

/* How many files can be open at once across the whole machine. Several
   descriptors can share one of these, so this is a count of opens rather
   than a count of programs times their limit. */
#define OF_MAX 32

typedef enum { OF_FREE = 0, OF_CONSOLE, OF_FILE, OF_PIPE } ofkind_t;

typedef struct {
    ofkind_t kind;

    /* How many descriptors, in any process, point here. fork raises it, and
       close lowers it; the description goes when it reaches zero, which is
       why a child closing its copy does not shut the parent's file. */
    u16 refs;

    /* A file. Read into memory when opened and written back when closed,
       which is the same trade the rest of the filesystem makes. The position
       lives here rather than in the descriptor, so two descriptors sharing
       this description share where they are in the file — that is what makes
       two commands appending to one file take turns instead of overwriting
       each other. */
    char path[VFS_PATH_MAX];
    u8  *data;
    u32  size, cap, pos;
    bool writable, dirty;

    /* A pipe, and which end of it this is. */
    pipe_t *pipe;
    bool    writing;
} ofile_t;

static ofile_t ofiles[OF_MAX];

/* Slot zero is the console and is never freed. Every process is born with
   three descriptors pointing at it, and a program that closes all three has
   not destroyed the machine's only output: it has merely stopped being able
   to reach it, which is the correct meaning of closing a descriptor. */
#define OF_CONSOLE_SLOT 0

static void ensure_console(void) {
    if (ofiles[OF_CONSOLE_SLOT].kind != OF_CONSOLE) {
        memset(&ofiles[OF_CONSOLE_SLOT], 0, sizeof(ofile_t));
        ofiles[OF_CONSOLE_SLOT].kind = OF_CONSOLE;
        ofiles[OF_CONSOLE_SLOT].refs = 1;
    }
}

/* The kernel itself opens files — the self test does, and so does anything
   running before there is a task at all — so there has to be a table for
   when task_current() is nothing. */
static i16  kernel_slots[FD_MAX];
static bool kernel_ready;

static i16 *table(void) {
    task_t *t = task_current();
    if (t) return t->fd;
    if (!kernel_ready) { fd_table_init(kernel_slots); kernel_ready = true; }
    return kernel_slots;
}

void fd_init(void) {
    memset(ofiles, 0, sizeof(ofiles));
    ensure_console();
    fd_table_init(kernel_slots);
    kernel_ready = true;
}

/* --- descriptions -------------------------------------------------------- */

static int of_alloc(ofkind_t kind) {
    ensure_console();
    for (int i = 1; i < OF_MAX; i++) {
        if (ofiles[i].kind == OF_FREE) {
            memset(&ofiles[i], 0, sizeof(ofile_t));
            ofiles[i].kind = kind;
            ofiles[i].refs = 1;
            return i;
        }
    }
    return -1;
}

static bool of_unref(int oi) {
    if (oi <= OF_CONSOLE_SLOT || oi >= OF_MAX) return true;
    ofile_t *f = &ofiles[oi];
    if (f->kind == OF_FREE) return true;
    if (f->refs > 1) { f->refs--; return true; }

    bool ok = true;
    if (f->kind == OF_FILE) {
        /* The last descriptor going is what puts the file back on the disk.
           Nothing before this point has touched it. */
        if (f->dirty)
            ok = vfs_write(f->path, f->data ? f->data : (const u8 *)"", f->size);
        if (f->data) kfree(f->data);
    } else if (f->kind == OF_PIPE) {
        pipe_close(f->pipe, f->writing);
    }
    memset(f, 0, sizeof(ofile_t));
    return ok;
}

/* What this descriptor could do right now, of the things being asked
   about.

   A file is always ready, the way it is everywhere: a read that reaches the
   end returns nothing, and nothing is an answer rather than a wait. The
   console is ready when a key is waiting. A pipe is ready when there are
   bytes in it, or when the other end has gone -- and a read end whose
   writers have all closed reports POLLIN as well as POLLHUP, so a program
   that asked only about reading still wakes up and reads its zero. */
static short fd_ready_now(int fd, short want);

static ofile_t *lookup(int fd) {
    if (fd < 0 || fd >= FD_MAX) return 0;
    i16 oi = table()[fd];
    if (oi < 0 || oi >= OF_MAX) return 0;
    ensure_console();
    ofile_t *f = &ofiles[oi];
    return f->kind == OF_FREE ? 0 : f;
}

static int lowest_free(void) {
    i16 *t = table();
    for (int i = 0; i < FD_MAX; i++)
        if (t[i] < 0) return i;
    return -1;
}

/* --- the tables ---------------------------------------------------------- */

void fd_table_init(i16 *slots) {
    ensure_console();
    slots[FD_STDIN]  = OF_CONSOLE_SLOT;
    slots[FD_STDOUT] = OF_CONSOLE_SLOT;
    slots[FD_STDERR] = OF_CONSOLE_SLOT;
    for (int i = 3; i < FD_MAX; i++) slots[i] = -1;
}

void fd_table_clone(i16 *dst, const i16 *src) {
    ensure_console();
    for (int i = 0; i < FD_MAX; i++) {
        dst[i] = src[i];
        if (src[i] > OF_CONSOLE_SLOT && src[i] < OF_MAX &&
            ofiles[src[i]].kind != OF_FREE)
            ofiles[src[i]].refs++;
    }
}

void fd_table_release(i16 *slots) {
    for (int i = 0; i < FD_MAX; i++) {
        if (slots[i] >= 0) of_unref(slots[i]);
        slots[i] = -1;
    }
}

/* --- the console --------------------------------------------------------- */

/* A line at a time, with the editing done here rather than by every program
 * that wants a line. A program asking for input gets nothing back until a
 * newline arrives, and what it gets has already had the backspaces applied.
 *
 * The alternative is handing over keys as they arrive and making each
 * program implement erase for itself, which is how you end up with a shell
 * where backspace works and a prompt somewhere else where it does not. */
/* Waiting for a key, from inside a system call.
 *
 * A system call arrives through an interrupt gate, and an interrupt gate
 * clears the interrupt flag. So for the whole of a call nothing can arrive:
 * not the timer, not the keyboard, not the serial line. A loop that waits
 * for a key in that state waits with the door shut, and the key it is
 * waiting for is on the other side of it. The shell printed its prompt and
 * then the machine stopped, which is exactly what that looks like.
 *
 * So the door is opened for the length of the wait and shut again
 * afterwards. The rest of the call stays as indivisible as it was; only the
 * waiting is interruptible, which is what waiting ought to mean. It is also
 * what makes task_idle_wait work at all: it halts, and a halt with the flag
 * clear is a halt nothing ends.
 */
static void console_pause(void) {
    bool shut = !interrupts_enabled();
    if (shut) sti();
    task_idle_wait();
    if (shut) cli();
}

static int console_read(void *dst, u32 len) {
    char *out = (char *)dst;
    u32 n = 0;
    if (!len) return 0;

    /* Whoever is reading the console is who an interrupt is about, or whose
       children it is about. Said every time rather than once, because the
       answer changes: a shell reads, then a program it started reads. */
    task_t *me = task_current();
    if (me) signal_console_reader(me->pid);

    for (;;) {
        int ch = kbd_trygetchar();
        if (ch < 0) { console_pause(); continue; }
        if (KEY_IS_SPECIAL(ch)) continue;
        char c = (char)KEY_CODE(ch);

        if (c == '\n') {
            kputc('\n');
            if (n < len) out[n++] = '\n';
            return (int)n;
        }
        if (c == '\b') {
            if (n) { n--; kputc('\b'); }
            continue;
        }
        /* End of input, typed. Without this a program reading until it runs
           out has no way to say it has finished, and a shell has no way to
           be left. */
        if (c == 4) return (int)n;

        /* And the interruption, which by the time it is read here has
           already been raised against whatever it was meant for. What is
           left to do is throw away the half typed line and hand back an
           empty one, so that whoever asked prints a fresh prompt rather
           than acting on what was abandoned. Empty rather than nothing:
           nothing is what end of input means, and the two must not be
           confused or ctrl-C would close the shell. */
        if (c == 3) {
            kputc('^');
            kputc('C');
            kputc('\n');
            if (len) { out[0] = '\n'; return 1; }
            return 0;
        }

        if (c >= ' ' && c < 127 && n < len) {
            out[n++] = c;
            kputc(c);
            if (n == len) return (int)n;
        }
    }
}

static int console_write(const void *src, u32 len) {
    const char *p = (const char *)src;
    for (u32 i = 0; i < len; i++) kputc(p[i]);
    return (int)len;
}

/* --- files --------------------------------------------------------------- */

static bool grow_to(ofile_t *f, u32 need) {
    if (need <= f->cap) return true;
    u32 cap = f->cap ? f->cap : 256;
    while (cap < need) cap *= 2;
    u8 *bigger = (u8 *)kmalloc(cap);
    if (!bigger) return false;
    if (f->size) memcpy(bigger, f->data, f->size);
    memset(bigger + f->size, 0, cap - f->size);
    if (f->data) kfree(f->data);
    f->data = bigger;
    f->cap = cap;
    return true;
}

int fd_open(const char *path, u32 flags) {
    char abs[VFS_PATH_MAX];
    if (!vfs_resolve(path, abs, sizeof(abs))) return -1;

    u32 size = 0;
    bool is_dir = false;
    bool exists = vfs_stat(abs, &size, &is_dir);
    if (is_dir) return -1;
    if (!exists && !(flags & O_CREATE)) return -1;
    if ((flags & O_WRITE) && vfs_generated(abs)) return -1;

    /* The number first, because running out of numbers is the cheap failure
       and there is no point reading a file in to then find there is nowhere
       to put it. */
    int fd = lowest_free();
    if (fd < 0) return -1;

    int oi = of_alloc(OF_FILE);
    if (oi < 0) return -1;

    ofile_t *f = &ofiles[oi];
    strncpy(f->path, abs, VFS_PATH_MAX - 1);
    f->writable = (flags & (O_WRITE | O_CREATE | O_TRUNC | O_APPEND)) != 0;

    if (exists && !(flags & O_TRUNC)) {
        if (!grow_to(f, size ? size : 1)) { of_unref(oi); return -1; }
        int got = vfs_read(abs, f->data, size);
        if (got < 0) { of_unref(oi); return -1; }
        f->size = (u32)got;
    } else {
        f->dirty = true;              /* an empty file that must be created */
    }

    f->pos = (flags & O_APPEND) ? f->size : 0;
    table()[fd] = (i16)oi;
    return fd;
}

int fd_read(int fd, void *buf, u32 len) {
    ofile_t *f = lookup(fd);
    if (!f || !buf) return -1;

    switch (f->kind) {
    case OF_CONSOLE:
        return console_read(buf, len);
    case OF_PIPE:
        if (f->writing) return -1;       /* the wrong end of it */
        return pipe_read(f->pipe, buf, len);
    case OF_FILE: {
        if (f->pos >= f->size) return 0;
        u32 n = f->size - f->pos;
        if (n > len) n = len;
        memcpy(buf, f->data + f->pos, n);
        f->pos += n;
        return (int)n;
    }
    default:
        return -1;
    }
}

int fd_write(int fd, const void *buf, u32 len) {
    ofile_t *f = lookup(fd);
    if (!f || !buf) return -1;

    switch (f->kind) {
    case OF_CONSOLE:
        return console_write(buf, len);
    case OF_PIPE:
        if (!f->writing) return -1;
        return pipe_write(f->pipe, buf, len);
    case OF_FILE:
        if (!f->writable) return -1;
        if (!grow_to(f, f->pos + len)) return -1;
        memcpy(f->data + f->pos, buf, len);
        f->pos += len;
        if (f->pos > f->size) f->size = f->pos;
        f->dirty = true;
        return (int)len;
    default:
        return -1;
    }
}

int fd_seek(int fd, i32 offset, u32 whence) {
    ofile_t *f = lookup(fd);
    /* A pipe and a console have no position to move to, and pretending
       otherwise would make a program that seeks look like it worked. */
    if (!f || f->kind != OF_FILE) return -1;

    i32 base = whence == 1 ? (i32)f->pos : whence == 2 ? (i32)f->size : 0;
    i32 want = base + offset;
    if (want < 0) return -1;
    f->pos = (u32)want;
    return want;
}

int fd_size(int fd) {
    ofile_t *f = lookup(fd);
    if (!f) return -1;
    if (f->kind == OF_PIPE) return (int)pipe_count(f->pipe);
    if (f->kind != OF_FILE) return -1;
    return (int)f->size;
}

bool fd_close(int fd) {
    if (fd < 0 || fd >= FD_MAX) return false;
    i16 *t = table();
    if (t[fd] < 0) return false;
    int oi = t[fd];
    t[fd] = -1;
    return of_unref(oi);
}

/* What has been written, put where the power going will not take it.
 *
 * A file lives in memory until its last descriptor closes, which is fine
 * for a program that writes something and stops and is no use at all to one
 * that keeps a file open -- a log, a database, anything long running. For
 * those, everything written since the file was opened is in memory and on
 * the disk there is nothing.
 *
 * vfs_write goes through fat_write_file, which writes the new copy into
 * clusters nothing points at, flushes, swings the directory entry in one
 * sector write and flushes again. So this returning true means the bytes
 * are on the drive and not in its cache.
 *
 * A pipe and the console have no disk behind them and are quietly fine.
 */
static short fd_ready_now(int fd, short want) {
    ofile_t *f = lookup(fd);
    if (!f) return POLLNVAL;

    short got = 0;
    if (f->kind == OF_CONSOLE) {
        if (kbd_has_char()) got |= POLLIN;
        got |= POLLOUT;                     /* the screen is always willing */
    } else if (f->kind == OF_FILE) {
        got |= POLLIN | POLLOUT;
    } else if (f->kind == OF_PIPE && f->pipe) {
        if (f->writing) {
            if (!f->pipe->readers) got |= POLLERR;
            else if (f->pipe->count < PIPE_SIZE) got |= POLLOUT;
        } else {
            if (f->pipe->count) got |= POLLIN;
            else if (!f->pipe->writers) got |= POLLIN | POLLHUP;
        }
    } else {
        return POLLNVAL;
    }

    /* What was asked about, plus the three that are reported whether they
       were asked about or not. A program cannot decline to be told that the
       other end has gone. */
    return (short)(got & (want | POLLERR | POLLHUP | POLLNVAL));
}

int fd_poll(pollfd_t *fds, u32 n, int timeout_ms) {
    if (n > POLL_MAX) return -1;

    /* Looked at again rather than woken.
     *
       Every one of these has a wait queue behind it already -- a pipe has
       two -- and waiting on several at once means being on several queues
       and being taken off all of them when any one fires, which this
       kernel's wait has no way to express. So this looks, and if nothing
       has happened it sleeps a tick and looks again.

       The cost is latency: up to one tick, ten milliseconds, later than a
       queue would have been. The cost of the other one is a rewrite of
       waiting, and this is honest about which it is rather than pretending
       a tick is instant. */
    u64 hz = timer_hz();
    u64 deadline = 0;
    if (timeout_ms > 0)
        deadline = timer_ticks() + ((u64)timeout_ms * hz + 999u) / 1000u + 1;

    for (;;) {
        int ready = 0;
        for (u32 i = 0; i < n; i++) {
            fds[i].revents = fd_ready_now(fds[i].fd, fds[i].events);
            if (fds[i].revents) ready++;
        }
        if (ready) return ready;
        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0 && timer_ticks() >= deadline) return 0;
        task_sleep(1000 / (u32)hz ? 1000 / (u32)hz : 1);
    }
}

bool fd_sync(int fd) {
    ofile_t *f = lookup(fd);
    if (!f) return false;
    if (f->kind != OF_FILE) return true;

    if (!f->dirty) return diskfs_flush();   /* nothing of ours; still ask the drive */

    if (!vfs_write(f->path, f->data ? f->data : (const u8 *)"", f->size))
        return false;
    f->dirty = false;
    return true;
}

/* --- duplicating --------------------------------------------------------- */

int fd_dup(int fd) {
    ofile_t *f = lookup(fd);
    if (!f) return -1;
    int to = lowest_free();
    if (to < 0) return -1;
    i16 *t = table();
    t[to] = t[fd];
    if (t[fd] > OF_CONSOLE_SLOT) ofiles[t[fd]].refs++;
    return to;
}

int fd_dup2(int fd, int to) {
    if (to < 0 || to >= FD_MAX) return -1;
    ofile_t *f = lookup(fd);
    if (!f) return -1;
    if (fd == to) return to;

    i16 *t = table();
    if (t[to] >= 0) of_unref(t[to]);
    t[to] = t[fd];
    if (t[fd] > OF_CONSOLE_SLOT) ofiles[t[fd]].refs++;
    return to;
}

/* --- pipes --------------------------------------------------------------- */

bool fd_pipe(int ends[2]) {
    if (!ends) return false;

    /* Both numbers before either description, so a failure half way through
       is a failure that changed nothing. */
    i16 *t = table();
    int r = lowest_free();
    if (r < 0) return false;
    t[r] = OF_CONSOLE_SLOT;                  /* held, so the next call skips it */
    int w = lowest_free();
    if (w < 0) { t[r] = -1; return false; }

    pipe_t *p = pipe_new();
    if (!p) { t[r] = -1; return false; }

    int ri = of_alloc(OF_PIPE);
    int wi = ri < 0 ? -1 : of_alloc(OF_PIPE);
    if (wi < 0) {
        if (ri >= 0) { ofiles[ri].kind = OF_FREE; }
        t[r] = -1;
        pipe_close(p, false);
        pipe_close(p, true);
        return false;
    }

    ofiles[ri].pipe = p;
    ofiles[ri].writing = false;
    ofiles[wi].pipe = p;
    ofiles[wi].writing = true;

    t[r] = (i16)ri;
    t[w] = (i16)wi;
    ends[0] = r;
    ends[1] = w;
    return true;
}

u32 fd_live(void) {
    u32 n = 0;
    for (int i = 1; i < OF_MAX; i++)
        if (ofiles[i].kind != OF_FREE) n++;
    return n;
}
