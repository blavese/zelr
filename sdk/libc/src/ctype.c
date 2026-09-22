/* The character tests, as tests rather than as a table.
 *
 * Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each takes an int because the standard says so: the value is either an
 * unsigned char or EOF, and a char parameter could not hold both. Anything
 * outside that range answers no, rather than reading off the end of a table
 * that is not here in the first place.
 *
 * ASCII only, and said out loud: this machine has one character set, and
 * pretending otherwise would mean a locale, which it also does not have.
 */
#include <ctype.h>

int isdigit(int c)  { return c >= '0' && c <= '9'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int isalpha(int c)  { return islower(c) || isupper(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isblank(int c)  { return c == ' ' || c == '\t'; }
int iscntrl(int c)  { return (c >= 0 && c < 32) || c == 127; }
int isgraph(int c)  { return c > 32 && c < 127; }
int isprint(int c)  { return c >= 32 && c < 127; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }

int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n'
        || c == '\v' || c == '\f' || c == '\r';
}

int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
