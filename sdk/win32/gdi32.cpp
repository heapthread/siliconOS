// win32run - gdi32: device contexts, pens, brushes, fonts, bitmaps and DIBs, lines and shapes, blits with
// raster operations, and text drawn with silicon's own TrueType fonts (DejaVu Sans / Sans Mono).
#include "gdi.h"

namespace rt {

static inline double fsqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }

// ---------------------------------------------------------------- the object table
static const int MAX_GDI = 16384;
static GdiObj* objs[MAX_GDI];
static Lock gdi_lock;
static const u64 GDI_BASE = 0x00800000;                   // handles look like 0x008xxxxx
static int next_free = 32;                                 // the first 32 are the stock objects

HGDIOBJ gdi_add(GdiObj* o) {
    Guard g(gdi_lock);
    for (int n = 0; n < MAX_GDI; n++) {
        int i = next_free;
        next_free = next_free + 1 >= MAX_GDI ? 32 : next_free + 1;
        if (!objs[i]) { objs[i] = o; o->handle = (HGDIOBJ)(GDI_BASE + (u64)i * 4); return o->handle; }
    }
    set_error(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
}
GdiObj* gdi_get(HGDIOBJ h, int type) {
    u64 v = (u64)h;
    if (v < GDI_BASE || (v & 3) || (v - GDI_BASE) / 4 >= MAX_GDI) return nullptr;
    GdiObj* o = objs[(v - GDI_BASE) / 4];
    if (!o || (type && o->type != type)) return nullptr;
    return o;
}
static void gdi_remove(GdiObj* o) {
    u64 i = ((u64)o->handle - GDI_BASE) / 4;
    if (i < MAX_GDI && objs[i] == o) objs[i] = nullptr;
}
DC* dc_get(HDC h) { return (DC*)gdi_get(h, G_DC); }

// ---------------------------------------------------------------- surfaces
static int mask_shift(u32 m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int mask_bits(u32 m) { int n = 0; m >>= mask_shift(m); while (m & 1) { n++; m >>= 1; } return n; }
static u32 chan_get(u32 v, u32 m) { if (!m) return 0; int s = mask_shift(m), b = mask_bits(m); u32 c = (v & m) >> s; return b >= 8 ? c >> (b - 8) : (c * 255 + ((1u << b) - 1) / 2) / ((1u << b) - 1); }
static u32 chan_put(u32 c, u32 m) { if (!m) return 0; int s = mask_shift(m), b = mask_bits(m); u32 v = b >= 8 ? c << (b - 8) : c >> (8 - b); return (v << s) & m; }

u32 surf_get(const Surf& s, int x, int y) {
    const u8* row = s.bits + (i64)y * s.stride;
    switch (s.bpp) {
    case 32: { u32 v = ((const u32*)row)[x]; if (s.rmask == 0xFF0000 && s.gmask == 0xFF00 && s.bmask == 0xFF) return v & 0xFFFFFF;
               return (chan_get(v, s.rmask) << 16) | (chan_get(v, s.gmask) << 8) | chan_get(v, s.bmask); }
    case 24: { const u8* p = row + x * 3; return ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0]; }
    case 16: { u32 v = ((const u16*)row)[x]; return (chan_get(v, s.rmask) << 16) | (chan_get(v, s.gmask) << 8) | chan_get(v, s.bmask); }
    case 8:  { u8 i = row[x]; return s.palette && i < s.ncolors ? s.palette[i] & 0xFFFFFF : (u32)i * 0x010101; }
    case 4:  { u8 i = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15; return s.palette && i < s.ncolors ? s.palette[i] & 0xFFFFFF : (i & 8 ? 0xFFFFFF : 0); }
    case 1:  { u8 i = (row[x >> 3] >> (7 - (x & 7))) & 1; return s.palette && s.ncolors >= 2 ? s.palette[i] & 0xFFFFFF : (i ? 0xFFFFFF : 0); }
    }
    return 0;
}
static int nearest_index(const Surf& s, u32 rgb) {
    int best = 0;
    u32 bd = ~0u;
    for (int i = 0; i < s.ncolors; i++) {
        u32 p = s.palette[i];
        int dr = (int)((p >> 16) & 255) - (int)((rgb >> 16) & 255), dg = (int)((p >> 8) & 255) - (int)((rgb >> 8) & 255), db = (int)(p & 255) - (int)(rgb & 255);
        u32 d = (u32)(dr * dr + dg * dg + db * db);
        if (d < bd) { bd = d; best = i; if (!d) break; }
    }
    return best;
}
void surf_put(Surf& s, int x, int y, u32 rgb) {
    u8* row = s.bits + (i64)y * s.stride;
    switch (s.bpp) {
    case 32: if (s.rmask == 0xFF0000 && s.gmask == 0xFF00 && s.bmask == 0xFF) ((u32*)row)[x] = rgb;
             else ((u32*)row)[x] = chan_put((rgb >> 16) & 255, s.rmask) | chan_put((rgb >> 8) & 255, s.gmask) | chan_put(rgb & 255, s.bmask);
             break;
    case 24: { u8* p = row + x * 3; p[0] = (u8)rgb; p[1] = (u8)(rgb >> 8); p[2] = (u8)(rgb >> 16); break; }
    case 16: ((u16*)row)[x] = (u16)(chan_put((rgb >> 16) & 255, s.rmask) | chan_put((rgb >> 8) & 255, s.gmask) | chan_put(rgb & 255, s.bmask)); break;
    case 8: row[x] = (u8)(s.palette ? nearest_index(s, rgb) : (((rgb >> 16) & 255) * 30 + ((rgb >> 8) & 255) * 59 + (rgb & 255) * 11) / 100); break;
    case 4: { int i = s.palette ? nearest_index(s, rgb) : 0; u8* b = &row[x >> 1]; if (x & 1) *b = (u8)((*b & 0xF0) | i); else *b = (u8)((*b & 0x0F) | (i << 4)); break; }
    case 1: {
        int i;
        if (s.palette && s.ncolors >= 2) i = nearest_index(s, rgb);
        else i = (((rgb >> 16) & 255) + ((rgb >> 8) & 255) + (rgb & 255)) >= 384 ? 1 : 0;
        u8* b = &row[x >> 3];
        u8 bit = (u8)(0x80 >> (x & 7));
        *b = i ? (u8)(*b | bit) : (u8)(*b & ~bit);
        break;
    }
    }
}
static inline bool fast32(const Surf& s) { return s.bpp == 32 && s.rmask == 0xFF0000 && s.gmask == 0xFF00 && s.bmask == 0xFF; }

// ---------------------------------------------------------------- raster operations
static inline u32 rop2_apply(int r, u32 d, u32 p) {
    switch (r) {
    case 13: return p;
    case 7: return d ^ p;
    case 6: return ~d & 0xFFFFFF;
    case 11: return d;
    case 1: return 0;
    case 16: return 0xFFFFFF;
    }
    u32 t = (u32)(r - 1) & 15, res = 0;
    if (t & 8) res |= p & d;
    if (t & 4) res |= p & ~d;
    if (t & 2) res |= ~p & d;
    if (t & 1) res |= ~p & ~d;
    return res & 0xFFFFFF;
}
static inline u32 rop3_apply(u32 rop, u32 p, u32 s, u32 d) {
    switch (rop) {
    case 0xCC0020: return s;                 // SRCCOPY
    case 0xF00021: return p;                 // PATCOPY
    case 0x000042: return 0;                 // BLACKNESS
    case 0xFF0062: return 0xFFFFFF;          // WHITENESS
    case 0xEE0086: return s | d;             // SRCPAINT
    case 0x8800C6: return s & d;             // SRCAND
    case 0x660046: return s ^ d;             // SRCINVERT
    case 0x550009: return ~d & 0xFFFFFF;     // DSTINVERT
    case 0x330008: return ~s & 0xFFFFFF;     // NOTSRCCOPY
    case 0x5A0049: return p ^ d;             // PATINVERT
    case 0xAA0029: return d;                 // NOP
    }
    u32 t = (rop >> 16) & 255, res = 0;
    for (int i = 0; i < 8; i++) {
        if (!((t >> i) & 1)) continue;
        u32 v = ((i & 4) ? p : ~p) & ((i & 2) ? s : ~s) & ((i & 1) ? d : ~d);
        res |= v;
    }
    return res & 0xFFFFFF;
}
static bool rop_uses_src(u32 rop) { u32 t = (rop >> 16) & 255; return ((t >> 2) & 0x33) != (t & 0x33); }
static bool rop_uses_dst(u32 rop) { u32 t = (rop >> 16) & 255; return ((t >> 1) & 0x55) != (t & 0x55); }

// ---------------------------------------------------------------- stock objects and DC state
static Brush* new_brush(int style, COLORREF c, int hatch) { Brush* b = (Brush*)izalloc(sizeof(Brush)); b->type = G_BRUSH; b->style = style; b->color = c; b->hatch = hatch; return b; }
static Pen* new_pen(int style, int width, COLORREF c) { Pen* p = (Pen*)izalloc(sizeof(Pen)); p->type = G_PEN; p->style = style; p->width = width; p->color = c; return p; }
static void font_measure(Font* f);
static Font* new_font(const LOGFONTW* lf) {
    Font* f = (Font*)izalloc(sizeof(Font));
    f->type = G_FONT;
    f->lf = *lf;
    int h = lf->lfHeight;
    // negative: character (em) height; positive: cell height, which includes the internal leading
    float px = h < 0 ? (float)-h : h > 0 ? (float)h / 1.164f : 13.0f;
    if (px < 4) px = 4;
    f->px100 = (int)(px * 100);
    f->bold = lf->lfWeight >= 600;
    char face[64];
    utf8_from_wide(lf->lfFaceName, -1, face, sizeof(face));
    f->mono = (lf->lfPitchAndFamily & 3) == 1 || (lf->lfPitchAndFamily & 0xF0) == 0x30 || sfind(face, "Courier") || sfind(face, "Consol") ||
              sfind(face, "Lucida Console") || sfind(face, "Terminal") || sfind(face, "Fixedsys") || sfind(face, "Mono");
    font_measure(f);
    return f;
}
static bool stock_ready = false;
static void stock_init() {
    if (stock_ready) return;
    stock_ready = true;
    auto put = [](int i, GdiObj* o) { o->stock = true; objs[i] = o; o->handle = (HGDIOBJ)(GDI_BASE + (u64)i * 4); };
    put(0, new_brush(0, 0xFFFFFF, 0));      // WHITE_BRUSH
    put(1, new_brush(0, 0xC0C0C0, 0));      // LTGRAY_BRUSH
    put(2, new_brush(0, 0x808080, 0));      // GRAY_BRUSH
    put(3, new_brush(0, 0x404040, 0));      // DKGRAY_BRUSH
    put(4, new_brush(0, 0x000000, 0));      // BLACK_BRUSH
    put(5, new_brush(1, 0, 0));             // NULL_BRUSH
    put(6, new_pen(0, 0, 0xFFFFFF));        // WHITE_PEN
    put(7, new_pen(0, 0, 0x000000));        // BLACK_PEN
    put(8, new_pen(5, 0, 0));               // NULL_PEN
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -12; lf.lfWeight = 400;
    wide_from_utf8("Courier New", -1, lf.lfFaceName, 32);
    lf.lfPitchAndFamily = 0x31;
    put(10, new_font(&lf));                 // OEM_FIXED_FONT
    put(11, new_font(&lf));                 // ANSI_FIXED_FONT
    lf.lfPitchAndFamily = 0x22;
    wide_from_utf8("Arial", -1, lf.lfFaceName, 32);
    put(12, new_font(&lf));                 // ANSI_VAR_FONT
    lf.lfHeight = 16; lf.lfWeight = 700;
    wide_from_utf8("System", -1, lf.lfFaceName, 32);
    put(13, new_font(&lf));                 // SYSTEM_FONT
    lf.lfHeight = -12; lf.lfWeight = 400;
    wide_from_utf8("Segoe UI", -1, lf.lfFaceName, 32);
    put(14, new_font(&lf));                 // DEVICE_DEFAULT_FONT
    Palette* pal = (Palette*)izalloc(sizeof(Palette));
    pal->type = G_PALETTE;
    static const u32 sys20[20] = {0, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0, 0xC0DCC0, 0xA6CAF0,
                                  0xFFFBF0, 0xA0A0A4, 0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};
    for (int i = 0; i < 20; i++) pal->entries[i] = sys20[i];
    pal->n = 20;
    put(15, pal);                           // DEFAULT_PALETTE
    lf.lfWeight = 700; lf.lfHeight = 16;
    wide_from_utf8("Fixedsys", -1, lf.lfFaceName, 32);
    lf.lfPitchAndFamily = 0x31;
    put(16, new_font(&lf));                 // SYSTEM_FIXED_FONT
    lf.lfWeight = 400; lf.lfHeight = -11; lf.lfPitchAndFamily = 0x22;
    wide_from_utf8("MS Shell Dlg", -1, lf.lfFaceName, 32);
    put(17, new_font(&lf));                 // DEFAULT_GUI_FONT
    put(18, new_brush(0, 0xFFFFFF, 0));     // DC_BRUSH (its colour comes from the DC)
    put(19, new_pen(0, 0, 0));              // DC_PEN
    Bitmap* mono = (Bitmap*)izalloc(sizeof(Bitmap));
    mono->type = G_BITMAP;
    static u8 one_px[4] = {0};
    static u32 bw[2] = {0, 0xFFFFFF};
    mono->s.bits = one_px; mono->s.w = 1; mono->s.h = 1; mono->s.stride = 4; mono->s.bpp = 1; mono->s.palette = bw; mono->s.ncolors = 2;
    put(20, mono);                          // the default bitmap of a memory DC
}
HGDIOBJ stock_object(int i) { stock_init(); return i >= 0 && i < 21 && objs[i] ? objs[i]->handle : nullptr; }
Font* font_default() { stock_init(); return (Font*)objs[13]; }

HDC dc_new(int kind, Wnd* wnd) {
    stock_init();
    DC* dc = (DC*)izalloc(sizeof(DC));
    dc->type = G_DC;
    dc->kind = kind;
    dc->wnd = wnd;
    DCState& s = dc->st;
    s.brush = (Brush*)objs[0];
    s.pen = (Pen*)objs[7];
    s.font = (Font*)objs[13];
    s.bmp = kind == DCK_MEMORY ? (Bitmap*)objs[20] : nullptr;
    s.pal = (Palette*)objs[15];
    s.text_color = 0;
    s.bk_color = 0xFFFFFF;
    s.dc_brush = 0xFFFFFF;
    s.dc_pen = 0;
    s.bk_mode = 2;                          // OPAQUE
    s.rop2 = 13;                            // R2_COPYPEN
    s.stretch_mode = 1;
    s.map_mode = 1;
    s.poly_mode = 1;                        // ALTERNATE
    return gdi_add(dc);
}
void dc_free(DC* dc) { gdi_remove(dc); ifree(dc); }

bool dc_target(DC* dc, Surf* s, int* ox, int* oy, RECT* clip) {
    int bx = 0, by = 0;
    RECT c;
    if (dc->kind == DCK_MEMORY) {
        if (!dc->st.bmp) return false;
        *s = dc->st.bmp->s;
        c = {0, 0, s->w, s->h};
    } else if (dc->kind == DCK_WINDOW && dc->wnd && !dc->wnd->destroyed) {
        Wnd* top = wnd_top(dc->wnd);
        if (!top || !top->surf.bits) return false;
        *s = top->surf;
        wnd_client_origin(dc->wnd, &bx, &by);
        c = {bx, by, bx + dc->wnd->cw, by + dc->wnd->ch};
        // a child is also clipped by its parents
        for (Wnd* p = dc->wnd->parent; p; p = p->parent) {
            int px, py;
            wnd_client_origin(p, &px, &py);
            if (c.left < px) c.left = px;
            if (c.top < py) c.top = py;
            if (c.right > px + p->cw) c.right = px + p->cw;
            if (c.bottom > py + p->ch) c.bottom = py + p->ch;
        }
        if (dc->paint) {
            if (c.left < bx + dc->paint_rect.left) c.left = bx + dc->paint_rect.left;
            if (c.top < by + dc->paint_rect.top) c.top = by + dc->paint_rect.top;
            if (c.right > bx + dc->paint_rect.right) c.right = bx + dc->paint_rect.right;
            if (c.bottom > by + dc->paint_rect.bottom) c.bottom = by + dc->paint_rect.bottom;
        }
        if (c.right > s->w) c.right = s->w;
        if (c.bottom > s->h) c.bottom = s->h;
        dc->wnd->dirty = true;
        top->dirty = true;
    } else return false;
    if (dc->st.has_clip) {
        RECT u = dc->st.clip;
        u.left += bx; u.right += bx; u.top += by; u.bottom += by;
        if (c.left < u.left) c.left = u.left;
        if (c.top < u.top) c.top = u.top;
        if (c.right > u.right) c.right = u.right;
        if (c.bottom > u.bottom) c.bottom = u.bottom;
    }
    if (c.left < 0) c.left = 0;
    if (c.top < 0) c.top = 0;
    *clip = c;
    *ox = bx - dc->st.win_org.x + dc->st.vp_org.x;
    *oy = by - dc->st.win_org.y + dc->st.vp_org.y;
    return c.right > c.left && c.bottom > c.top;
}

// ---------------------------------------------------------------- filling
static const u8 hatch_bits[6][8] = {
    {0, 0, 0, 0xFF, 0, 0, 0, 0}, {0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08}, {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01},
    {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80}, {0x08, 0x08, 0x08, 0xFF, 0x08, 0x08, 0x08, 0x08}, {0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81},
};
// the brush colour at a device pixel; *skip for transparent hatch gaps
static inline u32 brush_px(DC* dc, Brush* b, int x, int y, bool* skip) {
    *skip = false;
    if (b == (Brush*)objs[18]) return rgb_of(dc->st.dc_brush);
    if (b->style == 0) return rgb_of(b->color);
    if (b->style == 2) {
        bool on = (hatch_bits[b->hatch % 6][y & 7] >> (7 - (x & 7))) & 1;
        if (on) return rgb_of(b->color);
        if (dc->st.bk_mode == 1) { *skip = true; return 0; }
        return rgb_of(dc->st.bk_color);
    }
    if (b->style == 3 && b->pattern && b->pattern->s.w && b->pattern->s.h) {
        const Surf& p = b->pattern->s;
        int px = ((x - dc->st.brush_org.x) % p.w + p.w) % p.w, py = ((y - dc->st.brush_org.y) % p.h + p.h) % p.h;
        u32 c = surf_get(p, px, py);
        if (p.bpp == 1) c = c ? rgb_of(dc->st.bk_color) : rgb_of(dc->st.text_color);
        return c;
    }
    *skip = true;
    return 0;
}
static void span(DC* dc, Surf& s, const RECT& clip, int x0, int x1, int y, u32 color, int rop) {
    if (y < clip.top || y >= clip.bottom) return;
    if (x0 < clip.left) x0 = clip.left;
    if (x1 > clip.right) x1 = clip.right;
    if (x0 >= x1) return;
    (void)dc;
    if (fast32(s)) {
        u32* r = row32(s, y);
        if (rop == 13) { for (int x = x0; x < x1; x++) r[x] = color; }
        else for (int x = x0; x < x1; x++) r[x] = rop2_apply(rop, r[x] & 0xFFFFFF, color);
    } else for (int x = x0; x < x1; x++) surf_put(s, x, y, rop == 13 ? color : rop2_apply(rop, surf_get(s, x, y), color));
}
static void brush_span(DC* dc, Surf& s, const RECT& clip, Brush* b, int x0, int x1, int y, int rop) {
    if (!b || b->style == 1) return;
    if (b->style == 0 || b == (Brush*)objs[18]) { bool sk; span(dc, s, clip, x0, x1, y, brush_px(dc, b, x0, y, &sk), rop); return; }
    if (y < clip.top || y >= clip.bottom) return;
    if (x0 < clip.left) x0 = clip.left;
    if (x1 > clip.right) x1 = clip.right;
    for (int x = x0; x < x1; x++) {
        bool skip;
        u32 c = brush_px(dc, b, x, y, &skip);
        if (!skip) surf_put(s, x, y, rop == 13 ? c : rop2_apply(rop, surf_get(s, x, y), c));
    }
}
void fill_rect_dc(DC* dc, const RECT* r, Brush* b) {
    Surf s; int ox, oy; RECT clip;
    if (!b || !dc_target(dc, &s, &ox, &oy, &clip)) return;
    for (int y = r->top + oy; y < r->bottom + oy; y++) brush_span(dc, s, clip, b, r->left + ox, r->right + ox, y, 13);
}

// ---------------------------------------------------------------- lines (Bresenham, wide pens as squares)
static void plot(DC* dc, Surf& s, const RECT& clip, int x, int y, u32 c, int w) {
    if (w <= 1) {
        if (x < clip.left || y < clip.top || x >= clip.right || y >= clip.bottom) return;
        int rop = dc->st.rop2;
        if (fast32(s)) { u32* p = &row32(s, y)[x]; *p = rop == 13 ? c : rop2_apply(rop, *p & 0xFFFFFF, c); }
        else surf_put(s, x, y, rop == 13 ? c : rop2_apply(rop, surf_get(s, x, y), c));
        return;
    }
    int h = w / 2;
    for (int yy = y - h; yy < y - h + w; yy++) span(dc, s, clip, x - h, x - h + w, yy, c, dc->st.rop2);
}
static void line_dev(DC* dc, Surf& s, const RECT& clip, int x0, int y0, int x1, int y1, Pen* pen, bool last) {
    if (!pen || pen->style == 5) return;
    u32 c = pen == (Pen*)objs[19] ? rgb_of(dc->st.dc_pen) : rgb_of(pen->color);
    int w = pen->width < 1 ? 1 : pen->width;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    static const u32 dash[5] = {0xFFFFFFFF, 0xFFFFFF00, 0xCCCCCCCC, 0xFFF0F0F0, 0xFFF0CCF0};     // solid, dash, dot, dashdot, dashdotdot
    u32 pattern = pen->style >= 0 && pen->style <= 4 && w == 1 ? dash[pen->style] : 0xFFFFFFFF;
    int n = 0;
    for (;;) {
        if (x0 == x1 && y0 == y1 && !last) break;
        if ((pattern >> (31 - (n & 31))) & 1) plot(dc, s, clip, x0, y0, c, w);
        n++;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// ---------------------------------------------------------------- polygons (scanline fill)
static void fill_poly_dev(DC* dc, Surf& s, const RECT& clip, const POINT* pts, int n, Brush* b, bool winding) {
    if (!b || b->style == 1 || n < 3) return;
    int ymin = pts[0].y, ymax = pts[0].y;
    for (int i = 1; i < n; i++) { if (pts[i].y < ymin) ymin = pts[i].y; if (pts[i].y > ymax) ymax = pts[i].y; }
    if (ymin < clip.top) ymin = clip.top;
    if (ymax > clip.bottom) ymax = clip.bottom;
    struct X { int x; int dir; };
    X* xs = (X*)ialloc(sizeof(X) * (usize)n);
    for (int y = ymin; y < ymax; y++) {
        int k = 0;
        double fy = y + 0.5;
        for (int i = 0; i < n; i++) {
            const POINT& a = pts[i];
            const POINT& c = pts[(i + 1) % n];
            if (a.y == c.y) continue;
            bool up = a.y < c.y;
            int y0 = up ? a.y : c.y, y1 = up ? c.y : a.y;
            if (fy < y0 || fy >= y1) continue;
            double x = a.x + (fy - a.y) * (c.x - a.x) / (double)(c.y - a.y);
            xs[k].x = (int)(x + 0.5);
            xs[k].dir = up ? 1 : -1;
            k++;
        }
        for (int i = 1; i < k; i++) { X t = xs[i]; int j = i - 1; while (j >= 0 && xs[j].x > t.x) { xs[j + 1] = xs[j]; j--; } xs[j + 1] = t; }
        if (!winding) for (int i = 0; i + 1 < k; i += 2) brush_span(dc, s, clip, b, xs[i].x, xs[i + 1].x, y, 13);
        else {
            int wn = 0;
            for (int i = 0; i + 1 < k; i++) { wn += xs[i].dir; if (wn) brush_span(dc, s, clip, b, xs[i].x, xs[i + 1].x, y, 13); }
        }
    }
    ifree(xs);
}

double c_cos_d(double x);
double c_sin_d(double x);
// points of an ellipse inscribed in [l, r) x [t, b), from start to end angle (radians, counter-clockwise)
static int ellipse_points(int l, int t, int r, int b, double a0, double a1, POINT* out, int max) {
    double cx = (l + r - 1) / 2.0, cy = (t + b - 1) / 2.0, rx = (r - l - 1) / 2.0, ry = (b - t - 1) / 2.0;
    double span = a1 - a0;
    int steps = (int)((rx + ry) * 0.5 * (span < 0 ? -span : span)) + 8;
    if (steps > max - 1) steps = max - 1;
    for (int i = 0; i <= steps; i++) {
        double a = a0 + span * i / steps;
        out[i].x = (int)(cx + rx * c_cos_d(a) + (cx + rx * c_cos_d(a) >= 0 ? 0.5 : -0.5));
        out[i].y = (int)(cy - ry * c_sin_d(a) + (cy - ry * c_sin_d(a) >= 0 ? 0.5 : -0.5));
    }
    return steps + 1;
}
double c_cos_d(double x) { long double r; __asm__("fcos" : "=t"(r) : "0"((long double)x)); return (double)r; }
double c_sin_d(double x) { long double r; __asm__("fsin" : "=t"(r) : "0"((long double)x)); return (double)r; }
static double c_atan2_d(double y, double x) { long double r; __asm__("fpatan" : "=t"(r) : "0"((long double)x), "u"((long double)y) : "st(1)"); return (double)r; }

// ---------------------------------------------------------------- the DC API
static BOOL WINAPI GdiFlush() { present_all(); return TRUE; }
static HDC WINAPI CreateCompatibleDC(HDC h) { (void)h; return dc_new(DCK_MEMORY, nullptr); }
static HDC WINAPI CreateDCW(const WCHAR* drv, const WCHAR* dev, const WCHAR* out, const void* init) { (void)drv; (void)dev; (void)out; (void)init; return dc_new(DCK_SCREEN, nullptr); }
static HDC WINAPI CreateDCA(const char* drv, const char* dev, const char* out, const void* init) { (void)drv; (void)dev; (void)out; (void)init; return dc_new(DCK_SCREEN, nullptr); }
static HDC WINAPI CreateICW(const WCHAR* drv, const WCHAR* dev, const WCHAR* out, const void* init) { return CreateDCW(drv, dev, out, init); }
static BOOL WINAPI DeleteDC(HDC h) {
    DC* dc = dc_get(h);
    if (!dc) return FALSE;
    if (dc->st.bmp && !dc->st.bmp->stock) dc->st.bmp->selected_in = nullptr;
    if (dc->wnd && dc->wnd->own_dc == dc) dc->wnd->own_dc = nullptr;
    dc_free(dc);
    return TRUE;
}
static int WINAPI SaveDC(HDC h) { DC* dc = dc_get(h); if (!dc || dc->nsaved >= 16) return 0; dc->saved[dc->nsaved++] = dc->st; return dc->nsaved; }
static BOOL WINAPI RestoreDC(HDC h, int n) {
    DC* dc = dc_get(h);
    if (!dc) return FALSE;
    int idx = n < 0 ? dc->nsaved + n : n - 1;
    if (idx < 0 || idx >= dc->nsaved) return FALSE;
    dc->st = dc->saved[idx];
    dc->nsaved = idx;
    return TRUE;
}

static HGDIOBJ WINAPI GetStockObject(int i) { return stock_object(i); }
static HGDIOBJ WINAPI SelectObject(HDC h, HGDIOBJ o) {
    DC* dc = dc_get(h);
    GdiObj* g = gdi_get(o);
    if (!dc || !g) { set_error(ERROR_INVALID_HANDLE); return nullptr; }
    HGDIOBJ old = nullptr;
    switch (g->type) {
    case G_BRUSH: old = dc->st.brush->handle; dc->st.brush = (Brush*)g; break;
    case G_PEN: old = dc->st.pen->handle; dc->st.pen = (Pen*)g; break;
    case G_FONT: old = dc->st.font->handle; dc->st.font = (Font*)g; break;
    case G_BITMAP: {
        if (dc->kind != DCK_MEMORY) return nullptr;
        Bitmap* b = (Bitmap*)g;
        old = dc->st.bmp ? dc->st.bmp->handle : nullptr;
        if (dc->st.bmp) dc->st.bmp->selected_in = nullptr;
        dc->st.bmp = b;
        b->selected_in = dc;
        break;
    }
    case G_REGION: {
        Region* r = (Region*)g;
        dc->st.has_clip = true;
        dc->st.clip = r->r;
        return (HGDIOBJ)(u64)2;                                 // SIMPLEREGION
    }
    case G_PALETTE: old = dc->st.pal->handle; dc->st.pal = (Palette*)g; break;
    default: return nullptr;
    }
    return old;
}
static BOOL WINAPI DeleteObject(HGDIOBJ o) {
    GdiObj* g = gdi_get(o);
    if (!g) return FALSE;
    if (g->stock) return TRUE;
    if (g->type == G_BITMAP) {
        Bitmap* b = (Bitmap*)g;
        if (b->selected_in) return FALSE;                       // Windows refuses while it is selected
        if (b->own_bits) vm_free(b->s.bits - (b->s.stride < 0 ? (i64)(b->s.h - 1) * -b->s.stride : 0), 0, MEM_RELEASE);
        if (b->s.palette) ifree(b->s.palette);
    }
    if (g->type == G_DC) return FALSE;
    gdi_remove(g);
    ifree(g);
    return TRUE;
}
static HGDIOBJ WINAPI GetCurrentObject(HDC h, u32 type) {
    DC* dc = dc_get(h);
    if (!dc) return nullptr;
    switch (type) {
    case 1: return dc->st.pen->handle;
    case 2: return dc->st.brush->handle;
    case 5: return dc->st.pal->handle;
    case 6: return dc->st.font->handle;
    case 7: return dc->st.bmp ? dc->st.bmp->handle : nullptr;
    }
    return nullptr;
}
struct LOGBRUSH { u32 lbStyle; COLORREF lbColor; u64 lbHatch; };
struct LOGPEN { u32 lopnStyle; POINT lopnWidth; COLORREF lopnColor; };
struct BITMAP_ { i32 bmType, bmWidth, bmHeight, bmWidthBytes; u16 bmPlanes, bmBitsPixel; u32 pad; void* bmBits; };
struct DIBSECTION { BITMAP_ dsBm; u8 dsBmih[40]; u32 dsBitfields[3]; HANDLE dshSection; u32 dsOffset; };
static int obj_info(HGDIOBJ o, int n, void* out, bool wide) {
    GdiObj* g = gdi_get(o);
    if (!g) return 0;
    switch (g->type) {
    case G_BRUSH: { Brush* b = (Brush*)g; LOGBRUSH lb = {(u32)b->style, b->color, (u64)b->hatch}; if (!out) return sizeof(lb); memcpy(out, &lb, (usize)(n < (int)sizeof(lb) ? n : (int)sizeof(lb))); return n < (int)sizeof(lb) ? n : (int)sizeof(lb); }
    case G_PEN: { Pen* p = (Pen*)g; LOGPEN lp = {(u32)p->style, {p->width, 0}, p->color}; if (!out) return sizeof(lp); memcpy(out, &lp, (usize)(n < (int)sizeof(lp) ? n : (int)sizeof(lp))); return n < (int)sizeof(lp) ? n : (int)sizeof(lp); }
    case G_FONT: {
        Font* f = (Font*)g;
        if (wide) { int k = n < (int)sizeof(LOGFONTW) ? n : (int)sizeof(LOGFONTW); if (!out) return sizeof(LOGFONTW); memcpy(out, &f->lf, (usize)k); return k; }
        u8 a[60];
        memcpy(a, &f->lf, 28);
        utf8_from_wide(f->lf.lfFaceName, -1, (char*)a + 28, 32);
        if (!out) return 60;
        int k = n < 60 ? n : 60;
        memcpy(out, a, (usize)k);
        return k;
    }
    case G_BITMAP: {
        Bitmap* b = (Bitmap*)g;
        BITMAP_ bm;
        memset(&bm, 0, sizeof(bm));
        bm.bmWidth = b->s.w; bm.bmHeight = b->s.h;
        bm.bmWidthBytes = ((b->s.w * b->s.bpp + 15) / 16) * 2;
        bm.bmPlanes = 1; bm.bmBitsPixel = (u16)b->s.bpp;
        bm.bmBits = b->dib ? (b->s.stride < 0 ? b->s.bits + (i64)(b->s.h - 1) * b->s.stride : b->s.bits) : nullptr;
        if (b->dib) bm.bmWidthBytes = b->s.stride < 0 ? -b->s.stride : b->s.stride;
        if (!out) return sizeof(BITMAP_);
        if (b->dib && n >= (int)sizeof(DIBSECTION)) {
            DIBSECTION ds;
            memset(&ds, 0, sizeof(ds));
            ds.dsBm = bm;
            memcpy(ds.dsBmih, b->bih, 40);
            ds.dsBitfields[0] = b->s.rmask; ds.dsBitfields[1] = b->s.gmask; ds.dsBitfields[2] = b->s.bmask;
            memcpy(out, &ds, sizeof(ds));
            return sizeof(ds);
        }
        int k = n < (int)sizeof(bm) ? n : (int)sizeof(bm);
        memcpy(out, &bm, (usize)k);
        return k;
    }
    case G_PALETTE: if (out && n >= 2) *(u16*)out = (u16)((Palette*)g)->n; return 2;
    }
    return 0;
}
static int WINAPI GetObjectW(HGDIOBJ o, int n, void* out) { return obj_info(o, n, out, true); }
static int WINAPI GetObjectA(HGDIOBJ o, int n, void* out) { return obj_info(o, n, out, false); }
static u32 WINAPI GetObjectType(HGDIOBJ o) {
    GdiObj* g = gdi_get(o);
    if (!g) return 0;
    static const u32 t[] = {0, 2, 1, 6, 7, 3, 8, 5};           // OBJ_BRUSH, OBJ_PEN, OBJ_FONT, OBJ_BITMAP, OBJ_DC, OBJ_REGION, OBJ_PAL
    if (g->type == G_DC) return ((DC*)g)->kind == DCK_MEMORY ? 10 : 3;   // OBJ_MEMDC, OBJ_DC
    return t[g->type];
}

static HANDLE WINAPI CreateSolidBrush(COLORREF c) { return gdi_add(new_brush(0, c & 0xFFFFFF, 0)); }
static HANDLE WINAPI CreateHatchBrush(int h, COLORREF c) { return gdi_add(new_brush(2, c & 0xFFFFFF, h)); }
static HANDLE WINAPI CreatePatternBrush(HANDLE bmp) {
    Bitmap* b = (Bitmap*)gdi_get(bmp, G_BITMAP);
    if (!b) return nullptr;
    Brush* br = new_brush(3, 0, 0);
    br->pattern = b;
    return gdi_add(br);
}
static HANDLE WINAPI CreateBrushIndirect(const LOGBRUSH* lb) {
    if (lb->lbStyle == 1) return stock_object(5);
    if (lb->lbStyle == 3) return CreatePatternBrush((HANDLE)lb->lbHatch);
    return gdi_add(new_brush((int)lb->lbStyle, lb->lbColor & 0xFFFFFF, (int)lb->lbHatch));
}
static HANDLE WINAPI CreateDIBPatternBrushPt(const void* dib, u32 usage);
static HANDLE WINAPI CreatePen(int style, int width, COLORREF c) { return gdi_add(new_pen(style & 15, width, c & 0xFFFFFF)); }
static HANDLE WINAPI CreatePenIndirect(const LOGPEN* lp) { return CreatePen((int)lp->lopnStyle, lp->lopnWidth.x, lp->lopnColor); }
static HANDLE WINAPI ExtCreatePen(u32 style, u32 width, const LOGBRUSH* lb, u32 n, const u32* st) { (void)n; (void)st; return CreatePen((int)(style & 15), (int)width, lb ? lb->lbColor : 0); }
static COLORREF WINAPI SetDCBrushColor(HDC h, COLORREF c) { DC* dc = dc_get(h); if (!dc) return 0xFFFFFFFF; COLORREF o = dc->st.dc_brush; dc->st.dc_brush = c; return o; }
static COLORREF WINAPI SetDCPenColor(HDC h, COLORREF c) { DC* dc = dc_get(h); if (!dc) return 0xFFFFFFFF; COLORREF o = dc->st.dc_pen; dc->st.dc_pen = c; return o; }
static COLORREF WINAPI GetDCBrushColor(HDC h) { DC* dc = dc_get(h); return dc ? dc->st.dc_brush : 0xFFFFFFFF; }
static COLORREF WINAPI GetDCPenColor(HDC h) { DC* dc = dc_get(h); return dc ? dc->st.dc_pen : 0xFFFFFFFF; }
static COLORREF WINAPI SetTextColor(HDC h, COLORREF c) { DC* dc = dc_get(h); if (!dc) return 0xFFFFFFFF; COLORREF o = dc->st.text_color; dc->st.text_color = c & 0xFFFFFF; return o; }
static COLORREF WINAPI GetTextColor(HDC h) { DC* dc = dc_get(h); return dc ? dc->st.text_color : 0; }
static COLORREF WINAPI SetBkColor(HDC h, COLORREF c) { DC* dc = dc_get(h); if (!dc) return 0xFFFFFFFF; COLORREF o = dc->st.bk_color; dc->st.bk_color = c & 0xFFFFFF; return o; }
static COLORREF WINAPI GetBkColor(HDC h) { DC* dc = dc_get(h); return dc ? dc->st.bk_color : 0; }
static int WINAPI SetBkMode(HDC h, int m) { DC* dc = dc_get(h); if (!dc) return 0; int o = dc->st.bk_mode; dc->st.bk_mode = m; return o; }
static int WINAPI GetBkMode(HDC h) { DC* dc = dc_get(h); return dc ? dc->st.bk_mode : 0; }
static int WINAPI SetROP2(HDC h, int r) { DC* dc = dc_get(h); if (!dc) return 0; int o = dc->st.rop2; dc->st.rop2 = r; return o; }
static int WINAPI GetROP2(HDC h) { DC* dc = dc_get(h); return dc ? dc->st.rop2 : 0; }
static u32 WINAPI SetTextAlign(HDC h, u32 a) { DC* dc = dc_get(h); if (!dc) return 0xFFFFFFFF; u32 o = (u32)dc->st.text_align; dc->st.text_align = (int)a; return o; }
static u32 WINAPI GetTextAlign(HDC h) { DC* dc = dc_get(h); return dc ? (u32)dc->st.text_align : 0; }
static int WINAPI SetStretchBltMode(HDC h, int m) { DC* dc = dc_get(h); if (!dc) return 0; int o = dc->st.stretch_mode; dc->st.stretch_mode = m; return o; }
static int WINAPI GetStretchBltMode(HDC h) { DC* dc = dc_get(h); return dc ? dc->st.stretch_mode : 0; }
static int WINAPI SetMapMode(HDC h, int m) { DC* dc = dc_get(h); if (!dc) return 0; int o = dc->st.map_mode; dc->st.map_mode = m; return o; }
static int WINAPI GetMapMode(HDC h) { DC* dc = dc_get(h); return dc ? dc->st.map_mode : 0; }
static int WINAPI SetPolyFillMode(HDC h, int m) { DC* dc = dc_get(h); if (!dc) return 0; int o = dc->st.poly_mode; dc->st.poly_mode = m; return o; }
static int WINAPI SetTextCharacterExtra(HDC h, int e) { DC* dc = dc_get(h); if (!dc) return 0x80000000; int o = dc->st.char_extra; dc->st.char_extra = e; return o; }
static BOOL WINAPI SetViewportOrgEx(HDC h, int x, int y, POINT* old) { DC* dc = dc_get(h); if (!dc) return FALSE; if (old) *old = dc->st.vp_org; dc->st.vp_org = {x, y}; return TRUE; }
static BOOL WINAPI SetWindowOrgEx(HDC h, int x, int y, POINT* old) { DC* dc = dc_get(h); if (!dc) return FALSE; if (old) *old = dc->st.win_org; dc->st.win_org = {x, y}; return TRUE; }
static BOOL WINAPI GetViewportOrgEx(HDC h, POINT* p) { DC* dc = dc_get(h); if (!dc) return FALSE; *p = dc->st.vp_org; return TRUE; }
static BOOL WINAPI GetWindowOrgEx(HDC h, POINT* p) { DC* dc = dc_get(h); if (!dc) return FALSE; *p = dc->st.win_org; return TRUE; }
static BOOL WINAPI OffsetViewportOrgEx(HDC h, int x, int y, POINT* old) { DC* dc = dc_get(h); if (!dc) return FALSE; if (old) *old = dc->st.vp_org; dc->st.vp_org.x += x; dc->st.vp_org.y += y; return TRUE; }
static BOOL WINAPI OffsetWindowOrgEx(HDC h, int x, int y, POINT* old) { DC* dc = dc_get(h); if (!dc) return FALSE; if (old) *old = dc->st.win_org; dc->st.win_org.x += x; dc->st.win_org.y += y; return TRUE; }
static BOOL WINAPI SetViewportExtEx(HDC h, int x, int y, SIZE* old) { (void)h; (void)x; (void)y; if (old) *old = {1, 1}; return TRUE; }
static BOOL WINAPI SetWindowExtEx(HDC h, int x, int y, SIZE* old) { (void)h; (void)x; (void)y; if (old) *old = {1, 1}; return TRUE; }
static BOOL WINAPI SetBrushOrgEx(HDC h, int x, int y, POINT* old) { DC* dc = dc_get(h); if (!dc) return FALSE; if (old) *old = dc->st.brush_org; dc->st.brush_org = {x, y}; return TRUE; }
static BOOL WINAPI GetCurrentPositionEx(HDC h, POINT* p) { DC* dc = dc_get(h); if (!dc) return FALSE; *p = dc->st.pos; return TRUE; }
static BOOL WINAPI DPtoLP(HDC h, POINT* p, int n) { DC* dc = dc_get(h); if (!dc) return FALSE; for (int i = 0; i < n; i++) { p[i].x += dc->st.win_org.x - dc->st.vp_org.x; p[i].y += dc->st.win_org.y - dc->st.vp_org.y; } return TRUE; }
static BOOL WINAPI LPtoDP(HDC h, POINT* p, int n) { DC* dc = dc_get(h); if (!dc) return FALSE; for (int i = 0; i < n; i++) { p[i].x -= dc->st.win_org.x - dc->st.vp_org.x; p[i].y -= dc->st.win_org.y - dc->st.vp_org.y; } return TRUE; }
static BOOL WINAPI SetGraphicsMode(HDC h, int m) { (void)h; (void)m; return 1; }
static BOOL WINAPI SetWorldTransform(HDC h, const void* x) { (void)h; (void)x; return TRUE; }
static BOOL WINAPI ModifyWorldTransform(HDC h, const void* x, u32 m) { (void)h; (void)x; (void)m; return TRUE; }
static int WINAPI SetArcDirection(HDC h, int d) { (void)h; (void)d; return 1; }

static int screen_w() { return (int)(si::win_ctl(0, si::WC_SCREEN_SIZE) & 0xFFFF); }
static int screen_h() { return (int)((si::win_ctl(0, si::WC_SCREEN_SIZE) >> 16) & 0xFFFF); }
static int WINAPI GetDeviceCaps(HDC h, int i) {
    DC* dc = dc_get(h);
    int bpp = dc && dc->kind == DCK_MEMORY && dc->st.bmp ? dc->st.bmp->s.bpp : 32;
    switch (i) {
    case 2: return 1;                       // TECHNOLOGY: DT_RASDISPLAY
    case 4: return screen_w() * 254 / 960;  // HORZSIZE, mm
    case 6: return screen_h() * 254 / 960;
    case 8: case 118: return screen_w();    // HORZRES, DESKTOPHORZRES
    case 10: case 117: return screen_h();   // VERTRES, DESKTOPVERTRES
    case 12: return bpp;                    // BITSPIXEL
    case 14: return 1;                      // PLANES
    case 24: return -1;                     // NUMCOLORS: more than 256
    case 38: return 0x7E99;                 // RASTERCAPS
    case 40: return 1;                      // ASPECTX
    case 42: return 1;
    case 44: return 1;
    case 88: case 90: return 96;            // LOGPIXELSX/Y
    case 104: return 0;                     // SIZEPALETTE
    case 108: return 24;                    // COLORRES
    case 116: return 60;                    // VREFRESH
    case 119: return 0;                     // BLTALIGNMENT
    case 120: return 0x0F;                  // SHADEBLENDCAPS
    }
    return 0;
}

// ---------------------------------------------------------------- shapes
static BOOL WINAPI MoveToEx(HDC h, int x, int y, POINT* old) { DC* dc = dc_get(h); if (!dc) return FALSE; if (old) *old = dc->st.pos; dc->st.pos = {x, y}; return TRUE; }
static BOOL WINAPI LineTo(HDC h, int x, int y) {
    DC* dc = dc_get(h);
    if (!dc) return FALSE;
    Surf s; int ox, oy; RECT clip;
    if (dc_target(dc, &s, &ox, &oy, &clip)) line_dev(dc, s, clip, dc->st.pos.x + ox, dc->st.pos.y + oy, x + ox, y + oy, dc->st.pen, false);
    dc->st.pos = {x, y};
    return TRUE;
}
static BOOL WINAPI Polyline(HDC h, const POINT* p, int n) {
    DC* dc = dc_get(h);
    if (!dc || n < 2) return FALSE;
    Surf s; int ox, oy; RECT clip;
    if (dc_target(dc, &s, &ox, &oy, &clip))
        for (int i = 0; i + 1 < n; i++) line_dev(dc, s, clip, p[i].x + ox, p[i].y + oy, p[i + 1].x + ox, p[i + 1].y + oy, dc->st.pen, false);
    return TRUE;
}
static BOOL WINAPI PolylineTo(HDC h, const POINT* p, u32 n) { DC* dc = dc_get(h); if (!dc) return FALSE; for (u32 i = 0; i < n; i++) LineTo(h, p[i].x, p[i].y); return TRUE; }
static BOOL WINAPI PolyPolyline(HDC h, const POINT* p, const u32* counts, u32 n) { for (u32 i = 0; i < n; i++) { Polyline(h, p, (int)counts[i]); p += counts[i]; } return TRUE; }
static BOOL WINAPI Polygon(HDC h, const POINT* p, int n) {
    DC* dc = dc_get(h);
    if (!dc || n < 2) return FALSE;
    Surf s; int ox, oy; RECT clip;
    if (!dc_target(dc, &s, &ox, &oy, &clip)) return TRUE;
    POINT* d = (POINT*)ialloc(sizeof(POINT) * (usize)n);
    for (int i = 0; i < n; i++) d[i] = {p[i].x + ox, p[i].y + oy};
    fill_poly_dev(dc, s, clip, d, n, dc->st.brush, dc->st.poly_mode == 2);
    for (int i = 0; i < n; i++) line_dev(dc, s, clip, d[i].x, d[i].y, d[(i + 1) % n].x, d[(i + 1) % n].y, dc->st.pen, false);
    ifree(d);
    return TRUE;
}
static BOOL WINAPI PolyPolygon(HDC h, const POINT* p, const i32* counts, int n) { for (int i = 0; i < n; i++) { Polygon(h, p, counts[i]); p += counts[i]; } return TRUE; }
static BOOL WINAPI Rectangle(HDC h, int l, int t, int r, int b) {
    DC* dc = dc_get(h);
    if (!dc) return FALSE;
    Surf s; int ox, oy; RECT clip;
    if (!dc_target(dc, &s, &ox, &oy, &clip)) return TRUE;
    if (r < l) { int x = l; l = r; r = x; }
    if (b < t) { int x = t; t = b; b = x; }
    bool pen = dc->st.pen && dc->st.pen->style != 5;
    int inset = pen ? 1 : 0;
    for (int y = t + inset; y < b - inset; y++) brush_span(dc, s, clip, dc->st.brush, l + inset + ox, r - inset + ox, y + oy, 13);
    if (pen) {
        l += ox; r += ox; t += oy; b += oy;
        line_dev(dc, s, clip, l, t, r - 1, t, dc->st.pen, true);
        line_dev(dc, s, clip, l, b - 1, r - 1, b - 1, dc->st.pen, true);
        line_dev(dc, s, clip, l, t, l, b - 1, dc->st.pen, true);
        line_dev(dc, s, clip, r - 1, t, r - 1, b - 1, dc->st.pen, true);
    }
    return TRUE;
}
static BOOL WINAPI Ellipse(HDC h, int l, int t, int r, int b) {
    DC* dc = dc_get(h);
    if (!dc) return FALSE;
    Surf s; int ox, oy; RECT clip;
    if (!dc_target(dc, &s, &ox, &oy, &clip)) return TRUE;
    if (r < l) { int x = l; l = r; r = x; }
    if (b < t) { int x = t; t = b; b = x; }
    double cx = (l + r) / 2.0, cy = (t + b) / 2.0, rx = (r - l) / 2.0, ry = (b - t) / 2.0;
    if (rx <= 0 || ry <= 0) return TRUE;
    bool pen = dc->st.pen && dc->st.pen->style != 5;
    int pw = pen ? (dc->st.pen->width < 1 ? 1 : dc->st.pen->width) : 0;
    u32 pc = pen ? (dc->st.pen == (Pen*)objs[19] ? rgb_of(dc->st.dc_pen) : rgb_of(dc->st.pen->color)) : 0;
    for (int y = t; y < b; y++) {
        double dy = (y + 0.5 - cy) / ry;
        if (dy * dy > 1) continue;
        double half = rx * fsqrt(1 - dy * dy);
        int x0 = (int)(cx - half + 0.5), x1 = (int)(cx + half + 0.5);
        if (!pen) { brush_span(dc, s, clip, dc->st.brush, x0 + ox, x1 + ox, y + oy, 13); continue; }
        // the inner ellipse is the brush, the ring between is the pen
        double irx = rx - pw, iry = ry - pw;
        int i0 = x0, i1 = x0;
        if (irx > 0 && iry > 0) {
            double idy = (y + 0.5 - cy) / iry;
            if (idy * idy <= 1) { double ih = irx * fsqrt(1 - idy * idy); i0 = (int)(cx - ih + 0.5); i1 = (int)(cx + ih + 0.5); }
        }
        if (i1 > i0) {
            brush_span(dc, s, clip, dc->st.brush, i0 + ox, i1 + ox, y + oy, 13);
            span(dc, s, clip, x0 + ox, i0 + ox, y + oy, pc, dc->st.rop2);
            span(dc, s, clip, i1 + ox, x1 + ox, y + oy, pc, dc->st.rop2);
        } else span(dc, s, clip, x0 + ox, x1 + ox, y + oy, pc, dc->st.rop2);
    }
    return TRUE;
}
static BOOL WINAPI RoundRect(HDC h, int l, int t, int r, int b, int ew, int eh) {
    DC* dc = dc_get(h);
    if (!dc) return FALSE;
    if (ew < 2 || eh < 2) return Rectangle(h, l, t, r, b);
    POINT pts[512];
    int n = 0;
    int rx = ew / 2, ry = eh / 2;
    const double PI = 3.14159265358979;
    n += ellipse_points(r - ew, t, r, t + eh, 0, PI / 2, pts + n, 128);
    n += ellipse_points(l, t, l + ew, t + eh, PI / 2, PI, pts + n, 128);
    n += ellipse_points(l, b - eh, l + ew, b, PI, PI * 1.5, pts + n, 128);
    n += ellipse_points(r - ew, b - eh, r, b, PI * 1.5, PI * 2, pts + n, 128);
    (void)rx; (void)ry;
    return Polygon(h, pts, n);
}
static double angle_of(int cx, int cy, int x, int y) { return c_atan2_d(-(double)(y - cy), (double)(x - cx)); }
static BOOL arc_common(HDC h, int l, int t, int r, int b, int xs, int ys, int xe, int ye, int kind) {
    DC* dc = dc_get(h);
    if (!dc) return FALSE;
    int cx = (l + r) / 2, cy = (t + b) / 2;
    double a0 = angle_of(cx, cy, xs, ys), a1 = angle_of(cx, cy, xe, ye);
    while (a1 <= a0) a1 += 2 * 3.14159265358979;
    POINT pts[1030];
    int n = ellipse_points(l, t, r, b, a0, a1, pts, 1024);
    if (kind == 0) return Polyline(h, pts, n);
    if (kind == 1) pts[n++] = {cx, cy};                         // pie
    return Polygon(h, pts, n);
}
static BOOL WINAPI Arc(HDC h, int l, int t, int r, int b, int xs, int ys, int xe, int ye) { return arc_common(h, l, t, r, b, xs, ys, xe, ye, 0); }
static BOOL WINAPI Pie(HDC h, int l, int t, int r, int b, int xs, int ys, int xe, int ye) { return arc_common(h, l, t, r, b, xs, ys, xe, ye, 1); }
static BOOL WINAPI Chord(HDC h, int l, int t, int r, int b, int xs, int ys, int xe, int ye) { return arc_common(h, l, t, r, b, xs, ys, xe, ye, 2); }
static BOOL WINAPI PolyBezier(HDC h, const POINT* p, u32 n) {
    if (n < 4) return Polyline(h, p, (int)n);
    POINT out[4096];
    int k = 0;
    for (u32 i = 0; i + 3 < n && k < 4000; i += 3) {
        for (int s = 0; s <= 24; s++) {
            double t = s / 24.0, u = 1 - t;
            double x = u * u * u * p[i].x + 3 * u * u * t * p[i + 1].x + 3 * u * t * t * p[i + 2].x + t * t * t * p[i + 3].x;
            double y = u * u * u * p[i].y + 3 * u * u * t * p[i + 1].y + 3 * u * t * t * p[i + 2].y + t * t * t * p[i + 3].y;
            out[k++] = {(int)(x + 0.5), (int)(y + 0.5)};
        }
    }
    return Polyline(h, out, k);
}
static COLORREF WINAPI SetPixel(HDC h, int x, int y, COLORREF c) {
    DC* dc = dc_get(h);
    if (!dc) return 0xFFFFFFFF;
    Surf s; int ox, oy; RECT clip;
    if (!dc_target(dc, &s, &ox, &oy, &clip)) return 0xFFFFFFFF;
    x += ox; y += oy;
    if (x < clip.left || y < clip.top || x >= clip.right || y >= clip.bottom) return 0xFFFFFFFF;
    surf_put(s, x, y, rgb_of(c));
    return cref_of(surf_get(s, x, y));
}
static BOOL WINAPI SetPixelV(HDC h, int x, int y, COLORREF c) { return SetPixel(h, x, y, c) != 0xFFFFFFFF; }
static COLORREF WINAPI GetPixel(HDC h, int x, int y) {
    DC* dc = dc_get(h);
    if (!dc) return 0xFFFFFFFF;
    Surf s; int ox, oy; RECT clip;
    bool paint = dc->paint;
    dc->paint = false;
    bool ok = dc_target(dc, &s, &ox, &oy, &clip);
    dc->paint = paint;
    if (!ok) return 0xFFFFFFFF;
    x += ox; y += oy;
    if (x < clip.left || y < clip.top || x >= clip.right || y >= clip.bottom) return 0xFFFFFFFF;
    return cref_of(surf_get(s, x, y));
}
static BOOL WINAPI PatBlt(HDC h, int x, int y, int w, int hh, u32 rop) {
    DC* dc = dc_get(h);
    if (!dc) return FALSE;
    Surf s; int ox, oy; RECT clip;
    if (!dc_target(dc, &s, &ox, &oy, &clip)) return TRUE;
    if (w < 0) { x += w; w = -w; }
    if (hh < 0) { y += hh; hh = -hh; }
    for (int yy = y + oy; yy < y + oy + hh; yy++) {
        if (yy < clip.top || yy >= clip.bottom) continue;
        for (int xx = x + ox; xx < x + ox + w; xx++) {
            if (xx < clip.left || xx >= clip.right) continue;
            bool skip;
            u32 p = brush_px(dc, dc->st.brush, xx, yy, &skip);
            u32 d = surf_get(s, xx, yy);
            surf_put(s, xx, yy, rop3_apply(rop, p, 0, d));
        }
    }
    return TRUE;
}

// ---------------------------------------------------------------- bitmaps and DIBs
struct BITMAPINFOHEADER { u32 biSize; i32 biWidth, biHeight; u16 biPlanes, biBitCount; u32 biCompression, biSizeImage; i32 biXPelsPerMeter, biYPelsPerMeter; u32 biClrUsed, biClrImportant; };
struct BITMAPV4 { BITMAPINFOHEADER h; u32 rmask, gmask, bmask, amask; };

// a surface describing DIB bits laid out as the header says
static bool surf_from_dib(const void* bmi, const void* bits, Surf* s, u32 usage, DC* dc) {
    const BITMAPINFOHEADER* h = (const BITMAPINFOHEADER*)bmi;
    if (h->biSize == 12) {                                       // BITMAPCOREHEADER
        const u16* c = (const u16*)bmi;
        BITMAPINFOHEADER t;
        memset(&t, 0, sizeof(t));
        t.biSize = 40; t.biWidth = c[2]; t.biHeight = (i16)c[3]; t.biBitCount = c[5];
        (void)t;
        return false;
    }
    if (h->biCompression != 0 && h->biCompression != 3) return false;    // RLE and JPEG/PNG are not supported
    int bpp = h->biBitCount;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return false;
    int w = h->biWidth, ht = h->biHeight < 0 ? -h->biHeight : h->biHeight;
    int stride = ((w * bpp + 31) / 32) * 4;
    s->w = w; s->h = ht; s->bpp = bpp;
    s->bits = (u8*)bits;
    s->stride = stride;
    if (h->biHeight > 0) { s->bits = (u8*)bits + (i64)(ht - 1) * stride; s->stride = -stride; }
    s->palette = nullptr;
    s->ncolors = 0;
    if (bpp == 16) { s->rmask = 0x7C00; s->gmask = 0x3E0; s->bmask = 0x1F; }
    else { s->rmask = 0xFF0000; s->gmask = 0xFF00; s->bmask = 0xFF; }
    const u8* after = (const u8*)bmi + h->biSize;
    if (h->biCompression == 3) {
        const u32* m = h->biSize >= 52 ? &((const BITMAPV4*)bmi)->rmask : (const u32*)after;
        s->rmask = m[0]; s->gmask = m[1]; s->bmask = m[2];
        if (h->biSize < 52) after += 12;
    }
    if (bpp <= 8) {
        int n = h->biClrUsed ? (int)h->biClrUsed : 1 << bpp;
        if (n > 256) n = 256;
        s->palette = (u32*)ialloc(256 * 4);
        s->ncolors = n;
        if (usage == 1 && dc) {                                  // DIB_PAL_COLORS: indices into the DC's palette
            const u16* idx = (const u16*)after;
            for (int i = 0; i < n; i++) s->palette[i] = dc->st.pal->entries[idx[i] % (dc->st.pal->n ? dc->st.pal->n : 1)];
        } else {
            const u32* q = (const u32*)after;
            for (int i = 0; i < n; i++) s->palette[i] = q[i] & 0xFFFFFF;
        }
    }
    return true;
}

static Bitmap* new_bitmap(int w, int h, int bpp, bool dib) {
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768) { set_error(ERROR_INVALID_PARAMETER); return nullptr; }
    Bitmap* b = (Bitmap*)izalloc(sizeof(Bitmap));
    b->type = G_BITMAP;
    b->dib = dib;
    int stride = dib ? ((w * bpp + 31) / 32) * 4 : ((w * bpp + 15) / 16) * 2;
    if (bpp == 32 || bpp == 24) stride = ((w * bpp + 31) / 32) * 4;
    usize size = (usize)stride * (usize)h;
    u8* bits = (u8*)vm_alloc(nullptr, size ? size : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!bits) { ifree(b); set_error(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    b->own_bits = true;
    b->s.bits = bits; b->s.w = w; b->s.h = h; b->s.stride = stride; b->s.bpp = bpp;
    b->s.rmask = 0xFF0000; b->s.gmask = 0xFF00; b->s.bmask = 0xFF;      // (izalloc'd: the defaults in Surf don't apply)
    if (bpp == 1) { b->s.palette = (u32*)ialloc(8); b->s.palette[0] = 0; b->s.palette[1] = 0xFFFFFF; b->s.ncolors = 2; }
    return b;
}
static HANDLE WINAPI CreateCompatibleBitmap(HDC h, int w, int hh) {
    DC* dc = dc_get(h);
    if (!dc) return nullptr;
    // compatible with what the DC draws on: a memory DC that still has its default bitmap is monochrome
    int bpp = dc->kind == DCK_MEMORY && dc->st.bmp ? dc->st.bmp->s.bpp : 32;
    if (bpp != 1) bpp = 32;
    if (w == 0 || hh == 0) { w = hh = 1; bpp = 1; }
    Bitmap* b = new_bitmap(w, hh, bpp, false);
    return b ? gdi_add(b) : nullptr;
}
static HANDLE WINAPI CreateDiscardableBitmap(HDC h, int w, int hh) { return CreateCompatibleBitmap(h, w, hh); }
static HANDLE WINAPI CreateBitmap(int w, int h, u32 planes, u32 bpp, const void* bits) {
    (void)planes;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) bpp = 32;
    Bitmap* b = new_bitmap(w, h, (int)bpp, false);
    if (!b) return nullptr;
    if (bpp == 16) { b->s.rmask = 0x7C00; b->s.gmask = 0x3E0; b->s.bmask = 0x1F; }
    if (bits) {
        int src_stride = ((w * (int)bpp + 15) / 16) * 2;            // device-dependent bitmaps: WORD-aligned rows
        for (int y = 0; y < h; y++) memcpy(b->s.bits + (i64)y * b->s.stride, (const u8*)bits + (i64)y * src_stride, (usize)(src_stride < b->s.stride ? src_stride : b->s.stride));
    }
    return gdi_add(b);
}
static HANDLE WINAPI CreateBitmapIndirect(const BITMAP_* bm) { return CreateBitmap(bm->bmWidth, bm->bmHeight, bm->bmPlanes, bm->bmBitsPixel, bm->bmBits); }
static HANDLE WINAPI CreateDIBSection(HDC h, const void* bmi, u32 usage, void** bits_out, HANDLE section, u32 offset) {
    (void)section; (void)offset;
    const BITMAPINFOHEADER* bh = (const BITMAPINFOHEADER*)bmi;
    if (!bh || bh->biWidth <= 0 || !bh->biHeight) { set_error(ERROR_INVALID_PARAMETER); return nullptr; }
    int w = bh->biWidth, ht = bh->biHeight < 0 ? -bh->biHeight : bh->biHeight;
    Bitmap* b = new_bitmap(w, ht, bh->biBitCount, true);
    if (!b) return nullptr;
    Surf s;
    void* raw = b->s.bits;
    if (!surf_from_dib(bmi, raw, &s, usage, dc_get(h))) { vm_free(raw, 0, MEM_RELEASE); ifree(b); set_error(ERROR_INVALID_PARAMETER); return nullptr; }
    if (b->s.palette) ifree(b->s.palette);
    b->s = s;
    memcpy(b->bih, bmi, 40);
    if (bits_out) *bits_out = raw;
    return gdi_add(b);
}

// copy a rectangle between surfaces with a raster operation, stretching (nearest neighbour) as needed
static void blit_core(DC* ddc, Surf& d, const RECT& clip, int dx, int dy, int dw, int dh,
                      const Surf* s, int sx, int sy, int sw, int sh, u32 rop, DC* sdc, bool transparent = false, u32 tkey = 0) {
    bool use_src = rop_uses_src(rop) && s;
    bool use_dst = rop_uses_dst(rop);
    bool flipx = false, flipy = false;
    if (dw < 0) { dx += dw; dw = -dw; flipx = !flipx; }
    if (dh < 0) { dy += dh; dh = -dh; flipy = !flipy; }
    if (sw < 0) { sx += sw; sw = -sw; flipx = !flipx; }
    if (sh < 0) { sy += sh; sh = -sh; flipy = !flipy; }
    if (!dw || !dh || (use_src && (!sw || !sh))) return;
    int x0 = dx < clip.left ? clip.left : dx, x1 = dx + dw > clip.right ? clip.right : dx + dw;
    int y0 = dy < clip.top ? clip.top : dy, y1 = dy + dh > clip.bottom ? clip.bottom : dy + dh;
    if (x0 >= x1 || y0 >= y1) return;
    bool same = use_src && sw == dw && sh == dh && !flipx && !flipy;
    // the colours of a monochrome source on a colour destination come from the destination DC
    bool mono_src = use_src && s->bpp == 1 && d.bpp != 1;
    u32 mono0 = ddc ? rgb_of(ddc->st.text_color) : 0, mono1 = ddc ? rgb_of(ddc->st.bk_color) : 0xFFFFFF;
    bool to_mono = use_src && s->bpp != 1 && d.bpp == 1;
    u32 src_bk = sdc ? rgb_of(sdc->st.bk_color) : 0xFFFFFF;
    bool fast = rop == 0xCC0020 && same && !mono_src && !to_mono && !transparent && fast32(d) && fast32(*s);
    Brush* br = ddc ? ddc->st.brush : nullptr;
    for (int y = y0; y < y1; y++) {
        int syy = 0;
        if (use_src) {
            int ry = same ? y - dy : (int)((i64)(y - dy) * sh / dh);
            syy = sy + (flipy ? sh - 1 - ry : ry);
            if (syy < 0 || syy >= s->h) continue;
        }
        if (fast) {
            int ax = x0, bx = x1;
            int sxa = sx + (ax - dx);
            if (sxa < 0) { ax -= sxa; sxa = 0; }
            if (sxa + (bx - ax) > s->w) bx = ax + (s->w - sxa);
            if (bx > ax) memmove(row32(d, y) + ax, row32(*s, syy) + sxa, (usize)(bx - ax) * 4);
            continue;
        }
        for (int x = x0; x < x1; x++) {
            u32 sv = 0;
            if (use_src) {
                int rx = same ? x - dx : (int)((i64)(x - dx) * sw / dw);
                int sxx = sx + (flipx ? sw - 1 - rx : rx);
                if (sxx < 0 || sxx >= s->w) continue;
                sv = surf_get(*s, sxx, syy);
                if (transparent && sv == tkey) continue;
                if (mono_src) sv = sv ? mono1 : mono0;
                else if (to_mono) sv = sv == src_bk ? 0xFFFFFF : 0;
            }
            u32 pv = 0;
            if (br && (rop == 0xF00021 || ((rop >> 16) & 0xF0) != (((rop >> 16) & 0x0F) << 4))) { bool sk; pv = brush_px(ddc, br, x, y, &sk); }
            u32 dv = use_dst ? surf_get(d, x, y) : 0;
            surf_put(d, x, y, rop3_apply(rop, pv, sv, dv));
        }
    }
}

static BOOL stretch(HDC hd, int x, int y, int w, int h, HDC hs, int sx, int sy, int sw, int sh, u32 rop) {
    DC* d = dc_get(hd);
    if (!d) return FALSE;
    Surf ds; int ox, oy; RECT clip;
    if (!dc_target(d, &ds, &ox, &oy, &clip)) return TRUE;
    rop &= 0x00FFFFFF;
    if (!rop_uses_src(rop)) { blit_core(d, ds, clip, x + ox, y + oy, w, h, nullptr, 0, 0, 0, 0, rop, nullptr); return TRUE; }
    DC* s = dc_get(hs);
    if (!s) return FALSE;
    Surf ss; int sox, soy; RECT sclip;
    bool paint = s->paint;
    s->paint = false;
    bool ok = dc_target(s, &ss, &sox, &soy, &sclip);
    s->paint = paint;
    if (!ok) return TRUE;
    // the source rectangle is limited to what the source DC can see
    blit_core(d, ds, clip, x + ox, y + oy, w, h, &ss, sx + sox, sy + soy, sw, sh, rop, s);
    return TRUE;
}
static BOOL WINAPI BitBlt(HDC hd, int x, int y, int w, int h, HDC hs, int sx, int sy, u32 rop) { return stretch(hd, x, y, w, h, hs, sx, sy, w, h, rop); }
static BOOL WINAPI StretchBlt(HDC hd, int x, int y, int w, int h, HDC hs, int sx, int sy, int sw, int sh, u32 rop) { return stretch(hd, x, y, w, h, hs, sx, sy, sw, sh, rop); }
static BOOL WINAPI MaskBlt(HDC hd, int x, int y, int w, int h, HDC hs, int sx, int sy, HANDLE mask, int mx, int my, u32 rop) { (void)mask; (void)mx; (void)my; return BitBlt(hd, x, y, w, h, hs, sx, sy, rop & 0xFFFFFF); }
static BOOL WINAPI TransparentBlt(HDC hd, int x, int y, int w, int h, HDC hs, int sx, int sy, int sw, int sh, u32 key) {
    DC* d = dc_get(hd);
    DC* s = dc_get(hs);
    if (!d || !s) return FALSE;
    Surf ds, ss; int ox, oy, sox, soy; RECT clip, sclip;
    if (!dc_target(d, &ds, &ox, &oy, &clip) || !dc_target(s, &ss, &sox, &soy, &sclip)) return TRUE;
    blit_core(d, ds, clip, x + ox, y + oy, w, h, &ss, sx + sox, sy + soy, sw, sh, 0xCC0020, s, true, rgb_of(key));
    return TRUE;
}
struct BLENDFUNCTION { u8 BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat; };
static BOOL WINAPI AlphaBlend(HDC hd, int x, int y, int w, int h, HDC hs, int sx, int sy, int sw, int sh, BLENDFUNCTION bf) {
    DC* d = dc_get(hd);
    DC* s = dc_get(hs);
    if (!d || !s || w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return FALSE;
    Surf ds, ss; int ox, oy, sox, soy; RECT clip, sclip;
    if (!dc_target(d, &ds, &ox, &oy, &clip) || !dc_target(s, &ss, &sox, &soy, &sclip)) return TRUE;
    bool per_px = bf.AlphaFormat & 1;                             // AC_SRC_ALPHA: premultiplied
    u32 ca = bf.SourceConstantAlpha;
    for (int yy = 0; yy < h; yy++) {
        int dyy = y + oy + yy;
        if (dyy < clip.top || dyy >= clip.bottom) continue;
        int syy = sy + soy + (int)((i64)yy * sh / h);
        if (syy < 0 || syy >= ss.h) continue;
        for (int xx = 0; xx < w; xx++) {
            int dxx = x + ox + xx;
            if (dxx < clip.left || dxx >= clip.right) continue;
            int sxx = sx + sox + (int)((i64)xx * sw / w);
            if (sxx < 0 || sxx >= ss.w) continue;
            u32 sv = ss.bpp == 32 ? row32(ss, syy)[sxx] : surf_get(ss, sxx, syy) | 0xFF000000;
            u32 dv = surf_get(ds, dxx, dyy);
            u32 out = 0;
            u32 sa = per_px ? (sv >> 24) : 255;
            for (int k = 0; k < 24; k += 8) {
                u32 sc = (sv >> k) & 255, dc_ = (dv >> k) & 255;
                u32 v = per_px ? (sc * ca + dc_ * (255 - sa * ca / 255) * 255 / 255) / 255 : (sc * ca + dc_ * (255 - ca)) / 255;
                out |= (v > 255 ? 255 : v) << k;
            }
            surf_put(ds, dxx, dyy, out);
        }
    }
    return TRUE;
}
static BOOL WINAPI GradientFill(HDC h, void* verts, u32 nv, void* mesh, u32 nm, u32 mode) {
    struct TRIVERTEX { i32 x, y; u16 r, g, b, a; };
    TRIVERTEX* v = (TRIVERTEX*)verts;
    DC* dc = dc_get(h);
    if (!dc || mode > 1) return FALSE;                          // rectangles only
    Surf s; int ox, oy; RECT clip;
    if (!dc_target(dc, &s, &ox, &oy, &clip)) return TRUE;
    u32* m = (u32*)mesh;
    for (u32 i = 0; i < nm; i++) {
        if (m[i * 2] >= nv || m[i * 2 + 1] >= nv) continue;
        TRIVERTEX a = v[m[i * 2]], b = v[m[i * 2 + 1]];
        int l = a.x < b.x ? a.x : b.x, r = a.x < b.x ? b.x : a.x, t = a.y < b.y ? a.y : b.y, bt = a.y < b.y ? b.y : a.y;
        for (int y = t; y < bt; y++)
            for (int x = l; x < r; x++) {
                int dx = x + ox, dy = y + oy;
                if (dx < clip.left || dx >= clip.right || dy < clip.top || dy >= clip.bottom) continue;
                double f = mode == 0 ? (r > l ? (double)(x - a.x) / (b.x - a.x) : 0) : (bt > t ? (double)(y - a.y) / (b.y - a.y) : 0);
                if (f < 0) f = 0;
                if (f > 1) f = 1;
                u32 R = (u32)((a.r + (b.r - a.r) * f)) >> 8, G = (u32)((a.g + (b.g - a.g) * f)) >> 8, B = (u32)((a.b + (b.b - a.b) * f)) >> 8;
                surf_put(s, dx, dy, (R << 16) | (G << 8) | B);
            }
    }
    return TRUE;
}

static int dib_to_dc(DC* dc, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, const void* bits, const void* bmi, u32 usage, u32 rop) {
    Surf ds; int ox, oy; RECT clip;
    if (!dc_target(dc, &ds, &ox, &oy, &clip)) return sh;
    Surf src;
    if (!surf_from_dib(bmi, bits, &src, usage, dc)) return 0;
    // DIB source coordinates count from the bottom-left for bottom-up bitmaps; surf_from_dib already flipped rows
    const BITMAPINFOHEADER* h = (const BITMAPINFOHEADER*)bmi;
    int syy = h->biHeight > 0 ? src.h - sy - sh : sy;
    blit_core(dc, ds, clip, dx + ox, dy + oy, dw, dh, &src, sx, syy, sw, sh, rop, nullptr);
    if (src.palette) ifree(src.palette);
    return sh;
}
static int WINAPI StretchDIBits(HDC h, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, const void* bits, const void* bmi, u32 usage, u32 rop) {
    DC* dc = dc_get(h);
    if (!dc || !bits || !bmi) return 0;
    return dib_to_dc(dc, dx, dy, dw, dh, sx, sy, sw, sh, bits, bmi, usage, rop);
}
static int WINAPI SetDIBitsToDevice(HDC h, int dx, int dy, u32 w, u32 hh, int sx, int sy, u32 start, u32 lines, const void* bits, const void* bmi, u32 usage) {
    DC* dc = dc_get(h);
    if (!dc || !bits || !bmi) return 0;
    const BITMAPINFOHEADER* bh = (const BITMAPINFOHEADER*)bmi;
    // the bits hold scan lines start .. start+lines (counted from the bottom for bottom-up DIBs)
    BITMAPINFOHEADER band = *bh;
    u8 hdr[40 + 1024 + 12];
    usize extra = bh->biBitCount <= 8 ? (usize)((bh->biClrUsed ? bh->biClrUsed : 1u << bh->biBitCount) * (usage == 1 ? 2 : 4)) : bh->biCompression == 3 ? 12 : 0;
    if (extra > 1036) extra = 1036;
    band.biSize = 40;
    band.biHeight = bh->biHeight < 0 ? -(i32)lines : (i32)lines;
    memcpy(hdr, &band, 40);
    memcpy(hdr + 40, (const u8*)bmi + bh->biSize, extra);
    int full_h = bh->biHeight < 0 ? -bh->biHeight : bh->biHeight;
    // destination rows covered by this band
    int top_line = bh->biHeight > 0 ? full_h - (int)start - (int)lines : (int)start;
    int src_y = bh->biHeight > 0 ? (full_h - sy - (int)hh) : sy;
    int y0 = src_y > top_line ? src_y : top_line;
    int y1 = src_y + (int)hh < top_line + (int)lines ? src_y + (int)hh : top_line + (int)lines;
    if (y1 <= y0) return (int)lines;
    Surf ds; int ox, oy; RECT clip;
    if (!dc_target(dc, &ds, &ox, &oy, &clip)) return (int)lines;
    Surf src;
    if (!surf_from_dib(hdr, bits, &src, usage, dc)) return 0;
    blit_core(dc, ds, clip, dx + ox, dy + oy + (y0 - src_y), (int)w, y1 - y0, &src, sx, y0 - top_line, (int)w, y1 - y0, 0xCC0020, nullptr);
    if (src.palette) ifree(src.palette);
    return (int)lines;
}
// a bitmap's pixels into DIB form, or DIB pixels into a bitmap
static int WINAPI GetDIBits(HDC h, HANDLE bmp, u32 start, u32 lines, void* bits, void* bmi, u32 usage) {
    Bitmap* b = (Bitmap*)gdi_get(bmp, G_BITMAP);
    BITMAPINFOHEADER* bh = (BITMAPINFOHEADER*)bmi;
    if (!b || !bh) return 0;
    if (!bh->biBitCount || !bits) {
        // fill in the header (and colour table) only
        bh->biWidth = b->s.w;
        bh->biHeight = b->s.h;
        bh->biPlanes = 1;
        if (!bh->biBitCount) bh->biBitCount = (u16)(b->s.bpp == 16 ? 16 : b->s.bpp);
        bh->biCompression = bh->biBitCount == 16 || bh->biBitCount == 32 ? 3 : 0;
        bh->biSizeImage = (u32)(((b->s.w * bh->biBitCount + 31) / 32) * 4 * b->s.h);
        if (bh->biCompression == 3) { u32* m = (u32*)((u8*)bmi + 40); m[0] = b->s.bpp == 16 ? b->s.rmask : 0xFF0000; m[1] = b->s.bpp == 16 ? b->s.gmask : 0xFF00; m[2] = b->s.bpp == 16 ? b->s.bmask : 0xFF; }
        if (bh->biBitCount <= 8 && b->s.palette) memcpy((u8*)bmi + 40, b->s.palette, (usize)b->s.ncolors * 4);
        return b->s.h;
    }
    Surf dst;
    BITMAPINFOHEADER band = *bh;
    band.biHeight = bh->biHeight < 0 ? -(i32)lines : (i32)lines;
    u8 hdr[40 + 1024 + 12];
    memcpy(hdr, &band, 40);
    if (bh->biBitCount <= 8) {
        int n = 1 << bh->biBitCount;
        u32* pal = (u32*)((u8*)bmi + 40);
        if (b->s.palette) for (int i = 0; i < n && i < b->s.ncolors; i++) pal[i] = b->s.palette[i];
        else for (int i = 0; i < n; i++) pal[i] = (u32)(i * 255 / (n - 1)) * 0x010101;
        memcpy(hdr + 40, pal, (usize)n * 4);
    } else if (bh->biCompression == 3) memcpy(hdr + 40, (u8*)bmi + 40, 12);
    if (!surf_from_dib(hdr, bits, &dst, usage, dc_get(h))) return 0;
    int src_y = bh->biHeight > 0 ? b->s.h - (int)start - (int)lines : (int)start;
    RECT clip = {0, 0, dst.w, dst.h};
    blit_core(nullptr, dst, clip, 0, 0, dst.w, dst.h, &b->s, 0, src_y, dst.w, dst.h, 0xCC0020, nullptr);
    if (dst.palette) ifree(dst.palette);
    return (int)lines;
}
static int WINAPI SetDIBits(HDC h, HANDLE bmp, u32 start, u32 lines, const void* bits, const void* bmi, u32 usage) {
    Bitmap* b = (Bitmap*)gdi_get(bmp, G_BITMAP);
    if (!b || !bits || !bmi) return 0;
    const BITMAPINFOHEADER* bh = (const BITMAPINFOHEADER*)bmi;
    BITMAPINFOHEADER band = *bh;
    band.biHeight = bh->biHeight < 0 ? -(i32)lines : (i32)lines;
    u8 hdr[40 + 1024 + 12];
    memcpy(hdr, &band, 40);
    usize extra = bh->biBitCount <= 8 ? (usize)((bh->biClrUsed ? bh->biClrUsed : 1u << bh->biBitCount) * (usage == 1 ? 2 : 4)) : bh->biCompression == 3 ? 12 : 0;
    if (extra > 1036) extra = 1036;
    memcpy(hdr + 40, (const u8*)bmi + bh->biSize, extra);
    Surf src;
    if (!surf_from_dib(hdr, bits, &src, usage, dc_get(h))) return 0;
    int dst_y = bh->biHeight > 0 ? b->s.h - (int)start - (int)lines : (int)start;
    RECT clip = {0, 0, b->s.w, b->s.h};
    blit_core(nullptr, b->s, clip, 0, dst_y, src.w, src.h, &src, 0, 0, src.w, src.h, 0xCC0020, nullptr);
    if (src.palette) ifree(src.palette);
    return (int)lines;
}
static HANDLE WINAPI CreateDIBitmap(HDC h, const void* bih, u32 init, const void* bits, const void* bmi, u32 usage) {
    const BITMAPINFOHEADER* bh = (const BITMAPINFOHEADER*)bih;
    if (!bh) return nullptr;
    int ht = bh->biHeight < 0 ? -bh->biHeight : bh->biHeight;
    HANDLE b = CreateCompatibleBitmap(h, bh->biWidth, ht);
    if (b && (init & 4) && bits && bmi) SetDIBits(h, b, 0, (u32)ht, bits, bmi, usage);    // CBM_INIT
    return b;
}
static HANDLE WINAPI CreateDIBPatternBrushPt(const void* dib, u32 usage) {
    const BITMAPINFOHEADER* bh = (const BITMAPINFOHEADER*)dib;
    usize extra = bh->biBitCount <= 8 ? (usize)((bh->biClrUsed ? bh->biClrUsed : 1u << bh->biBitCount) * (usage == 1 ? 2 : 4)) : 0;
    HANDLE bmp = CreateDIBitmap(nullptr, dib, 4, (const u8*)dib + bh->biSize + extra, dib, usage);
    return bmp ? CreatePatternBrush(bmp) : nullptr;
}
static i32 WINAPI GetBitmapBits(HANDLE bmp, i32 n, void* out) {
    Bitmap* b = (Bitmap*)gdi_get(bmp, G_BITMAP);
    if (!b) return 0;
    int stride = ((b->s.w * b->s.bpp + 15) / 16) * 2;
    i32 total = stride * b->s.h;
    if (n > total) n = total;
    for (int y = 0; y * stride < n; y++) memcpy((u8*)out + y * stride, b->s.bits + (i64)y * b->s.stride, (usize)(n - y * stride < stride ? n - y * stride : stride));
    return n;
}
static i32 WINAPI SetBitmapBits(HANDLE bmp, u32 n, const void* in) {
    Bitmap* b = (Bitmap*)gdi_get(bmp, G_BITMAP);
    if (!b) return 0;
    int stride = ((b->s.w * b->s.bpp + 15) / 16) * 2;
    for (int y = 0; (u32)(y * stride) < n && y < b->s.h; y++) memcpy(b->s.bits + (i64)y * b->s.stride, (const u8*)in + y * stride, (usize)stride);
    return (i32)n;
}
static BOOL WINAPI GetBitmapDimensionEx(HANDLE b, SIZE* s) { (void)b; s->cx = s->cy = 0; return TRUE; }
static u32 WINAPI SetDIBColorTable(HDC h, u32 start, u32 n, const u32* colors) {
    DC* dc = dc_get(h);
    if (!dc || !dc->st.bmp || !dc->st.bmp->s.palette) return 0;
    Surf& s = dc->st.bmp->s;
    for (u32 i = 0; i < n && start + i < 256; i++) s.palette[start + i] = colors[i] & 0xFFFFFF;
    if ((int)(start + n) > s.ncolors) s.ncolors = (int)(start + n > 256 ? 256 : start + n);
    return n;
}
static u32 WINAPI GetDIBColorTable(HDC h, u32 start, u32 n, u32* colors) {
    DC* dc = dc_get(h);
    if (!dc || !dc->st.bmp || !dc->st.bmp->s.palette) return 0;
    Surf& s = dc->st.bmp->s;
    u32 k = 0;
    for (; k < n && (int)(start + k) < s.ncolors; k++) colors[k] = s.palette[start + k];
    return k;
}

// ---------------------------------------------------------------- palettes (the screen is true colour)
struct LOGPALETTE { u16 ver, n; u32 entries[1]; };
static HANDLE WINAPI CreatePalette(const LOGPALETTE* lp) {
    Palette* p = (Palette*)izalloc(sizeof(Palette));
    p->type = G_PALETTE;
    p->n = lp->n > 256 ? 256 : lp->n;
    for (int i = 0; i < p->n; i++) { u32 e = lp->entries[i]; p->entries[i] = ((e & 0xFF) << 16) | (e & 0xFF00) | ((e >> 16) & 0xFF); }
    return gdi_add(p);
}
static HANDLE WINAPI CreateHalftonePalette(HDC h) { (void)h; return stock_object(15); }
static HANDLE WINAPI SelectPalette(HDC h, HANDLE pal, BOOL bg) {
    (void)bg;
    DC* dc = dc_get(h);
    Palette* p = (Palette*)gdi_get(pal, G_PALETTE);
    if (!dc || !p) return nullptr;
    HANDLE old = dc->st.pal->handle;
    dc->st.pal = p;
    return old;
}
static u32 WINAPI RealizePalette(HDC h) { (void)h; return 0; }
static u32 WINAPI SetPaletteEntries(HANDLE pal, u32 start, u32 n, const u32* e) {
    Palette* p = (Palette*)gdi_get(pal, G_PALETTE);
    if (!p) return 0;
    for (u32 i = 0; i < n && start + i < 256; i++) { u32 v = e[i]; p->entries[start + i] = ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF); }
    return n;
}
static u32 WINAPI GetPaletteEntries(HANDLE pal, u32 start, u32 n, u32* e) {
    Palette* p = (Palette*)gdi_get(pal, G_PALETTE);
    if (!p) return 0;
    if (!e) return (u32)p->n;
    u32 k = 0;
    for (; k < n && (int)(start + k) < p->n; k++) e[k] = cref_of(p->entries[start + k]);
    return k;
}
static BOOL WINAPI AnimatePalette(HANDLE pal, u32 start, u32 n, const u32* e) { return SetPaletteEntries(pal, start, n, e) != 0; }
static u32 WINAPI GetSystemPaletteEntries(HDC h, u32 start, u32 n, u32* e) { (void)h; return GetPaletteEntries(stock_object(15), start, n, e); }
static u32 WINAPI GetNearestPaletteIndex(HANDLE pal, COLORREF c) {
    Palette* p = (Palette*)gdi_get(pal, G_PALETTE);
    if (!p || !p->n) return 0;
    Surf s; s.palette = p->entries; s.ncolors = p->n;
    return (u32)nearest_index(s, rgb_of(c));
}
static COLORREF WINAPI GetNearestColor(HDC h, COLORREF c) { (void)h; return c; }
static u32 WINAPI SetSystemPaletteUse(HDC h, u32 u) { (void)h; (void)u; return 1; }
static u32 WINAPI GetSystemPaletteUse(HDC h) { (void)h; return 1; }

// ---------------------------------------------------------------- regions (rectangles)
static HANDLE new_region(int l, int t, int r, int b) {
    Region* g = (Region*)izalloc(sizeof(Region));
    g->type = G_REGION;
    if (r < l) { int x = l; l = r; r = x; }
    if (b < t) { int x = t; t = b; b = x; }
    g->r = {l, t, r, b};
    return gdi_add(g);
}
static HANDLE WINAPI CreateRectRgn(int l, int t, int r, int b) { return new_region(l, t, r, b); }
static HANDLE WINAPI CreateRectRgnIndirect(const RECT* r) { return new_region(r->left, r->top, r->right, r->bottom); }
static HANDLE WINAPI CreateRoundRectRgn(int l, int t, int r, int b, int w, int h) { (void)w; (void)h; return new_region(l, t, r, b); }
static HANDLE WINAPI CreateEllipticRgn(int l, int t, int r, int b) { return new_region(l, t, r, b); }
static HANDLE WINAPI CreatePolygonRgn(const POINT* p, int n, int mode) {
    (void)mode;
    if (n <= 0) return new_region(0, 0, 0, 0);
    int l = p[0].x, r = p[0].x, t = p[0].y, b = p[0].y;
    for (int i = 1; i < n; i++) { if (p[i].x < l) l = p[i].x; if (p[i].x > r) r = p[i].x; if (p[i].y < t) t = p[i].y; if (p[i].y > b) b = p[i].y; }
    return new_region(l, t, r, b);
}
static bool rect_empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }
static int region_kind(const RECT& r) { return rect_empty(r) ? 1 : 2; }       // NULLREGION, SIMPLEREGION
static int WINAPI CombineRgn(HANDLE dst, HANDLE a, HANDLE b, int mode) {
    Region* d = (Region*)gdi_get(dst, G_REGION);
    Region* x = (Region*)gdi_get(a, G_REGION);
    Region* y = (Region*)gdi_get(b, G_REGION);
    if (!d || !x) return 0;
    RECT r = x->r;
    if (mode == 5) { d->r = r; return region_kind(r); }        // RGN_COPY
    if (!y) return 0;
    RECT q = y->r;
    switch (mode) {
    case 1:                                                      // RGN_AND
        r = {r.left > q.left ? r.left : q.left, r.top > q.top ? r.top : q.top, r.right < q.right ? r.right : q.right, r.bottom < q.bottom ? r.bottom : q.bottom};
        if (rect_empty(r)) r = {0, 0, 0, 0};
        break;
    case 2: case 3:                                              // RGN_OR, RGN_XOR: the bounding box
        if (rect_empty(r)) r = q;
        else if (!rect_empty(q)) r = {r.left < q.left ? r.left : q.left, r.top < q.top ? r.top : q.top, r.right > q.right ? r.right : q.right, r.bottom > q.bottom ? r.bottom : q.bottom};
        break;
    case 4:                                                      // RGN_DIFF: only when a whole side is cut off
        if (q.top <= r.top && q.bottom >= r.bottom) { if (q.left <= r.left && q.right > r.left) r.left = q.right; else if (q.right >= r.right && q.left < r.right) r.right = q.left; }
        else if (q.left <= r.left && q.right >= r.right) { if (q.top <= r.top && q.bottom > r.top) r.top = q.bottom; else if (q.bottom >= r.bottom && q.top < r.bottom) r.bottom = q.top; }
        if (rect_empty(r)) r = {0, 0, 0, 0};
        break;
    }
    d->r = r;
    return region_kind(r);
}
static BOOL WINAPI SetRectRgn(HANDLE h, int l, int t, int r, int b) { Region* g = (Region*)gdi_get(h, G_REGION); if (!g) return FALSE; g->r = {l, t, r, b}; return TRUE; }
static int WINAPI OffsetRgn(HANDLE h, int x, int y) { Region* g = (Region*)gdi_get(h, G_REGION); if (!g) return 0; g->r.left += x; g->r.right += x; g->r.top += y; g->r.bottom += y; return region_kind(g->r); }
static int WINAPI GetRgnBox(HANDLE h, RECT* r) { Region* g = (Region*)gdi_get(h, G_REGION); if (!g) return 0; *r = g->r; return region_kind(g->r); }
static BOOL WINAPI PtInRegion(HANDLE h, int x, int y) { Region* g = (Region*)gdi_get(h, G_REGION); return g && x >= g->r.left && x < g->r.right && y >= g->r.top && y < g->r.bottom; }
static BOOL WINAPI RectInRegion(HANDLE h, const RECT* r) { Region* g = (Region*)gdi_get(h, G_REGION); return g && r->left < g->r.right && r->right > g->r.left && r->top < g->r.bottom && r->bottom > g->r.top; }
static BOOL WINAPI EqualRgn(HANDLE a, HANDLE b) { Region* x = (Region*)gdi_get(a, G_REGION); Region* y = (Region*)gdi_get(b, G_REGION); return x && y && !memcmp(&x->r, &y->r, sizeof(RECT)); }
static BOOL WINAPI FillRgn(HDC h, HANDLE rgn, HANDLE br) { Region* g = (Region*)gdi_get(rgn, G_REGION); DC* dc = dc_get(h); Brush* b = (Brush*)gdi_get(br, G_BRUSH); if (!g || !dc || !b) return FALSE; fill_rect_dc(dc, &g->r, b); return TRUE; }
static BOOL WINAPI PaintRgn(HDC h, HANDLE rgn) { DC* dc = dc_get(h); Region* g = (Region*)gdi_get(rgn, G_REGION); if (!dc || !g) return FALSE; fill_rect_dc(dc, &g->r, dc->st.brush); return TRUE; }
static BOOL WINAPI FrameRgn(HDC h, HANDLE rgn, HANDLE br, int w, int hh) {
    Region* g = (Region*)gdi_get(rgn, G_REGION);
    DC* dc = dc_get(h);
    Brush* b = (Brush*)gdi_get(br, G_BRUSH);
    if (!g || !dc || !b) return FALSE;
    RECT r = g->r;
    RECT e[4] = {{r.left, r.top, r.right, r.top + hh}, {r.left, r.bottom - hh, r.right, r.bottom}, {r.left, r.top, r.left + w, r.bottom}, {r.right - w, r.top, r.right, r.bottom}};
    for (auto& q : e) fill_rect_dc(dc, &q, b);
    return TRUE;
}
static BOOL WINAPI InvertRgn(HDC h, HANDLE rgn) {
    Region* g = (Region*)gdi_get(rgn, G_REGION);
    if (!g) return FALSE;
    return PatBlt(h, g->r.left, g->r.top, g->r.right - g->r.left, g->r.bottom - g->r.top, 0x550009);
}
static int WINAPI SelectClipRgn(HDC h, HANDLE rgn) {
    DC* dc = dc_get(h);
    if (!dc) return 0;
    Region* g = (Region*)gdi_get(rgn, G_REGION);
    if (!g) { dc->st.has_clip = false; return 2; }
    dc->st.has_clip = true;
    dc->st.clip = g->r;
    return region_kind(g->r);
}
static int WINAPI ExtSelectClipRgn(HDC h, HANDLE rgn, int mode) {
    DC* dc = dc_get(h);
    if (!dc) return 0;
    if (mode == 5 || !dc->st.has_clip) return SelectClipRgn(h, rgn);
    Region* g = (Region*)gdi_get(rgn, G_REGION);
    if (!g) return 0;
    if (mode == 1) {                                             // RGN_AND
        RECT& c = dc->st.clip;
        if (c.left < g->r.left) c.left = g->r.left;
        if (c.top < g->r.top) c.top = g->r.top;
        if (c.right > g->r.right) c.right = g->r.right;
        if (c.bottom > g->r.bottom) c.bottom = g->r.bottom;
    } else dc->st.clip = g->r;
    return region_kind(dc->st.clip);
}
static int WINAPI IntersectClipRect(HDC h, int l, int t, int r, int b) {
    DC* dc = dc_get(h);
    if (!dc) return 0;
    RECT n = {l - dc->st.win_org.x + dc->st.vp_org.x, t - dc->st.win_org.y + dc->st.vp_org.y, r - dc->st.win_org.x + dc->st.vp_org.x, b - dc->st.win_org.y + dc->st.vp_org.y};
    if (!dc->st.has_clip) { dc->st.has_clip = true; dc->st.clip = n; }
    else {
        RECT& c = dc->st.clip;
        if (c.left < n.left) c.left = n.left;
        if (c.top < n.top) c.top = n.top;
        if (c.right > n.right) c.right = n.right;
        if (c.bottom > n.bottom) c.bottom = n.bottom;
    }
    return region_kind(dc->st.clip);
}
static int WINAPI ExcludeClipRect(HDC h, int l, int t, int r, int b) { (void)l; (void)t; (void)r; (void)b; DC* dc = dc_get(h); return dc ? 3 : 0; }    // COMPLEXREGION: nothing removed
static int WINAPI OffsetClipRgn(HDC h, int x, int y) { DC* dc = dc_get(h); if (!dc || !dc->st.has_clip) return 1; dc->st.clip.left += x; dc->st.clip.right += x; dc->st.clip.top += y; dc->st.clip.bottom += y; return 2; }
static int WINAPI GetClipBox(HDC h, RECT* r) {
    DC* dc = dc_get(h);
    if (!dc) return 0;
    Surf s; int ox, oy; RECT clip;
    if (!dc_target(dc, &s, &ox, &oy, &clip)) { *r = {0, 0, 0, 0}; return 1; }
    *r = {clip.left - ox, clip.top - oy, clip.right - ox, clip.bottom - oy};
    return 2;
}
static int WINAPI GetClipRgn(HDC h, HANDLE rgn) {
    DC* dc = dc_get(h);
    Region* g = (Region*)gdi_get(rgn, G_REGION);
    if (!dc || !g) return -1;
    if (!dc->st.has_clip) return 0;
    g->r = dc->st.clip;
    return 1;
}
static BOOL WINAPI PtVisible(HDC h, int x, int y) { RECT r; if (GetClipBox(h, &r) < 2) return FALSE; return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static BOOL WINAPI RectVisible(HDC h, const RECT* q) { RECT r; if (GetClipBox(h, &r) < 2) return FALSE; return q->left < r.right && q->right > r.left && q->top < r.bottom && q->bottom > r.top; }

// ---------------------------------------------------------------- fonts and text
static void font_measure(Font* f) {
    si::TextArgs a;
    memset(&a, 0, sizeof(a));
    a.px100 = f->px100;
    a.flags = (f->bold ? si::TX_BOLD : 0) | (f->mono ? si::TX_MONO : 0) | si::TX_MEASURE;
    si::text_raster(&a, "x", 1);
    f->avgw = a.out_w;
    f->asc = a.out_asc; f->desc = a.out_desc; f->lh = a.out_lh;
    si::text_raster(&a, "W", 1);
    f->maxw = a.out_w;
}
void text_metrics(Font* f, int* asc, int* desc, int* lh, int* avgw, int* maxw) { *asc = f->asc; *desc = f->desc; *lh = f->lh; *avgw = f->avgw; *maxw = f->maxw; }
int text_width_utf8(Font* f, const char* s, int n) {
    if (n <= 0) return 0;
    si::TextArgs a;
    memset(&a, 0, sizeof(a));
    a.px100 = f->px100;
    a.flags = (f->bold ? si::TX_BOLD : 0) | (f->mono ? si::TX_MONO : 0) | si::TX_MEASURE;
    int total = 0;
    while (n > 0) {
        int k = n > 4000 ? 4000 : n;
        total += si::text_raster(&a, s, k);
        s += k; n -= k;
    }
    return total;
}

static HANDLE WINAPI CreateFontIndirectW(const LOGFONTW* lf) { return lf ? gdi_add(new_font(lf)) : nullptr; }
static HANDLE WINAPI CreateFontIndirectA(const void* lfa) {
    if (!lfa) return nullptr;
    LOGFONTW lf;
    memcpy(&lf, lfa, 28);
    wide_from_utf8((const char*)lfa + 28, -1, lf.lfFaceName, 32);
    return CreateFontIndirectW(&lf);
}
static HANDLE WINAPI CreateFontW(int h, int w, int esc, int orient, int weight, u32 italic, u32 under, u32 strike, u32 cs, u32 op, u32 cp, u32 q, u32 pf, const WCHAR* face) {
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = h; lf.lfWidth = w; lf.lfEscapement = esc; lf.lfOrientation = orient; lf.lfWeight = weight;
    lf.lfItalic = (u8)italic; lf.lfUnderline = (u8)under; lf.lfStrikeOut = (u8)strike; lf.lfCharSet = (u8)cs;
    lf.lfOutPrecision = (u8)op; lf.lfClipPrecision = (u8)cp; lf.lfQuality = (u8)q; lf.lfPitchAndFamily = (u8)pf;
    if (face) { usize n = wlen(face); if (n > 31) n = 31; memcpy(lf.lfFaceName, face, n * 2); }
    return CreateFontIndirectW(&lf);
}
static HANDLE WINAPI CreateFontA(int h, int w, int esc, int orient, int weight, u32 italic, u32 under, u32 strike, u32 cs, u32 op, u32 cp, u32 q, u32 pf, const char* face) {
    WCHAR wf[32] = {0};
    if (face) wide_from_utf8(face, -1, wf, 32);
    return CreateFontW(h, w, esc, orient, weight, italic, under, strike, cs, op, cp, q, pf, wf);
}
static HANDLE WINAPI CreateFontIndirectExW(const void* e) { return CreateFontIndirectW((const LOGFONTW*)e); }

// draw UTF-8 text: the kernel rasterizes coverage, this blends it in the text colour with the DC's clipping
void draw_text_utf8(DC* dc, int x, int y, const char* s, int n, const RECT* opaque, int* out_w) {
    Font* f = dc->st.font ? dc->st.font : font_default();
    int w = text_width_utf8(f, s, n) + dc->st.char_extra * n;
    if (out_w) *out_w = w;
    Surf d; int ox, oy; RECT clip;
    if (!dc_target(dc, &d, &ox, &oy, &clip)) return;
    if (opaque) {
        RECT r = *opaque;
        u32 bk = rgb_of(dc->st.bk_color);
        for (int yy = r.top + oy; yy < r.bottom + oy; yy++) span(dc, d, clip, r.left + ox, r.right + ox, yy, bk, 13);
    } else if (dc->st.bk_mode == 2) {
        u32 bk = rgb_of(dc->st.bk_color);
        for (int yy = y + oy; yy < y + oy + f->lh; yy++) span(dc, d, clip, x + ox, x + ox + w, yy, bk, 13);
    }
    if (!n) return;
    int bw = w + f->lh, bh = f->lh + 2;
    int dx0 = x + ox - f->lh / 4, dy0 = y + oy;
    // only rasterize what can be seen
    if (dx0 >= clip.right || dx0 + bw <= clip.left || dy0 >= clip.bottom || dy0 + bh <= clip.top) return;
    if (bw <= 0 || bh <= 0 || (i64)bw * bh > 16 * 1024 * 1024) return;
    u32* buf = (u32*)vm_alloc(nullptr, (usize)bw * bh * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return;
    si::TextArgs a;
    memset(&a, 0, sizeof(a));
    a.pixels = (u64)buf; a.w = bw; a.h = bh;
    a.x = f->lh / 4; a.baseline = f->asc;
    a.px100 = f->px100;
    a.flags = (f->bold ? si::TX_BOLD : 0) | (f->mono ? si::TX_MONO : 0);
    if (!dc->st.char_extra) si::text_raster(&a, s, n);
    else {
        // letter spacing: one character at a time
        for (int i = 0; i < n;) {
            int k = 1;
            while (i + k < n && ((u8)s[i + k] & 0xC0) == 0x80) k++;
            a.x += si::text_raster(&a, s + i, k) + dc->st.char_extra;
            i += k;
        }
    }
    u32 tc = rgb_of(dc->st.text_color);
    u32 tr = (tc >> 16) & 255, tg = (tc >> 8) & 255, tb = tc & 255;
    for (int yy = 0; yy < bh; yy++) {
        int dy = dy0 + yy;
        if (dy < clip.top || dy >= clip.bottom) continue;
        const u32* src = buf + (usize)yy * bw;
        for (int xx = 0; xx < bw; xx++) {
            u32 cov = src[xx] & 255;
            if (!cov) continue;
            int dx = dx0 + xx;
            if (dx < clip.left || dx >= clip.right) continue;
            u32 dv = fast32(d) ? row32(d, dy)[dx] : surf_get(d, dx, dy);
            u32 r = (dv >> 16) & 255, g = (dv >> 8) & 255, b = dv & 255;
            r += ((int)tr - (int)r) * (int)cov / 255;
            g += ((int)tg - (int)g) * (int)cov / 255;
            b += ((int)tb - (int)b) * (int)cov / 255;
            u32 out = (r << 16) | (g << 8) | b;
            if (fast32(d)) row32(d, dy)[dx] = out; else surf_put(d, dx, dy, out);
        }
    }
    if (f->lf.lfUnderline) for (int xx = 0; xx < w; xx++) { int dx = x + ox + xx, dy = y + oy + f->asc + 1; if (dx >= clip.left && dx < clip.right && dy >= clip.top && dy < clip.bottom) surf_put(d, dx, dy, tc); }
    if (f->lf.lfStrikeOut) for (int xx = 0; xx < w; xx++) { int dx = x + ox + xx, dy = y + oy + f->asc * 2 / 3; if (dx >= clip.left && dx < clip.right && dy >= clip.top && dy < clip.bottom) surf_put(d, dx, dy, tc); }
    vm_free(buf, 0, MEM_RELEASE);
}

// the position text goes to, from the alignment flags
static void aligned(DC* dc, int* x, int* y, int w) {
    Font* f = dc->st.font;
    int a = dc->st.text_align;
    if (a & 1) { *x = dc->st.pos.x; *y = dc->st.pos.y; }            // TA_UPDATECP
    if ((a & 6) == 6) *x -= w / 2;                                    // TA_CENTER
    else if (a & 2) *x -= w;                                          // TA_RIGHT
    if ((a & 24) == 24) *y -= f->asc;                                 // TA_BASELINE
    else if (a & 8) *y -= f->lh;                                      // TA_BOTTOM
}
static BOOL text_out(HDC h, int x, int y, const char* s, int n, const RECT* opaque, bool clipped) {
    DC* dc = dc_get(h);
    if (!dc) return FALSE;
    int w = text_width_utf8(dc->st.font, s, n) + dc->st.char_extra * n;
    aligned(dc, &x, &y, w);
    DCState saved = dc->st;
    if (clipped && opaque) IntersectClipRect(h, opaque->left, opaque->top, opaque->right, opaque->bottom);
    draw_text_utf8(dc, x, y, s, n, opaque, nullptr);
    bool keep_pos = dc->st.text_align & 1;
    dc->st = saved;
    if (keep_pos) dc->st.pos.x = x + ((dc->st.text_align & 6) == 2 ? 0 : w);
    return TRUE;
}
static BOOL WINAPI TextOutA(HDC h, int x, int y, const char* s, int n) { if (n < 0) return FALSE; return text_out(h, x, y, s, n, nullptr, false); }
static BOOL WINAPI TextOutW(HDC h, int x, int y, const WCHAR* s, int n) {
    if (n < 0) return FALSE;
    int k = utf8_from_wide(s, n, nullptr, 0);
    char* u = (char*)ialloc((usize)k + 1);
    utf8_from_wide(s, n, u, k);
    BOOL r = text_out(h, x, y, u, k, nullptr, false);
    ifree(u);
    return r;
}
static BOOL WINAPI ExtTextOutW(HDC h, int x, int y, u32 opts, const RECT* rc, const WCHAR* s, u32 n, const i32* dx) {
    (void)dx;
    int k = s ? utf8_from_wide(s, (int)n, nullptr, 0) : 0;
    char* u = (char*)ialloc((usize)k + 1);
    if (k) utf8_from_wide(s, (int)n, u, k);
    BOOL r = text_out(h, x, y, u, k, (opts & 2) ? rc : nullptr, (opts & 4) != 0);     // ETO_OPAQUE, ETO_CLIPPED
    ifree(u);
    return r;
}
static BOOL WINAPI ExtTextOutA(HDC h, int x, int y, u32 opts, const RECT* rc, const char* s, u32 n, const i32* dx) {
    (void)dx;
    return text_out(h, x, y, s ? s : "", s ? (int)n : 0, (opts & 2) ? rc : nullptr, (opts & 4) != 0);
}
static BOOL WINAPI GetTextExtentPoint32A(HDC h, const char* s, int n, SIZE* sz) {
    DC* dc = dc_get(h);
    Font* f = dc ? dc->st.font : font_default();
    sz->cx = text_width_utf8(f, s, n) + (dc ? dc->st.char_extra * n : 0);
    sz->cy = f->lh;
    return TRUE;
}
static BOOL WINAPI GetTextExtentPoint32W(HDC h, const WCHAR* s, int n, SIZE* sz) {
    int k = utf8_from_wide(s, n, nullptr, 0);
    char* u = (char*)ialloc((usize)k + 1);
    utf8_from_wide(s, n, u, k);
    DC* dc = dc_get(h);
    Font* f = dc ? dc->st.font : font_default();
    sz->cx = text_width_utf8(f, u, k) + (dc ? dc->st.char_extra * n : 0);
    sz->cy = f->lh;
    ifree(u);
    return TRUE;
}
static BOOL WINAPI GetTextExtentExPointW(HDC h, const WCHAR* s, int n, int max, int* fit, int* dx, SIZE* sz) {
    DC* dc = dc_get(h);
    Font* f = dc ? dc->st.font : font_default();
    int total = 0, fits = 0;
    for (int i = 0; i < n; i++) {
        char u[4];
        int k = utf8_from_wide(s + i, 1, u, 4);
        total += text_width_utf8(f, u, k);
        if (dx) dx[i] = total;
        if (total <= max) fits = i + 1;
    }
    if (fit) *fit = fits;
    sz->cx = total;
    sz->cy = f->lh;
    return TRUE;
}
static BOOL WINAPI GetTextExtentExPointA(HDC h, const char* s, int n, int max, int* fit, int* dx, SIZE* sz) {
    WCHAR* w = (WCHAR*)ialloc(((usize)n + 1) * 2);
    int k = wide_from_utf8(s, n, w, n + 1);
    BOOL r = GetTextExtentExPointW(h, w, k, max, fit, dx, sz);
    ifree(w);
    return r;
}
static BOOL WINAPI GetTextExtentPointA(HDC h, const char* s, int n, SIZE* sz) { return GetTextExtentPoint32A(h, s, n, sz); }
static BOOL WINAPI GetTextExtentPointW(HDC h, const WCHAR* s, int n, SIZE* sz) { return GetTextExtentPoint32W(h, s, n, sz); }
struct TEXTMETRICW {
    i32 tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth, tmMaxCharWidth, tmWeight, tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY;
    WCHAR tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    u8 tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
};
struct TEXTMETRICA {
    i32 tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth, tmMaxCharWidth, tmWeight, tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY;
    u8 tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    u8 tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
};
static void fill_tm(Font* f, TEXTMETRICW* tm) {
    memset(tm, 0, sizeof(*tm));
    tm->tmHeight = f->lh;
    tm->tmAscent = f->asc;
    tm->tmDescent = f->desc;
    tm->tmInternalLeading = f->lh - f->px100 / 100;
    tm->tmAveCharWidth = f->avgw;
    tm->tmMaxCharWidth = f->maxw;
    tm->tmWeight = f->bold ? 700 : 400;
    tm->tmDigitizedAspectX = tm->tmDigitizedAspectY = 96;
    tm->tmFirstChar = 32; tm->tmLastChar = 0xFFFC; tm->tmDefaultChar = '?'; tm->tmBreakChar = ' ';
    tm->tmItalic = f->lf.lfItalic; tm->tmUnderlined = f->lf.lfUnderline; tm->tmStruckOut = f->lf.lfStrikeOut;
    tm->tmPitchAndFamily = (u8)((f->mono ? 0x30 : 0x21) | 6);       // TMPF_VECTOR | TMPF_TRUETYPE (and not fixed pitch for proportional fonts)
    if (f->mono) tm->tmPitchAndFamily &= (u8)~1;
    tm->tmCharSet = 0;
}
static BOOL WINAPI GetTextMetricsW(HDC h, TEXTMETRICW* tm) { DC* dc = dc_get(h); fill_tm(dc ? dc->st.font : font_default(), tm); return TRUE; }
static BOOL WINAPI GetTextMetricsA(HDC h, TEXTMETRICA* tm) {
    TEXTMETRICW w;
    GetTextMetricsW(h, &w);
    memcpy(tm, &w, 44);
    tm->tmFirstChar = 32; tm->tmLastChar = 255; tm->tmDefaultChar = '?'; tm->tmBreakChar = ' ';
    tm->tmItalic = w.tmItalic; tm->tmUnderlined = w.tmUnderlined; tm->tmStruckOut = w.tmStruckOut; tm->tmPitchAndFamily = w.tmPitchAndFamily; tm->tmCharSet = 0;
    return TRUE;
}
static int WINAPI GetTextFaceW(HDC h, int n, WCHAR* out) {
    DC* dc = dc_get(h);
    if (!dc) return 0;
    const WCHAR* f = dc->st.font->lf.lfFaceName;
    int k = (int)wlen(f);
    if (!out) return k + 1;
    if (k >= n) k = n - 1;
    memcpy(out, f, (usize)k * 2);
    out[k] = 0;
    return k;
}
static int WINAPI GetTextFaceA(HDC h, int n, char* out) {
    DC* dc = dc_get(h);
    if (!dc) return 0;
    char u[64];
    utf8_from_wide(dc->st.font->lf.lfFaceName, -1, u, sizeof(u));
    if (!out) return (int)slen(u) + 1;
    slcpy(out, u, (usize)n);
    return (int)slen(out);
}
static BOOL WINAPI GetCharWidth32W(HDC h, u32 a, u32 b, i32* out) {
    DC* dc = dc_get(h);
    Font* f = dc ? dc->st.font : font_default();
    for (u32 c = a; c <= b; c++) { WCHAR w = (WCHAR)c; char u[4]; int k = utf8_from_wide(&w, 1, u, 4); out[c - a] = text_width_utf8(f, u, k); }
    return TRUE;
}
static BOOL WINAPI GetCharWidth32A(HDC h, u32 a, u32 b, i32* out) { return GetCharWidth32W(h, a, b, out); }
struct ABC { i32 a; u32 b; i32 c; };
static BOOL WINAPI GetCharABCWidthsW(HDC h, u32 a, u32 b, ABC* out) {
    for (u32 c = a; c <= b; c++) { i32 w; GetCharWidth32W(h, c, c, &w); out[c - a] = {0, (u32)w, 0}; }
    return TRUE;
}
static BOOL WINAPI GetCharABCWidthsA(HDC h, u32 a, u32 b, ABC* out) { return GetCharABCWidthsW(h, a, b, out); }
static u32 WINAPI GetFontData(HDC h, u32 table, u32 off, void* buf, u32 n) { (void)h; (void)table; (void)off; (void)buf; (void)n; return 0xFFFFFFFF; }
static u32 WINAPI GetGlyphOutlineW(HDC h, u32 c, u32 fmt, void* gm, u32 n, void* buf, const void* m) { (void)h; (void)c; (void)fmt; (void)gm; (void)n; (void)buf; (void)m; return 0xFFFFFFFF; }
static u32 WINAPI GetOutlineTextMetricsW(HDC h, u32 n, void* out) { (void)h; (void)n; (void)out; return 0; }
typedef int (WINAPI *FontEnumProc)(const void* lf, const void* tm, u32 type, LPARAM p);
static int enum_fonts(FontEnumProc fn, LPARAM p, bool wide) {
    static const char* faces[] = {"Arial", "Courier New", "Segoe UI", "Tahoma", "Times New Roman", "Consolas", "MS Sans Serif", "System"};
    int r = 1;
    for (const char* face : faces) {
        u8 lf[92];
        memset(lf, 0, sizeof(lf));
        LOGFONTW* l = (LOGFONTW*)lf;
        l->lfHeight = -12; l->lfWeight = 400;
        bool mono = sfind(face, "Courier") || sfind(face, "Consolas");
        l->lfPitchAndFamily = mono ? 0x31 : 0x22;
        if (wide) wide_from_utf8(face, -1, l->lfFaceName, 32); else slcpy((char*)lf + 28, face, 32);
        TEXTMETRICW tm;
        memset(&tm, 0, sizeof(tm));
        tm.tmHeight = 15; tm.tmAscent = 12; tm.tmDescent = 3; tm.tmAveCharWidth = 7; tm.tmMaxCharWidth = 12; tm.tmWeight = 400;
        r = fn(lf, &tm, 4, p);                                     // TRUETYPE_FONTTYPE
        if (!r) break;
    }
    return r;
}
static int WINAPI EnumFontFamiliesExW(HDC h, void* lf, FontEnumProc fn, LPARAM p, u32 f) { (void)h; (void)lf; (void)f; return enum_fonts(fn, p, true); }
static int WINAPI EnumFontFamiliesExA(HDC h, void* lf, FontEnumProc fn, LPARAM p, u32 f) { (void)h; (void)lf; (void)f; return enum_fonts(fn, p, false); }
static int WINAPI EnumFontFamiliesW(HDC h, const WCHAR* face, FontEnumProc fn, LPARAM p) { (void)h; (void)face; return enum_fonts(fn, p, true); }
static int WINAPI EnumFontFamiliesA(HDC h, const char* face, FontEnumProc fn, LPARAM p) { (void)h; (void)face; return enum_fonts(fn, p, false); }
static int WINAPI EnumFontsW(HDC h, const WCHAR* face, FontEnumProc fn, LPARAM p) { (void)h; (void)face; return enum_fonts(fn, p, true); }
static int WINAPI AddFontResourceExW(const WCHAR* n, u32 f, void* r) { (void)n; (void)f; (void)r; return 1; }
static int WINAPI AddFontResourceW(const WCHAR* n) { (void)n; return 1; }
static int WINAPI AddFontResourceA(const char* n) { (void)n; return 1; }
static BOOL WINAPI RemoveFontResourceW(const WCHAR* n) { (void)n; return TRUE; }
static HANDLE WINAPI AddFontMemResourceEx(void* d, u32 n, void* r, u32* count) { (void)d; (void)n; (void)r; if (count) *count = 1; static int dummy; return &dummy; }
static BOOL WINAPI RemoveFontMemResourceEx(HANDLE h) { (void)h; return TRUE; }
static u32 WINAPI GetFontLanguageInfo(HDC h) { (void)h; return 0; }
static u32 WINAPI GetCharacterPlacementW(HDC h, const WCHAR* s, int n, int max, void* res, u32 f) { (void)max; (void)res; (void)f; SIZE sz; GetTextExtentPoint32W(h, s, n, &sz); return (u32)(sz.cx | (sz.cy << 16)); }

// metafiles and printing are not supported; these keep programs that touch them running
static int WINAPI StartDocW(HDC h, const void* di) { (void)h; (void)di; return -1; }
static int WINAPI EndDoc(HDC h) { (void)h; return -1; }
static int WINAPI StartPage(HDC h) { (void)h; return -1; }
static int WINAPI EndPage(HDC h) { (void)h; return -1; }
static int WINAPI ChoosePixelFormat(HDC h, const void* pfd) { (void)h; (void)pfd; return 1; }
static BOOL WINAPI SetPixelFormat(HDC h, int f, const void* pfd) { (void)h; (void)f; (void)pfd; return TRUE; }
static int WINAPI DescribePixelFormat(HDC h, int f, u32 n, void* pfd) { (void)h; (void)f; if (pfd && n) memset(pfd, 0, n); return 1; }
static BOOL WINAPI SwapBuffers(HDC h) { (void)h; present_all(); return TRUE; }
static BOOL WINAPI BeginPath(HDC h) { (void)h; return TRUE; }
static BOOL WINAPI EndPath(HDC h) { (void)h; return TRUE; }
static BOOL WINAPI StrokePath(HDC h) { (void)h; return TRUE; }
static BOOL WINAPI FillPath(HDC h) { (void)h; return TRUE; }
static BOOL WINAPI StrokeAndFillPath(HDC h) { (void)h; return TRUE; }

static const Export gdi_exports[] = {
    E(GdiFlush), E(CreateCompatibleDC), E(CreateDCW), E(CreateDCA), E(CreateICW), E(DeleteDC), E(SaveDC), E(RestoreDC), E(GetStockObject),
    E(SelectObject), E(DeleteObject), E(GetCurrentObject), E(GetObjectW), E(GetObjectA), E(GetObjectType), E(CreateSolidBrush),
    E(CreateHatchBrush), E(CreatePatternBrush), E(CreateBrushIndirect), E(CreateDIBPatternBrushPt), E(CreatePen), E(CreatePenIndirect),
    E(ExtCreatePen), E(SetDCBrushColor), E(SetDCPenColor), E(GetDCBrushColor), E(GetDCPenColor), E(SetTextColor), E(GetTextColor),
    E(SetBkColor), E(GetBkColor), E(SetBkMode), E(GetBkMode), E(SetROP2), E(GetROP2), E(SetTextAlign), E(GetTextAlign),
    E(SetStretchBltMode), E(GetStretchBltMode), E(SetMapMode), E(GetMapMode), E(SetPolyFillMode), E(SetTextCharacterExtra),
    E(SetViewportOrgEx), E(SetWindowOrgEx), E(GetViewportOrgEx), E(GetWindowOrgEx), E(OffsetViewportOrgEx), E(OffsetWindowOrgEx),
    E(SetViewportExtEx), E(SetWindowExtEx), E(SetBrushOrgEx), E(GetCurrentPositionEx), E(DPtoLP), E(LPtoDP), E(SetGraphicsMode),
    E(SetWorldTransform), E(ModifyWorldTransform), E(SetArcDirection), E(GetDeviceCaps), E(MoveToEx), E(LineTo), E(Polyline),
    E(PolylineTo), E(PolyPolyline), E(Polygon), E(PolyPolygon), E(Rectangle), E(Ellipse), E(RoundRect), E(Arc), E(Pie), E(Chord),
    E(PolyBezier), E(SetPixel), E(SetPixelV), E(GetPixel), E(PatBlt), E(CreateCompatibleBitmap), E(CreateDiscardableBitmap),
    E(CreateBitmap), E(CreateBitmapIndirect), E(CreateDIBSection), E(BitBlt), E(StretchBlt), E(MaskBlt), E(StretchDIBits),
    E(SetDIBitsToDevice), E(GetDIBits), E(SetDIBits), E(CreateDIBitmap), E(GetBitmapBits), E(SetBitmapBits), E(GetBitmapDimensionEx),
    E(SetDIBColorTable), E(GetDIBColorTable), E(CreatePalette), E(CreateHalftonePalette), E(SelectPalette), E(RealizePalette),
    E(SetPaletteEntries), E(GetPaletteEntries), E(AnimatePalette), E(GetSystemPaletteEntries), E(GetNearestPaletteIndex),
    E(GetNearestColor), E(SetSystemPaletteUse), E(GetSystemPaletteUse), E(CreateRectRgn), E(CreateRectRgnIndirect),
    E(CreateRoundRectRgn), E(CreateEllipticRgn), E(CreatePolygonRgn), E(CombineRgn), E(SetRectRgn), E(OffsetRgn), E(GetRgnBox),
    E(PtInRegion), E(RectInRegion), E(EqualRgn), E(FillRgn), E(PaintRgn), E(FrameRgn), E(InvertRgn), E(SelectClipRgn), E(ExtSelectClipRgn),
    E(IntersectClipRect), E(ExcludeClipRect), E(OffsetClipRgn), E(GetClipBox), E(GetClipRgn), E(PtVisible), E(RectVisible),
    E(CreateFontIndirectW), E(CreateFontIndirectA), E(CreateFontW), E(CreateFontA), E(CreateFontIndirectExW), E(TextOutA), E(TextOutW),
    E(ExtTextOutW), E(ExtTextOutA), E(GetTextExtentPoint32A), E(GetTextExtentPoint32W), E(GetTextExtentExPointW), E(GetTextExtentExPointA),
    E(GetTextExtentPointA), E(GetTextExtentPointW), E(GetTextMetricsW), E(GetTextMetricsA), E(GetTextFaceW), E(GetTextFaceA),
    E(GetCharWidth32W), E(GetCharWidth32A), EA("GetCharWidthW", GetCharWidth32W), EA("GetCharWidthA", GetCharWidth32A), E(GetCharABCWidthsW),
    E(GetCharABCWidthsA), E(GetFontData), E(GetGlyphOutlineW), E(GetOutlineTextMetricsW), E(EnumFontFamiliesExW), E(EnumFontFamiliesExA),
    E(EnumFontFamiliesW), E(EnumFontFamiliesA), E(EnumFontsW), E(AddFontResourceExW), E(AddFontResourceW), E(AddFontResourceA),
    E(RemoveFontResourceW), E(AddFontMemResourceEx), E(RemoveFontMemResourceEx), E(GetFontLanguageInfo), E(GetCharacterPlacementW),
    E(StartDocW), E(EndDoc), E(StartPage), E(EndPage), E(ChoosePixelFormat), E(SetPixelFormat), E(DescribePixelFormat), E(SwapBuffers),
    E(BeginPath), E(EndPath), E(StrokePath), E(FillPath), E(StrokeAndFillPath),
};
EXPORT_TABLE("gdi32.dll", gdi_exports);
static const Export msimg_exports[] = { E(TransparentBlt), E(AlphaBlend), E(GradientFill) };
EXPORT_TABLE("msimg32.dll", msimg_exports);
static const Export gdi_more[] = { EA("GdiTransparentBlt", TransparentBlt), EA("GdiAlphaBlend", AlphaBlend), EA("GdiGradientFill", GradientFill) };
EXPORT_TABLE("gdi32.dll", gdi_more);

}  // namespace rt
