// silicon - Wallpapers picker
#include "apps.h"

struct WallpapersApp : App {
    Smooth sc;
    ScrollBar sb;
    int cw = 0, ch = 0;
    const char* app_id() override { return "wallpapers"; }
    const char* title() override { return "Wallpapers"; }
    int pref_w() override { return 3 * 148 + 20; }
    int pref_h() override { return 430; }
    bool resizable() override { return false; }
    void on_resize(int w, int h) override { cw = w; ch = h; }
    void tick() override { if (sc.step()) ui_invalidate(); }

    Rect cell(int i) { return {14 + (i % 3) * 148, 44 + (i / 3) * 128 - sc.pos(), 136, 120}; }

    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        c.set_clip({0, 34, w, h - 34});
        for (int i = 0; i < wallpaper_count(); i++) {
            Rect r = cell(i);
            Wallpaper* wp = wallpaper_get(i);
            bool cur = i == settings.wallpaper;
            bool on = hot(r.x, r.y, r.w, r.h);
            if (cur) c.fill(r.x - 4, r.y - 4, r.w + 8, 104, accent());
            else if (on) c.fill(r.x - 4, r.y - 4, r.w + 8, 104, blend(th::paper, accent(), 80));
            c.rect(r.x + 3, r.y - 1, 130, 98, th::ink);
            c.blit(wp->thumb, r.x + 4, r.y);
            char nm[20];
            strlcpy(nm, wp->name, sizeof(nm));
            c.text_centered(r.x, r.y + 102, r.w, nm, cur ? accent() : th::text, 1, cur);
            if (wp->custom) c.text(r.x + 8, r.y + 4, "pixel", th::white, 1, true);
        }
        c.reset_clip();
        c.set_clip({0, 0, w, h});
        c.fill(0, 0, w, 34, th::panel);
        c.hline(0, 33, w, th::border);
        c.text(12, 11, "pick a backdrop", th::text, 1, true);
        char b[48];
        snprintf(b, sizeof(b), "%d wallpapers", wallpaper_count());
        c.text(w - text_width(b) - 12, 11, b, th::muted);
        int rows = (wallpaper_count() + 2) / 3;
        if (rows * 128 > h - 44) sb.draw(c, w - 10, 34, h - 34, rows * 128 + 10, h - 44, sc.pos());
    }
    void on_mouse(const MouseEv& e) override {
        int rows = (wallpaper_count() + 2) / 3;
        int total = rows * 128 + 10, maxs = max(0, total - (ch - 44));
        if (e.kind == M_WHEEL) sc.set(clamp(sc.target - e.wheel * 64, 0, maxs));
        int off = sc.target;
        if (sb.handle(e, cw - 10, 34, ch - 34, total, ch - 44, off)) { sc.jump(off); return; }
        if (e.kind == M_DOWN && e.y > 34)
            for (int i = 0; i < wallpaper_count(); i++) if (cell(i).contains(e.x, e.y)) wallpaper_set(i);
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        int n = wallpaper_count();
        if (e.key == K_RIGHT) wallpaper_set((settings.wallpaper + 1) % n);
        if (e.key == K_LEFT) wallpaper_set((settings.wallpaper + n - 1) % n);
        if (e.key == K_DOWN) wallpaper_set(min(settings.wallpaper + 3, n - 1));
        if (e.key == K_UP) wallpaper_set(max(settings.wallpaper - 3, 0));
        ui_invalidate();
    }
};

App* new_wallpapers(const char*) { return new WallpapersApp(); }
