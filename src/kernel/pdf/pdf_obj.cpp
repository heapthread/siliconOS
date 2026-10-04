// silicon - PDF: file structure (xref, object streams), the object parser, stream decoding,
// the page tree, document info and the outline
#include "pdf_int.h"
#include "../gfx/gfx.h"
#include "../text.h"

// ================================================================ arena
void* Arena::alloc(u32 n) {
    n = (n + 7) & ~7u;
    if (n > left) {
        u32 sz = max(n, 128u * 1024);
        u8* b = (u8*)kcalloc(sz);
        blocks.push(b);
        cur = b;
        left = sz;
    }
    void* r = cur;
    cur += n;
    left -= n;
    return r;
}
void Arena::free_all() {
    for (int i = 0; i < blocks.len; i++) kfree(blocks[i]);
    blocks.free();
    cur = nullptr;
    left = 0;
}
void Arena::reset() {                      // keep the first block; users initialise what they allocate
    for (int i = 1; i < blocks.len; i++) kfree(blocks[i]);
    if (blocks.len > 1) blocks.len = 1;
    if (blocks.len == 1) { cur = blocks[0]; left = 128 * 1024; }
}

PdfDoc::~PdfDoc() {
    render_cache_free(this);
    for (int i = 0; i < fonts.len; i++) pdf_font_free(fonts[i].font);
    fonts.free();
    for (int i = 0; i < dec.len; i++) kfree(dec[i].d);
    dec.free();
    for (int i = 0; i < objstms.len; i++) {
        ObjStm* s = objstms[i];
        kfree(s->data); kfree(s->nums); kfree(s->offs); kfree(s);
    }
    objstms.free();
    for (int i = 0; i < texts.len; i++) { texts[i]->chars.free(); delete texts[i]; }
    texts.free();
    pages.free();
    outline.free();
    if (xref) kfree(xref);
    if (cache) kfree(cache);
    if (data) kfree(data);
    arena.free_all();
}

// ================================================================ lexer / parser
static inline bool is_ws(u8 c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
static inline bool is_delim(u8 c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }
static inline bool is_reg(u8 c) { return !is_ws(c) && !is_delim(c); }
static inline int hexv(u8 c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

struct Parser {
    PdfDoc* d;
    Arena* ar;
    const u8* b;
    u32 n, p;
    bool refs;
    int depth;
};

static Obj* nobj(Parser& ps, u8 type) {
    Obj* o = (Obj*)ps.ar->alloc(sizeof(Obj));
    memset(o, 0, sizeof(Obj));
    o->type = type;
    return o;
}
Obj* new_obj(PdfDoc* d, u8 type) {
    Obj* o = (Obj*)d->arena.alloc(sizeof(Obj));
    memset(o, 0, sizeof(Obj));
    o->type = type;
    return o;
}

static void skip_ws(Parser& ps) {
    for (;;) {
        while (ps.p < ps.n && is_ws(ps.b[ps.p])) ps.p++;
        if (ps.p < ps.n && ps.b[ps.p] == '%') {
            while (ps.p < ps.n && ps.b[ps.p] != '\n' && ps.b[ps.p] != '\r') ps.p++;
            continue;
        }
        break;
    }
}

static Obj* parse(Parser& ps);

static bool scan_int(const u8* b, u32 n, u32& p, u32* out) {        // unsigned decimal integer
    u32 q = p;
    u64 v = 0;
    if (q >= n || b[q] < '0' || b[q] > '9') return false;
    while (q < n && b[q] >= '0' && b[q] <= '9') { v = v * 10 + (b[q] - '0'); if (v > 0xFFFFFFFFull) v = 0xFFFFFFFFull; q++; }
    p = q;
    *out = (u32)v;
    return true;
}

static Obj* parse_number(Parser& ps) {
    u32 p = ps.p;
    bool neg = false, isint = true;
    while (p < ps.n && (ps.b[p] == '+' || ps.b[p] == '-')) { if (ps.b[p] == '-') neg = true; p++; }
    double v = 0;
    bool any = false;
    while (p < ps.n && ps.b[p] >= '0' && ps.b[p] <= '9') { v = v * 10 + (ps.b[p] - '0'); p++; any = true; }
    if (p < ps.n && ps.b[p] == '.') {
        isint = false;
        p++;
        double f = 0.1;
        while (p < ps.n && ((ps.b[p] >= '0' && ps.b[p] <= '9') || ps.b[p] == '-')) {
            if (ps.b[p] != '-') { v += (ps.b[p] - '0') * f; f *= 0.1; any = true; }
            p++;
        }
    }
    if (!any && isint) return nullptr;         // a lone sign: treat as a keyword
    // swallow trailing junk of a malformed number such as 1.2.3
    while (p < ps.n && is_reg(ps.b[p]) && ((ps.b[p] >= '0' && ps.b[p] <= '9') || ps.b[p] == '.')) p++;
    Obj* o = nobj(ps, O_NUM);
    o->n = neg ? -v : v;
    o->is_int = isint;
    ps.p = p;
    return o;
}

static Obj* make_str(Parser& ps, const Vec<u8>& out) {
    Obj* o = nobj(ps, O_STR);
    u8* s = (u8*)ps.ar->alloc(out.len + 1);
    memcpy(s, out.data, out.len);
    s[out.len] = 0;
    o->str.s = s;
    o->str.len = out.len;
    return o;
}

static Obj* parse_string(Parser& ps) {
    ps.p++;                                    // at '('
    Vec<u8> out;
    int depth = 1;
    while (ps.p < ps.n) {
        u8 c = ps.b[ps.p++];
        if (c == '(') { depth++; out.push(c); }
        else if (c == ')') { if (--depth == 0) break; out.push(c); }
        else if (c == '\\' && ps.p < ps.n) {
            u8 e = ps.b[ps.p++];
            switch (e) {
            case 'n': out.push('\n'); break;
            case 'r': out.push('\r'); break;
            case 't': out.push('\t'); break;
            case 'b': out.push('\b'); break;
            case 'f': out.push('\f'); break;
            case '\r': if (ps.p < ps.n && ps.b[ps.p] == '\n') ps.p++; break;
            case '\n': break;
            default:
                if (e >= '0' && e <= '7') {
                    int v = e - '0';
                    for (int k = 0; k < 2 && ps.p < ps.n && ps.b[ps.p] >= '0' && ps.b[ps.p] <= '7'; k++) v = v * 8 + (ps.b[ps.p++] - '0');
                    out.push((u8)v);
                } else out.push(e);
            }
        } else if (c == '\r') {
            if (ps.p < ps.n && ps.b[ps.p] == '\n') ps.p++;
            out.push('\n');
        } else out.push(c);
    }
    Obj* o = make_str(ps, out);
    out.free();
    return o;
}

static Obj* parse_hex(Parser& ps) {
    ps.p++;
    Vec<u8> out;
    int hi = -1;
    while (ps.p < ps.n) {
        u8 c = ps.b[ps.p++];
        if (c == '>') break;
        int v = hexv(c);
        if (v < 0) continue;
        if (hi < 0) hi = v; else { out.push((u8)(hi * 16 + v)); hi = -1; }
    }
    if (hi >= 0) out.push((u8)(hi * 16));
    Obj* o = make_str(ps, out);
    out.free();
    return o;
}

static Obj* parse_name(Parser& ps) {
    ps.p++;
    u32 st = ps.p;
    while (ps.p < ps.n && is_reg(ps.b[ps.p])) ps.p++;
    u32 len = ps.p - st;
    u8* s = (u8*)ps.ar->alloc(len + 1);
    u32 k = 0;
    for (u32 i = 0; i < len; i++) {
        u8 c = ps.b[st + i];
        if (c == '#' && i + 2 < len && hexv(ps.b[st + i + 1]) >= 0 && hexv(ps.b[st + i + 2]) >= 0) {
            s[k++] = (u8)(hexv(ps.b[st + i + 1]) * 16 + hexv(ps.b[st + i + 2]));
            i += 2;
        } else s[k++] = c;
    }
    s[k] = 0;
    Obj* o = nobj(ps, O_NAME);
    o->str.s = s;
    o->str.len = k;
    return o;
}

static Obj* make_key(Parser& ps, const u8* s, u32 len) {
    Obj* o = nobj(ps, O_KEY);
    u8* c = (u8*)ps.ar->alloc(len + 1);
    memcpy(c, s, len);
    c[len] = 0;
    o->str.s = c;
    o->str.len = len;
    return o;
}

static bool key_is(Obj* o, const char* k) { return o && o->type == O_KEY && !strcmp((const char*)o->str.s, k); }

static Obj* parse(Parser& ps) {
    skip_ws(ps);
    if (ps.p >= ps.n) return nullptr;
    if (ps.depth > 64) return nullptr;
    u8 c = ps.b[ps.p];
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
        u32 st = ps.p;
        Obj* o = parse_number(ps);
        if (o) {
            if (ps.refs && o->is_int && o->n >= 0 && ps.b[st] >= '0' && ps.b[st] <= '9') {
                u32 q = ps.p;
                while (q < ps.n && is_ws(ps.b[q])) q++;
                u32 gen;
                if (q > ps.p && scan_int(ps.b, ps.n, q, &gen)) {
                    u32 r = q;
                    while (r < ps.n && is_ws(ps.b[r])) r++;
                    if (r > q && r < ps.n && ps.b[r] == 'R' && (r + 1 >= ps.n || !is_reg(ps.b[r + 1]))) {
                        Obj* ref = nobj(ps, O_REF);
                        ref->ref.num = (u32)o->n;
                        ref->ref.gen = gen;
                        ps.p = r + 1;
                        return ref;
                    }
                }
            }
            return o;
        }
        // fall through: a lone sign is a keyword
    }
    if (c == '(') return parse_string(ps);
    if (c == '/') return parse_name(ps);
    if (c == '<') {
        if (ps.p + 1 < ps.n && ps.b[ps.p + 1] == '<') {
            ps.p += 2;
            Vec<DictEnt> ents;
            ps.depth++;
            for (;;) {
                Obj* k = parse(ps);
                if (!k || key_is(k, ">>")) break;
                if (k->type != O_NAME) continue;
                Obj* v = parse(ps);
                if (!v) break;
                if (key_is(v, ">>")) break;
                if (v->type == O_KEY) continue;           // stray keyword
                bool dup = false;
                for (int i = 0; i < ents.len; i++) if (!strcmp(ents[i].key, (const char*)k->str.s)) { ents[i].val = v; dup = true; }
                if (!dup) { DictEnt e; e.key = (const char*)k->str.s; e.val = v; ents.push(e); }
            }
            ps.depth--;
            Obj* o = nobj(ps, O_DICT);
            o->dict.n = ents.len;
            o->dict.e = (DictEnt*)ps.ar->alloc(sizeof(DictEnt) * (ents.len + 1));
            for (int i = 0; i < ents.len; i++) o->dict.e[i] = ents[i];
            ents.free();
            return o;
        }
        return parse_hex(ps);
    }
    if (c == '[') {
        ps.p++;
        Vec<Obj*> items;
        ps.depth++;
        for (;;) {
            Obj* v = parse(ps);
            if (!v || key_is(v, "]")) break;
            if (v->type == O_KEY) continue;
            items.push(v);
        }
        ps.depth--;
        Obj* o = nobj(ps, O_ARR);
        o->arr.n = items.len;
        o->arr.v = (Obj**)ps.ar->alloc(sizeof(Obj*) * (items.len + 1));
        for (int i = 0; i < items.len; i++) o->arr.v[i] = items[i];
        items.free();
        return o;
    }
    if (c == ']') { ps.p++; return make_key(ps, (const u8*)"]", 1); }
    if (c == '>') {
        if (ps.p + 1 < ps.n && ps.b[ps.p + 1] == '>') { ps.p += 2; return make_key(ps, (const u8*)">>", 2); }
        ps.p++;
        return make_key(ps, (const u8*)">", 1);
    }
    if (c == '{' || c == '}' || c == ')') { ps.p++; return make_key(ps, &c, 1); }
    // keyword
    u32 st = ps.p;
    while (ps.p < ps.n && is_reg(ps.b[ps.p])) ps.p++;
    if (ps.p == st) ps.p++;
    u32 len = ps.p - st;
    if (len == 4 && !memcmp(ps.b + st, "true", 4)) { Obj* o = nobj(ps, O_BOOL); o->b = true; return o; }
    if (len == 5 && !memcmp(ps.b + st, "false", 5)) { Obj* o = nobj(ps, O_BOOL); o->b = false; return o; }
    if (len == 4 && !memcmp(ps.b + st, "null", 4)) return nobj(ps, O_NULL);
    return make_key(ps, ps.b + st, len);
}

Obj* parse_obj_at(PdfDoc* d, const u8* buf, u32 n, u32* pos, bool refs) {
    Parser ps{d, &d->arena, buf, n, *pos, refs, 0};
    Obj* o = parse(ps);
    *pos = ps.p;
    return o;
}

// ================================================================ accessors
Obj* pdf_res(PdfDoc* d, Obj* o) {
    for (int i = 0; i < 16 && o && o->type == O_REF; i++) o = pdf_get(d, o->ref.num);
    if (o && o->type == O_REF) return nullptr;
    return o;
}
Obj* draw_get(Obj* dict, const char* key) {
    if (!is_dict(dict)) return nullptr;
    for (u32 i = 0; i < dict->dict.n; i++) if (!strcmp(dict->dict.e[i].key, key)) return dict->dict.e[i].val;
    return nullptr;
}
Obj* dget(PdfDoc* d, Obj* dict, const char* key) {
    Obj* v = draw_get(dict, key);
    if (!v) return nullptr;
    v = pdf_res(d, v);
    return v && v->type == O_NULL ? nullptr : v;
}
Obj* dget2(PdfDoc* d, Obj* dict, const char* key, const char* alt) {
    Obj* v = dget(d, dict, key);
    return v ? v : dget(d, dict, alt);
}
Obj* aget(PdfDoc* d, Obj* arr, u32 i) {
    if (!arr || arr->type != O_ARR || i >= arr->arr.n) return nullptr;
    return pdf_res(d, arr->arr.v[i]);
}
double onum(Obj* o, double def) { return o && o->type == O_NUM ? o->n : def; }
bool is_name(Obj* o, const char* nm) { return o && o->type == O_NAME && !strcmp((const char*)o->str.s, nm); }
const char* oname(Obj* o) { return o && o->type == O_NAME ? (const char*)o->str.s : ""; }

// ================================================================ xref
static void xgrow(PdfDoc* d, u32 n) {
    if (n <= d->nobj) return;
    if (n > 8000000) n = 8000000;
    if (n <= d->nobj) return;
    XEnt* nx = (XEnt*)kmalloc(sizeof(XEnt) * n);
    Obj** nc = (Obj**)kcalloc(sizeof(Obj*) * n);
    for (u32 i = 0; i < n; i++) { nx[i].type = 255; nx[i].a = nx[i].b = 0; }
    if (d->xref) { memcpy(nx, d->xref, sizeof(XEnt) * d->nobj); kfree(d->xref); }
    if (d->cache) { memcpy(nc, d->cache, sizeof(Obj*) * d->nobj); kfree(d->cache); }
    d->xref = nx;
    d->cache = nc;
    d->nobj = n;
}

static void xset(PdfDoc* d, u32 num, u8 type, u32 a, u32 b) {
    if (num >= 8000000) return;
    if (num >= d->nobj) xgrow(d, max(num + 1, d->nobj * 2));
    XEnt& e = d->xref[num];
    if (e.type != 255) return;                    // the newest section defines it first
    e.type = type; e.a = a; e.b = b;
}

static const u8* memchr_u8(const u8* p, u8 c, u32 n) {
    for (u32 i = 0; i < n; i++) if (p[i] == c) return p + i;
    return nullptr;
}

static Obj* parse_indirect_at(PdfDoc* d, u32 off, u32* num_out, int depth);

static bool stream_len_ok(PdfDoc* d, u32 soff, u32 slen) {
    if ((u64)soff + slen > d->len) return false;
    u32 p = soff + slen;
    while (p < d->len && is_ws(d->data[p])) p++;
    return p + 9 <= d->len && !memcmp(d->data + p, "endstream", 9);
}

static Obj* parse_indirect_at(PdfDoc* d, u32 off, u32* num_out, int depth) {
    if (off >= d->len) return nullptr;
    Parser ps{d, &d->arena, d->data, d->len, off, true, 0};
    u32 num = 0, gen = 0;
    skip_ws(ps);
    if (!scan_int(ps.b, ps.n, ps.p, &num)) return nullptr;
    skip_ws(ps);
    if (!scan_int(ps.b, ps.n, ps.p, &gen)) return nullptr;
    skip_ws(ps);
    if (ps.p + 3 > ps.n || memcmp(ps.b + ps.p, "obj", 3)) return nullptr;
    ps.p += 3;
    if (num_out) *num_out = num;
    Obj* o = parse(ps);
    if (!o) return nullptr;
    if (o->type == O_KEY) return nullptr;
    o->gen = gen;
    if (o->type == O_DICT) {
        skip_ws(ps);
        if (ps.p + 6 <= ps.n && !memcmp(ps.b + ps.p, "stream", 6)) {
            u32 p = ps.p + 6;
            if (p < ps.n && ps.b[p] == '\r') p++;
            if (p < ps.n && ps.b[p] == '\n') p++;
            o->type = O_STREAM;
            o->soff = p;
            u32 slen = 0;
            bool have = false;
            Obj* L = draw_get(o, "Length");
            if (L && depth < 4) {
                if (L->type == O_REF) {
                    d->depth++;
                    Obj* lo = d->depth < 8 ? pdf_get(d, L->ref.num) : nullptr;
                    d->depth--;
                    if (lo && lo->type == O_NUM) { slen = (u32)lo->n; have = true; }
                } else if (L->type == O_NUM) { slen = (u32)L->n; have = true; }
            }
            if (!have || !stream_len_ok(d, p, slen)) {
                u32 q = p;
                bool found = false;
                while (q + 9 <= d->len) {
                    const u8* h = memchr_u8(d->data + q, 'e', d->len - q);
                    if (!h) break;
                    q = (u32)(h - d->data);
                    if (q + 9 <= d->len && !memcmp(d->data + q, "endstream", 9)) { found = true; break; }
                    q++;
                }
                if (!found) q = d->len;
                u32 e = q;
                if (e > p && d->data[e - 1] == '\n') e--;
                if (e > p && d->data[e - 1] == '\r') e--;
                slen = e - p;
            }
            o->slen = slen;
        }
    }
    return o;
}

static bool parse_xref_stream(PdfDoc* d, Obj* s, bool first) {
    if (!s || s->type != O_STREAM) return false;
    u32 len;
    u8* data = pdf_stream(d, s, &len, nullptr);
    if (!data) return false;
    Obj* W = dget(d, s, "W");
    if (!W || W->type != O_ARR || W->arr.n < 3) return false;
    int w[3];
    for (int i = 0; i < 3; i++) { w[i] = (int)onum(aget(d, W, i), 0); if (w[i] < 0 || w[i] > 8) return false; }
    int rowl = w[0] + w[1] + w[2];
    if (rowl <= 0) return false;
    u32 size = (u32)onum(dget(d, s, "Size"), 0);
    xgrow(d, size);
    Obj* idx = dget(d, s, "Index");
    Vec<u32> ranges;
    if (idx && idx->type == O_ARR) {
        for (u32 i = 0; i + 1 < idx->arr.n; i += 2) {
            ranges.push((u32)onum(aget(d, idx, i), 0));
            ranges.push((u32)onum(aget(d, idx, i + 1), 0));
        }
    } else { ranges.push(0); ranges.push(size); }
    u32 pos = 0;
    for (int r = 0; r + 1 < ranges.len; r += 2) {
        for (u32 k = 0; k < ranges[r + 1]; k++) {
            if (pos + rowl > len) break;
            u64 f[3];
            const u8* p = data + pos;
            for (int i = 0; i < 3; i++) {
                f[i] = 0;
                for (int j = 0; j < w[i]; j++) f[i] = (f[i] << 8) | *p++;
            }
            if (w[0] == 0) f[0] = 1;
            pos += rowl;
            u32 num = ranges[r] + k;
            if (f[0] == 1) xset(d, num, 1, (u32)f[1], (u32)f[2]);
            else if (f[0] == 2) xset(d, num, 2, (u32)f[1], (u32)f[2]);
            else if (f[0] == 0) xset(d, num, 0, 0, 0);
        }
    }
    ranges.free();
    if (first || !d->trailer) { if (!d->trailer) d->trailer = s; }
    return true;
}

static bool read_xref_chain(PdfDoc* d, u32 off) {
    u32 seen[64];
    int nseen = 0;
    bool any = false;
    while (off < d->len) {
        bool dup = false;
        for (int i = 0; i < nseen; i++) if (seen[i] == off) dup = true;
        if (dup || nseen >= 64) break;
        seen[nseen++] = off;
        Parser ps{d, &d->arena, d->data, d->len, off, true, 0};
        skip_ws(ps);
        Obj* tr = nullptr;
        if (ps.p + 4 <= d->len && !memcmp(d->data + ps.p, "xref", 4)) {
            ps.p += 4;
            for (;;) {
                skip_ws(ps);
                if (ps.p + 7 <= d->len && !memcmp(d->data + ps.p, "trailer", 7)) { ps.p += 7; break; }
                u32 start, count;
                if (!scan_int(ps.b, ps.n, ps.p, &start)) return any;
                skip_ws(ps);
                if (!scan_int(ps.b, ps.n, ps.p, &count)) return any;
                if (count > 8000000) return any;
                for (u32 i = 0; i < count; i++) {
                    skip_ws(ps);
                    u32 a, g;
                    if (!scan_int(ps.b, ps.n, ps.p, &a)) return any;
                    skip_ws(ps);
                    if (!scan_int(ps.b, ps.n, ps.p, &g)) return any;
                    skip_ws(ps);
                    if (ps.p >= ps.n) return any;
                    u8 t = ps.b[ps.p++];
                    if (t == 'n') xset(d, start + i, 1, a, g);
                    else if (t == 'f') xset(d, start + i, 0, 0, 0);
                    else return any;
                }
            }
            tr = parse(ps);
            if (!tr || tr->type != O_DICT) return any;
            any = true;
            Obj* xs = dget(d, tr, "XRefStm");
            if (xs && xs->type == O_NUM) {
                Obj* so = parse_indirect_at(d, (u32)xs->n, nullptr, 0);
                parse_xref_stream(d, so, false);
            }
            if (!d->trailer) d->trailer = tr;
        } else {
            Obj* so = parse_indirect_at(d, off, nullptr, 0);
            if (!so || so->type != O_STREAM) return any;
            if (!parse_xref_stream(d, so, true)) return any;
            tr = so;
            any = true;
        }
        Obj* prev = dget(d, tr, "Prev");
        if (!prev || prev->type != O_NUM) break;
        off = (u32)prev->n;
    }
    return any;
}

static void load_objstm_entries(PdfDoc* d, u32 stm_num);

static void forget_objstms(PdfDoc* d) {
    for (int i = 0; i < d->objstms.len; i++) { ObjStm* s = d->objstms[i]; kfree(s->data); kfree(s->nums); kfree(s->offs); kfree(s); }
    d->objstms.clear();
}

// no usable xref: scan the file for "N G obj"
static void rebuild_xref(PdfDoc* d) {
    d->rebuilt = true;
    if (d->xref) { kfree(d->xref); d->xref = nullptr; }
    if (d->cache) { kfree(d->cache); d->cache = nullptr; }
    d->nobj = 0;
    d->trailer = nullptr;
    xgrow(d, 1024);
    forget_objstms(d);
    Vec<u32> stms;
    Obj* best_trailer = nullptr;
    const u8* b = d->data;
    u32 n = d->len;
    for (u32 i = 0; i + 3 <= n; i++) {
        if (b[i] != 'o' || b[i + 1] != 'b' || b[i + 2] != 'j') continue;
        if (i > 0 && b[i - 1] == 'd') continue;               // endobj
        if (i + 3 < n && is_reg(b[i + 3])) continue;
        u32 q = i;
        while (q > 0 && is_ws(b[q - 1])) q--;
        u32 e2 = q;
        while (q > 0 && b[q - 1] >= '0' && b[q - 1] <= '9') q--;
        if (q == e2 || q == 0) continue;
        u32 s2 = q;
        while (q > 0 && is_ws(b[q - 1])) q--;
        if (q == s2) continue;
        u32 e1 = q;
        while (q > 0 && b[q - 1] >= '0' && b[q - 1] <= '9') q--;
        if (q == e1) continue;
        u32 s1 = q, num = 0;
        u32 pp = s1;
        if (!scan_int(b, n, pp, &num)) continue;
        if (num >= 8000000) continue;
        if (num >= d->nobj) xgrow(d, max(num + 1, d->nobj * 2));
        d->xref[num].type = 1;
        d->xref[num].a = s1;
        d->xref[num].b = 0;
        d->cache[num] = nullptr;
    }
    for (u32 i = 0; i + 7 <= n; i++) {
        if (b[i] == 't' && !memcmp(b + i, "trailer", 7)) {
            u32 p = i + 7;
            Obj* t = parse_obj_at(d, b, n, &p, true);
            if (t && t->type == O_DICT && draw_get(t, "Root")) best_trailer = t;
        }
    }
    Obj* cat = nullptr;
    for (u32 i = 0; i < d->nobj; i++) {
        if (d->xref[i].type != 1) continue;
        Obj* o = pdf_get(d, i);
        if (!o || !is_dict(o)) continue;
        Obj* t = dget(d, o, "Type");
        if (is_name(t, "ObjStm") && o->type == O_STREAM) stms.push(i);
        else if (is_name(t, "XRef") && draw_get(o, "Root")) { if (!best_trailer) best_trailer = o; }
        else if (is_name(t, "Catalog")) cat = o;
    }
    for (int i = 0; i < stms.len; i++) load_objstm_entries(d, stms[i]);
    stms.free();
    if (!best_trailer && !cat) {
        for (u32 i = 0; i < d->nobj; i++) {                   // a catalog inside an object stream
            if (d->xref[i].type != 2) continue;
            Obj* o = pdf_get(d, i);
            if (o && is_dict(o) && is_name(dget(d, o, "Type"), "Catalog")) { cat = o; break; }
        }
    }
    if (best_trailer) d->trailer = best_trailer;
    else if (cat) {
        Obj* t = new_obj(d, O_DICT);
        t->dict.e = (DictEnt*)d->arena.alloc(sizeof(DictEnt) * 2);
        Obj* r = new_obj(d, O_REF);
        r->ref.num = cat->num;
        t->dict.e[0].key = "Root";
        t->dict.e[0].val = r;
        t->dict.n = 1;
        d->trailer = t;
    }
}

// ================================================================ object streams
static ObjStm* get_objstm(PdfDoc* d, u32 num) {
    for (int i = 0; i < d->objstms.len; i++) if (d->objstms[i]->num == num) return d->objstms[i];
    if (d->objstms.len > 24) {                        // keep the memory bounded
        ObjStm* s = d->objstms[0];
        kfree(s->data); kfree(s->nums); kfree(s->offs); kfree(s);
        d->objstms.remove(0);
    }
    if (d->depth > 6) return nullptr;
    d->depth++;
    Obj* so = pdf_get(d, num);
    d->depth--;
    if (!so || so->type != O_STREAM) return nullptr;
    u32 len;
    u8* data = pdf_stream_copy(d, so, &len);
    if (!data) return nullptr;
    u32 n = (u32)onum(dget(d, so, "N"), 0);
    u32 first = (u32)onum(dget(d, so, "First"), 0);
    if (n > 1000000 || first > len) { kfree(data); return nullptr; }
    ObjStm* s = (ObjStm*)kcalloc(sizeof(ObjStm));
    s->num = num; s->data = data; s->len = len; s->n = n; s->first = first;
    s->nums = (u32*)kcalloc(sizeof(u32) * (n + 1));
    s->offs = (u32*)kcalloc(sizeof(u32) * (n + 1));
    u32 p = 0;
    u32 got = 0;
    for (u32 i = 0; i < n; i++) {
        while (p < len && is_ws(data[p])) p++;
        u32 a, b;
        if (!scan_int(data, len, p, &a)) break;
        while (p < len && is_ws(data[p])) p++;
        if (!scan_int(data, len, p, &b)) break;
        s->nums[i] = a; s->offs[i] = b;
        got++;
    }
    s->n = got;
    d->objstms.push(s);
    return s;
}

static void load_objstm_entries(PdfDoc* d, u32 stm_num) {
    ObjStm* s = get_objstm(d, stm_num);
    if (!s) return;
    for (u32 i = 0; i < s->n; i++) {
        u32 num = s->nums[i];
        if (num >= 8000000) continue;
        if (num >= d->nobj) xgrow(d, max(num + 1, d->nobj * 2));
        if (d->xref[num].type == 255 || d->xref[num].type == 2) { d->xref[num].type = 2; d->xref[num].a = stm_num; d->xref[num].b = i; d->cache[num] = nullptr; }
    }
}

Obj* pdf_get(PdfDoc* d, u32 num) {
    if (num >= d->nobj || !d->xref) return nullptr;
    if (d->cache[num]) return d->cache[num];
    XEnt e = d->xref[num];
    Obj* o = nullptr;
    if (e.type == 1) {
        static Obj busy;
        d->cache[num] = &busy;                   // cycle guard while the object (and its Length) loads
        u32 got = 0;
        o = parse_indirect_at(d, e.a, &got, 0);
        d->cache[num] = nullptr;
        if (o && d->crypt_method) pdf_decrypt_obj(d, o, num, o->gen);
    } else if (e.type == 2) {
        ObjStm* s = get_objstm(d, e.a);
        if (s && e.b < s->n) {
            u32 p = s->first + s->offs[e.b];
            if (p < s->len) {
                Parser ps{d, &d->arena, s->data, s->len, p, true, 0};
                o = parse(ps);
                if (o && o->type == O_KEY) o = nullptr;
            }
        }
    }
    if (o) {
        o->num = num;
        d->cache[num] = o;
    }
    return o;
}

// ================================================================ stream decoding
static u8* dup_bytes(const u8* s, u32 n) {
    u8* r = (u8*)kmalloc(n + 1);
    memcpy(r, s, n);
    return r;
}

static const char* canon_filter(const char* f) {
    static const struct { const char* a; const char* b; } t[] = {
        {"Fl", "FlateDecode"}, {"LZW", "LZWDecode"}, {"A85", "ASCII85Decode"}, {"AHx", "ASCIIHexDecode"},
        {"RL", "RunLengthDecode"}, {"CCF", "CCITTFaxDecode"}, {"DCT", "DCTDecode"},
    };
    for (auto& e : t) if (!strcmp(f, e.a)) return e.b;
    return f;
}

static u8* decode_stream(PdfDoc* d, Obj* s, u32* outlen, char* imgf) {
    if (imgf) imgf[0] = 0;
    if (!s || s->type != O_STREAM || (u64)s->soff + s->slen > d->len) { *outlen = 0; return (u8*)kcalloc(1); }
    u8* data = dup_bytes(d->data + s->soff, s->slen);
    u32 len = s->slen;
    if (d->crypt_method && s->num) data = pdf_decrypt_stream(d, data, &len, s->num, s->gen);
    Obj* F = dget(d, s, "Filter");
    Obj* DP = dget(d, s, "DecodeParms");
    if (!F) { F = dget(d, s, "F"); if (F && !(F->type == O_NAME || F->type == O_ARR)) F = nullptr; }
    if (!DP) DP = dget(d, s, "DP");
    u32 nf = !F ? 0 : F->type == O_ARR ? F->arr.n : 1;
    for (u32 i = 0; i < nf; i++) {
        Obj* fo = F->type == O_ARR ? aget(d, F, i) : F;
        const char* fn = canon_filter(oname(fo));
        Obj* parms = DP ? (DP->type == O_ARR ? aget(d, DP, i) : (i == 0 ? DP : nullptr)) : nullptr;
        if (parms && !is_dict(parms)) parms = nullptr;
        u8* out = nullptr;
        u32 olen = 0;
        if (!strcmp(fn, "FlateDecode")) {
            out = zlib_decompress(data, len, &olen, 192u * 1024 * 1024);
            if (!out && len > 2) out = deflate_decompress(data + 2, len - 2, &olen, 192u * 1024 * 1024);
            if (!out) { out = (u8*)kmalloc(16); olen = 0; }
            int pred = (int)onum(dget(d, parms, "Predictor"), 1);
            if (pred > 1) out = apply_predictor(out, &olen, pred, (int)onum(dget(d, parms, "Colors"), 1),
                                                (int)onum(dget(d, parms, "BitsPerComponent"), 8), (int)onum(dget(d, parms, "Columns"), 1));
        } else if (!strcmp(fn, "LZWDecode")) {
            int early = (int)onum(dget(d, parms, "EarlyChange"), 1);
            out = lzw_decode(data, len, early, &olen);
            if (!out) { out = (u8*)kmalloc(16); olen = 0; }
            int pred = (int)onum(dget(d, parms, "Predictor"), 1);
            if (pred > 1) out = apply_predictor(out, &olen, pred, (int)onum(dget(d, parms, "Colors"), 1),
                                                (int)onum(dget(d, parms, "BitsPerComponent"), 8), (int)onum(dget(d, parms, "Columns"), 1));
        } else if (!strcmp(fn, "ASCII85Decode")) {
            out = a85_decode(data, len, &olen);
        } else if (!strcmp(fn, "ASCIIHexDecode")) {
            out = ahx_decode(data, len, &olen);
        } else if (!strcmp(fn, "RunLengthDecode")) {
            out = rl_decode(data, len, &olen);
        } else if (!strcmp(fn, "Crypt")) {
            continue;
        } else {
            // DCTDecode, JPXDecode, CCITTFaxDecode, JBIG2Decode: the image code takes it from here
            if (imgf) {
                if (!strcmp(fn, "DCTDecode")) strcpy(imgf, "DCT");
                else if (!strcmp(fn, "JPXDecode")) strcpy(imgf, "JPX");
                else if (!strcmp(fn, "CCITTFaxDecode")) strcpy(imgf, "CCF");
                else if (!strcmp(fn, "JBIG2Decode")) strcpy(imgf, "JBIG2");
                else strlcpy(imgf, "?", 8);
            }
            break;
        }
        kfree(data);
        if (!out) { *outlen = 0; return (u8*)kcalloc(1); }
        data = out;
        len = olen;
    }
    *outlen = len;
    return data;
}

u8* pdf_stream_copy(PdfDoc* d, Obj* s, u32* len) {
    char tmp[8];
    return decode_stream(d, s, len, tmp);
}

u8* pdf_stream(PdfDoc* d, Obj* s, u32* len, char* imgf) {
    for (int i = 0; i < d->dec.len; i++) {
        if (d->dec[i].o == s) {
            d->dec[i].stamp = ++d->dec_stamp;
            *len = d->dec[i].len;
            if (imgf) strcpy(imgf, d->dec[i].imgf);
            return d->dec[i].d;
        }
    }
    DecEnt e;
    e.o = s;
    e.d = decode_stream(d, s, &e.len, e.imgf);
    e.stamp = ++d->dec_stamp;
    // keep the cache bounded (the new entry is never the victim)
    while (d->dec.len > 0 && (d->dec_bytes + e.len > 40u * 1024 * 1024 || d->dec.len > 48)) {
        int v = 0;
        for (int i = 1; i < d->dec.len; i++) if (d->dec[i].stamp < d->dec[v].stamp) v = i;
        d->dec_bytes -= d->dec[v].len;
        kfree(d->dec[v].d);
        d->dec.remove(v);
    }
    d->dec.push(e);
    d->dec_bytes += e.len;
    *len = e.len;
    if (imgf) strcpy(imgf, e.imgf);
    return e.d;
}

// ================================================================ text strings
void pdf_text_string(Obj* o, char* out, int n) {
    if (n <= 0) return;
    out[0] = 0;
    if (!o || o->type != O_STR) return;
    const u8* s = o->str.s;
    u32 len = o->str.len;
    int k = 0;
    auto put_cp = [&](u32 cp) {
        char g[4];
        int m = glyph_for(cp, g);
        for (int i = 0; i < m && k < n - 1; i++) out[k++] = g[i];
    };
    if (len >= 2 && s[0] == 0xFE && s[1] == 0xFF) {
        for (u32 i = 2; i + 1 < len; i += 2) {
            u32 cp = (s[i] << 8) | s[i + 1];
            if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < len) {
                u32 lo = (s[i + 2] << 8) | s[i + 3];
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            put_cp(cp);
        }
    } else if (len >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) {
        const char* p = (const char*)s + 3;
        const char* e = (const char*)s + len;
        while (p < e) put_cp(utf8_decode(p, e));
    } else {
        static const u16 hi[32] = {0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x192, 0x2044, 0x2039, 0x203A, 0x2212, 0x2030, 0x201E, 0x201C, 0x201D, 0x2018,
                                   0x2019, 0x201A, 0x2122, 0xFB01, 0xFB02, 0x141, 0x152, 0x160, 0x178, 0x17D, 0x131, 0x142, 0x153, 0x161, 0x17E, 0};
        for (u32 i = 0; i < len; i++) {
            u8 c = s[i];
            if (c >= 0x80 && c < 0xA0) put_cp(hi[c - 0x80] ? hi[c - 0x80] : '?');
            else put_cp(c);
        }
    }
    out[k] = 0;
}

// ================================================================ page tree
static void box_from(PdfDoc* d, Obj* a, float* b, bool* ok) {
    *ok = false;
    if (!a || a->type != O_ARR || a->arr.n < 4) return;
    double v[4];
    for (int i = 0; i < 4; i++) v[i] = onum(aget(d, a, i), 0);
    b[0] = (float)min(v[0], v[2]); b[1] = (float)min(v[1], v[3]);
    b[2] = (float)max(v[0], v[2]); b[3] = (float)max(v[1], v[3]);
    *ok = true;
}

struct PageCtx { Obj* res; float media[4]; bool has_media; float crop[4]; bool has_crop; int rotate; };

static void walk_pages(PdfDoc* d, Obj* node, PageCtx ctx, int depth) {
    if (!is_dict(node) || depth > 40 || d->pages.len > 200000) return;
    Obj* r = dget(d, node, "Resources");
    if (r) ctx.res = r;
    bool ok;
    float b[4];
    box_from(d, dget(d, node, "MediaBox"), b, &ok);
    if (ok) { memcpy(ctx.media, b, sizeof(b)); ctx.has_media = true; }
    box_from(d, dget(d, node, "CropBox"), b, &ok);
    if (ok) { memcpy(ctx.crop, b, sizeof(b)); ctx.has_crop = true; }
    Obj* rot = dget(d, node, "Rotate");
    if (rot && rot->type == O_NUM) ctx.rotate = (int)rot->n;
    Obj* kids = dget(d, node, "Kids");
    if (kids && kids->type == O_ARR && !is_name(dget(d, node, "Type"), "Page")) {
        for (u32 i = 0; i < kids->arr.n; i++) walk_pages(d, aget(d, kids, i), ctx, depth + 1);
        return;
    }
    PdfPage pg;
    pg.dict = node;
    pg.res = ctx.res;
    pg.num = node->num;
    float mb[4] = {0, 0, 612, 792};
    if (ctx.has_media) memcpy(mb, ctx.media, sizeof(mb));
    if (ctx.has_crop) {
        float x0 = max(mb[0], ctx.crop[0]), y0 = max(mb[1], ctx.crop[1]), x1 = min(mb[2], ctx.crop[2]), y1 = min(mb[3], ctx.crop[3]);
        if (x1 > x0 && y1 > y0) { mb[0] = x0; mb[1] = y0; mb[2] = x1; mb[3] = y1; }
    }
    if (mb[2] - mb[0] < 1 || mb[3] - mb[1] < 1) { mb[0] = 0; mb[1] = 0; mb[2] = 612; mb[3] = 792; }
    memcpy(pg.box, mb, sizeof(mb));
    int rt = ctx.rotate % 360;
    if (rt < 0) rt += 360;
    pg.rotate = (rt / 90) * 90;
    d->pages.push(pg);
}

static void collect_pages(PdfDoc* d) {
    d->pages.clear();
    Obj* pages = dget(d, d->root, "Pages");
    PageCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    walk_pages(d, pages, ctx, 0);
}

// ================================================================ open
static bool setup_catalog(PdfDoc* d) {
    if (!d->trailer) return false;
    d->root = dget(d, d->trailer, "Root");
    if (!is_dict(d->root)) return false;
    collect_pages(d);
    return d->pages.len > 0;
}

PdfDoc* pdf_open(const u8* data, u32 len, char* err, int errn) {
    auto fail = [&](const char* m) -> PdfDoc* { if (err && errn > 0) strlcpy(err, m, errn); return nullptr; };
    if (!data || len < 16) return fail("not a PDF file");
    u32 hdr = 0xFFFFFFFF;
    for (u32 i = 0; i + 5 <= len && i < 1024; i++) if (!memcmp(data + i, "%PDF-", 5)) { hdr = i; break; }
    if (hdr == 0xFFFFFFFF) return fail("not a PDF file");
    PdfDoc* d = new PdfDoc();
    d->len = len - hdr;
    d->data = (u8*)kmalloc_try(d->len + 16);
    if (!d->data) { delete d; return fail("out of memory"); }
    memcpy(d->data, data + hdr, d->len);
    memset(d->data + d->len, 0, 16);
    u32 from = d->len > 2048 ? d->len - 2048 : 0;
    const u8* sx = nullptr;
    for (u32 i = from; i + 9 <= d->len; i++) if (!memcmp(d->data + i, "startxref", 9)) sx = d->data + i;
    bool ok = false;
    if (sx) {
        u32 p = (u32)(sx - d->data) + 9;
        while (p < d->len && is_ws(d->data[p])) p++;
        u32 off;
        if (scan_int(d->data, d->len, p, &off)) {
            xgrow(d, 64);
            ok = read_xref_chain(d, off);
            if (ok) ok = setup_catalog(d);
        }
    }
    if (!ok) {
        rebuild_xref(d);
        ok = setup_catalog(d);
    }
    if (!ok) { delete d; return fail("cannot read the document structure"); }
    if (draw_get(d->trailer, "Encrypt")) {
        d->encrypted = true;
        if (!pdf_setup_encryption(d)) { delete d; return fail("password protected PDF"); }
        // objects parsed before the key was known must be read again, decrypted this time
        for (u32 i = 0; i < d->nobj; i++) d->cache[i] = nullptr;
        for (int i = 0; i < d->dec.len; i++) kfree(d->dec[i].d);
        d->dec.clear();
        d->dec_bytes = 0;
        forget_objstms(d);
        if (!setup_catalog(d)) { delete d; return fail("cannot read the document"); }
    }
    // optional content: groups that are off by default
    Obj* ocp = dget(d, d->root, "OCProperties");
    if (ocp) {
        Obj* dd = dget(d, ocp, "D");
        Obj* off = dget(d, dd, "OFF");
        if (off && off->type == O_ARR)
            for (u32 i = 0; i < off->arr.n && d->n_hidden < 64; i++)
                if (off->arr.v[i]->type == O_REF) d->hidden_ocgs[d->n_hidden++] = off->arr.v[i]->ref.num;
        Obj* bs = dget(d, dd, "BaseState");
        if (is_name(bs, "OFF")) {
            Obj* ocgs = dget(d, ocp, "OCGs");
            Obj* on = dget(d, dd, "ON");
            d->n_hidden = 0;
            for (u32 i = 0; ocgs && ocgs->type == O_ARR && i < ocgs->arr.n && d->n_hidden < 64; i++) {
                if (ocgs->arr.v[i]->type != O_REF) continue;
                bool is_on = false;
                for (u32 k = 0; on && on->type == O_ARR && k < on->arr.n; k++) if (on->arr.v[k]->type == O_REF && on->arr.v[k]->ref.num == ocgs->arr.v[i]->ref.num) is_on = true;
                if (!is_on) d->hidden_ocgs[d->n_hidden++] = ocgs->arr.v[i]->ref.num;
            }
        }
    }
    return d;
}

void pdf_close(PdfDoc* d) { delete d; }
int pdf_page_count(PdfDoc* d) { return d->pages.len; }

void pdf_page_size(PdfDoc* d, int page, float* w, float* h) {
    if (page < 0 || page >= d->pages.len) { *w = 612; *h = 792; return; }
    PdfPage& p = d->pages[page];
    float pw = p.box[2] - p.box[0], ph = p.box[3] - p.box[1];
    if (p.rotate == 90 || p.rotate == 270) swap(pw, ph);
    *w = pw; *h = ph;
}

// ================================================================ info / outline
const char* pdf_info(PdfDoc* d, const char* key) {
    for (int i = 0; i < d->n_info; i++) if (!strcmp(d->info_key[i], key)) return d->info_buf[i];
    Obj* info = dget(d, d->trailer, "Info");
    int slot = d->n_info < 4 ? d->n_info++ : 3;
    strlcpy(d->info_key[slot], key, sizeof(d->info_key[slot]));
    d->info_buf[slot][0] = 0;
    Obj* v = dget(d, info, key);
    if (v && v->type == O_STR) pdf_text_string(v, d->info_buf[slot], sizeof(d->info_buf[slot]));
    return d->info_buf[slot];
}

int pdf_page_of_ref(PdfDoc* d, Obj* raw) {
    if (!raw) return -1;
    if (raw->type == O_REF) {
        for (int i = 0; i < d->pages.len; i++) if (d->pages[i].num == raw->ref.num) return i;
        return -1;
    }
    if (raw->type == O_NUM) { int p = (int)raw->n; return p >= 0 && p < d->pages.len ? p : -1; }
    return -1;
}

static Obj* nametree_find(PdfDoc* d, Obj* node, const u8* key, u32 klen, int depth) {
    if (!is_dict(node) || depth > 12) return nullptr;
    Obj* names = dget(d, node, "Names");
    if (names && names->type == O_ARR) {
        for (u32 i = 0; i + 1 < names->arr.n; i += 2) {
            Obj* k = aget(d, names, i);
            if (k && k->type == O_STR && k->str.len == klen && !memcmp(k->str.s, key, klen)) return aget(d, names, i + 1);
        }
    }
    Obj* kids = dget(d, node, "Kids");
    if (kids && kids->type == O_ARR)
        for (u32 i = 0; i < kids->arr.n; i++) {
            Obj* r = nametree_find(d, aget(d, kids, i), key, klen, depth + 1);
            if (r) return r;
        }
    return nullptr;
}

void pdf_dest_page(PdfDoc* d, Obj* dest, int* page) {
    *page = -1;
    dest = pdf_res(d, dest);
    if (!dest) return;
    if (dest->type == O_NAME || dest->type == O_STR) {
        const u8* k = dest->str.s;
        u32 kl = dest->str.len;
        Obj* v = nullptr;
        if (dest->type == O_NAME) v = dget(d, dget(d, d->root, "Dests"), (const char*)k);
        if (!v) {
            Obj* names = dget(d, d->root, "Names");
            v = nametree_find(d, dget(d, names, "Dests"), k, kl, 0);
        }
        if (!v) return;
        dest = v;
    }
    if (is_dict(dest)) dest = dget(d, dest, "D");
    if (dest && dest->type == O_ARR && dest->arr.n > 0) *page = pdf_page_of_ref(d, dest->arr.v[0]);
}

static void outline_walk(PdfDoc* d, Obj* item, int level, int* budget) {
    for (int guard = 0; item && is_dict(item) && guard < 4000 && *budget > 0; guard++) {
        (*budget)--;
        PdfOutline o;
        char t[96];
        pdf_text_string(dget(d, item, "Title"), t, sizeof(t));
        strlcpy(o.title, t, sizeof(o.title));
        o.level = level;
        o.page = -1;
        Obj* dest = dget(d, item, "Dest");
        if (dest) pdf_dest_page(d, dest, &o.page);
        else {
            Obj* act = dget(d, item, "A");
            if (act && is_name(dget(d, act, "S"), "GoTo")) pdf_dest_page(d, dget(d, act, "D"), &o.page);
        }
        d->outline.push(o);
        Obj* first = dget(d, item, "First");
        if (first && level < 8) outline_walk(d, first, level + 1, budget);
        item = dget(d, item, "Next");
    }
}

static void build_outline(PdfDoc* d) {
    d->outline_built = true;
    Obj* ol = dget(d, d->root, "Outlines");
    if (!ol) return;
    int budget = 5000;
    outline_walk(d, dget(d, ol, "First"), 0, &budget);
}

int pdf_outline_count(PdfDoc* d) {
    if (!d->outline_built) build_outline(d);
    return d->outline.len;
}
const PdfOutline* pdf_outline(PdfDoc* d, int i) {
    if (!d->outline_built) build_outline(d);
    return i >= 0 && i < d->outline.len ? &d->outline[i] : nullptr;
}

// ================================================================ content streams
static bool inline_image_data(ContentLex& lx, Obj* dict, u32 start, u32* len, u32* after) {
    PdfDoc* d = lx.doc;
    Obj* F = dget2(d, dict, "Filter", "F");
    if (!F) {                                          // unfiltered: the size is known exactly
        double w = onum(dget2(d, dict, "Width", "W"), 0), h = onum(dget2(d, dict, "Height", "H"), 0);
        double bpc = onum(dget2(d, dict, "BitsPerComponent", "BPC"), 8);
        Obj* cs = dget2(d, dict, "ColorSpace", "CS");
        Obj* imo = dget2(d, dict, "ImageMask", "IM");
        bool im = imo && imo->type == O_BOOL && imo->b;
        int nc = 0;
        if (im) { nc = 1; bpc = 1; }
        else if (cs && cs->type == O_NAME) {
            const char* c = (const char*)cs->str.s;
            if (!strcmp(c, "G") || !strcmp(c, "DeviceGray") || !strcmp(c, "CalGray")) nc = 1;
            else if (!strcmp(c, "RGB") || !strcmp(c, "DeviceRGB") || !strcmp(c, "CalRGB")) nc = 3;
            else if (!strcmp(c, "CMYK") || !strcmp(c, "DeviceCMYK")) nc = 4;
        } else if (cs && cs->type == O_ARR) {
            Obj* f = aget(d, cs, 0);
            if (is_name(f, "I") || is_name(f, "Indexed")) nc = 1;
        }
        if (nc && w > 0 && h > 0) {
            u64 rowb = ((u64)w * nc * (u64)bpc + 7) / 8;
            u64 total = rowb * (u64)h;
            if (start + total <= lx.n) {
                u32 e = start + (u32)total;
                while (e < lx.n && is_ws(lx.d[e])) e++;
                if (e + 2 <= lx.n && lx.d[e] == 'E' && lx.d[e + 1] == 'I') { *len = (u32)total; *after = e + 2; return true; }
            }
        }
    }
    for (u32 q = start; q + 2 <= lx.n; q++) {
        if (lx.d[q] != 'E' || lx.d[q + 1] != 'I') continue;
        if (q > start && !is_ws(lx.d[q - 1])) continue;
        if (q + 2 < lx.n && !is_ws(lx.d[q + 2]) && !is_delim(lx.d[q + 2])) continue;
        bool ok = true;
        for (u32 k = q + 2; k < lx.n && k < q + 12; k++) {
            u8 c = lx.d[k];
            if (c > 127 || (c < 32 && !is_ws(c))) { ok = false; break; }
        }
        if (!ok) continue;
        u32 e = q;
        if (e > start && is_ws(lx.d[e - 1])) e--;
        *len = e - start;
        *after = q + 2;
        return true;
    }
    *len = lx.n - start;
    *after = lx.n;
    return true;
}

bool content_next(ContentLex& lx, Vec<Obj*>& ops, char* op, int opn) {
    ops.clear();
    lx.ar.reset();
    Parser ps{lx.doc, &lx.ar, lx.d, lx.n, lx.p, false, 0};
    for (;;) {
        Obj* o = parse(ps);
        if (!o) { lx.p = ps.p; return false; }
        if (o->type != O_KEY) { ops.push(o); if (ops.len > 4096) ops.clear(); continue; }
        strlcpy(op, (const char*)o->str.s, opn);
        if (!strcmp(op, "BI")) {
            Vec<DictEnt> ents;
            for (;;) {
                Obj* k = parse(ps);
                if (!k) break;
                if (key_is(k, "ID")) break;
                if (k->type != O_NAME) continue;
                Obj* v = parse(ps);
                if (!v) break;
                if (key_is(v, "ID")) break;
                DictEnt e; e.key = (const char*)k->str.s; e.val = v; ents.push(e);
            }
            Obj* dict = nobj(ps, O_DICT);
            dict->dict.n = ents.len;
            dict->dict.e = (DictEnt*)ps.ar->alloc(sizeof(DictEnt) * (ents.len + 1));
            for (int i = 0; i < ents.len; i++) dict->dict.e[i] = ents[i];
            ents.free();
            u32 p = ps.p;
            if (p + 1 < ps.n && ps.b[p] == '\r' && ps.b[p + 1] == '\n') p += 2;
            else if (p < ps.n && is_ws(ps.b[p])) p++;
            u32 len, after;
            inline_image_data(lx, dict, p, &len, &after);
            lx.img_off = p;
            lx.img_len = len;
            ps.p = after;
            ops.clear();
            ops.push(dict);
        }
        lx.p = ps.p;
        return true;
    }
}
