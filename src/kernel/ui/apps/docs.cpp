// silicon - Docs: a reader for office documents. Word (.docx) and OpenDocument text (.odt) show as pages
// of text, spreadsheets (.xlsx, .ods, .csv, .tsv) as a grid with sheet tabs. Read-only, with
// "save as text / csv" so the content can be edited in Slate.
#include "apps.h"

u8* deflate_decompress(const u8* in, u32 inlen, u32* outlen, u32 max_out);   // gfx/imgdec.cpp

bool is_office_name(const char* n) {
    return str_icontains(n, ".docx") || str_icontains(n, ".xlsx") || str_icontains(n, ".odt") || str_icontains(n, ".ods") ||
           str_icontains(n, ".csv") || str_icontains(n, ".tsv");
}

// ---- reading files out of a zip container
static u32 rd16(const u8* p) { return p[0] | (p[1] << 8); }
static u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

// returns a kmalloc'd, NUL-terminated copy of the entry, or nullptr
static char* zip_extract(FsNode* z, const char* name, u32* out_len) {
    const u8* d = z->data;
    u32 n = z->size;
    if (n < 22) return nullptr;
    int eocd = -1;
    for (int i = (int)n - 22; i >= 0 && i > (int)n - 65600; i--) if (rd32(d + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) return nullptr;
    u32 count = rd16(d + eocd + 10), off = rd32(d + eocd + 16);
    for (u32 i = 0; i < count; i++) {
        if (off + 46 > n || rd32(d + off) != 0x02014b50) break;
        u32 method = rd16(d + off + 10), csize = rd32(d + off + 20), usize = rd32(d + off + 24);
        u32 nl = rd16(d + off + 28), xl = rd16(d + off + 30), cl = rd16(d + off + 32), loff = rd32(d + off + 42);
        if (off + 46 + nl > n) break;
        if (nl == strlen(name) && !memcmp(d + off + 46, name, nl)) {
            if (loff + 30 > n || rd32(d + loff) != 0x04034b50) return nullptr;
            u32 start = loff + 30 + rd16(d + loff + 26) + rd16(d + loff + 28);
            if (start + csize > n) return nullptr;
            char* out = nullptr;
            u32 len = 0;
            if (method == 0) {
                out = (char*)kmalloc(csize + 1);
                memcpy(out, d + start, csize);
                len = csize;
            } else if (method == 8) {
                u32 ol = 0;
                u8* o = deflate_decompress(d + start, csize, &ol, max(usize, 1u) + 64);
                if (!o) return nullptr;
                out = (char*)krealloc(o, ol + 1);
                len = ol;
            } else return nullptr;
            out[len] = 0;
            if (out_len) *out_len = len;
            return out;
        }
        off += 46 + nl + xl + cl;
    }
    return nullptr;
}

// ---- a tiny XML walker: kind 0 open tag, 1 close tag, 2 self-closing tag, 3 text
template <class F> static void xml_walk(const char* s, u32 len, F f) {
    const char* p = s;
    const char* end = s + len;
    while (p < end) {
        if (*p == '<') {
            const char* q = p + 1;
            if (q < end && (*q == '?' || *q == '!')) { while (q < end && *q != '>') q++; p = q + 1; continue; }
            bool closing = q < end && *q == '/';
            if (closing) q++;
            char name[48];
            int nn = 0;
            while (q < end && *q != ' ' && *q != '>' && *q != '/' && *q != '\n' && *q != '\r' && *q != '\t') { if (nn < 47) name[nn++] = *q; q++; }
            name[nn] = 0;
            const char* attrs = q;
            while (q < end && *q != '>') q++;
            bool self = q > p && q[-1] == '/';
            f(closing ? 1 : self ? 2 : 0, name, attrs, (int)(q - attrs), (const char*)nullptr, 0);
            p = q + 1;
        } else {
            const char* q = p;
            while (q < end && *q != '<') q++;
            f(3, "", (const char*)nullptr, 0, p, (int)(q - p));
            p = q;
        }
    }
}
static bool attr_get(const char* attrs, int alen, const char* key, char* out, int cap) {
    int kl = (int)strlen(key);
    for (int i = 0; i + kl + 2 < alen; i++) {
        if ((i == 0 || attrs[i - 1] == ' ') && !memcmp(attrs + i, key, kl) && attrs[i + kl] == '=') {
            char q = attrs[i + kl + 1];
            int o = 0;
            for (int j = i + kl + 2; j < alen && attrs[j] != q && o < cap - 1; j++) out[o++] = attrs[j];
            out[o] = 0;
            return true;
        }
    }
    return false;
}
// text with XML entities, converted to silicon's codepage, appended to a growing buffer
static void append_text(Vec<char>& dst, const char* t, int n) {
    char* tmp = (char*)kmalloc(n + 1);
    int o = 0;
    for (int i = 0; i < n;) {
        if (t[i] == '&') {
            const char* e = nullptr;
            for (int k = 1; k < min(n - i, 10); k++) if (t[i + k] == ';') { e = t + i + k; break; }
            if (e) {
                int l = (int)(e - (t + i)) - 1;
                const char* ent = t + i + 1;
                char c = 0;
                if (l == 3 && !memcmp(ent, "amp", 3)) c = '&';
                else if (l == 2 && !memcmp(ent, "lt", 2)) c = '<';
                else if (l == 2 && !memcmp(ent, "gt", 2)) c = '>';
                else if (l == 4 && !memcmp(ent, "quot", 4)) c = '"';
                else if (l == 4 && !memcmp(ent, "apos", 4)) c = '\'';
                else if (l >= 2 && ent[0] == '#') {
                    int v = ent[1] == 'x' ? 0 : atoi(ent + 1);
                    if (ent[1] == 'x') for (int k = 2; k <= l; k++) { char h = ent[k]; v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10); }
                    char u[5];
                    int ul = utf8_encode((u32)v, u);
                    for (int k = 0; k < ul; k++) tmp[o++] = u[k];
                    i += l + 2;
                    continue;
                }
                if (c) { tmp[o++] = c; i += l + 2; continue; }
            }
        }
        tmp[o++] = t[i++];
    }
    tmp[o] = 0;
    char* sil = (char*)kmalloc(o * 2 + 8);
    utf8_to_silicon(tmp, o, sil, o * 2 + 8);
    for (char* p = sil; *p; p++) dst.push(*p);
    kfree(sil);
    kfree(tmp);
}
static char* vec_str(Vec<char>& v) {
    char* s = (char*)kmalloc(v.len + 1);
    for (int i = 0; i < v.len; i++) s[i] = v[i];
    s[v.len] = 0;
    v.clear();
    return s;
}

struct DocsApp : App {
    struct Block { char* text; u8 style; bool bullet; };     // style: 0 body, 1..3 headings
    struct Line { const char* s; int n; u8 style; bool bullet_first, bullet_cont, end; };
    struct Sheet { char name[32]; Vec<char*> cells; int nrows = 0, ncols = 0; };

    FsNode* file = nullptr;
    char err[96] = "";
    bool is_sheet = false;
    Vec<Block> blocks;
    Vec<Line> lines;
    int lay_w = -1, doc_h = 0;
    Vec<Sheet*> sheets;
    int cur_sheet = 0;
    Vec<int> col_w;
    Smooth sc;
    int hx = 0;
    ScrollBar sb;
    int cw = 0, ch = 0;
    static const int BAR = 34, ROW = 18, HEADW = 40, TABS = 24;

    DocsApp(const char* arg) { if (arg && arg[0]) open_arg(arg); }
    ~DocsApp() { clear(); }
    const char* app_id() override { return "docs"; }
    char tbuf[80];
    const char* title() override {
        if (file) { snprintf(tbuf, sizeof(tbuf), "%s - Docs", file->name); return tbuf; }
        return "Docs";
    }
    int pref_w() override { return 700; }
    int pref_h() override { return 520; }
    u32 background() override { return 0xFF5A5470; }

    void clear() {
        for (int i = 0; i < blocks.len; i++) kfree(blocks[i].text);
        blocks.clear();
        lines.clear();
        for (int i = 0; i < sheets.len; i++) {
            for (int k = 0; k < sheets[i]->cells.len; k++) if (sheets[i]->cells[k]) kfree(sheets[i]->cells[k]);
            sheets[i]->cells.free();
            delete sheets[i];
        }
        sheets.clear();
        col_w.clear();
        lay_w = -1;
        is_sheet = false;
        cur_sheet = 0;
        sc.jump(0);
        hx = 0;
    }
    void push_block(Vec<char>& txt, int style, bool bullet) {
        Block b;
        b.text = vec_str(txt);
        b.style = (u8)style;
        b.bullet = bullet;
        blocks.push(b);
    }

    // ---- word documents
    void parse_docx(char* xml, u32 len) {
        Vec<char> para, cell, row;
        int style = 0, tdepth = 0;
        bool bullet = false, in_t = false, any_cell = false;
        xml_walk(xml, len, [&](int kind, const char* name, const char* attrs, int alen, const char* text, int tlen) {
            if (kind == 3) { if (in_t) append_text(para, text, tlen); return; }
            if (kind == 1) {
                if (!strcmp(name, "w:t")) in_t = false;
                else if (!strcmp(name, "w:p")) {
                    if (tdepth > 0) {
                        if (cell.len) cell.push(' ');
                        for (int i = 0; i < para.len; i++) cell.push(para[i]);
                        para.clear();
                    } else if (para.len || true) push_block(para, style, bullet);
                } else if (!strcmp(name, "w:tc")) {
                    if (any_cell) { row.push(' '); row.push('|'); row.push(' '); }
                    for (int i = 0; i < cell.len; i++) row.push(cell[i]);
                    cell.clear();
                    any_cell = true;
                } else if (!strcmp(name, "w:tr")) { push_block(row, 0, false); any_cell = false; }
                else if (!strcmp(name, "w:tbl")) tdepth = max(0, tdepth - 1);
                return;
            }
            if (!strcmp(name, "w:p")) { style = 0; bullet = false; para.clear(); }
            else if (!strcmp(name, "w:pStyle")) {
                char v[40];
                if (attr_get(attrs, alen, "w:val", v, sizeof(v))) {
                    if (str_icontains(v, "title") || !strcmp(v, "Heading1")) style = 1;
                    else if (!strcmp(v, "Heading2") || str_icontains(v, "subtitle")) style = 2;
                    else if (!strcmp(v, "Heading3") || !strcmp(v, "Heading4")) style = 3;
                    else if (str_icontains(v, "list")) bullet = true;
                }
            } else if (!strcmp(name, "w:numPr")) bullet = true;
            else if (!strcmp(name, "w:t")) { if (kind == 0) in_t = true; }
            else if (!strcmp(name, "w:tab")) para.push(' '), para.push(' '), para.push(' '), para.push(' ');
            else if (!strcmp(name, "w:tbl")) tdepth++;
        });
        para.free(); cell.free(); row.free();
    }
    // ---- OpenDocument text
    void parse_odt(char* xml, u32 len) {
        Vec<char> para;
        int style = 0;
        bool bullet = false;
        xml_walk(xml, len, [&](int kind, const char* name, const char* attrs, int alen, const char* text, int tlen) {
            if (kind == 3) { if (para.len || true) append_text(para, text, tlen); return; }
            if (kind == 1) {
                if (!strcmp(name, "text:p") || !strcmp(name, "text:h")) { push_block(para, style, bullet); style = 0; }
                else if (!strcmp(name, "text:list-item")) bullet = false;
                return;
            }
            if (!strcmp(name, "text:p")) para.clear();
            else if (!strcmp(name, "text:h")) { para.clear(); char v[8]; style = attr_get(attrs, alen, "text:outline-level", v, sizeof(v)) ? clamp(atoi(v), 1, 3) : 1; }
            else if (!strcmp(name, "text:list-item")) bullet = true;
            else if (!strcmp(name, "text:tab")) para.push(' '), para.push(' '), para.push(' '), para.push(' ');
            else if (!strcmp(name, "text:s")) para.push(' ');
        });
        para.free();
    }
    // ---- spreadsheets
    Sheet* new_sheet(const char* name) {
        Sheet* s = new Sheet();
        strlcpy(s->name, name, sizeof(s->name));
        sheets.push(s);
        return s;
    }
    static void set_cell(Sheet* s, int r, int c, char* text) {
        if (r >= 2000 || c >= 60) { kfree(text); return; }
        if (c >= s->ncols) {
            int nc = c + 1;
            Vec<char*> nv;
            for (int i = 0; i < s->nrows; i++) for (int k = 0; k < nc; k++) nv.push(k < s->ncols ? s->cells[i * s->ncols + k] : nullptr);
            s->cells.free();
            s->cells = nv;
            s->ncols = nc;
        }
        while (s->nrows <= r) { for (int k = 0; k < s->ncols; k++) s->cells.push(nullptr); s->nrows++; }
        char*& slot = s->cells[r * s->ncols + c];
        if (slot) kfree(slot);
        slot = text;
    }
    static int col_of(const char* ref) { int c = 0; for (; *ref >= 'A' && *ref <= 'Z'; ref++) c = c * 26 + (*ref - 'A' + 1); return c - 1; }
    static int row_of(const char* ref) { while (*ref && (*ref < '0' || *ref > '9')) ref++; return atoi(ref) - 1; }

    void parse_xlsx(FsNode* z) {
        // shared strings
        Vec<char*> shared;
        u32 sl;
        if (char* ss = zip_extract(z, "xl/sharedStrings.xml", &sl)) {
            Vec<char> cur;
            bool in_si = false, in_t = false;
            xml_walk(ss, sl, [&](int kind, const char* name, const char*, int, const char* text, int tlen) {
                if (kind == 3) { if (in_si && in_t) append_text(cur, text, tlen); return; }
                if (kind == 0 && !strcmp(name, "si")) { in_si = true; cur.clear(); }
                else if (kind == 0 && !strcmp(name, "t")) in_t = true;
                else if (kind == 1 && !strcmp(name, "t")) in_t = false;
                else if (kind == 1 && !strcmp(name, "si")) { shared.push(vec_str(cur)); in_si = false; }
            });
            cur.free();
            kfree(ss);
        }
        // sheet names, in order
        Vec<char*> names;
        u32 wl;
        if (char* wb = zip_extract(z, "xl/workbook.xml", &wl)) {
            xml_walk(wb, wl, [&](int kind, const char* name, const char* attrs, int alen, const char*, int) {
                if (kind != 3 && kind != 1 && !strcmp(name, "sheet")) { char v[40]; if (attr_get(attrs, alen, "name", v, sizeof(v))) names.push(strdup(v)); }
            });
            kfree(wb);
        }
        for (int i = 0; i < 12; i++) {
            char path[48];
            snprintf(path, sizeof(path), "xl/worksheets/sheet%d.xml", i + 1);
            u32 xl;
            char* xml = zip_extract(z, path, &xl);
            if (!xml) { if (i == 0) continue; break; }
            Sheet* sh = new_sheet(i < names.len ? names[i] : path + 14);
            Vec<char> val;
            int r = 0, c = 0;
            char type[12] = "";
            bool in_v = false, in_t = false;
            xml_walk(xml, xl, [&](int kind, const char* name, const char* attrs, int alen, const char* text, int tlen) {
                if (kind == 3) { if (in_v || in_t) append_text(val, text, tlen); return; }
                if (kind == 0 && !strcmp(name, "c")) {
                    char ref[16];
                    if (attr_get(attrs, alen, "r", ref, sizeof(ref))) { c = col_of(ref); r = row_of(ref); }
                    if (!attr_get(attrs, alen, "t", type, sizeof(type))) type[0] = 0;
                    val.clear();
                } else if (kind == 0 && !strcmp(name, "v")) in_v = true;
                else if (kind == 0 && !strcmp(name, "t")) in_t = true;
                else if (kind == 1 && !strcmp(name, "v")) in_v = false;
                else if (kind == 1 && !strcmp(name, "t")) in_t = false;
                else if (kind == 1 && !strcmp(name, "c")) {
                    if (val.len) {
                        char* s = vec_str(val);
                        if (!strcmp(type, "s")) { int ix = atoi(s); kfree(s); s = strdup(ix >= 0 && ix < shared.len ? shared[ix] : ""); }
                        set_cell(sh, r, c, s);
                    }
                }
            });
            val.free();
            kfree(xml);
        }
        for (int i = 0; i < shared.len; i++) kfree(shared[i]);
        for (int i = 0; i < names.len; i++) kfree(names[i]);
        shared.free(); names.free();
    }
    void parse_ods(char* xml, u32 len) {
        Sheet* sh = nullptr;
        Vec<char> cell;
        int r = -1, c = 0, rep = 1;
        bool in_cell = false;
        xml_walk(xml, len, [&](int kind, const char* name, const char* attrs, int alen, const char* text, int tlen) {
            if (kind == 3) { if (in_cell) append_text(cell, text, tlen); return; }
            if (kind == 0 || kind == 2) {
                if (!strcmp(name, "table:table")) { char v[32] = "sheet"; attr_get(attrs, alen, "table:name", v, sizeof(v)); sh = new_sheet(v); r = -1; }
                else if (!strcmp(name, "table:table-row") && sh) { r++; c = 0; }
                else if ((!strcmp(name, "table:table-cell") || !strcmp(name, "table:covered-table-cell")) && sh) {
                    char v[12];
                    rep = attr_get(attrs, alen, "table:number-columns-repeated", v, sizeof(v)) ? clamp(atoi(v), 1, 60) : 1;
                    cell.clear();
                    in_cell = kind == 0;
                    if (kind == 2) { c += rep; in_cell = false; }
                }
            } else if (kind == 1) {
                if (!strcmp(name, "text:p") && in_cell && cell.len) cell.push(' ');
                else if (!strcmp(name, "table:table-cell") || !strcmp(name, "table:covered-table-cell")) {
                    if (cell.len && sh) { while (cell.len && cell[cell.len - 1] == ' ') cell.len--; char* s = vec_str(cell); for (int k = 0; k < rep && c + k < 60; k++) set_cell(sh, r, c + k, k ? strdup(s) : s); if (rep > 1) {} }
                    c += rep;
                    in_cell = false;
                    cell.clear();
                }
            }
        });
        cell.free();
    }
    void parse_csv(const char* t, char sep) {
        Sheet* sh = new_sheet("data");
        int r = 0, c = 0;
        Vec<char> f;
        bool q = false, any = false;
        for (; *t; t++) {
            if (q) {
                if (*t == '"') { if (t[1] == '"') { f.push('"'); t++; } else q = false; }
                else f.push(*t);
                continue;
            }
            if (*t == '"') { q = true; any = true; }
            else if (*t == sep) { if (f.len || any) set_cell(sh, r, c, vec_str(f)); c++; any = false; }
            else if (*t == '\n' || *t == '\r') {
                if (*t == '\r' && t[1] == '\n') t++;
                if (f.len || any || c) set_cell(sh, r, c, vec_str(f));
                r++; c = 0; any = false;
            } else f.push(*t);
        }
        if (f.len || any || c) set_cell(sh, r, c, vec_str(f));
        f.free();
    }

    void open_arg(const char* path) override {
        FsNode* n = fs_lookup(path);
        if (!n || n->dir) return;
        clear();
        file = n;
        err[0] = 0;
        u32 len = 0;
        if (str_icontains(n->name, ".docx")) {
            char* x = zip_extract(n, "word/document.xml", &len);
            if (x) { parse_docx(x, len); kfree(x); } else strlcpy(err, "this isn't a Word document", sizeof(err));
        } else if (str_icontains(n->name, ".odt")) {
            char* x = zip_extract(n, "content.xml", &len);
            if (x) { parse_odt(x, len); kfree(x); } else strlcpy(err, "this isn't an OpenDocument text file", sizeof(err));
        } else if (str_icontains(n->name, ".xlsx")) {
            is_sheet = true;
            parse_xlsx(n);
            if (!sheets.len) strlcpy(err, "this isn't an Excel workbook", sizeof(err));
        } else if (str_icontains(n->name, ".ods")) {
            is_sheet = true;
            char* x = zip_extract(n, "content.xml", &len);
            if (x) { parse_ods(x, len); kfree(x); } else strlcpy(err, "this isn't an OpenDocument spreadsheet", sizeof(err));
        } else if (str_icontains(n->name, ".csv") || str_icontains(n->name, ".tsv")) {
            is_sheet = true;
            parse_csv(fs_read_text(n), str_icontains(n->name, ".tsv") ? '\t' : ',');
        } else strlcpy(err, "Docs can't read this kind of file", sizeof(err));
        if (is_sheet) compute_cols();
        set_title(title());
        ui_invalidate();
    }
    bool on_drop(const char* p, int, int) override {
        FsNode* n = fs_lookup(p);
        if (!n || n->dir || !is_office_name(n->name)) return false;
        open_arg(p);
        return true;
    }
    void compute_cols() {
        col_w.clear();
        if (cur_sheet >= sheets.len) return;
        Sheet* s = sheets[cur_sheet];
        for (int c = 0; c < s->ncols; c++) {
            int w = 3;
            for (int r = 0; r < s->nrows; r++) { char* t = s->cells[r * s->ncols + c]; if (t) w = max(w, min((int)strlen(t), 24)); }
            col_w.push(w * 8 + 14);
        }
        sc.jump(0); hx = 0;
    }

    // ---- text layout
    static int line_h(const Line& l) { return l.n == 0 ? 12 : l.style == 1 ? 28 : 16; }
    void relayout(int page_w) {
        lines.clear();
        lay_w = page_w;
        int cols = max(8, (page_w - 40) / 8);
        int y = 24;
        for (int i = 0; i < blocks.len; i++) {
            Block& b = blocks[i];
            int scale = b.style == 1 ? 2 : 1;
            int bc = b.bullet ? cols - 3 : cols;
            int per = max(4, bc / scale);
            const char* s = b.text;
            int n = (int)strlen(s);
            if (!n) { lines.push({s, 0, 0, false, false, true}); y += 12; continue; }
            bool first = true;
            while (n > 0) {
                int take = min(n, per);
                if (take < n) { int k = take; while (k > 0 && s[k] != ' ') k--; if (k > per / 3) take = k; }
                lines.push({s, take, b.style, b.bullet && first, b.bullet && !first, false});
                y += scale == 2 ? 28 : 16;
                s += take; n -= take;
                while (*s == ' ' && n > 0) { s++; n--; }
                first = false;
            }
            lines[lines.len - 1].end = true;
            y += b.style ? 8 : 4;
        }
        doc_h = y + 24;
    }
    int content_h() { return is_sheet ? (cur_sheet < sheets.len ? 18 + sheets[cur_sheet]->nrows * ROW + 8 : 0) : doc_h; }
    void tick() override { if (sc.step()) ui_invalidate(); }

    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        c.fill(0, 0, w, BAR, th::panel);
        c.hline(0, BAR - 1, w, th::border);
        c.blit(icon("docs")->small, 10, 9);
        c.text(34, 11, file ? file->name : "documents", th::text, 1, true);
        draw_button(c, w - 80, 6, 70, 22, "Open" G_ELLIP, hot(w - 80, 6, 70, 22));
        if (file && !err[0]) draw_button(c, w - 190, 6, 104, 22, is_sheet ? "Save as csv" : "Save as text", hot(w - 190, 6, 104, 22));
        if (!file || err[0]) {
            c.text_centered(0, h / 2 - 12, w, err[0] ? err : "open a document, spreadsheet or csv file", 0xFFE0DAEE, 1, true);
            c.text_centered(0, h / 2 + 8, w, ".docx  .odt  .xlsx  .ods  .csv  .tsv", 0xFFB8ACD6);
            return;
        }
        if (is_sheet) draw_sheet(c, w, h);
        else draw_text(c, w, h);
    }
    void draw_text(Canvas& c, int w, int h) {
        int pw = min(w - 40, 680);
        if (lay_w != pw) relayout(pw);
        int px = (w - 11 - pw) / 2;
        c.set_clip({0, BAR, w - 11, h - BAR});
        c.fill_alpha(px + 4, BAR + 14 - sc.pos() + 4, pw, doc_h, 0xFF000000, 80);
        c.fill(px, BAR + 14 - sc.pos(), pw, doc_h, 0xFFFFFFFF);
        int y = BAR + 14 + 24 - sc.pos();
        for (int i = 0; i < lines.len; i++) {
            Line& l = lines[i];
            int scale = l.style == 1 ? 2 : 1;
            int lh = line_h(l);
            if (l.n && y + lh >= BAR && y < h) {
                int x = px + 20;
                if (l.bullet_first) c.fill(x + 3, y + 5, 4, 4, th::text);
                if (l.bullet_first || l.bullet_cont) x += 24;
                u32 col = l.style == 1 ? 0xFF1A1428 : l.style == 2 ? 0xFF3A2F80 : l.style == 3 ? 0xFF4A4280 : 0xFF2A2538;
                c.text_n(x, y, l.s, l.n, col, scale, l.style != 0);
                if (l.style == 2) c.hline(x, y + 14, min(l.n * 8, pw - 40), 0xFFD8D0F0);
            }
            y += lh;
            if (l.end && l.n) y += l.style ? 8 : 4;
        }
        c.set_clip({0, 0, w, h});
        sb.draw(c, w - 10, BAR, h - BAR, doc_h + 28, h - BAR, sc.pos());
    }
    void draw_sheet(Canvas& c, int w, int h) {
        Sheet* s = cur_sheet < sheets.len ? sheets[cur_sheet] : nullptr;
        int area_h = h - BAR - (sheets.len > 1 ? TABS : 0);
        c.fill(0, BAR, w, area_h, 0xFFFFFFFF);
        if (!s) return;
        c.set_clip({0, BAR, w - 11, area_h});
        // column headers
        int x = HEADW - hx;
        c.fill(0, BAR, w, 18, th::panel);
        for (int k = 0; k < s->ncols; k++) {
            if (x + col_w[k] >= HEADW && x < w) {
                char hn[4] = {0};
                if (k < 26) { hn[0] = (char)('A' + k); } else { hn[0] = (char)('A' + k / 26 - 1); hn[1] = (char)('A' + k % 26); }
                c.text_centered(x, BAR + 3, col_w[k], hn, th::muted, 1, true);
                c.vline(x + col_w[k] - 1, BAR, 18, th::border);
            }
            x += col_w[k];
        }
        int first = max(0, sc.pos() / ROW), last = min(s->nrows, (sc.pos() + area_h) / ROW + 2);
        for (int r = first; r < last; r++) {
            int y = BAR + 18 + r * ROW - sc.pos();
            if (r % 2) c.fill(HEADW, y, w, ROW, 0xFFF6F3FC);
            int cx = HEADW - hx;
            for (int k = 0; k < s->ncols; k++) {
                char* t = s->cells[r * s->ncols + k];
                if (t && cx + col_w[k] >= HEADW && cx < w) {
                    bool num = (t[0] >= '0' && t[0] <= '9') || t[0] == '-' || t[0] == '.';
                    bool allnum = num;
                    for (const char* q = t; *q && allnum; q++) if (!((*q >= '0' && *q <= '9') || *q == '.' || *q == '-' || *q == ',' || *q == '%' || *q == 'E' || *q == 'e')) allnum = false;
                    int tw = text_width(t);
                    int tx = allnum ? cx + col_w[k] - 8 - min(tw, col_w[k] - 14) : cx + 6;
                    c.text_n(max(tx, cx + 2), y + 3, t, (col_w[k] - 10) / 8, th::text);
                }
                c.vline(cx + col_w[k] - 1, y, ROW, 0xFFE4DEF0);
                cx += col_w[k];
            }
            c.hline(HEADW, y + ROW - 1, w, 0xFFE4DEF0);
        }
        // row numbers, drawn last so they stay put
        c.fill(0, BAR + 18, HEADW, area_h, th::panel);
        for (int r = first; r < last; r++) {
            char n[12];
            snprintf(n, sizeof(n), "%d", r + 1);
            c.text_centered(0, BAR + 18 + r * ROW - sc.pos() + 3, HEADW, n, th::muted);
        }
        c.fill(0, BAR, HEADW, 18, th::panel_dk);
        c.set_clip({0, 0, w, h});
        sb.draw(c, w - 10, BAR, area_h, content_h(), area_h, sc.pos());
        if (sheets.len > 1) {
            c.fill(0, h - TABS, w, TABS, th::panel);
            c.hline(0, h - TABS, w, th::border);
            int tx = 6;
            for (int i = 0; i < sheets.len; i++) {
                int tw2 = (int)strlen(sheets[i]->name) * 8 + 20;
                draw_button(c, tx, h - TABS + 2, tw2, 20, sheets[i]->name, hot(tx, h - TABS + 2, tw2, 20), i == cur_sheet);
                tx += tw2 + 4;
            }
        }
    }

    void save_as_text() {
        if (!file) return;
        Vec<char> out;
        if (is_sheet) {
            for (int si = 0; si < sheets.len; si++) {
                if (si != cur_sheet && sheets.len > 1) continue;
                Sheet* s = sheets[si];
                for (int r = 0; r < s->nrows; r++) {
                    for (int k = 0; k < s->ncols; k++) {
                        if (k) out.push(',');
                        char* t = s->cells[r * s->ncols + k];
                        if (!t) continue;
                        bool q = false;
                        for (const char* p = t; *p; p++) if (*p == ',' || *p == '"') q = true;
                        if (q) out.push('"');
                        for (const char* p = t; *p; p++) { if (*p == '"') out.push('"'); out.push(*p); }
                        if (q) out.push('"');
                    }
                    out.push('\n');
                }
            }
        } else {
            for (int i = 0; i < blocks.len; i++) {
                if (blocks[i].bullet) { out.push('-'); out.push(' '); }
                for (const char* p = blocks[i].text; *p; p++) out.push(*p);
                out.push('\n');
            }
        }
        char* sil = vec_str(out);
        int n = (int)strlen(sil);
        char* u = (char*)kmalloc(n * 4 + 8);
        silicon_to_utf8(sil, n, u, n * 4 + 8);
        char base[FS_NAME_MAX];
        strlcpy(base, file->name, sizeof(base));
        char* dot = strrchr(base, '.');
        if (dot) *dot = 0;
        strlcat(base, is_sheet ? ".csv" : ".txt", sizeof(base));
        char nm[FS_NAME_MAX];
        FsNode* dir = file->parent ? file->parent : fs_home();
        fs_unique_name(dir, base, nm, sizeof(nm));
        FsNode* f = fs_create(dir, nm, false);
        if (f && fs_write(f, u, (u32)strlen(u))) ui_notify("Saved", nm, "docs");
        kfree(u);
        kfree(sil);
        out.free();
    }

    void on_mouse(const MouseEv& e) override {
        int area_h = ch - BAR - (is_sheet && sheets.len > 1 ? TABS : 0);
        int off = sc.target;
        if (file && sb.handle(e, cw - 10, BAR, area_h, content_h() + (is_sheet ? 0 : 28), area_h, off)) { sc.jump(off); ui_invalidate(); return; }
        if (e.kind == M_WHEEL) {
            if (kbd_shift_down() && is_sheet) { hx = max(0, hx - e.wheel * 60); }
            else sc.set(clamp(sc.target - e.wheel * 60, 0, max(0, content_h() + (is_sheet ? 0 : 28) - area_h)));
            ui_invalidate();
            return;
        }
        if (e.kind == M_MOVE) { ui_invalidate(); return; }
        if (e.kind != M_UP || e.button != 1) return;
        if (e.y < BAR) {
            if (Rect{cw - 80, 6, 70, 22}.contains(e.x, e.y))
                pick_file("Open document", "/home", "docx,odt,xlsx,ods,csv,tsv", false, "", [](App* a, const char* p) { a->open_arg(p); });
            else if (file && !err[0] && Rect{cw - 190, 6, 104, 22}.contains(e.x, e.y)) save_as_text();
            return;
        }
        if (is_sheet && sheets.len > 1 && e.y >= ch - TABS) {
            int tx = 6;
            for (int i = 0; i < sheets.len; i++) {
                int tw2 = (int)strlen(sheets[i]->name) * 8 + 20;
                if (Rect{tx, ch - TABS + 2, tw2, 20}.contains(e.x, e.y)) { cur_sheet = i; compute_cols(); ui_invalidate(); return; }
                tx += tw2 + 4;
            }
        }
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down || !file) return;
        int area_h = ch - BAR - (is_sheet && sheets.len > 1 ? TABS : 0);
        int mx = max(0, content_h() + (is_sheet ? 0 : 28) - area_h);
        if (e.key == K_DOWN) sc.set(min(mx, sc.target + 54));
        else if (e.key == K_UP) sc.set(max(0, sc.target - 54));
        else if (e.key == K_PGDN || e.key == ' ') sc.set(min(mx, sc.target + area_h - 40));
        else if (e.key == K_PGUP) sc.set(max(0, sc.target - area_h + 40));
        else if (e.key == K_HOME) sc.set(0);
        else if (e.key == K_END) sc.set(mx);
        else if (e.key == K_LEFT && is_sheet) hx = max(0, hx - 60);
        else if (e.key == K_RIGHT && is_sheet) hx += 60;
        ui_invalidate();
    }
};

App* new_docs(const char* arg) { return new DocsApp(arg); }
