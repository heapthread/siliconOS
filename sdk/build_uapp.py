#!/usr/bin/env python3
"""Build a silicon user program (.uapp): a ring 3 process with its own address space.

    python sdk/build_uapp.py main.cpp [more.cpp ...] -o myapp.uapp

Sources are compiled freestanding and linked at the fixed user address 0x40000000 (every process has
its own address space, so no relocations are needed). The file is a small header ("UAPP", version,
entry offset, image size) followed by the flat image. Needs MinGW-w64 g++, ld, objcopy and nm.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
UDIR = os.path.join(HERE, "user")
BASE = 0x40000000
FLAGS = [
    "-std=c++20", "-O2", "-ffreestanding", "-fno-exceptions", "-fno-rtti", "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables", "-fno-stack-protector", "-fno-threadsafe-statics", "-mno-red-zone", "-mno-stack-arg-probe",
    "-nostdlib", "-fno-builtin", "-fno-tree-loop-distribute-patterns", "-fno-use-cxa-atexit", "-mcmodel=small",
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-I" + UDIR,
]


def tool(name):
    p = shutil.which(name)
    if not p:
        sys.exit(f"build_uapp: cannot find '{name}' on PATH")
    return p


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(" ".join(cmd))
        print(r.stdout + r.stderr)
        sys.exit(1)
    if r.stderr.strip():
        print(r.stderr.strip())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sources", nargs="+")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()
    cxx = tool("g++")
    with tempfile.TemporaryDirectory() as tmp:
        objs = []
        for i, src in enumerate(args.sources + [os.path.join(UDIR, "crt0.cpp")]):
            o = os.path.join(tmp, f"o{i}.o")
            run([cxx, "-c", src, "-o", o] + FLAGS)
            objs.append(o)
        script = open(os.path.join(UDIR, "user.ld.in")).read().replace("@BASE@", hex(BASE))
        ld_path = os.path.join(tmp, "user.ld")
        open(ld_path, "w").write(script)
        exe = os.path.join(tmp, "app.exe")
        binf = os.path.join(tmp, "app.bin")
        run([tool("ld"), "-m", "i386pep", "-T", ld_path, "--image-base", "0", "--section-alignment", "0x1000",
             "--file-alignment", "0x1000", "-nostdlib", "--no-insert-timestamp", "-e", "_start", "-o", exe] + objs)
        run([tool("objcopy"), "-O", "binary", exe, binf])
        nm = subprocess.run([tool("nm"), exe], capture_output=True, text=True).stdout
        entry = None
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] in ("_start", "__start"):
                entry = int(parts[0], 16) - BASE
        if entry is None:
            sys.exit("build_uapp: no _start found")
        image = open(binf, "rb").read()
    out = b"UAPP" + struct.pack("<IIIIIII", 1, entry, len(image), 0, 0, 0, 0) + image
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    open(args.output, "wb").write(out)
    print(f"[uapp] {args.output}: {len(image)} bytes")


if __name__ == "__main__":
    main()
