// silicon - DNS stub resolver (A records) with a small cache
#include "net.h"

struct DnsCache { char name[128]; ip4 ip; u64 expires; bool used; };
struct DnsQuery { char name[128]; u16 id; u64 sent; int tries; int state; u64 done_at; ip4 ip; bool used; };   // state 0 wait, 1 ok, -1 fail

static DnsCache cache[48];
static DnsQuery queries[8];
static const u16 DNS_PORT = 53053;
static bool bound = false;

static void send_query(DnsQuery& q) {
    u8 p[300];
    int o = 0;
    p[o++] = q.id >> 8; p[o++] = q.id & 0xFF;
    p[o++] = 0x01; p[o++] = 0x00;                 // recursion desired
    p[o++] = 0; p[o++] = 1;                       // one question
    p[o++] = 0; p[o++] = 0; p[o++] = 0; p[o++] = 0; p[o++] = 0; p[o++] = 0;
    const char* s = q.name;
    while (*s) {
        const char* dot = strchr(s, '.');
        int l = dot ? (int)(dot - s) : (int)strlen(s);
        if (l > 63 || o + l + 2 > 280) break;
        p[o++] = (u8)l;
        memcpy(p + o, s, l);
        o += l;
        s += l;
        if (*s == '.') s++;
    }
    p[o++] = 0;
    p[o++] = 0; p[o++] = 1;                       // type A
    p[o++] = 0; p[o++] = 1;                       // class IN
    q.sent = ticks();
    q.tries++;
    udp_send(netcfg.dns, DNS_PORT, 53, p, o);
}

static u32 skip_name(const u8* d, u32 len, u32 o) {
    while (o < len) {
        u8 l = d[o];
        if (l == 0) return o + 1;
        if ((l & 0xC0) == 0xC0) return o + 2;
        o += l + 1;
    }
    return len;
}

static void dns_receive(ip4, u16, const u8* d, u32 len) {
    if (len < 12) return;
    u16 id = (d[0] << 8) | d[1];
    DnsQuery* q = nullptr;
    for (auto& x : queries) if (x.used && x.state == 0 && x.id == id) q = &x;
    if (!q) return;
    int rcode = d[3] & 0x0F;
    u16 qd = (d[4] << 8) | d[5], an = (d[6] << 8) | d[7];
    u32 o = 12;
    for (int i = 0; i < qd; i++) o = skip_name(d, len, o) + 4;
    for (int i = 0; i < an && o + 10 <= len; i++) {
        o = skip_name(d, len, o);
        if (o + 10 > len) break;
        u16 type = (d[o] << 8) | d[o + 1];
        u32 ttl = ((u32)d[o + 4] << 24) | ((u32)d[o + 5] << 16) | (d[o + 6] << 8) | d[o + 7];
        u16 rdlen = (d[o + 8] << 8) | d[o + 9];
        o += 10;
        if (type == 1 && rdlen == 4 && o + 4 <= len) {
            memcpy(&q->ip, d + o, 4);
            q->state = 1;
            q->done_at = ticks();
            // cache it
            DnsCache* c = &cache[0];
            for (auto& e : cache) if (!e.used) { c = &e; break; } else if (e.expires < c->expires) c = &e;
            strlcpy(c->name, q->name, sizeof(c->name));
            c->ip = q->ip;
            c->expires = ticks() + (u64)clamp<u32>(ttl, 30, 3600) * 1000;
            c->used = true;
            return;
        }
        o += rdlen;
    }
    q->state = -1;
    q->done_at = ticks();
    (void)rcode;
}

int dns_resolve(const char* name, ip4* ip) {
    if (str_to_ip(name, ip)) return 1;
    if (!strcmp(name, "localhost")) { *ip = netcfg.ip; return 1; }
    if (!net_link_up()) return 0;
    if (!bound) { udp_bind(DNS_PORT, dns_receive); bound = true; }
    for (auto& c : cache)
        if (c.used && !strcmp(c.name, name)) {
            if (c.expires > ticks()) { *ip = c.ip; return 1; }
            c.used = false;
        }
    for (auto& q : queries)
        if (q.used && !strcmp(q.name, name)) {
            if (q.state == 1) { *ip = q.ip; q.used = false; return 1; }
            if (q.state == -1) { q.used = false; return -1; }
            return 0;
        }
    DnsQuery* q = nullptr;
    for (auto& x : queries) if (!x.used) { q = &x; break; }
    if (!q) return 0;
    memset(q, 0, sizeof(*q));
    q->used = true;
    strlcpy(q->name, name, sizeof(q->name));
    q->id = (u16)rand();
    send_query(*q);
    return 0;
}

void dns_tick() {
    u64 t = ticks();
    for (auto& q : queries) {
        if (!q.used) continue;
        if (q.state == 0 && t - q.sent > 1200) {
            if (q.tries >= 4) { q.state = -1; q.done_at = t; }
            else send_query(q);
        }
        // nobody collected the answer: forget it
        if (q.state != 0 && t - q.done_at > 20000) q.used = false;
    }
}

void dns_flush() { for (auto& c : cache) c.used = false; }
