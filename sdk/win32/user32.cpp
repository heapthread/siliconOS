// win32run - user32: window classes, windows, the message queue, painting, keyboard and mouse input,
// timers, MessageBox, the clipboard and a few standard controls.
//
// Every top-level window is a window of silicon's desktop (win_create): the runtime draws its client area
// into a pixel buffer it shares with the desktop, which puts silicon's own frame and title bar around it.
// Child windows are drawn into their top-level window's buffer. Input arrives as silicon events and is
// turned into WM_ messages for the thread that owns the window.
#include "gdi.h"
#include "crt.h"

namespace rt {

// ---------------------------------------------------------------- constants
enum : u32 {
    WM_NULL = 0, WM_CREATE = 1, WM_DESTROY = 2, WM_MOVE = 3, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_KILLFOCUS = 8,
    WM_ENABLE = 0xA, WM_SETREDRAW = 0xB, WM_SETTEXT = 0xC, WM_GETTEXT = 0xD, WM_GETTEXTLENGTH = 0xE, WM_PAINT = 0xF, WM_CLOSE = 0x10,
    WM_QUERYENDSESSION = 0x11, WM_QUIT = 0x12, WM_ERASEBKGND = 0x14, WM_SYSCOLORCHANGE = 0x15, WM_SHOWWINDOW = 0x18,
    WM_ACTIVATEAPP = 0x1C, WM_SETCURSOR = 0x20, WM_MOUSEACTIVATE = 0x21, WM_GETMINMAXINFO = 0x24, WM_SETFONT = 0x30, WM_GETFONT = 0x31,
    WM_WINDOWPOSCHANGING = 0x46, WM_WINDOWPOSCHANGED = 0x47, WM_NOTIFY = 0x4E, WM_GETICON = 0x7F, WM_SETICON = 0x80,
    WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_NCCALCSIZE = 0x83, WM_NCHITTEST = 0x84, WM_NCPAINT = 0x85, WM_NCACTIVATE = 0x86,
    WM_GETDLGCODE = 0x87, WM_NCMOUSEMOVE = 0xA0, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_DEADCHAR = 0x103,
    WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105, WM_SYSCHAR = 0x106, WM_UNICHAR = 0x109, WM_INITDIALOG = 0x110, WM_COMMAND = 0x111,
    WM_SYSCOMMAND = 0x112, WM_TIMER = 0x113, WM_HSCROLL = 0x114, WM_VSCROLL = 0x115, WM_INITMENU = 0x116, WM_INITMENUPOPUP = 0x117,
    WM_CTLCOLORMSGBOX = 0x132, WM_CTLCOLOREDIT = 0x133, WM_CTLCOLORLISTBOX = 0x134, WM_CTLCOLORBTN = 0x135, WM_CTLCOLORDLG = 0x136,
    WM_CTLCOLORSCROLLBAR = 0x137, WM_CTLCOLORSTATIC = 0x138, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202,
    WM_LBUTTONDBLCLK = 0x203, WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_RBUTTONDBLCLK = 0x206, WM_MBUTTONDOWN = 0x207,
    WM_MBUTTONUP = 0x208, WM_MOUSEWHEEL = 0x20A, WM_PARENTNOTIFY = 0x210, WM_ENTERSIZEMOVE = 0x231, WM_EXITSIZEMOVE = 0x232,
    WM_MOUSELEAVE = 0x2A3, WM_DPICHANGED = 0x2E0, WM_USER = 0x400, WM_APP = 0x8000,
};
enum : u32 { WS_OVERLAPPED = 0, WS_POPUP = 0x80000000u, WS_CHILD = 0x40000000, WS_MINIMIZE = 0x20000000, WS_VISIBLE = 0x10000000,
             WS_DISABLED = 0x08000000, WS_CLIPSIBLINGS = 0x04000000, WS_CLIPCHILDREN = 0x02000000, WS_MAXIMIZE = 0x01000000,
             WS_CAPTION = 0x00C00000, WS_BORDER = 0x00800000, WS_DLGFRAME = 0x00400000, WS_VSCROLL = 0x00200000, WS_HSCROLL = 0x00100000,
             WS_SYSMENU = 0x00080000, WS_THICKFRAME = 0x00040000, WS_MINIMIZEBOX = 0x20000, WS_MAXIMIZEBOX = 0x10000 };
static const i32 CW_USEDEFAULT = (i32)0x80000000;
struct MSG { HWND hwnd; u32 message; u32 pad; WPARAM wParam; LPARAM lParam; u32 time; POINT pt; u32 lPrivate; };
struct CREATESTRUCTW { void* lpCreateParams; HINSTANCE hInstance; HANDLE hMenu; HWND hwndParent; i32 cy, cx, y, x; i32 style; const WCHAR* lpszName; const WCHAR* lpszClass; u32 dwExStyle; };
struct PAINTSTRUCT { HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore, fIncUpdate; u8 rgbReserved[32]; };
static inline LPARAM MAKELP(int lo, int hi) { return (LPARAM)(u32)(((u32)(u16)lo) | ((u32)(u16)hi << 16)); }

// ---------------------------------------------------------------- classes
struct Cls {
    WCHAR name[128];
    u16 atom;
    u32 style;
    WNDPROC proc;
    bool unicode;
    int cls_extra, wnd_extra;
    HINSTANCE inst;
    HANDLE icon, icon_sm, cursor, brush;
    u8* extra;
    Cls* next;
};
static Cls* classes = nullptr;
static u16 next_atom = 0xC000;
static Lock user_lock;

static void cls_name_from(const void* name, bool unicode, WCHAR* out) {
    if ((u64)name < 0x10000) { out[0] = 0; return; }
    if (unicode) { usize n = wlen((const WCHAR*)name); if (n > 127) n = 127; memcpy(out, name, n * 2); out[n] = 0; }
    else wide_from_utf8((const char*)name, -1, out, 128);
}
static Cls* cls_find(const void* name, bool unicode) {
    if ((u64)name < 0x10000) { for (Cls* c = classes; c; c = c->next) if (c->atom == (u16)(u64)name) return c; return nullptr; }
    WCHAR n[128];
    cls_name_from(name, unicode, n);
    for (Cls* c = classes; c; c = c->next) if (!wicmp(c->name, n)) return c;
    return nullptr;
}
struct WNDCLASSEXW { u32 cbSize, style; WNDPROC lpfnWndProc; i32 cbClsExtra, cbWndExtra; HINSTANCE hInstance; HANDLE hIcon, hCursor, hbrBackground; const void* lpszMenuName; const void* lpszClassName; HANDLE hIconSm; };
struct WNDCLASSW { u32 style; WNDPROC lpfnWndProc; i32 cbClsExtra, cbWndExtra; HINSTANCE hInstance; HANDLE hIcon, hCursor, hbrBackground; const void* lpszMenuName; const void* lpszClassName; };
static u16 register_class(const WNDCLASSEXW* wc, bool unicode) {
    if (!wc || !wc->lpszClassName) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    Guard g(user_lock);
    Cls* old = cls_find(wc->lpszClassName, unicode);
    if (old && old->inst == wc->hInstance) { set_error(ERROR_CLASS_ALREADY_EXISTS); return 0; }
    Cls* c = (Cls*)izalloc(sizeof(Cls));
    cls_name_from(wc->lpszClassName, unicode, c->name);
    if ((u64)wc->lpszClassName < 0x10000) snfmt((char*)c->name, 0, "");
    c->atom = next_atom++;
    c->style = wc->style;
    c->proc = wc->lpfnWndProc;
    c->unicode = unicode;
    c->cls_extra = wc->cbClsExtra;
    c->wnd_extra = wc->cbWndExtra;
    c->inst = wc->hInstance;
    c->icon = wc->hIcon; c->icon_sm = wc->hIconSm; c->cursor = wc->hCursor; c->brush = wc->hbrBackground;
    if (c->cls_extra > 0) c->extra = (u8*)izalloc((usize)c->cls_extra);
    c->next = classes;
    classes = c;
    return c->atom;
}
static u16 WINAPI RegisterClassExW(const WNDCLASSEXW* wc) { return register_class(wc, true); }
static u16 WINAPI RegisterClassExA(const WNDCLASSEXW* wc) { return register_class(wc, false); }
static u16 WINAPI RegisterClassW(const WNDCLASSW* w) {
    WNDCLASSEXW e = {sizeof(e), w->style, w->lpfnWndProc, w->cbClsExtra, w->cbWndExtra, w->hInstance, w->hIcon, w->hCursor, w->hbrBackground, w->lpszMenuName, w->lpszClassName, nullptr};
    return register_class(&e, true);
}
static u16 WINAPI RegisterClassA(const WNDCLASSW* w) {
    WNDCLASSEXW e = {sizeof(e), w->style, w->lpfnWndProc, w->cbClsExtra, w->cbWndExtra, w->hInstance, w->hIcon, w->hCursor, w->hbrBackground, w->lpszMenuName, w->lpszClassName, nullptr};
    return register_class(&e, false);
}
static BOOL WINAPI UnregisterClassW(const WCHAR* n, HINSTANCE i) {
    (void)i;
    Guard g(user_lock);
    Cls* c = cls_find(n, true);
    if (!c) { set_error(ERROR_CLASS_DOES_NOT_EXIST); return FALSE; }
    for (Cls** p = &classes; *p; p = &(*p)->next) if (*p == c) { *p = c->next; break; }
    return TRUE;
}
static BOOL WINAPI UnregisterClassA(const char* n, HINSTANCE i) {
    (void)i;
    Guard g(user_lock);
    Cls* c = cls_find(n, false);
    if (!c) { set_error(ERROR_CLASS_DOES_NOT_EXIST); return FALSE; }
    for (Cls** p = &classes; *p; p = &(*p)->next) if (*p == c) { *p = c->next; break; }
    return TRUE;
}
static BOOL class_info(const void* name, bool unicode, WNDCLASSEXW* out) {
    Cls* c = cls_find(name, unicode);
    if (!c) { set_error(ERROR_CLASS_DOES_NOT_EXIST); return FALSE; }
    out->style = c->style; out->lpfnWndProc = c->proc; out->cbClsExtra = c->cls_extra; out->cbWndExtra = c->wnd_extra;
    out->hInstance = c->inst; out->hIcon = c->icon; out->hCursor = c->cursor; out->hbrBackground = c->brush; out->lpszMenuName = nullptr;
    out->lpszClassName = name; out->hIconSm = c->icon_sm;
    return TRUE;
}
static BOOL WINAPI GetClassInfoExW(HINSTANCE i, const WCHAR* n, WNDCLASSEXW* out) { (void)i; return class_info(n, true, out); }
static BOOL WINAPI GetClassInfoExA(HINSTANCE i, const char* n, WNDCLASSEXW* out) { (void)i; return class_info(n, false, out); }
static BOOL WINAPI GetClassInfoW(HINSTANCE i, const WCHAR* n, WNDCLASSW* out) {
    WNDCLASSEXW e;
    if (!GetClassInfoExW(i, n, &e)) return FALSE;
    memcpy(out, &e.style, sizeof(WNDCLASSW));
    return TRUE;
}
static BOOL WINAPI GetClassInfoA(HINSTANCE i, const char* n, WNDCLASSW* out) {
    WNDCLASSEXW e;
    if (!GetClassInfoExA(i, n, &e)) return FALSE;
    memcpy(out, &e.style, sizeof(WNDCLASSW));
    return TRUE;
}

// ---------------------------------------------------------------- windows
static const int MAX_WND = 4096;
static Wnd* wnds[MAX_WND];
static Wnd* by_kwin[16];
static const u64 HWND_BASE = 0x00020000;
Wnd* wnd_get(HWND h) {
    u64 v = (u64)h;
    if (v < HWND_BASE || (v - HWND_BASE) / 2 >= MAX_WND || (v & 1)) return nullptr;
    Wnd* w = wnds[(v - HWND_BASE) / 2];
    return w && !w->destroyed ? w : nullptr;
}
static Wnd* wnd_or_error(HWND h) { Wnd* w = wnd_get(h); if (!w) set_error(ERROR_INVALID_WINDOW_HANDLE); return w; }
Wnd* wnd_top(Wnd* w) { while (w && w->parent) w = w->parent; return w; }
void wnd_client_origin(Wnd* w, int* x, int* y) {
    int ax = 0, ay = 0;
    for (Wnd* p = w; p && p->parent; p = p->parent) { ax += p->rect.left + p->nc_l; ay += p->rect.top + p->nc_t; }
    *x = ax; *y = ay;
}
void wnd_mark_dirty(Wnd* w) { Wnd* t = wnd_top(w); if (t) t->dirty = true; }
void present_all() {
    for (int i = 1; i < 16; i++) {
        Wnd* w = by_kwin[i];
        if (w && w->dirty) { w->dirty = false; si::win_ctl(w->kwin, si::WC_PRESENT); }
    }
}

// the non-client margins Windows would give a window with this style (silicon draws its own frame)
static void nc_margins(u32 style, u32 ex, int* l, int* t, int* r, int* b) {
    (void)ex;
    int f = 0, cap = 0;
    if (style & WS_CHILD) { f = (style & WS_BORDER) ? 1 : 0; if ((style & WS_CAPTION) == WS_CAPTION) cap = 23; }
    else if (style & WS_THICKFRAME) f = 8;
    else if (style & (WS_DLGFRAME | WS_BORDER)) f = 3;
    else if (!(style & WS_POPUP)) f = 3;
    if ((style & WS_CAPTION) == WS_CAPTION && !(style & WS_CHILD)) cap = 23;
    if (ex & 0x200) f += 2;                                        // WS_EX_CLIENTEDGE
    *l = *r = *b = f;
    *t = f + cap;
}

// thread message queues
struct MsgQ {
    u32 tid;
    MSG* ring;
    int cap, head, tail;
    bool quit;
    int quit_code;
    HWND focus, active, capture;
    u8 keys[256];
    u32 chars[256];                     // the character the last key-down of each virtual key carried
    MSG cur;
    int pending_bytes;                  // UTF-8 bytes of a WM_CHAR still to hand to an ANSI loop
    u8 bytes[4];
    MsgQ* next;
};
static MsgQ* queues = nullptr;
static MsgQ* queue_for(u32 tid) {
    for (MsgQ* q = queues; q; q = q->next) if (q->tid == tid) return q;
    MsgQ* q = (MsgQ*)izalloc(sizeof(MsgQ));
    q->tid = tid;
    q->cap = 512;
    q->ring = (MSG*)izalloc(sizeof(MSG) * (usize)q->cap);
    q->next = queues;
    queues = q;
    return q;
}
static MsgQ* my_queue() { Guard g(user_lock); return queue_for(cur_thr()->id); }
static int screen_mouse_x = 0, screen_mouse_y = 0;
static u32 msg_time() { return (u32)now_ms(); }
static volatile u32 pump_owner = 0;
static bool post_to(MsgQ* q, HWND h, u32 m, WPARAM w, LPARAM l) {
    Guard g(user_lock);
    int nt = (q->tail + 1) % q->cap;
    if (nt == q->head) {                                           // grow
        MSG* r = (MSG*)izalloc(sizeof(MSG) * (usize)q->cap * 2);
        int n = 0;
        for (int i = q->head; i != q->tail; i = (i + 1) % q->cap) r[n++] = q->ring[i];
        ifree(q->ring);
        q->ring = r; q->head = 0; q->tail = n; q->cap *= 2;
        nt = q->tail + 1;
    }
    MSG& mm = q->ring[q->tail];
    mm.hwnd = h; mm.message = m; mm.wParam = w; mm.lParam = l; mm.time = msg_time(); mm.pt = {screen_mouse_x, screen_mouse_y};
    q->tail = nt;
    if (pump_owner && pump_owner != cur_thr()->id) si::event_post();      // wake whoever is waiting in silicon
    return true;
}

static HWND wnd_add(Wnd* w) {
    Guard g(user_lock);
    for (int i = 1; i < MAX_WND; i++)
        if (!wnds[i]) { wnds[i] = w; w->h = (HWND)(HWND_BASE + (u64)i * 2); return w->h; }
    return nullptr;
}

// calling a window procedure, converting the messages that carry text when the procedure wants the other kind
static LRESULT call_proc(WNDPROC proc, bool proc_unicode, HWND h, u32 m, WPARAM w, LPARAM l, bool caller_unicode) {
    if (!proc) return 0;
    if (proc_unicode == caller_unicode) return proc(h, m, w, l);
    switch (m) {
    case WM_SETTEXT: {
        if (!l) return proc(h, m, w, l);
        if (caller_unicode) { char* a = utf8_dup((const WCHAR*)l); LRESULT r = proc(h, m, w, (LPARAM)a); ifree(a); return r; }
        WCHAR* x = wide_dup((const char*)l); LRESULT r = proc(h, m, w, (LPARAM)x); ifree(x); return r;
    }
    case WM_GETTEXT: {
        if (!w || !l) return proc(h, m, w, l);
        if (caller_unicode) {
            char* tmp = (char*)izalloc(w * 4 + 4);
            proc(h, m, w * 4, (LPARAM)tmp);
            int n = wide_from_utf8(tmp, -1, (WCHAR*)l, (int)w);
            if (!n) { ((WCHAR*)l)[w - 1] = 0; n = (int)w; }
            ifree(tmp);
            return n - 1;
        }
        WCHAR* tmp = (WCHAR*)izalloc(w * 2 + 2);
        proc(h, m, w, (LPARAM)tmp);
        int n = utf8_from_wide(tmp, -1, (char*)l, (int)w);
        if (!n) { ((char*)l)[w - 1] = 0; n = (int)w; }
        ifree(tmp);
        return n - 1;
    }
    case WM_NCCREATE: case WM_CREATE: {
        CREATESTRUCTW cs = *(CREATESTRUCTW*)l;
        void *a = nullptr, *b = nullptr;
        if (caller_unicode) {
            if (cs.lpszName) cs.lpszName = (const WCHAR*)(a = utf8_dup(cs.lpszName));
            if ((u64)cs.lpszClass >= 0x10000) cs.lpszClass = (const WCHAR*)(b = utf8_dup(cs.lpszClass));
        } else {
            if (cs.lpszName) cs.lpszName = (const WCHAR*)(a = wide_dup((const char*)cs.lpszName));
            if ((u64)cs.lpszClass >= 0x10000) cs.lpszClass = (const WCHAR*)(b = wide_dup((const char*)cs.lpszClass));
        }
        LRESULT r = proc(h, m, w, (LPARAM)&cs);
        if (a) ifree(a);
        if (b) ifree(b);
        return r;
    }
    case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR:
        if (caller_unicode && w >= 0x80) {                         // an ANSI procedure gets the UTF-8 bytes one at a time
            char u[4];
            WCHAR c = (WCHAR)w;
            int k = utf8_from_wide(&c, 1, u, 4);
            LRESULT r = 0;
            for (int i = 0; i < k; i++) r = proc(h, m, (u8)u[i], l);
            return r;
        }
        return proc(h, m, w, l);
    }
    return proc(h, m, w, l);
}
static LRESULT send(Wnd* wnd, u32 m, WPARAM w, LPARAM l, bool unicode = true) {
    if (!wnd) return 0;
    return call_proc(wnd->proc, wnd->unicode, wnd->h, m, w, l, unicode);
}

static void invalidate(Wnd* w, const RECT* r, bool erase) {
    RECT a = r ? *r : RECT{0, 0, w->cw, w->ch};
    if (a.left < 0) a.left = 0;
    if (a.top < 0) a.top = 0;
    if (a.right > w->cw) a.right = w->cw;
    if (a.bottom > w->ch) a.bottom = w->ch;
    if (a.right <= a.left || a.bottom <= a.top) return;
    if (!w->need_paint) w->inval = a;
    else {
        if (a.left < w->inval.left) w->inval.left = a.left;
        if (a.top < w->inval.top) w->inval.top = a.top;
        if (a.right > w->inval.right) w->inval.right = a.right;
        if (a.bottom > w->inval.bottom) w->inval.bottom = a.bottom;
    }
    w->need_paint = true;
    if (erase) w->erase = true;
    // children inside the area need painting too
    for (Wnd* c = w->first_child; c; c = c->next_sibling) {
        if (!c->visible) continue;
        RECT cr = {a.left - c->rect.left - c->nc_l, a.top - c->rect.top - c->nc_t, a.right - c->rect.left - c->nc_l, a.bottom - c->rect.top - c->nc_t};
        if (cr.right > 0 && cr.bottom > 0 && cr.left < c->cw && cr.top < c->ch) invalidate(c, &cr, erase);
    }
    MsgQ* q;
    { Guard g(user_lock); q = queue_for(w->tid); }
    if (pump_owner && pump_owner != cur_thr()->id) si::event_post();
    (void)q;
}

// the top-level window's screen position, kept from silicon's move events
static void top_screen_client(Wnd* t, int* x, int* y) { *x = t->rect.left + t->nc_l; *y = t->rect.top + t->nc_t; }

static void update_top_pos(Wnd* t) {
    i64 v = si::win_ctl(t->kwin, si::WC_GET_POS);
    int x = (int)(i32)(v & 0xFFFFFFFF), y = (int)(i32)(v >> 32);
    if (x == -1 && y == -1) return;
    t->rect = {x - t->nc_l, y - t->nc_t, x + t->cw + t->nc_r, y + t->ch + t->nc_b};
}

// the shared pixel buffer has a new size
static void resize_surface(Wnd* t, int cw, int ch) {
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    if ((u64)cw * ch > t->surf_cap) return;
    t->cw = cw; t->ch = ch;
    t->surf.w = cw; t->surf.h = ch; t->surf.stride = cw * 4;
    t->rect.right = t->rect.left + t->nc_l + cw + t->nc_r;
    t->rect.bottom = t->rect.top + t->nc_t + ch + t->nc_b;
}

static HWND create_window(u32 ex, const void* cls_name, const void* title, u32 style, int x, int y, int w, int h, HWND parent_h, HANDLE menu,
                          HINSTANCE inst, void* param, bool unicode) {
    Cls* c;
    { Guard g(user_lock); c = cls_find(cls_name, unicode); }
    if (!c) {
        char n[128];
        if ((u64)cls_name < 0x10000) snfmt(n, sizeof(n), "#%u", (u32)(u64)cls_name);
        else if (unicode) utf8_from_wide((const WCHAR*)cls_name, -1, n, sizeof(n));
        else slcpy(n, (const char*)cls_name, sizeof(n));
        msg("CreateWindow: no window class \"%s\"", n);
        set_error(ERROR_CLASS_DOES_NOT_EXIST);
        return nullptr;
    }
    Wnd* parent = wnd_get(parent_h);
    if ((style & WS_CHILD) && !parent) { set_error(ERROR_INVALID_WINDOW_HANDLE); return nullptr; }
    if (!(style & WS_CHILD) && (style & WS_POPUP) == 0) style |= WS_CAPTION | WS_CLIPSIBLINGS;    // an overlapped window always has a caption
    Wnd* wnd = (Wnd*)izalloc(sizeof(Wnd));
    wnd->cls = c;
    wnd->proc = c->proc;
    wnd->unicode = c->unicode;
    wnd->style = style;
    wnd->exstyle = ex;
    wnd->inst = inst;
    wnd->tid = cur_thr()->id;
    wnd->enabled = !(style & WS_DISABLED);
    if (c->wnd_extra > 0) { wnd->extra = (u8*)izalloc((usize)c->wnd_extra); wnd->nextra = c->wnd_extra; }
    nc_margins(style, ex, &wnd->nc_l, &wnd->nc_t, &wnd->nc_r, &wnd->nc_b);
    if (style & WS_CHILD) { wnd->parent = parent; wnd->id = (u64)menu; }
    else { wnd->owner = parent ? wnd_top(parent) : nullptr; wnd->menu = menu; }
    if (!wnd_add(wnd)) { ifree(wnd); set_error(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    HWND hw = wnd->h;

    int sw = (int)(si::win_ctl(0, si::WC_SCREEN_SIZE) & 0xFFFF), sh = (int)((si::win_ctl(0, si::WC_SCREEN_SIZE) >> 16) & 0xFFFF);
    if (!(style & WS_CHILD)) {
        if (w == CW_USEDEFAULT || w <= 0) { w = sw * 3 / 4; h = sh * 3 / 4; }
        if (h == CW_USEDEFAULT || h <= 0) h = sh * 3 / 4;
        if (x == CW_USEDEFAULT) x = y = -1;
    } else {
        if (x == CW_USEDEFAULT) x = y = 0;
        if (w == CW_USEDEFAULT) w = h = 0;
    }
    int cw = w - wnd->nc_l - wnd->nc_r, ch = h - wnd->nc_t - wnd->nc_b;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    wnd->cw = cw; wnd->ch = ch;
    if (style & WS_CHILD) {
        wnd->rect = {x, y, x + w, y + h};
        // the last child created is on top: keep the list in z-order, last = topmost
        Wnd** p = &parent->first_child;
        while (*p) p = &(*p)->next_sibling;
        *p = wnd;
    } else {
        if (cw > sw - 8) cw = sw - 8;
        if (ch > sh - 72) ch = sh - 72;
        char t[64] = "";
        if (title) { if (unicode) utf8_from_wide((const WCHAR*)title, -1, t, sizeof(t)); else slcpy(t, (const char*)title, sizeof(t)); }
        u64 out[2];
        int kw = si::win_create(cw, ch, t, (style & WS_THICKFRAME) ? 1 : 0, out);
        if (kw <= 0) { msg("silicon has no room for another window"); wnds[((u64)hw - HWND_BASE) / 2] = nullptr; ifree(wnd); set_error(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
        wnd->kwin = kw;
        by_kwin[kw] = wnd;
        wnd->surf.bits = (u8*)out[0];
        wnd->surf_cap = (u32)out[1];
        wnd->surf.bpp = 32;
        wnd->surf.rmask = 0xFF0000; wnd->surf.gmask = 0xFF00; wnd->surf.bmask = 0xFF;
        wnd->cw = cw; wnd->ch = ch;
        wnd->surf.w = cw; wnd->surf.h = ch; wnd->surf.stride = cw * 4;
        if (x >= 0) si::win_ctl(kw, si::WC_MOVE, x + wnd->nc_l, y + wnd->nc_t);
        int px = x >= 0 ? x : (sw - cw) / 2 - wnd->nc_l, py = y >= 0 ? y : (sh - ch) / 2 - wnd->nc_t;
        wnd->rect = {px, py, px + wnd->nc_l + cw + wnd->nc_r, py + wnd->nc_t + ch + wnd->nc_b};
        // the background starts as the class brush would paint it
        u32 bg = sys_color(5);
        for (int yy = 0; yy < ch; yy++) for (int xx = 0; xx < cw; xx++) row32(wnd->surf, yy)[xx] = bg;
    }
    if (title) {
        if (unicode) wnd->text = wdup((const WCHAR*)title);
        else wnd->text = wide_dup((const char*)title);
    }
    CREATESTRUCTW cs = {param, inst, menu, parent_h, h, w, y, x, (i32)style, (const WCHAR*)title, (const WCHAR*)cls_name, ex};
    if (!send(wnd, WM_NCCREATE, 0, (LPARAM)&cs, unicode)) {
        trace("WM_NCCREATE refused");
        extern BOOL WINAPI DestroyWindow_(HWND);
        DestroyWindow_(hw);
        return nullptr;
    }
    if (send(wnd, WM_CREATE, 0, (LPARAM)&cs, unicode) == -1) {
        trace("WM_CREATE refused");
        extern BOOL WINAPI DestroyWindow_(HWND);
        DestroyWindow_(hw);
        return nullptr;
    }
    if (!wnd_get(hw)) return nullptr;
    send(wnd, WM_SIZE, 0, MAKELP(wnd->cw, wnd->ch));
    send(wnd, WM_MOVE, 0, MAKELP(wnd->rect.left + wnd->nc_l, wnd->rect.top + wnd->nc_t));
    if (wnd->parent) send(wnd->parent, WM_PARENTNOTIFY, MAKELP(WM_CREATE, (int)wnd->id), (LPARAM)hw);
    if (style & WS_VISIBLE) {
        extern BOOL WINAPI ShowWindow_(HWND, int);
        wnd->style &= ~WS_VISIBLE;
        ShowWindow_(hw, (style & WS_MAXIMIZE) ? 3 : (style & WS_MINIMIZE) ? 6 : 1);
    }
    trace("CreateWindow %p %s%dx%d", hw, (style & WS_CHILD) ? "child " : "", wnd->cw, wnd->ch);
    return hw;
}
static HWND WINAPI CreateWindowExW(u32 ex, const WCHAR* cls, const WCHAR* title, u32 style, int x, int y, int w, int h, HWND parent, HANDLE menu, HINSTANCE inst, void* param) {
    return create_window(ex, cls, title, style, x, y, w, h, parent, menu, inst, param, true);
}
static HWND WINAPI CreateWindowExA(u32 ex, const char* cls, const char* title, u32 style, int x, int y, int w, int h, HWND parent, HANDLE menu, HINSTANCE inst, void* param) {
    return create_window(ex, cls, title, style, x, y, w, h, parent, menu, inst, param, false);
}

static void remove_timers(HWND h);
BOOL WINAPI DestroyWindow_(HWND h) {
    Wnd* w = wnd_or_error(h);
    if (!w) return FALSE;
    if (w->tid != cur_thr()->id) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
    if (w->visible && w->kwin) si::win_ctl(w->kwin, si::WC_HIDE);
    w->visible = false;
    send(w, WM_DESTROY, 0, 0);
    // children go too (after the parent hears about it, as Windows does)
    while (Wnd* c = w->first_child) { w->first_child = c->next_sibling; c->parent = nullptr; DestroyWindow_(c->h); }
    // windows it owns go too
    for (int i = 1; i < MAX_WND; i++) if (wnds[i] && wnds[i]->owner == w && !wnds[i]->destroyed) DestroyWindow_(wnds[i]->h);
    send(w, WM_NCDESTROY, 0, 0);
    remove_timers(h);
    MsgQ* q = my_queue();
    if (q->focus == h) q->focus = nullptr;
    if (q->active == h) q->active = nullptr;
    if (q->capture == h) q->capture = nullptr;
    if (w->parent) {
        for (Wnd** p = &w->parent->first_child; *p; p = &(*p)->next_sibling) if (*p == w) { *p = w->next_sibling; break; }
        invalidate(w->parent, &w->rect, true);
    }
    if (w->kwin) { si::win_ctl(w->kwin, si::WC_DESTROY); by_kwin[w->kwin] = nullptr; }
    w->destroyed = true;
    if (w->own_dc) { DC* d = w->own_dc; w->own_dc = nullptr; dc_free(d); }
    // the slot stays taken by the destroyed window so stale handles don't find a new window
    return TRUE;
}
static BOOL WINAPI DestroyWindow(HWND h) { return DestroyWindow_(h); }

static void set_focus_to(MsgQ* q, HWND h);
BOOL WINAPI ShowWindow_(HWND h, int cmd) {
    Wnd* w = wnd_or_error(h);
    if (!w) return FALSE;
    bool was = w->visible;
    bool show = cmd != 0;
    if (show && !was) {
        w->style |= WS_VISIBLE;
        w->visible = true;
        send(w, WM_SHOWWINDOW, TRUE, 0);
        if (w->kwin) {
            si::win_ctl(w->kwin, si::WC_SHOW);
            update_top_pos(w);
            i64 sz = si::win_ctl(w->kwin, si::WC_GET_SIZE);
            int cw = (int)(sz & 0xFFFF), ch = (int)((sz >> 16) & 0xFFFF);
            if (cw != w->cw || ch != w->ch) { resize_surface(w, cw, ch); send(w, WM_SIZE, 0, MAKELP(w->cw, w->ch)); }
            MsgQ* q = my_queue();
            if (cmd != 4 && cmd != 8 && cmd != 7) {                    // the "no activate" variants
                q->active = h;
                send(w, WM_ACTIVATE, 1, 0);
                if (!q->focus || wnd_top(wnd_get(q->focus)) != w) set_focus_to(q, h);
            }
        }
        invalidate(w, nullptr, true);
        if (w->parent) invalidate(w->parent, &w->rect, true);
    } else if (!show && was) {
        send(w, WM_SHOWWINDOW, FALSE, 0);
        w->style &= ~WS_VISIBLE;
        w->visible = false;
        if (w->kwin) si::win_ctl(w->kwin, si::WC_HIDE);
        if (w->parent) invalidate(w->parent, &w->rect, true);
    }
    if (w->kwin && show) {
        if (cmd == 2 || cmd == 6 || cmd == 7) { si::win_ctl(w->kwin, si::WC_MINIMIZE); w->minimized = true; }
        else if (cmd == 3 && !w->maximized) { si::win_ctl(w->kwin, si::WC_MAXIMIZE); w->maximized = true; }
        else if (cmd == 9 && w->maximized) { si::win_ctl(w->kwin, si::WC_MAXIMIZE); w->maximized = false; }
        else if (cmd == 9 && w->minimized) { si::win_ctl(w->kwin, si::WC_FOCUS); w->minimized = false; }
    }
    return was;
}
static BOOL WINAPI ShowWindow(HWND h, int cmd) { return ShowWindow_(h, cmd); }
static BOOL WINAPI ShowWindowAsync(HWND h, int cmd) { return ShowWindow_(h, cmd); }

// ---------------------------------------------------------------- painting
static HDC get_window_dc(Wnd* w) {
    if (w->own_dc) return w->own_dc->handle;
    HDC h = dc_new(DCK_WINDOW, w);
    DC* dc = dc_get(h);
    if (w->cls->style & 0x60) { dc->own = true; w->own_dc = dc; }      // CS_OWNDC, CS_CLASSDC
    return h;
}
static HDC WINAPI GetDC(HWND h) {
    if (!h) return dc_new(DCK_SCREEN, nullptr);
    Wnd* w = wnd_or_error(h);
    return w ? get_window_dc(w) : nullptr;
}
static HDC WINAPI GetDCEx(HWND h, HANDLE rgn, u32 f) { (void)rgn; (void)f; return GetDC(h); }
static HDC WINAPI GetWindowDC(HWND h) { return GetDC(h); }
static int WINAPI ReleaseDC(HWND h, HDC hdc) {
    (void)h;
    DC* dc = dc_get(hdc);
    if (!dc) return 0;
    if (dc->wnd) wnd_mark_dirty(dc->wnd);
    present_all();
    if (dc->own) return 1;
    dc_free(dc);
    return 1;
}
static HANDLE WINAPI GetSysColorBrush(int i);
static HANDLE class_brush(Wnd* w) {
    HANDLE b = w->cls->brush;
    if ((u64)b > 0 && (u64)b <= 31) return GetSysColorBrush((int)(u64)b - 1);   // COLOR_xxx + 1
    return b;
}
static HDC WINAPI BeginPaint(HWND h, PAINTSTRUCT* ps) {
    Wnd* w = wnd_or_error(h);
    if (!w) return nullptr;
    HDC hdc = get_window_dc(w);
    DC* dc = dc_get(hdc);
    RECT r = w->need_paint ? w->inval : RECT{0, 0, 0, 0};
    bool erase = w->erase;
    w->need_paint = false;
    w->erase = false;
    dc->paint = true;
    dc->paint_rect = r;
    memset(ps, 0, sizeof(*ps));
    ps->hdc = hdc;
    ps->rcPaint = r;
    if (erase) ps->fErase = !send(w, WM_ERASEBKGND, (WPARAM)hdc, 0);
    return hdc;
}
static BOOL WINAPI EndPaint(HWND h, const PAINTSTRUCT* ps) {
    Wnd* w = wnd_get(h);
    DC* dc = dc_get(ps->hdc);
    if (dc) dc->paint = false;
    if (w) wnd_mark_dirty(w);
    if (dc && !dc->own) dc_free(dc);
    present_all();
    return TRUE;
}
static BOOL WINAPI InvalidateRect(HWND h, const RECT* r, BOOL erase) {
    if (!h) { for (int i = 1; i < 16; i++) if (by_kwin[i]) invalidate(by_kwin[i], nullptr, erase); return TRUE; }
    Wnd* w = wnd_or_error(h);
    if (!w) return FALSE;
    invalidate(w, r, erase);
    return TRUE;
}
static BOOL WINAPI InvalidateRgn(HWND h, HANDLE rgn, BOOL erase) {
    RECT r;
    extern int WINAPI GetRgnBox_(HANDLE, RECT*);
    if (rgn && GetRgnBox_(rgn, &r)) return InvalidateRect(h, &r, erase);
    return InvalidateRect(h, nullptr, erase);
}
int WINAPI GetRgnBox_(HANDLE h, RECT* r) {
    Region* g = (Region*)gdi_get(h, G_REGION);
    if (!g) return 0;
    *r = g->r;
    return 2;
}
static BOOL WINAPI ValidateRect(HWND h, const RECT* r) {
    Wnd* w = wnd_or_error(h);
    if (!w) return FALSE;
    if (!r || (r->left <= w->inval.left && r->top <= w->inval.top && r->right >= w->inval.right && r->bottom >= w->inval.bottom)) { w->need_paint = false; w->erase = false; }
    return TRUE;
}
static BOOL WINAPI ValidateRgn(HWND h, HANDLE rgn) { (void)rgn; return ValidateRect(h, nullptr); }
static BOOL WINAPI GetUpdateRect(HWND h, RECT* r, BOOL erase) {
    Wnd* w = wnd_or_error(h);
    if (!w) return FALSE;
    if (r) *r = w->need_paint ? w->inval : RECT{0, 0, 0, 0};
    (void)erase;
    return w->need_paint;
}
static int WINAPI GetUpdateRgn(HWND h, HANDLE rgn, BOOL erase) {
    Wnd* w = wnd_or_error(h);
    Region* g = (Region*)gdi_get(rgn, G_REGION);
    if (!w || !g) return 0;
    (void)erase;
    g->r = w->need_paint ? w->inval : RECT{0, 0, 0, 0};
    return w->need_paint ? 2 : 1;
}
static void paint_now(Wnd* w) {
    if (!w->visible || w->destroyed) return;
    if (w->need_paint) send(w, WM_PAINT, 0, 0);
    for (Wnd* c = w->first_child; c; c = c->next_sibling) paint_now(c);
}
static BOOL WINAPI UpdateWindow(HWND h) {
    Wnd* w = wnd_or_error(h);
    if (!w) return FALSE;
    paint_now(w);
    present_all();
    return TRUE;
}
static BOOL WINAPI RedrawWindow(HWND h, const RECT* r, HANDLE rgn, u32 flags) {
    (void)rgn;
    Wnd* w = h ? wnd_or_error(h) : nullptr;
    if (h && !w) return FALSE;
    if (!w) { for (int i = 1; i < 16; i++) if (by_kwin[i]) RedrawWindow(by_kwin[i]->h, nullptr, nullptr, flags); return TRUE; }
    if (flags & 1) invalidate(w, r, (flags & 4) != 0);             // RDW_INVALIDATE, RDW_ERASE
    if (flags & 8) w->need_paint = false;                          // RDW_VALIDATE
    if (flags & 0x100) paint_now(w);                               // RDW_UPDATENOW
    present_all();
    return TRUE;
}
static BOOL WINAPI LockWindowUpdate(HWND h) { (void)h; return TRUE; }
static BOOL WINAPI GdiFlushUser() { present_all(); return TRUE; }
static int WINAPI ScrollWindowEx(HWND h, int dx, int dy, const RECT* scroll, const RECT* clip, HANDLE rgn, RECT* upd, u32 flags) {
    (void)rgn;
    Wnd* w = wnd_or_error(h);
    if (!w) return 0;
    Wnd* t = wnd_top(w);
    int ox, oy;
    wnd_client_origin(w, &ox, &oy);
    RECT r = scroll ? *scroll : RECT{0, 0, w->cw, w->ch};
    if (clip) { if (r.left < clip->left) r.left = clip->left; if (r.top < clip->top) r.top = clip->top; if (r.right > clip->right) r.right = clip->right; if (r.bottom > clip->bottom) r.bottom = clip->bottom; }
    int rw = r.right - r.left, rh = r.bottom - r.top;
    if (rw > 0 && rh > 0 && t->surf.bits) {
        int y0 = dy > 0 ? rh - 1 : 0, y1 = dy > 0 ? -1 : rh, st = dy > 0 ? -1 : 1;
        for (int y = y0; y != y1; y += st) {
            int sy = y - dy;
            if (sy < 0 || sy >= rh) continue;
            int sx0 = dx > 0 ? 0 : -dx, n = rw - (dx > 0 ? dx : -dx);
            if (n <= 0) continue;
            int ty = oy + r.top + y, syy = oy + r.top + sy;
            if (ty < 0 || ty >= t->surf.h || syy < 0 || syy >= t->surf.h) continue;
            memmove(row32(t->surf, ty) + ox + r.left + sx0 + dx, row32(t->surf, syy) + ox + r.left + sx0, (usize)n * 4);
        }
        t->dirty = true;
    }
    RECT exposed = r;
    if (dx > 0) exposed.right = r.left + dx; else if (dx < 0) exposed.left = r.right + dx;
    if (dy > 0) exposed.bottom = r.top + dy; else if (dy < 0) exposed.top = r.bottom + dy;
    if (!dx) { exposed.left = r.left; exposed.right = r.right; }
    if (!dy) { exposed.top = r.top; exposed.bottom = r.bottom; }
    if (upd) *upd = exposed;
    if (flags & 2) invalidate(w, &exposed, (flags & 4) != 0);     // SW_INVALIDATE, SW_ERASE
    for (Wnd* c = w->first_child; c && (flags & 1); c = c->next_sibling) { c->rect.left += dx; c->rect.right += dx; c->rect.top += dy; c->rect.bottom += dy; }
    return 2;
}
static BOOL WINAPI ScrollWindow(HWND h, int dx, int dy, const RECT* r, const RECT* clip) { return ScrollWindowEx(h, dx, dy, r, clip, nullptr, nullptr, 2 | 4) != 0; }
static BOOL WINAPI ScrollDC(HDC h, int dx, int dy, const RECT* r, const RECT* clip, HANDLE rgn, RECT* upd) {
    DC* dc = dc_get(h);
    if (!dc || !dc->wnd) return FALSE;
    return ScrollWindowEx(dc->wnd->h, dx, dy, r, clip, rgn, upd, 0) != 0;
}

// ---------------------------------------------------------------- system colours and metrics
static const u32 sys_colors[31] = {
    0xC8C8C8, 0x000000, 0xD1B499, 0xDBCDBF, 0xF0F0F0, 0xFFFFFF, 0x646464, 0x000000, 0x000000, 0x000000, 0xB4B4B4, 0xFCF7F4, 0xABABAB,
    0xD77800, 0xFFFFFF, 0xF0F0F0, 0xA0A0A0, 0x6D6D6D, 0x000000, 0x000000, 0xFFFFFF, 0x696969, 0xE3E3E3, 0x000000, 0xE1FFFF, 0xB5B5B5,
    0xCC6600, 0xEAD1B9, 0xF2E4D7, 0xFF9933, 0xF0F0F0,
};                                                                 // COLORREFs (0x00BBGGRR), Windows 10 defaults
u32 sys_color(int i) { return i >= 0 && i < 31 ? rgb_of(sys_colors[i]) : 0; }
static u32 WINAPI GetSysColor(int i) { return i >= 0 && i < 31 ? sys_colors[i] : 0; }
static HANDLE sys_brushes[31];
static HANDLE WINAPI GetSysColorBrush(int i) {
    if (i < 0 || i >= 31) return nullptr;
    if (!sys_brushes[i]) {
        extern HANDLE solid_brush_stock(COLORREF);
        sys_brushes[i] = solid_brush_stock(sys_colors[i]);
    }
    return sys_brushes[i];
}
HANDLE solid_brush_stock(COLORREF c) {
    Brush* b = (Brush*)izalloc(sizeof(Brush));
    b->type = G_BRUSH; b->style = 0; b->color = c; b->stock = true;
    return gdi_add(b);
}
static BOOL WINAPI SetSysColors(int n, const int* idx, const u32* c) { (void)n; (void)idx; (void)c; return TRUE; }
static int screen_wh(bool h) { i64 v = si::win_ctl(0, si::WC_SCREEN_SIZE); return h ? (int)((v >> 16) & 0xFFFF) : (int)(v & 0xFFFF); }
static int WINAPI GetSystemMetrics(int i) {
    switch (i) {
    case 0: case 16: case 61: case 59: case 78: return screen_wh(false);
    case 1: case 62: case 60: case 79: return screen_wh(true);
    case 17: return screen_wh(true) - 23;
    case 2: case 3: case 9: case 10: case 20: case 21: return 17;
    case 4: return 23;                      // SM_CYCAPTION
    case 5: case 6: return 1;
    case 7: case 8: return 3;
    case 11: case 12: case 13: case 14: return 32;
    case 15: return 20;                     // SM_CYMENU
    case 19: return 1;                      // SM_MOUSEPRESENT
    case 23: return 0;
    case 28: return 136;
    case 29: return 39;
    case 30: case 31: return 36;
    case 32: case 33: return 4;             // SM_CXFRAME
    case 34: return 136;
    case 35: return 39;
    case 36: case 37: return 4;
    case 38: case 39: return 75;
    case 43: return 3;                      // SM_CMOUSEBUTTONS
    case 45: case 46: return 2;             // SM_CXEDGE
    case 49: case 50: return 16;            // SM_CXSMICON
    case 51: return 23;
    case 52: case 53: return 22;
    case 55: case 56: return 19;            // SM_CXMENUSIZE...
    case 63: return 1;                      // SM_NETWORK
    case 67: return 0;                      // SM_CLEANBOOT
    case 75: return 1;                      // SM_MOUSEWHEELPRESENT
    case 76: case 77: return 0;
    case 80: return 1;                      // SM_CMONITORS
    case 81: return 1;                      // SM_SAMEDISPLAYFORMAT
    case 92: return 4;                      // SM_CXPADDEDBORDER
    case 0x1000: return 0;                  // SM_REMOTESESSION
    }
    return 0;
}
static int WINAPI GetSystemMetricsForDpi(int i, u32 dpi) { (void)dpi; return GetSystemMetrics(i); }
static BOOL WINAPI AdjustWindowRectEx(RECT* r, u32 style, BOOL menu, u32 ex) {
    int l, t, rr, b;
    nc_margins(style, ex, &l, &t, &rr, &b);
    if (!(style & WS_CHILD) && !(style & WS_POPUP)) { int tt; nc_margins(style | WS_CAPTION, ex, &l, &tt, &rr, &b); t = tt; }
    (void)menu;                                                    // silicon shows no menu bar, so it takes no room
    r->left -= l; r->top -= t; r->right += rr; r->bottom += b;
    return TRUE;
}
static BOOL WINAPI AdjustWindowRect(RECT* r, u32 style, BOOL menu) { return AdjustWindowRectEx(r, style, menu, 0); }
static BOOL WINAPI AdjustWindowRectExForDpi(RECT* r, u32 style, BOOL menu, u32 ex, u32 dpi) { (void)dpi; return AdjustWindowRectEx(r, style, menu, ex); }

// ---------------------------------------------------------------- geometry
static BOOL WINAPI GetClientRect(HWND h, RECT* r) {
    Wnd* w = wnd_or_error(h);
    if (!w) return FALSE;
    *r = {0, 0, w->cw, w->ch};
    return TRUE;
}
static void screen_origin(Wnd* w, int* x, int* y) {
    // the client area's (0,0) in screen coordinates
    int ax, ay;
    wnd_client_origin(w, &ax, &ay);
    Wnd* t = wnd_top(w);
    int tx, ty;
    top_screen_client(t, &tx, &ty);
    *x = tx + ax; *y = ty + ay;
}
static BOOL WINAPI GetWindowRect(HWND h, RECT* r) {
    Wnd* w = wnd_or_error(h);
    if (!w) return FALSE;
    int x, y;
    screen_origin(w, &x, &y);
    *r = {x - w->nc_l, y - w->nc_t, x + w->cw + w->nc_r, y + w->ch + w->nc_b};
    return TRUE;
}
static BOOL WINAPI ClientToScreen(HWND h, POINT* p) {
    Wnd* w = wnd_or_error(h);
    if (!w) return FALSE;
    int x, y;
    screen_origin(w, &x, &y);
    p->x += x; p->y += y;
    return TRUE;
}
static BOOL WINAPI ScreenToClient(HWND h, POINT* p) {
    Wnd* w = wnd_or_error(h);
    if (!w) return FALSE;
    int x, y;
    screen_origin(w, &x, &y);
    p->x -= x; p->y -= y;
    return TRUE;
}
static int WINAPI MapWindowPoints(HWND from, HWND to, POINT* p, u32 n) {
    int fx = 0, fy = 0, tx = 0, ty = 0;
    if (Wnd* f = wnd_get(from)) screen_origin(f, &fx, &fy);
    if (Wnd* t = wnd_get(to)) screen_origin(t, &tx, &ty);
    for (u32 i = 0; i < n; i++) { p[i].x += fx - tx; p[i].y += fy - ty; }
    return (int)(u16)(fx - tx) | ((int)(u16)(fy - ty) << 16);
}
static BOOL WINAPI SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, u32 flags) {
    (void)after;
    Wnd* w = wnd_or_error(h);
    if (!w) return FALSE;
    bool nomove = flags & 2, nosize = flags & 1;
    if (!nosize) {
        int ncw = cx - w->nc_l - w->nc_r, nch = cy - w->nc_t - w->nc_b;
        if (ncw < 1) ncw = 1;
        if (nch < 1) nch = 1;
        if (ncw != w->cw || nch != w->ch) {
            if (w->kwin) {
                if (si::win_ctl(w->kwin, si::WC_SET_SIZE, ncw, nch) == 0) resize_surface(w, ncw, nch);
            } else { w->cw = ncw; w->ch = nch; w->rect.right = w->rect.left + cx; w->rect.bottom = w->rect.top + cy; }
            send(w, WM_SIZE, 0, MAKELP(w->cw, w->ch));
            invalidate(w, nullptr, true);
        }
    }
    if (!nomove) {
        if (w->kwin) { si::win_ctl(w->kwin, si::WC_MOVE, x + w->nc_l, y + w->nc_t); update_top_pos(w); }
        else {
            int ow = w->rect.right - w->rect.left, oh = w->rect.bottom - w->rect.top;
            if (w->parent) invalidate(w->parent, &w->rect, true);
            w->rect = {x, y, x + ow, y + oh};
            invalidate(w, nullptr, true);
        }
        send(w, WM_MOVE, 0, MAKELP(x + w->nc_l, y + w->nc_t));
    }
    if (flags & 0x40) ShowWindow_(h, 5);                          // SWP_SHOWWINDOW
    if (flags & 0x80) ShowWindow_(h, 0);                          // SWP_HIDEWINDOW
    if (w->kwin && !(flags & 0x10) && w->visible) si::win_ctl(w->kwin, si::WC_FOCUS);   // !SWP_NOACTIVATE
    return TRUE;
}
static BOOL WINAPI MoveWindow(HWND h, int x, int y, int w, int hh, BOOL repaint) { (void)repaint; return SetWindowPos(h, nullptr, x, y, w, hh, 0x14); }
static HANDLE WINAPI BeginDeferWindowPos(int n) { (void)n; static int dummy; return &dummy; }
static HANDLE WINAPI DeferWindowPos(HANDLE d, HWND h, HWND after, int x, int y, int cx, int cy, u32 f) { SetWindowPos(h, after, x, y, cx, cy, f); return d; }
static BOOL WINAPI EndDeferWindowPos(HANDLE d) { (void)d; return TRUE; }
struct WINDOWPLACEMENT { u32 length, flags, showCmd; POINT ptMinPosition, ptMaxPosition; RECT rcNormalPosition; };
static BOOL WINAPI GetWindowPlacement(HWND h, WINDOWPLACEMENT* p) {
    Wnd* w = wnd_or_error(h);
    if (!w) return FALSE;
    p->flags = 0;
    p->showCmd = w->minimized ? 2 : w->maximized ? 3 : w->visible ? 1 : 0;
    p->ptMinPosition = {-1, -1};
    p->ptMaxPosition = {-1, -1};
    GetWindowRect(h, &p->rcNormalPosition);
    return TRUE;
}
static BOOL WINAPI SetWindowPlacement(HWND h, const WINDOWPLACEMENT* p) {
    const RECT& r = p->rcNormalPosition;
    SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, 0x14);
    ShowWindow_(h, (int)p->showCmd);
    return TRUE;
}
static BOOL WINAPI IsWindow(HWND h) { return wnd_get(h) != nullptr; }
static BOOL WINAPI IsWindowVisible(HWND h) { Wnd* w = wnd_get(h); while (w) { if (!w->visible) return FALSE; w = w->parent; } return h && wnd_get(h); }
static BOOL WINAPI IsIconic(HWND h) { Wnd* w = wnd_get(h); return w && w->minimized; }
static BOOL WINAPI IsZoomed(HWND h) { Wnd* w = wnd_get(h); return w && w->maximized; }
static BOOL WINAPI IsWindowEnabled(HWND h) { Wnd* w = wnd_get(h); return w && w->enabled; }
static BOOL WINAPI EnableWindow(HWND h, BOOL e) {
    Wnd* w = wnd_get(h);
    if (!w) return FALSE;
    bool was_disabled = !w->enabled;
    w->enabled = e != 0;
    if (e) w->style &= ~WS_DISABLED; else w->style |= WS_DISABLED;
    if (was_disabled == !e) return was_disabled;
    send(w, WM_ENABLE, e, 0);
    return was_disabled;
}
static BOOL WINAPI IsChild(HWND p, HWND c) { Wnd* w = wnd_get(c); Wnd* pw = wnd_get(p); for (w = w ? w->parent : nullptr; w; w = w->parent) if (w == pw) return TRUE; return FALSE; }
static HWND WINAPI GetParent(HWND h) { Wnd* w = wnd_get(h); if (!w) return nullptr; return w->parent ? w->parent->h : w->owner ? w->owner->h : nullptr; }
static HWND WINAPI GetAncestor(HWND h, u32 f) {
    Wnd* w = wnd_get(h);
    if (!w) return nullptr;
    if (f == 1) return w->parent ? w->parent->h : nullptr;        // GA_PARENT
    Wnd* t = wnd_top(w);
    if (f == 3) while (t->owner) t = t->owner;                    // GA_ROOTOWNER
    return t->h;
}
static HWND WINAPI SetParent(HWND h, HWND np) {
    Wnd* w = wnd_get(h);
    Wnd* p = wnd_get(np);
    if (!w) return nullptr;
    HWND old = w->parent ? w->parent->h : nullptr;
    if (w->parent && p && (w->style & WS_CHILD)) {
        for (Wnd** q = &w->parent->first_child; *q; q = &(*q)->next_sibling) if (*q == w) { *q = w->next_sibling; break; }
        w->next_sibling = nullptr;
        Wnd** q = &p->first_child;
        while (*q) q = &(*q)->next_sibling;
        *q = w;
        w->parent = p;
        invalidate(p, nullptr, true);
    }
    return old;
}
static HWND WINAPI GetDesktopWindow() { return (HWND)(u64)0x10010; }
static HWND WINAPI GetShellWindow() { return GetDesktopWindow(); }
static HWND WINAPI GetWindow(HWND h, u32 cmd) {
    Wnd* w = wnd_get(h);
    if (!w) return nullptr;
    switch (cmd) {
    case 5: return w->first_child ? w->first_child->h : nullptr;  // GW_CHILD
    case 2: return w->next_sibling ? w->next_sibling->h : nullptr; // GW_HWNDNEXT
    case 4: return w->owner ? w->owner->h : nullptr;              // GW_OWNER
    case 0: { if (!w->parent) return h; return w->parent->first_child ? w->parent->first_child->h : nullptr; }   // GW_HWNDFIRST
    case 1: { if (!w->parent) return h; Wnd* c = w->parent->first_child; while (c && c->next_sibling) c = c->next_sibling; return c ? c->h : nullptr; }
    case 3: { if (!w->parent) return nullptr; Wnd* prev = nullptr; for (Wnd* c = w->parent->first_child; c && c != w; c = c->next_sibling) prev = c; return prev ? prev->h : nullptr; }
    }
    return nullptr;
}
static HWND WINAPI GetTopWindow(HWND h) { return h ? GetWindow(h, 5) : (by_kwin[1] ? by_kwin[1]->h : nullptr); }
typedef BOOL (WINAPI *EnumWndProc)(HWND, LPARAM);
static BOOL WINAPI EnumWindows(EnumWndProc fn, LPARAM p) {
    for (int i = 1; i < MAX_WND; i++) if (wnds[i] && !wnds[i]->destroyed && !wnds[i]->parent) if (!fn(wnds[i]->h, p)) return FALSE;
    return TRUE;
}
static BOOL WINAPI EnumThreadWindows(u32 tid, EnumWndProc fn, LPARAM p) {
    for (int i = 1; i < MAX_WND; i++) if (wnds[i] && !wnds[i]->destroyed && !wnds[i]->parent && wnds[i]->tid == tid) if (!fn(wnds[i]->h, p)) return FALSE;
    return TRUE;
}
static BOOL enum_children(Wnd* w, EnumWndProc fn, LPARAM p) {
    for (Wnd* c = w->first_child; c; c = c->next_sibling) { if (!fn(c->h, p)) return FALSE; if (!enum_children(c, fn, p)) return FALSE; }
    return TRUE;
}
static BOOL WINAPI EnumChildWindows(HWND h, EnumWndProc fn, LPARAM p) { Wnd* w = wnd_get(h); return w ? enum_children(w, fn, p) : EnumWindows(fn, p); }
static HWND find_window(const void* cls, const void* title, bool unicode) {
    Cls* c = cls ? cls_find(cls, unicode) : nullptr;
    if (cls && !c) return nullptr;
    WCHAR t[256] = {0};
    if (title) { if (unicode) { usize n = wlen((const WCHAR*)title); if (n > 255) n = 255; memcpy(t, title, n * 2); } else wide_from_utf8((const char*)title, -1, t, 256); }
    for (int i = 1; i < MAX_WND; i++) {
        Wnd* w = wnds[i];
        if (!w || w->destroyed || w->parent) continue;
        if (c && w->cls != c) continue;
        if (title && (!w->text || wcmp(w->text, t))) continue;
        return w->h;
    }
    return nullptr;
}
static HWND WINAPI FindWindowW(const WCHAR* c, const WCHAR* t) { return find_window(c, t, true); }
static HWND WINAPI FindWindowA(const char* c, const char* t) { return find_window(c, t, false); }
static HWND WINAPI FindWindowExW(HWND p, HWND after, const WCHAR* c, const WCHAR* t) { (void)p; (void)after; return find_window(c, t, true); }
static HWND WINAPI FindWindowExA(HWND p, HWND after, const char* c, const char* t) { (void)p; (void)after; return find_window(c, t, false); }
static HWND WINAPI GetDlgItem(HWND h, int id) {
    Wnd* w = wnd_get(h);
    if (!w) return nullptr;
    for (Wnd* c = w->first_child; c; c = c->next_sibling) if ((int)c->id == id) return c->h;
    return nullptr;
}
static int WINAPI GetDlgCtrlID(HWND h) { Wnd* w = wnd_get(h); return w ? (int)w->id : 0; }
static Wnd* child_at(Wnd* w, int x, int y, int* cx, int* cy) {
    // the topmost visible child under (x, y) (client coordinates of w), recursively
    Wnd* hit = nullptr;
    for (Wnd* c = w->first_child; c; c = c->next_sibling) {
        if (!c->visible) continue;
        int lx = x - c->rect.left - c->nc_l, ly = y - c->rect.top - c->nc_t;
        if (x >= c->rect.left && y >= c->rect.top && x < c->rect.right && y < c->rect.bottom) { hit = c; *cx = lx; *cy = ly; }
    }
    if (!hit) { *cx = x; *cy = y; return w; }
    int nx, ny;
    Wnd* deeper = child_at(hit, *cx, *cy, &nx, &ny);
    *cx = nx; *cy = ny;
    return deeper;
}
static HWND WINAPI WindowFromPoint(POINT p) {
    for (int i = 15; i >= 1; i--) {
        Wnd* t = by_kwin[i];
        if (!t || !t->visible) continue;
        int x, y;
        top_screen_client(t, &x, &y);
        if (p.x >= x && p.y >= y && p.x < x + t->cw && p.y < y + t->ch) { int cx, cy; return child_at(t, p.x - x, p.y - y, &cx, &cy)->h; }
    }
    return nullptr;
}
static HWND WINAPI ChildWindowFromPoint(HWND h, POINT p) {
    Wnd* w = wnd_get(h);
    if (!w) return nullptr;
    for (Wnd* c = w->first_child; c; c = c->next_sibling) if (c->visible && p.x >= c->rect.left && p.y >= c->rect.top && p.x < c->rect.right && p.y < c->rect.bottom) return c->h;
    return h;
}
static HWND WINAPI ChildWindowFromPointEx(HWND h, POINT p, u32 f) { (void)f; return ChildWindowFromPoint(h, p); }
static HWND WINAPI RealChildWindowFromPoint(HWND h, POINT p) { return ChildWindowFromPoint(h, p); }
static u32 WINAPI GetWindowThreadProcessId(HWND h, u32* pid) { Wnd* w = wnd_get(h); if (pid) *pid = 0x40; return w ? w->tid : 0; }
static BOOL WINAPI IsGUIThread(BOOL convert) { (void)convert; return TRUE; }
static BOOL WINAPI IsWindowUnicode(HWND h) { Wnd* w = wnd_get(h); return w && w->unicode; }

// ---------------------------------------------------------------- window text and long values
static void set_text(Wnd* w, const WCHAR* t) {
    if (w->text) ifree(w->text);
    w->text = t ? wdup(t) : nullptr;
    if (w->kwin) {
        char u[64] = "";
        if (t) utf8_from_wide(t, -1, u, sizeof(u));
        if (!u[0] && t && *t) slcpy(u, "?", sizeof(u));
        si::win_ctl(w->kwin, si::WC_TITLE, (si::word)u);
    }
    if (w->parent) invalidate(w, nullptr, true);
}
static BOOL WINAPI SetWindowTextW(HWND h, const WCHAR* t) { Wnd* w = wnd_or_error(h); if (!w) return FALSE; return (BOOL)send(w, WM_SETTEXT, 0, (LPARAM)t, true); }
static BOOL WINAPI SetWindowTextA(HWND h, const char* t) { Wnd* w = wnd_or_error(h); if (!w) return FALSE; return (BOOL)send(w, WM_SETTEXT, 0, (LPARAM)t, false); }
static int WINAPI GetWindowTextW(HWND h, WCHAR* out, int n) { Wnd* w = wnd_or_error(h); if (!w || n <= 0) return 0; out[0] = 0; return (int)send(w, WM_GETTEXT, (WPARAM)n, (LPARAM)out, true); }
static int WINAPI GetWindowTextA(HWND h, char* out, int n) { Wnd* w = wnd_or_error(h); if (!w || n <= 0) return 0; out[0] = 0; return (int)send(w, WM_GETTEXT, (WPARAM)n, (LPARAM)out, false); }
static int WINAPI GetWindowTextLengthW(HWND h) { Wnd* w = wnd_or_error(h); return w ? (int)send(w, WM_GETTEXTLENGTH, 0, 0, true) : 0; }
static int WINAPI GetWindowTextLengthA(HWND h) { Wnd* w = wnd_or_error(h); return w ? (int)send(w, WM_GETTEXTLENGTH, 0, 0, false) : 0; }
static int WINAPI InternalGetWindowText(HWND h, WCHAR* out, int n) { Wnd* w = wnd_get(h); if (!w || n <= 0) return 0; int k = w->text ? (int)wlen(w->text) : 0; if (k >= n) k = n - 1; if (k) memcpy(out, w->text, (usize)k * 2); out[k] = 0; return k; }
static int WINAPI GetClassNameW(HWND h, WCHAR* out, int n) { Wnd* w = wnd_get(h); if (!w || n <= 0) return 0; int k = (int)wlen(w->cls->name); if (k >= n) k = n - 1; memcpy(out, w->cls->name, (usize)k * 2); out[k] = 0; return k; }
static int WINAPI GetClassNameA(HWND h, char* out, int n) { Wnd* w = wnd_get(h); if (!w || n <= 0) return 0; utf8_from_wide(w->cls->name, -1, out, n); out[n - 1] = 0; return (int)slen(out); }
static u32 WINAPI RealGetWindowClassW(HWND h, WCHAR* out, u32 n) { return (u32)GetClassNameW(h, out, (int)n); }

enum { GWLP_WNDPROC = -4, GWLP_HINSTANCE = -6, GWLP_HWNDPARENT = -8, GWLP_ID = -12, GWL_STYLE = -16, GWL_EXSTYLE = -20, GWLP_USERDATA = -21 };
static i64 get_long(Wnd* w, int i) {
    switch (i) {
    case GWLP_WNDPROC: return (i64)w->proc;
    case GWLP_HINSTANCE: return (i64)w->inst;
    case GWLP_HWNDPARENT: return (i64)(w->parent ? w->parent->h : w->owner ? w->owner->h : nullptr);
    case GWLP_ID: return (i64)w->id;
    case GWL_STYLE: return (i64)(u32)(w->style | (w->visible ? WS_VISIBLE : 0));
    case GWL_EXSTYLE: return w->exstyle;
    case GWLP_USERDATA: return w->userdata;
    }
    if (i >= 0 && i + 8 <= w->nextra) return *(i64*)(w->extra + i);
    if (i >= 0 && i + 4 <= w->nextra) return *(i32*)(w->extra + i);
    set_error(ERROR_INVALID_PARAMETER);
    return 0;
}
static i64 set_long(Wnd* w, int i, i64 v, bool unicode, bool wide64) {
    i64 old = get_long(w, i);
    switch (i) {
    case GWLP_WNDPROC: w->proc = (WNDPROC)v; w->unicode = unicode; return old;
    case GWLP_HINSTANCE: w->inst = (HINSTANCE)v; return old;
    case GWLP_HWNDPARENT: w->owner = wnd_get((HWND)v); return old;
    case GWLP_ID: w->id = (u64)v; return old;
    case GWL_STYLE: w->style = (u32)v; return old;
    case GWL_EXSTYLE: w->exstyle = (u32)v; return old;
    case GWLP_USERDATA: w->userdata = v; return old;
    }
    if (i >= 0 && wide64 && i + 8 <= w->nextra) { *(i64*)(w->extra + i) = v; return old; }
    if (i >= 0 && i + 4 <= w->nextra) { *(i32*)(w->extra + i) = (i32)v; return (i32)old; }
    set_error(ERROR_INVALID_PARAMETER);
    return 0;
}
static i64 WINAPI GetWindowLongPtrW(HWND h, int i) { Wnd* w = wnd_or_error(h); return w ? get_long(w, i) : 0; }
static i64 WINAPI GetWindowLongPtrA(HWND h, int i) { return GetWindowLongPtrW(h, i); }
static i64 WINAPI SetWindowLongPtrW(HWND h, int i, i64 v) { Wnd* w = wnd_or_error(h); return w ? set_long(w, i, v, true, true) : 0; }
static i64 WINAPI SetWindowLongPtrA(HWND h, int i, i64 v) { Wnd* w = wnd_or_error(h); return w ? set_long(w, i, v, false, true) : 0; }
static i32 WINAPI GetWindowLongW(HWND h, int i) { return (i32)GetWindowLongPtrW(h, i); }
static i32 WINAPI GetWindowLongA(HWND h, int i) { return (i32)GetWindowLongPtrW(h, i); }
static i32 WINAPI SetWindowLongW(HWND h, int i, i32 v) { Wnd* w = wnd_or_error(h); return w ? (i32)set_long(w, i, v, true, false) : 0; }
static i32 WINAPI SetWindowLongA(HWND h, int i, i32 v) { Wnd* w = wnd_or_error(h); return w ? (i32)set_long(w, i, v, false, false) : 0; }
static u16 WINAPI GetWindowWord(HWND h, int i) { return (u16)GetWindowLongPtrW(h, i); }
static u16 WINAPI SetWindowWord(HWND h, int i, u16 v) { Wnd* w = wnd_get(h); if (!w || i < 0 || i + 2 > w->nextra) return 0; u16 o = *(u16*)(w->extra + i); *(u16*)(w->extra + i) = v; return o; }
enum { GCLP_MENUNAME = -8, GCLP_HBRBACKGROUND = -10, GCLP_HCURSOR = -12, GCLP_HICON = -14, GCLP_HMODULE = -16, GCL_CBWNDEXTRA = -18, GCL_CBCLSEXTRA = -20,
       GCLP_WNDPROC = -24, GCL_STYLE = -26, GCW_ATOM = -32, GCLP_HICONSM = -34 };
static u64 WINAPI GetClassLongPtrW(HWND h, int i) {
    Wnd* w = wnd_or_error(h);
    if (!w) return 0;
    Cls* c = w->cls;
    switch (i) {
    case GCLP_HBRBACKGROUND: return (u64)c->brush;
    case GCLP_HCURSOR: return (u64)c->cursor;
    case GCLP_HICON: return (u64)c->icon;
    case GCLP_HICONSM: return (u64)c->icon_sm;
    case GCLP_HMODULE: return (u64)c->inst;
    case GCL_CBWNDEXTRA: return (u64)c->wnd_extra;
    case GCL_CBCLSEXTRA: return (u64)c->cls_extra;
    case GCLP_WNDPROC: return (u64)c->proc;
    case GCL_STYLE: return c->style;
    case GCW_ATOM: return c->atom;
    }
    if (i >= 0 && i + 8 <= c->cls_extra) return *(u64*)(c->extra + i);
    return 0;
}
static u64 WINAPI GetClassLongPtrA(HWND h, int i) { return GetClassLongPtrW(h, i); }
static u64 WINAPI SetClassLongPtrW(HWND h, int i, i64 v) {
    Wnd* w = wnd_or_error(h);
    if (!w) return 0;
    Cls* c = w->cls;
    u64 old = GetClassLongPtrW(h, i);
    switch (i) {
    case GCLP_HBRBACKGROUND: c->brush = (HANDLE)v; break;
    case GCLP_HCURSOR: c->cursor = (HANDLE)v; break;
    case GCLP_HICON: c->icon = (HANDLE)v; break;
    case GCLP_HICONSM: c->icon_sm = (HANDLE)v; break;
    case GCLP_WNDPROC: c->proc = (WNDPROC)v; break;
    case GCL_STYLE: c->style = (u32)v; break;
    default: if (i >= 0 && i + 8 <= c->cls_extra) *(i64*)(c->extra + i) = v;
    }
    return old;
}
static u64 WINAPI SetClassLongPtrA(HWND h, int i, i64 v) { return SetClassLongPtrW(h, i, v); }
static u32 WINAPI GetClassLongW(HWND h, int i) { return (u32)GetClassLongPtrW(h, i); }
static u32 WINAPI GetClassLongA(HWND h, int i) { return (u32)GetClassLongPtrW(h, i); }
static u32 WINAPI SetClassLongW(HWND h, int i, i32 v) { return (u32)SetClassLongPtrW(h, i, v); }
static u32 WINAPI SetClassLongA(HWND h, int i, i32 v) { return (u32)SetClassLongPtrW(h, i, v); }
static BOOL WINAPI SetPropW(HWND h, const WCHAR* name, HANDLE v) {
    Wnd* w = wnd_get(h);
    if (!w) return FALSE;
    WCHAR atom[8];
    if ((u64)name < 0x10000) { atom[0] = '#'; atom[1] = (WCHAR)(u64)name; atom[2] = 0; name = atom; }
    for (Wnd::Prop* p = w->props; p; p = p->next) if (!wcmp(p->name, name)) { p->value = v; return TRUE; }
    Wnd::Prop* p = (Wnd::Prop*)izalloc(sizeof(Wnd::Prop));
    p->name = wdup(name);
    p->value = v;
    p->next = w->props;
    w->props = p;
    return TRUE;
}
static HANDLE WINAPI GetPropW(HWND h, const WCHAR* name) {
    Wnd* w = wnd_get(h);
    if (!w) return nullptr;
    WCHAR atom[8];
    if ((u64)name < 0x10000) { atom[0] = '#'; atom[1] = (WCHAR)(u64)name; atom[2] = 0; name = atom; }
    for (Wnd::Prop* p = w->props; p; p = p->next) if (!wcmp(p->name, name)) return p->value;
    return nullptr;
}
static HANDLE WINAPI RemovePropW(HWND h, const WCHAR* name) {
    Wnd* w = wnd_get(h);
    if (!w) return nullptr;
    WCHAR atom[8];
    if ((u64)name < 0x10000) { atom[0] = '#'; atom[1] = (WCHAR)(u64)name; atom[2] = 0; name = atom; }
    for (Wnd::Prop** p = &w->props; *p; p = &(*p)->next) if (!wcmp((*p)->name, name)) { HANDLE v = (*p)->value; *p = (*p)->next; return v; }
    return nullptr;
}
static WCHAR* prop_name_a(const char* n, WCHAR* buf) { if ((u64)n < 0x10000) return (WCHAR*)n; wide_from_utf8(n, -1, buf, 128); return buf; }
static BOOL WINAPI SetPropA(HWND h, const char* n, HANDLE v) { WCHAR b[128]; return SetPropW(h, prop_name_a(n, b), v); }
static HANDLE WINAPI GetPropA(HWND h, const char* n) { WCHAR b[128]; return GetPropW(h, prop_name_a(n, b)); }
static HANDLE WINAPI RemovePropA(HWND h, const char* n) { WCHAR b[128]; return RemovePropW(h, prop_name_a(n, b)); }

// ---------------------------------------------------------------- focus, activation, capture
static void set_focus_to(MsgQ* q, HWND h) {
    HWND old = q->focus;
    if (old == h) return;
    q->focus = h;
    if (Wnd* o = wnd_get(old)) send(o, WM_KILLFOCUS, (WPARAM)h, 0);
    if (Wnd* n = wnd_get(h)) send(n, WM_SETFOCUS, (WPARAM)old, 0);
}
static HWND WINAPI SetFocus(HWND h) {
    MsgQ* q = my_queue();
    HWND old = q->focus;
    if (h && !wnd_get(h)) { set_error(ERROR_INVALID_WINDOW_HANDLE); return nullptr; }
    set_focus_to(q, h);
    return old;
}
static HWND WINAPI GetFocus() { return my_queue()->focus; }
static HWND WINAPI GetActiveWindow() { return my_queue()->active; }
static HWND WINAPI SetActiveWindow(HWND h) {
    MsgQ* q = my_queue();
    HWND old = q->active;
    Wnd* w = wnd_get(h);
    if (!w) return nullptr;
    w = wnd_top(w);
    q->active = w->h;
    if (w->kwin) si::win_ctl(w->kwin, si::WC_FOCUS);
    return old;
}
static HWND WINAPI GetForegroundWindow() {
    for (int i = 1; i < 16; i++) if (by_kwin[i] && si::win_ctl(i, si::WC_IS_FOCUSED) == 1) return by_kwin[i]->h;
    return my_queue()->active;
}
static BOOL WINAPI SetForegroundWindow(HWND h) { return SetActiveWindow(h) || wnd_get(h); }
static BOOL WINAPI BringWindowToTop(HWND h) { return SetForegroundWindow(h); }
static BOOL WINAPI AllowSetForegroundWindow(u32 pid) { (void)pid; return TRUE; }
static BOOL WINAPI LockSetForegroundWindow(u32 c) { (void)c; return TRUE; }
static BOOL WINAPI SwitchToThisWindow(HWND h, BOOL f) { (void)f; return SetForegroundWindow(h); }
static HWND WINAPI SetCapture(HWND h) { MsgQ* q = my_queue(); HWND old = q->capture; q->capture = h; return old; }
static BOOL WINAPI ReleaseCapture() { my_queue()->capture = nullptr; return TRUE; }
static HWND WINAPI GetCapture() { return my_queue()->capture; }
struct TRACKMOUSEEVENT { u32 cbSize, dwFlags; HWND hwndTrack; u32 dwHoverTime; };
static BOOL WINAPI TrackMouseEvent(TRACKMOUSEEVENT* t) { Wnd* w = wnd_get(t->hwndTrack); if (w) w->track_leave = (t->dwFlags & 2) && !(t->dwFlags & 0x80000000); return TRUE; }
static BOOL WINAPI _TrackMouseEvent(TRACKMOUSEEVENT* t) { return TrackMouseEvent(t); }
static HWND WINAPI GetLastActivePopup(HWND h) { return h; }
static BOOL WINAPI FlashWindow(HWND h, BOOL inv) { (void)h; (void)inv; return TRUE; }
static BOOL WINAPI FlashWindowEx(void* fi) { (void)fi; return TRUE; }
static BOOL WINAPI CloseWindow(HWND h) { Wnd* w = wnd_get(h); if (!w || !w->kwin) return FALSE; si::win_ctl(w->kwin, si::WC_MINIMIZE); w->minimized = true; return TRUE; }
static BOOL WINAPI OpenIcon(HWND h) { return ShowWindow_(h, 9); }

// ---------------------------------------------------------------- keyboard
static u8 vk_from_key(int key, int ch, int mods) {
    (void)mods;
    if (key >= 'a' && key <= 'z') return (u8)(key - 32);
    if (key >= 'A' && key <= 'Z') return (u8)key;
    if (key >= '0' && key <= '9') return (u8)key;
    switch (key) {
    case 8: return 0x08; case 9: return 0x09; case 10: case 13: return 0x0D; case 27: return 0x1B; case ' ': return 0x20;
    case 0x100: return 0x26; case 0x101: return 0x28; case 0x102: return 0x25; case 0x103: return 0x27;    // up down left right
    case 0x104: return 0x24; case 0x105: return 0x23; case 0x106: return 0x21; case 0x107: return 0x22;    // home end pgup pgdn
    case 0x108: return 0x2D; case 0x109: return 0x2E;                                                      // insert delete
    case 0x116: case 0x117: return 0x10;                                                                   // shift
    case 0x118: case 0x119: return 0x11;                                                                   // ctrl
    case 0x11A: case 0x11B: return 0x12;                                                                   // alt
    case 0x11C: return 0x14;                                                                               // caps lock
    case 0x11D: return 0x5B;                                                                               // super
    case ';': case ':': return 0xBA; case '=': case '+': return 0xBB; case ',': case '<': return 0xBC; case '-': case '_': return 0xBD;
    case '.': case '>': return 0xBE; case '/': case '?': return 0xBF; case '`': case '~': return 0xC0; case '[': case '{': return 0xDB;
    case '\\': case '|': return 0xDC; case ']': case '}': return 0xDD; case '\'': case '"': return 0xDE;
    case '!': return '1'; case '@': return '2'; case '#': return '3'; case '$': return '4'; case '%': return '5'; case '^': return '6';
    case '&': return '7'; case '*': return '8'; case '(': return '9'; case ')': return '0';
    }
    if (key >= 0x10A && key <= 0x115) return (u8)(0x70 + key - 0x10A);                                   // F1..F12
    if (ch >= 'a' && ch <= 'z') return (u8)(ch - 32);
    return 0;
}
static u32 scan_of(u8 vk) {
    static const u8 sc[256] = {
        0, 0, 0, 0x46, 0, 0, 0, 0, 0x0E, 0x0F, 0, 0, 0x4C, 0x1C, 0, 0, 0x2A, 0x1D, 0x38, 0, 0x3A, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0,
        0x39, 0x49, 0x51, 0x4F, 0x47, 0x4B, 0x48, 0x4D, 0x50, 0, 0, 0, 0x54, 0x52, 0x53, 0, 0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0, 0, 0, 0, 0, 0, 0, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
        0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, 0x5B, 0x5C, 0x5D, 0, 0, 0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D,
        0x47, 0x48, 0x49, 0x37, 0x4E, 0, 0x4A, 0x53, 0x35, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
    };
    if (vk >= 0xBA) {
        switch (vk) { case 0xBA: return 0x27; case 0xBB: return 0x0D; case 0xBC: return 0x33; case 0xBD: return 0x0C; case 0xBE: return 0x34;
                      case 0xBF: return 0x35; case 0xC0: return 0x29; case 0xDB: return 0x1A; case 0xDC: return 0x2B; case 0xDD: return 0x1B; case 0xDE: return 0x28; }
        return 0;
    }
    return sc[vk];
}
static short WINAPI GetKeyState(int vk) { MsgQ* q = my_queue(); u8 s = q->keys[vk & 255]; return (short)(((s & 0x80) ? 0xFF80 : 0) | (s & 1)); }
static short WINAPI GetAsyncKeyState(int vk) { MsgQ* q = my_queue(); return (q->keys[vk & 255] & 0x80) ? (short)0x8000 : 0; }
static BOOL WINAPI GetKeyboardState(u8* out) { memcpy(out, my_queue()->keys, 256); return TRUE; }
static BOOL WINAPI SetKeyboardState(const u8* in) { memcpy(my_queue()->keys, in, 256); return TRUE; }
static u32 WINAPI MapVirtualKeyW(u32 code, u32 type) {
    if (type == 0 || type == 3) return scan_of((u8)code);         // MAPVK_VK_TO_VSC
    if (type == 2) return code >= 'A' && code <= 'Z' ? code : code >= '0' && code <= '9' ? code : code == 0x20 ? ' ' : 0;   // MAPVK_VK_TO_CHAR
    if (type == 1 || type == 4) { for (u32 v = 1; v < 256; v++) if (scan_of((u8)v) == code) return v; return 0; }   // MAPVK_VSC_TO_VK
    return 0;
}
static u32 WINAPI MapVirtualKeyA(u32 code, u32 type) { return MapVirtualKeyW(code, type); }
static u32 WINAPI MapVirtualKeyExW(u32 code, u32 type, HANDLE l) { (void)l; return MapVirtualKeyW(code, type); }
static short WINAPI VkKeyScanW(WCHAR c) { u8 vk = vk_from_key(c, c, 0); bool sh = (c >= 'A' && c <= 'Z') || schr("!@#$%^&*()_+{}|:\"<>?~", (char)c); return vk ? (short)(vk | (sh ? 0x100 : 0)) : -1; }
static short WINAPI VkKeyScanA(char c) { return VkKeyScanW((u8)c); }
static short WINAPI VkKeyScanExW(WCHAR c, HANDLE l) { (void)l; return VkKeyScanW(c); }
static HANDLE WINAPI GetKeyboardLayout(u32 tid) { (void)tid; return (HANDLE)(u64)0x04090409; }
static int WINAPI GetKeyboardLayoutList(int n, HANDLE* out) { if (n && out) out[0] = (HANDLE)(u64)0x04090409; return 1; }
static BOOL WINAPI GetKeyboardLayoutNameW(WCHAR* out) { wide_from_utf8("00000409", -1, out, 9); return TRUE; }
static HANDLE WINAPI ActivateKeyboardLayout(HANDLE h, u32 f) { (void)h; (void)f; return (HANDLE)(u64)0x04090409; }
static int WINAPI GetKeyboardType(int t) { return t == 0 ? 4 : t == 2 ? 12 : 0; }
static int WINAPI GetKeyNameTextW(i32 lp, WCHAR* out, int n) {
    u32 sc = (lp >> 16) & 0xFF;
    u32 vk = MapVirtualKeyW(sc, 1);
    char b[16];
    if (vk >= 'A' && vk <= 'Z') snfmt(b, sizeof(b), "%c", vk);
    else if (vk >= 0x70 && vk <= 0x7B) snfmt(b, sizeof(b), "F%u", vk - 0x6F);
    else snfmt(b, sizeof(b), "Key %u", vk);
    return wide_from_utf8(b, -1, out, n) - 1;
}
static int WINAPI GetKeyNameTextA(i32 lp, char* out, int n) { WCHAR w[32]; GetKeyNameTextW(lp, w, 32); return utf8_from_wide(w, -1, out, n) - 1; }
static int WINAPI ToUnicode(u32 vk, u32 sc, const u8* state, WCHAR* out, int n, u32 f) {
    (void)sc; (void)f;
    if (n < 1) return 0;
    bool shift = state && (state[0x10] & 0x80);
    if (vk >= 'A' && vk <= 'Z') { out[0] = (WCHAR)(shift ? vk : vk + 32); return 1; }
    if (vk >= '0' && vk <= '9') { out[0] = (WCHAR)(shift ? ")!@#$%^&*("[vk - '0'] : vk); return 1; }
    if (vk == 0x20 || vk == 0x0D || vk == 0x08 || vk == 0x09 || vk == 0x1B) { out[0] = (WCHAR)vk; return 1; }
    return 0;
}
static int WINAPI ToUnicodeEx(u32 vk, u32 sc, const u8* st, WCHAR* out, int n, u32 f, HANDLE l) { (void)l; return ToUnicode(vk, sc, st, out, n, f); }
static int WINAPI ToAscii(u32 vk, u32 sc, const u8* st, u16* out, u32 f) { WCHAR w[2]; int r = ToUnicode(vk, sc, st, w, 2, f); if (r) *out = w[0]; return r; }
static void WINAPI keybd_event(u8 vk, u8 sc, u32 f, u64 x) { (void)vk; (void)sc; (void)f; (void)x; }
static void WINAPI mouse_event(u32 f, u32 x, u32 y, u32 d, u64 e) { (void)f; (void)x; (void)y; (void)d; (void)e; }
static u32 WINAPI SendInput(u32 n, void* in, int sz) { (void)in; (void)sz; return n; }
static BOOL WINAPI RegisterHotKey(HWND h, int id, u32 m, u32 vk) { (void)h; (void)id; (void)m; (void)vk; return TRUE; }
static BOOL WINAPI UnregisterHotKey(HWND h, int id) { (void)h; (void)id; return TRUE; }
static BOOL WINAPI BlockInput(BOOL b) { (void)b; return TRUE; }

// ---------------------------------------------------------------- the mouse cursor
static const struct { u32 id; int shape; } cursor_map[] = {
    {32512, 0}, {32513, 2}, {32514, 6}, {32515, 5}, {32516, 0}, {32640, 3}, {32641, 0}, {32642, 4}, {32643, 9}, {32644, 7}, {32645, 8},
    {32646, 3}, {32648, 0}, {32649, 1}, {32650, 6}, {32651, 0},
};
static HANDLE cur_cursor = (HANDLE)(u64)(0x10000 | 32512);
static int show_count = 0;
static HANDLE WINAPI LoadCursorW(HINSTANCE i, const WCHAR* name) { (void)i; u32 id = (u64)name < 0x10000 ? (u32)(u64)name : 32512; return (HANDLE)(u64)(0x10000 | id); }
static HANDLE WINAPI LoadCursorA(HINSTANCE i, const char* name) { (void)i; u32 id = (u64)name < 0x10000 ? (u32)(u64)name : 32512; return (HANDLE)(u64)(0x10000 | id); }
static HANDLE WINAPI LoadCursorFromFileW(const WCHAR* f) { (void)f; return (HANDLE)(u64)(0x10000 | 32512); }
static int cursor_shape(HANDLE c) {
    u64 v = (u64)c;
    if ((v >> 16) != 1) return 0;
    for (auto& m : cursor_map) if (m.id == (v & 0xFFFF)) return m.shape;
    return 0;
}
static HANDLE WINAPI SetCursor(HANDLE c) {
    HANDLE old = cur_cursor;
    cur_cursor = c;
    MsgQ* q = my_queue();
    Wnd* w = wnd_get(q->active);
    if (!w) for (int i = 1; i < 16; i++) if (by_kwin[i]) { w = by_kwin[i]; break; }
    if (w) w = wnd_top(w);
    if (w && w->kwin) si::win_ctl(w->kwin, si::WC_SET_CURSOR, c ? cursor_shape(c) : 0);
    return old;
}
static HANDLE WINAPI GetCursor() { return cur_cursor; }
static int WINAPI ShowCursor(BOOL s) { show_count += s ? 1 : -1; return show_count; }
static BOOL WINAPI GetCursorPos(POINT* p) {
    i64 v = si::win_ctl(0, si::WC_CURSOR_POS);
    p->x = (int)(i32)(v & 0xFFFFFFFF);
    p->y = (int)(i32)(v >> 32);
    return TRUE;
}
static BOOL WINAPI GetPhysicalCursorPos(POINT* p) { return GetCursorPos(p); }
static BOOL WINAPI SetCursorPos(int x, int y) { (void)x; (void)y; return TRUE; }
static BOOL WINAPI ClipCursor(const RECT* r) { (void)r; return TRUE; }
static BOOL WINAPI GetClipCursor(RECT* r) { *r = {0, 0, screen_wh(false), screen_wh(true)}; return TRUE; }
struct CURSORINFO { u32 cbSize, flags; HANDLE hCursor; POINT ptScreenPos; };
static BOOL WINAPI GetCursorInfo(CURSORINFO* ci) { ci->flags = show_count >= 0 ? 1 : 0; ci->hCursor = cur_cursor; GetCursorPos(&ci->ptScreenPos); return TRUE; }
static HANDLE WINAPI CreateCursor(HINSTANCE i, int x, int y, int w, int h, const void* a, const void* b) { (void)i; (void)x; (void)y; (void)w; (void)h; (void)a; (void)b; return (HANDLE)(u64)(0x10000 | 32512); }
static BOOL WINAPI DestroyCursor(HANDLE c) { (void)c; return TRUE; }
static HANDLE WINAPI CopyCursor(HANDLE c) { return c; }
static BOOL WINAPI SetSystemCursor(HANDLE c, u32 id) { (void)c; (void)id; return TRUE; }
static u32 WINAPI GetDoubleClickTime() { return 500; }
static BOOL WINAPI SetDoubleClickTime(u32 t) { (void)t; return TRUE; }
static BOOL WINAPI SwapMouseButton(BOOL s) { (void)s; return FALSE; }

// icons: a handle that draws nothing
static HANDLE WINAPI LoadIconW(HINSTANCE i, const WCHAR* n) { (void)i; return (HANDLE)(u64)(0x20000 | ((u64)n < 0x10000 ? (u64)n & 0xFFFF : 32512)); }
static HANDLE WINAPI LoadIconA(HINSTANCE i, const char* n) { (void)i; return (HANDLE)(u64)(0x20000 | ((u64)n < 0x10000 ? (u64)n & 0xFFFF : 32512)); }
static HANDLE WINAPI CreateIconIndirect(void* ii) { (void)ii; return (HANDLE)(u64)0x2FFFF; }
static HANDLE WINAPI CreateIcon(HINSTANCE i, int w, int h, u8 p, u8 b, const u8* a, const u8* x) { (void)i; (void)w; (void)h; (void)p; (void)b; (void)a; (void)x; return (HANDLE)(u64)0x2FFFF; }
static HANDLE WINAPI CreateIconFromResourceEx(u8* d, u32 n, BOOL ic, u32 v, int w, int h, u32 f) { (void)d; (void)n; (void)ic; (void)v; (void)w; (void)h; (void)f; return (HANDLE)(u64)0x2FFFF; }
static BOOL WINAPI DestroyIcon(HANDLE h) { (void)h; return TRUE; }
static HANDLE WINAPI CopyIcon(HANDLE h) { return h; }
static BOOL WINAPI DrawIcon(HDC d, int x, int y, HANDLE i) { (void)d; (void)x; (void)y; (void)i; return TRUE; }
static BOOL WINAPI DrawIconEx(HDC d, int x, int y, HANDLE i, int w, int h, u32 s, HANDLE b, u32 f) { (void)d; (void)x; (void)y; (void)i; (void)w; (void)h; (void)s; (void)b; (void)f; return TRUE; }
static BOOL WINAPI GetIconInfo(HANDLE i, void* info) { (void)i; memset(info, 0, 32); return TRUE; }
static int WINAPI LookupIconIdFromDirectoryEx(u8* d, BOOL ic, int w, int h, u32 f) { (void)d; (void)ic; (void)w; (void)h; (void)f; return 1; }
static HANDLE WINAPI ExtractIconW(HINSTANCE i, const WCHAR* f, u32 n) { (void)i; (void)f; (void)n; return nullptr; }

// ---------------------------------------------------------------- timers
struct Timer { HWND hwnd; u64 id; u32 ms; u64 due; void* proc; u32 tid; bool used; };
static Timer timers[256];
static u64 next_timer_id = 1;
static u64 WINAPI SetTimer(HWND h, u64 id, u32 ms, void* proc) {
    if (h && !wnd_get(h)) { set_error(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    if (ms < 10) ms = 10;
    if (ms > 0x7FFFFFFF) ms = 0x7FFFFFFF;
    Guard g(user_lock);
    u32 tid = cur_thr()->id;
    if (!h) id = 0;
    for (auto& t : timers) if (t.used && t.hwnd == h && h && t.id == id) { t.ms = ms; t.due = now_ms() + ms; t.proc = proc; return id; }
    for (auto& t : timers)
        if (!t.used) {
            t = {h, h ? id : 0x7FFF + next_timer_id++, ms, now_ms() + ms, proc, tid, true};
            if (pump_owner && pump_owner != tid) si::event_post();
            return h ? (id ? id : 1) : t.id;
        }
    set_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}
static BOOL WINAPI KillTimer(HWND h, u64 id) {
    Guard g(user_lock);
    for (auto& t : timers) if (t.used && t.hwnd == h && t.id == id) { t.used = false; return TRUE; }
    set_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}
static void remove_timers(HWND h) { Guard g(user_lock); for (auto& t : timers) if (t.used && t.hwnd == h) t.used = false; }
static u64 WINAPI SetCoalescableTimer(HWND h, u64 id, u32 ms, void* proc, u32 tol) { (void)tol; return SetTimer(h, id, ms, proc); }

// ---------------------------------------------------------------- the message loop
static int mouse_buttons = 0;                                     // MK_LBUTTON 1, MK_RBUTTON 2, MK_MBUTTON 0x10
static u64 last_click_t[2];
static int last_click_x[2], last_click_y[2];

// turn one silicon event into messages for the owning thread's queue
static void deliver(const si::Event2& e) {
    if (e.type == si::EV_USER) return;
    if (e.win <= 0 || e.win >= 16) return;
    Wnd* t = by_kwin[e.win];
    if (!t || t->destroyed) return;
    MsgQ* q;
    { Guard g(user_lock); q = queue_for(t->tid); }
    switch (e.type) {
    case si::EV_KEY: {
        int key = e.a, ch = e.b, down = e.c, mods = e.d;
        u8 vk = vk_from_key(key, ch, mods);
        if (!vk) break;
        bool was = q->keys[vk] & 0x80;
        if (down) { q->keys[vk] |= 0x80; if (!was) q->keys[vk] ^= 1; } else q->keys[vk] &= (u8)~0x80;
        // left/right variants of the modifiers
        if (vk == 0x10) q->keys[key == 0x116 ? 0xA0 : 0xA1] = q->keys[vk] & 0x80;
        if (vk == 0x11) q->keys[key == 0x118 ? 0xA2 : 0xA3] = q->keys[vk] & 0x80;
        if (vk == 0x12) q->keys[key == 0x11A ? 0xA4 : 0xA5] = q->keys[vk] & 0x80;
        q->keys[0x10] = (q->keys[0x10] & 1) | ((mods & 1) ? 0x80 : (vk == 0x10 && down ? 0x80 : (vk == 0x10 ? 0 : q->keys[0x10] & 0x80)));
        if (down) {
            u32 c = (u32)ch;
            if (c == 10) c = 13;
            if ((mods & 2) && c >= 'a' && c <= 'z') c -= 'a' - 1;          // ctrl+letter: a control character
            else if ((mods & 2) && c >= 'A' && c <= 'Z') c -= 'A' - 1;
            if (vk == 0x2E || (vk >= 0x21 && vk <= 0x28) || (vk >= 0x70 && vk <= 0x7B) || vk == 0x10 || vk == 0x11 || vk == 0x12) c = 0;
            q->chars[vk] = c;
        }
        bool sys = (mods & 4) || vk == 0x79;                                  // alt held, or F10
        u32 msgid = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
        LPARAM lp = 1 | ((LPARAM)scan_of(vk) << 16) | ((mods & 4) ? (1u << 29) : 0) | ((down ? was : 1) ? (1u << 30) : 0) | (down ? 0 : (1u << 31));
        if (vk >= 0x21 && vk <= 0x2E) lp |= 1 << 24;                          // extended keys
        HWND target = q->focus && wnd_top(wnd_get(q->focus)) == t ? q->focus : t->h;
        post_to(q, target, msgid, vk, lp);
        break;
    }
    case si::EV_MOUSE: {
        int x = e.a, y = e.b, kind = e.d & 0xFF;
        bool left_held = e.d & 0x100;
        int tx, ty;
        top_screen_client(t, &tx, &ty);
        screen_mouse_x = tx + x; screen_mouse_y = ty + y;
        if (kind == 0 || kind == 5) {                                         // move, leave
            if (left_held) mouse_buttons |= 1; else mouse_buttons &= ~1;
        }
        int cx, cy;
        Wnd* target = q->capture && wnd_top(wnd_get(q->capture)) == t ? wnd_get(q->capture) : child_at(t, x, y, &cx, &cy);
        if (target == wnd_get(q->capture)) { int ox, oy; wnd_client_origin(target, &ox, &oy); cx = x - ox; cy = y - oy; }
        if (target && !target->enabled) target = t, cx = x, cy = y;
        WPARAM mk = (WPARAM)mouse_buttons | ((q->keys[0x10] & 0x80) ? 4 : 0) | ((q->keys[0x11] & 0x80) ? 8 : 0);
        u32 msgid = 0;
        int b = e.c == 2 ? 1 : 0;
        switch (kind) {
        case 0: msgid = WM_MOUSEMOVE; break;
        case 1: case 3: {
            mouse_buttons |= b ? 2 : 1;
            mk |= b ? 2 : 1;
            // double clicks: silicon sends its own; a class without CS_DBLCLKS gets a second down instead
            bool dbl = kind == 3;
            if (!dbl) {
                u64 now = now_ms();
                if (now - last_click_t[b] < 500 && (x > last_click_x[b] ? x - last_click_x[b] : last_click_x[b] - x) < 4 && (y > last_click_y[b] ? y - last_click_y[b] : last_click_y[b] - y) < 4) dbl = true;
                last_click_t[b] = dbl ? 0 : now; last_click_x[b] = x; last_click_y[b] = y;
            }
            if (dbl && !(target->cls->style & 8)) dbl = false;
            msgid = b ? (dbl ? WM_RBUTTONDBLCLK : WM_RBUTTONDOWN) : (dbl ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN);
            q->keys[b ? 2 : 1] |= 0x80;
            // a click activates and focuses
            if (target->h != q->focus && !(target->cls->style & 0x0)) {}
            break;
        }
        case 2: mouse_buttons &= b ? ~2 : ~1; mk &= b ? ~(WPARAM)2 : ~(WPARAM)1; msgid = b ? WM_RBUTTONUP : WM_LBUTTONUP; q->keys[b ? 2 : 1] &= 0x7F; break;
        case 4: {                                                             // wheel: screen coordinates, the focus window
            int delta = -e.c * 120;
            HWND to = q->focus && wnd_top(wnd_get(q->focus)) == t ? q->focus : t->h;
            post_to(q, to, WM_MOUSEWHEEL, (WPARAM)(mk | ((u32)(u16)(i16)delta << 16)), MAKELP(screen_mouse_x, screen_mouse_y));
            return;
        }
        case 5: if (target->track_leave) { target->track_leave = false; post_to(q, target->h, WM_MOUSELEAVE, 0, 0); } return;
        }
        if (!msgid) break;
        // tell the window which cursor to show (DefWindowProc picks the class cursor)
        if (msgid == WM_MOUSEMOVE) post_to(q, target->h, WM_SETCURSOR, (WPARAM)target->h, MAKELP(1, WM_MOUSEMOVE));
        post_to(q, target->h, msgid, mk, MAKELP(cx, cy));
        break;
    }
    case si::EV_CLOSE: post_to(q, t->h, WM_SYSCOMMAND, 0xF060, 0); break;               // SC_CLOSE
    case si::EV_RESIZE:
        resize_surface(t, e.a, e.b);
        post_to(q, t->h, WM_SIZE, t->maximized ? 2 : 0, MAKELP(t->cw, t->ch));
        invalidate(t, nullptr, true);
        break;
    case si::EV_FOCUS: {
        if (e.a) {
            q->active = t->h;
            post_to(q, t->h, WM_ACTIVATEAPP, TRUE, 0);
            post_to(q, t->h, WM_ACTIVATE, 1, 0);
            t->minimized = false;
        } else {
            post_to(q, t->h, WM_ACTIVATE, 0, 0);
            post_to(q, t->h, WM_ACTIVATEAPP, FALSE, 0);
            memset(q->keys, 0, sizeof(q->keys));                             // keys held when focus left are let go
            mouse_buttons = 0;
        }
        break;
    }
    case si::EV_MOVE: {
        int cx = e.a, cy = e.b;
        int w = t->rect.right - t->rect.left, h = t->rect.bottom - t->rect.top;
        t->rect = {cx - t->nc_l, cy - t->nc_t, cx - t->nc_l + w, cy - t->nc_t + h};
        post_to(q, t->h, WM_MOVE, 0, MAKELP(cx, cy));
        break;
    }
    }
}

// read what silicon has for this program; with wait, sleep until something arrives (or for at most ms)
static void pump(bool wait, u32 ms) {
    u32 me = cur_thr()->id;
    if (!__sync_bool_compare_and_swap(&pump_owner, 0u, me)) {
        if (pump_owner == me) return;
        if (wait) si::sleep(ms && ms < 5 ? ms : 5);                // another thread is reading silicon's events
        return;
    }
    si::Event2 e;
    bool got = false;
    while (si::event2(&e, false)) { deliver(e); got = true; }
    if (!got && wait) {
        if (si::event2(&e, true, ms ? ms : 0)) { deliver(e); while (si::event2(&e, false)) deliver(e); }
    }
    pump_owner = 0;
}

static bool in_range(u32 m, u32 lo, u32 hi) { return (!lo && !hi) || (m >= lo && m <= hi); }
static bool owned_by(Wnd* w, HWND filter) { if (!filter || filter == (HWND)(i64)-1) return true; for (; w; w = w->parent) if (w->h == filter) return true; return false; }

static Wnd* find_paint(Wnd* w, u32 tid) {
    if (!w->visible || w->destroyed || w->tid != tid) return nullptr;
    if (w->need_paint) return w;
    for (Wnd* c = w->first_child; c; c = c->next_sibling) if (Wnd* p = find_paint(c, tid)) return p;
    return nullptr;
}

// one message for this thread, if there is one: posted and input messages, then paint, then timers
static bool next_message(MSG* out, HWND hwnd, u32 lo, u32 hi, bool remove) {
    MsgQ* q = my_queue();
    u32 tid = q->tid;
    {
        Guard g(user_lock);
        for (int i = q->head; i != q->tail; i = (i + 1) % q->cap) {
            MSG& m = q->ring[i];
            if (hwnd == (HWND)(i64)-1 ? m.hwnd != nullptr : (hwnd && !owned_by(wnd_get(m.hwnd), hwnd))) continue;
            if (!in_range(m.message, lo, hi)) continue;
            *out = m;
            if (remove) {
                for (int j = i; j != q->head; j = (j - 1 + q->cap) % q->cap) q->ring[j] = q->ring[(j - 1 + q->cap) % q->cap];
                q->head = (q->head + 1) % q->cap;
            }
            return true;
        }
    }
    if (q->quit && in_range(WM_QUIT, lo, hi)) {
        out->hwnd = nullptr; out->message = WM_QUIT; out->wParam = (WPARAM)q->quit_code; out->lParam = 0; out->time = msg_time();
        if (remove) q->quit = false;
        return true;
    }
    if (in_range(WM_PAINT, lo, hi)) {
        for (int i = 1; i < MAX_WND; i++) {
            Wnd* w = wnds[i];
            if (!w || w->parent || w->destroyed) continue;
            Wnd* p = find_paint(w, tid);
            if (p && owned_by(p, hwnd)) { *out = {p->h, WM_PAINT, 0, 0, 0, msg_time(), {screen_mouse_x, screen_mouse_y}, 0}; return true; }
        }
    }
    if (in_range(WM_TIMER, lo, hi)) {
        u64 now = now_ms();
        Guard g(user_lock);
        for (auto& t : timers) {
            if (!t.used || t.tid != tid || now < t.due) continue;
            if (t.hwnd && !owned_by(wnd_get(t.hwnd), hwnd)) continue;
            *out = {t.hwnd, WM_TIMER, 0, (WPARAM)t.id, (LPARAM)t.proc, msg_time(), {screen_mouse_x, screen_mouse_y}, 0};
            if (remove) t.due = now + t.ms;
            return true;
        }
    }
    return false;
}
static u32 next_timer_wait() {
    u64 now = now_ms(), best = ~0ull;
    u32 tid = cur_thr()->id;
    Guard g(user_lock);
    for (auto& t : timers) if (t.used && t.tid == tid && t.due < best) best = t.due;
    if (best == ~0ull) return 0;
    return best <= now ? 1 : (u32)(best - now);
}
// an ANSI loop gets a non-ASCII WM_CHAR as UTF-8 bytes, one message each
static void to_ansi(MSG* m) {
    if ((m->message == WM_CHAR || m->message == WM_SYSCHAR) && m->wParam >= 0x80) {
        MsgQ* q = my_queue();
        WCHAR c = (WCHAR)m->wParam;
        char u[4];
        int k = utf8_from_wide(&c, 1, u, 4);
        m->wParam = (u8)u[0];
        for (int i = k - 1; i >= 1; i--) {                         // the rest go to the front of the queue
            Guard g(user_lock);
            q->head = (q->head - 1 + q->cap) % q->cap;
            q->ring[q->head] = *m;
            q->ring[q->head].wParam = (u8)u[i];
        }
    }
}
static BOOL get_message(MSG* m, HWND h, u32 lo, u32 hi, bool unicode) {
    for (;;) {
        pump(false, 0);
        if (next_message(m, h, lo, hi, true)) {
            if (!unicode) to_ansi(m);
            my_queue()->cur = *m;
            return m->message != WM_QUIT;
        }
        present_all();
        pump(true, next_timer_wait());
    }
}
static BOOL WINAPI GetMessageW(MSG* m, HWND h, u32 lo, u32 hi) { return get_message(m, h, lo, hi, true); }
static BOOL WINAPI GetMessageA(MSG* m, HWND h, u32 lo, u32 hi) { return get_message(m, h, lo, hi, false); }
static BOOL peek_message(MSG* m, HWND h, u32 lo, u32 hi, u32 flags, bool unicode) {
    pump(false, 0);
    present_all();
    if (!next_message(m, h, lo, hi, flags & 1)) {
        // a program that only peeks still lets the others run
        si::yield();
        return FALSE;
    }
    if (!unicode && (flags & 1)) to_ansi(m);
    my_queue()->cur = *m;
    return TRUE;
}
static BOOL WINAPI PeekMessageW(MSG* m, HWND h, u32 lo, u32 hi, u32 f) { return peek_message(m, h, lo, hi, f, true); }
static BOOL WINAPI PeekMessageA(MSG* m, HWND h, u32 lo, u32 hi, u32 f) { return peek_message(m, h, lo, hi, f, false); }
static BOOL WINAPI WaitMessage() {
    MSG m;
    for (;;) {
        pump(false, 0);
        if (next_message(&m, nullptr, 0, 0, false)) return TRUE;
        present_all();
        pump(true, next_timer_wait());
    }
}
static u32 WINAPI MsgWaitForMultipleObjectsEx(u32 n, const HANDLE* hs, u32 timeout, u32 mask, u32 flags) {
    (void)mask; (void)flags;
    u64 end = timeout == 0xFFFFFFFF ? ~0ull : now_ms() + timeout;
    auto wfmo = k32<u32 (WINAPI*)(u32, const HANDLE*, BOOL, u32)>("WaitForMultipleObjects");
    MSG m;
    for (;;) {
        if (n) { u32 r = wfmo(n, hs, FALSE, 0); if (r < n) return r; }
        pump(false, 0);
        if (next_message(&m, nullptr, 0, 0, false)) return n;     // WAIT_OBJECT_0 + n: input is waiting
        if (now_ms() >= end) return 258;
        present_all();
        u32 t = next_timer_wait();
        u64 left = end - now_ms();
        if (n) pump(true, 5);
        else pump(true, end == ~0ull ? t : (u32)(t && t < left ? t : left));
    }
}
static u32 WINAPI MsgWaitForMultipleObjects(u32 n, const HANDLE* hs, BOOL all, u32 timeout, u32 mask) { (void)all; return MsgWaitForMultipleObjectsEx(n, hs, timeout, mask, 0); }
static u32 WINAPI GetQueueStatus(u32 f) { MSG m; pump(false, 0); return next_message(&m, nullptr, 0, 0, false) ? (f & 0xFFFF) << 16 | (f & 0xFFFF) : 0; }
static BOOL WINAPI GetInputState() { return GetQueueStatus(0x407) != 0; }
static i32 WINAPI GetMessageTime() { return (i32)my_queue()->cur.time; }
static u32 WINAPI GetMessagePos() { POINT p = my_queue()->cur.pt; return (u32)(u16)p.x | ((u32)(u16)p.y << 16); }
static LPARAM WINAPI GetMessageExtraInfo() { return 0; }
static BOOL WINAPI TranslateMessage(const MSG* m) {
    if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN) return FALSE;
    MsgQ* q = my_queue();
    u32 c = q->chars[m->wParam & 255];
    if (!c) return FALSE;
    // the character goes to the front, so it arrives right after the key-down
    Guard g(user_lock);
    q->head = (q->head - 1 + q->cap) % q->cap;
    MSG& n = q->ring[q->head];
    n = *m;
    n.message = m->message == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR;
    n.wParam = c;
    return TRUE;
}
typedef void (WINAPI *TimerProc)(HWND, u32, u64, u32);
static LRESULT dispatch(const MSG* m, bool unicode) {
    if (m->message == WM_TIMER && m->lParam) { ((TimerProc)m->lParam)(m->hwnd, WM_TIMER, m->wParam, (u32)now_ms()); return 0; }
    Wnd* w = wnd_get(m->hwnd);
    if (!w) return 0;
    LRESULT r = call_proc(w->proc, w->unicode, m->hwnd, m->message, m->wParam, m->lParam, unicode);
    if (m->message == WM_PAINT && wnd_get(m->hwnd) && w->need_paint) {
        // a WM_PAINT handler that didn't paint: Windows validates nothing and would send it again; give up once
        static int nagged = 0;
        if (++nagged > 1000) { w->need_paint = false; nagged = 0; }
    }
    return r;
}
static LRESULT WINAPI DispatchMessageW(const MSG* m) { return dispatch(m, true); }
static LRESULT WINAPI DispatchMessageA(const MSG* m) { return dispatch(m, false); }
static void WINAPI PostQuitMessage(int code) { MsgQ* q = my_queue(); q->quit = true; q->quit_code = code; }
static BOOL post_message(HWND h, u32 m, WPARAM w, LPARAM l) {
    if (h == (HWND)(u64)0xFFFF) {                                  // HWND_BROADCAST
        for (int i = 1; i < 16; i++) if (by_kwin[i]) post_message(by_kwin[i]->h, m, w, l);
        return TRUE;
    }
    if (!h) { post_to(my_queue(), nullptr, m, w, l); return TRUE; }
    Wnd* wnd = wnd_or_error(h);
    if (!wnd) return FALSE;
    MsgQ* q;
    { Guard g(user_lock); q = queue_for(wnd->tid); }
    return post_to(q, h, m, w, l);
}
static BOOL WINAPI PostMessageW(HWND h, u32 m, WPARAM w, LPARAM l) { return post_message(h, m, w, l); }
static BOOL WINAPI PostMessageA(HWND h, u32 m, WPARAM w, LPARAM l) { return post_message(h, m, w, l); }
static BOOL WINAPI PostThreadMessageW(u32 tid, u32 m, WPARAM w, LPARAM l) { MsgQ* q; { Guard g(user_lock); q = queue_for(tid); } return post_to(q, nullptr, m, w, l); }
static BOOL WINAPI PostThreadMessageA(u32 tid, u32 m, WPARAM w, LPARAM l) { return PostThreadMessageW(tid, m, w, l); }
static LRESULT send_message(HWND h, u32 m, WPARAM w, LPARAM l, bool unicode) {
    if (h == (HWND)(u64)0xFFFF) { for (int i = 1; i < 16; i++) if (by_kwin[i]) send(by_kwin[i], m, w, l, unicode); return 0; }
    Wnd* wnd = wnd_or_error(h);
    if (!wnd) return 0;
    return send(wnd, m, w, l, unicode);
}
static LRESULT WINAPI SendMessageW(HWND h, u32 m, WPARAM w, LPARAM l) { return send_message(h, m, w, l, true); }
static LRESULT WINAPI SendMessageA(HWND h, u32 m, WPARAM w, LPARAM l) { return send_message(h, m, w, l, false); }
static LRESULT WINAPI SendMessageTimeoutW(HWND h, u32 m, WPARAM w, LPARAM l, u32 f, u32 t, u64* r) { (void)f; (void)t; LRESULT v = send_message(h, m, w, l, true); if (r) *r = (u64)v; return 1; }
static LRESULT WINAPI SendMessageTimeoutA(HWND h, u32 m, WPARAM w, LPARAM l, u32 f, u32 t, u64* r) { (void)f; (void)t; LRESULT v = send_message(h, m, w, l, false); if (r) *r = (u64)v; return 1; }
static BOOL WINAPI SendNotifyMessageW(HWND h, u32 m, WPARAM w, LPARAM l) { send_message(h, m, w, l, true); return TRUE; }
static BOOL WINAPI SendNotifyMessageA(HWND h, u32 m, WPARAM w, LPARAM l) { send_message(h, m, w, l, false); return TRUE; }
static BOOL WINAPI ReplyMessage(LRESULT r) { (void)r; return FALSE; }
static BOOL WINAPI InSendMessage() { return FALSE; }
static LRESULT WINAPI CallWindowProcW(WNDPROC p, HWND h, u32 m, WPARAM w, LPARAM l) { Wnd* wnd = wnd_get(h); return call_proc(p, wnd && p == wnd->proc ? wnd->unicode : true, h, m, w, l, true); }
static LRESULT WINAPI CallWindowProcA(WNDPROC p, HWND h, u32 m, WPARAM w, LPARAM l) { Wnd* wnd = wnd_get(h); return call_proc(p, wnd && p == wnd->proc ? wnd->unicode : false, h, m, w, l, false); }
static u32 WINAPI RegisterWindowMessageW(const WCHAR* n) { (void)n; static u32 next = 0xC100; return next++; }
static u32 WINAPI RegisterWindowMessageA(const char* n) { (void)n; static u32 next = 0xC800; return next++; }

// ---------------------------------------------------------------- the default window procedure
struct MINMAXINFO { POINT reserved, maxSize, maxPos, minTrack, maxTrack; };
static LRESULT def_proc(HWND h, u32 m, WPARAM w, LPARAM l, bool unicode) {
    Wnd* wnd = wnd_get(h);
    if (!wnd) return 0;
    switch (m) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
        if (cs && cs->lpszName && (u64)cs->lpszName >= 0x10000) { WCHAR* t = unicode ? wdup(cs->lpszName) : wide_dup((const char*)cs->lpszName); set_text(wnd, t); ifree(t); }
        return TRUE;
    }
    case WM_NCCALCSIZE: return 0;
    case WM_NCHITTEST: return 1;                                    // HTCLIENT
    case WM_NCACTIVATE: return TRUE;
    case WM_NCPAINT: return 0;
    case WM_CLOSE: DestroyWindow_(h); return 0;
    case WM_SYSCOMMAND:
        switch (w & 0xFFF0) {
        case 0xF060: send(wnd, WM_CLOSE, 0, 0); return 0;          // SC_CLOSE
        case 0xF020: ShowWindow_(h, 6); return 0;                  // SC_MINIMIZE
        case 0xF030: ShowWindow_(h, 3); return 0;                  // SC_MAXIMIZE
        case 0xF120: ShowWindow_(h, 9); return 0;                  // SC_RESTORE
        }
        return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: {
        HANDLE b = class_brush(wnd);
        Brush* br = (Brush*)gdi_get(b, G_BRUSH);
        if (!br) return 0;
        DC* dc = dc_get((HDC)w);
        if (!dc) return 0;
        RECT r = {0, 0, wnd->cw, wnd->ch};
        fill_rect_dc(dc, &r, br);
        return 1;
    }
    case WM_SETTEXT: { if (unicode) set_text(wnd, (const WCHAR*)l); else { WCHAR* t = l ? wide_dup((const char*)l) : nullptr; set_text(wnd, t); if (t) ifree(t); } return TRUE; }
    case WM_GETTEXT: {
        if (!w) return 0;
        const WCHAR* t = wnd->text ? wnd->text : L"";
        if (unicode) { usize n = wlen(t); if (n >= w) n = w - 1; memcpy((WCHAR*)l, t, n * 2); ((WCHAR*)l)[n] = 0; return (LRESULT)n; }
        int n = utf8_from_wide(t, -1, (char*)l, (int)w);
        if (!n) { ((char*)l)[w - 1] = 0; return (LRESULT)w - 1; }
        return n - 1;
    }
    case WM_GETTEXTLENGTH: return wnd->text ? (unicode ? (LRESULT)wlen(wnd->text) : (LRESULT)utf8_from_wide(wnd->text, -1, nullptr, 0) - 1) : 0;
    case WM_SETCURSOR:
        if ((l & 0xFFFF) == 1 && wnd->cls->cursor) { SetCursor(wnd->cls->cursor); return TRUE; }
        if ((l & 0xFFFF) == 1 && !wnd->cls->cursor && wnd->parent) return send(wnd->parent, m, w, l);
        return FALSE;
    case WM_MOUSEACTIVATE: return 1;                                // MA_ACTIVATE
    case WM_ACTIVATE: if ((w & 0xFFFF) && !wnd->parent) { MsgQ* q = my_queue(); if (!q->focus || wnd_top(wnd_get(q->focus)) != wnd) set_focus_to(q, h); } return 0;
    case WM_SYSKEYDOWN: if (w == 0x73 && (l & (1 << 29))) { post_message(wnd_top(wnd)->h, WM_SYSCOMMAND, 0xF060, 0); } return 0;    // Alt+F4
    case WM_SYSCHAR: return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)l;
        mm->maxSize = {screen_wh(false), screen_wh(true)};
        mm->maxPos = {0, 0};
        mm->minTrack = {136, 39};
        mm->maxTrack = {screen_wh(false) + 16, screen_wh(true) + 16};
        return 0;
    }
    case WM_SETICON: { HANDLE o = w ? wnd->icon : wnd->icon_small; if (w) wnd->icon = (HANDLE)l; else wnd->icon_small = (HANDLE)l; return (LRESULT)o; }
    case WM_GETICON: return (LRESULT)(w ? wnd->icon : wnd->icon_small);
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN: case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLORSCROLLBAR: {
        extern COLORREF WINAPI SetTextColor_(HDC, COLORREF);
        extern COLORREF WINAPI SetBkColor_(HDC, COLORREF);
        SetTextColor_((HDC)w, GetSysColor(m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX ? 8 : 18));
        SetBkColor_((HDC)w, GetSysColor(m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX ? 5 : 15));
        return (LRESULT)GetSysColorBrush(m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX ? 5 : 15);
    }
    case WM_QUERYENDSESSION: return TRUE;
    case WM_GETFONT: return 0;
    case WM_PARENTNOTIFY: return 0;
    case WM_SHOWWINDOW: return 0;
    case WM_GETDLGCODE: return 0;
    case WM_WINDOWPOSCHANGED: return 0;
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: return 0;
    case WM_MOUSEWHEEL: if (wnd->parent) return send(wnd->parent, m, w, l, unicode); return 0;
    }
    return 0;
}
COLORREF WINAPI SetTextColor_(HDC h, COLORREF c) { DC* dc = dc_get(h); if (!dc) return 0; COLORREF o = dc->st.text_color; dc->st.text_color = c; return o; }
COLORREF WINAPI SetBkColor_(HDC h, COLORREF c) { DC* dc = dc_get(h); if (!dc) return 0; COLORREF o = dc->st.bk_color; dc->st.bk_color = c; return o; }
static LRESULT WINAPI DefWindowProcW(HWND h, u32 m, WPARAM w, LPARAM l) { return def_proc(h, m, w, l, true); }
static LRESULT WINAPI DefWindowProcA(HWND h, u32 m, WPARAM w, LPARAM l) { return def_proc(h, m, w, l, false); }

// ---------------------------------------------------------------- rectangles
static BOOL WINAPI SetRect(RECT* r, int l, int t, int rr, int b) { if (!r) return FALSE; *r = {l, t, rr, b}; return TRUE; }
static BOOL WINAPI SetRectEmpty(RECT* r) { if (!r) return FALSE; *r = {0, 0, 0, 0}; return TRUE; }
static BOOL WINAPI CopyRect(RECT* d, const RECT* s) { if (!d || !s) return FALSE; *d = *s; return TRUE; }
static BOOL WINAPI InflateRect(RECT* r, int dx, int dy) { if (!r) return FALSE; r->left -= dx; r->right += dx; r->top -= dy; r->bottom += dy; return TRUE; }
static BOOL WINAPI OffsetRect(RECT* r, int dx, int dy) { if (!r) return FALSE; r->left += dx; r->right += dx; r->top += dy; r->bottom += dy; return TRUE; }
static BOOL WINAPI IsRectEmpty(const RECT* r) { return !r || r->right <= r->left || r->bottom <= r->top; }
static BOOL WINAPI IntersectRect(RECT* d, const RECT* a, const RECT* b) {
    RECT r = {a->left > b->left ? a->left : b->left, a->top > b->top ? a->top : b->top, a->right < b->right ? a->right : b->right, a->bottom < b->bottom ? a->bottom : b->bottom};
    if (r.right <= r.left || r.bottom <= r.top) { *d = {0, 0, 0, 0}; return FALSE; }
    *d = r;
    return TRUE;
}
static BOOL WINAPI UnionRect(RECT* d, const RECT* a, const RECT* b) {
    if (IsRectEmpty(a)) { if (IsRectEmpty(b)) { *d = {0, 0, 0, 0}; return FALSE; } *d = *b; return TRUE; }
    if (IsRectEmpty(b)) { *d = *a; return TRUE; }
    *d = {a->left < b->left ? a->left : b->left, a->top < b->top ? a->top : b->top, a->right > b->right ? a->right : b->right, a->bottom > b->bottom ? a->bottom : b->bottom};
    return TRUE;
}
static BOOL WINAPI SubtractRect(RECT* d, const RECT* a, const RECT* b) {
    RECT r = *a, x;
    if (!IntersectRect(&x, a, b)) { *d = r; return !IsRectEmpty(&r); }
    if (x.top == r.top && x.bottom == r.bottom) { if (x.left == r.left) r.left = x.right; else if (x.right == r.right) r.right = x.left; }
    else if (x.left == r.left && x.right == r.right) { if (x.top == r.top) r.top = x.bottom; else if (x.bottom == r.bottom) r.bottom = x.top; }
    *d = r;
    return !IsRectEmpty(&r);
}
static BOOL WINAPI PtInRect(const RECT* r, POINT p) { return r && p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
static BOOL WINAPI EqualRect(const RECT* a, const RECT* b) { return a && b && !memcmp(a, b, sizeof(RECT)); }

// ---------------------------------------------------------------- drawing helpers of user32
static int WINAPI FillRect(HDC h, const RECT* r, HANDLE br) {
    DC* dc = dc_get(h);
    if (!dc || !r) return 0;
    if ((u64)br > 0 && (u64)br <= 31) br = GetSysColorBrush((int)(u64)br - 1);
    Brush* b = (Brush*)gdi_get(br, G_BRUSH);
    if (!b) return 0;
    fill_rect_dc(dc, r, b);
    return 1;
}
static int WINAPI FrameRect(HDC h, const RECT* r, HANDLE br) {
    RECT e[4] = {{r->left, r->top, r->right, r->top + 1}, {r->left, r->bottom - 1, r->right, r->bottom}, {r->left, r->top, r->left + 1, r->bottom}, {r->right - 1, r->top, r->right, r->bottom}};
    if (r->right <= r->left || r->bottom <= r->top) return 0;
    for (auto& q : e) FillRect(h, &q, br);
    return 1;
}
static BOOL WINAPI InvertRect(HDC h, const RECT* r) {
    extern BOOL WINAPI PatBlt_(HDC, int, int, int, int, u32);
    return PatBlt_(h, r->left, r->top, r->right - r->left, r->bottom - r->top, 0x550009);
}
BOOL WINAPI PatBlt_(HDC h, int x, int y, int w, int hh, u32 rop) {
    auto pb = k32<BOOL (WINAPI*)(HDC, int, int, int, int, u32)>("PatBlt");
    return pb ? pb(h, x, y, w, hh, rop) : FALSE;
}
static void* gdi_fn(const char* name) { const char* dll; return find_builtin(name, &dll); }
static BOOL WINAPI DrawFocusRect(HDC h, const RECT* r) {
    DC* dc = dc_get(h);
    if (!dc) return FALSE;
    auto setpix = (COLORREF (WINAPI*)(HDC, int, int, COLORREF))gdi_fn("SetPixel");
    auto getpix = (COLORREF (WINAPI*)(HDC, int, int))gdi_fn("GetPixel");
    for (int x = r->left; x < r->right; x += 2) { setpix(h, x, r->top, getpix(h, x, r->top) ^ 0xFFFFFF); setpix(h, x, r->bottom - 1, getpix(h, x, r->bottom - 1) ^ 0xFFFFFF); }
    for (int y = r->top; y < r->bottom; y += 2) { setpix(h, r->left, y, getpix(h, r->left, y) ^ 0xFFFFFF); setpix(h, r->right - 1, y, getpix(h, r->right - 1, y) ^ 0xFFFFFF); }
    return TRUE;
}
static BOOL WINAPI DrawEdge(HDC h, RECT* r, u32 edge, u32 flags) {
    // raised: light top-left, dark bottom-right; sunken the other way round
    bool sunken_outer = edge & 2, sunken_inner = edge & 8, raised_outer = edge & 1, raised_inner = edge & 4;
    auto line = [&](int x0, int y0, int x1, int y1, int col) { RECT q = {x0, y0, x1, y1}; FillRect(h, &q, GetSysColorBrush(col)); };
    RECT q = *r;
    if (raised_outer || sunken_outer) {
        int tl = raised_outer ? 22 : 16, br = raised_outer ? 21 : 20;
        if (flags & 1) line(q.left, q.top, q.left + 1, q.bottom, tl);
        if (flags & 2) line(q.left, q.top, q.right, q.top + 1, tl);
        if (flags & 4) line(q.right - 1, q.top, q.right, q.bottom, br);
        if (flags & 8) line(q.left, q.bottom - 1, q.right, q.bottom, br);
        InflateRect(&q, -1, -1);
    }
    if (raised_inner || sunken_inner) {
        int tl = raised_inner ? 20 : 21, br = raised_inner ? 16 : 22;
        if (flags & 1) line(q.left, q.top, q.left + 1, q.bottom, tl);
        if (flags & 2) line(q.left, q.top, q.right, q.top + 1, tl);
        if (flags & 4) line(q.right - 1, q.top, q.right, q.bottom, br);
        if (flags & 8) line(q.left, q.bottom - 1, q.right, q.bottom, br);
        InflateRect(&q, -1, -1);
    }
    if (flags & 0x800) FillRect(h, &q, GetSysColorBrush(15));       // BF_MIDDLE
    if (flags & 0x2000) *r = q;                                     // BF_ADJUST
    return TRUE;
}
static BOOL WINAPI DrawFrameControl(HDC h, RECT* r, u32 type, u32 state) {
    FillRect(h, r, GetSysColorBrush(15));
    DrawEdge(h, r, (state & 0x200) ? 10 : 5, 15);
    (void)type;
    return TRUE;
}
static BOOL WINAPI DrawStateW(HDC h, HANDLE br, void* fn, LPARAM l, WPARAM w, int x, int y, int cx, int cy, u32 f) { (void)h; (void)br; (void)fn; (void)l; (void)w; (void)x; (void)y; (void)cx; (void)cy; (void)f; return TRUE; }
static BOOL WINAPI GrayStringW(HDC h, HANDLE br, void* fn, LPARAM l, int n, int x, int y, int cx, int cy) { (void)h; (void)br; (void)fn; (void)l; (void)n; (void)x; (void)y; (void)cx; (void)cy; return TRUE; }

static void IntersectClipRect_(HDC h, const RECT* r);
// DrawText: lines, word wrapping, alignment and & prefixes
static int draw_text(HDC h, const char* s, int n, RECT* r, u32 fmt) {
    DC* dc = dc_get(h);
    if (!dc || !r) return 0;
    if (n < 0) n = (int)slen(s);
    Font* f = dc->st.font;
    // remove & prefixes (&& is a single &)
    char* text = (char*)ialloc((usize)n + 1);
    int tn = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '&' && !(fmt & 0x800)) { if (i + 1 < n && s[i + 1] == '&') { text[tn++] = '&'; i++; } continue; }
        if (s[i] == '\t' && (fmt & 0x40)) { for (int k = 0; k < 4; k++) text[tn++] = ' '; continue; }
        text[tn++] = s[i];
    }
    text[tn] = 0;
    int width = r->right - r->left;
    struct Line { int start, len, w; };
    Line lines[512];
    int nl = 0;
    int i = 0;
    while (i <= tn && nl < 512) {
        int start = i, end = i;
        if (fmt & 0x20) end = tn;                                   // DT_SINGLELINE
        else while (end < tn && text[end] != '\n' && text[end] != '\r') end++;
        // word wrap inside [start, end)
        if ((fmt & 0x10) && !(fmt & 0x20)) {
            int ls = start;
            while (ls < end && nl < 512) {
                int fit = ls, last_space = -1;
                for (int k = ls; k <= end; k++) {
                    if (k == end || text[k] == ' ') {
                        if (text_width_utf8(f, text + ls, k - ls) <= width || fit == ls) { fit = k; if (k < end) last_space = k; }
                        else break;
                    }
                }
                if (fit == ls) {                                    // one word wider than the box: break it
                    fit = ls + 1;
                    while (fit < end && text_width_utf8(f, text + ls, fit + 1 - ls) <= width) fit++;
                }
                (void)last_space;
                lines[nl++] = {ls, fit - ls, text_width_utf8(f, text + ls, fit - ls)};
                ls = fit;
                while (ls < end && text[ls] == ' ') ls++;
            }
            if (start == end && nl < 512) lines[nl++] = {start, 0, 0};
        } else lines[nl++] = {start, end - start, text_width_utf8(f, text + start, end - start)};
        if (end >= tn) break;
        i = end + 1;
        if (text[end] == '\r' && i < tn && text[i] == '\n') i++;
    }
    int total_h = nl * f->lh;
    int maxw = 0;
    for (int k = 0; k < nl; k++) if (lines[k].w > maxw) maxw = lines[k].w;
    if (fmt & 0x400) {                                              // DT_CALCRECT
        r->right = r->left + ((fmt & 0x10) && maxw < width ? (maxw ? maxw : 0) : maxw);
        if ((fmt & 0x10) && maxw > width) r->right = r->left + width;
        r->bottom = r->top + total_h;
        ifree(text);
        return total_h;
    }
    int y = r->top;
    if (fmt & 0x20) {
        if (fmt & 4) y = r->top + (r->bottom - r->top - f->lh) / 2;   // DT_VCENTER
        else if (fmt & 8) y = r->bottom - f->lh;                    // DT_BOTTOM
    }
    DCState saved = dc->st;
    if (!(fmt & 0x100)) IntersectClipRect_(h, r);
    for (int k = 0; k < nl; k++) {
        Line& L = lines[k];
        const char* p = text + L.start;
        int len = L.len, w = L.w;
        char ell[600];
        if ((fmt & 0x8000) && w > width && len > 0) {               // DT_END_ELLIPSIS
            int keep = len;
            while (keep > 0) {
                memcpy(ell, p, (usize)keep);
                memcpy(ell + keep, "...", 4);
                w = text_width_utf8(f, ell, keep + 3);
                if (w <= width) break;
                keep--;
                while (keep > 0 && ((u8)p[keep] & 0xC0) == 0x80) keep--;
            }
            if (keep <= 0) { memcpy(ell, "...", 4); keep = 0; }
            p = ell;
            len = keep + 3;
        }
        int x = r->left;
        if (fmt & 1) x = r->left + (width - w) / 2;                 // DT_CENTER
        else if (fmt & 2) x = r->right - w;                         // DT_RIGHT
        draw_text_utf8(dc, x, y, p, len, nullptr, nullptr);
        y += f->lh;
    }
    dc->st = saved;
    ifree(text);
    return total_h;
}
static void IntersectClipRect_(HDC h, const RECT* r) {
    auto icr = (int (WINAPI*)(HDC, int, int, int, int))gdi_fn("IntersectClipRect");
    icr(h, r->left, r->top, r->right, r->bottom);
}
static int WINAPI DrawTextA(HDC h, const char* s, int n, RECT* r, u32 fmt) { return draw_text(h, s, n, r, fmt); }
static int WINAPI DrawTextW(HDC h, const WCHAR* s, int n, RECT* r, u32 fmt) {
    if (n < 0) n = (int)wlen(s);
    int k = utf8_from_wide(s, n, nullptr, 0);
    char* u = (char*)ialloc((usize)k + 1);
    utf8_from_wide(s, n, u, k);
    u[k] = 0;
    int res = draw_text(h, u, k, r, fmt);
    ifree(u);
    return res;
}
static int WINAPI DrawTextExW(HDC h, WCHAR* s, int n, RECT* r, u32 fmt, void* p) { (void)p; return DrawTextW(h, s, n, r, fmt); }
static int WINAPI DrawTextExA(HDC h, char* s, int n, RECT* r, u32 fmt, void* p) { (void)p; return DrawTextA(h, s, n, r, fmt); }
static i32 WINAPI TabbedTextOutW(HDC h, int x, int y, const WCHAR* s, int n, int nt, const int* tabs, int org) {
    (void)nt; (void)tabs; (void)org;
    RECT r = {x, y, x + 10000, y + 1000};
    DrawTextW(h, s, n, &r, 0x20 | 0x40 | 0x100 | 0x800);
    SIZE sz;
    auto ext = (BOOL (WINAPI*)(HDC, const WCHAR*, int, SIZE*))gdi_fn("GetTextExtentPoint32W");
    ext(h, s, n, &sz);
    return sz.cx | (sz.cy << 16);
}

// ---------------------------------------------------------------- MessageBox
struct MsgBox { const WCHAR* text; const WCHAR* caption; u32 type; int result; int buttons[3]; int nb; int hot; int def; RECT brect[3]; };
static const char* button_label(int id) {
    switch (id) { case 1: return "OK"; case 2: return "Cancel"; case 3: return "Abort"; case 4: return "Retry"; case 5: return "Ignore"; case 6: return "Yes"; case 7: return "No"; case 10: return "Try Again"; case 11: return "Continue"; }
    return "?";
}
static LRESULT WINAPI msgbox_proc(HWND h, u32 m, WPARAM w, LPARAM l) {
    MsgBox* mb = (MsgBox*)wnd_get(h)->userdata;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Wnd* wnd = wnd_get(h);
        RECT all = {0, 0, wnd->cw, wnd->ch};
        FillRect(dc, &all, GetSysColorBrush(5));
        RECT foot = {0, wnd->ch - 48, wnd->cw, wnd->ch};
        FillRect(dc, &foot, GetSysColorBrush(15));
        auto sel = (HGDIOBJ (WINAPI*)(HDC, HGDIOBJ))gdi_fn("SelectObject");
        auto bkm = (int (WINAPI*)(HDC, int))gdi_fn("SetBkMode");
        sel(dc, stock_object(17));
        bkm(dc, 1);
        int icon = (mb->type >> 4) & 7;
        int tx = 16;
        if (icon) {
            static const COLORREF ic[5] = {0, 0x2010E0, 0xD07000, 0x00A0F0, 0xD07000};
            HANDLE b = solid_brush_stock(ic[icon < 5 ? icon : 4]);
            auto sb = (HGDIOBJ (WINAPI*)(HDC, HGDIOBJ))gdi_fn("SelectObject");
            auto el = (BOOL (WINAPI*)(HDC, int, int, int, int))gdi_fn("Ellipse");
            sb(dc, b);
            sb(dc, stock_object(8));
            el(dc, 16, 18, 48, 50);
            auto stc = (COLORREF (WINAPI*)(HDC, COLORREF))gdi_fn("SetTextColor");
            stc(dc, 0xFFFFFF);
            RECT ir = {16, 18, 48, 50};
            DrawTextA(dc, icon == 1 ? "X" : icon == 2 ? "?" : "!", 1, &ir, 0x25);
            tx = 64;
        }
        auto stc = (COLORREF (WINAPI*)(HDC, COLORREF))gdi_fn("SetTextColor");
        stc(dc, 0);
        RECT tr = {tx, 22, wnd->cw - 16, wnd->ch - 56};
        DrawTextW(dc, mb->text, -1, &tr, 0x10 | 0x800);
        for (int i = 0; i < mb->nb; i++) {
            RECT b = mb->brect[i];
            FillRect(dc, &b, GetSysColorBrush(i == mb->hot ? 22 : 15));
            HANDLE frame = solid_brush_stock(i == mb->def ? 0xD77800 : 0xADADAD);
            FrameRect(dc, &b, frame);
            DrawTextA(dc, button_label(mb->buttons[i]), -1, &b, 0x25);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: {
        POINT p = {(i16)(l & 0xFFFF), (i16)((l >> 16) & 0xFFFF)};
        int hot = -1;
        for (int i = 0; i < mb->nb; i++) if (PtInRect(&mb->brect[i], p)) hot = i;
        if (hot != mb->hot) { mb->hot = hot; InvalidateRect(h, nullptr, FALSE); }
        if (m == WM_LBUTTONUP && hot >= 0) mb->result = mb->buttons[hot];
        return 0;
    }
    case WM_KEYDOWN:
        if (w == 0x0D || w == 0x20) mb->result = mb->buttons[mb->def];
        else if (w == 0x1B) { for (int i = 0; i < mb->nb; i++) if (mb->buttons[i] == 2 || mb->buttons[i] == 7) mb->result = mb->buttons[i]; if (mb->nb == 1) mb->result = mb->buttons[0]; }
        else if (w == 0x09 || w == 0x27) { mb->def = (mb->def + 1) % mb->nb; InvalidateRect(h, nullptr, FALSE); }
        else if (w == 0x25) { mb->def = (mb->def + mb->nb - 1) % mb->nb; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_CLOSE: {
        int r = 0;
        for (int i = 0; i < mb->nb; i++) if (mb->buttons[i] == 2) r = 2;
        if (mb->nb == 1) r = mb->buttons[0];
        if (r) mb->result = r;
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
static int message_box(HWND owner, const WCHAR* text, const WCHAR* caption, u32 type) {
    if (!caption) caption = L"Error";
    if (!text) text = L"";
    char t[600], c[200];
    utf8_from_wide(text, -1, t, sizeof(t));
    utf8_from_wide(caption, -1, c, sizeof(c));
    msg("MessageBox \"%s\": %s", c, t);
    MsgBox mb;
    memset(&mb, 0, sizeof(mb));
    mb.text = text; mb.caption = caption; mb.type = type; mb.hot = -1;
    static const int sets[7][3] = {{1}, {1, 2}, {3, 4, 5}, {6, 7, 2}, {6, 7}, {4, 2}, {2, 10, 11}};
    static const int counts[7] = {1, 2, 3, 3, 2, 2, 3};
    int set = (int)(type & 15);
    if (set > 6) set = 0;
    mb.nb = counts[set];
    for (int i = 0; i < mb.nb; i++) mb.buttons[i] = sets[set][i];
    mb.def = (int)((type >> 8) & 3);
    if (mb.def >= mb.nb) mb.def = 0;
    // a window sized to the text
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = msgbox_proc;
        wc.lpszClassName = L"#32770";
        wc.hCursor = LoadCursorW(nullptr, (const WCHAR*)(u64)32512);
        RegisterClassExW(&wc);
        registered = true;
    }
    HDC sdc = GetDC(nullptr);
    auto sel = (HGDIOBJ (WINAPI*)(HDC, HGDIOBJ))gdi_fn("SelectObject");
    sel(sdc, stock_object(17));
    int icon_w = ((type >> 4) & 7) ? 48 : 0;
    RECT tr = {0, 0, 360, 0};
    DrawTextW(sdc, text, -1, &tr, 0x10 | 0x400 | 0x800);
    extern BOOL WINAPI DeleteDC_(HDC);
    DeleteDC_(sdc);
    int cw = tr.right + 32 + icon_w, ch = (tr.bottom > 32 ? tr.bottom : 32) + 44 + 48;
    int bw = 88, bspace = mb.nb * bw + (mb.nb - 1) * 8 + 24;
    if (cw < bspace) cw = bspace;
    if (cw < 200) cw = 200;
    for (int i = 0; i < mb.nb; i++) {
        int x = cw - 12 - (mb.nb - i) * (bw + 8) + 8;
        mb.brect[i] = {x, ch - 38, x + bw, ch - 12};
    }
    Wnd* ow = wnd_get(owner);
    if (ow) ow = wnd_top(ow);
    bool owner_enabled = ow && ow->enabled;
    if (ow) EnableWindow(ow->h, FALSE);
    HWND h = CreateWindowExW(0, L"#32770", caption, WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, 0, cw + 6, ch + 29, owner, nullptr, nullptr, nullptr);
    if (!h) { if (ow && owner_enabled) EnableWindow(ow->h, TRUE); return 1; }
    wnd_get(h)->userdata = (i64)&mb;
    ShowWindow_(h, 1);
    UpdateWindow(h);
    MSG m;
    while (!mb.result) {
        if (!GetMessageW(&m, nullptr, 0, 0)) { PostQuitMessage((int)m.wParam); mb.result = mb.buttons[0]; break; }
        // input meant for the owner is swallowed while the box is up
        Wnd* target = wnd_get(m.hwnd);
        if (target && ow && wnd_top(target) == ow && (m.message >= WM_KEYDOWN && m.message <= WM_MBUTTONUP && m.message != WM_TIMER)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    DestroyWindow_(h);
    if (ow && owner_enabled) { EnableWindow(ow->h, TRUE); SetActiveWindow(ow->h); }
    msg("MessageBox \"%s\" closed with %s", c, button_label(mb.result));
    return mb.result;
}
BOOL WINAPI DeleteDC_(HDC h) { auto d = (BOOL (WINAPI*)(HDC))gdi_fn("DeleteDC"); return d(h); }
static int WINAPI MessageBoxW(HWND o, const WCHAR* t, const WCHAR* c, u32 type) { return message_box(o, t, c, type); }
static int WINAPI MessageBoxA(HWND o, const char* t, const char* c, u32 type) {
    WCHAR* wt = t ? wide_dup(t) : nullptr;
    WCHAR* wc = c ? wide_dup(c) : nullptr;
    int r = message_box(o, wt, wc, type);
    if (wt) ifree(wt);
    if (wc) ifree(wc);
    return r;
}
static int WINAPI MessageBoxExW(HWND o, const WCHAR* t, const WCHAR* c, u32 type, u16 lang) { (void)lang; return MessageBoxW(o, t, c, type); }
static int WINAPI MessageBoxExA(HWND o, const char* t, const char* c, u32 type, u16 lang) { (void)lang; return MessageBoxA(o, t, c, type); }
static int WINAPI MessageBoxTimeoutW(HWND o, const WCHAR* t, const WCHAR* c, u32 type, u16 lang, u32 ms) { (void)lang; (void)ms; return MessageBoxW(o, t, c, type); }
struct MSGBOXPARAMSW { u32 cbSize; HWND hwndOwner; HINSTANCE hInstance; const WCHAR* lpszText; const WCHAR* lpszCaption; u32 dwStyle; };
static int WINAPI MessageBoxIndirectW(const MSGBOXPARAMSW* p) { return MessageBoxW(p->hwndOwner, p->lpszText, p->lpszCaption, p->dwStyle); }
static BOOL WINAPI MessageBeep(u32 t) { (void)t; return TRUE; }

// ---------------------------------------------------------------- the clipboard (silicon's, as text)
static bool clip_open = false;
static HANDLE clip_data[2];                                         // CF_TEXT, CF_UNICODETEXT that this program put there
static BOOL WINAPI OpenClipboard(HWND h) { (void)h; if (clip_open) { set_error(ERROR_ACCESS_DENIED); return FALSE; } clip_open = true; return TRUE; }
static BOOL WINAPI CloseClipboard() { clip_open = false; return TRUE; }
static BOOL WINAPI EmptyClipboard() { clip_data[0] = clip_data[1] = nullptr; si::clip_set(""); return TRUE; }
static HANDLE WINAPI SetClipboardData(u32 fmt, HANDLE h) {
    if (!h) return nullptr;
    if (fmt == 13) { char* u = utf8_dup((const WCHAR*)h); si::clip_set(u); ifree(u); clip_data[1] = h; }
    else if (fmt == 1 || fmt == 7) { si::clip_set((const char*)h); clip_data[0] = h; }
    return h;
}
static HANDLE WINAPI GetClipboardData(u32 fmt) {
    char* buf = (char*)ialloc(65536);
    int n = si::clip_get(buf, 65536);
    if (n < 0) n = 0;
    buf[n < 65535 ? n : 65535] = 0;
    HANDLE r = nullptr;
    if (fmt == 13) {
        int k = wide_from_utf8(buf, -1, nullptr, 0);
        WCHAR* w = (WCHAR*)h_alloc((usize)k * 2);
        wide_from_utf8(buf, -1, w, k);
        r = w;
    } else if (fmt == 1 || fmt == 7) {
        usize k = slen(buf) + 1;
        char* a = (char*)h_alloc(k);
        memcpy(a, buf, k);
        r = a;
    }
    ifree(buf);
    return r;
}
static BOOL WINAPI IsClipboardFormatAvailable(u32 fmt) { char b[2]; return (fmt == 1 || fmt == 13 || fmt == 7) && si::clip_get(b, 2) > 0; }
static int WINAPI CountClipboardFormats() { return IsClipboardFormatAvailable(13) ? 2 : 0; }
static u32 WINAPI EnumClipboardFormats(u32 f) { if (!IsClipboardFormatAvailable(13)) return 0; return f == 0 ? 13 : f == 13 ? 1 : 0; }
static u32 WINAPI RegisterClipboardFormatW(const WCHAR* n) { (void)n; static u32 next = 0xC000; return next++; }
static u32 WINAPI RegisterClipboardFormatA(const char* n) { (void)n; static u32 next = 0xC400; return next++; }
static HWND WINAPI GetClipboardOwner() { return nullptr; }
static u32 WINAPI GetClipboardSequenceNumber() { return 1; }
static BOOL WINAPI AddClipboardFormatListener(HWND h) { (void)h; return TRUE; }
static BOOL WINAPI RemoveClipboardFormatListener(HWND h) { (void)h; return TRUE; }
static HWND WINAPI SetClipboardViewer(HWND h) { (void)h; return nullptr; }
static BOOL WINAPI ChangeClipboardChain(HWND a, HWND b) { (void)a; (void)b; return TRUE; }

// ---------------------------------------------------------------- menus, accelerators, hooks, dialogs: accepted, not shown
static u64 next_menu = 0x30000;
static HANDLE WINAPI CreateMenu() { return (HANDLE)(next_menu += 4); }
static HANDLE WINAPI CreatePopupMenu() { return (HANDLE)(next_menu += 4); }
static BOOL WINAPI DestroyMenu(HANDLE m) { (void)m; return TRUE; }
static BOOL WINAPI AppendMenuW(HANDLE m, u32 f, u64 id, const WCHAR* s) { (void)m; (void)f; (void)id; (void)s; return TRUE; }
static BOOL WINAPI AppendMenuA(HANDLE m, u32 f, u64 id, const char* s) { (void)m; (void)f; (void)id; (void)s; return TRUE; }
static BOOL WINAPI InsertMenuW(HANDLE m, u32 p, u32 f, u64 id, const WCHAR* s) { (void)m; (void)p; (void)f; (void)id; (void)s; return TRUE; }
static BOOL WINAPI InsertMenuItemW(HANDLE m, u32 i, BOOL b, const void* mi) { (void)m; (void)i; (void)b; (void)mi; return TRUE; }
static BOOL WINAPI InsertMenuItemA(HANDLE m, u32 i, BOOL b, const void* mi) { (void)m; (void)i; (void)b; (void)mi; return TRUE; }
static BOOL WINAPI ModifyMenuW(HANDLE m, u32 p, u32 f, u64 id, const WCHAR* s) { (void)m; (void)p; (void)f; (void)id; (void)s; return TRUE; }
static BOOL WINAPI DeleteMenu(HANDLE m, u32 p, u32 f) { (void)m; (void)p; (void)f; return TRUE; }
static BOOL WINAPI RemoveMenu(HANDLE m, u32 p, u32 f) { (void)m; (void)p; (void)f; return TRUE; }
static BOOL WINAPI SetMenu(HWND h, HANDLE m) { Wnd* w = wnd_get(h); if (!w) return FALSE; w->menu = m; return TRUE; }
static HANDLE WINAPI GetMenu(HWND h) { Wnd* w = wnd_get(h); return w ? w->menu : nullptr; }
static BOOL WINAPI DrawMenuBar(HWND h) { (void)h; return TRUE; }
static HANDLE WINAPI GetSystemMenu(HWND h, BOOL r) { (void)h; (void)r; return (HANDLE)(u64)0x3FFFC; }
static HANDLE WINAPI GetSubMenu(HANDLE m, int p) { (void)m; (void)p; return (HANDLE)(next_menu += 4); }
static int WINAPI GetMenuItemCount(HANDLE m) { (void)m; return 0; }
static u32 WINAPI GetMenuItemID(HANDLE m, int p) { (void)m; (void)p; return 0xFFFFFFFF; }
static u32 WINAPI GetMenuState(HANDLE m, u32 id, u32 f) { (void)m; (void)id; (void)f; return 0; }
static BOOL WINAPI GetMenuItemInfoW(HANDLE m, u32 i, BOOL b, void* mi) { (void)m; (void)i; (void)b; (void)mi; return FALSE; }
static BOOL WINAPI SetMenuItemInfoW(HANDLE m, u32 i, BOOL b, const void* mi) { (void)m; (void)i; (void)b; (void)mi; return TRUE; }
static u32 WINAPI CheckMenuItem(HANDLE m, u32 id, u32 f) { (void)m; (void)id; (void)f; return 0; }
static BOOL WINAPI CheckMenuRadioItem(HANDLE m, u32 a, u32 b, u32 c, u32 f) { (void)m; (void)a; (void)b; (void)c; (void)f; return TRUE; }
static BOOL WINAPI EnableMenuItem(HANDLE m, u32 id, u32 f) { (void)m; (void)id; (void)f; return 0; }
static BOOL WINAPI SetMenuDefaultItem(HANDLE m, u32 i, u32 b) { (void)m; (void)i; (void)b; return TRUE; }
static BOOL WINAPI TrackPopupMenu(HANDLE m, u32 f, int x, int y, int r, HWND h, const RECT* rc) { (void)m; (void)f; (void)x; (void)y; (void)r; (void)h; (void)rc; return 0; }
static BOOL WINAPI TrackPopupMenuEx(HANDLE m, u32 f, int x, int y, HWND h, void* p) { (void)m; (void)f; (void)x; (void)y; (void)h; (void)p; return 0; }
static HANDLE WINAPI LoadMenuW(HINSTANCE i, const WCHAR* n) { (void)i; (void)n; return CreateMenu(); }
static HANDLE WINAPI LoadMenuA(HINSTANCE i, const char* n) { (void)i; (void)n; return CreateMenu(); }
static BOOL WINAPI IsMenu(HANDLE m) { return (u64)m >= 0x30000 && (u64)m < 0x40000; }
static BOOL WINAPI SetMenuInfo(HANDLE m, const void* mi) { (void)m; (void)mi; return TRUE; }
static BOOL WINAPI EndMenu() { return TRUE; }
static HANDLE WINAPI LoadAcceleratorsW(HINSTANCE i, const WCHAR* n) { (void)i; (void)n; return (HANDLE)(u64)0x40004; }
static HANDLE WINAPI LoadAcceleratorsA(HINSTANCE i, const char* n) { (void)i; (void)n; return (HANDLE)(u64)0x40004; }
static HANDLE WINAPI CreateAcceleratorTableW(void* a, int n) { (void)a; (void)n; return (HANDLE)(u64)0x40008; }
static BOOL WINAPI DestroyAcceleratorTable(HANDLE h) { (void)h; return TRUE; }
static int WINAPI TranslateAcceleratorW(HWND h, HANDLE a, MSG* m) { (void)h; (void)a; (void)m; return 0; }
static int WINAPI TranslateAcceleratorA(HWND h, HANDLE a, MSG* m) { (void)h; (void)a; (void)m; return 0; }
static BOOL WINAPI IsDialogMessageW(HWND h, MSG* m) { (void)h; (void)m; return FALSE; }
static BOOL WINAPI IsDialogMessageA(HWND h, MSG* m) { (void)h; (void)m; return FALSE; }
static HANDLE WINAPI SetWindowsHookExW(int id, void* fn, HINSTANCE m, u32 tid) { (void)id; (void)fn; (void)m; (void)tid; return (HANDLE)(u64)0x50004; }
static HANDLE WINAPI SetWindowsHookExA(int id, void* fn, HINSTANCE m, u32 tid) { (void)id; (void)fn; (void)m; (void)tid; return (HANDLE)(u64)0x50004; }
static BOOL WINAPI UnhookWindowsHookEx(HANDLE h) { (void)h; return TRUE; }
static LRESULT WINAPI CallNextHookEx(HANDLE h, int c, WPARAM w, LPARAM l) { (void)h; (void)c; (void)w; (void)l; return 0; }
static HANDLE WINAPI SetWinEventHook(u32 a, u32 b, HINSTANCE m, void* fn, u32 p, u32 t, u32 f) { (void)a; (void)b; (void)m; (void)fn; (void)p; (void)t; (void)f; return (HANDLE)(u64)0x50008; }
static BOOL WINAPI UnhookWinEvent(HANDLE h) { (void)h; return TRUE; }
static BOOL WINAPI EndDialog(HWND h, i64 r) { (void)r; DestroyWindow_(h); return TRUE; }
static LRESULT WINAPI DefDlgProcW(HWND h, u32 m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
static LRESULT WINAPI DefDlgProcA(HWND h, u32 m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
static u32 WINAPI GetDialogBaseUnits() { return (16u << 16) | 8; }
static BOOL WINAPI MapDialogRect(HWND h, RECT* r) { (void)h; r->left = r->left * 6 / 4; r->right = r->right * 6 / 4; r->top = r->top * 13 / 8; r->bottom = r->bottom * 13 / 8; return TRUE; }
static BOOL WINAPI SetDlgItemTextW(HWND h, int id, const WCHAR* s) { return SetWindowTextW(GetDlgItem(h, id), s); }
static BOOL WINAPI SetDlgItemTextA(HWND h, int id, const char* s) { return SetWindowTextA(GetDlgItem(h, id), s); }
static u32 WINAPI GetDlgItemTextW(HWND h, int id, WCHAR* s, int n) { return (u32)GetWindowTextW(GetDlgItem(h, id), s, n); }
static u32 WINAPI GetDlgItemTextA(HWND h, int id, char* s, int n) { return (u32)GetWindowTextA(GetDlgItem(h, id), s, n); }
static BOOL WINAPI SetDlgItemInt(HWND h, int id, u32 v, BOOL sg) { char b[16]; snfmt(b, sizeof(b), sg ? "%d" : "%u", v); return SetDlgItemTextA(h, id, b); }
static u32 WINAPI GetDlgItemInt(HWND h, int id, BOOL* ok, BOOL sg) { (void)sg; char b[32]; GetDlgItemTextA(h, id, b, 32); if (ok) *ok = b[0] != 0; u32 v = 0; bool neg = b[0] == '-'; for (const char* p = b + neg; *p >= '0' && *p <= '9'; p++) v = v * 10 + (u32)(*p - '0'); return neg ? (u32)-(i32)v : v; }
static LRESULT WINAPI SendDlgItemMessageW(HWND h, int id, u32 m, WPARAM w, LPARAM l) { return SendMessageW(GetDlgItem(h, id), m, w, l); }
static LRESULT WINAPI SendDlgItemMessageA(HWND h, int id, u32 m, WPARAM w, LPARAM l) { return SendMessageA(GetDlgItem(h, id), m, w, l); }
static BOOL WINAPI CheckDlgButton(HWND h, int id, u32 c) { SendMessageW(GetDlgItem(h, id), 0xF1, c, 0); return TRUE; }      // BM_SETCHECK
static u32 WINAPI IsDlgButtonChecked(HWND h, int id) { return (u32)SendMessageW(GetDlgItem(h, id), 0xF0, 0, 0); }         // BM_GETCHECK
static HWND WINAPI GetNextDlgTabItem(HWND d, HWND c, BOOL prev) { (void)prev; Wnd* w = wnd_get(c); return w && w->next_sibling ? w->next_sibling->h : GetWindow(d, 5); }
static HWND WINAPI GetNextDlgGroupItem(HWND d, HWND c, BOOL prev) { return GetNextDlgTabItem(d, c, prev); }

// ---------------------------------------------------------------- scroll bars (values only; silicon draws none)
struct SCROLLINFO { u32 cbSize, fMask; i32 nMin, nMax; u32 nPage; i32 nPos, nTrackPos; };
static int WINAPI SetScrollInfo(HWND h, int bar, const SCROLLINFO* si_, BOOL redraw) {
    (void)redraw;
    Wnd* w = wnd_get(h);
    if (!w || bar < 0 || bar > 1) return 0;
    i32* s = w->scroll[bar];
    if (si_->fMask & 1) { s[0] = si_->nMin; s[1] = si_->nMax; }
    if (si_->fMask & 2) s[2] = (i32)si_->nPage;
    if (si_->fMask & 4) s[3] = si_->nPos;
    int maxpos = s[1] - (s[2] > 0 ? s[2] - 1 : 0);
    if (s[3] > maxpos) s[3] = maxpos;
    if (s[3] < s[0]) s[3] = s[0];
    return s[3];
}
static BOOL WINAPI GetScrollInfo(HWND h, int bar, SCROLLINFO* si_) {
    Wnd* w = wnd_get(h);
    if (!w || bar < 0 || bar > 1) return FALSE;
    i32* s = w->scroll[bar];
    if (si_->fMask & 1) { si_->nMin = s[0]; si_->nMax = s[1]; }
    if (si_->fMask & 2) si_->nPage = (u32)s[2];
    if (si_->fMask & 4) si_->nPos = s[3];
    if (si_->fMask & 16) si_->nTrackPos = s[3];
    return TRUE;
}
static int WINAPI SetScrollPos(HWND h, int bar, int pos, BOOL redraw) { Wnd* w = wnd_get(h); if (!w || bar < 0 || bar > 1) return 0; int o = w->scroll[bar][3]; SCROLLINFO s = {sizeof(s), 4, 0, 0, 0, pos, 0}; SetScrollInfo(h, bar, &s, redraw); return o; }
static int WINAPI GetScrollPos(HWND h, int bar) { Wnd* w = wnd_get(h); return w && bar >= 0 && bar <= 1 ? w->scroll[bar][3] : 0; }
static BOOL WINAPI SetScrollRange(HWND h, int bar, int a, int b, BOOL redraw) { SCROLLINFO s = {sizeof(s), 1, a, b, 0, 0, 0}; SetScrollInfo(h, bar, &s, redraw); return TRUE; }
static BOOL WINAPI GetScrollRange(HWND h, int bar, int* a, int* b) { Wnd* w = wnd_get(h); if (!w || bar < 0 || bar > 1) return FALSE; *a = w->scroll[bar][0]; *b = w->scroll[bar][1]; return TRUE; }
static BOOL WINAPI ShowScrollBar(HWND h, int bar, BOOL s) { (void)h; (void)bar; (void)s; return TRUE; }
static BOOL WINAPI EnableScrollBar(HWND h, u32 bar, u32 f) { (void)h; (void)bar; (void)f; return TRUE; }
static BOOL WINAPI GetScrollBarInfo(HWND h, i32 id, void* info) { (void)h; (void)id; (void)info; return FALSE; }

// ---------------------------------------------------------------- the screen, monitors and settings
struct MONITORINFOEXW { u32 cbSize; RECT rcMonitor, rcWork; u32 dwFlags; WCHAR szDevice[32]; };
static HANDLE WINAPI MonitorFromWindow(HWND h, u32 f) { (void)h; (void)f; return (HANDLE)(u64)0x12340; }
static HANDLE WINAPI MonitorFromPoint(POINT p, u32 f) { (void)p; (void)f; return (HANDLE)(u64)0x12340; }
static HANDLE WINAPI MonitorFromRect(const RECT* r, u32 f) { (void)r; (void)f; return (HANDLE)(u64)0x12340; }
static BOOL monitor_info(void* out, bool wide) {
    MONITORINFOEXW* mi = (MONITORINFOEXW*)out;
    mi->rcMonitor = {0, 0, screen_wh(false), screen_wh(true)};
    mi->rcWork = {0, 0, screen_wh(false), screen_wh(true)};
    mi->dwFlags = 1;                                                // MONITORINFOF_PRIMARY
    if (mi->cbSize >= sizeof(MONITORINFOEXW) - 32) {
        if (wide && mi->cbSize >= sizeof(MONITORINFOEXW)) wide_from_utf8("\\\\.\\DISPLAY1", -1, mi->szDevice, 32);
        else if (!wide && mi->cbSize >= 72) slcpy((char*)mi->szDevice, "\\\\.\\DISPLAY1", 32);
    }
    return TRUE;
}
static BOOL WINAPI GetMonitorInfoW(HANDLE m, void* mi) { (void)m; return monitor_info(mi, true); }
static BOOL WINAPI GetMonitorInfoA(HANDLE m, void* mi) { (void)m; return monitor_info(mi, false); }
typedef BOOL (WINAPI *MonEnumProc)(HANDLE, HDC, RECT*, LPARAM);
static BOOL WINAPI EnumDisplayMonitors(HDC h, const RECT* clip, MonEnumProc fn, LPARAM p) { (void)h; (void)clip; RECT r = {0, 0, screen_wh(false), screen_wh(true)}; fn((HANDLE)(u64)0x12340, nullptr, &r, p); return TRUE; }
static BOOL fill_devmode(u8* dm, bool wide) {
    // DEVMODE: the fields up to dmDisplayFrequency, at their offsets for the W and A layouts
    int base = wide ? 64 : 32;                                      // dmDeviceName is 32 characters
    *(u16*)(dm + base + 4) = (u16)(wide ? 220 : 156);              // dmSize
    *(u32*)(dm + base + 8) = 0x5C0000;                              // dmFields: bits, width, height, frequency
    int off = wide ? 168 : 104;                                     // dmBitsPerPel
    *(u32*)(dm + off) = 32;
    *(u32*)(dm + off + 4) = (u32)screen_wh(false);
    *(u32*)(dm + off + 8) = (u32)screen_wh(true);
    *(u32*)(dm + off + 16) = 60;                                    // dmDisplayFrequency
    return TRUE;
}
static BOOL WINAPI EnumDisplaySettingsW(const WCHAR* dev, u32 mode, void* dm) { (void)dev; if (mode != 0xFFFFFFFF && mode != 0xFFFFFFFE && mode > 0) return FALSE; return fill_devmode((u8*)dm, true); }
static BOOL WINAPI EnumDisplaySettingsA(const char* dev, u32 mode, void* dm) { (void)dev; if (mode != 0xFFFFFFFF && mode != 0xFFFFFFFE && mode > 0) return FALSE; return fill_devmode((u8*)dm, false); }
static BOOL WINAPI EnumDisplaySettingsExW(const WCHAR* dev, u32 mode, void* dm, u32 f) { (void)f; return EnumDisplaySettingsW(dev, mode, dm); }
static i32 WINAPI ChangeDisplaySettingsW(void* dm, u32 f) {
    (void)f;
    if (!dm) return 0;
    u32 w = *(u32*)((u8*)dm + 172), h = *(u32*)((u8*)dm + 176);
    return w == (u32)screen_wh(false) && h == (u32)screen_wh(true) ? 0 : -2;   // DISP_CHANGE_BADMODE: the program stays windowed
}
static i32 WINAPI ChangeDisplaySettingsA(void* dm, u32 f) {
    (void)f;
    if (!dm) return 0;
    u32 w = *(u32*)((u8*)dm + 108), h = *(u32*)((u8*)dm + 112);
    return w == (u32)screen_wh(false) && h == (u32)screen_wh(true) ? 0 : -2;
}
static i32 WINAPI ChangeDisplaySettingsExW(const WCHAR* n, void* dm, HWND h, u32 f, void* p) { (void)n; (void)h; (void)p; return ChangeDisplaySettingsW(dm, f); }
static BOOL WINAPI EnumDisplayDevicesW(const WCHAR* dev, u32 i, void* dd, u32 f) {
    (void)dev; (void)f;
    if (i > 0) return FALSE;
    u8* d = (u8*)dd;
    wide_from_utf8("\\\\.\\DISPLAY1", -1, (WCHAR*)(d + 4), 32);
    wide_from_utf8("silicon display", -1, (WCHAR*)(d + 68), 128);
    *(u32*)(d + 324) = 5;                                           // attached to the desktop, primary
    return TRUE;
}
static BOOL WINAPI EnumDisplayDevicesA(const char* dev, u32 i, void* dd, u32 f) {
    (void)dev; (void)f;
    if (i > 0) return FALSE;
    u8* d = (u8*)dd;
    slcpy((char*)(d + 4), "\\\\.\\DISPLAY1", 32);
    slcpy((char*)(d + 36), "silicon display", 128);
    *(u32*)(d + 164) = 5;
    return TRUE;
}
struct NONCLIENTMETRICSW { u32 cbSize; i32 iBorderWidth, iScrollWidth, iScrollHeight, iCaptionWidth, iCaptionHeight; LOGFONTW lfCaptionFont; i32 iSmCaptionWidth, iSmCaptionHeight; LOGFONTW lfSmCaptionFont; i32 iMenuWidth, iMenuHeight; LOGFONTW lfMenuFont, lfStatusFont, lfMessageFont; i32 iPaddedBorderWidth; };
static void ui_font(LOGFONTW* lf) { memset(lf, 0, sizeof(*lf)); lf->lfHeight = -12; lf->lfWeight = 400; wide_from_utf8("Segoe UI", -1, lf->lfFaceName, 32); }
static BOOL WINAPI SystemParametersInfoW(u32 a, u32 ui, void* pv, u32 f) {
    (void)ui; (void)f;
    switch (a) {
    case 0x30: if (pv) *(RECT*)pv = {0, 0, screen_wh(false), screen_wh(true)}; return TRUE;      // SPI_GETWORKAREA
    case 0x29: {                                                    // SPI_GETNONCLIENTMETRICS
        NONCLIENTMETRICSW* m = (NONCLIENTMETRICSW*)pv;
        u32 sz = m->cbSize;
        memset((u8*)m + 4, 0, sz - 4);
        m->iBorderWidth = 1; m->iScrollWidth = m->iScrollHeight = 17; m->iCaptionWidth = 36; m->iCaptionHeight = 22;
        m->iSmCaptionWidth = 22; m->iSmCaptionHeight = 22; m->iMenuWidth = 19; m->iMenuHeight = 19;
        ui_font(&m->lfCaptionFont); ui_font(&m->lfSmCaptionFont); ui_font(&m->lfMenuFont); ui_font(&m->lfStatusFont); ui_font(&m->lfMessageFont);
        if (sz >= sizeof(NONCLIENTMETRICSW)) m->iPaddedBorderWidth = 4;
        return TRUE;
    }
    case 0x1F: if (pv) ui_font((LOGFONTW*)pv); return TRUE;        // SPI_GETICONTITLELOGFONT
    case 0x68: if (pv) *(u32*)pv = 3; return TRUE;                 // SPI_GETWHEELSCROLLLINES
    case 0x6C: if (pv) *(u32*)pv = 3; return TRUE;                 // SPI_GETWHEELSCROLLCHARS
    case 0x4A: if (pv) *(BOOL*)pv = TRUE; return TRUE;              // SPI_GETFONTSMOOTHING
    case 0x200C: if (pv) *(u32*)pv = 2; return TRUE;               // SPI_GETFONTSMOOTHINGTYPE
    case 0x16: if (pv) *(u32*)pv = 1; return TRUE;                 // SPI_GETKEYBOARDDELAY
    case 0x0A: if (pv) *(u32*)pv = 31; return TRUE;                // SPI_GETKEYBOARDSPEED
    case 0x70: if (pv) *(u32*)pv = 10; return TRUE;                // SPI_GETMOUSESPEED
    case 0x2000: if (pv) *(u32*)pv = 530; return TRUE;             // SPI_GETCARETWIDTH... roughly
    case 0x42: if (pv) memset(pv, 0, 8); return TRUE;              // SPI_GETHIGHCONTRAST
    }
    if (pv && a < 0x100 && (a & 1) == 0) *(u32*)pv = 0;
    return TRUE;
}
static BOOL WINAPI SystemParametersInfoA(u32 a, u32 ui, void* pv, u32 f) {
    if (a == 0x29 && pv) {                                          // the A layout has narrow face names
        u8 w[sizeof(NONCLIENTMETRICSW)];
        ((NONCLIENTMETRICSW*)w)->cbSize = sizeof(NONCLIENTMETRICSW);
        SystemParametersInfoW(a, ui, w, f);
        u8* o = (u8*)pv;
        u32 sz = *(u32*)o;
        memset(o + 4, 0, sz - 4);
        memcpy(o + 4, w + 4, 20);
        return TRUE;
    }
    if (a == 0x1F && pv) { memset(pv, 0, 60); *(i32*)pv = -12; *(i32*)((u8*)pv + 16) = 400; slcpy((char*)pv + 28, "Segoe UI", 32); return TRUE; }
    return SystemParametersInfoW(a, ui, pv, f);
}
static BOOL WINAPI SystemParametersInfoForDpi(u32 a, u32 ui, void* pv, u32 f, u32 dpi) { (void)dpi; return SystemParametersInfoW(a, ui, pv, f); }
static BOOL WINAPI SetProcessDPIAware() { return TRUE; }
static BOOL WINAPI SetProcessDpiAwarenessContext(HANDLE c) { (void)c; return TRUE; }
static HANDLE WINAPI SetThreadDpiAwarenessContext(HANDLE c) { (void)c; return (HANDLE)(i64)-4; }
static HANDLE WINAPI GetThreadDpiAwarenessContext() { return (HANDLE)(i64)-4; }
static BOOL WINAPI IsProcessDPIAware() { return TRUE; }
static u32 WINAPI GetDpiForWindow(HWND h) { (void)h; return 96; }
static u32 WINAPI GetDpiForSystem() { return 96; }
static BOOL WINAPI AreDpiAwarenessContextsEqual(HANDLE a, HANDLE b) { return a == b; }
static BOOL WINAPI IsValidDpiAwarenessContext(HANDLE a) { (void)a; return TRUE; }
static BOOL WINAPI EnableNonClientDpiScaling(HWND h) { (void)h; return TRUE; }
static HRESULT WINAPI SetProcessDpiAwareness(int v) { (void)v; return 0; }
static HRESULT WINAPI GetDpiForMonitor(HANDLE m, int t, u32* x, u32* y) { (void)m; (void)t; *x = *y = 96; return 0; }
static HRESULT WINAPI GetProcessDpiAwareness(HANDLE p, int* v) { (void)p; *v = 2; return 0; }
static BOOL WINAPI SetWindowDisplayAffinity(HWND h, u32 a) { (void)h; (void)a; return TRUE; }
static int WINAPI SetWindowRgn(HWND h, HANDLE r, BOOL redraw) { (void)h; (void)r; (void)redraw; return 1; }
static int WINAPI GetWindowRgn(HWND h, HANDLE r) { (void)h; (void)r; return 0; }
static BOOL WINAPI AnimateWindow(HWND h, u32 t, u32 f) { (void)t; return ShowWindow_(h, (f & 0x10000) ? 0 : 5) || TRUE; }
static BOOL WINAPI ShowOwnedPopups(HWND h, BOOL s) { (void)h; (void)s; return TRUE; }
static BOOL WINAPI SetLayeredWindowAttributes(HWND h, COLORREF k, u8 a, u32 f) { (void)h; (void)k; (void)a; (void)f; return TRUE; }
static BOOL WINAPI UpdateLayeredWindow(HWND h, HDC d, POINT* p, SIZE* s, HDC sd, POINT* sp, COLORREF k, void* b, u32 f) { (void)h; (void)d; (void)p; (void)s; (void)sd; (void)sp; (void)k; (void)b; (void)f; return TRUE; }
static BOOL WINAPI GetTitleBarInfo(HWND h, void* t) { (void)h; (void)t; return FALSE; }
static BOOL WINAPI GetWindowInfo(HWND h, void* info) {
    Wnd* w = wnd_get(h);
    if (!w) return FALSE;
    u8* p = (u8*)info;
    GetWindowRect(h, (RECT*)(p + 4));
    RECT c;
    int x, y;
    screen_origin(w, &x, &y);
    c = {x, y, x + w->cw, y + w->ch};
    *(RECT*)(p + 20) = c;
    *(u32*)(p + 36) = w->style;
    *(u32*)(p + 40) = w->exstyle;
    return TRUE;
}
static BOOL WINAPI GetGUIThreadInfo(u32 tid, void* info) { (void)tid; u8* p = (u8*)info; MsgQ* q = my_queue(); *(HWND*)(p + 8) = q->active; *(HWND*)(p + 16) = q->focus; *(HWND*)(p + 24) = q->capture; return TRUE; }
static BOOL WINAPI CreateCaret(HWND h, HANDLE b, int w, int hh) { (void)h; (void)b; (void)w; (void)hh; return TRUE; }
static BOOL WINAPI DestroyCaret() { return TRUE; }
static BOOL WINAPI ShowCaret(HWND h) { (void)h; return TRUE; }
static BOOL WINAPI HideCaret(HWND h) { (void)h; return TRUE; }
static BOOL WINAPI SetCaretPos(int x, int y) { (void)x; (void)y; return TRUE; }
static BOOL WINAPI GetCaretPos(POINT* p) { *p = {0, 0}; return TRUE; }
static BOOL WINAPI SetCaretBlinkTime(u32 t) { (void)t; return TRUE; }
static u32 WINAPI GetCaretBlinkTime() { return 530; }
static BOOL WINAPI ExitWindowsEx(u32 f, u32 r) { (void)f; (void)r; set_error(ERROR_ACCESS_DENIED); return FALSE; }
static BOOL WINAPI LockWorkStation() { return FALSE; }
static HANDLE WINAPI GetProcessWindowStation() { return (HANDLE)(u64)0x60004; }
static HANDLE WINAPI GetThreadDesktop(u32 t) { (void)t; return (HANDLE)(u64)0x60008; }
static BOOL WINAPI GetUserObjectInformationW(HANDLE h, int i, void* p, u32 n, u32* need) {
    (void)h;
    if (i == 1 && p && n >= 12) { memset(p, 0, 12); *(u32*)((u8*)p + 8) = 1; if (need) *need = 12; return TRUE; }   // UOI_FLAGS: visible
    if (need) *need = 0;
    return FALSE;
}
static HANDLE WINAPI OpenInputDesktop(u32 f, BOOL i, u32 a) { (void)f; (void)i; (void)a; return (HANDLE)(u64)0x60008; }
static BOOL WINAPI CloseDesktop(HANDLE h) { (void)h; return TRUE; }
static BOOL WINAPI RegisterTouchWindow(HWND h, u32 f) { (void)h; (void)f; return TRUE; }
static BOOL WINAPI RegisterRawInputDevices(const void* d, u32 n, u32 s) { (void)d; (void)n; (void)s; return TRUE; }
static u32 WINAPI GetRawInputData(HANDLE r, u32 c, void* d, u32* s, u32 h) { (void)r; (void)c; (void)d; (void)h; if (s) *s = 0; return (u32)-1; }
static u32 WINAPI GetRawInputDeviceList(void* l, u32* n, u32 s) { (void)l; (void)s; *n = 0; return 0; }
static HANDLE WINAPI RegisterDeviceNotificationW(HANDLE r, void* f, u32 fl) { (void)r; (void)f; (void)fl; return (HANDLE)(u64)0x60010; }
static BOOL WINAPI UnregisterDeviceNotification(HANDLE h) { (void)h; return TRUE; }
static HANDLE WINAPI RegisterPowerSettingNotification(HANDLE r, const void* g, u32 f) { (void)r; (void)g; (void)f; return (HANDLE)(u64)0x60014; }
static BOOL WINAPI UnregisterPowerSettingNotification(HANDLE h) { (void)h; return TRUE; }
static BOOL WINAPI ChangeWindowMessageFilter(u32 m, u32 f) { (void)m; (void)f; return TRUE; }
static BOOL WINAPI ChangeWindowMessageFilterEx(HWND h, u32 m, u32 a, void* f) { (void)h; (void)m; (void)a; (void)f; return TRUE; }
static BOOL WINAPI GetLastInputInfo(void* p) { *(u32*)((u8*)p + 4) = (u32)now_ms(); return TRUE; }
static BOOL WINAPI WinHelpW(HWND h, const WCHAR* f, u32 c, u64 d) { (void)h; (void)f; (void)c; (void)d; return FALSE; }

// ---------------------------------------------------------------- images from resources and .bmp files
static HANDLE bitmap_from_dib(const u8* dib, u32 size) {
    if (size < 40) return nullptr;
    auto cds = (HANDLE (WINAPI*)(HDC, const void*, u32, void**, HANDLE, u32))gdi_fn("CreateDIBSection");
    const u32* h = (const u32*)dib;
    u32 hsz = h[0];
    u16 bpp = *(const u16*)(dib + 14);
    u32 comp = h[4];
    u32 used = h[8];
    u32 colors = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    u32 off = hsz + colors * 4 + (comp == 3 && hsz == 40 ? 12 : 0);
    void* bits = nullptr;
    HANDLE b = cds(nullptr, dib, 0, &bits, nullptr, 0);
    if (!b || !bits) return nullptr;
    i32 w = *(const i32*)(dib + 4), ht = *(const i32*)(dib + 8);
    u32 stride = (u32)(((w * bpp + 31) / 32) * 4);
    u32 n = stride * (u32)(ht < 0 ? -ht : ht);
    if (off + n > size) n = size > off ? size - off : 0;
    memcpy(bits, dib + off, n);
    return b;
}
static HANDLE load_image(HINSTANCE inst, const WCHAR* name, u32 type, u32 flags) {
    if (type != 0) {                                                // icons and cursors: a handle that draws nothing
        if (type == 2) return LoadCursorW(inst, (u64)name < 0x10000 ? name : (const WCHAR*)(u64)32512);
        return LoadIconW(inst, name);
    }
    if (flags & 0x10) {                                             // LR_LOADFROMFILE
        char path[MAX_PATH];
        if (!wpath_to_silicon(name, path, sizeof(path))) return nullptr;
        int n = si::file_size(path);
        if (n < 54) { set_error(ERROR_FILE_NOT_FOUND); return nullptr; }
        u8* data = (u8*)ialloc((usize)n);
        si::file_read(path, data, n);
        HANDLE r = nullptr;
        if (data[0] == 'B' && data[1] == 'M') {
            u32 off = *(u32*)(data + 10);
            r = bitmap_from_dib(data + 14, (u32)n - 14);
            // the bits start where the file header says, which bitmap_from_dib assumed was right after the colours
            if (r && off >= 14) {
                Bitmap* b = (Bitmap*)gdi_get(r, G_BITMAP);
                u8* top = b->s.stride < 0 ? b->s.bits + (i64)(b->s.h - 1) * b->s.stride : b->s.bits;
                usize stride = (usize)(b->s.stride < 0 ? -b->s.stride : b->s.stride);
                usize sz = stride * (usize)b->s.h;
                if (off + sz <= (usize)n) memcpy(top, data + off, sz);
            }
        }
        ifree(data);
        return r;
    }
    u32 size = 0;
    const u8* dib = (const u8*)res_lookup(inst, (const WCHAR*)2, name, &size);    // RT_BITMAP
    if (!dib) { set_error(ERROR_RESOURCE_NAME_NOT_FOUND); return nullptr; }
    return bitmap_from_dib(dib, size);
}
static HANDLE WINAPI LoadImageW(HINSTANCE i, const WCHAR* n, u32 t, int w, int h, u32 f) { (void)w; (void)h; return load_image(i, n, t, f); }
static HANDLE WINAPI LoadImageA(HINSTANCE i, const char* n, u32 t, int w, int h, u32 f) {
    (void)w; (void)h;
    if ((u64)n < 0x10000) return load_image(i, (const WCHAR*)n, t, f);
    WCHAR* wn = wide_dup(n);
    HANDLE r = load_image(i, wn, t, f);
    ifree(wn);
    return r;
}
static HANDLE WINAPI LoadBitmapW(HINSTANCE i, const WCHAR* n) { if (!i) return nullptr; return load_image(i, n, 0, 0); }
static HANDLE WINAPI LoadBitmapA(HINSTANCE i, const char* n) { return LoadImageA(i, n, 0, 0, 0, 0); }
static HANDLE WINAPI CopyImage(HANDLE h, u32 t, int w, int hh, u32 f) { (void)t; (void)w; (void)hh; (void)f; return h; }

// ---------------------------------------------------------------- standard controls: BUTTON, STATIC, EDIT (simple)
static LRESULT WINAPI button_proc(HWND h, u32 m, WPARAM w, LPARAM l) {
    Wnd* wnd = wnd_get(h);
    if (!wnd) return 0;
    u32 kind = wnd->style & 15;
    i64& state = wnd->userdata;                                     // bit 0 checked, bit 1 pushed, bit 2 hot
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r = {0, 0, wnd->cw, wnd->ch};
        auto sel = (HGDIOBJ (WINAPI*)(HDC, HGDIOBJ))gdi_fn("SelectObject");
        auto bkm = (int (WINAPI*)(HDC, int))gdi_fn("SetBkMode");
        auto stc = (COLORREF (WINAPI*)(HDC, COLORREF))gdi_fn("SetTextColor");
        sel(dc, stock_object(17));
        bkm(dc, 1);
        stc(dc, wnd->enabled ? 0 : 0x6D6D6D);
        char text[256] = "";
        if (wnd->text) utf8_from_wide(wnd->text, -1, text, sizeof(text));
        if (kind == 7) {                                            // BS_GROUPBOX
            RECT f = {0, 7, wnd->cw, wnd->ch};
            FrameRect(dc, &f, GetSysColorBrush(16));
            RECT t = {8, 0, wnd->cw - 8, 16};
            DrawTextA(dc, text, -1, &t, 0x20);
        } else if (kind == 2 || kind == 3 || kind == 4 || kind == 9 || kind == 5 || kind == 6) {   // check boxes and radio buttons
            FillRect(dc, &r, GetSysColorBrush(15));
            RECT box = {0, (wnd->ch - 13) / 2, 13, (wnd->ch - 13) / 2 + 13};
            FillRect(dc, &box, GetSysColorBrush(5));
            FrameRect(dc, &box, GetSysColorBrush(16));
            if (state & 1) { RECT in = {box.left + 3, box.top + 3, box.right - 3, box.bottom - 3}; FillRect(dc, &in, GetSysColorBrush(8)); }
            RECT t = {18, 0, wnd->cw, wnd->ch};
            DrawTextA(dc, text, -1, &t, 0x24);
        } else {                                                    // push buttons
            FillRect(dc, &r, GetSysColorBrush((state & 2) ? 16 : (state & 4) ? 22 : 15));
            HANDLE frame = solid_brush_stock((wnd->style & 1) ? 0xD77800 : 0xADADAD);    // BS_DEFPUSHBUTTON
            FrameRect(dc, &r, frame);
            DrawTextA(dc, text, -1, &r, 0x25);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: SetCapture(h); state |= 2; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        bool was = state & 2;
        state &= ~2;
        InvalidateRect(h, nullptr, FALSE);
        POINT p = {(i16)(l & 0xFFFF), (i16)((l >> 16) & 0xFFFF)};
        if (was && p.x >= 0 && p.y >= 0 && p.x < wnd->cw && p.y < wnd->ch) {
            if (kind == 3 || kind == 2) state ^= 1;                // checkbox
            if (kind == 9 || kind == 4) state |= 1;                // radio
            if (wnd->parent) send(wnd->parent, WM_COMMAND, MAKELP((int)wnd->id, 0), (LPARAM)h);    // BN_CLICKED
        }
        return 0;
    }
    case WM_MOUSEMOVE: if (!(state & 4)) { state |= 4; InvalidateRect(h, nullptr, FALSE); TRACKMOUSEEVENT t = {sizeof(t), 2, h, 0}; TrackMouseEvent(&t); } return 0;
    case WM_MOUSELEAVE: state &= ~4; InvalidateRect(h, nullptr, FALSE); return 0;
    case 0xF0: return state & 1;                                    // BM_GETCHECK
    case 0xF1: state = (state & ~1) | (w & 1); InvalidateRect(h, nullptr, FALSE); return 0;  // BM_SETCHECK
    case 0xF2: return state & 3;                                    // BM_GETSTATE
    case 0xF5: if (wnd->parent) send(wnd->parent, WM_COMMAND, MAKELP((int)wnd->id, 0), (LPARAM)h); return 0;   // BM_CLICK
    case WM_SETTEXT: { LRESULT r = DefWindowProcW(h, m, w, l); InvalidateRect(h, nullptr, TRUE); return r; }
    case WM_ENABLE: InvalidateRect(h, nullptr, TRUE); return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h, m, w, l);
}
static LRESULT WINAPI static_proc(HWND h, u32 m, WPARAM w, LPARAM l) {
    Wnd* wnd = wnd_get(h);
    if (!wnd) return 0;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r = {0, 0, wnd->cw, wnd->ch};
        auto sel = (HGDIOBJ (WINAPI*)(HDC, HGDIOBJ))gdi_fn("SelectObject");
        auto bkm = (int (WINAPI*)(HDC, int))gdi_fn("SetBkMode");
        HANDLE br = wnd->parent ? (HANDLE)send(wnd->parent, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)h) : nullptr;
        if (!br) br = GetSysColorBrush(15);
        FillRect(dc, &r, br);
        sel(dc, wnd->userdata ? (HGDIOBJ)wnd->userdata : stock_object(17));
        bkm(dc, 1);
        u32 ss = wnd->style & 0x1F;
        if (ss == 7 || ss == 8 || ss == 9) FrameRect(dc, &r, GetSysColorBrush(ss == 7 ? 21 : 16));
        else if (wnd->text) DrawTextW(dc, wnd->text, -1, &r, (ss == 1 ? 1 : ss == 2 ? 2 : 0) | ((wnd->style & 0x200) ? 0x24 : 0x10));
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_SETTEXT) { LRESULT r = DefWindowProcW(h, m, w, l); InvalidateRect(h, nullptr, TRUE); return r; }
    if (m == WM_SETFONT) { wnd->userdata = (i64)w; if (l) InvalidateRect(h, nullptr, TRUE); return 0; }
    if (m == WM_GETFONT) return (LRESULT)wnd->userdata;
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_NCHITTEST) return (wnd->style & 0x100) ? 1 : -1;  // SS_NOTIFY
    return DefWindowProcW(h, m, w, l);
}
static LRESULT WINAPI edit_proc(HWND h, u32 m, WPARAM w, LPARAM l) {
    Wnd* wnd = wnd_get(h);
    if (!wnd) return 0;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r = {0, 0, wnd->cw, wnd->ch};
        auto sel = (HGDIOBJ (WINAPI*)(HDC, HGDIOBJ))gdi_fn("SelectObject");
        auto bkm = (int (WINAPI*)(HDC, int))gdi_fn("SetBkMode");
        FillRect(dc, &r, GetSysColorBrush(5));
        FrameRect(dc, &r, GetSysColorBrush(my_queue()->focus == h ? 13 : 16));
        sel(dc, wnd->userdata ? (HGDIOBJ)wnd->userdata : stock_object(17));
        bkm(dc, 1);
        RECT t = {4, 2, wnd->cw - 4, wnd->ch - 2};
        WCHAR* s = wnd->text ? wnd->text : (WCHAR*)L"";
        if (wnd->style & 0x20) {                                   // ES_PASSWORD
            usize n = wlen(s);
            WCHAR* stars = (WCHAR*)ialloc((n + 1) * 2);
            for (usize i = 0; i < n; i++) stars[i] = '*';
            stars[n] = 0;
            DrawTextW(dc, stars, -1, &t, (wnd->style & 4) ? 0x10 : 0x24);
            ifree(stars);
        } else DrawTextW(dc, s, -1, &t, (wnd->style & 4) ? 0x10 : 0x24);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: SetFocus(h); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_CHAR: {
        if (wnd->style & 0x800) return 0;                          // ES_READONLY
        usize n = wnd->text ? wlen(wnd->text) : 0;
        WCHAR* t = (WCHAR*)izalloc((n + 2) * 2);
        if (n) memcpy(t, wnd->text, n * 2);
        if (w == 8) { if (n) t[--n] = 0; }
        else if (w == 13) { if (wnd->style & 4) { t[n++] = '\r'; } else { ifree(t); return 0; } }
        else if (w >= 32 || w == 9) t[n++] = (WCHAR)w;
        else { ifree(t); return 0; }
        t[n] = 0;
        if (wnd->text) ifree(wnd->text);
        wnd->text = t;
        InvalidateRect(h, nullptr, FALSE);
        if (wnd->parent) send(wnd->parent, WM_COMMAND, MAKELP((int)wnd->id, 0x300), (LPARAM)h);    // EN_CHANGE
        return 0;
    }
    case WM_GETDLGCODE: return 0x81;                                // DLGC_WANTCHARS | DLGC_WANTARROWS
    case WM_SETTEXT: { LRESULT r = DefWindowProcW(h, m, w, l); InvalidateRect(h, nullptr, FALSE); return r; }
    case WM_SETFONT: wnd->userdata = (i64)w; return 0;
    case WM_GETFONT: return (LRESULT)wnd->userdata;
    case 0xC5: return 0;                                            // EM_LIMITTEXT
    case 0xB1: return 0;                                            // EM_SETSEL
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, (const WCHAR*)(u64)32513)); return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}
static struct BuiltinClasses {
    BuiltinClasses() {}
} builtin_classes;
static void register_builtins() {
    static bool done = false;
    if (done) return;
    done = true;
    struct { const char* name; WNDPROC proc; int extra; } list[] = {{"Button", button_proc, 0}, {"Static", static_proc, 0}, {"Edit", edit_proc, 0}};
    for (auto& c : list) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        WCHAR n[32];
        wide_from_utf8(c.name, -1, n, 32);
        wc.cbSize = sizeof(wc);
        wc.style = 0x8;                                             // CS_DBLCLKS
        wc.lpfnWndProc = c.proc;
        wc.lpszClassName = n;
        wc.hCursor = LoadCursorW(nullptr, (const WCHAR*)(u64)32512);
        register_class(&wc, true);
    }
}
static struct RegisterBuiltins { RegisterBuiltins() { register_builtins(); } } register_builtins_now;

static const Export user_exports[] = {
    E(RegisterClassExW), E(RegisterClassExA), E(RegisterClassW), E(RegisterClassA), E(UnregisterClassW), E(UnregisterClassA),
    E(GetClassInfoExW), E(GetClassInfoExA), E(GetClassInfoW), E(GetClassInfoA), E(CreateWindowExW), E(CreateWindowExA),
    E(DestroyWindow), E(ShowWindow), E(ShowWindowAsync), E(GetDC), E(GetDCEx), E(GetWindowDC), E(ReleaseDC), E(BeginPaint), E(EndPaint),
    E(InvalidateRect), E(InvalidateRgn), E(ValidateRect), E(ValidateRgn), E(GetUpdateRect), E(GetUpdateRgn), E(UpdateWindow), E(RedrawWindow),
    E(LockWindowUpdate), E(ScrollWindowEx), E(ScrollWindow), E(ScrollDC), E(GetSysColor), E(GetSysColorBrush), E(SetSysColors),
    E(GetSystemMetrics), E(GetSystemMetricsForDpi), E(AdjustWindowRectEx), E(AdjustWindowRect), E(AdjustWindowRectExForDpi),
    E(GetClientRect), E(GetWindowRect), E(ClientToScreen), E(ScreenToClient), E(MapWindowPoints), E(SetWindowPos), E(MoveWindow),
    E(BeginDeferWindowPos), E(DeferWindowPos), E(EndDeferWindowPos), E(GetWindowPlacement), E(SetWindowPlacement), E(IsWindow),
    E(IsWindowVisible), E(IsIconic), E(IsZoomed), E(IsWindowEnabled), E(EnableWindow), E(IsChild), E(GetParent), E(GetAncestor),
    E(SetParent), E(GetDesktopWindow), E(GetShellWindow), E(GetWindow), E(GetTopWindow), E(EnumWindows), E(EnumThreadWindows),
    E(EnumChildWindows), E(FindWindowW), E(FindWindowA), E(FindWindowExW), E(FindWindowExA), E(GetDlgItem), E(GetDlgCtrlID),
    E(WindowFromPoint), E(ChildWindowFromPoint), E(ChildWindowFromPointEx), E(RealChildWindowFromPoint), E(GetWindowThreadProcessId),
    E(IsGUIThread), E(IsWindowUnicode), E(SetWindowTextW), E(SetWindowTextA), E(GetWindowTextW), E(GetWindowTextA), E(GetWindowTextLengthW),
    E(GetWindowTextLengthA), E(InternalGetWindowText), E(GetClassNameW), E(GetClassNameA), E(RealGetWindowClassW), E(GetWindowLongPtrW),
    E(GetWindowLongPtrA), E(SetWindowLongPtrW), E(SetWindowLongPtrA), E(GetWindowLongW), E(GetWindowLongA), E(SetWindowLongW),
    E(SetWindowLongA), E(GetWindowWord), E(SetWindowWord), E(GetClassLongPtrW), E(GetClassLongPtrA), E(SetClassLongPtrW), E(SetClassLongPtrA),
    E(GetClassLongW), E(GetClassLongA), E(SetClassLongW), E(SetClassLongA), E(SetPropW), E(GetPropW), E(RemovePropW), E(SetPropA),
    E(GetPropA), E(RemovePropA), E(SetFocus), E(GetFocus), E(GetActiveWindow), E(SetActiveWindow), E(GetForegroundWindow),
    E(SetForegroundWindow), E(BringWindowToTop), E(AllowSetForegroundWindow), E(LockSetForegroundWindow), E(SwitchToThisWindow),
    E(SetCapture), E(ReleaseCapture), E(GetCapture), E(TrackMouseEvent), E(_TrackMouseEvent), E(GetLastActivePopup), E(FlashWindow),
    E(FlashWindowEx), E(CloseWindow), E(OpenIcon), E(GetKeyState), E(GetAsyncKeyState), E(GetKeyboardState), E(SetKeyboardState),
    E(MapVirtualKeyW), E(MapVirtualKeyA), E(MapVirtualKeyExW), E(VkKeyScanW), E(VkKeyScanA), E(VkKeyScanExW), E(GetKeyboardLayout),
    E(GetKeyboardLayoutList), E(GetKeyboardLayoutNameW), E(ActivateKeyboardLayout), E(GetKeyboardType), E(GetKeyNameTextW), E(GetKeyNameTextA),
    E(ToUnicode), E(ToUnicodeEx), E(ToAscii), E(keybd_event), E(mouse_event), E(SendInput), E(RegisterHotKey), E(UnregisterHotKey),
    E(BlockInput), E(LoadCursorW), E(LoadCursorA), E(LoadCursorFromFileW), E(SetCursor), E(GetCursor), E(ShowCursor), E(GetCursorPos),
    E(GetPhysicalCursorPos), E(SetCursorPos), E(ClipCursor), E(GetClipCursor), E(GetCursorInfo), E(CreateCursor), E(DestroyCursor),
    E(CopyCursor), E(SetSystemCursor), E(GetDoubleClickTime), E(SetDoubleClickTime), E(SwapMouseButton), E(LoadIconW), E(LoadIconA),
    E(CreateIconIndirect), E(CreateIcon), E(CreateIconFromResourceEx), E(DestroyIcon), E(CopyIcon), E(DrawIcon), E(DrawIconEx), E(GetIconInfo),
    E(LookupIconIdFromDirectoryEx), E(SetTimer), E(KillTimer), E(SetCoalescableTimer), E(GetMessageW), E(GetMessageA), E(PeekMessageW),
    E(PeekMessageA), E(WaitMessage), E(MsgWaitForMultipleObjectsEx), E(MsgWaitForMultipleObjects), E(GetQueueStatus), E(GetInputState),
    E(GetMessageTime), E(GetMessagePos), E(GetMessageExtraInfo), E(TranslateMessage), E(DispatchMessageW), E(DispatchMessageA),
    E(PostQuitMessage), E(PostMessageW), E(PostMessageA), E(PostThreadMessageW), E(PostThreadMessageA), E(SendMessageW), E(SendMessageA),
    E(SendMessageTimeoutW), E(SendMessageTimeoutA), E(SendNotifyMessageW), E(SendNotifyMessageA), E(ReplyMessage), E(InSendMessage),
    E(CallWindowProcW), E(CallWindowProcA), E(RegisterWindowMessageW), E(RegisterWindowMessageA), E(DefWindowProcW), E(DefWindowProcA),
    E(SetRect), E(SetRectEmpty), E(CopyRect), E(InflateRect), E(OffsetRect), E(IsRectEmpty), E(IntersectRect), E(UnionRect), E(SubtractRect),
    E(PtInRect), E(EqualRect), E(FillRect), E(FrameRect), E(InvertRect), E(DrawFocusRect), E(DrawEdge), E(DrawFrameControl), E(DrawStateW),
    E(GrayStringW), E(DrawTextA), E(DrawTextW), E(DrawTextExW), E(DrawTextExA), E(TabbedTextOutW), E(MessageBoxW), E(MessageBoxA),
    E(MessageBoxExW), E(MessageBoxExA), E(MessageBoxTimeoutW), E(MessageBoxIndirectW), E(MessageBeep), E(OpenClipboard), E(CloseClipboard),
    E(EmptyClipboard), E(SetClipboardData), E(GetClipboardData), E(IsClipboardFormatAvailable), E(CountClipboardFormats),
    E(EnumClipboardFormats), E(RegisterClipboardFormatW), E(RegisterClipboardFormatA), E(GetClipboardOwner), E(GetClipboardSequenceNumber),
    E(AddClipboardFormatListener), E(RemoveClipboardFormatListener), E(SetClipboardViewer), E(ChangeClipboardChain), E(CreateMenu),
    E(CreatePopupMenu), E(DestroyMenu), E(AppendMenuW), E(AppendMenuA), E(InsertMenuW), E(InsertMenuItemW), E(InsertMenuItemA), E(ModifyMenuW),
    E(DeleteMenu), E(RemoveMenu), E(SetMenu), E(GetMenu), E(DrawMenuBar), E(GetSystemMenu), E(GetSubMenu), E(GetMenuItemCount), E(GetMenuItemID),
    E(GetMenuState), E(GetMenuItemInfoW), E(SetMenuItemInfoW), E(CheckMenuItem), E(CheckMenuRadioItem), E(EnableMenuItem), E(SetMenuDefaultItem),
    E(TrackPopupMenu), E(TrackPopupMenuEx), E(LoadMenuW), E(LoadMenuA), E(IsMenu), E(SetMenuInfo), E(EndMenu), E(LoadAcceleratorsW),
    E(LoadAcceleratorsA), E(CreateAcceleratorTableW), E(DestroyAcceleratorTable), E(TranslateAcceleratorW), E(TranslateAcceleratorA),
    E(IsDialogMessageW), E(IsDialogMessageA), E(SetWindowsHookExW), E(SetWindowsHookExA), E(UnhookWindowsHookEx), E(CallNextHookEx),
    E(SetWinEventHook), E(UnhookWinEvent), E(EndDialog), E(DefDlgProcW), E(DefDlgProcA), E(GetDialogBaseUnits), E(MapDialogRect),
    E(SetDlgItemTextW), E(SetDlgItemTextA), E(GetDlgItemTextW), E(GetDlgItemTextA), E(SetDlgItemInt), E(GetDlgItemInt), E(SendDlgItemMessageW),
    E(SendDlgItemMessageA), E(CheckDlgButton), E(IsDlgButtonChecked), E(GetNextDlgTabItem), E(GetNextDlgGroupItem), E(SetScrollInfo),
    E(GetScrollInfo), E(SetScrollPos), E(GetScrollPos), E(SetScrollRange), E(GetScrollRange), E(ShowScrollBar), E(EnableScrollBar),
    E(GetScrollBarInfo), E(MonitorFromWindow), E(MonitorFromPoint), E(MonitorFromRect), E(GetMonitorInfoW), E(GetMonitorInfoA),
    E(EnumDisplayMonitors), E(EnumDisplaySettingsW), E(EnumDisplaySettingsA), E(EnumDisplaySettingsExW), E(ChangeDisplaySettingsW),
    E(ChangeDisplaySettingsA), E(ChangeDisplaySettingsExW), E(EnumDisplayDevicesW), E(EnumDisplayDevicesA), E(SystemParametersInfoW),
    E(SystemParametersInfoA), E(SystemParametersInfoForDpi), E(SetProcessDPIAware), E(SetProcessDpiAwarenessContext),
    E(SetThreadDpiAwarenessContext), E(GetThreadDpiAwarenessContext), E(IsProcessDPIAware), E(GetDpiForWindow), E(GetDpiForSystem),
    E(AreDpiAwarenessContextsEqual), E(IsValidDpiAwarenessContext), E(EnableNonClientDpiScaling), E(SetWindowDisplayAffinity),
    E(SetWindowRgn), E(GetWindowRgn), E(AnimateWindow), E(ShowOwnedPopups), E(SetLayeredWindowAttributes), E(UpdateLayeredWindow),
    E(GetTitleBarInfo), E(GetWindowInfo), E(GetGUIThreadInfo), E(CreateCaret), E(DestroyCaret), E(ShowCaret), E(HideCaret), E(SetCaretPos),
    E(GetCaretPos), E(SetCaretBlinkTime), E(GetCaretBlinkTime), E(ExitWindowsEx), E(LockWorkStation), E(GetProcessWindowStation),
    E(GetThreadDesktop), E(GetUserObjectInformationW), E(OpenInputDesktop), E(CloseDesktop), E(RegisterTouchWindow),
    E(RegisterRawInputDevices), E(GetRawInputData), E(GetRawInputDeviceList), E(RegisterDeviceNotificationW), E(UnregisterDeviceNotification),
    E(RegisterPowerSettingNotification), E(UnregisterPowerSettingNotification), E(ChangeWindowMessageFilter), E(ChangeWindowMessageFilterEx),
    E(GetLastInputInfo), E(WinHelpW), E(LoadImageW), E(LoadImageA), E(LoadBitmapW), E(LoadBitmapA), E(CopyImage),
    EA("GdiFlushUser", GdiFlushUser),
};
EXPORT_TABLE("user32.dll", user_exports);
static const Export shcore_exports[] = { E(SetProcessDpiAwareness), E(GetDpiForMonitor), E(GetProcessDpiAwareness) };
EXPORT_TABLE("shcore.dll", shcore_exports);
static const Export shell_icon[] = { E(ExtractIconW) };
EXPORT_TABLE("shell32.dll", shell_icon);
static const Export comctl_exports[] = { E(_TrackMouseEvent) };
EXPORT_TABLE("comctl32.dll", comctl_exports);

}  // namespace rt
