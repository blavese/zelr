#pragma once
#include "types.h"

/* File descriptors, and the two layers underneath them.
 *
 * This used to be one layer. A program opened a file, got back an index into
 * a single kernel-wide array, and that index was tagged with the pid so no
 * other program could use it. It worked, and it was not a process model:
 * the numbers were global, so a program's first file might be 3 on a quiet
 * machine and 9 on a busy one; a forked child inherited nothing, because the
 * tag no longer matched; and there was no such thing as standard output, so
 * there was nothing for a shell to redirect.
 *
 * Unix splits it in two, and the split is the whole design:
 *
 *   - an open file description: the file itself, the position in it, whether
 *     it may be written. There is one of these per open, and several
 *     descriptors can point at the same one.
 *
 *   - a descriptor table: a small array per process, mapping the numbers a
 *     program uses to descriptions. The numbers are per process, so 1 is
 *     always standard output and never anything else.
 *
 * Everything a shell does falls out of that. fork copies the table, so both
 * sides refer to the same descriptions and a child inherits what the parent
 * had open. exec keeps the table, which is the point: the child arranges its
 * output between fork and exec, and the program it becomes knows nothing
 * about it. dup2 puts one description at another number, which is redirection
 * written out in full. Two descriptors on one description share a position,
 * which is why two commands appending to the same file do not overwrite each
 * other.
 *
 * The three numbers every program starts with point at the console, so a
 * program that writes to 1 writes to the screen unless somebody arranged
 * otherwise, and does not have to know which.
 */

/* How many files one process can have open at once. Matched by TASK_MAX_FD
   in include/sched.h, which is where the array actually lives; kernel/fd.c
   asserts they are the same number. */
#define FD_MAX 16

#define FD_STDIN  0
#define FD_STDOUT 1
#define FD_STDERR 2

void fd_init(void);

/* --- the table a task carries -------------------------------------------
 *
 * Passed as a bare array rather than a struct so that the scheduler can hold
 * one without knowing what is in it. Each entry is an index into the open
 * file table, or -1 for a number that is not open. */

void fd_table_init(i16 *slots);                       /* three consoles */
void fd_table_clone(i16 *dst, const i16 *src);        /* what fork does */
void fd_table_release(i16 *slots);                    /* what exit does */

/* --- what a program calls ------------------------------------------------ */

int  fd_open(const char *path, u32 flags);            /* flags are vfs.h's */
int  fd_read(int fd, void *buf, u32 len);
int  fd_write(int fd, const void *buf, u32 len);
int  fd_seek(int fd, i32 offset, u32 whence);         /* 0 set, 1 cur, 2 end */
int  fd_size(int fd);
bool fd_close(int fd);

/* The lowest free number, pointing at the same description as `fd`. */
int  fd_dup(int fd);

/* `to`, pointing at the same description as `fd`, closing whatever `to` was
   first. Redirection is this call and nothing else. */
int  fd_dup2(int fd, int to);

/* A pipe, as a pair of numbers: ends[0] reads, ends[1] writes. */
bool fd_pipe(int ends[2]);

/* How many descriptions are open, across the whole machine. A number that
   only goes up is a leak, which is what this is for. */
u32  fd_live(void);
