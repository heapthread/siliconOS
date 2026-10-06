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
    void copy_selection() {
        if (!con.has_sel()) return;
        char* buf = (char*)kmalloc(con.cols * con.scroll_total() + 16);
        int n = con.sel_text(buf, con.cols * con.scroll_total() + 16);
        char* u = (char*)kmalloc(n * 4 + 8);
        silicon_to_utf8(buf, n, u, n * 4 + 8);
        clip_set(u, (u32)strlen(u));
        kfree(u);
        kfree(buf);
    }
    void paste_clipboard() {
        if (!clip_has() || sh.busy()) return;
        u32 n = clip_len();
        char* s = (char*)kmalloc(n + 8);
        utf8_to_silicon(clip_get(), n, s, n + 8);
        sh.paste(s);
        kfree(s);
        con.sel_clear();
    }
    void on_key(const KeyEvent& e) override {
        bool letter = e.key == 'c' || e.key == 'C' || e.key == 'v' || e.key == 'V' || e.key == 'a' || e.key == 'A';
        if (e.down && e.ctrl && letter && !e.alt) {
            char k = (char)(e.key | 0x20);
            if (k == 'c' && (e.shift || con.has_sel())) { copy_selection(); ui_invalidate(); return; }
            if (k == 'v') { paste_clipboard(); ui_invalidate(); return; }
            if (k == 'a' && e.shift) { con.sel_all(); ui_invalidate(); return; }
        }
        if (e.down && e.key != K_NONE && !e.ctrl && !e.alt) con.sel_clear();
        sh.on_key(e);
        ui_invalidate();
    }
    static void ctx_fn(void* ctx, int cmd) { ((App*)ctx)->on_command(cmd); }
    bool on_drop(const char* path, int, int) override {
        if (sh.busy()) return false;
        char b[270];
        snprintf(b, sizeof(b), "%s ", path);
        sh.paste(b);
        ui_invalidate();
        return true;
    }
    bool selecting = false;
    int click_n = 0;
    u64 click_t = 0;
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_WHEEL) { con.scroll_view(e.wheel * 3); ui_invalidate(); return; }
        int off = con.scroll_offset();
        if (sb.handle(e, cw - 10, 0, ch, con.scroll_total(), con.rows, off)) { con.scroll_to(off); ui_invalidate(); return; }
        int col = (e.x - 6) / con.cell_w(), row = (e.y - 5) / con.cell_h();
        if (e.kind == M_DOWN && e.button == 1) {
            u64 now = ticks();
            click_n = (now - click_t < 450 && click_n < 3) ? click_n + 1 : 1;
            click_t = now;
            if (click_n == 2) { con.sel_word(col, row); selecting = false; }
            else if (click_n == 3) { con.sel_line(row); selecting = false; }
            else { con.sel_begin(col, row); selecting = true; }
            ui_invalidate();
        } else if (e.kind == M_DBL && e.button == 1) {
            con.sel_word(col, row); selecting = false; ui_invalidate();
        } else if (e.kind == M_MOVE && selecting && e.left) {
            con.sel_update(col, row);
            ui_invalidate();
        } else if (e.kind == M_UP && e.button == 1) {
            selecting = false;
            if (!con.has_sel()) con.sel_clear();
            ui_invalidate();
        } else if (e.kind == M_UP && e.button == 2) {
            Menu m;
            m.add("Copy", 30, "ctrl+shift+c", con.has_sel()).add("Paste", 31, "ctrl+v", clip_has() && !sh.busy()).sep().add("Select all", 32).add("Clear", 2);
            Rect cl = win->client();
            menu_popup(&m, cl.x + e.x, cl.y + e.y, ctx_fn, this);
        } else if (e.kind == M_UP && e.button == 3) {
            paste_clipboard();
            ui_invalidate();
        }
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
        strlcpy(m[1].title, "Edit", sizeof(m[1].title));
        m[1].add("Copy", 30, "ctrl+shift+c", con.has_sel()).add("Paste", 31, "ctrl+v", clip_has() && !sh.busy()).sep().add("Select all", 32, "ctrl+shift+a");
        m[2].clear();
        strlcpy(m[2].title, "Theme", sizeof(m[2].title));
        for (int i = 0; i < console_theme_count; i++) m[2].add(console_themes[i].name, 10 + i, "", true, con.theme == i);
        m[3].clear();
        strlcpy(m[3].title, "Run", sizeof(m[3].title));
        m[3].add("fetch", 20).add("tree ~", 21).add("rain", 22).add("snake", 23).add("life", 24);
        return 4;
    }
    void on_command(int cmd) override {
        if (cmd == 1) launch("__new__", "terminal");
        else if (cmd == 2) { con.clear(); sh.start_prompt(); }
        else if (cmd == 3) close();
        else if (cmd == 30) copy_selection();
        else if (cmd == 31) paste_clipboard();
        else if (cmd == 32) con.sel_all();
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
        else if (r == SR_LOCK) ui_lock(false);
        else if (r == SR_SLEEP) ui_lock(true);
        else if (r == SR_SETUP) s->out("the firmware setup takes over the whole screen: leave the desktop first,\nor reboot and press DEL\n");
    }
    bool host_is_ui() override { return true; }
};

App* new_terminal(const char*) { return new TerminalApp(); }
