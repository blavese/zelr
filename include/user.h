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

/* --- memory a program asks for, and is given when it touches it ---------
 *
 * Between the heap's ceiling and the program image there are 128 MiB that
 * nothing else uses. That is where a mapping goes.
 *
 * Nothing is allocated when one is made. A mapping is an entry in a list
 * saying which addresses this program is allowed to reach for, and the
 * pages arrive one at a time as it reaches -- so a program that maps a
 * gigabyte and writes a page has been given a page. That is the whole
 * difference between what a program's address space costs and what it uses,
 * and it is why an allocator can ask for room it might need rather than
 * room it has.
 */
#define USER_MMAP_BASE (USER_SPACE_BASE + 0x38000000ull)
#define USER_MMAP_MAX  (USER_SPACE_BASE + 0x40000000ull)   /* the image */

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2

/* Reserves a range and returns where it starts, or 0. The length is rounded
   up to whole pages. Nothing is mapped yet. */
u64 user_mmap(u64 len, int prot);

/* Gives one back: the pages that did arrive are freed and the range stops
   being one this program may reach for. */
bool user_munmap(u64 at, u64 len);

/* A page a program reached for inside something it mapped, supplied.
 *
   Called from the fault handler and nowhere else. False means the address
   was not in anything this program asked for, which is the difference
   between a program using what it was given and a program with a bad
   pointer -- and the second of those is the one that ends. */
bool user_fault_fill(u64 addr, u64 err);

/* Forgets every mapping, which is what exec does. */
void user_drop_mappings(void);

/* The top of a program's stack. Where the stack pointer actually starts is
   a little below it and depends on the words the program was given, so it
   comes back from user_build_stack rather than being a constant here. */
#define USER_STACK_TOP   (USER_SPACE_BASE + 0x50000000ull)
