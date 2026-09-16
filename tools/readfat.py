#!/usr/bin/env python3
"""Independent FAT16 reader for a raw disk image.

The kernel writing a filesystem it can read back only proves it is
self-consistent. This parses the image from the specification instead, so a
disagreement means the kernel is wrong rather than merely idiosyncratic.

    python tools/readfat.py zelr.img              list the volume
    python tools/readfat.py zelr.img FILE         print one file
    python tools/readfat.py zelr.img --put SRC [AS]   copy a file in
"""
import struct
import sys

SECTOR = 512

def lfn_checksum(short11):
    """The checksum a long name entry carries of its short name."""
    total = 0
    for b in short11:
        total = (((total & 1) << 7) + (total >> 1) + b) & 0xFF
    return total




class Fat16:
    def __init__(self, path, writable=False):
        self.f = open(path, "r+b" if writable else "rb")
        boot = self._sector(0)

        if boot[510] != 0x55 or boot[511] != 0xAA:
            raise ValueError("no 0x55AA boot signature: not a formatted volume")

        self.bytes_per_sector = struct.unpack_from("<H", boot, 11)[0]
        self.sectors_per_cluster = boot[13]
        self.reserved = struct.unpack_from("<H", boot, 14)[0]
        self.num_fats = boot[16]
        self.root_entries = struct.unpack_from("<H", boot, 17)[0]
        total16 = struct.unpack_from("<H", boot, 19)[0]
        self.media = boot[21]
        self.fat_sectors = struct.unpack_from("<H", boot, 22)[0]
        total32 = struct.unpack_from("<I", boot, 32)[0]
        self.total_sectors = total16 or total32
        self.label = boot[43:54].decode("ascii", "replace").strip()
        self.fs_type = boot[54:62].decode("ascii", "replace").strip()

        if self.bytes_per_sector != SECTOR:
            raise ValueError(f"unexpected sector size {self.bytes_per_sector}")

        self.fat_start = self.reserved
        self.root_sectors = (self.root_entries * 32 + SECTOR - 1) // SECTOR
        self.root_start = self.fat_start + self.num_fats * self.fat_sectors
        self.data_start = self.root_start + self.root_sectors
        self.clusters = (self.total_sectors - self.data_start) // self.sectors_per_cluster

        if not (4085 <= self.clusters <= 65524):
            raise ValueError(
                f"cluster count {self.clusters} is outside the FAT16 range; "
                "this volume would be read as FAT12 or FAT32"
            )

        # mutable: put_file edits entries in place before flushing
        self.fat = bytearray(self._read(self.fat_start, self.fat_sectors))

    def _sector(self, lba):
        self.f.seek(lba * SECTOR)
        return self.f.read(SECTOR)

    def _read(self, lba, count):
        self.f.seek(lba * SECTOR)
        return self.f.read(count * SECTOR)

    def fat_entry(self, cluster):
        return struct.unpack_from("<H", self.fat, cluster * 2)[0]

    def fats_agree(self):
        """Every copy of the table must be identical or repair tools complain."""
        first = self._read(self.fat_start, self.fat_sectors)
        for i in range(1, self.num_fats):
            other = self._read(self.fat_start + i * self.fat_sectors, self.fat_sectors)
            if other != first:
                return False
        return True

    def entries(self, cluster=None):
        """Directory contents. The root is a fixed run of sectors; every other
        directory is an ordinary cluster chain, so they are read differently
        and then look the same."""
        if cluster is None:
            raw = self._read(self.root_start, self.root_sectors)
            limit = self.root_entries
        else:
            raw = b""
            for c in self.chain(cluster):
                lba = self.data_start + (c - 2) * self.sectors_per_cluster
                raw += self._read(lba, self.sectors_per_cluster)
            limit = len(raw) // 32

        out = []
        pending = {}          # sequence number -> its thirteen characters
        pending_sum = None

        for i in range(limit):
            e = raw[i * 32:(i + 1) * 32]
            if not e or e[0] == 0x00:
                break
            if e[0] == 0xE5:
                pending, pending_sum = {}, None
                continue
            attr = e[11]

            if attr & 0x0F == 0x0F:
                # A fragment of a long name. They come before the short entry
                # they belong to, numbered backwards, and each carries a
                # checksum of that short name so that a volume edited by
                # something which does not understand them can be spotted.
                seq = e[0] & 0x1F
                chars = e[1:11] + e[14:26] + e[28:32]
                text = ""
                for k in range(0, len(chars), 2):
                    code = chars[k] | (chars[k + 1] << 8)
                    if code in (0x0000, 0xFFFF):
                        break
                    text += chr(code)
                if pending_sum is not None and pending_sum != e[13]:
                    pending = {}
                pending_sum = e[13]
                pending[seq] = text
                continue

            if attr & 0x08:              # volume label
                pending, pending_sum = {}, None
                continue

            name = e[0:8].decode("ascii", "replace").rstrip()
            ext = e[8:11].decode("ascii", "replace").rstrip()
            full = f"{name}.{ext}" if ext else name

            # The long name wins, when there is a complete one whose checksum
            # matches the short name sitting here.
            if pending and pending_sum == lfn_checksum(e[0:11]):
                parts = [pending[k] for k in sorted(pending) if k in pending]
                if len(parts) == max(pending):
                    full = "".join(parts)
            pending, pending_sum = {}, None
            first = struct.unpack_from("<H", e, 26)[0]
            size = struct.unpack_from("<I", e, 28)[0]
            out.append((full, first, size, attr))
        return out

    def walk(self, cluster=None, prefix="/", depth=0):
        """Every entry in the tree, deepest last. Guards against a directory
        that points at itself, which a damaged image can do."""
        if depth > 16:
            return
        for name, first, size, attr in self.entries(cluster):
            if name.startswith("."):
                continue
            is_dir = bool(attr & 0x10)
            yield (prefix + name, first, size, is_dir, depth)
            if is_dir and first >= 2:
                yield from self.walk(first, prefix + name + "/", depth + 1)

    def chain(self, start):
        out = []
        c = start
        guard = 0
        while 2 <= c < 0xFFF8 and guard < 100000:
            out.append(c)
            c = self.fat_entry(c)
            guard += 1
        return out

    def read_file(self, path):
        """Reads by path, so "/docs/deep/x.txt" works and not only a root
        level name."""
        want = "/" + path.strip("/")
        for full, cluster, size, is_dir, _ in self.walk():
            if is_dir:
                continue
            if full.upper() == want.upper():
                data = b""
                for c in self.chain(cluster):
                    lba = self.data_start + (c - 2) * self.sectors_per_cluster
                    data += self._read(lba, self.sectors_per_cluster)
                return data[:size]
        return None


    # --- writing -----------------------------------------------------------

    def _write(self, lba, data):
        self.f.seek(lba * SECTOR)
        self.f.write(data)

    def _set_fat(self, cluster, value):
        struct.pack_into("<H", self.fat, cluster * 2, value)

    def _flush_fat(self):
        """Both copies, so the volume stays consistent for other readers."""
        for i in range(self.num_fats):
            self._write(self.fat_start + i * self.fat_sectors, bytes(self.fat))

    def _free_clusters(self, n):
        out = []
        for c in range(2, self.clusters + 2):
            if self.fat_entry(c) == 0:
                out.append(c)
                if len(out) == n:
                    return out
        raise ValueError("not enough free space on the volume")

    def put_file(self, name, data):
        name = name.upper()
        base, _, ext = name.partition(".")
        if len(base) > 8 or len(ext) > 3:
            raise ValueError(f"{name!r} does not fit an 8.3 name")

        per_cluster = self.sectors_per_cluster * SECTOR
        need = max(1, (len(data) + per_cluster - 1) // per_cluster) if data else 0
        chain = self._free_clusters(need) if need else []

        for i, c in enumerate(chain):
            last = i == len(chain) - 1
            self._set_fat(c, 0xFFFF if last else chain[i + 1])
            lba = self.data_start + (c - 2) * self.sectors_per_cluster
            chunk = data[i * per_cluster:(i + 1) * per_cluster]
            self._write(lba, chunk.ljust(per_cluster, bytes([0])))
        self._flush_fat()

        entry = bytearray(32)
        entry[0:8] = base.ljust(8).encode("ascii")
        entry[8:11] = ext.ljust(3).encode("ascii")
        entry[11] = 0x20                                   # archive
        struct.pack_into("<H", entry, 22, 0)               # time
        struct.pack_into("<H", entry, 24, 0x5A21)          # date
        struct.pack_into("<H", entry, 26, chain[0] if chain else 0)
        struct.pack_into("<I", entry, 28, len(data))

        raw = bytearray(self._read(self.root_start, self.root_sectors))
        slot = None
        for i in range(self.root_entries):
            first = raw[i * 32]
            if first in (0x00, 0xE5):
                slot = i
                break
            existing = raw[i * 32:i * 32 + 11]
            if existing == entry[0:11]:                    # replace in place
                slot = i
                break
        if slot is None:
            raise ValueError("root directory is full")

        raw[slot * 32:(slot + 1) * 32] = entry
        self._write(self.root_start, bytes(raw))
        self.f.flush()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    if len(sys.argv) >= 3 and sys.argv[2] == "--put":
        if len(sys.argv) < 4:
            print("usage: readfat.py IMAGE --put SRC [AS]", file=sys.stderr)
            return 2
        src = sys.argv[3]
        as_name = sys.argv[4] if len(sys.argv) > 4 else src.replace("\\", "/").split("/")[-1]
        fs = Fat16(sys.argv[1], writable=True)
        with open(src, "rb") as fh:
            data = fh.read()
        fs.put_file(as_name, data)
        print(f"wrote {as_name} ({len(data)} bytes) into {sys.argv[1]}")
        return 0

    fs = Fat16(sys.argv[1])

    if len(sys.argv) >= 3:
        data = fs.read_file(sys.argv[2])
        if data is None:
            print(f"{sys.argv[2]}: not found", file=sys.stderr)
            return 1
        sys.stdout.write(data.decode("utf-8", "replace"))
        return 0

    print(f"label            {fs.label!r}")
    print(f"reported type    {fs.fs_type!r}")
    print(f"sector size      {fs.bytes_per_sector}")
    print(f"cluster          {fs.sectors_per_cluster} sectors "
          f"({fs.sectors_per_cluster * SECTOR} bytes)")
    print(f"fat copies       {fs.num_fats} x {fs.fat_sectors} sectors")
    print(f"root entries     {fs.root_entries}")
    print(f"total sectors    {fs.total_sectors}")
    print(f"data clusters    {fs.clusters}  (FAT16 range is 4085..65524)")
    print(f"fat copies match {fs.fats_agree()}")
    print(f"reserved entry0  {fs.fat_entry(0):#06x}   entry1 {fs.fat_entry(1):#06x}")
    print()

    tree = list(fs.walk())
    files = [t for t in tree if not t[3]]
    dirs = [t for t in tree if t[3]]
    print(f"{len(files)} file(s) in {len(dirs) + 1} director"
          f"{'y' if not dirs else 'ies'}:")

    for name, cluster, size, is_dir, depth in tree:
        indent = "  " * depth
        leaf = name.rsplit("/", 1)[-1]
        if is_dir:
            print(f"     <dir>  {indent}{leaf}/")
            continue
        chain = fs.chain(cluster)
        need = (size + fs.sectors_per_cluster * SECTOR - 1) // (fs.sectors_per_cluster * SECTOR)
        need = max(need, 1) if size else 0
        status = "ok" if len(chain) >= need else f"SHORT CHAIN ({len(chain)} < {need})"
        print(f"  {size:8d}  {indent}{leaf:<14} cluster {cluster:<6} "
              f"chain {len(chain):<4} {status}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
