// silicon - Monitor: running apps and live performance graphs
#include "apps.h"
#include "../../arch/arch.h"

struct MonitorApp : App {
    int tab = 1;
    int sel = -1;
    u8 cpu[90], mem[90];
    int samples = 0;
    u64 last_t = 0, last_idle = 0, last_ticks = 0;
    int cw = 0, ch = 0;

    MonitorApp() { memset(cpu, 0, sizeof(cpu)); memset(mem, 0, sizeof(mem)); last_ticks = ticks(); last_idle = idle_ticks(); }
    const char* app_id() override { return "monitor"; }
    const char* title() override { return "Monitor"; }
    int pref_w() override { return 560; }
    int pref_h() override { return 380; }
    void on_resize(int w, int h) override { cw = w; ch = h; }

    void sample() {
        u64 t = ticks(), idle = idle_ticks();
        u64 dt = t - last_ticks, di = idle - last_idle;
        int busy = dt ? 100 - (int)(di * 100 / dt) : 0;
        last_ticks = t; last_idle = idle;
        memmove(cpu, cpu + 1, sizeof(cpu) - 1);
        memmove(mem, mem + 1, sizeof(mem) - 1);
        cpu[sizeof(cpu) - 1] = (u8)clamp(busy, 0, 100);
        mem[sizeof(mem) - 1] = (u8)(heap_used() * 100 / max<u64>(1, heap_total()));
        samples++;
    }
    void tick() override {
        if (ticks() - last_t >= 500) { last_t = ticks(); sample(); ui_invalidate(); }
    }

    void graph(Canvas& c, int x, int y, int w, int h, u8* data, u32 col, const char* label, const char* value) {
        c.fill(x, y, w, h, 0xFF16121F);
        c.rect(x, y, w, h, th::ink);
        for (int gx = x + w; gx > x; gx -= 20) c.vline(gx, y + 1, h - 2, 0xFF221C30);
        for (int gy = y + h / 4; gy < y + h; gy += h / 4) c.hline(x + 1, gy, w - 2, 0xFF221C30);
        int n = sizeof(cpu);
        int bw = max(1, (w - 2) / n);
        for (int i = 0; i < n; i++) {
            int v = data[i] * (h - 4) / 100;
            int bx = x + 1 + i * bw;
            if (v) {
                c.fill(bx, y + h - 2 - v, bw, v, blend(0xFF16121F, col, 90));
                c.fill(bx, y + h - 2 - v, bw, 2, col);
            }
        }
        c.text(x + 8, y + 6, label, 0xFFB8AED0, 1, true);
        c.text(x + w - text_width(value, 2, true) - 8, y + 6, value, col, 2, true);
    }

    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        // tabs
        c.fill(0, 0, w, 32, th::panel);
        c.hline(0, 31, w, th::border);
        const char* tabs[2] = {"Apps", "Performance"};
        for (int i = 0; i < 2; i++) {
            Rect r{10 + i * 110, 6, 100, 26};
            if (tab == i) { c.fill(r.x, r.y, r.w, r.h, th::paper); c.rect(r.x, r.y, r.w, r.h + 1, th::border); c.hline(r.x + 1, r.y + r.h, r.w - 2, th::paper); }
            c.text_centered(r.x, r.y + 7, r.w, tabs[i], tab == i ? th::text : th::muted, 1, tab == i);
        }
        if (tab == 0) {
            c.fill(10, 42, w - 20, 20, th::panel);
            c.text(46, 46, "app", th::muted, 1, true);
            c.text(w - 260, 46, "state", th::muted, 1, true);
            c.text(w - 150, 46, "running for", th::muted, 1, true);
            int n = wm_count();
            for (int i = 0; i < n; i++) {
                Window* win2 = wm_get(n - 1 - i);
                int y = 64 + i * 24;
                if (y > h - 60) break;
                bool s = i == sel;
                if (s) c.fill(10, y, w - 20, 24, accent());
                else if (i % 2) c.fill(10, y, w - 20, 24, 0xFFE6E1F0);
                c.blit(icon(win2->app->icon_name())->small, 18, y + 4);
                c.text_n(46, y + 6, win2->title, (w - 330) / 8, s ? th::white : th::text);
                c.text(w - 260, y + 6, win2->minimized ? "minimized" : (win2 == wm_focused() ? "focused" : "running"), s ? th::white : th::muted);
                u64 sec = ticks() > win2->opened_at ? (ticks() - win2->opened_at) / 1000 : 0;
                char t[24];
                snprintf(t, sizeof(t), "%um %02us", (u32)(sec / 60), (u32)(sec % 60));
                c.text(w - 150, y + 6, t, s ? th::white : th::muted);
            }
            bool can = sel >= 0 && sel < n;
            draw_button(c, w - 230, h - 34, 100, 24, "Switch to", hot(w - 230, h - 34, 100, 24), false, can);
            draw_button(c, w - 120, h - 34, 100, 24, "End task", hot(w - 120, h - 34, 100, 24), true, can);
            char st[48];
            snprintf(st, sizeof(st), "%d app%s running", n, n == 1 ? "" : "s");
            c.text(14, h - 28, st, th::muted);
            return;
        }
        char v[32];
        snprintf(v, sizeof(v), "%d%%", cpu[sizeof(cpu) - 1]);
        int gw = w - 20;
        graph(c, 10, 42, gw, 110, cpu, accent(), "cpu (busy)", v);
        char used[24];
        fmt_bytes(heap_used(), used, sizeof(used));
        graph(c, 10, 160, gw, 90, mem, 0xFF4FD1C5, "memory (heap)", used);
        // facts
        int y = 262;
        char b[96];
        u64 s = ticks() / 1000;
        auto fact = [&](int x, const char* k, const char* val) {
            c.text(x, y, k, th::muted, 1, true);
            c.text(x + 86, y, val, th::text);
        };
        snprintf(b, sizeof(b), "%uh %um %us", (u32)(s / 3600), (u32)(s / 60 % 60), (u32)(s % 60));
        fact(14, "uptime", b);
        char tot[24];
        fmt_bytes(heap_total(), tot, sizeof(tot));
        snprintf(b, sizeof(b), "%s free of %s", tot, used);
        snprintf(b, sizeof(b), "%s / %s", used, tot);
        fact(w / 2, "heap", b);
        y += 18;
        snprintf(b, sizeof(b), "%u MB", (u32)(mem_total_bytes >> 20));
        fact(14, "ram", b);
        char du[24], dc[24];
        fmt_bytes(fs_disk_used(), du, sizeof(du));
        fmt_bytes(fs_disk_capacity(), dc, sizeof(dc));
        snprintf(b, sizeof(b), "%s / %s", du, dc);
        fact(w / 2, "disk", b);
        y += 18;
        snprintf(b, sizeof(b), "%d nodes", fs_count(fs_root()));
        fact(14, "files", b);
        snprintf(b, sizeof(b), "%d", wm_count());
        fact(w / 2, "windows", b);
        y += 18;
        snprintf(b, sizeof(b), "%.30s", cpu_brand());
        fact(14, "cpu", b);
    }

    void on_mouse(const MouseEv& e) override {
        if (e.kind != M_DOWN && e.kind != M_UP && e.kind != M_DBL) return;
        if (e.kind == M_DOWN) {
            for (int i = 0; i < 2; i++) if (Rect{10 + i * 110, 6, 100, 26}.contains(e.x, e.y)) tab = i;
            if (tab == 0 && e.y >= 64 && e.y < ch - 60) sel = (e.y - 64) / 24;
        }
        int n = wm_count();
        bool can = sel >= 0 && sel < n;
        if (tab == 0 && can) {
            Window* target = wm_get(n - 1 - sel);
            if (e.kind == M_DBL || (e.kind == M_UP && Rect{cw - 230, ch - 34, 100, 24}.contains(e.x, e.y))) wm_focus(target);
            else if (e.kind == M_UP && Rect{cw - 120, ch - 34, 100, 24}.contains(e.x, e.y)) {
                if (target->app == this) close();
                else target->app->force_close();
                sel = -1;
            }
        }
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        if (e.key == K_TAB) tab ^= 1;
        if (e.key == K_DOWN) sel++;
        if (e.key == K_UP && sel > 0) sel--;
        ui_invalidate();
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "View", 24);
        m[0].add("Apps", 1, "", true, tab == 0).add("Performance", 2, "", true, tab == 1);
        return 1;
    }
    void on_command(int cmd) override { tab = cmd - 1; ui_invalidate(); }
};

App* new_monitor(const char*) { return new MonitorApp(); }
