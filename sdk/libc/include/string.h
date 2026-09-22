/* string.h -- Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The signatures are the ones C specifies, which is the whole point of
 * this directory existing beside sdk/zelr.h: that header has strlen and
 * memcpy too, spelled the way this project spells them, and a program
 * written elsewhere expects these. */
#pragma once
#include <stddef.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *p, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *p, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
char  *strdup(const char *s);
char  *strtok(char *s, const char *sep);
size_t strspn(const char *s, const char *set);
size_t strcspn(const char *s, const char *set);
char  *strpbrk(const char *s, const char *set);
const char *strerror(int err);
