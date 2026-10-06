#!/usr/bin/env python3
"""Build a silicon app module.

    python sdk/build_app.py main.cpp [more.cpp ...] -o myapp.sapp

The sources are compiled freestanding with the same flags as the kernel, linked twice at two
different base addresses, and the differences between the two images become the relocation table
the kernel applies when it loads the module. Needs MinGW-w64 g++, ld, objcopy and nm on PATH.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BASE_A, BASE_B = 0x10000000, 0x20000000
FLAGS = [
    "-std=c++20", "-O2", "-ffreestanding", "-fno-exceptions", "-fno-rtti", "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables", "-fno-stack-protector", "-fno-threadsafe-statics", "-mno-red-zone", "-mno-stack-arg-probe",
    "-nostdlib", "-fno-builtin", "-fno-tree-loop-distribute-patterns", "-fno-use-cxa-atexit", "-mcmodel=small",
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-I" + HERE,
]


def tool(name):
    p = shutil.which(name)
    if not p:
        sys.exit(f"build_app: cannot find '{name}' on PATH")
    return p


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(" ".join(cmd))
        print(r.stdout + r.stderr)
        sys.exit(1)
    if r.stderr.strip():
        print(r.stderr.strip())


def link(objs, base, tmp, tag):
    script = open(os.path.join(HERE, "app.ld.in")).read().replace("@BASE@", hex(base))
    ld_path = os.path.join(tmp, f"app_{tag}.ld")
    open(ld_path, "w").write(script)
    exe = os.path.join(tmp, f"app_{tag}.exe")
    binf = os.path.join(tmp, f"app_{tag}.bin")
    run([tool("ld"), "-m", "i386pep", "-T", ld_path, "--image-base", "0", "--section-alignment", "0x1000",
         "--file-alignment", "0x1000", "-nostdlib", "--no-insert-timestamp", "-e", "silicon_main", "-o", exe] + objs)
    run([tool("objcopy"), "-O", "binary", exe, binf])
    nm = subprocess.run([tool("nm"), exe], capture_output=True, text=True).stdout
    entry = None
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in ("silicon_main", "_silicon_main"):
            entry = int(parts[0], 16)
    if entry is None:
        sys.exit("build_app: the module has no silicon_main (link the SDK's crt.cpp)")
    return open(binf, "rb").read(), entry - base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sources", nargs="+")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()
    cxx = tool("g++")
    with tempfile.TemporaryDirectory() as tmp:
        objs = []
        for i, src in enumerate(args.sources + [os.path.join(HERE, "crt.cpp")]):
            o = os.path.join(tmp, f"o{i}.o")
            run([cxx, "-c", src, "-o", o] + FLAGS + (["-x", "c++"] if src.endswith(".c") else []))
            objs.append(o)
        a, entry_a = link(objs, BASE_A, tmp, "a")
        b, entry_b = link(objs, BASE_B, tmp, "b")
    if len(a) != len(b) or entry_a != entry_b:
        sys.exit("build_app: the two links differ in size; cannot make a relocation table")
    delta = BASE_B - BASE_A
    relocs = []
    for off in range(0, len(a) - 3, 4):
        x = struct.unpack_from("<I", a, off)[0]
        y = struct.unpack_from("<I", b, off)[0]
        if x != y:
            if (y - x) & 0xFFFFFFFF == delta:
                relocs.append(off)
            else:
                sys.exit(f"build_app: unexpected difference at offset {off:#x} (a pointer the loader can't fix up)")
    # 64-bit pointers are a low dword that changes and a high dword that stays 0, so dword relocations cover them
    out = bytearray(b"SAPP")
    out += struct.pack("<IIIIII", 1, len(a), entry_a, BASE_A, 0, len(relocs))
    for off in relocs:
        out += struct.pack("<I", off)
    out += a
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    open(args.output, "wb").write(out)
    print(f"[sapp] {args.output}: {len(a)} bytes, {len(relocs)} relocations")


if __name__ == "__main__":
    main()
