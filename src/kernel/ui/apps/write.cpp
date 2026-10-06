// silicon - Write: a Unicode text editor with scalable fonts (DejaVu Sans), right-to-left text
// (Hebrew and Arabic, with Arabic letter shaping and bidirectional ordering) and the keyboard
// layouts of drv/kbd.cpp. Files are UTF-8.
#include "apps.h"
#include "../../gfx/vfont.h"

// ---------------------------------------------------------------- Arabic shaping and bidi helpers
struct ArForm { u16 cp, form; u8 n; };     // n: 1 isolated only, 2 isolated + final, 4 isolated, final, initial, medial
static const ArForm ar_forms[] = {
    {0x621, 0xFE80, 1}, {0x622, 0xFE81, 2}, {0x623, 0xFE83, 2}, {0x624, 0xFE85, 2}, {0x625, 0xFE87, 2}, {0x626, 0xFE89, 4},
    {0x627, 0xFE8D, 2}, {0x628, 0xFE8F, 4}, {0x629, 0xFE93, 2}, {0x62A, 0xFE95, 4}, {0x62B, 0xFE99, 4}, {0x62C, 0xFE9D, 4},
    {0x62D, 0xFEA1, 4}, {0x62E, 0xFEA5, 4}, {0x62F, 0xFEA9, 2}, {0x630, 0xFEAB, 2}, {0x631, 0xFEAD, 2}, {0x632, 0xFEAF, 2},
    {0x633, 0xFEB1, 4}, {0x634, 0xFEB5, 4}, {0x635, 0xFEB9, 4}, {0x636, 0xFEBD, 4}, {0x637, 0xFEC1, 4}, {0x638, 0xFEC5, 4},
    {0x639, 0xFEC9, 4}, {0x63A, 0xFECD, 4}, {0x641, 0xFED1, 4}, {0x642, 0xFED5, 4}, {0x643, 0xFED9, 4}, {0x644, 0xFEDD, 4},
    {0x645, 0xFEE1, 4}, {0x646, 0xFEE5, 4}, {0x647, 0xFEE9, 4}, {0x648, 0xFEED, 2}, {0x649, 0xFEEF, 2}, {0x64A, 0xFEF1, 4},
};
static const ArForm* ar_find(u32 cp) {
    if (cp < 0x621 || cp > 0x64A) return nullptr;
    for (auto& f : ar_forms) if (f.cp == cp) return &f;
    return nullptr;
}
static bool ar_transparent(u32 cp) { return (cp >= 0x64B && cp <= 0x65F) || cp == 0x670; }

// turns logical Arabic letters into their presentation forms (lam + alef become one glyph; the alef slot becomes 0)
static void shape_arabic(const u32* cps, int n, u32* out) {
    for (int i = 0; i < n; i++) out[i] = cps[i];
    for (int i = 0; i < n; i++) {
        const ArForm* f = ar_find(cps[i]);
        if (!f) continue;
        int p = i - 1;
        while (p >= 0 && ar_transparent(cps[p])) p--;
        const ArForm* pf = p >= 0 ? ar_find(cps[p]) : nullptr;
        bool prev_joins = pf && pf->n == 4 && out[p] != 0;
        int q = i + 1;
        while (q < n && ar_transparent(cps[q])) q++;
        const ArForm* nf = q < n ? ar_find(cps[q]) : nullptr;
        // lam + alef ligature
        if (cps[i] == 0x644 && nf && q == i + 1 && (cps[q] == 0x627 || cps[q] == 0x623 || cps[q] == 0x625 || cps[q] == 0x622)) {
            u32 iso = cps[q] == 0x627 ? 0xFEFB : cps[q] == 0x623 ? 0xFEF7 : cps[q] == 0x625 ? 0xFEF9 : 0xFEF5;
            out[i] = prev_joins ? iso + 1 : iso;
            out[q] = 0;
            i = q;
            continue;
        }
        bool next_joins = f->n == 4 && nf && nf->n >= 2;
        int form = 0;
        if (prev_joins && f->n >= 2) form = next_joins ? 3 : 1;
        else form = next_joins ? 2 : 0;
        if (form >= f->n) form = form == 3 ? 1 : 0;
        out[i] = f->form + form;
    }
}

enum { BC_L, BC_R, BC_N };
static int bidi_class(u32 cp) {
    if ((cp >= 0x590 && cp <= 0x8FF && !(cp >= 0x660 && cp <= 0x669)) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF)) {
        if (cp >= 0x64B && cp <= 0x65F) return BC_N;               // marks follow their letter
        return BC_R;
    }
    if (cp == ' ' || cp == '\t' || (cp < 0x30 && cp > 0) || (cp >= 0x3A && cp <= 0x40) || (cp >= 0x5B && cp <= 0x60) || (cp >= 0x7B && cp <= 0xBF && cp != 0xAA && cp != 0xB5 && cp != 0xBA)) return BC_N;
    if (cp >= 0x2000 && cp <= 0x206F) return BC_N;
    return BC_L;
}
static u32 mirror(u32 c) {
    switch (c) { case '(': return ')'; case ')': return '('; case '[': return ']'; case ']': return '['; case '{': return '}'; case '}': return '{'; case '<': return '>'; case '>': return '<'; }
    return c;
}

struct WriteApp : App {
    Vec<char> buf;                 // UTF-8 text
    int cur = 0, anc = -1;         // caret and selection anchor: byte offsets
    float size = 20;
    bool bold = false;
    char path[256] = "";
    bool dirty = false;
    Smooth sc;
    ScrollBar sb;
    int cw = 0, ch = 0;
    float want_x = -1;
    bool dragging = false;
    int cursor_blink_t = 0;

    struct Glyph { int bi, bl; u32 shown; int gid; float x, adv; int level; };
    struct Line { int a, b; int g0, g1; float y, h; bool rtl; float x0, w; };
    Vec<Glyph> glyphs;
    Vec<Line> lines;
    float doc_h = 0;
    int lay_w = -1;
    bool lay_dirty = true;
    char tbuf[96];
    static const int BAR = 34, ST = 20, MARGIN = 16;

    WriteApp(const char* arg) { if (arg && arg[0]) open_arg(arg); }
    ~WriteApp() { buf.free(); glyphs.free(); lines.free(); }
    const char* app_id() override { return "write"; }
    const char* title() override {
        const char* n = path[0] ? strrchr(path, '/') + 1 : "untitled";
        snprintf(tbuf, sizeof(tbuf), "%s%s - Write", n, dirty ? " *" : "");
        return tbuf;
    }
    int pref_w() override { return 700; }
    int pref_h() override { return 500; }
    u32 background() override { return 0xFFFDFBFF; }

    // ------------------------------------------------------------ text helpers
    int len() { return buf.len; }
    int char_len_at(int i) { if (i >= buf.len) return 0; u8 c = (u8)buf[i]; int n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; return min(n, buf.len - i); }
    int prev_char(int i) { if (i <= 0) return 0; i--; while (i > 0 && ((u8)buf[i] & 0xC0) == 0x80) i--; return i; }
    u32 cp_at(int i) { const char* p = buf.data + i; return utf8_decode(p, buf.data + buf.len); }
    void insert(const char* s, int n) {
        delete_selection();
        for (int i = 0; i < n; i++) buf.insert(cur + i, s[i]);
        cur += n;
        touch();
    }
    void insert_cp(u32 cp) { char u[5]; int n = utf8_encode(cp, u); insert(u, n); }
    bool has_sel() { return anc >= 0 && anc != cur; }
    void sel_range(int* a, int* b) { *a = min(anc, cur); *b = max(anc, cur); }
    void delete_selection() {
        if (!has_sel()) { anc = -1; return; }
        int a, b;
        sel_range(&a, &b);
        for (int i = b - 1; i >= a; i--) buf.remove(i);
        cur = a; anc = -1;
        touch();
    }
    void touch() { lay_dirty = true; dirty = true; want_x = -1; set_title(title()); ui_invalidate(); }
    void copy_sel(bool cut) {
        if (!has_sel()) return;
        int a, b;
        sel_range(&a, &b);
        char* t = (char*)kmalloc(b - a + 1);
        for (int i = a; i < b; i++) t[i - a] = buf[i];
        t[b - a] = 0;
        clip_set(t, (u32)(b - a));
        kfree(t);
        if (cut) delete_selection();
    }
    void paste() {
        const char* t = clip_get();
        u32 n = clip_len();
        if (!n) return;
        // keep it a plain text document: no carriage returns or control characters
        delete_selection();
        for (u32 i = 0; i < n; i++) {
            u8 c = (u8)t[i];
            if (c == '\r' || (c < 32 && c != '\n' && c != '\t')) continue;
            buf.insert(cur++, (char)c);
        }
        touch();
    }

    // ------------------------------------------------------------ files
    void open_arg(const char* p) override {
        FsNode* f = fs_lookup(p);
        if (!f || f->dir) return;
        buf.clear();
        for (u32 i = 0; i < f->size; i++) if (f->data[i] != '\r') buf.push((char)f->data[i]);
        strlcpy(path, p, sizeof(path));
        cur = 0; anc = -1; dirty = false; lay_dirty = true;
        sc.jump(0);
        set_title(title());
        ui_invalidate();
    }
    bool on_drop(const char* p, int, int) override {
        FsNode* f = fs_lookup(p);
        if (!f || f->dir) return false;
        open_arg(p);
        return true;
    }
    void save_to(const char* p) {
        FsNode* f = fs_lookup(p);
        if (!f) f = fs_create_path(p, false);
        if (!f || f->dir) { info("can't save there."); return; }
        if (!fs_write(f, buf.data, (u32)buf.len)) { info("the disk is full."); return; }
        strlcpy(path, p, sizeof(path));
        dirty = false;
        set_title(title());
        ui_invalidate();
    }
    void save() {
        if (path[0]) save_to(path);
        else pick_file("Save document", "/home", "txt", true, "untitled.txt", [](App* a, const char* p) { ((WriteApp*)a)->save_to(p); });
    }

    // ------------------------------------------------------------ layout
    VFont* font() { return vfont_get(bold); }
    void relayout() {
        lay_dirty = false;
        glyphs.clear(); lines.clear();
        VFont* f = font();
        int page_w = max(80, lay_w);
        lay_w = page_w;
        float px = size;
        float lh = vfont_line_height(px);
        float y = 0;
        int p = 0;
        int total = buf.len;
        for (;;) {
            int pa = p, pb = p;
            while (pb < total && buf[pb] != '\n') pb++;
            layout_paragraph(f, pa, pb, px, lh, &y, page_w - 2 * MARGIN);
            if (pb >= total) break;
            p = pb + 1;
        }
        doc_h = y + 40;
    }
    void layout_paragraph(VFont* f, int pa, int pb, float px, float lh, float* y, float width) {
        // decode
        Vec<u32> cps; Vec<int> bi; Vec<int> bl;
        for (int i = pa; i < pb;) {
            const char* s = buf.data + i;
            const char* e = buf.data + pb;
            u32 c = utf8_decode(s, e);
            int l = (int)(s - (buf.data + i));
            cps.push(c == '\t' ? ' ' : c); bi.push(i); bl.push(l);
            i += l;
        }
        int n = cps.len;
        bool rtl = false;
        for (int i = 0; i < n; i++) { int k = bidi_class(cps[i]); if (k == BC_R) { rtl = true; break; } if (k == BC_L) break; }
        Vec<u32> shaped;
        for (int i = 0; i < n; i++) shaped.push(0);
        if (n) shape_arabic(cps.data, n, shaped.data);
        if (n == 0) {
            lines.push({pa, pb, glyphs.len, glyphs.len, *y, lh, rtl, 0, 0});
            *y += lh;
            cps.free(); bi.free(); bl.free(); shaped.free();
            return;
        }
        // widths
        Vec<float> adv; Vec<int> gids;
        for (int i = 0; i < n; i++) {
            int g = shaped[i] ? vfont_gid(f, shaped[i]) : 0;
            gids.push(g);
            adv.push(shaped[i] ? vfont_advance(f, g, px) : 0.0f);
        }
        int i0 = 0;
        while (i0 < n) {
            // greedy line break at spaces
            float w = 0;
            int i1 = i0, last_space = -1;
            while (i1 < n) {
                if (w + adv[i1] > width && i1 > i0) break;
                w += adv[i1];
                if (cps[i1] == ' ') last_space = i1;
                i1++;
            }
            if (i1 < n && last_space >= i0) i1 = last_space + 1;
            emit_line(pa, pb, i0, i1, cps, shaped, bi, bl, gids, adv, rtl, *y, lh, width);
            *y += lh;
            i0 = i1;
        }
        cps.free(); bi.free(); bl.free(); shaped.free(); adv.free(); gids.free();
    }
    void emit_line(int pa, int pb, int i0, int i1, Vec<u32>& cps, Vec<u32>& shaped, Vec<int>& bi, Vec<int>& bl, Vec<int>& gids, Vec<float>& adv,
                   bool rtl, float y, float lh, float width) {
        int n = i1 - i0;
        // bidi classes and levels
        Vec<int> cls, lvl, order;
        for (int i = 0; i < n; i++) cls.push(bidi_class(cps[i0 + i]));
        // neutrals take the direction of their neighbours when both agree, else the paragraph direction
        int base = rtl ? BC_R : BC_L;
        for (int i = 0; i < n; i++) {
            if (cls[i] != BC_N) continue;
            int j = i;
            while (j < n && cls[j] == BC_N) j++;
            int before = i > 0 ? cls[i - 1] : base, after = j < n ? cls[j] : base;
            int d = before == after ? before : base;
            for (int k = i; k < j; k++) cls[k] = d == BC_N ? base : d;
            i = j - 1;
        }
        for (int i = 0; i < n; i++) lvl.push(rtl ? (cls[i] == BC_R ? 1 : 2) : (cls[i] == BC_R ? 1 : 0));
        for (int i = 0; i < n; i++) order.push(i);
        int maxl = 0, minodd = 99;
        for (int i = 0; i < n; i++) { maxl = max(maxl, lvl[i]); if (lvl[i] & 1) minodd = min(minodd, lvl[i]); }
        for (int L = maxl; L >= minodd && L >= 1; L--) {
            for (int i = 0; i < n;) {
                if (lvl[order[i]] < L) { i++; continue; }
                int j = i;
                while (j < n && lvl[order[j]] >= L) j++;
                for (int a = i, b = j - 1; a < b; a++, b--) swap(order[a], order[b]);
                i = j;
            }
        }
        // visual positions
        float total = 0;
        for (int i = 0; i < n; i++) total += adv[i0 + i];
        float x = rtl ? width - total : 0;
        int g0 = glyphs.len;
        for (int i = 0; i < n; i++) glyphs.push({0, 0, 0, 0, 0, 0, 0});
        for (int v = 0; v < n; v++) {
            int li = order[v];
            Glyph& g = glyphs[g0 + li];
            g.bi = bi[i0 + li]; g.bl = bl[i0 + li];
            u32 sh = shaped[i0 + li];
            if ((lvl[li] & 1) && sh) sh = mirror(sh);
            g.shown = sh;
            g.gid = sh ? (sh == shaped[i0 + li] ? gids[i0 + li] : vfont_gid(font(), sh)) : 0;
            g.adv = adv[i0 + li];
            g.x = x;
            g.level = lvl[li];
            x += g.adv;
        }
        int a = bi[i0], b = i1 < (int)cps.len ? bi[i1] : pb;
        lines.push({a, b, g0, g0 + n, y, lh, rtl, rtl ? width - total : 0, total});
        (void)pa;
        cls.free(); lvl.free(); order.free();
    }

    // line and caret geometry
    int line_of(int byte) {
        for (int i = 0; i < lines.len; i++) {
            if (byte >= lines[i].a && byte < lines[i].b) return i;
            if (byte == lines[i].b && (i + 1 >= lines.len || lines[i + 1].a != byte || true) && (i + 1 >= lines.len || lines[i + 1].a > byte || buf.len == byte || (byte < buf.len && buf[byte] == '\n'))) return i;
        }
        return max(0, lines.len - 1);
    }
    float caret_x(int line, int byte) {
        Line& L = lines[line];
        for (int g = L.g0; g < L.g1; g++) {
            if (glyphs[g].bi == byte) return (glyphs[g].level & 1) ? glyphs[g].x + glyphs[g].adv : glyphs[g].x;
        }
        if (L.g1 == L.g0) return L.x0;
        Glyph& last = glyphs[L.g1 - 1];
        // after the last logical character
        return (last.level & 1) ? last.x : last.x + last.adv;
    }
    int byte_at(int line, float x) {
        Line& L = lines[line];
        int best = L.b;
        float bd = 1e9f;
        for (int g = L.g0; g < L.g1; g++) {
            Glyph& G = glyphs[g];
            bool odd = G.level & 1;
            float before = odd ? G.x + G.adv : G.x, after = odd ? G.x : G.x + G.adv;
            if (fabs_(x - before) < bd) { bd = fabs_(x - before); best = G.bi; }
            if (fabs_(x - after) < bd) { bd = fabs_(x - after); best = G.bi + G.bl; }
        }
        if (L.g1 > L.g0) { int last_end = glyphs[L.g1 - 1].bi + glyphs[L.g1 - 1].bl; (void)last_end; }
        return min(best, buf.len);
    }
    static float fabs_(float v) { return v < 0 ? -v : v; }

    // ------------------------------------------------------------ drawing
    int view_w() { return cw - 11; }
    int view_h() { return ch - BAR - ST; }
    void tick() override { if (sc.step()) ui_invalidate(); static u64 lt; if (ticks() - lt > 500) { lt = ticks(); ui_invalidate(); } }

    void draw(Canvas& c, int w, int h) override {
        if (w != cw || h != ch) { cw = w; ch = h; lay_dirty = true; }
        VFont* f = font();
        c.fill(0, 0, w, BAR, th::panel);
        c.hline(0, BAR - 1, w, th::border);
        draw_button(c, 8, 6, 26, 22, "A-", hot(8, 6, 26, 22));
        char b[32];
        snprintf(b, sizeof(b), "%d", (int)size);
        c.text_centered(36, 11, 28, b, th::text, 1, true);
        draw_button(c, 66, 6, 26, 22, "A+", hot(66, 6, 26, 22));
        draw_button(c, 98, 6, 26, 22, "B", hot(98, 6, 26, 22), bold);
        draw_button(c, 130, 6, 100, 22, kbd_layout_label(settings.kbd_layout), hot(130, 6, 100, 22));
        draw_button(c, w - 76, 6, 66, 22, "Open" G_ELLIP, hot(w - 76, 6, 66, 22));
        draw_button(c, w - 146, 6, 62, 22, "Save", hot(w - 146, 6, 62, 22), false, dirty || !path[0]);
        if (!f) { c.text_centered(0, h / 2, w, "the font data is missing from this build", th::red, 1, true); return; }
        if (lay_dirty || lay_w != w - 11) { lay_w = w - 11; relayout(); }
        c.set_clip({0, BAR, w - 11, h - BAR - ST});
        int top = BAR - sc.pos() + 12;
        float px = size;
        int a = 0, bsel = 0;
        if (has_sel()) sel_range(&a, &bsel);
        int cl = lines.len ? line_of(cur) : 0;
        for (int li = 0; li < lines.len; li++) {
            Line& L = lines[li];
            int ly = top + (int)L.y;
            if (ly + (int)L.h < BAR || ly > h) continue;
            float baseline = ly + vfont_ascent(px);
            for (int g = L.g0; g < L.g1; g++) {
                Glyph& G = glyphs[g];
                if (has_sel() && G.bi >= a && G.bi < bsel)
                    c.fill_alpha(MARGIN + (int)G.x, ly, (int)(G.adv + 0.99f), (int)L.h, accent(), 90);
                if (G.shown && G.shown != ' ') vfont_draw_gid(c, f, G.gid, px, MARGIN + G.x, baseline, 0xFF1E1830);
            }
            if (li == cl && !has_sel() && (ticks() / 530) % 2 == 0) {
                float cx = caret_x(li, cur);
                c.fill(MARGIN + (int)cx, ly + 2, 2, (int)L.h - 3, 0xFF1E1830);
            }
        }
        if (!buf.len && (ticks() / 530) % 2 == 0) c.fill(MARGIN, top + 2, 2, (int)vfont_line_height(px) - 3, 0xFF1E1830);
        c.set_clip({0, 0, w, h});
        sb.draw(c, w - 10, BAR, h - BAR - ST, (int)doc_h + 24, h - BAR - ST, sc.pos());
        // status bar
        c.fill(0, h - ST, w, ST, th::panel);
        c.hline(0, h - ST, w, th::border);
        int ln = 1, col = 1;
        for (int i = 0; i < cur && i < buf.len; i++) { if (buf[i] == '\n') { ln++; col = 1; } else if (((u8)buf[i] & 0xC0) != 0x80) col++; }
        snprintf(b, sizeof(b), "ln %d, col %d", ln, col);
        c.text(10, h - ST + 4, b, th::muted);
        char t[64];
        snprintf(t, sizeof(t), "%d characters  " G_BULLET "  UTF-8  " G_BULLET "  %s", count_chars(), kbd_layout_label(settings.kbd_layout));
        c.text(w - text_width(t) - 14, h - ST + 4, t, th::muted);
    }
    int count_chars() { int n = 0; for (int i = 0; i < buf.len; i++) if (((u8)buf[i] & 0xC0) != 0x80) n++; return n; }

    void ensure_visible() {
        if (lay_dirty) relayout();
        int li = line_of(cur);
        if (li < 0 || li >= lines.len) return;
        int y = 12 + (int)lines[li].y, hgt = (int)lines[li].h;
        int t = sc.target;
        if (y < t) t = y - 8;
        if (y + hgt > t + view_h()) t = y + hgt - view_h() + 8;
        sc.set(clamp(t, 0, max(0, (int)doc_h + 24 - view_h())));
    }

    // ------------------------------------------------------------ input
    void cycle_layout(int d) {
        settings.kbd_layout = (settings.kbd_layout + d + kbd_layout_count()) % kbd_layout_count();
        settings_save();
        ui_notify("Keyboard", kbd_layout_label(settings.kbd_layout), "write");
    }
    void set_size(float s) { size = clamp(s, 10.0f, 72.0f); lay_dirty = true; ui_invalidate(); }
    void on_mouse(const MouseEv& e) override {
        int off = sc.target;
        if (sb.handle(e, cw - 10, BAR, view_h(), (int)doc_h + 24, view_h(), off)) { sc.jump(off); ui_invalidate(); return; }
        if (e.kind == M_WHEEL) { sc.set(clamp(sc.target - e.wheel * 60, 0, max(0, (int)doc_h + 24 - view_h()))); ui_invalidate(); return; }
        if (e.y < BAR) {
            if (e.kind != M_UP || e.button != 1) return;
            if (Rect{8, 6, 26, 22}.contains(e.x, e.y)) set_size(size - 2);
            else if (Rect{66, 6, 26, 22}.contains(e.x, e.y)) set_size(size + 2);
            else if (Rect{98, 6, 26, 22}.contains(e.x, e.y)) { bold = !bold; lay_dirty = true; }
            else if (Rect{130, 6, 100, 22}.contains(e.x, e.y)) cycle_layout(1);
            else if (Rect{cw - 76, 6, 66, 22}.contains(e.x, e.y)) open_dialog();
            else if (Rect{cw - 146, 6, 62, 22}.contains(e.x, e.y)) save();
            ui_invalidate();
            return;
        }
        if (e.y >= ch - ST) return;
        if (lay_dirty) relayout();
        auto hit = [&]() {
            float y = (float)(e.y - BAR + sc.pos() - 12);
            int li = lines.len - 1;
            for (int i = 0; i < lines.len; i++) if (y < lines[i].y + lines[i].h) { li = i; break; }
            return byte_at(max(0, li), (float)(e.x - MARGIN));
        };
        if (!lines.len) return;
        if (e.kind == M_DOWN && e.button == 1) {
            cur = hit();
            anc = kbd_shift_down() ? (anc >= 0 ? anc : cur) : cur;
            dragging = true;
            want_x = -1;
        } else if (e.kind == M_MOVE && dragging && e.left) {
            cur = hit();
        } else if (e.kind == M_UP) {
            dragging = false;
            if (anc == cur) anc = -1;
        } else if (e.kind == M_DBL) {
            cur = hit();
            int a = cur, b = cur;
            while (a > 0 && !is_space(a - 1)) a = prev_char(a);
            while (b < buf.len && !is_space(b)) b += char_len_at(b);
            anc = a; cur = b;
        }
        ui_invalidate();
    }
    bool is_space(int i) { u8 c = (u8)buf[i]; return c == ' ' || c == '\n' || c == '\t'; }
    void open_dialog() {
        pick_file("Open document", "/home", "txt,md,text", false, "", [](App* a, const char* p) { a->open_arg(p); });
    }

    void move_to(int pos, bool extend) {
        if (extend) { if (anc < 0) anc = cur; }
        else anc = -1;
        cur = clamp(pos, 0, buf.len);
        ensure_visible();
        ui_invalidate();
    }
    void vertical(int dir, bool extend) {
        if (lay_dirty) relayout();
        int li = line_of(cur);
        if (want_x < 0) want_x = caret_x(li, cur);
        float wx = want_x;
        int nl = li + dir;
        if (nl < 0) { move_to(0, extend); want_x = wx; return; }
        if (nl >= lines.len) { move_to(buf.len, extend); want_x = wx; return; }
        move_to(byte_at(nl, wx), extend);
        want_x = wx;
    }
    bool rtl_at(int pos) {
        if (lay_dirty) relayout();
        int li = line_of(pos);
        return li < lines.len && lines[li].rtl;
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.ctrl && !e.alt) {
            switch (e.key) {
            case 'a': anc = 0; cur = buf.len; ui_invalidate(); return;
            case 'c': copy_sel(false); return;
            case 'x': copy_sel(true); return;
            case 'v': paste(); ensure_visible(); return;
            case 's': save(); return;
            case 'o': open_dialog(); return;
            case 'n': buf.clear(); path[0] = 0; cur = 0; anc = -1; dirty = false; lay_dirty = true; set_title(title()); ui_invalidate(); return;
            case 'b': bold = !bold; lay_dirty = true; ui_invalidate(); return;
            case '+': case '=': set_size(size + 2); return;
            case '-': set_size(size - 2); return;
            case K_HOME: move_to(0, e.shift); return;
            case K_END: move_to(buf.len, e.shift); return;
            case 'k': cycle_layout(e.shift ? -1 : 1); return;
            }
            return;
        }
        switch (e.key) {
        case K_LEFT: case K_RIGHT: {
            bool right = e.key == K_RIGHT;
            if (has_sel() && !e.shift) { int a, b; sel_range(&a, &b); move_to(right ? b : a, false); return; }
            bool flip = rtl_at(cur);
            bool fwd = right != flip;
            move_to(fwd ? cur + char_len_at(cur) : prev_char(cur), e.shift);
            return;
        }
        case K_UP: vertical(-1, e.shift); return;
        case K_DOWN: vertical(1, e.shift); return;
        case K_PGUP: for (int i = 0; i < max(1, (int)(view_h() / vfont_line_height(size)) - 1); i++) vertical(-1, e.shift); return;
        case K_PGDN: for (int i = 0; i < max(1, (int)(view_h() / vfont_line_height(size)) - 1); i++) vertical(1, e.shift); return;
        case K_HOME: case K_END: {
            if (lay_dirty) relayout();
            int li = line_of(cur);
            move_to(e.key == K_HOME ? lines[li].a : (lines[li].b > lines[li].a && lines[li].b <= buf.len && buf.len > 0 && lines[li].b < buf.len && buf[lines[li].b - 1] == ' ' ? lines[li].b - 1 : lines[li].b), e.shift);
            return;
        }
        case K_BACKSPACE:
            if (has_sel()) delete_selection();
            else if (cur > 0) { int p = prev_char(cur); for (int i = cur - 1; i >= p; i--) buf.remove(i); cur = p; touch(); }
            ensure_visible();
            return;
        case K_DELETE:
            if (has_sel()) delete_selection();
            else if (cur < buf.len) { int n = char_len_at(cur); for (int i = 0; i < n; i++) buf.remove(cur); touch(); }
            return;
        case K_ENTER: insert("\n", 1); ensure_visible(); return;
        case K_TAB: insert("    ", 4); return;
        case K_ESC: anc = -1; ui_invalidate(); return;
        }
        if (e.cp >= 32 && e.cp != 127 && !e.alt) { insert_cp(e.cp); ensure_visible(); }
    }

    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "File", 24);
        m[0].add("New", 1, "ctrl+n").add("Open" G_ELLIP, 2, "ctrl+o").add("Save", 3, "ctrl+s");
        m[1].clear(); strlcpy(m[1].title, "Edit", 24);
        m[1].add("Cut", 4, "ctrl+x", has_sel()).add("Copy", 5, "ctrl+c", has_sel()).add("Paste", 6, "ctrl+v", clip_has()).sep().add("Select all", 7, "ctrl+a");
        m[2].clear(); strlcpy(m[2].title, "Format", 24);
        m[2].add("Bigger", 8, "ctrl++").add("Smaller", 9, "ctrl+-").add("Bold", 10, "ctrl+b", true, bold);
        m[3].clear(); strlcpy(m[3].title, "Keyboard", 24);
        for (int i = 0; i < kbd_layout_count(); i++) m[3].add(kbd_layout_label(i), 20 + i, i == 0 ? "ctrl+k" : "", true, settings.kbd_layout == i);
        return 4;
    }
    void on_command(int cmd) override {
        switch (cmd) {
        case 1: buf.clear(); path[0] = 0; cur = 0; anc = -1; dirty = false; lay_dirty = true; set_title(title()); break;
        case 2: open_dialog(); break;
        case 3: save(); break;
        case 4: copy_sel(true); break;
        case 5: copy_sel(false); break;
        case 6: paste(); break;
        case 7: anc = 0; cur = buf.len; break;
        case 8: set_size(size + 2); break;
        case 9: set_size(size - 2); break;
        case 10: bold = !bold; lay_dirty = true; break;
        default:
            if (cmd >= 20 && cmd < 20 + kbd_layout_count()) { settings.kbd_layout = cmd - 20; settings_save(); }
        }
        ui_invalidate();
    }
};

App* new_write(const char* arg) { return new WriteApp(arg); }
