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
bool TextField::on_key(const KeyEvent& e) {
    if (!e.down) return false;
    switch (e.key) {
    case K_LEFT: if (cur > 0) cur--; return true;
    case K_RIGHT: if (cur < len) cur++; return true;
    case K_HOME: cur = 0; return true;
    case K_END: cur = len; return true;
    case K_BACKSPACE:
        if (cur > 0) { memmove(text + cur - 1, text + cur, len - cur + 1); cur--; len--; }
        return true;
    case K_DELETE:
        if (cur < len) { memmove(text + cur, text + cur + 1, len - cur); len--; }
        return true;
    }
    if (e.ctrl && e.key == 'u') { text[0] = 0; len = cur = 0; return true; }
    if (e.ch >= 32 && e.ch < 127 && !e.ctrl && len < (int)sizeof(text) - 1) {
        memmove(text + cur + 1, text + cur, len - cur + 1);
        text[cur++] = e.ch;
        len++;
        return true;
    }
    return false;
}

void TextField::draw(Canvas& c, int x, int y, int w) {
    draw_bevel(c, x, y, w, 20, th::white, true);
    if (focused) c.rect(x - 1, y - 1, w + 2, 22, accent());
    int max_chars = (w - 10) / FONT_W;
    int start = max(0, cur - max_chars + 1);
    c.text_n(x + 5, y + 4, text + start, max_chars, th::text);
    if (focused && (ticks() / 500) % 2 == 0) c.vline(x + 5 + (cur - start) * FONT_W, y + 3, 14, accent());
}

// ---------------------------------------------------------------- dialogs
struct Dialog {
    int kind;                 // 0 ask, 1 prompt, 2 info
    char msg[200];
    char btn[3][16];
    int nb;
    TextField field;
    void (*cb_ask)(App*, int);
    void (*cb_text)(App*, const char*);
};

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

void dialog_draw(App* a, Canvas& c, int w, int h) {
    Dialog* d = a->dialog;
    c.dither(0, 0, w, h, 0xFF2A2240);
    Rect r = dialog_rect(d, w, h);
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

static void dialog_finish(App* a, int choice) {
    Dialog* d = a->dialog;
    a->dialog = nullptr;
    if (d->kind == 0 && d->cb_ask) d->cb_ask(a, choice);
    if (d->kind == 1 && d->cb_text && choice == 0) d->cb_text(a, d->field.text);
    delete d;
    ui_invalidate();
}

void dialog_mouse(App* a, const MouseEv& e, int w, int h) {
    if (e.kind != M_UP || e.button != 1) return;
    Dialog* d = a->dialog;
    Rect r = dialog_rect(d, w, h);
    for (int i = 0; i < d->nb; i++)
        if (dialog_button(d, r, i).contains(e.x, e.y)) { dialog_finish(a, i); return; }
}

void dialog_key(App* a, const KeyEvent& e) {
    if (!e.down) return;
    Dialog* d = a->dialog;
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
