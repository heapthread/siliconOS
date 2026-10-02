// silicon - Minesweeper
#include "apps.h"

struct MinesApp : App {
    static const int CELL = 20, TOP = 46, PAD = 10;
    int W = 9, H = 9, M = 10, level = 0;
    u8 mine[30 * 20], open[30 * 20], flag[30 * 20], num[30 * 20];
    bool started = false, over = false, won = false;
    int boom = -1;
    u64 t0 = 0, t_end = 0;
    int best[3] = {0, 0, 0};

    MinesApp() { load_best(); reset(); }
    const char* app_id() override { return "minesweeper"; }
    const char* title() override { return "Minesweeper"; }
    int pref_w() override { return W * CELL + 2 * PAD; }
    int pref_h() override { return TOP + H * CELL + PAD + 4; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFFC9C2DB; }

    void load_best() {
        FsNode* f = fs_lookup("/home/games/minesweeper.best");
        if (!f) return;
        const char* t = fs_read_text(f);
        for (int i = 0; i < 3 && *t; i++) { best[i] = atoi(t); while (*t && *t != ' ') t++; while (*t == ' ') t++; }
    }
    void save_best() {
        char b[48];
        snprintf(b, sizeof(b), "%d %d %d\n", best[0], best[1], best[2]);
        fs_mkdirs("/home/games");
        fs_write_text("/home/games/minesweeper.best", b);
    }

    void set_level(int l) {
        static const int dims[3][3] = {{9, 9, 10}, {16, 16, 40}, {24, 16, 72}};
        level = l;
        W = dims[l][0]; H = dims[l][1]; M = dims[l][2];
        reset();
        if (win) {
            Rect r = win->r;
            r.w = pref_w() + 2 * BORDER;
            r.h = pref_h() + TITLE_H + BORDER;
            r.x = clamp(r.x, 0, fb.width - r.w);
            r.y = clamp(r.y, TOPBAR_H, fb.height - r.h);
            wm_set_geometry(win, r);
        }
    }
    void reset() {
        memset(mine, 0, sizeof(mine)); memset(open, 0, sizeof(open)); memset(flag, 0, sizeof(flag));
        started = over = won = false;
        boom = -1;
        ui_invalidate();
    }
    void plant(int safe) {
        int sx = safe % W, sy = safe / W, placed = 0;
        while (placed < M) {
            int p = rand() % (W * H);
            int x = p % W, y = p / W;
            if (mine[p] || (iabs(x - sx) <= 1 && iabs(y - sy) <= 1)) continue;
            mine[p] = 1;
            placed++;
        }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx >= 0 && yy >= 0 && xx < W && yy < H) n += mine[yy * W + xx];
                    }
                num[y * W + x] = (u8)n;
            }
        started = true;
        t0 = ticks();
    }
    void reveal(int x, int y) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        int p = y * W + x;
        if (open[p] || flag[p]) return;
        open[p] = 1;
        if (mine[p]) { lose(p); return; }
        if (!num[p]) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if (dx || dy) reveal(x + dx, y + dy);
    }
    void lose(int p) {
        over = true;
        boom = p;
        t_end = ticks();
        for (int i = 0; i < W * H; i++) if (mine[i]) open[i] = 1;
        ui_beep(1);
    }
    void check_win() {
        if (over) return;
        for (int i = 0; i < W * H; i++) if (!mine[i] && !open[i]) return;
        over = won = true;
        t_end = ticks();
        for (int i = 0; i < W * H; i++) if (mine[i]) flag[i] = 1;
        int secs = (int)((t_end - t0) / 1000);
        char b[64];
        if (!best[level] || secs < best[level]) { best[level] = secs; save_best(); snprintf(b, sizeof(b), "new best: %d seconds!", secs); }
        else snprintf(b, sizeof(b), "cleared in %d seconds", secs);
        ui_notify("Minesweeper", b, "minesweeper");
        ui_beep(2);
    }
    void chord(int x, int y) {
        int p = y * W + x, f = 0;
        if (!open[p] || !num[p]) return;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int xx = x + dx, yy = y + dy;
            if (xx >= 0 && yy >= 0 && xx < W && yy < H) f += flag[yy * W + xx];
        }
        if (f == num[p]) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) reveal(x + dx, y + dy);
    }

    void led(Canvas& c, int x, int y, int v) {
        c.fill(x, y, 52, 26, 0xFF1A0A12);
        c.rect(x, y, 52, 26, th::ink);
        char b[8];
        snprintf(b, sizeof(b), "%03d", clamp(v, -99, 999));
        c.text(x + 3, y + 1, b, 0xFFFF4060, 2, true);
    }

    void draw(Canvas& c, int w, int h) override {
        int flags = 0;
        for (int i = 0; i < W * H; i++) flags += flag[i];
        draw_bevel(c, PAD - 2, 6, w - 2 * PAD + 4, 34, 0xFFC9C2DB, true);
        led(c, PAD + 2, 10, M - flags);
        int secs = started ? (int)(((over ? t_end : ticks()) - t0) / 1000) : 0;
        led(c, w - PAD - 54, 10, secs);
        // the chip face
        int fx = w / 2 - 14;
        bool fh = hot(fx, 9, 28, 28);
        draw_bevel(c, fx, 9, 28, 28, fh ? 0xFFFFE070 : 0xFFF0C840, fh && ui_mouse_left);
        u32 ink = th::ink;
        if (over && !won) { c.text(fx + 6, 13, "x", ink, 1, true); c.text(fx + 15, 13, "x", ink, 1, true); c.hline(fx + 9, 27, 10, ink); }
        else if (won) { c.fill(fx + 5, 15, 18, 4, ink); c.hline(fx + 9, 26, 10, ink); c.put(fx + 8, 25, ink); c.put(fx + 19, 25, ink); }
        else { c.fill(fx + 8, 15, 3, 3, ink); c.fill(fx + 17, 15, 3, 3, ink); c.hline(fx + 9, 26, 10, ink); c.put(fx + 8, 25, ink); c.put(fx + 19, 25, ink); }
        if (best[level]) { char b[24]; snprintf(b, sizeof(b), "best %ds", best[level]); c.text(PAD + 60, 17, b, 0xFF6A6088); }
        static const u32 ncol[9] = {0, 0xFF2F5DA8, 0xFF2E8B57, 0xFFE5486A, 0xFF4A3470, 0xFF8E2A2A, 0xFF16808C, 0xFF1A1428, 0xFF746A8C};
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int p = y * W + x;
                int px = PAD + x * CELL, py = TOP + y * CELL;
                if (!open[p]) {
                    bool hv = !over && hot(px, py, CELL, CELL);
                    draw_bevel(c, px, py, CELL, CELL, hv ? 0xFFE6E0F4 : 0xFFD6CFE6);
                    if (flag[p]) {
                        c.vline(px + 9, py + 4, 12, ink);
                        c.fill(px + 5, py + 4, 5, 5, th::red);
                        c.fill(px + 6, py + 15, 8, 2, ink);
                    }
                } else {
                    c.fill(px, py, CELL, CELL, p == boom ? th::red : 0xFFEDE9F5);
                    c.rect(px, py, CELL, CELL, 0xFFC0B8D4);
                    if (mine[p]) {
                        c.circle(px + 10, py + 10, 5, ink, true);
                        c.hline(px + 3, py + 10, 15, ink);
                        c.vline(px + 10, py + 3, 15, ink);
                        c.fill(px + 8, py + 8, 2, 2, th::white);
                    } else if (num[p]) {
                        char b[2] = {(char)('0' + num[p]), 0};
                        c.text(px + 6, py + 4, b, ncol[num[p]], 1, true);
                    }
                }
            }
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_UP && Rect{pref_w() / 2 - 14, 9, 28, 28}.contains(e.x, e.y)) { reset(); return; }
        int x = (e.x - PAD) / CELL, y = (e.y - TOP) / CELL;
        bool in = e.x >= PAD && e.y >= TOP && x < W && y < H;
        if (!in || over) { ui_invalidate(); return; }
        int p = y * W + x;
        if (e.kind == M_DOWN && e.button == 2 && !open[p]) { flag[p] ^= 1; ui_beep(0); }
        if (e.kind == M_DBL && e.button == 1) chord(x, y);
        if (e.kind == M_UP && e.button == 1) {
            if (!started) plant(p);
            if (open[p]) chord(x, y);
            else reveal(x, y);
            check_win();
        }
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        if (e.key == 'r' || e.key == K_F2) reset();
        if (e.key == '1') set_level(0);
        if (e.key == '2') set_level(1);
        if (e.key == '3') set_level(2);
    }
    void tick() override {
        static u64 last = 0;
        if (started && !over && ticks() - last > 1000) { last = ticks(); ui_invalidate(); }
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "Game", 24);
        m[0].add("New game", 1, "r").sep().add("Easy  9x9", 2, "1", true, level == 0)
            .add("Medium 16x16", 3, "2", true, level == 1).add("Hard  24x16", 4, "3", true, level == 2);
        return 1;
    }
    void on_command(int cmd) override { if (cmd == 1) reset(); else set_level(cmd - 2); ui_invalidate(); }
};

App* new_minesweeper(const char*) { return new MinesApp(); }
