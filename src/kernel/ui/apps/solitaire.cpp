// silicon - Klondike solitaire (draw one). Click a card, then click where it goes; double-click sends a card up.
#include "gamekit.h"

struct SolitaireApp : GameApp {
    static const int CW = 60, CH = 84, MX = 12, SP = 72, FT = 34, TOPY = FT + 10, TABY = FT + 112;
    struct Pile { u8 c[52]; int n = 0; };     // card = suit * 13 + rank - 1, bit 7 = face up
    Pile stock, waste, found[4], tab[7];
    int sel_pile = -1, sel_idx = 0;           // pile ids: 0 waste, 1..4 foundations, 5..11 tableau
    bool won = false;
    int moves = 0;

    SolitaireApp() { gid = "solitaire"; load_best(); reset_game(); }
    const char* app_id() override { return "solitaire"; }
    const char* title() override { return "Solitaire"; }
    int pref_w() override { return MX * 2 + SP * 7 - (SP - CW); }
    int pref_h() override { return 560; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF1E5A44; }

    static int rank(u8 c) { return (c & 0x7F) % 13 + 1; }
    static int suit(u8 c) { return (c & 0x7F) / 13; }
    static bool red(u8 c) { return suit(c) == 1 || suit(c) == 2; }
    static bool up(u8 c) { return c & 0x80; }

    void reset_game() override {
        if (score > best) { best = score; game_save_best(gid, score); }
        u8 deck[52];
        for (int i = 0; i < 52; i++) deck[i] = (u8)i;
        for (int i = 51; i > 0; i--) { int j = rand() % (i + 1); swap(deck[i], deck[j]); }
        int k = 0;
        stock.n = waste.n = 0;
        for (auto& f : found) f.n = 0;
        for (int t = 0; t < 7; t++) {
            tab[t].n = 0;
            for (int i = 0; i <= t; i++) tab[t].c[tab[t].n++] = deck[k++] | (i == t ? 0x80 : 0);
        }
        while (k < 52) stock.c[stock.n++] = deck[k++];
        sel_pile = -1; won = false; over = false; started = true; score = 0; moves = 0;
        ui_invalidate();
    }
    Pile* pile(int id) { return id == 0 ? &waste : id <= 4 ? &found[id - 1] : &tab[id - 5]; }
    int colx(int col) { return MX + col * SP; }
    int tab_y(Pile& p, int i) {
        int y = TABY;
        for (int k = 0; k < i; k++) y += up(p.c[k]) ? 24 : 10;
        return y;
    }

    bool can_found(u8 card, Pile& f) {
        if (f.n == 0) return rank(card) == 1;
        u8 t = f.c[f.n - 1];
        return suit(t) == suit(card) && rank(card) == rank(t) + 1;
    }
    bool can_tab(u8 card, Pile& t) {
        if (t.n == 0) return rank(card) == 13;
        u8 top = t.c[t.n - 1];
        return up(top) && red(top) != red(card) && rank(card) == rank(top) - 1;
    }
    void flip_top(Pile& p) { if (p.n && !up(p.c[p.n - 1])) { p.c[p.n - 1] |= 0x80; score += 5; } }

    // move cards from (src, idx..end) onto dest
    bool move_to(int src, int idx, int dest) {
        Pile* s = pile(src);
        Pile* d = pile(dest);
        if (src == dest || idx >= s->n) return false;
        u8 card = s->c[idx];
        int cnt = s->n - idx;
        if (dest >= 1 && dest <= 4) { if (cnt != 1 || !can_found(card, *d)) return false; }
        else if (dest >= 5) { if (!can_tab(card, *d)) return false; }
        else return false;
        for (int i = 0; i < cnt; i++) d->c[d->n++] = s->c[idx + i];
        s->n = idx;
        if (dest >= 1 && dest <= 4) score += 10;
        else if (src == 0) score += 5;
        if (src >= 5) flip_top(*s);
        moves++;
        ui_beep(0);
        int total = 0;
        for (auto& f : found) total += f.n;
        if (total == 52) { won = true; over = true; score += 200; if (score > best) { best = score; game_save_best(gid, score); } ui_beep(2); }
        return true;
    }
    void to_foundation(int src) {
        Pile* s = pile(src);
        if (!s->n) return;
        for (int f = 1; f <= 4; f++) if (move_to(src, s->n - 1, f)) return;
    }
    bool hit(int mx, int my, int* id, int* idx) {
        if (my >= TOPY && my < TOPY + CH) {
            for (int f = 0; f < 4; f++) if (mx >= colx(3 + f) && mx < colx(3 + f) + CW) { *id = 1 + f; *idx = max(0, found[f].n - 1); return true; }
            if (mx >= colx(1) && mx < colx(1) + CW) { *id = 0; *idx = max(0, waste.n - 1); return true; }
            if (mx >= colx(0) && mx < colx(0) + CW) { *id = -2; *idx = 0; return true; }
        }
        for (int t = 0; t < 7; t++) {
            if (mx < colx(t) || mx >= colx(t) + CW || my < TABY) continue;
            Pile& p = tab[t];
            int best_i = -1;
            for (int i = 0; i < p.n; i++) {
                int y = tab_y(p, i);
                int hgt = i == p.n - 1 ? CH : (up(p.c[i]) ? 24 : 10);
                if (my >= y && my < y + hgt) best_i = i;
            }
            *id = 5 + t; *idx = best_i < 0 ? max(0, p.n - 1) : best_i;
            if (p.n == 0) *idx = 0;
            return true;
        }
        return false;
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind != M_DOWN && e.kind != M_DBL) return;
        if (e.button != 1) return;
        if (won) { reset_game(); return; }
        int id, idx;
        if (!hit(e.x, e.y, &id, &idx)) { sel_pile = -1; ui_invalidate(); return; }
        if (id == -2) {
            sel_pile = -1;
            if (stock.n) { waste.c[waste.n++] = stock.c[--stock.n] | 0x80; }
            else { while (waste.n) stock.c[stock.n++] = waste.c[--waste.n] & 0x7F; score = max(0, score - 20); }
            ui_invalidate();
            return;
        }
        if (e.kind == M_DBL) {
            Pile* p = pile(id);
            if (p->n && (id == 0 || id >= 5) && idx == p->n - 1) { to_foundation(id); sel_pile = -1; }
            ui_invalidate();
            return;
        }
        if (sel_pile >= 0) {
            bool ok = move_to(sel_pile, sel_idx, id);
            sel_pile = -1;
            if (ok) { ui_invalidate(); return; }
        }
        Pile* p = pile(id);
        if (p->n && up(p->c[idx])) { sel_pile = id; sel_idx = idx; }
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.key == 'n') { reset_game(); return; }
        if (e.key == 'f' && sel_pile >= 0) { to_foundation(sel_pile); sel_pile = -1; ui_invalidate(); }
    }

    void suit_shape(Canvas& c, int s, int cx, int cy, int r, u32 col) {
        switch (s) {
        case 0: // spade
            for (int k = 0; k <= r; k++) { int w = k * 7 / 8; c.hline(cx - w, cy - r + k, 2 * w + 1, col); }
            c.circle(cx - r / 2, cy + r / 6, r / 2 + 1, col, true);
            c.circle(cx + r / 2, cy + r / 6, r / 2 + 1, col, true);
            c.fill(cx - 1, cy + r / 2, 3, r / 2 + 1, col);
            break;
        case 1: { // heart
            c.circle(cx - r / 2, cy - r / 3, r / 2 + 1, col, true);
            c.circle(cx + r / 2, cy - r / 3, r / 2 + 1, col, true);
            int tot = r * 4 / 3 + 1;
            for (int k = 0; k < tot; k++) { int w = (r + r / 2) * (tot - k) / tot; c.hline(cx - w, cy - r / 3 + k, 2 * w + 1, col); }
            break;
        }
        case 2: // diamond
            for (int k = -r; k <= r; k++) { int w = (r - iabs(k)) * 3 / 4; c.hline(cx - w, cy + k, 2 * w + 1, col); }
            break;
        default: // club
            c.circle(cx, cy - r / 2, r / 2 + 1, col, true);
            c.circle(cx - r / 2, cy + r / 4, r / 2 + 1, col, true);
            c.circle(cx + r / 2, cy + r / 4, r / 2 + 1, col, true);
            c.fill(cx - 1, cy, 3, r + 1, col);
            break;
        }
    }
    void card_at(Canvas& c, u8 card, int x, int y, bool selected, int hgt) {
        if (!up(card)) {
            c.fill(x, y, CW, hgt, 0xFF2A3F8A);
            c.rect(x, y, CW, hgt, th::white);
            if (hgt >= CH) for (int k = 4; k < CH - 4; k += 6) c.hline(x + 4, y + k, CW - 8, 0xFF3C55B0);
            return;
        }
        c.fill(x, y, CW, hgt, selected ? 0xFFFFF4C0 : 0xFFFBF9FF);
        c.rect(x, y, CW, hgt, selected ? th::gold : 0xFF6A6288);
        u32 col = red(card) ? 0xFFD23A58 : 0xFF1A1428;
        static const char* names[14] = {"", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};
        c.text(x + 4, y + 4, names[rank(card)], col, 1, true);
        suit_shape(c, suit(card), x + 8 + (rank(card) == 10 ? 8 : 0) + 8, y + 22, 5, col);
        if (hgt >= CH) suit_shape(c, suit(card), x + CW / 2, y + CH / 2 + 4, 14, col);
    }
    void draw(Canvas& c, int w, int h) override {
        bar(c, w, 0xFF123E2E, 0xFF2A7A58, 0xFFE8F8EE, 0xFF8AC8A8);
        // stock / waste / foundations
        c.rect(colx(0), TOPY, CW, CH, 0xFF3A8A68);
        if (stock.n) card_at(c, 0, colx(0), TOPY, false, CH);
        else c.text_centered(colx(0), TOPY + CH / 2 - 6, CW, "again", 0xFF8AC8A8);
        if (waste.n) card_at(c, waste.c[waste.n - 1], colx(1), TOPY, sel_pile == 0, CH);
        for (int f = 0; f < 4; f++) {
            c.rect(colx(3 + f), TOPY, CW, CH, 0xFF3A8A68);
            if (found[f].n) card_at(c, found[f].c[found[f].n - 1], colx(3 + f), TOPY, false, CH);
            else c.text_centered(colx(3 + f), TOPY + CH / 2 - 6, CW, "A", 0xFF3A8A68, 2, true);
        }
        for (int t = 0; t < 7; t++) {
            Pile& p = tab[t];
            if (!p.n) c.rect(colx(t), TABY, CW, CH, 0xFF3A8A68);
            for (int i = 0; i < p.n; i++) {
                int hgt = i == p.n - 1 ? CH : (up(p.c[i]) ? 24 : 10);
                bool s = sel_pile == 5 + t && i >= sel_idx;
                card_at(c, p.c[i], colx(t), tab_y(p, i), s, hgt);
            }
        }
        if (won) {
            c.fill_alpha(0, FT, w, h - FT, 0xFF000000, 140);
            c.text_centered(0, h / 2 - 30, w, "YOU WIN", th::white, 4, true);
            c.text_centered(0, h / 2 + 18, w, "click to deal again", 0xFFD8F0E0);
        }
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "Game", 24);
        m[0].add("New deal", 1, "n").add("Send selected up", 2, "f");
        return 1;
    }
    void on_command(int cmd) override {
        if (cmd == 1) reset_game();
        if (cmd == 2 && sel_pile >= 0) { to_foundation(sel_pile); sel_pile = -1; }
        ui_invalidate();
    }
};

App* new_solitaire(const char*) { return new SolitaireApp(); }
