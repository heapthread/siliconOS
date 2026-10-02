// silicon - state shared by the desktop modules (desktop.cpp, dock.cpp, chrome.cpp)
#pragma once
#include "wm.h"

extern int SW, SH;
extern Vec<Window*> windows;            // z-order, last = top
extern bool desk_running;

// ---------------------------------------------------------------- damage
void damage(Rect r);
void damage_all();
void damage_window(Window* w);          // window rect plus its shadow
Rect window_bounds(Window* w);          // what a window covers on screen right now (anims included)

// ---------------------------------------------------------------- app dispatch context
extern Window* cur_win;
struct AppCtx {
    Window* prev;
    explicit AppCtx(Window* w) : prev(cur_win) { cur_win = w; }
    ~AppCtx() { cur_win = prev; }
};

// ---------------------------------------------------------------- easing (p in 0..256)
static inline int ease_out(int p) { p = clamp(p, 0, 256); return 256 - (256 - p) * (256 - p) / 256; }
static inline int ease_in_out(int p) {
    p = clamp(p, 0, 256);
    return p < 128 ? 2 * p * p / 256 : 256 - 2 * (256 - p) * (256 - p) / 256;
}
static inline int lerp(int a, int b, int p) { return a + (b - a) * p / 256; }
static inline Rect lerp_rect(Rect a, Rect b, int p) {
    return {lerp(a.x, b.x, p), lerp(a.y, b.y, p), lerp(a.w, b.w, p), lerp(a.h, b.h, p)};
}
static inline Rect rect_union(Rect a, Rect b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    int x0 = min(a.x, b.x), y0 = min(a.y, b.y);
    int x1 = max(a.x + a.w, b.x + b.w), y1 = max(a.y + a.h, b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}
static inline Rect grow(Rect r, int n) { return {r.x - n, r.y - n, r.w + 2 * n, r.h + 2 * n}; }

// ---------------------------------------------------------------- dock (dock.cpp)
struct DockItem { const char* app_id; Rect r; int lift16; u64 bounce_t; };
extern DockItem dock_items[24];
extern int dock_count;
extern int dock_hover;
extern int dock_press;
extern Rect dock_rect;
extern u64 dock_anim_t;
Rect dock_area();                       // dock plus pins, labels and lift headroom
void dock_layout();
bool dock_animate();                    // true while something moves
void draw_dock(Canvas& c);
int  dock_hit(int x, int y);
void dock_click(int i);
void dock_context(int i);
Rect dock_icon_rect(const char* id);

// ---------------------------------------------------------------- chrome (chrome.cpp)
struct MenuState {
    bool open = false;
    Menu menu;
    int x, y, hover = -1;
    MenuFn fn = nullptr;
    void* ctx = nullptr;
    Window* ctx_win = nullptr;          // app window to run the callback in
    int bar_index = -1;                 // >= 0 when opened from the top bar
    u64 t0 = 0;
};
extern MenuState popup;
Rect menu_rect();
int  menu_hit(int mx, int my);
void draw_menu(Canvas& c);
void menu_close();
void menu_activate(int item);

extern Rect bar_menu_rects[5];
extern int bar_menu_count;
extern Rect chip_rect, clock_rect, search_rect;
void open_bar_menu(int i);
void draw_topbar(Canvas& c);

extern bool launcher_open;
extern u64 launcher_t0;
extern TextField launcher_field;
extern int launcher_sel, lcount;
Rect launcher_rect();
void launcher_show();
void launcher_hide();
void launcher_refresh();
void launcher_activate(int i);
void draw_launcher(Canvas& c);

extern bool switcher_open;
extern int switcher_sel;
Rect switcher_rect();
void draw_switcher(Canvas& c);

Rect toast_area();
bool toasts_active();
void draw_toasts(Canvas& c);

extern int desk_sel;
bool wm_focused_covers(int x, int y);
int  desk_hit(int x, int y);
void draw_desktop_icons(Canvas& c);
void desk_click(int i, int button, bool dbl, int mx, int my);
void desk_background_menu(int mx, int my);
void desk_key(const KeyEvent& e);

void draw_cursor(Canvas& c);
Rect cursor_rect();

enum {
    SYS_ABOUT = 9001, SYS_SETTINGS, SYS_LAUNCHER, SYS_TERMINAL_MODE, SYS_RESTART, SYS_SHUTDOWN,
    SYS_NEW_FOLDER, SYS_NEW_FILE, SYS_WALLPAPER, SYS_NEXT_WALL, SYS_GO_HOME, SYS_GO_PICS, SYS_GO_NOTES,
    SYS_GO_TRASH, SYS_GO_ROOT, SYS_TIPS, SYS_NEW_TERMINAL, SYS_MONITOR, SYS_MIN_ALL, SYS_CLOSE_WIN,
    SYS_OVERVIEW, SYS_BROWSER, SYS_SAVER,
};
void saver_start();                     // screensaver (desktop.cpp)
void sys_command(int cmd);

// ---------------------------------------------------------------- overview (desktop.cpp)
extern bool overview_on;
void overview_toggle();
