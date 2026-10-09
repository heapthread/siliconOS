// silicon - Apps: every app, utility and game in one searchable grid
#include "apps.h"

struct AppsApp : App {
    TextField search;
    Smooth sc;
    ScrollBar sb;
    int cw = 0, ch = 0, sel = -1;

    struct Entry { const AppInfo* info; int cat; };
    Vec<Entry> shown;

    static const int TILE_W = 142, TILE_H = 88, HEAD = 30, TOP = 46;
    static const int NCAT = 4;

    AppsApp() { search.focused = true; refresh(); }
    ~AppsApp() { shown.free(); }
    const char* app_id() override { return "apps"; }
    const char* title() override { return "Apps"; }
    int pref_w() override { return 640; }
    int pref_h() override { return 520; }

    static int category(const char* id) {
        static const char* games[] = {"stacker", "g2048", "breakout", "flappy", "minesweeper", "snake", "reversi", "pong", "invaders",
                                      "asteroids", "runner", "connect4", "memory", "wordy", "sudoku", "solitaire", "chess", "games"};
        static const char* utils[] = {"calendar", "tasks", "hex", "storage", "convert", "palette", "capture", "weather", "typing",
                                      "network", "archive", "calculator", "clock", "piano", "emoji", "fonts", "docs", "mail"};
        static const char* sys[] = {"settings", "monitor", "about", "power", "trash", "wallpapers", "notifications", "firewall", "apps"};
        for (auto g : games) if (!strcmp(g, id)) return 2;
        for (auto u : utils) if (!strcmp(u, id)) return 1;
        for (auto s : sys) if (!strcmp(s, id)) return 3;
        return 0;
    }
    static const char* cat_name(int c) {
        static const char* n[NCAT] = {"Apps", "Utilities", "Games", "System"};
        return n[c];
    }
    static const char* icon_of(const char* id) { return !strcmp(id, "pdfview") ? "pdf" : id; }

    void refresh() {
        shown.clear();
        for (int c = 0; c < NCAT; c++)
            for (int i = 0; i < app_info_count; i++) {
                const AppInfo& a = app_infos[i];
                if (!strcmp(a.id, "module") || category(a.id) != c) continue;
                if (search.len && !str_icontains(a.name, search.text) && !str_icontains(a.desc, search.text) && !str_icontains(a.id, search.text)) continue;
                shown.push({&a, c});
            }
        sel = shown.len ? 0 : -1;
        sc.jump(0);
    }
    int cols() { return max(1, (cw - 24) / (TILE_W + 6)); }
    // layout: category headers and tiles flow down the page
    struct Pos { int x, y; };
    Pos pos_of(int idx, int* header_y_out = nullptr) {
        int y = 0, col = 0, last_cat = -1;
        Pos p = {0, 0};
        for (int i = 0; i <= idx && i < shown.len; i++) {
            if (shown[i].cat != last_cat) { if (last_cat >= 0) y += TILE_H + 8; y += HEAD; last_cat = shown[i].cat; col = 0; }
            else if (col >= cols()) { col = 0; y += TILE_H + 8; }
            p = {12 + col * (TILE_W + 6), y};
            col++;
        }
        return p;
    }
    int content_h() {
        if (!shown.len) return 0;
        Pos p = pos_of(shown.len - 1);
        return p.y + TILE_H + 16;
    }
    void tick() override { if (sc.step()) ui_invalidate(); }

    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        c.fill(0, 0, w, TOP - 2, th::panel);
        c.hline(0, TOP - 3, w, th::border);
        c.blit(icon("apps")->small, 12, 15);
        search.focused = true;
        search.draw(c, 36, 9, w - 36 - 14);
        if (!search.len) c.text(44, 17, "search apps, utilities and games", th::muted);
        c.set_clip({0, TOP - 2, w - 11, h - TOP + 2});
        int last_cat = -1;
        for (int i = 0; i < shown.len; i++) {
            Pos p = pos_of(i);
            int y = TOP + p.y - sc.pos();
            if (shown[i].cat != last_cat) {
                last_cat = shown[i].cat;
                int hy = y - HEAD + 6;
                c.text(14, hy + 4, cat_name(last_cat), th::text, 1, true);
                c.hline(14 + text_width(cat_name(last_cat), 1, true) + 8, hy + 10, w - 60 - text_width(cat_name(last_cat), 1, true), th::border);
            }
            if (y + TILE_H < TOP || y > h) continue;
            bool on = hot(p.x, y, TILE_W, TILE_H);
            if (on || i == sel) draw_bevel(c, p.x, y, TILE_W, TILE_H, i == sel ? blend(th::paper, accent(), 60) : th::panel);
            c.blit(icon(icon_of(shown[i].info->id))->big, p.x + (TILE_W - 64) / 2, y + 4);
            c.text_centered(p.x, y + 70, TILE_W, shown[i].info->name, th::text, 1, true);
            if (on) c.text_centered(p.x, y + 70 + 0, TILE_W, "", th::muted);
        }
        if (!shown.len) c.text_centered(0, TOP + 80, w, "nothing matches", th::muted, 1, true);
        c.set_clip({0, 0, w, h});
        sb.draw(c, w - 10, TOP, h - TOP, content_h() + 8, h - TOP, sc.pos());
        // the description of the hovered tile
        if (sel >= 0 && sel < shown.len) {
            c.fill(0, h - 20, w - 11, 20, th::panel);
            c.hline(0, h - 20, w - 11, th::border);
            char b[120];
            snprintf(b, sizeof(b), "%s  " G_BULLET "  %s", shown[sel].info->name, shown[sel].info->desc);
            c.text_n(10, h - 15, b, (w - 40) / 8, th::muted);
        }
    }
    int hit(int mx, int my) {
        if (my < TOP) return -1;
        for (int i = 0; i < shown.len; i++) {
            Pos p = pos_of(i);
            if (Rect{p.x, TOP + p.y - sc.pos(), TILE_W, TILE_H}.contains(mx, my)) return i;
        }
        return -1;
    }
    void launch_idx(int i) {
        if (i < 0 || i >= shown.len) return;
        launch(shown[i].info->id);
    }
    void on_mouse(const MouseEv& e) override {
        int off = sc.target;
        if (sb.handle(e, cw - 10, TOP, ch - TOP, content_h() + 8, ch - TOP, off)) { sc.jump(off); ui_invalidate(); return; }
        if (e.kind == M_WHEEL) { sc.set(clamp(sc.target - e.wheel * 60, 0, max(0, content_h() + 8 - (ch - TOP)))); ui_invalidate(); return; }
        if (e.y < TOP) {
            if (e.kind == M_DOWN) search.click(e.x - 36, cw - 50, kbd_shift_down());
            else if (e.kind == M_MOVE && e.left) search.drag(e.x - 36, cw - 50);
            ui_invalidate();
            return;
        }
        int i = hit(e.x, e.y);
        if (e.kind == M_MOVE) { if (i >= 0 && i != sel) { sel = i; } ui_invalidate(); return; }
        if (e.kind == M_UP && e.button == 1 && i >= 0) launch_idx(i);
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.key == K_ENTER) { launch_idx(sel); return; }
        if (e.key == K_DOWN) sel = min(sel + cols(), shown.len - 1);
        else if (e.key == K_UP) sel = max(sel - cols(), 0);
        else if (e.key == K_RIGHT && !search.has_sel() && search.cur == search.len) sel = min(sel + 1, shown.len - 1);
        else if (e.key == K_LEFT && search.cur == search.len && sel > 0) sel--;
        else {
            char before[128];
            strlcpy(before, search.text, sizeof(before));
            if (search.on_key(e) && strcmp(before, search.text)) refresh();
        }
        if (sel >= 0) {
            int y = pos_of(sel).y;
            int t = sc.target;
            if (y < t) t = max(0, y - HEAD);
            if (y + TILE_H > t + ch - TOP) t = y + TILE_H - (ch - TOP) + 12;
            sc.set(max(0, t));
        }
        ui_invalidate();
    }
};

App* new_apps(const char*) { return new AppsApp(); }
