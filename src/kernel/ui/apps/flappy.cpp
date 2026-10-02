// silicon - Flappy Chip: flap a little chip through the PCB traces. Space, up or click to flap.
#include "apps.h"

static const int FW = 400, FH = 520, GROUND = 470;
static const int FP = 256;
static const int GAP = 132, PIPE_W = 58, SPACING = 200;

struct Pipe { int x; int gap_y; bool scored; };
struct Puff { int x, y; u64 t0; };

struct FlappyApp : App {
    int y = FH / 2 * FP, vy = 0;
    Vec<Pipe> pipes;
    Vec<Puff> puffs;
    int score = 0, best = 0;
    int scroll = 0;                       // world offset in pixels (for parallax)
    enum { READY, PLAY, DEAD } state = READY;
    u64 last = 0, dead_t0 = 0, score_t0 = 0;
    int stars_x[40], stars_y[40];

    FlappyApp() {
        best = game_best("flappy");
        for (int i = 0; i < 40; i++) { stars_x[i] = rand() % FW; stars_y[i] = rand() % 260; }
        reset();
    }
    ~FlappyApp() { pipes.free(); puffs.free(); }
    const char* app_id() override { return "flappy"; }
    const char* title() override { return "Flappy Chip"; }
    int pref_w() override { return FW; }
    int pref_h() override { return FH; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF1A1430; }

    void reset() {
        y = FH / 2 * FP;
        vy = 0;
        pipes.clear();
        puffs.clear();
        score = 0;
        state = READY;
        for (int i = 0; i < 3; i++) pipes.push(Pipe{FW + 120 + i * SPACING, rand_gap(), false});
        ui_invalidate();
    }
    int rand_gap() { return 90 + rand() % (GROUND - GAP - 150); }
    void flap() {
        if (state == DEAD) { if (ticks() - dead_t0 > 600) reset(); return; }
        if (state == READY) state = PLAY;
        vy = -6 * FP - FP / 2;
        puffs.push(Puff{90, y / FP + 8, ticks()});
        speaker_beep(880, 12);
    }
    void die() {
        if (state == DEAD) return;
        state = DEAD;
        dead_t0 = ticks();
        ui_beep(1);
        if (score > best) { best = score; game_save_best("flappy", best); ui_notify("Flappy Chip", "new high score!", "flappy"); }
    }

    void tick() override {
        u64 t = ticks();
        int frames = (int)min<u64>(6, (t - last) / 12);       // 12 ms steps
        if (!frames) return;
        last += (u64)frames * 12;
        if (t - last > 100) last = t;
        for (int i = puffs.len - 1; i >= 0; i--) if (t - puffs[i].t0 > 400) puffs.remove(i);
        ui_invalidate();
        for (int f = 0; f < frames; f++) {
            if (state == READY) { y = (FH / 2 + (int)(4 * ((t / 120) % 2 ? 1 : -1))) * FP; scroll += 2; continue; }
            // gravity, even when dead (the chip falls to the ground)
            vy = min(vy + FP * 4 / 10, 10 * FP);
            y += vy;
            if (y / FP > GROUND - 12) { y = (GROUND - 12) * FP; die(); }
            if (y < 0) { y = 0; vy = 0; }
            if (state != PLAY) continue;
            scroll += 2;
            for (auto& p : pipes) {
                p.x -= 2;
                if (!p.scored && p.x + PIPE_W < 80) { p.scored = true; score++; score_t0 = t; speaker_beep(1320, 20); }
                // collision with the chip's box
                int cx = 80, cy = y / FP, r = 11;
                if (cx + r > p.x && cx - r < p.x + PIPE_W && (cy - r < p.gap_y || cy + r > p.gap_y + GAP)) die();
            }
            if (pipes.len && pipes[0].x + PIPE_W < -10) {
                pipes.remove(0);
                pipes.push(Pipe{pipes.back().x + SPACING, rand_gap(), false});
            }
        }
    }

    void draw_pipe(Canvas& c, int x, int y0, int y1, bool top) {
        if (y1 <= y0) return;
        // a gold PCB trace bundle with a via cap
        c.fill(x, y0, PIPE_W, y1 - y0, 0xFF2E7A4E);
        c.fill(x + 4, y0, 6, y1 - y0, 0xFF4FBF84);
        for (int k = 14; k < PIPE_W - 6; k += 10) c.fill(x + k, y0, 3, y1 - y0, 0xFFE0B456);
        c.vline(x, y0, y1 - y0, 0xFF1A4A30);
        c.vline(x + PIPE_W - 1, y0, y1 - y0, 0xFF1A4A30);
        int cy = top ? y1 - 18 : y0;
        c.fill(x - 5, cy, PIPE_W + 10, 18, 0xFF3A9A62);
        c.hline(x - 5, cy, PIPE_W + 10, 0xFF7FE0A8);
        c.hline(x - 5, cy + 17, PIPE_W + 10, 0xFF1A4A30);
        for (int k = 0; k < 4; k++) c.circle(x + 8 + k * 14, cy + 9, 3, 0xFFE0B456, true);
    }

    void draw_chip(Canvas& c, int cx, int cy, int tilt) {
        // tilt -2..3: the body is drawn as columns shifted vertically to fake a rotation
        for (int col = -11; col <= 11; col++) {
            int dy = col * tilt / 6;
            int x = cx + col;
            bool edge = col == -11 || col == 11;
            c.fill(x, cy - 8 + dy, 1, 17, edge ? 0xFF221C36 : 0xFF3A3354);
            if (col > -8 && col < 8) c.fill(x, cy - 4 + dy, 1, 9, 0xFF9B7BFF);
            if (col > -4 && col < 4) c.fill(x, cy - 1 + dy, 1, 3, 0xFFDCCFFF);
            if ((col + 11) % 5 == 2) {
                c.fill(x, cy - 11 + dy, 1, 3, th::gold);
                c.fill(x, cy + 9 + dy, 1, 3, th::gold);
            }
        }
        // eye
        int ey = cy - 5 + 6 * tilt / 6;
        c.fill(cx + 5, ey, 4, 4, 0xFFFFFFFF);
        c.fill(cx + 7, ey + 1, 2, 2, 0xFF101018);
        // wing pins flapping
        int wing = vy < 0 ? -3 : 2;
        c.fill(cx - 15, cy + wing, 5, 2, th::gold);
        c.fill(cx - 15, cy + wing + 4, 5, 2, th::gold);
    }

    void draw(Canvas& c, int w, int h) override {
        u64 t = ticks();
        // sky
        c.gradient_v(0, 0, FW, GROUND, 0xFF120E26, 0xFF3A2456, 8);
        for (int i = 0; i < 40; i++) {
            int sx = ((stars_x[i] - scroll / 8) % FW + FW) % FW;
            c.fill(sx, stars_y[i], 1, 1, (i + t / 400) % 7 ? 0xFF8A7EB8 : 0xFFFFFFFF);
        }
        // far city of server racks (slow parallax)
        for (int i = 0; i < 12; i++) {
            int bw = 34, bx = ((i * 52 - scroll / 4) % (12 * 52) + 12 * 52) % (12 * 52) - 52;
            int bh = 60 + (i * 37) % 90;
            c.fill(bx, GROUND - bh, bw, bh, 0xFF241A3C);
            for (int ly = GROUND - bh + 8; ly < GROUND - 6; ly += 10)
                c.fill(bx + 6 + (ly / 10 % 3) * 8, ly, 4, 2, ((ly + i * 7 + t / 300) % 5) ? 0xFF3A2E5A : 0xFF7FE0A8);
        }
        for (auto& p : pipes) {
            draw_pipe(c, p.x, 0, p.gap_y, true);
            draw_pipe(c, p.x, p.gap_y + GAP, GROUND, false);
        }
        // ground: a scrolling circuit board edge
        c.fill(0, GROUND, FW, FH - GROUND, 0xFF2A1F44);
        c.hline(0, GROUND, FW, 0xFF9B7BFF);
        for (int x = -(scroll % 24); x < FW; x += 24) {
            c.fill(x, GROUND + 6, 12, 3, th::gold);
            c.fill(x + 10, GROUND + 6, 3, 14, th::gold);
            c.circle(x + 11, GROUND + 22, 3, th::gold, true);
        }
        for (auto& pf : puffs) {
            int el = (int)(t - pf.t0);
            int r = 2 + el / 80;
            c.circle(pf.x - el / 20, pf.y + el / 30, r, blend(0xFF3A2456, 0xFFDCCFFF, (u32)max(0, 200 - el / 2)), false);
        }
        int tilt = clamp(vy / FP / 2, -2, 3);
        if (state == DEAD) tilt = 3;
        draw_chip(c, 80, y / FP, tilt);
        // score
        char b[16];
        snprintf(b, sizeof(b), "%d", score);
        int pop = t - score_t0 < 200 ? 1 : 0;
        c.text_shadow((FW - text_width(b, 4 + pop, true)) / 2, 30 - pop * 4, b, 0xFFFFFFFF, 0xFF120E26, 4 + pop, true);
        if (state == READY) {
            c.text_centered(0, 150, FW, "FLAPPY CHIP", 0xFFFFE08A, 3, true);
            c.text_centered(0, 330, FW, "space, up or click to flap", 0xFFDCCFFF, 1, true);
            snprintf(b, sizeof(b), "best %d", best);
            c.text_centered(0, 350, FW, b, 0xFF9A90C0);
        }
        if (state == DEAD) {
            int el = (int)min<u64>(256, (t - dead_t0) * 256 / 300);
            if (t - dead_t0 < 120) c.fill_alpha(0, 0, FW, FH, 0xFFFFFFFF, 140);
            int py = 140 + (256 - ease_out(el)) * 60 / 256;
            draw_bevel(c, 70, py, FW - 140, 140, 0xFFF4F0FA);
            c.text_centered(70, py + 16, FW - 140, "game over", 0xFF4A3E6A, 2, true);
            snprintf(b, sizeof(b), "score %d", score);
            c.text_centered(70, py + 56, FW - 140, b, 0xFF4A3E6A, 1, true);
            snprintf(b, sizeof(b), "best %d", best);
            c.text_centered(70, py + 76, FW - 140, b, 0xFF8A7EA8, 1, true);
            const char* medal = score >= 40 ? "platinum die" : score >= 20 ? "gold pins" : score >= 10 ? "silver trace" : "";
            if (medal[0]) c.text_centered(70, py + 100, FW - 140, medal, 0xFFC08A2E, 1, true);
            if (t - dead_t0 > 600) c.text_centered(0, py + 160, FW, "flap to try again", 0xFFFFFFFF, 1, true);
        }
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.key == ' ' || e.key == K_UP || e.key == 'w' || e.key == K_ENTER) flap();
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_DOWN && e.button == 1) flap();
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "Game", 24);
        m[0].add("New game", 1);
        return 1;
    }
    void on_command(int) override { reset(); }
};

App* new_flappy(const char*) { return new FlappyApp(); }
