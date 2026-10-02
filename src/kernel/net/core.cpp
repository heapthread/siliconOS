// silicon - ethernet, ARP, IPv4, ICMP, UDP and the DHCP client
#include "net.h"
#include "../drv/pci.h"
#include "../settings.h"

bool rtl8139_init(u8 mac_out[6]);
bool rtl8139_present();
bool rtl8139_send(const void* data, u32 len);
void rtl8139_poll();
void rtl8139_stats(u64* txp, u64* rxp, u64* txb, u64* rxb);
void tcp_receive(ip4 src, ip4 dst, const u8* seg, u32 len);
void tcp_tick();
void dns_tick();

NetConfig netcfg;
static const u8 BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ================================================================ helpers
void ip_to_str(ip4 ip, char* out) {
    snprintf(out, 16, "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
}

bool str_to_ip(const char* s, ip4* out) {
    u32 parts[4];
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return false;
        u32 v = 0;
        while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
        if (v > 255) return false;
        parts[i] = v;
        if (i < 3) { if (*s != '.') return false; s++; }
    }
    if (*s) return false;
    *out = make_ip(parts[0], parts[1], parts[2], parts[3]);
    return true;
}

u16 ip_checksum(const void* data, u32 len, u32 sum) {
    const u8* p = (const u8*)data;
    while (len > 1) { sum += (p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((u16)~sum);
}

struct __attribute__((packed)) EthHdr { u8 dst[6], src[6]; u16 type; };
struct __attribute__((packed)) ArpPkt {
    u16 htype, ptype;
    u8 hlen, plen;
    u16 op;
    u8 sha[6];
    ip4 spa;
    u8 tha[6];
    ip4 tpa;
};
struct __attribute__((packed)) IpHdr {
    u8 vihl, tos;
    u16 len, id, frag;
    u8 ttl, proto;
    u16 csum;
    ip4 src, dst;
};

static bool eth_send(const u8 dst[6], u16 type, const void* payload, u32 len) {
    u8 frame[1600];
    if (len > 1500) return false;
    EthHdr* e = (EthHdr*)frame;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, netcfg.mac, 6);
    e->type = htons(type);
    memcpy(frame + sizeof(EthHdr), payload, len);
    return rtl8139_send(frame, sizeof(EthHdr) + len);
}

// ================================================================ ARP
struct ArpEntry { ip4 ip; u8 mac[6]; u64 t; bool valid; };
static ArpEntry arp_cache[32];
struct Pending { ip4 hop; u8 data[1520]; u32 len; u64 t0; u64 last_req; int tries; bool used; };
static Pending pending[16];

static void arp_send(u16 op, const u8 tha[6], ip4 tpa, const u8 dst[6]) {
    ArpPkt a;
    a.htype = htons(1);
    a.ptype = htons(0x0800);
    a.hlen = 6;
    a.plen = 4;
    a.op = htons(op);
    memcpy(a.sha, netcfg.mac, 6);
    a.spa = netcfg.ip;
    memcpy(a.tha, tha, 6);
    a.tpa = tpa;
    eth_send(dst, 0x0806, &a, sizeof(a));
}

static ArpEntry* arp_lookup(ip4 ip) {
    for (auto& e : arp_cache) if (e.valid && e.ip == ip) return &e;
    return nullptr;
}

static void arp_learn(ip4 ip, const u8 mac[6]) {
    ArpEntry* e = arp_lookup(ip);
    if (!e) {
        e = &arp_cache[0];
        for (auto& c : arp_cache) { if (!c.valid) { e = &c; break; } if (c.t < e->t) e = &c; }
    }
    e->ip = ip;
    memcpy(e->mac, mac, 6);
    e->t = ticks();
    e->valid = true;
    // flush anything that was waiting for this address
    for (auto& p : pending)
        if (p.used && p.hop == ip) { eth_send(mac, 0x0800, p.data, p.len); p.used = false; }
}

static void arp_receive(const u8* d, u32 len) {
    if (len < sizeof(ArpPkt)) return;
    const ArpPkt* a = (const ArpPkt*)d;
    if (ntohs(a->ptype) != 0x0800) return;
    if (a->spa) arp_learn(a->spa, a->sha);
    if (ntohs(a->op) == 1 && netcfg.ip && a->tpa == netcfg.ip) arp_send(2, a->sha, a->spa, a->sha);
}

static void arp_tick() {
    u64 t = ticks();
    for (auto& p : pending) {
        if (!p.used) continue;
        if (t - p.t0 > 3000) { p.used = false; continue; }
        if (t - p.last_req > 500) {
            static const u8 zero[6] = {0};
            arp_send(1, zero, p.hop, BCAST);
            p.last_req = t;
        }
    }
}

// ================================================================ IPv4
static u16 ip_id = 1;

bool ip_send(ip4 dst, u8 proto, const void* payload, u32 len) {
    if (!netcfg.up || len > 1480) return false;
    u8 pkt[1520];
    IpHdr* h = (IpHdr*)pkt;
    h->vihl = 0x45;
    h->tos = 0;
    h->len = htons((u16)(20 + len));
    h->id = htons(ip_id++);
    h->frag = htons(0x4000);              // don't fragment
    h->ttl = 64;
    h->proto = proto;
    h->csum = 0;
    h->src = netcfg.ip;
    h->dst = dst;
    h->csum = ip_checksum(h, 20);
    memcpy(pkt + 20, payload, len);
    u32 total = 20 + len;
    if (dst == 0xFFFFFFFF) return eth_send(BCAST, 0x0800, pkt, total);
    ip4 hop = ((dst & netcfg.mask) == (netcfg.ip & netcfg.mask)) ? dst : netcfg.gateway;
    ArpEntry* e = arp_lookup(hop);
    if (e) return eth_send(e->mac, 0x0800, pkt, total);
    // queue until ARP answers
    for (auto& p : pending)
        if (!p.used) {
            p.used = true;
            p.hop = hop;
            memcpy(p.data, pkt, total);
            p.len = total;
            p.t0 = ticks();
            p.last_req = 0;
            arp_tick();
            return true;
        }
    return false;
}

// ================================================================ ICMP
struct PingWait { u16 id, seq; u64 t0; PingReply cb; void* ctx; bool used; };
static PingWait pings[16];

bool icmp_ping(ip4 dst, u16 id, u16 seq, PingReply cb, void* ctx) {
    u8 pkt[64];
    pkt[0] = 8; pkt[1] = 0; pkt[2] = pkt[3] = 0;
    pkt[4] = id >> 8; pkt[5] = id & 0xFF;
    pkt[6] = seq >> 8; pkt[7] = seq & 0xFF;
    for (int i = 8; i < 64; i++) pkt[i] = (u8)("silicon says hi! "[i % 17]);
    u16 c = ip_checksum(pkt, 64);
    memcpy(pkt + 2, &c, 2);
    for (auto& p : pings)
        if (!p.used) { p = PingWait{id, seq, ticks(), cb, ctx, true}; break; }
    return ip_send(dst, 1, pkt, 64);
}

static void icmp_receive(const IpHdr* ih, const u8* d, u32 len) {
    if (len < 8) return;
    if (d[0] == 8) {                      // echo request: answer it
        u8 rep[1480];
        u32 n = min<u32>(len, sizeof(rep));
        memcpy(rep, d, n);
        rep[0] = 0;
        rep[2] = rep[3] = 0;
        u16 c = ip_checksum(rep, n);
        memcpy(rep + 2, &c, 2);
        ip_send(ih->src, 1, rep, n);
    } else if (d[0] == 0) {               // echo reply
        u16 id = (d[4] << 8) | d[5], seq = (d[6] << 8) | d[7];
        for (auto& p : pings)
            if (p.used && p.id == id && p.seq == seq) {
                p.used = false;
                if (p.cb) p.cb(p.ctx, seq, (u32)(ticks() - p.t0), ih->src, ih->ttl);
            }
    }
}

// ================================================================ UDP
struct UdpBind { u16 port; UdpHandler h; };
static UdpBind binds[16];

bool udp_bind(u16 port, UdpHandler h) {
    for (auto& b : binds) if (!b.port) { b.port = port; b.h = h; return true; }
    return false;
}

void udp_unbind(u16 port) { for (auto& b : binds) if (b.port == port) b.port = 0; }

bool udp_send(ip4 dst, u16 sport, u16 dport, const void* data, u32 len) {
    u8 pkt[1480];
    if (len + 8 > sizeof(pkt)) return false;
    pkt[0] = sport >> 8; pkt[1] = sport & 0xFF;
    pkt[2] = dport >> 8; pkt[3] = dport & 0xFF;
    pkt[4] = (len + 8) >> 8; pkt[5] = (len + 8) & 0xFF;
    pkt[6] = pkt[7] = 0;                  // checksum optional over IPv4
    memcpy(pkt + 8, data, len);
    return ip_send(dst, 17, pkt, len + 8);
}

static void udp_receive(const IpHdr* ih, const u8* d, u32 len) {
    if (len < 8) return;
    u16 sport = (d[0] << 8) | d[1], dport = (d[2] << 8) | d[3];
    u16 ulen = (d[4] << 8) | d[5];
    if (ulen < 8 || ulen > len) return;
    for (auto& b : binds)
        if (b.port == dport && b.h) { b.h(ih->src, sport, d + 8, ulen - 8); return; }
}

// ================================================================ frames in
void net_receive_frame(const u8* f, u32 len) {
    if (len < sizeof(EthHdr)) return;
    const EthHdr* e = (const EthHdr*)f;
    u16 type = ntohs(e->type);
    const u8* d = f + sizeof(EthHdr);
    len -= sizeof(EthHdr);
    if (type == 0x0806) { arp_receive(d, len); return; }
    if (type != 0x0800 || len < 20) return;
    const IpHdr* ih = (const IpHdr*)d;
    if ((ih->vihl >> 4) != 4) return;
    u32 ihl = (ih->vihl & 0x0F) * 4;
    u32 tot = ntohs(ih->len);
    if (tot > len || tot < ihl) return;
    if (ntohs(ih->frag) & 0x3FFF) return;          // no fragment reassembly
    bool for_us = ih->dst == netcfg.ip || ih->dst == 0xFFFFFFFF || !netcfg.ip;
    if (!for_us) return;
    // learn the sender's MAC for free when it's on our subnet
    if (netcfg.ip && (ih->src & netcfg.mask) == (netcfg.ip & netcfg.mask)) arp_learn(ih->src, e->src);
    const u8* payload = d + ihl;
    u32 plen = tot - ihl;
    switch (ih->proto) {
    case 1: icmp_receive(ih, payload, plen); break;
    case 17: udp_receive(ih, payload, plen); break;
    case 6: tcp_receive(ih->src, ih->dst, payload, plen); break;
    }
}

// ================================================================ DHCP
static u32 dhcp_xid = 0;
static int dhcp_state = 0;                // 0 idle, 1 discovering, 2 requesting, 3 bound
static u64 dhcp_t = 0;
static int dhcp_tries = 0;
static ip4 dhcp_offer = 0, dhcp_server = 0;

static void dhcp_send(int type) {
    u8 p[300];
    memset(p, 0, sizeof(p));
    p[0] = 1; p[1] = 1; p[2] = 6;
    memcpy(p + 4, &dhcp_xid, 4);
    p[10] = 0x80;                          // please broadcast the reply
    memcpy(p + 28, netcfg.mac, 6);
    p[236] = 99; p[237] = 130; p[238] = 83; p[239] = 99;
    int o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = (u8)type;
    if (type == 3) {
        p[o++] = 50; p[o++] = 4; memcpy(p + o, &dhcp_offer, 4); o += 4;
        p[o++] = 54; p[o++] = 4; memcpy(p + o, &dhcp_server, 4); o += 4;
    }
    p[o++] = 12; p[o++] = (u8)strlen(settings.hostname); memcpy(p + o, settings.hostname, strlen(settings.hostname)); o += strlen(settings.hostname);
    p[o++] = 55; p[o++] = 3; p[o++] = 1; p[o++] = 3; p[o++] = 6;   // ask for mask, router, dns
    p[o++] = 255;
    ip4 saved = netcfg.ip;
    netcfg.ip = 0;
    udp_send(0xFFFFFFFF, 68, 67, p, (u32)max(o, 300));
    netcfg.ip = saved;
}

static void dhcp_receive(ip4, u16, const u8* d, u32 len) {
    if (len < 240 || d[0] != 2 || memcmp(d + 4, &dhcp_xid, 4)) return;
    if (d[236] != 99 || d[237] != 130 || d[238] != 83 || d[239] != 99) return;
    ip4 yi;
    memcpy(&yi, d + 16, 4);
    int type = 0;
    ip4 mask = make_ip(255, 255, 255, 0), router = 0, dns = 0, server = 0;
    for (u32 o = 240; o < len;) {
        u8 opt = d[o++];
        if (opt == 0) continue;
        if (opt == 255 || o >= len) break;
        u8 l = d[o++];
        if (o + l > len) break;
        if (opt == 53) type = d[o];
        else if (opt == 1 && l >= 4) memcpy(&mask, d + o, 4);
        else if (opt == 3 && l >= 4) memcpy(&router, d + o, 4);
        else if (opt == 6 && l >= 4) memcpy(&dns, d + o, 4);
        else if (opt == 54 && l >= 4) memcpy(&server, d + o, 4);
        o += l;
    }
    if (type == 2 && dhcp_state == 1) {
        dhcp_offer = yi;
        dhcp_server = server;
        dhcp_state = 2;
        dhcp_tries = 0;
        dhcp_t = ticks();
        dhcp_send(3);
    } else if (type == 5 && dhcp_state == 2) {
        netcfg.ip = yi;
        netcfg.mask = mask;
        netcfg.gateway = router ? router : server;
        netcfg.dns = dns ? dns : netcfg.gateway;
        netcfg.configured = true;
        netcfg.dhcp = true;
        dhcp_state = 3;
        char a[16], g[16], n[16];
        ip_to_str(yi, a); ip_to_str(netcfg.gateway, g); ip_to_str(netcfg.dns, n);
        klog("dhcp: %s gw %s dns %s\n", a, g, n);
    }
}

void net_dhcp_start() {
    if (!netcfg.up) return;
    dhcp_xid = rand() ^ (u32)ticks();
    dhcp_state = 1;
    dhcp_tries = 0;
    dhcp_t = ticks();
    dhcp_send(1);
}

static void dhcp_tick() {
    if (dhcp_state != 1 && dhcp_state != 2) return;
    if (ticks() - dhcp_t < 1500) return;
    dhcp_t = ticks();
    if (++dhcp_tries >= 4) {
        // no DHCP server: fall back to QEMU user-mode networking defaults
        netcfg.ip = make_ip(10, 0, 2, 15);
        netcfg.mask = make_ip(255, 255, 255, 0);
        netcfg.gateway = make_ip(10, 0, 2, 2);
        netcfg.dns = make_ip(10, 0, 2, 3);
        netcfg.configured = true;
        netcfg.dhcp = false;
        dhcp_state = 3;
        klog("dhcp: no answer, using 10.0.2.15\n");
        return;
    }
    dhcp_send(dhcp_state == 1 ? 1 : 3);
}

// ================================================================ init / poll
bool crypto_selftest();

void net_init() {
    memset(&netcfg, 0, sizeof(netcfg));
    crypto_selftest();
    pci_scan();
    if (!settings.net_enabled) { klog("net: disabled in setup\n"); return; }
    if (!rtl8139_init(netcfg.mac)) { klog("net: no supported network card\n"); return; }
    netcfg.up = true;
    udp_bind(68, dhcp_receive);
    net_dhcp_start();
}

void net_poll() {
    if (!netcfg.up) return;
    rtl8139_poll();
    arp_tick();
    dhcp_tick();
    dns_tick();
    tcp_tick();
}

bool tcp_busy();
bool net_busy() { return netcfg.up && (!netcfg.configured || tcp_busy()); }

bool net_link_up() { return netcfg.up && netcfg.configured; }

void net_stats(u64* txp, u64* rxp, u64* txb, u64* rxb) { rtl8139_stats(txp, rxp, txb, rxb); }
