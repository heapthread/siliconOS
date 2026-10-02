// silicon - Terminal app: the shell in a window
#include "apps.h"
#include "../../term/shell.h"

struct TerminalApp : App, ShellHost {
    Console con;
    Shell sh;

    TerminalApp() {
        con.scale = 1;
        con.theme = clamp(settings.term_theme, 0, console_theme_count - 1);
        con.init(78, 32);
        sh.init(&con, this);
        sh.banner();
        sh.start_prompt();
    }
    ~TerminalApp() { con.free(); }

    const char* app_id() override { return "terminal"; }
    const char* title() override { return "Terminal"; }
    int pref_w() override { return 78 * 8 + 12; }
    int pref_h() override { return 32 * 12 + 10; }
    u32 background() override { return con.color(0); }

    void draw(Canvas& c, int w, int h) override {
        bool focused = win == wm_focused();
        con.render(c, 6, 5, focused);
        if (con.scroll_total() > con.rows)
            sb.draw(c, w - 10, 0, h, con.scroll_total(), con.rows, con.scroll_offset());
    }
    ScrollBar sb;
    int cw = 0, ch = 0;
    bool hover_effects() override { return false; }
    void on_resize(int w, int h) override { cw = w; ch = h; con.resize((w - 22) / 8, (h - 10) / 12); }
    void on_key(const KeyEvent& e) override { sh.on_key(e); ui_invalidate(); }
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_WHEEL) { con.scroll_view(e.wheel * 3); ui_invalidate(); return; }
        int off = con.scroll_offset();
        if (sb.handle(e, cw - 10, 0, ch, con.scroll_total(), con.rows, off)) { con.scroll_to(off); ui_invalidate(); }
    }
    void tick() override {
        sh.tick();
        if (con.dirty) ui_invalidate();
    }
    int menus(Menu* m) override {
        m[0].clear();
        strlcpy(m[0].title, "Shell", sizeof(m[0].title));
        m[0].add("New window", 1, "ctrl+alt+t").add("Clear", 2, "ctrl+l").sep().add("Close", 3, "ctrl+w");
        m[1].clear();
        strlcpy(m[1].title, "Theme", sizeof(m[1].title));
        for (int i = 0; i < console_theme_count; i++) m[1].add(console_themes[i].name, 10 + i, "", true, con.theme == i);
        m[2].clear();
        strlcpy(m[2].title, "Run", sizeof(m[2].title));
        m[2].add("fetch", 20).add("tree ~", 21).add("rain", 22).add("snake", 23).add("life", 24);
        return 3;
    }
    void on_command(int cmd) override {
        if (cmd == 1) launch("__new__", "terminal");
        else if (cmd == 2) { con.clear(); sh.start_prompt(); }
        else if (cmd == 3) close();
        else if (cmd >= 10 && cmd < 20) { con.theme = cmd - 10; settings.term_theme = con.theme; settings_save(); con.dirty = true; }
        else if (cmd >= 20 && !sh.busy()) {
            const char* cmds[] = {"fetch", "tree ~", "rain", "snake", "life"};
            con.write(cmds[cmd - 20]);
            con.write("\n");
            sh.run_line(cmds[cmd - 20]);
            if (!sh.busy()) sh.start_prompt();
        }
        ui_invalidate();
    }

    void shell_request(Shell* s, ShellRequest r, const char* arg) override {
        if (r == SR_EXIT) force_close();
        else if (r == SR_OPEN) {
            if (fs_lookup(arg)) open_path(arg);
            else if (!launch(arg)) s->outf("open: nothing called %s\n", arg);
        } else if (r == SR_WALLPAPER) ui_wallpaper_command(s, arg);
        else if (r == SR_SETUP) s->out("the firmware setup takes over the whole screen: leave the desktop first,\nor reboot and press DEL\n");
    }
    bool host_is_ui() override { return true; }
};

App* new_terminal(const char*) { return new TerminalApp(); }
