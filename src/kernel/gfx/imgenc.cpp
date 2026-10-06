// silicon - image encoding: 24-bit BMP files
#include "gfx.h"

static void le32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

u8* image_encode_bmp(const Image& img, u32* len) {
    int w = img.w, h = img.h;
    if (!img.px || w <= 0 || h <= 0) return nullptr;
    u32 row = (u32)((w * 3 + 3) & ~3);
    u32 size = 54 + row * h;
    u8* buf = (u8*)kmalloc_try(size);
    if (!buf) return nullptr;
    memset(buf, 0, 54);
    buf[0] = 'B'; buf[1] = 'M';
    le32(buf + 2, size); le32(buf + 10, 54); le32(buf + 14, 40);
    le32(buf + 18, w); le32(buf + 22, h); buf[26] = 1; buf[28] = 24;
    le32(buf + 34, row * h);
    for (int y = 0; y < h; y++) {
        u8* d = buf + 54 + (u32)(h - 1 - y) * row;
        const u32* s = img.px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            u32 p = s[x];
            u32 a = p >> 24;
            // flatten transparency onto white
            u32 r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (a < 255) { r = (r * a + 255 * (255 - a)) / 255; g = (g * a + 255 * (255 - a)) / 255; b = (b * a + 255 * (255 - a)) / 255; }
            d[x * 3] = (u8)b; d[x * 3 + 1] = (u8)g; d[x * 3 + 2] = (u8)r;
        }
        for (u32 k = (u32)w * 3; k < row; k++) d[k] = 0;
    }
    *len = size;
    return buf;
}
