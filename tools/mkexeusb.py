#!/usr/bin/env python3
"""Put Windows programs (or any files) on a FAT32 USB stick image for silicon.

    python tools/mkexeusb.py out.img program.exe [more files or folders ...] [--size MB] [--label NAME]

Plug it in when booting with SILICON_USB_IMG=out.img (python build.py run), or hot-plug it from the QEMU
monitor. silicon mounts it at /mnt/<label> (WIN32 by default), and the programs run from there:

    win32run /mnt/WIN32/program.exe
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfat import Fat


def add_path(fs, dir_cluster, path):
    name = os.path.basename(os.path.normpath(path))
    if os.path.isdir(path):
        sub = fs.add(dir_cluster, name)
        for entry in sorted(os.listdir(path)):
            add_path(fs, sub, os.path.join(path, entry))
    else:
        fs.add(dir_cluster, name, open(path, "rb").read())


def main():
    args = sys.argv[1:]
    size, label, files = 64, "WIN32", []
    i = 0
    while i < len(args):
        if args[i] == "--size":
            size = int(args[i + 1]); i += 2
        elif args[i] == "--label":
            label = args[i + 1]; i += 2
        else:
            files.append(args[i]); i += 1
    if len(files) < 2:
        sys.exit(__doc__)
    out, inputs = files[0], files[1:]
    fs = Fat(size, label)
    for p in inputs:
        add_path(fs, 2, p)
    open(out, "wb").write(fs.finish())
    print(f"wrote {out}: {len(inputs)} item(s), label {label}")


if __name__ == "__main__":
    main()
