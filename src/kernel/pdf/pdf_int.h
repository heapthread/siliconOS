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
    u32 num;                                       // object number it was loaded as (0 = direct)
};

struct XEnt { u8 type; u32 a, b; };                // 0 free, 1 at offset a, 2 inside object stream a at index b

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

    ~PdfDoc();
};

// ---- objects (pdf_obj.cpp)
Obj*  pdf_get(PdfDoc* d, u32 num);
Obj*  pdf_res(PdfDoc* d, Obj* o);                      // follow references
Obj*  dget(PdfDoc* d, Obj* dict, const char* key);     // resolved entry of a dict / stream, or nullptr
Obj*  aget(PdfDoc* d, Obj* arr, u32 i);                // resolved array element
double onum(Obj* o, double def);
bool  is_name(Obj* o, const char* nm);
const char* oname(Obj* o);                             // "" when not a name
u8*   pdf_stream(PdfDoc* d, Obj* s, u32* len, char* image_filter);   // decoded bytes owned by the doc cache
u8*   pdf_stream_copy(PdfDoc* d, Obj* s, u32* len);    // decoded bytes the caller frees
Obj*  parse_obj_at(PdfDoc* d, const u8* buf, u32 n, u32* pos, bool* is_key);   // one object, refs allowed
void  pdf_text_string(Obj* o, char* out, int n);        // PDFDocEncoding / UTF-16 -> silicon text

// content stream tokenizer: returns false at the end. operands are filled in, then the operator
struct ContentLex {
    const u8* d; u32 n, p;
    PdfDoc* doc;
};
struct Operand { Obj* o; };
bool content_next(ContentLex& lx, Vec<Obj*>& operands, char* op, int opn, Obj** inline_img_dict, u32* img_off, u32* img_len);
