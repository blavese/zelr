#include "pipe.h"
#include "heap.h"
#include "string.h"
#include "wait.h"

/* See include/pipe.h for what a pipe is for. What is here is the ring and
 * the waiting, and the waiting is the part worth reading.
 *
 * A reader that found the pipe empty could spin: look, find nothing, give
 * the slice back, look again. That works and costs the whole processor. So
 * it blocks on an address instead, and whoever puts bytes in wakes that
 * address. The cost of waiting is then nothing at all until there is
 * something to say.
 *
 * Both waits are written as loops around the condition rather than a single
 * sleep, because a wakeup is a hint and not a promise: two readers can be
 * woken by one byte, and the one that arrives second has to find the pipe
 * empty again and go back to sleep.
 */

static u32 live;

/* A wait that has gone on this long has almost certainly deadlocked — a
 * program on both ends of its own pipe, writing more than fits before it
 * reads any of it. Unix would block forever and leave the process to be
 * killed; there is no signal here to kill it with, so the call gives up and
 * says what it managed instead of hanging a machine with no way out. */
#define PIPE_GIVE_UP_MS 10000u

/* Long enough that waiting is free, short enough that a wakeup which went
 * missing costs a pause rather than a hang. Nothing should depend on this:
 * it is a safety net under the wakeups, not the mechanism. */
#define PIPE_POLL_MS 200u

pipe_t *pipe_new(void) {
    pipe_t *p = (pipe_t *)kcalloc(sizeof(pipe_t));
    if (!p) return 0;
    p->readers = 1;
    p->writers = 1;
    live++;
    return p;
}

u32 pipe_count(const pipe_t *p) { return p ? p->count : 0; }
u32 pipe_live(void) { return live; }

int pipe_read(pipe_t *p, void *dst, u32 len) {
    if (!p || !dst) return -1;
    if (!len) return 0;

    u32 waited = 0;
    while (p->count == 0) {
        /* Nothing in it and nobody left to put anything in it. That is not
           an error and not a wait: it is the end of the file. */
        if (p->writers == 0) return 0;
        if (!wait_on(&p->has_data, PIPE_POLL_MS)) {
            waited += PIPE_POLL_MS;
            if (waited >= PIPE_GIVE_UP_MS) return 0;
        }
    }

    u32 n = p->count < len ? p->count : len;
    u8 *out = (u8 *)dst;
    for (u32 i = 0; i < n; i++) {
        out[i] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_SIZE;
    }
    p->count -= n;

    /* Room appeared, so anything that stopped for want of it can go on. */
    wake_all(&p->has_space);
    return (int)n;
}

int pipe_write(pipe_t *p, const void *src, u32 len) {
    if (!p || !src) return -1;
    if (!len) return 0;

    const u8 *in = (const u8 *)src;
    u32 done = 0;
    u32 waited = 0;

    while (done < len) {
        /* Writing into a pipe with no reader is writing into nothing. Say
           so rather than filling the buffer and blocking forever. */
        if (p->readers == 0) return done ? (int)done : -1;

        if (p->count == PIPE_SIZE) {
            if (!wait_on(&p->has_space, PIPE_POLL_MS)) {
                waited += PIPE_POLL_MS;
                if (waited >= PIPE_GIVE_UP_MS) return done ? (int)done : -1;
            }
            continue;
        }
        waited = 0;

        u32 room = PIPE_SIZE - p->count;
        u32 n = len - done;
        if (n > room) n = room;
        for (u32 i = 0; i < n; i++) {
            p->buf[p->tail] = in[done + i];
            p->tail = (p->tail + 1) % PIPE_SIZE;
        }
        p->count += n;
        done += n;

        /* Wake after every chunk rather than at the end, so a reader waiting
           on a large write does not sit idle until the whole of it is in. */
        wake_all(&p->has_data);
    }
    return (int)done;
}

void pipe_close(pipe_t *p, bool write_end) {
    if (!p) return;

    if (write_end) {
        if (p->writers) p->writers--;
        /* The last writer closing is what ends the file, and a reader
           already asleep has to be told: nothing else will ever wake it. */
        if (p->writers == 0) wake_all(&p->has_data);
    } else {
        if (p->readers) p->readers--;
        if (p->readers == 0) wake_all(&p->has_space);
    }

    if (p->readers == 0 && p->writers == 0) {
        if (live) live--;
        kfree(p);
    }
}
