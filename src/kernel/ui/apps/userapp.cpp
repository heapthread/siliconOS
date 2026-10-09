// silicon - the window of a user program (ring 3). The process draws into a pixel buffer it shares
// with the kernel; this app shows that buffer and turns mouse and keyboard input into events the
// process reads with win_event. If the process misbehaves it is killed, the desktop carries on.
#include "apps.h"
#include "../../proc/proc.h"

class UserApp : public App {
public:
    Proc* proc;
    char ttl[48];
    int  w, h;
    bool rs;
    bool closing = false;
    u64  close_t = 0;

    UserApp(Proc* p, int w_, int h_, const char* t, int flags) : proc(p), w(w_), h(h_), rs((flags & 1) != 0) {
        strlcpy(ttl, t, sizeof(ttl));
    }
    const char* app_id() override { return "module"; }
    const char* title() override { return ttl; }
    const char* icon_name() override { return "module"; }
    int  pref_w() override { return w; }
    int  pref_h() override { return h; }
    bool resizable() override { return rs; }
    bool wants_key_up() override { return true; }
    bool hover_effects() override { return true; }

    void draw(Canvas& c, int cw, int ch) override {
        if (!proc || !proc->surf || proc->dead) { c.fill(0, 0, cw, ch, th::paper); c.text(12, 12, proc && proc->dead ? proc->why : "starting...", th::muted); return; }
        int sw = proc->sw, sh = proc->sh;
        c.blit_raw(proc->surf, min(cw, sw), min(ch, sh), sw, 0, 0);
        if (cw > sw) c.fill(sw, 0, cw - sw, ch, th::paper);
        if (ch > sh) c.fill(0, sh, cw, ch - sh, th::paper);
    }
    void on_mouse(const MouseEv& e) override {
        if (!proc || proc->dead) return;
        UEvent ev = {EV_MOUSE, e.x, e.y, e.button, (i32)e.kind | (e.left ? 0x100 : 0)};
        if (e.kind == M_WHEEL) ev.b = e.y, ev.c = e.wheel;
        proc_push_event(proc, ev);
    }
    void on_key(const KeyEvent& e) override {
        if (!proc || proc->dead) return;
        UEvent ev = {EV_KEY, (i32)e.key, (i32)(e.cp ? e.cp : (u8)e.ch), e.down ? 1 : 0, (e.shift ? 1 : 0) | (e.ctrl ? 2 : 0) | (e.alt ? 4 : 0)};
        proc_push_event(proc, ev);
    }
    void on_resize(int nw, int nh) override {
        w = nw; h = nh;
        if (!proc || !proc->surf || proc->dead) return;
        u64 cap = (u64)proc->surf_pages * 4096 / 4;
        if ((u64)nw * nh > cap) return;
        proc->sw = nw; proc->sh = nh;
        UEvent ev = {EV_RESIZE, nw, nh, 0, 0};
        proc_push_event(proc, ev);
    }
    bool can_close() override {
        if (!proc || proc->dead) return true;
        if (closing) { proc_kill(proc, "closed"); return true; }     // second click: no more waiting
        closing = true;
        close_t = ticks();
        UEvent ev = {EV_CLOSE, 0, 0, 0, 0};
        proc_push_event(proc, ev);
        return false;
    }
    void tick() override {
        if (!proc) { force_close(); return; }
        if (proc->dead) { userapp_detach(this); force_close(); return; }
        if (closing && ticks() - close_t > 1500) { proc_kill(proc, "did not answer the close request"); userapp_detach(this); force_close(); return; }
        if (proc->dirty) { proc->dirty = false; ui_invalidate(); }
    }
    ~UserApp() { if (proc && proc->app == this) proc->app = nullptr; }
};

UserApp* userapp_create(Proc* p, int w, int h, const char* title, int flags) {
    UserApp* a = new UserApp(p, w, h, title, flags);
    Window* win = wm_open(a, w, h);
    win->resizable = a->resizable();
    return a;
}
void userapp_set_title(UserApp* a, const char* t) { strlcpy(a->ttl, t, sizeof(a->ttl)); a->set_title(t); }
void userapp_close(UserApp* a) { a->force_close(); }
void userapp_detach(UserApp* a) { if (a->proc) { if (a->proc->app == a) a->proc->app = nullptr; a->proc = nullptr; } }
int  userapp_screen_pixels() { extern int SW, SH; return SW * SH; }

// ---- opening a .uapp
App* new_uapp_note(const char* msg) { (void)msg; return nullptr; }

void uapp_run(const char* path) {
    char err[96];
    Proc* p = proc_spawn(path, err, sizeof(err));
    if (!p) ui_notify("Can't run program", err, "module");
}
