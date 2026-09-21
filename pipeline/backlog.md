# Backlog

What the pipeline works through, in order. The top `todo` is taken next.

`author` is who writes it; the other one reviews. They alternate so that
neither model's blind spots get to compound, and so that every change is read
by something that did not write it.

`state` is one of `todo`, `doing`, `done`, `blocked`. The scripts write this
column; everything else is for people. A `blocked` task keeps its logs under
`pipeline/state/<id>/` and needs a look before it goes back to `todo`.

Keep tasks small enough that one of them is a single sitting's work. "Add a
driver" is a task; "finish the operating system" is not.

| id | author | title | state |
|---|---|---|---|
| rtc-clock | claude | Read the real time from the CMOS clock and expose it as /sys/time, so the desktop shows a clock rather than an uptime counter | blocked |
| pipes | codex | Add pipes: a pipe syscall, a read and write end, blocking on the wait queues that already exist, so two ring 3 programs can talk to each other | blocked |
| lfn-read | claude | Read VFAT long filenames, so a file saved as somethinglong.txt keeps its name instead of being truncated to 8.3 | blocked |
| ap-tasks | codex | Let the other processors run scheduled tasks rather than only work handed to them, with the locking that needs, and a test that proves work really ran on another core | done |
| editor | claude | A text editor for the desktop: open, edit, save, using the terminal's line editing and the window resize it already supports | todo |
| nvme | codex | An NVMe block driver behind the existing block layer, so it works on a machine with no AHCI controller | todo |
| demand-pages | claude | Map user pages on demand rather than all at once, so a program's address space costs what it uses | todo |
| xhci-hid | codex | USB: xHCI controller and a HID keyboard, because a modern laptop has no PS/2 port and the desktop is unusable without one | todo |

## Ideas not yet tasks

Too big or too vague to hand to an agent as they stand. Break one down before
moving it up.

Two entries were sitting here long after they had been done. Sound is an
HD Audio driver, an Ensoniq one and a player; signals are in, three of them.
Both are struck out rather than deleted, because an idea that turns out to
have been finished is worth a line saying so.

- ~~Sound. An AC97 or HD Audio driver, and something to play through it.~~
  Done: `kernel/hda.c`, `kernel/ens.c`, `userland/music.c`.
- ~~Signals, or something like them: a way to interrupt a running program.~~
  Done, in the narrow sense: three signals and no handlers. The rest of it
  is the entry below.
- ~~**Signal handlers.** A program can be interrupted, killed or asked to
  end, and it cannot be told and carry on.~~ Done: `kernel/signal.c` builds
  the frame, `sdk/zelr.h` carries the few instructions a handler returns
  through, `userland/sigtest.c` checks it. What is still missing is
  `sigaction`: no mask a program can set for itself, and no restarting of an
  interrupted system call.
- ~~**Memory mapping.** `sbrk` moves a break and that is the whole of it.~~
  Done for anonymous memory: `mmap` reserves a range and the pages arrive as
  they are touched, so an address space costs what it is used. Mapping a
  *file* is still not there, and is what a page cache would be built on.
- **A page cache, and swap.** Two pages of the same file mapped twice are
  two copies; nothing is ever evicted. Both need file mapping first.
- **Something to build zelr on zelr.** An assembler, then a compiler for a
  subset of C, then a linker. The threshold, and much the largest thing on
  this page. It needs the two above first, because a compiler is a program.
- A service model, so the desktop and the network come up as supervised
  things rather than calls in a row inside `kmain`.
- A packet filter, and something to look at traffic with.
- Multiple users, which needs permissions, which needs a great deal else.
- A lock finer than the one covering the whole kernel, so two processors can
  be inside it at once. The coarse one is in; what it costs is that every
  system call queues behind every other.
- TLS as more than one session, so a page's pictures can come over https in
  parallel the way they already do over http.
