// silicon - VPN: a WireGuard client. Noise_IKpsk2 handshake (X25519, ChaCha20-Poly1305, BLAKE2s),
// transport packets carried in UDP, and a virtual interface: packets for the tunnel's allowed
// addresses are encrypted and sent to the peer, and packets that come back are handed to the
// normal IP stack as if they had arrived on a network card.
#include "net.h"
#include "crypto.h"
#include "../fs/fs.h"
#include "../drv/drivers.h"

// ================================================================ BLAKE2s
static const u32 B2_IV[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
static const u8 B2_SIGMA[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}};

static inline u32 ror32(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
static inline u32 ld32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static inline void st32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

struct B2s {
    u32 h[8];
    u8 buf[64];
    u32 n;
    u64 t;
    void compress(const u8* blk, bool last) {
        u32 m[16], v[16];
        for (int i = 0; i < 16; i++) m[i] = ld32(blk + i * 4);
        for (int i = 0; i < 8; i++) { v[i] = h[i]; v[i + 8] = B2_IV[i]; }
        v[12] ^= (u32)t; v[13] ^= (u32)(t >> 32);
        if (last) v[14] = ~v[14];
        for (int r = 0; r < 10; r++) {
            const u8* s = B2_SIGMA[r];
            auto G = [&](int a, int b, int c, int d, u32 x, u32 y) {
                v[a] = v[a] + v[b] + x; v[d] = ror32(v[d] ^ v[a], 16);
                v[c] = v[c] + v[d];     v[b] = ror32(v[b] ^ v[c], 12);
                v[a] = v[a] + v[b] + y; v[d] = ror32(v[d] ^ v[a], 8);
                v[c] = v[c] + v[d];     v[b] = ror32(v[b] ^ v[c], 7);
            };
            G(0, 4, 8, 12, m[s[0]], m[s[1]]); G(1, 5, 9, 13, m[s[2]], m[s[3]]);
            G(2, 6, 10, 14, m[s[4]], m[s[5]]); G(3, 7, 11, 15, m[s[6]], m[s[7]]);
            G(0, 5, 10, 15, m[s[8]], m[s[9]]); G(1, 6, 11, 12, m[s[10]], m[s[11]]);
            G(2, 7, 8, 13, m[s[12]], m[s[13]]); G(3, 4, 9, 14, m[s[14]], m[s[15]]);
        }
        for (int i = 0; i < 8; i++) h[i] ^= v[i] ^ v[i + 8];
    }
    void init(int outlen, const u8* key, int keylen) {
        for (int i = 0; i < 8; i++) h[i] = B2_IV[i];
        h[0] ^= 0x01010000u ^ ((u32)keylen << 8) ^ (u32)outlen;
        n = 0; t = 0;
        if (keylen) { memset(buf, 0, 64); memcpy(buf, key, keylen); n = 64; }
    }
    void update(const void* data, u32 len) {
        const u8* p = (const u8*)data;
        while (len) {
            if (n == 64) { t += 64; compress(buf, false); n = 0; }
            u32 k = min(64 - n, len);
            memcpy(buf + n, p, k);
            n += k; p += k; len -= k;
        }
    }
    void final(u8* out, int outlen) {
        t += n;
        memset(buf + n, 0, 64 - n);
        compress(buf, true);
        u8 full[32];
        for (int i = 0; i < 8; i++) st32(full + i * 4, h[i]);
        memcpy(out, full, outlen);
    }
};

static void blake2s(u8* out, int outlen, const u8* key, int keylen, const u8* in, u32 inlen) {
    B2s s;
    s.init(outlen, key, keylen);
    s.update(in, inlen);
    s.final(out, outlen);
}

static void hash2(u8 out[32], const u8* a, u32 al, const u8* b, u32 bl) {
    B2s s;
    s.init(32, nullptr, 0);
    s.update(a, al);
    if (bl) s.update(b, bl);
    s.final(out, 32);
}

static void hmac_b2(u8 out[32], const u8* key, u32 klen, const u8* in1, u32 l1, const u8* in2 = nullptr, u32 l2 = 0) {
    u8 k[64], ipad[64], opad[64], inner[32];
    memset(k, 0, 64);
    if (klen > 64) blake2s(k, 32, nullptr, 0, key, klen); else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    B2s s;
    s.init(32, nullptr, 0);
    s.update(ipad, 64); s.update(in1, l1); if (l2) s.update(in2, l2);
    s.final(inner, 32);
    s.init(32, nullptr, 0);
    s.update(opad, 64); s.update(inner, 32);
    s.final(out, 32);
}

// HKDF with HMAC-BLAKE2s: n outputs of 32 bytes
static void kdf(const u8 key[32], const u8* in, u32 inlen, u8* o1, u8* o2, u8* o3) {
    u8 prk[32], t[33];
    hmac_b2(prk, key, 32, in, inlen);
    u8 one = 1;
    hmac_b2(o1, prk, 32, &one, 1);
    if (!o2) return;
    memcpy(t, o1, 32); t[32] = 2;
    hmac_b2(o2, prk, 32, t, 33);
    if (!o3) return;
    memcpy(t, o2, 32); t[32] = 3;
    hmac_b2(o3, prk, 32, t, 33);
}

// ================================================================ ChaCha20-Poly1305 (RFC 8439)
static void chacha_block(const u32 st[16], u8 out[64]) {
    u32 x[16];
    memcpy(x, st, 64);
    auto QR = [&](int a, int b, int c, int d) {            // chacha rotates left
        x[a] += x[b]; x[d] = ror32(x[d] ^ x[a], 16);
        x[c] += x[d]; x[b] = ror32(x[b] ^ x[c], 20);
        x[a] += x[b]; x[d] = ror32(x[d] ^ x[a], 24);
        x[c] += x[d]; x[b] = ror32(x[b] ^ x[c], 25);
    };
    for (int i = 0; i < 10; i++) {
        QR(0, 4, 8, 12); QR(1, 5, 9, 13); QR(2, 6, 10, 14); QR(3, 7, 11, 15);
        QR(0, 5, 10, 15); QR(1, 6, 11, 12); QR(2, 7, 8, 13); QR(3, 4, 9, 14);
    }
    for (int i = 0; i < 16; i++) st32(out + i * 4, x[i] + st[i]);
}

static void chacha_init(u32 st[16], const u8 key[32], u32 ctr, const u8 nonce[12]) {
    st[0] = 0x61707865; st[1] = 0x3320646e; st[2] = 0x79622d32; st[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) st[4 + i] = ld32(key + i * 4);
    st[12] = ctr;
    for (int i = 0; i < 3; i++) st[13 + i] = ld32(nonce + i * 4);
}

static void chacha_xor(const u8 key[32], const u8 nonce[12], u32 ctr, const u8* in, u8* out, u32 len) {
    u32 st[16];
    chacha_init(st, key, ctr, nonce);
    u8 ks[64];
    for (u32 off = 0; off < len; off += 64) {
        chacha_block(st, ks);
        st[12]++;
        u32 n = min<u32>(64, len - off);
        for (u32 i = 0; i < n; i++) out[off + i] = in[off + i] ^ ks[i];
    }
}

struct Poly1305 {
    u32 r[5], h[5], pad[4];
    u8 buf[16];
    u32 n;
    void init(const u8 key[32]) {
        r[0] = (ld32(key + 0)) & 0x3ffffff;
        r[1] = (ld32(key + 3) >> 2) & 0x3ffff03;
        r[2] = (ld32(key + 6) >> 4) & 0x3ffc0ff;
        r[3] = (ld32(key + 9) >> 6) & 0x3f03fff;
        r[4] = (ld32(key + 12) >> 8) & 0x00fffff;
        for (int i = 0; i < 5; i++) h[i] = 0;
        for (int i = 0; i < 4; i++) pad[i] = ld32(key + 16 + i * 4);
        n = 0;
    }
    void block(const u8* m, u32 hibit) {
        u32 r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4];
        u32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
        u32 h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4];
        h0 += (ld32(m + 0)) & 0x3ffffff;
        h1 += (ld32(m + 3) >> 2) & 0x3ffffff;
        h2 += (ld32(m + 6) >> 4) & 0x3ffffff;
        h3 += (ld32(m + 9) >> 6) & 0x3ffffff;
        h4 += (ld32(m + 12) >> 8) | hibit;
        u64 d0 = (u64)h0 * r0 + (u64)h1 * s4 + (u64)h2 * s3 + (u64)h3 * s2 + (u64)h4 * s1;
        u64 d1 = (u64)h0 * r1 + (u64)h1 * r0 + (u64)h2 * s4 + (u64)h3 * s3 + (u64)h4 * s2;
        u64 d2 = (u64)h0 * r2 + (u64)h1 * r1 + (u64)h2 * r0 + (u64)h3 * s4 + (u64)h4 * s3;
        u64 d3 = (u64)h0 * r3 + (u64)h1 * r2 + (u64)h2 * r1 + (u64)h3 * r0 + (u64)h4 * s4;
        u64 d4 = (u64)h0 * r4 + (u64)h1 * r3 + (u64)h2 * r2 + (u64)h3 * r1 + (u64)h4 * r0;
        u32 c;
        c = (u32)(d0 >> 26); h0 = (u32)d0 & 0x3ffffff;
        d1 += c; c = (u32)(d1 >> 26); h1 = (u32)d1 & 0x3ffffff;
        d2 += c; c = (u32)(d2 >> 26); h2 = (u32)d2 & 0x3ffffff;
        d3 += c; c = (u32)(d3 >> 26); h3 = (u32)d3 & 0x3ffffff;
        d4 += c; c = (u32)(d4 >> 26); h4 = (u32)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        h[0] = h0; h[1] = h1; h[2] = h2; h[3] = h3; h[4] = h4;
    }
    void update(const u8* m, u32 len) {
        while (len) {
            u32 k = min(16 - n, len);
            memcpy(buf + n, m, k);
            n += k; m += k; len -= k;
            if (n == 16) { block(buf, 1u << 24); n = 0; }
        }
    }
    void final(u8 tag[16]) {
        if (n) { buf[n] = 1; for (u32 i = n + 1; i < 16; i++) buf[i] = 0; block(buf, 0); }
        u32 h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4], c;
        c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
        c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
        c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
        c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
        c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
        u32 g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
        u32 g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
        u32 g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
        u32 g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
        u32 g4 = h4 + c - (1u << 26);
        u32 mask = (g4 >> 31) - 1;                       // all ones when h >= p
        g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
        mask = ~mask;
        h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
        u64 f;
        u32 o0 = (h0 | (h1 << 26));
        u32 o1 = ((h1 >> 6) | (h2 << 20));
        u32 o2 = ((h2 >> 12) | (h3 << 14));
        u32 o3 = ((h3 >> 18) | (h4 << 8));
        f = (u64)o0 + pad[0]; st32(tag + 0, (u32)f);
        f = (u64)o1 + pad[1] + (f >> 32); st32(tag + 4, (u32)f);
        f = (u64)o2 + pad[2] + (f >> 32); st32(tag + 8, (u32)f);
        f = (u64)o3 + pad[3] + (f >> 32); st32(tag + 12, (u32)f);
    }
};

static void aead_tag(const u8 key[32], const u8 nonce[12], const u8* aad, u32 alen, const u8* ct, u32 clen, u8 tag[16]) {
    u8 otk[64];
    u32 st[16];
    chacha_init(st, key, 0, nonce);
    chacha_block(st, otk);
    Poly1305 p;
    p.init(otk);
    static const u8 zeros[16] = {0};
    p.update(aad, alen);
    if (alen % 16) p.update(zeros, 16 - alen % 16);
    p.update(ct, clen);
    if (clen % 16) p.update(zeros, 16 - clen % 16);
    u8 lens[16];
    st32(lens, alen); st32(lens + 4, 0); st32(lens + 8, clen); st32(lens + 12, 0);
    p.update(lens, 16);
    p.final(tag);
}

static void aead_seal(const u8 key[32], u64 counter, const u8* aad, u32 alen, const u8* pt, u32 len, u8* out) {   // out: len + 16
    u8 nonce[12] = {0};
    for (int i = 0; i < 8; i++) nonce[4 + i] = (u8)(counter >> (8 * i));
    chacha_xor(key, nonce, 1, pt, out, len);
    aead_tag(key, nonce, aad, alen, out, len, out + len);
}

static bool aead_open(const u8 key[32], u64 counter, const u8* aad, u32 alen, const u8* in, u32 len, u8* out) {    // len includes the tag
    if (len < 16) return false;
    u8 nonce[12] = {0};
    for (int i = 0; i < 8; i++) nonce[4 + i] = (u8)(counter >> (8 * i));
    u8 tag[16];
    aead_tag(key, nonce, aad, alen, in, len - 16, tag);
    u8 diff = 0;
    for (int i = 0; i < 16; i++) diff |= tag[i] ^ in[len - 16 + i];
    if (diff) return false;
    chacha_xor(key, nonce, 1, in, out, len - 16);
    return true;
}

// ================================================================ configuration
static const char* CFG_PATH = "/home/.config/vpn";

static int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}
static bool b64_32(const char* s, u8 out[32]) {
    int n = 0, acc = 0, bits = 0;
    for (; *s && *s != '='; s++) {
        int v = b64v(*s);
        if (v < 0) return false;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; if (n < 32) out[n] = (u8)(acc >> bits); n++; acc &= (1 << bits) - 1; }
    }
    return n == 32;
}
static void b64_enc(const u8* d, int n, char* out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        u32 v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out[o++] = T[v >> 18]; out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '='; out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}

struct Allowed { ip4 net, mask; };

struct Wg {
    bool configured, enabled;
    u8 priv[32], pub[32], peer_pub[32], psk[32];
    ip4 addr, endpoint_ip;
    u16 endpoint_port, listen_port;
    char endpoint_host[96];
    Allowed allowed[8];
    int nallowed;
    u16 keepalive;
    // handshake
    u8 mac1_key[32];                     // Hash(label-mac1 || peer public)
    u32 my_index;
    u8 hs_c[32], hs_h[32], hs_eph_priv[32];
    u64 hs_sent;                         // when the initiation went out (0 = none outstanding)
    int hs_tries;
    // session
    bool have_session;
    u32 peer_index;
    u8 send_key[32], recv_key[32];
    u64 send_ctr;
    u64 recv_max; u64 recv_window;       // replay window
    u64 session_t0, last_sent, last_recv;
    bool endpoint_resolving;
    u64 tx_bytes, rx_bytes;
    u64 tx_packets, rx_packets;
    u64 last_handshake_ok;
    char status[80];
};
static Wg wg;

static void set_status(const char* s) { strlcpy(wg.status, s, sizeof(wg.status)); }

static bool parse_cidr(const char* s, Allowed* a) {
    char tmp[32];
    strlcpy(tmp, s, sizeof(tmp));
    char* sl = strchr(tmp, '/');
    int bits = 32;
    if (sl) { *sl = 0; bits = atoi(sl + 1); }
    ip4 ip;
    if (!str_to_ip(tmp, &ip) || bits < 0 || bits > 32) return false;
    a->mask = bits ? htonl(~0u << (32 - bits)) : 0;
    a->net = ip & a->mask;
    return true;
}

void vpn_load() {
    memset(&wg, 0, sizeof(wg));
    FsNode* f = fs_lookup(CFG_PATH);
    if (!f) { set_status("not set up"); return; }
    const char* t = fs_read_text(f);
    bool have_priv = false, have_peer = false;
    while (*t) {
        const char* e = strchr(t, '\n');
        int l = e ? (int)(e - t) : (int)strlen(t);
        char line[200];
        memcpy(line, t, min(l, 199));
        line[min(l, 199)] = 0;
        char* eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            const char* k = line;
            const char* v = eq + 1;
            if (!strcmp(k, "private")) have_priv = b64_32(v, wg.priv);
            else if (!strcmp(k, "peer_public")) have_peer = b64_32(v, wg.peer_pub);
            else if (!strcmp(k, "preshared")) b64_32(v, wg.psk);
            else if (!strcmp(k, "address")) str_to_ip(v, &wg.addr);
            else if (!strcmp(k, "endpoint")) {
                char h[96];
                strlcpy(h, v, sizeof(h));
                char* colon = strrchr(h, ':');
                if (colon) { *colon = 0; wg.endpoint_port = (u16)atoi(colon + 1); }
                strlcpy(wg.endpoint_host, h, sizeof(wg.endpoint_host));
            } else if (!strcmp(k, "allowed")) {
                char list[160];
                strlcpy(list, v, sizeof(list));
                for (char* p = list; p && *p;) {
                    char* c = strchr(p, ',');
                    if (c) *c = 0;
                    while (*p == ' ') p++;
                    if (wg.nallowed < 8 && parse_cidr(p, &wg.allowed[wg.nallowed])) wg.nallowed++;
                    p = c ? c + 1 : nullptr;
                }
            } else if (!strcmp(k, "keepalive")) wg.keepalive = (u16)atoi(v);
            else if (!strcmp(k, "listen")) wg.listen_port = (u16)atoi(v);
            else if (!strcmp(k, "enabled")) wg.enabled = atoi(v) != 0;
        }
        if (!e) break;
        t = e + 1;
    }
    if (!wg.listen_port) wg.listen_port = 51821;
    if (!wg.endpoint_port) wg.endpoint_port = 51820;
    if (!have_priv || !have_peer || !wg.addr || !wg.endpoint_host[0]) { set_status("incomplete settings"); return; }
    x25519_base(wg.pub, wg.priv);
    u8 buf[8 + 32];
    memcpy(buf, "mac1----", 8);
    memcpy(buf + 8, wg.peer_pub, 32);
    blake2s(wg.mac1_key, 32, nullptr, 0, buf, sizeof(buf));
    wg.configured = true;
    set_status(wg.enabled ? "starting" : "off");
}

void vpn_save_text(const char* text) {
    fs_mkdirs("/home/.config");
    fs_write_text(CFG_PATH, text);
}

// ================================================================ handshake
static void tai64n(u8 out[12]) {
    u64 secs = 4611686018427387914ull + 946684800ull + rtc_stamp();       // 2^62 + 10 + unix time
    for (int i = 0; i < 8; i++) out[i] = (u8)(secs >> (56 - 8 * i));
    u32 ns = (u32)(ticks() % 1000) * 1000000;
    for (int i = 0; i < 4; i++) out[8 + i] = (u8)(ns >> (24 - 8 * i));
}

static const char WG_CONSTRUCTION[] = "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s";
static const char WG_IDENTIFIER[] = "WireGuard v1 zx2c4 Jason@zx2c4.com";

static void clamp_key(u8 k[32]) { k[0] &= 248; k[31] &= 127; k[31] |= 64; }

static void send_udp(const u8* d, u32 len) {
    udp_send(wg.endpoint_ip, wg.listen_port, wg.endpoint_port, d, len);
    wg.last_sent = ticks();
}

static void send_initiation() {
    u8 msg[148];
    memset(msg, 0, sizeof(msg));
    msg[0] = 1;
    random_bytes((u8*)&wg.my_index, 4);
    st32(msg + 4, wg.my_index);
    // C = Hash(Construction); H = Hash(C || Identifier); H = Hash(H || Spub_r)
    u8* C = wg.hs_c;
    u8* H = wg.hs_h;
    blake2s(C, 32, nullptr, 0, (const u8*)WG_CONSTRUCTION, sizeof(WG_CONSTRUCTION) - 1);
    hash2(H, C, 32, (const u8*)WG_IDENTIFIER, sizeof(WG_IDENTIFIER) - 1);
    hash2(H, H, 32, wg.peer_pub, 32);
    // ephemeral
    random_bytes(wg.hs_eph_priv, 32);
    clamp_key(wg.hs_eph_priv);
    u8 epub[32];
    x25519_base(epub, wg.hs_eph_priv);
    kdf(C, epub, 32, C, nullptr, nullptr);
    hash2(H, H, 32, epub, 32);
    memcpy(msg + 8, epub, 32);
    // static
    u8 dh[32], k[32];
    x25519(dh, wg.hs_eph_priv, wg.peer_pub);
    kdf(C, dh, 32, C, k, nullptr);
    aead_seal(k, 0, H, 32, wg.pub, 32, msg + 40);
    hash2(H, H, 32, msg + 40, 48);
    // timestamp
    x25519(dh, wg.priv, wg.peer_pub);
    kdf(C, dh, 32, C, k, nullptr);
    u8 ts[12];
    tai64n(ts);
    aead_seal(k, 0, H, 32, ts, 12, msg + 88);
    hash2(H, H, 32, msg + 88, 28);
    // mac1 over the first 116 bytes, mac2 stays zero (no cookie)
    blake2s(msg + 116, 16, wg.mac1_key, 32, msg, 116);
    wg.hs_sent = ticks();
    wg.hs_tries++;
    send_udp(msg, sizeof(msg));
    set_status("handshake sent");
}

static void handle_response(const u8* d, u32 len) {
    if (len != 92 || !wg.hs_sent) return;
    if (ld32(d + 8) != wg.my_index) return;
    u8 mac[16];
    u8 my_mac_key[32], tmp[8 + 32];
    memcpy(tmp, "mac1----", 8);
    memcpy(tmp + 8, wg.pub, 32);
    blake2s(my_mac_key, 32, nullptr, 0, tmp, sizeof(tmp));
    blake2s(mac, 16, my_mac_key, 32, d, 60);
    if (memcmp(mac, d + 60, 16)) { klog("vpn: handshake reply with a bad mac\n"); return; }
    u8 C[32], H[32], dh[32], k[32], T[32];
    memcpy(C, wg.hs_c, 32);
    memcpy(H, wg.hs_h, 32);
    const u8* er = d + 12;
    kdf(C, er, 32, C, nullptr, nullptr);
    hash2(H, H, 32, er, 32);
    x25519(dh, wg.hs_eph_priv, er);
    kdf(C, dh, 32, C, nullptr, nullptr);
    x25519(dh, wg.priv, er);
    kdf(C, dh, 32, C, nullptr, nullptr);
    kdf(C, wg.psk, 32, C, T, k);
    hash2(H, H, 32, T, 32);
    u8 empty[1];
    if (!aead_open(k, 0, H, 32, d + 44, 16, empty)) { klog("vpn: handshake reply did not authenticate\n"); set_status("handshake failed (wrong keys?)"); return; }
    hash2(H, H, 32, d + 44, 16);
    // transport keys: the initiator sends with the first one
    kdf(C, nullptr, 0, wg.send_key, wg.recv_key, nullptr);
    wg.peer_index = ld32(d + 4);
    wg.send_ctr = 0;
    wg.recv_max = 0; wg.recv_window = 0;
    wg.have_session = true;
    wg.session_t0 = ticks();
    wg.last_recv = ticks();
    wg.last_handshake_ok = ticks();
    wg.hs_sent = 0;
    wg.hs_tries = 0;
    set_status("connected");
    klog("vpn: tunnel up (peer index %x)\n", wg.peer_index);
    char b[48], a[16];
    ip_to_str(wg.addr, a);
    snprintf(b, sizeof(b), "your tunnel address is %s", a);
    void ui_notify(const char*, const char*, const char*);
    ui_notify("VPN connected", b, "vpn");
}

// ================================================================ transport
static void send_transport(const u8* pkt, u32 len) {
    u32 padded = (len + 15) & ~15u;
    u8 msg[1500];
    if (padded + 32 > sizeof(msg)) return;
    u8 plain[1500];
    memset(plain, 0, padded);
    memcpy(plain, pkt, len);
    msg[0] = 4; msg[1] = msg[2] = msg[3] = 0;
    st32(msg + 4, wg.peer_index);
    u64 ctr = wg.send_ctr++;
    st32(msg + 8, (u32)ctr); st32(msg + 12, (u32)(ctr >> 32));
    aead_seal(wg.send_key, ctr, nullptr, 0, plain, padded, msg + 16);
    send_udp(msg, 16 + padded + 16);
    wg.tx_bytes += len;
    wg.tx_packets++;
}

void ip_deliver_inner(const u8* d, u32 len);                 // core.cpp: hand an IPv4 packet to the stack

static void handle_transport(const u8* d, u32 len) {
    if (!wg.have_session || len < 32 || ld32(d + 4) != wg.my_index) return;
    u64 ctr = ld32(d + 8) | ((u64)ld32(d + 12) << 32);
    // replay protection: a 64-packet sliding window
    if (ctr + 64 <= wg.recv_max && wg.recv_max) return;
    if (ctr <= wg.recv_max && wg.recv_max - ctr < 64 && (wg.recv_window >> (wg.recv_max - ctr)) & 1) return;
    u8 plain[1500];
    if (len - 16 > sizeof(plain) + 16) return;
    if (!aead_open(wg.recv_key, ctr, nullptr, 0, d + 16, len - 16, plain)) return;
    if (ctr > wg.recv_max) { u64 sh = ctr - wg.recv_max; wg.recv_window = sh >= 64 ? 0 : wg.recv_window << sh; wg.recv_window |= 1; wg.recv_max = ctr; }
    else wg.recv_window |= 1ull << (wg.recv_max - ctr);
    wg.last_recv = ticks();
    u32 plen = len - 32;
    if (plen == 0) return;                                    // keepalive
    // the real length comes from the IP header (the rest is padding)
    if ((plain[0] >> 4) != 4 || plen < 20) return;
    u32 tot = (plain[2] << 8) | plain[3];
    if (tot > plen || tot < 20) return;
    wg.rx_bytes += tot;
    wg.rx_packets++;
    ip_deliver_inner(plain, tot);
}

static void udp_in(ip4, u16, const u8* d, u32 len) {
    if (len < 4 || !wg.configured) return;
    u32 type = ld32(d);
    if (type == 2) handle_response(d, len);
    else if (type == 4) handle_transport(d, len);
}

// ================================================================ interface
bool vpn_active() { return wg.configured && wg.enabled && wg.have_session; }

bool vpn_routes(ip4 dst) {
    if (!wg.configured || !wg.enabled || !netcfg.configured) return false;
    if (dst == wg.endpoint_ip) return false;                  // never wrap the tunnel's own packets
    if (dst == 0xFFFFFFFF || (dst & netcfg.mask) == (netcfg.ip & netcfg.mask)) return false;   // local network stays local
    for (int i = 0; i < wg.nallowed; i++) if ((dst & wg.allowed[i].mask) == wg.allowed[i].net) return true;
    return false;
}
ip4 vpn_address() { return wg.addr; }

// send an IPv4 packet (header included) through the tunnel; false when the tunnel isn't usable
bool vpn_send_ip(const u8* pkt, u32 len) {
    if (!wg.have_session) {                                   // not connected yet: start the handshake, drop the packet
        if (wg.enabled && !wg.hs_sent) send_initiation();
        return false;
    }
    send_transport(pkt, len);
    return true;
}

void vpn_set_enabled(bool on) {
    if (!wg.configured) return;
    wg.enabled = on;
    wg.have_session = false;
    wg.hs_sent = 0;
    wg.hs_tries = 0;
    wg.endpoint_ip = 0;
    wg.endpoint_resolving = on;
    set_status(on ? "connecting" : "off");
    if (on) {
        udp_bind(wg.listen_port, udp_in);
    } else udp_unbind(wg.listen_port);
    char t[300];
    snprintf(t, sizeof(t), "enabled=%d\n", on ? 1 : 0);
    // keep the rest of the file, only flip the flag
    FsNode* f = fs_lookup(CFG_PATH);
    if (f) {
        const char* old = fs_read_text(f);
        char out[1200];
        out[0] = 0;
        for (const char* p = old; *p;) {
            const char* e = strchr(p, '\n');
            int l = e ? (int)(e - p) : (int)strlen(p);
            if (strncmp(p, "enabled=", 8) && l && strlen(out) + l + 2 < sizeof(out)) { int o = (int)strlen(out); memcpy(out + o, p, l); out[o + l] = '\n'; out[o + l + 1] = 0; }
            if (!e) break;
            p = e + 1;
        }
        strlcat(out, t, sizeof(out));
        fs_write_text(CFG_PATH, out);
    }
}

void vpn_init() {
    vpn_load();
    if (wg.configured && wg.enabled) { wg.enabled = false; vpn_set_enabled(true); }
}

void vpn_tick() {
    if (!wg.configured || !wg.enabled || !netcfg.configured) return;
    u64 t = ticks();
    if (!wg.endpoint_ip) {
        ip4 ip;
        if (str_to_ip(wg.endpoint_host, &ip)) wg.endpoint_ip = ip;
        else {
            int st = dns_resolve(wg.endpoint_host, &ip);
            if (st > 0) wg.endpoint_ip = ip;
            else if (st < 0) { set_status("can't find the server"); return; }
        }
        if (!wg.endpoint_ip) return;
    }
    if (!wg.have_session) {
        if (!wg.hs_sent || t - wg.hs_sent > 5000) {
            if (wg.hs_tries >= 6 && t - wg.hs_sent < 20000) { set_status("no answer from the server"); return; }
            send_initiation();
        }
        return;
    }
    // keepalive, and a fresh handshake every two minutes
    if (wg.keepalive && t - wg.last_sent > (u64)wg.keepalive * 1000) send_transport(nullptr, 0);
    if (t - wg.session_t0 > 120000 && !wg.hs_sent) send_initiation();
    if (wg.hs_sent && t - wg.hs_sent > 5000) { wg.hs_sent = 0; wg.hs_tries = 0; }
    if (t - wg.last_recv > 180000) { wg.have_session = false; set_status("connection lost"); }
}

// ================================================================ status for the UI and shell
void vpn_info(VpnInfo* o) {
    memset(o, 0, sizeof(*o));
    o->configured = wg.configured; o->enabled = wg.enabled; o->up = wg.have_session;
    strlcpy(o->status, wg.status, sizeof(o->status));
    if (wg.configured) {
        b64_enc(wg.pub, 32, o->public_key);
        snprintf(o->endpoint, sizeof(o->endpoint), "%s:%u", wg.endpoint_host, wg.endpoint_port);
        ip_to_str(wg.addr, o->address);
    }
    o->tx = wg.tx_bytes; o->rx = wg.rx_bytes;
    o->rx_age_s = wg.have_session ? (ticks() - wg.last_recv) / 1000 : 0;
}

void vpn_make_keypair(char priv_b64[48], char pub_b64[48]) {
    u8 k[32], p[32];
    random_bytes(k, 32);
    clamp_key(k);
    x25519_base(p, k);
    b64_enc(k, 32, priv_b64);
    b64_enc(p, 32, pub_b64);
}

// printed by `vpn selftest`: known inputs, compared against reference implementations by tools/wgpeer.py --selftest
void vpn_selftest(char* out, int n) {
    u8 h[32], m[16], key[32], ct[40], k2[32], k3[32];
    blake2s(h, 32, nullptr, 0, (const u8*)"abc", 3);
    int o = snprintf(out, n, "b2s ");
    for (int i = 0; i < 8; i++) o += snprintf(out + o, n - o, "%02x", h[i]);
    u8 mk[8]; memcpy(mk, "somekey!", 8);
    blake2s(m, 16, mk, 8, (const u8*)"abc", 3);
    o += snprintf(out + o, n - o, "\nb2s-keyed ");
    for (int i = 0; i < 8; i++) o += snprintf(out + o, n - o, "%02x", m[i]);
    for (int i = 0; i < 32; i++) key[i] = (u8)i;
    hmac_b2(h, key, 32, (const u8*)"hello", 5);
    o += snprintf(out + o, n - o, "\nhmac ");
    for (int i = 0; i < 8; i++) o += snprintf(out + o, n - o, "%02x", h[i]);
    kdf(key, (const u8*)"input", 5, h, k2, k3);
    o += snprintf(out + o, n - o, "\nkdf ");
    for (int i = 0; i < 4; i++) o += snprintf(out + o, n - o, "%02x", h[i]);
    for (int i = 0; i < 4; i++) o += snprintf(out + o, n - o, "%02x", k2[i]);
    for (int i = 0; i < 4; i++) o += snprintf(out + o, n - o, "%02x", k3[i]);
    {
        u8 nn[12] = {0, 0, 0, 9, 0, 0, 0, 0x4a, 0, 0, 0, 0}, blk[64], z[64] = {0};
        chacha_xor(key, nn, 1, z, blk, 64);
        o += snprintf(out + o, n - o, "\nchacha ");
        for (int i = 0; i < 8; i++) o += snprintf(out + o, n - o, "%02x", blk[i]);
    }
    aead_seal(key, 5, (const u8*)"aad", 3, (const u8*)"plaintext-123456", 16, ct);
    o += snprintf(out + o, n - o, "\naead ");
    for (int i = 0; i < 32; i++) o += snprintf(out + o, n - o, "%02x", ct[i]);
}

// write the settings (keeping this machine's private key when it has one) and reload them
bool vpn_configure(const char* endpoint, const char* server_key, const char* address, const char* allowed, char* err, int errlen) {
    u8 tmp[32];
    ip4 ip;
    if (!endpoint[0] || !strchr(endpoint, ':')) { snprintf(err, errlen, "the server needs a host and a port (host:port)"); return false; }
    if (!b64_32(server_key, tmp)) { snprintf(err, errlen, "the server key must be 44 characters of base64"); return false; }
    if (!str_to_ip(address, &ip)) { snprintf(err, errlen, "the tunnel address must look like 10.7.0.2"); return false; }
    char priv[48], pub[48];
    if (wg.configured) b64_enc(wg.priv, 32, priv); else vpn_make_keypair(priv, pub);
    char cfg[700];
    snprintf(cfg, sizeof(cfg), "private=%s\npeer_public=%s\naddress=%s\nendpoint=%s\nallowed=%s\nkeepalive=15\nenabled=0\n", priv, server_key, address, endpoint,
             allowed[0] ? allowed : "0.0.0.0/0");
    vpn_save_text(cfg);
    bool was = wg.enabled;
    vpn_load();
    if (was) vpn_set_enabled(false);
    return wg.configured;
}

void vpn_current(char* host, char* key, char* addr, char* allowed) {
    host[0] = key[0] = addr[0] = allowed[0] = 0;
    if (!wg.configured) return;
    snprintf(host, 100, "%s:%u", wg.endpoint_host, wg.endpoint_port);
    b64_enc(wg.peer_pub, 32, key);
    ip_to_str(wg.addr, addr);
    for (int i = 0; i < wg.nallowed; i++) {
        char a[20];
        ip_to_str(wg.allowed[i].net, a);
        int bits = 0;
        for (u32 m = ntohl(wg.allowed[i].mask); m & 0x80000000u; m <<= 1) bits++;
        char one[28];
        snprintf(one, sizeof(one), "%s%s/%d", i ? "," : "", a, bits);
        strlcat(allowed, one, 100);
    }
}
