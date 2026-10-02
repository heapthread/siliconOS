// silicon - 2D drawing on 32-bit ARGB surfaces
#pragma once
#include "../core.h"

// glyph codes for the extended silicon codepage (see tools/font.txt)
#define G_FULL   "\x80"
#define G_DARK   "\x81"
#define G_MED    "\x82"
#define G_LIGHT  "\x83"
#define G_H      "\x84"
#define G_V      "\x85"
#define G_TL     "\x86"
#define G_TR     "\x87"
#define G_BL     "\x88"
#define G_BR     "\x89"
#define G_LT     "\x8a"
#define G_RT     "\x8b"
#define G_TT     "\x8c"
#define G_BT     "\x8d"
#define G_X      "\x8e"
#define G_LEFT   "\x8f"
#define G_RIGHT  "\x90"
#define G_UP     "\x91"
#define G_DOWN   "\x92"
#define G_BULLET "\x93"
#define G_CHECK  "\x94"
#define G_TIMES  "\x95"
#define G_DEG    "\x96"
#define G_ELLIP  "\x97"
#define G_PLAY   "\x98"
#define G_BACK   "\x99"
#define G_TRIUP  "\x9a"
#define G_TRIDN  "\x9b"
#define G_CHIP   "\x9c"
#define G_HEART  "\x9d"
#define G_POWER  "\x9e"
#define G_DIAM   "\x9f"

static const int FONT_W = 8;
static const int FONT_H = 12;

static inline u32 rgb(u32 r, u32 g, u32 b) { return 0xFF000000 | (r << 16) | (g << 8) | b; }
static inline u32 argb(u32 a, u32 r, u32 g, u32 b) { return (a << 24) | (r << 16) | (g << 8) | b; }
static inline u32 blend(u32 dst, u32 src, u32 a) {
    u32 ia = 255 - a;
    u32 rb = (((src & 0xFF00FF) * a + (dst & 0xFF00FF) * ia) >> 8) & 0xFF00FF;
    u32 g  = (((src & 0x00FF00) * a + (dst & 0x00FF00) * ia) >> 8) & 0x00FF00;
    return 0xFF000000 | rb | g;
}
static inline u32 shade(u32 c, int pct) {   // pct < 100 darkens, > 100 lightens
    int r = ((c >> 16) & 255) * pct / 100, g = ((c >> 8) & 255) * pct / 100, b = (c & 255) * pct / 100;
    return rgb(min(r, 255), min(g, 255), min(b, 255));
}
static inline u32 mix(u32 a, u32 b, int t /*0..255*/) { return blend(a, b, (u32)t); }

struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect intersect(const Rect& o) const {
        int x0 = max(x, o.x), y0 = max(y, o.y);
        int x1 = min(x + w, o.x + o.w), y1 = min(y + h, o.y + o.h);
        if (x1 <= x0 || y1 <= y0) return {0, 0, 0, 0};
        return {x0, y0, x1 - x0, y1 - y0};
    }
    bool empty() const { return w <= 0 || h <= 0; }
};

struct Image {
    int w = 0, h = 0;
    u32* px = nullptr;          // ARGB, alpha honoured by blit
};

struct Canvas {
    u32* px = nullptr;
    int w = 0, h = 0, pitch = 0;     // pitch in pixels
    Rect clip{0, 0, 0, 0};
    int ox = 0, oy = 0;              // drawing origin offset

    void init(u32* p, int width, int height, int pitch_px);
    void set_clip(Rect r);           // r in canvas coordinates (origin applied)
    void reset_clip();
    void origin(int x, int y) { ox = x; oy = y; }

    inline void put(int x, int y, u32 c) {
        x += ox; y += oy;
        if (x < clip.x || y < clip.y || x >= clip.x + clip.w || y >= clip.y + clip.h) return;
        px[y * pitch + x] = c;
    }
    inline u32 get(int x, int y) const {
        x += ox; y += oy;
        if (x < 0 || y < 0 || x >= w || y >= h) return 0;
        return px[y * pitch + x];
    }
    void clear(u32 c);
    void fill(int x, int y, int w, int h, u32 c);
    void fill_alpha(int x, int y, int w, int h, u32 c, u32 a);
    void rect(int x, int y, int w, int h, u32 c);
    void hline(int x, int y, int w, u32 c);
    void vline(int x, int y, int h, u32 c);
    void line(int x0, int y0, int x1, int y1, u32 c);
    void dither(int x, int y, int w, int h, u32 c, int phase = 0);     // 50% checker
    void gradient_v(int x, int y, int w, int h, u32 top, u32 bottom, int bands = 0);
    void circle(int cx, int cy, int r, u32 c, bool filled);
    void blit(const Image& img, int x, int y);                         // alpha aware
    void blit_scaled(const Image& img, int x, int y, int dw, int dh);  // nearest, alpha aware
    void blit_tinted(const Image& img, int x, int y, u32 tint, u32 amount);
    void blit_raw(const u32* src, int sw, int sh, int spitch, int x, int y);  // opaque copy
    // nearest-neighbour stretch of an opaque image with a global alpha (0..255)
    void blit_stretch_alpha(const Image& img, int x, int y, int dw, int dh, u32 alpha);
    // pixel dissolve between two full-screen images: pixels whose Bayer threshold < level (0..16) come from b
    void dissolve(const Image& a, const Image& b, int level);
    // text: scale 1/2/3..., bold smears one pixel right
    int  draw_char(int x, int y, u8 c, u32 col, int scale = 1, bool bold = false);
    int  text(int x, int y, const char* s, u32 col, int scale = 1, bool bold = false);
    int  text_n(int x, int y, const char* s, int n, u32 col, int scale = 1, bool bold = false);
    void text_centered(int x, int y, int w, const char* s, u32 col, int scale = 1, bool bold = false);
    void text_shadow(int x, int y, const char* s, u32 col, u32 sh, int scale = 1, bool bold = false);
};

int text_width(const char* s, int scale = 1, bool bold = false);
extern const u8 font_rows[256][12];

// .sil image decoding (see tools/assets.py)
bool image_decode(const u8* data, u32 len, Image& out);
Image image_scale(const Image& src, int w, int h);
Image image_new(int w, int h, u32 fill);
void  image_free(Image& img);

// ---------------------------------------------------------------- image files (gfx/imgdec.cpp)
// decode PNG / GIF / JPEG (baseline + progressive) / BMP into ARGB; img.px is kmalloc'd
bool image_load(const u8* data, u32 len, Image& img);
const char* image_format(const u8* data, u32 len);   // "png", "jpeg", ... or nullptr
void image_shrink(Image& img, int maxw, int maxh);    // box filter, keeps the aspect ratio
