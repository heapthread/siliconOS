// silicon - Settings: appearance, display, desktop, terminal, security, system, about
#include "apps.h"
#include "../../term/console.h"
#include "../../display.h"
#include "../lockscreen.h"

enum {
    ID_NAV = 1, ID_ACCENT = 20, ID_WALL_CHOOSE = 40, ID_WALL_NEXT, ID_RES = 50, ID_FPS = 70, ID_CHECK = 100,
    ID_THEME = 140, ID_SHELL = 160, ID_FIELD = 200, ID_SAVE_NAMES = 220, ID_RESET, ID_ABOUT, ID_PW_SET, ID_PW_REMOVE,
    ID_SLEEP = 240, ID_SAVER = 260, ID_LOCK_NOW = 280, ID_SLEEP_NOW, ID_FSCHECK = 290,
};

struct Res { int w, h; };
static const Res RESOLUTIONS[] = {{0, 0}, {1024, 768}, {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}};
static const int FPS_CHOICES[] = {0, 30, 60, 75, 90, 120, 144, 165, 240, 1000};
static const int SLEEP_CHOICES[] = {0, 5, 10, 15, 30, 60};
static const int SAVER_CHOICES[] = {0, 1, 2, 5, 10, 15};

struct SettingsApp : App {
    int page = 0;
    TextField user, host, pw_cur, pw1, pw2;
    int focus = -1;                       // 0 user, 1 host, 2 current pw, 3 new pw, 4 repeat
    int cw = 0, ch = 0;
    struct Hit { Rect r; int id; };
    Vec<Hit> hits;
    char msg[96] = "";
    bool msg_ok = true;
    u64 msg_t = 0;

    SettingsApp() {
        user.set(settings.user);
        host.set(settings.hostname);
        pw_cur.password = pw1.password = pw2.password = true;
    }
    ~SettingsApp() { hits.free(); }
    const char* app_id() override { return "settings"; }
    const char* title() override { return "Settings"; }
    int pref_w() override { return 640; }
    int pref_h() override { return 520; }
    bool resizable() override { return false; }
    void on_resize(int w, int h) override { cw = w; ch = h; }

    static const int NPAGES = 7;
    static constexpr const char* pages[NPAGES] = {"Appearance", "Display", "Desktop", "Terminal", "Security", "System", "About"};
    static const int NAV_W = 150;
    Rect nav(int i) { return {8, 12 + i * 34, NAV_W - 16, 30}; }

    TextField* field(int i) {
        switch (i) { case 0: return &user; case 1: return &host; case 2: return &pw_cur; case 3: return &pw1; case 4: return &pw2; }
        return nullptr;
    }

    // ------------------------------------------------------------ immediate-mode helpers
    void add_hit(Rect r, int id) { hits.push(Hit{r, id}); }
    bool btn(Canvas& c, int x, int y, int w, int h, const char* label, int id, bool primary = false, bool on = false) {
        bool hv = hot(x, y, w, h);
        draw_button(c, x, y, w, h, label, hv, primary || on);
        add_hit({x, y, w, h}, id);
        return hv;
    }
    void check(Canvas& c, int x, int y, const char* label, bool on, int id) {
        draw_checkbox(c, x, y, label, on);
        add_hit({x, y - 2, 20 + (int)strlen(label) * 8 + 8, 20}, id);
    }
    // a row of mutually exclusive chips; wraps at maxw
    void chips(Canvas& c, int x, int y, int maxw, const char* const* labels, int n, int sel, int id_base, int chip_w = 0) {
        int cx = x, cy = y;
        for (int i = 0; i < n; i++) {
            int w = chip_w ? chip_w : (int)strlen(labels[i]) * 8 + 22;
            if (cx + w > x + maxw) { cx = x; cy += 28; }
            btn(c, cx, cy, w, 24, labels[i], id_base + i, false, sel == i);
            cx += w + 6;
        }
    }
    int chips_rows(int maxw, const char* const* labels, int n, int chip_w = 0) {
        int cx = 0, rows = 1;
        for (int i = 0; i < n; i++) {
            int w = chip_w ? chip_w : (int)strlen(labels[i]) * 8 + 22;
            if (cx + w > maxw) { cx = 0; rows++; }
            cx += w + 6;
        }
        return rows;
    }
    void label(Canvas& c, int x, int y, const char* t) { c.text(x, y, t, th::muted, 1, true); }
    void say(const char* m, bool ok) { strlcpy(msg, m, sizeof(msg)); msg_ok = ok; msg_t = ticks(); ui_invalidate(); }

    void draw_nav_icon(Canvas& c, int i, int x, int y) {
        static const char* names[NPAGES] = {"wallpapers", nullptr, "silicon", "terminal", nullptr, "settings", "about"};
        if (names[i]) { c.blit(icon(names[i])->small, x, y); return; }
        int e = emoji_index(i == 1 ? 0x1F4BB : 0x1F512);
        if (const Image* s = emoji_sprite(e, 16)) c.blit(*s, x, y - 1);
    }

    // ------------------------------------------------------------ drawing
    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        hits.clear();
        c.fill(0, 0, NAV_W, h, th::panel_dk);
        c.vline(NAV_W - 1, 0, h, th::border);
        for (int i = 0; i < NPAGES; i++) {
            Rect r = nav(i);
            bool on = page == i;
            if (on) c.fill(r.x, r.y, r.w, r.h, accent());
            else if (hot(r.x, r.y, r.w, r.h)) c.fill(r.x, r.y, r.w, r.h, blend(th::panel_dk, accent(), 60));
            draw_nav_icon(c, i, r.x + 6, r.y + 7);
            c.text(r.x + 30, r.y + 10, pages[i], on ? th::white : th::text, 1, on);
            add_hit(r, ID_NAV + i);
        }
        int x = NAV_W + 22, y = 16;
        c.text(x, y, pages[page], th::text, 2, true);
        y += 40;
        switch (page) {
        case 0: page_appearance(c, x, y); break;
        case 1: page_display(c, x, y, w); break;
        case 2: page_desktop(c, x, y); break;
        case 3: page_terminal(c, x, y); break;
        case 4: page_security(c, x, y, w); break;
        case 5: page_system(c, x, y); break;
        case 6: page_about(c, x, y); break;
        }
        if (msg[0] && ticks() - msg_t < 5000) {
            c.fill(NAV_W, h - 28, w - NAV_W, 28, msg_ok ? blend(th::paper, th::green, 60) : blend(th::paper, th::red, 70));
            c.text(NAV_W + 14, h - 20, msg, th::text, 1, true);
        }
    }

    void page_appearance(Canvas& c, int x, int y) {
        label(c, x, y, "accent color");
        y += 18;
        for (int i = 0; i < accent_count; i++) {
            Rect r{x + i * 58, y, 50, 50};
            draw_bevel(c, r.x, r.y, r.w, r.h, accent_colors[i], settings.accent == i);
            if (settings.accent == i) c.text_centered(r.x, r.y + 19, r.w, G_CHECK, th::white, 1, true);
            add_hit(r, ID_ACCENT + i);
        }
        y += 58;
        c.text(x, y, accent_names[settings.accent], th::text);
        y += 30;
        label(c, x, y, "wallpaper");
        y += 18;
        Wallpaper* wp = wallpaper_get(settings.wallpaper);
        c.rect(x - 1, y - 1, wp->thumb.w + 2, wp->thumb.h + 2, th::ink);
        c.blit(wp->thumb, x, y);
        c.text(x + 140, y + 10, wp->name, th::text, 1, true);
        btn(c, x + 140, y + 34, 110, 24, "Choose" G_ELLIP, ID_WALL_CHOOSE, true);
        btn(c, x + 260, y + 34, 70, 24, "Next", ID_WALL_NEXT);
    }

    void page_display(Canvas& c, int x, int y, int w) {
        int maxw = w - x - 20;
        label(c, x, y, "resolution");
        y += 18;
        static char names[7][16];
        const char* labels[7];
        for (int i = 0; i < 7; i++) {
            if (!RESOLUTIONS[i].w) strlcpy(names[i], "fit window", sizeof(names[i]));
            else snprintf(names[i], sizeof(names[i]), "%dx%d", RESOLUTIONS[i].w, RESOLUTIONS[i].h);
            labels[i] = names[i];
        }
        int sel = 0;
        for (int i = 1; i < 7; i++) if (settings.res_w == RESOLUTIONS[i].w && settings.res_h == RESOLUTIONS[i].h) sel = i;
        chips(c, x, y, maxw, labels, 7, sel, ID_RES);
        y += chips_rows(maxw, labels, 7) * 28 + 2;
        char b[120];
        snprintf(b, sizeof(b), "now: %dx%d%s", fb.width, fb.height, settings.res_w ? "" : "  " G_BULLET "  follows the size of the window");
        c.text(x, y, b, th::text);
        y += 16;
        c.text(x, y, fb_can_resize() ? "drag the window edge and silicon fills it" : "this display can't change its size", th::muted);
        y += 34;

        label(c, x, y, "frame rate");
        y += 18;
        static char fnames[10][16];
        const char* flabels[10];
        for (int i = 0; i < 10; i++) {
            if (!FPS_CHOICES[i]) snprintf(fnames[i], sizeof(fnames[i]), "auto");
            else if (FPS_CHOICES[i] >= 1000) snprintf(fnames[i], sizeof(fnames[i]), "max");
            else snprintf(fnames[i], sizeof(fnames[i]), "%d", FPS_CHOICES[i]);
            flabels[i] = fnames[i];
        }
        int fsel = 0;
        for (int i = 1; i < 10; i++) if (settings.fps_limit == FPS_CHOICES[i]) fsel = i;
        chips(c, x, y, maxw, flabels, 10, fsel, ID_FPS, 54);
        y += chips_rows(maxw, flabels, 10, 54) * 28 + 2;
        snprintf(b, sizeof(b), "monitor: %d Hz  " G_BULLET "  cap: %d fps  " G_BULLET "  drawing: %d fps", display_monitor_hz(),
                 display_target_fps(), ui_fps());
        c.text(x, y, b, th::text);
        y += 30;
        check(c, x, y, "show the fps counter in the top bar", settings.show_fps, ID_CHECK + 4);
    }

    void page_desktop(Canvas& c, int x, int y) {
        check(c, x, y, "lift dock icons when hovering", settings.dock_magnify, ID_CHECK + 0);
        check(c, x, y + 28, "24-hour clock", settings.clock24, ID_CHECK + 1);
        check(c, x, y + 56, "show seconds in the top bar", settings.show_seconds, ID_CHECK + 2);
        check(c, x, y + 84, "pc speaker sounds", settings.sounds, ID_CHECK + 3);
        label(c, x, y + 130, "shortcuts");
        const char* sc[] = {"ctrl+space   launcher", "alt+tab      switch windows", "f3           overview",
                            "f11          maximize", "ctrl+alt+t   new terminal", "ctrl+alt+l   lock the screen",
                            "ctrl+w       close window", "ctrl+c / ctrl+v   copy and paste, even with Windows",
                            "drag a window to an edge to snap it"};
        for (int i = 0; i < 9; i++) c.text(x, y + 150 + i * 16, sc[i], th::text);
    }

    void page_terminal(Canvas& c, int x, int y) {
        label(c, x, y, "color theme");
        y += 18;
        int per_row = 2, tw = 210;
        for (int i = 0; i < console_theme_count; i++) {
            Rect r{x + (i % per_row) * (tw + 10), y + (i / per_row) * 44, tw, 38};
            const ConsoleTheme& t = console_themes[i];
            bool on = settings.term_theme == i;
            c.fill(r.x, r.y, r.w, r.h, t.colors[0]);
            c.rect(r.x, r.y, r.w, r.h, on ? accent() : th::ink);
            if (on) c.rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, accent());
            c.text(r.x + 8, r.y + 6, t.name, t.colors[7], 1, true);
            c.text(r.x + 8, r.y + 20, "user@silicon:~ $", t.colors[13]);
            for (int k = 1; k < 8; k++) c.fill(r.x + 128 + k * 10, r.y + 12, 8, 12, t.colors[k + 8]);
            add_hit(r, ID_THEME + i);
        }
        y += ((console_theme_count + per_row - 1) / per_row) * 44 + 8;
        check(c, x, y, "big font in the full-screen terminal", settings.term_scale == 2, ID_CHECK + 5);
        label(c, x, y + 30, "default shell");
        static const char* names[3] = {"bash", "zsh", "fish"};
        int sel = !strcmp(settings.shell, "zsh") ? 1 : !strcmp(settings.shell, "fish") ? 2 : 0;
        chips(c, x, y + 48, 360, names, 3, sel, ID_SHELL, 70);
        c.text(x + 236, y + 54, "for new terminals", th::muted);
    }

    void page_security(Canvas& c, int x, int y, int w) {
        int maxw = w - x - 20;
        check(c, x, y, "show the sign-in screen when silicon starts", settings.greeter, ID_CHECK + 6);
        y += 34;
        bool has = password_is_set();
        label(c, x, y, has ? "password: set" : "password: none");
        y += 20;
        if (has) {
            c.text(x, y, "current", th::muted);
            pw_cur.focused = focus == 2;
            pw_cur.draw(c, x + 100, y - 4, 200);
            add_hit({x + 100, y - 4, 200, 20}, ID_FIELD + 2);
            y += 28;
        }
        c.text(x, y, "new", th::muted);
        pw1.focused = focus == 3;
        pw1.draw(c, x + 100, y - 4, 200);
        add_hit({x + 100, y - 4, 200, 20}, ID_FIELD + 3);
        y += 28;
        c.text(x, y, "repeat", th::muted);
        pw2.focused = focus == 4;
        pw2.draw(c, x + 100, y - 4, 200);
        add_hit({x + 100, y - 4, 200, 20}, ID_FIELD + 4);
        y += 30;
        btn(c, x + 100, y - 4, has ? 100 : 130, 22, has ? "Change" : "Set password", ID_PW_SET, true);
        if (has) btn(c, x + 208, y - 4, 92, 22, "Remove", ID_PW_REMOVE);
        y += 36;
        label(c, x, y, "sleep the screen after");
        y += 18;
        static const char* sl[6] = {"never", "5 min", "10 min", "15 min", "30 min", "1 hour"};
        int ssel = 0;
        for (int i = 0; i < 6; i++) if (settings.sleep_min == SLEEP_CHOICES[i]) ssel = i;
        chips(c, x, y, maxw, sl, 6, ssel, ID_SLEEP, 68);
        y += 36;
        label(c, x, y, "start the screensaver after");
        y += 18;
        static const char* sv[6] = {"never", "1 min", "2 min", "5 min", "10 min", "15 min"};
        int vsel = 0;
        for (int i = 0; i < 6; i++) if (settings.saver_min == SAVER_CHOICES[i]) vsel = i;
        chips(c, x, y, maxw, sv, 6, vsel, ID_SAVER, 68);
        y += 40;
        btn(c, x, y, 110, 24, "Lock now", ID_LOCK_NOW, true);
        btn(c, x + 120, y, 110, 24, "Sleep now", ID_SLEEP_NOW);
    }

    void page_system(Canvas& c, int x, int y) {
        label(c, x, y, "user name");
        user.focused = focus == 0;
        user.draw(c, x, y + 16, 220);
        add_hit({x, y + 16, 220, 20}, ID_FIELD + 0);
        label(c, x, y + 52, "host name");
        host.focused = focus == 1;
        host.draw(c, x, y + 68, 220);
        add_hit({x, y + 68, 220, 20}, ID_FIELD + 1);
        btn(c, x + 230, y + 68, 70, 20, "Save", ID_SAVE_NAMES, true);
        label(c, x, y + 104, "startup");
        check(c, x, y + 124, "start in the desktop instead of the terminal", settings.boot_target == 1, ID_FSCHECK + 0);
        check(c, x, y + 148, "fast boot (shorter firmware screen)", settings.fast_boot, ID_FSCHECK + 1);
        check(c, x, y + 172, "network card (after a restart)", settings.net_enabled, ID_FSCHECK + 2);
        c.text(x, y + 194, "more options: press DEL while booting", th::muted);
        char b[96], used[24], cap[24];
        fmt_bytes(fs_disk_used(), used, sizeof(used));
        fmt_bytes(fs_disk_capacity(), cap, sizeof(cap));
        snprintf(b, sizeof(b), "SiliconFS: %s of %s, %d nodes", used, cap, fs_count(fs_root()));
        label(c, x, y + 224, "storage");
        c.text(x, y + 242, fs_persistent() ? b : "RAM only - no disk attached", th::text);
        int pct = (int)((u64)fs_disk_used() * 100 / max(1u, fs_disk_capacity()));
        draw_progress(c, x, y + 260, 300, 12, max(pct, 2), accent());
        c.text(x, y + 292, "danger zone", th::red, 1, true);
        btn(c, x, y + 310, 160, 24, "Reset all files" G_ELLIP, ID_RESET);
    }

    void page_about(Canvas& c, int x, int y) {
        c.blit(icon("silicon")->big, x, y);
        c.text(x + 80, y + 8, "silicon " SILICON_VERSION, th::text, 2, true);
        c.text(x + 80, y + 36, "a pixel OS made from scratch", th::muted);
        c.text(x, y + 90, "bootloader  own MBR + stage 2 (real -> long mode)", th::text);
        c.text(x, y + 108, "kernel      C++ and x86_64 assembly", th::text);
        c.text(x, y + 126, "storage     SiliconFS on the IDE disk", th::text);
        c.text(x, y + 144, "network     RTL8139, TCP/IP, TLS, HTTP", th::text);
        c.text(x, y + 162, "desktop     silicon-die dock, pixel windows", th::text);
        btn(c, x, y + 200, 140, 24, "More info" G_ELLIP, ID_ABOUT);
    }

    // ------------------------------------------------------------ actions
    static void reset_cb(App*, int choice) {
        if (choice != 0) return;
        fs_format();
        settings_load();
        ui_notify("Files reset", "SiliconFS is factory fresh", "disk");
    }

    void save_names() {
        if (user.len) strlcpy(settings.user, user.text, sizeof(settings.user));
        if (host.len) strlcpy(settings.hostname, host.text, sizeof(settings.hostname));
        settings_save();
        say("names saved", true);
    }

    void set_password() {
        bool has = password_is_set();
        if (has && !password_check(pw_cur.text)) { say("the current password is wrong", false); return; }
        if (!pw1.len) { say("type a new password first", false); return; }
        if (strcmp(pw1.text, pw2.text)) { say("the two passwords differ", false); return; }
        password_set(pw1.text);
        pw_cur.set(""); pw1.set(""); pw2.set("");
        focus = -1;
        say(has ? "password changed" : "password set: the sign-in screen asks for it now", true);
    }

    void remove_password() {
        if (!password_is_set()) return;
        if (!password_check(pw_cur.text)) { say("type the current password to remove it", false); return; }
        password_set("");
        pw_cur.set(""); pw1.set(""); pw2.set("");
        say("password removed", true);
    }

    void act(int id) {
        if (id >= ID_NAV && id < ID_NAV + NPAGES) { page = id - ID_NAV; focus = -1; return; }
        if (id >= ID_ACCENT && id < ID_ACCENT + accent_count) { settings.accent = id - ID_ACCENT; settings_save(); return; }
        if (id == ID_WALL_CHOOSE) { launch("wallpapers"); return; }
        if (id == ID_WALL_NEXT) { wallpaper_set((settings.wallpaper + 1) % wallpaper_count()); return; }
        if (id >= ID_RES && id < ID_RES + 7) {
            settings.res_w = RESOLUTIONS[id - ID_RES].w;
            settings.res_h = RESOLUTIONS[id - ID_RES].h;
            settings_save();
            display_refresh_from_settings();
            return;
        }
        if (id >= ID_FPS && id < ID_FPS + 10) { settings.fps_limit = FPS_CHOICES[id - ID_FPS]; settings_save(); return; }
        if (id >= ID_CHECK && id < ID_CHECK + 8) {
            switch (id - ID_CHECK) {
            case 0: settings.dock_magnify = !settings.dock_magnify; break;
            case 1: settings.clock24 = !settings.clock24; break;
            case 2: settings.show_seconds = !settings.show_seconds; break;
            case 3: settings.sounds = !settings.sounds; break;
            case 4: settings.show_fps = !settings.show_fps; break;
            case 5: settings.term_scale = settings.term_scale == 2 ? 1 : 2; break;
            case 6: settings.greeter = !settings.greeter; break;
            }
            settings_save();
            return;
        }
        if (id >= ID_THEME && id < ID_THEME + console_theme_count) { settings.term_theme = id - ID_THEME; settings_save(); return; }
        if (id >= ID_SHELL && id < ID_SHELL + 3) {
            static const char* names[3] = {"bash", "zsh", "fish"};
            strlcpy(settings.shell, names[id - ID_SHELL], sizeof(settings.shell));
            settings_save();
            return;
        }
        if (id >= ID_FIELD && id < ID_FIELD + 5) return;
        if (id >= ID_SLEEP && id < ID_SLEEP + 6) { settings.sleep_min = SLEEP_CHOICES[id - ID_SLEEP]; settings_save(); return; }
        if (id >= ID_SAVER && id < ID_SAVER + 6) { settings.saver_min = SAVER_CHOICES[id - ID_SAVER]; settings_save(); return; }
        switch (id) {
        case ID_SAVE_NAMES: save_names(); break;
        case ID_PW_SET: set_password(); break;
        case ID_PW_REMOVE: remove_password(); break;
        case ID_LOCK_NOW: ui_lock(false); break;
        case ID_SLEEP_NOW: ui_lock(true); break;
        case ID_RESET: ask("erase every file and restore the starter files? this can't be undone.", "Erase", "Cancel", nullptr, reset_cb); break;
        case ID_ABOUT: launch("about"); break;
        case ID_FSCHECK + 0: settings.boot_target = settings.boot_target ? 0 : 1; settings_save(); break;
        case ID_FSCHECK + 1: settings.fast_boot = !settings.fast_boot; settings_save(); break;
        case ID_FSCHECK + 2: settings.net_enabled = !settings.net_enabled; settings_save(); break;
        }
    }

    int hit_at(int x, int y) {
        for (int i = hits.len - 1; i >= 0; i--) if (hits[i].r.contains(x, y)) return i;
        return -1;
    }

    void on_mouse(const MouseEv& e) override {
        TextField* f = focus >= 0 ? field(focus) : nullptr;
        if (e.kind == M_MOVE && e.left && f && f->anchor >= 0) {
            for (int i = 0; i < hits.len; i++)
                if (hits[i].id == ID_FIELD + focus) { f->drag(e.x - hits[i].r.x, hits[i].r.w); ui_invalidate(); break; }
            return;
        }
        if (e.kind != M_DOWN || e.button != 1) return;
        int h = hit_at(e.x, e.y);
        if (h < 0) { focus = -1; ui_invalidate(); return; }
        int id = hits[h].id;
        if (id >= ID_FIELD && id < ID_FIELD + 5) {
            focus = id - ID_FIELD;
            field(focus)->click(e.x - hits[h].r.x, hits[h].r.w, kbd_shift_down());
        } else {
            focus = -1;
            act(id);
        }
        ui_invalidate();
    }

    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        TextField* f = focus >= 0 ? field(focus) : nullptr;
        if (f) {
            if (e.key == K_ENTER) {
                if (page == 5) { save_names(); focus = -1; }
                else if (page == 4) set_password();
            } else if (e.key == K_TAB) {
                int lo = page == 5 ? 0 : 2, hi = page == 5 ? 1 : 4;
                if (!password_is_set() && page == 4) lo = 3;
                focus = focus >= hi ? lo : focus + 1;
                field(focus)->select_all();
            } else if (e.key == K_ESC) focus = -1;
            else f->on_key(e);
        } else if (e.key == K_DOWN) page = (page + 1) % NPAGES;
        else if (e.key == K_UP) page = (page + NPAGES - 1) % NPAGES;
        ui_invalidate();
    }

    bool wants_ibeam(int x, int y) override {
        for (int i = 0; i < hits.len; i++)
            if (hits[i].id >= ID_FIELD && hits[i].id < ID_FIELD + 5 && hits[i].r.contains(x, y)) return true;
        return false;
    }

    void tick() override {
        static u64 last = 0;
        if ((focus >= 0 || (msg[0] && ticks() - msg_t < 5200) || page == 1) && ticks() - last > 500) { last = ticks(); ui_invalidate(); }
    }
};

App* new_settings(const char*) { return new SettingsApp(); }
