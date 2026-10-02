// silicon - Snake (desktop edition)
#include "apps.h"

struct SnakeApp : App {
    static const int GW = 24, GH = 18, CELL = 20, TOP = 36;
    Vec<int> body;
    int dir = 1, next_dir = 1, food = 0, score = 0, best = 0;
    bool over = false, paused = false, started = false;
    u64 last = 0;

    SnakeApp() {
        FsNode* f = fs_lookup("/home/games/snake.best");
        if (f) best = atoi(fs_read_text(f));
        reset();
    }
    ~SnakeApp() { body.free(); }
    const char* app_id() override { return "snake"; }
    const char* title() override { return "Snake"; }
    int pref_w() override { return GW * CELL; }
    int pref_h() override { return TOP + GH * CELL; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF14201C; }

    void reset() {
        body.clear();
        for (int i = 0; i < 4; i++) body.push((GH / 2) * GW + 6 - i);
        dir = next_dir = 1;
        score = 0;
        over = paused = started = false;
        place_food();
        ui_invalidate();
    }
    void place_food() {
        for (;;) { food = rand() % (GW * GH); if (body.find(food) < 0) return; }
    }
    void draw(Canvas& c, int w, int h) override {
        c.fill(0, 0, w, TOP, 0xFF0E1714);
        c.hline(0, TOP - 1, w, 0xFF2A4A3E);
        char b[64];
        snprintf(b, sizeof(b), "score %d", score);
        c.text(10, 8, b, 0xFF7FE0A8, 2, true);
        snprintf(b, sizeof(b), "best %d", best);
        c.text(w - text_width(b, 1, true) - 10, 13, b, 0xFF5A9A80, 1, true);
        for (int y = 0; y < GH; y++)
            for (int x = 0; x < GW; x++)
                if ((x + y) % 2 == 0) c.fill(x * CELL + 9, TOP + y * CELL + 9, 2, 2, 0xFF1C2C26);
        // food: a glowing bit of silicon
        int fx = (food % GW) * CELL, fy = TOP + (food / GW) * CELL;
        int pulse = (int)(ticks() / 120 % 4);
        c.fill(fx + 3, fy + 3, 14, 14, 0xFF3A2A50);
        c.fill(fx + 5, fy + 5, 10, 10, 0xFF9B7BFF);
        c.fill(fx + 7, fy + 7, 6, 6, pulse < 2 ? 0xFFDCCFFF : 0xFFB8A4FF);
        for (int k = 0; k < 3; k++) { c.fill(fx + 5 + k * 4, fy + 1, 2, 2, th::gold); c.fill(fx + 5 + k * 4, fy + 17, 2, 2, th::gold); }
        for (int i = body.len - 1; i >= 0; i--) {
            int px = (body[i] % GW) * CELL, py = TOP + (body[i] / GW) * CELL;
            u32 col = i == 0 ? 0xFFA8F5C6 : (i % 2 ? 0xFF4FBF84 : 0xFF5FD094);
            c.fill(px + 1, py + 1, CELL - 2, CELL - 2, col);
            c.hline(px + 2, py + 1, CELL - 4, blend(col, th::white, 90));
            if (i == 0) {
                int ex = dir == 1 ? 12 : dir == 3 ? 4 : 5, ey = dir == 2 ? 12 : dir == 0 ? 4 : 5;
                int ex2 = dir == 0 || dir == 2 ? ex + 8 : ex, ey2 = dir == 1 || dir == 3 ? ey + 8 : ey;
                c.fill(px + ex, py + ey, 3, 3, th::ink);
                c.fill(px + ex2, py + ey2, 3, 3, th::ink);
            }
        }
        if (!started || over || paused) {
            c.fill_alpha(0, TOP, w, h - TOP, 0xFF000000, 120);
            const char* t = over ? "GAME OVER" : paused ? "PAUSED" : "SNAKE";
            c.text_centered(0, TOP + 120, w, t, th::white, 4, true);
            c.text_centered(0, TOP + 180, w, over ? "space to play again" : "arrows to move, space to start", 0xFFB8F0D0);
        }
    }
    void on_key(const KeyEvent& e) override {
        if (e.key == ' ' || e.key == K_ENTER) {
            if (over) reset();
            if (!started) started = true;
            else paused = !paused;
        }
        int nd = -1;
        if (e.key == K_UP || e.key == 'w') nd = 0;
        if (e.key == K_RIGHT || e.key == 'd') nd = 1;
        if (e.key == K_DOWN || e.key == 's') nd = 2;
        if (e.key == K_LEFT || e.key == 'a') nd = 3;
        if (nd >= 0) {
            if (!started && !over) started = true;
            if ((nd + 2) % 4 != dir) next_dir = nd;
        }
        ui_invalidate();
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_UP && (over || !started)) { if (over) reset(); started = true; ui_invalidate(); }
    }
    void tick() override {
        if (!started || over || paused) { if (ticks() - last > 120) { last = ticks(); ui_invalidate(); } return; }
        if (ticks() - last < (u64)max(60, 130 - score * 3)) return;
        last = ticks();
        dir = next_dir;
        int hx = body[0] % GW, hy = body[0] / GW;
        if (dir == 0) hy--; else if (dir == 1) hx++; else if (dir == 2) hy++; else hx--;
        int np = hy * GW + hx;
        if (hx < 0 || hy < 0 || hx >= GW || hy >= GH || body.find(np) >= 0) {
            over = true;
            ui_beep(1);
            if (score > best) {
                best = score;
                char b[16];
                snprintf(b, sizeof(b), "%d\n", best);
                fs_mkdirs("/home/games");
                fs_write_text("/home/games/snake.best", b);
                ui_notify("Snake", "new high score!", "snake");
            }
            ui_invalidate();
            return;
        }
        body.insert(0, np);
        if (np == food) { score++; place_food(); ui_beep(0); }
        else body.len--;
        ui_invalidate();
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "Game", 24);
        m[0].add("New game", 1).add("Pause", 2, "space");
        return 1;
    }
    void on_command(int cmd) override { if (cmd == 1) { reset(); started = true; } else paused = !paused; ui_invalidate(); }
};

App* new_snake(const char*) { return new SnakeApp(); }
