# 09a -- The SDK, the libc, and the userland toolkit headers

Source root: the repository root (main, 2026-09-22, two commits after v0.37.0).
Static reading only (no toolchain on this machine). Every claim below was read in the file cited; claims marked
"(by reasoning)" are derived from the code but could not be executed. Kernel files outside this area
(`kernel/syscall.c`, `kernel/user.c`, `kernel/fd.c`, `kernel/signal.c`, `kernel/elf.c`, `kernel/fpu.c`,
`kernel/sched.c`, `kernel/timer.c`) were read in part to pin down the program ABI; they are owned by other atlas sections.

---

## 1. Scope

| File | Lines | Role |
|---|---|---|
| `sdk/README.md` | 134 | The program interface written down: entry stack, address space, flags, what is missing, syscall-number policy |
| `sdk/build.sh` | 46 | One `zig cc` invocation that builds one `.c` into a freestanding static ELF linked with `zelr.ld` |
| `sdk/hello.c` | 39 | The smallest SDK program; what `tools/sdkcheck.py` builds outside the tree |
| `sdk/zelr.h` | 983 | THE user/kernel interface: 63 syscall numbers, `syscall()` inline asm, all wrappers, structs, key/event codes, signal trampoline, `_start` |
| `sdk/zelr.ld` | 10 | Link script: `ENTRY(_start)`, image at `0x8040000000`, 4 KiB-aligned `.text/.rodata/.data/.bss` |
| `sdk/libc/README.md` | 104 | What the from-scratch C library has, lacks, and how it coexists with `zelr.h` |
| `sdk/libc/build.sh` | 53 | Same compile line plus `-nostdinc -Iinclude -I.. -DZELR_NO_SUGAR -DZELR_NO_START` and the 7 libc sources |
| `sdk/libc/hello.c` | 63 | Pure standard-C test program (`LIBC_HELLO_OK`), built by `tools/libccheck.py` |
| `sdk/libc/include/assert.h` | 14 | `assert` → `_zassert` (no include guard, per the standard) |
| `sdk/libc/include/ctype.h` | 21 | 14 ASCII classification/case functions |
| `sdk/libc/include/errno.h` | 37 | `extern int errno;` + 24 E* constants |
| `sdk/libc/include/limits.h` | 23 | Hard-coded LP64 limits |
| `sdk/libc/include/math.h` | 33 | 19 functions + `M_PI`, `M_E`, `HUGE_VAL`, `NAN`, `INFINITY` |
| `sdk/libc/include/stdarg.h` | 13 | `__builtin_va_*` |
| `sdk/libc/include/stdbool.h` | 7 | `bool`/`true`/`false` macros |
| `sdk/libc/include/stddef.h` | 19 | `size_t`/`ptrdiff_t`/`wchar_t` from compiler macros, `NULL`, `offsetof`, `max_align_t` |
| `sdk/libc/include/stdint.h` | 71 | Fixed/least/fast/ptr/max integer types and limits/`*_C` macros |
| `sdk/libc/include/stdio.h` | 66 | `FILE` (opaque `struct _zfile`), 35 functions |
| `sdk/libc/include/stdlib.h` | 48 | malloc family, exit/abort/atexit, ato*/strto*, abs/div, qsort/bsearch, rand, getenv/system |
| `sdk/libc/include/string.h` | 33 | 23 mem*/str* functions (`strerror` returns `const char *`) |
| `sdk/libc/include/time.h` | 17 | `time_t`, `clock_t`, `CLOCKS_PER_SEC 1000`, `time`, `clock` |
| `sdk/libc/src/crt0.c` | 51 | libc entry point: `_start` → `_zstart` → `_zstdio_start`, `main`, `exit` |
| `sdk/libc/src/ctype.c` | 37 | The character tests as comparisons (no table) |
| `sdk/libc/src/math.c` | 235 | Series/iteration math (see §3.5.8; `atan` is wrong) |
| `sdk/libc/src/stdio.c` | 633 | FILE over zelr descriptors + one sink-based formatter for the printf family |
| `sdk/libc/src/stdlib.c` | 435 | First-fit sbrk allocator, exit machinery, number parsing, qsort, LCG rand |
| `sdk/libc/src/string.c` | 237 | Plain string functions, word-at-a-time `memcpy`/`memset` |
| `sdk/libc/src/time.c` | 37 | `clock()` = `ticks()`, `time()` = -1 |
| `userland/alloc.h` | 216 | The allocator zelr-native programs include: segregated free lists, boundary tags, sbrk-backed |
| `userland/args.h` | 53 | `args_split` (in-place word splitting with `"` grouping), `args_join` |
| `userland/draw.h` | 231 | `surface` + clipped primitives, 8x16 bitmap text, anti-aliased `face.h` text, `mix` |
| `userland/ui.h` | 832 | Immediate-mode widget toolkit + theme loader (`/zelr.cfg`, `/sys/theme`), modern/bevel "look" |
| `userland/font.h` | 105 | Generated (tools/genfont.py) 8x16 bitmap font, ASCII 32..126, `static const` in-header copy |
| `userland/face.h` | 3327 | SKIMMED. Generated (tools/genface.py) 8-bit coverage glyphs, 6 faces |
| `tools/sdkcheck.py` | 143 | Copies 4 sdk files out of the tree, builds, puts result on a mkfat FAT16 image, boots, types its name |
| `tools/libccheck.py` | 145 | Same for `sdk/` + `sdk/libc/hello.c`; asserts 16 exact output facts |

(Line counts include blank lines; PowerShell `Measure-Object -Line` under-counts because it skips blanks.)

---

## 2. Big picture

**What this area is.** Everything a ring-3 program links against, which is nothing: zelr has no shared libraries and
no dynamic linker, so the "SDK" is a header (`sdk/zelr.h`) of `static inline` wrappers around `int $0x80`, a link script,
and a build line. A second layer, `sdk/libc/`, is a from-scratch C standard library compiled *alongside* the program
(no `libc.a`), so that portable C can be built for zelr. A third layer, the `userland/*.h` "toolkit" headers
(`alloc.h`, `draw.h`, `ui.h`, `args.h`, `font.h`, `face.h`), is what every in-tree app includes to get a heap,
pixel drawing, text rendering and a consistent widget look.

**Design decisions and their stated reasons.**

* *One header is the whole interface* (`sdk/README.md:31-33`, `zelr.h:6-9`): every call is `int $0x80` with the number
  in `rax`, "so there is nothing to link against and nothing to find at runtime". `zelr.h` carries its own license notice
  because it is the file most likely to be copied alone.
* *Numbers are the ABI and are never reused* (`sdk/README.md:123-134`, `include/syscall.h:76-80`): 44 was `SYS_GETARG`
  and is a permanent gap. `tools/abicheck.py` (run on every gate, `pipeline/gate.sh:264-265`) compares the numbers and
  5 struct shapes in `sdk/zelr.h` against `include/syscall.h` because "the two sides are written out twice and nothing
  else makes them agree".
* *Every argument is a 64-bit word* (`zelr.h:47-49, 195-197`): "an int would quietly cut the top half off a pointer".
* *System V entry* (`zelr.h:949-967`, `kernel/user.c:67-131`): the kernel places argc/argv/NULL/NULL on the new stack
  before the first instruction so a program never races to fetch its words (the old `SYS_GETARG` design needed
  interrupts off to avoid that race) and "a startup file written for another system would find what it expects".
* *The entry point is naked asm, not a function* (`zelr.h:957-961`; `kernel/user.c:16-33`): the kernel enters with
  rsp 16-aligned; a compiled function assumes rsp ≡ 8 (mod 16) after a call, and when SSE was enabled the misaligned
  frames produced GP faults on aligned vector spills (found by the allocator test).
* *Handlers need a ring-3 trampoline the program supplies* (`zelr.h:592-603`): the kernel has no code mapped in ring 3
  and will not make the stack executable, so `signal()` passes the address of `__zelr_sigreturn`.
* *Flags* (`sdk/README.md:83-103`, `sdk/build.sh:29-35`): large code model because the image is above 2 GiB; no red
  zone because the kernel writes below rsp (really: the signal frame, `kernel/signal.c:185-190`); no PIC/PIE because
  nothing relocates; `-fno-builtin` so loops are not turned into calls to a `memcpy` that does not exist.
* *Allocator lives in ring 3* (`sdk/README.md:78-81`, `userland/alloc.h:1-21`, `kernel/user.c:147-155`): the kernel only
  moves a break (`SYS_SBRK`) or reserves lazily-filled anonymous ranges (`SYS_MMAP`); "an allocator is something a
  program can write and a system call number is not", so `alloc.h` is not part of `sdk/`.
* *libc coexistence* (`zelr.h:15-31`, `sdk/libc/README.md:70-87`): `ZELR_NO_SUGAR` removes zelr.h's non-standard
  `int strlen`/`void strcpy`/`exit`/`putc`/`fread`/`fwrite`/`rename`/... spellings; `ZELR_NO_START` removes zelr.h's
  `_start` so exactly one entry point (crt0's) exists. `zelr_exit`, `zelr_putc`, `zelr_fread`, `zelr_fwrite`,
  `zelr_rename` are always present and are how the libc reaches the kernel.
* *Toolkit is immediate mode* (`ui.h:9-17`): no widget tree; a program describes the frame and gets back whether the
  user acted; the palette comes from the same `/zelr.cfg` the window manager reads, so apps follow the desktop theme.
* *Text is anti-aliased coverage bitmaps baked at build time* (`draw.h:127-135`, `tools/genface.py:956-965`): every
  program that includes `draw.h` carries all six user faces (~64 KB of coverage), "so it is short on purpose".

**Floating point in ring 3.** The kernel itself is built `-mno-sse -mno-sse2 -mno-mmx -mno-80387` (`build.sh:45`), but
user programs are **not** (`sdk/build.sh:36-44`, `userland/build.sh:27-35`, `sdk/libc/build.sh:39-51` pass no
`-mno-sse`). `kernel/fpu.c:40-66` clears CR0.EM/TS, sets CR0.MP and CR4.OSFXSR|OSXMMEXCPT on every CPU, `fninit`s;
the scheduler FXSAVEs/FXRSTORs a per-task 512-byte area on every switch (`kernel/sched.c:493, 522`); a new task gets
`fpu_blank` (x87 CW 0x037F, MXCSR 0x1F80, all exceptions masked, `kernel/fpu.c:87-110`); fork copies the parent's area
(`kernel/sched.c:281`). So ring 3 may use x87 and SSE2 freely, and the libc's doubles are real SSE2 IEEE doubles.
`userland/fptest.c` (run by `tools/ring3check.py:31`) checks this. Caveats: signal frames do not save FP state and
exec does not reset it (§10).

---

## 3. File-by-file detail

### 3.1 `sdk/zelr.h` -- the interface

**Global conventions.**

* Types (`:34-49`): `u32 u16 u8 i32 u64 i64`; `typedef _Bool bool; #define true 1 / false 0` (`:43-45`);
  `zelr_word` = `long long` (every syscall argument).
* `ZELR_STR2/ZELR_STR` (`:55-56`) stringify a SYS_ number for use inside asm (only `_start` and the trampoline).
* `syscall(n,a,b,c)` (`:198-205`): `int $0x80`, `rax`=n, `rbx`=a, `rcx`=b, `rdx`=c, result in `rax` (full 64 bits:
  `kernel/syscall.c:1096`), clobbers `"memory"` only. This is correct because the kernel saves/restores every GPR in
  `registers_t` (`include/idt.h:10-15`) and only `rax` is written back. Unknown or retired numbers (44, ≥64) return -1
  (`kernel/syscall.c:1092-1094`). Max 3 arguments.
* The syscall gate is an interrupt gate: interrupts are off during a call except where the kernel explicitly waits
  (`kernel/fd.c:192-212`).

**Syscall number table** (all verified equal to `include/syscall.h:4-181`; 64 slots, 63 live):

| # | Name | # | Name | # | Name | # | Name |
|---|---|---|---|---|---|---|---|
| 0 | EXIT | 16 | FWRITE | 32 | SPAWN | 48 | FORK |
| 1 | PUTC | 17 | SEEK | 33 | WAIT | 49 | EXEC |
| 2 | WRITE | 18 | UNLINK | 34 | KILL | 50 | GETPPID |
| 3 | GETPID | 19 | MKDIR | 35 | TASKS | 51 | DUP |
| 4 | TICKS | 20 | RMDIR | 36 | WIN_RESIZABLE | 52 | DUP2 |
| 5 | SLEEP | 21 | READDIR | 37 | WIN_RESIZE | 53 | PIPE |
| 6 | READ_FILE | 22 | STAT | 38 | CLIP_SET | 54 | SIGNAL |
| 7 | WIN_CREATE | 23 | CHDIR | 39 | CLIP_GET | 55 | SIGSEND |
| 8 | WIN_SURFACE | 24 | GETCWD | 40 | SOUND_INFO | 56 | WIN_TEXT |
| 9 | WIN_SIZE | 25 | CONNECT | 41 | SOUND_WRITE | 57 | WIN_FIND |
| 10 | WIN_POLL | 26 | SEND | 42 | POWER | 58 | SIGRETURN |
| 11 | WIN_COMMIT | 27 | RECV | 43 | SPAWN_ARGV | 59 | MMAP |
| 12 | WIN_CLOSE | 28 | DISCONNECT | 44 | *retired (GETARG)* | 60 | MUNMAP |
| 13 | OPEN | 29 | RESOLVE | 45 | TLS_CONNECT | 61 | FSYNC |
| 14 | CLOSE | 30 | NETINFO | 46 | TLS_STATUS | 62 | RENAME |
| 15 | FREAD | 31 | SYSINFO | 47 | SBRK | 63 | POLL |

Note the defines are not in numeric order in the header (45/46 at `:131-132`, 47 at `:141`, 56/57 at `:180-181`,
58-63 at `:114-128`).

**API grouped by area** (wrapper → syscall; kernel-side limits from `kernel/syscall.c` where relevant):

*Process end / console / time*
| Wrapper | Line | Syscall | Notes |
|---|---|---|---|
| `zelr_exit(int)` noreturn | 215-219 | 0 EXIT | status in rbx → `task_exit_with` (`syscall.c:86-91`); loops forever after |
| `exit(int)` [sugar] | 222-223 | 0 | calls `zelr_exit` |
| `putc(char)` [sugar], `zelr_putc(char)` | 225, 228 | 1 PUTC | writes 1 byte to **fd 1** (`syscall.c:506-509`), one syscall per char |
| `getpid()` | 229 | 3 | |
| `ticks()` | 230 | 4 | `(i32)timer_ticks()`; **100 Hz** (10 ms/tick, `kernel/main.c:459`); unit undocumented in zelr.h |
| `sleep_ms(int)` | 231 | 5 | rounded up to ≥1 tick when >0 (`kernel/sched.c:654-656`) |
| `write(const char*, int)` | 233-235 | 2 WRITE | rbx=0 (ignored); writes to **fd 1** (redirectable, `syscall.c:511-525`); len > 65536 → -1 |
| `read_file(name, buf, cap)` | 237-239 | 6 | whole-file read via VFS; name ≤ 127 chars (`FS_NAME_MAX` 128) |

*String sugar* (`:248-312`, only without `ZELR_NO_SUGAR`): `int strlen`, `strcmp`, `strncmp(int n)`, `void strcpy`,
`void strncpy(d,s,n)` (copies ≤ n-1, always terminates -- not C semantics), `memset/memcpy(…, int n)` byte loops,
`puts` (one `write`, **no newline added** -- unlike C), `putn(int)` (decimal; INT_MIN mishandled, §10),
`utoa(u32, out)` → length.

*Files and directories* (`:314-401`). Flags `O_READ 0x01, O_WRITE 0x02, O_CREATE 0x04, O_TRUNC 0x08, O_APPEND 0x10`
(= `include/vfs.h:23-27`), `SEEK_SET/CUR/END 0/1/2`. `zelr_stat {u32 size; u32 is_dir; char name[64];}` (72 bytes,
abichecked).
| Wrapper | Line | Syscall | Notes |
|---|---|---|---|
| `open(path, u32 flags)` | 335-337 | 13 | returns lowest free fd (per-process table of 16, fds 0-2 are the console); whole file read into kernel memory at open, written back at last close (`kernel/fd.c:290-326, 109-127`) |
| `close(fd)` | 338 | 14 | 0/-1; -1 also when the write-back at last close failed |
| `zelr_fread/zelr_fwrite(fd, buf, len)` | 345-350 | 15/16 | len ≤ 1 MiB per call (`syscall.c:580-592`); console read = line-edited, blocking |
| `fread/fwrite(fd, buf, len)` [sugar] | 352-359 | 15/16 | |
| `seek(fd, int off, whence)` | 360-362 | 17 | returns new pos (i32) or -1; files only; past-EOF allowed |
| `unlink/mkdir/rmdir(path)` | 364-366 | 18/19/20 | 0/-1 |
| `readdir(path, index, out)` | 369-371 | 21 | 1 entry / 0 end / -1 error |
| `stat(path, out)` | 372-374 | 22 | fills size/is_dir, name zeroed |
| `chdir(path)` / `getcwd(buf, cap)` | 375-378 | 23/24 | getcwd: 1 ≤ cap ≤ 128, returns length |
| `slurp(path, buf, cap)` | 381-393 | open+read+close | whole file into caller buffer, returns length or -1 |
| `spit(path, buf, len)` | 395-401 | open(W\|C\|T)+write+close | |
| `zelr_rename(from,to)` / `rename` [sugar] | 727-735 | 62 | same directory, 8.3 names only (`zelr.h:684-693`) |
| `fsync(fd)` | 737-739 | 61 | bytes on the drive when 0 |
| `poll(pollfd_t*, u32 n, timeout_ms)` | 723-725 | 63 | n ≤ 16; re-checks every tick; `POLLIN 1, POLLOUT 4, POLLERR 8, POLLHUP 0x10, POLLNVAL 0x20`; `pollfd_t {int fd; short events, revents;}` (`:709-719`, matches `include/fd.h:85-97`, not abichecked) |
| `dup(fd)` / `dup2(fd, to)` / `pipe(int[2])` | 630-641 | 51/52/53 | pipe buffer 4096 (`PIPE_SIZE`) |

*Network* (`:403-474`). `zelr_netinfo {u32 up, ip, gateway, netmask, dns; u8 mac[6]; u16 pad;}`.
`NET_ERR_DOWN -2, RESOLVE -3, CONNECT -4, TLS -5, BUSY -6` (`:189-193`), `NET_EOF -2` (`:460`, same value as
`NET_ERR_DOWN`; context distinguishes).
| Wrapper | Line | Syscall | Notes |
|---|---|---|---|
| `connect(host, port)` | 425-427 | 25 | socket handle (0..5, `TCP_MAX` 6 machine-wide) or NET_ERR_*; port 0 → -1; host ≤ 127 chars |
| `connect_tls(host, port)` | 443-445 | 45 | handle ≥ 0 on success (NOT "zero", §10); port 0 → 443; one secure socket machine-wide (`syscall.c:786-787`) |
| `tls_status(out, cap, which)`, `tls_why`, `tls_what` | 447-451 | 46 | `TLS_WHY 0`, `TLS_WHAT 1`; cap 1..256; machine-wide last result |
| `send(sock, buf, len)` | 452-454 | 26 | **len 1..1400 plain, 1..8192 TLS**, else -1 (`syscall.c:840`) -- undocumented in zelr.h |
| `recv(sock, buf, len)` | 462-464 | 27 | len ≤ 65536; waits up to 4 s; bytes / 0 (nothing in time) / `NET_EOF` |
| `disconnect(sock)` | 465-467 | 28 | |
| `resolve(host, u32 *out)` | 469-471 | 29 | |
| `netinfo(out)` | 472-474 | 30 | |

*Machine info / processes* (`:476-572, 745-764`). `zelr_sysinfo` 12×u32 (`cpus_found, cpus_started, mem_total_kb,
mem_used_kb, heap_total_kb, mem_free_kb, uptime_seconds, tasks, screen_w, screen_h, syscalls, disk_kb_free`).
`TASK_READY 0 … TASK_DEAD 4`. `zelr_task {u32 pid, state, slices, idle, user; char name[64];}` (84 bytes; the comment
at `:504-507` records the historical 32-vs-64 overflow).
| Wrapper | Line | Syscall | Notes |
|---|---|---|---|
| `sysinfo(out)` | 488-490 | 31 | |
| `argv_len(argv)` | 520-524 | -- | counts to NULL |
| `spawnv(path, argv)` | 528-531 | 43 SPAWN_ARGV | ≤ 64 words, ≤ 2048 bytes of strings total (`ARGV_BYTES`, `syscall.c:126-165`); child gets fresh fds 0-2 (not the parent's redirections, `kernel/sched.c:326-329`) |
| `spawn_arg(path, arg)` | 535-541 | 43 | `{path, arg, NULL}` |
| `spawn(path)` | 745-747 | 32 | argv = `{path}` |
| `fork()` | 551-553 | 48 | COW copy; child gets parent's fds, handlers, VMAs, brk, FPU state |
| `execv(path, argv)` / `exec(path, arg)` | 557-568 | 49 | keeps fds, forgets handlers/mappings/heap (`syscall.c:228-301`) |
| `getppid()` | 570-572 | 50 | |
| `wait_for(pid)` | 748-750 | 33 | exit status (128+sig when killed by a signal) |
| `kill(pid)` | 751-753 | 34 | ends any task except self (not a signal) |
| `tasks(index, out)` | 755-757 | 35 | 1/0 |
| `run_program(path)` | 760-764 | 32+33 | |

*Signals* (`:574-627`). `SIGINT 2, SIGKILL 9, SIGTERM 15`; `sighandler_t = void(*)(int)`; `SIG_DFL 0`, `SIG_IGN 1`.
* `__zelr_sigreturn` (`:604-610`): `static`, `naked, used`; body `movl $58,%eax; int $0x80; 1: jmp 1b`.
* `signal(sig, how)` (`:619-622`) → 54 with rdx = `&__zelr_sigreturn`; returns 0/-1 (not the old handler). SIGKILL
  refused (`kernel/signal.c:63`).
* `send_signal(pid, sig)` (`:625-627`) → 55.
* Delivery round trip (kernel side, `kernel/signal.c:169-258`): on return to ring 3 with a pending handled signal the
  kernel copies the whole `registers_t` to `sp = (rsp - sizeof(registers_t)) & ~15`, stores the trampoline address at
  `sp-8`, sets rsp = `sp-8`, rip = handler, rdi = sig, rax = 0. The handler's `ret` pops into the trampoline with
  rsp = `sp` (16-aligned); `SYS_SIGRETURN` checks alignment, that the pages are user pages, that saved cs/ss are ring 3
  and rip ≥ `USER_SPACE_BASE`, restores the frame with rflags masked to `0x0CD5 | 0x202`, and clears one
  `sig_running` bit. Failure ends the program with status 139.

*Memory* (`:643-672, 741-743`). `PROT_NONE 0, PROT_READ 1, PROT_WRITE 2`.
| Wrapper | Line | Syscall | Notes |
|---|---|---|---|
| `map(u64 len, prot)` | 663-665 | 59 | reserves a range in the 128 MiB window `0x8038000000-0x8040000000`, lowest fit, ≤ 16 ranges (`VMA_MAX`); pages arrive zeroed on first touch; returns 0 on failure |
| `unmap(at, len)` | 670-672 | 60 | exact range only |
| `sbrk(i64 delta)` | 741-743 | 47 | returns old break, **0 on failure** (not -1); break starts at `0x8010000000`, max `0x8038000000`; byte granular, pages mapped as crossed; negative delta lowers the break but keeps the pages (`kernel/user.c:156-194`) |

*Clipboard / sound / power* (`:766-811`): `clip_set(text, len)` (38; len ≤ 65536), `clip_get(out, cap)` (39; cap 0 →
length), `clip_len()`; `zelr_sound {u32 present, rate, channels, reserved;}`, `sound_info` (40), `sound_write(frames,
count)` (41; ≤ 4096 frames per call, blocks while the buffer drains); `power_off()`/`power_reboot()` (42 with
`POWER_OFF 0`/`POWER_REBOOT 1`; flushes disks first).

*Windows, events, theme text, find* (`:813-947`).
* Events: `WIN_EV_NONE 0, MOUSE 1, KEY 2, CLOSE 3, RESIZE 4 (x,y = new content size), SCROLL 5 (y = steps, +down),
  FIND 6 (y = match index)`; `WIN_BTN_LEFT 1, WIN_BTN_RIGHT 2, WIN_BTN_DOWN 0x80` (set on the event that starts a press).
* `win_event {u32 type; i32 x, y; u32 buttons; u32 key;}` (20 bytes; = `wm_event_t` in `include/wm.h:53-58`, not
  abichecked).
* Keys: `KEY_UP 0x100 … KEY_INSERT 0x109`, `KEY_F1 0x110` (F1..F12 consecutive), `KEY_MOD_CTRL 0x20000`,
  `KEY_CODE(k) = k & 0xFFFF`, `KEY_CTRL(k)`, `KEY_IS_SPECIAL(k) = KEY_CODE(k) >= 0x100`; ctrl+letter arrives as the
  control character (1..26) OR `KEY_MOD_CTRL` (`kernel/wm.c:3982`); `key_ctrl_letter(k)` (`:878-884`) maps back.
* `win_create(title, w, h)` (7; title ≤ 31 chars), `win_surface(h)` (8; `u32*` row-major XRGB, at
  `0x8060000000 + n*8 MiB`, stable across resize), `win_width/win_height(h)` (9; decodes `cw<<16|ch`),
  `win_set_text(h, s, len)` (56; ≤ 4096 bytes, copied), `win_find_query(out, cap)` (57), `win_poll(h, ev)` (10; 1/0),
  `win_commit(h)` (11), `win_close(h)` (12), `win_allow_resize(h)` (36), `win_resize(h, w, h)` (37).
* `RGB(r,g,b)` (`:947`) = `r<<16 | g<<8 | b`.

*Entry point* `_start` (`:969-983`, unless `ZELR_NO_START`): `naked`, section `.text._start`, global.
```
xorl %ebp,%ebp ; movq (%rsp),%rdi (argc) ; leaq 8(%rsp),%rsi (argv) ; andq $-16,%rsp
call main ; movl %eax,%ebx ; movl $0,%eax ; int $0x80 ; 1: jmp 1b
```
`envp` is not passed in rdx (a 3-argument `main` gets garbage); `main(void)` works.

### 3.2 `sdk/zelr.ld`
`ENTRY(_start)`; `. = 0x8040000000`; `.text : ALIGN(4K) { *(.text._start) *(.text*) }`, `.rodata`, `.data`,
`.bss : ALIGN(4K) { *(COMMON) *(.bss*) }`. No PHDRS, no /DISCARD/, no `.init_array` (constructors never run),
orphans such as `.eh_frame` placed by lld. The kernel loader (`kernel/elf.c:59-116`) accepts only ELF64 LE `ET_EXEC`
x86-64, entry and every PT_LOAD inside `[0x8000000000, 0x804FF00000)`, and maps every page **RW+User regardless of
PF_R/W/X** (no NX, writable text). Non-PT_LOAD segments (incl. PT_TLS) are ignored, so `_Thread_local` does not work.

### 3.3 `sdk/build.sh` -- flags and what each prevents
`cd "$(dirname "$0")"`, `SRC=$1`, `OUT=${2:-${SRC%.c}.elf}`, `CC=${CC:-$ZIG cc}`. Note: SRC/OUT are resolved relative to
the sdk directory, not the caller's cwd (libc/build.sh fixes this with `$OLDPWD`, §10).

| Flag | Why |
|---|---|
| `-target x86_64-freestanding-none` | no OS, no host libc/crt |
| `-I.` | finds `zelr.h` next to the script |
| `-ffreestanding` | no hosted assumptions (in GCC semantics it also implies `-fno-builtin`) |
| `-nostdlib` | no crt1.o, no libc, **no compiler-rt/libgcc**: helpers like `__udivti3` are unavailable |
| `-static` | no PT_INTERP, no dynamic linking |
| `-O2 -std=gnu11` | GNU asm/extensions |
| `-fno-sanitize=undefined -fno-stack-protector -fno-stack-check` | no UBSan runtime, no `__stack_chk_*` (needs TLS/libc), no stack probes |
| `-fno-builtin` | stated: stops loop→`memcpy` idiom replacement (`README.md:98-100`) |
| `-fno-pic -fno-pie` | ET_EXEC at a fixed address; loader rejects ET_DYN |
| `-mcmodel=large` | image at `0x8040000000` is beyond ±2 GiB absolute addressing |
| `-mno-red-zone` | kernel writes the signal frame directly below rsp |
| `-Wall -Wextra` | |
| `-Wl,-T,zelr.ld -Wl,--build-id=none` | layout; no build-id note |

Not passed: `-mno-sse` (SSE is intended), `-fno-asynchronous-unwind-tables` (so `.eh_frame` is emitted), `-Werror`.

### 3.4 `sdk/hello.c` and `sdk/README.md`
`hello.c` prints `hello from outside the tree.`, `  pid     <n>`, `  argv    [a] [b] …`, `  1..100  5050`, returns 0.
`README.md` documents: the program model (`:16-33`), the entry stack (`:35-61`, argc/argv/NULL/NULL/strings, rsp
16-aligned, ≤ 64 words in one page, over-long vectors refused), the address space (`:63-81`), flags (`:83-103`),
what is missing (`:105-121`: no sigaction/mask/restart/siginfo, no environment, no threads, no file mapping, no
fcntl/ioctl/non-blocking, no shared libraries) and the number policy (`:123-134`). Drift in §10.

### 3.5 `sdk/libc/`

#### 3.5.1 Build (`sdk/libc/build.sh`)
Resolves SRC/OUT against `$OLDPWD` (`:20-24`). Adds to the sdk flags: `-nostdinc` (only `include/` headers; this also
removes clang's own `float.h`, `iso646.h`, `stdalign.h`, `inttypes.h`…), `-Iinclude -I..`, `-DZELR_NO_SUGAR
-DZELR_NO_START`, links with `../zelr.ld`, compiles `src/crt0.c string.c ctype.c stdlib.c stdio.c math.c time.c` in
the same invocation. Every src file also `#define`s both macros if unset so it compiles standalone.

#### 3.5.2 Startup (`src/crt0.c`)
`_start` (`:43-51`, naked, `.text._start`): `xorl %ebp,%ebp; movq %rsp,%rdi; andq $-16,%rsp; call _zstart; 1: jmp 1b`.
`_zstart(long *sp)` (`:35-41`): `argc = sp[0]`, `argv = sp+1`, `_zstdio_start()`, `exit(main(argc, argv))`.
No constructors, no environ, no auxv parsing.

#### 3.5.3 errno / assert
`int errno;` defined in `stdlib.c:33` (plain global; one thread). Set only by: `malloc`/`calloc`/`grow` (ENOMEM),
`fopen` (EINVAL, ENOENT for *any* open failure, EMFILE), `fseek`/`ftell` on the console (ESPIPE), `system` (ENOSYS).
Never ERANGE/EDOM. `assert.h` has no guard (re-includable with NDEBUG toggled); `_zassert` (`stdlib.c:184-189`)
prints `file:line: assertion failed: expr` to stderr and `abort()`s (status 134, after flushing). It declares
`fprintf(void *, …)` and `void *stderr` locally rather than including stdio.h.

#### 3.5.4 stdio (`src/stdio.c`)
`struct _zfile` (`:33-46`): `int fd` (kernel fd, or `FD_CONSOLE` = -1), `used`, `reading`, `writing`, `eof`, `err`,
`no_close` (std streams), `unget` (-1 or one pushed-back byte), `long pos` (logical position for ftell),
`int len` (bytes in buffer), `int at` (read cursor), `unsigned char buf[FBUF=4096]`. sizeof = 4144.
`static FILE files[FOPEN_MAX=16]`, `stdin/stdout/stderr = &files[0..2]`; user FILEs use slots 3..15 (13 max).

* `_zstdio_start` (`:60-66`): all `unget=-1`; slots 0-2 used, `fd=FD_CONSOLE`, `no_close=1`.
* `raw_write` (`:70-73`): console → `write()` (SYS_WRITE → **fd 1**, for stdout *and* stderr); else `zelr_fwrite`.
* `raw_read` (`:75-78`): console → **returns 0** ("nothing reads the console yet") so **stdin is always EOF**; else
  `zelr_fread`.
* `fflush` (`:80-96`): NULL → all used; writes `len` bytes, resets buffer even on failure, `err=1` and EOF on a short
  write, `pos += sent` on success.
* `leave_read` (`:103-109`): seeks the fd back by the unread read-ahead when switching read→write.
* `fopen` (`:120-148`): mode[0] `r`→`O_READ`, `w`→`O_WRITE|O_CREATE|O_TRUNC`, `a`→`O_WRITE|O_CREATE|O_APPEND`, any `+`
  adds `O_READ|O_WRITE`; `b`/`x` ignored; append sets `pos = seek(fd,0,SEEK_END)`.
* `freopen` (`:150-153`): `fclose(f)` then `fopen` → returns a *different* FILE (std streams cannot be redirected).
* `fclose` (`:155-166`): flush; `close(fd)` result ignored; std streams stay `used`.
* Reading: `fill` (`:170-181`; flushes if writing, reads ≤ 4096, eof on ≤ 0), `fgetc` (`:183-194`), `getc`, `getchar`,
  `ungetc` (one byte), `fgets` (`:207-219`; NULL when nothing read, incl. cap 1), `fread` (`:221-244`; memcpy from
  buffer; no size*count overflow check).
* Writing: `fputc` (`:248-260`; line-flush on console `\n`, flush at `len >= FBUF` *after* storing), `putc`, `putchar`,
  `fwrite` (`:265-289`; fills buffer in chunks, flushes when full *before* filling; console flushes if the chunk has a
  `\n`), `fputs` (via fwrite), `puts` (adds `\n`).
* Position: `fseek` (`:306-320`; flush, drop buffer and unget, `seek(fd,(int)off,whence)`), `ftell` (`:322-326`;
  returns `pos`), `rewind`, `feof`, `ferror`, `clearerr`.
* `remove` = `unlink` (no directories), `rename` = `zelr_rename` (kernel limits apply), `perror` (`what: strerror\n`
  to stderr).
* Formatter (`:346-579`): `sink {FILE *f; char *mem; size_t cap; size_t count;}`; `emit` → `fputc` for a FILE or
  bounded store for memory; `count` always increments (snprintf return). Supported: flags `- 0 + space #`, width and
  precision incl. `*` (negative `*` width → left), length `hh h` (parsed, **ignored**), `l ll z j t`; conversions
  `c s d i u x X o p f F e E g G n %`; unknown → `%` + char. `%p` = `0x` + hex (NULL → `0x0`). `#` only for hex.
  Integer precision → leading zeros. `%e/%g` print fixed notation (README says so).
* `put_double` (`:387-435`): `nan`/`inf` (lowercase, width ignored); precision default 6, clamped to 17; rounding by
  adding `0.5·10^-prec` then `(unsigned long long)` for the integer part and repeated ×10 for digits; `%.0f` of 2.5 is
  `3` (libccheck asserts this). Width padding applies; zero padding goes before the sign (§10). No `space`/`#`.
* Wrappers `vfprintf, vsnprintf` (terminator at `min(count, cap-1)`), `vsprintf`/`sprintf` (cap = `SIZE_MAX`),
  `vprintf`, `printf`, `fprintf`, `snprintf`.
* Not implemented: the whole scanf family, `setvbuf/setbuf`, `tmpfile/tmpnam`, `fgetpos/fsetpos` (type `fpos_t`
  exists), `getline`, `fileno/fdopen`.

#### 3.5.5 stdlib (`src/stdlib.c`)
* Heap (`:35-155`): `block {size_t size; struct block *next; int used;}` → **24-byte header**; `ALIGN 16`,
  `CHUNK 64 KiB`, singly linked address-ordered list `heap_head`. `malloc`: `n==0→1`, overflow guard, round to 16,
  first fit + `split` (only if ≥ `want+HDR+16` left), else `grow` (sbrk ≥ 64 KiB, append, `join_forward` with the old
  tail). `free`: ignores NULL and already-free blocks (double-free "caught" silently), forward join, then a full walk
  joining every free block forward (O(n)). `calloc` overflow-checked. `realloc`: NULL→malloc, 0→free/NULL, never
  shrinks, grows by malloc+copy+free. Memory is never returned to the kernel. Payload alignment is only 8 (§10).
* Exit (`:157-189`): `atexit` (≤ 32, returns -1 when full), `exit` (atexit LIFO → `_zflush_all` → `zelr_exit`),
  `abort` (flush, status 134).
* Numbers (`:191-305`): `digit_of`, `scan_uint` (C whitespace, sign, `0x` only if a hex digit follows, base 0 octal
  detection, end = start when nothing parsed; **no overflow detection**), `strtol` (clamps only if the ull exceeds
  LLONG_MAX), `strtoul` (negation), `strtoll/strtoull` = long versions (LP64), `atoi/atol/atoll`, `strtod` (digit
  accumulation, fraction by `scale /= 10`, exponent by repeated ×/÷10; no inf/nan/hex), `atof`, `abs`, `labs`, `div`,
  `ldiv`.
* `qsort` (`:334-392`): insertion sort for n ≤ 12, else median-of-three Hoare partition with the pivot parked at
  `hi-size`, recurse into the smaller half (log depth); `bsearch` standard.
* `rand` (`:415-422`): 64-bit LCG `s = s*6364136223846793005 + 1442695040888963407`, returns `(s>>33)&0x7FFFFFFF`
  (`RAND_MAX 2^31-1`), seed 1; `srand(seed)` sets the state.
* `getenv` → NULL always; `system` → -1, `errno=ENOSYS`.
* Declared-but-missing / not declared: `llabs`, `lldiv` (though `lldiv_t` is declared), `strtof/strtold`,
  `aligned_alloc`, `_Exit/quick_exit`, multibyte functions.

#### 3.5.6 string (`src/string.c`)
`memcpy` (8-byte words when both pointers are 8-aligned, then bytes), `memmove` (forward via memcpy when `d<s`,
backward bytes otherwise -- correct), `memset` (words when dst aligned), `memcmp`, `memchr`, `strlen`, `strnlen`,
`strcpy`, `strncpy` (C semantics: pads, may not terminate), `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`
(finds the terminator), `strrchr`, `strstr` (naive), `strdup`, `strspn`, `strcspn`, `strpbrk`, `strtok` (static
`tok_rest`), `strerror` (16 messages, else `"unknown error"`, returns `const char *`). Missing: `strcoll`, `strxfrm`,
`strndup`, `strcasecmp`, `strsep`, `strerror_r`.

#### 3.5.7 ctype, time, headers
`ctype.c`: ASCII comparisons; out-of-range (incl. EOF and ≥128) answers false. `time.c`: `clock()` =
`(clock_t)ticks()` with `CLOCKS_PER_SEC 1000` (ticks are 100 Hz → 10× wrong, §10); `time()` stores/returns -1.
Headers: `stddef.h` uses `__SIZE_TYPE__/__PTRDIFF_TYPE__/__WCHAR_TYPE__`, `max_align_t = long double`; `stdint.h`
hard-codes `int64_t = long long` (clang's native is `long`), fast types 64-bit except `*fast8`; `limits.h` hard-codes
LP64 values; `math.h` `NAN`/`INFINITY` are float builtins; `isnan`/`isinf` are functions, not macros.

#### 3.5.8 math (`src/math.c`)
| Function | Method | Notes |
|---|---|---|
| `fabs, trunc, floor, ceil, round` | via `(long long)` cast; `trunc` returns x when `|x| ≥ 2^53` | `round` is floor(x+0.5) (fails at 0.49999999999999994); signed zeros lost |
| `fmod(a,b)` | `a - trunc(a/b)*b` | NaN for b=0/inf a; inaccurate for large quotients |
| `sqrt` | exponent halved by bit surgery, 5 Newton steps on the mantissa, odd exponent × √2 | subnormals wrong |
| `exp` | `k = round(x/ln2)`, 17-term Taylor on r, exponent-field add | flushes results < ~2^-1022 to 0 (no subnormals); x > 709.78 → HUGE_VAL |
| `log` | mantissa in [√½, √2], atanh series in `(m-1)/(m+1)` (16 terms) | subnormals wrong |
| `log2, log10` | `log(x)/const` | |
| `pow` | integer exponent with `|e|<1024` → square-and-multiply (exact); else `exp(e*log(b))`; negative base with non-integer e → NaN | `pow(-2, 2000)` → NaN (should be inf); `pow(1, NaN)` → NaN (C says 1) |
| `sin, cos` | reduce by `round(x/(π/2))`, 8-term Taylor on `[-π/4, π/4]`, quadrant switch | large |x| loses precision (admitted) |
| `tan` | `sin/cos`, `HUGE_VAL` when cos == 0 | |
| `atan` | fold to [0,1], 40-term "Euler" series | **wrong** (§10): uses `z²` where Euler needs `x²/(1+x²)`; `atan(1) ≈ 0.6046` |
| `atan2` | quadrant fix-up of `atan(y/x)` | inherits the atan error |
| `isnan, isinf` | builtins | |
Missing: `asin acos sinh cosh tanh hypot cbrt exp2 expm1 log1p ldexp frexp modf fmin fmax copysign nan rint lround`
and all `float`/`long double` variants; no `float.h`.

### 3.6 `userland/alloc.h` -- the zelr-native allocator
* Constants: `AL_ALIGN 16`, `AL_CHUNK 64 KiB`, `AL_BINS 16`, `AL_FOOT 16` (8 meaningful bytes; 16 keeps every block
  and payload 16-aligned -- an 8-byte footer caused GP faults on aligned SSE moves, `:51-63`).
* `al_block {u64 size /*payload bytes | AL_INUSE(1)*/; al_block *next, *prev /*bin list while free*/; u64 pad;}` →
  **`AL_HDR` = 32**. Layout: `[32-byte header][payload, multiple of 16][16-byte footer (size|inuse)]`.
* Globals (all `static` in the header, so one allocator per translation unit): `al_bins[16]`, `al_top` (one past the
  heap), `al_bottom` (first sbrk result), `al_live` (payload bytes in use).
* `al_bin_of(n)`: `n>>=5`, then count halvings, capped at 15 → bin 0: <32, bin b: [2^(b+4), 2^(b+5)), bin 15: ≥ 512 KiB.
* `al_unlink/al_link` (doubly-linked bin lists; `al_link` writes the footer).
* `al_grow(need)`: `max(need+48, 64 KiB)` rounded to 4 KiB, `sbrk`, one free block ending at the new `al_top`; does
  not merge with a free tail block (merging happens lazily in `free`).
* `al_split(b, want)`: splits when ≥ `want + 32 + 16 + 16` remains.
* `malloc(u64 n)`: `n==0 → 0`; round to 16; scan bins from `al_bin_of(want)` upward, first block ≥ want; else
  `al_grow` and recurse. No overflow guard (§10).
* `free(p)`: NULL/already-free ignored; merge forward if the next block (inside `al_top`) is free; merge backward via
  the previous block's footer if `b > al_bottom` and the computed head is ≥ `al_bottom`; relink.
* `calloc(u64 n)` -- **one argument** (total bytes), zeroes with zelr.h's `memset(…, int)`; `realloc(p, n)` never
  shrinks, copies `have` bytes; `heap_live()`, `heap_size()` (= `al_top - al_bottom`).
* Requires contiguous sbrk results: two allocators (or a direct `sbrk` caller) sharing the break corrupt each other
  (`zelr.h:542-545`).
* Users: `alloctest.c, cowtest.c, fdtest.c, forktest.c, jpegtest.c, jsprobe.c, jstest.c, layouttest.c, pagetest.c,
  pngtest.c, sh.c, svgtest.c, wiretest.c` and headers `fetch.h, jpeg.h, js.h, png.h, svg.h` (hence the browser).

### 3.7 `userland/args.h`
* `args_split(char *line, char **out, int max)` (`:19-39`): skips spaces/tabs; a word starting with `"` runs to the
  next `"` (quotes removed, may contain spaces; unterminated runs to end; `""` yields an empty word); other words end
  at space/tab; terminators are written into `line`; returns the count (≤ max). Quotes are recognised only at word
  start (`ab"c d"` → `ab"c`, `d"`). No escapes, no single quotes (deliberate, `:15-17`).
* `args_join(words, n, out, cap)` (`:45-53`): joins with single spaces, truncates at `cap-1`, always writes `out[len]`
  (cap must be ≥ 1); does not re-quote.
* Only user: `userland/sh.c:125` (`args_split`); `args_join` is unused.

### 3.8 `userland/draw.h`
* `surface {u32 *px; int w, h;}` (stride = w).
* Primitives (all clip to the surface): `fill`, `rect(x,y,w,h,c)`, `frame` (1-px outline), `round_rect(…, r, c)` (rows
  inset along a circle, no AA), `disc(cx,cy,r,c)`, `line` (Bresenham, per-pixel clip), `mix(under, over, alpha/255)`
  (integer lerp, truncating).
* Bitmap text (font.h): `glyph(s,x,y,ch,c)` (bit `0x80>>col`, ASCII 32..126), `text` (8-px advance),
  `text_centred` (uses zelr.h's `int strlen`). Now used only by `settings.c` and `css.h`.
* Face text (face.h, `:127-231`): `UI_FACE_SMALL 0 (13px)`, `BODY 1 (15)`, `HEAD 2 (20)`, `BOLD 3 (15 bold)`,
  `MONO 4 (15 mono)`, `MONOB 5 (15 bold mono)`; `MONO_W` = advance of `' '` in face 4, `MONO_H` = size+3;
  `face_of(which)` (out-of-range → 0), `face_h(which)` = pixel size, `face_w(str, which)` = sum of advances,
  `face_draw_clip(s, x, y, x0, x1, str, fg, which)`: baseline = `y + size*4/5` (y is the top of the line), glyph
  pixels at `baseline - top + gy`, alpha 255 → fg, else `mix`; clipped to `[x0,x1)` and the surface; characters
  outside 32..126 draw as space; stops once the pen passes x1. `face_draw` (clip = whole width), `mono_char`,
  `face_centred`.

### 3.9 `userland/ui.h` -- the widget toolkit
**Metrics** (`:25-31`): `UI_PAD 8, UI_GAP 6, UI_ROW 26, UI_BTN_H 30, UI_TITLE_H 28, UI_RADIUS 6, UI_SCROLL_W 10`;
`UI_MENUBAR_H 20` (`:734`); `UI_KEYS 16` (`:357`); `UI_PRESETS 6` (`:201`).

**`ui_theme`** (`:33-70`): `bg` (window ground), `panel` (toolbars/sidebars), `fg`, `dim`, `accent`, `accent_fg`,
`soft` (low-strength accent for a selected row), `line` (borders/separators), `warn`; the four bevel edges
`edge_hi` (outer top-left), `edge_light` (inner top-left), `edge_shadow` (inner bottom-right), `edge_dark` (outer
bottom-right); `well` (inside sunk things); `int modern`; `stroke` (hairline replacing a bevel); `raised` (one layer up
from bg).

**Theme loading** `ui_load_theme()` (`:233-342`):
1. `slurp("/zelr.cfg")` into 1024 bytes; `ui_cfg_int(text, key, fallback)` (`:207-231`) finds `key` at a line start
   followed by one space; decimal or `0x` hex; no negatives.
2. `preset` (default 1, invalid → 0), `light` (default 1), `look` (default 0). **look 0 = modern, non-zero = bevelled
   ("built")**: `t.modern = !look`. Defaults match the kernel (`kernel/theme.c:336-338`: modern, light, preset 1).
3. `accent` from the file, else `UI_ACCENTS[preset]` (teal 2CC7A0, indigo 6E8AE8, amber E0A03C, rose E06A8C, slate
   8A9BB0, lime 9AD14A -- same order/values as `kernel/theme.c:25-32`); then overridden by `accent` in `/sys/theme`
   (192-byte buffer) when present, so hand-set accents follow the window manager.
4. Modern palettes (`:273-306`): light bg F4F4F7/panel FAFAFC/fg 17181C/dim 5D606A/line DDDDE3/stroke D2D3DA/raised &
   well FFFFFF; dark bg 22242B/panel 2A2D35/fg E6E8EA/dim 9AA0A8/line 353841/stroke 3C404A/raised 2F323B/well 1B1D23;
   `accent_fg` white; edges set harmlessly (`hi=light=raised`, `shadow=dark=stroke`); `soft = mix(bg, accent, 40 light /
   48 dark)`; `warn E06C60`.
5. Bevel palettes (`:308-341`): light bg=panel D6D3CD, fg 121214, dim 5C5A57, line 8E8B86, edges FFFFFF/E8E6E1/868480/
   3C3B39, well FFFFFF; dark bg=panel 2A2D33, fg E6E8EA, dim 9AA0A8, line 181A1E, edges 5A5F68/3C4048/1C1E22/0C0D10, well
   1A1C20; `stroke = raised = 0`.
In practice callers reload the theme every frame (`calc.c:220`, `blackjack.c:533`, `files.c:373`, …), i.e. two file
reads per frame.

**Light & shape helpers**: `ui_bevel(s,x,y,w,h, tl_out, tl_in, br_in, br_out)` (2-px bevel; top row owns the corner),
`ui_raised` (hi, light, shadow, dark), `ui_sunken` (dark, shadow, light, hi), `ui_groove` (shadow line over hi line),
`ui_corner_cover` (4×4 supersampled corner coverage 0..255), `ui_round(…, r, c, alpha)` (anti-aliased rounded rect,
optional translucency), `ui_round_outline(…, r, fill, line)` (hairline = shape minus 1-px-smaller shape).

**Input** `ui_input {int mx, my, down, pressed, released, right_pressed; u32 key; u32 keys[16]; int nkeys; int scroll;}`;
`ui_begin` clears per-frame fields; `ui_feed(in, ev)` folds one `win_event` (left edge detection via `down`;
`right_pressed` whenever an event has the right button set; first key in `key`, up to 16 in `keys[]`; scroll sums
`ev.y`); `ui_hit(in, x,y,w,h)`. Widgets consume `released`/`right_pressed` when they act, so overlapping widgets
cannot both claim one click.

**Widgets** (each returns on the frame the button is *released* inside it):
| Widget | Line | Returns / notes | Modern branch? |
|---|---|---|---|
| `ui_button(s,in,t,x,y,w,label)` | 407-440 | 1 on click; held = bevel flips + label moves 1 px; modern = rounded card, accent outline on hover/press | yes |
| `ui_button_primary` | 443-473 | accent-filled; bevel mode adds an inner "default button" ring | yes |
| `ui_row(…, label, right, selected)` | 477-507 | 1 click, 2 right-click; selected = `soft` fill + 3-px accent bar; right-aligned secondary text | no |
| `ui_label`, `ui_dim_label` | 509-517 | BODY face in fg/dim | -- |
| `ui_section(…, w, title)` | 520-525 | BOLD title + rule; returns next y (`y + 28 + 6`) | no |
| `ui_toggle(…, label, value)` | 527-540 | 40×20 pill; returns new value | no |
| `ui_slider(…, w, value, lo, hi)` | 543-566 | returns new value while dragged over it (hit zone y-6..y+24) | no |
| `ui_scrollbar(s,t,x,y,h, first, shown, total)` | 570-579 | draws only if total > shown; thumb ≥ 20 px | no |
| `ui_field` / `ui_field_key(f, key)` / `ui_field_draw(…, f, placeholder)` | 585-681 | caller owns `buf/cap/len/cursor/focused`; keys: `\b`, `KEY_DELETE`, `LEFT/RIGHT/HOME/END`, printable insert; draw scrolls so the caret (blinks every 30 ticks = 300 ms) stays visible; returns 1 on the click frame | yes |
| `ui_well(…, &ix,&iy,&iw,&ih)` | 687-699 | sunk/rounded content area, returns the 2-px-inset interior | yes |
| `ui_toolbar(s,t,w,h)`, `ui_toolbar_gap` | 704-722 | panel strip with line (modern) or groove | yes |
| `ui_menubar(s,in,t,w,titles,count,open,x_out)` | 736-761 | draws titles, open one in accent; returns hovered index or -1; program owns which is open | no (uses `ui_raised`) |
| `ui_statusbar(s,t,w,h,left,right)` | 763-798 | bottom `UI_ROW` strip; bevel mode = two sunk panels | yes |
| `ui_menu(s,in,t,x,y,w,items,count)` | 805-832 | popup clamped into the surface, 3-step darkened "shadow", returns chosen index on release | no |
All text is drawn with `face.h` (BODY 15px; BOLD for `ui_section`); the bitmap font is not used by `ui.h`.
Users: `blackjack.c, calc.c, browser.c, files.c, monitor.c, music.c, notes.c, poker.c, settings.c`, `cards.h`.

### 3.10 `userland/font.h` and `userland/face.h`
* `font.h`: generated by `tools/genfont.py` (which writes `kernel/font.c` and `userland/font.h`); `FONT_W 8, FONT_H 16,
  FONT_FIRST 32, FONT_LAST 126`; `static const unsigned char font8x16[95][16]` (1520 bytes; each byte one row, MSB =
  leftmost). The kernel uses `include/font.h` (`extern` declaration) with data in `kernel/font.c`.
* `face.h` (skimmed): generated by `tools/genface.py` (`emit("userland/face.h", …, FACES_USER)`, letterforms drawn in
  that script). `face_glyph {short w, h /*bitmap*/, left, top /*vs pen*/, advance; unsigned int at /*offset into
  pixels*/;}`; `FACE_FIRST 32, FACE_LAST 126, FACE_COUNT 95, FACE_SIZES 6`; per face a coverage blob (one byte per
  pixel, 0 = background, 255 = solid) and a 95-entry glyph table: `face_px_13[8506]/face_g_13`, `face_px_15[9870]`,
  `face_px_20[15698]`, `face_px_15b[10202]`, `face_px_15m[9925]`, `face_px_15bm[10187]` (≈ 64.4 KB coverage);
  `face_t {short size; short bold, mono; const face_glyph *glyphs; const unsigned char *pixels;}`;
  `face_faces[6]` = 13, 15, 20, 15b, 15m, 15bm (`:3320-3327`). The kernel's `include/face.h` has **8** faces
  (13, 15, 20, 26, 15b, 20b, 15m, 15bm), so face indices are not portable between kernel and userland.

### 3.11 `tools/sdkcheck.py` and `tools/libccheck.py`
Both: `build_once()` (runs `bash build.sh` at the repo root unless `ZELR_PREBUILT=1`, `harness.py:817-825`),
`tempfile.mkdtemp`, then:
* **Build outside the tree.** sdkcheck copies exactly `zelr.h, zelr.ld, build.sh, hello.c` (`:37, 45-57`) and runs
  `bash build.sh hello.c outside`; libccheck copies the whole `sdk/` tree (`:37-49`) and runs
  `bash build.sh hello.c foreign` in `sdk/libc`.
* **Write a FAT image.** `python tools/mkfat.py <ROOT>/sdkcheck.<pid>.img 32768 <program>:home/outside` (libccheck:
  `libccheck.<pid>.img`, `home/foreign`) → a 32 MiB unpartitioned FAT16 volume written by the host-side
  spec implementation (`tools/mkfat.py:11`), directories created as needed.
* **Boot.** `Guest(image, memory=256, reuse=True)` (`harness.py:602-646`): `qemu -kernel build/zelr.bin -m 256
  -no-reboot -display none -serial stdio -drive file=IMG,format=raw,if=ide,index=0 -monitor tcp:… -append console`;
  `reuse=True` keeps the mkfat image instead of truncating a blank disk. `wait_boot()` waits for the first shell
  prompt (`zelr> ` / `zelr:/home> `; the shell starts in `/home`).
* **Type and read.** `vm.fresh(line)` types one char every 50 ms, waits for the next prompt, returns only that
  command's output.
* sdkcheck asserts (7): build succeeded; `4096 < size < 200 KiB`; image written; `ls /home` lists `outside`;
  `outside alpha beta` prints `hello from outside the tree`; output has `  pid` and `  1..100  5050`; output has
  `[outside] [alpha] [beta]`.
* libccheck asserts (16): build; `8192 < size < 600 KiB`; image; `foreign one two` prints `LIBC_HELLO_OK` (else stop);
  `hello from a program with a libc`; `argv[0] = foreign`, `argv[1] = one`, `argv[2] = two`;
  `THE QUICK BROWN FOX (19 characters)`; `sorted: 1 3 7 19 42 56 88`; `7     7 7    | 00007 +7`;
  `beef BEEF 10 z text 0x1234`; `3.142 3    -1.50`; `strtol ok`; `sqrt(2) = 1.414214`; `pow(2,10) = 1024`;
  `sin(pi/6) = 0.500000`; both `read back:` lines.
* Cleanup: `vm.stop()`, `rmtree(work)`, remove image unless `--keep` (but see §10: `Guest.stop` already deletes it).
* Gate: both run only in `screen`/`full` mode (`pipeline/gate.sh:405, 552-558, 593-594`); default mode is `fast`.

---

## 4. Control flow and lifecycles

**Build → run (SDK program).** `bash sdk/build.sh prog.c` → ELF at `0x8040000000` → copied to any VFS path → shell
finds it by name → `SYS_SPAWN_ARGV`/fork+`SYS_EXEC` → kernel: `vfs_slurp` → `elf_load` (maps PT_LOADs RW+U, zeroes
BSS) → `user_build_stack` (16 pages at `0x804FFF0000..0x8050000000`, strings in the top page with argv[0] highest,
then `argc, argv[0..argc-1], NULL, NULL` with rsp 16-aligned; >64 words or overflow → refuse) →
`task_create_user` (all GPRs 0, rflags 0x202, rip = e_entry, FPU blank, fds 0-2 = console, cwd inherited) →
`_start` → `main(argc, argv)` → `SYS_EXIT(status)`.

**Entry stack layout** (top page, growing down):
```
0x8050000000  ── top of stack
              argv[0] string (highest) … argv[argc-1] string
              0-15 bytes of zero padding (alignment)
              NULL            (end of an empty environment)
              NULL            (end of argv)
              argv[argc-1] … argv[0] pointers
rsp  ─────►   argc            (16-byte aligned)
```
No auxv (not even AT_NULL). Stack grows on demand by page faults down to `0x804FF00000` (1 MiB).

**libc program lifecycle.** `_start` (crt0) → `_zstart(sp)` → `_zstdio_start()` → `main` → `exit(rc)` → atexit
handlers LIFO → `fflush(NULL)` → `SYS_EXIT`. Files left open are written back by the kernel when the process's fd
table is released; libc buffers are only flushed by exit/abort/fflush/fclose (a kill loses them).

**FILE state machine.** `{idle} --fgetc/fread→fill--> {reading: len=bytes read, at cursor}`;
`{reading} --fputc/fwrite→leave_read (seek back len-at)--> {writing: len pending}`;
`{writing} --fill (from fgetc/fread only when at>=len)→fflush--> {reading}`; `fseek` → `{idle}` after flush and buffer
drop; console FILEs flush on `\n` and at 4096.

**Allocator (alloc.h) lifecycle.** first `malloc` → `al_grow` (sbrk ≥ 64 KiB) → block in bin → split on allocation →
`free` coalesces with neighbours via header/footer → `al_link` into a bin. The heap never shrinks.

**Immediate-mode frame (ui.h).** Typical loop (`calc.c:215-229`): get size/surface → `ui_load_theme()` →
`ui_begin(&in)` → `while (win_poll) ui_feed` → draw widgets (each reads `in`, consumes clicks) → `win_commit`.

**Signal handler lifecycle.** `signal(SIGTERM, h)` records `h` and the trampoline → `send_signal` sets pending →
on the next return to ring 3 (syscall exit or timer tick) the kernel builds the frame and enters `h(sig)` → `ret` to
`__zelr_sigreturn` → `SYS_SIGRETURN` restores the interrupted frame. A second same signal waits in `sig_pending`
until the first handler returns. fork inherits handlers; exec forgets them.

---

## 5. Interfaces

**Exports.**
* `sdk/zelr.h`: everything in §3.1. Included by every `userland/*.c` (via `-I../sdk`, `userland/build.sh:28`), by
  `draw.h`, `alloc.h`, `args.h`, `face.h`, and by the libc sources (with `ZELR_NO_SUGAR`/`ZELR_NO_START`).
* `sdk/zelr.ld`: used by `sdk/build.sh`, `sdk/libc/build.sh`, `userland/build.sh:34`.
* libc: the standard names in §3.5 plus internal `_zstdio_start`, `_zflush_all`, `_zassert`, `_zstart`.
* `alloc.h`: `malloc, free, calloc(u64), realloc, heap_live, heap_size`.
* `draw.h`: `surface, fill, rect, frame, round_rect, disc, line, glyph, text, text_centred, mix, UI_FACE_*, MONO_W,
  MONO_H, face_of, face_h, face_w, face_draw_clip, face_draw, mono_char, face_centred`.
* `ui.h`: §3.9. `args.h`: `args_split, args_join`.

**Dependencies.**
* Kernel syscall table `kernel/syscall.c:1021-1085`; process/stack builder `kernel/user.c`; fd layer `kernel/fd.c`;
  signals `kernel/signal.c`; ELF loader `kernel/elf.c`; FPU `kernel/fpu.c`; window server `include/winsrv.h`.
* `/zelr.cfg` (written by `kernel/theme.c:498-549`: header, then `look`, `light`, `preset` or the colour keys, then the
  knobs) and `/sys/theme` (`kernel/sysfs.c:339-351`: `accent desktop surface text text_dim light look preset`).
* Generators: `tools/genfont.py` (font.h), `tools/genface.py` (face.h). Checkers: `tools/abicheck.py`,
  `tools/sdkcheck.py`, `tools/libccheck.py`, `tools/ring3check.py`, `tools/mkfat.py`, `tools/harness.py`.

---

## 6. Concurrency, locking, memory ownership, invariants

* **No threads in ring 3.** Every piece of state here is unsynchronised by design: libc `errno`, `files[]`,
  `heap_head`, `tok_rest`, `rand_state`, `at_exit_fns`; alloc.h `al_*`. Signal handlers are the only asynchronous
  entry: calling `malloc`/`printf`/`strtok` from a handler that interrupted the same function corrupts state (nothing
  is async-signal-safe; not documented).
* **Header-static state.** alloc.h/draw.h/ui.h globals are `static` in headers: one copy per translation unit. All
  in-tree programs are single-file, so this holds; a multi-file program must include alloc.h in exactly one TU.
* **One break owner.** Invariant: sbrk results are contiguous and only one allocator moves the break (alloc.h's
  footer walk and the libc's `join_forward`/`grow` depend on it).
* **alloc.h invariants:** blocks tile `[al_bottom, al_top)`; every free block is in exactly the bin of its current
  size; footer == header size word; payloads 16-aligned (block starts 16-aligned because sbrk starts page-aligned
  and all sizes are multiples of 16).
* **libc heap invariants:** list in address order; `used` flag accurate; payload = block + 24 (8-aligned only).
* **Kernel memory copies.** Every struct result is copied whole by the kernel (`sizeof` its own struct) into the
  caller's buffer -- the reason abicheck exists. Strings passed in are copied (paths ≤ 127 chars). `win_set_text`
  copies; `win_surface` memory is owned by the kernel and remapped on resize at the same address.
* **Ownership across fork/exec.** fork: COW address space, shared open-file descriptions (shared positions), copied
  handlers/VMAs/brk/FPU. exec: new address space and stack, heap/VMAs/handlers reset, fds kept, FPU state kept.
  spawn: fresh fds 0-2.
* **FILE buffers** are owned by the libc; data reaches the kernel on flush and the disk only when the kernel's last
  descriptor closes (or `fsync`, which the libc does not expose).

---

## 7. Limits and magic numbers

| Value | Where | Meaning |
|---|---|---|
| `0x8000000000` | `include/paging.h:62` | `USER_SPACE_BASE` (PML4 entry 1) |
| `0x8010000000` | `include/user.h:38` | heap base (`USER_HEAP_BASE`) |
| `0x8038000000` | `include/user.h:39, 62` | heap max / mmap window start |
| `0x8040000000` | `zelr.ld:5`, `include/user.h:63` | image link address / mmap window end |
| `0x804FF00000` | `kernel/elf.c:43` | `USER_LOAD_MAX` (image + stack-growth boundary) |
| `0x8050000000` | `include/user.h:109` | `USER_STACK_TOP` |
| 16 pages / 1 MiB | `kernel/user.c:49`, `include/user.h:104` | initial stack / max stack |
| `0x8060000000`, 8 MiB step | `include/winsrv.h:19-20` | window surfaces |
| 64 words, 2048 bytes | `include/user.h:25`, `kernel/syscall.c:126` | argv limits (spawnv/execv) |
| 16 | `include/fd.h:41, 91`, `include/sched.h:8` | fds per process, poll entries, VMAs |
| 65536 / 1 MiB | `kernel/syscall.c:522, 582` | `write` / `fread`,`fwrite` per call |
| 1400 / 8192 | `kernel/syscall.c:840` | `send` per call (plain / TLS) |
| 65536, 4 s | `kernel/syscall.c:856-869` | `recv` per call, wait |
| 6 | `include/tcp.h:17` | sockets machine-wide; 1 TLS |
| 128 / 64 | `include/fat.h:4,9` | path / name max (libc `FILENAME_MAX` says 256) |
| 100 Hz | `kernel/main.c:459` | `ticks()` rate |
| 4096 | `stdio.c:31`, `stdio.h:13-14` | `FBUF`/`BUFSIZ`; `FOPEN_MAX` 16 (13 user) |
| 24 / 16 / 64 KiB | `stdlib.c:43-45` | libc block header / rounding / sbrk chunk |
| 32 | `stdlib.c:159` | `ATEXIT_MAX` |
| 12 | `stdlib.c:340` | qsort insertion-sort cutoff |
| 6364136223846793005, 1442695040888963407 | `stdlib.c:418` | rand LCG constants |
| 17 | `stdio.c:404` | max `%f` precision |
| 134 / 139 | `stdlib.c:181`, `kernel/syscall.c:404` | abort status / bad sigreturn status |
| 32 / 16 / 16 / 64 KiB / 16 bins | `alloc.h:28-64` | alloc.h header / footer / align / chunk / bins |
| 13, 15, 20, 15b, 15m, 15bm | `face.h:3320-3327` | user faces 0..5 |
| 1024 / 192 | `ui.h:234, 265` | theme file buffers |
| 30 ticks | `ui.h:675` | caret blink (300 ms) |
| 20 | `ui.h:734` | menu bar height |

---

## 8. Tests

| Test | Covers | What it checks |
|---|---|---|
| `tools/sdkcheck.py` (gate screen/full) | zelr.h + zelr.ld + build.sh sufficiency, entry stack, argv, exit | §3.11 (7 checks) |
| `tools/libccheck.py` (gate screen/full) | libc build outside tree, printf padding/bases/doubles, qsort, strtol, strdup/toupper/strlen, sqrt/pow/sin, fopen/fprintf/fgets/remove | §3.11 (16 checks) |
| `tools/abicheck.py` (every gate) | syscall names/numbers both sides, no duplicate numbers; shapes of `zelr_task, zelr_stat, zelr_sysinfo, zelr_netinfo, zelr_sound` | not checked: `win_event`, `pollfd_t`, O_*/POLL*/KEY_*/WIN_EV_*/NET_ERR_* constants |
| `tools/ring3check.py` → `fptest` | SSE/x87 in ring 3, preserved across switches | 1/3, 0.1+0.2, Newton roots, 1000 reciprocals, sign, double width |
| → `alloctest` | alloc.h | empty start, basic alloc/free, double free, malloc(0), 16-byte alignment incl. split blocks, churn, large blocks grow the heap, realloc keeps contents |
| → `argvtest` | entry stack via zelr.h `_start`, spawnv/execv | argv[0], NULL terminator, `argv_len == argc`, words with spaces, fork+execv and spawnv vectors, >page vector refused |
| → `sigtest` | signal trampoline | handler runs with the right number, one-at-a-time, SIGKILL uncatchable, SIG_IGN, integer registers survive a timer-delivered signal, fork inherits, exec forgets (FP registers not checked) |
| → `maptest`, `polltest`, `fdtest`, `forktest`, `cowtest`, `durtest`, `sleeptest`, `faulttest` | map/unmap, poll, descriptors, fork/exec, COW, fsync/rename, sleep, faults | per their ok() lists |
| `tools/shell_test.sh` (`wintest`) | window surface mapping | surface at `0x0000008060000000`, 3072 pixels writable, other program's window unreachable |
| Screenshot harnesses (`tools/shotcheck.py`, `setcheck.py`, `browsercheck.py`, …) | ui.h colours in practice | accent colours such as TEAL/INDIGO appear |
| Kernel selftest | -- | none of this area (ring 3 only) |

Untested here: stdin/stderr behaviour, `atan/atan2`, `strtod`, `fseek(SEEK_CUR)`/`ftell` while buffered, `clock()`,
libc malloc alignment and `realloc`, `strtok`, printf edge cases (`%hd`, `%#x` with padding, zero-padded negative
`%f`), ui.h keyboard handling with ctrl.

---

## 9. How to extend

* **A new syscall.** Pick the next unused number (never 44, never reuse), add `#define SYS_X n` to both
  `include/syscall.h` and `sdk/zelr.h`, a handler in `kernel/syscall.c` `TABLE`, and a `static inline` wrapper in
  zelr.h (pass every pointer/size as `zelr_word`; max 3 args in rbx/rcx/rdx). If a struct crosses the boundary, declare
  it on both sides and add the pair to `PAIRS` in `tools/abicheck.py`. Update the counts in `zelr.h:11`,
  `sdk/README.md:31`, `README.md:1304, 1542`. If the name collides with a C standard name, put the short name under
  `#ifndef ZELR_NO_SUGAR` and provide a `zelr_` spelling.
* **A new libc function.** Implement in the relevant `src/*.c`, declare in `include/*.h`; a new source file must be
  added to the list in `sdk/libc/build.sh:50-51`. Reach the kernel only through zelr.h names that survive
  `ZELR_NO_SUGAR`. Add an exact-output assertion to `sdk/libc/hello.c` + `tools/libccheck.py`.
* **Cheap, high-value libc fixes** (see §10 for evidence): give `stdin/stdout/stderr` real fds 0/1/2
  (`raw_read` → `zelr_fread(0,…)`, stdout → `zelr_fwrite(1,…)`, stderr → `zelr_fwrite(2,…)`); `clock()` →
  `ticks()*10` (or read the rate from `/sys/uptime` "at N Hz"); `atan` → `z2 = x*z` (i.e. `x²/(1+x²)`); flush in
  `fputc` *before* storing when `len == FBUF`; call `leave_read` (and account for `unget`) in `fseek` before
  `SEEK_CUR`; `ftell` = `pos + (writing ? len : 0)`; pad the libc block header to 32 bytes for 16-byte payloads; check
  `close()` in `fclose`; `time()` could parse `/sys/time` (`now YYYY-MM-DD HH:MM:SS`, local CMOS time, no timezone).
* **A new widget.** Follow the immediate-mode pattern in ui.h: take `(surface*, ui_input*, const ui_theme*, geometry,
  caller-owned state)`, draw both a `t->modern` branch (rounded, `stroke`/`raised`) and a bevel branch
  (`ui_raised`/`ui_sunken`), use `face_*` with `UI_FACE_BODY`, return an action on `over && in->released` and clear
  `in->released` when consumed.
* **Theme keys.** Add the key to `kernel/theme.c` `KNOBS` (writer/parser/settings follow) and read it with
  `ui_cfg_int`; keys near the end of `/zelr.cfg` risk falling outside ui.h's 1024-byte buffer.
* **Fonts.** Change sizes in `tools/genface.py` (`FACES_USER`) and regenerate; keep `UI_FACE_*` indices in `draw.h` in
  step with `face_faces[]` order; do not hand-edit `face.h`/`font.h`.
* **Pitfalls the comments warn about.** Two allocators on one break (`zelr.h:542-545`); a struct that grows on the
  kernel side overflows ring 3 (`zelr.h:504-507`, `tools/abicheck.py:7-16`); an 8-byte alloc.h footer misaligns SSE
  (`alloc.h:51-63`); the entry point must be naked and align itself (`kernel/user.c:16-33`); `recv` 0 vs `NET_EOF`
  (`zelr.h:455-460`); `-fno-builtin` loop→memcpy (`sdk/README.md:98-100`).
* **Pitfalls not written down** (by reasoning): zelr.h's `memcpy`/`memset` are `static inline`, so if clang emits a
  real `memcpy` call for a large aggregate copy, a zelr.h-only program fails to link; `-nostdlib` means no
  compiler-rt (`__int128` division etc.); `_Thread_local` is not supported (PT_TLS ignored, no FS base); a signal
  handler must not use floating point or large struct copies (FP state is not saved); do not include `<stdbool.h>`
  before `zelr.h`.

---

## 10. Doc drift and suspicious code

### 10.1 Drift (verified)
1. **Syscall count disagrees in four places.** `zelr.h:11` "forty-seven"; `README.md:1304` "fifty-seven";
   `sdk/README.md:31` and `README.md:1542` "sixty-three". Actual: 64 slots, 63 live (`include/syscall.h`,
   `kernel/syscall.c:1021-1085`).
2. `zelr.h:171-175` (and `include/syscall.h:131-135`, `kernel/syscall.c:336-339`): "three signals and no handlers"  -- 
   handlers exist (`zelr.h:592-622`, `kernel/signal.c:132-258`).
3. `zelr.h:429-431` `connect_tls`: "Zero means the connection is open" -- it returns the socket handle
   (`kernel/syscall.c:808`), which is only 0 for the first slot.
4. `zelr.h:645-647` (and `include/user.h:56-57`, `include/syscall.h:148-150`): "asking for a gigabyte … costs a page"  -- 
   the mmap window is 128 MiB (`include/user.h:62-63`); `user_mmap` returns 0 for len > 128 MiB (`kernel/user.c:235`).
5. Misplaced comments in zelr.h: the sbrk description sits above `fork` (`:542-545`, sbrk is at `:741`); the fsync and
   rename descriptions sit above the poll section (`:674-693`, functions at `:727-739`).
6. `sdk/README.md:70` "the top of the stack, sixteen pages of it" -- 16 pages are pre-mapped but the stack grows on
   demand to 1 MiB (`include/user.h:88-104`, `kernel/user.c:306-319`). (`kernel/user.c:35-48` has the same stale claim.)
7. `sdk/README.md:65-76`: the address table omits the mmap window (`0x8038000000-0x8040000000`) and "nothing above the
   stack exists" is false -- window surfaces are mapped at `0x8060000000+` (`include/winsrv.h:19-20`).
8. `sdk/README.md:78` "sbrk is the only call underneath an allocator" -- `map()`/`SYS_MMAP` also exists.
9. `sdk/README.md:37-48`, `zelr.h:951-955`, `kernel/user.c:86-88` "the way System V says": there is **no auxiliary
   vector** (`kernel/user.c:119-127` writes argc, argv, NULL, NULL only; above it are 0-15 padding bytes and the
   strings). A foreign crt0 that walks auxv past envp reads padding/string bytes as auxv entries.
10. `sdk/README.md:90-91`, `sdk/build.sh:31-32`: the red-zone reason "an interrupt can arrive … and the kernel does not
    respect the 128 bytes below rsp" -- a ring-3 interrupt switches to the kernel stack (RSP0) and never touches the user
    stack; what writes below rsp is signal delivery (`kernel/signal.c:185-190`). The flag is still required.
11. `stdio.c:55-57` "0/1/2 are not descriptors this machine hands out" and `:76` "nothing reads the console yet" -- every
    process starts with fds 0/1/2 on the console (`kernel/fd.c:158-164`), console reads are line-edited and blocking
    (`kernel/fd.c:214-266`), pipes/redirections exist. Consequences: **libc `stdin` is always EOF** (even in a pipeline)
    and **`stderr` goes to fd 1** via `SYS_WRITE` (`kernel/syscall.c:519-525`), so `prog > file` captures error output.
12. `time.c:26-27` "Milliseconds since this machine started" -- `ticks()` is 100 Hz (`kernel/main.c:459`;
    `userland/polltest.c:40` multiplies by 10), so `clock()/CLOCKS_PER_SEC` is **10× too small**.
13. `time.c:6-9`, `sdk/libc/README.md:40-43`, `time.h:4-7`: "the kernel … has no idea what year it is" -- the kernel reads
    the CMOS RTC (`include/rtc.h`) and publishes `/sys/time` (`kernel/sysfs.c:275-283`); only a syscall/timezone is missing.
14. `sdk/libc/README.md:56-60`, `math.c:12-15` "accurate to about twelve significant figures" -- false for `atan`/`atan2`
    (10.2 #1).
15. `stdlib.c:410-414` "The constants are the ones Park and Miller published" -- 6364136223846793005/1442695040888963407
    are Knuth's MMIX LCG constants; Park–Miller "minimal standard" is 16807 mod 2^31−1.
16. `stdlib.c:260-262` strtod "exact for everything a program is likely to have written down" -- `strtod("0.3")` computes
    `0 + 3*0.1 = 0.30000000000000004`, not the double nearest 0.3 (by reasoning, IEEE double arithmetic).
17. `stdio.c:465-466` "h and hh are read and ignored … the cast below is what narrows it" -- no narrowing cast exists;
    `%hhd` of 300 prints `300`, `%hu` of 70000 prints `70000`.
18. `stdio.c:6-9` "a FILE is in one mode or the other at a time: switching flushes" -- write→read without `fflush`
    returns the unflushed output bytes as input, because `fgetc`/`fread` only refill when `at >= len`
    (`stdio.c:191, 234`). (UB for the caller per the C standard, but contradicts the comment.)
19. `alloc.h:25-27` "the header is already sixteen wide" and `:44` "pad; to sixteen" -- `sizeof(al_block)` is 32.
20. `args.h:6-8` "six programs need it" -- only `userland/sh.c:125` uses `args_split`; `args_join` is unused.
21. `ui.h:23-24` "The numbers are multiples of 4" -- `UI_GAP 6`, `UI_ROW 26`, `UI_BTN_H 30`, `UI_RADIUS 6`,
    `UI_SCROLL_W 10` are not.
22. `ui.h:183` "Read once at startup" -- callers call `ui_load_theme()` every frame (`calc.c:220`, `blackjack.c:533`,
    `files.c:373`, `monitor.c:203`, `music.c:339`, `poker.c:663`, `settings.c:777`, `browser.c:1542`): two file reads per
    frame.
23. `sdk/libc/README.md:21` "the types, from the compiler rather than guessed" -- only `size_t/ptrdiff_t/wchar_t/
    intptr_t/uintptr_t` are; `stdint.h` fixed types and all of `limits.h` are hard-coded.
24. Top-level `README.md` does not describe `sdk/libc` or `tools/libccheck.py`; `README.md:1303-1305` still says "Four
    files in `sdk/`"; `README.md:75, 1768` "No libc" now reads oddly next to a shipped libc.
25. `sdkcheck.py`/`libccheck.py` `--keep`: `Guest(image, …, reuse=True)` has `keep=False`, and `Guest.stop()` deletes a
    non-kept disk (`tools/harness.py:766-770`), so the image is gone before the script's own `--keep` logic runs.
26. `kernel/syscall.c:775-778` says TLS session state is single while `:811-814` says "there is a session per
    connection" (kernel-side contradiction; the single-TLS limit is enforced at `:786-787`, matching `zelr.h:439-442`).

### 10.2 Suspected bugs (verified by reading; consequences by reasoning)
1. **`atan` is mathematically wrong** -- `math.c:215-223`. The loop computes `Σ (2i)!!/(2i+1)!! · z^(2i+1)` with
   `z = x/(1+x²)`, `z2 = z*z`. Euler's series needs the ratio `x²/(1+x²)` (= `x*z`), not `z²`. The code's sum equals
   `asin(z)/sqrt(1-z²)`: `atan(1)` → 0.6046 (true 0.7854), `atan(0.5)` → 0.4490 (true 0.4636); agrees with atan only to
   O(x⁵) (`x - x³/3 - 7x⁵/15` vs `+x⁵/5`). `atan2` inherits it. Fix: `double z2 = x * z;`.
2. **`fputc` buffer overflow** -- `stdio.c:253`. `fwrite` can leave `len == FBUF` (`:274-284` flushes only when a chunk
   finds the buffer already full), then `fputc` stores `buf[4096]` before flushing. `files[]` is contiguous and
   `buf` ends the struct (sizeof 4144), so the byte lands on the next FILE's `fd` low byte (e.g. stdout overflow turns
   stderr's fd from -1 into 0xFFFFFF78 → all stderr output then fails). Trigger: exactly 4096 buffered bytes via
   `fwrite`/`fputs` (no `\n` on the console), then `fputc`/`putchar`/`printf`.
3. **`fseek(f, off, SEEK_CUR)` while reading** -- `stdio.c:306-320` discards the buffer without seeking back, so the
   offset is applied to the kernel position, which is ahead by the read-ahead (`len-at`) and by a pending `ungetc`.
   `fseek(f, 0, SEEK_CUR)` after reading 10 bytes of a 100-byte file lands at 100.
4. **`ftell` ignores pending writes** -- `stdio.c:322-326` returns `pos`, which only advances in `fflush` (`:94`);
   `fopen("w"); fputs("hello"); ftell()` → 0.
5. **`freopen` cannot redirect a standard stream** -- `stdio.c:150-153` returns a new slot; `stdout` stays the console.
6. **`fclose` hides write failures** -- `stdio.c:158` ignores `close()`, but the kernel writes a file to disk only at
   the last close and reports failure there (`kernel/fd.c:116-121`, `kernel/syscall.c:576-578`).
7. **libc `malloc` returns 8-byte-aligned memory** -- header is 24 bytes (`stdlib.c:37-43`), payload = block+24; blocks
   from `grow` start 16-aligned (heap base page-aligned, chunks multiples of 16), so their payloads are ≡ 8 (mod 16).
   `max_align_t` is `long double` (16) and SSE is enabled in ring 3, so aligned vector moves on such memory (e.g.
   16-aligned struct types) can #GP -- the exact failure alloc.h's comment describes.
8. **`grow()` returns the wrong block when the break is not contiguous** -- `stdlib.c:94-95`: if the old tail is free but
   not adjacent, `join_forward` does nothing and `last->used ? b : last` returns the small old tail; `malloc` then fails
   with ENOMEM and the new chunk is leaked. Only when something else moved the break.
9. **`strtol` family: no overflow handling** -- `stdlib.c:219-227` wraps past 2^64, never sets ERANGE; clamping only
   works in the `(LLONG_MAX, ULLONG_MAX]` window.
10. **`strtod` exponent loop** -- `stdlib.c:294-295`: `e` iterations of ×/÷10 (`"1e2000000000"` runs ~2·10⁹ iterations;
    longer exponents overflow `int e`, UB).
11. **`system(NULL)` returns -1** (`stdlib.c:431-435`), i.e. non-zero = "a command processor is available".
12. **`%f` for |v| ≥ 2^64** -- `stdio.c:412` `(unsigned long long)v` is out of range; with clang's usual lowering
    `printf("%f", 1e20)` prints `0.000000` (by reasoning).
13. **`%0Nf` of a negative number pads before the sign** (`stdio.c:431-432`: `000-1.50`); `%#010x`/`%#.8x` put zeros
    before `0x` (`stdio.c:517-521, 556-576`); `%.0d` of 0 prints `0`; `%L` is not parsed (desynchronises arguments);
    `% f` and `%#o` ignored; precision > 17 silently clamped.
14. **`putn(INT_MIN)`** -- `zelr.h:296` `v = -v` overflows; digits come out as characters below `'0'`.
15. **`ui_field_key` inserts ctrl chords** -- `ui.h:616-617` tests `key < 32` on the whole value, but windows receive
    `KEY_CODE | KEY_MOD_CTRL` (`kernel/wm.c:3982`), so ctrl+A (0x20001) passes and inserts byte 0x01. Raw keys are
    passed by `files.c:399, 405`, `notes.c:272`, `browser.c:1435, 1453`. Similarly ctrl+H is not treated as `'\b'`.
16. **`right_pressed` is not an edge** -- `ui.h:387` sets it on every mouse event while the right button is held
    (`WIN_BTN_DOWN` is ignored), so `ui_row` returns 2 on each frame of a right-button drag.
17. **alloc.h `malloc` has no overflow guard** -- `alloc.h:137`: `n` within 15 of 2^64 rounds to `want = 0` and returns
    any small block; `n ≥ 2^63` makes `al_grow` pass a negative delta to `sbrk`, which the kernel treats as a give-back
    and answers with the old break (`kernel/user.c:167-175`); `al_grow` then writes a footer ~2^63 bytes away (fault)
    after having lowered the break.
18. **Signal handlers clobber FP/SSE state** -- the signal frame is `registers_t` only (`kernel/signal.c:201-213`);
    FXSAVE/FXRSTOR happen only on task switch (`kernel/sched.c:493, 522`). Since programs are built with SSE enabled and
    clang uses xmm registers for doubles and aggregate copies, a handler can corrupt the interrupted code's FP/vector
    registers and MXCSR. `sigtest.c` only checks integer registers.
19. **Nested different signals** -- `kernel/signal.c:255-256` clears the *lowest* set `sig_running` bit on return, not
    the returning handler's, so a SIGINT handler interrupted by a SIGTERM handler can be re-entered after SIGTERM's
    returns (weakens `zelr.h:617-618`).
20. **`<stdbool.h>` before `zelr.h` does not compile** -- `zelr.h:43` `typedef _Bool bool;` becomes `typedef _Bool _Bool;`
    once `bool` is a macro; `sdk/libc/README.md:84-87` recommends including zelr.h *after* the standard headers.
21. **exec keeps the FPU state** -- `kernel/syscall.c:290-296` resets GPRs but not the FXSAVE area (no `fpu_blank`); an
    exec'd program inherits MXCSR/x87 control words.
22. **`sdk/build.sh` resolves paths against the sdk directory** (`:16-20`), unlike `sdk/libc/build.sh:20-24`;
    `bash sdk/build.sh prog.c` from elsewhere looks for `sdk/prog.c`.
23. Minor: alloc.h `calloc(u64)` is non-standard (one argument); libc `FILENAME_MAX 256` vs kernel path max 128;
    `remove()` cannot remove directories; `rewind` does not clear the error flag; `fgets(buf, 1, f)` returns NULL;
    `args_join` writes `out[0]` when `cap == 0`; math edge cases in §3.5.8 (subnormals, `pow(-2,2000)`, signed zeros).

---

## 11. Open questions

1. Does zig/clang with these flags ever emit a real `memcpy`/`memset` call for large aggregate copies in a
   zelr.h-only program? If so it fails to link (zelr.h's versions are `static inline`); no userland file defines an
   external `memcpy`. Needs a build to confirm.
2. Does `-ffreestanding` already imply `-fno-builtin` in zig cc (it does in GCC's documented semantics)? If so, the
   README's "`-fno-builtin` is the one that bites" is redundant rather than wrong.
3. How does lld lay out orphan sections (`.eh_frame`, `.comment`) under `zelr.ld`, and into which PT_LOAD? Harmless
   today because every page is mapped RW+U, but relevant if NX/read-only text is ever added to `kernel/elf.c`.
4. Should the libc adopt alloc.h's allocator (16-aligned, O(1)-ish bins) instead of its own first-fit list, or should
   both be unified in `sdk/`? The README deliberately keeps alloc.h out of `sdk/`.
5. Does the userland shell (`userland/sh.c`) support `2>` redirection? If so, the libc's stderr-on-fd-1 behaviour is
   observable in pipelines today.
6. Is the missing auxv intentional (the README only promises the empty environment's terminator)? Porting a real crt0
   (musl, newlib) would need at least an `AT_NULL` pair after envp.
7. Was `ticks()` ever 1000 Hz (which would explain `CLOCKS_PER_SEC 1000` and the "milliseconds" comment)? The kernel
   has run the PIT at 100 Hz as far as this tree shows (`kernel/timer.c:14, 59-61`).
