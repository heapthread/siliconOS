// silicon - crypto for TLS 1.3: SHA-256, HMAC, HKDF, X25519, AES-128-GCM, random bytes
#pragma once
#include "../core.h"

struct Sha256 {
    u32 h[8];
    u8  buf[64];
    u64 total;
    u32 n;
    void init();
    void update(const void* data, u32 len);
    void final(u8 out[32]);          // does not modify *this (works on a copy)
};
void sha256(const void* data, u32 len, u8 out[32]);
void hmac_sha256(const u8* key, u32 klen, const u8* msg, u32 mlen, u8 out[32]);
void hkdf_extract(const u8* salt, u32 slen, const u8* ikm, u32 ilen, u8 out[32]);
void hkdf_expand_label(const u8 secret[32], const char* label, const u8* ctx, u32 clen, u8* out, u32 len);

void x25519(u8 out[32], const u8 scalar[32], const u8 point[32]);
void x25519_base(u8 out[32], const u8 scalar[32]);

struct AesGcm {
    u32 rk[44];                       // AES-128 round keys
    u8  H[16];
    void init(const u8 key[16]);
    void encrypt_block(const u8 in[16], u8 out[16]) const;
    void seal(const u8 nonce[12], const u8* aad, u32 alen, const u8* pt, u32 len, u8* ct, u8 tag[16]) const;
    bool open(const u8 nonce[12], const u8* aad, u32 alen, const u8* ct, u32 len, const u8 tag[16], u8* pt) const;
};

void random_bytes(u8* out, u32 n);
