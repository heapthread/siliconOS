// silicon - TLS client: 1.3 (RFC 8446, TLS_AES_128_GCM_SHA256) and 1.2 (RFC 5246,
// ECDHE-RSA/ECDSA-AES128-GCM-SHA256), both with an X25519 key exchange.
// The server's certificate chain is checked against a built-in root store (net/x509.cpp, net/roots.inc),
// the host name must match, and the handshake signature (TLS 1.3 CertificateVerify, TLS 1.2 ServerKeyExchange)
// must be made by the certificate's key. A site whose certificate fails can still be reached by pinning
// that exact certificate with the `trust` command; `trust off` turns checking off altogether.
// Not checked: revocation, key usage, name constraints.
#include "net.h"
#include "crypto.h"
#include "x509.h"
#include "../fs/fs.h"
#include "../drv/drivers.h"

struct TlsConn {
    int  sock;
    char host[128];
    int  phase;                  // 0 send hello, 1 wait server hello, 2 server handshake, 3 open, 4 (1.2) wait server finished, -1 failed
    char err[200];
    u8   priv[32], pub[32];
    Sha256 transcript;
    u8   hs_secret[32], c_hs[32], s_hs[32], c_ap[32], s_ap[32];
    AesGcm rd, wr;
    u8   rd_iv[12], wr_iv[12];
    u64  rd_seq, wr_seq;
    bool rd_on, wr_on;
    u8*  in;  u32 in_len, in_cap;          // raw bytes from TCP
    u8*  hs;  u32 hs_len, hs_cap;          // handshake message reassembly
    u8*  app; u32 app_len, app_cap, app_rd;
    bool closed;
    u8*  leaf; u32 leaf_len;       // the server certificate (DER)
    bool checked;                 // the chain was verified (or pinned), so the handshake signature must be too
    // TLS 1.2
    bool v12;
    bool v12_group;               // TLS 1.2 key exchange: false x25519, true P-256
    u8   crand[32], srand[32], master[48];
    bool have_ske;
    u8   spub[65];               // the server's ephemeral key (x25519: 32 bytes, P-256: 65)
    u8   spub_len;
    u8   p256priv[32];
};

static void grow(u8*& b, u32& cap, u32 need) {
    if (need <= cap) return;
    u32 nc = max(cap * 2, need + 1024);
    b = (u8*)krealloc(b, nc);
    cap = nc;
}

static void tls_fail(TlsConn* t, const char* why) {
    if (t->phase != -1) klog("tls: %s\n", why);
    strlcpy(t->err, why, sizeof(t->err));
    t->phase = -1;
}

// ---------------------------------------------------------------- certificate checking
static char last_host[128];
static u8 last_fp[32];
static bool last_valid = false;

static void hex_of(const u8* d, int n, char* out) {
    static const char* H = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[i * 2] = H[d[i] >> 4]; out[i * 2 + 1] = H[d[i] & 15]; }
    out[n * 2] = 0;
}

bool tls_verify_enabled() {
    FsNode* f = fs_lookup("/home/.config/tls_verify");
    if (!f || f->dir) return true;
    return memcmp(fs_read_text(f), "off", 3) != 0;
}
void tls_verify_set(bool on) { fs_mkdirs("/home/.config"); fs_write_text("/home/.config/tls_verify", on ? "on\n" : "off\n"); }

static bool pinned(const char* host, const u8 fp[32]) {
    FsNode* f = fs_lookup("/home/.config/tls_trust");
    if (!f || f->dir) return false;
    char hex[65];
    hex_of(fp, 32, hex);
    const char* t = fs_read_text(f);
    while (*t) {
        const char* e = strchr(t, '\n');
        int l = e ? (int)(e - t) : (int)strlen(t);
        char line[220];
        memcpy(line, t, min(l, 219));
        line[min(l, 219)] = 0;
        char* sp = strchr(line, ' ');
        if (sp) { *sp = 0; if (!strcmp(line, host) && !strcmp(sp + 1, hex)) return true; }
        if (!e) break;
        t = e + 1;
    }
    return false;
}

// pin the certificate of the last connection to `host` that failed its check
bool tls_trust_add(const char* host, char* msg, int n) {
    if (!last_valid || strcmp(last_host, host)) { snprintf(msg, (size_t)n, "no refused certificate from %s to trust (try connecting first)", host); return false; }
    char hex[65], line[220];
    hex_of(last_fp, 32, hex);
    snprintf(line, sizeof(line), "%s %s\n", host, hex);
    fs_mkdirs("/home/.config");
    FsNode* f = fs_create_path("/home/.config/tls_trust", false);
    if (!f || !fs_append(f, line, (u32)strlen(line))) { snprintf(msg, (size_t)n, "can't save the list"); return false; }
    snprintf(msg, (size_t)n, "pinned the certificate %.16s... for %s", hex, host);
    return true;
}
bool tls_trust_remove(const char* host) {
    FsNode* f = fs_lookup("/home/.config/tls_trust");
    if (!f || f->dir) return false;
    const char* t = fs_read_text(f);
    char* out = (char*)kmalloc(strlen(t) + 2);
    int o = 0;
    bool removed = false;
    while (*t) {
        const char* e = strchr(t, '\n');
        int l = e ? (int)(e - t) : (int)strlen(t);
        int hl = (int)strlen(host);
        if (l > hl && !memcmp(t, host, hl) && t[hl] == ' ') removed = true;
        else { memcpy(out + o, t, l); o += l; out[o++] = '\n'; }
        if (!e) break;
        t = e + 1;
    }
    out[o] = 0;
    fs_write_text("/home/.config/tls_trust", out);
    kfree(out);
    return removed;
}

// the list of certificates from a Certificate message: u24 length + certificate (+ u16 extensions in TLS 1.3), repeated
static bool check_chain(TlsConn* t, const u8* list, u32 listlen, bool v13) {
    const u8* certs[8];
    u32 lens[8];
    int n = 0;
    u32 o = 0;
    while (o + 3 <= listlen && n < 8) {
        u32 cl = ((u32)list[o] << 16) | (list[o + 1] << 8) | list[o + 2];
        o += 3;
        if (o + cl > listlen) { tls_fail(t, "damaged certificate list"); return false; }
        certs[n] = list + o; lens[n] = cl; n++;
        o += cl;
        if (v13) { if (o + 2 > listlen) break; o += 2 + (((u32)list[o] << 8) | list[o + 1]); }
    }
    if (!n || !lens[0]) { tls_fail(t, "the server sent no certificate"); return false; }
    if (t->leaf) kfree(t->leaf);
    t->leaf = (u8*)kmalloc(lens[0]);
    memcpy(t->leaf, certs[0], lens[0]);
    t->leaf_len = lens[0];
    if (!tls_verify_enabled()) return true;
    char err[110];
    u8 fp[32];
    t->checked = true;
    if (x509_verify_chain(certs, lens, n, t->host, (i64)rtc_stamp(), err, sizeof(err), fp)) return true;
    if (pinned(t->host, fp)) return true;
    strlcpy(last_host, t->host, sizeof(last_host));
    memcpy(last_fp, fp, 32);
    last_valid = true;
    char m[200];
    snprintf(m, sizeof(m), "%s (to connect anyway: trust add %s)", err, t->host);
    tls_fail(t, m);
    return false;
}

static void set_keys(AesGcm& g, u8 iv[12], u64& seq, const u8 secret[32]) {
    u8 key[16];
    hkdf_expand_label(secret, "key", nullptr, 0, key, 16);
    hkdf_expand_label(secret, "iv", nullptr, 0, iv, 12);
    g.init(key);
    seq = 0;
}

// TLS 1.2 PRF: P_SHA256(secret, label || seed)
static void prf(const u8* secret, u32 slen, const char* label, const u8* seed, u32 seedlen, u8* out, u32 len) {
    u8 ls[160];
    u32 ll = (u32)strlen(label);
    memcpy(ls, label, ll);
    memcpy(ls + ll, seed, seedlen);
    u32 lsl = ll + seedlen;
    u8 a[32], buf[32 + 160], blk[32];
    hmac_sha256(secret, slen, ls, lsl, a);              // A(1)
    while (len) {
        memcpy(buf, a, 32);
        memcpy(buf + 32, ls, lsl);
        hmac_sha256(secret, slen, buf, 32 + lsl, blk);
        u32 n = min<u32>(len, 32);
        memcpy(out, blk, n);
        out += n;
        len -= n;
        hmac_sha256(secret, slen, a, 32, a);           // A(i+1)
    }
}

static void make_nonce(const u8 iv[12], u64 seq, u8 out[12]) {
    memcpy(out, iv, 12);
    for (int i = 0; i < 8; i++) out[4 + i] ^= (u8)(seq >> (56 - 8 * i));
}

// ---------------------------------------------------------------- records out
static bool send_record(TlsConn* t, u8 type, const u8* data, u32 len) {
    u8 rec[16384 + 256];
    if (!t->wr_on) {
        rec[0] = type; rec[1] = 3; rec[2] = 3;
        rec[3] = len >> 8; rec[4] = len & 0xFF;
        memcpy(rec + 5, data, len);
        return tcp_send(t->sock, rec, len + 5);
    }
    if (t->v12) {
        // 1.2 GCM: explicit nonce (our sequence number) || ciphertext || tag
        u64 seq = t->wr_seq++;
        u32 clen = 8 + len + 16;
        rec[0] = type; rec[1] = 3; rec[2] = 3;
        rec[3] = clen >> 8; rec[4] = clen & 0xFF;
        u8 nonce[12], aad[13];
        memcpy(nonce, t->wr_iv, 4);
        for (int i = 0; i < 8; i++) nonce[4 + i] = aad[i] = (u8)(seq >> (56 - 8 * i));
        memcpy(rec + 5, nonce + 4, 8);
        aad[8] = type; aad[9] = 3; aad[10] = 3; aad[11] = len >> 8; aad[12] = len & 0xFF;
        t->wr.seal(nonce, aad, 13, data, len, rec + 13, rec + 13 + len);
        return tcp_send(t->sock, rec, clen + 5);
    }
    // encrypted: inner plaintext = data || type, outer type application_data
    u32 inner = len + 1, clen = inner + 16;
    rec[0] = 23; rec[1] = 3; rec[2] = 3;
    rec[3] = clen >> 8; rec[4] = clen & 0xFF;
    u8 pt[16384 + 1];
    memcpy(pt, data, len);
    pt[len] = type;
    u8 nonce[12];
    make_nonce(t->wr_iv, t->wr_seq++, nonce);
    t->wr.seal(nonce, rec, 5, pt, inner, rec + 5, rec + 5 + inner);
    return tcp_send(t->sock, rec, clen + 5);
}

// ---------------------------------------------------------------- client hello
static void send_client_hello(TlsConn* t) {
    random_bytes(t->priv, 32);
    x25519_base(t->pub, t->priv);
    u8 m[600];
    u32 o = 4;                                     // handshake header filled at the end
    m[o++] = 3; m[o++] = 3;                        // legacy_version TLS 1.2
    random_bytes(t->crand, 32);
    memcpy(m + o, t->crand, 32); o += 32;          // random
    m[o++] = 32; random_bytes(m + o, 32); o += 32; // legacy session id (middlebox compat)
    // TLS_AES_128_GCM_SHA256, ECDHE-RSA-AES128-GCM-SHA256, ECDHE-ECDSA-AES128-GCM-SHA256
    m[o++] = 0; m[o++] = 6; m[o++] = 0x13; m[o++] = 0x01; m[o++] = 0xC0; m[o++] = 0x2F; m[o++] = 0xC0; m[o++] = 0x2B;
    m[o++] = 1; m[o++] = 0;                        // null compression
    u32 ext_len_at = o;
    o += 2;
    auto ext = [&](u16 type, const u8* data, u32 len) {
        m[o++] = type >> 8; m[o++] = type & 0xFF;
        m[o++] = len >> 8; m[o++] = len & 0xFF;
        memcpy(m + o, data, len); o += len;
    };
    // server_name
    {
        u8 d[140];
        u32 hl = (u32)strlen(t->host);
        d[0] = (hl + 3) >> 8; d[1] = (hl + 3) & 0xFF;
        d[2] = 0;
        d[3] = hl >> 8; d[4] = hl & 0xFF;
        memcpy(d + 5, t->host, hl);
        ext(0x0000, d, hl + 5);
    }
    // supported_groups: x25519 first; secp256r1 is listed too because servers with an ECDSA certificate
    // refuse a client that doesn't list the certificate's curve (TLS 1.2 key exchange still uses x25519)
    { u8 d[] = {0, 4, 0x00, 0x1d, 0x00, 0x17}; ext(0x000a, d, sizeof(d)); }
    { u8 d[] = {0, 16, 0x04, 0x03, 0x08, 0x04, 0x04, 0x01, 0x05, 0x03, 0x08, 0x05, 0x05, 0x01, 0x08, 0x06, 0x06, 0x01};
      ext(0x000d, d, sizeof(d)); }                                             // signature_algorithms
    { u8 d[] = {4, 0x03, 0x04, 0x03, 0x03}; ext(0x002b, d, sizeof(d)); }       // supported_versions: 1.3, 1.2
    { u8 d[] = {1, 0}; ext(0x000b, d, sizeof(d)); }                            // ec_point_formats: uncompressed
    { u8 d[] = {0}; ext(0xff01, d, sizeof(d)); }                               // renegotiation_info
    { u8 d[] = {1, 1}; ext(0x002d, d, sizeof(d)); }                            // psk_key_exchange_modes
    { u8 d[] = {0, 9, 8, 'h', 't', 't', 'p', '/', '1', '.', '1'}; ext(0x0010, d, sizeof(d)); }   // alpn
    {
        u8 d[38];
        d[0] = 0; d[1] = 36;
        d[2] = 0x00; d[3] = 0x1d;
        d[4] = 0; d[5] = 32;
        memcpy(d + 6, t->pub, 32);
        ext(0x0033, d, sizeof(d));                                             // key_share
    }
    u32 el = o - ext_len_at - 2;
    m[ext_len_at] = el >> 8; m[ext_len_at + 1] = el & 0xFF;
    u32 body = o - 4;
    m[0] = 1;
    m[1] = (u8)(body >> 16); m[2] = (u8)(body >> 8); m[3] = (u8)body;
    t->transcript.update(m, o);
    send_record(t, 22, m, o);
    t->phase = 1;
}

// ---------------------------------------------------------------- handshake messages in
static void handle_server_hello(TlsConn* t, const u8* m, u32 len) {
    // m: full message incl. 4-byte header
    if (len < 4 + 2 + 32 + 1) { tls_fail(t, "short server hello"); return; }
    static const u8 hrr[32] = {0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
                               0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};
    if (!memcmp(m + 6, hrr, 32)) { tls_fail(t, "server asked for a different key exchange"); return; }
    u32 o = 4 + 2 + 32;
    u8 sid = m[o++];
    o += sid;
    u16 suite = (m[o] << 8) | m[o + 1];
    o += 3;                                         // suite + compression
    memcpy(t->srand, m + 6, 32);
    u32 el = (m[o] << 8) | m[o + 1];
    o += 2;
    u32 end = min(o + el, len);
    const u8* key = nullptr;
    bool v13 = false;
    while (o + 4 <= end) {
        u16 type = (m[o] << 8) | m[o + 1], l = (m[o + 2] << 8) | m[o + 3];
        o += 4;
        if (type == 0x002b && l >= 2 && m[o] == 3 && m[o + 1] == 4) v13 = true;
        if (type == 0x0033 && l >= 36 && m[o] == 0 && m[o + 1] == 0x1d) key = m + o + 4;
        o += l;
    }
    if (!v13) {
        // TLS 1.2: the key exchange comes in ServerKeyExchange
        if (suite != 0xC02F && suite != 0xC02B) { tls_fail(t, "server picked an unsupported cipher"); return; }
        t->v12 = true;
        t->transcript.update(m, len);
        t->phase = 2;
        return;
    }
    if (suite != 0x1301) { tls_fail(t, "server picked an unsupported cipher"); return; }
    if (!key) { tls_fail(t, "no key share from server"); return; }
    t->transcript.update(m, len);
    u8 shared[32];
    x25519(shared, t->priv, key);
    // key schedule
    u8 early[32], derived[32], empty_hash[32], th[32];
    hkdf_extract(nullptr, 0, (const u8*)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 32, early);
    sha256("", 0, empty_hash);
    hkdf_expand_label(early, "derived", empty_hash, 32, derived, 32);
    hkdf_extract(derived, 32, shared, 32, t->hs_secret);
    t->transcript.final(th);
    hkdf_expand_label(t->hs_secret, "c hs traffic", th, 32, t->c_hs, 32);
    hkdf_expand_label(t->hs_secret, "s hs traffic", th, 32, t->s_hs, 32);
    set_keys(t->rd, t->rd_iv, t->rd_seq, t->s_hs);
    t->rd_on = true;
    t->phase = 2;
}

static void handle_encrypted_hs(TlsConn* t, const u8* m, u32 len) {
    u8 type = m[0];
    if (type == 20) {                                // server Finished
        u8 th[32], fk[32], expect[32];
        t->transcript.final(th);
        hkdf_expand_label(t->s_hs, "finished", nullptr, 0, fk, 32);
        hmac_sha256(fk, 32, th, 32, expect);
        if (len < 4 + 32 || memcmp(expect, m + 4, 32)) { tls_fail(t, "server finished check failed"); return; }
        t->transcript.update(m, len);
        u8 th2[32];
        t->transcript.final(th2);
        // application secrets
        u8 empty_hash[32], derived[32], master[32], zero[32] = {0};
        sha256("", 0, empty_hash);
        hkdf_expand_label(t->hs_secret, "derived", empty_hash, 32, derived, 32);
        hkdf_extract(derived, 32, zero, 32, master);
        hkdf_expand_label(master, "c ap traffic", th2, 32, t->c_ap, 32);
        hkdf_expand_label(master, "s ap traffic", th2, 32, t->s_ap, 32);
        // our Finished, encrypted with the client handshake key
        u8 ccs = 1;
        bool was = t->wr_on;
        t->wr_on = false;
        send_record(t, 20, &ccs, 1);               // compatibility change_cipher_spec
        t->wr_on = was;
        set_keys(t->wr, t->wr_iv, t->wr_seq, t->c_hs);
        t->wr_on = true;
        u8 cfk[32], fin[36];
        hkdf_expand_label(t->c_hs, "finished", nullptr, 0, cfk, 32);
        fin[0] = 20; fin[1] = 0; fin[2] = 0; fin[3] = 32;
        hmac_sha256(cfk, 32, th2, 32, fin + 4);
        send_record(t, 22, fin, 36);
        set_keys(t->wr, t->wr_iv, t->wr_seq, t->c_ap);
        set_keys(t->rd, t->rd_iv, t->rd_seq, t->s_ap);
        t->phase = 3;
        return;
    }
    if (type == 11) {                                // Certificate: request context, then the list
        if (len >= 9 && 4 + 1 + m[4] + 3 <= len) {
            u32 o = 5 + m[4];
            u32 ll = ((u32)m[o] << 16) | (m[o + 1] << 8) | m[o + 2];
            o += 3;
            if (o + ll <= len) { if (!check_chain(t, m + o, ll, true)) return; }
            else { tls_fail(t, "damaged certificate message"); return; }
        } else { tls_fail(t, "damaged certificate message"); return; }
    } else if (type == 15 && t->checked) {           // CertificateVerify: a signature over the transcript so far
        u8 th[32];
        t->transcript.final(th);
        if (len < 8) { tls_fail(t, "damaged certificate verify"); return; }
        u16 scheme = (u16)((m[4] << 8) | m[5]);
        u32 sl = ((u32)m[6] << 8) | m[7];
        PubKey pk;
        u8 content[64 + 34 + 32];
        memset(content, 0x20, 64);
        memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
        content[97] = 0;
        memcpy(content + 98, th, 32);
        if (8 + sl > len || !t->leaf || !x509_public_key(t->leaf, t->leaf_len, &pk) || !x509_verify_tls_sig(pk, scheme, content, sizeof(content), m + 8, sl)) {
            tls_fail(t, "the server couldn't prove it owns its certificate");
            return;
        }
    }
    // EncryptedExtensions, Certificate, CertificateVerify: part of the transcript
    t->transcript.update(m, len);
}

// TLS 1.2: Certificate, ServerKeyExchange, ServerHelloDone arrive in the clear
static void handle_v12_hs(TlsConn* t, const u8* m, u32 len) {
    u8 type = m[0];
    if (t->phase == 4) {                            // server Finished (already decrypted)
        if (type != 20) return;
        u8 th[32], expect[12];
        t->transcript.final(th);
        prf(t->master, 48, "server finished", th, 32, expect, 12);
        if (len < 16 || memcmp(expect, m + 4, 12)) { tls_fail(t, "server finished check failed"); return; }
        t->phase = 3;
        return;
    }
    if (type == 13) { tls_fail(t, "server wants a client certificate"); return; }
    t->transcript.update(m, len);
    if (type == 11) {                               // Certificate
        if (len < 7) { tls_fail(t, "damaged certificate message"); return; }
        u32 ll = ((u32)m[4] << 16) | (m[5] << 8) | m[6];
        if (7 + ll > len) { tls_fail(t, "damaged certificate message"); return; }
        check_chain(t, m + 7, ll, false);
        return;
    }
    if (type == 12) {                               // ServerKeyExchange: curve, key, then a signature over both randoms and these
        if (len < 8 || m[4] != 3) { tls_fail(t, "unsupported key exchange"); return; }
        u32 grp = ((u32)m[5] << 8) | m[6], pl = m[7];
        bool ok_curve = (grp == 0x001d && pl == 32) || (grp == 0x0017 && pl == 65 && len > 8 && m[8] == 4);
        if (!ok_curve || 8 + pl > len) { tls_fail(t, "server wants a curve silicon lacks"); return; }
        if (t->checked) {
            if (len < 8 + pl + 4) { tls_fail(t, "damaged key exchange"); return; }
            u16 scheme = (u16)((m[8 + pl] << 8) | m[9 + pl]);
            u32 sl = ((u32)m[10 + pl] << 8) | m[11 + pl];
            u8 signed_data[64 + 4 + 65];
            memcpy(signed_data, t->crand, 32);
            memcpy(signed_data + 32, t->srand, 32);
            memcpy(signed_data + 64, m + 4, 4 + pl);
            PubKey pk;
            if (12 + pl + sl > len || !t->leaf || !x509_public_key(t->leaf, t->leaf_len, &pk) || !x509_verify_tls_sig(pk, scheme, signed_data, 64 + 4 + pl, m + 12 + pl, sl)) {
                tls_fail(t, "the server couldn't prove it owns its certificate");
                return;
            }
        }
        t->v12_group = grp == 0x0017;
        memcpy(t->spub, m + 8, pl);
        t->spub_len = (u8)pl;
        t->have_ske = true;
        return;
    }
    if (type != 14) return;                         // ServerHelloDone: our turn
    if (!t->have_ske) { tls_fail(t, "server sent no key exchange (RSA key transport isn't supported)"); return; }
    u8 pms[32], seed[64], kb[40];
    u8 cke[4 + 66];
    u32 ckl;
    if (t->v12_group) {                                                // secp256r1
        bool nz = false;
        while (!nz) { random_bytes(t->p256priv, 32); t->p256priv[0] &= 0x7F; for (int i = 0; i < 32; i++) nz |= t->p256priv[i] != 0; }
        u8 pub[65];
        if (!p256_public(t->p256priv, pub) || !p256_shared(t->p256priv, t->spub, pms)) { tls_fail(t, "the server's key exchange value is invalid"); return; }
        cke[0] = 16; cke[1] = 0; cke[2] = 0; cke[3] = 66; cke[4] = 65;
        memcpy(cke + 5, pub, 65);
        ckl = 4 + 66;
    } else {
        x25519(pms, t->priv, t->spub);
        cke[0] = 16; cke[1] = 0; cke[2] = 0; cke[3] = 33; cke[4] = 32;
        memcpy(cke + 5, t->pub, 32);
        ckl = 4 + 33;
    }
    memcpy(seed, t->crand, 32);
    memcpy(seed + 32, t->srand, 32);
    prf(pms, 32, "master secret", seed, 64, t->master, 48);
    memcpy(seed, t->srand, 32);
    memcpy(seed + 32, t->crand, 32);
    prf(t->master, 48, "key expansion", seed, 64, kb, 40);
    // ClientKeyExchange
    t->transcript.update(cke, ckl);
    send_record(t, 22, cke, ckl);
    u8 ccs = 1;
    send_record(t, 20, &ccs, 1);
    t->wr.init(kb);
    t->rd.init(kb + 16);
    memcpy(t->wr_iv, kb + 32, 4);
    memcpy(t->rd_iv, kb + 36, 4);
    t->wr_seq = t->rd_seq = 0;
    t->wr_on = true;
    // Finished
    u8 th[32], fin[16];
    t->transcript.final(th);
    fin[0] = 20; fin[1] = 0; fin[2] = 0; fin[3] = 12;
    prf(t->master, 48, "client finished", th, 32, fin + 4, 12);
    t->transcript.update(fin, sizeof(fin));
    send_record(t, 22, fin, sizeof(fin));
    t->phase = 4;
}

static void process_handshake(TlsConn* t) {
    while (t->hs_len >= 4) {
        u32 l = ((u32)t->hs[1] << 16) | (t->hs[2] << 8) | t->hs[3];
        if (t->hs_len < 4 + l) return;
        u32 total = 4 + l;
        if (t->phase == 1) {
            if (t->hs[0] != 2) { tls_fail(t, "expected server hello"); return; }
            handle_server_hello(t, t->hs, total);
        } else if (t->v12 && (t->phase == 2 || t->phase == 4)) handle_v12_hs(t, t->hs, total);
        else if (t->phase == 2) handle_encrypted_hs(t, t->hs, total);
        // phase 3: post-handshake messages (session tickets) are ignored
        memmove(t->hs, t->hs + total, t->hs_len - total);
        t->hs_len -= total;
        if (t->phase < 0) return;
    }
}

static void deliver(TlsConn* t, u8 type, const u8* d, u32 len) {
    if (type == 22) {
        grow(t->hs, t->hs_cap, t->hs_len + len);
        memcpy(t->hs + t->hs_len, d, len);
        t->hs_len += len;
        process_handshake(t);
    } else if (type == 23) {
        if (t->app_rd == t->app_len) t->app_rd = t->app_len = 0;
        grow(t->app, t->app_cap, t->app_len + len);
        memcpy(t->app + t->app_len, d, len);
        t->app_len += len;
    } else if (type == 21) {
        if (len >= 2 && d[1] == 0) t->closed = true;            // close_notify
        else { char m[64]; snprintf(m, sizeof(m), "server sent alert %u", len >= 2 ? d[1] : 0); tls_fail(t, m); t->closed = true; }
    }
}

static void pump(TlsConn* t) {
    for (;;) {
        u32 avail = tcp_available(t->sock);
        if (!avail) break;
        grow(t->in, t->in_cap, t->in_len + avail);
        t->in_len += tcp_recv(t->sock, t->in + t->in_len, avail);
    }
    while (t->in_len >= 5 && t->phase >= 0) {
        u8 type = t->in[0];
        u32 len = (t->in[3] << 8) | t->in[4];
        if (len > 16384 + 2048) { tls_fail(t, "oversized record"); return; }
        if (t->in_len < 5 + len) break;
        u8* body = t->in + 5;
        if (type == 20) {
            // change_cipher_spec: ignored in 1.3, switches the read side on in 1.2
            if (t->v12 && t->phase == 4) t->rd_on = true;
        } else if (t->v12 && t->rd_on) {
            if (len < 24) { tls_fail(t, "short record"); return; }
            u32 n = len - 24;
            u8 nonce[12], aad[13];
            memcpy(nonce, t->rd_iv, 4);
            memcpy(nonce + 4, body, 8);
            u64 seq = t->rd_seq++;
            for (int i = 0; i < 8; i++) aad[i] = (u8)(seq >> (56 - 8 * i));
            aad[8] = type; aad[9] = 3; aad[10] = 3; aad[11] = n >> 8; aad[12] = n & 0xFF;
            u8* pt = (u8*)kmalloc(n + 1);
            if (!t->rd.open(nonce, aad, 13, body + 8, n, body + 8 + n, pt)) {
                kfree(pt);
                tls_fail(t, "record failed to decrypt");
                return;
            }
            deliver(t, type, pt, n);
            kfree(pt);
        } else if (t->rd_on && type == 23) {
            if (len < 17) { tls_fail(t, "short record"); return; }
            u8 nonce[12];
            make_nonce(t->rd_iv, t->rd_seq++, nonce);
            u8* pt = (u8*)kmalloc(len);
            if (!t->rd.open(nonce, t->in, 5, body, len - 16, body + len - 16, pt)) {
                kfree(pt);
                tls_fail(t, "record failed to decrypt");
                return;
            }
            u32 n = len - 16;
            while (n && pt[n - 1] == 0) n--;          // strip padding
            if (n) deliver(t, pt[n - 1], pt, n - 1);
            kfree(pt);
        } else deliver(t, type, body, len);
        memmove(t->in, t->in + 5 + len, t->in_len - 5 - len);
        t->in_len -= 5 + len;
    }
}

// ---------------------------------------------------------------- public api
TlsConn* tls_new(int sock, const char* host) {
    TlsConn* t = new TlsConn();
    t->sock = sock;
    strlcpy(t->host, host, sizeof(t->host));
    t->transcript.init();
    return t;
}

int tls_handshake(TlsConn* t) {
    if (t->phase == 0) send_client_hello(t);
    if (t->phase < 0) return -1;
    if (tcp_failed(t->sock)) { tls_fail(t, "connection dropped"); return -1; }
    pump(t);
    if (t->phase < 0) return -1;
    if (t->phase == 3) return 1;
    if (tcp_peer_closed(t->sock)) { tls_fail(t, "server hung up during the handshake"); return -1; }
    return 0;
}

bool tls_send(TlsConn* t, const void* data, u32 len) {
    if (t->phase != 3) return false;
    const u8* p = (const u8*)data;
    while (len) {
        u32 n = min<u32>(len, 16000);
        if (!send_record(t, 23, p, n)) return false;
        p += n;
        len -= n;
    }
    return true;
}

u32 tls_recv(TlsConn* t, void* buf, u32 max) {
    if (t->phase >= 0) pump(t);
    u32 n = min(max, t->app_len - t->app_rd);
    memcpy(buf, t->app + t->app_rd, n);
    t->app_rd += n;
    return n;
}

bool tls_closed(TlsConn* t) {
    if (t->app_rd < t->app_len) return false;
    return t->closed || t->phase < 0 || tcp_peer_closed(t->sock) || tcp_failed(t->sock);
}

const char* tls_version(TlsConn* t) { return t->v12 ? "tls 1.2" : "tls 1.3"; }
const char* tls_error(TlsConn* t) { return t->err[0] ? t->err : "unknown error"; }

void tls_free(TlsConn* t) {
    if (!t) return;
    if (t->in) kfree(t->in);
    if (t->hs) kfree(t->hs);
    if (t->app) kfree(t->app);
    if (t->leaf) kfree(t->leaf);
    delete t;
}
