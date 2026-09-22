/* errno.h -- Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The kernel underneath returns a negative number and does not say which
 * of these it meant, so what is set here is what the libc itself can tell:
 * a file that would not open is ENOENT, an allocation that failed is
 * ENOMEM. Numbers that nothing sets are here because programs compare
 * against them, and comparing against a name that does not exist is a
 * compile error rather than a wrong answer. */
#pragma once

extern int errno;

#define EPERM    1
#define ENOENT   2
#define EINTR    4
#define EIO      5
#define EBADF    9
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EBUSY   16
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define EFBIG   27
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define EPIPE   32
#define ERANGE  34
#define ENOSYS  38
#define ENOTEMPTY 39
