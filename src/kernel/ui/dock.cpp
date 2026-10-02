// silicon - the dock, drawn as a chip: a package with gold pins on all four sides, an inset
// silicon die with bond pads wired to the pins, one cell per app, and copper traces feeding a
// bus. Hovering lights the app's trace and fires a pulse out to the pins.
#include "desk.h"

DockItem dock_items[24];
int dock_count = 0;
int dock_hover = -1;
int dock_press = -1;
Rect dock_rect;
u64 dock_anim_t = 0;

static const int DOCK_H = 94;

Rect dock_area() { return {dock_rect.x - 12, dock_rect.y - 44, dock_rect.w + 24, dock_rect.h + 54}; }

static bool app_running(const char* id) { return wm_find(id) != nullptr; }

// icon size: 64 normally, smaller when lots of apps are open so the die still fits the screen
static int isz = 64;

static const Image& dock_icon_img(Icon* ic) {
    if (isz == 64) return ic->big;
    if (isz == 32) return ic->mid;
    if (!ic->dock48.px) {               // box-filtered copy, made once
        ic->dock48 = image_new(64, 64, 0);
        memcpy(ic->dock48.px, ic->big.px, 64 * 64 * 4);
        image_shrink(ic->dock48, 48, 48);
    }
    return ic->dock48;
}

void dock_layout() {
    const char* prev[24];
    int prev_n = dock_count;
    for (int i = 0; i < dock_count; i++) prev[i] = dock_items[i].app_id;
    int n = 0;
    for (int i = 0; i < app_info_count; i++)
        if (app_infos[i].in_dock) dock_items[n++].app_id = app_infos[i].id;
    // running apps that are not pinned
    for (int w = 0; w < windows.len && n < 22; w++) {
        const char* id = windows[w]->app->app_id();
        bool found = !strcmp(id, "trash");
        for (int i = 0; i < n; i++) if (!strcmp(dock_items[i].app_id, id)) found = true;
        if (!found) dock_items[n++].app_id = id;
    }
    dock_items[n++].app_id = "trash";
    bool changed = n != prev_n;
    for (int i = 0; i < n && !changed; i++) changed = dock_items[i].app_id != prev[i];
    Rect old = dock_area();
    dock_count = n;
    int pad = 20, sep = 14;
    int fit = (SW - 40 - 2 * pad - sep) / max(1, dock_count);
    isz = fit >= 72 ? 64 : fit >= 56 ? 48 : 32;
    int slot = isz == 64 ? 72 : isz == 48 ? 56 : max(38, min(fit, 44));
    int total = dock_count * slot + 2 * pad + sep - 8;
    int x = (SW - total) / 2;
    int y = SH - DOCK_H - 10;
    dock_rect = {x, y, total, DOCK_H};
    int cx = x + pad;
    for (int i = 0; i < dock_count; i++) {
        if (i == dock_count - 1) cx += sep;
        dock_items[i].r = {cx, y + 10 + (64 - isz), isz, isz};
        cx += slot;
    }
    if (changed) { damage(old); damage(dock_area()); }
}

Rect dock_icon_rect(const char* id) {
    for (int i = 0; i < dock_count; i++) if (!strcmp(dock_items[i].app_id, id)) return dock_items[i].r;
    return {SW / 2 - 16, SH - 60, 32, 32};
}

bool dock_animate() {
    static u64 last = 0;
    u64 t = ticks();
    int dt = (int)min<u64>(t - last, 50);
    last = t;
    bool moving = false;
    for (int i = 0; i < dock_count; i++) {
        int target = 0;
        if (dock_hover >= 0 && settings.dock_magnify) {
            int d = iabs(i - dock_hover);
            target = (d == 0 ? 11 : d == 1 ? 5 : d == 2 ? 2 : 0) * 16;
        }
        int& l = dock_items[i].lift16;
        if (l != target) {
            // exponential approach, frame-rate independent-ish
            int step = (target - l) * min(dt, 40) / 60;
            if (step == 0) step = target > l ? 1 : -1;
            l += step;
            if (iabs(target - l) < 3) l = target;
            moving = true;
        }
        if (dock_items[i].bounce_t) moving = true;
    }
    if (moving || dock_hover >= 0) damage(dock_area());
    return moving;
}

void draw_dock(Canvas& c) {
    Rect d = dock_rect;
    if (c.clip.intersect(dock_area()).empty()) return;
    Rect die{d.x + 7, d.y + 7, d.w - 14, d.h - 14};
    u32 body = 0xFF1C1829, body_hi = 0xFF342E48;
    u32 die_c = 0xFF2A2440, die_dot = 0xFF352E50, die_hi = 0xFF443B64, die_lo = 0xFF16121F;
    u32 pin = th::gold, pin_dk = th::gold_dk, wire = 0xFF8C7246;
    u32 copper = 0xFF7A5C34, lit = accent();
    u64 t = ticks();
    int bus_y = die.y + die.h - 5;

    int pulse = -1, span = 1;
    if (dock_hover >= 0) {
        int hcx = dock_items[dock_hover].r.x + isz / 2;
        span = max(hcx - die.x, die.x + die.w - hcx);
        pulse = (int)((t - dock_anim_t) / 3 % (u64)(span + 60));
    }
    bool edge_flash = pulse >= span && pulse < span + 24;

    c.dither(d.x + 5, d.y + 7, d.w, d.h, 0xFF000000);
    for (int px = d.x + 12; px < d.x + d.w - 14; px += 10) {
        c.fill(px, d.y - 7, 5, 7, pin);
        c.hline(px, d.y - 7, 5, pin_dk);
        c.vline(px + 4, d.y - 6, 6, pin_dk);
        c.fill(px, d.y + d.h, 5, 7, pin);
        c.hline(px, d.y + d.h + 6, 5, pin_dk);
        c.vline(px + 4, d.y + d.h, 6, pin_dk);
    }
    for (int py = d.y + 14; py < d.y + d.h - 14; py += 12) {
        u32 sp = edge_flash ? blend(pin, lit, 200) : pin;
        c.fill(d.x - 8, py, 8, 5, sp);
        c.vline(d.x - 8, py, 5, pin_dk);
        c.fill(d.x + d.w, py, 8, 5, sp);
        c.vline(d.x + d.w + 7, py, 5, pin_dk);
    }
    c.fill(d.x + 1, d.y, d.w - 2, d.h, body);
    c.fill(d.x, d.y + 1, d.w, d.h - 2, body);
    c.hline(d.x + 2, d.y, d.w - 4, th::ink);
    c.hline(d.x + 2, d.y + d.h - 1, d.w - 4, th::ink);
    c.vline(d.x, d.y + 2, d.h - 4, th::ink);
    c.vline(d.x + d.w - 1, d.y + 2, d.h - 4, th::ink);
    c.put(d.x + 1, d.y + 1, th::ink); c.put(d.x + d.w - 2, d.y + 1, th::ink);
    c.put(d.x + 1, d.y + d.h - 2, th::ink); c.put(d.x + d.w - 2, d.y + d.h - 2, th::ink);
    c.hline(d.x + 2, d.y + 1, d.w - 4, body_hi);
    for (int px = d.x + 12; px < d.x + d.w - 14; px += 10) {
        c.vline(px + 2, d.y + 2, die.y - d.y - 2, wire);
        c.vline(px + 2, die.y + die.h, d.y + d.h - die.y - die.h - 2, wire);
    }
    for (int py = d.y + 14; py < d.y + d.h - 14; py += 12) {
        c.hline(d.x + 2, py + 2, die.x - d.x - 2, wire);
        c.hline(die.x + die.w, py + 2, d.x + d.w - die.x - die.w - 2, wire);
    }
    c.fill(die.x, die.y, die.w, die.h, die_c);
    for (int yy = die.y + 4; yy < die.y + die.h - 2; yy += 6)
        for (int xx = die.x + 4 + ((yy - die.y) / 6 % 2) * 3; xx < die.x + die.w - 2; xx += 6) c.put(xx, yy, die_dot);
    c.hline(die.x, die.y, die.w, die_hi);
    c.vline(die.x, die.y, die.h, die_hi);
    c.hline(die.x, die.y + die.h - 1, die.w, die_lo);
    c.vline(die.x + die.w - 1, die.y, die.h, die_lo);
    for (int px = d.x + 12; px < d.x + d.w - 14; px += 10) {
        c.fill(px + 1, die.y + 1, 3, 2, pin);
        c.fill(px + 1, die.y + die.h - 3, 3, 2, pin);
    }
    c.circle(die.x + 6, die.y + 6, 2, 0xFF7A5A8A, true);

    c.fill(die.x + 4, bus_y, die.w - 8, 2, copper);
    for (int i = 0; i < dock_count; i++) {
        Rect r = dock_items[i].r;
        int cx = r.x + isz / 2 - 1;
        c.rect(r.x - 3, die.y + 3, isz + 6, die.h - 10, 0xFF312A48);
        c.fill(cx, r.y + r.h + 2, 2, bus_y - (r.y + r.h + 2), copper);
        c.fill(cx - 1, bus_y - 1, 4, 4, copper);
        c.put(cx, bus_y, die_c);
        c.put(cx + 1, bus_y + 1, die_c);
    }
    int sx = dock_items[dock_count - 1].r.x - 10;
    for (int yy = die.y + 3; yy < die.y + die.h - 8; yy += 3) c.put(sx, yy, 0xFF4E4570);

    if (dock_hover >= 0) {
        Rect r = dock_items[dock_hover].r;
        int cx = r.x + isz / 2 - 1;
        c.fill_alpha(r.x - 3, die.y + 3, isz + 6, die.h - 10, lit, 42);
        c.rect(r.x - 3, die.y + 3, isz + 6, die.h - 10, lit);
        c.fill(r.x - 3, die.y + 3, 2, 2, th::white);
        c.fill(r.x + isz + 1, die.y + 3, 2, 2, th::white);
        c.fill(cx, r.y + r.h + 2, 2, bus_y - (r.y + r.h + 2), lit);
        c.fill(die.x + 4, bus_y, die.w - 8, 2, blend(copper, lit, 110));
        for (int k = 0; k < 10 && pulse < span; k++) {
            int lx = cx - pulse + k, rx = cx + 1 + pulse - k;
            u32 col = blend(lit, th::white, 220 - k * 20);
            if (lx > die.x + 3) c.fill(lx, bus_y - 1, 1, 4, col);
            if (rx < die.x + die.w - 4) c.fill(rx, bus_y - 1, 1, 4, col);
        }
    }

    for (int i = 0; i < dock_count; i++) {
        DockItem& it = dock_items[i];
        int bounce = 0;
        if (it.bounce_t) {
            // two springy hops that get smaller
            int el = (int)(t - it.bounce_t);
            if (el > 700) it.bounce_t = 0;
            else {
                int hop = el < 350 ? el : el - 350, height = el < 350 ? 18 : 8;
                int p = hop * 256 / 350;
                bounce = height * 4 * p * (256 - p) / (256 * 256);
            }
        }
        int y = it.r.y - it.lift16 / 16 - bounce;
        Icon* ic = icon(it.app_id);
        const Image& im = dock_icon_img(ic);
        if (i == dock_press && ui_mouse_left) c.blit_tinted(im, it.r.x, y + 2, 0xFF000000, 70);
        else c.blit(im, it.r.x, y);
        int cx = it.r.x + isz / 2 - 1;
        bool run = strcmp(it.app_id, "trash") && app_running(it.app_id);
        if (run) {
            int ly = bus_y - 5;
            // the LED breathes slowly
            int glow = 50 + (int)(iabs((int)(t / 8 % 128) - 64));
            c.fill_alpha(cx - 6, ly - 2, 14, 7, lit, glow);
            c.fill(cx - 3, ly, 8, 3, blend(lit, th::white, 90));
            int pi = (cx - (d.x + 12) + 5) / 10;
            int px = d.x + 12 + pi * 10;
            c.fill(px, d.y + d.h, 5, 7, blend(lit, th::white, 60));
        }
        if (!strcmp(it.app_id, "trash")) {
            int n = fs_trash_dir()->children.len;
            if (n) {
                char b[8];
                snprintf(b, sizeof(b), "%d", n);
                int bw = max(14, text_width(b) + 6);
                int bx = it.r.x + isz * 50 / 64;
                c.fill(bx, y - 2, bw, 14, th::red);
                c.rect(bx, y - 2, bw, 14, th::ink);
                c.text_centered(bx, y, bw, b, th::white, 1, true);
            }
        }
    }
    if (dock_hover >= 0 && !popup.open) {
        DockItem& it = dock_items[dock_hover];
        const char* name = app_display_name(it.app_id);
        int tw = text_width(name, 1, true) + 16;
        int tx = it.r.x + isz / 2 - tw / 2, ty = d.y - 38 - it.lift16 / 32;
        c.dither(tx + 3, ty + 3, tw, 20, th::ink);
        c.fill(tx, ty, tw, 20, th::bar_bg);
        c.rect(tx, ty, tw, 20, lit);
        c.text(tx + 8, ty + 4, name, th::white, 1, true);
        int px = it.r.x + isz / 2;
        c.fill(px - 3, ty + 20, 7, 1, lit);
        c.fill(px - 2, ty + 21, 5, 1, lit);
        c.fill(px - 1, ty + 22, 3, 1, lit);
    }
}

int dock_hit(int x, int y) {
    if (!Rect{dock_rect.x - 8, dock_rect.y - 20, dock_rect.w + 16, dock_rect.h + 28}.contains(x, y)) return -1;
    for (int i = 0; i < dock_count; i++) {
        Rect r = dock_items[i].r;
        if (x >= r.x - 4 && x < r.x + r.w + 4 && y >= r.y - 20) return i;
    }
    return -1;
}

void dock_click(int i) {
    const char* id = dock_items[i].app_id;
    Window* w = wm_find(id);
    if (w) {
        if (w == wm_focused() && !w->minimized) wm_minimize(w);
        else {
            for (int k = 0; k < windows.len; k++)
                if (!strcmp(windows[k]->app->app_id(), id) && windows[k]->minimized) { wm_focus(windows[k]); k = -1; }
            wm_focus(w);
        }
        return;
    }
    dock_items[i].bounce_t = ticks();
    launch(id);
}

enum { DOCKCMD_OPEN = 1, DOCKCMD_NEW, DOCKCMD_QUIT, DOCKCMD_EMPTY, DOCKCMD_SHOWALL };
static const char* dock_menu_app = nullptr;
static void dock_menu_fn(void*, int cmd) {
    const char* id = dock_menu_app;
    if (cmd == DOCKCMD_OPEN) launch(id);
    else if (cmd == DOCKCMD_NEW) launch("__new__", id);
    else if (cmd == DOCKCMD_QUIT) {
        for (int k = windows.len - 1; k >= 0; k--)
            if (!strcmp(windows[k]->app->app_id(), id)) { AppCtx ctx(windows[k]); windows[k]->app->close(); }
    } else if (cmd == DOCKCMD_EMPTY) {
        int n = fs_trash_dir()->children.len;
        fs_empty_trash();
        char b[64];
        snprintf(b, sizeof(b), "%d item%s deleted for good", n, n == 1 ? "" : "s");
        ui_notify("Trash emptied", b, "trash");
    } else if (cmd == DOCKCMD_SHOWALL) {
        for (int k = 0; k < windows.len; k++)
            if (!strcmp(windows[k]->app->app_id(), id)) wm_focus(windows[k]);
    }
}

void dock_context(int i) {
    dock_menu_app = dock_items[i].app_id;
    Menu m;
    m.add(app_display_name(dock_menu_app), 0, "", false);
    m.sep();
    if (!strcmp(dock_menu_app, "trash")) {
        m.add("Open Trash", DOCKCMD_OPEN);
        m.add("Empty Trash", DOCKCMD_EMPTY, "", fs_trash_dir()->children.len > 0);
    } else {
        bool run = app_running(dock_menu_app);
        if (!run) m.add("Open", DOCKCMD_OPEN);
        else m.add("Show all windows", DOCKCMD_SHOWALL);
        bool single = true;
        for (int k = 0; k < app_info_count; k++) if (!strcmp(app_infos[k].id, dock_menu_app)) single = app_infos[k].single;
        if (!single) m.add("New window", DOCKCMD_NEW);
        if (run) m.sep().add("Quit", DOCKCMD_QUIT);
    }
    Rect r = dock_items[i].r;
    int mh = 6;
    for (int k = 0; k < m.count; k++) mh += m.items[k].separator ? 7 : 18;
    menu_popup(&m, r.x, r.y - mh - 14, dock_menu_fn, nullptr);
}
