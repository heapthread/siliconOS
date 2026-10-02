// silicon - Stacker: falling blocks (7 tetrominoes, ghost piece, hold, lock delay, line flashes)
#include "apps.h"

static const int BW = 10, BH = 20, CS = 22;
static const int BX = 14, BY = 14;               // board origin inside the window
static const int PANEL_X = BX + BW * CS + 16;

// each piece: 4 rotations x 4 cells, as (x, y) in a 4x4 box
static const u8 SHAPES[7][4][4][2] = {
    {{{0,1},{1,1},{2,1},{3,1}}, {{2,0},{2,1},{2,2},{2,3}}, {{0,2},{1,2},{2,2},{3,2}}, {{1,0},{1,1},{1,2},{1,3}}},   // I
    {{{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}},   // O
    {{{1,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{2,1},{1,2}}, {{1,0},{0,1},{1,1},{1,2}}},   // T
    {{{1,0},{2,0},{0,1},{1,1}}, {{1,0},{1,1},{2,1},{2,2}}, {{1,1},{2,1},{0,2},{1,2}}, {{0,0},{0,1},{1,1},{1,2}}},   // S
    {{{0,0},{1,0},{1,1},{2,1}}, {{2,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{1,2},{2,2}}, {{1,0},{0,1},{1,1},{0,2}}},   // Z
    {{{0,0},{0,1},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{1,2}}, {{0,1},{1,1},{2,1},{2,2}}, {{1,0},{1,1},{0,2},{1,2}}},   // J
    {{{2,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{1,2},{2,2}}, {{0,1},{1,1},{2,1},{0,2}}, {{0,0},{1,0},{1,1},{1,2}}},   // L
};
static const u32 COLORS[8] = {0, 0xFF50C8E6, 0xFFFADC5A, 0xFFAA6EFF, 0xFF5AD278, 0xFFFF6478, 0xFF5A82FF, 0xFFFFAA46};

struct Spark { int x, y, vx, vy; u32 c; u64 t0; };

struct StackerApp : App {
    u8 board[BH][BW];
    int cur = 0, rot = 0, px = 3, py = 0;
    int next[3], hold = -1;
    bool held = false;
    int bag[7], bag_n = 0;
    int score = 0, lines = 0, level = 1, best = 0;
    bool over = false, paused = false, started = false;
    u64 last_fall = 0, lock_t = 0;
    int lock_moves = 0;
    bool on_ground = false;
    // line clear animation
    int clearing[4], n_clearing = 0;
    u64 clear_t0 = 0;
    Vec<Spark> sparks;
    u64 shake_t0 = 0;
    char banner[24] = "";
    u64 banner_t0 = 0;

    StackerApp() {
        best = game_best("stacker");
        reset();
    }
    ~StackerApp() { sparks.free(); }
    const char* app_id() override { return "stacker"; }
    const char* title() override { return "Stacker"; }
    int pref_w() override { return PANEL_X + 150; }
    int pref_h() override { return BY * 2 + BH * CS; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF161224; }

    int draw_piece() {
        if (!bag_n) {
            for (int i = 0; i < 7; i++) bag[i] = i;
            for (int i = 6; i > 0; i--) { int j = rand() % (i + 1); int t = bag[i]; bag[i] = bag[j]; bag[j] = t; }
            bag_n = 7;
        }
        return bag[--bag_n];
    }
    void reset() {
        memset(board, 0, sizeof(board));
        bag_n = 0;
        for (int i = 0; i < 3; i++) next[i] = draw_piece();
        hold = -1;
        score = lines = 0;
        level = 1;
        over = paused = false;
        n_clearing = 0;
        sparks.clear();
        spawn();
        ui_invalidate();
    }
    void spawn() {
        cur = next[0];
        next[0] = next[1];
        next[1] = next[2];
        next[2] = draw_piece();
        rot = 0;
        px = 3;
        py = cur == 0 ? -1 : 0;
        held = false;
        on_ground = false;
        lock_moves = 0;
        last_fall = ticks();
        if (collides(px, py, rot)) {
            over = true;
            ui_beep(1);
            if (score > best) { best = score; game_save_best("stacker", best); ui_notify("Stacker", "new high score!", "stacker"); }
        }
    }
    bool collides(int x, int y, int r) {
        for (int k = 0; k < 4; k++) {
            int cx = x + SHAPES[cur][r][k][0], cy = y + SHAPES[cur][r][k][1];
            if (cx < 0 || cx >= BW || cy >= BH) return true;
            if (cy >= 0 && board[cy][cx]) return true;
        }
        return false;
    }
    int ghost_y() {
        int y = py;
        while (!collides(px, y + 1, rot)) y++;
        return y;
    }
    void touched() {      // a successful move while resting on the stack delays the lock
        if (on_ground && lock_moves < 15) { lock_t = ticks(); lock_moves++; }
    }
    bool move(int dx, int dy) {
        if (collides(px + dx, py + dy, rot)) return false;
        px += dx;
        py += dy;
        if (dx) touched();
        return true;
    }
    void rotate(int dir) {
        int nr = (rot + dir + 4) % 4;
        static const int kicks[][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {-2, 0}, {2, 0}, {-1, -1}, {1, -1}};
        for (auto& k : kicks)
            if (!collides(px + k[0], py + k[1], nr)) { px += k[0]; py += k[1]; rot = nr; touched(); ui_beep(0); return; }
    }
    void lock() {
        for (int k = 0; k < 4; k++) {
            int cx = px + SHAPES[cur][rot][k][0], cy = py + SHAPES[cur][rot][k][1];
            if (cy < 0) { over = true; continue; }
            board[cy][cx] = (u8)(cur + 1);
        }
        if (over) {
            ui_beep(1);
            if (score > best) { best = score; game_save_best("stacker", best); }
            return;
        }
        n_clearing = 0;
        for (int y = 0; y < BH; y++) {
            bool full = true;
            for (int x = 0; x < BW; x++) if (!board[y][x]) full = false;
            if (full) clearing[n_clearing++] = y;
        }
        if (n_clearing) {
            clear_t0 = ticks();
            static const int pts[5] = {0, 100, 300, 500, 800};
            score += pts[n_clearing] * level;
            static const char* names[5] = {"", "single", "double", "triple", "STACKER!"};
            strlcpy(banner, names[n_clearing], sizeof(banner));
            banner_t0 = ticks();
            if (n_clearing == 4) shake_t0 = ticks();
            for (int i = 0; i < n_clearing; i++)
                for (int x = 0; x < BW; x++)
                    for (int s = 0; s < 2; s++) {
                        Spark sp;
                        sp.x = (BX + x * CS + CS / 2) * 16;
                        sp.y = (BY + clearing[i] * CS + CS / 2) * 16;
                        sp.vx = (rand() % 81) - 40;
                        sp.vy = -(rand() % 60) - 10;
                        sp.c = COLORS[board[clearing[i]][x]];
                        sp.t0 = ticks();
                        sparks.push(sp);
                    }
            ui_beep(2);
        } else {
            ui_beep(0);
            spawn();
        }
    }
    void finish_clear() {
        for (int i = 0; i < n_clearing; i++) {
            int row = clearing[i];
            for (int y = row; y > 0; y--) memcpy(board[y], board[y - 1], BW);
            memset(board[0], 0, BW);
        }
        lines += n_clearing;
        level = 1 + lines / 10;
        n_clearing = 0;
        spawn();
    }
    void hard_drop() {
        int gy = ghost_y();
        score += (gy - py) * 2;
        // a streak where the piece fell
        for (int k = 0; k < 4; k++) {
            Spark sp;
            sp.x = (BX + (px + SHAPES[cur][rot][k][0]) * CS + CS / 2) * 16;
            sp.y = (BY + (gy + SHAPES[cur][rot][k][1]) * CS + CS) * 16;
            sp.vx = (rand() % 41) - 20;
            sp.vy = -(rand() % 30);
            sp.c = COLORS[cur + 1];
            sp.t0 = ticks();
            sparks.push(sp);
        }
        py = gy;
        lock();
    }
    void do_hold() {
        if (held) return;
        if (hold < 0) { hold = cur; spawn(); }
        else { int t = hold; hold = cur; cur = t; rot = 0; px = 3; py = cur == 0 ? -1 : 0; }
        held = true;
        on_ground = false;
    }

    void tick() override {
        u64 t = ticks();
        for (int i = sparks.len - 1; i >= 0; i--) {
            Spark& s = sparks[i];
            s.x += s.vx;
            s.y += s.vy;
            s.vy += 3;
            if (t - s.t0 > 900) sparks.remove(i);
        }
        if (sparks.len || (banner[0] && t - banner_t0 < 1200) || t - shake_t0 < 400) ui_invalidate();
        if (!started || over || paused) return;
        if (n_clearing) {
            ui_invalidate();
            if (t - clear_t0 > 260) finish_clear();
            return;
        }
        u64 interval = (u64)max(60, 800 - (level - 1) * 75);
        bool ground = collides(px, py + 1, rot);
        if (ground && !on_ground) { on_ground = true; lock_t = t; }
        if (!ground) on_ground = false;
        if (on_ground) {
            if (t - lock_t > 500) lock();
        } else if (t - last_fall >= interval) {
            last_fall = t;
            py++;
        }
        ui_invalidate();
    }

    void cell(Canvas& c, int x, int y, u32 col, bool ghost = false) {
        if (ghost) {
            c.rect(x + 1, y + 1, CS - 2, CS - 2, blend(0xFF161224, col, 150));
            c.rect(x + 2, y + 2, CS - 4, CS - 4, blend(0xFF161224, col, 70));
            return;
        }
        c.fill(x + 1, y + 1, CS - 2, CS - 2, col);
        c.fill(x + 1, y + 1, CS - 2, 3, blend(col, 0xFFFFFFFF, 110));
        c.fill(x + 1, y + 1, 3, CS - 2, blend(col, 0xFFFFFFFF, 70));
        c.fill(x + 1, y + CS - 4, CS - 2, 3, shade(col, 65));
        c.fill(x + CS - 4, y + 1, 3, CS - 2, shade(col, 75));
        c.fill(x + 7, y + 7, CS - 14, CS - 14, blend(col, 0xFFFFFFFF, 40));
    }
    void mini(Canvas& c, int x, int y, int p) {
        if (p < 0) return;
        for (int k = 0; k < 4; k++) {
            int cx = SHAPES[p][0][k][0], cy = SHAPES[p][0][k][1];
            int ox = p == 0 ? 0 : p == 1 ? -6 : 6;
            c.fill(x + ox + cx * 13 + 1, y + cy * 13 + 1, 11, 11, COLORS[p + 1]);
            c.hline(x + ox + cx * 13 + 1, y + cy * 13 + 1, 11, blend(COLORS[p + 1], 0xFFFFFFFF, 110));
        }
    }

    void draw(Canvas& c, int w, int h) override {
        u64 t = ticks();
        int sx = 0, sy = 0;
        if (t - shake_t0 < 400) { int a = (int)(400 - (t - shake_t0)) / 60; sx = (rand() % (2 * a + 1)) - a; sy = (rand() % (2 * a + 1)) - a; }
        // board well
        c.fill(BX - 4 + sx, BY - 4 + sy, BW * CS + 8, BH * CS + 8, 0xFF2A2442);
        c.fill(BX + sx, BY + sy, BW * CS, BH * CS, 0xFF0E0B18);
        for (int y = 0; y < BH; y++)
            for (int x = 0; x < BW; x++) c.fill(BX + sx + x * CS + CS / 2, BY + sy + y * CS + CS / 2, 1, 1, 0xFF2A2442);
        // settled blocks, with the clearing rows flashing white
        for (int y = 0; y < BH; y++) {
            bool clr = false;
            for (int i = 0; i < n_clearing; i++) if (clearing[i] == y) clr = true;
            for (int x = 0; x < BW; x++) {
                if (!board[y][x]) continue;
                u32 col = COLORS[board[y][x]];
                if (clr) {
                    int p = (int)min<u64>(255, (t - clear_t0) * 255 / 260);
                    col = blend(col, 0xFFFFFFFF, (u32)(((t - clear_t0) / 50) % 2 ? 255 : p));
                    int shrink = p * (CS / 2) / 255;
                    c.fill(BX + sx + x * CS + shrink, BY + sy + y * CS + shrink, CS - 2 * shrink, CS - 2 * shrink, col);
                    continue;
                }
                cell(c, BX + sx + x * CS, BY + sy + y * CS, col);
            }
        }
        if (started && !over && !n_clearing) {
            int gy = ghost_y();
            for (int k = 0; k < 4; k++) {
                int cx = px + SHAPES[cur][rot][k][0];
                if (gy + SHAPES[cur][rot][k][1] >= 0) cell(c, BX + sx + cx * CS, BY + sy + (gy + SHAPES[cur][rot][k][1]) * CS, COLORS[cur + 1], true);
            }
            // the falling piece glides between rows
            int interval = max(60, 800 - (level - 1) * 75);
            int frac = on_ground ? 0 : (int)min<u64>(CS - 1, (t - last_fall) * CS / (u64)interval);
            if (collides(px, py + 1, rot)) frac = 0;
            for (int k = 0; k < 4; k++) {
                int cx = px + SHAPES[cur][rot][k][0], cy = py + SHAPES[cur][rot][k][1];
                if (cy < 0) continue;
                u32 col = COLORS[cur + 1];
                if (on_ground) col = blend(col, 0xFFFFFFFF, (u32)(((t - lock_t) / 80) % 2 ? 40 : 0));
                cell(c, BX + sx + cx * CS, BY + sy + cy * CS + frac, col);
            }
        }
        for (auto& s : sparks) {
            int a = (int)min<u64>(255, (t - s.t0) * 255 / 900);
            c.fill(s.x / 16, s.y / 16, 3, 3, blend(s.c, 0xFF0E0B18, (u32)a));
        }
        // side panel
        int x = PANEL_X;
        c.text(x, BY, "NEXT", 0xFF8A7EB8, 1, true);
        draw_bevel(c, x, BY + 16, 120, 140, 0xFF221C36, true);
        for (int i = 0; i < 3; i++) mini(c, x + 34, BY + 26 + i * 44, next[i]);
        c.text(x, BY + 170, "HOLD", 0xFF8A7EB8, 1, true);
        draw_bevel(c, x, BY + 186, 120, 50, 0xFF221C36, true);
        mini(c, x + 34, BY + 196, hold);
        char b[32];
        auto stat = [&](int y, const char* label, int v) {
            c.text(x, y, label, 0xFF8A7EB8, 1, true);
            snprintf(b, sizeof(b), "%d", v);
            c.text(x, y + 14, b, 0xFFEDE6FF, 2, true);
        };
        stat(BY + 252, "SCORE", score);
        stat(BY + 296, "LINES", lines);
        stat(BY + 340, "LEVEL", level);
        stat(BY + 384, "BEST", best);
        if (banner[0] && t - banner_t0 < 1200) {
            int el = (int)(t - banner_t0);
            int rise = ease_out(min(256, el * 256 / 300)) * 30 / 256;
            u32 a = el > 900 ? (u32)(255 - (el - 900) * 255 / 300) : 255;
            int bw = text_width(banner, 2, true);
            c.text_shadow(BX + (BW * CS - bw) / 2, BY + BH * CS / 2 - rise, banner, blend(0xFF0E0B18, 0xFFFFE08A, a), 0xFF000000, 2, true);
        }
        if (!started || over || paused) {
            c.fill_alpha(BX, BY, BW * CS, BH * CS, 0xFF000000, 150);
            const char* tt = over ? "GAME OVER" : paused ? "PAUSED" : "STACKER";
            c.text_centered(BX, BY + 150, BW * CS, tt, 0xFFFFFFFF, over ? 3 : 3, true);
            c.text_centered(BX, BY + 200, BW * CS, over ? "space: again" : "space to start", 0xFFCFC4F5);
            if (!over && !paused) {
                static const char* help[] = {"left/right  move", "up / x  rotate", "z  rotate back", "down  soft drop",
                                             "space  hard drop", "c  hold", "p  pause"};
                for (int i = 0; i < 7; i++) c.text_centered(BX, BY + 240 + i * 16, BW * CS, help[i], 0xFF9A90C0);
            }
        }
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (!started || over) {
            if (e.key == ' ' || e.key == K_ENTER) { if (over) reset(); started = true; }
            ui_invalidate();
            return;
        }
        if (e.key == 'p' || e.key == K_ESC) { paused = !paused; ui_invalidate(); return; }
        if (paused || n_clearing) return;
        switch (e.key) {
        case K_LEFT: move(-1, 0); break;
        case K_RIGHT: move(1, 0); break;
        case K_DOWN: if (move(0, 1)) { score++; last_fall = ticks(); } break;
        case K_UP: case 'x': rotate(1); break;
        case 'z': rotate(-1); break;
        case ' ': hard_drop(); break;
        case 'c': do_hold(); break;
        }
        ui_invalidate();
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_UP && (!started || over)) { if (over) reset(); started = true; ui_invalidate(); }
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "Game", 24);
        m[0].add("New game", 1).add("Pause", 2, "p");
        return 1;
    }
    void on_command(int cmd) override {
        if (cmd == 1) { reset(); started = true; }
        else paused = !paused;
        ui_invalidate();
    }
};

App* new_stacker(const char*) { return new StackerApp(); }
