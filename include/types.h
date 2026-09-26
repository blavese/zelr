#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

typedef uint32_t u32; typedef int32_t  i32;
typedef uint16_t u16; typedef int16_t  i16;
typedef uint8_t  u8;  typedef int8_t   i8;
typedef uint64_t u64; typedef int64_t  i64;

#define KERNEL_NAME    "zelr"

/* Who wrote it and under what terms.
 *
 * Here rather than only in a file beside the source, because this ends up
 * inside the kernel image: a copy of zelr that has had the author taken
 * off it is a copy somebody had to modify to make, and the modification
 * is the thing the licence does not allow them to hide.
 *
 * Printed at boot, by `uname`, and in the system information window. */
#define KERNEL_AUTHOR  "blavese"
#define KERNEL_YEARS   "2026"
#define KERNEL_LICENSE "GPL-3.0-or-later"
#define KERNEL_VERSION "0.38.0"
