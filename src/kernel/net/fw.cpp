// silicon - firewall (a stateless packet filter on the IP layer) and the HTTP proxy setting
#include "net.h"
#include "../fs/fs.h"
#include "../drv/drivers.h"

bool fw_enabled = true;
bool fw_out_default_allow = true;
static Vec<FwRule> rules;
static FwLogEntry log_buf[40];
static int log_n = 0, log_head = 0;
static u32 blocked_in = 0, blocked_out = 0;
static bool loaded = false;

static const char* FW_FILE = "/home/.config/firewall";

static void fw_load() {
    loaded = true;
    rules.clear();
    FsNode* f = fs_lookup(FW_FILE);
    if (!f) return;
    const char* t = fs_read_text(f);
    while (*t) {
        const char* e = strchr(t, '\n');
        int l = e ? (int)(e - t) : (int)strlen(t);
        char line[120];
        memcpy(line, t, min(l, 119));
        line[min(l, 119)] = 0;
        if (str_starts(line, "enabled=")) fw_enabled = atoi(line + 8) != 0;
        else if (str_starts(line, "out=")) fw_out_default_allow = line[4] == 'a';
        else {
            // allow|dir|proto|port|ip|mask
            char* f[6];
            int n = 0;
            f[n++] = line;
            for (char* p = line; *p && n < 6; p++) if (*p == '|') { *p = 0; f[n++] = p + 1; }
            if (n == 6) {
                FwRule r;
                r.allow = f[0][0] == 'a';
                r.dir = (u8)atoi(f[1]); r.proto = (u8)atoi(f[2]); r.port = (u16)atoi(f[3]);
                if (!str_to_ip(f[4], &r.ip)) r.ip = 0;
                if (!str_to_ip(f[5], &r.mask)) r.mask = 0;
                r.hits = 0;
                rules.push(r);
            }
        }
        if (!e) break;
        t = e + 1;
    }
}
static void fw_ensure() { if (!loaded) fw_load(); }

void fw_save() {
    fw_ensure();
    char* buf = (char*)kmalloc(rules.len * 80 + 64);
    int o = snprintf(buf, 64, "enabled=%d\nout=%s\n", fw_enabled ? 1 : 0, fw_out_default_allow ? "allow" : "deny");
    for (int i = 0; i < rules.len; i++) {
        char a[20], b[20];
        ip_to_str(rules[i].ip, a);
        ip_to_str(rules[i].mask, b);
        o += snprintf(buf + o, 80, "%s|%d|%d|%d|%s|%s\n", rules[i].allow ? "allow" : "deny", rules[i].dir, rules[i].proto, rules[i].port, a, b);
    }
    fs_mkdirs("/home/.config");
    fs_write_text(FW_FILE, buf);
    kfree(buf);
}

int fw_rule_count() { fw_ensure(); return rules.len; }
FwRule* fw_rule(int i) { fw_ensure(); return i >= 0 && i < rules.len ? &rules[i] : nullptr; }
void fw_add(const FwRule& r) { fw_ensure(); rules.push(r); fw_save(); }
void fw_remove(int i) { fw_ensure(); if (i >= 0 && i < rules.len) { rules.remove(i); fw_save(); } }
void fw_swap(int i, int j) { fw_ensure(); if (i >= 0 && j >= 0 && i < rules.len && j < rules.len) { swap(rules[i], rules[j]); fw_save(); } }
void fw_clear_rules() { fw_ensure(); rules.clear(); fw_save(); }
int fw_log_count() { return log_n; }
const FwLogEntry* fw_log(int i) { return i >= 0 && i < log_n ? &log_buf[(log_head - 1 - i + 40 * 2) % 40] : nullptr; }   // 0 = newest
void fw_log_clear() { log_n = log_head = 0; blocked_in = blocked_out = 0; }
u32 fw_blocked_in() { return blocked_in; }
u32 fw_blocked_out() { return blocked_out; }

bool fw_check(bool inbound, u8 proto, ip4 remote, u16 port) {
    fw_ensure();
    if (!fw_enabled) return true;
    if (proto == 17 && (port == 67 || port == 68)) return true;      // DHCP always works
    int hit = -1;
    bool allow = inbound ? true : fw_out_default_allow;
    for (int i = 0; i < rules.len; i++) {
        FwRule& r = rules[i];
        if (!(r.dir & (inbound ? 1 : 2))) continue;
        if (r.proto && r.proto != proto) continue;
        if (r.port && r.port != port) continue;
        if (r.mask && (remote & r.mask) != (r.ip & r.mask)) continue;
        hit = i;
        allow = r.allow;
        r.hits++;
        break;
    }
    if (allow) return true;
    if (inbound) blocked_in++; else blocked_out++;
    FwLogEntry& e = log_buf[log_head];
    e.stamp = rtc_stamp(); e.inbound = inbound; e.proto = proto; e.remote = remote; e.port = port; e.rule = hit;
    log_head = (log_head + 1) % 40;
    if (log_n < 40) log_n++;
    return false;
}

// ---- proxy: "host:port" in /home/.config/proxy; empty or missing = connect directly
bool net_proxy_get(char* host, int n, u16* port) {
    FsNode* f = fs_lookup("/home/.config/proxy");
    if (!f || !f->size) return false;
    char line[160];
    const char* t = fs_read_text(f);
    int l = 0;
    while (t[l] && t[l] != '\n' && l < 159) { line[l] = t[l]; l++; }
    line[l] = 0;
    while (l && (line[l - 1] == ' ' || line[l - 1] == '\r')) line[--l] = 0;
    if (!l) return false;
    char* colon = strrchr(line, ':');
    int p = 8080;
    if (colon) { p = atoi(colon + 1); *colon = 0; }
    if (!line[0] || p <= 0 || p > 65535) return false;
    strlcpy(host, line, n);
    *port = (u16)p;
    return true;
}
void net_proxy_set(const char* hostport) {
    fs_mkdirs("/home/.config");
    fs_write_text("/home/.config/proxy", hostport ? hostport : "");
}
