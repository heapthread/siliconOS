"""Make a small FAT12 / FAT16 / FAT32 disk image for testing USB drives in QEMU.

    python tools/mkfat.py out.img [size_mb] [label] [--fat16|--fat12]     creates the image with a few sample files
    python tools/mkfat.py --ls out.img                            lists what is on an image (a tiny FAT reader)
"""
import struct
import sys

SECTOR = 512


class Fat:
    def __init__(self, size_mb=64, label="SILICONUSB", fat16=False, fat12=False):
        self.fat12 = fat12
        fat16 = fat16 or fat12
        self.fat16 = fat16
        self.total = size_mb * 1024 * 1024 // SECTOR
        self.spc = 2 if fat16 else 1
        self.reserved = 1 if fat16 else 32
        self.root_sectors = 32 if fat16 else 0
        clusters = (self.total - self.reserved - self.root_sectors) // self.spc
        self.fat_sectors = (((clusters + 2) * 3 + 1) // 2 + SECTOR - 1) // SECTOR if fat12 else ((clusters + 2) * (2 if fat16 else 4) + SECTOR - 1) // SECTOR
        self.root_start = self.reserved + 2 * self.fat_sectors
        self.data_start = self.root_start + self.root_sectors
        self.clusters = (self.total - self.data_start) // self.spc
        self.EOC = 0xFFF if fat12 else 0xFFFF if fat16 else 0x0FFFFFFF
        self.img = bytearray(self.total * SECTOR)
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0xFF8 if fat12 else 0xFFF8 if fat16 else 0x0FFFFFF8
        self.fat[1] = self.EOC
        self.next = 3
        self.dirs = {2: bytearray()}          # cluster 2 stands for the root directory
        if not fat16:
            self.fat[2] = self.EOC            # FAT32 root lives in cluster 2
        self.used = {}
        boot = bytearray(SECTOR)
        boot[0:3] = bytes([0xEB, 0x58, 0x90])
        boot[3:11] = b"SILICON "
        lab = label.upper().ljust(11)[:11].encode()
        if fat16:
            t16 = self.total if self.total < 65536 else 0
            struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, self.spc, self.reserved, 2, 512, t16, 0xF8,
                             self.fat_sectors, 63, 255, 0, 0 if t16 else self.total)
            boot[36] = 0x80
            boot[38] = 0x29
            struct.pack_into("<I", boot, 39, 0x51C0B10C)
            boot[43:54] = lab
            boot[54:62] = b"FAT12   " if fat12 else b"FAT16   "
        else:
            struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, self.spc, self.reserved, 2, 0, 0, 0xF8, 0, 63, 255, 0, self.total)
            struct.pack_into("<IHHIHH", boot, 36, self.fat_sectors, 0, 0, 2, 1, 6)
            boot[64] = 0x80
            boot[66] = 0x29
            struct.pack_into("<I", boot, 67, 0x51C0B10C)
            boot[71:82] = lab
            boot[82:90] = b"FAT32   "
        boot[510:512] = bytes([0x55, 0xAA])
        self.img[0:SECTOR] = boot
        if not fat16:
            self.img[6 * SECTOR:7 * SECTOR] = boot
            fsi = bytearray(SECTOR)
            struct.pack_into("<I", fsi, 0, 0x41615252)
            struct.pack_into("<I", fsi, 484, 0x61417272)
            struct.pack_into("<II", fsi, 488, 0xFFFFFFFF, 0xFFFFFFFF)
            fsi[510:512] = bytes([0x55, 0xAA])
            self.img[SECTOR:2 * SECTOR] = fsi

    def csize(self):
        return self.spc * SECTOR

    def alloc(self, data):
        n = max(1, (len(data) + self.csize() - 1) // self.csize())
        first = self.next
        for i in range(n):
            c = self.next
            self.next += 1
            self.fat[c] = c + 1 if i + 1 < n else self.EOC
            off = (self.data_start + (c - 2) * self.spc) * SECTOR
            chunk = data[i * self.csize():(i + 1) * self.csize()]
            self.img[off:off + len(chunk)] = chunk
        return first

    @staticmethod
    def short(name, used):
        base, _, ext = name.upper().partition(".")
        base = "".join(ch for ch in base if ch.isalnum() or ch in "_-")[:6] or "FILE"
        n = 1
        while True:
            cand = (base + "~%d" % n)[:8].ljust(8) + ext[:3].ljust(3)
            if cand not in used:
                used.add(cand)
                return cand.encode()
            n += 1

    def entries(self, name, attr, cluster, size, used):
        s = self.short(name, used)
        csum = 0
        for b in s:
            csum = (((csum & 1) << 7) + (csum >> 1) + b) & 0xFF
        out = bytearray()
        chars = [ord(c) for c in name] + [0]
        chars += [0xFFFF] * ((13 - len(chars) % 13) % 13)
        parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
        for idx in range(len(parts) - 1, -1, -1):
            e = bytearray(32)
            e[0] = (idx + 1) | (0x40 if idx == len(parts) - 1 else 0)
            e[11] = 0x0F
            e[13] = csum
            p = parts[idx]
            e[1:11] = struct.pack("<5H", *p[0:5])
            e[14:26] = struct.pack("<6H", *p[5:11])
            e[28:32] = struct.pack("<2H", *p[11:13])
            out += e
        d = bytearray(32)
        d[0:11] = s
        d[11] = attr
        struct.pack_into("<H", d, 14, 0x21)
        struct.pack_into("<H", d, 16, 0x2A21)
        struct.pack_into("<H", d, 18, 0x2A21)
        struct.pack_into("<H", d, 20, cluster >> 16)
        struct.pack_into("<H", d, 22, 0x21)
        struct.pack_into("<H", d, 24, 0x2A21)
        struct.pack_into("<H", d, 26, cluster & 0xFFFF)
        struct.pack_into("<I", d, 28, size)
        return bytes(out + d)

    def add(self, dir_cluster, name, data=None):
        used = self.used.setdefault(dir_cluster, set())
        if data is None:                                     # a directory
            c = self.next
            self.next += 1
            self.fat[c] = self.EOC
            self.dirs[c] = bytearray()
            dot = bytearray(32)
            dot[0:11] = b".          "
            dot[11] = 0x10
            dot[20:22] = struct.pack("<H", c >> 16)
            dot[26:28] = struct.pack("<H", c & 0xFFFF)
            dd = bytearray(32)
            dd[0:11] = b"..         "
            dd[11] = 0x10
            pc = 0 if dir_cluster == 2 else dir_cluster
            dd[20:22] = struct.pack("<H", pc >> 16)
            dd[26:28] = struct.pack("<H", pc & 0xFFFF)
            self.dirs[c] += dot + dd
            self.dirs[dir_cluster] += self.entries(name, 0x10, c, 0, used)
            return c
        first = self.alloc(data) if data else 0
        self.dirs[dir_cluster] += self.entries(name, 0x20, first, len(data), used)
        return first

    def finish(self):
        for c, d in self.dirs.items():
            data = bytes(d)
            if c == 2 and self.fat16:
                off = self.root_start * SECTOR
                self.img[off:off + len(data)] = data
                continue
            n = max(1, (len(data) + self.csize() - 1) // self.csize())
            chain = [c] + [self.next + i for i in range(n - 1)]
            self.next += n - 1
            for i, cc in enumerate(chain):
                self.fat[cc] = chain[i + 1] if i + 1 < n else self.EOC
                off = (self.data_start + (cc - 2) * self.spc) * SECTOR
                piece = data[i * self.csize():(i + 1) * self.csize()]
                self.img[off:off + len(piece)] = piece
        if self.fat12:
            fat = bytearray((len(self.fat) * 3 + 1) // 2 + 2)
            for c, val in enumerate(self.fat):
                o = c * 3 // 2
                if c & 1:
                    fat[o] = (fat[o] & 0x0F) | ((val & 0x0F) << 4)
                    fat[o + 1] = val >> 4
                else:
                    fat[o] = val & 0xFF
                    fat[o + 1] = (fat[o + 1] & 0xF0) | (val >> 8)
            fat = bytes(fat)
        else:
            fat = struct.pack(("<%dH" if self.fat16 else "<%dI") % len(self.fat), *self.fat)
        for k in range(2):
            o = (self.reserved + k * self.fat_sectors) * SECTOR
            self.img[o:o + len(fat)] = fat
        return bytes(self.img)


def read_fat(path):
    d = open(path, "rb").read()
    bps, spc, rsv, nfats, root_ents = struct.unpack_from("<HBHBH", d, 11)
    fat_sz = struct.unpack_from("<H", d, 22)[0] or struct.unpack_from("<I", d, 36)[0]
    total = struct.unpack_from("<H", d, 19)[0] or struct.unpack_from("<I", d, 32)[0]
    root_sectors = (root_ents * 32 + bps - 1) // bps
    root_start = rsv + nfats * fat_sz
    data0 = root_start + root_sectors
    clusters = (total - data0) // spc
    fat32 = clusters >= 65525
    fat12 = clusters < 4085
    root = struct.unpack_from("<I", d, 44)[0] if fat32 else 0
    esz = 4 if fat32 else 2
    if fat12:
        raw = d[rsv * bps:(rsv + fat_sz) * bps]
        fat = []
        for c in range(fat_sz * bps * 2 // 3):
            o = c * 3 // 2
            pair = raw[o] | (raw[o + 1] << 8)
            fat.append((pair >> 4) if c & 1 else (pair & 0xFFF))
    else:
        fat = struct.unpack_from(("<%dI" if fat32 else "<%dH") % (fat_sz * bps // esz), d, rsv * bps)
    eoc = 0x0FFFFFF8 if fat32 else 0xFF8 if fat12 else 0xFFF8

    def chain(c):
        out = []
        while 2 <= c < eoc:
            out.append(c)
            c = fat[c] & (0x0FFFFFFF if fat32 else 0xFFF if fat12 else 0xFFFF)
        return out

    def cdata(c):
        return b"".join(d[(data0 + (x - 2) * spc) * bps:(data0 + (x - 2) * spc + spc) * bps] for x in chain(c))

    def walk(raw, prefix, out):
        lfn = []
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            if e[11] == 0x0F:
                chars = struct.unpack("<5H", e[1:11]) + struct.unpack("<6H", e[14:26]) + struct.unpack("<2H", e[28:32])
                lfn.insert(0, "".join(chr(ch) for ch in chars if ch not in (0, 0xFFFF)))
                continue
            if e[11] & 0x08:
                lfn = []
                continue
            name = "".join(lfn) or (e[0:8].decode().strip() + ("." + e[8:11].decode().strip() if e[8:11].strip() else ""))
            lfn = []
            if e[0:1] == b".":
                continue
            cl = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
            size = struct.unpack_from("<I", e, 28)[0]
            if e[11] & 0x10:
                out.append((prefix + name + "/", 0, b""))
                walk(cdata(cl), prefix + name + "/", out)
            else:
                out.append((prefix + name, size, cdata(cl)[:size] if cl else b""))

    out = []
    raw = cdata(root) if fat32 else d[root_start * bps:(root_start + root_sectors) * bps]
    walk(raw, "/", out)
    return out


if __name__ == "__main__":
    if sys.argv[1] == "--ls":
        for n, sz, data in read_fat(sys.argv[2]):
            print("%8d  %s" % (sz, n))
        sys.exit()
    fs = Fat(int(sys.argv[2]) if len(sys.argv) > 2 else 64, sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else "SILICONUSB",
             "--fat16" in sys.argv, "--fat12" in sys.argv)
    fs.add(2, "hello.txt", b"Hello from a FAT drive!\n")
    docs = fs.add(2, "Documents Folder")
    fs.add(docs, "A long file name here.txt", b"long names work\n" * 50)
    fs.add(2, "big.bin", bytes((i * 7) % 256 for i in range(300000 if "--fat12" not in sys.argv else 1000000)))
    open(sys.argv[1], "wb").write(fs.finish())
    print("wrote", sys.argv[1])
