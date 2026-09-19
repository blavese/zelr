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

/* Starting a program on something, and asking what this program was started
   on. One string, which for everything that uses it is a path. */
#define SYS_SPAWN_ARG     43
#define SYS_GETARG        44

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
