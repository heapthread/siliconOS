// silicon - scalable fonts (see vfont.h)
#include "vfont.h"
#include "../pdf/pdf_gfx.h"
#include "../text.h"

struct Bitmap { int gid = -1; int size4 = 0; int w = 0, h = 0, left = 0, top = 0; u8* cov = nullptr; u32 used = 0; };

struct VFont {
    TTFont* tt = nullptr;
    int cmap_plat = 3, cmap_enc = 1;
    static const int CACHE = 768;
    Bitmap cache[CACHE];
    u32 clock = 0;
    float adv_cache[4096];
    bool adv_have[4096];
};

static VFont* fonts[4] = {nullptr, nullptr, nullptr, nullptr};
static bool tried[4] = {false, false, false, false};

VFont* vfont_get(bool bold, bool mono) {
    int k = (bold ? 1 : 0) + (mono ? 2 : 0);
    if (fonts[k] || tried[k]) return fonts[k];
    tried[k] = true;
    u32 len = 0;
    const u8* data = asset_find(mono ? (bold ? "font/DejaVuSansMono-Bold" : "font/DejaVuSansMono")
                                      : (bold ? "font/DejaVuSans-Bold" : "font/DejaVuSans"), &len);
    if (!data) return nullptr;
    TTFont* t = tt_open(data, len, 0);
    if (!t) return nullptr;
    VFont* f = new VFont();
    f->tt = t;
    if (tt_has_cmap(t, 3, 10)) { f->cmap_plat = 3; f->cmap_enc = 10; }
    else if (tt_has_cmap(t, 3, 1)) { f->cmap_plat = 3; f->cmap_enc = 1; }
    else if (tt_has_cmap(t, 0, 3)) { f->cmap_plat = 0; f->cmap_enc = 3; }
    memset(f->adv_have, 0, sizeof(f->adv_have));
    fonts[k] = f;
    return f;
}

int vfont_gid(VFont* f, u32 cp) {
    if (!f) return 0;
    int g = tt_cmap_lookup(f->tt, f->cmap_plat, f->cmap_enc, cp);
    if (!g && f->cmap_enc == 10) g = tt_cmap_lookup(f->tt, 3, 1, cp);
    return g;
}

float vfont_advance(VFont* f, int gid, float px) {
    if (!f) return px * 0.6f;
    if (gid >= 0 && gid < 4096) {
        if (!f->adv_have[gid]) { f->adv_cache[gid] = (float)tt_advance(f->tt, gid); f->adv_have[gid] = true; }
        return f->adv_cache[gid] * px;
    }
    return (float)tt_advance(f->tt, gid) * px;
}
float vfont_ascent(float px) { return px * 0.928f; }
float vfont_line_height(float px) { return px * 1.164f; }

// a sink that collects coverage into a byte bitmap
struct CovSink : Sink {
    u8* buf; int w;
    void row(int y, int x0, int x1, const u8* cov) override {
        for (int x = x0; x < x1; x++) buf[y * w + x] = cov[x - x0];
    }
};

static Bitmap* render_glyph(VFont* f, int gid, float px) {
    int size4 = (int)(px * 4 + 0.5f);
    Bitmap* slot = nullptr;
    u32 oldest = 0xFFFFFFFFu;
    int h0 = (gid * 31 + size4) & (VFont::CACHE - 1);
    for (int k = 0; k < 8; k++) {
        Bitmap& b = f->cache[(h0 + k) & (VFont::CACHE - 1)];
        if (b.gid == gid && b.size4 == size4) { b.used = ++f->clock; return &b; }
        if (b.gid < 0) { slot = &b; break; }
        if (b.used < oldest) { oldest = b.used; slot = &b; }
    }
    if (slot->cov) kfree(slot->cov);
    *slot = Bitmap();
    slot->gid = gid; slot->size4 = size4; slot->used = ++f->clock;
    Path p;
    if (!tt_outline(f->tt, gid, p) || !p.s.len) { p.free(); return slot; }
    float scale = size4 / 4.0f;
    // to pixels, y down
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < p.s.len; i++) {
        PSeg& g = p.s[i];
        int np = g.op == P_CURVE ? 3 : g.op == P_CLOSE ? 0 : 1;
        int base = g.op == P_CURVE ? 0 : 0;
        for (int k = 0; k < np; k++) {
            g.x[base + k * 2] *= scale;
            g.x[base + k * 2 + 1] *= -scale;
            minx = min(minx, g.x[base + k * 2]); maxx = max(maxx, g.x[base + k * 2]);
            miny = min(miny, g.x[base + k * 2 + 1]); maxy = max(maxy, g.x[base + k * 2 + 1]);
        }
    }
    int left = (int)(minx < 0 ? minx - 1 : minx), top = (int)(miny < 0 ? miny - 1 : miny);
    int w = (int)(maxx + 1.999f) - left + 1, hgt = (int)(maxy + 1.999f) - top + 1;
    if (w <= 0 || hgt <= 0 || w > 400 || hgt > 400) { p.free(); return slot; }
    for (int i = 0; i < p.s.len; i++) {
        PSeg& g = p.s[i];
        int np = g.op == P_CURVE ? 3 : g.op == P_CLOSE ? 0 : 1;
        for (int k = 0; k < np; k++) { g.x[k * 2] -= left; g.x[k * 2 + 1] -= top; }
    }
    slot->cov = (u8*)kcalloc_try((size_t)w * hgt);
    if (!slot->cov) { p.free(); return slot; }
    slot->w = w; slot->h = hgt; slot->left = left; slot->top = top;
    CovSink sink;
    sink.buf = slot->cov; sink.w = w;
    raster_fill(p, false, {0, 0, w, hgt}, sink);
    p.free();
    return slot;
}

bool vfont_draw_gid(Canvas& c, VFont* f, int gid, float px, float x, float baseline, u32 argb) {
    if (!f || gid <= 0) return false;
    Bitmap* b = render_glyph(f, gid, px);
    if (!b->cov) return false;
    int ox = (int)(x < 0 ? x - 0.5f : x + 0.5f) + b->left + c.ox;
    int oy = (int)(baseline < 0 ? baseline - 0.5f : baseline + 0.5f) + b->top + c.oy;
    u32 a0 = argb >> 24;
    u32 rgb = argb & 0xFFFFFF;
    for (int yy = 0; yy < b->h; yy++) {
        int dy = oy + yy;
        if (dy < c.clip.y || dy >= c.clip.y + c.clip.h) continue;
        u32* row = c.px + (size_t)dy * c.pitch;
        const u8* src = b->cov + yy * b->w;
        for (int xx = 0; xx < b->w; xx++) {
            u32 cov = src[xx];
            if (!cov) continue;
            int dx = ox + xx;
            if (dx < c.clip.x || dx >= c.clip.x + c.clip.w) continue;
            u32 a = cov * a0 / 255;
            if (a >= 255) row[dx] = 0xFF000000 | rgb;
            else row[dx] = blend(row[dx], rgb | 0xFF000000, a);
        }
    }
    return true;
}

float vfont_text_width_utf8(VFont* f, const char* s, int n, float px) {
    const char* end = s + n;
    float w = 0;
    while (s < end) {
        u32 cp = utf8_decode(s, end);
        w += vfont_advance(f, vfont_gid(f, cp), px);
    }
    return w;
}
