# zelr's libc

A C standard library, written from scratch, for programs that have never
heard of zelr.

`sdk/zelr.h` next door is enough to write a program *for* zelr. This is the
other direction: take C that was written for some other machine, compile it,
and run it here.

    bash build.sh myprogram.c
    # -> myprogram.elf, which runs on zelr

`hello.c` in this directory is the worked example. Every line of it is
standard C — `stdio.h`, `stdlib.h`, `string.h`, `ctype.h`, `math.h` — and
the word "zelr" does not appear in it.

## what is here

| header | what it has |
| --- | --- |
| `stddef.h` `stdint.h` `stdbool.h` `limits.h` `stdarg.h` | the types, from the compiler rather than guessed |
| `string.h` | `mem*`, `str*`, `strtok`, `strdup`, `strerror` |
| `ctype.h` | the character tests, ASCII |
| `stdlib.h` | `malloc` family, `strtol` family, `qsort`, `bsearch`, `rand`, `exit`, `atexit` |
| `stdio.h` | `FILE`, `fopen`/`fread`/`fwrite`/`fseek`, `printf` family, `perror` |
| `math.h` | `sqrt`, `pow`, `exp`, `log`, `sin`, `cos`, `tan`, `atan2`, rounding |
| `time.h` | `clock` |
| `errno.h` `assert.h` | `errno`, `assert` |

Each one is implemented in `src/`, in about the order a program meets them.

## what is not here, and why

**No locale.** One character set, and `ctype.h` says ASCII out loud rather
than pretending a table would be consulted.

**No wide characters.** `wchar_t` exists because `stddef.h` has to declare
it; nothing uses it.

**No calendar.** `clock()` works and `time()` returns `(time_t)-1`, which is
the standard's own answer for a machine that cannot tell you. The kernel
counts ticks since it started and does not know what year it is; a program
told it is 1970 behaves worse than one told nobody knows.

**No environment.** `getenv` returns `NULL` for every name, because nothing
sets one and nothing passes one on. `system()` sets `ENOSYS`.

**No threads, no signals here.** One thread in a program. Signals are in
`sdk/zelr.h`, where they are zelr's rather than POSIX's.

**`printf` prints `%f` in fixed notation**, six places by default, rounded at
the place being printed. `%e` and `%g` are accepted and print the same way.
An exact decimal expansion of a binary fraction is a much larger piece of
work than anything else in this directory.

**`math.h` is accurate to about twelve significant figures**, not the
fifteen a double holds. Every function there is a series or an iteration
written out, with the range reduction that makes it converge; getting the
last three figures would mean reducing the argument in higher precision than
the type being reduced.

## how it fits together

A zelr program is a freestanding static executable with no dynamic linking,
so there is no `libc.so` and no `libc.a` either: `build.sh` compiles the
libc's sources alongside yours in one invocation. That is a dozen files
rather than one, which is a reason to list them and not a reason to
introduce a build system.

Two macros arrange for the libc and `sdk/zelr.h` to share a program without
arguing over names, and `build.sh` sets both:

- `ZELR_NO_SUGAR` — `zelr.h` keeps its system calls and stands aside from
  `strlen`, `memcpy`, `puts`, `exit`, `fread` and the rest. Its versions are
  spelled the way this project spells them (`int strlen`, `void strcpy`),
  which is right for a program with no libc and a collision for a program
  with one. The always-available `zelr_` names (`zelr_exit`, `zelr_fread`,
  `zelr_fwrite`, `zelr_rename`) are how the libc reaches the kernel.
- `ZELR_NO_START` — the entry point comes from `src/crt0.c` rather than from
  the header, so that exactly one file in the program has it. What `crt0.c`
  adds is what a libc owes a program: `stdout` exists before `main` runs,
  and what was written to it goes out when `main` returns.

You can still use `sdk/zelr.h` from a program built this way — for the
network, the window system, signals, or anything else zelr has and C does
not. Include it after the standard headers and the two macros are already
set for you.

## proving it

`tools/libccheck.py` copies `sdk/` out of the tree, builds `hello.c` there
with nothing else in reach, writes the result onto a FAT volume this
repository made, boots a machine with that volume as its disk, and types the
program's name.

It then checks what was printed rather than only that something was: the
padding and alignment, `qsort`'s ordering, `strtol`'s stopping point, the
arithmetic to six places, and a file written and read back. A libc that runs
and prints the wrong number is worse than no libc at all.

---

Copyright (C) 2026 blavese. SPDX-License-Identifier: GPL-3.0-or-later.
See `NOTICE` in the repository root.
