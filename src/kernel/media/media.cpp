// silicon - media decoders: WAV, MP3, FLAC and AVI (Motion JPEG)
#include "media.h"
#include "../fs/fs.h"

#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3.h"

static inline u32 le16(const u8* p) { return p[0] | (p[1] << 8); }
static inline u32 le32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static inline u32 be32(const u8* p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static inline i16 clip16(int v) { return (i16)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

bool media_is_media_name(const char* n) {
    return str_icontains(n, ".wav") || str_icontains(n, ".mp3") || str_icontains(n, ".flac") || str_icontains(n, ".aac") || str_icontains(n, ".m4a") || str_icontains(n, ".mp4") || str_icontains(n, ".m4b") || str_icontains(n, ".ogg") || str_icontains(n, ".oga") || str_icontains(n, ".avi");
}

// ================================================================ WAV
struct WavTrack : AudioTrack {
    const u8* d = nullptr;
    u32 len = 0;
    int tag = 1, ch = 2, bits = 16, align = 4, spb = 0;     // spb: samples per ADPCM block
    u64 pos = 0;                                            // next frame
    // ADPCM block cache
    i16 blk[4096 * 2];
    u64 blk_first = 1ull << 60;
    int blk_n = 0;

    static int step_table(int i) {
        static const u16 t[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
            107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
            1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
            10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
        return t[i];
    }
    static void adpcm_step(int nib, int& pred, int& idx) {
        static const i8 adj[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
        int step = step_table(idx);
        int diff = step >> 3;
        if (nib & 1) diff += step >> 2;
        if (nib & 2) diff += step >> 1;
        if (nib & 4) diff += step;
        if (nib & 8) diff = -diff;
        pred = clamp(pred + diff, -32768, 32767);
        idx = clamp(idx + adj[nib & 7], 0, 88);
    }
    void decode_block(u64 b) {
        const u8* p = d + b * align;
        if ((b + 1) * align > len) { blk_n = 0; blk_first = b * spb; return; }
        int pred[2], idx[2];
        for (int c = 0; c < ch; c++) { pred[c] = (i16)le16(p + c * 4); idx[c] = min<int>(p[c * 4 + 2], 88); }
        blk_n = 0;
        blk[0] = (i16)pred[0]; blk[1] = (i16)pred[ch > 1 ? 1 : 0];
        int n = 1;
        const u8* q = p + 4 * ch;
        int groups = (align - 4 * ch) / (4 * ch);
        for (int g = 0; g < groups && n < spb; g++) {
            for (int c = 0; c < ch; c++) {
                for (int k = 0; k < 4; k++) {
                    u8 byte = q[(g * ch + c) * 4 + k];
                    for (int h = 0; h < 2; h++) {
                        adpcm_step(h ? byte >> 4 : byte & 15, pred[c], idx[c]);
                        int s = n + k * 2 + h;
                        if (s < 4096) blk[s * 2 + (c ? 1 : 0)] = (i16)pred[c];
                        if (ch == 1 && s < 4096) blk[s * 2 + 1] = (i16)pred[0];
                    }
                }
            }
            n += 8;
        }
        blk_n = min(n, spb);
        blk_first = b * spb;
    }
    int read(i16* out, int n) override {
        int got = 0;
        while (got < n && pos < frames) {
            if (tag == 0x11) {
                u64 b = pos / spb;
                if (blk_first != b * spb) decode_block(b);
                int off = (int)(pos - b * spb);
                if (off >= blk_n) { pos = (b + 1) * spb; continue; }
                out[got * 2] = blk[off * 2];
                out[got * 2 + 1] = blk[off * 2 + 1];
                got++; pos++;
                continue;
            }
            const u8* p = d + pos * align;
            int l = 0, r = 0;
            for (int c = 0; c < min(ch, 2); c++) {
                int v = 0;
                const u8* s = p + c * (bits / 8);
                if (tag == 3) { float f; memcpy(&f, s, 4); v = (int)(f * 32767.0f); }
                else if (bits == 8) v = ((int)s[0] - 128) << 8;
                else if (bits == 16) v = (i16)le16(s);
                else if (bits == 24) v = (i16)le16(s + 1);
                else v = (i16)le16(s + 2);
                if (c == 0) l = v; else r = v;
            }
            if (ch == 1) r = l;
            out[got * 2] = clip16(l);
            out[got * 2 + 1] = clip16(r);
            got++; pos++;
        }
        return got;
    }
    void seek(u64 f) override { pos = min(f, frames); }
};

static bool wav_parse(const u8* d, u32 n, WavTrack* t, u32* data_off, u32* data_len, char* err, int errlen) {
    if (n < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) { snprintf(err, errlen, "not a WAV file"); return false; }
    u32 p = 12;
    bool have_fmt = false;
    *data_off = 0; *data_len = 0;
    while (p + 8 <= n) {
        u32 sz = le32(d + p + 4);
        if (!memcmp(d + p, "fmt ", 4) && p + 8 + 16 <= n) {
            const u8* f = d + p + 8;
            t->tag = le16(f);
            t->ch = le16(f + 2);
            t->rate = le32(f + 4);
            t->align = le16(f + 12);
            t->bits = le16(f + 14);
            if (t->tag == 0xFFFE && sz >= 40) t->tag = le16(f + 24);
            if (t->tag == 0x11 && sz >= 20) t->spb = le16(f + 18);
            have_fmt = true;
        } else if (!memcmp(d + p, "data", 4)) {
            *data_off = p + 8;
            *data_len = min(sz, n - (p + 8));
            break;
        }
        p += 8 + sz + (sz & 1);
    }
    if (!have_fmt || !*data_off) { snprintf(err, errlen, "damaged WAV file"); return false; }
    if (t->ch < 1 || t->ch > 8 || t->rate < 1000 || t->rate > 192000) { snprintf(err, errlen, "unsupported WAV layout"); return false; }
    bool ok = (t->tag == 1 && (t->bits == 8 || t->bits == 16 || t->bits == 24 || t->bits == 32)) || (t->tag == 3 && t->bits == 32) ||
              (t->tag == 0x11 && t->ch <= 2 && t->spb > 0 && t->spb <= 4096);
    if (!ok) { snprintf(err, errlen, "this WAV format (tag %d, %d bit) isn't supported", t->tag, t->bits); return false; }
    if (t->tag != 0x11) t->align = t->ch * (t->bits / 8);
    return true;
}

// ================================================================ MP3 (minimp3, public domain)
struct Mp3Track : AudioTrack {
    mp3dec_t dec;
    const u8* d = nullptr;
    u32 len = 0;
    Vec<u32> frame_off;                  // where each frame starts
    int cur = 0;                         // next frame to decode
    i16 pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int pcm_n = 0, pcm_pos = 0;          // decoded frames waiting (per channel count)
    int ch = 2;
    int skip = 0;                        // frames to drop after a seek (decoder warm-up)
    int per_frame = 1152;

    u8* own = nullptr;                   // a private copy of the stream (MP3 inside AVI)
    ~Mp3Track() { frame_off.free(); if (own) kfree(own); }

    int read(i16* out, int n) override {
        int got = 0;
        while (got < n) {
            if (pcm_pos >= pcm_n) {
                if (cur >= frame_off.len) break;
                mp3dec_frame_info_t info;
                u32 off = frame_off[cur];
                int s = mp3dec_decode_frame(&dec, d + off, (int)(len - off), pcm, &info);
                cur++;
                if (s <= 0) { pcm_n = pcm_pos = 0; continue; }
                ch = info.channels;
                pcm_n = s; pcm_pos = 0;
                if (skip > 0) { skip--; pcm_pos = pcm_n; continue; }
            }
            int take = min(n - got, pcm_n - pcm_pos);
            for (int i = 0; i < take; i++) {
                if (ch == 1) { out[(got + i) * 2] = out[(got + i) * 2 + 1] = pcm[pcm_pos + i]; }
                else { out[(got + i) * 2] = pcm[(pcm_pos + i) * 2]; out[(got + i) * 2 + 1] = pcm[(pcm_pos + i) * 2 + 1]; }
            }
            got += take;
            pcm_pos += take;
        }
        return got;
    }
    void seek(u64 f) override {
        int fr = (int)(f / per_frame);
        int start = max(0, fr - 2);                       // a couple of frames to refill the bit reservoir
        mp3dec_init(&dec);
        cur = min(start, frame_off.len);
        skip = fr - start;
        pcm_n = pcm_pos = 0;
        // drop the part of the target frame before the sample
        u64 into = f - (u64)fr * per_frame;
        if (into && cur < frame_off.len) {
            i16 tmp[1152 * 2];
            int left = (int)into;
            // decode warm-up frames and the target frame, then discard `into` frames
            while (left > 0) { int g = read(tmp, min(left, 1152)); if (g <= 0) break; left -= g; }
        }
    }
};

static int id3_skip(const u8* d, u32 n, char* title, char* artist) {
    if (n < 10 || memcmp(d, "ID3", 3)) return 0;
    u32 sz = ((d[6] & 0x7F) << 21) | ((d[7] & 0x7F) << 14) | ((d[8] & 0x7F) << 7) | (d[9] & 0x7F);
    u32 end = min<u32>(10 + sz, n), p = 10;
    int ver = d[3];
    while (p + 10 < end) {
        const u8* f = d + p;
        if (!f[0]) break;
        u32 fs = ver == 4 ? (((f[4] & 0x7F) << 21) | ((f[5] & 0x7F) << 14) | ((f[6] & 0x7F) << 7) | (f[7] & 0x7F)) : be32(f + 4);
        if (fs == 0 || p + 10 + fs > end) break;
        char* dst = !memcmp(f, "TIT2", 4) ? title : !memcmp(f, "TPE1", 4) ? artist : nullptr;
        if (dst && fs > 1) {
            const u8* t = f + 10;
            int enc = t[0], o = 0;
            if (enc == 0 || enc == 3) { for (u32 i = 1; i < fs && o < 62; i++) if (t[i] >= 32) dst[o++] = (char)t[i]; }
            else if (enc == 1 || enc == 2) { for (u32 i = 1; i + 1 < fs && o < 62; i += 2) { u8 lo = t[i], hi = t[i + 1]; if (enc == 1 && i == 1 && (lo == 0xFF || lo == 0xFE)) continue; if (!hi && lo >= 32) dst[o++] = (char)lo; } }
            dst[o] = 0;
        }
        p += 10 + fs;
    }
    return (int)end;
}

static Mp3Track* mp3_open(const u8* d, u32 n, Media* m, char* err, int errlen) {
    Mp3Track* t = new Mp3Track();
    int start = id3_skip(d, n, m->title, m->artist);
    t->d = d; t->len = n;
    mp3dec_init(&t->dec);
    u32 pos = (u32)start;
    u64 total = 0;
    int rate = 0;
    while (pos + 4 < n) {
        mp3dec_frame_info_t info;
        mp3dec_t scan;
        mp3dec_init(&scan);
        int s = mp3dec_decode_frame(&scan, d + pos, (int)(n - pos), nullptr, &info);
        if (info.frame_bytes <= 0) { pos++; if (n - pos < 4) break; continue; }
        if (s > 0 || info.frame_bytes > 0) {
            if (s > 0) {
                if (!rate) { rate = info.hz; t->ch = info.channels; t->per_frame = s; }
                t->frame_off.push(pos + info.frame_offset);
                total += s;
            }
        }
        pos += info.frame_offset + info.frame_bytes;
        if (t->frame_off.len > 400000) break;
    }
    if (!t->frame_off.len || !rate) { snprintf(err, errlen, "no MP3 audio found"); delete t; return nullptr; }
    t->rate = rate;
    t->frames = total;
    return t;
}

// ================================================================ FLAC
struct Bits {
    const u8* d; u32 len; u64 bitpos;
    u32 read(int n) {
        u32 v = 0;
        while (n > 0) {
            u32 byte = (u32)(bitpos >> 3);
            if (byte >= len) { bitpos += n; return v << n; }
            int avail = 8 - (int)(bitpos & 7);
            int take = min(avail, n);
            u32 bits = (d[byte] >> (avail - take)) & ((1u << take) - 1);
            v = (v << take) | bits;
            bitpos += take; n -= take;
        }
        return v;
    }
    int sread(int n) { if (!n) return 0; u32 v = read(n); return (int)(v << (32 - n)) >> (32 - n); }
    u32 unary() { u32 q = 0; while (bitpos >> 3 < len) { if (read(1)) return q; q++; } return q; }
    bool eof() const { return (bitpos >> 3) >= len; }
};

struct FlacTrack : AudioTrack {
    const u8* d = nullptr;
    u32 len = 0, first_frame = 0, pos_byte = 0;
    int ch = 2, bps = 16, maxblock = 4608;
    int32_t* sub[8] = {};
    int blk_n = 0, blk_pos = 0;
    u64 next_sample = 0;                  // sample number of the next frame to decode
    i16* out_blk = nullptr;

    ~FlacTrack() { for (auto p : sub) if (p) kfree(p); if (out_blk) kfree(out_blk); }

    static int order_coef(int order, int k) {
        static const i8 c[5][4] = {{0}, {1}, {2, -1}, {3, -3, 1}, {4, -6, 4, -1}};
        return c[order][k];
    }

    bool residual(Bits& b, int bs, int order, int32_t* x) {
        int method = b.read(2);
        if (method > 1) return false;
        int porder = b.read(4);
        int parts = 1 << porder;
        int pos = order;
        for (int p = 0; p < parts; p++) {
            int n = porder == 0 ? bs - order : (p == 0 ? (bs >> porder) - order : bs >> porder);
            int param = b.read(method ? 5 : 4);
            int esc = method ? 31 : 15;
            if (n < 0) return false;
            if (param == esc) {
                int bits = b.read(5);
                for (int i = 0; i < n; i++) x[pos++] = b.sread(bits);
            } else {
                for (int i = 0; i < n; i++) {
                    u32 q = b.unary();
                    u32 v = (q << param) | (param ? b.read(param) : 0);
                    x[pos++] = (int32_t)(v >> 1) ^ -(int32_t)(v & 1);
                }
            }
            if (b.eof()) return false;
        }
        return true;
    }

    bool subframe(Bits& b, int bs, int sbps, int32_t* x) {
        if (b.read(1)) return false;
        int type = b.read(6);
        int wasted = 0;
        if (b.read(1)) wasted = b.unary() + 1;
        sbps -= wasted;
        if (type == 0) { int32_t v = b.sread(sbps); for (int i = 0; i < bs; i++) x[i] = v; }
        else if (type == 1) { for (int i = 0; i < bs; i++) x[i] = b.sread(sbps); }
        else if (type >= 8 && type <= 12) {
            int order = type - 8;
            for (int i = 0; i < order; i++) x[i] = b.sread(sbps);
            if (!residual(b, bs, order, x)) return false;
            for (int i = order; i < bs; i++) {
                int64_t pred = 0;
                for (int k = 0; k < order; k++) pred += (int64_t)order_coef(order, k) * x[i - 1 - k];
                x[i] = (int32_t)(x[i] + pred);
            }
        } else if (type >= 32) {
            int order = type - 31;
            for (int i = 0; i < order; i++) x[i] = b.sread(sbps);
            int prec = b.read(4) + 1;
            int shift = b.sread(5);
            int32_t coef[32];
            for (int i = 0; i < order; i++) coef[i] = b.sread(prec);
            if (!residual(b, bs, order, x)) return false;
            for (int i = order; i < bs; i++) {
                int64_t sum = 0;
                for (int k = 0; k < order; k++) sum += (int64_t)coef[k] * x[i - 1 - k];
                x[i] = (int32_t)(x[i] + (shift >= 0 ? (sum >> shift) : (sum << -shift)));
            }
        } else return false;
        if (wasted) for (int i = 0; i < bs; i++) x[i] = (int32_t)((uint32_t)x[i] << wasted);
        return true;
    }

    // decode the frame at pos_byte into out_blk (stereo i16); returns the number of samples or 0
    int decode_frame(u64* sample_no) {
        if (pos_byte + 6 >= len) return 0;
        const u8* p = d + pos_byte;
        if (p[0] != 0xFF || (p[1] & 0xFE) != 0xF8) {            // lost sync: search for the next frame
            u32 q = pos_byte + 1;
            while (q + 2 < len && !(d[q] == 0xFF && (d[q + 1] & 0xFE) == 0xF8)) q++;
            pos_byte = q;
            return -1;
        }
        Bits b = {d, len, (u64)pos_byte * 8};
        b.read(14); b.read(1);
        int variable = b.read(1);
        int bscode = b.read(4), rcode = b.read(4), chan = b.read(4), szcode = b.read(3);
        b.read(1);
        // UTF-8 style frame / sample number
        u32 first = b.read(8);
        int extra = first < 0x80 ? 0 : first < 0xE0 ? 1 : first < 0xF0 ? 2 : first < 0xF8 ? 3 : first < 0xFC ? 4 : first < 0xFE ? 5 : 6;
        u64 num = extra == 0 ? first : (first & (0x3F >> extra));
        for (int i = 0; i < extra; i++) num = (num << 6) | (b.read(8) & 0x3F);
        int bs = bscode == 1 ? 192 : bscode >= 2 && bscode <= 5 ? 576 << (bscode - 2) : bscode >= 8 ? 256 << (bscode - 8) : 0;
        if (bscode == 6) bs = b.read(8) + 1;
        else if (bscode == 7) bs = b.read(16) + 1;
        if (rcode == 12) b.read(8); else if (rcode == 13 || rcode == 14) b.read(16);
        b.read(8);                                              // crc-8
        int sbps = szcode == 0 ? bps : szcode == 1 ? 8 : szcode == 2 ? 12 : szcode == 4 ? 16 : szcode == 5 ? 20 : szcode == 6 ? 24 : 0;
        if (!bs || !sbps || bs > maxblock || chan > 10) { pos_byte++; return -1; }
        int nch = chan <= 7 ? chan + 1 : 2;
        if (nch > 8) return -1;
        for (int c = 0; c < nch; c++) {
            int bits = sbps + ((chan == 8 && c == 1) || (chan == 9 && c == 0) || (chan == 10 && c == 1) ? 1 : 0);
            if (!subframe(b, bs, bits, sub[c])) { pos_byte++; return -1; }
        }
        if (chan == 8) for (int i = 0; i < bs; i++) sub[1][i] = sub[0][i] - sub[1][i];
        else if (chan == 9) for (int i = 0; i < bs; i++) sub[0][i] = sub[0][i] + sub[1][i];
        else if (chan == 10) for (int i = 0; i < bs; i++) {
            int32_t mid = sub[0][i], side = sub[1][i];
            mid = (int32_t)(((uint32_t)mid << 1) | (side & 1));
            sub[0][i] = (mid + side) >> 1;
            sub[1][i] = (mid - side) >> 1;
        }
        b.bitpos = (b.bitpos + 7) & ~7ull;
        b.read(16);                                             // crc-16
        pos_byte = (u32)(b.bitpos >> 3);
        int shift = sbps - 16;
        for (int i = 0; i < bs; i++) {
            int l = sub[0][i], r = nch > 1 ? sub[1][i] : l;
            out_blk[i * 2] = clip16(shift >= 0 ? l >> shift : l << -shift);
            out_blk[i * 2 + 1] = clip16(shift >= 0 ? r >> shift : r << -shift);
        }
        *sample_no = variable ? num : num * (u64)(bs);
        (void)variable;
        return bs;
    }

    int read(i16* out, int n) override {
        int got = 0;
        while (got < n) {
            if (blk_pos >= blk_n) {
                u64 sn = 0;
                int r = decode_frame(&sn);
                if (r < 0) continue;
                if (r == 0) break;
                blk_n = r; blk_pos = 0;
                next_sample = sn + r;
            }
            int take = min(n - got, blk_n - blk_pos);
            memcpy(out + got * 2, out_blk + blk_pos * 2, (size_t)take * 4);
            got += take; blk_pos += take;
        }
        return got;
    }
    void seek(u64 f) override {
        pos_byte = first_frame;
        blk_n = blk_pos = 0;
        u64 sn = 0;
        while (true) {
            u32 save = pos_byte;
            int r = decode_frame(&sn);
            if (r < 0) continue;
            if (r == 0) break;
            if (sn + r > f) {
                blk_n = r; blk_pos = (int)(f > sn ? f - sn : 0);
                (void)save;
                break;
            }
        }
    }
};

static FlacTrack* flac_open(const u8* d, u32 n, Media* m, char* err, int errlen) {
    if (n < 42 || memcmp(d, "fLaC", 4)) { snprintf(err, errlen, "not a FLAC file"); return nullptr; }
    FlacTrack* t = new FlacTrack();
    t->d = d; t->len = n;
    u32 p = 4;
    bool last = false, have = false;
    while (!last && p + 4 <= n) {
        last = d[p] & 0x80;
        int type = d[p] & 0x7F;
        u32 sz = (d[p + 1] << 16) | (d[p + 2] << 8) | d[p + 3];
        p += 4;
        if (type == 0 && sz >= 34) {
            const u8* s = d + p;
            t->maxblock = (s[2] << 8) | s[3];
            u64 v = ((u64)s[10] << 56) | ((u64)s[11] << 48) | ((u64)s[12] << 40) | ((u64)s[13] << 32) | ((u64)s[14] << 24) | ((u64)s[15] << 16) | ((u64)s[16] << 8) | s[17];
            t->rate = (int)(v >> 44);
            t->ch = (int)((v >> 41) & 7) + 1;
            t->bps = (int)((v >> 36) & 31) + 1;
            t->frames = v & 0xFFFFFFFFFull;
            have = true;
        } else if (type == 4 && p + 8 < n) {                         // vorbis comments: TITLE= ARTIST=
            const u8* s = d + p;
            u32 vl = le32(s), q = 4 + vl;
            if (q + 4 <= sz) {
                u32 cnt = le32(s + q);
                q += 4;
                for (u32 i = 0; i < cnt && q + 4 <= sz; i++) {
                    u32 cl = le32(s + q);
                    q += 4;
                    if (q + cl > sz) break;
                    const char* c = (const char*)s + q;
                    if (cl > 6 && !strncmp(c, "TITLE=", 6)) { u32 k = min<u32>(cl - 6, 62); memcpy(m->title, c + 6, k); m->title[k] = 0; }
                    if (cl > 7 && !strncmp(c, "ARTIST=", 7)) { u32 k = min<u32>(cl - 7, 62); memcpy(m->artist, c + 7, k); m->artist[k] = 0; }
                    q += cl;
                }
            }
        }
        p += sz;
    }
    if (!have || t->ch > 8 || t->maxblock < 16 || t->maxblock > 65535 || t->bps < 4 || t->bps > 32 || t->rate < 1000) {
        snprintf(err, errlen, "unsupported FLAC stream"); delete t; return nullptr;
    }
    t->first_frame = t->pos_byte = p;
    for (int i = 0; i < t->ch; i++) t->sub[i] = (int32_t*)kmalloc((size_t)t->maxblock * 4 + 16);
    t->out_blk = (i16*)kmalloc((size_t)t->maxblock * 4 + 16);
    return t;
}

// ================================================================ Ogg Vorbis (stb_vorbis, see vorbis.cpp)
void* vorbis_open(const u8* data, u32 len, int* rate, u64* frames);
int   vorbis_read(void* h, i16* out, int n);
void  vorbis_seek(void* h, u64 frame);
void  vorbis_close(void* h);

struct OggTrack : AudioTrack {
    void* h = nullptr;
    ~OggTrack() { vorbis_close(h); }
    int read(i16* out, int n) override { return vorbis_read(h, out, n); }
    void seek(u64 f) override { vorbis_seek(h, f); }
};

// ================================================================ AVI with Motion JPEG
struct AviVideo : VideoTrack {
    const u8* d = nullptr;
    u32 len = 0;
    Vec<u32> off, sz;
    ~AviVideo() { off.free(); sz.free(); }
    bool decode(int index, Image& out) override {
        if (index < 0 || index >= off.len) return false;
        const u8* f = d + off[index];
        u32 n = sz[index];
        if (n < 4 || f[0] != 0xFF || f[1] != 0xD8) return false;
        // frames without Huffman tables use the standard ones
        bool has_dht = false;
        for (u32 i = 2; i + 4 < n && i < 2048;) {
            if (f[i] != 0xFF) break;
            u8 mk = f[i + 1];
            if (mk == 0xC4) { has_dht = true; break; }
            if (mk == 0xDA) break;
            i += 2 + ((f[i + 2] << 8) | f[i + 3]);
        }
        if (has_dht) return image_load(f, n, out);
        static const u8 dht[] = {
#include "jpeg_dht.inc"
        };
        u8* tmp = (u8*)kmalloc_try(n + sizeof(dht));
        if (!tmp) return false;
        memcpy(tmp, f, 2);
        memcpy(tmp + 2, dht, sizeof(dht));
        memcpy(tmp + 2 + sizeof(dht), f + 2, n - 2);
        bool ok = image_load(tmp, n + sizeof(dht), out);
        kfree(tmp);
        return ok;
    }
};

struct AviAudio : AudioTrack {                 // PCM audio chunks, read through the list of spans
    const u8* d = nullptr;
    Vec<u32> off, sz;
    Vec<u64> cum;                              // bytes before each chunk
    int ch = 2, bits = 16;
    u64 pos = 0;                               // next frame
    u64 total_bytes = 0;
    ~AviAudio() { off.free(); sz.free(); cum.free(); }

    u8 byte_at(u64 b, int* span) {
        int s = *span;
        while (s + 1 < cum.len && b >= cum[s + 1]) s++;
        while (s > 0 && b < cum[s]) s--;
        *span = s;
        return d[off[s] + (b - cum[s])];
    }
    int read(i16* out, int n) override {
        int bpf = ch * (bits / 8), got = 0, span = 0;
        while (got < n && (pos + 1) * bpf <= total_bytes) {
            u64 b = pos * bpf;
            int l = 0, r = 0;
            for (int c = 0; c < min(ch, 2); c++) {
                int v;
                if (bits == 8) v = ((int)byte_at(b + c, &span) - 128) << 8;
                else v = (i16)(byte_at(b + c * 2, &span) | (byte_at(b + c * 2 + 1, &span) << 8));
                if (c == 0) l = v; else r = v;
            }
            if (ch == 1) r = l;
            out[got * 2] = (i16)l; out[got * 2 + 1] = (i16)r;
            got++; pos++;
        }
        return got;
    }
    void seek(u64 f) override { pos = min(f, frames); }
};

static bool avi_open(const u8* d, u32 n, Media* m, char* err, int errlen) {
    if (n < 64 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "AVI ", 4)) { snprintf(err, errlen, "not an AVI file"); return false; }
    AviVideo* v = new AviVideo();
    AviAudio* a = new AviAudio();
    v->d = d; v->len = n;
    a->d = d;
    u32 usec = 40000;
    int stream_kind[8] = {};
    int nstreams = 0;
    u32 vscale = 1, vrate = 25;
    bool v_mjpg = false, a_pcm = false, a_mp3 = false;
    int a_rate = 44100;
    // walk the chunks recursively with a small explicit stack
    struct Scope { u32 pos, end; };
    Scope st[8];
    int sp = 0;
    st[sp++] = {12, n};
    int vstream = -1, astream = -1;
    while (sp) {
        Scope& s = st[sp - 1];
        if (s.pos + 8 > s.end) { sp--; continue; }
        const u8* c = d + s.pos;
        u32 csz = le32(c + 4);
        u32 body = s.pos + 8;
        u32 next = body + csz + (csz & 1);
        if (!memcmp(c, "LIST", 4)) {
            bool movi = !memcmp(d + body, "movi", 4);
            s.pos = next;
            if (sp < 8) st[sp++] = {body + 4, min(body + csz, n)};
            if (movi) {
                // index the frames and audio chunks
                u32 p = body + 4, end = min(body + csz, n);
                while (p + 8 <= end) {
                    const u8* cc = d + p;
                    u32 z = le32(cc + 4);
                    if (p + 8 + z > n) break;
                    int sn = (cc[0] >= '0' && cc[0] <= '9' && cc[1] >= '0' && cc[1] <= '9') ? (cc[0] - '0') * 10 + (cc[1] - '0') : -1;
                    if (cc[2] == 'd' && (cc[3] == 'c' || cc[3] == 'b') && sn == vstream) { v->off.push(p + 8); v->sz.push(z); }
                    else if (cc[2] == 'w' && cc[3] == 'b' && sn == astream) { a->cum.push(a->total_bytes); a->off.push(p + 8); a->sz.push(z); a->total_bytes += z; }
                    else if (!memcmp(cc, "LIST", 4)) { p += 12; continue; }          // 'rec ' lists
                    p += 8 + z + (z & 1);
                }
                sp--;                                                               // movi is done
            }
            continue;
        }
        if (!memcmp(c, "avih", 4) && csz >= 8) usec = le32(d + body);
        if (!memcmp(c, "strh", 4) && csz >= 28) {
            const u8* h = d + body;
            if (!memcmp(h, "vids", 4)) { stream_kind[nstreams] = 1; if (vstream < 0) { vstream = nstreams; v_mjpg = !memcmp(h + 4, "MJPG", 4) || !memcmp(h + 4, "mjpg", 4) || !memcmp(h + 4, "AVI1", 4); vscale = le32(h + 20); vrate = le32(h + 24); } }
            else if (!memcmp(h, "auds", 4)) { stream_kind[nstreams] = 2; if (astream < 0) astream = nstreams; }
            nstreams++;
        }
        if (!memcmp(c, "strf", 4)) {
            int sidx = nstreams - 1;
            if (sidx == vstream && csz >= 40) { v->w = (int)le32(d + body + 4); v->h = (int)le32(d + body + 8); if (v->h < 0) v->h = -v->h; if (!memcmp(d + body + 16, "MJPG", 4) || !memcmp(d + body + 16, "mjpg", 4)) v_mjpg = true; }
            if (sidx == astream && csz >= 16) {
                int tag = le16(d + body);
                a->ch = le16(d + body + 2);
                a_rate = (int)le32(d + body + 4);
                a->bits = le16(d + body + 14);
                a_mp3 = tag == 0x55 || tag == 0x50;
                a_pcm = tag == 1 && (a->bits == 8 || a->bits == 16) && a->ch >= 1 && a->ch <= 2;
            }
        }
        s.pos = next;
    }
    if (!v_mjpg || !v->off.len || v->w <= 0 || v->h <= 0) {
        snprintf(err, errlen, "this AVI isn't Motion JPEG (no other video codec is built in)");
        delete v; delete a; return false;
    }
    v->nframes = v->off.len;
    v->fps_x1000 = vscale ? (int)((u64)vrate * 1000 / vscale) : (usec ? (int)(1000000000ull / usec) : 25000);
    if (v->fps_x1000 < 1000) v->fps_x1000 = 25000;
    m->video = v;
    if (a_pcm && a->off.len) {
        a->rate = a_rate;
        a->frames = a->total_bytes / (a->ch * (a->bits / 8));
        m->audio = a;
    } else if (a_mp3 && a->off.len) {
        // MP3 audio chunks: join them into one stream and decode it like an .mp3 file
        u8* joined = (u8*)kmalloc_try(a->total_bytes + 16);
        if (joined) {
            u32 o = 0;
            for (int i = 0; i < a->off.len; i++) { memcpy(joined + o, d + a->off[i], a->sz[i]); o += a->sz[i]; }
            char e2[64];
            Media tmp_title; tmp_title.title[0] = tmp_title.artist[0] = 0;
            if (Mp3Track* t = mp3_open(joined, o, &tmp_title, e2, sizeof(e2))) { t->own = joined; m->audio = t; }
            else kfree(joined);
        }
        delete a;
    } else delete a;
    m->duration_ms = (u64)v->nframes * 1000000ull / v->fps_x1000;
    return true;
}

// ================================================================ AAC (ADTS files and MP4 / M4A)
struct AacDec;
AacDec* aac_open(int object_type, int sr_index, int channels, char* err, int errlen);
void aac_close(AacDec* d);
int aac_rate(AacDec* d);
bool aac_decode(AacDec* d, const u8* data, u32 len, i16* out);

struct AacTrack : AudioTrack {
    AacDec* dec = nullptr;
    const u8* d = nullptr;
    Vec<u32> off, size;
    int cur = 0;
    i16 pcm[2048];
    int pcm_pos = 1024, pcm_n = 1024;
    ~AacTrack() { if (dec) aac_close(dec); off.free(); size.free(); }
    int read(i16* out, int n) override {
        int got = 0;
        while (got < n) {
            if (pcm_pos >= pcm_n) {
                if (cur >= off.len) break;
                if (!aac_decode(dec, d + off[cur], size[cur], pcm)) memset(pcm, 0, sizeof(pcm));
                cur++;
                pcm_pos = 0;
                pcm_n = 1024;
            }
            int take = min(n - got, pcm_n - pcm_pos);
            memcpy(out + got * 2, pcm + pcm_pos * 2, (size_t)take * 4);
            got += take; pcm_pos += take;
        }
        return got;
    }
    void seek(u64 f) override {
        int fr = (int)(f / 1024);
        int start = max(0, fr - 2);                          // frames before it rebuild the overlap
        char e[32];
        aac_close(dec);
        dec = aac_open(2, sr_idx, chans, e, sizeof(e));
        cur = min(start, off.len);
        pcm_pos = pcm_n = 1024;
        while (cur < fr && cur < off.len) { aac_decode(dec, d + off[cur], size[cur], pcm); cur++; }
        if (cur < off.len) {
            aac_decode(dec, d + off[cur], size[cur], pcm);
            cur++;
            pcm_pos = (int)(f - (u64)fr * 1024);
            pcm_n = 1024;
        }
    }
    int sr_idx = 4, chans = 2;
};

static int sr_to_index(int rate) {
    static const int r[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    for (int i = 0; i < 13; i++) if (r[i] == rate) return i;
    return -1;
}

static AacTrack* aac_track_from(const u8* data, int obj, int sri, int ch, char* err, int errlen) {
    AacTrack* t = new AacTrack();
    t->d = data;
    t->sr_idx = sri; t->chans = ch;
    t->dec = aac_open(obj, sri, ch, err, errlen);
    if (!t->dec) { delete t; return nullptr; }
    t->rate = aac_rate(t->dec);
    return t;
}

static AacTrack* adts_open(const u8* d, u32 n, Media* m, char* err, int errlen) {
    int skip = id3_skip(d, n, m->title, m->artist);
    AacTrack* t = nullptr;
    u32 pos = (u32)skip;
    while (pos + 7 < n) {
        if (d[pos] != 0xFF || (d[pos + 1] & 0xF6) != 0xF0) { pos++; continue; }
        int prot_absent = d[pos + 1] & 1;
        int profile = (d[pos + 2] >> 6) & 3, sri = (d[pos + 2] >> 2) & 15, ch = ((d[pos + 2] & 1) << 2) | (d[pos + 3] >> 6);
        u32 flen = ((d[pos + 3] & 3) << 11) | (d[pos + 4] << 3) | (d[pos + 5] >> 5);
        int hdr = prot_absent ? 7 : 9;
        if (flen < (u32)hdr || pos + flen > n + 0u) { if (pos + flen > n && t) break; pos++; continue; }
        if (!t) {
            if (sri > 12) { pos++; continue; }
            t = aac_track_from(d, profile + 1, sri, ch ? ch : 2, err, errlen);
            if (!t) return nullptr;
        }
        t->off.push(pos + hdr);
        t->size.push(flen - hdr);
        pos += flen;
    }
    if (!t || !t->off.len) { snprintf(err, errlen, "no AAC frames found"); delete t; return nullptr; }
    t->frames = (u64)t->off.len * 1024;
    return t;
}

// ---- MP4: boxes
struct Box { u32 start, body, end; char type[5]; };
static bool box_next(const u8* d, u32 n, u32& pos, u32 limit, Box& b) {
    if (pos + 8 > limit || limit > n) return false;
    u32 sz = be32(d + pos);
    b.start = pos;
    memcpy(b.type, d + pos + 4, 4);
    b.type[4] = 0;
    b.body = pos + 8;
    u64 full = sz;
    if (sz == 1) { if (pos + 16 > limit) return false; full = ((u64)be32(d + pos + 8) << 32) | be32(d + pos + 12); b.body = pos + 16; }
    else if (sz == 0) full = limit - pos;
    if (full < 8 || pos + full > limit) { if (pos + full > limit && full >= 8) full = limit - pos; else return false; }
    b.end = (u32)(pos + full);
    pos = b.end;
    return true;
}
static bool box_find(const u8* d, u32 n, u32 from, u32 to, const char* type, Box& out) {
    u32 p = from;
    Box b;
    while (box_next(d, n, p, to, b)) if (!strcmp(b.type, type)) { out = b; return true; }
    return false;
}

struct Mp4Track {
    char handler[5];
    u32 timescale, duration;
    char codec[5];
    int channels, rate;
    Vec<u32> sz, off_chunk, stsc_first, stsc_n;
    Vec<u32> stts_cnt, stts_delta;
    u8 asc[64]; int asc_len;
    int width, height;
    bool is64;
    Vec<u64> off64;
    ~Mp4Track() { sz.free(); off_chunk.free(); stsc_first.free(); stsc_n.free(); stts_cnt.free(); stts_delta.free(); off64.free(); }
};

static void mp4_parse_stbl(const u8* d, u32 n, Box stbl, Mp4Track& t) {
    Box b;
    if (box_find(d, n, stbl.body, stbl.end, "stsd", b) && b.body + 16 <= b.end) {
        u32 p = b.body + 8;
        Box e;
        if (box_next(d, n, p, b.end, e)) {
            memcpy(t.codec, e.type, 5);
            if (!strcmp(t.handler, "soun") && e.body + 28 <= e.end) {
                t.channels = (int)((d[e.body + 16] << 8) | d[e.body + 17]);
                t.rate = (int)(be32(d + e.body + 24) >> 16);
                u32 q = e.body + 28;
                Box c;
                u32 qq = q;
                while (box_next(d, n, qq, e.end, c)) {
                    if (!strcmp(c.type, "esds")) {
                        // descriptors: ES (0x03) > DecoderConfig (0x04) > DecoderSpecificInfo (0x05)
                        u32 o = c.body + 4;
                        auto desc_len = [&](u32& pp) { u32 l = 0; for (int i = 0; i < 4 && pp < c.end; i++) { u8 x = d[pp++]; l = (l << 7) | (x & 0x7F); if (!(x & 0x80)) break; } return l; };
                        while (o + 2 < c.end) {
                            u8 tag = d[o++];
                            u32 l = desc_len(o);
                            if (tag == 0x03) o += 3;
                            else if (tag == 0x04) o += 13;
                            else if (tag == 0x05) { t.asc_len = (int)min<u32>(l, 64); memcpy(t.asc, d + o, t.asc_len); break; }
                            else o += l;
                        }
                    }
                }
            } else if (!strcmp(t.handler, "vide") && e.body + 28 <= e.end) {
                t.width = (int)((d[e.body + 24] << 8) | d[e.body + 25]);
                t.height = (int)((d[e.body + 26] << 8) | d[e.body + 27]);
            }
        }
    }
    if (box_find(d, n, stbl.body, stbl.end, "stsz", b) && b.body + 12 <= b.end) {
        u32 fixed = be32(d + b.body + 4), cnt = be32(d + b.body + 8);
        for (u32 i = 0; i < cnt && i < 4000000; i++) {
            if (fixed) t.sz.push(fixed);
            else if (b.body + 12 + (i + 1) * 4 <= b.end) t.sz.push(be32(d + b.body + 12 + i * 4));
        }
    }
    if (box_find(d, n, stbl.body, stbl.end, "stsc", b) && b.body + 8 <= b.end) {
        u32 cnt = be32(d + b.body + 4);
        for (u32 i = 0; i < cnt && b.body + 8 + (i + 1) * 12 <= b.end; i++) { t.stsc_first.push(be32(d + b.body + 8 + i * 12)); t.stsc_n.push(be32(d + b.body + 12 + i * 12)); }
    }
    if (box_find(d, n, stbl.body, stbl.end, "stco", b) && b.body + 8 <= b.end) {
        u32 cnt = be32(d + b.body + 4);
        for (u32 i = 0; i < cnt && b.body + 8 + (i + 1) * 4 <= b.end; i++) t.off64.push(be32(d + b.body + 8 + i * 4));
    } else if (box_find(d, n, stbl.body, stbl.end, "co64", b) && b.body + 8 <= b.end) {
        u32 cnt = be32(d + b.body + 4);
        for (u32 i = 0; i < cnt && b.body + 8 + (i + 1) * 8 <= b.end; i++) t.off64.push(((u64)be32(d + b.body + 8 + i * 8) << 32) | be32(d + b.body + 12 + i * 8));
    }
    if (box_find(d, n, stbl.body, stbl.end, "stts", b) && b.body + 8 <= b.end) {
        u32 cnt = be32(d + b.body + 4);
        for (u32 i = 0; i < cnt && b.body + 8 + (i + 1) * 8 <= b.end; i++) { t.stts_cnt.push(be32(d + b.body + 8 + i * 8)); t.stts_delta.push(be32(d + b.body + 12 + i * 8)); }
    }
}

// the file offset of every sample in a track
static void mp4_sample_offsets(Mp4Track& t, Vec<u32>& offs) {
    int s = 0;
    for (int ch = 0; ch < t.off64.len; ch++) {
        u32 chunk_no = (u32)ch + 1, per = 1;
        for (int k = 0; k < t.stsc_first.len; k++) if (t.stsc_first[k] <= chunk_no) per = t.stsc_n[k];
        u64 o = t.off64[ch];
        for (u32 i = 0; i < per && s < t.sz.len; i++, s++) { offs.push((u32)o); o += t.sz[s]; }
    }
}

static bool mp4_open(const u8* d, u32 n, Media* m, char* err, int errlen) {
    Box moov;
    if (!box_find(d, n, 0, n, "moov", moov)) { snprintf(err, errlen, "this MP4 has no movie box (a streaming or damaged file)"); return false; }
    Box trak;
    u32 p = moov.body;
    Mp4Track* audio = nullptr;
    char video_codec[5] = "";
    int vw = 0, vh = 0;
    bool have_video = false;
    Box b;
    while (box_next(d, n, p, moov.end, trak)) {
        if (!strcmp(trak.type, "udta") || !strcmp(trak.type, "meta")) {
            // iTunes style tags: ©nam / ©ART
            u32 q = trak.body + (!strcmp(trak.type, "meta") ? 4 : 0);
            Box ilst;
            if (box_find(d, n, q, trak.end, "ilst", ilst)) {
                u32 qq = ilst.body;
                Box it;
                while (box_next(d, n, qq, ilst.end, it)) {
                    Box dat;
                    if (!box_find(d, n, it.body, it.end, "data", dat) || dat.end <= dat.body + 8) continue;
                    u32 l = min<u32>(dat.end - dat.body - 8, 62);
                    if (it.type[0] == (char)0xA9 && !memcmp(it.type + 1, "nam", 3)) { memcpy(m->title, d + dat.body + 8, l); m->title[l] = 0; }
                    if (it.type[0] == (char)0xA9 && !memcmp(it.type + 1, "ART", 3)) { memcpy(m->artist, d + dat.body + 8, l); m->artist[l] = 0; }
                }
            }
            continue;
        }
        if (strcmp(trak.type, "trak")) continue;
        Box mdia, hdlr, mdhd, minf, stbl;
        if (!box_find(d, n, trak.body, trak.end, "mdia", mdia)) continue;
        Mp4Track* t = new Mp4Track();
        memset(t->handler, 0, sizeof(t->handler)); memset(t->codec, 0, sizeof(t->codec));
        t->asc_len = 0; t->width = t->height = t->channels = t->rate = 0;
        if (box_find(d, n, mdia.body, mdia.end, "hdlr", hdlr) && hdlr.body + 12 <= hdlr.end) memcpy(t->handler, d + hdlr.body + 8, 4);
        if (box_find(d, n, mdia.body, mdia.end, "mdhd", mdhd) && mdhd.body + 20 <= mdhd.end) {
            if (d[mdhd.body] == 1) { t->timescale = be32(d + mdhd.body + 20); t->duration = be32(d + mdhd.body + 28); }
            else { t->timescale = be32(d + mdhd.body + 12); t->duration = be32(d + mdhd.body + 16); }
        }
        if (box_find(d, n, mdia.body, mdia.end, "minf", minf) && box_find(d, n, minf.body, minf.end, "stbl", stbl)) mp4_parse_stbl(d, n, stbl, *t);
        if (!strcmp(t->handler, "soun") && !audio && t->sz.len) audio = t;
        else {
            if (!strcmp(t->handler, "vide")) { have_video = true; strlcpy(video_codec, t->codec, sizeof(video_codec)); vw = t->width; vh = t->height; }
            delete t;
        }
    }
    (void)b;
    if (have_video) snprintf(m->note, sizeof(m->note), "video track (%s, %dx%d) can't be decoded: only the sound plays", video_codec, vw, vh);
    if (!audio) { snprintf(err, errlen, have_video ? "this MP4 has video (%s) but no sound I can play; H.264 video isn't supported" : "no audio track found", video_codec); return false; }
    if (strcmp(audio->codec, "mp4a")) { snprintf(err, errlen, "audio codec '%s' isn't supported (AAC only)", audio->codec); delete audio; return false; }
    if (audio->asc_len < 2) { snprintf(err, errlen, "the AAC settings are missing"); delete audio; return false; }
    int obj = audio->asc[0] >> 3, sri = ((audio->asc[0] & 7) << 1) | (audio->asc[1] >> 7), ch = (audio->asc[1] >> 3) & 15;
    if (sri == 15) { snprintf(err, errlen, "unusual AAC sample rate"); delete audio; return false; }
    AacTrack* t = aac_track_from(d, obj, sri, ch, err, errlen);
    if (!t) { delete audio; return false; }
    Vec<u32> offs;
    mp4_sample_offsets(*audio, offs);
    for (int i = 0; i < offs.len && i < audio->sz.len; i++) {
        if ((u64)offs[i] + audio->sz[i] > n) break;
        t->off.push(offs[i]);
        t->size.push(audio->sz[i]);
    }
    offs.free();
    delete audio;
    if (!t->off.len) { snprintf(err, errlen, "no AAC samples found"); delete t; return false; }
    t->frames = (u64)t->off.len * 1024;
    m->audio = t;
    return true;
}

// ================================================================ the front door
Media* media_open(const u8* data, u32 size, const char* name, char* err, int errlen) {
    Media* m = new Media();
    m->data = data; m->size = size;
    m->title[0] = m->artist[0] = m->note[0] = 0;
    err[0] = 0;
    bool ok = false;
    if (str_icontains(name, ".wav") || (size > 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WAVE", 4))) {
        strlcpy(m->kind, "wav", sizeof(m->kind));
        WavTrack* t = new WavTrack();
        u32 off, dl;
        if (wav_parse(data, size, t, &off, &dl, err, errlen)) {
            t->d = data + off; t->len = dl;
            t->frames = t->tag == 0x11 ? (u64)(dl / t->align) * t->spb : dl / t->align;
            m->audio = t;
            ok = true;
        } else delete t;
    } else if (str_icontains(name, ".mp3") || (size > 3 && (!memcmp(data, "ID3", 3) || (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0)))) {
        strlcpy(m->kind, "mp3", sizeof(m->kind));
        if (Mp3Track* t = mp3_open(data, size, m, err, errlen)) { m->audio = t; ok = true; }
    } else if (str_icontains(name, ".flac") || (size > 4 && !memcmp(data, "fLaC", 4))) {
        strlcpy(m->kind, "flac", sizeof(m->kind));
        if (FlacTrack* t = flac_open(data, size, m, err, errlen)) { m->audio = t; ok = true; }
    } else if (str_icontains(name, ".mp4") || str_icontains(name, ".m4a") || str_icontains(name, ".m4b") || (size > 12 && !memcmp(data + 4, "ftyp", 4))) {
        strlcpy(m->kind, "m4a", sizeof(m->kind));
        snprintf(err, errlen, "AAC/MP4 decoding is not finished yet (the decoder is still out of tune), so these files are not played");
        ok = false;
    } else if (str_icontains(name, ".aac") || (size > 7 && data[0] == 0xFF && (data[1] & 0xF6) == 0xF0)) {
        strlcpy(m->kind, "aac", sizeof(m->kind));
        snprintf(err, errlen, "AAC decoding is not finished yet, so .aac files are not played");
    } else if (str_icontains(name, ".ogg") || str_icontains(name, ".oga") || (size > 4 && !memcmp(data, "OggS", 4))) {
        strlcpy(m->kind, "ogg", sizeof(m->kind));
        OggTrack* t = new OggTrack();
        t->h = vorbis_open(data, size, &t->rate, &t->frames);
        if (t->h) { m->audio = t; ok = true; } else { snprintf(err, errlen, "this isn't an Ogg Vorbis stream"); delete t; }
    } else if (str_icontains(name, ".avi") || (size > 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "AVI ", 4))) {
        strlcpy(m->kind, "avi", sizeof(m->kind));
        ok = avi_open(data, size, m, err, errlen);
    } else snprintf(err, errlen, "not a media file I know (WAV, MP3, FLAC, AVI)");
    if (!ok) { if (!err[0]) snprintf(err, errlen, "can't play this file"); delete m; return nullptr; }
    if (m->audio && !m->duration_ms) m->duration_ms = m->audio->frames * 1000 / max(1, m->audio->rate);
    return m;
}

void media_free(Media* m) {
    if (!m) return;
    delete m->audio;
    delete m->video;
    delete m;
}

void media_seek(Media* m, u64 ms) {
    if (!m->audio) return;
    u64 f = ms * (u64)m->audio->rate / 1000;
    m->audio->seek(f);
    m->pos16 = f << 16;
    m->cache_base = f;
    m->cache_len = 0;
    m->eof = false;
}

// 48 kHz stereo out of the track's native rate (linear interpolation)
int media_fill(void* mp, i16* out, int frames) {
    Media* m = (Media*)mp;
    AudioTrack* t = m->audio;
    if (!t) return 0;
    u32 step = (u32)(((u64)t->rate << 16) / 48000);
    int made = 0, peak = 0;
    const int CAP = 8192;
    while (made < frames) {
        u64 i0 = m->pos16 >> 16;
        if (i0 + 1 >= m->cache_base + (u64)m->cache_len) {
            // drop what we've passed and read more
            if (i0 > m->cache_base) {
                u64 drop = min<u64>(i0 - m->cache_base, (u64)m->cache_len);
                memmove(m->cache, m->cache + drop * 2, (size_t)(m->cache_len - drop) * 4);
                m->cache_base += drop;
                m->cache_len -= (int)drop;
            }
            while (!m->eof && m->cache_len < CAP / 2) {
                int g = t->read(m->cache + m->cache_len * 2, CAP - m->cache_len);
                if (g <= 0) { m->eof = true; break; }
                m->cache_len += g;
            }
            if (i0 + 1 >= m->cache_base + (u64)m->cache_len) {
                if (!m->eof || i0 >= m->cache_base + (u64)m->cache_len) break;     // nothing left
            }
        }
        int k = (int)(i0 - m->cache_base);
        if (k < 0) { m->cache_base = i0; m->cache_len = 0; continue; }
        int k1 = k + 1 < m->cache_len ? k + 1 : k;
        int fr = (int)(m->pos16 & 0xFFFF);
        int l = m->cache[k * 2] + (((m->cache[k1 * 2] - m->cache[k * 2]) * fr) >> 16);
        int r = m->cache[k * 2 + 1] + (((m->cache[k1 * 2 + 1] - m->cache[k * 2 + 1]) * fr) >> 16);
        out[made * 2] = (i16)l;
        out[made * 2 + 1] = (i16)r;
        if (l > peak) peak = l; else if (-l > peak) peak = -l;
        if (r > peak) peak = r; else if (-r > peak) peak = -r;
        made++;
        m->pos16 += step;
    }
    m->level = peak * 100 / 32768;
    return made;
}
