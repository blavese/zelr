# 15. History and public face of zelr

Scope: how zelr got from its first commit (2026-08-28) to `main` at `6048716a` (2026-09-22), why it is shaped the way it is, and what zelr.org says about it.

Sources: all 150 commit messages, the notes of all 40 GitHub releases, zelr.org as fetched on 2026-09-26 (HTML plus live response headers), and `README.md`, `NOTICE`, `pipeline/` and a few source files in the tree.

Conventions used here:
- All times are UTC.
- The zelr.org releases page prints the short sha of every tag from v0.1.0 to v0.35.0, and that is where the tag-to-commit mapping below comes from. v0.36.0 and v0.37.0 are assumed to sit on the commits titled `zelr 0.36.0` and `zelr 0.37.0`.
- Commit subjects are given word for word because they identify the commits. Commit bodies and release notes are paraphrased, not quoted.
- In the export, the git author field of all 150 commits reads `blaves`. The GitHub owner, copyright holder and boot-banner name is `blavese`, so the export field may be truncated.

---

## 0. At a glance

- **Size and span.** 150 commits by one author, from 2026-08-28 14:12 to 2026-09-22 14:23 (25 days). There are about 51,800 words of commit prose; the median body is about 300 words.
- **Releases.** 40 of them, v0.1.0 to v0.37.0, including three patch releases (v0.6.1, v0.6.2, v0.16.1). v0.37.0 was cut at 2026-09-22 14:02, and two more commits have landed on `main` since.
- **Name.** The system was called **nyx** until `1b634221` (2026-09-14 15:45). v0.11.0 is the first zelr release. The GitHub repository moved from `blavese/nyx` (the v0.6.1 notes still link there) to `blavese/zelr`.
- **Architecture.** For its first two days it was a **32-bit multiboot kernel that QEMU loaded directly**. It got its own BIOS bootloader on 08-29 (`e7511b5d`) and a UEFI loader plus the **64-bit long-mode** port on 08-30 (`bab4aac6`, `2506b9a7`).
- **Licence.** **MIT up to and including v0.16.1**, then **GPL-3.0-or-later from v0.17.0** (`9c5edad5`, 2026-09-16 12:38).
- **Shape of the history.** An **11-day silence** (09-03 07:41 to 09-14 07:26) splits it in two. Before the gap there were 41 commits and 10 releases in 5.7 days. After it there were 109 commits and 30 releases in 8.3 days, peaking at 37 commits and 4 releases on 2026-09-21.
- **AI assistance.**
  - **55 of 150 commits** carry `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`. They are exactly the 55 newest commits, from `0e6e9a27` (2026-09-18 10:57) onward.
  - Earlier than that, Codex reviews found the bugs fixed in `7898701c` (2026-08-29).
  - A Claude Code plus Codex pipeline ran from 08-31 to 09-03 and **landed nothing**.
- **Website.** zelr.org (served by Cloudflare) still advertises **v0.35.0** and repeats figures that are no longer true: 548 checks, 58 system calls, single-connection TCP, no regex or arrow functions in JS, about 70,000 lines, and "No libc". **The site's source is not in the repository.**

---

## 1. Timeline

### 1.1 Pace

| UTC day | Commits | Releases | Notes |
|---|---|---|---|
| 2026-08-28 | 9 | 5 (v0.1.0 to v0.5.0) | first day: kernel, disk, network, ring 3, window manager |
| 2026-08-29 | 5 | 0 | bootloader, VFS, review-driven fixes |
| 2026-08-30 | 13 | 4 (v0.6.0, v0.6.1, v0.6.2, v0.7.0) | own bootloader, then UEFI and the 64-bit port within 4 hours |
| 2026-08-31 | 9 | 1 (v0.8.0) | boot hardening, then the pipeline is introduced |
| 2026-09-01 | 0 | 0 | |
| 2026-09-02 | 3 | 0 | pipeline runs (2 of the 3 commits were written by the pipeline itself) |
| 2026-09-03 | 2 | 0 | pipeline made cheaper, and the fix for the v0.8.0 launcher crash |
| 2026-09-04 to 09-13 | 0 | 0 | **gap** |
| 2026-09-14 | 15 | 3 (v0.9.0, v0.10.0, v0.11.0) | real-laptop hardware, desktop apps, rename to zelr |
| 2026-09-15 | 11 | 2 (v0.12.0, v0.13.0) | first boot on VMware, USB keyboard |
| 2026-09-16 | 14 | 8 (v0.14.0 to v0.20.0 incl. v0.16.1) | most releases in a day |
| 2026-09-17 | 9 | 6 (v0.21.0 to v0.26.0) | apps, trackpad, WPA2 maths, RNDIS, the browser |
| 2026-09-18 | 6 | 2 (v0.27.0, v0.28.0) | PCnet, then TLS 1.3. AI trailers begin |
| 2026-09-19 | 3 | 1 (v0.29.0) | |
| 2026-09-20 | 10 | 3 (v0.30.0 to v0.32.0) | Unix processes, JS, images, network rework |
| 2026-09-21 | 37 | 4 (v0.33.0 to v0.36.0) | peak: SMP scheduling, COW, SDK, signals, mmap |
| 2026-09-22 | 4 | 1 (v0.37.0) | plus the two post-release commits |

- **Average pace.** 6.0 commits per calendar day, 10 per active day (15 active days), and 1.6 releases per calendar day.
- **Gaps longer than 12 hours between commits.**
  - **263.7 h**: `5e7d0904` (09-03 07:41) to `9ef316a2` (09-14 07:26).
  - 44.6 h: 08-31 09:41 to 09-02 06:16, inside the pipeline period.
  - 28.7 h: 09-19 06:02 to 09-20 10:42.
  - The rest are overnight breaks of 12 to 22 h.
- **What the gap looks like from the record.** Nothing says why it happened. Before it, the pipeline was the plan. After it there is no pipeline activity at all, and the first commit back (`9ef316a2`, the black box) opens by saying the first boot on a laptop is the one run with no serial line to watch. Everything after that is aimed at real hardware. The home page presents the day-one brief the same way: keep building until it boots and works on a real laptop.

### 1.2 Eras

| Era | Span (UTC) | Commits | Releases | Character |
|---|---|---|---|---|
| A. nyx, 32-bit | 08-28 14:12 to 08-30 07:53 | 19 | v0.1.0 to v0.6.2 (8) | multiboot kernel under QEMU `-kernel`, then its own BIOS bootloader. Features land at release speed (five releases on day one) |
| B. 64-bit and UEFI | 08-30 08:22 to 08-31 09:01 | 15 | v0.7.0, v0.8.0 | UEFI loader from the spec, long-mode port, one image with four boot paths, boot hardening for strict firmware |
| C. The pipeline experiment | 08-31 09:31 to 09-03 07:41 | 7 | none | two-model autonomous pipeline plus the gate. Nothing landed from it |
| (gap) | 09-03 07:41 to 09-14 07:26 | 0 | none | no activity |
| D1. Laptop hardware, rename | 09-14 07:26 to 15:53 | 15 | v0.9.0 to v0.11.0 | black box, ACPI under UEFI, GPT, IOAPIC, FAT32, NVMe, desktop apps, outline font, nyx becomes zelr |
| D2. Laptop and desktop sprint | 09-15 04:27 to 09-17 16:15 | 32 | v0.12.0 to v0.25.0 (15) | VMware boot, xHCI/USB, memory map, audio, USB storage, GPL, desktop redesigns, trackpad, WPA2 maths, RNDIS |
| D3. The web | 09-17 16:56 to 09-19 06:06 | 11 | v0.26.0 to v0.29.0 | browser, PCnet, full TLS 1.3 stack written from scratch. **AI trailers start at `0e6e9a27`** |
| D4. Unix, the real web, hardening | 09-20 10:42 to 09-22 14:23 | 51 | v0.30.0 to v0.37.0 (8) | fork/exec/pipes/signals, JS, image decoders, SMP scheduling with one kernel lock, COW, SDK, fault isolation, mmap, libc |

### 1.3 Release by release, with the commits in each

Each entry gives the tag commit, a paraphrase of the headline content, and every commit since the previous tag (time, sha, subject).

#### Era A. nyx, 32-bit

**v0.1.0**, 08-28 14:57, tag `e0a221cc`
- Multiboot kernel in 32-bit protected mode:
  - GDT and TSS, a 256-entry IDT, 8259 PICs, bitmap physical memory manager, two-level paging, coalescing heap, preemptive round-robin scheduling.
  - Drivers for VGA text, PS/2, PIT and 16550.
  - A flat in-memory filesystem and a shell on keyboard or serial.
- A boot-time self test (31 checks) reports to the host through QEMU's debug-exit port, and a serial harness drives the shell.
- `nyx.exe` is a WPF launcher with the kernel embedded; it installs QEMU through winget. `guide` starts a guided tour.
- Commits:
  - 14:12 `c32f23dc` Initial commit: a small x86 operating system
  - 14:56 `e0a221cc` Add a Windows launcher and a guided first run

**v0.2.0**, 08-28 15:28, tag `b8806da2`
- ATA PIO disk: the whole in-memory filesystem is written out as one image.
- RTL8139 driver and Ethernet, ARP, IPv4, ICMP, UDP, DHCP and DNS.
- A single-connection TCP client and HTTP `fetch`. 44 checks.
- Commits:
  - 15:27 `b8806da2` Add a disk and a network stack

**v0.3.0**, 08-28 18:07, tag `d985dba1`
- VBE framebuffer through the Bochs ports; a bitmap font generated from a TrueType face; a PS/2 mouse.
- **FAT16 replaces the private disk format.**
- Ring 3 with 7 syscalls on `int 0x80`, and an ELF32 loader. 71 checks.
- Commits:
  - 17:26 `481d9de7` Add a framebuffer, a bitmap font and a mouse
  - 17:39 `5c12567a` Replace the private disk format with FAT16
  - 18:06 `d985dba1` Add userspace: ring 3, system calls and an ELF loader

**v0.4.0**, 08-28 18:28, tag `1eb6c98d`
- Compositing window manager, paint (still kernel code) and an about window. The heap grows from 4 to 16 MiB. 86 checks.
- Commits:
  - 18:27 `1eb6c98d` Add a desktop: window manager, paint program and colours

**v0.5.0**, 08-28 22:43, tag `5fff6bee`
- AHCI and e1000 drivers, tried before ATA and RTL8139.
- Ordered, crash-safe FAT writes.
- TCP retransmission.
- A window server maps surfaces into ring 3, so paint becomes an ELF.
- User programs are pasted into the kernel image.
- The version string finally leaves 0.1.0. 106 checks.
- Commits:
  - 22:37 `b937f79c` Add AHCI and e1000, crash-safe writes, TCP retransmission, paint in ring 3
  - 22:41 `5fff6bee` Bump to 0.5.0

**v0.6.0**, 08-30 07:05, tag `774d9d8b`
- **Its own BIOS bootloader** (`cdboot.S`: A20, E820, INT 13h, a copy above 1 MiB through unreal mode, a multiboot-style handoff). **A hybrid ISO/USB image** written by `tools/mkiso.py` (ISO 9660, El Torito and an MBR).
- FAT16 directories and a VFS. 18 new syscalls, 31 in total.
- A ring 3 terminal. Settings is a ring 3 program that writes `/nyx.cfg`, which the window manager re-reads.
- The other processors are started through the ACPI MADT and INIT/SIPI, and run functions handed to them.
- Review-found fixes. 174 checks, 183 on 4 CPUs.
- The notes explicitly decline 64-bit as work with nothing visible to show for it. It happened four hours later.
- Commits:
  - 08-29 03:35 `dfd3a27b` Add directories, a VFS, and file and socket system calls
  - 03:51 `2117e44c` Add a terminal, a settings program, and a themeable desktop
  - 04:04 `e7511b5d` Boot on real hardware: a bootloader and a bootable image
  - 15:01 `7898701c` Fix a heap corruption, a syscall pointer check, and the FAT allocator
  - 16:44 `f32a9f92` Fix resource cleanup and workflow checks (empty body; there is no `.github/` in the tree today)
  - 08-30 06:23 `302b42bb` Start the other processors
  - 06:35 `ebefcc48` Merge the bug fixes, and stop the SSE checker tripping on call displacements. This merges two lines of work that had both started from 0.5.0.
  - 07:03 `774d9d8b` Bump to 0.6.0, and stop the build shipping a stale user program

**v0.6.1**, 08-30 07:35, tag `7ab04230`
- Stage one now survives firmware that loads only the first 512 bytes. This was VMware's "invalid part of memory" error.
- The README admits the traced font and the bundled .NET runtime.
- Commits:
  - 07:33 `7ab04230` Make the bootloader survive firmware that loads only one sector

**v0.6.2**, 08-30 07:53, tag `310adec9`
- 95 hand-drawn 8x16 glyphs replace the PIL-rendered Consolas. From here on everything is declared written from scratch except the .NET launcher.
- Commits:
  - 07:51 `310adec9` Draw the font by hand

#### Era B. 64-bit and UEFI

**v0.7.0**, 08-30 11:38, tag `2ea9dda3`
- **A UEFI loader written from the specification.** `uefi/efi.h` is transcribed from the spec rather than taken from gnu-efi. The loader uses GOP, finds ACPI in the configuration table and runs the ExitBootServices loop.
- **`include/handoff.h`** replaces multiboot as the one entry contract.
- **The long-mode port:**
  - four-level paging;
  - user space moved to 512 GiB;
  - a 64-bit ELF loader and trampoline;
  - the multiboot a.out kludge, so QEMU `-kernel` still works.
- **One image, four boot paths.** BIOS or UEFI, from a disc or a stick. `tools/mkfat.py` writes the ESP. 174 checks, 183 on 4 CPUs.
- Commits:
  - 08:22 `bab4aac6` Add a UEFI loader, written from the specification
  - 10:54 `2506b9a7` Port the kernel to 64-bit long mode
  - 10:58 `99ef2d8e` Boot the real kernel through UEFI
  - 11:22 `f9c37761` One image that boots BIOS or UEFI, from a disc or a stick
  - 11:34 `2ea9dda3` Bump to 0.7.0
- Note: `34db02d3` (the launcher switched to `qemu-system-x86_64`) landed at 11:36. That is after the tagged commit but 2 minutes before the release was published, so it belongs to v0.8.0 by ancestry.

**v0.8.0**, 08-31 09:01, tag `7ae3a310`
- Wait queues; spawn, wait and kill with exit status.
- A shaped filesystem (`/home`, `/doc`, `/cfg`, `/tmp`) and generated `/sys` and `/bin`.
- The terminal is rewritten: line editor, history, completion, 36 commands.
- Windows can be minimised, maximised, snapped and resized. Three more wallpapers.
- Four boot fixes: A20, stage-one verification, the read packet, and surface lifetime.
- **The published `nyx.exe` crashes before opening** (`DllNotFoundException`). The fix `5e7d0904` landed on 09-03 but only shipped with v0.9.0, eleven days later.
- Commits:
  - 08-30 11:36 `34db02d3` Point the Windows launcher at the 64-bit machine
  - 11:57 `0906d47c` Add wait queues, spawn, and exit status
  - 12:22 `92c142e9` Give the filesystem a shape and rewrite the terminal
  - 08-31 06:03 `81440051` Give windows a full set of controls, and fix two ways resizing broke
  - 06:17 `d951bfd4` Add three wallpapers, one that moves, and a switch for the playful bits
  - 06:21 `bf6fb15c` Bring the documentation in line with what is there
  - 07:08 `9443bbef` Move stage one's read packet out of the sector it overwrites
  - 07:19 `d0c2a37c` Stop stage one from trusting what it read back
  - 07:34 `ff3a019d` Open the A20 gate properly, and refuse to boot without it
  - 09:00 `7ae3a310` Call this 0.8.0

#### Era C. The pipeline experiment (no release)

- 08-31 09:31 `fa574d5c` Add a pipeline that runs both models against a gate neither can pass
- 09:41 `43105911` Check both agents answer before starting a run
- 09-02 06:16 `406ad564` Let the agents run things, and run several tasks at once
- 06:17 `22c2a8cd` pipeline: claim rtc-clock pipes lfn-read (written by `batch.sh`)
- 07:36 `97048348` pipeline: after splitting the batch (written by `batch.sh`)
- 09-03 06:01 `62c1fb0b` Make the pipeline cheap by default
- 07:41 `5e7d0904` Fix the launcher crashing before it opens

#### Era D1. Laptop hardware and the rename (2026-09-14)

**v0.9.0**, 09-14 12:21, tag `9ee1d535`
- **The black box:**
  - the kernel narrates its boot into a static buffer;
  - the buffer is mirrored to serial, painted on a panic, and kept in reserved sectors;
  - `/sys/boot` is this boot and `/sys/lastboot` the previous one.
- PCIe ECAM through MCFG, and the XSDT. **The RSDP is read from the UEFI handoff**, which is what found all the CPUs under UEFI.
- The block layer splits requests.
- GPT and MBR are read only, with a cautious choice of volume.
- IOAPIC with the firmware's interrupt overrides. FAT32. NVMe.
- The gate runs steps in parallel. 253 checks, 261 on q35 and NVMe.
- Commits:
  - 07:26 `9ef316a2` Record what the machine was doing when it stopped
  - 08:10 `e7bc2094` Reach PCIe configuration space, and read the tables UEFI actually provides
  - 09:16 `162b0550` Read the partition table, and be careful about which volume is ours
  - 11:56 `d0b4c2f1` Route interrupts properly, read FAT32, and drive an NVMe disk
  - 11:56 `ed5ad1d5` Run the gate's independent steps at once
  - 12:18 `9ee1d535` Call this 0.9.0

**v0.10.0**, 09-14 14:22, tag `eb9e64d1`
- A clipboard in the kernel (`/sys/clipboard`) and an immediate-mode widget set (`userland/ui.h`).
- A file manager, a text editor, and copy and paste in the terminal.
- A CMOS RTC, so the panel shows a clock. Settings becomes an application.
- **An outline typeface** (`tools/genface.py`, rasterised at build time). The terminal keeps the bitmap font. 285 checks, 293 on q35.
- Commits:
  - 12:57 `c4aa6083` Give the desktop a clipboard, a file manager and a text editor
  - 13:13 `09bc4710` Read the hardware clock, and make Settings a settings application
  - 14:18 `c6a04d8f` Draw the interface in a real typeface
  - 14:21 `eb9e64d1` Call this 0.10.0

**v0.11.0**, 09-14 15:53, tag `a2c408fe`
- **The rename to zelr**: 283 replacements across 71 files. The fixed-width magics become ZELRKRN, ZELRBOT1, ZELRSMP1 and ZLR1. Old disks are carried across.
- A `nyx.bat` shortcut with five modes.
- The launcher asks for whpx acceleration.
- The gate builds once and shows failing output.
- **Defects in this release:**
  - its assets still reported 0.10.0 from inside (fixed by `e85dfd1e`);
  - it triple-faulted on VMware (fixed in v0.12.0).
- Commits:
  - 14:39 `3a43d2a2` Add a shortcut for booting it in QEMU
  - 15:22 `9f20417e` Build once for the gate, not once per harness
  - 15:22 `2bc4dd09` Run it with hardware acceleration
  - 15:45 `1b634221` Rename the system to zelr
  - 15:51 `a2c408fe` Check that the handoff magic agrees with itself

#### Era D2. Laptop and desktop sprint

**v0.12.0**, 09-15 08:27, tag `9255eeb0`
- **The first ever boot on VMware.** The unreal-mode segment limit is re-established after every BIOS call, and the loader checks the kernel actually arrived.
- A VMware SVGA II display driver.
- The version is fixed, and `tools/check_version.py` guards it.
- This is a new release on purpose rather than new files uploaded under the old tag.
- Commits:
  - 04:27 `e85dfd1e` Set the version to 0.11.0
  - 04:37 `e11eb296` Check the reported version against the newest tag
  - 05:22 `0b8a897a` Drive VMware's display adapter, so the desktop has somewhere to draw
  - 05:22 `0a278c83` Keep unreal mode across BIOS calls, and check the kernel arrived
  - 08:23 `9255eeb0` Call this 0.12.0

**v0.13.0**, 09-15 11:29, tag `a35bc108`
- **xHCI and USB HID boot protocol**, so a laptop keyboard works.
- Idle APs halt and are woken by IPI; spinning APs had starved the BSP.
- The harnesses wait on conditions (`tools/harness.py`). The gate uses exit status and a lock.
- Commits:
  - 11:26 `d3983b6a` Let an idle processor sleep instead of spinning
  - 11:27 `0d83457c` Wait for the thing being waited for, not for a guess at how long it takes
  - 11:27 `908ba871` Drive the usb controller, so a laptop keyboard works
  - 11:27 `a35bc108` Call this 0.13.0

**v0.14.0**, 09-16 05:59, tag `4dc69945`, **no assets**
- USB hubs down to five tiers, and hot-plug handled from a task.
- All memory the firmware reports is mapped (2 MiB pages above 64 MiB), with the MMIO holes left unmapped.
- Commits:
  - 09-15 11:32 `d64f1537` The readme said window resizing did not exist, long after it did
  - 11:53 `a049f85c` Map the memory the machine has, instead of the first 64 MiB of it
  - 09-16 05:59 `4dc69945` Walk hubs, and notice something being plugged in

**v0.15.0**, 09-16 07:12, tag `32fd308c`, **no assets**
- Desktop design: the title bar becomes a surface, the palette is derived from three colours, bold weights are added, and the taskbar floats.
- The heap is sized from the video mode. The loader picks the smallest mode when none fit.
- Reboot works on machines with no 8042.
- Commits:
  - 06:51 `4cc1eb31` Map the screen the laptop has, and reboot a machine with no 8042
  - 07:12 `32fd308c` Give the desktop a design, instead of a set of colours

**v0.16.0**, 09-16 08:55, tag `ee9f599a`
- USB mass storage (bulk-only SCSI).
- Menu animation, with flushing limited to a damage rectangle.
- Commits:
  - 08:31 `3c22354a` Animate the menus, and redraw only what moved
  - 08:55 `ee9f599a` Read and write a USB stick

**v0.16.1**, 09-16 11:54, tag `3fa89199`
- **The 8042 input-death fix**: one path for every byte, plus a poll from the timer tick.
- README corrections.
- **The last MIT release.**
- `ab6d8d90` (empty body) is most likely where `pipeline/release.sh` arrived; its header comment tells the same story.
- Commits:
  - 08:56 `c1f5dc49` Readme said a usb stick was enumerated and ignored, which it no longer is
  - 09:06 `ab6d8d90` The releases never had the two files the readme says to download
  - 09:06 `90f06838` The kernel is 1.5 MB now, not 700 KB
  - 11:54 `3fa89199` One place owns the 8042, and a byte nobody took no longer kills input

**v0.17.0**, 09-16 12:39, tag `9c5edad5`
- A write-combining framebuffer and 8-byte memcpy/memset/memmove: a full-screen flush went from about 1.5 s to about 1 ms.
- Five README screenshots produced by `tools/shots.py`. A "screen" mode for the gate.
- **MIT becomes GPLv3.** The subject's "forks stay open" refers to exactly this.
- Commits:
  - 12:33 `fca59620` Stop writing the screen one uncached byte at a time
  - 12:38 `9c5edad5` Show what it looks like now, and make forks stay open

**v0.18.0**, 09-16 13:38, tag `16d6acd8`
- Intel HD Audio; `beep`; sound syscalls. The pitch of a recording is measured.
- Commits:
  - 13:38 `16d6acd8` Make a sound, and measure that it was the right one

**v0.19.0**, 09-16 16:14, tag `379bed46`
- Numbered disks; `/usb` mounting; VFAT long names; ACPI S5 shutdown by decoding AML.
- Commits:
  - 16:13 `379bed46` Mount a usb stick, keep long filenames, and turn the machine off

**v0.20.0**, 09-16 17:35, tag `d802003f`
- The desktop starts by itself.
- An auto-hiding floating panel with **pinned apps**.
- Five animated wallpapers and a runtime resolution change.
- Commits:
  - 17:34 `d802003f` A panel that gets out of the way, and a desktop that starts itself

**v0.21.0**, 09-17 07:16, tag `ae69771e`
- A system monitor, a music player and a calculator.
- Open-with: a task can now be started with one string argument.
- Volume control, the mouse wheel, and light mode everywhere.
- A fix for the readdir struct overflow. The gate now compares struct definitions across kernel and user headers.
- Commits:
  - 05:38 `e6325949` A directory that ring 3 saw as empty, a wheel, and light mode everywhere
  - 07:15 `ae69771e` Three more programs, a volume, and opening a file in the thing that reads it

**v0.22.0**, 09-17 08:22, tag `0a4f4120`
- A Synaptics trackpad (two-finger scroll, tap). The boot log reports i2c controllers.
- Commits:
  - 08:21 `0a4f4120` A trackpad, rather than a mouse a trackpad is pretending to be

**v0.23.0**, 09-17 11:17, tag `fcd0321a`
- A network icon and panel with three states.
- WPA2 key arithmetic checked against published vectors.
- Wireless controllers are classified (vendor firmware needed, or Atheros). 381 checks.
- Commits:
  - 10:37 `a5586229` The arithmetic a wireless password is made of
  - 11:17 `fcd0321a` A network on the panel, and an icon that says which of three things is wrong

**v0.24.0**, 09-17 11:47, tag `8aa13e43`
- RNDIS Ethernet over USB, for phone tethering.
- Commits:
  - 11:47 `8aa13e43` Ethernet over usb, which is how a laptop with no usable wireless gets on

**v0.25.0**, 09-17 16:15, tag `5168ba0e`
- A bevelled desktop "built rather than tinted": square windows, a flush panel, a menu bar and drawn desktop icons.
- A fix for lost clicks.
- Commits:
  - 16:15 `5168ba0e` A desktop that is built rather than tinted

#### Era D3. The web

**v0.26.0**, 09-17 20:41, tag `724c2673`
- **A web browser**: HTML without CSS, http only, running on the system's own TCP stack.
- TCP sequence-number, receive-window and FIN fixes.
- Commits:
  - 16:56 `1f9d69c3` Fix two bugs that stopped a page of any size arriving
  - 20:40 `724c2673` A web browser

**v0.27.0**, 09-18 05:20, tag `a8b3ed57`
- An AMD PCnet driver: the card VMware gives an unrecognised guest. 392 checks.
- Commits:
  - 05:20 `a8b3ed57` Drive the AMD PCnet, the card VMware hands an unknown guest

**v0.28.0**, 09-18 10:58, tag `0e6e9a27`
- **https with its own TLS 1.3**:
  - SHA-256, HMAC and HKDF;
  - AES-GCM and X25519;
  - RSA, and ECDSA on P-256 and P-384;
  - X.509 with a 48-root store.
- The key schedule was fixed by checking it against RFC 8448. 515 checks.
- **This is the first commit with a Claude trailer.**
- Commits:
  - 05:40 `2e07dcf0` The arithmetic https is made of
  - 05:47 `4a6915db` ECDSA over P-256
  - 05:53 `57379215` P-384 and SHA-384, because one curve is not enough
  - 06:05 `fd12462a` Certificates, chains of them, and who to believe
  - 10:57 `0e6e9a27` https, and the one message the key schedule was out by

**v0.29.0**, 09-19 06:06, tag `6161281a`
- Kernel stacks go to 32 KiB, with a guard.
- SHA-384 signatures over RSA; the store's copy of a root is preferred to the one a server sends.
- DHCP runs at boot. `connect` distinguishes five failure reasons. A fix for polling the PCnet. DNS retries.
- Commits:
  - 06:02 `67cfa304` The stack the certificate arithmetic did not quite fit on
  - 06:02 `28490abc` A chain is not all one hash, and a root is its own affair
  - 06:02 `6161281a` An address without being asked, and a reason when there is none

#### Era D4. Unix, the real web, hardening

**v0.30.0**, 09-20 10:42, tag `b621ddce`
- SSE for ring 3; the kernel stays integer only.
- `sbrk` and a malloc, a JavaScript interpreter, fork and exec, descriptor tables, `dup2`, pipes, three signals, and `/bin/sh`.
- A new desktop "material".
- Commits:
  - 10:42 `b621ddce` A shell, and the six things that had to exist underneath it

**v0.31.0**, 09-20 16:49, tag `bc662355`
- PNG, JPEG and SVG decoders; flexbox.
- **A dock replaces the taskbar, and the pinned apps are removed.**
- The 31 desktop settings move into one table.
- Commits:
  - 16:48 `bc662355` Pictures, a dock, and thirty one numbers that used to be constants

**v0.32.0**, 09-20 19:37, tag `6016cb4f`
- A triple fault fixed by taking the network stack out of interrupt context.
- FAT32 formatting.
- The heap takes a quarter of RAM.
- Frame diffing split across CPUs.
- An Ensoniq audio driver.
- Commits:
  - 19:32 `6016cb4f` A triple fault, a second processor, and the card VMware actually gives you

**v0.33.0**, 09-21 07:25, tag `6ae86e42`
- A boot-sector stub, and in-place repair for disks installed by 0.32.0.
- The browser becomes interactive: events, the DOM, timers, forms, regex, `querySelector`, gzip, keep-alive, cookies and arrow functions.
- A find bar (Ctrl+F).
- Commits:
  - 09-20 20:20 `5e62b39d` A page that can be clicked on, and a script left running to hear it
  - 20:51 `3689baf7` A search box, which is the commonest thing on the web and did not work
  - 21:08 `d29a65dd` Selectors, and the slash that is not a divide
  - 21:25 `4b858949` What goes over the wire, and how often it has to go
  - 21:43 `168fbaf4` Arrows, instanceof and labels, found by asking google what it needed
  - 22:12 `0fe2d1bb` A boot sector that ends in 55 AA has to contain something that runs
  - 23:52 `b8c7a777` A find that looks at the screen, instead of a second start menu
  - 09-21 07:24 `6ae86e42` A disk that is already wrong is not mended by fixing the formatter

**v0.34.0**, 09-21 13:36, tag `60ba6166`
- An idle task, which is what finally made sleep work.
- Six TCP connections.
- Double-buffered window surfaces.
- A program search path.
- COW fork.
- A TSS per CPU, and SMP scheduling under one kernel lock.
- Blackjack and poker.
- **UEFI is known to be broken in this release, and the loader now says why.**
- Commits:
  - 08:32 `c004480b` Two card games, and the rules written down so they can be wrong out loud
  - 08:36 `71499483` One winner takes the s, and a cloth rather than bands of green
  - 09:47 `9421daf5` Somewhere to go when there is nothing to do
  - 10:03 `48e2e7c4` More than one connection, which is a different machine
  - 10:14 `ad87d42b` A frame that was finished, rather than one that was being drawn
  - 10:19 `2c59a539` The looking, not the loading
  - 10:29 `842fe452` Fork stops copying what the child is about to throw away
  - 10:46 `5a6fdf94` A task state segment each, before anything is scheduled on one
  - 12:12 `7812519e` Programs on more than one processor
  - 12:14 `11848dd3` The readme said six things that had stopped being true
  - 12:36 `9872d2cf` A third tool that did not know about the Games row
  - 12:56 `6f2b58cf` Three checks that were reading before the machine had finished speaking
  - 13:31 `ca1b8aa0` The uefi loader stops insisting, and says what is in the way
  - 13:36 `60ba6166` zelr 0.34.0

**v0.35.0**, 09-21 15:55, tag `5164ffaa`
- **The kernel is linked at 16 MiB, so UEFI boots again.**
- System V argv.
- An SDK.
- The gate's "second go" rule.
- Commits:
  - 14:03 `775b8ed5` The kernel is linked at sixteen megabytes, and boots under UEFI again
  - 14:28 `d3a21b9b` A program is started on a vector of words, the way System V says
  - 14:32 `c91e550b` An SDK, so a program can be written somewhere that is not this repository
  - 15:51 `bba15123` Four checks that were reporting a busy host as a broken machine
  - 15:54 `529c140e` The self test table in the readme had not added up for a while
  - 15:55 `5164ffaa` zelr 0.35.0

**v0.36.0**, 09-21 18:56, tag `dbe08ab7` (inferred)
- Signal handlers.
- The JS event loop, `script src` and XHR.
- A fault in ring 3 now ends the program rather than the machine.
- `mmap`.
- A power-cut test.
- `fsync`, `rename` and `poll`.
- Commits:
  - 16:31 `a49f2614` A program can be told about a signal and carry on
  - 16:55 `73dd558a` A page can do its work after it has been read
  - 18:19 `12cc46b5` Four browser checks that took their picture before the page was drawn
  - 18:19 `652a8047` A program that faults ends, and the machine does not
  - 18:19 `d0200f96` Memory that costs what it is used rather than what it is asked for
  - 18:33 `eb7ad550` Saying now about a file, and a comment about crash safety that is now a fact
  - 18:38 `25642755` Waiting on several descriptors at once
  - 18:55 `dbe08ab7` zelr 0.36.0

**v0.37.0**, 09-22 14:02, tag `4a6948e4` (inferred)
- Searching from the address bar through DuckDuckGo lite.
- A 121-root store taken from Mozilla's set.
- A growable user stack.
- Five CSS fixes, plus `<center>`, `position` and `box-sizing`.
- `call`, `apply` and `bind`.
- A NOTICE, and the author's name compiled into the kernel.
- Commits:
  - 09-21 19:25 `eedd2f85` Say who wrote this, in the places a copy cannot quietly drop
  - 20:04 `193a0dfa` The tag that is older than the standard which removed it, and three that are not
  - 20:23 `3f4cdb5b` call, apply and bind, and an error that says which name
  - 20:38 `c698bdb0` A field the page styled is drawn the way the page styled it
  - 21:10 `76965afe` Searching works, and so does following what it finds
  - 21:25 `9a8d8dc0` A stack that grows when the program needs more of it
  - 23:37 `ff791fec` Pages the size of real pages, read the way they were written
  - 23:37 `1a2bb487` Text longer than the box it is typed in
  - 09-22 14:01 `f1802ad6` The readme says that searching works
  - 14:01 `4a6948e4` zelr 0.37.0

**After v0.37.0 (on `main`, not released):**
- 14:10 `011292c7` A TLS session per connection, rather than one for the machine
- 14:23 `6048716a` A libc, written from scratch, and a program that has never heard of zelr

See section 7 for what these two change.

### 1.4 Growth metrics

**Boot self-test check count.** The count is for one CPU unless noted.

| Point | Checks |
|---|---|
| v0.1.0 | 31 |
| v0.2.0 | 44 |
| v0.3.0 | 71 |
| v0.4.0 | 86 |
| v0.5.0 | 106 |
| `dfd3a27b` | 148 |
| v0.6.0, v0.7.0 | 174 (183 on 4 CPUs) |
| pipeline start | 213 |
| `9ef316a2` | 234 |
| v0.9.0 | 253 (261 on q35) |
| v0.10.0 | 285 (293) |
| v0.17.0 | 291 |
| v0.23.0 | 381 |
| v0.27.0 | 392 |
| crypto commits | 434, then 457, then 491 |
| v0.28.0 | 515 (523 on q35) |
| v0.29.0 | 522 (530, 531) |
| v0.32.0 | 543 (559 on 2 CPUs) |
| v0.34.0 | 548 (559 on 4, 556 on q35) |
| v0.35.0 and v0.36.0 | 552 (563 on 2 CPUs, 560 on q35) |
| `76965afe` | 553 |
| `011292c7` | **560** |

**Release asset sizes.** These are the zelr.org figures, which appear to be MiB labelled "MB".

| Release | ISO |
|---|---|
| v0.6.x | 0.5 MB |
| v0.7.0 | 4.6 (the ESP arrives) |
| v0.10.0 to v0.13.0 | 5.4 |
| v0.21.0 | 6.0 |
| v0.28.0 | 6.5 |
| v0.30.0 | 12 (ESP sized from its contents; outline font at many sizes) |
| v0.31.0 | 15 |
| v0.33.0 | 18 |
| v0.35.0 | 20 |
| v0.37.0 | 21,561,344 bytes |

| Release | EXE |
|---|---|
| v0.1.0 to v0.4.0 | 68 MB |
| v0.5.0 to v0.12.0 | 155 |
| v0.35.0 | 163 |
| v0.37.0 | 171,597,277 bytes (about 164 MiB) |

- The README still says the kernel is "about 1.5 MB" and the exe 162 MB.
- The commits of 09-21 describe the kernel image as a little over 9 MB. That is why it had to move to 16 MiB.

**Gate wall-clock time:**
- 55 min serial became 17 min parallel (`ed5ad1d5`).
- It crept back to 35 min, then fell to 17 once the harnesses waited on conditions (`0d83457c`).
- The screen mode takes 4.5 min (`fca59620`).
- The slowest step went from 452 s to 55 s (`b621ddce`).
- The full gate took 16 m 37 s at v0.35.0 (`bba15123`).

---

## 2. Architecture decisions and their recorded reasons

The reasons are paraphrased from the commit messages and release notes unless marked otherwise.

### 2.1 Provenance and verification

| Decision | Where | Recorded reason |
|---|---|---|
| No third-party code: no libc, clang from zig as the only toolchain | `c32f23dc` onward | The founding rule. Later commits enforce it against themselves: `310adec9` redraws the font because the traced one made the README's claim untrue, and `7ab04230` confesses the exceptions. The one standing exception is the .NET/WPF runtime in the Windows launcher (NOTICE). |
| Implement formats from the specification, and write the tools too | `e7511b5d` (ISO), `f9c37761` (FAT writer for the ESP), `bab4aac6` (`efi.h` from the UEFI spec, not gnu-efi) | No mtools, xorriso, isohybrid, GRUB or syslinux. The image and the boot path are the project's own. |
| Check every implementation against something outside the project | `5c12567a`, `a5586229`, `2e07dcf0`, `0e6e9a27`, `1f9d69c3` | `tools/readfat.py` shares no code with the kernel, so a disagreement means the kernel is wrong. Crypto is checked against FIPS and RFC vectors, TLS against RFC 8448 and real sites, HTTP against Python's own server. Sound is checked by recording pitch, and forms by asking the server what arrived. |
| A check has to be seen failing before it counts | throughout (e.g. `9ef316a2`, `d0b4c2f1`, `0d83457c`, `842fe452`) | Many checks were found passing vacuously. Each new check is run against a deliberately broken build. Protections QEMU cannot exercise are marked in the source as unverified rather than verified. |
| Say what failed and why, by name | `9ef316a2`, `6161281a`, `a8b3ed57`, `ca1b8aa0` | Real-hardware failures are silent. So: the black box; five `connect` failure reasons; naming controllers that have no driver; a UEFI loader that prints what is in the way. |

### 2.2 Boot and platform

| Decision | Where | Recorded reason |
|---|---|---|
| Write its own BIOS bootloader | `e7511b5d` (08-29) | Until then QEMU `-kernel` was doing the loader's job, and nothing does that on a real machine. Unreal mode is the only way to write above 1 MiB while keeping the BIOS calls. |
| Write its own UEFI loader | `bab4aac6` | A UEFI machine never enters real mode or runs a boot sector. |
| Its own handoff structure rather than multiboot | `bab4aac6`, `f9c37761` | Multiboot cannot describe a framebuffer the firmware chose, has no field for the ACPI pointer, and is 32-bit. Both loaders now build the same structure, so the kernel never knows which one started it. |
| Go 64-bit (long mode) | `2506b9a7` (08-30 10:54) | **No commit gives a standalone reason.** Four hours earlier the v0.6.0 notes had dismissed the port as a large rewrite with nothing visible to show for it. The UEFI loader written 75 minutes later deliberately produced a 64-bit handoff, with a stub standing in for the kernel until the port was done. So the record frames 64-bit as part of booting through UEFI on current hardware, in service of the real-laptop goal. `2ea9dda3` then lists USB and NVMe as what still stood between it and a modern laptop. |
| One ISO that boots four ways | `f9c37761` | BIOS or UEFI, from a disc or a stick: El Torito carries two catalog entries, and an MBR partition marks the ESP. |
| User space at 512 GiB | `2506b9a7` | One PML4 entry covers 512 GiB and the kernel's entry is shared into every address space, so a program at 1 GiB would have put its page tables inside the kernel's. |
| Kernel moved from 1 MiB to 16 MiB | `775b8ed5` (v0.35.0) | 1 MiB is the traditional 32-bit spot. Once the image passed about 7 MB, a kernel of about 9 MB at 1 MiB overlapped the ACPI NVS region that the tested UEFI firmware keeps at 8 MiB, and both UEFI paths died for two releases. 16 MiB clears it and stays inside the 64 MiB mapped a page at a time. The cost is a memory floor (it passes on 40 MiB). `linker.ld` is now the single source, read by `tools/loadaddr.py` and enforced by `check_loader.py`. |
| The UEFI loader loads anywhere, then moves the kernel | `ca1b8aa0` | Asking the firmware for a fixed address can fail. Boot-services memory becomes the kernel's after ExitBootServices, so it reads the image wherever the firmware allows and moves it afterwards, after first proving the destination is free. |
| IOAPIC instead of the 8259 pair | `d0b4c2f1` | The 8259 is emulated rather than present on modern chipsets. The firmware's override list must be honoured (QEMU routes IRQ0 to input 2). |
| Read GPT and MBR but never write them; never touch the ESP; never format an existing partition | `162b0550` | On a laptop, the wrong guess breaks somebody's machine. GPT is read first because GPT disks carry a decoy MBR. |
| Black box log | `9ef316a2` | On the first boot on a laptop there is no serial line. The log is mirrored to serial, painted on a panic, and kept on disk. |

### 2.3 Memory, processes, scheduling

| Decision | Where | Recorded reason |
|---|---|---|
| Map what the firmware reports, in 2 MiB pages above 64 MiB, and leave the holes | `a049f85c` | Mapping the MMIO gap as ordinary memory would give drivers cached mappings of their own registers. Pages are 2 MiB because 4 KiB pages would cost 32 MiB of tables for 16 GiB. |
| Heap sizing | `1eb6c98d`, `4cc1eb31`, `6016cb4f` | The back buffer comes out of the heap. First 16 MiB, then sized from the video mode, finally a quarter of RAM up to 512 MiB, starting after the kernel image. |
| Kernel stacks of 32 KiB, painted, with a guard word | `67cfa304` | Certificate verification needed 16,216 to 16,640 bytes of a 16,384-byte stack. Stacks come from the heap, so an overflow silently corrupted a neighbour's block header. The size is recorded beside the measurement that justifies it. |
| Programs pasted into the kernel image, never saved to disk | `b937f79c` | A fresh install has something to run, and rebuilding the kernel really does update the programs. |
| `/sys` and `/bin` are generated when read | `92c142e9` | They cannot go stale. |
| fork: eager copy at first, COW later | `842fe452` | The first design avoided a fault handler and reference counts. It was abandoned because a two-command shell pipeline throws those copies away: a 4 MiB fork cost 4208 KiB and now costs 40. A frame gets a one-byte reference count. |
| exec keeps descriptors; the descriptor table sits in front of shared open-file descriptions | `b621ddce` | This is what makes redirection work: one change to `sys_write` made every program redirectable. |
| argv on the stack, System V layout; syscall numbers frozen | `d3a21b9b` | Startup code written elsewhere expects that layout. The numbers are now an ABI, so a retired call leaves a gap (44 is retired). |
| No SMP scheduling at first: APs run handed functions | `302b42bb` | Sharing the scheduler would need locks on the heap, the task list, the filesystem and every driver, and a mistake would be a once-an-hour fault. |
| Later, SMP scheduling behind **one kernel lock** | `7812519e` (v0.34.0) | Called "the coarsest lock there is and it is the honest one to start with". The alternative was about forty separate lock changes, any one of which could corrupt the machine now and then. It works because the kernel is non-preemptive: syscalls enter through an interrupt gate. Kernel tasks stay on the BSP. Timer IRQs and wake IPIs never block on the lock. |
| The kernel stays integer-only; SSE only for ring 3 | `b621ddce` (v0.30.0) | An interrupt can land between any two instructions, so a handler touching vector registers would have to save them all. Before v0.30.0 the whole system was built without SSE (checked by `tools/check_sse.py`). Hence the fixed-point sine, easing and calculator, and the font rasterised at build time. |
| Idle task per CPU; round robin walks exactly one lap | `9421daf5` | Without an idle task a sleep returned at once, which produced four different symptoms. |
| `poll` looks again each tick instead of waiting on several queues | `25642755` | The wait machinery cannot sit on several queues at once. The trade is up to 10 ms of latency against rewriting waiting, and the comment says so. |
| Anonymous `mmap` only, 16 fixed ranges per task | `d0200f96` | File mapping would mean holding files open behind a program's back or doing path lookups inside the fault handler. The fault handler cannot allocate. |
| Signals delivered only on return to ring 3; the return trampoline lives in `sdk/zelr.h` | `a49f2614` | The kernel has no code mapped in ring 3, and the alternative is an executable stack. Frames read back from the user stack are checked before use. |
| A ring 3 fault ends the program with status 139; a kernel fault still panics | `652a8047` | A faulting program is wrong. A faulting kernel has a wrong idea of its own memory, and carrying on would write that to disk. |

### 2.4 Drivers, storage, network

| Decision | Where | Recorded reason |
|---|---|---|
| FAT16 replaces the private disk format | `5c12567a` (08-28) | The private format made the disk a sealed box. FAT is fiddly but universally readable. |
| Ordered crash-safe writes | `b937f79c`; proven in `eb7ad550` | New clusters are written and flushed, one sector write swings the directory entry, then the old chain is freed. `crashcheck` kills the VM at random moments. `rename` only edits the entry in place, because FAT cannot record a rename that is half done. |
| xHCI polled on the timer tick | `908ba871` | Costs up to 10 ms per key press and avoids needing MSI. |
| The network stack runs in a task, not in the IRQ handler | `6016cb4f` | Running the stack in the handler recursed through ARP and polling until the stack overflowed and the machine triple-faulted. An interrupt should only queue work. |
| DHCP at startup | `6161281a` | Nobody reads "no address" as "press the button first". |
| TLS 1.3 only, one suite (AES-128-GCM-SHA256 with X25519), no resumption | v0.28.0 notes, README | Practical attacks on TLS work by steering two modern implementations into an old mode they both still support; not having the old mode is the defence. |
| TLS connect does the handshake inside the syscall | `0e6e9a27` | There is no way to forget to turn encryption on, and a failed handshake closes the socket. Typed addresses default to https. |
| The trust store is generated from a published store | `fd12462a`, `76965afe` | It is correct only if it agrees with everybody else's. First 48 roots derived from Windows, then 121 from Mozilla's set via `tools/mkroots.py`. Names are compared as raw encoding; wildcards cover only the leftmost label; only SAN is used. |
| Constant-time where secrets are involved | `2e07dcf0`, `a5586229` | GCM multiplication bit by bit rather than with tables; the X25519 ladder; a MIC compare with no early exit. RSA verification is not constant-time, because everything in it is public. |
| No vendor firmware, so no wireless for most cards; RNDIS tethering instead | `fcd0321a`, `8aa13e43` | Most wireless MACs run vendor firmware, which a from-scratch kernel will not carry. Atheros parts are the exception. |
| Six TCP connections, then TLS per connection | `48e2e7c4`, `011292c7` | File-level state had been a real limit, not a simplification. |

### 2.5 Desktop, browser, SDK, tooling

| Decision | Where | Recorded reason |
|---|---|---|
| Settings talks to the WM only through a file (`/nyx.cfg`, later `/zelr.cfg`) | `2117e44c` | Ring 3 has no syscall into the window manager. The WM re-reads the file four times a second. Later there is one settings table in `kernel/theme.c` (`bc662355`) because four hand-kept lists had drifted. |
| Immediate-mode widget set | `c4aa6083` | Fits a system where a window is pixels plus an event queue, in about a tenth of the code. |
| A clipboard in the kernel, text only | `c4aa6083` | Two ring 3 programs have no other way to hand each other bytes. |
| Icons are drawn, never stored | `5168ba0e`, `d802003f`, `c004480b` | A bitmap per icon needs a file format, a loader and a directory before anything appears. |
| Server-side double buffering, copied rather than flipped | `ad87d42b` | The compositor was catching half-drawn frames. A flip would break paint, which relies on its surface persisting. |
| Fonts rasterised at build time | `c6a04d8f` | Rasterising in the kernel would need floating point and a scanline filler. |
| Browser rules | `724c2673`, `b621ddce`, `5e62b39d`, `73dd558a`, `76965afe` | Started with no CSS on purpose. JS features it lacks are refused by name. XHR but no `fetch`, because there are no promises. No capture phase. Cookies live only in memory. Address-bar searches go to DuckDuckGo lite, because Google's results need JS. |
| An SDK of four files, with userland built from exactly those | `c91e550b` | Programs should not need a clone of the repository. `sdkcheck` builds outside the tree to prove it. |
| Everything through one gate (`pipeline/gate.sh`) | `fa574d5c` and later | Serious bugs were found by someone other than the author, or by a test. The gate decides, not the agent. A step that fails gets a second go alone, which forgives a busy host and nothing else (`bba15123`). |
| A new release rather than re-uploaded assets | `9255eeb0` | The tag and its assets must describe the same code. |
| GPL-3.0-or-later | `9c5edad5` | A modified version must stay open. Earlier versions stay MIT. |
| Author compiled into the kernel image | `eedd2f85` | Removing the credit takes a deliberate rebuild, which the GPL does not let anyone hide. |

---

## 3. Bugs found and fixed: a catalogue of traps

Each row gives the symptom, the root cause, the fix and the commit.

### 3.1 Boot and firmware

| Symptom | Root cause | Fix | Commit |
|---|---|---|---|
| "could not read the boot device" after 12 chunks | Asked for a sector past the end of the image | Clamp the last read | `e7511b5d` |
| VMware "invalid part of memory" on boot | Firmware loaded only 512 bytes of the loader; the reload used the kernel's LBA; the boot drive was saved inside the overwritten region | Stage one stands alone and reloads the rest; the loader carries its own LBA; the drive is parked elsewhere | `7ab04230` |
| Possible corruption on strict firmware | The disk address packet sat inside the 2 KiB being overwritten | Move it | `9443bbef` |
| A dead machine on a wrong sector size | Trusted the sector size the firmware reported | Read elsewhere, check a signature beyond byte 512, retry 512 and 2048 | `d0c2a37c` |
| Triple fault with a blank screen | A20 was checked only after the first method, so the kernel copy wrapped over the IVT | Four methods, each verified; refuse to boot with a message | `ff3a019d` |
| GP fault booting under UEFI | A stale UEFI loader in `build/` jumped into the multiboot header | `build.sh` builds the loader | `99ef2d8e` |
| The loader refuses its own signature after the rename | A `.set` and a `.long` of the same magic drifted apart | `check_loader.py`; the handoff magic's halves are checked too | `1b634221`, `a2c408fe` |
| **Triple fault on every VMware boot, since the beginning** | VMware's INT 13h reloads FS and drops the unreal-mode limit. The resulting #GP is vector 13, the same as INT 13h, so every failed copy "succeeded" | Re-enter unreal mode after every BIOS call; check the kernel arrived before jumping | `0a278c83` |
| Boots once, then VMware "invalid part of memory" on every later boot | The formatter wrote 55 AA with no code, so the BIOS ran zeros; the FAT32 jump offset was wrong too | A 25-byte stub, a jump that depends on FAT width, and `bootcheck`; in-place repair on mount, with three guards | `0fe2d1bb`, `6ae86e42` |
| UEFI dead for two releases, reporting only `0x800000000000000e` | A kernel of about 9 MB linked at 1 MiB overlapped the ACPI NVS at 8 MiB | Load anywhere and move, with a diagnostic; link at 16 MiB | `ca1b8aa0`, `775b8ed5` |
| BIOS paths triple-faulted right after the move | The seventh copy of the load address was spelled `0x00100000`, which a search for `0x100000` does not find | Single source of truth plus a build check | `775b8ed5` |
| Black screen on hi-res laptops | Every mode was over the cap, so the loader kept the firmware's (largest) mode | Take the smallest mode | `4cc1eb31` |
| The ESP overflowed | The ESP was a fixed 4096 KB and the kernel had reached 4066 KB | Size it from its contents | `b621ddce` |
| One CPU and no ACPI on UEFI machines | The RSDP stored in the handoff was never read | Read it; use XSDT and MCFG | `e7bc2094` |
| An EFI shell instead of a boot | UEFI firmware and `-kernel` were given together | Boot the disc image | `3a43d2a2` |

### 3.2 Memory and paging

| Symptom | Root cause | Fix | Commit |
|---|---|---|---|
| The paint window failed to open | The 3 MiB back buffer filled a 4 MiB heap, and the PMM had not reserved the heap | 16 MiB heap and `pmm_reserve` | `1eb6c98d` |
| Address spaces leaking into each other | A program at 1 GiB shared the kernel's PML4 entry | User space at 512 GiB | `2506b9a7` |
| A page fault on a window surface | `sys_win_surface` returned `i32`, truncating the pointer, and an 8-digit print made the truncated value look right | Syscalls return a machine word; print 16 digits | `2506b9a7` |
| Freezes when a window was resized | `unmap_page` worked only on the loaded address space, and surfaces were swapped mid-draw | `unmap_page_in`; the swap happens on the program's next call | `81440051` |
| A program could make the kernel write into MMIO | `user_range_ok` accepted any present page | Require the user bit at both levels | `7898701c` |
| Machines with 16 GiB used 64 MiB | A fixed identity map | Map what the firmware reports, in 2 MiB pages, leaving the holes | `a049f85c` |
| A black screen with no message on UEFI laptops | The heap was fixed at 16 MiB and the back buffer did not fit (and UEFI has no VGA text mode) | Size the heap from the mode; draw direct as a fallback | `4cc1eb31` |
| The heap would have collided with the image | The heap started at a fixed 8 MiB, 400 KB past the image end | Start after the image; take a quarter of RAM | `6016cb4f` |
| Random allocator faults | A paging self-test left an unmapped page inside the heap | Restore it | `6016cb4f` |
| **Random deaths, 3 runs in 10, some with no output** | 16 KiB kernel stacks overflowed during certificate maths into the heap header next door | 32 KiB, painted stacks, a guard word checked on every switch | `67cfa304` |
| COW never ran on a real fault; any ring 3 bug panicked | A second vector-14 handler in `paging.c` panicked first | One fault path; status 139 | `652a8047` |
| Every `%p` since the 64-bit port printed a wrong, plausible address | Cast to `u32` | Print 64 bits; the self test asks for all 16 digits | `652a8047` |
| A fork that did not fork | COW needs the parent's PTEs made read-only as well, plus a TLB reload; ring-0 writes from syscalls land on COW pages | Handle COW before anything else, without requiring the user bit | `842fe452` |
| The browser died on a deep page | A fixed 16-page user stack | Grows on demand to 1 MiB | `9a8d8dc0` |
| SSE faults in every program | The ring 3 entry stack was aligned to 16 rather than 16 minus 8 | Fix the alignment; later, move entry into startup code | `b621ddce`, `d3a21b9b` |

### 3.3 Interrupts, scheduling, SMP

| Symptom | Root cause | Fix | Commit |
|---|---|---|---|
| **A machine given more cores never runs anything** | Spinning APs starved the BSP of timer ticks under a hypervisor, and the LAPIC was enabled only on the BSP | APs halt and are woken by IPI; enable each LAPIC | `d3983b6a` |
| No AP starts | PE and PG were set in separate writes in the trampoline | Set both in one write | `2506b9a7` |
| Silent hang after the last boot mark | The MADT interrupt overrides were ignored (IRQ0 arrives on GSI 2) | Honour them | `d0b4c2f1` |
| Waits that could never be satisfied | Blocking syscalls spun with IF clear, because an interrupt gate clears it | Wait with interrupts on | `b621ddce` |
| The clock ran double (100 Hz read as 200) | Yield raised the timer vector, which also ran the tick handler | Separate the yield path | `6016cb4f` |
| Short sleeps did not wait | `ms*hz/1000` truncated anything under 10 ms to 0 ticks | Round up (0 means yield) | `6016cb4f`, `9421daf5` |
| `sleep_ms(100)` took 0 ms; starvation; lossy typing; half-drawn frames | No idle task, so `pick_next` returned the task that had just asked to sleep | An idle task per CPU; one-lap round robin | `9421daf5` |
| Interrupt frames from two CPUs corrupting each other (latent) | One TSS for all CPUs | A TSS per CPU; RSP0 written before start | `5a6fdf94` |
| Boot hung at `== smp` | LAPIC calibration waited on ticks with IF clear, and ran before the LAPIC was mapped (so the rate was 0) | Read the 8254 directly, after mapping | `7812519e` |
| APs ran nothing | `sched_init` ran after `smp_init` and wiped the idle tasks | Fix the init order; add `/sys/cpu` counters | `7812519e` |
| Heisenbug, 1 run in 3 | Lock release was decided by task kind instead of the saved frame, so a program preempted mid-syscall resumed without the lock | Decide from the frame | `7812519e` |
| Deadlock in the compositor | IRQs queued on the kernel lock | The timer skips a tick instead of blocking; the wake IPI takes no lock | `7812519e` |
| Garbled screen from an AP | PAT was never programmed on the APs | Program it on every CPU | `6016cb4f` |
| Keyboard and mouse dead until power cycle | An 8042 byte arrived before unmask and used up the edge | One path for every byte, plus a timer poll | `3fa89199` |
| The trackpad was reported absent | The timer drained the Synaptics reply | Detect before interrupts are on | `0a4f4120` |
| A boot that never finishes on xHCI machines | Delays counted in ticks before interrupts existed | Count delays in port writes | `908ba871` |

### 3.4 Device drivers

| Symptom | Root cause | Fix | Commit |
|---|---|---|---|
| MMIO devices worked only by luck | The memory-space bit was never set | Set it | `7898701c` |
| The previous packet's tail leaked onto the wire | Padding copied up to 60 bytes past the frame | Zero-fill instead | `7898701c` |
| Text console only on VMware | No Bochs VBE there, and the boot log hard-coded "vbe" | SVGA II driver with FIFO rectangle updates; the backend is reported truthfully | `0b8a897a` |
| 1.5 s per redraw | An uncached framebuffer plus a volatile byte-wise memcpy | PAT slot 4 write-combining, 8-byte copies, `sfence` | `fca59620` |
| One HDA command worked, then all timed out | RINTCNT was 1 with the response interrupt disabled | Enable it | `16d6acd8` |
| Every sound stuttered | The ring was silenced only behind the play position | Silence everything outside the queue | `16d6acd8` |
| Notes repeated forever on VMware | The ES1371 play position never moves | Fall back to the clock after 250 ms | `6016cb4f` |
| A USB stick refused commands | Sense data must be read after attach; the context entry count dropped an endpoint | Read sense and retry; the count only rises | `ee9f599a` |
| Truncated USB descriptors | The max packet size was guessed | Start at 8, then update | `908ba871` |
| The second RNDIS message never sent | Control buffers on the kernel stack are not identity mapped; InfoBufferOffset was 20 instead of 0 | Heap buffers, and `xhci_control` refuses stack pointers; offset 0 | `8aa13e43` |
| Large pages stopped at three fifths | PCnet acknowledgement cleared IENA; the RX ring (48 KB) was smaller than the 64 KB window | Keep IENA set; 64 buffers | `a8b3ed57` |
| The PCnet worked for a while, then stopped | The poll never touched CSR0 | Read and acknowledge CSR0 | `6161281a` |
| A wait on an ownership bit hung | e1000 rings were not volatile | Make them volatile | `a8b3ed57` |
| Silent write failures on AHCI | The block layer did not split requests (AHCI takes at most 8 sectors) | Split in `blockdev.c` | `e7bc2094` |
| Reboot locked up on laptops | The 8042 status reads 0xFF when there is no 8042, so the wait never ended | Bounded wait, then the reset register, then a triple fault | `4cc1eb31` |
| Triple fault about 10 s into every other boot | The NIC IRQ ran the whole stack inline and recursed through ARP until the stack overflowed | IRQ queues frames; a task runs the stack | `6016cb4f` |

### 3.5 Filesystem

| Symptom | Root cause | Fix | Commit |
|---|---|---|---|
| Duplicate README.TXT and readme.txt | No case folding | Fold 8.3 names to lower case | `5c12567a` |
| Writing was O(volume) per cluster | The allocator reread a sector per entry and restarted at 2 | Cache plus a resume hint (43,647 reads became 542) | `7898701c` |
| Two root files destroyed on FAT32 | `fat_reclaim` freed the root cluster, because nothing points at it; the root read as empty; cluster numbers were truncated to 16 bits | Fix all three; the test image puts a file at cluster 70,000 | `d0b4c2f1` |
| `ls /` empty in ring 3 | A 32-byte name field, but `VFS_NAME_MAX` is 64, so `strncpy` padding overwrote saved registers | Widen the field; `static_assert`; the gate compares headers | `e6325949`, `ae69771e` |
| An 8 GB disk never formatted | The formatter wrote only FAT16 with 2 KB clusters | Scale the cluster size; FAT32 | `6016cb4f` |
| Rebooting destroyed the log of why it failed | This boot's flush overwrote the previous record | Lift the record into memory first | `9ef316a2` |
| A FAT16 image made by mkfat is not recognised as zelr's own | The serial variant byte: mkfat writes 02, the kernel 00 | **Left alone on purpose in the rename** | `1b634221` |

### 3.6 Network and TLS

| Symptom | Root cause | Fix | Commit |
|---|---|---|---|
| Redirects failed at random | The local port came from the clock, so two connections could share one | Walk ports; skip ports in use | `724c2673`, `48e2e7c4` |
| Local fetches always failed | The sequence counter was advanced after sending while the IRQ side updated it, leaving the connection one byte ahead; reads did not consume; the window was a constant; FIN was off by one | Compute the sequence first; consuming reads; a real window | `1f9d69c3` |
| A 200 KB page came back whole only half the time | A FIN was accepted out of order | Accept FIN only in sequence; `tcp_recv` polls the card | `6016cb4f` |
| "Connection refused" with "no error" | No DHCP at boot; `connect` returned -1 for everything; DNS sent once | DHCP task; five reasons; four DNS tries | `6161281a` |
| Handshake done, but the first record failed to authenticate | Application secrets were derived over a transcript one message short | RFC 8448 known answers | `0e6e9a27` |
| No keys could be made | `rng_init` was never called; jitter was sampled before interrupts were on | Sample in the timer IRQ | `0e6e9a27` |
| Point doubling was wrong | The field-add output aliased the second operand | Fix the add; four permanent curve checks | `4a6915db` |
| Every certificate failed | The SPKI parser was one nesting level short | Fix the parser | `fd12462a` |
| The BBC and archive.org refused | Only the SHA-256 DigestInfo prefix was written; a served root carried an old SHA-1 self-signature | Add all three prefixes; prefer the store's copy | `28490abc` |
| Correctly signed sites rejected | `mkroots.py` never existed; the store was Windows-derived; chains were processed in arrival order (geeksforgeeks sends its leaf twice); the walk went past the first trust anchor | Mozilla store (121 roots); issuers by name; stop at the first anchor | `76965afe` |
| Pictures loaded one after another | A single connection held in file-level variables | Six connections; peer address checked; sockets owned by the caller | `48e2e7c4` |
| One encrypted connection per machine | `static tls_t T` | A session per connection | `011292c7` |

### 3.7 Desktop, input, UI

| Symptom | Root cause | Fix | Commit |
|---|---|---|---|
| Heap corruption on closing paint | `wm_close` freed a canvas owned by the server, through an interior pointer | Leave it to the server | `7898701c` |
| The surface was off by a page | Mapped one page early | Fix the mapping | `b937f79c` |
| Ctrl+C was impossible | The WM stripped modifiers, and Ctrl+letter arrives as a control code | Pass Ctrl through; `key_ctrl_letter` | `c4aa6083` |
| Hour-wrong clock reads (latent) | RTC update-in-progress, and BCD | Read twice and compare; honour BCD | `09bc4710` |
| The strokes of H vanished | An even-odd fill over overlapping strokes | Union per contour | `c6a04d8f` |
| Letters missing from typed text | One key per frame; the browser drained 16 keys and dropped the rest | Queue all keys | `724c2673`, `3689baf7` |
| Clicks lost | The WM sampled the buttons once per pass; the 32-entry event ring filled with moves | The driver keeps every transition; moves are folded only between identical button states | `724c2673`, `5168ba0e` |
| A colour set by hand turned black | Hex without `0x` parsed as 6 | Add the prefix | `bc662355` |
| Two addresses run together | Select-all happened on focus instead of on click | Select on click | `bc662355` |
| An idle machine read 100% CPU | Slices were counted, not work | Count halted ticks as idle | `ae69771e` |
| 78 / 4 computed as 78 / 784 | Display state leaked into the operand | Fix it | `ae69771e` |
| Empty maximised card games | The compositor read frames mid-draw | Server-side double buffer (the per-game shadow buffers removed) | `c004480b`, `ad87d42b` |
| Long URLs typed in the terminal cut at column 80 | The line length was capped at the window width | Scroll the line horizontally | `1a2bb487` |

### 3.8 Browser, JS, CSS

| Symptom | Root cause | Fix | Commit |
|---|---|---|---|
| **The browser silently disappeared** | The built-in program table was full at 16 and registration returned quietly | A compile-time assert (hit again at 32, and the build caught it) | `b621ddce`, `168fbaf4` |
| `className` did nothing | Only the newest element's attribute run could grow | Relocate the run | `b621ddce` |
| `a.b` on null was silent | Returned undefined | Throw | `b621ddce` |
| Every row stacked | The flexbox code never ran (`lay_is_block_node`) | Count `display:flex` | `bc662355` |
| JPEG gradients came out as noise | A Taylor series evaluated at up to 20 rad | Range reduction | `bc662355` |
| No inputs on any page | `LK_FIELD` was never created | Form controls | `3689baf7` |
| Regex against divide | One-token lexing | Track whether the last token can end an expression | `d29a65dd` |
| Handlers had nothing to run in | The JS world was torn down after load | A world per page | `5e62b39d` |
| Google stacked against the left edge | No `<center>`; `box-sizing` backwards | Add `<center>`; fix `box-sizing` | `193a0dfa` |
| "this is not a function" | No `call`, `apply` or `bind` | Implement them; name the callee | `3f4cdb5b` |
| Blank or one-word-wide pages | Lengths in hundredths in a `short` (960px became 304); `::-webkit-scrollbar` matched like `*`; `height:%` resolved against the width; `vw` ignored; buffers too small; decompression all or nothing | Fix each | `ff791fec` |
| Framework pages blank | A `visibility:hidden` wrapper | Lay out again ignoring visibility. **Only described in the v0.37.0 notes; no commit message mentions it** | v0.37.0 |

### 3.9 Tests, harness, gate, build, release

| Symptom | Root cause | Fix | Commit |
|---|---|---|---|
| False SSE hits | Byte-scanning immediates and displacements | Filter addresses; resolve calls and RIP-relative operands | `1eb6c98d`, `ebefcc48`, `f9c37761` |
| A stale program shipped in the image | zig cache plus `.incbin`: `builtin.S` was unchanged | Put a checksum of the blobs on the compiler command line | `774d9d8b` |
| The v0.8.0 launcher crashed | `PublishSingleFile` hid the WPF native libraries | Packaging lives in the csproj | `5e7d0904` |
| The black-box test passed with no disk write | It read the serial mirror | Fence the output | `9ef316a2` |
| The FAT32 test could not see truncation | All clusters were below 65,535 | Place a file at 70,000 | `d0b4c2f1` |
| Parallel harnesses deleted each other's disks | Images named after the harness | Name by PID | `ed5ad1d5` |
| "Feature broken" that was really a stale coordinate (three times) | Hard-coded geometry | Derive positions from the source or the screen | `c6a04d8f`, `bc662355`, `5168ba0e`, `9872d2cf` |
| A free-count assert failed only under whpx | The collector frees on a later tick | Bounded wait | `2bc4dd09` |
| A blank FAIL line | `grep -q` pipes; four builds racing into one directory | Keep step output; `ZELR_PREBUILT` | `9f20417e` |
| The version check passed either way | It matched a word present in both outcomes | Use the exit status | `e11eb296` |
| A check asserted the bug | It checked the spin counter climbed | Require the idle AP to be asleep | `d3983b6a` |
| 1 run in 10 flaky | Fixed sleeps; screenshots read before the write finished; an undrained serial pipe froze the guest; three checks vacuous | Wait on conditions; the gate uses exit status and a lock | `0d83457c` |
| Checks passing on echoed text | `Guest.run` returns the whole console | `fresh()`; read only the command's output | `ee9f599a`, `379bed46` |
| Serial reads captured by a focused window; an icon lit by its own open panel | Harness assumptions | Rewritten checks; the README table generated from a run | `fcd0321a` |
| "Busy host" failures | Dropped drags and serial bytes; reads too early; `click_for` double-entered a digit | `drag_for`; wait for the value; retry the whole sequence; a second go alone | `6f2b58cf`, `bba15123`, `12cc46b5` |
| A misdiagnosed regression | One run each side of a check that fails half the time | `page_settled`; the lesson written down | `12cc46b5` |
| Tests silently weakened by the compiler | Division by zero and impossible pointers optimised away | Volatile sinks | `652a8047` |
| A broken-kernel test turned into a fork bomb | A child that loses its argv cannot tell it is a child | The parent leaves a note | `d3a21b9b` |
| Self-tests asserting data rather than code | The old certificate store | Check invariants instead | `76965afe` |
| Releases had no downloadable files | Notes written by hand, artifacts never built | `pipeline/release.sh` builds, checks the version, uploads | `ab6d8d90` (probable) |
| The KERNEL_VERSION string drifted twice | Edited by hand | `tools/check_version.py` in the gate | `e85dfd1e`, `e11eb296` |

### 3.10 The lessons, generalised

1. **Duplicated facts drift.** Examples:
   - handoff magic halves; stage-one `.set` against `.long`;
   - the load address in seven places;
   - syscall numbers and structs declared in two headers;
   - settings defaults kept twice;
   - harness geometry; the version string.

   The project's answer each time was a single source and a build or gate check: `check_loader.py`, `loadaddr.py`, `abicheck.py`, `check_version.py`, the `theme.c` table, and glyph advances read from the generated header. Keep adding those.
2. **Narrow types truncate silently.** Examples: `i32` pointer returns, `%p` as `u32`, 16-bit cluster numbers, CSS lengths in a `short`, 32-bit sector numbers. Partitions starting past 2 TB are still skipped for this reason.
3. **Nothing that runs before interrupts are enabled may wait on ticks.** Examples: xHCI init, LAPIC calibration, jitter entropy, trackpad detection.
4. **QEMU is lenient.** It loads every boot sector, keeps the FS limit, reports doorbell stride 0, updates the RTC atomically, and routes to devices behind a hub regardless. VMware, VirtualBox and real hardware are not lenient. Protections QEMU cannot exercise stay marked as unverified.
5. **Silence is the normal failure mode.** It shows up as a status code, a black screen, a reset, a missing program, or a dot printed per failed copy. Every fix added a way for the failure to name itself.
6. **Tests pass for the wrong reason.** A check is not done until it has been seen failing against a deliberately broken build, and until it measures against something outside the code under test: a server, a recording, the clock, free memory.
7. **Timing.** Do not sleep and then look; wait for the condition. A busy host drops serial bytes and mouse messages. One run either side of a flaky check proves nothing.
8. **Kernel stacks come from the heap**, so an overflow corrupts a neighbour. Keep deep work (crypto, recursion) off interrupt paths and watch the guard.
9. **Anything a device reads by DMA must be at an identity-mapped address, and anything a device writes must be volatile.** MMIO must never be mapped cacheable.
10. **Moving work from an IRQ into a task exposes races** that were safe only because an IRQ cannot be preempted.
11. **Stale build artifacts ship.** The zig cache with `.incbin`, the UEFI loader in `build/`, single-file publish, and assets built from a tree other than the tag all did. Rebuild everything the release depends on, and check the version string inside each artifact.

---

## 4. Removed, renamed or replaced

| Date | Commit | From | To |
|---|---|---|---|
| 08-28 | `5c12567a` | Private disk format readable only by its own kernel | FAT16 (and FAT32 from `d0b4c2f1`, 09-14) |
| 08-28 | `b937f79c` | paint as kernel code | A ring 3 ELF over the window server |
| 08-29 | `dfd3a27b` | Whole in-RAM filesystem rewritten to disk on each change | VFS: built-in programs, then FAT, then RAM fs; `fs.c` only for machines with no disk |
| 08-29 | `e7511b5d`, `bab4aac6` | QEMU `-kernel` as the bootloader | Its own BIOS loader and UEFI loader (`-kernel` kept for development through the a.out kludge) |
| 08-30 | `bab4aac6`, `f9c37761` | Multiboot structure | `include/handoff.h` |
| 08-30 | `2506b9a7` | 32-bit protected mode, two-level paging, ELF32 | 64-bit long mode, four levels, ELF64 |
| 08-30 | `34db02d3` | `qemu-system-i386` and an embedded ELF in the launcher | `qemu-system-x86_64` booting the image |
| 08-30 | `2ea9dda3` | README claims "BIOS only" and "32-bit only" | Removed |
| 08-30 | `310adec9` | Bitmap font rendered from Consolas through PIL | Hand-drawn 8x16 glyphs |
| 09-14 | `c6a04d8f` | Bitmap font for the UI | Outline typeface from `tools/genface.py` (weights at `32fd308c`; the monospace derived by condensing at `b621ddce`). The kernel console keeps the 8x16 bitmap |
| 09-14 | `d0b4c2f1` | 8259 PIC routing | IOAPIC plus the firmware's overrides |
| 09-14 | `e7bc2094` | RSDT and a low-memory RSDP scan only | XSDT, and the RSDP from the UEFI handoff |
| 09-14 | `09bc4710` | Uptime counter on the panel; a planned second config file `/cfg/theme` | A real clock; a single config file |
| 09-14 | `1b634221` | **nyx** | **zelr**. Magics: NYXKERN became ZELRKRN, NYXBOOT1 became ZELRBOT1, NYXSMP01 became ZELRSMP1, the handoff magic and FAT volume stamp changed (ZLR). `/nyx.cfg` became `/zelr.cfg`, `nyx.bat` became `zelr.bat`, `NYX_PREBUILT` became `ZELR_PREBUILT`, and the C# namespace changed. GitHub `blavese/nyx` became `blavese/zelr`. **No reason for the rename is recorded.** |
| 09-15 | `a049f85c` | Fixed 64 MiB identity map | Map all reported RAM |
| 09-16 | `9c5edad5` | **MIT licence** | **GPL-3.0-or-later** (MIT applies to v0.16.1 and earlier) |
| 09-16 | `3c22354a` | Full-screen flush every frame | Damage rectangles, then band diffing (`6016cb4f`) |
| 09-16 to 09-20 | several | Desktop look | Rounded and shadowed (v0.6.0), then surfaces with an accent frame (v0.15.0), then the bevelled "built rather than tinted" look (v0.25.0), then a translucent "material" with the bevel kept as a Settings option (v0.30.0) |
| 09-16 to 09-20 | several | Taskbar | Flat taskbar with badge (v0.6.0), then floating (v0.15.0), then an auto-hiding panel with **pinned apps** (`d802003f`, v0.20.0), then a flush full-width bevelled panel (v0.25.0), then a floating glass **dock**: **pinned app badges removed** and a search field added (`bc662355`, v0.31.0) |
| 09-20 | `b8c7a777` | Search field in the middle of the dock (a second launcher) | Find button in the tray plus Ctrl+F, searching the screen |
| 09-20 | `bc662355` | Launcher with 13 rows; four hand-kept settings lists | Two columns of kinds (a Games row from `c004480b`); one settings table |
| 09-17 to 09-21 | `ae69771e`, `d3a21b9b` | One string argument per task, and `SYS_GETARG` (44) | argv on the stack; 44 retired; `SYS_SPAWN_ARG` renamed `SYS_SPAWN_ARGV` |
| 09-20 | `b621ddce` | Only the kernel shell | Also `/bin/sh` in ring 3 (the kernel shell stays on the console) |
| 09-21 | `842fe452` | Eager fork copy | Copy-on-write |
| 09-21 | `ad87d42b` | Per-game shadow buffers | Server-side second surface |
| 09-21 | `652a8047` | Every exception panics | Ring 3 faults end the program |
| 09-21 | `9a8d8dc0` | 16-page user stack | Grows to 1 MiB |
| 09-21 | `775b8ed5` | Kernel at 1 MiB | Kernel at 16 MiB |
| 09-21 | `76965afe` | 48 roots from the Windows store; searches through Google | 121 roots from Mozilla (`tools/mkroots.py`); searches through DuckDuckGo lite |
| 09-20, 09-21 | `4b858949`, `48e2e7c4`, `011292c7` | `Accept-Encoding: identity`, a connection per request, one TCP connection, one TLS state | gzip, keep-alive, six connections, a TLS session per connection |
| 09-03 | `62c1fb0b` | Pipeline default `max` (Opus writes, Codex reviews) | Default `balanced` (Codex writes, Sonnet reviews) |

---

## 5. The AI-assistance record

### 5.1 Chronology

1. **Codex reviews first (about 08-29).** `pipeline/README.md` credits Codex with finding three bugs in code whose own tests passed: the double free in `wm_close`, the permission check that accepted merely-present pages, and the quadratic FAT allocator. Those are exactly the fixes in `7898701c` (2026-08-29 15:01). They were developed on a separate line from 0.5.0 and merged in `ebefcc48`. **Neither commit carries a trailer.**
2. **The pipeline (08-31 09:31 to 09-03 07:41).** `fa574d5c` added scripts that run Claude Code and Codex headless:
   - one model writes and the other reviews the diff as JSON;
   - the author answers each finding;
   - `gate.sh` decides mechanically (build, 213 kernel checks and the serial shell test; then the four boot paths and three desktop harnesses);
   - failures are deleted and marked blocked, with logs kept under `pipeline/state/` (gitignored).

   The backlog held eight tasks. What happened when it ran:
   - The first run died after 14 s because the claude CLI token had expired, so `preflight.sh` was added (`43105911`).
   - The first real cycle wrote a CMOS driver but could not run the build, because it had edit permission only. That led to the tool allowlist, worktree batches (`batch.sh`) and matched models (`406ad564`).
   - `batch.sh` itself committed `22c2a8cd` (claiming rtc-clock, pipes and lfn-read) and `97048348` (after the batch failed together and was split).
   - `62c1fb0b` added the `thrifty`, `balanced` and `max` profiles and `land.sh`, because agent turns were the binding cost.
3. **The pipeline stops.** There is no pipeline commit after 09-03.
   - There is no `pipeline: X landed` commit.
   - No commit carries the pipeline's landing trailer (`Written by X, reviewed by Y`).
   - The backlog still shows rtc-clock, pipes and lfn-read as `blocked`.

   **No pipeline-authored change ever reached `main`.** What lasted is `pipeline/gate.sh`, run by hand as the project's gate ever since. `pipeline/release.sh` was added to the same directory later, probably in `ab6d8d90`.
4. **Interactive work with trailers (09-18 10:57 onward).** Every commit from `0e6e9a27` to `6048716a` (55 commits) ends with `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.
5. **The NOTICE (`eedd2f85`, 09-21).** It records that some of the work was AI-assisted and marked by trailers. The commit counts 43 trailers among 138 commits, which is correct for the commits before it. It says the design, direction, decisions and copyright are the author's.

### 5.2 Trailers in numbers

- **55 of 150 commits (37%).** All carry the same trailer and the same model name: **Claude Opus 5**. No other trailer type appears (no Signed-off-by, no Reviewed-by).

**By release:**

| Release | Commits with a trailer |
|---|---|
| v0.28.0 | 1 of 5 (`0e6e9a27` only; the four crypto commits that morning have none) |
| v0.29.0 | 3 of 3 |
| v0.30.0 | 1 of 1 |
| v0.31.0 | 1 of 1 |
| v0.32.0 | 1 of 1 |
| v0.33.0 | 8 of 8 |
| v0.34.0 | 14 of 14 |
| v0.35.0 | 6 of 6 |
| v0.36.0 | 8 of 8 |
| v0.37.0 | 10 of 10 |
| after v0.37.0 | 2 of 2 |
| v0.1.0 to v0.27.0 | 0 of 95 |

- **Model and tool names anywhere in the record:**
  - `Claude Opus 5` in the trailers;
  - `Claude Code` and `Codex` as tools;
  - `CLAUDE_MODEL` set to `opus` or `sonnet`;
  - `CODEX_MODEL` set to `gpt-5.6-sol` (the "strongest") or `gpt-5.6-luna` (the cheap one), in `pipeline/models.sh` and the README.

### 5.3 Pipeline backlog against where each feature actually landed

| Backlog task | Pipeline author | Backlog state today | Actually landed in | Trailer |
|---|---|---|---|---|
| rtc-clock | claude | blocked | `09bc4710` (09-14) | no |
| pipes | codex | blocked | `b621ddce` (09-20) | yes |
| lfn-read | claude | blocked | `379bed46` (09-16) | no |
| ap-tasks | codex | done | `7812519e` (09-21) | yes |
| editor | claude | todo | `c4aa6083` (09-14) | no |
| nvme | codex | todo | `d0b4c2f1` (09-14) | no |
| demand-pages | claude | todo | `842fe452` (COW) and `d0200f96` (mmap), 09-21 | yes |
| xhci-hid | codex | todo | `908ba871` (09-15) | no |

The backlog table has not been updated for five of these. `9872d2cf` updated only ap-tasks and the ideas list.

### 5.4 What can and cannot be read from the record

- **Can:** which commits declare AI co-authorship (the 55); that the pipeline produced nothing that landed; that Codex review fed `7898701c`.
- **Cannot:** which of the 95 earlier commits were written or assisted by a model. The voice and structure of the messages are identical before and after the trailers start, first-person asides included (e.g. `5e7d0904` and `3fa89199` without a trailer, `12cc46b5` and `193a0dfa` with one), so style says nothing about authorship.
- **Discrepancies to be aware of:**
  - The NOTICE speaks of "an AI model" and of trailers marking the commits it touched. The project's own pipeline README shows a second model family (Codex) contributing findings to a commit with no trailer.
  - zelr.org's copy says "1 person writing it" and "written by one person" and never mentions AI assistance.

---

## 6. Style: how to write commits and release notes here

### 6.1 Subjects

- **Format.**
  - One line with no trailing period (0 of 150 have one).
  - Average 51 characters, maximum 80.
  - No `feat:`/`fix:` prefixes, scopes, issue numbers or emoji.
  - The only prefixed subjects are the two machine-written `pipeline:` commits.
- **Two eras of mood.**
  - Through `379bed46` (v0.19.0), subjects were imperative: 71 of those 80 commits. Examples: `Add ...`, `Fix ...`, `Drive the usb controller, so a laptop keyboard works`, `Map the memory the machine has, instead of the first 64 MiB of it`.
  - From `d802003f` (09-16 17:34) onward, only 3 of 70 are imperative (`a8b3ed57`, `1f9d69c3`, `eedd2f85`). **Current practice** names the new state, the capability, or the fault as a noun phrase or plain statement.
- **Recurring shapes:**
  - "X, and Y" for two things in one commit;
  - "X, rather than Y" or "X, instead of Y";
  - a wry or literary turn is normal (`One winner takes the s, ...`).
  - A subject may begin in lower case when it starts with a code word or protocol (`call, apply and bind, ...`, `https, and ...`).
  - Lower-case `usb`, `uefi`, `https` inside subjects is common, but not consistent.
- **Special kinds:**
  - Test fixes name the checks and their mistake: `Four checks that were reporting a busy host as a broken machine`.
  - README fixes say what the readme claimed: `The readme said six things that had stopped being true`.
  - Release commits are `zelr 0.N.0`.
- **Representative subjects to imitate:**
  - `A stack that grows when the program needs more of it` (`9a8d8dc0`)
  - `The kernel is linked at sixteen megabytes, and boots under UEFI again` (`775b8ed5`)
  - `Four checks that were reporting a busy host as a broken machine` (`bba15123`)
  - `The readme said six things that had stopped being true` (`11848dd3`)
  - `zelr 0.37.0` (`4a6948e4`)

### 6.2 Bodies

- **Length and layout.**
  - Long-form prose, typically 150 to 600 words; the median is about 300 and the longest is 1,562 (`6016cb4f`).
  - Wrapped at about 72 to 76 columns. Paragraphs are separated by blank lines.
  - 10 commits have an empty body; all are README tweaks, bare version bumps, or pipeline bookkeeping.
- **Order of content.** The usual order is:
  1. what went wrong, as seen from outside;
  2. the mechanism, explained plainly;
  3. what changed;
  4. what it deliberately does not do, and why;
  5. what checks it, including that the check was run against a broken build and failed;
  6. before and after measurements;
  7. a tally line of suite counts, such as `self test 560, https 21.` or `Self test 548 on one processor, 559 on four, 556 on q35.`

  Long commits (20 of them, from `5168ba0e` onward) split sections with a dashed rule line, e.g. `--- what checks it ------...`, padded to about column 76.
- **Punctuation and spelling.**
  - **Dashes are written ` -- `**: 43 commits use it, and exactly one em-dash appears in 150 commits (`b621ddce`).
  - British spelling: colour, behaviour, centre, grey, recognise, maximised. The NOTICE and site use "licence" for the noun; the code and some commits say "license".
  - Small and medium numbers are spelled out in prose ("sixteen pages", "thirty two buffers", "two hundred and forty kilobytes"), usually without hyphens. Measurements and addresses are numerals (4208 KiB, `0x800000`).
  - Code identifiers appear bare or in backticks. There is no Markdown heading or bullet structure; two-space indented blocks hold measurements or program output.
- **Tone.**
  - Plain, exact and unhyped. Mistakes are admitted, including the author's own misdiagnoses (`12cc46b5`, `842fe452`), and uncertainty is stated.
  - Aphoristic sentences explain principles.
  - First person singular is used sparingly.
- **Trailer.** AI-assisted commits end with a blank line and `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`. Given the NOTICE, a future developer using any AI assistance should keep the record complete in the same way, naming whichever model was used.
- **Gate vocabulary.** Gate step names are plain-English claims, such as "the windows go where they are told" and "a file survives the power going out mid-write". Harnesses are `tools/<thing>check.py`.

### 6.3 Release notes

- **Title and assets.** The title is `zelr X.Y.Z`, set by `pipeline/release.sh`. Assets are `zelr.exe` and `zelr.iso`; for v0.10.0 and earlier they were `nyx.exe`, `nyx.iso` and `nyx.elf`.
- **Structure has evolved:**
  - v0.1.0 to v0.4.0: `## Windows` / `## New in this release` (or what is in it) / `## Verified` / `## Known limits`, with try-it command blocks. v0.5.0 is plain paragraphs.
  - v0.6.0 to v0.13.0: `###` sections such as On Windows, On a real machine, What is new, Fixed, Not here, Verification, What still does not work, Downloads, Where it runs (a small table per hypervisor).
  - v0.14.0 to v0.29.0: a bold one-line headline, a few paragraphs, a `**Checks:**` paragraph, and a stock closing line pointing Windows users to `zelr.exe` and real hardware to `zelr.iso`.
  - **v0.30.0 onward, the current form:**
    - lower-case `##` headings phrased like commit subjects;
    - bold lead-ins;
    - measurements in indented code blocks;
    - fixed closing sections: `## known, and not fixed` (the honest limits list, updated every release), `## the checks`, and sometimes `## also in this cycle` or `## what this still cannot do`.
    - From v0.35.0 the first sentence reads "This release is about ...".
- **Em-dashes.** The GitHub notes contain 95 em-dashes (v0.6.1 to v0.8.0 and v0.30.0 to v0.37.0). The copy on zelr.org contains none: whatever generates the site replaced them with colons, which sometimes produces awkward sentences. **Write release notes with ` -- ` or rewritten sentences, not em-dashes**, to match the commits and survive the site.

### 6.4 Versioning and how a release is cut

- **Scheme.**
  - `0.MINOR.0` for every release that adds capability, roughly one per headline feature or cluster, often several a day.
  - `0.MINOR.PATCH` only for fix-only releases: v0.6.1 (bootloader), v0.6.2 (font), v0.16.1 (input).
  - There is no 1.0, and there are no pre-release tags.
  - A fix after a release gets a new release rather than re-uploaded assets.
- **Single source.** `#define KERNEL_VERSION "0.37.0"` in `include/types.h` is the only version literal in the tree (checked by search). The kernel prints it in the boot banner and `uname`, the harnesses read it from the header (since `7ae3a310`), and it ends up inside both assets.
- **Bump commits over time:**
  - `Bump to 0.5.0`, `Bump to 0.6.0, and ...`, `Bump to 0.7.0`;
  - `Call this 0.8.0` through `Call this 0.13.0`;
  - `Set the version to 0.11.0`, after the fact;
  - v0.14.0 to v0.33.0 have **no separate bump commit**: each tag sits on the feature commit, so the bump was folded in (inferred from the tag mapping);
  - v0.34.0 onward has a dedicated `zelr 0.N.0` commit with a one-to-three-sentence summary body (v0.34.0's is empty).

  The messages do not list files. Since `KERNEL_VERSION` is the only version literal, a `zelr 0.N.0` commit changes at least that line.
- **Guards:**
  - `tools/check_version.py` (a gate step) fails if `KERNEL_VERSION` is behind the newest local tag.
  - `pipeline/release.sh v0.N.0 notes.md` refuses if the tag and `KERNEL_VERSION` differ. It then:
    - builds `build/zelr.iso` (`build.sh` and `tools/mkiso.py`) and `build/launcher/zelr.exe` (`dotnet publish launcher/ZelrLauncher.csproj`);
    - checks the version string is inside both;
    - creates or edits the GitHub release titled `zelr 0.N.0`;
    - uploads both assets.
- **Procedure as practised for v0.34.0 to v0.37.0:**
  1. Land the features, each with checks.
  2. Correct the README in its own commit ("The readme says ...").
  3. Commit `zelr 0.N.0` with the version set and a short summary.
  4. Run `bash pipeline/gate.sh full`.
  5. Run `bash pipeline/release.sh v0.N.0 notes.md`.
  6. Run `git fetch --tags`. `gh` creates the tag on the remote, and until you fetch, `check_version` compares against the previous tag. It was once green while a release out of date.
  7. **Update zelr.org separately** (see section 8). This step has been skipped for the last two releases.

---

## 7. The two commits after v0.37.0

### 7.1 `011292c7` (2026-09-22 14:10): A TLS session per connection, rather than one for the machine

What it claims to change:
- **Before.** The TCP stack had held six connections since `48e2e7c4`, but `kernel/tls.c` kept all TLS state in one file-level `static tls_t T`. A machine could have one encrypted connection plus plain ones, so a page and its pictures were fetched over https one handshake after another.
- **After.** Every field of the old state belongs to a per-connection session, named by the TCP handle it runs over. There is no second handle space: a session belongs to exactly one connection, and a connection carries at most one session. The tree confirms this: `static tls_t sessions[TCP_MAX]`.
- **Failure reporting.** A failed handshake tears down its connection, so the failure reason is also stored where a caller with no connection can read it. `SYS_TLS_STATUS` keeps its old signature, so SDK programs are not affected.
- **Self test.** The test's "is anything connected" question is now answered by `tls_any()`, and seven new checks show that sessions really are separate.
- **Tally.** Self test 560, https 21, seven real sites over https.
- **What it does not claim.** It says nothing about the browser now fetching https resources in parallel. It only removes the kernel limit.
- **Docs not yet updated:**
  - README limits still say "TLS is one session at a time" (line about 1606);
  - the v0.34.0 to v0.37.0 release notes list it as a known limit;
  - the backlog idea "TLS as more than one session" is still open;
  - `include/syscall.h` still describes TLS on "the same one socket".

### 7.2 `6048716a` (2026-09-22 14:23): A libc, written from scratch, and a program that has never heard of zelr

What it claims to change:
- **Motivation.** `sdkcheck` proved an outside program runs, but only one written against zelr's own `sdk/zelr.h`. The real question is whether C written for another system runs here.
- **`sdk/libc`, all implemented in the tree:**
  - headers `stddef`, `stdint`, `stdbool`, `limits`, `stdarg`, `string`, `ctype`, `stdlib`, `stdio`, `math`, `time`, `errno` and `assert`, with the signatures C specifies;
  - a first-fit allocator over `sbrk` that coalesces free blocks;
  - `FILE` as a descriptor plus a buffer;
  - one formatter shared by `printf` and `snprintf`;
  - a median-of-three `qsort`;
  - `math.h` functions as series or iterations with range reduction.
- **Two macros to coexist with `sdk/zelr.h`:**
  - `ZELR_NO_SUGAR` makes the header stop defining `strlen`, `memcpy`, `puts`, `exit`, `fread` and `rename`, leaving `zelr_`-prefixed names for the libc to call.
  - `ZELR_NO_START` moves the entry point into `crt0.c`, so `stdout` exists before `main`.
  - A program using only `zelr.h` with neither macro behaves exactly as before, and `sdkcheck` passes unchanged.
- **`tools/libccheck.py`** (a gate step, "and one written in nothing but standard C"):
  - copies `sdk/` out of the tree and builds `hello.c` there;
  - writes it to a FAT volume built by the repository, boots, and runs it;
  - checks the exact output: padding and alignment, `qsort` order, where `strtol` stopped, `sqrt`, `pow` and `sin` to six places, and an `fprintf` then `fgets` file round trip.
- **Tally.** libc 16, sdk 7.
- **Docs not yet updated:**
  - README ("No libc" at lines about 75 and 1768; the layout lists neither `sdk/libc` nor `tools/libccheck.py`; the SDK section still says "fifty-seven system calls" while the limits section says sixty-three, and 63 is correct);
  - NOTICE ("There is no libc");
  - zelr.org ("No libc").

  The spirit of those claims survives, since the libc is original, but the words are now literally wrong.

---

## 8. zelr.org

### 8.1 Hosting and headers (live HEAD on 2026-09-26)

- **Server.** `Server: cloudflare`, `CF-Cache-Status: HIT`, `CF-RAY`, NEL/Report-To pointing at Cloudflare. `Server-Timing` shows `cfWorker` time and zero origin time, so it is served from a Cloudflare Worker or static-assets deployment (Pages or Workers; the product cannot be told apart from the headers).
- **Security headers:**
  - `Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; form-action 'none'; frame-ancestors 'none'; base-uri 'self'`
  - `Strict-Transport-Security: max-age=31536000; includeSubDomains`
  - `X-Frame-Options: DENY`, `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`
  - `Permissions-Policy: geolocation=(), microphone=(), camera=(), interest-cohort=()`
- **Analytics.** Every page also carries an injected **Cloudflare Web Analytics beacon** (`static.cloudflareinsights.com/beacon.min.js`). **The site's own CSP blocks it** (`script-src 'self'`, `connect-src 'self'`), so analytics presumably record nothing and browsers log CSP violations.
- **Charset.** HTML is served as `Content-Type: text/html` without a charset. The pages rely on `<meta charset="utf-8">`, which is fine in browsers; naive fetchers see mojibake.
- **Design.** The stylesheet uses system fonts only (no external fonts), a dark default with a light scheme, and an orange accent (`#ff9040`). It describes itself as a "technical-modern" hairline-grid design.

### 8.2 Pages

| URL | Contents |
|---|---|
| `/` | Hero ("x86, 64-bit, written from scratch"). Stats: ~70,000 lines; **548** self-checks; 0 runtime deps; 1 person. Download rail "**v0.35.0, 21 Sep 2026**", with buttons to `github.com/blavese/zelr/releases/latest/download/zelr.exe` and `.iso`. Desktop screenshot. The day-one brief (keep building until it boots and works on a real laptop). "01 What it actually does": boot, usb, memory, disk, network, desktop, processes, javascript, programs, testing. Boot-log screenshot. "02 What went wrong along the way": three cases (8042 input death; the uncached framebuffer; three vacuous tests). "03 Getting it running": Windows, real hardware, from source with `run.sh`. "04 What it doesn't do". Footer links: Feed, Releases, Source, Licence, Donate. |
| `/releases` | "What changed, and what broke." States **38 releases**, 28 Aug to 21 Sep 2026, and that the project was called nyx until v0.11.0. Each entry: version, date, 7-character tag sha, headline, notes as HTML, asset links with sizes, and sha256 sums. Newest entry is v0.35.0. |
| `/source` | "Read the source." Archive **`zelr-src.tar.gz`: 2.5 MB, 380 files, the v0.35.0 tag, sha256 `ec7eaebc3427b598e2d3ae5782d7b5a58e649487ad8e80cc29d3ee1491d8b301`** (not re-verified here; downloading needs permission; HEAD confirms `application/gzip`). A tree with per-directory line counts (**70,761** hand-written lines of C and assembly). A generated-files table (40,472 lines: font tables, `roots.c`, test chain). A larger-modules table. Build steps (`tar xzf`, `cd zelr-0.35.0`, `./build.sh`, `./run.sh`, `-T`, `-i`). A note that Zig is only the compiler, and the claim "548 passed, 0 failed". |
| `/code/` | "The tree, file by file": a browsable listing of **every file at v0.35.0**, with line counts per file (README.md 1,738, build.sh 64, linker.ld 40, run.sh 47, zelr.bat 190). Directories: boot, bootloader, include, kernel, launcher, pipeline, sdk, tools, uefi, userland. **LICENSE and `docs/` are not listed; NOTICE did not exist yet.** |
| `/license` | GPL v3 introduction and full text. The rail label says "GPL-3.0". The final section is adapted to "HOW TO APPLY THIS TO zelr" with the copyright line for blavese. **No mention that v0.16.1 and earlier were MIT.** |
| `/donate` | "Support the project." **Monero (XMR) only**, one-off, no subscription, nothing unlocked. QR code (`/monero-qr.svg`), address `42hxor9c...ALzR5`, and a copy button (`/donate.js`). Describes one person working in their own time, no company. |
| `/releases.xml` | Atom feed "zelr releases", author `blavese`. **25 entries, v0.35.0 down to v0.12.0.** `<updated>2026-09-21T15:55:59Z</updated>` (the v0.35.0 release time). Entry ids `tag:zelr.org,2026:vX.Y.Z`, linking to `/releases#vX-Y-Z`. |
| Other | `/sitemap.xml` (/, /releases, /source, /code/, /license, /donate); `robots.txt` (allow all, sitemap); `site.webmanifest` (standalone PWA, `#0a0a0b`, icons 192 and 512); favicons; `og.png` (1200x630); `screenshot-desktop.png`; `screenshot-boot.png`; `style.css`. |

**Which version it advertises:** v0.35.0 everywhere (home, source archive, `/code/`, top of releases, feed). The site was last regenerated between v0.35.0 (09-21 15:55) and v0.36.0 (09-21 18:56). Much of its prose uses v0.34.0 figures (548 checks).

### 8.3 Claims on the site that are now stale

| Site claim | Where | Now (repo `main`, `6048716a`) | Stale since |
|---|---|---|---|
| v0.35.0, 21 Sep 2026; 38 releases; archive, tree and feed at v0.35.0 | home, `/releases`, `/source`, `/code/`, feed | v0.37.0 (22 Sep) plus two commits; 40 releases | v0.36.0 |
| Download buttons | home | They point at `releases/latest`, so they deliver **v0.37.0 binaries under a v0.35.0 label**, and the site has **no checksums for them** | v0.36.0 |
| 548 self-checks; "548-check self test"; "548 passed, 0 failed"; `selftest.c` described as "the 548 checks" | home, `/source` | 560 (per `011292c7`; the README says 552) | already wrong at v0.35.0 (552) |
| ~70,000 lines; 70,761 hand-written lines; per-directory and per-module counts | home meta and stats, `/source` | About 75,600 hand-written lines by the same directory split (my count of .c/.h/.S/.s/.asm/.inc: kernel 35,700, userland 30,000, sdk 3,152, include 4,667). Generated `kernel/roots.c` went from 4,867 to 11,576 lines (48 roots became 121). Modules grew: `js*.h` 6,345, `selftest.c` 3,605, `fat.c` 1,763, `browser.c` 1,746, `css.h` 1,468, `layout.h` 1,416, `syscall.c` 1,101, `tls.c` 904, `term.c` 1,875, `sh.c` 340 | v0.36.0 and v0.37.0 |
| Fifty-eight system calls | home limits; `/source` (for `syscall.c`) | **63** (numbers 0 to 63, 44 retired) | v0.36.0 |
| "a single-connection TCP client" | home, network entry | Six connections (v0.34.0); TLS per connection (`011292c7`) | v0.34.0, already stale when published |
| "Intel e1000 or Realtek RTL8139" | home, network entry | Also AMD PCnet (v0.27.0) and RNDIS over USB (v0.24.0) | v0.24.0 |
| Disk: AHCI, then ATA PIO | home, disk entry | NVMe too (v0.9.0; the hero does mention NVMe) | v0.9.0 |
| JS has no classes, async, regular expressions or arrow functions | home limits | Regex, arrow functions, `instanceof` and labels since v0.33.0. Still no classes, async, generators or promises | v0.33.0 |
| JS runs scripts after parse and before layout (implying one pass) | home, javascript entry | Persistent page world: listeners, timers, `script src`, XHR, load events (v0.33.0, v0.36.0) | v0.33.0 |
| "Type guide once it's up, or desktop for the windowing system" | home, getting running | The desktop opens by itself | v0.20.0 |
| "No libc"; "There is no libc" | home meta, og, prose | An original libc in `sdk/libc` | `6048716a` (unreleased) |
| "380 files" in the archive | `/source` | 401 files in the current tree copy | v0.36.0 |
| TCP implemented in `kernel/net.c` | `/source` | TCP lives in `kernel/tcp.c` | (older) |
| "1 person writing it"; "written by one person" | home | NOTICE records AI assistance; 55 of 150 commits carry Claude trailers | 09-18 onward |

Figures on the site that are still true: 64 GiB memory cap; external-hub hot-plug not noticed until reboot; one TLS version and suite; kernel integer-only; no wireless driver; SIGKILL cannot be caught.

### 8.4 Other defects on the site

- **Release notes are mechanically transformed on the site.** Em-dashes become colons, and indented preformatted blocks are flattened into run-on paragraphs (e.g. the four-boot-path table becomes one line, and so does `sdkcheck` output).
- **The analytics beacon is blocked by the site's own CSP.**
- The "testing" entry says the on-boot self-test takes screenshots; those are host-side harnesses.
- The licence page label omits "-or-later" and the MIT history.
- The README in the repository does not link to zelr.org at all.

### 8.5 Where the site's source lives

**Not in the repository.** A search of the tree finds no `zelr.org` reference, no site HTML or CSS, no feed generator, no Monero or donation material and no Cloudflare or wrangler config. Whoever takes over needs the site's own source and deploy credentials from the author. The only site-related artifact derivable from the repo is the source tarball (`git archive` of a tag). The site's releases page and feed are evidently generated from the GitHub release notes by a tool that also strips em-dashes; that tool is not in the tree.

---

## 9. Open threads

### 9.1 Stated next steps that have not been done

| Promise | Where | Status |
|---|---|---|
| **Self-hosting**: assembler, then a C-subset compiler, then linker. The notes call it the next threshold | v0.35.0 and v0.36.0 notes; backlog ideas | Not started. The backlog says it needs file mapping and a page cache first |
| **A lock finer than the one kernel lock** | `9872d2cf` (promoted to "next"); backlog; README | Open |
| **An Atheros wireless driver**, promised as coming next | v0.23.0 and v0.24.0 notes | Never started |
| File mapping, then a page cache and swap | v0.36.0 notes; backlog | Open |
| `sigaction`, signal masks, restarting interrupted syscalls | v0.36.0 notes; README | Open |
| Promises, `fetch`, `async`/`await`, generators, classes | v0.36.0 notes; `168fbaf4`; README | Open. Two of Google's ten scripts need generators or async, and Google's first script still stops (`3f4cdb5b`) |
| Layout emits about four times as many links as the document has (6,552 against 1,649), unexplained | v0.37.0 notes | Open |
| Framework-built pages cannot be shown | v0.37.0 notes | Stated limit |
| Floats and table column widths not laid out | v0.34.0 to v0.36.0 notes (`position` arrived in v0.37.0) | Presumably open |
| Persistent cookies (deferred until the machine has somewhere to list and delete them) | `4b858949` | Open |
| Dock at top or sides, settable font sizes, program lists in launcher and desktop | v0.31.0 notes | Open |
| A service model, a packet filter and traffic viewer, multiple users and permissions | backlog ideas; v0.36.0 notes | Open |
| Harness typing delays (50 ms per character, 6 s boot sleep), flagged for another pass | `ed5ad1d5` | Partly addressed by `0d83457c` |

### 9.2 Known limitations stated in the latest notes and README

- **Structural:**
  - no users or permissions;
  - no `fcntl` or `ioctl`;
  - no environment (`envp` is empty);
  - no shared libraries or dynamic linking;
  - no process groups or job control;
  - eight windows at most;
  - the system-info window is still kernel code.
- **Filesystem:**
  - two volumes at a time;
  - ASCII names only;
  - `rename` within one directory only;
  - anonymous `mmap` only, 16 ranges per task.
- **Memory:**
  - capped at 64 GiB;
  - the kernel at 16 MiB sets a memory floor (passes on 40 MiB).
- **Network:**
  - TCP sends one segment at a time, with no congestion control, window scaling or SACK;
  - no TLS resumption;
  - ping reaches only the local network under QEMU.
- **USB:**
  - keyboards, mice, hubs and storage only;
  - only root ports are watched for hot-plug;
  - polled with up to 10 ms of latency.
- **Blocking:**
  - `poll` is implemented by polling (up to 10 ms latency);
  - the XHR request stalls the browser while it runs.
- **Browser:** no capture phase; `instanceof` without inheritance; `bind` does not bind arguments; regex has no lookaround, backreferences, named groups or sticky flag; `position: fixed` is measured against the page; `bottom` alone is ignored.
- **Disks:** NVMe namespaces with 4 KiB blocks are refused; partitions past 2 TB are skipped (32-bit sector numbers).
- **Wireless:** impossible without vendor firmware, except on Atheros.

### 9.3 Unverified or unexplained

- Protections QEMU cannot exercise, marked as unverified:
  - the NVMe doorbell stride (`d0b4c2f1`);
  - RTC torn-read protection (`09bc4710`);
  - the xHCI hub bit (`4dc69945`);
  - the 8042 bring-up sequence (`3fa89199`, described as defensive rather than proven);
  - the trackpad on real hardware (v0.22.0).
- The v0.34.0 notes record one `smpcheck` failure after the lock fix that could never be reproduced.
- The FAT serial asymmetry (mkfat's FAT16 images are not recognised as zelr's own) was left deliberately and never revisited.
- `f32a9f92` "Fix resource cleanup and workflow checks" has no body, and no workflow files exist.
- The `release.sh` header says that for fifteen releases neither asset was attached. GitHub now shows assets for every release except v0.14.0 and v0.15.0, so either older assets were backfilled or the claim was loose. Unresolved.
- The v0.37.0 visibility-fallback feature appears only in the release notes, not in any commit message or the README.

### 9.4 Documents that are out of date today

| Document | Stale content |
|---|---|
| `README.md` | "TLS is one session at a time"; "runs 552 checks" (560 now); SDK section "fifty-seven system calls" (63); "No libc" in two places; layout omits `sdk/libc`, `tools/libccheck.py`, `tools/mkroots.py`; "kernel/elf.c elf32 loader" (it is ELF64); "userland/ ... a terminal, paint, settings and three small tests" (dozens of programs); kernel "about 1.5 MB" and exe "162 MB" |
| `NOTICE` | "138 commits" (150); "There is no libc"; its date span (29 Aug to 22 Sep) is local time, while the first commit is 2026-08-28 14:12 UTC |
| `pipeline/README.md` and `pipeline/cycle.sh` | "234 self checks" (560); model names in `models.sh` (`gpt-5.6-sol`, `gpt-5.6-luna`, `sonnet`, `opus`) may be out of date for anyone re-running the pipeline |
| `pipeline/backlog.md` | Five tasks shown as todo or blocked although done by hand (see 5.3); the TLS idea is done |
| `include/syscall.h` | Comment says three signals and no handlers (handlers since `a49f2614`); the TLS comment speaks of "the same one socket" |
| zelr.org | Everything in 8.3 |

### 9.5 Historical release defects, for support questions

| Release | Defect | Fixed in |
|---|---|---|
| v0.6.0 | ISO fails on firmware that loads one sector | v0.6.1 |
| v0.8.0 | `nyx.exe` exits with `DllNotFoundException` before opening | fix `5e7d0904`, shipped in v0.9.0 |
| v0.11.0 | Reports 0.10.0 internally, and triple-faults on VMware | v0.12.0 |
| All releases before v0.12.0 | Never booted on VMware | v0.12.0 |
| v0.12.0 and earlier | Could not take laptop keyboard input (no USB) | v0.13.0 |
| v0.14.0, v0.15.0 | No downloadable assets | never backfilled |
| v0.16.0 and earlier | Keyboard and mouse die if a key is pressed during boot | v0.16.1 |
| v0.32.0 | Disks it formats boot once and never again. Recovery: boot the v0.33.0 or later ISO with the disk attached; the sector is repaired on mount | v0.33.0 |
| v0.33.0, v0.34.0 | Do not boot under UEFI (BIOS works) | v0.35.0 |
