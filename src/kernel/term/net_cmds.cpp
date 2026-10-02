// silicon - network shell commands: ifconfig, ping, nslookup, curl, wget, netstat, lspci, dhcp
#include "shell.h"
#include "../net/net.h"
#include "../drv/pci.h"

// ================================================================ ifconfig
void cmd_ifconfig(Shell& sh, int, char**) {
    if (!netcfg.up) { sh.err("no network card found (start QEMU with -device rtl8139)\n"); return; }
    char ip[16], mask[16], gw[16], dns[16];
    ip_to_str(netcfg.ip, ip); ip_to_str(netcfg.mask, mask); ip_to_str(netcfg.gateway, gw); ip_to_str(netcfg.dns, dns);
    u64 txp, rxp, txb, rxb;
    net_stats(&txp, &rxp, &txb, &rxb);
    sh.outf(C_BWHITE "eth0" C_RESET ": flags=<UP,BROADCAST,RUNNING> mtu 1500  " C_GRAY "rtl8139\n" C_RESET);
    if (netcfg.configured) sh.outf("      inet " C_BGREEN "%s" C_RESET "  netmask %s  %s\n", ip, mask, netcfg.dhcp ? "(dhcp)" : "(static)");
    else sh.out(C_BYEL "      waiting for an address (dhcp)...\n" C_RESET);
    sh.outf("      ether %x:%x:%x:%x:%x:%x\n", netcfg.mac[0], netcfg.mac[1], netcfg.mac[2], netcfg.mac[3], netcfg.mac[4], netcfg.mac[5]);
    sh.outf("      gateway %s  dns %s\n", gw, dns);
    sh.outf("      RX packets %u  bytes %u\n      TX packets %u  bytes %u\n", (u32)rxp, (u32)rxb, (u32)txp, (u32)txb);
}

void cmd_dhcp(Shell& sh, int, char**) {
    if (!netcfg.up) { sh.err("no network card\n"); return; }
    netcfg.configured = false;
    net_dhcp_start();
    sh.out("asking for an address... (check with ifconfig)\n");
}

void cmd_lspci(Shell& sh, int, char**) {
    for (int i = 0; i < pci_count(); i++) {
        PciDevice* d = pci_get(i);
        const char* extra = d->vendor == 0x10EC && d->device == 0x8139 ? "  " C_BGREEN "<- rtl8139 (silicon driver)" C_RESET
                          : d->vendor == 0x1234 && d->device == 0x1111 ? "  " C_GRAY "<- bochs/qemu vga" C_RESET : "";
        sh.outf("%02x:%02x.%u  %04x:%04x  %s%s\n", d->bus, d->dev, d->fn, d->vendor, d->device, d->name, extra);
    }
}

void cmd_netstat(Shell& sh, int, char**) {
    sh.out(C_BWHITE "proto  local port  remote                 state         rx      tx\n" C_RESET);
    int n = 0;
    for (int i = 0; i < tcp_socket_count(); i++) {
        int st; ip4 rip; u16 rp, lp; u32 rx, tx;
        if (!tcp_socket_info(i, &st, &rip, &rp, &lp, &rx, &tx)) continue;
        char ip[16], rem[32];
        ip_to_str(rip, ip);
        snprintf(rem, sizeof(rem), "%s:%u", ip, rp);
        sh.outf("tcp    %-10u  %-21s  %-12s  %-6u  %u\n", lp, rem, tcp_state_name(st), rx, tx);
        n++;
    }
    if (!n) sh.out(C_GRAY "no open connections\n" C_RESET);
}

// ================================================================ ping (a console program)
struct Pinger : ConsoleProgram {
    Shell& sh;
    char host[128];
    ip4 ip = 0;
    int phase = 0;          // 0 resolving, 1 pinging, 2 done
    int sent = 0, recvd = 0, count;
    u32 min_rtt = 0xFFFFFFFF, max_rtt = 0, sum_rtt = 0;
    u64 t_last = 0, t0;
    u16 id;
    bool finished = false;

    Pinger(Shell& s, const char* h, int c) : sh(s), count(c) { strlcpy(host, h, sizeof(host)); t0 = ticks(); id = (u16)rand(); }

    static void reply(void* ctx, u16 seq, u32 rtt, ip4 from, u8 ttl) {
        Pinger* p = (Pinger*)ctx;
        char ip[16];
        ip_to_str(from, ip);
        p->recvd++;
        p->min_rtt = min(p->min_rtt, rtt);
        p->max_rtt = max(p->max_rtt, rtt);
        p->sum_rtt += rtt;
        p->sh.outf("64 bytes from " C_BWHITE "%s" C_RESET ": icmp_seq=%u ttl=%u time=" C_BGREEN "%u ms\n" C_RESET, ip, seq, ttl, rtt);
    }
    void summary() {
        sh.outf(C_GRAY "--- %s ping statistics ---\n" C_RESET, host);
        sh.outf("%d packets transmitted, %d received, %d%% packet loss\n", sent, recvd, sent ? (sent - recvd) * 100 / sent : 0);
        if (recvd) sh.outf("rtt min/avg/max = %u/%u/%u ms\n", min_rtt, sum_rtt / recvd, max_rtt);
        if (!recvd) sh.status = 1;
        finished = true;
    }
    void on_key(const KeyEvent& e) override {
        if (e.ctrl && e.key == 'c') { sh.out("^C\n"); if (phase == 1) summary(); else finished = true; }
    }
    void tick() override {
        if (finished) return;
        if (phase == 0) {
            int st = dns_resolve(host, &ip);
            if (st < 0 || (st == 0 && ticks() - t0 > 8000)) { sh.err("ping: %s: name or service not known\n", host); finished = true; return; }
            if (st > 0) {
                char s[16];
                ip_to_str(ip, s);
                sh.outf("PING %s (%s) 56 data bytes\n", host, s);
                phase = 1;
            }
            return;
        }
        if (sent < count && (sent == 0 || ticks() - t_last >= 1000)) {
            sent++;
            t_last = ticks();
            icmp_ping(ip, id, (u16)sent, reply, this);
        }
        if (sent >= count && ticks() - t_last > 1500) summary();
    }
    bool done() override { return finished; }
};

void cmd_ping(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: ping [-c count] <host>\n"); return; }
    int count = 4;
    const char* host = argv[argc - 1];
    for (int i = 1; i < argc - 1; i++) if (!strcmp(argv[i], "-c")) count = atoi(argv[i + 1]);
    if (!net_link_up()) { sh.err("ping: network is unreachable\n"); return; }
    sh.prog_keep_screen = true;
    sh.start_program(new Pinger(sh, host, clamp(count, 1, 100)));
}

// ================================================================ nslookup
struct Lookup : ConsoleProgram {
    Shell& sh;
    char host[128];
    u64 t0;
    bool finished = false;
    Lookup(Shell& s, const char* h) : sh(s) { strlcpy(host, h, sizeof(host)); t0 = ticks(); }
    void on_key(const KeyEvent& e) override { if (e.ctrl && e.key == 'c') finished = true; }
    void tick() override {
        ip4 ip;
        int st = dns_resolve(host, &ip);
        if (st == 0 && ticks() - t0 < 8000) return;
        char dns[16];
        ip_to_str(netcfg.dns, dns);
        sh.outf(C_GRAY "Server:  %s\n" C_RESET, dns);
        if (st > 0) { char s[16]; ip_to_str(ip, s); sh.outf("Name:    %s\nAddress: " C_BGREEN "%s\n" C_RESET, host, s); }
        else sh.err("** server can't find %s\n", host);
        finished = true;
    }
    bool done() override { return finished; }
};

void cmd_nslookup(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: nslookup <host>\n"); return; }
    if (!net_link_up()) { sh.err("nslookup: network is unreachable\n"); return; }
    sh.prog_keep_screen = true;
    sh.start_program(new Lookup(sh, argv[argc - 1]));
}

// ================================================================ curl / wget
struct Fetcher : ConsoleProgram {
    Shell& sh;
    HttpRequest* req;
    char save_to[128];
    bool headers_only, show_progress, finished = false;
    int last_phase = -1;
    u64 last_report = 0, t_start = ticks();

    Fetcher(Shell& s, const char* url, const char* out, bool head, bool progress) : sh(s), headers_only(head), show_progress(progress) {
        strlcpy(save_to, out ? out : "", sizeof(save_to));
        req = http_get(url, head ? "HEAD" : "GET");
    }
    ~Fetcher() { http_free(req); }
    void on_key(const KeyEvent& e) override { if (e.ctrl && e.key == 'c') { sh.out("^C\n"); finished = true; } }
    void tick() override {
        if (finished) return;
        int ph = http_step(req);
        if (show_progress && ph != last_phase) {
            static const char* names[] = {"resolving", "connecting", "securing (tls 1.3)", "sending request", "receiving", "", ""};
            if (ph <= HTTP_RECEIVING) sh.outf(C_GRAY "%s" G_ELLIP "\n" C_RESET, names[ph]);
            last_phase = ph;
        }
        if (show_progress && ph == HTTP_RECEIVING && ticks() - last_report > 700) {
            last_report = ticks();
            u32 got = http_received(req), exp = http_expected(req);
            if (exp) sh.outf(C_GRAY "  %u / %u bytes\n" C_RESET, got, exp);
            else if (got) sh.outf(C_GRAY "  %u bytes\n" C_RESET, got);
        }
        if (ph == HTTP_ERROR) { sh.err("curl: %s\n", http_error(req)); finished = true; return; }
        if (ph != HTTP_DONE) return;
        u32 len;
        const u8* body = http_body(req, &len);
        if (headers_only) {
            sh.outf("HTTP/1.1 %d\n", http_status(req));
            const char* hs[] = {"content-type", "content-length", "server", "date", "location", "last-modified"};
            for (const char* h : hs) { const char* v = http_header(req, h); if (v) sh.outf("%s: %s\n", h, v); }
        } else if (save_to[0]) {
            FsNode* f = fs_lookup(save_to, sh.cwd);
            if (!f) f = fs_create_path(save_to, false, sh.cwd);
            if (!f || f->dir) sh.err("cannot write %s\n", save_to);
            else {
                fs_write(f, body, len);
                u64 ms = max<u64>(1, ticks() - t_start);
                sh.outf(C_BGREEN "saved" C_RESET " %u bytes to %s " C_GRAY "(HTTP %d, %s, %u.%u s, %u KB/s)\n" C_RESET, len, save_to,
                        http_status(req), http_content_type(req), (u32)(ms / 1000), (u32)(ms % 1000 / 100), (u32)((u64)len * 1000 / 1024 / ms));
            }
        } else {
            // print text bodies; keep the terminal tidy by dropping control characters
            char chunk[1025];
            u32 o = 0;
            while (o < len) {
                u32 n = min<u32>(1024, len - o);
                for (u32 i = 0; i < n; i++) { char c = (char)body[o + i]; chunk[i] = (c == '\n' || c == '\t' || ((u8)c >= 32 && (u8)c < 127)) ? c : (c == '\r' ? ' ' : '?'); }
                chunk[n] = 0;
                sh.out(chunk);
                o += n;
            }
            if (len && body[len - 1] != '\n') sh.out("\n");
        }
        finished = true;
    }
    bool done() override { return finished; }
};

void cmd_curl(Shell& sh, int argc, char** argv) {
    const char* url = nullptr;
    const char* out = nullptr;
    bool head = false, verbose = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-I")) head = true;
        else if (!strcmp(argv[i], "-v")) verbose = true;
        else url = argv[i];
    }
    if (!url) { sh.out("usage: curl [-I] [-v] [-o file] <url>\n"); return; }
    if (!net_link_up()) { sh.err("curl: network is unreachable\n"); return; }
    sh.prog_keep_screen = true;
    sh.start_program(new Fetcher(sh, url, out, head, verbose));
}

void cmd_wget(Shell& sh, int argc, char** argv) {
    const char* url = nullptr;
    const char* out = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc) out = argv[++i];
        else url = argv[i];
    }
    if (!url) { sh.out("usage: wget [-O file] <url>\n"); return; }
    if (!net_link_up()) { sh.err("wget: network is unreachable\n"); return; }
    char name[64];
    if (!out) {
        Url u;
        url_parse(url, u);
        const char* slash = strrchr(u.path, '/');
        strlcpy(name, slash && slash[1] ? slash + 1 : "index.html", sizeof(name));
        char* q = strchr(name, '?');
        if (q) *q = 0;
        out = name;
    }
    sh.outf("--> %s\n", url);
    sh.prog_keep_screen = true;
    sh.start_program(new Fetcher(sh, url, out, false, true));
}
