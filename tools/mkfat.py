#!/usr/bin/env python3
"""Builds a FAT16 or FAT32 filesystem image, from the specification.

This exists because the UEFI half of a bootable image is an EFI System
Partition, and an ESP is a FAT filesystem. The usual way to make one is
mtools or a loopback mount, and zelr does not use either: the kernel already
implements FAT16 from the specification and so does the host-side reader, so
this is the third implementation of the same document and the one that has to
create a volume rather than read it.

  python tools/mkfat.py [--fat32] [--at-cluster=N] OUT.img SIZE_KB FILE[:PATH] ...

A path may name a directory, which is created as needed:

  python tools/mkfat.py esp.img 2048 BOOTX64.EFI:EFI/BOOT/BOOTX64.EFI
"""
import os
import struct
import sys

SECTOR = 512


class Fat16Builder:
    """Lays out a volume in memory, then writes it in one go.

    Cluster allocation is sequential and nothing is ever deleted, which is
    what an image built once and never modified allows."""

    def __init__(self, size_kb, label="ZELR"):
        self.total_sectors = (size_kb * 1024) // SECTOR

        # FAT16 needs at least 4085 clusters to be FAT16 at all, and no more
        # than 65524 to still be. The cluster size is what moves a given
        # volume into that window, so pick the smallest one that does rather
        # than forcing the image to be large enough to suit a fixed choice.
        self.sectors_per_cluster = 0
        for spc in (1, 2, 4, 8, 16, 32, 64):
            approx = self.total_sectors // spc
            if 4200 <= approx <= 65000:
                self.sectors_per_cluster = spc
                break
        if not self.sectors_per_cluster:
            self.sectors_per_cluster = 4

        self.reserved = 4
        self.num_fats = 2
        self.root_entries = 512
        self.label = label

        self.root_sectors = (self.root_entries * 32 + SECTOR - 1) // SECTOR

        # The table has to describe the clusters that are left after the table
        # itself is subtracted, so solve for its size.
        self.fat_sectors = 1
        for _ in range(16):
            data = (self.total_sectors - self.reserved
                    - self.num_fats * self.fat_sectors - self.root_sectors)
            clusters = data // self.sectors_per_cluster
            need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
            if need == self.fat_sectors:
                break
            self.fat_sectors = need

        self.fat_start = self.reserved
        self.root_start = self.fat_start + self.num_fats * self.fat_sectors
        self.data_start = self.root_start + self.root_sectors
        self.clusters = ((self.total_sectors - self.data_start)
                         // self.sectors_per_cluster)

        if self.clusters < 4085:
            raise SystemExit(
                "%d KiB is too small for FAT16: %d clusters, and the format "
                "needs 4085" % (size_kb, self.clusters))
        if self.clusters > 65524:
            raise SystemExit("%d KiB is too large for FAT16" % size_kb)

        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0xFFF8
        self.fat[1] = 0xFFFF
        self.next_free = 2

        self.cluster_bytes = self.sectors_per_cluster * SECTOR
        self.data = bytearray(self.clusters * self.cluster_bytes)

        # name -> (first cluster, size, is_dir). The root is not in here: it
        # lives in its own fixed area rather than in a cluster chain.
        self.root = []
        self.dirs = {}          # path -> list of entries

    # --- allocation ------------------------------------------------------

    def alloc_chain(self, nbytes):
        """A run of clusters, linked, with the last marked as the end."""
        n = max(1, (nbytes + self.cluster_bytes - 1) // self.cluster_bytes)
        if self.next_free + n > self.clusters + 2:
            raise SystemExit("the image is full")
        first = self.next_free
        for i in range(n):
            c = first + i
            self.fat[c] = 0xFFFF if i == n - 1 else c + 1
        self.next_free += n
        return first, n

    def write_cluster(self, cluster, data, offset=0):
        at = (cluster - 2) * self.cluster_bytes + offset
        self.data[at:at + len(data)] = data

    def write_chain(self, first, data):
        c = first
        done = 0
        while done < len(data):
            chunk = data[done:done + self.cluster_bytes]
            self.write_cluster(c, chunk)
            done += len(chunk)
            c = self.fat[c]
            if c >= 0xFFF8:
                break

    # --- names -----------------------------------------------------------

    @staticmethod
    def to_83(name):
        name = name.upper()
        if "." in name:
            base, ext = name.rsplit(".", 1)
        else:
            base, ext = name, ""
        return (base[:8].ljust(8) + ext[:3].ljust(3)).encode("ascii")

    @staticmethod
    def dir_record(name83, cluster, size, is_dir):
        rec = bytearray(32)
        rec[0:11] = name83
        rec[11] = 0x10 if is_dir else 0x20
        struct.pack_into("<H", rec, 22, 0)          # time
        struct.pack_into("<H", rec, 24, 0x5A21)     # date
        struct.pack_into("<H", rec, 26, cluster)
        struct.pack_into("<I", rec, 28, 0 if is_dir else size)
        return bytes(rec)

    # --- the tree --------------------------------------------------------

    def ensure_dir(self, path):
        """Creates a directory and everything above it. Returns its first
        cluster, or None for the root."""
        path = path.strip("/")
        if not path:
            return None
        if path in self.dirs:
            return self.dirs[path][0]

        parent_path, _, name = path.rpartition("/")
        parent_cluster = self.ensure_dir(parent_path) if parent_path else None

        cluster, _ = self.alloc_chain(self.cluster_bytes)

        # A directory begins with the two entries that make it navigable. ".."
        # pointing at cluster 0 means the root, which is what the format says
        # even though the root has no cluster of its own.
        entries = bytearray()
        entries += self.dir_record(b".          ", cluster, 0, True)
        entries += self.dir_record(b"..         ", parent_cluster or 0, 0, True)
        self.write_cluster(cluster, bytes(entries))

        self.dirs[path] = (cluster, len(entries))
        self._add_entry(parent_path, self.dir_record(self.to_83(name), cluster, 0, True))
        return cluster

    def _add_entry(self, dir_path, record):
        dir_path = dir_path.strip("/")
        if not dir_path:
            self.root.append(record)
            return
        cluster, used = self.dirs[dir_path]
        if used + 32 > self.cluster_bytes:
            raise SystemExit("directory %s is full" % dir_path)
        self.write_cluster(cluster, record, used)
        self.dirs[dir_path] = (cluster, used + 32)

    def add_file(self, path, content):
        path = path.strip("/")
        dir_path, _, name = path.rpartition("/")
        if dir_path:
            self.ensure_dir(dir_path)

        first, _ = self.alloc_chain(len(content))
        self.write_chain(first, content)
        self._add_entry(dir_path, self.dir_record(self.to_83(name), first,
                                                  len(content), False))

    # --- output ----------------------------------------------------------

    def boot_sector(self):
        s = bytearray(SECTOR)
        s[0:3] = bytes([0xEB, 0x3C, 0x90])
        s[3:11] = b"ZELR    "
        struct.pack_into("<H", s, 11, SECTOR)
        s[13] = self.sectors_per_cluster
        struct.pack_into("<H", s, 14, self.reserved)
        s[16] = self.num_fats
        struct.pack_into("<H", s, 17, self.root_entries)
        struct.pack_into("<H", s, 19,
                         self.total_sectors if self.total_sectors < 0x10000 else 0)
        s[21] = 0xF8
        struct.pack_into("<H", s, 22, self.fat_sectors)
        struct.pack_into("<H", s, 24, 32)       # sectors per track
        struct.pack_into("<H", s, 26, 8)        # heads
        struct.pack_into("<I", s, 28, 0)        # hidden sectors
        struct.pack_into("<I", s, 32,
                         self.total_sectors if self.total_sectors >= 0x10000 else 0)
        s[36] = 0x80
        s[38] = 0x29
        struct.pack_into("<I", s, 39, 0x5A4C5202)
        s[43:54] = self.label.upper().ljust(11)[:11].encode("ascii")
        s[54:62] = b"FAT16   "
        s[510] = 0x55
        s[511] = 0xAA
        return bytes(s)

    def build(self):
        img = bytearray(self.total_sectors * SECTOR)

        img[0:SECTOR] = self.boot_sector()

        fat_bytes = bytearray(self.fat_sectors * SECTOR)
        for i, v in enumerate(self.fat):
            struct.pack_into("<H", fat_bytes, i * 2, v)
        for copy in range(self.num_fats):
            at = (self.fat_start + copy * self.fat_sectors) * SECTOR
            img[at:at + len(fat_bytes)] = fat_bytes

        root_bytes = b"".join(self.root)
        if len(root_bytes) > self.root_sectors * SECTOR:
            raise SystemExit("too many entries in the root directory")
        at = self.root_start * SECTOR
        img[at:at + len(root_bytes)] = root_bytes

        at = self.data_start * SECTOR
        img[at:at + len(self.data)] = self.data
        return bytes(img)


class Fat32Builder:
    """The same document, the other variant.

    FAT32 differs from FAT16 in three ways that matter here: table entries
    are four bytes rather than two, the root directory is an ordinary cluster
    chain instead of a fixed run of sectors, and a directory entry's cluster
    number is split across two fields sixteen bytes apart. The third is the
    one that bites: write only the low half and a volume reads back correctly
    until something lands above cluster 65535.

    Which variant a volume is is decided by its cluster count and nothing
    else, so this refuses to produce one small enough to be read as FAT16.
    """

    def __init__(self, size_kb, label="ZELR32"):
        self.total_sectors = (size_kb * 1024) // SECTOR

        # Small clusters, so a modest image still clears 65524 of them. One
        # sector per cluster needs about 33 MiB; anything larger can afford
        # more per cluster but there is no reason to here.
        self.sectors_per_cluster = 1
        self.reserved = 32              # boot sector, fsinfo, backup at 6
        self.num_fats = 2
        self.label = label

        self.fat_sectors = 1
        for _ in range(32):
            data = (self.total_sectors - self.reserved
                    - self.num_fats * self.fat_sectors)
            clusters = data // self.sectors_per_cluster
            need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if need == self.fat_sectors:
                break
            self.fat_sectors = need

        self.fat_start = self.reserved
        self.data_start = self.fat_start + self.num_fats * self.fat_sectors
        self.clusters = ((self.total_sectors - self.data_start)
                         // self.sectors_per_cluster)

        if self.clusters <= 65524:
            raise SystemExit(
                "%d KiB gives %d clusters, which is FAT16 by the only "
                "definition that counts; FAT32 needs more than 65524"
                % (size_kb, self.clusters))

        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.next_free = 2

        self.cluster_bytes = self.sectors_per_cluster * SECTOR
        self.data = bytearray(self.clusters * self.cluster_bytes)

        # The root is a directory like any other, keyed by the empty string,
        # which is what removes the special case FAT16 needs everywhere.
        self.root_cluster, _ = self.alloc_chain(self.cluster_bytes)
        self.dirs = {"": (self.root_cluster, 0)}

    # --- allocation ------------------------------------------------------

    def alloc_chain(self, nbytes):
        n = max(1, (nbytes + self.cluster_bytes - 1) // self.cluster_bytes)
        if self.next_free + n > self.clusters + 2:
            raise SystemExit("the image is full")
        first = self.next_free
        for i in range(n):
            c = first + i
            self.fat[c] = 0x0FFFFFFF if i == n - 1 else c + 1
        self.next_free += n
        return first, n

    def write_cluster(self, cluster, data, offset=0):
        at = (cluster - 2) * self.cluster_bytes + offset
        self.data[at:at + len(data)] = data

    def write_chain(self, first, data):
        c = first
        done = 0
        while done < len(data):
            chunk = data[done:done + self.cluster_bytes]
            self.write_cluster(c, chunk)
            done += len(chunk)
            c = self.fat[c]
            if c >= 0x0FFFFFF8:
                break

    # --- the tree --------------------------------------------------------

    @staticmethod
    def dir_record(name83, cluster, size, is_dir):
        rec = bytearray(32)
        rec[0:11] = name83
        rec[11] = 0x10 if is_dir else 0x20
        struct.pack_into("<H", rec, 20, (cluster >> 16) & 0xFFFF)   # high half
        struct.pack_into("<H", rec, 22, 0)                          # time
        struct.pack_into("<H", rec, 24, 0x5A21)                     # date
        struct.pack_into("<H", rec, 26, cluster & 0xFFFF)           # low half
        struct.pack_into("<I", rec, 28, 0 if is_dir else size)
        return bytes(rec)

    def ensure_dir(self, path):
        path = path.strip("/")
        if not path:
            return self.root_cluster
        if path in self.dirs:
            return self.dirs[path][0]

        parent_path, _, name = path.rpartition("/")
        parent_cluster = self.ensure_dir(parent_path)

        cluster, _ = self.alloc_chain(self.cluster_bytes)
        entries = bytearray()
        entries += self.dir_record(b".          ", cluster, 0, True)
        # ".." naming the root is written as zero even here, where the root
        # does have a cluster of its own. The specification says so.
        up = 0 if parent_cluster == self.root_cluster else parent_cluster
        entries += self.dir_record(b"..         ", up, 0, True)
        self.write_cluster(cluster, bytes(entries))

        self.dirs[path] = (cluster, len(entries))
        self._add_entry(parent_path,
                        self.dir_record(Fat16Builder.to_83(name), cluster, 0, True))
        return cluster

    def _add_entry(self, dir_path, record):
        dir_path = dir_path.strip("/")
        cluster, used = self.dirs[dir_path]
        if used + 32 > self.cluster_bytes:
            raise SystemExit("directory '%s' is full" % (dir_path or "/"))
        self.write_cluster(cluster, record, used)
        self.dirs[dir_path] = (cluster, used + 32)

    def skip_to_cluster(self, cluster):
        """Moves allocation forward, so the next file lands at a high cluster
        number.

        This exists for one test. A cluster number is split across two fields
        in a directory entry, and a reader that takes only the low half is
        correct for every file below cluster 65536 and wrong for every file
        above it. An image whose files all sit near the front cannot tell the
        two apart, so the test needs a file that is deliberately out there."""
        if cluster <= self.next_free:
            return
        if cluster >= self.clusters + 2:
            raise SystemExit("cluster %d is past the end of the volume" % cluster)
        self.next_free = cluster

    def add_file(self, path, content):
        path = path.strip("/")
        dir_path, _, name = path.rpartition("/")
        if dir_path:
            self.ensure_dir(dir_path)
        first, _ = self.alloc_chain(len(content))
        self.write_chain(first, content)
        self._add_entry(dir_path,
                        self.dir_record(Fat16Builder.to_83(name), first,
                                        len(content), False))
        return first

    # --- output ----------------------------------------------------------

    def boot_sector(self):
        s = bytearray(SECTOR)
        s[0:3] = bytes([0xEB, 0x58, 0x90])
        s[3:11] = b"ZELR    "
        struct.pack_into("<H", s, 11, SECTOR)
        s[13] = self.sectors_per_cluster
        struct.pack_into("<H", s, 14, self.reserved)
        s[16] = self.num_fats
        struct.pack_into("<H", s, 17, 0)            # no fixed root on FAT32
        struct.pack_into("<H", s, 19, 0)            # the 16 bit total is unused
        s[21] = 0xF8
        struct.pack_into("<H", s, 22, 0)            # zero here means look at 36
        struct.pack_into("<H", s, 24, 32)
        struct.pack_into("<H", s, 26, 8)
        struct.pack_into("<I", s, 28, 0)
        struct.pack_into("<I", s, 32, self.total_sectors)
        struct.pack_into("<I", s, 36, self.fat_sectors)
        struct.pack_into("<H", s, 40, 0)            # flags: both tables live
        struct.pack_into("<H", s, 42, 0)            # version
        struct.pack_into("<I", s, 44, self.root_cluster)
        struct.pack_into("<H", s, 48, 1)            # fsinfo sector
        struct.pack_into("<H", s, 50, 6)            # backup boot sector
        s[64] = 0x80
        s[66] = 0x29
        struct.pack_into("<I", s, 67, 0x5A4C5200)
        s[71:82] = self.label.upper()[:11].ljust(11).encode("ascii")
        s[82:90] = b"FAT32   "
        s[510], s[511] = 0x55, 0xAA
        return bytes(s)

    def fsinfo_sector(self):
        s = bytearray(SECTOR)
        struct.pack_into("<I", s, 0, 0x41615252)
        struct.pack_into("<I", s, 484, 0x61417272)
        struct.pack_into("<I", s, 488, self.clusters - (self.next_free - 2))
        struct.pack_into("<I", s, 492, self.next_free)
        s[510], s[511] = 0x55, 0xAA
        return bytes(s)

    def build(self):
        img = bytearray(self.total_sectors * SECTOR)
        boot = self.boot_sector()
        img[0:SECTOR] = boot
        img[SECTOR:2 * SECTOR] = self.fsinfo_sector()
        img[6 * SECTOR:7 * SECTOR] = boot          # the backup copy

        table = bytearray(self.fat_sectors * SECTOR)
        for i, v in enumerate(self.fat):
            struct.pack_into("<I", table, i * 4, v & 0x0FFFFFFF)
        for copy in range(self.num_fats):
            at = (self.fat_start + copy * self.fat_sectors) * SECTOR
            img[at:at + len(table)] = table

        at = self.data_start * SECTOR
        img[at:at + len(self.data)] = self.data
        return bytes(img)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2

    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    want32 = "--fat32" in sys.argv
    # Where to put the next file, so a test can place one past cluster 65535.
    skip = 0
    for a in sys.argv[1:]:
        if a.startswith("--at-cluster="):
            skip = int(a.split("=", 1)[1])

    out = args[0]
    size_kb = int(args[1])
    fs = Fat32Builder(size_kb) if want32 else Fat16Builder(size_kb)

    for spec in args[2:]:
        # Split on the last colon, not the first: a Windows path starts with
        # a drive letter and a colon, and splitting there would leave a source
        # of "C" and a destination of the rest.
        src, sep, dest = spec.rpartition(":")
        if not sep or not os.path.exists(src):
            src, dest = spec, os.path.basename(spec)
        if skip:
            fs.skip_to_cluster(skip)
            skip = 0
        with open(src, "rb") as f:
            first = fs.add_file(dest, f.read())
        print("  %-28s %d bytes%s"
              % (dest, os.path.getsize(src),
                 "  at cluster %d" % first if first else ""))

    open(out, "wb").write(fs.build())
    print("wrote %s (%d KiB, FAT%d, %d clusters)"
          % (out, size_kb, 32 if want32 else 16, fs.clusters))
    return 0


if __name__ == "__main__":
    sys.exit(main())
