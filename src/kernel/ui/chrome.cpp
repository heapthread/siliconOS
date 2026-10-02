// silicon - desktop chrome: cursors, popup menus, top bar, launcher, alt-tab, toasts,
// desktop icons and the system commands they trigger
#include "desk.h"
bool is_image_name(const char* name);

// ================================================================ cursors
static const char* cur_arrow[] = {
    "X..........", "XX.........", "XoX........", "XooX.......", "XoooX......", "XooooX.....",
    "XoooooX....", "XooooooX...", "XoooooooX..", "XooooooooX.", "XoooooXXXXX", "XooXooX....",
    "XoX.XooX...", "XX..XooX...", "X....XooX..", ".....XooX..", "......XX...", nullptr};
static const char* cur_hand[] = {
    "....XX........", "...XooX.......", "...XooX.......", "...XooX.......", "...XooXXX.....",
    "...XooXooXXX..", "XX.XooXooXooXX", "XoXXooooooXooX", "XooXoooooooooX", ".XoooooooooooX",
    "..XooooooooooX", "..XoooooooooX.", "...XooooooooX.", "...XoooooooX..", "....XooooooX..",
    "....XXXXXXXX..", nullptr};
static const char* cur_ibeam[] = {
    "XXX.XXX", "XooXooX", ".XXoXX.", "..XoX..", "..XoX..", "..XoX..", "..XoX..", "..XoX..",
    "..XoX..", "..XoX..", ".XXoXX.", "XooXooX", "XXX.XXX", nullptr};
static const char* cur_move[] = {
    "......X......", ".....XoX.....", "....XoooX....", "...XXXoXXX...", "..X..XoX..X..",
    ".XoXXXoXXXoX.", "XoooooooooooX", ".XoXXXoXXXoX.", "..X..XoX..X..", "...XXXoXXX...",
    "....XoooX....", ".....XoX.....", "......X......", nullptr};
static const char* cur_resize[] = {
    "XXXXXX.....", "XooooX.....", "XoooX......", "XooXoX.....", "XoX.XoX....", "XX...XoX...",
    "....XoX.XX", ".....XoXoX", "......XooX", ".....XoooX", "....XooooX", "....XXXXXX", nullptr};
static const char* cur_cross[] = {
    "....XXX....", "....XoX....", "....XoX....", "....XoX....", "XXXXX.XXXXX", "Xooo...oooX",
    "XXXXX.XXXXX", "....XoX....", "....XoX....", "....XoX....", "....XXX....", nullptr};
static const char* cur_wait[] = {
    "XXXXXXXXXXX", "XoooooooooX", ".XoooooooX.", ".XooXoXooX.", "..XooXooX..", "...XoooX...",
    "....XoX....", "....XoX....", "...XoooX...", "..XoooooX..", ".XooXoXooX.", ".XoXoXoXoX.",
    "XoooooooooX", "XXXXXXXXXXX", nullptr};

Rect cursor_rect() { return {ui_mouse_x - 8, ui_mouse_y - 8, 26, 28}; }

void draw_cursor(Canvas& c) {
    const char** art = cur_arrow;
    int hx = 0, hy = 0;
    switch (ui_cursor) {
    case CUR_HAND: art = cur_hand; hx = 4; break;
    case CUR_IBEAM: art = cur_ibeam; hx = 3; hy = 6; break;
    case CUR_MOVE: art = cur_move; hx = 6; hy = 6; break;
    case CUR_RESIZE: art = cur_resize; hx = 5; hy = 5; break;
    case CUR_CROSS: art = cur_cross; hx = 5; hy = 5; break;
    case CUR_WAIT: art = cur_wait; hx = 5; hy = 7; break;
    default: break;
    }
    // soft 1px shadow makes the cursor readable on busy wallpapers
    for (int y = 0; art[y]; y++)
        for (int x = 0; art[y][x]; x++)
            if (art[y][x] != '.') c.put(ui_mouse_x - hx + x + 1, ui_mouse_y - hy + y + 1, blend(c.get(ui_mouse_x - hx + x + 1, ui_mouse_y - hy + y + 1), 0xFF000000, 90));
    for (int y = 0; art[y]; y++)
        for (int x = 0; art[y][x]; x++) {
            char ch = art[y][x];
            if (ch == 'X') c.put(ui_mouse_x - hx + x, ui_mouse_y - hy + y, th::ink);
            else if (ch == 'o') c.put(ui_mouse_x - hx + x, ui_mouse_y - hy + y, th::white);
        }
}

// ================================================================ popup menus
MenuState popup;

static int menu_width(Menu& m) {
    int w = 120;
    for (int i = 0; i < m.count; i++)
        w = max(w, text_width(m.items[i].label) + text_width(m.items[i].shortcut) + 52);
    return w;
}
static int menu_height(Menu& m) {
    int h = 6;
    for (int i = 0; i < m.count; i++) h += m.items[i].separator ? 7 : 18;
    return h;
}

Rect menu_rect() {
    if (!popup.open) return {0, 0, 0, 0};
    return {popup.x, popup.y, menu_width(popup.menu) + 6, menu_height(popup.menu) + 6};
}

void menu_popup(Menu* m, int x, int y, MenuFn fn, void* ctx) {
    if (popup.open) damage(menu_rect());
    popup.menu = *m;
    int w = menu_width(popup.menu), h = menu_height(popup.menu);
    popup.x = clamp(x, 0, SW - w - 2);
    popup.y = clamp(y, TOPBAR_H, SH - h - 2);
    popup.fn = fn;
    popup.ctx = ctx;
    popup.ctx_win = cur_win;
    popup.hover = -1;
    popup.open = true;
    popup.bar_index = -1;
    popup.t0 = ticks();
    damage(menu_rect());
    damage({0, 0, SW, TOPBAR_H});
}

void menu_close() {
    if (!popup.open) return;
    damage(menu_rect());
    damage({0, 0, SW, TOPBAR_H});
    popup.open = false;
}

void menu_activate(int item) {
    MenuFn fn = popup.fn;
    void* ctx = popup.ctx;
    Window* w = popup.ctx_win;
    int cmd = popup.menu.items[item].cmd;
    menu_close();
    if (!fn) return;
    // make sure the context window still exists
    if (w && windows.find(w) < 0) return;
    AppCtx actx(w);
    fn(ctx, cmd);
}

int menu_hit(int mx, int my) {
    Menu& m = popup.menu;
    int w = menu_width(m);
    if (mx < popup.x || mx >= popup.x + w) return -1;
    int y = popup.y + 3;
    for (int i = 0; i < m.count; i++) {
        int h = m.items[i].separator ? 7 : 18;
        if (my >= y && my < y + h) return m.items[i].separator || !m.items[i].enabled ? -2 : i;
        y += h;
    }
    return -1;
}

void draw_menu(Canvas& c) {
    if (!popup.open) return;
    Menu& m = popup.menu;
    int w = menu_width(m), h = menu_height(m);
    int x = popup.x, y = popup.y;
    // unroll downwards over 110ms
    int p = ease_out((int)min<u64>(256, (ticks() - popup.t0) * 256 / 110));
    int shown = max(8, h * p / 256);
    if (p < 256) damage(menu_rect());
    Rect saved = c.clip;
    c.clip = c.clip.intersect({x, y, w + 6, shown + 6});
    c.dither(x + 4, y + 4, w, h, th::ink);
    c.fill(x, y, w, h, th::paper);
    c.rect(x, y, w, h, th::ink);
    c.hline(x + 1, y + 1, w - 2, th::white);
    int yy = y + 3;
    for (int i = 0; i < m.count; i++) {
        MenuItem& it = m.items[i];
        if (it.separator) {
            for (int sx = x + 6; sx < x + w - 6; sx += 2) c.put(sx, yy + 3, th::border);
            yy += 7;
            continue;
        }
        bool hot = i == popup.hover;
        if (hot) c.fill(x + 3, yy, w - 6, 18, accent());
        u32 col = !it.enabled ? th::shadow : hot ? th::white : th::text;
        if (it.checked) c.text(x + 8, yy + 3, G_CHECK, col);
        c.text(x + 22, yy + 3, it.label, col, 1, !it.enabled && i == 0);
        if (it.shortcut[0]) c.text(x + w - 10 - text_width(it.shortcut), yy + 3, it.shortcut, hot ? th::white : th::muted);
        yy += 18;
    }
    if (p < 256) c.hline(x, y + shown - 1, w, th::ink);
    c.clip = saved;
}

// ================================================================ top bar
static Menu bar_menus[5];
int bar_menu_count = 0;
Rect bar_menu_rects[5];
Rect chip_rect, clock_rect, search_rect;

static void build_bar_menus() {
    Window* f = wm_focused();
    bar_menus[0].clear();
    bar_menus[0].add("About silicon", SYS_ABOUT)
        .sep()
        .add("Settings" G_ELLIP, SYS_SETTINGS)
        .add("Launcher", SYS_LAUNCHER, "ctrl+space")
        .add("Overview", SYS_OVERVIEW, "f3")
        .add("Monitor", SYS_MONITOR)
        .add("Screensaver", SYS_SAVER)
        .sep()
        .add("Terminal mode", SYS_TERMINAL_MODE)
        .sep()
        .add("Restart" G_ELLIP, SYS_RESTART)
        .add("Shut down" G_ELLIP, SYS_SHUTDOWN);
    int n = 0;
    if (f) {
        for (int i = 1; i < 5; i++) bar_menus[i].clear();
        AppCtx ctx(f);
        n = f->app->menus(&bar_menus[1]);
    }
    if (!f || n == 0) {
        Menu& file = bar_menus[1];
        file.clear();
        strlcpy(file.title, "File", sizeof(file.title));
        file.add("New terminal", SYS_NEW_TERMINAL, "ctrl+alt+t").add("New folder", SYS_NEW_FOLDER).add("New text file", SYS_NEW_FILE);
        if (f) file.sep().add("Close window", SYS_CLOSE_WIN, "ctrl+w");
        Menu& view = bar_menus[2];
        view.clear();
        strlcpy(view.title, "View", sizeof(view.title));
        view.add("Overview", SYS_OVERVIEW, "f3").add("Wallpapers" G_ELLIP, SYS_WALLPAPER).add("Next wallpaper", SYS_NEXT_WALL)
            .sep().add("Minimize all", SYS_MIN_ALL);
        Menu& go = bar_menus[3];
        go.clear();
        strlcpy(go.title, "Go", sizeof(go.title));
        go.add("Home", SYS_GO_HOME).add("Notes", SYS_GO_NOTES).add("Pictures", SYS_GO_PICS).add("Disk", SYS_GO_ROOT)
          .add("Trash", SYS_GO_TRASH).sep().add("Web browser", SYS_BROWSER);
        Menu& help = bar_menus[4];
        help.clear();
        strlcpy(help.title, "Help", sizeof(help.title));
        help.add("silicon tips", SYS_TIPS).add("About silicon", SYS_ABOUT);
        n = 4;
    }
    bar_menu_count = 1 + n;
}

static void bar_menu_fn(void* ctx, int cmd) {
    if (cmd >= 9001) { sys_command(cmd); return; }
    Window* f = wm_focused();
    if (f && f->app == ctx) { AppCtx actx(f); f->app->on_command(cmd); }
}

void open_bar_menu(int i) {
    build_bar_menus();
    if (i >= bar_menu_count) return;
    Window* f = wm_focused();
    AppCtx actx(nullptr);
    menu_popup(&bar_menus[i], bar_menu_rects[i].x, TOPBAR_H, bar_menu_fn, f ? (void*)f->app : nullptr);
    popup.bar_index = i;
}

void draw_topbar(Canvas& c) {
    if (c.clip.intersect({0, 0, SW, TOPBAR_H}).empty()) return;
    build_bar_menus();
    c.fill_alpha(0, 0, SW, TOPBAR_H, th::bar_bg, 205);
    c.hline(0, TOPBAR_H - 1, SW, blend(th::bar_bg, accent(), 70));
    c.hline(0, 0, SW, blend(th::bar_bg, th::white, 30));
    chip_rect = {4, 0, 28, TOPBAR_H};
    bool chip_open = popup.open && popup.bar_index == 0;
    if (chip_open || chip_rect.contains(ui_mouse_x, ui_mouse_y))
        c.fill(chip_rect.x, 2, chip_rect.w, TOPBAR_H - 4, blend(th::bar_bg, accent(), chip_open ? 200 : 80));
    c.blit(icon("silicon")->small, 10, 4);
    bar_menu_rects[0] = chip_rect;
    Window* f = wm_focused();
    const char* app_name = f ? app_display_name(f->app->app_id()) : "silicon";
    int x = 42;
    c.text(x, 6, app_name, th::white, 1, true);
    x += text_width(app_name) + 22;
    for (int i = 1; i < bar_menu_count; i++) {
        int w = text_width(bar_menus[i].title) + 16;
        bar_menu_rects[i] = {x - 8, 0, w, TOPBAR_H};
        bool open = popup.open && popup.bar_index == i;
        bool hov = !open && bar_menu_rects[i].contains(ui_mouse_x, ui_mouse_y);
        if (open) c.fill(x - 8, 2, w, TOPBAR_H - 4, accent());
        else if (hov) c.fill(x - 8, 2, w, TOPBAR_H - 4, blend(th::bar_bg, accent(), 60));
        c.text(x, 6, bar_menus[i].title, open ? th::white : 0xFFD8D0EA);
        x += w;
    }
    DateTime d;
    rtc_read(d);
    char clock[48];
    int hour = d.hour;
    const char* ampm = "";
    if (!settings.clock24) { ampm = hour >= 12 ? " PM" : " AM"; hour = hour % 12; if (!hour) hour = 12; }
    if (settings.show_seconds) snprintf(clock, sizeof(clock), "%s %d %s  %d:%02d:%02d%s", weekday_name(d.weekday), d.day, month_name(d.month), hour, d.minute, d.second, ampm);
    else snprintf(clock, sizeof(clock), "%s %d %s  %d:%02d%s", weekday_name(d.weekday), d.day, month_name(d.month), hour, d.minute, ampm);
    int cw = text_width(clock);
    int rx = SW - cw - 12;
    clock_rect = {rx - 6, 0, cw + 12, TOPBAR_H};
    if (clock_rect.contains(ui_mouse_x, ui_mouse_y)) c.fill(clock_rect.x, 2, clock_rect.w, TOPBAR_H - 4, blend(th::bar_bg, accent(), 80));
    c.text(rx, 6, clock, th::white);
    rx -= 30;
    search_rect = {rx - 2, 0, 24, TOPBAR_H};
    if (search_rect.contains(ui_mouse_x, ui_mouse_y) || launcher_open) c.fill(search_rect.x, 2, search_rect.w, TOPBAR_H - 4, blend(th::bar_bg, accent(), 80));
    c.blit(icon("launcher")->small, rx + 2, 4);
    // network status: a little signal glyph
    rx -= 26;
    extern bool net_link_up();
    bool up = net_link_up();
    for (int i = 0; i < 4; i++) {
        int bh = 3 + i * 3;
        c.fill(rx + i * 4, 17 - bh, 3, bh, up ? (i < 4 ? 0xFFE8E0FF : 0) : 0xFF4A4262);
    }
    rx -= 50;
    int pct = (int)(heap_used() * 100 / max<u64>(1, heap_total()));
    int lit = clamp((pct * 5 + 99) / 100, 1, 5);
    for (int i = 0; i < 5; i++) c.fill(rx + i * 8, 8, 6, 8, i < lit ? accent() : 0xFF3A3352);
    if (settings.show_fps) {
        char fps[16];
        if (ui_fps() < 6) strlcpy(fps, "idle", sizeof(fps));   // nothing changed on screen: no frames needed
        else snprintf(fps, sizeof(fps), "%d fps", ui_fps());
        rx -= text_width(fps) + 12;
        c.text(rx, 6, fps, ui_fps() >= 50 || ui_fps() < 6 ? 0xFF7FE0A8 : ui_fps() >= 25 ? 0xFFFFD27F : 0xFFFF6B8B, 1, true);
    }
    rx -= text_width(settings.user) + 14;
    c.text(rx, 6, settings.user, 0xFFB8AED0);
}

// ================================================================ launcher (ctrl+space)
bool launcher_open = false;
u64 launcher_t0 = 0;
TextField launcher_field;
int launcher_sel = 0, lcount = 0;
struct LResult { char label[64]; char path[200]; const char* app; const char* icon; };
static LResult lres[9];

static void launcher_collect(FsNode* n, const char* q) {
    for (int i = 0; i < n->children.len && lcount < 9; i++) {
        FsNode* c = n->children[i];
        if (n == fs_root() && !strcmp(c->name, "trash")) continue;
        if (str_icontains(c->name, q)) {
            LResult& r = lres[lcount++];
            strlcpy(r.label, c->name, sizeof(r.label));
            fs_path(c, r.path, sizeof(r.path));
            r.app = nullptr;
            r.icon = c->dir ? "folder" : (str_icontains(c->name, ".pix") || is_image_name(c->name) ? "image_file" : "text_file");
        }
        if (c->dir) launcher_collect(c, q);
    }
}

void launcher_refresh() {
    damage(launcher_rect());
    lcount = 0;
    const char* q = launcher_field.text;
    // "a web address" gets a browse result first
    if (str_starts(q, "http://") || str_starts(q, "https://") || (strchr(q, '.') && !strchr(q, ' ') && !strchr(q, '/') && strlen(q) > 3)) {
        LResult& r = lres[lcount++];
        snprintf(r.label, sizeof(r.label), "open %s", q);
        strlcpy(r.path, q, sizeof(r.path));
        r.app = "browser";
        r.icon = "browser";
    }
    for (int i = 0; i < app_info_count && lcount < 9; i++) {
        if (!q[0] || str_icontains(app_infos[i].name, q) || str_icontains(app_infos[i].id, q)) {
            LResult& r = lres[lcount++];
            strlcpy(r.label, app_infos[i].name, sizeof(r.label));
            r.path[0] = 0;
            r.app = app_infos[i].id;
            r.icon = app_infos[i].id;
        }
    }
    if (q[0]) launcher_collect(fs_root(), q);
    if (q[0] && lcount < 9 && !(lcount && lres[0].app && lres[0].path[0])) {
        LResult& r = lres[lcount++];
        snprintf(r.label, sizeof(r.label), "search the web for \"%s\"", q);
        strlcpy(r.path, q, sizeof(r.path));
        r.app = "browser";
        r.icon = "browser";
    }
    launcher_sel = clamp(launcher_sel, 0, max(0, lcount - 1));
    damage(launcher_rect());
}

void launcher_show() {
    launcher_open = true;
    launcher_t0 = ticks();
    launcher_field.set("");
    launcher_field.focused = true;
    launcher_sel = 0;
    launcher_refresh();
    menu_close();
    damage_all();
}

void launcher_hide() {
    if (!launcher_open) return;
    launcher_open = false;
    damage_all();
}

Rect launcher_rect() { return {(SW - 480) / 2 - 8, 130, 496, 84 + lcount * 34}; }

void launcher_activate(int i) {
    if (i < 0 || i >= lcount) return;
    launcher_hide();
    AppCtx actx(nullptr);
    if (lres[i].app && lres[i].path[0]) launch(lres[i].app, lres[i].path);
    else if (lres[i].app) launch(lres[i].app);
    else open_path(lres[i].path);
}

void draw_launcher(Canvas& c) {
    if (!launcher_open) return;
    int p = ease_out((int)min<u64>(256, (ticks() - launcher_t0) * 256 / 160));
    if (p < 256) damage_all();
    c.fill_alpha(0, TOPBAR_H, SW, SH - TOPBAR_H, 0xFF100C1C, (u32)(120 * p / 256));
    Rect rr = launcher_rect();
    Rect r{rr.x + 8, rr.y + 20 - 20 * p / 256, 480, 64 + lcount * 34};
    c.dither(r.x + 6, r.y + 6, r.w, r.h, th::ink);
    draw_bevel(c, r.x, r.y, r.w, r.h, th::paper);
    c.fill(r.x + 2, r.y + 2, r.w - 4, 4, accent());
    c.blit(icon("launcher")->mid, r.x + 14, r.y + 18);
    draw_bevel(c, r.x + 56, r.y + 16, r.w - 72, 36, th::white, true);
    const char* t = launcher_field.text;
    if (!t[0]) c.text(r.x + 66, r.y + 22, "search apps, files, urls", th::shadow, 2);
    else c.text(r.x + 66, r.y + 22, t, th::text, 2);
    if ((ticks() / 500) % 2 == 0) c.fill(r.x + 66 + launcher_field.cur * 16, r.y + 21, 2, 24, accent());
    for (int i = 0; i < lcount; i++) {
        int y = r.y + 60 + i * 34;
        bool sel = i == launcher_sel;
        if (sel) c.fill(r.x + 8, y, r.w - 16, 32, accent());
        c.blit(icon(lres[i].icon)->mid, r.x + 14, y);
        c.text(r.x + 56, y + 5, lres[i].label, sel ? th::white : th::text, 1, true);
        c.text_n(r.x + 56, y + 18, lres[i].app ? (lres[i].path[0] ? "web" : "application") : lres[i].path, 50,
                 sel ? 0xFFEDE6FF : th::muted);
    }
}

// ================================================================ alt-tab switcher with live previews
bool switcher_open = false;
int switcher_sel = 0;

Rect switcher_rect() {
    int n = windows.len;
    int w = n * 172 + 24, h = 168;
    return {(SW - w) / 2, (SH - h) / 2 - 40, w, h};
}

void draw_switcher(Canvas& c) {
    if (!switcher_open || !windows.len) return;
    int n = windows.len;
    Rect r = switcher_rect();
    c.dither(r.x + 6, r.y + 6, r.w, r.h, th::ink);
    c.fill_alpha(r.x, r.y, r.w, r.h, th::bar_bg, 235);
    c.rect(r.x, r.y, r.w, r.h, accent());
    for (int i = 0; i < n; i++) {
        Window* win = windows[n - 1 - i];
        int ix = r.x + 12 + i * 172;
        bool sel = i == switcher_sel;
        if (sel) { c.fill(ix - 4, r.y + 8, 168, 130, blend(th::bar_bg, accent(), 140)); c.rect(ix - 4, r.y + 8, 168, 130, accent()); }
        // live thumbnail of the window surface, aspect preserved
        if (win->surf.px) {
            int tw = 160, th_ = 104;
            int sw = win->surf.w, sh = win->surf.h;
            if (sw * th_ > sh * tw) th_ = sh * tw / sw; else tw = sw * th_ / sh;
            int tx = ix + (160 - tw) / 2, ty = r.y + 14 + (104 - th_) / 2;
            c.blit_stretch_alpha(win->surf, tx, ty, tw, th_, win->minimized ? 130 : 255);
            c.rect(tx - 1, ty - 1, tw + 2, th_ + 2, th::ink);
        }
        c.blit(icon(win->app->icon_name())->mid, ix + 128, r.y + 96);
        char t[24];
        strlcpy(t, win->title, sizeof(t));
        c.text_centered(ix, r.y + 144, 160, t, sel ? th::white : 0xFFB8AED0, 1, sel);
    }
}

// ================================================================ toasts
struct Toast { char title[40]; char body[96]; const char* icon; u64 start; };
static Vec<Toast> toasts;

void ui_beep(int kind) {
    if (!settings.sounds) return;
    if (kind == 1) speaker_beep(180, 60);
    else if (kind == 2) { speaker_beep(660, 40); speaker_beep(990, 60); }
    else speaker_beep(1200, 8);
}

void ui_notify(const char* title, const char* body, const char* icon_name) {
    Toast t;
    strlcpy(t.title, title, sizeof(t.title));
    strlcpy(t.body, body, sizeof(t.body));
    t.icon = icon_name;
    t.start = ticks();
    if (toasts.len >= 4) toasts.remove(0);
    toasts.push(t);
    damage(toast_area());
}

Rect toast_area() { return {SW - 300, TOPBAR_H, 300, 4 * 62 + 20}; }
// expire old toasts here (outside compose, so the damage they leave behind isn't lost)
bool toasts_active() {
    u64 t = ticks();
    for (int i = toasts.len - 1; i >= 0; i--)
        if (t - toasts[i].start > 4400) { toasts.remove(i); damage(toast_area()); }
    return toasts.len > 0;
}

void draw_toasts(Canvas& c) {
    u64 t = ticks();
    if (c.clip.intersect(toast_area()).empty()) return;
    for (int i = 0; i < toasts.len; i++) {
        Toast& to = toasts[i];
        int el = (int)(t - to.start);
        // slide in with ease-out, slide out with ease-in
        int slide = 0;
        if (el < 220) slide = 300 - 300 * ease_out(el * 256 / 220) / 256;
        else if (el > 4000) { int p = (el - 4000) * 256 / 400; slide = 300 * p * p / (256 * 256); }
        int w = 280, h = 54;
        int x = SW - w - 10 + slide, y = TOPBAR_H + 10 + i * (h + 8);
        c.dither(x + 4, y + 4, w, h, th::ink);
        draw_bevel(c, x, y, w, h, th::paper);
        c.fill(x + 2, y + 2, 3, h - 4, accent());
        c.blit(icon(to.icon)->mid, x + 12, y + 11);
        c.text(x + 52, y + 9, to.title, th::text, 1, true);
        const char* b = to.body;
        for (int line = 0; line < 2 && *b; line++) {
            int n = (int)strlen(b);
            if (n > 27) { n = 27; while (n > 0 && b[n] != ' ') n--; if (!n) n = 27; }
            c.text_n(x + 52, y + 24 + line * 13, b, n, th::muted);
            b += n;
            while (*b == ' ') b++;
        }
        // a thin progress line shows how long the toast stays
        int life = clamp(4000 - el, 0, 4000);
        c.hline(x + 6, y + h - 3, (w - 12) * life / 4000, blend(th::paper, accent(), 120));
    }
}

// ================================================================ desktop icons (contents of /home)
int desk_sel = -1;

static Rect desk_cell(int i) {
    int rows = max(1, (SH - TOPBAR_H - 130) / 86);
    int col = i / rows, row = i % rows;
    return {14 + col * 90, TOPBAR_H + 14 + row * 86, 86, 84};
}

void draw_desktop_icons(Canvas& c) {
    FsNode* home = fs_home();
    if (!home) return;
    fs_sort(home);
    for (int i = 0; i < home->children.len; i++) {
        Rect r = desk_cell(i);
        if (c.clip.intersect(r).empty()) continue;
        FsNode* n = home->children[i];
        const char* ic = n->dir ? (n->children.len ? "folder_full" : "folder")
                       : str_icontains(n->name, ".pix") || is_image_name(n->name) ? "image_file"
                       : str_icontains(n->name, ".htm") ? "browser" : "text_file";
        Image& img = icon(ic)->big;
        int ix = r.x + (r.w - 64) / 2;
        bool sel = i == desk_sel;
        bool hov = !sel && r.contains(ui_mouse_x, ui_mouse_y) && !wm_focused_covers(ui_mouse_x, ui_mouse_y);
        if (sel) c.fill_alpha(ix - 3, r.y - 3, 70, 70, accent(), 110);
        else if (hov) c.fill_alpha(ix - 3, r.y - 3, 70, 70, th::white, 40);
        c.blit(img, ix, r.y);
        char shown[24];
        strlcpy(shown, n->name, sizeof(shown));
        if (strlen(n->name) > 11) { shown[9] = 0; strlcat(shown, G_ELLIP, sizeof(shown)); }
        int tw = text_width(shown);
        int tx = r.x + (r.w - tw) / 2;
        if (sel) c.fill(tx - 3, r.y + 67, tw + 6, 14, accent());
        else c.fill_alpha(tx - 3, r.y + 67, tw + 6, 14, th::bar_bg, 120);
        c.text(tx, r.y + 68, shown, th::white);
    }
}

int desk_hit(int x, int y) {
    FsNode* home = fs_home();
    if (!home) return -1;
    for (int i = 0; i < home->children.len; i++) if (desk_cell(i).contains(x, y)) return i;
    return -1;
}

enum { DESK_OPEN = 1, DESK_TRASH, DESK_NEW_FOLDER, DESK_NEW_FILE, DESK_WALL, DESK_TERM, DESK_SETTINGS, DESK_OVERVIEW };
static FsNode* desk_menu_node = nullptr;

static void desk_menu_fn(void*, int cmd) {
    char p[256];
    switch (cmd) {
    case DESK_OPEN: if (desk_menu_node) { fs_path(desk_menu_node, p, sizeof(p)); open_path(p); } break;
    case DESK_TRASH:
        if (desk_menu_node) {
            ui_notify("Moved to Trash", desk_menu_node->name, "trash");
            fs_trash(desk_menu_node);
            desk_sel = -1;
        }
        break;
    case DESK_NEW_FOLDER: sys_command(SYS_NEW_FOLDER); break;
    case DESK_NEW_FILE: sys_command(SYS_NEW_FILE); break;
    case DESK_WALL: launch("wallpapers"); break;
    case DESK_TERM: launch("terminal"); break;
    case DESK_SETTINGS: launch("settings"); break;
    case DESK_OVERVIEW: overview_toggle(); break;
    }
    damage_all();
}

void desk_click(int i, int button, bool dbl, int mx, int my) {
    desk_sel = i;
    desk_menu_node = fs_home()->children[i];
    damage({0, TOPBAR_H, 400, SH - TOPBAR_H});
    if (dbl && button == 1) { char p[256]; fs_path(desk_menu_node, p, sizeof(p)); open_path(p); }
    else if (button == 2) {
        Menu m;
        m.add(desk_menu_node->name, 0, "", false).sep().add("Open", DESK_OPEN).add("Move to Trash", DESK_TRASH);
        menu_popup(&m, mx, my, desk_menu_fn, nullptr);
    }
}

void desk_background_menu(int mx, int my) {
    Menu m;
    m.add("New folder", DESK_NEW_FOLDER).add("New text file", DESK_NEW_FILE).sep()
     .add("Open terminal", DESK_TERM).add("Overview", DESK_OVERVIEW, "f3").add("Change wallpaper" G_ELLIP, DESK_WALL)
     .add("Settings" G_ELLIP, DESK_SETTINGS);
    menu_popup(&m, mx, my, desk_menu_fn, nullptr);
}

void desk_key(const KeyEvent& e) {
    FsNode* home = fs_home();
    int n = home ? home->children.len : 0;
    if (!n) return;
    damage({0, TOPBAR_H, 400, SH - TOPBAR_H});
    if (e.key == K_DOWN || e.key == K_RIGHT) desk_sel = (desk_sel + 1) % n;
    else if (e.key == K_UP || e.key == K_LEFT) desk_sel = (desk_sel + n - 1) % n;
    else if (e.key == K_ENTER && desk_sel >= 0) { char p[256]; fs_path(home->children[desk_sel], p, sizeof(p)); open_path(p); }
    else if (e.key == K_DELETE && desk_sel >= 0) { fs_trash(home->children[desk_sel]); desk_sel = -1; }
}

// ================================================================ system commands
void sys_command(int cmd) {
    char name[FS_NAME_MAX];
    AppCtx actx(nullptr);
    switch (cmd) {
    case SYS_ABOUT: launch("about"); break;
    case SYS_SETTINGS: launch("settings"); break;
    case SYS_LAUNCHER: launcher_show(); break;
    case SYS_OVERVIEW: overview_toggle(); break;
    case SYS_SAVER: saver_start(); break;
    case SYS_MONITOR: launch("monitor"); break;
    case SYS_BROWSER: launch("browser"); break;
    case SYS_TERMINAL_MODE: desk_running = false; break;
    case SYS_RESTART: launch("power", "restart"); break;
    case SYS_SHUTDOWN: launch("power", "shutdown"); break;
    case SYS_NEW_TERMINAL: launch("__new__", "terminal"); break;
    case SYS_NEW_FOLDER:
        fs_unique_name(fs_home(), "new folder", name, sizeof(name));
        fs_create(fs_home(), name, true);
        ui_notify("Folder created", name, "folder");
        break;
    case SYS_NEW_FILE:
        fs_unique_name(fs_home(), "untitled.txt", name, sizeof(name));
        fs_create(fs_home(), name, false);
        ui_notify("File created", name, "text_file");
        break;
    case SYS_WALLPAPER: launch("wallpapers"); break;
    case SYS_NEXT_WALL: wallpaper_set((settings.wallpaper + 1) % wallpaper_count()); break;
    case SYS_GO_HOME: open_path("/home"); break;
    case SYS_GO_NOTES: open_path("/home/notes"); break;
    case SYS_GO_PICS: open_path("/home/pictures"); break;
    case SYS_GO_ROOT: open_path("/"); break;
    case SYS_GO_TRASH: launch("trash"); break;
    case SYS_TIPS:
        ui_notify("Tip", "ctrl+space opens the launcher", "launcher");
        ui_notify("Tip", "f3 shows every window at once", "silicon");
        ui_notify("Tip", "drag a window to a screen edge to snap it", "silicon");
        break;
    case SYS_MIN_ALL: for (int i = 0; i < windows.len; i++) wm_minimize(windows[i]); break;
    case SYS_CLOSE_WIN: if (wm_focused()) { AppCtx c2(wm_focused()); wm_focused()->app->close(); } break;
    }
    damage_all();
}
