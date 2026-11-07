"""Wrap a raw disk image in a fixed VHD footer so Windows can mount it (Mount-DiskImage) to check test images.

    python tools/rawtovhd.py in.img out.vhd
"""
import struct
import sys
import uuid

raw = open(sys.argv[1], "rb").read()
size = len(raw)
sectors = size // 512
spt, heads = 63, 16
cyl = min(sectors // (spt * heads), 65535)
f = bytearray(512)
f[0:8] = b"conectix"
struct.pack_into(">I", f, 8, 2)
struct.pack_into(">I", f, 12, 0x00010000)
struct.pack_into(">Q", f, 16, 0xFFFFFFFFFFFFFFFF)
struct.pack_into(">I", f, 24, 0)
f[28:32] = b"sili"
struct.pack_into(">I", f, 32, 0x00010000)
f[36:40] = b"Wi2k"
struct.pack_into(">QQ", f, 40, size, size)
struct.pack_into(">HBB", f, 56, cyl, heads, spt)
struct.pack_into(">I", f, 60, 2)
f[68:84] = uuid.uuid4().bytes
cs = (~sum(f)) & 0xFFFFFFFF
struct.pack_into(">I", f, 64, cs)
open(sys.argv[2], "wb").write(raw + bytes(f))
