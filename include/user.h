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
