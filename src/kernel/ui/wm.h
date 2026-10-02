// silicon - desktop internals: windows, apps, menus, widgets, theme
#pragma once
#include "../core.h"
#include "../gfx/gfx.h"
#include "../drv/drivers.h"
#include "../fs/fs.h"
#include "../settings.h"
#include "ui.h"

// ---------------------------------------------------------------- theme
namespace th {
static const u32 ink      = 0xFF1A1428;
static const u32 paper    = 0xFFEEEAF6;
static const u32 panel    = 0xFFE0DAEE;
static const u32 panel_dk = 0xFFCBC3DF;
static const u32 border   = 0xFF8A7EA8;
static const u32 text     = 0xFF221C30;
static const u32 muted    = 0xFF746A8C;
static const u32 white    = 0xFFFFFFFF;
static const u32 hilite   = 0xFFFFFFFF;
static const u32 shadow   = 0xFF9C92B8;
static const u32 red      = 0xFFE5486A;
static const u32 green    = 0xFF3FB27F;
static const u32 gold     = 0xFFECBC5C;
static const u32 gold_dk  = 0xFFAA7830;
static const u32 bar_bg   = 0xFF141020;
}

static const int TOPBAR_H = 24;
static const int TITLE_H = 24;
static const int BORDER = 3;

// ---------------------------------------------------------------- assets
struct Icon {
    const char* name;
    Image big;      // 64x64
    Image mid;      // 32x32
    Image small;    // 16x16
    Image dock48;   // 48x48, made on demand when the dock is crowded
};
Icon* icon(const char* name);          // never null (falls back to "file")
struct Wallpaper { const char* name; Image full; Image thumb; bool custom; };
int  wallpaper_count();
Wallpaper* wallpaper_get(int i);
void wallpaper_set(int i);
int  wallpaper_add_custom(const char* name, Image full);   // e.g. a Pixel painting

// ---------------------------------------------------------------- input
enum MouseKind { M_MOVE, M_DOWN, M_UP, M_DBL, M_WHEEL, M_LEAVE };
struct MouseEv {
    MouseKind kind;
    int x, y;          // relative to the receiver
    int button;        // 1 left, 2 right, 3 middle
    int wheel;
    bool left;         // left button held
};

enum CursorShape { CUR_ARROW, CUR_HAND, CUR_IBEAM, CUR_MOVE, CUR_RESIZE, CUR_CROSS, CUR_WAIT };
extern CursorShape ui_cursor;
void ui_invalidate();                  // inside an app call: that window re-renders; else full repaint
void ui_invalidate_all();              // every window re-renders (theme / global state changed)
int  ui_fps();
void ui_notify(const char* title, const char* body, const char* icon_name = "silicon");
void ui_beep(int kind);                // 0 click, 1 error, 2 success

// ---------------------------------------------------------------- menus
typedef void (*MenuFn)(void* ctx, int cmd);
struct MenuItem {
    char label[40];
    char shortcut[16];
    int  cmd;
    bool enabled;
    bool separator;
    bool checked;
};
struct Menu {
    char title[24];
    MenuItem items[20];
    int count = 0;
    void clear() { count = 0; }
    Menu& add(const char* label, int cmd, const char* shortcut = "", bool enabled = true, bool checked = false);
    Menu& sep();
};
void menu_popup(Menu* m, int x, int y, MenuFn fn, void* ctx);   // context menu at screen position

// ---------------------------------------------------------------- apps and windows
struct Window;

class App {
public:
    Window* win = nullptr;
    virtual ~App() {}
    virtual const char* app_id() = 0;                 // e.g. "files"
    virtual const char* title() = 0;
    virtual const char* icon_name() { return app_id(); }
    virtual void draw(Canvas& c, int w, int h) = 0;   // origin = client top-left
    virtual void on_mouse(const MouseEv& e) {}
    virtual void on_key(const KeyEvent& e) {}
    virtual bool wants_key_up() { return false; }       // games: also deliver key releases
    virtual void tick() {}
    virtual bool can_close() { return true; }         // return false to veto (show a dialog)
    virtual void on_resize(int w, int h) {}
    virtual int  menus(Menu* out) { return 0; }       // fill up to 4 menus for the top bar
    virtual void on_command(int cmd) {}
    virtual u32  background() { return th::paper; }
    virtual int  pref_w() { return 480; }
    virtual int  pref_h() { return 320; }
    virtual bool resizable() { return true; }
    virtual void open_arg(const char* arg) {}         // e.g. a path for a running app
    virtual bool wants_ibeam(int x, int y) { return false; }
    virtual bool hover_effects() { return true; }      // re-render while the mouse moves over it
    virtual int  cursor_at(int x, int y) { return wants_ibeam(x, y) ? 2 : 0; }   // a CursorShape

    // modal helpers drawn over the client area
    void ask(const char* msg, const char* b1, const char* b2, const char* b3, void (*cb)(App*, int));
    void prompt(const char* msg, const char* initial, void (*cb)(App*, const char*));
    void info(const char* msg);
    struct Dialog* dialog = nullptr;

    bool hot(int x, int y, int w, int h);             // live mouse inside a client-relative rect
    void close();                                     // request close (honours can_close)
    void force_close();
    void set_title(const char* t);
};

enum WinAnim { A_NONE, A_OPEN, A_CLOSE, A_MIN, A_RESTORE, A_GEOM };

struct Window {
    App*  app;
    Rect  r;                 // outer frame on screen
    Rect  restore;           // geometry before maximize / snap
    bool  minimized = false, maximized = false;
    bool  resizable = true;
    int   min_w = 200, min_h = 120;
    char  title[64];
    u64   opened_at = 0;
    // compositor state
    Image surf;              // the whole window (frame + client) rendered off-screen
    bool  dirty = true;
    int   anim = A_NONE;
    u64   anim_t0 = 0;
    u32   anim_dur = 0;
    Rect  anim_from{0, 0, 0, 0}, anim_to{0, 0, 0, 0};
    Image anim_img;          // snapshot used while the geometry morphs
    Rect client() const { return {r.x + BORDER, r.y + TITLE_H, r.w - 2 * BORDER, r.h - TITLE_H - BORDER}; }
};

Window* wm_open(App* app, int w, int h, int x = -1, int y = -1);
void    wm_close(Window* w);
void    wm_focus(Window* w);
Window* wm_focused();
Window* wm_find(const char* app_id);
int     wm_count();
Window* wm_get(int i);
void    wm_minimize(Window* w);
void    wm_toggle_max(Window* w);
void    wm_set_geometry(Window* w, Rect r);
void    wallpaper_set(int i);

// app launching (apps.cpp)
App*  launch(const char* app_id, const char* arg = nullptr);    // focuses existing single-instance apps
void  open_path(const char* path);                               // open a file/dir with the right app
const char* app_display_name(const char* app_id);
struct AppInfo { const char* id; const char* name; const char* desc; bool single; bool in_dock; };
extern const AppInfo app_infos[];
extern const int app_info_count;

// ---------------------------------------------------------------- widgets (widgets.cpp)
void draw_bevel(Canvas& c, int x, int y, int w, int h, u32 face, bool sunken = false);
void draw_button(Canvas& c, int x, int y, int w, int h, const char* label, bool hot,
                 bool primary = false, bool enabled = true);
void draw_checkbox(Canvas& c, int x, int y, const char* label, bool on);
void draw_scrollbar(Canvas& c, int x, int y, int h, int total, int visible, int offset);
void draw_panel(Canvas& c, int x, int y, int w, int h, const char* title = nullptr);
void draw_progress(Canvas& c, int x, int y, int w, int h, int pct, u32 color);
void draw_icon_label(Canvas& c, Icon* ic, int x, int y, int cell_w, const char* label, bool selected, bool small);
extern int ui_mouse_x, ui_mouse_y;   // screen coords
extern bool ui_mouse_left;
bool hover_in(Window* w, int x, int y, int ww, int hh);   // client-relative hit test against the live mouse

struct TextField {
    char text[128];
    int  len = 0, cur = 0;
    bool focused = false;
    void set(const char* s) { strlcpy(text, s, sizeof(text)); len = cur = (int)strlen(text); }
    bool on_key(const KeyEvent& e);    // true if consumed; Enter is not consumed
    void draw(Canvas& c, int x, int y, int w);
};

// dialogs (widgets.cpp)
void dialog_draw(App* a, Canvas& c, int w, int h);
void dialog_mouse(App* a, const MouseEv& e, int w, int h);
void dialog_key(App* a, const KeyEvent& e);

// smooth scrolling: set target, call step() every tick, read pos()
struct Smooth {
    int target = 0;
    int p16 = 0;             // position * 16
    void jump(int v) { target = v; p16 = v * 16; }
    void set(int v) { target = v; }
    int  pos() const { return p16 / 16; }
    bool step() {
        int goal = target * 16;
        if (p16 == goal) return false;
        int d = (goal - p16) / 4;
        if (d == 0) d = goal > p16 ? 1 : -1;
        p16 += d;
        if (iabs(goal - p16) < 4) p16 = goal;
        return true;
    }
};

// draggable scrollbar: draws at (x,y) with height h. handle() returns true if the event was consumed;
// it updates offset (clamped to 0..total-visible)
struct ScrollBar {
    bool dragging = false;
    int grab = 0;
    void draw(Canvas& c, int x, int y, int h, int total, int visible, int offset);
    bool handle(const MouseEv& e, int x, int y, int h, int total, int visible, int& offset);
};

// number formatting helpers
void fmt_bytes(u64 b, char* out, int n);
void fmt_double(double v, char* out, int n, int max_decimals = 8);
