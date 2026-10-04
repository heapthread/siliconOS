// silicon - PDF internals shared by the object layer, the renderer and the font code
#pragma once
#include "pdf.h"

enum { O_NULL, O_BOOL, O_NUM, O_STR, O_NAME, O_ARR, O_DICT, O_STREAM, O_REF, O_KEY };

struct Obj;
struct DictEnt { const char* key; Obj* val; };
struct Obj {
    u8   type;
    bool is_int;
    union {
        bool b;
        double n;
        struct { const u8* s; u32 len; } str;      // O_STR, O_NAME (nul terminated), O_KEY
        struct { Obj** v; u32 n; } arr;
        struct { DictEnt* e; u32 n; } dict;        // O_DICT and O_STREAM
        struct { u32 num, gen; } ref;
    };
    u32 soff, slen;                                // O_STREAM: where the raw data sits in the file
    u32 num, gen;                                  // object number it was loaded as (0 = direct) and its generation
};

struct XEnt { u8 type; u32 a, b; };                // 0 free, 1 at offset a, 2 inside object stream a at index b, 255 unset

struct DecEnt { Obj* o; u8* d; u32 len; char imgf[8]; u32 stamp; };
struct ObjStm { u32 num; u8* data; u32 len; u32 n, first; u32* nums; u32* offs; };

struct PdfPage { Obj* dict; Obj* res; float box[4]; int rotate; u32 num; };

struct PdfFont;
struct PdfTextCache { int page; Vec<PdfChar> chars; };

struct Arena {
    Vec<u8*> blocks;
    u8* cur = nullptr;
    u32 left = 0;
    void* alloc(u32 n);
    void free_all();
    void reset();
};

struct PdfDoc {
    u8* data = nullptr;
    u32 len = 0;
    Arena arena;
    XEnt* xref = nullptr;
    u32 nobj = 0;
    Obj** cache = nullptr;
    Obj* trailer = nullptr;
    Obj* root = nullptr;
    Vec<ObjStm*> objstms;
    Vec<PdfPage> pages;
    Vec<DecEnt> dec;
    u32 dec_bytes = 0, dec_stamp = 0;
    bool encrypted = false;
    // security handler (standard, RC4 / AES-128 / AES-256)
    u8 key[32];
    int keylen = 0;
    int crypt_method = 0;                  // 0 none, 1 RC4, 2 AES-128 CBC, 3 AES-256 CBC
    char err[96] = "";
    // outline and info
    Vec<PdfOutline> outline;
    bool outline_built = false;
    char info_buf[4][160];
    char info_key[4][16];
    int n_info = 0;
    // text of pages
    Vec<PdfTextCache*> texts;
    // fonts
    struct FontEnt { Obj* dict; PdfFont* font; };
    Vec<FontEnt> fonts;
    int depth = 0;
    bool rebuilt = false;
    struct RenderCache* rc = nullptr;
    u32 hidden_ocgs[64];                   // object numbers of optional content groups that are off
    int n_hidden = 0;

    ~PdfDoc();
};

// ---- objects (pdf_obj.cpp)
Obj*  pdf_get(PdfDoc* d, u32 num);
Obj*  pdf_res(PdfDoc* d, Obj* o);                      // follow references
Obj*  dget(PdfDoc* d, Obj* dict, const char* key);     // resolved entry of a dict / stream, or nullptr
Obj*  dget2(PdfDoc* d, Obj* dict, const char* key, const char* alt);   // key, else alt (inline image abbreviations)
Obj*  draw_get(Obj* dict, const char* key);            // unresolved entry (to see a reference)
Obj*  aget(PdfDoc* d, Obj* arr, u32 i);                // resolved array element
double onum(Obj* o, double def);
bool  is_name(Obj* o, const char* nm);
const char* oname(Obj* o);                             // "" when not a name
static inline bool is_dict(Obj* o) { return o && (o->type == O_DICT || o->type == O_STREAM); }
static inline u32  alen(Obj* o) { return o && o->type == O_ARR ? o->arr.n : 0; }
u8*   pdf_stream(PdfDoc* d, Obj* s, u32* len, char* image_filter);   // decoded bytes owned by the doc cache
u8*   pdf_stream_copy(PdfDoc* d, Obj* s, u32* len);
u8*   pdf_stream_decode(PdfDoc* d, Obj* s, u32* len, char* image_filter);   // decoded bytes the caller frees, image filter name left in place
u8*   pdf_apply_filters(PdfDoc* d, Obj* dict, u8* data, u32 len, u32* outlen, char* image_filter);   // consumes data    // decoded bytes the caller frees
Obj*  parse_obj_at(PdfDoc* d, const u8* buf, u32 n, u32* pos, bool refs);   // one object; keywords come back as O_KEY
Obj*  new_obj(PdfDoc* d, u8 type);
void  render_cache_free(PdfDoc* d);
void  pdf_text_string(Obj* o, char* out, int n);        // PDFDocEncoding / UTF-16 -> silicon text
void  pdf_dest_page(PdfDoc* d, Obj* dest, int* page);   // resolves a destination (array, name or string)
int   pdf_page_of_ref(PdfDoc* d, Obj* raw);

// stream filters (pdf_filter.cpp)
u8* lzw_decode(const u8* in, u32 n, int early, u32* outn);
u8* a85_decode(const u8* in, u32 n, u32* outn);
u8* ahx_decode(const u8* in, u32 n, u32* outn);
u8* rl_decode(const u8* in, u32 n, u32* outn);
u8* apply_predictor(u8* data, u32* len, int pred, int colors, int bpc, int columns);   // frees/keeps data
u8* ccitt_decode(const u8* in, u32 n, int k, int cols, int rows, bool black_is1, bool byte_align, u32* outn);

// ---- rendering geometry
struct Mat { double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; };
static inline Mat mat_mul(const Mat& m, const Mat& n) {       // m then n (row vectors)
    Mat r;
    r.a = m.a * n.a + m.b * n.c;  r.b = m.a * n.b + m.b * n.d;
    r.c = m.c * n.a + m.d * n.c;  r.d = m.c * n.b + m.d * n.d;
    r.e = m.e * n.a + m.f * n.c + n.e;  r.f = m.e * n.b + m.f * n.d + n.f;
    return r;
}
static inline void mat_apply(const Mat& m, double x, double y, double* ox, double* oy) {
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}
Mat mat_inv(const Mat& m, bool* ok);

// math without a libm (pdf_math.cpp)
double m_sqrt(double x);
double m_sin(double x);
double m_cos(double x);
double m_atan2(double y, double x);
double m_pow(double x, double y);
double m_exp(double x);
double m_log(double x);
double m_floor(double x);
double m_fabs(double x);
static inline double m_ceil(double x) { return -m_floor(-x); }

// ---- vector paths
enum { P_MOVE, P_LINE, P_CURVE, P_CLOSE };
struct PSeg { u8 op; float x[6]; };      // MOVE/LINE use x[0..1]; CURVE x[0..5]
struct Path {
    Vec<PSeg> s;
    float cx = 0, cy = 0, sx = 0, sy = 0;
    void clear() { s.clear(); }
    void free() { s.free(); }
    void move(float x, float y) { PSeg g; g.op = P_MOVE; g.x[0] = x; g.x[1] = y; s.push(g); cx = sx = x; cy = sy = y; }
    void line(float x, float y) { if (!s.len) move(x, y); PSeg g; g.op = P_LINE; g.x[0] = x; g.x[1] = y; s.push(g); cx = x; cy = y; }
    void curve(float x1, float y1, float x2, float y2, float x3, float y3) {
        if (!s.len) move(x1, y1);
        PSeg g; g.op = P_CURVE; g.x[0] = x1; g.x[1] = y1; g.x[2] = x2; g.x[3] = y2; g.x[4] = x3; g.x[5] = y3; s.push(g); cx = x3; cy = y3;
    }
    void quad(float x1, float y1, float x2, float y2) {
        curve(cx + 2.0f / 3 * (x1 - cx), cy + 2.0f / 3 * (y1 - cy), x2 + 2.0f / 3 * (x1 - x2), y2 + 2.0f / 3 * (y1 - y2), x2, y2);
    }
    void close() { if (!s.len) return; PSeg g; g.op = P_CLOSE; s.push(g); cx = sx; cy = sy; }
    void rect(float x, float y, float w, float h) { move(x, y); line(x + w, y); line(x + w, y + h); line(x, y + h); close(); }
    void append(const Path& o, const Mat& m);      // transformed copy
};

// ---- fonts (pdf_font.cpp, pdf_glyph.cpp)
PdfFont* pdf_font_load(PdfDoc* d, Obj* fdict);                     // cached, never null (falls back)
void     pdf_font_free(PdfFont* f);
int      font_next_code(PdfFont* f, const u8* s, int n, int pos, u32* code);     // returns bytes consumed (>=1)
double   font_width(PdfFont* f, u32 code);                         // text space units (1.0 = font size)
bool     font_is_type3(PdfFont* f);
bool     font_vertical(PdfFont* f);
// outline of a code in text space (1 unit = font size); returns false when it has no shape
bool     font_glyph(PdfFont* f, u32 code, Path& out);
// fallback bitmap font: draws code as coverage into a w*h byte mask (used when the font has no outlines)
bool     font_fallback_cell(PdfFont* f, u32 code, int* ch);         // true = the code is a drawable character (ch = silicon glyph byte)
u32      font_unicode(PdfFont* f, u32 code);
int      font_unicodes(PdfFont* f, u32 code, u32* out);              // up to 4 code points (ligatures), count

Obj*     font_type3_proc(PdfFont* f, u32 code);
Obj*     font_type3_dict(PdfFont* f);
void     font_type3_matrix(PdfFont* f, double m[6]);
bool     font_italic(PdfFont* f);
bool     font_bold(PdfFont* f);
bool     font_has_outlines(PdfFont* f);
double   font_ascent(PdfFont* f);
double   font_descent(PdfFont* f);
void     font_vmetrics(PdfFont* f, u32 code, double* w1, double* vx, double* vy);

// glyph-name tables (pdf_font.cpp)
int  glyph_name_to_uni(const char* name);                 // -1 when unknown
const char* uni_to_glyph_name(u32 cp);                    // nullptr when unknown

// ---- font file parsers (pdf_glyph.cpp)
struct TTFont;
TTFont* tt_open(const u8* data, u32 len, int index);
void    tt_free(TTFont* t);
int     tt_glyph_count(TTFont* t);
int     tt_cmap_lookup(TTFont* t, int platform, int enc, u32 cp);   // 0 when missing
bool    tt_has_cmap(TTFont* t, int platform, int enc);
int     tt_post_glyph(TTFont* t, const char* name);                 // via the post table names, 0 when unknown
bool    tt_outline(TTFont* t, int gid, Path& out);                  // in 1/units_per_em
double  tt_advance(TTFont* t, int gid);                             // 1/units_per_em
bool    tt_is_cff(TTFont* t);
const u8* tt_cff_table(TTFont* t, u32* len);

struct CffFont;
CffFont* cff_open(const u8* data, u32 len);
void     cff_free(CffFont* c);
int      cff_glyph_count(CffFont* c);
bool     cff_is_cid(CffFont* c);
int      cff_gid_for_name(CffFont* c, const char* name);
int      cff_gid_for_cid(CffFont* c, int cid);
int      cff_gid_for_code(CffFont* c, int code);                    // built-in encoding
const char* cff_glyph_name(CffFont* c, int gid);
bool     cff_outline(CffFont* c, int gid, Path& out, double* adv);  // in 1/1000 (scaled by the font matrix)
void     cff_matrix(CffFont* c, double m[6]);

struct T1Font;
T1Font* t1_open(const u8* data, u32 len, u32 len1, u32 len2);
void    t1_free(T1Font* t);
bool    t1_outline(T1Font* t, const char* name, Path& out, double* adv);
const char* t1_builtin_name(T1Font* t, int code);                   // built-in encoding
void    t1_matrix(T1Font* t, double m[6]);
bool    t1_has_glyph(T1Font* t, const char* name);

// ---- content stream tokenizer: returns false at the end. operands are filled in, then the operator
struct ContentLex {
    const u8* d; u32 n, p;
    PdfDoc* doc;
    Arena ar;                      // operands of the current operator live here
    u32 img_off, img_len;          // inline image data after "BI" (operands[0] is its dict)
};
bool content_next(ContentLex& lx, Vec<Obj*>& operands, char* op, int opn);

// encryption (pdf_crypt.cpp)
bool pdf_setup_encryption(PdfDoc* d);       // false when a password is needed
void pdf_decrypt_obj(PdfDoc* d, Obj* o, u32 num, u32 gen);
u8*  pdf_decrypt_stream(PdfDoc* d, u8* data, u32* len, u32 num, u32 gen);

// ---- the interpreter (pdf_render.cpp)
struct RenderOpts {
    bool draw;                   // false: only collect text
    Image* out;                  // page bitmap (draw mode)
    Mat base;                    // user space -> device
    PdfTextCache* text;          // receives characters when non-null
    int page;
};
void pdf_run_page(PdfDoc* d, PdfPage& pg, RenderOpts& o);
int  pdf_text_cmp_dummy();
