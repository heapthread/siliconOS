// silicon - PDF viewer: continuous scrolling, zoom, outline sidebar, text selection and find.
// Pages are rendered one per tick into a small bitmap cache; the visible ones come first.
#include "apps.h"
#include "../../pdf/pdf.h"

bool is_pdf_name(const char* name) { return str_icontains(name, ".pdf"); }

static const int BAR_H = 34;
static const int SIDE_W = 190;
static const int GAP = 12;
static const int SB_W = 10;
static const u32 CACHE_BUDGET = 48u * 1024 * 1024;

struct PdfPos { int page, idx; };
static bool pos_less(const PdfPos& a, const PdfPos& b) { return a.page < b.page || (a.page == b.page && a.idx < b.idx); }

struct PdfApp : App {
    PdfDoc* doc = nullptr;
    int npages = 0;
    char name[64] = "";
    char err[120] = "";
    // layout (rebuilt when the width, zoom or page set changes)
    Vec<float> pw, ph;                // page sizes in points
    Vec<int> py;                      // top of each page in document pixels
    int total_h = 0, max_pw = 0;
    float scale = 1.0f;
    bool fit_width = true;
    float zoom_f = 1.25f;
    int lay_w = -1;                   // view width the layout was built for
    int cw = 0, ch = 0;
    Smooth sc;
    ScrollBar sb;
    int hx = 0;                       // horizontal scroll when wider than the view
    // bitmap cache
    struct Slot { Image img; int page = -1; int key = 0; u64 used = 0; };
    Slot slots[10];
    int fail_page = -1, fail_key = 0;
    // sidebar
    bool side = false;
    Smooth osc;
    // selection
    PdfPos sel_a{0, 0}, sel_b{0, 0};
    bool has_sel = false, selecting = false;
    // find
    bool find_on = false;
    TextField fld;
    char last_q[128] = "";
    int fp = -1, fi = 0, flen = 0;    // current match
    bool not_found = false;
    char tbuf[96];

    PdfApp(const char* arg) { if (arg && arg[0]) open_arg(arg); }
    ~PdfApp() {
        for (auto& s : slots) if (s.img.px) kfree(s.img.px);
        if (doc) pdf_close(doc);
        pw.free(); ph.free(); py.free();
    }
    const char* app_id() override { return "pdfview"; }
    const char* icon_name() override { return "pdf"; }
    const char* title() override {
        if (doc) { snprintf(tbuf, sizeof(tbuf), "%s - PDF", name); return tbuf; }
        return "PDF";
    }
    int pref_w() override { return 780; }
    int pref_h() override { return 580; }
    u32 background() override { return 0xFF5A5470; }

    // ------------------------------------------------------------ loading
    void unload() {
        for (auto& s : slots) { if (s.img.px) kfree(s.img.px); s = Slot(); }
        if (doc) pdf_close(doc);
        doc = nullptr;
        npages = 0;
        pw.clear(); ph.clear(); py.clear();
        has_sel = selecting = false;
        fp = -1; flen = 0; not_found = false;
        fail_page = -1;
        lay_w = -1;
        sc.jump(0); osc.jump(0); hx = 0;
    }
    void open_arg(const char* path) override {
        FsNode* f = fs_lookup(path);
        if (!f || f->dir) return;
        unload();
        err[0] = 0;
        strlcpy(name, f->name, sizeof(name));
        char e[96] = "";
        doc = pdf_open(f->data, f->size, e, sizeof(e));
        if (!doc) {
            snprintf(err, sizeof(err), "%s", e[0] ? e : "this isn't a PDF silicon understands");
        } else {
            npages = pdf_page_count(doc);
            for (int i = 0; i < npages; i++) {
                float w = 612, h = 792;
                pdf_page_size(doc, i, &w, &h);
                if (w < 1) w = 612;
                if (h < 1) h = 792;
                pw.push(w); ph.push(h);
            }
            fit_width = true;
            side = pdf_outline_count(doc) > 0 && cw > 520;
        }
        set_title(title());
        ui_invalidate();
    }

    // ------------------------------------------------------------ layout
    int side_w() { return side ? SIDE_W : 0; }
    int view_w() { return cw - side_w() - SB_W; }
    int view_h() { return ch - BAR_H; }

    void relayout() {
        if (!npages) return;
        int vw = view_w();
        float mw = 1;
        for (int i = 0; i < npages; i++) if (pw[i] > mw) mw = pw[i];
        float old_total = (float)max(total_h, 1);
        float frac = py.len ? (float)sc.target / old_total : 0;
        if (fit_width) scale = clamp((float)(vw - 2 * GAP) / mw, 0.15f, 6.0f);
        else scale = clamp(zoom_f, 0.15f, 6.0f);
        py.clear();
        int y = GAP;
        max_pw = 0;
        for (int i = 0; i < npages; i++) {
            py.push(y);
            y += page_h(i) + GAP;
            max_pw = max(max_pw, page_w(i));
        }
        total_h = y;
        lay_w = vw;
        int maxs = max(0, total_h - view_h());
        sc.jump(clamp((int)(frac * total_h), 0, maxs));
        clamp_h();
    }
    int page_w(int i) { return max(1, (int)(pw[i] * scale + 0.999f)); }
    int page_h(int i) { return max(1, (int)(ph[i] * scale + 0.999f)); }
    int area_w() { return max(view_w(), max_pw + 2 * GAP); }
    void clamp_h() { hx = clamp(hx, 0, max(0, area_w() - view_w())); }
    int page_left(int i) { return side_w() + (area_w() - page_w(i)) / 2 - hx; }
    int page_top(int i) { return BAR_H + py[i] - sc.pos(); }
    int scale_key() { return (int)(scale * 1000); }
    int cur_page() {
        int mid = sc.pos() + view_h() / 3;
        int p = 0;
        for (int i = 0; i < npages; i++) if (py[i] <= mid) p = i;
        return p;
    }
    void max_scroll_clamp() { sc.set(clamp(sc.target, 0, max(0, total_h - view_h()))); }
    void goto_page(int p) {
        if (!npages) return;
        p = clamp(p, 0, npages - 1);
        sc.set(clamp(py[p] - 6, 0, max(0, total_h - view_h())));
        ui_invalidate();
    }
    void set_zoom(float z) {
        if (!npages) return;
        fit_width = false;
        zoom_f = clamp(z, 0.25f, 5.0f);
        lay_w = -1;
        ui_invalidate();
    }

    // ------------------------------------------------------------ bitmap cache
    Slot* slot_for(int page) {
        for (auto& s : slots) if (s.page == page && s.key == scale_key() && s.img.px) return &s;
        return nullptr;
    }
    u32 cache_bytes() {
        u32 t = 0;
        for (auto& s : slots) if (s.img.px) t += (u32)s.img.w * s.img.h * 4;
        return t;
    }
    bool is_visible(int page) {
        int top = sc.pos(), bot = top + view_h();
        return py[page] + page_h(page) >= top && py[page] <= bot;
    }
    void evict_one(bool allow_visible) {
        Slot* v = nullptr;
        for (auto& s : slots) {
            if (!s.img.px) continue;
            if (!allow_visible && s.key == scale_key() && is_visible(s.page)) continue;
            if (!v || s.used < v->used) v = &s;
        }
        if (v) { kfree(v->img.px); *v = Slot(); }
    }
    void render_pending() {
        if (!doc || !npages || lay_w < 0) return;
        // stale scales out first
        for (auto& s : slots) if (s.img.px && s.key != scale_key()) { kfree(s.img.px); s = Slot(); }
        int mid = sc.pos() + view_h() / 2;
        int best = -1, bd = 0;
        for (int i = 0; i < npages; i++) {
            if (!is_visible(i) || slot_for(i)) continue;
            if (fail_page == i && fail_key == scale_key()) continue;
            int d = iabs(py[i] + page_h(i) / 2 - mid);
            if (best < 0 || d < bd) { best = i; bd = d; }
        }
        if (best < 0) return;
        u32 need = (u32)page_w(best) * page_h(best) * 4;
        if (need > CACHE_BUDGET) { fail_page = best; fail_key = scale_key(); return; }
        while (cache_bytes() + need > CACHE_BUDGET) {
            u32 before = cache_bytes();
            evict_one(false);
            if (cache_bytes() == before) break;
        }
        Slot* s = nullptr;
        for (auto& t : slots) if (!t.img.px) { s = &t; break; }
        if (!s) { evict_one(false); for (auto& t : slots) if (!t.img.px) { s = &t; break; } }
        if (!s) { evict_one(true); for (auto& t : slots) if (!t.img.px) { s = &t; break; } }
        if (!s) return;
        Image img;
        if (pdf_render(doc, best, scale, &img) && img.px) {
            s->img = img;
            s->page = best;
            s->key = scale_key();
            s->used = ticks();
        } else {
            fail_page = best;
            fail_key = scale_key();
        }
        ui_invalidate();
    }
    void tick() override {
        if (sc.step()) ui_invalidate();
        if (osc.step()) ui_invalidate();
        if (lay_w >= 0 && lay_w != view_w()) { lay_w = -1; ui_invalidate(); }
        if (doc && lay_w >= 0) render_pending();
    }
    void on_resize(int w, int h) override { cw = w; ch = h; lay_w = -1; }

    // ------------------------------------------------------------ text hit testing
    bool pos_at(int mx, int my, PdfPos* out) {
        if (!npages || lay_w < 0) return false;
        int dy = my - BAR_H + sc.pos();
        int pg = npages - 1;
        for (int i = 0; i < npages; i++) if (dy < py[i] + page_h(i) + GAP / 2) { pg = i; break; }
        float x = (mx - page_left(pg)) / scale, y = (dy - py[pg]) / scale;
        const PdfChar* t;
        int n = pdf_page_text(doc, pg, &t);
        int best = -1;
        float bd = 0;
        for (int i = 0; i < n; i++) {
            if (t[i].cp == 10) continue;
            float ddx = x < t[i].x0 ? t[i].x0 - x : x > t[i].x1 ? x - t[i].x1 : 0;
            float ddy = y < t[i].y0 ? t[i].y0 - y : y > t[i].y1 ? y - t[i].y1 : 0;
            float d = ddx + ddy * 4;
            if (best < 0 || d < bd) { best = i; bd = d; }
        }
        if (best < 0) return false;
        *out = {pg, best};
        return true;
    }
    void sel_range(PdfPos* lo, PdfPos* hi) {
        if (pos_less(sel_b, sel_a)) { *lo = sel_b; *hi = sel_a; }
        else { *lo = sel_a; *hi = sel_b; }
    }
    void copy_selection() {
        if (!doc || !has_sel) return;
        PdfPos lo, hi;
        sel_range(&lo, &hi);
        u32 cap = 4096, len = 0;
        char* buf = (char*)kmalloc_try(cap);
        if (!buf) return;
        for (int p = lo.page; p <= hi.page; p++) {
            const PdfChar* t;
            int n = pdf_page_text(doc, p, &t);
            int a = p == lo.page ? lo.idx : 0, b = p == hi.page ? hi.idx : n - 1;
            for (int i = a; i <= b && i < n; i++) {
                if (len + 8 > cap) {
                    char* nb = (char*)krealloc(buf, cap * 2);
                    if (!nb) break;
                    buf = nb; cap *= 2;
                }
                len += utf8_encode(t[i].cp, buf + len);
            }
            if (p < hi.page && len + 2 < cap) buf[len++] = '\n';
        }
        buf[len] = 0;
        clip_set(buf, len);
        kfree(buf);
        ui_notify("Copied", "selected text is on the clipboard", "pdf");
    }

    // ------------------------------------------------------------ find
    static u32 fold(u32 c) { return c >= 'A' && c <= 'Z' ? c + 32 : (c == 0xA0 || c == 10) ? ' ' : c; }
    bool match_at(const PdfChar* t, int n, int i, const char* q, int ql) {
        if (i + ql > n) return false;
        for (int j = 0; j < ql; j++) if (fold(t[i + j].cp) != fold((u8)q[j])) return false;
        return true;
    }
    bool find_from(int page, int idx, int dir) {
        int ql = (int)strlen(fld.text);
        if (!doc || ql == 0) { fp = -1; flen = 0; not_found = false; return false; }
        for (int k = 0; k <= npages; k++) {
            int p = ((page + dir * k) % npages + npages) % npages;
            const PdfChar* t;
            int n = pdf_page_text(doc, p, &t);
            bool first = k == 0;
            if (dir > 0) {
                for (int i = first ? idx : 0; i + ql <= n; i++)
                    if (match_at(t, n, i, fld.text, ql)) { set_match(p, i, ql, t); return true; }
            } else {
                for (int i = first ? min(idx, n - ql) : n - ql; i >= 0; i--)
                    if (match_at(t, n, i, fld.text, ql)) { set_match(p, i, ql, t); return true; }
            }
        }
        fp = -1; flen = 0; not_found = true;
        return false;
    }
    void set_match(int p, int i, int ql, const PdfChar* t) {
        fp = p; fi = i; flen = ql; not_found = false;
        int y = py[p] + (int)(t[i].y0 * scale);
        sc.set(clamp(y - view_h() / 3, 0, max(0, total_h - view_h())));
        int x = (int)(t[i].x0 * scale) + (area_w() - page_w(p)) / 2;
        if (x < hx || x > hx + view_w() - 40) { hx = x - view_w() / 3; clamp_h(); }
    }
    void find_step(int dir) {
        if (fp >= 0) find_from(fp, fi + dir, dir);
        else find_from(cur_page(), dir > 0 ? 0 : 1 << 20, dir);
        ui_invalidate();
    }
    void toggle_find() {
        find_on = !find_on;
        fld.focused = find_on;
        if (find_on) fld.select_all();
        ui_invalidate();
    }

    // ------------------------------------------------------------ drawing
    void highlight(Canvas& c, int pg, int a, int b, int pl, int sy, u32 col, u32 alpha) {
        const PdfChar* t;
        int n = pdf_page_text(doc, pg, &t);
        a = max(a, 0); b = min(b, n - 1);
        bool run = false;
        float rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
        auto flush = [&]() {
            if (!run) return;
            int x0 = pl + (int)(rx0 * scale), x1 = pl + (int)(rx1 * scale + 0.99f);
            int y0 = sy + (int)(ry0 * scale), y1 = sy + (int)(ry1 * scale + 0.99f);
            c.fill_alpha(x0, y0, max(1, x1 - x0), max(1, y1 - y0), col, alpha);
            run = false;
        };
        for (int i = a; i <= b; i++) {
            if (t[i].cp == 10) continue;
            float h = t[i].y1 - t[i].y0;
            if (run && iabs((int)((t[i].y0 - ry0) * 4)) < (int)((ry1 - ry0) * 2) && t[i].x0 <= rx1 + h * 0.8f && t[i].x1 >= rx0) {
                rx0 = min(rx0, t[i].x0); rx1 = max(rx1, t[i].x1);
                ry0 = min(ry0, t[i].y0); ry1 = max(ry1, t[i].y1);
            } else {
                flush();
                run = true;
                rx0 = t[i].x0; rx1 = t[i].x1; ry0 = t[i].y0; ry1 = t[i].y1;
            }
        }
        flush();
    }

    void draw_page(Canvas& c, int i) {
        int x = page_left(i), y = page_top(i), w = page_w(i), h = page_h(i);
        c.fill_alpha(x + 4, y + 4, w, h, 0xFF000000, 90);
        c.fill(x, y, w, h, 0xFFFFFFFF);
        Slot* s = slot_for(i);
        if (s) {
            s->used = ticks();
            if (s->img.w == w && s->img.h == h) c.blit(s->img, x, y);
            else c.blit_scaled(s->img, x, y, w, h);
        } else if (fail_page == i && fail_key == scale_key()) {
            c.text_centered(x, y + h / 2 - 6, w, "can't draw this page", 0xFF8A7EA8);
        } else {
            c.text_centered(x, y + h / 2 - 6, w, "rendering" G_ELLIP, 0xFFB0A8C8);
        }
        if (!doc) return;
        // find matches
        if (find_on && fld.text[0]) {
            int ql = (int)strlen(fld.text);
            const PdfChar* t;
            int n = pdf_page_text(doc, i, &t);
            for (int k = 0; k + ql <= n; k++) {
                if (!match_at(t, n, k, fld.text, ql)) continue;
                bool cur = i == fp && k == fi;
                highlight(c, i, k, k + ql - 1, x, y, cur ? 0xFFFF8C1A : 0xFFFFD21E, cur ? 150 : 110);
                k += ql - 1;
            }
        }
        if (has_sel) {
            PdfPos lo, hi;
            sel_range(&lo, &hi);
            if (i >= lo.page && i <= hi.page)
                highlight(c, i, i == lo.page ? lo.idx : 0, i == hi.page ? hi.idx : 1 << 20, x, y, 0xFF2D62E0, 90);
        }
    }

    void draw_outline(Canvas& c, int h) {
        int sw = SIDE_W;
        c.fill(0, BAR_H, sw, h - BAR_H, th::paper);
        c.vline(sw - 1, BAR_H, h - BAR_H, th::border);
        c.set_clip({0, BAR_H, sw - 1, h - BAR_H});
        int n = pdf_outline_count(doc);
        int cp = cur_page();
        for (int i = 0; i < n; i++) {
            const PdfOutline* o = pdf_outline(doc, i);
            int y = BAR_H + 6 + i * 18 - osc.pos();
            if (y + 18 < BAR_H || y > h) continue;
            bool on = hot(0, y, sw - 1, 18);
            if (on) c.fill(2, y, sw - 5, 18, blend(th::paper, accent(), 70));
            else if (o->page == cp) c.fill(2, y, sw - 5, 18, th::panel);
            int ind = 8 + min(o->level, 5) * 10;
            c.text_n(ind, y + 5, o->title, (sw - ind - 6) / 8, o->page >= 0 ? th::text : th::muted);
        }
        c.set_clip({0, 0, cw, h});
    }

    void draw(Canvas& c, int w, int h) override {
        if (w != cw || h != ch) { cw = w; ch = h; lay_w = -1; }
        // toolbar
        c.fill(0, 0, w, BAR_H, th::panel);
        c.hline(0, BAR_H - 1, w, th::border);
        draw_button(c, 8, 6, 44, 22, "List", hot(8, 6, 44, 22), side, doc && pdf_outline_count(doc) > 0);
        draw_button(c, 58, 6, 26, 22, "<", hot(58, 6, 26, 22), false, doc && cur_page() > 0);
        draw_button(c, 88, 6, 26, 22, ">", hot(88, 6, 26, 22), false, doc && cur_page() < npages - 1);
        draw_button(c, 210, 6, 26, 22, "-", hot(210, 6, 26, 22), false, doc != nullptr);
        draw_button(c, 240, 6, 26, 22, "+", hot(240, 6, 26, 22), false, doc != nullptr);
        draw_button(c, 270, 6, 40, 22, "fit", hot(270, 6, 40, 22), fit_width, doc != nullptr);
        draw_button(c, w - 58, 6, 50, 22, "Find", hot(w - 58, 6, 50, 22), find_on, doc != nullptr);
        if (doc) {
            char b[40];
            snprintf(b, sizeof(b), "%d / %d", cur_page() + 1, npages);
            c.text(122, 11, b, th::text);
            snprintf(b, sizeof(b), "%d%%", (int)(scale * 100 / 1.3333f + 0.5f));
            c.text(316, 11, b, th::muted);
        }
        if (find_on) {
            fld.focused = true;
            fld.draw(c, w - 268, 6, 150);
            draw_button(c, w - 114, 6, 24, 22, "<", hot(w - 114, 6, 24, 22));
            draw_button(c, w - 86, 6, 24, 22, ">", hot(w - 86, 6, 24, 22));
            if (not_found && w > 700) c.text(w - 268 - 80, 11, "not found", th::red);
        }
        if (!doc) {
            c.fill(0, BAR_H, w, h - BAR_H, 0xFF5A5470);
            c.text_centered(0, BAR_H + (h - BAR_H) / 2 - 24, w, err[0] ? err : "no document open", 0xFFE0DAEE, 2, true);
            if (!err[0]) c.text_centered(0, BAR_H + (h - BAR_H) / 2 + 14, w, "open one from Files, or use File > Open", 0xFFB8ACD6);
            draw_button(c, w / 2 - 50, BAR_H + (h - BAR_H) / 2 + 44, 100, 24, "Open" G_ELLIP, hot(w / 2 - 50, BAR_H + (h - BAR_H) / 2 + 44, 100, 24), true);
            return;
        }
        if (lay_w < 0) relayout();
        // pages
        c.set_clip({side_w(), BAR_H, view_w(), h - BAR_H});
        for (int i = 0; i < npages; i++) {
            int y = page_top(i);
            if (y + page_h(i) < BAR_H || y > h) continue;
            draw_page(c, i);
        }
        c.set_clip({0, 0, w, h});
        sb.draw(c, w - SB_W, BAR_H, h - BAR_H, total_h, view_h(), sc.pos());
        if (side) draw_outline(c, h);
    }

    // ------------------------------------------------------------ input
    bool in_pages(int x, int y) { return x >= side_w() && x < cw - SB_W && y >= BAR_H; }
    int cursor_at(int x, int y) override { return doc && in_pages(x, y) ? CUR_IBEAM : CUR_ARROW; }
    bool wants_ibeam(int x, int y) override { return doc && in_pages(x, y); }

    void open_dialog() {
        pick_file("Open PDF", "/home", "pdf", false, "", [](App* a, const char* p) { ((PdfApp*)a)->open_arg(p); });
    }

    void on_mouse(const MouseEv& e) override {
        if (!doc) {
            if (e.kind == M_UP && e.button == 1 && Rect{cw / 2 - 50, BAR_H + (ch - BAR_H) / 2 + 44, 100, 24}.contains(e.x, e.y)) open_dialog();
            return;
        }
        if (e.y < BAR_H) {
            if (find_on && e.x >= cw - 268 && e.x < cw - 118) {
                if (e.kind == M_DOWN && e.button == 1) fld.click(e.x - (cw - 268), 150, kbd_shift_down());
                else if (e.kind == M_MOVE && e.left) fld.drag(e.x - (cw - 268), 150);
                ui_invalidate();
                return;
            }
            if (e.kind != M_UP || e.button != 1) return;
            if (Rect{8, 6, 44, 22}.contains(e.x, e.y) && pdf_outline_count(doc) > 0) { side = !side; lay_w = -1; }
            else if (Rect{58, 6, 26, 22}.contains(e.x, e.y)) goto_page(cur_page() - 1);
            else if (Rect{88, 6, 26, 22}.contains(e.x, e.y)) goto_page(cur_page() + 1);
            else if (Rect{210, 6, 26, 22}.contains(e.x, e.y)) set_zoom(scale / 1.2f);
            else if (Rect{240, 6, 26, 22}.contains(e.x, e.y)) set_zoom(scale * 1.2f);
            else if (Rect{270, 6, 40, 22}.contains(e.x, e.y)) { fit_width = true; lay_w = -1; }
            else if (Rect{cw - 58, 6, 50, 22}.contains(e.x, e.y)) toggle_find();
            else if (find_on && Rect{cw - 114, 6, 24, 22}.contains(e.x, e.y)) find_step(-1);
            else if (find_on && Rect{cw - 86, 6, 24, 22}.contains(e.x, e.y)) find_step(1);
            ui_invalidate();
            return;
        }
        // outline sidebar
        if (side && e.x < SIDE_W) {
            int n = pdf_outline_count(doc);
            int maxo = max(0, n * 18 + 12 - view_h());
            if (e.kind == M_WHEEL) { osc.set(clamp(osc.target - e.wheel * 54, 0, maxo)); ui_invalidate(); }
            else if (e.kind == M_UP && e.button == 1) {
                int i = (e.y - BAR_H - 6 + osc.pos()) / 18;
                if (i >= 0 && i < n) { const PdfOutline* o = pdf_outline(doc, i); if (o->page >= 0) goto_page(o->page); }
            } else if (e.kind == M_MOVE) ui_invalidate();
            return;
        }
        int off = sc.target;
        if (sb.handle(e, cw - SB_W, BAR_H, view_h(), total_h, view_h(), off)) { sc.jump(off); ui_invalidate(); return; }
        if (lay_w < 0) return;
        if (e.kind == M_WHEEL) {
            if (kbd_ctrl_down()) set_zoom(scale * (e.wheel > 0 ? 1.15f : 1 / 1.15f));
            else if (kbd_shift_down()) { hx += -e.wheel * 60; clamp_h(); }
            else { sc.set(clamp(sc.target - e.wheel * 70, 0, max(0, total_h - view_h()))); }
            ui_invalidate();
            return;
        }
        PdfPos p;
        if (e.kind == M_DOWN && e.button == 1) {
            fld.focused = false;
            if (pos_at(e.x, e.y, &p)) { sel_a = sel_b = p; selecting = true; has_sel = false; }
            else { selecting = false; has_sel = false; }
            ui_invalidate();
        } else if (e.kind == M_MOVE && selecting && e.left) {
            if (pos_at(e.x, e.y, &p)) { sel_b = p; has_sel = p.page != sel_a.page || p.idx != sel_a.idx; ui_invalidate(); }
        } else if (e.kind == M_UP) {
            selecting = false;
        } else if (e.kind == M_DBL && pos_at(e.x, e.y, &p)) {
            const PdfChar* t;
            int n = pdf_page_text(doc, p.page, &t);
            auto sep = [&](int i) { return t[i].cp == 32 || t[i].cp == 10 || t[i].cp == 0xA0; };
            int a = p.idx, b = p.idx;
            while (a > 0 && !sep(a - 1)) a--;
            while (b < n - 1 && !sep(b + 1)) b++;
            sel_a = {p.page, a}; sel_b = {p.page, b};
            has_sel = !sep(p.idx);
            selecting = false;
            ui_invalidate();
        }
    }

    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.ctrl && e.key == 'o') { open_dialog(); return; }
        if (!doc) return;
        if (e.ctrl && e.key == 'f') { toggle_find(); return; }
        if (e.ctrl && e.key == 'c' && !(find_on && fld.has_sel())) { copy_selection(); return; }
        if (e.ctrl && e.key == 'a') {
            sel_a = {0, 0};
            const PdfChar* t;
            int n = pdf_page_text(doc, npages - 1, &t);
            sel_b = {npages - 1, max(0, n - 1)};
            has_sel = true;
            ui_invalidate();
            return;
        }
        if (find_on) {
            if (e.key == K_ESC) { find_on = false; fld.focused = false; ui_invalidate(); return; }
            if (e.key == K_ENTER) { find_step(e.shift ? -1 : 1); return; }
            char before[128];
            strlcpy(before, fld.text, sizeof(before));
            if (fld.on_key(e)) {
                if (strcmp(before, fld.text)) {
                    strlcpy(last_q, fld.text, sizeof(last_q));
                    if (fp >= 0) find_from(fp, fi, 1);
                    else find_from(cur_page(), 0, 1);
                }
                ui_invalidate();
                return;
            }
        }
        int step = 50, page = max(40, view_h() - 40);
        switch (e.key) {
        case K_DOWN: sc.set(sc.target + step); break;
        case K_UP: sc.set(sc.target - step); break;
        case K_PGDN: case ' ': sc.set(sc.target + (e.shift && e.key == ' ' ? -page : page)); break;
        case K_PGUP: sc.set(sc.target - page); break;
        case K_HOME: sc.set(0); break;
        case K_END: sc.set(total_h); break;
        case K_LEFT: if (area_w() > view_w()) { hx -= 60; clamp_h(); } else goto_page(cur_page() - 1); break;
        case K_RIGHT: if (area_w() > view_w()) { hx += 60; clamp_h(); } else goto_page(cur_page() + 1); break;
        case '+': case '=': set_zoom(scale * 1.2f); break;
        case '-': set_zoom(scale / 1.2f); break;
        case '0': fit_width = true; lay_w = -1; break;
        case '1': set_zoom(1.3333f); break;
        default: return;
        }
        max_scroll_clamp();
        ui_invalidate();
    }

    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "File", 24);
        m[0].add("Open" G_ELLIP, 1, "ctrl+o");
        m[1].clear(); strlcpy(m[1].title, "Edit", 24);
        m[1].add("Copy", 2, "ctrl+c", has_sel).add("Select all", 12, "ctrl+a", doc != nullptr).sep()
            .add("Find" G_ELLIP, 3, "ctrl+f", doc != nullptr);
        m[2].clear(); strlcpy(m[2].title, "View", 24);
        m[2].add("Outline", 4, "", doc && pdf_outline_count(doc) > 0, side).sep()
            .add("Zoom in", 5, "+", doc != nullptr).add("Zoom out", 6, "-", doc != nullptr)
            .add("Fit width", 7, "0", doc != nullptr, fit_width).add("Actual size", 11, "1", doc != nullptr);
        m[3].clear(); strlcpy(m[3].title, "Go", 24);
        m[3].add("Next page", 8, "right", doc != nullptr).add("Previous page", 9, "left", doc != nullptr).sep()
            .add("First page", 10, "home", doc != nullptr);
        return 4;
    }
    void on_command(int cmd) override {
        switch (cmd) {
        case 1: open_dialog(); break;
        case 2: copy_selection(); break;
        case 3: toggle_find(); break;
        case 4: side = !side; lay_w = -1; break;
        case 5: set_zoom(scale * 1.2f); break;
        case 6: set_zoom(scale / 1.2f); break;
        case 7: fit_width = true; lay_w = -1; break;
        case 8: goto_page(cur_page() + 1); break;
        case 9: goto_page(cur_page() - 1); break;
        case 10: goto_page(0); break;
        case 11: set_zoom(1.3333f); break;
        case 12:
            if (doc) {
                const PdfChar* t;
                int n = pdf_page_text(doc, npages - 1, &t);
                sel_a = {0, 0}; sel_b = {npages - 1, max(0, n - 1)}; has_sel = true;
            }
            break;
        }
        ui_invalidate();
    }
};

App* new_pdfview(const char* arg) { return new PdfApp(arg); }
