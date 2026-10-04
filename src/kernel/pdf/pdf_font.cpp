// silicon - PDF fonts: font dictionaries, encodings, CMaps, widths and glyph lookup
#include "pdf_gfx.h"
#include "../text.h"

// ================================================================ glyph names
struct GName { const char* name; u16 uni; };
static const GName g_names[] = {
    {"space", 32}, {"exclam", 33}, {"quotedbl", 34}, {"numbersign", 35}, {"dollar", 36}, {"percent", 37}, {"ampersand", 38}, {"quotesingle", 39},
    {"parenleft", 40}, {"parenright", 41}, {"asterisk", 42}, {"plus", 43}, {"comma", 44}, {"hyphen", 45}, {"period", 46}, {"slash", 47},
    {"zero", 48}, {"one", 49}, {"two", 50}, {"three", 51}, {"four", 52}, {"five", 53}, {"six", 54}, {"seven", 55}, {"eight", 56}, {"nine", 57},
    {"colon", 58}, {"semicolon", 59}, {"less", 60}, {"equal", 61}, {"greater", 62}, {"question", 63}, {"at", 64},
    {"bracketleft", 91}, {"backslash", 92}, {"bracketright", 93}, {"asciicircum", 94}, {"underscore", 95}, {"grave", 96},
    {"braceleft", 123}, {"bar", 124}, {"braceright", 125}, {"asciitilde", 126},
    {"nbspace", 160}, {"nonbreakingspace", 160}, {"exclamdown", 161}, {"cent", 162}, {"sterling", 163}, {"currency", 164}, {"yen", 165}, {"brokenbar", 166},
    {"section", 167}, {"dieresis", 168}, {"copyright", 169}, {"ordfeminine", 170}, {"guillemotleft", 171}, {"logicalnot", 172}, {"sfthyphen", 173},
    {"registered", 174}, {"macron", 175}, {"degree", 176}, {"plusminus", 177}, {"twosuperior", 178}, {"threesuperior", 179}, {"acute", 180},
    {"mu", 181}, {"paragraph", 182}, {"periodcentered", 183}, {"cedilla", 184}, {"onesuperior", 185}, {"ordmasculine", 186}, {"guillemotright", 187},
    {"onequarter", 188}, {"onehalf", 189}, {"threequarters", 190}, {"questiondown", 191},
    {"Agrave", 192}, {"Aacute", 193}, {"Acircumflex", 194}, {"Atilde", 195}, {"Adieresis", 196}, {"Aring", 197}, {"AE", 198}, {"Ccedilla", 199},
    {"Egrave", 200}, {"Eacute", 201}, {"Ecircumflex", 202}, {"Edieresis", 203}, {"Igrave", 204}, {"Iacute", 205}, {"Icircumflex", 206}, {"Idieresis", 207},
    {"Eth", 208}, {"Ntilde", 209}, {"Ograve", 210}, {"Oacute", 211}, {"Ocircumflex", 212}, {"Otilde", 213}, {"Odieresis", 214}, {"multiply", 215},
    {"Oslash", 216}, {"Ugrave", 217}, {"Uacute", 218}, {"Ucircumflex", 219}, {"Udieresis", 220}, {"Yacute", 221}, {"Thorn", 222}, {"germandbls", 223},
    {"agrave", 224}, {"aacute", 225}, {"acircumflex", 226}, {"atilde", 227}, {"adieresis", 228}, {"aring", 229}, {"ae", 230}, {"ccedilla", 231},
    {"egrave", 232}, {"eacute", 233}, {"ecircumflex", 234}, {"edieresis", 235}, {"igrave", 236}, {"iacute", 237}, {"icircumflex", 238}, {"idieresis", 239},
    {"eth", 240}, {"ntilde", 241}, {"ograve", 242}, {"oacute", 243}, {"ocircumflex", 244}, {"otilde", 245}, {"odieresis", 246}, {"divide", 247},
    {"oslash", 248}, {"ugrave", 249}, {"uacute", 250}, {"ucircumflex", 251}, {"udieresis", 252}, {"yacute", 253}, {"thorn", 254}, {"ydieresis", 255},
    {"Euro", 0x20AC}, {"quoteleft", 0x2018}, {"quoteright", 0x2019}, {"quotedblleft", 0x201C}, {"quotedblright", 0x201D}, {"quotesinglbase", 0x201A},
    {"quotedblbase", 0x201E}, {"endash", 0x2013}, {"emdash", 0x2014}, {"bullet", 0x2022}, {"ellipsis", 0x2026}, {"dagger", 0x2020}, {"daggerdbl", 0x2021},
    {"perthousand", 0x2030}, {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A}, {"fi", 0xFB01}, {"fl", 0xFB02}, {"ff", 0xFB00}, {"ffi", 0xFB03},
    {"ffl", 0xFB04}, {"trademark", 0x2122}, {"OE", 0x152}, {"oe", 0x153}, {"Scaron", 0x160}, {"scaron", 0x161}, {"Zcaron", 0x17D}, {"zcaron", 0x17E},
    {"Ydieresis", 0x178}, {"florin", 0x192}, {"circumflex", 0x2C6}, {"tilde", 0x2DC}, {"caron", 0x2C7}, {"breve", 0x2D8}, {"dotaccent", 0x2D9},
    {"ring", 0x2DA}, {"ogonek", 0x2DB}, {"hungarumlaut", 0x2DD}, {"fraction", 0x2044}, {"minus", 0x2212}, {"dotlessi", 0x131}, {"Lslash", 0x141},
    {"lslash", 0x142}, {"summation", 0x2211}, {"product", 0x220F}, {"pi", 0x3C0}, {"partialdiff", 0x2202}, {"radical", 0x221A}, {"infinity", 0x221E},
    {"integral", 0x222B}, {"approxequal", 0x2248}, {"notequal", 0x2260}, {"lessequal", 0x2264}, {"greaterequal", 0x2265}, {"lozenge", 0x25CA},
    {"Omega", 0x3A9}, {"Delta", 0x2206}, {"apple", 0xF8FF}, {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4}, {"epsilon", 0x3B5},
    {"zeta", 0x3B6}, {"eta", 0x3B7}, {"theta", 0x3B8}, {"iota", 0x3B9}, {"kappa", 0x3BA}, {"lambda", 0x3BB}, {"nu", 0x3BD}, {"xi", 0x3BE},
    {"omicron", 0x3BF}, {"rho", 0x3C1}, {"sigma", 0x3C3}, {"tau", 0x3C4}, {"upsilon", 0x3C5}, {"phi", 0x3C6}, {"chi", 0x3C7}, {"psi", 0x3C8},
    {"omega", 0x3C9}, {"Gamma", 0x393}, {"Theta", 0x398}, {"Lambda", 0x39B}, {"Xi", 0x39E}, {"Pi", 0x3A0}, {"Sigma", 0x3A3}, {"Upsilon", 0x3A5},
    {"Phi", 0x3A6}, {"Psi", 0x3A8}, {"arrowright", 0x2192}, {"arrowleft", 0x2190}, {"arrowup", 0x2191}, {"arrowdown", 0x2193}, {"arrowboth", 0x2194},
    {"element", 0x2208}, {"emptyset", 0x2205}, {"universal", 0x2200}, {"existential", 0x2203}, {"logicaland", 0x2227}, {"logicalor", 0x2228},
    {"intersection", 0x2229}, {"union", 0x222A}, {"propersubset", 0x2282}, {"reflexsubset", 0x2286}, {"equivalence", 0x2261}, {"proportional", 0x221D},
    {"asteriskmath", 0x2217}, {"periodcentered", 0xB7}, {"bulletoperator", 0x2219}, {"minusplus", 0x2213}, {"circlemultiply", 0x2297}, {"circleplus", 0x2295},
    {"angleleft", 0x2329}, {"angleright", 0x232A}, {"nabla", 0x2207}, {"similar", 0x223C}, {"congruent", 0x2245}, {"orthogonal", 0x22A5},
    {"Scedilla", 0x15E}, {"scedilla", 0x15F}, {"Gbreve", 0x11E}, {"gbreve", 0x11F}, {"Idotaccent", 0x130}, {"Cacute", 0x106}, {"cacute", 0x107},
    {"Ccaron", 0x10C}, {"ccaron", 0x10D}, {"Dcaron", 0x10E}, {"dcaron", 0x10F}, {"Ecaron", 0x11A}, {"ecaron", 0x11B}, {"Nacute", 0x143}, {"nacute", 0x144},
    {"Ncaron", 0x147}, {"ncaron", 0x148}, {"Rcaron", 0x158}, {"rcaron", 0x159}, {"Sacute", 0x15A}, {"sacute", 0x15B}, {"Tcaron", 0x164}, {"tcaron", 0x165},
    {"Uring", 0x16E}, {"uring", 0x16F}, {"Zacute", 0x179}, {"zacute", 0x17A}, {"Zdotaccent", 0x17B}, {"zdotaccent", 0x17C}, {"Aogonek", 0x104},
    {"aogonek", 0x105}, {"Eogonek", 0x118}, {"eogonek", 0x119}, {"Odblacute", 0x150}, {"odblacute", 0x151}, {"Udblacute", 0x170}, {"udblacute", 0x171},
    {"quotereversed", 0x201B}, {"onethird", 0x2153}, {"twothirds", 0x2154}, {"oneeighth", 0x215B}, {"checkmark", 0x2713}, {"ballotx", 0x2717},
};
static const int N_GNAMES = ARRAY_LEN(g_names);

static const char* ascii_names[95] = {
    "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quotesingle", "parenleft", "parenright", "asterisk", "plus", "comma",
    "hyphen", "period", "slash", "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "colon", "semicolon", "less", "equal",
    "greater", "question", "at", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
    "Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum", "underscore", "grave", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",
    "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "braceleft", "bar", "braceright", "asciitilde"};

static int hexd(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

int glyph_name_to_uni(const char* name) {
    if (!name || !name[0]) return -1;
    char nm[48];
    int k = 0;
    for (; name[k] && name[k] != '.' && k < 47; k++) nm[k] = name[k];
    nm[k] = 0;
    if (!nm[0]) return -1;
    if (nm[1] == 0 && nm[0] >= 32 && nm[0] < 127) return nm[0];
    for (int i = 0; i < 95; i++) if (!strcmp(nm, ascii_names[i])) return 32 + i;
    for (int i = 0; i < N_GNAMES; i++) if (!strcmp(nm, g_names[i].name)) return g_names[i].uni;
    if (nm[0] == 'u' && nm[1] == 'n' && nm[2] == 'i' && strlen(nm) >= 7) {
        int v = 0;
        for (int i = 3; i < 7; i++) { int h = hexd(nm[i]); if (h < 0) return -1; v = v * 16 + h; }
        return v;
    }
    if (nm[0] == 'u' && strlen(nm) >= 5 && strlen(nm) <= 7) {
        int v = 0;
        for (int i = 1; nm[i]; i++) { int h = hexd(nm[i]); if (h < 0) return -1; v = v * 16 + h; }
        return v;
    }
    return -1;
}

const char* uni_to_glyph_name(u32 cp) {
    if (cp >= 32 && cp < 127) return ascii_names[cp - 32];
    for (int i = 0; i < N_GNAMES; i++) if (g_names[i].uni == cp) return g_names[i].name;
    return nullptr;
}

// ================================================================ encodings (code -> unicode)
static const u16 std_hi[96] = {   // codes 160..255
    0, 0xA1, 0xA2, 0xA3, 0x2044, 0xA5, 0x192, 0xA7, 0xA4, 0x27, 0x201C, 0xAB, 0x2039, 0x203A, 0xFB01, 0xFB02, 0, 0x2013, 0x2020, 0x2021, 0xB7, 0, 0xB6, 0x2022,
    0x201A, 0x201E, 0x201D, 0xBB, 0x2026, 0x2030, 0, 0xBF, 0, 0x60, 0xB4, 0x2C6, 0x2DC, 0xAF, 0x2D8, 0x2D9, 0xA8, 0, 0x2DA, 0xB8, 0, 0x2DD, 0x2DB, 0x2C7,
    0x2014, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xC6, 0, 0xAA, 0, 0, 0, 0, 0x141, 0xD8, 0x152, 0xBA, 0, 0, 0, 0,
    0, 0xE6, 0, 0, 0, 0x131, 0, 0, 0x142, 0xF8, 0x153, 0xDF, 0, 0, 0, 0};
static const u16 win_80[32] = {0x20AC, 0, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0, 0x17D, 0,
                               0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0, 0x17E, 0x178};
static const u16 mac_80[128] = {
    0xC4, 0xC5, 0xC7, 0xC9, 0xD1, 0xD6, 0xDC, 0xE1, 0xE0, 0xE2, 0xE4, 0xE3, 0xE5, 0xE7, 0xE9, 0xE8, 0xEA, 0xEB, 0xED, 0xEC, 0xEE, 0xEF, 0xF1, 0xF3, 0xF2, 0xF4, 0xF6, 0xF5, 0xFA, 0xF9, 0xFB, 0xFC,
    0x2020, 0xB0, 0xA2, 0xA3, 0xA7, 0x2022, 0xB6, 0xDF, 0xAE, 0xA9, 0x2122, 0xB4, 0xA8, 0x2260, 0xC6, 0xD8, 0x221E, 0xB1, 0x2264, 0x2265, 0xA5, 0xB5, 0x2202, 0x2211, 0x220F, 0x3C0, 0x222B, 0xAA, 0xBA, 0x3A9, 0xE6, 0xF8,
    0xBF, 0xA1, 0xAC, 0x221A, 0x192, 0x2248, 0x2206, 0xAB, 0xBB, 0x2026, 0xA0, 0xC0, 0xC3, 0xD5, 0x152, 0x153, 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0xF7, 0x25CA, 0xFF, 0x178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0xB7, 0x201A, 0x201E, 0x2030, 0xC2, 0xCA, 0xC1, 0xCB, 0xC8, 0xCD, 0xCE, 0xCF, 0xCC, 0xD3, 0xD4, 0xF8FF, 0xD2, 0xDA, 0xDB, 0xD9, 0x131, 0x2C6, 0x2DC, 0xAF, 0x2D8, 0x2D9, 0x2DA, 0xB8, 0x2DD, 0x2DB, 0x2C7};

enum { ENC_STD, ENC_WIN, ENC_MAC, ENC_NONE };
static void fill_encoding(int enc, u32* uni) {
    for (int c = 0; c < 256; c++) {
        u32 u = 0;
        if (c >= 32 && c < 127) u = c;
        if (enc == ENC_STD) {
            if (c == 39) u = 0x2019;
            else if (c == 96) u = 0x2018;
            else if (c >= 160) u = std_hi[c - 160];
            else if (c >= 127) u = 0;
        } else if (enc == ENC_WIN) {
            if (c >= 128 && c < 160) u = win_80[c - 128];
            else if (c >= 160) u = c;
            if (c == 127) u = 0x2022;
        } else if (enc == ENC_MAC) {
            if (c >= 128) u = mac_80[c - 128];
        } else u = 0;
        uni[c] = u;
    }
}

const char* std_enc_name(int code) {
    static u32 u[256];
    static bool init = false;
    if (!init) { fill_encoding(ENC_STD, u); init = true; }
    if (code < 0 || code > 255 || !u[code]) return nullptr;
    return uni_to_glyph_name(u[code]);
}

static int mac_code_for(u32 u) {
    if (u < 128) return (int)u;
    for (int i = 0; i < 128; i++) if (mac_80[i] == u) return 128 + i;
    return -1;
}

// ================================================================ CMaps
struct CMapRange { u32 lo, hi, dst; u8 nb; };
struct CMapMulti { u32 code; u32 cps[4]; u8 n; };
struct CMap {
    Vec<CMapRange> cs;            // codespace ranges
    Vec<CMapRange> map;           // code -> cid (dst) or unicode
    Vec<CMapMulti> multi;         // bfchar with several characters
    bool has_cs = false;
    int wmode = 0;
    bool identity = false;
    ~CMap() { cs.free(); map.free(); multi.free(); }
};

static u32 str_val(Obj* o, int* nb) {
    u32 v = 0;
    if (!o || o->type != O_STR) { *nb = 0; return 0; }
    *nb = (int)min(o->str.len, 4u);
    for (u32 i = 0; i < o->str.len && i < 4; i++) v = (v << 8) | o->str.s[i];
    return v;
}
static u32 utf16_first(Obj* o, u32* cps, u8* n) {
    *n = 0;
    if (!o || o->type != O_STR) return 0;
    const u8* s = o->str.s;
    u32 len = o->str.len;
    if (len == 1) { cps[(*n)++] = s[0]; return cps[0]; }
    for (u32 i = 0; i + 1 < len && *n < 4; i += 2) {
        u32 cp = (s[i] << 8) | s[i + 1];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < len) {
            u32 lo = (s[i + 2] << 8) | s[i + 3];
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        cps[(*n)++] = cp;
    }
    return *n ? cps[0] : 0;
}

static CMap* cmap_parse(PdfDoc* d, const u8* data, u32 len, bool unicode) {
    CMap* m = new CMap();
    u32 pos = 0;
    Vec<Obj*> st;
    for (;;) {
        Obj* o = parse_obj_at(d, data, len, &pos, false);
        if (!o) break;
        if (o->type != O_KEY) { st.push(o); if (st.len > 4000) st.clear(); continue; }
        const char* k = (const char*)o->str.s;
        if (!strcmp(k, "endcodespacerange")) {
            for (int i = 0; i + 1 < st.len; i += 2) {
                int n1, n2;
                CMapRange r;
                r.lo = str_val(st[i], &n1); r.hi = str_val(st[i + 1], &n2); r.nb = (u8)n1; r.dst = 0;
                if (n1 >= 1) { m->cs.push(r); m->has_cs = true; }
            }
        } else if (!strcmp(k, "endcidrange") || (!unicode && !strcmp(k, "endbfrange"))) {
            for (int i = 0; i + 2 < st.len; i += 3) {
                int n1, n2;
                CMapRange r;
                r.lo = str_val(st[i], &n1); r.hi = str_val(st[i + 1], &n2);
                r.dst = st[i + 2]->type == O_NUM ? (u32)st[i + 2]->n : 0;
                r.nb = (u8)n1;
                m->map.push(r);
            }
        } else if (!strcmp(k, "endcidchar")) {
            for (int i = 0; i + 1 < st.len; i += 2) {
                int n1;
                CMapRange r;
                r.lo = r.hi = str_val(st[i], &n1);
                r.dst = st[i + 1]->type == O_NUM ? (u32)st[i + 1]->n : 0;
                r.nb = (u8)n1;
                m->map.push(r);
            }
        } else if (unicode && !strcmp(k, "endbfchar")) {
            for (int i = 0; i + 1 < st.len; i += 2) {
                int n1;
                u32 cps[4];
                u8 nc;
                u32 code = str_val(st[i], &n1);
                u32 first = utf16_first(st[i + 1], cps, &nc);
                CMapRange r; r.lo = r.hi = code; r.dst = first; r.nb = (u8)n1;
                m->map.push(r);
                if (nc > 1) { CMapMulti mm; mm.code = code; mm.n = nc; for (int q = 0; q < 4; q++) mm.cps[q] = q < nc ? cps[q] : 0; m->multi.push(mm); }
            }
        } else if (unicode && !strcmp(k, "endbfrange")) {
            for (int i = 0; i + 2 < st.len; i += 3) {
                int n1, n2;
                u32 lo = str_val(st[i], &n1), hi = str_val(st[i + 1], &n2);
                if (st[i + 2]->type == O_ARR) {
                    for (u32 q = 0; q < st[i + 2]->arr.n && lo + q <= hi; q++) {
                        u32 cps[4]; u8 nc;
                        u32 first = utf16_first(st[i + 2]->arr.v[q], cps, &nc);
                        CMapRange r; r.lo = r.hi = lo + q; r.dst = first; r.nb = (u8)n1;
                        m->map.push(r);
                        if (nc > 1) { CMapMulti mm; mm.code = lo + q; mm.n = nc; for (int z = 0; z < 4; z++) mm.cps[z] = z < nc ? cps[z] : 0; m->multi.push(mm); }
                    }
                } else {
                    u32 cps[4]; u8 nc;
                    u32 first = utf16_first(st[i + 2], cps, &nc);
                    CMapRange r; r.lo = lo; r.hi = hi; r.dst = first; r.nb = (u8)n1;
                    m->map.push(r);
                }
            }
        } else if (!strcmp(k, "def")) {
            if (st.len >= 2 && is_name(st[st.len - 2], "WMode")) m->wmode = (int)onum(st[st.len - 1], 0);
        }
        st.clear();
    }
    st.free();
    return m;
}

static bool cmap_lookup(const CMap* m, u32 code, u32* out) {
    for (int i = m->map.len - 1; i >= 0; i--) {
        const CMapRange& r = m->map[i];
        if (code >= r.lo && code <= r.hi) { *out = r.dst + (code - r.lo); return true; }
    }
    return false;
}

// ================================================================ the font
struct GCEnt { u32 code; int next; bool ok; Path path; };
struct CidW { u32 lo, hi; float w; };

struct PdfFont {
    PdfDoc* d = nullptr;
    Obj* dict = nullptr;
    int kind = 0;                  // 0 simple, 1 Type0, 3 Type3
    char base[64] = "";
    bool italic = false, bold = false, serif = false, mono = false, symbolic = false;
    double ascent = 0.9, descent = -0.25;
    // simple font
    float wd[256];
    double missing = -1;
    u32 uni[256];
    const char* gname[256];
    bool has_encoding = false, enc_differences[256];
    // font program
    int fmt = 0;                   // 0 none, 1 truetype, 2 cff, 3 type1
    u8* fdata = nullptr;
    TTFont* tt = nullptr;
    CffFont* cff = nullptr;
    T1Font* t1 = nullptr;
    // CID
    bool cid = false, vertical = false;
    CMap* cmap = nullptr;
    bool cmap_identity = true;
    u8* cid2gid = nullptr;
    u32 cid2gid_n = 0;
    double dw = 1000;
    Vec<CidW> cw;
    double dw2[2] = {880, -1000};
    CMap* tou = nullptr;
    bool cid_truetype = false;
    // type 3
    Obj* t3procs = nullptr;
    Obj* t3res = nullptr;
    double fm[6] = {0.001, 0, 0, 0.001, 0, 0};
    Obj* t3names[256];
    // glyph cache
    Vec<GCEnt> gc;
    int gbucket[512];
    ~PdfFont() {
        for (int i = 0; i < gc.len; i++) gc[i].path.free();
        gc.free();
        if (tt) tt_free(tt);
        if (cff) cff_free(cff);
        if (t1) t1_free(t1);
        if (fdata) kfree(fdata);
        if (cmap) delete cmap;
        if (tou) delete tou;
        if (cid2gid) kfree(cid2gid);
        cw.free();
    }
};

void pdf_font_free(PdfFont* f) { delete f; }
bool font_is_type3(PdfFont* f) { return f->kind == 3; }
bool font_vertical(PdfFont* f) { return f->vertical; }
bool font_italic(PdfFont* f) { return f->italic; }
bool font_bold(PdfFont* f) { return f->bold; }
double font_ascent(PdfFont* f) { return f->ascent; }
double font_descent(PdfFont* f) { return f->descent; }
Obj* font_type3_dict(PdfFont* f) { return f->t3res; }
void font_type3_matrix(PdfFont* f, double m[6]) { for (int i = 0; i < 6; i++) m[i] = f->fm[i]; }
bool font_has_outlines(PdfFont* f) { return f->fmt != 0; }
Obj* font_type3_proc(PdfFont* f, u32 code) { return code < 256 ? f->t3names[code] : nullptr; }

// Helvetica / Times metrics of ASCII for fonts that come without widths
static const u16 helv_w[95] = {278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,
                               1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,
                               333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};
static const u16 times_w[95] = {250, 333, 408, 500, 500, 833, 778, 180, 333, 333, 500, 564, 250, 333, 250, 278, 500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 278, 278, 564, 564, 564, 444,
                                921, 722, 667, 667, 722, 611, 556, 722, 722, 333, 389, 722, 611, 889, 722, 722, 556, 722, 667, 556, 611, 722, 722, 944, 722, 722, 611, 333, 278, 333, 469, 500,
                                333, 444, 500, 444, 500, 444, 333, 500, 500, 278, 278, 500, 278, 778, 500, 500, 500, 500, 333, 389, 278, 500, 500, 722, 500, 500, 444, 480, 200, 480, 541};

static double std_width(PdfFont* f, u32 code) {
    if (f->mono) return 0.6;
    u32 u = code < 256 ? f->uni[code] : 0;
    if (u >= 32 && u < 127) return (f->serif ? times_w[u - 32] : helv_w[u - 32]) / 1000.0;
    if (u == 0xA0) return f->serif ? 0.25 : 0.278;
    return f->serif ? 0.5 : 0.556;
}

static void set_style_from_name(PdfFont* f) {
    const char* b = f->base;
    const char* plus = strchr(b, '+');
    if (plus && plus - b == 6) b = plus + 1;
    f->italic = f->italic || str_icontains(b, "italic") || str_icontains(b, "oblique") || str_icontains(b, "-it") || str_icontains(b, ",it");
    f->bold = f->bold || str_icontains(b, "bold") || str_icontains(b, "black") || str_icontains(b, "heavy") || str_icontains(b, "semibold");
    if (str_icontains(b, "courier") || str_icontains(b, "mono") || str_icontains(b, "consolas") || str_icontains(b, "typewriter")) f->mono = true;
    if (str_icontains(b, "times") || str_icontains(b, "serif") || str_icontains(b, "georgia") || str_icontains(b, "garamond") || str_icontains(b, "palatino") ||
        str_icontains(b, "bookman") || str_icontains(b, "cmr") || str_icontains(b, "minion") || str_icontains(b, "century") || str_icontains(b, "roman"))
        f->serif = true;
    if (str_icontains(b, "sans") || str_icontains(b, "arial") || str_icontains(b, "helvetica") || str_icontains(b, "calibri") || str_icontains(b, "verdana")) f->serif = false;
}

static void load_font_program(PdfFont* f, Obj* desc) {
    if (!desc) return;
    Obj* ff = dget(f->d, desc, "FontFile");
    Obj* ff2 = dget(f->d, desc, "FontFile2");
    Obj* ff3 = dget(f->d, desc, "FontFile3");
    Obj* s = ff2 ? ff2 : ff3 ? ff3 : ff;
    if (!s || s->type != O_STREAM) return;
    u32 len;
    u8* data = pdf_stream_copy(f->d, s, &len);
    if (!data || len < 4) { if (data) kfree(data); return; }
    f->fdata = data;
    if (data[0] == 0 && data[1] == 1 && data[2] == 0 && data[3] == 0 || !memcmp(data, "true", 4) || !memcmp(data, "OTTO", 4) || !memcmp(data, "ttcf", 4)) {
        f->tt = tt_open(data, len, 0);
        if (f->tt) {
            if (tt_is_cff(f->tt)) {
                u32 cl;
                const u8* ct = tt_cff_table(f->tt, &cl);
                if (ct) f->cff = cff_open(ct, cl);
                f->fmt = f->cff ? 2 : 0;
            } else f->fmt = 1;
        }
    } else if (data[0] == 1 && data[1] == 0 && ff3) {
        f->cff = cff_open(data, len);
        f->fmt = f->cff ? 2 : 0;
    } else if (data[0] == '%' || data[0] == 0x80) {
        u32 l1 = (u32)onum(dget(f->d, s, "Length1"), 0), l2 = (u32)onum(dget(f->d, s, "Length2"), 0);
        f->t1 = t1_open(data, len, l1, l2);
        f->fmt = f->t1 ? 3 : 0;
    } else if (data[0] == 1 && data[1] == 0) {
        f->cff = cff_open(data, len);
        f->fmt = f->cff ? 2 : 0;
    }
    if (f->fmt == 0) { kfree(f->fdata); f->fdata = nullptr; }
}

static void read_descriptor(PdfFont* f, Obj* desc) {
    if (!desc) return;
    int flags = (int)onum(dget(f->d, desc, "Flags"), 0);
    if (flags & 1) f->mono = true;
    if (flags & 2) f->serif = true;
    if (flags & 4) f->symbolic = true;
    if (flags & 64) f->italic = true;
    if (flags & 0x40000) f->bold = true;
    f->missing = onum(dget(f->d, desc, "MissingWidth"), -1);
    double asc = onum(dget(f->d, desc, "Ascent"), 0), dsc = onum(dget(f->d, desc, "Descent"), 0);
    if (asc > 100) f->ascent = asc / 1000.0;
    if (dsc < -20) f->descent = dsc / 1000.0;
    if (onum(dget(f->d, desc, "ItalicAngle"), 0) != 0) f->italic = true;
    if (onum(dget(f->d, desc, "StemV"), 0) >= 140) f->bold = true;
}

static void setup_simple(PdfFont* f, Obj* fd, Obj* desc) {
    PdfDoc* d = f->d;
    for (int i = 0; i < 256; i++) { f->wd[i] = -1; f->uni[i] = 0; f->gname[i] = nullptr; f->enc_differences[i] = false; }
    Obj* enc = dget(d, fd, "Encoding");
    int base = -1;
    Obj* diffs = nullptr;
    if (enc) {
        if (enc->type == O_NAME) {
            const char* e = (const char*)enc->str.s;
            base = !strcmp(e, "WinAnsiEncoding") ? ENC_WIN : !strcmp(e, "MacRomanEncoding") ? ENC_MAC : !strcmp(e, "StandardEncoding") ? ENC_STD : !strcmp(e, "MacExpertEncoding") ? ENC_STD : -1;
        } else if (is_dict(enc)) {
            Obj* be = dget(d, enc, "BaseEncoding");
            if (be && be->type == O_NAME) {
                const char* e = (const char*)be->str.s;
                base = !strcmp(e, "WinAnsiEncoding") ? ENC_WIN : !strcmp(e, "MacRomanEncoding") ? ENC_MAC : !strcmp(e, "StandardEncoding") ? ENC_STD : -1;
            }
            diffs = dget(d, enc, "Differences");
        }
        f->has_encoding = base >= 0 || diffs;
    }
    bool embedded_symbolic_builtin = false;
    if (base < 0) {
        // no base: the font's own encoding for embedded Type1 / CFF / symbolic fonts, StandardEncoding otherwise
        if (f->fmt == 3 || f->fmt == 2) embedded_symbolic_builtin = true;
        base = f->fmt == 1 && !f->symbolic ? ENC_WIN : f->symbolic && f->fmt == 1 ? ENC_NONE : ENC_STD;
        if (f->fmt == 0 && f->symbolic) base = ENC_STD;
    }
    fill_encoding(base, f->uni);
    for (int c = 0; c < 256; c++) f->gname[c] = f->uni[c] ? uni_to_glyph_name(f->uni[c]) : nullptr;
    if (embedded_symbolic_builtin) {
        for (int c = 0; c < 256; c++) {
            const char* n = nullptr;
            if (f->fmt == 3) n = t1_builtin_name(f->t1, c);
            else if (f->fmt == 2 && f->cff) { int g = cff_gid_for_code(f->cff, c); if (g > 0) n = cff_glyph_name(f->cff, g); }
            if (n && strcmp(n, ".notdef")) { f->gname[c] = n; int u = glyph_name_to_uni(n); f->uni[c] = u > 0 ? (u32)u : 0; }
        }
    }
    if (diffs && diffs->type == O_ARR) {
        int code = 0;
        for (u32 i = 0; i < diffs->arr.n; i++) {
            Obj* o = aget(d, diffs, i);
            if (!o) continue;
            if (o->type == O_NUM) code = (int)o->n;
            else if (o->type == O_NAME && code >= 0 && code < 256) {
                f->gname[code] = (const char*)o->str.s;
                int u = glyph_name_to_uni((const char*)o->str.s);
                f->uni[code] = u > 0 ? (u32)u : 0;
                f->enc_differences[code] = true;
                code++;
            }
        }
    }
    // widths
    int first = (int)onum(dget(d, fd, "FirstChar"), 0);
    Obj* ws = dget(d, fd, "Widths");
    if (ws && ws->type == O_ARR) {
        for (u32 i = 0; i < ws->arr.n; i++) {
            int c = first + (int)i;
            if (c >= 0 && c < 256) f->wd[c] = (float)(onum(aget(d, ws, i), 0) / 1000.0);
        }
    }
}

static void setup_type3(PdfFont* f, Obj* fd) {
    PdfDoc* d = f->d;
    f->kind = 3;
    for (int i = 0; i < 256; i++) { f->wd[i] = 0; f->uni[i] = 0; f->gname[i] = nullptr; f->t3names[i] = nullptr; }
    Obj* fm = dget(d, fd, "FontMatrix");
    if (fm && fm->type == O_ARR && fm->arr.n >= 6) for (int i = 0; i < 6; i++) f->fm[i] = onum(aget(d, fm, i), f->fm[i]);
    f->t3procs = dget(d, fd, "CharProcs");
    f->t3res = dget(d, fd, "Resources");
    Obj* enc = dget(d, fd, "Encoding");
    fill_encoding(ENC_STD, f->uni);
    if (is_dict(enc)) {
        Obj* diffs = dget(d, enc, "Differences");
        int code = 0;
        for (u32 i = 0; diffs && i < diffs->arr.n; i++) {
            Obj* o = aget(d, diffs, i);
            if (!o) continue;
            if (o->type == O_NUM) code = (int)o->n;
            else if (o->type == O_NAME && code >= 0 && code < 256) {
                f->gname[code] = (const char*)o->str.s;
                int u = glyph_name_to_uni((const char*)o->str.s);
                f->uni[code] = u > 0 ? (u32)u : 0;
                f->t3names[code] = dget(d, f->t3procs, (const char*)o->str.s);
                code++;
            }
        }
    }
    int first = (int)onum(dget(d, fd, "FirstChar"), 0);
    Obj* ws = dget(d, fd, "Widths");
    for (u32 i = 0; ws && i < ws->arr.n; i++) {
        int c = first + (int)i;
        if (c >= 0 && c < 256) f->wd[c] = (float)(onum(aget(d, ws, i), 0) * f->fm[0]);
    }
}

static void sort_cw(Vec<CidW>& v) {
    for (int i = 1; i < v.len; i++) {
        CidW x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j].lo > x.lo) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
}

static void setup_cid(PdfFont* f, Obj* fd) {
    PdfDoc* d = f->d;
    f->kind = 1;
    f->cid = true;
    Obj* desc_arr = dget(d, fd, "DescendantFonts");
    Obj* df = desc_arr && desc_arr->type == O_ARR ? aget(d, desc_arr, 0) : desc_arr;
    if (!is_dict(df)) df = fd;
    const char* bf = oname(dget(d, df, "BaseFont"));
    if (bf[0]) strlcpy(f->base, bf, sizeof(f->base));
    Obj* desc = dget(d, df, "FontDescriptor");
    read_descriptor(f, desc);
    load_font_program(f, desc);
    set_style_from_name(f);
    f->cid_truetype = is_name(dget(d, df, "Subtype"), "CIDFontType2");
    Obj* enc = dget(d, fd, "Encoding");
    f->cmap_identity = true;
    if (enc && enc->type == O_NAME) {
        const char* e = (const char*)enc->str.s;
        f->vertical = !strcmp(e, "Identity-V") || (strlen(e) > 2 && !strcmp(e + strlen(e) - 2, "-V"));
    } else if (enc && enc->type == O_STREAM) {
        u32 len;
        u8* data = pdf_stream_copy(d, enc, &len);
        f->cmap = cmap_parse(d, data, len, false);
        kfree(data);
        f->cmap_identity = false;
        f->vertical = f->cmap->wmode == 1 || onum(dget(d, enc, "WMode"), 0) == 1;
    }
    f->dw = onum(dget(d, df, "DW"), 1000);
    Obj* W = dget(d, df, "W");
    if (W && W->type == O_ARR) {
        for (u32 i = 0; i < W->arr.n;) {
            Obj* a = aget(d, W, i);
            Obj* b = aget(d, W, i + 1);
            if (!a || a->type != O_NUM || !b) break;
            if (b->type == O_ARR) {
                for (u32 k = 0; k < b->arr.n; k++) { CidW w; w.lo = w.hi = (u32)a->n + k; w.w = (float)onum(aget(d, b, k), 0); f->cw.push(w); }
                i += 2;
            } else {
                Obj* c = aget(d, W, i + 2);
                CidW w; w.lo = (u32)a->n; w.hi = (u32)onum(b, 0); w.w = (float)onum(c, 0);
                f->cw.push(w);
                i += 3;
            }
        }
        sort_cw(f->cw);
    }
    Obj* dw2 = dget(d, df, "DW2");
    if (dw2 && dw2->type == O_ARR && dw2->arr.n >= 2) { f->dw2[0] = onum(aget(d, dw2, 0), 880); f->dw2[1] = onum(aget(d, dw2, 1), -1000); }
    Obj* c2g = dget(d, df, "CIDToGIDMap");
    if (c2g && c2g->type == O_STREAM) f->cid2gid = pdf_stream_copy(d, c2g, &f->cid2gid_n);
    Obj* tu = dget(d, fd, "ToUnicode");
    if (tu && tu->type == O_STREAM) {
        u32 len;
        u8* data = pdf_stream_copy(d, tu, &len);
        f->tou = cmap_parse(d, data, len, true);
        kfree(data);
    }
}

PdfFont* pdf_font_load(PdfDoc* d, Obj* fd) {
    for (int i = 0; i < d->fonts.len; i++) if (d->fonts[i].dict == fd) return d->fonts[i].font;
    PdfFont* f = new PdfFont();
    f->d = d;
    f->dict = fd;
    for (int i = 0; i < 512; i++) f->gbucket[i] = -1;
    for (int i = 0; i < 256; i++) { f->wd[i] = -1; f->uni[i] = 0; f->gname[i] = nullptr; f->t3names[i] = nullptr; f->enc_differences[i] = false; }
    const char* sub = oname(dget(d, fd, "Subtype"));
    const char* bf = oname(dget(d, fd, "BaseFont"));
    strlcpy(f->base, bf, sizeof(f->base));
    set_style_from_name(f);
    if (!strcmp(sub, "Type0")) {
        setup_cid(f, fd);
    } else if (!strcmp(sub, "Type3")) {
        setup_type3(f, fd);
    } else {
        Obj* desc = dget(d, fd, "FontDescriptor");
        read_descriptor(f, desc);
        load_font_program(f, desc);
        set_style_from_name(f);
        if (!strncmp(f->base, "Symbol", 6) || !strncmp(f->base, "ZapfDingbats", 12)) f->symbolic = true;
        setup_simple(f, fd, desc);
        Obj* tu = dget(d, fd, "ToUnicode");
        if (tu && tu->type == O_STREAM) {
            u32 len;
            u8* data = pdf_stream_copy(d, tu, &len);
            f->tou = cmap_parse(d, data, len, true);
            kfree(data);
        }
    }
    PdfDoc::FontEnt e;
    e.dict = fd;
    e.font = f;
    d->fonts.push(e);
    return f;
}

// ================================================================ codes and widths
int font_next_code(PdfFont* f, const u8* s, int n, int pos, u32* code) {
    if (!f->cid) { *code = s[pos]; return 1; }
    if (f->cmap && f->cmap->has_cs) {
        for (int len = 1; len <= 4 && pos + len <= n; len++) {
            u32 v = 0;
            for (int i = 0; i < len; i++) v = (v << 8) | s[pos + i];
            for (int i = 0; i < f->cmap->cs.len; i++) {
                const CMapRange& r = f->cmap->cs[i];
                if (r.nb == len && v >= r.lo && v <= r.hi) { *code = v; return len; }
            }
        }
        *code = s[pos];
        return 1;
    }
    if (pos + 1 < n) { *code = (s[pos] << 8) | s[pos + 1]; return 2; }
    *code = s[pos];
    return 1;
}

static u32 code_to_cid(PdfFont* f, u32 code) {
    if (f->cmap) { u32 c; if (cmap_lookup(f->cmap, code, &c)) return c; return 0; }
    return code;
}

static float cid_width(PdfFont* f, u32 cid) {
    int lo = 0, hi = f->cw.len - 1;
    // ranges are sorted by lo but may overlap, so scan back from the candidate
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->cw[mid].lo <= cid) lo = mid + 1; else hi = mid - 1;
    }
    for (int i = hi; i >= 0 && i > hi - 8; i--) if (cid >= f->cw[i].lo && cid <= f->cw[i].hi) return f->cw[i].w;
    return (float)f->dw;
}

double font_width(PdfFont* f, u32 code) {
    if (f->cid) return cid_width(f, code_to_cid(f, code)) / 1000.0;
    if (code > 255) return 0.5;
    if (f->kind == 3) return f->wd[code];
    if (f->wd[code] >= 0) return f->wd[code];
    // no Widths entry: the embedded program knows
    if (f->missing >= 0 && f->fmt != 0) return f->missing / 1000.0;
    if (f->fmt == 0) return std_width(f, code);
    if (f->missing >= 0) return f->missing / 1000.0;
    return std_width(f, code);
}

void font_vmetrics(PdfFont* f, u32 code, double* w1, double* vx, double* vy) {
    double w0 = font_width(f, code);
    *w1 = f->dw2[1] / 1000.0;
    *vx = w0 / 2;
    *vy = f->dw2[0] / 1000.0;
}

int font_unicodes(PdfFont* f, u32 code, u32* out) {
    if (f->tou) {
        for (int i = 0; i < f->tou->multi.len; i++)
            if (f->tou->multi[i].code == code) { for (int k = 0; k < f->tou->multi[i].n; k++) out[k] = f->tou->multi[i].cps[k]; return f->tou->multi[i].n; }
        u32 u;
        if (cmap_lookup(f->tou, code, &u) && u) { out[0] = u; return 1; }
    }
    if (f->cid) {
        if (f->tou) return 0;
        out[0] = code;                           // Identity encodings of unembedded fonts: best effort
        return code ? 1 : 0;
    }
    if (code < 256 && f->uni[code]) { out[0] = f->uni[code]; return 1; }
    return 0;
}

u32 font_unicode(PdfFont* f, u32 code) {
    u32 o[4];
    return font_unicodes(f, code, o) ? o[0] : 0;
}

bool font_fallback_cell(PdfFont* f, u32 code, int* ch) {
    u32 u = font_unicode(f, code);
    if (!u) u = code < 128 ? code : 0;
    if (u == 0 || u == 32 || u == 0xA0) return false;
    char g[4];
    int n = glyph_for(u, g);
    if (n != 1) { if (n == 2) { *ch = '?'; return true; } return false; }
    *ch = (u8)g[0];
    return true;
}

// ================================================================ outlines
static int find_gid(PdfFont* f, u32 code) {
    if (f->cid) {
        u32 cid = code_to_cid(f, code);
        if (f->fmt == 1) {
            if (f->cid2gid) return cid * 2 + 1 < f->cid2gid_n ? (f->cid2gid[cid * 2] << 8) | f->cid2gid[cid * 2 + 1] : 0;
            return (int)cid;
        }
        if (f->fmt == 2) {
            if (cff_is_cid(f->cff)) return cff_gid_for_cid(f->cff, (int)cid);
            if (f->cid2gid) return cid * 2 + 1 < f->cid2gid_n ? (f->cid2gid[cid * 2] << 8) | f->cid2gid[cid * 2 + 1] : 0;
            return (int)cid;
        }
        return 0;
    }
    if (code > 255) return 0;
    if (f->fmt == 1) {
        TTFont* t = f->tt;
        u32 u = f->uni[code];
        int g = 0;
        bool nonsym_enc = f->has_encoding && !f->symbolic;
        if (nonsym_enc || (u && !f->symbolic)) {
            if (u) {
                g = tt_cmap_lookup(t, 3, 1, u);
                if (!g) g = tt_cmap_lookup(t, 0, -1, u);
                if (!g) { int mc = mac_code_for(u); if (mc >= 0) g = tt_cmap_lookup(t, 1, 0, (u32)mc); }
            }
            if (!g && f->gname[code]) g = tt_post_glyph(t, f->gname[code]);
            if (g) return g;
        }
        // symbolic: use the code through the cmaps the font offers
        if (tt_has_cmap(t, 3, 0)) {
            g = tt_cmap_lookup(t, 3, 0, code);
            if (!g) g = tt_cmap_lookup(t, 3, 0, 0xF000 + code);
            if (!g) g = tt_cmap_lookup(t, 3, 0, 0xF100 + code);
            if (!g) g = tt_cmap_lookup(t, 3, 0, 0xF200 + code);
            if (g) return g;
        }
        g = tt_cmap_lookup(t, 1, 0, code);
        if (g) return g;
        if (u) { g = tt_cmap_lookup(t, 3, 1, u); if (g) return g; }
        g = tt_cmap_lookup(t, 3, 1, code);
        if (!g && f->gname[code]) g = tt_post_glyph(t, f->gname[code]);
        return g;
    }
    if (f->fmt == 2) {
        if (cff_is_cid(f->cff)) return code < (u32)cff_glyph_count(f->cff) ? (int)code : 0;
        int g = 0;
        if (f->gname[code]) g = cff_gid_for_name(f->cff, f->gname[code]);
        if (!g && f->uni[code]) {
            // the name in the font may differ (uniXXXX, aliases)
            const char* nm = uni_to_glyph_name(f->uni[code]);
            if (nm) g = cff_gid_for_name(f->cff, nm);
        }
        if (!g && !f->enc_differences[code]) g = cff_gid_for_code(f->cff, (int)code);
        if (!g && f->tt) { u32 u = f->uni[code]; g = u ? tt_cmap_lookup(f->tt, 3, 1, u) : 0; }
        return g;
    }
    return 0;
}

static GCEnt* gc_find(PdfFont* f, u32 code) {
    int b = (int)(code & 511);
    for (int i = f->gbucket[b]; i >= 0; i = f->gc[i].next) if (f->gc[i].code == code) return &f->gc[i];
    return nullptr;
}

bool font_glyph(PdfFont* f, u32 code, Path& out) {
    if (f->fmt == 0 || f->kind == 3) return false;
    GCEnt* e = gc_find(f, code);
    if (!e) {
        GCEnt ne;
        ne.code = code;
        ne.ok = false;
        int b = (int)(code & 511);
        ne.next = f->gbucket[b];
        if (f->fmt == 3) {
            const char* nm = code < 256 ? f->gname[code] : nullptr;
            double adv;
            if (nm) ne.ok = t1_outline(f->t1, nm, ne.path, &adv);
            if (!ne.ok && code < 256 && f->uni[code]) {
                const char* alt = uni_to_glyph_name(f->uni[code]);
                if (alt && (!nm || strcmp(alt, nm))) ne.ok = t1_outline(f->t1, alt, ne.path, &adv);
            }
            if (!ne.ok && code < 256 && !f->enc_differences[code]) {
                const char* bn = t1_builtin_name(f->t1, (int)code);
                if (bn) ne.ok = t1_outline(f->t1, bn, ne.path, &adv);
            }
        } else {
            int gid = find_gid(f, code);
            if (gid > 0 || (gid == 0 && f->cid)) {
                if (f->fmt == 1) ne.ok = tt_outline(f->tt, gid, ne.path);
                else { double adv; ne.ok = cff_outline(f->cff, gid, ne.path, &adv); }
            }
        }
        f->gc.push(ne);
        f->gbucket[b] = f->gc.len - 1;
        e = &f->gc[f->gc.len - 1];
    }
    if (!e->ok) return false;
    for (int i = 0; i < e->path.s.len; i++) out.s.push(e->path.s[i]);
    return true;
}
