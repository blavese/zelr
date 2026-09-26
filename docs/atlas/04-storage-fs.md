# Atlas 04: storage, filesystems, VFS, sysfs, layout, black box

Source: the repository root (zelr 0.37.0 + 2 commits, `KERNEL_VERSION "0.37.0"` in include/types.h:25).
Everything below comes from reading the code statically. I built nothing and ran nothing. Line numbers are 1-based and taken from the files as they stand.

---

## 1. Scope

| File | Lines | Role |
|---|---|---|
| include/ata.h | 13 | ATA PIO driver API (`ata_init/present/sectors/max_run/model/read/write/flush`) |
| include/ahci.h | 11 | AHCI driver API (same shape) |
| include/nvme.h | 29 | NVMe driver API plus a design comment (admin and I/O queue pairs) |
| include/blockdev.h | 63 | Block layer: `SECTOR_SIZE`, `blkdev_t`, disk numbering, the per-id and boot-disk calls |
| include/parts.h | 55 | Partition tables: `part_t`, `part_scheme_t`, `crc32` |
| include/diskfs.h | 23 | Bringing volumes up: mount, format, removable mount |
| include/fat.h | 95 | FAT16/32 API, volume selection, boot-sector ownership helpers |
| include/vfs.h | 69 | The single namespace used by the rest of the kernel; `O_*` flags |
| include/fs.h | 32 | In-memory fallback filesystem (`file_t`) |
| include/sysfs.h | 51 | The live tree (/sys and /bin) API, `SYSFS_MAX_PROGRAMS`, `SYSFS_MAX` |
| include/layout.h | 9 | `layout_init`, `layout_home` |
| include/blackbox.h | 74 | Boot log / black box API and constants |
| kernel/ata.c | 164 | ATA PIO, primary bus master, LBA28, polled |
| kernel/ahci.c | 298 | AHCI, first SATA port only, one command table, 4 KiB bounce buffer, polled |
| kernel/nvme.c | 366 | NVMe, admin and I/O queue pairs of 64 entries each, one-page PRP, polled, namespace 1 only |
| kernel/blockdev.c | 157 | Device table (`BLK_MAX` 4), request splitting, picks the boot driver (AHCI, then NVMe, then ATA) |
| kernel/parts.c | 302 | GPT (both CRCs) and MBR reading, boot disk only, bitwise CRC32 |
| kernel/diskfs.c | 174 | Picks the boot volume, prefers "ours", refuses the ESP, formats only unpartitioned disks, USB mount |
| kernel/fat.c | 1763 | FAT16/FAT32 read/write/format, LFN, crash-safe write, rename, reclaim, boot-sector stub and repair |
| kernel/vfs.c | 352 | Path resolution, routing to sysfs, FAT volume 0/1 or the RAM fs |
| kernel/fs.c | 97 | RAM filesystem: 64 whole-path entries |
| kernel/sysfs.c | 467 | 15 generated /sys files and the /bin program table (48 slots) |
| kernel/layout.c | 150 | Creates /home /doc /cfg /tmp, seeds 3 files once per disk (generation marker), empties /tmp |
| kernel/blackbox.c | 326 | Boot log: static 16 KiB buffer, serial mirror, panic screen, reserved-sector record, previous-boot recovery |
| tools/fat32_test.sh | 127 | Two boots on an mkfat.py FAT32 image (a file past cluster 65535), plus a FAT16 image |
| tools/nvme_test.sh | 110 | NVMe on q35: format, write, survive reboot, GPT-on-NVMe |
| tools/mountcheck.py | 124 | USB stick on xHCI: mount, list, read, write, copy between volumes, verified by readfat.py |
| tools/namecheck.py | 99 | LFN write/read/delete across two boots, verified by readfat.py |
| tools/blackbox_test.sh | 110 | Two boots; the second reads /sys/lastboot, fenced |
| tools/crashcheck.py | 154 | Kills QEMU during rewrites of a 64 KiB file and checks the file is whole A or whole B |
| userland/crashwrite.c | 130 | The ring-3 writer and checker crashcheck drives (`/home/crash.dat`) |

Other files I read partly, for cross-references: kernel/main.c (boot order), kernel/fd.c (open-file cache, fsync), kernel/syscall.c (file syscalls), kernel/usb.c and kernel/usbdisk.c (stick registration), kernel/shell.c (kernel console commands), kernel/timer.c, kernel/idt.c and kernel/sched.c (locking and preemption), kernel/builtin.c (the /bin table), kernel/selftest.c (storage sections), kernel/printf.c (panic), tools/gpt_test.sh, tools/mkgpt.py, tools/mkfat.py, tools/harness.py, pipeline/gate.sh, userland/durtest.c, README.md.

---

## 2. Big picture

### 2.1 Layering

```
ring-3 syscalls (syscall.c) ─┐     kernel shell (shell.c), wm.c, theme.c, pins.c, layout.c, selftest
fd.c (whole-file buffers) ───┤              │
                             ▼              ▼
                     vfs.c  vfs_resolve() -> absolute clean path
                       │  1. vfs_generated(): /sys/* and /bin/*  -> sysfs.c (read-only)
                       │  2. route(): "/usb[/...]" -> fat_select(FAT_VOL_USB), prefix stripped
                       │              otherwise   -> fat_select(FAT_VOL_DISK)
                       │  3. fat_mounted() on the selected volume ? fat.c : fs.c (RAM)
                       ▼
                     fat.c   volumes[2] (fatvol_t), macros onto volumes[current_volume]
                       │  vol_read/vol_write: partition-relative, bounded by part_sectors
                       ▼
                  blockdev.c  devices[BLK_MAX=4]; id 0 = boot disk; USB stick registers (usually id 1)
                       │  blk_read_on/blk_write_on split requests into max_run chunks
         ┌─────────────┼─────────────┬───────────────┐
      ahci.c (8)    nvme.c (8)    ata.c (255)    usbdisk.c (8, other area)
```

- diskfs.c runs once at boot (from main.c:421). It calls parts.c, decides which partition is the boot volume, and after mounting runs `fat_boot_repair` and `fat_reclaim`. main.c auto-formats when the result is "no FAT" (-2).
- layout.c runs in the `init` kernel task (main.c:233-237) once the scheduler is up.
- blackbox.c exists from the first line of `kmain` (main.c:306). It is flushed to disk at main.c:645 and in `panic` (printf.c:176).

### 2.2 Design decisions and the reasons the comments give

- **Disks have numbers** (blockdev.h:6-17). At first there was one disk and `blk_read` took no id, so a USB stick could be dumped but never mounted. Disk 0 is still "the one booted from" and keeps the old calls, so older code is unchanged. Later arrivals register and get their own id.
- **Requests are split in one place** (blockdev.c:68-77, blockdev.h:48-49). Drivers take different run lengths: AHCI 8, NVMe 8, ATA 255, USB 8. The black box once asked for 32 sectors and "silently never wrote a byte on any AHCI" machine. An over-long request looks like a disk error, so splitting lives in `blk_read_on/blk_write_on`.
- **Driver order is AHCI, then NVMe, then ATA** (blockdev.c:1-9, 135-136): "Tried in the order that changes least, so every machine that worked before this picks the same controller it always did."
- **Everything is polled** (nvme.c:3-8). The block layer is synchronous, so an interrupt would only set a flag that the same loop waits on.
- **Identity-mapped heap** (ahci.c:109-111, nvme.c:10-13, 190-196). A `kmalloc` pointer is also the physical address given to the controller. NVMe checks this with `virt_to_phys`; AHCI assumes it.
- **NVMe moves one page per command** (nvme.c:50-53). A PRP list "is a second allocation and a second failure mode, for a driver whose caller splits anyway".
- **Partition tables are read, never written** (parts.h:18-20): "writing one is how you destroy their disk by getting it wrong." GPT is read first because "a GPT disk always carries an MBR as well, so asking the other way round finds the decoy" (parts.c:294-295). Both GPT CRCs are checked. MBR entries are range-checked (parts.c:3-9). Sector numbers are 32-bit, so GPT entries past 2 TiB are skipped rather than truncated, because "a truncated start is a valid looking address that points at somebody else's data" (parts.c:10-13, 210-213).
- **Volume rules are "deliberately timid"** (diskfs.c:13-24): use a partition zelr formatted; also use any other FAT partition that is not the ESP; never touch the ESP; never format an existing partition, only "an unpartitioned disk with no filesystem on it". Section 10 shows that the last rule is enforced by `parts_count()==0`, which is weaker than the comment claims.
- **FAT is used for interoperability** (fat.c:3-8). Every field sits where the specification puts it.
- **FAT width is decided by cluster count, never by the label** (fat.c:15-22, 387-392).
- **FAT updates are written through** (fat.c:285-289). The crash-safety of `fat_write_file` needs the new chain on disk before the directory entry points at it. Both FAT copies are written.
- **Crash-safe replace** (fat.c:1433-1477): new chain first, flush, one-sector directory entry commit, flush, then release the old chain. Clusters stranded by a crash are recovered by `fat_reclaim` at mount (fat.c:1665-1674).
- **LFN** (fat.c:918-935). Short names are still written for anything that fits 8.3, "so every file this system wrote before today reads back exactly as it did". The short-name checksum stops stale long entries from being believed.
- **Rename only changes the name inside the existing entry** (fat.c:1481-1513). A second entry would briefly give one chain two names, and deleting either would free the other's clusters. Hence same directory only, and the new name must fit 8.3.
- **The VFS resolves each path once, "at the front door"** (vfs.c:8-10). The live tree is checked first so a disk file cannot shadow /sys or /bin (sysfs.h:16-17).
- **/sys files are functions** (sysfs.c:1-11). They are never stale, and any program can read, pipe or grep them instead of each state having its own shell command.
- **/bin is never copied to disk** (builtin.c:7-9, README.md:626-630). Otherwise a rebuilt kernel would keep running the copies the first boot wrote.
- **Layout: six directories, seeded once** (layout.c:1-20, 30-36). Checking whether a file exists is not enough, because the user may have deleted it. A generation number recorded on disk means a file is offered once.
- **Black box** (blackbox.h:5-25, blackbox.c:3-15). A static buffer that works before paging and the heap, with three ways out (serial, screen, disk). The disk copy is written only to a volume zelr formatted. Four boot-sector conditions guard it, plus a blank-or-log check on the region (blackbox.c:243-250).

---

## 3. File-by-file detail

### 3.1 include/blockdev.h + kernel/blockdev.c: block layer

Constants (blockdev.h): `SECTOR_SIZE 512` (:4), `BLK_MAX 4` (:19), `BLK_BOOT 0` (:20), `BLK_NONE 0xFFFFFFFFu` (:21).

`blkdev_t` (blockdev.h:25-34). The structure is kept, not copied, so it must outlive the registration.

| field | meaning |
|---|---|
| `const char *driver` | "ahci", "nvme", "ata", "usb" |
| `bool (*read)(u32 lba,u32 count,void*)` | driver read, count ≤ max_run |
| `bool (*write)(u32,u32,const void*)` | driver write |
| `bool (*flush)(void)` | cache flush |
| `u32 (*sectors)(void)` | total sectors (512-byte units assumed) |
| `u32 (*max_run)(void)` | largest count per request |
| `const char *(*model)(void)` | model string |
| `bool removable` | false for built-in drivers, true for USB |

There is **no sector-size field**. The whole layer assumes 512 bytes.

State: `static const blkdev_t *devices[BLK_MAX]` (blockdev.c:18).

Functions:
- `u32 blk_register(const blkdev_t*)` (:20-29): the first free slot, or `BLK_NONE`. If no boot disk was found, slot 0 is free, so a USB stick becomes disk 0 and "the boot disk" for every legacy call.
- `void blk_unregister(u32 id)` (:31-33): clears the slot.
- `blk_count` (:35-40) returns the highest used id + 1. It is unused in the tree, as is `blk_device_removable`.
- Accessors `blk_device_present/sectors/model/driver/removable` (:46-66). Defaults for a missing device: 0, "", "none", false.
- `bool blk_read_on(u32 id,u32 lba,u32 count,void *buf)` (:78-94). Refuses a missing device, a missing read, or count 0. `run = max_run ? max_run() : count`; refuses run 0. Loops in chunks of `min(count, run)` and advances `buf` by `n*SECTOR_SIZE`. Stops at the first failure, so a partial transfer returns false.
- `blk_write_on` (:96-112): same shape.
- `blk_flush_on(id)` (:114-117): `d->flush()`, or false. Its only caller is `blk_flush` (BLK_BOOT).
- Static descriptors (:121-132): `AHCI_DEV {"ahci", …, false}`, `NVME_DEV`, `ATA_DEV`.
- `bool blk_init(void)` (:134-144): `ahci_init()`, else `nvme_init()`, else `ata_init()`. Sets `devices[BLK_BOOT] = found`, which may be null.
- Boot-disk wrappers (:146-157): `blk_present/sectors/model/driver/read/write/flush` all use `BLK_BOOT`.

USB registration (other area, for context): usbdisk.c:248-251 has `USB_DEV = {"usb", usbdisk_read, usbdisk_write, usbdisk_flush, usbdisk_sectors, usbdisk_max_run, usbdisk_model, true}`. `usbdisk_attach` calls `blk_register` (usbdisk.c:279) and `usbdisk_detach` calls `blk_unregister` (:289). `usbdisk_max_run` is 8 (:243). `usbdisk_flush` returns `attached` and does nothing (:246). usbdisk honours the device's READ CAPACITY block size (`sector_bytes`, :193) and transfers `count*sector_bytes` (:214). The block layer does not know that size (see §10).

### 3.2 include/ata.h + kernel/ata.c: ATA PIO

- Ports (ata.c:12-21): DATA 0x1F0, ERROR 0x1F1, SECCOUNT 0x1F2, LBA_LO/MID/HI 0x1F3-5, DRIVE 0x1F6, STATUS/COMMAND 0x1F7, CONTROL 0x3F6. Status bits (:23-28): ERR 0x01, DRQ 0x08, SRV 0x10, DF 0x20, RDY 0x40, BSY 0x80. Commands (:30-33): READ 0x20, WRITE 0x30, FLUSH 0xE7, IDENTIFY 0xEC.
- State: `present`, `total_sectors`, `char model[41]` (:35-37).
- `delay400` (:41-43) reads the alternate status 4 times (about 400 ns). `wait_not_busy` (:45-51) spins up to 100,000,000 status reads. `wait_drq` (:53-60) fails on ERR and succeeds on !BSY && DRQ.
- `ata_init` (:62-104): CONTROL=0x02 (nIEN, polled), DRIVE=0xA0 (master). A floating bus reads 0xFF and means absent (:72). IDENTIFY; status 0 means no drive (:81). Non-zero LBA_MID/HI after IDENTIFY means ATAPI, which is refused (:85). Reads 256 words. `total_sectors = id[61]<<16 | id[60]` is **LBA28 only** (:92), so at most 0x0FFFFFFF sectors (128 GiB). The model comes from words 27-46 byte-swapped, trailing spaces trimmed. `present = total_sectors > 0`.
- `ata_max_run` returns 255 (:111): an 8-bit count where 0 would mean 256, which is not used.
- `select_lba` (:114-120): DRIVE = 0xE0 | lba[27:24].
- `ata_read` (:122-137): checks count 1..255 and `lba+count <= total_sectors`. One READ command, then per sector `wait_drq`, 256 `inw`, `delay400`.
- `ata_write` (:139-155): per-word `outw` ("No rep outsw here: some controllers need a moment between words"), then **`return ata_flush()`**, so every write command is followed by FLUSH CACHE.
- `ata_flush` (:157-164): DRIVE=0xE0, CMD 0xE7, waits for !BSY.

### 3.3 include/ahci.h + kernel/ahci.c: AHCI

Constants (:23-40): `SATA_SIG_ATA 0x00000101`, `HBA_PORT_DET_PRESENT 3`, `HBA_PORT_IPM_ACTIVE 1`, PxCMD bits ST 0x0001, FRE 0x0010, FR 0x4000, CR 0x8000. ATA commands READ DMA EXT 0x25, WRITE DMA EXT 0x35, IDENTIFY 0xEC. `ATA_DEV_BUSY 0x80`, `ATA_DEV_DRQ 0x08`, `FIS_TYPE_REG_H2D 0x27`.

Structures, laid out per the specification:
- `hba_port_t` (:42-49), volatile, 0x80 bytes: clb, clbu, fb, fbu, is, ie, cmd, rsv0, tfd, sig, ssts, sctl, serr, sact, ci, sntf, fbs, rsv1[11], vendor[4].
- `hba_mem_t` (:51-56): cap, ghc, is, pi, vs, ccc_ctl, ccc_pts, em_loc, em_ctl, cap2, bohc (to 0x2C); `rsv[0xA0-0x2C]`; `vendor[0x100-0xA0]`; `ports[32]` at 0x100.
- `hba_cmd_header_t` (:58-65), 32 bytes: `cfl_a_w_p` (CFL in the low 5 bits, A, W bit 6, P), `r_b_c_pmp`, `prdtl`, `volatile prdbc`, `ctba`, `ctbau`, `rsv1[4]`.
- `hba_prdt_entry_t` (:67-70): dba, dbau, rsv0, `dbc_i` (byte count - 1 in the low 22 bits, I in bit 31).
- `hba_cmd_tbl_t` (:72-77), 256 bytes: `cfis[64]`, `acmd[16]`, `rsv[48]`, `prdt[8]`. Only `prdt[0]` is used.
- `fis_reg_h2d_t` (:79-87), 20 bytes.

State (:89-99): `hba`, `port`, `port_no=-1`, `present`, `total_sectors`, `model[41]`, `cmd_list`, `cmd_tbl`, `fis_area`, `dma_buf` (a 4 KiB bounce buffer).

- `ahci_max_run` returns 8 (:106): "One PRDT entry pointing at one staging buffer, and that buffer is four kilobytes."
- `alloc_aligned(bytes, align)` (:112-118): `kmalloc(bytes+align)`, rounds up, zeroes. The raw pointer is never freed.
- `stop_port` (:120-128): clears ST and FRE, then spins up to 1,000,000 while FR or CR is set. `start_port` (:130-134): **unbounded** `while (cmd & CR)`, then sets FRE and ST.
- `find_slot` (:137-142): the first bit clear in `sact|ci`. Every slot shares the one `cmd_tbl`, which is fine because commands are synchronous.
- `wait_done(slot)` (:144-150): up to 20,000,000 spins until `ci` clears. `is` bit 30 (TFES) means failure.
- `run_command(command, lba, count, write, bytes)` (:154-195): `is=~0`; picks a slot; header CFL=5 dwords (+W for writes), prdtl=1, ctba/ctbau; zeroes cmd_tbl; `prdt[0]` = dma_buf with `bytes-1 | 1<<31`. The FIS is H2D with C=1, command, 48-bit LBA, device=1<<6 (LBA), and count. It waits up to 10,000,000 spins for TFD !BSY !DRQ (and continues regardless), writes `ci = 1<<slot`, then `wait_done`.
- `ahci_read`/`ahci_write` (:197-214): count 1..8, bounds check, copy through `dma_buf`.
- **`ahci_flush` returns `present`** (:216), with the comment "DMA writes are already through". No FLUSH CACHE is sent (§10).
- `identify` (:218-239): IDENTIFY into dma_buf. Uses the 48-bit count from words 100-103 if non-zero, else words 60-61, clamped to 0xFFFFFFFF (2 TiB). The logical sector size (words 106/117-118) is not examined.
- `ahci_init` (:241-298): `pci_find_class(0x01,0x06,0x01)`, bus master, ABAR = BAR5 (config 0x24) & ~0xF, `paging_map_device(abar, 0x2000)`, GHC.AE (bit 31). It takes the **first** implemented port with DET=3, IPM=1 and sig 0x101 (skipping ATAPI and port multipliers). Then `stop_port`, allocations (cmd_list 1 KiB aligned 1024, FIS 256/256, cmd_tbl aligned 128, dma_buf 4 KiB/4096), CLB/FB programmed, SERR/IS cleared, IE=0 (polled), `start_port`, `identify`. There is no HBA reset, no BIOS/OS handoff (BOHC) and no CAP.S64A check; the upper address halves are always written.

### 3.4 include/nvme.h + kernel/nvme.c: NVMe

Registers (nvme.c:24-31): CAP 0x00 (64-bit), VS 0x08, CC 0x14, CSTS 0x1C, AQA 0x24, ASQ 0x28, ACQ 0x30, doorbells at 0x1000. `CC_ENABLE` bit 0, `CSTS_READY` bit 0, `CSTS_FATAL` bit 1. Admin opcodes: CREATE_SQ 0x01, CREATE_CQ 0x05, IDENTIFY 0x06. I/O opcodes: FLUSH 0x00, WRITE 0x01, READ 0x02. `QUEUE_ENTRIES 64` (both pairs), `IO_QID 1`, `MAX_SECTORS_PER_RUN 8` (one 4 KiB page).

Types:
- `sqe_t` (:55-65), 64 bytes: opcode, flags, cid, nsid, reserved(u64), metadata, prp1, prp2, cdw10..cdw15.
- `cqe_t` (:67-74), 16 bytes: result, reserved, sq_head, sq_id, cid, status (phase in bit 0, the status code above it).
- `queue_t` (:76-83): `sqe_t *sq; cqe_t *cq; u16 sq_tail, cq_head; u8 phase; u16 qid`. `cq` is **not volatile** (§10, latent).

State (:85-94): `regs`, `admin`, `io`, `buffer` (a 4 KiB transfer page), `doorbell_stride`, `timeout_ms`, `total_sectors`, `sector_shift=9`, `present`, `model[41]`, `next_cid=1`.

- `rd64`/`wr64` access the two halves low first (:105-114): "a 64 bit MMIO read is not something every chipset handles".
- `doorbell(qid, completion)` (:119-121) = `0x1000 + (2*qid + completion) * doorbell_stride`. The stride is `4 << CAP.DSTRD` (bits 35:32) (:261), because assuming adjacent doorbells "silently rings the wrong bell" on some hardware.
- `submit(q, cmd)` (:141-167): assigns `cid` (never 0) and copies to `sq[sq_tail]`. Tail = (tail+1) % 64, then rings the SQ doorbell. Deadline = `timer_ticks() + timeout_ms/10 + 2`, which assumes 100 Hz. It polls `cq[cq_head].status & 1 == q->phase`. On a hit it takes `code = status >> 1`, advances the head, flips the phase on wrap, rings the CQ doorbell and returns `code == 0`. It fails on CSTS.CFS or the deadline. The cid is **not** compared, and SQ-full is not checked (one command in flight).
- `wait_ready(want)` (:169-178): polls CSTS.RDY with the same deadline and CFS check.
- `make_queues` (:182-201): four 4 KiB-aligned queue arrays plus `buffer`. `virt_to_phys` is checked for admin.sq, io.sq and buffer only, not the two CQs. Both phases start at 1.
- `identify(cns, nsid, out)` (:203-213): the admin IDENTIFY lands in `buffer`, then is copied.
- `create_io_queues` (:215-234): CQ first ("a submission queue names the completion queue its results go to"). `cdw10 = (63<<16) | 1`; `cdw11 = 1` (PC, no interrupts). Then the SQ with `cdw11 = (1<<16) | 1`.
- `nvme_init` (:236-326):
  1. `pci_find_class(0x01,0x08,0x02)`, bus master, 64-bit BAR0+BAR1 ("the firmware puts the controller above four gigabytes"), `paging_map_device(base, 0x2000)`.
  2. CAP: DSTRD gives the stride; TO×500 ms gives `timeout_ms`, 30000 if 0 or larger. MQES+1 must be ≥ 64. CAP bit 37 (NVM command set) is required.
  3. CC.EN=0, wait RDY=0. `make_queues`. AQA = (63<<16)|63. ASQ, ACQ.
  4. CC = MPS 0 (4 KiB) | CSS 0 | IOSQES 6 | IOCQES 4 | EN, then wait RDY=1.
  5. Identify controller (CNS 1): the model is bytes 24..63, trimmed. `static u8 ident[4096]`.
  6. `create_io_queues`.
  7. Identify namespace 1 (CNS 0): NSZE is the u64 at offset 0, FLBAS = byte 26 & 0xF, LBAF at 128+4×flbas, LBADS = bits 23:16. LBADS must be 9..12 **and exactly 9**; "a namespace formatted with 4096 byte blocks ... is refused rather than silently misread". NSZE is clamped to 0xFFFFFFFF.
  - CAP.MPSMIN is not checked. Only namespace 1 is ever used ("the first is what a disk has").
- `transfer` (:330-349): count 1..8, overflow-safe bounds check (`lba > total || count > total-lba`). PRP1 = buffer, CDW10 = lba, CDW11 = 0, CDW12 = count-1. Copies in and out of `buffer`.
- `nvme_flush` (:359-366): a real I/O FLUSH on nsid 1.

### 3.5 include/parts.h + kernel/parts.c: partition tables

Constants: `PARTS_MAX 16`, `PART_NAME_MAX 37`. `part_scheme_t`: NONE, MBR, GPT (parts.h:25-29).

`part_t` (parts.h:31-38):

| field | meaning |
|---|---|
| `u32 start` | first sector (absolute LBA on disk 0) |
| `u32 sectors` | length |
| `u8 mbr_type` | MBR type byte, 0 for GPT |
| `bool efi_system` | GPT type GUID = ESP, or MBR type 0xEF |
| `bool looks_like_fat` | the first sector passes `sector_looks_like_fat` |
| `char name[37]` | GPT name (ASCII, '?' otherwise) or MBR type description |

State: `table[16]`, `count`, `scheme`, `char gpt_error[64]` (parts.c:37-40).

- `u32 crc32(const void*, u32)` (:24-33): bitwise IEEE (poly 0xEDB88320). Its only callers are here.
- `sector_looks_like_fat(lba)` (:68-79): reads **disk 0** (`blk_read`). Needs 0x55AA, BytsPerSec == 512, SecPerClus a power of two and non-zero, NumFATs non-zero. The comment says this is "the same evidence fat_mount goes on".
- `add()` (:81-93): stops at 16 and truncates the name to 36 characters.
- `gpt_header_t` (:97-112), packed, 92 bytes: sig[8], revision, header_size, header_crc, reserved, my_lba, alternate_lba, first_usable, last_usable, disk_guid[16], entry_lba, entry_count, entry_size, entry_crc.
- `gpt_entry_t` (:114-121): type_guid[16], unique_guid[16], first_lba, last_lba, attributes, `u16 name[36]`.
- `GUID_EFI_SYSTEM` (:123-126): C12A7328-F81F-11D2-BA4B-00A0C93EC93B in on-disk byte order.
- `read_gpt` (:143-229), header at LBA 1 only. **The backup GPT is never consulted.** Checks:
  - "EFI PART", otherwise silently "not GPT".
  - `92 ≤ header_size ≤ 512` (error "header size %d").
  - CRC over `header_size` bytes with the CRC field zeroed in a copy (error "header checksum").
  - `128 ≤ entry_size ≤ 4096` ("entry size").
  - `1 ≤ entry_count ≤ 256` ("entry count").
  - `2 ≤ entry_lba ≤ 0xFFFFFFFF` ("entry array out of reach").
  - `array_bytes = count*size ≤ 32 KiB` (a `static u8 array[256*128]`, error "entry array too large").
  - The array is read ("entry array unreadable") and its CRC checked ("entry array checksum").
  - Entries: skip a zero type GUID, `last < first`, anything past 32 bits, and anything outside `blk_sectors()`. The name comes from UTF-16 (outside 0x20..0x7E becomes '?'), or "partition N". `add(..., esp = type==ESP GUID)`.
  - Success sets `scheme = GPT` **even when no entries survived**.
- `mbr_type_name` (:233-245): 0x01 fat12; 0x04/06/0E fat16; 0x0B/0C fat32; 0x07 "ntfs or exfat"; 0x83 linux; 0x82 linux swap; 0xEF efi system; 0x05/0F extended; otherwise unknown.
- `read_mbr` (:247-283): needs 0x55AA. Four entries at 446+16i. Skips type 0, **0xEE** (protective, "Taking it at face value would hand out the disk as one big partition"), start or sectors of 0, out of range, and extended 0x05/0x0F (the chain is not followed). Returns true only if at least one was added (then `scheme = MBR`).
- `parts_scan` (:287-302): resets state; returns if no disk 0; `read_gpt() || read_mbr()`; otherwise NONE. **Only disk 0 is scanned.** USB sticks have their own minimal MBR parse in diskfs.c.

### 3.6 include/diskfs.h + kernel/diskfs.c: choosing, preparing and repairing the volume

- `FAT_TYPES[] = {0x01,0x04,0x06,0x0B,0x0C,0x0E,0xEF}` (:44), used for USB MBR entries only. It includes 0xEF (ESP) and 0x01 (FAT12, which mount then refuses).
- `bool diskfs_mount_removable(u32 dev)` (:52-78):
  1. Reads sector 0 of `dev`. If 0x55AA, each of the 4 MBR entries with `start != 0` and a type in `FAT_TYPES` is tried with `fat_mount_on(FAT_VOL_USB, dev, start)`. Logs "usb volume mounted from partition %d at sector %d".
  2. Otherwise `fat_mount_on(FAT_VOL_USB, dev, 0)` ("usb volume mounted from the whole device").
  3. Otherwise "usb disk has no filesystem this kernel reads".
  - There is no GPT support for sticks: a GPT stick's protective 0xEE entry is not in the list, and the whole-device mount of LBA 0 fails.
- `diskfs_unmount_removable` = `fat_forget_volume(FAT_VOL_USB)` (:80-82). `diskfs_removable_mounted` (:84) is unused.
- `diskfs_available` = `blk_present()`; `diskfs_mounted` = `fat_mounted()` (**the selected volume**); `diskfs_flush` = `blk_flush()` (disk 0) (:86-88).
- `bool diskfs_format(void)` (:90-101): refuses without disk 0 or if `parts_count() > 0` (kprintf "  fs      refusing to format: the disk is partitioned", bb_log "fs refused to format a partitioned disk"). Otherwise `fat_format("ZELR")`.
- `finish_mount` (:103-119): `fat_boot_repair()` (kprintf "  fs      boot sector repaired, this disk was made without one" and bb_log "fs boot sector repaired: the mark was there and no program behind it"), then `fat_reclaim()` (kprintf only: "  fs      reclaimed %d cluster(s) from an unclean shutdown"). Returns `fat_count("/")`.
- `int diskfs_mount(void)` (:121-174):
  - Returns -1 with no disk 0, which cannot happen from main.c because it is only called when `blk_init()` succeeded.
  - `parts_scan()`; if `parts_gpt_error()` is non-empty, bb_log "parts gpt rejected: %s".
  - If `parts_count()==0`: bb_log "parts none, treating the disk as one volume"; `fat_mount_at(0)`, otherwise **-2**; then `finish_mount`.
  - Otherwise bb_log "parts %s, %d partition(s)". **Two passes** over partitions with `looks_like_fat`. The ESP is skipped (logged in pass 0: "parts skipping partition %d (%s): efi system partition"). Pass 0 accepts only `fat_is_zelr_volume()`; pass 1 accepts the first that mounts. Then kprintf "  fs      fat16 on partition %d (%s)%s" (always says fat16) and bb_log "fs mounted partition %d at lba %d, %s" (ours / not ours), then `finish_mount`.
  - Nothing usable: bb_log "fs no usable fat partition among %d", **-2**.

### 3.7 include/fat.h + kernel/fat.c: FAT16/FAT32

#### Public constants (fat.h)
`FAT_PATH_MAX 128` (:4), `FAT_NAME_MAX 64` (:9; 63 characters plus NUL), `FAT_VOLUMES 2`, `FAT_VOL_DISK 0`, `FAT_VOL_USB 1` (:13-15).

#### Private constants (fat.c)
- ATTR (:35-41): RO 0x01, HIDDEN 0x02, SYSTEM 0x04, VOLUME_ID 0x08, DIRECTORY 0x10, ARCHIVE 0x20, LFN 0x0F.
- `ENT_FREE 0x00`, `ENT_DELETED 0xE5` (:43-44).
- EOC (:49-52): EOC16_MIN 0xFFF8, EOC16 0xFFFF, EOC32_MIN 0x0FFFFFF8, EOC32 0x0FFFFFFF. FAT32 entries are 28 bits and the top 4 bits are preserved.
- `ZELR_VOLUME_ID 0x5A4C5200` (:438): ASCII "ZLR" over a zero.
- LFN: `ATTR_LFN_MASK 0x3F`, `LFN_CHARS 13`, `LFN_LAST 0x40` (:936-938).

#### On-disk structures
- `dirent_t` (:54-65), packed 32 bytes: `name[11]`, attr, nt_reserved, create_tenth, create_time, create_date, access_date, `cluster_hi` (offset 20, zero on FAT16), write_time, write_date, `cluster_lo` (offset 26), size.
- `lfn_t` (:940-949): seq, part1[10] (5 UTF-16), attr, type, checksum, part2[12] (6), `u16 cluster` (0), part3[4] (2).
- BPB fields read by offset in `fat_mount_at` (:360-372, 407): 11 BytsPerSec, 13 SecPerClus, 14 RsvdSecCnt, 16 NumFATs, 17 RootEntCnt, 19 TotSec16, 22 FATSz16, 32 TotSec32, 36 FATSz32, 44 RootClus.
- Boot-sector fields written by `fat_format_at` (:687-729):
  - Common: jump `EB (code_at-2) 90`; OEM "ZELR    "; BytsPerSec 512; SecPerClus; Rsvd; NumFATs 2; RootEntCnt (512 or 0); TotSec16 0; Media 0xF8; FATSz16 (FAT16 only); SecPerTrk 32; Heads 8; HiddSec 0; TotSec32 = total.
  - FAT32: FATSz32, ExtFlags 0, FSVer 0, RootClus 2, FSInfo 1, BkBootSec 6, DrvNum 0x80 @64, BootSig 0x29 @66, VolID @67, label @71, "FAT32   " @82.
  - FAT16: DrvNum 0x80 @36, BootSig 0x29 @38, VolID @39, label @43, "FAT16   " @54.
  - Then the stub at `code_at` (0x3E for FAT16, 0x5A for FAT32) and 0x55AA.
- FSInfo, FAT32 only (:744-755): 0x41615252 @0, 0x61417272 @484, free count and next free both 0xFFFFFFFF ("unknown"), 0xAA550000 @508. Written at sectors 1 and 7, the boot backup at 6. zelr never updates FSInfo afterwards.

#### Types and state
- `dir_t {bool root; u32 cluster;}` (:68-71); `ROOT = {true, 0}` (:221).
- `fatvol_t` (:87-113), one per volume:

| field | meaning |
|---|---|
| `dev` | block device id used by `vol_read/vol_write` |
| `part_base` | LBA of the volume start on `dev` |
| `part_sectors` | bound on volume-relative LBAs; 0 = unbounded (during mount, and format of a whole disk) |
| `fat_bits` | 16 or 32 |
| `root_cluster` | FAT32 root chain start (0 on FAT16) |
| `mounted` | |
| `bytes_per_sector, sectors_per_cluster, reserved_sectors, num_fats, root_entries, total_sectors, fat_sectors` | BPB copies |
| `fat_start, root_start, root_sectors, data_start, cluster_count` | derived layout |
| `alloc_hint` | next-fit allocation start (initially 2) |
| `fat_cache_lba, fat_cache_valid, fat_cache[512]` | a one-sector FAT cache (write-through) |

- `static fatvol_t volumes[2] = {{.fat_bits=16,.alloc_hint=2}, …}` (:115-118).
- `static u32 current_volume = FAT_VOL_DISK` (:123). Macros (:143-164) map the old global names (`part_base`, `mounted`, `fat_cache`, …) onto `volumes[current_volume]`, so the rest of the file "did not have to change".
- Two shared static buffers (:192-193): `u8 sec[512]` (file data and boot sector) and `u8 dsec[512]` (directory entries). The comment says "Three buffers"; the third is the per-volume `fat_cache`.

#### Volume selection
- `fat_select(vol)` (:125-127): sets `current_volume` if valid. `fat_selected` (:129) is unused. `fat_mounted_on(vol)` (:131-133).
- `fat_forget_volume(vol)` (:135-141): clears mounted and the cache, resets fat_bits to 16 and hint to 2.
- `fat_mount_on(vol, dev, base)` (:339-347): saves `current_volume`, selects `vol`, sets `volumes[vol].dev`, calls `fat_mount_at`, then restores the selection.

#### Low level
- `vol_read/vol_write(lba,count,buf)` (:168-178): bounded by `part_sectors` when non-zero, then `blk_read_on/blk_write_on(CUR.dev, part_base+lba, …)`.
- `fat_get(cluster)` (:262-269): `fat_cache_load(fat_start + off/512)`. **A failed read returns `eoc()`**, which silently truncates the chain. FAT32 values are masked to 28 bits.
- `fat_set(cluster, value)` (:271-297): loads the sector and updates it (FAT32 keeps the top 4 bits). It then writes that one sector to **every FAT copy** (`lba + copy*fat_sectors`). On failure it invalidates the cache and returns false.
- `alloc_cluster()` (:299-310): next-fit from `alloc_hint`, wrapping once over `cluster_count`. Marks EOC through `fat_set` and advances the hint. Returns 0 when the volume is full.
- `free_chain(c)` (:312-320): walks and zeroes entries (the `fat_set` result is ignored) and moves the hint back.
- `cluster_lba(c) = data_start + (c-2)*spc` (:322-324). `zero_cluster(c)` (:326-332) uses `sec`.

#### Mount
`bool fat_mount_at(u32 base_lba)` (:349-429), on the selected volume:
1. `mounted=false`, `fat_forget()`, device present?, `part_base=base`, `part_sectors=0`, read sector 0, check 0x55AA.
2. Read the BPB. `fat_sectors = FATSz16 ? FATSz16 : FATSz32`; `total = TotSec16 ? TotSec16 : TotSec32`.
3. Refuse BytsPerSec ≠ 512, and spc/NumFATs/FATSz of 0.
4. FAT16-style layout: `fat_start = rsvd`, `root_sectors = ceil(root_entries*32/512)`, `root_start = fat_start + nfats*fatsz`, `data_start = root_start + root_sectors`, `cluster_count = (total-data_start)/spc`.
5. `cluster_count < 4085` is refused (FAT12). `fat_bits = cluster_count > 65524 ? 32 : 16`.
6. FAT32: RootEntCnt must be 0; the layout is recomputed with no root region; RootClus (@44) must be in `[2, cluster_count+2)`. FAT16: RootEntCnt must be non-zero.
7. `total != 0`; `part_sectors = total`. **Disk bound: `u32 disk = blk_sectors();` (disk 0!)** Refuses if `part_base >= disk || total > disk - part_base` (:421-425).
8. `mounted = true`.

It does not check that the FAT is large enough for `cluster_count`, and does not look at FSInfo or ExtFlags.

- `fat_mount()` = `fat_mount_at(0)` (unused). `fat_type()` returns 16/32, or 0 if unmounted. `fat_base()` returns `part_base`.

#### Ownership
- `fat_is_zelr_volume()` (:443-448): re-reads sector 0 of the mounted volume and calls `fat_boot_is_ours`.
- `boot_is_fat32(boot)` (:459-461): RootEntCnt == 0 && FATSz16 == 0.
- **`fat_boot_is_ours(boot)`** (:463-468): 0x55AA && OEM == "ZELR    " && the u32 at 67 (FAT32) or 39 (FAT16) == 0x5A4C5200.
- `fat_boot_log_lba(boot)` (:475): 8 on FAT32 (after the boot, FSInfo and backups at 6/7), else 1.
- `fat_boot_reserved(boot)` (:477): RsvdSecCnt.

#### Format
- `fat_layout(total, spc, bits, reserved, fats, roots, &fsize, &clusters)` (:492-526): iterates up to 16 times solving FAT size against cluster count; if it doesn't settle, it rounds up.
- `fat_boot_stub(sec, at)` (:549-578): the **25-byte real-mode program**: `FA` cli; `31 C0` xor ax,ax; `8E D8` mov ds,ax; `BE lo hi` mov si,msg; `AC` lodsb; `84 C0` test al,al; `74 09` jz stop; `B4 0E` mov ah,0Eh; `BB 07 00` mov bx,7; `CD 10` int 10h; `EB F2` jmp next; `F4` hlt; `EB FD` jmp stop. The message "This disk holds files. Start the machine from its installer instead.\r\n" (71 bytes with NUL) follows the code. `si` is patched to `0x7C00 + at + 25`. The stub is skipped if it would not fit before 510. The branch offsets check out (jz to 22, jmp next to 8, jmp stop to 22).
- `fat_boot_repair()` (:602-627): mounted; sector 0 is ours (0x55AA + OEM + serial); `boot[code_at] == 0` (code_at 0x3E or 0x5A), meaning "the jump lands on nought". It then rewrites bytes 0-2 as `EB (code_at-2) 90`, writes the stub and writes sector 0. On FAT32 it also writes the backup at BkBootSec if `0 < spare < reserved_sectors`. It is idempotent: a sector that already carries the stub fails the zero test. It touches only the jump and the space after the BPB.
- `fat_format_at(base, sectors, label)` (:629-788):
  1. **Requires disk 0 present** (`blk_present()`) but writes through `vol_write`, i.e. **`CUR.dev`**. `fat_forget()`; `part_base = base`; `part_sectors = sectors`.
  2. `total = sectors ? sectors : blk_sectors() - base` (disk-0 size). Refuses `total < 8192`.
  3. FAT16 first: spc 4,8,16,32,64 (2-32 KiB clusters), reserved = 1 + 32 = **33**, 2 FATs, 512 root entries. Accept clusters in [4085, 65524].
  4. Otherwise FAT32: starting spc 8 (≤ 8 GiB), 16 (≤ 16 GiB), 32 (≤ 32 GiB) or 64, doubling up to 128. Reserved = 8 + 32 = **40**. Accept clusters in [65525, 0x0FFFFFF5].
  5. Write the boot sector. Zero sectors 1..reserved-1 ("leaving a previous volume's log there would either be read back as this volume's own history or block the log entirely"). FAT32: FSInfo at 1 and 7, boot copy at 6.
  6. Zero both FATs. FAT[0..1] (and FAT32 FAT[2]=EOC for the root). Zero the root (FAT16: 32 sectors; FAT32: cluster 2).
  7. `blk_flush()`, then `fat_mount_at(base)`.
  - `fat_format(label)` = `fat_format_at(0, 0, label)` (:790).

#### Directories
- `entries_per_cluster` (:794). `dir_capacity(d)` (:798-806): FAT16 root gives `root_entries`; otherwise the chain length × entries per cluster, loop-guarded.
- `ent_cluster/set_ent_cluster` (:816-825): FAT32 combines hi and lo; FAT16 writes hi = 0.
- `dir_locate(d, index)` (:827-855): FAT16 root is a fixed region; otherwise it walks the chain (guarded).
- `dir_read` (:857-863) reads into `dsec`. `dir_write` (:865-871) is a read-modify-write of one sector through `dsec`.
- `dir_grow(d)` (:876-892): the FAT16 root cannot grow. Finds the last cluster, allocates, zeroes and links it.
- `dir_free_slot(d)` (:895-908): the first entry whose `name[0]` is 0x00 or 0xE5; grows once.
- `entry_is_real(e)` (:911-916): not free or deleted, not LFN, not a volume ID.

#### Long file names
- `short_checksum` (:951-956): the standard rotate-right sum.
- `long_name_of(d, index, shortent, out, cap)` (:985-1029):
  - Walks backwards from `index-1` while `(attr & 0x3F) == 0x0F`.
  - Each entry's checksum must match the short name (otherwise 0: "left over from another name").
  - `seq & 0x1F` must be non-zero. `at = (seq-1)*13`; `at ≥ 63` returns 0, so names over 63 characters fall back to 8.3.
  - Code units 0x0000 and 0xFFFF are skipped. **Anything ≥ 0x80 becomes '?'** ("rather than half of a character nobody can type").
  - Stops at `LFN_LAST`. Returns 0 unless complete, non-empty and `len < cap`.
  - Sequence continuity is not verified.
- `same_name(a,b)` (:1031-1037): case-insensitive ASCII compare.
- `needs_long(name)` (:1046-1061): a base over 8 characters, an extension over 3, a space, or more than one dot. **Case alone is not a reason** ("making those long as well would change how existing volumes read"). Characters that are illegal in 8.3 (`+,;=[]`, bytes ≥ 0x80) are not considered.
- `short_taken(d, name11)` (:1063-1073).
- `make_alias(d, name, out)` (:1080-1114): up to 6 upcased characters of the base (spaces dropped, stopping at the first '.'), or 'X' if none. Then `~N` for N = 1..999, and up to 3 characters from the **last** dot. It checks `short_taken`. If all 999 are taken it returns a duplicate (the last candidate).
- `dir_free_run(d, want)` (:1117-1133): consecutive free or deleted slots; grows once.
- `dir_put_name(d, name, e)` (:1139-1183): an 8.3 name takes `dir_free_slot` and `to_83`. A long name (`chars ≤ 63`) takes `n = ceil(chars/13)` LFN entries plus one short. It finds a free run of n+1, makes the alias and checksum, and writes the LFN entries **nearest-first as seq 1** (the furthest is `n | 0x40`). Characters are written as `(u16)(u8)name[pos]`, then 0x0000 and 0xFFFF padding. It returns the short slot, **which the caller writes** (the commit).
- `dir_drop_long(d, index, shortent)` (:1187-1200): marks the preceding LFN entries with a matching checksum as 0xE5, down to LFN_LAST.
- **`dir_find(d, name, out)`** (:1202-1222): for each real entry, first `memcmp(e.name, to_83(name))` (**truncating** 8.3), then the long name via `same_name`. It stops at the first 0x00 entry. See §10 for the aliasing bug.

#### Paths
- `next_component` (:1229-1239) collapses slashes.
- `resolve_parent(path, &parent, leaf)` (:1243-1286): descends all but the last component. It has "." and ".." handling, but `dir_find(…, "..")` can never match (§10); this is unreachable through the VFS.
- `resolve_dir` (:1289-1313).

#### File operations
- `fat_list(path, index, name, size, dir)` (:1317-1351): returns -1 if unmounted or not a directory, 0 past the end, 1 on a hit. Skips non-real entries and names starting with '.'. The name is the long name if present, else `from_83` (lowercased). **`strncpy(name_out, real, 63)` does not terminate a 63-character name.**
- `fat_count(path)` (:1353-1357): calls `fat_list(i)` until it fails, which is O(n²) in directory size.
- `fat_stat` (:1359-1377): "", "." and ".." report a directory.
- `fat_read_file(path, buf, cap)` (:1381-1408): follows the chain for `min(size, cap)` bytes, sector by sector through `sec`. A directory gives -1.
- **`fat_write_file(path, buf, size)`** (:1410-1479): the crash-safe protocol. See §4.3.
- `fits_83(name)` (:1517-1523): the name survives to_83 then from_83 unchanged, case-insensitively.
- **`fat_rename(from, to)`** (:1525-1571): both parents are resolved and must be identical (`cluster` and `root`). The destination leaf must `fits_83`. Finds the source; returns true if the names match case-insensitively. If the destination exists: refuses a directory, otherwise `fat_delete_file(to)` and re-finds the source. Then `dir_drop_long` (before the rename, "so a power cut here leaves the file under its 8.3 name"), `to_83(dst)` into `e.name`, one `dir_write`, `blk_flush()`. Directories may be renamed.
- `fat_delete_file` (:1573-1590): refuses directories. **`free_chain` first**, then `dir_drop_long`, then `name[0]=0xE5` and `dir_write`, then `blk_flush`.
- `fat_mkdir` (:1594-1640): allocates and zeroes a cluster, writes "." (itself) and ".." (the parent, **0 for the root even on FAT32**, per the spec), flushes, and only then `dir_put_name` plus the entry write (attr DIRECTORY, size 0), then flushes.
- `fat_rmdir` (:1642-1663): refuses names starting with '.'. The directory must be empty (`fat_count(path)`). `free_chain` first, then marks 0xE5. **It never calls `dir_drop_long`.**
- Every written entry gets `write_date = 0x5A21` (2025-01-01) and `write_time = 0`. No RTC is used; create and access dates are 0.

#### Reclaim and statistics
- **`fat_reclaim()`** (:1675-1755): allocates a byte map of `cluster_count+2` and a queue (64 entries, doubling). On FAT32 it marks the root chain first. The comment records that doing otherwise freed the root: "two files in the root, gone". It then walks the directories breadth-first with an explicit queue: every real non-dot entry's chain is marked (loop-guarded), and subdirectories are enqueued. The sweep frees every allocated, unmarked cluster (`fat_get(c) != 0`). If anything was freed it calls `blk_flush()` and resets the hint to 2. Returns the count. See §10 for the failure modes.
- `fat_total_clusters`, `fat_cluster_bytes` (= spc×512), `fat_free_bytes` (:1757-1763, a full FAT scan with a **u32 byte result**).

### 3.8 include/vfs.h + kernel/vfs.c: the namespace

Constants (vfs.h): `VFS_PATH_MAX = FAT_PATH_MAX` (128), `VFS_NAME_MAX = FAT_NAME_MAX` (64), `VFS_MAX_OPEN 16` (**unused**). Open flags: `O_READ 0x01`, `O_WRITE 0x02`, `O_CREATE 0x04`, `O_TRUNC 0x08`, `O_APPEND 0x10` (used by fd.c).

- `vfs_add_builtin` delegates to `sysfs_add_program`; `vfs_builtin_count` to `sysfs_program_count` (:26-30).
- `bool vfs_generated(abs)` (:37-40): `sysfs_owns(abs)` or "/bin" and "/bin/…". fd.c uses it too, to refuse `O_WRITE`.
- `vfs_disk_backed()` = `fat_mounted()` (**the selected volume**) (:42).
- The working directory lives per task in `task_t.cwd`, with a static `cwd_fallback` ("/") when there is no current task (:46-58).
- **`vfs_resolve(in, out, cap)`** (:64-114): relative input is joined with the cwd in a 256-byte work buffer (longer input is silently truncated). Components longer than 63 characters are **silently truncated**. "." is dropped. ".." pops using a `starts[32]` offset stack and stops at the root. Depth over 32 or overflowing `cap` fails. The output never has a repeated or trailing slash. Tested by selftest [paths].
- `ram_list(dir, index, …)` (:121-148): direct children of `dir` among the RAM entries (names are whole paths).
- **`route(abs)`** (:163-171): "/usb" or "/usb/…" selects `FAT_VOL_USB` and returns the remainder (or "/"). Everything else selects `FAT_VOL_DISK` and returns `abs`. **The selection persists** after the call.
- `vfs_list(path, index, …)` (:173-198): resolve (default "."). Generated paths go to `sysfs_list`. At the root, the virtual entries "bin", "sys" (and "usb" if the USB volume is mounted) come first, then the stored ones with a shifted index. Then `route` and `fat_list` if the selected volume is mounted, else `ram_list(abs)`.
- `vfs_count` (:200-204).
- `vfs_stat` (:206-226): "/" is always a directory. Generated paths go to `sysfs_stat`; then fat, else `fs_find`.
- `vfs_read` (:230-244): generated paths go to `sysfs_read`; then fat, else the RAM copy.
- `vfs_write` (:246-254): generated paths are refused; then `fat_write_file`, else `fs_write`. **With no stick mounted, a /usb write goes to the RAM fs.**
- `vfs_append` (:256-271): stat, then `kmalloc(size+len)`, read, concatenate and `vfs_write`. A whole rewrite through the crash-safe path each time.
- `vfs_delete` (:273-281).
- `vfs_rename` (:283-294): resolve both; refuse generated; refuse if `!fat_mounted()` (checked **before routing**, on whatever was selected last); then **`fat_rename(route(a), route(b))`** (§10: cross-volume).
- `vfs_mkdir` / `vfs_rmdir` (:296-317): refuse "/" and generated paths. rmdir also refuses non-empty via `vfs_count`. The RAM fallback is `fs_mkdir`/`fs_delete`.
- `vfs_chdir` (:319-329): the target must stat as a directory ("/" always passes).
- `vfs_slurp(path, &size)` (:331-342): stat, then `kmalloc(size or 1)`, then read. The caller frees.
- `vfs_init()` = `sysfs_init()` (:350-352).

Node types: only file and directory (`bool dir`). There are no links, devices, permissions or timestamps in the API. /sys and /bin are directories; /sys files are files whose size is known by rendering; /usb is a virtual directory while mounted.

### 3.9 include/fs.h + kernel/fs.c: RAM filesystem

- `FS_MAX_FILES 64` (files and directories together), `FS_NAME_MAX 128`.
- `file_t {char name[128]; u8 *data; u32 size, cap; bool used, is_dir;}`. The name is an **absolute path**.
- `fs_init` zeroes the table (main.c:414). `fs_find` is a linear strcmp. `fs_create` finds or takes a free slot. `ensure(f, need)` doubles capacity from 64 bytes via kmalloc and copy.
- `fs_write` replaces the contents (refused for directories). `fs_append` (**unused**). `fs_mkdir` fails if the name exists.
- `fs_delete` frees and clears one entry; it does not cascade, and vfs_rmdir checks emptiness first.
- `fs_count`, `fs_at(i)` (i-th used entry), `fs_bytes_used` (shell `mem`).

### 3.10 include/sysfs.h + kernel/sysfs.c: /sys and /bin

Constants: `SYSFS_MAX_PROGRAMS 48` (sysfs.h:30). It is public because a private limit of 16 once silently dropped the browser, and it is `_Static_assert`ed in builtin.c:126-128. `SYSFS_MAX 4096` (render buffer). `sysfs_render_t = u32 (*)(char *out, u32 cap)`.

Programs: `program_t {name, data, size}`, `programs[48]`, `n_programs`. `sysfs_add_program` silently drops the 49th (:53-59). `program_named` is a linear lookup. `sysfs_init` just resets `n_programs = 0` (:372). It is called by `vfs_init` (main.c:415) **before** `builtin_install` (main.c:444).

`put(o, fmt, …)` (:76-85): formats into a 256-byte line and appends up to `cap-1`, always NUL-terminated.

**Every /sys file** (`nodes[]`, :353-369). All render into `static char scratch[SYSFS_MAX]` (:378).

| path | function | contents |
|---|---|---|
| /sys/version | `render_version` :87 | `name      zelr`, `version   0.37.0`, `mode      x86-64 long mode`, `programs  N built in` |
| /sys/memory | `render_memory` :96 | `frames    T total, U used, F free`; `physical  X KiB total, Y KiB free`; `heap      A KiB of B KiB used` |
| /sys/tasks | `render_tasks` :108 | header `pid  state     ring  slices  name`, then per task `%-4d %-9s %-5s %-7d %s` (ring "3"/"0") |
| /sys/uptime | `render_uptime` :124 | `ticks     N at HZ Hz`; `uptime    Hh Mm Ss`; `tasks     N, B of them blocked`; `wakeups   W, B blocked now` |
| /sys/cpu | `render_cpu` :137 | `described N`; `started   N`; `apictimer N counts per second`; per CPU `cpu%-6d apic A, running/halted S slices, T ticks, L busy` (lock misses) |
| /sys/devices | `render_devices` :153 | `disk      MODEL via DRIVER, N MiB` or `disk      none`; `sound …` (position "timed here"/"from the controller") or none; `network   NAME MAC` or none; `video     WxH 32bpp` or `vga text`; `usb       DESC, N report(s)` or `no controller`; `keyboard  ps/2[ and usb]`; `storage   fat16 on disk` or `memory only` (via `vfs_disk_backed`, the selected volume); optional `usbdisk   MODEL, N sector(s) of B bytes` |
| /sys/net | `render_net` :194 | `no network card`, or address/netmask/gateway/dns, `state configured` or `no address yet`, `packets`, `queue N waiting, D deepest, X dropped`, `tcp R resent, O out of order, S reset` |
| /sys/programs | `render_programs` :215 | `%-16s %d bytes` per program |
| /sys/boot | `render_boot` :232 | this boot's log (`bb_text`), **tail** kept if over cap-1 (4095) |
| /sys/lastboot | `render_lastboot` :248 | `bb_prev(b, cap)`, which is the **head** of the previous log. Or `no record` plus two explanatory lines ("There is one only after a boot that reached the scheduler, or / that panicked, on a volume this kernel formatted.") |
| /sys/clipboard | `render_clipboard` :263 | `clip_get`, or `(empty)` |
| /sys/time | `render_time` :275 | `now       <rtc_format>`; `source    cmos clock` or `none`; `uptime    N s` |
| /sys/screen | `render_screen` :290 | `width`, `height`, `settable 0/1`, `source <fb_backend>`, `frames`, `shared`, `lastkib`, `fullkib`, `sentkib` (for settings.c) |
| /sys/settings | `render_settings` :321 | `# key value low high default label`, then one line per theme knob (`theme_knob_*`) |
| /sys/theme | `render_theme` :339 | `accent 0x…`, `desktop`, `surface`, `text`, `text_dim`, `light 0/1`, `look 0/1` (1 = LOOK_BUILT), `preset N` |

Readers in userland include settings.c (settings, theme, screen, memory, cpu, devices, version, uptime, net) and ui.h (/sys/theme).

- `sysfs_owns(abs)`: "/sys" or "/sys/…" (:380-382). `is_dir_path`: "/sys" or "/bin" (:385-387).
- `bin_name(abs)`: "/bin/NAME" with no further slash (:396-402).
- `sysfs_stat` (:404-427): a node's size is found by **rendering** it; a program's size is its image size.
- `sysfs_read` (:429-447): renders and copies `min(len, cap)`, or copies the program bytes straight from the kernel image.
- `sysfs_list` (:449-467): lists "/sys" (15 nodes, each rendered for its size) and "/bin". Any other path returns 0, not -1, so `ls /sys/version` prints "(empty)".

**/bin listing.** builtin.c:70-117, 46 programs in this order: hello, count, fptest, alloctest, jstest, forktest, wintest, term, spawntest, settings, paint, files, notes, monitor, calc, music, browser, sh, echo, cat, ls, wc, grep, fdtest, pagetest, spin, ps, pngtest, jpegtest, svgtest, layouttest, wiretest, jsprobe, blackjack, poker, cardtest, sleeptest, halfdrawn, cowtest, argvtest, sigtest, faulttest, maptest, crashwrite, durtest, polltest. That leaves 2 free slots before the static assert fires.

### 3.11 include/layout.h + kernel/layout.c: directory layout

- `DIRS[] = {"/home","/doc","/cfg","/tmp"}` (:27). Together with the generated /bin and /sys these are the "six directories" (:1-16).
- `SEED_GENERATION 1` (:37). `SEED_MARKER "/cfg/seeded"` (:38). `static u32 seeded_through`.
- `seed(gen, path, text)` (:42-46): skips if `gen <= seeded_through` ("already offered once") or if the path exists ("the user has their own"); otherwise `vfs_write` (the result is ignored).
- `read_marker` (:48-58): leading decimal digits of /cfg/seeded, else 0. `write_marker` (:60-64): "%d\n".
- `empty_tmp` (:68-77): up to 256 iterations of "list /tmp index 0, then `vfs_delete` or `vfs_rmdir`". It **breaks at the first entry it cannot remove**.
- `layout_init` (:79-148): mkdir each missing DIR; `empty_tmp()`; read the marker; seed the three files; write the marker **last** only if `seeded_through < SEED_GENERATION` ("so a machine that loses power partway through seeding tries again next time").
- Seeded files, all generation 1:
  - **/doc/readme** (:87-108): what zelr is, prompt commands `guide`, `help`, `desktop`, and the terminal's `help`, `tree`, `sys`. It mentions "the FAT16 driver".
  - **/doc/filesystem** (:110-135): what each directory is. It says /cfg holds the desktop theme and that the directories are "on a FAT16 volume".
  - **/home/notes** (:137-140): "A file on a real disk. …"
- `layout_home()` returns "/home" (:150). `init_task` does `vfs_chdir(layout_home())` (main.c:235).
- Other files programs keep: `/zelr.cfg` (THEME_FILE, theme.h:11) and `/zelr.pins` (PIN_FILE, pins.h:17) are in the **root**. `/cfg/history` and `/cfg/term` (term.c:34-35), `/cfg/notes-open` (notes.c:184) and `/cfg/seeded` are in /cfg.
- On a machine with no disk, the RAM fs is re-seeded every boot.

### 3.12 include/blackbox.h + kernel/blackbox.c: boot log

Constants (blackbox.h): `BB_SECTORS 32`, `BB_LBA_MIN 1`, `BB_BYTES 16384`, `BB_MAGIC 0x5842594E` (bytes "NYBX", from the pre-v0.11 name).

`bb_head_t` (blackbox.c:29-35), 48 bytes and the same in memory and on disk: `magic, boot, len, sum (FNV-1a of the text), char build[32]` (KERNEL_VERSION). `BB_TEXT_MAX = 16384 - 48 = 16336`.

State (all BSS, "this has to work before heap_init runs"):
- `image[BB_BYTES]` (header + text)
- `len`, `boot_number`
- `wrapped` (set, never read)
- `saved[BB_BYTES]`, `saved_len`, `saved_valid` (the previous boot)
- `log_lba` (default 1)
- `scratch[512]` (static, "the fault path has no stack to spare")

Functions:
- `checksum` is FNV-1a over the text (:47-51).
- `drop_oldest_half` (:58-68): keeps about the last 8168 bytes, starting at a line boundary. "What killed the machine is at the end."
- `append(s)` (:70-78): per character, drops the oldest half when full, stores the character and **`serial_putc`s it** (the serial mirror).
- `stamp()` (:85-91): "[%5d] " milliseconds = ticks×1000/hz. It reads 0 before `timer_init` (main.c:459), which the comment calls useful because it places the line before the timer.
- `bb_init` (:93-101): clears everything and writes "zelr 0.37.0 boot log\n".
- `bb_log(fmt, …)` (:103-112): a 256-byte line with the stamp and a newline.
- `bb_mark(phase)` (:114-119): "[ms] == phase". The last mark is "the thing that did not finish".
- `bb_fault(r, what)` (:121-136): reads CR2 and CR3 and logs "!! what", vec/err/rip/cs/flags, rsp/ss/cr2/cr3, rax..rdx, rsi/rdi/rbp/r8. `%p` prints 64 bits (selftest checks "0x00804000dead1000").
- `bb_text/bb_len/bb_boot_number` (:138-140).
- `screen_lines(start, count, rows)` (:145-155): the last `rows` lines.
- `line_colour(line)` (:160-167): "!!" 0xFF6060; "==" 0x80D0FF; leading space (register lines) 0xFF9090; otherwise 0xC8C8C8.
- `bb_screen()` (:169-206):
  - Framebuffer: `rows = min((h-40)/16, 60)`; clear 0x120008; red bar 0xA01020 22 px; header "zelr stopped. this is the boot log."; lines up to 127 characters at y = 28+16·row; `fb_flush`.
  - VGA: white on black, "zelr stopped. boot log:", the last 23 lines.
- `volume_base()` (:220-222) = `fat_mounted() ? fat_base() : 0`, using the **selected** volume.
- `volume_is_ours(sec)` (:232-241), the four boot-sector conditions: disk 0 present; read `volume_base()`; `fat_boot_is_ours` (1. 0x55AA, 2. OEM "ZELR    ", 3. serial 0x5A4C5200 at 39/67); 4. `fat_boot_reserved ≥ fat_boot_log_lba + 32`. It then sets `log_lba` (1 on FAT16, 8 on FAT32).
- `region_is_free()` (:251-258): the **first** log sector must start with BB_MAGIC or be all zero. "Unrecognised data is never written over."
- **`bool bb_flush()`** (:264-291): `volume_is_ours` and `region_is_free`. It re-reads the on-disk header to take `prev.boot`, then `boot_number = prev+1`. It fills the header (magic, boot, len, sum, build) and writes `ceil((48+len)/512)` sectors (≤ 32) through `blk_write`, which splits. Then `blk_flush`.
- **`void bb_recover()`** (:301-318): `volume_is_ours`; reads all 32 sectors into `saved`; magic, `len ≤ BB_TEXT_MAX` and checksum must be right, otherwise "torn, or never finished". Sets `saved_valid` and logs "black box: recovered boot %d, %d bytes". **It must run before the first `bb_flush`.**
- `u32 bb_prev(out, cap)` (:320-326): copies the **first** `min(saved_len, cap-1)` bytes and NUL-terminates.

Where it is called:
- `bb_init` and `bb_mark` throughout `kmain` (main.c:306-644). Marks in order: "serial, vga", "gdt", "idt", "pic", "memory", "paging", "heap", "acpi, pcie", "video", "filesystem", "disk", "clock", "timer", "entropy", "smp", "network", "input", "sound", "interrupt routing", "syscalls, window server, scheduler", "handing over to the scheduler".
- `bb_recover` at main.c:426, right after `diskfs_mount`.
- `bb_flush` at main.c:645 and in `panic` (printf.c:176, followed by `bb_screen` and the message "(black box not written: not a zelr volume)" when it refused).
- `bb_fault` in idt.c:231 before panicking on an unhandled kernel exception.
- `bb_log` is used across the kernel (usb, net, disk, …).

---

## 4. Control flow and lifecycles

### 4.1 Boot storage sequence (main.c)
1. main.c:306 `bb_init()`. Everything logged from here is mirrored to serial.
2. main.c:413-416 `bb_mark("filesystem")`, `fs_init()`, `vfs_init()` (which resets the program table), `fd_init()`.
3. main.c:417-418 `bb_mark("disk")`, `blk_init()`: AHCI, then NVMe, then ATA; the first that answers becomes disk 0.
4. If a disk was found: kprintf and bb_log "disk MODEL via DRIVER, N MiB", then `n = diskfs_mount()`:
   - `parts_scan()` (disk 0 only), log the GPT rejection reason, choose the volume (§3.6), and in `finish_mount` run `fat_boot_repair` then `fat_reclaim`.
5. main.c:426 `bb_recover()`, which reads the previous record out of the mounted volume before anything writes this boot's record.
6. `n ≥ 0`: "fs fatNN mounted, N entries in the root". `n == -2`: **`diskfs_format()`** gives "fs new disk prepared" or "fs could not prepare the disk" (the latter leaves the machine on the RAM fs). The `else` branch ("disk unreadable, using memory only", main.c:438) is unreachable.
7. No disk: "disk none: no controller this kernel can drive". The RAM fs is used.
8. main.c:444 `builtin_install()` fills /bin.
9. main.c:459 `timer_init(100)`. Only now does `timer_ticks()` advance. NVMe deadlines and log stamps before this point are stuck at 0.
10. main.c:523 `usb_init()`: a stick present at boot is attached, registered and mounted through `diskfs_mount_removable` (single-threaded, before the scheduler).
11. main.c:613 `usb_start_service()` starts the kernel task that polls port changes every 300 ms (usb.c:712-717) and mounts or unmounts sticks when they are plugged or pulled.
12. main.c:644-645 `bb_mark("handing over to the scheduler")`, `bb_flush()`. This is the only non-panic disk write of the log. Later log lines reach the disk only through a panic.
13. The `init` task: `layout_init()` then `vfs_chdir("/home")`, then `shell_task()`. In a selftest boot, the `selftest` task runs instead and `test_layout` calls `layout_init` itself.

### 4.2 Choosing and preparing the boot volume
```
parts_scan: GPT valid? -> table from GPT (count may be 0)
            else MBR with a usable non-0xEE, non-extended entry? -> table
            else count = 0 (scheme NONE)
count == 0  -> fat_mount_at(0) ok ? mount : return -2 -> main.c auto-format (whole disk)
count > 0   -> pass 0: first non-ESP FAT-looking partition that is ours
               pass 1: first non-ESP FAT-looking partition that mounts
               none -> -2 -> main.c calls diskfs_format -> refused (partitioned) -> RAM fs
```

### 4.3 Crash-safe write (`fat_write_file`, fat.c:1410-1479)
1. `resolve_parent`; refuse an empty leaf.
2. `dir_find(parent, leaf)`. If it exists: refuse a directory and remember `old_chain`. If not: `dir_put_name` reserves the short slot and **writes the LFN entries now**; `attr = ARCHIVE`.
3. For each cluster: `alloc_cluster` (writes EOC to both FATs), link from the previous cluster (`fat_set`), write `spc` sectors of data with the last sector zero-padded. On any failure: `free_chain(first)` and return false.
4. **`blk_flush()`** (disk 0): "Make sure the data is on the platter before anything points at it."
5. Set cluster (hi and lo), size, `write_date 0x5A21`, `write_time 0`.
6. **`dir_write(parent, slot, &e)`**: a one-sector read-modify-write. **This is the commit.**
7. `blk_flush()`; if it fails, return false. The commit may already be on disk; the old chain then leaks until the next reclaim.
8. `free_chain(old_chain)` and `blk_flush()`.

Crash outcomes. Before 6: the old file is intact, and new clusters leak until `fat_reclaim` at the next mount. Between 6 and 8: the new file is live and the old chain leaks until reclaim. The new-file case can leave orphan LFN entries, which are harmless but use slots. Caveat: the "flush" barriers are real only on ATA and NVMe (§10).

### 4.4 Open files, fsync and the caching model (fd.c, another area)
- `fd_open` (fd.c:290-326) reads the **whole file into memory** (`ofile_t.data`) unless `O_TRUNC`. Writes go to memory and set `dirty` (fd.c:351-372).
- The last close (`of_unref`, fd.c:109-127) calls `vfs_write(path, data, size)` if dirty: one full crash-safe rewrite.
- `fd_sync` (fd.c:480-491, `SYS_FSYNC` = 61): a dirty file is written through `vfs_write` and then marked clean. A clean file only gets `diskfs_flush()` (disk 0).
- Two opens of one file get independent copies.
- The VFS and FAT layers keep no cache except one FAT sector per volume. Every stat, read or list re-reads directory sectors, and FAT updates are written through immediately.
- `sys_power` (syscall.c:463-475) and the desktop's Shut down (wm.c:2975-2979) call only `diskfs_flush()`. Dirty open descriptions are not written back.

### 4.5 USB stick lifecycle
Plug: `usbdisk_attach` then `blk_register` (the first free id, normally 1), then `diskfs_mount_removable(id)` (usb.c:551-558), trying MBR partitions, then the whole device.
Unplug: `forget_root(port)` (usb.c:681-694) calls `diskfs_unmount_removable()` for **every** device on that root port, then `usbdisk_detach` (which unregisters if the slot matches).
Only one stick is supported (usbdisk.c:258).
Paths under /usb route to volume 1. While nothing is mounted they fall through to the RAM fs.

### 4.6 Black-box state machine
```
bb_init -> append/log/mark (RAM + serial)
  -> [disk mounted] bb_recover: volume ours? read 32 sectors -> magic/len/sum ok? saved_valid
  -> bb_flush at handover: ours? region free? boot = prev.boot + 1; write header + text
  -> panic: bb_log("!! panic: …"), bb_flush (boot counter increments again), bb_screen, halt
/sys/boot   = the tail of image text (≤ 4095 bytes)
/sys/lastboot = the head of saved text (≤ 4095 bytes) or "no record"
```

### 4.7 Layout seeding
`read_marker` gives G. For each seed with gen > G whose path is missing, write it. If G < SEED_GENERATION, write the marker. Deleting /cfg/seeded re-offers every file whose path is missing.

---

## 5. Interfaces

### 5.1 Exported by this area, and who uses it
- **Block layer**:
  - `blk_init`: main.c:418.
  - `blk_present/sectors/model/driver`: main.c, shell.c (`disk`), sysfs.c (/sys/devices), selftest.
  - `blk_read/blk_write`: parts.c, blackbox.c, selftest [disk] and [black box].
  - `blk_read_on/blk_write_on`: fat.c `vol_*`, diskfs.c.
  - `blk_flush`: fat.c (9 sites), blackbox.c, diskfs_flush.
  - `blk_register/unregister`: usbdisk.c.
- **parts**: diskfs.c only.
- **diskfs**:
  - `diskfs_mount` and `diskfs_format`: main.c.
  - `diskfs_format`: shell `format`.
  - `diskfs_flush`: shell `sync`, shell.c:579, fd.c:485, syscall.c:466 (power), wm.c:2978.
  - `diskfs_mount_removable/unmount_removable`: usb.c.
  - `diskfs_mounted/available`: shell.c.
- **fat**:
  - Direct users: vfs.c, diskfs.c, blackbox.c (`fat_boot_is_ours`, `fat_boot_log_lba`, `fat_boot_reserved`, `fat_mounted`, `fat_base`).
  - main.c (`fat_type`), shell.c (`fat_mounted`, `fat_total_clusters`, `fat_cluster_bytes`, `fat_free_bytes`), selftest [fat], [built-in programs] and [black box].
- **vfs**:
  - fd.c (`vfs_resolve/stat/read/write/generated`), syscall.c (`read_file`, `unlink`, `mkdir`, `rmdir`, `readdir`, `stat`, `chdir`, `getcwd`, `rename`, exec/spawn via `vfs_slurp`), shell.c, layout.c, theme.c, pins.c, wm.c (launch), builtin.c, selftest.
- **sysfs**: vfs.c; builtin.c (the `SYSFS_MAX_PROGRAMS` assert).
- **layout**: main.c init_task, selftest.
- **blackbox**: everywhere via `bb_log`/`bb_mark`; idt.c (`bb_fault`); printf.c panic (`bb_flush`, `bb_screen`); sysfs.c (`bb_text`, `bb_len`, `bb_prev`).

Syscalls that reach this area (numbers from include/syscall.h and sdk/zelr.h): READ_FILE 6, OPEN 13, CLOSE 14, FREAD 15, FWRITE 16, SEEK 17, UNLINK 18, MKDIR 19, RMDIR 20, READDIR 21 (`zelr_stat_t.name ≥ VFS_NAME_MAX`, asserted at syscall.c:650), STAT 22, CHDIR 23, GETCWD 24, POWER 42, FSYNC 61, RENAME 62.

### 5.2 Dependencies of this area
pci.c (`pci_find_class`, `pci_read32`, `pci_enable_bus_master`); paging.c (`paging_map_device`, `virt_to_phys`); heap.c (`kmalloc/kfree`, identity-mapped); io.h port I/O; timer.c (`timer_ticks`, `timer_hz`); sched.c (`task_current()->cwd`, `task_list` for /sys/tasks); serial.c, vga.c, fb.c and gfx.c (black box); string.c (`strncpy` does **not** terminate on truncation, string.c:103-108); printf.c (`kformat`, `kvformat`); every subsystem sampled by /sys (pmm, heap, smp, lapic, netdev, net, tcp, sound, usb, usbdisk, fb, rtc, clipboard, theme, wait).

---

## 6. Concurrency, locking, ownership, invariants

- **No lock in this area.** It relies on the big kernel lock (sched.c:97-118, idt.c:150-188, 287).
  - Interrupt gates mean syscalls run with IF=0, so a ring-3 file syscall runs to completion without preemption unless it sleeps.
  - Kernel tasks run with IF=1 (sched.c:207). The timer path calls `scheduler_switch` whatever mode it interrupted (idt.c:256-258).
  - So a kernel task such as the kernel shell, init/layout, wm, theme or pins saving, or the USB hot-plug service can be switched out in the middle of a fat.c operation. Another task can then enter fat.c: on the same CPU, or on another CPU once the lock is released on a return to ring 3.
  - Unprotected shared state includes fat.c's `sec`, `dsec`, per-volume `fat_cache` and **`current_volume`**, sysfs's `scratch`, and blackbox's `image` and `scratch`.
  - `fat_mount_on`'s save/select/restore of `current_volume` is racy against any concurrent `route()`.
  - At boot everything is single-threaded until `sched_start`, including the mount of a stick present at boot.
- The black box is written from the panic path with interrupts off. The drivers are polled, so this works, but an NVMe deadline cannot expire there (no ticks).
- Ownership:
  - `vfs_slurp` returns a buffer the caller must free.
  - `blk_register` keeps the caller's `blkdev_t` pointer (static in every driver).
  - `sysfs_add_program` keeps the name and data pointers (kernel image).
  - The RAM fs owns `file_t.data`.
  - `fat_reclaim` frees its temporaries.
  - AHCI and NVMe aligned allocations are never freed.
- Invariants and assumptions:
  - Sectors are 512 bytes everywhere. NVMe enforces it (LBADS 9) and FAT mount enforces BytsPerSec 512; AHCI, ATA and USB do not check.
  - The heap is identity-mapped.
  - `vol_*` never touches LBAs outside `[part_base, part_base+part_sectors)` once mounted.
  - The directory-entry write is the single commit point.
  - The FAT is written through with both copies identical.
  - The disk log is only written where `fat_boot_is_ours` holds and the region is blank or already a log.
  - /sys and /bin are never writable (`vfs_generated`, fd.c:299).
  - `vfs_resolve` output is canonical, so the FAT layer never sees "." or "..".

---

## 7. Limits and magic numbers

| Item | Value | Where |
|---|---|---|
| Sector size | 512 | blockdev.h:4 |
| Block devices | 4 (id 0 boot) | blockdev.h:19-21 |
| Per-request max | AHCI 8, NVMe 8, ATA 255, USB 8 | ahci.c:106, nvme.c:53, ata.c:111, usbdisk.c:243 |
| ATA capacity | LBA28 (≤ 0x0FFFFFFF sectors, 128 GiB) | ata.c:92 |
| AHCI/NVMe capacity | clamped to 0xFFFFFFFF sectors (2 TiB) | ahci.c:228, nvme.c:322 |
| ATA spin | 100,000,000 status reads | ata.c:46,54 |
| AHCI spins | wait_done 20M, TFD 10M, stop_port 1M, start_port unbounded | ahci.c:145,190,123,131 |
| NVMe queues | 64 entries each (admin and I/O), I/O QID 1, namespace 1 | nvme.c:47-48 |
| NVMe timeout | CAP.TO×500 ms, 30 s if 0 or larger; ticks = ms/10+2 (100 Hz assumed) | nvme.c:153,262-263 |
| Partitions | 16 | parts.h:22 |
| GPT header size | 92..512 | parts.c:151 |
| GPT entries | count 1..256, size 128..4096, array ≤ 32 KiB, entry_lba 2..2^32-1 | parts.c:166-188 |
| FAT volumes | 2 (disk, usb) | fat.h:13 |
| Path / name | 128 / 64 (63 characters) | fat.h:4,9 |
| FAT12 floor | < 4085 clusters refused | fat.c:391 |
| FAT16/32 split | > 65524 clusters means FAT32 | fat.c:392 |
| Format minimum | 8192 sectors checked; in practice FAT16 needs about 16.4k sectors (≈ 8 MiB) | fat.c:637,646-653 |
| FAT16 format | spc 4..64, reserved 33, 512 root entries, 2 FATs | fat.c:646-653 |
| FAT32 format | spc 8/16/32/64 start by size, up to 128; reserved 40 | fat.c:658-671 |
| Boot code offset | 0x3E (FAT16) / 0x5A (FAT32) | fat.c:609,687 |
| Boot stub | 25 bytes plus a 71-byte message | fat.c:550-567 |
| Volume serial | 0x5A4C5200 ("ZLR\0") at 39 / 67 | fat.c:438,466 |
| OEM name | "ZELR    " | fat.c:465,691 |
| Timestamp | write_date 0x5A21 (2025-01-01), time 0 | fat.c:1465,1617,1637 |
| LFN | ≤ 63 characters, 13 per entry, ≥ 0x80 read as '?' | fat.c:1007,1018,1149 |
| Alias numbers | ~1..~999 | fat.c:1094 |
| FSInfo | 0x41615252 / 0x61417272 / 0xFFFFFFFF×2 / 0xAA550000 | fat.c:749-753 |
| Reclaim queue | 64, doubling | fat.c:1692 |
| VFS resolve | depth 32, work buffer 256, component truncated at 63 | vfs.c:67,80,93 |
| RAM fs | 64 entries, 128-character names, 64-byte initial buffer | fs.h:11-12, fs.c:38 |
| /bin programs | 48 (46 used) | sysfs.h:30, builtin.c |
| /sys render buffer | 4096 (content ≤ 4095), line 255 | sysfs.h:34, sysfs.c:80 |
| /sys nodes | 15 | sysfs.c:353-369 |
| Seed generation | 1, marker /cfg/seeded | layout.c:37-38 |
| /tmp emptying | ≤ 256 entries | layout.c:70 |
| Black box | 32 sectors = 16 KiB, header 48, text 16336 | blackbox.h:27-34 |
| Log position | LBA 1 (FAT16) / 8 (FAT32), relative to the volume | fat.c:475 |
| BB magic | 0x5842594E ("NYBX") | blackbox.h:34 |
| BB overflow | keep about 8168 bytes from a line start | blackbox.c:59 |
| Panic screen | ≤ 60 rows at 16 px on the framebuffer, 23 lines on VGA, 127 columns | blackbox.c:172-173,182,199 |
| bb_log line | 256 | blackbox.c:104 |
| mkfat.py serials | FAT16 0x5A4C5202 (not "ours"), FAT32 0x5A4C5200 ("ours") | tools/mkfat.py:214,430 |

---

## 8. Tests

### 8.1 Kernel selftest (`-append selftest`, section order at selftest.c:3557-3595)
- **[filesystem]** `test_fs` (selftest.c:239-255), 7 checks: VFS write, read, stat size, append, count grows, delete (root files /t.txt and /u.txt).
- **[paths]** `test_paths` (:257-284), 11 checks: `vfs_resolve` absolute, "//", ".", "..", "/../../..", "/", then chdir into /sub with relative and ".." resolution.
- **[directories]** `test_directories` (:286-312), 12 checks: mkdir, stat, a second mkdir fails, a nested file, count, nested dirs, "/d/deep/../deep/x", rmdir refuses non-empty, cleanup.
- **[open files]** `test_open_files` (:918-…), 30 checks in total. The file part covers fd 3 first, overwrite in place, close writes it out, chunked reads, seek, dup/dup2 sharing a position; the rest is pipes (fd.c area).
- **[disk]** `test_disk` (:368-418), 12 checks: read/write/restore of sector `blk_sectors()-4`, then a 16-sector run at `blk_sectors()-24` (which forces splitting on AHCI, NVMe and USB), and count 0 refused.
- **[fat]** `test_fat` (:521-564), 14 checks: mounted; cluster count in range for the width; cluster bytes ≥ 512; a multi-cluster file (2 clusters + 137 bytes) written, read back byte for byte, listed, deleted; mkdir /sub, the same file inside it, `fat_count("/sub")==1`, cleanup.
- **[built-in programs]** `test_builtin` (:878-916), 8 checks: every program registered, /bin/browser present (the comment calls it "the last"; it is no longer), /bin/paint is an ELF, cannot be written or deleted, not present on disk.
- **[live tree]** `test_live_tree` (:2948-3010): /sys and /bin are directories and listed in "/"; /sys/version content and stat size equal to the read size; memory, tasks, devices content; /sys/uptime changes over 60 ms; read-only through vfs_write, delete, fd_open(O_WRITE) and mkdir; readable through fd; missing files fail.
- **[layout]** `test_layout` (:3012-3040): the 4 directories exist; /doc/readme is over 100 bytes; re-running does not restore a deleted /home/notes and keeps user files; /tmp is emptied by a re-run.
- **[black box]** `test_blackbox` (:3062-3174), 21 checks:
  - log line, mark, fault record with a full 64-bit RIP;
  - overflow keeps the newest and drops the oldest;
  - on a zelr-formatted disk image: flush, then recover round-trips; one flipped text byte is refused as torn and restoring it makes it readable again;
  - OEM changed to "MSWIN4.1" means refused and not read; restoring it means accepted;
  - "NOT A BLACK BOX" in the first log sector means refused; a zeroed region is accepted.
  - It uses LBA 0 directly, so it assumes an unpartitioned image. It calls `bb_flush` 5 times, and the boot counter advances each time.
- README.md:1375-1379: the same suite also runs on q35 (AHCI), which found the missing request split.

### 8.2 Harnesses (all run by pipeline/gate.sh)
- **tools/blackbox_test.sh** (gate "fast", gate.sh:341). A 32 MiB zero image on i440fx, so ATA PIO.
  - Boot 1: "== handing over to the scheduler", "first boot done", "fs new disk prepared".
  - Boot 2: output fenced between BEGIN_RECORD and END_RECORD markers around `cat /sys/lastboot`. It must contain "== gdt", "== disk", "== handing over to the scheduler", "boot log" and "fs new disk prepared", and must not contain "no record" or "fs fat16 mounted". The comment explains the fence: unfenced, 4 of 7 checks passed with the disk write removed.
  - This depends on the first boot's log being under 4095 bytes, because /sys/lastboot shows the head.
- **tools/fat32_test.sh** (gate "full", :363). `mkfat.py --fat32` 40 MiB with HELLO32.TXT, SUB/INNER.TXT and HIGH.TXT at cluster 70000 (tests the high cluster half). ATA.
  - Boot 1: "fat32 mounted", not "fat16 mounted", reads the root file and the file past 65535, not "reclaimed". It writes made32.txt and newdir/deep.txt.
  - Boot 2: both survive, the originals still read, not "reclaimed", not "no such file".
  - Then a 32 MiB FAT16 mkfat image is read as fat16.
  - An mkfat FAT32 volume carries the zelr serial, so the kernel runs `fat_boot_repair` on it at the first mount. This is not checked.
- **tools/nvme_test.sh** (full, :366). q35 plus `-device nvme`.
  - Boot 1 on a 64 MiB zero image: "via nvme", not "disk    none", "written through nvme".
  - Boot 2: files survive, and a check that the previous log was recovered.
  - Then an mkgpt.py image: "via nvme", "parts gpt", "efi system partition", not "mounted partition 1", "read from a gpt partition".
  - Blind spots: see §10, D11.
- **tools/gpt_test.sh** (full, :360; outside my scope but relevant):
  - A good GPT: "parts gpt, 2 partition(s)", ESP skipped, "fs mounted partition 2", file read.
  - A bad header CRC and a bad entry CRC: "gpt rejected: …", nothing mounted or served.
  - A bare image: "parts none" and "new disk prepared".
  - **It does not check that the corrupt-GPT disks are left unformatted.** They are formatted (§10, S1).
- **tools/mountcheck.py** (full, :447). q35 (AHCI boot disk, 32 MiB), qemu-xhci plus usb-storage with an 8 MiB mkfat FAT16 stick (serial ...02, not ours).
  - Boot serial shows "usb volume mounted". `ls /usb` shows HELLO.TXT. The file can be read. `write /usb/MADE.TXT …` then `ls` shows both files (listing rather than reading back, because an unmounted /usb falls through to the RAM fs). `cp /usb/HELLO.TXT /home/COPIED.TXT` then cat. `ls /` shows "usb/". readfat.py on the host sees MADE.TXT with the right content.
  - The stick is smaller than the boot disk, so the `blk_sectors()` bound bug (§10, S10) is not exercised.
- **tools/namecheck.py** (full, :451). q35 (AHCI).
  - Boot 1: write /home/a-rather-long-file-name.txt, two.dots.here.txt and plain.txt; list, cat.
  - Boot 2: still listed and readable; `rm` of the dotted name removes it. readfat.py sees the long name, not the deleted one, and the short one.
- **tools/crashcheck.py** plus **userland/crashwrite.c** (full, :547). Default machine, i.e. i440fx and **ATA PIO**, 64 MiB.
  - `crashwrite A` then the file is whole A across a clean restart.
  - 6 rounds (`--rounds=N`) of `crashwrite` (endless B/A 64 KiB rewrites of /home/crash.dat, printing CRASH_TURN) killed 0.15-2.5 s after the first CRASH_TURN. After each, `crashwrite check` must print CRASH_WHOLE A or B (not TORN, SHORT, MISSING or WRONG). Both A and B must appear over the rounds. Finally `ls /home` shows crash.dat and `disk` output mentions fat.
  - crashwrite.c: `SIZE 64*1024`, `open(O_WRITE|O_CREATE|O_TRUNC)`, fwrite loop, `close` is the commit ("a file is held in memory until the last descriptor goes"). `check` reads through a fresh descriptor.
- **tools/ring3check.py** runs userland/durtest.c ("saying now about a file, and renaming one", ring3check.py:48):
  - Data is not on disk before fsync. fsync writes it; a second fsync rewrites it; close afterwards keeps it.
  - rename in the same directory works; replacing a file via a temp works.
  - Cross-directory, long-target and missing-source renames are refused; rename to itself is allowed.

---

## 9. How to extend

- **A new disk driver**: implement the 7 `blkdev_t` hooks. Add a static descriptor and an arm to `blk_init` (blockdev.c:134-144) in the order you want probed; the comment asks that existing machines keep picking the same controller. For a hot-plug device, call `blk_register`/`blk_unregister` and then diskfs. Keep `max_run` honest; the block layer splits for you. If the device is not 512-byte, the block layer needs a sector-size field first (§10, S13).
- **Interrupt-driven I/O**: nvme.c:3-8 says to add it only when there is other work to overlap. Everything above is synchronous.
- **More than one stick / more volumes**: `FAT_VOLUMES`, `route()`, `fat_mount_on`, and usbdisk's single-stick limit. Replace the global `current_volume` with an explicit volume argument, or at least save and restore it around every VFS call (§10, S6).
- **GPT on sticks or other disks**: parts.c reads disk 0 only through `blk_read`. Parameterise it by device id and reuse it in `diskfs_mount_removable`.
- **FAT features**:
  - Timestamps would need an RTC hook in the three `write_date` sites.
  - Rename across directories or to long names is deliberately absent (fat.c:1481-1513). Doing it safely needs a journal or intent record that FAT lacks. The comment recommends copy plus delete.
  - UTF-8/UCS-2 long names: extend `dir_put_name` (fat.c:1174) and `long_name_of` (fat.c:1018) together.
- **A new /sys file**: add a `render_x(char*, u32)` using `put()` and one row in `nodes[]` (sysfs.c:353-369). Output must fit in 4096 bytes and is rendered twice per open (stat, then read).
- **A new built-in program**: add a line to builtin.c `PROGRAMS[]` and the incbin in builtin.S. Only 2 slots remain before the `SYSFS_MAX_PROGRAMS` (48) static assert fires; raise it in sysfs.h:30.
- **A new shipped file**: add `seed(N, path, text)` with **N = SEED_GENERATION+1** and bump `SEED_GENERATION` (layout.c:30-37). Never reuse an old generation, or existing disks will not get it. Never re-offer an old one, or deleted files come back.
- **Black box**: keep it allocation-free. `bb_recover` must precede any `bb_flush`. The reserved-area arithmetic lives in `fat_boot_log_lba` and `fat_format_at` (33 or 40 reserved sectors). The comments warn that the reserved area might later hold a second-stage loader, which is why `region_is_free` exists.
- **Pitfalls the comments call out**:
  - The request-split history (blockdev.c:68-77).
  - The FAT32 root must be marked in reclaim (fat.c:1684-1691).
  - FAT32 cluster_hi (fat.c:809-815; fat32_test.sh:66-73).
  - The jump offset per width (fat.c:683-686).
  - Tests must fence or `fresh()` their output, because typed commands and serial-mirrored log lines otherwise satisfy greps (blackbox_test.sh:11-17, mountcheck.py:75-91).

---

## 10. Doc drift and suspicious code

Ordered by severity. Every item was checked against the code as cited.

### Likely bugs / hazards

**S1. Auto-format can wipe a disk that is not blank.**
main.c:430-434 formats whenever `diskfs_mount()` returns -2. `diskfs_format` refuses only if `parts_count() > 0` (diskfs.c:95). `parts_count()==0` covers much more than a blank image:
- a GPT rejected by any check (bad header or entry CRC, unreadable header or array, `entry_count > 256`, and so on; parts.c:143-200);
- a *valid* GPT whose entries were all skipped (unused, past 2 TiB, out of range), since `read_gpt` still returns true with count 0;
- an MBR whose only entries are 0xEE, extended (0x05/0x0F) or out of range (parts.c:258-275);
- an I/O error reading sectors 0 and 1.

In each case diskfs.c:135-141 tries `fat_mount_at(0)`. It fails on a protective MBR (BytsPerSec is 0) or on a read error, returns -2, and `fat_format("ZELR")` writes a new boot sector, reserved area, FATs and root from LBA 0 of **disk 0**. On a laptop booted from a stick, disk 0 is the internal NVMe or AHCI drive. The comment at diskfs.c:126-131 even notes that a rejected GPT "is a real disk whose table this kernel could not trust", but the code treats it as blank.

tools/gpt_test.sh:76-88 drives exactly this path, a corrupt header or entry CRC on a 64 MiB image with an ESP and a data partition. It only asserts that nothing is mounted or served, so the format (which prints "fs new disk prepared") passes unnoticed. The fix would be to auto-format only when sector 0 has no 0x55AA signature and no partitions were found, or only when the disk is all zeros.

**S2. `dir_find` 8.3 aliasing: one name can resolve to a different file** (fat.c:1202-1211).
The truncating `to_83(name)` is compared against every short entry *before* long names are checked. A name that needs LFN therefore matches an existing file whose short name equals its truncated form:
- "chapter10.txt" matches "chapter1.txt" (CHAPTER1TXT);
- "index.html" matches "index.htm";
- "document1.txt" matches "document.txt".

`fat_write_file` then overwrites the other file, and `fat_stat`, `fat_read_file`, `fat_delete_file`, `fat_rename` and `fat_mkdir` ("already there") all act on the wrong entry. The 8.3 compare should only run when `!needs_long(name)` (or `fits_83(name)`).

**S3. `fat_rename` to a file's own alias deletes the file** (fat.c:1539-1560).
Take `rename longname.txt LONGNA~1.TXT`. The destination fits 8.3; `same_name` is false; `dir_find(dst)` finds the *source* entry by its short name; `fat_delete_file(to)` deletes it; re-finding the source fails and the call returns false. The file is gone.

**S4. Delete and rmdir are not crash-ordered** (fat.c:1585-1588, 1658-1661).
The chain is freed (FAT written through) *before* the entry is marked deleted. A power cut in between leaves a live entry pointing at free clusters. The next allocation reuses them, which cross-links two files. `fat_reclaim` only frees reachable-but-unreferenced space and cannot repair this. Doing the entry write first would reduce the failure to a leak, which is what the file's own philosophy prescribes.

**S5. `fat_reclaim` can free live data** (fat.c:1675-1755).
- (a) If the queue cannot grow (`kmalloc` fails), `break` at :1728 leaves only the *entry loop*. The rest of that directory's entries stay unmarked and the sweep frees their clusters, despite the comment "stop widening, do not lose data".
- (b) A `dir_read` failure (:1712), or a chain cut short by a failed FAT read inside `dir_capacity` (`fat_get` returns EOC on error, :265), likewise leaves entries unmarked and freed.
- (c) Bad-cluster marks (0xFFF7 / 0x0FFFFFF7) are non-zero and unreachable, so they are "reclaimed" into the free pool (:1744-1748).
- (d) A directory whose subdirectory points at an ancestor is enqueued repeatedly until the queue cannot grow. Corrupt volumes only, but it chains into (a).

Reclaim runs on every mount, including foreign volumes (S20).

**S6. Volume selection leaks out of the VFS** (vfs.c:163-171 with fat.c:123).
`route()` leaves `current_volume` set to whatever the last VFS path chose. Every direct `fat_*` caller then acts on it:
- `diskfs_mounted` and `vfs_disk_backed` (used by /sys/devices "storage" and shell `mem`);
- shell `disk` and `mem` (they print the USB volume's clusters and free space after an `ls /usb`);
- blackbox `volume_base()` (it reads disk 0 at the USB partition's LBA; the ours-check makes it refuse, so the log is lost rather than misplaced);
- **`fat_format_at`**. It sizes the layout from `blk_sectors()` (disk 0) but writes through `vol_write` to `CUR.dev`. So the kernel shell's `format` typed after any /usb access writes a disk-0-sized FAT layout onto the **USB stick** (fat.c:630-636, 733).

**S7. Cross-volume rename is not rejected** (vfs.c:293).
`fat_rename(route(a), route(b))` runs on whichever volume the *second* `route` call selected. clang evaluates the arguments left to right, but the order is unspecified in C anyway. The other path is interpreted on the wrong volume. Example: `rename("/usb/x.txt", "/y.txt")` becomes a disk-volume rename of "/x.txt" to "/y.txt", which deletes an existing disk /y.txt first and renames an unrelated disk /x.txt. It can also return true when nothing was moved (`/usb/a` to `/a` when disk /a exists hits the same-name case). Reachable from ring 3 through SYS_RENAME (62).

**S8. AHCI "flush" is a no-op** (ahci.c:216, "DMA writes are already through").
DMA completion does not mean the drive's volatile write cache reached the media. On AHCI, which is the default on real machines and on q35, both flush barriers in the crash-safe protocol (§4.3 steps 4 and 7) and the black-box flush do nothing. The data-before-commit ordering can be lost on a real power cut. ATA sends FLUSH CACHE after every write (ata.c:154) and NVMe sends FLUSH (nvme.c:359-366). crashcheck.py runs on i440fx (ATA) and a QEMU kill does not model a drive cache, so it cannot catch this. usbdisk_flush is also a no-op (usbdisk.c:246, no SYNCHRONIZE CACHE).

**S9. Every fat.c flush goes to disk 0.**
fat.c calls `blk_flush()` = `blk_flush_on(BLK_BOOT)` at :785, 1461, 1472, 1477, 1570, 1589, 1627, 1639, 1662 and 1750. `blk_flush_on` has no other callers. Writes to /usb flush the internal disk, not the stick. With no disk 0 the stick itself is id 0, so it is correct by accident.

**S10. The mount bound uses the boot disk's size for every volume** (fat.c:421, `u32 disk = blk_sectors();`).
A USB volume larger than disk 0 is refused as corrupt. A USB volume claiming more sectors than the stick has is not caught. It should be `blk_device_sectors(CUR.dev)`. mountcheck.py passes because its 8 MiB stick is smaller than the 32 MiB boot disk.

**S11. /sys/lastboot shows the head of the previous log, not the tail** (blackbox.c:320-326 via sysfs.c:248-250, with `SYSFS_MAX` 4096).
`bb_prev` copies the *first* cap-1 bytes. The comments (sysfs.c:245-247, blackbox.h:42-44) and README.md:331-332 say to read the *last* mark. A previous log over 4095 bytes (it can hold 16 KiB) loses exactly its end. /sys/boot correctly keeps the tail (sysfs.c:235-239). blackbox_test passes only because a QEMU first boot's log is short.

**S12. NVMe timeouts are inert during boot** (nvme.c:153, 170).
Deadlines are computed from `timer_ticks()`, which does not advance until `timer_init(100)` at main.c:459. `blk_init()` and `diskfs_mount()` run earlier (main.c:418-421), so a controller that never becomes ready or never completes a command hangs boot forever. The same holds on the panic path (interrupts off). Also, `submit` does not compare `cid` (:155-161), so after a genuine timeout a late completion would be taken as the next command's. And `q->cq` is not volatile; the poll is correct only because the loop calls the opaque `timer_ticks()` (no LTO in build.sh:41-50).

**S13. A USB stick with non-512-byte sectors overruns kernel buffers** (cross-area, likely).
usbdisk reports and transfers `count*sector_bytes` (usbdisk.c:193, 214). The block layer has no sector size, and `diskfs_mount_removable` reads one "sector" into `u8 sec[512]` on the stack (diskfs.c:55-56), as do `fat_mount_at` and every `vol_read`. A 4096-byte-sector stick would write 4096 bytes into 512-byte buffers. (Whether `xhci_bulk` bounds the copy is an open question, §11.)

**S14. `fat_rmdir` leaves the directory's LFN entries behind** (fat.c:1658-1662).
There is no `dir_drop_long`, unlike `fat_delete_file` (:1586). A long-named directory that is removed leaves orphan LFN entries: slots are lost for good, and chkdsk or fsck would flag them.

**S15. `fat_free_bytes` overflows u32** (fat.c:1757-1763) above 4 GiB free. zelr formats FAT32 for disks over about 2 GiB, so this is reachable. Shell `mem` and `disk` print the wrapped value.

**S16. A 63-character long name is not NUL-terminated by `fat_list`** (fat.c:1340, `strncpy(name_out, real, 63)`, and string.c:103-108 does not terminate).
`sys_readdir` zeroes its buffer (syscall.c:659), so it is safe. The kernel shell `ls` (shell.c:131) and `empty_tmp` (layout.c:69) use uninitialised stack buffers and read garbage.

**S17. Long names with non-ASCII bytes are write-only.**
`dir_put_name` stores raw bytes as UTF-16 code units (fat.c:1174). `long_name_of` turns anything ≥ 0x80 into '?' (fat.c:1018), so `dir_find` can never match the original name. The file lists as "caf??…" and cannot be opened by either spelling.

**S18. ".." lookup inside fat.c can never succeed** (fat.c:231-241 with 1258-1266, 1296-1304).
`to_83("..")` yields `"        .  "`, which does not match the on-disk `"..         "`. `long_name_of` finds no LFN in front of it, so `dir_find(…, "..")` always fails. This is latent: `vfs_resolve` removes "." and ".." before FAT sees a path.

**S19. `empty_tmp` gives up at the first non-empty subdirectory** (layout.c:71-75).
It always looks at entry 0. If that entry is a non-empty directory, both delete and rmdir fail and the loop breaks, so nothing else in /tmp is removed. This contradicts "Files left there from last time are gone" (layout.c:66-67).

**S20. Foreign FAT partitions are adopted read-write** (a stated design choice, diskfs.c:16-17), with side effects the comments do not discuss:
- `layout_init` creates /home, /doc, /cfg and /tmp on somebody else's partition and writes the seed files, /zelr.cfg and /zelr.pins.
- `empty_tmp` deletes the contents of any existing TMP directory there (names compare case-insensitively).
- `fat_reclaim` rewrites its FAT (S5).

**S21. USB MBR scan accepts type 0xEF** (diskfs.c:44).
The "never touch the ESP" rule is only enforced for disk 0, so a bootable stick's ESP can be mounted at /usb and written. GPT-formatted sticks cannot be mounted at all (§3.6).

**S22. `/usb` with nothing mounted falls through to the RAM fs** (vfs.c:195-197, 246-253, 296-305).
Writes silently succeed into memory and are lost at reboot. `mkdir /usb` creates a RAM directory, which then lists and chdirs as if it were the stick. A file opened on /usb and closed after the stick was pulled is written to RAM (fd.c:120). mountcheck.py:87-91 acknowledges this.

**S23. The black-box boot counter increments on every `bb_flush`** (blackbox.c:268-275).
It reads back this boot's own header. A boot that reaches the scheduler and later panics counts twice, and a selftest run advances it by about 5.

**S24. `region_is_free` checks only the first of the 32 log sectors** (blackbox.c:251-258), not the whole region the comment at :243-250 promises to protect.

**S25. Preemptible kernel tasks can interleave inside fat.c** (§6).
In particular, the hot-plug mount in the `usb` service task (`fat_mount_on` saves and restores `current_volume`, fat.c:339-347) could write USB geometry into `volumes[DISK]` if another kernel task calls `route()` in between. Low probability, high impact.

**S26. Unreachable branch in main.c:438** ("disk unreadable, using memory only"). `diskfs_mount` returns -1 only when `!blk_present()`, which is impossible inside `if (blk_init())`.

Also, `start_port` in AHCI spins forever if CR never clears (ahci.c:131). `fat_mount_at` does not check that the FAT is large enough for `cluster_count`. `make_alias` returns a duplicate alias once ~1..~999 are all taken (fat.c:1094-1113). A failed seed write still records the marker (layout.c:45, 144-147).

### Doc and comment drift
- **D1.** fat.c:23-25 says "Formatting only produces FAT16". fat_format_at writes FAT32 above about 2 GiB (fat.c:655-672), and the comment at fat.c:489-491 says so.
- **D2.** FAT16-only wording remains in:
  - diskfs.h:19 ("-2 if it is not FAT16");
  - vfs.h:11 ("the FAT16 volume");
  - diskfs.c:164 (kprintf "fat16 on partition" for either width);
  - sysfs.c:187 (/sys/devices "storage   fat16 on disk");
  - shell.c:361 (`disk`: "format   FAT16");
  - layout.c:96 and :133 (seeded /doc/readme and /doc/filesystem);
  - fat32_test.sh:4-5 ("it formats FAT16").
- **D3.** The seeded /doc/filesystem (layout.c:129-130) and the layout.c:9 comment say the desktop keeps its theme in /cfg. `THEME_FILE` is "/zelr.cfg" (theme.h:11) and `PIN_FILE` is "/zelr.pins" (pins.h:17), both in the root.
- **D4.** fat.c has orphaned or duplicated comments:
  - :73-76 describes `part_base` above the struct;
  - :195-202 describes the FAT cache and hint, whose code moved into `fatvol_t`;
  - :188-191 says "Three buffers" but two are declared (the third is the per-volume `fat_cache`);
  - :808 is a stray "The sector holding entry `index`" header.
- **D5.** fat.c:684-686 says the old `EB 3C` jump landed "in the middle of the volume label" on FAT32. It lands at 0x3E, inside BPB_Reserved (52-63); the label is at 71-81.
- **D6.** README.md:483-489 says "Disk. Two drivers behind one block layer … same four calls". There are three disk drivers plus USB, and a per-id API. README.md:1671 describes diskfs.c as "reading and writing the filesystem image"; it now only chooses and prepares volumes. README.md:495-496 says "Files are worked on in memory and written through on every change": FAT writes are through, but fd.c holds open files in memory until close or fsync.
- **D7.** vfs.c:292 says "the live tree has no rename"; the branch actually covers the RAM fs. vfs.c:32-36 has two stacked comments for one function.
- **D8.** nvme.c:10-13 says the identity-map assumption "is shared with the AHCI driver and is checked at startup". Only nvme.c checks it (3 of its 5 buffers); ahci.c never does.
- **D9.** parts.c:21-23 speaks of a 16 KiB entry array; the code accepts up to 32 KiB.
- **D10.** selftest.c:889-892 checks /bin/browser as "the last one in the list". builtin.c now ends with polltest (46 entries); the count comparison at :886-887 still catches the overflow.
- **D11.** nvme_test.sh:92 greps the whole second-boot transcript for "== handing over to the scheduler". That boot mirrors its own marks to serial, so the check passes whether or not the record was recovered, which is exactly the pitfall blackbox_test.sh:11-17 describes. nvme_test.sh:74 ("written through nvme") is satisfied by the echoed command `write onnvme.txt written through nvme` itself.
- **D12.** crashcheck.py:31 documents `[--rounds N]` but :57-59 only parses `--rounds=N`. gate.sh:544 says "Six boots"; it is 2 + 2×rounds + 1 = 15.
- **D13.** fd.c:404-418: the fsync explanation sits above `fd_ready_now` rather than `fd_sync` (fd.c area).
- **D14.** `BB_MAGIC` spells "NYBX", a leftover of the pre-v0.11 "nyx" name.
- **D15.** Unused API: `VFS_MAX_OPEN`, `fs_append`, `fat_selected`, `fat_mount`, `blk_count`, `blk_device_removable`, `diskfs_removable_mounted`. ahci.c includes pmm.h and timer.h without using them.

---

## 11. Open questions

1. Does `xhci_bulk` (xhci.c) copy exactly `len` bytes into the caller's buffer, or does it bounce through a bounded buffer? This decides whether S13 is a stack overflow or a truncated or garbled read.
2. Is the AHCI drive write cache ever disabled elsewhere (for example SET FEATURES), which would make S8 moot? I found no such code in ahci.c.
3. Is heap memory guaranteed below 4 GiB? AHCI writes upper halves, so it is not strictly needed, but CAP.S64A is never checked.
4. Intent of S20: is emptying /tmp and seeding on a foreign ("not ours") FAT partition deliberate, or was layout written assuming a zelr volume?
5. Should /sys/lastboot keep the tail (as /sys/boot does) or is the head intentional? Every comment suggests the tail.
6. gpt_test.sh's corrupt-GPT cases: was the auto-format of those images noticed and accepted, or missed (S1)?
7. The `usb` service task and the `init` task can both do filesystem work concurrently after boot. Is there any higher-level serialisation (a wait queue or similar) that I missed? None is visible in vfs.c or fat.c.
8. Should `forget_root` unmount /usb when a non-disk device on another root port is unplugged? usb.c:681-694 calls `diskfs_unmount_removable()` for every device on the port, including keyboards. That is USB-area code, but it drops the /usb mount.
