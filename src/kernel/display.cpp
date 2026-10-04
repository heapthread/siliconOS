#include "display.h"
#include "settings.h"
#include "drv/drivers.h"
#include "arch/arch.h"

static void (*hooks[6])();
static int nhooks = 0;
static int mon_hz = 60;
static int cand_w = 0, cand_h = 0;
static u64 cand_since = 0, last_poll = 0;

void display_on_change(void (*fn)()) { if (nhooks < 6) hooks[nhooks++] = fn; }
int display_monitor_hz() { return mon_hz; }

int display_target_fps() {
    int f = settings.fps_limit;
    if (f <= 0) f = mon_hz;
    return clamp(f, 24, 1000);
}

bool display_apply(int w, int h) {
    int ow = fb.width, oh = fb.height;
    if (!fb_set_mode(w, h)) return false;
    if (fb.width == ow && fb.height == oh) return true;
    mouse_set_bounds(fb.width, fb.height);
    for (int i = 0; i < nhooks; i++) hooks[i]();
    return true;
}

static int bridge_w = 0, bridge_h = 0;
void display_bridge_size(int w, int h) { bridge_w = w; bridge_h = h; }
void display_bridge_hz(int hz) { if (hz >= 24 && hz <= 540) mon_hz = hz; }

static bool wished(int* w, int* h) {
    if (settings.res_w > 0 && settings.res_h > 0) { *w = settings.res_w; *h = settings.res_h; return true; }
    if (bridge_w > 0) { *w = bridge_w; *h = bridge_h; return true; }
    int hz;
    if (fb_edid_mode(w, h, &hz)) { mon_hz = hz; return true; }
    return false;
}

void display_init() {
    int w, h, hz;
    if (fb_edid_mode(&w, &h, &hz)) mon_hz = hz;
    if (wished(&w, &h)) display_apply(w, h);
    klog("display: %dx%d, monitor %d Hz, %s\n", fb.width, fb.height, mon_hz, fb_can_resize() ? "resizable" : "fixed");
}

void display_refresh_from_settings() {
    int w, h;
    if (wished(&w, &h)) display_apply(w, h);
}

void display_poll() {
    bridge_poll();
    u64 t = ticks();
    if (t - last_poll < 120) return;
    last_poll = t;
    int hz, ew, eh;
    if (fb_edid_mode(&ew, &eh, &hz)) {
        if (!bridge_w) mon_hz = hz;                    // keeps the refresh rate fresh as well
        static int lw, lh;
        if (ew != lw || eh != lh) { lw = ew; lh = eh; klog("edid: %dx%d @ %d Hz\n", ew, eh, hz); }
    }
    if (settings.res_w > 0) return;                    // a fixed resolution was chosen
    if (!fb_can_resize()) return;
    int w, h;
    if (!wished(&w, &h)) return;
    fb_limit_mode(&w, &h);
    if (w == fb.width && h == fb.height) { cand_w = cand_h = 0; return; }
    if (w != cand_w || h != cand_h) { cand_w = w; cand_h = h; cand_since = t; return; }
    if (t - cand_since < 250) return;                  // still being dragged
    cand_w = cand_h = 0;
    display_apply(w, h);
}
