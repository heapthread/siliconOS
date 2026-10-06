// silicon - PDF: content stream interpreter, painting, text and the public rendering API
#include "pdf_gfx.h"
#include "../text.h"

enum { B_NORMAL, B_MULTIPLY, B_SCREEN, B_DARKEN, B_LIGHTEN, B_OVERLAY, B_DIFFERENCE };

// ================================================================ pixel blending
static inline u32 blend_px(u32 dst, u32 src, u32 a, int mode) {
    if (a == 0) return dst;
    u32 da = dst >> 24;
    u32 sr = (src >> 16) & 255, sg = (src >> 8) & 255, sb = src & 255;
    if (mode != B_NORMAL && da == 255) {
        u32 dr = (dst >> 16) & 255, dg = (dst >> 8) & 255, db = dst & 255;
        auto f = [&](u32 s, u32 d) -> u32 {
            switch (mode) {
            case B_MULTIPLY: return s * d / 255;
            case B_SCREEN: return s + d - s * d / 255;
            case B_DARKEN: return min(s, d);
            case B_LIGHTEN: return max(s, d);
            case B_OVERLAY: return d < 128 ? 2 * s * d / 255 : 255 - 2 * (255 - s) * (255 - d) / 255;
            case B_DIFFERENCE: return s > d ? s - d : d - s;
            }
            return s;
        };
        sr = f(sr, dr); sg = f(sg, dg); sb = f(sb, db);
    }
    if (da == 255) {
        u32 dr = (dst >> 16) & 255, dg = (dst >> 8) & 255, db = dst & 255;
        u32 ia = 255 - a;
        u32 r = (sr * a + dr * ia + 127) / 255, g = (sg * a + dg * ia + 127) / 255, b = (sb * a + db * ia + 127) / 255;
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    }
    u32 dr = (dst >> 16) & 255, dg = (dst >> 8) & 255, db = dst & 255;
    u32 oa = a + da * (255 - a) / 255;
    if (oa == 0) return 0;
    u32 wd = da * (255 - a) / 255;
    u32 r = (sr * a + dr * wd) / oa, g = (sg * a + dg * wd) / oa, b = (sb * a + db * wd) / oa;
    return (oa << 24) | (min(r, 255u) << 16) | (min(g, 255u) << 8) | min(b, 255u);
}

// ================================================================ state
enum { PK_SOLID, PK_SHADING, PK_TILING };
struct Paint {
    int kind = PK_SOLID;
    u32 rgb = 0xFF000000;
    Obj* pat = nullptr;              // pattern dictionary / stream
    u32 under = 0;                   // colour of an uncolored tiling pattern
};

struct GState {
    Mat ctm;
    Paint fp, sp;
    ColorSpace* fcs = nullptr; ColorSpace* scs = nullptr;
    double fa = 1, sa = 1;
    double lw = 1, miter = 10, dphase = 0;
    int cap = 0, join = 0;
    double dash[16]; int ndash = 0;
    ClipMask* clip = nullptr;
    int blend = B_NORMAL;
    // text state
    double Tc = 0, Tw = 0, Th = 1, Tl = 0, Ts = 0;
    int Tr = 0;
    PdfFont* font = nullptr;
    double fsize = 0;
};

struct TileEnt { Obj* pat; u32 under; Image img; double tw, th; };
struct ShadeEnt { Obj* pat; Shading* sh; };

struct R {
    PdfDoc* d;
    RenderOpts* o;
    Image* img;
    bool draw;
    Vec<GState> stack;
    GState gs;
    Path path;
    int clip_pending = 0;
    Mat tm, tlm;
    Path text_clip;
    Obj* res = nullptr;
    Mat base;
    int depth = 0;
    int hide = 0;
    Vec<u8> mc;
    bool uncolored = false;
    u32 budget = 4000000;
    Vec<TileEnt> tiles;
    Vec<ShadeEnt> shades;
    bool type3_d1 = false;
};

static void gs_init(R& r) {
    r.gs = GState();
    r.gs.ctm = r.base;
    r.gs.fcs = r.gs.scs = cs_device(1);
    r.gs.fp.rgb = r.gs.sp.rgb = 0xFF000000;
    r.gs.clip = ClipMask::make({0, 0, r.img ? r.img->w : 0, r.img ? r.img->h : 0});
}

// ================================================================ sinks
struct SolidSink : Sink {
    Image* img; ClipMask* clip; u32 rgb; u32 alpha; int mode;
    void row(int y, int x0, int x1, const u8* cov) override {
        u32* dst = img->px + (size_t)y * img->w;
        const u8* cm = clip->m ? clip->m + (size_t)(y - clip->b.y0) * (clip->b.x1 - clip->b.x0) - clip->b.x0 : nullptr;
        for (int x = x0; x < x1; x++) {
            u32 a = cov[x - x0];
            if (!a) continue;
            if (cm) { a = a * cm[x] / 255; if (!a) continue; }
            if (alpha != 255) a = a * alpha / 255;
            dst[x] = blend_px(dst[x], rgb, a, mode);
        }
    }
};

struct ShadeSink : Sink {
    Image* img; ClipMask* clip; Shading* sh; u32 alpha; int mode;
    bool has_bg; u32 bg;
    void row(int y, int x0, int x1, const u8* cov) override {
        u32* dst = img->px + (size_t)y * img->w;
        for (int x = x0; x < x1; x++) {
            u32 a = cov[x - x0];
            if (!a) continue;
            a = a * clip->at(x, y) / 255;
            if (!a) continue;
            u32 c;
            if (!shading_at(sh, x + 0.5, y + 0.5, &c)) { if (!has_bg) continue; c = bg; }
            if (alpha != 255) a = a * alpha / 255;
            dst[x] = blend_px(dst[x], c, a, mode);
        }
    }
};

struct TileSink : Sink {
    Image* img; ClipMask* clip; Image* tile; Mat inv; double bx, by, xs, ys; u32 alpha; int mode;
    void row(int y, int x0, int x1, const u8* cov) override {
        u32* dst = img->px + (size_t)y * img->w;
        for (int x = x0; x < x1; x++) {
            u32 a = cov[x - x0];
            if (!a) continue;
            a = a * clip->at(x, y) / 255;
            if (!a) continue;
            double px, py;
            mat_apply(inv, x + 0.5, y + 0.5, &px, &py);
            double u = (px - bx) / xs, v = (py - by) / ys;
            u -= m_floor(u); v -= m_floor(v);
            int tx = (int)(u * tile->w), ty = (int)((1 - v) * tile->h);
            if (tx >= tile->w) tx = tile->w - 1;
            if (ty >= tile->h) ty = tile->h - 1;
            if (ty < 0) ty = 0;
            u32 c = tile->px[(size_t)ty * tile->w + tx];
            u32 ca = c >> 24;
            if (!ca) continue;
            a = a * ca / 255;
            if (alpha != 255) a = a * alpha / 255;
            dst[x] = blend_px(dst[x], c, a, mode);
        }
    }
};

struct ImageSink : Sink {
    Image* img; ClipMask* clip; const Image* src; Mat inv; u32 alpha; int mode; bool smooth;
    void row(int y, int x0, int x1, const u8* cov) override {
        u32* dst = img->px + (size_t)y * img->w;
        int sw = src->w, sh = src->h;
        for (int x = x0; x < x1; x++) {
            u32 a = cov[x - x0];
            if (!a) continue;
            a = a * clip->at(x, y) / 255;
            if (!a) continue;
            double u, v;
            mat_apply(inv, x + 0.5, y + 0.5, &u, &v);
            u32 c;
            if (!smooth) {
                int ix = (int)u, iy = (int)v;
                if (ix < 0) ix = 0; if (ix >= sw) ix = sw - 1;
                if (iy < 0) iy = 0; if (iy >= sh) iy = sh - 1;
                c = src->px[(size_t)iy * sw + ix];
            } else {
                double fx = u - 0.5, fy = v - 0.5;
                int ix = (int)m_floor(fx), iy = (int)m_floor(fy);
                u32 wx = (u32)((fx - ix) * 256), wy = (u32)((fy - iy) * 256);
                int x1i = ix + 1, y1i = iy + 1;
                if (ix < 0) ix = 0; if (ix >= sw) ix = sw - 1;
                if (x1i < 0) x1i = 0; if (x1i >= sw) x1i = sw - 1;
                if (iy < 0) iy = 0; if (iy >= sh) iy = sh - 1;
                if (y1i < 0) y1i = 0; if (y1i >= sh) y1i = sh - 1;
                u32 c00 = src->px[(size_t)iy * sw + ix], c10 = src->px[(size_t)iy * sw + x1i];
                u32 c01 = src->px[(size_t)y1i * sw + ix], c11 = src->px[(size_t)y1i * sw + x1i];
                u64 w00 = (256 - wx) * (256 - wy), w10 = wx * (256 - wy), w01 = (256 - wx) * wy, w11 = wx * wy;
                u64 ws[4] = {w00, w10, w01, w11};
                u32 cs4[4] = {c00, c10, c01, c11};
                u64 sa = 0, sr = 0, sg = 0, sb = 0;
                for (int k = 0; k < 4; k++) {
                    u64 wa = ws[k] * (cs4[k] >> 24);
                    sa += wa;
                    sr += wa * ((cs4[k] >> 16) & 255);
                    sg += wa * ((cs4[k] >> 8) & 255);
                    sb += wa * (cs4[k] & 255);
                }
                u32 aa = (u32)(sa >> 16);
                if (aa == 0) continue;
                c = (aa << 24) | ((u32)(sr / sa) << 16) | ((u32)(sg / sa) << 8) | (u32)(sb / sa);
            }
            u32 ca = c >> 24;
            if (!ca) continue;
            a = a * ca / 255;
            if (alpha != 255) a = a * alpha / 255;
            dst[x] = blend_px(dst[x], c, a, mode);
        }
    }
};

// ================================================================ pattern helpers
static Shading* get_shading(R& r, Obj* pat_or_sh, bool is_pattern_dict) {
    for (int i = 0; i < r.shades.len; i++) if (r.shades[i].pat == pat_or_sh) return r.shades[i].sh;
    Obj* sd = is_pattern_dict ? dget(r.d, pat_or_sh, "Shading") : pat_or_sh;
    Shading* s = shading_parse(r.d, sd, r.res);
    ShadeEnt e; e.pat = pat_or_sh; e.sh = s;
    r.shades.push(e);
    return s;
}

static void run_stream(R& r, const u8* data, u32 len);

static Image* get_tile(R& r, Obj* pat, u32 under, const Mat& pm, double* tw_out, double* th_out) {
    double xs = onum(dget(r.d, pat, "XStep"), 0), ys = onum(dget(r.d, pat, "YStep"), 0);
    Obj* bb = dget(r.d, pat, "BBox");
    double bx0 = onum(aget(r.d, bb, 0), 0), by0 = onum(aget(r.d, bb, 1), 0), bx1 = onum(aget(r.d, bb, 2), 1), by1 = onum(aget(r.d, bb, 3), 1);
    if (xs == 0) xs = bx1 - bx0;
    if (ys == 0) ys = by1 - by0;
    if (xs < 0) xs = -xs;
    if (ys < 0) ys = -ys;
    if (xs < 1e-6 || ys < 1e-6) return nullptr;
    for (int i = 0; i < r.tiles.len; i++) if (r.tiles[i].pat == pat && r.tiles[i].under == under) { *tw_out = xs; *th_out = ys; return &r.tiles[i].img; }
    double sx = m_sqrt(pm.a * pm.a + pm.b * pm.b), sy = m_sqrt(pm.c * pm.c + pm.d * pm.d);
    int tw = clamp((int)m_ceil(xs * sx), 1, 1500), th = clamp((int)m_ceil(ys * sy), 1, 1500);
    TileEnt te;
    te.pat = pat; te.under = under; te.tw = xs; te.th = ys;
    te.img.w = tw; te.img.h = th;
    te.img.px = (u32*)kcalloc((size_t)tw * th * 4);
    // render the cell
    R sub;
    sub.d = r.d; sub.o = r.o; sub.img = &te.img; sub.draw = true;
    sub.depth = r.depth + 1;
    sub.res = dget(r.d, pat, "Resources");
    if (!sub.res) sub.res = r.res;
    Mat cell;
    cell.a = tw / xs; cell.b = 0; cell.c = 0; cell.d = -th / ys;
    cell.e = -bx0 * tw / xs; cell.f = th + by0 * th / ys;
    sub.base = cell;
    gs_init(sub);
    int paint_type = (int)onum(dget(r.d, pat, "PaintType"), 1);
    if (paint_type == 2) { sub.gs.fp.rgb = sub.gs.sp.rgb = under | 0xFF000000; sub.uncolored = true; }
    if (r.depth < 4 && pat->type == O_STREAM) {
        u32 len;
        u8* data = pdf_stream_copy(r.d, pat, &len);
        run_stream(sub, data, len);
        kfree(data);
    }
    sub.gs.clip->unref();
    for (int i = 0; i < sub.stack.len; i++) sub.stack[i].clip->unref();
    sub.stack.free();
    for (int i = 0; i < sub.tiles.len; i++) kfree(sub.tiles[i].img.px);
    sub.tiles.free();
    for (int i = 0; i < sub.shades.len; i++) shading_free(sub.shades[i].sh);
    sub.shades.free();
    r.tiles.push(te);
    *tw_out = xs; *th_out = ys;
    return &r.tiles[r.tiles.len - 1].img;
}

static bool pattern_matrix(R& r, Obj* pat, Mat* m) {
    Obj* mo = dget(r.d, pat, "Matrix");
    Mat pm;
    if (mo && mo->type == O_ARR && mo->arr.n >= 6) {
        pm.a = onum(aget(r.d, mo, 0), 1); pm.b = onum(aget(r.d, mo, 1), 0); pm.c = onum(aget(r.d, mo, 2), 0);
        pm.d = onum(aget(r.d, mo, 3), 1); pm.e = onum(aget(r.d, mo, 4), 0); pm.f = onum(aget(r.d, mo, 5), 0);
    }
    *m = mat_mul(pm, r.base);
    return true;
}

// ================================================================ painting
static bool clip_rect_for(R& r, IRect* cb) {
    *cb = irect_isect(r.gs.clip->b, {0, 0, r.img->w, r.img->h});
    return !irect_empty(*cb);
}

static void paint_dev_path(R& r, const Path& dp, bool eo, bool stroke_paint, double alpha_scale = 1.0) {
    if (!r.draw || r.hide || dp.s.len < 2) return;
    IRect cb;
    if (!clip_rect_for(r, &cb)) return;
    const Paint& p = stroke_paint ? r.gs.sp : r.gs.fp;
    double al = (stroke_paint ? r.gs.sa : r.gs.fa) * alpha_scale;
    u32 a255 = (u32)clamp((int)(al * 255 + 0.5), 0, 255);
    if (a255 == 0) return;
    if (p.kind == PK_SOLID) {
        SolidSink s;
        s.img = r.img; s.clip = r.gs.clip; s.rgb = p.rgb; s.alpha = a255; s.mode = r.gs.blend;
        raster_fill(dp, eo, cb, s);
    } else if (p.kind == PK_SHADING) {
        Shading* sh = get_shading(r, p.pat, true);
        if (!sh) return;
        Mat pm;
        pattern_matrix(r, p.pat, &pm);
        shading_set_matrix(sh, pm);
        ShadeSink s;
        s.img = r.img; s.clip = r.gs.clip; s.sh = sh; s.alpha = a255; s.mode = r.gs.blend;
        s.bg = shading_background(sh, &s.has_bg);
        raster_fill(dp, eo, cb, s);
    } else {
        Mat pm;
        pattern_matrix(r, p.pat, &pm);
        double xs, ys;
        Image* tile = get_tile(r, p.pat, p.under, pm, &xs, &ys);
        if (!tile) return;
        Obj* bb = dget(r.d, p.pat, "BBox");
        bool ok;
        TileSink s;
        s.img = r.img; s.clip = r.gs.clip; s.tile = tile; s.inv = mat_inv(pm, &ok);
        if (!ok) return;
        s.bx = onum(aget(r.d, bb, 0), 0); s.by = onum(aget(r.d, bb, 1), 0); s.xs = xs; s.ys = ys;
        s.alpha = a255; s.mode = r.gs.blend;
        raster_fill(dp, eo, cb, s);
    }
}

static void fill_path(R& r, bool eo) { paint_dev_path(r, r.path, eo, false); }

static void stroke_dev(R& r, const Path& dp) {
    if (!r.draw || r.hide) return;
    double sc = m_sqrt(m_fabs(r.gs.ctm.a * r.gs.ctm.d - r.gs.ctm.b * r.gs.ctm.c));
    double w = r.gs.lw * sc;
    double alpha_scale = 1.0;
    if (w < 1.0) { alpha_scale = max(0.45, w); w = 1.0; }
    double dash[16];
    for (int i = 0; i < r.gs.ndash; i++) dash[i] = r.gs.dash[i] * sc;
    Path poly;
    stroke_path(dp, w, r.gs.cap, r.gs.join, r.gs.miter, dash, r.gs.ndash, r.gs.dphase * sc, poly);
    paint_dev_path(r, poly, false, true, alpha_scale);
    poly.free();
}

static void end_path(R& r) {
    if (r.clip_pending && r.draw) {
        ClipMask* nc = clip_intersect_path(r.gs.clip, r.path, r.clip_pending == 2);
        r.gs.clip->unref();
        r.gs.clip = nc;
    }
    r.clip_pending = 0;
    r.path.clear();
}

// ================================================================ images
static void box_shrink(Image& im, int nw, int nh) {
    if (nw >= im.w && nh >= im.h) return;
    nw = max(1, min(nw, im.w)); nh = max(1, min(nh, im.h));
    u32* out = (u32*)kmalloc_try((size_t)nw * nh * 4);
    if (!out) return;
    for (int y = 0; y < nh; y++) {
        int y0 = (int)((i64)y * im.h / nh), y1 = max(y0 + 1, (int)((i64)(y + 1) * im.h / nh));
        for (int x = 0; x < nw; x++) {
            int x0 = (int)((i64)x * im.w / nw), x1 = max(x0 + 1, (int)((i64)(x + 1) * im.w / nw));
            u32 a = 0, rr = 0, gg = 0, bb = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const u32* row = im.px + (size_t)yy * im.w;
                for (int xx = x0; xx < x1; xx++) {
                    u32 c = row[xx], ca = c >> 24;
                    a += ca; rr += ((c >> 16) & 255) * ca; gg += ((c >> 8) & 255) * ca; bb += (c & 255) * ca; n++;
                }
            }
            if (a == 0) { out[(size_t)y * nw + x] = 0; continue; }
            out[(size_t)y * nw + x] = ((a / n) << 24) | ((rr / a) << 16) | ((gg / a) << 8) | (bb / a);
        }
    }
    kfree(im.px);
    im.px = out; im.w = nw; im.h = nh;
}

static void draw_image_obj(R& r, Image& im) {
    if (!im.px || im.w < 1 || im.h < 1) return;
    if (!r.draw || r.hide) { kfree(im.px); return; }
    const Mat& m = r.gs.ctm;
    double wd = m_sqrt(m.a * m.a + m.b * m.b), hd = m_sqrt(m.c * m.c + m.d * m.d);
    if (wd < 0.01 || hd < 0.01) { kfree(im.px); return; }
    if (im.w > wd * 1.5 + 1 || im.h > hd * 1.5 + 1) box_shrink(im, (int)m_ceil(wd) + 1, (int)m_ceil(hd) + 1);
    // device -> image pixel space: image row 0 is the top of the unit square
    bool ok;
    Mat inv = mat_inv(m, &ok);
    if (!ok) { kfree(im.px); return; }
    Mat toimg;
    toimg.a = im.w; toimg.b = 0; toimg.c = 0; toimg.d = -im.h; toimg.e = 0; toimg.f = im.h;     // unit -> pixels
    Mat dev2img = mat_mul(inv, toimg);
    Path quad;
    quad.rect(0, 0, 1, 1);
    Path dq;
    dq.append(quad, m);
    quad.free();
    IRect cb;
    if (clip_rect_for(r, &cb)) {
        u32 a255 = (u32)clamp((int)(r.gs.fa * 255 + 0.5), 0, 255);
        ImageSink s;
        s.img = r.img; s.clip = r.gs.clip; s.src = &im; s.inv = dev2img; s.alpha = a255; s.mode = r.gs.blend;
        s.smooth = !(im.w <= 2 && im.h <= 2) || true;
        raster_fill(dq, false, cb, s);
    }
    dq.free();
    kfree(im.px);
    im.px = nullptr;
}

// ================================================================ text
struct TextCtx;
static void show_glyph_fallback(R& r, PdfFont* f, u32 code, double w0, const Mat& trm);

static void emit_text_chars(R& r, PdfFont* f, u32 code, double w0, const Mat& trm, bool is_space) {
    if (!r.o->text) return;
    u32 cps[4];
    int n = font_unicodes(f, code, cps);
    if (n == 0) { if (is_space) { cps[0] = 32; n = 1; } else return; }
    double a = font_ascent(f), dd = font_descent(f);
    if (a < 0.6) a = 0.8;
    if (dd > -0.1) dd = -0.2;
    if (a > 1.2) a = 1.0;
    if (dd < -0.5) dd = -0.3;
    double xs[4], ys[4];
    double cx[4] = {0, w0, w0, 0}, cy[4] = {dd, dd, a, a};
    if (w0 <= 0) { cx[1] = cx[2] = 0.3; }
    for (int i = 0; i < 4; i++) mat_apply(trm, cx[i], cy[i], &xs[i], &ys[i]);
    float x0 = (float)min(min(xs[0], xs[1]), min(xs[2], xs[3])), x1 = (float)max(max(xs[0], xs[1]), max(xs[2], xs[3]));
    float y0 = (float)min(min(ys[0], ys[1]), min(ys[2], ys[3])), y1 = (float)max(max(ys[0], ys[1]), max(ys[2], ys[3]));
    for (int i = 0; i < n; i++) {
        PdfChar c;
        c.x0 = x0 + (x1 - x0) * i / n; c.x1 = x0 + (x1 - x0) * (i + 1) / n;
        c.y0 = y0; c.y1 = y1;
        c.cp = cps[i];
        r.o->text->chars.push(c);
    }
}

static void run_type3_glyph(R& r, PdfFont* f, u32 code, const Mat& trm) {
    Obj* proc = font_type3_proc(f, code);
    if (!proc || proc->type != O_STREAM || r.depth > 6) return;
    double fm[6];
    font_type3_matrix(f, fm);
    Mat fmat; fmat.a = fm[0]; fmat.b = fm[1]; fmat.c = fm[2]; fmat.d = fm[3]; fmat.e = fm[4]; fmat.f = fm[5];
    GState saved = r.gs;
    saved.clip->ref();
    Obj* saved_res = r.res;
    Path saved_path = r.path;
    r.path = Path();
    bool saved_unc = r.uncolored;
    Mat saved_tm = r.tm, saved_tlm = r.tlm;
    r.gs.ctm = mat_mul(fmat, trm);
    Obj* t3res = font_type3_dict(f);
    if (t3res) r.res = t3res;
    r.gs.lw = 1; r.gs.cap = r.gs.join = 0; r.gs.ndash = 0;
    r.depth++;
    u32 len;
    u8* data = pdf_stream_copy(r.d, proc, &len);
    int sp0 = r.stack.len;
    run_stream(r, data, len);
    while (r.stack.len > sp0) { r.gs.clip->unref(); r.gs = r.stack.back(); r.stack.len--; }
    kfree(data);
    r.depth--;
    r.path.free();
    r.path = saved_path;
    r.gs.clip->unref();
    r.gs = saved;
    r.res = saved_res;
    r.uncolored = saved_unc;
    r.tm = saved_tm; r.tlm = saved_tlm;
}

static void show_string(R& r, const u8* s, int n) {
    PdfFont* f = r.gs.font;
    if (!f) return;
    bool vert = font_vertical(f);
    int pos = 0;
    while (pos < n) {
        u32 code;
        int l = font_next_code(f, s, n, pos, &code);
        pos += l;
        double w0 = font_width(f, code);
        bool is_space = l == 1 && code == 32;
        Mat sm;
        sm.a = r.gs.fsize * r.gs.Th; sm.d = r.gs.fsize; sm.f = r.gs.Ts;
        Mat trm = mat_mul(sm, mat_mul(r.tm, r.gs.ctm));
        if (vert) {
            double w1, vx, vy;
            font_vmetrics(f, code, &w1, &vx, &vy);
            Mat sh; sh.e = -vx; sh.f = -vy;
            trm = mat_mul(sh, trm);
        }
        r.budget = r.budget > 4 ? r.budget - 4 : 0;
        emit_text_chars(r, f, code, w0, trm, is_space);
        if (r.draw && !r.hide && r.gs.Tr != 3 && (r.gs.Tr < 4 || r.gs.Tr != 7 || true)) {
            if (font_is_type3(f)) {
                if (r.gs.Tr != 3) run_type3_glyph(r, f, code, trm);
            } else {
                Path gp;
                if (font_glyph(f, code, gp)) {
                    Path dp;
                    dp.append(gp, trm);
                    int mode = r.gs.Tr & 3;
                    if (mode == 0 || mode == 2) paint_dev_path(r, dp, false, false);
                    if (mode == 1 || mode == 2) {
                        double scale = m_sqrt(m_fabs(trm.a * trm.d - trm.b * trm.c));
                        (void)scale;
                        stroke_dev(r, dp);
                    }
                    if (r.gs.Tr >= 4) r.text_clip.append(dp, Mat());
                    dp.free();
                } else if (!font_has_outlines(f)) {
                    show_glyph_fallback(r, f, code, w0, trm);
                }
                gp.free();
            }
        }
        if (vert) {
            double w1, vx, vy;
            font_vmetrics(f, code, &w1, &vx, &vy);
            double ty = w1 * r.gs.fsize + r.gs.Tc + (is_space ? r.gs.Tw : 0);
            Mat t; t.f = ty;
            r.tm = mat_mul(t, r.tm);
        } else {
            double tx = (w0 * r.gs.fsize + r.gs.Tc + (is_space ? r.gs.Tw : 0)) * r.gs.Th;
            Mat t; t.e = tx;
            r.tm = mat_mul(t, r.tm);
        }
    }
}

// non-embedded fonts: scale up the silicon bitmap font smoothly
extern const u8 font_rows[256][12];
static void show_glyph_fallback(R& r, PdfFont* f, u32 code, double w0, const Mat& trm) {
    int ch;
    if (!font_fallback_cell(f, code, &ch)) return;
    static Image cell[256];
    static u8 have[256];
    bool bold = font_bold(f);
    int key = ch;
    Image* im = &cell[key];
    const int PW = 12, PH = 16;                       // 8x12 bitmap with room around it
    if (!have[key]) {
        have[key] = 1;
        im->w = PW; im->h = PH;
        im->px = (u32*)kcalloc((size_t)PW * PH * 4);
        for (int y = 0; y < 12; y++) {
            u8 bits = font_rows[ch][y];
            for (int x = 0; x < 8; x++) {
                if (bits & (0x80 >> x)) {
                    im->px[(y + 2) * PW + x + 2] = 0xFF000000;
                    if (x < 7 && bold) im->px[(y + 2) * PW + x + 3] = 0xFF000000;
                }
            }
        }
    }
    // glyph box in em units: baseline at pixel row 10 of the 8x12 cell, 0.09 em per row
    double adv = w0 > 0.05 ? w0 : 0.5;
    double gw = adv;
    double shear = font_italic(f) ? 0.2 : 0.0;
    // image pixel (px,py) -> em: x = (px-2)/8 * gw, y = (12 - py) * 0.09 (py counts from the top of the padded image)
    // image pixel (px,py) -> em: x = (px-2)/8 * gw, y = (12 - py) * 0.09 (py counts from the top of the padded image)
    Mat to_em;
    to_em.a = gw / 8.0; to_em.b = 0; to_em.c = shear * 0.09 * -1; to_em.d = 0;
    // x_em = (px - 2) * gw/8 + shear * y_em ; y_em = (12 - py) * 0.09
    Mat im2em;
    im2em.a = gw / 8.0; im2em.b = 0;
    im2em.c = -shear * 0.09; im2em.d = -0.09;
    im2em.e = -2 * gw / 8.0 + shear * 12 * 0.09; im2em.f = 12 * 0.09;
    Mat im2dev = mat_mul(im2em, trm);
    bool ok;
    Mat inv = mat_inv(im2dev, &ok);
    if (!ok) return;
    // coverage quad: the padded image rectangle in device space
    Path quad, dq;
    quad.rect(0, 0, (float)PW, (float)PH);
    dq.append(quad, im2dev);
    quad.free();
    IRect cb;
    if (clip_rect_for(r, &cb)) {
        ImageSink s;
        s.img = r.img; s.clip = r.gs.clip; s.src = im; s.inv = inv; s.alpha = 255; s.mode = r.gs.blend; s.smooth = true;
        // tint with the fill colour: use a SolidSink-like wrapper
        struct Tint : Sink {
            ImageSink* is; R* rr; bool down;
            void row(int y, int x0, int x1, const u8* cov) override {
                u32* dst = rr->img->px + (size_t)y * rr->img->w;
                for (int x = x0; x < x1; x++) {
                    u32 a = cov[x - x0];
                    if (!a) continue;
                    a = a * is->clip->at(x, y) / 255;
                    if (!a) continue;
                    auto at = [&](int xx, int yy) -> double {
                        if (xx < 0 || yy < 0 || xx >= PW || yy >= PH) return 0;
                        return (is->src->px[yy * PW + xx] >> 24) / 255.0;
                    };
                    double c;
                    if (down) {                                        // minifying: average a few samples inside the device pixel
                        c = 0;
                        for (int sy = 0; sy < 3; sy++)
                            for (int sx = 0; sx < 3; sx++) {
                                double u, v;
                                mat_apply(is->inv, x + (sx + 0.5) / 3, y + (sy + 0.5) / 3, &u, &v);
                                c += at((int)m_floor(u), (int)m_floor(v));
                            }
                        c = c / 9.0 * 1.35;
                    } else {
                        double u, v;
                        mat_apply(is->inv, x + 0.5, y + 0.5, &u, &v);
                        double fx = u - 0.5, fy = v - 0.5;
                        int ix = (int)m_floor(fx), iy = (int)m_floor(fy);
                        double wx = fx - ix, wy = fy - iy;
                        c = at(ix, iy) * (1 - wx) * (1 - wy) + at(ix + 1, iy) * wx * (1 - wy) + at(ix, iy + 1) * (1 - wx) * wy + at(ix + 1, iy + 1) * wx * wy;
                        c = (c - 0.3) * 2.0;
                    }
                    if (c <= 0) continue;
                    if (c > 1) c = 1;
                    a = (u32)(a * c);
                    if (!a) continue;
                    dst[x] = blend_px(dst[x], rr->gs.fp.rgb, a * (u32)(rr->gs.fa * 255) / 255, rr->gs.blend);
                }
            }
        } t;
        t.is = &s; t.rr = &r;
        t.down = m_sqrt(m_fabs(inv.a * inv.d - inv.b * inv.c)) > 0.85;
        raster_fill(dq, false, cb, t);
    }
    dq.free();
}

// ================================================================ colour operators
static void set_color_space(R& r, bool stroke, Obj* name) {
    ColorSpace* cs = cs_parse(r.d, name, r.res);
    if (stroke) r.gs.scs = cs; else r.gs.fcs = cs;
    Paint& p = stroke ? r.gs.sp : r.gs.fp;
    double c[8];
    cs_initial(cs, c);
    if (cs->kind == CS_PATTERN) { p.kind = PK_SOLID; p.rgb = 0xFF000000; return; }
    p.kind = PK_SOLID;
    p.rgb = cs_rgb(cs, c);
}

static void set_color(R& r, bool stroke, Vec<Obj*>& ops) {
    if (r.uncolored) return;
    ColorSpace* cs = stroke ? r.gs.scs : r.gs.fcs;
    Paint& p = stroke ? r.gs.sp : r.gs.fp;
    if (cs->kind == CS_PATTERN) {
        if (!ops.len || ops.back()->type != O_NAME) return;
        Obj* pats = dget(r.d, r.res, "Pattern");
        Obj* pat = dget(r.d, pats, (const char*)ops.back()->str.s);
        if (!pat) return;
        int pt = (int)onum(dget(r.d, pat, "PatternType"), 1);
        p.pat = pat;
        p.kind = pt == 2 ? PK_SHADING : PK_TILING;
        p.under = 0xFF000000;
        if (cs->base && ops.len > 1) {
            double c[8];
            for (int i = 0; i < 8; i++) c[i] = i < ops.len - 1 ? onum(ops[i], 0) : 0;
            p.under = cs_rgb(cs->base, c);
        }
        return;
    }
    double c[8];
    int n = cs->n;
    for (int i = 0; i < 8; i++) c[i] = i < ops.len && ops[i]->type == O_NUM ? ops[i]->n : 0;
    if (ops.len < n) return;
    p.kind = PK_SOLID;
    if (cs->kind == CS_SEP && cs->none) { p.rgb = 0x00000000; return; }
    p.rgb = cs_rgb(cs, c);
}

static void set_device_color(R& r, bool stroke, int n, Vec<Obj*>& ops) {
    if (r.uncolored) return;
    if (ops.len < n) return;
    double c[4];
    for (int i = 0; i < n; i++) c[i] = onum(ops[ops.len - n + i], 0);
    ColorSpace* cs = cs_device(n);
    if (stroke) r.gs.scs = cs; else r.gs.fcs = cs;
    Paint& p = stroke ? r.gs.sp : r.gs.fp;
    p.kind = PK_SOLID;
    p.rgb = cs_rgb(cs, c);
}

// ================================================================ xobjects
static bool oc_hidden(R& r, Obj* oc) {
    if (!oc || r.d->n_hidden == 0) return false;
    auto ocg_hidden = [&](u32 num) { for (int i = 0; i < r.d->n_hidden; i++) if (r.d->hidden_ocgs[i] == num) return true; return false; };
    if (oc->type == O_REF) {
        if (ocg_hidden(oc->ref.num)) return true;
        Obj* o = pdf_res(r.d, oc);
        if (is_dict(o) && is_name(dget(r.d, o, "Type"), "OCMD")) {
            Obj* g = draw_get(o, "OCGs");
            if (g && g->type == O_REF) return ocg_hidden(g->ref.num);
            Obj* ga = pdf_res(r.d, g);
            if (ga && ga->type == O_ARR) {
                bool any_visible = false;
                for (u32 i = 0; i < ga->arr.n; i++) if (ga->arr.v[i]->type == O_REF && !ocg_hidden(ga->arr.v[i]->ref.num)) any_visible = true;
                return !any_visible && ga->arr.n > 0;
            }
        }
    }
    return false;
}

static void do_form(R& r, Obj* form) {
    if (r.depth > 10 || form->type != O_STREAM) return;
    if (oc_hidden(r, draw_get(form, "OC"))) return;
    GState saved = r.gs;
    saved.clip->ref();
    Obj* saved_res = r.res;
    Path saved_path = r.path;
    r.path = Path();
    Mat saved_tm = r.tm, saved_tlm = r.tlm;
    Obj* mo = dget(r.d, form, "Matrix");
    if (mo && mo->type == O_ARR && mo->arr.n >= 6) {
        Mat fm;
        fm.a = onum(aget(r.d, mo, 0), 1); fm.b = onum(aget(r.d, mo, 1), 0); fm.c = onum(aget(r.d, mo, 2), 0);
        fm.d = onum(aget(r.d, mo, 3), 1); fm.e = onum(aget(r.d, mo, 4), 0); fm.f = onum(aget(r.d, mo, 5), 0);
        r.gs.ctm = mat_mul(fm, r.gs.ctm);
    }
    Obj* bb = dget(r.d, form, "BBox");
    if (bb && bb->type == O_ARR && bb->arr.n >= 4) {
        double x0 = onum(aget(r.d, bb, 0), 0), y0 = onum(aget(r.d, bb, 1), 0), x1 = onum(aget(r.d, bb, 2), 0), y1 = onum(aget(r.d, bb, 3), 0);
        Path rc;
        rc.move((float)x0, (float)y0); rc.line((float)x1, (float)y0); rc.line((float)x1, (float)y1); rc.line((float)x0, (float)y1); rc.close();
        Path dr;
        dr.append(rc, r.gs.ctm);
        ClipMask* nc = clip_intersect_path(r.gs.clip, dr, false);
        r.gs.clip->unref();
        r.gs.clip = nc;
        rc.free(); dr.free();
    }
    Obj* fres = dget(r.d, form, "Resources");
    if (fres) r.res = fres;
    r.depth++;
    Mat saved_base = r.base;
    u32 len;
    u8* data = pdf_stream_copy(r.d, form, &len);
    int sp0 = r.stack.len;
    run_stream(r, data, len);
    while (r.stack.len > sp0) { r.gs.clip->unref(); r.gs = r.stack.back(); r.stack.len--; }
    kfree(data);
    r.depth--;
    r.base = saved_base;
    r.path.free();
    r.path = saved_path;
    r.gs.clip->unref();
    r.gs = saved;
    r.res = saved_res;
    r.tm = saved_tm; r.tlm = saved_tlm;
}

static void do_xobject(R& r, const char* name) {
    Obj* xs = dget(r.d, r.res, "XObject");
    Obj* x = dget(r.d, xs, name);
    if (!x || x->type != O_STREAM) return;
    const char* st = oname(dget(r.d, x, "Subtype"));
    if (!strcmp(st, "Image")) {
        if (!r.draw || r.hide) return;
        if (oc_hidden(r, draw_get(x, "OC"))) return;
        Image im;
        if (pdf_decode_image(r.d, x, nullptr, 0, r.res, r.gs.fp.rgb, &im)) draw_image_obj(r, im);
    } else if (!strcmp(st, "Form")) {
        do_form(r, x);
    }
}

static void do_inline_image(R& r, Obj* dict, const u8* data, u32 len) {
    if (!r.draw || r.hide) return;
    Image im;
    if (pdf_decode_image(r.d, dict, data, len, r.res, r.gs.fp.rgb, &im)) draw_image_obj(r, im);
}

static void do_shading(R& r, const char* name) {
    if (!r.draw || r.hide) return;
    Obj* shs = dget(r.d, r.res, "Shading");
    Obj* sd = dget(r.d, shs, name);
    if (!sd) return;
    Shading* sh = get_shading(r, sd, false);
    if (!sh) return;
    shading_set_matrix(sh, r.gs.ctm);
    IRect cb;
    if (!clip_rect_for(r, &cb)) return;
    Path rc;
    rc.rect((float)cb.x0, (float)cb.y0, (float)(cb.x1 - cb.x0), (float)(cb.y1 - cb.y0));
    ShadeSink s;
    s.img = r.img; s.clip = r.gs.clip; s.sh = sh; s.alpha = (u32)clamp((int)(r.gs.fa * 255 + 0.5), 0, 255); s.mode = r.gs.blend;
    s.bg = 0; s.has_bg = false;
    raster_fill(rc, false, cb, s);
    rc.free();
}

static void apply_extgstate(R& r, Obj* gsd) {
    if (!is_dict(gsd)) return;
    for (u32 i = 0; i < gsd->dict.n; i++) {
        const char* k = gsd->dict.e[i].key;
        Obj* v = pdf_res(r.d, gsd->dict.e[i].val);
        if (!v) continue;
        if (!strcmp(k, "LW")) r.gs.lw = onum(v, 1);
        else if (!strcmp(k, "LC")) r.gs.cap = (int)onum(v, 0);
        else if (!strcmp(k, "LJ")) r.gs.join = (int)onum(v, 0);
        else if (!strcmp(k, "ML")) r.gs.miter = onum(v, 10);
        else if (!strcmp(k, "CA")) r.gs.sa = clamp(onum(v, 1), 0.0, 1.0);
        else if (!strcmp(k, "ca")) r.gs.fa = clamp(onum(v, 1), 0.0, 1.0);
        else if (!strcmp(k, "D") && v->type == O_ARR && v->arr.n >= 2) {
            Obj* da = aget(r.d, v, 0);
            r.gs.ndash = 0;
            for (u32 q = 0; da && q < da->arr.n && q < 16; q++) r.gs.dash[r.gs.ndash++] = onum(aget(r.d, da, q), 0);
            r.gs.dphase = onum(aget(r.d, v, 1), 0);
        } else if (!strcmp(k, "BM")) {
            const char* bm = v->type == O_NAME ? (const char*)v->str.s : (v->type == O_ARR ? oname(aget(r.d, v, 0)) : "");
            r.gs.blend = !strcmp(bm, "Multiply") ? B_MULTIPLY : !strcmp(bm, "Screen") ? B_SCREEN : !strcmp(bm, "Darken") ? B_DARKEN :
                         !strcmp(bm, "Lighten") ? B_LIGHTEN : !strcmp(bm, "Overlay") ? B_OVERLAY : !strcmp(bm, "Difference") ? B_DIFFERENCE : B_NORMAL;
        } else if (!strcmp(k, "Font") && v->type == O_ARR && v->arr.n >= 2) {
            Obj* fd = aget(r.d, v, 0);
            if (is_dict(fd)) { r.gs.font = pdf_font_load(r.d, fd); r.gs.fsize = onum(aget(r.d, v, 1), 1); }
        }
    }
}

// ================================================================ interpreter
constexpr u32 K(const char* s) {
    return (u32)(u8)s[0] | (s[0] && s[1] ? (u32)(u8)s[1] << 8 : 0) | (s[0] && s[1] && s[2] ? (u32)(u8)s[2] << 16 : 0);
}

static double num(Vec<Obj*>& o, int i) { return i >= 0 && i < o.len ? onum(o[i], 0) : 0; }

static void add_pt(R& r, double x, double y, double* ox, double* oy) { mat_apply(r.gs.ctm, x, y, ox, oy); }

static void run_stream(R& r, const u8* data, u32 len) {
    ContentLex lx;
    lx.d = data; lx.n = len; lx.p = 0; lx.doc = r.d; lx.img_off = lx.img_len = 0;
    Vec<Obj*> ops;
    char op[16];
    int sp_base = r.stack.len;
    while (content_next(lx, ops, op, sizeof(op))) {
        if (r.budget == 0) break;
        r.budget--;
        u32 key = K(op);
        switch (key) {
        // ---- graphics state
        case K("q"):
            if (r.stack.len < 120) { r.gs.clip->ref(); r.stack.push(r.gs); }
            break;
        case K("Q"):
            if (r.stack.len > sp_base) { r.gs.clip->unref(); r.gs = r.stack.back(); r.stack.len--; }
            break;
        case K("cm"):
            if (ops.len >= 6) {
                Mat m; m.a = num(ops, 0); m.b = num(ops, 1); m.c = num(ops, 2); m.d = num(ops, 3); m.e = num(ops, 4); m.f = num(ops, 5);
                r.gs.ctm = mat_mul(m, r.gs.ctm);
            }
            break;
        case K("w"): r.gs.lw = num(ops, 0); break;
        case K("J"): r.gs.cap = (int)num(ops, 0); break;
        case K("j"): r.gs.join = (int)num(ops, 0); break;
        case K("M"): r.gs.miter = num(ops, 0); break;
        case K("d"):
            if (ops.len >= 2 && ops[0]->type == O_ARR) {
                r.gs.ndash = 0;
                for (u32 i = 0; i < ops[0]->arr.n && i < 16; i++) r.gs.dash[r.gs.ndash++] = onum(ops[0]->arr.v[i], 0);
                r.gs.dphase = num(ops, 1);
            }
            break;
        case K("ri"): case K("i"): break;
        case K("gs"):
            if (ops.len && ops[0]->type == O_NAME) apply_extgstate(r, dget(r.d, dget(r.d, r.res, "ExtGState"), (const char*)ops[0]->str.s));
            break;
        // ---- path construction
        case K("m"): { double x, y; add_pt(r, num(ops, 0), num(ops, 1), &x, &y); r.path.move((float)x, (float)y); break; }
        case K("l"): { double x, y; add_pt(r, num(ops, 0), num(ops, 1), &x, &y); r.path.line((float)x, (float)y); break; }
        case K("c"): {
            double x1, y1, x2, y2, x3, y3;
            add_pt(r, num(ops, 0), num(ops, 1), &x1, &y1); add_pt(r, num(ops, 2), num(ops, 3), &x2, &y2); add_pt(r, num(ops, 4), num(ops, 5), &x3, &y3);
            r.path.curve((float)x1, (float)y1, (float)x2, (float)y2, (float)x3, (float)y3);
            break;
        }
        case K("v"): {
            double x2, y2, x3, y3;
            add_pt(r, num(ops, 0), num(ops, 1), &x2, &y2); add_pt(r, num(ops, 2), num(ops, 3), &x3, &y3);
            r.path.curve(r.path.cx, r.path.cy, (float)x2, (float)y2, (float)x3, (float)y3);
            break;
        }
        case K("y"): {
            double x1, y1, x3, y3;
            add_pt(r, num(ops, 0), num(ops, 1), &x1, &y1); add_pt(r, num(ops, 2), num(ops, 3), &x3, &y3);
            r.path.curve((float)x1, (float)y1, (float)x3, (float)y3, (float)x3, (float)y3);
            break;
        }
        case K("h"): r.path.close(); break;
        case K("re"): {
            double x = num(ops, 0), y = num(ops, 1), w = num(ops, 2), h = num(ops, 3);
            double p[4][2];
            add_pt(r, x, y, &p[0][0], &p[0][1]); add_pt(r, x + w, y, &p[1][0], &p[1][1]);
            add_pt(r, x + w, y + h, &p[2][0], &p[2][1]); add_pt(r, x, y + h, &p[3][0], &p[3][1]);
            r.path.move((float)p[0][0], (float)p[0][1]);
            for (int i = 1; i < 4; i++) r.path.line((float)p[i][0], (float)p[i][1]);
            r.path.close();
            break;
        }
        // ---- painting
        case K("S"): stroke_dev(r, r.path); end_path(r); break;
        case K("s"): r.path.close(); stroke_dev(r, r.path); end_path(r); break;
        case K("f"): case K("F"): fill_path(r, false); end_path(r); break;
        case K("f*"): fill_path(r, true); end_path(r); break;
        case K("B"): fill_path(r, false); stroke_dev(r, r.path); end_path(r); break;
        case K("B*"): fill_path(r, true); stroke_dev(r, r.path); end_path(r); break;
        case K("b"): r.path.close(); fill_path(r, false); stroke_dev(r, r.path); end_path(r); break;
        case K("b*"): r.path.close(); fill_path(r, true); stroke_dev(r, r.path); end_path(r); break;
        case K("n"): end_path(r); break;
        case K("W"): r.clip_pending = 1; break;
        case K("W*"): r.clip_pending = 2; break;
        // ---- colour
        case K("CS"): if (ops.len && !r.uncolored) set_color_space(r, true, ops[0]); break;
        case K("cs"): if (ops.len && !r.uncolored) set_color_space(r, false, ops[0]); break;
        case K("SC"): case K("SCN"): set_color(r, true, ops); break;
        case K("sc"): case K("scn"): set_color(r, false, ops); break;
        case K("G"): set_device_color(r, true, 1, ops); break;
        case K("g"): set_device_color(r, false, 1, ops); break;
        case K("RG"): set_device_color(r, true, 3, ops); break;
        case K("rg"): set_device_color(r, false, 3, ops); break;
        case K("K"): set_device_color(r, true, 4, ops); break;
        case K("k"): set_device_color(r, false, 4, ops); break;
        case K("sh"): if (ops.len && ops[0]->type == O_NAME) do_shading(r, (const char*)ops[0]->str.s); break;
        // ---- objects
        case K("Do"): if (ops.len && ops[0]->type == O_NAME) do_xobject(r, (const char*)ops[0]->str.s); break;
        case K("BI"): if (ops.len && is_dict(ops[0])) do_inline_image(r, ops[0], data + lx.img_off, lx.img_len); break;
        // ---- text
        case K("BT"): r.tm = r.tlm = Mat(); r.text_clip.clear(); break;
        case K("ET"):
            if (r.text_clip.s.len && r.draw) {
                ClipMask* nc = clip_intersect_path(r.gs.clip, r.text_clip, false);
                r.gs.clip->unref();
                r.gs.clip = nc;
            }
            r.text_clip.clear();
            break;
        case K("Tc"): r.gs.Tc = num(ops, 0); break;
        case K("Tw"): r.gs.Tw = num(ops, 0); break;
        case K("Tz"): r.gs.Th = num(ops, 0) / 100.0; break;
        case K("TL"): r.gs.Tl = num(ops, 0); break;
        case K("Tr"): r.gs.Tr = (int)num(ops, 0); break;
        case K("Ts"): r.gs.Ts = num(ops, 0); break;
        case K("Tf"):
            if (ops.len >= 2 && ops[0]->type == O_NAME) {
                Obj* fonts = dget(r.d, r.res, "Font");
                Obj* fd = dget(r.d, fonts, (const char*)ops[0]->str.s);
                r.gs.font = is_dict(fd) ? pdf_font_load(r.d, fd) : nullptr;
                r.gs.fsize = num(ops, 1);
            }
            break;
        case K("Td"): { Mat t; t.e = num(ops, 0); t.f = num(ops, 1); r.tlm = mat_mul(t, r.tlm); r.tm = r.tlm; break; }
        case K("TD"): { r.gs.Tl = -num(ops, 1); Mat t; t.e = num(ops, 0); t.f = num(ops, 1); r.tlm = mat_mul(t, r.tlm); r.tm = r.tlm; break; }
        case K("Tm"):
            if (ops.len >= 6) { Mat m; m.a = num(ops, 0); m.b = num(ops, 1); m.c = num(ops, 2); m.d = num(ops, 3); m.e = num(ops, 4); m.f = num(ops, 5); r.tm = r.tlm = m; }
            break;
        case K("T*"): { Mat t; t.f = -r.gs.Tl; r.tlm = mat_mul(t, r.tlm); r.tm = r.tlm; break; }
        case K("Tj"): if (ops.len && ops[0]->type == O_STR) show_string(r, ops[0]->str.s, (int)ops[0]->str.len); break;
        case K("'"): {
            Mat t; t.f = -r.gs.Tl; r.tlm = mat_mul(t, r.tlm); r.tm = r.tlm;
            if (ops.len && ops[0]->type == O_STR) show_string(r, ops[0]->str.s, (int)ops[0]->str.len);
            break;
        }
        case K("\""): {
            if (ops.len >= 3) { r.gs.Tw = num(ops, 0); r.gs.Tc = num(ops, 1); }
            Mat t; t.f = -r.gs.Tl; r.tlm = mat_mul(t, r.tlm); r.tm = r.tlm;
            if (ops.len && ops.back()->type == O_STR) show_string(r, ops.back()->str.s, (int)ops.back()->str.len);
            break;
        }
        case K("TJ"):
            if (ops.len && ops[0]->type == O_ARR) {
                Obj* a = ops[0];
                bool vert = r.gs.font && font_vertical(r.gs.font);
                for (u32 i = 0; i < a->arr.n; i++) {
                    Obj* e = a->arr.v[i];
                    if (e->type == O_STR) show_string(r, e->str.s, (int)e->str.len);
                    else if (e->type == O_NUM) {
                        double adj = -e->n / 1000.0 * r.gs.fsize;
                        Mat t;
                        if (vert) t.f = adj; else t.e = adj * r.gs.Th;
                        r.tm = mat_mul(t, r.tm);
                    }
                }
            }
            break;
        // ---- type 3 glyph metrics
        case K("d0"): break;
        case K("d1"): r.uncolored = true; break;
        // ---- marked content
        case K("BMC"): r.mc.push(0); break;
        case K("BDC"): {
            bool hid = false;
            if (ops.len >= 2 && is_name(ops[0], "OC")) {
                Obj* oc = nullptr;
                if (ops[1]->type == O_NAME) oc = draw_get(dget(r.d, r.res, "Properties"), (const char*)ops[1]->str.s);
                if (oc && oc_hidden(r, oc)) hid = true;
            }
            r.mc.push(hid ? 1 : 0);
            if (hid) r.hide++;
            break;
        }
        case K("EMC"):
            if (r.mc.len) { if (r.mc.back()) r.hide--; r.mc.len--; }
            break;
        default: break;
        }
    }
    // unbalanced q: restore
    while (r.stack.len > sp_base) { r.gs.clip->unref(); r.gs = r.stack.back(); r.stack.len--; }
    ops.free();
    lx.ar.free_all();
}

// ================================================================ page level
static u8* page_content(PdfDoc* d, PdfPage& pg, u32* len) {
    Obj* c = dget(d, pg.dict, "Contents");
    Vec<u8> buf;
    auto add = [&](Obj* s) {
        if (!s || s->type != O_STREAM) return;
        u32 l;
        u8* data = pdf_stream_copy(d, s, &l);
        for (u32 i = 0; i < l; i++) buf.push(data[i]);
        buf.push('\n');
        kfree(data);
    };
    if (c && c->type == O_ARR) for (u32 i = 0; i < c->arr.n; i++) add(aget(d, c, i));
    else add(c);
    buf.push(0);
    *len = buf.len - 1;
    return buf.data;
}

static void draw_annotations(R& r, PdfPage& pg) {
    PdfDoc* d = r.d;
    Obj* an = dget(d, pg.dict, "Annots");
    if (!an || an->type != O_ARR) return;
    for (u32 i = 0; i < an->arr.n && i < 2000; i++) {
        Obj* a = aget(d, an, i);
        if (!is_dict(a)) continue;
        int flags = (int)onum(dget(d, a, "F"), 0);
        if (flags & (2 | 32)) continue;                         // Hidden, NoView
        if (is_name(dget(d, a, "Subtype"), "Popup")) continue;
        if (oc_hidden(r, draw_get(a, "OC"))) continue;
        Obj* ap = dget(d, a, "AP");
        Obj* n = dget(d, ap, "N");
        if (n && n->type != O_STREAM && is_dict(n)) {
            Obj* as = dget(d, a, "AS");
            n = as && as->type == O_NAME ? dget(d, n, (const char*)as->str.s) : nullptr;
        }
        if (!n || n->type != O_STREAM) continue;
        Obj* rect = dget(d, a, "Rect");
        if (!rect || rect->type != O_ARR || rect->arr.n < 4) continue;
        double rx0 = onum(aget(d, rect, 0), 0), ry0 = onum(aget(d, rect, 1), 0), rx1 = onum(aget(d, rect, 2), 0), ry1 = onum(aget(d, rect, 3), 0);
        if (rx0 > rx1) swap(rx0, rx1);
        if (ry0 > ry1) swap(ry0, ry1);
        Obj* bb = dget(d, n, "BBox");
        double b0 = 0, b1 = 0, b2 = 1, b3 = 1;
        if (bb && bb->type == O_ARR && bb->arr.n >= 4) { b0 = onum(aget(d, bb, 0), 0); b1 = onum(aget(d, bb, 1), 0); b2 = onum(aget(d, bb, 2), 1); b3 = onum(aget(d, bb, 3), 1); }
        Mat fm;
        Obj* mo = dget(d, n, "Matrix");
        if (mo && mo->type == O_ARR && mo->arr.n >= 6) {
            fm.a = onum(aget(d, mo, 0), 1); fm.b = onum(aget(d, mo, 1), 0); fm.c = onum(aget(d, mo, 2), 0);
            fm.d = onum(aget(d, mo, 3), 1); fm.e = onum(aget(d, mo, 4), 0); fm.f = onum(aget(d, mo, 5), 0);
        }
        // transformed appearance box
        double cx[4] = {b0, b2, b2, b0}, cy[4] = {b1, b1, b3, b3};
        double tx0 = 1e30, ty0 = 1e30, tx1 = -1e30, ty1 = -1e30;
        for (int k = 0; k < 4; k++) {
            double x, y;
            mat_apply(fm, cx[k], cy[k], &x, &y);
            tx0 = min(tx0, x); tx1 = max(tx1, x); ty0 = min(ty0, y); ty1 = max(ty1, y);
        }
        double sx = tx1 - tx0 > 1e-6 ? (rx1 - rx0) / (tx1 - tx0) : 1, sy = ty1 - ty0 > 1e-6 ? (ry1 - ry0) / (ty1 - ty0) : 1;
        Mat A;
        A.a = sx; A.d = sy; A.e = rx0 - tx0 * sx; A.f = ry0 - ty0 * sy;
        GState saved = r.gs;
        saved.clip->ref();
        r.gs.ctm = mat_mul(A, r.base);
        // do_form applies the form matrix itself
        do_form(r, n);
        r.gs.clip->unref();
        r.gs = saved;
    }
}

void pdf_run_page(PdfDoc* d, PdfPage& pg, RenderOpts& o) {
    R r;
    r.d = d; r.o = &o; r.img = o.out; r.draw = o.draw;
    r.base = o.base;
    r.res = pg.res;
    Image dummy;
    if (!r.img) { dummy.w = 1; dummy.h = 1; r.img = &dummy; }
    gs_init(r);
    if (!o.draw) r.gs.clip->b = {0, 0, 1, 1};
    u32 len;
    u8* data = page_content(d, pg, &len);
    run_stream(r, data, len);
    if (data) kfree(data);
    while (r.stack.len) { r.gs.clip->unref(); r.gs = r.stack.back(); r.stack.len--; }
    r.path.free();
    r.text_clip.free();
    r.mc.free();
    draw_annotations(r, pg);
    r.gs.clip->unref();
    r.stack.free();
    for (int i = 0; i < r.tiles.len; i++) kfree(r.tiles[i].img.px);
    r.tiles.free();
    for (int i = 0; i < r.shades.len; i++) shading_free(r.shades[i].sh);
    r.shades.free();
}

static Mat page_base(const PdfPage& p, double s) {
    double x0 = p.box[0], y0 = p.box[1], x1 = p.box[2], y1 = p.box[3];
    Mat m;
    switch (p.rotate) {
    case 90:  m.a = 0;  m.b = s;  m.c = s;  m.d = 0;  m.e = -y0 * s; m.f = -x0 * s; break;
    case 180: m.a = -s; m.b = 0;  m.c = 0;  m.d = s;  m.e = x1 * s;  m.f = -y0 * s; break;
    case 270: m.a = 0;  m.b = -s; m.c = -s; m.d = 0;  m.e = y1 * s;  m.f = x1 * s;  break;
    default:  m.a = s;  m.b = 0;  m.c = 0;  m.d = -s; m.e = -x0 * s; m.f = y1 * s;  break;
    }
    return m;
}

bool pdf_render(PdfDoc* d, int page, float scale, Image* out) {
    if (page < 0 || page >= d->pages.len || scale <= 0) return false;
    PdfPage& pg = d->pages[page];
    float pw, ph;
    pdf_page_size(d, page, &pw, &ph);
    int w = (int)m_ceil(pw * scale), h = (int)m_ceil(ph * scale);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if ((u64)w * h > 40000000ull) return false;
    u32* px = (u32*)kmalloc_try((size_t)w * h * 4);
    if (!px) return false;
    memset32(px, 0xFFFFFFFF, (size_t)w * h);
    out->w = w; out->h = h; out->px = px;
    RenderOpts o;
    o.draw = true; o.out = out; o.base = page_base(pg, scale); o.text = nullptr; o.page = page;
    pdf_run_page(d, pg, o);
    return true;
}

// ================================================================ text extraction
static void finish_text(Vec<PdfChar>& in, Vec<PdfChar>& out) {
    for (int i = 0; i < in.len; i++) {
        PdfChar c = in[i];
        if (i > 0 && out.len > 0) {
            const PdfChar& p = out.back();
            float h = max(p.y1 - p.y0, c.y1 - c.y0);
            float pm = (p.y0 + p.y1) / 2, cm = (c.y0 + c.y1) / 2;
            if (p.cp != 10 && (m_fabs(cm - pm) > h * 0.55f || c.x0 < p.x0 - h * 2)) {
                PdfChar nl;
                nl.x0 = p.x1; nl.x1 = p.x1; nl.y0 = p.y0; nl.y1 = p.y1; nl.cp = 10;
                out.push(nl);
            } else if (p.cp != 10 && p.cp != ' ' && c.cp != ' ' && c.x0 - p.x1 > h * 0.18f) {
                PdfChar sp;
                sp.x0 = p.x1; sp.x1 = c.x0; sp.y0 = p.y0; sp.y1 = p.y1; sp.cp = ' ';
                out.push(sp);
            }
        }
        out.push(c);
    }
}

int pdf_page_text(PdfDoc* d, int page, const PdfChar** chars) {
    *chars = nullptr;
    if (page < 0 || page >= d->pages.len) return 0;
    for (int i = 0; i < d->texts.len; i++) if (d->texts[i]->page == page) { *chars = d->texts[i]->chars.data; return d->texts[i]->chars.len; }
    if (d->texts.len >= 48) { d->texts[0]->chars.free(); delete d->texts[0]; d->texts.remove(0); }
    PdfTextCache raw;
    raw.page = page;
    RenderOpts o;
    o.draw = false; o.out = nullptr; o.base = page_base(d->pages[page], 1.0); o.text = &raw; o.page = page;
    pdf_run_page(d, d->pages[page], o);
    PdfTextCache* tc = new PdfTextCache();
    tc->page = page;
    finish_text(raw.chars, tc->chars);
    raw.chars.free();
    d->texts.push(tc);
    *chars = tc->chars.data;
    return tc->chars.len;
}
