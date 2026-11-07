// silicon - Firewall: packet filter rules, a log of what was blocked, and the HTTP proxy setting
#include "apps.h"
#include "../../net/net.h"

struct FirewallApp : App {
    int tab = 0;                                  // 0 rules, 1 log, 2 proxy, 3 vpn
    bool n_allow = false;                         // the new rule: action, direction, protocol
    int n_dir = 2, n_proto = 0;
    TextField port, addr, proxy, vhost, vkey, vaddr;
    int vfocus = 0;
    int focus = 0;                                // which text field has the keys: 0 port, 1 address, 2 proxy
    Smooth sc;
    int cw = 0, ch = 0;
    char msg[96] = "";
    static const int BAR = 38, TABS = 28, ROW = 22;

    FirewallApp() {
        port.set("");
        addr.set("");
        char host[128];
        u16 pp;
        if (net_proxy_get(host, sizeof(host), &pp)) { char b[160]; snprintf(b, sizeof(b), "%s:%u", host, pp); proxy.set(b); }
        else proxy.set("");
        char h[100], k[48], a[16], al[100];
        vpn_current(h, k, a, al);
        vhost.set(h); vkey.set(k); vaddr.set(a[0] ? a : "10.7.0.2");
    }
    const char* app_id() override { return "firewall"; }
    const char* title() override { return "Firewall"; }
    int pref_w() override { return 560; }
    int pref_h() override { return 460; }

    static const char* dir_name(int d) { return d == 1 ? "in" : d == 2 ? "out" : "both"; }
    static const char* proto_name(int p) { return p == 1 ? "icmp" : p == 6 ? "tcp" : p == 17 ? "udp" : "any"; }
    static int next_proto(int p) { return p == 0 ? 6 : p == 6 ? 17 : p == 17 ? 1 : 0; }
    static void fmt_rule_addr(const FwRule& r, char* out, int n) {
        if (!r.mask) { strlcpy(out, "any", n); return; }
        char a[20];
        ip_to_str(r.ip, a);
        int bits = 0;
        for (u32 m = ntohl(r.mask); m & 0x80000000u; m <<= 1) bits++;
        snprintf(out, n, bits == 32 ? "%s" : "%s/%d", a, bits);
    }
    static bool parse_addr(const char* s, ip4* ip, ip4* mask) {
        if (!s[0] || !strcmp(s, "any")) { *ip = 0; *mask = 0; return true; }
        char tmp[40];
        strlcpy(tmp, s, sizeof(tmp));
        int bits = 32;
        char* sl = strchr(tmp, '/');
        if (sl) { bits = atoi(sl + 1); *sl = 0; }
        if (!str_to_ip(tmp, ip) || bits < 0 || bits > 32) return false;
        *mask = bits ? htonl(0xFFFFFFFFu << (32 - bits)) : 0;
        if (!*mask) return true;
        *ip &= *mask;
        return true;
    }
    void add_rule(bool allow, int dir, int proto, int prt, const char* a) {
        FwRule r;
        r.allow = allow; r.dir = (u8)dir; r.proto = (u8)proto; r.port = (u16)prt; r.hits = 0;
        if (!parse_addr(a, &r.ip, &r.mask)) { snprintf(msg, sizeof(msg), "that address isn't valid (try 10.0.0.0/8)"); return; }
        fw_add(r);
        msg[0] = 0;
    }
    void preset_web() {
        fw_clear_rules();
        fw_out_default_allow = false;
        add_rule(true, 2, 6, 80, "");
        add_rule(true, 2, 6, 443, "");
        add_rule(true, 2, 17, 53, "");
        add_rule(true, 2, 6, 53, "");
        add_rule(true, 2, 1, 0, "");
        fw_save();
        snprintf(msg, sizeof(msg), "outgoing traffic limited to web, DNS and ping");
    }
    void tick() override {
        static int last = -1;
        int sig = fw_log_count() * 3 + (int)fw_blocked_in() + (int)fw_blocked_out();
        if (sig != last) { last = sig; ui_invalidate(); }
        if (sc.step()) ui_invalidate();
    }

    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        c.fill(0, 0, w, BAR, th::panel);
        c.hline(0, BAR - 1, w, th::border);
        c.blit(icon("firewall")->small, 10, 11);
        char b[64];
        snprintf(b, sizeof(b), "%s", fw_enabled ? "firewall is on" : "firewall is OFF");
        c.text(34, 13, b, fw_enabled ? th::green : th::red, 1, true);
        draw_button(c, w - 96, 7, 86, 24, fw_enabled ? "Turn off" : "Turn on", hot(w - 96, 7, 86, 24), !fw_enabled);
        static const char* names[4] = {"Rules", "Log", "Proxy", "VPN"};
        for (int i = 0; i < 4; i++) draw_button(c, 10 + i * 78, BAR + 4, 74, 22, names[i], hot(10 + i * 78, BAR + 4, 74, 22), tab == i);
        int top = BAR + TABS + 8;
        if (tab == 0) draw_rules(c, w, h, top);
        else if (tab == 1) draw_log(c, w, h, top);
        else if (tab == 2) draw_proxy(c, w, h, top);
        else draw_vpn(c, w, h, top);
        if (msg[0]) c.text_n(10, h - 16, msg, (w - 20) / 8, th::muted);
    }
    void draw_rules(Canvas& c, int w, int h, int top) {
        draw_button(c, 10, top, 200, 22, fw_out_default_allow ? "outgoing default: allow" : "outgoing default: DENY", hot(10, top, 200, 22), !fw_out_default_allow);
        draw_button(c, 220, top, 150, 22, "Web & DNS only", hot(220, top, 150, 22));
        draw_button(c, 376, top, 80, 22, "Clear", hot(376, top, 80, 22), false, fw_rule_count() > 0);
        int y = top + 30;
        c.fill(0, y, w, 18, th::panel);
        c.text(10, y + 3, "action", th::muted); c.text(70, y + 3, "dir", th::muted); c.text(116, y + 3, "proto", th::muted);
        c.text(172, y + 3, "port", th::muted); c.text(226, y + 3, "address", th::muted); c.text(w - 150, y + 3, "hits", th::muted);
        y += 20;
        int n = fw_rule_count();
        for (int i = 0; i < n; i++) {
            FwRule* r = fw_rule(i);
            int yy = y + i * ROW;
            if (yy + ROW > h - 80) break;
            if (i % 2) c.fill(0, yy, w, ROW, 0xFFF2EEFA);
            c.text(10, yy + 5, r->allow ? "allow" : "deny", r->allow ? th::green : th::red, 1, true);
            c.text(70, yy + 5, dir_name(r->dir), th::text);
            c.text(116, yy + 5, proto_name(r->proto), th::text);
            char p[12];
            if (r->port) snprintf(p, sizeof(p), "%u", r->port); else strlcpy(p, "any", sizeof(p));
            c.text(172, yy + 5, p, th::text);
            char a[40];
            fmt_rule_addr(*r, a, sizeof(a));
            c.text(226, yy + 5, a, th::text);
            snprintf(p, sizeof(p), "%u", r->hits);
            c.text(w - 150, yy + 5, p, th::muted);
            c.text(w - 90, yy + 5, G_UP, hot(w - 96, yy, 20, ROW) ? accent() : th::muted, 1, true);
            c.text(w - 66, yy + 5, G_DOWN, hot(w - 72, yy, 20, ROW) ? accent() : th::muted, 1, true);
            c.text(w - 38, yy + 5, "x", hot(w - 44, yy, 20, ROW) ? th::red : th::muted, 1, true);
        }
        if (!n) c.text_centered(0, y + 20, w, "no rules: everything is allowed", th::muted);
        // new rule
        int ny = h - 70;
        c.hline(0, ny - 8, w, th::border);
        draw_button(c, 10, ny, 62, 22, n_allow ? "allow" : "deny", hot(10, ny, 62, 22), n_allow);
        draw_button(c, 78, ny, 56, 22, dir_name(n_dir), hot(78, ny, 56, 22));
        draw_button(c, 140, ny, 56, 22, proto_name(n_proto), hot(140, ny, 56, 22));
        port.focused = focus == 0;
        port.draw(c, 202, ny, 64);
        if (!port.len) c.text(208, ny + 5, "port", th::muted);
        addr.focused = focus == 1;
        addr.draw(c, 272, ny, 160);
        if (!addr.len) c.text(278, ny + 5, "any address", th::muted);
        draw_button(c, w - 118, ny, 106, 22, "Add rule", hot(w - 118, ny, 106, 22), true);
    }
    void draw_log(Canvas& c, int w, int h, int top) {
        char b[96];
        snprintf(b, sizeof(b), "blocked: %u incoming, %u outgoing", fw_blocked_in(), fw_blocked_out());
        c.text(10, top + 4, b, th::text, 1, true);
        draw_button(c, w - 96, top, 86, 22, "Clear log", hot(w - 96, top, 86, 22), false, fw_log_count() > 0);
        int y = top + 30;
        for (int i = 0; i < fw_log_count() && y + 18 < h - 20; i++, y += 18) {
            const FwLogEntry* e = fw_log(i);
            DateTime dt;
            stamp_to_datetime(e->stamp, dt);
            char ip[48];
            if (e->v6) ip6_to_str(e->remote6, ip, sizeof(ip)); else ip_to_str(e->remote, ip);
            snprintf(b, sizeof(b), e->v6 ? "%02d:%02d:%02d  %s  %s  [%s]:%u" : "%02d:%02d:%02d  %s  %s  %s:%u", dt.hour, dt.minute, dt.second, e->inbound ? "in " : "out", FirewallApp::proto_name(e->proto), ip, e->port);
            c.text(10, y, b, th::text);
            if (e->rule >= 0) { snprintf(b, sizeof(b), "rule %d", e->rule + 1); c.text(w - 80, y, b, th::muted); }
            else c.text(w - 80, y, "default", th::muted);
        }
        if (!fw_log_count()) c.text_centered(0, top + 70, w, "nothing has been blocked", th::muted);
    }
    void draw_vpn(Canvas& c, int w, int h, int top) {
        VpnInfo vi;
        vpn_info(&vi);
        c.text(10, top + 4, "WireGuard VPN", th::text, 1, true);
        u32 sc_col = vi.up ? th::green : vi.enabled ? th::gold_dk : th::muted;
        c.disc(24, top + 34, 6, sc_col);
        char b[160];
        if (vi.up) snprintf(b, sizeof(b), "connected"); else snprintf(b, sizeof(b), "%s: %s", vi.enabled ? "connecting" : "off", vi.status);
        c.text(38, top + 28, b, sc_col, 1, true);
        if (vi.up) { snprintf(b, sizeof(b), "sent %u KB, received %u KB", (u32)(vi.tx >> 10), (u32)(vi.rx >> 10)); c.text(10, top + 48, b, th::muted); }
        draw_button(c, w - 110, top + 22, 100, 26, vi.enabled ? "Turn off" : "Turn on", hot(w - 110, top + 22, 100, 26), !vi.enabled, vi.configured);
        int y = top + 78;
        static const char* labels[3] = {"server (host:port)", "server public key", "my tunnel address"};
        TextField* fs[3] = {&vhost, &vkey, &vaddr};
        for (int i = 0; i < 3; i++) {
            c.text(10, y + i * 46, labels[i], th::muted);
            fs[i]->focused = vfocus == i;
            fs[i]->draw(c, 10, y + 16 + i * 46, w - 130);
        }
        draw_button(c, w - 110, y + 15 + 2 * 46, 100, 24, "Save", hot(w - 110, y + 15 + 2 * 46, 100, 24), true);
        if (vi.configured) {
            c.text(10, y + 3 * 46 + 4, "my public key (give this to the server):", th::muted);
            c.text(10, y + 3 * 46 + 20, vi.public_key, th::text);
        }
    }
    void draw_proxy(Canvas& c, int w, int h, int top) {
        c.text(10, top + 4, "HTTP proxy for Beam, wget and curl", th::text, 1, true);
        c.text(10, top + 24, "host:port, for example 192.168.1.5:3128. Leave it empty to connect directly.", th::muted);
        proxy.focused = true;
        proxy.draw(c, 10, top + 50, w - 130);
        draw_button(c, w - 110, top + 49, 100, 24, "Save", hot(w - 110, top + 49, 100, 24), true);
        char host[128];
        u16 pp;
        if (net_proxy_get(host, sizeof(host), &pp)) { char b[200]; snprintf(b, sizeof(b), "in use: %s port %u  " G_BULLET "  http pages go to the proxy, https uses CONNECT", host, pp); c.text_n(10, top + 90, b, (w - 20) / 8, th::green); }
        else c.text(10, top + 90, "no proxy: connecting directly", th::muted);
    }

    void on_mouse(const MouseEv& e) override {
        int top = BAR + TABS + 8;
        if (e.kind == M_MOVE) {
            if (e.left && tab == 0 && focus == 0) port.drag(e.x - 202, 64);
            else if (e.left && tab == 0 && focus == 1) addr.drag(e.x - 272, 160);
            else if (e.left && tab == 2) proxy.drag(e.x - 10, cw - 130);
            else if (e.left && tab == 3) (vfocus == 0 ? vhost : vfocus == 1 ? vkey : vaddr).drag(e.x - 10, cw - 130);
            ui_invalidate();
            return;
        }
        if (e.kind == M_DOWN && e.button == 1) {
            int ny = ch - 70;
            if (tab == 0 && Rect{202, ny, 64, 22}.contains(e.x, e.y)) { focus = 0; port.click(e.x - 202, 64, kbd_shift_down()); }
            else if (tab == 0 && Rect{272, ny, 160, 22}.contains(e.x, e.y)) { focus = 1; addr.click(e.x - 272, 160, kbd_shift_down()); }
            else if (tab == 2 && Rect{10, top + 50, cw - 130, 22}.contains(e.x, e.y)) proxy.click(e.x - 10, cw - 130, kbd_shift_down());
            else if (tab == 3) {
                for (int i = 0; i < 3; i++)
                    if (Rect{10, top + 78 + 16 + i * 46, cw - 130, 22}.contains(e.x, e.y)) { vfocus = i; (i == 0 ? vhost : i == 1 ? vkey : vaddr).click(e.x - 10, cw - 130, kbd_shift_down()); }
            }
            ui_invalidate();
            return;
        }
        if (e.kind != M_UP || e.button != 1) return;
        if (Rect{cw - 96, 7, 86, 24}.contains(e.x, e.y)) { fw_enabled = !fw_enabled; fw_save(); }
        for (int i = 0; i < 4; i++) if (Rect{10 + i * 78, BAR + 4, 74, 22}.contains(e.x, e.y)) tab = i;
        if (tab == 0) {
            if (Rect{10, top, 200, 22}.contains(e.x, e.y)) { fw_out_default_allow = !fw_out_default_allow; fw_save(); }
            else if (Rect{220, top, 150, 22}.contains(e.x, e.y)) preset_web();
            else if (Rect{376, top, 80, 22}.contains(e.x, e.y)) { fw_clear_rules(); fw_out_default_allow = true; fw_save(); }
            int y = top + 50;
            for (int i = 0; i < fw_rule_count(); i++) {
                int yy = y + i * ROW;
                if (e.y < yy || e.y >= yy + ROW) continue;
                if (e.x >= cw - 44 && e.x < cw - 24) { fw_remove(i); break; }
                if (e.x >= cw - 96 && e.x < cw - 76) { fw_swap(i, i - 1); break; }
                if (e.x >= cw - 72 && e.x < cw - 52) { fw_swap(i, i + 1); break; }
            }
            int ny = ch - 70;
            if (Rect{10, ny, 62, 22}.contains(e.x, e.y)) n_allow = !n_allow;
            else if (Rect{78, ny, 56, 22}.contains(e.x, e.y)) n_dir = n_dir == 1 ? 2 : n_dir == 2 ? 3 : 1;
            else if (Rect{140, ny, 56, 22}.contains(e.x, e.y)) n_proto = next_proto(n_proto);
            else if (Rect{cw - 118, ny, 106, 22}.contains(e.x, e.y)) { add_rule(n_allow, n_dir, n_proto, atoi(port.text), addr.text); if (!msg[0]) { port.set(""); addr.set(""); } }
        } else if (tab == 1) {
            if (Rect{cw - 96, top, 86, 22}.contains(e.x, e.y)) fw_log_clear();
        } else if (tab == 3) {
            VpnInfo vi;
            vpn_info(&vi);
            if (vi.configured && Rect{cw - 110, top + 22, 100, 26}.contains(e.x, e.y)) vpn_set_enabled(!vi.enabled);
            else if (Rect{cw - 110, top + 78 + 15 + 2 * 46, 100, 24}.contains(e.x, e.y)) {
                char err[96] = "", al[100], h2[100], k2[48], a2[16];
                vpn_current(h2, k2, a2, al);
                if (vpn_configure(vhost.text, vkey.text, vaddr.text, al, err, sizeof(err))) snprintf(msg, sizeof(msg), "saved; turn the VPN on to connect");
                else strlcpy(msg, err, sizeof(msg));
            }
        } else if (Rect{cw - 110, top + 49, 100, 24}.contains(e.x, e.y)) {
            net_proxy_set(proxy.text);
            snprintf(msg, sizeof(msg), proxy.len ? "proxy saved" : "proxy cleared");
        }
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (tab == 0) {
            TextField& f = focus == 1 ? addr : port;
            if (e.key == K_TAB) { focus ^= 1; ui_invalidate(); return; }
            if (e.key == K_ENTER) { add_rule(n_allow, n_dir, n_proto, atoi(port.text), addr.text); if (!msg[0]) { port.set(""); addr.set(""); } ui_invalidate(); return; }
            if (f.on_key(e)) ui_invalidate();
        } else if (tab == 3) {
            if (e.key == K_TAB) { vfocus = (vfocus + 1) % 3; ui_invalidate(); return; }
            if ((vfocus == 0 ? vhost : vfocus == 1 ? vkey : vaddr).on_key(e)) ui_invalidate();
        } else if (tab == 2) {
            if (e.key == K_ENTER) { net_proxy_set(proxy.text); snprintf(msg, sizeof(msg), proxy.len ? "proxy saved" : "proxy cleared"); ui_invalidate(); return; }
            if (proxy.on_key(e)) ui_invalidate();
        }
    }
};

App* new_firewall(const char*) { return new FirewallApp(); }
