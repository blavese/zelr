/* See include/clipboard.h. */
#include "clipboard.h"
#include "string.h"

static char buffer[CLIP_MAX];
static u32  length;
static u32  generation;

void clip_init(void) {
    length = 0;
    generation = 0;
    buffer[0] = 0;
}

bool clip_set(const char *text, u32 len) {
    if (!text) return false;
    if (len > CLIP_MAX - 1) return false;

    memcpy(buffer, text, len);
    buffer[len] = 0;
    length = len;
    generation++;
    return true;
}

u32 clip_get(char *out, u32 cap) {
    if (!out || cap == 0) return 0;
    u32 n = length < cap - 1 ? length : cap - 1;
    memcpy(out, buffer, n);
    out[n] = 0;
    return n;
}

u32 clip_len(void)        { return length; }
u32 clip_generation(void) { return generation; }
