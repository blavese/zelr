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

/* The same, with the words the program was started on. argv[0] is what the
   caller was asked for, the way it is everywhere else; a program reads them
   off its own stack rather than asking for them, so there is no window in
   which it is running and they are not there yet. */
int user_spawn_elf_argv(const char *name, const u8 *image, u32 size,
                        int argc, const char *const *argv);

/* How many words a program can be started on. The whole block -- the strings
   and the vector pointing into them -- is built inside the last page of the
   stack, so the real limit is a page and this is only the shape of it. */
#define USER_ARGV_MAX 64

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

/* Maps a fresh stack into an address space and puts the program's words on
   it, and says where the stack pointer goes. Public because exec builds one
   for a program it is putting into a process that already exists.

   Fails, and maps nothing worth keeping, when the words do not fit. */
bool user_build_stack(u64 dir, int argc, const char *const *argv,
                      u64 *rsp_out);

/* The top of a program's stack. Where the stack pointer actually starts is
   a little below it and depends on the words the program was given, so it
   comes back from user_build_stack rather than being a constant here. */
#define USER_STACK_TOP   (USER_SPACE_BASE + 0x50000000ull)
