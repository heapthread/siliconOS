// silicon - image decoders for the web and the image viewer:
// PNG (with a small inflate), GIF (first frame), JPEG (baseline + progressive) and BMP.
#include "gfx.h"

static const int MAX_DIM = 4096;
static const u32 MAX_PIXELS = 6u * 1024 * 1024;

static u32* alloc_pixels(int w, int h) {
    if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM || (u32)w * (u32)h > MAX_PIXELS) return nullptr;
    return (u32*)kmalloc_try((size_t)w * h * 4);
}
static inline u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static inline u32 be16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }
static inline u32 le16(const u8* p) { return p[0] | ((u32)p[1] << 8); }
static inline u32 le32(const u8* p) { return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline u8 clamp8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

// ================================================================ inflate (RFC 1951)
struct Inflate {
    const u8* in; u32 inlen, inpos;
    u32 bitbuf; int bitcnt;
    u8* out; u32 outlen, outcap;
    bool err, full;
    int bits(int n) {
        u32 v = bitbuf;
        while (bitcnt < n) {
            if (inpos >= inlen) { err = true; return 0; }
            v |= (u32)in[inpos++] << bitcnt;
            bitcnt += 8;
        }
        bitbuf = v >> n;
        bitcnt -= n;
        return (int)(v & ((1u << n) - 1));
    }
};

struct Huff {
    i16 count[16];
    i16 symbol[320];
    u16 fast[512];          // 9-bit lookup: (len << 9) | symbol, 0 = slow path
};

static int huff_build(Huff& h, const u8* len, int n) {
    for (int i = 0; i < 16; i++) h.count[i] = 0;
    for (int s = 0; s < n; s++) h.count[len[s]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return -1;
    }
    i16 offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h.count[l];
    for (int s = 0; s < n; s++)
        if (len[s]) h.symbol[offs[len[s]]++] = (i16)s;
    // fast table: deflate codes are sent msb-first but the bitstream is lsb-first, so reverse
    for (int i = 0; i < 512; i++) h.fast[i] = 0;
    int code = 0, idx = 0;
    for (int l = 1; l <= 9; l++) {
        for (int k = 0; k < h.count[l]; k++) {
            int sym = h.symbol[idx++];
            int rev = 0;
            for (int b = 0; b < l; b++) if (code & (1 << b)) rev |= 1 << (l - 1 - b);
            for (int fill = rev; fill < 512; fill += 1 << l) h.fast[fill] = (u16)((l << 9) | sym);
            code++;
        }
        code <<= 1;
    }
    return left;
}

static int huff_decode(Inflate& s, const Huff& h) {
    // try the 9-bit table first when enough input is buffered
    while (s.bitcnt < 9 && s.inpos < s.inlen) { s.bitbuf |= (u32)s.in[s.inpos++] << s.bitcnt; s.bitcnt += 8; }
    if (s.bitcnt >= 9) {
        u16 e = h.fast[s.bitbuf & 511];
        if (e) {
            int l = e >> 9;
            s.bitbuf >>= l;
            s.bitcnt -= l;
            return e & 511;
        }
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= s.bits(1);
        if (s.err) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const u16 LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const u8 LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const u16 DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                              4097, 6145, 8193, 12289, 16385, 24577};
static const u8 DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static bool inflate_codes(Inflate& s, const Huff& lc, const Huff& dc) {
    for (;;) {
        int sym = huff_decode(s, lc);
        if (sym < 0 || s.err) return false;
        if (sym < 256) {
            if (s.outlen >= s.outcap) { s.full = true; return true; }
            s.out[s.outlen++] = (u8)sym;
        } else if (sym == 256) return true;
        else {
            sym -= 257;
            if (sym >= 29) return false;
            u32 len = LBASE[sym] + s.bits(LEXT[sym]);
            int ds = huff_decode(s, dc);
            if (ds < 0 || ds >= 30) return false;
            u32 dist = DBASE[ds] + s.bits(DEXT[ds]);
            if (s.err || dist > s.outlen) return false;
            if (s.outlen + len > s.outcap) { len = s.outcap - s.outlen; s.full = true; }
            u8* o = s.out + s.outlen;
            const u8* src = o - dist;
            for (u32 i = 0; i < len; i++) o[i] = src[i];
            s.outlen += len;
            if (s.full) return true;
        }
    }
}

static bool inflate(Inflate& s) {
    static Huff fixed_l, fixed_d;
    static bool fixed_ready = false;
    if (!fixed_ready) {
        u8 l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        huff_build(fixed_l, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        huff_build(fixed_d, l, 30);
        fixed_ready = true;
    }
    Huff* lc = (Huff*)kmalloc(sizeof(Huff) * 2);
    Huff* dc = lc + 1;
    bool ok = true;
    int last;
    do {
        last = s.bits(1);
        int type = s.bits(2);
        if (s.err) { ok = false; break; }
        if (type == 0) {
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.inpos + 4 > s.inlen) { ok = false; break; }
            u32 len = le16(s.in + s.inpos);
            s.inpos += 4;
            if (s.inpos + len > s.inlen) len = s.inlen - s.inpos;
            if (s.outlen + len > s.outcap) { len = s.outcap - s.outlen; s.full = true; }
            memcpy(s.out + s.outlen, s.in + s.inpos, len);
            s.outlen += len;
            s.inpos += len;
        } else if (type == 1) {
            if (!inflate_codes(s, fixed_l, fixed_d)) { ok = false; break; }
        } else if (type == 2) {
            static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            int nlen = s.bits(5) + 257, ndist = s.bits(5) + 1, ncode = s.bits(4) + 4;
            if (nlen > 286 || ndist > 30) { ok = false; break; }
            u8 lengths[320];
            memset(lengths, 0, sizeof(lengths));
            for (int i = 0; i < ncode; i++) lengths[order[i]] = (u8)s.bits(3);
            if (huff_build(*lc, lengths, 19) < 0) { ok = false; break; }
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = huff_decode(s, *lc);
                if (sym < 0) { ok = false; break; }
                if (sym < 16) lengths[idx++] = (u8)sym;
                else {
                    int len = 0, rep;
                    if (sym == 16) { if (!idx) { ok = false; break; } len = lengths[idx - 1]; rep = 3 + s.bits(2); }
                    else if (sym == 17) rep = 3 + s.bits(3);
                    else rep = 11 + s.bits(7);
                    if (idx + rep > nlen + ndist) { ok = false; break; }
                    while (rep--) lengths[idx++] = (u8)len;
                }
            }
            if (!ok) break;
            if (huff_build(*lc, lengths, nlen) < 0 || huff_build(*dc, lengths + nlen, ndist) < 0) { ok = false; break; }
            if (!inflate_codes(s, *lc, *dc)) { ok = false; break; }
        } else { ok = false; break; }
        if (s.full) break;
    } while (!last);
    kfree(lc);
    return ok || s.outlen > 0;
}

// ================================================================ PNG
static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static bool png_decode(const u8* d, u32 n, Image& img) {
    if (n < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return false;
    u32 w = 0, h = 0;
    int depth = 0, ct = 0, il = 0;
    u32 pal[256];
    for (int i = 0; i < 256; i++) pal[i] = 0xFF000000;
    bool trns = false;
    u16 tr = 0, tg = 0, tb = 0;
    // pass 1: header, palette and the total compressed size
    u32 idat_total = 0;
    for (u32 p = 8; p + 12 <= n;) {
        u32 len = be32(d + p);
        const u8* type = d + p + 4;
        const u8* c = d + p + 8;
        if (p + 12 + len > n) break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = be32(c); h = be32(c + 4); depth = c[8]; ct = c[9]; il = c[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            for (u32 i = 0; i < len / 3 && i < 256; i++) pal[i] = 0xFF000000 | ((u32)c[i * 3] << 16) | ((u32)c[i * 3 + 1] << 8) | c[i * 3 + 2];
        } else if (!memcmp(type, "tRNS", 4)) {
            trns = true;
            if (ct == 3) for (u32 i = 0; i < len && i < 256; i++) pal[i] = (pal[i] & 0xFFFFFF) | ((u32)c[i] << 24);
            else if (ct == 0 && len >= 2) tg = (u16)be16(c);
            else if (ct == 2 && len >= 6) { tr = (u16)be16(c); tg = (u16)be16(c + 2); tb = (u16)be16(c + 4); }
        } else if (!memcmp(type, "IDAT", 4)) idat_total += len;
        else if (!memcmp(type, "IEND", 4)) break;
        p += 12 + len;
    }
    int ch = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : ct == 6 ? 4 : 0;
    if (!ch || !w || !h || !idat_total || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)) return false;
    u32* px = alloc_pixels((int)w, (int)h);
    if (!px) return false;
    u8* z = (u8*)kmalloc_try(idat_total);
    if (!z) { kfree(px); return false; }
    u32 zl = 0;
    for (u32 p = 8; p + 12 <= n;) {
        u32 len = be32(d + p);
        if (p + 12 + len > n) break;
        if (!memcmp(d + p + 4, "IDAT", 4)) { memcpy(z + zl, d + p + 8, len); zl += len; }
        p += 12 + len;
    }
    static const int AX[7] = {0, 4, 0, 2, 0, 1, 0}, AY[7] = {0, 0, 4, 0, 2, 0, 1};
    static const int DX[7] = {8, 8, 4, 4, 2, 2, 1}, DY[7] = {8, 8, 8, 4, 4, 2, 2};
    int passes = il ? 7 : 1;
    int bitspp = ch * depth;
    int fbpp = max(1, bitspp / 8);
    u32 raw_size = 0;
    for (int ps = 0; ps < passes; ps++) {
        u32 pw = il ? (w - AX[ps] + DX[ps] - 1) / DX[ps] : w;
        u32 ph = il ? (h - AY[ps] + DY[ps] - 1) / DY[ps] : h;
        if (AX[ps] >= (int)w && il) pw = 0;
        if (AY[ps] >= (int)h && il) ph = 0;
        if (pw && ph) raw_size += ph * (1 + (pw * bitspp + 7) / 8);
    }
    u8* raw = (u8*)kcalloc_try(raw_size + 16);
    if (!raw) { kfree(z); kfree(px); return false; }
    Inflate s;
    memset(&s, 0, sizeof(s));
    s.in = z + 2;
    s.inlen = zl > 2 ? zl - 2 : 0;
    s.out = raw;
    s.outcap = raw_size;
    bool ok = zl > 2 && (z[0] & 15) == 8 && inflate(s);
    kfree(z);
    if (!ok) { kfree(raw); kfree(px); return false; }
    memset(px, 0, (size_t)w * h * 4);
    u8* rp = raw;
    u32 mask = (1u << (depth < 8 ? depth : 8)) - 1;
    for (int ps = 0; ps < passes; ps++) {
        u32 pw = il ? (w - AX[ps] + DX[ps] - 1) / DX[ps] : w;
        u32 ph = il ? (h - AY[ps] + DY[ps] - 1) / DY[ps] : h;
        if (il && (AX[ps] >= (int)w || AY[ps] >= (int)h)) continue;
        if (!pw || !ph) continue;
        u32 stride = (pw * bitspp + 7) / 8;
        u8* prev = nullptr;
        for (u32 y = 0; y < ph; y++) {
            u8 f = rp[0];
            u8* row = rp + 1;
            for (u32 i = 0; i < stride; i++) {
                int a = i >= (u32)fbpp ? row[i - fbpp] : 0;
                int b = prev ? prev[i] : 0;
                int c = prev && i >= (u32)fbpp ? prev[i - fbpp] : 0;
                int v = row[i];
                switch (f) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) >> 1; break;
                case 4: v += paeth(a, b, c); break;
                }
                row[i] = (u8)v;
            }
            u32 oy = il ? AY[ps] + y * DY[ps] : y;
            for (u32 x = 0; x < pw; x++) {
                u32 ox = il ? AX[ps] + x * DX[ps] : x;
                u32 r, g, b, a = 255;
                auto sample = [&](int k) -> u32 {   // channel k of pixel x, 8-bit
                    if (depth == 8) return row[x * ch + k];
                    if (depth == 16) return row[(x * ch + k) * 2];
                    u32 bit = (x * ch + k) * depth;
                    return (row[bit >> 3] >> (8 - depth - (bit & 7))) & mask;
                };
                auto sample16 = [&](int k) -> u32 {
                    if (depth == 16) return be16(row + (x * ch + k) * 2);
                    return sample(k);
                };
                switch (ct) {
                case 0: {
                    u32 v = sample(0);
                    if (trns && sample16(0) == tg) a = 0;
                    if (depth < 8) v = v * 255 / mask;
                    r = g = b = v;
                    break;
                }
                case 2:
                    r = sample(0); g = sample(1); b = sample(2);
                    if (trns && sample16(0) == tr && sample16(1) == tg && sample16(2) == tb) a = 0;
                    break;
                case 3: {
                    u32 c = pal[sample(0) & 255];
                    px[oy * w + ox] = c;
                    continue;
                }
                case 4: r = g = b = sample(0); a = sample(1); break;
                default: r = sample(0); g = sample(1); b = sample(2); a = sample(3); break;
                }
                px[oy * w + ox] = (a << 24) | (r << 16) | (g << 8) | b;
            }
            prev = row;
            rp += 1 + stride;
        }
    }
    kfree(raw);
    img.w = (int)w;
    img.h = (int)h;
    img.px = px;
    return true;
}

// ================================================================ GIF (first frame)
static bool gif_decode(const u8* d, u32 n, Image& img) {
    if (n < 13 || memcmp(d, "GIF8", 4)) return false;
    int W = (int)le16(d + 6), H = (int)le16(d + 8);
    u8 flags = d[10];
    u32 gct[256], lct[256];
    u32 p = 13;
    if (flags & 0x80) {
        int sz = 2 << (flags & 7);
        if (p + sz * 3 > n) return false;
        for (int i = 0; i < sz; i++) gct[i] = 0xFF000000 | ((u32)d[p + i * 3] << 16) | ((u32)d[p + i * 3 + 1] << 8) | d[p + i * 3 + 2];
        p += sz * 3;
    } else for (int i = 0; i < 256; i++) gct[i] = 0xFF000000 | (i * 0x010101);
    u32* px = alloc_pixels(W, H);
    if (!px) return false;
    memset(px, 0, (size_t)W * H * 4);
    int transp = -1;
    bool done = false;
    while (p < n && !done) {
        u8 b = d[p++];
        if (b == 0x21) {
            if (p >= n) break;
            u8 label = d[p++];
            if (label == 0xF9 && p + 5 < n && d[p] >= 4) { if (d[p + 1] & 1) transp = d[p + 4]; }
            while (p < n && d[p]) p += d[p] + 1;
            p++;
        } else if (b == 0x2C) {
            if (p + 9 > n) break;
            int ix = (int)le16(d + p), iy = (int)le16(d + p + 2), iw = (int)le16(d + p + 4), ih = (int)le16(d + p + 6);
            u8 f = d[p + 8];
            p += 9;
            u32* tbl = gct;
            if (f & 0x80) {
                int sz = 2 << (f & 7);
                if (p + sz * 3 > n) break;
                for (int i = 0; i < sz; i++) lct[i] = 0xFF000000 | ((u32)d[p + i * 3] << 16) | ((u32)d[p + i * 3 + 1] << 8) | d[p + i * 3 + 2];
                p += sz * 3;
                tbl = lct;
            }
            bool interlace = f & 0x40;
            if (p >= n) break;
            int minc = d[p++];
            if (minc < 2 || minc > 11) break;
            // gather the sub-blocks
            u32 total = 0;
            for (u32 q = p; q < n && d[q]; q += d[q] + 1) total += d[q];
            u8* data = (u8*)kmalloc_try(total + 4);
            if (!data) break;
            u32 dl = 0;
            while (p < n && d[p]) {
                u32 l = d[p];
                if (p + 1 + l > n) l = n - p - 1;
                memcpy(data + dl, d + p + 1, l);
                dl += l;
                p += d[p] + 1;
            }
            p++;
            // rows in output order
            int* rows = (int*)kmalloc(sizeof(int) * max(ih, 1));
            if (interlace) {
                int k = 0;
                static const int st[4] = {0, 4, 2, 1}, sp[4] = {8, 8, 4, 2};
                for (int ps = 0; ps < 4; ps++) for (int y = st[ps]; y < ih; y += sp[ps]) rows[k++] = y;
            } else for (int y = 0; y < ih; y++) rows[y] = y;
            // LZW
            u16* prefix = (u16*)kmalloc(4096 * 2);
            u8* suffix = (u8*)kmalloc(4096);
            u8* stack = (u8*)kmalloc(4097);
            int clear = 1 << minc, eoi = clear + 1;
            int csize = minc + 1, next = clear + 2, old = -1, first = 0;
            for (int i = 0; i < clear; i++) { suffix[i] = (u8)i; prefix[i] = 0; }
            u32 bitpos = 0;
            u32 outp = 0, total_px = (u32)iw * ih;
            auto emit = [&](u8 idx) {
                if (outp >= total_px) return;
                int x = (int)(outp % iw), y = rows[outp / iw];
                outp++;
                int X = ix + x, Y = iy + y;
                if (X < W && Y < H && idx != transp) px[Y * W + X] = tbl[idx];
            };
            while (bitpos + csize <= dl * 8 && outp < total_px) {
                u32 code = 0;
                for (int k = 0; k < csize; k++, bitpos++) if (data[bitpos >> 3] & (1 << (bitpos & 7))) code |= 1u << k;
                if ((int)code == clear) { csize = minc + 1; next = clear + 2; old = -1; continue; }
                if ((int)code == eoi) break;
                if (old < 0) {
                    if ((int)code >= clear) break;
                    emit((u8)code);
                    old = first = (int)code;
                    continue;
                }
                int in = (int)code, sp = 0;
                if ((int)code > next) break;
                if ((int)code == next) { stack[sp++] = (u8)first; code = (u32)old; }
                while ((int)code >= clear && sp < 4096) { stack[sp++] = suffix[code]; code = prefix[code]; }
                first = (int)code;
                stack[sp++] = (u8)first;
                while (sp) emit(stack[--sp]);
                if (next < 4096) {
                    prefix[next] = (u16)old;
                    suffix[next] = (u8)first;
                    next++;
                    if (next == (1 << csize) && csize < 12) csize++;
                }
                old = in;
            }
            kfree(prefix); kfree(suffix); kfree(stack); kfree(rows); kfree(data);
            done = true;
        } else break;
    }
    if (!done) { kfree(px); return false; }
    img.w = W;
    img.h = H;
    img.px = px;
    return true;
}

// ================================================================ JPEG (baseline + progressive, huffman)
static const u8 ZZ[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                          41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                          30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct JHuff {
    u8 vals[256];
    int mincode[17], maxcode[18], valptr[17];
    u16 fast[512];          // (len << 8) | value, 0 = slow
    bool ok;
};
struct JComp {
    int id, hs, vs, tq, td, ta;
    int bw, bh;
    i16* coef;
    int pred;
    u8* plane;
};

static int idct_tab[8][8];  // [x][u] = 4096 * C(u)/2 * cos((2x+1)u pi/16)
static void idct_init() {
    static bool ready = false;
    if (ready) return;
    static const int COS[9] = {4096, 4017, 3784, 3406, 2896, 2276, 1567, 799, 0};   // cos(k pi/16) * 4096
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++) {
            int m = ((2 * x + 1) * u) % 32;
            if (m > 16) m = 32 - m;
            int c = m <= 8 ? COS[m] : -COS[16 - m];
            int v = u == 0 ? c * 2896 / 4096 : c;       // C(0) = 1/sqrt2
            idct_tab[x][u] = v / 2;
        }
    ready = true;
}

static void idct8x8(const int* in, u8* out, int stride) {
    int tmp[64];
    for (int v = 0; v < 8; v++) {           // rows: transform along u
        const int* r = in + v * 8;
        bool zero = !(r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7]);
        for (int x = 0; x < 8; x++) {
            int s;
            if (zero) s = r[0] * idct_tab[x][0];
            else { s = 0; for (int u = 0; u < 8; u++) s += r[u] * idct_tab[x][u]; }
            tmp[v * 8 + x] = (s + 2048) >> 12;
        }
    }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int s = 0;
            for (int v = 0; v < 8; v++) s += tmp[v * 8 + x] * idct_tab[y][v];
            out[y * stride + x] = clamp8(((s + 2048) >> 12) + 128);
        }
}

struct Jpeg {
    const u8* d; u32 n, pos;
    u32 bbuf; int bcnt; bool marker;
    u16 q[4][64];
    JHuff dc[4], ac[4];
    JComp c[4];
    int nc, W, H, hmax, vmax, mcux, mcuy;
    bool progressive, have_frame;
    int rst_int, eobrun;
    int transform;
    // current scan
    int ns, sc[4], ss, se, ah, al;

    void fill() {
        while (bcnt <= 24) {
            u32 b = 0;
            if (!marker && pos < n) {
                b = d[pos];
                if (b == 0xFF) {
                    u8 nx = pos + 1 < n ? d[pos + 1] : 0;
                    if (nx == 0) pos += 2;
                    else { marker = true; b = 0; }
                } else pos++;
            }
            bbuf |= b << (24 - bcnt);
            bcnt += 8;
        }
    }
    int bit() {
        if (bcnt < 1) fill();
        int v = (int)(bbuf >> 31);
        bbuf <<= 1;
        bcnt--;
        return v;
    }
    int getbits(int k) {
        if (!k) return 0;
        if (bcnt < k) fill();
        int v = (int)(bbuf >> (32 - k));
        bbuf <<= k;
        bcnt -= k;
        return v;
    }
    static int extend(int v, int s) { return s && v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }
    int decode(JHuff& h) {
        if (bcnt < 16) fill();
        u16 e = h.fast[bbuf >> 23];
        if (e) {
            int l = e >> 8;
            bbuf <<= l;
            bcnt -= l;
            return e & 255;
        }
        int code = 0;
        for (int l = 1; l <= 16; l++) {
            code = (code << 1) | bit();
            if (code <= h.maxcode[l]) return h.vals[h.valptr[l] + code - h.mincode[l]];
        }
        return -1;
    }

    bool sof(const u8* s, u32 len) {
        if (len < 6 || s[0] != 8 || have_frame) return false;
        H = (int)be16(s + 1);
        W = (int)be16(s + 3);
        nc = s[5];
        if (nc != 1 && nc != 3 && nc != 4) return false;
        if (len < 6 + 3u * nc || !W || !H || W > MAX_DIM || H > MAX_DIM || (u32)W * H > MAX_PIXELS) return false;
        hmax = vmax = 1;
        for (int i = 0; i < nc; i++) {
            JComp& k = c[i];
            k.id = s[6 + i * 3];
            k.hs = s[7 + i * 3] >> 4;
            k.vs = s[7 + i * 3] & 15;
            k.tq = s[8 + i * 3] & 3;
            if (k.hs < 1 || k.hs > 4 || k.vs < 1 || k.vs > 4) return false;
            hmax = max(hmax, k.hs);
            vmax = max(vmax, k.vs);
        }
        mcux = (W + 8 * hmax - 1) / (8 * hmax);
        mcuy = (H + 8 * vmax - 1) / (8 * vmax);
        for (int i = 0; i < nc; i++) {
            JComp& k = c[i];
            k.bw = mcux * k.hs;
            k.bh = mcuy * k.vs;
            k.coef = (i16*)kcalloc_try((size_t)k.bw * k.bh * 64 * 2);
            if (!k.coef) return false;
        }
        have_frame = true;
        return true;
    }
    bool dht(const u8* s, u32 len) {
        u32 p = 0;
        while (p + 17 <= len) {
            int tc = s[p] >> 4, th = s[p] & 3;
            JHuff& h = tc ? ac[th] : dc[th];
            int counts[16], total = 0;
            for (int i = 0; i < 16; i++) { counts[i] = s[p + 1 + i]; total += counts[i]; }
            p += 17;
            if (total > 256 || p + total > len) return false;
            memcpy(h.vals, s + p, total);
            p += total;
            int code = 0, k = 0;
            for (int i = 0; i < 512; i++) h.fast[i] = 0;
            for (int l = 1; l <= 16; l++) {
                h.valptr[l] = k;
                h.mincode[l] = code;
                for (int j = 0; j < counts[l - 1]; j++, k++, code++)
                    if (l <= 9)
                        for (int f = code << (9 - l); f < (code + 1) << (9 - l); f++) h.fast[f] = (u16)((l << 8) | h.vals[k]);
                h.maxcode[l] = counts[l - 1] ? code - 1 : -1;
                code <<= 1;
            }
            h.maxcode[17] = 0x7FFFFFFF;
            h.ok = true;
        }
        return true;
    }
    bool dqt(const u8* s, u32 len) {
        u32 p = 0;
        while (p < len) {
            int pq = s[p] >> 4, tq = s[p] & 3;
            p++;
            for (int i = 0; i < 64; i++) {
                if (p + (pq ? 2 : 1) > len) return false;
                q[tq][ZZ[i]] = pq ? (u16)be16(s + p) : s[p];
                p += pq ? 2 : 1;
            }
        }
        return true;
    }

    bool block(JComp& k, int bx, int by) {
        if (bx >= k.bw || by >= k.bh) return true;
        i16* co = k.coef + ((size_t)by * k.bw + bx) * 64;
        if (!progressive) {
            int t = decode(dc[k.td]);
            if (t < 0) return false;
            int diff = t ? extend(getbits(t), t) : 0;
            k.pred += diff;
            co[0] = (i16)k.pred;
            for (int i = 1; i < 64;) {
                int rs = decode(ac[k.ta]);
                if (rs < 0) return false;
                int r = rs >> 4, s = rs & 15;
                if (!s) {
                    if (r != 15) break;
                    i += 16;
                    continue;
                }
                i += r;
                if (i > 63) break;
                co[ZZ[i]] = (i16)extend(getbits(s), s);
                i++;
            }
            return true;
        }
        if (ss == 0) {                              // DC scans
            if (ah == 0) {
                int t = decode(dc[k.td]);
                if (t < 0) return false;
                int diff = t ? extend(getbits(t), t) : 0;
                k.pred += diff;
                co[0] = (i16)(k.pred * (1 << al));
            } else if (bit()) co[0] |= (i16)(1 << al);
            return true;
        }
        if (ah == 0) {                              // AC first pass
            if (eobrun) { eobrun--; return true; }
            for (int i = ss; i <= se;) {
                int rs = decode(ac[k.ta]);
                if (rs < 0) return false;
                int r = rs >> 4, s = rs & 15;
                if (!s) {
                    if (r < 15) {
                        eobrun = (1 << r) - 1;
                        if (r) eobrun += getbits(r);
                        break;
                    }
                    i += 16;
                    continue;
                }
                i += r;
                if (i > 63) break;
                co[ZZ[i]] = (i16)(extend(getbits(s), s) * (1 << al));
                i++;
            }
            return true;
        }
        // AC refinement
        i16 p1 = (i16)(1 << al), m1 = (i16)(-1 * (1 << al));
        int i = ss;
        if (eobrun) {
            eobrun--;
            for (; i <= se; i++) {
                i16* pc = &co[ZZ[i]];
                if (*pc && bit() && !(*pc & p1)) *pc += *pc > 0 ? p1 : m1;
            }
            return true;
        }
        while (i <= se) {
            int rs = decode(ac[k.ta]);
            if (rs < 0) return false;
            int r = rs >> 4, s = rs & 15;
            i16 val = 0;
            if (!s) {
                if (r < 15) {
                    eobrun = (1 << r) - 1;
                    if (r) eobrun += getbits(r);
                    r = 64;
                }
            } else val = bit() ? p1 : m1;
            while (i <= se) {
                i16* pc = &co[ZZ[i++]];
                if (*pc) { if (bit() && !(*pc & p1)) *pc += *pc > 0 ? p1 : m1; }
                else {
                    if (!r) { *pc = val; break; }
                    r--;
                }
            }
        }
        return true;
    }

    void restart() {
        bbuf = 0;
        bcnt = 0;
        marker = false;
        while (pos + 1 < n && !(d[pos] == 0xFF && d[pos + 1] >= 0xD0 && d[pos + 1] <= 0xD7)) pos++;
        if (pos + 1 < n) pos += 2;
        for (int i = 0; i < nc; i++) c[i].pred = 0;
        eobrun = 0;
    }

    bool scan() {
        bbuf = 0;
        bcnt = 0;
        marker = false;
        eobrun = 0;
        for (int i = 0; i < nc; i++) c[i].pred = 0;
        int count = 0;
        if (ns == 1) {
            JComp& k = c[sc[0]];
            int bw = ((W * k.hs + hmax - 1) / hmax + 7) / 8;
            int bh = ((H * k.vs + vmax - 1) / vmax + 7) / 8;
            for (int by = 0; by < bh; by++)
                for (int bx = 0; bx < bw; bx++) {
                    if (!block(k, bx, by)) return false;
                    if (rst_int && ++count % rst_int == 0 && !(by == bh - 1 && bx == bw - 1)) restart();
                }
        } else {
            for (int my = 0; my < mcuy; my++)
                for (int mx = 0; mx < mcux; mx++) {
                    for (int j = 0; j < ns; j++) {
                        JComp& k = c[sc[j]];
                        for (int v = 0; v < k.vs; v++)
                            for (int h = 0; h < k.hs; h++)
                                if (!block(k, mx * k.hs + h, my * k.vs + v)) return false;
                    }
                    if (rst_int && ++count % rst_int == 0 && !(my == mcuy - 1 && mx == mcux - 1)) restart();
                }
        }
        return true;
    }

    bool sos(const u8* s, u32 len) {
        if (!have_frame || len < 1) return false;
        ns = s[0];
        if (ns < 1 || ns > 4 || len < 4 + 2u * ns) return false;
        for (int i = 0; i < ns; i++) {
            int id = s[1 + i * 2];
            int idx = -1;
            for (int j = 0; j < nc; j++) if (c[j].id == id) idx = j;
            if (idx < 0) return false;
            sc[i] = idx;
            c[idx].td = s[2 + i * 2] >> 4 & 3;
            c[idx].ta = s[2 + i * 2] & 3;
        }
        ss = s[1 + ns * 2];
        se = s[2 + ns * 2];
        ah = s[3 + ns * 2] >> 4;
        al = s[3 + ns * 2] & 15;
        if (!progressive) { ss = 0; se = 63; ah = al = 0; }
        if (se > 63) se = 63;
        return true;
    }

    bool run(Image& img) {
        if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
        idct_init();
        pos = 2;
        transform = -1;
        bool any_scan = false;
        for (;;) {
            while (pos < n && d[pos] != 0xFF) pos++;
            while (pos < n && d[pos] == 0xFF) pos++;
            if (pos >= n) break;
            u8 m = d[pos++];
            if (m == 0xD9) break;
            if (m == 0x00 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
            if (pos + 2 > n) break;
            u32 L = be16(d + pos);
            if (L < 2 || pos + L > n) break;
            const u8* seg = d + pos + 2;
            u32 sl = L - 2;
            u32 next = pos + L;
            bool ok = true;
            switch (m) {
            case 0xC0: case 0xC1: progressive = false; ok = sof(seg, sl); break;
            case 0xC2: progressive = true; ok = sof(seg, sl); break;
            case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
                return false;       // lossless / arithmetic coding
            case 0xC4: ok = dht(seg, sl); break;
            case 0xDB: ok = dqt(seg, sl); break;
            case 0xDD: if (sl >= 2) rst_int = (int)be16(seg); break;
            case 0xEE: if (sl >= 12 && !memcmp(seg, "Adobe", 5)) transform = seg[11]; break;
            case 0xDA:
                if (!sos(seg, sl)) return finish(img, any_scan);
                pos = next;
                if (!scan()) return finish(img, true);      // keep what decoded so far
                any_scan = true;
                continue;
            }
            if (!ok) return false;
            pos = next;
        }
        return finish(img, any_scan);
    }

    bool finish(Image& img, bool any) {
        if (!have_frame || !any) { free_all(); return false; }
        int tmp[64];
        for (int i = 0; i < nc; i++) {
            JComp& k = c[i];
            int pw = k.bw * 8;
            k.plane = (u8*)kmalloc_try((size_t)pw * k.bh * 8);
            if (!k.plane) { free_all(); return false; }
            const u16* qt = q[k.tq];
            for (int by = 0; by < k.bh; by++)
                for (int bx = 0; bx < k.bw; bx++) {
                    const i16* co = k.coef + ((size_t)by * k.bw + bx) * 64;
                    for (int j = 0; j < 64; j++) tmp[j] = co[j] * qt[j];
                    idct8x8(tmp, k.plane + (size_t)by * 8 * pw + bx * 8, pw);
                }
            kfree(k.coef);
            k.coef = nullptr;
        }
        u32* px = alloc_pixels(W, H);
        if (!px) { free_all(); return false; }
        for (int y = 0; y < H; y++) {
            u32* o = px + (size_t)y * W;
            const u8* rows[4];
            int sx[4];
            for (int i = 0; i < nc; i++) {
                int pw = c[i].bw * 8;
                rows[i] = c[i].plane + (size_t)(y * c[i].vs / vmax) * pw;
                sx[i] = c[i].hs;
            }
            for (int x = 0; x < W; x++) {
                if (nc == 1) { u32 v = rows[0][x]; o[x] = 0xFF000000 | v * 0x010101; continue; }
                int Y = rows[0][x * sx[0] / hmax], cb = rows[1][x * sx[1] / hmax] - 128, cr = rows[2][x * sx[2] / hmax] - 128;
                int r, g, b;
                if (nc == 3 && transform == 0) { r = Y; g = cb + 128; b = cr + 128; }
                else {
                    r = Y + ((91881 * cr + 32768) >> 16);
                    g = Y - ((22554 * cb + 46802 * cr - 32768) >> 16);
                    b = Y + ((116130 * cb + 32768) >> 16);
                }
                if (nc == 4) {          // adobe CMYK / YCCK, stored inverted
                    int kk = rows[3][x * sx[3] / hmax];
                    if (transform == 0) { r = Y; g = cb + 128; b = cr + 128; }
                    r = clamp8(r) * kk / 255; g = clamp8(g) * kk / 255; b = clamp8(b) * kk / 255;
                }
                o[x] = 0xFF000000 | ((u32)clamp8(r) << 16) | ((u32)clamp8(g) << 8) | clamp8(b);
            }
        }
        free_all();
        img.w = W;
        img.h = H;
        img.px = px;
        return true;
    }
    void free_all() {
        for (int i = 0; i < nc && i < 4; i++) {
            if (c[i].coef) kfree(c[i].coef);
            if (c[i].plane) kfree(c[i].plane);
            c[i].coef = nullptr;
            c[i].plane = nullptr;
        }
    }
};

static bool jpeg_decode(const u8* d, u32 n, Image& img) {
    Jpeg* j = (Jpeg*)kcalloc(sizeof(Jpeg));
    j->d = d;
    j->n = n;
    bool ok = j->run(img);
    j->free_all();                  // whatever a failed decode left behind
    kfree(j);
    return ok;
}

// ================================================================ BMP
static bool bmp_decode(const u8* d, u32 n, Image& img) {
    if (n < 54 || d[0] != 'B' || d[1] != 'M') return false;
    u32 off = le32(d + 10), hs = le32(d + 14);
    int w = (int)le32(d + 18), h = (int)le32(d + 22);
    int bpp = (int)le16(d + 28), comp = (int)le32(d + 30);
    bool flip = h > 0;
    if (h < 0) h = -h;
    if ((comp != 0 && comp != 3) || (bpp != 8 && bpp != 24 && bpp != 32)) return false;
    u32* px = alloc_pixels(w, h);
    if (!px) return false;
    u32 pal[256];
    if (bpp == 8)
        for (int i = 0; i < 256; i++) {
            u32 p = 14 + hs + i * 4;
            pal[i] = p + 3 < n ? 0xFF000000 | ((u32)d[p + 2] << 16) | ((u32)d[p + 1] << 8) | d[p] : 0xFF000000;
        }
    u32 stride = ((u32)w * bpp / 8 + 3) & ~3u;
    for (int y = 0; y < h; y++) {
        u32 ro = off + (u32)(flip ? h - 1 - y : y) * stride;
        for (int x = 0; x < w; x++) {
            u32 p = ro + (u32)x * bpp / 8;
            u32 c = 0xFF000000;
            if (p + bpp / 8 <= n) {
                if (bpp == 8) c = pal[d[p]];
                else c = 0xFF000000 | ((u32)d[p + 2] << 16) | ((u32)d[p + 1] << 8) | d[p];
            }
            px[y * w + x] = c;
        }
    }
    img.w = w;
    img.h = h;
    img.px = px;
    return true;
}

// ================================================================ api
const char* image_format(const u8* d, u32 n) {
    if (n >= 8 && !memcmp(d, "\x89PNG", 4)) return "png";
    if (n >= 6 && !memcmp(d, "GIF8", 4)) return "gif";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "jpeg";
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return "bmp";
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return "webp";
    for (u32 i = 0; i < n && i < 256; i++) if (!memcmp(d + i, "<svg", 4)) return "svg";
    return nullptr;
}

bool image_load(const u8* d, u32 n, Image& img) {
    img.w = img.h = 0;
    img.px = nullptr;
    const char* f = image_format(d, n);
    if (!f) return false;
    if (!strcmp(f, "png")) return png_decode(d, n, img);
    if (!strcmp(f, "gif")) return gif_decode(d, n, img);
    if (!strcmp(f, "jpeg")) return jpeg_decode(d, n, img);
    if (!strcmp(f, "bmp")) return bmp_decode(d, n, img);
    return false;
}

// box-filter shrink so the image fits maxw x maxh (keeps the aspect ratio)
void image_shrink(Image& img, int maxw, int maxh) {
    if (!img.px || (img.w <= maxw && img.h <= maxh)) return;
    int nw = img.w, nh = img.h;
    if (nw > maxw) { nh = (int)((i64)nh * maxw / nw); nw = maxw; }
    if (nh > maxh) { nw = (int)((i64)nw * maxh / nh); nh = maxh; }
    nw = max(nw, 1);
    nh = max(nh, 1);
    u32* out = (u32*)kmalloc_try((size_t)nw * nh * 4);
    if (!out) return;               // keep the bigger picture rather than nothing
    for (int y = 0; y < nh; y++) {
        int y0 = (int)((i64)y * img.h / nh), y1 = max(y0 + 1, (int)((i64)(y + 1) * img.h / nh));
        for (int x = 0; x < nw; x++) {
            int x0 = (int)((i64)x * img.w / nw), x1 = max(x0 + 1, (int)((i64)(x + 1) * img.w / nw));
            u32 a = 0, r = 0, g = 0, b = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy += 1 + (y1 - y0) / 4)
                for (int xx = x0; xx < x1; xx += 1 + (x1 - x0) / 4) {
                    u32 c = img.px[(size_t)yy * img.w + xx];
                    u32 ca = c >> 24;
                    a += ca;
                    r += ((c >> 16) & 255) * ca;
                    g += ((c >> 8) & 255) * ca;
                    b += (c & 255) * ca;
                    cnt++;
                }
            u32 oa = a / cnt;
            out[y * nw + x] = a ? (oa << 24) | ((r / a) << 16) | ((g / a) << 8) | (b / a) : 0;
        }
    }
    kfree(img.px);
    img.px = out;
    img.w = nw;
    img.h = nh;
}
