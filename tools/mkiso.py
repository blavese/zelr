#!/usr/bin/env python3
"""Builds one bootable image that starts on any of four paths.

There is no third-party tool involved: no xorriso, no mtools, no isohybrid.
The image is written from the specifications, and the EFI System Partition
inside it is built by tools/mkfat.py, which is also ours.

The four ways in:

    BIOS, from a disc    El Torito catalog, first entry
    BIOS, from a stick   the MBR at the front of the image
    UEFI, from a disc    El Torito catalog, second entry, platform 0xEF
    UEFI, from a stick   an MBR partition marked as an EFI system partition

The last two both point at the same embedded FAT filesystem, because a UEFI
machine will not run a boot sector and wants a file called BOOTX64.EFI on a
volume it can read.

Layout, in 2048 byte sectors:

    0..15   system area, which is where the MBR lives
    16      primary volume descriptor
    17      boot record volume descriptor, pointing at the catalog
    18      volume descriptor set terminator
    19      El Torito boot catalog
    20      path table, little endian
    21      path table, big endian
    22      root directory
    23      the BIOS bootloader, which the firmware loads at 0x7C00
    ...     the EFI system partition, a FAT16 filesystem
    ...     the kernel, flattened, which the BIOS bootloader loads at 1 MiB

  python tools/mkiso.py [OUT.iso]
"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
SECTOR = 2048

PVD_LBA = 16
BRVD_LBA = 17
TERM_LBA = 18
CATALOG_LBA = 19
PATH_L_LBA = 20
PATH_M_LBA = 21
ROOT_DIR_LBA = 22
LOADER_LBA = 23


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def pad(data, size):
    if len(data) > size:
        raise ValueError("%d bytes will not fit in %d" % (len(data), size))
    return data + b"\x00" * (size - len(data))


def sectors_for(nbytes):
    return (nbytes + SECTOR - 1) // SECTOR


def dir_record(name, lba, length, is_dir):
    """One ISO 9660 directory record. `name` is bytes: b"\\x00" for ".",
    b"\\x01" for "..", otherwise an 8.3 name with a version suffix."""
    rec = bytearray()
    rec.append(0)                       # length, filled in at the end
    rec.append(0)                       # extended attribute length
    rec += both32(lba)
    rec += both32(length)
    rec += bytes([125, 1, 1, 0, 0, 0, 0])   # 2025-01-01 00:00 UTC
    rec.append(0x02 if is_dir else 0x00)
    rec.append(0)                       # file unit size
    rec.append(0)                       # interleave gap
    rec += both16(1)                    # volume sequence number
    rec.append(len(name))
    rec += name
    if len(rec) % 2:
        rec.append(0)                   # records are even sized
    rec[0] = len(rec)
    return bytes(rec)


def primary_volume_descriptor(total_sectors, root_extent, root_size,
                              path_table_size):
    d = bytearray(SECTOR)
    d[0] = 1
    d[1:6] = b"CD001"
    d[6] = 1
    d[8:40] = b" " * 32                                  # system identifier
    d[40:72] = b"ZELR".ljust(32)                          # volume identifier
    d[80:88] = both32(total_sectors)
    d[120:124] = both16(1)                               # volume set size
    d[124:128] = both16(1)                               # volume sequence
    d[128:132] = both16(SECTOR)
    d[132:140] = both32(path_table_size)
    d[140:144] = struct.pack("<I", PATH_L_LBA)
    d[148:152] = struct.pack(">I", PATH_M_LBA)
    d[156:190] = dir_record(b"\x00", root_extent, root_size, True)
    d[190:318] = b" " * 128                              # volume set
    d[318:446] = b"zelr".ljust(128)                       # publisher
    d[446:574] = b" " * 128                              # data preparer
    d[574:702] = b"tools/mkiso.py".ljust(128)            # application
    d[702:739] = b" " * 37
    d[739:776] = b" " * 37
    d[776:813] = b" " * 37
    stamp = b"2025010100000000" + b"\x00"
    d[813:830] = stamp
    d[830:847] = stamp
    d[847:864] = b"0" * 16 + b"\x00"                     # expiry: never
    d[864:881] = stamp
    d[881] = 1                                           # file structure version
    return bytes(d)


def boot_record_descriptor(catalog_lba):
    d = bytearray(SECTOR)
    d[0] = 0
    d[1:6] = b"CD001"
    d[6] = 1
    d[7:39] = b"EL TORITO SPECIFICATION".ljust(32, b"\x00")
    d[71:75] = struct.pack("<I", catalog_lba)
    return bytes(d)


def terminator():
    d = bytearray(SECTOR)
    d[0] = 255
    d[1:6] = b"CD001"
    d[6] = 1
    return bytes(d)


def boot_catalog(loader_lba, loader_sectors_512, esp_lba, esp_sectors_512):
    """Two platforms in one catalog.

    The first two entries are the ones every El Torito image has: a validation
    entry and the default entry, which firmware reads as the x86 one. A UEFI
    machine ignores those and looks for a section header naming platform 0xEF,
    which is what the second pair is.

    The validation entry has to make the first 32 bytes sum to zero as 16-bit
    words, which is the only integrity check in the format."""
    val = bytearray(32)
    val[0] = 1                                   # header id
    val[1] = 0                                   # x86
    val[4:28] = b"zelr".ljust(24, b"\x00")
    val[30] = 0x55
    val[31] = 0xAA
    total = sum(struct.unpack("<16H", bytes(val)))
    struct.pack_into("<H", val, 28, (-total) & 0xFFFF)

    bios = bytearray(32)
    bios[0] = 0x88                               # bootable
    bios[1] = 0                                  # no emulation
    struct.pack_into("<H", bios, 2, 0)           # load segment 0 means 0x7C00
    bios[4] = 0                                  # system type
    struct.pack_into("<H", bios, 6, loader_sectors_512)
    struct.pack_into("<I", bios, 8, loader_lba)

    # A section header, marked final, introducing one EFI entry.
    header = bytearray(32)
    header[0] = 0x91                             # final section header
    header[1] = 0xEF                             # EFI
    struct.pack_into("<H", header, 2, 1)         # one entry follows
    header[4:32] = b"efi".ljust(28, b"\x00")

    efi = bytearray(32)
    efi[0] = 0x88
    efi[1] = 0                                   # no emulation
    struct.pack_into("<H", efi, 2, 0)
    efi[4] = 0
    struct.pack_into("<H", efi, 6, esp_sectors_512 & 0xFFFF)
    struct.pack_into("<I", efi, 8, esp_lba)

    return pad(bytes(val) + bytes(bios) + bytes(header) + bytes(efi), SECTOR)


def path_tables(root_extent):
    """One record, for the root itself."""
    le = bytearray()
    le.append(1)            # length of the directory identifier
    le.append(0)            # extended attribute length
    le += struct.pack("<I", root_extent)
    le += struct.pack("<H", 1)      # parent is itself
    le.append(0)            # the identifier: a single zero byte
    le.append(0)            # padding, because the identifier length is odd

    be = bytearray()
    be.append(1)
    be.append(0)
    be += struct.pack(">I", root_extent)
    be += struct.pack(">H", 1)
    be.append(0)
    be.append(0)

    return bytes(le), bytes(be), len(le)


def root_directory(root_extent, entries):
    """entries is a list of (name, lba, length)."""
    d = bytearray()
    d += dir_record(b"\x00", root_extent, SECTOR, True)
    d += dir_record(b"\x01", root_extent, SECTOR, True)
    for name, lba, length in entries:
        d += dir_record(name, lba, length, False)
    return pad(bytes(d), SECTOR)


def patch_loader(loader, payload_lba, payload_bytes, entry, load_addr,
                 loader_lba):
    """The loader carries five values it cannot know until the image is laid
    out. They sit behind a signature so this does not have to understand the
    code around them.

    The last one is where the loader itself lives, which it needs because the
    first thing it does is read the rest of itself in rather than trust the
    firmware to have loaded all of it."""
    sig = loader.find(b"ZELRBOT1")
    if sig < 0:
        raise SystemExit("the loader has no ZELRBOT1 signature")
    out = bytearray(loader)
    struct.pack_into("<IIIII", out, sig + 8,
                     payload_lba, payload_bytes, entry, load_addr, loader_lba)
    return bytes(out)


def hybrid_mbr(code, total_sectors_512, loader_lba_512, loader_sectors_512,
               esp_lba_512, esp_sectors_512):
    """The first 512 bytes, holding boot code and a partition table.

    A BIOS booting from a disc reads the El Torito catalog; one booting from
    a USB stick reads this instead. Without it the same file that boots from
    a disc does nothing from a stick. The code came from bootloader/mbr.S and
    only needs telling where the real bootloader is."""
    if len(code) > 446:
        raise SystemExit("the MBR code is %d bytes; 446 is the limit" % len(code))

    mbr = bytearray(512)
    mbr[:len(code)] = code


    # Find it properly rather than by guessing: the packet is 4-aligned and
    # starts with 16, 0.
    for off in range(0, len(code) - 16, 4):
        if mbr[off] == 16 and mbr[off + 1] == 0 and mbr[off + 4:off + 6] == b"\x00\x7c":
            struct.pack_into("<H", mbr, off + 2, loader_sectors_512)
            struct.pack_into("<I", mbr, off + 8, loader_lba_512)
            break
    else:
        raise SystemExit("could not find the disk address packet in the MBR")

    # One partition, type 0x17 (hidden IFS) as isohybrid images use, covering
    # everything, marked bootable.
    part = bytearray(16)
    part[0] = 0x80                                  # bootable
    part[1:4] = bytes([0x00, 0x02, 0x00])           # CHS start, near enough
    part[4] = 0x17
    part[5:8] = bytes([0xFF, 0xFF, 0xFF])           # CHS end, saturated
    struct.pack_into("<I", part, 8, 0)              # first LBA
    struct.pack_into("<I", part, 12, total_sectors_512)
    mbr[446:462] = part

    # A second partition describing the EFI system partition, so firmware
    # booting this from a stick finds a volume it can read. Type 0xEF is what
    # says "this is an ESP"; without it a UEFI machine sees a disc image and
    # nothing it recognises.
    esp = bytearray(16)
    esp[0] = 0x00                                   # not the active one
    esp[1:4] = bytes([0xFE, 0xFF, 0xFF])
    esp[4] = 0xEF
    esp[5:8] = bytes([0xFE, 0xFF, 0xFF])
    struct.pack_into("<I", esp, 8, esp_lba_512)
    struct.pack_into("<I", esp, 12, esp_sectors_512)
    mbr[462:478] = esp

    mbr[510] = 0x55
    mbr[511] = 0xAA
    return bytes(mbr)


def entry_point(elf_path):
    data = open(elf_path, "rb").read()
    return struct.unpack_from("<I", data, 24)[0]


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD, "zelr.iso")

    # Built here when this is run on its own, and not when the gate runs
    # it. The gate builds once and then starts several harnesses at the
    # same time; a second build rewrites build/zelr.bin and build/zelr.elf
    # underneath whichever machine is reading them, which on Windows is a
    # permission error rather than a torn file.
    if os.environ.get("ZELR_PREBUILT") != "1":
        subprocess.run(["bash", "build.sh"], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run(["bash", "bootloader/build.sh"], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)

    kernel_elf = os.path.join(BUILD, "zelr.elf")
    payload_path = os.path.join(BUILD, "zelr.bin")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "flatten.py"),
                    kernel_elf, payload_path, "0x100000"],
                   cwd=ROOT, check=True, stdout=subprocess.DEVNULL)

    loader = open(os.path.join(BUILD, "cdboot.bin"), "rb").read()
    payload = open(payload_path, "rb").read()
    entry = entry_point(kernel_elf)

    # The EFI half: a FAT filesystem holding the loader firmware looks for and
    # the kernel it loads. Built by tools/mkfat.py, which is ours.
    esp_path = os.path.join(BUILD, "esp.img")
    efi_path = os.path.join(BUILD, "BOOTX64.EFI")
    # Big enough for what is going into it, rather than a number chosen when
    # the kernel was smaller. It was 4096 KB, and the kernel reached 4066: it
    # fit, and the filesystem around it did not, so the whole of this step
    # failed on a build that was otherwise fine. A size that is worked out
    # cannot go stale that way.
    want_kb = (len(payload) + os.path.getsize(efi_path) + 1023) // 1024
    want_kb += 1024                       # room for the filesystem around it
    esp_kb = max(4096, ((want_kb + 1023) // 1024) * 1024)
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mkfat.py"),
                    esp_path, str(esp_kb),
                    efi_path + ":EFI/BOOT/BOOTX64.EFI",
                    payload_path + ":zelr.bin"],
                   cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    esp = open(esp_path, "rb").read()

    loader_sectors = sectors_for(len(loader))
    esp_lba = LOADER_LBA + loader_sectors
    esp_sectors = sectors_for(len(esp))
    payload_lba = esp_lba + esp_sectors
    payload_sectors = sectors_for(len(payload))
    total_sectors = payload_lba + payload_sectors
    # Round the image up so a read that overshoots slightly still lands
    # inside it. Drives are entitled to refuse a read past the end, and the
    # failure is indistinguishable from a scratched disc.
    total_sectors = (total_sectors + 15) & ~15

    loader = patch_loader(loader, payload_lba, len(payload), entry, 0x100000,
                          LOADER_LBA)

    le_table, be_table, table_size = path_tables(ROOT_DIR_LBA)
    root = root_directory(ROOT_DIR_LBA,
                          [(b"ZELR.BIN;1", payload_lba, len(payload))])

    image = bytearray(total_sectors * SECTOR)

    def put(lba, data):
        image[lba * SECTOR:lba * SECTOR + len(data)] = data

    put(PVD_LBA, primary_volume_descriptor(total_sectors, ROOT_DIR_LBA,
                                           SECTOR, table_size))
    put(BRVD_LBA, boot_record_descriptor(CATALOG_LBA))
    put(TERM_LBA, terminator())
    put(CATALOG_LBA, boot_catalog(LOADER_LBA, sectors_for(len(loader)) * 4,
                                  esp_lba, esp_sectors * 4))
    put(PATH_L_LBA, pad(le_table, SECTOR))
    put(PATH_M_LBA, pad(be_table, SECTOR))
    put(ROOT_DIR_LBA, root)
    put(LOADER_LBA, loader)
    put(esp_lba, esp)
    put(payload_lba, payload)

    mbr_code = open(os.path.join(BUILD, "mbr.bin"), "rb").read()
    image[0:512] = hybrid_mbr(mbr_code, total_sectors * 4,
                              LOADER_LBA * 4, sectors_for(len(loader)) * 4,
                              esp_lba * 4, esp_sectors * 4)

    open(out_path, "wb").write(image)

    print("wrote %s" % out_path)
    print("  loader     %d bytes at sector %d" % (len(loader), LOADER_LBA))
    print("  esp        %d KiB at sector %d, FAT16 with BOOTX64.EFI"
          % (len(esp) // 1024, esp_lba))
    print("  kernel     %d bytes at sector %d, entry %#x"
          % (len(payload), payload_lba, entry))
    print("  image      %d sectors, %d KiB"
          % (total_sectors, total_sectors * SECTOR // 1024))
    print("  bios: El Torito and the MBR   uefi: the EFI catalog entry and "
          "the ESP partition")
    return 0


if __name__ == "__main__":
    sys.exit(main())
