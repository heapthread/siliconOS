// silicon - Clock: analog face, big pixel digits and a stopwatch
#include "apps.h"

// sin * 1024 for whole degrees
int isin(int deg) {
    static const short t[91] = {0,18,36,54,71,89,107,125,143,160,178,195,213,230,248,265,282,299,316,333,350,367,384,400,
        416,433,449,465,481,496,512,527,543,558,573,587,602,616,630,644,658,672,685,698,711,724,737,749,761,773,784,796,
        807,818,828,839,849,859,868,878,887,896,904,912,920,928,935,943,949,956,962,968,974,979,984,989,994,998,1002,1005,
        1008,1011,1014,1016,1018,1020,1022,1023,1023,1024,1024};
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg <= 90) return t[deg];
    if (deg <= 180) return t[180 - deg];
    if (deg <= 270) return -t[deg - 180];
    return -t[360 - deg];
}
int icos(int deg) { return isin(deg + 90); }

struct ClockApp : App {
    bool sw_running = false;
    u64 sw_start = 0, sw_acc = 0, last_poll = 0;
    int last_sec = -1;
    const char* app_id() override { return "clock"; }
    const char* title() override { return "Clock"; }
    int pref_w() override { return 520; }
    int pref_h() override { return 300; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF1C1630; }

    void hand(Canvas& c, int cx, int cy, int deg, int len, int thick, u32 col) {
        int x = cx + isin(deg) * len / 1024, y = cy - icos(deg) * len / 1024;
        for (int t = 0; t < thick; t++) { c.line(cx + t, cy, x + t, y, col); c.line(cx, cy + t, x, y + t, col); }
    }

    void draw(Canvas& c, int w, int h) override {
        DateTime d;
        rtc_read(d);
        int cx = 116, cy = 140, r = 96;
        c.circle(cx, cy, r + 5, 0xFF3A3058, true);
        c.circle(cx, cy, r, 0xFFF0ECFA, true);
        for (int i = 0; i < 60; i++) {
            int x = cx + isin(i * 6) * (r - 7) / 1024, y = cy - icos(i * 6) * (r - 7) / 1024;
            if (i % 5 == 0) c.fill(x - 1, y - 1, 3, 3, 0xFF4A3F6A);
            else c.put(x, y, 0xFFB0A6CC);
        }
        hand(c, cx, cy, (d.hour % 12) * 30 + d.minute / 2, r - 44, 3, th::ink);
        hand(c, cx, cy, d.minute * 6, r - 20, 2, th::ink);
        hand(c, cx, cy, d.second * 6, r - 12, 1, th::red);
        c.fill(cx - 2, cy - 2, 5, 5, th::red);

        char t[24];
        int hr = d.hour;
        if (!settings.clock24) { hr %= 12; if (!hr) hr = 12; }
        snprintf(t, sizeof(t), "%02d:%02d", hr, d.minute);
        c.text(240, 26, t, th::white, 5, true);
        snprintf(t, sizeof(t), ":%02d", d.second);
        c.text(240 + 5 * 40 + 4, 62, t, accent(), 2, true);
        char ds[48];
        snprintf(ds, sizeof(ds), "%s, %s %d %d", weekday_name(d.weekday), month_name(d.month), d.day, d.year);
        c.text(244, 96, ds, 0xFFB8AED0, 1, true);

        u64 el = sw_acc + (sw_running ? ticks() - sw_start : 0);
        snprintf(t, sizeof(t), "%02u:%02u.%02u", (u32)(el / 60000), (u32)(el / 1000 % 60), (u32)(el / 10 % 100));
        c.fill(240, 136, 260, 120, 0xFF2A2242);
        c.rect(240, 136, 260, 120, 0xFF4A3F6A);
        c.text(252, 146, "stopwatch", 0xFF9C92B8);
        c.text(252, 168, t, th::white, 3, true);
        draw_button(c, 252, 216, 112, 26, sw_running ? "Stop" : "Start", hot(252, 216, 112, 26), true);
        draw_button(c, 376, 216, 112, 26, "Reset", hot(376, 216, 112, 26));
        c.text(244, h - 22, "space starts/stops, r resets", 0xFF6A6088);
    }
    void toggle() {
        if (sw_running) { sw_acc += ticks() - sw_start; sw_running = false; }
        else { sw_start = ticks(); sw_running = true; }
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_UP) {
            if (Rect{252, 216, 112, 26}.contains(e.x, e.y)) toggle();
            if (Rect{376, 216, 112, 26}.contains(e.x, e.y)) { sw_running = false; sw_acc = 0; }
        }
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        if (e.key == ' ') toggle();
        if (e.key == 'r') { sw_running = false; sw_acc = 0; }
        ui_invalidate();
    }
    void tick() override {
        if (sw_running) ui_invalidate();
        if (ticks() - last_poll > 250) {
            last_poll = ticks();
            DateTime d;
            rtc_read(d);
            if (d.second != last_sec) { last_sec = d.second; ui_invalidate(); }
        }
    }
};
App* new_clock(const char*) { return new ClockApp(); }
