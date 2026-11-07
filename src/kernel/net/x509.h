// silicon - X.509 certificate checking for TLS: DER parsing, RSA (PKCS#1 v1.5 and PSS) and ECDSA
// (P-256, P-384) signature checks, a built-in root store (net/roots.inc, made by tools/gen_roots.py
// from the Mozilla bundle) and host name matching.
#pragma once
#ifndef X509_HOST
#include "../core.h"
#endif

struct PubKey {
    int kind;                     // 0 unknown, 1 RSA, 2 ECDSA P-256, 3 ECDSA P-384
    const u8* n; u32 nlen;        // RSA modulus (no leading zero)
    u32 e;                        // RSA public exponent
    const u8* pt; u32 ptlen;      // EC point, uncompressed (04 X Y)
};

// Checks a certificate chain as sent by a server (leaf first). `now` is seconds since 2000-01-01 UTC.
// On success returns true. On failure returns false with a reason in err; fp gets the leaf's SHA-256
// fingerprint either way (when the leaf could be read).
bool x509_verify_chain(const u8* const* certs, const u32* lens, int count, const char* host, i64 now,
                       char* err, int errlen, u8 fp[32]);

// the public key of one certificate (pointers into der)
bool x509_public_key(const u8* der, u32 len, PubKey* out);

// verify a TLS signature (SignatureScheme 0x0401 rsa_pkcs1_sha256, 0x0804 rsa_pss_rsae_sha256, 0x0403 ecdsa_secp256r1_sha256 ...)
bool x509_verify_tls_sig(const PubKey& key, u16 scheme, const u8* msg, u32 mlen, const u8* sig, u32 slen);

void sha384_hash(const void* data, u32 len, u8 out[48]);

// P-256 key exchange (ECDHE for TLS 1.2 servers that prefer secp256r1 to x25519)
bool p256_public(const u8 priv[32], u8 pub[65]);                       // priv * G, uncompressed
bool p256_shared(const u8 priv[32], const u8 peer[65], u8 out[32]);    // x coordinate of priv * peer; false for a point not on the curve
