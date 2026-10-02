// silicon - Game library
#include "apps.h"

int game_best(const char* name) {
    char path[64];
    snprintf(path, sizeof(path), "/home/games/%s.best", name);
    FsNode* f = fs_lookup(path);
    return f ? atoi(fs_read_text(f)) : 0;
}

void game_save_best(const char* name, int score) {
    char path[64], b[16];
    snprintf(path, sizeof(path), "/home/games/%s.best", name);
    snprintf(b, sizeof(b), "%d\n", score);
    fs_mkdirs("/home/games");
    fs_write_text(path, b);
}

struct GamesApp : App {
    int sel = 0;
    const char* app_id() override { return "games"; }
    const char* title() override { return "Games"; }
    int pref_w() override { return 420; }
    int pref_h() override { return 44 + NG * 78 + 8 < 560 ? 44 + NG * 78 + 8 : 560; }
    bool resizable() override { return false; }

    struct G { const char* id; const char* name; const char* desc; };
    static const int NG = 7;
    G games[NG] = {
        {"stacker", "Stacker", "stack falling blocks, clear lines"},
        {"g2048", "2048", "slide matching chips together"},
        {"breakout", "Breakout", "knock every transistor off the die"},
        {"flappy", "Flappy Chip", "flap through the PCB traces"},
        {"minesweeper", "Minesweeper", "flag the mines, clear the board"},
        {"snake", "Snake", "eat the bits, avoid your tail"},
        {"reversi", "Reversi", "outflank the silicon AI"},
    };
    Smooth sc;
    ScrollBar sb;
    int ch = 0;
    int content_h() { return NG * 78 + 8; }
    Rect row(int i) { return {12, 44 + i * 78 - sc.pos(), 386, 72}; }
    Rect play(int i) { Rect r = row(i); return {r.x + r.w - 70, r.y + 24, 58, 22}; }

    void tick() override { if (sc.step()) ui_invalidate(); }
    void draw(Canvas& c, int w, int h) override {
        ch = h;
        c.set_clip({0, 35, w, h - 35});
        for (int i = 0; i < NG; i++) {
            Rect r = row(i);
            if (r.y + r.h < 35 || r.y > h) continue;
            bool on = hot(r.x, r.y, r.w, r.h);
            if (on || sel == i) draw_bevel(c, r.x, r.y, r.w, r.h, sel == i ? blend(th::paper, accent(), 60) : th::panel);
            c.blit(icon(games[i].id)->big, r.x + 6, r.y + 4);
            c.text(r.x + 82, r.y + 16, games[i].name, th::text, 2, true);
            c.text(r.x + 82, r.y + 46, games[i].desc, th::muted);
            Rect p = play(i);
            draw_button(c, p.x, p.y, p.w, p.h, "Play", hot(p.x, p.y, p.w, p.h), true);
            int b = game_best(games[i].id);
            if (b) { char s[24]; snprintf(s, sizeof(s), "best %d", b); c.text(p.x + p.w - text_width(s), r.y + 8, s, th::muted); }
        }
        c.set_clip({0, 0, w, h});
        sb.draw(c, w - 10, 35, h - 35, content_h(), h - 35, sc.pos());
        c.fill(0, 0, w, 34, th::panel);
        c.hline(0, 34, w, th::border);
        c.blit(icon("games")->small, 12, 9);
        c.text(36, 11, "game library", th::text, 1, true);
        c.text(w - 140, 11, "7 games installed", th::muted);
    }
    void on_mouse(const MouseEv& e) override {
        int off = sc.target;
        if (sb.handle(e, 420 - 10, 35, ch - 35, content_h(), ch - 35, off)) { sc.jump(off); ui_invalidate(); return; }
        if (e.kind == M_WHEEL) { sc.set(clamp(sc.target - e.wheel * 50, 0, max(0, content_h() - (ch - 35)))); ui_invalidate(); return; }
        for (int i = 0; i < NG; i++)
            if (e.y > 35 && row(i).contains(e.x, e.y)) {
                sel = i;
                if (e.kind == M_DBL || (e.kind == M_UP && play(i).contains(e.x, e.y))) launch(games[i].id);
            }
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        if (e.key == K_DOWN) sel = (sel + 1) % NG;
        if (e.key == K_UP) sel = (sel + NG - 1) % NG;
        if (e.key == K_ENTER) launch(games[sel].id);
        ui_invalidate();
    }
};
App* new_games(const char*) { return new GamesApp(); }
