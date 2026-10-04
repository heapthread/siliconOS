// silicon - PDF graphics internals: rasterizer, colour spaces, functions, clip masks
#pragma once
#include "pdf_int.h"

struct IRect { int x0, y0, x1, y1; };
static inline IRect irect_isect(IRect a, IRect b) { return {max(a.x0, b.x0), max(a.y0, b.y0), min(a.x1, b.x1), min(a.y1, b.y1)}; }
static inline bool irect_empty(IRect a) { return a.x1 <= a.x0 || a.y1 <= a.y0; }

// ---- rasterizer (pdf_raster.cpp)
struct Sink {
    virtual void row(int y, int x0, int x1, const u8* cov) = 0;     // cov[i] is the coverage of pixel x0 + i
    virtual ~Sink() {}
};
// scan converts a path given in device space; every covered row is delivered to the sink
void raster_fill(const Path& p, bool evenodd, IRect clip, Sink& sink);
// turns a path (device space) into polygons that fill (non-zero) to the stroke
void stroke_path(const Path& p, double width, int cap, int join, double miter, const double* dash, int ndash, double phase, Path& out);
// flattened polylines of a path
struct Poly { Vec<float> pts; bool closed = false; };
void flatten_path(const Path& p, Vec<Poly>& out);
bool path_is_rect(const Path& p, float* x0, float* y0, float* x1, float* y1);    // axis aligned rectangle?
void path_bounds(const Path& p, float* x0, float* y0, float* x1, float* y1);

// ---- clip masks
struct ClipMask {
    int refs = 1;
    IRect b;                       // bounds in page pixels
    u8* m = nullptr;               // (b.x1-b.x0) * (b.y1-b.y0) coverage, or null when the bounds are the whole clip
    static ClipMask* make(IRect b) { ClipMask* c = new ClipMask(); c->b = b; return c; }
    ClipMask* ref() { refs++; return this; }
    void unref() { if (--refs == 0) { if (m) kfree(m); delete this; } }
    u8 at(int x, int y) const {
        if (x < b.x0 || y < b.y0 || x >= b.x1 || y >= b.y1) return 0;
        return m ? m[(size_t)(y - b.y0) * (b.x1 - b.x0) + (x - b.x0)] : 255;
    }
};
// intersect: the result is a new mask (the old one keeps its reference count)
ClipMask* clip_intersect_path(ClipMask* cur, const Path& devpath, bool evenodd);

// ---- functions (pdf_color.cpp)
struct PdfFunc {
    int type = -1;                 // 0 sampled, 2 exponential, 3 stitching, 4 calculator, 100 array of 1-output functions
    int m = 1, n = 1;
    double domain[16], range[16];
    bool has_range = false;
    // type 0
    int size[8]; int bps = 8; double encode[16], decode[16]; u8* samples = nullptr; u32 nsamples = 0;
    // type 2
    double c0[8], c1[8], N = 1;
    // type 3
    Vec<PdfFunc*> subs; double bounds[64]; double enc3[128]; int nsub = 0;
    // type 4
    Vec<u32> prog;                 // opcodes, see pdf_color.cpp
    Vec<double> consts;
    void eval(const double* in, double* out) const;
    ~PdfFunc();
};
PdfFunc* fn_parse(PdfDoc* d, Obj* o, int depth = 0);

// ---- colour spaces (pdf_color.cpp)
enum { CS_GRAY, CS_RGB, CS_CMYK, CS_CAL_GRAY, CS_CAL_RGB, CS_LAB, CS_INDEXED, CS_SEP, CS_PATTERN, CS_ICC };
struct ColorSpace {
    int kind = CS_GRAY;
    int n = 1;
    ColorSpace* base = nullptr;    // Indexed base, Separation alternate, Pattern underlying
    PdfFunc* fn = nullptr;
    u8* lut = nullptr;             // Indexed: (hival + 1) * base->n bytes
    int hival = 0;
    double rng[4] = {-100, 100, -100, 100};
    double white[3] = {0.9505, 1, 1.089};
    double gamma[3] = {1, 1, 1};
    double mat[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool none = false;             // Separation "None"
    bool owned_base = false;
    ~ColorSpace();
};
ColorSpace* cs_device(int n);                                           // shared DeviceGray / RGB / CMYK
ColorSpace* cs_parse(PdfDoc* d, Obj* o, Obj* res, int depth = 0);        // never null (DeviceGray fallback); owned by the doc cache
u32  cs_rgb(const ColorSpace* cs, const double* comps);                 // opaque ARGB
void cs_initial(const ColorSpace* cs, double* comps);
u32  rgb_from_cmyk(double c, double m, double y, double k);
void render_cache_free(PdfDoc* d);

// ---- shading (pdf_shade.cpp)
struct Shading;
Shading* shading_parse(PdfDoc* d, Obj* o, Obj* res);                     // null when unsupported
void     shading_free(Shading* s);
// shade a device-space pixel; returns false when the pixel is outside the shading (no paint)
bool     shading_at(Shading* s, double dx, double dy, u32* argb);
void     shading_set_matrix(Shading* s, const Mat& to_device);           // pattern space -> device
bool     shading_has_bbox(Shading* s, double* box);
u32      shading_background(Shading* s, bool* has);
// mesh shadings are drawn into a bitmap covering `area` first
void     shading_prepare(Shading* s, IRect area);

// ---- images (pdf_image.cpp); `inline_data` is non-null for inline images. fill_rgb colours stencil masks
bool pdf_decode_image(PdfDoc* d, Obj* dict, const u8* inline_data, u32 inline_len, Obj* res, u32 fill_rgb, Image* out);
