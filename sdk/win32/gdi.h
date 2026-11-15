// win32run - GDI and USER: what gdi32.cpp and user32.cpp share.
#pragma once
#include "rt.h"

namespace rt {

struct POINT { i32 x, y; };
struct SIZE { i32 cx, cy; };
struct RECT { i32 left, top, right, bottom; };
typedef HANDLE HWND;
typedef HANDLE HDC;
typedef HANDLE HGDIOBJ;
typedef u32 COLORREF;

// ---------------------------------------------------------------- pixel surfaces
// Pixels are kept in the program's own format (a DIB section is memory the program writes into), so a
// surface describes any of 1/4/8/16/24/32 bits per pixel, top-down or bottom-up (negative stride).
struct Surf {
    u8*  bits = nullptr;          // the top row
    int  w = 0, h = 0;
    int  stride = 0;              // bytes from one row to the next (negative for bottom-up storage)
    int  bpp = 32;
    u32  rmask = 0xFF0000, gmask = 0xFF00, bmask = 0xFF;    // 16 and 32 bpp bit fields
    u32* palette = nullptr;       // 1/4/8 bpp: 0x00RRGGBB entries
    int  ncolors = 0;
};
u32  surf_get(const Surf& s, int x, int y);              // 0x00RRGGBB
void surf_put(Surf& s, int x, int y, u32 rgb);
static inline u32* row32(const Surf& s, int y) { return (u32*)(s.bits + (i64)y * s.stride); }
static inline u32 rgb_of(COLORREF c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }
static inline COLORREF cref_of(u32 rgb) { return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF); }

// ---------------------------------------------------------------- GDI objects
enum GdiType { G_NONE, G_BRUSH, G_PEN, G_FONT, G_BITMAP, G_DC, G_REGION, G_PALETTE };
struct GdiObj { int type; bool stock; HGDIOBJ handle; };
struct Bitmap;
struct Brush : GdiObj { int style; COLORREF color; int hatch; Bitmap* pattern; };
struct Pen : GdiObj { int style; int width; COLORREF color; };
struct LOGFONTW {
    i32 lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    u8 lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    WCHAR lfFaceName[32];
};
struct Font : GdiObj { LOGFONTW lf; int px100; bool bold, mono; int asc, desc, lh, avgw, maxw; };
struct Bitmap : GdiObj { Surf s; bool own_bits; bool dib; u8 bih[40]; void* selected_in; };
struct Region : GdiObj { RECT r; };                     // rectangles only (the bounding box of anything else)
struct Palette : GdiObj { u32 entries[256]; int n; };
struct Wnd;
enum { DCK_WINDOW, DCK_MEMORY, DCK_SCREEN };
struct DCState {
    Brush* brush; Pen* pen; Font* font; Bitmap* bmp; Palette* pal;
    COLORREF text_color, bk_color, dc_brush, dc_pen;
    int bk_mode, rop2, text_align, stretch_mode, map_mode, poly_mode, char_extra;
    POINT pos, vp_org, win_org, brush_org;
    bool has_clip; RECT clip;                           // the program's clip rectangle, device coordinates
};
struct DC : GdiObj {
    int kind;
    Wnd* wnd;                                           // window DCs
    bool paint;                                         // from BeginPaint: clipped to the update rectangle
    RECT paint_rect;
    bool own;                                           // CS_OWNDC / CS_CLASSDC: ReleaseDC keeps it
    DCState st;
    DCState saved[16];
    int nsaved;
};
GdiObj* gdi_get(HGDIOBJ h, int type = G_NONE);
HGDIOBJ gdi_add(GdiObj* o);
DC*     dc_get(HDC h);
HDC     dc_new(int kind, Wnd* wnd);
void    dc_free(DC* dc);
// where a DC draws: the surface, the device offset of logical (0,0), and the device clip rectangle
bool    dc_target(DC* dc, Surf* s, int* ox, int* oy, RECT* clip);
Font*   font_default();
void    text_metrics(Font* f, int* asc, int* desc, int* lh, int* avgw, int* maxw);
int     text_width_utf8(Font* f, const char* s, int n);
void    draw_text_utf8(DC* dc, int x, int y, const char* s, int n, const RECT* opaque, int* out_w);   // x, y: logical, top-left of the text
void    fill_rect_dc(DC* dc, const RECT* r, Brush* b);
HGDIOBJ stock_object(int i);

// ---------------------------------------------------------------- windows (user32.cpp)
struct Cls;
typedef LRESULT (WINAPI *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
struct Wnd {
    HWND  h;
    Cls*  cls;
    WNDPROC proc;
    bool  unicode;                // the window procedure wants W strings
    u32   style, exstyle;
    Wnd*  parent;                 // child windows (WS_CHILD)
    Wnd*  owner;
    Wnd*  first_child;
    Wnd*  next_sibling;
    RECT  rect;                   // the window rectangle: screen coordinates for top-level, parent client coordinates for children
    int   cw, ch;                 // client size
    int   nc_l, nc_t, nc_r, nc_b; // non-client margins
    int   kwin;                   // top-level: silicon's window id
    Surf  surf;                   // top-level: the window's pixels (32 bpp, shared with the desktop)
    u32   surf_cap;               // pixels the shared buffer can hold
    bool  dirty;                  // drawn to since the last present
    WCHAR* text;
    i64   userdata;
    u8*   extra;
    int   nextra;
    u64   id;                     // child id / menu
    HINSTANCE inst;
    RECT  inval;
    bool  need_paint, erase;
    bool  visible, enabled, destroyed, minimized, maximized;
    u32   tid;                    // the thread that made it (its messages go there)
    HANDLE icon, icon_small;
    DC*   own_dc;
    HANDLE menu;
    struct Prop { WCHAR* name; HANDLE value; Prop* next; } *props;
    i32   scroll[2][4];           // SB_HORZ, SB_VERT: min, max, page, pos
    bool  track_leave;
};
Wnd*  wnd_get(HWND h);
Wnd*  wnd_top(Wnd* w);            // the top-level window a window is drawn into
void  wnd_client_origin(Wnd* w, int* x, int* y);        // client (0,0) inside the top-level surface
void  wnd_mark_dirty(Wnd* w);
void  present_all();
u32   sys_color(int i);

}  // namespace rt
