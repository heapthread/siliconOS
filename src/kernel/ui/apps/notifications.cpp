// silicon - Notifications: the history of everything silicon has told you, do not disturb, per-app muting
#include "apps.h"

struct NotificationsApp : App {
    Smooth sc;
    ScrollBar sb;
    int ch = 0, cw = 0;
    static const int BAR = 38, ROW = 58;

    const char* app_id() override { return "notifications"; }
    const char* title() override { return "Notifications"; }
    int pref_w() override { return 400; }
    int pref_h() override { return 470; }

    int content_h() { return notif_count() * ROW + 10; }
    // newest first
    const NotifRec* rec(int row) { return notif_get(notif_count() - 1 - row); }
    void tick() override {
        if (sc.step()) ui_invalidate();
        if (notif_unseen()) { notif_mark_seen(); ui_invalidate(); }
        static int last_n = -1;
        if (last_n != notif_count()) { last_n = notif_count(); ui_invalidate(); }
    }
    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        c.fill(0, 0, w, BAR, th::panel);
        c.hline(0, BAR - 1, w, th::border);
        draw_checkbox(c, 10, 11, "Do not disturb", settings.dnd);
        draw_button(c, w - 84, 7, 74, 24, "Clear all", hot(w - 84, 7, 74, 24), false, notif_count() > 0);
        c.set_clip({0, BAR, w - 11, h - BAR});
        int n = notif_count();
        if (!n) c.text_centered(0, BAR + 80, w, "no notifications", th::muted, 1, true);
        for (int r = 0; r < n; r++) {
            const NotifRec* x = rec(r);
            int y = BAR + 4 + r * ROW - sc.pos();
            if (y + ROW < BAR || y > h) continue;
            bool on = hot(0, y, w - 11, ROW);
            c.fill(8, y, w - 27, ROW - 4, on ? th::panel : 0xFFF6F2FC);
            c.rect(8, y, w - 27, ROW - 4, 0xFFD8D2E8);
            c.fill(8, y, 3, ROW - 4, notif_muted(x->icon) ? th::muted : accent());
            c.blit(icon(x->icon)->mid, 18, y + 10);
            c.text_n(58, y + 7, x->title, (w - 160) / 8, th::text, 1, true);
            DateTime dt;
            stamp_to_datetime(x->stamp, dt);
            char tm[16];
            snprintf(tm, sizeof(tm), "%02d:%02d", dt.hour, dt.minute);
            c.text(w - 27 - text_width(tm), y + 7, tm, th::muted);
            c.text_n(58, y + 23, x->body, (w - 90) / 8, th::muted);
            if (on) {
                bool m = notif_muted(x->icon);
                c.text(58, y + 38, m ? "unmute this app" : "mute this app", hot(58, y + 34, 120, 14) ? accent() : th::muted);
                c.text(w - 44, y + 38, "x", hot(w - 52, y + 34, 24, 14) ? th::red : th::muted, 1, true);
            }
        }
        c.set_clip({0, 0, w, h});
        sb.draw(c, w - 10, BAR, h - BAR, content_h(), h - BAR, sc.pos());
    }
    void on_mouse(const MouseEv& e) override {
        int off = sc.target;
        if (sb.handle(e, cw - 10, BAR, ch - BAR, content_h(), ch - BAR, off)) { sc.jump(off); ui_invalidate(); return; }
        if (e.kind == M_WHEEL) { sc.set(clamp(sc.target - e.wheel * 56, 0, max(0, content_h() - (ch - BAR)))); ui_invalidate(); return; }
        if (e.kind == M_MOVE) { ui_invalidate(); return; }
        if (e.kind != M_UP || e.button != 1) return;
        if (e.y < BAR) {
            if (Rect{10, 8, 150, 22}.contains(e.x, e.y)) { settings.dnd = !settings.dnd; settings_save(); }
            else if (Rect{cw - 84, 7, 74, 24}.contains(e.x, e.y)) notif_clear();
            ui_invalidate();
            return;
        }
        int r = (e.y - BAR - 4 + sc.pos()) / ROW;
        const NotifRec* x = r >= 0 && r < notif_count() ? rec(r) : nullptr;
        if (!x) return;
        int y = BAR + 4 + r * ROW - sc.pos();
        if (Rect{cw - 52, y + 34, 24, 14}.contains(e.x, e.y)) notif_remove(notif_count() - 1 - r);
        else if (Rect{58, y + 34, 120, 14}.contains(e.x, e.y)) { char src[24]; strlcpy(src, x->icon, sizeof(src)); notif_set_muted(src, !notif_muted(src)); }
        ui_invalidate();
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "Notifications", 24);
        m[0].add("Do not disturb", 1, "", true, settings.dnd).add("Clear all", 2, "", notif_count() > 0);
        return 1;
    }
    void on_command(int cmd) override {
        if (cmd == 1) { settings.dnd = !settings.dnd; settings_save(); }
        if (cmd == 2) notif_clear();
        ui_invalidate();
    }
};

App* new_notifications(const char*) { return new NotificationsApp(); }
