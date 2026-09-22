/* stdio.h -- Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A FILE here is a descriptor, a buffer and the two flags that say how a
 * read ended. The kernel has open, read, write, close and lseek and that
 * is the whole of what is underneath: no mode strings, no locale, no wide
 * characters, and no locking, because there is one thread in a program. */
#pragma once
#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)
#define BUFSIZ 4096
#define FOPEN_MAX 16
#define FILENAME_MAX 256

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct _zfile FILE;
typedef long fpos_t;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE  *fopen(const char *path, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *f);
int    fclose(FILE *f);
int    fflush(FILE *f);

size_t fread(void *p, size_t size, size_t count, FILE *f);
size_t fwrite(const void *p, size_t size, size_t count, FILE *f);

int    fgetc(FILE *f);
int    getc(FILE *f);
int    getchar(void);
char  *fgets(char *buf, int cap, FILE *f);
int    ungetc(int c, FILE *f);

int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);
int    putchar(int c);
int    fputs(const char *s, FILE *f);
int    puts(const char *s);

int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);

int    printf(const char *fmt, ...);
int    fprintf(FILE *f, const char *fmt, ...);
int    sprintf(char *out, const char *fmt, ...);
int    snprintf(char *out, size_t cap, const char *fmt, ...);
int    vprintf(const char *fmt, va_list ap);
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    vsprintf(char *out, const char *fmt, va_list ap);
int    vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);

int    remove(const char *path);
int    rename(const char *from, const char *to);
void   perror(const char *what);
