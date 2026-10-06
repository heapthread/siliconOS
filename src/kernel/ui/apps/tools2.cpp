// silicon - utilities: Capture, Weather, Typing, Network, Archive
#include "apps.h"
#include "../../net/net.h"

u8* deflate_decompress(const u8* in, u32 inlen, u32* outlen, u32 max_out);   // gfx/imgdec.cpp

static const int BAR = 34;
static void toolbar(Canvas& c, int w, const char* icon_id, const char* label) {
    c.fill(0, 0, w, BAR, th::panel);
    c.hline(0, BAR - 1, w, th::border);
    c.blit(icon(icon_id)->small, 10, 9);
    c.text(34, 11, label, th::text, 1, true);
}

// ================================================================ Capture
struct CaptureApp : App {
    int delay_s = 3;
    int state = 0;                 // 0 idle, 1 counting down, 2 grab
    u64 t_start = 0;
    char last_path[96] = "";
    int shots = 0;
    u32 last_w = 0, last_h = 0, last_size = 0;
    Image preview;

    ~CaptureApp() { if (preview.px) kfree(preview.px); }
    const char* app_id() override { return "capture"; }
    const char* title() override { return "Capture"; }
    int pref_w() override { return 440; }
    int pref_h() override { return 320; }
    bool resizable() override { return false; }

    void start() {
        state = 1; t_start = ticks();
        if (delay_s > 0 || true) wm_minimize(win);
    }
    static void put32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
    void grab() {
        const Canvas& b = fb.back;
        int w = b.w, h = b.h;
        u32 row = (u32)((w * 3 + 3) & ~3);
        u32 size = 54 + row * h;
        u8* buf = (u8*)kmalloc_try(size);
        if (!buf) { ui_notify("Capture", "out of memory", "capture"); return; }
        memset(buf, 0, 54);
        buf[0] = 'B'; buf[1] = 'M';
        put32(buf + 2, size); put32(buf + 10, 54); put32(buf + 14, 40);
        put32(buf + 18, w); put32(buf + 22, h); buf[26] = 1; buf[28] = 24;
        put32(buf + 34, row * h);
        for (int y = 0; y < h; y++) {
            u8* d = buf + 54 + (u32)(h - 1 - y) * row;
            const u32* s = b.px + y * b.pitch;
            for (int x = 0; x < w; x++) { u32 p = s[x]; d[x * 3] = (u8)p; d[x * 3 + 1] = (u8)(p >> 8); d[x * 3 + 2] = (u8)(p >> 16); }
        }
        fs_mkdirs("/home/pictures");
        char name[FS_NAME_MAX];
        fs_unique_name(fs_lookup("/home/pictures"), "capture.bmp", name, sizeof(name));
        snprintf(last_path, sizeof(last_path), "/home/pictures/%s", name);
        FsNode* f = fs_create_path(last_path, false);
        bool ok = f && fs_write(f, buf, size);
        kfree(buf);
        if (!ok) { ui_notify("Capture", "no room to save", "capture"); last_path[0] = 0; return; }
        shots++;
        last_w = w; last_h = h; last_size = size;
        if (preview.px) { kfree(preview.px); preview = Image(); }
        Image img = image_new(w, h, 0);
        if (img.px) {
            for (int y = 0; y < h; y++) memcpy(img.px + y * w, b.px + y * b.pitch, (size_t)w * 4);
            image_shrink(img, 340, 190);
            preview = img;
        }
        ui_beep(2);
        ui_notify("Screenshot saved", last_path, "capture");
    }
    void tick() override {
        if (state == 1) {
            u64 el = ticks() - t_start;
            if (el >= (u64)delay_s * 1000 + 450) { state = 2; grab(); state = 0; wm_focus(win); ui_invalidate_all(); }
        }
    }
    void draw(Canvas& c, int w, int h) override {
        toolbar(c, w, "capture", "screen capture");
        c.text(14, BAR + 14, "delay", th::muted);
        for (int i = 0; i < 4; i++) {
            static const int ds[4] = {0, 3, 5, 10};
            char b[8];
            snprintf(b, sizeof(b), ds[i] ? "%ds" : "now", ds[i]);
            draw_button(c, 60 + i * 52, BAR + 10, 48, 22, b, hot(60 + i * 52, BAR + 10, 48, 22), delay_s == ds[i]);
        }
        draw_button(c, w - 130, BAR + 10, 116, 22, "Take screenshot", hot(w - 130, BAR + 10, 116, 22), true);
        int py = BAR + 46;
        c.fill(16, py, w - 32, 200, 0xFF2A2440);
        if (preview.px) c.blit(preview, 16 + (w - 32 - preview.w) / 2, py + (200 - preview.h) / 2);
        else c.text_centered(16, py + 90, w - 32, "your screenshot shows up here", 0xFF8A80B8);
        if (last_path[0]) {
            char b[120];
            snprintf(b, sizeof(b), "%dx%d  " G_BULLET "  %s", last_w, last_h, last_path);
            c.text_n(16, py + 208, b, (w - 32) / 8, th::muted);
            draw_button(c, w - 110, h - 34, 96, 22, "Open", hot(w - 110, h - 34, 96, 22));
        }
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind != M_UP || e.button != 1) return;
        static const int ds[4] = {0, 3, 5, 10};
        for (int i = 0; i < 4; i++) if (Rect{60 + i * 52, BAR + 10, 48, 22}.contains(e.x, e.y)) delay_s = ds[i];
        int w = win ? win->client().w : 380, h = win ? win->client().h : 320;
        if (Rect{w - 130, BAR + 10, 116, 22}.contains(e.x, e.y)) start();
        if (last_path[0] && Rect{w - 110, h - 34, 96, 22}.contains(e.x, e.y)) open_path(last_path);
        ui_invalidate();
    }
};

// ================================================================ Weather
struct WeatherApp : App {
    TextField city;
    HttpRequest* req = nullptr;
    char fields[10][64];
    bool have = false;
    char status[100] = "type a city and press Enter";
    u64 t0 = 0;

    WeatherApp() {
        FsNode* f = fs_lookup("/home/.config/weather");
        city.set(f ? fs_read_text(f) : "London");
        for (char* p = city.text; *p; p++) if (*p == '\n') { *p = 0; city.len = (int)(p - city.text); city.cur = city.len; break; }
        city.focused = true;
        fetch();
    }
    ~WeatherApp() { if (req) http_free(req); }
    const char* app_id() override { return "weather"; }
    const char* title() override { return "Weather"; }
    int pref_w() override { return 440; }
    int pref_h() override { return 340; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF1C2A44; }

    void fetch() {
        if (!city.len) return;
        if (req) http_free(req);
        char enc[200], url[400];
        url_encode(city.text, enc, sizeof(enc));
        snprintf(url, sizeof(url), "http://wttr.in/%s?format=%%l|%%C|%%t|%%f|%%w|%%h|%%p|%%P|%%S|%%s", enc);
        req = http_get(url);
        http_set_user_agent(req, "curl/8.5.0 (silicon)");
        t0 = ticks();
        have = false;
        strlcpy(status, "fetching" G_ELLIP, sizeof(status));
        fs_mkdirs("/home/.config");
        fs_write_text("/home/.config/weather", city.text);
        ui_invalidate();
    }
    void tick() override {
        if (!req) return;
        int ph = http_step(req);
        ui_invalidate();
        if (ph == HTTP_ERROR) { snprintf(status, sizeof(status), "can't reach the weather service"); http_free(req); req = nullptr; return; }
        if (ph != HTTP_DONE) { if (ticks() - t0 > 20000) { http_free(req); req = nullptr; strlcpy(status, "timed out", sizeof(status)); } return; }
        u32 len;
        const u8* body = http_body(req, &len);
        char* txt = (char*)kmalloc(len + 1);
        memcpy(txt, body, len);
        txt[len] = 0;
        char conv[600];
        utf8_to_silicon(txt, min(len, 500u), conv, sizeof(conv));
        kfree(txt);
        int status_code = http_status(req);
        http_free(req);
        req = nullptr;
        int n = 0;
        const char* p = conv;
        while (n < 10) {
            const char* e = p;
            while (*e && *e != '|' && *e != '\n') e++;
            int l = min((int)(e - p), 63);
            memcpy(fields[n], p, l);
            fields[n][l] = 0;
            n++;
            if (*e != '|') break;
            p = e + 1;
        }
        if (n < 8 || status_code >= 400) { snprintf(status, sizeof(status), "no forecast for that place (http %d)", status_code); return; }
        have = true;
        status[0] = 0;
    }
    void draw(Canvas& c, int w, int h) override {
        toolbar(c, w, "weather", "weather");
        city.focused = true;
        city.draw(c, 14, BAR + 10, w - 28 - 70);
        draw_button(c, w - 84, BAR + 9, 70, 22, "Go", hot(w - 84, BAR + 9, 70, 22), true);
        if (!have) { c.text_centered(0, h / 2 + 10, w, status, 0xFFB8C8F0, 1, true); return; }
        // condition icon
        const char* cond = fields[1];
        int ix = 56, iy = BAR + 100;
        bool sun = str_icontains(cond, "sun") || str_icontains(cond, "clear");
        bool rain = str_icontains(cond, "rain") || str_icontains(cond, "drizzle") || str_icontains(cond, "shower");
        bool snow = str_icontains(cond, "snow") || str_icontains(cond, "sleet");
        bool cloud = !sun || str_icontains(cond, "cloud") || str_icontains(cond, "overcast") || rain || snow || str_icontains(cond, "fog") || str_icontains(cond, "mist");
        if (sun) { c.circle(ix, iy, 24, 0xFFFFD84A, true); for (int a = 0; a < 8; a++) { int dx = (a % 4 == 0 ? 1 : a % 4 == 2 ? -1 : 0) * 36, dy = (a % 4 == 1 ? 1 : a % 4 == 3 ? -1 : 0) * 36; c.line(ix + dx * 3 / 4, iy + dy * 3 / 4, ix + dx, iy + dy, 0xFFFFD84A); } }
        if (cloud) {
            int ox = sun ? 18 : 0, oy = sun ? 14 : 0;
            c.circle(ix - 12 + ox, iy + oy, 16, 0xFFE0E8F8, true); c.circle(ix + 8 + ox, iy - 8 + oy, 20, 0xFFE0E8F8, true);
            c.circle(ix + 26 + ox, iy + 2 + oy, 14, 0xFFE0E8F8, true); c.fill(ix - 12 + ox, iy + oy, 40, 16, 0xFFE0E8F8);
        }
        if (rain) for (int k = 0; k < 4; k++) c.line(ix - 12 + k * 14, iy + 28, ix - 18 + k * 14, iy + 42, 0xFF6AA8FF);
        if (snow) for (int k = 0; k < 4; k++) c.fill(ix - 14 + k * 14, iy + 32 + (k % 2) * 6, 4, 4, th::white);
        c.text(120, BAR + 62, fields[0], 0xFFB8C8F0, 1, true);
        c.text(120, BAR + 82, fields[2], th::white, 4, true);
        c.text(120, BAR + 124, fields[1], 0xFFE0E8FF, 2, true);
        static const char* labels[6] = {"feels like", "wind", "humidity", "rain", "pressure", "sunrise / sunset"};
        for (int i = 0; i < 5; i++) {
            int x = 20 + (i % 3) * 136, y = BAR + 180 + (i / 3) * 52;
            c.fill(x, y, 128, 44, 0xFF26385C);
            c.text(x + 8, y + 6, labels[i], 0xFF8AA0D0);
            c.text_n(x + 8, y + 22, fields[3 + i], 15, th::white, 1, true);
        }
        char b[100];
        snprintf(b, sizeof(b), "%.5s / %.5s", fields[8], fields[9]);
        int x = 20 + 2 * 136, y = BAR + 180 + 52;
        c.fill(x, y, 128, 44, 0xFF26385C);
        c.text(x + 8, y + 6, labels[5], 0xFF8AA0D0);
        c.text_n(x + 8, y + 22, b, 15, th::white, 1, true);
    }
    void on_mouse(const MouseEv& e) override {
        int w = win ? win->client().w : 440;
        if (e.y >= BAR && e.y < BAR + 38 && e.kind == M_DOWN) city.click(e.x - 14, w - 28 - 70, kbd_shift_down());
        if (e.kind == M_MOVE && e.left && e.y < BAR + 38) city.drag(e.x - 14, w - 28 - 70);
        if (e.kind == M_UP && Rect{w - 84, BAR + 9, 70, 22}.contains(e.x, e.y)) fetch();
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.key == K_ENTER) { fetch(); return; }
        if (city.on_key(e)) ui_invalidate();
    }
};

// ================================================================ Typing
struct TypingApp : App {
    static const int NT = 10;
    const char* texts[NT] = {
        "the quick brown fox jumps over the lazy dog while the sun sets behind the hills",
        "silicon is a tiny operating system built from scratch with a kernel a desktop and a lot of patience",
        "a good program is one that does what you expect and nothing that you do not",
        "practice makes progress so keep your eyes on the text and let your fingers find the keys",
        "pack my box with five dozen liquor jugs and send them off before the morning rain begins",
        "every journey of a thousand miles begins with a single step and a well tied pair of shoes",
        "small steady habits beat big occasional bursts of effort when you want to get really fast",
        "how vexingly quick daft zebras jump over the old fence at the edge of the quiet farm",
        "the best way to learn how a computer works is to build one piece by piece and see it boot",
        "bits and bytes flow through tiny wires and somehow they turn into pictures sound and words",
    };
    int cur_text = 0, pos = 0, errors = 0, wpm = 0, acc = 100, best = 0;
    u64 t_start = 0, t_end = 0;
    bool running = false, done = false;
    bool wrong = false;      // the last key was wrong

    TypingApp() { best = game_best("typing"); next(); }
    const char* app_id() override { return "typing"; }
    const char* title() override { return "Typing"; }
    int pref_w() override { return 560; }
    int pref_h() override { return 320; }
    u32 background() override { return 0xFF1A1830; }
    void next() {
        cur_text = (cur_text + 1 + rand() % (NT - 1)) % NT;
        pos = errors = 0; running = done = wrong = false; wpm = 0; acc = 100;
        ui_invalidate();
    }
    void restart() { pos = errors = 0; running = done = wrong = false; ui_invalidate(); }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.key == K_ESC) { restart(); return; }
        if (e.key == K_TAB || (done && e.key == K_ENTER)) { next(); return; }
        if (done) return;
        const char* t = texts[cur_text];
        int len = (int)strlen(t);
        char ch = (char)e.key;
        if (e.key == K_BACKSPACE) { wrong = false; ui_invalidate(); return; }
        if (e.key < 32 || e.key > 126) return;
        if (!running) { running = true; t_start = ticks(); }
        if (ch == t[pos]) { pos++; wrong = false; }
        else { errors++; wrong = true; }
        if (pos >= len) {
            running = false; done = true; t_end = ticks();
            double mins = (t_end - t_start) / 60000.0;
            wpm = mins > 0 ? (int)((len / 5.0) / mins + 0.5) : 0;
            acc = (int)(len * 100 / (len + errors));
            if (wpm > best) { best = wpm; game_save_best("typing", wpm); ui_notify("Typing", "new best speed!", "typing"); }
            ui_beep(2);
        }
        ui_invalidate();
    }
    void tick() override { if (running) ui_invalidate(); }
    void draw(Canvas& c, int w, int h) override {
        c.fill(0, 0, w, BAR, 0xFF121022);
        c.hline(0, BAR - 1, w, 0xFF3A3560);
        c.blit(icon("typing")->small, 10, 9);
        c.text(34, 11, "typing test", 0xFFE0D8FF, 1, true);
        char b[64];
        int live_wpm = wpm;
        if (running && ticks() - t_start > 1500) live_wpm = (int)((pos / 5.0) / ((ticks() - t_start) / 60000.0));
        snprintf(b, sizeof(b), "%d wpm   best %d", live_wpm, best);
        c.text(w - text_width(b) - 12, 11, b, 0xFF9B8AD8);
        const char* t = texts[cur_text];
        int len = (int)strlen(t);
        int cols = (w - 48) / 16;
        // wrap on spaces
        int x = 0, y = BAR + 36, line_start = 0;
        for (int i = 0; i <= len;) {
            int j = i;
            while (j < len && t[j] != ' ') j++;
            int wl = j - i + (j < len ? 1 : 0);
            if (x + wl > cols && x > 0) { x = 0; y += 36; }
            for (int k = i; k < i + wl; k++) {
                u32 col = k < pos ? 0xFF7FE0A8 : 0xFF8A80B8;
                if (k == pos) { c.fill(24 + x * 16 - 1, y - 2, 16, 24, wrong ? 0xFF7A2A44 : 0xFF3A3560); col = th::white; }
                char s[2] = {t[k], 0};
                if (t[k] == ' ' && k == pos) c.fill(24 + x * 16, y + 18, 14, 2, th::white);
                else c.text(24 + x * 16, y, s, col, 2, k < pos);
                x++;
            }
            i += wl;
            (void)line_start;
            if (j >= len) break;
        }
        draw_progress(c, 24, h - 52, w - 48, 8, pos * 100 / max(1, len), 0xFF9B7BFF);
        if (done) {
            c.fill_alpha(0, BAR, w, h - BAR, 0xFF000000, 150);
            snprintf(b, sizeof(b), "%d wpm", wpm);
            c.text_centered(0, h / 2 - 40, w, b, th::white, 4, true);
            snprintf(b, sizeof(b), "%d%% accuracy  " G_BULLET "  %d mistakes", acc, errors);
            c.text_centered(0, h / 2 + 8, w, b, 0xFFD8D0F0);
            c.text_centered(0, h / 2 + 30, w, "enter or tab for another", 0xFF9B8AD8);
        } else if (!running && pos == 0) c.text_centered(0, h - 28, w, "start typing to begin   (esc restarts, tab skips)", 0xFF6A6090);
    }
    void on_mouse(const MouseEv& e) override { if (e.kind == M_UP && done) next(); }
};

// ================================================================ Network
struct NetworkApp : App {
    TextField host;
    int phase = 0;              // 0 idle, 1 resolving, 2 pinging
    ip4 target = 0;
    int sent = 0, got = 0;
    u32 rtt_min = 0, rtt_max = 0, rtt_sum = 0;
    u64 t0 = 0, next_at = 0;
    char lines[8][80];
    int nlines = 0;
    u16 id = 0x5110;
    static const int COUNT = 5;

    NetworkApp() { host.set("example.com"); host.focused = true; }
    const char* app_id() override { return "network"; }
    const char* title() override { return "Network"; }
    int pref_w() override { return 480; }
    int pref_h() override { return 500; }

    void addline(const char* s) {
        if (nlines == 8) { for (int i = 1; i < 8; i++) memcpy(lines[i - 1], lines[i], 80); nlines--; }
        strlcpy(lines[nlines++], s, 80);
    }
    static void reply(void* ctx, u16 seq, u32 rtt, ip4 from, u8 ttl) {
        NetworkApp* a = (NetworkApp*)ctx;
        char ip[20], b[80];
        ip_to_str(from, ip);
        snprintf(b, sizeof(b), "reply from %s: seq=%d ttl=%d time=%u ms", ip, seq, ttl, rtt);
        a->addline(b);
        a->got++;
        if (!a->rtt_min || rtt < a->rtt_min) a->rtt_min = rtt;
        a->rtt_max = max(a->rtt_max, rtt);
        a->rtt_sum += rtt;
        ui_invalidate();
    }
    void start() {
        if (phase) return;
        nlines = 0; sent = got = 0; rtt_min = rtt_max = rtt_sum = 0;
        if (!net_link_up()) { addline("network is unreachable"); return; }
        if (str_to_ip(host.text, &target)) { phase = 2; next_at = ticks(); }
        else { phase = 1; t0 = ticks(); dns_flush(); }
        ui_invalidate();
    }
    void tick() override {
        static u64 lt;
        if (ticks() - lt > 500) { lt = ticks(); ui_invalidate(); }
        if (phase == 1) {
            int st = dns_resolve(host.text, &target);
            if (st > 0) { phase = 2; next_at = ticks(); char b[80], ip[20]; ip_to_str(target, ip); snprintf(b, sizeof(b), "pinging %s [%s]", host.text, ip); addline(b); }
            else if (st < 0 || ticks() - t0 > 8000) { addline("name or service not known"); phase = 0; }
        } else if (phase == 2 && ticks() >= next_at) {
            if (sent < COUNT) { icmp_ping(target, id, (u16)(sent + 1), reply, this); sent++; next_at = ticks() + 1000; }
            else if (ticks() > next_at + 800) {
                char b[80];
                snprintf(b, sizeof(b), "%d sent, %d received, %d%% loss%s", sent, got, (sent - got) * 100 / sent, got ? "" : " (no replies)");
                addline(b);
                if (got) { snprintf(b, sizeof(b), "rtt min/avg/max = %u/%u/%u ms", rtt_min, rtt_sum / got, rtt_max); addline(b); }
                phase = 0;
            }
        }
    }
    void kv(Canvas& c, int x, int y, const char* k, const char* v) { c.text(x, y, k, th::muted); c.text(x + 110, y, v, th::text, 1, true); }
    void draw(Canvas& c, int w, int h) override {
        toolbar(c, w, "network", "network");
        char b[80], v[48];
        bool up = net_link_up();
        c.circle(w - 20, 17, 5, up ? th::green : th::red, true);
        c.text(w - 28 - text_width(up ? "online" : "offline"), 11, up ? "online" : "offline", th::muted);
        int y = BAR + 14;
        c.text(14, y, "interface", th::text, 1, true); y += 20;
        snprintf(v, sizeof(v), "%02x:%02x:%02x:%02x:%02x:%02x", netcfg.mac[0], netcfg.mac[1], netcfg.mac[2], netcfg.mac[3], netcfg.mac[4], netcfg.mac[5]);
        kv(c, 14, y, "mac address", netcfg.up ? v : "no card"); y += 18;
        ip_to_str(netcfg.ip, v); kv(c, 14, y, "address", netcfg.configured ? v : "-"); y += 18;
        ip_to_str(netcfg.mask, v); kv(c, 14, y, "netmask", netcfg.configured ? v : "-"); y += 18;
        ip_to_str(netcfg.gateway, v); kv(c, 14, y, "gateway", netcfg.configured ? v : "-"); y += 18;
        ip_to_str(netcfg.dns, v); kv(c, 14, y, "dns", netcfg.configured ? v : "-"); y += 18;
        kv(c, 14, y, "configured by", netcfg.dhcp ? "dhcp" : "static"); y += 24;
        u64 txp, rxp, txb, rxb;
        net_stats(&txp, &rxp, &txb, &rxb);
        char a1[24], a2[24];
        fmt_bytes(txb, a1, sizeof(a1)); fmt_bytes(rxb, a2, sizeof(a2));
        snprintf(b, sizeof(b), "%u sent (%s)   %u received (%s)", (u32)txp, a1, (u32)rxp, a2);
        kv(c, 14, y, "traffic", ""); c.text(124, y, b, th::text); y += 26;
        c.text(14, y, "connections", th::text, 1, true); y += 20;
        int shown = 0;
        for (int i = 0; i < tcp_socket_count() && shown < 4; i++) {
            int st; ip4 rip; u16 rp, lp; u32 rx, tx;
            if (!tcp_socket_info(i, &st, &rip, &rp, &lp, &rx, &tx) || st == TCP_FREE) continue;
            char ip[20];
            ip_to_str(rip, ip);
            snprintf(b, sizeof(b), "%s:%d  %s", ip, rp, tcp_state_name(st));
            c.text(14, y, b, th::text);
            y += 16; shown++;
        }
        if (!shown) { c.text(14, y, "no open connections", th::muted); y += 16; }
        y += 12;
        c.text(14, y, "ping", th::text, 1, true); y += 20;
        host.focused = true;
        host.draw(c, 14, y, w - 28 - 80);
        draw_button(c, w - 88, y, 74, 22, phase ? "..." : "Ping", hot(w - 88, y, 74, 22), true, phase == 0);
        ping_y = y;
        y += 30;
        c.fill(14, y, w - 28, 8 * 16 + 8, 0xFF1E1830);
        for (int i = 0; i < nlines; i++) c.text_n(20, y + 6 + i * 16, lines[i], (w - 44) / 8, 0xFFB8F0D0);
    }
    int ping_y = 0;
    void on_mouse(const MouseEv& e) override {
        int w = win ? win->client().w : 480;
        if (e.y >= ping_y && e.y < ping_y + 24 && e.x < w - 90) {
            if (e.kind == M_DOWN) host.click(e.x - 14, w - 28 - 80, kbd_shift_down());
            else if (e.kind == M_MOVE && e.left) host.drag(e.x - 14, w - 28 - 80);
        }
        if (e.kind == M_UP && Rect{w - 88, ping_y, 74, 22}.contains(e.x, e.y)) start();
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.key == K_ENTER) { start(); return; }
        if (host.on_key(e)) ui_invalidate();
    }
};

// ================================================================ Archive (zip)
struct ArchiveApp : App {
    struct Entry { char name[96]; u32 csize, usize, off; u16 method; bool dir; };
    Vec<Entry> entries;
    FsNode* zip = nullptr;
    char err[96] = "";
    int sel = -1;
    bool create_mode = false;
    struct Pend { FsNode* n; char name[96]; };
    Vec<Pend> pend;
    Smooth sc;
    ScrollBar sb;
    int ch = 0;
    static const int ROW = 20, TOP = BAR + 28;

    ArchiveApp(const char* arg) { if (arg && arg[0]) open_arg(arg); }
    ~ArchiveApp() { entries.free(); pend.free(); }
    const char* app_id() override { return "archive"; }
    const char* title() override { return "Archive"; }
    int pref_w() override { return 540; }
    int pref_h() override { return 420; }
    static u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
    static u32 rd16(const u8* p) { return p[0] | (p[1] << 8); }

    void open_arg(const char* path) override {
        FsNode* f = fs_lookup(path);
        if (!f || f->dir) return;
        zip = f; create_mode = false; entries.clear(); err[0] = 0; sel = -1; sc.jump(0);
        const u8* d = f->data;
        u32 n = f->size;
        if (n < 22) { strlcpy(err, "this isn't a zip file", sizeof(err)); return; }
        int eocd = -1;
        for (int i = (int)n - 22; i >= 0 && i > (int)n - 65600; i--) if (rd32(d + i) == 0x06054b50) { eocd = i; break; }
        if (eocd < 0) { strlcpy(err, "this isn't a zip file", sizeof(err)); return; }
        u32 count = rd16(d + eocd + 10), off = rd32(d + eocd + 16);
        for (u32 i = 0; i < count; i++) {
            if (off + 46 > n || rd32(d + off) != 0x02014b50) break;
            Entry e;
            e.method = (u16)rd16(d + off + 10);
            e.csize = rd32(d + off + 20); e.usize = rd32(d + off + 24);
            u32 nl = rd16(d + off + 28), xl = rd16(d + off + 30), cl = rd16(d + off + 32);
            e.off = rd32(d + off + 42);
            u32 l = min(nl, 95u);
            if (off + 46 + nl > n) break;
            memcpy(e.name, d + off + 46, l);
            e.name[l] = 0;
            e.dir = l > 0 && e.name[l - 1] == '/';
            entries.push(e);
            off += 46 + nl + xl + cl;
        }
        if (!entries.len) strlcpy(err, "the archive is empty", sizeof(err));
        set_title(title());
        ui_invalidate();
    }
    bool extract(const Entry& e, const char* base) {
        if (str_icontains(e.name, "..") || e.name[0] == '/') return false;
        char path[300];
        snprintf(path, sizeof(path), "%s/%s", base, e.name);
        int l = (int)strlen(path);
        if (e.dir) { path[l - 1] = 0; fs_mkdirs(path); return true; }
        char dirp[300];
        strlcpy(dirp, path, sizeof(dirp));
        char* sl = strrchr(dirp, '/');
        if (sl) { *sl = 0; fs_mkdirs(dirp); }
        const u8* d = zip->data;
        if (e.off + 30 > zip->size || rd32(d + e.off) != 0x04034b50) return false;
        u32 start = e.off + 30 + rd16(d + e.off + 26) + rd16(d + e.off + 28);
        if (start + e.csize > zip->size) return false;
        FsNode* f = fs_create_path(path, false);
        if (!f) return false;
        if (e.method == 0) return fs_write(f, d + start, e.csize);
        if (e.method == 8) {
            u32 out_len = 0;
            u8* out = deflate_decompress(d + start, e.csize, &out_len, max(e.usize, 1u) + 64);
            if (!out) return false;
            bool ok = fs_write(f, out, out_len);
            kfree(out);
            return ok;
        }
        return false;
    }
    void extract_all() {
        if (!zip) return;
        char base[160], stem[FS_NAME_MAX];
        strlcpy(stem, zip->name, sizeof(stem));
        char* dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        FsNode* par = zip->parent ? zip->parent : fs_home();
        char pp[120];
        fs_path(par, pp, sizeof(pp));
        char un[FS_NAME_MAX];
        fs_unique_name(par, stem, un, sizeof(un));
        snprintf(base, sizeof(base), "%s/%s", !strcmp(pp, "/") ? "" : pp, un);
        fs_mkdirs(base);
        int ok = 0, bad = 0;
        for (int i = 0; i < entries.len; i++) { if (extract(entries[i], base)) ok++; else bad++; }
        char msg[96];
        snprintf(msg, sizeof(msg), "%d extracted%s to %s", ok, bad ? ", some skipped" : "", base);
        ui_notify("Archive", msg, "archive");
        ui_beep(bad ? 1 : 2);
    }
    // ---- creating
    static u32 crc32(const u8* p, u32 n) {
        static u32 tab[256];
        if (!tab[1]) for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
        u32 c = 0xFFFFFFFFu;
        for (u32 i = 0; i < n; i++) c = tab[(c ^ p[i]) & 255] ^ (c >> 8);
        return ~c;
    }
    void add_node(FsNode* n, const char* prefix) {
        char name[96];
        snprintf(name, sizeof(name), "%s%s%s", prefix, n->name, n->dir ? "/" : "");
        if (pend.len < 400) pend.push({n, {0}});
        strlcpy(pend[pend.len - 1].name, name, 96);
        if (n->dir) for (int i = 0; i < n->children.len; i++) add_node(n->children[i], name);
    }
    void add_path(const char* p) {
        FsNode* n = fs_lookup(p);
        if (!n || n == fs_root()) return;
        add_node(n, "");
        ui_invalidate();
    }
    static void p16(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
    static void p32(u8* p, u32 v) { p16(p, v & 0xFFFF); p16(p + 2, v >> 16); }
    void build(const char* out_name) {
        u32 total = 22;
        for (int i = 0; i < pend.len; i++) total += 30 + 46 + 2 * (u32)strlen(pend[i].name) + (pend[i].n->dir ? 0 : pend[i].n->size);
        u8* z = (u8*)kmalloc_try(total + 16);
        if (!z) { ui_notify("Archive", "out of memory", "archive"); return; }
        u32 o = 0;
        u32 offs[400], crcs[400];
        for (int i = 0; i < pend.len; i++) {
            FsNode* n = pend[i].n;
            u32 nl = (u32)strlen(pend[i].name), sz = n->dir ? 0 : n->size;
            crcs[i] = n->dir ? 0 : crc32(n->data, sz);
            offs[i] = o;
            u8* h = z + o;
            memset(h, 0, 30);
            p32(h, 0x04034b50); p16(h + 4, 10); p16(h + 10, 0); p16(h + 12, 0x21);
            p32(h + 14, crcs[i]); p32(h + 18, sz); p32(h + 22, sz); p16(h + 26, nl);
            memcpy(h + 30, pend[i].name, nl);
            o += 30 + nl;
            if (sz) { memcpy(z + o, n->data, sz); o += sz; }
        }
        u32 cd = o;
        for (int i = 0; i < pend.len; i++) {
            FsNode* n = pend[i].n;
            u32 nl = (u32)strlen(pend[i].name), sz = n->dir ? 0 : n->size;
            u8* h = z + o;
            memset(h, 0, 46);
            p32(h, 0x02014b50); p16(h + 4, 20); p16(h + 6, 10); p16(h + 12, 0); p16(h + 14, 0x21);
            p32(h + 16, crcs[i]); p32(h + 20, sz); p32(h + 24, sz); p16(h + 28, nl);
            if (n->dir) p32(h + 38, 0x10);
            p32(h + 42, offs[i]);
            memcpy(h + 46, pend[i].name, nl);
            o += 46 + nl;
        }
        u8* e = z + o;
        memset(e, 0, 22);
        p32(e, 0x06054b50); p16(e + 8, pend.len); p16(e + 10, pend.len); p32(e + 12, o - cd); p32(e + 16, cd);
        o += 22;
        char path[160], nm[FS_NAME_MAX];
        char base[FS_NAME_MAX];
        strlcpy(base, out_name, sizeof(base));
        if (!str_icontains(base, ".zip")) strlcat(base, ".zip", sizeof(base));
        fs_unique_name(fs_home(), base, nm, sizeof(nm));
        snprintf(path, sizeof(path), "/home/%s", nm);
        FsNode* f = fs_create_path(path, false);
        bool ok = f && fs_write(f, z, o);
        kfree(z);
        if (ok) { ui_notify("Archive created", path, "archive"); pend.clear(); ui_beep(2); }
        else ui_notify("Archive", "couldn't write the archive", "archive");
    }

    int count() { return create_mode ? pend.len : entries.len; }
    int content_h() { return count() * ROW + 8; }
    void tick() override { if (sc.step()) ui_invalidate(); }
    void draw(Canvas& c, int w, int h) override {
        ch = h;
        toolbar(c, w, "archive", zip && !create_mode ? zip->name : "archive");
        draw_button(c, w - 340, 6, 80, 22, "Open" G_ELLIP, hot(w - 340, 6, 80, 22));
        draw_button(c, w - 254, 6, 80, 22, "Extract", hot(w - 254, 6, 80, 22), !create_mode, !create_mode && zip && entries.len);
        draw_button(c, w - 168, 6, 70, 22, "New zip", hot(w - 168, 6, 70, 22), create_mode);
        if (create_mode) {
            draw_button(c, w - 92, 6, 80, 22, "Create" G_ELLIP, hot(w - 92, 6, 80, 22), true, pend.len > 0);
            c.fill(0, BAR, w, 28, 0xFFF0ECFA);
            draw_button(c, 10, BAR + 3, 90, 22, "Add file" G_ELLIP, hot(10, BAR + 3, 90, 22));
            draw_button(c, 106, BAR + 3, 110, 22, "Add folder" G_ELLIP, hot(106, BAR + 3, 110, 22));
            draw_button(c, 222, BAR + 3, 70, 22, "Clear", hot(222, BAR + 3, 70, 22), false, pend.len > 0);
        } else {
            c.fill(0, BAR, w, 28, 0xFFF0ECFA);
            char b[64];
            if (zip) { snprintf(b, sizeof(b), "%d items", entries.len); c.text(12, BAR + 9, b, th::muted); }
            c.text(w - 130, BAR + 9, "name", th::muted); c.text(w - 70, BAR + 9, "size", th::muted);
        }
        c.hline(0, TOP - 1, w, th::border);
        c.set_clip({0, TOP, w - 11, h - TOP});
        if (err[0] && !create_mode) c.text_centered(0, TOP + 60, w, err, th::muted, 1, true);
        if (!zip && !create_mode) c.text_centered(0, TOP + 60, w, "open a .zip file, or press New zip to make one", th::muted, 1, true);
        if (create_mode && !pend.len) c.text_centered(0, TOP + 60, w, "add files or folders to put in the archive", th::muted, 1, true);
        for (int i = 0; i < count(); i++) {
            int y = TOP + 4 + i * ROW - sc.pos();
            if (y + ROW < TOP || y > h) continue;
            bool on = hot(0, y, w - 11, ROW);
            if (i == sel || on) c.fill(4, y, w - 19, ROW - 1, i == sel ? blend(th::paper, accent(), 70) : th::panel);
            const char* nm = create_mode ? pend[i].name : entries[i].name;
            bool dir = create_mode ? pend[i].n->dir : entries[i].dir;
            u32 sz = create_mode ? (dir ? 0 : pend[i].n->size) : entries[i].usize;
            c.blit(icon(dir ? "folder" : "file")->small, 10, y + 2);
            c.text_n(32, y + 4, nm, (w - 200) / 8, th::text);
            if (!dir) { char b[24]; fmt_bytes(sz, b, sizeof(b)); c.text(w - 20 - text_width(b), y + 4, b, th::muted); }
        }
        c.set_clip({0, 0, w, h});
        sb.draw(c, w - 10, TOP, h - TOP, content_h(), h - TOP, sc.pos());
    }
    void on_mouse(const MouseEv& e) override {
        int w = win ? win->client().w : 540;
        int off = sc.target;
        if (sb.handle(e, w - 10, TOP, ch - TOP, content_h(), ch - TOP, off)) { sc.jump(off); ui_invalidate(); return; }
        if (e.kind == M_WHEEL) { sc.set(clamp(sc.target - e.wheel * 60, 0, max(0, content_h() - (ch - TOP)))); ui_invalidate(); return; }
        if (e.kind != M_UP || e.button != 1) return;
        if (e.y < BAR) {
            if (Rect{w - 340, 6, 80, 22}.contains(e.x, e.y))
                pick_file("Open archive", "/home", "zip", false, "", [](App* a, const char* p) { a->open_arg(p); });
            else if (Rect{w - 254, 6, 80, 22}.contains(e.x, e.y) && !create_mode) extract_all();
            else if (Rect{w - 168, 6, 70, 22}.contains(e.x, e.y)) create_mode = !create_mode;
            else if (create_mode && Rect{w - 92, 6, 80, 22}.contains(e.x, e.y) && pend.len)
                prompt("Name of the new archive", "archive", [](App* a, const char* t) { if (t[0]) ((ArchiveApp*)a)->build(t); });
        } else if (e.y < TOP) {
            if (create_mode) {
                if (Rect{10, BAR + 3, 90, 22}.contains(e.x, e.y))
                    pick_file("Add a file", "/home", "", false, "", [](App* a, const char* p) { ((ArchiveApp*)a)->add_path(p); });
                else if (Rect{106, BAR + 3, 110, 22}.contains(e.x, e.y))
                    prompt("Folder to add (a path)", "/home/", [](App* a, const char* p) { ((ArchiveApp*)a)->add_path(p); });
                else if (Rect{222, BAR + 3, 70, 22}.contains(e.x, e.y)) pend.clear();
            }
        } else {
            int i = (e.y - TOP - 4 + sc.pos()) / ROW;
            sel = i >= 0 && i < count() ? i : -1;
        }
        ui_invalidate();
    }
    int menus(Menu* m) override { return 0; }
};

App* new_capture(const char*) { return new CaptureApp(); }
App* new_weather(const char*) { return new WeatherApp(); }
App* new_typing(const char*) { return new TypingApp(); }
App* new_network(const char*) { return new NetworkApp(); }
App* new_archive(const char* a) { return new ArchiveApp(a); }
