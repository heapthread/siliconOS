// silicon - the dock: a floating translucent shelf of app icons with a dot under the ones that are running.
#include "desk.h"

DockItem dock_items[24];
int dock_count = 0;
int dock_hover = -1;
int dock_press = -1;
Rect dock_rect;
u64 dock_anim_t = 0;

static const int DOCK_H = 84;

Rect dock_area() { return {dock_rect.x - 24, dock_rect.y - 52, dock_rect.w + 48, dock_rect.h + 70}; }

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
    int pad = 16, sep = 18;
    int fit = (SW - 40 - 2 * pad - sep) / max(1, dock_count);
    isz = fit >= 72 ? 64 : fit >= 56 ? 48 : 32;
    int slot = isz == 64 ? 72 : isz == 48 ? 56 : max(38, min(fit, 44));
    int total = dock_count * slot + 2 * pad + sep - 8;
    int x = (SW - total) / 2;
    int y = SH - DOCK_H - 12;
    dock_rect = {x, y, total, DOCK_H};
    int cx = x + pad;
    for (int i = 0; i < dock_count; i++) {
        if (i == dock_count - 1) cx += sep;
        dock_items[i].r = {cx, y + 8 + (64 - isz), isz, isz};
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
    u64 t = ticks();
    c.shadow(d.x, d.y + 4, d.w, d.h, 20, 14, 70);
    c.rrect(d.x, d.y, d.w, d.h, 20, th::bar_bg, 205);
    c.rrect_stroke(d.x, d.y, d.w, d.h, 20, th::white, 46);
    // a hairline before the trash
    if (dock_count > 1) {
        int sx = dock_items[dock_count - 1].r.x - 11;
        c.vline(sx, d.y + 16, d.h - 32, 0x40FFFFFF);
    }
    for (int i = 0; i < dock_count; i++) {
        DockItem& it = dock_items[i];
        int bounce = 0;
        if (it.bounce_t) {
            int el = (int)(t - it.bounce_t);
            if (el > 360) it.bounce_t = 0;
            else {
                int p = el * 256 / 360;
                bounce = 10 * 4 * p * (256 - p) / (256 * 256);
            }
        }
        int y = it.r.y - it.lift16 / 16 - bounce;
        Icon* ic = icon(it.app_id);
        const Image& im = dock_icon_img(ic);
        bool hov = i == dock_hover, prs = i == dock_press && ui_mouse_left;
        if (hov) c.rrect(it.r.x - 5, d.y + 4, isz + 10, d.h - 8, 14, th::white, prs ? 44 : 28);
        if (prs) c.blit_tinted(im, it.r.x, y + 1, 0xFF000000, 60);
        else c.blit(im, it.r.x, y);
        int cx = it.r.x + isz / 2;
        bool run = strcmp(it.app_id, "trash") && app_running(it.app_id);
        if (run) c.disc(cx, d.y + d.h - 6, 2, th::white, 230);
        if (!strcmp(it.app_id, "trash")) {
            int n = fs_trash_dir()->children.len;
            if (n) {
                char b[8];
                snprintf(b, sizeof(b), "%d", n);
                int bw = max(16, text_width(b, 1, true) + 10);
                int bx = it.r.x + isz - bw + 4;
                c.rrect(bx, y - 2, bw, 16, 8, th::red);
                c.text_centered(bx, y, bw, b, th::white, 1, true);
            }
        }
    }
    if (dock_hover >= 0 && !popup.open) {
        DockItem& it = dock_items[dock_hover];
        const char* name = app_display_name(it.app_id);
        int tw = text_width(name, 1, true) + 20;
        int tx = clamp(it.r.x + isz / 2 - tw / 2, 4, SW - tw - 4), ty = d.y - 36;
        c.shadow(tx, ty + 2, tw, 26, 8, 8, 70);
        c.rrect(tx, ty, tw, 26, 8, th::bar_bg, 240);
        c.text(tx + 10, ty + 7, name, th::white, 1, true);
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
    int mh = 14;
    for (int k = 0; k < m.count; k++) mh += m.items[k].separator ? MENU_SEP_H : MENU_ITEM_H;
    menu_popup(&m, r.x, r.y - mh - 14, dock_menu_fn, nullptr);
}
