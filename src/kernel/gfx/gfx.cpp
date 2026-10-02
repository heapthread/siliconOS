// silicon - 2D drawing primitives
#include "gfx.h"

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
    if (dw == img.w && dh == img.h && alpha >= 255) { blit_raw(img.px, img.w, img.h, img.w, x, y); return; }
    Rect r = Rect{x + ox, y + oy, dw, dh}.intersect(clip);
    if (r.empty()) return;
    // 16.16 fixed point stepping
    u32 sx_step = (u32)(((u64)img.w << 16) / dw), sy_step = (u32)(((u64)img.h << 16) / dh);
    u32 sx0 = (u32)(r.x - (x + ox)) * sx_step;
    u32 sy = (u32)(r.y - (y + oy)) * sy_step;
    for (int yy = 0; yy < r.h; yy++, sy += sy_step) {
        const u32* srow = img.px + (sy >> 16) * img.w;
        u32* d = px + (r.y + yy) * pitch + r.x;
        u32 sx = sx0;
        if (alpha >= 255) for (int xx = 0; xx < r.w; xx++, sx += sx_step) d[xx] = srow[sx >> 16];
        else for (int xx = 0; xx < r.w; xx++, sx += sx_step) d[xx] = blend(d[xx], srow[sx >> 16], alpha);
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

int Canvas::draw_char(int x, int y, u8 c, u32 col, int scale, bool bold) {
    const u8* g = font_rows[c];
    if (scale == 1) {
        // fast path: whole glyph inside the clip -> write rows directly
        int ax = x + ox, ay = y + oy;
        if (ax >= clip.x && ay >= clip.y && ax + 9 <= clip.x + clip.w && ay + FONT_H <= clip.y + clip.h) {
            u32* p = px + ay * pitch + ax;
            for (int row = 0; row < FONT_H; row++, p += pitch) {
                u8 bits = g[row];
                if (bold) bits |= bits >> 1;
                if (!bits) continue;
                for (int b = 0; b < 8; b++) if (bits & (0x80 >> b)) p[b] = col;
            }
            return FONT_W;
        }
        if (ax + 9 <= clip.x || ay + FONT_H <= clip.y || ax >= clip.x + clip.w || ay >= clip.y + clip.h) return FONT_W;
    }
    for (int row = 0; row < FONT_H; row++) {
        u8 bits = g[row];
        if (bold) bits |= bits >> 1;
        if (!bits) continue;
        for (int b = 0; b < 8; b++) {
            if (!(bits & (0x80 >> b))) continue;
            if (scale == 1) put(x + b, y + row, col);
            else fill(x + b * scale, y + row * scale, scale, scale, col);
        }
    }
    return FONT_W * scale;
}

int Canvas::text_n(int x, int y, const char* s, int n, u32 col, int scale, bool bold) {
    int x0 = x;
    for (int i = 0; i < n && s[i]; i++) {
        if (s[i] == '\n') { x = x0; y += FONT_H * scale; continue; }
        x += draw_char(x, y, (u8)s[i], col, scale, bold);
    }
    return x - x0;
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

int text_width(const char* s, int scale, bool) {
    int n = 0, best = 0;
    for (; *s; s++) {
        if (*s == '\n') { best = max(best, n); n = 0; continue; }
        n++;
    }
    return max(best, n) * FONT_W * scale;
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
