# 08 - Kernel entry (kmain), boot sequence, kernel shell, first run, selftest

Source tree: the repository root (zelr 0.37.0 + 2 commits, `KERNEL_VERSION "0.37.0"` at include/types.h:25).
All line numbers are from that tree. "Verified in the real log" refers to
a local selftest log (a real `bash run.sh -T` run: QEMU i440fx, `-m 64`, one CPU, rtl8139, fresh 16 MiB IDE `zelr.img`, result `556 passed, 0 failed`, `SELFTEST_PASS`).

---

## 1. Scope

| File | Lines | Role |
|---|---|---|
| kernel/main.c | 647 | `kmain_multiboot` (builds a `handoff_t` from multiboot info) and `kmain` (the whole boot sequence, heap sizing, device surveys, task creation, hand-over to the scheduler) |
| kernel/shell.c | 631 | The kernel's own console shell (`shell_task`): line input from PS/2/USB keyboard or serial, 39 command words, run-by-name program lookup, auto-start of the desktop |
| include/shell.h | 7 | `shell_task`, `shell_console_only` |
| kernel/welcome.c | 97 | First-run text (`welcome_print`) and the `guide` tour (`guide_print`) |
| include/welcome.h | 3 | `welcome_print`, `guide_print` |
| kernel/selftest.c | 3605 | Boot-time self test: 53 test functions in 50 printed sections, `ok()` counting, `SELFTEST_PASS/FAIL` |
| include/selftest.h | 3 | `int selftest_run(void)` (returns failures) |
| tools/shell_test.sh | 123 | Bash harness: types 20 lines at the kernel shell over serial, 17 grep checks |
| tools/shcheck.py | 233 | Python harness: starts the ring 3 `/bin/sh` from the kernel shell and checks redirection, pipes, ^C, built-ins, exit (19 checks) |

Cross-referenced (not owned) to explain behaviour: kernel/blackbox.c (boot-log marks, panic screen), kernel/printf.c (`panic`, `kputc`), kernel/idt.c (fault path, kernel lock at the ISR door), kernel/sched.c (tasks, idle, reaping, stack paint), kernel/smp.c, kernel/keyboard.c, kernel/wm.c (`wm_run`, Escape), kernel/net.c (DHCP task, net task), kernel/usb.c, kernel/layout.c, kernel/theme.c, kernel/vfs.c, kernel/fs.c, kernel/http.c, run.sh, pipeline/gate.sh, tools/harness.py, boot/boot.S, uefi/loader.c.

---

## 2. Big picture

**What the area is.** Everything between "a loader jumped into the kernel" and "the first task runs", plus the two things the first task can be: the kernel console shell (normal boot) or the self test (`selftest` on the kernel command line).

**Entry.** Three loaders, one door (`kmain(handoff_t *h)`):
- QEMU `-kernel` (multiboot a.out kludge in boot/boot.S) -> 32-bit trampoline -> long mode -> `kmain_multiboot(magic, mbi)` (boot/boot.S:230-232) which fills a static `handoff_t multiboot_handoff` and calls `kmain`.
- zelr's BIOS loader (bootloader/cdboot.S) -> `_start32` with the handoff in ebx -> `kmain(mb_info)` (boot/boot.S:235-237).
- zelr's UEFI loader (uefi/loader.c) -> `_start64` -> `kmain(rdi)` (boot/boot.S:245-251).
Both of zelr's own loaders zero the handoff (`zero_handoff` in cdboot.S:661; `mem_set(h,0,...)` uefi/loader.c:411), so **only a multiboot boot can carry a kernel command line**. Everything that depends on a command-line word (`selftest`, `console`) is therefore a QEMU/`-kernel`-only feature.

**kmain design.** A single linear function that brings up every subsystem in dependency order, bracketing each stage with `bb_mark("<phase>")` so the black-box boot log (serial mirror + disk record + panic screen) names the last phase reached. Interrupts are **off for the whole of kmain** (nothing in it executes `sti`; `sched_start` does, sched.c:603), so the tick counter never moves and every boot-log line is stamped `[    0]` (verified: real log lines 4-75). The kernel lock is first taken in `sched_start` (sched.c:600).

Main design decisions and the reasons the comments give:
- **Heap follows the image** (`heap_base` = `__kernel_end + BITMAP_ROOM` rounded up to 1 MiB, main.c:53-70): the old fixed 8 MiB base left the frame bitmap no room once the image (which embeds every user program) grew; the failure mode was "no memory" with no warning. `BITMAP_ROOM` = 2 MiB describes 64 GiB (= `KERNEL_SPACE_MAX_GB`).
- **Heap size = share of memory with a floor and a ceiling** (main.c:72-152): floor `HEAP_MIN` 24 MiB because the compositor's back buffer + comparison copy are the largest allocations and a failed allocation silently falls back to VGA text mode, which a UEFI machine does not have ("it boots, and the screen stays black, and nothing says why"); share `usable/4` because the old fixed size meant "giving zelr four gigabytes changed nothing whatsoever"; ceiling 512 MiB; never past the contiguous usable run it sits in (minus 1 MiB); above 64 MiB it must end on a 2 MiB boundary because that memory is mapped only in 2 MiB pages.
- **`sched_init` before `smp_init`** (main.c:479-486): each AP adopts its boot stack as its idle task and joins the task ring; `sched_init` clears the ring, so calling it afterwards discarded those tasks ("Nine thousand interrupts and not one task").
- **IRQ routing in one place, after every driver registered its handler and before interrupts are enabled** (main.c:583-588): routing a line whose vector has no handler would deliver an interrupt nothing acknowledges.
- **Service tasks created by kmain itself** (main.c:611-617): `usb_start_service` (hot-plug scan) and `net_start_service` (the stack must run even when nobody waits, since the NIC interrupt no longer runs it).
- **DHCP at boot, as a task, not in self-test mode** (main.c:623-636): an address used to require pressing a panel button; written beside the card it "silently created nothing" because there was no scheduler yet; not during selftest because the test runs its own exchange on the same socket.
- **The desktop opens by itself** (shell.c:598-608) "because that is what switching a computer on is supposed to do"; Escape leaves it; the `console` word (or the `autodesktop 0` setting) suppresses it so serial-driven tests get a prompt.
- **Self test in-kernel, exit status via QEMU isa-debug-exit** (main.c:156-161) so a script gets a real exit code.

**How it fits.** kmain is the only caller of nearly every subsystem `*_init`. The kernel shell is the fallback UI and the test harnesses' control channel (every tools/*.py harness types at it over serial). The self test is the kernel's regression suite, run by `run.sh -T`, `zelr.bat :test` and three `pipeline/gate.sh` configurations.

---

## 3. File-by-file detail

### 3.1 kernel/main.c

Includes nearly every subsystem header (main.c:1-49). `extern u8 __kernel_end[]` (main.c:51, linker symbol).

**Constants / globals**
| Name | Value | Line | Meaning |
|---|---|---|---|
| `BITMAP_ROOM` | `2ull*1024*1024` | 65 | room between image end and heap for the PMM bitmap (64 GiB of frames) |
| `HEAP_MIN` | 24 MiB | 81 | heap floor |
| `HEAP_SHARE` | 4 | 92 | heap = usable/4 |
| `HEAP_CEIL` | 512 MiB | 93 | heap ceiling |
| `heap_bytes` | init `HEAP_MIN` | 95 | final heap size |
| `want_selftest` | false | 154 | set by the `selftest` word |
| `multiboot_handoff` | static `handoff_t` | 246 | handoff for multiboot boots (boot.S stack is small, so not on the stack) |

**Functions**
- `static u64 heap_base(void)` (67-70): `(__kernel_end + BITMAP_ROOM + 0xFFFFF) & ~0xFFFFF`. Real log: `heap 24 MiB at 0x0000000001c00000` (28 MiB).
- `static u64 usable_total(const handoff_t *h)` (97-102): sum of `MEM_USABLE` region lengths.
- `static u64 usable_run_from(const handoff_t *h, u64 from)` (110-123): length of contiguous usable memory starting at `from`; regions may be unordered, so it repeats passes (max `HANDOFF_MAX_REGIONS`=128) extending `end` while some usable region satisfies `base <= end < base+len`. Returns 0 if `from` is not inside usable memory (then the heap would be 0 bytes).
- `static u64 heap_size_for(const handoff_t *h, u64 base)` (125-152): `screen = fb_pitch*4*fb_height` if a loader framebuffer exists (`fb_pitch` is **in pixels**, include/handoff.h:43), else 0; `need = HEAP_MIN + roundup_MiB(2*screen)`; `want = max(usable/4, need)`, capped at `HEAP_CEIL`, then at `run-1MiB`; if `base+want > KERNEL_LOW_MB (64) MiB` the end is rounded down to 2 MiB. Note a multiboot boot has `fb_base=0`, so `screen=0` and the 1024x768 buffers (6 MiB) must fit inside `HEAP_MIN`.
- `static void machine_exit(u32 code)` (158-161): `outl(0xF4, code); for(;;) hlt();`. QEMU's isa-debug-exit ends the VM with status `(code<<1)|1`.
- `static const char *pci_vendor_name(u16)` (175-188): 0x8086 intel, 0x168C atheros, 0x10EC realtek, 0x14E4 broadcom, 0x1814 ralink, 0x14C3 mediatek, 0x1969 qualcomm, 0x1AF4 virtio, 0x1022 amd, else "unknown". Reason (163-174): whether a wireless card can be driven at all depends on the vendor (most need vendor firmware; Atheros MACs are in hardware).
- `static void net_survey(void)` (190-215): lists up to 6 PCI class 02/00 (ethernet) then 02/80 (other network = wireless) devices; prints `  card    %s %04x:%04x at %d:%d.%d, %s` with note "ethernet" / "wireless, no firmware needed" (Atheros) / "wireless, needs vendor firmware"; `bb_log("card %04x:%04x %s")`.
- `static void banner(void)` (217-226): the 5-line box (`KERNEL_NAME`, `KERNEL_VERSION` padded `%-25s`, "x86-64 long mode", `(c) KERNEL_YEARS KERNEL_AUTHOR`, `KERNEL_LICENSE`), light cyan.
- `static void selftest_task(void)` (228-231): `failures = selftest_run(); machine_exit(failures ? 2 : 0);` (code 2, not the failure count).
- `static void init_task(void)` (233-237): `layout_init(); vfs_chdir(layout_home() /* "/home" */); shell_task();` - never returns.
- `void kmain_multiboot(u32 magic, u32 mbi_addr)` (250-299): zero the handoff, `magic = HANDOFF_MAGIC` (0x5A454C5236344846 "ZELR64HF"), `loader = "multiboot"`; if `magic != MULTIBOOT_BOOTLOADER_MAGIC` -> `serial_init(); panic(...)` (bb not yet initialised); copies `mbi->cmdline` if flags bit 2 (max 127 chars into `cmdline[128]`); converts the multiboot mmap (flags bit 6) entry by entry (`p += e->size + 4`) mapping type 1 -> `MEM_USABLE`, everything else -> `MEM_RESERVED` (ACPI-reclaimable becomes reserved); if no regions, fabricates one at 0x100000 of `(mem_upper+1024)*1024` bytes (see bug B5); forces `fb_base = 0` ("fb_init sets a mode itself through the VBE ports, which only exists on a machine that has a BIOS"); `rsdp` stays 0 (ACPI then searches low memory).
- `void kmain(handoff_t *h)` (301-647): the boot sequence, detailed step by step in section 4.2.

### 3.2 kernel/shell.c and include/shell.h

Header comment (1-3): reads "from the PS/2 keyboard or the serial line, whichever produces a character first, so it can be driven by a person at the console or by a script piping into QEMU". The actual merge is in `kbd_trygetchar` (keyboard.c:187-209): the keyboard ring (PS/2 and USB keyboards both inject there) first, then `serial_trygetc()` (IRQ4-buffered), with CR (13) converted to LF (10).

**Constants / state**
| Name | Value | Line |
|---|---|---|
| `LINE_MAX` | 256 (at most 254 typed chars accepted: `len < LINE_MAX - 2`) | 37 |
| `ARG_MAX` | 16 words | 38 |
| `static char line[LINE_MAX]` | the single input buffer, split in place | 40 |
| `static volatile u32 spawn_n` | incremented by `counter_task`, never read anywhere | 199 |
| `static bool console_only` | set by `shell_console_only()` | 591 |

**Helpers**
- `num(const char*)` (42-46): leading decimal digits only; non-digits give 0.
- `split(char *s, char **argv, u32 max)` (59-77): words separated by spaces only (not tabs); a word starting with `"` runs to the next `"` (quotes removed; unterminated quote runs to end of line); no escapes, no single quotes - deliberately identical to userland/args.h so both shells agree on where a word ends (comment 48-58).
- `join_from(argv, argc, start, out, cap)` (158-165): rejoins words with single spaces (so `echo a   b` prints `a b`, quotes are gone).
- `prompt()` (207-211): `zelr> ` at `/`, else `zelr:<cwd>> ` (harness regex `zelr(?::\S*)?> `, tools/harness.py:67).
- `enter_desktop()` (217-243): `vfs_slurp("/bin/term")`; if readable, `user_spawn_elf("/bin/term", ...)` (prints `desktop: term: <elf_error>` on failure; if the file cannot be read it silently continues with no terminal); then `wm_run()` (the compositor loop runs **inside this task**); on return `if (fb_active()) fbcon_clear(); vga_clear(); kprintf("back at the shell\n")`.
- `run_by_name(argc, argv)` (257-290): tries, in order, the name as typed (relative to cwd, or absolute), `/usb/<name>`, `/bin/<name>`. First readable file wins: `user_spawn_elf_argv(path, img, size, argc, argv)` (argv[0] = the word as typed, task name = the resolved path), then `task_wait(rc)`. A file that exists but is not a valid ELF prints `<path>: <elf_error>` and **stops the search** (comment 283-285). Returns false only if nothing was found.
- `counter_task()` (200-203): 3 x (`spawn_n++`, `task_sleep(400)`), `task_exit()`. Produces no output.
- `cmd_help`, `cmd_ls`, `cmd_cat`, `cmd_ps`, `cmd_mem`, `cmd_uptime`: see the command table.

**`execute(char *buf)` (292-589)**: `split`, then a strcmp chain; built-in names shadow /bin programs of the same name (`ls`, `cat`, `echo`, `ps` always run the kernel version; `/bin/ls` etc. must be typed as a path or via `exec`). Anything unknown goes to `run_by_name`; if that fails: `%s: not a command, and no program of that name here, on a stick or in /bin`.

**Every kernel shell command** (39 words; `help` lists 34 of them - not `mouse`, `beep`, `stick`, `shutdown`, `poweroff`, nor the run-by-name fallback):

| Command (line) | Arguments | What it does / prints | Subsystem called |
|---|---|---|---|
| `help` (298) | - | `cmd_help` (79-115): fixed list | printf |
| `guide` (299) | - | `guide_print()` tour | welcome.c |
| `desktop` (300-303) | - | "the desktop needs a framebuffer" if `!fb_active()`, else `enter_desktop()` | user.c, wm.c |
| `bg` (304-313) | `PROGRAM [ARG...]` | `vfs_slurp(argv[1])` (path relative to cwd, **no /bin search**), `user_spawn_elf_argv(argv[1], ..., argc-1, argv+1)`; prints `[pid] <prog> running in the background` or `bg: <prog>: <elf_error>` / `no such file`; never waited (record freed after `REAP_GRACE`) | vfs, user.c |
| `exec` (314-333) | `PROGRAM [ARG...]` | same load, then `task_wait(pid)` so output does not interleave with the prompt; usage text says `e.g. exec hello` (see drift D14) | vfs, user.c, sched |
| `ring3` (334-338) | - | `user_spawn_stub("ring3")`: the kernel/userstub.S stub prints `ring3\n` via 6 SYS_PUTC then SYS_EXIT; shell prints `started pid N in ring 3` | user.c |
| `mouse` (339-347) | - | `no mouse` or pointer x,y, buttons (left/right/middle), move count | mouse.c |
| `sync` (348-351) | - | `diskfs_flush()` -> `flushed to disk` / `sync: no disk` (writes are write-through; this flushes the drive cache) | diskfs, blockdev |
| `format` (352-354) | - | `diskfs_format()` with **no confirmation** -> `disk formatted` / `format failed` (diskfs refuses a partitioned disk, diskfs.c:93-97) | diskfs |
| `disk` (355-364) | - | model, sectors (and MiB), mounted state; if FAT mounted `format   FAT16, N clusters of B bytes` (hard-coded FAT16, bug D15) and free KiB | blockdev, fat |
| `net` (365-391) | - | no driven card: `card VVVV:DDDD on the bus, no driver for it` or `no network card`; else card name, MAC, address/netmask/gateway/dns or `address  none, run: dhcp`, packet counts | netdev, net |
| `dhcp` (392-398) | - | `asking for an address...`, `net_dhcp(6000)` directly -> `got a.b.c.d` / `no answer` (does not check `net_dhcp_busy()`, bug B2) | net |
| `ping` (399-411) | `ADDRESS` | needs an address; IP literal or `net_resolve(...,4000)`; 4 x `net_ping(target, 2000)` -> `reply from X: seq=n time=Nms` / `no reply from X: seq=n` | net |
| `fetch` (412-425) | `[https://]HOST [PATH] [SAVEAS]` | needs an address; `http_get(host, path?:"/", save)`; plain http unless `https://` (http.c:68-69); maps `HTTP_ERR_*` (-1 resolve, -2 connect, -6 too long, -7 TLS (already reported), -3 send, -5 empty, -4 memory) to messages; saving is broken (bug B1) | http.c, tls |
| `resolve` (426-433) | `HOSTNAME` | `net_resolve(...,4000)` -> `X is a.b.c.d` / `cannot resolve X` (fails immediately without a DNS server, net.c:727) | net |
| `ls` (434) | `[PATH]` | `cmd_ls` (117-143): `<dir>  name/` and `%10d  name` lines, summary `N file(s) in B bytes[, D director(y|ies)]` or `(empty)`; errors `ls: bad path`, `ls: X: not found`, `ls: X: not a directory` | vfs |
| `cat` (435-436) | `NAME` | `cmd_cat` (145-156): stat, `vfs_slurp`, raw bytes to console, trailing newline added if missing | vfs |
| `write` / `append` (437-445) | `NAME TEXT...` | joins the words + `\n`, `vfs_write`/`vfs_append` -> `ok`/`failed` | vfs |
| `cp` (446-468) | `SRC DST` | whole file through one heap buffer; `ok` or `cp: ...` errors | vfs, heap |
| `rm` (469-471) | `NAME` | `vfs_delete` -> `ok` / `rm: no such file` (any failure) | vfs |
| `cd` (472-474) | `[PATH]` | no argument goes to `/` (not /home); `cd: X: not a directory` | vfs |
| `pwd` (475-476) | - | `vfs_cwd()` | vfs |
| `mkdir` / `rmdir` (477-482) | `NAME` | `ok` / `mkdir: failed` / `rmdir: not empty, or not a directory` | vfs |
| `ps` (483) | - | `cmd_ps` (167-180): `  PID  STATE     SLICES  WAITING  NAME`, one row per task in the ring (state via `task_state_name`) | sched |
| `cpus` (484-497) | - | found/started, table `CPU APIC ROLE JOBS` (role "kernel" for cpu 0, "worker" otherwise, `(did not start)`), footer "the kernel runs on cpu 0; the others take work handed to them" (partly stale, D16) | smp |
| `mem` (498) | - | `cmd_mem` (182-190): physical KiB total/used/free, heap KiB/bytes, files (on disk KiB free, or RAM fs count/bytes), serial counters irqs/got/read/dropped | pmm, heap, vfs, fat, fs, serial |
| `uptime` (499) | - | `up S.cc seconds (T ticks at H Hz)` | timer |
| `uname` (500-503) | - | `zelr 0.37.0 x86_64` / `(c) 2026 blavese, GPL-3.0-or-later` | types.h |
| `clear` (504) | - | `vga_clear()` (which clears fbcon when a framebuffer is active, vga.c:29-30) | vga/fbcon |
| `echo` (505-508) | `TEXT...` | joined words | - |
| `spawn` (509-511) | - | `task_create("counter", counter_task)` -> `spawned pid N` | sched |
| `fault` (512-516) | - | `dividing by zero...` then a kernel-mode `1/0`. There is no vector-0 handler (registered handlers: 3 (selftest only), 32, 33, 36, 44, NIC IRQs, 0x80), so idt.c:207-236 runs `bb_fault` + `panic("unhandled exception 0 ...")`: **the machine stops** after painting the log | idt, blackbox, printf |
| `beep` (517-531) | `[HZ] [MS]` | default 440 Hz / 400 ms, clamps 20..20000 Hz, <=10000 ms; `sound_tone` (blocking); `no sound device` | sound |
| `stick` (532-570) | none / `read LBA` / `write LBA [BYTE]` | USB disk: model and size; raw read prints first 16 bytes (`%x`, unpadded); raw write fills one sector with BYTE (default 0), **no confirmation** | usbdisk |
| `shutdown`, `poweroff` (571-581) | - | `cannot power off: <power_describe()>` if no ACPI S5; else `powering off`, `diskfs_flush()`, `power_off()`, `the firmware did not take it` if it returns | power, diskfs |
| `reboot` (582-584) | - | `rebooting`, `power_reboot()` (8042 0xFE, then 0xCF9, then triple fault); **no flush** (tools/blackbox_test.sh relies on that) | power |
| *(anything else)* (585-588) | argv | `run_by_name` (cwd, /usb, /bin), else "not a command..." | vfs, user.c |

Commands that live in the **ring 3 terminal (`/bin/term`), not the kernel shell**: `tree`, `run`, `sys`, `get`, `theme` (userland/term.c:1320-1349), plus tab completion, history, PageUp. The kernel shell has **no history and no line editing beyond backspace**.

**`shell_console_only(void)` (593)**: sets `console_only`; called only from kmain's command-line scan (main.c:315).

**`shell_task(void)` (595-631)**:
1. `welcome_print()`.
2. `theme_init()` (reads `/zelr.cfg`); if `fb_active() && !console_only && theme()->autodesktop` (knob default 1, theme.c:125) -> `enter_desktop()`.
3. `prompt()`, then forever: `ch = kbd_trygetchar()`; if none, `task_idle_wait()` (halt until the next interrupt, counted as idle ticks); `c = (char)KEY_CODE(ch)` (modifier bits dropped: "The console has no shortcuts, so a chord is just its character"); `'\n'` -> echo newline, NUL-terminate, `execute(line)`, reset, prompt; `'\b'` -> delete one char and echo `\b`; printable 32..126 appended and echoed while `len < 254`; everything else ignored.

Input quirks (verified from code): special keys are truncated to their low byte, so `KEY_DELETE` (0x108, keyboard.h:18) becomes `'\b'` and acts as backspace, `KEY_INSERT` becomes TAB (ignored), F12 (0x11B) becomes ESC (ignored); serial DEL (0x7F, what most terminals send for backspace) is ignored; Ctrl-C (3) is ignored by the shell but is raised as an interrupt by the keyboard/serial driver (keyboard.c:44-48); a CRLF line ending executes the line and then an empty line (extra prompt).

### 3.3 kernel/welcome.c and include/welcome.h

- `void welcome_print(void)` (6-18): white "Welcome. You are talking to an operating system / that was written from scratch."; grey "There is no Windows underneath this..." (3 lines); green `Try typing:  guide`; grey `Or:          help    for the full command list`.
- `void guide_print(void)` (20-97): "A short tour" in five coloured sections:
  1. The desktop - `desktop`; drag/close/maximise/put away, resize corner, snap, Alt+Tab, Alt+arrow, Alt+D; "Pick a colour and drag on the white area to draw" (paint, stale: D18); "Escape returns you here."
  2. Files that stay put - `ls`, `cat /doc/readme`, `write notes hello`, `mkdir work`, `cd work`, `disk`; /home, /doc, /cfg, /tmp explained; "Files are written to a real disk."
  3. The internet - `dhcp`, `net`, `ping 10.0.2.2`, `resolve example.com`, `fetch example.com / page.html` "Then: cat page.html" (broken: B1).
  4. Running real programs - `exec /bin/hello`, `bg /bin/count`, `exec /bin/wintest`, `exec /bin/spawntest`, `ps`; programs are separate ring 3 executables living in the kernel image.
  5. The machine itself - `cat /sys/memory`, `/sys/tasks`, `/sys/devices`, `ls /sys`; `fault` "The CPU catches that last one and the kernel prints where it happened rather than just stopping" (misleading: D20).
  Closing line: "None of this can harm your real computer. It runs inside an emulator..." (stale since zelr boots real hardware: D21).
  There is no `tour` command; only `guide`.

First-run reality: on a machine with a framebuffer and `autodesktop` on, the welcome text is printed and then the desktop covers it immediately; when the user presses Escape, `enter_desktop` clears the console before printing "back at the shell" (shell.c:240-242), so the welcome/tips are never visible on screen (they remain in the serial log). Seeded first-run files come from layout.c: `/doc/readme`, `/doc/filesystem`, `/home/notes` (generation 1, marker `/cfg/seeded`).

### 3.4 kernel/selftest.c and include/selftest.h

**Framework.**
- `static int passed, failed;` (68); `static void ok(const char *what, bool cond)` (70-73) prints `  PASS  <what>` or `  FAIL  <what>` via `kprintf` (screen **and** serial) and counts.
- SKIPs are plain `kprintf("  SKIP  ...")` lines and are **not counted**.
- Extra informational lines (8-space indent): `fifty yields added N ticks` (only on failure, 348), `irq0 arrives on input N` (3292), `the play position moved ...` (3525), `the deepest this run went left N bytes of 32768 spare` (1724).
- `int selftest_run(void)` (3548-3605): resets counters, prints `\n=== zelr self test ===`, then 50 `[section]` headers calling 53 test functions in a fixed order, then `\n%d passed, %d failed` and `SELFTEST_PASS` or `SELFTEST_FAIL`; returns `failed`.
- It runs in the kernel task `"selftest"` on CPU 0, interrupts on, kernel lock held (kernel tasks are CPU 0 only, sched.c:423-427, 464). The task's working directory is `/` (no `init_task`, so no `layout_init`/`chdir` until the [layout] section calls `layout_init`).

**Exit protocol.** `selftest_task` -> `machine_exit(failures ? 2 : 0)` -> `outl(0xF4, code)`. With `-device isa-debug-exit,iobase=0xf4,iosize=0x04` QEMU exits with `(code<<1)|1`: pass = 1, fail = 5. `run.sh -T` maps 1 to 0 and passes other codes through (run.sh:41-44); `zelr.bat :test` treats errorlevel >= 2 as failure (zelr.bat:178-179); `pipeline/gate.sh` ignores the status and greps `SELFTEST_PASS` (gate.sh:291-292). Without the device the task just halts in `for(;;) hlt()` and the machine keeps running.

**Kernel-stack depth check.** Every kernel stack is painted with `STACK_PAINT` 0xC5C5C5C5C5C5C5C5 when created (sched.c:38-48, 182); the scheduler panics on every switch if the bottom word is overwritten (sched.c:501-502). `test_stack` (1718-1731) runs last on purpose: the paint left on the selftest task's own stack records the deepest the whole run went (deepest path: certificate chain verification with 512-byte moduli). It prints the headroom and requires `room >= TASK_STACK_SIZE/4` (8192 of 32768). Real log: `the deepest this run went left 16352 bytes of 32768 spare` (~16.4 KiB used).

**Helpers** (not tests): `make_elf` (573-593, `ELF_PHOFF` 64; a 192-byte ELF64 image built from spec offsets), `hex_digit`/`is_hex`/`from_hex` (1421-1440), `syn_pack`/`syn_feed`/`syn_lift`/`park_pointer` (2701-2726; Synaptics 6-byte reports fed to `syn_byte`), `contains` (2941-2946), `bb_contains` and `static char bb_scratch[BB_BYTES]` (3048-3060), `legacy_read32` (3184-3189; an independent CF8/CFC implementation to compare ECAM against), SMP helpers `SMP_ADDS` 20000, `test_lock`, `shared_counter`, `seen_arg[]`, `smp_add_work`, `tss_report_t`/`report_tss` (1151-1180), `waiter_task` (1302-1309), `timeout_task` (2912-2918), `on_breakpoint` (352).

#### Section table and counts

Columns: **A** = real `run.sh -T` log (i440fx, -m 64, 1 CPU, rtl8139, fresh IDE disk; verified, total 556). **B** = `pipeline/gate.sh` `selftest` (i440fx, -m 256, fresh 32 MiB IDE, QEMU default NIC; static count 560, matching commit 011292c7's message "self test 560"). **README** = README.md:1333-1357 table (claims 552).

| # | Header | Function(s) (lines) | A | B | README | Conditions / hardware |
|---|---|---|---|---|---|---|
| 1 | [string] | test_string (75-92) | 8 | 8 | 8 | none |
| 2 | [the identity map] | test_identity_map (116-171) | **2** | 6 | 6 | +2 if mapped > low+4 MiB (RAM above ~68 MiB); +2 if the probe page at low+10 MiB fits and a frame is free; +1 if mapped > 4 GiB |
| 3 | [physical memory] | test_pmm (173-181) | 4 | 4 | 4 | none |
| 4 | [paging] | test_paging (183-204) | 5 | 5 | 5 | none |
| 5 | [user access] | test_user_access (211-237) | 5 | 5 | 5 | 1 failure-only check if scratch space/frames fail |
| 6 | [heap] | test_heap (94-112) | 5 | 5 | 5 | none |
| 7 | [filesystem] | test_fs (239-255) | 7 | 7 | 7 | works on FAT or RAM fs |
| 8 | [paths] | test_paths (257-284) | 11 | 11 | 11 | none |
| 9 | [directories] | test_directories (286-312) | 12 | 12 | 12 | none |
| 10 | [open files] | test_open_files (918-996) | 30 | 30 | 30 | 3 checks inside `if (other > 0)` |
| 11 | [timer] | test_timer (314-349) | 3 | 3 | 3 | timing |
| 12 | [interrupts] | test_interrupts (351-365) | 2 | 2 | 2 | none |
| 13 | [disk] | test_disk (368-418) | 12 | 12 | 12 | SKIP without a disk; last 6 need > 64 sectors |
| 14 | [fat] | test_fat (521-564) | 14 | 14 | 14 | SKIP without a disk |
| 15 | [network] | test_net (420-447) | 9 | 9 | 9 | 2 without a driven NIC (SKIP), 4 without a lease; needs slirp DHCP, gateway ICMP and real DNS for example.com |
| 16 | [elf] | test_elf (573-632) | 7 | 7 | 7 | none |
| 17 | [userspace] | test_userspace (634-663) | 4 | 4 | 4 | prints the stub's `ring3` line |
| 18 | [video] | test_video (450-470) | 7 | 7 | 7 | SKIP without framebuffer; needs exactly 1024x768 |
| 19 | [mouse] | test_mouse (513-518) + test_mouse_edges (479-511) | 4 | 4 | 4 | the first check SKIPs without a mouse |
| 20 | [graphics] | test_gfx (666-710) | 13 | 13 | 13 | none |
| 21 | [windows] | test_wm (712-801) | 7 | 7 | 7 | SKIP without framebuffer |
| 22 | [window server] | test_winsrv (803-876) | 19 | 19 | 19 | 7 inside `if (ua)` |
| 23 | [built-in programs] | test_builtin (878-916) | 8 | 8 | 8 | last check needs disk + FAT |
| 24 | [theme] | test_theme (1057-1135) | 19 | 19 | 19 | deletes /zelr.cfg |
| 25 | [taskbar] | test_pins (998-1055) | 18 | 18 | 18 | deletes /zelr.pins |
| 26 | [live tree] | test_live_tree (2948-3010) | 19 | 19 | 19 | none |
| 27 | [layout] | test_layout (3012-3040) | 9 | 9 | 9 | deletes /home/notes |
| 28 | [waiting] | test_waiting (1311-1338) + test_idle_accounting (1351-1407) | 16 | 16 | 16 | idle accounting has no header of its own |
| 29 | [trackpad] | test_trackpad (2728-2908) | 25 | 25 | 25 | first check fails on a real Synaptics laptop |
| 30 | [crypto] | test_crypto (2398-2527) | 22 | 22 | 22 | none |
| 31 | [sha-256] | test_sha256 (1443-1558) | 15 | 15 | 15 | none |
| 32 | [aes-gcm] | test_gcm (1772-1845) | 11 | 11 | 11 | none |
| 33 | [x25519] | test_x25519 (1848-1912) | 8 | 8 | 8 | none |
| 34 | [rsa] | test_rsa (1915-1998) | 8 | 8 | 8 | none |
| 35 | [p-256] | test_p256 (2001-2104) | 13 | 13 | 13 | none |
| 36 | [sha-512] | test_sha512 (2107-2136) | 4 | 4 | 4 | none |
| 37 | [p-384] | test_p384 (2139-2182) | 6 | 6 | 6 | none |
| 38 | [certificates] | test_x509 (2185-2396) | **40** | 40 | 39 | 43 call sites, 40 execute (2 if/else pairs, 1 either/or in the truncation loop) |
| 39 | [randomness] | test_rng (1670-1700) | 5 | 5 | 5 | waits up to 4 s for the jitter source |
| 40 | [tls 1.3] | test_tls_schedule (1573-1657) + test_tls (1739-1769) | **26** | 26 | 19 | 12 + 8 + `TCP_MAX` (6) per-session checks |
| 41 | [wpa] | test_wpa (2546-2683) | 19 | 19 | 19 | none |
| 42 | [wait timeouts] | test_wait_timeout (2920-2933) | 3 | 3 | 3 | none |
| 43 | [processors] | test_smp (1182-1289) | 4 | 4 | 4 | 4 then SKIP on 1 CPU; 15 with >= 2 CPUs (13 if CPU 1 does not answer the TSS query) |
| 44 | [black box] | test_blackbox (3062-3174) | 21 | 21 | 21 | 9 without a disk or without a zelr volume at LBA 0 |
| 45 | [acpi and pcie] | test_pcie (3191-3259) | 4 | 4 | 4 | 4 without MCFG (i440fx); up to 12 with ECAM (q35) |
| 46 | [interrupt routing] | test_irqs (3271-3334) | 9 | 9 | 9 | 1 with only an 8259 |
| 47 | [clipboard] | test_clipboard (3339-3381) | 14 | 14 | 14 | none |
| 48 | [clock] | test_clock (3392-3483) | 18 | 18 | 18 | SKIP without CMOS RTC |
| 49 | [sound] | test_sound (3506-3546) | 0 (SKIP) | 0 | not listed | 3 with an HDA/Ensoniq controller |
| 50 | [kernel stack] | test_stack (1718-1731) | 2 | 2 | 2 | none |
| | **Total** | | **556** | **560** | 552 | |

Derived totals (static, from the conditions above): gate `selftest_q35` (q35, AHCI, -m 256) = 568 if the q35 default NIC is driven (README's own q35 figure implies it); `-smp 4` on the gate config = 571; `zelr.bat :test` (q35, -smp 2, -m 512) = 579; theoretical maximum 583 (identity map 7 with > 4 GiB RAM, ECAM 12, SMP 15, sound 3). The README table was regenerated from a run in commit 529c140e (2026-09-21); since then 76965afe added 1 [certificates] check ("self test 553") and 011292c7 added 7 [tls 1.3] checks ("self test 560").

#### What every check verifies (labels verbatim, by section)

1. **[string]**: strlen; strcmp equal; strcmp order; strcpy; memset; memcpy; memcmp; memmove overlap (overlapping forward move gives "aabcde").
2. **[the identity map]**: covers at least the low region (`paging_mapped_bytes() >= 64 MiB`); every 4 MiB step in 1..64 MiB translates to itself; (cond) memory above it is mapped too (low+2 MiB); and can be written and read back (pattern 0x5A5AC3C3A5A53C3C, restored); (cond) a 2 MiB page can be split to remap one page inside it (probe low+10 MiB); and the pages beside it are undisturbed; (cond, > 4 GiB) the hole below 4 GiB is left for devices (`virt_to_phys(0xC0000000) == 0`).
3. **[physical memory]**: frames allocate (distinct); page aligned; free count dropped by 2; restored.
4. **[paging]** (v = 0x00D00000): map_page; write through mapping; virt_to_phys agrees; unmap clears translation; and the identity mapping is put back.
5. **[user access]** (scratch directory): a kernel page above user space (0x38000000) is mapped; but ring 3 cannot reach it (`virt_is_user_in`); a user page (0x39000000) is reachable; its kernel neighbour in the same table (0x39001000) is not; an unmapped address (0x3A000000) is not.
6. **[heap]**: kmalloc returns distinct blocks; writable; coalesces back to baseline (`heap_used` restored); 512 KiB allocation; kcalloc zeroes.
7. **[filesystem]** (/t.txt, /u.txt): write; read back; stat size; append; append grew; create second file (root count +1); delete.
8. **[paths]**: absolute left alone; repeated slashes collapse; dot; dot dot climbs; dot dot in the middle; stops at root; root; mkdir+chdir /sub; cwd follows; relative name resolves inside; `../g.txt` leaves it.
9. **[directories]** (/d): mkdir; stats as dir; mkdir twice fails; file inside; read back; count 1 inside; nesting /d/deep/x; nested read; `..` inside a path resolves; rmdir refuses non-empty; emptying lets rmdir work; gone.
10. **[open files]** (/fd.txt): open for write; first fd is 3; write returns 8; seek to 0; overwrite in place; size 8; close writes out; file has "ABCdefgh"; chunked read; position continues ("def"); seek to end = 8; read past end = 0; missing file without O_CREATE fails; `fd_dup` gives another number; shared position; close one leaves the other; both wrote "123456"; `fd_dup2(fd, 9)`; fd 9 reads "123"; dup2 of an unopened fd refused; pipe made; ends differ; write "hello"; read "hello"; write to read end refused; read from write end refused; pipe not seekable; EOF after writer closes; `pipe_live() == 0`; `fd_live()` back to the starting value.
11. **[timer]**: ticks advance over `sleep_ms(60)`; 4..20 ticks; 50 `task_yield()` add < 10 ticks (a yield must not count as a tick; VEC_YIELD is a separate vector).
12. **[interrupts]**: `int $3` reaches a handler registered on vector 3; two more make +3 (the handler stays registered afterwards).
13. **[disk]**: size > 0; read sector `sectors-4`; write a pattern; read back; equal; original restored; (> 64 sectors) 16-sector run at `sectors-24` reads; writes; reads back; equal; restored; zero-length request refused (AHCI caps a command at 8 sectors, ATA at 255: the block layer must split).
14. **[fat]**: volume mounted; cluster count in range for FAT16 (4085..65524) or FAT32 (65525..0x0FFFFFF5); cluster >= 512 bytes; multi-cluster file (2 clusters + 137 bytes) written; same length; same bytes; appears in dir; delete; gone; `/sub` made; file in it; byte for byte; `/sub` holds exactly 1; cleanup.
15. **[network]**: a driven card is not also reported undriven; an undriven card is named by non-0/0xFFFF ids; (card) nonzero MAC; `net_dhcp(8000)` lease; (lease) IP, gateway, DNS nonzero; gateway answers ICMP (3000 ms); `example.com` resolves (6000 ms).
16. **[elf]**: rejects 8-byte buffer (`ELF_ERR_SHORT`); bad magic; 32-bit class; machine 40 (ARM) -> `ELF_ERR_TYPE`; segment at 0x00100000 -> `ELF_ERR_RANGE`; filesz 4096 past the file -> `ELF_ERR_OVERFLOW`; well-formed accepted with entry `USER_SPACE_BASE+0x40000000`.
17. **[userspace]**: `user_spawn_stub("selftest-ring3")` gives pid > 0; it exited; `syscall_count()` rose by >= 7 (6 putc + exit); frames reclaimed (polled up to 50 x 10 ms because the reaper runs on a later tick).
18. **[video]**: 1024x768; pitch = width*4; text cols = width/FONT_W; pixel round trip at (900,700); rect fills; rect stops at its edge; glyph 'A' has ink.
19. **[mouse]**: pointer on screen (if a mouse); press and release inside one frame both kept (`mouse_inject` + `mouse_take_edge`); exactly 2 edges in order; the press carries the position it happened at.
20. **[graphics]** (32x24 surface): clear; rect interior; outside untouched; clips top-left; clips bottom-right; line start; line end; disc centre; disc radius; text ink; `kformat` "%d/%s/%x"; left alignment; buffer-size truncation.
21. **[windows]**: two windows created; outer size = client + `WM_BORDER*2` by client + `WM_TOP + WM_BORDER`; consecutive moves fold into one; presses never fold (3 events); a press survives `WM_EVENT_QUEUE*2` (64) moves; the release stays at the let-go position (52) rather than the next move; close.
22. **[window server]** (PIDs 4242/4243, 64x48): create; size `(64<<16)|48`; other pid gets -1; surface maps at `WINSRV_SURFACE_BASE`; aliases window pixels; is not the canvas the desktop reads; canvas not yet updated; commit copies whole frame; including last pixel; later drawing invisible until next commit; same address again; foreign pid cannot map; commit accepted; foreign commit refused; close; handle dead; heap shrank (no bad free of a carved surface); two windows per program; `winsrv_release` drops both.
23. **[built-in programs]**: >= 3 programs; `vfs_builtin_count() == builtin_count_programs()` (46); `/bin/browser` > 1 KiB ("the last of them", stale: D5); `/bin/paint` > 1 KiB; ELF magic; not writable; not deletable; (disk+FAT) no `paint` in the FAT root.
24. **[theme]**: default accent nonzero; is a preset; preset 3 changes accent only; save; reload reports change; accent of preset 4 back; second reload reports no change; hand-written config ("preset 2, wallpaper 1, corner 14, shadows 0") written; picked up; preset; wallpaper `WALLPAPER_GRID`; corner 14; shadows off; wallpaper 99 falls back; the knob table has `corner`; corner 900 clamps to the table's `hi` (24); every one of the 31 knobs clamps to `hi` for 99999; and to `max(lo,0)` for 0.
25. **[taskbar]**: default list non-empty; contains /bin/term; not /bin/nope; pin /bin/count; count +1; duplicate refused; move to front; term moved to 1; move to back; move past front clamps; past back clamps; save; reload same length; same order; same label "Count"; unpin all; empty list persists; list stops at `PIN_MAX` (8).
26. **[live tree]**: /sys dir; /bin dir; both listed in `/`; /sys/version readable; contains name and version; stat size = read size; /sys/memory has "frames" and "heap"; /sys/tasks lists "selftest"; /sys/devices has "video"; /sys/uptime differs after 60 ms; /sys files not writable, not deletable, not openable for write; no mkdir inside; /bin/paint not writable; /sys/memory openable read-only; /sys/nothing reads < 0 and does not stat.
27. **[layout]**: /home, /doc, /cfg, /tmp exist; /doc/readme > 100 bytes; re-running `layout_init` does not restore deleted /home/notes; leaves /home/kept; file in /tmp; next `layout_init` empties /tmp.
28. **[waiting]**: waiter task created; it blocked (`wait_on` with no deadline); `task_blocked_count() >= 1`; slices unchanged over 150 ms; not self-woken; `wake_all` runs it; `task_wait` returns its exit status 7; `task_wait(999999) == -1`; then idle accounting: 5 task-state names distinct; lowercase words; current task exists; spinning 30 ticks gets > 10 slices; with 0 idle ticks; waiting 30 ticks via `task_idle_wait` gets > 10 slices; >= 80% counted idle; idle clearly exceeds spinning.
29. **[trackpad]**: machine did not detect a pad (`!syn_present()`); pad answer {02,47,07} recognised; mouse answer not; one-bit-off answer not; pointer parked at (400,300); first contact moves nothing; 600 units right = +100 px; y inverted (up the pad = up the screen); sub-pixel remainder kept (2+2 units nothing, third step +1 px); a 1500-unit jump ignored; two fingers (w=0) do not move; 240 units down = scroll 2; one finger 300 units = +50 px; second finger landing does not move; nor scroll; finger leaving does not move; quick tap = left click (sampled before printing, timing-sensitive, comment 2837-2844); click held one pass; released after 120 ms; two-finger tap = right click; resting finger (300 ms) is not a tap; travelled touch is not a tap; report found mid-stream decoded; bad 4th byte discards the report.
30. **[crypto]**: SHA-1 abc, 2-block, million 'a', empty (FIPS 180-1); HMAC-SHA1 RFC 2202 cases 1, 2 and long key; PBKDF2-SHA1 RFC 6070 with 1, 2, 4096 rounds; WPA PSK IEEE 802.11i annex H two vectors; AES-128 key accepted, encrypts, decrypts; AES-256 accepted, encrypts; 192-bit refused (label drift D7); RFC 3394 unwrap works, is right, bit flip refused, wrong KEK refused.
31. **[sha-256]**: abc; empty; 2-block; million; 55-byte and 56-byte padding boundaries; HMAC-SHA256 RFC 4231 cases 1, 2, long key; HKDF RFC 5869 case 1 extract/expand, case 3 extract (no salt)/expand (no info); TLS 1.3 early secret (32 zero bytes, RFC 8448); derived secret.
32. **[aes-gcm]**: 128-bit key; tag over nothing (NIST case 1); zeros encrypt/tag (case 2); case 4 ciphertext with AAD and partial block; its tag; open; changed message refused; changed tag refused; changed AAD refused; refused open zeroes the buffer.
33. **[x25519]**: RFC 7748 vectors 1 and 2; public values for Alice and Bob (6.1); shared secret; both sides equal; zero peer refused; peer value 1 refused.
34. **[rsa]**: real 4096-bit CA signature verifies; different hash refused; changed signature refused; real 2048-bit signature (over *.google.com leaf) verifies; valid signature under wrong key refused; short (255-byte) signature refused; signature >= modulus refused; zero signature refused.
35. **[p-256]**: k=1, 2, 3 times G match published x; n*G is infinity; real ECDSA verifies; different hash; r changed; s changed; zero r; zero s; off-curve key; compressed (0x02) form refused; wrong-key hash refused.
36. **[sha-512]**: SHA-384 abc, empty; SHA-512 abc; SHA-384 112-byte (padding block of its own).
37. **[p-384]**: 1*G, 2*G; n*G infinity; real signature; different hash; P-384 key refused by the P-256 code.
38. **[certificates]**: leaf/intermediate/root parse; leaf EC P-256 (65-byte key); intermediate RSA 2048 e=65537; root RSA 4096; leaf not CA; intermediate CA; issuer/subject chain; intermediate signed leaf; root signed intermediate; root did not sign leaf; validity read; `*.google.com` matches www.google.com; case-insensitive; one label only; "nonesuch.example.org" not matched (label says "bare domain", D6); example.com not matched; suffix trick not matched; empty not matched; full chain verifies at fixed time 20261001000000; wrong host -> `X509_WRONG_NAME`; before -> `X509_NOT_YET_VALID`; after -> `X509_EXPIRED`; tampered byte 110 refused; 2-cert chain with root in store verifies; lone leaf refused; SHA-384-signed cert parses; `X509_SIG_RSA_SHA384`; issuer in root store; SHA-384 signature checks (or 2 forced FAILs if the root is missing); altered byte 200 refused; empty and 1-byte inputs refused; truncations (cut*17, cut 1..63) all refused (one check); outer length 0xFFFF refused; > 10 roots; unknown name not trusted; `roots_at(0)` found by name; every root found by its own subject.
39. **[randomness]**: `rng_ready()` within 4 s; `rng_sources()` names one; 64 bytes not all zero; two requests differ; second half of a 64-byte request differs from the first.
40. **[tls 1.3]** (RFC 8448 trace): schedule reaches early/derived; handshake secret; c hs / s hs traffic secrets; server key (16) and iv (12); client key and iv; derived after handshake; master secret; c ap / s ap traffic secrets; then `tls_any()` false; `tls_connect(-1, "")` refused; `tls_error(-1)` says why; nothing opened; `tls_send(0)` fails; `tls_recv(0)` returns 0; `tls_close(0)` harmless; out-of-range handles (-1, `TCP_MAX`, `TCP_MAX+99`) inactive; 6 x "every session starts closed".
41. **[wpa]**: `wpa_pmk("password","IEEE")` published key; "IEEF" differs; PRF block 1 = HMAC-SHA1(label, 0, data, counter 0); block 2 counter 1; PTK symmetric in (AA,SPA,ANonce,SNonce) order; nonce bit changes it; AP changes it; password changes it; EAPOL MIC checks; checking does not modify; changed byte 40 fails; changed MIC fails; wrong KCK fails; 32-byte frame refused; GTK unwrap works; length 16; right key; 20-byte wrapping refused; 8-byte refused.
42. **[wait timeouts]**: a `wait_on(&never, 120)` returns; reports timeout (not wake); after 10..60 ticks.
43. **[processors]**: >= 1 CPU described; CPU 0 started; CPU 0 uses `GDT_TSS(0)` (0x28); has rsp0; (SMP) all found started; >= 1 helper; CPU 1 uses `GDT_TSS(1)`; with its own rsp0; CPU 1 idle spins < 100 in 50 ms (asleep, not spinning); `smp_run` accepted by every helper (20 tries x 10 ms, because the compositor may be using one); all finished within 8000 ms; counter exactly `20000*(helpers+1)` (lock excludes); each got its own argument; each `jobs` counter advanced; lock free afterwards.
44. **[black box]**: `bb_log` line present; `bb_mark` writes `== selftest phase`; log grew; `bb_fault` writes `!! a test fault`, `vec=14`, full 64-bit rip `0x00804000dead1000`; after 400 filler lines length < `BB_BYTES`, newest survives, oldest dropped; (zelr volume at LBA 0 with room) record written; `bb_recover`+`bb_prev` reads it; identical; torn record (byte 64 flipped) refused; good record readable again; OEM name "MSWIN4.1" makes `bb_flush` refuse; and not read; restored volume accepted; unrecognised data in the reserved sector not overwritten; blank region written.
45. **[acpi and pcie]**: tables found; ntables > 0; ncpus > 0; no MCFG -> SKIP + extended offset 0x100 reads 0xFFFFFFFF; with ECAM: base published; page aligned; something on buses 0..3; port and ECAM agree on id and class for every present function; host bridge answers via ECAM; offset 0x100 does not alias the header; offset 0x1000 reads absent; slot 32 reads absent; every MCFG entry sane.
46. **[interrupt routing]**: (8259 only: SKIP + `timer_hz() > 0`) LAPIC present; >= 16 IOAPIC inputs; every ACPI override names an existing input; IRQ0's GSI known (prints it; 2 on QEMU); ticks advance over 60 ms; spin calibrated per tick; masking IRQ0 stops ticks for ~4 ticks of spinning (no `sleep_ms` allowed while masked); unmasking restarts; route of input 200 refused.
47. **[clipboard]**: fresh empty; set; length; get length; same bytes; "asking with no buffer answers the length" (only calls `clip_len`, D8); 8-byte buffer gets 7; terminated; `CLIP_MAX+16` refused; old content kept; `CLIP_MAX-1` accepted; full length; set replaces; generation +1.
48. **[clock]**: reads; month, day, hour, minute, second in range; 4-digit year 1970..2199; second read; same date; not backwards; second changes within 3 s; > 1000 of 4000 hammer reads succeed; none go backwards (midnight excepted); `rtc_format` 19 chars; dashes at 4,7; colons at 13,16; short form hh:mm; 3-byte buffer not overrun. (The comment at 3445-3450 says the torn-read check has never caught anything under QEMU; treat as unverified.)
49. **[sound]**: rate 8000..96000; 1..8 channels; play position advance over 200 ms within 4x of `rate/5*frame`.
50. **[kernel stack]**: current task exists; headroom >= 8192 of 32768.

### 3.5 tools/shell_test.sh (123 lines)

- Builds unless `ZELR_PREBUILT=1` (the gate sets it), reads `VERSION` from include/types.h (15).
- Runs `qemu-system-x86_64 -kernel build/zelr.bin -m 64 -no-reboot -display none -serial stdio -append console` under a 90 s timeout (94). **No disk** (so the in-memory fs is exercised, not FAT) and no explicit NIC.
- Types one character every 0.05 s, 0.45 s after each line (52-61), because QEMU's stdio serial drops bytes with no back-pressure (4-9). Sequence (63-92): wait 2.5 s; `uname`; `ls`; `cat /doc/readme`; `write notes.txt shell wrote this`; `cat notes.txt`; `rm notes.txt`; `cat notes.txt`; `mkdir docs`; `cd docs`; `pwd`; `write inner.txt nested file`; `cd /`; `cat docs/inner.txt`; `ls docs`; `ps`; `mem`; `spawn`; `exec /bin/hello`; (1.5 s) `exec /bin/wintest`; (1.5 s) `echo done testing`; (2.5 s) `reboot` (ends QEMU because of `-no-reboot`).
- 17 `check` greps over the **whole transcript** (97-100): `zelr $VERSION x86_64`; `notes`; `written from scratch`; `shell wrote this`; `no such file`; `/home/docs`; `nested file`; `PID`; `running `; `physical:`; `spawned pid`; `hello from a program`; `wintest: surface at 0x0000008060000000` (= `USER_SPACE_BASE 0x8000000000 + 0x60000000`); `wintest: wrote and read back 3072 pixels` (64x48); `closed, handle is dead`; `wintest: ok`; `done testing`. Exit status = number of failures. Weakness: five of these are satisfied by the shell's own echo of typed input or by the welcome text (bug B10).

### 3.6 tools/shcheck.py (233 lines)

- `Guest(DISK, memory=256)` from tools/harness.py (32 MiB fresh IDE disk, `-append console` by default, harness.py:602-632), `wait_boot()` = first kernel prompt.
- Types `exec /bin/sh` at the kernel shell and waits for "zelr shell" (30 s). Then, at the ring 3 shell (prompt regex `\$ $`), 19 checks: runs `echo hello`; `nosuchprogram` says "not found"; `echo one > /t.txt` then `cat /t.txt` has "one"; `echo two >> /t.txt` then `wc /t.txt` = 2 lines, 2 words; shell still prints (`echo back`); `wc < /t.txt` = 2 lines; `cat /t.txt | grep two` has "two" and not "one"; `ls /bin | wc` >= 15 lines; `ls /bin | grep sh | wc` >= 1; `spin` prints "spinning"; `chr(3)` gives the prompt back; `echo still here`; `ps | grep spin` shows no spin; `pwd` == "/home" (cwd inherited from the kernel shell's init task); `cd /bin` then `pwd` ends with /bin; `ls | grep echo` finds echo; `exit` returns to a kernel prompt matching `zelr(?::\S*)?> `.
- `body()` (55-71) strips the echoed command and the trailing prompt, which is why it does not suffer shell_test.sh's echo problem.

---

## 4. Control flow and lifecycles

### 4.1 Entry paths
```
QEMU -kernel  -> boot.S _start/_start32 (32-bit, build PML4 identity 4 GiB 2 MiB pages)
              -> long_start -> kmain_multiboot(mb_magic, mb_info) -> kmain(&multiboot_handoff)
cdboot.S      -> _start32 with ebx = 0x70000 handoff (from_handoff=1) -> long_start -> kmain(mb_info)
uefi/loader.c -> handoff at 0x70000, kernel at 0x1000000, entry from "ZELRKRN" header -> _start64 -> kmain(rdi)
```
If `kmain` ever returned, boot.S halts (`cli; hlt` loop, boot.S:239-241, 252-254). It does not: `sched_start` never returns.

### 4.2 kmain, step by step (with every phase mark)

`bb_mark(x)` writes `[ms] == x` to the black-box buffer and mirrors it to serial only (blackbox.c:114-119, `append` uses `serial_putc`); `kprintf` lines go to the screen and serial. All stamps in kmain are `[    0]` (interrupts off). The real log (lines 3-75) is shown in brackets where useful.

| Step | Mark (bb_mark) | Code (main.c) | Screen/serial output (kprintf) | Black-box extra (bb_log) |
|---|---|---|---|---|
| 0 | - | 302-306 `serial_init(); bb_init();` | `zelr 0.37.0 boot log` (serial only, bb_init) | |
| 1 | `serial, vga` | 307-308 `vga_init()` | | |
| 2 | - | 310-311 handoff check -> `panic("started without a handoff structure")` | | |
| 3 | - | 313-316 command line scan: every position `strncmp(p,"selftest",8)` sets `want_selftest`; `strncmp(p,"console",7)` calls `shell_console_only()` | | |
| 4 | - | 318 `banner()` | the 6-line box | |
| 5 | `gdt` | 321 `gdt_init()` | `  gdt     flat segments, tss installed` | |
| 6 | `idt` | 323 `idt_init()` | `  idt     256 vectors` | |
| 7 | `pic` | 325 `pic_init()` | `  pic     irqs remapped to 32..47` | |
| 8 | `memory` | 327-336 `hbase = heap_base(); pmm_init(h, hbase);` then `heap_bytes = heap_size_for(h,hbase); pmm_reserve(hbase, heap_bytes);` | `  memory  N MiB usable, via <loader>` [53 MiB, multiboot] | `memory N KiB usable, loader X` |
| 8b | (same phase) | 342-343 `fpu_init()` (SSE on before any program runs; APs do it themselves) | `  fpu     sse enabled, 512 bytes of state per task` | |
| 9 | `paging` | 346-350 `paging_init(h); paging_init_pat();` (PAT before anything is mapped) | `  paging  enabled, N MiB mapped` [64] | |
| 10 | `heap` | 352-366 `heap_init(hbase, heap_bytes); pmm_share_init();` (COW share counts need the heap; until then fork copies) | `  heap    N MiB at M MiB, P MiB left for programs` [24 at 28, 29 left] | `heap N MiB at <addr>` |
| 11 | `acpi, pcie` | 378-389 `acpi_use_rsdp(h->rsdp); acpi_init(); pci_ecam_init()` (before any bus probe; needs paging) | `  pcie    ecam at P, buses 0..N` / `  pcie    none, using the legacy config ports` | `pcie ecam at ..., tables via xsdt/rsdt` / `pcie no mcfg, legacy config ports only` |
| 12 | `video` | 392-412 `fb_base ? fb_adopt(base,w,h,pitch) : fb_init(1024,768)`; on success `fbcon_init()` | `  zelr 0.37.0`, `  video   WxH 32bpp, CxR text` [1024x768, 128x48] / `  video   no adapter found, vga text mode`. Everything printed before this point is only on serial or VGA text memory | `video WxH 32bpp, <backend>, full flush N kcycles, write combining/uncached` + `video double buffered` / `NO BACK BUFFER...`; or `video none: no vbe and no vmware adapter, vga text only` |
| 13 | `filesystem` | 414-416 `fs_init(); vfs_init(); fd_init();` | | |
| 14 | `disk` | 418-443 `blk_init()` -> `diskfs_mount()` -> `bb_recover()` (after the mount because the log lives in the volume, before any new record) -> n>=0 mounted, n==-2 blank -> `diskfs_format()`, else unreadable | `  disk    <model> via <driver>, N MiB`; `  fs      fatNN mounted, N entries in the root` / `  fs      new disk prepared` / `  fs      could not prepare the disk` / `  fs      disk unreadable, using memory only`; or `  disk    none, files will not persist` (diskfs itself may print `  fs      fat16 on partition ...` and log `parts ...`) | `disk ...`, `fs ...`, `black box: recovered boot N, B bytes`, `disk none: no controller this kernel can drive` |
| 14b | (same phase) | 444-445 `builtin_install()` (46 ELFs become /bin) | `  progs   46 built in` | |
| 15 | `clock` | 448-456 `rtc_init()` | `  clock   YYYY-MM-DD HH:MM:SS` / `  clock   none, uptime only` | `clock ...` / `clock none: no usable cmos clock` |
| 16 | `timer` | 459 `timer_init(100)` (PIT programmed, IRQ0 unmasked, but IF still 0) | `  timer   100 Hz` | |
| 17 | `entropy` | 466-473 `rng_init()` | `  random  <sources>` / `  random  no hardware source, collecting timing jitter` | `random ...` |
| 18 | `smp` | 486 `sched_init()` then 488 `smp_init()` (APs adopt their stacks as `idle` tasks under the kernel lock) | `  cpu     N processors, M started` / `  cpu     1 processor` | `cpu N found, M started` |
| 19 | `network` | 496-507 `netdev_init()` -> `net_init()`; `wifi_init()`; `net_survey()` | `  net     <name> <mac>` / `  net     no card found`; `  card    ...` per NIC | `net <name>` / `net no card this kernel can drive`; `card ...` |
| 20 | `input` | 511-524 `ps2_init()` (before both drivers), `keyboard_init()`, `if (fb_active() && mouse_init())`, `usb_init()` (after keyboard and mouse, because what it finds is handed to them) | `  mouse   ps/2[ with a wheel], pointer at X,Y`; `  usb     <description>` if xHCI | `mouse none: no ps/2 pointer answered` (also when no framebuffer) |
| 21 | `sound` | 527-546 `sound_init()`; else list PCI 04/01 and 04/03 undriven controllers | `  sound   <desc>, N Hz` / `  sound   VVVV:DDDD at b:s.f, no driver` / `  sound   no controller found` | `sound ...` / `sound none, N undriven controller(s) on the bus` |
| 21b | (same phase) | 548-549 `serial_enable_irq()` | `  input   ps/2 keyboard + serial (irq driven)` | |
| 21c | (same phase) | 555-563 pointer report | `  pointer synaptics M.m, caps XXXXXX[, two finger scroll]` / `  pointer ps/2 mouse[ with a wheel]` / `  pointer none found` | |
| 21d | (same phase) | 571-581 i2c survey (PCI 0C/80 and 11/80, up to 4) | `  i2c     VVVV:DDDD at b:s.f, no driver` | |
| 22 | `interrupt routing` | 590-605 `ioapic_init()`; if `acpi()->has_8259` `pic_disable()`; route IRQ 0..15 whose vector `32+irq` has a handler | `  irqs    ioapic, N input(s), M routed` [24, 5] / `  irqs    8259, no ioapic described` | `irqs ioapic, ..., 8259 masked/absent` / `irqs 8259 only, no ioapic in the tables` |
| 23 | `syscalls, window server, scheduler` | 608-637 `syscall_init(); clip_init(); winsrv_init(); usb_start_service(); net_start_service();` then `task_create("selftest")` or `task_create("init")` + `net_dhcp_start()` | | |
| 24 | - | 639 | `  sched   N task(s)` (`task_count()`, the whole ring: AP idle tasks included; real log: 2) | |
| 25 | `handing over to the scheduler` | 644-646 `bb_flush()` (writes this boot's record if the volume is zelr's), `sched_start()` | | |

Phase attribution pitfalls: `fpu_init` sits in "memory"; `builtin_install` in "disk"; `wifi_init`/`net_survey` in "network"; `usb_init` in "input"; `serial_enable_irq`, the pointer report and the i2c survey in "sound" - a hang there is reported as "sound".

`sched_start()` (sched.c:587-605): creates CPU 0's `idle` task (after the count was printed), takes the kernel lock, sets `started`, `sti`, halts; the first IRQ0 enters `scheduler_switch`, which picks the head of the ring.

### 4.3 Tasks at boot and where they run

| Task name | Created by | When | CPU | Notes |
|---|---|---|---|---|
| `idle` (one per started AP) | `sched_adopt_ap` from `ap_main` (smp.c:285-287) | during `smp_init` | its own AP only | the AP's boot stack; the AP's work loop (smp.c:294-317) runs here |
| `usb` | `usb_start_service` (usb.c:719-721) | step 23, only if xHCI started | CPU 0 | every 300 ms checks `xhci_took_port_change()` and rescans |
| `net` | `net_start_service` (net.c:539-544) | step 23, always | CPU 0 | `net_poll()` then `task_sleep(10)` forever |
| `selftest` | kmain 619 | step 23, `selftest` mode | CPU 0 | `selftest_run` then `machine_exit` |
| `init` | kmain 621 | step 23, normal mode | CPU 0 | `layout_init`, chdir /home, `shell_task`; the compositor (`wm_run`) runs inside this task |
| `dhcp` | `net_dhcp_start` (net.c:657-661) | step 23, normal mode, only if `netdev_up()` | CPU 0 | `net_dhcp(6000)` once, clears `dhcp_asking`, exits |
| `idle` (CPU 0) | `sched_start` | after the count | CPU 0 | `hlt` loop charging idle ticks |

Rules (sched.c:423-465): kernel tasks (`!p->user`) are only picked on CPU 0; APs pick only ring 3 tasks or their own idle task; an AP with pending `smp_run` work stays idle to do it. There is **no reaper task**: dead tasks are freed inside `scheduler_switch` once reaped by `task_wait` or `REAP_GRACE` (1000 ticks = 10 s) after death (sched.c:535-555). Typical PIDs on one CPU without xHCI: net=1, init=2, dhcp=3, idle=4 (real selftest log: `sched 2 task(s)` = net + selftest). The compositor hands half of each frame to a free AP via `smp_run(helper, bands_helper)` with a bounded wait (fb.c:440-469).

### 4.4 Normal boot: console, desktop, Escape, back
1. `init_task`: `layout_init()` (create /home /doc /cfg /tmp, empty /tmp, seed files once) -> `vfs_chdir("/home")` -> `shell_task()`.
2. `welcome_print()`; `theme_init()`.
3. If framebuffer && not `console` && `autodesktop` -> `enter_desktop()`: spawn `/bin/term` (ring 3, any CPU), then `wm_run()` in this task.
4. In `wm_run` (wm.c:3878-4078) keys come from the same `kbd_trygetchar()` (so serial input reaches the desktop too). Escape (27): closes the find bar if open, else the launcher menu if open, else breaks the loop (wm.c:3952-3956). Other exits: launcher "Leave desktop" (wm.c:2974), a failed "Shut down" (wm.c:2975-2984); `wm_quit()` exists with no callers.
5. On leaving: every window closed (`wm_close`; user-owned windows get `WM_EV_CLOSE` so the program can exit), WM state reset, pointer autodraw back on (wm.c:4064-4077).
6. Back in `enter_desktop`: console cleared, `back at the shell`, then `shell_task` prints the prompt `zelr:/home> `.
7. Typing `desktop` repeats step 3 (a new `/bin/term` each time).
Verified by tests: tools/deskcheck.py:734-761 (boot with no command line opens the desktop; no `zelr:/home>` on serial; Escape yields a prompt), tools/clipcheck.py:89 (waits for "back at the shell").

### 4.5 Kernel shell input state machine
`IDLE(prompt shown)` --char 32..126 & len<254--> append+echo --'\b'/Delete--> erase one --'\n' or serial CR--> `execute(line)` (may block in `task_wait` for `exec`/by-name, or run `wm_run` for `desktop`) --> prompt --> `IDLE`. No key is ever buffered by the shell while a command runs; keystrokes typed during `exec` go to the program if it reads fd 0 (`console_read`, fd.c:214-249), otherwise they wait in the keyboard/serial buffers (256-entry keyboard ring).

### 4.6 Self-test lifecycle
kmain (`want_selftest`) -> `task_create("selftest", selftest_task)`; no `init`, no `dhcp` -> `sched_start` -> `selftest_run()` (50 sections in the fixed order of section 3.4) -> summary -> `machine_exit(0|2)` -> QEMU exits 1 or 5. Order constraints the code relies on: `test_net` does its own DHCP (so kmain must not start one); `test_layout` calls `layout_init` itself; `test_stack` must be last; `test_irqs` must not sleep while IRQ0 is masked (3303-3311).

### 4.7 Panic path as seen from main
- Direct panics in main.c: `not booted by a multiboot loader (magic=%x)` (260, before `bb_init`; `serial_init` first so it can be said) and `started without a handoff structure` (311).
- Kernel faults: idt.c `isr_dispatch` -> COW / demand-fill / "end the program" (exit 139) for ring 3 faults; a kernel fault -> `bb_fault(r, EXC[n])` (register dump into the log) -> `panic("unhandled exception %d (%s) ...")` (idt.c:231-236). The kernel shell's `fault` command takes this path.
- `panic` (printf.c:157-181): `cli`, white-on-red `*** KERNEL PANIC ***` + reason on the console, `bb_log("!! panic: ...")`, **disk first** (`bb_flush()`, so the record survives even if painting faults), then `bb_screen()` paints the log tail: on a framebuffer, header bar "zelr stopped. this is the boot log." and up to 60 lines, colour-coded (`!!` red 0xFF6060, `==` phase blue 0x80D0FF, register lines 0xFF9090); in VGA text, "zelr stopped. boot log:" and the last 23 lines (blackbox.c:169-206). If the record was not written: `(black box not written: not a zelr volume)`. Then `hlt` forever.
- Stack overflow is caught at the next switch: `panic("the %s task ran off the end of its kernel stack")` (sched.c:501-502).
- After a failed boot the record is readable on the next boot at `/sys/lastboot`; the current boot's log is `/sys/boot` (sysfs.c:232-257, 362-363).

---

## 5. Interfaces

**Exported by this area**
| Symbol | Defined | Callers |
|---|---|---|
| `void kmain_multiboot(u32, u32)` | main.c:250 | boot/boot.S:232 |
| `void kmain(handoff_t *)` | main.c:301 | boot/boot.S:237 (BIOS loader), :251 (UEFI `_start64`), main.c:298 |
| `void shell_task(void)` | shell.c:595 | main.c:236 (`init_task`) only |
| `void shell_console_only(void)` | shell.c:593 | main.c:315 only |
| `void welcome_print(void)`, `void guide_print(void)` | welcome.c:6, 20 | shell.c:596, 299 only |
| `int selftest_run(void)` | selftest.c:3548 | main.c:229 only |
Nothing else in the kernel calls into these files; all other interaction is through the serial line (harnesses) and the VFS.

**Depends on** (called from kmain): `serial_init`, `bb_init/bb_mark/bb_log/bb_recover/bb_flush`, `vga_init`, `gdt_init`, `idt_init`, `pic_init/pic_disable`, `pmm_init/pmm_reserve/pmm_free_frames/pmm_share_init`, `fpu_init`, `paging_init/paging_init_pat/paging_mapped_bytes`, `heap_init`, `acpi_use_rsdp/acpi_init/acpi()`, `pci_ecam_init/pci_ecam_base/pci_ecam_last_bus/pci_list_class`, `fb_adopt/fb_init/fb_*`, `fbcon_init`, `fs_init/vfs_init/fd_init`, `blk_init/blk_model/blk_driver/blk_sectors`, `diskfs_mount/diskfs_format`, `fat_type`, `builtin_install/builtin_count_programs`, `rtc_init/rtc_format`, `timer_init`, `rng_init/rng_ready/rng_sources`, `sched_init/task_create/task_count/sched_start`, `smp_init/smp_cpu_count/smp_started`, `netdev_init/netdev_name/net_init/net_mac/wifi_init/net_start_service/net_dhcp_start`, `ps2_init/keyboard_init/mouse_init/...`, `usb_init/usb_present/usb_describe/usb_start_service`, `sound_init/...`, `serial_enable_irq`, `syn_*`, `ioapic_init/ioapic_route_irq/ioapic_inputs`, `idt_has_handler`, `syscall_init`, `clip_init`, `winsrv_init`, `layout_init/layout_home`, `vfs_chdir`.
The shell depends on vfs, user.c (`user_spawn_elf`, `user_spawn_elf_argv`, `user_spawn_stub`), elf (`elf_error`), sched (`task_wait/task_create/task_list/task_state_name/task_idle_wait`), wm (`wm_run`), net/netdev/http, diskfs/blockdev/fat, usbdisk, sound, power, mouse, smp, pmm/heap/fs/serial counters, theme (`theme_init`, `theme()->autodesktop`), keyboard (`kbd_trygetchar`, `KEY_CODE`), fbcon/vga.
The self test depends on almost every subsystem header it includes (selftest.c:3-66), plus test-only hooks: `mouse_inject`, `mouse_take_edge`, `syn_byte`, `syn_reset_state`, `syn_tick`, `syn_answer_is_pad`, `tss_current_selector`, `tss_stack_of`, `winsrv_window`, `vfs_builtin_count`, `pipe_live`, `fd_live`, `theme_knob_*`, `roots_*`, `fat_boot_*`, `bb_text/bb_len/bb_prev`.

**Harness-facing contracts** (break these and the tools break): prompt format `zelr> ` / `zelr:<cwd>> ` (harness.py:67); the `console` command-line word (harness.py:625-632); `SELFTEST_PASS`/`SELFTEST_FAIL` and `N passed, M failed` lines (gate.sh:291-292); isa-debug-exit codes; `back at the shell`; `uname` format `zelr <ver> x86_64`; `ps` header `PID` and state words; `mem` `physical:`; `spawned pid`; boot-log phase names `== gdt`, `== disk`, `== handing over to the scheduler` (tools/blackbox_test.sh:76-95); `fs new disk prepared` / `fs fat16 mounted` bb lines (blackbox_test.sh:78,101-102).

---

## 6. Concurrency, locking, memory ownership, invariants

- **kmain runs with interrupts off and without the kernel lock.** No `sleep_ms`/`task_sleep` may be called in kmain (`sleep_ms` halts waiting for ticks that never come, timer.c:76-82). Boot-log stamps are all 0. The kernel lock is acquired once in `sched_start` and thereafter held by CPU 0 whenever it runs kernel code (including every kernel task), released only when returning to ring 3 or to an idle task (idt.c:287).
- **AP start-up overlaps kmain.** `ap_main` sets `started = true` (smp.c:279) before taking the kernel lock and calling `sched_adopt_ap` (kcalloc + ring insertion, smp.c:285-287); `start_cpu` returns as soon as it sees `started` (smp.c:214), and the BSP continues (next AP's `kmalloc(AP_STACK_SIZE)`, then network init) without the lock. The heap has no lock (heap.c). See B11.
- **Kernel tasks are preemptible on CPU 0** (frames built with IF set, sched.c:207; IRQ0 enters `scheduler_switch`, idt.c:256-258), despite sched.h:180 calling the kernel "non-preemptive" (that holds for system calls, which enter through interrupt gates). So the `init`, `net`, `dhcp`, `usb` and `selftest` tasks interleave at tick boundaries; shared state between them is protected only by the fact that they never run simultaneously.
- **The shell and the compositor poll.** `shell_task` and `wm_run` loop with `task_idle_wait()`, stay READY, and therefore CPU 0 keeps the kernel lock nearly continuously while either is up (open question Q1).
- **Ownership in the shell**: every `vfs_slurp` buffer is freed after spawning or printing (`cmd_cat`, `enter_desktop`, `run_by_name`, `bg`, `exec`); `argv` pointers point into the static `line` buffer and are copied onto the new program's stack by `user_build_stack` before `line` is reused; `cp` allocates `size ? size : 1` bytes. Children started by `bg`/`spawn` are never waited; their records disappear after `REAP_GRACE`.
- **Self-test side effects** (it is not isolated): deletes `/zelr.cfg` (1058, 1134) and `/zelr.pins` (999, 1053) and leaves them deleted; deletes `/home/notes` (3026) and the seed marker prevents re-seeding; creates/deletes /t.txt, /u.txt, /sub, /d, /fd.txt, /sptest.bin, /home/kept, /tmp/scratch; writes and restores raw sectors near the end of the disk (test_disk) and the boot sector's OEM name and the black-box sectors (test_blackbox); resets the clipboard (`clip_init`); leaves a breakpoint handler on vector 3; masks IRQ0 briefly; temporarily remaps 13 MiB and writes a pattern at `low+2 MiB` (inside the kernel heap on a 256 MiB machine, restored after an `ok()` print, see B13).
- **Invariants relied on**: `sched_init` precedes `smp_init` and any `task_create`; every IRQ handler is registered before the "interrupt routing" phase (handlers registered later are never routed through the IOAPIC); `fbcon_init` only after paging and the heap; `bb_recover` after the mount and before `bb_flush`; the multiboot handoff lives in BSS (boot.S stack is small).

---

## 7. Limits and magic numbers

| Value | Where | Meaning |
|---|---|---|
| `BITMAP_ROOM` 2 MiB | main.c:65 | PMM bitmap space (64 GiB of frames) |
| `HEAP_MIN` 24 MiB / `HEAP_SHARE` 4 / `HEAP_CEIL` 512 MiB | main.c:81, 92, 93 | heap floor / share / ceiling |
| 1 MiB | main.c:138 | left free beside the heap in its run |
| 2 MiB rounding above `KERNEL_LOW_MB` (64) | main.c:145-149, paging.h:33 | heap end alignment above the page-mapped region |
| `fb_init(1024, 768)` | main.c:394 | mode chosen on BIOS/multiboot boots (selftest [video] requires it) |
| `timer_init(100)` | main.c:459 | 100 Hz PIT |
| 6 NICs, 4 sound, 4 i2c devices | main.c:191, 536, 572 | survey limits |
| IRQ 0..15 -> vectors 32..47 | main.c:593-595 | routing loop |
| port 0xF4, codes 0/2 -> exit 1/5 | main.c:159, 230 | isa-debug-exit |
| `cmdline[128]`, `loader[32]`, `HANDOFF_MAX_REGIONS` 128 | handoff.h:34, 57-58 | handoff limits |
| `HANDOFF_MAGIC` 0x5A454C5236344846 | handoff.h:19 | "ZELR64HF" |
| multiboot flags bit 2 / bit 6 | main.c:265, 270 | cmdline / mmap valid |
| `LINE_MAX` 256 (254 usable), `ARG_MAX` 16 | shell.c:37-38 | shell line and word limits |
| 440 Hz / 400 ms; 20..20000 Hz; 10000 ms | shell.c:523-527 | `beep` defaults/clamps |
| 4 pings x 2000 ms; resolve 4000 ms; dhcp 6000 ms | shell.c:404-408, 395, 429 | network command timeouts |
| 3 x 400 ms | shell.c:201 | `spawn` counter task |
| `TASK_STACK_SIZE` 32768, pass mark 8192 | sched.h:32, selftest.c:1730 | kernel stack and [kernel stack] threshold |
| `STACK_PAINT` 0xC5C5C5C5C5C5C5C5 | sched.c:38 | stack paint |
| `REAP_GRACE` 1000 ticks (10 s) | sched.c:71 | dead-task record lifetime |
| `SMP_ADDS` 20000; `SMP_MAX_CPUS` 16 | selftest.c:1151, smp.h:30 | SMP counter test |
| `TCP_MAX` 6 | tcp.h:17 | number of per-session TLS checks |
| `PIN_MAX` 8, `PIN_FILE` "/zelr.pins", `THEME_FILE` "/zelr.cfg", 31 knobs | pins.h:14,17, theme.h:11, theme.c:88-131 | taskbar/theme tests |
| `CLIP_MAX` 65536 | clipboard.h:16 | clipboard tests |
| `SYSFS_MAX` 4096 | sysfs.h:34 | live-tree read buffers |
| `BB_SECTORS` 32 (16 KiB), `BB_MAGIC` 0x5842594E "NYBX", `BB_LBA_MIN` 1 | blackbox.h:27-34 | black box |
| `WM_EVENT_QUEUE` 32 | wm.h:60 | backlog test uses 64 moves |
| `WINSRV_SURFACE_BASE` 0x8060000000 | winsrv.h:19, paging.h:62 | expected by shell_test.sh |
| 46 built-in programs (limit `SYSFS_MAX_PROGRAMS` 48) | builtin.c:70-128, sysfs.h:30 | `progs 46 built in` |
| selftest timing: 60 ms -> 4..20 ticks; 50 yields < 10 ticks; wait 120 ms -> 10..60 ticks; 150 ms blocked; 4 s RNG wait; 30-tick spin/idle, > 10 slices, >= 80% idle; AP spins < 100 in 50 ms; 8000 ms join; 200 ms sound window within 4x; 4000 RTC reads | selftest.c | timing tolerances (all measured in 100 Hz ticks) |
| X.509 times 20261001000000 / 20200101000000 / 20300101000000 | selftest.c:2248, 2259, 2262 | fixed verification moments |
| probe addresses 0x00D00000, 0x38000000..0x3A000000, low+2 MiB, low+10 MiB, 0xC0000000 | selftest.c:185, 220-232, 134-169 | paging tests |
| shell_test.sh: 0.05 s/char, 0.45 s/line, 90 s timeout, -m 64, no disk | shell_test.sh:52-61, 94 | serial typing |

---

## 8. Tests

- **The self test itself** (section 3.4): `run.sh -T` (i440fx, -m 64, persistent `zelr.img` 16 MiB, rtl8139; real result 556), `zelr.bat :test` (q35, -smp 2, -m 512, AHCI, `zelr.img`), `pipeline/gate.sh` `selftest` (-m 256, fresh 32 MiB IDE; 560), `selftest_q35` (q35 + AHCI, fresh 32 MiB; 568 static), `selftest_nvme` (full mode, q35 + NVMe 64 MiB). Hardware needs per section are in the table in 3.4. Network sections need the host's DNS (example.com) through slirp.
- **tools/shell_test.sh** (gate "the shell answers over serial", gate.sh:332): the kernel shell's file commands on the RAM fs, `ps`, `mem`, `spawn`, `exec` of hello and wintest, `echo`, `reboot` ending the VM.
- **tools/shcheck.py**: the kernel shell's `exec` + returning from a ring 3 shell (`exit` back to `zelr...> `), plus the userland shell's own features.
- **Other harnesses exercising this area**: tools/deskcheck.py:734-784 (autodesktop with no command line, Escape, `write /zelr.cfg width 1920`, `desktop` again), tools/blackbox_test.sh (boot record across a reboot: `== gdt`, `== disk`, `== handing over to the scheduler`, `cat /sys/lastboot`), tools/powercheck.py (`shutdown`), tools/progcheck.py (`cp /bin/hello /home/mine`, running by name, `write /home/lump ...` as a non-ELF, `disk`), tools/bootcheck.py (`ls /`, `write`, `sync`), tools/smpcheck.py (`bg /bin/spin` x6, `cat /sys/cpu`, `ps`, `exec /bin/hello`), tools/netcheck.py (`net`, `dhcp`), tools/tlscheck.py and tools/webcheck.py (`fetch` without a save file, `resolve`), tools/enscheck.py and soundcheck.py (`beep`), tools/crashcheck.py, mountcheck.py, namecheck.py, ring3check.py, piccheck.py (`exec`, `ls`, `cp`, `write` at the kernel shell), tools/clipcheck.py (`desktop`, "back at the shell"), and many GUI checks that type `desktop`.
- **Not covered by any test**: `fetch HOST PATH FILE` saving (bug B1), `dhcp` while the boot DHCP task is running (B2), `stick`, `format`, `ring3`, `fault` (by design: it stops the machine), `cpus`, `mouse`, the Delete/serial-backspace quirks.

---

## 9. How to extend

- **A new boot step**: put it inside the right `bb_mark` phase, or add a new mark before it (the mark is what a photographed panic screen or `/sys/lastboot` will name). Keep it free of sleeps (interrupts are off); if it registers an IRQ handler it must run before `bb_mark("interrupt routing")` (main.c:589), otherwise the IOAPIC never routes it. If it creates a task it must come after `sched_init()` (main.c:486) or the task is thrown away. Say what was found in both `kprintf` (screen) and `bb_log` (record), as every existing step does.
- **A new kernel task**: `task_create(name, fn)`; it will only ever run on CPU 0 and holds the kernel lock while running; block with `task_sleep`/`wait_on`, never spin; exit with `task_exit`.
- **A new kernel-shell command**: add an `else if (!strcmp(c, "name"))` arm in `execute()` (shell.c:292-588) before the final `run_by_name` fallback; add it to `cmd_help` (79-115) - five existing commands were never added; remember a built-in shadows any `/bin` program of the same name; words come from `split()` (spaces, double quotes, max 16). If output must be machine-checked, keep the wording stable (harnesses grep it).
- **A new command-line word**: add a `strncmp` in main.c:313-316 (substring match anywhere). Only multiboot boots have a command line; zelr's own loaders always pass an empty one.
- **A new self-test section**: write `static void test_x(void)` using `ok()`; print `[x]` in `selftest_run` before calling it; keep `test_stack` last; SKIP by printing `  SKIP  reason` (not counted) rather than returning silently; restore every piece of global state you touch (files, theme, pins, clipboard, IRQ masks, mappings); evaluate timing-sensitive conditions before calling `ok()` because each `ok()` prints to the framebuffer console and a scroll can take longer than several ticks (selftest.c:2837-2844); never `sleep_ms` with IRQ0 masked (3303-3311). Then update the README table by regenerating it from a run (as commit 529c140e did) and mention which configuration the numbers are for; update pipeline/README.md:82, pipeline/cycle.sh:207, pipeline/prompts/implement.md:19,41, review.md:21 if the count is quoted.
- **Welcome/guide text**: it describes commands; change it together with the commands (several lines are already stale, section 10).
- **Pitfalls the comments warn about**: sched_init/smp_init order (main.c:479-485); IRQ routing placement (583-588); heap floor vs black screen on UEFI (72-80); DHCP must not run during selftest (634-635); the black box must never write to a volume zelr did not format (blackbox.c:1-15); a program found by name but not an ELF must not continue the search (shell.c:283-285); do not count a yield as a tick (selftest.c:321-342); a blocked task must not be scheduled (1291-1296); every CPU needs its own TSS (1186-1192).

---

## 10. Doc drift and suspicious code (verified)

### Drift
- **D1** README.md:1330, 1359: "runs 552 checks". The same README table (1333-1357) is for a larger-memory config ([the identity map] 6 needs > ~76 MiB); `run.sh -T` (-m 64) gives 2 there and **556 in total today** (verified real log). In the table's own configuration the total is now **560**: [certificates] 39 -> 40 (commit 76965afe, "every root in the store is looked up") and [tls 1.3] 19 -> 26 (commit 011292c7, seven per-session checks). README.md:1373 "-smp 4 reaches 563" is now 571; README.md:1376 "q35 ... reach 560" is now 568.
- **D2** Other stale counts: website "548" (and "3,561" lines for selftest.c, now 3605); pipeline/README.md:82 and pipeline/cycle.sh:207 "234"; pipeline/prompts/implement.md:19, 41 and review.md:21 "213".
- **D3** README.md:1606 "TLS is one session at a time" contradicts test_tls (selftest.c:1763-1768) and commit 011292c7 ("A TLS session per connection").
- **D4** selftest.c:567-568 comment "a minimal but structurally valid ELF32 header" directly above the ELF64 builder.
- **D5** selftest.c:889-892: "The last one in the list by name" checks `/bin/browser`, but the program table now ends with `polltest` (builtin.c:116); browser is 17th of 46. The truncation it guards against is still caught by the count comparison at 886-887.
- **D6** selftest.c:2231 label "and it is not the bare domain" tests `nonesuch.example.org`; the bare domain would be `google.com`.
- **D7** selftest.c:2500 "a key of a size that does not exist is refused" for 192 bits; AES-192 exists (FIPS-197); the implementation simply does not support it.
- **D8** selftest.c:3352-3353 "asking with no buffer answers the length" only calls `clip_len()` again (same as 3345); `clip_get` with no buffer is not exercised.
- **D9** selftest.c:194-199: "This address is 13 MiB ... it is inside the kernel heap" - the kernel is linked at 16 MiB (linker.ld:33) and the heap starts above the image (real log: 28 MiB), so 0x00D00000 is below the kernel in PMM-managed frames.
- **D10** selftest.c:1409-1419 header lists SHA-1/RFC 2202/PBKDF2/802.11i/AES/RFC 3394 sources, but sits above the hex helpers and `test_sha256`; the tests it describes are `test_crypto` at 2398.
- **D11** main.c:367-372: two comment paragraphs about the video step ("Needs paging to map the aperture...", "A UEFI loader has already chosen a mode...") sit above the ACPI/PCIe block instead of `bb_mark("video")` (391).
- **D12** main.c:513-518: `bb_log("mouse none: no ps/2 pointer answered")` is also logged when there is no framebuffer and `mouse_init` was never called.
- **D13** shell.c:82 help "desktop  windows, a mouse and a paint program": the desktop opens `/bin/term` (shell.c:218-228).
- **D14** shell.c:315 usage "e.g. exec hello": `exec` loads `argv[1]` relative to the cwd (317) with no /bin search, and the shell starts in /home, so the example fails (`exec: hello: no such file`); plain `hello` works through `run_by_name`.
- **D15** shell.c:361 `disk` prints "format   FAT16" regardless of `fat_type()`; FAT32 volumes are mounted by this kernel (main.c:427 prints `fat%d`).
- **D16** shell.c:496 "the kernel runs on cpu 0; the others take work handed to them" and ROLE "worker": APs also run ring 3 programs (sched.c:428-465; README.md:1595-1601).
- **D17** shell.c:232-241 comment says both consoles must be cleared; `vga_clear()` already calls `fbcon_clear()` when a framebuffer is active (vga.c:29-30), so the framebuffer console is cleared twice (harmless).
- **D18** welcome.c:35-38 guide: "Pick a colour and drag on the white area to draw" describes paint as if the desktop opened it.
- **D19** welcome.c:63-65 guide: `fetch example.com / page.html` then `cat page.html` does not work (B1).
- **D20** welcome.c:89-91 "the kernel prints where it happened rather than just stopping": a kernel-mode divide error panics (no vector-0 handler; idt.c:206-236); the machine halts after printing.
- **D21** welcome.c:94-95 "It runs inside an emulator": zelr boots real BIOS/UEFI machines (README "running it on a real machine").
- **D22** First-run text is effectively invisible on framebuffer machines with `autodesktop` on: the desktop covers it and `enter_desktop` clears the console on return (shell.c:240-241).
- **D23** blackbox.c:80-84 says stamps make slow probes visible; during kmain interrupts are off so every boot-phase line is stamped `[    0]` (real log lines 4-75; README.md:342-345 shows the same zeros).
- **D24** sched.c:590-592 "the count printed at boot is the number of tasks this machine was asked to run": `task_count()` includes the AP idle tasks adopted during `smp_init`.
- **D25** docs/boot.png shows 0.33.0 with "progs 36 built in" (now 46).
- **D26** zelr.bat:14-16 "It maps 64 MiB of itself and ignores the rest": paging maps all usable memory up to 64 GiB (paging.c:469-492) and the heap takes a quarter (main.c:83-93).
- **D27** layout.c:129-130 (seeded /doc/filesystem) says the desktop keeps its theme in /cfg; `THEME_FILE` is `/zelr.cfg` (theme.h:11). Cross-area, but it is first-run text.

### Suspected bugs
- **B1** (reachable only from the kernel shell) http.c:158 saves `fetch ... FILE` with `fs_write(save_as, ...)`, the in-memory fs, under the raw unresolved name. With a FAT volume mounted, the VFS never looks at the RAM fs (vfs.c:236-243, 246-253); without a disk it looks up the absolute path (vfs.c:239, `fs_find(abs)`) while the file was stored as e.g. `page.html` (fs.c:22-33). So "saved to page.html" is printed (http.c:161) but `cat page.html` fails in both cases. No test saves a file (webcheck/tlscheck fetch without FILE).
- **B2** shell.c:392-398 `dhcp` calls `net_dhcp(6000)` directly without `net_dhcp_busy()`; the boot-time `dhcp` task (net.c:651-661) may still be mid-exchange, and net.c:642-646 states the two share one socket and transaction id and must not overlap. Kernel tasks interleave at ticks, so typing `dhcp` in the first ~6 s after boot races.
- **B3** selftest.c:174-175 (`u32 f1 = pmm_alloc_frame()`) and 212, 215 (`u32 dir = paging_new_directory()`, `u32 kp/up/kp2`) truncate u64 physical addresses (pmm.h:17, paging.h:90). Harmless while the PMM hands out frames below 4 GiB (it allocates low-first), but on a machine where it did not, `map_page_in` would write page tables at a wrong address and wrong frames would be freed.
- **B4** run.sh:18, 30-40 (and zelr.bat:173-177): the self test boots on the same persistent `zelr.img` as normal runs, and the test permanently deletes `/zelr.cfg`, `/zelr.pins` and `/home/notes` (the seed marker stops notes being restored) and rewrites boot/black-box sectors (restored). Running `./run.sh -T` wipes a user's settings, pins and notes. The gate uses fresh images (gate.sh:283-284).
- **B5** main.c:284-292: the fallback region for a loader without an mmap starts at 1 MiB with length `(mem_upper + 1024) KiB`, i.e. it ends 1 MiB beyond what the loader reported (the comment says "up to what it claimed"); it also does not check multiboot flag bit 0 (mem_* valid). QEMU always supplies an mmap.
- **B6** main.c:313-316: command-line words match as substrings anywhere (`noselftest` enables the self test; any word containing `console` suppresses the desktop).
- **B7** shell.c:616: `(char)KEY_CODE(ch)` truncates special keys; `KEY_DELETE` (0x108) becomes `'\b'` and deletes the previous character. `console_read` skips special keys (fd.c:228).
- **B8** shell.c:624-626: only 0x08 erases; terminals usually send 0x7F for Backspace over serial, which is ignored.
- **B9** shell.c:59-77: with 17 or more words the 16th argv keeps the rest of the line, because the loop exits at `n == max` before the next iteration NUL-terminates the 16th word.
- **B10** tools/shell_test.sh:98-119: 5 of 17 checks can never fail because the kernel shell echoes typed characters to serial (shell.c:627-628 -> `kputc` -> serial) or the welcome text contains the string: `notes` (typed `write notes.txt ...`), `written from scratch` (welcome.c:9), `shell wrote this`, `nested file`, `done testing` (all typed). harness.py's `fresh()` (712-724) exists for exactly this failure mode.
- **B11** SMP boot race (section 6): `sched_adopt_ap` runs kcalloc and ring insertion on the AP while the BSP continues kmain without the kernel lock and allocates from the same unlocked heap (smp.c:173, 214, 279-287). Window of microseconds; not verified at run time.
- **B12** selftest.c:3414-3422: "the date did not change under us" and "time did not run backwards" fail if the two reads straddle midnight (the hammer loop at 3461 excludes midnight; these do not). Rare flake.
- **B13** selftest.c:133-159: on a 256 MiB machine low+2 MiB (66 MiB) and low+10 MiB (74 MiB) are inside the kernel heap (about 28-92 MiB); the pattern written at 139 stays there while `ok()` prints (preemptible, slow framebuffer scroll) until the restore at 141, and the 2 MiB page at 74 MiB is left split. Low risk (probably free heap interior), but not what the comment implies.
- **B14** Environmental: [network] requires real DNS for example.com through slirp; an offline host makes the whole self test report `SELFTEST_FAIL`. [trackpad] check 1 fails on a real Synaptics laptop booted by a multiboot loader with `selftest`.
- **B15** Destructive commands without confirmation: `format` (unpartitioned disks only), `stick write`. `reboot` does not flush the disk cache (intentional per tools/blackbox_test.sh:71-73).

---

## 11. Open questions

- **Q1 (for the sched/SMP owner)** CPU 0 keeps the kernel lock while any kernel task runs, and the kernel shell and the compositor are always READY polling loops (`task_idle_wait` halts with the lock held; idt.c:287 releases only on return to ring 3 or idle). An AP-resident program entering a system call spins in `kernel_lock_acquire` with interrupts off (idt.c:186) until CPU 0 next schedules another ring 3 task or its idle task. With exactly one runnable program that happens to be on an AP (e.g. a program started from the desktop terminal while the terminal blocks waiting for it), could it stall indefinitely? tools/smpcheck.py only uses `/bin/spin`, which makes no system calls while spinning. Needs a run-time test with `-smp 2`.
- **Q2** Ctrl-C from the kernel console: the kernel shell never calls `signal_console_reader` (only `console_read` does, fd.c:223). Which task does `signal_interrupt()` target for a program started with `exec` that never reads the console? (signal.c not read here.)
- **Q3** Is the AP-adoption heap race (B11) observable under QEMU MTTCG/WHPX/KVM?
- **Q4** Is http.c's `fs_write` a leftover from when fs.c mirrored the disk (fs.c:3-7)? Should `fetch` save through `vfs_write`?
- **Q5** Should `dhcp` use `net_dhcp_start()`/`net_dhcp_busy()`, and should the self test refuse to run on a disk that holds user data (e.g. only on a volume it just formatted)?
- **Q6** q35's default NIC is e1000e; the README's q35 total implies it is driven (so [network] = 9 there). Not verified in this static read; affects the q35/zelr.bat totals given above (568/579).
- **Q7** Multiboot boots size the heap without the screen (`fb_base = 0`), but the theme can later request up to 4096x4096 through `fb_set_mode` (wm.c:3184-3187). On `-m 64` (24 MiB heap) a large mode's back buffers cannot fit; what is the intended failure mode?
