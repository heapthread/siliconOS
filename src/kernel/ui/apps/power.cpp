// silicon - Power: restart or shut down
#include "apps.h"

struct PowerApp : App {
    int sel;
    int state = 0;          // 0 choosing, 1 restarting, 2 shutting down
    u64 at = 0;
    PowerApp(const char* arg) { sel = arg && !strcmp(arg, "restart") ? 0 : 1; }
    const char* app_id() override { return "power"; }
    const char* title() override { return "Power"; }
    const char* icon_name() override { return "shutdown"; }
    int pref_w() override { return 380; }
    int pref_h() override { return 210; }
    bool resizable() override { return false; }

    Rect opt(int i) { return {24 + i * 172, 40, 160, 110}; }

    void draw(Canvas& c, int w, int h) override {
        if (state) {
            c.fill(0, 0, w, h, 0xFF1A1428);
            c.text_centered(0, h / 2 - 20, w, state == 1 ? "restarting" G_ELLIP : "powering off" G_ELLIP, th::white, 2, true);
            c.text_centered(0, h / 2 + 10, w, "your files are saved", 0xFF9C92B8);
            return;
        }
        c.text_centered(0, 14, w, "what should silicon do?", th::text, 1, true);
        const char* names[2] = {"Restart", "Shut down"};
        const char* icons[2] = {"restart", "shutdown"};
        for (int i = 0; i < 2; i++) {
            Rect r = opt(i);
            bool on = hot(r.x, r.y, r.w, r.h) || sel == i;
            draw_bevel(c, r.x, r.y, r.w, r.h, on ? blend(th::panel, accent(), 70) : th::panel);
            c.blit(icon(icons[i])->big, r.x + (r.w - 64) / 2, r.y + 12);
            c.text_centered(r.x, r.y + 84, r.w, names[i], th::text, 1, true);
        }
        c.text_centered(0, h - 24, w, "enter confirms " G_BULLET " esc cancels", th::muted);
    }
    void go(int which) {
        state = which == 0 ? 1 : 2;
        at = ticks();
        fs_sync();
        ui_invalidate();
    }
    void on_mouse(const MouseEv& e) override {
        if (state) return;
        for (int i = 0; i < 2; i++)
            if (opt(i).contains(e.x, e.y)) {
                if (sel != i) { sel = i; ui_invalidate(); }
                if (e.kind == M_UP && e.button == 1) go(i);
            }
    }
    void on_key(const KeyEvent& e) override {
        if (state) return;
        if (e.key == K_LEFT || e.key == K_RIGHT || e.key == K_TAB) sel ^= 1;
        else if (e.key == K_ENTER) go(sel);
        else if (e.key == K_ESC) close();
        ui_invalidate();
    }
    void tick() override {
        if (state && ticks() - at > 700) {
            if (state == 1) power_reboot();
            power_off();
        }
    }
};
App* new_power(const char* arg) { return new PowerApp(arg); }
