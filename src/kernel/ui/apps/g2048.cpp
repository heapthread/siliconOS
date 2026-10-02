// silicon - 2048: slide the chips together. Tiles glide, merges pop, new tiles grow in.
#include "apps.h"

static const int N = 4, TS = 92, GAP = 10, TOP = 70;
static const int BOARD = N * TS + (N + 1) * GAP;

struct Tile {
    int v;
    int x, y;          // destination cell
    int fx, fy;        // where the slide started
    bool merged, fresh;
    bool dying;        // slid into another tile; removed when the animation ends
};

struct G2048App : App {
    Vec<Tile> tiles;
    int score = 0, best = 0, shown_score = 0;
    bool over = false, won = false, keep_going = false;
    u64 anim_t0 = 0;
    int gain = 0;
    u64 gain_t0 = 0;
    // undo one move
    Vec<Tile> undo_tiles;
    int undo_score = 0;
    bool can_undo = false;

    G2048App() {
        best = game_best("2048");
        reset();
    }
    ~G2048App() { tiles.free(); undo_tiles.free(); }
    const char* app_id() override { return "g2048"; }
    const char* title() override { return "2048"; }
    int pref_w() override { return BOARD + 28; }
    int pref_h() override { return TOP + BOARD + 40; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFFF4F0FA; }

    int at(int x, int y) {
        for (int i = 0; i < tiles.len; i++) if (!tiles[i].dying && tiles[i].x == x && tiles[i].y == y) return i;
        return -1;
    }
    void add_random() {
        int free_cells[16], n = 0;
        for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) if (at(x, y) < 0) free_cells[n++] = y * N + x;
        if (!n) return;
        int c = free_cells[rand() % n];
        Tile t{rand() % 10 ? 2 : 4, c % N, c / N, c % N, c / N, false, true, false};
        tiles.push(t);
    }
    void reset() {
        tiles.clear();
        score = shown_score = 0;
        over = won = keep_going = false;
        can_undo = false;
        add_random();
        add_random();
        anim_t0 = ticks();
        ui_invalidate();
    }
    bool animating() { return ticks() - anim_t0 < 220; }
    void settle() {
        for (int i = tiles.len - 1; i >= 0; i--) {
            if (tiles[i].dying) { tiles.remove(i); continue; }
            tiles[i].fx = tiles[i].x;
            tiles[i].fy = tiles[i].y;
            tiles[i].merged = tiles[i].fresh = false;
        }
    }
    bool moves_left() {
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                int a = at(x, y);
                if (a < 0) return true;
                int r = x + 1 < N ? at(x + 1, y) : -1, d = y + 1 < N ? at(x, y + 1) : -1;
                if (r >= 0 && tiles[r].v == tiles[a].v) return true;
                if (d >= 0 && tiles[d].v == tiles[a].v) return true;
            }
        return false;
    }
    // dir: 0 up, 1 right, 2 down, 3 left
    void slide(int dir) {
        if (over || (won && !keep_going)) return;
        settle();
        undo_tiles.clear();
        for (int i = 0; i < tiles.len; i++) undo_tiles.push(tiles[i]);
        int prev_score = score;
        int dx = dir == 1 ? 1 : dir == 3 ? -1 : 0, dy = dir == 2 ? 1 : dir == 0 ? -1 : 0;
        bool moved = false;
        gain = 0;
        // walk cells starting from the side we're sliding toward
        for (int line = 0; line < N; line++) {
            int last_merge_pos = -99;
            for (int k = 0; k < N; k++) {
                int x, y;
                if (dx) { x = dx > 0 ? N - 1 - k : k; y = line; }
                else { y = dy > 0 ? N - 1 - k : k; x = line; }
                int i = at(x, y);
                if (i < 0) continue;
                int cx = x, cy = y;
                while (true) {
                    int nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= N || ny >= N) break;
                    int j = at(nx, ny);
                    if (j < 0) { cx = nx; cy = ny; continue; }
                    int pos = dx ? nx : ny;
                    if (tiles[j].v == tiles[i].v && !tiles[j].merged && pos != last_merge_pos) {
                        // merge: i slides onto j and disappears, j doubles
                        tiles[j].v *= 2;
                        tiles[j].merged = true;
                        score += tiles[j].v;
                        gain += tiles[j].v;
                        last_merge_pos = pos;
                        tiles[i].x = nx;
                        tiles[i].y = ny;
                        tiles[i].dying = true;
                        moved = true;
                        if (tiles[j].v == 2048 && !won) { won = true; ui_notify("2048", "you made a 2048 chip!", "g2048"); ui_beep(2); }
                    }
                    break;
                }
                if (!tiles[i].dying && (cx != x || cy != y)) { tiles[i].x = cx; tiles[i].y = cy; moved = true; }
            }
        }
        if (!moved) return;
        undo_score = prev_score;
        can_undo = true;
        add_random();
        anim_t0 = ticks();
        if (gain) { gain_t0 = ticks(); ui_beep(0); }
        if (score > best) { best = score; game_save_best("2048", best); }
        if (!moves_left()) { over = true; ui_beep(1); }
        ui_invalidate();
    }
    void undo() {
        if (!can_undo) return;
        tiles.clear();
        for (int i = 0; i < undo_tiles.len; i++) tiles.push(undo_tiles[i]);
        settle();
        score = shown_score = undo_score;
        over = false;
        can_undo = false;
        ui_invalidate();
    }

    static u32 tile_color(int v) {
        switch (v) {
        case 2: return 0xFFECE4FA;
        case 4: return 0xFFDCCFFA;
        case 8: return 0xFFC4A8FF;
        case 16: return 0xFFA884FF;
        case 32: return 0xFF8E62F2;
        case 64: return 0xFF7444DC;
        case 128: return 0xFFFFC870;
        case 256: return 0xFFFFB24A;
        case 512: return 0xFFFF9636;
        case 1024: return 0xFFFF7A5A;
        case 2048: return 0xFFFF5A82;
        default: return 0xFF3A2E5A;
        }
    }
    Rect cell_rect(int x, int y) { return {14 + GAP + x * (TS + GAP), TOP + GAP + y * (TS + GAP), TS, TS}; }

    void tick() override {
        if (animating() || shown_score != score || ticks() - gain_t0 < 700) {
            if (shown_score < score) shown_score += max(1, (score - shown_score) / 6);
            if (shown_score > score) shown_score = score;
            ui_invalidate();
        } else if (tiles.len) {
            bool dirty = false;
            for (auto& t : tiles) if (t.dying || t.merged || t.fresh || t.fx != t.x || t.fy != t.y) dirty = true;
            if (dirty) { settle(); ui_invalidate(); }
        }
    }

    void draw_tile(Canvas& c, Rect r, int v, int scale256) {
        int w = r.w * scale256 / 256, h = r.h * scale256 / 256;
        int x = r.x + (r.w - w) / 2, y = r.y + (r.h - h) / 2;
        u32 col = tile_color(v);
        c.fill(x, y + 3, w, h, shade(col, 70));
        c.fill(x, y, w, h, col);
        c.fill(x, y, w, 3, blend(col, 0xFFFFFFFF, 120));
        // tiny gold pins along the sides: they're chips after all
        if (scale256 > 200) {
            for (int k = 0; k < 4; k++) {
                int py = y + 14 + k * (h - 28) / 3;
                c.fill(x - 3, py, 3, 4, th::gold);
                c.fill(x + w, py, 3, 4, th::gold);
            }
        }
        char b[12];
        snprintf(b, sizeof(b), "%d", v);
        int len = (int)strlen(b);
        int sc = len <= 2 ? 4 : len == 3 ? 3 : 2;
        if (scale256 < 200) sc = max(1, sc * scale256 / 256);
        u32 tc = v <= 4 ? 0xFF4A3E6A : 0xFFFFFFFF;
        int tw = text_width(b, sc, true);
        c.text(x + (w - tw) / 2, y + (h - 12 * sc) / 2 + 1, b, tc, sc, true);
    }

    void draw(Canvas& c, int w, int h) override {
        // header
        c.text(14, 14, "2048", 0xFF5A4A8A, 4, true);
        auto box = [&](int x, const char* label, int v) {
            c.fill(x, 12, 92, 44, 0xFF8E7EB8);
            c.text_centered(x, 16, 92, label, 0xFFE6DCFF, 1, true);
            char b[16];
            snprintf(b, sizeof(b), "%d", v);
            c.text_centered(x, 32, 92, b, 0xFFFFFFFF, 2, true);
        };
        box(w - 14 - 92 * 2 - 8, "SCORE", shown_score);
        box(w - 14 - 92, "BEST", best);
        u64 t = ticks();
        if (t - gain_t0 < 700 && gain) {
            int el = (int)(t - gain_t0);
            char b[16];
            snprintf(b, sizeof(b), "+%d", gain);
            c.text_centered(w - 14 - 92 * 2 - 8, 30 - el * 26 / 700, 92, b, blend(0xFF8E7EB8, 0xFF4A3E6A, (u32)(255 - el * 255 / 700)), 2, true);
        }
        // board
        c.fill(14, TOP, BOARD, BOARD, 0xFFB8ACD6);
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) { Rect r = cell_rect(x, y); c.fill(r.x, r.y, r.w, r.h, 0xFFCEC4E6); }
        int p = (int)min<u64>(256, (t - anim_t0) * 256 / 120);
        int e = ease_out(p);
        // sliding / dying tiles first, then the rest on top
        for (int pass = 0; pass < 2; pass++)
            for (auto& tl : tiles) {
                if ((pass == 0) != tl.dying) continue;
                if (tl.dying && p >= 256) continue;
                if (tl.fresh && p < 256) {
                    // grows in after the slide
                    int q = (int)min<u64>(256, (t - anim_t0) * 256 / 220);
                    if (q < 140) continue;
                    draw_tile(c, cell_rect(tl.x, tl.y), tl.v, ease_out((q - 140) * 256 / 116));
                    continue;
                }
                Rect a = cell_rect(tl.fx, tl.fy), b = cell_rect(tl.x, tl.y);
                Rect r = lerp_rect(a, b, e);
                int sc = 256;
                if (tl.merged && p >= 256) {
                    int q = (int)min<u64>(256, (t - anim_t0 - 120) * 256 / 100);
                    sc = 256 + (q < 128 ? q : 256 - q) * 40 / 128;        // pop
                }
                draw_tile(c, r, tl.merged && p < 256 ? tl.v / 2 : tl.v, sc);
            }
        c.text(14, TOP + BOARD + 14, "arrows / wasd to slide  " G_BULLET "  u to undo", 0xFF8A7EA8);
        if (over || (won && !keep_going)) {
            c.fill_alpha(14, TOP, BOARD, BOARD, over ? 0xFFF4F0FA : 0xFFFFD27A, 190);
            c.text_centered(14, TOP + BOARD / 2 - 40, BOARD, over ? "no moves left" : "2048!", 0xFF4A3E6A, 3, true);
            c.text_centered(14, TOP + BOARD / 2 + 4, BOARD, over ? "space: new game" : "space: keep going", 0xFF6A5E8A, 1, true);
        }
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.key == ' ' || e.key == K_ENTER) {
            if (over) reset();
            else if (won && !keep_going) keep_going = true;
            ui_invalidate();
            return;
        }
        if (e.key == 'u') { undo(); return; }
        if (animating()) settle();
        if (e.key == K_UP || e.key == 'w') slide(0);
        else if (e.key == K_RIGHT || e.key == 'd') slide(1);
        else if (e.key == K_DOWN || e.key == 's') slide(2);
        else if (e.key == K_LEFT || e.key == 'a') slide(3);
    }
    int sx0 = 0, sy0 = 0;
    void on_mouse(const MouseEv& e) override {
        // drag to swipe
        if (e.kind == M_DOWN) { sx0 = e.x; sy0 = e.y; }
        if (e.kind == M_UP) {
            int dx = e.x - sx0, dy = e.y - sy0;
            if (over && iabs(dx) < 8 && iabs(dy) < 8) { reset(); return; }
            if (iabs(dx) > 30 || iabs(dy) > 30) {
                if (animating()) settle();
                if (iabs(dx) > iabs(dy)) slide(dx > 0 ? 1 : 3);
                else slide(dy > 0 ? 2 : 0);
            }
        }
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "Game", 24);
        m[0].add("New game", 1).add("Undo", 2, "u", can_undo);
        return 1;
    }
    void on_command(int cmd) override { if (cmd == 1) reset(); else undo(); }
};

App* new_g2048(const char*) { return new G2048App(); }
