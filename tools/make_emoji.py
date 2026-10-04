"""silicon's emoji: ~170 hand-composed 16x16 pixel sprites.

Writes art/silicon/emoji.png (a sheet, 16 sprites per row, sprite n sits at cell n-1) and
src/kernel/gfx/emoji_data.inc (the code point and name of every sprite, in sheet order).
Deterministic, no fonts involved: every pixel comes from the drawing code below.
"""
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S = 16

# ---------------------------------------------------------------- palette
def C(r, g, b, a=255):
    return (r, g, b, a)


YEL = C(255, 205, 66)
YEL_HI = C(255, 228, 132)
YEL_SH = C(238, 168, 38)
OUT = C(128, 78, 22)
DK = C(66, 38, 12)
WH = C(255, 255, 255)
RED = C(228, 48, 68)
RED_DK = C(150, 20, 40)
PINK = C(255, 150, 176)
ORG = C(255, 150, 40)
GRN = C(70, 190, 90)
GRN_DK = C(30, 120, 50)
BLU = C(70, 150, 245)
BLU_DK = C(30, 80, 170)
LBL = C(150, 210, 255)
PUR = C(160, 100, 235)
PUR_DK = C(100, 50, 160)
BRN = C(150, 96, 52)
BRN_DK = C(96, 56, 28)
BRN_LT = C(210, 160, 100)
GRY = C(170, 170, 184)
GRY_DK = C(100, 100, 116)
GRY_LT = C(220, 220, 230)
BLK = C(40, 36, 52)
GLD = C(250, 200, 50)
GLD_DK = C(190, 130, 20)
CRM = C(255, 238, 200)
SKIN = C(255, 206, 150)
SKIN_DK = C(224, 160, 100)
TRANS = (0, 0, 0, 0)

LEG = {
    '.': TRANS, 'k': DK, 'w': WH, 'r': RED, 'p': PINK, 'y': YEL, 'Y': YEL_HI, 'o': ORG, 'g': GRN, 'G': GRN_DK,
    'b': BLU, 'B': BLU_DK, 'l': LBL, 'u': PUR, 'U': PUR_DK, 'n': BRN, 'N': BRN_DK, 'c': BRN_LT, 'a': GRY,
    'A': GRY_DK, 'L': GRY_LT, 'K': BLK, 'd': GLD, 'D': GLD_DK, 'm': CRM, 's': SKIN, 'S': SKIN_DK, 'R': RED_DK,
    'x': OUT,
}


def new():
    return Image.new("RGBA", (S, S), TRANS)


def pat(im, x0, y0, rows):
    px = im.load()
    for dy, row in enumerate(rows):
        for dx, ch in enumerate(row):
            if ch == ' ' or ch == '.':
                continue
            x, y = x0 + dx, y0 + dy
            if 0 <= x < S and 0 <= y < S:
                px[x, y] = LEG[ch]


def put(im, pts, col):
    px = im.load()
    for x, y in pts:
        if 0 <= x < S and 0 <= y < S:
            px[x, y] = col


def rect(im, x0, y0, x1, y1, col):
    ImageDraw.Draw(im).rectangle([x0, y0, x1, y1], fill=col)


def ell(im, x0, y0, x1, y1, col, outline=None):
    ImageDraw.Draw(im).ellipse([x0, y0, x1, y1], fill=col, outline=outline)


def poly(im, pts, col):
    ImageDraw.Draw(im).polygon(pts, fill=col)


def line(im, pts, col):
    ImageDraw.Draw(im).line(pts, fill=col)


def outline(im, col=OUT):
    """one pixel outline around every opaque pixel (4-neighbourhood)"""
    src = im.load()
    out = im.copy()
    o = out.load()
    for y in range(S):
        for x in range(S):
            if src[x, y][3] > 0:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < S and 0 <= yy < S and src[xx, yy][3] > 128:
                    o[x, y] = col
                    break
    return out


def darken(col, f):
    return C(int(col[0] * f), int(col[1] * f), int(col[2] * f))


def outline_auto(im):
    """outline each object in a darker version of the colour it touches"""
    src = im.load()
    out = im.copy()
    o = out.load()
    for y in range(S):
        for x in range(S):
            if src[x, y][3] > 0:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < S and 0 <= yy < S and src[xx, yy][3] > 128:
                    o[x, y] = darken(src[xx, yy], 0.45)
                    break
    return out


# ---------------------------------------------------------------- faces
def face_base(col=YEL, hi=YEL_HI, sh=YEL_SH, edge=OUT):
    im = new()
    ell(im, 1, 1, 14, 14, col, edge)
    # shading: lighter top-left, darker bottom-right
    put(im, [(3, 3), (4, 2), (5, 2), (2, 4), (2, 5)], hi)
    put(im, [(12, 11), (11, 12), (10, 13), (12, 10), (13, 9), (9, 13)], sh)
    return im


EYES = {
    # each is the LEFT eye in a 4x4 box at (3, 5); mirrored for the right eye unless listed in ASYM
    'dot': ['....', '.kk.', '.kk.', '....'],
    'big': ['.kk.', 'kkkk', 'kkkk', '.kk.'],
    'white': ['.kk.', 'kwwk', 'kkwk', '.kk.'],
    'happy': ['....', '.kk.', 'k..k', '....'],        # ^ ^
    'closed': ['....', '....', 'kkkk', '....'],
    'wink_l': ['....', '....', 'kkkk', '....'],
    'laugh': ['k...', '.kk.', '.kk.', 'k...'],        # > <
    'squint': ['....', 'kkk.', '..kk', 'kkk.'],
    'heart': ['rr.r', 'rrrr', '.rr.', '..r.'],
    'star': ['.dd.', 'dddd', '.dd.', 'd..d'],
    'cross': ['k..k', '.kk.', '.kk.', 'k..k'],
    'sleep': ['....', '....', 'kkkk', '.kk.'],
    'roll': ['wkkw', 'wwww', 'wkkw', '.ww.'],
    'tear': ['.kk.', '.kk.', '.bk.', '.b..'],
    'glass': ['KKKK', 'KKKK', '.KK.', '....'],
    'worry': ['....', '.kk.', '.kk.', 'k...'],
    'side': ['.wk.', 'wwkk', 'wwkk', '.wk.'],
    'hot': ['....', '.kk.', '.kk.', '....'],
    'up': ['.kk.', '.kk.', '....', '....'],
}
MIRROR_SKIP = {'laugh_r'}


def eyes(im, kind, kind_r=None):
    left = EYES[kind]
    right_src = EYES[kind_r or kind]
    right = [row[::-1] if (kind_r is None and kind not in ('wink_l', 'side')) else row for row in right_src]
    if kind == 'laugh':
        right = ['...k', '.kk.', '.kk.', '...k']
    if kind == 'side':
        right = ['.kw.', 'kkww', 'kkww', '.kw.']
    pat(im, 3, 4, left)
    pat(im, 9, 4, right)


MOUTHS = {
    'smile': (4, 9, ['k......k', '.kkkkkk.']),
    'grin': (4, 9, ['kkkkkkkk', 'kwwwwwwk', '.kwwwwk.', '..kkkk..']),
    'open': (4, 9, ['kkkkkkkk', '.krrrrk.', '..krrk..', '...kk...']),
    'flat': (4, 11, ['.kkkkkk.']),
    'frown': (4, 10, ['..kkkk..', '.k....k.', 'k......k']),
    'o': (5, 9, ['.kkkk.', 'k....k', 'k....k', '.kkkk.']),
    'small_o': (6, 10, ['.kk.', 'k..k', '.kk.']),
    'tongue': (4, 9, ['kkkkkkkk', '.kkkkkk.', '..krrk..', '...rr...']),
    'smirk': (5, 10, ['....kk', '.kkk..', 'k.....']),
    'wavy': (4, 11, ['.k.k.k.k']),
    'zig': (4, 10, ['kkkkkkkk', 'kwkwkwkk', 'kkkkkkkk']),
    'big': (4, 9, ['kkkkkkkk', 'kwwwwwwk', 'krrrrrrk', 'krrrrrrk', '.kkkkkk.']),
    'none': (0, 0, []),
    'kiss': (6, 10, ['.kk.', 'kkrk', '.kk.']),
    'drool': (4, 9, ['k......k', '.kkkkkk.', '.....bb.', '.....b..']),
    'pout': (5, 10, ['..kkkk..', '.k....k.']),
    'rainbow': (4, 11, ['.kkkkkk.']),
    'teeth_frown': (4, 10, ['.kkkkkk.', 'kwwwwwwk', '.kkkkkk.']),
    'slight': (5, 10, ['k.....k', '.kkkkk.']),
    'sad_open': (5, 10, ['..kkkk..', '.kbbbbk.', '.k....k.']),
    'cry_open': (4, 10, ['.kkkkkk.', 'kkrrrrkk', 'k......k']),
    'sick': (4, 10, ['kkkkkkkk', '.kggggk.', '..kkkk..']),
}


def mouth(im, kind):
    if kind == 'none':
        return
    x0, y0, rows = MOUTHS[kind]
    pat(im, x0, y0, rows)


def make_face(eyes_kind, mouth_kind, extra=None, base=None, eyes_r=None):
    im = face_base(*(base or ()))
    eyes(im, eyes_kind, eyes_r)
    mouth(im, mouth_kind)
    if extra:
        extra(im)
    return im


def brow(im, left, right):
    put(im, left, DK)
    put(im, right, DK)


def x_sweat(im):
    put(im, [(12, 2), (12, 3), (13, 4)], BLU)
    put(im, [(12, 4), (11, 3)], LBL)


def x_blush(im):
    put(im, [(2, 8), (3, 8), (2, 9), (3, 9), (12, 8), (13, 8), (12, 9), (13, 9)], PINK)


def x_tears(im):
    put(im, [(3, 8), (3, 9), (3, 10), (4, 9), (12, 8), (12, 9), (12, 10), (11, 9)], BLU)


def x_tears_big(im):
    put(im, [(2, 7), (2, 8), (3, 8), (2, 9), (3, 9), (4, 9), (3, 10), (4, 10), (13, 7), (13, 8), (12, 8), (13, 9), (12, 9), (11, 9), (12, 10), (11, 10)], LBL)


def x_halo(im):
    put(im, [(5, 0), (6, 0), (7, 0), (8, 0), (9, 0), (10, 0)], GLD)
    put(im, [(4, 1), (11, 1)], GLD_DK)


def x_zzz(im):
    put(im, [(11, 1), (12, 1), (13, 1), (12, 2), (11, 3), (12, 3), (13, 3)], BLU_DK)


def x_hearts(im):
    put(im, [(1, 3), (2, 2), (3, 3), (2, 4), (13, 3), (14, 2), (15, 3), (14, 4)], RED)


def x_thermo(im):
    put(im, [(10, 11), (11, 12), (12, 13), (13, 14)], GRY_LT)
    put(im, [(9, 11), (10, 12), (11, 13), (12, 14)], RED)


def x_mask(im):
    rect(im, 3, 9, 12, 13, LBL)
    line(im, [(3, 9), (12, 9)], WH)
    put(im, [(2, 10), (13, 10)], WH)
    line(im, [(4, 11), (11, 11)], BLU)


def x_sunglasses_bar(im):
    rect(im, 2, 5, 13, 5, BLK)


def x_fire_hot(im):
    rect(im, 3, 9, 12, 12, RED)
    put(im, [(4, 13), (6, 13), (8, 13), (10, 13)], RED)


def x_ice(im):
    put(im, [(0, 7), (1, 8), (14, 7), (15, 8), (7, 0), (8, 0)], LBL)


def x_steam(im):
    put(im, [(2, 2), (3, 1), (13, 2), (12, 1)], WH)
    put(im, [(2, 3), (13, 3)], GRY_LT)


def x_devil(im):
    put(im, [(2, 0), (2, 1), (3, 1), (13, 0), (13, 1), (12, 1)], PUR_DK)


def x_dollar(im):
    put(im, [(7, 14), (8, 14)], GRN)


def x_sweat_big(im):
    put(im, [(12, 1), (12, 2), (11, 3), (12, 3), (13, 3), (12, 4)], LBL)


def x_vomit(im):
    put(im, [(7, 14), (8, 14), (9, 14)], GRN)


FACES = []


def F(cp, name, ek, mk, extra=None, base=None, er=None):
    FACES.append((cp, name, lambda: make_face(ek, mk, extra, base, er)))


def brows_up(im):
    put(im, [(3, 3), (4, 3), (5, 3), (10, 3), (11, 3), (12, 3)], DK)


def brows_angry(im):
    put(im, [(3, 3), (4, 4), (5, 4), (12, 3), (11, 4), (10, 4)], DK)


def brows_sad(im):
    put(im, [(3, 4), (4, 3), (5, 3), (12, 4), (11, 3), (10, 3)], DK)


def brow_one(im):
    put(im, [(10, 2), (11, 2), (12, 3)], DK)


RED_FACE = (RED, C(255, 120, 130), C(190, 30, 50), C(120, 20, 30))
BLUE_FACE = (C(140, 200, 255), C(210, 235, 255), C(90, 150, 230), C(40, 80, 150))
GREEN_FACE = (C(150, 210, 110), C(200, 240, 160), C(100, 160, 70), C(50, 100, 30))
PURPLE_FACE = (PUR, C(200, 160, 255), PUR_DK, C(60, 30, 100))

F(0x1F600, 'grinning face', 'dot', 'grin')
F(0x1F603, 'grinning face with big eyes', 'big', 'open')
F(0x1F604, 'grinning face with smiling eyes', 'happy', 'open')
F(0x1F601, 'beaming face', 'happy', 'grin')
F(0x1F606, 'grinning squinting face', 'laugh', 'open')
F(0x1F605, 'grinning face with sweat', 'dot', 'open', x_sweat)
F(0x1F602, 'face with tears of joy', 'happy', 'open', x_tears_big)
F(0x1F923, 'rolling on the floor laughing', 'laugh', 'open', x_tears_big)
F(0x1F642, 'slightly smiling face', 'dot', 'slight')
F(0x1F643, 'upside-down face', 'dot', 'frown')
F(0x1F609, 'winking face', 'dot', 'smile', None, None, 'wink_l')
F(0x1F60A, 'smiling face with smiling eyes', 'happy', 'smile', x_blush)
F(0x1F607, 'smiling face with halo', 'dot', 'smile', x_halo)
F(0x1F970, 'smiling face with hearts', 'dot', 'smile', lambda im: (x_blush(im), x_hearts(im)))
F(0x1F60D, 'smiling face with heart-eyes', 'heart', 'open')
F(0x1F929, 'star-struck', 'star', 'open')
F(0x1F618, 'face blowing a kiss', 'dot', 'kiss', x_blush, None, 'wink_l')
F(0x1F617, 'kissing face', 'dot', 'kiss')
F(0x1F60B, 'face savoring food', 'happy', 'tongue')
F(0x1F61B, 'face with tongue', 'dot', 'tongue')
F(0x1F61C, 'winking face with tongue', 'dot', 'tongue', None, None, 'wink_l')
F(0x1F92A, 'zany face', 'big', 'tongue', None, None, 'dot')
F(0x1F911, 'money-mouth face', 'dot', 'open', x_dollar, GREEN_FACE)
F(0x1F914, 'thinking face', 'dot', 'flat', brow_one)
F(0x1F928, 'face with raised eyebrow', 'dot', 'flat', brow_one)
F(0x1F610, 'neutral face', 'dot', 'flat')
F(0x1F611, 'expressionless face', 'closed', 'flat')
F(0x1F636, 'face without mouth', 'dot', 'none')
F(0x1F60F, 'smirking face', 'dot', 'smirk')
F(0x1F612, 'unamused face', 'side', 'flat', brows_sad)
F(0x1F644, 'face with rolling eyes', 'roll', 'flat')
F(0x1F62C, 'grimacing face', 'dot', 'zig')
F(0x1F60C, 'relieved face', 'closed', 'smile')
F(0x1F614, 'pensive face', 'closed', 'frown', brows_sad)
F(0x1F62A, 'sleepy face', 'sleep', 'small_o', x_sweat)
F(0x1F634, 'sleeping face', 'closed', 'small_o', x_zzz)
F(0x1F637, 'face with medical mask', 'dot', 'none', x_mask)
F(0x1F912, 'face with thermometer', 'dot', 'flat', x_thermo)
F(0x1F922, 'nauseated face', 'squint', 'sick', x_vomit, GREEN_FACE)
F(0x1F975, 'hot face', 'worry', 'open', x_steam, RED_FACE)
F(0x1F976, 'cold face', 'dot', 'teeth_frown', x_ice, BLUE_FACE)
F(0x1F974, 'woozy face', 'roll', 'wavy')
F(0x1F635, 'face with crossed-out eyes', 'cross', 'open')
F(0x1F60E, 'smiling face with sunglasses', 'glass', 'smile', x_sunglasses_bar)
F(0x1F913, 'nerd face', 'white', 'smile')
F(0x1F615, 'confused face', 'dot', 'wavy')
F(0x1F61F, 'worried face', 'worry', 'frown', brows_sad)
F(0x1F641, 'slightly frowning face', 'dot', 'frown')
F(0x2639, 'frowning face', 'dot', 'frown')
F(0x1F62E, 'face with open mouth', 'big', 'o')
F(0x1F632, 'astonished face', 'white', 'o', brows_up)
F(0x1F633, 'flushed face', 'white', 'flat', x_blush)
F(0x1F97A, 'pleading face', 'white', 'small_o', brows_sad)
F(0x1F626, 'frowning face with open mouth', 'dot', 'o', brows_sad)
F(0x1F628, 'fearful face', 'white', 'sad_open', x_sweat)
F(0x1F630, 'anxious face with sweat', 'dot', 'sad_open', x_sweat_big)
F(0x1F625, 'sad but relieved face', 'worry', 'frown', x_sweat)
F(0x1F622, 'crying face', 'worry', 'frown', x_tears)
F(0x1F62D, 'loudly crying face', 'closed', 'cry_open', x_tears_big)
F(0x1F631, 'face screaming in fear', 'white', 'big', brows_up, BLUE_FACE)
F(0x1F616, 'confounded face', 'laugh', 'wavy')
F(0x1F623, 'persevering face', 'laugh', 'frown')
F(0x1F61E, 'disappointed face', 'dot', 'frown', brows_sad)
F(0x1F613, 'downcast face with sweat', 'dot', 'frown', x_sweat)
F(0x1F629, 'weary face', 'laugh', 'o')
F(0x1F62B, 'tired face', 'laugh', 'big')
F(0x1F624, 'face with steam from nose', 'dot', 'smirk', brows_angry)
F(0x1F621, 'pouting face', 'dot', 'pout', brows_angry, RED_FACE)
F(0x1F620, 'angry face', 'dot', 'frown', brows_angry, RED_FACE)
F(0x1F92C, 'face with symbols on mouth', 'dot', 'zig', brows_angry, RED_FACE)
F(0x1F608, 'smiling face with horns', 'dot', 'smile', lambda im: (x_devil(im), brows_angry(im)), PURPLE_FACE)
F(0x1F47F, 'angry face with horns', 'dot', 'frown', lambda im: (x_devil(im), brows_angry(im)), PURPLE_FACE)

SPECIAL = []


def S_(cp, name):
    def deco(fn):
        SPECIAL.append((cp, name, fn))
        return fn
    return deco


@S_(0x1F480, 'skull')
def _skull():
    im = new()
    ell(im, 2, 1, 13, 11, GRY_LT)
    rect(im, 4, 10, 11, 14, GRY_LT)
    pat(im, 3, 4, ['.kk..kk.', 'kkkk.kkkk'[:8], '.kk..kk.'])
    put(im, [(7, 8), (8, 8)], DK)
    put(im, [(5, 12), (7, 12), (9, 12), (11, 12), (6, 11), (8, 11), (10, 11)], GRY)
    return outline(im, GRY_DK)


@S_(0x1F4A9, 'pile of poo')
def _poo():
    im = new()
    ell(im, 2, 9, 13, 14, BRN)
    ell(im, 4, 5, 11, 10, BRN)
    ell(im, 6, 2, 9, 6, BRN)
    put(im, [(7, 1), (8, 1)], BRN)
    put(im, [(5, 11), (6, 12), (9, 12), (10, 11)], WH)
    put(im, [(5, 12), (10, 12)], DK)
    pat(im, 4, 11, ['..k..k..'])
    put(im, [(6, 13), (7, 13), (8, 13), (9, 13)], DK)
    return outline(im, BRN_DK)


@S_(0x1F921, 'clown face')
def _clown():
    im = face_base(WH, GRY_LT, GRY_LT, GRY_DK)
    ell(im, 6, 7, 9, 10, RED)
    pat(im, 3, 4, ['.bb.', 'bbbb', 'bbbb', '.bb.'])
    pat(im, 9, 4, ['.bb.', 'bbbb', 'bbbb', '.bb.'])
    put(im, [(4, 12), (5, 12), (6, 12), (7, 12), (8, 12), (9, 12), (10, 12), (11, 12)], RED)
    rect(im, 0, 5, 1, 9, ORG)
    rect(im, 14, 5, 15, 9, ORG)
    return im


@S_(0x1F47B, 'ghost')
def _ghost():
    im = new()
    ell(im, 2, 1, 13, 11, WH)
    rect(im, 2, 6, 13, 14, WH)
    put(im, [(3, 14), (6, 14), (9, 14), (12, 14)], TRANS)
    put(im, [(4, 14), (5, 14), (7, 14), (8, 14), (10, 14), (11, 14)], WH)
    put(im, [(3, 15), (6, 15), (9, 15), (12, 15)], TRANS)
    pat(im, 4, 5, ['.kk..kk.', '.kk..kk.'])
    pat(im, 6, 9, ['.kk.', '.kk.'])
    return outline(im, GRY_DK)


@S_(0x1F47D, 'alien')
def _alien():
    im = new()
    ell(im, 2, 1, 13, 14, GRN)
    poly(im, [(5, 14), (8, 15), (11, 14)], GRN)
    pat(im, 3, 5, ['.kkk..kkk.'[:7], 'kkkkk.kkkkk'[:7]])
    pat(im, 3, 4, ['kk...', 'kkk..', '.kkk.'])
    pat(im, 9, 4, ['..kk', '.kkk', 'kkk.'][::1])
    put(im, [(7, 11), (8, 11)], GRN_DK)
    return outline(im, GRN_DK)


@S_(0x1F916, 'robot')
def _robot():
    im = new()
    rect(im, 2, 4, 13, 13, GRY)
    rect(im, 7, 1, 8, 3, GRY_DK)
    put(im, [(7, 0), (8, 0)], RED)
    rect(im, 3, 6, 6, 8, BLU)
    rect(im, 9, 6, 12, 8, BLU)
    put(im, [(4, 7), (10, 7)], WH)
    rect(im, 4, 11, 11, 12, GRY_DK)
    for x in (5, 7, 9):
        put(im, [(x, 11)], WH)
    rect(im, 0, 7, 1, 10, GRY_DK)
    rect(im, 14, 7, 15, 10, GRY_DK)
    return outline(im, BLK)


@S_(0x1F63A, 'grinning cat')
def _cat():
    im = new()
    ell(im, 2, 3, 13, 14, ORG)
    poly(im, [(2, 7), (2, 0), (7, 3)], ORG)
    poly(im, [(13, 7), (13, 0), (8, 3)], ORG)
    put(im, [(3, 2), (3, 3), (4, 3), (12, 2), (12, 3), (11, 3)], PINK)
    pat(im, 4, 7, ['.kk..kk.', '.kk..kk.'])
    put(im, [(7, 10), (8, 10)], PINK)
    pat(im, 5, 11, ['k.kk.k', '.kkkk.'])
    return outline(im, BRN_DK)


HEARTS = [
    (0x2764, 'red heart', RED, C(255, 130, 140), RED_DK),
    (0x1F9E1, 'orange heart', ORG, C(255, 200, 120), C(180, 90, 10)),
    (0x1F49B, 'yellow heart', C(255, 220, 60), C(255, 245, 160), GLD_DK),
    (0x1F49A, 'green heart', GRN, C(150, 235, 160), GRN_DK),
    (0x1F499, 'blue heart', BLU, C(170, 210, 255), BLU_DK),
    (0x1F49C, 'purple heart', PUR, C(210, 180, 255), PUR_DK),
    (0x1F5A4, 'black heart', C(70, 66, 86), C(120, 116, 140), BLK),
    (0x1F90D, 'white heart', WH, WH, GRY),
]


def heart(col, hi, dk, broken=False):
    im = new()
    ell(im, 1, 2, 8, 9, col)
    ell(im, 7, 2, 14, 9, col)
    poly(im, [(1, 7), (14, 7), (8, 14), (7, 14)], col)
    put(im, [(3, 4), (4, 3), (3, 5)], hi)
    if broken:
        put(im, [(8, 2), (7, 4), (9, 5), (7, 7), (9, 9), (8, 11), (8, 13)], TRANS)
        put(im, [(9, 3), (8, 5), (8, 8), (9, 10)], TRANS)
    return outline(im, dk)


for cp, name, col, hi, dk in HEARTS:
    SPECIAL.append((cp, name, (lambda col=col, hi=hi, dk=dk: heart(col, hi, dk))))
SPECIAL.append((0x1F494, 'broken heart', lambda: heart(RED, C(255, 130, 140), RED_DK, True)))


@S_(0x1F495, 'two hearts')
def _two_hearts():
    im = new()
    for ox, oy, col in ((0, 5, PINK), (6, 0, RED)):
        ell(im, ox, oy, ox + 5, oy + 5, col)
        ell(im, ox + 4, oy, ox + 9, oy + 5, col)
        poly(im, [(ox, oy + 4), (ox + 9, oy + 4), (ox + 5, oy + 9), (ox + 4, oy + 9)], col)
    return outline_auto(im)


@S_(0x1F496, 'sparkling heart')
def _sparkle_heart():
    im = heart(RED, C(255, 130, 140), RED_DK)
    put(im, [(13, 1), (13, 0), (13, 2), (12, 1), (14, 1)], GLD)
    put(im, [(1, 12), (1, 11), (1, 13), (0, 12), (2, 12)], GLD)
    return im


@S_(0x1F44D, 'thumbs up')
def _thumbs_up():
    im = new()
    rect(im, 1, 7, 4, 14, BLU)
    pat(im, 5, 6, ['ss.', 'ssss', 'sssss', 'sssss', 'sssss', 'sssss', 'ssss.'])
    pat(im, 7, 1, ['ss', 'ss', 'ss', 'ss', 'ss'])
    pat(im, 5, 5, ['ss', 'ss'])
    return outline(im, SKIN_DK)


@S_(0x1F44E, 'thumbs down')
def _thumbs_down():
    im = _thumbs_up().transpose(Image.FLIP_TOP_BOTTOM)
    return im


@S_(0x1F44B, 'waving hand')
def _wave():
    im = new()
    pat(im, 3, 3, ['.s.s.s..', '.s.s.s.s', '.sssssss', '.sssssss', '.sssssss', 'ssssssss', 'ssssssss', '.ssssss.', '..ssss..'])
    put(im, [(1, 2), (0, 4), (14, 1), (15, 3)], LBL)
    return outline(im, SKIN_DK)


@S_(0x1F44F, 'clapping hands')
def _clap():
    im = new()
    pat(im, 1, 4, ['..ss.....', '.sssss...', 'sssssss.s', 'sssssssss', '.sssssss.', '..sssss..'])
    pat(im, 7, 3, ['..ss', '.sss', 'ssss'])
    put(im, [(0, 1), (2, 0), (14, 2), (13, 0), (15, 5)], GLD)
    return outline(im, SKIN_DK)


@S_(0x1F64C, 'raising hands')
def _raise():
    im = new()
    pat(im, 1, 1, ['.s....s.', '.s....s.', 'ss....ss', 'sss..sss', '.ssssss.', '..ssss..'][:6])
    rect(im, 3, 9, 12, 14, BLU)
    pat(im, 2, 7, ['ssssssssssss'])
    put(im, [(0, 3), (15, 3), (0, 6), (15, 6)], GLD)
    return outline(im, SKIN_DK)


@S_(0x1F64F, 'folded hands')
def _pray():
    im = new()
    pat(im, 4, 1, ['..ss..', '.ssss.', '.ssss.', 'ssssss', 'ssssss', 'ssssss', '.ssss.', '.ssss.', '.ssss.', '..ss..'])
    line(im, [(7, 3), (7, 11)], SKIN_DK)
    line(im, [(8, 3), (8, 11)], SKIN_DK)
    return outline(im, SKIN_DK)


@S_(0x270C, 'victory hand')
def _victory():
    im = new()
    pat(im, 3, 1, ['.ss..ss.', '.ss..ss.', '.ss..ss.', '.ss.sss.', '.ssssss.', 'ssssssss', 'ssssssss', '.ssssss.', '..ssss..'])
    return outline(im, SKIN_DK)


@S_(0x1F44C, 'OK hand')
def _ok():
    im = new()
    pat(im, 2, 3, ['..ss.ss.', '..ss.ss.', '.ss.sss.', '.sss.ss.', 'ss.s.ss.', 's..s.s..'])
    put(im, [(5, 10), (6, 10), (7, 11), (4, 11)], TRANS)
    pat(im, 3, 8, ['ssssss', 'ssssss', '.ssss.', '..ss..'])
    ell(im, 3, 6, 7, 10, TRANS, SKIN_DK)
    return outline(im, SKIN_DK)


@S_(0x1F4AA, 'flexed biceps')
def _bicep():
    im = new()
    pat(im, 1, 3, ['....sss.', '...ssss.', '.sssssss', 'sssssss.', 'ssssss..', 'ssss....'])
    ell(im, 7, 3, 14, 11, SKIN)
    rect(im, 12, 1, 14, 4, SKIN)
    rect(im, 2, 9, 7, 14, SKIN)
    put(im, [(9, 5), (10, 5), (9, 6)], SKIN_DK)
    return outline(im, SKIN_DK)


@S_(0x1F44A, 'oncoming fist')
def _fist():
    im = new()
    rect(im, 3, 4, 13, 11, SKIN)
    for x in (6, 9, 12):
        put(im, [(x, 5), (x, 6)], SKIN_DK)
    rect(im, 1, 6, 3, 12, SKIN)
    line(im, [(4, 8), (11, 8)], SKIN_DK)
    return outline(im, SKIN_DK)


@S_(0x261D, 'index pointing up')
def _point_up():
    im = new()
    pat(im, 5, 1, ['.ss', '.ss', '.ss', '.ss', 'sss.s', 'sssss'[:5], 'sssss', 'ssss.', '.ss..'][:9])
    pat(im, 4, 5, ['ssssss', 'ssssss', 'ssssss', '.sssss', '.ssss.'])
    return outline(im, SKIN_DK)


@S_(0x1F449, 'backhand index pointing right')
def _point_right():
    return _point_up().transpose(Image.ROTATE_270)


@S_(0x1F448, 'backhand index pointing left')
def _point_left():
    return _point_up().transpose(Image.ROTATE_90)


@S_(0x270B, 'raised hand')
def _raised_hand():
    im = new()
    pat(im, 3, 1, ['.s.s.s.s', '.s.s.s.s', '.s.s.s.s', 'ssssssss', 'ssssssss', 'ssssssss', '.ssssss.', '..ssss..'])
    pat(im, 1, 8, ['ss', '.s'])
    return outline(im, SKIN_DK)


@S_(0x1F91D, 'handshake')
def _handshake():
    im = new()
    rect(im, 0, 5, 3, 11, BLU)
    rect(im, 12, 5, 15, 11, GRN)
    pat(im, 3, 5, ['ssssssss', 'ssssssss', 'ssssssss', '.ssssss.', '..ssss..'])
    line(im, [(6, 7), (10, 7)], SKIN_DK)
    return outline(im, SKIN_DK)


@S_(0x2705, 'check mark button')
def _check():
    im = new()
    rect(im, 1, 1, 14, 14, GRN)
    line(im, [(4, 8), (6, 11), (12, 4)], WH)
    line(im, [(4, 7), (6, 10), (12, 3)], WH)
    return outline(im, GRN_DK)


@S_(0x2714, 'check mark')
def _check2():
    im = new()
    line(im, [(2, 8), (6, 13), (14, 2)], GRN)
    line(im, [(2, 7), (6, 12), (14, 1)], GRN)
    line(im, [(3, 8), (6, 12), (13, 2)], GRN)
    return outline(im, GRN_DK)


@S_(0x274C, 'cross mark')
def _cross():
    im = new()
    for d in range(-1, 2):
        line(im, [(2 + max(d, 0), 2 - min(d, 0)), (13 + min(d, 0), 13 - max(d, 0))], RED)
        line(im, [(13 + min(d, 0), 2 - min(d, 0)), (2 + max(d, 0), 13 - max(d, 0))], RED)
    return outline(im, RED_DK)


@S_(0x2B50, 'star')
def _star():
    im = new()
    poly(im, [(8, 0), (10, 5), (15, 6), (11, 9), (12, 14), (8, 11), (4, 14), (5, 9), (1, 6), (6, 5)], GLD)
    put(im, [(7, 3), (8, 3), (7, 5), (3, 6)], YEL_HI)
    return outline(im, GLD_DK)


@S_(0x1F31F, 'glowing star')
def _glow_star():
    im = _star()
    put(im, [(0, 1), (1, 0), (1, 2), (2, 1), (14, 12), (15, 13), (13, 13), (14, 14)], YEL_HI)
    return im


@S_(0x2728, 'sparkles')
def _sparkles():
    im = new()
    pat(im, 5, 4, ['...d...', '...d...', '..ddd..', 'ddddddd', '..ddd..', '...d...', '...d...'])
    pat(im, 0, 0, ['.d.', 'ddd', '.d.'])
    pat(im, 12, 11, ['.d.', 'ddd', '.d.'])
    return im


@S_(0x1F525, 'fire')
def _fire():
    im = new()
    poly(im, [(8, 0), (11, 5), (13, 9), (12, 13), (8, 15), (4, 13), (3, 9), (5, 6), (6, 8), (7, 4)], ORG)
    poly(im, [(8, 6), (10, 9), (10, 13), (8, 14), (6, 13), (6, 9)], YEL)
    poly(im, [(8, 10), (9, 12), (8, 13), (7, 12)], WH)
    return outline(im, RED_DK)


@S_(0x1F4A5, 'collision')
def _boom():
    im = new()
    poly(im, [(8, 0), (10, 4), (15, 3), (12, 7), (15, 11), (10, 10), (8, 15), (6, 10), (1, 11), (4, 7), (1, 3), (6, 4)], ORG)
    poly(im, [(8, 4), (10, 7), (8, 11), (6, 7)], YEL)
    return outline(im, RED_DK)


@S_(0x1F4AF, 'hundred points')
def _hundred():
    im = new()
    pat(im, 0, 3, ['.r..rrr.rrr.rrr'[:15]] + ['rr.r..rr.r..rr.'[:15]] * 0)
    for i, (x, rows) in enumerate(((0, ['.r', 'rr', '.r', '.r', '.r']), (4, ['rrr', 'r.r', 'r.r', 'r.r', 'rrr']), (9, ['rrr', 'r.r', 'r.r', 'r.r', 'rrr']))):
        pat(im, x + 1, 4, rows)
    line(im, [(1, 11), (14, 11)], RED)
    line(im, [(2, 13), (13, 13)], RED)
    return im


@S_(0x26A1, 'high voltage')
def _bolt():
    im = new()
    poly(im, [(9, 0), (3, 9), (7, 9), (6, 15), (13, 6), (9, 6), (11, 0)], YEL)
    return outline(im, GLD_DK)


@S_(0x2757, 'exclamation mark')
def _excl():
    im = new()
    rect(im, 6, 1, 9, 10, RED)
    rect(im, 6, 12, 9, 14, RED)
    return outline(im, RED_DK)


@S_(0x2753, 'question mark')
def _quest():
    im = new()
    pat(im, 4, 1, ['.rrrr.', 'rr..rr', '....rr', '...rr.', '..rr..', '..rr..'])
    rect(im, 6, 9, 9, 10, TRANS)
    rect(im, 6, 12, 9, 14, RED)
    pat(im, 6, 8, ['rr', 'rr'])
    return outline(im, RED_DK)


@S_(0x26A0, 'warning')
def _warn():
    im = new()
    poly(im, [(8, 1), (15, 14), (1, 14)], YEL)
    rect(im, 7, 5, 8, 9, DK)
    rect(im, 7, 11, 8, 12, DK)
    return outline(im, GLD_DK)


@S_(0x1F6AB, 'prohibited')
def _prohibited():
    im = new()
    ell(im, 1, 1, 14, 14, None, RED)
    ell(im, 2, 2, 13, 13, None, RED)
    line(im, [(3, 12), (12, 3)], RED)
    line(im, [(3, 11), (11, 3)], RED)
    line(im, [(4, 12), (12, 4)], RED)
    return im


@S_(0x1F4A4, 'zzz')
def _zzz():
    im = new()
    pat(im, 2, 8, ['bbbb', '..b.', '.b..', 'bbbb'])
    pat(im, 8, 2, ['bbbbb', '...b.', '..b..', '.b...', 'bbbbb'])
    return im


@S_(0x1F4AC, 'speech balloon')
def _speech():
    im = new()
    rect(im, 1, 2, 14, 10, WH)
    poly(im, [(3, 10), (3, 14), (8, 10)], WH)
    line(im, [(4, 5), (11, 5)], GRY)
    line(im, [(4, 7), (9, 7)], GRY)
    return outline(im, GRY_DK)


@S_(0x1F4AD, 'thought balloon')
def _thought():
    im = new()
    ell(im, 1, 1, 14, 9, WH)
    ell(im, 2, 10, 5, 13, WH)
    put(im, [(1, 14), (2, 14)], WH)
    return outline(im, GRY_DK)


@S_(0x1F389, 'party popper')
def _party():
    im = new()
    poly(im, [(1, 15), (4, 5), (10, 11)], GLD)
    line(im, [(4, 8), (7, 11)], ORG)
    put(im, [(9, 2), (12, 4), (13, 8), (10, 6), (7, 3), (14, 1), (15, 6)], RED)
    put(im, [(12, 2), (8, 5), (14, 10), (11, 8)], BLU)
    put(im, [(10, 1), (13, 6), (5, 2)], GRN)
    return outline(im, BRN_DK)


@S_(0x1F381, 'wrapped gift')
def _gift():
    im = new()
    rect(im, 2, 6, 13, 14, RED)
    rect(im, 1, 4, 14, 7, C(255, 100, 110))
    rect(im, 7, 4, 8, 14, GLD)
    pat(im, 4, 1, ['.dd.dd.', 'd..d..d'][:2])
    put(im, [(5, 2), (4, 3), (6, 3), (10, 2), (9, 3), (11, 3)], GLD)
    return outline(im, RED_DK)


@S_(0x1F3C6, 'trophy')
def _trophy():
    im = new()
    rect(im, 4, 1, 11, 7, GLD)
    poly(im, [(4, 7), (11, 7), (9, 10), (6, 10)], GLD)
    rect(im, 7, 10, 8, 12, GLD)
    rect(im, 4, 12, 11, 14, GLD_DK)
    ell(im, 0, 2, 4, 6, None, GLD_DK)
    ell(im, 11, 2, 15, 6, None, GLD_DK)
    put(im, [(5, 2), (5, 3), (5, 4)], YEL_HI)
    return outline(im, BRN_DK)


@S_(0x1F451, 'crown')
def _crown():
    im = new()
    poly(im, [(1, 12), (1, 4), (5, 8), (8, 2), (11, 8), (15, 4), (15, 12)], GLD)
    rect(im, 1, 12, 14, 14, GLD_DK)
    put(im, [(8, 9), (4, 11), (12, 11)], RED)
    return outline(im, BRN_DK)


@S_(0x1F48E, 'gem stone')
def _gem():
    im = new()
    poly(im, [(3, 2), (12, 2), (15, 6), (8, 15), (1, 6)], LBL)
    poly(im, [(3, 2), (7, 2), (5, 6), (1, 6)], WH)
    poly(im, [(5, 6), (10, 6), (8, 15)], BLU)
    line(im, [(1, 6), (15, 6)], BLU_DK)
    return outline(im, BLU_DK)


@S_(0x1F4A1, 'light bulb')
def _bulb():
    im = new()
    ell(im, 3, 0, 12, 9, YEL)
    poly(im, [(5, 8), (10, 8), (9, 11), (6, 11)], YEL)
    rect(im, 5, 11, 10, 12, GRY)
    rect(im, 6, 13, 9, 14, GRY_DK)
    put(im, [(5, 3), (5, 4), (6, 2)], WH)
    return outline(im, GLD_DK)


@S_(0x1F512, 'locked')
def _lock():
    im = new()
    ell(im, 4, 0, 11, 9, None, GRY_DK)
    ell(im, 5, 1, 10, 9, None, GRY_DK)
    rect(im, 2, 7, 13, 14, GLD)
    put(im, [(7, 10), (8, 10), (7, 11), (8, 11), (7, 12)], DK)
    return outline(im, GLD_DK)


@S_(0x1F511, 'key')
def _key():
    im = new()
    ell(im, 1, 1, 7, 7, GLD)
    ell(im, 3, 3, 5, 5, TRANS)
    rect(im, 6, 6, 8, 8, GLD)
    line(im, [(7, 7), (14, 14)], GLD)
    line(im, [(8, 7), (15, 14)], GLD)
    put(im, [(11, 12), (12, 13), (13, 15), (14, 14)], GLD)
    return outline(im, GLD_DK)


@S_(0x1F514, 'bell')
def _bell():
    im = new()
    ell(im, 3, 1, 12, 11, GLD)
    rect(im, 3, 6, 12, 11, GLD)
    rect(im, 1, 11, 14, 12, GLD)
    ell(im, 6, 12, 9, 15, GLD_DK)
    put(im, [(5, 3), (5, 4)], YEL_HI)
    return outline(im, BRN_DK)


@S_(0x1F4E7, 'e-mail')
def _mail():
    im = new()
    rect(im, 1, 3, 14, 12, WH)
    line(im, [(1, 3), (8, 8), (14, 3)], GRY_DK)
    return outline(im, GRY_DK)


@S_(0x1F4C1, 'file folder')
def _folder():
    im = new()
    rect(im, 1, 3, 6, 5, GLD)
    rect(im, 1, 5, 14, 13, YEL)
    return outline(im, GLD_DK)


@S_(0x1F4C4, 'page facing up')
def _page():
    im = new()
    rect(im, 3, 1, 12, 14, WH)
    for y in (4, 6, 8, 10, 12):
        line(im, [(5, y), (10, y)], GRY)
    return outline(im, GRY_DK)


@S_(0x1F4F7, 'camera')
def _camera():
    im = new()
    rect(im, 1, 4, 14, 13, GRY)
    rect(im, 4, 2, 7, 4, GRY_DK)
    ell(im, 5, 5, 11, 11, BLK)
    ell(im, 6, 6, 10, 10, LBL)
    put(im, [(7, 7), (8, 7)], WH)
    put(im, [(12, 6)], RED)
    return outline(im, BLK)


@S_(0x1F4BB, 'laptop')
def _laptop():
    im = new()
    rect(im, 2, 2, 13, 10, GRY_DK)
    rect(im, 3, 3, 12, 9, BLU)
    line(im, [(4, 5), (9, 5)], WH)
    rect(im, 0, 11, 15, 13, GRY)
    return outline(im, BLK)


@S_(0x1F4F1, 'mobile phone')
def _phone():
    im = new()
    rect(im, 4, 0, 11, 15, BLK)
    rect(im, 5, 2, 10, 12, LBL)
    put(im, [(7, 14), (8, 14)], GRY)
    return outline(im, GRY_DK)


@S_(0x23F0, 'alarm clock')
def _alarm():
    im = new()
    ell(im, 2, 2, 13, 13, WH)
    line(im, [(7, 7), (7, 4)], DK)
    line(im, [(7, 7), (10, 8)], DK)
    ell(im, 0, 0, 4, 4, RED)
    ell(im, 11, 0, 15, 4, RED)
    return outline(im, GRY_DK)


@S_(0x231A, 'watch')
def _watch():
    im = new()
    rect(im, 5, 0, 10, 15, GRY_DK)
    ell(im, 2, 3, 13, 12, WH)
    line(im, [(7, 7), (7, 5)], DK)
    line(im, [(7, 7), (10, 8)], DK)
    return outline(im, BLK)


@S_(0x1F4A3, 'bomb')
def _bomb():
    im = new()
    ell(im, 1, 5, 12, 15, BLK)
    put(im, [(4, 8), (3, 9), (4, 9)], GRY)
    line(im, [(10, 6), (12, 3), (14, 3)], BRN)
    put(im, [(14, 1), (15, 2), (13, 2), (15, 4)], ORG)
    put(im, [(14, 2)], YEL)
    return outline(im, GRY_DK)


@S_(0x1F3AE, 'video game')
def _gamepad():
    im = new()
    ell(im, 0, 4, 15, 14, GRY)
    rect(im, 4, 6, 5, 11, BLK)
    rect(im, 2, 8, 7, 9, BLK)
    put(im, [(10, 7), (11, 7), (10, 8), (11, 8)], RED)
    put(im, [(12, 9), (13, 9), (12, 10), (13, 10)], BLU)
    return outline(im, GRY_DK)


@S_(0x1F3B2, 'game die')
def _die():
    im = new()
    rect(im, 1, 1, 14, 14, WH)
    for x, y in ((4, 4), (11, 4), (7, 7), (4, 10), (11, 10)):
        rect(im, x, y, x + 1, y + 1, DK)
    return outline(im, GRY_DK)


@S_(0x1F3B5, 'musical note')
def _note():
    im = new()
    rect(im, 5, 2, 6, 11, BLU_DK)
    rect(im, 5, 2, 13, 4, BLU_DK)
    rect(im, 12, 2, 13, 9, BLU_DK)
    ell(im, 2, 10, 6, 14, BLU_DK)
    ell(im, 9, 8, 13, 12, BLU_DK)
    return outline(im, BLK)


@S_(0x1F3A7, 'headphone')
def _headphone():
    im = new()
    ell(im, 2, 1, 13, 12, None, GRY_DK)
    ell(im, 3, 2, 12, 12, None, GRY_DK)
    ell(im, 4, 3, 11, 12, None, GRY_DK)
    rect(im, 1, 8, 4, 14, RED)
    rect(im, 11, 8, 14, 14, RED)
    return outline(im, BLK)


@S_(0x1F3A4, 'microphone')
def _mic():
    im = new()
    ell(im, 5, 0, 10, 8, GRY)
    rect(im, 5, 4, 10, 8, GRY)
    line(im, [(6, 1), (6, 6)], WH)
    ell(im, 3, 4, 12, 12, None, GRY_DK)
    rect(im, 7, 12, 8, 14, GRY_DK)
    rect(im, 5, 14, 10, 15, GRY_DK)
    return outline(im, BLK)


@S_(0x1F4DA, 'books')
def _books():
    im = new()
    rect(im, 1, 9, 14, 14, BLU)
    rect(im, 2, 5, 13, 9, RED)
    rect(im, 3, 1, 12, 5, GRN)
    for y, c in ((11, WH), (7, WH), (3, WH)):
        line(im, [(3, y), (12, y)], c)
    return outline(im, BLK)


@S_(0x270F, 'pencil')
def _pencil():
    im = new()
    poly(im, [(11, 0), (15, 4), (5, 14), (1, 15), (2, 11)], YEL)
    poly(im, [(11, 0), (15, 4), (13, 6), (9, 2)], PINK)
    poly(im, [(1, 15), (2, 11), (5, 14)], SKIN)
    put(im, [(1, 15)], DK)
    return outline(im, GLD_DK)


@S_(0x1F4DD, 'memo')
def _memo():
    im = new()
    rect(im, 2, 1, 12, 14, WH)
    for y in (4, 6, 8, 10):
        line(im, [(4, y), (10, y)], BLU)
    poly(im, [(10, 13), (15, 5), (14, 3), (9, 11)], YEL)
    return outline(im, GRY_DK)


@S_(0x1F50D, 'magnifying glass')
def _magnifier():
    im = new()
    ell(im, 1, 1, 10, 10, LBL)
    ell(im, 1, 1, 10, 10, None, GRY_DK)
    ell(im, 2, 2, 9, 9, None, GRY_DK)
    line(im, [(9, 9), (14, 14)], BRN)
    line(im, [(10, 9), (15, 14)], BRN)
    put(im, [(3, 3), (4, 3), (3, 4)], WH)
    return outline(im, BLK)


@S_(0x1F680, 'rocket')
def _rocket():
    im = new()
    poly(im, [(13, 1), (14, 2), (9, 11), (5, 7)], GRY_LT)
    poly(im, [(13, 1), (14, 2), (12, 4), (11, 3)], RED)
    ell(im, 8, 4, 10, 6, LBL)
    poly(im, [(4, 8), (1, 11), (5, 11)], RED)
    poly(im, [(8, 12), (11, 14), (7, 15), (5, 11)], RED)
    poly(im, [(3, 12), (1, 15), (4, 14)], ORG)
    return outline(im, GRY_DK)


@S_(0x1F697, 'automobile')
def _car():
    im = new()
    poly(im, [(2, 8), (4, 3), (11, 3), (14, 8)], RED)
    rect(im, 0, 8, 15, 12, RED)
    poly(im, [(4, 4), (7, 4), (7, 7), (3, 7)], LBL)
    poly(im, [(8, 4), (11, 4), (13, 7), (8, 7)], LBL)
    ell(im, 2, 10, 6, 14, BLK)
    ell(im, 9, 10, 13, 14, BLK)
    put(im, [(4, 12), (11, 12)], GRY)
    return outline(im, RED_DK)


@S_(0x2708, 'airplane')
def _plane():
    im = new()
    poly(im, [(14, 1), (15, 2), (11, 6), (12, 11), (10, 12), (8, 8), (4, 12), (3, 11), (6, 7), (2, 5), (3, 4), (8, 5)], WH)
    return outline(im, GRY_DK)


@S_(0x1F3E0, 'house')
def _house():
    im = new()
    poly(im, [(0, 8), (8, 1), (15, 8)], RED)
    rect(im, 2, 8, 13, 14, CRM)
    rect(im, 6, 10, 9, 14, BRN)
    rect(im, 3, 9, 5, 11, LBL)
    rect(im, 10, 9, 12, 11, LBL)
    return outline(im, BRN_DK)


@S_(0x26BD, 'soccer ball')
def _soccer():
    im = new()
    ell(im, 1, 1, 14, 14, WH)
    poly(im, [(8, 5), (11, 7), (10, 10), (6, 10), (5, 7)], DK)
    put(im, [(8, 2), (8, 3), (3, 4), (12, 4), (3, 11), (12, 11), (8, 13)], DK)
    return outline(im, GRY_DK)


@S_(0x1F3C0, 'basketball')
def _basket():
    im = new()
    ell(im, 1, 1, 14, 14, ORG)
    line(im, [(1, 8), (14, 8)], DK)
    line(im, [(8, 1), (8, 14)], DK)
    ell(im, -3, 3, 5, 12, None, DK)
    ell(im, 10, 3, 18, 12, None, DK)
    return outline(im, BRN_DK)


@S_(0x1F3B8, 'guitar')
def _guitar():
    im = new()
    ell(im, 1, 7, 8, 14, ORG)
    ell(im, 4, 5, 10, 11, ORG)
    ell(im, 4, 8, 6, 10, DK)
    line(im, [(8, 8), (14, 2)], BRN)
    line(im, [(9, 8), (15, 2)], BRN)
    rect(im, 13, 0, 15, 2, BRN_DK)
    return outline(im, BRN_DK)


@S_(0x2600, 'sun')
def _sun():
    im = new()
    ell(im, 4, 4, 11, 11, YEL)
    for a, b in (((7, 0), (8, 2)), ((7, 13), (8, 15)), ((0, 7), (2, 8)), ((13, 7), (15, 8))):
        rect(im, a[0], a[1], b[0], b[1], GLD)
    put(im, [(2, 2), (3, 3), (13, 2), (12, 3), (2, 13), (3, 12), (13, 13), (12, 12)], GLD)
    put(im, [(5, 5), (6, 5), (5, 6)], YEL_HI)
    return im


@S_(0x1F319, 'crescent moon')
def _moon():
    im = new()
    ell(im, 2, 1, 14, 14, YEL)
    ell(im, 6, 0, 17, 13, TRANS)
    return outline(im, GLD_DK)


@S_(0x2601, 'cloud')
def _cloud():
    im = new()
    ell(im, 1, 6, 7, 13, WH)
    ell(im, 4, 2, 11, 10, WH)
    ell(im, 8, 5, 15, 13, WH)
    rect(im, 4, 9, 11, 13, WH)
    return outline(im, GRY)


@S_(0x1F327, 'cloud with rain')
def _rain():
    im = _cloud().crop((0, 0, 16, 16))
    im = Image.alpha_composite(new(), im)
    px = im.load()
    for x in (3, 7, 11):
        for y in (13, 14):
            px[x, y] = BLU
    return im


@S_(0x2744, 'snowflake')
def _snow():
    im = new()
    line(im, [(7, 0), (7, 15)], LBL)
    line(im, [(8, 0), (8, 15)], LBL)
    line(im, [(0, 7), (15, 7)], LBL)
    line(im, [(0, 8), (15, 8)], LBL)
    line(im, [(2, 2), (13, 13)], LBL)
    line(im, [(13, 2), (2, 13)], LBL)
    return im


@S_(0x1F308, 'rainbow')
def _rainbow():
    im = new()
    for i, col in enumerate((RED, ORG, YEL, GRN, BLU, PUR)):
        r = 7 - i
        ImageDraw.Draw(im).arc([8 - r - 1, 14 - r - 1 + 0, 8 + r, 14 + r + 1], 180, 360, fill=col, width=1)
    for i, col in enumerate((RED, ORG, YEL, GRN, BLU, PUR)):
        r = 7 - i
        ImageDraw.Draw(im).arc([7 - r, 13 - r, 8 + r, 13 + r], 180, 360, fill=col, width=1)
    return im


@S_(0x1F30D, 'globe')
def _globe():
    im = new()
    ell(im, 1, 1, 14, 14, BLU)
    pat(im, 3, 3, ['.gg..', 'ggg..', '.gg..'])
    pat(im, 7, 7, ['..ggg', '.gggg', '..gg.', '...g.'])
    put(im, [(4, 3), (5, 3)], LBL)
    return outline(im, BLU_DK)


@S_(0x1F332, 'evergreen tree')
def _pine():
    im = new()
    poly(im, [(8, 0), (13, 6), (3, 6)], GRN)
    poly(im, [(8, 3), (14, 10), (2, 10)], GRN)
    poly(im, [(8, 6), (15, 13), (1, 13)], GRN_DK)
    rect(im, 7, 13, 8, 15, BRN)
    return outline(im, GRN_DK)


@S_(0x1F333, 'deciduous tree')
def _tree():
    im = new()
    ell(im, 1, 0, 14, 10, GRN)
    ell(im, 3, 4, 13, 12, GRN)
    rect(im, 7, 10, 8, 15, BRN)
    put(im, [(4, 3), (5, 3), (4, 4)], C(150, 235, 160))
    return outline(im, GRN_DK)


@S_(0x1F337, 'tulip')
def _tulip():
    im = new()
    poly(im, [(4, 2), (4, 8), (8, 9), (12, 8), (12, 2), (10, 5), (8, 2), (6, 5)], RED)
    rect(im, 7, 9, 8, 15, GRN)
    poly(im, [(8, 12), (13, 9), (11, 13)], GRN)
    return outline(im, GRN_DK)


@S_(0x1F339, 'rose')
def _rose():
    im = new()
    ell(im, 3, 0, 12, 9, RED)
    pat(im, 5, 2, ['.rrrr.', 'rR..Rr', 'rRrrRr', '.rRRr.'])
    rect(im, 7, 9, 8, 15, GRN)
    poly(im, [(8, 12), (14, 9), (11, 14)], GRN)
    return outline(im, RED_DK)


@S_(0x1F33B, 'sunflower')
def _sunflower():
    im = new()
    for ang in range(0, 360, 45):
        import math
        cx, cy = 8 + 5 * math.cos(math.radians(ang)), 6 + 5 * math.sin(math.radians(ang))
        ell(im, int(cx - 2), int(cy - 2), int(cx + 2), int(cy + 2), YEL)
    ell(im, 5, 3, 10, 9, BRN)
    rect(im, 7, 10, 8, 15, GRN)
    return outline(im, GLD_DK)


@S_(0x1F340, 'four leaf clover')
def _clover():
    im = new()
    for cx, cy in ((5, 5), (10, 5), (5, 10), (10, 10)):
        ell(im, cx - 3, cy - 3, cx + 2, cy + 2, GRN)
    rect(im, 6, 6, 9, 9, GRN)
    line(im, [(8, 9), (12, 15)], GRN_DK)
    return outline(im, GRN_DK)


@S_(0x1F331, 'seedling')
def _seedling():
    im = new()
    rect(im, 7, 7, 8, 14, GRN_DK)
    poly(im, [(7, 8), (1, 3), (6, 2), (8, 6)], GRN)
    poly(im, [(8, 7), (14, 2), (15, 7), (9, 10)], GRN)
    rect(im, 4, 14, 11, 15, BRN)
    return outline(im, GRN_DK)


def animal(base, ears, belly=None, nose=PINK, snout=None, eyes_dk=DK):
    im = new()
    ears(im, base)
    ell(im, 2, 3, 13, 14, base)
    if snout:
        ell(im, 4, 8, 11, 14, snout)
    pat(im, 4, 6, ['.kk..kk.', '.kk..kk.'][:2])
    put(im, [(7, 9), (8, 9), (7, 10), (8, 10)], nose if nose != PINK else DK)
    return im


def ears_round(im, base):
    ell(im, 1, 0, 5, 4, base)
    ell(im, 10, 0, 14, 4, base)


def ears_point(im, base):
    poly(im, [(2, 7), (2, 0), (7, 3)], base)
    poly(im, [(13, 7), (13, 0), (8, 3)], base)


def ears_long(im, base):
    ell(im, 3, 0, 6, 8, base)
    ell(im, 9, 0, 12, 8, base)
    ell(im, 4, 1, 5, 6, PINK)
    ell(im, 10, 1, 11, 6, PINK)


def ears_floppy(im, base):
    ell(im, 0, 3, 4, 11, BRN_DK)
    ell(im, 11, 3, 15, 11, BRN_DK)


ANIMALS = [
    (0x1F436, 'dog face', BRN_LT, ears_floppy, CRM),
    (0x1F431, 'cat face', ORG, ears_point, CRM),
    (0x1F42D, 'mouse face', GRY, ears_round, None),
    (0x1F439, 'hamster', BRN_LT, ears_round, CRM),
    (0x1F430, 'rabbit face', WH, ears_long, None),
    (0x1F98A, 'fox', ORG, ears_point, WH),
    (0x1F43B, 'bear', BRN, ears_round, BRN_LT),
    (0x1F43C, 'panda', WH, ears_round, None),
    (0x1F428, 'koala', GRY, ears_round, None),
    (0x1F42F, 'tiger face', ORG, ears_round, CRM),
    (0x1F981, 'lion', C(240, 170, 60), ears_round, CRM),
    (0x1F42E, 'cow face', WH, ears_point, PINK),
    (0x1F437, 'pig face', PINK, ears_point, C(255, 120, 150)),
    (0x1F438, 'frog', GRN, ears_round, None),
    (0x1F435, 'monkey face', BRN, ears_round, BRN_LT),
]


def make_animal(base, ears, snout):
    im = new()
    ears(im, base)
    ell(im, 2, 3, 13, 14, base)
    if snout:
        ell(im, 4, 8, 11, 14, snout)
    pat(im, 4, 6, ['.kk..kk.', '.kk..kk.'])
    put(im, [(7, 9), (8, 9)], DK)
    put(im, [(6, 12), (9, 12), (7, 13), (8, 13)], DK)
    return outline_auto(im)


for cp, name, base, ears, snout in ANIMALS:
    SPECIAL.append((cp, name, (lambda b=base, e=ears, s=snout: make_animal(b, e, s))))


def panda():
    im = new()
    ell(im, 1, 0, 5, 4, BLK)
    ell(im, 10, 0, 14, 4, BLK)
    ell(im, 2, 3, 13, 14, WH)
    ell(im, 3, 5, 6, 9, BLK)
    ell(im, 9, 5, 12, 9, BLK)
    put(im, [(4, 7), (10, 7)], WH)
    put(im, [(7, 10), (8, 10), (7, 11)], DK)
    return outline(im, GRY_DK)


for i, (cp, name, fn) in enumerate(SPECIAL):
    if cp == 0x1F43C:
        SPECIAL[i] = (cp, name, panda)


@S_(0x1F414, 'chicken')
def _chicken():
    im = new()
    ell(im, 2, 3, 13, 14, WH)
    poly(im, [(6, 0), (8, 3), (4, 3)], RED)
    put(im, [(7, 0), (8, 1)], RED)
    poly(im, [(8, 8), (13, 9), (8, 11)], ORG)
    put(im, [(5, 6), (6, 6), (5, 7), (6, 7)], DK)
    put(im, [(8, 12), (9, 12)], RED)
    return outline(im, GRY_DK)


@S_(0x1F427, 'penguin')
def _penguin():
    im = new()
    ell(im, 3, 0, 12, 14, BLK)
    ell(im, 5, 5, 10, 14, WH)
    put(im, [(5, 3), (6, 3), (9, 3), (10, 3)], WH)
    put(im, [(6, 3), (9, 3)], DK)
    poly(im, [(7, 5), (9, 5), (8, 7)], ORG)
    rect(im, 4, 14, 6, 15, ORG)
    rect(im, 9, 14, 11, 15, ORG)
    return outline(im, BLK)


@S_(0x1F426, 'bird')
def _bird():
    im = new()
    ell(im, 2, 4, 12, 13, BLU)
    ell(im, 7, 1, 13, 7, BLU)
    poly(im, [(13, 4), (15, 5), (13, 6)], ORG)
    put(im, [(11, 3)], DK)
    ell(im, 3, 7, 8, 11, LBL)
    poly(im, [(2, 8), (0, 6), (2, 12)], BLU_DK)
    return outline(im, BLU_DK)


@S_(0x1F989, 'owl')
def _owl():
    im = new()
    ell(im, 2, 1, 13, 14, BRN)
    poly(im, [(2, 3), (3, 0), (6, 2)], BRN)
    poly(im, [(13, 3), (12, 0), (9, 2)], BRN)
    ell(im, 3, 4, 7, 8, WH)
    ell(im, 8, 4, 12, 8, WH)
    put(im, [(5, 6), (10, 6), (5, 5), (10, 5)], DK)
    poly(im, [(7, 8), (9, 8), (8, 10)], ORG)
    ell(im, 5, 10, 10, 14, BRN_LT)
    return outline(im, BRN_DK)


@S_(0x1F984, 'unicorn')
def _unicorn():
    im = new()
    ell(im, 2, 4, 12, 14, WH)
    poly(im, [(5, 4), (7, 0), (9, 4)], GLD)
    poly(im, [(3, 6), (2, 3), (5, 5)], PINK)
    pat(im, 4, 8, ['.kk.', '.kk.'])
    ell(im, 9, 9, 14, 14, PINK)
    put(im, [(0, 5), (0, 6), (1, 7)], PUR)
    return outline(im, GRY_DK)


@S_(0x1F41D, 'honeybee')
def _bee():
    im = new()
    ell(im, 3, 5, 13, 12, YEL)
    for x in (6, 9):
        line(im, [(x, 5), (x, 12)], DK)
    ell(im, 4, 0, 8, 5, LBL)
    ell(im, 8, 0, 12, 5, LBL)
    put(im, [(12, 8)], DK)
    return outline(im, DK)


@S_(0x1F98B, 'butterfly')
def _butterfly():
    im = new()
    ell(im, 0, 1, 7, 8, BLU)
    ell(im, 8, 1, 15, 8, BLU)
    ell(im, 1, 8, 7, 14, PUR)
    ell(im, 8, 8, 14, 14, PUR)
    rect(im, 7, 3, 8, 13, DK)
    put(im, [(3, 3), (12, 3), (4, 10), (11, 10)], WH)
    return outline(im, BLU_DK)


@S_(0x1F40C, 'snail')
def _snail():
    im = new()
    ell(im, 4, 2, 13, 11, BRN)
    ell(im, 6, 4, 11, 9, BRN_LT)
    rect(im, 0, 11, 14, 13, SKIN)
    line(im, [(1, 11), (1, 7)], SKIN)
    line(im, [(3, 11), (4, 7)], SKIN)
    return outline(im, BRN_DK)


@S_(0x1F40D, 'snake')
def _snake():
    im = new()
    line(im, [(2, 13), (6, 12), (10, 13), (13, 10), (9, 8), (5, 7), (8, 4), (12, 3)], GRN)
    line(im, [(2, 12), (6, 11), (10, 12), (12, 10), (9, 9), (5, 8), (8, 5), (12, 4)], GRN)
    put(im, [(13, 2), (13, 3), (14, 3)], GRN)
    put(im, [(13, 2)], DK)
    put(im, [(14, 4), (15, 5)], RED)
    return outline(im, GRN_DK)


@S_(0x1F422, 'turtle')
def _turtle():
    im = new()
    ell(im, 2, 3, 12, 12, GRN)
    ell(im, 3, 4, 11, 10, GRN_DK)
    ell(im, 11, 6, 15, 10, GRN)
    rect(im, 3, 11, 5, 14, GRN)
    rect(im, 9, 11, 11, 14, GRN)
    put(im, [(14, 7)], DK)
    return outline(im, GRN_DK)


@S_(0x1F419, 'octopus')
def _octopus():
    im = new()
    ell(im, 2, 0, 13, 9, PUR)
    for x in (2, 5, 8, 11):
        rect(im, x, 8, x + 2, 13, PUR)
    put(im, [(3, 14), (6, 14), (9, 14), (12, 14)], PUR)
    pat(im, 4, 3, ['.kk..kk.', '.kk..kk.'])
    return outline(im, PUR_DK)


@S_(0x1F420, 'tropical fish')
def _fish():
    im = new()
    ell(im, 1, 4, 11, 12, ORG)
    poly(im, [(10, 8), (15, 3), (15, 13)], ORG)
    put(im, [(3, 7), (3, 8)], DK)
    line(im, [(6, 5), (6, 11)], WH)
    return outline(im, BRN_DK)


@S_(0x1F42C, 'dolphin')
def _dolphin():
    im = new()
    ell(im, 0, 3, 13, 13, BLU)
    poly(im, [(8, 3), (12, 0), (11, 5)], BLU)
    poly(im, [(12, 9), (15, 5), (15, 12)], BLU)
    ell(im, 1, 8, 9, 13, LBL)
    put(im, [(4, 6)], DK)
    return outline(im, BLU_DK)


@S_(0x1F433, 'spouting whale')
def _whale():
    im = new()
    ell(im, 0, 6, 13, 14, BLU)
    poly(im, [(12, 9), (15, 5), (15, 12)], BLU)
    put(im, [(3, 9)], DK)
    line(im, [(5, 5), (5, 2)], LBL)
    put(im, [(4, 1), (6, 1)], LBL)
    return outline(im, BLU_DK)


FOODS = []


def fruit(col, hi, dk, stem=True, leaf=GRN):
    im = new()
    ell(im, 1, 3, 14, 15, col)
    put(im, [(4, 6), (4, 7), (5, 5)], hi)
    if stem:
        rect(im, 7, 0, 8, 3, BRN)
        poly(im, [(8, 2), (13, 0), (11, 3)], leaf)
    return outline(im, dk)


@S_(0x1F34E, 'red apple')
def _apple():
    return fruit(RED, C(255, 150, 150), RED_DK)


@S_(0x1F34A, 'tangerine')
def _orange():
    return fruit(ORG, C(255, 210, 140), C(160, 80, 10))


@S_(0x1F34B, 'lemon')
def _lemon():
    im = new()
    ell(im, 0, 3, 15, 13, YEL)
    put(im, [(0, 7), (0, 8), (15, 7), (15, 8)], YEL)
    put(im, [(4, 5), (5, 5), (4, 6)], YEL_HI)
    return outline(im, GLD_DK)


@S_(0x1F34C, 'banana')
def _banana():
    im = new()
    poly(im, [(2, 3), (5, 2), (7, 11), (13, 10), (14, 12), (6, 15), (1, 8)], YEL)
    put(im, [(2, 2), (3, 1)], BRN)
    return outline(im, GLD_DK)


@S_(0x1F349, 'watermelon')
def _melon():
    im = new()
    ell(im, 0, -9, 15, 14, GRN)
    ell(im, 1, -8, 14, 12, RED)
    ell(im, 0, -9, 15, 14, None, GRN_DK)
    ell(im, 1, -8, 14, 13, None, WH)
    put(im, [(4, 6), (8, 8), (11, 5), (6, 10), (10, 10)], DK)
    return outline(im, GRN_DK)


@S_(0x1F347, 'grapes')
def _grapes():
    im = new()
    for x, y in ((4, 4), (8, 4), (6, 7), (10, 7), (4, 8), (8, 10), (6, 12)):
        ell(im, x - 2, y - 2, x + 2, y + 2, PUR)
        put(im, [(x - 1, y - 1)], C(210, 180, 255))
    rect(im, 7, 0, 8, 2, BRN)
    poly(im, [(9, 1), (13, 0), (11, 3)], GRN)
    return outline(im, PUR_DK)


@S_(0x1F353, 'strawberry')
def _strawberry():
    im = new()
    poly(im, [(1, 4), (14, 4), (12, 11), (8, 15), (3, 11)], RED)
    poly(im, [(2, 4), (5, 1), (8, 3), (11, 1), (13, 4), (8, 5)], GRN)
    put(im, [(4, 7), (7, 8), (10, 7), (6, 11), (9, 11), (8, 13)], YEL)
    return outline(im, RED_DK)


@S_(0x1F352, 'cherries')
def _cherries():
    im = new()
    ell(im, 0, 8, 6, 14, RED)
    ell(im, 8, 9, 14, 15, RED)
    line(im, [(3, 8), (8, 1), (11, 9)], GRN_DK)
    poly(im, [(8, 1), (14, 1), (11, 4)], GRN)
    put(im, [(2, 10), (10, 11)], WH)
    return outline(im, RED_DK)


@S_(0x1F351, 'peach')
def _peach():
    im = fruit(C(255, 170, 140), WH, C(190, 90, 60))
    line(im, [(8, 5), (7, 10), (8, 14)], C(220, 120, 90))
    return im


@S_(0x1F34D, 'pineapple')
def _pineapple():
    im = new()
    ell(im, 3, 5, 12, 15, YEL)
    for x, y in ((5, 8), (8, 8), (10, 8), (6, 11), (9, 11), (7, 13)):
        put(im, [(x, y)], GLD_DK)
    poly(im, [(8, 5), (4, 0), (7, 3), (8, 0), (9, 3), (12, 0)], GRN)
    return outline(im, GLD_DK)


@S_(0x1F955, 'carrot')
def _carrot():
    im = new()
    poly(im, [(2, 3), (12, 7), (2, 15)], ORG)
    poly(im, [(10, 3), (13, 0), (14, 4)], GRN)
    poly(im, [(12, 5), (15, 4), (14, 8)], GRN)
    put(im, [(5, 6), (6, 9), (4, 11)], C(200, 100, 20))
    return outline(im, C(160, 80, 10))


@S_(0x1F33D, 'ear of corn')
def _corn():
    im = new()
    ell(im, 4, 0, 11, 13, YEL)
    for y in (3, 6, 9):
        line(im, [(5, y), (10, y)], GLD_DK)
    poly(im, [(7, 8), (2, 15), (5, 15), (9, 11)], GRN)
    poly(im, [(8, 8), (14, 15), (11, 15), (7, 11)], GRN)
    return outline(im, GLD_DK)


@S_(0x1F354, 'hamburger')
def _burger():
    im = new()
    ell(im, 1, 1, 14, 8, BRN_LT)
    rect(im, 1, 5, 14, 6, BRN_LT)
    put(im, [(5, 3), (8, 2), (11, 3)], CRM)
    rect(im, 0, 7, 15, 8, GRN)
    rect(im, 1, 9, 14, 10, BRN_DK)
    rect(im, 1, 11, 14, 11, ORG)
    ell(im, 1, 11, 14, 15, BRN_LT)
    return outline(im, BRN_DK)


@S_(0x1F35F, 'french fries')
def _fries():
    im = new()
    for x in range(3, 13, 2):
        rect(im, x, 0 + (x % 3), x + 1, 8, YEL)
    poly(im, [(2, 7), (13, 7), (11, 15), (4, 15)], RED)
    line(im, [(5, 9), (5, 14)], WH)
    line(im, [(8, 9), (8, 14)], WH)
    return outline(im, GLD_DK)


@S_(0x1F355, 'pizza')
def _pizza():
    im = new()
    poly(im, [(1, 2), (15, 2), (8, 15)], YEL)
    rect(im, 1, 1, 15, 3, BRN_LT)
    for x, y in ((5, 5), (9, 5), (7, 8), (8, 11)):
        ell(im, x - 1, y - 1, x + 1, y + 1, RED)
    return outline(im, BRN_DK)


@S_(0x1F32D, 'hot dog')
def _hotdog():
    im = new()
    ell(im, 0, 5, 15, 13, BRN_LT)
    rect(im, 1, 7, 14, 11, RED)
    line(im, [(2, 8), (4, 10), (6, 8), (8, 10), (10, 8), (12, 10)], YEL)
    return outline(im, BRN_DK)


@S_(0x1F32E, 'taco')
def _taco():
    im = new()
    ell(im, 0, 3, 15, 20, YEL)
    ell(im, 2, 5, 13, 20, TRANS)
    put(im, [(3, 5), (5, 4), (8, 4), (11, 4), (12, 5)], GRN)
    put(im, [(4, 6), (7, 6), (10, 6)], RED)
    return outline(im, GLD_DK)


@S_(0x1F363, 'sushi')
def _sushi():
    im = new()
    ell(im, 1, 6, 14, 14, WH)
    ell(im, 2, 3, 13, 9, ORG)
    line(im, [(4, 5), (11, 8)], WH)
    line(im, [(6, 4), (12, 7)], WH)
    return outline(im, GRY_DK)


@S_(0x1F370, 'shortcake')
def _cake():
    im = new()
    poly(im, [(1, 4), (14, 4), (14, 14), (1, 14)], CRM)
    rect(im, 1, 4, 14, 6, PINK)
    rect(im, 1, 9, 14, 9, PINK)
    ell(im, 6, 0, 9, 4, RED)
    return outline(im, BRN_DK)


@S_(0x1F382, 'birthday cake')
def _bday():
    im = new()
    rect(im, 1, 6, 14, 14, CRM)
    rect(im, 1, 6, 14, 8, PINK)
    rect(im, 1, 11, 14, 11, PINK)
    for x in (4, 7, 10):
        rect(im, x, 2, x + 1, 5, BLU)
        put(im, [(x, 0), (x, 1)], ORG)
    return outline(im, BRN_DK)


@S_(0x1F36A, 'cookie')
def _cookie():
    im = new()
    ell(im, 1, 1, 14, 14, BRN_LT)
    for x, y in ((5, 4), (10, 5), (4, 9), (8, 8), (11, 11), (7, 12)):
        put(im, [(x, y), (x + 1, y), (x, y + 1)], BRN_DK)
    return outline(im, BRN_DK)


@S_(0x1F36B, 'chocolate bar')
def _choc():
    im = new()
    rect(im, 2, 1, 13, 14, BRN)
    for x in (5, 9):
        line(im, [(x, 1), (x, 14)], BRN_DK)
    for y in (5, 9):
        line(im, [(2, y), (13, y)], BRN_DK)
    rect(im, 2, 1, 13, 3, RED)
    return outline(im, BRN_DK)


@S_(0x1F369, 'doughnut')
def _donut():
    im = new()
    ell(im, 1, 1, 14, 14, BRN_LT)
    ell(im, 1, 1, 14, 11, PINK)
    ell(im, 5, 5, 10, 10, TRANS)
    put(im, [(4, 3), (8, 2), (11, 5), (3, 7), (12, 8), (6, 11)], WH)
    return outline(im, BRN_DK)


@S_(0x1F366, 'soft ice cream')
def _icecream():
    im = new()
    poly(im, [(3, 8), (12, 8), (8, 15)], BRN_LT)
    ell(im, 2, 3, 13, 9, WH)
    ell(im, 4, 0, 11, 5, PINK)
    line(im, [(5, 9), (8, 14)], BRN)
    return outline(im, GRY_DK)


@S_(0x2615, 'hot beverage')
def _coffee():
    im = new()
    rect(im, 1, 5, 11, 13, WH)
    ell(im, 9, 6, 15, 12, None, GRY)
    rect(im, 2, 6, 10, 8, BRN)
    rect(im, 0, 14, 12, 15, GRY)
    line(im, [(4, 3), (4, 1)], GRY_LT)
    line(im, [(8, 4), (8, 0)], GRY_LT)
    return outline(im, GRY_DK)


@S_(0x1F37A, 'beer mug')
def _beer():
    im = new()
    rect(im, 2, 4, 11, 14, YEL)
    rect(im, 2, 1, 11, 4, WH)
    ell(im, 10, 6, 15, 12, None, GRY)
    for x in (4, 7, 9):
        line(im, [(x, 6), (x, 13)], GLD_DK)
    return outline(im, GLD_DK)


@S_(0x1F377, 'wine glass')
def _wine():
    im = new()
    poly(im, [(3, 1), (12, 1), (11, 7), (8, 9), (4, 7)], WH)
    poly(im, [(4, 4), (11, 4), (10, 7), (8, 8), (5, 7)], RED)
    rect(im, 7, 9, 8, 13, GRY)
    rect(im, 4, 14, 11, 15, GRY)
    return outline(im, GRY_DK)


@S_(0x1F95B, 'glass of milk')
def _milk():
    im = new()
    poly(im, [(3, 2), (12, 2), (11, 15), (4, 15)], WH)
    poly(im, [(3, 6), (12, 6), (11, 15), (4, 15)], CRM)
    return outline(im, GRY_DK)


F(0x1F917, 'hugging face', 'happy', 'grin', lambda im: put(im, [(0, 10), (1, 11), (14, 11), (15, 10), (1, 12), (14, 12)], SKIN))
F(0x1F973, 'partying face', 'happy', 'open', lambda im: (poly(im, [(8, -2), (5, 3), (11, 3)], PUR), put(im, [(8, 0), (7, 2)], GLD)))
F(0x1F92F, 'exploding head', 'big', 'o', lambda im: put(im, [(2, 0), (4, 1), (8, 0), (11, 1), (13, 0), (0, 3), (15, 3)], ORG))
F(0x1F924, 'drooling face', 'dot', 'drool')
F(0x1F910, 'zipper-mouth face', 'dot', 'flat', lambda im: line(im, [(4, 11), (11, 11)], GRY_DK))
F(0x1F9D0, 'face with monocle', 'dot', 'flat', lambda im: ell(im, 9, 3, 13, 8, None, GLD_DK), None, 'big')


@S_(0x1F440, 'eyes')
def _eyes():
    im = new()
    ell(im, 0, 3, 7, 12, WH)
    ell(im, 8, 3, 15, 12, WH)
    ell(im, 3, 6, 6, 10, BLU_DK)
    ell(im, 11, 6, 14, 10, BLU_DK)
    put(im, [(4, 7), (12, 7)], WH)
    return outline(im, GRY_DK)


def monkey(c1, ear_hands):
    im = new()
    ell(im, 1, 3, 14, 15, BRN)
    ell(im, 3, 6, 12, 14, BRN_LT)
    ell(im, -1, 5, 3, 9, BRN)
    ell(im, 12, 5, 16, 9, BRN)
    put(im, [(4, 7), (5, 7), (10, 7), (11, 7)], DK)
    put(im, [(7, 10), (8, 10)], DK)
    put(im, [(5, 12), (6, 13), (7, 13), (8, 13), (9, 13), (10, 12)], DK)
    ear_hands(im)
    return outline_auto(im)


@S_(0x1F648, 'see-no-evil monkey')
def _see_no():
    return monkey(None, lambda im: (rect(im, 3, 5, 12, 8, BRN_DK), put(im, [(4, 6), (11, 6)], BRN)))


@S_(0x1F649, 'hear-no-evil monkey')
def _hear_no():
    return monkey(None, lambda im: (rect(im, 0, 5, 3, 9, BRN_DK), rect(im, 12, 5, 15, 9, BRN_DK)))


@S_(0x1F64A, 'speak-no-evil monkey')
def _speak_no():
    return monkey(None, lambda im: rect(im, 4, 10, 11, 13, BRN_DK))


@S_(0x1F4BE, 'floppy disk')
def _floppy():
    im = new()
    rect(im, 1, 1, 14, 14, BLU)
    rect(im, 4, 1, 11, 6, GRY_LT)
    rect(im, 9, 2, 10, 5, DK)
    rect(im, 3, 9, 12, 14, WH)
    line(im, [(5, 11), (10, 11)], GRY)
    return outline(im, BLU_DK)


@S_(0x1F4BF, 'optical disk')
def _cd():
    im = new()
    ell(im, 1, 1, 14, 14, GRY_LT)
    ell(im, 6, 6, 9, 9, TRANS)
    put(im, [(3, 5), (4, 4), (5, 3)], LBL)
    put(im, [(10, 12), (11, 11), (12, 10)], PINK)
    return outline(im, GRY_DK)


@S_(0x1F375, 'teacup')
def _tea():
    im = new()
    ell(im, 0, 3, 12, 14, WH)
    rect(im, 0, 3, 12, 8, WH)
    rect(im, 1, 4, 11, 6, GRN)
    ell(im, 10, 5, 15, 11, None, GRY)
    return outline(im, GRY_DK)


@S_(0x1F3AF, 'bullseye')
def _dart():
    im = new()
    ell(im, 1, 1, 14, 14, RED)
    ell(im, 3, 3, 12, 12, WH)
    ell(im, 5, 5, 10, 10, RED)
    ell(im, 7, 7, 8, 8, WH)
    line(im, [(8, 8), (15, 1)], GRY_DK)
    return outline(im, RED_DK)


@S_(0x1F9E9, 'puzzle piece')
def _puzzle():
    im = new()
    rect(im, 2, 3, 12, 14, GRN)
    ell(im, 6, 0, 10, 4, GRN)
    ell(im, 10, 7, 15, 11, GRN)
    ell(im, 0, 7, 3, 10, TRANS)
    return outline(im, GRN_DK)


@S_(0x1F4E6, 'package')
def _package():
    im = new()
    poly(im, [(1, 4), (8, 1), (15, 4), (15, 12), (8, 15), (1, 12)], BRN_LT)
    line(im, [(1, 4), (8, 7), (15, 4)], BRN)
    line(im, [(8, 7), (8, 15)], BRN)
    rect(im, 5, 2, 6, 6, CRM)
    return outline(im, BRN_DK)


@S_(0x1F4CC, 'pushpin')
def _pin():
    im = new()
    ell(im, 5, 0, 12, 7, RED)
    rect(im, 7, 7, 10, 9, RED_DK)
    line(im, [(8, 9), (3, 15)], GRY_DK)
    put(im, [(7, 2), (6, 3)], PINK)
    return outline(im, RED_DK)


@S_(0x1F4CE, 'paperclip')
def _clip():
    im = new()
    ImageDraw.Draw(im).arc([4, 0, 11, 8], 180, 360, fill=GRY_DK)
    line(im, [(4, 4), (4, 12)], GRY_DK)
    line(im, [(11, 4), (11, 14)], GRY_DK)
    ImageDraw.Draw(im).arc([4, 9, 11, 16], 0, 180, fill=GRY_DK)
    line(im, [(7, 3), (7, 11)], GRY_DK)
    return im


@S_(0x2702, 'scissors')
def _scissors():
    im = new()
    line(im, [(2, 1), (13, 12)], GRY)
    line(im, [(2, 12), (13, 1)], GRY)
    ell(im, 0, 0, 3, 3, None, RED)
    ell(im, 0, 11, 3, 15, None, RED)
    return outline(im, GRY_DK)


@S_(0x1F527, 'wrench')
def _wrench():
    im = new()
    line(im, [(4, 11), (12, 3)], GRY)
    line(im, [(5, 12), (13, 4)], GRY)
    ell(im, 9, 0, 15, 6, GRY)
    put(im, [(12, 1), (13, 2), (12, 3)], TRANS)
    ell(im, 1, 9, 5, 14, GRY)
    return outline(im, GRY_DK)


@S_(0x1F528, 'hammer')
def _hammer():
    im = new()
    rect(im, 2, 1, 12, 5, GRY)
    line(im, [(7, 5), (4, 14)], BRN)
    line(im, [(8, 5), (5, 14)], BRN)
    return outline(im, GRY_DK)


@S_(0x2699, 'gear')
def _gear():
    im = new()
    ell(im, 3, 3, 12, 12, GRY)
    for a, b in (((6, 0), (9, 2)), ((6, 13), (9, 15)), ((0, 6), (2, 9)), ((13, 6), (15, 9))):
        rect(im, a[0], a[1], b[0], b[1], GRY)
    put(im, [(2, 2), (3, 2), (2, 3), (13, 2), (12, 2), (13, 3), (2, 13), (3, 13), (2, 12), (13, 13), (12, 13), (13, 12)], GRY)
    ell(im, 6, 6, 9, 9, TRANS)
    return outline(im, GRY_DK)


@S_(0x1F5D1, 'wastebasket')
def _bin():
    im = new()
    rect(im, 3, 4, 12, 14, GRY)
    rect(im, 2, 2, 13, 3, GRY_DK)
    rect(im, 6, 0, 9, 1, GRY_DK)
    for x in (5, 7, 9, 11):
        line(im, [(x, 6), (x, 12)], GRY_DK)
    return outline(im, BLK)


@S_(0x1F4C5, 'calendar')
def _calendar():
    im = new()
    rect(im, 1, 2, 14, 14, WH)
    rect(im, 1, 2, 14, 5, RED)
    for x in (4, 10):
        rect(im, x, 0, x + 1, 3, GRY_DK)
    for y in (8, 11):
        for x in (3, 6, 9, 12):
            put(im, [(x, y), (x + 1, y)], GRY_DK)
    return outline(im, GRY_DK)


@S_(0x1F4CA, 'bar chart')
def _bars():
    im = new()
    rect(im, 1, 1, 14, 14, WH)
    rect(im, 3, 8, 5, 13, BLU)
    rect(im, 7, 4, 9, 13, GRN)
    rect(im, 11, 6, 13, 13, RED)
    return outline(im, GRY_DK)


@S_(0x1F4C8, 'chart increasing')
def _chart_up():
    im = new()
    rect(im, 1, 1, 14, 14, WH)
    line(im, [(3, 12), (6, 8), (9, 10), (13, 3)], GRN)
    line(im, [(3, 13), (6, 9), (9, 11), (13, 4)], GRN)
    return outline(im, GRY_DK)


@S_(0x1F4CB, 'clipboard')
def _clipboard():
    im = new()
    rect(im, 2, 2, 13, 15, BRN_LT)
    rect(im, 4, 4, 11, 14, WH)
    rect(im, 5, 0, 10, 3, GRY)
    for y in (6, 8, 10, 12):
        line(im, [(5, y), (10, y)], GRY)
    return outline(im, BRN_DK)


@S_(0x1F4E2, 'loudspeaker')
def _loud():
    im = new()
    poly(im, [(1, 6), (6, 6), (12, 1), (12, 14), (6, 10), (1, 10)], RED)
    rect(im, 2, 10, 4, 14, RED)
    line(im, [(14, 5), (15, 4)], ORG)
    line(im, [(14, 8), (15, 8)], ORG)
    line(im, [(14, 11), (15, 12)], ORG)
    return outline(im, RED_DK)


@S_(0x1F50A, 'speaker high volume')
def _speaker():
    im = new()
    poly(im, [(1, 5), (4, 5), (8, 1), (8, 14), (4, 10), (1, 10)], GRY)
    ImageDraw.Draw(im).arc([6, 4, 11, 11], -60, 60, fill=GRY_DK)
    ImageDraw.Draw(im).arc([6, 1, 14, 14], -60, 60, fill=GRY_DK)
    return outline(im, BLK)


@S_(0x1F4A8, 'dashing away')
def _dash():
    im = new()
    for y, x0, x1 in ((3, 3, 13), (7, 1, 12), (11, 4, 14)):
        line(im, [(x0, y), (x1, y)], LBL)
        line(im, [(x0, y + 1), (x1 - 2, y + 1)], WH)
    return im


@S_(0x1F4A6, 'sweat droplets')
def _drops():
    im = new()
    for x, y in ((3, 2), (9, 5), (4, 9)):
        poly(im, [(x + 2, y), (x + 4, y + 4), (x, y + 4)], LBL)
        ell(im, x, y + 2, x + 4, y + 6, LBL)
    return outline(im, BLU_DK)


@S_(0x1F4A2, 'anger symbol')
def _anger():
    im = new()
    for pts in (((2, 5), (5, 2)), ((10, 2), (13, 5)), ((2, 10), (5, 13)), ((10, 13), (13, 10))):
        for d in (0, 1):
            line(im, [(pts[0][0] + d, pts[0][1]), (pts[1][0] + d, pts[1][1])], RED)
    return im


def arrow(direction):
    im = new()
    poly(im, [(1, 8), (8, 1), (8, 5), (15, 5), (15, 11), (8, 11), (8, 15)], BLU)
    return outline(im, BLU_DK).rotate(direction, resample=Image.NEAREST)


SPECIAL.append((0x2B05, 'left arrow', lambda: arrow(0)))
SPECIAL.append((0x2B06, 'up arrow', lambda: arrow(270)))
SPECIAL.append((0x27A1, 'right arrow', lambda: arrow(180)))
SPECIAL.append((0x2B07, 'down arrow', lambda: arrow(90)))


@S_(0x1F501, 'repeat button')
def _repeat():
    im = new()
    ImageDraw.Draw(im).arc([2, 2, 13, 13], 20, 300, fill=GRN, width=2)
    poly(im, [(10, 0), (15, 3), (9, 6)], GRN)
    return outline(im, GRN_DK)


@S_(0x267B, 'recycling symbol')
def _recycle():
    return _repeat()


@S_(0x2139, 'information')
def _info():
    im = new()
    ell(im, 1, 1, 14, 14, BLU)
    rect(im, 7, 3, 8, 4, WH)
    rect(im, 7, 6, 8, 12, WH)
    return outline(im, BLU_DK)


def dot(col, dk):
    im = new()
    ell(im, 1, 1, 14, 14, col)
    put(im, [(4, 4), (5, 3), (4, 5)], WH)
    return outline(im, dk)


SPECIAL.append((0x1F7E2, 'green circle', lambda: dot(GRN, GRN_DK)))
SPECIAL.append((0x1F534, 'red circle', lambda: dot(RED, RED_DK)))
SPECIAL.append((0x1F535, 'blue circle', lambda: dot(BLU, BLU_DK)))
SPECIAL.append((0x1F7E1, 'yellow circle', lambda: dot(YEL, GLD_DK)))
SPECIAL.append((0x1F7E0, 'orange circle', lambda: dot(ORG, BRN_DK)))
SPECIAL.append((0x1F7E3, 'purple circle', lambda: dot(PUR, PUR_DK)))


@S_(0x25B6, 'play button')
def _play():
    im = new()
    poly(im, [(3, 1), (13, 8), (3, 14)], GRN)
    return outline(im, GRN_DK)


@S_(0x23F8, 'pause button')
def _pause():
    im = new()
    rect(im, 3, 2, 6, 13, BLU)
    rect(im, 9, 2, 12, 13, BLU)
    return outline(im, BLU_DK)


@S_(0x23F9, 'stop button')
def _stop():
    im = new()
    rect(im, 2, 2, 13, 13, RED)
    return outline(im, RED_DK)


@S_(0x1F3C1, 'chequered flag')
def _flag():
    im = new()
    rect(im, 2, 0, 3, 15, GRY_DK)
    for y in range(1, 9):
        for x in range(4, 14):
            put(im, [(x, y)], DK if (x + y) % 2 else WH)
    return outline(im, BLK)


@S_(0x1F9E0, 'brain')
def _brain():
    im = new()
    ell(im, 1, 2, 8, 13, PINK)
    ell(im, 7, 2, 14, 13, PINK)
    line(im, [(8, 2), (8, 13)], RED)
    put(im, [(4, 5), (5, 6), (4, 8), (10, 5), (11, 6), (11, 9), (5, 10), (10, 11)], RED)
    return outline(im, RED_DK)


@S_(0x1F4B0, 'money bag')
def _moneybag():
    im = new()
    ell(im, 1, 5, 14, 15, GLD)
    poly(im, [(5, 6), (10, 6), (11, 2), (4, 2)], GLD)
    rect(im, 4, 1, 11, 2, BRN)
    pat(im, 6, 8, ['.kk.', 'k...', '.kk.', '...k', '.kk.'])
    return outline(im, BRN_DK)


@S_(0x1F4B5, 'dollar banknote')
def _dollar():
    im = new()
    rect(im, 0, 3, 15, 12, GRN)
    ell(im, 5, 4, 10, 11, C(150, 235, 160))
    pat(im, 7, 5, ['.kk', 'k..', '.k.', '..k', 'kk.'])
    return outline(im, GRN_DK)


@S_(0x1F9F8, 'teddy bear')
def _teddy():
    return make_animal(BRN_LT, ears_round, CRM)


@S_(0x1F52E, 'crystal ball')
def _crystal():
    im = new()
    ell(im, 1, 0, 14, 12, PUR)
    put(im, [(4, 3), (5, 2), (4, 4)], WH)
    rect(im, 3, 12, 12, 14, BRN)
    return outline(im, PUR_DK)


@S_(0x1F3A8, 'artist palette')
def _palette():
    im = new()
    ell(im, 0, 2, 15, 14, BRN_LT)
    ell(im, 9, 9, 13, 13, TRANS)
    put(im, [(3, 6), (4, 6), (3, 7)], RED)
    put(im, [(6, 4), (7, 4), (6, 5)], YEL)
    put(im, [(10, 4), (11, 4), (10, 5)], GRN)
    put(im, [(12, 7), (13, 7), (12, 8)], BLU)
    return outline(im, BRN_DK)


ALL = []


def collect():
    seen = set()
    for cp, name, fn in FACES + SPECIAL:
        if cp in seen:
            continue
        seen.add(cp)
        im = fn()
        if im.size != (S, S):
            im = im.crop((0, 0, S, S))
        ALL.append((cp, name, im))


def main():
    collect()
    n = len(ALL)
    cols = 16
    rows = (n + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * S, rows * S), TRANS)
    for i, (cp, name, im) in enumerate(ALL):
        sheet.alpha_composite(im, ((i % cols) * S, (i // cols) * S))
    import io
    out = os.path.join(ROOT, "art", "silicon")
    os.makedirs(out, exist_ok=True)
    buf = io.BytesIO()
    sheet.save(buf, "PNG")
    png_path = os.path.join(out, "emoji.png")
    if not os.path.exists(png_path) or open(png_path, "rb").read() != buf.getvalue():
        open(png_path, "wb").write(buf.getvalue())
    lines = ["// generated by tools/make_emoji.py: code point and name of every emoji sprite, in sheet order\n"]
    for cp, name, im in ALL:
        lines.append('{0x%X, "%s"},\n' % (cp, name))
    text = "".join(lines)
    inc = os.path.join(ROOT, "src", "kernel", "gfx", "emoji_data.inc")
    if not os.path.exists(inc) or open(inc, encoding="utf-8").read() != text:
        with open(inc, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    return ALL


if __name__ == "__main__":
    items = main()
    print(len(items), "emoji")
