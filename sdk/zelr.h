/* zelr — the interface between a program and the kernel.
 *
 * Copyright (C) 2026 blavese
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is the one most likely to be copied on its own, because it is
 * the whole of what a program needs. It carries its own notice for that
 * reason. See NOTICE in the repository this came from.
 */

/* The entire user-facing interface: forty-seven system calls and a little
   sugar. Nothing is linked in from the kernel; every call below crosses the
   ring boundary through int 0x80.
 *
 * Two things here can be turned off, and sdk/libc turns both off:
 *
 *   ZELR_NO_SUGAR   leaves out strlen, memcpy, puts, exit and the rest of
 *                   the convenience names. They are spelled the way this
 *                   project spells them -- `int strlen`, `void strcpy` --
 *                   rather than the way C standardised them, which is fine
 *                   for a program with no libc and a collision for a
 *                   program with one. The system calls are the same either
 *                   way.
 *
 *   ZELR_NO_START   leaves out _start. A program built from one file wants
 *                   the entry point in the header; a program built from
 *                   several needs exactly one of them to have it, and the
 *                   libc's crt0.c is that one.
 *
 * A program that includes nothing but this header and sets neither is what
 * this header has always been, and still works unchanged. */
#pragma once

typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef int                i32;
typedef unsigned long long u64;
typedef long long          i64;

/* _Bool is a keyword; the spellings are the header, which there is not one
   of here. */
typedef _Bool bool;
#define true  1
#define false 0

/* Wide enough to hold a pointer, which on this machine an int is not. Every
   argument that crosses into the kernel goes through one of these. */
typedef long long          zelr_word;

/* A system call number as a string, for the two places that need one
   inside an asm block: the startup code and the signal trampoline.
   Written out rather than repeated as a literal, because a number in
   two places is a number that comes to disagree. */
#define ZELR_STR2(x) #x
#define ZELR_STR(x) ZELR_STR2(x)

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

/* Sound, and the power switch. */
#define SYS_SOUND_INFO    40
#define SYS_SOUND_WRITE   41
#define SYS_POWER         42
#define POWER_OFF     0
#define POWER_REBOOT  1
/* A path, a vector of words and how many there are. 44 was SYS_GETARG and
   is retired: a program reads its words off its own stack now. */
#define SYS_SPAWN_ARGV    43

/* What the few instructions a handler returns into ask for. Never called
   directly by a program. */
#define SYS_SIGRETURN     58

/* Memory a program asks for and is given as it touches it. Anonymous
   only: zeroed pages, no file mapping. */
#define SYS_MMAP          59
#define SYS_MUNMAP        60

/* What has been written, put where the power going will not take it. */
#define SYS_FSYNC         61

/* A different name for a file, within one directory. */
#define SYS_RENAME        62

/* Waiting on several descriptors at once. */
#define SYS_POLL          63

/* The same socket, encrypted. See connect_tls below. */
#define SYS_TLS_CONNECT   45
#define SYS_TLS_STATUS    46

/* Moves this program's heap break and returns where it was.
 *
   The one call that makes an allocator possible up here. Everything in ring
   3 was fixed arrays before it: a program decided at compile time how big
   the largest thing it would ever handle was, and the browser's limit on
   the size of a page was a number in a header rather than a property of
   the machine. */
#define SYS_SBRK          47

/* --- processes, the way Unix means the word ------------------------------
 *
 * Starting a program was one call: hand over a path, get a pid. That is a
 * spawn, and it is a fine thing to have, but it is not what a shell is built
 * out of. A shell needs to make a copy of itself, change something in the
 * copy, and only then become the new program, because everything it wants to
 * set up first — where the output goes, what the working directory is —
 * belongs to the child and must not touch the parent.
 *
 * So: fork makes the copy and returns twice, zero in the child and the
 * child's pid in the parent. exec replaces the program running in the
 * calling process without making a new one. getppid says who forked you. */
#define SYS_FORK          48
#define SYS_EXEC          49
#define SYS_GETPPID       50

/* --- descriptors -------------------------------------------------------
 *
 * A program's open files are numbered per program now, 0 in for input, 1 out
 * and 2 for errors, and these three calls are what a shell does with them.
 * dup2 is redirection; pipe is the other half of a pipeline. Neither is
 * interesting on its own: what makes them worth having is that they happen
 * between a fork and an exec, so the program being run never learns that its
 * output is not the screen. */
#define SYS_DUP           51
#define SYS_DUP2          52
#define SYS_PIPE          53

/* --- signals -----------------------------------------------------------
 *
 * SYS_SIGNAL says what this program wants done with one; SYS_SIGSEND raises
 * one against another program. There are three signals and no handlers; see
 * include/signal.h, which says what that leaves out and why. */
#define SYS_SIGNAL        54
#define SYS_SIGSEND       55

/* What a window is showing, in words, and what is being looked for. */
#define SYS_WIN_TEXT      56
#define SYS_WIN_FIND      57
#define TLS_WHY   0
#define TLS_WHAT  1

/* Why connect() or connect_tls() said no. In the order the attempt happens,
   so the first one that applies is the one that comes back: a machine with
   no address never reaches the resolver, and nothing that failed before the
   handshake has anything to say about a certificate. */
#define NET_ERR_DOWN     (-2)
#define NET_ERR_RESOLVE  (-3)
#define NET_ERR_CONNECT  (-4)
#define NET_ERR_TLS      (-5)
#define NET_ERR_BUSY     (-6)

/* The one door into the kernel. The registers are the same ones a 32-bit zelr
   used, only twice as wide, which is why every argument is a word rather than
   an int: an int would quietly cut the top half off a pointer. */
static inline zelr_word syscall(zelr_word n, zelr_word a, zelr_word b, zelr_word c) {
    zelr_word r;
    __asm__ volatile ("int $0x80"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}

/* Said to be noreturn because it is, and because that is what tells the
   compiler the end of a program that finishes on one is not reachable.
   Without it every window program ends in a warning about a missing
   return, and in a freestanding build main is not special enough to be
   excused it. The loop is unreachable and is what makes the promise
   true rather than merely claimed. */
/* Ending, spelled so that the libc can end a program too without owning
   the name `exit`: that one belongs to <stdlib.h> when there is one. */
__attribute__((noreturn))
static inline void zelr_exit(int code) {
    syscall(SYS_EXIT, code, 0, 0);
    for (;;) { }
}

#ifndef ZELR_NO_SUGAR
__attribute__((noreturn))
static inline void exit(int code) { zelr_exit(code); }

static inline void putc(char ch)         { syscall(SYS_PUTC, ch, 0, 0); }
#endif

static inline void zelr_putc(char ch)    { syscall(SYS_PUTC, ch, 0, 0); }
static inline int  getpid(void)          { return syscall(SYS_GETPID, 0, 0, 0); }
static inline int  ticks(void)           { return syscall(SYS_TICKS, 0, 0, 0); }
static inline void sleep_ms(int ms)      { syscall(SYS_SLEEP, ms, 0, 0); }

static inline int write(const char *s, int len) {
    return syscall(SYS_WRITE, 0, (zelr_word)s, len);
}

static inline int read_file(const char *name, char *buf, int cap) {
    return syscall(SYS_READ_FILE, (zelr_word)name, (zelr_word)buf, cap);
}

/* --- strings -------------------------------------------------------------

   Enough of a string library to write a program with, for a program that
   has no libc. One built against sdk/libc gets <string.h> instead, with the
   signatures C actually specifies, and sets ZELR_NO_SUGAR so that these are
   not also declared. */

#ifndef ZELR_NO_SUGAR
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
#endif  /* ZELR_NO_SUGAR */

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
    char name[64];              /* the kernel writes all of it */
} zelr_stat;

static inline int open(const char *path, u32 flags) {
    return syscall(SYS_OPEN, (zelr_word)path, (zelr_word)flags, 0);
}
static inline int close(int fd)  { return syscall(SYS_CLOSE, fd, 0, 0); }

/* Reading and writing a descriptor. Spelled twice: the zelr_ names are
   always here, and the short ones only when there is no libc to argue
   with -- <stdio.h> has an fread and an fwrite of its own, with four
   arguments and a FILE, and two functions of one name is not a thing C
   allows however different they are. */
static inline int zelr_fread(int fd, void *buf, int len) {
    return syscall(SYS_FREAD, fd, (zelr_word)buf, len);
}
static inline int zelr_fwrite(int fd, const void *buf, int len) {
    return syscall(SYS_FWRITE, fd, (zelr_word)buf, len);
}

#ifndef ZELR_NO_SUGAR
static inline int fread(int fd, void *buf, int len) {
    return zelr_fread(fd, buf, len);
}
static inline int fwrite(int fd, const void *buf, int len) {
    return zelr_fwrite(fd, buf, len);
}
#endif
static inline int seek(int fd, int off, int whence) {
    return syscall(SYS_SEEK, fd, off, whence);
}

static inline int unlink(const char *path) { return syscall(SYS_UNLINK, (zelr_word)path, 0, 0); }
static inline int mkdir(const char *path)  { return syscall(SYS_MKDIR, (zelr_word)path, 0, 0); }
static inline int rmdir(const char *path)  { return syscall(SYS_RMDIR, (zelr_word)path, 0, 0); }

/* Returns 1 when an entry was produced, 0 past the end, -1 on error. */
static inline int readdir(const char *path, int index, zelr_stat *out) {
    return syscall(SYS_READDIR, (zelr_word)path, index, (zelr_word)out);
}
static inline int stat(const char *path, zelr_stat *out) {
    return syscall(SYS_STAT, (zelr_word)path, (zelr_word)out, 0);
}
static inline int chdir(const char *path) { return syscall(SYS_CHDIR, (zelr_word)path, 0, 0); }
static inline int getcwd(char *buf, int cap) {
    return syscall(SYS_GETCWD, (zelr_word)buf, cap, 0);
}

/* Reads a whole file into a caller-supplied buffer. Returns the length. */
static inline int slurp(const char *path, char *buf, int cap) {
    int fd = open(path, O_READ);
    if (fd < 0) return -1;
    int total = 0;
    for (;;) {
        int n = zelr_fread(fd, buf + total, cap - total);
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
    int n = zelr_fwrite(fd, buf, len);
    close(fd);
    return n;
}

/* --- the network ---------------------------------------------------------

   Several connections at once. connect() gives back a small number and
   everything afterwards is handed that number, which is what lets a page
   fetch its pictures while it is still reading the page, and two programs
   be online at the same time. It used to be one socket for the whole
   machine and a second program was told it was busy.

   The number belongs to the program that opened it. Handing another
   program's number to send() does not work, which with one socket was not
   a question anybody could ask.

   `host` may be a name or a dotted address. */

typedef struct {
    u32 up;
    u32 ip, gateway, netmask, dns;
    u8  mac[6];
    u16 pad;
} zelr_netinfo;

/* A socket, or one of the NET_ERR_ numbers. */
static inline int connect(const char *host, int port) {
    return syscall(SYS_CONNECT, (zelr_word)host, port, 0);
}

/* The same thing with TLS 1.3 done on it first, including checking that the
   certificate at the other end is for `host`. Zero means the connection is
   open and everything sent through send() from here on is encrypted; there
   is no separate call to turn it on and so no way to forget one.
 *
   Anything else means no connection at all, encrypted or otherwise: a failed
   handshake closes the socket rather than leaving one open that a caller
   might use anyway. tls_why() says what went wrong, and is meant to be shown
   to somebody rather than logged.

   One of these at a time on the machine. The stack holds several
   connections; the TLS session state in the kernel is still single, so a
   second encrypted connection is refused rather than quietly taking the
   first one's keys. Plain ones alongside it are fine. */
static inline int connect_tls(const char *host, int port) {
    return syscall(SYS_TLS_CONNECT, (zelr_word)host, port, 0);
}

static inline int tls_status(char *out, int cap, int which) {
    return syscall(SYS_TLS_STATUS, (zelr_word)out, cap, which);
}
static inline int tls_why(char *out, int cap)  { return tls_status(out, cap, TLS_WHY); }
static inline int tls_what(char *out, int cap) { return tls_status(out, cap, TLS_WHAT); }
static inline int send(int sock, const void *buf, int len) {
    return syscall(SYS_SEND, sock, (zelr_word)buf, len);
}
/* recv gives back the number of bytes it took out of the connection, 0 when
   nothing arrived in time and the connection is still up, and NET_EOF once
   the other end has finished and there is nothing left. A loop that stops on
   0 stops early on a slow server; one that never stops hangs on a finished
   one. */
#define NET_EOF (-2)

static inline int recv(int sock, void *buf, int len) {
    return syscall(SYS_RECV, sock, (zelr_word)buf, len);
}
static inline int disconnect(int sock) {
    return syscall(SYS_DISCONNECT, sock, 0, 0);
}

static inline int resolve(const char *host, u32 *out) {
    return syscall(SYS_RESOLVE, (zelr_word)host, (zelr_word)out, 0);
}
static inline int netinfo(zelr_netinfo *out) {
    return syscall(SYS_NETINFO, (zelr_word)out, 0, 0);
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
} zelr_sysinfo;

static inline int sysinfo(zelr_sysinfo *out) {
    return syscall(SYS_SYSINFO, (zelr_word)out, 0, 0);
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

/* Must match zelr_task_t in include/syscall.h, which the kernel copies
   whole into whatever address is handed to tasks(). See tools/abicheck.py:
   this was 32 and the kernel's was 64, so every call wrote 32 bytes past
   the end of the caller's variable. */
typedef struct {
    u32  pid;
    u32  state;
    u32  slices;
    u32  idle;
    u32  user;
    char name[64];
} zelr_task;

/* How many words there are before the null. The vector main was handed
   ends in one, and so must any vector handed to the two calls below: that
   is the convention everywhere and it is why they do not take a count. */
static inline int argv_len(char *const *argv) {
    int n = 0;
    if (argv) while (argv[n]) n++;
    return n;
}

/* Starts a program on a vector of words. argv[0] is the program's own name
   by convention, and nothing enforces that. */
static inline int spawnv(const char *path, char *const *argv) {
    return (int)syscall(SYS_SPAWN_ARGV, (zelr_word)path, (zelr_word)argv,
                        argv_len(argv));
}

/* Starts one on a single word, which is almost always the file it is being
   asked to open. */
static inline int spawn_arg(const char *path, const char *arg) {
    char *v[3];
    v[0] = (char *)path;
    v[1] = (char *)arg;
    v[2] = 0;
    return spawnv(path, v);
}
/* Moves the heap break and returns where it was, which is the address of
   whatever was just handed out. See alloc.h, which is the only thing that
   should be calling this: two allocators sharing one break would each
   believe they owned what the other handed out. */
/* Makes a copy of this process. Returns 0 in the copy, the copy's pid in
   the original, and -1 when it could not.
 *
   Both sides come back from this same line. That is the whole of it and it
   is the thing that takes a moment to believe the first time. */
static inline int fork(void) {
    return (int)syscall(SYS_FORK, 0, 0, 0);
}

/* Replaces the program running in this process. Returns only on failure,
   because on success there is nothing left to return to. */
static inline int execv(const char *path, char *const *argv) {
    return (int)syscall(SYS_EXEC, (zelr_word)path, (zelr_word)argv,
                        argv_len(argv));
}

static inline int exec(const char *path, const char *arg) {
    char *v[3];
    v[0] = (char *)path;
    v[1] = (char *)arg;
    v[2] = 0;
    return execv(path, v);
}

static inline int getppid(void) {
    return (int)syscall(SYS_GETPPID, 0, 0, 0);
}

/* --- signals -------------------------------------------------------------
 *
 * Three of them. A program can have one ignored, take what it does by
 * default -- which for all three is that it ends -- or be told and carry
 * on. A shell ignores SIGINT so that ctrl-C reaches the program it
 * started rather than the shell waiting for it.
 *
 * SIGKILL can be neither caught nor ignored, because something has to be
 * final. */
#define SIGINT   2
#define SIGKILL  9
#define SIGTERM 15

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

/* Where a handler returns to.
 *
 * A handler is an ordinary function and ends in a ret, so something has to
 * be under it to return to. The kernel cannot provide it: it has no code
 * mapped in ring 3, and putting a few instructions on the stack for the
 * program to run would mean a stack that can be executed. So the program
 * carries them, and hands the address over when it asks for a handler.
 *
 * The call below does not come back. It puts the program where the signal
 * found it, which may be in the middle of a completely different function,
 * so the loop after it is unreachable and is there to say so.
 */
__attribute__((naked, used))
static void __zelr_sigreturn(void) {
    __asm__ volatile(
        "movl $" ZELR_STR(SYS_SIGRETURN) ", %eax\n"
        "int $0x80\n"
        "1: jmp 1b\n");
}

/* SIG_DFL, SIG_IGN, or a function of yours.

   A handler runs on the program's own stack, between two of its own
   instructions, so everything it touches is the program's. It may not
   return to its caller -- there isn't one -- and it may take as long as it
   likes: the same signal is held off until it returns rather than
   arriving on top of itself. */
static inline int signal(int sig, sighandler_t how) {
    return (int)syscall(SYS_SIGNAL, sig, (zelr_word)how,
                        (zelr_word)&__zelr_sigreturn);
}

/* Raises one against another program. KILL cannot be ignored. */
static inline int send_signal(int pid, int sig) {
    return (int)syscall(SYS_SIGSEND, pid, sig, 0);
}

/* The lowest free descriptor, referring to the same open file as `fd`. */
static inline int dup(int fd) { return (int)syscall(SYS_DUP, fd, 0, 0); }

/* Makes `to` refer to what `fd` refers to, closing whatever `to` was. This
   is how output is redirected: dup2(f, 1) in a freshly forked child. */
static inline int dup2(int fd, int to) {
    return (int)syscall(SYS_DUP2, fd, to, 0);
}

/* A pipe, as two descriptors: ends[0] is read from, ends[1] written to. */
static inline int pipe(int ends[2]) {
    return (int)syscall(SYS_PIPE, (zelr_word)ends, 0, 0);
}

/* --- memory that costs what it is used -----------------------------------
 *
 * A mapping is a promise, not an allocation. Nothing is taken when this
 * returns; a page arrives the first time you touch one, so asking for a
 * gigabyte and writing a page costs a page.
 *
 * That is what it is for. An allocator can reserve the room it might need
 * rather than the room it has, and a program can keep a large sparse thing
 * without paying for the empty parts of it.
 *
 * Anonymous only, and zeroed. There is no file mapping: read() reads files.
 *
 *   char *p = map(64 * 1024 * 1024, PROT_READ | PROT_WRITE);
 *   if (p) { p[0] = 1; unmap(p, 64 * 1024 * 1024); }
 */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2

/* Returns the address, or 0. The length is rounded up to whole pages. */
static inline void *map(u64 len, int prot) {
    return (void *)syscall(SYS_MMAP, (zelr_word)len, prot, 0);
}

/* Gives one back, whole. The address and length must be the ones map
   returned: a partial unmap would mean splitting one range into two, and an
   unmap that fails halfway has taken the pages and kept the promise. */
static inline int unmap(void *at, u64 len) {
    return (int)syscall(SYS_MUNMAP, (zelr_word)at, (zelr_word)len, 0);
}

/* Puts what has been written to this file where the power going will not
   take it, and does not return until it is there.

   A file is held in memory until its last descriptor closes. That is fine
   for a program that writes something and stops, and no use at all to one
   that keeps a file open: everything written since is in memory and on
   the disk there is nothing. This is how a program says now.

   Returning 0 means the bytes are on the drive rather than in its cache.
   A pipe and the console have no disk behind them and are quietly fine. */
/* A different name for a file, within one directory on the disk.

   One write of eleven bytes inside the directory entry that is already
   there, so there is never a moment with two names for the file or none.
   An existing destination is removed first.

   Across directories is refused, and so is a name that does not fit 8.3,
   because both would need a second entry and FAT has nowhere to say that
   two entries are one rename in progress. Copy and delete instead: it is
   slower, it is not atomic either, and it does not corrupt anything. */
/* --- waiting on several descriptors at once ----------------------------
 *
 * Every read blocks, which is fine for a program doing one thing and
 * impossible for one doing two: reading either descriptor is a commitment
 * to it, and the other one is then not being watched.
 *
 * The bits are the ones every system uses, so code written elsewhere means
 * what it says here.
 *
 *   pollfd_t w[2] = { { 0, POLLIN, 0 }, { fd, POLLIN, 0 } };
 *   if (poll(w, 2, 1000) > 0) { ... }
 *
 * It looks again rather than being woken, so it can be up to a tick late.
 * At most sixteen at once.
 */
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

typedef struct {
    int   fd;
    short events;
    short revents;
} pollfd_t;

/* How many have something to report, 0 if the wait ran out, -1 on a bad
   ask. A negative timeout waits for as long as it takes. */
static inline int poll(pollfd_t *fds, u32 n, int timeout_ms) {
    return (int)syscall(SYS_POLL, (zelr_word)fds, (zelr_word)n, timeout_ms);
}

static inline int zelr_rename(const char *from, const char *to) {
    return (int)syscall(SYS_RENAME, (zelr_word)from, (zelr_word)to, 0);
}

#ifndef ZELR_NO_SUGAR
static inline int rename(const char *from, const char *to) {
    return zelr_rename(from, to);
}
#endif

static inline int fsync(int fd) {
    return (int)syscall(SYS_FSYNC, fd, 0, 0);
}

static inline void *sbrk(i64 delta) {
    return (void *)syscall(SYS_SBRK, (zelr_word)delta, 0, 0);
}

static inline int spawn(const char *path) {
    return syscall(SYS_SPAWN, (zelr_word)path, 0, 0);
}
static inline int wait_for(int pid) {
    return syscall(SYS_WAIT, pid, 0, 0);
}
static inline int kill(int pid) {
    return syscall(SYS_KILL, pid, 0, 0);
}
/* Returns 1 when an entry was produced, 0 past the end. */
static inline int tasks(int index, zelr_task *out) {
    return syscall(SYS_TASKS, index, (zelr_word)out, 0);
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
    return syscall(SYS_CLIP_SET, (zelr_word)text, len, 0);
}

/* With cap 0 this answers how many bytes are waiting, so a caller can size a
   buffer before asking for the contents. */
static inline int clip_get(char *out, int cap) {
    return syscall(SYS_CLIP_GET, (zelr_word)out, cap, 0);
}

static inline int clip_len(void) { return syscall(SYS_CLIP_GET, 0, 0, 0); }

/* --- sound ----------------------------------------------------------------

   Signed sixteen bit samples, interleaved, at whatever rate the hardware
   agreed to, which is why the rate is asked for rather than assumed. A write
   returns when the frames are in the buffer, which for a buffer that only
   drains in real time means it waits. */
typedef struct {
    u32 present;
    u32 rate;
    u32 channels;
    u32 reserved;
} zelr_sound;

static inline int sound_info(zelr_sound *out) {
    return syscall(SYS_SOUND_INFO, (zelr_word)out, 0, 0);
}

static inline int sound_write(const short *frames, int count) {
    return syscall(SYS_SOUND_WRITE, (zelr_word)frames, (zelr_word)count, 0);
}

/* Neither of these comes back on a machine where it works. */
static inline int power_off(void) {
    return syscall(SYS_POWER, POWER_OFF, 0, 0);
}
static inline int power_reboot(void) {
    return syscall(SYS_POWER, POWER_REBOOT, 0, 0);
}

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

/* The wheel turned over this window: y is the number of steps, positive
   downward. Nothing has to ask for these; a window that ignores them
   simply does not scroll. */
#define WIN_EV_SCROLL 5
/* Somebody is looking for a word. What they are looking for does not fit in
   an event, so ask for it with win_find_query; ev.y is which match to go
   to, counting from nought, so pressing return again walks through them. */
#define WIN_EV_FIND   6

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
    return syscall(SYS_WIN_CREATE, (zelr_word)title, w, h);
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

/* What this window is showing, in words, so the desktop can find things in
   it. A program that never says is never searched -- which is honest, and
   is why the find bar counts the windows it can look in.

   Said again whenever what is shown changes. It is a copy, taken here, so
   the buffer is the caller's to reuse the moment this returns. */
static inline int win_set_text(int handle, const char *s, int len) {
    return syscall(SYS_WIN_TEXT, handle, (zelr_word)s, len);
}

/* And what is being looked for, once WIN_EV_FIND says somebody is. */
static inline int win_find_query(char *out, int cap) {
    return syscall(SYS_WIN_FIND, (zelr_word)out, cap, 0);
}

static inline int win_poll(int handle, win_event *ev) {
    return syscall(SYS_WIN_POLL, handle, (zelr_word)ev, 0);
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

/* --- where a program begins ----------------------------------------------
 *
 * The kernel enters here with the stack laid out the way System V says it
 * is: the count, then the vector, then a null, then an environment that is
 * empty. So a program defines main and reads its words from its arguments,
 * the way a program does anywhere else, and a startup file written for
 * another system would find what it expects too.
 *
 * This is startup code rather than a function. Nothing called it, so there
 * is no return address and no frame to unwind, which is what `naked` says
 * and why the body is instructions rather than C: the prologue a compiler
 * would write here would be reading a frame that does not exist.
 *
 * main may take the two arguments or take none. Both work, because the
 * caller cleans up on this machine and an argument nobody reads costs a
 * register that was going to be written anyway. It may also simply return:
 * what it returns becomes the exit status, and what follows the call makes
 * sure it never comes back here.
 */

#ifndef ZELR_NO_START
__attribute__((naked, section(".text._start")))
void _start(void) {
    __asm__ volatile(
        "xorl %ebp, %ebp\n"                      /* the frame chain ends */
        "movq (%rsp), %rdi\n"                    /* argc */
        "leaq 8(%rsp), %rsi\n"                   /* argv */
        "andq $-16, %rsp\n"                      /* what main is promised */
        "call main\n"
        "movl %eax, %ebx\n"                      /* the status it returned */
        "movl $" ZELR_STR(SYS_EXIT) ", %eax\n"
        "int $0x80\n"
        "1: jmp 1b\n");                          /* exit does not return */
}
#endif  /* ZELR_NO_START */
