// silicon - pixel widgets, dialogs and small helpers
#include "wm.h"

// ---------------------------------------------------------------- formatting
void fmt_bytes(u64 b, char* out, int n) {
    if (b < 1024) snprintf(out, n, "%u B", (u32)b);
    else if (b < 1024 * 1024) snprintf(out, n, "%u.%u KB", (u32)(b >> 10), (u32)((b % 1024) * 10 / 1024));
    else snprintf(out, n, "%u.%u MB", (u32)(b >> 20), (u32)(((b >> 10) % 1024) * 10 / 1024));
}

void fmt_double(double v, char* out, int n, int max_dec) {
    if (v != v) { strlcpy(out, "error", n); return; }
    bool neg = v < 0;
    if (neg) v = -v;
    if (v >= 1e15) { strlcpy(out, neg ? "-overflow" : "overflow", n); return; }
    u64 ip = (u64)v;
    double frac = v - (double)ip;
    u64 scale = 1;
    for (int i = 0; i < max_dec; i++) scale *= 10;
    u64 fp = (u64)(frac * scale + 0.5);
    if (fp >= scale) { ip++; fp -= scale; }
    char fs[24];
    int fl = 0;
    if (fp) {
        snprintf(fs, sizeof(fs), "%0*lu", max_dec, fp);
        fl = (int)strlen(fs);
        while (fl > 0 && fs[fl - 1] == '0') fs[--fl] = 0;
    }
    if (fl) snprintf(out, n, "%s%lu.%s", neg ? "-" : "", ip, fs);
    else snprintf(out, n, "%s%lu", neg && ip ? "-" : "", ip);
}

// ---------------------------------------------------------------- primitives
void draw_bevel(Canvas& c, int x, int y, int w, int h, u32 face, bool sunken) {
    c.fill(x + 1, y + 1, w - 2, h - 2, face);
    c.hline(x + 1, y, w - 2, th::ink);
    c.hline(x + 1, y + h - 1, w - 2, th::ink);
    c.vline(x, y + 1, h - 2, th::ink);
    c.vline(x + w - 1, y + 1, h - 2, th::ink);
    u32 hi = sunken ? shade(face, 80) : blend(face, th::white, 140);
    u32 lo = sunken ? blend(face, th::white, 120) : shade(face, 78);
    c.hline(x + 1, y + 1, w - 2, hi);
    c.vline(x + 1, y + 1, h - 2, hi);
    c.hline(x + 1, y + h - 2, w - 2, lo);
    c.vline(x + w - 2, y + 1, h - 2, lo);
}

void draw_button(Canvas& c, int x, int y, int w, int h, const char* label, bool hot, bool primary, bool enabled) {
    u32 face = primary ? accent() : th::panel;
    if (hot && enabled) face = primary ? blend(face, th::white, 50) : blend(face, accent(), 50);
    bool pressed = hot && ui_mouse_left && enabled;
    draw_bevel(c, x, y, w, h, face, pressed);
    u32 col = !enabled ? th::shadow : primary ? th::white : th::text;
    int off = pressed ? 1 : 0;
    c.text_centered(x + off, y + (h - FONT_H) / 2 + off, w, label, col, 1, primary);
}

void draw_checkbox(Canvas& c, int x, int y, const char* label, bool on) {
    draw_bevel(c, x, y, 14, 14, th::white, true);
    if (on) {
        c.fill(x + 3, y + 3, 8, 8, accent());
        c.hline(x + 3, y + 3, 8, blend(accent(), th::white, 120));
    }
    c.text(x + 20, y + 1, label, th::text);
}

void draw_scrollbar(Canvas& c, int x, int y, int h, int total, int visible, int offset) {
    c.fill(x, y, 10, h, th::panel_dk);
    c.vline(x, y, h, th::border);
    if (total <= visible) return;
    int th_h = max(16, h * visible / total);
    int ty = y + (h - th_h) * offset / max(1, total - visible);
    draw_bevel(c, x + 1, ty, 9, th_h, accent());
}

void draw_panel(Canvas& c, int x, int y, int w, int h, const char* title) {
    c.fill(x, y, w, h, th::panel);
    c.rect(x, y, w, h, th::border);
    c.hline(x + 1, y + 1, w - 2, th::white);
    if (title) {
        int tw = text_width(title) + 8;
        c.fill(x + 8, y - 6, tw, 12, th::panel);
        c.text(x + 12, y - 6, title, th::muted, 1, true);
    }
}

void draw_progress(Canvas& c, int x, int y, int w, int h, int pct, u32 color) {
    draw_bevel(c, x, y, w, h, th::white, true);
    int fw = max(pct > 0 ? 4 : 0, (w - 4) * clamp(pct, 0, 100) / 100);
    // segmented "die" fill
    for (int sx = 0; sx + 4 <= fw; sx += 6) c.fill(x + 2 + sx, y + 2, 4, h - 4, color);
}

void draw_icon_label(Canvas& c, Icon* ic, int x, int y, int cell_w, const char* label, bool selected, bool small) {
    const Image& img = small ? ic->mid : ic->big;
    int ix = x + (cell_w - img.w) / 2;
    if (selected) c.fill_alpha(ix - 3, y - 3, img.w + 6, img.h + 6, accent(), 90);
    c.blit(img, ix, y);
    char shown[24];
    strlcpy(shown, label, sizeof(shown));
    if (strlen(label) > 13) { shown[11] = 0; strlcat(shown, G_ELLIP, sizeof(shown)); }
    int tw = text_width(shown);
    int tx = x + (cell_w - tw) / 2, ty = y + img.h + 4;
    if (selected) c.fill(tx - 2, ty - 1, tw + 4, FONT_H + 2, accent());
    c.text(tx, ty, shown, selected ? th::white : th::text);
}

// ---------------------------------------------------------------- text field
void draw_marquee(Canvas& c, Rect r) {
    if (r.w < 2 || r.h < 2) return;
    u32 a = accent();
    c.fill_alpha(r.x, r.y, r.w, r.h, a, 70);
    u32 edge = blend(a, th::white, 70);
    c.fill_alpha(r.x, r.y, r.w, 1, edge, 230);
    c.fill_alpha(r.x, r.y + r.h - 1, r.w, 1, edge, 230);
    c.fill_alpha(r.x, r.y + 1, 1, r.h - 2, edge, 230);
    c.fill_alpha(r.x + r.w - 1, r.y + 1, 1, r.h - 2, edge, 230);
}

void TextField::erase_sel() {
    if (!has_sel()) return;
    int a = min(anchor, cur), b = max(anchor, cur);
    memmove(text + a, text + b, len - b + 1);
    len -= b - a;
    cur = a;
    anchor = -1;
}

void TextField::insert(const char* s, int n) {
    erase_sel();
    anchor = -1;
    for (int i = 0; i < n && len < (int)sizeof(text) - 1; i++) {
        char ch = s[i];
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        if ((u8)ch < 32) continue;
        memmove(text + cur + 1, text + cur, len - cur + 1);
        text[cur++] = ch;
        len++;
    }
}

void TextField::copy_sel() {
    if (!has_sel()) return;
    int a = min(anchor, cur), b = max(anchor, cur);
    char u[300];
    silicon_to_utf8(text + a, b - a, u, sizeof(u));
    clip_set(u);
}

static int word_left(const char* t, int i) {
    while (i > 0 && t[i - 1] == ' ') i--;
    while (i > 0 && t[i - 1] != ' ') i--;
    return i;
}
static int word_right(const char* t, int len, int i) {
    while (i < len && t[i] != ' ') i++;
    while (i < len && t[i] == ' ') i++;
    return i;
}

bool TextField::on_key(const KeyEvent& e) {
    if (!e.down) return false;
    auto move = [&](int to) {
        if (e.shift) { if (anchor < 0) anchor = cur; }
        else anchor = -1;
        cur = clamp(to, 0, len);
    };
    switch (e.key) {
    case K_LEFT:
        if (!e.shift && has_sel() && !e.ctrl) { cur = min(anchor, cur); anchor = -1; }
        else move(e.ctrl ? word_left(text, cur) : cur - 1);
        return true;
    case K_RIGHT:
        if (!e.shift && has_sel() && !e.ctrl) { cur = max(anchor, cur); anchor = -1; }
        else move(e.ctrl ? word_right(text, len, cur) : cur + 1);
        return true;
    case K_HOME: move(0); return true;
    case K_END: move(len); return true;
    case K_BACKSPACE:
        anchor = has_sel() ? anchor : -1;
        if (has_sel()) { erase_sel(); return true; }
        if (e.ctrl) { int t = word_left(text, cur); memmove(text + t, text + cur, len - cur + 1); len -= cur - t; cur = t; return true; }
        if (cur > 0) { memmove(text + cur - 1, text + cur, len - cur + 1); cur--; len--; }
        return true;
    case K_DELETE:
        anchor = has_sel() ? anchor : -1;
        if (has_sel()) { erase_sel(); return true; }
        if (cur < len) { memmove(text + cur, text + cur + 1, len - cur); len--; }
        return true;
    }
    if (e.ctrl && !e.alt) {
        switch (e.key) {
        case 'a': select_all(); return true;
        case 'c': copy_sel(); return true;
        case 'x': copy_sel(); erase_sel(); return true;
        case 'v': {
            if (!clip_has()) return true;
            char tmp[300];
            utf8_to_silicon(clip_get(), min(clip_len(), 290u), tmp, sizeof(tmp));
            insert(tmp, (int)strlen(tmp));
            return true;
        }
        case 'u': text[0] = 0; len = cur = 0; anchor = -1; return true;
        }
        return false;
    }
    if (e.ch >= 32 && e.ch < 127 && !e.alt) {
        if (has_sel()) erase_sel();
        anchor = -1;
        if (len < (int)sizeof(text) - 1) {
            memmove(text + cur + 1, text + cur, len - cur + 1);
            text[cur++] = e.ch;
            len++;
        }
        return true;
    }
    return false;
}

void TextField::click(int rel_x, int w, bool shift) {
    int i = clamp(scroll + (rel_x - 5 + FONT_W / 2) / FONT_W, 0, len);
    if (shift) { if (anchor < 0) anchor = cur; }
    else anchor = i;                        // a click may start a drag selection
    cur = i;
}

void TextField::drag(int rel_x, int w) {
    cur = clamp(scroll + (rel_x - 5 + FONT_W / 2) / FONT_W, 0, len);
}

void TextField::draw(Canvas& c, int x, int y, int w) {
    draw_bevel(c, x, y, w, 20, th::white, true);
    if (focused) c.rect(x - 1, y - 1, w + 2, 22, accent());
    int max_chars = max(1, (w - 10) / FONT_W);
    if (cur < scroll) scroll = cur;
    if (cur > scroll + max_chars - 1) scroll = cur - max_chars + 1;
    scroll = clamp(scroll, 0, max(0, len));
    int start = scroll;
    if (has_sel()) {
        int a = max(min(anchor, cur), start), b = min(max(anchor, cur), start + max_chars);
        if (b > a) c.fill(x + 5 + (a - start) * FONT_W, y + 3, (b - a) * FONT_W, 14, blend(th::white, accent(), 130));
    }
    if (password) {
        for (int i = start; i < len && i < start + max_chars; i++) c.fill(x + 5 + (i - start) * FONT_W + 2, y + 7, 4, 4, th::text);
    } else c.text_n(x + 5, y + 4, text + start, max_chars, th::text);
    if (focused && (ticks() / 500) % 2 == 0) c.vline(x + 5 + (cur - start) * FONT_W, y + 3, 14, accent());
}

// ---------------------------------------------------------------- dialogs
struct Dialog {
    int kind;                 // 0 ask, 1 prompt, 2 info, 3 file picker
    char msg[200];
    char btn[3][16];
    int nb;
    TextField field;
    void (*cb_ask)(App*, int);
    void (*cb_text)(App*, const char*);
    // file picker
    char dir[256];
    char exts[64];
    bool save;
    int sel, scroll;
    u64 click_t;
    int click_i;
    Dialog() { kind = 0; nb = 0; sel = -1; scroll = 0; click_t = 0; click_i = -1; save = false; dir[0] = 0; exts[0] = 0; }
};

// ---- file picker helpers
static bool pick_ext_ok(Dialog* d, const char* name) {
    if (!d->exts[0]) return true;
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    dot++;
    const char* p = d->exts;
    while (*p) {
        const char* e = strchr(p, ',');
        int n = e ? (int)(e - p) : (int)strlen(p);
        if ((int)strlen(dot) == n && str_ieq_prefix(dot, p) && !strncmp(dot, p, 0 + n)) return true;
        bool same = (int)strlen(dot) == n;
        for (int i = 0; same && i < n; i++) if (to_lower(dot[i]) != to_lower(p[i])) same = false;
        if (same) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

bool is_image_name(const char* name);
struct PickRow { FsNode* n; bool up; };
static int pick_rows(Dialog* d, PickRow* out, int cap) {
    FsNode* dir = fs_lookup(d->dir);
    if (!dir || !dir->dir) { dir = fs_home(); fs_path(dir, d->dir, sizeof(d->dir)); }
    fs_sort(dir);
    int n = 0;
    if (dir->parent && n < cap) out[n++] = {dir->parent, true};
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < dir->children.len && n < cap; i++) {
            FsNode* c = dir->children[i];
            if (c->dir != (pass == 0)) continue;
            if (!c->dir && !pick_ext_ok(d, c->name)) continue;
            out[n++] = {c, false};
        }
    return n;
}

static const int PICK_W = 500, PICK_H = 350, PICK_ROW = 20;
static Rect pick_rect(int w, int h) {
    int dw = min(PICK_W, w - 16), dh = min(PICK_H, h - 16);
    return {(w - dw) / 2, (h - dh) / 2, dw, dh};
}
static Rect pick_list(Rect r) { return {r.x + 12, r.y + 62, r.w - 24, r.h - 62 - 74}; }

void App::ask(const char* msg, const char* b1, const char* b2, const char* b3, void (*cb)(App*, int)) {
    if (dialog) delete dialog;
    dialog = new Dialog();
    dialog->kind = 0;
    strlcpy(dialog->msg, msg, sizeof(dialog->msg));
    const char* bs[3] = {b1, b2, b3};
    dialog->nb = 0;
    for (int i = 0; i < 3; i++) if (bs[i]) strlcpy(dialog->btn[dialog->nb++], bs[i], 16);
    dialog->cb_ask = cb;
    ui_invalidate();
}

void App::prompt(const char* msg, const char* initial, void (*cb)(App*, const char*)) {
    if (dialog) delete dialog;
    dialog = new Dialog();
    dialog->kind = 1;
    strlcpy(dialog->msg, msg, sizeof(dialog->msg));
    dialog->field.set(initial ? initial : "");
    dialog->field.focused = true;
    strlcpy(dialog->btn[0], "OK", 16);
    strlcpy(dialog->btn[1], "Cancel", 16);
    dialog->nb = 2;
    dialog->cb_text = cb;
    ui_invalidate();
}

void App::pick_file(const char* title, const char* dir, const char* exts, bool save, const char* default_name,
                    void (*cb)(App*, const char* path)) {
    if (dialog) delete dialog;
    dialog = new Dialog();
    dialog->kind = 3;
    strlcpy(dialog->msg, title, sizeof(dialog->msg));
    FsNode* d = fs_lookup(dir && dir[0] ? dir : "/home");
    if (!d || !d->dir) d = fs_home();
    fs_path(d, dialog->dir, sizeof(dialog->dir));
    strlcpy(dialog->exts, exts ? exts : "", sizeof(dialog->exts));
    dialog->save = save;
    dialog->field.set(default_name ? default_name : "");
    dialog->field.focused = true;
    strlcpy(dialog->btn[0], save ? "Save" : "Open", 16);
    strlcpy(dialog->btn[1], "Cancel", 16);
    dialog->nb = 2;
    dialog->cb_text = cb;
    ui_invalidate();
}

void App::info(const char* msg) {
    if (dialog) delete dialog;
    dialog = new Dialog();
    dialog->kind = 2;
    strlcpy(dialog->msg, msg, sizeof(dialog->msg));
    strlcpy(dialog->btn[0], "OK", 16);
    dialog->nb = 1;
    ui_invalidate();
}

static Rect dialog_rect(Dialog* d, int w, int h) {
    if (d->kind == 3) return pick_rect(w, h);
    int dw = min(340, w - 16), dh = d->kind == 1 ? 120 : 100;
    return {(w - dw) / 2, max(8, (h - dh) / 2), dw, dh};
}

static Rect dialog_button(Dialog* d, Rect r, int i) {
    int gap = 8, x = r.x + r.w - 12;
    for (int k = d->nb - 1; k >= i; k--) {
        int bw = max(76, text_width(d->btn[k], 1, true) + 20);
        x -= bw;
        if (k == i) return {x, r.y + r.h - 32, bw, 22};
        x -= gap;
    }
    return {x, r.y + r.h - 32, 76, 22};
}

static void pick_draw(App* a, Dialog* d, Canvas& c, Rect r, int cw_, int ch_) {
    c.text(r.x + 14, r.y + 12, d->msg, th::text, 1, true);
    // path bar
    draw_bevel(c, r.x + 12, r.y + 30, r.w - 24, 22, th::white, true);
    c.blit(icon("folder")->small, r.x + 18, r.y + 33);
    c.text_n(r.x + 40, r.y + 35, d->dir, (r.w - 70) / 8, th::text);
    Rect lr = pick_list(r);
    draw_bevel(c, lr.x, lr.y, lr.w, lr.h, th::white, true);
    PickRow rows[300];
    int n = pick_rows(d, rows, 300);
    int vis = (lr.h - 4) / PICK_ROW;
    d->scroll = clamp(d->scroll, 0, max(0, n - vis));
    c.set_clip({lr.x + 2, lr.y + 2, lr.w - 4, lr.h - 4});
    for (int i = 0; i < vis && d->scroll + i < n; i++) {
        int k = d->scroll + i;
        int y = lr.y + 2 + i * PICK_ROW;
        bool on = k == d->sel;
        bool hv = a->hot(lr.x + 2, y, lr.w - 14, PICK_ROW);
        if (on) c.fill(lr.x + 2, y, lr.w - 14, PICK_ROW, blend(th::white, accent(), 150));
        else if (hv) c.fill(lr.x + 2, y, lr.w - 14, PICK_ROW, blend(th::white, accent(), 50));
        FsNode* nd = rows[k].n;
        const char* ic = rows[k].up || nd->dir ? "folder" : (is_image_name(nd->name) ? "image_file" : "text_file");
        c.blit(icon(ic)->small, lr.x + 8, y + 2);
        char nm[80];
        if (rows[k].up) strlcpy(nm, G_UP " ..", sizeof(nm));
        else snprintf(nm, sizeof(nm), "%s%s", nd->name, nd->dir ? "/" : "");
        c.text_n(lr.x + 30, y + 4, nm, (lr.w - 140) / 8, on ? th::ink : th::text, 1, nd->dir || rows[k].up);
        if (!nd->dir && !rows[k].up) {
            char sz[24];
            fmt_bytes(nd->size, sz, sizeof(sz));
            c.text(lr.x + lr.w - 16 - text_width(sz) - 4, y + 4, sz, th::muted);
        }
    }
    c.set_clip({0, 0, cw_, ch_});
    if (n > vis) {
        c.fill(lr.x + lr.w - 10, lr.y + 2, 8, lr.h - 4, th::panel_dk);
        int th_h = max(16, (lr.h - 4) * vis / n);
        int ty = lr.y + 2 + (lr.h - 4 - th_h) * d->scroll / max(1, n - vis);
        c.fill(lr.x + lr.w - 10, ty, 8, th_h, accent());
    }
    c.text(r.x + 14, r.y + r.h - 62, d->save ? "name" : "file", th::muted);
    d->field.draw(c, r.x + 56, r.y + r.h - 66, r.w - 70);
}

void dialog_draw(App* a, Canvas& c, int w, int h) {
    Dialog* d = a->dialog;
    c.dither(0, 0, w, h, 0xFF2A2240);
    Rect r = dialog_rect(d, w, h);
    if (d->kind == 3) {
        c.dither(r.x + 4, r.y + 4, r.w, r.h, th::ink);
        draw_bevel(c, r.x, r.y, r.w, r.h, th::paper);
        c.fill(r.x + 2, r.y + 2, r.w - 4, 3, accent());
        pick_draw(a, d, c, r, w, h);
        for (int i = 0; i < d->nb; i++) {
            Rect b = dialog_button(d, r, i);
            draw_button(c, b.x, b.y, b.w, b.h, d->btn[i], a->hot(b.x, b.y, b.w, b.h), i == 0);
        }
        return;
    }
    c.dither(r.x + 4, r.y + 4, r.w, r.h, th::ink);
    draw_bevel(c, r.x, r.y, r.w, r.h, th::paper);
    c.fill(r.x + 2, r.y + 2, r.w - 4, 3, accent());
    c.blit(icon(d->kind == 2 ? "about" : "silicon")->mid, r.x + 12, r.y + 14);
    // word-wrap the message
    int tx = r.x + 52, maxc = (r.w - 64) / FONT_W, ty = r.y + 16;
    const char* m = d->msg;
    while (*m && ty < r.y + r.h - 40) {
        int n = (int)strlen(m);
        if (n > maxc) { n = maxc; while (n > 0 && m[n] != ' ') n--; if (!n) n = maxc; }
        c.text_n(tx, ty, m, n, th::text);
        m += n;
        while (*m == ' ') m++;
        ty += 14;
    }
    if (d->kind == 1) d->field.draw(c, r.x + 52, r.y + 52, r.w - 64);
    for (int i = 0; i < d->nb; i++) {
        Rect b = dialog_button(d, r, i);
        draw_button(c, b.x, b.y, b.w, b.h, d->btn[i], a->hot(b.x, b.y, b.w, b.h), i == 0);
    }
}

static void pick_activate(App* a, Dialog* d, int k);

static void dialog_finish(App* a, int choice) {
    Dialog* d = a->dialog;
    if (d->kind == 3 && choice == 0) {
        // Open/Save: a typed or selected name inside the current folder
        if (!d->field.len) return;
        char p[300];
        if (d->field.text[0] == '/') strlcpy(p, d->field.text, sizeof(p));
        else snprintf(p, sizeof(p), "%s%s%s", d->dir, strcmp(d->dir, "/") ? "/" : "", d->field.text);
        FsNode* n = fs_lookup(p);
        if (n && n->dir) { strlcpy(d->dir, p, sizeof(d->dir)); d->field.set(""); d->sel = -1; d->scroll = 0; ui_invalidate(); return; }
        if (!d->save && !n) return;
        a->dialog = nullptr;
        if (d->cb_text) d->cb_text(a, p);
        delete d;
        ui_invalidate();
        return;
    }
    a->dialog = nullptr;
    if (d->kind == 0 && d->cb_ask) d->cb_ask(a, choice);
    if (d->kind == 1 && d->cb_text && choice == 0) d->cb_text(a, d->field.text);
    delete d;
    ui_invalidate();
}

static void pick_activate(App* a, Dialog* d, int k) {
    PickRow rows[300];
    int n = pick_rows(d, rows, 300);
    if (k < 0 || k >= n) return;
    FsNode* nd = rows[k].n;
    if (rows[k].up || nd->dir) {
        fs_path(nd, d->dir, sizeof(d->dir));
        d->sel = -1;
        d->scroll = 0;
        d->field.set("");
        ui_invalidate();
        return;
    }
    d->field.set(nd->name);
    dialog_finish(a, 0);
}

void dialog_mouse(App* a, const MouseEv& e, int w, int h) {
    Dialog* d = a->dialog;
    if (d->kind == 3) {
        Rect r = dialog_rect(d, w, h);
        Rect lr = pick_list(r);
        PickRow rows[300];
        int n = pick_rows(d, rows, 300);
        int vis = (lr.h - 4) / PICK_ROW;
        if (e.kind == M_WHEEL) { d->scroll = clamp(d->scroll - e.wheel * 3, 0, max(0, n - vis)); ui_invalidate(); return; }
        if (e.kind == M_DOWN && e.button == 1 && lr.contains(e.x, e.y)) {
            int k = d->scroll + (e.y - lr.y - 2) / PICK_ROW;
            if (k >= 0 && k < n) {
                bool dbl = d->click_i == k && ticks() - d->click_t < 450;
                d->sel = k;
                d->click_i = k;
                d->click_t = ticks();
                if (!rows[k].up && !rows[k].n->dir) d->field.set(rows[k].n->name);
                if (dbl) { pick_activate(a, d, k); return; }
                ui_invalidate();
            }
            return;
        }
    }
    if (e.kind != M_UP || e.button != 1) return;
    Rect r = dialog_rect(d, w, h);
    for (int i = 0; i < d->nb; i++)
        if (dialog_button(d, r, i).contains(e.x, e.y)) { dialog_finish(a, i); return; }
}

void dialog_key(App* a, const KeyEvent& e) {
    if (!e.down) return;
    Dialog* d = a->dialog;
    if (d->kind == 3) {
        PickRow rows[300];
        int n = pick_rows(d, rows, 300);
        if (e.key == K_UP) { d->sel = max(0, (d->sel < 0 ? n : d->sel) - 1); }
        else if (e.key == K_DOWN) { d->sel = min(n - 1, d->sel + 1); }
        else if (e.key == K_BACKSPACE && !d->field.len) { if (n && rows[0].up) pick_activate(a, d, 0); return; }
        else if (e.key == K_ENTER) {
            if (d->field.len) dialog_finish(a, 0);
            else if (d->sel >= 0) pick_activate(a, d, d->sel);
            return;
        } else if (e.key == K_ESC) { dialog_finish(a, d->nb - 1); return; }
        else d->field.on_key(e);
        if (d->sel >= 0 && d->sel < n && (e.key == K_UP || e.key == K_DOWN) && !rows[d->sel].up && !rows[d->sel].n->dir)
            d->field.set(rows[d->sel].n->name);
        int vis = (pick_list(pick_rect(a->win ? a->win->client().w : 400, a->win ? a->win->client().h : 300)).h - 4) / PICK_ROW;
        if (d->sel >= 0) d->scroll = clamp(d->scroll, max(0, d->sel - vis + 1), d->sel);
        ui_invalidate();
        return;
    }
    if (e.key == K_ENTER) { dialog_finish(a, 0); return; }
    if (e.key == K_ESC) { dialog_finish(a, d->nb - 1); return; }
    if (d->kind == 1) d->field.on_key(e);
    ui_invalidate();
}

// ---------------------------------------------------------------- menus
Menu& Menu::add(const char* label, int cmd, const char* shortcut, bool enabled, bool checked) {
    if (count >= 20) return *this;
    MenuItem& it = items[count++];
    strlcpy(it.label, label, sizeof(it.label));
    strlcpy(it.shortcut, shortcut, sizeof(it.shortcut));
    it.cmd = cmd;
    it.enabled = enabled;
    it.separator = false;
    it.checked = checked;
    return *this;
}

Menu& Menu::sep() {
    if (count >= 20) return *this;
    MenuItem& it = items[count++];
    memset(&it, 0, sizeof(it));
    it.separator = true;
    return *this;
}

// ---------------------------------------------------------------- app helpers
bool App::hot(int x, int y, int w, int h) { return hover_in(win, x, y, w, h); }

void App::set_title(const char* t) {
    if (win) strlcpy(win->title, t, sizeof(win->title));
    ui_invalidate();
}

void App::close() {
    if (!win) return;
    if (!can_close()) return;
    wm_close(win);
}

void App::force_close() {
    if (win) wm_close(win);
}

// ---------------------------------------------------------------- draggable scrollbar
static int thumb_h(int h, int total, int visible) { return max(18, h * visible / max(1, total)); }

void ScrollBar::draw(Canvas& c, int x, int y, int h, int total, int visible, int offset) {
    c.fill(x, y, 10, h, th::panel_dk);
    c.vline(x, y, h, th::border);
    if (total <= visible) return;
    int th_h = thumb_h(h, total, visible);
    int ty = y + (h - th_h) * clamp(offset, 0, total - visible) / max(1, total - visible);
    draw_bevel(c, x + 1, ty, 9, th_h, dragging ? blend(accent(), th::white, 60) : accent());
    for (int k = -2; k <= 2; k += 2) c.hline(x + 3, ty + th_h / 2 + k, 5, blend(accent(), th::ink, 90));
}

bool ScrollBar::handle(const MouseEv& e, int x, int y, int h, int total, int visible, int& offset) {
    if (total <= visible) { dragging = false; return false; }
    int th_h = thumb_h(h, total, visible);
    int range = max(1, total - visible), track = max(1, h - th_h);
    int ty = y + track * clamp(offset, 0, range) / range;
    if (e.kind == M_DOWN || e.kind == M_DBL) {
        if (e.x < x || e.x >= x + 11 || e.y < y || e.y >= y + h) return false;
        if (e.y >= ty && e.y < ty + th_h) grab = e.y - ty;
        else grab = th_h / 2;
        dragging = true;
    } else if (e.kind == M_UP) {
        if (!dragging) return false;
        dragging = false;
        return true;
    } else if (e.kind != M_MOVE || !dragging) {
        return false;
    }
    offset = clamp((e.y - grab - y) * range / track, 0, range);
    ui_invalidate();
    return true;
}
