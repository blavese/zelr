# Writing a program for zelr

Everything a program needs to be built somewhere else and run on zelr is in
this directory: the header, the link script, a build line and an example.
Nothing here includes anything from the rest of the repository, and
`tools/sdkcheck.py` proves it by copying these files into a directory outside
the tree, building there, and running the result on a real machine.

```bash
bash build.sh hello.c
```

That produces `hello.elf`. Put it on a zelr disk — anywhere, it does not have
to be `/bin` — and type its name.

## What a program is

A single freestanding static ELF executable. No libc, no shared libraries, no
dynamic linking, no relocation: it is linked to run at one address and it runs
at that address, in its own page tables, in ring 3.

```c
#include "zelr.h"

int main(int argc, char **argv) {
    puts("hello\n");
    return 0;
}
```

`zelr.h` is the whole interface: fifty-eight system calls and a little sugar
over them. Every one is `int $0x80` with the number in `rax`, so there is
nothing to link against and nothing to find at runtime.

## Where it begins

The kernel enters `_start`, which `zelr.h` defines for you, with the stack
laid out the way System V says:

```
    rsp ->  argc
            argv[0]
            ...
            argv[argc-1]
            NULL            the end of the vector
            NULL            an environment, which is always empty here
            the strings themselves
```

`rsp` is on a sixteen byte boundary. `_start` reads the count and the vector,
aligns, calls `main`, and calls `exit` with whatever `main` returned. A
program may define `int main(void)` instead and ignore all of it.

The environment is a pair of nulls rather than something missing, so startup
code that walks past the end of `argv` looking for one finds the terminator
it expects rather than reading whatever is next.

There is a limit on how much a program can be started on: the block is built
inside one page of the new program's stack, up to 64 words. Asking for more
is refused — the program is not started at all — rather than started on a
vector quietly cut short.

## The address space

```
  0x0000008000000000   the bottom of the half a program has to itself
  0x0000008010000000   the heap grows up from here, a page at a time (sbrk)
  0x0000008038000000   and no further
  0x0000008040000000   the program image, which is where zelr.ld links it
  0x0000008050000000   the top of the stack, sixteen pages of it
```

Nothing below the program image is mapped until it is asked for, and nothing
above the stack exists. Running off either end is a page fault naming the
program, which is the better of the two failures available and is still a
failure.

`sbrk` is the only call underneath an allocator. `userland/alloc.h` in this
repository is one written over it and is yours to copy; it is not here,
because an allocator is something a program can write and a system call
number is not.

## The flags, and why each one

```
-target x86_64-freestanding-none
-ffreestanding -nostdlib -static
-mcmodel=large      linked at 0x8040000000, further from zero than a
                    32-bit displacement reaches
-mno-red-zone       an interrupt can arrive between any two instructions,
                    and the kernel does not respect the 128 bytes below rsp
-fno-pic -fno-pie   nothing relocates anything
-fno-builtin        or a copy loop becomes a call to memcpy, which is not
                    here to be called
-Wl,-T,zelr.ld
```

`-fno-builtin` is the one that bites. Clang recognises a hand-written byte
loop and replaces it with a call into a C library that does not exist, and
the error arrives from the linker naming a function nobody wrote.

`build.sh` uses zig for its bundled clang and lld, which cross compile to this
target with nothing installed. Any clang and lld will do: set `CC`.

## What is not here

- **No `sigaction`.** `signal()` takes a handler and that is the whole of
  it: no mask a program can set for itself, no restarting of an
  interrupted system call, no siginfo. A handler is `void f(int)`, it runs
  on your own stack, and the same signal is held off until it returns.
- **No environment.** `envp` is a terminator and nothing else.
- **No threads.** `fork` makes a process; there is nothing smaller.
- **No memory mapping.** `sbrk` moves a break, and that is the whole of it.
- **No shared libraries**, which is why every program here is one `.c` file:
  what would be a library is a header included into it.

## The numbers are the interface

The system call numbers in `zelr.h` are fixed. A program built against them
has nothing but the number to go on — there is no name to look up and nothing
to negotiate — so a call that goes away leaves a gap rather than having its
number reused. 44 is the first of those: it was `SYS_GETARG`, which is how a
program used to ask for the single string it was started on, before a program
was started on words.

`tools/abicheck.py` compares this header against `include/syscall.h` on every
run of the gate, because the two sides are written out twice and nothing else
makes them agree.
