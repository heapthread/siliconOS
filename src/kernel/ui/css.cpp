// silicon - CSS engine for Beam.
// Parses style sheets (rules, @media, custom properties, !important), indexes selectors by
// their rightmost id / class / tag, matches them right-to-left against the element stack and
// cascades the declarations into a CssComputed the HTML layout applies.
#include "css.h"

static const char* sstr(const char* h, const char* n) {
    int l = (int)strlen(n);
    for (; *h; h++) if (!strncmp(h, n, l)) return h;
    return nullptr;
}

// ================================================================ data
struct Compound {
    u32 tag, id;
    u32 cls[4];
    u32 attr[3], attr_val[3];         // attr_val 0 = presence only
    u8  ncls, nattr;
    u8  comb;                         // relation to the compound on the left: 1 descendant, 2 child
    u8  flags;                        // 1 :link, 2 :root
};
struct Selector { int comp0; u8 ncomp; u32 spec; int rule; int next; };
struct Decl { u8 prop; u8 important; int val; };
struct Rule { int decl0, ndecl; int media; int cache_vw; i8 cache_ok; };

enum Prop : u8 {
    P_NONE, P_DISPLAY, P_VISIBILITY, P_COLOR, P_BG, P_BG_COLOR, P_FONT_WEIGHT, P_FONT_STYLE, P_FONT_SIZE,
    P_FONT_FAMILY, P_FONT, P_TEXT_ALIGN, P_TEXT_DECORATION, P_TEXT_TRANSFORM, P_WHITE_SPACE, P_LIST_STYLE,
    P_MARGIN, P_MARGIN_T, P_MARGIN_R, P_MARGIN_B, P_MARGIN_L, P_PADDING, P_PADDING_T, P_PADDING_R, P_PADDING_B,
    P_PADDING_L, P_WIDTH, P_MAX_WIDTH, P_HEIGHT, P_POSITION, P_LEFT, P_TOP, P_BORDER, P_BORDER_T, P_BORDER_R,
    P_BORDER_B, P_BORDER_L, P_BORDER_WIDTH, P_BORDER_STYLE, P_BORDER_COLOR, P_FLEX_DIRECTION, P_FLOAT, P_CLIP,
    P_CLIP_PATH, P_OVERFLOW, P_MARGIN_INLINE, P_PADDING_INLINE, P_PADDING_BLOCK, P_MARGIN_BLOCK,
};
static const struct { const char* name; u8 prop; } PROPS[] = {
    {"display", P_DISPLAY}, {"visibility", P_VISIBILITY}, {"color", P_COLOR}, {"background", P_BG},
    {"background-color", P_BG_COLOR}, {"font-weight", P_FONT_WEIGHT}, {"font-style", P_FONT_STYLE},
    {"font-size", P_FONT_SIZE}, {"font-family", P_FONT_FAMILY}, {"font", P_FONT}, {"text-align", P_TEXT_ALIGN},
    {"text-decoration", P_TEXT_DECORATION}, {"text-decoration-line", P_TEXT_DECORATION},
    {"text-transform", P_TEXT_TRANSFORM}, {"white-space", P_WHITE_SPACE}, {"list-style", P_LIST_STYLE},
    {"list-style-type", P_LIST_STYLE}, {"margin", P_MARGIN}, {"margin-top", P_MARGIN_T}, {"margin-right", P_MARGIN_R},
    {"margin-bottom", P_MARGIN_B}, {"margin-left", P_MARGIN_L}, {"padding", P_PADDING}, {"padding-top", P_PADDING_T},
    {"padding-right", P_PADDING_R}, {"padding-bottom", P_PADDING_B}, {"padding-left", P_PADDING_L},
    {"width", P_WIDTH}, {"max-width", P_MAX_WIDTH}, {"height", P_HEIGHT}, {"position", P_POSITION}, {"left", P_LEFT},
    {"top", P_TOP}, {"border", P_BORDER}, {"border-top", P_BORDER_T}, {"border-right", P_BORDER_R},
    {"border-bottom", P_BORDER_B}, {"border-left", P_BORDER_L}, {"border-width", P_BORDER_WIDTH},
    {"border-style", P_BORDER_STYLE}, {"border-color", P_BORDER_COLOR}, {"flex-direction", P_FLEX_DIRECTION},
    {"float", P_FLOAT}, {"clip", P_CLIP}, {"clip-path", P_CLIP_PATH}, {"overflow", P_OVERFLOW},
    {"margin-inline", P_MARGIN_INLINE}, {"padding-inline", P_PADDING_INLINE}, {"padding-block", P_PADDING_BLOCK},
    {"margin-block", P_MARGIN_BLOCK}, {"margin-inline-start", P_MARGIN_L}, {"margin-inline-end", P_MARGIN_R},
    {"padding-inline-start", P_PADDING_L}, {"padding-inline-end", P_PADDING_R}, {"margin-block-start", P_MARGIN_T},
    {"margin-block-end", P_MARGIN_B}, {"padding-block-start", P_PADDING_T}, {"padding-block-end", P_PADDING_B},
    {"inset-inline-start", P_LEFT},
};

struct CssSheet {
    Vec<Compound> comps;
    Vec<Selector> sels;
    Vec<Decl> decls;
    Vec<Rule> rules;
    char* pool = nullptr;
    int pool_len = 0, pool_cap = 0;
    Vec<u32> var_name;
    Vec<int> var_val;
    // selector index: open addressing table of (key -> first selector)
    u32* hkeys = nullptr;
    int* hhead = nullptr;
    int hcap = 0;
    int universal = -1;
    bool dirty = true;
};

u32 css_hash(const char* s, int n) {
    u32 h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (u8)s[i]; h *= 16777619u; }
    return h ? h : 1;
}
u32 css_hash_lower(const char* s, int n) {
    u32 h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (u8)to_lower(s[i]); h *= 16777619u; }
    return h ? h : 1;
}

static int pool_add(CssSheet* s, const char* t, int n) {
    if (s->pool_len + n + 1 > s->pool_cap) {
        int nc = max(s->pool_cap * 2, s->pool_len + n + 8192);
        s->pool = (char*)krealloc(s->pool, nc);
        s->pool_cap = nc;
    }
    int off = s->pool_len;
    memcpy(s->pool + off, t, n);
    s->pool[off + n] = 0;
    s->pool_len += n + 1;
    return off;
}

CssSheet* css_new() { return new CssSheet(); }
int css_rule_count(CssSheet* s) { return s ? s->rules.len : 0; }
void css_free(CssSheet* s) {
    if (!s) return;
    s->comps.free(); s->sels.free(); s->decls.free(); s->rules.free();
    s->var_name.free(); s->var_val.free();
    if (s->pool) kfree(s->pool);
    if (s->hkeys) kfree(s->hkeys);
    if (s->hhead) kfree(s->hhead);
    delete s;
}

// ================================================================ helpers
static bool is_ws(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f'; }
static bool is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || (u8)c >= 0x80 || c == '\\';
}
static const char* skip_ws(const char* p, const char* e) { while (p < e && is_ws(*p)) p++; return p; }

// find the matching close brace for the block that starts after *p == '{'
static const char* block_end(const char* p, const char* e) {
    int depth = 0;
    char q = 0;
    for (; p < e; p++) {
        if (q) { if (*p == '\\') p++; else if (*p == q) q = 0; continue; }
        if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '{') depth++;
        else if (*p == '}') { if (--depth == 0) return p; }
    }
    return e;
}

// ================================================================ lengths and colours
static bool starts_ci(const char* s, const char* p) { return str_ieq_prefix(s, p); }

// parse a CSS length; returns false for things we can't evaluate (calc, auto, ...)
static bool parse_len(const char* v, int pct_base, int vw, int* out) {
    while (is_ws(*v)) v++;
    if (starts_ci(v, "calc(") || starts_ci(v, "min(") || starts_ci(v, "max(") || starts_ci(v, "clamp(")) {
        // take the first plain length inside; good enough for "clamp(1rem, 2vw, 2rem)" and friends
        const char* p = strchr(v, '(') + 1;
        while (*p && !((*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) p++;
        if (!*p) return false;
        return parse_len(p, pct_base, vw, out);
    }
    bool neg = false;
    if (*v == '-') { neg = true; v++; } else if (*v == '+') v++;
    if (!((*v >= '0' && *v <= '9') || *v == '.')) return false;
    int ip = 0, frac = 0, fdiv = 1;
    while (*v >= '0' && *v <= '9') { ip = ip * 10 + (*v - '0'); v++; if (ip > 100000) ip = 100000; }
    if (*v == '.') { v++; while (*v >= '0' && *v <= '9') { if (fdiv < 1000) { frac = frac * 10 + (*v - '0'); fdiv *= 10; } v++; } }
    int milli = ip * 1000 + frac * 1000 / fdiv;      // value * 1000
    int px;
    if (starts_ci(v, "px")) px = milli / 1000;
    else if (starts_ci(v, "rem") || starts_ci(v, "em")) px = milli * 16 / 1000;
    else if (starts_ci(v, "pt")) px = milli * 4 / 3000;
    else if (starts_ci(v, "%")) px = (int)((i64)milli * pct_base / 100000);
    else if (starts_ci(v, "vw") || starts_ci(v, "vi")) px = (int)((i64)milli * vw / 100000);
    else if (starts_ci(v, "vh") || starts_ci(v, "vb")) px = milli * 6 / 1000;
    else if (starts_ci(v, "ch") || starts_ci(v, "ex")) px = milli * 8 / 1000;
    else if (starts_ci(v, "cm")) px = milli * 38 / 1000;
    else if (starts_ci(v, "mm")) px = milli * 38 / 10000;
    else if (starts_ci(v, "in")) px = milli * 96 / 1000;
    else if (!*v || is_ws(*v) || *v == ';' || *v == '!' || *v == ',' || *v == ')' || *v == '/') px = milli / 1000;   // unitless
    else return false;
    *out = neg ? -px : px;
    return true;
}

static int hex1(char c) { c = to_lower(c); return c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= '0' && c <= '9' ? c - '0' : -1; }

static int read_num(const char*& p, bool* pct) {
    while (is_ws(*p) || *p == ',' || *p == '/') p++;
    bool neg = *p == '-';
    if (neg || *p == '+') p++;
    int ip = 0, fr = 0, fd = 1;
    while (*p >= '0' && *p <= '9') ip = ip * 10 + (*p++ - '0');
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') { if (fd < 1000) { fr = fr * 10 + (*p - '0'); fd *= 10; } p++; } }
    int v = ip * 1000 + fr * 1000 / fd;
    *pct = *p == '%';
    if (*pct) p++;
    else if (starts_ci(p, "deg")) p += 3;
    return neg ? -v : v;                    // x1000
}

static u32 hsl_to_rgb(int h, int s, int l) {   // h degrees, s/l 0..1000
    h = ((h % 360) + 360) % 360;
    int c = (1000 - iabs(2 * l - 1000)) * s / 1000;
    int hp = h * 1000 / 60;
    int x = c * (1000 - iabs(hp % 2000 - 1000)) / 1000;
    int r = 0, g = 0, b = 0;
    if (hp < 1000) { r = c; g = x; } else if (hp < 2000) { r = x; g = c; } else if (hp < 3000) { g = c; b = x; }
    else if (hp < 4000) { g = x; b = c; } else if (hp < 5000) { r = x; b = c; } else { r = c; b = x; }
    int m = l - c / 2;
    auto ch = [&](int v) { return (u32)clamp((v + m) * 255 / 1000, 0, 255); };
    return (ch(r) << 16) | (ch(g) << 8) | ch(b);
}

bool css_parse_color(const char* v, u32& out) {
    while (is_ws(*v)) v++;
    if (*v == '#') {
        v++;
        int d[8], n = 0;
        while (n < 8 && hex1(v[n]) >= 0) { d[n] = hex1(v[n]); n++; }
        if (is_ident(v[n])) return false;
        u32 r, g, b, a = 255;
        if (n == 3 || n == 4) { r = d[0] * 17; g = d[1] * 17; b = d[2] * 17; if (n == 4) a = d[3] * 17; }
        else if (n == 6 || n == 8) { r = d[0] * 16 + d[1]; g = d[2] * 16 + d[3]; b = d[4] * 16 + d[5]; if (n == 8) a = d[6] * 16 + d[7]; }
        else return false;
        out = (a << 24) | (r << 16) | (g << 8) | b;
        return true;
    }
    if (starts_ci(v, "rgb")) {
        const char* p = strchr(v, '(');
        if (!p) return false;
        p++;
        bool pct;
        int c[4] = {0, 0, 0, 1000};
        for (int i = 0; i < 4; i++) {
            const char* q = p;
            while (is_ws(*q) || *q == ',' || *q == '/') q++;
            if (*q == ')' || !*q) break;
            c[i] = read_num(p, &pct);
            if (i < 3) c[i] = pct ? c[i] * 255 / 100000 : c[i] / 1000;
            else if (pct) c[i] /= 100;
        }
        out = ((u32)clamp(c[3] * 255 / 1000, 0, 255) << 24) | ((u32)clamp(c[0], 0, 255) << 16) |
              ((u32)clamp(c[1], 0, 255) << 8) | (u32)clamp(c[2], 0, 255);
        return true;
    }
    if (starts_ci(v, "hsl")) {
        const char* p = strchr(v, '(');
        if (!p) return false;
        p++;
        bool pct;
        int h = read_num(p, &pct) / 1000, s = read_num(p, &pct) / 100, l = read_num(p, &pct) / 100;
        int a = 1000;
        const char* q = p;
        while (is_ws(*q) || *q == ',' || *q == '/') q++;
        if (*q && *q != ')') { a = read_num(p, &pct); if (pct) a /= 100; }
        out = ((u32)clamp(a * 255 / 1000, 0, 255) << 24) | hsl_to_rgb(h, s, l);
        return true;
    }
    if (starts_ci(v, "transparent")) { out = 0; return true; }
    static const struct { const char* n; u32 c; } names[] = {
        {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000}, {"green", 0x008000}, {"blue", 0x0000FF},
        {"navy", 0x000080}, {"maroon", 0x800000}, {"purple", 0x800080}, {"teal", 0x008080}, {"gray", 0x808080},
        {"grey", 0x808080}, {"silver", 0xC0C0C0}, {"yellow", 0xFFFF00}, {"orange", 0xFFA500}, {"olive", 0x808000},
        {"lime", 0x00FF00}, {"aqua", 0x00FFFF}, {"cyan", 0x00FFFF}, {"fuchsia", 0xFF00FF}, {"magenta", 0xFF00FF},
        {"brown", 0xA52A2A}, {"darkred", 0x8B0000}, {"darkblue", 0x00008B}, {"darkgreen", 0x006400},
        {"darkgray", 0xA9A9A9}, {"darkgrey", 0xA9A9A9}, {"lightgray", 0xD3D3D3}, {"lightgrey", 0xD3D3D3},
        {"dimgray", 0x696969}, {"dimgrey", 0x696969}, {"whitesmoke", 0xF5F5F5}, {"gainsboro", 0xDCDCDC},
        {"steelblue", 0x4682B4}, {"slategray", 0x708090}, {"darkslategray", 0x2F4F4F}, {"crimson", 0xDC143C},
        {"gold", 0xFFD700}, {"pink", 0xFFC0CB}, {"indigo", 0x4B0082}, {"violet", 0xEE82EE}, {"coral", 0xFF7F50},
        {"tomato", 0xFF6347}, {"orangered", 0xFF4500}, {"royalblue", 0x4169E1}, {"dodgerblue", 0x1E90FF},
        {"skyblue", 0x87CEEB}, {"lightblue", 0xADD8E6}, {"beige", 0xF5F5DC}, {"ivory", 0xFFFFF0},
        {"linen", 0xFAF0E6}, {"tan", 0xD2B48C}, {"khaki", 0xF0E68C}, {"salmon", 0xFA8072}, {"seagreen", 0x2E8B57},
        {"forestgreen", 0x228B22}, {"midnightblue", 0x191970}, {"aliceblue", 0xF0F8FF}, {"ghostwhite", 0xF8F8FF},
        {"lavender", 0xE6E6FA}, {"snow", 0xFFFAFA}, {"honeydew", 0xF0FFF0}, {"mintcream", 0xF5FFFA},
        {"azure", 0xF0FFFF}, {"oldlace", 0xFDF5E6}, {"cornsilk", 0xFFF8DC}, {"lightyellow", 0xFFFFE0},
        {"darkorange", 0xFF8C00}, {"firebrick", 0xB22222}, {"chocolate", 0xD2691E}, {"sienna", 0xA0522D},
        {"rebeccapurple", 0x663399}, {"darkviolet", 0x9400D3}, {"slateblue", 0x6A5ACD}, {"cornflowerblue", 0x6495ED},
    };
    for (auto& nm : names) {
        int l = (int)strlen(nm.n);
        if (str_ieq_prefix(v, nm.n) && !is_ident(v[l])) { out = 0xFF000000 | nm.c; return true; }
    }
    return false;
}

// ================================================================ selectors
static bool parse_ident(const char*& p, const char* e, char* out, int n) {
    int o = 0;
    while (p < e && is_ident(*p)) {
        if (*p == '\\') { p++; if (p >= e) break; }
        if (o < n - 1) out[o++] = *p;
        p++;
    }
    out[o] = 0;
    return o > 0;
}

// returns false when the selector uses something silicon can't match (then the selector is dropped)
static bool parse_selector(CssSheet* s, const char* p, const char* e, int rule) {
    int first = s->comps.len;
    u8 comb = 0;
    u32 spec = 0;
    bool any = false;
    p = skip_ws(p, e);
    while (p < e) {
        Compound c;
        memset(&c, 0, sizeof(c));
        c.comb = comb;
        char name[96];
        bool got = false;
        if (*p == '*') { p++; got = true; }
        else if (is_ident(*p)) { parse_ident(p, e, name, sizeof(name)); c.tag = css_hash_lower(name, (int)strlen(name)); spec += 1; got = true; }
        while (p < e) {
            if (*p == '#') {
                p++;
                if (!parse_ident(p, e, name, sizeof(name))) return false;
                c.id = css_hash(name, (int)strlen(name));
                spec += 10000;
            } else if (*p == '.') {
                p++;
                if (!parse_ident(p, e, name, sizeof(name))) return false;
                if (c.ncls < 4) c.cls[c.ncls++] = css_hash(name, (int)strlen(name));
                spec += 100;
            } else if (*p == '[') {
                p++;
                p = skip_ws(p, e);
                if (!parse_ident(p, e, name, sizeof(name))) return false;
                u32 an = css_hash_lower(name, (int)strlen(name)), av = 0;
                p = skip_ws(p, e);
                if (*p == '=') {
                    p++;
                    p = skip_ws(p, e);
                    char val[96];
                    int o = 0;
                    if (*p == '"' || *p == '\'') {
                        char q = *p++;
                        while (p < e && *p != q) { if (o < 95) val[o++] = *p; p++; }
                        p++;
                    } else while (p < e && *p != ']' && !is_ws(*p)) { if (o < 95) val[o++] = *p; p++; }
                    val[o] = 0;
                    av = css_hash(val, o) | 1;
                    p = skip_ws(p, e);
                    if (*p == 'i' || *p == 's') p++;
                } else if (*p != ']') return false;          // ~= |= ^= $= *=
                p = skip_ws(p, e);
                if (*p != ']') return false;
                p++;
                if (c.nattr < 3) { c.attr[c.nattr] = an; c.attr_val[c.nattr] = av; c.nattr++; }
                spec += 100;
            } else if (*p == ':') {
                p++;
                if (*p == ':') return false;                   // pseudo-elements
                if (!parse_ident(p, e, name, sizeof(name))) return false;
                for (char* q = name; *q; q++) *q = to_lower(*q);
                if (!strcmp(name, "link") || !strcmp(name, "any-link")) c.flags |= 1;
                else if (!strcmp(name, "root")) c.flags |= 2;
                else return false;                             // :hover, :first-child, :not(), ...
                spec += 100;
            } else break;
            got = true;
        }
        if (!got) return false;
        s->comps.push(c);
        any = true;
        // combinator
        const char* q = skip_ws(p, e);
        bool ws = q != p;
        p = q;
        if (p >= e) break;
        if (*p == '>') { comb = 2; p = skip_ws(p + 1, e); }
        else if (*p == '+' || *p == '~') return false;
        else if (ws) comb = 1;
        else return false;
    }
    if (!any) return false;
    Selector sel;
    sel.comp0 = first;
    sel.ncomp = (u8)(s->comps.len - first);
    sel.spec = spec;
    sel.rule = rule;
    sel.next = -1;
    s->sels.push(sel);
    return true;
}

// ================================================================ declarations
static u8 prop_id(const char* n, int len) {
    for (auto& p : PROPS) if ((int)strlen(p.name) == len && !strncmp(p.name, n, len)) return p.prop;
    return P_NONE;
}

static void parse_decls(CssSheet* s, const char* p, const char* e, bool root_vars) {
    while (p < e) {
        p = skip_ws(p, e);
        const char* ns = p;
        while (p < e && *p != ':' && *p != ';') p++;
        if (p >= e || *p == ';') { p++; continue; }
        const char* ne = p;
        while (ne > ns && is_ws(ne[-1])) ne--;
        p++;
        const char* vs = skip_ws(p, e);
        // value runs to the next ';' outside parentheses / quotes
        int depth = 0;
        char q = 0;
        const char* ve = vs;
        while (ve < e) {
            if (q) { if (*ve == q) q = 0; }
            else if (*ve == '"' || *ve == '\'') q = *ve;
            else if (*ve == '(') depth++;
            else if (*ve == ')') depth--;
            else if (*ve == ';' && depth <= 0) break;
            ve++;
        }
        p = ve + 1;
        const char* vend = ve;
        while (vend > vs && is_ws(vend[-1])) vend--;
        bool important = false;
        for (const char* b = vs; b + 9 < vend + 1; b++)
            if (*b == '!' && str_ieq_prefix(b + 1, "important")) { important = true; vend = b; while (vend > vs && is_ws(vend[-1])) vend--; break; }
        int nl = (int)(ne - ns);
        if (nl > 2 && ns[0] == '-' && ns[1] == '-') {
            if (root_vars) {
                s->var_name.push(css_hash(ns, nl));
                s->var_val.push(pool_add(s, vs, (int)(vend - vs)));
            }
            continue;
        }
        char lname[40];
        if (nl >= (int)sizeof(lname)) continue;
        for (int i = 0; i < nl; i++) lname[i] = to_lower(ns[i]);
        u8 pid = prop_id(lname, nl);
        if (!pid) continue;
        Decl d;
        d.prop = pid;
        d.important = important;
        d.val = pool_add(s, vs, (int)(vend - vs));
        s->decls.push(d);
    }
}

// ================================================================ rules, @media
static void parse_rules(CssSheet* s, const char* p, const char* e, int media);

static void parse_rule(CssSheet* s, const char* pre, const char* pre_end, const char* body, const char* body_end, int media) {
    Rule r;
    r.decl0 = s->decls.len;
    r.media = media;
    r.cache_vw = -1;
    r.cache_ok = 0;
    // custom properties count when they're declared on the document root
    // (only a plain :root / html / body / * selector: themed variants like html.night don't count)
    bool root = false;
    {
        const char* a = pre;
        for (const char* q = pre; q <= pre_end; q++) {
            if (q < pre_end && *q != ',') continue;
            const char* b = q;
            while (a < b && is_ws(*a)) a++;
            while (b > a && is_ws(b[-1])) b--;
            int l = (int)(b - a);
            if ((l == 5 && str_ieq_prefix(a, ":root")) || (l == 4 && (str_ieq_prefix(a, "html") || str_ieq_prefix(a, "body"))) ||
                (l == 1 && *a == '*') || (l == 10 && str_ieq_prefix(a, "html:root")))
                root = true;
            a = q + 1;
        }
    }
    parse_decls(s, body, body_end, root);
    r.ndecl = s->decls.len - r.decl0;
    if (!r.ndecl) return;
    int ri = s->rules.len;
    s->rules.push(r);
    // selector list
    const char* a = pre;
    int depth = 0;
    for (const char* q = pre; q <= pre_end; q++) {
        if (q < pre_end && (*q == '(' || *q == '[')) depth++;
        if (q < pre_end && (*q == ')' || *q == ']')) depth--;
        if (q == pre_end || (*q == ',' && depth == 0)) {
            const char* b = q;
            while (b > a && is_ws(b[-1])) b--;
            const char* aa = skip_ws(a, b);
            if (b > aa) parse_selector(s, aa, b, ri);
            a = q + 1;
        }
    }
}

static void parse_rules(CssSheet* s, const char* p, const char* e, int media) {
    while (p < e) {
        p = skip_ws(p, e);
        if (p >= e) break;
        if (*p == '}' || *p == ';') { p++; continue; }
        if (*p == '<') {                                   // stray "<!--" / "-->" in old style blocks
            while (p < e && *p != '>') p++;
            p++;
            continue;
        }
        if (*p == '@') {
            const char* kw = ++p;
            while (p < e && is_ident(*p)) p++;
            int kl = (int)(p - kw);
            const char* pre = p;
            while (p < e && *p != '{' && *p != ';') p++;
            if (p >= e) break;
            if (*p == ';') { p++; continue; }               // @import, @charset, @layer a, b;
            const char* be = block_end(p, e);
            auto kwis = [&](const char* k) { return kl == (int)strlen(k) && str_ieq_prefix(kw, k); };
            if (kwis("media")) {
                int m = pool_add(s, pre, (int)(p - pre));
                parse_rules(s, p + 1, be, m);
            } else if (kwis("supports") || kwis("layer") || kwis("container") || kwis("document") || kwis("scope")) {
                parse_rules(s, p + 1, be, media);
            }
            // @font-face, @keyframes, @page, ... are skipped
            p = be + 1;
            continue;
        }
        const char* pre = p;
        while (p < e && *p != '{' && *p != '}') p++;
        if (p >= e) break;
        if (*p == '}') { p++; continue; }
        const char* pre_end = p;
        while (pre_end > pre && is_ws(pre_end[-1])) pre_end--;
        const char* be = block_end(p, e);
        // nested rules (CSS nesting) aren't supported: only the declarations before them count
        const char* body = p + 1;
        const char* body_end = be;
        for (const char* q = body; q < be; q++) if (*q == '{') { body_end = q; while (body_end > body && *body_end != ';') body_end--; break; }
        parse_rule(s, pre, pre_end, body, body_end, media);
        p = be + 1;
    }
}

void css_add(CssSheet* s, const char* text, u32 len) {
    if (!s || !text || !len) return;
    // strip comments into a private copy
    char* buf = (char*)kmalloc(len + 1);
    u32 o = 0;
    char q = 0;
    for (u32 i = 0; i < len; i++) {
        if (q) { if (text[i] == q && text[i - 1] != '\\') q = 0; buf[o++] = text[i]; continue; }
        if (text[i] == '"' || text[i] == '\'') { q = text[i]; buf[o++] = text[i]; continue; }
        if (text[i] == '/' && i + 1 < len && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(text[i] == '*' && text[i + 1] == '/')) i++;
            i++;
            buf[o++] = ' ';
            continue;
        }
        buf[o++] = text[i];
    }
    buf[o] = 0;
    parse_rules(s, buf, buf + o, -1);
    kfree(buf);
    s->dirty = true;
}

// ================================================================ @media evaluation
static bool media_ok(const char* m, int vw) {
    // a comma list: true when any part is
    char part[256];
    while (*m) {
        int n = 0;
        int depth = 0;
        while (*m && !(*m == ',' && depth == 0)) {
            if (*m == '(') depth++;
            if (*m == ')') depth--;
            if (n < 255) part[n++] = to_lower(*m);
            m++;
        }
        if (*m) m++;
        part[n] = 0;
        bool ok = true;
        const char* p = part;
        while (is_ws(*p)) p++;
        bool neg = false;
        if (str_ieq_prefix(p, "not ")) { neg = true; p += 4; }
        if (str_ieq_prefix(p, "only ")) p += 5;
        while (is_ws(*p)) p++;
        if (str_ieq_prefix(p, "print") || str_ieq_prefix(p, "speech") || str_ieq_prefix(p, "tv") || str_ieq_prefix(p, "handheld")) ok = false;
        for (const char* f = p; *f; f++) {
            if (*f != '(') continue;
            const char* x = f + 1;
            while (is_ws(*x)) x++;
            int v;
            auto after_colon = [&](const char* y) { y = strchr(y, ':'); return y ? y + 1 : nullptr; };
            if (str_ieq_prefix(x, "max-width")) { const char* y = after_colon(x); if (y && parse_len(y, vw, vw, &v) && vw > v) ok = false; }
            else if (str_ieq_prefix(x, "min-width")) { const char* y = after_colon(x); if (y && parse_len(y, vw, vw, &v) && vw < v) ok = false; }
            else if (str_ieq_prefix(x, "max-height")) { const char* y = after_colon(x); if (y && parse_len(y, 600, vw, &v) && 600 > v) ok = false; }
            else if (str_ieq_prefix(x, "min-height")) { const char* y = after_colon(x); if (y && parse_len(y, 600, vw, &v) && 600 < v) ok = false; }
            else if (str_ieq_prefix(x, "width") || str_ieq_prefix(x, "inline-size")) {         // range syntax
                const char* y = x;
                while (*y && *y != '<' && *y != '>' && *y != ')') y++;
                bool lt = *y == '<';
                if (*y == '<' || *y == '>') {
                    bool eq = y[1] == '=';
                    y += eq ? 2 : 1;
                    if (parse_len(y, vw, vw, &v)) {
                        if (lt && !(eq ? vw <= v : vw < v)) ok = false;
                        if (!lt && !(eq ? vw >= v : vw > v)) ok = false;
                    }
                }
            } else if (str_ieq_prefix(x, "prefers-color-scheme")) { if (sstr(x, "dark")) ok = false; }
            else if (str_ieq_prefix(x, "orientation")) { if (sstr(x, "portrait")) ok = false; }
            else if (str_ieq_prefix(x, "hover")) { if (sstr(x, "none")) ok = false; }
            else if (str_ieq_prefix(x, "pointer") || str_ieq_prefix(x, "any-pointer")) { if (sstr(x, "coarse") || sstr(x, "none")) ok = false; }
            else if (sstr(x, "resolution") || sstr(x, "pixel-ratio")) { if (sstr(x, "min")) ok = false; }
            else if (str_ieq_prefix(x, "prefers-reduced-motion") || str_ieq_prefix(x, "prefers-contrast") ||
                     str_ieq_prefix(x, "forced-colors") || str_ieq_prefix(x, "display-mode")) {
                if (!sstr(x, "no-preference") && !sstr(x, "browser") && !sstr(x, "none")) ok = false;
            }
        }
        if (neg) ok = !ok;
        if (ok) return true;
    }
    return false;
}

// ================================================================ index
static u32 key_of(char kind, u32 h) { return h * 4 + (kind == '#' ? 1 : kind == '.' ? 2 : 3); }

static void build_index(CssSheet* s) {
    if (s->hkeys) kfree(s->hkeys);
    if (s->hhead) kfree(s->hhead);
    int cap = 64;
    while (cap < s->sels.len * 2) cap *= 2;
    s->hcap = cap;
    s->hkeys = (u32*)kcalloc(sizeof(u32) * cap);
    s->hhead = (int*)kmalloc(sizeof(int) * cap);
    for (int i = 0; i < cap; i++) s->hhead[i] = -1;
    s->universal = -1;
    // insert in reverse so each bucket's list comes out in source order
    for (int i = s->sels.len - 1; i >= 0; i--) {
        Selector& sel = s->sels[i];
        Compound& c = s->comps[sel.comp0 + sel.ncomp - 1];
        u32 key = c.id ? key_of('#', c.id) : c.ncls ? key_of('.', c.cls[0]) : c.tag ? key_of('t', c.tag) : 0;
        if (!key) { sel.next = s->universal; s->universal = i; continue; }
        u32 slot = key & (cap - 1);
        while (s->hkeys[slot] && s->hkeys[slot] != key) slot = (slot + 1) & (cap - 1);
        s->hkeys[slot] = key;
        sel.next = s->hhead[slot];
        s->hhead[slot] = i;
    }
    s->dirty = false;
}

static int bucket(CssSheet* s, u32 key) {
    u32 slot = key & (s->hcap - 1);
    while (s->hkeys[slot]) {
        if (s->hkeys[slot] == key) return s->hhead[slot];
        slot = (slot + 1) & (s->hcap - 1);
    }
    return -1;
}

// ================================================================ matching
static bool match_comp(const Compound& c, const CssElem& e) {
    if (c.tag && c.tag != e.tag) return false;
    if (c.id && c.id != e.id) return false;
    for (int i = 0; i < c.ncls; i++) {
        bool f = false;
        for (int k = 0; k < e.ncls && !f; k++) f = e.cls[k] == c.cls[i];
        if (!f) return false;
    }
    for (int i = 0; i < c.nattr; i++) {
        bool f = false;
        for (int k = 0; k < e.nattrs && !f; k++) f = e.attrs[k] == c.attr[i] && (!c.attr_val[i] || e.attr_vals[k] == c.attr_val[i]);
        if (!f) return false;
    }
    if ((c.flags & 1) && !e.has_href) return false;
    if ((c.flags & 2) && e.tag != css_hash_lower("html", 4)) return false;
    return true;
}

static bool match_from(CssSheet* s, const Selector& sel, int ci, const CssElem* st, int ei) {
    if (ci == 0) return true;
    const Compound& c = s->comps[sel.comp0 + ci];
    const Compound& left = s->comps[sel.comp0 + ci - 1];
    if (c.comb == 2) return ei > 0 && match_comp(left, st[ei - 1]) && match_from(s, sel, ci - 1, st, ei - 1);
    for (int k = ei - 1; k >= 0; k--)
        if (match_comp(left, st[k]) && match_from(s, sel, ci - 1, st, k)) return true;
    return false;
}

static bool rule_media(CssSheet* s, Rule& r, int vw) {
    if (r.media < 0) return true;
    if (r.cache_vw != vw) { r.cache_vw = vw; r.cache_ok = media_ok(s->pool + r.media, vw); }
    return r.cache_ok;
}

// ================================================================ values
static const char* find_var(CssSheet* s, const char* name, int n) {
    u32 h = css_hash(name, n);
    for (int i = s->var_name.len - 1; i >= 0; i--) if (s->var_name[i] == h) return s->pool + s->var_val[i];
    return nullptr;
}

// substitute var(--x, fallback); depth-limited
static void resolve_vars(CssSheet* s, const char* v, char* out, int n, int depth = 0) {
    int o = 0;
    while (*v && o < n - 1) {
        if (str_ieq_prefix(v, "var(") && depth < 4) {
            const char* p = v + 4;
            while (is_ws(*p)) p++;
            const char* ns = p;
            while (*p && *p != ',' && *p != ')' && !is_ws(*p)) p++;
            int nl = (int)(p - ns);
            while (is_ws(*p)) p++;
            const char* fb = nullptr;
            int fbl = 0;
            int d = 1;
            const char* q = p;
            if (*q == ',') { fb = q + 1; }
            while (*q && d) { if (*q == '(') d++; else if (*q == ')') d--; if (d) q++; }
            if (fb) fbl = (int)(q - fb);
            const char* val = s ? find_var(s, ns, nl) : nullptr;
            char tmp[256];
            if (val) strlcpy(tmp, val, sizeof(tmp));
            else if (fb) { int l = min(fbl, 255); memcpy(tmp, fb, l); tmp[l] = 0; }
            else tmp[0] = 0;
            char sub[256];
            resolve_vars(s, tmp, sub, sizeof(sub), depth + 1);
            for (const char* t = sub; *t && o < n - 1; t++) out[o++] = *t;
            v = *q ? q + 1 : q;
            continue;
        }
        out[o++] = *v++;
    }
    out[o] = 0;
}

static bool has_word(const char* v, const char* w) {
    int l = (int)strlen(w);
    for (const char* p = v; *p; p++)
        if (str_ieq_prefix(p, w) && (p == v || !is_ident(p[-1])) && !is_ident(p[l])) return true;
    return false;
}

// split a value into whitespace-separated tokens (respecting parentheses)
static int tokens(const char* v, char tok[6][64]) {
    int n = 0;
    while (*v && n < 6) {
        while (is_ws(*v)) v++;
        if (!*v) break;
        int o = 0, d = 0;
        while (*v && (d || !is_ws(*v))) {
            if (*v == '(') d++;
            if (*v == ')') d--;
            if (o < 63) tok[n][o++] = *v;
            v++;
        }
        tok[n][o] = 0;
        n++;
    }
    return n;
}

static void box4(const char* v, int vw, int* out) {
    char t[6][64];
    int n = tokens(v, t);
    int x[4];
    for (int i = 0; i < n && i < 4; i++) if (!parse_len(t[i], vw, vw, &x[i])) x[i] = has_word(t[i], "auto") ? CSS_UNSET + 1 : CSS_UNSET;
    if (n == 1) { out[0] = out[1] = out[2] = out[3] = x[0]; }
    else if (n == 2) { out[0] = out[2] = x[0]; out[1] = out[3] = x[1]; }
    else if (n == 3) { out[0] = x[0]; out[1] = out[3] = x[1]; out[2] = x[2]; }
    else if (n >= 4) { out[0] = x[0]; out[1] = x[1]; out[2] = x[2]; out[3] = x[3]; }
}

static void border_side(const char* v, int vw, int& w, u32& c) {
    if (has_word(v, "none") || has_word(v, "hidden")) { w = 0; return; }
    char t[6][64];
    int n = tokens(v, t);
    int bw = 1;
    u32 col = 0;
    bool any = false;
    for (int i = 0; i < n; i++) {
        int x;
        u32 cc;
        if (parse_len(t[i], vw, vw, &x)) { bw = x; any = true; }
        else if (has_word(t[i], "thin")) { bw = 1; any = true; }
        else if (has_word(t[i], "medium")) { bw = 2; any = true; }
        else if (has_word(t[i], "thick")) { bw = 3; any = true; }
        else if (css_parse_color(t[i], cc)) { col = cc; any = true; }
        else if (has_word(t[i], "solid") || has_word(t[i], "dashed") || has_word(t[i], "dotted") || has_word(t[i], "double") ||
                 has_word(t[i], "groove") || has_word(t[i], "ridge") || has_word(t[i], "inset") || has_word(t[i], "outset")) any = true;
    }
    if (!any) return;
    w = bw;
    c = col;
}

static void apply(CssComputed& o, u8 prop, const char* raw, int vw, CssSheet* s) {
    char v[512];
    if (sstr(raw, "var(")) resolve_vars(s, raw, v, sizeof(v));
    else strlcpy(v, raw, sizeof(v));
    if (has_word(v, "inherit") || has_word(v, "unset") || has_word(v, "initial") || has_word(v, "revert")) return;
    int x;
    u32 c;
    switch (prop) {
    case P_DISPLAY:
        if (has_word(v, "none")) o.display = CD_NONE;
        else if (has_word(v, "inline-block") || has_word(v, "inline-flex") || has_word(v, "inline-grid") || has_word(v, "inline-table")) o.display = CD_INLINE_BLOCK;
        else if (has_word(v, "inline")) o.display = CD_INLINE;
        else if (has_word(v, "list-item")) o.display = CD_LIST_ITEM;
        else if (has_word(v, "flex")) o.display = CD_FLEX;
        else if (has_word(v, "block") || has_word(v, "grid") || has_word(v, "flow-root")) o.display = CD_BLOCK;
        else if (has_word(v, "table")) o.display = CD_TABLE;
        else if (has_word(v, "contents")) o.display = CD_CONTENTS;
        break;
    case P_VISIBILITY: o.hidden = has_word(v, "hidden") || has_word(v, "collapse"); break;
    case P_COLOR: if (css_parse_color(v, c) && (c >> 24) > 60) { o.has_color = true; o.color = c | 0xFF000000; } break;
    case P_BG_COLOR: case P_BG: {
        if (has_word(v, "none") && prop == P_BG) { o.has_bg = true; o.bg = 0; break; }
        char t[6][64];
        int n = tokens(v, t);
        for (int i = 0; i < n; i++)
            if (css_parse_color(t[i], c)) { o.has_bg = true; o.bg = (c >> 24) < 80 ? 0 : (c | 0xFF000000); break; }
        break;
    }
    case P_FONT_WEIGHT:
        if (has_word(v, "bold") || has_word(v, "bolder")) o.bold = 1;
        else if (has_word(v, "normal") || has_word(v, "lighter")) o.bold = 0;
        else if (parse_len(v, 0, vw, &x)) o.bold = x >= 600;
        break;
    case P_FONT_STYLE: o.italic = has_word(v, "italic") || has_word(v, "oblique"); break;
    case P_FONT_SIZE: {
        static const struct { const char* k; int px; } kw[] = {{"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16},
                                                              {"large", 18}, {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 48}};
        bool done = false;
        for (auto& k : kw) if (has_word(v, k.k)) { o.font_px = k.px; o.font_pct = 0; done = true; break; }
        if (done) break;
        if (has_word(v, "larger")) { o.font_pct = 120; o.font_px = 0; break; }
        if (has_word(v, "smaller")) { o.font_pct = 83; o.font_px = 0; break; }
        const char* p = v;
        while (is_ws(*p)) p++;
        // em and % are relative to the parent; rem / px are absolute
        const char* u = p;
        while ((*u >= '0' && *u <= '9') || *u == '.') u++;
        if ((starts_ci(u, "em") && !starts_ci(u, "rem")) || *u == '%') {
            bool pct;
            const char* q = p;
            int m = read_num(q, &pct);
            o.font_pct = pct ? m / 1000 : m / 10;
            o.font_px = 0;
        } else if (parse_len(v, 16, vw, &x) && x > 0) { o.font_px = x; o.font_pct = 0; }
        break;
    }
    case P_FONT_FAMILY: o.mono = has_word(v, "monospace") || str_icontains(v, "mono") || str_icontains(v, "courier") || str_icontains(v, "consolas"); break;
    case P_FONT: {
        char t[6][64];
        int n = tokens(v, t);
        for (int i = 0; i < n; i++) {
            if (has_word(t[i], "bold") || has_word(t[i], "bolder") || !strcmp(t[i], "600") || !strcmp(t[i], "700") || !strcmp(t[i], "800") || !strcmp(t[i], "900")) o.bold = 1;
            else if (has_word(t[i], "italic")) o.italic = 1;
            else if ((t[i][0] >= '0' && t[i][0] <= '9') || t[i][0] == '.') {
                char* sl = strchr(t[i], '/');
                if (sl) *sl = 0;
                if (strlen(t[i]) > 0) apply(o, P_FONT_SIZE, t[i], vw, s);
            }
        }
        if (str_icontains(v, "mono") || str_icontains(v, "courier")) o.mono = 1;
        break;
    }
    case P_TEXT_ALIGN:
        if (has_word(v, "center")) o.align = 1;
        else if (has_word(v, "right") || has_word(v, "end")) o.align = 2;
        else if (has_word(v, "left") || has_word(v, "start") || has_word(v, "justify")) o.align = 0;
        break;
    case P_TEXT_DECORATION:
        if (has_word(v, "none")) { o.underline = 0; o.strike = 0; }
        if (has_word(v, "underline")) o.underline = 1;
        if (has_word(v, "line-through")) o.strike = 1;
        break;
    case P_TEXT_TRANSFORM: o.upper = has_word(v, "uppercase"); break;
    case P_WHITE_SPACE: o.pre = has_word(v, "pre") || has_word(v, "pre-wrap") || has_word(v, "break-spaces"); break;
    case P_LIST_STYLE: if (has_word(v, "none")) o.list_none = 1; else if (v[0]) o.list_none = 0; break;
    case P_MARGIN: case P_PADDING: {
        int b[4] = {CSS_UNSET, CSS_UNSET, CSS_UNSET, CSS_UNSET};
        box4(v, vw, b);
        int* dst = prop == P_MARGIN ? o.margin : o.padding;
        for (int i = 0; i < 4; i++) if (b[i] != CSS_UNSET) dst[i] = b[i];
        if (prop == P_MARGIN) o.margin_auto_lr = b[1] == CSS_UNSET + 1 && b[3] == CSS_UNSET + 1;
        break;
    }
    case P_MARGIN_INLINE: case P_PADDING_INLINE: case P_MARGIN_BLOCK: case P_PADDING_BLOCK: {
        char t[6][64];
        int n = tokens(v, t);
        int a, b2;
        bool auto_a = n >= 1 && has_word(t[0], "auto");
        if (n < 1) break;
        if (!parse_len(t[0], vw, vw, &a)) a = auto_a ? CSS_UNSET + 1 : CSS_UNSET;
        if (n < 2 || !parse_len(t[1], vw, vw, &b2)) b2 = n >= 2 && has_word(t[1], "auto") ? CSS_UNSET + 1 : a;
        int* dst = (prop == P_MARGIN_INLINE || prop == P_MARGIN_BLOCK) ? o.margin : o.padding;
        int i0 = (prop == P_MARGIN_INLINE || prop == P_PADDING_INLINE) ? 3 : 0, i1 = i0 == 3 ? 1 : 2;
        if (a != CSS_UNSET) dst[i0] = a;
        if (b2 != CSS_UNSET) dst[i1] = b2;
        if (prop == P_MARGIN_INLINE) o.margin_auto_lr = a == CSS_UNSET + 1 && b2 == CSS_UNSET + 1;
        break;
    }
    case P_MARGIN_T: case P_MARGIN_R: case P_MARGIN_B: case P_MARGIN_L: {
        int i = prop - P_MARGIN_T;
        if (parse_len(v, vw, vw, &x)) o.margin[i] = x;
        else if (has_word(v, "auto")) o.margin[i] = CSS_UNSET + 1;
        o.margin_auto_lr = o.margin[1] == CSS_UNSET + 1 && o.margin[3] == CSS_UNSET + 1;
        break;
    }
    case P_PADDING_T: case P_PADDING_R: case P_PADDING_B: case P_PADDING_L:
        if (parse_len(v, vw, vw, &x)) o.padding[prop - P_PADDING_T] = x;
        break;
    case P_WIDTH: if (parse_len(v, vw, vw, &x)) o.width = max(x, 0); else if (has_word(v, "auto")) o.width = -1; break;
    case P_MAX_WIDTH: if (parse_len(v, vw, vw, &x)) o.max_width = max(x, 0); else if (has_word(v, "none")) o.max_width = -1; break;
    case P_HEIGHT: if (parse_len(v, 600, vw, &x)) o.height = max(x, 0); else if (has_word(v, "auto")) o.height = -1; break;
    case P_POSITION:
        o.position = has_word(v, "absolute") ? 1 : has_word(v, "fixed") ? 2 : has_word(v, "relative") ? 3 : has_word(v, "sticky") ? 4 : 0;
        break;
    case P_LEFT: if (parse_len(v, vw, vw, &x)) o.left = x; break;
    case P_TOP: if (parse_len(v, 600, vw, &x)) o.top = x; break;
    case P_BORDER: for (int i = 0; i < 4; i++) border_side(v, vw, o.border_w[i], o.border_c[i]); break;
    case P_BORDER_T: case P_BORDER_R: case P_BORDER_B: case P_BORDER_L: {
        int i = prop - P_BORDER_T;
        border_side(v, vw, o.border_w[i], o.border_c[i]);
        break;
    }
    case P_BORDER_WIDTH: {
        int b[4] = {CSS_UNSET, CSS_UNSET, CSS_UNSET, CSS_UNSET};
        box4(v, vw, b);
        for (int i = 0; i < 4; i++) if (b[i] != CSS_UNSET && b[i] > CSS_UNSET + 1) o.border_w[i] = b[i];
        break;
    }
    case P_BORDER_STYLE: if (has_word(v, "none") || has_word(v, "hidden")) for (int i = 0; i < 4; i++) o.border_w[i] = 0; break;
    case P_BORDER_COLOR: if (css_parse_color(v, c)) for (int i = 0; i < 4; i++) o.border_c[i] = c; break;
    case P_FLEX_DIRECTION: o.flex_column = has_word(v, "column") || has_word(v, "column-reverse"); break;
    case P_FLOAT: o.float_side = has_word(v, "left") ? 1 : has_word(v, "right") ? 2 : 0; break;
    case P_CLIP:
        if (str_icontains(v, "rect(")) {
            // rect(0 0 0 0) / rect(1px, 1px, 1px, 1px): the classic "visually hidden"
            const char* p = strchr(v, '(') + 1;
            int a[4] = {0, 0, 0, 0}, k = 0;
            while (*p && k < 4) {
                while (*p == ' ' || *p == ',') p++;
                if (!parse_len(p, vw, vw, &a[k])) a[k] = 0;
                k++;
                while (*p && *p != ' ' && *p != ',' && *p != ')') p++;
                if (*p == ')') break;
            }
            if (iabs(a[2] - a[0]) <= 1 || iabs(a[1] - a[3]) <= 1) o.hidden = 1;
        }
        break;
    case P_CLIP_PATH: if (str_icontains(v, "inset(50%") || str_icontains(v, "inset(100%")) o.hidden = 1; break;
    case P_OVERFLOW: break;
    }
}

static void init_computed(CssComputed& o) {
    memset(&o, 0, sizeof(o));
    o.display = CD_UNSET;
    o.bold = o.italic = o.mono = o.underline = o.strike = o.upper = o.pre = o.list_none = -1;
    o.align = -1;
    for (int i = 0; i < 4; i++) { o.margin[i] = o.padding[i] = CSS_UNSET; }
    o.width = o.max_width = o.height = -1;
    o.left = o.top = CSS_UNSET;
    o.flex_column = 0;
    o.hidden = 0;
}

static void apply_inline(CssComputed& o, const char* st, int vw, CssSheet* s, bool important_only) {
    // parse "a: b; c: d" without a sheet
    const char* p = st;
    while (*p) {
        while (is_ws(*p) || *p == ';') p++;
        const char* ns = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') continue;
        const char* ne = p;
        while (ne > ns && is_ws(ne[-1])) ne--;
        p++;
        const char* vs = p;
        int depth = 0;
        while (*p && !(*p == ';' && depth == 0)) { if (*p == '(') depth++; if (*p == ')') depth--; p++; }
        char val[512];
        int vl = min((int)(p - vs), 511);
        memcpy(val, vs, vl);
        val[vl] = 0;
        bool imp = false;
        char* bang = strchr(val, '!');
        if (bang && str_ieq_prefix(bang + 1, "important")) { *bang = 0; imp = true; }
        if (important_only && !imp) continue;
        char lname[40];
        int nl = (int)(ne - ns);
        if (nl <= 0 || nl >= 40) continue;
        for (int i = 0; i < nl; i++) lname[i] = to_lower(ns[i]);
        u8 pid = prop_id(lname, nl);
        if (pid) apply(o, pid, val, vw, s);
    }
}

void css_compute(CssSheet* s, const CssElem* st, int depth, int vw, const char* inline_style, CssComputed& out) {
    init_computed(out);
    const CssElem& e = st[depth - 1];
    if (s && s->rules.len) {
        if (s->dirty) build_index(s);
        // candidates: id bucket, class buckets, tag bucket, universal list
        struct Hit { u32 spec; int rule; };
        Hit hits[96];
        int nh = 0;
        auto consider = [&](int si) {
            for (; si >= 0; si = s->sels[si].next) {
                Selector& sel = s->sels[si];
                if (!match_comp(s->comps[sel.comp0 + sel.ncomp - 1], e)) continue;
                if (!match_from(s, sel, sel.ncomp - 1, st, depth - 1)) continue;
                if (!rule_media(s, s->rules[sel.rule], vw)) continue;
                bool dup = false;
                for (int k = 0; k < nh; k++) if (hits[k].rule == sel.rule) { if (sel.spec > hits[k].spec) hits[k].spec = sel.spec; dup = true; }
                if (!dup && nh < 96) hits[nh++] = Hit{sel.spec, sel.rule};
            }
        };
        if (e.id) consider(bucket(s, key_of('#', e.id)));
        for (int i = 0; i < e.ncls; i++) consider(bucket(s, key_of('.', e.cls[i])));
        consider(bucket(s, key_of('t', e.tag)));
        consider(s->universal);
        // cascade order: specificity, then source order
        for (int i = 1; i < nh; i++) {
            Hit h = hits[i];
            int j = i - 1;
            while (j >= 0 && (hits[j].spec > h.spec || (hits[j].spec == h.spec && hits[j].rule > h.rule))) { hits[j + 1] = hits[j]; j--; }
            hits[j + 1] = h;
        }
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1 && inline_style) apply_inline(out, inline_style, vw, s, false);
            for (int i = 0; i < nh; i++) {
                Rule& r = s->rules[hits[i].rule];
                for (int k = 0; k < r.ndecl; k++) {
                    Decl& d = s->decls[r.decl0 + k];
                    if ((bool)d.important != (pass == 1)) continue;
                    apply(out, d.prop, s->pool + d.val, vw, s);
                }
            }
        }
        if (inline_style) apply_inline(out, inline_style, vw, s, true);
    } else if (inline_style) apply_inline(out, inline_style, vw, s, false);
    // the usual ways of hiding things that are only for screen readers or scripts
    if ((out.position == 1 || out.position == 2) && ((out.left != CSS_UNSET && out.left <= -500) || (out.top != CSS_UNSET && out.top <= -500)))
        out.hidden = 1;
    if (out.width >= 0 && out.width <= 1 && out.height >= 0 && out.height <= 1) out.hidden = 1;
}
