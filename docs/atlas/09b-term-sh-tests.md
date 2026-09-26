# 09b: the terminal, /bin/sh, the coreutils and the ring 3 test programs

Source: the repository root (github.com/blavese/zelr main, 2026-09-22, two commits after v0.37.0).
This was a static reading only. Nothing was built or run. Every claim below has a file:line reference, and anything I could not check without running the machine is marked as such.

---

## 1. Scope

Line counts are real line counts, blank lines included.

| File | Lines | Role |
|---|---|---|
| `userland/term.c` | 1875 | The desktop terminal (`/bin/term`). A windowed ring 3 program with a 400-line scrollback, a line editor, history in `/cfg/history`, tab completion, 38 built-in commands, themes in `/cfg/term`, mouse selection and the clipboard, find-text publishing, and a program launcher (spawnv + wait_for). |
| `userland/sh.c` | 340 | `/bin/sh`, a console shell built on fork/dup2/execv/pipe/wait_for. Supports `|`, `<`, `>`, `>>`, a trailing `&` and double quotes. Built-ins: cd, pwd, exit, help, jobs. |
| `userland/cat.c` | 50 | `cat [FILE...]`. With no operands it copies fd 0 to fd 1. |
| `userland/count.c` | 20 | Demo and test target. Prints 5 "tick" lines 700 ms apart (about 3.5 s), then exits 0. |
| `userland/echo.c` | 20 | `echo WORDS...`, joined by single spaces. |
| `userland/grep.c` | 92 | `grep WHAT [FILE...]`. Plain substring match; reads stdin when given no files. Exit 0 found, 1 none, 2 usage. |
| `userland/hello.c` | 15 | Oldest demo program. Prints its pid and 1..100 summed (5050). |
| `userland/ls.c` | 55 | `ls [DIR...]`, one name per line, `/` after directories. |
| `userland/ps.c` | 44 | Task table: PID, STATE, SLICES, NAME. |
| `userland/wc.c` | 74 | `wc [FILE...]` prints lines, words and bytes, plus a total line for more than one file. |
| `userland/spin.c` | 24 | An endless busy loop that never sleeps. The target for the ctrl-C test. |
| `userland/alloctest.c` | 185 | Ring 3 allocator (alloc.h) stress test. Marker `ALLOCTEST_PASS`/`_FAIL`. |
| `userland/argvtest.c` | 140 | argv/argc shape across fork+execv and spawnv, plus refusal of an oversize vector. `ARGVTEST_*`. |
| `userland/cowtest.c` | 120 | Copy-on-write fork, measured through `sysinfo.mem_free_kb`. `COWTEST_*`. |
| `userland/durtest.c` | 132 | fsync and rename semantics, including the refusals. `DURTEST_*`. |
| `userland/faulttest.c` | 126 | Ring 3 faults end the program (status 139) and the machine survives 100 of them without leaking. `FAULTTEST_*`. |
| `userland/fdtest.c` | 209 | Descriptor inheritance, per-process numbering, dup2 redirection, pipes, a two-child pipeline, bad fds. `FDTEST_*`. |
| `userland/forktest.c` | 162 | fork memory separation, getppid, several children, heap after fork, exec success and failure. `FORKTEST_*`. |
| `userland/fptest.c` | 103 | SSE doubles in ring 3, checked against exact scaled results. `FPTEST_*`. |
| `userland/halfdrawn.c` | 60 | Window that paints red without committing, then green or mint and commits. Checked by tearcheck.py. |
| `userland/maptest.c` | 145 | Anonymous mmap: on-demand cost, zeroed pages, bounds, inheritance, unmap, slot limit. `MAPTEST_*`. |
| `userland/polltest.c` | 148 | poll() on pipes and files: timing, POLLNVAL, POLLHUP, the 16-entry limit. `POLLTEST_*`. |
| `userland/sigtest.c` | 185 | Signal handlers: delivery, no re-entry, SIGKILL rules, SIG_IGN, register preservation, fork and exec rules. `SIGTEST_*`. |
| `userland/sleeptest.c` | 124 | sleep_ms measured against ticks(). `SLEEPTEST_*`. |
| `userland/spawntest.c` | 40 | spawn, wait_for and tasks(). Prints `spawntest: ok`. No harness runs it. |
| `userland/wintest.c` | 48 | Window server basics from ring 3: create, size, surface mapping, commit, close, foreign handle. Run by `tools/shell_test.sh`. |
| `tools/termcheck.py` | 150 | Types into the desktop terminal through QEMU `sendkey` and judges each step by the background colour of the theme it selected. |
| `tools/progcheck.py` | 98 | Runs a program by bare name from a non-/bin location. Note: it drives the **kernel** console shell, not term.c. |
| `tools/ring3check.py` | 107 | Runs 19 self-checking ring 3 suites with `exec /bin/NAME` at the kernel shell and looks for their `*_PASS` markers. |

Consulted for context but documented elsewhere: `tools/shcheck.py` (234, the only harness for sh and the coreutils), `tools/shell_test.sh` (124, runs wintest), `tools/tearcheck.py` (104, runs halfdrawn), `tools/clipcheck.py` (125, terminal copy and paste), `tools/deskcheck.py`, `tools/findcheck.py`, `tools/appcheck.py`, `tools/shotcheck.py`, `tools/harness.py`, `userland/args.h` (54), `userland/draw.h` (232), `userland/face.h` (generated), `userland/alloc.h`, `sdk/zelr.h`, `kernel/builtin.S`, `kernel/builtin.c`, `kernel/shell.c`, `kernel/fd.c`, `kernel/keyboard.c`, `kernel/signal.c`, `kernel/syscall.c`, `kernel/wm.c`, `kernel/winsrv.c`, `kernel/layout.c` and `pipeline/gate.sh`.

---

## 2. Big picture

### 2.1 What these programs are

All of them are ordinary freestanding ELF64 executables. `userland/build.sh` compiles each `*.c` with `zig cc -target x86_64-freestanding-none -O2 -I../sdk -T ../sdk/zelr.ld`, with SSE left **on** (unlike the kernel). They are pasted into the kernel image by `kernel/builtin.S` (`.incbin "build/user/NAME.elf"`) and registered under `/bin/NAME` by `kernel/builtin.c:70-117` through `vfs_add_builtin`. They are never copied to the disk (`builtin.c:7-9`). Of the 48 slots allowed by `SYSFS_MAX_PROGRAMS` (`include/sysfs.h:30`), 46 are used, which matches the boot log's "progs 46 built in". A `_Static_assert` at `builtin.c:126-128` enforces the limit. The only interface they share with the kernel is `sdk/zelr.h` (int 0x80 wrappers, structs and constants), plus header-only helpers in `userland/` (`draw.h`, `face.h`, `args.h`, `alloc.h`).

**The terminal (`/bin/term`)** is the window the desktop opens with. `kernel/shell.c:217-231` (`enter_desktop`) runs `vfs_slurp("/bin/term")` and `user_spawn_elf` before `wm_run()`. It can also be started from the context menu (`wm.c:375`), the launcher (`wm.c:394`), the desktop icon (`wm.c:873`) and the default taskbar pin (`pins.c:20`).

The header says it is "deliberately not a terminal emulator. There is no pty and no escape sequence parser; a line carries its own colour" (`term.c:10-12`). It is a self-contained interpreter. The 38 built-in commands run inside the terminal process and print into its own scrollback. Anything else on the command line is looked up as a program (cwd, then `/usb`, then `/bin`), started with `spawnv`, and waited for with `wait_for`.

**The terminal does not capture a child program's output.** Spawned tasks get a fresh fd table whose fds 0, 1 and 2 are the kernel console (`kernel/sched.c:191-194`, `kernel/fd.c:158-164`). `puts` and `putc` write to fd 1 (`syscall.c:506-525`), which for the console means `kputc`: the serial line plus fbcon or VGA (`printf.c:16-24`, `fd.c:268-272`). So `hello` typed in the terminal prints on the serial console and the kernel text console, not in the terminal window. The terminal only reports "pid N finished, T ticks" (`term.c:929-934`). The same applies to the coreutils, and it is one reason they are only really usable from `/bin/sh` on the console.

**/bin/sh** is a separate, classic Unix-style shell meant for the kernel console. The harness starts it with `exec /bin/sh` at the kernel shell prompt (`tools/shcheck.py:85`). It reads lines from fd 0 (the kernel console's line discipline, `fd.c:214-266`). A pipeline is `pipe()`, `fork()` and `dup2()` on each side, then `execv("/bin/NAME", argv)`. It ignores SIGINT itself and restores `SIG_DFL` in each child before exec, so that ctrl-C kills the job and not the shell (`sh.c:239-255`, `sh.c:313-317`).

**The coreutils** (cat, echo, grep, ls, ps, wc) exist "to make an arrangement observable" (`tools/shcheck.py:16-19`). Each one reads fd 0 when it gets no operands and writes fd 1. **count, spin and hello** are demo and test targets.

**The ring 3 test programs** each print a line per check (`PASS`/`FAIL` or `ok`/`FAIL`) and end with a unique marker `NAME_PASS` or `NAME_FAIL`. The reason is given at `ring3check.py:12-15`: looking for the marker rather than counting PASS lines means a suite that dies halfway through fails instead of passing with fewer checks. The kernel self-test cannot reach any of this ring 3 behaviour (`ring3check.py:5-7`, `gate.sh:296-301`).

### 2.2 How input and output flow

- **Keys to the terminal.** PS/2 or USB go into the kernel keyboard ring (`keyboard.c:50-60`). Serial bytes are pulled in by `kbd_trygetchar` when that ring is empty (`keyboard.c:187-199`, which also turns CR into LF). While the desktop runs, `wm_run` in the kernel shell task drains `kbd_trygetchar` (`wm.c:3952`). Escape leaves the desktop (`wm.c:3953-3956`); desktop shortcuts, the find bar and the launcher field come next. Everything else goes to the top non-minimised window as `WM_EV_KEY`, carrying `KEY_CODE | ctrl bit` (`wm.c:3968-3988`). The terminal reads these with `win_poll`. So the test tools can type into the terminal either over the serial line (`vm.type`, used by tearcheck, findcheck and appcheck) or at the PS/2 keyboard (`sendkey`, used by termcheck and clipcheck).
- **Terminal output.** It goes into its own surface via `face_draw` in the 15 px monospaced face, then `win_commit`, then the compositor. It also publishes the text of its last 60 lines through `win_set_text`, so the desktop's ctrl+F can search it (`term.c:298-316`).
- **Program stdout.** It goes to the kernel console, never to the terminal window (see 2.1).
- **Program stdin.** It is the kernel console. A program that reads fd 0 while the desktop is up competes with `wm_run` for the same `kbd_trygetchar` (`fd.c:226` against `wm.c:3952`).

### 2.3 Design decisions and the reasons the code gives

- **No escape parser; a colour slot per line.** "Six colours is the whole palette ... Everything printed picks one of them, which is why there is no escape sequence parser here" (`term.c:37-42`). A line's colour is stored as a slot index (`C_FG`, `C_DIM`, `C_ACCENT`, `C_WARN`) rather than an RGB value, "so switching palette recolours everything already on screen rather than only what comes next" (`term.c:70-72`).
- **One command table.** It drives dispatch, help and tab completion: "Adding a command here is the whole job of adding a command" (`term.c:430-434`, `term.c:1301-1304`).
- **Program search order: cwd, then `/usb`, then `/bin`.** "A program somebody has just downloaded or copied is the one they mean. /bin is last, so a name that exists in both runs the one in front of you rather than the one that shipped, which is the way round that makes a program replaceable" (`term.c:1389-1400`). The kernel shell does the same (`kernel/shell.c:245-290`). A name that contains a slash is used as a path and not searched (`term.c:1408-1416`).
- **The input line scrolls horizontally, half a screen at a time.** "Because a window that moves on every keystroke makes the text slide about under what is being typed" (`term.c:361-364`). The line is not cut at the window width. A long browser URL used to lose everything past column 80, which is why (`term.c:1665-1672`). It is still hard-capped at `COLS` = 160.
- **The selection is stored as (line, col), not pixels**, "because the window can be resized and the view scrolled underneath a selection, and both of those move the pixels without moving the text" (`term.c:99-105`). Copying strips trailing spaces (`term.c:239-241`).
- **Copy with nothing selected copies the input line.** "The other thing somebody reaches for copy to get" (`term.c:270-271`). Paste stops at the first newline, because "a newline in the middle would submit half of it" (`term.c:288-290`).
- **Find publishes only the recent lines**, "because find answers about what can be seen and what has scrolled away cannot" (`term.c:298-305`).
- **Themes.** An explicit `theme NAME` is saved in `/cfg/term` and always wins. Otherwise the terminal follows the desktop's `light` key in `/zelr.cfg` (light gives paper, dark gives slate). With no settings file at all it is slate, "because that is what a terminal is" (`term.c:1196-1205`).
- **History outlives the window.** It is kept in `/cfg/history` and "rewritten whole each time, which at sixty lines is nothing" (`term.c:1475-1476`). MAX_HIST is actually 64.
- **Resizable.** "The scrollback is stored as text, so a new size is only a different number of rows" (`term.c:1794-1796`). Lines are in fact pre-wrapped at print time; see section 10.
- **`get` defaults to https** "for the same reason the browser does it" (`term.c:1105-1108`). It uses HTTP/1.0 because "1.1 lets a server chunk it, and nothing here puts chunks back together" (`term.c:1136-1137`).
- **sh built-ins must run in the shell process.** "A `cd` that ran as a child would change the child's directory and then the child would exit" (`sh.c:144-148`). They are only recognised alone: "`cd x | wc` would run the cd in a child and change nothing" (`sh.c:333-334`).
- **sh sends its own errors to fd 2**, "a shell whose complaints went down the pipe would feed them to the next program" (`sh.c:52-53`).
- **sh restores SIGINT in each child.** An ignored signal stays ignored across exec, so without the reset "`spin` could not be interrupted and the shell sat waiting for it, which is the machine lost" (`sh.c:244-255`, found by measurement).
- **sh closes its copies of the pipe ends.** "A reader waits for the last writer to go, and a shell that kept its copy open would be that writer forever." It also resets them to -1 so that it never double-closes a number that has since been reused (`sh.c:280-286`).
- **ls prints one name per line** so that `ls | wc` counts files (`ls.c:3-5`). **grep matches plain text**, since there is no regex engine and "pretending otherwise ... would be worse" (`grep.c:3-6`). **cat with no operands** "is what turns a pipe into something a person can see" (`cat.c:3-6`).

---

## 3. File-by-file detail

### 3.1 `userland/term.c` (1875 lines)

Includes `zelr.h` and `draw.h` (`term.c:17-18`). It does not include `ui.h`: this is the one program that does not use the desktop's palette (`term.c:1198-1201`).

#### 3.1.1 Constants (`term.c:20-35`)

| Name | Value | Meaning |
|---|---|---|
| `COLS` | 160 | Maximum columns per stored line, per input line and per history entry |
| `MAX_LINES` | 400 | Scrollback ring size |
| `PAD` | 8 | Pixel margin around the text grid |
| `MAX_HIST` | 64 | History entries kept |
| `VFS_PATH` | 128 | Path buffers ("what the kernel will accept"). Matches `FAT_PATH_MAX` 128. |
| `VFS_NAME` | 32 | Name buffers for completion. **Not** what the kernel accepts: `VFS_NAME_MAX` is `FAT_NAME_MAX` = 64 (`include/fat.h:9`). See section 10. |
| `IO_BUF` | 16384 | Shared file and network buffer `io[]` (`term.c:600`). Reads cap at 16383 bytes. |
| `HISTORY_FILE` | `"/cfg/history"` | History, one entry per line |
| `THEME_FILE` | `"/cfg/term"` | Palette name, written without a newline |

The grid comes from the face, not from a constant (`draw.h:145-157`). `MONO_W` is the advance of ' ' in `face_faces[UI_FACE_MONO]`, which is **9 px** (`face.h:2690`, 15 px regular monospaced, `face.h:3325`). `MONO_H` is `size + 3` = **18 px**. With the default 760×480 window, `fit_to_window` (`term.c:1774-1780`) gives cols = (760-16)/9 = **82** and rows = (480-16)/18 = **25**: 24 scrollback rows plus the prompt row. cols is clamped to 8..160 and rows to at least 2. The face is derived from the proportional face and condensed or centred into a 620-em cell "so that column n is under column n on the line above, which is the only thing a terminal actually needs". It replaced the 8×16 bitmap font (`tools/genface.py:959-1012`).

#### 3.1.2 Palettes (`term.c:44-66`)

`typedef struct { const char *name; u32 bg, fg, dim, accent, warn, cursor; } palette;`. Five entries are stored in `PALETTES[]`, and `pal` holds a copy of the active one (`term.c:68`).

| name | bg | fg | dim | accent | warn | cursor |
|---|---|---|---|---|---|---|
| slate (index 0) | 10141A | C8D2DA | 6B7A87 | 5ED1A0 | E07A6A | 5ED1A0 |
| paper (index 1) | F2EEE4 | 2A2824 | 8A8478 | 1F6F8B | B03A2E | 1F6F8B |
| amber | 140E04 | FFB030 | 8A5E18 | FFE080 | FF6040 | FFB030 |
| phosphor | 020A02 | 40E050 | 1E7028 | A0FFB0 | FF7050 | 40E050 |
| ink | 0A0C18 | CFD4EE | 636C92 | 9A86E8 | E87B9B | 9A86E8 |

The slots are `C_FG` 0, `C_DIM` 1, `C_ACCENT` 2 and `C_WARN` 3 (`term.c:73-76`); `colour_of(slot)` maps them (`term.c:78-85`). The test tools rely on the bg values: termcheck, clipcheck and shotcheck look for SLATE 10141A, PAPER F2EEE4, AMBER 140E04 and PHOSPHOR 020A02.

#### 3.1.3 Global state

- Scrollback: `char lines[400][161]`, `u8 slots[400]`, `n_lines` (lines in use), `first` (ring index of the oldest line), `view` (lines scrolled back from the end; 0 is live) (`term.c:89-93`).
- Window: `surface scr` (px, w, h), `int win` (handle), `rows`, `cols` (`term.c:95-97`).
- Selection: `sel_active`, `sel_dragging`, `sel_l0/sel_c0` (anchor), `sel_l1/sel_c1` (moving end). Line numbers are logical scrollback indices, 0 being the oldest (`term.c:106-109`). `sel_range` orders the ends (`term.c:112-118`); `sel_clear` resets (`term.c:120`).
- Input line: `char input[161]`, `in_off` (first visible column), `in_len`, `in_pos` (cursor), `blink` (frame counter) (`term.c:123-126`).
- History: `char hist[64][161]` (newest last), `n_hist`, `hist_at` (0 means the live line; k means k entries back), `hist_stash[161]` (the live line set aside while browsing) (`term.c:130-133`).
- Line builder: `char work[161]`, `work_n` (`term.c:181-182`).
- `static char io[16384]` (`term.c:600`) is shared by `read_all`, `get`, `history_save` and `history_load`.
- Completion candidates: `char cand[32][32]`, `n_cand` (`term.c:1509-1510`).

#### 3.1.4 The scrollback and printing

- `line_at(i)` and `slot_at(i)` index logically: `(first + i) % MAX_LINES` (`term.c:135-136`).
- `push_line(s, slot)` appends. Once the ring is full it overwrites the oldest line and advances `first` (`term.c:138-149`).
- `print(s, slot)` splits at `\n`, wraps at the **current** `cols`, drops `\r`, turns other control and non-ASCII bytes into `.`, and expands tabs. The tab arithmetic is off by one (section 10). Lines are stored already wrapped (`term.c:152-173`).
- `say`, `dim`, `good` and `err` are `print` with `C_FG`, `C_DIM`, `C_ACCENT` and `C_WARN` (`term.c:175-178`).
- Line builder: `w_reset`, `w_str`, `w_ch`, `w_num(u32)`, `w_pad(col)` and `w_rnum(u32, width)` for right-aligned numbers. All of them clip at `COLS` (`term.c:184-205`). `need(usage)` prints `usage: ...` in warn colour (`term.c:460`).

#### 3.1.5 Rendering (`draw_all`, `term.c:318-400`)

1. Fill the surface with `pal.bg`, then `publish_text()`.
2. The visible range is `end = n_lines - view` and `start = end - (rows-1)`, both clamped at 0. Each line is drawn at `y = PAD + k*MONO_H` in its slot colour. If a selection covers the line, the covered part first gets a `mix(bg, accent, 120)` rectangle.
3. The prompt row sits at `py = PAD + (rows-1)*MONO_H`, with a 1 px line in `dim` at `py-3`. It shows the cwd in accent (from `getcwd`, falling back to "/"), a space, and `"> "` in dim.
4. Input viewport: `room = cols - strlen(cwd) - 3`, at least 8. If the cursor moves left of `in_off`, `in_off` jumps back to `in_pos - room/2`. If it moves past the right edge, `in_off = in_pos - room + 1`.
5. Cursor: shown only while `view == 0`, blinking with `(blink/12) % 2 == 0`. Inside the text it is a block in `pal.cursor` with the character redrawn in `pal.bg`. At the end of the line it is a 2 px bar.
6. When `view > 0`, a dim box at top right reads `" N lines back, End returns "`. Any key except PageUp or PageDown returns to the bottom (`term.c:1729`).

`publish_text` (`term.c:306-316`) sends the last 60 lines of the scrollback, each followed by `\n`, from a 4096-byte buffer (`WM_TEXT_MAX` is 4096, `include/wm.h:77`), through `win_set_text`. It runs on every repaint, including idle cursor blinks. The terminal ignores `WIN_EV_FIND` (type 6): the loop never tests for it, so it never highlights a match.

`point_to_cell(x, y)` (`term.c:212-233`) maps a pixel to a (line, col) with the same start/end arithmetic. The row is clamped to the text rows and the column to `0..cols`.

#### 3.1.6 Selection, copy and paste

- `selection_text(out, cap)` (`term.c:242-264`) flattens the ordered range, joins lines with `\n` and strips trailing spaces from each.
- `copy_selection()` (`term.c:266-279`) uses a `static char out[COLS*40]` (6400 bytes). With no selection it copies the input line (`clip_set(input, in_len)`), prints dim "copied the input line", or "nothing to copy" when the line is empty. With a selection it prints "copied".
- `paste_clipboard()` (`term.c:281-296`) prints "clipboard is empty" when `clip_len() <= 0`. Otherwise `clip_get(incoming, 161)` inserts characters up to the first `\n` or `\r` through `insert_char`, then prints "pasted". Every one of these status messages is pushed into the scrollback.
- Mouse (`term.c:1841-1863`). A press (`WIN_BTN_DOWN` 0x80, from either button) starts dragging and clears the old selection. Motion with `WIN_BTN_LEFT` moves the end, and `sel_active` becomes true when the ends differ. Release stops dragging. A click therefore selects nothing, and clicking does not move the text cursor.
- Keys: ctrl-A selects the whole scrollback, lines 0 to n_lines-1 (the comment says "everything on screen"). ctrl-L clears the selection. Any other key except PageUp and PageDown clears it too (`term.c:1725`).

#### 3.1.7 Paths and helpers

- `path_join(dir, name, out, cap)`: an absolute `name` is used as is; otherwise `dir + '/' + name` (`term.c:406-417`).
- `parse_num(s, fallback)`: decimal digits only; anything else gives the fallback (`term.c:419-428`).
- `split(s, argv, max)`: splits on spaces only, with no quoting and no tabs. It is called with max 16, and past 16 words the 16th absorbs the rest of the line (`term.c:440-449`, section 10).
- `join_from(argv, argc, from, out, cap)` rejoins words with single spaces (`term.c:451-458`).

#### 3.1.8 The 38 built-in commands (`COMMANDS[]`, `term.c:1315-1354`)

The table row is `{name, handler, args, what}` (`term.c:1308-1313`) and `command_named` does a linear strcmp (`term.c:1357-1361`). A built-in **shadows** any `/bin` program of the same name, so ls, cat, echo, grep, wc and ps typed by bare name always run the built-in.

| # | Command | Args | Behaviour (and messages) | Lines |
|---|---|---|---|---|
| 1 | `help` | [COMMAND] | Without args: dim "commands", then each `"  name args"` padded to column 28 plus the description, a blank line, and three dim hints ("A name on its own runs that program: here, then /usb, then /bin." / "Tab completes. Up and down walk through history." / "PageUp and PageDown scroll. Escape leaves the desktop."). With an arg: `name args` in accent plus the description, or "no such command". F1 also runs it. | 1363-1387 |
| 2 | `ls` | [PATH] (default ".") | A file prints `%10u  PATH`. A directory prints `"     <dir>  NAME/"` in accent for subdirectories and `%10u  NAME` for files, then a dim summary "N file(s) in B bytes[, D director(y/ies)]". An empty **or nonexistent** directory prints "(empty)". | 480-518 |
| 3 | `shutdown` | | "switching off", then `power_off()`. If that returns: "this machine will not power off by itself". SYS_POWER flushes the disk (`syscall.c:463-475`), but terminal history is **not** saved. | 464-471 |
| 4 | `reboot` | | "restarting", `power_reboot()`, then "this machine did not restart". | 473-478 |
| 5 | `tree` | [PATH] | "no such path" / "that is a file". Prints the root in accent, then `walk()` to depth 6 with two-space indents: directories as `NAME/` in accent, files padded to column 28 with the size right-aligned in 8. Summary: "F files, D directories, B bytes". | 520-564 |
| 6 | `cd` | [PATH] (default `/home`) | `chdir`. "not a directory" on failure; otherwise prints the new cwd dim. | 1274-1279 |
| 7 | `pwd` | | Prints the cwd. | 1281-1285 |
| 8 | `cat` | FILE... | Prints a dim `== NAME ==` header per file when given more than one, then the content (at most 16383 bytes, silently truncated). Errors from `read_all`: "no such file", "that is a directory", "cannot open it". | 623-629, 603-621 |
| 9 | `head` | FILE [LINES=10] | First N lines. A non-numeric N means 10. | 660-663, 633-658 |
| 10 | `tail` | FILE [LINES=10] | Last N lines. | 665-668 |
| 11 | `grep` | TEXT FILE... | Case-sensitive substring. Prints `LINE: text`, or `FILE:LINE: text` for more than one file; one hit per line; "no matches" dim. Needs at least one file (no stdin). | 695-727 |
| 12 | `find` | NAME [WHERE="/"] | Names containing NAME, searched `hunt()` to depth ≤6 with at most 200 matches. Prints full paths (directories in accent). Summary "N match(es)" or "nothing matched". | 566-596 |
| 13 | `hex` | FILE [BYTES=256] | 16 bytes per line: a 4-hex-digit offset, hex pairs with an extra gap after the 8th, and `|ascii|`. "N more bytes" when truncated. | 730-765 |
| 14 | `wc` | FILE | `%7u lines%8u words%9u bytes  FILE`. Counts a final unterminated line; words split on space, `\n`, `\t` and `\r`. One file only. | 670-691 |
| 15 | `stat` | PATH | path, kind, then entries (directory, counted with readdir) or size. A "kept" line decides by the typed prefix: `/sys` "generated when read, stored nowhere", `/bin` "inside the kernel image", `/tmp` "on the disk, cleared at boot", anything else "on the disk". | 842-865 |
| 16 | `write` | FILE TEXT... | `O_WRITE|O_CREATE|O_TRUNC`. The text is the words joined by single spaces (at most 159 characters) plus `\n`. Prints "N bytes to FILE", or "cannot open it" / "write failed". deskcheck uses it to rewrite `/zelr.cfg`. | 769-788 |
| 17 | `append` | FILE TEXT... | The same with `O_APPEND`. | 789 |
| 18 | `cp` | SRC DST | `copy_file`: `read_all(src)`. If DST is a directory the target is DST/basename(SRC). Then `spit`, then "N bytes to TARGET" or "cannot write it". **Truncates at 16383 bytes.** | 809-835 |
| 19 | `mv` | SRC DST | `copy_file`, then `unlink(SRC)` on success. **It does not use rename(), and above 16383 bytes it loses data** (section 10). Directories cannot be moved. | 837-840 |
| 20 | `rm` | FILE... | `unlink` each: dim "removed X" or warn "rm: X". | 791-797 |
| 21 | `mkdir` | NAME | "ok" or "mkdir failed". | 799-802 |
| 22 | `rmdir` | NAME | "ok" or "not empty, or not a directory". | 804-807 |
| 23 | `run` | PROGRAM [&] | Uses `/bin/PROGRAM` unless PROGRAM starts with `/`. Calls `spawn(path)` **without arguments** (argv[0] is the path). `&` as the second word means don't wait. Bare names are looked up in /bin only. | 941-952 |
| 24 | `ps` | | Header `" pid  ring  state      slices  name"`. Each row: pid in width 4, ring "3" or "0" (from `t.user`), a state word, slices in width 7 and the name. Its own row is in accent with `"   <- this terminal"`. | 874-901 |
| 25 | `kill` | PID | Errors: "that is not a pid", "not this one" (its own pid), "no such task". On success `kill()` (SYS_KILL) and "killed N". SYS_KILL has no ring or ownership check (section 10). | 903-910 |
| 26 | `sys` | [NAME] | Without an arg: lists `/sys` entries between two dim lines. With NAME: prints `/sys/NAME` (an absolute NAME is used as is). | 954-970 |
| 27 | `watch` | FILE [TIMES=5, max 20] | Reads and prints the file repeatedly with a dim `-- read i --` header, repainting (`draw_all` + `win_commit`) and sleeping 500 ms between reads. The event loop is blocked meanwhile. | 972-991 |
| 28 | `time` | COMMAND... | Runs the rest through `run_line`, then "took T ticks, T*10 ms" in accent. The timer is 100 Hz (`kernel/main.c:459`). | 993-1009 |
| 29 | `mem` | | A 40-character `#`/`.` bar of used over total physical memory, "used of total KiB used, free free", the kernel heap in KiB (`heap_total_kb`), and disk KiB free. | 1027-1051 |
| 30 | `uptime` | | "up [Hh ][Mm ]Ss, T tasks, N system calls served". | 1011-1025 |
| 31 | `net` | | address, gateway, netmask, resolver as dotted quads (high byte first) and the MAC. "no network" when not up. | 1055-1079 |
| 32 | `resolve` | HOST | "HOST is a.b.c.d" in accent, or "could not look it up". | 1081-1086 |
| 33 | `get` | HOST [PATH="/"] [FILE] | See 3.1.9. | 1088-1185 |
| 34 | `theme` | [NAME] | Without an arg: lists palettes, marking the active one "   <- in use", then "theme NAME to change it; it is remembered in /cfg/term". With NAME: `apply_palette`, `save_theme`, "theme is now NAME". Unknown names give "no such theme, try theme with no argument". | 1233-1255 |
| 35 | `history` | | `%4u  entry` for each entry, or "nothing yet". | 1257-1263 |
| 36 | `echo` | TEXT | The joined words. | 1268-1272 |
| 37 | `clear` | | `n_lines = first = view = 0`. The selection is not cleared. | 1265-1266 |
| 38 | `about` | | Fixed text: "zelr terminal", pid, "ring 3", "access system calls only". | 1287-1299 |

README.md:908 says "thirty-six commands of its own"; the table has 38. There is no `jobs`, `exec` or `bg` in the terminal. `jobs` exists only in `/bin/sh`, and `exec` and `bg` only in the kernel shell.

#### 3.1.9 `get` in detail (`term.c:1099-1185`)

1. `https://` and `http://` prefixes are matched case-insensitively by `starts_scheme` (`term.c:1090-1097`). The default is secure. A path cannot be part of HOST: `get example.com/x` treats `example.com/x` as the host name.
2. Prints dim "connecting to HOST" and repaints, since the connect can block for seconds.
3. Secure: `connect_tls(host, 443)`. The kernel returns a socket handle, or `NET_ERR_*` < 0 (`syscall.c:779-808`). On failure it prints `tls_why()` if non-empty, else "could not connect". On success it prints dim `secure: <tls_what()>`. Plain: `connect(host, 80)`.
4. The request, built in 512 bytes and silently truncated beyond that: `GET <path> HTTP/1.0\r\nHost: <host>\r\nConnection: close\r\nUser-Agent: zelr-term\r\n\r\n`.
5. Receive loop into `io`: stop on `NET_EOF` (-2), any other negative value, three consecutive 0 returns (the kernel's recv timeout), or 16383 bytes. Then `disconnect`.
6. "nothing came back", or "N bytes received". The headers are split at the first CRLFCRLF; without one the whole response is treated as the body. The status line (at most 63 characters) is printed.
7. With FILE: `spit` the body, then "saved N bytes to FILE" or "could not save it". Without FILE: print the body. Redirects are not followed and an error status is still saved.

#### 3.1.10 Program launching (`term.c:912-952`, `term.c:1389-1460`)

- `PROG_PATH[] = { 0 (cwd), "/usb", "/bin" }` (`term.c:1401`).
- `find_program(name, out, cap)` (`term.c:1405-1430`). If the name contains `/`, it is copied verbatim and the result is `stat(out)==0 && !is_dir`, with no search. Otherwise each place is tried in order (cwd via `getcwd` and `path_join`) and the first `stat`-able non-directory wins. It **does not check that the file is an ELF**. So in the default cwd `/home`, typing `notes` finds the seeded text file `/home/notes` (`kernel/layout.c:137-140`) before `/bin/notes`, and prints "cannot start /home/notes". progcheck.py (lines 71-82) asserts exactly this behaviour of the kernel shell.
- `run_line(cmdline)` (`term.c:1434-1460`):
  1. `split` into at most 16 words.
  2. If the first word is a built-in, call it.
  3. Otherwise `find_program`. `bg` is set if the last word starts with `&`; the vector is `argv[0..n)` with n = argc, or argc-1 when bg, capped at 15, and is null-terminated. Then `start_program_on(path, argv, bg)`. argv[0] is the name **as typed**, not the resolved path.
  4. If nothing was found: `NAME: not a command, and no program of that name here, on a stick or in /bin` in warn.
- `start_program_on(path, argv, background)` (`term.c:915-935`) calls `spawnv(path, argv)`, or `spawn(path)` when argv is 0. On failure: "cannot start PATH". In the background it prints "PATH started as pid N" and **never waits for the task**. In the foreground it calls `wait_for(pid)` and times it with ticks, then prints "pid N finished, T ticks" in dim or "pid N exited with S, T ticks" in warn. S is printed as `u32`, so the -1 left by SYS_KILL shows as 4294967295.
- While waiting in the foreground the terminal's single thread is blocked. No events are processed and nothing repaints, so a close request stays queued. The window event queue holds 31 events and silently drops the rest (`include/wm.h:60`, `wm.c:633-634`). A child that opens a window goes on top (`wm.c:554`) and takes the keyboard. **ctrl-C in the terminal means copy**, and the terminal never sends signals. A foreground `spin` therefore hangs that terminal until it is killed from another terminal (`kill`) or from the Monitor (`userland/monitor.c:352`).

#### 3.1.11 History (`term.c:1462-1500`, `1686-1702`)

- `history_add(line)` skips empty lines and exact repeats of the last entry. When full it shifts everything down one, dropping the oldest (`term.c:1464-1473`). Lines of only spaces are added.
- `history_save()` writes every entry plus `\n` into `io` and `spit`s it to `/cfg/history`, **only when non-empty**. It is called **only** on `WIN_EV_CLOSE` (`term.c:1815-1818`). Leaving the desktop with Escape closes every window (`wm.c:4064`), so the history is saved then too, unless the terminal is blocked on a foreground child.
- `history_load()` at startup: `slurp` the file (at most 16383 bytes), split on `\n` and add each non-empty line.
- `history_step(delta)`: Up is +1 and Down is -1. On the first Up the live line is stashed. `hist_at` is clamped to `0..n_hist`, and position k shows `hist[n_hist - k]`; returning to 0 restores the stash.
- With several terminals open, whichever closes last overwrites the file and the others' new entries are lost (each rewrites the whole file from memory).

#### 3.1.12 Tab completion (`term.c:1502-1634`)

1. `word_start()` scans left from the cursor to a space. `stem` runs from there to the cursor. `first_word(start)` is true when only spaces precede it.
2. **First word:** candidates are `COMMANDS[].name` plus every `readdir("/bin")` name with the stem as a prefix. Programs in the cwd or `/usb` are not candidates, and neither is a path typed as the first word. Duplicates are **not** removed: ls, cat, echo, grep, wc and ps come back twice.
3. **Later words:** everything up to the last `/` in the stem names the directory (`"."` when there is no slash, `"/"` for a leading slash). Directory entries whose names start with the leaf become candidates, and a directory gets a trailing `/`. Names are truncated to 29 characters (`with[VFS_NAME]`, `strncpy(..., VFS_NAME-2)`).
4. `cand_add` stops at 32 candidates (`term.c:1512-1517`).
5. `shared = common_prefix()`. If it is longer than the stem, `replace_word(start, cand[0], shared)` inserts it and keeps the text after the cursor. If there was exactly one candidate that does not end in `/`, a space is appended at **the end of the line** and the cursor moves there (`term.c:1616-1619`, a bug when completing mid-line).
6. Otherwise, with more than one candidate, the candidates are printed as dim lines wrapped to the width.

#### 3.1.13 The line editor (`on_key`, `term.c:1704-1768`)

- Ctrl chords are handled first. `key_ctrl_letter` maps codes 1-26 back to letters: `c` copies, `v` pastes, `a` selects everything, `l` clears the selection, and any other ctrl chord is swallowed.
- Then `key = KEY_CODE(key)`. Any key other than PageUp or PageDown clears the selection and sets `view = 0`.

| Key | Action |
|---|---|
| Enter (`\n`) | `submit()`: echo `"cwd > input"` in dim, `history_add`, reset the input, `hist_at` and `view`, then `run_line(copy)` (`term.c:1644-1662`) |
| Tab | `complete()` |
| Backspace (`\b`) | delete the character before the cursor |
| Left / Right | move the cursor |
| Home / End | cursor to the start or end of the line (End also leaves scrollback) |
| Delete | delete the character under the cursor |
| Up / Down | history (`history_step(+1/-1)`) |
| PageUp | `view += rows/2`, clamped to `n_lines-(rows-1)` |
| PageDown | `view -= rows/2`, clamped at 0 |
| F1 | `run_line("help")`. Not echoed and not added to history. |
| 32..126 | `insert_char`, ignored once `in_len >= 160` |

Insert, F2-F12 and DEL (127, as sent by a serial terminal) do nothing. Escape never reaches the terminal because the WM takes it to leave the desktop.

#### 3.1.14 Main loop (`term.c:1782-1875`)

1. `win_create("terminal", 760, 480)`. On failure `puts("term: no window\n")` and return 1. `win_surface`, `win_width` and `win_height` follow, with return 1 if the surface is missing or the size is non-positive.
2. `fit_to_window()`, `win_allow_resize(win)`, `load_theme()`, `history_load()`.
3. Banner: "zelr terminal" in accent, "A shell running in ring 3. Type help, or press F1." and "Tab completes, up and down are history, PageUp scrolls." in dim, then a blank line. `draw_all` and `win_commit`.
4. Forever:
   - Drain `win_poll(win,&ev)==1`:
     - `WIN_EV_CLOSE` (3): `history_save`, `win_close`, return 0.
     - `WIN_EV_RESIZE` (4): `scr.w = ev.x`, `scr.h = ev.y` (the surface address is unchanged, per `zelr.h:932-938`), then `fit_to_window`, `view = 0`.
     - `WIN_EV_SCROLL` (5): `view -= ev.y*3`, clamped. A positive y means the wheel moved down, towards the live end.
     - `WIN_EV_KEY` (2): `on_key`.
     - `WIN_EV_MOUSE` (1): selection.
   - `blink++`, and force a repaint every 12 iterations.
   - If anything changed: `draw_all` and `win_commit`.
   - `sleep_ms(20)`. The loop polls; there is no blocking wait for events.

#### 3.1.15 Themes (`term.c:1187-1255`)

- `apply_palette(i)` falls back to index 0 when out of range (`term.c:1189-1192`).
- `save_theme(name)` calls `spit("/cfg/term", name, strlen)`, with no newline (`term.c:1194`).
- `default_palette()` reads at most 511 bytes of `/zelr.cfg` and finds a line starting with `light `. A value of `'0'` gives 0 (slate) and anything else gives 1 (paper). If the key or the file is missing it returns 0 (`term.c:1206-1219`). The kernel's own default is light (`kernel/theme.c:337`), but it only writes `/zelr.cfg` from `theme_save` (volume changes, the self-test and the settings program). On a fresh disk the file does not exist, which is why every harness sees slate.
- `load_theme()` reads at most 31 bytes of `/cfg/term`, cuts at `\n` or `\r`, and matches the name exactly, falling back to `default_palette()` (`term.c:1221-1231`). A theme only applies to terminals started afterwards; open ones keep theirs.

### 3.2 `userland/sh.c` (340 lines)

Includes `zelr.h`, `alloc.h` (unused, since no malloc is called) and `args.h` (`sh.c:27-29`).

**Constants** (`sh.c:31-33`): `LINE_MAX` 512, `WORDS_MAX` 32 (`args_split` gets 31 so a null always fits), `STAGES_MAX` 4.

**`stage_t`** (`sh.c:37-43`) has `char *argv[32]`, `int argc`, `const char *in` (the `<` path), `const char *out` (the `>` or `>>` path) and `int append` (1 for `>>`). The global is `char line[512]` (`sh.c:45`).

**Helpers.**
- `say(s)` is `puts` (fd 1).
- `say_err(a, b)` writes `"sh: a[: b]\n"` to **fd 2** (a 256-byte buffer; a is capped at n<200 and b at n<250) (`sh.c:49-64`).
- `has_slash` (`sh.c:66-69`).

**Grammar, as implemented:**
- `parse(text, stages, &count, &background)` (`sh.c:101-142`):
  1. Strip trailing spaces and `\n`. If the last character is `&`, set background and drop it. This applies to the whole line, and also to `a&` with no space.
  2. Cut the line at **every `|` character before any quote handling**, so a `|` inside quotes still splits.
  3. Each piece goes through `args_split(piece, argv, 31)` and then `take_redirections`.
  4. An empty stage in a pipeline (`ls |`, `| wc`) gives the error "there is nothing on one side of the |". A fifth stage gives "that is more stages than this shell has".
- `args_split` (`userland/args.h:19-39`) splits on space and tab. A `"` at the start of a word keeps everything up to the next `"` and drops the quotes. There is no escaping and no single quote, and a quote in the middle of a word is literal.
- `take_redirections(st)` (`sh.c:75-98`) treats the standalone words `<`, `>` and `>>` as operators; the next word is the path and both are removed. An operator with no path gives "a redirection needs somewhere to go". Since quotes are already gone, a quoted `">"` still counts as an operator. `a>b` is an ordinary word. `2>` is not recognised.
- Not supported: `;`, `&&`, `||`, variables, globbing, job control, `2>` and here-docs.

**Built-ins** (`builtin`, `sh.c:150-187`). They are only considered when the line is a single stage with no redirection (`sh.c:335`).

| Built-in | Behaviour |
|---|---|
| `exit` | `exit(0)` (ignores any argument) |
| `cd [DIR]` | Default is **`/`**. On failure: `sh: cannot go to: DIR` |
| `pwd` | Prints the cwd and `\n` |
| `help` | Fixed text listing the operators and the built-ins (`sh.c:168-176`) |
| `jobs` | "this shell does not keep track of background work yet" |

Any other first word, including a built-in used with a redirection or inside a pipeline, is exec'd, so `pwd > f` fails with "sh: not found: pwd".

**Running** (`run`, `sh.c:219-303`). For each stage i:
1. If i is not the last stage, `pipe(ends)`; on failure "no pipe to be had".
2. `fork()`; on failure "cannot make another process".
3. In the child:
   - `signal(SIGINT, SIG_DFL)`.
   - If there is a carried read end: `dup2(carried, 0)` and close it.
   - If not last: close `ends[0]`, `dup2(ends[1], 1)`, close `ends[1]`.
   - Then `<` (`O_READ`) or `>`/`>>` (`O_WRITE|O_CREATE|O_TRUNC` or `O_APPEND`) through `open_for` (`sh.c:213-217`). A named file beats the pipe. An open failure gives `sh: cannot read/write: X` and `exit(1)`.
   - `become(st)`.
4. In the parent: close the carried fd and set it to -1. If not last, close `ends[1]` and carry `ends[0]`. Record the pid.
5. After the loop: close any carried fd. In the background case print `[<pid of the last stage>]` and return without waiting (nothing ever reaps these). Otherwise `wait_for` every pid; statuses are ignored.

`become(st)` (`sh.c:198-211`) uses argv[0] verbatim if it contains a slash, otherwise `"/bin/" + argv0`. **Only /bin is searched**, unlike the cwd, `/usb`, `/bin` order of the terminal and the kernel shell. Then `execv(path, st->argv)`. If that returns: `sh: not found: NAME` and `exit(127)`.

**Main loop** (`sh.c:307-340`):
1. `signal(SIGINT, SIG_IGN)`.
2. Banner: "zelr shell. type help for what it can do, exit to leave."
3. Loop: prompt `cwd + " $ "` (`sh.c:307-311`). `fread(0, line, 511)`: a result ≤0 prints `\n` and returns 0 (that is ctrl-D, 0x04, in `console_read`, `fd.c:243`). Then parse, handle built-ins, and `run`.

Reading more than one line from a pipe or file in a single `fread` would glue lines together (only a trailing `\n` is stripped), so sh is for interactive console use only.

**ctrl-C with sh.** The serial byte 3 raises `signal_interrupt()` in the serial ISR (`kernel/serial.c:56`). A PS/2 ctrl-C raises it in `push()` (`keyboard.c:44-58`). `signal_interrupt` (`signal.c:262-287`) sends SIGINT to every live child of `console_pid`. `console_pid` is the task that last called `console_read`, set in `fd.c:219-223`; here that is sh. If there are no children, the reader itself gets SIGINT, and sh ignores it. The child dies with status 130 and `wait_for` returns. The queued byte 3 is later read by `console_read`, which echoes `^C\n` and returns `"\n"` (`fd.c:252-258`). sh then parses an empty line and prints a fresh prompt.

### 3.3 The coreutils

Each takes `(ac, av)` and uses `argv = av+1` as its operands. Output always goes through `puts` and `putc`, both of which write to fd 1 (`syscall.c:506-525`), so it can be redirected and piped. **Error messages also go to fd 1** (`cat.c:40-42`, `grep.c:66,79-81`, `ls.c:16-18`, `wc.c:58-60`), against sh's rule that complaints belong on fd 2.

| Program | Behaviour | Exit |
|---|---|---|
| `cat` (`cat.c`) | `drain(fd)` copies 4096-byte chunks to fd 1 until `fread` returns 0; a short `fwrite` is retried. No operands means drain fd 0. A bad file gives `cat: cannot read X` and cat moves on. | 1 if any file failed |
| `echo` (`echo.c`) | argv[1..] joined by single spaces, then `\n`. No `-n`. | 0 |
| `grep` (`grep.c`) | `grep WHAT [FILE...]`. `scan()` reads 2048-byte chunks into a 1024-byte line buffer and `contains()` does a plain substring test (an empty needle matches everything). Matching lines are printed; a last line without a newline still counts. With no files it reads fd 0. No args: `usage: grep WHAT [FILE ...]`, exit 2. Lines over 1023 characters are cut and **lose one character** per cut (`grep.c:37-47`). | 0 found, 1 none, 2 usage |
| `ls` (`ls.c`) | With no operands, list `getcwd()` (or "/"). For each directory, `readdir` from index 0 until it returns 0, printing each name plus `/` for directories. With more than one operand, each gets a `NAME:` header and a blank line between. A readdir error gives `ls: cannot read X` (this includes a regular file given as an operand). | 1 if any failed |
| `ps` (`ps.c`) | Header `PID` (width 6), `STATE` (10), `SLICES` (9), `NAME`. Rows: pid right-aligned in 4 plus 2 spaces, state (`{"ready","running","sleeping","blocked","dead"}`, or `?` for anything ≥5), slices right-aligned in 7 plus 2 spaces, name. It lists every task the kernel returns. | 0 |
| `wc` (`wc.c`) | Counts bytes, `\n` and words (separated by space, tab and `\n`; `\r` is **not** a separator). No operands means count fd 0 and print `L W B`. Files print `L W B NAME`, and more than one adds a `total` line. It does not add a line for an unterminated final line, which differs from the terminal's built-in `wc`. | 1 if any failed |

### 3.4 Demo programs

- **hello.c.** Prints "hello from a program the kernel had never seen.", its pid, "ring 3, reached through int 0x80", "loaded from an ELF file on the FAT16 disk" (stale: /bin/hello is served from the kernel image), and "arithmetic 5050 (1..100 summed in user space)". Exits 0. shell_test.sh looks for "hello from a program". progcheck looks for that marker and for "5050". spawntest requires status 0. polltest opens `/bin/hello` as an always-readable file.
- **count.c.** Prints "counting in the background, pid N", then 5 lines "  tick i at T ticks" with `sleep_ms(700)` between them, then "done". Exits 0 after about 3.5 s. forktest execs it and expects status ≠99. sigtest execs it and kills it with SIGTERM, expecting 143. spawntest waits for it (at least 5 ticks). appcheck types `run count &`, but see section 10 on where those keys actually go. The kernel guide suggests `bg /bin/count` (`welcome.c:71`), and the pins self-test uses `/bin/count` as a sample path without running it (`selftest.c:1010-1036`).
- **spin.c.** Prints "spinning", then `for(;;) n++` on a volatile counter and never sleeps: "the harder case, and the one that used to be impossible to stop" (`spin.c:16-19`). It is the target of shcheck's ctrl-C test.

### 3.5 The ring 3 test programs

Common pattern: a static failure counter, an `ok(what, cond)` that prints `"  PASS  "`/`"  FAIL  "` or `"  ok   "`/`"  FAIL "` before the label, and a final marker. All of them are run by `tools/ring3check.py` through `exec /bin/NAME` (which the kernel shell waits for, `kernel/shell.c:314-333`), except halfdrawn (tearcheck.py), wintest (shell_test.sh) and spawntest (nothing).

#### alloctest.c: the allocator (`ALLOCTEST_PASS`, ring3check timeout 90 s)

This tests the `userland/alloc.h` allocator: segregated free lists with boundary tags, `AL_ALIGN` 16, `AL_CHUNK` 64 KiB, `AL_FOOT` 16, and growth through `sbrk`.

Checks:
1. `heap_size()==0` at start ("a program starts with no heap at all").
2. `malloc(100)` works and keeps its contents.
3. A second block is distinct and the first is still intact.
4. A double free is harmless (`free` returns early when `AL_INUSE` is clear, `alloc.h:158`).
5. `malloc(0)==0`, and `free(0)` is allowed.
6. Alignment. 64 blocks of `1+7i` bytes are held **at the same time**, which forces splits: the first version passed by reusing one block while every split block was 8 bytes out (`alloctest.c:89-95`).
7. Churn: 1200 rounds over 400 slots driven by xorshift32 with seed `0x5A4C5200`. An occupied slot is checked and freed. An empty slot is allocated 8..207 bytes, or 2000..10999 bytes with probability 1/32, and filled with `tag + i`.
8. Every block still holds what was written.
9. The heap is under 1 MiB after churn, printed in KiB.
10. `after_first` > 0. This is the heap size at round 100, and it is only recorded if round 100 happened to be an allocation (section 10).
11. `heap_live()==0` after everything is freed, and then a single `malloc(heap_size()/2)` succeeds.
12. A 400 KiB block grows the heap and is fully writable.
13. `realloc(64 → 4096)` keeps the contents.

Returns the failure count.

#### argvtest.c: the argument vector (`ARGVTEST_PASS`, 120 s)

`SELF` is "/bin/argvtest" and `MARK` is "/home/argvtest.run". `CHILD_ARGV` is `{SELF, "child", "two words", "four", NULL}`.

- **Child mode**, when `argv[1]=="child"`: argc==4, argv[0]'s basename is "argvtest", argv[2]=="two words" (a vector that had been glued into one string and split again would arrive as two words), argv[3]=="four", argv[4]==NULL.
- **Parent mode.** If MARK exists without "child", print "FAIL a child was started on a vector and got none of it" and `ARGVTEST_FAIL`. This guard exists because a kernel that loses the vector would otherwise fork-bomb: each child would run as a parent (`argvtest.c:23-28`). Then:
  1. argv[0] is non-empty and names itself.
  2. `argv[argc]==NULL` and `argv_len(argv)==argc`.
  3. Create MARK.
  4. fork plus `execv(SELF, CHILD_ARGV)` gives `wait_for==0`; if the exec fails the child exits 70.
  5. `spawnv(SELF, CHILD_ARGV)` gives `wait_for==0`.
  6. A 2999-character word must be **refused** (`spawnv<0`): the kernel caps all strings together at `ARGV_BYTES` 2048 (`syscall.c:126`). A truncated vector is "the failure nobody sees".
  7. Remove MARK and print the marker.

#### cowtest.c: copy-on-write fork (`COWTEST_PASS`, 180 s)

Allocates 1024×4096 bytes (4 MiB) and touches one byte per page. `before = mem_free_kb`. Then fork:
- The child sleeps 2500 ms, writes 0x5A to every page, reads them all back, and exits 0 or 2.
- The parent checks:
  1. `fork_cost = before - after_fork < 512 KiB` ("a fork does not copy the program's memory").
  2. After writing 0xA5 to every page, `write_cost > 3000 KiB`.
  3. The parent kept its own writes (this PASS line is only printed if nothing failed earlier).
  4. The child's status is 0.
  5. After 400 ms, `after_child > after_write + 2000 KiB` ("the child's memory comes back"). This catches refcount leaks.

README says the measured cost went from 4208 KiB to 40 (`README.md:660-661`).

#### durtest.c: fsync and rename (`DURTEST_PASS`, 180 s)

Paths: `/home/dur-a.txt` (A), `/home/dur-b.txt` (B), `/home/dur-t.txt` (TMP) and `/cfg/dur-a.txt` (DEEP). `on_disk(path)` reads through a **fresh** descriptor.

Checks:
1. After writing "first" to an open fd, `on_disk(A) <= 0`: nothing is on disk until somebody says so. This pins the write-back-until-close semantics.
2. `fsync==0`, and then A reads "first".
3. `seek(fd,0,0)`, write "second", fsync; A reads "second" and still does after close.
4. `rename(A,B)==0`; B has "second" and A is gone.
5. A rename over an existing file replaces it (`TMP → B` gives "replaced") and TMP is gone.
6. Refusals, each followed by a check that the file is untouched: across directories (`B → /cfg/dur-a.txt`), to a name that is not 8.3 (`a-name-far-too-long-for-eight-three.text`), and from a source that does not exist.
7. `rename(B,B)==0` and B is unchanged.
8. Unlink B.

The reason given for the refusals: a second directory entry would be needed, and FAT has nowhere to record that two entries are one rename in progress (`durtest.c:104-111`, `zelr.h:684-693`).

#### faulttest.c: faults end programs, not the machine (`FAULTTEST_PASS`, 240 s)

`FAULTED` = 139. The kernel ends any ring 3 exception with `task_exit_with(139)` (`kernel/idt.c:114-143`). Each child is forked with `faulting_child(fn)`.

Checks:
1. Read from null gives 139.
2. "and the machine is still here", which always passes; the point is that the program is still running.
3. Write to null gives 139.
4. Divide by zero gives a nonzero status (the kernel returns 139 here too).
5. Reading `0xFFFF800000000000` (the kernel half) gives 139.
6. 100 null-write children all give 139.
7. `before.mem_free_kb - after.mem_free_kb < 4096`, with "memory moved by N KiB" printed.

The volatile globals `nowhere`, `zero`, `sink` and `far_away` stop the optimiser from folding away the undefined behaviour. The comments record two earlier false passes: the division was deleted, and a constant kernel address folded to address 0 (`faulttest.c:37-56`).

#### fdtest.c: descriptors across processes (`FDTEST_PASS`, 120 s)

Uses `/fdt.txt` and `/fdt2.txt` at the root.

Checks:
1. The first open returns 3 (0, 1 and 2 are the console).
2. A child reading the inherited fd gets "one" (exit 31), and the parent continues from " two": the position is shared.
3. In a child, `open` returns 4, then `close(3)` and `open` returns 3 (exit 32), while the parent's 3 is untouched.
4. Redirection: the child does `dup2(file,1)` and `puts("redirected")` (exit 33), and the file holds exactly 10 bytes, "redirected".
5. A pipe between processes: "down the pipe" (13 bytes), exit 34, and the next `fread` returns 0 once the writer is gone.
6. A two-child pipeline, left `puts("carried")` and right `fread(0)`, gives 35.
7. fd 11: fread, fwrite, dup and close all return <0.

This is the same sequence of operations sh performs.

#### forktest.c: fork and exec (`FORKTEST_PASS`, 120 s)

Checks:
1. fork returns >0 to the parent; the child sets `shared_counter=7` and exits 42; the parent still sees 100.
2. Heap and .data separation: the child writes `buf[0]='z'` and `shared_text` (exit 17); the parent still has 'a' and "untouched".
3. `getppid()==me` (exit 55).
4. Four children exit with 1..4, and the statuses sum to 10.
5. A child allocates 200×300 bytes (exit 23), and the parent can allocate afterwards.
6. `exec("/bin/count", 0)` gives a status ≠99 (count takes about 3.5 s).
7. `exec("/bin/there-is-no-such-thing")` returns nonzero, and the child exits 64.

#### fptest.c: floating point in ring 3 (`FPTEST_PASS`, 60 s)

`check(what, got, want_micros)` compares `(long long)(got*1e6)` exactly and prints the value to six decimals from a copy. An earlier version negated in place and printed PASS for failing negative values (`fptest.c:32-35`).

| Case | Expected micros |
|---|---|
| 1/3 | 333333 |
| 2/3 | 666666 |
| 0.1+0.2 | 300000 |
| Newton √2, 40 iterations | 1414213 |
| Newton √10 | 3162277 |
| H(1000), harmonic sum | 7485470 |
| -7/8 | -875000 |
| 1/1024 | 976 |
| (123456789+1)-123456789 | 1000000 (proves double rather than float precision) |

This depends on `kernel/fpu.c:40-66` (CR0.EM cleared, CR4.OSFXSR/OSXMMEXCPT set, fxsave/fxrstor per task).

#### halfdrawn.c: finished frames only (run by tools/tearcheck.py)

Creates window "Half drawn", 360×260. Each loop:
1. Poll events; `WIN_EV_CLOSE` ends the loop.
2. Fill the surface with `HALF_RED` 0xC81E1E **without committing** and sleep 200 ms.
3. Fill with `HALF_GREEN` 0x1E9632 or `HALF_MINT` 0x1EC864, alternating, then `win_commit` and sleep 200 ms.

It exits if the surface is lost. It never publishes red, so a screenshot containing red means the compositor read a frame that was not finished. The alternating greens show the window is still live.

tearcheck.py types `desktop`, then `halfdrawn\n` over serial. The keys land in the desktop terminal, which starts `/bin/halfdrawn` by name in the foreground. It then takes 14 screenshots at uneven intervals of 0.13 s + 0.01·i. Pass requires at least 4000 green-or-mint pixels at the start, zero red in every sample, and both shades seen.

#### maptest.c: anonymous mappings (`MAPTEST_PASS`, 180 s)

Checks:
1. `map(64 MiB, RW)` is non-null and costs under 256 KiB.
2. Touching 16 pages 1 MiB apart costs between 64 and 511 KiB.
3. Bytes 1..63 of each touched page are zero, and the bytes written are kept.
4. A child writing at `p+BIG`, one page past the end, gets 139.
5. A child writing an untouched page at `p+40 MiB` exits 0, and the parent still reads 0 there.
6. `unmap(p,BIG)==0`, and `after_unmap - after_touch >= 48 KiB`.
7. A child touching `p[0]` after the unmap gets 139.
8. 32 `map(4096)` calls: the number granted must be between 1 and 16 (`VMA_MAX` is 16, `include/sched.h:8`; `kernel/user.c:230-254`).

The mmap window is `USER_MMAP_BASE..USER_MMAP_MAX`, 128 MiB (`include/user.h:62-63`).

#### polltest.c: poll (`POLLTEST_PASS`, 180 s)

Checks:
1. A pipe can be made. `pipe()` is called **twice** (`polltest.c:46-47`, section 10).
2. poll on the empty pipe with a 200 ms timeout returns 0 after at least 150 ms.
3. After one byte is written, poll(2000) returns 1 with POLLIN in under 60 ms, and the byte is 'x'.
4. `/bin/hello` opened as a file is always readable (asks for POLLIN|POLLOUT, checks POLLIN, timeout 0).
5. fd 999 gives `n==1` with POLLNVAL.
6. A child sleeps 250 ms and writes "y"; the parent closes its write end and calls poll(5000). It must return POLLIN after between 150 and 2000 ms.
7. Once the writer has exited: POLLHUP within 200 ms, and `fread` returns 0.
8. 24 pollfds give -1 (`POLL_MAX` is 16, `syscall.c:357`).

Elapsed time is `(ticks()-t)*10` ms.

#### sigtest.c: signal handlers (`SIGTEST_PASS`, 180 s)

Checks:
1. The reference value `churn(60000)` is computed with nothing interrupting: eight chained registers, deliberately not a simple sum.
2. `signal(SIGTERM, on_signal)==0`. A self-sent SIGTERM is delivered on syscall return, so the handler has already run (`caught==1`) and received SIGTERM.
3. No re-entry. The handler re-sends SIGTERM once and busy-loops for 400000 iterations. Pass needs `deepest==1` and, within 40×20 ms, `caught==2`.
4. `signal(SIGKILL, quiet)` and `signal(SIGKILL, SIG_IGN)` both fail.
5. With `SIG_IGN`, a self-sent SIGTERM does nothing within 60 ms.
6. Register preservation. With `quiet` installed, a child sends SIGTERM to the parent after 30 ms while the parent runs `churn(60000)`. Pass needs `caught>=1` and the same answer as before (section 10: nothing proves the signal landed mid-churn).
7. Fork inherits the handler: the child sends itself SIGTERM, sees `caught==1` and exits 0.
8. Exec forgets handlers: fork, `execv("/bin/count")`, the parent sleeps 120 ms and sends SIGTERM, and the status must be `128+15`, i.e. 143 (`signal.c:124,197`).

The kernel side is in `kernel/signal.c`, and the handler returns through the `__zelr_sigreturn` trampoline in `zelr.h:604-622`.

#### sleeptest.c: sleep_ms (`SLEEPTEST_PASS`, 120 s)

Starts with a 20 ms warm-up sleep. Checks:
1. `sleep_ms(100)` takes between 90 and 599 ms.
2. 20×`sleep_ms(5)` takes at least 100 ms. The kernel rounds up with `(ms*hz+999)/1000` (`kernel/timer.c:78`).
3. 25×20 ms take at least 450 ms.
4. 30×16 ms take at least 300 ms.
5. 50×`sleep_ms(0)` take under 200 ms.
6. Over 40 samples 10 ms apart, `ticks()` never goes backwards and increases more than 20 times.

The margins only fail early sleeps, because the emulator can be arbitrarily late (`sleeptest.c:19-21`).

#### spawntest.c: spawn and wait (prints `spawntest: ok`; no harness)

Checks:
1. `spawn("/bin/hello")` then `wait_for` gives 0.
2. `spawn("/bin/count")` is waited for at least 5 ticks.
3. `wait_for(9999)==-1`, for a pid that does not exist (`kernel/sched.c:378-381`).
4. `tasks()` returns at least one task.

Exit 0 or 1. It appears only in the kernel's guide (`kernel/welcome.c:73`).

#### wintest.c: windows from ring 3 (run by tools/shell_test.sh)

Checks:
1. `win_create("wintest",64,48)` returns a handle and the size is 64×48.
2. The surface address is printed with all 16 hex digits; eight digits once made a truncated pointer look correct.
3. 3072 pixels are written with `i*7+1` and read back.
4. `win_commit==0` and `win_close==0`.
5. `win_surface` on the closed handle returns 0.
6. `win_width(5)==-1`. Handles are global slot indices with a pid owner check (`winsrv.c:125-130`).

It prints "wintest: ok". shell_test.sh checks for `wintest: surface at 0x0000008060000000`, which is `WINSRV_SURFACE_BASE`, `USER_SPACE_BASE + 0x60000000` (`include/winsrv.h:19`), plus `wrote and read back 3072 pixels`, `closed, handle is dead` and `wintest: ok`. It runs in console mode, with no desktop.

### 3.6 The tool scripts

#### `tools/termcheck.py`: typing into the terminal

Boots `Guest(DISK, memory=64)`, types `desktop\n` over serial, and waits for the screen. Keys are then typed with the QEMU monitor's `sendkey` at the PS/2 keyboard; the `NAMED` map covers space, return, tab, `/`, `.`, `-`, `_` and `,`. Each step waits (up to 30 s, 60 s for the first) until more than 50000 pixels in the rectangle `PAGE=(120,120,700,460)` are exactly the expected theme background.

| Step | Typed | Expected colour | Proves |
|---|---|---|---|
| 1 | (nothing) | SLATE | The terminal opens in its default colours, which needs no `/zelr.cfg` on a fresh disk |
| 2 | `theme paper⏎` | PAPER | A typed command reaches a ring 3 program |
| 3 | `them`, Tab, `amber⏎` | AMBER | Tab completes a command name. The one candidate gets an automatic space: "theme " + "amber". |
| 4 | Up, Up, Enter | PAPER | Up walks back through history (the entry two back is "theme paper") |
| 5 | `theme phosphr`, Left, `o`, Enter | PHOSPHOR | Left moves the cursor and typing inserts |
| 6 | `theme paperx`, Backspace, Enter | PAPER | Backspace |
| 7 | `xtheme amber`, Home, Delete, Enter | AMBER | Home and Delete |
| 8 | `cat /cfg/term⏎` | still AMBER | The terminal survived. The file's contents are not checked. |

Gate: "typing reaches the terminal", screen and full modes (`gate.sh:410-411`).

#### `tools/progcheck.py`: a program from the disk, run by name

**It drives the kernel's serial console shell.** `vm.run` and `vm.fresh` type at the `zelr>` prompt (`harness.py:704-724`), so it exercises `kernel/shell.c:257-290` `run_by_name`, not term.c's `find_program`, although both have the same design. `Guest(DISK, size_mb=64, memory=256)`.

1. `cp /bin/hello /home/mine`; `ls /home` must contain "mine".
2. `disk` output must contain "fat".
3. Typing `mine` must print `MARK="hello from a program the kernel had never seen"` and `SUM="5050"`: it ran as a program rather than being printed.
4. `notaprogram` must say "not a command" and contain "/bin".
5. `write /home/lump not an elf at all`, then `lump`: the output must contain "lump" and not "not a command". The file was found in the cwd and refused by name, without falling through to /bin.
6. `hello` must still run from /bin.

Gate: "a program on the disk runs by typing its name", screen and full (`gate.sh:540,591`).

#### `tools/ring3check.py`: the ring 3 suites

One `Guest(DISK, memory=256)`. For each `(name, marker, label, timeout)` in `SUITES` (`ring3check.py:30-50`) it calls `vm.run("exec /bin/%s" % name, timeout)`. That call returns the **whole serial transcript**, which is safe only because every marker is unique.
- "LABEL runs at all" passes if either the `_PASS` or the `_FAIL` marker appears.
- "and every one of its checks passed" needs the `_PASS` marker.
- On failure it prints up to 13 lines containing FAIL or "wanted", or else the last six lines.

It then runs `jstest` again and parses a `" of " ... "passed"` line for a total, requiring at least 86 cases.

SUITES and timeouts (s): fptest 60, alloctest 90, jstest 180, forktest 120, fdtest 120, pagetest 120, pngtest 120, jpegtest 180, svgtest 180, layouttest 120, cardtest 120, sleeptest 120, cowtest 180, argvtest 120, sigtest 180, faulttest 240, maptest 180, durtest 180, polltest 180. That is 19 suites; the ones not in my scope belong to other areas.

Gate: fast mode, "what a program can do that it could not", with a 600 s timeout (`gate.sh:302-303`).

---

## 4. Control flow and lifecycles

### 4.1 Terminal lifecycle

1. The kernel boots and `init_task` chdirs to `/home` (`kernel/main.c:233-236`). `shell_task` then opens the desktop, either by itself on a machine with a screen or when `desktop` is typed. `enter_desktop` spawns `/bin/term`, which inherits cwd `/home` (`sched.c:170-174`) and gets a fresh console fd table.
2. `main`: create the window, fit the grid, allow resizing, load the theme and the history, print the banner, draw.
3. Steady state is a 20 ms polling loop (3.1.14).
4. The terminal ends through `WIN_EV_CLOSE`: from its close button, from "Close all windows", or from leaving the desktop, since `wm_run` closes every window (`wm.c:4064`) and `wm_close` pushes `WM_EV_CLOSE` to user-owned windows (`wm.c:559-566`). On close it saves history, calls `win_close` and returns 0. A second instance can be opened from the launcher or context menu, and each has its own history in memory.

### 4.2 From typing to output

1. The keystroke becomes a WM key event, then `win_poll`, then `on_key`.
2. Enter calls `submit()`: the prompt line is echoed and the line is added to history.
3. `run_line`: `split`. A built-in handler runs synchronously and pushes lines into the scrollback; otherwise `find_program` and `start_program_on` (a blocking wait unless `&`).
4. `changed = 1`, then `draw_all` and `win_commit`.

Long built-ins (`get`, `watch`, `find /`, `tree /`) block the loop too; `get` and `watch` repaint by hand.

### 4.3 The view state (scrollback)

- `view = 0` means live.
- PageUp raises it by rows/2, clamped to `n_lines-(rows-1)`. PageDown lowers it by rows/2, clamped at 0.
- The wheel changes it by `-3*steps`.
- It drops back to 0 on any other key, on a resize, and on submit.
- The cursor is hidden and the "lines back" box is shown whenever `view > 0`.

### 4.4 Selection states

- **Idle**: `sel_active=0`, `sel_dragging=0`.
- A press moves to **dragging** (active=0) at the pressed cell.
- A left-drag keeps dragging and sets active to "the ends differ".
- A release leaves active as it was and stops dragging.
- ctrl-A makes it active over everything. ctrl-L and most keys go back to idle. PageUp and PageDown keep it.

### 4.5 History browsing states

- `hist_at = 0` is live.
- Up: stash the live line when `hist_at` is 0, increment `hist_at` (at most `n_hist`), and show `hist[n_hist-hist_at]`.
- Down: decrement `hist_at`, showing the stash at 0.
- Submitting resets `hist_at` to 0.

### 4.6 Launching a program

`find_program` checks the path or searches cwd, `/usb` and `/bin`. Then `spawnv(path, argv)`: the kernel copies the vector (at most 2048 bytes, `syscall.c:115-186`), slurps the ELF and builds a System V stack. The task name is the path, so `ps` shows `/bin/NAME`. In the foreground the terminal waits and reports the status (0; 139 for a fault; 128+sig for a signal; -1 for SYS_KILL). In the background it reports the pid and never reaps the task.

### 4.7 sh pipeline sequence

With stages A|B|C, where the parent is P:
1. P: `pipe(e1)`, fork A. A: dup2 `e1[1]`→1, exec. P: close `e1[1]`, `carried=e1[0]`.
2. P: `pipe(e2)`, fork B. B: dup2 `carried`→0 and `e2[1]`→1, exec. P: close `carried` and `e2[1]`, `carried=e2[0]`.
3. P: fork C. C: dup2 `carried`→0, exec. P: close `carried`.
4. P: `wait_for` A, B and C in order, or with `&` print `[pidC]`.

Redirections are applied in each child after the pipe plumbing, so they override it.

### 4.8 What ctrl-C does where

| Context | Effect |
|---|---|
| `/bin/sh` on the console, job running | SIGINT goes to sh's children; they die with 130 and sh prints a new prompt (`signal.c:262-281`) |
| `/bin/sh` at its prompt | SIGINT to sh is ignored; `console_read` echoes `^C` and returns an empty line |
| Desktop terminal | The WM passes the ctrl bit and the terminal **copies**. The PS/2 driver still calls `signal_interrupt()` on every ctrl-C (`keyboard.c:44-58`), which targets the last console reader's children, or the reader itself. The terminal never reads the console, so its own foreground child is not interrupted. |
| Kernel shell | The kernel shell reads `kbd_trygetchar` directly (`shell.c:613`), not `console_read`, so it never becomes `console_pid` |

---

## 5. Interfaces

### 5.1 What starts these programs

- `/bin/term`: `kernel/shell.c:217-231` (desktop start), `wm.c:375` ("Open terminal"), `wm.c:394` (launcher "Terminal"), `wm.c:873` (desktop icon), `pins.c:20` (default pin), or any program via spawn.
- `/bin/sh`: `exec /bin/sh` at the kernel shell (`shcheck.py:85`). Typing `sh` in the desktop terminal would start it with its I/O on the kernel console, which is not useful.
- Coreutils: sh pipelines. count is exec'd by forktest and sigtest and spawned by spawntest. hello is used by spawntest, polltest, shell_test.sh and progcheck.
- Test programs: `exec /bin/NAME` at the kernel shell (ring3check, shell_test.sh). halfdrawn is started by name from the desktop terminal (tearcheck).
- The desktop terminal also serves as the harnesses' program launcher: tearcheck (`halfdrawn`), findcheck, browsercheck, formcheck and livecheck (`browser http://…`), appcheck (`calc`, `monitor`, `run count &`, `music /usb/TONE.WAV`), gamecheck (`poker`).

### 5.2 System calls used

- **term.c:**
  - windows: `win_create`, `win_surface`, `win_width`, `win_height`, `win_allow_resize`, `win_poll`, `win_commit`, `win_close`, `win_set_text`
  - clipboard: `clip_set`, `clip_get`
  - files: `getcwd`, `chdir`, `stat`, `readdir`, `open`, `fread`, `fwrite`, `close`, `unlink`, `mkdir`, `rmdir` (and `slurp`/`spit` built on them)
  - processes: `spawn`, `spawnv`, `wait_for`, `kill`, `tasks`, `getpid`, `ticks`, `sleep_ms`
  - machine: `sysinfo`, `netinfo`, `resolve`, `power_off`, `power_reboot`
  - network: `connect`, `connect_tls`, `tls_why`/`tls_what` (SYS_TLS_STATUS), `send`, `recv`, `disconnect`
  - console: `puts` (only for the two startup error lines)
- **sh.c:** `fork`, `execv`, `dup2`, `pipe`, `open`, `close`, `wait_for`, `signal`, `chdir`, `getcwd`, `fread`, `fwrite`, `exit`, `puts`, `putn`.
- **Headers:** `sdk/zelr.h` (structs `zelr_stat` {size, is_dir, name[64]}, `zelr_task` {pid, state, slices, idle, user, name[64]}, `zelr_sysinfo`, `zelr_netinfo`, `win_event` {type, x, y, buttons, key}, `pollfd_t`, `KEY_*`, `WIN_EV_*`, `O_*`, `SIG*`, `NET_EOF`), `userland/draw.h` (`surface`, `fill`, `rect`, `mix`, `face_draw`, `MONO_W`, `MONO_H`, `UI_FACE_MONO`), `userland/face.h`, `userland/args.h`, `userland/alloc.h`.

### 5.3 Files these programs touch

| Path | Who | How |
|---|---|---|
| `/cfg/history` | term | read at startup, rewritten whole on close |
| `/cfg/term` | term | read at startup, written by `theme NAME` |
| `/zelr.cfg` | term | reads at most 511 bytes looking for the `light` key. `write` and `append` let the user edit it, and deskcheck does. |
| `/sys/*` | term (`sys`, `watch`, `cat`) | read |
| `/bin`, `/usb`, cwd | term (search and completion) | stat and readdir |
| `/home/argvtest.run` | argvtest | a guard marker, created and then removed |
| `/home/dur-*.txt`, `/cfg/dur-a.txt` | durtest | created, renamed and removed |
| `/fdt.txt`, `/fdt2.txt` | fdtest | created and removed |

### 5.4 Kernel behaviour these programs depend on

- fds 0, 1 and 2 are the console (`fd.c:158-164`). `console_read` edits a line, echoes, treats ^D as EOF, and turns ^C into `"\n"` (`fd.c:214-266`).
- `SYS_PUTC` and `SYS_WRITE` go to fd 1 (`syscall.c:506-525`).
- A spawned task gets a fresh fd table and inherits the cwd (`sched.c:191-194`, `sched.c:170-174`). A forked task clones the fd table (`sched.c:291`).
- An exception ends the program with 139 (`idt.c:142`). A default signal ends it with 128+sig (`signal.c:124,197`). `SYS_KILL` gives -1 (`syscall.c:332`). `task_wait` on an unknown pid returns -1 (`sched.c:380`).
- WM key routing: the top window (`wm.c:3968-3988`); new windows open on top (`wm.c:554`); Escape leaves the desktop (`wm.c:3953`).
- The window server clamps sizes to 32..1600 × 32..1200 (`winsrv.c:157`) and cascades placement by `handle % 5` (`winsrv.c:195-203`).

---

## 6. Concurrency, locking, memory ownership, invariants

- **Every program here is single-threaded.** There are no threads in zelr processes (`alloc.h:19-20`). The terminal's UI thread does everything, so any blocking call freezes the window: foreground children, `get`, `watch`, and deep `find` or `tree`.
- **The window event queue** holds 32 slots (31 usable) per window (`include/wm.h:60`). Mouse moves are coalesced, and a full queue drops new events (`wm.c:597-642`). Keys typed while the terminal is blocked either queue up or are dropped.
- **Shared buffer `io[16384]`.** read_all, get, history_save and history_load all use it. There is no concurrency inside the process, so this is safe, but a command that holds `io` contents (e.g. `grep`, which scans io) must not call anything that reuses it mid-scan. Today none does.
- **Invariants:**
  - `0 <= n_lines <= 400`; the logical line i lives at `(first+i)%400`.
  - Every stored line is at most `cols`-at-print-time ≤ 160 characters and null-terminated.
  - `0 <= in_pos <= in_len <= 160`, and `input[in_len]==0`.
  - `0 <= hist_at <= n_hist <= 64`.
  - `view <= max(0, n_lines-(rows-1))`, except briefly after a resize before the next clamp. Draw clamps `start` and `end` anyway.
- **The selection is logical line indices.** They would shift if lines were pushed while a selection exists and the ring is full. In practice pressing Enter clears the selection before output, so this does not happen.
- **sh fd hygiene.**
  - The parent closes every pipe end it hands to a child, so readers see EOF (`sh.c:280-288`), and resets its variables to -1.
  - Children close the ends they do not use.
  - Leak: if `pipe()` succeeds and `fork()` fails, the new ends are never closed (`sh.c:231-237`).
- **sh signal invariant.** sh itself ignores SIGINT; every child is `SIG_DFL` before exec.
- **The clipboard** is one kernel buffer shared by all programs (`zelr.h:766-782`). The terminal copies whole strings with `clip_set`.
- **Multiple terminals.** There is no locking around `/cfg/history` or `/cfg/term`, so the last writer wins.
- **Test programs:**
  - cowtest and maptest depend on nothing else allocating memory heavily during their measurement windows; the thresholds are wide.
  - sigtest's register check depends on a scheduling race: churn must still be running 30 ms after the fork.
  - polltest and sleeptest depend on the 100 Hz tick; their elapsed times are multiples of 10 ms.

---

## 7. Limits and magic numbers

| Value | Where | Meaning |
|---|---|---|
| 160 | `term.c:20` COLS | Maximum line, input and history width |
| 400 | `term.c:21` | Scrollback lines |
| 8 px | `term.c:22` PAD | Margin |
| 64 | `term.c:23` | History entries |
| 128 / 32 | `term.c:27-28` | Path and name buffers (the name limit is shorter than the kernel's 64) |
| 16384 | `term.c:32` IO_BUF | File and net buffer; 16383 bytes usable |
| 9 × 18 px | `draw.h:152,157`, `face.h:2690,3325` | Monospace cell |
| 760 × 480 | `term.c:1783` | Default window, giving 82 × 25 cells |
| 20 ms | `term.c:1873` | Loop sleep |
| 12 | `term.c:382,1867` | Blink period in loop iterations (about 240 ms or more) |
| 3 | `term.c:1835` | Lines per wheel step |
| rows/2 | `term.c:1748,1753` | PageUp/PageDown step |
| 60 lines / 4096 B | `term.c:307-309` | Find text published (`WM_TEXT_MAX` 4096) |
| 6400 B | `term.c:267` | Copy buffer (COLS×40) |
| 161 B | `term.c:285-286` | Paste read (stops at the first newline) |
| 32 × 32 | `term.c:1509` | Completion candidates; names cut to 29 characters |
| 16 words | `term.c:1435-1436` | split limit; programs get at most 15 words |
| 6 / 200 | `term.c:523,568` | tree and find depth; find match cap |
| 5 / 20 / 500 ms | `term.c:976-988` | watch default count, cap, interval |
| 256 | `term.c:735` | hex default bytes |
| 10 | `term.c:662,667` | head and tail default |
| 40 | `term.c:1034` | mem bar width |
| 512 B, 3 empty reads, ports 443/80 | `term.c:1134-1151` | get request buffer and receive loop |
| 511 B / 31 B | `term.c:1207-1208`, `1222-1223` | Bytes read from /zelr.cfg and /cfg/term |
| `/home` | `term.c:1275` | terminal `cd` default (sh's is `/`) |
| 512 / 32 (31) / 4 | `sh.c:31-33` | sh line, words, stages |
| 160 | `sh.c:199` | sh exec path buffer |
| 127 | `sh.c:210` | sh "not found" exit status |
| 4096 | `cat.c:10`, `wc.c:9` | I/O chunk |
| 1024 / 2048 | `grep.c:10-13` | grep line and chunk |
| 5 × 700 ms | `count.c:10-16` | count run time (about 3.5 s) |
| 139 | `faulttest.c:35`, `maptest.c:47`, `idt.c:142` | Status after a fault |
| 128+15 = 143 | `sigtest.c:178` | Status after SIGTERM |
| 0x5A4C5200 | `alloctest.c:38` | xorshift seed |
| 400 slots, 1200 rounds, < 1 MiB | `alloctest.c:46,113,143` | Churn parameters and threshold |
| 1024 pages; < 512 KiB; > 3000 KiB; > 2000 KiB | `cowtest.c:48,83,95,116` | COW thresholds |
| 64 MiB; < 256 KiB; 64..511 KiB; ≥ 48 KiB; ≤ 16 maps | `maptest.c:38-138` | mmap thresholds (`VMA_MAX` 16) |
| 150 / 60 / 2000 / 200 ms; 24 > 16 | `polltest.c` | poll timing; `POLL_MAX` 16 |
| 90-599 ms; ≥100; ≥450; ≥300; < 200 | `sleeptest.c` | Sleep thresholds |
| 3000-byte word | `argvtest.c:130` | Must exceed `ARGV_BYTES` 2048 (`syscall.c:126`) |
| 0xC81E1E / 0x1E9632 / 0x1EC864, 200 ms | `halfdrawn.c:26-28,48,55` | Tear-test colours and timing |
| 64 × 48, 3072 px | `wintest.c:17,35` | Window test |
| 0x0000008060000000 | `shell_test.sh:115`, `winsrv.h:19` | First window surface in a process |
| 48 (46 used) | `sysfs.h:30`, `builtin.c:70-117` | /bin program slots |
| 100 Hz | `kernel/main.c:459` | Tick rate: ticks×10 = ms |

---

## 8. Tests

| Harness | Gate mode | What it drives | What it asserts about this area |
|---|---|---|---|
| `tools/ring3check.py` | fast (`gate.sh:302-303`) | `exec /bin/NAME` at the kernel shell | The `*_PASS` markers of alloctest, argvtest, cowtest, durtest, faulttest, fdtest, forktest, fptest, maptest, polltest, sigtest and sleeptest (plus jstest, pagetest, pngtest, jpegtest, svgtest, layouttest and cardtest from other areas) |
| `tools/shell_test.sh` | fast (`gate.sh:332-333`) | Serial typing at the kernel shell, `-append console`, 64 MiB | `exec /bin/hello` gives "hello from a program". `exec /bin/wintest` gives the surface address, 3072 pixels, "closed, handle is dead" and "wintest: ok". |
| `tools/termcheck.py` | screen/full (`gate.sh:410-411`) | QEMU sendkey into the desktop terminal | Default slate, typing, tab completion (auto-space), history Up, Left insert, Backspace, Home+Delete, survival after `cat /cfg/term` (3.6) |
| `tools/shcheck.py` | screen/full (`gate.sh:460,597`) | `exec /bin/sh`, then serial typing | Banner "zelr shell". `echo hello`. `nosuchprogram` gives "not found". `echo one > /t.txt`, then `cat`. `echo two >> /t.txt`, then `wc` gives 2 lines and 2 words. The shell still prints to the screen. `wc < /t.txt`. `cat /t.txt | grep two` passes "two" and filters out "one". `ls /bin | wc` gives at least 15 lines. `ls /bin | grep sh | wc` gives at least 1. `spin`, then ctrl-C (byte 3) brings the prompt back, `echo still here` works, and `ps | grep spin` shows nothing. `pwd` gives `/home`. `cd /bin`, `pwd` ends in `/bin`, `ls | grep echo`. `exit` returns to the kernel prompt. |
| `tools/progcheck.py` | screen/full (`gate.sh:540,591`) | The kernel shell | Bare-name execution from cwd, the refusal message, a non-ELF file refused by name (3.6). **It does not test term.c.** |
| `tools/tearcheck.py` | screen/full (`gate.sh:535,590`) | Serial typing of `halfdrawn` into the desktop terminal | No red in 14 samples, both greens seen, at least 4000 green pixels |
| `tools/clipcheck.py` | full only (`gate.sh:372-373`) | sendkey into the terminal | ctrl-C copies the input line, ctrl-V pastes into `echo`, a decoy copy, ctrl-A plus ctrl-C copies the whole scrollback. Then `/sys/clipboard` is read on the console: it must contain the marker and more than three lines. |
| `tools/deskcheck.py` | screen/full | The desktop, with the terminal as its subject | The terminal opens at 760×480 (between 300000 and 380000 slate pixels). Minimise and restore. **Maximise: "the program redrew into the space it was given"** (the `WIN_EV_RESIZE` path). Corner resize. `write /zelr.cfg wallpaper 4` and `width 800` typed in the terminal. `help`, then the wheel scrolls the terminal and scrolling back restores it (`deskcheck.py:525-556`). |
| `tools/shotcheck.py` | screen/full | The desktop | "a ring 3 terminal drew its window" and "the terminal has a dark page" (more than 100000 slate pixels) |
| `tools/findcheck.py`, `appcheck.py`, `browsercheck.py`, `formcheck.py`, `livecheck.py`, `gamecheck.py` | screen/full | Serial typing of program names into the terminal | They use the terminal as a launcher (bare-name search reaching /bin, argument passing). None asserts terminal behaviour directly. |
| kernel selftest | `-append selftest` | | Nothing in this area runs. `test_pins` uses `/bin/term` and `/bin/count` as paths (`selftest.c:998-1036`). `test_builtin` checks /bin registration (`selftest.c:878-903`). |

**Not tested anywhere:**
- spawntest.
- The terminal's program search order (cwd over `/usb` over `/bin`) and `&`.
- PageUp and PageDown.
- `get`, `tree`, `find`, `hex`, `cp`, `mv`, `rm`, `stat`, `kill` and `time`.
- History persistence in `/cfg/history` across restarts, and theme persistence across restarts (termcheck reads `/cfg/term` but does not check its contents).
- Mouse drag selection.
- Paste truncation at a newline.
- Completion of paths and directories.
- sh's `jobs`, background jobs and quoting.

---

## 9. How to extend

- **A new terminal command.** Write `static void cmd_x(int argc, char **argv)` and add one row to `COMMANDS[]` (`term.c:1315-1354`); `help` and Tab pick it up. Pitfalls:
  1. A name that matches a `/bin` program shadows that program.
  2. Arguments are split on spaces only, with no quotes, at most 16 words.
  3. Print with `say`, `dim`, `good` and `err`, building lines with the `w_*` helpers. Lines are capped at 160 characters and wrapped at the current width.
  4. `read_all` stops at 16383 bytes. Stream (open/fread) anything larger, especially when copying.
  5. Anything that blocks freezes the window. Repaint with `draw_all(); win_commit(win);` (as `get` and `watch` do), or restructure.
  6. Update README.md:908's command count.
- **Showing program output in the terminal** is the largest missing feature. `spawnv` cannot do it, because spawned tasks get fresh console descriptors (`sched.c:191-194`). The path is sh's: `pipe`, `fork`, `dup2(w,1)`/`dup2(w,2)` in the child, then `execv`. The parent would close the write end and feed the read end into `print()`. That needs a loop that does not block in `wait_for`: poll the pipe with `poll()` (POLLIN, and POLLHUP at EOF, as polltest shows) with a short timeout, interleaved with `win_poll`. There is no WNOHANG wait, so poll `tasks()` or treat POLLHUP as the end and call `wait_for` afterwards. Once programs print, decide how control bytes are handled; `print()` currently turns them into `.`.
- **ctrl-C in the terminal.** With a non-blocking loop, the terminal could `send_signal(pid, SIGINT)` when ctrl-C arrives while a foreground child runs, and keep copy when nothing is running. Note that the keyboard driver already raises `signal_interrupt()` for the console reader (`keyboard.c:44-58`).
- **The program search** lives in `PROG_PATH[]` (`term.c:1401`), the kernel's `run_by_name` (`shell.c:257-290`) and sh's `become()` (`sh.c:198-211`, /bin only). Keep all three consistent. Consider requiring an ELF header before accepting a candidate: the seeded `/home/notes` shadows `/bin/notes`.
- **sh.** For `;` or `&&`, split the line before `parse`. For operators that respect quotes, tokenise first (args_split) and cut on `|` tokens afterwards. For `jobs`, keep the background pids and reap them with `wait_for` or poll `tasks()`. For `2>`, add a case in `take_redirections`. Keep resetting SIGINT in children; the comment at `sh.c:244-254` explains why.
- **A new ring 3 test program.**
  1. Write `userland/NAME.c` with the `ok()` pattern and a unique `NAME_PASS`/`NAME_FAIL` marker.
  2. Add an `.incbin` block to `kernel/builtin.S` and an extern plus a row to `kernel/builtin.c`. Only 2 of the 48 slots are free; raise `SYSFS_MAX_PROGRAMS` in `include/sysfs.h:30`, and the `_Static_assert` will stop the build if you forget.
  3. Add a `SUITES` row to `tools/ring3check.py` with a timeout.

  Lessons the comments record: stop the compiler from folding away undefined behaviour with volatile variables (`faulttest.c:37-56`); measure against the clock or `sysinfo` rather than trusting the kernel's own answer (`sleeptest.c:15-18`, `cowtest.c:14-19`); hold allocations at the same time to force splits (`alloctest.c:89-95`); print negative values from a copy (`fptest.c:32-35`); leave a guard file before self-spawning (`argvtest.c:23-28`).
- **ABI pitfalls.** `zelr_task` must match the kernel's struct size (`zelr.h:504-507`, `tools/abicheck.py`), and `zelr_stat.name` must be as wide as the kernel writes, 64 (`README.md:1471-1480`).

---

## 10. Doc drift and suspicious code

Everything here was verified by reading, with the file:line evidence given.

### 10.1 Doc drift

1. **README.md:908** says the terminal has "thirty-six commands of its own". `COMMANDS[]` has **38** (`term.c:1315-1354`).
2. **README.md:1005-1008** says the terminal "passes one [string] too, so `music /usb/tone.wav` and `notes readme` do what they look like". Two problems:
   - It passes a whole vector now (`term.c:1446-1453`).
   - In the default cwd `/home`, `notes` resolves first to the **seeded text file `/home/notes`** (`kernel/layout.c:137-140`), because `find_program` checks cwd first and only stats (`term.c:1418-1428`). The result is "cannot start /home/notes", and the editor never starts by bare name from `/home` unless that file is deleted. The kernel shell behaves the same way, and progcheck asserts it for a file named `lump`.
3. **README.md:942-945 and README.md:1711** say there is "no floating point anywhere in this system", that a program using a double "would fault", and that calc works in millionths "because there is no fpu". In fact `kernel/fpu.c:40-66` enables SSE and saves and restores FPU state per task; `fptest.c` computes with doubles and ring3check requires `FPTEST_PASS` in the fast gate.
4. **README.md:1749-1752**: "a terminal, paint, settings and three small tests" and "the four test harnesses". There are 46 embedded programs, 15 of them test programs in this area alone, and dozens of harnesses.
5. **term.c:25-28**: VFS_NAME 32 is described as "what the kernel will accept". The kernel accepts 64 (`include/fat.h:9`, `include/vfs.h:18`). Tab completion truncates names longer than 29 characters (`term.c:1596-1597,1514`). With a single match it then inserts the truncated name **plus a space**, which is a wrong path.
6. **term.c:30-31**: "Anything larger is printed in full but never held in one piece." Not true: `read_all` stops at 16383 bytes (`term.c:611-617`). cat, head, tail, wc, grep, hex, sys, watch, cp, mv and get all silently see only the first 16383 bytes.
7. **term.c:1710**: "select everything on screen". It selects the whole scrollback (`term.c:1712-1715`), and the copy then truncates at 6399 bytes (`term.c:267`).
8. **term.c:304-305 and README.md:816**: find sees "the lines you can still see". What is published is always the last 60 lines (`term.c:309`), whatever the window height (24 rows by default) and whatever the scroll position.
9. **term.c:1794-1796**: "a new size is only a different number of rows". Lines are wrapped when printed, at the width in force then (`term.c:157`). Widening does not re-join them, and narrowing clips (`face_draw_clip`) instead of rewrapping.
10. **sh.c:22-25**: sh "passes one argument string to a program rather than a vector". Stale: `become()` passes the vector to `execv`, and its own comment says so (`sh.c:191-207`). **sh.c:8-12 and README.md:696-697** ("knows three system calls and nothing else") are rhetorical; it also uses pipe, open, close, wait_for, signal, chdir, getcwd, fread, fwrite and exit.
11. **hello.c:7** says it is "loaded from an ELF file on the FAT16 disk". /bin/hello is served from the kernel image (`kernel/builtin.c:1-9`), and the disk may be FAT32.
12. **ring3check.py:1-15**: the docstring says "The six things a program can now do", then "All five", then "One machine for all five". SUITES has 19 entries.
13. **termcheck.py:135-142** is labelled "the theme is remembered", but it only checks that the window is still amber after `cat /cfg/term`. It checks neither the file's contents nor persistence across a restart.
14. **deskcheck.py:177,181** computes chip positions for a window called "zelr terminal". term.c names its window **"terminal"** (`term.c:1783`), and chips are drawn from `w->title` (`wm.c:549`, `wm.c:2720-2750`). The test still works because the computed point happens to fall inside the real chip.
15. **kernel/selftest.c:889-892** claims to check "the last one in the list by name" using `/bin/browser`. The last registered program is `polltest` (`builtin.c:116`). The count comparison at `selftest.c:886-887` still covers what it is meant to catch.
16. **pipeline/backlog.md:19,22,24** lists "pipes" as blocked, "editor" as todo and "demand-pages" as todo. All three exist: fdtest and polltest exercise pipes, notes.c is the editor, and maptest exercises demand paging.
17. **sdk/zelr.h:429-433** says of `connect_tls`: "Zero means the connection is open". The kernel returns a socket handle (`syscall.c:779-808`). term.c already treats the value as a handle, correctly.
18. Minor: "sixty lines" of history (`term.c:1476`) against MAX_HIST 64.

### 10.2 Suspicious code and likely bugs in term.c

1. **Data loss in `mv`, truncation in `cp`** (`term.c:811-840`). `copy_file` copies at most 16383 bytes and prints the truncated count as success. `mv` then `unlink`s the source, so moving any file larger than 16 KiB destroys the rest of it. `mv` also does not use `rename()`, which durtest shows exists.
2. **Tab expansion is off by one** (`term.c:164-167`). Tab stops land on columns 1, 5, 9…: a tab at column 0 gives one space and a tab at column 3 gives two.
3. **Out-of-bounds read in `head_tail`** (`term.c:639`). With an empty file (total 0) it reads `io[-1]`; `head` and `tail` of an empty file then print one blank line. `cmd_wc` guards the same test (`term.c:683`); `head_tail` does not.
4. **`ls` on a missing path says "(empty)"** (`term.c:488-512`). `stat` fails, the readdir error −1 is treated as "end of directory", and no error is printed.
5. **Duplicate completion candidates** (`term.c:1564-1569`). ls, cat, echo, grep, wc and ps come from both `COMMANDS` and `/bin`. Completing them never appends a space, `ls`+Tab lists "ls  ls", and the 32-candidate cap means a Tab on an empty first word shows only the first 32 table commands and never any /bin programs.
6. **The automatic space goes at the end of the line** (`term.c:1616-1619`). It is appended at `in_len`, not after the completed word, and the cursor moves to the end of the line. That is wrong when completing in the middle of a line.
7. **Input silently capped at 160 characters** (`term.c:1673`). This is the same silent-truncation failure the comment at `term.c:1665-1672` describes, just moved from 80 to 160.
8. **More than 16 words** (`term.c:440-449,1450-1452`). `split` stops without terminating the 16th word, so `argv[15]` holds the rest of the line. Programs are then capped at 15 words and lose the 16th and everything after it; built-ins rejoin them. Background detection only looks at the first character of the last word, so `&x` also counts (`term.c:1444`).
9. **A foreground child freezes the terminal** (`term.c:925-926`, section 3.1.10). There is no way to interrupt it from that terminal (ctrl-C copies), and meanwhile close events and keys queue or are dropped.
10. **A negative status prints as u32** (`term.c:931-932`). A child ended by SYS_KILL (status −1) shows as "exited with 4294967295".
11. **`kill` can end any task** (`term.c:903-910`). `sys_kill` checks only "not myself" (`syscall.c:318-334`), and `signal_end_task` has no ring check (`signal.c:29-45`). `ps` lists ring-0 tasks, so the terminal (or the Monitor) appears able to end kernel tasks, including the one running the compositor. Not verified at runtime.
12. **History is only saved on close** (`term.c:1815-1818`). `shutdown` and `reboot` typed in the terminal skip `history_save` (`term.c:464-478`), and with several terminals the last to close overwrites the others.
13. **`run` is narrower than typing the name** (`term.c:941-952`). It searches only `/bin`, passes no arguments, and `run ./x` builds `/bin/./x`.
14. **A first word with a slash is never completed** (`term.c:1563`): it is matched only against command and /bin names.
15. **`stat`'s "kept" line trusts the typed prefix** (`term.c:861-863`). Relative paths are misclassified, and `/system...` would count as `/sys`.
16. **`get` limits** (`term.c:1099-1159`):
    - no path inside HOST and no port
    - the request is silently truncated at 512 bytes
    - redirects are not followed
    - with no CRLFCRLF the whole response is saved as the body
17. **`WIN_EV_FIND` is ignored** (event loop at `term.c:1814-1864`). README.md:817-818 says programs paint behind the found word; the terminal does not.
18. **Stdin readers fight the desktop.** A program started from the terminal that reads stdin (sh; cat, wc or grep with no files) reads the **kernel console** and competes with `wm_run` for `kbd_trygetchar` (`fd.c:226` against `wm.c:3952`). Its output also goes to the console, not the window.
19. Dead code: `w_str(st.is_dir ? "" : "");` (`term.c:530`).
20. `cmd_clear` leaves any selection active, holding stale line indices (`term.c:1265-1266`).

### 10.3 Suspicious code and likely bugs in sh and the coreutils

1. **Pipe ends leak if fork fails** (`sh.c:231-237`). If `pipe()` succeeded and `fork()` fails, `ends[0]` and `ends[1]` are never closed.
2. **Quotes do not protect operators** (`sh.c:113-116,125`).
   - `|` is cut before quotes are handled, so `echo "a | b"` becomes two broken stages.
   - Quotes are removed before `take_redirections` runs (`args.h:27-31`), so `echo ">"` is treated as a redirection.
3. **Search and `cd` differ from the terminal.** sh looks only in `/bin` (`sh.c:200-205`), and `cd` with no argument goes to `/` (`sh.c:156`). The terminal's `cd` goes to `/home` (`term.c:1275`).
4. **Background jobs are never reaped**, and `jobs` is a stub (`sh.c:180-185,295-299`). Exit statuses are never shown.
5. `sh.c:28` and `fdtest.c:15` include `alloc.h` without using it.
6. **grep drops characters on long lines** (`grep.c:37-47`). The byte at each 1023-character cut is dropped, not carried into the next piece.
7. **Coreutils print errors on fd 1** (`cat.c:40-42`, `grep.c:66,79-81`, `ls.c:16-18`, `wc.c:58-60`). The messages go down pipes and into files, against the rule stated at `sh.c:52-53`.
8. **Built-in and /bin `wc` disagree**: `/bin/wc` does not count an unterminated last line or treat `\r` as a separator, and the terminal's `wc` does both. `/bin/ls` on a regular file says "cannot read"; the terminal's `ls` prints the file.

### 10.4 Suspicious code and likely bugs in the tests

1. **polltest.c:46-47 calls `pipe()` twice.** The check `if (pipe(ends) != 0 && ends[0] < 0)` creates a second pipe as a side effect. The first pipe's two fds leak (and are inherited by the child). The results are not affected.
2. **alloctest.c:133 may never record `after_first`.** It is only set if round 100 is an allocation; a free `continue`s past it. The RNG is deterministic, so the result is fixed per build and presumably passes, but it is fragile if N, the seed or the size rules change.
3. **sigtest.c:126-139 does not prove the signal landed mid-churn.** If the child's SIGTERM arrives after churn finishes, it is delivered on `wait_for`'s return, `caught>=1` still holds and the answers match trivially.
4. **argvtest.c's guard file can get stuck** (lines 100-107, 137). If the parent dies after creating `/home/argvtest.run`, every later run fails at once with "a child was started on a vector and got none of it" until the file is deleted.
5. **cowtest.c:100** prints the PASS line "the parent kept its own writes" only when nothing failed earlier; a mismatch is still reported by the loop.
6. **ring3check.py:63,87-95.** `out` is the whole transcript. If the second jstest run printed nothing, the case-count parse would quietly reuse the first run's line.
7. **appcheck.py:219-233** (outside my file list, but it concerns the terminal as launcher). After `monitor⏎` is typed into the terminal, the Monitor window opens on top (`wm.c:554`) and the terminal blocks in `wait_for`. The next input, `run count &⏎`, therefore goes to the Monitor, which feeds keys only to its toolkit and has no text input (`monitor.c:208-211`), not to the terminal. The check "the monitor notices another program starting" compares a screen patch, so it can pass just because the Monitor's graph changes over time, without count ever starting.
8. **spawntest.c is never run** by any harness or the gate.

---

## 11. Open questions

1. **Where does console output from programs started in the desktop terminal appear?** `kputc` calls `fbcon_putc`, which draws straight to the framebuffer (`printf.c:16-24`, `fbcon.c:96-121`). Does it show over the desktop until the next composite, or is it hidden? This needs a runtime check, since it decides what users see when they run `hello` from the terminal.
2. **Can the terminal's `kill` (SYS_KILL) end the kernel shell or compositor task, or other kernel tasks?** If it can, what happens? The code has no guard (10.2 item 11).
3. **Are background tasks ever reclaimed?** The terminal's `&` and sh's `&` never call `wait_for` on them. The answer depends on the kernel's reaping policy (`t->reaped`, `died_at`); see area 03.
4. **Who wins a keystroke** when a program started from the desktop terminal reads the console while `wm_run` also polls `kbd_trygetchar`?
5. **Does the WM do anything visible for the terminal on ctrl+F** (for example raise it or report a match count) given that the terminal ignores `WIN_EV_FIND`?
6. **Why does the README say 36 commands?** Perhaps `shutdown`/`reboot` (placed oddly, third and fourth in the table) were added later. There is no git history here to check.
7. **Does a USB keyboard's ctrl-C raise SIGINT?** `keyboard_inject` (`keyboard.c:174-177`) does not call `interrupting()`, unlike the PS/2 `push()`.
8. **Is `/bin/sh` meant to be usable from the desktop terminal?** Today it would read and write the kernel console. The design suggests sh is console-only and the terminal is the desktop shell, but nothing says so explicitly.
9. **Is wintest's `win_width(5)` a meaningful "foreign window" test?** In console mode (shell_test.sh) slot 5 is simply unused, so it only tests an empty slot, not another program's window.
