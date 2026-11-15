#!/usr/bin/env python3
"""Build win32run, the Win32 runtime, as a silicon user program (.uapp).

    python sdk/win32/build.py -o build/gen/uapp/win32run.uapp

Like sdk/build_uapp.py, but the runtime keeps its own unwind tables (.pdata/.xdata) so that Windows
exceptions can be unwound through it, and it is built from every .cpp in this folder.
"""
import argparse
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
UDIR = os.path.join(os.path.dirname(HERE), "user")
BASE = 0x40000000
FLAGS = [
    "-std=c++20", "-O2", "-ffreestanding", "-fno-exceptions", "-fno-rtti", "-fno-stack-protector", "-fno-threadsafe-statics",
    "-mno-red-zone", "-mno-stack-arg-probe", "-nostdlib", "-fno-builtin", "-fno-tree-loop-distribute-patterns",
    "-fno-use-cxa-atexit", "-fno-strict-aliasing", "-mcmodel=small", "-Wall", "-Wextra", "-Wno-unused-parameter",
    "-Wno-unused-function", "-Wno-missing-field-initializers", "-Wno-class-memaccess", "-I" + UDIR, "-I" + HERE,
]

LD_SCRIPT = """
ENTRY(_start)
SECTIONS
{
    . = 0x%x;
    .text : { *(.text) *(SORT(.text$*)) *(.text.*) }
    .rdata : {
        *(.rdata) *(SORT(.rdata$*)) *(.rodata) *(.rodata.*)
        . = ALIGN(4);
        __rt_pdata_start = .;
        *(.pdata) *(SORT(.pdata$*))
        __rt_pdata_end = .;
        *(.xdata) *(SORT(.xdata$*))
    }
    .data : {
        __ctors_start = .;
        *(.ctors) *(SORT(.ctors.*))
        __ctors_end = .;
        *(.data) *(SORT(.data$*)) *(.bss) *(SORT(.bss$*)) *(COMMON)
        . = ALIGN(16);
    }
    /DISCARD/ : { *(.edata) *(.idata*) *(.reloc) *(.debug*) *(.comment) *(.drectve) *(.note*) *(.eh_frame) }
}
""" % BASE


def tool(name):
    p = shutil.which(name)
    if not p:
        sys.exit(f"win32run build: cannot find '{name}' on PATH")
    return p


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if r.returncode != 0:
        print(" ".join(cmd))
        print(out)
        return False
    if out:
        print(out)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()
    cxx = tool("g++")
    srcs = sorted(glob.glob(os.path.join(HERE, "*.cpp"))) + [os.path.join(UDIR, "crt0.cpp")]
    with tempfile.TemporaryDirectory() as tmp:
        objs = [os.path.join(tmp, f"o{i}.o") for i in range(len(srcs))]
        with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            ok = list(ex.map(lambda so: run([cxx, "-c", so[0], "-o", so[1]] + FLAGS), zip(srcs, objs)))
        if not all(ok):
            sys.exit("win32run build: compile failed")
        ld_path = os.path.join(tmp, "rt.ld")
        open(ld_path, "w").write(LD_SCRIPT)
        exe = os.path.join(tmp, "rt.exe")
        binf = os.path.join(tmp, "rt.bin")
        if not run([tool("ld"), "-m", "i386pep", "-T", ld_path, "--image-base", "0", "--section-alignment", "0x1000",
                    "--file-alignment", "0x1000", "-nostdlib", "--no-insert-timestamp", "-e", "_start", "-o", exe] + objs):
            sys.exit("win32run build: link failed")
        if not run([tool("objcopy"), "-O", "binary", exe, binf]):
            sys.exit(1)
        nm = subprocess.run([tool("nm"), exe], capture_output=True, text=True).stdout
        entry = None
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] in ("_start", "__start"):
                entry = int(parts[0], 16) - BASE
        if entry is None:
            sys.exit("win32run build: no _start")
        image = open(binf, "rb").read()
    out = b"UAPP" + struct.pack("<IIIIIII", 1, entry, len(image), 0, 0, 0, 0) + image
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    open(args.output, "wb").write(out)
    print(f"[win32run] {args.output}: {len(image) // 1024} KiB")


if __name__ == "__main__":
    main()
