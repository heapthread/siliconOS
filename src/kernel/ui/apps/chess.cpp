// silicon - Chess against a small alpha-beta engine (you play white). Castling, en passant, promotion (to queen).
#include "apps.h"

struct ChessApp : App {
    static const int CELL = 56, OFF = 12, FT = 0;
    struct Pos {
        char b[64];                 // 'PNBRQK' white, lowercase black, 0 empty; index 0 = a8
        bool cwk, cwq, cbk, cbq;    // castling rights
        int ep;                     // en passant target square or -1
        bool white;                 // side to move
    };
    struct Move { u8 from, to; };
    Pos pos;
    Vec<Pos> history;
    int sel = -1, last_from = -1, last_to = -1;
    bool over = false, thinking = false;
    char msg[64] = "your move";
    u64 think_at = 0;

    ChessApp() { reset(); }
    ~ChessApp() { history.free(); }
    const char* app_id() override { return "chess"; }
    const char* title() override { return "Chess"; }
    int pref_w() override { return OFF * 2 + CELL * 8 + 150; }
    int pref_h() override { return OFF * 2 + CELL * 8; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF2A2340; }

    void reset() {
        static const char* init = "rnbqkbnrpppppppp                                PPPPPPPPRNBQKBNR";
        for (int i = 0; i < 64; i++) pos.b[i] = init[i] == ' ' ? 0 : init[i];
        pos.cwk = pos.cwq = pos.cbk = pos.cbq = true;
        pos.ep = -1; pos.white = true;
        history.clear();
        sel = last_from = last_to = -1; over = thinking = false;
        strlcpy(msg, "your move", sizeof(msg));
        ui_invalidate();
    }
    static bool is_white(char p) { return p >= 'A' && p <= 'Z'; }
    static char lower(char p) { return p >= 'A' && p <= 'Z' ? p + 32 : p; }

    bool attacked(const Pos& p, int sq, bool by_white) {
        int x = sq % 8, y = sq / 8;
        // pawns
        int py = by_white ? y + 1 : y - 1;
        if (py >= 0 && py < 8) for (int dx = -1; dx <= 1; dx += 2) {
            int px = x + dx;
            if (px >= 0 && px < 8 && p.b[py * 8 + px] == (by_white ? 'P' : 'p')) return true;
        }
        static const int kn[8][2] = {{1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}};
        for (auto& k : kn) {
            int nx = x + k[0], ny = y + k[1];
            if (nx >= 0 && nx < 8 && ny >= 0 && ny < 8 && p.b[ny * 8 + nx] == (by_white ? 'N' : 'n')) return true;
        }
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && nx < 8 && ny >= 0 && ny < 8 && p.b[ny * 8 + nx] == (by_white ? 'K' : 'k')) return true;
            bool diag = dx && dy;
            int cx = x + dx, cy = y + dy;
            while (cx >= 0 && cx < 8 && cy >= 0 && cy < 8) {
                char q = p.b[cy * 8 + cx];
                if (q) {
                    if (is_white(q) == by_white) {
                        char l = lower(q);
                        if (l == 'q' || (diag && l == 'b') || (!diag && l == 'r')) return true;
                    }
                    break;
                }
                cx += dx; cy += dy;
            }
        }
        return false;
    }
    int find_king(const Pos& p, bool white) { for (int i = 0; i < 64; i++) if (p.b[i] == (white ? 'K' : 'k')) return i; return -1; }
    bool in_check(const Pos& p, bool white) { int k = find_king(p, white); return k >= 0 && attacked(p, k, !white); }

    void apply(Pos& p, Move m) {
        char pc = p.b[m.from];
        char cap = p.b[m.to];
        int fx = m.from % 8, tx = m.to % 8;
        p.ep = -1;
        if (lower(pc) == 'p') {
            if (tx != fx && !cap) p.b[(m.from / 8) * 8 + tx] = 0;          // en passant capture
            if (iabs(m.to / 8 - m.from / 8) == 2) p.ep = (m.from + m.to) / 2;
        }
        if (lower(pc) == 'k' && iabs(tx - fx) == 2) {                       // castling: move the rook
            if (tx == 6) { p.b[m.from + 1] = p.b[m.from + 3]; p.b[m.from + 3] = 0; }
            else { p.b[m.from - 1] = p.b[m.from - 4]; p.b[m.from - 4] = 0; }
        }
        p.b[m.to] = pc; p.b[m.from] = 0;
        if (pc == 'P' && m.to < 8) p.b[m.to] = 'Q';
        if (pc == 'p' && m.to >= 56) p.b[m.to] = 'q';
        if (pc == 'K') p.cwk = p.cwq = false;
        if (pc == 'k') p.cbk = p.cbq = false;
        if (m.from == 56 || m.to == 56) p.cwq = false;
        if (m.from == 63 || m.to == 63) p.cwk = false;
        if (m.from == 0 || m.to == 0) p.cbq = false;
        if (m.from == 7 || m.to == 7) p.cbk = false;
        p.white = !p.white;
    }
    // pseudo-legal moves for the side to move
    int gen(const Pos& p, Move* out) {
        int n = 0;
        bool w = p.white;
        for (int sq = 0; sq < 64; sq++) {
            char pc = p.b[sq];
            if (!pc || is_white(pc) != w) continue;
            int x = sq % 8, y = sq / 8;
            char l = lower(pc);
            auto add = [&](int tx, int ty) { out[n++] = {(u8)sq, (u8)(ty * 8 + tx)}; };
            if (l == 'p') {
                int dy = w ? -1 : 1, sy = w ? 6 : 1;
                int ny = y + dy;
                if (ny < 0 || ny > 7) continue;
                if (!p.b[ny * 8 + x]) {
                    add(x, ny);
                    if (y == sy && !p.b[(ny + dy) * 8 + x]) add(x, ny + dy);
                }
                for (int dx = -1; dx <= 1; dx += 2) {
                    int nx = x + dx;
                    if (nx < 0 || nx > 7) continue;
                    char t = p.b[ny * 8 + nx];
                    if ((t && is_white(t) != w) || (!t && p.ep == ny * 8 + nx)) add(nx, ny);
                }
            } else if (l == 'n') {
                static const int kn[8][2] = {{1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}};
                for (auto& k : kn) {
                    int nx = x + k[0], ny = y + k[1];
                    if (nx < 0 || nx > 7 || ny < 0 || ny > 7) continue;
                    char t = p.b[ny * 8 + nx];
                    if (!t || is_white(t) != w) add(nx, ny);
                }
            } else {
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    bool diag = dx && dy;
                    if (l == 'b' && !diag) continue;
                    if (l == 'r' && diag) continue;
                    int cx = x + dx, cy = y + dy;
                    while (cx >= 0 && cx < 8 && cy >= 0 && cy < 8) {
                        char t = p.b[cy * 8 + cx];
                        if (!t) add(cx, cy);
                        else { if (is_white(t) != w) add(cx, cy); break; }
                        if (l == 'k') break;
                        cx += dx; cy += dy;
                    }
                }
                if (l == 'k') {
                    int home = w ? 60 : 4;
                    if (sq == home && !attacked(p, home, !w)) {
                        bool ks = w ? p.cwk : p.cbk, qs = w ? p.cwq : p.cbq;
                        if (ks && !p.b[home + 1] && !p.b[home + 2] && p.b[home + 3] == (w ? 'R' : 'r') &&
                            !attacked(p, home + 1, !w) && !attacked(p, home + 2, !w)) add(6, home / 8);
                        if (qs && !p.b[home - 1] && !p.b[home - 2] && !p.b[home - 3] && p.b[home - 4] == (w ? 'R' : 'r') &&
                            !attacked(p, home - 1, !w) && !attacked(p, home - 2, !w)) add(2, home / 8);
                    }
                }
            }
        }
        return n;
    }
    int legal(const Pos& p, Move* out) {
        Move all[256];
        int n = gen(p, all), k = 0;
        for (int i = 0; i < n; i++) {
            Pos q = p;
            apply(q, all[i]);
            if (!in_check(q, p.white)) out[k++] = all[i];
        }
        return k;
    }

    static int value(char pc) {
        switch (lower(pc)) { case 'p': return 100; case 'n': return 320; case 'b': return 330; case 'r': return 500; case 'q': return 900; case 'k': return 20000; }
        return 0;
    }
    int evaluate(const Pos& p) {       // from white's point of view
        int s = 0;
        for (int i = 0; i < 64; i++) {
            char pc = p.b[i];
            if (!pc) continue;
            int x = i % 8, y = i / 8;
            int v = value(pc);
            int cen = 3 - max(iabs(2 * x - 7), iabs(2 * y - 7)) / 2;   // 0..3
            char l = lower(pc);
            if (l == 'p') v += (is_white(pc) ? 6 - y : y - 1) * 8 + cen * 3;
            else if (l == 'n' || l == 'b') v += cen * 8;
            else if (l == 'q') v += cen * 2;
            else if (l == 'k') v -= cen * 6;
            s += is_white(pc) ? v : -v;
        }
        return s;
    }
    int nodes = 0;
    int negamax(const Pos& p, int depth, int alpha, int beta) {
        nodes++;
        if (depth == 0) { int e = evaluate(p); return p.white ? e : -e; }
        Move mv[256];
        int n = legal(p, mv);
        if (n == 0) return in_check(p, p.white) ? -100000 - depth : 0;
        // order: captures first
        int score[256];
        for (int i = 0; i < n; i++) score[i] = p.b[mv[i].to] ? value(p.b[mv[i].to]) * 10 - value(p.b[mv[i].from]) / 10 : 0;
        for (int i = 0; i < n; i++) {
            int bi = i;
            for (int j = i + 1; j < n; j++) if (score[j] > score[bi]) bi = j;
            swap(score[i], score[bi]); swap(mv[i], mv[bi]);
            Pos q = p;
            apply(q, mv[i]);
            int v = -negamax(q, depth - 1, -beta, -alpha);
            if (v >= beta) return v;
            if (v > alpha) alpha = v;
        }
        return alpha;
    }
    void ai_move() {
        Move mv[256];
        int n = legal(pos, mv);
        if (!n) return;
        int best_i = 0, best_v = -10000000;
        nodes = 0;
        int depth = 3;
        for (int i = 0; i < n; i++) {
            Pos q = pos;
            apply(q, mv[i]);
            int v = -negamax(q, depth - 1, -10000000, 10000000) + (int)(rand() % 6);
            if (v > best_v) { best_v = v; best_i = i; }
        }
        do_move(mv[best_i]);
    }
    void do_move(Move m) {
        history.push(pos);
        bool cap = pos.b[m.to] != 0;
        apply(pos, m);
        last_from = m.from; last_to = m.to;
        ui_beep(cap ? 1 : 0);
        Move mv[256];
        int n = legal(pos, mv);
        bool chk = in_check(pos, pos.white);
        if (n == 0) {
            over = true;
            if (chk) strlcpy(msg, pos.white ? "checkmate - silicon wins" : "checkmate - you win!", sizeof(msg));
            else strlcpy(msg, "stalemate - a draw", sizeof(msg));
            ui_beep(pos.white ? 1 : 2);
        } else strlcpy(msg, pos.white ? (chk ? "check! your move" : "your move") : (chk ? "check!" : "silicon is thinking..."), sizeof(msg));
        ui_invalidate();
    }
    void tick() override {
        if (thinking && ticks() >= think_at) { thinking = false; ai_move(); }
    }
    void on_mouse(const MouseEv& e) override {
        int sx = OFF * 2 + CELL * 8;
        if (e.kind == M_UP && e.button == 1) {
            if (Rect{sx, 50, 130, 24}.contains(e.x, e.y)) reset();
            else if (Rect{sx, 82, 130, 24}.contains(e.x, e.y)) on_command_undo();
            ui_invalidate();
            return;
        }
        if (e.kind != M_DOWN || e.button != 1 || over || thinking || !pos.white) return;
        int x = (e.x - OFF) / CELL, y = (e.y - OFF) / CELL;
        if (e.x < OFF || e.y < OFF || x < 0 || x > 7 || y < 0 || y > 7) { sel = -1; ui_invalidate(); return; }
        int sq = y * 8 + x;
        char pc = pos.b[sq];
        if (sel >= 0) {
            Move mv[256];
            int n = legal(pos, mv);
            for (int i = 0; i < n; i++)
                if (mv[i].from == sel && mv[i].to == sq) {
                    sel = -1;
                    do_move(mv[i]);
                    if (!over) { thinking = true; think_at = ticks() + 200; }
                    return;
                }
        }
        sel = pc && is_white(pc) ? sq : -1;
        ui_invalidate();
    }
    void draw(Canvas& c, int w, int h) override {
        Move mv[256];
        int n = 0;
        if (sel >= 0) n = legal(pos, mv);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int sq = y * 8 + x;
                bool dark = (x + y) % 2;
                u32 col = dark ? 0xFF7A6AA8 : 0xFFE0D8F0;
                if (sq == last_from || sq == last_to) col = blend(col, th::gold, 90);
                if (sq == sel) col = blend(col, 0xFF5AD08A, 140);
                int px = OFF + x * CELL, py = OFF + y * CELL;
                c.fill(px, py, CELL, CELL, col);
                char pc = pos.b[sq];
                if (pc) {
                    char s[2] = {(char)(lower(pc) - 32), 0};
                    bool wp = is_white(pc);
                    int tw = 24;
                    c.text(px + CELL / 2 - tw / 2 + 2, py + CELL / 2 - 12 + 2, s, wp ? 0xFF3A2F60 : 0xFFE0D8F0, 3, true);
                    c.text(px + CELL / 2 - tw / 2, py + CELL / 2 - 12, s, wp ? th::white : 0xFF161226, 3, true);
                }
                if (y == 7) { char f[2] = {(char)('a' + x), 0}; c.text(px + CELL - 9, py + CELL - 12, f, dark ? 0xFFE0D8F0 : 0xFF7A6AA8); }
                if (x == 0) { char r[2] = {(char)('8' - y), 0}; c.text(px + 2, py + 2, r, dark ? 0xFFE0D8F0 : 0xFF7A6AA8); }
            }
        for (int i = 0; i < n; i++)
            if (mv[i].from == sel) {
                int px = OFF + (mv[i].to % 8) * CELL + CELL / 2, py = OFF + (mv[i].to / 8) * CELL + CELL / 2;
                c.circle(px, py, pos.b[mv[i].to] ? 22 : 6, 0xFF3FB27F, !pos.b[mv[i].to]);
            }
        int sx = OFF * 2 + CELL * 8;
        c.text(sx, 20, msg, 0xFFE0D8FF, 1, true);
        draw_button(c, sx, 50, 130, 24, "New game", hot(sx, 50, 130, 24), true);
        draw_button(c, sx, 82, 130, 24, "Undo move", hot(sx, 82, 130, 24), false, history.len >= 2 && !thinking);
        // captured material
        int wm = 0, bm = 0;
        for (int i = 0; i < 64; i++) { if (pos.b[i] && lower(pos.b[i]) != 'k') { if (is_white(pos.b[i])) wm += value(pos.b[i]); else bm += value(pos.b[i]); } }
        char b[40];
        snprintf(b, sizeof(b), "material %s%d", wm >= bm ? "+" : "-", iabs(wm - bm) / 100);
        c.text(sx, 124, b, 0xFFB8ACD6);
        c.text(sx, 144, "you are white", 0xFF8A80B8);
    }
    void on_command_undo() {
        if (history.len >= 2 && !thinking) {
            pos = history[history.len - 2];
            history.len -= 2;
            sel = last_from = last_to = -1; over = false;
            strlcpy(msg, "your move", sizeof(msg));
        }
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "Game", 24);
        m[0].add("New game", 1).add("Undo move", 2, "", history.len >= 2);
        return 1;
    }
    void on_command(int cmd) override {
        if (cmd == 1) reset();
        if (cmd == 2) on_command_undo();
        ui_invalidate();
    }
};

App* new_chess(const char*) { return new ChessApp(); }
