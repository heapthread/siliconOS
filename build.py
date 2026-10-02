#!/usr/bin/env python3
"""silicon build script.

    python build.py            build build/silicon.img
    python build.py run        build and boot in QEMU
    python build.py clean      remove build outputs (keeps nothing)
    options: --fresh-disk      wipe the SiliconFS region (files you created)
             --headless        (run) no window, QEMU monitor on tcp:127.0.0.1:4444
"""
import os
import subprocess
import sys
import shutil
import struct
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src")
BUILD = os.path.join(ROOT, "build")
OBJ = os.path.join(BUILD, "obj")
GEN = os.path.join(BUILD, "gen")
IMG = os.path.join(BUILD, "silicon.img")

DISK_SECTORS = 64 * 1024 * 1024 // 512      # 64 MiB disk
STAGE2_SECTORS = 63
KERNEL_LBA = 64
FS_LBA = 32768                               # SiliconFS starts at 16 MiB

CXXFLAGS = [
    "-std=c++20", "-O2", "-ffreestanding", "-fno-exceptions", "-fno-rtti",
    "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-fno-stack-protector",
    "-fno-threadsafe-statics", "-mno-red-zone", "-mno-stack-arg-probe", "-nostdlib",
    "-fno-builtin", "-fno-tree-loop-distribute-patterns", "-fno-use-cxa-atexit",
    "-mcmodel=small", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
    "-Wno-class-memaccess", "-Wno-volatile", "-I" + os.path.join(SRC, "kernel"), "-I" + GEN,
]


def find_tool(name, extra_dirs=()):
    p = shutil.which(name)
    if p:
        return p
    for d in extra_dirs:
        cand = os.path.join(d, name + ".exe")
        if os.path.exists(cand):
            return cand
    sys.exit(f"build: cannot find '{name}' on PATH")


NASM = find_tool("nasm", [os.path.expandvars(r"%LOCALAPPDATA%\bin\NASM"), r"C:\Program Files\NASM"])
CXX = find_tool("g++")
LD = find_tool("ld")
OBJCOPY = find_tool("objcopy")
QEMU = find_tool("qemu-system-x86_64", [r"C:\Program Files\qemu"])


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        print(" ".join(cmd))
        print(r.stdout + r.stderr)
        sys.exit(1)
    if r.stderr.strip():
        print(r.stderr.strip())
    return r


def newer(target, deps):
    if not os.path.exists(target):
        return True
    t = os.path.getmtime(target)
    return any(os.path.getmtime(d) > t for d in deps if os.path.exists(d))


def walk(root, exts):
    out = []
    for d, _, files in os.walk(root):
        for f in files:
            if os.path.splitext(f)[1] in exts:
                out.append(os.path.join(d, f))
    return sorted(out)


def obj_name(path):
    rel = os.path.relpath(path, SRC).replace(os.sep, "_").replace("/", "_")
    return os.path.join(OBJ, os.path.splitext(rel)[0] + ".o")


def step_generated():
    font_src = os.path.join(ROOT, "tools", "font.txt")
    font_out = os.path.join(SRC, "kernel", "gfx", "font_data.inc")
    if newer(font_out, [font_src, os.path.join(ROOT, "tools", "make_font.py")]):
        print("[gen] font")
        run([sys.executable, os.path.join(ROOT, "tools", "make_font.py")])
    art = os.path.join(ROOT, "tools", "make_art.py")
    if os.path.exists(art):
        run([sys.executable, art])
    assets = os.path.join(ROOT, "tools", "assets.py")
    if os.path.exists(assets):
        run([sys.executable, assets, GEN])


def step_boot():
    outs = {}
    for name in ("stage1", "stage2"):
        src = os.path.join(SRC, "boot", name + ".asm")
        out = os.path.join(BUILD, name + ".bin")
        if newer(out, [src]):
            print(f"[asm] {name}")
            run([NASM, "-f", "bin", src, "-o", out])
        outs[name] = out
    if os.path.getsize(outs["stage2"]) > STAGE2_SECTORS * 512:
        sys.exit("build: stage2 too large")
    return outs


def step_kernel():
    headers = walk(os.path.join(SRC, "kernel"), {".h", ".inc"}) + walk(GEN, {".h", ".inc"})
    newest_header = max([os.path.getmtime(h) for h in headers] + [0])
    objs = []
    jobs = []
    asm_srcs = walk(os.path.join(SRC, "kernel"), {".asm"}) + walk(GEN, {".asm"})
    for s in asm_srcs:
        o = obj_name(s) if s.startswith(SRC) else os.path.join(OBJ, "gen_" + os.path.basename(s)[:-4] + ".o")
        objs.append(o)
        deps = [s] + walk(GEN, {".sil"})
        if newer(o, deps):
            jobs.append((s, o, [NASM, "-f", "win64", "-I" + GEN + os.sep, s, "-o", o]))
    for s in walk(os.path.join(SRC, "kernel"), {".cpp"}):
        o = obj_name(s)
        objs.append(o)
        if not os.path.exists(o) or os.path.getmtime(s) > os.path.getmtime(o) or newest_header > os.path.getmtime(o):
            jobs.append((s, o, [CXX, "-c", s, "-o", o] + CXXFLAGS))

    def compile_one(job):
        s, o, cmd = job
        r = subprocess.run(cmd, capture_output=True, text=True)
        return s, r

    failed = False
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for s, r in ex.map(compile_one, jobs):
            print(f"[cc] {os.path.relpath(s, ROOT)}")
            out = (r.stdout + r.stderr).strip()
            if out:
                print(out)
            if r.returncode != 0:
                failed = True
    if failed:
        sys.exit("build: compile failed")

    exe = os.path.join(BUILD, "kernel.pe")
    kbin = os.path.join(BUILD, "kernel.bin")
    ld_script = os.path.join(SRC, "kernel", "linker.ld")
    if jobs or newer(exe, objs + [ld_script]):
        print("[ld] kernel")
        run([LD, "-m", "i386pep", "-T", ld_script, "--image-base", "0", "--section-alignment", "0x1000",
             "--file-alignment", "0x1000", "-nostdlib", "--no-insert-timestamp", "-e", "kentry",
             "-Map", os.path.join(BUILD, "kernel.map"), "-o", exe] + objs)
        run([OBJCOPY, "-O", "binary", "-R", ".bss", exe, kbin])
    return kbin


def step_image(boot, kbin, fresh):
    stage1 = bytearray(open(boot["stage1"], "rb").read())
    stage2 = open(boot["stage2"], "rb").read()
    kernel = open(kbin, "rb").read()
    ksect = (len(kernel) + 511) // 512
    if KERNEL_LBA + ksect >= FS_LBA:
        sys.exit(f"build: kernel too large ({len(kernel)} bytes)")
    struct.pack_into("<I", stage1, 0x1B0, ksect)

    if fresh or not os.path.exists(IMG) or os.path.getsize(IMG) != DISK_SECTORS * 512:
        print("[img] creating fresh disk")
        with open(IMG, "wb") as f:
            f.truncate(DISK_SECTORS * 512)
    with open(IMG, "r+b") as f:
        f.seek(0)
        f.write(stage1)
        f.seek(512)
        f.write(stage2.ljust(STAGE2_SECTORS * 512, b"\0"))
        f.seek(KERNEL_LBA * 512)
        f.write(kernel.ljust(ksect * 512, b"\0"))
    print(f"[img] {os.path.relpath(IMG, ROOT)}: kernel {len(kernel) // 1024} KiB ({ksect} sectors)")


def build(fresh=False):
    for d in (BUILD, OBJ, GEN):
        os.makedirs(d, exist_ok=True)
    step_generated()
    boot = step_boot()
    kbin = step_kernel()
    step_image(boot, kbin, fresh)


def audio_driver():
    """PC-speaker beeps need an audio backend; fall back to none if dsound is missing."""
    try:
        out = subprocess.run([QEMU, "-audiodev", "help"], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return "none"
    for drv in ("dsound", "sdl", "pa", "alsa", "coreaudio"):
        if drv in out.split():
            return drv
    return "none"


def qemu_cmd(headless=False):
    audio = "none" if headless else audio_driver()
    # hardware virtualization when available (WHPX on Windows, KVM/HVF elsewhere), else TCG
    accel = os.environ.get("SILICON_ACCEL", "whpx:tcg" if os.name == "nt" else "kvm:hvf:tcg")
    cmd = [QEMU, "-name", "silicon", "-m", "512M"]
    for a in accel.split(":"):
        cmd += ["-accel", a]
    cmd += [
           "-drive", f"file={IMG},format=raw,if=ide,index=0",
           "-vga", "std", "-rtc", "base=localtime",
           "-netdev", "user,id=net0", "-device", "rtl8139,netdev=net0",
           "-serial", "file:" + os.path.join(BUILD, "serial.log"),
           "-audiodev", f"{audio},id=snd0",
           "-machine", "pc,pcspk-audiodev=snd0"]
    if not headless and os.name == "nt" and "whpx" in accel:
        # QEMU's GTK window starves a WHPX guest (each port access can stall ~100 ms); SDL doesn't
        cmd += ["-display", "sdl"]
    if headless:
        # the test harness injects relative PS/2 motion, so turn the absolute vmmouse off
        cmd[cmd.index("pc,pcspk-audiodev=snd0")] = "pc,pcspk-audiodev=snd0,vmport=off"
        cmd += ["-display", "none", "-monitor", "tcp:127.0.0.1:4444,server,nowait"]
    return cmd


def main():
    args = sys.argv[1:]
    fresh = "--fresh-disk" in args
    if "clean" in args:
        shutil.rmtree(BUILD, ignore_errors=True)
        return
    build(fresh)
    if "run" in args:
        cmd = qemu_cmd("--headless" in args)
        print(" ".join(cmd))
        if "--headless" in args:
            subprocess.Popen(cmd)
        else:
            subprocess.run(cmd)


if __name__ == "__main__":
    main()
