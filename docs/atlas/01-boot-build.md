# 01 -- Boot, build and imaging

Tree: the repository root (github.com/blavese/zelr `main`, 2026-09-22, v0.37.0 + 2 commits).
Method: static reading only. Nothing was built or run, because there is no zig, python, bash or qemu on this machine. `file:L` means the line in that file. "Verified" means I read the code path myself. "Estimated" means hand arithmetic.

---

## 1. Scope

| File | Lines | Role |
|---|---|---|
| `boot/boot.S` | 260 | Kernel front: `.kheader` entry table, multiboot a.out-kludge header, 32→64-bit switch, three entry points (`_start`, `_start32`, `_start64`) |
| `linker.ld` | 40 | Kernel link script. Links at 16 MiB and is the single source of truth for the load address |
| `include/handoff.h` | 59 | `handoff_t`, the one loader→kernel contract, plus `HANDOFF_MAGIC` and the memory kinds |
| `include/multiboot.h` | 27 | Minimal multiboot v1 info and mmap structs, used only by `kmain_multiboot` |
| `bootloader/cdboot.S` | 805 | BIOS loader (disc and stick): stage-1 self-reload, A20, E820, unreal mode, payload copy to 16 MiB, handoff build, jump to `_start32` |
| `bootloader/mbr.S` | 97 | Hybrid-image MBR. Relocates to 0x0600 and loads cdboot from LBA 92 to 0x7C00 |
| `bootloader/trampoline.S` | 111 | SMP AP start code. Copied to 0x8000; goes real mode → long mode with the BSP's CR3 |
| `bootloader/link.ld` | 14 | cdboot link at 0x7C00 (single PT_LOAD, `.bss` folded in) |
| `bootloader/mbr.ld` | 8 | MBR link at 0x7C00 |
| `bootloader/trampoline.ld` | 9 | Trampoline link at 0x8000 |
| `bootloader/build.sh` | 37 | Builds `cdboot.bin`, `mbr.bin`, `trampoline.bin` and runs `check_loader.py` |
| `uefi/efi.h` | 304 | Hand transcription of the UEFI structures zelr uses (not gnu-efi) |
| `uefi/loader.c` | 474 | UEFI loader `BOOTX64.EFI`: GOP, ACPI pointer, reads `zelr.bin`, memory map + ExitBootServices loop, jump to `_start64` |
| `uefi/stub64.c` | 33 | Old stand-in kernel for UEFI bring-up. **Dead: no script builds it, and it no longer works with the loader** |
| `uefi/stub.ld` | 7 | Link script for the stub (1 MiB). Dead |
| `uefi/build.sh` | 24 | Builds `build/BOOTX64.EFI` |
| `build.sh` | 64 | Top-level build: userland, bootloader, uefi, kernel ELF, flat image |
| `userland/build.sh` | 37 | Builds every `userland/*.c` into `build/user/<name>.elf` |
| `run.sh` | 47 | QEMU launcher (bash): window, `-t`, `-T` selftest, `-i` ISO |
| `zelr.bat` | 190 | QEMU launcher (Windows): plain, modern (UEFI), iso, serial, test |
| `.gitignore` | 17 | Ignores `build/`, images, objects, and similar |
| `kernel/builtin.S` | 338 | `.incbin` of `build/trampoline.bin` and 46 user ELFs into `.rodata` |
| `kernel/builtin.c` | 137 | Table of built-in programs, registered into `/bin` |
| `include/builtin.h` | 7 | `builtin_install()`, `builtin_count_programs()` |
| `kernel/userstub.S` | 32 | Position-independent ring-3 stub that prints "ring3" (used by `ring3` and the selftest) |
| `tools/flatten.py` | 82 | ELF (32 or 64) → flat image at a base address. Includes `.bss` as zeros |
| `tools/loadaddr.py` | 43 | Reads the kernel load address from `linker.ld` |
| `tools/check_loader.py` | 133 | Build check: cdboot signature placement, handoff magic halves, load-address agreement |
| `tools/check_sse.py` | 157 | SSE/MMX opcode byte scanner. **Not invoked by any build script** |
| `tools/check_version.py` | 81 | `KERNEL_VERSION` must not be behind the newest git tag (gate only) |
| `tools/whereis.py` | 49 | Address → symbol. **Asserts ELF32, so it cannot read the ELF64 kernel** |
| `tools/mkiso.py` | 399 | Builds the 4-way bootable hybrid ISO `build/zelr.iso` |
| `tools/mkfat.py` | 504 | FAT16/FAT32 image builder (ESP and test volumes) |
| `tools/mkgpt.py` | 180 | GPT test disk: protective MBR, ESP and data partition, optional bad CRCs |
| `tools/readfat.py` | 323 | Independent FAT16 reader/writer for raw images |
| `tools/bootcheck.py` | 207 | Harness: the kernel-formatted disk must be BIOS-bootable (boot stub) and gets repaired |
| `tools/iso_test.sh` | 109 | Boots `zelr.iso` four ways: BIOS disc, BIOS stick, UEFI disc, UEFI stick |
| `tools/gpt_test.sh` | 101 | Boots against `mkgpt.py` images: good GPT, bad header CRC, bad entry CRC, no table |
| `kernel/main.c` (L1–320) | 647 total | Heap sizing helpers, `kmain_multiboot` (multiboot → handoff), `kmain` entry and handoff validation up to `gdt_init` |

---

## 2. Big picture

### What this area is
Everything between "power on" (or "QEMU started") and `kmain(handoff_t *)`. That covers three independent loaders plus one kernel front end. It also covers how the kernel image and the bootable media are produced, and the SMP trampoline, which is built with the bootloaders and embedded in the kernel.

### Main design decisions and their stated reasons

1. **One entry contract: `handoff_t`, not multiboot.** Multiboot "cannot describe a framebuffer the firmware chose, has no room for an ACPI pointer, and is 32-bit" (`include/handoff.h:11-13`; README:422-425). Every path builds a `handoff_t`, and "after that the kernel never asks how it was started" (`handoff.h:9-10`). The layout uses fixed-width C types because "one of them is a PE binary built for a different ABI" (`handoff.h:15-17`). Under the MS ABI `long` is 32 bits, so only `unsigned long long` and `unsigned int` appear.

2. **The kernel is linked and loaded at 16 MiB physical, identity-mapped, and never relocated** (`linker.ld:1-26`). The old address was 1 MiB. Once the kernel passed about 7 MiB, UEFI firmware's ACPI tables and NVS (at 8 MiB on the tested machine) sat inside the kernel's range, and both UEFI paths stopped booting "for two releases" (`linker.ld:7-15`). 16 MiB clears that and is still inside the 64 MiB that `paging.c` maps a page at a time (`linker.ld:17-19`; `KERNEL_LOW_MB` 64, `include/paging.h:33`). The cost is 15 MiB of unused low memory and a higher memory floor: the image passes its 552 selftest checks with 40 MB (`linker.ld:21-26`).

3. **One flat image, `build/zelr.bin`, is what every loader copies.** A BIOS loader needs raw bytes. No multiboot loader accepts a 64-bit ELF, so the multiboot header uses the a.out kludge and points at the flat file (`boot/boot.S:38-48`; `build.sh:52-54`). The UEFI loader simply reads the file. `tools/flatten.py` fills every PT_LOAD up to `p_memsz` (`flatten.py:67-74`), so **`.bss` is in the file as zeros**. No loader zeroes `.bss` and neither does the kernel (see §6).

4. **The entry points are stated inside the image** (`.kheader`, the first 24 bytes): `"ZELRKRN\0"`, `u64 _start64`, `u64 _start32` (`boot.S:21-29`; `linker.ld:28-30`). The first bytes of the image are headers, not code, so a non-multiboot loader "has no way to know where to jump" without this.

5. **The load address is decided in one place, `linker.ld`** (`. = 16M;`). `build.sh` and `mkiso.py` read it through `tools/loadaddr.py`. The two places that cannot read it are checked by `tools/check_loader.py`, which fails the build when they disagree: `KERNEL_PHYS` in `uefi/loader.c` and `patch_load` in `cdboot.S` (`loadaddr.py:1-13`, `check_loader.py:89-127`). This came from a real failure: a seventh copy spelled `0x00100000` survived the move and produced `#UD` at address 3 (`cdboot.S:716-722`; README:1588-1594).

6. **BIOS loader design (`cdboot.S`).**
   - It is one ≤2 KiB file with two stages. Stage 1 lives in the first 512 bytes and re-reads the whole file itself, because some firmware loads one sector despite the catalog asking for four (`cdboot.S:25-32`).
   - It supports both sector sizes, 2048 on a disc and 512 on a stick, and "every step names itself" on screen (`cdboot.S:219-221`).
   - A20 is opened four ways, "each one tested rather than assumed", and it refuses to continue if none works (`cdboot.S:305-315`).
   - Unreal mode is re-entered after every INT 13h. VMware's disk BIOS reloads FS and loses the 4 GiB limit; real-mode #GP is vector 13, the same as INT 13h, so out-of-bounds writes silently "called the disk service" (`cdboot.S:599-615`).
   - It checks that `"ZELR"` arrived at the load address before jumping (`cdboot.S:243-247`).

7. **UEFI loader design (`uefi/loader.c`).**
   - Its own `efi.h` is transcribed from the specification rather than taken from gnu-efi. Uncalled members stay declared as `void *` so the offsets stay right (`efi.h:1-12`).
   - It takes the largest 32-bit RGB/BGR GOP mode no larger than 3840×2160, because the kernel heap must fit two back buffers (`loader.c:108-112`).
   - It prefers the ACPI 2.0 GUID.
   - The kernel address "is not negotiable, but the moment is". It asks for 16 MiB; if refused, it loads anywhere and moves the kernel into place after ExitBootServices (`loader.c:194-209`).
   - It uses the documented get-map/exit loop (`loader.c:322-328`).

8. **Built-in user programs are embedded, not installed.** `.incbin` puts them in `.rodata`, and `/bin` serves them straight from the image. They are "deliberately not copied onto the disk", or rebuilding the kernel would appear to change nothing (`builtin.c:7-9`). `-DZELR_BLOB_STAMP=<cksum>` forces zig's compile cache to notice changed blobs (`build.sh:33-39`).

9. **Images are written from the specifications by project tools.** There is no xorriso, mtools or isohybrid (`mkiso.py:4-6`; `mkfat.py:4-9`; `mkgpt.py:4-8`). Timestamps are fixed (2025-01-01), so the image bytes depend only on the inputs.

10. **SMP trampoline** is a flat binary linked at 0x8000 and copied there at run time. Parameters are found by the signature `"ZELRSMP1"`. The AP uses the BSP's own page tables from its first instruction (`trampoline.S:13-22`).

11. **There is no SSE or FPU in the kernel.** SSE2 is baseline on x86-64, so without `-mno-sse` etc. clang emits xmm moves for integer and struct copies, and those fault with #UD before CR4.OSFXSR is set (`build.sh:5-10`).

---

## 3. File-by-file detail

### 3.1 `include/handoff.h`

- `HANDOFF_MAGIC 0x5A454C5236344846ull` (L19). The comment calls it `"ZELR64HF"`, which is how the hex number reads. In memory (little-endian) the bytes are `46 48 34 36 52 4C 45 5A`.
- `MEM_USABLE 1`, `MEM_RESERVED 2`, `MEM_ACPI 3` (L23-25). `MEM_ACPI` is commented "reclaimable once the tables have been read", but nothing in the kernel ever reclaims it: `pmm_init` frees only `MEM_USABLE` (`pmm.c:89-96`).
- `mem_region_t { u64 base; u64 len; u32 type; u32 pad; }`, 24 bytes (L27-32).
- `HANDOFF_MAX_REGIONS 128` (L34).

**`handoff_t` layout** (natural alignment, no padding holes; 3296 = 0xCE0 bytes, fits in one page):

| Offset | Size | Field | Meaning | cdboot `.set` |
|---|---|---|---|---|
| 0 | 8 | `magic` | `HANDOFF_MAGIC` | `H_MAGIC 0` |
| 8 | 8 | `fb_base` | linear framebuffer physical address, 0 = none | `H_FB_BASE 8` (unused) |
| 16 | 4 | `fb_width` | pixels | -- |
| 20 | 4 | `fb_height` | pixels | -- |
| 24 | 4 | `fb_pitch` | **in pixels**, not bytes | -- |
| 28 | 4 | `fb_bpp` | always 32 from UEFI; never read | -- |
| 32 | 8 | `kernel_base` | where the image was put | `H_KERNEL_BASE 32` |
| 40 | 8 | `kernel_size` | image bytes (the full flat file, `.bss` included) | `H_KERNEL_SIZE 40` |
| 48 | 8 | `rsdp` | ACPI RSDP, 0 = kernel searches low memory | `H_RSDP 48` (unused) |
| 56 | 8 | `region_count` | valid entries in `regions` | `H_REGION_COUNT 56` |
| 64 | 3072 | `regions[128]` | memory map, 24 B each | `H_REGIONS 64`, `REGION_SIZE 24`, `MAX_REGIONS 128` |
| 3136 | 32 | `loader[32]` | "bios" / "uefi" / "multiboot" (printed in the boot log) | `H_LOADER 3136` |
| 3168 | 128 | `cmdline[128]` | only the multiboot path fills it | -- |

**Who writes and who reads each field** (verified by grep):

| Field | cdboot (BIOS) | loader.c (UEFI) | kmain_multiboot | Kernel reads |
|---|---|---|---|---|
| magic | yes | yes | yes | `main.c:310` |
| fb_* | zero | GOP mode | zero | `main.c:126-127` (heap sizing), `:392-393` (`fb_adopt`). `fb_bpp` is never read |
| kernel_base/size | `patch_load`/`patch_bytes` | `KERNEL_PHYS`/file size | **zero** | **never**: the PMM uses `__kernel_start`/`__kernel_end` (`pmm.c:18,103-105`) |
| rsdp | zero | config-table GUID | zero | `main.c:378` `acpi_use_rsdp` |
| regions | E820 (1,2,3) | merged EFI map (1,2,3) | mmap (1,2 only) | `pmm_init`, `paging_init`, `usable_total`/`usable_run_from` |
| loader | "bios" | "uefi" | "multiboot" | `main.c:330,332` |
| cmdline | empty | empty | from mbi | `main.c:313-316` |

### 3.2 `boot/boot.S`

The header comment (L1-19) says "two ways into this kernel", but there are three entries. It covers the multiboot entry and UEFI's `_start64` and was not updated for `_start32`.

- **`.kheader`** (L24-29, `.align 16`): `.ascii "ZELRKRN"`, `.byte 0`, `.quad _start64`, `.quad _start32`. By `linker.ld` it is the first input of `.text`, so it sits at image offset 0 = physical 0x1000000.
- **Multiboot constants** (L31-36): `ALIGN 1<<0`, `MEMINFO 1<<1`, `KLUDGE 1<<16`, so `FLAGS = 0x00010003`. `MAGIC 0x1BADB002`. `CHECKSUM = -(MAGIC+FLAGS) = 0xE4514FFB`.
- **Multiboot header** (L49-59, `.section .multiboot`, `.align 4`; lands at image offset 24): magic, flags, checksum, `header_addr = multiboot_header`, `load_addr = __kernel_start` (16M), `load_end_addr = 0` ("load to the end of the file"), `bss_end_addr = __bss_end`, `entry_addr = _start`. These are 32-bit fields holding 64-bit symbols, which works because everything is below 4 GiB.
- **`.bss`** (L61-69): `pml4` (4 KiB), `pdpt` (4 KiB), `pd` (4×4 KiB), then a 64 KiB stack `stack_bottom..stack_top` (16-aligned). These live inside the kernel image, above 16 MiB.
- **`.rodata` GDT** (L71-85): null, code `0x00209A0000000000` (L=1, P, DPL0), data `0x0000920000000000`. Selectors: code 0x08, data 0x10. `gdt64_ptr` is `.word limit; .quad base`.
- **`_start`** (L89-103), the multiboot entry in 32-bit PM with paging off: `cli`; `esp=stack_top`; `ebp=0`. It stores `eax` in `mb_magic` and `ebx` in `mb_info`, because long mode clears the upper register halves (L96-99). It sets `from_handoff=0` and falls into `go_long`.
- **`_start32`** (L107-115), the BIOS entry from cdboot with `ebx` = handoff pointer: `cli`; stack; `mb_info=ebx`; `from_handoff=1`.
- **`go_long`** (L117-141): `check_long_mode`, then `build_tables`, then CR4 |= PAE (bit 5), CR3 = `pml4`, EFER (MSR 0xC0000080) |= LME (bit 8), CR0 |= PG (bit 31), which "actually enters long mode", then `lgdt gdt64_ptr`, then `ljmp $0x08, $long_start`.
- **`check_long_mode`** (L145-154): CPUID 0x80000000 must report ≥ 0x80000001, and CPUID 0x80000001 EDX bit 29 (LM) must be set. On failure, **`no_long_mode`** (L156-170) writes `"zelr needs a 64-bit processor"` (L260) to VGA text at 0xB8000 with attribute 0x4F (white on red), then `cli; hlt`.
- **`build_tables`** (L174-209):
  - Zeroes 6 pages (`rep stosl`, ecx = 4096*6/4).
  - Sets `pml4[0] = pdpt|3` and `pdpt[0..3] = pd+i*4096 | 3`.
  - Fills 2048 PDEs of 2 MiB with flags `0x83` (P|RW|PS), with `adc` carrying into the high dword.
  - Result: identity map of 0–4 GiB. The comment's reason is to reach a firmware framebuffer near 2 GiB and the LAPIC just under 4 GiB before `paging.c` takes over (L15-18).
- **`long_start`** (L212-241, `.code64`): DS/ES/FS/GS/SS = 0x10; `rsp=stack_top`; `rbp=0`. If `from_handoff==0` it calls `kmain_multiboot(edi=mb_magic, esi=mb_info)`; otherwise it calls `kmain(edi=mb_info)`. Afterwards `cli; hlt` loop. At each call `rsp ≡ 8 (mod 16)` at callee entry, which is SysV-correct.
- **`_start64`** (L245-254), the UEFI entry: already in long mode and paged, `rdi` = handoff. `cli`; `rsp=stack_top`; `rbp=0`; `call kmain`. It does **not** load a GDT or segment registers and keeps the firmware's page tables until `gdt_init` and `paging_init`.
- **`.data`** (L256-260): `mb_magic`, `mb_info`, `from_handoff` (all `.long`), and `msg_no64`.

### 3.3 `linker.ld`

`ENTRY(_start)`; `. = 16M; __kernel_start = .`.

| Output section | Contents |
|---|---|
| `.text : ALIGN(4K)` | `*(.kheader) *(.multiboot) *(.text*)` |
| `.rodata : ALIGN(4K)` | `*(.rodata*)` |
| `.data : ALIGN(4K)` | `*(.data*)` |
| `.bss : ALIGN(4K)` | `__bss_start`, `*(COMMON) *(.bss*)`, `__bss_end` |

Then `__kernel_end = .` (L31-40). There is no `PHDRS` and no `/DISCARD/`.

Constraints from the comment (L28-30):
- `.kheader` must be first.
- `.multiboot` must stay in the first 8 KiB.

Linker symbols are used by:
- `__kernel_start`: `pmm.c:18,103` and the multiboot header.
- `__kernel_end`: `main.c:51,68` (heap base) and `pmm.c:18,66` (bitmap placement).
- `__bss_end`: the multiboot header.
- `__bss_start`: nothing found.

### 3.4 `bootloader/cdboot.S` (BIOS loader, 16/32-bit, linked at 0x7C00)

**Patch block** (L42-48). It follows `jmp stage1; nop` and the signature `"ZELRBOT1"`. `tools/mkiso.py:patch_loader` finds the signature and writes five little-endian u32 values at sig+8:

| Field | Written by mkiso | Used by cdboot |
|---|---|---|
| `patch_lba` | ISO sector of `zelr.bin` | `load_payload` |
| `patch_bytes` | `len(zelr.bin)` | `load_payload`, `zero_handoff` (kernel_size) |
| `patch_entry` | ELF `e_entry` low 32 bits (= `_start`, the **multiboot** entry) | **never read** |
| `patch_load` | `load_address()` (0x1000000). Default in source is `.long 0x1000000`, which `check_loader.py` checks | copy destination, payload check, `.kheader` read |
| `patch_self` | `LOADER_LBA` = 23 | stage-1 self-reload |

The header comment (L22) says "four values"; there are five.

**Constants** (L59-85):
- `HANDOFF_SEG 0x7000` (handoff at linear 0x70000) and the `H_*` offsets (table in §3.1). The comment says they are "include/handoff.h laid out by hand … If that structure changes, these change with it" (L57-58).
- `BUFFER_SEG 0x1000` (0x10000, the 32 KiB read buffer).
- `DRIVE_STASH 0x7000` (a linear byte, unrelated to `HANDOFF_SEG` despite the same number).
- `S1_DAP 0x7010`, `S1_SCRATCH 0x9000`, `S1_SIG 0x31524C5A` (`"ZLR1"`), `CHUNK_SECTORS 16` (16×2048 = 32 KiB).

**Stage 1** (L89-198) must fit below 0x7E00; by hand estimate it ends near offset 0x146, 326 bytes.
1. `cli`; DS=ES=SS=0; SP=0x7C00. Save DL to `s1_drive` and to `[DRIVE_STASH]`, outside the file, because the self-reload overwrites the in-file copy with the disc value 0 (L72-75). `sti`.
2. INT 13h AH=48h into `s1_params` (buffer size 0x1E). Bytes per sector come from offset 24. Values of 0 or above 2048 are ignored (default 2048).
3. Up to 3 attempts with `s1_sizes = {reported, 2048, 512}` (L204-205):
   - sectors = 2048 / size; device LBA = `patch_self` × sectors.
   - Build the DAP at 0x7010 (16 bytes, count, buffer 0000:9000, LBA). INT 13h AH=42h.
   - Accept the read if the dword at `S1_SCRATCH + (s1_signature - _start)` equals `S1_SIG` (L165-166). The signature sits past the first 512 bytes, so finding it proves the whole file arrived.
4. On success, `rep movsw` 1024 words from 0x9000 to 0x7C00. The executing bytes are identical; the modified `s1_*` variables revert to their disc values but are not used again. Reload DL from `DRIVE_STASH` and `jmp real_start`.
5. On failure after 3 tries, print `"zelr: cannot read the boot device\r\n"` via INT 10h teletype and `hlt` loop.

**Stage 2 `real_start`** (L213-257). Each step prints its name first: `msg_boot`, `"a20 "`, `"geometry "`, `"memory "`, `"unreal "`, `"loading "`, then dots, then `"starting kernel"`. The sequence is:
1. `enable_a20` (L415-448). `a20_test` (L320-358) compares 0000:0500 with FFFF:0510 (= 0x100500) and restores both bytes. If the gate is closed, try in order, re-testing after each:
   - INT 15h AX=2401h;
   - port 0x92 (set bit 1, clear bit 0, which would reset);
   - `kbc_a20`: KBC 0xAD disable, 0xD0 read output port, 0xD1 write back with bit 1, 0xAE enable, with bounded waits of 0xFFFF polls (L363-413).
   If all fail, `die("the a20 gate will not open")`.
2. `probe_geometry` (L454-483): AH=48h again. Sets `sector_size` (default 2048), `lba_scale = 2048/sector_size`, `chunk_sectors = 32768/sector_size`. It does **not** reuse what stage 1 proved (see §10).
3. `zero_handoff` (L661-698): byte-zero `H_LOADER+160` = 3296 bytes at 0x70000, then:
   - magic as two dwords, `0x36344846` at +0 and `0x5A454C52` at +4 (the format `check_loader.py` checks);
   - `kernel_base = patch_load`, `kernel_size = patch_bytes` (high dwords 0);
   - `loader = "bios"`.
   `fb` and `rsdp` stay 0 on purpose. The kernel picks a mode itself and finds ACPI by scanning low memory (L657-660).
4. `read_memory_map` (L492-538): E820 loop (`EAX=0xE820`, `EDX='SMAP'`, `ECX=20`) writing directly into `ES:DI = 0x7000:(64 + n*24)`. The first 20 bytes of an E820 entry match `mem_region_t`. Types 1 and 3 are kept and everything else becomes 2; `pad` is zeroed. The loop stops when EBX=0, on CF, on a non-SMAP reply, or at 128 entries. Zero entries means `die("the firmware would not describe memory")`. `region_count` is written as two dwords.
5. `enter_unreal` (L544-560): `cli`; push DS; `lgdt gdt_desc`; CR0.PE=1; FS = 0x10 (4 GiB flat data); CR0.PE=0; pop DS; `sti`. The 4 GiB limit stays in FS's hidden descriptor cache.
6. `load_payload` (L564-648). Initialise `cur_lba=patch_lba`, `cur_dest=patch_load`, `remaining=patch_bytes`. Loop:
   - Sector count is `chunk_sectors`, or for the last chunk `ceil(remaining/sector_size)`, so it never reads past the image (L575-578).
   - DAP at `dap`: buffer 1000:0000, LBA = `cur_lba × lba_scale` (low dword only). INT 13h AH=42h. CF means `die("could not read the boot device")`.
   - `call enter_unreal` after **every** INT 13h (L599-616).
   - Copy `ceil4(min(remaining, 32768))/4` dwords through `%fs:(%esi)` → `%fs:(%edi)`. It may copy up to 3 bytes of buffer garbage past the end of the file, which is harmless.
   - `cur_lba += 16`; `cur_dest += 32768`; `remaining -= 32768`. A borrow means done. Print `"."`.
7. `check_payload` (L264-276): the dword at `%fs:patch_load` must be `0x524C455A` (`"ZELR"`), else `"the kernel did not reach 1 MiB; halted"`. The message text is stale (16 MiB now).
8. Print `"starting kernel"`; `cli`; `lgdt gdt_desc`; CR0.PE=1; `ljmpl $0x08, $protected`.

**`protected`** (L703-731, `.code32`): DS/ES/FS/GS/SS = 0x10; ESP = 0x7C00. `ecx = *(u32 *)(patch_load + 16)` is the `.kheader` `_start32` pointer (low half). `ebx = 0x70000`. `jmp *%ecx`.

**Data** (L735-805):
- `boot_drive`, `mmap_bytes` (**unused leftover**), `sector_size`, `lba_scale`, `chunk_sectors`, `cur_lba`, `cur_dest`, `remaining`.
- `dap` (16 B), `params` (0x1E).
- `gdt`: null, code32 base 0 limit 4G access 0x9A flags 0xCF, data32 0x92 0xCF. `gdt_desc`.
- Strings.
- `s1_signature: .long 0x31524C5A` last in the file (L802-805).

### 3.5 `bootloader/mbr.S` (hybrid MBR, used only for BIOS from a stick)

- `RELOC 0x0600`, `LOADER 0x7C00` (L19-20).
- `cli`; DS=ES=SS=0; SP=0x7C00; `cld`; copy 256 words from 0x7C00 to 0x0600; `ljmp $0, $RELOC+(moved-_start)` (L22-38).
- `moved`: `sti`; save DL; print `"zelr\r\n"`; INT 13h AH=42h with its DAP; restore DL; `ljmp $0, $0x7C00` (L40-56). CF prints `"no bootloader\r\n"` and halts (L58-62).
- The DAP defaults to count 4 (512-byte sectors) and LBA 92, which is ISO sector 23 × 4. mkiso re-patches both (`mkiso.py:254-262`), finding the DAP by the pattern `10 00 ?? ?? 00 7C`, 4-aligned (L83-94).
- There is no AH=41h extensions check.
- Must be ≤ 446 bytes, which mkiso enforces (`mkiso.py:247-248`). The comment says it uses "about a third" (L12-13).

### 3.6 `bootloader/trampoline.S` (AP start code, linked at 0x8000)

- `BASE 0x8000` (L28). The BSP sends SIPI vector `0x8000>>12 = 0x08` (`smp.c:209`), so the AP starts at CS=0x0800, IP=0 in real mode.
- The 16-bit part (L30-64):
  - `cli; cld`; DS=ES=SS=0;
  - CR4 |= PAE;
  - `CR3 = (u32)param_cr3`. This is a **32-bit load**, so the kernel PML4 must be below 4 GiB;
  - EFER.LME;
  - **CR0 |= PG|PE in a single write**. Real mode cannot enable paging on its own; this goes straight to IA-32e compatibility mode (L54-58);
  - `lgdt BASE+(gdt_desc-_start)`. With 16-bit operand size it loads a 24-bit base, which is fine below 16 MiB;
  - `ljmpl $0x08, $(BASE+(long_mode-_start))`.
- `long_mode` (L67-87): segments = 0x10; `rsp = param_stack`; `rbp = 0`; `rdi = param_arg`; `call *param_entry`; `hlt` loop if it returns.
- GDT (L89-100): null, `0x00209A0000000000`, `0x0000920000000000`. `gdt_desc` is `.word` + `.quad`.
- Parameter block (L102-111): `.align 8`, `"ZELRSMP1"`, then `param_cr3`, `param_stack`, `param_entry`, `param_arg` (4 × u64).
- The kernel patches it in `smp.c:152-168` (`patch_trampoline`). It scans the 0x8000 copy in 4-byte steps for the signature (loop bound `i + 8 + 32 <= len`) and writes `paging_kernel_directory()`, the 16-aligned stack top, `(u64)ap_main`, and the CPU index.
- The copy is made by `smp.c:417-418`, `memcpy(TRAMPOLINE_PHYS=0x8000, trampoline_start, len)`, only when more than one CPU is usable (`smp.c:413`).
- "Startup is serialised, so one slot is enough" (L21-22).
- It does not set EFER.NXE or any CR4 bit besides PAE. That is fine today: grep finds no NX use.
- Size is roughly 200 bytes (estimate).

### 3.7 Bootloader link scripts

- `bootloader/link.ld`: `ENTRY(_start)`, `PHDRS { text PT_LOAD FLAGS(7); }`, `. = 0x7C00`, one `.text` containing `.text* .rodata* .data* .bss* COMMON`. It defines `__loader_end`, which nothing uses (grep), and discards `.comment .note* .eh_frame*`. The explicit PHDRS stops lld from giving the ELF headers their own load segment below 0x7C00; that would put 52 bytes of header in front of the code after flattening (L4-6).
- `bootloader/mbr.ld`: same idea, `.bss*` discarded.
- `bootloader/trampoline.ld`: `. = 0x8000`, `.bss*` discarded. "Every address inside it is absolute."

### 3.8 `bootloader/build.sh`

It runs from the repo root and finds zig through `$ZIG`, then `command -v zig`, then a hard-coded WinGet path `/c/Users/admin/AppData/Local/Microsoft/WinGet/Packages` (L7-11).

1. cdboot: `zig cc -target x86-freestanding-none -mcpu=i686 -ffreestanding -nostdlib -static -c`, then `zig ld.lld -T bootloader/link.ld`, then `python tools/flatten.py build/cdboot.elf build/cdboot.bin 0x7C00`, then `python tools/check_loader.py build/cdboot.bin` (L17-25).
2. mbr: same flags, `mbr.ld`, flattened at 0x7C00 (L27-30).
3. trampoline: `-target x86_64-freestanding-none` (it contains `.code64`), `trampoline.ld`, flattened at 0x8000 (L34-37).

`set -e`, so any failure aborts `build.sh`.

### 3.9 `uefi/efi.h`

Base types: `UINTN`/`EFI_STATUS` are u64, `CHAR16` is u16. `EFIAPI = __attribute__((ms_abi))`. Status constants: SUCCESS, LOAD_ERROR, INVALID_PARAMETER, UNSUPPORTED, BUFFER_TOO_SMALL, NOT_FOUND. `EFI_ERROR(s)` tests bit 63.

Structs, each checked against the UEFI specification order:
- `EFI_GUID`, `EFI_TABLE_HEADER`.
- `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`: reset, output_string, test_string, query_mode, set_mode, set_attribute, clear_screen, set_cursor_position, enable_cursor, mode.
- `EFI_SIMPLE_TEXT_INPUT_PROTOCOL`, `EFI_INPUT_KEY`.
- `EFI_ALLOCATE_TYPE`; memory type defines `EfiReservedMemoryType` 0 … `EfiPersistentMemory` 14.
- `EFI_MEMORY_DESCRIPTOR`: type, pad, physical_start, virtual_start, pages, attribute.
- GOP: `EFI_GOP_GUID`, the pixel format enum, `EFI_GRAPHICS_OUTPUT_MODE_INFORMATION`, `…_PROTOCOL_MODE`, `EFI_GRAPHICS_OUTPUT_PROTOCOL` (query_mode, set_mode, blt, mode).
- `EFI_LOADED_IMAGE_PROTOCOL` with its GUID.
- `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` and `EFI_FILE_PROTOCOL` (revision, open, close, delete, read, write, get_position, set_position, get_info, set_info, flush); `EFI_FILE_MODE_READ`.
- `EFI_BOOT_SERVICES` in full specification order. Typed members: allocate_pages, free_pages, get_memory_map, allocate_pool, free_pool, handle_protocol, exit_boot_services, stall, set_watchdog_timer, open_protocol, locate_handle_buffer, locate_protocol.
- `EFI_CONFIGURATION_TABLE`, `EFI_SYSTEM_TABLE`.
- `EFI_ACPI_20_TABLE_GUID`, `EFI_ACPI_10_TABLE_GUID`; `guid_eq()`.

I checked every layout against the specification's member order and found no offset errors.

### 3.10 `uefi/loader.c`

Globals: `ST`, `BS` (L22-23). Constants: `KERNEL_PHYS 0x1000000ull` (L388, checked by `check_loader.py`) and `HANDOFF_PHYS 0x70000ull` (L393).

Helpers:
- `print`, `print_hex` (always 16 digits), `print_dec`.
- `die(why, status)` prints `"\r\nzelr: <why> (<hex>)"` and `hlt` loops (L55-62).
- `mem_set`, `str_copy`.

**`setup_graphics(h)`** (L83-144):
1. `locate_protocol(GOP)`. If absent, print a message and return with `fb_base=0`.
2. Walk modes `0..max_mode-1` with `query_mode` and skip formats other than `PixelRedGreenBlueReserved8BitPerColor` / `PixelBlueGreenRedReserved8BitPerColor`.
3. Track the smallest 32-bit mode. Choose the largest by `w*h` with `w ≤ 3840` and `h ≤ 2160`; the first mode wins ties.
4. If nothing fits the cap, choose the smallest 32-bit mode rather than the current one, because the current mode is "usually the panel's own resolution … the one that was just rejected" (L117-124).
5. `set_mode` only if the choice differs from the current mode. On failure, print and keep the current mode.
6. Fill `fb_base = mode->frame_buffer_base`, `fb_width/height`, `fb_pitch = pixels_per_scan_line`, `fb_bpp = 32`.

The **pixel format itself is not passed** (see §10).

**`find_acpi(h)`** (L151-164): scan the configuration table. An ACPI 2.0 GUID match returns immediately. An ACPI 1.0 match is kept only if nothing is set yet.

**`open_boot_file(image, name)`** (L170-192): `handle_protocol(LoadedImage)`, then the `device_handle` → SimpleFileSystem, then `open_volume` → root → `root->open(name, READ)`, then close the root. This opens relative to the **volume root**. The comment says "same directory this loader was loaded from", which is inaccurate.

**`load_kernel(image, load_at, &size, &moved)`** (L210-250):
1. Open `u"zelr.bin"`.
2. Size by `set_position(0xFFFF…FF)` (end of file), `get_position`, then rewind.
3. `pages = ceil(size/4096)`. Try `allocate_pages(AllocateAddress, EfiLoaderData, pages, &0x1000000)`. If that fails, use `AllocateAnyPages` and set `moved=1`.
4. One `read` of the whole file, checking for a short read. Print size and address.

**`usable_after_exit(type)`** (L254-261) is true for `EfiConventionalMemory`, `EfiBootServicesCode`, `EfiBootServicesData`, `EfiLoaderCode`. `EfiLoaderData` is deliberately **not** usable: it holds the handoff page and the kernel.

**`range_is_ours_after_exit(base, len)`** (L270-320) runs before exit only in the moved case:
- It takes its own map into a 32 KiB static `probe_buffer` and walks forward from `base`. Every byte must be covered by descriptors that are `usable_after_exit` or `EfiLoaderData`.
- A gap in the map counts as not ours.
- An offending descriptor is named on screen: "acpi tables", "acpi nvs", "firmware code", "firmware data", "reserved" or "something", with its base address.

**`take_the_machine(image, h)`** (L329-378): up to 8 attempts of:
1. `get_memory_map` into a 32 KiB static `map_buffer`. An error means die.
2. Rebuild `h->regions`:
   - skip `pages==0`;
   - kind: usable per `usable_after_exit`, `EfiACPIReclaimMemory` becomes `MEM_ACPI`, everything else `MEM_RESERVED`;
   - merge with the previous entry when the kind matches and it is contiguous;
   - silently stop at 128 entries.
3. `exit_boot_services(image, key)`. Success returns. Nothing else is called between getting the map and exiting.

After 8 failures: `die("the firmware would not hand over the machine")`.

**`EfiMain`** (L395-474):
1. `clear_screen`; print `"zelr uefi loader"`; `set_watchdog_timer(0,0,0,0)`.
2. `allocate_pages(AllocateAddress, EfiLoaderData, 1, &0x70000)`. Failure is fatal (`"cannot reserve the handoff page"`).
3. Zero the handoff; set magic; `loader = "uefi"`.
4. `find_acpi` (print it); `setup_graphics`; `load_kernel(KERNEL_PHYS)`.
5. In the moved case, require `range_is_ours_after_exit(KERNEL_PHYS, size)`, else die.
6. `kernel_base = KERNEL_PHYS`, `kernel_size = size`. Print `"leaving the firmware"`. `take_the_machine`.
7. If moved, byte-wise memmove from the staging copy to 0x1000000. The direction is chosen by overlap (L444-450).
8. Check the first 8 bytes equal `"ZELRKRN\0"`; on mismatch, silent `hlt` (nothing can print now).
9. `entry = *(u64 *)(base + 8)` (`_start64`). Call it through a `sysv_abi` function-pointer cast with `h`, because the loader is MS ABI and the kernel is SysV (L466-471).

### 3.11 `uefi/build.sh`
`zig cc -target x86_64-uefi -ffreestanding -nostdlib -fshort-wchar -fno-sanitize=undefined -fno-stack-protector -fno-stack-check -mno-red-zone -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -o build/BOOTX64.EFI uefi/loader.c`

It produces a PE32+ EFI application. The entry is `EfiMain` (zig/lld-link default for the uefi target). There is no `-mno-sse`; firmware runs with SSE enabled. `-fshort-wchar` has no effect because the code uses `u""` (char16_t) literals.

### 3.12 `uefi/stub64.c`, `uefi/stub.ld` (dead)
`kentry(handoff_t *h)` checks the magic, paints a gradient that exposes a wrong pitch, and draws a white bar whose length is 12 px per usable region. `stub.ld` links it at 0x100000 with `.entry` first.

No script references either file (grep). They also cannot work with the current loader, which needs the `"ZELRKRN"` header, the entry at +8, and a load at 16 MiB.

### 3.13 `kernel/main.c` L1–320 (entry side)

- `extern u8 __kernel_end[]` (L51).
- `BITMAP_ROOM 2 MiB` (L65). `heap_base() = roundup_1MiB(__kernel_end + 2 MiB)` (L67-70). The PMM bitmap sits at page-aligned `__kernel_end` and must fit below `heap_base` (`pmm.c:66-79`).
- `HEAP_MIN 24 MiB` (L81), `HEAP_SHARE 4` (L92), `HEAP_CEIL 512 MiB` (L93).
- `usable_total` (L97-102); `usable_run_from` (L110-123), the contiguous usable run from an address, iterated to a fixpoint.
- `heap_size_for(h, base)` (L125-152):
  - need = 24 MiB + 2 × (`fb_pitch*4*fb_height`) rounded up to 1 MiB;
  - want = max(need, usable/4), capped at 512 MiB;
  - capped at (run − 1 MiB);
  - if it crosses `KERNEL_LOW_MB` (64 MiB), truncated to a 2 MiB boundary.
  - Usage belongs to the kmain agent's area.
- `machine_exit(code)` (L158-161): `outl(0xF4, code)`, the QEMU isa-debug-exit device, whose exit status is `(code<<1)|1`.
- `pci_vendor_name`, `net_survey` (L175-215) and `banner()` (L217-226) belong to the other agent. The banner prints `KERNEL_NAME KERNEL_VERSION` and "x86-64 long mode", which `iso_test.sh` greps.
- `selftest_task` → `machine_exit(failures ? 2 : 0)` (L228-231); `init_task` → `layout_init`, `vfs_chdir(layout_home())`, `shell_task` (L233-237).
- **`static handoff_t multiboot_handoff`** (L246) lives in `.bss` because boot.S's stack "is not very large".
- **`kmain_multiboot(u32 magic, u32 mbi_addr)`** (L250-299):
  1. Zero the static handoff; set magic; `loader = "multiboot"`.
  2. If `magic != 0x2BADB002`: `serial_init(); panic("not booted by a multiboot loader (magic=%x)")`.
  3. mbi flags bit 2 means copy `cmdline` (strncpy, 127 max).
  4. Bit 6 means walk the mmap (`p += e->size + 4`). Type 1 becomes `MEM_USABLE`, everything else `MEM_RESERVED` (**ACPI-reclaimable becomes reserved**, unlike the BIOS path). Capped at 128.
  5. No regions: fallback `{base 0x100000, len (mem_upper+1024)*1024, USABLE}`. This is off by 1 MiB (§10), and flags bit 0 is not checked.
  6. `fb_base = 0`, then `kmain(h)`.

  `kernel_base`, `kernel_size` and `rsdp` stay 0.
- **`kmain(handoff_t *h)`** (L301+):
  1. `serial_init()`, then `bb_init()`, `bb_mark("serial, vga")`, `vga_init()`. None of these touch `h`.
  2. `if (!h || h->magic != HANDOFF_MAGIC) panic("started without a handoff structure")` (L310-311).
  3. Scan `cmdline` by **substring** at every offset: `"selftest"` sets `want_selftest`, `"console"` calls `shell_console_only()` (L313-316).
  4. `banner()`, then subsystem init begins with `gdt_init` (L320-321).

  Handoff consumption points after that: `pmm_init(h, hbase)` L328, `h->loader` L330/332, `heap_size_for` L335, `paging_init(h)` L346, `acpi_use_rsdp(h->rsdp)` L378, `fb_adopt(fb_base, w, h, pitch)` or `fb_init(1024,768)` L392-394. `h` is not kept after that.

### 3.14 `include/multiboot.h`
`MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002`. `mb_mmap_entry_t` is packed `{u32 size; u64 addr; u64 len; u32 type;}`. `multiboot_info_t` is packed and runs from `flags` through `vbe_interface_len`, with no framebuffer fields. The only user is `main.c`.

### 3.15 `kernel/builtin.S`, `kernel/builtin.c`, `include/builtin.h`

- `builtin.S`:
  - `.section .rodata`; `.balign 4096`; `trampoline_start: .incbin "build/trampoline.bin"; trampoline_end:` (L11-17).
  - 46 program stanzas, each `.balign 4`, two `.global`s, `builtin_<n>_start: .incbin "build/user/<n>.elf"; builtin_<n>_end:`. `wiretest` (L237-241) and `jsprobe` (L243-247) lack the `.balign 4`; this is harmless.
  - Paths are relative to the repo root, where `build.sh` runs the compiler (L9-10).
  - Programs in file order: hello, count, fptest, alloctest, jstest, forktest, wintest, term, spawntest, settings, paint, files, calc, music, monitor, notes, browser, sh, echo, cat, ls, wc, grep, fdtest, pagetest, spin, ps, pngtest, jpegtest, svgtest, layouttest, wiretest, jsprobe, blackjack, poker, cardtest, sleeptest, halfdrawn, cowtest, argvtest, sigtest, faulttest, maptest, crashwrite, durtest, polltest.
  - They match the 46 `userland/*.c` files one to one.
- `builtin.c`:
  - Externs (L15-60) and `program_t {name, start, end}` (L62-65).
  - `PROGRAMS[]` has 46 entries (L70-117). Names carry no extension ("paint", not "paint.elf").
  - `N_PROGRAMS` (L119). `_Static_assert(N_PROGRAMS <= SYSFS_MAX_PROGRAMS, …)` (L126-128), with `SYSFS_MAX_PROGRAMS 48` in `include/sysfs.h:30`. That leaves **2 free slots**. The assert exists because a 17th program, the browser, was once silently dropped (L121-125).
  - `builtin_count_programs()` (L130).
  - `builtin_install()` (L132-137) calls `vfs_add_builtin(name, start, size)` → `sysfs_add_program` (`vfs.c:26-28`) → a pointer and size stored in `programs[48]` (`sysfs.c:44-59`). There is no copy, and the function silently returns if full.
- Callers: `main.c:444-445` (after the disk mount; prints `"progs %d built in"`); `selftest.c:879,887`. `trampoline_start/_end` are used by `smp.c:37,154,417-418`.

### 3.16 `kernel/userstub.S`
`.rodata` blob `user_stub_start..user_stub_end`. It makes six `int $0x80` calls with `eax=1` (SYS_PUTC) and `ebx` = 'r','i','n','g','3','\n', then `eax=0, ebx=0` (SYS_EXIT) and `jmp .`. It is position-independent. `kernel/user.c:351-366` (`user_spawn_stub`) copies it with `load_flat` to `USER_CODE_BASE`. It is used by the shell command `ring3` (`shell.c:334-337`) and by selftest `[userspace]` (`selftest.c:634-646`, which expects ≥ 7 syscalls). The syscall numbers match `include/syscall.h:4-5` and `sdk/zelr.h:58-59`.

### 3.17 `build.sh` (top level)

1. `set -e`; `cd` to the repo; find zig (same fallback path as above) or exit with `"zig not found; set ZIG=/path/to/zig"`. `filesize()` handles GNU and BSD `stat`.
2. `mkdir -p build`, then `bash userland/build.sh`, `bash bootloader/build.sh`, `bash uefi/build.sh`. User programs and the trampoline go first because they are `.incbin`'d (L25-29).
3. `SRC = find boot kernel -name '*.c' -o -name '*.S' | sort`. That is `boot/boot.S`, 87 `kernel/*.c`, and `kernel/{builtin,gdt_flush,idt_flush,isr,userstub}.S`.
4. `BLOBS = cat build/user/*.elf build/trampoline.bin | cksum | cut -d' ' -f1` (L39).
5. One `zig cc` invocation compiles and links everything to `build/zelr.elf` (flags in §7.2).
6. `LOAD_AT=$(python tools/loadaddr.py)`, then `python tools/flatten.py build/zelr.elf build/zelr.bin "$LOAD_AT"` (L60-61). Print the sizes.

It requires bash, `python` (that exact name), find, sort, tr, cat, cksum, cut and stat. The README lists only "QEMU and Zig" (README:355).

### 3.18 `userland/build.sh`
It runs in `userland/`. For every `*.c`:

`zig cc -target x86_64-freestanding-none -I../sdk -ffreestanding -nostdlib -static -O2 -std=gnu11 -fno-sanitize=undefined -fno-stack-protector -fno-stack-check -fno-builtin -fno-pic -fno-pie -mcmodel=large -mno-red-zone -Wall -Wextra -Wl,-T,../sdk/zelr.ld -Wl,--build-id=none -o ../build/user/<name>.elf`

`sdk/zelr.ld` links at `0x8040000000`, which is why `-mcmodel=large` is needed. SSE is **allowed** in userland. It builds every `.c`, whether or not `builtin.S` lists it, and never deletes stale ELFs.

### 3.19 `run.sh`, `zelr.bat`, `.gitignore`

- `run.sh`:
  - Finds QEMU (`$QEMU`, PATH, `/c/Program Files/qemu/…`). Always runs `bash build.sh` first.
  - Disk `$ZELR_DISK` or `zelr.img`, created at 16 MiB. It migrates an old `nyx.img`.
  - `COMMON`: `-kernel build/zelr.bin -m 64 -no-reboot`, IDE disk, user-net rtl8139.
  - Modes:
    - `-i`: `mkiso.py`, then `-cdrom build/zelr.iso -boot d -m 64` (BIOS disc).
    - `-T`: `-append selftest -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04`. Exit status 1 is mapped to 0.
    - `-t`: headless.
    - default: window.
- `zelr.bat`:
  - Needs a prebuilt `build\zelr.bin`. `ACCEL=accel=whpx:tcg`. Creates `zelr.img` at 64 MiB (fsutil, falling back to PowerShell) and migrates `nyx.img`.
  - Modes:
    - **plain**: q35, `-smp 2 -m 512`, AHCI, e1000, `-kernel` (multiboot).
    - **modern**: q35, `-smp 4 -m 1024`, pflash `edk2-x86_64-code.fd`, `-cdrom build\zelr.iso -boot d` (UEFI disc), NVMe, e1000. Falls back to plain if the firmware is missing.
    - **iso**: q35 SeaBIOS, `-cdrom` (BIOS disc).
    - **serial**: headless `-kernel`.
    - **test**: `-kernel … -append selftest` with isa-debug-exit. errorlevel ≥ 2 means failure.
- `.gitignore`: `build/`, `*.o`, `*.elf`, `*.ppm`, `*.img`, `*.img.gz`, `launcher/{bin,obj,publish}/`, `pipeline/state/`, `__pycache__/`, `*.pyc`.

### 3.20 `tools/flatten.py`, `tools/loadaddr.py`

- `flatten.py IN.elf OUT.bin [BASE]`:
  - `load_segments` parses ELF32 or ELF64 program headers (the ELF64 field order differs) and keeps PT_LOAD with `memsz>0` as `(p_paddr, p_memsz, file bytes)`.
  - `main`: `base` = argument or the lowest paddr; `end = max(paddr+memsz)`; errors if a segment is below `base`; writes a zero `bytearray(end-base)` with the segments copied in; prints `base/entry/size`.
  - Error messages: "is not an ELF file", "has an ELF class of", "has nothing to load", "a segment sits below the base address".
- `loadaddr.py`: `load_address()` applies `re.search(r"^\s*\.\s*=\s*(\d+)([KMG]?)\s*;", linker.ld, re.M)` and scales by K/M/G (L26-34). Run as a script it prints `0x1000000`. **Only decimal plus an optional suffix is understood.** Writing `. = 0x1000000;` would fail with "linker.ld does not say where the kernel is linked".

### 3.21 `tools/check_loader.py`
`SIGNATURE b"ZLR1"`, `FIRST_SECTOR 512`, `STAGE1_READS 2048` (L19-21). Exit code = `main(path) or check_handoff(root) or check_load_address(root)` (L130-133).

1. `main`: `data.find("ZLR1", 512)` must exist. A copy only below 512 gets a specific message. It must end within 2048 bytes (L24-46).
2. `check_handoff`: `HANDOFF_MAGIC` from `handoff.h` must equal (hi<<32 | lo), where lo and hi are parsed from the two `movl $0x…, %es:H_MAGIC` lines in cdboot, anchored at end of line (L49-86). This was added after the magic went wrong during the nyx→zelr rename.
3. `check_load_address`: `KERNEL_PHYS` in `loader.c` and `^patch_load: .long 0x…` in cdboot must both equal `load_address()` (L89-127).

It does **not** check the `H_*` offsets or the `zero_handoff` size against `handoff.h`, that stage 1 fits in 512 bytes, the MBR, or the trampoline.

### 3.22 `tools/check_sse.py` (not wired in)
It scans every `SHT_PROGBITS` section with `SHF_EXECINSTR` (ELF32 or ELF64) for:
- `66 0F {6E,7E,D6,6F,7F,28,29}`: always reported;
- bare `0F {10,11,28,29,58,59,6E,7E}`: filtered when the bytes look like part of an operand. The filters are an absolute address after `68/B8-BF/A1/A3`, a RIP-relative disp32 (`modrm & 0xC7 == 05`) landing inside the image, an `E8/E9` rel32 landing inside the section, or any unaligned u32 inside the image.

It prints up to 10 hits and exits 1 if any remain. The heuristics come from the 32-bit era; the docstring example `push $0x00110f00` is a 1 MiB-era address. **No script invokes it** (grep over the whole tree, including `pipeline/`), yet README:1431 says it "fails the build".

### 3.23 `tools/check_version.py`
`kernel_version()` applies the regex `#define\s+KERNEL_VERSION\s+"([^"]+)"` to `include/types.h`. Today that is **"0.37.0"** (`types.h:25`). `newest_tag()` runs `git tag --sort=-v:refname` and takes the first `v\d+(\.\d+)*`.

Outcomes:
- fail (1) if `parts(version) < parts(tag)`;
- pass if equal or ahead;
- pass if there are no tags or no git, which is the case in this copy.

The docstring warns it reads only local tags (a release cut via `gh release create` needs `git fetch --tags`). `parts()` would raise on a non-numeric version such as "0.38.0-rc1". It runs only as the gate's `vercheck` step (`pipeline/gate.sh:256-257`), in every mode.

### 3.24 `tools/whereis.py` (broken for this kernel)
`assert d[4] == 1, "expected 32-bit ELF"` (L8), and all parsing uses ELF32 layouts. `build/zelr.elf` is ELF64, so the tool fails immediately.

### 3.25 `tools/mkiso.py` -- the hybrid image

Constants: `SECTOR 2048`, `PVD_LBA 16`, `BRVD_LBA 17`, `TERM_LBA 18`, `CATALOG_LBA 19`, `PATH_L_LBA 20`, `PATH_M_LBA 21`, `ROOT_DIR_LBA 22`, `LOADER_LBA 23` (L44-53).

`main()` (L298-395):
1. Unless `ZELR_PREBUILT=1`, run `bash build.sh` then `bash bootloader/build.sh`. The second run is redundant because build.sh already runs it. The gate sets `ZELR_PREBUILT=1` so parallel harnesses do not rebuild (L301-310).
2. Re-flatten `zelr.elf` to `zelr.bin` at `load_address()`. Read `cdboot.bin`, `zelr.bin`, and `entry = e_entry` (low 32 bits, L293-295).
3. ESP size in KiB: `want = ceil((len(zelr.bin)+len(BOOTX64.EFI))/1024) + 1024`, then `esp_kb = max(4096, roundup(want, 1024))`. Run `mkfat.py build/esp.img esp_kb BOOTX64.EFI:EFI/BOOT/BOOTX64.EFI zelr.bin:zelr.bin` (FAT16) (L325-340).
4. `esp_lba = 23 + sectors(loader)` (= 24); `payload_lba = esp_lba + sectors(esp)`; `total = roundup16(payload_lba + sectors(payload))` (L342-351).
5. `patch_loader` (L221-236): five u32 values after `"ZELRBOT1"`: `payload_lba, len(payload), entry, load_at, 23`.
6. Write the structures below, then the MBR over bytes 0-511.

**Image layout** (2048-byte sectors):

| Sector | Content |
|---|---|
| 0 (bytes 0–511) | `hybrid_mbr`: mbr.bin code (≤446 B, DAP patched to count = loader_sectors×4, LBA = 92). Partition 1: 0x80 active, CHS 00 02 00, **type 0x17**, LBA 0, size = whole image (total×4). Partition 2: 0x00, CHS FE FF FF, **type 0xEF**, LBA esp_lba×4, size esp_sectors×4. `55 AA`. The partitions overlap. |
| 1–15 | zero (rest of system area) |
| 16 | PVD: type 1 `CD001` v1; volume id `"ZELR"`; space size = total; set/seq 1; block 2048; path table size 10 at L20/M21; root record (LBA 22, 2048 B); publisher `"zelr"`; application `"tools/mkiso.py"`; dates `2025010100000000\0`; expiry all `'0'`; file structure version 1 |
| 17 | Boot record VD: `EL TORITO SPECIFICATION`, catalog LBA 19 at byte 71 |
| 18 | Terminator (type 255) |
| 19 | Boot catalog (see below) |
| 20 / 21 | Path tables L / M: one root record |
| 22 | Root directory: `.`, `..`, `ZELR.BIN;1` → payload (dated 2025-01-01) |
| 23 | `cdboot.bin` (patched, ≤ 2048 B) |
| 24 … | ESP: FAT16, `\EFI\BOOT\BOOTX64.EFI` and `\ZELR.BIN` |
| payload_lba … | `zelr.bin`, the raw payload cdboot loads. The kernel is stored **twice**: here and inside the ESP. |
| … total | zero padding to a multiple of 16 sectors, so a read that overshoots slightly stays inside the image |

**Boot catalog** (`boot_catalog`, L145-187):
- Validation entry: id 1, platform 0 (x86), `"zelr"`, word checksum so the 16 words sum to 0, `55 AA`.
- Default entry: `0x88` bootable, no emulation, load segment 0 (0x7C0), sector count `sectors(loader)×4` = 4 virtual 512 B sectors, RBA 23.
- Section header: `0x91` (final), platform `0xEF`, 1 entry, id `"efi"`.
- EFI entry: `0x88`, no emulation, count `(esp_sectors×4) & 0xFFFF`, RBA `esp_lba`.

Worked example (illustrative): with `zelr.bin` = 10 MiB and the EFI binary about 20 KiB, `esp_kb` = 12288, the ESP is 6144 sectors at 24, the payload is at 6168, and the total is 11296 sectors (about 22 MiB).

### 3.26 `tools/mkfat.py`

**`Fat16Builder(size_kb, label="ZELR")`** (L24-241):
- Geometry:
  - spc = the smallest of 1,2,4,…,64 with `total/spc` in [4200, 65000], else 4;
  - reserved 4, 2 FATs, 512 root entries (32 sectors);
  - FAT size solved iteratively;
  - enforces 4085 ≤ clusters ≤ 65524 (SystemExit otherwise).
- FAT[0]=0xFFF8, FAT[1]=0xFFFF.
- Sequential allocation (`alloc_chain`, L93-103), never frees.
- Names: `to_83` uppercases and truncates to 8.3 with no LFN, no `~1`, and no collision detection (L122-129).
- `dir_record` (L131-140): attribute 0x10 or 0x20, time 0, date `0x5A21` (2025-01-01), cluster, size.
- Directories are one cluster ("directory … is full" otherwise). The root holds at most 512 entries.
- Boot sector (L194-219):
  - `EB 3C 90`, OEM `"ZELR    "`, 512 B/sector, spc, reserved 4, 2 FATs, 512 root entries;
  - total16/total32, media F8, FAT size, 32 sectors/track, 8 heads, 0 hidden;
  - drive 0x80, signature 0x29, **serial 0x5A4C5202**, label, `"FAT16   "`, `55AA`;
  - **no boot code**.
- `add_file` returns `None`.

**`Fat32Builder(size_kb, label="ZELR32")`** (L244-461):
- spc 1, reserved 32, 2 FATs; root cluster = the first allocation (2).
- Needs > 65524 clusters (about ≥ 33 MiB).
- Directory entries split the cluster number into high (offset 20) and low (offset 26) halves.
- `..` naming the root is written as 0 (per the specification).
- `skip_to_cluster(n)` exists for the high-cluster test.
- Boot sector (L406-434): `EB 58 90`, OEM `"ZELR    "`, root sectors 0, total16 0, FAT16-size 0, total32, FAT32 size at 36, flags 0, version 0, root cluster, FSInfo 1, backup 6, drive 0x80 at 64, signature 0x29 at 66, **serial 0x5A4C5200** at 67, label at 71, `"FAT32   "` at 82.
- FSInfo (L436-443): 0x41615252, 0x61417272 at 484, free count, next free, and `55 AA` at 510.
- `build` writes the boot sector at 0 and 6, FSInfo at 1 (the FSInfo backup at 7 is not written), and FAT entries masked to 28 bits.

**CLI** (L464-500): `mkfat.py [--fat32] [--at-cluster=N] OUT SIZE_KB FILE[:PATH]…`. The argument is split on the **last** colon so Windows drive letters work. If the source does not exist, the whole spec is treated as a path. `--at-cluster` applies before the first file only, and **only works with `--fat32`** (Fat16Builder has no `skip_to_cluster`).

Side effect in the kernel: FAT32 images carry OEM `"ZELR    "` and serial `0x5A4C5200`, which equals `ZELR_VOLUME_ID` (`fat.c:438`). `fat_boot_is_ours()` (`fat.c:463-468`) therefore treats them as kernel-formatted volumes, and `fat_boot_repair()` (run on every mount, `diskfs.c:110`) will write its boot stub into them. FAT16 images use serial `…02` and are not "ours".

Used by `mkiso.py`, `mkgpt.py` (import), `fat32_test.sh`, `mountcheck.py`, `appcheck.py`, `libccheck.py` and `sdkcheck.py`.

### 3.27 `tools/mkgpt.py` (test disk, not a boot image)
`SECTOR 512`, `ENTRIES 128`, `ENTRY_SIZE 128`. GUIDs: ESP `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`, basic data `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7`, disk `DEADBEEF-CAFE-1234-5678-9ABCDEF01234`. The `guid()` byte order is little-endian for the first three groups (L37-43).

`build(path, total_kb=65536, bad_header_crc, bad_entry_crc)` (L95-164):
- LBA 0: protective MBR (type 0xEE, start LBA 1, size `min(total-1, 0xFFFFFFFF)`).
- LBA 1: header (rev 0x00010000, size 92, my/alt LBA, first usable 34, last usable total-34, entries at 2, 128×128, CRC32 of the entries, header CRC32).
- LBA 2-33: entries. The backup entries are at total-33 and the backup header at total-1.
- Partition 1 is the ESP: LBA 34 plus 16 MiB, name "EFI System Partition", FAT16 label `"ESP"`, file `EFI/BOOT/PLACEHOLD.TXT` (8.3-truncated to `PLACEHOL.TXT`).
- Partition 2 is "zelr data", running to last usable, FAT16 label `"ZELRDATA"`, file `HELLO.TXT` = `"read from a gpt partition\n"`.
- `--bad-header-crc` stores 0xDEADBEEF as the **primary** header CRC; the backup stays valid. `--bad-entry-crc` inverts the entry CRC in **both** headers.

### 3.28 `tools/readfat.py`
It reads FAT16 volumes that start at byte 0 (superfloppy); partitions are not supported. It rejects the volume unless the cluster count is 4085–65524 and sectors are 512 bytes.
- `entries()` decodes LFN with checksum validation and completeness.
- `walk()` is guarded to depth 16 and skips names beginning with ".".
- `read_file(path)` is case-insensitive.
- `put_file(name, data)` writes an 8.3 name, root only, to both FAT copies. A same-name entry is replaced in place **without freeing its old chain**.

CLI: list the volume (boot parameters, whether the FAT copies agree, chain status per file), print one file, or `--put SRC [AS]`. Used by `namecheck.py` and `mountcheck.py`.

### 3.29 Tests: `iso_test.sh`, `gpt_test.sh`, `bootcheck.py`
See §8.

---

## 4. Control flow and lifecycles

### 4.1 Build pipeline (`bash build.sh`)
```
userland/build.sh      -> build/user/<46 names>.elf           (ELF64 @0x8040000000)
bootloader/build.sh    -> build/cdboot.{o,elf,bin}  -> check_loader.py
                          build/mbr.{o,elf,bin}
                          build/trampoline.{o,elf,bin}
uefi/build.sh          -> build/BOOTX64.EFI                   (PE32+)
BLOBS = cksum(build/user/*.elf + build/trampoline.bin)
zig cc ... -DZELR_BLOB_STAMP=$BLOBS boot/boot.S kernel/*.c kernel/*.S -> build/zelr.elf
loadaddr.py (0x1000000) ; flatten.py -> build/zelr.bin        (16 MiB .. __kernel_end, .bss zero-filled)
```
`python tools/mkiso.py` then adds `build/esp.img` and `build/zelr.iso` and re-flattens `zelr.bin`. Other artifacts in the tree:
- `zelr.img` at the repo root (run.sh 16 MiB, zelr.bat 64 MiB);
- `build/isotest.img` (temporary);
- `build/bootcheck.ppm`, `bootcheck.<pid>.img`, `gpttest.<pid>.img`;
- `build/launcher/zelr.exe` (pipeline release). The launcher embeds `build/zelr.bin` (`launcher/ZelrLauncher.csproj:33`).

### 4.2 The five ways in

| Path | Firmware finds | Loader chain | CPU mode at kernel | Kernel entry | Handoff built by |
|---|---|---|---|---|---|
| BIOS, disc | El Torito default entry (RBA 23, 4×512) | cdboot | 32-bit PM, paging off | `_start32` (ebx=0x70000) | cdboot at 0x70000 |
| BIOS, stick | MBR at LBA 0 | mbr.S → cdboot (512 B sectors) | 32-bit PM | `_start32` | cdboot |
| UEFI, disc | El Torito section 0xEF → ESP image | firmware FAT → `\EFI\BOOT\BOOTX64.EFI` | long mode, firmware paging | `_start64` (rdi=0x70000) | loader.c at 0x70000 |
| UEFI, stick | MBR partition type 0xEF (or possibly El Torito, see §11) | same | long mode | `_start64` | loader.c |
| QEMU `-kernel` | multiboot header (kludge) in `zelr.bin` | QEMU | 32-bit PM | `_start` (eax=0x2BADB002, ebx=mbi) | `kmain_multiboot` (static in `.bss`) |

**BIOS disc, step by step:**
1. The firmware reads the BRVD (LBA 17), then the catalog (LBA 19), loads 2048 bytes from RBA 23 to 0x7C00, and jumps with DL = CD drive.
2. Stage 1: sector size 2048, so `patch_self`×1 = LBA 23. Reload one sector to 0x9000, check `"ZLR1"`, copy to 0x7C00.
3. Stage 2: A20 → geometry (2048, scale 1, chunk 16) → zero handoff → E820 → unreal → 32 KiB chunks from `patch_lba` to 0x10000, then copied to 0x1000000+ → check `"ZELR"`.
4. `lgdt`, PE, `ljmpl 0x08:protected`; `ecx = *(0x1000010)`, `ebx = 0x70000`, `jmp ecx`.
5. `_start32` → `go_long` (4 GiB 2 MiB identity map, PAE, LME, PG, `lgdt gdt64`, `ljmp 0x08`) → `long_start` → `kmain(0x70000)`.

**BIOS stick:**
1. The BIOS loads the MBR at 0x7C00 (DL=0x80). It relocates to 0x0600, reads 4×512 from LBA 92 to 0x7C00, and jumps.
2. cdboot runs as above with sector size 512: stage 1 reads LBA 23×4 = 92 (4 sectors), and stage 2 uses `lba_scale` 4 and `chunk_sectors` 64.

**UEFI (disc or stick):**
1. The firmware mounts the ESP and runs `BOOTX64.EFI` → `EfiMain` (§3.10): handoff page 0x70000, ACPI, GOP, `zelr.bin` from the ESP **root** to 16 MiB (or staged), ExitBootServices loop, optional move, `"ZELRKRN\0"` check.
2. `(*(u64 *)(0x1000008))(h)` → `_start64` → `kmain(h)`.

**QEMU -kernel:**
1. QEMU finds the multiboot magic at file offset 24. With KLUDGE set it ignores ELF, copies the file to `load_addr` = 16 MiB, zero-fills to `bss_end`, which is already the end of the file, and enters `_start` with eax=0x2BADB002, ebx=mbi.
2. `_start` → `go_long` → `long_start` → `kmain_multiboot` → `kmain`. This is the only path with a command line (`-append selftest` / `console`).

### 4.3 Mode transitions
- **BIOS:**
  - real (16-bit, SS:SP 0:7C00)
  - → brief PE=1 to load FS with a 4 GiB descriptor, → PE=0 ("unreal": real mode with the FS limit at 4 GiB), redone after every INT 13h
  - → protected 32-bit (flat GDT 0x08/0x10, ESP 0x7C00)
  - → [kernel] PAE, CR3, LME, PG (compatibility mode) → far jump to a 64-bit CS: long mode.
- **Multiboot:** 32-bit PM → the same kernel steps.
- **UEFI:** already long mode and identity-paged. The kernel keeps the firmware GDT until `gdt_init` and the firmware CR3 until the end of `paging_init` (`paging.c:503`).
- **AP:** real mode at 0x8000 → PAE, CR3 (BSP tables), LME → CR0.PE|PG together (compatibility mode) → `lgdt` → far jump 0x08 → 64-bit → `ap_main(index)` on its own stack.

### 4.4 Kernel entry and handoff validation
`_start*` → [`kmain_multiboot` checks 0x2BADB002 and panics otherwise] → `kmain`: serial, blackbox, VGA, then `h && h->magic == HANDOFF_MAGIC` or panic, then cmdline flags, then banner, then `gdt_init` (the start of the other agent's area).

### 4.5 SMP bring-up (trampoline side)
1. `smp_init` counts usable APs. If there are fewer than 2 CPUs it stops.
2. `memcpy(0x8000, trampoline, len)`.
3. For each AP, `start_cpu(i)`:
   - kmalloc the stack, paint it, `tss_set_stack_for`;
   - `patch_trampoline(top, i)`;
   - INIT assert/deassert, 10 ms;
   - up to two SIPIs (vector 0x08), each followed by up to 200×1 ms polls of `cpus[i].info.started` (`smp.c:170-219`).

### 4.6 Physical addresses used during boot

| Physical | Size | Use | Owner / path |
|---|---|---|---|
| 0x00500 and 0x100500 | 1 B each | A20 wrap test (restored) | cdboot |
| 0x00600–0x007FF | 512 | relocated MBR | mbr.S (BIOS stick) |
| 0x07000 | 1 | `DRIVE_STASH` | cdboot stage 1 |
| 0x07010 | 16 | `S1_DAP` | cdboot stage 1 |
| < 0x07C00 | stack | SS:SP 0:7C00, later ESP 0x7C00 | mbr, cdboot |
| 0x07C00–0x083FF | 2048 | cdboot.bin (also where the MBR lands first) | firmware, mbr, stage 1 |
| 0x08000–~0x080C8 | ~200 (estimate) | SMP trampoline and parameter block | `smp.c` |
| 0x09000–0x097FF | 2048 | `S1_SCRATCH` reload buffer | cdboot stage 1 |
| 0x10000–0x17FFF | 32 KiB | INT 13h read buffer (`BUFFER_SEG`) | cdboot |
| 0x70000–0x70CDF | 3296 (1 page reserved on UEFI) | `handoff_t` | cdboot, loader.c |
| 0xB8000 | text | "needs a 64-bit processor" | boot.S |
| 0x1000000 (16 MiB) | = zelr.bin | kernel, `.bss` included | all |
| 0x1000000+8 / +16 | 8 / 8 | `_start64` / `_start32` pointers | UEFI loader / cdboot |
| 0x1000000+24 | 32 | multiboot header | QEMU |
| kernel `.bss` | 24 KiB + 64 KiB | boot PML4/PDPT/PD×4 and boot stack | boot.S |
| any | size | UEFI staging copy (EfiLoaderData, reported `MEM_RESERVED` forever) | loader.c (moved case) |
| page-aligned `__kernel_end` | ≤ 2 MiB | PMM bitmap | pmm.c |
| `roundup_MiB(__kernel_end + 2 MiB)` | `heap_bytes` | kernel heap | main.c |
| 0 – 4 GiB | map | 2 MiB identity map (BIOS and multiboot only), replaced by `paging_init` | boot.S |
| 0 – 64 MiB | map | 4 KiB identity map, plus usable RAM in 2 MiB pages up to 64 GiB | paging.c |
| 0x8040000000 | -- | link base of built-in user ELFs | sdk/zelr.ld |

---

## 5. Interfaces

**Exported by this area:**
- Entry symbols `_start` (the ELF entry and multiboot), `_start32`, `_start64` (`boot.S`). They call `kmain(handoff_t *)` and `kmain_multiboot(u32, u32)` in `main.c`.
- Linker symbols `__kernel_start`, `__kernel_end`, `__bss_start`, `__bss_end` (users in §3.3).
- `handoff_t` and constants. Consumers: `main.c`, `pmm.c` (`pmm_init`), `paging.c` (`paging_init`), `include/pmm.h`, `include/paging.h`.
- `trampoline_start` / `trampoline_end`, used by `smp.c`.
- `builtin_<name>_start/_end`, used by `builtin.c`.
- `builtin_install()` (`main.c:444`) and `builtin_count_programs()` (`main.c:445`, `selftest.c:879,887`).
- `user_stub_start/_end`, used by `user.c:51,355`.
- Build artifacts:
  - `build/zelr.bin`: `-kernel` in run.sh, zelr.bat, harness.py and the *_test.sh scripts; the ESP; the launcher embed.
  - `build/zelr.iso`: iso_test.sh, zelr.bat iso/modern, `pipeline/release.sh`.

**Depended on:**
- BIOS services: INT 10h AH=0Eh; INT 13h AH=42h/48h; INT 15h AX=2401h and E820. Ports 0x92, 0x60 and 0x64.
- UEFI services: boot services, GOP, LoadedImage, SimpleFileSystem, the configuration table.
- The multiboot v1 contract (QEMU).
- Kernel functions used from this area: `serial_init`, `bb_*`, `vga_init`, `panic`, `strncpy`, `memset`; `vfs_add_builtin` → `sysfs_add_program`; `paging_kernel_directory` and `ap_main` (patched into the trampoline).
- Tools: python 3, bash, zig (cc and ld.lld), coreutils; `git` for check_version.

---

## 6. Concurrency, locking, memory ownership, invariants

- **Single-threaded until SMP.** Everything here runs on the BSP with interrupts off, except that the BIOS loader runs with IF=1 for BIOS calls. The trampoline has **one parameter slot**, and correctness depends on `start_cpu` being serial.
  - After a timed-out AP, `start_cpu` returns false without freeing the stack, and the next CPU's parameters overwrite the slot. An AP that wakes late would run with the next CPU's stack and index (potential race, §10).
- **`.bss` is zero only because it is inside `zelr.bin`.** `flatten.py` uses `p_memsz` (L67), cdboot copies `patch_bytes = len(zelr.bin)`, the UEFI loader reads the whole file, and multiboot sets `load_end=0` with `bss_end=__bss_end`, which is the end of the file. Neither `boot.S` nor `kmain` zeroes `.bss`. A future change that stops including `.bss` in the flat file, for example to shrink the ISO, would break all three loaders silently.
- **Handoff lifetime.** The BIOS and UEFI handoffs are at 0x70000, below 1 MiB. `pmm_init` never frees the first MiB (`pmm.c:98-100`), and the UEFI page is EfiLoaderData, reported `MEM_RESERVED`. The multiboot handoff is the static in `.bss`. `kmain` reads `h` only during init (the last use is `main.c:392-393`) and keeps no pointer to it.
- **The kernel image is protected by linker symbols, not by the handoff.** `pmm_init` reserves `[__kernel_start, bitmap end)`. `kernel_base`/`kernel_size` are informational only.
- **UEFI firmware structures still in use after exit.**
  - `_start64` runs on the firmware's page tables until `paging_init` loads CR3 (`paging.c:503`), and on the firmware GDT until `gdt_init`.
  - The loader reports `EfiBootServicesCode/Data` as `MEM_USABLE`.
  - `paging_init` allocates frames through the PMM **before** switching CR3 (`paging.c:449-466`), starting from the lowest free frame above 1 MiB.
  - Invariant assumed: no firmware page-table page is among those frames. See §10.
- **The trampoline assumes:**
  - the kernel PML4 is below 4 GiB (32-bit `movl` to CR3, `trampoline.S:45-46`);
  - 0x8000 is identity-mapped in the kernel tables (low 64 MiB, page by page);
  - the page stays reserved (first MiB);
  - NX is not used (EFER.NXE is not set).
- **Unreal-mode invariant (BIOS).** FS must be reloaded after any BIOS call that may reload segment registers. `load_payload` does this after every INT 13h. `check_payload` runs after INT 10h prints without re-entering unreal mode, and fails safe by printing its message.
- **UEFI get-map/exit discipline.** Only arithmetic runs between `get_memory_map` and `exit_boot_services`: no prints, no allocations.
- **Load-address invariant.** `linker.ld` = `loadaddr.py` = `KERNEL_PHYS` = `patch_load` default = mkiso patch. The first four are enforced at build time. The image must also end below `KERNEL_LOW_MB` (64 MiB) with 2 MiB of bitmap room, which nothing enforces (§9).

---

## 7. Limits and magic numbers

### 7.1 Table

| Name / value | Where | Meaning |
|---|---|---|
| `0x1000000` (16 MiB) | `linker.ld:33` (`16M`), `loader.c:388`, `cdboot.S:47` | kernel physical load and link address |
| `HANDOFF_MAGIC 0x5A454C5236344846` | `handoff.h:19`; cdboot `0x36344846`/`0x5A454C52` (L676-677) | handoff validity |
| `HANDOFF_MAX_REGIONS 128` | `handoff.h:34` | memory map entries (silent truncation on all paths) |
| handoff size 3296 (0xCE0) | derived; `cdboot.S:667` `H_LOADER+160` | zero range |
| `0x70000` | `cdboot.S:59` (`HANDOFF_SEG 0x7000`), `loader.c:393` | handoff physical address |
| `"ZELRKRN\0"` + `_start64`@+8 + `_start32`@+16 | `boot.S:24-29` | kernel image header |
| `0x1BADB002`, flags `0x00010003`, checksum `0xE4514FFB` | `boot.S:31-36` | multiboot v1 header (a.out kludge) |
| `0x2BADB002` | `multiboot.h:4` | multiboot loader magic |
| `"ZELRBOT1"` | `cdboot.S:43` | patch-block signature |
| `"ZLR1"` = `0x31524C5A` | `cdboot.S:84,805`; `check_loader.py:19` | stage-1 proof-of-full-read signature (after byte 512, within 2048) |
| 512 / 2048 | `check_loader.py:20-21` | first-sector and stage-1 read sizes |
| `DRIVE_STASH 0x7000`, `S1_DAP 0x7010`, `S1_SCRATCH 0x9000` | `cdboot.S:76-83` | stage-1 scratch |
| `BUFFER_SEG 0x1000` (0x10000), `CHUNK_SECTORS 16` (32 KiB) | `cdboot.S:71,85` | BIOS read chunk |
| sector sizes tried {reported, 2048, 512}; > 2048 rejected | `cdboot.S:112-114,205,468-469` | 4Kn media unsupported |
| E820 `ECX=20`, `'SMAP' 0x534D4150` | `cdboot.S:501-507` | 20-byte entries |
| A20 test 0000:0500 vs FFFF:0510 | `cdboot.S:317-358` | wrap detection |
| KBC wait 0xFFFF polls | `cdboot.S:397,407` | bounded waits |
| MBR `RELOC 0x0600`; DAP 4 sectors at LBA 92 | `mbr.S:19,87,93` | stick path |
| MBR code ≤ 446 B | `mkiso.py:247` | partition table starts at 446 |
| `TRAMPOLINE_PHYS 0x8000`, SIPI vector 0x08 | `smp.c:39,209`; `trampoline.S:28` | AP start |
| `"ZELRSMP1"` + 4×u64 | `trampoline.S:107-111` | AP parameter block |
| GOP cap 3840×2160 | `loader.c:111-112` | back-buffer ceiling |
| map buffers 32 KiB (×2 static) | `loader.c:271,330` | EFI map capacity (about 680 descriptors of 48 B) |
| ExitBootServices attempts 8 | `loader.c:332` | retry cap |
| 4 GiB identity map, 2 MiB pages, flags 0x83 | `boot.S:174-209` | boot page tables |
| boot stack 64 KiB | `boot.S:67-69` | shared by all entry paths |
| `KERNEL_LOW_MB 64` | `paging.h:33` | 4 KiB-mapped low region; the image must fit below it |
| `BITMAP_ROOM 2 MiB`, `HEAP_MIN 24 MiB`, `HEAP_SHARE 4`, `HEAP_CEIL 512 MiB` | `main.c:65,81,92-93` | memory layout behind the image |
| `SYSFS_MAX_PROGRAMS 48` (46 used) | `sysfs.h:30`; `builtin.c:126` | built-in program slots |
| user link base `0x8040000000` | `sdk/zelr.ld:5` | built-in ELF address |
| ISO: PVD 16, BRVD 17, TERM 18, catalog 19, path tables 20/21, root 22, loader 23 | `mkiso.py:46-53` | fixed ISO sectors |
| ESP ≥ 4096 KiB, +1024 KiB slack, rounded to MiB | `mkiso.py:332-334` | ESP sizing |
| El Torito EFI count `& 0xFFFF` | `mkiso.py:184` | wraps above about 32 MiB ESP |
| image padded to a multiple of 16 sectors (32 KiB) | `mkiso.py:351` | overshoot tolerance |
| MBR partition types 0x17 (whole, active), 0xEF (ESP) | `mkiso.py:264-286` | hybrid table |
| FAT16: spc from {1…64} for 4200–65000 clusters; reserved 4; root 512; serial `0x5A4C5202` | `mkfat.py:38-49,214` | ESP and test volumes |
| FAT32: spc 1, reserved 32, backup at 6, serial `0x5A4C5200` (= kernel `ZELR_VOLUME_ID`) | `mkfat.py:264-266,430`; `fat.c:438` | test volumes |
| FAT date `0x5A21` (2025-01-01); ISO date 2025-01-01 | `mkfat.py:137,340`; `mkiso.py:82,118` | fixed timestamps |
| GPT: 128×128 entries, ESP 16 MiB at LBA 34, default disk 64 MiB | `mkgpt.py:29-31,95-106` | test disk |
| isa-debug-exit iobase 0xF4; exit `(code<<1)\|1` | `main.c:156-161`; run.sh:39-43 | selftest status |
| `KERNEL_VERSION "0.37.0"` | `include/types.h:25` | version string |

### 7.2 Compiler and linker flags, with reasons

| Target | Flags | Reason (stated, or "not stated") |
|---|---|---|
| Kernel (`build.sh:41-50`) | `-target x86_64-freestanding-none` | zig's clang as the cross compiler, no separate toolchain (L2-3) |
| | `-ffreestanding -nostdlib -static -O2 -std=gnu11` | no libc or crt; not otherwise stated |
| | `-fno-sanitize=undefined -fno-stack-protector -fno-stack-check` | no runtime support for these; not stated |
| | `-fno-builtin` | not stated. README:1423-1426: it did *not* stop clang turning memset's loop into a call to memset; the fix was volatile pointers |
| | `-fno-omit-frame-pointer` | not stated. Entry code zeroes `rbp`, but no in-kernel rbp-chain walker was found |
| | `-fno-pic -fno-pie` | absolute addresses at the fixed link address (implied) |
| | `-mno-sse -mno-sse2 -mno-mmx -mno-80387` | stated: xmm use before OSFXSR means #UD; no floating point in the kernel (L5-10) |
| | `-mno-red-zone` | not stated. Standard for kernels: interrupt frames land on the current stack |
| | `-mcmodel=small` | not stated. The kernel sits below 2 GiB |
| | `-Wall -Wextra -Wno-unused-parameter` | warnings only, no `-Werror` |
| | `-Iinclude -DZELR_BLOB_STAMP=$BLOBS` | stated: zig's cache ignores `.incbin` inputs (L33-38). The macro is referenced nowhere in the source |
| | `-Wl,-T,linker.ld -Wl,--build-id=none -Wl,-z,max-page-size=4096` | not stated. They keep a build-id note out of the image and keep PT_LOAD alignment at 4 KiB |
| cdboot / mbr (`bootloader/build.sh:17-29`) | `-target x86-freestanding-none -mcpu=i686 -ffreestanding -nostdlib -static -c`; `ld.lld -T` | BIOS loads raw bytes, so link at 0x7C00 and flatten (L2-3) |
| trampoline (`bootloader/build.sh:34-36`) | `-target x86_64-freestanding-none … -c`; `ld.lld -T trampoline.ld` | fixed low address, no relocation (L32-33) |
| UEFI (`uefi/build.sh:17-22`) | `-target x86_64-uefi -ffreestanding -nostdlib -fshort-wchar -fno-sanitize=undefined -fno-stack-protector -fno-stack-check -mno-red-zone -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter` | PE for the firmware, MS calling convention (L2-4) |
| Userland (`userland/build.sh:27-35`) | `-target x86_64-freestanding-none -I../sdk … -fno-builtin -fno-pic -fno-pie -mcmodel=large -mno-red-zone -Wall -Wextra -Wl,-T,../sdk/zelr.ld -Wl,--build-id=none` | programs are "not linked against the kernel in any way" (L2-4). `large` because the link base is above 2 GiB |

---

## 8. Tests

- **`tools/iso_test.sh`** (gate `full` only, `gate.sh:396-402`; 900 s timeout):
  - Builds the ISO (with `ZELR_PREBUILT` in the gate) and creates a fresh 32 MiB scratch disk per run.
  - Boots, over `-serial stdio` with `-m 256 -no-reboot -display none`:
    1. `-cdrom ISO -boot d` (BIOS disc);
    2. ISO as IDE disk 0, `-boot c` (BIOS stick);
    3. pflash `edk2-x86_64-code.fd` plus `-cdrom` (UEFI disc);
    4. pflash plus the ISO as IDE disk 0 (UEFI stick).
    The UEFI runs are skipped if the firmware file is missing (`$ZELR_UEFI_FW`).
  - Types ESC (leave the desktop), `write booted.txt <path>`, `cat booted.txt`, `reboot`.
  - Greps for `"zelr $VERSION"` (from `types.h`), `"long mode"`, `"progs"`, `"zelr:/home>"` and the path name. It proves banner, version, builtin install and a working shell with a writable disk. Exit status = number of failures.
- **`tools/gpt_test.sh`** (gate `full`):
  - Boots `-kernel build/zelr.bin -append console` against:
    - `mkgpt.py` good: expects `parts gpt, 2 partition(s)`, `efi system partition`, no `mounted partition 1`, `fs mounted partition 2`, and the file text;
    - bad header CRC: expects `gpt rejected: header checksum`, nothing mounted. The kernel does **not** fall back to the valid backup header, and must not fall back to the 0xEE protective entry;
    - bad entry CRC: `gpt rejected: entry array checksum`;
    - 32 MiB zero image: `parts none`, `new disk prepared`.
  - Each boot ends with `reboot`, so nothing is flushed.
- **`tools/bootcheck.py`** (gate `screen`/`full`):
  - Tests the *kernel's* FAT formatter boot sector, not zelr's loaders.
  - A first boot formats a 64 MiB disk and writes `/keepme.txt`.
  - Checks `55AA`, a FAT16/FAT32 label, the jump `EB 3C 90` / `EB 58 90`, and non-zero code at `want+2`.
  - A bare-BIOS boot of that disk must draw more than 150 lit pixels in rows 118–150 of a screendump.
  - It then restores the "old broken" sector: `EB 3C 90` and zeros after the BPB. It boots the kernel (whose `fat_boot_repair` mends it), checks the file survived, and bare-BIOS boots again.
- **Build-time checks** (§9, §10). `check_loader.py` runs on every build. `flatten.py`'s below-base check. `_Static_assert` in `builtin.c`. mkiso's MBR size and DAP checks. `check_version.py` runs in every gate mode.
- **Kernel selftest sections touching this area** (`selftest.c:3551-3601`, run with `-append selftest`, i.e. the multiboot path only):
  - `[built-in programs]` `test_builtin` (L878-916): count ≥ 3; `vfs_builtin_count() == builtin_count_programs()`; `/bin/browser` and `/bin/paint` present and > 1 KiB; paint starts with `7F 'E' 'L' 'F'`; `/bin` cannot be written or deleted; `paint` is not on the FAT volume.
  - `[userspace]` (L634+): `user_spawn_stub` runs and makes ≥ 7 syscalls.
  - `[live tree]` (L2966-2969): `/sys/version` contains `KERNEL_NAME` and `KERNEL_VERSION`.
  - `[the identity map]` (L116+): 1:1 from 1 MiB to 64 MiB.
  - `[processors]` `test_smp` (L1182+): with ≥ 2 CPUs, every processor was started (the trampoline works). `run.sh -T` uses the default single CPU, so this part is SKIPPED there; `zelr.bat test` uses `-smp 2`.
- **Not covered by anything automated:**
  - the `H_*` offsets and `zero_handoff` size against `handoff.h`;
  - stage 1 fitting in 512 bytes;
  - the ESP catalog count overflow;
  - pixel format;
  - the UEFI moved-kernel path (QEMU/OVMF grants 16 MiB, so it probably never runs in CI; not verifiable here);
  - `check_sse.py`, `whereis.py`, `uefi/stub64.c`.

---

## 9. How to extend

- **Change the load address.**
  1. Edit only `linker.ld`'s `. = 16M;`. Keep the decimal-plus-suffix form, because `loadaddr.py` cannot parse hex.
  2. Update `KERNEL_PHYS` (`loader.c:388`) and the `patch_load` default (`cdboot.S:47`); `check_loader.py` fails the build until both match.
  3. Keep `__kernel_end + BITMAP_ROOM` below 64 MiB (`KERNEL_LOW_MB`). On UEFI the kernel range is EfiLoaderData, reported `MEM_RESERVED`, so anything above 64 MiB would not be mapped by `paging_init`'s 2 MiB pass. Nothing checks this.
  4. Keep clear of the firmware's reserved areas; this is why the address moved from 1 MiB.
- **Change `handoff_t`.**
  1. Update `cdboot.S` `H_*` (L59-69) and the zero length `H_LOADER+160` (L667) **by hand**. Nothing checks them.
  2. Update `kmain_multiboot`. The UEFI loader recompiles automatically.
  3. Use only `unsigned long long` and `unsigned int` (MS ABI for the PE loader).
  4. Consider changing `HANDOFF_MAGIC` for incompatible layouts (keep cdboot's two halves; `check_loader.py` verifies them).
  5. Natural next fields: a pixel format / channel masks from GOP, and the UEFI load options as a command line.
- **Command line on BIOS/UEFI.** Neither loader fills `cmdline`. For UEFI, `EFI_LOADED_IMAGE_PROTOCOL.load_options`/`load_options_size` is already declared in `efi.h:167-168`: convert UTF-16 to ASCII into `h->cmdline`. Remember that `kmain` matches `selftest`/`console` as substrings anywhere.
- **Add a built-in program.**
  1. Create `userland/<name>.c`; `userland/build.sh` builds it automatically.
  2. Add a stanza to `kernel/builtin.S` (`.balign 4`, two `.global`s, start label, `.incbin "build/user/<name>.elf"`, end label).
  3. Add the extern and a `PROGRAMS[]` entry in `kernel/builtin.c`.
  4. Only 2 slots remain before `SYSFS_MAX_PROGRAMS` (48, `include/sysfs.h:30`) must be raised; the static assert will say so.
  5. `ZELR_BLOB_STAMP` takes care of cache invalidation. A `.c` file not added to both lists is built but silently absent from `/bin`.
- **Add another loader or boot path.**
  1. Produce the flat `zelr.bin` bytes at 16 MiB, including the zero `.bss` (copy the whole file).
  2. Build a `handoff_t`.
  3. Enter either through `*(u64 *)(img+8)` in long mode with an identity map covering the image, the handoff and the stack (`rdi=h`, SysV), or through `*(u32 *)(img+16)` in 32-bit flat PM with paging off and `ebx=h`.
  4. Do not use cdboot's `patch_entry` / the ELF `e_entry`. That is `_start`, the multiboot entry, which would panic with "not booted by a multiboot loader".
- **Grow the images.** The ESP is sized automatically. The El Torito EFI count wraps above 65535×512 B; saturate it at 0xFFFF or use a count of 0 or 1 if the ESP passes about 32 MiB. The kernel is stored twice in the ISO.
- **Change the trampoline.**
  - Keep `"ZELRSMP1"` 8-aligned and followed by 4 × u64.
  - The CR3 load is 32-bit, so change it to a 64-bit load after entering long mode if the PML4 could ever sit above 4 GiB.
  - If NX is introduced, set EFER.NXE (bit 11) alongside LME here and in `boot.S`.
  - It must stay small enough to live at 0x8000 inside the reserved first MiB.
- **Pitfalls the comments warn about:**
  - never spell an address literally (the `0x00100000` story);
  - stage 1 must not use memory inside the 2 KiB file after the reload;
  - re-establish unreal mode after BIOS calls;
  - `.multiboot` must stay in the first 8 KiB and `.kheader` first;
  - nothing may run between `get_memory_map` and `ExitBootServices`;
  - `/bin` must never be copied to disk;
  - parallel harnesses must not rebuild (`ZELR_PREBUILT=1`).

---

## 10. Doc drift and suspicious code (verified unless marked)

### Likely bugs and latent defects
1. **Stage 2 discards the sector size stage 1 proved** (`cdboot.S:125-172` vs `454-483`). Stage 1 falls back through {reported, 2048, 512} and validates by signature. `probe_geometry` then re-asks INT 13h AH=48h and trusts any value from 1 to 2048. On firmware that misreports (the exact case stage 1 guards against), stage 1 succeeds on a fallback while stage 2 reads the payload with the wrong `lba_scale` and chunk size, and halts with "the kernel did not reach 1 MiB". Stage 1's result cannot survive: its variables live in the region overwritten by the self-copy, and only `DRIVE_STASH` is kept outside.
2. **The UEFI pixel format is lost.** `setup_graphics` accepts both `PixelRedGreenBlueReserved8BitPerColor` and `PixelBlueGreenRedReserved8BitPerColor` (`loader.c:102-103`), but `handoff_t` has no format field and `fb_adopt(base,w,h,pitch)` (`fb.c:119`) assumes one layout. On an RGB-only GOP the red and blue channels will be swapped. Also, if no 32-bit mode exists at all, the loader keeps the current mode and reports it as 32 bpp with that mode's `frame_buffer_base`, whatever its format (`loader.c:121-135`); for a BltOnly mode that base may be meaningless.
3. **Multiboot fallback region is 1 MiB too long** (`main.c:287-290`). It uses `base = 0x100000`, `len = (mem_upper + 1024) * 1024`. `mem_upper` is KiB above 1 MiB, so the length should be `mem_upper*1024`; as written the region ends 1 MiB past RAM. `flags` bit 0 (mem_* valid) is not checked either. It only matters for a multiboot loader that supplies no mmap.
4. **El Torito EFI sector count is masked, not saturated** (`mkiso.py:184`). Once the ESP exceeds 65535 virtual sectors (about 32 MiB, i.e. a kernel image of about 31 MiB) the count wraps to a small number. It is latent today.
5. **The gate's build step ignores `build.sh`'s exit status** (`pipeline/gate.sh:227-228`, run with `set -uo pipefail` and no `-e`). Only the literal word `error` in the output fails "it builds". None of the failure messages of `check_loader.py`, `loadaddr.py`, `flatten.py` or "zig not found" contain it. Such a failure aborts `build.sh` (`set -e`) before the kernel is compiled; the gate reports success and then tests the **previous** `build/zelr.bin`, because `ZELR_PREBUILT=1` blocks rebuilds.
6. **UEFI moved-kernel hazards** (potential; occurrence not verified). `range_is_ours_after_exit` counts `EfiLoaderCode` and `EfiBootServicesData` as free targets (`loader.c:254-261,292`). The byte move after exit (`loader.c:444-450`), however, runs from the loader's own image, on the firmware-allocated stack, under the firmware page tables, all of which live in those types. A 16 MiB range that covers any of them would crash mid-copy with nothing on screen.
7. **UEFI: firmware page tables are reported usable while still live** (potential). See §6. `paging_init` allocates and writes frames through the PMM before switching CR3 (`paging.c:449-503`). If firmware placed page-table pages in low boot-services memory above 1 MiB (the loader comment says the tested firmware uses boot-services memory around 1 MiB, `loader.c:197-202`), the first allocations could overwrite them.
8. **The UEFI handoff address is fixed with no fallback** (`loader.c:405-409`). Firmware that owns 0x70000 makes the loader die. The comment's reason ("everything the firmware allocated stops being described the moment boot services end", L390-392) is inaccurate: this page is itself an ordinary `EfiLoaderData` allocation, and such allocations stay described (as reserved) in the map the loader builds. Any `EfiLoaderData` page would do.
9. **SMP late-AP race on the single parameter slot** (potential; `smp.c:193-219`, `trampoline.S:19-22`). A timed-out AP keeps its stack allocated, and the slot is repatched for the next CPU. If the late AP then wakes, two CPUs share a stack and index.
10. `mkfat.py --at-cluster` without `--fat32` raises `AttributeError`: `Fat16Builder` has no `skip_to_cluster` (`mkfat.py:488-489`).
11. `readfat.py put_file` replaces a same-name entry without freeing its old chain, leaking clusters (`readfat.py:248-251`). A name with two dots puts a `.` into the extension field.
12. `kmain`'s cmdline matching is substring-based at every offset (`main.c:313-316`), so e.g. `noselftest` enables the selftest.
13. `check_version.py`'s `parts()` crashes on a non-numeric version component.
14. The `build.sh` BLOBS pipeline under `set -e` without `pipefail` (`build.sh:39`): a failed `cat` is not detected. zig's `.incbin` would fail afterwards anyway.

### Dead or unused code
- `uefi/stub64.c` and `uefi/stub.ld` are never built and incompatible with the current loader (no `ZELRKRN` header, 1 MiB link).
- `cdboot.S`: `patch_entry` (patched but never read), `mmap_bytes` (L738), and `H_FB_BASE`/`H_RSDP` are unused. `bootloader/link.ld` defines `__loader_end`, which nothing uses. `__bss_start` has no user.
- `handoff_t.kernel_base`, `.kernel_size` and `.fb_bpp` are written but never read by the kernel. `MEM_ACPI` is never reclaimed.
- `tools/check_sse.py` is not invoked anywhere. `tools/whereis.py` is broken for ELF64 (`whereis.py:8`).
- `-DZELR_BLOB_STAMP` is intentionally unreferenced (cache key only).

### Doc and comment drift
- `README.md:1431`: "Fixed with -mcpu=i686, and tools/check_sse.py now fails the build". The kernel uses `-mno-sse…`, only the 16/32-bit loaders use `-mcpu=i686`, and check_sse is not part of any build.
- `README.md:450`: "Two-level paging. The low 16 MiB is identity mapped". The code has 4-level paging with 64 MiB page by page. `README.md:623` and `:1687` say "ELF32 loader"; user programs are ELF64 (x86_64 target).
- `README.md:355-356`: "You need QEMU and Zig". Python (invoked as `python`) and bash are required by `build.sh`.
- `README.md:398`: the launcher "embeds build/zelr.elf". `launcher/ZelrLauncher.csproj:33` embeds `..\build\zelr.bin`.
- `README.md:1749-1753`: "userland/ … three small tests" (there are 46 programs) and "the four test harnesses".
- `include/handoff.h:7`: "the BIOS loader builds it from INT 15h and the VBE ports". cdboot never touches VBE (fb stays 0). The kernel's `fb_init` later uses the Bochs VBE ports 0x1CE, which exist only under QEMU/Bochs/VirtualBox. On real BIOS hardware without VMware SVGA that means VGA text mode. `cdboot.S:657-659` makes the same implicit assumption.
- `include/handoff.h:46`: kernel_base/size exist "so it can avoid handing itself out". Unused. `:25` says `MEM_ACPI` is "reclaimable". Never reclaimed.
- `boot/boot.S:3`: "two ways into this kernel". There are three entries (`_start`, `_start32`, `_start64`).
- `cdboot.S`:
  - L5 says "build the multiboot structure" (it builds `handoff_t`);
  - L14-16 and L308 say "copy … up to 1 MiB";
  - `msg_nopayload` (L790) says "did not reach 1 MiB" (16 MiB now);
  - L22 says "four values" (there are five);
  - L799-801 says "bootloader/build.sh checks it"; it is `tools/check_loader.py`, called by it.
- `tools/mkiso.py:31`: the kernel is loaded "at 1 MiB". It uses `load_address()` = 16 MiB.
- `uefi/loader.c:168-169`: "same directory this loader was loaded from". It opens relative to the volume root, and mkiso places `zelr.bin` at the ESP root while the loader is in `\EFI\BOOT\`.
- `zelr.bat`:
  - L97 "one processor" (it uses `-smp 2`);
  - L111-112 "zelr has no USB stack yet" (xHCI, HID and mass storage exist per README:203-218);
  - L14-16 "It maps 64 MiB of itself and ignores the rest". The kernel maps usable RAM up to 64 GiB and sizes its heap to a quarter, up to 512 MiB (`main.c:83-93`, `paging.c:469-492`).
- `tools/mkfat.py:7`: "the kernel already implements FAT16". It implements FAT16 and FAT32.
- `tools/check_sse.py` docstring: 32-bit-era examples.

---

## 11. Open questions

1. **What does "UEFI from a stick" actually exercise under OVMF?** From memory of EDK2's PartitionDxe, not verified here: `PartitionValidMbr` rejects MBRs whose partitions overlap, and zelr's partition 1 (LBA 0, whole image) overlaps partition 2 (0xEF). EDK2 also tries El Torito before MBR, and newer versions accept El Torito on 512-byte media. If so, `iso_test.sh`'s stick run may boot through the El Torito EFI entry rather than the 0xEF partition, leaving the MBR-ESP route untested on real UEFI machines. Worth confirming on EDK2 and real firmware.
2. **What is the actual kernel image size**, and the resulting stage-1, `cdboot.bin`, `mbr.bin` and trampoline sizes? They cannot be measured without a build. Comments suggest `zelr.bin` is around 9–10 MiB (`linker.ld:9-12`).
3. **Does lld emit orphan sections (`.eh_frame`, and so on) into the kernel image?** `linker.ld` has no `/DISCARD/`, and the build does not pass `-fno-asynchronous-unwind-tables`. They are harmless unless placed ahead of `.kheader`, which lld's rank-based orphan placement should not do.
4. Does zig's `x86_64-uefi` build leave side files (`.pdb`/`.lib`) in `build/`?
5. Is the FAT32 test-image serial `0x5A4C5200`, equal to the kernel's `ZELR_VOLUME_ID`, intentional? It makes the kernel repair and claim those images as its own. The FAT16 builder deliberately(?) uses `…02`.
6. Does zig link compiler-rt into the kernel with `-nostdlib`? `kernel/divide.c`, "64-bit division helpers libgcc would normally provide", suggests a 32-bit-era workaround that may now be dead. This belongs to another agent's area.
7. Is the UEFI moved-kernel path, and the fixed 0x70000 handoff, ever exercised on the firmware the author tests against? OVMF normally grants both.
8. Under QEMU's multiboot loader, where do the mbi, mmap and cmdline live relative to `__kernel_end`, where the PMM bitmap is placed? `kmain_multiboot` copies everything before `pmm_init`, so it is safe either way. The exact layout is QEMU-internal and was not verified.
