#pragma once
#include "types.h"

#define SYS_EXIT       0
#define SYS_PUTC       1
#define SYS_WRITE      2
#define SYS_GETPID     3
#define SYS_TICKS      4
#define SYS_SLEEP      5
#define SYS_READ_FILE  6

/* The window server. A graphical program creates a window, asks where its
   pixels landed in its own address space, and then draws into them. */
#define SYS_WIN_CREATE   7
#define SYS_WIN_SURFACE  8
#define SYS_WIN_SIZE     9
#define SYS_WIN_POLL    10
#define SYS_WIN_COMMIT  11
#define SYS_WIN_CLOSE   12

/* Files. A program can now create, read, write and delete them, and walk
   directories, which is what lets the shell move out of the kernel. */
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

/* Sockets. One TCP connection at a time, which is what the stack supports. */
#define SYS_CONNECT     25
#define SYS_SEND        26
#define SYS_RECV        27
#define SYS_DISCONNECT  28
#define SYS_RESOLVE     29
#define SYS_NETINFO     30
#define SYS_SYSINFO      31

/* Starting other programs. Until these existed, only the kernel could run
   one, which is why the shell and the launcher had to live inside it. */
#define SYS_SPAWN        32
#define SYS_WAIT         33
#define SYS_KILL         34
#define SYS_TASKS        35

/* A window that can change size. A program has to ask for this: one that
   never expected it would carry on drawing at the size it last read. */
#define SYS_WIN_RESIZABLE 36
#define SYS_WIN_RESIZE    37

/* Copy and paste. Text only; see include/clipboard.h for why. */
#define SYS_CLIP_SET      38
#define SYS_CLIP_GET      39

/* Sound. A program asks what the hardware is and then hands over frames of
   signed sixteen bit samples, interleaved, at the rate it was told. */
#define SYS_SOUND_INFO    40
#define SYS_SOUND_WRITE   41

/* Switching the machine off, or restarting it. Neither returns when it
   works, and the first says so rather than hanging when the firmware wanted
   something this kernel does not do. */
#define SYS_POWER         42

/* Starting a program on a vector of words: a path, an array of pointers,
   and how many. The words land on the new program's stack before its first
   instruction, the way System V says, so there is no call for reading them
   back and no moment when a program is running without them.

   44 was SYS_GETARG, which is how a program used to ask for the single
   string it was started on. It is retired rather than reused. These numbers
   are the interface now -- a program compiled against them somewhere else
   has nothing but the number to go on -- so from here they are kept, and a
   call that goes is a gap. */
#define SYS_SPAWN_ARGV    43

/* The same one socket, with a TLS 1.3 handshake done on it before anything
   is sent. Afterwards SYS_SEND and SYS_RECV carry the same bytes they always
   did and the encryption is not the caller's business, which is the point:
   a program should not be able to get this wrong by forgetting a step.
 *
   SYS_TLS_STATUS says what happened, because "it did not connect" is not
   worth showing anybody. Which one it answers is in rdx: TLS_WHY for the
   reason the last attempt failed, TLS_WHAT for what was agreed. */
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

/* Asked for by the few instructions a handler returns into, and by
   nothing else. It puts back the frame the program was interrupted at,
   so it does not return to its caller the way a call does -- it returns
   to wherever the program was when the signal arrived. */
#define SYS_SIGRETURN     58

/* --- memory a program asks for -----------------------------------------
 *
 * Nothing is allocated when one of these is made. A mapping is a promise
 * that a range of addresses is the program's to reach for, and the pages
 * arrive one at a time as it reaches -- so a program that maps a gigabyte
 * and writes a page has been given a page.
 *
 * Anonymous only: zeroed memory and nothing else. There is no file
 * mapping, because doing that honestly means holding a file open behind
 * the program's back or reading a path inside the fault handler, and read
 * still reads files.
 */
#define SYS_MMAP          59
#define SYS_MUNMAP        60

/* What has been written to a file, put where the power going will not
   take it. A file is held in memory until its last descriptor closes, so
   a program that keeps one open has nothing on the disk until this. */
#define SYS_FSYNC         61

/* A different name for a file, within one directory. Across directories
   is refused rather than done unsafely -- FAT has nowhere to say that two
   entries are one rename in progress, so a power cut in the middle would
   leave two names for one file and deleting either would corrupt the
   other. A caller that needs it can copy and delete. */
#define SYS_RENAME        62

/* What a window is showing, in words, so the desktop's find can look
   through it; and what is being looked for, for a window that has just been
   told somebody is looking. */
#define SYS_WIN_TEXT      56
#define SYS_WIN_FIND      57
#define TLS_WHY   0
#define TLS_WHAT  1

/* Why a connection did not happen.
 *
 * Connecting used to answer -1 for every reason there is, which put the
 * caller in the position of having to guess. A browser given that guessed
 * the last thing in the chain and told somebody their certificate had been
 * refused, on a machine that had no address at all and had never sent a
 * packet. These are ordered the way the attempt is: no card or no address,
 * then no answer from the resolver, then no answer from the host, and only
 * then anything the certificate had to do with. */
#define NET_ERR_DOWN     -2     /* no card, or no address on it */
#define NET_ERR_RESOLVE  -3     /* the name did not turn into an address */
#define NET_ERR_CONNECT  -4     /* the address did not answer */
#define NET_ERR_TLS      -5     /* it answered and would not prove who it was */
#define NET_ERR_BUSY     -6     /* the one connection is already in use */

#define POWER_OFF     0
#define POWER_REBOOT  1

typedef struct {
    u32 present;
    u32 rate;
    u32 channels;
    u32 reserved;
} sound_info_t;

/* What SYS_TASKS reports about one task. */
typedef struct {
    u32  pid;
    u32  state;              /* 0 ready, 1 running, 2 sleeping, 3 blocked, 4 dead */
    u32  slices;
    u32  idle;               /* of those, spent halted waiting */
    u32  user;
    char name[64];
} zelr_task_t;

/* What SYS_SYSINFO fills in: what the machine is, as far as a program is
   allowed to know. */
typedef struct {
    u32 cpus_found, cpus_started;
    u32 mem_total_kb, mem_used_kb;
    u32 heap_total_kb, mem_free_kb;
    u32 uptime_seconds;
    u32 tasks;
    u32 screen_w, screen_h;
    u32 syscalls;
    u32 disk_kb_free;
} zelr_sysinfo_t;

/* What SYS_STAT and SYS_READDIR fill in. Fixed layout: ring 3 reads this
   straight out of a buffer the kernel wrote. */
typedef struct {
    u32  size;
    u32  is_dir;
    /* As wide as VFS_NAME_MAX, which is checked in syscall.c. The kernel
       fills this with strncpy and strncpy pads to the full width, so a
       narrower field here is written past rather than truncated. */
    char name[64];
} zelr_stat_t;

/* What SYS_NETINFO fills in. */
typedef struct {
    u32 up;
    u32 ip, gateway, netmask, dns;
    u8  mac[6];
    u16 pad;
} zelr_netinfo_t;

void syscall_init(void);

/* Drops anything a dead task was holding, such as the one TCP socket. */
void syscall_release(u32 pid);

/* How many system calls have been served since boot. */
u32 syscall_count(void);
