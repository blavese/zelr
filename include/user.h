#pragma once
#include "types.h"

/* Runs the built-in ring 3 test program. Returns its pid, or negative. */
int user_spawn_stub(const char *name);

/* Runs a flat binary: the image is copied to the load address and entered
   at its first byte. */
int user_spawn_flat(const char *name, const u8 *image, u32 size);

/* Loads an ELF executable and runs it in ring 3. Returns the pid, or a
   negative ELF_ERR_* code. */
int user_spawn_elf(const char *name, const u8 *image, u32 size);

/* The same, with one string the new program can ask for. Set before the task
   is able to run, because a program that reads its argument at startup would
   otherwise race the call that gave it one. */
int user_spawn_elf_arg(const char *name, const u8 *image, u32 size,
                       const char *arg);

/* Moves the calling program's heap break by `delta` bytes and returns where
   it was before, which is the address of whatever was just handed out. A
   delta of zero asks where it is without moving it.

   Returns 0 when it cannot: out of frames, or asked for more than the space
   between the heap and the program image above it. */
u64 user_sbrk(i64 delta);

/* Where a program's heap starts. Below the image rather than above it: the
   image can be loaded as high as USER_LOAD_MAX and the stack sits above
   that, so the only room that is certainly free is underneath. */
#define USER_HEAP_BASE (USER_SPACE_BASE + 0x10000000ull)
#define USER_HEAP_MAX  (USER_SPACE_BASE + 0x38000000ull)

/* Maps a fresh stack into an address space. Public because exec builds one
   for a program it is putting into a process that already exists. */
bool user_build_stack(u64 dir);

/* Where a program's stack pointer starts. See kernel/user.c for why this is
   eight below a sixteen byte boundary rather than on one. */
#define USER_STACK_TOP   (USER_SPACE_BASE + 0x50000000ull)
#define USER_STACK_START (USER_STACK_TOP - 8)
