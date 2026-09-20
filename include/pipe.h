#pragma once
#include "types.h"

/* A pipe: bytes written at one end and read at the other.
 *
 * There is nothing to it but a ring buffer and two rules, and the rules are
 * the whole point. A reader with nothing to read waits instead of being told
 * there is nothing; a writer with nowhere to put it waits instead of losing
 * it. That is what makes `a | b` work without either program knowing the
 * other exists: b is simply slow to read, and a stops until it catches up.
 *
 * The third rule is how it ends. A read on an empty pipe that no longer has
 * a writer returns zero rather than waiting, because zero is how every
 * program already spells end of file, and a pipeline whose left hand side
 * has finished must not hang its right hand side forever.
 *
 * The buffer is fixed and small. A pipe is not storage: anything that wants
 * to hold four kilobytes of output is holding it in its own memory, and a
 * pipe that grew to fit whatever was poured into it would turn a program
 * that forgot to read into a machine that ran out of memory.
 */

#define PIPE_SIZE 4096u

typedef struct pipe {
    u8  buf[PIPE_SIZE];
    u32 head;                /* where the next byte is read from */
    u32 tail;                /* where the next byte is written to */
    u32 count;               /* how many are in it */

    /* How many descriptors still refer to each end. A pipe goes away when
       both reach zero, and the interesting one is writers: the moment it
       hits zero every reader is at end of file. */
    u16 readers, writers;

    /* Two addresses to wait on: one for a reader with nothing to read, one
       for a writer with nowhere to put it. Their contents are never looked
       at — a wait queue is keyed by the address alone — so they are a byte
       each and exist only to be distinct. */
    u8 has_data, has_space;
} pipe_t;

/* A new pipe with one reader and one writer already counted, because the
   only thing that makes one is the call that hands back both ends. */
pipe_t *pipe_new(void);

/* Blocks until there is something, and returns 0 at end of file. Never
   returns less than it could: a short read means that is all there was. */
int pipe_read(pipe_t *p, void *dst, u32 len);

/* Blocks until it all fits. Returns -1 when nobody is left to read it,
   which is this kernel's version of the signal it does not have yet. */
int pipe_write(pipe_t *p, const void *src, u32 len);

/* Drops one reference to one end. Frees the pipe when both ends are gone. */
void pipe_close(pipe_t *p, bool write_end);

/* How many bytes are sitting in it, for anything that wants to look without
   taking them. */
u32 pipe_count(const pipe_t *p);

/* How many pipes exist right now, which is a leak check: a shell that ran a
   thousand pipelines should be back at zero. */
u32 pipe_live(void);
