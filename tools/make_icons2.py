"""Icons for the newer apps and games (32x32 pixel art, scaled up by make_art.save_icon).
make_art.build_icons() calls build(module) so the helpers (new, rect, save_icon, colours) are shared.
"""
from PIL import ImageDraw

NAMES = ["calendar", "tasks", "hex", "storage", "convert", "palette", "capture", "weather", "typing", "network",
         "archive", "pong", "invaders", "asteroids", "runner", "connect4", "memory", "wordy", "sudoku",
         "solitaire", "chess", "emoji"]

DARK = (30, 26, 52, 255)
RED = (229, 72, 106, 255)
GREEN = (63, 178, 127, 255)
BLUE = (80, 140, 240, 255)
YEL = (250, 214, 80, 255)
GREY = (190, 180, 210, 255)
SLATE = (74, 68, 104, 255)


def tile(color=DARK, hi=None):
    im = new()
    d = ImageDraw.Draw(im)
    rect(d, 3, 3, 28, 28, color)
    if hi:
        rect(d, 3, 3, 28, 6, hi)
    return im, d


def icon_calendar():
    im = new(); d = ImageDraw.Draw(im)
    rect(d, 4, 5, 27, 28, WHITE)
    rect(d, 4, 5, 27, 11, RED)
    rect(d, 9, 2, 10, 7, GOLD); rect(d, 21, 2, 22, 7, GOLD)
    for r in range(3):
        for c in range(4):
            rect(d, 7 + c * 5, 14 + r * 5, 9 + c * 5, 16 + r * 5, PINK if (r, c) == (1, 2) else VIOLET_LT)
    return im


def icon_tasks():
    im = new(); d = ImageDraw.Draw(im)
    rect(d, 6, 4, 25, 29, CHIP)
    rect(d, 12, 2, 19, 6, GOLD)
    rect(d, 8, 8, 23, 27, WHITE)
    for i in range(3):
        y = 11 + i * 6
        rect(d, 10, y, 12, y + 2, GREEN)
        rect(d, 15, y + 1, 21, y + 1, GREY)
    return im


def icon_hex():
    im, d = tile(DARK, CHIP_HI)
    for r in range(3):
        for c in range(3):
            rect(d, 6 + c * 7, 10 + r * 6, 8 + c * 7, 13 + r * 6, TEAL if (r + c) % 2 == 0 else VIOLET_LT)
    return im


def icon_storage():
    im = new(); d = ImageDraw.Draw(im)
    rect(d, 5, 9, 26, 23, VIOLET_DK)
    d.ellipse([5, 19, 26, 28], fill=VIOLET_DK)
    d.ellipse([5, 4, 26, 14], fill=VIOLET_LT)
    rect(d, 20, 18, 23, 19, GOLD)
    return im


def icon_convert():
    im, d = tile(CHIP)
    d.polygon([(7, 11), (20, 11), (20, 7), (27, 13), (20, 19), (20, 15), (7, 15)], fill=TEAL)
    d.polygon([(25, 20), (12, 20), (12, 16), (5, 22), (12, 28), (12, 24), (25, 24)], fill=PINK)
    return im


def icon_palette():
    im = new(); d = ImageDraw.Draw(im)
    d.ellipse([3, 5, 28, 28], fill=(222, 190, 140, 255))
    for (x, y, c) in ((8, 12, RED), (13, 8, YEL), (19, 9, GREEN), (23, 14, BLUE), (9, 19, PINK)):
        d.ellipse([x - 2, y - 2, x + 2, y + 2], fill=c)
    return im


def icon_capture():
    im, d = tile(DARK)
    for (x, y, dx, dy) in ((5, 5, 1, 1), (26, 5, -1, 1), (5, 26, 1, -1), (26, 26, -1, -1)):
        rect(d, min(x, x + dx * 5), y, max(x, x + dx * 5), y, WHITE)
        rect(d, x, min(y, y + dy * 5), x, max(y, y + dy * 5), WHITE)
    d.ellipse([11, 11, 20, 20], fill=VIOLET)
    d.ellipse([14, 14, 17, 17], fill=WHITE)
    return im


def icon_weather():
    im = new(); d = ImageDraw.Draw(im)
    d.ellipse([15, 3, 27, 15], fill=YEL)
    d.ellipse([3, 14, 14, 25], fill=WHITE)
    d.ellipse([9, 10, 22, 24], fill=WHITE)
    d.ellipse([16, 15, 28, 25], fill=WHITE)
    rect(d, 8, 19, 24, 25, WHITE)
    return im


def icon_typing():
    im, d = tile(CHIP, CHIP_HI)
    for r in range(3):
        for c in range(5 - (r == 2)):
            x = 5 + c * 5 + (r == 1) * 2
            rect(d, x, 10 + r * 5, x + 3, 12 + r * 5, WHITE if r != 2 else GREY)
    rect(d, 9, 24, 21, 25, WHITE)
    return im


def icon_network():
    im, d = tile(DARK)
    pts = [(16, 9), (8, 22), (24, 22)]
    for i in range(3):
        d.line([pts[i], pts[(i + 1) % 3]], fill=VIOLET_LT, width=1)
    for (x, y), c in zip(pts, (TEAL, PINK, GOLD)):
        d.ellipse([x - 3, y - 3, x + 3, y + 3], fill=c)
    return im


def icon_archive():
    im = new(); d = ImageDraw.Draw(im)
    rect(d, 5, 6, 26, 27, (200, 150, 80, 255))
    rect(d, 5, 6, 26, 10, (230, 185, 110, 255))
    for y in range(10, 22, 3):
        rect(d, 15, y, 16, y + 1, WHITE if (y // 3) % 2 else INK)
    rect(d, 14, 22, 17, 25, GOLD)
    return im


def icon_pong():
    im, d = tile(DARK)
    rect(d, 6, 10, 8, 20, VIOLET_LT); rect(d, 23, 12, 25, 22, PINK)
    for y in range(5, 28, 4):
        rect(d, 15, y, 16, y + 1, CHIP_HI)
    rect(d, 17, 15, 19, 17, WHITE)
    return im


def icon_invaders():
    im, d = tile((14, 12, 28, 255))
    rows = ["00100000100", "00010001000", "00111111100", "01101110110", "11111111111", "10111111101", "10100000101", "00011011000"]
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch == "1":
                rect(d, 5 + x * 2, 6 + y * 2, 6 + x * 2, 7 + y * 2, GREEN)
    return im


def icon_asteroids():
    im, d = tile((10, 8, 22, 255))
    d.polygon([(6, 12), (11, 6), (18, 8), (21, 14), (16, 21), (8, 19)], outline=GREY)
    d.polygon([(22, 18), (26, 26), (22, 24), (18, 26)], outline=GREEN)
    d.point([(14, 14), (23, 8)], fill=WHITE)
    return im


def icon_runner():
    im, d = tile((40, 34, 72, 255))
    rect(d, 4, 24, 27, 27, (90, 80, 140, 255))
    rect(d, 7, 12, 14, 19, VIOLET); rect(d, 12, 14, 13, 15, WHITE)
    rect(d, 8, 20, 9, 23, GOLD); rect(d, 12, 20, 13, 22, GOLD)
    rect(d, 21, 17, 24, 24, GREY); rect(d, 19, 18, 20, 19, GOLD); rect(d, 25, 20, 26, 21, GOLD)
    return im


def icon_connect4():
    im = new(); d = ImageDraw.Draw(im)
    rect(d, 3, 4, 28, 27, (47, 77, 176, 255))
    cols = [[0, 0, 0, 0], [0, 0, 1, 0], [0, 2, 1, 2], [0, 1, 2, 1]]
    for r in range(4):
        for c in range(4):
            col = (20, 28, 56, 255) if not cols[r][c] else RED if cols[r][c] == 1 else YEL
            d.ellipse([5 + c * 6, 6 + r * 5, 9 + c * 6, 10 + r * 5], fill=col)
    return im


def icon_memory():
    im = new(); d = ImageDraw.Draw(im)
    rect(d, 3, 4, 14, 15, (69, 56, 122, 255)); rect(d, 17, 4, 28, 15, WHITE)
    rect(d, 3, 18, 14, 29, WHITE); rect(d, 17, 18, 28, 29, (69, 56, 122, 255))
    d.ellipse([20, 7, 25, 12], fill=RED); d.polygon([(5, 27), (8, 20), (12, 27)], fill=GREEN)
    rect(d, 7, 8, 9, 11, VIOLET_LT); rect(d, 21, 22, 23, 25, VIOLET_LT)
    return im


def icon_wordy():
    im, d = tile((28, 26, 44, 255))
    for i, c in enumerate((GREEN, YEL, SLATE, GREEN)):
        rect(d, 5 + i * 6, 8, 9 + i * 6, 12, c)
    for i, c in enumerate((YEL, GREEN, GREEN, SLATE)):
        rect(d, 5 + i * 6, 14, 9 + i * 6, 18, c)
    for i in range(4):
        rect(d, 5 + i * 6, 20, 9 + i * 6, 24, (56, 50, 80, 255))
    return im


def icon_sudoku():
    im = new(); d = ImageDraw.Draw(im)
    rect(d, 3, 3, 28, 28, WHITE)
    for i in (3, 12, 21):
        rect(d, i, 3, i + 1, 28, INK)
        rect(d, 3, i, 28, i + 1, INK)
    rect(d, 28, 3, 28, 28, INK); rect(d, 3, 28, 28, 28, INK)
    for (x, y) in ((6, 6), (15, 6), (24, 15), (6, 24), (15, 15), (24, 24)):
        rect(d, x, y, x + 2, y + 2, VIOLET_DK)
    return im


def icon_solitaire():
    im = new(); d = ImageDraw.Draw(im)
    rect(d, 3, 6, 17, 27, WHITE)
    d.polygon([(10, 16), (6, 11), (14, 11)], fill=INK); rect(d, 9, 16, 11, 20, INK)
    rect(d, 14, 4, 28, 25, WHITE)
    d.ellipse([16, 9, 21, 14], fill=RED); d.ellipse([21, 9, 26, 14], fill=RED); d.polygon([(16, 13), (26, 13), (21, 21)], fill=RED)
    return im


def icon_chess():
    im, d = tile((60, 50, 100, 255))
    d.ellipse([12, 6, 19, 13], fill=WHITE)
    d.polygon([(13, 13), (18, 13), (21, 23), (10, 23)], fill=WHITE)
    rect(d, 8, 23, 23, 26, WHITE)
    rect(d, 15, 3, 16, 7, GOLD); rect(d, 13, 4, 18, 5, GOLD)
    return im


def icon_emoji():
    im = new(); d = ImageDraw.Draw(im)
    d.ellipse([3, 3, 28, 28], fill=YEL)
    rect(d, 10, 11, 12, 15, INK); rect(d, 19, 11, 21, 15, INK)
    d.arc([9, 12, 22, 24], 20, 160, fill=INK, width=2)
    return im


def build(m):
    """m is the make_art module: borrow its helpers and write every icon."""
    for k in ("new", "rect", "save_icon", "INK", "CHIP", "CHIP_HI", "GOLD", "VIOLET", "VIOLET_LT", "VIOLET_DK", "PINK", "TEAL", "WHITE"):
        globals()[k] = getattr(m, k)
    for n in NAMES:
        m.save_icon(globals()["icon_" + n](), n)
