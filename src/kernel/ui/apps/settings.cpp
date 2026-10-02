// silicon - Settings
#include "apps.h"
#include "../../term/console.h"

struct SettingsApp : App {
    int page = 0;
    TextField user, host;
    int focus = -1;     // 0 user, 1 host
    int cw = 0, ch = 0;

    SettingsApp() { user.set(settings.user); host.set(settings.hostname); }
    const char* app_id() override { return "settings"; }
    const char* title() override { return "Settings"; }
    int pref_w() override { return 560; }
    int pref_h() override { return 470; }
    bool resizable() override { return false; }
    void on_resize(int w, int h) override { cw = w; ch = h; }

    static constexpr const char* pages[5] = {"Appearance", "Desktop", "Terminal", "System", "About"};
    static constexpr const char* page_icons[5] = {"wallpapers", "silicon", "terminal", "settings", "about"};
    static const int NAV_W = 150;

    Rect nav(int i) { return {8, 12 + i * 34, NAV_W - 16, 30}; }

    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        c.fill(0, 0, NAV_W, h, th::panel_dk);
        c.vline(NAV_W - 1, 0, h, th::border);
        for (int i = 0; i < 5; i++) {
            Rect r = nav(i);
            bool on = page == i;
            if (on) c.fill(r.x, r.y, r.w, r.h, accent());
            else if (hot(r.x, r.y, r.w, r.h)) c.fill(r.x, r.y, r.w, r.h, blend(th::panel_dk, accent(), 60));
            c.blit(icon(page_icons[i])->small, r.x + 6, r.y + 7);
            c.text(r.x + 30, r.y + 10, pages[i], on ? th::white : th::text, 1, on);
        }
        int x = NAV_W + 20, y = 18;
        c.text(x, y, pages[page], th::text, 2, true);
        y += 40;
        switch (page) {
        case 0: {
            c.text(x, y, "accent color", th::muted, 1, true);
            y += 18;
            for (int i = 0; i < accent_count; i++) {
                Rect r{x + i * 58, y, 50, 50};
                draw_bevel(c, r.x, r.y, r.w, r.h, accent_colors[i], settings.accent == i);
                if (settings.accent == i) c.text_centered(r.x, r.y + 19, r.w, G_CHECK, th::white, 1, true);
            }
            y += 58;
            c.text(x, y, accent_names[settings.accent], th::text);
            y += 30;
            c.text(x, y, "wallpaper", th::muted, 1, true);
            y += 18;
            Wallpaper* wp = wallpaper_get(settings.wallpaper);
            c.rect(x - 1, y - 1, wp->thumb.w + 2, wp->thumb.h + 2, th::ink);
            c.blit(wp->thumb, x, y);
            c.text(x + 140, y + 10, wp->name, th::text, 1, true);
            draw_button(c, x + 140, y + 34, 110, 24, "Choose" G_ELLIP, hot(x + 140, y + 34, 110, 24), true);
            draw_button(c, x + 260, y + 34, 70, 24, "Next", hot(x + 260, y + 34, 70, 24));
            break;
        }
        case 1: {
            draw_checkbox(c, x, y, "lift dock icons when hovering", settings.dock_magnify);
            draw_checkbox(c, x, y + 28, "24-hour clock", settings.clock24);
            draw_checkbox(c, x, y + 56, "show seconds in the top bar", settings.show_seconds);
            draw_checkbox(c, x, y + 84, "pc speaker sounds", settings.sounds);
            draw_checkbox(c, x, y + 112, "frames-per-second counter in the top bar", settings.show_fps);
            c.text(x, y + 150, "shortcuts", th::muted, 1, true);
            const char* sc[] = {"ctrl+space   launcher", "alt+tab      switch windows", "f3           overview",
                                "f11          maximize", "ctrl+alt+t   new terminal", "ctrl+w       close window",
                                "drag a window to an edge to snap it"};
            for (int i = 0; i < 7; i++) c.text(x, y + 170 + i * 16, sc[i], th::text);
            break;
        }
        case 2: {
            c.text(x, y, "color theme", th::muted, 1, true);
            y += 18;
            for (int i = 0; i < console_theme_count; i++) {
                Rect r{x, y + i * 44, 340, 38};
                const ConsoleTheme& t = console_themes[i];
                bool on = settings.term_theme == i;
                c.fill(r.x, r.y, r.w, r.h, t.colors[0]);
                c.rect(r.x, r.y, r.w, r.h, on ? accent() : th::ink);
                if (on) c.rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, accent());
                c.text(r.x + 10, r.y + 6, t.name, t.colors[7], 1, true);
                c.text(r.x + 10, r.y + 20, "user@silicon:~ $ ls", t.colors[13]);
                for (int k = 1; k < 8; k++) c.fill(r.x + 200 + k * 16, r.y + 12, 12, 12, t.colors[k + 8]);
            }
            y += console_theme_count * 44 + 6;
            draw_checkbox(c, x, y, "big font in the full-screen terminal", settings.term_scale == 2);
            c.text(x, y + 30, "default shell", th::muted, 1, true);
            for (int i = 0; i < 3; i++) {
                static const char* names[3] = {"bash", "zsh", "fish"};
                bool on = !strcmp(settings.shell, names[i]);
                draw_button(c, x + i * 76, y + 48, 70, 24, names[i], hot(x + i * 76, y + 48, 70, 24), on);
            }
            c.text(x + 236, y + 54, "for new terminals", th::muted);
            break;
        }
        case 3: {
            c.text(x, y, "user name", th::muted, 1, true);
            user.focused = focus == 0;
            user.draw(c, x, y + 16, 220);
            c.text(x, y + 52, "host name", th::muted, 1, true);
            host.focused = focus == 1;
            host.draw(c, x, y + 68, 220);
            draw_button(c, x + 230, y + 68, 70, 20, "Save", hot(x + 230, y + 68, 70, 20), true);
            c.text(x, y + 104, "startup", th::muted, 1, true);
            draw_checkbox(c, x, y + 122, "start in the desktop instead of the terminal", settings.boot_target == 1);
            draw_checkbox(c, x, y + 146, "fast boot (shorter firmware screen)", settings.fast_boot);
            draw_checkbox(c, x, y + 170, "network card (after a restart)", settings.net_enabled);
            c.text(x, y + 192, "more options: press DEL while booting", th::muted);
            char b[96], used[24], cap[24];
            fmt_bytes(fs_disk_used(), used, sizeof(used));
            fmt_bytes(fs_disk_capacity(), cap, sizeof(cap));
            snprintf(b, sizeof(b), "SiliconFS: %s of %s, %d nodes", used, cap, fs_count(fs_root()));
            c.text(x, y + 222, "storage", th::muted, 1, true);
            c.text(x, y + 240, fs_persistent() ? b : "RAM only - no disk attached", th::text);
            int pct = (int)((u64)fs_disk_used() * 100 / max(1u, fs_disk_capacity()));
            draw_progress(c, x, y + 258, 300, 12, max(pct, 2), accent());
            c.text(x, y + 290, "danger zone", th::red, 1, true);
            draw_button(c, x, y + 308, 160, 24, "Reset all files" G_ELLIP, hot(x, y + 308, 160, 24));
            break;
        }
        case 4: {
            c.blit(icon("silicon")->big, x, y);
            c.text(x + 80, y + 8, "silicon " SILICON_VERSION, th::text, 2, true);
            c.text(x + 80, y + 36, "a pixel OS made from scratch", th::muted);
            c.text(x, y + 90, "bootloader  own MBR + stage 2 (real -> long mode)", th::text);
            c.text(x, y + 108, "kernel      C++ and x86_64 assembly", th::text);
            c.text(x, y + 126, "storage     SiliconFS on the IDE disk", th::text);
            c.text(x, y + 144, "desktop     silicon-die dock, pixel windows", th::text);
            draw_button(c, x, y + 180, 140, 24, "More info" G_ELLIP, hot(x, y + 180, 140, 24));
            break;
        }
        }
    }

    static void reset_cb(App* a, int choice) {
        if (choice != 0) return;
        fs_format();
        settings_load();
        ui_notify("Files reset", "SiliconFS is factory fresh", "disk");
    }

    void on_mouse(const MouseEv& e) override {
        if (e.kind != M_DOWN) return;
        for (int i = 0; i < 5; i++) if (nav(i).contains(e.x, e.y)) { page = i; focus = -1; }
        int x = NAV_W + 20, y = 58;
        switch (page) {
        case 0:
            for (int i = 0; i < accent_count; i++) if (Rect{x + i * 58, y + 18, 50, 50}.contains(e.x, e.y)) { settings.accent = i; settings_save(); }
            if (Rect{x + 140, y + 124 + 34, 110, 24}.contains(e.x, e.y)) launch("wallpapers");
            if (Rect{x + 260, y + 124 + 34, 70, 24}.contains(e.x, e.y)) wallpaper_set((settings.wallpaper + 1) % wallpaper_count());
            break;
        case 1: {
            bool* opts[5] = {nullptr, &settings.clock24, &settings.show_seconds, &settings.sounds, &settings.show_fps};
            for (int i = 0; i < 5; i++)
                if (Rect{x, y + i * 28, 300, 16}.contains(e.x, e.y)) {
                    if (i == 0) settings.dock_magnify = !settings.dock_magnify;
                    else *opts[i] = !*opts[i];
                    settings_save();
                }
            break;
        }
        case 2:
            for (int i = 0; i < console_theme_count; i++)
                if (Rect{x, y + 18 + i * 44, 340, 38}.contains(e.x, e.y)) { settings.term_theme = i; settings_save(); }
            if (Rect{x, y + 18 + console_theme_count * 44 + 6, 300, 16}.contains(e.x, e.y)) {
                settings.term_scale = settings.term_scale == 2 ? 1 : 2;
                settings_save();
            }
            for (int i = 0; i < 3; i++) {
                static const char* names[3] = {"bash", "zsh", "fish"};
                if (Rect{x + i * 76, y + 18 + console_theme_count * 44 + 6 + 48, 70, 24}.contains(e.x, e.y)) {
                    strlcpy(settings.shell, names[i], sizeof(settings.shell));
                    settings_save();
                }
            }
            break;
        case 3:
            focus = -1;
            if (Rect{x, y + 16, 220, 20}.contains(e.x, e.y)) focus = 0;
            if (Rect{x, y + 68, 220, 20}.contains(e.x, e.y)) focus = 1;
            if (Rect{x + 230, y + 68, 70, 20}.contains(e.x, e.y)) save_names();
            if (Rect{x, y + 122, 340, 16}.contains(e.x, e.y)) { settings.boot_target = settings.boot_target ? 0 : 1; settings_save(); }
            if (Rect{x, y + 146, 340, 16}.contains(e.x, e.y)) { settings.fast_boot = !settings.fast_boot; settings_save(); }
            if (Rect{x, y + 170, 340, 16}.contains(e.x, e.y)) { settings.net_enabled = !settings.net_enabled; settings_save(); }
            if (Rect{x, y + 308, 160, 24}.contains(e.x, e.y))
                ask("erase every file and restore the starter files? this can't be undone.", "Erase", "Cancel", nullptr, reset_cb);
            break;
        case 4:
            if (Rect{x, y + 180, 140, 24}.contains(e.x, e.y)) launch("about");
            break;
        }
        ui_invalidate();
    }
    void save_names() {
        if (user.len) strlcpy(settings.user, user.text, sizeof(settings.user));
        if (host.len) strlcpy(settings.hostname, host.text, sizeof(settings.hostname));
        settings_save();
        ui_notify("Saved", "names updated", "settings");
    }
    void on_key(const KeyEvent& e) override {
        if (page == 3 && focus >= 0) {
            if (e.key == K_ENTER) { save_names(); focus = -1; }
            else if (e.key == K_TAB) focus ^= 1;
            else (focus == 0 ? user : host).on_key(e);
        } else if (e.key == K_DOWN) page = (page + 1) % 5;
        else if (e.key == K_UP) page = (page + 4) % 5;
        ui_invalidate();
    }
    bool wants_ibeam(int x, int y) override { return page == 3 && x > NAV_W + 20 && x < NAV_W + 240 && ((y > 74 && y < 94) || (y > 126 && y < 146)); }
    void tick() override {
        static u64 last = 0;
        if (focus >= 0 && ticks() - last > 500) { last = ticks(); ui_invalidate(); }
    }
};

App* new_settings(const char*) { return new SettingsApp(); }
