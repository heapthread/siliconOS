// silicon - the window of a user program (ring 3). The process draws into a pixel buffer it shares
// with the kernel; this app shows that buffer and turns mouse and keyboard input into events the
// process reads with win_event. If the process misbehaves it is killed, the desktop carries on.
#include "apps.h"
#include "../../proc/proc.h"

class UserApp : public App {
public:
    Proc* proc;
    int  wid;                          // which of the process's windows (proc->win[wid])
    char ttl[64];
    bool closing = false;
    u64  close_t = 0;
    bool was_focused = false;
    Rect last_client{0, 0, 0, 0};

    UserApp(Proc* p, int w) : proc(p), wid(w) { strlcpy(ttl, p->win[w].title, sizeof(ttl)); }
    UWin* uw() { return proc && !proc->dead && proc->win[wid].used ? &proc->win[wid] : nullptr; }
    void push(UEvent ev) { ev.win = wid; proc_push_event(proc, ev); }
    const char* app_id() override { return "module"; }
    const char* title() override { return ttl; }
    const char* icon_name() override { return "module"; }
    int  pref_w() override { return proc ? proc->win[wid].sw : 320; }
    int  pref_h() override { return proc ? proc->win[wid].sh : 240; }
    bool resizable() override { return proc && proc->win[wid].resizable; }
    bool wants_key_up() override { return true; }
    bool hover_effects() override { return true; }
    int  cursor_at(int x, int y) override { (void)x; (void)y; UWin* u = uw(); return u ? u->cursor : 0; }

    void draw(Canvas& c, int cw, int ch) override {
        UWin* u = uw();
        if (!u || !u->surf) { c.fill(0, 0, cw, ch, th::paper); c.text(12, 12, proc && proc->dead ? proc->why : "starting...", th::muted); return; }
        int sw = u->sw, sh = u->sh;
        // copy with the alpha forced on: Windows programs (and plenty of others) leave the top byte at 0,
        // and the compositor blends the rows that hold the window's rounded corners
        int w = min(cw, sw), h = min(ch, sh);
        for (int y = 0; y < h; y++) {
            int dy = y + c.oy;
            if (dy < c.clip.y || dy >= c.clip.y + c.clip.h) continue;
            const u32* src = u->surf + (u64)y * sw;
            u32* dst = c.px + (u64)dy * c.pitch + c.ox;
            for (int x = 0; x < w; x++) dst[x] = src[x] | 0xFF000000;
        }
        if (cw > sw) c.fill(sw, 0, cw - sw, ch, th::paper);
        if (ch > sh) c.fill(0, sh, cw, ch - sh, th::paper);
    }
    void on_mouse(const MouseEv& e) override {
        if (!uw()) return;
        UEvent ev = {EV_MOUSE, e.x, e.y, e.button, (i32)e.kind | (e.left ? 0x100 : 0), 0};
        if (e.kind == M_WHEEL) ev.b = e.y, ev.c = e.wheel;
        push(ev);
    }
    void on_key(const KeyEvent& e) override {
        if (!uw()) return;
        UEvent ev = {EV_KEY, (i32)e.key, (i32)(e.cp ? e.cp : (u8)e.ch), e.down ? 1 : 0, (e.shift ? 1 : 0) | (e.ctrl ? 2 : 0) | (e.alt ? 4 : 0), 0};
        push(ev);
    }
    void on_resize(int nw, int nh) override {
        UWin* u = uw();
        if (!u || !u->surf) return;
        if ((u64)nw * nh > (u64)u->pages * 1024) return;
        if (nw == u->sw && nh == u->sh) return;
        u->sw = nw; u->sh = nh;
        push({EV_RESIZE, nw, nh, 0, 0, 0});
    }
    bool can_close() override {
        if (!uw()) return true;
        if (closing) { proc_kill(proc, "closed"); return true; }     // second click: no more waiting
        closing = true;
        close_t = ticks();
        push({EV_CLOSE, 0, 0, 0, 0, 0});
        return false;
    }
    void tick() override {
        UWin* u = uw();
        if (!proc || (proc && proc->dead)) { userapp_detach(this); force_close(); return; }
        if (!u) { force_close(); return; }
        if (closing && ticks() - close_t > 3000) { proc_kill(proc, "did not answer the close request"); userapp_detach(this); force_close(); return; }
        if (u->dirty) { u->dirty = false; ui_invalidate(); }
        // tell the program when it gains or loses the keyboard, and when its window moves
        bool f = win && wm_focused() == win;
        if (f != was_focused) { was_focused = f; if (f) closing = false; push({EV_FOCUS, f ? 1 : 0, 0, 0, 0, 0}); }
        if (win) {
            Rect cl = win->client();
            if (cl.x != last_client.x || cl.y != last_client.y) { last_client = cl; push({EV_MOVE, cl.x, cl.y, 0, 0, 0}); }
        }
    }
    ~UserApp() { if (proc && proc->win[wid].app == this) proc->win[wid].app = nullptr; }
};

UserApp* userapp_create(Proc* p, int wid) {
    UserApp* a = new UserApp(p, wid);
    UWin& u = p->win[wid];
    // the position the program asked for is the client area's; the window frame goes around it
    int x = u.x >= 0 ? u.x - BORDER : -1, y = u.y >= 0 ? u.y - TITLE_H : -1;
    Window* win = wm_open(a, u.sw, u.sh, x, y);
    win->resizable = a->resizable();
    return a;
}
void userapp_set_title(UserApp* a, const char* t) { strlcpy(a->ttl, t, sizeof(a->ttl)); a->set_title(t); }
void userapp_close(UserApp* a) { a->force_close(); }
void userapp_detach(UserApp* a) { if (a->proc) { if (a->proc->win[a->wid].app == a) a->proc->win[a->wid].app = nullptr; a->proc = nullptr; } }
int  userapp_screen_pixels() { extern int SW, SH; return SW * SH; }
void userapp_geometry(UserApp* a, int* x, int* y, int* w, int* h) {
    if (!a->win) return;
    Rect c = a->win->client();
    *x = c.x; *y = c.y; *w = c.w; *h = c.h;
}
void userapp_set_geometry(UserApp* a, int x, int y, int w, int h) {
    if (!a->win) return;
    Rect r = a->win->r;
    if (x >= 0) { r.x = x - BORDER; r.y = y - TITLE_H; }
    if (w > 0) { r.w = w + 2 * BORDER; r.h = h + TITLE_H + BORDER; }
    wm_set_geometry(a->win, r);
}
void userapp_focus(UserApp* a) { if (a->win) wm_focus(a->win); }
bool userapp_focused(UserApp* a) { return a->win && wm_focused() == a->win; }
void userapp_minimize(UserApp* a, int how) { if (!a->win) return; if (how == 0) wm_minimize(a->win); else wm_toggle_max(a->win); }

// ---- opening a .uapp
App* new_uapp_note(const char* msg) { (void)msg; return nullptr; }

// a Windows program opened from the desktop: no console, what it prints goes to the serial log
void win32_run(const char* path) {
    char err[96], cmd[220], cwd[200];
    snprintf(cmd, sizeof(cmd), "\"%s\"", path);
    strlcpy(cwd, path, sizeof(cwd));
    char* sl = strrchr(cwd, '/');
    if (sl && sl != cwd) *sl = 0;
    Proc* p = win32_spawn(cmd, cwd, false, err, sizeof(err));
    if (!p) ui_notify("Can't run program", err, "module");
}

void uapp_run(const char* path) {
    char err[96];
    Proc* p = proc_spawn(path, err, sizeof(err));
    if (!p) ui_notify("Can't run program", err, "module");
}
