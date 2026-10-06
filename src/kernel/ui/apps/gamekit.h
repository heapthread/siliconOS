// silicon - shared bits for the arcade games: score/best handling, overlay, fixed timestep, sine table
#pragma once
#include "apps.h"

// angles are 0..255 for a full turn; results are scaled by 256
static inline int gsin(int a) {
    a &= 255;
    int neg = a >= 128;
    if (neg) a -= 128;
    int u = a * (128 - a);
    int v = 256 * 16 * u / (81920 - 4 * u);
    return neg ? -v : v;
}
static inline int gcos(int a) { return gsin(a + 64); }

struct GameApp : App {
    const char* gid = "";
    int score = 0, best = 0;
    bool over = false, paused = false, started = false;
    u64 last = 0;

    void load_best() { best = game_best(gid); }
    void end_game() {
        if (over) return;
        over = true;
        ui_beep(1);
        if (score > best) {
            best = score;
            game_save_best(gid, score);
            ui_notify(app_display_name(gid), "new high score!", gid);
        }
        ui_invalidate();
    }
    // number of 16 ms simulation steps due since the last call (0..4)
    int steps() {
        u64 now = ticks();
        if (!last || now - last > 500) last = now;
        int n = 0;
        while (now - last >= 16 && n < 4) { last += 16; n++; }
        return n;
    }
    void bar(Canvas& c, int w, u32 bg, u32 line, u32 fg, u32 dim, const char* extra = nullptr) {
        c.fill(0, 0, w, 34, bg);
        c.hline(0, 33, w, line);
        char b[64];
        snprintf(b, sizeof(b), "score %d", score);
        c.text(10, 9, b, fg, 2, true);
        snprintf(b, sizeof(b), "best %d", best);
        int bw = text_width(b, 1, true);
        c.text(w - bw - 10, 13, b, dim, 1, true);
        if (extra) c.text(w - bw - 20 - text_width(extra), 13, extra, dim);
    }
    void overlay(Canvas& c, int w, int h, const char* title, const char* hint, u32 hint_col = 0xFFD8D0F0) {
        c.fill_alpha(0, 34, w, h - 34, 0xFF000000, 130);
        c.text_centered(0, h / 2 - 30, w, title, th::white, 4, true);
        c.text_centered(0, h / 2 + 18, w, hint, hint_col);
    }
    void restart_keys(const KeyEvent& e) {
        if (e.key == ' ' || e.key == K_ENTER) {
            if (over) { reset_game(); started = true; }
            else if (!started) started = true;
            else paused = !paused;
            last = 0;
            ui_invalidate();
        }
        if (e.key == 'p' && started && !over) { paused = !paused; ui_invalidate(); }
    }
    virtual void reset_game() {}
    bool running() { return started && !over && !paused; }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "Game", 24);
        m[0].add("New game", 1).add("Pause", 2, "p");
        return 1;
    }
    void on_command(int cmd) override {
        if (cmd == 1) { reset_game(); started = true; last = 0; }
        else if (started && !over) paused = !paused;
        ui_invalidate();
    }
};
