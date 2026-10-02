"""Generate silicon's own pixel art: icons (32x32 drawn, scaled to 64x64) and
wallpapers (256x192 drawn, scaled to 1024x768). Output: art/silicon/.
Deterministic, so rebuilding gives identical files.
"""
import math
import os
import random

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "art", "silicon")

INK = (26, 20, 40, 255)
CHIP = (52, 46, 72, 255)
CHIP_HI = (84, 76, 112, 255)
GOLD = (236, 188, 92, 255)
GOLD_DK = (170, 120, 48, 255)
VIOLET = (150, 120, 255, 255)
VIOLET_LT = (204, 188, 255, 255)
VIOLET_DK = (98, 72, 196, 255)
PINK = (255, 118, 150, 255)
TEAL = (79, 209, 197, 255)
WHITE = (246, 242, 255, 255)


def new(w=32, h=32):
    return Image.new("RGBA", (w, h), (0, 0, 0, 0))


def outline(img, color=INK):
    """1px outline around every opaque pixel (4-neighbourhood)."""
    w, h = img.size
    src = img.load()
    out = img.copy()
    o = out.load()
    for y in range(h):
        for x in range(w):
            if src[x, y][3]:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < w and 0 <= yy < h and src[xx, yy][3] > 128:
                    o[x, y] = color
                    break
    return out


def up(img, f=2):
    return img.resize((img.width * f, img.height * f), Image.NEAREST)


def rect(d, x0, y0, x1, y1, c):
    d.rectangle([x0, y0, x1, y1], fill=c)


def save_icon(img, name):
    img = up(outline(img))
    img.save(os.path.join(OUT, "icons", name + ".png"))


# ---------------------------------------------------------------- icons
def icon_silicon():
    im = new()
    d = ImageDraw.Draw(im)
    for p in (9, 13, 17, 21):
        rect(d, p, 3, p + 1, 6, GOLD)
        rect(d, p, 25, p + 1, 28, GOLD)
        rect(d, 3, p, 6, p + 1, GOLD)
        rect(d, 25, p, 28, p + 1, GOLD)
        d.point([(p, 3), (p, 28), (3, p), (28, p)], fill=GOLD_DK)
    rect(d, 7, 7, 24, 24, CHIP)
    rect(d, 7, 7, 24, 7, CHIP_HI)
    rect(d, 7, 7, 7, 24, CHIP_HI)
    rect(d, 11, 11, 20, 20, VIOLET_DK)
    rect(d, 12, 12, 19, 19, VIOLET)
    rect(d, 14, 14, 17, 17, VIOLET_LT)
    d.point([(12, 12), (13, 12), (12, 13)], fill=WHITE)
    rect(d, 9, 9, 9, 9, PINK)
    return im


def icon_calculator():
    im = new()
    d = ImageDraw.Draw(im)
    body, shadow = (206, 200, 222, 255), (150, 142, 176, 255)
    rect(d, 6, 2, 25, 29, shadow)
    rect(d, 6, 2, 24, 28, body)
    rect(d, 8, 4, 22, 10, (34, 70, 68, 255))
    rect(d, 9, 5, 21, 9, (60, 120, 112, 255))
    # "42" in tiny pixels
    digits = ["#.#..##", "#.#...#", "###..#.", "..#.#..", "..#.###"]
    for y, row in enumerate(digits):
        for x, ch in enumerate(row):
            if ch == "#":
                d.point((13 + x, 5 + y), fill=(170, 255, 230, 255))
    colors = [(110, 102, 142, 255)] * 3 + [(255, 168, 80, 255)]
    for r in range(4):
        for c in range(4):
            x, y = 8 + c * 4, 13 + r * 4
            col = colors[c]
            if r == 3 and c == 3:
                col = PINK
            rect(d, x, y, x + 2, y + 2, col)
            d.point((x, y), fill=tuple(min(255, v + 50) for v in col[:3]) + (255,))
    return im


def icon_pixel():
    im = new()
    d = ImageDraw.Draw(im)
    rect(d, 3, 4, 25, 26, (120, 82, 50, 255))
    rect(d, 5, 6, 23, 24, (246, 238, 222, 255))
    heart = [".##.##.", "#######", "#######", ".#####.", "..###..", "...#..."]
    for y, row in enumerate(heart):
        for x, ch in enumerate(row):
            if ch == "#":
                rect(d, 7 + x * 2, 9 + y * 2, 8 + x * 2, 10 + y * 2, PINK if (x + y) % 5 else (255, 170, 190, 255))
    # pencil
    for i in range(9):
        x, y = 28 - i, 9 + i
        rect(d, x - 1, y - 1, x + 1, y + 1, (255, 206, 84, 255))
    d.point([(28, 8), (29, 9)], fill=(255, 140, 160, 255))
    rect(d, 18, 18, 19, 19, (230, 200, 160, 255))
    d.point((17, 20), fill=INK)
    return im


def icon_clock():
    im = new()
    d = ImageDraw.Draw(im)
    d.ellipse([2, 2, 29, 29], fill=(70, 58, 106, 255))
    d.ellipse([4, 4, 27, 27], fill=(240, 236, 250, 255))
    d.ellipse([4, 4, 27, 27], outline=(200, 194, 220, 255))
    for a in range(12):
        ang = a * math.pi / 6
        x, y = 16 + math.cos(ang) * 10, 16 + math.sin(ang) * 10
        d.point((round(x) - (1 if x > 16 else 0), round(y) - (1 if y > 16 else 0)), fill=(110, 100, 140, 255))
    d.line([16, 16, 16, 8], fill=INK, width=2)
    d.line([16, 16, 22, 19], fill=INK, width=2)
    d.line([15, 16, 9, 22], fill=PINK, width=1)
    rect(d, 15, 15, 16, 16, PINK)
    return im


def icon_about():
    im = new()
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([3, 3, 28, 28], radius=5, fill=VIOLET_DK)
    d.rounded_rectangle([3, 3, 28, 26], radius=5, fill=VIOLET)
    rect(d, 6, 5, 25, 5, VIOLET_LT)
    rect(d, 14, 7, 17, 10, WHITE)
    rect(d, 12, 13, 17, 14, WHITE)
    rect(d, 14, 13, 17, 22, WHITE)
    rect(d, 12, 22, 19, 23, WHITE)
    return im


def icon_launcher():
    im = new()
    d = ImageDraw.Draw(im)
    for i in range(8):
        rect(d, 19 + i, 19 + i, 21 + i, 21 + i, (90, 70, 140, 255))
    d.ellipse([3, 3, 22, 22], fill=(70, 58, 106, 255))
    d.ellipse([5, 5, 20, 20], fill=(150, 210, 255, 255))
    d.ellipse([5, 5, 20, 20], outline=(200, 236, 255, 255))
    # sparkle
    rect(d, 12, 8, 12, 16, WHITE)
    rect(d, 8, 12, 16, 12, WHITE)
    rect(d, 11, 11, 13, 13, WHITE)
    d.point([(7, 8), (8, 7)], fill=WHITE)
    return im


def icon_monitor_chart():
    im = new()
    d = ImageDraw.Draw(im)
    rect(d, 2, 4, 29, 24, (60, 54, 88, 255))
    rect(d, 4, 6, 27, 22, (20, 18, 34, 255))
    pts = [(5, 18), (9, 15), (12, 17), (16, 10), (19, 13), (23, 8), (26, 11)]
    for a, b in zip(pts, pts[1:]):
        d.line([a, b], fill=TEAL)
    rect(d, 12, 25, 19, 26, (60, 54, 88, 255))
    rect(d, 9, 27, 22, 28, (90, 82, 124, 255))
    return im


def icon_image_file():
    im = new()
    d = ImageDraw.Draw(im)
    rect(d, 4, 5, 27, 26, (236, 230, 246, 255))
    rect(d, 6, 7, 25, 21, (120, 170, 255, 255))
    rect(d, 6, 15, 25, 21, (90, 190, 120, 255))
    d.polygon([(6, 21), (13, 12), (19, 21)], fill=(70, 140, 90, 255))
    d.polygon([(14, 21), (20, 14), (25, 21)], fill=(60, 120, 80, 255))
    rect(d, 20, 9, 22, 11, (255, 230, 120, 255))
    rect(d, 6, 23, 18, 24, (180, 170, 200, 255))
    return im


def icon_text_file():
    im = new()
    d = ImageDraw.Draw(im)
    d.polygon([(6, 3), (20, 3), (26, 9), (26, 28), (6, 28)], fill=(244, 240, 252, 255))
    d.polygon([(20, 3), (26, 9), (20, 9)], fill=(196, 186, 226, 255))
    for i, y in enumerate((12, 15, 18, 21, 24)):
        rect(d, 9, y, 23 - (i % 3) * 3, y, (140, 126, 190, 255))
    return im


def icon_browser():
    im = new()
    d = ImageDraw.Draw(im)
    # light beam behind the globe
    d.polygon([(2, 30), (12, 12), (20, 12), (30, 30)], fill=(150, 120, 255, 120))
    d.ellipse([5, 3, 26, 24], fill=(40, 120, 200, 255))
    # continents
    for (x0, y0, x1, y1) in ((9, 7, 14, 12), (16, 10, 22, 15), (11, 15, 15, 20), (19, 17, 22, 20)):
        d.rectangle([x0, y0, x1, y1], fill=(90, 200, 120, 255))
    d.ellipse([5, 3, 26, 24], outline=(170, 220, 255, 255))
    d.arc([1, 9, 30, 18], 160, 380, fill=(204, 188, 255, 255), width=2)
    d.point([(9, 6), (10, 5)], fill=WHITE)
    return im


def icon_stacker():
    im = new()
    d = ImageDraw.Draw(im)
    cols = {"T": (170, 110, 255, 255), "L": (255, 170, 70, 255), "S": (90, 210, 120, 255),
            "I": (80, 200, 230, 255), "Z": (255, 100, 120, 255), "O": (250, 220, 90, 255)}
    grid = ["....TTT.",
            ".....T..",
            "........",
            "I......O",
            "I.LL..OO",
            "I.L.ZZOO",
            "ISL..ZZ.",
            "SSLLLZZ.",
            "S.LLLOO.",
            ]
    rect(d, 3, 1, 28, 30, (30, 26, 46, 255))
    for gy, row in enumerate(grid):
        for gx, ch in enumerate(row):
            if ch == ".":
                continue
            c = cols.get(ch, cols["T"])
            x, y = 4 + gx * 3, 3 + gy * 3
            rect(d, x, y, x + 2, y + 2, c)
            d.point([(x, y)], fill=WHITE)
    return im


def icon_2048():
    im = new()
    d = ImageDraw.Draw(im)
    rect(d, 2, 2, 29, 29, (120, 104, 150, 255))
    tiles = [((236, 228, 250, 255), 0, 0), ((206, 190, 255, 255), 1, 0), ((170, 140, 255, 255), 0, 1),
             ((255, 180, 90, 255), 1, 1)]
    for c, tx, ty in tiles:
        x, y = 4 + tx * 13, 4 + ty * 13
        rect(d, x, y, x + 11, y + 11, c)
    # a tiny "2" on the first tile, "8" on the last
    for (x, y) in ((6, 6), (7, 6), (8, 6), (8, 7), (7, 8), (6, 9), (6, 10), (7, 10), (8, 10)):
        d.point([(x + 1, y)], fill=INK)
    for (x, y) in ((19, 19), (20, 19), (21, 19), (19, 20), (21, 20), (20, 21), (19, 22), (21, 22), (19, 23), (20, 23), (21, 23)):
        d.point([(x, y)], fill=INK)
    return im


def icon_breakout():
    im = new()
    d = ImageDraw.Draw(im)
    rows = [(255, 100, 120, 255), (255, 170, 70, 255), (250, 220, 90, 255), (90, 210, 120, 255)]
    for r, c in enumerate(rows):
        for b in range(4):
            if (r, b) in ((2, 1), (3, 2), (3, 3)):
                continue
            x, y = 2 + b * 7, 3 + r * 4
            rect(d, x, y, x + 5, y + 2, c)
    rect(d, 15, 20, 17, 22, WHITE)
    rect(d, 9, 26, 22, 28, VIOLET)
    rect(d, 9, 26, 22, 26, VIOLET_LT)
    return im


def icon_flappy():
    im = new()
    d = ImageDraw.Draw(im)
    rect(d, 22, 0, 29, 9, (90, 210, 120, 255))
    rect(d, 21, 8, 30, 10, (60, 170, 90, 255))
    rect(d, 22, 21, 29, 31, (90, 210, 120, 255))
    rect(d, 21, 20, 30, 22, (60, 170, 90, 255))
    # the chip, with gold pins as wings
    for p in (9, 12, 15):
        rect(d, 2, p, 4, p, GOLD)
        rect(d, 17, p, 19, p, GOLD)
    rect(d, 5, 8, 16, 18, CHIP)
    rect(d, 5, 8, 16, 8, CHIP_HI)
    rect(d, 8, 11, 13, 15, VIOLET)
    rect(d, 12, 10, 14, 12, WHITE)
    d.point([(13, 11)], fill=INK)
    rect(d, 15, 14, 18, 15, PINK)
    return im


def icon_photos():
    im = new()
    d = ImageDraw.Draw(im)
    rect(d, 3, 4, 28, 27, WHITE)
    rect(d, 5, 6, 26, 21, (110, 170, 240, 255))
    d.polygon([(5, 21), (12, 12), (17, 17), (21, 13), (26, 21)], fill=(70, 150, 90, 255))
    d.ellipse([19, 7, 23, 11], fill=(250, 220, 90, 255))
    rect(d, 5, 23, 15, 24, (190, 180, 210, 255))
    return im


def icon_piano():
    im = new()
    d = ImageDraw.Draw(im)
    rect(d, 2, 6, 29, 26, (40, 32, 60, 255))
    rect(d, 2, 6, 29, 9, (90, 76, 130, 255))
    for i in range(7):
        x = 3 + i * 4
        rect(d, x, 11, x + 2, 25, WHITE)
    for i in (0, 1, 3, 4, 5):
        x = 5 + i * 4
        rect(d, x, 11, x + 1, 19, INK)
    # a note floating up
    rect(d, 22, 1, 22, 5, TEAL)
    rect(d, 20, 4, 21, 5, TEAL)
    return im


def build_icons():
    os.makedirs(os.path.join(OUT, "icons"), exist_ok=True)
    save_icon(icon_piano(), "piano")
    save_icon(icon_stacker(), "stacker")
    save_icon(icon_2048(), "g2048")
    save_icon(icon_breakout(), "breakout")
    save_icon(icon_flappy(), "flappy")
    save_icon(icon_photos(), "photos")
    save_icon(icon_silicon(), "silicon")
    save_icon(icon_calculator(), "calculator")
    save_icon(icon_pixel(), "pixel")
    save_icon(icon_clock(), "clock")
    save_icon(icon_about(), "about")
    save_icon(icon_launcher(), "launcher")
    save_icon(icon_monitor_chart(), "monitor")
    save_icon(icon_image_file(), "image_file")
    save_icon(icon_text_file(), "text_file")
    save_icon(icon_browser(), "browser")


# ---------------------------------------------------------------- wallpapers
W, H = 256, 192


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def bands(d, y0, y1, top, bottom, n):
    for i in range(n):
        ya = y0 + (y1 - y0) * i // n
        yb = y0 + (y1 - y0) * (i + 1) // n - 1
        d.rectangle([0, ya, W - 1, yb], fill=lerp(top, bottom, i / max(1, n - 1)))


def stars(d, rng, count, ymax, colors):
    for _ in range(count):
        x, y = rng.randrange(W), rng.randrange(ymax)
        d.point((x, y), fill=rng.choice(colors))


def noise1d(rng, n, octaves=3):
    vals = [0.0] * n
    amp, step = 1.0, n // 4
    for _ in range(octaves):
        pts = [rng.random() for _ in range(n // step + 2)]
        for x in range(n):
            i, f = divmod(x / step, 1)
            i = int(i)
            f = f * f * (3 - 2 * f)
            vals[x] += (pts[i] * (1 - f) + pts[i + 1] * f) * amp
        amp *= 0.5
        step = max(1, step // 2)
    return vals


def wall_wafer():
    rng = random.Random(11)
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    horizon = 132
    bands(d, 0, horizon, (12, 8, 26), (84, 38, 104), 14)
    stars(d, rng, 120, 100, [(255, 240, 255), (170, 150, 220), (120, 100, 170)])
    # the wafer rising like a planet
    cx, cy, r = 128, 136, 84
    for y in range(cy - r, horizon):
        for x in range(cx - r, cx + r + 1):
            dx, dy = x - cx, y - cy
            dist = math.hypot(dx, dy)
            if dist > r:
                continue
            t = (y - (cy - r)) / (2 * r)
            # iridescent sheen bands across the wafer
            phase = (dx * 0.6 + dy) / 18.0
            k = (math.sin(phase) + 1) / 2
            base = lerp((96, 70, 170), (60, 150, 170), k)
            base = lerp(base, (230, 120, 170), max(0.0, math.sin(phase * 0.5 + 1)) * 0.35)
            base = lerp(base, (24, 16, 40), t * 0.5)
            if dist > r - 3:
                base = lerp(base, (210, 200, 255), 0.55)
            # dies: 10px grid with 1px streets
            if (x - cx + 200) % 10 == 0 or (y - cy + 200) % 10 == 0:
                base = lerp(base, (20, 14, 36), 0.45)
            d.point((x, y), fill=base)
    # synthwave floor
    bands(d, horizon, H, (36, 18, 52), (12, 8, 20), 6)
    vx = 128
    for i in range(-14, 15):
        d.line([vx, horizon, vx + i * 22, H], fill=(120, 60, 160))
    y, gap = horizon + 2, 2
    while y < H:
        d.line([0, y, W, y], fill=(150, 70, 180))
        gap = int(gap * 1.45) + 1
        y += gap
    d.line([0, horizon, W, horizon], fill=(255, 150, 200))
    return im


def wall_circuit_dusk():
    rng = random.Random(23)
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    horizon = 104
    bands(d, 0, horizon, (34, 16, 58), (255, 150, 96), 16)
    stars(d, rng, 50, 50, [(255, 220, 240), (170, 130, 200)])
    # striped sun
    sx, sy, sr = 176, horizon - 6, 30
    for y in range(sy - sr, horizon):
        t = (y - (sy - sr)) / sr
        stripe = y > sy - 12 and (y - sy) % 5 in (0, 1)
        if stripe:
            continue
        half = int(math.sqrt(max(0, sr * sr - (y - sy) ** 2)))
        d.line([sx - half, y, sx + half, y], fill=lerp((255, 232, 120), (255, 92, 140), t))
    # far skyline of chips
    for i in range(14):
        x = rng.randrange(-10, W)
        w, h = rng.randrange(10, 26), rng.randrange(4, 12)
        d.rectangle([x, horizon - h, x + w, horizon], fill=(58, 26, 70))
        for px in range(x + 2, x + w - 1, 3):
            d.point((px, horizon - h - 1), fill=(90, 50, 96))
    # board
    d.rectangle([0, horizon, W, H], fill=(10, 36, 34))
    for y in range(horizon, H, 4):
        d.line([0, y, W, y], fill=(12, 40, 38))
    pads = []
    for _ in range(26):
        x, y = rng.randrange(0, W), rng.randrange(horizon + 6, H)
        color = rng.choice([(79, 209, 197), (236, 188, 92), (70, 160, 150)])
        length = rng.randrange(20, 90)
        dirx = rng.choice([-1, 1])
        pts = [(x, y)]
        for _ in range(4):
            seg = rng.randrange(6, max(7, length // 2))
            if rng.random() < 0.5:
                x += seg * dirx
            else:
                step = rng.choice([-1, 1])
                for _ in range(seg // 2):
                    x += dirx
                    y += step
                    if y < horizon + 3 or y >= H:
                        break
            y = max(horizon + 3, min(H - 1, y))
            pts.append((x, y))
        d.line(pts, fill=color)
        pads.append((pts[0], color))
        pads.append((pts[-1], color))
    for (x, y), c in pads:
        d.rectangle([x - 1, y - 1, x + 1, y + 1], fill=c)
        d.point((x, y), fill=(10, 36, 34))
    for _ in range(5):
        x, y = rng.randrange(10, W - 30), rng.randrange(horizon + 14, H - 20)
        w, h = rng.randrange(14, 26), rng.randrange(10, 16)
        for px in range(x + 2, x + w - 1, 3):
            d.line([px, y - 2, px, y + h + 2], fill=(200, 170, 110))
        d.rectangle([x, y, x + w, y + h], fill=(28, 24, 36))
        d.line([x, y, x + w, y], fill=(60, 54, 76))
        d.point((x + 2, y + 2), fill=(255, 120, 150))
    d.line([0, horizon, W, horizon], fill=(255, 190, 150))
    return im


def wall_silicon_valley():
    rng = random.Random(5)
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    bands(d, 0, H, (10, 14, 40), (70, 50, 110), 18)
    stars(d, rng, 150, 120, [(255, 255, 255), (180, 190, 255), (120, 120, 190)])
    # moon
    for y in range(20, 52):
        for x in range(30, 66):
            if math.hypot(x - 47, y - 35) <= 13 and math.hypot(x - 53, y - 33) > 12:
                d.point((x, y), fill=(236, 230, 210) if math.hypot(x - 44, y - 38) > 3 else (206, 198, 180))
    layers = [(118, (52, 46, 100), 22), (138, (36, 30, 72), 18), (160, (20, 16, 42), 14)]
    for li, (base, col, amp) in enumerate(layers):
        n = noise1d(rng, W + 8)
        tops = [int(base - n[x] * amp) for x in range(W)]
        for x in range(W):
            d.line([x, tops[x], x, H], fill=col)
        # server towers on the two nearer ridges
        if li >= 1:
            for _ in range(5 + li * 2):
                x = rng.randrange(4, W - 16)
                w = rng.randrange(6, 12)
                h = rng.randrange(12, 34) + li * 6
                top = min(tops[x:x + w]) - h
                d.rectangle([x, top, x + w, tops[x] + 4], fill=tuple(max(0, c - 8) for c in col))
                for wy in range(top + 3, tops[x], 4):
                    for wx in range(x + 2, x + w - 1, 3):
                        if rng.random() < 0.45:
                            d.point((wx, wy), fill=rng.choice([(255, 220, 120), (79, 209, 197), (180, 160, 255)]))
                d.line([x + w // 2, top - 6, x + w // 2, top], fill=col)
                d.point((x + w // 2, top - 7), fill=(255, 90, 110))
        # fog between layers
        fy = base + 6
        for y in range(fy, fy + 3):
            for x in range(0, W, 2):
                if rng.random() < 0.5:
                    d.point((x + (y % 2), y), fill=lerp(col, (120, 110, 170), 0.35))
    return im


def build_wallpapers():
    os.makedirs(os.path.join(OUT, "wallpapers"), exist_ok=True)
    for name, fn in (("wafer", wall_wafer), ("circuit_dusk", wall_circuit_dusk),
                     ("silicon_valley", wall_silicon_valley)):
        path = os.path.join(OUT, "wallpapers", name + ".png")
        fn().resize((W * 4, H * 4), Image.NEAREST).save(path)


def main():
    stamp = os.path.join(OUT, ".stamp")
    me = os.path.abspath(__file__)
    if os.path.exists(stamp) and os.path.getmtime(stamp) >= os.path.getmtime(me):
        return
    build_icons()
    build_wallpapers()
    open(stamp, "w").write("ok")
    print("[gen] art")


if __name__ == "__main__":
    main()
