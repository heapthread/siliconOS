"""Make a small exFAT disk image for testing silicon's (read-only) exFAT support in QEMU.

    python tools/mkexfat.py out.img [size_mb] [label] [--mbr]     creates the image with a few sample files
                                                                    (--mbr puts it in a partition behind an MBR)
Check it independently with 7-Zip:  7z l out.img
"""
import struct
import sys

SECTOR = 512
SPC_SHIFT = 3                       # 4 KiB clusters
CS = SECTOR << SPC_SHIFT


def rol_hash16(h, b):
    return (((h << 15) | (h >> 1)) + b) & 0xFFFF


def upcase_table():
    t = bytearray()
    for c in range(128):
        t += struct.pack("<H", ord(chr(c).upper()) if 97 <= c <= 122 else c)
    return bytes(t)


def upcase_char(ch):
    o = ord(ch)
    return o - 32 if 97 <= o <= 122 else o


def dos_ts(y=2026, mo=11, d=7, h=12, mi=30, s=0):
    return ((y - 1980) << 25) | (mo << 21) | (d << 16) | (h << 11) | (mi << 5) | (s // 2)


class ExFat:
    def __init__(self, size_mb=8, label="EXFATDISK"):
        self.total = size_mb * 1024 * 1024 // SECTOR
        self.fat_off = 32
        spc = 1 << SPC_SHIFT
        est_clusters = (self.total - 64) // spc
        self.fat_len = ((est_clusters + 2) * 4 + SECTOR - 1) // SECTOR
        self.heap_off = ((self.fat_off + self.fat_len + 7) // 8) * 8        # cluster heap aligned to the cluster size
        self.clusters = (self.total - self.heap_off) // spc
        self.img = bytearray(self.total * SECTOR)
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0xFFFFFFF8
        self.fat[1] = 0xFFFFFFFF
        self.bitmap = bytearray((self.clusters + 7) // 8)
        self.label = label
        self.next = 2
        self.bm_cl = self.alloc_contig((len(self.bitmap) + CS - 1) // CS, chain=True)
        self.up = upcase_table()
        self.up_cl = self.alloc_contig((len(self.up) + CS - 1) // CS, chain=True)
        self.write_clusters(self.up_cl, self.up)
        self.root_cl = self.alloc_contig(1, chain=True)
        self.dirs = {self.root_cl: bytearray()}
        self.dir_clusters = {self.root_cl: [self.root_cl]}

    def alloc_contig(self, n, chain):
        first = self.next
        for i in range(n):
            c = self.next + i
            self.bitmap[(c - 2) >> 3] |= 1 << ((c - 2) & 7)
            if chain:
                self.fat[c] = c + 1 if i + 1 < n else 0xFFFFFFFF
        self.next += n
        return first

    def alloc_one(self):
        c = self.next
        self.next += 1
        self.bitmap[(c - 2) >> 3] |= 1 << ((c - 2) & 7)
        return c

    def write_clusters(self, first, data):
        off = (self.heap_off + (first - 2) * (1 << SPC_SHIFT)) * SECTOR
        self.img[off:off + len(data)] = data

    def write_chain(self, chain, data):
        for i, c in enumerate(chain):
            self.write_clusters(c, data[i * CS:(i + 1) * CS])

    @staticmethod
    def entry_set(name, attr, first, size, nofat, ts):
        up = "".join(chr(upcase_char(ch)) for ch in name)
        h = 0
        for b in up.encode("utf-16-le"):
            h = rol_hash16(h, b)
        units = list(name.encode("utf-16-le"))
        chars = [units[i] | (units[i + 1] << 8) for i in range(0, len(units), 2)]
        names = [chars[i:i + 15] for i in range(0, len(chars), 15)]
        sec = 1 + len(names)
        f = bytearray(32)
        f[0] = 0x85
        f[1] = sec
        struct.pack_into("<H", f, 4, attr)
        struct.pack_into("<III", f, 8, ts, ts, ts)
        s = bytearray(32)
        s[0] = 0xC0
        s[1] = 0x03 if nofat else 0x01
        s[3] = len(chars)
        struct.pack_into("<H", s, 4, h)
        struct.pack_into("<Q", s, 8, size)
        struct.pack_into("<I", s, 20, first)
        struct.pack_into("<Q", s, 24, size)
        out = bytearray(f + s)
        for part in names:
            e = bytearray(32)
            e[0] = 0xC1
            for j, ch in enumerate(part):
                struct.pack_into("<H", e, 2 + j * 2, ch)
            out += e
        cs = 0
        for i, b in enumerate(out):
            if i in (2, 3):
                continue
            cs = (((cs << 15) | (cs >> 1)) + b) & 0xFFFF
        struct.pack_into("<H", out, 2, cs)
        return bytes(out)

    def add_file(self, dir_cl, name, data, fragmented=False, other=None):
        nclusters = max(1, (len(data) + CS - 1) // CS) if data else 0
        if not data:
            first, nofat = 0, False
        elif not fragmented:
            first = self.alloc_contig(nclusters, chain=False)
            self.write_chain(range(first, first + nclusters), data)
            nofat = True
        else:                                    # interleave with scratch clusters so the file is not contiguous
            chain = []
            for i in range(nclusters):
                chain.append(self.alloc_one())
                self.alloc_one()                 # a hole owned by nobody (marked used, like another file)
            for i, c in enumerate(chain):
                self.fat[c] = chain[i + 1] if i + 1 < len(chain) else 0xFFFFFFFF
            self.write_chain(chain, data)
            first, nofat = chain[0], False
        self.dirs[dir_cl] += self.entry_set(name, 0x20, first, len(data), nofat, dos_ts())
        return first

    def add_dir(self, parent_cl, name):
        c = self.alloc_contig(1, chain=True)
        self.dirs[c] = bytearray()
        self.dir_clusters[c] = [c]
        self.dirs[parent_cl] += self.entry_set(name, 0x10, c, CS, False, dos_ts())
        return c

    def finish(self, start_lba=0):
        # root directory: label, bitmap, upcase, then the entries
        lab = self.label.encode("utf-16-le")
        e = bytearray(32)
        e[0] = 0x83
        e[1] = len(self.label)
        e[2:2 + len(lab)] = lab
        bm = bytearray(32)
        bm[0] = 0x81
        struct.pack_into("<I", bm, 20, self.bm_cl)
        struct.pack_into("<Q", bm, 24, len(self.bitmap))
        up = bytearray(32)
        up[0] = 0x82
        cs = 0
        for b in self.up:
            cs = (((cs << 31) | (cs >> 1)) + b) & 0xFFFFFFFF
        struct.pack_into("<I", up, 4, cs)
        struct.pack_into("<I", up, 20, self.up_cl)
        struct.pack_into("<Q", up, 24, len(self.up))
        self.dirs[self.root_cl] = bytearray(e + bm + up) + self.dirs[self.root_cl]
        # directories bigger than a cluster get more clusters
        for c, d in list(self.dirs.items()):
            n = max(1, (len(d) + CS - 1) // CS)
            chain = [c] + [self.alloc_one() for _ in range(n - 1)]
            for i, cc in enumerate(chain):
                self.fat[cc] = chain[i + 1] if i + 1 < len(chain) else 0xFFFFFFFF
            self.write_chain(chain, bytes(d))
        self.write_clusters(self.bm_cl, bytes(self.bitmap))
        # FAT
        fat = struct.pack("<%dI" % len(self.fat), *self.fat)
        self.img[self.fat_off * SECTOR:self.fat_off * SECTOR + len(fat)] = fat
        # boot region
        vbr = bytearray(SECTOR)
        vbr[0:3] = bytes([0xEB, 0x76, 0x90])
        vbr[3:11] = b"EXFAT   "
        struct.pack_into("<Q", vbr, 64, start_lba)
        struct.pack_into("<Q", vbr, 72, self.total)
        struct.pack_into("<IIII", vbr, 80, self.fat_off, self.fat_len, self.heap_off, self.clusters)
        struct.pack_into("<I", vbr, 96, self.root_cl)
        struct.pack_into("<I", vbr, 100, 0x51C0E0F4)
        struct.pack_into("<H", vbr, 104, 0x0100)
        struct.pack_into("<H", vbr, 106, 0)
        vbr[108] = 9
        vbr[109] = SPC_SHIFT
        vbr[110] = 1
        vbr[111] = 0x80
        vbr[112] = 0xFF
        vbr[510:512] = bytes([0x55, 0xAA])
        region = bytearray()
        region += vbr
        for i in range(8):
            ext = bytearray(SECTOR)
            struct.pack_into("<I", ext, 508, 0xAA550000)
            region += ext
        region += bytearray(SECTOR)             # OEM parameters (unused)
        region += bytearray(SECTOR)             # reserved
        cs = 0
        for i, b in enumerate(region):
            if i in (106, 107, 112):
                continue
            cs = (((cs << 31) | (cs >> 1)) + b) & 0xFFFFFFFF
        region += struct.pack("<I", cs) * (SECTOR // 4)
        self.img[0:len(region)] = region
        self.img[12 * SECTOR:12 * SECTOR + len(region)] = region
        return bytes(self.img)


if __name__ == "__main__":
    size = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 8
    label = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else "EXFATDISK"
    fs = ExFat(size, label)
    fs.add_file(fs.root_cl, "hello.txt", b"Hello from an exFAT drive!\n")
    docs = fs.add_dir(fs.root_cl, "Documents Folder")
    fs.add_file(docs, "A long file name here, longer than fifteen characters.txt", b"long names work\n" * 50)
    fs.add_file(fs.root_cl, "contiguous.bin", bytes((i * 7) % 256 for i in range(300000)))
    fs.add_file(fs.root_cl, "fragmented.bin", bytes((i * 13 + 5) % 256 for i in range(200000)), fragmented=True)
    fs.add_file(fs.root_cl, "empty.txt", b"")
    img = fs.finish()
    if "--mbr" in sys.argv:
        start = 2048
        mbr = bytearray(SECTOR)
        mbr[446 + 4] = 0x07
        struct.pack_into("<II", mbr, 446 + 8, start, len(img) // SECTOR)
        mbr[510:512] = bytes([0x55, 0xAA])
        img = bytes(mbr) + bytes(start * SECTOR - SECTOR) + img
    open(sys.argv[1], "wb").write(img)
    print("wrote", sys.argv[1], len(img) // 1024, "KiB")
