/* The entire user-facing interface: thirty-six system calls and a little
   sugar. There is no libc here, and nothing is linked in from the kernel;
   every call below crosses the ring boundary through int 0x80. */
#pragma once

typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef int                i32;
typedef unsigned long long u64;

/* _Bool is a keyword; the spellings are the header, which there is not one
   of here. */
typedef _Bool bool;
#define true  1
#define false 0

/* Wide enough to hold a pointer, which on this machine an int is not. Every
   argument that crosses into the kernel goes through one of these. */
typedef long long          nyx_word;

#define SYS_EXIT       0
#define SYS_PUTC       1
#define SYS_WRITE      2
#define SYS_GETPID     3
#define SYS_TICKS      4
#define SYS_SLEEP      5
#define SYS_READ_FILE  6

#define SYS_WIN_CREATE   7
#define SYS_WIN_SURFACE  8
#define SYS_WIN_SIZE     9
#define SYS_WIN_POLL    10
#define SYS_WIN_COMMIT  11
#define SYS_WIN_CLOSE   12

#define SYS_OPEN        13
#define SYS_CLOSE       14
#define SYS_FREAD       15
#define SYS_FWRITE      16
#define SYS_SEEK        17
#define SYS_UNLINK      18
#define SYS_MKDIR       19
#define SYS_RMDIR       20
#define SYS_READDIR     21
#define SYS_STAT        22
#define SYS_CHDIR       23
#define SYS_GETCWD      24

#define SYS_CONNECT     25
#define SYS_SEND        26
#define SYS_RECV        27
#define SYS_DISCONNECT  28
#define SYS_RESOLVE     29
#define SYS_NETINFO     30
#define SYS_SYSINFO     31
#define SYS_SPAWN       32
#define SYS_WAIT        33
#define SYS_KILL        34
#define SYS_TASKS       35
#define SYS_WIN_RESIZABLE 36
#define SYS_WIN_RESIZE    37
#define SYS_CLIP_SET      38
#define SYS_CLIP_GET      39

/* The one door into the kernel. The registers are the same ones a 32-bit nyx
   used, only twice as wide, which is why every argument is a word rather than
   an int: an int would quietly cut the top half off a pointer. */
static inline nyx_word syscall(nyx_word n, nyx_word a, nyx_word b, nyx_word c) {
    nyx_word r;
    __asm__ volatile ("int $0x80"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}

static inline void exit(int code)        { syscall(SYS_EXIT, code, 0, 0); }

static inline void putc(char ch)         { syscall(SYS_PUTC, ch, 0, 0); }
static inline int  getpid(void)          { return syscall(SYS_GETPID, 0, 0, 0); }
static inline int  ticks(void)           { return syscall(SYS_TICKS, 0, 0, 0); }
static inline void sleep_ms(int ms)      { syscall(SYS_SLEEP, ms, 0, 0); }

static inline int write(const char *s, int len) {
    return syscall(SYS_WRITE, 0, (nyx_word)s, len);
}

static inline int read_file(const char *name, char *buf, int cap) {
    return syscall(SYS_READ_FILE, (nyx_word)name, (nyx_word)buf, cap);
}

/* --- strings -------------------------------------------------------------

   Enough of a string library to write a program with. There is no libc to
   link against, so this is all of it. */

static inline int strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static inline int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static inline int strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static inline void strcpy(char *d, const char *s) {
    while ((*d++ = *s++)) { }
}

static inline void strncpy(char *d, const char *s, int n) {
    int i = 0;
    for (; i < n - 1 && s[i]; i++) d[i] = s[i];
    if (n > 0) d[i] = 0;
}

static inline void *memset(void *p, int v, int n) {
    unsigned char *b = (unsigned char *)p;
    for (int i = 0; i < n; i++) b[i] = (unsigned char)v;
    return p;
}

static inline void *memcpy(void *d, const void *s, int n) {
    unsigned char *a = (unsigned char *)d;
    const unsigned char *b = (const unsigned char *)s;
    for (int i = 0; i < n; i++) a[i] = b[i];
    return d;
}

static inline void puts(const char *s) { write(s, strlen(s)); }

static inline void putn(int v) {
    char buf[12];
    int n = 0;
    if (v < 0) { putc('-'); v = -v; }
    if (v == 0) { putc('0'); return; }
    while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) putc(buf[n]);
}

/* Formats an unsigned value into `out`, returning its length. */
static inline int utoa(u32 v, char *out) {
    char tmp[12];
    int n = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return 1; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

/* --- files ---------------------------------------------------------------

   Paths may be relative; the kernel joins them to this program's working
   directory, which it inherited from whoever started it. */

#define O_READ   0x01
#define O_WRITE  0x02
#define O_CREATE 0x04
#define O_TRUNC  0x08
#define O_APPEND 0x10

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct {
    u32  size;
    u32  is_dir;
    char name[32];
} nyx_stat;

static inline int open(const char *path, u32 flags) {
    return syscall(SYS_OPEN, (nyx_word)path, (nyx_word)flags, 0);
}
static inline int close(int fd)  { return syscall(SYS_CLOSE, fd, 0, 0); }

static inline int fread(int fd, void *buf, int len) {
    return syscall(SYS_FREAD, fd, (nyx_word)buf, len);
}
static inline int fwrite(int fd, const void *buf, int len) {
    return syscall(SYS_FWRITE, fd, (nyx_word)buf, len);
}
static inline int seek(int fd, int off, int whence) {
    return syscall(SYS_SEEK, fd, off, whence);
}

static inline int unlink(const char *path) { return syscall(SYS_UNLINK, (nyx_word)path, 0, 0); }
static inline int mkdir(const char *path)  { return syscall(SYS_MKDIR, (nyx_word)path, 0, 0); }
static inline int rmdir(const char *path)  { return syscall(SYS_RMDIR, (nyx_word)path, 0, 0); }

/* Returns 1 when an entry was produced, 0 past the end, -1 on error. */
static inline int readdir(const char *path, int index, nyx_stat *out) {
    return syscall(SYS_READDIR, (nyx_word)path, index, (nyx_word)out);
}
static inline int stat(const char *path, nyx_stat *out) {
    return syscall(SYS_STAT, (nyx_word)path, (nyx_word)out, 0);
}
static inline int chdir(const char *path) { return syscall(SYS_CHDIR, (nyx_word)path, 0, 0); }
static inline int getcwd(char *buf, int cap) {
    return syscall(SYS_GETCWD, (nyx_word)buf, cap, 0);
}

/* Reads a whole file into a caller-supplied buffer. Returns the length. */
static inline int slurp(const char *path, char *buf, int cap) {
    int fd = open(path, O_READ);
    if (fd < 0) return -1;
    int total = 0;
    for (;;) {
        int n = fread(fd, buf + total, cap - total);
        if (n <= 0) break;
        total += n;
        if (total >= cap) break;
    }
    close(fd);
    return total;
}

static inline int spit(const char *path, const void *buf, int len) {
    int fd = open(path, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) return -1;
    int n = fwrite(fd, buf, len);
    close(fd);
    return n;
}

/* --- the network ---------------------------------------------------------

   One connection at a time, because that is what the kernel's TCP supports.
   `host` may be a name or a dotted address. */

typedef struct {
    u32 up;
    u32 ip, gateway, netmask, dns;
    u8  mac[6];
    u16 pad;
} nyx_netinfo;

static inline int connect(const char *host, int port) {
    return syscall(SYS_CONNECT, (nyx_word)host, port, 0);
}
static inline int send(const void *buf, int len) {
    return syscall(SYS_SEND, 0, (nyx_word)buf, len);
}
static inline int recv(void *buf, int len) {
    return syscall(SYS_RECV, 0, (nyx_word)buf, len);
}
static inline int disconnect(void) { return syscall(SYS_DISCONNECT, 0, 0, 0); }

static inline int resolve(const char *host, u32 *out) {
    return syscall(SYS_RESOLVE, (nyx_word)host, (nyx_word)out, 0);
}
static inline int netinfo(nyx_netinfo *out) {
    return syscall(SYS_NETINFO, (nyx_word)out, 0, 0);
}

/* What the machine is, as far as a program is allowed to know. */
typedef struct {
    u32 cpus_found, cpus_started;
    u32 mem_total_kb, mem_used_kb;
    u32 heap_total_kb, mem_free_kb;
    u32 uptime_seconds;
    u32 tasks;
    u32 screen_w, screen_h;
    u32 syscalls;
    u32 disk_kb_free;
} nyx_sysinfo;

static inline int sysinfo(nyx_sysinfo *out) {
    return syscall(SYS_SYSINFO, (nyx_word)out, 0, 0);
}

/* --- other programs ------------------------------------------------------

   spawn starts one and returns its pid; wait blocks until it finishes and
   gives back what it returned from main. A program that is never waited for
   still runs and still exits; its status is simply not collected. */

#define TASK_READY    0
#define TASK_RUNNING  1
#define TASK_SLEEPING 2
#define TASK_BLOCKED  3
#define TASK_DEAD     4

typedef struct {
    u32  pid;
    u32  state;
    u32  slices;
    u32  user;
    char name[32];
} nyx_task;

static inline int spawn(const char *path) {
    return syscall(SYS_SPAWN, (nyx_word)path, 0, 0);
}
static inline int wait_for(int pid) {
    return syscall(SYS_WAIT, pid, 0, 0);
}
static inline int kill(int pid) {
    return syscall(SYS_KILL, pid, 0, 0);
}
/* Returns 1 when an entry was produced, 0 past the end. */
static inline int tasks(int index, nyx_task *out) {
    return syscall(SYS_TASKS, index, (nyx_word)out, 0);
}

/* Starts a program and waits for it, which is what a shell wants. */
static inline int run_program(const char *path) {
    int pid = spawn(path);
    if (pid < 0) return pid;
    return wait_for(pid);
}

/* --- the clipboard --------------------------------------------------------

   One buffer shared by every program, text only. The length is given rather
   than inferred so a selection out of a grid, which is not terminated, can be
   copied as it stands. */

static inline int clip_set(const char *text, int len) {
    return syscall(SYS_CLIP_SET, (nyx_word)text, len, 0);
}

/* With cap 0 this answers how many bytes are waiting, so a caller can size a
   buffer before asking for the contents. */
static inline int clip_get(char *out, int cap) {
    return syscall(SYS_CLIP_GET, (nyx_word)out, cap, 0);
}

static inline int clip_len(void) { return syscall(SYS_CLIP_GET, 0, 0, 0); }

/* --- windows -------------------------------------------------------------

   A window is a handle and a block of pixels the kernel maps into this
   program's address space. Draw into the pixels, call win_commit, and the
   window manager puts them on the screen. */

#define WIN_EV_NONE  0
#define WIN_EV_MOUSE 1
#define WIN_EV_KEY   2
#define WIN_EV_CLOSE 3
/* The window is a different size. x and y carry the new content size, and
   whatever was drawn is gone: ask for the surface again and redraw. */
#define WIN_EV_RESIZE 4

/* Bit 7 of buttons is set on the event that started a press, so a program
   can tell a new stroke from the middle of one. */
#define WIN_BTN_LEFT  0x01
#define WIN_BTN_RIGHT 0x02
#define WIN_BTN_DOWN  0x80

typedef struct {
    u32 type;
    i32 x, y;
    u32 buttons;
    u32 key;
} win_event;

/* Keys that are not characters arrive in the same field, above the range a
   character can occupy, so one value carries either. */
#define KEY_UP        0x100
#define KEY_DOWN      0x101
#define KEY_LEFT      0x102
#define KEY_RIGHT     0x103
#define KEY_HOME      0x104
#define KEY_END       0x105
#define KEY_PAGE_UP   0x106
#define KEY_PAGE_DOWN 0x107
#define KEY_DELETE    0x108
#define KEY_INSERT    0x109
#define KEY_F1        0x110      /* F1..F12 run consecutively */

/* The control key arrives as a bit alongside the character, so ctrl+c and a
   plain c are one field apart. Shift is not here: it is already folded into
   the character, and alt belongs to the desktop. */
#define KEY_MOD_CTRL  0x20000
#define KEY_CODE(k)   ((k) & 0xFFFF)
#define KEY_CTRL(k)   (((k) & KEY_MOD_CTRL) != 0)

#define KEY_IS_SPECIAL(k) (KEY_CODE(k) >= 0x100)

/* Which letter a control chord was.
 *
 * The keyboard driver folds ctrl+letter into the control character it has
 * meant since teletypes: ctrl+a is 1, ctrl+c is 3, ctrl+v is 22. The
 * modifier bit rides alongside, so the chord is recognisable, but comparing
 * the code against 'c' finds nothing. This turns it back into the letter. */
static inline int key_ctrl_letter(u32 k) {
    u32 c = KEY_CODE(k);
    if (c >= 1 && c <= 26) return 'a' + (int)c - 1;
    if (c >= 'a' && c <= 'z') return (int)c;
    if (c >= 'A' && c <= 'Z') return (int)c - 'A' + 'a';
    return 0;
}

static inline int win_create(const char *title, int w, int h) {
    return syscall(SYS_WIN_CREATE, (nyx_word)title, w, h);
}

/* The address of this window's pixels, row major, one u32 per pixel. */
static inline u32 *win_surface(int handle) {
    return (u32 *)syscall(SYS_WIN_SURFACE, handle, 0, 0);
}

static inline int win_width(int handle) {
    int v = syscall(SYS_WIN_SIZE, handle, 0, 0);
    return v < 0 ? -1 : ((v >> 16) & 0xFFFF);
}

static inline int win_height(int handle) {
    int v = syscall(SYS_WIN_SIZE, handle, 0, 0);
    return v < 0 ? -1 : (v & 0xFFFF);
}

static inline int win_poll(int handle, win_event *ev) {
    return syscall(SYS_WIN_POLL, handle, (nyx_word)ev, 0);
}

static inline int win_commit(int handle) {
    return syscall(SYS_WIN_COMMIT, handle, 0, 0);
}

static inline int win_close(int handle) {
    return syscall(SYS_WIN_CLOSE, handle, 0, 0);
}

/* Says this program can cope with its window changing size. Until it does,
   the desktop leaves the window alone: the edges do not drag, and maximise
   and snapping are refused. After it, a WIN_EV_RESIZE says the surface has
   been replaced, and everything drawn into the old one is gone.

   The address from win_surface stays the same across a resize, so a program
   that keeps the pointer does not have to ask again, only redraw. */
static inline int win_allow_resize(int handle) {
    return syscall(SYS_WIN_RESIZABLE, handle, 0, 0);
}

static inline int win_resize(int handle, int w, int h) {
    return syscall(SYS_WIN_RESIZE, handle, w, h);
}

#define RGB(r, g, b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))
