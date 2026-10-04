// silicon - PDF: stream filters (LZW, ASCII85, ASCIIHex, RunLength) and predictors
#include "pdf_int.h"

static const u32 MAX_OUT = 192u * 1024 * 1024;

struct Buf {
    u8* d = nullptr;
    u32 n = 0, cap = 0;
    bool fail = false;
    void put(u8 c) {
        if (n >= cap) {
            u32 nc = cap ? cap * 2 : 4096;
            if (nc > MAX_OUT || nc < cap) { fail = true; return; }
            u8* nd = (u8*)kmalloc_try(nc);
            if (!nd) { fail = true; return; }
            if (d) { memcpy(nd, d, n); kfree(d); }
            d = nd; cap = nc;
        }
        d[n++] = c;
    }
    void put_n(const u8* s, u32 k) {
        for (u32 i = 0; i < k && !fail; i++) put(s[i]);
    }
};

u8* ahx_decode(const u8* in, u32 n, u32* outn) {
    Buf b;
    int hi = -1;
    for (u32 i = 0; i < n; i++) {
        u8 c = in[i];
        if (c == '>') break;
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { b.put((u8)(hi * 16 + v)); hi = -1; }
    }
    if (hi >= 0) b.put((u8)(hi * 16));
    if (b.fail) { kfree(b.d); return nullptr; }
    if (!b.d) b.put(0), b.n = 0;
    *outn = b.n;
    return b.d;
}

u8* a85_decode(const u8* in, u32 n, u32* outn) {
    Buf b;
    u32 grp[5];
    int k = 0;
    u32 i = 0;
    if (n >= 2 && in[0] == '<' && in[1] == '~') i = 2;
    for (; i < n; i++) {
        u8 c = in[i];
        if (c == '~') break;
        if (c == 'z' && k == 0) { b.put(0); b.put(0); b.put(0); b.put(0); continue; }
        if (c < '!' || c > 'u') continue;
        grp[k++] = c - '!';
        if (k == 5) {
            u32 v = 0;
            for (int j = 0; j < 5; j++) v = v * 85 + grp[j];
            b.put((u8)(v >> 24)); b.put((u8)(v >> 16)); b.put((u8)(v >> 8)); b.put((u8)v);
            k = 0;
        }
    }
    if (k > 1) {
        for (int j = k; j < 5; j++) grp[j] = 84;
        u32 v = 0;
        for (int j = 0; j < 5; j++) v = v * 85 + grp[j];
        for (int j = 0; j < k - 1; j++) b.put((u8)(v >> (24 - 8 * j)));
    }
    if (b.fail) { kfree(b.d); return nullptr; }
    if (!b.d) b.put(0), b.n = 0;
    *outn = b.n;
    return b.d;
}

u8* rl_decode(const u8* in, u32 n, u32* outn) {
    Buf b;
    u32 i = 0;
    while (i < n && !b.fail) {
        u8 l = in[i++];
        if (l == 128) break;
        if (l < 128) {
            u32 c = l + 1u;
            if (i + c > n) c = n - i;
            b.put_n(in + i, c);
            i += c;
        } else {
            if (i >= n) break;
            u8 v = in[i++];
            for (int k = 0; k < 257 - l && !b.fail; k++) b.put(v);
        }
    }
    if (b.fail) { kfree(b.d); return nullptr; }
    if (!b.d) b.put(0), b.n = 0;
    *outn = b.n;
    return b.d;
}

u8* lzw_decode(const u8* in, u32 n, int early, u32* outn) {
    struct Ent { u16 prev; u8 ch; u8 first; u32 len; };
    Ent* tab = (Ent*)kmalloc(sizeof(Ent) * 4097);
    for (int i = 0; i < 256; i++) { tab[i].prev = 0xFFFF; tab[i].ch = (u8)i; tab[i].first = (u8)i; tab[i].len = 1; }
    Buf b;
    int next = 258, bits = 9;
    u32 acc = 0;
    int nacc = 0;
    int prev = -1;
    u32 i = 0;
    u8* tmp = (u8*)kmalloc(4100);
    while (!b.fail) {
        while (nacc < bits && i < n) { acc = (acc << 8) | in[i++]; nacc += 8; }
        if (nacc < bits) break;
        int code = (acc >> (nacc - bits)) & ((1 << bits) - 1);
        nacc -= bits;
        if (code == 256) { next = 258; bits = 9; prev = -1; continue; }
        if (code == 257) break;
        u32 len;
        if (code < next && (code < 256 || code >= 258)) {
            len = tab[code].len;
            int c = code;
            for (u32 k = len; k > 0; k--) { tmp[k - 1] = tab[c].ch; c = tab[c].prev; }
        } else if (code == next && prev >= 0) {
            len = tab[prev].len + 1;
            int c = prev;
            for (u32 k = len - 1; k > 0; k--) { tmp[k - 1] = tab[c].ch; c = tab[c].prev; }
            tmp[len - 1] = tab[prev].first;
        } else break;
        b.put_n(tmp, len);
        if (prev >= 0 && next < 4096) {
            tab[next].prev = (u16)prev;
            tab[next].ch = tmp[0];
            tab[next].first = tab[prev].first;
            tab[next].len = tab[prev].len + 1;
            next++;
        }
        prev = code;
        int lim = next + early;
        bits = lim >= 2048 ? 12 : lim >= 1024 ? 11 : lim >= 512 ? 10 : 9;
    }
    kfree(tmp);
    kfree(tab);
    if (b.fail) { kfree(b.d); return nullptr; }
    if (!b.d) b.put(0), b.n = 0;
    *outn = b.n;
    return b.d;
}

u8* apply_predictor(u8* data, u32* len, int pred, int colors, int bpc, int columns) {
    if (pred <= 1) return data;
    if (colors < 1) colors = 1;
    if (bpc < 1) bpc = 8;
    if (columns < 1) columns = 1;
    u32 bpp = (u32)((colors * bpc + 7) / 8);
    u32 rowb = (u32)(((u64)columns * colors * bpc + 7) / 8);
    if (rowb == 0) return data;
    if (pred == 2) {                                   // TIFF
        u32 rows = *len / rowb;
        for (u32 r = 0; r < rows; r++) {
            u8* row = data + (size_t)r * rowb;
            if (bpc == 8) {
                for (u32 i = bpp; i < rowb; i++) row[i] = (u8)(row[i] + row[i - bpp]);
            } else if (bpc == 16) {
                for (u32 i = bpp; i + 1 < rowb; i += 2) {
                    u32 v = ((row[i] << 8) | row[i + 1]) + ((row[i - bpp] << 8) | row[i - bpp + 1]);
                    row[i] = (u8)(v >> 8); row[i + 1] = (u8)v;
                }
            } else {                                   // 1, 2, 4 bit samples
                int mask = (1 << bpc) - 1;
                int total = columns * colors;
                for (int i = colors; i < total; i++) {
                    int bi = i * bpc, bj = (i - colors) * bpc;
                    int a = (row[bi >> 3] >> (8 - bpc - (bi & 7))) & mask;
                    int p = (row[bj >> 3] >> (8 - bpc - (bj & 7))) & mask;
                    int v = (a + p) & mask;
                    int sh = 8 - bpc - (bi & 7);
                    row[bi >> 3] = (u8)((row[bi >> 3] & ~(mask << sh)) | (v << sh));
                }
            }
        }
        return data;
    }
    // PNG predictors: every row starts with a tag byte
    u32 stride = rowb + 1;
    u32 rows = (*len + stride - 1) / stride;
    u8* out = (u8*)kmalloc_try((size_t)rows * rowb + 1);
    if (!out) return data;
    u8* zero = (u8*)kcalloc(rowb + 1);
    u32 orow = 0;
    for (u32 r = 0; r < rows; r++) {
        u32 off = r * stride;
        if (off >= *len) break;
        u8 tag = data[off];
        const u8* src = data + off + 1;
        u32 avail = min(rowb, *len - off - 1);
        u8* cur = out + (size_t)r * rowb;
        const u8* up = r ? out + (size_t)(r - 1) * rowb : zero;
        memset(cur, 0, rowb);
        for (u32 i = 0; i < avail; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = up[i], c = i >= bpp ? up[i - bpp] : 0;
            int x = src[i], v;
            switch (tag) {
            case 1: v = x + a; break;
            case 2: v = x + b; break;
            case 3: v = x + ((a + b) >> 1); break;
            case 4: {
                int p = a + b - c, pa = iabs(p - a), pb = iabs(p - b), pc = iabs(p - c);
                v = x + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
                break;
            }
            default: v = x; break;
            }
            cur[i] = (u8)v;
        }
        orow = r + 1;
    }
    kfree(zero);
    kfree(data);
    *len = orow * rowb;
    return out;
}

// CCITT fax decoding is added in pdf_ccitt.cpp
