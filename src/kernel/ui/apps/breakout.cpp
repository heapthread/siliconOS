// silicon - Breakout: knock the transistors off the die. Mouse or arrows move the paddle,
// some bricks drop power-ups (wide paddle, multi-ball, slow ball, extra life).
#include "apps.h"

static const int FW = 520, FH = 440, HUD = 28;
static const int COLS = 13, ROWS = 7, BRW = 38, BRH = 16, BR_X = 13, BR_Y = 46;
static const int FP = 256;               // fixed point

struct Ball { int x, y, vx, vy; bool stuck; int trail_x[6], trail_y[6]; };
struct Drop { int x, y, kind; };
struct Bit { int x, y, vx, vy; u32 c; u64 t0; };

struct BreakoutApp : App {
    u8 bricks[ROWS][COLS];               // 0 empty, 1..3 hits left, 9 steel
    u8 color_row[ROWS][COLS];
    Vec<Ball> balls;
    Vec<Drop> drops;
    Vec<Bit> bits;
    int pad_x = FW / 2, pad_w = 80, pad_target = FW / 2;
    int lives = 3, score = 0, best = 0, level = 1;
    bool over = false, paused = false, cleared = false;
    bool left_down = false, right_down = false;
    u64 last = 0, wide_until = 0, slow_until = 0, flash_t0 = 0, level_t0 = 0;

    BreakoutApp() {
        best = game_best("breakout");
        new_game();
    }
    ~BreakoutApp() { balls.free(); drops.free(); bits.free(); }
    const char* app_id() override { return "breakout"; }
    const char* title() override { return "Breakout"; }
    int pref_w() override { return FW; }
    int pref_h() override { return FH + HUD; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF100C1C; }
    bool wants_key_up() override { return true; }
    int cursor_at(int, int) override { return CUR_CROSS; }

    void new_game() {
        lives = 3;
        score = 0;
        level = 1;
        over = false;
        build_level();
    }
    void build_level() {
        memset(bricks, 0, sizeof(bricks));
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++) {
                bool on = true;
                switch ((level - 1) % 5) {
                case 1: on = (r + c) % 2 == 0; break;                                   // checkers
                case 2: on = c >= r && c < COLS - r; break;                              // pyramid
                case 3: on = c != 6 && r != 3; break;                                    // crossroads
                case 4: on = (c / 2 + r) % 3 != 0; break;                                // stripes
                }
                if (!on) continue;
                bricks[r][c] = (u8)(r < 2 ? 2 : 1);
                if (level >= 3 && r == 0) bricks[r][c] = 3;
                if (level >= 4 && r == 3 && c % 4 == 1) bricks[r][c] = 9;
                color_row[r][c] = (u8)r;
            }
        balls.clear();
        drops.clear();
        serve();
        cleared = false;
        level_t0 = ticks();
        wide_until = slow_until = 0;
        pad_w = 80;
    }
    void serve() {
        Ball b;
        memset(&b, 0, sizeof(b));
        b.stuck = true;
        b.x = pad_x * FP;
        b.y = (FH - 30) * FP;
        for (int i = 0; i < 6; i++) { b.trail_x[i] = b.x; b.trail_y[i] = b.y; }
        balls.push(b);
    }
    void launch_ball(Ball& b) {
        if (!b.stuck) return;
        b.stuck = false;
        int sp = speed();
        b.vx = (rand() % 2 ? 1 : -1) * sp * 5 / 10;
        b.vy = -sp * 87 / 100;
    }
    int speed() { return (4 * FP + min(level, 6) * FP * 4 / 10) * (ticks() < slow_until ? 6 : 10) / 10; }
    static int isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }

    void burst(int x, int y, u32 col, int n) {
        for (int i = 0; i < n; i++) {
            Bit b{x * 16, y * 16, (int)(rand() % 97) - 48, -(int)(rand() % 64) - 8, col, ticks()};
            bits.push(b);
        }
    }
    u32 brick_color(int r, int hits) {
        static const u32 rows[ROWS] = {0xFFFF6478, 0xFFFF9A50, 0xFFFAD25A, 0xFF5AD278, 0xFF50C8E6, 0xFF7C8CFF, 0xFFB47CFF};
        if (hits == 9) return 0xFF8A8AA0;
        u32 c = rows[r];
        return hits > 1 ? shade(c, 80 + 0 * hits) : c;
    }

    void hit_brick(int r, int c) {
        u8& b = bricks[r][c];
        if (b == 9) { ui_beep(0); return; }
        int x = BR_X + c * BRW + BRW / 2, y = BR_Y + r * BRH + BRH / 2;
        if (--b == 0) {
            score += 10 * level;
            burst(x, y, brick_color(r, 1), 10);
            ui_beep(0);
            if (rand() % 7 == 0) drops.push(Drop{x * FP, y * FP, (int)(rand() % 4)});
        } else {
            score += 5;
            burst(x, y, 0xFFFFFFFF, 3);
        }
    }

    void step_ball(Ball& b, int dt_frames) {
        if (b.stuck) { b.x = pad_x * FP; b.y = (FH - 30) * FP; return; }
        for (int s = 0; s < dt_frames; s++) {
            // move in two halves (x then y) so we know which side a brick was hit on
            b.x += b.vx;
            if (b.x < 5 * FP) { b.x = 5 * FP; b.vx = -b.vx; }
            if (b.x > (FW - 5) * FP) { b.x = (FW - 5) * FP; b.vx = -b.vx; }
            if (brick_at(b.x, b.y)) b.vx = -b.vx, b.x += b.vx;
            b.y += b.vy;
            if (b.y < (5) * FP) { b.y = 5 * FP; b.vy = -b.vy; }
            if (brick_at(b.x, b.y)) b.vy = -b.vy, b.y += b.vy;
            // paddle: the bounce angle depends on where it hits
            int py = FH - 22;
            if (b.vy > 0 && b.y / FP >= py - 4 && b.y / FP <= py + 6 && iabs(b.x / FP - pad_x) <= pad_w / 2 + 4) {
                int off = (b.x / FP - pad_x) * 256 / (pad_w / 2 + 4);           // -256..256
                int sp = speed();
                b.vx = sp * off / 256 * 9 / 10;
                int vy = isqrt(max(0, (sp * sp - b.vx * b.vx) / 64)) * 8;      // keep the speed constant
                b.vy = -max(vy, sp / 3);
                b.y = (py - 5) * FP;
                ui_beep(0);
            }
        }
        for (int i = 5; i > 0; i--) { b.trail_x[i] = b.trail_x[i - 1]; b.trail_y[i] = b.trail_y[i - 1]; }
        b.trail_x[0] = b.x;
        b.trail_y[0] = b.y;
    }
    bool brick_at(int fx, int fy) {
        int x = fx / FP, y = fy / FP;
        if (x < BR_X || y < BR_Y) return false;
        int c = (x - BR_X) / BRW, r = (y - BR_Y) / BRH;
        if (c < 0 || c >= COLS || r < 0 || r >= ROWS || !bricks[r][c]) return false;
        hit_brick(r, c);
        return true;
    }

    void tick() override {
        u64 t = ticks();
        int frames = (int)min<u64>(6, (t - last) / 8);      // 8 ms physics steps
        if (!frames) return;
        last += (u64)frames * 8;
        if (t - last > 100) last = t;
        for (int i = bits.len - 1; i >= 0; i--) {
            Bit& b = bits[i];
            b.x += b.vx * frames;
            b.y += b.vy * frames;
            b.vy += 4 * frames;
            if (t - b.t0 > 700) bits.remove(i);
        }
        ui_invalidate();
        if (over || paused || cleared) {
            if (cleared && t - level_t0 > 1600) { level++; build_level(); }
            return;
        }
        // paddle: keys push the target, the paddle eases toward it
        if (left_down) pad_target -= 7 * frames;
        if (right_down) pad_target += 7 * frames;
        pad_w = t < wide_until ? 130 : 80;
        pad_target = clamp(pad_target, pad_w / 2, FW - pad_w / 2);
        pad_x += (pad_target - pad_x) * min(10, frames * 4) / 10;
        for (int i = balls.len - 1; i >= 0; i--) {
            step_ball(balls[i], frames);
            if (balls[i].y > (FH + 10) * FP) balls.remove(i);
        }
        if (!balls.len) {
            lives--;
            flash_t0 = t;
            ui_beep(1);
            if (lives <= 0) {
                over = true;
                if (score > best) { best = score; game_save_best("breakout", best); ui_notify("Breakout", "new high score!", "breakout"); }
            } else serve();
        }
        for (int i = drops.len - 1; i >= 0; i--) {
            Drop& d = drops[i];
            d.y += FP * frames * 2 / 2;
            int x = d.x / FP, y = d.y / FP;
            if (y >= FH - 26 && y <= FH - 14 && iabs(x - pad_x) <= pad_w / 2 + 8) {
                if (d.kind == 0) wide_until = t + 12000;
                else if (d.kind == 1) {
                    int n = balls.len;
                    for (int k = 0; k < n && balls.len < 6; k++) {
                        Ball nb = balls[k];
                        nb.stuck = false;
                        nb.vx = -nb.vx + ((int)(rand() % 3) - 1) * FP / 2;
                        if (!nb.vy) nb.vy = -speed();
                        balls.push(nb);
                    }
                } else if (d.kind == 2) slow_until = t + 10000;
                else lives = min(lives + 1, 6);
                burst(x, y, 0xFFFFE08A, 12);
                ui_beep(2);
                drops.remove(i);
            } else if (y > FH + 10) drops.remove(i);
        }
        bool any = false;
        for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) if (bricks[r][c] && bricks[r][c] != 9) any = true;
        if (!any) { cleared = true; level_t0 = t; score += 100 * level; ui_beep(2); }
    }

    void draw(Canvas& c, int w, int h) override {
        u64 t = ticks();
        // hud
        c.fill(0, 0, w, HUD, 0xFF1C1630);
        char b[64];
        snprintf(b, sizeof(b), "score %d", score);
        c.text(10, 8, b, 0xFFEDE6FF, 1, true);
        snprintf(b, sizeof(b), "level %d", level);
        c.text_centered(0, 8, w, b, 0xFFB8A8F0, 1, true);
        for (int i = 0; i < lives; i++) { c.fill(w - 16 - i * 16, 10, 10, 8, 0xFFB47CFF); c.fill(w - 15 - i * 16, 11, 8, 2, 0xFFDCCFFF); }
        snprintf(b, sizeof(b), "best %d", best);
        c.text(w - 16 - 6 * 16 - text_width(b), 8, b, 0xFF7A70A0);
        int ox = c.ox, oy = c.oy;
        c.origin(ox, oy + HUD);
        // die background: faint grid traces
        c.fill(0, 0, FW, FH, 0xFF100C1C);
        for (int x = 20; x < FW; x += 40) c.vline(x, 0, FH, 0xFF161228);
        for (int y = 20; y < FH; y += 40) c.hline(0, y, FW, 0xFF161228);
        if (t - flash_t0 < 300) c.fill_alpha(0, 0, FW, FH, 0xFFFF4060, (u32)(80 - (t - flash_t0) * 80 / 300));
        // bricks
        for (int r = 0; r < ROWS; r++)
            for (int col = 0; col < COLS; col++) {
                int v = bricks[r][col];
                if (!v) continue;
                int x = BR_X + col * BRW, y = BR_Y + r * BRH;
                u32 cc = brick_color(r, v);
                c.fill(x + 1, y + 1, BRW - 2, BRH - 2, cc);
                c.hline(x + 1, y + 1, BRW - 2, blend(cc, 0xFFFFFFFF, 120));
                c.hline(x + 1, y + BRH - 2, BRW - 2, shade(cc, 60));
                if (v == 9) { c.fill(x + 6, y + 6, 4, 4, 0xFF5A5A70); c.fill(x + BRW - 10, y + 6, 4, 4, 0xFF5A5A70); }
                else if (v > 1) for (int k = 0; k < v; k++) c.fill(x + 6 + k * 6, y + 6, 3, 3, blend(cc, 0xFFFFFFFF, 160));
            }
        // drops: little capsules with a letter
        static const char* dl[4] = {"W", "M", "S", "+"};
        static const u32 dc[4] = {0xFF50C8E6, 0xFFFF9A50, 0xFF5AD278, 0xFFFF6478};
        for (auto& d : drops) {
            int x = d.x / FP, y = d.y / FP;
            c.fill(x - 11, y - 6, 22, 12, dc[d.kind]);
            c.hline(x - 11, y - 6, 22, blend(dc[d.kind], 0xFFFFFFFF, 140));
            c.text(x - 4, y - 5, dl[d.kind], 0xFF100C1C, 1, true);
        }
        // paddle
        int py = FH - 22;
        u32 pc = t < wide_until ? 0xFF50C8E6 : 0xFFB47CFF;
        c.fill(pad_x - pad_w / 2, py, pad_w, 8, pc);
        c.hline(pad_x - pad_w / 2, py, pad_w, blend(pc, 0xFFFFFFFF, 150));
        for (int k = 4; k < pad_w - 4; k += 8) c.fill(pad_x - pad_w / 2 + k, py + 8, 3, 3, th::gold);
        // balls with a fading trail
        for (auto& bl : balls) {
            for (int i = 5; i >= 1; i--)
                c.fill(bl.trail_x[i] / FP - 2, bl.trail_y[i] / FP - 2, 5, 5, blend(0xFF100C1C, t < slow_until ? 0xFF5AD278 : 0xFFDCCFFF, (u32)(150 - i * 25)));
            c.fill(bl.x / FP - 4, bl.y / FP - 4, 9, 9, 0xFFFFFFFF);
            c.fill(bl.x / FP - 3, bl.y / FP - 5, 7, 11, 0xFFFFFFFF);
            c.fill(bl.x / FP - 5, bl.y / FP - 3, 11, 7, 0xFFFFFFFF);
        }
        for (auto& bt : bits) {
            u32 a = (u32)min<u64>(255, (t - bt.t0) * 255 / 700);
            c.fill(bt.x / 16, bt.y / 16, 3, 3, blend(bt.c, 0xFF100C1C, a));
        }
        if (balls.len && balls[0].stuck && !over && !cleared)
            c.text_centered(0, FH / 2 + 40, FW, "click or space to launch", 0xFF9A90C0);
        if (cleared) {
            snprintf(b, sizeof(b), "level %d clear!", level);
            c.text_centered(0, FH / 2 - 20, FW, b, 0xFFFFE08A, 3, true);
        }
        if (over || paused) {
            c.fill_alpha(0, 0, FW, FH, 0xFF000000, 150);
            c.text_centered(0, FH / 2 - 40, FW, over ? "GAME OVER" : "PAUSED", 0xFFFFFFFF, 4, true);
            c.text_centered(0, FH / 2 + 20, FW, over ? "space or click to play again" : "p to resume", 0xFFCFC4F5);
        }
        c.origin(ox, oy);
    }
    void on_key(const KeyEvent& e) override {
        if (e.key == K_LEFT || e.key == 'a') left_down = e.down;
        if (e.key == K_RIGHT || e.key == 'd') right_down = e.down;
        if (!e.down) return;
        if (e.key == ' ' || e.key == K_UP) {
            if (over) { new_game(); return; }
            for (auto& b : balls) launch_ball(b);
        }
        if (e.key == 'p' || e.key == K_ESC) paused = !paused;
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_MOVE || e.kind == M_DOWN) pad_target = e.x;
        if (e.kind == M_DOWN && e.button == 1) {
            if (over) { new_game(); return; }
            for (auto& b : balls) launch_ball(b);
        }
    }
    bool hover_effects() override { return true; }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "Game", 24);
        m[0].add("New game", 1).add("Pause", 2, "p");
        return 1;
    }
    void on_command(int cmd) override { if (cmd == 1) new_game(); else paused = !paused; }
};

App* new_breakout(const char*) { return new BreakoutApp(); }
