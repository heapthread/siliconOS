// silicon - AAC-LC decoder (ISO 14496-3): ADTS streams and raw frames from MP4, mono and stereo.
// Written for silicon after the structure of JAAD (public domain), whose Huffman codebooks are
// used (aac_tables.inc). Supported: long/short/start/stop windows with sine and KBD shapes,
// section/scalefactor decoding, pulse data, TNS, mid/side and intensity stereo, PNS.
// Not supported: Main-profile prediction, LTP, SBR/PS (HE-AAC plays without the extra bandwidth),
// coupling channels and more than two channels.
#ifdef AAC_HOST            // tools/aac_host_test.cpp builds this file on the development machine
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int8_t i8; typedef int16_t i16; typedef int32_t i32; typedef int64_t i64;
template <typename T> static inline T min(T a, T b) { return a < b ? a : b; }
template <typename T> static inline T max(T a, T b) { return a > b ? a : b; }
template <typename T> static inline T clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
static void* kmalloc(size_t n) { return malloc(n); }
static void kfree(void* p) { free(p); }
#else
#include "media.h"
#endif

struct HuffRow { u8 len; u32 code; i8 v[4]; };
#include "aac_tables.inc"

// ---------------------------------------------------------------- math helpers (x87)
static double xsin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static double xcos(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
static double xsqrt(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
static double xexp2(double x) {
    double fl = (double)(long long)x;
    if (fl > x) fl -= 1;
    double fr = x - fl, r;
    __asm__("f2xm1" : "=t"(r) : "0"(fr));
    r += 1.0;
    __asm__("fscale" : "+t"(r) : "u"(fl));
    return r;
}
static const double PI = 3.14159265358979323846;

// ---------------------------------------------------------------- tables built on first use
static const int LONGN = 1024, SHORTN = 128;
static float sine_long[LONGN], sine_short[SHORTN], kbd_long[LONGN], kbd_short[SHORTN];
static float iq_table[8192 + 16];
static float sf_table[256 + 200];
static float twid_long_c[LONGN / 2], twid_long_s[LONGN / 2], twid_short_c[SHORTN / 2], twid_short_s[SHORTN / 2];
static float pre_long[LONGN][2], post_long[LONGN][2];       // (cos, sin) twiddles for the DCT-IV
static float pre_short[SHORTN][2], post_short[SHORTN][2];
static bool tables_ready = false;

static double bessel_i0(double x) {
    double sum = 1, term = 1, q = x * x / 4;
    for (int k = 1; k < 60; k++) { term *= q / ((double)k * k); sum += term; if (term < 1e-14 * sum) break; }
    return sum;
}

static void make_kbd(float* w, int n, double alpha) {
    double* k = (double*)kmalloc(sizeof(double) * (n + 1));
    double total = 0;
    for (int j = 0; j <= n; j++) {
        double r = 2.0 * j / n - 1.0;
        k[j] = bessel_i0(PI * alpha * xsqrt(1.0 - r * r));
        total += k[j];
    }
    double acc = 0;
    for (int j = 0; j < n; j++) { acc += k[j]; w[j] = (float)xsqrt(acc / total); }
    kfree(k);
}

static void make_dct4_twiddles(int m, float (*pre)[2], float (*post)[2]) {
    for (int n = 0; n < m / 2; n++) {
        pre[n][0] = (float)xcos(PI * n / m);   pre[n][1] = (float)-xsin(PI * n / m);
        post[n][0] = (float)xcos(PI * (n + 0.25) / m); post[n][1] = (float)-xsin(PI * (n + 0.25) / m);
    }
}

static void init_tables() {
    if (tables_ready) return;
    tables_ready = true;
    for (int i = 0; i < LONGN; i++) sine_long[i] = (float)xsin(PI * (i + 0.5) / (2 * LONGN));
    for (int i = 0; i < SHORTN; i++) sine_short[i] = (float)xsin(PI * (i + 0.5) / (2 * SHORTN));
    make_kbd(kbd_long, LONGN, 4.0);
    make_kbd(kbd_short, SHORTN, 6.0);
    for (int i = 1; i < 8192 + 16; i++) {                   // x^(4/3) = x * cbrt(x)
        double x = i, g = x > 1 ? x / 3 : 1;
        for (int it = 0; it < 40; it++) g = (2 * g + x / (g * g)) / 3;
        iq_table[i] = (float)(x * g);
    }
    for (int i = 0; i < 256 + 200; i++) sf_table[i] = (float)xexp2((i - 200) / 4.0);
    for (int n = 0; n < LONGN / 2; n++) { twid_long_c[n] = (float)xcos(-2 * PI * n / (LONGN / 2)); twid_long_s[n] = (float)xsin(-2 * PI * n / (LONGN / 2)); }
    for (int n = 0; n < SHORTN / 2; n++) { twid_short_c[n] = (float)xcos(-2 * PI * n / (SHORTN / 2)); twid_short_s[n] = (float)xsin(-2 * PI * n / (SHORTN / 2)); }
    make_dct4_twiddles(LONGN, pre_long, post_long);
    make_dct4_twiddles(SHORTN, pre_short, post_short);
}

// ---------------------------------------------------------------- FFT (radix 2, in place)
static void fft(float* re, float* im, int n, const float* tc, const float* ts) {
    for (int i = 1, j = 0; i < n; i++) {                    // bit reversal
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int step = n / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                float wr = tc[k * step], wi = ts[k * step];
                int a = i + k, b = i + k + len / 2;
                float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
            }
    }
}

// IMDCT of M coefficients into 2M samples (AAC definition, scaled by 1/M)
static void imdct(const float* X, float* out, int M) {
    float re[LONGN / 2], im[LONGN / 2], y[LONGN];
    const float (*pre)[2] = M == LONGN ? pre_long : pre_short;
    const float (*post)[2] = M == LONGN ? post_long : post_short;
    const float* tc = M == LONGN ? twid_long_c : twid_short_c;
    const float* ts = M == LONGN ? twid_long_s : twid_short_s;
    int h = M / 2;
    for (int n = 0; n < h; n++) {
        float a = X[2 * n], b = X[M - 1 - 2 * n];
        re[n] = a * pre[n][0] - b * pre[n][1];
        im[n] = a * pre[n][1] + b * pre[n][0];
    }
    fft(re, im, h, tc, ts);
    for (int k = 0; k < h; k++) {
        float cr = re[k] * post[k][0] - im[k] * post[k][1];
        float ci = re[k] * post[k][1] + im[k] * post[k][0];
        y[2 * k] = cr;
        y[M - 1 - 2 * k] = -ci;
    }
    float s = 1.0f / M;
    for (int n = 0; n < 2 * M; n++) {
        int m = n + M / 2;
        out[n] = s * (m < M ? y[m] : m < 2 * M ? -y[2 * M - 1 - m] : -y[m - 2 * M]);
    }
}

// ---------------------------------------------------------------- bit reader
struct Bits {
    const u8* d; u32 len; u32 pos;       // pos in bits
    u32 get(int n) {
        u32 v = 0;
        while (n > 0) {
            u32 byte = pos >> 3;
            int avail = 8 - (int)(pos & 7), take = n < avail ? n : avail;
            u32 b = byte < len ? d[byte] : 0;
            v = (v << take) | ((b >> (avail - take)) & ((1u << take) - 1));
            pos += take; n -= take;
        }
        return v;
    }
    u32 bit() { return get(1); }
    void align() { pos = (pos + 7) & ~7u; }
    bool overrun() const { return pos > len * 8 + 64; }
};

// ---------------------------------------------------------------- scalefactor band tables
static const short SWB_L_96[] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 156, 172, 188, 212, 240, 276, 320, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960, 1024};
static const short SWB_L_64[] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 100, 112, 124, 140, 156, 172, 192, 216, 240, 268, 304, 344, 384, 424, 464, 504, 544, 584, 624, 664, 704, 744, 784, 824, 864, 904, 944, 984, 1024};
static const short SWB_L_48[] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292, 320, 352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 1024};
static const short SWB_L_32[] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292, 320, 352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 960, 992, 1024};
static const short SWB_L_24[] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 52, 60, 68, 76, 84, 92, 100, 108, 116, 124, 136, 148, 160, 172, 188, 204, 220, 240, 260, 284, 308, 336, 364, 396, 432, 468, 508, 552, 600, 652, 704, 768, 832, 896, 960, 1024};
static const short SWB_L_16[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 100, 112, 124, 136, 148, 160, 172, 184, 196, 212, 228, 244, 260, 280, 300, 320, 344, 368, 396, 424, 456, 492, 532, 572, 616, 664, 716, 772, 832, 896, 960, 1024};
static const short SWB_L_8[] = {0, 12, 24, 36, 48, 60, 72, 84, 96, 108, 120, 132, 144, 156, 172, 188, 204, 220, 236, 252, 268, 288, 308, 328, 348, 372, 396, 420, 448, 476, 508, 544, 580, 620, 664, 712, 764, 820, 880, 944, 1024};
static const short* const SWB_LONG[12] = {SWB_L_96, SWB_L_96, SWB_L_64, SWB_L_48, SWB_L_48, SWB_L_32, SWB_L_24, SWB_L_24, SWB_L_16, SWB_L_16, SWB_L_16, SWB_L_8};
static const u8 SWB_LONG_N[12] = {41, 41, 47, 49, 49, 51, 47, 47, 43, 43, 43, 40};
static const short SWB_S_96[] = {0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 64, 92, 128};
static const short SWB_S_48[] = {0, 4, 8, 12, 16, 20, 28, 36, 44, 56, 68, 80, 96, 112, 128};
static const short SWB_S_24[] = {0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 64, 76, 92, 108, 128};
static const short SWB_S_16[] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 60, 72, 88, 108, 128};
static const short SWB_S_8[] = {0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 60, 72, 88, 108, 128};
static const short* const SWB_SHORT[12] = {SWB_S_96, SWB_S_96, SWB_S_96, SWB_S_48, SWB_S_48, SWB_S_48, SWB_S_24, SWB_S_24, SWB_S_16, SWB_S_16, SWB_S_16, SWB_S_8};
static const u8 SWB_SHORT_N[12] = {12, 12, 12, 14, 14, 14, 15, 15, 15, 15, 15, 15};
static const u8 TNS_MAX_L[12] = {31, 31, 34, 40, 42, 51, 46, 46, 42, 42, 42, 39};
static const u8 TNS_MAX_S[12] = {9, 9, 10, 14, 14, 14, 14, 14, 14, 14, 14, 14};
static const int SAMPLE_RATES[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

static const float TNS_C_0_3[8] = {0.f, 0.43388373f, 0.78183150f, 0.97492790f, -0.98480773f, -0.86602539f, -0.64278758f, -0.34202015f};
static const float TNS_C_1_3[4] = {0.f, 0.43388373f, -0.64278758f, -0.34202015f};
static const float TNS_C_0_4[16] = {0.f, 0.20791170f, 0.40673664f, 0.58778524f, 0.74314481f, 0.86602539f, 0.95105654f, 0.99452192f,
                                    -0.99573416f, -0.96182561f, -0.89516330f, -0.79801720f, -0.67369562f, -0.52643216f, -0.36124167f, -0.18374951f};
static const float TNS_C_1_4[8] = {0.f, 0.20791170f, 0.40673664f, 0.58778524f, -0.67369562f, -0.52643216f, -0.36124167f, -0.18374951f};

// ---------------------------------------------------------------- Huffman
static int hcb_find(Bits& b, const HuffRow* t, int n) {
    int len = t[0].len;
    u32 cw = b.get(len);
    int off = 0;
    while (cw != t[off].code || len != t[off].len) {
        off++;
        if (off >= n) return -1;
        int j = t[off].len - len;
        len = t[off].len;
        cw = (cw << j) | b.get(j);
    }
    return off;
}

static const HuffRow* const BOOKS[11] = {hcb1, hcb2, hcb3, hcb4, hcb5, hcb6, hcb7, hcb8, hcb9, hcb10, hcb11};
static const int BOOK_N[11] = {81, 81, 81, 81, 81, 81, 64, 64, 169, 169, 289};
static const bool UNSIGNED_BOOK[11] = {false, false, true, true, false, false, true, true, true, true, true};

static bool spectral_symbol(Bits& b, int cb, int* out) {
    int o = hcb_find(b, BOOKS[cb - 1], BOOK_N[cb - 1]);
    if (o < 0) return false;
    const HuffRow& r = BOOKS[cb - 1][o];
    int n = cb < 5 ? 4 : 2;
    for (int i = 0; i < n; i++) out[i] = r.v[i];
    if (cb < 11) {
        if (UNSIGNED_BOOK[cb - 1]) for (int i = 0; i < n; i++) if (out[i] && b.bit()) out[i] = -out[i];
    } else {
        for (int i = 0; i < 2; i++) if (out[i] && b.bit()) out[i] = -out[i];
        for (int i = 0; i < 2; i++) {
            int a = out[i] < 0 ? -out[i] : out[i];
            if (a == 16) {
                int k = 4;
                while (b.bit()) { k++; if (k > 24) return false; }
                int v = (int)b.get(k) | (1 << k);
                out[i] = out[i] < 0 ? -v : v;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- stream structures
struct IcsInfo {
    int win_seq = 0, shape = 0, prev_shape = 0, max_sfb = 0, ngroups = 1, nwin = 1, nswb = 0;
    u8 glen[8] = {1};
    const short* swb = nullptr;
    bool eight() const { return win_seq == 2; }
};

struct Tns { u8 nfilt[8]; u8 len[8][3], order[8][3]; bool dir[8][3]; float coef[8][3][20]; };

struct Chan {
    float overlap[LONGN];
    int prev_shape = 0;
    IcsInfo info;
    int global_gain;
    u8 sfb_cb[128], sect_end[128];
    float sf[128];
    int q[LONGN];
    float spec[LONGN];
    float pcm[LONGN];
    bool tns_present;
    Tns tns;
};

struct AacDec {
    int object_type = 2, sr_index = 4, channels = 2;
    Chan ch[2];
    u32 noise_state = 0x1F2E3D4C;
    bool ms_used[128];
    int ms_mask = 0;
    bool failed = false;
    char err[64] = "";
};

static bool read_info(Bits& b, AacDec* d, IcsInfo& info, bool& pred_present) {
    b.bit();
    info.win_seq = (int)b.get(2);
    info.prev_shape = info.shape;
    info.shape = (int)b.bit();
    info.ngroups = 1;
    info.glen[0] = 1;
    pred_present = false;
    if (info.win_seq == 2) {
        info.max_sfb = (int)b.get(4);
        for (int i = 0; i < 7; i++) {
            if (b.bit()) info.glen[info.ngroups - 1]++;
            else { info.glen[info.ngroups++] = 1; }
        }
        info.nwin = 8;
        info.swb = SWB_SHORT[d->sr_index];
        info.nswb = SWB_SHORT_N[d->sr_index];
    } else {
        info.max_sfb = (int)b.get(6);
        info.nwin = 1;
        info.swb = SWB_LONG[d->sr_index];
        info.nswb = SWB_LONG_N[d->sr_index];
        pred_present = b.bit() != 0;
        if (pred_present) { snprintf(d->err, sizeof(d->err), "AAC Main profile prediction isn't supported"); return false; }
    }
    return info.max_sfb <= info.nswb;
}

static bool read_section(Bits& b, Chan& c) {
    IcsInfo& info = c.info;
    memset(c.sfb_cb, 0, sizeof(c.sfb_cb));
    memset(c.sect_end, 0, sizeof(c.sect_end));
    int bits = info.eight() ? 3 : 5, esc = (1 << bits) - 1, idx = 0;
    for (int g = 0; g < info.ngroups; g++) {
        int k = 0;
        while (k < info.max_sfb) {
            int end = k;
            int cb = (int)b.get(4);
            if (cb == 12) return false;
            int incr;
            while ((incr = (int)b.get(bits)) == esc) end += incr;
            end += incr;
            if (end > info.max_sfb || b.overrun()) return false;
            for (; k < end; k++) { if (idx >= 128) return false; c.sfb_cb[idx] = (u8)cb; c.sect_end[idx++] = (u8)end; }
        }
    }
    return true;
}

static bool read_scalefactors(Bits& b, Chan& c) {
    IcsInfo& info = c.info;
    int offset[3] = {c.global_gain, c.global_gain - 90, 0};
    bool noise_first = true;
    int idx = 0;
    for (int g = 0; g < info.ngroups; g++) {
        for (int sfb = 0; sfb < info.max_sfb;) {
            int end = c.sect_end[idx];
            int cb = c.sfb_cb[idx];
            if (cb == 0) { for (; sfb < end; sfb++, idx++) c.sf[idx] = 0; }
            else if (cb == 15 || cb == 14) {
                for (; sfb < end; sfb++, idx++) {
                    int o = hcb_find(b, hcb_sf, 121);
                    if (o < 0) return false;
                    offset[2] += hcb_sf[o].v[0] - 60;
                    int t = clamp(offset[2], -155, 100);
                    c.sf[idx] = sf_table[-t + 200];
                }
            } else if (cb == 13) {
                for (; sfb < end; sfb++, idx++) {
                    if (noise_first) { offset[1] += (int)b.get(9) - 256; noise_first = false; }
                    else { int o = hcb_find(b, hcb_sf, 121); if (o < 0) return false; offset[1] += hcb_sf[o].v[0] - 60; }
                    int t = clamp(offset[1], -100, 155);
                    c.sf[idx] = -sf_table[t + 200];
                }
            } else {
                for (; sfb < end; sfb++, idx++) {
                    int o = hcb_find(b, hcb_sf, 121);
                    if (o < 0) return false;
                    offset[0] += hcb_sf[o].v[0] - 60;
                    if (offset[0] > 255 || offset[0] < 0) return false;
                    c.sf[idx] = sf_table[offset[0] - 100 + 200];
                }
            }
            if (b.overrun()) return false;
        }
    }
    return true;
}

static bool read_tns(Bits& b, Chan& c) {
    IcsInfo& info = c.info;
    bool sh = info.eight();
    int nb = sh ? 1 : 2, lb = sh ? 4 : 6, ob = sh ? 3 : 5;
    for (int w = 0; w < info.nwin; w++) {
        c.tns.nfilt[w] = (u8)b.get(nb);
        if (!c.tns.nfilt[w]) continue;
        int res = (int)b.bit();
        for (int f = 0; f < c.tns.nfilt[w]; f++) {
            c.tns.len[w][f] = (u8)b.get(lb);
            c.tns.order[w][f] = (u8)b.get(ob);
            if (c.tns.order[w][f] > 20) return false;
            if (c.tns.order[w][f]) {
                c.tns.dir[w][f] = b.bit() != 0;
                int compress = (int)b.bit();
                int clen = res + 3 - compress;
                for (int i = 0; i < c.tns.order[w][f]; i++) {
                    u32 v = b.get(clen);
                    int sel = 2 * compress + res;            // 0: (0,3) 1: (0,4) 2: (1,3) 3: (1,4) as in the tables
                    c.tns.coef[w][f][i] = sel == 0 ? TNS_C_0_3[v] : sel == 1 ? TNS_C_0_4[v] : sel == 2 ? TNS_C_1_3[v] : TNS_C_1_4[v];
                }
            }
        }
    }
    return true;
}

static bool read_spectrum(Bits& b, AacDec* d, Chan& c, bool pulse, int pulse_n, int pulse_pos[4], int pulse_amp[4]) {
    IcsInfo& info = c.info;
    memset(c.q, 0, sizeof(c.q));
    int group_off = 0, idx = 0;
    for (int g = 0; g < info.ngroups; g++) {
        int glen = info.glen[g];
        for (int sfb = 0; sfb < info.max_sfb; sfb++, idx++) {
            int cb = c.sfb_cb[idx];
            int off0 = group_off + info.swb[sfb], width = info.swb[sfb + 1] - info.swb[sfb];
            if (cb == 0 || cb >= 13) continue;
            int num = cb >= 5 ? 2 : 4;
            for (int w = 0; w < glen; w++) {
                int off = off0 + w * 128;
                for (int k = 0; k < width; k += num) {
                    int v[4];
                    if (!spectral_symbol(b, cb, v) || b.overrun()) return false;
                    for (int j = 0; j < num; j++) if (off + k + j < LONGN) c.q[off + k + j] = v[j];
                }
            }
        }
        group_off += glen * 128;
    }
    if (pulse) for (int i = 0; i < pulse_n; i++) {
        int p = pulse_pos[i];
        if (p >= 0 && p < LONGN) c.q[p] += c.q[p] > 0 ? pulse_amp[i] : -pulse_amp[i];
    }
    // inverse quantisation, scalefactors and PNS
    group_off = 0; idx = 0;
    memset(c.spec, 0, sizeof(c.spec));
    for (int g = 0; g < info.ngroups; g++) {
        int glen = info.glen[g];
        for (int sfb = 0; sfb < info.max_sfb; sfb++, idx++) {
            int cb = c.sfb_cb[idx];
            int off0 = group_off + info.swb[sfb], width = info.swb[sfb + 1] - info.swb[sfb];
            for (int w = 0; w < glen; w++) {
                int off = off0 + w * 128;
                if (cb == 13) {                                   // perceptual noise substitution
                    float energy = 0;
                    for (int k = 0; k < width; k++) {
                        d->noise_state = 1664525u * d->noise_state + 1013904223u;
                        c.spec[off + k] = (float)(i32)d->noise_state;
                        energy += c.spec[off + k] * c.spec[off + k];
                    }
                    float scale = energy > 0 ? (float)(c.sf[idx] / xsqrt(energy)) : 0;
                    for (int k = 0; k < width; k++) c.spec[off + k] *= scale;
                } else if (cb != 0 && cb < 13) {
                    for (int k = 0; k < width; k++) {
                        int qv = c.q[off + k];
                        int a = qv < 0 ? -qv : qv;
                        float m = a < 8192 + 16 ? iq_table[a] : 0;
                        c.spec[off + k] = (qv < 0 ? -m : m) * c.sf[idx];
                    }
                }
            }
        }
        group_off += glen * 128;
    }
    return true;
}

static bool read_ics(Bits& b, AacDec* d, Chan& c, bool common_window) {
    c.global_gain = (int)b.get(8);
    bool pred;
    if (!common_window && !read_info(b, d, c.info, pred)) return false;
    if (!read_section(b, c)) return false;
    if (!read_scalefactors(b, c)) return false;
    bool pulse = b.bit() != 0;
    int pn = 0, ppos[4] = {0}, pamp[4] = {0};
    if (pulse) {
        if (c.info.eight()) return false;
        pn = (int)b.get(2) + 1;
        int start = (int)b.get(6);
        if (start >= c.info.nswb) return false;
        int pos = c.info.swb[start];
        for (int i = 0; i < pn; i++) { pos += (int)b.get(5); ppos[i] = pos; pamp[i] = (int)b.get(4); }
    }
    c.tns_present = b.bit() != 0;
    if (c.tns_present && !read_tns(b, c)) return false;
    if (b.bit()) { snprintf(d->err, sizeof(d->err), "gain control isn't supported"); return false; }
#ifdef AAC_DEBUG
    { int n13 = 0, n14 = 0; for (int i = 0; i < 128; i++) { if (c.sfb_cb[i] == 13) n13++; if (c.sfb_cb[i] >= 14) n14++; } if (n13 || n14) fprintf(stderr, "  pns %d intensity %d pulse %d\n", n13, n14, pulse); }
#endif
    return read_spectrum(b, d, c, pulse, pn, ppos, pamp);
}

static void apply_tns(AacDec* d, Chan& c) {
    IcsInfo& info = c.info;
    bool sh = info.eight();
    int max_tns = sh ? TNS_MAX_S[d->sr_index] : TNS_MAX_L[d->sr_index];
    for (int w = 0; w < info.nwin; w++) {
        int bottom = info.nswb;
        for (int f = 0; f < c.tns.nfilt[w]; f++) {
            int top = bottom;
            bottom = max(top - (int)c.tns.len[w][f], 0);
            int start = min(min(bottom, max_tns), info.max_sfb), end = min(min(top, max_tns), info.max_sfb);
            int s = info.swb[start], e = info.swb[end];
            int order = c.tns.order[w][f];
            if (!order || e <= s) continue;
            // reflection coefficients to direct-form LPC coefficients (step-up)
            float a[21], tmp[21];
            a[0] = 1;
            for (int m = 1; m <= order; m++) {
                float k = c.tns.coef[w][f][m - 1];
                for (int i = 1; i < m; i++) tmp[i] = a[i] + k * a[m - i];
                for (int i = 1; i < m; i++) a[i] = tmp[i];
                a[m] = k;
            }
            float* x = c.spec + w * 128;
            if (!c.tns.dir[w][f]) {
                for (int i = s; i < e; i++) { float sum = 0; for (int j = 1; j <= order && i - j >= s; j++) sum += a[j] * x[i - j]; x[i] -= sum; }
            } else {
                for (int i = e - 1; i >= s; i--) { float sum = 0; for (int j = 1; j <= order && i + j < e; j++) sum += a[j] * x[i + j]; x[i] -= sum; }
            }
        }
    }
}

// ---------------------------------------------------------------- filterbank (after JAAD's FilterBank)
static void filterbank(Chan& c) {
    IcsInfo& info = c.info;
    const float* LW[2] = {sine_long, kbd_long};
    const float* SW[2] = {sine_short, kbd_short};
    const int length = LONGN, shortLen = SHORTN, mid = (length - shortLen) / 2, trans = shortLen / 2;
    int shape = info.shape, prev = c.prev_shape;
    float buf[2 * LONGN], *ov = c.overlap, *out = c.pcm;
    int i;
    switch (info.win_seq) {
    case 0:
        imdct(c.spec, buf, length);
        for (i = 0; i < length; i++) out[i] = ov[i] + buf[i] * LW[prev][i];
        for (i = 0; i < length; i++) ov[i] = buf[length + i] * LW[shape][length - 1 - i];
        break;
    case 1:
        imdct(c.spec, buf, length);
        for (i = 0; i < length; i++) out[i] = ov[i] + buf[i] * LW[prev][i];
        for (i = 0; i < mid; i++) ov[i] = buf[length + i];
        for (i = 0; i < shortLen; i++) ov[mid + i] = buf[length + mid + i] * SW[shape][shortLen - i - 1];
        for (i = 0; i < mid; i++) ov[mid + shortLen + i] = 0;
        break;
    case 2:
        for (i = 0; i < 8; i++) imdct(c.spec + i * shortLen, buf + 2 * i * shortLen, shortLen);
        for (i = 0; i < mid; i++) out[i] = ov[i];
        for (i = 0; i < shortLen; i++) {
            out[mid + i] = ov[mid + i] + buf[i] * SW[prev][i];
            out[mid + 1 * shortLen + i] = ov[mid + shortLen * 1 + i] + buf[shortLen * 1 + i] * SW[shape][shortLen - 1 - i] + buf[shortLen * 2 + i] * SW[shape][i];
            out[mid + 2 * shortLen + i] = ov[mid + shortLen * 2 + i] + buf[shortLen * 3 + i] * SW[shape][shortLen - 1 - i] + buf[shortLen * 4 + i] * SW[shape][i];
            out[mid + 3 * shortLen + i] = ov[mid + shortLen * 3 + i] + buf[shortLen * 5 + i] * SW[shape][shortLen - 1 - i] + buf[shortLen * 6 + i] * SW[shape][i];
            if (i < trans) out[mid + 4 * shortLen + i] = ov[mid + shortLen * 4 + i] + buf[shortLen * 7 + i] * SW[shape][shortLen - 1 - i] + buf[shortLen * 8 + i] * SW[shape][i];
        }
        for (i = 0; i < shortLen; i++) {
            if (i >= trans) ov[mid + 4 * shortLen + i - length] = buf[shortLen * 7 + i] * SW[shape][shortLen - 1 - i] + buf[shortLen * 8 + i] * SW[shape][i];
            ov[mid + 5 * shortLen + i - length] = buf[shortLen * 9 + i] * SW[shape][shortLen - 1 - i] + buf[shortLen * 10 + i] * SW[shape][i];
            ov[mid + 6 * shortLen + i - length] = buf[shortLen * 11 + i] * SW[shape][shortLen - 1 - i] + buf[shortLen * 12 + i] * SW[shape][i];
            ov[mid + 7 * shortLen + i - length] = buf[shortLen * 13 + i] * SW[shape][shortLen - 1 - i] + buf[shortLen * 14 + i] * SW[shape][i];
            ov[mid + 8 * shortLen + i - length] = buf[shortLen * 15 + i] * SW[shape][shortLen - 1 - i];
        }
        for (i = 0; i < mid; i++) ov[mid + shortLen + i] = 0;
        break;
    case 3:
        imdct(c.spec, buf, length);
        for (i = 0; i < mid; i++) out[i] = ov[i];
        for (i = 0; i < shortLen; i++) out[mid + i] = ov[mid + i] + buf[mid + i] * SW[prev][i];
        for (i = 0; i < mid; i++) out[mid + shortLen + i] = ov[mid + shortLen + i] + buf[mid + shortLen + i];
        for (i = 0; i < length; i++) ov[i] = buf[length + i] * LW[shape][length - 1 - i];
        break;
    }
    c.prev_shape = shape;
}

// ---------------------------------------------------------------- one frame
AacDec* aac_open(int object_type, int sr_index, int channels, char* err, int errlen) {
    init_tables();
    if (object_type != 2 && object_type != 5 && object_type != 29) { snprintf(err, errlen, "AAC object type %d isn't supported (only AAC-LC)", object_type); return nullptr; }
    if (sr_index < 0 || sr_index > 12) { snprintf(err, errlen, "bad sample rate"); return nullptr; }
    if (channels < 1 || channels > 2) { snprintf(err, errlen, "%d channel AAC isn't supported (mono and stereo only)", channels); return nullptr; }
    AacDec* d = new AacDec();
    d->object_type = 2;
    d->sr_index = sr_index;
    d->channels = channels;
    return d;
}
void aac_close(AacDec* d) { delete d; }
int aac_rate(AacDec* d) { return SAMPLE_RATES[d->sr_index]; }

// decodes one raw_data_block into 1024 stereo frames; false on a bad frame
bool aac_decode(AacDec* d, const u8* data, u32 len, i16* out) {
    Bits b = {data, len, 0};
    int chans_done = 0;
    bool got_pcm = false;
    d->err[0] = 0;
    for (;;) {
        int type = (int)b.get(3);
        if (type == 7) break;
        if (b.overrun()) return false;
        if (type == 0 || type == 3) {                        // SCE, LFE
            b.get(4);
            if (chans_done >= 2) return false;
            Chan& c = d->ch[chans_done];
            if (!read_ics(b, d, c, false)) return false;
            if (c.tns_present) apply_tns(d, c);
            filterbank(c);
            chans_done++;
            got_pcm = true;
        } else if (type == 1) {                              // CPE
            b.get(4);
            bool common = b.bit() != 0;
            Chan& l = d->ch[0];
            Chan& r = d->ch[1];
            d->ms_mask = 0;
            memset(d->ms_used, 0, sizeof(d->ms_used));
            if (common) {
                bool pred;
                if (!read_info(b, d, l.info, pred)) return false;
                int pl = r.info.shape;
                r.info = l.info;
                r.info.prev_shape = pl;
                d->ms_mask = (int)b.get(2);
                if (d->ms_mask == 1) {
                    for (int i = 0; i < l.info.ngroups * l.info.max_sfb && i < 128; i++) d->ms_used[i] = b.bit() != 0;
                } else if (d->ms_mask == 2) memset(d->ms_used, 1, sizeof(d->ms_used));
                else if (d->ms_mask == 3) return false;
            }
            if (!read_ics(b, d, l, common) || !read_ics(b, d, r, common)) return false;
#ifdef AAC_DEBUG
            { static int fno = 0; fprintf(stderr, "f%d L seq%d ng%d g[%d %d %d %d] maxsfb%d tns%d R seq%d ng%d tns%d ms%d common%d\n", fno++, l.info.win_seq, l.info.ngroups, l.info.glen[0], l.info.glen[1], l.info.glen[2], l.info.glen[3], l.info.max_sfb, l.tns_present, r.info.win_seq, r.info.ngroups, r.tns_present, d->ms_mask, common); }
#endif
            // mid/side, then intensity
            IcsInfo& info = l.info;
            int g_off = 0, idx = 0;
            for (int g = 0; g < info.ngroups; g++) {
                for (int s = 0; s < info.max_sfb; s++, idx++) {
                    if (d->ms_used[idx] && l.sfb_cb[idx] < 13 && r.sfb_cb[idx] < 13)
                        for (int w = 0; w < info.glen[g]; w++) {
                            int off = g_off + w * 128 + info.swb[s];
                            for (int j = 0; j < info.swb[s + 1] - info.swb[s]; j++) {
                                float t = l.spec[off + j] - r.spec[off + j];
                                l.spec[off + j] += r.spec[off + j];
                                r.spec[off + j] = t;
                            }
                        }
                }
                g_off += info.glen[g] * 128;
            }
            g_off = 0; idx = 0;
            for (int g = 0; g < info.ngroups; g++) {
                for (int s = 0; s < info.max_sfb; s++, idx++) {
                    int cb = r.sfb_cb[idx];
                    if (cb != 14 && cb != 15) continue;
                    int sign = cb == 15 ? 1 : -1;
                    if (d->ms_mask) sign *= d->ms_used[idx] ? -1 : 1;
                    float scale = sign * r.sf[idx];
                    for (int w = 0; w < info.glen[g]; w++) {
                        int off = g_off + w * 128 + info.swb[s];
                        for (int j = 0; j < info.swb[s + 1] - info.swb[s]; j++) r.spec[off + j] = l.spec[off + j] * scale;
                    }
                }
                g_off += info.glen[g] * 128;
            }
            if (l.tns_present) apply_tns(d, l);
            if (r.tns_present) apply_tns(d, r);
            filterbank(l);
            filterbank(r);
            chans_done = 2;
            got_pcm = true;
        } else if (type == 4) {                              // DSE
            b.get(4);
            int align = (int)b.bit();
            int cnt = (int)b.get(8);
            if (cnt == 255) cnt += (int)b.get(8);
            if (align) b.align();
            b.pos += cnt * 8;
        } else if (type == 6) {                              // FIL
            int cnt = (int)b.get(4);
            if (cnt == 15) cnt += (int)b.get(8) - 1;
            b.pos += cnt * 8;                                // SBR / dynamic range data is skipped
        } else if (type == 5) {                              // PCE
            b.get(4); b.get(2); b.get(4);
            int nfront = (int)b.get(4), nside = (int)b.get(4), nback = (int)b.get(4), nlfe = (int)b.get(2), nassoc = (int)b.get(3), ncc = (int)b.get(4);
            if (b.bit()) b.get(4);
            if (b.bit()) b.get(4);
            if (b.bit()) b.get(3);
            b.pos += (nfront + nside + nback) * 5 + nlfe * 4 + nassoc * 4 + ncc * 5;
            b.align();
            int cm = (int)b.get(8);
            b.pos += cm * 8;
        } else return false;                                  // CCE and anything else
    }
    if (!got_pcm) return false;
    const float K = 1.0f;
    for (int i = 0; i < LONGN; i++) {
        float l = d->ch[0].pcm[i] * K, r = (chans_done > 1 ? d->ch[1].pcm[i] : d->ch[0].pcm[i]) * K;
        out[i * 2] = (i16)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[i * 2 + 1] = (i16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    return true;
}
