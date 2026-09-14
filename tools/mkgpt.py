#!/usr/bin/env python3
"""Builds a GPT disk image, so the partition reader has something to read.

The kernel's own checks cannot cover this. A disk image has one filesystem
across the whole of it, which is the case the partition code exists to stop
being the only one, and QEMU will not invent a partition table. So one is
built here, from the specification, the same way tools/mkfat.py builds the
FAT volume that goes inside it.

The layout matches what a machine that boots through UEFI actually has: a
protective MBR so an MBR-only system sees a full disk it dare not touch, a
GPT header, an entry array, an EFI System Partition, and a data partition.
That the kernel must mount the second and leave the first alone is the whole
point of the exercise.

  python tools/mkgpt.py OUT.img

Both checksums are real. Writing them wrongly on purpose is how the tests
check that the kernel refuses a table that does not add up.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfat import Fat16Builder                                  # noqa: E402

SECTOR = 512
ENTRIES = 128
ENTRY_SIZE = 128

GUID_EFI_SYSTEM = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
GUID_BASIC_DATA = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"


def guid(text):
    """A GUID as GPT stores it: the first three groups little endian, the
    last two as written. Getting this wrong is the classic GPT bug, because
    the result still looks like a GUID."""
    a, b, c, d, e = text.split("-")
    return (struct.pack("<IHH", int(a, 16), int(b, 16), int(c, 16))
            + bytes.fromhex(d) + bytes.fromhex(e))


def gpt_header(my_lba, alt_lba, first_usable, last_usable, entry_lba,
               entry_crc, disk_guid):
    h = bytearray(92)
    h[0:8] = b"EFI PART"
    struct.pack_into("<I", h, 8, 0x00010000)        # revision 1.0
    struct.pack_into("<I", h, 12, 92)
    struct.pack_into("<I", h, 16, 0)                # crc, filled in below
    struct.pack_into("<I", h, 20, 0)
    struct.pack_into("<Q", h, 24, my_lba)
    struct.pack_into("<Q", h, 32, alt_lba)
    struct.pack_into("<Q", h, 40, first_usable)
    struct.pack_into("<Q", h, 48, last_usable)
    h[56:72] = disk_guid
    struct.pack_into("<Q", h, 72, entry_lba)
    struct.pack_into("<I", h, 80, ENTRIES)
    struct.pack_into("<I", h, 84, ENTRY_SIZE)
    struct.pack_into("<I", h, 88, entry_crc)
    struct.pack_into("<I", h, 16, zlib.crc32(bytes(h)) & 0xFFFFFFFF)
    return bytes(h)


def entry(type_guid, unique_guid, first_lba, last_lba, name):
    e = bytearray(ENTRY_SIZE)
    e[0:16] = type_guid
    e[16:32] = unique_guid
    struct.pack_into("<Q", e, 32, first_lba)
    struct.pack_into("<Q", e, 40, last_lba)
    struct.pack_into("<Q", e, 48, 0)
    n = name.encode("utf-16-le")[:70]
    e[56:56 + len(n)] = n
    return bytes(e)


def protective_mbr(total_sectors):
    """Type 0xEE covering the disk. A system that reads only MBR sees one
    partition of a type it does not know and leaves the disk alone, which is
    the entire purpose."""
    m = bytearray(SECTOR)
    p = 446
    m[p + 0] = 0x00
    m[p + 1], m[p + 2], m[p + 3] = 0x00, 0x02, 0x00      # starts at LBA 1
    m[p + 4] = 0xEE
    m[p + 5], m[p + 6], m[p + 7] = 0xFF, 0xFF, 0xFF
    struct.pack_into("<I", m, p + 8, 1)
    struct.pack_into("<I", m, p + 12, min(total_sectors - 1, 0xFFFFFFFF))
    m[510], m[511] = 0x55, 0xAA
    return bytes(m)


def build(path, total_kb=65536, bad_header_crc=False, bad_entry_crc=False):
    total_sectors = (total_kb * 1024) // SECTOR

    entry_sectors = (ENTRIES * ENTRY_SIZE) // SECTOR         # 32
    first_usable = 2 + entry_sectors                         # 34
    last_usable = total_sectors - 1 - entry_sectors - 1

    # An EFI System Partition, then the data partition the kernel should
    # actually use. Sizes are what a small machine has, rounded to something
    # FAT16 can describe.
    esp_start = first_usable
    esp_sectors = 16 * 1024 * 1024 // SECTOR                 # 16 MiB
    esp_end = esp_start + esp_sectors - 1

    data_start = esp_end + 1
    data_end = last_usable
    data_sectors = data_end - data_start + 1

    entries = bytearray(ENTRIES * ENTRY_SIZE)
    entries[0:ENTRY_SIZE] = entry(
        guid(GUID_EFI_SYSTEM), guid("11111111-2222-3333-4444-555555555555"),
        esp_start, esp_end, "EFI System Partition")
    entries[ENTRY_SIZE:2 * ENTRY_SIZE] = entry(
        guid(GUID_BASIC_DATA), guid("66666666-7777-8888-9999-AAAAAAAAAAAA"),
        data_start, data_end, "zelr data")

    entry_crc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF
    if bad_entry_crc:
        entry_crc ^= 0xFFFFFFFF

    disk_guid = guid("DEADBEEF-CAFE-1234-5678-9ABCDEF01234")
    primary = gpt_header(1, total_sectors - 1, first_usable, last_usable,
                         2, entry_crc, disk_guid)
    if bad_header_crc:
        primary = bytearray(primary)
        struct.pack_into("<I", primary, 16, 0xDEADBEEF)
        primary = bytes(primary)

    img = bytearray(total_sectors * SECTOR)
    img[0:SECTOR] = protective_mbr(total_sectors)
    img[SECTOR:SECTOR + len(primary)] = primary
    img[2 * SECTOR:2 * SECTOR + len(entries)] = entries

    # The backup, at the end, where the specification puts it.
    backup_entry_lba = total_sectors - 1 - entry_sectors
    backup = gpt_header(total_sectors - 1, 1, first_usable, last_usable,
                        backup_entry_lba, entry_crc, disk_guid)
    img[backup_entry_lba * SECTOR:backup_entry_lba * SECTOR + len(entries)] = entries
    img[(total_sectors - 1) * SECTOR:(total_sectors - 1) * SECTOR + len(backup)] = backup

    # A FAT16 volume in each, so both look mountable and the kernel has to
    # choose on something other than whether it can read them.
    esp = Fat16Builder(esp_sectors * SECTOR // 1024, label="ESP")
    esp.add_file("EFI/BOOT/PLACEHOLD.TXT", b"not the volume to use\n")
    esp_img = esp.build()
    img[esp_start * SECTOR:esp_start * SECTOR + len(esp_img)] = esp_img

    data = Fat16Builder(data_sectors * SECTOR // 1024, label="ZELRDATA")
    data.add_file("HELLO.TXT", b"read from a gpt partition\n")
    data_img = data.build()
    img[data_start * SECTOR:data_start * SECTOR + len(data_img)] = data_img

    open(path, "wb").write(bytes(img))
    print("wrote %s (%d KiB)" % (path, total_kb))
    print("  protective mbr at 0, gpt header at 1, entries at 2")
    print("  esp          lba %d..%d  (%d MiB)"
          % (esp_start, esp_end, esp_sectors * SECTOR // 1048576))
    print("  zelr data     lba %d..%d  (%d MiB)"
          % (data_start, data_end, data_sectors * SECTOR // 1048576))
    return 0


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    return build(args[0],
                 total_kb=int(args[1]) if len(args) > 1 else 65536,
                 bad_header_crc="--bad-header-crc" in flags,
                 bad_entry_crc="--bad-entry-crc" in flags)


if __name__ == "__main__":
    sys.exit(main())
