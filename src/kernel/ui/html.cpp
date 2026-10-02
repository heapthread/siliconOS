// silicon - HTML tokenizer + flow layout. Streams tags and text through a style stack and
// places words into lines; produces a flat list of positioned items the browser paints.
#include "html.h"
#include "css.h"
#include "../net/net.h"

static const int LINE_H = 16;          // per text scale
static const int MARGIN = 14;

void HtmlDoc::clear() {
    items.clear();
    for (int i = 0; i < links.len; i++) kfree(links[i]);
    links.clear();
    fields.clear();
    forms.clear();
    title[0] = 0;
    height = 0;
    pool_len = 0;
    images = 0;
    for (int i = 0; i < img_urls.len; i++) kfree(img_urls[i]);
    img_urls.clear();
    for (int i = 0; i < css_urls.len; i++) kfree(css_urls[i]);
    css_urls.clear();
    img_sized.clear();
}

// ================================================================ text decoding
struct Entity { const char* name; const char* text; };
static const Entity entities[] = {
    {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "}, {"copy", "(c)"},
    {"reg", "(r)"}, {"trade", "(tm)"}, {"mdash", "-"}, {"ndash", "-"}, {"hellip", "..."}, {"laquo", "<<"},
    {"raquo", ">>"}, {"middot", "\x93"}, {"bull", "\x93"}, {"lsquo", "'"}, {"rsquo", "'"}, {"ldquo", "\""},
    {"rdquo", "\""}, {"times", "x"}, {"divide", "/"}, {"deg", "\x96"}, {"larr", "\x8f"}, {"rarr", "\x90"},
    {"uarr", "\x91"}, {"darr", "\x92"}, {"hearts", "\x9d"}, {"euro", "EUR"}, {"pound", "GBP"}, {"cent", "c"},
    {"sect", "S"}, {"para", "P"}, {"shy", ""}, {"zwj", ""}, {"zwnj", ""}, {"thinsp", " "}, {"ensp", " "}, {"emsp", " "},
};

// map a unicode code point onto silicon's ASCII-ish font
static void put_codepoint(u32 cp, char* out, int& o, int n) {
    auto put = [&](const char* s) { while (*s && o < n - 1) out[o++] = *s++; };
    if (cp < 128) { if (o < n - 1) out[o++] = (char)cp; return; }
    if (cp == 0xA0 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F) { put(" "); return; }
    if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x2032) { put("'"); return; }
    if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x2033) { put("\""); return; }
    if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212 || cp == 0x2010 || cp == 0x2011) { put("-"); return; }
    if (cp == 0x2026) { put("..."); return; }
    if (cp == 0x2022 || cp == 0xB7 || cp == 0x25CF) { put("\x93"); return; }
    if (cp == 0x2190) { put("\x8f"); return; }
    if (cp == 0x2192) { put("\x90"); return; }
    if (cp == 0x2191) { put("\x91"); return; }
    if (cp == 0x2193) { put("\x92"); return; }
    if (cp == 0xB0) { put("\x96"); return; }
    if (cp == 0xD7) { put("x"); return; }
    if (cp == 0xA9) { put("(c)"); return; }
    if (cp == 0xAE) { put("(r)"); return; }
    if (cp == 0x2122) { put("(tm)"); return; }
    if (cp == 0x2713 || cp == 0x2714) { put("\x94"); return; }
    if (cp == 0x2665 || cp == 0x2764) { put("\x9d"); return; }
    if (cp == 0x200B || cp == 0xFEFF || cp == 0xAD) return;
    // latin-1 letters with marks -> base letter
    static const char* lat = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (cp >= 0xC0 && cp <= 0xFF) { char c[2] = {lat[cp - 0xC0], 0}; put(c); return; }
    if (cp >= 0x100 && cp <= 0x17F) { static const char* ext = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
        char c[2] = {ext[cp - 0x100], 0}; put(c); return; }
    put("?");
}

// decode entities + UTF-8 into the pool; collapse whitespace unless pre
static int decode(const char* s, int len, char* out, int n, bool pre) {
    int o = 0;
    for (int i = 0; i < len && o < n - 4;) {
        u8 c = (u8)s[i];
        if (c == '&') {
            int j = i + 1;
            while (j < len && j - i < 12 && s[j] != ';' && s[j] != '&' && s[j] != ' ' && s[j] != '<') j++;
            if (j < len && s[j] == ';') {
                if (s[i + 1] == '#') {
                    u32 cp = 0;
                    if (s[i + 2] == 'x' || s[i + 2] == 'X') {
                        for (int k = i + 3; k < j; k++) {
                            char h = to_lower(s[k]);
                            cp = cp * 16 + (h >= 'a' ? h - 'a' + 10 : h - '0');
                        }
                    } else for (int k = i + 2; k < j; k++) cp = cp * 10 + (s[k] - '0');
                    put_codepoint(cp, out, o, n);
                    i = j + 1;
                    continue;
                }
                bool found = false;
                for (auto& e : entities)
                    if ((int)strlen(e.name) == j - i - 1 && !strncmp(e.name, s + i + 1, j - i - 1)) {
                        for (const char* t = e.text; *t; t++) out[o++] = *t;
                        found = true;
                        break;
                    }
                if (found) { i = j + 1; continue; }
            }
            out[o++] = '&';
            i++;
            continue;
        }
        if (c >= 0x80) {
            u32 cp;
            int extra;
            if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
            else { put_codepoint(c, out, o, n); i++; continue; }     // latin-1 byte
            if (i + extra >= len) { i++; continue; }          // truncated sequence
            for (int k = 1; k <= extra && i + k < len; k++) cp = (cp << 6) | ((u8)s[i + k] & 0x3F);
            i += extra + 1;
            if (cp >= 0x1F000) continue;                              // emoji: no glyphs
            put_codepoint(cp, out, o, n);
            continue;
        }
        if (!pre && (c == '\n' || c == '\r' || c == '\t')) c = ' ';
        if (c == '\r') { i++; continue; }
        out[o++] = (char)c;
        i++;
    }
    out[o] = 0;
    return o;
}

// ================================================================ colours
static bool parse_color(const char* v, u32& out) {
    while (*v == ' ') v++;
    struct { const char* n; u32 c; } names[] = {
        {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xCC0000}, {"green", 0x008000}, {"blue", 0x0000CC},
        {"navy", 0x000080}, {"maroon", 0x800000}, {"purple", 0x800080}, {"teal", 0x008080}, {"gray", 0x808080},
        {"grey", 0x808080}, {"silver", 0xC0C0C0}, {"yellow", 0xCCCC00}, {"orange", 0xDD7700}, {"olive", 0x808000},
        {"lime", 0x00AA00}, {"aqua", 0x008B8B}, {"fuchsia", 0xCC00CC}, {"brown", 0x8B4513}, {"darkred", 0x8B0000},
        {"darkblue", 0x00008B}, {"darkgreen", 0x006400},
    };
    if (*v == '#') {
        v++;
        u32 x = 0;
        int n = 0;
        while (n < 6) {
            char h = to_lower(*v);
            if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f'))) break;
            x = x * 16 + (h <= '9' ? h - '0' : h - 'a' + 10);
            v++; n++;
        }
        if (n == 3) x = ((x & 0xF00) * 0x1100) | ((x & 0xF0) * 0x110) | ((x & 0xF) * 0x11);
        else if (n != 6) return false;
        out = 0xFF000000 | x;
        return true;
    }
    for (auto& nm : names) if (str_ieq_prefix(v, nm.n)) { out = 0xFF000000 | nm.c; return true; }
    return false;
}

// reject colours that would be unreadable on silicon's paper background
static u32 readable(u32 c, u32 bg) {
    auto lum = [](u32 x) { return (int)(((x >> 16) & 255) * 3 + ((x >> 8) & 255) * 6 + (x & 255)) / 10; };
    return iabs(lum(c) - lum(bg)) < 70 ? 0xFF221C30 : c;
}

// ================================================================ layout engine
struct Style {
    char tag[12];
    u32  color;
    u8   scale;
    bool bold, underline, strike, mono;
    int  link;
    bool pre;
    int  align;               // 0 left, 1 center, 2 right
    int  indent, rindent;     // left / right insets of the content box
    u32  bgc;                 // background behind this text (for contrast checks)
    u32  ibg;                 // inline background (css on inline elements)
    bool upper, nobullet;
    int  fpx;                 // font size in css px
};

struct ElemState {
    char tag[12];
    bool generic, forced_inline, promote, block, flex_row;
    int  bg_item, bg_y, bar_item, bar_y, pad_b, margin_b, border_b;
    int  y0, items0, content0;      // to collapse boxes that end up empty
    u32  border_b_c;
};

static bool is_void_tag(const char* n) {
    static const char* v[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param",
                              "source", "track", "wbr", "keygen", "frame"};
    for (auto t : v) if (!strcmp(n, t)) return true;
    return false;
}
static bool is_block_tag(const char* n) {
    static const char* b[] = {"p", "div", "section", "article", "header", "footer", "nav", "main", "aside", "figure",
                              "figcaption", "address", "dl", "dt", "dd", "details", "summary", "fieldset", "center",
                              "form", "table", "caption", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "menu", "li",
                              "blockquote", "pre", "hr", "tr", "body", "html", "listing", "xmp", "hgroup", "search"};
    for (auto t : b) if (!strcmp(n, t)) return true;
    return false;
}

struct ListState { bool ordered; int n; };

struct Layout {
    HtmlDoc& d;
    const char* base;
    int width;
    Style st[48];
    int sp = 0;
    ListState lists[12];
    int nlists = 0;
    int x, y, line_start, line_h;
    bool space_pending = false;
    bool in_title = false;
    int  cur_form = -1;
    int  pre_bg_start = -1, pre_bg_y = 0;
    int  quote_items[8];
    int  quote_y[8];
    int  nquote = 0;
    bool in_select = false, select_first = false;
    int  select_field = -1;
    int  in_textarea = -1;
    int  col_in_row = 0;
    int  in_cell = 0;
    const HtmlDoc* keep;
    int  field_count = 0;
    // css
    CssSheet* sheet = nullptr;
    CssElem estack[64];
    ElemState es[64];
    int edepth = 0;
    const CssComputed* cur = nullptr;    // style of the element being opened (for tag())
    const char* src = nullptr;
    u32 srclen = 0, pos = 0;

    Layout(HtmlDoc& doc, const char* b, int w, const HtmlDoc* k) : d(doc), base(b), width(w), keep(k) {
        memset(&st[0], 0, sizeof(Style));
        st[0].color = d.fg;
        st[0].scale = 1;
        st[0].link = -1;
        st[0].bgc = d.bg;
        st[0].fpx = 16;
        x = MARGIN; y = MARGIN; line_start = 0; line_h = LINE_H;
    }
    Style& top() { return st[sp]; }
    int left() { return MARGIN + top().indent; }
    int right() { return max(width - MARGIN - top().rindent, left() + 120); }

    int pool_add(const char* s, int n) {
        if (d.pool_len + n + 1 > d.pool_cap) {
            int nc = max(d.pool_cap * 2, d.pool_len + n + 4096);
            d.pool = (char*)krealloc(d.pool, nc);
            d.pool_cap = nc;
        }
        int off = d.pool_len;
        memcpy(d.pool + off, s, n);
        d.pool[off + n] = 0;
        d.pool_len += n + 1;
        return off;
    }

    void finish_line() {
        if (d.items.len > line_start) {
            int lh = 0, lw = 0, minx = 1 << 30;
            for (int i = line_start; i < d.items.len; i++) {
                HtmlItem& it = d.items[i];
                if (it.kind == HK_RECT && it.h > 4) continue;
                lh = max(lh, it.h);
                lw = max(lw, it.x + it.w);
                minx = min(minx, it.x);
            }
            lh = max(lh, LINE_H * (int)top().scale);
            int shift = 0;
            if (top().align == 1 && lw < right()) shift = (right() - lw - (minx - left())) / 2;
            if (top().align == 2 && lw < right()) shift = right() - lw;
            for (int i = line_start; i < d.items.len; i++) {
                HtmlItem& it = d.items[i];
                if (it.kind == HK_RECT && it.h > 4) continue;
                it.y = y + (lh - it.h) / 2 + (it.kind == HK_TEXT ? 1 : 0);
                it.x += shift;
            }
            y += lh;
        }
        line_start = d.items.len;
        x = left();
        space_pending = false;
    }
    bool marker_pending = false;    // a list marker waits for its first content on the same line
    void newline() {
        if (marker_pending) { x = max(x, left()); return; }
        if (x > left() || d.items.len > line_start) finish_line(); else x = left();
    }
    int last_gap_y = -1;
    void blank_line(int px) {
        if (marker_pending) return;
        newline();
        // collapse consecutive vertical gaps
        if (last_gap_y == y) return;
        y += px;
        last_gap_y = y;
    }

    HtmlItem& add(u8 kind, int w, int h) {
        marker_pending = false;
        HtmlItem it;
        memset(&it, 0, sizeof(it));
        it.kind = kind;
        it.x = x;
        it.y = y;
        it.w = w;
        it.h = h;
        it.color = top().color;
        it.scale = top().scale;
        it.bold = top().bold;
        it.underline = top().underline;
        it.strike = top().strike;
        it.link = top().link;
        it.field = -1;
        it.img = -1;
        it.bg = top().ibg ? top().ibg : top().mono ? 0xFFE6E1F0 : 0;
        d.items.push(it);
        return d.items.back();
    }

    void word(const char* w, int n) {
        int cw = 8 * top().scale;
        int ww = n * cw;
        if (space_pending && x > left()) {
            if (x + cw + ww > right()) finish_line();
            else x += cw;
        }
        space_pending = false;
        // a very long word wraps hard
        while (ww > right() - left() && n > 0) {
            int fit = max(1, (right() - x) / cw);
            if (x > left() && fit < 4) { finish_line(); fit = max(1, (right() - x) / cw); }
            HtmlItem& it = add(HK_TEXT, fit * cw, LINE_H * top().scale - 4 * top().scale + 2);
            it.text = pool_add(w, min(fit, n));
            it.tlen = min(fit, n);
            w += fit; n -= fit;
            finish_line();
            ww = n * cw;
        }
        if (n <= 0) return;
        if (x + ww > right() && x > left()) finish_line();
        HtmlItem& it = add(HK_TEXT, ww, 12 * top().scale);
        it.text = pool_add(w, n);
        it.tlen = n;
        x += ww;
    }

    void text(const char* raw, int len) {
        if (in_title) {
            char t[256];
            decode(raw, min(len, 250), t, sizeof(t), false);
            strlcat(d.title, t, sizeof(d.title));
            return;
        }
        if (in_textarea >= 0) {
            char t[256];
            decode(raw, min(len, 250), t, sizeof(t), true);
            if (!keep) strlcat(d.fields[in_textarea].value, t, sizeof(d.fields[in_textarea].value));
            return;
        }
        if (in_select) {
            if (select_first && select_field >= 0) {
                char t[128];
                decode(raw, min(len, 120), t, sizeof(t), false);
                char* s = t;
                while (*s == ' ') s++;
                if (*s) {
                    HtmlItem& it = add(HK_FIELD, min(200, (int)strlen(s) * 8 + 24), 20);
                    it.field = select_field;
                    it.text = pool_add(s, (int)strlen(s));
                    it.tlen = (int)strlen(s);
                    x += it.w + 4;
                    select_first = false;
                }
            }
            return;
        }
        char* buf = (char*)kmalloc(len * 3 + 16);
        int n = decode(raw, len, buf, len * 3 + 16, top().pre);
        if (top().upper) for (int i = 0; i < n; i++) buf[i] = to_upper(buf[i]);
        if (top().pre) {
            int s = 0;
            for (int i = 0; i <= n; i++) {
                if (i == n || buf[i] == '\n') {
                    if (i > s) {
                        HtmlItem& it = add(HK_TEXT, (i - s) * 8 * top().scale, 12 * top().scale);
                        it.text = pool_add(buf + s, i - s);
                        it.tlen = i - s;
                        x += it.w;
                    }
                    if (i < n) finish_line();
                    s = i + 1;
                }
            }
        } else {
            int i = 0;
            if (n && buf[0] == ' ') space_pending = true;
            while (i < n) {
                while (i < n && buf[i] == ' ') { space_pending = true; i++; }
                int s = i;
                while (i < n && buf[i] != ' ') i++;
                if (i > s) word(buf + s, i - s);
                if (i < n) space_pending = true;
            }
        }
        kfree(buf);
    }

    void push(const char* tag) {
        if (sp >= 47) return;
        st[sp + 1] = st[sp];
        sp++;
        strlcpy(st[sp].tag, tag, sizeof(st[sp].tag));
    }
    void pop(const char* tag) {
        for (int i = sp; i > 0; i--)
            if (!strcmp(st[i].tag, tag)) { sp = i - 1; return; }
    }

    static const char* attr(const char* attrs, const char* name, char* out, int n) {
        int nl = (int)strlen(name);
        const char* p = attrs;
        while (*p) {
            while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r' || *p == '/') p++;
            const char* an = p;
            while (*p && *p != '=' && *p != ' ' && *p != '>' && *p != '\n' && *p != '\t') p++;
            int al = (int)(p - an);
            const char* v = nullptr;
            int vl = 0;
            while (*p == ' ') p++;
            if (*p == '=') {
                p++;
                while (*p == ' ') p++;
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    v = p;
                    while (*p && *p != q) p++;
                    vl = (int)(p - v);
                    if (*p) p++;
                } else {
                    v = p;
                    while (*p && *p != ' ' && *p != '>' && *p != '\n' && *p != '\t') p++;
                    vl = (int)(p - v);
                }
            }
            if (al == nl && str_ieq_prefix(an, name)) {
                if (!v) { out[0] = 0; return out; }
                decode(v, min(vl, n - 1), out, n, true);
                return out;
            }
            if (!al) p++;
        }
        return nullptr;
    }

    void heading(int level) {
        blank_line(level <= 2 ? 10 : 6);
        push(level == 1 ? "h1" : level == 2 ? "h2" : level == 3 ? "h3" : level == 4 ? "h4" : level == 5 ? "h5" : "h6");
        top().bold = true;
        top().fpx = level == 1 ? 32 : level == 2 ? 24 : level == 3 ? 19 : 16;
        if (level <= 2) top().scale = 2;
        if (level == 3) top().color = readable(0xFF4A3470, d.bg);
    }

    void tag(const char* name, bool end, const char* attrs) {
        char a[512];
        // block structure first
        auto is = [&](const char* t) { return !strcmp(name, t); };
        if (is("title")) { in_title = !end; return; }
        if (is("br")) { if (x == left() && d.items.len == line_start) y += LINE_H * top().scale; else finish_line(); return; }
        if (is("p") || is("div") || is("section") || is("article") || is("header") || is("footer") || is("nav") ||
            is("main") || is("aside") || is("figure") || is("figcaption") || is("address") || is("dl") || is("details") ||
            is("summary") || is("fieldset") || is("center") || is("form") || is("table") || is("dd") || is("dt") || is("caption")) {
            // inside table cells, layout divs/centers flow inline (old sites build rows out of them)
            bool soft = in_cell > 0 && (is("div") || is("center") || is("span") || is("form") || is("section") || is("nav"));
            if (soft) {
                if (is("form")) {
                    if (!end) {
                        HtmlForm f;
                        memset(&f, 0, sizeof(f));
                        char act[512], tmp[512], m[16];
                        strlcpy(f.action, attr(attrs, "action", act, sizeof(act)) ? act : base, sizeof(f.action));
                        url_resolve(base, f.action, tmp, sizeof(tmp));
                        strlcpy(f.action, tmp, sizeof(f.action));
                        f.post = attr(attrs, "method", m, sizeof(m)) && str_ieq_prefix(m, "post");
                        d.forms.push(f);
                        cur_form = d.forms.len - 1;
                    }
                    return;
                }
                if (!end) push(name); else pop(name);
                return;
            }
            if (is("table")) { if (!end) in_cell = 0; }
            if (is("p") || is("dl") || is("table")) blank_line(8); else newline();
            if (is("form")) {
                if (!end) {
                    HtmlForm f;
                    memset(&f, 0, sizeof(f));
                    char act[512];
                    strlcpy(f.action, attr(attrs, "action", act, sizeof(act)) ? act : base, sizeof(f.action));
                    char tmp[512];
                    url_resolve(base, f.action, tmp, sizeof(tmp));
                    strlcpy(f.action, tmp, sizeof(f.action));
                    char m[16];
                    f.post = attr(attrs, "method", m, sizeof(m)) && str_ieq_prefix(m, "post");
                    d.forms.push(f);
                    cur_form = d.forms.len - 1;
                } else cur_form = -1;
                return;
            }
            if (!end) {
                push(name);
                if (is("center") || (attr(attrs, "align", a, sizeof(a)) && str_ieq_prefix(a, "center"))) top().align = 1;
                if (is("dd")) top().indent += 24;
                if (is("dt")) top().bold = true;
                if (is("caption")) top().align = 1;
                char sty[256];
                if (attr(attrs, "style", sty, sizeof(sty)) && str_icontains(sty, "text-align:center")) top().align = 1;
            } else pop(name);
            return;
        }
        if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
            if (!end) heading(name[1] - '0');
            else { pop(name); blank_line(6); }
            return;
        }
        if (is("hr")) {
            blank_line(4);
            HtmlItem& it = add(HK_RECT, right() - left(), 2);
            it.color = 0xFFB0A6C8;
            y += 8;
            line_start = d.items.len;
            return;
        }
        if (is("ul") || is("ol") || is("menu")) {
            if (!end) {
                newline();
                if (nlists < 12) { lists[nlists].ordered = is("ol"); lists[nlists].n = 0; nlists++; }
                push(name);
                top().indent += 26;
            } else {
                pop(name);
                if (nlists) nlists--;
                newline();
                if (!nlists) blank_line(4);
            }
            return;
        }
        if (is("li")) {
            marker_pending = false;
            if (end) { newline(); return; }
            newline();
            x = left();
            if (nlists && lists[nlists - 1].ordered) {
                char num[8];
                snprintf(num, sizeof(num), "%d.", ++lists[nlists - 1].n);
                x = left() - 8 * (int)strlen(num) - 6;
                HtmlItem& it = add(HK_TEXT, 8 * (int)strlen(num), 12);
                it.text = pool_add(num, (int)strlen(num));
                it.tlen = (int)strlen(num);
                it.link = -1;
            } else if (!top().nobullet && !(cur && cur->list_none == 1)) {
                x = left() - 14;
                HtmlItem& it = add(HK_BULLET, 8, 12);
                it.color = 0xFF8A7EA8;
                it.link = -1;
            }
            x = left();
            marker_pending = true;
            return;
        }
        if (is("blockquote")) {
            if (!end) {
                blank_line(6);
                if (nquote < 8) { quote_y[nquote] = y; HtmlItem& it = add(HK_RECT, 3, 0); it.color = 0xFFC9C0DE; quote_items[nquote++] = d.items.len - 1; line_start = d.items.len; }
                push(name);
                top().indent += 20;
                top().color = readable(0xFF5A5070, d.bg);
                x = left();
            } else {
                pop(name);
                newline();
                if (nquote) { nquote--; HtmlItem& it = d.items[quote_items[nquote]]; it.x = left() + 4; it.y = quote_y[nquote]; it.h = max(4, y - quote_y[nquote]); }
                blank_line(6);
            }
            return;
        }
        if (is("pre") || is("listing") || is("xmp")) {
            if (!end) {
                blank_line(6);
                push(name);
                top().pre = true;
                top().indent += 8;
                pre_bg_start = d.items.len;
                pre_bg_y = y;
                HtmlItem& it = add(HK_RECT, right() - left() + 16, 0);
                it.x = left() - 12;
                it.color = 0xFFEFEBF6;
                line_start = d.items.len;
                x = left();
            } else {
                newline();
                if (pre_bg_start >= 0) { HtmlItem& it = d.items[pre_bg_start]; it.y = pre_bg_y - 4; it.h = y - pre_bg_y + 8; pre_bg_start = -1; }
                pop(name);
                blank_line(8);
            }
            return;
        }
        if (is("tr")) { newline(); col_in_row = 0; if (!end) y += 2; return; }
        if (is("td") || is("th")) {
            if (!end) {
                if (col_in_row++ > 0) { x += 16; space_pending = false; }
                push(name);
                in_cell++;
                if (is("th")) top().bold = true;
                top().align = attr(attrs, "align", a, sizeof(a)) && str_ieq_prefix(a, "center") ? 1 : 0;
            } else { pop(name); if (in_cell) in_cell--; }
            return;
        }
        // inline formatting
        if (is("b") || is("strong")) { if (!end) { push(name); top().bold = true; } else pop(name); return; }
        if (is("i") || is("em") || is("cite") || is("var") || is("dfn")) {
            if (!end) { push(name); top().color = readable(0xFF6A4C9C, d.bg); } else pop(name);
            return;
        }
        if (is("u") || is("ins")) { if (!end) { push(name); top().underline = true; } else pop(name); return; }
        if (is("s") || is("strike") || is("del")) { if (!end) { push(name); top().strike = true; } else pop(name); return; }
        if (is("code") || is("tt") || is("kbd") || is("samp")) { if (!end) { push(name); top().mono = true; } else pop(name); return; }
        if (is("mark")) { if (!end) { push(name); top().mono = true; } else pop(name); return; }
        if (is("big")) { if (!end) { push(name); top().bold = true; } else pop(name); return; }
        if (is("font") || is("span")) {
            if (!end) {
                push(name);
                u32 c;
                if (attr(attrs, "color", a, sizeof(a)) && parse_color(a, c)) top().color = readable(c, d.bg);
                char sty[256];
                if (attr(attrs, "style", sty, sizeof(sty))) {
                    const char* cs = strstr_i(sty, "color:");
                    if (cs && (cs == sty || cs[-1] == ';' || cs[-1] == ' ') && parse_color(cs + 6, c)) top().color = readable(c, d.bg);
                    if (str_icontains(sty, "font-weight:bold") || str_icontains(sty, "font-weight: bold")) top().bold = true;
                }
            } else pop(name);
            return;
        }
        if (is("a")) {
            if (!end) {
                push(name);
                if (attr(attrs, "href", a, sizeof(a)) && a[0]) {
                    char full[600];
                    url_resolve(base, a, full, sizeof(full));
                    d.links.push(strdup(full));
                    top().link = d.links.len - 1;
                    top().color = readable(0xFF2F5DA8, d.bg);
                    top().underline = true;
                }
            } else pop(name);
            return;
        }
        if (is("img")) {
            if (end) return;
            char alt[128] = "";
            attr(attrs, "alt", alt, sizeof(alt));
            char wv[16] = "", hv[16] = "";
            bool has_w = attr(attrs, "width", wv, sizeof(wv)) && !strchr(wv, '%');
            bool has_h = attr(attrs, "height", hv, sizeof(hv)) && !strchr(hv, '%');
            int w = has_w ? atoi(wv) : 0;
            int h = has_h ? atoi(hv) : 0;
            if (cur && cur->width > 0) { w = cur->width; has_w = true; }
            if (cur && cur->height > 0) { h = cur->height; has_h = true; }
            if ((has_w && w <= 1) || (has_h && h <= 1)) return;  // tracking pixel
            // where the picture lives (lazy loaders often keep it in data-src)
            char srcv[600] = "";
            if (!attr(attrs, "src", srcv, sizeof(srcv)) || !srcv[0] || str_ieq_prefix(srcv, "data:"))
                attr(attrs, "data-src", srcv, sizeof(srcv));
            int img = -1;
            int nw = 0, nh = 0;
            if (srcv[0] && !str_ieq_prefix(srcv, "data:")) {
                char full[600];
                url_resolve(base, srcv, full, sizeof(full));
                for (int i = 0; i < d.img_urls.len && img < 0; i++) if (!strcmp(d.img_urls[i], full)) img = i;
                if (img < 0 && d.img_urls.len < 400) { d.img_urls.push(strdup(full)); d.img_sized.push(1); img = d.img_urls.len - 1; }
                if (img >= 0 && !(w && h)) d.img_sized[img] = 0;
                if (img >= 0 && d.img_size && !d.img_size(d.img_ctx, d.img_urls[img], &nw, &nh)) nw = nh = 0;
            }
            if (nw > 0 && nh > 0) {             // the picture is loaded: fill in missing sizes from it
                if (!w && !h) { w = nw; h = nh; }
                else if (!h) h = max(1, (int)((i64)nh * w / nw));
                else if (!w) w = max(1, (int)((i64)nw * h / nh));
            } else if (!w || !h) {              // not known yet: a placeholder sized by the alt text
                if (!w) w = alt[0] ? min(300, (int)strlen(alt) * 8 + 34) : 48;
                if (!h) h = 36;
            }
            int maxw = right() - left();
            if (w > maxw) { h = max(1, (int)((i64)h * maxw / w)); w = maxw; }
            if (h > 900) { w = max(1, (int)((i64)w * 900 / h)); h = 900; }
            w = max(w, 4);
            h = max(h, 4);
            if (x + w > right() && x > left()) finish_line();
            if (space_pending && x > left()) x += 8;
            space_pending = false;
            HtmlItem& it = add(HK_IMAGE, w, h);
            it.text = pool_add(alt, (int)strlen(alt));
            it.tlen = (int)strlen(alt);
            it.color = 0xFF8A7EA8;
            it.img = img;
            x += w + 4;
            d.images++;
            return;
        }
        if (is("input") || is("button") || is("textarea") || is("select")) {
            if (end) {
                if (is("textarea")) in_textarea = -1;
                if (is("select")) { in_select = false; }
                if (is("button")) {}
                return;
            }
            HtmlField f;
            memset(&f, 0, sizeof(f));
            f.form = cur_form;
            attr(attrs, "name", f.name, sizeof(f.name));
            char t[16] = "text";
            if (is("input")) attr(attrs, "type", t, sizeof(t));
            else if (is("button")) strlcpy(t, "submit", sizeof(t));
            else if (is("textarea")) strlcpy(t, "textarea", sizeof(t));
            else strlcpy(t, "select", sizeof(t));
            for (char* c = t; *c; c++) *c = to_lower(*c);
            strlcpy(f.type, t, sizeof(f.type));
            attr(attrs, "value", f.value, sizeof(f.value));
            f.checked = attr(attrs, "checked", a, sizeof(a)) != nullptr;
            // keep what the user typed across re-layouts
            if (keep && field_count < keep->fields.len && !strcmp(keep->fields[field_count].type, f.type)) {
                strlcpy(f.value, keep->fields[field_count].value, sizeof(f.value));
                f.checked = keep->fields[field_count].checked;
            }
            field_count++;
            d.fields.push(f);
            int fi = d.fields.len - 1;
            if (!strcmp(t, "hidden")) return;
            if (space_pending && x > left()) x += 8;
            space_pending = false;
            if (!strcmp(t, "submit") || !strcmp(t, "button") || !strcmp(t, "reset") || !strcmp(t, "image")) {
                const char* label = f.value[0] ? f.value : (is("button") ? "" : "Submit");
                if (is("button") && !label[0]) label = "OK";
                int w = (int)strlen(label) * 8 + 20;
                if (x + w > right() && x > left()) finish_line();
                HtmlItem& it = add(HK_BUTTON, w, 22);
                it.field = fi;
                it.text = pool_add(label, (int)strlen(label));
                it.tlen = (int)strlen(label);
                x += w + 6;
            } else if (!strcmp(t, "checkbox") || !strcmp(t, "radio")) {
                HtmlItem& it = add(HK_CHECK, 14, 14);
                it.field = fi;
                x += 20;
            } else if (!strcmp(t, "select")) {
                in_select = true;
                select_first = true;
                select_field = fi;
            } else {
                char sz[8];
                int w = attr(attrs, "size", sz, sizeof(sz)) ? atoi(sz) * 8 + 12 : 220;
                w = clamp(w, 80, right() - left());
                int h = !strcmp(t, "textarea") ? 72 : 20;
                if (!strcmp(t, "textarea")) { newline(); w = min(right() - left(), 480); in_textarea = fi; }
                if (x + w > right() && x > left()) finish_line();
                HtmlItem& it = add(HK_FIELD, w, h);
                it.field = fi;
                x += w + 6;
            }
            return;
        }
        if (is("body") && !end) {
            u32 c;
            if (attr(attrs, "bgcolor", a, sizeof(a)) && parse_color(a, c)) { d.bg = c; for (int i = 0; i <= sp; i++) st[i].bgc = c; }
            if (attr(attrs, "text", a, sizeof(a)) && parse_color(a, c)) { d.fg = c; st[0].color = c; top().color = c; }
            return;
        }
    }

    static const char* strstr_i(const char* h, const char* n) {
        for (; *h; h++) if (str_ieq_prefix(h, n)) return h;
        return nullptr;
    }

    // ------------------------------------------------------------ css: element open / close
    void make_elem(const char* name, const char* attrs, CssElem& e) {
        memset(&e, 0, sizeof(e));
        e.tag = css_hash_lower(name, (int)strlen(name));
        char v[256];
        if (attr(attrs, "id", v, sizeof(v)) && v[0]) e.id = css_hash(v, (int)strlen(v));
        if (attr(attrs, "class", v, sizeof(v))) {
            const char* p = v;
            while (*p && e.ncls < 8) {
                while (*p == ' ' || *p == '\t' || *p == '\n') p++;
                const char* q = p;
                while (*q && *q != ' ' && *q != '\t' && *q != '\n') q++;
                if (q > p) e.cls[e.ncls++] = css_hash(p, (int)(q - p));
                p = q;
            }
        }
        // attribute names (and values) for [attr] selectors
        const char* p = attrs;
        while (*p && e.nattrs < 10) {
            while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r' || *p == '/') p++;
            const char* an = p;
            while (*p && *p != '=' && *p != ' ' && *p != '>' && *p != '\n' && *p != '\t' && *p != '/') p++;
            int al = (int)(p - an);
            if (!al) { if (*p) p++; continue; }
            u32 vh = css_hash("", 0) | 1;
            while (*p == ' ') p++;
            if (*p == '=') {
                p++;
                while (*p == ' ') p++;
                const char* vs;
                int vl;
                if (*p == '"' || *p == '\'') { char q = *p++; vs = p; while (*p && *p != q) p++; vl = (int)(p - vs); if (*p) p++; }
                else { vs = p; while (*p && *p != ' ' && *p != '>' && *p != '\n' && *p != '\t') p++; vl = (int)(p - vs); }
                vh = css_hash(vs, vl) | 1;
            }
            e.attrs[e.nattrs] = css_hash_lower(an, al);
            e.attr_vals[e.nattrs] = vh;
            e.nattrs++;
            if (al == 4 && str_ieq_prefix(an, "href")) e.has_href = true;
        }
    }

    // skip a hidden element's whole subtree in the source
    void skip_element(const char* name) {
        int nl = (int)strlen(name);
        int depth = 1;
        bool optional_end = !strcmp(name, "li") || !strcmp(name, "p") || !strcmp(name, "dt") || !strcmp(name, "dd") ||
                            !strcmp(name, "option") || !strcmp(name, "tr") || !strcmp(name, "td") || !strcmp(name, "th");
        while (pos < srclen) {
            if (src[pos] != '<') { pos++; continue; }
            if (pos + 3 < srclen && src[pos + 1] == '!' && src[pos + 2] == '-' && src[pos + 3] == '-') {
                pos += 4;
                while (pos + 2 < srclen && !(src[pos] == '-' && src[pos + 1] == '-' && src[pos + 2] == '>')) pos++;
                pos += 3;
                continue;
            }
            bool end = src[pos + 1] == '/';
            u32 ns = pos + (end ? 2 : 1);
            bool same = ns + nl <= srclen && str_ieq_prefix(src + ns, name) && !((src[ns + nl] >= 'a' && src[ns + nl] <= 'z') ||
                                                                               (src[ns + nl] >= 'A' && src[ns + nl] <= 'Z') ||
                                                                               (src[ns + nl] >= '0' && src[ns + nl] <= '9') || src[ns + nl] == '-');
            if (optional_end && depth == 1 && end && !same) {
                // the parent is closing: an unclosed <li>/<p> ends here (leave the parent's end tag)
                const char* closers[] = {"ul", "ol", "menu", "div", "table", "tbody", "body", "section", "dl", "select", "td", "tr"};
                for (auto c : closers) {
                    int cl = (int)strlen(c);
                    if (str_ieq_prefix(src + ns, c) && !((src[ns + cl] >= 'a' && src[ns + cl] <= 'z'))) return;
                }
            }
            if (same) {
                if (!end && optional_end && depth == 1) return;     // a sibling starts: we're done
                if (end) depth--; else depth++;
                if (!depth) { while (pos < srclen && src[pos] != '>') pos++; pos++; return; }
            }
            pos++;
        }
    }

    int find_open(const char* name, const char* stop1 = nullptr, const char* stop2 = nullptr, const char* stop3 = nullptr) {
        for (int k = edepth - 1; k >= 0; k--) {
            if (!strcmp(es[k].tag, name)) return k;
            if ((stop1 && !strcmp(es[k].tag, stop1)) || (stop2 && !strcmp(es[k].tag, stop2)) || (stop3 && !strcmp(es[k].tag, stop3))) return -1;
        }
        return -1;
    }

    // HTML's optional end tags: <p> ends at the next block, <li> at the next <li>, ...
    void auto_close(const char* name) {
        auto is = [&](const char* t) { return !strcmp(name, t); };
        if (edepth && !strcmp(es[edepth - 1].tag, "p") && is_block_tag(name) && !is("li") && !is("dt") && !is("dd") && !is("tr"))
            close_elem("p");
        int k = -1;
        if (is("li")) k = find_open("li", "ul", "ol", "menu");
        else if (is("dt") || is("dd")) { k = find_open("dd", "dl"); if (k < 0) k = find_open("dt", "dl"); }
        else if (is("td") || is("th")) { k = find_open("td", "tr", "table"); if (k < 0) k = find_open("th", "tr", "table"); }
        else if (is("tr")) k = find_open("tr", "table");
        else if (is("option")) k = find_open("option", "select");
        if (k >= 0) close_elem(es[k].tag);
    }

    void open_elem(const char* name, const char* attrs) {
        bool vd = is_void_tag(name) || (attrs[0] && attrs[strlen(attrs) - 1] == '/' && !is_block_tag(name) && strcmp(name, "a") && strcmp(name, "span"));
        if (!strcmp(name, "head")) return;
        auto_close(name);
        CssComputed cc;
        int slot = min(edepth, 63);
        make_elem(name, attrs, estack[slot]);
        char sty[600];
        const char* inline_style = attr(attrs, "style", sty, sizeof(sty)) ? sty : nullptr;
        css_compute(sheet, estack, slot + 1, width, inline_style, cc);
        char tmp[8], tmp_bg[32];
        bool root = !strcmp(name, "html") || !strcmp(name, "body");
        if (!root && (cc.display == CD_NONE || cc.hidden || (attr(attrs, "hidden", tmp, sizeof(tmp)) && cc.display == CD_UNSET))) {
            if (!vd) skip_element(name);
            return;
        }
        // old-school bgcolor="" works like a css background
        if (!cc.has_bg && attr(attrs, "bgcolor", tmp_bg, sizeof(tmp_bg))) { u32 c; if (parse_color(tmp_bg, c)) { cc.has_bg = true; cc.bg = c; } }
        bool default_block = is_block_tag(name);
        bool table_part = !strcmp(name, "tr") || !strcmp(name, "td") || !strcmp(name, "th") || !strcmp(name, "table");
        bool parent_flex_row = edepth > 0 && es[edepth - 1].flex_row;
        bool force_inline = !vd && default_block && !table_part && !root &&
                            (cc.display == CD_INLINE || cc.display == CD_INLINE_BLOCK ||
                             (parent_flex_row && cc.display != CD_BLOCK && cc.display != CD_FLEX && cc.display != CD_TABLE));
        bool promote = !vd && !default_block && !root && !in_cell &&
                       (cc.display == CD_BLOCK || cc.display == CD_FLEX || cc.display == CD_LIST_ITEM || cc.display == CD_TABLE) &&
                       strcmp(name, "img") && strcmp(name, "input") && strcmp(name, "button") && strcmp(name, "select") &&
                       strcmp(name, "textarea") && strcmp(name, "svg");
        cur = &cc;
        int sp0 = sp;
        if (force_inline) {
            push(name);
            if (x > left()) space_pending = true;
        } else {
            if (promote) newline();
            tag(name, false, attrs);
        }
        cur = nullptr;
        if (vd) return;
        bool generic = sp == sp0;
        if (generic) push(name);
        if (edepth >= 64) return;
        ElemState& e = es[edepth];
        memset(&e, 0, sizeof(e));
        strlcpy(e.tag, name, sizeof(e.tag));
        e.generic = generic;
        e.forced_inline = force_inline;
        e.promote = promote;
        e.block = ((default_block && !force_inline) || promote) && (!in_cell || table_part);
        e.bg_item = e.bar_item = -1;
        e.flex_row = cc.display == CD_FLEX && !cc.flex_column;
        edepth++;
        apply_css(cc, e, root);
    }

    void apply_css(const CssComputed& cc, ElemState& e, bool root) {
        Style& t = top();
        Style& parent = st[max(sp - 1, 0)];
        if (root && cc.has_bg && cc.bg) {
            d.bg = cc.bg;
            for (int i = 0; i <= sp; i++) st[i].bgc = cc.bg;
        }
        if (cc.has_color) {
            t.color = readable(cc.color, t.bgc);
            if (root) { d.fg = t.color; st[0].color = t.color; }
        }
        if (cc.bold >= 0) t.bold = cc.bold;
        if (cc.italic == 1 && !cc.has_color) t.color = readable(0xFF6A4C9C, t.bgc);
        if (cc.mono >= 0 && !e.block) t.mono = cc.mono;
        if (cc.underline >= 0) t.underline = cc.underline;
        if (cc.strike >= 0) t.strike = cc.strike;
        if (cc.upper >= 0) t.upper = cc.upper;
        if (cc.pre >= 0) t.pre = cc.pre;
        if (cc.list_none >= 0) t.nobullet = cc.list_none;
        int px = cc.font_px ? cc.font_px : cc.font_pct ? parent.fpx * cc.font_pct / 100 : 0;
        if (px > 0) {
            t.fpx = px;
            t.scale = px >= 44 ? 3 : px >= 23 ? 2 : 1;
        }
        if (!e.block) {
            if (cc.has_bg && cc.bg && cc.bg != t.bgc) t.ibg = cc.bg;
            return;
        }
        // a block inside an element with an inline background (a <td bgcolor> holding a table) paints it as a box
        u32 bgcol = cc.has_bg ? cc.bg : t.ibg;
        t.ibg = 0;
        if (cc.align >= 0) t.align = cc.align;
        // vertical margin before the box
        int mt = cc.margin[0];
        if (mt != CSS_UNSET && mt > 4 && !root) blank_line(min(mt, 32) * 2 / 3);
        // horizontal margins, width and centring
        int avail = right() - left();
        int ml = cc.margin[3] > 0 ? min(cc.margin[3], avail / 3) : 0;
        int mr = cc.margin[1] > 0 ? min(cc.margin[1], avail / 3) : 0;
        t.indent += ml;
        t.rindent += mr;
        avail = right() - left();
        int mw = cc.max_width;
        if (cc.width >= 240 || (cc.width > 0 && cc.margin_auto_lr)) mw = mw < 0 ? cc.width : min(mw, cc.width);
        if (mw > 0 && mw < avail) {
            int extra = avail - mw;
            if (cc.margin_auto_lr) { t.indent += extra / 2; t.rindent += extra - extra / 2; }
            else t.rindent += extra;
        }
        bool bg = bgcol && bgcol != t.bgc && !root;
        e.y0 = y;
        e.items0 = d.items.len;
        bool any_box = bg || cc.border_w[0] > 0 || cc.border_w[3] > 0 || (cc.padding[0] > 0 && cc.padding[0] != CSS_UNSET);
        if (any_box) newline();
        if (cc.border_w[0] > 0 && !root) {
            HtmlItem& it = add(HK_RECT, right() - left(), min(cc.border_w[0], 4));
            it.x = left();
            it.color = cc.border_c[0] ? (cc.border_c[0] | 0xFF000000) : 0xFFC9C0DE;
            y += min(cc.border_w[0], 4);
            line_start = d.items.len;
        }
        if (bg) {
            HtmlItem& it = add(HK_RECT, right() - left(), 0);
            it.x = left();
            it.color = bgcol;
            e.bg_item = d.items.len - 1;
            e.bg_y = y;
            line_start = d.items.len;
            t.bgc = bgcol;
            // text that would vanish on the new background
            t.color = readable(t.color, bgcol);
        }
        if (cc.border_w[3] > 0 && !root) {
            HtmlItem& it = add(HK_RECT, min(cc.border_w[3], 4), 0);
            it.x = left();
            it.color = cc.border_c[3] ? (cc.border_c[3] | 0xFF000000) : 0xFFC9C0DE;
            e.bar_item = d.items.len - 1;
            e.bar_y = y;
            line_start = d.items.len;
            t.indent += 8;
        }
        int pt = cc.padding[0] > 0 ? min(cc.padding[0], 24) : 0;
        int pl = cc.padding[3] > 0 ? min(cc.padding[3], 40) : 0;
        int pr = cc.padding[1] > 0 ? min(cc.padding[1], 40) : 0;
        if (bg && pl < 6) pl = 6;
        if (bg && pr < 6) pr = 6;
        y += pt;
        t.indent += pl;
        t.rindent += pr;
        e.pad_b = cc.padding[2] > 0 ? min(cc.padding[2], 24) : (bg ? 4 : 0);
        e.margin_b = cc.margin[2] != CSS_UNSET && cc.margin[2] > 4 ? min(cc.margin[2], 32) * 2 / 3 : 0;
        e.border_b = cc.border_w[2] > 0 ? min(cc.border_w[2], 4) : 0;
        e.border_b_c = cc.border_c[2] ? (cc.border_c[2] | 0xFF000000) : 0xFFC9C0DE;
        if (x < left() || x == MARGIN) x = left();
        e.content0 = d.items.len;
    }

    void finish_elem(int k) {
        ElemState& e = es[k];
        if (e.forced_inline) {
            pop(e.tag);
            space_pending = true;
            return;
        }
        if (e.block && (e.bg_item >= 0 || e.bar_item >= 0) && d.items.len == e.content0) {
            // nothing inside: a decorative box (often positioned behind floats) - drop it
            for (int i = e.items0; i < d.items.len; i++) d.items[i].h = 0;
            y = e.y0;
            e.pad_b = e.border_b = 0;
            e.bg_item = e.bar_item = -1;
        }
        if (e.block && (e.bg_item >= 0 || e.bar_item >= 0 || e.pad_b || e.border_b)) {
            newline();
            y += e.pad_b;
            if (e.bg_item >= 0 && e.bg_item < d.items.len) d.items[e.bg_item].h = max(0, y - e.bg_y);
            if (e.bar_item >= 0 && e.bar_item < d.items.len) d.items[e.bar_item].h = max(0, y - e.bar_y);
            if (e.border_b) {
                HtmlItem& it = add(HK_RECT, right() - left(), e.border_b);
                it.x = left();
                it.color = e.border_b_c;
                y += e.border_b + 2;
            }
            line_start = d.items.len;
        }
        tag(e.tag, true, "");
        if (e.generic) pop(e.tag);
        if (e.margin_b) blank_line(e.margin_b);
        if (e.promote) newline();
    }

    void close_elem(const char* name) {
        int k = -1;
        for (int i = edepth - 1; i >= 0; i--) if (!strcmp(es[i].tag, name)) { k = i; break; }
        if (k < 0) { if (strcmp(name, "head")) tag(name, true, ""); return; }
        for (int i = edepth - 1; i >= k; i--) { edepth = i + 1; finish_elem(i); }
        edepth = k;
    }

    void run(const char* s, u32 len) {
        src = s;
        srclen = len;
        pos = 0;
        while (pos < len) {
            u32 i = pos;
            if (s[i] != '<') {
                u32 j = i;
                while (j < len && s[j] != '<') j++;
                text(s + i, (int)(j - i));
                pos = j;
                continue;
            }
            // comment
            if (i + 3 < len && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
                u32 j = i + 4;
                while (j + 2 < len && !(s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>')) j++;
                pos = j + 3;
                continue;
            }
            if (i + 1 < len && (s[i + 1] == '!' || s[i + 1] == '?')) {
                while (i < len && s[i] != '>') i++;
                pos = i + 1;
                continue;
            }
            bool end = i + 1 < len && s[i + 1] == '/';
            u32 ns = i + (end ? 2 : 1);
            if (ns >= len || !((s[ns] >= 'a' && s[ns] <= 'z') || (s[ns] >= 'A' && s[ns] <= 'Z'))) {
                text(s + i, 1);
                pos = i + 1;
                continue;
            }
            char name[16];
            int nl = 0;
            u32 k = ns;
            while (k < len && s[k] != ' ' && s[k] != '>' && s[k] != '/' && s[k] != '\n' && s[k] != '\t' && s[k] != '\r') {
                if (nl < 15) name[nl++] = to_lower(s[k]);
                k++;
            }
            name[nl] = 0;
            // find the end of the tag, respecting quotes
            u32 ae = k;
            char q = 0;
            while (ae < len && (q || s[ae] != '>')) {
                if (q) { if (s[ae] == q) q = 0; }
                else if (s[ae] == '"' || s[ae] == '\'') q = s[ae];
                ae++;
            }
            char attrs[1024];
            u32 alen = min<u32>(ae - k, sizeof(attrs) - 1);
            memcpy(attrs, s + k, alen);
            attrs[alen] = 0;
            pos = ae + 1;
            // raw text elements: skip their contents entirely
            if (!end && (!strcmp(name, "script") || !strcmp(name, "style") ||
                         !strcmp(name, "svg") || !strcmp(name, "template") || !strcmp(name, "iframe"))) {
                char close[24];
                snprintf(close, sizeof(close), "</%s", name);
                int cl = (int)strlen(close);
                u32 j = pos;
                while (j < len && !(s[j] == '<' && j + cl <= len && str_ieq_prefix(s + j, close))) j++;
                while (j < len && s[j] != '>') j++;
                pos = j + 1;
                continue;
            }
            if (end) close_elem(name);
            else open_elem(name, attrs);
        }
        while (edepth > 0) { edepth--; finish_elem(edepth); }
        finish_line();
        d.height = y + MARGIN;
    }
};

// find <style> blocks and <link rel=stylesheet> in the page: sheets get parsed (when given one),
// linked sheet addresses go to doc.css_urls for the browser to fetch
static bool media_screen(const char* m) {
    return !(str_icontains(m, "print") || str_icontains(m, "speech")) || str_icontains(m, "screen") || str_icontains(m, "all");
}
static void collect_css(const char* s, u32 len, const char* base, HtmlDoc& d, CssSheet* sheet) {
    for (u32 i = 0; i + 6 < len; i++) {
        if (s[i] != '<') continue;
        bool style = str_ieq_prefix(s + i + 1, "style") && (s[i + 6] == '>' || s[i + 6] == ' ' || s[i + 6] == '\n');
        bool link = str_ieq_prefix(s + i + 1, "link") && (s[i + 5] == ' ' || s[i + 5] == '\n' || s[i + 5] == '\t');
        if (!style && !link) {
            // don't look inside scripts
            if (str_ieq_prefix(s + i + 1, "script")) {
                u32 j = i + 7;
                while (j + 8 < len && !(s[j] == '<' && str_ieq_prefix(s + j, "</script"))) j++;
                i = j;
            }
            continue;
        }
        u32 ae = i + 1;
        char q = 0;
        while (ae < len && (q || s[ae] != '>')) { if (q) { if (s[ae] == q) q = 0; } else if (s[ae] == '"' || s[ae] == '\'') q = s[ae]; ae++; }
        char attrs[1024];
        u32 alen = min<u32>(ae - i - 1, sizeof(attrs) - 1);
        memcpy(attrs, s + i + 1, alen);
        attrs[alen] = 0;
        char m[128] = "", tmp[600];
        Layout::attr(attrs, "media", m, sizeof(m));
        if (style) {
            u32 j = ae + 1;
            while (j + 7 < len && !(s[j] == '<' && str_ieq_prefix(s + j, "</style"))) j++;
            if (sheet && media_screen(m)) css_add(sheet, s + ae + 1, j - (ae + 1));
            i = j;
        } else {
            char rel[64];
            if (!Layout::attr(attrs, "rel", rel, sizeof(rel)) || !str_icontains(rel, "stylesheet") || str_icontains(rel, "alternate")) continue;
            if (!Layout::attr(attrs, "href", tmp, sizeof(tmp)) || !tmp[0] || !media_screen(m)) continue;
            char full[600];
            url_resolve(base, tmp, full, sizeof(full));
            if (d.css_urls.len < 12) d.css_urls.push(strdup(full));
            i = ae;
        }
    }
}

void html_free_css(HtmlDoc& doc) {
    if (doc.css) css_free((CssSheet*)doc.css);
    doc.css = nullptr;
    doc.css_key = 0;
}

void html_layout(const char* html, u32 len, const char* base_url, int width, HtmlDoc& doc, bool plain_text, const HtmlDoc* keep) {
    doc.clear();
    if (!doc.bg) doc.bg = 0xFFFBF9FF;
    if (!doc.fg) doc.fg = 0xFF221C30;
    CssSheet* sheet = nullptr;
    if (!plain_text) {
        // the parsed sheet is cached: it only changes with the page or the fetched sheets
        u32 key = (u32)(u64)html ^ (len * 2654435761u) ^ (doc.ext_css_len * 40503u) ^ (u32)(u64)doc.ext_css;
        if (!doc.css || doc.css_key != key) {
            html_free_css(doc);
            sheet = css_new();
            if (doc.ext_css && doc.ext_css_len) css_add(sheet, doc.ext_css, doc.ext_css_len);
            collect_css(html, len, base_url, doc, sheet);
            doc.css = sheet;
            doc.css_key = key;
        } else {
            sheet = (CssSheet*)doc.css;
            collect_css(html, len, base_url, doc, nullptr);
        }
    }
    Layout* l = new Layout(doc, base_url, max(width, 200), keep);
    l->sheet = sheet;
    if (plain_text) {
        l->push("pre");
        l->top().pre = true;
        l->text(html, (int)len);
        l->finish_line();
        doc.height = l->y + MARGIN;
    } else l->run(html, len);
    delete l;
}
