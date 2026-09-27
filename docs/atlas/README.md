# The zelr atlas

A subsystem-by-subsystem map of zelr, written so that whoever works on it next can find anything without
re-reading 120,000 lines. It describes the tree at **commit 6048716** (`main`, 2026-09-22, two commits
after v0.37.0). It was written on 2026-09-26 by reading every file in the repository (generated data
skimmed) and checking the results against a real build and test run.

- Each numbered file covers one area, in the same eleven sections: scope, big picture, file-by-file
  detail, control flow, interfaces, concurrency and ownership, limits, tests, how to extend, **doc drift
  and suspicious code**, and open questions.
- File:line references are to 6048716. Once the tree moves on, trust the code over this atlas and fix
  the atlas.
- "Verified" in a §10 means the claim was checked against the source by reading, not by running a
  reproduction, unless it says so.

## Files

| File | Area | Covers |
|---|---|---|
| [01-boot-build.md](01-boot-build.md) | Boot, build, imaging | `boot/`, `bootloader/`, `uefi/`, `linker.ld`, `build.sh`, `run.sh`, `zelr.bat`, `handoff.h`, `builtin.S`, the image writers `mkiso/mkgpt/mkfat/readfat`, the build checks |
| [02-cpu-memory.md](02-cpu-memory.md) | CPU, interrupts, memory, SMP, platform | GDT/TSS, IDT/ISR, PIC/LAPIC/IOAPIC, ACPI, SMP, paging, PMM, heap, FPU, PIT, RTC, RNG, serial, power, PCI |
| [03-processes-syscalls.md](03-processes-syscalls.md) | Tasks, processes, system calls | scheduler, the big kernel lock, wait queues, signals, ELF64 loader, fork/COW/exec, fds, pipes, poll, **the full syscall table** |
| [04-storage-fs.md](04-storage-fs.md) | Storage and filesystems | ATA/AHCI/NVMe, the block layer, GPT/MBR, FAT16/32 + LFN, crash-safe writes, VFS, `/sys`, layout, the black box boot log |
| [05-devices.md](05-devices.md) | Devices | PS/2, keyboard, mouse, Synaptics, xHCI/USB/HID/mass storage, HD Audio, Ensoniq, sound, framebuffer/VBE/SVGA/GOP, fbcon, VGA, clipboard, pins |
| [06a-network.md](06a-network.md) | Network | netdev, e1000, PCnet, RTL8139, RNDIS, ARP/IPv4/ICMP/UDP/DHCP/DNS, TCP, the kernel HTTP client, wifi detection, the test web server |
| [06b-tls-crypto.md](06b-tls-crypto.md) | TLS and cryptography | TLS 1.3, X.509 chains, RSA, ECDSA P-256/384, X25519, SHA-1/256/384/512, HMAC/HKDF/PBKDF2, AES/GCM, WPA2, the root store |
| [07-wm-desktop.md](07-wm-desktop.md) | Desktop | the compositing window manager, window server, theme and its 31 knobs, gfx primitives |
| [08-kmain-shell-selftest.md](08-kmain-shell-selftest.md) | Kernel entry and self test | `kmain` phases, boot tasks, the kernel shell and its commands, first run, **the selftest sections** |
| [09a-sdk-libc-toolkit.md](09a-sdk-libc-toolkit.md) | SDK, libc, toolkit | `sdk/zelr.h` ABI, `zelr.ld`, `sdk/libc`, `alloc.h`, `args.h`, `draw.h`, `ui.h` widgets |
| [09b-term-sh-tests.md](09b-term-sh-tests.md) | Terminal and shell | `term.c` and its built-ins, `/bin/sh`, coreutils, the ring 3 test programs |
| [10-apps-games.md](10-apps-games.md) | Desktop programs | files, settings, notes, paint, music, calc, monitor, blackjack, poker |
| [11-browser.md](11-browser.md) | Web browser | URL/fetch/cookies/keep-alive, HTML, DOM, CSS cascade, layout, painting, forms, events |
| [12-js-engine.md](12-js-engine.md) | JavaScript | values, parser, interpreter, built-ins, regex engine, DOM bindings, timers, XHR |
| [13-media-fonts.md](13-media-fonts.md) | Pictures and type | inflate, PNG, JPEG, SVG, the outline typeface generator, the 8x16 font |
| [14-tests-pipeline-launcher.md](14-tests-pipeline-launcher.md) | Verification and delivery | `harness.py`, **the test catalogue**, `gate.sh` tiers, the two-model pipeline, `release.sh`, the Windows launcher |
| [15-history-website.md](15-history-website.md) | History and public face | every release and commit, decisions and their reasons, the bug catalogue, commit/release style, zelr.org |
| [16-design-system.md](16-design-system.md) | Design system | principles, colour tokens, the two looks, shape, type, spacing, icons, motion, wallpapers, components, voice |

## System map

```
  firmware / emulator                     loaders (all build one handoff_t, magic "ZELR64HF")
  ---------------------------------------------------------------------------------------------
  BIOS, disc (El Torito entry 1) --\
  BIOS, stick (MBR)  ---------------+--> bootloader/cdboot.S  (real mode, unreal mode copies up) --\
  UEFI, disc (El Torito 0xEF)  ----\                                                               |
  UEFI, stick (ESP)  ---------------+--> uefi/loader.c = \EFI\BOOT\BOOTX64.EFI (GOP, ACPI, map)  ---+--> kernel
  QEMU -kernel (development)  -------> boot/boot.S multiboot a.out kludge, 32 -> 64 bit  ---------/    at 16 MiB

  kmain (kernel/main.c), boot log phases in order:
    serial,vga > gdt > idt > pic > memory > paging > heap > acpi,pcie > video > filesystem > disk >
    clock > timer > entropy > smp > network > input > sound > interrupt routing >
    syscalls, window server, scheduler > handing over to the scheduler  [> selftest phase]

  kernel (ring 0, one big lock held whenever a CPU is not in ring 3)
  +-- CPU & memory ........ gdt idt isr pic lapic ioapic acpi smp paging pmm heap fpu timer    (02)
  +-- tasks & processes ... sched wait signal user elf syscall fd pipe apps                    (03)
  +-- storage ............. ata ahci nvme usbdisk > blockdev > parts > fat/diskfs > vfs        (04)
  |                         vfs also serves /bin (builtin.S), /sys (sysfs.c), RAM fs (fs.c)
  +-- devices ............. ps2 keyboard mouse synaptics | xhci usb | hda ens sound |         (05)
  |                         fb svga vga fbcon | clipboard pins
  +-- network ............. e1000 pcnet rtl8139 usbnet > netdev > net > tcp > http            (06a)
  +-- crypto & tls ........ sha* gcm crypto x25519 rsa ec x509 roots tls wpa rng              (06b)
  +-- desktop ............. wm (runs inside the shell task) winsrv gfx theme                  (07)
  +-- console ............. shell welcome selftest blackbox                                  (08, 04)
  ========================= int 0x80, 64 system calls (include/syscall.h = sdk/zelr.h) =========
  ring 3 (userland/, built only with sdk/)
    term sh coreutils | files notes paint settings monitor music calc | blackjack poker |       (09b, 10)
    browser = web fetch html dom css layout + js jsparse jsrun jsdom jsregex +                (11, 12)
              png jpeg svg inflate + facetext                                                  (13)
    toolkit = ui.h draw.h alloc.h args.h face.h font.h, optional sdk/libc                      (09a)
  host side: build.sh (zig cc) | tools/ image writers, generators, harnesses | pipeline/ gate |
             launcher/ (C#/WPF zelr.exe)                                                        (01, 14)
```

## Verified by running (2026-09-26, on a Windows 11 host)

- **Toolchain.** Git for Windows 2.55 (Git Bash), Python 3.11.8, Zig 0.16.0, and QEMU 11.1.0 installed per
  user in `%LOCALAPPDATA%\Programs\qemu`.
- **`bash build.sh`** succeeded in 55 s. It built 46 ring 3 programs, `cdboot.bin` (1,592 B), `mbr.bin`
  (131 B), `trampoline.bin` (200 B), `BOOTX64.EFI` (11,776 B), `zelr.elf` (11.3 MB) and `zelr.bin`
  (9.97 MB). The only warnings are unused static functions in `jsrun.h` and `jsdom.h`.
- **`bash run.sh -T`** (i440fx, `-m 64`, one CPU, rtl8139, IDE): **556 passed, 0 failed**. It skipped the SMP
  checks, MCFG and sound, which is expected on that machine.
- **`bash pipeline/gate.sh full`**: **every one of its 47 steps passed, in 8 min 44 s** (GATE_EXIT=0). The
  selftest reached 560 on the gate machine (`-m 256`) and 568 each on q35 and on NVMe. All four ISO boot
  paths passed, as did every desktop, USB, sound, network, https, browser, SMP, crash-safety, SDK and libc
  harness. QEMU 11.1 accepts the harness's `server,nowait` monitor syntax.
- **Reproductions** (scripts in [repro/](repro/)):
  - the network freeze (finding 12);
  - JS `call`/`apply`/`bind` (finding 13).

## Since 6048716

What has changed in the tree since the atlas was written, newest first. File:line references in the
numbered files are still to 6048716; where they disagree with this list, this list and the code win.

### 0.49.1: a power-cut check that tossed a coin

- **crashcheck (14 §10 U, 04 §8).** "Both A and B turn up across six power cuts" was six coin tosses
  once a write took milliseconds, all alike about one run in 32, and it failed the 0.49.0 gate once.
  crashwrite fills each copy with a generation number, carried on across boots, and says which it
  finished; every survivor must be no older than the last one it said. Three runs on the working
  build left the last reported copy or the one after it every time; with rewrites that skip the
  commit, every survivor was generation 0 against up to 106 reported. The usage line and gate.sh's
  "Six boots" (it is fifteen) are corrected.

### 0.49.0: the console that cost a third of the self test

Each change has a check that was run against a build broken for it alone and seen to fail there; the
cost was measured by counting the cycles `kputc` spends drawing, over a whole selftest run, in a
scratch build.

- **Scrolling (05 §3.14).** Once the text reached the bottom, every line moved the 3 MB back buffer
  up and sent the whole screen. It jumps by a quarter of the rows now.
- **Characters.** Glyphs are drawn a back-buffer row at a time rather than through 128 `fb_put`
  calls, and the cells a character touches go to the card as one rect rather than three.
- **The pointer.** It came off and went back for every character, wherever it was; now only when it
  overlaps the cursor's rows or the character scrolls (`mouse_over`).
- Checks in `[video]` (`test_console`, 4): whole-screen sends over two screens of newlines
  (`fb_frames`), one rect a character (`fb_rect_flushes`), the pointer left alone by text far from it
  (`mouse_hides`), and a drawn 'A' against the font. The first version of the pointer check read its
  count after a PASS line had been printed, which can scroll, so it depended on where the cursor
  happened to be; it reads both counts before reporting now.
- Measured: the console took 12.4 billion of a selftest run's 36.8 billion cycles, a third; now 1.5
  billion of 26.3 billion.

Counts after 0.49.0:
- selftest 662 (pc, 64 MiB), 668 (256 MiB), 675 (q35), 689 (`-smp 4`), in 51 sections;
- gate full 53 steps.

### 0.48.0: the disk asked for a sector at a time

Each change has a check that was run against a build broken for it alone and seen to fail there; the
gains were measured by counting the commands each operation hands the drivers (`blk_io`, shown in
`/sys/devices`) and the cycles it takes, on the ATA machine (pc) and the AHCI one (q35).

- **ATA flushed after every write (04 §3).** `ata_write` ended in FLUSH CACHE, a host fsync each on
  QEMU; it waits for the drive and checks ERR/DF now, as AHCI always did. Callers flush where the
  order matters. `[fat]` compares the drive's flushes (`ata_flushes`) with the ones asked for.
- **Runs of clusters (04 §4.3).** File data is read and written a run of neighbouring clusters to a
  request through a 4 KiB-aligned 32 KiB staging buffer, never the caller's (a program's memory, which
  the xHCI refuses). `fat_format` zeroes its areas the same way.
- **The table (04 §2).** Allocating a file's chain and freeing one hold their changes in the cached
  sector and write it once a sector (`fat_defer`); the order of writes is unchanged.
- **Directory sectors (04 §2).** Eight kept (`dcache`); fat.c's own writes are followed exactly, and
  anyone else's write (`blk_writes` moving) drops them all. Not checked: that `vol_write` drops a kept
  sector it overwrites, which cannot be seen through the calls (a directory must be empty to go).
- Measured, before and after: writing 256 KiB, 1023 writes and 3761 Mcycles on ATA (116 on AHCI), now
  11 writes and 120 Mcycles (16 on AHCI, 67 writes); reading it, 513 reads, now 8 (64 on AHCI);
  listing forty files, 2045 reads and 559 Mcycles, now none and 2; writing forty small files, 2740
  reads and 1775 Mcycles, now 2 reads and 334; deleting them, 1151 reads, now none.

Counts after 0.48.0:
- selftest 658 (pc, 64 MiB), 664 (256 MiB), 671 (q35), 685 (`-smp 4`), in 51 sections;
- gate full 53 steps.

### 0.47.0: a download fifty times faster

Each change has a check that was run against a build broken for it alone and seen to fail there; the
gains were measured on the guest's own clock and on QEMU's capture of the card's traffic.

- **A measurement (06a §3.10).** `fetch` prints how long the exchange took, timed with the guest's
  ticks from `tcp_open` to the close: timing it from the host had measured the harness typing.
- **The e1000 ring (06a §10 M2).** 128 descriptors rather than 32. slirp sends a whole 64 KiB window,
  45 frames, in 0.37 ms, and 31 usable descriptors lost the last 13 of every burst; each loss then cost
  slirp's retransmission timer, 1.5 s. 250,000 bytes took 800-1000 ms and take 10-20 ms (a 10 ms tick),
  with nothing resent and nothing out of order. webcheck counts the server's resends in the capture: 73
  with the old ring.
- **Bytes past a hole are kept (06a §3.9).** `take`/`hold`/`absorb`: in place in the receive buffer, up
  to 8 stretches, and a FIN past a hole waits for it. Measured honestly: with the old ring it made no
  difference (780-990 ms), because the losses came 13 at a time and a sender without selective
  acknowledgements repairs one a round; it is kept because a single loss then costs one resend rather
  than a timer. New self test section `[tcp]`, 8 checks, fed by hand across the sequence wrap.
- **Waiting for the network (06a §10 D1).** `net_wait` sleeps on a channel `net_receive` wakes rather
  than for a whole tick. On the wire: with the tick wait two of five connections took 18.3 and 27.9 ms,
  sitting out ticks; with the wake none of ten took more than 13.4 ms (4.5 ms of it the server).
- **The send side (06a §10 M1, 06b).** A SYN offers MSS 1460 (webcheck reads it from the capture), and
  a TLS record goes to TCP in one write, header and body together (two `[tls 1.3]` checks).

Counts after 0.47.0:
- selftest 652 (pc, 64 MiB), 658 (256 MiB), 666 (q35), 679 (`-smp 4`), in 51 sections;
- gate full 53 steps.

### 0.46.0: drawing a frame ten times cheaper

Each change has a check that was run against a build broken for it alone and seen to fail there; the
gains were measured by switching each off in turn.

- **A measurement (07).** `composite()` counts the cycles it spends drawing (`rdtsc`, not the send):
  /sys/screen `draws` and `drawmc`, printed by framecheck.
- **The wallpaper (07 §3.9.12).** A still wallpaper is drawn once into a copy, keyed by everything it is
  drawn from, and frames start from the copy; the copy is taken only with four times its size free.
  536 million cycles a frame without it, 51.5 million with it, for framecheck's pointer moves, and the
  desktop drew 111 frames in the moves rather than 56.
- **Window frames (07 §10 16).** The hairline and refill touch only the ring (`fb_round_ring_aa`); solid
  runs of rounded rectangles are written through the row (`fb_row`). 70.8 million cycles a frame with the
  old three passes, 51.5 with the ring.
- Checks in `[graphics]`: the ring equals the old passes pixel for pixel; solid runs equal a per-pixel
  reference; the copy equals a fresh draw; the copy follows a desktop colour change (first written
  against bloom, which does not use that colour, where it could not fail: now against the gradient).

Counts after 0.46.0:
- selftest 642 (pc, 64 MiB; the wallpaper checks SKIP there), 648 (256 MiB), 656 (q35), 669 (`-smp 4`);
- gate full 53 steps.

### 0.45.0: the file manager's harness, keys that arrive together, and checks that waited for time

Each change has a check that was run against a build broken for it alone and seen to fail there.

- **Files (10).** ctrl+c, ctrl+x, ctrl+v and F2, sharing `take`/`begin_rename` with the menus, and every
  key of a frame rather than the first. `tools/filescheck.py` (gate `filestest`, 11 checks) copies,
  renames and moves from the keyboard and reads the result off the disk image while the guest runs.
  Failed with ctrl+c and F2 made to do nothing, and with Files taking only the first key of a frame.
- **Keys to programs (07).** The desktop's loop takes every waiting key each pass rather than one, so
  typing reaches a program at once rather than at the frame rate: eighteen keys typed quickly were acted
  on 1.4 s after the first, against 8.9 s with one key a pass. That made the first-key-only programs
  (Notes, the calculator, Settings, blackjack, poker) drop keys, so they walk `in.keys` too.
- **Harnesses (14 §10 N, V).** iso_test.sh checks the line `cat` printed rather than the echoed command
  (failed on all four paths with the file not read back). livecheck and gamecheck wait for what they
  check. volcheck compares the quietest burst with the loudest. `Guest` reads the serial line in blocks.
  The harness still types at 50 ms a character, deliberately: bursts would cut its tolerance for a
  guest stall from 800 ms to tens of milliseconds.

Counts after 0.45.0:
- selftest unchanged (640, 644, 652, 665);
- gate full 53 steps.

### 0.44.0: the window being used, alt+tab, and Escape

Each change has a check that was run against a build broken for it alone and seen to fail there.

- **The window being used (07 §10 6).** `focus_index()`/`focused_window()`, the frontmost non-minimised
  window, is what is drawn focused, highlighted on the dock, typed into and acted on by alt+m/f/q/arrows
  and by a click on its own chip. alt+m on a minimised top window used to minimise it again.
- **alt+tab (7).** A walk over a snapshot of the stack while alt is held, ended by letting go
  (`kbd_alt()` every loop pass), a button, another key, or the window closing; the one reached ends in
  front with the previous front behind it. It raised the window behind the front one, so three windows
  could never all be reached.
- **Escape (14).** `wm_key`: Escape closes find, the launcher, the context menu, the volume or network
  panel; then goes to the focused window if it asked (`SYS_WIN_ESCAPE` 64, `win_want_escape`: the
  browser, Files, the calculator, blackjack); else leaves. alt+Escape always leaves. gamecheck and
  netcheck leave with alt+Escape.
- **The dock (5).** Drawing and hit test share `chip_width`. The disagreement was latent: the regular and
  bold 15 px faces have identical advances.
- **Leaving (13, partly).** The find bar and network panel are closed on the way out.

Counts after 0.44.0:
- selftest 640 (pc, 64 MiB), 644 (256 MiB), 652 (q35), 665 (`-smp 4`); 64 live system calls;
- gate full 52 steps.

### 0.43.0: the JavaScript engine's numbers, `new`, key order, errors and memory

Each change has a check that was run against a build broken for it alone and seen to fail there
(jstest, 202 cases, 58 new).

- **Numbers (12 §10 B5).** `userland/jsnum.h`: correctly rounded reading (one exact operation for
  fifteen digits and a small power of ten, exact big integers otherwise) and shortest round-trip
  printing. The lexer, `Number()`, `parseFloat` (now the longest decimal at the front) and JSON read
  through it. 0.1 + 0.2 prints 0.30000000000000004; 0.3 reads as the double nearest it.
- **`new X().y` (B3).** `js_parse_new`: the constructor is a member expression and its arguments are
  the first parentheses. The parser's `J->nodes[n].x = parse(...)` stores now go through a local
  (B19), because parsing can move the node array.
- **Key order and hidden properties (B13, B14).** An insertion-order list on every object;
  `js_own_keys` gives indices ascending then insertion order, enumerable only, to for-in,
  `Object.keys`/`values` and JSON. The engine's `__ctor__` and the like are non-enumerable
  (`js_set_hidden`). JSON leaves out properties whose value is undefined or a function.
- **Errors (B21).** Error, TypeError, RangeError, ReferenceError, SyntaxError and EvalError; the
  engine throws them rather than strings; `instanceof Error` holds for all six. Found on the way:
  `return f()` replaced a throw from f with undefined (B26).
- **Memory.** A finished call's scope that no function value captured goes back to size-class free
  lists; `arguments` is built only for a function whose text names it; string and array methods
  called where they are fetched share one native each; the names looked up on every call are
  interned. Before, each call kept about 600 bytes and a page ran out at about 40,000 calls; jstest
  now makes 200,000 calls, string method calls and pushes.

Counts after 0.43.0:
- selftest unchanged (626, 630, 638, 651); jstest 202;
- gate full 52 steps.

### 0.42.0: other systems' disks, names past ASCII, TLS manners, SVG paint, the colour order

Each change has a check that was run against a build broken for it alone and seen to fail there.

- **Foreign FAT volumes (04 S20).** `fat_made_here()` (the OEM field says "ZELR"). On a volume without it
  `finish_mount` skips the reclaim and `layout_init` makes, seeds and empties nothing; the shell starts in
  `/` when there is no `/home`. Check: `gpt_test.sh`, a GPT partition and a whole disk made as "MSDOS5.0"
  with a lost chain and `TMP/KEEP.TXT` (`mkfat.py --foreign`, `mkgpt.py foreign`): every byte outside the
  written file unchanged.
- **FAT S15, S16, S17.** `fat_free_bytes` is u64; `fat_list` copies the terminator; long names are UTF-8
  above and UTF-16 on the disk both ways, any byte past ASCII makes a name long, invalid UTF-8 is refused.
  Checks: `[fat]` mounts a 16 GiB FAT32 volume that exists only as answers to reads, lists a 63-byte name
  into a filled buffer, writes and reads "café au lait.txt" and "大.txt" (first byte 0xE5).
- **TLS (06b §10.4, 10.7, 10.8).** Fatal alerts on failure, in the clear before there is a key and sealed
  after, never in answer to the server's own; KeyUpdate read, and answered when asked, under the old key;
  a served copy of a store root is set aside before its dates are looked at. Checks: `[tls 1.3]` byte for
  byte against traced keys and independently worked next secrets; `[x509]` an expired root copy passes and
  an expired intermediate still fails.
- **libc (09a §10.2 #3, #4).** `fseek(SEEK_CUR)` from the logical position; `ftell` counts buffered writes.
  Checks: libccheck, two new.
- **SVG (13 §10 3-6, 11).** Paint is inherited (fill, stroke, their widths, opacities and rule, up to the
  root), opacity multiplies down; `rect` and `polygon` strokes close; `<defs>`, `<clipPath>`, `<mask>`,
  `<symbol>`, `<pattern>`, `<marker>` are not drawn; `transparent` is no paint. Checks: svgtest, ten new.
  The closing bug was found by one of the inheritance checks failing on the fixed build.
- **DOM (11 §10 #34).** The end tag of an element the tag table does not know closes it
  (`dp_pop_to_name`). It closed nothing, so after a custom element or an SVG `<g>` every sibling nested
  inside; with inheritance in, a group's fill would have leaked into everything after it. Found by the
  `<defs>` check failing on the fixed build. Checks: layouttest and svgtest.
- **UEFI colour order (01 §10 #2, 05 B7).** `handoff_t.fb_format` at 3296 (the structure is now 3304 bytes),
  set by the loader; fb.c swaps red and blue on the copy to the card only. Check: `[video]` through
  `fb_test_orders`. Not run on a real RGB panel: QEMU's OVMF offers BGR modes only.

Counts after 0.42.0:
- selftest 626 (pc, 64 MiB), 630 (256 MiB), 638 (q35), 651 (`-smp 4`);
- gate full 52 steps.

### 0.41.0: memory and waiting under preemption, the clocks, FP state, and the checks themselves

Each change has a check that was run against a build broken for it alone and seen to fail there.

- **Heap, PMM, task ring (02 §10 B8, B9, B10, B28).**
  - `spin_lock_irqsave` (smp.h) around every heap and PMM change: IF off for this processor's own ticks
    (the collector frees memory inside the timer interrupt), a spinlock for processors without the kernel
    lock (boot, `smp_run` work). `heap_check` walks and adds up the heap.
  - `ring_insert` (sched.c) joins a task with IF off, `next` written before the link.
  - `kmalloc` refuses sizes above 0xFFFFFF00 (B8). `can_collect` requires `on_cpu < 0` (B28).
  - Checks: probes (`heap_test_probe`, `pmm_test_probe`, `sched_test_probe`) yield where a tick could land
    while a rival task asks for the same thing; a two-processor heap hammer with one side off the kernel
    lock; "the heap still adds up after all of that" at the end of the self test.
- **Waiting for input (03, console_pause).** `input_wait`/`input_wake` (keyboard.c) block the waiting task
  until a key, a serial byte or the mouse; console reads, the kernel shell and the desktop loop use it.
  Check: `[userspace]` starts a flat ring 3 program that reads the console and wants it blocked and nearly
  never scheduled until a key is injected.
- **Clocks (02 §10 B5, B6, B27).** PIT mode 2 for the calibration, checked against one CMOS second; a gate
  for vector 0xFF (`isr255`, counted, no EOI); the PIT tick counted before the lock and taking the try-lock
  path.
- **FP state (09a §10.2, 18 and 21).** The signal frame carries the FXSAVE image (MXCSR masked by
  `mxcsr_mask` before FXRSTOR) and the handler starts clean; exec calls `task_fpu_reset`; fork copies the
  parent's live state into the child's aligned area. Checks: sigtest, five new.
- **Serial.** FCR 0xC7: a fourteen-byte trigger, because QEMU accepts only as many bytes as the trigger and
  its stdio backend drops the rest. Check: `tools/serialcheck.py`, a gate step.
- **The checks (14 §10 B, N, S, AB).** The gate's build step reads build.sh's exit status and build.sh runs
  `set -eo pipefail` and deletes the old image first; check_sse is a gate step again with its false alarms
  fixed; defaultcheck compares what is still duplicated and found Settings deleting `text_dim`, so Settings
  keeps lines it does not manage (`keep_unmanaged`, and setcheck checks it); shell_test matches whole lines,
  which exposed a path in it that had never worked; the gate runs the self test on four processors.

Counts after 0.41.0:
- selftest 609 (pc, 64 MiB), 613 (256 MiB), 621 (q35), 634 (`-smp 4`);
- gate full 52 steps.

### 0.40.0: multiprocessor memory, window lifetimes, JavaScript, and data loss

Each change has a check that was run against a deliberately broken build (or, for the ring 3 fixes, the
committed old sources) and seen to fail there.

- **Multiprocessor memory (finding 14 and 02 §10 B1-B4).**
  - `paging_current_directory()` reads CR3; the global `current_pml4` is gone. `paging_init` builds the
    kernel tables by name, since CR3 is still the loader's then.
  - Resolved copy-on-write and demand faults leave through `leave:` in `isr_dispatch`, which releases the
    lock and delivers signals. A lock still held on entry from ring 3 is counted as `kept` in `/sys/cpu`.
  - `paging_protect_writes()` sets CR0.WP on the boot processor (end of `paging_init`) and every AP
    (`ap_main`). A ring 0 fault at a user address that copy on write cannot resolve ends the calling program
    (`on_behalf_of_program`).
  - The ragged ends of usable regions above 64 MiB are `pmm_reserve`d; there were 480 such frames at 256 MiB.
  - Checks:
    - `/bin/cputest` in ring3check and in smpcheck (four processors);
    - `[physical memory]` "every frame that can be handed out is mapped".
- **Frames that waited for a busy processor (02 §10 B12).** Found when smpcheck's new cputest step lost its
  typing under a loaded gate. `smp_helper` now also requires the AP to be in its idle task
  (`sched_cpu_idle`); the AP claims a job with a compare-and-swap; `fb_flush` takes back a half nobody
  claimed (`smp_take_back`) and does it itself instead of spinning up to 20,000,000 `pause`s with IF=0 and
  the lock held. The slot also has `running`, from the claim until the job returns, because claiming cleared
  the only thing the scheduler looked at: a flusher preempted mid-wait freed the lock and the helper was given
  a program halfway through its half. `smp_work_pending`, `smp_run`, `smp_busy` and `smp_wait` include it,
  so `smp_wait` means finished. `/sys/screen` gains `stalled`.
  - Checks: smpcheck, with a spinner on every processor, "no flush waited out a processor that was running
    a program"; and in `[processors]`, with a held job and more ring 3 spinners than processors, "a processor running
    a program is not offered as a helper", "work taken back before it started never runs" and "a processor in
    the middle of handed work is not given a program". Each of the three `[processors]` checks failed on a build
    broken for it alone. smpcheck's counter caught the idle-test break only before `running` existed: with
    it, a preempted flusher lets the helper reach idle and finish, so that break showed as a cputest failure
    instead.
  - The gate now runs the self test with `-smp 4` too ("the same checks on four processors"); before, the
    multiprocessor half of `[processors]` ran only by hand.
  - smpcheck also waits for cputest's whole verdict (it had read `CPUTEST_PA`), and its `ps` check looks at
    what `ps` printed (it matched the earlier `bg /bin/spin` lines and could not fail).
  - New finding by reading: 02 §10 B28, the collector does not check `on_cpu`.
- **Window lifetimes (finding 6).**
  - `wm_close` clears `resizing`.
  - `held_button` records which button started a hold, and releasing that button ends it.
  - Surfaces are mapped `PTE_BORROWED`: `copy_table` leaves them out of a fork and `free_table` never frees
    their frames. `sys_exec` calls `winsrv_release` before freeing the old space.
  - `apply_snap` sets `maximized` only when the resize went through.
  - Surfaces go up to 2560x1600 with a 16 MiB step (`WINSRV_MAX_W/H`).
  - Checks: `[window server]` `test_window_lifetimes`, 12 checks, through hooks `wm_test_holds`,
    `wm_test_pointer`, `wm_test_begin_resize` and `wm_test_maximize`. The existing window-server checks now
    run in a scratch address space, answering open question 2 in 07.
- **JavaScript (finding 13, 12 §10 B1, B2, B5).**
  - call/apply/bind take the target from `this`. Natives can read `J->callee`, which is how a bound function
    finds its own record; bound arguments are kept.
  - The for clause parses with `no_in`, and the target form sets `d = 0`.
  - `js_num_text` prints 15 significant digits, rounded (17 for whole numbers of 2^53 or more below 10^21).
  - jstest now counts every case: 162.
- **Data loss in programs (09b, 10 §10).**
  - The terminal's cp copies in chunks and checks the size; mv renames when it can.
  - Files copies and renames the same way.
  - Notes' buffer grows, and a file it could not read whole cannot be saved.
  - Check: `tools/bigfilecheck.py`.
- **libc (finding 9).**
  - atan uses y = x^2/(1+x^2).
  - fputc flushes a full buffer before storing.
  - stdin, stdout and stderr are descriptors 0, 1 and 2.
  - `clock()` scales ticks by the rate in `/sys/uptime`.
  - Check: libccheck, 22 checks.
- **PNG (finding 10).** The chunk bound is checked in two steps. Check: pngtest.

Counts after 0.40.0:
- selftest 595 (pc, 64 MiB), 599 (256 MiB), 607 (q35), 614 (`-smp 4`) -- as 0.40.0 shipped;
- 48 ring 3 programs, which is `SYSFS_MAX_PROGRAMS`;
- gate full 50 steps.

### 0.39.0: six of the findings below, fixed

Each change has a check that was run against a deliberately broken build and seen to fail there.

- **The network freeze (finding 12).**
  - `net_wait()` in `net.c` sleeps a tick (`task_sleep(1)`) where every wait in `net.c` and `tcp.c` used
    to spin, so ticks advance inside a system call and other tasks run.
  - Because a network call can now be interleaved with another:
    - sockets are reserved before the wait (`sock_take` in `syscall.c`);
    - DNS is taken one asker at a time (`dns_take`);
    - `take_slot` in `tcp.c` claims atomically;
    - the chain and leaf in `tls.c` are per session (they were statics).
  - A task ended from outside -- `sys_kill`, or a signal's default action, which runs inside the scheduler
    -- goes through `syscall_abandon`. It drops sockets, TLS sessions, half-open connections (`tcp_abandon`
    by owner pid) and its turn at delivery or DNS (`net_abandon`), all without waiting.
  - Retransmission inside calls works now, since `pump_one` sees time pass.
  - Check: `tools/hangcheck.py` (gate `hangtest`), with `/bin/hangtest`. `repro/nethang.py` now reports
    NOT FROZEN.
- **TLS CertificateVerify (finding 1).**
  - `tls_flight_step` enforces extensions, certificate, signature, finished, each once and in that order.
  - A finished message with no signature before it fails as "the server never proved it holds the
    certificate's key".
  - Check: `[tls 1.3]` `test_tls_order`, 8 checks.
- **Auto-format (finding 2).**
  - `diskfs_format` formats only a blank disk (the first 128 KiB and the last sector all zero) or the volume
    already mounted across the whole disk.
  - A refused table is left alone and the refusal is logged.
  - Check: `tools/gpt_test.sh` now compares the whole image before and after, and has a
    Linux-style unpartitioned disk case.
- **FAT (finding 3).**
  - `dir_find` compares 8.3 names only for names that fit 8.3.
  - A rename to a file's own alias keeps the file.
  - `vfs_rename` refuses two volumes.
  - The volume selection lasts one VFS call (`unroute`).
  - `fat_format` always means the boot disk.
  - Delete and rmdir write the entry and flush before freeing, and rmdir now drops its long name entries
    too (S14).
  - `fat_reclaim` frees nothing unless the walk read every directory and every table sector, never frees a
    bad-cluster mark, and queues each directory once.
  - The mount bound uses the volume's own disk size (S10).
  - Check: `[fat]` `test_fat_names`, 18 checks, using two self-test fault hooks: `fat_test_writes_left`
    and `fat_test_subdirs_unreadable`.
- **Flushes (finding 4).**
  - `ahci_flush` issues FLUSH CACHE EXT (a command with no PRDT).
  - `usbdisk_flush` issues SYNCHRONIZE CACHE(10), and accepts only ILLEGAL REQUEST as "no cache".
  - `vol_flush` in fat.c flushes the volume's own disk.
  - `diskfs_flush` flushes the stick as well.
  - Check: `tools/mountcheck.py` watches QEMU's own trace of `ide_bus_exec_cmd` (cmd 0xea) and
    `scsi_req_parsed` (command 53).
- **Kill and signal (finding 5).**
  - `may_end` in `syscall.c`: only tasks with `user` set, and never idle.
  - Monitor offers Stop only for programs.
  - Check: in `hangcheck.py`.

Counts after 0.39.0:
- selftest 582 (pc, 64 MiB), 586 (256 MiB), 594 (q35), 597 (`-smp 4`), still 50 sections;
- 47 ring 3 programs;
- gate full 48 steps.

Findings 1-5 and 12 below are kept as they were written, for the reasoning.

## Most important findings, across all areas

These are the verified-by-reading defects that matter most, most severe first. The details and the
reasoning are in each file's §10.

1. **FIXED in 0.39.0. TLS server authentication can be bypassed.** CertificateVerify is never required
   (`kernel/tls.c:681-758`), so a man in the middle holding a real site's public chain can intercept any
   https connection. [06b §10.1]
2. **FIXED in 0.39.0. The first-boot auto-format can wipe a disk that is not blank.** Any disk whose partition table the
   kernel rejects (a bad GPT CRC, an unreadable header, a protective MBR only) is formatted as disk 0. On a
   laptop booted from a stick, disk 0 is the internal drive. [04 §10 S1]
3. **FIXED in 0.39.0. FAT name handling can hit the wrong file.**
   - The 8.3 alias compare makes `chapter10.txt` resolve to `chapter1.txt` (fat.c:1202-1211).
   - Renaming a file to its own alias deletes it.
   - A cross-volume rename is not rejected; this is reachable from ring 3.
   - `format` after any `/usb` access writes onto the stick.
   - Deletes are not crash-ordered.
   - Reclaim can free live data under memory pressure.
   [04 §10 S2-S7]
4. **FIXED in 0.39.0. The crash-safety promise is weaker on real hardware than stated.** The AHCI and USB-stick "flush"
   are no-ops, and every flush goes to disk 0. [04 §10 S8, S9]
5. **FIXED in 0.39.0. Any program can kill or signal any task, including kernel services** (`sys_kill`, `sys_sigsend`
   have no ownership check). [03 §10.5]
6. **FIXED in 0.40.0. Window manager lifetime bugs.**
   - Closing a window while it is being resized leaves a dangling `resizing` pointer.
   - Right-button capture sticks until a left release.
   - fork and exec treat window surface pages as user pages.
   [07 §10]
7. **FIXED in 0.41.0. FP state is not isolated.** Signal handlers can clobber FP/SSE state, and exec keeps the FPU state
   (ring 3 is built with SSE on). [09a §10.2 #18, #21]
8. **FIXED in 0.41.0 (defaultcheck and the build step; the others as each was touched). Tests that cannot fail.**
   - `tools/defaultcheck.py` matches nothing in settings.c and passes on three vacuous checks (confirmed
     by reading its output in the gate).
   - The gate's build step only fails on the literal word "error" [01 §10 #5].
   - Several checks pass for other reasons than their comments say [04 D11, 06b §10.10].
9. **FIXED in 0.40.0. libc correctness.** `atan` is wrong, `fputc` can overflow into the next FILE, `stdin` is always EOF,
   and `clock()` is ten times too small. [09a §10.2]
10. **Untrusted input in the browser's decoders.** The PNG chunk bounds check wraps (png.h:146; FIXED in 0.40.0), and
    there are several SVG and JPEG conformance gaps (the SVG paint ones FIXED in 0.42.0). [13 §10]
11. **UEFI.** The GOP pixel format (RGB vs BGR) is not carried in the handoff (FIXED in 0.42.0), and the handoff
    address is fixed at 0x70000 with no fallback. [01 §10 #2, #8]
12. **FIXED in 0.39.0. A network syscall whose peer never answers freezes the machine. REPRODUCED 2026-09-26**
    (`repro/nethang.py`, prints FROZEN and exits 1 while the bug stands).
    - **Cause.**
      - int 0x80 is an interrupt gate (`idt.c:63`, flags 0xEE), so interrupts are off for the whole
        syscall, and `ticks` only advances in the IRQ0 handler (`timer.c:18`).
      - Every network wait keyed on `timer_ticks()` therefore never times out inside a syscall: ARP
        (`net.c:162-172`), TCP open, send and receive (`tcp.c:426-494`), DNS (`net.c:778`), DHCP and
        ping. Frames still arrive because `net_poll()` polls the card, which is why the ordinary path
        works.
    - **Reproduction.**
      - Setup: QEMU `-netdev user,restrict=on` (DHCP gives no gateway, and nothing outside answers);
        `exec /bin/wiretest http://93.184.216.34:80`.
      - The machine stops for good: `ticks` frozen at 945, and every monitor sample shows CPL 0 with
        IF clear. The shell never answers again.
      - Control: the kernel shell's own `fetch` to the same address (a kernel task, interrupts on)
        gives up after 2.6 s with "could not connect".
    - **Real-world trigger.** On a real network, a firewalled port, a silent DNS server or a dead
      neighbour is enough. Any ring 3 program, the browser included, can then freeze zelr.
    - **Reproduced again on an ordinary network** (with the gateway set):
      - Setup: a host server accepted the connection and never sent a byte, like a hung web server.
      - `wiretest` then waited in `tcp_recv`.
      - Result: ticks frozen (877 -> 877), CPL 0 with IF clear, and the shell dead.
    - Atlas 06a also notes that a kernel task preempted while it owns frame delivery can leave a
      syscall spinning. [06a §10]
13. **FIXED in 0.40.0. JavaScript `call`, `apply` and `bind` do not work. REPRODUCED 2026-09-26** (`repro/callbind.py`, prints BROKEN and exits 1 while the bug stands).
    - `jsprobe` against a page with one script per method: the control script ran, and all three stopped
      with "this is not a function".
    - Cause: the wrappers look up `__fn__` on the receiver `t` (the target function) instead of on the
      wrapper itself (`jsrun.h:450-460, 573-622`).
    - This is the feature commit 3f4cdb5b says it added for Google's front page, and nothing tests it.
      [12 §10]
14. **FIXED in 0.40.0 (with 02 B1, B3, B4). SMP: the big lock stays held into ring 3 after a copy-on-write or demand fault** (verified by
    reading).
    - `isr_dispatch` returns early at `idt.c:202-203` and `220-221`, skipping `kernel_lock_release()`
      at `:287`, and it skips signal delivery on those returns too.
    - This is not a deadlock: the next kernel entry on that CPU sees `kernel_lock_held_here()` and
      releases on exit. But every other CPU is kept out of the kernel until then.
    - Atlas 02 lists further SMP defects, not yet verified:
      - a single global `current_pml4` while CR3 is per CPU;
      - CR0.WP never set, so kernel writes bypass copy-on-write;
      - partially mapped 2 MiB chunks above 64 MiB handed out as frames.
      [02 §10]
15. **Network odds and ends.**
    - DHCP sends one DISCOVER and one REQUEST with no retry.
    - TCP never sends RST or MSS.
    - The ARP cache never expires.
    - `fetch ... SAVEAS` writes to the RAM filesystem.
    - A USB RNDIS adapter can hang boot and is never detached.
    [06a §10]

Documentation drift is catalogued in 15 §9.4 (by document) and in each file's §10.

## Glossary
- **handoff_t**: the one structure every loader hands the kernel (memory map, framebuffer, ACPI pointer,
  command line). Magic "ZELR64HF". `include/handoff.h`.
- **The big lock**: the single kernel lock held whenever a CPU is not executing ring 3. Whether it is
  released on the way out is decided by the interrupt frame being returned through (`kernel/idt.c`).
- **Knob**: one entry of `KNOBS[]` in `kernel/theme.c`, a numeric or switch setting stored in `/zelr.cfg`.
- **Face**: one size and weight of the generated typeface (a coverage table plus metrics).
- **Surface**: a window's pixel buffer. The program draws into `pixels`; `win_commit` copies them to
  `shown`, which the compositor reads.
- **Black box**: the boot log kept in sectors reserved at the front of a volume zelr formatted itself,
  readable as `/sys/boot` and `/sys/lastboot`.
- **Gate**: `pipeline/gate.sh`, the mechanical verification tiers `fast`, `screen` and `full`.
