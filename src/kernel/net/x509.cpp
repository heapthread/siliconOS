// silicon - X.509 certificate checking (see x509.h). Self-contained: its own SHA-2, big-number
// arithmetic (Montgomery), RSA and ECDSA verification. Only verification is needed, so nothing
// here handles secrets and nothing is constant-time.
#ifdef X509_HOST        // tools/x509_host_test.cpp builds this file on the development machine
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int64_t i64;
template <typename T> static inline T min(T a, T b) { return a < b ? a : b; }
#include "x509.h"
#else
#include "x509.h"
#endif

// ================================================================ SHA-2
static const u32 K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static inline u32 ror32(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_x(const u8* data, u32 len, u8 out[32]) {
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    u64 total = (u64)len * 8;
    u8 blk[128];
    u32 pos = 0;
    for (;;) {
        u32 n = len - pos >= 64 ? 64 : len - pos;
        const u8* src = data + pos;
        bool last = false;
        if (n < 64) {                                     // final block(s)
            memcpy(blk, src, n);
            blk[n] = 0x80;
            u32 padded = (n + 1 + 8 <= 64) ? 64 : 128;
            memset(blk + n + 1, 0, padded - n - 1);
            for (int i = 0; i < 8; i++) blk[padded - 1 - i] = (u8)(total >> (8 * i));
            for (u32 off = 0; off < padded; off += 64) {
                u32 w[64];
                for (int i = 0; i < 16; i++) w[i] = ((u32)blk[off + i * 4] << 24) | (blk[off + i * 4 + 1] << 16) | (blk[off + i * 4 + 2] << 8) | blk[off + i * 4 + 3];
                for (int i = 16; i < 64; i++) { u32 s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10); w[i] = w[i - 16] + s0 + w[i - 7] + s1; }
                u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
                for (int i = 0; i < 64; i++) {
                    u32 t1 = hh + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
                    u32 t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
                    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
                }
                h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
            }
            last = true;
        } else {
            u32 w[64];
            for (int i = 0; i < 16; i++) w[i] = ((u32)src[i * 4] << 24) | (src[i * 4 + 1] << 16) | (src[i * 4 + 2] << 8) | src[i * 4 + 3];
            for (int i = 16; i < 64; i++) { u32 s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10); w[i] = w[i - 16] + s0 + w[i - 7] + s1; }
            u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
            for (int i = 0; i < 64; i++) {
                u32 t1 = hh + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
                u32 t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
                hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
            pos += 64;
        }
        if (last) break;
    }
    for (int i = 0; i < 8; i++) { out[i * 4] = (u8)(h[i] >> 24); out[i * 4 + 1] = (u8)(h[i] >> 16); out[i * 4 + 2] = (u8)(h[i] >> 8); out[i * 4 + 3] = (u8)h[i]; }
}

static const u64 K512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull, 0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};
static inline u64 ror64(u64 x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha512_core(const u8* data, u32 len, const u64 iv[8], u8* out, int outlen) {
    u64 h[8];
    for (int i = 0; i < 8; i++) h[i] = iv[i];
    u64 bits = (u64)len * 8;
    u8 blk[256];
    u32 full = len / 128 * 128;
    u32 rem = len - full;
    memcpy(blk, data + full, rem);
    blk[rem] = 0x80;
    u32 padded = (rem + 1 + 16 <= 128) ? 128 : 256;
    memset(blk + rem + 1, 0, padded - rem - 1);
    for (int i = 0; i < 8; i++) blk[padded - 1 - i] = (u8)(bits >> (8 * i));
    u32 total = full + padded;
    for (u32 off = 0; off < total; off += 128) {
        const u8* p = off < full ? data + off : blk + (off - full);
        u64 w[80];
        for (int i = 0; i < 16; i++) { u64 v = 0; for (int j = 0; j < 8; j++) v = (v << 8) | p[i * 8 + j]; w[i] = v; }
        for (int i = 16; i < 80; i++) {
            u64 s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7), s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u64 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; i++) {
            u64 t1 = hh + (ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
            u64 t2 = (ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int i = 0; i < outlen; i++) out[i] = (u8)(h[i / 8] >> (56 - 8 * (i % 8)));
}
static void sha384_x(const u8* d, u32 n, u8 out[48]) {
    static const u64 iv[8] = {0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull, 0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull};
    sha512_core(d, n, iv, out, 48);
}
static void sha512_x(const u8* d, u32 n, u8 out[64]) {
    static const u64 iv[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
    sha512_core(d, n, iv, out, 64);
}
void sha384_hash(const void* data, u32 len, u8 out[48]) { sha384_x((const u8*)data, len, out); }

enum { H_NONE = 0, H_256 = 1, H_384 = 2, H_512 = 3 };
static int hash_len(int h) { return h == H_256 ? 32 : h == H_384 ? 48 : h == H_512 ? 64 : 0; }
static void hash_data(int h, const u8* d, u32 n, u8* out) {
    if (h == H_256) sha256_x(d, n, out); else if (h == H_384) sha384_x(d, n, out); else sha512_x(d, n, out);
}

// ================================================================ big numbers (Montgomery)
typedef u32 limb;
static const int MAXL = 132;                       // 4224 bits: enough for RSA-4096

static int cmp_l(const limb* a, const limb* b, int k) {
    for (int i = k - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    return 0;
}
static limb sub_l(limb* r, const limb* a, const limb* b, int k) {         // r = a - b, returns the borrow
    u64 borrow = 0;
    for (int i = 0; i < k; i++) { u64 d = (u64)a[i] - b[i] - borrow; r[i] = (limb)d; borrow = (d >> 32) & 1; }
    return (limb)borrow;
}
static limb add_l(limb* r, const limb* a, const limb* b, int k) {
    u64 c = 0;
    for (int i = 0; i < k; i++) { u64 s = (u64)a[i] + b[i] + c; r[i] = (limb)s; c = s >> 32; }
    return (limb)c;
}
static bool is_zero_l(const limb* a, int k) { for (int i = 0; i < k; i++) if (a[i]) return false; return true; }

// big-endian bytes to limbs; returns the limb count (at least 1) or -1 when too big
static int be_to_limbs(limb* out, const u8* b, u32 len) {
    while (len > 1 && *b == 0) { b++; len--; }
    int k = (int)((len + 3) / 4);
    if (k < 1) k = 1;
    if (k > MAXL) return -1;
    memset(out, 0, sizeof(limb) * MAXL);
    for (u32 i = 0; i < len; i++) { u32 pos = len - 1 - i; out[pos / 4] |= (limb)b[i] << (8 * (pos % 4)); }
    return k;
}
static void limbs_to_be(u8* out, u32 outlen, const limb* a, int k) {
    memset(out, 0, outlen);
    for (u32 pos = 0; pos < (u32)k * 4 && pos < outlen; pos++) out[outlen - 1 - pos] = (u8)(a[pos / 4] >> (8 * (pos % 4)));
}

struct Mont { limb m[MAXL]; limb r2[MAXL]; limb one[MAXL]; int k; limb n0; };

static void dbl_mod(limb* t, const limb* m, int k) {
    limb carry = 0;
    for (int i = 0; i < k; i++) { limb v = t[i]; t[i] = (v << 1) | carry; carry = v >> 31; }
    if (carry || cmp_l(t, m, k) >= 0) sub_l(t, t, m, k);
}

static void mont_init(Mont& M, const limb* mod, int k) {
    M.k = k;
    memset(M.m, 0, sizeof(M.m));
    memcpy(M.m, mod, k * sizeof(limb));
    limb inv = mod[0];
    for (int i = 0; i < 5; i++) inv *= 2 - mod[0] * inv;
    M.n0 = (limb)(0 - inv);
    limb t[MAXL];
    memset(t, 0, sizeof(t));
    t[0] = 1;
    for (int i = 0; i < 32 * k; i++) dbl_mod(t, M.m, k);
    memset(M.one, 0, sizeof(M.one));
    memcpy(M.one, t, k * sizeof(limb));
    for (int i = 0; i < 32 * k; i++) dbl_mod(t, M.m, k);
    memset(M.r2, 0, sizeof(M.r2));
    memcpy(M.r2, t, k * sizeof(limb));
}

static void mont_mul(const Mont& M, limb* r, const limb* a, const limb* b) {
    int k = M.k;
    limb t[MAXL + 2];
    memset(t, 0, (k + 2) * sizeof(limb));
    for (int i = 0; i < k; i++) {
        u64 c = 0;
        limb bi = b[i];
        for (int j = 0; j < k; j++) { u64 s = (u64)t[j] + (u64)a[j] * bi + c; t[j] = (limb)s; c = s >> 32; }
        u64 s = (u64)t[k] + c;
        t[k] = (limb)s;
        t[k + 1] = (limb)(s >> 32);
        limb mm = t[0] * M.n0;
        c = ((u64)t[0] + (u64)mm * M.m[0]) >> 32;
        for (int j = 1; j < k; j++) { s = (u64)t[j] + (u64)mm * M.m[j] + c; t[j - 1] = (limb)s; c = s >> 32; }
        s = (u64)t[k] + c;
        t[k - 1] = (limb)s;
        t[k] = t[k + 1] + (limb)(s >> 32);
    }
    if (t[k] || cmp_l(t, M.m, k) >= 0) sub_l(t, t, M.m, k);
    memcpy(r, t, k * sizeof(limb));
}

static void to_mont(const Mont& M, limb* r, const limb* a) { mont_mul(M, r, a, M.r2); }
static void from_mont(const Mont& M, limb* r, const limb* a) {
    limb one[MAXL];
    memset(one, 0, sizeof(one));
    one[0] = 1;
    mont_mul(M, r, a, one);
}

// r = base^e in the Montgomery domain (base is already in it); e is big-endian bytes
static void mont_pow(const Mont& M, limb* r, const limb* base, const u8* e, u32 elen) {
    limb acc[MAXL], b[MAXL];
    memcpy(acc, M.one, M.k * sizeof(limb));
    memcpy(b, base, M.k * sizeof(limb));
    for (u32 i = 0; i < elen; i++)
        for (int bit = 7; bit >= 0; bit--) {
            mont_mul(M, acc, acc, acc);
            if ((e[i] >> bit) & 1) mont_mul(M, acc, acc, b);
        }
    memcpy(r, acc, M.k * sizeof(limb));
}

// ================================================================ RSA
// returns the k-byte result of sig^e mod n, or false
static bool rsa_public(const PubKey& key, const u8* sig, u32 slen, u8* out, u32* outlen) {
    limb n[MAXL], s[MAXL], r[MAXL];
    int k = be_to_limbs(n, key.n, key.nlen);
    if (k < 1 || k < 8) return false;                                // at least 256 bits
    if (be_to_limbs(s, sig, slen) < 0) return false;
    if (cmp_l(s, n, MAXL) >= 0) return false;
    Mont M;
    mont_init(M, n, k);
    limb sm[MAXL];
    to_mont(M, sm, s);
    u8 e[4] = {(u8)(key.e >> 24), (u8)(key.e >> 16), (u8)(key.e >> 8), (u8)key.e};
    limb rm[MAXL];
    mont_pow(M, rm, sm, e, 4);
    from_mont(M, r, rm);
    u32 nbytes = key.nlen;
    while (nbytes > 1 && key.n[key.nlen - nbytes] == 0) nbytes--;
    limbs_to_be(out, nbytes, r, k);
    *outlen = nbytes;
    return true;
}

static const u8 DI256[19] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
static const u8 DI384[19] = {0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30};
static const u8 DI512[19] = {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};

static bool rsa_pkcs1_verify(const PubKey& key, int h, const u8* msg, u32 mlen, const u8* sig, u32 slen) {
    static u8 em[MAXL * 4];
    u32 emlen;
    if (!rsa_public(key, sig, slen, em, &emlen)) return false;
    u32 hl = (u32)hash_len(h);
    u8 digest[64];
    hash_data(h, msg, mlen, digest);
    const u8* di = h == H_256 ? DI256 : h == H_384 ? DI384 : DI512;
    u32 tlen = 19 + hl;
    if (emlen < tlen + 11) return false;
    u32 ps = emlen - tlen - 3;
    if (em[0] != 0 || em[1] != 1) return false;
    for (u32 i = 0; i < ps; i++) if (em[2 + i] != 0xFF) return false;
    if (em[2 + ps] != 0) return false;
    if (memcmp(em + 3 + ps, di, 19)) return false;
    return !memcmp(em + 3 + ps + 19, digest, hl);
}

static void mgf1(int h, const u8* seed, u32 slen, u8* out, u32 outlen) {
    u32 hl = (u32)hash_len(h);
    u8 buf[64 + 4 + 64];
    memcpy(buf, seed, slen);
    u8 d[64];
    for (u32 counter = 0, off = 0; off < outlen; counter++) {
        buf[slen] = (u8)(counter >> 24); buf[slen + 1] = (u8)(counter >> 16); buf[slen + 2] = (u8)(counter >> 8); buf[slen + 3] = (u8)counter;
        hash_data(h, buf, slen + 4, d);
        u32 n = min(hl, outlen - off);
        memcpy(out + off, d, n);
        off += n;
    }
}

static bool rsa_pss_verify(const PubKey& key, int h, const u8* msg, u32 mlen, const u8* sig, u32 slen) {
    static u8 em[MAXL * 4];
    u32 emlen_full;
    if (!rsa_public(key, sig, slen, em, &emlen_full)) return false;
    // the modulus bit length decides how many bits of em are used
    u32 bits = emlen_full * 8;
    u8 top = key.n[0];
    for (u32 i = 0; i < key.nlen && key.n[i] == 0; i++) top = key.n[i + 1 < key.nlen ? i + 1 : i];
    u32 lead = 0;
    while (lead < key.nlen && key.n[lead] == 0) lead++;
    top = key.n[lead];
    u32 topbits = 0;
    while (top) { topbits++; top >>= 1; }
    u32 modbits = (key.nlen - lead - 1) * 8 + topbits;
    (void)bits;
    u32 embits = modbits - 1;
    u32 emlen = (embits + 7) / 8;
    const u8* p = em + (emlen_full - emlen);                          // em may have one byte less than the modulus
    u32 hl = (u32)hash_len(h);
    if (emlen < hl + 2) return false;
    if (p[emlen - 1] != 0xBC) return false;
    u32 dblen = emlen - hl - 1;
    const u8* H = p + dblen;
    u32 zero_bits = 8 * emlen - embits;
    if (p[0] & (0xFF << (8 - zero_bits)) & 0xFF) { if (zero_bits) return false; }
    static u8 db[MAXL * 4];
    mgf1(h, H, hl, db, dblen);
    for (u32 i = 0; i < dblen; i++) db[i] ^= p[i];
    if (zero_bits) db[0] &= (u8)(0xFF >> zero_bits);
    u32 i = 0;
    while (i < dblen && db[i] == 0) i++;
    if (i >= dblen || db[i] != 1) return false;
    const u8* salt = db + i + 1;
    u32 slt = dblen - i - 1;
    u8 mh[64], mp[8 + 64 + 64], hh[64];
    hash_data(h, msg, mlen, mh);
    memset(mp, 0, 8);
    memcpy(mp + 8, mh, hl);
    if (slt > 64) return false;
    memcpy(mp + 8 + hl, salt, slt);
    hash_data(h, mp, 8 + hl + slt, hh);
    return !memcmp(hh, H, hl);
}

// ================================================================ ECDSA (P-256, P-384)
struct Curve {
    bool ready;
    int bytes;                               // field size in bytes
    int k;                                   // limbs
    Mont P, N;
    limb gx[MAXL], gy[MAXL];                 // in the Montgomery domain of P
    u8 pm2[48], nm2[48];                     // p-2, n-2 (for inversion)
};
static Curve curve256, curve384;

static int hex_bytes(const char* s, u8* out) {
    int n = 0;
    for (; s[0] && s[1]; s += 2) {
        auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
        out[n++] = (u8)(v(s[0]) * 16 + v(s[1]));
    }
    return n;
}

static void curve_init(Curve& c, int bits, const char* p, const char* n, const char* gx, const char* gy) {
    u8 b[48];
    c.bytes = bits / 8;
    int len = hex_bytes(p, b);
    limb t[MAXL];
    c.k = be_to_limbs(t, b, (u32)len);
    mont_init(c.P, t, c.k);
    memcpy(c.pm2, b, len); c.pm2[len - 1] -= 2;
    len = hex_bytes(n, b);
    be_to_limbs(t, b, (u32)len);
    mont_init(c.N, t, c.k);
    memcpy(c.nm2, b, len); c.nm2[len - 1] -= 2;
    len = hex_bytes(gx, b);
    be_to_limbs(t, b, (u32)len);
    to_mont(c.P, c.gx, t);
    len = hex_bytes(gy, b);
    be_to_limbs(t, b, (u32)len);
    to_mont(c.P, c.gy, t);
    c.ready = true;
}

static Curve* get_curve(int kind) {
    if (kind == 2) {
        if (!curve256.ready) curve_init(curve256, 256, "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff", "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
                                        "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296", "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
        return &curve256;
    }
    if (!curve384.ready) curve_init(curve384, 384, "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
                                    "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
                                    "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
                                    "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f");
    return &curve384;
}

struct Pt { limb x[MAXL], y[MAXL], z[MAXL]; };

static void f_add(const Mont& M, limb* r, const limb* a, const limb* b) {
    limb c = add_l(r, a, b, M.k);
    if (c || cmp_l(r, M.m, M.k) >= 0) sub_l(r, r, M.m, M.k);
}
static void f_sub(const Mont& M, limb* r, const limb* a, const limb* b) {
    limb br = sub_l(r, a, b, M.k);
    if (br) add_l(r, r, M.m, M.k);
}
static void f_mul(const Mont& M, limb* r, const limb* a, const limb* b) { mont_mul(M, r, a, b); }

static void pt_inf(const Curve& c, Pt& p) { memset(&p, 0, sizeof(p)); memcpy(p.x, c.P.one, c.k * 4); memcpy(p.y, c.P.one, c.k * 4); }
static bool pt_is_inf(const Curve& c, const Pt& p) { return is_zero_l(p.z, c.k); }

static void pt_dbl(const Curve& c, Pt& r, const Pt& p) {
    const Mont& M = c.P;
    if (pt_is_inf(c, p) || is_zero_l(p.y, c.k)) { pt_inf(c, r); return; }
    limb delta[MAXL], gamma[MAXL], beta[MAXL], alpha[MAXL], t1[MAXL], t2[MAXL], x3[MAXL], y3[MAXL], z3[MAXL];
    f_mul(M, delta, p.z, p.z);
    f_mul(M, gamma, p.y, p.y);
    f_mul(M, beta, p.x, gamma);
    f_sub(M, t1, p.x, delta);
    f_add(M, t2, p.x, delta);
    f_mul(M, alpha, t1, t2);
    f_add(M, t1, alpha, alpha);
    f_add(M, alpha, t1, alpha);                       // 3 * (x-delta)(x+delta)
    f_mul(M, x3, alpha, alpha);
    f_add(M, t1, beta, beta);
    f_add(M, t1, t1, t1);
    f_add(M, t2, t1, t1);                             // 8 beta
    f_sub(M, x3, x3, t2);
    f_add(M, t2, p.y, p.z);
    f_mul(M, z3, t2, t2);
    f_sub(M, z3, z3, gamma);
    f_sub(M, z3, z3, delta);
    f_sub(M, t1, t1, x3);                             // 4 beta - x3
    f_mul(M, y3, alpha, t1);
    f_mul(M, t2, gamma, gamma);
    f_add(M, t2, t2, t2); f_add(M, t2, t2, t2); f_add(M, t2, t2, t2);   // 8 gamma^2
    f_sub(M, y3, y3, t2);
    memcpy(r.x, x3, c.k * 4); memcpy(r.y, y3, c.k * 4); memcpy(r.z, z3, c.k * 4);
}

static void pt_add(const Curve& c, Pt& r, const Pt& p, const Pt& q) {
    const Mont& M = c.P;
    if (pt_is_inf(c, p)) { r = q; return; }
    if (pt_is_inf(c, q)) { r = p; return; }
    limb z1z1[MAXL], z2z2[MAXL], u1[MAXL], u2[MAXL], s1[MAXL], s2[MAXL], h[MAXL], i[MAXL], j[MAXL], rr[MAXL], v[MAXL], t[MAXL], x3[MAXL], y3[MAXL], z3[MAXL];
    f_mul(M, z1z1, p.z, p.z);
    f_mul(M, z2z2, q.z, q.z);
    f_mul(M, u1, p.x, z2z2);
    f_mul(M, u2, q.x, z1z1);
    f_mul(M, t, q.z, z2z2); f_mul(M, s1, p.y, t);
    f_mul(M, t, p.z, z1z1); f_mul(M, s2, q.y, t);
    f_sub(M, h, u2, u1);
    f_sub(M, rr, s2, s1);
    if (is_zero_l(h, c.k)) {
        if (is_zero_l(rr, c.k)) { pt_dbl(c, r, p); return; }
        pt_inf(c, r);
        return;
    }
    f_add(M, rr, rr, rr);
    f_add(M, t, h, h);
    f_mul(M, i, t, t);
    f_mul(M, j, h, i);
    f_mul(M, v, u1, i);
    f_mul(M, x3, rr, rr);
    f_sub(M, x3, x3, j);
    f_sub(M, x3, x3, v);
    f_sub(M, x3, x3, v);
    f_sub(M, t, v, x3);
    f_mul(M, y3, rr, t);
    f_mul(M, t, s1, j);
    f_add(M, t, t, t);
    f_sub(M, y3, y3, t);
    f_add(M, t, p.z, q.z);
    f_mul(M, z3, t, t);
    f_sub(M, z3, z3, z1z1);
    f_sub(M, z3, z3, z2z2);
    f_mul(M, z3, z3, h);
    memcpy(r.x, x3, c.k * 4); memcpy(r.y, y3, c.k * 4); memcpy(r.z, z3, c.k * 4);
}

// rawlen-byte big-endian integer -> limbs reduced below the modulus (one conditional subtraction)
static bool int_limbs(limb* out, const u8* b, u32 len, int k) {
    while (len > (u32)k * 4 && *b == 0) { b++; len--; }
    if (len > (u32)k * 4) return false;
    memset(out, 0, sizeof(limb) * MAXL);
    for (u32 i = 0; i < len; i++) { u32 pos = len - 1 - i; out[pos / 4] |= (limb)b[i] << (8 * (pos % 4)); }
    return true;
}

static bool ecdsa_verify(const PubKey& key, int hash, const u8* msg, u32 mlen, const u8* sig, u32 slen) {
    Curve* c = get_curve(key.kind);
    int k = c->k;
    u32 fb = (u32)c->bytes;
    if (key.ptlen != 1 + 2 * fb || key.pt[0] != 4) return false;
    // signature: SEQ { INTEGER r, INTEGER s }
    const u8* p = sig;
    const u8* end = sig + slen;
    auto tlv = [&](const u8*& q, const u8*& val, u32& vl) -> bool {
        if (end - q < 2) return false;
        q++;
        u32 l = *q++;
        if (l & 0x80) { int nb = l & 0x7F; if (!nb || nb > 2 || end - q < nb) return false; l = 0; while (nb--) l = (l << 8) | *q++; }
        if ((u32)(end - q) < l) return false;
        val = q; vl = l; q += l;
        return true;
    };
    if (slen < 8 || sig[0] != 0x30) return false;
    const u8* val; u32 vl;
    if (!tlv(p, val, vl)) return false;
    const u8* q = val;
    const u8 *rv, *sv; u32 rl, sl;
    if (*q != 2 || !tlv(q, rv, rl)) return false;
    if (*q != 2 || !tlv(q, sv, sl)) return false;
    limb r[MAXL], s[MAXL];
    if (!int_limbs(r, rv, rl, k) || !int_limbs(s, sv, sl, k)) return false;
    if (is_zero_l(r, k) || is_zero_l(s, k) || cmp_l(r, c->N.m, k) >= 0 || cmp_l(s, c->N.m, k) >= 0) return false;
    // e: the leftmost bits of the hash
    u8 digest[64];
    hash_data(hash, msg, mlen, digest);
    u32 hl = (u32)hash_len(hash);
    limb e[MAXL];
    if (!int_limbs(e, digest, min(hl, fb), k)) return false;
    if (cmp_l(e, c->N.m, k) >= 0) sub_l(e, e, c->N.m, k);
    // w = s^-1 mod n; u1 = e w; u2 = r w
    limb sm[MAXL], wm[MAXL], em[MAXL], rm[MAXL], u1m[MAXL], u2m[MAXL], u1[MAXL], u2[MAXL];
    to_mont(c->N, sm, s);
    mont_pow(c->N, wm, sm, c->nm2, fb);
    to_mont(c->N, em, e);
    to_mont(c->N, rm, r);
    mont_mul(c->N, u1m, em, wm);
    mont_mul(c->N, u2m, rm, wm);
    from_mont(c->N, u1, u1m);
    from_mont(c->N, u2, u2m);
    u8 b1[48], b2[48];
    limbs_to_be(b1, fb, u1, k);
    limbs_to_be(b2, fb, u2, k);
    // Q in the Montgomery domain
    Pt Q, G, GQ, R;
    limb t[MAXL];
    int_limbs(t, key.pt + 1, fb, k);
    to_mont(c->P, Q.x, t);
    int_limbs(t, key.pt + 1 + fb, fb, k);
    to_mont(c->P, Q.y, t);
    memcpy(Q.z, c->P.one, k * 4);
    memcpy(G.x, c->gx, k * 4); memcpy(G.y, c->gy, k * 4); memcpy(G.z, c->P.one, k * 4);
    pt_add(*c, GQ, G, Q);
    pt_inf(*c, R);
    for (u32 i = 0; i < fb; i++)
        for (int bit = 7; bit >= 0; bit--) {
            pt_dbl(*c, R, R);
            int a = (b1[i] >> bit) & 1, b = (b2[i] >> bit) & 1;
            if (a && b) pt_add(*c, R, R, GQ);
            else if (a) pt_add(*c, R, R, G);
            else if (b) pt_add(*c, R, R, Q);
        }
    if (pt_is_inf(*c, R)) return false;
    // affine x = X / Z^2
    limb zi[MAXL], zi2[MAXL], xa[MAXL], xn[MAXL];
    mont_pow(c->P, zi, R.z, c->pm2, fb);
    f_mul(c->P, zi2, zi, zi);
    f_mul(c->P, xa, R.x, zi2);
    from_mont(c->P, xn, xa);
    if (cmp_l(xn, c->N.m, k) >= 0) sub_l(xn, xn, c->N.m, k);
    return cmp_l(xn, r, k) == 0;
}

// ---------------------------------------------------------------- P-256 key exchange
static void ec_mul(const Curve& c, Pt& R, const Pt& P, const u8* k, u32 klen) {
    pt_inf(c, R);
    for (u32 i = 0; i < klen; i++)
        for (int bit = 7; bit >= 0; bit--) {
            pt_dbl(c, R, R);
            if ((k[i] >> bit) & 1) pt_add(c, R, R, P);
        }
}

static bool to_affine(const Curve& c, const Pt& R, u8* x, u8* y) {
    if (pt_is_inf(c, R)) return false;
    limb zi[MAXL], zi2[MAXL], zi3[MAXL], xa[MAXL], ya[MAXL], t[MAXL];
    mont_pow(c.P, zi, R.z, c.pm2, (u32)c.bytes);
    f_mul(c.P, zi2, zi, zi);
    f_mul(c.P, zi3, zi2, zi);
    f_mul(c.P, xa, R.x, zi2);
    f_mul(c.P, ya, R.y, zi3);
    from_mont(c.P, t, xa); limbs_to_be(x, (u32)c.bytes, t, c.k);
    from_mont(c.P, t, ya); limbs_to_be(y, (u32)c.bytes, t, c.k);
    return true;
}

bool p256_public(const u8 priv[32], u8 pub[65]) {
    Curve* c = get_curve(2);
    Pt G, R;
    memcpy(G.x, c->gx, c->k * 4); memcpy(G.y, c->gy, c->k * 4); memcpy(G.z, c->P.one, c->k * 4);
    ec_mul(*c, R, G, priv, 32);
    pub[0] = 4;
    return to_affine(*c, R, pub + 1, pub + 33);
}

bool p256_shared(const u8 priv[32], const u8 peer[65], u8 out[32]) {
    Curve* c = get_curve(2);
    int k = c->k;
    if (peer[0] != 4) return false;
    limb x[MAXL], y[MAXL], xm[MAXL], ym[MAXL];
    if (!int_limbs(x, peer + 1, 32, k) || !int_limbs(y, peer + 33, 32, k)) return false;
    if (cmp_l(x, c->P.m, k) >= 0 || cmp_l(y, c->P.m, k) >= 0) return false;
    to_mont(c->P, xm, x);
    to_mont(c->P, ym, y);
    // y^2 = x^3 - 3x + b
    static const char* B = "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b";
    u8 bb[32];
    hex_bytes(B, bb);
    limb bl[MAXL], bm[MAXL], lhs[MAXL], rhs[MAXL], t[MAXL], t3[MAXL];
    int_limbs(bl, bb, 32, k);
    to_mont(c->P, bm, bl);
    f_mul(c->P, lhs, ym, ym);
    f_mul(c->P, t, xm, xm);
    f_mul(c->P, rhs, t, xm);
    f_add(c->P, t3, xm, xm);
    f_add(c->P, t3, t3, xm);
    f_sub(c->P, rhs, rhs, t3);
    f_add(c->P, rhs, rhs, bm);
    if (cmp_l(lhs, rhs, k) != 0) return false;
    Pt Q, R;
    memcpy(Q.x, xm, k * 4); memcpy(Q.y, ym, k * 4); memcpy(Q.z, c->P.one, k * 4);
    ec_mul(*c, R, Q, priv, 32);
    u8 yy[32];
    return to_affine(*c, R, out, yy);
}

// ================================================================ DER and certificates
static bool tlv(const u8*& p, const u8* end, u8* tag, const u8** val, u32* vlen, const u8** start = nullptr) {
    if (end - p < 2) return false;
    const u8* s = p;
    u8 t = *p++;
    u32 l = *p++;
    if (l & 0x80) {
        int nb = l & 0x7F;
        if (nb == 0 || nb > 3 || end - p < nb) return false;
        l = 0;
        while (nb--) l = (l << 8) | *p++;
    }
    if ((u32)(end - p) < l) return false;
    *tag = t; *val = p; *vlen = l;
    p += l;
    if (start) *start = s;
    return true;
}

enum { SA_NONE = 0, SA_RSA256, SA_RSA384, SA_RSA512, SA_EC256, SA_EC384, SA_EC512, SA_SHA1 };

struct Cert {
    const u8* der; u32 der_len;
    const u8* tbs; u32 tbs_len;
    const u8* issuer; u32 issuer_len;
    const u8* subject; u32 subject_len;
    int sig_alg;
    const u8* sig; u32 sig_len;
    PubKey key;
    i64 not_before, not_after;
    bool has_bc, is_ca;
    int path_len;
    const u8* san; u32 san_len;
    char cn[96];
};

static i64 days_from_civil(i64 y, int m, int d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// UTCTime / GeneralizedTime to seconds since 2000-01-01
static bool parse_time(u8 tag, const u8* v, u32 l, i64* out) {
    auto two = [&](u32 i) { return (v[i] - '0') * 10 + (v[i + 1] - '0'); };
    i64 y;
    u32 o;
    if (tag == 0x17 && l >= 11) { int yy = two(0); y = yy < 50 ? 2000 + yy : 1900 + yy; o = 2; }
    else if (tag == 0x18 && l >= 13) { y = two(0) * 100 + two(2); o = 4; }
    else return false;
    int mo = two(o), d = two(o + 2), h = two(o + 4), mi = two(o + 6), s = (l >= o + 10 && v[o + 8] >= '0' && v[o + 8] <= '9') ? two(o + 8) : 0;
    *out = (days_from_civil(y, mo, d) - 10957) * 86400 + h * 3600 + mi * 60 + s;
    return true;
}

static int sigalg_from_oid(const u8* o, u32 l) {
    static const u8 rsa[8] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01};
    static const u8 ecd[7] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03};
    if (l == 9 && !memcmp(o, rsa, 8)) {
        switch (o[8]) { case 0x0B: return SA_RSA256; case 0x0C: return SA_RSA384; case 0x0D: return SA_RSA512; case 0x05: return SA_SHA1; }
    }
    if (l == 8 && !memcmp(o, ecd, 7)) {
        switch (o[7]) { case 2: return SA_EC256; case 3: return SA_EC384; case 4: return SA_EC512; case 1: return SA_SHA1; }
    }
    return SA_NONE;
}

static bool parse_spki(const u8* v, u32 l, PubKey* k) {
    const u8* p = v;
    const u8* end = v + l;
    u8 tag; const u8* val; u32 vl;
    memset(k, 0, sizeof(*k));
    if (!tlv(p, end, &tag, &val, &vl) || tag != 0x30) return false;
    const u8* op = val;                                       // inside the outer SEQUENCE: AlgorithmIdentifier, then the key bits
    const u8* oend = val + vl;
    const u8 *algv; u32 algl; u8 algt;
    if (!tlv(op, oend, &algt, &algv, &algl) || algt != 0x30) return false;
    const u8* ap = algv;
    const u8* aend = algv + algl;
    const u8 *oid, *par = nullptr; u32 ol, pl = 0;
    u8 t2;
    if (!tlv(ap, aend, &t2, &oid, &ol) || t2 != 6) return false;
    u8 pt = 0;
    if (ap < aend) tlv(ap, aend, &pt, &par, &pl);
    const u8* bits; u32 bl;
    if (!tlv(op, oend, &tag, &bits, &bl) || tag != 3 || bl < 2) return false;
    bits++; bl--;
    static const u8 RSA_OID[9] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01};
    static const u8 EC_OID[7] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
    static const u8 P256[8] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
    static const u8 P384[5] = {0x2B, 0x81, 0x04, 0x00, 0x22};
    if (ol == 9 && !memcmp(oid, RSA_OID, 9)) {
        const u8* q = bits;
        const u8* qend = bits + bl;
        const u8* sv; u32 sl; u8 st;
        if (!tlv(q, qend, &st, &sv, &sl) || st != 0x30) return false;
        const u8* r = sv;
        const u8* rend = sv + sl;
        const u8 *nv, *ev; u32 nl, el; u8 nt, et;
        if (!tlv(r, rend, &nt, &nv, &nl) || nt != 2 || !tlv(r, rend, &et, &ev, &el) || et != 2) return false;
        while (nl > 1 && *nv == 0) { nv++; nl--; }
        if (el > 4) return false;
        u32 e = 0;
        for (u32 i = 0; i < el; i++) e = (e << 8) | ev[i];
        k->kind = 1; k->n = nv; k->nlen = nl; k->e = e;
        return true;
    }
    if (ol == 7 && !memcmp(oid, EC_OID, 7) && pt == 6) {
        if (pl == 8 && !memcmp(par, P256, 8)) k->kind = 2;
        else if (pl == 5 && !memcmp(par, P384, 5)) k->kind = 3;
        else return false;
        k->pt = bits; k->ptlen = bl;
        return true;
    }
    return false;
}

static void parse_name_cn(const u8* name, u32 nl, char* out, int outlen) {     // name = contents of the Name SEQUENCE
    out[0] = 0;
    const u8* p = name;
    const u8* end = name + nl;
    u8 tag; const u8* v; u32 vl;
    while (p < end && tlv(p, end, &tag, &v, &vl)) {                           // RDN: SET
        const u8* q = v;
        const u8* qend = v + vl;
        const u8* av; u32 al; u8 at;
        while (q < qend && tlv(q, qend, &at, &av, &al)) {                     // AttributeTypeAndValue: SEQ
            const u8* a = av;
            const u8* aend = av + al;
            const u8 *ov, *sv; u32 ol, sl; u8 ot, st;
            if (!tlv(a, aend, &ot, &ov, &ol) || !tlv(a, aend, &st, &sv, &sl)) continue;
            if (ol == 3 && ov[0] == 0x55 && ov[1] == 4 && ov[2] == 3) {
                u32 n = min<u32>(sl, (u32)outlen - 1);
                memcpy(out, sv, n);
                out[n] = 0;
                return;
            }
        }
    }
}

static bool parse_cert(const u8* der, u32 len, Cert* c) {
    memset(c, 0, sizeof(*c));
    c->path_len = -1;
    const u8* p = der;
    const u8* end = der + len;
    u8 tag; const u8* val; u32 vl;
    const u8* start;
    if (!tlv(p, end, &tag, &val, &vl, &start) || tag != 0x30) return false;
    c->der = der; c->der_len = (u32)(p - der);
    const u8* q = val;
    const u8* qend = val + vl;
    const u8* tbs_val; u32 tbs_vl; const u8* tbs_start;
    if (!tlv(q, qend, &tag, &tbs_val, &tbs_vl, &tbs_start) || tag != 0x30) return false;
    c->tbs = tbs_start; c->tbs_len = (u32)((tbs_val + tbs_vl) - tbs_start);
    const u8* av; u32 al;
    if (!tlv(q, qend, &tag, &av, &al) || tag != 0x30) return false;
    {
        const u8* a = av;
        const u8 *ov; u32 ol; u8 ot;
        if (!tlv(a, av + al, &ot, &ov, &ol) || ot != 6) return false;
        c->sig_alg = sigalg_from_oid(ov, ol);
    }
    const u8* sv; u32 sl;
    if (!tlv(q, qend, &tag, &sv, &sl) || tag != 3 || sl < 2) return false;
    c->sig = sv + 1; c->sig_len = sl - 1;
    // inside the TBS
    const u8* t = tbs_val;
    const u8* tend = tbs_val + tbs_vl;
    const u8* v2; u32 l2; const u8* s2;
    if (!tlv(t, tend, &tag, &v2, &l2, &s2)) return false;
    if (tag == 0xA0) { if (!tlv(t, tend, &tag, &v2, &l2, &s2)) return false; }      // version, then the serial
    // v2 is now the serial; signature algorithm
    if (!tlv(t, tend, &tag, &v2, &l2, &s2) || tag != 0x30) return false;
    if (!tlv(t, tend, &tag, &v2, &l2, &s2) || tag != 0x30) return false;             // issuer
    c->issuer = s2; c->issuer_len = (u32)((v2 + l2) - s2);
    if (!tlv(t, tend, &tag, &v2, &l2, &s2) || tag != 0x30) return false;             // validity
    {
        const u8* vp = v2;
        const u8* vend = v2 + l2;
        const u8 *a; u32 b; u8 ta;
        if (!tlv(vp, vend, &ta, &a, &b) || !parse_time(ta, a, b, &c->not_before)) return false;
        if (!tlv(vp, vend, &ta, &a, &b) || !parse_time(ta, a, b, &c->not_after)) return false;
    }
    if (!tlv(t, tend, &tag, &v2, &l2, &s2) || tag != 0x30) return false;             // subject
    c->subject = s2; c->subject_len = (u32)((v2 + l2) - s2);
    parse_name_cn(v2, l2, c->cn, sizeof(c->cn));
    if (!tlv(t, tend, &tag, &v2, &l2, &s2) || tag != 0x30) return false;             // subjectPublicKeyInfo
    if (!parse_spki(s2, (u32)((v2 + l2) - s2), &c->key)) c->key.kind = 0;
    while (t < tend && tlv(t, tend, &tag, &v2, &l2, &s2)) {
        if (tag != 0xA3) continue;                                                  // extensions [3]
        const u8* e = v2;
        const u8* eend = v2 + l2;
        const u8 *sv2; u32 sl2; u8 st;
        if (!tlv(e, eend, &st, &sv2, &sl2) || st != 0x30) break;
        const u8* x = sv2;
        const u8* xend = sv2 + sl2;
        while (x < xend) {
            const u8 *ev; u32 el; u8 et;
            if (!tlv(x, xend, &et, &ev, &el) || et != 0x30) break;
            const u8* f = ev;
            const u8* fend = ev + el;
            const u8 *ov; u32 ol; u8 ot;
            if (!tlv(f, fend, &ot, &ov, &ol) || ot != 6) continue;
            const u8 *fv; u32 fl; u8 ft;
            if (!tlv(f, fend, &ft, &fv, &fl)) continue;
            if (ft == 1 && !tlv(f, fend, &ft, &fv, &fl)) continue;                  // skip "critical"
            if (ft != 4) continue;
            if (ol == 3 && ov[0] == 0x55 && ov[1] == 0x1D && ov[2] == 0x11) { c->san = fv; c->san_len = fl; }
            if (ol == 3 && ov[0] == 0x55 && ov[1] == 0x1D && ov[2] == 0x13) {
                c->has_bc = true;
                const u8* b = fv;
                const u8* bend = fv + fl;
                const u8 *sv3; u32 sl3; u8 s3;
                if (tlv(b, bend, &s3, &sv3, &sl3) && s3 == 0x30) {
                    const u8* y = sv3;
                    const u8* yend = sv3 + sl3;
                    const u8 *iv; u32 il; u8 it;
                    if (y < yend && tlv(y, yend, &it, &iv, &il) && it == 1) { c->is_ca = il && iv[0]; if (y < yend && tlv(y, yend, &it, &iv, &il) && it == 2 && il) c->path_len = iv[il - 1]; }
                    else if (y <= yend && it == 2 && il) c->path_len = iv[il - 1];
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- signatures
static bool verify_cert_sig(const Cert& c, const PubKey& issuer_key) {
    int h;
    bool rsa;
    switch (c.sig_alg) {
    case SA_RSA256: h = H_256; rsa = true; break;
    case SA_RSA384: h = H_384; rsa = true; break;
    case SA_RSA512: h = H_512; rsa = true; break;
    case SA_EC256: h = H_256; rsa = false; break;
    case SA_EC384: h = H_384; rsa = false; break;
    case SA_EC512: h = H_512; rsa = false; break;
    default: return false;
    }
    if (rsa) return issuer_key.kind == 1 && rsa_pkcs1_verify(issuer_key, h, c.tbs, c.tbs_len, c.sig, c.sig_len);
    return (issuer_key.kind == 2 || issuer_key.kind == 3) && ecdsa_verify(issuer_key, h, c.tbs, c.tbs_len, c.sig, c.sig_len);
}

bool x509_verify_tls_sig(const PubKey& key, u16 scheme, const u8* msg, u32 mlen, const u8* sig, u32 slen) {
    switch (scheme) {
    case 0x0401: return key.kind == 1 && rsa_pkcs1_verify(key, H_256, msg, mlen, sig, slen);
    case 0x0501: return key.kind == 1 && rsa_pkcs1_verify(key, H_384, msg, mlen, sig, slen);
    case 0x0601: return key.kind == 1 && rsa_pkcs1_verify(key, H_512, msg, mlen, sig, slen);
    case 0x0804: case 0x0809: return key.kind == 1 && rsa_pss_verify(key, H_256, msg, mlen, sig, slen);
    case 0x0805: case 0x080A: return key.kind == 1 && rsa_pss_verify(key, H_384, msg, mlen, sig, slen);
    case 0x0806: case 0x080B: return key.kind == 1 && rsa_pss_verify(key, H_512, msg, mlen, sig, slen);
    case 0x0403: return (key.kind == 2 || key.kind == 3) && ecdsa_verify(key, H_256, msg, mlen, sig, slen);      // TLS 1.2 lets any hash go with any curve
    case 0x0503: return (key.kind == 2 || key.kind == 3) && ecdsa_verify(key, H_384, msg, mlen, sig, slen);
    case 0x0603: return (key.kind == 2 || key.kind == 3) && ecdsa_verify(key, H_512, msg, mlen, sig, slen);
    }
    return false;
}

bool x509_public_key(const u8* der, u32 len, PubKey* out) {
    Cert c;
    if (!parse_cert(der, len, &c) || !c.key.kind) return false;
    *out = c.key;
    return true;
}

// ---------------------------------------------------------------- the root store
#ifdef X509_NO_ROOTS
static const u8 ROOTS[1] = {0};
static const u32 ROOTS_LEN = 0;
#else
#include "roots.inc"
#endif

struct RootRef { const u8* subject; u32 slen; PubKey key; };

static bool next_root(u32& off, RootRef* r) {
    if (off + 2 > ROOTS_LEN) return false;
    const u8* d = ROOTS;
    r->slen = (u32)((d[off] << 8) | d[off + 1]);
    off += 2;
    r->subject = d + off;
    off += r->slen;
    u8 kind = d[off++];
    u32 al = (u32)((d[off] << 8) | d[off + 1]);
    off += 2;
    const u8* a = d + off;
    off += al;
    u32 bl = (u32)((d[off] << 8) | d[off + 1]);
    off += 2;
    const u8* b = d + off;
    off += bl;
    memset(&r->key, 0, sizeof(r->key));
    r->key.kind = kind;
    if (kind == 1) { r->key.n = a; r->key.nlen = al; u32 e = 0; for (u32 i = 0; i < bl; i++) e = (e << 8) | b[i]; r->key.e = e; }
    else { r->key.pt = a; r->key.ptlen = al; }
    return true;
}

// ---------------------------------------------------------------- host names
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static bool name_match(const char* pattern, u32 plen, const char* host) {
    u32 hl = (u32)strlen(host);
    if (plen > 2 && pattern[0] == '*' && pattern[1] == '.') {
        const char* dot = strchr(host, '.');
        if (!dot) return false;
        u32 rest = hl - (u32)(dot - host);                                    // ".example.com"
        if (rest != plen - 1) return false;
        for (u32 i = 0; i < rest; i++) if (lower(dot[i]) != lower(pattern[1 + i])) return false;
        return dot != host;
    }
    if (plen != hl) return false;
    for (u32 i = 0; i < plen; i++) if (lower(pattern[i]) != lower(host[i])) return false;
    return true;
}

static bool parse_ipv4(const char* s, u8 out[4]) {
    int n = 0;
    u32 v = 0;
    bool any = false;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') { v = v * 10 + (u32)(*s - '0'); any = true; if (v > 255) return false; }
        else if (*s == '.' || *s == 0) { if (!any || n >= 4) return false; out[n++] = (u8)v; v = 0; any = false; if (!*s) break; }
        else return false;
    }
    return n == 4;
}

static bool host_matches(const Cert& c, const char* host) {
    u8 ip[4];
    bool is_ip = parse_ipv4(host, ip);
    bool any_san = false;
    if (c.san) {
        const u8* p = c.san;
        const u8* end = c.san + c.san_len;
        u8 tag; const u8* v; u32 vl;
        const u8* seq_end = end;
        if (tlv(p, end, &tag, &v, &vl) && tag == 0x30) {
            const u8* g = v;
            seq_end = v + vl;
            while (g < seq_end && tlv(g, seq_end, &tag, &v, &vl)) {
                if (tag == 0x82) { any_san = true; if (!is_ip && name_match((const char*)v, vl, host)) return true; }
                else if (tag == 0x87) { any_san = true; if (is_ip && vl == 4 && !memcmp(v, ip, 4)) return true; }
            }
        }
    }
    if (any_san) return false;
    return !is_ip && c.cn[0] && name_match(c.cn, (u32)strlen(c.cn), host);
}

// ---------------------------------------------------------------- chains
static bool subject_eq(const u8* a, u32 al, const u8* b, u32 bl) { return al == bl && !memcmp(a, b, al); }

bool x509_verify_chain(const u8* const* certs, const u32* lens, int count, const char* host, i64 now, char* err, int errlen, u8 fp[32]) {
    auto fail = [&](const char* m) { snprintf(err, (size_t)errlen, "%s", m); return false; };
    if (count < 1 || count > 10) return fail("the server sent no certificate");
    static Cert chain[10];
    for (int i = 0; i < count; i++) {
        if (!parse_cert(certs[i], lens[i], &chain[i])) { if (i == 0) return fail("the certificate can't be read"); count = i; break; }
    }
    sha256_x(certs[0], lens[0], fp);
    Cert& leaf = chain[0];
    if (now < leaf.not_before) return fail("the certificate isn't valid yet (is the clock right?)");
    if (now > leaf.not_after) return fail("the certificate has expired");
    if (host && host[0] && !host_matches(leaf, host)) return fail("the certificate is for a different name");
    int cur = 0;
    int used_intermediates = 0;
    for (int depth = 0; depth < 8; depth++) {
        Cert& c = chain[cur];
        if (c.sig_alg == SA_SHA1) return fail("the certificate is signed with SHA-1");
        // a root in the store?
        u32 off = 0;
        RootRef r;
        while (next_root(off, &r)) {
            if (!subject_eq(r.subject, r.slen, c.issuer, c.issuer_len)) continue;
            if (verify_cert_sig(c, r.key)) return true;
        }
        // is the certificate itself a stored root (the server sent it)?
        off = 0;
        while (next_root(off, &r)) {
            if (!subject_eq(r.subject, r.slen, c.subject, c.subject_len)) continue;
            if (r.key.kind == c.key.kind && ((r.key.kind == 1 && r.key.nlen == c.key.nlen && !memcmp(r.key.n, c.key.n, r.key.nlen)) ||
                                              (r.key.kind > 1 && r.key.ptlen == c.key.ptlen && !memcmp(r.key.pt, c.key.pt, r.key.ptlen)))) return true;
        }
        // an intermediate the server sent
        int next = -1;
        for (int i = 1; i < count; i++) {
            if (i == cur) continue;
            Cert& ic = chain[i];
            if (!subject_eq(ic.subject, ic.subject_len, c.issuer, c.issuer_len) || !ic.key.kind) continue;
            if (!verify_cert_sig(c, ic.key)) continue;
            if (now < ic.not_before || now > ic.not_after) continue;
            if (ic.has_bc && !ic.is_ca) continue;
            if (ic.path_len >= 0 && used_intermediates > ic.path_len) continue;
            next = i;
            break;
        }
        if (next < 0) {
            if (c.sig_alg == SA_NONE) return fail("the certificate uses a signature type silicon doesn't know");
            return fail("the certificate isn't signed by an authority silicon trusts");
        }
        used_intermediates++;
        cur = next;
    }
    return fail("the certificate chain is too long");
}
