# zelr: working notes for Claude

zelr is a 64-bit x86 operating system written from scratch in C and assembly. It has its own BIOS and
UEFI bootloaders, drivers, FAT filesystem, TCP/IP, TLS 1.3 with certificate checking, a compositing
desktop of ring 3 programs, and a web browser with its own CSS layout and JavaScript engine.

- Author and copyright: blavese. Licence: GPL-3.0-or-later (MIT up to v0.16.1).
- Upstream: https://github.com/blavese/zelr; site https://zelr.org (the site's source is not in this repo).
- The user has handed ongoing development to Claude.

**Deep reference: [docs/atlas/](docs/atlas/README.md).** It has one file per subsystem, written from a
full read of the tree at 6048716. Before changing an area:
- read its atlas file, including §10 (verified drift and suspected bugs);
- when you change behaviour, update the atlas;
- trust the code over the atlas when they disagree, and fix the atlas.

## House rules (they are the project's identity)
- **Nothing third party runs on the machine.** No libraries, packages or vendored code, and no image,
  icon or font files. Icons, cards and the typeface are drawn or generated in code. If a task seems to
  need a library, write the thing.
  - Host tools are fine: Zig as a C compiler, QEMU, bash, and the Python standard library. `certifi` is
    needed only to regenerate `kernel/roots.c` with `tools/mkroots.py`.
- **It has to actually work.** Run it.
  - Every change gets a check: a selftest section or a `tools/*check.py` gate step.
  - Break the thing on purpose, watch the check fail, restore it, and say that you did.
- **Never claim a pass you did not run.** Say plainly what could not be run.
- **Write like the code around it.**
  - Comments explain *why*, not *what*. Plain words, no em-dashes (write ` -- `).
  - British spelling (colour, behaviour, maximised).
  - UI labels are plain and lower case ("ask for an address").
- **Syscall numbers are never reused.** Retired numbers leave gaps (44 is retired). Structs shared with
  ring 3 are checked by `tools/abicheck.py`.
- **One source for every duplicated fact, plus a check.**
  - The load address lives in `linker.ld` (read by `tools/loadaddr.py`, checked by
    `tools/check_loader.py`).
  - The version lives in `include/types.h` `KERNEL_VERSION` (`tools/check_version.py`).
  - The settings live in `kernel/theme.c` `KNOBS[]`.
- **Commits** (see `docs/atlas/15-history-website.md` §6).
  - The subject is a noun phrase for the new state or fault: no prefix, no period. Examples: "A stack
    that grows when the program needs more of it", "Four checks that were reporting a busy host as a
    broken machine".
  - The body is prose in this order: symptom, mechanism, change, what it deliberately does not do, what
    checks it (including that it failed on a broken build), measurements, then a tally line such as
    "Self test 560, https 21."
  - AI-assisted commits end with `Co-Authored-By: <model> <noreply@anthropic.com>`.
  - Releases: a `zelr 0.N.0` commit, then `pipeline/release.sh`.
  - Land on `main` only once the gate has passed.

## Host requirements
- bash: Git Bash on Windows. Python 3, standard library only. Zig, used only as the C compiler (0.16.0 is
  verified).
- QEMU (`qemu-system-x86_64`), found through `$QEMU`, then `PATH`, then `C:\Program Files\qemu`. The UEFI
  paths need edk2 firmware; `tools/iso_test.sh` reads `ZELR_UEFI_FW`. A .NET SDK is needed only for
  `launcher/` (`release.sh`).
- On Windows, keep `core.autocrlf=false` in this repo. CRLF in the bash scripts breaks them.
- Notes about one particular machine belong in `CLAUDE.local.md`, which is not committed.

## Build, run, test
```
bash build.sh                  # ~1 min: 46 ring 3 programs (pasted into the kernel), bootloaders,
                               # BOOTX64.EFI, kernel -> build/zelr.elf and build/zelr.bin (~10 MB flat)
bash run.sh -T                 # kernel selftest on i440fx, 64 MiB: 556 checks, exit 0 = pass
bash run.sh                    # boot in a window (-t headless serial, -i through our own ISO bootloader)
python tools/mkiso.py          # build/zelr.iso: BIOS and UEFI, disc and stick, one image
bash pipeline/gate.sh fast     # build + selftest (pc and q35) + ring 3 + serial shell + black box
bash pipeline/gate.sh screen   # fast + the desktop/terminal/USB/input/sound/network/browser harnesses
bash pipeline/gate.sh full     # everything: 47 steps, all passed at 6048716 and at 0.38.0 (~9-11 min)
```
- **Selftest counts depend on the machine**: 556 (i440fx, 64 MiB), 560 (256 MiB, as in the gate), 568 (q35
  or NVMe), 571 with `-smp 4`. The README still says 552.
- **`run.sh -T` damages the persistent `zelr.img`**: the selftest deletes `/zelr.cfg`, `/zelr.pins` and
  `/home/notes`. Use a scratch image (`ZELR_DISK=...`) if that disk matters.
- The harnesses (`tools/*check.py`, on `tools/harness.py`):
  - boot QEMU and type over serial at 50 ms a character (stdio serial drops bursts);
  - drive the monitor for keys, mouse and screendumps, and assert by counting colours;
  - wait for conditions, never for a time.
  - The catalogue is in atlas 14.
- **Gate blind spots (atlas 14 §10).**
  - The build step fails only on the word "error", and otherwise tests the previous binary.
  - `boottest` is defined twice, so in full mode an ISO failure can be retried as bootcheck and pass.
  - `defaultcheck.py` compares nothing: its 3 checks are vacuous.
  - 4 of shell_test's checks match the echoed command.
  - `check_sse.py` is never run.
- The reproduction scripts in `docs/atlas/repro/` exit 1 while their bug stands.

## Architecture in one screen (map: docs/atlas/README.md)
- **Boot.**
  - `bootloader/cdboot.S` (BIOS: El Torito or the MBR on a stick), `uefi/loader.c` (BOOTX64.EFI) and
    `boot/boot.S` (QEMU `-kernel`, multiboot with the a.out kludge).
  - All build `handoff_t` (magic "ZELR64HF"). The kernel is linked at **16 MiB**.
- **kmain phases**, logged by the black box: serial/vga, gdt, idt, pic, memory, paging, heap,
  acpi/pcie, video, filesystem, disk, clock, timer, entropy, smp, network, input, sound, interrupt
  routing, syscalls/window server/scheduler, handover.
- **Boot tasks**: `usb` (with xHCI), `net`, `init` (the kernel shell, which also runs the desktop) or
  `selftest`, `dhcp`, and an idle task per CPU. The command line (multiboot only) knows `selftest` and
  `console`.
- **Memory.**
  - There is no higher half. PML4 entry 0 (identity map, shared) is the kernel's; user space is
    `0x80_0000_0000` and up. Offsets from there:
    - heap +0x1000_0000;
    - mmap +0x3800_0000 (128 MiB, 16 ranges);
    - image +0x4000_0000 (`sdk/zelr.ld`);
    - stack top +0x5000_0000, growing to 1 MiB;
    - window surfaces +0x6000_0000, 8 MiB each.
  - The low 64 MiB is mapped a page at a time; usable RAM above in 2 MiB pages, up to 64 GiB.
  - The heap is a quarter of RAM (24 MiB to 512 MiB), first fit and unlocked. The PMM is a next-fit
    bitmap with copy-on-write share counts.
- **CPU.**
  - Per-CPU GDT/TSS; 52 interrupt stubs; PIC then IOAPIC with MADT overrides; LAPIC timers on APs;
    PIT at 100 Hz on the BSP, the only time base.
  - **Every IDT gate is an interrupt gate, so syscalls run with interrupts off.** One big kernel lock is
    held whenever a CPU is not in ring 3. It is released only when the frame being returned through is
    ring 3 or idle (`kernel/idt.c:287`).
- **Tasks.** Round robin; kernel tasks stay on CPU 0; APs run user tasks. 32 KiB kernel stacks
  (certificate checking goes about 16 KiB deep).
- **Processes.**
  - int 0x80 with rax = number and rbx, rcx, rdx = arguments; 63 live calls. Static ELF64 only.
  - fork is copy-on-write; exec rewrites the frame. Descriptors: 32 open-file descriptions and 16 fds per
    task. Pipes; `poll` (which polls every tick).
  - Three signals with handlers. Network sockets are a separate table of 6, not fds.
- **Storage.** ATA / AHCI / NVMe / USB mass storage, then block ids, then GPT/MBR, then FAT16/32 with long
  names and staged crash-safe writes, then the VFS: `/`, `/usb`, `/bin` (`kernel/builtin.S`), `/sys`
  (`kernel/sysfs.c`), and a RAM fs.
- **Network.**
  - Drivers: e1000, then PCnet, then RTL8139, then RNDIS.
  - ARP/IPv4/ICMP/UDP/DHCP/DNS in `net.c`, pumped by `net_poll()`.
  - TCP: 6 client connections, one segment in flight.
  - TLS 1.3 client (AES-128-GCM, SHA-256, X25519) with a session per connection in the kernel. But
    `sys_connect_tls` still allows only one secure socket machine-wide.
  - Certificates: X.509 with RSA and ECDSA P-256/384; roots in `kernel/roots.c`.
- **Desktop.**
  - `kernel/wm.c` composites the whole back buffer each frame, inside the `init` task.
  - The window server has 8 slots (syscalls 7-12, 36, 37, 56, 57). Surfaces are double-buffered
    (`win_commit`). Each window has a 32-event ring with motion folding.
  - The theme is `/zelr.cfg` (31 knobs plus colours), re-read about four times a second. There is no
    theme syscall: programs read `/zelr.cfg` and `/sys/theme`.
  - Design system: `docs/atlas/16-design-system.md`.
- **Ring 3.**
  - `userland/`: single-file static programs built only from `sdk/` (`zelr.h`, `zelr.ld`). The toolkit
    is `ui.h`, `draw.h` and `alloc.h`.
  - The browser is web/fetch/html/dom/css/layout plus the `js*.h` engine plus png/jpeg/svg/inflate.
    Searches go to lite.duckduckgo.com.
  - `sdk/libc` is a from-scratch C library, newer than the README.
  - Programs started from the terminal print to the kernel console, not into the terminal window.

## Traps (each has bitten this project)
- **The kernel must not use SSE or x87** (`-mno-sse -mno-sse2 -mno-mmx -mno-80387`). Ring 3 *does* use
  SSE: it is built without those flags, and FXSAVE is kept per task.
- clang turns byte loops into `memset`/`memcpy` calls, and memset once recursed into itself. Byte movers
  use volatile pointers.
- Never hard-code the load address or the version.
- `kernel/builtin.S` uses `.incbin`, which the zig cache cannot see. `-DZELR_BLOB_STAMP` (a cksum of the
  blobs) is what makes a changed program reach the image.
- DMA memory must be identity-mapped heap, never the stack. Device-written memory is volatile; MMIO is
  mapped uncached.
- **Nothing that runs with interrupts off may wait on `timer_ticks()`**: it never advances. That covers
  early boot, the panic path and *every system call*. See known issue 1.
- Anything the compositor reads from a program's window must be published atomically (`win_commit`;
  canvas/cw/ch change with interrupts off).
- QEMU is lenient (boot sectors, NVMe doorbell stride, RTC, hubs, drive caches). Mark protections it cannot
  exercise as unverified.
- `/zelr.cfg` reload is incremental, and `look`/`light`/`preset` lines overwrite the colours that follow.
- Kernel face indices (8 faces) differ from ring 3's (6). `tools/harness.py` hard-codes the kernel order.
- Tests: hold mouse buttons across a WM pass, park the pointer outside compared areas, and never trust a
  check you have not seen fail.

## Known issues, most severe first (details in each atlas §10; "reproduced" = seen running on 2026-09-26)
1. **A ring 3 network call whose peer never answers freezes the machine. REPRODUCED**
   (`docs/atlas/repro/nethang.py`).
   - Waits in `net.c`/`tcp.c` spin on `timer_ticks()` inside int 0x80, where interrupts are off.
   - Triggers: a firewalled port, a silent DNS server, or no gateway.
2. **TLS server authentication can be bypassed**: CertificateVerify is never required
   (`kernel/tls.c:681-758`). [06b §10.1]
3. **The first-boot auto-format can wipe disk 0 when its partition table is rejected.** On a laptop booted
   from a stick, that is the internal drive (`main.c:430`, `diskfs.c:95-141`). [04 S1]
4. **FAT.**
   - The 8.3 alias compare makes `chapter10.txt` open `chapter1.txt` (`fat.c:1202`).
   - Renaming a file to its own alias deletes it.
   - A cross-volume rename is not rejected; this is reachable from ring 3.
   - `format` after a `/usb` access writes onto the stick.
   - Deletes are not crash-ordered; reclaim can free live data.
   [04 S2-S7]
5. **The AHCI and USB-stick flushes are no-ops, and every flush goes to disk 0**, so crash safety is weaker
   on real disks. [04 S8-S9]
6. **`sys_kill` and `sys_sigsend` have no ownership check.** Any program can end kernel services
   (Monitor's Stop can kill "idle"). [03 §10.5]
7. **SMP.**
   - The big lock is kept into ring 3 after copy-on-write or demand faults (`idt.c:202-203, 220-221`),
     locking other CPUs out until that CPU's next kernel entry.
   - Unverified: a global `current_pml4` against per-CPU CR3, CR0.WP never set, and partial 2 MiB chunks
     above 64 MiB handed out unmapped.
   [02 §10]
8. **Window manager.**
   - A window closed mid-resize leaves a dangling `resizing` pointer.
   - Right-button capture sticks.
   - fork and exec treat surface pages as user pages.
   - Maximising past 1600 px fails silently.
   [07 §10]
9. **JavaScript `call`, `apply` and `bind` are broken. REPRODUCED** (`docs/atlas/repro/callbind.py`).
   - `for (k in o)` without `var` fails to parse.
   - Decimals print truncated.
   - The arena is exhausted after about 40k calls.
   [12 §10]
10. **Data loss in the programs.**
    - The terminal's `cp`/`mv` stop at 16 KiB, then `mv` deletes the source.
    - Files' copy/move/rename use a 64 KiB buffer.
    - Notes truncates past 64 KiB.
    [09b, 10 §10]
11. **FP state.** Signal handlers can clobber FP/SSE registers, and exec keeps the old FPU state.
    [09a §10.2]
12. **libc.** `atan` is wrong, `fputc` overflows into the next FILE, `stdin` is always EOF, and `clock()`
    is ten times too small. [09a §10.2]
13. **Decoders and layout.**
    - The PNG chunk bounds check wraps (`png.h:146`).
    - SVG paint inheritance is missing.
    - A hidden last child makes layout run twice.
    [13, 11 §10]
14. **UEFI.** GOP RGB vs BGR is not carried in the handoff; the handoff is fixed at 0x70000. [01 §10]

## Documentation that is out of date (fix what you touch)
- **README.md**:
  - check counts (552);
  - syscall counts (57/58; the real number is 63);
  - "two-level paging" and "ELF32";
  - the kernel "about 1.5 MB" (about 10 MB);
  - "no floating point anywhere";
  - 11 wallpapers (12);
  - "single-connection TCP";
  - the TLS "one session" reason;
  - "No libc", and "four files in sdk/";
  - the terminal's "thirty-six commands" (38);
  - `-mcpu=i686`/check_sse.
- **Everything else**:
  - NOTICE: 138 commits, no libc.
  - `sdk/zelr.h`: "forty-seven system calls".
  - `include/syscall.h`: "one socket", "no handlers".
  - `zelr.bat`: "no USB stack yet".
  - `tools/whereis.py`: ELF32 only.
  - pipeline: prompts say 213 checks; the backlog lists done work as todo.
  - zelr.org: v0.35.0.
- Full list: `docs/atlas/15-history-website.md` §9.4 and each atlas §10.

## Where things are
| To change... | Look in |
|---|---|
| a syscall | `include/syscall.h` + `sdk/zelr.h` (same numbers) + `kernel/syscall.c`, then `tools/abicheck.py` |
| scheduling, the lock, fork/exec, signals | `kernel/sched.c`, `kernel/idt.c`, `kernel/syscall.c`, `kernel/signal.c`, `kernel/user.c` |
| files and disks | `kernel/fat.c` (+ `diskfs.c`, `vfs.c`, `parts.c`, `blockdev.c`); `/sys` in `kernel/sysfs.c` |
| a setting or the look | `kernel/theme.c` `KNOBS[]` + `include/theme.h`; `kernel/wm.c`, `kernel/gfx.c`; ring 3 `userland/ui.h` |
| a desktop program | `userland/<name>.c`; it lands in `/bin` via `kernel/builtin.S`/`builtin.c`; launcher `MCAT[]` in `kernel/wm.c` |
| the browser | `userland/browser.c`, `css.h`, `layout.h`, `html.h`, `dom.h`, `fetch.h`, `web.h`; JS in `js*.h` |
| boot | `bootloader/cdboot.S`, `uefi/loader.c`, `boot/boot.S`, `include/handoff.h`, `tools/mkiso.py` |
| tests | `kernel/selftest.c` (50 sections), `tools/*check.py`, `pipeline/gate.sh` |

## Workflow for a change
1. Read the atlas file for the area.
2. Change the code in the house style.
3. Add or extend a check, and prove it fails on a deliberately broken build.
4. `bash build.sh`, then `bash run.sh -T`, then the relevant `tools/*check.py`. Then `bash pipeline/gate.sh
   fast`; use `screen` or `full` when the desktop, boot or disks are touched.
5. Fix the doc drift you touched (README, atlas, this file).
6. Commit in the house style. Do not push unless the user says so.
