// silicon - 2D drawing primitives
#include "gfx.h"
#include "vfont.h"
#include "../text.h"

const u8 font_rows[256][12] = {
#include "font_data.inc"
};

void Canvas::init(u32* p, int width, int height, int pitch_px) {
    px = p; w = width; h = height; pitch = pitch_px;
    reset_clip();
    ox = oy = 0;
}

void Canvas::reset_clip() { clip = {0, 0, w, h}; }

void Canvas::set_clip(Rect r) {
    Rect full{0, 0, w, h};
    clip = Rect{r.x + ox, r.y + oy, r.w, r.h}.intersect(full);
}

void Canvas::clear(u32 c) {
    for (int y = 0; y < h; y++) memset32(px + y * pitch, c, w);
}

void Canvas::fill(int x, int y, int fw, int fh, u32 c) {
    Rect r = Rect{x + ox, y + oy, fw, fh}.intersect(clip);
    if (r.empty()) return;
    for (int yy = r.y; yy < r.y + r.h; yy++) memset32(px + yy * pitch + r.x, c, r.w);
}

void Canvas::fill_alpha(int x, int y, int fw, int fh, u32 c, u32 a) {
    if (a >= 255) { fill(x, y, fw, fh, c); return; }
    Rect r = Rect{x + ox, y + oy, fw, fh}.intersect(clip);
    if (r.empty()) return;
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        u32* row = px + yy * pitch;
        for (int xx = r.x; xx < r.x + r.w; xx++) row[xx] = blend(row[xx], c, a);
    }
}

void Canvas::hline(int x, int y, int fw, u32 c) { fill(x, y, fw, 1, c); }
void Canvas::vline(int x, int y, int fh, u32 c) { fill(x, y, 1, fh, c); }

void Canvas::rect(int x, int y, int fw, int fh, u32 c) {
    hline(x, y, fw, c);
    hline(x, y + fh - 1, fw, c);
    vline(x, y, fh, c);
    vline(x + fw - 1, y, fh, c);
}

void Canvas::line(int x0, int y0, int x1, int y1, u32 c) {
    int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Canvas::dither(int x, int y, int fw, int fh, u32 c, int phase) {
    Rect r = Rect{x + ox, y + oy, fw, fh}.intersect(clip);
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        u32* row = px + yy * pitch;
        for (int xx = r.x + ((r.x + yy + phase) & 1); xx < r.x + r.w; xx += 2) row[xx] = c;
    }
}

void Canvas::gradient_v(int x, int y, int fw, int fh, u32 top, u32 bottom, int bands) {
    for (int i = 0; i < fh; i++) {
        int t = fh > 1 ? i * 255 / (fh - 1) : 0;
        if (bands > 1) t = (t * bands / 256) * 255 / (bands - 1);
        hline(x, y + i, fw, mix(top, bottom, t));
    }
}

void Canvas::circle(int cx, int cy, int r, u32 c, bool filled) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        if (filled) {
            hline(cx - x, cy + y, 2 * x + 1, c);
            hline(cx - x, cy - y, 2 * x + 1, c);
            hline(cx - y, cy + x, 2 * y + 1, c);
            hline(cx - y, cy - x, 2 * y + 1, c);
        } else {
            put(cx + x, cy + y, c); put(cx - x, cy + y, c);
            put(cx + x, cy - y, c); put(cx - x, cy - y, c);
            put(cx + y, cy + x, c); put(cx - y, cy + x, c);
            put(cx + y, cy - x, c); put(cx - y, cy - x, c);
        }
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void Canvas::blit(const Image& img, int x, int y) {
    if (!img.px) return;
    Rect r = Rect{x + ox, y + oy, img.w, img.h}.intersect(clip);
    if (r.empty()) return;
    int sx0 = r.x - (x + ox), sy0 = r.y - (y + oy);
    for (int yy = 0; yy < r.h; yy++) {
        const u32* s = img.px + (sy0 + yy) * img.w + sx0;
        u32* d = px + (r.y + yy) * pitch + r.x;
        for (int xx = 0; xx < r.w; xx++) {
            u32 c = s[xx], a = c >> 24;
            if (a == 255) d[xx] = c;
            else if (a) d[xx] = blend(d[xx], c, a);
        }
    }
}

void Canvas::blit_scaled(const Image& img, int x, int y, int dw, int dh) {
    if (!img.px || dw <= 0 || dh <= 0) return;
    if (dw == img.w && dh == img.h) { blit(img, x, y); return; }
    Rect r = Rect{x + ox, y + oy, dw, dh}.intersect(clip);
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        int sy = (yy - (y + oy)) * img.h / dh;
        const u32* s = img.px + sy * img.w;
        u32* d = px + yy * pitch;
        for (int xx = r.x; xx < r.x + r.w; xx++) {
            u32 c = s[(xx - (x + ox)) * img.w / dw], a = c >> 24;
            if (a == 255) d[xx] = c;
            else if (a) d[xx] = blend(d[xx], c, a);
        }
    }
}

void Canvas::blit_tinted(const Image& img, int x, int y, u32 tint, u32 amount) {
    if (!img.px) return;
    Rect r = Rect{x + ox, y + oy, img.w, img.h}.intersect(clip);
    int sx0 = r.x - (x + ox), sy0 = r.y - (y + oy);
    for (int yy = 0; yy < r.h; yy++) {
        const u32* s = img.px + (sy0 + yy) * img.w + sx0;
        u32* d = px + (r.y + yy) * pitch + r.x;
        for (int xx = 0; xx < r.w; xx++) {
            u32 c = s[xx], a = c >> 24;
            if (!a) continue;
            c = blend(c, tint, amount);
            d[xx] = a == 255 ? c : blend(d[xx], c, a);
        }
    }
}

void Canvas::blit_raw(const u32* src, int sw, int sh, int spitch, int x, int y) {
    Rect r = Rect{x + ox, y + oy, sw, sh}.intersect(clip);
    if (r.empty()) return;
    int sx0 = r.x - (x + ox), sy0 = r.y - (y + oy);
    for (int yy = 0; yy < r.h; yy++)
        memcpy(px + (r.y + yy) * pitch + r.x, src + (sy0 + yy) * spitch + sx0, r.w * 4);
}

void Canvas::blit_stretch_alpha(const Image& img, int x, int y, int dw, int dh, u32 alpha) {
    if (!img.px || dw <= 0 || dh <= 0 || alpha == 0) return;
    Rect r = Rect{x + ox, y + oy, dw, dh}.intersect(clip);
    if (r.empty()) return;
    // pixels may carry their own alpha (rounded window corners); the rows in the middle are always opaque
    const int EDGE = 14;
    if (dw == img.w && dh == img.h) {
        int sx0 = r.x - (x + ox), sy0 = r.y - (y + oy);
        for (int yy = 0; yy < r.h; yy++) {
            int sy = sy0 + yy;
            const u32* s = img.px + sy * img.w + sx0;
            u32* d = px + (r.y + yy) * pitch + r.x;
            if (alpha >= 255 && sy >= EDGE && sy < img.h - EDGE) { memcpy(d, s, r.w * 4); continue; }
            for (int xx = 0; xx < r.w; xx++) {
                u32 c = s[xx], pa = c >> 24;
                if (pa == 255) d[xx] = alpha >= 255 ? c : blend(d[xx], c, alpha);
                else if (pa) d[xx] = blend(d[xx], c, pa * alpha / 255);
            }
        }
        return;
    }
    // 16.16 fixed point stepping
    u32 sx_step = (u32)(((u64)img.w << 16) / dw), sy_step = (u32)(((u64)img.h << 16) / dh);
    u32 sx0 = (u32)(r.x - (x + ox)) * sx_step;
    u32 sy = (u32)(r.y - (y + oy)) * sy_step;
    for (int yy = 0; yy < r.h; yy++, sy += sy_step) {
        const u32* srow = img.px + (sy >> 16) * img.w;
        u32* d = px + (r.y + yy) * pitch + r.x;
        u32 sx = sx0;
        for (int xx = 0; xx < r.w; xx++, sx += sx_step) {
            u32 c = srow[sx >> 16], pa = c >> 24;
            if (pa == 255) d[xx] = alpha >= 255 ? c : blend(d[xx], c, alpha);
            else if (pa) d[xx] = blend(d[xx], c, pa * alpha / 255);
        }
    }
}

void Canvas::dissolve(const Image& a, const Image& b, int level) {
    static const u8 bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    Rect r = clip;
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        int wy = yy - oy;
        if (wy < 0 || wy >= a.h || wy >= b.h) continue;
        const u32* ra = a.px + wy * a.w;
        const u32* rb = b.px + wy * b.w;
        u32* d = px + yy * pitch;
        for (int xx = r.x; xx < r.x + r.w; xx++) {
            int wx = xx - ox;
            if (wx < 0 || wx >= a.w) continue;
            // blocks of 4x4 pixels keep the transition chunky and pixel-arty
            d[xx] = bayer[(wy >> 2) & 3][(wx >> 2) & 3] < level ? rb[wx] : ra[wx];
        }
    }
}

// ---------------------------------------------------------------- text
bool gfx_vector_text = true;

static const u16 sil_hi[32] = {0x2588, 0x2593, 0x2592, 0x2591, 0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C,
                               0x2524, 0x252C, 0x2534, 0x253C, 0x2190, 0x2192, 0x2191, 0x2193, 0x2022, 0x2713, 0x00D7,
                               0x00B0, 0x2026, 0x25B6, 0x25C0, 0x25B2, 0x25BC, 0x25A3, 0x2665, 0x23FB, 0x25C6};
u32 silicon_cp(u8 b) { return b >= 0x80 && b < 0xA0 ? sil_hi[b - 0x80] : b; }

static const float UI_PX = 12.0f;
static const int   UI_BASE = 10;       // baseline below the top of a text row (proportional)
static const int   MONO_BASE = 9;

struct VTab { short gid[256]; float adv[256]; bool ready, ok; VFont* f; };
static VTab vtabs[4];

static VTab& vtab(bool bold, bool mono) {
    VTab& t = vtabs[(bold ? 1 : 0) + (mono ? 2 : 0)];
    if (t.ready) return t;
    t.ready = true;
    t.f = vfont_get(bold, mono);
    t.ok = t.f != nullptr;
    for (int b = 0; b < 256; b++) {
        t.gid[b] = 0;
        t.adv[b] = FONT_W / UI_PX;
        if (!t.ok || b < 32) continue;
        int g = vfont_gid(t.f, silicon_cp((u8)b));
        t.gid[b] = (short)g;
        if (g > 0) t.adv[b] = vfont_advance(t.f, g, 1.0f);
    }
    return t;
}

static int bitmap_char(Canvas& cv, int x, int y, u8 c, u32 col, int scale, bool bold) {
    const u8* g = font_rows[c];
    for (int row = 0; row < FONT_H; row++) {
        u8 bits = g[row];
        if (bold) bits |= bits >> 1;
        if (!bits) continue;
        for (int b = 0; b < 8; b++) {
            if (!(bits & (0x80 >> b))) continue;
            if (scale == 1) cv.put(x + b, y + row, col);
            else cv.fill(x + b * scale, y + row * scale, scale, scale, col);
        }
    }
    return FONT_W * scale;
}

int Canvas::draw_char(int x, int y, u8 c, u32 col, int scale, bool bold) {
    if (gfx_vector_text) {
        VTab& t = vtab(bold, true);
        if (t.ok && c >= 32 && t.gid[c] > 0) {
            if (x + ox + FONT_W * scale + 4 < clip.x || y + oy + FONT_H * scale + 4 < clip.y ||
                x + ox - 4 > clip.x + clip.w || y + oy - 4 > clip.y + clip.h) return FONT_W * scale;
            vfont_draw_gid(*this, t.f, t.gid[c], UI_PX * scale, (float)x, (float)(y + MONO_BASE * scale), col);
            return FONT_W * scale;
        }
        if (c == ' ') return FONT_W * scale;
    }
    return bitmap_char(*this, x, y, c, col, scale, bold);
}

int Canvas::text_mono(int x, int y, const char* s, u32 col, int scale, bool bold) {
    int x0 = x;
    for (; *s; s++) {
        if (*s == '\n') { x = x0; y += FONT_H * scale; continue; }
        if (is_emoji_lead((u8)*s) && s[1]) {
            if (const Image* e = emoji_sprite(emoji_from_bytes((u8)s[0], (u8)s[1]), emoji_px)) {
                int size = emoji_px * scale, ex = x + (2 * FONT_W * scale - size) / 2, ey = y - (emoji_px - FONT_H) * scale / 2;
                if (scale == 1) blit(*e, ex, ey); else blit_scaled(*e, ex, ey, size, size);
            }
            x += 2 * FONT_W * scale;
            s++;
            continue;
        }
        x += draw_char(x, y, (u8)*s, col, scale, bold);
    }
    return x - x0;
}

int text_width_mono(const char* s, int scale) {
    int n = 0, best = 0;
    for (; *s; s++) {
        if (*s == '\n') { best = max(best, n); n = 0; continue; }
        n++;
    }
    return max(best, n) * FONT_W * scale;
}

int text_line_h(int scale) { return (gfx_vector_text ? 14 : FONT_H) * scale; }

int Canvas::text_n(int x, int y, const char* s, int n, u32 col, int scale, bool bold) {
    int x0 = x;
    VTab* t = gfx_vector_text ? &vtab(bold, false) : nullptr;
    if (t && !t->ok) t = nullptr;
    float pen = (float)x;
    float px = UI_PX * scale;
    for (int i = 0; i < n && s[i]; i++) {
        u8 b = (u8)s[i];
        if (b == '\n') { x = x0; pen = (float)x; y += FONT_H * scale; continue; }
        if (is_emoji_lead(b) && i + 1 < n && s[i + 1]) {
            if (const Image* e = emoji_sprite(emoji_from_bytes(b, (u8)s[i + 1]), emoji_px)) {
                int size = emoji_px * scale, ex = (int)(pen + 0.5f) + (2 * FONT_W * scale - size) / 2, ey = y - (emoji_px - FONT_H) * scale / 2;
                if (scale == 1) blit(*e, ex, ey);
                else blit_scaled(*e, ex, ey, size, size);
            }
            pen += 2 * FONT_W * scale;
            i++;
            continue;
        }
        if (mono) { pen += draw_char((int)(pen + 0.5f), y, b, col, scale, bold); continue; }
        if (t) {
            if (b == ' ' || b < 32) { pen += t->adv[b < 32 ? 32 : b] * px; continue; }
            if (t->gid[b] > 0) {
                vfont_draw_gid(*this, t->f, t->gid[b], px, pen, (float)(y + UI_BASE * scale), col);
                pen += t->adv[b] * px;
                continue;
            }
        }
        pen += bitmap_char(*this, (int)(pen + 0.5f), y, b, col, scale, bold);
    }
    return (int)(pen + 0.5f) - x0;
}

int Canvas::text(int x, int y, const char* s, u32 col, int scale, bool bold) {
    return text_n(x, y, s, 0x7FFFFFFF, col, scale, bold);
}

void Canvas::text_centered(int x, int y, int fw, const char* s, u32 col, int scale, bool bold) {
    text(x + (fw - text_width(s, scale, bold)) / 2, y, s, col, scale, bold);
}

void Canvas::text_shadow(int x, int y, const char* s, u32 col, u32 sh, int scale, bool bold) {
    text(x + scale, y + scale, s, sh, scale, bold);
    text(x, y, s, col, scale, bold);
}

int text_width(const char* s, int scale, bool bold) {
    VTab* t = gfx_vector_text ? &vtab(bold, false) : nullptr;
    if (t && !t->ok) t = nullptr;
    if (!t) return text_width_mono(s, scale);
    float n = 0, best = 0;
    for (; *s; s++) {
        u8 b = (u8)*s;
        if (b == '\n') { best = max(best, n); n = 0; continue; }
        if (is_emoji_lead(b) && s[1]) { n += 2.0f * FONT_W / UI_PX; s++; continue; }
        n += t->adv[b < 32 ? 32 : b];
    }
    return (int)(max(best, n) * UI_PX * scale + 0.5f);
}

// ---------------------------------------------------------------- smooth shapes
static inline float fsqrt(float x) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
static int isqrt_u32(u32 v) {
    u32 r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (int)r;
}

// coverage 0..255 of a rounded rectangle at pixel (px, py) relative to its top-left
int rr_coverage(int px, int py, int w, int h, int r) {
    if (px < 0 || py < 0 || px >= w || py >= h) return 0;
    int cx, cy;
    if (px < r) cx = r; else if (px >= w - r) cx = w - r; else return 255;
    if (py < r) cy = r; else if (py >= h - r) cy = h - r; else return 255;
    int dx = cx * 16 - (px * 16 + 8), dy = cy * 16 - (py * 16 + 8);
    if (px >= w - r) dx = (px * 16 + 8) - cx * 16;
    if (py >= h - r) dy = (py * 16 + 8) - cy * 16;
    int d = isqrt_u32((u32)(dx * dx + dy * dy));
    int v = r * 16 - d + 8;
    return v <= 0 ? 0 : v >= 16 ? 255 : v * 255 / 16;
}

static inline int rr_cov(int px, int py, int w, int h, int r) { return rr_coverage(px, py, w, h, r); }

void Canvas::mix_px(int x, int y, u32 c, u32 a) {
    x += ox; y += oy;
    if (a == 0 || x < clip.x || y < clip.y || x >= clip.x + clip.w || y >= clip.y + clip.h) return;
    u32* p = px + y * pitch + x;
    *p = a >= 255 ? (c | 0xFF000000) : blend(*p, c, a);
}

void Canvas::rrect(int x, int y, int rw, int rh, int r, u32 c, u32 alpha) {
    r = min(r, min(rw, rh) / 2);
    if (r <= 0) { fill_alpha(x, y, rw, rh, c, alpha); return; }
    r = min(r, 32);
    for (int row = 0; row < rh; row++) {
        int ry = row < r ? row : row >= rh - r ? rh - 1 - row : -1;
        if (ry < 0) { fill_alpha(x, y + row, rw, 1, c, alpha); continue; }
        // the corner columns of this row
        for (int i = 0; i < r; i++) {
            int cv = rr_cov(i, row, rw, rh, r);
            if (!cv) continue;
            u32 a = cv * alpha / 255;
            mix_px(x + i, y + row, c, a);
            mix_px(x + rw - 1 - i, y + row, c, a);
        }
        fill_alpha(x + r, y + row, rw - 2 * r, 1, c, alpha);
    }
}

void Canvas::rrect_gradient(int x, int y, int rw, int rh, int r, u32 top, u32 bottom) {
    r = min(min(r, 32), min(rw, rh) / 2);
    for (int row = 0; row < rh; row++) {
        u32 c = mix(top, bottom, rh > 1 ? row * 255 / (rh - 1) : 0);
        int ry = row < r ? row : row >= rh - r ? rh - 1 - row : -1;
        if (ry < 0 || r <= 0) { fill(x, y + row, rw, 1, c); continue; }
        for (int i = 0; i < r; i++) {
            int cv = rr_cov(i, row, rw, rh, r);
            if (!cv) continue;
            mix_px(x + i, y + row, c, cv);
            mix_px(x + rw - 1 - i, y + row, c, cv);
        }
        fill(x + r, y + row, rw - 2 * r, 1, c);
    }
}

void Canvas::rrect_stroke(int x, int y, int rw, int rh, int r, u32 c, u32 alpha) {
    r = min(min(r, 32), min(rw, rh) / 2);
    for (int i = 0; i < rw; i++) {
        for (int row = 0; row < 2; row++) {
            int yy = row == 0 ? 0 : rh - 1;
            int outer = rr_cov(i, yy, rw, rh, r);
            int inner = rr_cov(i - 1, yy - 1, rw - 2, rh - 2, max(0, r - 1));
            int cv = outer - inner;
            if (cv > 0) mix_px(x + i, y + yy, c, cv * alpha / 255);
        }
    }
    for (int j = 1; j < rh - 1; j++) {
        for (int col = 0; col < 2; col++) {
            int xx = col == 0 ? 0 : rw - 1;
            int outer = rr_cov(xx, j, rw, rh, r);
            int inner = rr_cov(xx - 1, j - 1, rw - 2, rh - 2, max(0, r - 1));
            int cv = outer - inner;
            if (cv > 0) mix_px(x + xx, y + j, c, cv * alpha / 255);
        }
    }
}

void Canvas::shadow(int x, int y, int rw, int rh, int r, int spread, u32 alpha) {
    for (int d = 1; d <= spread; d++) {
        int w2 = rw + 2 * d, h2 = rh + 2 * d, r2 = r + d;
        int w1 = rw + 2 * (d - 1), h1 = rh + 2 * (d - 1), r1 = r + d - 1;
        u32 a = alpha * (u32)((spread - d + 1) * (spread - d + 1)) / (u32)(spread * spread);
        if (!a) continue;
        int x2 = x - d, y2 = y - d;
        auto at = [&](int px, int py) {                       // px, py relative to the ring's outer rect
            int o = rr_cov(px, py, w2, h2, r2);
            int i = rr_cov(px - 1, py - 1, w1, h1, r1);
            int cv = o - i;
            if (cv > 0) mix_px(x2 + px, y2 + py, 0xFF000000, cv * a / 255);
        };
        for (int i = 0; i < w2; i++) { at(i, 0); at(i, h2 - 1); }
        for (int j = 1; j < h2 - 1; j++) { at(0, j); at(w2 - 1, j); }
    }
}

void Canvas::disc(int cx, int cy, int r, u32 c, u32 alpha) { rrect(cx - r, cy - r, 2 * r, 2 * r, r, c, alpha); }

void Canvas::aa_line(float x0, float y0, float x1, float y1, float width, u32 c, u32 alpha) {
    float hw = width * 0.5f;
    int bx0 = (int)(min(x0, x1) - hw - 1), bx1 = (int)(max(x0, x1) + hw + 2);
    int by0 = (int)(min(y0, y1) - hw - 1), by1 = (int)(max(y0, y1) + hw + 2);
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int yy = by0; yy <= by1; yy++)
        for (int xx = bx0; xx <= bx1; xx++) {
            float px_ = xx + 0.5f - x0, py_ = yy + 0.5f - y0;
            float t = len2 > 0 ? (px_ * dx + py_ * dy) / len2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float ex = px_ - dx * t, ey = py_ - dy * t;
            float d = fsqrt(ex * ex + ey * ey);
            float cv = hw + 0.5f - d;
            if (cv <= 0) continue;
            if (cv > 1) cv = 1;
            mix_px(xx, yy, c, (u32)(cv * alpha));
        }
}

void Canvas::chevron(int cx, int cy, int size, bool right, u32 c) {
    float s = (float)size;
    if (right) { aa_line(cx - s * 0.4f, cy - s, cx + s * 0.5f, (float)cy, 1.6f, c); aa_line(cx + s * 0.5f, (float)cy, cx - s * 0.4f, cy + s, 1.6f, c); }
    else { aa_line(cx - s, cy - s * 0.4f, (float)cx, cy + s * 0.5f, 1.6f, c); aa_line((float)cx, cy + s * 0.5f, cx + s, cy - s * 0.4f, 1.6f, c); }
}

// ---------------------------------------------------------------- images
// .sil: "SIL1" u16 w u16 h u16 ncolors u16 reserved, palette[ncolors] u32 ARGB,
// then RLE over palette indices: byte t; t & 0x80 -> run of (t & 0x7F) + 1 copies
// of the next index byte, else (t + 1) literal index bytes follow.
bool image_decode(const u8* d, u32 len, Image& out) {
    if (len < 12 || d[0] != 'S' || d[1] != 'I' || d[2] != 'L' || d[3] != '1') return false;
    int w = d[4] | (d[5] << 8), h = d[6] | (d[7] << 8), nc = d[8] | (d[9] << 8);
    const u32* pal = (const u32*)(d + 12);
    const u8* p = d + 12 + nc * 4;
    const u8* end = d + len;
    out.w = w; out.h = h;
    out.px = (u32*)kmalloc((size_t)w * h * 4);
    u32 n = (u32)w * h, i = 0;
    while (i < n && p < end) {
        u8 t = *p++;
        if (t & 0x80) {
            u32 cnt = (t & 0x7F) + 1, c = pal[*p++];
            while (cnt-- && i < n) out.px[i++] = c;
        } else {
            u32 cnt = t + 1;
            while (cnt-- && i < n && p < end) out.px[i++] = pal[*p++];
        }
    }
    while (i < n) out.px[i++] = 0;
    return true;
}

Image image_new(int w, int h, u32 fill) {
    Image img;
    img.w = w; img.h = h;
    img.px = (u32*)kmalloc((size_t)w * h * 4);
    memset32(img.px, fill, (size_t)w * h);
    return img;
}

Image image_scale(const Image& src, int w, int h) {
    Image img = image_new(w, h, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            img.px[y * w + x] = src.px[(y * src.h / h) * src.w + x * src.w / w];
    return img;
}

void image_free(Image& img) {
    if (img.px) kfree(img.px);
    img.px = nullptr;
    img.w = img.h = 0;
}
