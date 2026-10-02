// silicon - Reversi against a small positional AI
#include "apps.h"

struct ReversiApp : App {
    static const int CELL = 44, OFF = 12;
    int b[64];           // 0 empty, 1 player (dark), 2 silicon (light)
    int turn = 1;
    bool over = false;
    u64 ai_at = 0;
    int last_move = -1;
    char msg[64] = "your move";

    ReversiApp() { reset(); }
    const char* app_id() override { return "reversi"; }
    const char* title() override { return "Reversi"; }
    int pref_w() override { return OFF * 2 + CELL * 8 + 170; }
    int pref_h() override { return OFF * 2 + CELL * 8; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF2A2340; }

    void reset() {
        memset(b, 0, sizeof(b));
        b[27] = b[36] = 2;
        b[28] = b[35] = 1;
        turn = 1; over = false; last_move = -1;
        strlcpy(msg, "your move", sizeof(msg));
        ui_invalidate();
    }
    int flips(const int* bd, int p, int who, bool apply, int* out) {
        if (bd[p]) return 0;
        int x0 = p % 8, y0 = p / 8, total = 0;
        static const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
        for (int d = 0; d < 8; d++) {
            int x = x0 + dx[d], y = y0 + dy[d], n = 0;
            while (x >= 0 && y >= 0 && x < 8 && y < 8 && bd[y * 8 + x] == 3 - who) { x += dx[d]; y += dy[d]; n++; }
            if (n && x >= 0 && y >= 0 && x < 8 && y < 8 && bd[y * 8 + x] == who) {
                total += n;
                if (apply && out) {
                    int xx = x0 + dx[d], yy = y0 + dy[d];
                    for (int k = 0; k < n; k++) { out[yy * 8 + xx] = who; xx += dx[d]; yy += dy[d]; }
                }
            }
        }
        if (apply && out && total) out[p] = who;
        return total;
    }
    int moves(const int* bd, int who) { int n = 0; for (int i = 0; i < 64; i++) if (flips(bd, i, who, false, nullptr)) n++; return n; }
    int count(int who) { int n = 0; for (int i = 0; i < 64; i++) n += b[i] == who; return n; }

    int evaluate(const int* bd) {
        static const int wt[64] = {
            100, -20, 10, 5, 5, 10, -20, 100, -20, -50, -2, -2, -2, -2, -50, -20, 10, -2, 1, 1, 1, 1, -2, 10,
            5, -2, 1, 0, 0, 1, -2, 5, 5, -2, 1, 0, 0, 1, -2, 5, 10, -2, 1, 1, 1, 1, -2, 10,
            -20, -50, -2, -2, -2, -2, -50, -20, 100, -20, 10, 5, 5, 10, -20, 100};
        int s = 0;
        for (int i = 0; i < 64; i++) s += bd[i] == 2 ? wt[i] : bd[i] == 1 ? -wt[i] : 0;
        return s + 4 * (moves(bd, 2) - moves(bd, 1));
    }
    int ai_choose() {
        int best = -1, best_score = -1000000;
        for (int p = 0; p < 64; p++) {
            if (!flips(b, p, 2, false, nullptr)) continue;
            int nb[64];
            memcpy(nb, b, sizeof(nb));
            flips(b, p, 2, true, nb);
            // assume the player answers with their best reply
            int worst = 1000000;
            bool any = false;
            for (int q = 0; q < 64; q++) {
                if (!flips(nb, q, 1, false, nullptr)) continue;
                int nb2[64];
                memcpy(nb2, nb, sizeof(nb2));
                flips(nb, q, 1, true, nb2);
                worst = min(worst, evaluate(nb2));
                any = true;
            }
            int sc = any ? worst : evaluate(nb) + 30;
            sc += rand() % 3;
            if (sc > best_score) { best_score = sc; best = p; }
        }
        return best;
    }
    void play(int p, int who) {
        flips(b, p, who, true, b);
        last_move = p;
        ui_beep(0);
        int other = 3 - who;
        if (moves(b, other)) turn = other;
        else if (moves(b, who)) { turn = who; strlcpy(msg, who == 1 ? "silicon must pass" : "you must pass", sizeof(msg)); }
        else finish();
        if (turn == 2 && !over) ai_at = ticks() + 450;
        if (turn == 1 && !over && strcmp(msg, "silicon must pass")) strlcpy(msg, "your move", sizeof(msg));
    }
    void finish() {
        over = true;
        int a = count(1), c2 = count(2);
        snprintf(msg, sizeof(msg), a > c2 ? "you win %d-%d!" : a < c2 ? "silicon wins %d-%d" : "draw %d-%d", max(a, c2), min(a, c2));
        ui_notify("Reversi", msg, "reversi");
        ui_beep(a > c2 ? 2 : 1);
    }

    void draw(Canvas& c, int w, int h) override {
        int bs = CELL * 8;
        c.fill(OFF - 4, OFF - 4, bs + 8, bs + 8, 0xFF0F3B2E);
        c.rect(OFF - 4, OFF - 4, bs + 8, bs + 8, th::ink);
        for (int i = 0; i < 64; i++) {
            int x = OFF + (i % 8) * CELL, y = OFF + (i / 8) * CELL;
            c.fill(x, y, CELL, CELL, ((i % 8) + (i / 8)) % 2 ? 0xFF1E6F58 : 0xFF237A61);
            c.rect(x, y, CELL, CELL, 0xFF145040);
            if (b[i]) {
                bool dark = b[i] == 1;
                c.circle(x + 22, y + 24, 16, 0xFF0A2A20, true);
                c.circle(x + 22, y + 22, 16, dark ? 0xFF2A2433 : 0xFFF0ECFA, true);
                c.circle(x + 22, y + 22, 11, dark ? 0xFF3A3352 : 0xFFDCD3F0, true);
                if (!dark) { c.fill(x + 17, y + 17, 10, 10, accent()); c.fill(x + 19, y + 19, 6, 6, blend(accent(), th::white, 140)); }
                else c.fill(x + 19, y + 19, 6, 6, 0xFF6B6385);
                if (i == last_move) c.rect(x + 2, y + 2, CELL - 4, CELL - 4, th::gold);
            } else if (turn == 1 && !over && flips(b, i, 1, false, nullptr)) {
                bool hv = hot(x, y, CELL, CELL);
                c.circle(x + 22, y + 22, hv ? 14 : 3, hv ? 0x802A2433 : 0xFF5FB89A, !hv);
                if (hv) c.circle(x + 22, y + 22, 14, 0xFF2A2433, false);
            }
        }
        int px = OFF * 2 + bs + 8;
        c.text(px, 20, "REVERSI", th::white, 2, true);
        c.circle(px + 10, 72, 9, 0xFF2A2433, true);
        c.circle(px + 10, 72, 9, 0xFF6B6385, false);
        char s[32];
        snprintf(s, sizeof(s), "you      %2d", count(1));
        c.text(px + 26, 67, s, th::white);
        c.circle(px + 10, 98, 9, 0xFFF0ECFA, true);
        c.fill(px + 7, 95, 6, 6, accent());
        snprintf(s, sizeof(s), "silicon  %2d", count(2));
        c.text(px + 26, 93, s, th::white);
        c.fill(px, 128, 150, 2, 0xFF4A3F6A);
        c.text(px, 142, msg, over ? th::gold : 0xFFD8CCF4, 1, true);
        if (turn == 2 && !over) c.text(px, 162, "thinking" G_ELLIP, 0xFF9C92B8);
        draw_button(c, px, h - 46, 140, 26, "New game", hot(px, h - 46, 140, 26), true);
    }
    void on_mouse(const MouseEv& e) override {
        int px = OFF * 2 + CELL * 8 + 8;
        if (e.kind == M_UP && Rect{px, pref_h() - 46, 140, 26}.contains(e.x, e.y)) { reset(); return; }
        if (e.kind == M_UP && e.button == 1 && turn == 1 && !over) {
            int x = (e.x - OFF) / CELL, y = (e.y - OFF) / CELL;
            if (e.x >= OFF && e.y >= OFF && x < 8 && y < 8 && flips(b, y * 8 + x, 1, false, nullptr)) play(y * 8 + x, 1);
            else ui_beep(1);
        }
        ui_invalidate();
    }
    void tick() override {
        if (turn == 2 && !over && ticks() >= ai_at) {
            int p = ai_choose();
            if (p >= 0) play(p, 2);
            else { turn = 1; if (!moves(b, 1)) finish(); }
            ui_invalidate();
        }
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "Game", 24);
        m[0].add("New game", 1);
        return 1;
    }
    void on_command(int) override { reset(); }
};

App* new_reversi(const char*) { return new ReversiApp(); }
