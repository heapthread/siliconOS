// silicon - puzzle games: Connect Four, Memory, Wordy, Sudoku
#include "gamekit.h"

// ================================================================ Connect Four
struct Connect4App : GameApp {
    static const int CELL = 64, COLS = 7, ROWS = 6, FT = 34, PAD = 10;
    int b[ROWS][COLS];                 // 0 empty, 1 player (red), 2 silicon (yellow)
    int winner = 0, hover_col = 3, last_r = -1, last_c = -1;
    bool win_cells[ROWS][COLS];
    u64 ai_at = 0;
    bool player_turn = true;

    Connect4App() { gid = "connect4"; load_best(); started = true; board_reset(); }
    const char* app_id() override { return "connect4"; }
    const char* title() override { return "Connect Four"; }
    int pref_w() override { return COLS * CELL + PAD * 2; }
    int pref_h() override { return FT + ROWS * CELL + 56 + PAD; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF1A2030; }

    void board_reset() {
        memset(b, 0, sizeof(b));
        memset(win_cells, 0, sizeof(win_cells));
        winner = 0; over = false; player_turn = true; last_r = last_c = -1;
        ui_invalidate();
    }
    void reset_game() override { score = 0; board_reset(); }
    int drop_row(int c) { for (int r = ROWS - 1; r >= 0; r--) if (!b[r][c]) return r; return -1; }
    bool four(int bd[ROWS][COLS], int who, bool mark) {
        static const int dx[4] = {1, 0, 1, 1}, dy[4] = {0, 1, 1, -1};
        bool any = false;
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++)
                for (int d = 0; d < 4; d++) {
                    int e_r = r + dy[d] * 3, e_c = c + dx[d] * 3;
                    if (e_r < 0 || e_r >= ROWS || e_c >= COLS) continue;
                    int k = 0;
                    for (; k < 4; k++) if (bd[r + dy[d] * k][c + dx[d] * k] != who) break;
                    if (k == 4) { any = true; if (mark) for (int j = 0; j < 4; j++) win_cells[r + dy[d] * j][c + dx[d] * j] = true; else return true; }
                }
        return any;
    }
    int eval(int bd[ROWS][COLS]) {
        static const int dx[4] = {1, 0, 1, 1}, dy[4] = {0, 1, 1, -1};
        int s = 0;
        for (int r = 0; r < ROWS; r++) s += (bd[r][3] == 2) * 3 - (bd[r][3] == 1) * 3;
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++)
                for (int d = 0; d < 4; d++) {
                    int e_r = r + dy[d] * 3, e_c = c + dx[d] * 3;
                    if (e_r < 0 || e_r >= ROWS || e_c >= COLS) continue;
                    int me = 0, you = 0;
                    for (int k = 0; k < 4; k++) { int v = bd[r + dy[d] * k][c + dx[d] * k]; me += v == 2; you += v == 1; }
                    if (me && you) continue;
                    if (me == 3) s += 5; else if (me == 2) s += 2;
                    if (you == 3) s -= 6; else if (you == 2) s -= 2;
                }
        return s;
    }
    // negamax from the point of view of `who` (2 = silicon)
    int search(int bd[ROWS][COLS], int who, int depth, int alpha, int beta) {
        if (four(bd, 3 - who, false)) return -100000 - depth;
        if (depth == 0) return who == 2 ? eval(bd) : -eval(bd);
        static const int order[7] = {3, 2, 4, 1, 5, 0, 6};
        bool any = false;
        int best = -1000000;
        for (int oi = 0; oi < 7; oi++) {
            int c = order[oi], r = -1;
            for (int rr = ROWS - 1; rr >= 0; rr--) if (!bd[rr][c]) { r = rr; break; }
            if (r < 0) continue;
            any = true;
            bd[r][c] = who;
            int v = -search(bd, 3 - who, depth - 1, -beta, -alpha);
            bd[r][c] = 0;
            if (v > best) best = v;
            if (best > alpha) alpha = best;
            if (alpha >= beta) break;
        }
        return any ? best : 0;
    }
    void place(int c, int who) {
        int r = drop_row(c);
        if (r < 0) return;
        b[r][c] = who; last_r = r; last_c = c;
        ui_beep(0);
        if (four(b, who, true)) {
            winner = who; over = true;
            if (who == 1) { score++; if (score > best) { best = score; game_save_best(gid, score); } ui_beep(2); }
            else ui_beep(1);
        } else {
            bool full = true;
            for (int cc = 0; cc < COLS; cc++) if (!b[0][cc]) full = false;
            if (full) { over = true; winner = 0; }
        }
        ui_invalidate();
    }
    void ai_move() {
        int best_c = -1, best_v = -2000000;
        static const int order[7] = {3, 2, 4, 1, 5, 0, 6};
        for (int oi = 0; oi < 7; oi++) {
            int c = order[oi], r = drop_row(c);
            if (r < 0) continue;
            b[r][c] = 2;
            int v = -search(b, 1, 6, -1000000, 1000000);
            b[r][c] = 0;
            if (v > best_v) { best_v = v; best_c = c; }
        }
        if (best_c >= 0) place(best_c, 2);
        player_turn = true;
    }
    void tick() override {
        if (!over && !player_turn && ticks() >= ai_at) ai_move();
    }
    int col_at(int x) { return clamp((x - PAD) / CELL, 0, COLS - 1); }
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_MOVE) { int c = col_at(e.x); if (c != hover_col) { hover_col = c; ui_invalidate(); } }
        if (e.kind == M_UP && e.button == 1) {
            if (over) { board_reset(); return; }
            if (!player_turn || drop_row(col_at(e.x)) < 0) return;
            place(col_at(e.x), 1);
            if (!over) { player_turn = false; ai_at = ticks() + 250; }
        }
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.key == K_LEFT) hover_col = max(0, hover_col - 1);
        if (e.key == K_RIGHT) hover_col = min(COLS - 1, hover_col + 1);
        if (e.key == ' ' || e.key == K_ENTER || e.key == K_DOWN) {
            if (over) { board_reset(); return; }
            if (player_turn && drop_row(hover_col) >= 0) { place(hover_col, 1); if (!over) { player_turn = false; ai_at = ticks() + 250; } }
        }
        if (e.key >= '1' && e.key <= '7') {
            hover_col = e.key - '1';
            if (!over && player_turn && drop_row(hover_col) >= 0) { place(hover_col, 1); if (!over) { player_turn = false; ai_at = ticks() + 250; } }
        }
        ui_invalidate();
    }
    void draw(Canvas& c, int w, int h) override {
        bar(c, w, 0xFF121828, 0xFF2E3A58, 0xFFFFD0D8, 0xFF7A88B0, "wins");
        int bx = PAD, by = FT + 50;
        if (!over && player_turn) c.circle(bx + hover_col * CELL + CELL / 2, FT + 28, 20, 0xFFE5486A, true);
        c.fill(bx - 4, by - 4, COLS * CELL + 8, ROWS * CELL + 8, 0xFF2F4DB0);
        for (int r = 0; r < ROWS; r++)
            for (int cc = 0; cc < COLS; cc++) {
                int cx = bx + cc * CELL + CELL / 2, cy = by + r * CELL + CELL / 2;
                u32 col = b[r][cc] == 1 ? 0xFFE5486A : b[r][cc] == 2 ? 0xFFECBC5C : 0xFF141C38;
                c.circle(cx, cy, 26, col, true);
                if (b[r][cc]) c.circle(cx, cy, 18, blend(col, th::white, 40), false);
                if (win_cells[r][cc]) c.circle(cx, cy, 29, th::white, false);
            }
        const char* msg = over ? (winner == 1 ? "you win!  click to play again" : winner == 2 ? "silicon wins.  click to play again" : "a draw.  click to play again")
                               : player_turn ? "your move" : "silicon is thinking...";
        c.text_centered(0, FT + 50 + ROWS * CELL + 18, w, msg, 0xFFC8D0F0, 1, true);
    }
};

// ================================================================ Memory
struct MemoryApp : GameApp {
    static const int N = 4, CELL = 84, GAP = 8, FT = 34;
    int card[N * N];         // pair id 0..7
    int state[N * N];        // 0 down, 1 up, 2 matched
    int first = -1, second = -1, moves = 0, pairs = 0;
    u64 hide_at = 0, t0 = 0;
    bool won = false;

    MemoryApp() { gid = "memory"; load_best(); reset_game(); }
    const char* app_id() override { return "memory"; }
    const char* title() override { return "Memory"; }
    int pref_w() override { return N * (CELL + GAP) + GAP; }
    int pref_h() override { return FT + N * (CELL + GAP) + GAP; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF231C38; }

    void reset_game() override {
        for (int i = 0; i < N * N; i++) { card[i] = i / 2; state[i] = 0; }
        for (int i = N * N - 1; i > 0; i--) { int j = rand() % (i + 1); swap(card[i], card[j]); }
        first = second = -1; moves = pairs = 0; won = false; over = false; started = true; t0 = ticks();
        ui_invalidate();
    }
    Rect cell(int i) { return {GAP + (i % N) * (CELL + GAP), FT + GAP + (i / N) * (CELL + GAP), CELL, CELL}; }
    void tick() override {
        if (second >= 0 && ticks() >= hide_at) { state[first] = state[second] = 0; first = second = -1; ui_invalidate(); }
        if (!won) { static u64 lt; if (ticks() - lt > 500) { lt = ticks(); ui_invalidate(); } }
    }
    void flip(int i) {
        if (won || state[i] != 0 || second >= 0) return;
        state[i] = 1;
        ui_beep(0);
        if (first < 0) { first = i; }
        else {
            second = i; moves++;
            if (card[first] == card[second]) {
                state[first] = state[second] = 2; first = second = -1; pairs++;
                ui_beep(2);
                if (pairs == N * N / 2) {
                    won = true;
                    if (!best || moves < best) { best = moves; game_save_best(gid, moves); ui_notify("Memory", "new best!", gid); }
                }
            } else hide_at = ticks() + 800;
        }
        ui_invalidate();
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind != M_UP || e.button != 1) return;
        if (won) { reset_game(); return; }
        for (int i = 0; i < N * N; i++) if (cell(i).contains(e.x, e.y)) flip(i);
    }
    void on_key(const KeyEvent& e) override { if (e.down && won && (e.key == ' ' || e.key == K_ENTER)) reset_game(); }
    void shape(Canvas& c, int id, int cx, int cy) {
        static const u32 cols[8] = {0xFFE5486A, 0xFFECBC5C, 0xFF3FB27F, 0xFF4FD1C5, 0xFF9B7BFF, 0xFFFF8CA8, 0xFF6AA8FF, 0xFFFFA050};
        u32 col = cols[id];
        switch (id) {
        case 0: c.circle(cx, cy, 22, col, true); break;
        case 1: c.fill(cx - 20, cy - 20, 40, 40, col); break;
        case 2: for (int k = -22; k <= 22; k++) { int hw = (22 - iabs(k)); c.hline(cx - hw, cy + k, hw * 2 + 1, col); } break;
        case 3: for (int k = 0; k < 40; k++) c.hline(cx - k / 2, cy - 20 + k, k + 1, col); break;
        case 4: c.fill(cx - 8, cy - 22, 16, 44, col); c.fill(cx - 22, cy - 8, 44, 16, col); break;
        case 5: c.circle(cx, cy, 22, col, false); c.circle(cx, cy, 21, col, false); c.circle(cx, cy, 14, col, false); c.circle(cx, cy, 13, col, false); break;
        case 6: c.fill(cx - 22, cy - 10, 44, 20, col); break;
        case 7: for (int k = 0; k < 40; k++) c.hline(cx - (39 - k) / 2, cy - 20 + k, 40 - k, col); break;
        }
    }
    void draw(Canvas& c, int w, int h) override {
        score = moves;
        c.fill(0, 0, w, FT, 0xFF160F26);
        c.hline(0, FT - 1, w, 0xFF3A2F5C);
        char b[48];
        snprintf(b, sizeof(b), "moves %d", moves);
        c.text(10, 9, b, 0xFFE0D0FF, 2, true);
        int secs = (int)((won ? 0 : (ticks() - t0)) / 1000);
        if (!won) { snprintf(b, sizeof(b), "%d:%02d", secs / 60, secs % 60); c.text(w - 60, 13, b, 0xFF8A7AB8); }
        for (int i = 0; i < N * N; i++) {
            Rect r = cell(i);
            if (state[i] == 0) {
                bool on = hot(r.x, r.y, r.w, r.h);
                c.fill(r.x, r.y, r.w, r.h, on ? 0xFF5A4A98 : 0xFF45387A);
                c.rect(r.x, r.y, r.w, r.h, 0xFF7A6AB8);
                c.text_centered(r.x, r.y + r.h / 2 - 12, r.w, "?", 0xFF9A8AD8, 3, true);
            } else {
                c.fill(r.x, r.y, r.w, r.h, state[i] == 2 ? 0xFF2E5A48 : 0xFFEEEAF6);
                shape(c, card[i], r.x + r.w / 2, r.y + r.h / 2);
            }
        }
        if (won) {
            c.fill_alpha(0, FT, w, h - FT, 0xFF000000, 140);
            c.text_centered(0, h / 2 - 24, w, "YOU WIN", th::white, 4, true);
            snprintf(b, sizeof(b), "%d moves (best %d)  click to play again", moves, best);
            c.text_centered(0, h / 2 + 22, w, b, 0xFFD8D0F0);
        }
    }
    int menus(Menu* m) override { m[0].clear(); strlcpy(m[0].title, "Game", 24); m[0].add("New game", 1); return 1; }
    void on_command(int) override { reset_game(); }
};

// ================================================================ Wordy
static const char* const WORDS[] = {
    "about", "above", "actor", "adapt", "alarm", "album", "alert", "alien", "align", "alloy", "alone", "angle", "apple", "arena",
    "arrow", "audio", "badge", "baker", "beach", "begin", "black", "blade", "blank", "blast", "blend", "board", "brain", "brave",
    "bread", "brick", "brief", "bring", "broad", "brown", "build", "cable", "candy", "carry", "chain", "chair", "chalk", "charm",
    "chart", "chase", "cheap", "check", "chess", "chief", "child", "chip", "clean", "clear", "climb", "clock", "close", "cloud",
    "coach", "coast", "color", "comet", "count", "craft", "crane", "crash", "crowd", "crown", "cycle", "daily", "dance", "delta",
    "depth", "dirty", "dream", "dress", "drink", "drive", "eagle", "early", "earth", "eight", "email", "empty", "enjoy", "enter",
    "equal", "error", "event", "every", "extra", "faint", "fancy", "field", "final", "flame", "flash", "fleet", "float", "floor",
    "focus", "force", "forth", "frame", "fresh", "front", "fruit", "ghost", "giant", "glass", "globe", "grace", "grade", "grain",
    "grand", "grape", "grass", "great", "green", "group", "guard", "guess", "guide", "happy", "heart", "heavy", "hello", "honey",
    "horse", "hotel", "house", "human", "ideal", "image", "index", "inner", "input", "jelly", "joint", "judge", "juice", "kebab",
    "knife", "laser", "later", "laugh", "layer", "learn", "least", "leave", "level", "light", "limit", "linux", "logic", "loose",
    "lucky", "lunch", "magic", "major", "maker", "march", "match", "maybe", "media", "metal", "meter", "might", "minor", "model",
    "money", "month", "motor", "mouse", "mouth", "movie", "music", "night", "noise", "north", "novel", "nurse", "ocean", "olive",
    "orbit", "order", "other", "paint", "panel", "paper", "party", "pasta", "pause", "peace", "phase", "phone", "photo", "piano",
    "pilot", "pixel", "pizza", "place", "plain", "plane", "plant", "plate", "point", "power", "press", "price", "pride", "prime",
    "print", "prize", "proof", "proud", "pulse", "queen", "quick", "quiet", "radio", "raise", "range", "rapid", "ratio", "reach",
    "ready", "reply", "right", "river", "robot", "rough", "round", "route", "royal", "rural", "scale", "scene", "scope", "score",
    "sense", "serve", "seven", "shade", "shape", "share", "sharp", "sheep", "sheet", "shell", "shift", "shine", "shirt", "shock",
    "short", "sight", "silly", "since", "skill", "sleep", "slice", "small", "smart", "smile", "smoke", "snake", "solid", "solve",
    "sound", "south", "space", "spare", "speak", "speed", "spell", "spend", "spice", "split", "sport", "squad", "stack", "staff",
    "stage", "stair", "stamp", "stand", "start", "state", "steam", "steel", "stick", "still", "stone", "store", "storm", "story",
    "sugar", "suite", "sunny", "super", "sweet", "swift", "table", "taste", "teach", "thank", "theme", "there", "thick", "thing",
    "think", "three", "throw", "tiger", "title", "toast", "today", "token", "tooth", "topic", "total", "touch", "tower", "trace",
    "track", "trade", "train", "treat", "trend", "trial", "trick", "truck", "trust", "truth", "twice", "uncle", "under", "union",
    "unity", "until", "upper", "urban", "usage", "usual", "valid", "value", "video", "virus", "visit", "vital", "voice", "waste",
    "watch", "water", "wheel", "where", "which", "while", "white", "whole", "woman", "world", "worry", "worth", "write", "wrong",
    "yield", "young", "youth", "zebra",
};
static const int NWORDS = sizeof(WORDS) / sizeof(WORDS[0]);

struct WordyApp : GameApp {
    static const int CELL = 52, GAP = 6, FT = 34;
    char answer[6];
    char guesses[6][6];
    int nguess = 0, len = 0;
    char cur[6] = "";
    bool won = false;
    int kstate[26];             // 0 unknown, 1 absent, 2 present, 3 correct
    char msg[48] = "";
    u64 msg_t = 0;

    WordyApp() { gid = "wordy"; load_best(); reset_game(); }
    const char* app_id() override { return "wordy"; }
    const char* title() override { return "Wordy"; }
    int pref_w() override { return 5 * (CELL + GAP) + GAP + 60; }
    int pref_h() override { return FT + 6 * (CELL + GAP) + 130; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF1C1A2C; }

    void reset_game() override {
        strlcpy(answer, WORDS[rand() % NWORDS], 6);
        nguess = len = 0; cur[0] = 0; won = over = false; started = true;
        memset(kstate, 0, sizeof(kstate));
        msg[0] = 0;
        ui_invalidate();
    }
    // result per letter: 3 correct, 2 present, 1 absent
    void grade(const char* g, int* res) {
        int used[5] = {0, 0, 0, 0, 0};
        for (int i = 0; i < 5; i++) res[i] = 1;
        for (int i = 0; i < 5; i++) if (g[i] == answer[i]) { res[i] = 3; used[i] = 1; }
        for (int i = 0; i < 5; i++) {
            if (res[i] == 3) continue;
            for (int j = 0; j < 5; j++) if (!used[j] && g[i] == answer[j]) { res[i] = 2; used[j] = 1; break; }
        }
    }
    void submit() {
        if (len < 5) { snprintf(msg, sizeof(msg), "not enough letters"); msg_t = ticks(); return; }
        strlcpy(guesses[nguess], cur, 6);
        int res[5];
        grade(cur, res);
        for (int i = 0; i < 5; i++) { int k = cur[i] - 'a'; if (res[i] > kstate[k]) kstate[k] = res[i]; }
        nguess++;
        if (!strcmp(cur, answer)) {
            won = over = true; score++;
            if (score > best) { best = score; game_save_best(gid, score); }
            ui_beep(2);
        } else if (nguess == 6) { over = true; score = 0; ui_beep(1); }
        len = 0; cur[0] = 0;
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (over) { if (e.key == ' ' || e.key == K_ENTER) reset_game(); return; }
        if (e.key >= 'a' && e.key <= 'z' && len < 5) { cur[len++] = (char)e.key; cur[len] = 0; }
        else if (e.key >= 'A' && e.key <= 'Z' && len < 5) { cur[len++] = (char)(e.key + 32); cur[len] = 0; }
        else if (e.key == K_BACKSPACE && len > 0) cur[--len] = 0;
        else if (e.key == K_ENTER) submit();
        ui_invalidate();
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_UP && over) reset_game();
    }
    void tick() override { if (msg[0] && ticks() - msg_t > 1500) { msg[0] = 0; ui_invalidate(); } }
    void draw(Canvas& c, int w, int h) override {
        bar(c, w, 0xFF121022, 0xFF343058, 0xFFE0D8FF, 0xFF8A80B8, "streak");
        int gx = (w - (5 * (CELL + GAP) - GAP)) / 2;
        for (int r = 0; r < 6; r++)
            for (int i = 0; i < 5; i++) {
                int x = gx + i * (CELL + GAP), y = FT + 10 + r * (CELL + GAP);
                char ch = 0;
                u32 bg = 0xFF28243E, fg = th::white;
                if (r < nguess) {
                    int res[5];
                    grade(guesses[r], res);
                    ch = guesses[r][i];
                    bg = res[i] == 3 ? 0xFF3FB27F : res[i] == 2 ? 0xFFD8A83A : 0xFF4A4468;
                } else if (r == nguess && i < len) { ch = cur[i]; bg = 0xFF3A3560; }
                c.fill(x, y, CELL, CELL, bg);
                if (r >= nguess) c.rect(x, y, CELL, CELL, r == nguess && i < len ? 0xFF8A80C8 : 0xFF45406A);
                if (ch) { char s[2] = {(char)(ch - 32), 0}; c.text(x + CELL / 2 - 12, y + CELL / 2 - 12, s, fg, 3, true); }
            }
        // keyboard
        static const char* rows[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
        for (int r = 0; r < 3; r++) {
            int n = (int)strlen(rows[r]);
            int kx = (w - n * 26) / 2, ky = FT + 10 + 6 * (CELL + GAP) + 6 + r * 28;
            for (int i = 0; i < n; i++) {
                int st = kstate[rows[r][i] - 'a'];
                u32 bg = st == 3 ? 0xFF3FB27F : st == 2 ? 0xFFD8A83A : st == 1 ? 0xFF2C2842 : 0xFF4A4468;
                c.fill(kx + i * 26, ky, 24, 24, bg);
                char s[2] = {(char)(rows[r][i] - 32), 0};
                c.text(kx + i * 26 + 8, ky + 6, s, st == 1 ? 0xFF7A7298 : th::white, 1, true);
            }
        }
        char b[40];
        snprintf(b, sizeof(b), "%d", score);
        c.text(w - 60, 13, b, 0xFF8A80B8);
        if (msg[0]) c.text_centered(0, h - 20, w, msg, 0xFFFFC0C8);
        if (over) {
            c.fill_alpha(0, FT + 90, w, 110, 0xFF000000, 190);
            if (won) c.text_centered(0, FT + 110, w, "SOLVED!", 0xFF7FE0A8, 3, true);
            else { char a[24]; snprintf(a, sizeof(a), "it was %s", answer); c.text_centered(0, FT + 108, w, a, 0xFFFFC0C8, 2, true); }
            c.text_centered(0, FT + 165, w, "space for a new word", 0xFFD8D0F0);
        }
    }
    int menus(Menu* m) override { m[0].clear(); strlcpy(m[0].title, "Game", 24); m[0].add("New word", 1); return 1; }
    void on_command(int) override { score = 0; reset_game(); }
};

// ================================================================ Sudoku
struct SudokuApp : App {
    static const int CELL = 44, FT = 34, OFF = 12;
    int sol[81], grid[81];
    bool given[81];
    int sel = 40, errors_on = 1;
    u64 t0 = 0, done_t = 0;
    bool solved = false;
    int diff = 1;       // 0 easy, 1 medium, 2 hard

    SudokuApp() { new_puzzle(); }
    const char* app_id() override { return "sudoku"; }
    const char* title() override { return "Sudoku"; }
    int pref_w() override { return OFF * 2 + CELL * 9; }
    int pref_h() override { return FT + OFF * 2 + CELL * 9 + 28; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFFEEEAF6; }

    bool ok_at(const int* g, int p, int v) {
        int r = p / 9, c = p % 9;
        for (int i = 0; i < 9; i++) {
            if (i != c && g[r * 9 + i] == v) return false;
            if (i != r && g[i * 9 + c] == v) return false;
        }
        int br = r / 3 * 3, bc = c / 3 * 3;
        for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++) { int q = (br + y) * 9 + bc + x; if (q != p && g[q] == v) return false; }
        return true;
    }
    bool fill(int* g, int p) {
        if (p == 81) return true;
        int order[9];
        for (int i = 0; i < 9; i++) order[i] = i + 1;
        for (int i = 8; i > 0; i--) { int j = rand() % (i + 1); swap(order[i], order[j]); }
        for (int k = 0; k < 9; k++) if (ok_at(g, p, order[k])) { g[p] = order[k]; if (fill(g, p + 1)) return true; g[p] = 0; }
        return false;
    }
    int nodes = 0;
    int count_solutions(int* g, int limit) {
        if (++nodes > 30000) return limit;
        int p = -1, bestn = 10;
        for (int i = 0; i < 81; i++) if (!g[i]) {
            int n = 0;
            for (int v = 1; v <= 9; v++) n += ok_at(g, i, v);
            if (n < bestn) { bestn = n; p = i; if (n <= 1) break; }
        }
        if (p < 0) return 1;
        int total = 0;
        for (int v = 1; v <= 9 && total < limit; v++) if (ok_at(g, p, v)) { g[p] = v; total += count_solutions(g, limit - total); g[p] = 0; }
        return total;
    }
    void new_puzzle() {
        memset(sol, 0, sizeof(sol));
        fill(sol, 0);
        memcpy(grid, sol, sizeof(grid));
        int remove = diff == 0 ? 36 : diff == 1 ? 46 : 54;
        int order[81];
        for (int i = 0; i < 81; i++) order[i] = i;
        for (int i = 80; i > 0; i--) { int j = rand() % (i + 1); swap(order[i], order[j]); }
        int removed = 0;
        for (int k = 0; k < 81 && removed < remove; k++) {
            int p = order[k], keep = grid[p];
            grid[p] = 0;
            int tmp[81];
            memcpy(tmp, grid, sizeof(tmp));
            nodes = 0;
            if (count_solutions(tmp, 2) != 1) grid[p] = keep;
            else removed++;
        }
        for (int i = 0; i < 81; i++) given[i] = grid[i] != 0;
        solved = false; t0 = ticks(); sel = 40;
        ui_invalidate();
    }
    bool check_solved() {
        for (int i = 0; i < 81; i++) if (!grid[i] || !ok_at(grid, i, grid[i])) return false;
        return true;
    }
    void tick() override {
        static u64 lt;
        if (!solved && ticks() - lt > 1000) { lt = ticks(); ui_invalidate(); }
    }
    void set_cell(int v) {
        if (solved || given[sel]) return;
        grid[sel] = v;
        if (v && check_solved()) { solved = true; done_t = ticks(); ui_beep(2); }
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        int r = sel / 9, c = sel % 9;
        if (e.key == K_UP && r > 0) sel -= 9;
        else if (e.key == K_DOWN && r < 8) sel += 9;
        else if (e.key == K_LEFT && c > 0) sel--;
        else if (e.key == K_RIGHT && c < 8) sel++;
        else if (e.key >= '1' && e.key <= '9') set_cell(e.key - '0');
        else if (e.key == '0' || e.key == K_BACKSPACE || e.key == K_DELETE) set_cell(0);
        else if (e.key == 'h' && !given[sel] && !solved) set_cell(sol[sel]);
        ui_invalidate();
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind != M_DOWN || e.button != 1) return;
        int c = (e.x - OFF) / CELL, r = (e.y - FT - OFF) / CELL;
        if (e.x >= OFF && e.y >= FT + OFF && c >= 0 && c < 9 && r >= 0 && r < 9) { sel = r * 9 + c; ui_invalidate(); }
    }
    void draw(Canvas& c, int w, int h) override {
        c.fill(0, 0, w, FT, th::panel);
        c.hline(0, FT - 1, w, th::border);
        u64 el = (solved ? done_t : ticks()) - t0;
        char b[64];
        int secs = (int)(el / 1000);
        snprintf(b, sizeof(b), "%s  %d:%02d", diff == 0 ? "easy" : diff == 1 ? "medium" : "hard", secs / 60, secs % 60);
        c.text(12, 12, b, th::text, 1, true);
        c.text(w - 150, 12, "1-9 fill, h hint", th::muted);
        int bx = OFF, by = FT + OFF;
        int selv = grid[sel];
        for (int i = 0; i < 81; i++) {
            int x = bx + (i % 9) * CELL, y = by + (i / 9) * CELL;
            u32 bg = th::white;
            if (i == sel) bg = blend(th::white, accent(), 90);
            else if (selv && grid[i] == selv) bg = blend(th::white, accent(), 45);
            else if (i / 9 == sel / 9 || i % 9 == sel % 9 || (i / 27 == sel / 27 && (i % 9) / 3 == (sel % 9) / 3)) bg = 0xFFF0ECFA;
            c.fill(x, y, CELL, CELL, bg);
            if (grid[i]) {
                bool bad = errors_on && !ok_at(grid, i, grid[i]);
                char s[2] = {(char)('0' + grid[i]), 0};
                c.text(x + CELL / 2 - 8, y + CELL / 2 - 12, s, bad ? th::red : given[i] ? th::ink : accent(), 3, given[i]);
            }
        }
        for (int i = 0; i <= 9; i++) {
            u32 col = i % 3 == 0 ? th::ink : th::border;
            int t = i % 3 == 0 ? 2 : 1;
            c.fill(bx + i * CELL - t / 2, by, t, 9 * CELL, col);
            c.fill(bx, by + i * CELL - t / 2, 9 * CELL, t, col);
        }
        if (solved) {
            c.fill_alpha(0, FT, w, h - FT, 0xFF000000, 120);
            c.text_centered(0, h / 2 - 20, w, "SOLVED!", th::white, 4, true);
            c.text_centered(0, h / 2 + 24, w, "Game > New puzzle for another", 0xFFE0D8FF);
        }
        c.text(OFF, FT + OFF * 2 + 9 * CELL + 6, errors_on ? "conflicts shown in red" : "conflicts hidden", th::muted);
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "Game", 24);
        m[0].add("New puzzle", 1).sep().add("Easy", 2, "", true, diff == 0).add("Medium", 3, "", true, diff == 1).add("Hard", 4, "", true, diff == 2)
            .sep().add("Show conflicts", 5, "", true, errors_on != 0).add("Hint", 6, "h");
        return 1;
    }
    void on_command(int cmd) override {
        if (cmd == 1) new_puzzle();
        if (cmd >= 2 && cmd <= 4) { diff = cmd - 2; new_puzzle(); }
        if (cmd == 5) errors_on ^= 1;
        if (cmd == 6 && !given[sel] && !solved) set_cell(sol[sel]);
        ui_invalidate();
    }
};

App* new_connect4(const char*) { return new Connect4App(); }
App* new_memory(const char*) { return new MemoryApp(); }
App* new_wordy(const char*) { return new WordyApp(); }
App* new_sudoku(const char*) { return new SudokuApp(); }
