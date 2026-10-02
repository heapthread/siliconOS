"""Convert silicon's PNG art into .sil blobs and an assembly table the kernel links in.

.sil = "SIL1" u16 w u16 h u16 ncolors u16 0, palette (u32 ARGB LE), RLE of palette indices:
    byte t: t & 0x80 -> repeat next index (t & 0x7F) + 1 times
            else     -> (t + 1) literal index bytes follow
Usage: python tools/assets.py <gen_dir>
"""
import os
import warnings
warnings.filterwarnings("ignore", category=DeprecationWarning)
import struct
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, "art")

# (asset name, source png relative to art/, target size or None)
ICONS = [
    ("terminal", "icons/terminal.png"), ("files", "icons/file_manager.png"),
    ("settings", "icons/settings.png"), ("monitor", "icons/process_tracker.png"),
    ("trash", "icons/trash.png"), ("wallpapers", "icons/wallpaper_switcher.png"),
    ("slate", "icons/slate.png"), ("games", "icons/game_library.png"),
    ("minesweeper", "icons/games/minesweeper.png"), ("snake", "icons/games/snake.png"),
    ("reversi", "icons/games/reversi.png"), ("restart", "icons/power_controls/restart.png"),
    ("shutdown", "icons/power_controls/shutdown.png"), ("folder", "icons/empty_folder.png"),
    ("folder_full", "icons/full_folder.png"), ("file", "icons/file.png"),
    ("disk", "icons/filesystem.png"),
    ("silicon", "silicon/icons/silicon.png"), ("calculator", "silicon/icons/calculator.png"),
    ("pixel", "silicon/icons/pixel.png"), ("clock", "silicon/icons/clock.png"),
    ("about", "silicon/icons/about.png"), ("launcher", "silicon/icons/launcher.png"),
    ("chart", "silicon/icons/monitor.png"), ("image_file", "silicon/icons/image_file.png"),
    ("text_file", "silicon/icons/text_file.png"), ("browser", "silicon/icons/browser.png"),
    ("stacker", "silicon/icons/stacker.png"), ("g2048", "silicon/icons/g2048.png"),
    ("breakout", "silicon/icons/breakout.png"), ("flappy", "silicon/icons/flappy.png"),
    ("photos", "silicon/icons/photos.png"), ("piano", "silicon/icons/piano.png"),
]
WALLPAPERS = [
    ("wafer", "silicon/wallpapers/wafer.png"),
    ("circuit dusk", "silicon/wallpapers/circuit_dusk.png"),
    ("silicon valley", "silicon/wallpapers/silicon_valley.png"),
    ("sunset mountains", "wallpapers/sunset_mountains.png"),
    ("night hills", "wallpapers/night_hills.png"),
    ("starry night", "wallpapers/starry_night.png"),
    ("distant city", "wallpapers/distant_city.png"),
    ("foggy valley", "wallpapers/foggy_valley.png"),
]


def fit_icon(im):
    im = im.convert("RGBA")
    if im.size != (64, 64):
        if max(im.size) > 64:
            im = im.resize((64, 64), Image.LANCZOS)
        canvas = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
        canvas.alpha_composite(im, ((64 - im.width) // 2, (64 - im.height) // 2))
        im = canvas
    return im


def encode(im):
    im = im.convert("RGBA")
    w, h = im.size
    colors = im.getcolors(1 << 24)
    if colors is not None and len(colors) <= 256:
        pal = [c for _, c in colors]
        index = {c: i for i, c in enumerate(pal)}
        idx = [index[p] for p in im.getdata()]
    else:
        alpha = im.getchannel("A")
        q = im.convert("RGB").quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE) \
            if alpha.getextrema() == (255, 255) else im.quantize(256, method=Image.Quantize.FASTOCTREE,
                                                                    dither=Image.Dither.NONE)
        raw = q.getpalette()
        rgba = list(im.getdata())
        idx = list(q.getdata())
        n = max(idx) + 1
        pal = [(raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2], 255) for i in range(n)]
        if alpha.getextrema() != (255, 255):
            # keep each palette slot's average alpha
            acc = [[0, 0] for _ in range(n)]
            for i, p in zip(idx, rgba):
                acc[i][0] += p[3]
                acc[i][1] += 1
            pal = [(r, g, b, (acc[i][0] // acc[i][1]) if acc[i][1] else 255) for i, (r, g, b, _) in enumerate(pal)]
    out = bytearray(b"SIL1")
    out += struct.pack("<HHHH", w, h, len(pal), 0)
    for r, g, b, a in pal:
        if a == 0:
            r = g = b = 0
        out += struct.pack("<I", (a << 24) | (r << 16) | (g << 8) | b)
    i, n = 0, len(idx)
    while i < n:
        run = 1
        while i + run < n and idx[i + run] == idx[i] and run < 128:
            run += 1
        if run >= 3:
            out += bytes([0x80 | (run - 1), idx[i]])
            i += run
            continue
        start = i
        while i < n and i - start < 128:
            if i + 2 < n and idx[i] == idx[i + 1] == idx[i + 2]:
                break
            i += 1
        out += bytes([i - start - 1]) + bytes(idx[start:i])
    return bytes(out)


def main():
    gen = sys.argv[1]
    sil_dir = os.path.join(gen, "sil")
    os.makedirs(sil_dir, exist_ok=True)
    entries = []
    for name, rel in ICONS:
        entries.append(("icon/" + name, rel, True))
    for name, rel in WALLPAPERS:
        entries.append(("wall/" + name, rel, False))
    asm_lines = ["; generated by tools/assets.py", "section .rdata", "global asset_table", "global asset_count",
                 "align 8", "asset_count: dq %d" % len(entries), "asset_table:"]
    data_lines = []
    me = os.path.getmtime(os.path.abspath(__file__))
    for i, (name, rel, is_icon) in enumerate(entries):
        src = os.path.join(ART, rel)
        fname = "a%02d.sil" % i
        dst = os.path.join(sil_dir, fname)
        if not os.path.exists(dst) or os.path.getmtime(dst) < max(os.path.getmtime(src), me):
            im = Image.open(src)
            im = fit_icon(im) if is_icon else im.convert("RGB").resize((1024, 768), Image.NEAREST)
            blob = encode(im)
            open(dst, "wb").write(blob)
            print(f"[sil] {name}: {len(blob) // 1024} KiB")
        size = os.path.getsize(dst)
        asm_lines.append(f"    dq name{i}, data{i}, {size}")
        data_lines += [f"name{i}: db \"{name}\", 0", "align 8", f"data{i}: incbin \"sil/{fname}\""]
    text = "\n".join(asm_lines + data_lines) + "\n"
    asm = os.path.join(gen, "assets.asm")
    if not os.path.exists(asm) or open(asm).read() != text:
        open(asm, "w").write(text)


if __name__ == "__main__":
    main()
