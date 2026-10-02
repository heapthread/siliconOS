// silicon - the desktop core: damage-tracking compositor, window manager with animated
// windows, edge snapping, the overview (expose) and the input/frame loop
#include "desk.h"
#include "../arch/arch.h"
#include "../term/shell.h"

// ================================================================ state
int SW, SH;
Vec<Window*> windows;
static Vec<Window*> closing;            // windows playing their close animation
static Vec<Window*> graveyard;          // freed at a safe point in the loop
bool desk_running = false;
int ui_mouse_x = 512, ui_mouse_y = 384;
bool ui_mouse_left = false;
CursorShape ui_cursor = CUR_ARROW;
Window* cur_win = nullptr;

enum DragKind { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_CAPTURE, DRAG_DOCK };
static DragKind drag = DRAG_NONE;
static Window* drag_win = nullptr;
static int drag_dx, drag_dy, drag_w0, drag_h0;
static Window* title_press = nullptr;
static Window* hover_win = nullptr;

static u64 last_click_t = 0;
static int last_click_x, last_click_y, last_click_btn;

// wallpaper dissolve
static int wall_prev = -1;
static u64 wall_t0 = 0;

// snapping
static int snap_zone = 0;               // 0 none, 1 left half, 2 right half, 3 maximize
static u64 snap_t0 = 0;

// overview
bool overview_on = false;
static u64 overview_t0 = 0;
static bool overview_leaving = false;
static Window* overview_pick = nullptr;

// frame stats
static int fps_now = 0, fps_count = 0;
static u64 fps_t0 = 0;
int ui_fps() { return fps_now; }

// ================================================================ damage
static Rect dmg[24];
static int dmg_n = 0;

void damage(Rect r) {
    r = r.intersect({0, 0, SW, SH});
    if (r.empty()) return;
    for (int i = 0; i < dmg_n; i++) {
        // merge with anything it touches (or nearly touches)
        if (!grow(dmg[i], 8).intersect(r).empty()) {
            dmg[i] = rect_union(dmg[i], r);
            // the merged rect may now swallow others
            for (int j = dmg_n - 1; j >= 0; j--)
                if (j != i && !grow(dmg[i], 8).intersect(dmg[j]).empty()) {
                    dmg[i] = rect_union(dmg[i], dmg[j]);
                    dmg[j] = dmg[--dmg_n];
                    if (i == dmg_n) i = j;
                }
            return;
        }
    }
    if (dmg_n == ARRAY_LEN(dmg)) {
        for (int i = 1; i < dmg_n; i++) dmg[0] = rect_union(dmg[0], dmg[i]);
        dmg_n = 1;
        dmg[0] = rect_union(dmg[0], r);
        return;
    }
    dmg[dmg_n++] = r;
}

void damage_all() { dmg[0] = {0, 0, SW, SH}; dmg_n = 1; }

Rect window_bounds(Window* w) {
    Rect b = {w->r.x, w->r.y, w->r.w + 7, w->r.h + 7};
    if (w->anim != A_NONE) b = rect_union(b, rect_union(grow(w->anim_from, 8), grow(w->anim_to, 8)));
    return b;
}

void damage_window(Window* w) { damage(window_bounds(w)); }

void ui_invalidate() {
    if (cur_win) { cur_win->dirty = true; damage_window(cur_win); }
    else damage_all();
}

void ui_invalidate_all() {
    for (int i = 0; i < windows.len; i++) windows[i]->dirty = true;
    damage_all();
}

// ================================================================ wallpaper
void wallpaper_set(int i) {
    i = clamp(i, 0, wallpaper_count() - 1);
    if (i == settings.wallpaper) return;
    wall_prev = settings.wallpaper;
    wall_t0 = ticks();
    settings.wallpaper = i;
    if (!wallpaper_get(i)->custom) settings_save();
    damage_all();
}

void ui_wallpaper_command(Shell* sh, const char* arg) {
    if (!arg) {
        for (int i = 0; i < wallpaper_count(); i++)
            sh->outf("  %s%2d  %s\n" C_RESET, i == settings.wallpaper ? C_BMAG C_BOLD : "", i, wallpaper_get(i)->name);
        sh->out("usage: wallpaper <number|name>\n");
        return;
    }
    for (int i = 0; i < wallpaper_count(); i++)
        if (!strcmp(wallpaper_get(i)->name, arg) || (arg[0] >= '0' && arg[0] <= '9' && atoi(arg) == i)) {
            wallpaper_set(i);
            sh->outf("wallpaper: %s\n", wallpaper_get(i)->name);
            return;
        }
    sh->outf("no wallpaper called %s\n", arg);
}

// ================================================================ windows
Window* wm_focused() {
    for (int i = windows.len - 1; i >= 0; i--) if (!windows[i]->minimized) return windows[i];
    return nullptr;
}
int wm_count() { return windows.len; }
Window* wm_get(int i) { return windows[i]; }

Window* wm_find(const char* id) {
    for (int i = windows.len - 1; i >= 0; i--) if (!strcmp(windows[i]->app->app_id(), id)) return windows[i];
    return nullptr;
}

bool wm_focused_covers(int x, int y) {
    for (int i = 0; i < windows.len; i++)
        if (!windows[i]->minimized && windows[i]->r.contains(x, y)) return true;
    return false;
}

static Rect work_area() { return {0, TOPBAR_H, SW, SH - TOPBAR_H - 118}; }

static void start_anim(Window* w, int kind, Rect from, Rect to, u32 dur) {
    damage_window(w);
    w->anim = kind;
    w->anim_t0 = ticks();
    w->anim_dur = dur;
    w->anim_from = from;
    w->anim_to = to;
    damage_window(w);
}

static void focus_changed(Window* old_focus) {
    Window* nf = wm_focused();
    if (old_focus == nf) return;
    if (old_focus) { old_focus->dirty = true; damage_window(old_focus); }
    if (nf) { nf->dirty = true; damage_window(nf); }
    damage({0, 0, SW, TOPBAR_H});
}

Window* wm_open(App* app, int w, int h, int x, int y) {
    Window* old = wm_focused();
    Window* win = new Window();
    win->app = app;
    app->win = win;
    strlcpy(win->title, app->title(), sizeof(win->title));
    w += 2 * BORDER;
    h += TITLE_H + BORDER;
    Rect wa = work_area();
    w = min(w, SW - 8);
    h = min(h, SH - TOPBAR_H - 8);
    if (x < 0) {
        static int cascade = 0;
        x = (SW - w) / 2 + (cascade % 6) * 24 - 60;
        y = wa.y + max(8, (wa.h - h) / 2) + (cascade % 6) * 20 - 40;
        cascade++;
    }
    x = clamp(x, 0, SW - w);
    y = clamp(y, TOPBAR_H, max(TOPBAR_H, SH - h));
    win->r = {x, y, w, h};
    win->restore = win->r;
    win->opened_at = ticks();
    windows.push(win);
    {
        AppCtx ctx(win);
        app->on_resize(win->client().w, win->client().h);
    }
    // grow out of a slightly smaller copy of itself
    Rect small{x + w / 16, y + h / 16, w - w / 8, h - h / 8};
    start_anim(win, A_OPEN, small, win->r, 190);
    focus_changed(old);
    return win;
}

void wm_close(Window* w) {
    int i = windows.find(w);
    if (i < 0) return;
    Window* old = wm_focused();
    windows.remove(i);
    if (drag_win == w) { drag = DRAG_NONE; drag_win = nullptr; }
    if (title_press == w) title_press = nullptr;
    if (hover_win == w) hover_win = nullptr;
    if (popup.open && popup.ctx_win == w) menu_close();
    if (overview_pick == w) overview_pick = nullptr;
    Rect small{w->r.x + w->r.w / 12, w->r.y + w->r.h / 12, w->r.w - w->r.w / 6, w->r.h - w->r.h / 6};
    if (w->minimized) graveyard.push(w);
    else {
        start_anim(w, A_CLOSE, w->r, small, 150);
        closing.push(w);
    }
    focus_changed(old == w ? nullptr : old);
    if (wm_focused()) { wm_focused()->dirty = true; damage_window(wm_focused()); }
    damage({0, 0, SW, TOPBAR_H});
}

void wm_focus(Window* w) {
    int i = windows.find(w);
    if (i < 0) return;
    Window* old = wm_focused();
    if (w->minimized) {
        w->minimized = false;
        start_anim(w, A_RESTORE, dock_icon_rect(w->app->app_id()), w->r, 240);
    }
    windows.remove(i);
    windows.push(w);
    damage_window(w);
    focus_changed(old);
}

void wm_minimize(Window* w) {
    if (w->minimized) return;
    Window* old = wm_focused();
    w->minimized = true;
    start_anim(w, A_MIN, w->r, dock_icon_rect(w->app->app_id()), 260);
    focus_changed(old);
}

// change geometry with a morph animation (the old surface stretches to the new rect)
void wm_set_geometry(Window* w, Rect r) {
    if (r.x == w->r.x && r.y == w->r.y && r.w == w->r.w && r.h == w->r.h) return;
    Rect from = w->r;
    image_free(w->anim_img);
    w->anim_img = w->surf;                  // keep the old pixels for the morph
    w->surf = Image();
    w->r = r;
    w->dirty = true;
    {
        AppCtx ctx(w);
        w->app->on_resize(w->client().w, w->client().h);
    }
    start_anim(w, A_GEOM, from, r, 170);
}

void wm_toggle_max(Window* w) {
    if (!w->resizable) return;
    if (w->maximized) { w->maximized = false; wm_set_geometry(w, w->restore); }
    else {
        w->restore = w->r;
        w->maximized = true;
        wm_set_geometry(w, work_area());
    }
}

bool hover_in(Window* w, int x, int y, int ww, int hh) {
    if (!w || overview_on) return false;
    Rect c = w->client();
    int mx = ui_mouse_x - c.x, my = ui_mouse_y - c.y;
    if (!c.contains(ui_mouse_x, ui_mouse_y)) return false;
    for (int i = windows.len - 1; i >= 0 && windows[i] != w; i--)
        if (!windows[i]->minimized && windows[i]->r.contains(ui_mouse_x, ui_mouse_y)) return false;
    return mx >= x && my >= y && mx < x + ww && my < y + hh;
}

// ================================================================ window rendering (into the window's own surface)
static Rect btn_close(Window* w) { return {w->r.x + w->r.w - BORDER - 22, w->r.y + 4, 18, 16}; }
static Rect btn_max(Window* w) { return {w->r.x + w->r.w - BORDER - 42, w->r.y + 4, 18, 16}; }
static Rect btn_min(Window* w) { return {w->r.x + w->r.w - BORDER - 62, w->r.y + 4, 18, 16}; }
static Rect grip(Window* w) { return {w->r.x + w->r.w - 14, w->r.y + w->r.h - 14, 14, 14}; }

static void draw_title_button(Canvas& c, Window* w, Rect sb, int kind, bool focused) {
    bool hot = sb.contains(ui_mouse_x, ui_mouse_y) && hover_win == w;
    Rect b{sb.x - w->r.x, sb.y - w->r.y, sb.w, sb.h};
    u32 face = hot ? (kind == 2 ? th::red : blend(th::paper, accent(), 90)) : (focused ? blend(accent(), th::white, 70) : th::panel);
    bool pressed = hot && ui_mouse_left && title_press == w;
    draw_bevel(c, b.x, b.y, b.w, b.h, face, pressed);
    u32 g = hot && kind == 2 ? th::white : th::ink;
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    if (kind == 0) c.fill(cx - 4, cy + 2, 8, 2, g);
    if (kind == 1) {
        if (w->maximized) { c.rect(cx - 2, cy - 5, 7, 6, g); c.fill(cx - 5, cy - 2, 7, 6, face); c.rect(cx - 5, cy - 2, 7, 6, g); }
        else { c.rect(cx - 4, cy - 4, 9, 8, g); c.hline(cx - 4, cy - 3, 9, g); }
    }
    if (kind == 2)
        for (int i = -3; i <= 3; i++) { c.fill(cx + i, cy + i, 2, 1, g); c.fill(cx + i, cy - i, 2, 1, g); }
}

static void render_window(Window* w) {
    if (w->surf.w != w->r.w || w->surf.h != w->r.h) {
        image_free(w->surf);
        w->surf = image_new(w->r.w, w->r.h, 0xFF000000);
    }
    Canvas c;
    c.init(w->surf.px, w->surf.w, w->surf.h, w->surf.w);
    bool focused = w == wm_focused() && !launcher_open && !overview_on;
    int W = w->r.w, H = w->r.h;
    u32 frame = focused ? blend(accent(), th::ink, 60) : 0xFF8C84A4;
    c.fill(0, 0, W, H, frame);
    c.rect(0, 0, W, H, th::ink);
    u32 top = focused ? accent() : 0xFFB4ACC8;
    u32 bot = focused ? shade(accent(), 62) : 0xFF9E96B6;
    for (int i = 0; i < TITLE_H - 1; i++) {
        int band = i * 4 / (TITLE_H - 1);
        c.hline(1, 1 + i, W - 2, mix(top, bot, band * 85));
    }
    if (focused) for (int yy = 6; yy < TITLE_H - 5; yy += 3) {
        int tx0 = 34 + text_width(w->title, 1, true) + 10;
        int tx1 = btn_min(w).x - w->r.x - 8;
        if (tx1 > tx0) c.hline(tx0, yy, tx1 - tx0, blend(bot, th::ink, 50));
    }
    c.blit(icon(w->app->icon_name())->small, 8, 4);
    if (focused) c.text(33, 8, w->title, shade(accent(), 40), 1, true);
    c.text(32, 7, w->title, focused ? th::white : 0xFF4A4262, 1, true);
    draw_title_button(c, w, btn_min(w), 0, focused);
    if (w->resizable) draw_title_button(c, w, btn_max(w), 1, focused);
    draw_title_button(c, w, btn_close(w), 2, focused);
    Rect cl{BORDER, TITLE_H, W - 2 * BORDER, H - TITLE_H - BORDER};
    c.fill(cl.x, cl.y, cl.w, cl.h, w->app->background());
    c.origin(cl.x, cl.y);
    c.set_clip({0, 0, cl.w, cl.h});
    {
        AppCtx ctx(w);
        w->app->draw(c, cl.w, cl.h);
        if (w->app->dialog) dialog_draw(w->app, c, cl.w, cl.h);
    }
    c.origin(0, 0);
    c.reset_clip();
    c.rect(cl.x - 1, cl.y - 1, cl.w + 2, cl.h + 2, th::ink);
    if (w->resizable && !w->maximized)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j <= i; j++) c.fill(W - 4 - j * 4, H - 12 + i * 4, 2, 2, th::border);
    w->dirty = false;
}

// where and how a window is shown this frame
struct Placement { Rect r; u32 alpha; const Image* img; bool shadow; };

static Placement placement(Window* w) {
    Placement p{w->r, 255, &w->surf, true};
    if (w->anim != A_NONE) {
        int t = (int)min<u64>(256, (ticks() - w->anim_t0) * 256 / max<u32>(1, w->anim_dur));
        switch (w->anim) {
        case A_OPEN: { int e = ease_out(t); p.r = lerp_rect(w->anim_from, w->anim_to, e); p.alpha = e; p.shadow = t > 200; break; }
        case A_CLOSE: { int e = ease_in_out(t); p.r = lerp_rect(w->anim_from, w->anim_to, e); p.alpha = 256 - e; p.shadow = false; break; }
        case A_MIN: { int e = ease_in_out(t); p.r = lerp_rect(w->anim_from, w->anim_to, e); p.alpha = 255 - e * 180 / 256; p.shadow = false; break; }
        case A_RESTORE: { int e = ease_out(t); p.r = lerp_rect(w->anim_from, w->anim_to, e); p.alpha = 75 + e * 180 / 256; p.shadow = t > 220; break; }
        case A_GEOM: { int e = ease_out(t); p.r = lerp_rect(w->anim_from, w->anim_to, e); if (w->anim_img.px) p.img = &w->anim_img; p.shadow = false; break; }
        }
        p.alpha = (u32)clamp<int>((int)p.alpha, 0, 255);
    }
    // overview: every visible window flies to its grid cell
    if (overview_on && !w->minimized) {
        extern Rect overview_cell(Window* w);
        int t = (int)min<u64>(256, (ticks() - overview_t0) * 256 / 260);
        int e = ease_out(overview_leaving ? 256 - t : t);
        if (overview_leaving) e = 256 - ease_out(t);
        p.r = lerp_rect(p.r, overview_cell(w), e);
        p.shadow = true;
    }
    return p;
}

static bool anim_tick(Window* w) {
    if (w->anim == A_NONE) return false;
    if (ticks() - w->anim_t0 >= w->anim_dur) {
        damage_window(w);
        if (w->anim == A_GEOM) image_free(w->anim_img);
        w->anim = A_NONE;
        damage_window(w);
        return false;
    }
    damage_window(w);
    return true;
}

static void draw_window(Canvas& c, Window* w) {
    Placement p = placement(w);
    if (!p.img->px || p.r.w <= 0 || p.r.h <= 0) return;
    if (c.clip.intersect(grow(p.r, 8)).empty()) return;
    if (p.shadow) c.dither(p.r.x + 6, p.r.y + 6, p.r.w, p.r.h, 0xFF000000);
    c.blit_stretch_alpha(*p.img, p.r.x, p.r.y, p.r.w, p.r.h, p.alpha);
}

// ================================================================ snapping
static Rect snap_rect(int zone) {
    Rect wa = work_area();
    if (zone == 1) return {0, wa.y, SW / 2, wa.h};
    if (zone == 2) return {SW / 2, wa.y, SW - SW / 2, wa.h};
    if (zone == 3) return wa;
    return {0, 0, 0, 0};
}

static void draw_snap_preview(Canvas& c) {
    if (!snap_zone) return;
    Rect target = snap_rect(snap_zone);
    int t = ease_out((int)min<u64>(256, (ticks() - snap_t0) * 256 / 160));
    Rect from{ui_mouse_x - 20, ui_mouse_y - 20, 40, 40};
    Rect r = lerp_rect(from, grow(target, -6), t);
    if (t < 256) damage(rect_union(grow(target, 2), grow(from, 2)));
    c.fill_alpha(r.x, r.y, r.w, r.h, accent(), 60);
    c.rect(r.x, r.y, r.w, r.h, accent());
    c.rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, blend(accent(), th::white, 120));
}

// ================================================================ overview (expose)
Rect overview_cell(Window* w) {
    int n = 0, idx = 0;
    for (int i = 0; i < windows.len; i++) {
        if (windows[i]->minimized) continue;
        if (windows[i] == w) idx = n;
        n++;
    }
    int cols = 1;
    while (cols * cols < n) cols++;
    int rows = (n + cols - 1) / cols;
    Rect wa{40, TOPBAR_H + 40, SW - 80, SH - TOPBAR_H - 180};
    int cw = wa.w / cols, ch = wa.h / max(1, rows);
    int col = idx % cols, row = idx / cols;
    Rect cell{wa.x + col * cw + 12, wa.y + row * ch + 12, cw - 24, ch - 40};
    int ww = w->r.w, wh = w->r.h;
    if (ww * cell.h > wh * cell.w) { wh = wh * cell.w / ww; ww = cell.w; } else { ww = ww * cell.h / max(1, wh); wh = cell.h; }
    ww = min(ww, w->r.w); wh = min(wh, w->r.h);
    return {cell.x + (cell.w - ww) / 2, cell.y + (cell.h - wh) / 2, ww, wh};
}

void overview_toggle() {
    if (overview_on && !overview_leaving) { overview_leaving = true; overview_t0 = ticks(); }
    else if (!overview_on) { overview_on = true; overview_leaving = false; overview_t0 = ticks(); menu_close(); launcher_hide(); }
    ui_invalidate_all();
}

static Window* overview_hit(int x, int y) {
    for (int i = windows.len - 1; i >= 0; i--)
        if (!windows[i]->minimized && overview_cell(windows[i]).contains(x, y)) return windows[i];
    return nullptr;
}

static void draw_overview_labels(Canvas& c) {
    if (!overview_on || overview_leaving) return;
    if (ticks() - overview_t0 < 200) return;
    c.text_centered(0, TOPBAR_H + 12, SW, "overview " G_BULLET " click a window, f3 or esc to leave", 0xFFEDE6FF, 1, true);
    for (int i = 0; i < windows.len; i++) {
        Window* w = windows[i];
        if (w->minimized) continue;
        Rect r = overview_cell(w);
        bool hot = r.contains(ui_mouse_x, ui_mouse_y);
        if (hot) { c.rect(r.x - 3, r.y - 3, r.w + 6, r.h + 6, accent()); c.rect(r.x - 2, r.y - 2, r.w + 4, r.h + 4, accent()); }
        int tw = text_width(w->title, 1, true) + 12;
        int tx = r.x + (r.w - tw) / 2, ty = r.y + r.h + 8;
        c.fill(tx, ty, tw, 18, hot ? accent() : th::bar_bg);
        c.text(tx + 6, ty + 3, w->title, th::white, 1, true);
    }
}

// ================================================================ screensaver: a warp through stars and chips
static bool saver_on = false;
static u64 saver_t0 = 0, saver_last = 0, last_input = 0;
static const u64 SAVER_AFTER = 5 * 60 * 1000;
struct WarpStar { int x, y, z; bool chip; };
static WarpStar warp[180];

static void warp_reset(WarpStar& s, bool far) {
    s.x = (rand() % 4001) - 2000;
    s.y = (rand() % 3001) - 1500;
    s.z = far ? 2000 + rand() % 2000 : 50 + rand() % 3950;
    s.chip = rand() % 30 == 0;
}

void saver_start() {
    if (saver_on) return;
    saver_on = true;
    saver_t0 = saver_last = ticks();
    for (auto& s : warp) warp_reset(s, false);
    menu_close();
    damage_all();
}

static void saver_stop() {
    saver_on = false;
    last_input = ticks();
    damage_all();
}

static int tri(u64 t, int span) {           // bounce 0..span..0
    if (span <= 0) return 0;
    int p = (int)(t % (u64)(2 * span));
    return p < span ? p : 2 * span - p;
}

static void draw_saver(Canvas& c) {
    u64 t = ticks();
    int dt = (int)min<u64>(t - saver_last, 50);
    saver_last = t;
    int fade = (int)min<u64>(255, (t - saver_t0) * 255 / 600);
    c.fill(0, 0, SW, SH, 0xFF07050D);
    int speed = 3 + (int)min<u64>(9, (t - saver_t0) / 700);
    for (auto& s : warp) {
        int oz = s.z;
        s.z -= dt * speed;
        if (s.z <= 16) { warp_reset(s, true); continue; }
        int sx = SW / 2 + s.x * 256 / s.z, sy = SH / 2 + s.y * 256 / s.z;
        if (sx < -80 || sy < -80 || sx > SW + 80 || sy > SH + 80) { warp_reset(s, true); continue; }
        int b = 255 - s.z * 255 / 4000;
        b = b * fade / 255;
        if (s.chip) {
            int size = clamp(48 * 256 / s.z, 4, 220);
            c.blit_scaled(icon("silicon")->big, sx - size / 2, sy - size / 2, size, size);
            continue;
        }
        // a short streak from where the star was a moment ago
        int px = SW / 2 + s.x * 256 / oz, py = SH / 2 + s.y * 256 / oz;
        u32 col = blend(0xFF07050D, s.z < 800 ? 0xFFFFFFFF : 0xFFB8A4FF, (u32)clamp(b, 0, 255));
        c.line(px, py, sx, sy, col);
        if (s.z < 600) c.fill(sx - 1, sy - 1, 2, 2, col);
    }
    // a slowly bouncing clock, so nothing burns in
    DateTime d;
    rtc_read(d);
    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d", d.hour, d.minute);
    int tw = text_width(b, 6, true), th_ = 12 * 6;
    int x = tri((t - saver_t0) / 30, SW - tw - 40) + 20, y = tri((t - saver_t0) / 41, SH - th_ - 60) + 20;
    c.text_shadow(x, y, b, blend(0xFF07050D, 0xFFEDE6FF, (u32)fade), 0xFF000000, 6, true);
    c.text(x + 4, y + th_ + 8, "silicon", blend(0xFF07050D, accent(), (u32)fade), 2, true);
}

// ================================================================ compose
static void compose_rect(Rect R) {
    Canvas& c = fb.back;
    c.set_clip(R);
    if (saver_on) {
        draw_saver(c);
        c.reset_clip();
        return;
    }
    // wallpaper, with a chunky pixel dissolve when it changes
    Wallpaper* wp = wallpaper_get(settings.wallpaper);
    int dlevel = wall_prev >= 0 ? (int)((ticks() - wall_t0) * 17 / 500) : 17;
    if (dlevel < 17 && wp->full.w == SW && wallpaper_get(wall_prev)->full.w == SW)
        c.dissolve(wallpaper_get(wall_prev)->full, wp->full, dlevel);
    else if (wp->full.w == SW && wp->full.h == SH) c.blit_raw(wp->full.px, SW, SH, SW, 0, 0);
    else c.blit_scaled(wp->full, 0, 0, SW, SH);
    if (overview_on) {
        int t = (int)min<u64>(256, (ticks() - overview_t0) * 256 / 260);
        int e = overview_leaving ? 256 - ease_out(t) : ease_out(t);
        c.fill_alpha(0, 0, SW, SH, 0xFF0C0916, (u32)(150 * e / 256));
    } else {
        draw_desktop_icons(c);
    }
    for (int i = 0; i < windows.len; i++) {
        Window* w = windows[i];
        if (w->minimized && w->anim != A_MIN) continue;
        draw_window(c, w);
    }
    for (int i = 0; i < closing.len; i++) draw_window(c, closing[i]);
    draw_overview_labels(c);
    draw_snap_preview(c);
    draw_dock(c);
    draw_topbar(c);
    draw_toasts(c);
    draw_launcher(c);
    draw_switcher(c);
    draw_menu(c);
    draw_cursor(c);
    c.reset_clip();
}

static void compose() {
    // render windows whose content changed, then repaint only the damaged parts of the screen
    for (int i = 0; i < windows.len; i++) if (windows[i]->dirty) render_window(windows[i]);
    Rect list[24];
    int n = dmg_n;
    memcpy(list, dmg, sizeof(Rect) * n);
    dmg_n = 0;
    for (int i = 0; i < n; i++) compose_rect(list[i]);
    for (int i = 0; i < n; i++) fb_present_rect(list[i]);
    fps_count++;
}

// ================================================================ input: mouse
static Window* window_at(int x, int y) {
    if (overview_on) return nullptr;
    for (int i = windows.len - 1; i >= 0; i--)
        if (!windows[i]->minimized && windows[i]->r.contains(x, y)) return windows[i];
    return nullptr;
}

static void send_mouse(Window* w, MouseKind kind, int button, int wheel = 0) {
    Rect cl = w->client();
    MouseEv e{kind, ui_mouse_x - cl.x, ui_mouse_y - cl.y, button, wheel, ui_mouse_left};
    AppCtx ctx(w);
    if (w->app->dialog) {
        if (kind == M_UP || kind == M_DOWN) dialog_mouse(w->app, e, cl.w, cl.h);
        ui_invalidate();
        return;
    }
    w->app->on_mouse(e);
}

static void set_cursor(CursorShape s) {
    if (s == ui_cursor) return;
    damage(cursor_rect());
    ui_cursor = s;
}

static void update_cursor() {
    CursorShape s = CUR_ARROW;
    if (drag == DRAG_MOVE) s = CUR_MOVE;
    else if (drag == DRAG_RESIZE) s = CUR_RESIZE;
    else if (launcher_open || popup.open) s = CUR_ARROW;
    else if (overview_on) s = overview_hit(ui_mouse_x, ui_mouse_y) ? CUR_HAND : CUR_ARROW;
    else if (dock_hover >= 0) s = CUR_HAND;
    else {
        Window* w = window_at(ui_mouse_x, ui_mouse_y);
        if (w) {
            Rect cl = w->client();
            if (w->resizable && !w->maximized && grip(w).contains(ui_mouse_x, ui_mouse_y)) s = CUR_RESIZE;
            else if (cl.contains(ui_mouse_x, ui_mouse_y) && !w->app->dialog) {
                AppCtx ctx(w);
                s = (CursorShape)w->app->cursor_at(ui_mouse_x - cl.x, ui_mouse_y - cl.y);
            }
        }
    }
    set_cursor(s);
}

static void end_drag() {
    if (drag == DRAG_MOVE && drag_win && snap_zone) {
        drag_win->restore = drag_win->r;
        drag_win->maximized = snap_zone == 3;
        wm_set_geometry(drag_win, snap_rect(snap_zone));
        damage(grow(snap_rect(snap_zone), 4));
    }
    snap_zone = 0;
    drag = DRAG_NONE;
    drag_win = nullptr;
}

static void on_button(int button, bool down) {
    int mx = ui_mouse_x, my = ui_mouse_y;

    if (popup.open) {
        if (down) {
            int hit = menu_hit(mx, my);
            if (hit == -1) {
                if (my < TOPBAR_H) {
                    for (int i = 0; i < bar_menu_count; i++)
                        if (bar_menu_rects[i].contains(mx, my)) {
                            if (popup.bar_index == i) { menu_close(); return; }
                            open_bar_menu(i);
                            return;
                        }
                }
                menu_close();
                if (my < TOPBAR_H) return;
            } else return;
        } else {
            int hit = menu_hit(mx, my);
            if (hit >= 0) menu_activate(hit);
            return;
        }
    }

    if (launcher_open) {
        if (!down) return;
        Rect r = launcher_rect();
        if (!r.contains(mx, my)) { launcher_hide(); return; }
        for (int i = 0; i < lcount; i++)
            if (Rect{r.x + 16, r.y + 60 + i * 34, r.w - 32, 32}.contains(mx, my)) { launcher_activate(i); return; }
        return;
    }

    if (overview_on) {
        if (down || overview_leaving) return;
        Window* w = overview_hit(mx, my);
        overview_pick = w;
        overview_toggle();
        if (w) wm_focus(w);
        return;
    }

    if (!down) {
        if (drag == DRAG_CAPTURE && drag_win) send_mouse(drag_win, M_UP, button);
        else if (drag == DRAG_DOCK) {
            int hit = dock_hit(mx, my);
            if (hit == dock_press && hit >= 0) dock_click(hit);
            damage(dock_area());
        } else if (drag == DRAG_NONE) {
            Window* w = window_at(mx, my);
            if (w && w->client().contains(mx, my) && button == 2) send_mouse(w, M_UP, 2);
        }
        Window* tp = title_press;
        title_press = nullptr;
        if (tp && button == 1 && drag != DRAG_RESIZE) {
            tp->dirty = true;
            damage_window(tp);
            if (window_at(mx, my) == tp && snap_zone == 0) {
                AppCtx ctx(tp);
                if (btn_close(tp).contains(mx, my)) tp->app->close();
                else if (btn_min(tp).contains(mx, my)) wm_minimize(tp);
                else if (tp->resizable && btn_max(tp).contains(mx, my)) wm_toggle_max(tp);
            }
        }
        end_drag();
        dock_press = -1;
        update_cursor();
        return;
    }

    bool dbl = button == last_click_btn && ticks() - last_click_t < 400 && iabs(mx - last_click_x) < 5 && iabs(my - last_click_y) < 5;
    last_click_t = dbl ? 0 : ticks();
    last_click_x = mx; last_click_y = my; last_click_btn = button;

    if (my < TOPBAR_H) {
        for (int i = 0; i < bar_menu_count; i++)
            if (bar_menu_rects[i].contains(mx, my)) { open_bar_menu(i); return; }
        if (search_rect.contains(mx, my)) { launcher_show(); return; }
        if (clock_rect.contains(mx, my)) { AppCtx ctx(nullptr); launch("clock"); return; }
        return;
    }
    int dh = dock_hit(mx, my);
    if (dh >= 0) {
        if (button == 2) { dock_context(dh); return; }
        dock_press = dh;
        drag = DRAG_DOCK;
        damage(dock_area());
        return;
    }
    Window* w = window_at(mx, my);
    if (w) {
        if (w != wm_focused()) wm_focus(w);
        Rect cl = w->client();
        if (my < w->r.y + TITLE_H) {
            title_press = w;
            w->dirty = true;
            damage_window(w);
            if (btn_close(w).contains(mx, my) || btn_min(w).contains(mx, my) || (w->resizable && btn_max(w).contains(mx, my))) return;
            if (dbl && button == 1) { wm_toggle_max(w); title_press = nullptr; return; }
            if (w->maximized) {
                int rel = (mx - w->r.x) * w->restore.w / max(1, w->r.w);
                w->maximized = false;
                damage_window(w);
                Rect r = w->restore;
                r.x = mx - rel;
                r.y = my - 10;
                w->r = r;
                w->dirty = true;
                AppCtx ctx(w);
                w->app->on_resize(w->client().w, w->client().h);
            }
            drag = DRAG_MOVE;
            drag_win = w;
            drag_dx = mx - w->r.x;
            drag_dy = my - w->r.y;
            return;
        }
        if (w->resizable && !w->maximized && grip(w).contains(mx, my)) {
            drag = DRAG_RESIZE;
            drag_win = w;
            drag_dx = mx; drag_dy = my;
            drag_w0 = w->r.w; drag_h0 = w->r.h;
            return;
        }
        if (cl.contains(mx, my)) {
            if (button == 1) { drag = DRAG_CAPTURE; drag_win = w; }
            send_mouse(w, dbl ? M_DBL : M_DOWN, button);
        }
        return;
    }
    int di = desk_hit(mx, my);
    if (di >= 0) { desk_click(di, button, dbl, mx, my); return; }
    if (desk_sel >= 0) { desk_sel = -1; damage({0, TOPBAR_H, 400, SH - TOPBAR_H}); }
    if (button == 2) desk_background_menu(mx, my);
}

static void on_mouse_move(int ox, int oy) {
    int mx = ui_mouse_x, my = ui_mouse_y;
    // cursor trail: repaint where it was and where it is
    damage({ox - 8, oy - 8, 26, 28});
    damage(cursor_rect());
    if (popup.open) {
        int h = menu_hit(mx, my);
        if (h != popup.hover) { popup.hover = h; damage(menu_rect()); }
        if (popup.bar_index >= 0 && my < TOPBAR_H)
            for (int i = 0; i < bar_menu_count; i++)
                if (bar_menu_rects[i].contains(mx, my) && i != popup.bar_index) open_bar_menu(i);
    }
    if (launcher_open) {
        Rect r = launcher_rect();
        for (int i = 0; i < lcount; i++)
            if (Rect{r.x + 16, r.y + 60 + i * 34, r.w - 32, 32}.contains(mx, my) && launcher_sel != i) { launcher_sel = i; damage(r); }
    }
    if (oy < TOPBAR_H + 2 || my < TOPBAR_H + 2) damage({0, 0, SW, TOPBAR_H});
    if (overview_on) { damage_all(); update_cursor(); return; }
    switch (drag) {
    case DRAG_MOVE: {
        damage_window(drag_win);
        drag_win->r.x = clamp(mx - drag_dx, -drag_win->r.w + 80, SW - 80);
        drag_win->r.y = clamp(my - drag_dy, TOPBAR_H, SH - TITLE_H);
        damage_window(drag_win);
        int zone = drag_win->resizable ? (mx <= 5 ? 1 : mx >= SW - 6 ? 2 : my <= TOPBAR_H + 2 ? 3 : 0) : 0;
        if (zone != snap_zone) {
            if (snap_zone) damage(grow(snap_rect(snap_zone), 4));
            snap_zone = zone;
            snap_t0 = ticks();
            if (zone) damage(grow(snap_rect(zone), 4));
        }
        break;
    }
    case DRAG_RESIZE: {
        damage_window(drag_win);
        drag_win->r.w = clamp(drag_w0 + mx - drag_dx, drag_win->min_w, SW - drag_win->r.x);
        drag_win->r.h = clamp(drag_h0 + my - drag_dy, drag_win->min_h, SH - drag_win->r.y);
        drag_win->maximized = false;
        drag_win->dirty = true;
        {
            AppCtx ctx(drag_win);
            drag_win->app->on_resize(drag_win->client().w, drag_win->client().h);
        }
        damage_window(drag_win);
        break;
    }
    case DRAG_CAPTURE:
        if (drag_win) send_mouse(drag_win, M_MOVE, 0);
        break;
    default: {
        int h = (popup.open || launcher_open) ? -1 : dock_hit(mx, my);
        if (h != dock_hover) { if (h >= 0 && dock_hover < 0) dock_anim_t = ticks(); dock_hover = h; damage(dock_area()); }
        Window* w = window_at(mx, my);
        if (w != hover_win) {
            if (hover_win) { hover_win->dirty = true; damage_window(hover_win); }
            hover_win = w;
        }
        if (w && !popup.open && !launcher_open) {
            bool in_title = my < w->r.y + TITLE_H + 2;
            if (in_title || w->app->hover_effects()) { w->dirty = true; damage_window(w); }
            if (w->client().contains(mx, my)) send_mouse(w, M_MOVE, 0);
        }
        // desktop icon hover glow
        if (!w) damage({0, TOPBAR_H, 400, SH - TOPBAR_H - 100});
        break;
    }
    }
    update_cursor();
}

// ================================================================ input: keyboard
static void on_key(const KeyEvent& e) {
    last_input = ticks();
    if (saver_on) { if (e.down) saver_stop(); return; }
    if (e.key == K_LALT || e.key == K_RALT) {
        if (!e.down && switcher_open) {
            switcher_open = false;
            damage_all();
            if (windows.len) wm_focus(windows[windows.len - 1 - switcher_sel]);
        }
        return;
    }
    if (!e.down) {
        Window* f = wm_focused();
        if (f && f->app->wants_key_up() && !launcher_open) { AppCtx ctx(f); f->app->on_key(e); }
        return;
    }
    if (e.alt && e.key == K_TAB) {
        if (!windows.len) return;
        if (!switcher_open) { switcher_open = true; switcher_sel = windows.len > 1 ? 1 : 0; }
        else switcher_sel = (switcher_sel + (e.shift ? windows.len - 1 : 1)) % windows.len;
        damage(grow(switcher_rect(), 8));
        return;
    }
    if (e.key == K_F3 || (e.ctrl && e.key == K_UP)) { overview_toggle(); return; }
    if (overview_on) { if (e.key == K_ESC || e.key == K_ENTER) overview_toggle(); return; }
    if (e.ctrl && e.key == ' ') { if (launcher_open) launcher_hide(); else launcher_show(); return; }
    if (e.key == K_SUPER) { launcher_show(); return; }
    if (e.ctrl && e.alt && e.key == 't') { AppCtx ctx(nullptr); launch("__new__", "terminal"); return; }
    if ((e.alt && e.key == K_F4) || (e.ctrl && e.key == 'w')) {
        if (wm_focused()) { AppCtx ctx(wm_focused()); wm_focused()->app->close(); }
        return;
    }
    if (e.key == K_F11 && wm_focused()) { wm_toggle_max(wm_focused()); return; }
    if (popup.open) {
        Menu& m = popup.menu;
        if (e.key == K_ESC) { menu_close(); return; }
        if (e.key == K_DOWN || e.key == K_UP) {
            int d = e.key == K_DOWN ? 1 : -1, h = popup.hover;
            for (int k = 0; k < m.count; k++) {
                h = (h + d + m.count) % m.count;
                if (!m.items[h].separator && m.items[h].enabled) break;
            }
            popup.hover = h;
            damage(menu_rect());
            return;
        }
        if ((e.key == K_LEFT || e.key == K_RIGHT) && popup.bar_index >= 0) {
            open_bar_menu((popup.bar_index + (e.key == K_RIGHT ? 1 : bar_menu_count - 1)) % bar_menu_count);
            return;
        }
        if (e.key == K_ENTER && popup.hover >= 0) menu_activate(popup.hover);
        return;
    }
    if (launcher_open) {
        Rect r = launcher_rect();
        if (e.key == K_ESC) { launcher_hide(); return; }
        if (e.key == K_ENTER) { launcher_activate(launcher_sel); return; }
        if (e.key == K_DOWN) launcher_sel = min(launcher_sel + 1, lcount - 1);
        else if (e.key == K_UP) launcher_sel = max(launcher_sel - 1, 0);
        else if (launcher_field.on_key(e)) { launcher_sel = 0; launcher_refresh(); }
        damage(rect_union(r, launcher_rect()));
        return;
    }
    Window* f = wm_focused();
    if (f) {
        AppCtx ctx(f);
        if (f->app->dialog) dialog_key(f->app, e);
        else f->app->on_key(e);
        return;
    }
    desk_key(e);
}

// ================================================================ main loop
void ui_run() {
    SW = fb.width;
    SH = fb.height;
    desk_running = true;
    popup.open = false;
    launcher_open = false;
    overview_on = false;
    if (settings.wallpaper >= wallpaper_count()) settings.wallpaper = 0;
    wall_prev = -1;
    MouseState ms;
    mouse_update(ms);
    ui_mouse_x = ms.x; ui_mouse_y = ms.y;
    dock_layout();
    ui_invalidate_all();
    static bool first = true;
    if (first) {
        first = false;
        char b[96];
        snprintf(b, sizeof(b), "welcome, %s. ctrl+space to search", settings.user);
        ui_notify("silicon desktop", b, "silicon");
    }
    u64 last_frame = 0, last_beat = 0;
    u32 fs_gen = fs_generation(), set_gen = settings_generation();
    bool prev_left = false, prev_right = false;
    fps_t0 = ticks();
    while (desk_running) {
        while (graveyard.len) {
            Window* g = graveyard.back();
            graveyard.len--;
            image_free(g->surf);
            image_free(g->anim_img);
            delete g->app;
            delete g;
        }
        KeyEvent ke;
        while (kbd_poll(ke)) on_key(ke);
        bool mupd = mouse_update(ms);
        if (mupd && saver_on) {
            last_input = ticks();
            ui_mouse_x = clamp(ms.x, 0, SW - 1);
            ui_mouse_y = clamp(ms.y, 0, SH - 1);
            prev_left = ms.left;
            prev_right = ms.right;
            saver_stop();
        } else if (mupd) {
            last_input = ticks();
            int ox = ui_mouse_x, oy = ui_mouse_y;
            ui_mouse_x = clamp(ms.x, 0, SW - 1);
            ui_mouse_y = clamp(ms.y, 0, SH - 1);
            bool left_changed = ms.left != prev_left, right_changed = ms.right != prev_right;
            ui_mouse_left = ms.left;
            if (ox != ui_mouse_x || oy != ui_mouse_y) on_mouse_move(ox, oy);
            if (left_changed) on_button(1, ms.left);
            if (right_changed) on_button(2, ms.right);
            prev_left = ms.left;
            prev_right = ms.right;
            if (ms.wheel) {
                Window* w = window_at(ui_mouse_x, ui_mouse_y);
                if (w && !w->app->dialog) send_mouse(w, M_WHEEL, 0, ms.wheel);
            }
        }
        extern void net_poll();
        net_poll();
        dock_layout();
        dock_animate();
        for (int i = 0; i < windows.len; i++) {
            Window* w = windows[i];
            AppCtx ctx(w);
            w->app->tick();
            anim_tick(w);
        }
        for (int i = closing.len - 1; i >= 0; i--)
            if (!anim_tick(closing[i])) { graveyard.push(closing[i]); closing.remove(i); }
        if (overview_on) {
            damage_all();
            if (overview_leaving && ticks() - overview_t0 > 260) { overview_on = false; ui_invalidate_all(); }
        }
        if (wall_prev >= 0) { if (ticks() - wall_t0 > 520) wall_prev = -1; damage_all(); }
        if (!last_input) last_input = ticks();
        if (!saver_on && ticks() - last_input > SAVER_AFTER) saver_start();
        if (saver_on) damage_all();
        if (toasts_active()) damage(toast_area());
        if (launcher_open && ticks() - launcher_t0 < 200) damage_all();
        // global state changed: re-render everything
        if (fs_generation() != fs_gen || settings_generation() != set_gen) {
            fs_gen = fs_generation();
            set_gen = settings_generation();
            ui_invalidate_all();
        }
        fs_tick();
        // heartbeat: clock + blinking carets
        if (ticks() - last_beat >= 500) {
            last_beat = ticks();
            damage({0, 0, SW, TOPBAR_H});
            Window* f = wm_focused();
            if (f) { f->dirty = true; damage_window(f); }
            if (launcher_open) damage(launcher_rect());
        }
        if (ticks() - fps_t0 >= 1000) {
            fps_now = (int)(fps_count * 1000 / (ticks() - fps_t0));
            fps_count = 0;
            fps_t0 = ticks();
            if (settings.show_fps) damage({0, 0, SW, TOPBAR_H});
        }
        if (dmg_n && ticks() - last_frame >= 15) {
            last_frame = ticks();
            compose();
        }
        // a hlt can oversleep a whole host timer period, so only sleep when nothing moves
        extern bool net_busy();
        if (dmg_n || ticks() - last_frame < 120 || net_busy()) cpu_rest();
        else cpu_idle();
    }
    fs_sync();
}
