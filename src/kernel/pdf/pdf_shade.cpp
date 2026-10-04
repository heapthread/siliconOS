// silicon - PDF: shadings (function based, axial, radial)
#include "pdf_gfx.h"

struct Shading {
    int type = 0;
    ColorSpace* cs = nullptr;
    PdfFunc* fn = nullptr;
    Mat inv;                         // device -> shading space
    bool inv_ok = false;
    double coords[6] = {0, 0, 0, 0, 0, 0};
    double t0 = 0, t1 = 1;
    bool ext0 = false, ext1 = false;
    bool has_bg = false;
    u32 bg = 0;
    bool has_bbox = false;
    double bbox[4];
    // type 1
    double dom[4] = {0, 1, 0, 1};
    Mat fm;                          // function space -> shading space
    Mat fm_inv;
    u32 lut[257];
    bool lut_ok = false;
};

static u32 shade_color(Shading* s, double t) {
    double in[1] = {t};
    double out[16];
    for (int i = 0; i < 16; i++) out[i] = 0;
    if (s->fn) s->fn->eval(in, out);
    else out[0] = t;
    return cs_rgb(s->cs, out);
}

void shading_free(Shading* s) {
    if (!s) return;
    if (s->fn) delete s->fn;
    delete s;
}

Shading* shading_parse(PdfDoc* d, Obj* o, Obj* res) {
    o = pdf_res(d, o);
    if (!is_dict(o)) return nullptr;
    int type = (int)onum(dget(d, o, "ShadingType"), 0);
    if (type < 1 || type > 3) return nullptr;
    Shading* s = new Shading();
    s->type = type;
    s->cs = cs_parse(d, dget(d, o, "ColorSpace"), res);
    Obj* fo = dget(d, o, "Function");
    if (fo) s->fn = fn_parse(d, fo);
    if (!s->fn && type != 1) { delete s; return nullptr; }
    if (type == 1 && !s->fn) { delete s; return nullptr; }
    Obj* bg = dget(d, o, "Background");
    if (bg && bg->type == O_ARR) {
        double c[8];
        for (int i = 0; i < 8; i++) c[i] = onum(aget(d, bg, i), 0);
        s->bg = cs_rgb(s->cs, c);
        s->has_bg = true;
    }
    Obj* bb = dget(d, o, "BBox");
    if (bb && bb->type == O_ARR && bb->arr.n >= 4) {
        for (int i = 0; i < 4; i++) s->bbox[i] = onum(aget(d, bb, i), 0);
        s->has_bbox = true;
    }
    Obj* co = dget(d, o, "Coords");
    for (int i = 0; i < 6; i++) s->coords[i] = onum(aget(d, co, i), 0);
    Obj* dm = dget(d, o, "Domain");
    if (type == 1) {
        for (int i = 0; i < 4; i++) s->dom[i] = onum(aget(d, dm, i), i & 1);
        Obj* m = dget(d, o, "Matrix");
        if (m && m->type == O_ARR && m->arr.n >= 6) {
            s->fm.a = onum(aget(d, m, 0), 1); s->fm.b = onum(aget(d, m, 1), 0); s->fm.c = onum(aget(d, m, 2), 0);
            s->fm.d = onum(aget(d, m, 3), 1); s->fm.e = onum(aget(d, m, 4), 0); s->fm.f = onum(aget(d, m, 5), 0);
        }
        bool ok;
        s->fm_inv = mat_inv(s->fm, &ok);
        if (!ok) { delete s; return nullptr; }
    } else {
        s->t0 = onum(aget(d, dm, 0), 0);
        s->t1 = onum(aget(d, dm, 1), 1);
        Obj* ex = dget(d, o, "Extend");
        if (ex && ex->type == O_ARR) {
            Obj* a = aget(d, ex, 0);
            Obj* b = aget(d, ex, 1);
            s->ext0 = a && a->type == O_BOOL && a->b;
            s->ext1 = b && b->type == O_BOOL && b->b;
        }
    }
    return s;
}

void shading_set_matrix(Shading* s, const Mat& to_device) { s->inv = mat_inv(to_device, &s->inv_ok); }
bool shading_has_bbox(Shading* s, double* box) { if (s->has_bbox) for (int i = 0; i < 4; i++) box[i] = s->bbox[i]; return s->has_bbox; }
u32 shading_background(Shading* s, bool* has) { *has = s->has_bg; return s->bg; }
void shading_prepare(Shading* s, IRect area) {}

bool shading_at(Shading* s, double dx, double dy, u32* argb) {
    if (!s->inv_ok) return false;
    double x, y;
    mat_apply(s->inv, dx, dy, &x, &y);
    if (s->has_bbox && (x < s->bbox[0] || x > s->bbox[2] || y < s->bbox[1] || y > s->bbox[3])) return false;
    if (s->type == 1) {
        double fx, fy;
        mat_apply(s->fm_inv, x, y, &fx, &fy);
        if (fx < s->dom[0] || fx > s->dom[1] || fy < s->dom[2] || fy > s->dom[3]) return false;
        double in[2] = {fx, fy};
        double out[16];
        for (int i = 0; i < 16; i++) out[i] = 0;
        s->fn->eval(in, out);
        *argb = cs_rgb(s->cs, out);
        return true;
    }
    double t;
    if (s->type == 2) {
        double x0 = s->coords[0], y0 = s->coords[1], x1 = s->coords[2], y1 = s->coords[3];
        double vx = x1 - x0, vy = y1 - y0;
        double len2 = vx * vx + vy * vy;
        double sp = len2 > 0 ? ((x - x0) * vx + (y - y0) * vy) / len2 : 0;
        if (sp < 0) { if (!s->ext0) return false; sp = 0; }
        if (sp > 1) { if (!s->ext1) return false; sp = 1; }
        t = sp;
    } else {
        double x0 = s->coords[0], y0 = s->coords[1], r0 = s->coords[2], x1 = s->coords[3], y1 = s->coords[4], r1 = s->coords[5];
        double cdx = x1 - x0, cdy = y1 - y0, dr = r1 - r0;
        double px = x - x0, py = y - y0;
        double a = cdx * cdx + cdy * cdy - dr * dr;
        double b = px * cdx + py * cdy + r0 * dr;
        double c = px * px + py * py - r0 * r0;
        double best = 0;
        bool found = false;
        auto consider = [&](double sv) {
            if (r0 + sv * dr < 0) return;
            if (sv < 0 && !s->ext0) return;
            if (sv > 1 && !s->ext1) return;
            if (!found || sv > best) { best = sv; found = true; }
        };
        if (m_fabs(a) < 1e-9) {
            if (m_fabs(b) > 1e-12) consider(c / (2 * b));
        } else {
            double disc = b * b - a * c;
            if (disc < 0) return false;
            double sq = m_sqrt(disc);
            consider((b + sq) / a);
            consider((b - sq) / a);
        }
        if (!found) return false;
        t = best < 0 ? 0 : best > 1 ? 1 : best;
    }
    if (!s->lut_ok) {
        for (int i = 0; i <= 256; i++) s->lut[i] = shade_color(s, s->t0 + (s->t1 - s->t0) * i / 256.0);
        s->lut_ok = true;
    }
    *argb = s->lut[(int)(t * 256 + 0.5)];
    return true;
}
