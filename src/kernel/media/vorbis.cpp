// silicon - Ogg Vorbis through stb_vorbis (public domain). The decoder wants a C library; this file
// provides the few pieces it needs (math on the x87 unit, qsort, a fixed memory arena).
#include "../core.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_CRT
#define assert(x) ((void)0)
#define alloca __builtin_alloca

static double floor(double x) {
    if (x > 4.5e15 || x < -4.5e15) return x;
    long long i = (long long)x;
    double r = (double)i;
    return r > x ? r - 1 : r;
}
static double ceil(double x) { double f = floor(x); return f < x ? f + 1 : f; }
static double sin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static double cos(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
static double log(double x) { double r; __asm__("fldln2; fxch; fyl2x" : "=t"(r) : "0"(x) : "st(1)"); return r; }
static double exp2d(double x) {                   // 2^x
    double r;
    double fl = floor(x), fr = x - fl;
    __asm__("f2xm1" : "=t"(r) : "0"(fr));
    r += 1.0;
    __asm__("fscale" : "+t"(r) : "u"(fl));
    return r;
}
static double exp(double x) { return exp2d(x * 1.4426950408889634); }
static double pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 0) return 0;
    if (x < 0) { double yi = floor(y); if (yi != y) return 0; double p = exp2d(y * (log(-x) * 1.4426950408889634)); return ((long long)yi & 1) ? -p : p; }
    return exp2d(y * (log(x) * 1.4426950408889634));
}
static double ldexp(double x, int e) { return x * exp2d((double)e); }
static int abs(int v) { return v < 0 ? -v : v; }

static void qsort(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*)) {
    u8* b = (u8*)base;
    u8 tmp[64];
    if (sz > sizeof(tmp)) return;
    // heap sort: no recursion, no extra memory
    auto sift = [&](size_t start, size_t end) {
        size_t root = start;
        while (root * 2 + 1 < end) {
            size_t child = root * 2 + 1;
            if (child + 1 < end && cmp(b + child * sz, b + (child + 1) * sz) < 0) child++;
            if (cmp(b + root * sz, b + child * sz) < 0) {
                memcpy(tmp, b + root * sz, sz); memcpy(b + root * sz, b + child * sz, sz); memcpy(b + child * sz, tmp, sz);
                root = child;
            } else return;
        }
    };
    for (size_t s = n / 2; s > 0; s--) sift(s - 1, n);
    for (size_t e = n; e > 1; e--) {
        memcpy(tmp, b, sz); memcpy(b, b + (e - 1) * sz, sz); memcpy(b + (e - 1) * sz, tmp, sz);
        sift(0, e - 1);
    }
}

#include "third_party/stb_vorbis.c"

struct VorbisStream {
    stb_vorbis* v;
    u8* arena;
    int rate;
    u64 frames;
};

void* vorbis_open(const u8* data, u32 len, int* rate, u64* frames) {
    VorbisStream* s = new VorbisStream();
    const int ARENA = 6 << 20;
    s->arena = (u8*)kmalloc_try(ARENA);
    if (!s->arena) { delete s; return nullptr; }
    stb_vorbis_alloc al;
    al.alloc_buffer = (char*)s->arena;
    al.alloc_buffer_length_in_bytes = ARENA;
    int err = 0;
    s->v = stb_vorbis_open_memory(data, (int)len, &err, &al);
    if (!s->v) { kfree(s->arena); delete s; return nullptr; }
    stb_vorbis_info info = stb_vorbis_get_info(s->v);
    s->rate = (int)info.sample_rate;
    s->frames = stb_vorbis_stream_length_in_samples(s->v);
    *rate = s->rate;
    *frames = s->frames;
    return s;
}

int vorbis_read(void* h, i16* out, int n) {          // stereo frames
    VorbisStream* s = (VorbisStream*)h;
    int got = 0;
    while (got < n) {
        int k = stb_vorbis_get_samples_short_interleaved(s->v, 2, out + got * 2, (n - got) * 2);
        if (k <= 0) break;
        got += k;
    }
    return got;
}

void vorbis_seek(void* h, u64 frame) { stb_vorbis_seek(((VorbisStream*)h)->v, (unsigned int)frame); }

void vorbis_close(void* h) {
    VorbisStream* s = (VorbisStream*)h;
    if (!s) return;
    kfree(s->arena);
    delete s;
}
