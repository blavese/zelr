#pragma once
#include "zelr.h"

/* Splitting a line into words.
 *
 * Written once because six programs need it and six programs getting it
 * slightly differently is how `grep "two words"` ends up working in one
 * place and not in another.
 *
 * The line is taken apart where it lies: each word is terminated in the
 * buffer at the space that ended it, and the array points into the line
 * rather than at copies. Nothing is allocated, so this works in a program
 * that has no heap and in one that has.
 *
 * Double quotes hold a word together and are removed. There is no escaping
 * and no single quote: a shell that supports everything is a shell nobody
 * can read, and this one says what it does not do rather than half doing it.
 */
static inline int args_split(char *line, char **out, int max) {
    int n = 0;
    char *p = line;

    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (*p == '"') {
            p++;
            out[n++] = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0;
        } else {
            out[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
        }
    }
    return n;
}

/* Puts the words back together into one string. Nothing needs this to run a
   program any more -- exec and spawn carry a vector -- so what is left is
   the places a line has to be shown or stored as it was typed. Returns the
   length written. */
static inline int args_join(char **words, int n, char *out, int cap) {
    int len = 0;
    for (int i = 0; i < n; i++) {
        if (len && len < cap - 1) out[len++] = ' ';
        for (const char *s = words[i]; *s && len < cap - 1; s++) out[len++] = *s;
    }
    out[len] = 0;
    return len;
}
