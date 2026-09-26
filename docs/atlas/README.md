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
  ========================= int 0x80, 63 system calls (include/syscall.h = sdk/zelr.h) =========
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
- **`bash pipeline/gate.sh full`**: **every one of its 53 steps passed, in 8 min 44 s** (GATE_EXIT=0). The
  selftest reached 560 on the gate machine (`-m 256`) and 568 each on q35 and on NVMe. All four ISO boot
  paths passed, as did every desktop, USB, sound, network, https, browser, SMP, crash-safety, SDK and libc
  harness. QEMU 11.1 accepts the harness's `server,nowait` monitor syntax.
- **Reproductions** (scripts in [repro/](repro/)):
  - the network freeze (finding 12);
  - JS `call`/`apply`/`bind` (finding 13).

## Most important findings, across all areas

These are the verified-by-reading defects that matter most, most severe first. The details and the
reasoning are in each file's §10.

1. **TLS server authentication can be bypassed.** CertificateVerify is never required
   (`kernel/tls.c:681-758`), so a man in the middle holding a real site's public chain can intercept any
   https connection. [06b §10.1]
2. **The first-boot auto-format can wipe a disk that is not blank.** Any disk whose partition table the
   kernel rejects (a bad GPT CRC, an unreadable header, a protective MBR only) is formatted as disk 0. On a
   laptop booted from a stick, disk 0 is the internal drive. [04 §10 S1]
3. **FAT name handling can hit the wrong file.**
   - The 8.3 alias compare makes `chapter10.txt` resolve to `chapter1.txt` (fat.c:1202-1211).
   - Renaming a file to its own alias deletes it.
   - A cross-volume rename is not rejected; this is reachable from ring 3.
   - `format` after any `/usb` access writes onto the stick.
   - Deletes are not crash-ordered.
   - Reclaim can free live data under memory pressure.
   [04 §10 S2-S7]
4. **The crash-safety promise is weaker on real hardware than stated.** The AHCI and USB-stick "flush"
   are no-ops, and every flush goes to disk 0. [04 §10 S8, S9]
5. **Any program can kill or signal any task, including kernel services** (`sys_kill`, `sys_sigsend`
   have no ownership check). [03 §10.5]
6. **Window manager lifetime bugs.**
   - Closing a window while it is being resized leaves a dangling `resizing` pointer.
   - Right-button capture sticks until a left release.
   - fork and exec treat window surface pages as user pages.
   [07 §10]
7. **FP state is not isolated.** Signal handlers can clobber FP/SSE state, and exec keeps the FPU state
   (ring 3 is built with SSE on). [09a §10.2 #18, #21]
8. **Tests that cannot fail.**
   - `tools/defaultcheck.py` matches nothing in settings.c and passes on three vacuous checks (confirmed
     by reading its output in the gate).
   - The gate's build step only fails on the literal word "error" [01 §10 #5].
   - Several checks pass for other reasons than their comments say [04 D11, 06b §10.10].
9. **libc correctness.** `atan` is wrong, `fputc` can overflow into the next FILE, `stdin` is always EOF,
   and `clock()` is ten times too small. [09a §10.2]
10. **Untrusted input in the browser's decoders.** The PNG chunk bounds check wraps (png.h:146), and
    there are several SVG and JPEG conformance gaps. [13 §10]
11. **UEFI.** The GOP pixel format (RGB vs BGR) is not carried in the handoff, and the handoff address
    is fixed at 0x70000 with no fallback. [01 §10 #2, #8]
12. **A network syscall whose peer never answers freezes the machine. REPRODUCED 2026-09-26**
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
    - Atlas 06a also notes that a kernel task preempted while it owns frame delivery can leave a
      syscall spinning. [06a §10]
13. **JavaScript `call`, `apply` and `bind` do not work. REPRODUCED 2026-09-26** (`repro/callbind.py`, prints BROKEN and exits 1 while the bug stands).
    - `jsprobe` against a page with one script per method: the control script ran, and all three stopped
      with "this is not a function".
    - Cause: the wrappers look up `__fn__` on the receiver `t` (the target function) instead of on the
      wrapper itself (`jsrun.h:450-460, 573-622`).
    - This is the feature commit 3f4cdb5b says it added for Google's front page, and nothing tests it.
      [12 §10]
14. **SMP: the big lock stays held into ring 3 after a copy-on-write or demand fault** (verified by
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
