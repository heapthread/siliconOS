// silicon - PDF: font file parsers (TrueType / OpenType, CFF, Type 1) producing glyph outlines
#include "pdf_gfx.h"

const char* std_enc_name(int code);        // pdf_font.cpp

static const char* strstr_c(const char* h, const char* n) {
    size_t nl = strlen(n);
    if (!nl) return h;
    for (; *h; h++) if (*h == *n && !strncmp(h, n, nl)) return h;
    return nullptr;
}

static inline u32 rd16(const u8* p) { return (p[0] << 8) | p[1]; }
static inline int rds16(const u8* p) { return (i16)((p[0] << 8) | p[1]); }
static inline u32 rd32(const u8* p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// ================================================================ TrueType
struct CmapSub { u16 pid, eid; u32 off; };
struct TTFont {
    const u8* d; u32 len;
    u32 glyf = 0, loca = 0, hmtx = 0, cmap = 0, head = 0, post = 0, cff = 0, cff_len = 0;
    int upem = 1000, nglyphs = 0, nhm = 0, loca_fmt = 0;
    Vec<CmapSub> subs;
    u32 glyf_len = 0, loca_len = 0, hmtx_len = 0, post_len = 0;
    // post names
    bool post_names = false;
    u32 post_idx = 0, post_str = 0;
};

TTFont* tt_open(const u8* data, u32 len, int index) {
    if (len < 12) return nullptr;
    u32 base = 0;
    if (!memcmp(data, "ttcf", 4)) {
        if (len < 16) return nullptr;
        u32 n = rd32(data + 8);
        if ((u32)index >= n) index = 0;
        base = rd32(data + 12 + 4 * index);
        if (base + 12 > len) return nullptr;
    }
    u32 nt = rd16(data + base + 4);
    TTFont* t = new TTFont();
    t->d = data; t->len = len;
    for (u32 i = 0; i < nt; i++) {
        u32 e = base + 12 + i * 16;
        if (e + 16 > len) break;
        const u8* r = data + e;
        u32 off = rd32(r + 8), tl = rd32(r + 12);
        if (off > len) continue;
        if (off + tl > len) tl = len - off;
        if (!memcmp(r, "glyf", 4)) { t->glyf = off; t->glyf_len = tl; }
        else if (!memcmp(r, "loca", 4)) { t->loca = off; t->loca_len = tl; }
        else if (!memcmp(r, "hmtx", 4)) { t->hmtx = off; t->hmtx_len = tl; }
        else if (!memcmp(r, "cmap", 4)) t->cmap = off;
        else if (!memcmp(r, "head", 4)) t->head = off;
        else if (!memcmp(r, "post", 4)) { t->post = off; t->post_len = tl; }
        else if (!memcmp(r, "CFF ", 4)) { t->cff = off; t->cff_len = tl; }
        else if (!memcmp(r, "maxp", 4)) { if (off + 6 <= len) t->nglyphs = (int)rd16(data + off + 4); }
        else if (!memcmp(r, "hhea", 4)) { if (off + 36 <= len) t->nhm = (int)rd16(data + off + 34); }
    }
    if (t->head && t->head + 54 <= len) {
        t->upem = (int)rd16(data + t->head + 18);
        if (t->upem < 16) t->upem = 1000;
        t->loca_fmt = rds16(data + t->head + 50);
    }
    if (t->cmap && t->cmap + 4 <= len) {
        u32 n = rd16(t->d + t->cmap + 2);
        for (u32 i = 0; i < n; i++) {
            u32 e = t->cmap + 4 + i * 8;
            if (e + 8 > len) break;
            CmapSub s;
            s.pid = (u16)rd16(data + e); s.eid = (u16)rd16(data + e + 2); s.off = t->cmap + rd32(data + e + 4);
            if (s.off + 4 <= len) t->subs.push(s);
        }
    }
    if (t->post && t->post_len >= 34 && rd32(data + t->post) == 0x00020000) {
        u32 ng = rd16(data + t->post + 32);
        t->post_idx = t->post + 34;
        t->post_str = t->post_idx + ng * 2;
        t->post_names = t->post_str <= t->post + t->post_len;
    }
    if (!t->cff && (!t->glyf || !t->loca)) { delete t; return nullptr; }
    if (t->nglyphs == 0 && t->loca) t->nglyphs = (int)(t->loca_len / (t->loca_fmt ? 4 : 2)) - 1;
    return t;
}
void tt_free(TTFont* t) { if (t) { t->subs.free(); delete t; } }
int tt_glyph_count(TTFont* t) { return t->nglyphs; }
bool tt_is_cff(TTFont* t) { return t->cff != 0; }
const u8* tt_cff_table(TTFont* t, u32* len) { *len = t->cff_len; return t->cff ? t->d + t->cff : nullptr; }

bool tt_has_cmap(TTFont* t, int pid, int eid) {
    for (int i = 0; i < t->subs.len; i++) if (t->subs[i].pid == pid && (eid < 0 || t->subs[i].eid == eid)) return true;
    return false;
}

static int cmap_sub_lookup(TTFont* t, u32 off, u32 cp) {
    const u8* d = t->d;
    u32 len = t->len;
    u32 fmt = rd16(d + off);
    if (fmt == 0) {
        if (cp < 256 && off + 6 + cp < len) return d[off + 6 + cp];
        return 0;
    }
    if (fmt == 4) {
        if (cp > 0xFFFF || off + 14 > len) return 0;
        u32 segx2 = rd16(d + off + 6);
        u32 endp = off + 14, startp = endp + segx2 + 2, deltap = startp + segx2, rangep = deltap + segx2;
        if (rangep + segx2 > len) return 0;
        u32 lo = 0, hi = segx2 / 2;
        while (lo < hi) {
            u32 mid = (lo + hi) / 2;
            if (rd16(d + endp + mid * 2) < cp) lo = mid + 1; else hi = mid;
        }
        if (lo >= segx2 / 2) return 0;
        u32 start = rd16(d + startp + lo * 2);
        if (cp < start) return 0;
        u32 delta = rd16(d + deltap + lo * 2), ro = rd16(d + rangep + lo * 2);
        if (ro == 0) return (int)((cp + delta) & 0xFFFF);
        u32 gp = rangep + lo * 2 + ro + (cp - start) * 2;
        if (gp + 2 > len) return 0;
        u32 g = rd16(d + gp);
        return g ? (int)((g + delta) & 0xFFFF) : 0;
    }
    if (fmt == 6) {
        u32 first = rd16(d + off + 6), cnt = rd16(d + off + 8);
        if (cp < first || cp >= first + cnt) return 0;
        u32 p = off + 10 + (cp - first) * 2;
        return p + 2 <= len ? (int)rd16(d + p) : 0;
    }
    if (fmt == 12 || fmt == 13) {
        u32 ng = rd32(d + off + 12);
        u32 lo = 0, hi = ng;
        while (lo < hi) {
            u32 mid = (lo + hi) / 2;
            u32 p = off + 16 + mid * 12;
            if (p + 12 > len) return 0;
            u32 s = rd32(d + p), e = rd32(d + p + 4);
            if (cp < s) hi = mid;
            else if (cp > e) lo = mid + 1;
            else return (int)(fmt == 12 ? rd32(d + p + 8) + (cp - s) : rd32(d + p + 8));
        }
        return 0;
    }
    return 0;
}

int tt_cmap_lookup(TTFont* t, int pid, int eid, u32 cp) {
    for (int i = 0; i < t->subs.len; i++) {
        if (t->subs[i].pid != pid || (eid >= 0 && t->subs[i].eid != eid)) continue;
        int g = cmap_sub_lookup(t, t->subs[i].off, cp);
        if (g) return g;
    }
    return 0;
}

int tt_post_glyph(TTFont* t, const char* name) {
    if (!t->post_names) return 0;
    const u8* d = t->d;
    u32 end = t->post + t->post_len;
    u32 ng = (t->post_str - t->post_idx) / 2;
    // walk the pascal strings once per query (rare)
    int idx_wanted = -1;
    u32 p = t->post_str;
    int ni = 0;
    u32 nl = (u32)strlen(name);
    while (p < end) {
        u32 l = d[p];
        if (p + 1 + l > end) break;
        if (l == nl && !memcmp(d + p + 1, name, l)) { idx_wanted = 258 + ni; break; }
        p += 1 + l;
        ni++;
    }
    if (idx_wanted < 0) return 0;
    for (u32 g = 0; g < ng; g++) if ((int)rd16(d + t->post_idx + g * 2) == idx_wanted) return (int)g;
    return 0;
}

double tt_advance(TTFont* t, int gid) {
    if (!t->hmtx || t->nhm < 1) return 0.5;
    int i = gid < t->nhm ? gid : t->nhm - 1;
    u32 p = t->hmtx + i * 4;
    if (p + 2 > t->len) return 0.5;
    return rd16(t->d + p) / (double)t->upem;
}

static bool glyph_range(TTFont* t, int gid, u32* off, u32* len) {
    if (gid < 0 || gid >= t->nglyphs) return false;
    u32 a, b;
    if (t->loca_fmt) {
        if ((u32)(gid + 2) * 4 > t->loca_len) return false;
        a = rd32(t->d + t->loca + gid * 4); b = rd32(t->d + t->loca + gid * 4 + 4);
    } else {
        if ((u32)(gid + 2) * 2 > t->loca_len) return false;
        a = rd16(t->d + t->loca + gid * 2) * 2u; b = rd16(t->d + t->loca + gid * 2 + 2) * 2u;
    }
    if (b <= a || a >= t->glyf_len) return false;
    *off = t->glyf + a;
    *len = min(b - a, t->glyf_len - a);
    return true;
}

struct TPt { float x, y; bool on; };

static void tt_contour(Path& out, const TPt* p, int n) {
    if (n < 2) return;
    int first, count;
    float sx, sy;
    if (p[0].on) { sx = p[0].x; sy = p[0].y; first = 1; count = n - 1; }
    else if (p[n - 1].on) { sx = p[n - 1].x; sy = p[n - 1].y; first = 0; count = n - 1; }
    else { sx = (p[0].x + p[n - 1].x) / 2; sy = (p[0].y + p[n - 1].y) / 2; first = 0; count = n; }
    out.move(sx, sy);
    bool pend = false;
    float ox = 0, oy = 0;
    for (int k = 0; k < count; k++) {
        const TPt& q = p[first + k];
        if (q.on) {
            if (pend) { out.quad(ox, oy, q.x, q.y); pend = false; }
            else out.line(q.x, q.y);
        } else {
            if (pend) { float mx = (ox + q.x) / 2, my = (oy + q.y) / 2; out.quad(ox, oy, mx, my); }
            ox = q.x; oy = q.y; pend = true;
        }
    }
    if (pend) out.quad(ox, oy, sx, sy);
    out.close();
}

static bool tt_glyph_path(TTFont* t, int gid, Path& out, int depth) {
    u32 off, len;
    if (!glyph_range(t, gid, &off, &len) || len < 10 || depth > 6) return false;
    const u8* g = t->d + off;
    int nc = rds16(g);
    float sc = 1.0f / t->upem;
    if (nc >= 0) {
        if (10 + (u32)nc * 2 + 2 > len) return false;
        int npts = nc ? (int)rd16(g + 10 + (nc - 1) * 2) + 1 : 0;
        if (npts <= 0 || npts > 20000) return nc == 0;
        u32 p = 10 + nc * 2;
        u32 il = rd16(g + p);
        p += 2 + il;
        TPt* pts = (TPt*)kmalloc(sizeof(TPt) * npts);
        u8* fl = (u8*)kmalloc(npts);
        for (int i = 0; i < npts;) {
            if (p >= len) { kfree(pts); kfree(fl); return false; }
            u8 f = g[p++];
            fl[i++] = f;
            if (f & 8) {
                if (p >= len) { kfree(pts); kfree(fl); return false; }
                int r = g[p++];
                while (r-- > 0 && i < npts) fl[i++] = f;
            }
        }
        int x = 0, y = 0;
        for (int i = 0; i < npts; i++) {
            u8 f = fl[i];
            if (f & 2) { if (p >= len) break; int dx = g[p++]; x += (f & 16) ? dx : -dx; }
            else if (!(f & 16)) { if (p + 1 >= len) break; x += rds16(g + p); p += 2; }
            pts[i].x = (float)x;
        }
        for (int i = 0; i < npts; i++) {
            u8 f = fl[i];
            if (f & 4) { if (p >= len) break; int dy = g[p++]; y += (f & 32) ? dy : -dy; }
            else if (!(f & 32)) { if (p + 1 >= len) break; y += rds16(g + p); p += 2; }
            pts[i].y = (float)y;
            pts[i].on = (f & 1) != 0;
        }
        for (int i = 0; i < npts; i++) { pts[i].x *= sc; pts[i].y *= sc; }
        int start = 0;
        for (int c = 0; c < nc; c++) {
            int end = (int)rd16(g + 10 + c * 2);
            if (end >= npts) break;
            tt_contour(out, pts + start, end - start + 1);
            start = end + 1;
        }
        kfree(pts); kfree(fl);
        return true;
    }
    // composite
    u32 p = 10;
    for (int guard = 0; guard < 64; guard++) {
        if (p + 4 > len) break;
        u32 flags = rd16(g + p), cg = rd16(g + p + 2);
        p += 4;
        int dx = 0, dy = 0;
        if (flags & 1) { if (p + 4 > len) break; if (flags & 2) { dx = rds16(g + p); dy = rds16(g + p + 2); } p += 4; }
        else { if (p + 2 > len) break; if (flags & 2) { dx = (i8)g[p]; dy = (i8)g[p + 1]; } p += 2; }
        Mat m;
        auto f2 = [&](u32 q) { return rds16(g + q) / 16384.0; };
        if (flags & 8) { if (p + 2 > len) break; m.a = m.d = f2(p); p += 2; }
        else if (flags & 0x40) { if (p + 4 > len) break; m.a = f2(p); m.d = f2(p + 2); p += 4; }
        else if (flags & 0x80) { if (p + 8 > len) break; m.a = f2(p); m.b = f2(p + 2); m.c = f2(p + 4); m.d = f2(p + 6); p += 8; }
        m.e = dx * sc; m.f = dy * sc;
        Path sub;
        if (tt_glyph_path(t, (int)cg, sub, depth + 1)) out.append(sub, m);
        sub.free();
        if (!(flags & 0x20)) break;
    }
    return true;
}

bool tt_outline(TTFont* t, int gid, Path& out) {
    if (!t->glyf) return false;
    int before = out.s.len;
    bool ok = tt_glyph_path(t, gid, out, 0);
    return ok && out.s.len > before;
}

// ================================================================ CFF
static const char* const cff_std_names[229] = {
    ".notdef", "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quoteright", "parenleft", "parenright", "asterisk", "plus", "comma", "hyphen",
    "period", "slash", "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "colon", "semicolon", "less", "equal", "greater", "question", "at",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "bracketleft", "backslash", "bracketright",
    "asciicircum", "underscore", "quoteleft", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "braceleft", "bar", "braceright", "asciitilde", "exclamdown", "cent", "sterling", "fraction", "yen", "florin", "section", "currency", "quotesingle", "quotedblleft",
    "guillemotleft", "guilsinglleft", "guilsinglright", "fi", "fl", "endash", "dagger", "daggerdbl", "periodcentered", "paragraph", "bullet", "quotesinglbase", "quotedblbase",
    "quotedblright", "guillemotright", "ellipsis", "perthousand", "questiondown", "grave", "acute", "circumflex", "tilde", "macron", "breve", "dotaccent", "dieresis", "ring",
    "cedilla", "hungarumlaut", "ogonek", "caron", "emdash", "AE", "ordfeminine", "Lslash", "Oslash", "OE", "ordmasculine", "ae", "dotlessi", "lslash", "oslash", "oe",
    "germandbls", "onesuperior", "logicalnot", "mu", "trademark", "Eth", "onehalf", "plusminus", "Thorn", "onequarter", "divide", "brokenbar", "degree", "thorn",
    "threequarters", "twosuperior", "registered", "minus", "eth", "multiply", "threesuperior", "copyright", "Aacute", "Acircumflex", "Adieresis", "Agrave", "Aring", "Atilde",
    "Ccedilla", "Eacute", "Ecircumflex", "Edieresis", "Egrave", "Iacute", "Icircumflex", "Idieresis", "Igrave", "Ntilde", "Oacute", "Ocircumflex", "Odieresis", "Ograve",
    "Otilde", "Scaron", "Uacute", "Ucircumflex", "Udieresis", "Ugrave", "Yacute", "Ydieresis", "Zcaron", "aacute", "acircumflex", "adieresis", "agrave", "aring", "atilde",
    "ccedilla", "eacute", "ecircumflex", "edieresis", "egrave", "iacute", "icircumflex", "idieresis", "igrave", "ntilde", "oacute", "ocircumflex", "odieresis", "ograve",
    "otilde", "scaron", "uacute", "ucircumflex", "udieresis", "ugrave", "yacute", "ydieresis", "zcaron"};

struct CffIndex { u32 count = 0, off_size = 0, offs = 0, data = 0, end = 0; };   // offs: start of the offset array; data: base for offsets

struct CffPriv { double defw = 0, nomw = 0; CffIndex subrs; bool has_subrs = false; };

struct CffFont {
    const u8* d; u32 len;
    CffIndex strings, gsubrs, charstrings, fdarray;
    u32 charset_off = 0, encoding_off = 0, fdselect_off = 0;
    bool cid = false;
    u16* sid = nullptr;                 // per glyph: SID (or CID)
    int nglyphs = 0;
    CffPriv priv;                       // for non-CID
    Vec<CffPriv> fdprivs;
    double fm[6] = {0.001, 0, 0, 0.001, 0, 0};
    bool has_fm = false;
    int enc_gid[256];
    bool has_enc = false;
    const char* name_cache = nullptr;
    Vec<double> fd_fm;                  // 6 per FD (when given)
    char tmpname[24];
};

static bool cff_index(const u8* d, u32 len, u32 pos, CffIndex& ix) {
    if (pos + 2 > len) return false;
    ix.count = rd16(d + pos);
    if (ix.count == 0) { ix.end = pos + 2; ix.off_size = 0; return true; }
    if (pos + 3 > len) return false;
    ix.off_size = d[pos + 2];
    if (ix.off_size < 1 || ix.off_size > 4) return false;
    ix.offs = pos + 3;
    ix.data = ix.offs + (ix.count + 1) * ix.off_size - 1;
    u32 last = ix.offs + ix.count * ix.off_size;
    if (last + ix.off_size > len) return false;
    u32 v = 0;
    for (u32 k = 0; k < ix.off_size; k++) v = (v << 8) | d[last + k];
    ix.end = ix.data + v;
    if (ix.end > len) ix.end = len;
    return true;
}
static u32 cff_off(const u8* d, const CffIndex& ix, u32 i) {
    u32 v = 0;
    const u8* p = d + ix.offs + i * ix.off_size;
    for (u32 k = 0; k < ix.off_size; k++) v = (v << 8) | p[k];
    return v;
}
static bool cff_item(const CffFont* c, const CffIndex& ix, u32 i, const u8** p, u32* n) {
    if (i >= ix.count) return false;
    u32 a = cff_off(c->d, ix, i), b = cff_off(c->d, ix, i + 1);
    if (b < a || ix.data + b > c->len) return false;
    *p = c->d + ix.data + a;
    *n = b - a;
    return true;
}

// DICT operands
struct DictVals { double v[48]; int n; };
static bool cff_dict_next(const u8* d, u32 len, u32& p, DictVals& dv, int* op) {
    dv.n = 0;
    while (p < len) {
        u8 b = d[p];
        if (b <= 21) {
            p++;
            if (b == 12) { if (p >= len) return false; *op = 0x0C00 | d[p++]; }
            else *op = b;
            return true;
        }
        double v = 0;
        if (b == 28) { if (p + 3 > len) return false; v = (i16)((d[p + 1] << 8) | d[p + 2]); p += 3; }
        else if (b == 29) { if (p + 5 > len) return false; v = (int)rd32(d + p + 1); p += 5; }
        else if (b == 30) {
            p++;
            char buf[48];
            int k = 0;
            bool done = false;
            while (p < len && !done) {
                u8 by = d[p++];
                for (int h = 0; h < 2; h++) {
                    int nb = h == 0 ? by >> 4 : by & 15;
                    if (nb <= 9) { if (k < 46) buf[k++] = (char)('0' + nb); }
                    else if (nb == 10) { if (k < 46) buf[k++] = '.'; }
                    else if (nb == 11) { if (k < 46) buf[k++] = 'E'; }
                    else if (nb == 12) { if (k < 45) { buf[k++] = 'E'; buf[k++] = '-'; } }
                    else if (nb == 14) { if (k < 46) buf[k++] = '-'; }
                    else if (nb == 15) { done = true; break; }
                }
            }
            buf[k] = 0;
            // parse a real
            double mant = 0, frac = 0.1;
            bool neg = false, infrac = false, eneg = false, inexp = false;
            int ex = 0;
            for (int i = 0; buf[i]; i++) {
                char ch = buf[i];
                if (ch == '-') { if (inexp) eneg = true; else neg = true; }
                else if (ch == '.') infrac = true;
                else if (ch == 'E') inexp = true;
                else if (inexp) ex = ex * 10 + (ch - '0');
                else if (infrac) { mant += (ch - '0') * frac; frac *= 0.1; }
                else mant = mant * 10 + (ch - '0');
            }
            v = (neg ? -mant : mant) * m_pow(10, eneg ? -ex : ex);
            if (dv.n < 48) dv.v[dv.n++] = v;
            continue;
        }
        else if (b >= 32 && b <= 246) { v = b - 139; p++; }
        else if (b >= 247 && b <= 250) { if (p + 2 > len) return false; v = (b - 247) * 256 + d[p + 1] + 108; p += 2; }
        else if (b >= 251 && b <= 254) { if (p + 2 > len) return false; v = -(b - 251) * 256 - d[p + 1] - 108; p += 2; }
        else { p++; continue; }
        if (dv.n < 48) dv.v[dv.n++] = v;
    }
    return false;
}

static void cff_read_private(CffFont* c, u32 size, u32 off, CffPriv& pr) {
    if (off + size > c->len) return;
    u32 p = off, end = off + size;
    DictVals dv;
    int op;
    int subrs_off = 0;
    while (cff_dict_next(c->d, end, p, dv, &op)) {
        if (op == 20 && dv.n) pr.defw = dv.v[0];
        else if (op == 21 && dv.n) pr.nomw = dv.v[0];
        else if (op == 19 && dv.n) subrs_off = (int)dv.v[0];
    }
    if (subrs_off > 0 && cff_index(c->d, c->len, off + subrs_off, pr.subrs)) pr.has_subrs = true;
}

CffFont* cff_open(const u8* data, u32 len) {
    if (len < 8 || data[0] != 1) return nullptr;
    CffFont* c = new CffFont();
    c->d = data; c->len = len;
    for (int i = 0; i < 256; i++) c->enc_gid[i] = 0;
    CffIndex names, tops;
    u32 p = data[2];
    if (!cff_index(data, len, p, names) || !cff_index(data, len, names.end, tops) || !cff_index(data, len, tops.end, c->strings) ||
        !cff_index(data, len, c->strings.end, c->gsubrs) || tops.count < 1) { delete c; return nullptr; }
    const u8* td;
    u32 tn;
    if (!cff_item(c, tops, 0, &td, &tn)) { delete c; return nullptr; }
    u32 cs_off = 0, priv_size = 0, priv_off = 0, fda_off = 0;
    DictVals dv;
    int op;
    u32 q = 0;
    while (cff_dict_next(td, tn, q, dv, &op)) {
        switch (op) {
        case 15: if (dv.n) c->charset_off = (u32)dv.v[0]; break;
        case 16: if (dv.n) c->encoding_off = (u32)dv.v[0]; break;
        case 17: if (dv.n) cs_off = (u32)dv.v[0]; break;
        case 18: if (dv.n >= 2) { priv_size = (u32)dv.v[0]; priv_off = (u32)dv.v[1]; } break;
        case 0x0C07: if (dv.n >= 6) { for (int i = 0; i < 6; i++) c->fm[i] = dv.v[i]; c->has_fm = true; } break;
        case 0x0C1E: c->cid = true; break;
        case 0x0C24: if (dv.n) fda_off = (u32)dv.v[0]; break;
        case 0x0C25: if (dv.n) c->fdselect_off = (u32)dv.v[0]; break;
        }
    }
    if (!cs_off || !cff_index(data, len, cs_off, c->charstrings)) { delete c; return nullptr; }
    c->nglyphs = (int)c->charstrings.count;
    if (c->cid && fda_off && cff_index(data, len, fda_off, c->fdarray)) {
        for (u32 i = 0; i < c->fdarray.count; i++) {
            const u8* fd;
            u32 fn;
            CffPriv pr;
            double fmv[6] = {0.001, 0, 0, 0.001, 0, 0};
            bool have = false;
            if (cff_item(c, c->fdarray, i, &fd, &fn)) {
                u32 r = 0;
                u32 ps = 0, po = 0;
                while (cff_dict_next(fd, fn, r, dv, &op)) {
                    if (op == 18 && dv.n >= 2) { ps = (u32)dv.v[0]; po = (u32)dv.v[1]; }
                    else if (op == 0x0C07 && dv.n >= 6) { for (int k = 0; k < 6; k++) fmv[k] = dv.v[k]; have = true; }
                }
                if (ps) cff_read_private(c, ps, po, pr);
            }
            c->fdprivs.push(pr);
            for (int k = 0; k < 6; k++) c->fd_fm.push(have ? fmv[k] : 0);
        }
        if (!c->has_fm && c->fd_fm.len >= 6 && c->fd_fm[0] != 0) { for (int k = 0; k < 6; k++) c->fm[k] = c->fd_fm[k]; c->has_fm = true; }
    } else if (priv_size) cff_read_private(c, priv_size, priv_off, c->priv);
    // charset
    c->sid = (u16*)kcalloc(sizeof(u16) * (c->nglyphs + 1));
    if (c->charset_off <= 2) {
        for (int g = 0; g < c->nglyphs; g++) c->sid[g] = (u16)(c->charset_off == 0 ? g : g);   // ISOAdobe; expert sets are rare
    } else {
        u32 o = c->charset_off;
        if (o < len) {
            int fmt = data[o++];
            int g = 1;
            if (fmt == 0) {
                while (g < c->nglyphs && o + 2 <= len) { c->sid[g++] = (u16)rd16(data + o); o += 2; }
            } else {
                while (g < c->nglyphs && o + 3 <= len) {
                    u32 first = rd16(data + o);
                    u32 left = fmt == 1 ? data[o + 2] : rd16(data + o + 2);
                    o += fmt == 1 ? 3 : 4;
                    for (u32 k = 0; k <= left && g < c->nglyphs; k++) c->sid[g++] = (u16)(first + k);
                }
            }
        }
    }
    // built-in encoding
    if (!c->cid && c->encoding_off > 1 && c->encoding_off < len) {
        u32 o = c->encoding_off;
        int fmt = data[o++];
        c->has_enc = true;
        if ((fmt & 127) == 0) {
            int n = data[o++];
            for (int i = 0; i < n && o < len; i++) { int code = data[o++]; if (i + 1 < c->nglyphs) c->enc_gid[code] = i + 1; }
        } else if ((fmt & 127) == 1) {
            int n = data[o++];
            int g = 1;
            for (int i = 0; i < n && o + 1 < len; i++) {
                int first = data[o++], left = data[o++];
                for (int k = 0; k <= left; k++) { if (first + k < 256 && g < c->nglyphs) c->enc_gid[first + k] = g; g++; }
            }
        }
        if (fmt & 128 && o < len) {
            int n = data[o++];
            for (int i = 0; i < n && o + 2 < len; i++) {
                int code = data[o++];
                u32 sidv = rd16(data + o); o += 2;
                for (int g = 0; g < c->nglyphs; g++) if (c->sid[g] == sidv) { c->enc_gid[code] = g; break; }
            }
        }
    }
    return c;
}

void cff_free(CffFont* c) {
    if (!c) return;
    if (c->sid) kfree(c->sid);
    c->fdprivs.free();
    c->fd_fm.free();
    delete c;
}
int cff_glyph_count(CffFont* c) { return c->nglyphs; }
bool cff_is_cid(CffFont* c) { return c->cid; }
void cff_matrix(CffFont* c, double m[6]) { for (int i = 0; i < 6; i++) m[i] = c->fm[i]; }

const char* cff_glyph_name(CffFont* c, int gid) {
    if (gid < 0 || gid >= c->nglyphs || c->cid) return nullptr;
    u32 s = c->sid[gid];
    if (s < 229) return cff_std_names[s];
    if (s >= 391) {
        const u8* p;
        u32 n;
        if (cff_item(c, c->strings, s - 391, &p, &n)) {
            u32 k = min(n, 23u);
            memcpy(c->tmpname, p, k);
            c->tmpname[k] = 0;
            return c->tmpname;
        }
    }
    return nullptr;
}

int cff_gid_for_name(CffFont* c, const char* name) {
    if (c->cid || !name) return 0;
    for (int g = 0; g < c->nglyphs; g++) {
        u32 s = c->sid[g];
        if (s < 229) { if (!strcmp(cff_std_names[s], name)) return g; }
        else if (s >= 391) {
            const u8* p;
            u32 n;
            if (cff_item(c, c->strings, s - 391, &p, &n) && n == strlen(name) && !memcmp(p, name, n)) return g;
        }
    }
    return 0;
}
int cff_gid_for_cid(CffFont* c, int cid) {
    if (!c->cid) return cid < c->nglyphs ? cid : 0;
    for (int g = 0; g < c->nglyphs; g++) if (c->sid[g] == cid) return g;
    return 0;
}
int cff_gid_for_code(CffFont* c, int code) {
    if (code < 0 || code > 255 || c->cid) return 0;
    if (c->has_enc) return c->enc_gid[code];
    const char* n = std_enc_name(code);          // StandardEncoding (id 0) and fallback
    return n ? cff_gid_for_name(c, n) : 0;
}

struct T2 {
    CffFont* c;
    Path* out;
    double st[64];
    int sp = 0;
    double x = 0, y = 0;
    int nstems = 0;
    bool have_width = false, open = false;
    double width;
    int depth = 0;
    const CffPriv* pr;
    double ox = 0, oy = 0;      // offset for seac accents
    bool done = false;
    int steps = 0;
};

static int subr_bias(u32 n) { return n < 1240 ? 107 : n < 33900 ? 1131 : 32768; }

static void t2_move(T2& t, double dx, double dy) {
    if (t.open) t.out->close();
    t.x += dx; t.y += dy;
    t.out->move((float)(t.x + t.ox), (float)(t.y + t.oy));
    t.open = true;
}
static void t2_line(T2& t, double dx, double dy) {
    if (!t.open) { t.out->move((float)(t.x + t.ox), (float)(t.y + t.oy)); t.open = true; }
    t.x += dx; t.y += dy;
    t.out->line((float)(t.x + t.ox), (float)(t.y + t.oy));
}
static void t2_curve(T2& t, double dx1, double dy1, double dx2, double dy2, double dx3, double dy3) {
    if (!t.open) { t.out->move((float)(t.x + t.ox), (float)(t.y + t.oy)); t.open = true; }
    double x1 = t.x + dx1, y1 = t.y + dy1, x2 = x1 + dx2, y2 = y1 + dy2, x3 = x2 + dx3, y3 = y2 + dy3;
    t.out->curve((float)(x1 + t.ox), (float)(y1 + t.oy), (float)(x2 + t.ox), (float)(y2 + t.oy), (float)(x3 + t.ox), (float)(y3 + t.oy));
    t.x = x3; t.y = y3;
}

static void t2_run(T2& t, const u8* code, u32 n);

static void t2_run(T2& t, const u8* code, u32 n) {
    if (t.depth > 10) return;
    u32 p = 0;
    auto take_width = [&](bool has_extra) {
        if (!t.have_width) {
            t.have_width = true;
            if (has_extra && t.sp > 0) {
                t.width = t.pr->nomw + t.st[0];
                for (int i = 1; i < t.sp; i++) t.st[i - 1] = t.st[i];
                t.sp--;
            } else t.width = t.pr->defw;
        }
    };
    while (p < n && !t.done) {
        if (++t.steps > 200000) { t.done = true; return; }
        u8 b = code[p++];
        if (b >= 32 || b == 28) {
            double v;
            if (b == 28) { if (p + 2 > n) return; v = (i16)((code[p] << 8) | code[p + 1]); p += 2; }
            else if (b <= 246) v = b - 139;
            else if (b <= 250) { if (p >= n) return; v = (b - 247) * 256 + code[p++] + 108; }
            else if (b <= 254) { if (p >= n) return; v = -(b - 251) * 256 - code[p++] - 108; }
            else { if (p + 4 > n) return; v = (int)rd32(code + p) / 65536.0; p += 4; }
            if (t.sp < 63) t.st[t.sp++] = v;
            continue;
        }
        switch (b) {
        case 1: case 3: case 18: case 23:
            take_width(t.sp & 1);
            t.nstems += t.sp / 2;
            t.sp = 0;
            break;
        case 19: case 20: {
            take_width(t.sp & 1);
            t.nstems += t.sp / 2;
            t.sp = 0;
            p += (t.nstems + 7) / 8;
            break;
        }
        case 21: take_width(t.sp > 2); if (t.sp >= 2) t2_move(t, t.st[t.sp - 2], t.st[t.sp - 1]); t.sp = 0; break;
        case 22: take_width(t.sp > 1); if (t.sp >= 1) t2_move(t, t.st[t.sp - 1], 0); t.sp = 0; break;
        case 4: take_width(t.sp > 1); if (t.sp >= 1) t2_move(t, 0, t.st[t.sp - 1]); t.sp = 0; break;
        case 5: for (int i = 0; i + 1 < t.sp; i += 2) t2_line(t, t.st[i], t.st[i + 1]); t.sp = 0; break;
        case 6: case 7: {
            bool h = b == 6;
            for (int i = 0; i < t.sp; i++) { if (h) t2_line(t, t.st[i], 0); else t2_line(t, 0, t.st[i]); h = !h; }
            t.sp = 0;
            break;
        }
        case 8: for (int i = 0; i + 5 < t.sp; i += 6) t2_curve(t, t.st[i], t.st[i + 1], t.st[i + 2], t.st[i + 3], t.st[i + 4], t.st[i + 5]); t.sp = 0; break;
        case 24: {
            int i = 0;
            for (; i + 5 < t.sp - 2; i += 6) t2_curve(t, t.st[i], t.st[i + 1], t.st[i + 2], t.st[i + 3], t.st[i + 4], t.st[i + 5]);
            if (i + 1 < t.sp) t2_line(t, t.st[i], t.st[i + 1]);
            t.sp = 0;
            break;
        }
        case 25: {
            int i = 0;
            for (; i + 1 < t.sp - 6; i += 2) t2_line(t, t.st[i], t.st[i + 1]);
            if (i + 5 < t.sp) t2_curve(t, t.st[i], t.st[i + 1], t.st[i + 2], t.st[i + 3], t.st[i + 4], t.st[i + 5]);
            t.sp = 0;
            break;
        }
        case 26: {
            int i = 0;
            double dx1 = 0;
            if (t.sp & 1) dx1 = t.st[i++];
            for (; i + 3 < t.sp; i += 4) { t2_curve(t, dx1, t.st[i], t.st[i + 1], t.st[i + 2], 0, t.st[i + 3]); dx1 = 0; }
            t.sp = 0;
            break;
        }
        case 27: {
            int i = 0;
            double dy1 = 0;
            if (t.sp & 1) dy1 = t.st[i++];
            for (; i + 3 < t.sp; i += 4) { t2_curve(t, t.st[i], dy1, t.st[i + 1], t.st[i + 2], t.st[i + 3], 0); dy1 = 0; }
            t.sp = 0;
            break;
        }
        case 30: case 31: {
            bool horiz = b == 31;
            int i = 0;
            while (i + 3 < t.sp) {
                double last = (t.sp - i == 5) ? t.st[i + 4] : 0;
                if (horiz) t2_curve(t, t.st[i], 0, t.st[i + 1], t.st[i + 2], last, t.st[i + 3]);
                else t2_curve(t, 0, t.st[i], t.st[i + 1], t.st[i + 2], t.st[i + 3], last);
                horiz = !horiz;
                i += 4;
                if (t.sp - i == 1) break;
            }
            t.sp = 0;
            break;
        }
        case 10: case 29: {
            if (t.sp < 1) break;
            const CffIndex& ix = b == 10 ? t.pr->subrs : t.c->gsubrs;
            if (b == 10 && !t.pr->has_subrs) { t.sp--; break; }
            int idx = (int)t.st[--t.sp] + subr_bias(ix.count);
            const u8* sp;
            u32 sn;
            if (idx >= 0 && cff_item(t.c, ix, (u32)idx, &sp, &sn)) { t.depth++; t2_run(t, sp, sn); t.depth--; }
            break;
        }
        case 11: return;
        case 14: {
            take_width(t.sp == 1 || t.sp == 5);
            if (t.open) { t.out->close(); t.open = false; }
            t.done = true;
            return;
        }
        case 12: {
            if (p >= n) return;
            u8 b2 = code[p++];
            switch (b2) {
            case 35: if (t.sp >= 12) { t2_curve(t, t.st[0], t.st[1], t.st[2], t.st[3], t.st[4], t.st[5]); t2_curve(t, t.st[6], t.st[7], t.st[8], t.st[9], t.st[10], t.st[11]); } t.sp = 0; break;
            case 34: if (t.sp >= 7) {
                t2_curve(t, t.st[0], 0, t.st[1], t.st[2], t.st[3], 0);
                t2_curve(t, t.st[4], 0, t.st[5], -t.st[2], t.st[6], 0);
            } t.sp = 0; break;
            case 36: if (t.sp >= 9) {
                t2_curve(t, t.st[0], t.st[1], t.st[2], t.st[3], t.st[4], 0);
                t2_curve(t, t.st[5], 0, t.st[6], t.st[7], t.st[8], -(t.st[1] + t.st[3] + t.st[7]));
            } t.sp = 0; break;
            case 37: if (t.sp >= 11) {
                double dx = t.st[0] + t.st[2] + t.st[4] + t.st[6] + t.st[8], dy = t.st[1] + t.st[3] + t.st[5] + t.st[7] + t.st[9];
                t2_curve(t, t.st[0], t.st[1], t.st[2], t.st[3], t.st[4], t.st[5]);
                if (m_fabs(dx) > m_fabs(dy)) t2_curve(t, t.st[6], t.st[7], t.st[8], t.st[9], t.st[10], -dy);
                else t2_curve(t, t.st[6], t.st[7], t.st[8], t.st[9], -dx, t.st[10]);
            } t.sp = 0; break;
            default: t.sp = 0; break;
            }
            break;
        }
        default: t.sp = 0; break;
        }
    }
}

static int fd_for_glyph(CffFont* c, int gid) {
    if (!c->fdselect_off || c->fdselect_off >= c->len) return 0;
    const u8* d = c->d;
    u32 o = c->fdselect_off;
    int fmt = d[o++];
    if (fmt == 0) return o + gid < c->len ? d[o + gid] : 0;
    if (fmt == 3) {
        u32 nr = rd16(d + o);
        o += 2;
        for (u32 i = 0; i < nr && o + 5 <= c->len; i++) {
            u32 first = rd16(d + o), nxt = rd16(d + o + 3);
            if ((u32)gid >= first && (u32)gid < nxt) return d[o + 2];
            o += 3;
        }
    }
    return 0;
}

static bool cff_glyph_run(CffFont* c, int gid, Path& out, double* adv, double ox, double oy, int depth) {
    const u8* cs;
    u32 cn;
    if (gid < 0 || !cff_item(c, c->charstrings, (u32)gid, &cs, &cn)) return false;
    T2 t;
    t.c = c;
    t.out = &out;
    t.ox = ox; t.oy = oy;
    int fd = c->cid ? fd_for_glyph(c, gid) : -1;
    t.pr = fd >= 0 && fd < c->fdprivs.len ? &c->fdprivs[fd] : &c->priv;
    t.width = t.pr->defw;
    t2_run(t, cs, cn);
    if (t.open) out.close();
    if (adv) *adv = t.width;
    return true;
}

bool cff_outline(CffFont* c, int gid, Path& out, double* adv) {
    Path raw;
    double w = 0;
    if (!cff_glyph_run(c, gid, raw, &w, 0, 0, 0)) { raw.free(); return false; }
    // endchar-seac: accented glyphs are composed from two standard glyphs
    const u8* cs;
    u32 cn;
    cff_item(c, c->charstrings, (u32)gid, &cs, &cn);
    if (raw.s.len == 0 && cn >= 5 && cs[cn - 1] == 14 && !c->cid) {
        // scan for the seac operands: the four numbers before endchar
        double v[8];
        int nv = 0;
        u32 p = 0;
        while (p < cn - 1) {
            u8 b = cs[p++];
            double x;
            if (b == 28) { x = (i16)((cs[p] << 8) | cs[p + 1]); p += 2; }
            else if (b >= 32 && b <= 246) x = b - 139;
            else if (b >= 247 && b <= 250) x = (b - 247) * 256 + cs[p++] + 108;
            else if (b >= 251 && b <= 254) x = -(b - 251) * 256 - cs[p++] - 108;
            else if (b == 255) { x = (int)rd32(cs + p) / 65536.0; p += 4; }
            else { nv = 0; continue; }
            if (nv < 8) v[nv++] = x; else { for (int i = 1; i < 8; i++) v[i - 1] = v[i]; v[7] = x; }
        }
        if (nv >= 4) {
            double adx = v[nv - 4], ady = v[nv - 3];
            const char* bn = std_enc_name((int)v[nv - 2]);
            const char* an = std_enc_name((int)v[nv - 1]);
            int bg = bn ? cff_gid_for_name(c, bn) : 0, ag = an ? cff_gid_for_name(c, an) : 0;
            if (bg) cff_glyph_run(c, bg, raw, nullptr, 0, 0, 1);
            if (ag) cff_glyph_run(c, ag, raw, nullptr, adx, ady, 1);
        }
    }
    Mat m;
    m.a = c->fm[0]; m.b = c->fm[1]; m.c = c->fm[2]; m.d = c->fm[3]; m.e = c->fm[4]; m.f = c->fm[5];
    out.append(raw, m);
    raw.free();
    if (adv) *adv = w * c->fm[0];
    return true;
}

// ================================================================ Type 1
struct T1Font {
    u8* clear = nullptr; u32 clear_n = 0;
    u8* priv = nullptr; u32 priv_n = 0;
    int lenIV = 4;
    struct Cs { char name[40]; const u8* d; u32 n; };
    Vec<Cs> chars;
    Vec<Cs> subrs;
    char* enc[256];
    bool std_enc = false;
    double fm[6] = {0.001, 0, 0, 0.001, 0, 0};
    u8* decbuf = nullptr;                   // owns decrypted charstring bytes
    u32 decpos = 0, deccap = 0;
};

static void t1_decrypt(const u8* in, u32 n, u8* out, u16 r, int skip, u32* outn) {
    u32 k = 0;
    for (u32 i = 0; i < n; i++) {
        u8 c = in[i];
        u8 pl = (u8)(c ^ (r >> 8));
        r = (u16)((c + r) * 52845 + 22719);
        if ((int)i >= skip) out[k++] = pl;
    }
    *outn = k;
}

static bool is_hexchar(u8 c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static const u8* mem_find(const u8* h, u32 n, const char* nd) {
    u32 l = (u32)strlen(nd);
    for (u32 i = 0; i + l <= n; i++) if (h[i] == (u8)nd[0] && !memcmp(h + i, nd, l)) return h + i;
    return nullptr;
}

T1Font* t1_open(const u8* data, u32 len, u32 len1, u32 len2) {
    // strip PFB segment headers
    u8* flat = nullptr;
    u32 flen = 0;
    u32 seg_clear = 0;
    if (len > 6 && data[0] == 0x80) {
        flat = (u8*)kmalloc(len);
        u32 p = 0;
        bool first_bin = false;
        while (p + 6 <= len && data[p] == 0x80) {
            u8 t = data[p + 1];
            if (t == 3) break;
            u32 sl = data[p + 2] | (data[p + 3] << 8) | (data[p + 4] << 16) | ((u32)data[p + 5] << 24);
            p += 6;
            if (p + sl > len) sl = len - p;
            if (t == 2 && !first_bin) { first_bin = true; seg_clear = flen; }
            memcpy(flat + flen, data + p, sl);
            flen += sl;
            p += sl;
        }
        if (!seg_clear) seg_clear = 0;
        len1 = seg_clear;
    } else {
        flat = (u8*)kmalloc(len);
        memcpy(flat, data, len);
        flen = len;
    }
    const u8* ee = mem_find(flat, flen, "eexec");
    if (!ee) { kfree(flat); return nullptr; }
    u32 cpos = (u32)(ee - flat) + 5;
    while (cpos < flen && (flat[cpos] == ' ' || flat[cpos] == '\r' || flat[cpos] == '\n' || flat[cpos] == '\t')) cpos++;
    T1Font* t = new T1Font();
    for (int i = 0; i < 256; i++) t->enc[i] = nullptr;
    t->clear_n = (u32)(ee - flat);
    t->clear = (u8*)kmalloc(t->clear_n + 1);
    memcpy(t->clear, flat, t->clear_n);
    t->clear[t->clear_n] = 0;
    // encrypted portion
    u32 en = flen - cpos;
    u8* enc = (u8*)kmalloc(en + 1);
    bool hex = true;
    for (u32 i = 0; i < 4 && cpos + i < flen; i++) if (!is_hexchar(flat[cpos + i])) hex = false;
    u32 n = 0;
    if (hex) {
        int hi = -1;
        for (u32 i = 0; i < en; i++) {
            u8 c = flat[cpos + i];
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) continue;
            if (hi < 0) hi = v; else { enc[n++] = (u8)(hi * 16 + v); hi = -1; }
        }
    } else { memcpy(enc, flat + cpos, en); n = en; }
    kfree(flat);
    t->priv = (u8*)kmalloc(n + 8);
    t1_decrypt(enc, n, t->priv, 55665, 4, &t->priv_n);
    kfree(enc);
    t->priv[t->priv_n] = 0;
    // font matrix and encoding from the clear text
    const char* s = (const char*)t->clear;
    const char* fm = strstr_c(s, "/FontMatrix");
    if (fm) {
        const char* b = strchr(fm, '[');
        if (!b) b = strchr(fm, '{');
        if (b) {
            b++;
            for (int i = 0; i < 6; i++) {
                while (*b == ' ') b++;
                bool neg = false;
                if (*b == '-') { neg = true; b++; }
                double v = 0, f = 0.1;
                bool fr = false;
                while ((*b >= '0' && *b <= '9') || *b == '.') { if (*b == '.') fr = true; else if (fr) { v += (*b - '0') * f; f *= 0.1; } else v = v * 10 + (*b - '0'); b++; }
                if (*b == 'e' || *b == 'E') {
                    b++;
                    bool en2 = false;
                    if (*b == '-') { en2 = true; b++; }
                    int ex = 0;
                    while (*b >= '0' && *b <= '9') ex = ex * 10 + (*b++ - '0');
                    v *= m_pow(10, en2 ? -ex : ex);
                }
                t->fm[i] = neg ? -v : v;
            }
        }
    }
    const char* ep = strstr_c(s, "/Encoding");
    if (ep) {
        if (strstr_c(ep, "StandardEncoding") && strstr_c(ep, "StandardEncoding") < strstr_c(ep, "readonly def") + 1000) t->std_enc = true;
        const char* q = ep;
        const char* endp = strstr_c(ep, "currentdict end");
        if (!endp) endp = s + t->clear_n;
        while ((q = strstr_c(q, "dup ")) && q < endp) {
            q += 4;
            int code = 0;
            bool any = false;
            while (*q >= '0' && *q <= '9') { code = code * 10 + (*q - '0'); q++; any = true; }
            if (!any) {
                if (*q == '8' || *q == '1') {}
                continue;
            }
            while (*q == ' ') q++;
            if (*q != '/') continue;
            q++;
            char nm[40];
            int k = 0;
            while (*q && *q != ' ' && *q != '\n' && *q != '\r' && k < 39) nm[k++] = *q++;
            nm[k] = 0;
            if (code >= 0 && code < 256) t->enc[code] = strdup(nm);
        }
        if (strstr_c(ep, "StandardEncoding def") && strstr_c(ep, "StandardEncoding def") < endp) t->std_enc = true;
    }
    // private dictionary: lenIV, Subrs, CharStrings
    const u8* P = t->priv;
    u32 PN = t->priv_n;
    const u8* li = mem_find(P, PN, "/lenIV");
    if (li) t->lenIV = atoi((const char*)li + 6);
    auto skip_ws = [&](u32& p) { while (p < PN && (P[p] == ' ' || P[p] == '\n' || P[p] == '\r' || P[p] == '\t')) p++; };
    auto read_int = [&](u32& p, int* v) -> bool {
        skip_ws(p);
        if (p >= PN || P[p] < '0' || P[p] > '9') return false;
        int x = 0;
        while (p < PN && P[p] >= '0' && P[p] <= '9') x = x * 10 + (P[p++] - '0');
        *v = x;
        return true;
    };
    auto read_tok = [&](u32& p, char* out, int cap) {
        skip_ws(p);
        int k = 0;
        while (p < PN && P[p] != ' ' && P[p] != '\n' && P[p] != '\r' && P[p] != '\t' && k < cap - 1) out[k++] = (char)P[p++];
        out[k] = 0;
    };
    const u8* sp = mem_find(P, PN, "/Subrs");
    if (sp) {
        u32 p = (u32)(sp - P) + 6;
        int cnt;
        if (read_int(p, &cnt)) {
            char arr[16];
            read_tok(p, arr, sizeof(arr));                 // "array"
            for (;;) {
                skip_ws(p);
                if (p + 3 <= PN && !memcmp(P + p, "dup", 3)) {
                    p += 3;
                    int idx, l;
                    if (!read_int(p, &idx) || !read_int(p, &l)) break;
                    char rd[16];
                    read_tok(p, rd, sizeof(rd));
                    p++;                               // single space
                    if (p + l > PN || idx < 0 || idx > 20000) break;
                    T1Font::Cs cs;
                    cs.name[0] = 0;
                    cs.d = P + p; cs.n = (u32)l;
                    while (t->subrs.len <= idx) { T1Font::Cs z; z.name[0] = 0; z.d = nullptr; z.n = 0; t->subrs.push(z); }
                    t->subrs[idx] = cs;
                    p += l;
                    // NP / | / noaccess put
                    char tk[16];
                    read_tok(p, tk, sizeof(tk));
                    if (!strcmp(tk, "noaccess")) read_tok(p, tk, sizeof(tk));
                } else break;
            }
        }
    }
    const u8* cp = mem_find(P, PN, "/CharStrings");
    if (cp) {
        u32 p = (u32)(cp - P) + 12;
        while (p < PN) {
            while (p < PN && P[p] != '/' ) {
                if (p + 3 <= PN && !memcmp(P + p, "end", 3) && (p + 3 >= PN || P[p + 3] <= ' ')) { p = PN; break; }
                p++;
            }
            if (p >= PN) break;
            p++;
            char nm[40];
            int k = 0;
            while (p < PN && P[p] > ' ' && k < 39) nm[k++] = (char)P[p++];
            nm[k] = 0;
            int l;
            if (!read_int(p, &l)) continue;
            char rd[16];
            read_tok(p, rd, sizeof(rd));
            p++;
            if (p + l > PN) break;
            T1Font::Cs cs;
            strlcpy(cs.name, nm, sizeof(cs.name));
            cs.d = P + p; cs.n = (u32)l;
            t->chars.push(cs);
            p += l;
        }
    }
    if (t->chars.len == 0) { t1_free(t); return nullptr; }
    return t;
}

void t1_free(T1Font* t) {
    if (!t) return;
    for (int i = 0; i < 256; i++) if (t->enc[i]) kfree(t->enc[i]);
    if (t->clear) kfree(t->clear);
    if (t->priv) kfree(t->priv);
    if (t->decbuf) kfree(t->decbuf);
    t->chars.free();
    t->subrs.free();
    delete t;
}
void t1_matrix(T1Font* t, double m[6]) { for (int i = 0; i < 6; i++) m[i] = t->fm[i]; }
const char* t1_builtin_name(T1Font* t, int code) {
    if (code < 0 || code > 255) return nullptr;
    if (t->enc[code]) return t->enc[code];
    if (t->std_enc) return std_enc_name(code);
    return nullptr;
}
static const T1Font::Cs* t1_find(T1Font* t, const char* name) {
    for (int i = 0; i < t->chars.len; i++) if (!strcmp(t->chars[i].name, name)) return &t->chars[i];
    return nullptr;
}
bool t1_has_glyph(T1Font* t, const char* name) { return t1_find(t, name) != nullptr; }

struct T1Run {
    T1Font* t;
    Path* out;
    double st[48]; int sp = 0;
    double ps[24]; int psp = 0;
    double x = 0, y = 0, sbx = 0, wx = 0;
    bool open = false;
    bool flex = false;
    double fx[8], fy[8]; int nf = 0;
    int depth = 0;
    double ox = 0, oy = 0;
    bool done = false;
    int steps = 0;
};

static void t1_decode(T1Font* t, const u8* in, u32 n, u8* out, u32* on) {
    t1_decrypt(in, n, out, 4330, t->lenIV < 0 ? 0 : t->lenIV, on);
}

static void t1_run(T1Run& r, const u8* enc, u32 encn);

static void t1_moveto(T1Run& r, double dx, double dy) {
    r.x += dx; r.y += dy;
    if (r.flex) {
        if (r.nf < 8) { r.fx[r.nf] = r.x; r.fy[r.nf] = r.y; r.nf++; }
        return;
    }
    if (r.open) { r.out->close(); r.open = false; }
    r.out->move((float)(r.x + r.ox), (float)(r.y + r.oy));
    r.open = true;
}
static void t1_lineto(T1Run& r, double dx, double dy) {
    if (!r.open) { r.out->move((float)(r.x + r.ox), (float)(r.y + r.oy)); r.open = true; }
    r.x += dx; r.y += dy;
    r.out->line((float)(r.x + r.ox), (float)(r.y + r.oy));
}
static void t1_curveto(T1Run& r, double a, double b, double c, double d, double e, double f) {
    if (!r.open) { r.out->move((float)(r.x + r.ox), (float)(r.y + r.oy)); r.open = true; }
    double x1 = r.x + a, y1 = r.y + b, x2 = x1 + c, y2 = y1 + d, x3 = x2 + e, y3 = y2 + f;
    r.out->curve((float)(x1 + r.ox), (float)(y1 + r.oy), (float)(x2 + r.ox), (float)(y2 + r.oy), (float)(x3 + r.ox), (float)(y3 + r.oy));
    r.x = x3; r.y = y3;
}

static void t1_run(T1Run& r, const u8* enc, u32 encn) {
    if (r.depth > 10) return;
    u8* buf = (u8*)kmalloc(encn + 8);
    u32 n;
    t1_decode(r.t, enc, encn, buf, &n);
    u32 p = 0;
    while (p < n && !r.done) {
        if (++r.steps > 100000) { r.done = true; break; }
        u8 b = buf[p++];
        if (b >= 32) {
            double v;
            if (b <= 246) v = b - 139;
            else if (b <= 250) { if (p >= n) break; v = (b - 247) * 256 + buf[p++] + 108; }
            else if (b <= 254) { if (p >= n) break; v = -(b - 251) * 256 - buf[p++] - 108; }
            else { if (p + 4 > n) break; v = (int)rd32(buf + p); p += 4; }
            if (r.sp < 47) r.st[r.sp++] = v;
            continue;
        }
        switch (b) {
        case 1: case 3: r.sp = 0; break;
        case 4: if (r.sp >= 1) t1_moveto(r, 0, r.st[r.sp - 1]); r.sp = 0; break;
        case 5: if (r.sp >= 2) t1_lineto(r, r.st[0], r.st[1]); r.sp = 0; break;
        case 6: if (r.sp >= 1) t1_lineto(r, r.st[0], 0); r.sp = 0; break;
        case 7: if (r.sp >= 1) t1_lineto(r, 0, r.st[0]); r.sp = 0; break;
        case 8: if (r.sp >= 6) t1_curveto(r, r.st[0], r.st[1], r.st[2], r.st[3], r.st[4], r.st[5]); r.sp = 0; break;
        case 9: if (r.open) { r.out->close(); r.open = false; } r.sp = 0; break;
        case 10: {
            if (r.sp < 1) break;
            int idx = (int)r.st[--r.sp];
            if (idx >= 0 && idx < r.t->subrs.len && r.t->subrs[idx].d) { r.depth++; t1_run(r, r.t->subrs[idx].d, r.t->subrs[idx].n); r.depth--; }
            break;
        }
        case 11: kfree(buf); return;
        case 13: if (r.sp >= 2) { r.sbx = r.st[0]; r.wx = r.st[1]; r.x = r.sbx; r.y = 0; } r.sp = 0; break;
        case 14: if (r.open) { r.out->close(); r.open = false; } r.done = true; break;
        case 21: if (r.sp >= 2) t1_moveto(r, r.st[r.sp - 2], r.st[r.sp - 1]); r.sp = 0; break;
        case 22: if (r.sp >= 1) t1_moveto(r, r.st[r.sp - 1], 0); r.sp = 0; break;
        case 30: if (r.sp >= 4) t1_curveto(r, 0, r.st[0], r.st[1], r.st[2], r.st[3], 0); r.sp = 0; break;
        case 31: if (r.sp >= 4) t1_curveto(r, r.st[0], 0, r.st[1], r.st[2], 0, r.st[3]); r.sp = 0; break;
        case 12: {
            if (p >= n) break;
            u8 b2 = buf[p++];
            switch (b2) {
            case 0: case 1: case 2: r.sp = 0; break;
            case 6: {                                       // seac: asb adx ady bchar achar
                if (r.sp >= 5) {
                    double asb = r.st[0], adx = r.st[1], ady = r.st[2];
                    const char* bn = std_enc_name((int)r.st[3]);
                    const char* an = std_enc_name((int)r.st[4]);
                    double sbx = r.sbx;
                    if (r.open) { r.out->close(); r.open = false; }
                    const T1Font::Cs* bc = bn ? t1_find(r.t, bn) : nullptr;
                    const T1Font::Cs* ac = an ? t1_find(r.t, an) : nullptr;
                    if (bc) { T1Run sub; sub.t = r.t; sub.out = r.out; sub.depth = r.depth + 1; sub.ox = r.ox; sub.oy = r.oy; t1_run(sub, bc->d, bc->n); }
                    if (ac) { T1Run sub; sub.t = r.t; sub.out = r.out; sub.depth = r.depth + 1; sub.ox = r.ox + sbx - asb + adx; sub.oy = r.oy + ady; t1_run(sub, ac->d, ac->n); }
                }
                r.done = true;
                break;
            }
            case 7: if (r.sp >= 4) { r.sbx = r.st[0]; r.wx = r.st[2]; r.x = r.st[0]; r.y = r.st[1]; } r.sp = 0; break;
            case 12: if (r.sp >= 2) { double d = r.st[r.sp - 1], a = r.st[r.sp - 2]; r.sp -= 2; r.st[r.sp++] = d != 0 ? a / d : 0; } break;
            case 16: {                                      // callothersubr
                if (r.sp < 2) { r.sp = 0; break; }
                int num = (int)r.st[--r.sp];
                int na = (int)r.st[--r.sp];
                if (na < 0 || na > r.sp) { r.sp = 0; break; }
                double args[16];
                for (int i = 0; i < na && i < 16; i++) args[i] = r.st[r.sp - 1 - i];     // args[0] was on top
                r.sp -= na;
                if (num == 1) { r.flex = true; r.nf = 0; }
                else if (num == 2) { /* point recorded by the rmoveto */ }
                else if (num == 0) {
                    r.flex = false;
                    if (r.nf >= 7) {
                        if (!r.open) { r.out->move((float)(r.fx[0] + r.ox), (float)(r.fy[0] + r.oy)); r.open = true; }
                        r.out->curve((float)(r.fx[1] + r.ox), (float)(r.fy[1] + r.oy), (float)(r.fx[2] + r.ox), (float)(r.fy[2] + r.oy), (float)(r.fx[3] + r.ox), (float)(r.fy[3] + r.oy));
                        r.out->curve((float)(r.fx[4] + r.ox), (float)(r.fy[4] + r.oy), (float)(r.fx[5] + r.ox), (float)(r.fy[5] + r.oy), (float)(r.fx[6] + r.ox), (float)(r.fy[6] + r.oy));
                        r.x = r.fx[6]; r.y = r.fy[6];
                    }
                    r.psp = 0;
                    r.ps[r.psp++] = r.y;
                    r.ps[r.psp++] = r.x;
                } else {
                    r.psp = 0;
                    for (int i = 0; i < na && i < 16 && r.psp < 24; i++) r.ps[r.psp++] = args[i];
                    if (num == 3 && na >= 1) { r.psp = 0; r.ps[r.psp++] = args[0]; }
                }
                break;
            }
            case 17: if (r.psp > 0 && r.sp < 47) r.st[r.sp++] = r.ps[--r.psp]; else if (r.sp < 47) r.st[r.sp++] = 0; break;
            case 33: if (r.sp >= 2) { r.x = r.st[r.sp - 2]; r.y = r.st[r.sp - 1]; } r.sp = 0; break;
            default: r.sp = 0; break;
            }
            break;
        }
        default: r.sp = 0; break;
        }
    }
    kfree(buf);
}

bool t1_outline(T1Font* t, const char* name, Path& out, double* adv) {
    const T1Font::Cs* c = t1_find(t, name);
    if (!c) return false;
    Path raw;
    T1Run r;
    r.t = t;
    r.out = &raw;
    t1_run(r, c->d, c->n);
    if (r.open) raw.close();
    Mat m;
    m.a = t->fm[0]; m.b = t->fm[1]; m.c = t->fm[2]; m.d = t->fm[3]; m.e = t->fm[4]; m.f = t->fm[5];
    out.append(raw, m);
    raw.free();
    if (adv) *adv = r.wx * t->fm[0];
    return true;
}
