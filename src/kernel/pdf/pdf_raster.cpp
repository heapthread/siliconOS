// silicon - PDF: anti-aliased scan converter, stroker and clip masks
#include "pdf_gfx.h"

static const int SUB = 4;                 // sub-scanlines per pixel row
static const int KUNIT = 1024;            // coverage units per sub-scanline (SUB * KUNIT = full pixel)

// ================================================================ flattening
void flatten_path(const Path& p, Vec<Poly>& out) {
    Poly* cur = nullptr;
    float cx = 0, cy = 0, sx = 0, sy = 0;
    auto start = [&](float x, float y) {
        out.push(Poly());
        cur = &out.back();
        cur->pts.push(x); cur->pts.push(y);
        cx = sx = x; cy = sy = y;
    };
    for (int i = 0; i < p.s.len; i++) {
        const PSeg& g = p.s[i];
        switch (g.op) {
        case P_MOVE: start(g.x[0], g.x[1]); break;
        case P_LINE:
            if (!cur) start(cx, cy);
            cur->pts.push(g.x[0]); cur->pts.push(g.x[1]);
            cx = g.x[0]; cy = g.x[1];
            break;
        case P_CURVE: {
            if (!cur) start(cx, cy);
            float x1 = g.x[0], y1 = g.x[1], x2 = g.x[2], y2 = g.x[3], x3 = g.x[4], y3 = g.x[5];
            auto d = [](float ax, float ay, float bx, float by) { float dx = ax - bx, dy = ay - by; return (float)m_sqrt(dx * dx + dy * dy); };
            float L = d(cx, cy, x1, y1) + d(x1, y1, x2, y2) + d(x2, y2, x3, y3);
            int n = (int)m_sqrt(L * 0.75) + 1;
            if (n > 80) n = 80;
            for (int k = 1; k <= n; k++) {
                float t = (float)k / n, u = 1 - t;
                float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, e = t * t * t;
                cur->pts.push(a * cx + b * x1 + c * x2 + e * x3);
                cur->pts.push(a * cy + b * y1 + c * y2 + e * y3);
            }
            cx = x3; cy = y3;
            break;
        }
        case P_CLOSE:
            if (cur) { cur->closed = true; cx = sx; cy = sy; cur = nullptr; }
            break;
        }
    }
}

static void free_polys(Vec<Poly>& v) {
    for (int i = 0; i < v.len; i++) v[i].pts.free();
    v.free();
}

void path_bounds(const Path& p, float* x0, float* y0, float* x1, float* y1) {
    float a = 1e30f, b = 1e30f, c = -1e30f, d = -1e30f;
    for (int i = 0; i < p.s.len; i++) {
        const PSeg& g = p.s[i];
        int np = g.op == P_CURVE ? 3 : g.op == P_CLOSE ? 0 : 1;
        for (int k = 0; k < np; k++) {
            float x = g.x[2 * k], y = g.x[2 * k + 1];
            if (x < a) a = x; if (x > c) c = x;
            if (y < b) b = y; if (y > d) d = y;
        }
    }
    *x0 = a; *y0 = b; *x1 = c; *y1 = d;
}

bool path_is_rect(const Path& p, float* x0, float* y0, float* x1, float* y1) {
    // M L L L (L) Z  with axis aligned sides
    float px[6], py[6];
    int n = 0;
    bool closed = false;
    for (int i = 0; i < p.s.len; i++) {
        const PSeg& g = p.s[i];
        if (g.op == P_MOVE && i == 0) { px[n] = g.x[0]; py[n] = g.x[1]; n++; }
        else if (g.op == P_LINE && !closed && n < 6) { px[n] = g.x[0]; py[n] = g.x[1]; n++; }
        else if (g.op == P_CLOSE && i == p.s.len - 1) closed = true;
        else return false;
    }
    if (n == 5 && px[4] == px[0] && py[4] == py[0]) n = 4;
    if (n != 4) return false;
    const float eps = 0.001f;
    bool a = m_fabs(px[0] - px[1]) < eps && m_fabs(py[1] - py[2]) < eps && m_fabs(px[2] - px[3]) < eps && m_fabs(py[3] - py[0]) < eps;
    bool b = m_fabs(py[0] - py[1]) < eps && m_fabs(px[1] - px[2]) < eps && m_fabs(py[2] - py[3]) < eps && m_fabs(px[3] - px[0]) < eps;
    if (!a && !b) return false;
    *x0 = min(min(px[0], px[1]), min(px[2], px[3]));
    *x1 = max(max(px[0], px[1]), max(px[2], px[3]));
    *y0 = min(min(py[0], py[1]), min(py[2], py[3]));
    *y1 = max(max(py[0], py[1]), max(py[2], py[3]));
    return true;
}

// ================================================================ scan conversion
struct Edge { float x0, y0, x1, y1, dxdy; int dir; int next; };
struct Cross { float x; int dir; };

void raster_fill(const Path& path, bool evenodd, IRect clip, Sink& sink) {
    if (irect_empty(clip) || path.s.len < 2) return;
    Vec<Poly> polys;
    flatten_path(path, polys);
    int rows = clip.y1 - clip.y0;
    int W = clip.x1 - clip.x0;
    int total = 0;
    for (int i = 0; i < polys.len; i++) total += polys[i].pts.len / 2;
    Edge* edges = (Edge*)kmalloc(sizeof(Edge) * (total + 8));
    int ne = 0;
    int* head = (int*)kmalloc(sizeof(int) * (rows + 1));
    for (int i = 0; i < rows; i++) head[i] = -1;
    for (int pi = 0; pi < polys.len; pi++) {
        const float* pt = polys[pi].pts.data;
        int n = polys[pi].pts.len / 2;
        if (n < 2) continue;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            if (j == 0 && n == 2 && i == 1) break;
            float ax = pt[2 * i], ay = pt[2 * i + 1], bx = pt[2 * j], by = pt[2 * j + 1];
            if (ay == by) continue;
            int dir = 1;
            if (ay > by) { swap(ax, bx); swap(ay, by); dir = -1; }
            if (by <= (float)clip.y0 || ay >= (float)clip.y1) continue;
            if (min(ax, bx) >= (float)clip.x1) continue;
            Edge& e = edges[ne];
            e.x0 = ax; e.y0 = ay; e.x1 = bx; e.y1 = by;
            e.dxdy = (bx - ax) / (by - ay);
            e.dir = dir;
            int row = (int)m_floor(ay) - clip.y0;
            if (row < 0) row = 0;
            e.next = head[row];
            head[row] = ne;
            ne++;
        }
    }
    free_polys(polys);
    Vec<int> active;
    int* part = (int*)kcalloc(sizeof(int) * (W + 3));
    int* run = (int*)kcalloc(sizeof(int) * (W + 3));
    u8* cov = (u8*)kmalloc(W + 3);
    Cross* cr = nullptr;
    int crcap = 0;
    float fx0 = (float)clip.x0, fx1 = (float)clip.x1;
    for (int row = 0; row < rows; row++) {
        for (int e = head[row]; e >= 0; e = edges[e].next) active.push(e);
        if (!active.len) continue;
        int minx = W + 1, maxx = -1;
        for (int k = 0; k < SUB; k++) {
            float ys = (float)(clip.y0 + row) + (k + 0.5f) / SUB;
            if (active.len > crcap) { crcap = active.len * 2; if (cr) kfree(cr); cr = (Cross*)kmalloc(sizeof(Cross) * crcap); }
            int nc = 0;
            for (int i = 0; i < active.len;) {
                const Edge& e = edges[active[i]];
                if (ys >= e.y1) { active[i] = active[active.len - 1]; active.len--; continue; }
                if (ys >= e.y0) {
                    cr[nc].x = e.x0 + (ys - e.y0) * e.dxdy;
                    cr[nc].dir = e.dir;
                    nc++;
                }
                i++;
            }
            for (int i = 1; i < nc; i++) {
                Cross c = cr[i];
                int j = i - 1;
                while (j >= 0 && cr[j].x > c.x) { cr[j + 1] = cr[j]; j--; }
                cr[j + 1] = c;
            }
            int wind = 0;
            float start = 0;
            bool inside = false;
            for (int i = 0; i < nc; i++) {
                wind += cr[i].dir;
                bool now = evenodd ? (wind & 1) != 0 : wind != 0;
                if (now && !inside) { start = cr[i].x; inside = true; }
                else if (!now && inside) {
                    inside = false;
                    float xa = start, xb = cr[i].x;
                    if (xb <= fx0 || xa >= fx1) continue;
                    if (xa < fx0) xa = fx0;
                    if (xb > fx1) xb = fx1;
                    xa -= fx0; xb -= fx0;
                    int ia = (int)xa, ib = (int)xb;
                    if (ia == ib) part[ia] += (int)((xb - xa) * KUNIT);
                    else {
                        part[ia] += (int)((ia + 1 - xa) * KUNIT);
                        part[ib] += (int)((xb - ib) * KUNIT);
                        run[ia + 1] += KUNIT;
                        run[ib] -= KUNIT;
                    }
                    if (ia < minx) minx = ia;
                    if (ib > maxx) maxx = ib;
                }
            }
            if (inside) {                                   // an unclosed crossing runs off to the right
                float xa = start;
                if (xa < fx1) {
                    if (xa < fx0) xa = fx0;
                    xa -= fx0;
                    int ia = (int)xa;
                    part[ia] += (int)((ia + 1 - xa) * KUNIT);
                    run[ia + 1] += KUNIT;
                    run[W] -= KUNIT;
                    if (ia < minx) minx = ia;
                    if (W - 1 > maxx) maxx = W - 1;
                }
            }
        }
        if (maxx >= 0) {
            if (maxx >= W) maxx = W - 1;
            int acc = 0;
            for (int i = 0; i < minx; i++) acc += run[i];
            bool any = false;
            for (int i = minx; i <= maxx; i++) {
                acc += run[i];
                int v = acc + part[i];
                int c = (v * 255 + 2048) >> 12;
                cov[i - minx] = (u8)(c > 255 ? 255 : c < 0 ? 0 : c);
                if (c > 0) any = true;
            }
            if (any) sink.row(clip.y0 + row, clip.x0 + minx, clip.x0 + maxx + 1, cov);
            for (int i = minx; i <= min(maxx + 2, W + 2); i++) { part[i] = 0; run[i] = 0; }
        }
    }
    if (cr) kfree(cr);
    kfree(part); kfree(run); kfree(cov);
    kfree(edges); kfree(head);
    active.free();
}

// ================================================================ clip masks
struct MaskSink : Sink {
    ClipMask* m;
    ClipMask* old;
    void row(int y, int x0, int x1, const u8* cov) override {
        int bw = m->b.x1 - m->b.x0;
        u8* dst = m->m + (size_t)(y - m->b.y0) * bw;
        for (int x = x0; x < x1; x++) {
            u32 c = cov[x - x0];
            if (old) c = c * old->at(x, y) / 255;
            dst[x - m->b.x0] = (u8)c;
        }
    }
};

ClipMask* clip_intersect_path(ClipMask* cur, const Path& devpath, bool evenodd) {
    float fx0, fy0, fx1, fy1;
    IRect nb;
    if (path_is_rect(devpath, &fx0, &fy0, &fx1, &fy1)) {
        nb = irect_isect(cur->b, {(int)m_floor(fx0 + 0.5), (int)m_floor(fy0 + 0.5), (int)m_floor(fx1 + 0.5), (int)m_floor(fy1 + 0.5)});
        ClipMask* c = ClipMask::make(irect_empty(nb) ? IRect{0, 0, 0, 0} : nb);
        if (cur->m && !irect_empty(nb)) {
            int bw = nb.x1 - nb.x0, bh = nb.y1 - nb.y0;
            c->m = (u8*)kmalloc((size_t)bw * bh);
            for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) c->m[(size_t)y * bw + x] = cur->at(nb.x0 + x, nb.y0 + y);
        }
        return c;
    }
    path_bounds(devpath, &fx0, &fy0, &fx1, &fy1);
    if (fx1 < fx0) return ClipMask::make({0, 0, 0, 0});
    nb = irect_isect(cur->b, {(int)m_floor(fx0), (int)m_floor(fy0), (int)m_ceil(fx1) + 1, (int)m_ceil(fy1) + 1});
    if (irect_empty(nb)) return ClipMask::make({0, 0, 0, 0});
    ClipMask* c = ClipMask::make(nb);
    c->m = (u8*)kcalloc((size_t)(nb.x1 - nb.x0) * (nb.y1 - nb.y0));
    MaskSink ms;
    ms.m = c;
    ms.old = cur->m ? cur : nullptr;
    raster_fill(devpath, evenodd, nb, ms);
    return c;
}

// ================================================================ stroking
static void emit_poly(Path& out, const float* pts, int n) {
    if (n < 3) return;
    float area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += pts[2 * i] * pts[2 * j + 1] - pts[2 * j] * pts[2 * i + 1];
    }
    if (area == 0) return;
    if (area > 0) {
        out.move(pts[0], pts[1]);
        for (int i = 1; i < n; i++) out.line(pts[2 * i], pts[2 * i + 1]);
    } else {
        out.move(pts[2 * (n - 1)], pts[2 * (n - 1) + 1]);
        for (int i = n - 2; i >= 0; i--) out.line(pts[2 * i], pts[2 * i + 1]);
    }
    out.close();
}

static void emit_circle(Path& out, float cx, float cy, float r) {
    int n = (int)(r * 1.5f) + 8;
    if (n > 40) n = 40;
    float pts[2 * 48];
    for (int i = 0; i < n; i++) {
        double a = 6.283185307179586 * i / n;
        pts[2 * i] = cx + r * (float)m_cos(a);
        pts[2 * i + 1] = cy + r * (float)m_sin(a);
    }
    emit_poly(out, pts, n);
}

static void stroke_poly(const float* pt, int n, bool closed, float hw, int cap, int join, float miter, Path& out) {
    // remove repeated points
    Vec<float> q;
    for (int i = 0; i < n; i++) {
        if (q.len >= 2 && m_fabs(q[q.len - 2] - pt[2 * i]) < 1e-4f && m_fabs(q[q.len - 1] - pt[2 * i + 1]) < 1e-4f) continue;
        q.push(pt[2 * i]); q.push(pt[2 * i + 1]);
    }
    int m = q.len / 2;
    if (closed && m > 1 && m_fabs(q[0] - q[2 * (m - 1)]) < 1e-4f && m_fabs(q[1] - q[2 * (m - 1) + 1]) < 1e-4f) { m--; }
    if (m == 1) {                                          // a dot
        if (cap == 1) emit_circle(out, q[0], q[1], hw);
        else if (cap == 2) { float p[8] = {q[0] - hw, q[1] - hw, q[0] + hw, q[1] - hw, q[0] + hw, q[1] + hw, q[0] - hw, q[1] + hw}; emit_poly(out, p, 4); }
        q.free();
        return;
    }
    if (m < 2) { q.free(); return; }
    int segs = closed ? m : m - 1;
    float dxs[1], dys[1];
    (void)dxs; (void)dys;
    Vec<float> dirs;                                       // unit directions per segment
    for (int i = 0; i < segs; i++) {
        int j = (i + 1) % m;
        float dx = q[2 * j] - q[2 * i], dy = q[2 * j + 1] - q[2 * i + 1];
        float l = (float)m_sqrt(dx * dx + dy * dy);
        if (l < 1e-6f) l = 1e-6f;
        dirs.push(dx / l); dirs.push(dy / l);
    }
    for (int i = 0; i < segs; i++) {
        int j = (i + 1) % m;
        float x0 = q[2 * i], y0 = q[2 * i + 1], x1 = q[2 * j], y1 = q[2 * j + 1];
        float dx = dirs[2 * i], dy = dirs[2 * i + 1];
        if (!closed && cap == 2) {
            if (i == 0) { x0 -= dx * hw; y0 -= dy * hw; }
            if (i == segs - 1) { x1 += dx * hw; y1 += dy * hw; }
        }
        float nx = -dy * hw, ny = dx * hw;
        float p[8] = {x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny};
        emit_poly(out, p, 4);
    }
    // joins
    int jstart = closed ? 0 : 1;
    int jend = closed ? m : m - 1;
    for (int v = jstart; v < jend; v++) {
        int a = (v + segs - 1) % segs, b = v % segs;
        float d0x = dirs[2 * a], d0y = dirs[2 * a + 1], d1x = dirs[2 * b], d1y = dirs[2 * b + 1];
        float cross = d0x * d1y - d0y * d1x;
        float dot = d0x * d1x + d0y * d1y;
        if (m_fabs(cross) < 1e-4f && dot > 0) continue;
        float vx = q[2 * v], vy = q[2 * v + 1];
        if (join == 1) { if (hw > 0.8f) emit_circle(out, vx, vy, hw); else join = 2; }
        if (join == 1) continue;
        float s = cross > 0 ? -1.0f : 1.0f;
        float n0x = -d0y * s * hw, n0y = d0x * s * hw, n1x = -d1y * s * hw, n1y = d1x * s * hw;
        if (join == 0) {
            float cosphi = n0x * n1x + n0y * n1y;           // hw^2 * cos(phi)
            float c1 = cosphi / (hw * hw);
            float ratio = c1 > -0.999f ? (float)m_sqrt(2.0f / (1.0f + c1)) : 1e9f;
            if (ratio <= miter) {
                float k = 1.0f / (1.0f + c1);
                float tx = vx + (n0x + n1x) * k, ty = vy + (n0y + n1y) * k;
                float p[8] = {vx, vy, vx + n0x, vy + n0y, tx, ty, vx + n1x, vy + n1y};
                emit_poly(out, p, 4);
                continue;
            }
        }
        float p[6] = {vx, vy, vx + n0x, vy + n0y, vx + n1x, vy + n1y};
        emit_poly(out, p, 3);
    }
    if (!closed && cap == 1) {
        emit_circle(out, q[0], q[1], hw);
        emit_circle(out, q[2 * (m - 1)], q[2 * (m - 1) + 1], hw);
    }
    dirs.free();
    q.free();
}

static void dash_poly(const Poly& pl, const double* dash, int nd, double phase, Vec<Poly>& out) {
    int n = pl.pts.len / 2;
    if (n < 2) return;
    Vec<float> pts;
    for (int i = 0; i < pl.pts.len; i++) pts.push(pl.pts[i]);
    if (pl.closed) { pts.push(pl.pts[0]); pts.push(pl.pts[1]); n++; }
    double total = 0;
    for (int i = 0; i < nd; i++) total += dash[i];
    if (total <= 1e-6) { Poly p; for (int i = 0; i < pts.len; i++) p.pts.push(pts[i]); out.push(p); pts.free(); return; }
    int idx = 0;
    double left;
    bool on = true;
    double ph = phase;
    if (ph < 0) ph = 0;
    ph = ph - total * 2 * m_floor(ph / (total * 2));
    while (ph >= dash[idx % nd] && nd > 0) {
        ph -= dash[idx % nd];
        idx++;
        on = !on;
        if (idx > 10000) break;
    }
    left = dash[idx % nd] - ph;
    Poly cur;
    int emitted = 0;
    if (on) { cur.pts.push(pts[0]); cur.pts.push(pts[1]); }
    for (int i = 0; i + 1 < n; i++) {
        double x0 = pts[2 * i], y0 = pts[2 * i + 1], x1 = pts[2 * i + 2], y1 = pts[2 * i + 3];
        double dx = x1 - x0, dy = y1 - y0;
        double len = m_sqrt(dx * dx + dy * dy);
        double pos = 0;
        while (len - pos > left) {
            pos += left;
            double t = len > 0 ? pos / len : 0;
            float px = (float)(x0 + dx * t), py = (float)(y0 + dy * t);
            if (on) {
                cur.pts.push(px); cur.pts.push(py);
                out.push(cur);
                cur = Poly();
                if (++emitted > 60000) { cur.pts.free(); pts.free(); return; }
            } else { cur.pts.push(px); cur.pts.push(py); }
            on = !on;
            idx++;
            left = dash[idx % nd];
            if (left <= 1e-6 && nd > 0) left = 1e-6;
        }
        left -= len - pos;
        if (on) { cur.pts.push((float)x1); cur.pts.push((float)y1); }
    }
    if (on && cur.pts.len >= 4) out.push(cur); else cur.pts.free();
    pts.free();
}

void stroke_path(const Path& p, double width, int cap, int join, double miter, const double* dash, int ndash, double phase, Path& out) {
    Vec<Poly> polys;
    flatten_path(p, polys);
    float hw = (float)(width * 0.5);
    bool use_dash = false;
    if (ndash > 0) { for (int i = 0; i < ndash; i++) if (dash[i] > 0) use_dash = true; }
    if (use_dash) {
        double d2[32];
        int nd = ndash;
        for (int i = 0; i < ndash && i < 16; i++) d2[i] = dash[i] < 0 ? 0 : dash[i];
        if (nd & 1) { for (int i = 0; i < nd; i++) d2[nd + i] = d2[i]; nd *= 2; }
        Vec<Poly> dashed;
        for (int i = 0; i < polys.len; i++) dash_poly(polys[i], d2, nd, phase, dashed);
        free_polys(polys);
        polys = dashed;
        for (int i = 0; i < polys.len; i++) polys[i].closed = false;
    }
    for (int i = 0; i < polys.len; i++)
        stroke_poly(polys[i].pts.data, polys[i].pts.len / 2, polys[i].closed, hw, cap, join, (float)miter, out);
    free_polys(polys);
}
