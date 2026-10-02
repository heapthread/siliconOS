// silicon - crypto primitives for TLS 1.3. Written from the specs (FIPS 180-4, RFC 2104,
// RFC 5869, RFC 8446 7.1, RFC 7748, FIPS 197, NIST SP 800-38D); self-tested at boot.
#include "crypto.h"
#include "../drv/drivers.h"

// ================================================================ SHA-256
static const u32 K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline u32 ror(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(u32 h[8], const u8* p) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = (p[4 * i] << 24) | (p[4 * i + 1] << 16) | (p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        u32 S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = hh + S1 + ch + K256[i] + w[i];
        u32 S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void Sha256::init() {
    static const u32 iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h, iv, sizeof(h));
    total = 0;
    n = 0;
}

void Sha256::update(const void* data, u32 len) {
    const u8* p = (const u8*)data;
    total += len;
    while (len) {
        u32 take = min(64 - n, len);
        memcpy(buf + n, p, take);
        n += take; p += take; len -= take;
        if (n == 64) { sha_block(h, buf); n = 0; }
    }
}

void Sha256::final(u8 out[32]) {
    Sha256 c = *this;
    u64 bits = c.total * 8;
    u8 pad = 0x80;
    c.update(&pad, 1);
    u8 zero = 0;
    while (c.n != 56) c.update(&zero, 1);
    u8 lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (u8)(bits >> (56 - 8 * i));
    c.update(lb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = c.h[i] >> 24; out[4 * i + 1] = c.h[i] >> 16; out[4 * i + 2] = c.h[i] >> 8; out[4 * i + 3] = c.h[i];
    }
}

void sha256(const void* data, u32 len, u8 out[32]) {
    Sha256 s;
    s.init();
    s.update(data, len);
    s.final(out);
}

// ================================================================ HMAC / HKDF
void hmac_sha256(const u8* key, u32 klen, const u8* msg, u32 mlen, u8 out[32]) {
    u8 k[64];
    memset(k, 0, 64);
    if (klen > 64) sha256(key, klen, k); else memcpy(k, key, klen);
    u8 ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    Sha256 s;
    u8 inner[32];
    s.init(); s.update(ipad, 64); s.update(msg, mlen); s.final(inner);
    s.init(); s.update(opad, 64); s.update(inner, 32); s.final(out);
}

void hkdf_extract(const u8* salt, u32 slen, const u8* ikm, u32 ilen, u8 out[32]) {
    u8 zero[32] = {0};
    if (!salt) { salt = zero; slen = 32; }
    hmac_sha256(salt, slen, ikm, ilen, out);
}

void hkdf_expand_label(const u8 secret[32], const char* label, const u8* ctx, u32 clen, u8* out, u32 len) {
    u8 info[300];
    u32 ll = (u32)strlen(label) + 6;
    u32 o = 0;
    info[o++] = (u8)(len >> 8);
    info[o++] = (u8)len;
    info[o++] = (u8)ll;
    memcpy(info + o, "tls13 ", 6); o += 6;
    memcpy(info + o, label, ll - 6); o += ll - 6;
    info[o++] = (u8)clen;
    memcpy(info + o, ctx, clen); o += clen;
    u8 t[32];
    u32 done = 0, tlen = 0;
    for (u8 ctr = 1; done < len; ctr++) {
        u8 msg[340];
        memcpy(msg, t, tlen);
        memcpy(msg + tlen, info, o);
        msg[tlen + o] = ctr;
        hmac_sha256(secret, 32, msg, tlen + o + 1, t);
        tlen = 32;
        u32 take = min<u32>(32, len - done);
        memcpy(out + done, t, take);
        done += take;
    }
}

// ================================================================ X25519 (field arithmetic after TweetNaCl, public domain)
typedef i64 gf[16];
static const gf gf_121665 = {0xDB41, 1};

static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}
static void sel25519(gf p, gf q, int b) {
    i64 c = ~(i64)(b - 1);
    for (int i = 0; i < 16; i++) { i64 t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack25519(u8* o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = (u8)(t[i] & 0xff); o[2 * i + 1] = (u8)(t[i] >> 8); }
}
static void unpack25519(gf o, const u8* n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void M(gf o, const gf a, const gf b) {
    i64 t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}
static void S(gf o, const gf a) { M(o, a, a); }
static void inv25519(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void x25519(u8 q[32], const u8 n[32], const u8 p[32]) {
    u8 z[32];
    gf x, a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, p);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; i--) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        A(e, a, c);
        Z(a, a, c);
        A(c, b, d);
        Z(b, b, d);
        S(d, e);
        S(f, a);
        M(a, c, a);
        M(c, b, e);
        A(e, a, c);
        Z(a, a, c);
        S(b, a);
        Z(c, d, f);
        M(a, c, gf_121665);
        A(a, a, d);
        M(c, c, a);
        M(a, d, f);
        M(d, b, x);
        S(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    inv25519(c, c);
    M(a, a, c);
    pack25519(q, a);
}

void x25519_base(u8 out[32], const u8 scalar[32]) {
    u8 nine[32] = {9};
    x25519(out, scalar, nine);
}

// ================================================================ AES-128
static u8 sbox[256];
static bool sbox_ready = false;

static void build_sbox() {
    u8 p = 1, q = 1;
    do {
        p = (u8)(p ^ (u8)(p << 1) ^ ((p & 0x80) ? 0x1B : 0));
        q ^= (u8)(q << 1);
        q ^= (u8)(q << 2);
        q ^= (u8)(q << 4);
        if (q & 0x80) q ^= 0x09;
        u8 x = q ^ (u8)((q << 1) | (q >> 7)) ^ (u8)((q << 2) | (q >> 6)) ^ (u8)((q << 3) | (q >> 5)) ^ (u8)((q << 4) | (q >> 4));
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    sbox_ready = true;
}

static inline u8 xt(u8 x) { return (u8)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

static u32 sub_word(u32 w) {
    return ((u32)sbox[w >> 24] << 24) | ((u32)sbox[(w >> 16) & 255] << 16) | ((u32)sbox[(w >> 8) & 255] << 8) | sbox[w & 255];
}

void AesGcm::encrypt_block(const u8 in[16], u8 out[16]) const {
    u8 s[16];
    memcpy(s, in, 16);
    auto add_rk = [&](int r) {
        for (int c = 0; c < 4; c++) {
            u32 w = rk[r * 4 + c];
            s[4 * c] ^= w >> 24; s[4 * c + 1] ^= w >> 16; s[4 * c + 2] ^= w >> 8; s[4 * c + 3] ^= w;
        }
    };
    add_rk(0);
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
        u8 t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[4 * c + r] = s[4 * ((c + r) % 4) + r];
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                u8 a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                s[4 * c] = xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3;
                s[4 * c + 1] = a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3;
                s[4 * c + 2] = a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3;
                s[4 * c + 3] = xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3);
            }
        } else memcpy(s, t, 16);
        add_rk(round);
    }
    memcpy(out, s, 16);
}

void AesGcm::init(const u8 key[16]) {
    if (!sbox_ready) build_sbox();
    for (int i = 0; i < 4; i++) rk[i] = (key[4 * i] << 24) | (key[4 * i + 1] << 16) | (key[4 * i + 2] << 8) | key[4 * i + 3];
    u8 rcon = 1;
    for (int i = 4; i < 44; i++) {
        u32 t = rk[i - 1];
        if (i % 4 == 0) { t = sub_word((t << 8) | (t >> 24)) ^ ((u32)rcon << 24); rcon = xt(rcon); }
        rk[i] = rk[i - 4] ^ t;
    }
    u8 zero[16] = {0};
    encrypt_block(zero, H);
}

// ---------------------------------------------------------------- GCM
static void gf_mul(u8 x[16], const u8 h[16]) {
    u64 zh = 0, zl = 0, vh = 0, vl = 0;
    for (int i = 0; i < 8; i++) { vh = (vh << 8) | h[i]; vl = (vl << 8) | h[8 + i]; }
    for (int i = 0; i < 128; i++) {
        if (x[i >> 3] & (0x80 >> (i & 7))) { zh ^= vh; zl ^= vl; }
        bool lsb = vl & 1;
        vl = (vl >> 1) | (vh << 63);
        vh >>= 1;
        if (lsb) vh ^= 0xE100000000000000ull;
    }
    for (int i = 0; i < 8; i++) { x[i] = (u8)(zh >> (56 - 8 * i)); x[8 + i] = (u8)(zl >> (56 - 8 * i)); }
}

static void ghash(const u8 H[16], const u8* aad, u32 alen, const u8* ct, u32 clen, u8 out[16]) {
    u8 x[16] = {0};
    auto absorb = [&](const u8* d, u32 n) {
        for (u32 o = 0; o < n; o += 16) {
            u32 take = min<u32>(16, n - o);
            for (u32 i = 0; i < take; i++) x[i] ^= d[o + i];
            gf_mul(x, H);
        }
    };
    absorb(aad, alen);
    absorb(ct, clen);
    u8 lens[16];
    u64 ab = (u64)alen * 8, cb = (u64)clen * 8;
    for (int i = 0; i < 8; i++) { lens[i] = (u8)(ab >> (56 - 8 * i)); lens[8 + i] = (u8)(cb >> (56 - 8 * i)); }
    for (int i = 0; i < 16; i++) x[i] ^= lens[i];
    gf_mul(x, H);
    memcpy(out, x, 16);
}

static void ctr_crypt(const AesGcm& g, const u8 nonce[12], const u8* in, u32 len, u8* out) {
    u8 ctr[16], ks[16];
    memcpy(ctr, nonce, 12);
    u32 c = 2;                        // counter 1 is reserved for the tag
    for (u32 o = 0; o < len; o += 16) {
        ctr[12] = c >> 24; ctr[13] = c >> 16; ctr[14] = c >> 8; ctr[15] = c;
        g.encrypt_block(ctr, ks);
        u32 take = min<u32>(16, len - o);
        for (u32 i = 0; i < take; i++) out[o + i] = in[o + i] ^ ks[i];
        c++;
    }
}

void AesGcm::seal(const u8 nonce[12], const u8* aad, u32 alen, const u8* pt, u32 len, u8* ct, u8 tag[16]) const {
    ctr_crypt(*this, nonce, pt, len, ct);
    u8 s[16], j0[16];
    ghash(H, aad, alen, ct, len, s);
    memcpy(j0, nonce, 12);
    j0[12] = j0[13] = j0[14] = 0; j0[15] = 1;
    u8 ek[16];
    encrypt_block(j0, ek);
    for (int i = 0; i < 16; i++) tag[i] = s[i] ^ ek[i];
}

bool AesGcm::open(const u8 nonce[12], const u8* aad, u32 alen, const u8* ct, u32 len, const u8 tag[16], u8* pt) const {
    u8 s[16], j0[16], ek[16];
    ghash(H, aad, alen, ct, len, s);
    memcpy(j0, nonce, 12);
    j0[12] = j0[13] = j0[14] = 0; j0[15] = 1;
    encrypt_block(j0, ek);
    u8 diff = 0;
    for (int i = 0; i < 16; i++) diff |= (u8)((s[i] ^ ek[i]) ^ tag[i]);
    if (diff) return false;
    ctr_crypt(*this, nonce, ct, len, pt);
    return true;
}

// ================================================================ randomness
static u8 pool[32];
static u64 pool_ctr = 0;

void random_bytes(u8* out, u32 n) {
    while (n) {
        u64 tsc;
        __asm__ volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(tsc) :: "rdx");
        u8 seed[32 + 8 + 8 + 8 + 4];
        memcpy(seed, pool, 32);
        memcpy(seed + 32, &tsc, 8);
        u64 t = ticks();
        memcpy(seed + 40, &t, 8);
        pool_ctr++;
        memcpy(seed + 48, &pool_ctr, 8);
        u32 r = rand();
        memcpy(seed + 56, &r, 4);
        sha256(seed, sizeof(seed), pool);
        u8 block[32];
        sha256(pool, 32, block);
        u32 take = min<u32>(32, n);
        memcpy(out, block, take);
        out += take;
        n -= take;
    }
}

// ================================================================ self test
static bool hex_eq(const u8* a, const char* hex) {
    for (int i = 0; hex[2 * i]; i++) {
        auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
        if (a[i] != (u8)((v(hex[2 * i]) << 4) | v(hex[2 * i + 1]))) return false;
    }
    return true;
}

bool crypto_selftest() {
    u8 out[32];
    sha256("abc", 3, out);
    bool sha_ok = hex_eq(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    AesGcm g;
    u8 key[16], pt[16];
    for (int i = 0; i < 16; i++) { key[i] = (u8)i; pt[i] = (u8)(i * 0x11); }
    g.init(key);
    g.encrypt_block(pt, out);
    bool aes_ok = hex_eq(out, "69c4e0d86a7b0430d8cdb78070b4c55a");
    // RFC 7748 section 5.2, first vector
    u8 sc[32], u[32];
    const char* sh = "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4";
    const char* uh = "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c";
    auto parse = [](const char* h, u8* o) {
        auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
        for (int i = 0; i < 32; i++) o[i] = (u8)((v(h[2 * i]) << 4) | v(h[2 * i + 1]));
    };
    parse(sh, sc);
    parse(uh, u);
    x25519(out, sc, u);
    bool x_ok = hex_eq(out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    // GCM: NIST test case 3 truncated to one block (key feffe9..., iv cafebabe...)
    klog("crypto selftest: sha256 %s, aes %s, x25519 %s\n", sha_ok ? "ok" : "FAIL", aes_ok ? "ok" : "FAIL", x_ok ? "ok" : "FAIL");
    return sha_ok && aes_ok && x_ok;
}
