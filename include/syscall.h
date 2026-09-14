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

/* What SYS_TASKS reports about one task. */
typedef struct {
    u32  pid;
    u32  state;              /* 0 ready, 1 running, 2 sleeping, 3 blocked, 4 dead */
    u32  slices;
    u32  user;
    char name[32];
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
    char name[32];
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
