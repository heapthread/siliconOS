// silicon - PDF: image XObjects and inline images decoded to ARGB
#include "pdf_gfx.h"

static inline u32 getbits(const u8* row, u32 idx, int bpc) {
    switch (bpc) {
    case 8: return row[idx];
    case 16: return (row[idx * 2] << 8) | row[idx * 2 + 1];
    case 1: return (row[idx >> 3] >> (7 - (idx & 7))) & 1;
    case 2: return (row[idx >> 2] >> (6 - 2 * (idx & 3))) & 3;
    case 4: return (row[idx >> 1] >> (4 - 4 * (idx & 1))) & 15;
    default: return 0;
    }
}

struct RawImg {
    u8* d = nullptr;            // owned
    u32 len = 0;
    char imgf[8] = "";
};

static void load_raw(PdfDoc* d, Obj* dict, const u8* inl, u32 inl_len, RawImg& r) {
    if (inl) {
        u8* copy = (u8*)kmalloc(inl_len + 1);
        memcpy(copy, inl, inl_len);
        r.d = pdf_apply_filters(d, dict, copy, inl_len, &r.len, r.imgf);
    } else {
        r.d = pdf_stream_decode(d, dict, &r.len, r.imgf);
    }
}

// gray alpha plane of a mask / soft mask stream; returns false when unusable
static bool decode_plane(PdfDoc* d, Obj* m, u8** out, int* mw, int* mh, bool stencil) {
    if (!m || m->type != O_STREAM) return false;
    int w = (int)onum(dget2(d, m, "Width", "W"), 0), h = (int)onum(dget2(d, m, "Height", "H"), 0);
    if (w <= 0 || h <= 0 || (u64)w * h > 40000000ull) return false;
    int bpc = (int)onum(dget2(d, m, "BitsPerComponent", "BPC"), stencil ? 1 : 8);
    Obj* imo = dget2(d, m, "ImageMask", "IM");
    if (imo && imo->type == O_BOOL && imo->b) { stencil = true; bpc = 1; }
    RawImg r;
    load_raw(d, m, nullptr, 0, r);
    u8* pl = (u8*)kmalloc((size_t)w * h);
    bool ok = true;
    if (!strcmp(r.imgf, "DCT")) {
        Image im;
        if (!image_load(r.d, r.len, im)) ok = false;
        else {
            if (im.w != w || im.h != h) { w = im.w; h = im.h; kfree(pl); pl = (u8*)kmalloc((size_t)w * h); }
            for (int i = 0; i < w * h; i++) pl[i] = (u8)((im.px[i] >> 8) & 255);
            kfree(im.px);
        }
    } else if (!strcmp(r.imgf, "CCF")) {
        Obj* parms = dget(d, m, "DecodeParms");
        if (parms && parms->type == O_ARR) parms = aget(d, parms, 0);
        int k = (int)onum(dget(d, parms, "K"), 0), cols = (int)onum(dget(d, parms, "Columns"), 1728);
        Obj* bi = dget(d, parms, "BlackIs1");
        Obj* ba = dget(d, parms, "EncodedByteAlign");
        u32 ol;
        u8* bits = ccitt_decode(r.d, r.len, k, cols, h, bi && bi->type == O_BOOL && bi->b, ba && ba->type == O_BOOL && ba->b, &ol);
        if (!bits) ok = false;
        else {
            u32 rb = ((u32)cols + 7) / 8;
            Obj* dec = dget(d, m, "Decode");
            bool inv = dec && onum(aget(d, dec, 0), 0) == 1;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    u32 v = (u32)y * rb < ol ? getbits(bits + (size_t)y * rb, (u32)x, 1) : 1;
                    bool paint = stencil ? ((v == 0) != inv) : false;
                    pl[(size_t)y * w + x] = stencil ? (paint ? 255 : 0) : (u8)(v ? 255 : 0);
                }
            kfree(bits);
        }
    } else if (r.imgf[0]) {
        ok = false;
    } else {
        Obj* dec = dget(d, m, "Decode");
        double d0 = dec ? onum(aget(d, dec, 0), 0) : 0, d1 = dec ? onum(aget(d, dec, 1), 1) : 1;
        u32 rb = ((u32)w * bpc + 7) / 8;
        double maxv = bpc == 16 ? 65535.0 : (double)((1 << bpc) - 1);
        for (int y = 0; y < h; y++) {
            const u8* row = r.d + (size_t)y * rb;
            bool have = (size_t)(y + 1) * rb <= r.len;
            for (int x = 0; x < w; x++) {
                if (!have) { pl[(size_t)y * w + x] = stencil ? 0 : 255; continue; }
                u32 v = getbits(row, (u32)x, bpc);
                if (stencil) {
                    bool paint = d0 == 0 ? v == 0 : v != 0;                // ImageMask semantics
                    pl[(size_t)y * w + x] = paint ? 255 : 0;
                } else {
                    double f = d0 + v / maxv * (d1 - d0);
                    pl[(size_t)y * w + x] = (u8)(f <= 0 ? 0 : f >= 1 ? 255 : f * 255 + 0.5);
                }
            }
        }
    }
    kfree(r.d);
    if (!ok) { kfree(pl); return false; }
    *out = pl; *mw = w; *mh = h;
    return true;
}

bool pdf_decode_image(PdfDoc* d, Obj* dict, const u8* inl, u32 inl_len, Obj* res, u32 fill_rgb, Image* out) {
    out->w = out->h = 0;
    out->px = nullptr;
    int w = (int)onum(dget2(d, dict, "Width", "W"), 0), h = (int)onum(dget2(d, dict, "Height", "H"), 0);
    if (w <= 0 || h <= 0 || (u64)w * h > 400000000ull) return false;
    Obj* imo = dget2(d, dict, "ImageMask", "IM");
    bool imask = imo && imo->type == O_BOOL && imo->b;
    int bpc = imask ? 1 : (int)onum(dget2(d, dict, "BitsPerComponent", "BPC"), 8);
    RawImg r;
    load_raw(d, dict, inl, inl_len, r);
    bool is_dct = !strcmp(r.imgf, "DCT");
    // ---- image soft mask / stencil mask / colour key
    Obj* sm = inl ? nullptr : dget(d, dict, "SMask");
    Obj* mk = inl ? nullptr : dget(d, dict, "Mask");
    u8* plane = nullptr;
    int pw = 0, ph = 0;
    bool plane_is_soft = false;
    if (sm) { if (decode_plane(d, sm, &plane, &pw, &ph, false)) plane_is_soft = true; }
    else if (mk && mk->type == O_STREAM) { if (decode_plane(d, mk, &plane, &pw, &ph, true)) plane_is_soft = false; }
    (void)plane_is_soft;
    int key[16];
    bool has_key = false;
    if (mk && mk->type == O_ARR && !plane) {
        for (u32 i = 0; i < mk->arr.n && i < 16; i++) key[i] = (int)onum(aget(d, mk, i), 0);
        has_key = true;
    }
    // ---- the colour samples
    Image img;
    int sw = w, sh = h;                          // size of the decoded bitmap
    bool ok = true;
    if (is_dct) {
        if (!image_load(r.d, r.len, img)) ok = false;
        else {
            sw = img.w; sh = img.h;
            // CMYK JPEGs (Adobe) come out inverted from some decoders: leave as decoded
        }
    } else if (!strcmp(r.imgf, "CCF") || !r.imgf[0]) {
        u8* bits = r.d;
        u32 blen = r.len;
        u8* own = nullptr;
        if (!strcmp(r.imgf, "CCF")) {
            Obj* parms = dget2(d, dict, "DecodeParms", "DP");
            if (parms && parms->type == O_ARR) parms = aget(d, parms, 0);
            int k = (int)onum(dget(d, parms, "K"), 0), cols = (int)onum(dget(d, parms, "Columns"), 1728);
            Obj* bi = dget(d, parms, "BlackIs1");
            Obj* ba = dget(d, parms, "EncodedByteAlign");
            own = ccitt_decode(r.d, r.len, k, cols, h, bi && bi->type == O_BOOL && bi->b, ba && ba->type == O_BOOL && ba->b, &blen);
            if (!own) ok = false;
            bits = own;
            bpc = 1;
            if (cols != w) { /* rows are `cols` wide; use that for the row stride */ w = min(w, cols); }
        }
        if (ok) {
            ColorSpace* cs = imask ? cs_device(1) : cs_parse(d, dget2(d, dict, "ColorSpace", "CS"), res);
            if (cs->kind == CS_PATTERN) cs = cs_device(1);
            int nc = max(1, cs->n);
            if (nc > 8) nc = 8;
            int step = 1;
            while ((u64)(w / step) * (h / step) > 12000000ull) step++;
            sw = (w + step - 1) / step; sh = (h + step - 1) / step;
            u32 stride_w = !strcmp(r.imgf, "CCF") ? (u32)max(1, (int)onum(dget(d, dget2(d, dict, "DecodeParms", "DP"), "Columns"), 1728)) : (u32)w;
            u32 rb = (stride_w * nc * bpc + 7) / 8;
            img.w = sw; img.h = sh;
            img.px = (u32*)kmalloc((size_t)sw * sh * 4);
            double maxv = bpc == 16 ? 65535.0 : (double)((1 << bpc) - 1);
            Obj* dec = dget2(d, dict, "Decode", "D");
            double dmin[8], dmax[8];
            for (int i = 0; i < nc; i++) {
                bool indexed = cs->kind == CS_INDEXED;
                dmin[i] = dec && dec->type == O_ARR ? onum(aget(d, dec, 2 * i), 0) : 0;
                dmax[i] = dec && dec->type == O_ARR ? onum(aget(d, dec, 2 * i + 1), indexed ? maxv : 1) : (indexed ? maxv : 1);
                if (cs->kind == CS_LAB && !(dec && dec->type == O_ARR)) {
                    if (i == 0) { dmin[i] = 0; dmax[i] = 100; }
                    else { dmin[i] = cs->rng[2 * (i - 1)]; dmax[i] = cs->rng[2 * (i - 1) + 1]; }
                }
            }
            bool default_dec = true;
            for (int i = 0; i < nc; i++) if (dmin[i] != 0 || dmax[i] != (cs->kind == CS_INDEXED ? maxv : 1)) default_dec = false;
            u32 lut[256];
            bool use_lut = nc == 1 && bpc <= 8;
            if (use_lut && !imask) {
                for (int v = 0; v <= (int)maxv; v++) {
                    double c[8] = {dmin[0] + v / maxv * (dmax[0] - dmin[0])};
                    if (cs->kind == CS_INDEXED) c[0] = dmin[0] + v * (dmax[0] - dmin[0]) / maxv;
                    lut[v] = cs_rgb(cs, c);
                }
            }
            bool fast_rgb = cs->kind == CS_RGB && bpc == 8 && default_dec && !has_key;
            for (int y = 0; y < sh; y++) {
                int sy = y * step;
                bool have = (size_t)(sy + 1) * rb <= blen;
                const u8* row = have ? bits + (size_t)sy * rb : nullptr;
                u32* o = img.px + (size_t)y * sw;
                for (int x = 0; x < sw; x++) {
                    int sx = x * step;
                    if (!row) { o[x] = 0; continue; }
                    if (imask) {
                        u32 v = getbits(row, (u32)sx, 1);
                        double d0 = dec && dec->type == O_ARR ? onum(aget(d, dec, 0), 0) : 0;
                        bool paint = d0 == 0 ? v == 0 : v != 0;
                        o[x] = paint ? (fill_rgb | 0xFF000000) : 0;
                        continue;
                    }
                    if (fast_rgb) { const u8* p = row + sx * 3; o[x] = 0xFF000000 | (p[0] << 16) | (p[1] << 8) | p[2]; continue; }
                    u32 smp[8];
                    for (int c = 0; c < nc; c++) smp[c] = getbits(row, (u32)sx * nc + c, bpc);
                    if (has_key) {
                        bool all = true;
                        for (int c = 0; c < nc; c++) if ((int)smp[c] < key[2 * c] || (int)smp[c] > key[2 * c + 1]) all = false;
                        if (all) { o[x] = 0; continue; }
                    }
                    if (use_lut) { o[x] = lut[smp[0] > maxv ? 0 : smp[0]]; continue; }
                    double c[8];
                    for (int i = 0; i < nc; i++) c[i] = dmin[i] + smp[i] / maxv * (dmax[i] - dmin[i]);
                    o[x] = cs_rgb(cs, c);
                }
            }
        }
        if (own) kfree(own);
    } else {
        ok = false;                                  // JPX, JBIG2: not supported
    }
    kfree(r.d);
    if (!ok) { if (plane) kfree(plane); if (img.px) kfree(img.px); return false; }
    if (is_dct && imask) {                           // a stencil that came as JPEG: use luminance
        for (int i = 0; i < sw * sh; i++) img.px[i] = (((img.px[i] >> 8) & 255) < 128) ? (fill_rgb | 0xFF000000) : 0;
    }
    // ---- merge the alpha plane
    if (plane) {
        int ow = max(sw, pw), oh = max(sh, ph);
        if ((u64)ow * oh > 16000000ull) { ow = sw; oh = sh; }
        u32* np = (u32*)kmalloc((size_t)ow * oh * 4);
        for (int y = 0; y < oh; y++) {
            int iy = (int)((i64)y * sh / oh), my = (int)((i64)y * ph / oh);
            for (int x = 0; x < ow; x++) {
                int ix = (int)((i64)x * sw / ow), mx = (int)((i64)x * pw / ow);
                u32 c = img.px[(size_t)iy * sw + ix];
                u32 a = plane[(size_t)my * pw + mx];
                a = a * (c >> 24) / 255;
                np[(size_t)y * ow + x] = (c & 0x00FFFFFF) | (a << 24);
            }
        }
        kfree(img.px);
        kfree(plane);
        img.px = np; sw = ow; sh = oh;
    }
    img.w = sw; img.h = sh;
    *out = img;
    return true;
}
