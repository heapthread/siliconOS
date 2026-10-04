// silicon - the greeter (login / lock screen) and the sleep screen
#include "desk.h"
#include "lockscreen.h"
#include "../arch/arch.h"
#include "../display.h"
#include "../net/crypto.h"
#include "../net/net.h"

// ================================================================ password
void password_hash(const char* pw, char out[72]) {
    char buf[200];
    snprintf(buf, sizeof(buf), "silicon-v1:%s:%s", settings.user, pw);
    u8 h[32];
    sha256(buf, (u32)strlen(buf), h);
    static const char* hx = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i * 2] = hx[h[i] >> 4]; out[i * 2 + 1] = hx[h[i] & 15]; }
    out[64] = 0;
}

bool password_is_set() { return settings.pass[0] != 0; }

bool password_check(const char* pw) {
    if (!password_is_set()) return true;
    char h[72];
    password_hash(pw, h);
    return !strcmp(h, settings.pass);
}

void password_set(const char* pw) {
    if (!pw || !pw[0]) settings.pass[0] = 0;
    else password_hash(pw, settings.pass);
    settings_save();
}

// ================================================================ shared bits
// a pixelated, darkened copy of the wallpaper: frosted glass the pixel-art way
static Image bg_cache;
static int bg_w = 0, bg_h = 0, bg_wall = -1;

static const Image& lock_background() {
    if (bg_cache.px && bg_w == fb.width && bg_h == fb.height && bg_wall == settings.wallpaper) return bg_cache;
    image_free(bg_cache);
    const Image& wall = ui_wallpaper_fit();
    Image small = image_new(wall.w, wall.h, 0);
    memcpy(small.px, wall.px, (size_t)wall.w * wall.h * 4);
    const int cell = 10;
    int sw = max(1, wall.w / cell), sh = max(1, wall.h / cell);
    image_shrink(small, sw, sh);
    bg_cache = image_new(wall.w, wall.h, 0xFF000000);
    for (int y = 0; y < wall.h; y++) {
        const u32* row = small.px + min(y / cell, small.h - 1) * small.w;
        u32* dst = bg_cache.px + y * wall.w;
        for (int x = 0; x < wall.w; x++) {
            u32 p = row[min(x / cell, small.w - 1)];
            // darken and tint toward violet
            u32 r = ((p >> 16) & 255) * 5 / 10, g = ((p >> 8) & 255) * 45 / 100, b = (p & 255) * 6 / 10 + 14;
            dst[x] = 0xFF000000 | (r << 16) | (g << 8) | min(b, 255u);
        }
    }
    image_free(small);
    bg_w = wall.w;
    bg_h = wall.h;
    bg_wall = settings.wallpaper;
    return bg_cache;
}

static void draw_moon(Canvas& c, int x, int y, int s, u32 col) {
    // a pixel crescent
    for (int j = -s; j <= s; j++)
        for (int i = -s; i <= s; i++) {
            if (i * i + j * j > s * s) continue;
            int ox = i - s / 2 - 1;
            if (ox * ox + j * j <= (s - 1) * (s - 1) * 9 / 10) continue;
            c.put(x + i, y + j, col);
        }
}

static const char* lock_weekday(int w) {
    static const char* n[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    return n[clamp(w, 0, 6)];
}

static int ease(int p) { return ease_out(p); }

// ================================================================ greeter
static void power_row(Canvas& c, int cx, int y, int hover) {
    // sleep, restart, shut down
    const char* tips[3] = {"Sleep", "Restart", "Shut down"};
    for (int i = 0; i < 3; i++) {
        int bx = cx - 60 + i * 44 - 16;
        bool hot = hover == i;
        c.fill_alpha(bx - 4, y - 4, 40, 40, hot ? 0xFFFFFFFF : 0xFF000000, hot ? 60 : 70);
        c.rect(bx - 4, y - 4, 40, 40, hot ? 0xFFFFFFFF : 0x00000000);
        if (i == 0) draw_moon(c, bx + 16, y + 16, 9, 0xFFFFF0C0);
        else c.blit(icon(i == 1 ? "restart" : "shutdown")->mid, bx, y);
        if (hot) c.text_centered(bx - 30, y + 40, 92, tips[i], 0xFFFFFFFF, 1, true);
    }
}

void greeter_run(bool at_boot) {
    TextField pw;
    pw.password = true;
    pw.focused = true;
    pw.text[0] = 0;
    const bool need_pw = password_is_set();
    u64 t0 = ticks(), shake_t0 = 0, leave_t0 = 0;
    bool wrong = false, leaving = false;
    int hover = -1;
    bool prev_left = false;
    MouseState ms;
    mouse_update(ms);
    int mx = ms.x, my = ms.y;
    int fw = fb.width, fh = fb.height;
    u64 last_frame = 0;
    (void)at_boot;

    for (;;) {
        // size changes while the greeter is up (the QEMU window got resized)
        display_poll();
        if (fb.width != fw || fb.height != fh) { fw = fb.width; fh = fb.height; }
        KeyEvent e;
        bool submit = false;
        while (kbd_poll(e)) {
            if (!e.down || leaving) continue;
            if (e.key == K_ENTER) submit = true;
            else if (need_pw) { pw.on_key(e); wrong = false; }
            else if (e.key == ' ') submit = true;
        }
        bool clicked = false;
        if (mouse_update(ms)) {
            mx = ms.x; my = ms.y;
            if (ms.left && !prev_left) clicked = true;
            prev_left = ms.left;
        }
        int cx = fw / 2, cy = fh / 2;
        int field_y = cy + 62;
        int pwr_y = fh - 80, pwr_cx = fw - 110;
        hover = -1;
        for (int i = 0; i < 3; i++)
            if (Rect{pwr_cx - 60 + i * 44 - 20, pwr_y - 8, 40, 44}.contains(mx, my)) hover = i;
        Rect go_btn = {cx - 70, field_y, 140, 32};
        if (clicked && !leaving) {
            if (hover == 0) { sleep_run(); t0 = ticks() - 400; }
            else if (hover == 1) { fs_sync(); power_reboot(); }
            else if (hover == 2) { fs_sync(); power_off(); }
            else if (!need_pw && (go_btn.contains(mx, my) || true)) submit = true;
            else if (need_pw && Rect{cx + 118, field_y, 32, 22}.contains(mx, my)) submit = true;
        }
        if (submit && !leaving) {
            if (password_check(pw.text)) { leaving = true; leave_t0 = ticks(); ui_beep(2); }
            else { wrong = true; shake_t0 = ticks(); pw.text[0] = 0; pw.len = pw.cur = 0; pw.anchor = -1; ui_beep(1); }
        }
        if (leaving && ticks() - leave_t0 > 450) break;

        // ---- frame pacing (30 fps is plenty here)
        u64 now = ticks();
        if (now - last_frame < 28) { net_poll(); cpu_idle(); continue; }
        last_frame = now;

        Canvas& c = fb.back;
        c.reset_clip();
        c.origin(0, 0);
        const Image& bg = lock_background();
        c.blit_raw(bg.px, fw, fh, fw, 0, 0);
        int el = (int)(now - t0);
        int in = ease(min(256, el * 256 / 600));                    // entrance 0..256
        int slide = (256 - in) * 24 / 256;
        // big clock
        DateTime d;
        rtc_read(d);
        char b[64];
        int hh = d.hour;
        if (!settings.clock24) { hh = hh % 12; if (!hh) hh = 12; }
        snprintf(b, sizeof(b), "%02d:%02d", hh, d.minute);
        int tw = text_width(b, 7, true);
        u32 a = (u32)in;
        c.text_shadow(cx - tw / 2, fh / 8 + slide, b, blend(0xFF000000, 0xFFFFFFFF, a), blend(0xFF000000, 0xFF241A44, a), 7, true);
        snprintf(b, sizeof(b), "%s, %d %s", lock_weekday(d.weekday), d.day, month_name(d.month));
        c.text_centered(0, fh / 8 + 90 + slide, fw, b, blend(0xFF000000, 0xFFD8CCF5, a), 2, true);

        // avatar: the chip in a glowing ring
        int pulse = (int)((now / 12) % 120);
        int glow = pulse < 60 ? pulse : 120 - pulse;
        int ay = cy - 70 + slide;
        for (int r = 62; r >= 52; r -= 2) c.fill_alpha(cx - r, ay - r, r * 2, r * 2, accent(), (u32)((66 - r) * 3 + glow / 3) * a / 256);
        c.fill_alpha(cx - 50, ay - 50, 100, 100, 0xFF150F28, 220 * a / 256);
        c.blit_scaled(icon("silicon")->big, cx - 40, ay - 40, 80, 80);
        // name
        c.text_centered(0, ay + 66, fw, settings.user, blend(0xFF000000, 0xFFFFFFFF, a), 2, true);

        // password field or sign-in pill
        int fx = field_y + slide;
        if (need_pw) {
            int sx = 0;
            if (shake_t0 && now - shake_t0 < 450) { int t = (int)(now - shake_t0); sx = ((t / 45) % 2 ? 1 : -1) * (14 - t / 32); }
            pw.focused = !leaving;
            pw.draw(c, cx - 110 + sx, fx, 220);
            bool hot = Rect{cx + 118 + sx, fx, 32, 22}.contains(mx, my);
            draw_button(c, cx + 118 + sx, fx, 32, 22, G_RIGHT, hot, true);
            if (wrong) c.text_centered(0, fx + 32, fw, "wrong password", 0xFFFF8CA0, 1, true);
            else c.text_centered(0, fx + 32, fw, "type your password and press enter", 0xFFB8ACD6);
        } else {
            bool hot = go_btn.contains(mx, my);
            draw_button(c, go_btn.x, go_btn.y + slide, go_btn.w, go_btn.h, "Sign in", hot, true);
            int pulse2 = (int)((now / 6) % 200);
            u32 ha = (u32)(110 + (pulse2 < 100 ? pulse2 : 200 - pulse2) / 2);
            c.text_centered(0, go_btn.y + 46 + slide, fw, "click anywhere or press enter", blend(0xFF000000, 0xFFC8BCEB, ha));
        }

        // corners
        c.text(24, fh - 40, settings.hostname, 0xFFE6DCFF, 2, true);
        c.text(24, fh - 20, "silicon " SILICON_VERSION, 0xFFA89CCB);
        {
            char ip[64], addr[24];
            if (netcfg.up && netcfg.configured) { ip_to_str(netcfg.ip, addr); snprintf(ip, sizeof(ip), "online " G_BULLET " %s", addr); }
            else snprintf(ip, sizeof(ip), "offline");
            c.text(fw - 24 - text_width(ip), 18, ip, 0xFFB8ACD6);
        }
        power_row(c, pwr_cx, pwr_y, hover);

        // draw the cursor (the desktop one isn't running)
        c.fill(mx, my, 2, 14, 0xFFFFFFFF);
        c.fill(mx, my, 10, 2, 0xFFFFFFFF);
        for (int i = 0; i < 8; i++) c.put(mx + i, my + i, 0xFFFFFFFF);

        // fades
        if (leaving) {
            int p = (int)min<u64>(255, (now - leave_t0) * 255 / 450);
            c.fill_alpha(0, 0, fw, fh, 0xFF000000, (u32)p);
        } else if (el < 250) c.fill_alpha(0, 0, fw, fh, 0xFF000000, (u32)(255 - el * 255 / 250));
        fb_present();
        net_poll();
    }
    bg_w = 0;                                    // let the cached background go on next use
    image_free(bg_cache);
    bg_cache = Image();
}

// ================================================================ sleep
void sleep_run() {
    int fw = fb.width, fh = fb.height;
    // fade to black with the screen as it is
    Canvas& c = fb.back;
    Image snap = image_new(fw, fh, 0);
    memcpy(snap.px, c.px, (size_t)fw * fh * 4);
    u64 t0 = ticks();
    for (;;) {
        u64 el = ticks() - t0;
        if (el > 500) break;
        c.blit_raw(snap.px, fw, fh, fw, 0, 0);
        c.fill_alpha(0, 0, fw, fh, 0xFF000000, (u32)(el * 255 / 500));
        fb_present();
        cpu_spin_us(8000);
    }
    image_free(snap);
    MouseState ms;
    mouse_update(ms);
    int mx0 = ms.x, my0 = ms.y;
    u64 last_draw = 0;
    // waking needs deliberate input: a bump of the mouse doesn't count, a key or a click does
    for (;;) {
        net_poll();
        KeyEvent e;
        bool wake = false;
        while (kbd_poll(e)) if (e.down) wake = true;
        if (mouse_update(ms)) {
            if (ms.left || ms.right || ms.wheel || iabs(ms.x - mx0) + iabs(ms.y - my0) > 24) wake = true;
        }
        if (wake) break;
        u64 now = ticks();
        if (now - last_draw >= 90) {
            last_draw = now;
            c.fill(0, 0, fw, fh, 0xFF000000);
            // a tiny drifting moon and a few z, dim enough not to light the room
            int cyc = (int)(now / 40 % 2400);
            int x = fw / 2 - 60 + (cyc % 1200) * 120 / 1200 * (cyc < 1200 ? 1 : -1) + (cyc < 1200 ? 0 : 120);
            int y = fh / 2 - 20 + ((cyc / 6) % 40 < 20 ? (cyc / 6) % 20 : 20 - (cyc / 6) % 20);
            draw_moon(c, x, y, 12, 0xFF3A3556);
            for (int i = 0; i < 3; i++) {
                int ph = (int)((now / 30 + i * 70) % 210);
                int zx = x + 18 + ph / 5, zy = y - 8 - ph / 4;
                u32 col = blend(0xFF000000, 0xFF4A4470, (u32)max(0, 200 - ph));
                c.text(zx, zy, "z", col, 1 + i, true);
            }
            // a power LED: the machine is on, just resting
            int br = (int)(now / 8 % 400);
            int lv = br < 200 ? br : 400 - br;
            c.fill(fw - 26, fh - 26, 6, 6, blend(0xFF000000, 0xFF8A6AFF, (u32)(20 + lv / 3)));
            fb_present();
        }
        cpu_idle();
    }
    // fade back in
    t0 = ticks();
    for (;;) {
        u64 el = ticks() - t0;
        if (el > 300) break;
        c.fill(0, 0, fw, fh, 0xFF000000);
        c.fill_alpha(0, 0, fw, fh, 0xFF0A0814, (u32)(el * 120 / 300));
        fb_present();
        cpu_spin_us(8000);
    }
    // swallow the key that woke us up
    KeyEvent e;
    while (kbd_poll(e)) {}
}

void lock_after_wake() {
    if (settings.greeter && password_is_set()) greeter_run(false);
}
