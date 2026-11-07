// silicon - IPv6: link-local and SLAAC addresses, neighbour discovery, ICMPv6 (ping), UDP, a
// AAAA resolver, and the hooks TCP uses. Everything is driven from net_poll() like the IPv4 side.
#include "net.h"

bool net_eth_send(const u8 dst[6], u16 type, const void* payload, u32 len);   // core.cpp
void tcp_receive6(const u8* src, const u8* d, u32 len);

Ip6Config ip6cfg;

struct __attribute__((packed)) Ip6Hdr {
    u32 vtf;                 // version, traffic class, flow label
    u16 plen;
    u8  next, hops;
    u8  src[16], dst[16];
};

static const u8 ALL_NODES[16] = {0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
static const u8 ALL_ROUTERS[16] = {0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};

static bool is_multicast(const u8* a) { return a[0] == 0xFF; }
static bool is_linklocal(const u8* a) { return a[0] == 0xFE && (a[1] & 0xC0) == 0x80; }
static bool eq16(const u8* a, const u8* b) { return !memcmp(a, b, 16); }
static bool is_zero16(const u8* a) { for (int i = 0; i < 16; i++) if (a[i]) return false; return true; }

static void mc_mac(const u8* ip, u8 mac[6]) { mac[0] = mac[1] = 0x33; memcpy(mac + 2, ip + 12, 4); }
static void solicited_node(const u8* target, u8 out[16]) {
    memset(out, 0, 16);
    out[0] = 0xFF; out[1] = 0x02; out[11] = 0x01; out[12] = 0xFF;
    memcpy(out + 13, target + 13, 3);
}

void ip6_to_str(const u8 a[16], char* out, int n) {
    // longest run of zero groups becomes ::
    u16 g[8];
    for (int i = 0; i < 8; i++) g[i] = (a[i * 2] << 8) | a[i * 2 + 1];
    int best = -1, blen = 0;
    for (int i = 0; i < 8;) {
        if (g[i]) { i++; continue; }
        int j = i;
        while (j < 8 && !g[j]) j++;
        if (j - i > blen && j - i >= 2) { best = i; blen = j - i; }
        i = j;
    }
    int o = 0;
    for (int i = 0; i < 8 && o < n - 8; i++) {
        if (i == best) { out[o++] = ':'; if (i == 0) out[o++] = ':'; i += blen - 1; continue; }
        o += snprintf(out + o, n - o, "%x", g[i]);
        if (i != 7) out[o++] = ':';
    }
    out[o] = 0;
}

bool str_to_ip6(const char* s, u8 out[16]) {
    u16 g[8];
    int ng = 0, gap = -1;
    memset(g, 0, sizeof(g));
    if (*s == '[') s++;
    if (s[0] == ':' && s[1] == ':') { gap = 0; s += 2; }
    else if (s[0] == ':') return false;
    while (*s && *s != ']' && *s != '%') {
        if (ng >= 8) return false;
        int v = 0, digits = 0;
        while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')) {
            int d = *s <= '9' ? *s - '0' : (*s | 0x20) - 'a' + 10;
            v = v * 16 + d;
            digits++; s++;
            if (digits > 4) return false;
        }
        if (!digits) return false;
        g[ng++] = (u16)v;
        if (*s == ':') {
            s++;
            if (*s == ':') { if (gap >= 0) return false; gap = ng; s++; }
        } else if (*s && *s != ']' && *s != '%') return false;
    }
    if (gap < 0 && ng != 8) return false;
    if (gap >= 0) {
        int fill = 8 - ng;
        if (fill < 1) return false;
        for (int i = ng - 1; i >= gap; i--) g[i + fill] = g[i];
        for (int i = gap; i < gap + fill; i++) g[i] = 0;
    }
    for (int i = 0; i < 8; i++) { out[i * 2] = g[i] >> 8; out[i * 2 + 1] = g[i] & 0xFF; }
    return true;
}

// ---------------------------------------------------------------- checksum
static u32 sum16(const u8* d, u32 n, u32 sum) {
    for (u32 i = 0; i + 1 < n; i += 2) sum += (d[i] << 8) | d[i + 1];
    if (n & 1) sum += d[n - 1] << 8;
    return sum;
}
u16 ip6_checksum(const u8* src, const u8* dst, u8 next, const u8* data, u32 len) {
    u32 sum = sum16(src, 16, 0);
    sum = sum16(dst, 16, sum);
    sum += len >> 16; sum += len & 0xFFFF;
    sum += next;
    sum = sum16(data, len, sum);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    u16 c = (u16)~sum;
    return c;
}

// ---------------------------------------------------------------- neighbours
struct Neigh { u8 ip[16]; u8 mac[6]; bool valid; u64 t; };
static Neigh neigh[16];
struct Pend6 { u8 hop[16]; u8 data[1500]; u32 len; u64 t0, last; int tries; bool used; };
static Pend6 pend[6];

static Neigh* neigh_find(const u8* ip) { for (auto& n : neigh) if (n.valid && eq16(n.ip, ip)) return &n; return nullptr; }

static bool send_raw(const u8 mac[6], const u8* packet, u32 len) { return net_eth_send(mac, 0x86DD, packet, len); }

static void build_and_send(const u8* src, const u8* dst, u8 next, u8 hops, const u8* payload, u32 len, const u8 mac[6]) {
    u8 pkt[1500];
    if (len + 40 > sizeof(pkt)) return;
    Ip6Hdr* h = (Ip6Hdr*)pkt;
    h->vtf = htonl(0x60000000);
    h->plen = htons((u16)len);
    h->next = next;
    h->hops = hops;
    memcpy(h->src, src, 16);
    memcpy(h->dst, dst, 16);
    memcpy(pkt + 40, payload, len);
    send_raw(mac, pkt, 40 + len);
}

static void send_ns(const u8* target) {
    u8 sn[16];
    solicited_node(target, sn);
    u8 p[32];
    memset(p, 0, sizeof(p));
    p[0] = 135;
    memcpy(p + 8, target, 16);
    p[24] = 1; p[25] = 1;                           // source link-layer address option
    memcpy(p + 26, netcfg.mac, 6);
    u16 c = ip6_checksum(ip6cfg.ll, sn, 58, p, 32);
    p[2] = c >> 8; p[3] = c & 0xFF;
    u8 mac[6];
    mc_mac(sn, mac);
    build_and_send(ip6cfg.ll, sn, 58, 255, p, 32, mac);
}

static void send_rs() {
    u8 p[16];
    memset(p, 0, sizeof(p));
    p[0] = 133;
    p[8] = 1; p[9] = 1;
    memcpy(p + 10, netcfg.mac, 6);
    u16 c = ip6_checksum(ip6cfg.ll, ALL_ROUTERS, 58, p, 16);
    p[2] = c >> 8; p[3] = c & 0xFF;
    u8 mac[6];
    mc_mac(ALL_ROUTERS, mac);
    build_and_send(ip6cfg.ll, ALL_ROUTERS, 58, 255, p, 16, mac);
}

const u8* ip6_source_for(const u8* dst) {
    if (is_linklocal(dst) || is_multicast(dst) || !ip6cfg.has_global) return ip6cfg.ll;
    return ip6cfg.global;
}

bool ip6_send(const u8* dst, u8 next, const void* payload, u32 len) {
    if (!ip6cfg.ready) return false;
    if (!fw_check6(false, next, dst, (const u8*)payload, len)) return false;
    const u8* src = ip6_source_for(dst);
    u8 mac[6];
    if (is_multicast(dst)) {
        mc_mac(dst, mac);
        build_and_send(src, dst, next, 64, (const u8*)payload, len, mac);
        return true;
    }
    const u8* hop = dst;
    bool onlink = is_linklocal(dst) || (ip6cfg.has_global && !memcmp(dst, ip6cfg.prefix, 8));
    if (!onlink) {
        if (!ip6cfg.has_router) return false;
        hop = ip6cfg.router;
    }
    // our own address: loop back
    if (eq16(dst, ip6cfg.ll) || (ip6cfg.has_global && eq16(dst, ip6cfg.global))) return false;
    Neigh* n = neigh_find(hop);
    if (n) { build_and_send(src, dst, next, 64, (const u8*)payload, len, n->mac); return true; }
    // not resolved yet: hold the packet and ask
    if (len + 40 > sizeof(pend[0].data)) return false;
    Pend6* slot = nullptr;
    for (auto& p : pend) if (!p.used) { slot = &p; break; }
    if (!slot) slot = &pend[0];
    slot->used = true;
    memcpy(slot->hop, hop, 16);
    Ip6Hdr* h = (Ip6Hdr*)slot->data;
    h->vtf = htonl(0x60000000);
    h->plen = htons((u16)len);
    h->next = next; h->hops = 64;
    memcpy(h->src, src, 16); memcpy(h->dst, dst, 16);
    memcpy(slot->data + 40, payload, len);
    slot->len = 40 + len;
    slot->t0 = slot->last = ticks();
    slot->tries = 1;
    send_ns(hop);
    return true;
}

static void learn(const u8* ip, const u8 mac[6]) {
    Neigh* n = neigh_find(ip);
    if (!n) {
        int oldest = 0;
        for (int i = 0; i < 16; i++) { if (!neigh[i].valid) { oldest = i; break; } if (neigh[i].t < neigh[oldest].t) oldest = i; }
        n = &neigh[oldest];
    }
    memcpy(n->ip, ip, 16); memcpy(n->mac, mac, 6);
    n->valid = true; n->t = ticks();
    for (auto& p : pend) {
        if (!p.used || !eq16(p.hop, ip)) continue;
        send_raw(mac, p.data, p.len);
        p.used = false;
    }
}

// ---------------------------------------------------------------- ICMPv6
struct PingWait { u16 id, seq; u64 t0; Ping6Reply cb; void* ctx; bool used; };
static PingWait pings[8];

bool ip6_ping(const u8* dst, u16 id, u16 seq, Ping6Reply cb, void* ctx) {
    u8 p[16];
    memset(p, 0, sizeof(p));
    p[0] = 128;
    p[4] = id >> 8; p[5] = id & 0xFF; p[6] = seq >> 8; p[7] = seq & 0xFF;
    for (int i = 8; i < 16; i++) p[i] = (u8)('a' + i);
    u16 c = ip6_checksum(ip6_source_for(dst), dst, 58, p, 16);
    p[2] = c >> 8; p[3] = c & 0xFF;
    for (auto& w : pings) if (!w.used) { w.used = true; w.id = id; w.seq = seq; w.t0 = ticks(); w.cb = cb; w.ctx = ctx; break; }
    return ip6_send(dst, 58, p, 16);
}

static void icmp6_receive(const Ip6Hdr* h, const u8* d, u32 len, const u8 src_mac[6]) {
    if (len < 8) return;
    if (ip6_checksum(h->src, h->dst, 58, d, len) != 0) return;
    u8 type = d[0];
    switch (type) {
    case 128: {                                      // echo request
        u8* r = (u8*)kmalloc(len);
        memcpy(r, d, len);
        r[0] = 129; r[2] = r[3] = 0;
        const u8* me = is_linklocal(h->dst) ? ip6cfg.ll : (ip6cfg.has_global ? ip6cfg.global : ip6cfg.ll);
        u16 c = ip6_checksum(me, h->src, 58, r, len);
        r[2] = c >> 8; r[3] = c & 0xFF;
        Neigh* n = neigh_find(h->src);
        if (!n && is_linklocal(h->src)) learn(h->src, src_mac);
        ip6_send(h->src, 58, r, len);
        kfree(r);
        break;
    }
    case 129: {
        u16 id = (d[4] << 8) | d[5], seq = (d[6] << 8) | d[7];
        for (auto& w : pings)
            if (w.used && w.id == id && w.seq == seq) { w.used = false; if (w.cb) w.cb(w.ctx, seq, (u32)(ticks() - w.t0), h->src, h->hops); break; }
        break;
    }
    case 134: {                                      // router advertisement
        if (len < 16) break;
        memcpy(ip6cfg.router, h->src, 16);
        ip6cfg.has_router = d[6] || d[7];
        learn(h->src, src_mac);
        for (u32 o = 16; o + 2 <= len;) {
            u8 ot = d[o], ol = d[o + 1] * 8;
            if (!ol || o + ol > len) break;
            if (ot == 1 && ol >= 8) learn(h->src, d + o + 2);
            if (ot == 3 && ol >= 32 && d[o + 2] == 64 && (d[o + 3] & 0x40)) {          // prefix, autonomous
                memcpy(ip6cfg.prefix, d + o + 16, 8);
                memset(ip6cfg.prefix + 8, 0, 8);
                memcpy(ip6cfg.global, ip6cfg.prefix, 8);
                memcpy(ip6cfg.global + 8, ip6cfg.ll + 8, 8);
                ip6cfg.has_global = true;
            }
            if (ot == 25 && ol >= 24) { memcpy(ip6cfg.dns, d + o + 8, 16); ip6cfg.has_dns = true; }
            o += ol;
        }
        break;
    }
    case 135: {                                      // neighbour solicitation
        if (len < 24) break;
        const u8* target = d + 8;
        bool mine = eq16(target, ip6cfg.ll) || (ip6cfg.has_global && eq16(target, ip6cfg.global));
        if (!mine) break;
        if (!is_zero16(h->src)) {
            for (u32 o = 24; o + 8 <= len; o += 8) if (d[o] == 1 && d[o + 1] == 1) learn(h->src, d + o + 2);
            u8 p[32];
            memset(p, 0, sizeof(p));
            p[0] = 136;
            p[4] = 0x60;                             // solicited, override
            memcpy(p + 8, target, 16);
            p[24] = 2; p[25] = 1;
            memcpy(p + 26, netcfg.mac, 6);
            u16 c = ip6_checksum(target, h->src, 58, p, 32);
            p[2] = c >> 8; p[3] = c & 0xFF;
            Neigh* n = neigh_find(h->src);
            if (n) build_and_send(target, h->src, 58, 255, p, 32, n->mac);
        }
        break;
    }
    case 136: {                                      // neighbour advertisement
        if (len < 24) break;
        const u8* target = d + 8;
        const u8* mac = src_mac;
        for (u32 o = 24; o + 8 <= len; o += 8) if (d[o] == 2 && d[o + 1] == 1) mac = d + o + 2;
        learn(target, mac);
        break;
    }
    }
}

// ---------------------------------------------------------------- UDP
struct Bind6 { u16 port; Udp6Handler h; };
static Bind6 binds6[8];

bool udp6_bind(u16 port, Udp6Handler h) {
    for (auto& b : binds6) if (!b.port) { b.port = port; b.h = h; return true; }
    return false;
}
void udp6_unbind(u16 port) { for (auto& b : binds6) if (b.port == port) b.port = 0; }

bool udp6_send(const u8* dst, u16 sport, u16 dport, const void* data, u32 len) {
    if (len > 1400) return false;
    u8 p[1440];
    p[0] = sport >> 8; p[1] = sport & 0xFF; p[2] = dport >> 8; p[3] = dport & 0xFF;
    u16 ul = (u16)(8 + len);
    p[4] = ul >> 8; p[5] = ul & 0xFF;
    p[6] = p[7] = 0;
    memcpy(p + 8, data, len);
    u16 c = ip6_checksum(ip6_source_for(dst), dst, 17, p, ul);
    if (!c) c = 0xFFFF;
    p[6] = c >> 8; p[7] = c & 0xFF;
    return ip6_send(dst, 17, p, ul);
}

// ---------------------------------------------------------------- receive
void ip6_receive(const u8* d, u32 len, const u8 src_mac[6]) {
    if (!ip6cfg.ready || len < 40) return;
    const Ip6Hdr* h = (const Ip6Hdr*)d;
    if ((ntohl(h->vtf) >> 28) != 6) return;
    u32 pl = ntohs(h->plen);
    if (pl + 40 > len) return;
    bool mine = eq16(h->dst, ip6cfg.ll) || (ip6cfg.has_global && eq16(h->dst, ip6cfg.global)) || eq16(h->dst, ALL_NODES);
    if (!mine && is_multicast(h->dst)) {                      // our solicited-node group
        u8 sn[16], sn2[16];
        solicited_node(ip6cfg.ll, sn);
        solicited_node(ip6cfg.has_global ? ip6cfg.global : ip6cfg.ll, sn2);
        mine = eq16(h->dst, sn) || eq16(h->dst, sn2);
    }
    if (!mine) return;
    const u8* p = d + 40;
    if (!fw_check6(true, h->next, h->src, p, pl)) return;
    switch (h->next) {
    case 58: icmp6_receive(h, p, pl, src_mac); break;
    case 17:
        if (pl >= 8) {
            u16 sport = (p[0] << 8) | p[1], dport = (p[2] << 8) | p[3];
            for (auto& b : binds6) if (b.port == dport && b.h) { b.h(h->src, sport, p + 8, pl - 8); break; }
        }
        break;
    case 6: tcp_receive6(h->src, p, pl); break;
    }
}

// ---------------------------------------------------------------- AAAA lookups
struct Q6 { char name[128]; u16 id; u64 sent; int tries, state; u8 ip[16]; bool used; };   // state 0 wait, 1 ok, -1 fail
static Q6 q6[4];
struct C6 { char name[128]; u8 ip[16]; bool used; };
static C6 c6[16];
static const u16 DNS6_PORT = 53056;
static bool dns6_bound = false;

static void dns6_send(Q6& q) {
    u8 p[300];
    int o = 0;
    p[o++] = q.id >> 8; p[o++] = q.id & 0xFF;
    p[o++] = 1; p[o++] = 0; p[o++] = 0; p[o++] = 1;
    for (int i = 0; i < 6; i++) p[o++] = 0;
    for (const char* s = q.name; *s;) {
        const char* dot = strchr(s, '.');
        int l = dot ? (int)(dot - s) : (int)strlen(s);
        if (l > 63 || o + l + 2 > 280) break;
        p[o++] = (u8)l; memcpy(p + o, s, l); o += l; s += l; if (*s == '.') s++;
    }
    p[o++] = 0; p[o++] = 0; p[o++] = 28; p[o++] = 0; p[o++] = 1;
    q.sent = ticks(); q.tries++;
    if (ip6cfg.has_dns) udp6_send(ip6cfg.dns, DNS6_PORT, 53, p, o);
    else if (netcfg.configured) udp_send(netcfg.dns, DNS6_PORT, 53, p, o);          // the IPv4 resolver answers AAAA too
}

static u32 skipn(const u8* d, u32 len, u32 o) {
    while (o < len) { u8 l = d[o]; if (!l) return o + 1; if ((l & 0xC0) == 0xC0) return o + 2; o += l + 1; }
    return len;
}

static void dns6_rx(const u8* d, u32 len) {
    if (len < 12) return;
    u16 id = (d[0] << 8) | d[1];
    Q6* q = nullptr;
    for (auto& x : q6) if (x.used && x.state == 0 && x.id == id) q = &x;
    if (!q) return;
    u16 qd = (d[4] << 8) | d[5], an = (d[6] << 8) | d[7];
    u32 o = 12;
    for (int i = 0; i < qd; i++) o = skipn(d, len, o) + 4;
    q->state = -1;
    for (int i = 0; i < an && o + 10 <= len; i++) {
        o = skipn(d, len, o);
        if (o + 10 > len) break;
        u16 type = (d[o] << 8) | d[o + 1], rl = (d[o + 8] << 8) | d[o + 9];
        o += 10;
        if (type == 28 && rl == 16 && o + 16 <= len) {
            memcpy(q->ip, d + o, 16);
            q->state = 1;
            for (auto& c : c6) if (!c.used) { c.used = true; strlcpy(c.name, q->name, sizeof(c.name)); memcpy(c.ip, q->ip, 16); break; }
            break;
        }
        o += rl;
    }
}
static void dns6_rx6(const u8*, u16, const u8* d, u32 len) { dns6_rx(d, len); }
static void dns6_rx4(ip4, u16, const u8* d, u32 len) { dns6_rx(d, len); }

// 1 resolved, 0 working, -1 failed (call repeatedly)
int dns6_resolve(const char* name, u8 out[16]) {
    if (!dns6_bound) { dns6_bound = true; udp6_bind(DNS6_PORT, dns6_rx6); udp_bind(DNS6_PORT, dns6_rx4); }
    for (auto& c : c6) if (c.used && !strcmp(c.name, name)) { memcpy(out, c.ip, 16); return 1; }
    for (auto& q : q6) if (q.used && !strcmp(q.name, name)) {
        if (q.state == 1) { memcpy(out, q.ip, 16); q.used = false; return 1; }
        if (q.state == -1) { q.used = false; return -1; }
        return 0;
    }
    for (auto& q : q6) if (!q.used) {
        memset(&q, 0, sizeof(q));
        q.used = true; q.id = (u16)rand(); strlcpy(q.name, name, sizeof(q.name));
        dns6_send(q);
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------- housekeeping
static u64 rs_t = 0;
static int rs_tries = 0;

void ip6_init() {
    memset(&ip6cfg, 0, sizeof(ip6cfg));
    ip6cfg.ll[0] = 0xFE; ip6cfg.ll[1] = 0x80;
    ip6cfg.ll[8] = netcfg.mac[0] ^ 2; ip6cfg.ll[9] = netcfg.mac[1]; ip6cfg.ll[10] = netcfg.mac[2];
    ip6cfg.ll[11] = 0xFF; ip6cfg.ll[12] = 0xFE;
    ip6cfg.ll[13] = netcfg.mac[3]; ip6cfg.ll[14] = netcfg.mac[4]; ip6cfg.ll[15] = netcfg.mac[5];
    ip6cfg.ready = true;
    rs_t = 0;
    rs_tries = 0;
}

void ip6_tick() {
    if (!ip6cfg.ready) return;
    u64 t = ticks();
    if (!ip6cfg.has_router && rs_tries < 6 && t - rs_t > 1500) { rs_t = t; rs_tries++; send_rs(); }
    for (auto& p : pend) {
        if (!p.used) continue;
        if (t - p.t0 > 3000 || p.tries > 3) { p.used = false; continue; }
        if (t - p.last > 800) { p.last = t; p.tries++; send_ns(p.hop); }
    }
    for (auto& w : pings) if (w.used && t - w.t0 > 5000) { w.used = false; if (w.cb) w.cb(w.ctx, w.seq, 0xFFFFFFFF, nullptr, 0); }
    for (auto& q : q6) if (q.used && q.state == 0 && t - q.sent > 1500) { if (q.tries >= 3) q.state = -1; else dns6_send(q); }
}
