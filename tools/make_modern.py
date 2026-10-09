"""silicon's flat, smooth icon set, cursors and wallpapers (art/modern/).

Everything is drawn at 4x and filtered down, so edges are anti-aliased. Deterministic.
    python tools/make_modern.py            writes art/modern/{icons,cursors,wallpapers}
    python tools/make_modern.py sheet      also writes art/modern/sheet.png (contact sheet for a look)
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "art", "modern")
FONT = os.path.join(ROOT, "art", "fonts", "DejaVuSans-Bold.ttf")
FONT_R = os.path.join(ROOT, "art", "fonts", "DejaVuSans.ttf")
S = 4                       # supersampling
SZ = 64

WHITE = (255, 255, 255, 255)
SOFT = (255, 255, 255, 190)
DIM = (255, 255, 255, 110)
INK = (24, 28, 38, 255)

PAL = {
    "blue": ((92, 164, 255), (40, 98, 230)),
    "teal": ((70, 218, 198), (18, 150, 152)),
    "green": ((104, 214, 124), (34, 152, 84)),
    "orange": ((255, 176, 76), (238, 100, 32)),
    "red": ((255, 116, 112), (214, 52, 72)),
    "pink": ((255, 126, 176), (212, 62, 134)),
    "purple": ((168, 128, 255), (100, 72, 214)),
    "indigo": ((116, 126, 255), (62, 62, 204)),
    "slate": ((84, 94, 114), (42, 48, 62)),
    "dark": ((58, 64, 80), (22, 26, 36)),
    "yellow": ((255, 220, 96), (244, 164, 34)),
    "gray": ((190, 198, 210), (128, 136, 152)),
    "sky": ((120, 206, 255), (54, 144, 236)),
    "navy": ((52, 72, 130), (22, 32, 72)),
    "lime": ((176, 226, 84), (92, 168, 40)),
}


class Art:
    """A 64x64 icon drawn in 64-unit coordinates at 4x."""

    def __init__(self):
        self.im = Image.new("RGBA", (SZ * S, SZ * S), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)

    def p(self, v):
        return v * S

    def pts(self, pts):
        return [(x * S, y * S) for x, y in pts]

    def rrect(self, x0, y0, x1, y1, r, fill, outline=None, width=1):
        self.d.rounded_rectangle([x0 * S, y0 * S, x1 * S - 1, y1 * S - 1], radius=r * S, fill=fill,
                                 outline=outline, width=int(width * S))

    def rect(self, x0, y0, x1, y1, fill):
        self.d.rectangle([x0 * S, y0 * S, x1 * S - 1, y1 * S - 1], fill=fill)

    def circle(self, cx, cy, r, fill=None, outline=None, width=1):
        self.d.ellipse([(cx - r) * S, (cy - r) * S, (cx + r) * S - 1, (cy + r) * S - 1], fill=fill, outline=outline,
                       width=int(width * S))

    def line(self, pts, w, fill, cap=True):
        P = self.pts(pts)
        self.d.line(P, fill=fill, width=int(w * S), joint="curve")
        if cap:
            for x, y in (P[0], P[-1]):
                r = w * S / 2
                self.d.ellipse([x - r, y - r, x + r, y + r], fill=fill)

    def poly(self, pts, fill):
        self.d.polygon(self.pts(pts), fill=fill)

    def arc(self, cx, cy, r, a0, a1, w, fill):
        self.d.arc([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], a0, a1, fill=fill, width=int(w * S))
        for a in (a0, a1):
            x = cx + r * math.cos(math.radians(a)) - 0
            y = cy + r * math.sin(math.radians(a))
            rr = w * S / 2
            self.d.ellipse([x * S - rr, y * S - rr, x * S + rr, y * S + rr], fill=fill)

    def text(self, x, y, s, size, fill, bold=True, anchor="mm"):
        f = ImageFont.truetype(FONT if bold else FONT_R, int(size * S))
        self.d.text((x * S, y * S), s, font=f, fill=fill, anchor=anchor)

    def done(self):
        return self.im.resize((SZ, SZ), Image.LANCZOS)


def gradient(w, h, top, bot):
    t = np.linspace(0, 1, h)[:, None, None]
    a = np.array(top, float)[None, None, :]
    b = np.array(bot, float)[None, None, :]
    arr = (a + (b - a) * t) * np.ones((1, w, 1))
    return Image.fromarray(arr.astype(np.uint8), "RGB")


def tile(pal):
    """The rounded square an app icon sits on. Returns an Art with the tile already painted."""
    top, bot = PAL[pal] if isinstance(pal, str) else pal
    a = Art()
    # soft shadow
    sh = Image.new("RGBA", a.im.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([5 * S, 7 * S, 59 * S, 61 * S], radius=14 * S, fill=(0, 0, 0, 70))
    sh = sh.filter(ImageFilter.GaussianBlur(2.2 * S))
    a.im.alpha_composite(sh)
    mask = Image.new("L", a.im.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([4 * S, 3 * S, 60 * S - 1, 59 * S - 1], radius=14 * S, fill=255)
    body = gradient(a.im.width, a.im.height, top, bot).convert("RGBA")
    # gloss: a faint light wash over the upper half
    gl = Image.new("RGBA", a.im.size, (255, 255, 255, 0))
    garr = np.zeros((a.im.height, a.im.width, 4), np.uint8)
    garr[..., :3] = 255
    ys = np.arange(a.im.height)[:, None]
    garr[..., 3] = np.clip(46 - (ys - 3 * S) * 46 / (34 * S), 0, 46).astype(np.uint8) * np.ones((1, a.im.width), np.uint8)
    gl = Image.fromarray(garr, "RGBA")
    body.alpha_composite(gl)
    a.im.paste(body, (0, 0), mask)
    a.d = ImageDraw.Draw(a.im)
    return a


# ================================================================ glyphs on a tile
def g_terminal(a):
    a.line([(16, 22), (27, 31), (16, 40)], 4.4, (110, 230, 150, 255))
    a.line([(31, 42), (46, 42)], 4.4, WHITE)


def g_files(a):
    a.rrect(13, 21, 51, 47, 5, WHITE)
    a.rrect(13, 17, 29, 26, 4, SOFT)
    a.rrect(13, 25, 51, 47, 5, WHITE)
    a.rect(13, 29, 51, 31, (255, 255, 255, 120))


def g_settings(a):
    cx, cy = 32, 31
    for i in range(8):
        ang = math.radians(i * 45)
        x, y = cx + 15 * math.cos(ang), cy + 15 * math.sin(ang)
        a.line([(cx + 10 * math.cos(ang), cy + 10 * math.sin(ang)), (x, y)], 6, WHITE, cap=False)
    a.circle(cx, cy, 12, WHITE)
    a.circle(cx, cy, 5, PAL["gray"][1] + (255,))


def g_monitor(a):
    a.line([(13, 44), (23, 32), (31, 38), (41, 22), (51, 28)], 4, WHITE)
    a.rrect(13, 46, 51, 48, 1, DIM)
    for x, h in ((17, 8), (27, 12), (37, 6)):
        a.rrect(x, 46 - h, x + 5, 46, 1.5, SOFT)


def g_trash(a):
    a.rrect(19, 24, 45, 49, 4, WHITE)
    a.rrect(15, 19, 49, 24, 2, WHITE)
    a.rrect(27, 14, 37, 20, 2, SOFT)
    for x in (26, 32, 38):
        a.line([(x, 29), (x, 44)], 2.4, PAL["gray"][1] + (255,))


def g_wallpapers(a):
    a.rrect(11, 15, 53, 47, 5, WHITE)
    a.circle(40, 24, 4.2, PAL["yellow"][0] + (255,))
    a.poly([(11, 44), (24, 28), (33, 38), (40, 32), (53, 44), (53, 42), (53, 47), (11, 47)], PAL["teal"][1] + (255,))


def g_notes(a):         # slate: a note sheet with lines
    a.rrect(16, 12, 48, 50, 4, WHITE)
    for y in (22, 29, 36):
        a.line([(22, y), (42, y)], 2.6, PAL["yellow"][1] + (255,))
    a.line([(22, 43), (33, 43)], 2.6, PAL["yellow"][1] + (255,))


def g_games(a):
    a.rrect(10, 22, 54, 44, 11, WHITE)
    a.rrect(18, 31, 28, 35, 1.5, PAL["indigo"][1] + (255,))
    a.rrect(21, 28, 25, 38, 1.5, PAL["indigo"][1] + (255,))
    a.circle(40, 29, 3, PAL["red"][0] + (255,))
    a.circle(46, 35, 3, PAL["green"][1] + (255,))


def g_minesweeper(a):
    a.circle(30, 34, 11, INK)
    for i in range(8):
        ang = math.radians(i * 45)
        a.line([(30 + 9 * math.cos(ang), 34 + 9 * math.sin(ang)), (30 + 15 * math.cos(ang), 34 + 15 * math.sin(ang))], 3, INK)
    a.circle(26, 30, 3, (255, 255, 255, 160))
    a.poly([(44, 14), (44, 28), (54, 21)], PAL["red"][0] + (255,))
    a.line([(44, 12), (44, 32)], 2.4, WHITE)


def g_snake(a):
    a.line([(16, 44), (16, 30), (30, 30), (30, 42), (44, 42), (44, 22)], 8, WHITE)
    a.circle(44, 20, 6, WHITE)
    a.circle(42, 18, 1.6, PAL["green"][1] + (255,))
    a.circle(46, 18, 1.6, PAL["green"][1] + (255,))
    a.circle(18, 17, 3.5, PAL["red"][0] + (255,))


def g_reversi(a):
    a.circle(24, 24, 9, INK)
    a.circle(40, 24, 9, WHITE)
    a.circle(24, 40, 9, WHITE)
    a.circle(40, 40, 9, INK)


def g_stacker(a):
    cols = [PAL["red"][0], PAL["yellow"][0], PAL["sky"][0], PAL["lime"][0]]
    cells = [(2, 1, 0), (1, 2, 0), (2, 2, 0), (3, 2, 0), (0, 3, 1), (1, 3, 1), (2, 3, 2), (3, 3, 2)]
    for x, y, c in cells:
        a.rrect(15 + x * 8, 14 + y * 8, 15 + x * 8 + 7, 14 + y * 8 + 7, 2, cols[c] + (255,))


def g_2048(a):
    a.text(32, 31, "2048", 15, WHITE)


def g_breakout(a):
    for i, c in enumerate(("red", "orange", "yellow")):
        for j in range(4):
            a.rrect(12 + j * 10, 14 + i * 7, 12 + j * 10 + 8, 14 + i * 7 + 5, 1.5, PAL[c][0] + (255,))
    a.circle(34, 40, 3.4, WHITE)
    a.rrect(22, 49, 42, 53, 2, WHITE)


def g_flappy(a):
    a.circle(30, 32, 13, (255, 232, 100, 255))
    a.circle(35, 28, 4.6, WHITE)
    a.circle(36.5, 28, 2, INK)
    a.poly([(40, 33), (52, 31), (42, 38)], PAL["orange"][0] + (255,))
    a.rrect(14, 32, 26, 40, 4, PAL["orange"][0] + (255,))
    a.rrect(44, 10, 54, 20, 2, PAL["green"][0] + (255,))
    a.rrect(10, 46, 20, 56, 2, PAL["green"][0] + (255,))


def g_photos(a):
    a.rrect(11, 15, 53, 49, 5, WHITE)
    a.circle(41, 25, 4.4, PAL["yellow"][0] + (255,))
    a.poly([(11, 45), (24, 28), (33, 39), (39, 33), (53, 46), (53, 49), (11, 49)], PAL["green"][1] + (255,))


def g_piano(a):
    a.rrect(11, 15, 53, 49, 4, WHITE)
    for x in (22, 32, 42):
        a.line([(x, 15), (x, 49)], 1.2, (200, 205, 215, 255), cap=False)
    for x in (18, 28, 39):
        a.rrect(x, 15, x + 7, 35, 2, INK)


def g_pdf(a):
    a.rrect(16, 10, 48, 54, 4, WHITE)
    a.poly([(38, 10), (48, 10), (48, 20)], (220, 224, 232, 255))
    a.rrect(21, 31, 43, 43, 2, PAL["red"][1] + (255,))
    a.text(32, 37, "PDF", 8, WHITE)
    a.line([(22, 21), (36, 21)], 2.2, (200, 205, 215, 255))
    a.line([(22, 26), (40, 26)], 2.2, (200, 205, 215, 255))


def g_module(a):
    a.poly([(32, 12), (50, 21), (50, 43), (32, 52), (14, 43), (14, 21)], WHITE)
    a.line([(14, 21), (32, 31), (50, 21)], 2.4, PAL["purple"][1] + (255,), cap=False)
    a.line([(32, 31), (32, 52)], 2.4, PAL["purple"][1] + (255,), cap=False)


def g_write(a):
    a.rrect(15, 11, 47, 53, 4, WHITE)
    a.line([(21, 21), (41, 21)], 2.6, PAL["blue"][1] + (255,))
    a.line([(21, 28), (41, 28)], 2.6, (190, 200, 220, 255))
    a.line([(21, 35), (35, 35)], 2.6, (190, 200, 220, 255))
    a.line([(36, 48), (51, 28)], 5, PAL["yellow"][0] + (255,))
    a.poly([(33, 52), (36, 46), (39, 49)], INK)


def g_calendar(a):
    a.rrect(12, 14, 52, 52, 5, WHITE)
    a.rrect(12, 14, 52, 26, 5, PAL["red"][0] + (255,))
    a.rect(12, 21, 52, 26, PAL["red"][0] + (255,))
    a.line([(22, 10), (22, 18)], 3, WHITE)
    a.line([(42, 10), (42, 18)], 3, WHITE)
    a.text(32, 40, "6", 17, PAL["slate"][0] + (255,))


def g_tasks(a):
    a.rrect(14, 12, 50, 52, 5, WHITE)
    for i, y in enumerate((23, 32, 41)):
        a.line([(19, y), (21, y + 2.4), (25, y - 2.6)], 2.4, PAL["green"][1] + (255,))
        a.line([(29, y), (44, y)], 2.6, (190, 200, 215, 255))


def g_hex(a):
    a.text(32, 32, "0x", 22, (130, 235, 190, 255))
    a.text(32, 50, "1F", 9, SOFT)


def g_storage(a):
    a.rrect(10, 22, 54, 46, 5, WHITE)
    a.rrect(10, 22, 54, 46, 5, None, outline=None)
    a.circle(46, 35, 2.8, PAL["green"][0] + (255,))
    a.line([(16, 35), (36, 35)], 3, (200, 206, 218, 255))
    a.rrect(10, 40, 54, 46, 3, (214, 220, 230, 255))


def g_convert(a):
    a.line([(16, 24), (45, 24)], 4, WHITE)
    a.poly([(44, 16), (54, 24), (44, 32)], WHITE)
    a.line([(48, 40), (19, 40)], 4, SOFT)
    a.poly([(20, 32), (10, 40), (20, 48)], SOFT)


def g_palette(a):
    a.circle(32, 32, 20, WHITE)
    a.circle(40, 40, 6, PAL["purple"][1] + (255,))
    for (x, y, c) in ((22, 26, "red"), (30, 20, "yellow"), (40, 24, "green"), (22, 38, "sky")):
        a.circle(x, y, 4.2, PAL[c][0] + (255,))


def g_capture(a):
    for (x, y, dx, dy) in ((14, 14, 1, 1), (50, 14, -1, 1), (14, 50, 1, -1), (50, 50, -1, -1)):
        a.line([(x, y + dy * 10), (x, y), (x + dx * 10, y)], 4, WHITE)
    a.circle(32, 32, 7, WHITE)


def g_weather(a):
    a.circle(24, 24, 9, (255, 226, 110, 255))
    for i in range(8):
        ang = math.radians(i * 45)
        a.line([(24 + 13 * math.cos(ang), 24 + 13 * math.sin(ang)), (24 + 17 * math.cos(ang), 24 + 17 * math.sin(ang))], 2.2, (255, 226, 110, 255))
    a.circle(34, 41, 9, WHITE)
    a.circle(44, 40, 7, WHITE)
    a.circle(24, 44, 6, WHITE)
    a.rrect(24, 41, 47, 50, 4, WHITE)


def g_typing(a):
    a.rrect(8, 20, 56, 46, 5, WHITE)
    for r in range(2):
        for c in range(7):
            a.rrect(12 + c * 6, 24 + r * 7, 15 + c * 6 + 1, 27 + r * 7 + 1, 1, (170, 180, 198, 255))
    a.rrect(20, 38, 44, 42, 1.5, (170, 180, 198, 255))


def g_network(a):
    a.circle(32, 32, 19, None, WHITE, 3)
    a.d.ellipse([22 * S, 13 * S, 42 * S, 51 * S], outline=WHITE, width=3 * S)
    a.line([(13, 32), (51, 32)], 2.6, WHITE, cap=False)


def g_archive(a):
    a.rrect(12, 20, 52, 52, 5, WHITE)
    a.rrect(12, 12, 52, 24, 5, SOFT)
    a.rect(29, 12, 35, 44, PAL["orange"][1] + (255,))
    for y in (16, 22, 28, 34, 40):
        a.rect(29, y, 35, y + 3, INK if (y // 6) % 2 else WHITE)
    a.rrect(28, 41, 36, 49, 2, PAL["orange"][1] + (255,))


def g_pong(a):
    a.rrect(12, 20, 17, 42, 2, WHITE)
    a.rrect(47, 24, 52, 46, 2, WHITE)
    a.circle(32, 34, 3.6, WHITE)
    for y in range(12, 56, 8):
        a.rrect(31, y, 33, y + 4, 1, DIM)


def g_invaders(a):
    sh = ["..#.....#..", "...#...#...", "..#######..", ".##.###.##.", "###########", "#.#######.#", "#.#.....#.#", "...##.##..."]
    for y, row in enumerate(sh):
        for x, ch in enumerate(row):
            if ch == "#":
                a.rect(11 + x * 3.6, 16 + y * 3.6, 11 + x * 3.6 + 3.6, 16 + y * 3.6 + 3.6, (130, 240, 160, 255))
    a.rrect(26, 50, 38, 54, 2, WHITE)


def g_asteroids(a):
    a.poly([(14, 24), (24, 12), (36, 14), (44, 24), (40, 36), (28, 42), (16, 36)], (190, 198, 214, 255))
    a.circle(26, 24, 3, (140, 148, 168, 255))
    a.circle(36, 31, 2.4, (140, 148, 168, 255))
    a.poly([(44, 40), (52, 52), (44, 49), (36, 52)], WHITE)


def g_runner(a):
    a.circle(34, 17, 5, WHITE)
    a.line([(33, 24), (28, 36), (20, 38)], 4.4, WHITE)
    a.line([(28, 36), (36, 42), (35, 52)], 4.4, WHITE)
    a.line([(32, 28), (42, 30), (46, 24)], 4, WHITE)
    a.line([(32, 28), (24, 28), (20, 33)], 4, WHITE)
    a.rrect(8, 54, 56, 57, 1.5, DIM)


def g_connect4(a):
    a.rrect(9, 13, 55, 51, 6, (36, 80, 200, 255))
    for r in range(4):
        for c in range(5):
            col = (255, 228, 96, 255) if (r, c) in ((3, 1), (3, 3), (2, 2)) else (232, 84, 90, 255) if (r, c) in ((3, 2), (2, 3), (3, 0)) else (22, 48, 130, 255)
            a.circle(17 + c * 8, 21 + r * 9, 3.6, col)


def g_memory(a):
    a.rrect(11, 16, 31, 48, 4, WHITE)
    a.rrect(33, 16, 53, 48, 4, SOFT)
    a.text(21, 33, "?", 17, PAL["purple"][1] + (255,))
    a.circle(43, 32, 5, PAL["pink"][0] + (255,))


def g_wordy(a):
    for i, (ch, c) in enumerate((("W", "green"), ("O", "yellow"), ("R", "gray"))):
        a.rrect(9 + i * 16, 22, 9 + i * 16 + 14, 36, 3, PAL[c][1] + (255,))
        a.text(16 + i * 16, 29.5, ch, 11, WHITE)
    for i in range(3):
        a.rrect(9 + i * 16, 40, 9 + i * 16 + 14, 52, 3, (255, 255, 255, 70))


def g_sudoku(a):
    a.rrect(12, 12, 52, 52, 4, WHITE)
    for k in (25.3, 38.7):
        a.line([(k, 12), (k, 52)], 1.6, (120, 130, 150, 255), cap=False)
        a.line([(12, k), (52, k)], 1.6, (120, 130, 150, 255), cap=False)
    a.text(19, 19, "5", 8, PAL["blue"][1] + (255,))
    a.text(32, 32, "3", 8, INK)
    a.text(45, 45, "9", 8, PAL["blue"][1] + (255,))


def g_solitaire(a):
    a.rrect(13, 14, 41, 48, 4, (255, 255, 255, 200))
    a.rrect(23, 18, 51, 52, 4, WHITE)
    a.text(30, 28, "A", 11, INK)
    a.text(37, 41, "♠", 17, INK)


def g_chess(a):
    a.circle(32, 19, 6.4, WHITE)
    a.poly([(26, 26), (38, 26), (41, 38), (23, 38)], WHITE)
    a.rrect(20, 38, 44, 44, 2.5, WHITE)
    a.rrect(16, 44, 48, 50, 3, WHITE)


def g_emoji(a):
    a.circle(32, 32, 20, (255, 214, 80, 255))
    a.circle(25, 27, 3, INK)
    a.circle(39, 27, 3, INK)
    a.arc(32, 32, 11, 25, 155, 3.4, INK)


def g_apps(a):
    for r in range(3):
        for c in range(3):
            a.rrect(13 + c * 14, 13 + r * 14, 13 + c * 14 + 10, 13 + r * 14 + 10, 3, WHITE if (r + c) % 2 == 0 else SOFT)


def g_notifications(a):
    a.poly([(32, 11), (42, 18), (44, 33), (50, 43), (14, 43), (20, 33), (22, 18)], WHITE)
    a.circle(32, 18, 8, WHITE)
    a.rect(14, 38, 50, 43, WHITE)
    a.circle(32, 49, 4.4, WHITE)


def g_firewall(a):
    a.poly([(32, 10), (51, 17), (51, 33), (32, 54), (13, 33), (13, 17)], WHITE)
    a.poly([(32, 25), (39, 35), (37, 44), (27, 44), (25, 35)], PAL["orange"][0] + (255,))
    a.poly([(32, 33), (36, 39), (34, 44), (30, 44), (28, 39)], (255, 226, 110, 255))


def g_docs(a):
    a.rrect(16, 10, 48, 54, 4, WHITE)
    a.poly([(38, 10), (48, 10), (48, 20)], (214, 222, 236, 255))
    for y in (24, 31, 38, 45):
        a.line([(22, y), (42, y)], 2.4, PAL["indigo"][0] + (255,) if y == 24 else (190, 200, 220, 255))


def g_mail(a):
    a.rrect(10, 17, 54, 47, 5, WHITE)
    a.line([(12, 20), (32, 35), (52, 20)], 3, PAL["blue"][1] + (255,))


def g_fonts(a):
    a.text(24, 38, "A", 26, WHITE)
    a.text(44, 40, "a", 20, SOFT)


def g_silicon(a):
    a.rrect(19, 19, 45, 45, 4, WHITE)
    a.rrect(25, 25, 39, 39, 2, PAL["indigo"][1] + (255,))
    for k in (24, 32, 40):
        a.rrect(k - 1.2, 11, k + 1.2, 19, 1, SOFT)
        a.rrect(k - 1.2, 45, k + 1.2, 53, 1, SOFT)
        a.rrect(11, k - 1.2, 19, k + 1.2, 1, SOFT)
        a.rrect(45, k - 1.2, 53, k + 1.2, 1, SOFT)


def g_pixel(a):
    for r in range(4):
        for c in range(4):
            col = WHITE if (r + c) % 2 == 0 else SOFT
            a.rect(12 + c * 8.5, 14 + r * 8.5, 12 + c * 8.5 + 7.5, 14 + r * 8.5 + 7.5, col)
    a.line([(36, 46), (52, 22)], 6, PAL["yellow"][0] + (255,))
    a.poly([(32, 53), (36, 45), (40, 49)], INK)


def g_clock(a):
    a.circle(32, 32, 20, WHITE)
    for i in range(12):
        ang = math.radians(i * 30)
        a.line([(32 + 16 * math.cos(ang), 32 + 16 * math.sin(ang)), (32 + 18 * math.cos(ang), 32 + 18 * math.sin(ang))], 1.6, (170, 178, 196, 255), cap=False)
    a.line([(32, 32), (32, 19)], 3, INK)
    a.line([(32, 32), (42, 37)], 3, INK)
    a.circle(32, 32, 2.4, PAL["red"][0] + (255,))


def g_about(a):
    a.circle(32, 32, 19, None, WHITE, 3.2)
    a.circle(32, 22, 2.8, WHITE)
    a.line([(32, 29), (32, 43)], 4, WHITE)


def g_launcher(a):
    a.circle(28, 28, 13, None, WHITE, 4.4)
    a.line([(38, 38), (51, 51)], 5.4, WHITE)


def g_browser(a):
    a.circle(32, 32, 20, WHITE)
    a.d.ellipse([23 * S, 12 * S, 41 * S, 52 * S], outline=PAL["sky"][1] + (255,), width=int(2.4 * S))
    a.line([(12, 32), (52, 32)], 2.4, PAL["sky"][1] + (255,), cap=False)
    a.line([(15, 22), (49, 22)], 2.2, PAL["sky"][1] + (255,), cap=False)
    a.line([(15, 42), (49, 42)], 2.2, PAL["sky"][1] + (255,), cap=False)


def g_calculator(a):
    a.rrect(16, 11, 48, 53, 5, WHITE)
    a.rrect(20, 15, 44, 25, 2.5, (60, 70, 92, 255))
    a.text(40, 20.5, "42", 8, (140, 240, 190, 255), anchor="rm")
    for r in range(4):
        for c in range(4):
            col = PAL["orange"][0] + (255,) if c == 3 else (196, 202, 216, 255)
            a.rrect(20 + c * 6.2, 29 + r * 6, 20 + c * 6.2 + 4.6, 29 + r * 6 + 4.4, 1.5, col)


def g_chart(a):
    g_monitor(a)


def g_restart(a):
    a.arc(32, 32, 16, 40, 330, 5, WHITE)
    a.poly([(42, 10), (52, 22), (38, 24)], WHITE)


def g_shutdown(a):
    a.arc(32, 34, 16, -50, 230, 5, WHITE)
    a.line([(32, 12), (32, 32)], 5, WHITE)


def g_media(a):
    a.circle(32, 32, 20, WHITE)
    a.poly([(27, 22), (27, 42), (44, 32)], PAL["red"][1] + (255,))


def g_vpn(a):
    a.rrect(16, 28, 48, 52, 5, WHITE)
    a.arc(32, 28, 10, 180, 360, 4.6, WHITE)
    a.circle(32, 39, 3.4, PAL["teal"][1] + (255,))
    a.line([(32, 41), (32, 46)], 3, PAL["teal"][1] + (255,))


TILES = {
    "terminal": ("dark", g_terminal), "calculator": ("slate", g_calculator), "files": ("blue", g_files), "settings": ("gray", g_settings),
    "monitor": ("teal", g_monitor), "chart": ("teal", g_chart), "trash": ("slate", g_trash),
    "wallpapers": ("purple", g_wallpapers), "slate": ("yellow", g_notes), "games": ("indigo", g_games),
    "minesweeper": ("gray", g_minesweeper), "snake": ("green", g_snake), "reversi": ("teal", g_reversi),
    "stacker": ("purple", g_stacker), "g2048": ("orange", g_2048), "breakout": ("navy", g_breakout),
    "flappy": ("sky", g_flappy), "photos": ("sky", g_photos), "piano": ("slate", g_piano), "pdf": ("red", g_pdf),
    "sapp": ("purple", g_module), "module": ("purple", g_module), "write": ("blue", g_write),
    "calendar": ("gray", g_calendar), "tasks": ("green", g_tasks), "hex": ("dark", g_hex),
    "storage": ("slate", g_storage), "convert": ("teal", g_convert), "palette": ("pink", g_palette),
    "capture": ("indigo", g_capture), "weather": ("sky", g_weather), "typing": ("slate", g_typing),
    "network": ("blue", g_network), "archive": ("orange", g_archive), "pong": ("dark", g_pong),
    "invaders": ("navy", g_invaders), "asteroids": ("dark", g_asteroids), "runner": ("orange", g_runner),
    "connect4": ("navy", g_connect4), "memory": ("purple", g_memory), "wordy": ("slate", g_wordy),
    "sudoku": ("blue", g_sudoku), "solitaire": ("green", g_solitaire), "chess": ("dark", g_chess),
    "emoji": ("yellow", g_emoji), "apps": ("indigo", g_apps), "notifications": ("red", g_notifications),
    "firewall": ("orange", g_firewall), "docs": ("indigo", g_docs), "mail": ("sky", g_mail),
    "fonts": ("pink", g_fonts), "silicon": ("indigo", g_silicon), "pixel": ("pink", g_pixel),
    "clock": ("slate", g_clock), "about": ("blue", g_about), "launcher": ("purple", g_launcher),
    "browser": ("blue", g_browser), "restart": ("orange", g_restart), "shutdown": ("red", g_shutdown),
    "media": ("red", g_media), "vpn": ("teal", g_vpn),
}


# ================================================================ file-type icons (no tile)
def doc(body=(255, 255, 255, 255), fold=(214, 220, 232, 255)):
    a = Art()
    sh = Image.new("RGBA", a.im.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([14 * S, 8 * S, 52 * S, 62 * S], radius=5 * S, fill=(0, 0, 0, 70))
    a.im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(2 * S)))
    a.d = ImageDraw.Draw(a.im)
    a.rrect(12, 5, 52, 59, 5, body, outline=(196, 202, 214, 255), width=1)
    a.poly([(40, 5), (52, 17), (40, 17)], fold)
    a.line([(40, 5), (40, 17), (52, 17)], 1, (196, 202, 214, 255), cap=False)
    return a


def i_file():
    a = doc()
    return a.done()


def i_text_file():
    a = doc()
    for y in (27, 33, 39, 45):
        a.line([(19, y), (45 if y != 45 else 34, y)], 2.4, (150, 160, 180, 255))
    return a.done()


def i_image_file():
    a = doc()
    a.rrect(18, 25, 46, 49, 3, (170, 214, 250, 255))
    a.circle(38, 33, 3.4, (255, 226, 110, 255))
    a.poly([(18, 46), (28, 35), (35, 43), (40, 38), (46, 46), (46, 49), (18, 49)], (96, 190, 130, 255))
    return a.done()


def folder(open_=False):
    a = Art()
    sh = Image.new("RGBA", a.im.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([6 * S, 16 * S, 58 * S, 58 * S], radius=6 * S, fill=(0, 0, 0, 80))
    a.im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(2 * S)))
    a.d = ImageDraw.Draw(a.im)
    back = (88, 150, 238, 255)
    a.rrect(6, 10, 28, 24, 4, back)
    a.rrect(6, 15, 58, 52, 6, back)
    if open_:
        a.rrect(11, 19, 53, 30, 2, (255, 255, 255, 235))
        a.rrect(15, 22, 49, 33, 2, (230, 238, 252, 255))
    front = Image.new("RGBA", a.im.size, (0, 0, 0, 0))
    fa = Art()
    fa.rrect(6, 22, 58, 54, 6, (120, 184, 255, 255))
    g = gradient(SZ * S, SZ * S, (140, 196, 255), (70, 140, 236)).convert("RGBA")
    m = fa.im.getchannel("A")
    a.im.paste(g, (0, 0), m)
    a.d = ImageDraw.Draw(a.im)
    return a.done()


def i_disk():
    a = Art()
    sh = Image.new("RGBA", a.im.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([6 * S, 22 * S, 58 * S, 54 * S], radius=7 * S, fill=(0, 0, 0, 80))
    a.im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(2 * S)))
    a.d = ImageDraw.Draw(a.im)
    a.rrect(6, 18, 58, 50, 7, (206, 212, 224, 255))
    a.rrect(6, 40, 58, 50, 7, (150, 158, 176, 255))
    a.rect(6, 40, 58, 44, (150, 158, 176, 255))
    a.line([(14, 29), (38, 29)], 3, (120, 128, 148, 255))
    a.circle(49, 45, 2.4, (110, 224, 140, 255))
    return a.done()


FILE_ICONS = {"file": i_file, "text_file": i_text_file, "image_file": i_image_file,
              "folder": lambda: folder(False), "folder_full": lambda: folder(True), "disk": i_disk}


def build_icons(sheet):
    d = os.path.join(OUT, "icons")
    os.makedirs(d, exist_ok=True)
    done = {}
    for name, (pal, fn) in TILES.items():
        a = tile(pal)
        fn(a)
        im = a.done()
        im.save(os.path.join(d, name + ".png"), optimize=True)
        done[name] = im
    for name, fn in FILE_ICONS.items():
        im = fn()
        im.save(os.path.join(d, name + ".png"), optimize=True)
        done[name] = im
    if sheet:
        cols = 12
        rows = (len(done) + cols - 1) // cols
        sh = Image.new("RGBA", (cols * 76, rows * 76), (236, 238, 243, 255))
        for i, (n, im) in enumerate(done.items()):
            sh.alpha_composite(im, ((i % cols) * 76 + 6, (i // cols) * 76 + 6))
        sh.save(os.path.join(OUT, "sheet.png"))


# ================================================================ cursors
CUR_S = 8


def cursor_img(w, h, painter):
    big = Image.new("RGBA", (w * CUR_S, h * CUR_S), (0, 0, 0, 0))
    painter(big)
    # soft shadow
    sh = Image.new("RGBA", big.size, (0, 0, 0, 0))
    a = big.getchannel("A")
    shadow = Image.new("RGBA", big.size, (0, 0, 0, 255))
    shadow.putalpha(a.point(lambda v: v * 90 // 255))
    shadow = shadow.filter(ImageFilter.GaussianBlur(0.9 * CUR_S))
    out = Image.new("RGBA", big.size, (0, 0, 0, 0))
    out.alpha_composite(shadow, (int(0.6 * CUR_S), int(0.9 * CUR_S)))
    out.alpha_composite(big)
    return out.resize((w, h), Image.LANCZOS)


def outlined(poly, big, fill=(255, 255, 255, 255), edge=(20, 24, 32, 255), pad=1.3):
    d = ImageDraw.Draw(big)
    P = [(x * CUR_S, y * CUR_S) for x, y in poly]
    d.polygon(P, fill=edge)
    d.line(P + [P[0]], fill=edge, width=int(pad * 2 * CUR_S), joint="curve")
    # inner fill: shrink by drawing the polygon again smaller via mask erosion
    mask = Image.new("L", big.size, 0)
    ImageDraw.Draw(mask).polygon(P, fill=255)
    er = mask.filter(ImageFilter.MinFilter(int(pad * CUR_S) * 2 + 1))
    inner = Image.new("RGBA", big.size, fill)
    big.paste(inner, (0, 0), er)


def c_arrow(big):
    outlined([(2, 1.2), (2, 17.5), (6.2, 13.6), (9, 19.8), (11.6, 18.6), (8.8, 12.6), (14.2, 12.6)], big)


def c_hand(big):
    pts = [(8, 1.5), (10.2, 1.5), (11, 3), (11, 9.5), (12.6, 9), (14.4, 9.6), (15.2, 10.6), (17, 10.4), (18.6, 11.6), (19, 13),
           (19, 18.5), (17, 22), (9, 22), (6, 18.5), (2.6, 13.4), (2.4, 12), (3.8, 11.2), (5.4, 12.4), (7, 14), (7, 3)]
    outlined(pts, big)


def c_ibeam(big):
    d = ImageDraw.Draw(big)
    for col, w in (((20, 24, 32, 255), 3.4), ((255, 255, 255, 255), 1.5)):
        W = int(w * CUR_S)
        d.line([(6.5 * CUR_S, 3 * CUR_S), (6.5 * CUR_S, 19 * CUR_S)], fill=col, width=W)
        d.line([(3.5 * CUR_S, 3 * CUR_S), (9.5 * CUR_S, 3 * CUR_S)], fill=col, width=W)
        d.line([(3.5 * CUR_S, 19 * CUR_S), (9.5 * CUR_S, 19 * CUR_S)], fill=col, width=W)


def c_cross(big):
    d = ImageDraw.Draw(big)
    for col, w in (((20, 24, 32, 255), 3.4), ((255, 255, 255, 255), 1.5)):
        W = int(w * CUR_S)
        d.line([(10 * CUR_S, 2 * CUR_S), (10 * CUR_S, 18 * CUR_S)], fill=col, width=W)
        d.line([(2 * CUR_S, 10 * CUR_S), (18 * CUR_S, 10 * CUR_S)], fill=col, width=W)


def c_move(big):
    pts = [(11, 1.5), (15, 6), (12.4, 6), (12.4, 9.6), (16, 9.6), (16, 7), (20.5, 11), (16, 15), (16, 12.4), (12.4, 12.4), (12.4, 16),
           (15, 16), (11, 20.5), (7, 16), (9.6, 16), (9.6, 12.4), (6, 12.4), (6, 15), (1.5, 11), (6, 7), (6, 9.6), (9.6, 9.6), (9.6, 6), (7, 6)]
    outlined(pts, big)


def c_resize(big):      # diagonal double arrow (nwse)
    pts = [(2, 2), (9.4, 2), (7, 4.4), (17.6, 15), (20, 12.6), (20, 20), (12.6, 20), (15, 17.6), (4.4, 7), (2, 9.4)]
    outlined(pts, big)


def c_wait(big):
    d = ImageDraw.Draw(big)
    cx, cy, r = 10 * CUR_S, 10 * CUR_S, 7.5 * CUR_S
    d.ellipse([cx - r - CUR_S, cy - r - CUR_S, cx + r + CUR_S, cy + r + CUR_S], fill=(20, 24, 32, 255))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(255, 255, 255, 255))
    d.arc([cx - r + 2 * CUR_S, cy - r + 2 * CUR_S, cx + r - 2 * CUR_S, cy + r - 2 * CUR_S], 0, 270, fill=(52, 120, 246, 255), width=int(2.4 * CUR_S))


def c_resize_h(big):
    outlined([(1.5, 10), (6, 5.5), (6, 8.2), (15, 8.2), (15, 5.5), (19.5, 10), (15, 14.5), (15, 11.8), (6, 11.8), (6, 14.5)], big)


def c_resize_v(big):
    outlined([(10, 1.5), (14.5, 6), (11.8, 6), (11.8, 15), (14.5, 15), (10, 19.5), (5.5, 15), (8.2, 15), (8.2, 6), (5.5, 6)], big)


def c_resize2(big):     # nesw
    outlined([(20, 2), (20, 9.4), (17.6, 7), (7, 17.6), (9.4, 20), (2, 20), (2, 12.6), (4.4, 15), (15, 4.4), (12.6, 2)], big)


CURSORS = {"resize_h": (21, 21, c_resize_h), "resize_v": (21, 21, c_resize_v), "resize2": (23, 23, c_resize2), "arrow": (22, 24, c_arrow), "hand": (22, 25, c_hand), "ibeam": (14, 24, c_ibeam), "cross": (21, 21, c_cross),
           "move": (23, 23, c_move), "resize": (23, 23, c_resize), "wait": (22, 22, c_wait)}


def build_cursors():
    d = os.path.join(OUT, "cursors")
    os.makedirs(d, exist_ok=True)
    for n, (w, h, fn) in CURSORS.items():
        cursor_img(w, h, fn).save(os.path.join(d, n + ".png"), optimize=True)


# ================================================================ wallpapers
W_W, W_H = 800, 500


def blob_wall(bg_top, bg_bot, blobs, vignette=0.0, seed=0):
    yy, xx = np.mgrid[0:W_H, 0:W_W].astype(np.float32)
    t = (yy / (W_H - 1))[..., None]
    bt, bb = np.array(bg_top, np.float32), np.array(bg_bot, np.float32)
    img = bt + (bb - bt) * t
    for (cx, cy, rad, col, a) in blobs:
        d = np.sqrt((xx - cx * W_W) ** 2 + (yy - cy * W_H) ** 2) / (rad * W_W)
        m = np.clip(1.0 - d, 0, 1) ** 2 * a
        img = img + (np.array(col, np.float32) - img) * m[..., None]
    if vignette:
        d = np.sqrt(((xx / W_W) - 0.5) ** 2 + ((yy / W_H) - 0.5) ** 2)
        img = img * (1 - vignette * np.clip(d - 0.25, 0, 1) * 1.6)[..., None]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")


def wave_wall(bg_top, bg_bot, layers):
    yy, xx = np.mgrid[0:W_H, 0:W_W].astype(np.float32)
    t = (yy / (W_H - 1))[..., None]
    img = np.array(bg_top, np.float32) + (np.array(bg_bot, np.float32) - np.array(bg_top, np.float32)) * t
    for (base, amp, f1, f2, ph, col, a) in layers:
        edge = base * W_H + amp * W_H * (np.sin(xx / W_W * f1 * 6.283 + ph) * 0.6 + np.sin(xx / W_W * f2 * 6.283 + ph * 2.1) * 0.4)
        m = np.clip((yy - edge) / 3.0 + 0.5, 0, 1) * a
        grad = np.clip((yy - edge) / (W_H * 0.5), 0, 1)[..., None]
        c = np.array(col, np.float32)
        img = img + (c * (1 - 0.35 * grad) - img) * m[..., None]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")


WALLS = {
    "aurora": lambda: blob_wall((18, 22, 52), (12, 14, 34), [(0.2, 0.25, 0.8, (80, 90, 255), 0.85), (0.8, 0.2, 0.7, (170, 80, 255), 0.7),
                                                          (0.55, 0.9, 0.9, (30, 160, 220), 0.55)], 0.5),
    "daybreak": lambda: blob_wall((255, 214, 170), (255, 150, 140), [(0.15, 0.1, 0.8, (255, 238, 200), 0.9), (0.85, 0.9, 0.9, (214, 100, 160), 0.8),
                                                                  (0.6, 0.5, 0.6, (255, 176, 120), 0.5)], 0.0),
    "lagoon": lambda: blob_wall((14, 74, 110), (6, 36, 66), [(0.8, 0.2, 0.8, (40, 190, 200), 0.8), (0.1, 0.9, 0.8, (30, 120, 200), 0.7),
                                                          (0.5, 0.5, 0.5, (20, 140, 170), 0.4)], 0.35),
    "graphite": lambda: blob_wall((58, 62, 74), (24, 26, 34), [(0.7, 0.15, 0.7, (110, 118, 140), 0.55), (0.1, 0.8, 0.7, (60, 70, 100), 0.5)], 0.45),
    "meadow": lambda: wave_wall((184, 226, 255), (214, 240, 220), [(0.58, 0.06, 1.0, 2.3, 0.4, (120, 196, 120), 1.0), (0.7, 0.07, 1.3, 2.9, 2.0, (80, 164, 100), 1.0),
                                                                 (0.84, 0.05, 0.8, 2.1, 4.1, (52, 128, 84), 1.0)]),
    "dusk": lambda: wave_wall((60, 48, 120), (240, 130, 130), [(0.62, 0.05, 0.9, 2.2, 0.8, (96, 60, 138), 1.0), (0.74, 0.06, 1.2, 2.6, 2.5, (62, 40, 108), 1.0),
                                                           (0.88, 0.04, 0.7, 1.9, 5.0, (36, 26, 76), 1.0)]),
    "orchid": lambda: blob_wall((240, 226, 252), (200, 214, 252), [(0.2, 0.2, 0.7, (255, 190, 230), 0.8), (0.85, 0.75, 0.8, (150, 170, 255), 0.7)], 0.0),
}


def build_walls():
    d = os.path.join(OUT, "wallpapers")
    os.makedirs(d, exist_ok=True)
    for n, fn in WALLS.items():
        fn().save(os.path.join(d, n + ".png"), optimize=True)


if __name__ == "__main__":
    build_icons("sheet" in sys.argv)
    build_cursors()
    build_walls()
    print("ok")
