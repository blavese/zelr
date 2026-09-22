/* stdlib.h -- Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     2147483647

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

void  *malloc(size_t n);
void  *calloc(size_t count, size_t size);
void  *realloc(void *p, size_t n);
void   free(void *p);

_Noreturn void exit(int status);
_Noreturn void abort(void);
int    atexit(void (*fn)(void));

int       atoi(const char *s);
long      atol(const char *s);
long long atoll(const char *s);
double    atof(const char *s);

long               strtol(const char *s, char **end, int base);
unsigned long      strtoul(const char *s, char **end, int base);
long long          strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double             strtod(const char *s, char **end);

int   abs(int v);
long  labs(long v);
div_t div(int num, int den);
ldiv_t ldiv(long num, long den);

void  qsort(void *base, size_t n, size_t size,
            int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));

int   rand(void);
void  srand(unsigned seed);

char *getenv(const char *name);
int   system(const char *command);
