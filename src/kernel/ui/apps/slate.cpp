// silicon - Slate: the text editor. Tabs, undo/redo, find and replace, syntax colours, UTF-8 and emoji,
// system clipboard, line tools, a file picker.
#include "apps.h"
#include "../../text.h"

enum {
    S_NEW = 1, S_OPEN, S_SAVE, S_SAVE_AS, S_CLOSE_TAB, S_CLOSE, S_CUT, S_COPY, S_PASTE, S_SELALL, S_DELLINE,
    S_NUMBERS, S_ZOOM, S_DUPLINE, S_UNDO, S_REDO, S_FIND, S_REPLACE, S_GOTO, S_MOVE_UP, S_MOVE_DOWN, S_COMMENT,
    S_INDENT, S_OUTDENT, S_DARK, S_NEXT_TAB, S_PREV_TAB, S_PREVIEW, S_EMOJI, S_WS, S_LANG = 100,
};

// ================================================================ syntax highlighting
enum Kind : u8 { K_TEXT, K_KEYWORD, K_TYPE, K_STRING, K_NUMBER, K_COMMENT, K_PREPROC, K_FUNC, K_PUNCT, K_HEAD, K_EMPH, K_LINK, K_TAG, K_ATTR };

struct Lang {
    const char* name;
    const char* exts;            // comma separated
    const char* keywords;        // space separated
    const char* types;
    const char* line_comment;    // "" = none
    const char* block_start;
    const char* block_end;
    bool hash_preproc, py_strings, backtick, html, markdown, shell_vars;
};

static const Lang LANGS[] = {
    {"Plain text", "txt,log,cfg,ini,conf", "", "", "", "", "", false, false, false, false, false, false},
    {"C / C++", "c,h,cpp,hpp,cc,cxx,hh,ino",
     "if else for while do switch case default break continue return goto sizeof typedef struct union enum class "
     "public private protected virtual override static const constexpr inline extern namespace using template typename "
     "new delete this nullptr true false try catch throw operator auto volatile register explicit friend final noexcept "
     "asm and or not",
     "int char short long float double void bool unsigned signed size_t u8 u16 u32 u64 i8 i16 i32 i64 uint8_t uint16_t "
     "uint32_t uint64_t int8_t int16_t int32_t int64_t string vector wchar_t",
     "//", "/*", "*/", true, false, false, false, false, false},
    {"Python", "py,pyw",
     "and as assert break class continue def del elif else except finally for from global if import in is lambda nonlocal "
     "not or pass raise return try while with yield True False None async await match case",
     "int str float list dict set tuple bool bytes object self cls print len range", "#", "", "", false, true, false, false, false, false},
    {"JavaScript", "js,mjs,ts,jsx,tsx,json",
     "var let const function return if else for while do switch case default break continue new delete this typeof "
     "instanceof in of class extends super import export from as try catch finally throw async await yield null "
     "undefined true false void static get set interface type enum implements",
     "Array Object String Number Boolean Map Set Promise Date Math JSON console window document", "//", "/*", "*/",
     false, false, true, false, false, false},
    {"HTML / XML", "html,htm,xml,svg,xhtml", "", "", "", "<!--", "-->", false, false, false, true, false, false},
    {"CSS", "css,scss", "important", "color background margin padding border display font width height position top left right bottom",
     "", "/*", "*/", false, false, false, false, false, false},
    {"Markdown", "md,markdown", "", "", "", "", "", false, false, false, false, true, false},
    {"Shell", "sh,bash,zsh,fish,bashrc,zshrc,profile",
     "if then else elif fi for while do done case esac function in return exit export alias set unset local echo cd ls cat "
     "grep source test true false",
     "", "#", "", "", false, false, false, false, false, true},
    {"Assembly", "asm,s,inc", "mov push pop call ret jmp je jne jz jnz cmp add sub mul div inc dec lea int cli sti hlt nop and or xor not shl shr in out",
     "eax ebx ecx edx esi edi esp ebp rax rbx rcx rdx rsi rdi rsp rbp r8 r9 r10 r11 r12 r13 r14 r15 al ah bl cl dl ax bx cx dx",
     ";", "", "", false, false, false, false, false, false},
    {"Rust / Go", "rs,go",
     "fn let mut pub use mod struct enum impl trait match if else for while loop return break continue as in const static "
     "type package import func var go defer chan select range map interface",
     "i8 i16 i32 i64 u8 u16 u32 u64 usize isize f32 f64 bool str String Vec Option Result int string error byte rune",
     "//", "/*", "*/", false, false, false, false, false, false},
};
static const int N_LANGS = (int)(sizeof(LANGS) / sizeof(LANGS[0]));

static bool in_list(const char* list, const char* w, int n) {
    const char* p = list;
    while (*p) {
        const char* e = p;
        while (*e && *e != ' ') e++;
        if ((int)(e - p) == n && !strncmp(p, w, n)) return true;
        p = *e ? e + 1 : e;
    }
    return false;
}

static int lang_for(const char* path) {
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char* dot = strrchr(base, '.');
    const char* ext = dot ? dot + 1 : base;
    if (!*ext) return 0;
    for (int i = 0; i < N_LANGS; i++) {
        const char* p = LANGS[i].exts;
        while (*p) {
            const char* e = strchr(p, ',');
            int n = e ? (int)(e - p) : (int)strlen(p);
            if ((int)strlen(ext) == n) {
                bool same = true;
                for (int k = 0; k < n; k++) if (to_lower(ext[k]) != p[k]) same = false;
                if (same) return i;
            }
            if (!e) break;
            p = e + 1;
        }
    }
    return 0;
}

// states carried from one line to the next
enum { ST_NONE = 0, ST_BLOCK = 1, ST_TRIPLE_D = 2, ST_TRIPLE_S = 3, ST_FENCE = 4 };

static bool is_idc(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// colours the bytes of one line, returns the state at its end
static int highlight(const Lang& L, const char* s, int len, int state, u8* k) {
    len = min(len, 4096);
    for (int i = 0; i < len; i++) k[i] = K_TEXT;
    int i = 0;
    if (L.markdown) {
        if (state == ST_FENCE) {
            for (int j = 0; j < len; j++) k[j] = K_STRING;
            if (len >= 3 && !strncmp(s, "```", 3)) return ST_NONE;
            return ST_FENCE;
        }
        if (len >= 3 && !strncmp(s, "```", 3)) { for (int j = 0; j < len; j++) k[j] = K_STRING; return ST_FENCE; }
        if (len && s[0] == '#') { for (int j = 0; j < len; j++) k[j] = K_HEAD; return ST_NONE; }
        if (len && (s[0] == '>' )) { for (int j = 0; j < len; j++) k[j] = K_COMMENT; return ST_NONE; }
        int st = 0;
        while (st < len && s[st] == ' ') st++;
        if (st < len && (s[st] == '-' || s[st] == '*' || s[st] == '+') && st + 1 < len && s[st + 1] == ' ') k[st] = K_KEYWORD;
        for (; i < len; i++) {
            if (s[i] == '`') { int j = i + 1; while (j < len && s[j] != '`') j++; for (int m = i; m <= min(j, len - 1); m++) k[m] = K_STRING; i = j; }
            else if (s[i] == '*' || s[i] == '_') { int j = i + 1; while (j < len && s[j] != s[i]) j++; if (j < len) { for (int m = i; m <= j; m++) k[m] = K_EMPH; i = j; } }
            else if (s[i] == '[') { int j = i + 1; while (j < len && s[j] != ']') j++; if (j + 1 < len && s[j + 1] == '(') { int e = j + 2; while (e < len && s[e] != ')') e++; for (int m = i; m <= min(e, len - 1); m++) k[m] = K_LINK; i = e; } }
        }
        return ST_NONE;
    }
    if (L.html) {
        bool in_tag = false;
        if (state == ST_BLOCK) {
            while (i < len) {
                k[i] = K_COMMENT;
                if (i >= 2 && !strncmp(s + i - 2, "-->", 3)) { i++; state = ST_NONE; break; }
                i++;
            }
            if (state == ST_BLOCK) return ST_BLOCK;
        }
        for (; i < len; i++) {
            char ch = s[i];
            if (!in_tag) {
                if (ch == '<') {
                    if (i + 3 < len && !strncmp(s + i, "<!--", 4)) {
                        int j = i;
                        bool closed = false;
                        for (; j < len; j++) { k[j] = K_COMMENT; if (j >= 2 && !strncmp(s + j - 2, "-->", 3)) { closed = true; break; } }
                        if (!closed) return ST_BLOCK;
                        i = j;
                        continue;
                    }
                    in_tag = true;
                    k[i] = K_PUNCT;
                    int j = i + 1;
                    if (j < len && (s[j] == '/' || s[j] == '!' || s[j] == '?')) { k[j] = K_PUNCT; j++; }
                    while (j < len && (is_idc(s[j]) || s[j] == '-' || s[j] == ':')) k[j++] = K_TAG;
                    i = j - 1;
                } else if (ch == '&') {
                    int j = i;
                    while (j < len && s[j] != ';' && s[j] != ' ') k[j++] = K_NUMBER;
                    if (j < len && s[j] == ';') k[j] = K_NUMBER;
                    i = j;
                }
            } else {
                if (ch == '>') { k[i] = K_PUNCT; in_tag = false; }
                else if (ch == '"' || ch == '\'') {
                    int j = i + 1;
                    while (j < len && s[j] != ch) j++;
                    for (int m = i; m <= min(j, len - 1); m++) k[m] = K_STRING;
                    i = j;
                } else if (is_idc(ch) || ch == '-' || ch == ':') { int j = i; while (j < len && (is_idc(s[j]) || s[j] == '-' || s[j] == ':')) k[j++] = K_ATTR; i = j - 1; }
                else k[i] = K_PUNCT;
            }
        }
        return ST_NONE;
    }
    int bl_s = (int)strlen(L.block_start), bl_e = (int)strlen(L.block_end), lc = (int)strlen(L.line_comment);
    // continue a block comment / triple-quoted string from the previous line
    if (state == ST_BLOCK) {
        while (i < len) {
            k[i] = K_COMMENT;
            if (bl_e && i + 1 >= bl_e && !strncmp(s + i + 1 - bl_e, L.block_end, bl_e)) { i++; state = ST_NONE; break; }
            i++;
        }
        if (state == ST_BLOCK) return ST_BLOCK;
    } else if (state == ST_TRIPLE_D || state == ST_TRIPLE_S) {
        const char* q = state == ST_TRIPLE_D ? "\"\"\"" : "'''";
        while (i < len) {
            k[i] = K_STRING;
            if (i >= 2 && !strncmp(s + i - 2, q, 3)) { i++; state = ST_NONE; break; }
            i++;
        }
        if (state != ST_NONE) return state;
    }
    // preprocessor line
    if (L.hash_preproc) {
        int st = 0;
        while (st < len && (s[st] == ' ' || s[st] == '\t')) st++;
        if (st < len && s[st] == '#' && i == 0) {
            for (int j = st; j < len; j++) k[j] = K_PREPROC;
            // a trailing comment still reads as a comment
            for (int j = st; j + 1 < len; j++)
                if (s[j] == '/' && s[j + 1] == '/') { for (int m = j; m < len; m++) k[m] = K_COMMENT; break; }
            return ST_NONE;
        }
    }
    for (; i < len;) {
        char ch = s[i];
        if (lc && !strncmp(s + i, L.line_comment, lc)) { for (int j = i; j < len; j++) k[j] = K_COMMENT; return ST_NONE; }
        if (bl_s && !strncmp(s + i, L.block_start, bl_s)) {
            int j = i + bl_s;
            for (int m = i; m < j && m < len; m++) k[m] = K_COMMENT;
            bool closed = false;
            while (j < len) {
                k[j] = K_COMMENT;
                if (j + 1 >= bl_e && !strncmp(s + j + 1 - bl_e, L.block_end, bl_e)) { closed = true; j++; break; }
                j++;
            }
            if (!closed) return ST_BLOCK;
            i = j;
            continue;
        }
        if (L.py_strings && (!strncmp(s + i, "\"\"\"", 3) || !strncmp(s + i, "'''", 3))) {
            const char* q = s[i] == '"' ? "\"\"\"" : "'''";
            int j = i + 3;
            for (int m = i; m < j && m < len; m++) k[m] = K_STRING;
            bool closed = false;
            while (j < len) {
                k[j] = K_STRING;
                if (j >= 2 && !strncmp(s + j - 2, q, 3) && j >= i + 5) { closed = true; j++; break; }
                j++;
            }
            if (!closed) return s[i] == '"' ? ST_TRIPLE_D : ST_TRIPLE_S;
            i = j;
            continue;
        }
        if (ch == '"' || ch == '\'' || (L.backtick && ch == '`')) {
            int j = i + 1;
            while (j < len && s[j] != ch) { if (s[j] == '\\' && j + 1 < len) j++; j++; }
            for (int m = i; m <= min(j, len - 1); m++) k[m] = K_STRING;
            i = j + 1;
            continue;
        }
        if (L.shell_vars && ch == '$') {
            int j = i + 1;
            if (j < len && s[j] == '{') { while (j < len && s[j] != '}') j++; }
            else while (j < len && is_idc(s[j])) j++;
            for (int m = i; m <= min(j, len - 1); m++) k[m] = K_TYPE;
            i = j + 1;
            continue;
        }
        if ((ch >= '0' && ch <= '9') || (ch == '.' && i + 1 < len && s[i + 1] >= '0' && s[i + 1] <= '9')) {
            int j = i;
            while (j < len && (is_idc(s[j]) || s[j] == '.')) j++;
            for (int m = i; m < j; m++) k[m] = K_NUMBER;
            i = j;
            continue;
        }
        if (is_idc(ch)) {
            int j = i;
            while (j < len && is_idc(s[j])) j++;
            int n = j - i;
            Kind kind = K_TEXT;
            if (in_list(L.keywords, s + i, n)) kind = K_KEYWORD;
            else if (in_list(L.types, s + i, n)) kind = K_TYPE;
            else if (j < len && s[j] == '(') kind = K_FUNC;
            for (int m = i; m < j; m++) k[m] = kind;
            i = j;
            continue;
        }
        if (ch == '{' || ch == '}' || ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == ';' || ch == ',' || ch == '=' ||
            ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '<' || ch == '>' || ch == '!' || ch == '&' || ch == '|' || ch == ':')
            k[i] = K_PUNCT;
        i++;
    }
    return ST_NONE;
}

// ================================================================ document
struct Op {
    int row, col;                // where the change starts
    char* removed;
    char* inserted;
    int cr, cc;                  // cursor before
    int nr, nc;                  // cursor after
    int group;
    u64 t;
};

struct Doc {
    Vec<char*> lines;
    int row = 0, col = 0, arow = -1, acol = 0;
    int goal = -1;                         // wished display column when moving up/down
    int left = 0;                          // first visible display column
    Smooth sc;
    char path[200] = "";
    bool modified = false;
    int lang = 0;
    Vec<Op> undo, redo;
    int group = 1;
    Vec<u8> states;                        // state at the start of each line (valid up to states_ok)
    int states_ok = 0;
    char title[48] = "untitled";

    Doc() { lines.push(strdup("")); }
    ~Doc() {
        for (int i = 0; i < lines.len; i++) kfree(lines[i]);
        lines.free();
        for (int i = 0; i < undo.len; i++) { kfree(undo[i].removed); kfree(undo[i].inserted); }
        for (int i = 0; i < redo.len; i++) { kfree(redo[i].removed); kfree(redo[i].inserted); }
        undo.free(); redo.free(); states.free();
    }
    void clear_lines() { for (int i = 0; i < lines.len; i++) kfree(lines[i]); lines.clear(); }

    bool has_sel() const { return arow >= 0 && (arow != row || acol != col); }
    void sel_range(int& r0, int& c0, int& r1, int& c1) const {
        if (arow < row || (arow == row && acol < col)) { r0 = arow; c0 = acol; r1 = row; c1 = col; }
        else { r0 = row; c0 = col; r1 = arow; c1 = acol; }
    }

    // text between two positions, newlines between lines; caller frees
    char* get(int r0, int c0, int r1, int c1) const {
        u32 total = 1;
        for (int r = r0; r <= r1; r++) total += (u32)strlen(lines[r]) + 1;
        char* out = (char*)kmalloc(total);
        u32 o = 0;
        for (int r = r0; r <= r1; r++) {
            const char* l = lines[r];
            int a = r == r0 ? c0 : 0, b = r == r1 ? c1 : (int)strlen(l);
            if (b < a) b = a;
            memcpy(out + o, l + a, b - a);
            o += b - a;
            if (r != r1) out[o++] = '\n';
        }
        out[o] = 0;
        return out;
    }

    void invalidate(int from_row) { states_ok = min(states_ok, from_row); }

    // the one mutation: replace [r0,c0)-(r1,c1) with ins (may contain newlines)
    void apply(int r0, int c0, int r1, int c1, const char* ins) {
        const char* first = lines[r0];
        const char* last = lines[r1];
        int tail_len = (int)strlen(last + c1), ins_len = (int)strlen(ins);
        // split the insert into lines
        Vec<char*> parts;
        const char* p = ins;
        for (;;) {
            const char* e = strchr(p, '\n');
            int l = e ? (int)(e - p) : (int)strlen(p);
            char* part = (char*)kmalloc(l + 1);
            memcpy(part, p, l);
            part[l] = 0;
            parts.push(part);
            if (!e) break;
            p = e + 1;
        }
        (void)ins_len;
        // compose: first[:c0] + parts[0], parts[1..], parts[last] + last[c1:]
        int np = parts.len;
        char* head = (char*)kmalloc(c0 + strlen(parts[0]) + (np == 1 ? tail_len : 0) + 1);
        memcpy(head, first, c0);
        strcpy(head + c0, parts[0]);
        if (np == 1) strcpy(head + c0 + strlen(parts[0]), last + c1);
        char* tail = nullptr;
        if (np > 1) {
            tail = (char*)kmalloc(strlen(parts[np - 1]) + tail_len + 1);
            strcpy(tail, parts[np - 1]);
            strcpy(tail + strlen(parts[np - 1]), last + c1);
        }
        for (int r = r0; r <= r1; r++) kfree(lines[r]);
        for (int r = r1; r > r0; r--) lines.remove(r);
        lines[r0] = head;
        for (int i = 1; i < np; i++) lines.insert(r0 + i, i == np - 1 ? tail : parts[i]);
        kfree(parts[0]);
        if (np > 1) kfree(parts[np - 1]);
        parts.free();
        invalidate(r0);
        modified = true;
    }

    static void end_of(int row, int col, const char* text, int& er, int& ec) {
        er = row;
        ec = col;
        for (const char* t = text; *t; t++) {
            if (*t == '\n') { er++; ec = 0; }
            else ec++;
        }
    }

    // edit with undo bookkeeping. cursor goes to the end of the inserted text.
    void edit(int r0, int c0, int r1, int c1, const char* ins, int grp = 0, bool merge = false) {
        char* removed = get(r0, c0, r1, c1);
        Op op;
        op.row = r0; op.col = c0;
        op.removed = removed;
        op.inserted = strdup(ins);
        op.cr = row; op.cc = col;
        apply(r0, c0, r1, c1, ins);
        int er, ec;
        end_of(r0, c0, ins, er, ec);
        row = er;
        col = ec;
        arow = -1;
        op.nr = row; op.nc = col;
        op.group = grp ? grp : ++group;
        op.t = ticks();
        for (int i = 0; i < redo.len; i++) { kfree(redo[i].removed); kfree(redo[i].inserted); }
        redo.clear();
        // merge a run of typed characters into one undo step
        if (merge && undo.len && !grp) {
            Op& last = undo.back();
            int lr, lc;
            end_of(last.row, last.col, last.inserted, lr, lc);
            if (!last.removed[0] && !removed[0] && lr == r0 && lc == c0 && op.t - last.t < 1200 && strlen(last.inserted) < 60 && !strchr(ins, '\n')) {
                u32 n = (u32)strlen(last.inserted), m = (u32)strlen(ins);
                char* ni = (char*)kmalloc(n + m + 1);
                memcpy(ni, last.inserted, n);
                memcpy(ni + n, ins, m + 1);
                kfree(last.inserted);
                last.inserted = ni;
                last.nr = row; last.nc = col;
                last.t = op.t;
                kfree(op.removed); kfree(op.inserted);
                return;
            }
        }
        undo.push(op);
        if (undo.len > 3000) { kfree(undo[0].removed); kfree(undo[0].inserted); undo.remove(0); }
    }

    void do_undo() {
        if (!undo.len) return;
        int g = undo.back().group;
        while (undo.len && undo.back().group == g) {
            Op op = undo.back();
            undo.len--;
            int er, ec;
            end_of(op.row, op.col, op.inserted, er, ec);
            apply(op.row, op.col, er, ec, op.removed);
            row = op.cr; col = op.cc; arow = -1;
            redo.push(op);
        }
    }
    void do_redo() {
        if (!redo.len) return;
        int g = redo.back().group;
        while (redo.len && redo.back().group == g) {
            Op op = redo.back();
            redo.len--;
            int er, ec;
            end_of(op.row, op.col, op.removed, er, ec);
            apply(op.row, op.col, er, ec, op.inserted);
            row = op.nr; col = op.nc; arow = -1;
            undo.push(op);
        }
    }

    int state_at(int line) {
        const Lang& L = LANGS[lang];
        if (!L.block_start[0] && !L.py_strings && !L.markdown) return ST_NONE;
        if (states.len < lines.len + 1) { while (states.len < lines.len + 1) states.push(0); }
        static u8 tmp[4100];
        while (states_ok < line) {
            int s = states[states_ok];
            const char* l = lines[states_ok];
            states[states_ok + 1] = (u8)highlight(L, l, (int)strlen(l), s, tmp);
            states_ok++;
        }
        return states[line];
    }
};

// ================================================================ utf-8 helpers on a line
static int next_col(const char* l, int col) {
    int n = (int)strlen(l);
    if (col >= n) return n;
    return col + utf8_len_at(l + col, l + n);
}
static int prev_col(const char* l, int col) {
    if (col <= 0) return 0;
    col--;
    while (col > 0 && ((u8)l[col] & 0xC0) == 0x80) col--;
    return col;
}
static u32 cp_at(const char* l, int col) {
    const char* p = l + col;
    return utf8_decode(p, l + strlen(l));
}
static int cp_width(u32 cp, int dcol) {
    if (cp == '\t') return 4 - dcol % 4;
    char g[4];
    return glyph_for(cp, g);
}
static int disp_col(const char* l, int upto) {
    int d = 0;
    const char* p = l;
    const char* end = l + upto;
    const char* all = l + strlen(l);
    while (p < end) { u32 cp = utf8_decode(p, all); d += cp_width(cp, d); }
    return d;
}
static int byte_at_disp(const char* l, int target) {
    int d = 0;
    const char* p = l;
    const char* all = l + strlen(l);
    while (p < all) {
        const char* q = p;
        u32 cp = utf8_decode(q, all);
        int w = cp_width(cp, d);
        if (d + w / 2 >= target && d + w > target - 0) { if (target - d < w - target + d) return (int)(p - l); }
        if (d + w > target) return target - d < (w + 1) / 2 ? (int)(p - l) : (int)(q - l);
        d += w;
        p = q;
    }
    return (int)strlen(l);
}
static bool is_word(u32 cp) { return cp >= 0x80 || is_idc((char)cp); }

// ================================================================ the app
static const int TAB_H = 26, FIND_H = 30, STATUS_H = 20;

struct SlateApp : App {
    Vec<Doc*> docs;
    int cur = 0;
    bool numbers = true, dark = false, whitespace = false;
    int scale = 1;
    bool mouse_sel = false;
    char status[64] = "";
    u64 status_t = 0;
    int cw = 0, ch = 0;
    ScrollBar sb;
    // find / replace
    bool find_on = false, replace_on = false, match_case = false;
    TextField find_f, repl_f;
    int find_focus = 0;                      // 0 find, 1 replace
    int match_count = 0;
    char tbuf[96];
    int click_count = 0;
    u64 click_t = 0;
    int tab_scroll = 0;

    SlateApp(const char* p) {
        docs.push(new Doc());
        if (p && p[0]) load_into(docs[0], p);
    }
    ~SlateApp() { for (int i = 0; i < docs.len; i++) delete docs[i]; docs.free(); }

    Doc* d() { return docs[cur]; }
    const char* app_id() override { return "slate"; }
    const char* title() override {
        snprintf(tbuf, sizeof(tbuf), "Slate " G_BULLET " %s%s", d()->title, d()->modified ? " " G_DIAM : "");
        return tbuf;
    }
    int pref_w() override { return 760; }
    int pref_h() override { return 500; }
    void on_resize(int w, int h) override { cw = w; ch = h; }
    bool wants_ibeam(int x, int y) override { return y > TAB_H + (find_on ? FIND_H * (replace_on ? 2 : 1) : 0) && y < ch - STATUS_H && x < cw - 10; }
    u32 background() override { return dark ? 0xFF1B1727 : 0xFFFBF9FF; }

    // ------------------------------------------------------------ colours
    u32 c_bg() { return dark ? 0xFF1B1727 : 0xFFFBF9FF; }
    u32 c_text() { return dark ? 0xFFE6E1F5 : 0xFF221C30; }
    u32 c_gutter() { return dark ? 0xFF231E33 : 0xFFEDE9F6; }
    u32 c_gutter_text() { return dark ? 0xFF6B6488 : 0xFFB0A6C8; }
    u32 c_cur_line() { return dark ? 0xFF262038 : 0xFFF3EEFF; }
    u32 c_sel() { return blend(c_bg(), accent(), dark ? 110 : 120); }
    u32 kind_color(u8 k) {
        static const u32 light[] = {0, 0xFF7C3AED, 0xFF0E7490, 0xFFB45309, 0xFF0F766E, 0xFF8A94A6, 0xFF9333EA, 0xFF1D4ED8,
                                    0xFF64748B, 0xFFB91C1C, 0xFF9D174D, 0xFF2563EB, 0xFFBE123C, 0xFF0369A1};
        static const u32 dk[] = {0, 0xFFC4B5FD, 0xFF67E8F9, 0xFFFCD34D, 0xFF86EFAC, 0xFF7C8AA5, 0xFFF0ABFC, 0xFF93C5FD,
                                 0xFF94A3B8, 0xFFFCA5A5, 0xFFF9A8D4, 0xFF7DD3FC, 0xFFFDA4AF, 0xFF7DD3FC};
        return k == K_TEXT ? c_text() : (dark ? dk[k] : light[k]);
    }

    // ------------------------------------------------------------ file i/o
    void retitle_doc(Doc* dc) {
        const char* s = strrchr(dc->path, '/');
        strlcpy(dc->title, dc->path[0] ? (s ? s + 1 : dc->path) : "untitled", sizeof(dc->title));
    }

    bool load_into(Doc* dc, const char* p) {
        FsNode* f = fs_lookup(p);
        if (!f || f->dir) return false;
        dc->clear_lines();
        const char* t = fs_read_text(f);
        for (;;) {
            const char* e = strchr(t, '\n');
            int l = e ? (int)(e - t) : (int)strlen(t);
            int keep = l;
            if (keep && t[keep - 1] == '\r') keep--;
            char* ln = (char*)kmalloc(keep + 1);
            memcpy(ln, t, keep);
            ln[keep] = 0;
            dc->lines.push(ln);
            if (!e) break;
            t = e + 1;
        }
        if (dc->lines.len > 1 && !dc->lines.back()[0]) { kfree(dc->lines.back()); dc->lines.len--; }
        fs_path(f, dc->path, sizeof(dc->path));
        dc->lang = lang_for(dc->path);
        dc->row = dc->col = dc->left = 0;
        dc->arow = -1;
        dc->modified = false;
        dc->states_ok = 0;
        dc->sc.jump(0);
        for (int i = 0; i < dc->undo.len; i++) { kfree(dc->undo[i].removed); kfree(dc->undo[i].inserted); }
        dc->undo.clear();
        for (int i = 0; i < dc->redo.len; i++) { kfree(dc->redo[i].removed); kfree(dc->redo[i].inserted); }
        dc->redo.clear();
        retitle_doc(dc);
        set_title(title());
        return true;
    }

    bool save_doc(Doc* dc, const char* p) {
        FsNode* f = fs_lookup(p);
        if (!f) f = fs_create_path(p, false);
        if (!f || f->dir) { info("can't save there. check the folder exists."); return false; }
        u32 total = 0;
        for (int i = 0; i < dc->lines.len; i++) total += (u32)strlen(dc->lines[i]) + 1;
        char* buf = (char*)kmalloc(total + 1);
        u32 o = 0;
        for (int i = 0; i < dc->lines.len; i++) {
            u32 l = (u32)strlen(dc->lines[i]);
            memcpy(buf + o, dc->lines[i], l);
            o += l;
            buf[o++] = '\n';
        }
        fs_write(f, buf, o);
        kfree(buf);
        fs_path(f, dc->path, sizeof(dc->path));
        dc->lang = lang_for(dc->path);
        dc->states_ok = 0;
        dc->modified = false;
        retitle_doc(dc);
        say("saved %u bytes", o);
        set_title(title());
        return true;
    }

    void say(const char* fmt, u32 n) { snprintf(status, sizeof(status), fmt, n); status_t = ticks(); }
    void open_path_tab(const char* p) {
        FsNode* f = fs_lookup(p);
        if (!f || f->dir) return;
        char full[200];
        fs_path(f, full, sizeof(full));
        for (int i = 0; i < docs.len; i++) if (!strcmp(docs[i]->path, full)) { cur = i; set_title(title()); ui_invalidate(); return; }
        Doc* dc = docs[cur];
        bool reuse = docs.len == 1 && !dc->path[0] && !dc->modified && dc->lines.len == 1 && !dc->lines[0][0];
        if (!reuse) { dc = new Doc(); docs.push(dc); cur = docs.len - 1; }
        load_into(dc, full);
        ui_invalidate();
    }
    void open_arg(const char* a) override { open_path_tab(a); }

    // ------------------------------------------------------------ geometry
    int lh() { return 15 * scale; }
    int cwid() { return 8 * scale; }
    int gutter() {
        if (!numbers) return 10;
        int digits = 1;
        for (int n = d()->lines.len; n >= 10; n /= 10) digits++;
        return max(3, digits) * 8 + 18;
    }
    int top_y() { return TAB_H + (find_on ? FIND_H * (replace_on ? 2 : 1) : 0); }
    int view_h() { return ch - top_y() - STATUS_H; }
    int vis_rows() { return max(1, view_h() / lh()); }
    int vis_cols() { return max(1, (cw - gutter() - 14) / cwid()); }
    int content_h() { return (d()->lines.len + vis_rows() - 2) * lh(); }

    void ensure_visible() {
        Doc* dc = d();
        int y = dc->row * lh();
        int cur_y = dc->sc.target;
        if (y < cur_y) dc->sc.set(y);
        else if (y + lh() > cur_y + view_h()) dc->sc.set(y + lh() - view_h());
        int dcn = disp_col(dc->lines[dc->row], dc->col);
        if (dcn < dc->left) dc->left = dcn;
        if (dcn >= dc->left + vis_cols() - 1) dc->left = dcn - vis_cols() + 2;
    }

    // ------------------------------------------------------------ editing commands
    void insert_text(const char* t, bool merge = false) {
        Doc* dc = d();
        if (dc->has_sel()) {
            int r0, c0, r1, c1;
            dc->sel_range(r0, c0, r1, c1);
            dc->edit(r0, c0, r1, c1, t);
        } else dc->edit(dc->row, dc->col, dc->row, dc->col, t, 0, merge);
        dc->goal = -1;
    }

    void paste() {
        if (!clip_has()) return;
        const char* src = clip_get();
        // keep it utf-8 as it is, only drop CR
        u32 n = clip_len();
        char* buf = (char*)kmalloc(n + 1);
        u32 o = 0;
        for (u32 i = 0; i < n; i++) if (src[i] != '\r') buf[o++] = src[i];
        buf[o] = 0;
        insert_text(buf);
        kfree(buf);
    }
    void copy_sel(bool cut) {
        Doc* dc = d();
        if (!dc->has_sel()) {
            // like most editors: nothing selected -> the whole line
            char* t = (char*)kmalloc(strlen(dc->lines[dc->row]) + 2);
            strcpy(t, dc->lines[dc->row]);
            { size_t tl = strlen(t); t[tl] = 10; t[tl + 1] = 0; }
            clip_set(t);
            kfree(t);
            if (cut) on_command(S_DELLINE);
            return;
        }
        int r0, c0, r1, c1;
        dc->sel_range(r0, c0, r1, c1);
        char* t = dc->get(r0, c0, r1, c1);
        clip_set(t);
        kfree(t);
        if (cut) insert_text("");
    }

    void newline() {
        Doc* dc = d();
        if (dc->has_sel()) insert_text("");
        const char* l = dc->lines[dc->row];
        int indent = 0;
        while (l[indent] == ' ' && indent < dc->col) indent++;
        char prev = dc->col > 0 ? l[dc->col - 1] : 0, nextc = l[dc->col];
        bool open_block = prev == '{' || prev == '(' || prev == '[' || (prev == ':' && dc->lang == 2);
        char buf[200];
        int n = 0;
        buf[n++] = '\n';
        for (int i = 0; i < indent && n < 150; i++) buf[n++] = ' ';
        if (open_block) for (int i = 0; i < 4; i++) buf[n++] = ' ';
        int caret_back = 0;
        if (open_block && (nextc == '}' || nextc == ')' || nextc == ']')) {
            buf[n++] = '\n';
            for (int i = 0; i < indent && n < 190; i++) buf[n++] = ' ';
            caret_back = indent + 1;
        }
        buf[n] = 0;
        dc->edit(dc->row, dc->col, dc->row, dc->col, buf);
        if (caret_back) { dc->row--; dc->col = (int)strlen(dc->lines[dc->row]); }
    }

    void move_to(int r, int c, bool extend) {
        Doc* dc = d();
        if (extend) { if (dc->arow < 0) { dc->arow = dc->row; dc->acol = dc->col; } }
        else dc->arow = -1;
        dc->row = clamp(r, 0, dc->lines.len - 1);
        dc->col = clamp(c, 0, (int)strlen(dc->lines[dc->row]));
    }

    void selected_rows(int& r0, int& r1) {
        Doc* dc = d();
        if (dc->has_sel()) {
            int a, b, c2, e2;
            dc->sel_range(a, b, c2, e2);
            r0 = a; r1 = e2;
            if (c2 == 0 && e2 > a) r1 = e2 - 1;
            if (r1 < r0) r1 = r0;
        } else r0 = r1 = dc->row;
    }

    void indent_rows(bool out) {
        Doc* dc = d();
        int r0, r1;
        selected_rows(r0, r1);
        int g = ++dc->group;
        int sr = dc->row, sc = dc->col;
        for (int r = r0; r <= r1; r++) {
            const char* l = dc->lines[r];
            if (!out) { dc->row = r; dc->col = 0; dc->edit(r, 0, r, 0, "    ", g); }
            else {
                int n = 0;
                while (n < 4 && l[n] == ' ') n++;
                if (!n && l[0] == '\t') n = 1;
                if (n) { dc->row = r; dc->col = n; dc->edit(r, 0, r, n, "", g); }
            }
        }
        dc->row = sr; dc->col = clamp(sc + (out ? 0 : 4), 0, (int)strlen(dc->lines[sr]));
        if (dc->arow >= 0) dc->arow = -1;
    }

    void toggle_comment() {
        Doc* dc = d();
        const Lang& L = LANGS[dc->lang];
        const char* lc = L.line_comment;
        if (!lc[0]) { if (L.block_start[0]) lc = nullptr; else return; }
        int r0, r1;
        selected_rows(r0, r1);
        int g = ++dc->group;
        char marker[8];
        if (lc) { snprintf(marker, sizeof(marker), "%s ", lc); }
        bool all = true;
        for (int r = r0; r <= r1; r++) {
            const char* l = dc->lines[r];
            int i = 0;
            while (l[i] == ' ') i++;
            if (!l[i]) continue;
            if (lc ? strncmp(l + i, lc, strlen(lc)) : strncmp(l + i, L.block_start, strlen(L.block_start))) all = false;
        }
        for (int r = r0; r <= r1; r++) {
            const char* l = dc->lines[r];
            int i = 0;
            while (l[i] == ' ') i++;
            if (!l[i]) continue;
            dc->row = r;
            dc->col = i;
            if (lc) {
                if (all) { int n = (int)strlen(lc); if (l[i + n] == ' ') n++; dc->edit(r, i, r, i + n, "", g); }
                else dc->edit(r, i, r, i, marker, g);
            } else {
                int n = (int)strlen(L.block_start), m = (int)strlen(L.block_end);
                int len = (int)strlen(l);
                if (all) {
                    if (len >= m && !strcmp(l + len - m, L.block_end)) dc->edit(r, len - m, r, len, "", g);
                    dc->edit(r, i, r, i + n, "", g);
                } else {
                    char a[16], b[16];
                    snprintf(a, sizeof(a), "%s ", L.block_start);
                    snprintf(b, sizeof(b), " %s", L.block_end);
                    dc->edit(r, len, r, len, b, g);
                    dc->edit(r, i, r, i, a, g);
                }
            }
        }
        dc->arow = -1;
        dc->row = clamp(dc->row, 0, dc->lines.len - 1);
        dc->col = clamp(dc->col, 0, (int)strlen(dc->lines[dc->row]));
    }

    void move_lines(int dir) {
        Doc* dc = d();
        int r0, r1;
        selected_rows(r0, r1);
        if ((dir < 0 && r0 == 0) || (dir > 0 && r1 >= dc->lines.len - 1)) return;
        int g = ++dc->group;
        // swap with the neighbouring line by deleting it and re-inserting on the other side
        int nb = dir < 0 ? r0 - 1 : r1 + 1;
        char* moved = strdup(dc->lines[nb]);
        int pr = dc->row, pc = dc->col;
        if (dir < 0) {
            // remove line nb (with its newline) and add it after r1
            dc->row = r0; dc->col = 0;
            dc->edit(nb, 0, nb + 1, 0, "", g);                           // lines shift up by one
            int endr = r1 - 1;
            int endc = (int)strlen(dc->lines[endr]);
            dc->edit(endr, endc, endr, endc, "\n", g);
            char* txt = moved;
            dc->edit(endr + 1, 0, endr + 1, 0, txt, g);
        } else {
            int endc = (int)strlen(dc->lines[nb]);
            dc->edit(nb, 0, nb, endc, "", g);                            // blank the neighbour...
            dc->edit(nb, 0, nb + 1, 0, "", g);                           // ...and drop its (now empty) line
            dc->edit(r0, 0, r0, 0, "", g);
            dc->edit(r0, 0, r0, 0, moved, g);
            dc->edit(r0, (int)strlen(moved), r0, (int)strlen(moved), "\n", g);
        }
        kfree(moved);
        dc->row = clamp(pr + dir, 0, dc->lines.len - 1);
        dc->col = clamp(pc, 0, (int)strlen(dc->lines[dc->row]));
        dc->arow = -1;
    }

    // ------------------------------------------------------------ find / replace
    static bool match_at(const char* s, const char* needle, int n, bool mc) {
        for (int i = 0; i < n; i++) {
            char a = s[i], b = needle[i];
            if (!a) return false;
            if (!mc) { a = to_lower(a); b = to_lower(b); }
            if (a != b) return false;
        }
        return true;
    }
    int count_matches() {
        int n = (int)strlen(find_f.text);
        if (!n) return 0;
        int total = 0;
        for (int r = 0; r < d()->lines.len && total < 9999; r++) {
            const char* l = d()->lines[r];
            for (int i = 0; l[i]; i++) if (match_at(l + i, find_f.text, n, match_case)) { total++; i += n - 1; }
        }
        return total;
    }
    bool find_next(bool forward, bool from_sel_start = false) {
        Doc* dc = d();
        int n = (int)strlen(find_f.text);
        if (!n) return false;
        int rows = dc->lines.len;
        int r = dc->row, c = dc->col;
        if (dc->has_sel()) {
            int r0, c0, r1, c1;
            dc->sel_range(r0, c0, r1, c1);
            if (forward) { r = r1; c = c1; } else { r = r0; c = c0; }
            if (from_sel_start) { r = r0; c = c0; }
        }
        for (int step = 0; step <= rows; step++) {
            int rr = ((forward ? r + step : r - step) % rows + rows) % rows;
            const char* l = dc->lines[rr];
            int len = (int)strlen(l);
            if (forward) {
                int from = step == 0 ? c : 0;
                for (int i = from; i + n <= len; i++)
                    if (match_at(l + i, find_f.text, n, match_case)) { select_match(rr, i, n); return true; }
            } else {
                int from = step == 0 ? c - n : len - n;
                for (int i = from; i >= 0; i--)
                    if (match_at(l + i, find_f.text, n, match_case)) { select_match(rr, i, n); return true; }
            }
        }
        return false;
    }
    void select_match(int r, int c, int n) {
        Doc* dc = d();
        dc->arow = r; dc->acol = c; dc->row = r; dc->col = c + n;
        ensure_visible();
    }
    void replace_one() {
        Doc* dc = d();
        int n = (int)strlen(find_f.text);
        if (!n) return;
        if (dc->has_sel()) {
            int r0, c0, r1, c1;
            dc->sel_range(r0, c0, r1, c1);
            if (r0 == r1 && c1 - c0 == n && match_at(dc->lines[r0] + c0, find_f.text, n, match_case)) {
                dc->edit(r0, c0, r1, c1, repl_f.text);
                match_count = count_matches();
            }
        }
        find_next(true);
    }
    void replace_all() {
        Doc* dc = d();
        int n = (int)strlen(find_f.text), rn = (int)strlen(repl_f.text);
        if (!n) return;
        int g = ++dc->group, done = 0;
        for (int r = 0; r < dc->lines.len; r++) {
            for (int i = 0; dc->lines[r][i];) {
                if (match_at(dc->lines[r] + i, find_f.text, n, match_case)) {
                    dc->edit(r, i, r, i + n, repl_f.text, g);
                    i += rn;
                    done++;
                } else i++;
            }
        }
        dc->arow = -1;
        match_count = count_matches();
        say("replaced %u matches", (u32)done);
    }
    void open_find(bool replace) {
        find_on = true;
        replace_on = replace;
        find_focus = 0;
        Doc* dc = d();
        if (dc->has_sel()) {
            int r0, c0, r1, c1;
            dc->sel_range(r0, c0, r1, c1);
            if (r0 == r1 && c1 - c0 < 100) {
                char* t = dc->get(r0, c0, r1, c1);
                find_f.set(t);
                kfree(t);
            }
        }
        find_f.select_all();
        match_count = count_matches();
        ensure_visible();
    }

    // ------------------------------------------------------------ bracket matching
    bool match_bracket(int& mr, int& mc, int& br, int& bc) {
        Doc* dc = d();
        const char* l = dc->lines[dc->row];
        int pos = -1;
        if (dc->col < (int)strlen(l) && strchr("()[]{}", l[dc->col]) && l[dc->col]) pos = dc->col;
        else if (dc->col > 0 && strchr("()[]{}", l[dc->col - 1])) pos = dc->col - 1;
        if (pos < 0) return false;
        char ch = l[pos];
        static const char open[] = "([{", close[] = ")]}";
        int dir = 0;
        char other = 0;
        for (int i = 0; i < 3; i++) {
            if (ch == open[i]) { dir = 1; other = close[i]; }
            if (ch == close[i]) { dir = -1; other = open[i]; }
        }
        int depth = 0, r = dc->row, c = pos, steps = 0;
        br = dc->row; bc = pos;
        for (;;) {
            if (++steps > 40000) return false;
            const char* cl = dc->lines[r];
            c += dir;
            while (c < 0 || c >= (int)strlen(cl)) {
                r += dir;
                if (r < 0 || r >= dc->lines.len) return false;
                cl = dc->lines[r];
                c = dir > 0 ? 0 : (int)strlen(cl) - 1;
                if (strlen(cl) == 0) { c = dir > 0 ? -1 : 0; if (dir < 0) continue; }
                if (strlen(cl) == 0) continue;
                break;
            }
            if (c < 0 || c >= (int)strlen(cl)) continue;
            if (cl[c] == ch) depth++;
            else if (cl[c] == other) { if (!depth) { mr = r; mc = c; return true; } depth--; }
        }
    }

    // ------------------------------------------------------------ drawing
    void draw_tabs(Canvas& c, int w) {
        c.fill(0, 0, w, TAB_H, th::panel_dk);
        c.hline(0, TAB_H - 1, w, th::border);
        int x = 4 - tab_scroll;
        for (int i = 0; i < docs.len; i++) {
            Doc* dc = docs[i];
            char nm[48];
            snprintf(nm, sizeof(nm), "%s%s", dc->title, dc->modified ? " " G_DIAM : "");
            int tw = min(170, (int)strlen(nm) * 8 + 40);
            bool on = i == cur;
            bool hv = hot(x, 2, tw, TAB_H - 2);
            c.fill(x, on ? 2 : 4, tw, TAB_H - (on ? 2 : 5), on ? th::paper : (hv ? blend(th::panel_dk, th::white, 70) : th::panel));
            c.hline(x, on ? 2 : 4, tw, on ? accent() : th::border);
            c.text_n(x + 8, 8, nm, (tw - 34) / 8, on ? th::text : th::muted, 1, on);
            bool xhot = hot(x + tw - 18, 6, 14, 14);
            c.text(x + tw - 15, 8, G_TIMES, xhot ? th::red : th::muted, 1, true);
            x += tw + 2;
        }
        bool nh = hot(x + 2, 4, 22, 20);
        c.text(x + 8, 8, "+", nh ? accent() : th::muted, 1, true);
    }
    // x-range of a tab (mirrors draw_tabs)
    bool tab_rect(int i, int& x, int& tw) {
        int px = 4 - tab_scroll;
        for (int k = 0; k < docs.len; k++) {
            char nm[48];
            snprintf(nm, sizeof(nm), "%s%s", docs[k]->title, docs[k]->modified ? " " G_DIAM : "");
            int t = min(170, (int)strlen(nm) * 8 + 40);
            if (k == i) { x = px; tw = t; return true; }
            px += t + 2;
        }
        x = px;
        tw = 22;
        return false;
    }

    void draw_find(Canvas& c, int w) {
        int y = TAB_H;
        c.fill(0, y, w, FIND_H * (replace_on ? 2 : 1), th::panel);
        c.hline(0, y + FIND_H * (replace_on ? 2 : 1) - 1, w, th::border);
        c.text(10, y + 9, "find", th::muted, 1, true);
        find_f.focused = find_focus == 0;
        find_f.draw(c, 56, y + 5, 220);
        draw_button(c, 284, y + 4, 24, 22, G_UP, hot(284, y + 4, 24, 22));
        draw_button(c, 312, y + 4, 24, 22, G_DOWN, hot(312, y + 4, 24, 22));
        draw_button(c, 342, y + 4, 34, 22, "Aa", hot(342, y + 4, 34, 22), match_case);
        char b[40];
        snprintf(b, sizeof(b), "%d match%s", match_count, match_count == 1 ? "" : "es");
        c.text(386, y + 9, b, match_count || !find_f.len ? th::muted : th::red);
        draw_button(c, w - 28, y + 4, 22, 22, G_TIMES, hot(w - 28, y + 4, 22, 22));
        if (replace_on) {
            c.text(10, y + FIND_H + 9, "with", th::muted, 1, true);
            repl_f.focused = find_focus == 1;
            repl_f.draw(c, 56, y + FIND_H + 5, 220);
            draw_button(c, 284, y + FIND_H + 4, 64, 22, "Replace", hot(284, y + FIND_H + 4, 64, 22));
            draw_button(c, 354, y + FIND_H + 4, 40, 22, "All", hot(354, y + FIND_H + 4, 40, 22));
        }
    }

    void draw_line(Canvas& c, Doc* dc, int li, int y, int w, int mr, int mc, int br, int bc) {
        const char* l = dc->lines[li];
        int len = (int)strlen(l);
        static u8 kinds[4100];
        int state = dc->state_at(li);
        highlight(LANGS[dc->lang], l, len, state, kinds);
        int gx = gutter();
        int cwi = cwid();
        // selection background
        int r0 = -1, c0 = 0, r1 = -1, c1 = 0;
        if (dc->has_sel()) dc->sel_range(r0, c0, r1, c1);
        if (r0 >= 0 && li >= r0 && li <= r1) {
            int a = li == r0 ? disp_col(l, c0) : 0;
            int b = li == r1 ? disp_col(l, c1) : disp_col(l, len) + 1;
            a = max(a - dc->left, 0); b = max(b - dc->left, 0);
            if (b > a) c.fill(gx + a * cwi, y - 1, (b - a) * cwi, lh(), c_sel());
        }
        // search matches
        if (find_on && find_f.len) {
            int n = find_f.len;
            for (int i = 0; i + n <= len; i++)
                if (match_at(l + i, find_f.text, n, match_case)) {
                    int a = disp_col(l, i) - dc->left, b = disp_col(l, i + n) - dc->left;
                    bool curm = dc->has_sel() && li == dc->row && i + n == dc->col;
                    if (b > 0) c.fill(gx + max(a, 0) * cwi, y - 1, (b - max(a, 0)) * cwi, lh(), curm ? 0xFFFFA040 : 0xFFFFE070);
                    i += n - 1;
                }
        }
        // bracket pair
        if (mr >= 0) {
            if (li == mr || li == br) {
                int cc = li == mr ? mc : bc;
                int a = disp_col(l, cc) - dc->left;
                if (a >= 0) c.rect(gx + a * cwi - 1, y - 1, cwi + 1, lh(), accent());
            }
        }
        // glyphs
        int d = 0;
        const char* p = l;
        const char* end = l + len;
        int xr = cw - 10;
        while (p < end) {
            const char* q = p;
            u32 cp = utf8_decode(q, end);
            int off = (int)(p - l);
            u32 col = kind_color(off < 4096 ? kinds[off] : (u8)K_TEXT);
            if (cp == '\t') {
                int wd = 4 - d % 4;
                if (whitespace) { int px = gx + (d - dc->left) * cwi; if (px >= gx && px < xr) c.text(px, y + scale - 1, G_RIGHT, blend(c_bg(), c_text(), 60), scale); }
                d += wd;
            } else {
                char g[4];
                int n = glyph_for(cp, g);
                int px = gx + (d - dc->left) * cwi;
                if (n == 2 && is_emoji_lead((u8)g[0])) {
                    if (px >= gx - cwi && px < xr) {
                        char two[3] = {g[0], g[1], 0};
                        int sv = c.emoji_px;
                        c.emoji_px = scale == 1 ? 12 : 12;
                        c.text(px, y + scale - 1, two, col, scale);
                        c.emoji_px = sv;
                    }
                } else {
                    for (int k = 0; k < n; k++) {
                        int gxp = px + k * cwi;
                        if (gxp >= gx && gxp + cwi <= xr) {
                            if (whitespace && g[k] == ' ') c.put(gxp + cwi / 2, y + 6 * scale, blend(c_bg(), c_text(), 50));
                            else c.draw_char(gxp, y + scale - 1, (u8)g[k], col, scale);
                        }
                    }
                }
                d += n;
            }
            p = q;
            if (gx + (d - dc->left) * cwi > xr) break;
        }
    }

    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        Doc* dc = d();
        c.fill(0, 0, w, h, c_bg());
        draw_tabs(c, w);
        if (find_on) draw_find(c, w);
        int top = top_y();
        int vh = view_h();
        int gx = gutter();
        if (numbers) {
            c.fill(0, top, gx - 4, vh, c_gutter());
            c.vline(gx - 4, top, vh, dark ? 0xFF332C4A : 0xFFD8D1E8);
        }
        c.set_clip({0, top, w - 10, vh});
        int first = dc->sc.pos() / lh(), yoff = dc->sc.pos() % lh();
        int mr = -1, mc = 0, br = 0, bc = 0;
        if (!dc->has_sel() && !match_bracket(mr, mc, br, bc)) mr = -1;
        for (int i = 0; i < vis_rows() + 2; i++) {
            int li = first + i;
            if (li >= dc->lines.len) break;
            int y = top + 3 + i * lh() - yoff;
            if (y > top + vh) break;
            if (li == dc->row && !dc->has_sel()) c.fill(gx - 3, y - 1, w - gx + 3, lh(), c_cur_line());
            if (numbers) {
                char num[12];
                snprintf(num, sizeof(num), "%d", li + 1);
                c.text(gx - 10 - (int)strlen(num) * 8, y + scale - 1, num, li == dc->row ? accent() : c_gutter_text(), 1, li == dc->row);
            }
            draw_line(c, dc, li, y, w, mr, mc, br, bc);
        }
        // caret
        int cy = top + 3 + dc->row * lh() - dc->sc.pos();
        int cx = gx + (disp_col(dc->lines[dc->row], dc->col) - dc->left) * cwid();
        if (win == wm_focused() && !(find_on && find_focus >= 0) && (ticks() / 530) % 2 == 0 && cy > top - lh() && cy < top + vh)
            c.fill(cx, cy - 1, 2, lh(), accent());
        c.set_clip({0, 0, w, h});
        // scrollbar
        sb.draw(c, w - 10, top, vh, content_h() + vh - lh(), vh, dc->sc.pos());
        // status bar
        c.fill(0, h - STATUS_H, w, STATUS_H, th::panel);
        c.hline(0, h - STATUS_H, w, th::border);
        char st[128];
        int selc = 0;
        if (dc->has_sel()) {
            int r0, c0, r1, c1;
            dc->sel_range(r0, c0, r1, c1);
            char* t = dc->get(r0, c0, r1, c1);
            selc = (int)strlen(t);
            kfree(t);
        }
        if (selc) snprintf(st, sizeof(st), "ln %d, col %d  " G_BULLET "  %d selected  " G_BULLET "  %d lines", dc->row + 1, disp_col(dc->lines[dc->row], dc->col) + 1, selc, dc->lines.len);
        else snprintf(st, sizeof(st), "ln %d, col %d  " G_BULLET "  %d lines", dc->row + 1, disp_col(dc->lines[dc->row], dc->col) + 1, dc->lines.len);
        c.text(8, h - 15, st, th::muted);
        char right[96];
        if (status_t && ticks() - status_t < 3000) strlcpy(right, status, sizeof(right));
        else snprintf(right, sizeof(right), "%s  " G_BULLET "  UTF-8  " G_BULLET "  %s%s", LANGS[dc->lang].name, scale == 1 ? "100%" : "200%",
                      dc->modified ? "  " G_BULLET "  unsaved" : "");
        c.text(w - text_width(right) - 10, h - 15, right, dc->modified && right[0] != 's' ? th::red : th::muted);
    }

    void tick() override {
        Doc* dc = d();
        if (dc->sc.step()) ui_invalidate();
        static u64 last = 0;
        if (ticks() - last > 260) { last = ticks(); ui_invalidate(); }
    }

    // ------------------------------------------------------------ mouse
    void pos_from_mouse(int x, int y, int& r, int& cc) {
        Doc* dc = d();
        r = clamp((y - top_y() - 3 + dc->sc.pos()) / lh(), 0, dc->lines.len - 1);
        int dcol = dc->left + (x - gutter() + cwid() / 2) / cwid();
        cc = byte_at_disp(dc->lines[r], max(0, dcol));
    }

    void select_word(int r, int cc) {
        Doc* dc = d();
        const char* l = dc->lines[r];
        int len = (int)strlen(l);
        int a = cc, b = cc;
        if (cc < len && !is_word(cp_at(l, cc))) {
            dc->arow = r; dc->acol = cc; dc->row = r; dc->col = next_col(l, cc);
            return;
        }
        while (a > 0 && is_word(cp_at(l, prev_col(l, a)))) a = prev_col(l, a);
        while (b < len && is_word(cp_at(l, b))) b = next_col(l, b);
        dc->arow = r; dc->acol = a; dc->row = r; dc->col = b;
    }

    void on_mouse(const MouseEv& e) override {
        Doc* dc = d();
        // tabs
        if (e.y < TAB_H) {
            if (e.kind == M_WHEEL) { tab_scroll = max(0, tab_scroll - e.wheel * 30); ui_invalidate(); return; }
            if (e.kind != M_DOWN || e.button == 0) return;
            for (int i = 0; i < docs.len; i++) {
                int x, tw;
                tab_rect(i, x, tw);
                if (e.x >= x && e.x < x + tw) {
                    if (e.button == 3 || (e.x >= x + tw - 18 && e.x < x + tw - 2)) { close_tab(i); return; }
                    cur = i;
                    set_title(title());
                    ui_invalidate();
                    return;
                }
            }
            int x, tw;
            tab_rect(docs.len, x, tw);
            if (e.x >= x + 2 && e.x < x + 26) { docs.push(new Doc()); cur = docs.len - 1; set_title(title()); }
            ui_invalidate();
            return;
        }
        // find bar
        if (find_on && e.y < top_y()) {
            if (e.kind != M_DOWN && e.kind != M_MOVE) return;
            int y = TAB_H;
            if (e.kind == M_MOVE && e.left) {
                TextField& f = find_focus == 0 ? find_f : repl_f;
                f.drag(e.x - 56, 220);
                ui_invalidate();
                return;
            }
            if (e.kind != M_DOWN) return;
            if (Rect{56, y + 5, 220, 20}.contains(e.x, e.y)) { find_focus = 0; find_f.click(e.x - 56, 220, kbd_shift_down()); }
            else if (replace_on && Rect{56, y + FIND_H + 5, 220, 20}.contains(e.x, e.y)) { find_focus = 1; repl_f.click(e.x - 56, 220, kbd_shift_down()); }
            else if (Rect{284, y + 4, 24, 22}.contains(e.x, e.y)) { find_next(false); }
            else if (Rect{312, y + 4, 24, 22}.contains(e.x, e.y)) { find_next(true); }
            else if (Rect{342, y + 4, 34, 22}.contains(e.x, e.y)) { match_case = !match_case; match_count = count_matches(); }
            else if (Rect{cw - 28, y + 4, 22, 22}.contains(e.x, e.y)) { find_on = false; }
            else if (replace_on && Rect{284, y + FIND_H + 4, 64, 22}.contains(e.x, e.y)) replace_one();
            else if (replace_on && Rect{354, y + FIND_H + 4, 40, 22}.contains(e.x, e.y)) replace_all();
            ui_invalidate();
            return;
        }
        int top = top_y(), vh = view_h();
        if (e.kind == M_WHEEL) {
            if (kbd_shift_down()) dc->left = max(0, dc->left - e.wheel * 4);
            else dc->sc.set(clamp(dc->sc.target - e.wheel * lh() * 3, 0, max(0, content_h())));
            ui_invalidate();
            return;
        }
        int off = dc->sc.target;
        if (sb.handle(e, cw - 10, top, vh, content_h() + vh - lh(), vh, off)) { dc->sc.jump(off); return; }
        if (e.y >= ch - STATUS_H) return;
        if (e.kind == M_DOWN && e.button == 1) {
            int r, cc;
            pos_from_mouse(e.x, e.y, r, cc);
            bool near = ticks() - click_t < 450;
            click_count = near ? click_count + 1 : 1;
            click_t = ticks();
            if (click_count == 2) { select_word(r, cc); mouse_sel = false; }
            else if (click_count >= 3) {
                dc->arow = r; dc->acol = 0;
                if (r + 1 < dc->lines.len) { dc->row = r + 1; dc->col = 0; } else { dc->row = r; dc->col = (int)strlen(dc->lines[r]); }
                mouse_sel = false;
                click_count = 0;
            } else {
                if (kbd_shift_down()) { if (dc->arow < 0) { dc->arow = dc->row; dc->acol = dc->col; } }
                else { dc->arow = r; dc->acol = cc; }
                dc->row = r; dc->col = cc;
                mouse_sel = true;
            }
            dc->goal = -1;
            find_focus = -1;
        } else if (e.kind == M_MOVE && mouse_sel && e.left) {
            int r, cc;
            int y = clamp(e.y, top, top + vh - 1);
            pos_from_mouse(e.x, y, r, cc);
            // drag past the edge scrolls
            if (e.y < top) dc->sc.set(max(0, dc->sc.target - lh()));
            if (e.y > top + vh) dc->sc.set(dc->sc.target + lh());
            dc->row = r; dc->col = cc;
            ensure_visible();
        } else if (e.kind == M_UP) {
            mouse_sel = false;
            if (e.button == 2) {
                int rr, cc;
                pos_from_mouse(e.x, e.y, rr, cc);
                Menu m;
                m.add("Cut", S_CUT, "ctrl+x", dc->has_sel()).add("Copy", S_COPY, "ctrl+c", dc->has_sel())
                 .add("Paste", S_PASTE, "ctrl+v", clip_has()).sep().add("Select all", S_SELALL, "ctrl+a")
                 .add("Find" G_ELLIP, S_FIND, "ctrl+f").add("Go to line" G_ELLIP, S_GOTO, "ctrl+g");
                Rect cl = win->client();
                menu_popup(&m, cl.x + e.x, cl.y + e.y, [](void* ctx, int cmd) { ((App*)ctx)->on_command(cmd); }, this);
            }
        }
        ui_invalidate();
    }

    // ------------------------------------------------------------ keyboard
    void close_tab(int i) {
        if (docs[i]->modified) {
            cur = i;
            can_close_tab_prompt();
            return;
        }
        if (docs.len == 1) { force_close(); return; }
        delete docs[i];
        docs.remove(i);
        cur = clamp(cur >= i && cur > 0 ? cur - 1 : cur, 0, docs.len - 1);
        set_title(title());
        ui_invalidate();
    }
    static void tab_close_cb(App* a, int choice) {
        SlateApp* s = (SlateApp*)a;
        if (choice == 0) {
            if (!s->d()->path[0]) { s->pick_file("Save as", "/home", "", true, "untitled.txt", save_as_then_close_cb); return; }
            if (!s->save_doc(s->d(), s->d()->path)) return;
        } else if (choice != 1) return;
        s->d()->modified = false;
        s->close_tab(s->cur);
    }
    static void save_as_then_close_cb(App* a, const char* p) {
        SlateApp* s = (SlateApp*)a;
        if (s->save_doc(s->d(), p)) s->close_tab(s->cur);
    }
    void can_close_tab_prompt() {
        char m[160];
        snprintf(m, sizeof(m), "save changes to %s before closing?", d()->title);
        ask(m, "Save", "Don't save", "Cancel", tab_close_cb);
    }

    int word_left(const char* l, int col) {
        while (col > 0 && !is_word(cp_at(l, prev_col(l, col)))) col = prev_col(l, col);
        while (col > 0 && is_word(cp_at(l, prev_col(l, col)))) col = prev_col(l, col);
        return col;
    }
    int word_right(const char* l, int col) {
        int len = (int)strlen(l);
        while (col < len && !is_word(cp_at(l, col))) col = next_col(l, col);
        while (col < len && is_word(cp_at(l, col))) col = next_col(l, col);
        return col;
    }

    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        Doc* dc = d();
        // the find bar owns the keyboard while a field has focus
        if (find_on && find_focus >= 0 && !(e.ctrl && (e.key == 'f' || e.key == 'h' || e.key == 's' || e.key == 'w' || e.key == 'n' || e.key == 'o'))) {
            TextField& f = find_focus == 0 ? find_f : repl_f;
            if (e.key == K_ESC) { find_on = false; find_focus = -1; ui_invalidate(); return; }
            if (e.key == K_ENTER) {
                if (find_focus == 1) replace_one();
                else find_next(!e.shift);
                ui_invalidate();
                return;
            }
            if (e.key == K_TAB && replace_on) { find_focus ^= 1; ui_invalidate(); return; }
            if (e.key == K_DOWN || e.key == K_UP) { find_next(e.key == K_DOWN); ui_invalidate(); return; }
            if (f.on_key(e)) { if (&f == &find_f) { match_count = count_matches(); if (find_next(true, true)) {} } ui_invalidate(); return; }
        }
        if (e.ctrl) {
            switch (e.key) {
            case 's': on_command(e.shift ? S_SAVE_AS : S_SAVE); return;
            case 'o': on_command(S_OPEN); return;
            case 'n': on_command(S_NEW); return;
            case 'w': on_command(S_CLOSE_TAB); return;
            case 'x': on_command(S_CUT); return;
            case 'c': on_command(S_COPY); return;
            case 'v': on_command(S_PASTE); return;
            case 'a': on_command(S_SELALL); return;
            case 'z': on_command(e.shift ? S_REDO : S_UNDO); return;
            case 'y': on_command(S_REDO); return;
            case 'f': on_command(S_FIND); return;
            case 'h': on_command(S_REPLACE); return;
            case 'g': on_command(S_GOTO); return;
            case 'k': on_command(S_DELLINE); return;
            case 'd': on_command(S_DUPLINE); return;
            case '/': on_command(S_COMMENT); return;
            case '=': case '+': scale = 2; ui_invalidate(); return;
            case '-': scale = 1; ui_invalidate(); return;
            case ']': on_command(S_INDENT); return;
            case '[': on_command(S_OUTDENT); return;
            case K_TAB: on_command(e.shift ? S_PREV_TAB : S_NEXT_TAB); return;
            }
        }
        if (e.alt && (e.key == K_UP || e.key == K_DOWN)) { on_command(e.key == K_UP ? S_MOVE_UP : S_MOVE_DOWN); return; }
        bool nav = e.key == K_UP || e.key == K_DOWN || e.key == K_LEFT || e.key == K_RIGHT || e.key == K_HOME ||
                   e.key == K_END || e.key == K_PGUP || e.key == K_PGDN;
        if (nav) {
            const char* l = dc->lines[dc->row];
            if (!e.shift && dc->has_sel() && (e.key == K_LEFT || e.key == K_RIGHT) && !e.ctrl) {
                int r0, c0, r1, c1;
                dc->sel_range(r0, c0, r1, c1);
                if (e.key == K_LEFT) { dc->row = r0; dc->col = c0; } else { dc->row = r1; dc->col = c1; }
                dc->arow = -1;
                dc->goal = -1;
                ensure_visible();
                ui_invalidate();
                return;
            }
            int nr = dc->row, nc = dc->col;
            bool vertical = false;
            switch (e.key) {
            case K_LEFT:
                if (e.ctrl) nc = word_left(l, nc);
                else if (nc > 0) nc = prev_col(l, nc);
                else if (nr > 0) { nr--; nc = (int)strlen(dc->lines[nr]); }
                break;
            case K_RIGHT:
                if (e.ctrl) nc = word_right(l, nc);
                else if (nc < (int)strlen(l)) nc = next_col(l, nc);
                else if (nr < dc->lines.len - 1) { nr++; nc = 0; }
                break;
            case K_UP: vertical = true; nr = max(0, nr - 1); break;
            case K_DOWN: vertical = true; nr = min(dc->lines.len - 1, nr + 1); break;
            case K_PGUP: vertical = true; nr = max(0, nr - vis_rows()); break;
            case K_PGDN: vertical = true; nr = min(dc->lines.len - 1, nr + vis_rows()); break;
            case K_HOME: {
                if (e.ctrl) { nr = 0; nc = 0; break; }
                int ind = 0;
                while (l[ind] == ' ') ind++;
                nc = nc == ind || ind >= (int)strlen(l) ? 0 : ind;
                break;
            }
            case K_END: if (e.ctrl) { nr = dc->lines.len - 1; nc = (int)strlen(dc->lines[nr]); } else nc = (int)strlen(l); break;
            }
            if (vertical) {
                if (dc->goal < 0) dc->goal = disp_col(l, dc->col);
                nc = byte_at_disp(dc->lines[nr], dc->goal);
            } else dc->goal = -1;
            move_to(nr, nc, e.shift);
            ensure_visible();
            ui_invalidate();
            return;
        }
        switch (e.key) {
        case K_ENTER: newline(); break;
        case K_TAB:
            if (e.shift) { indent_rows(true); break; }
            if (dc->has_sel()) {
                int r0, c0, r1, c1;
                dc->sel_range(r0, c0, r1, c1);
                if (r1 > r0) { indent_rows(false); break; }
            }
            {
                int dcn = disp_col(dc->lines[dc->row], dc->col);
                insert_text("    " + (dcn % 4));
            }
            break;
        case K_BACKSPACE:
            if (dc->has_sel()) { insert_text(""); break; }
            if (e.ctrl) {
                int t = word_left(dc->lines[dc->row], dc->col);
                if (t < dc->col) dc->edit(dc->row, t, dc->row, dc->col, "");
                else if (dc->row > 0) dc->edit(dc->row - 1, (int)strlen(dc->lines[dc->row - 1]), dc->row, 0, "");
            } else if (dc->col > 0) {
                const char* l = dc->lines[dc->row];
                int pc = prev_col(l, dc->col);
                // backspace through indentation one step at a time? keep it plain: one character
                dc->edit(dc->row, pc, dc->row, dc->col, "");
            } else if (dc->row > 0) dc->edit(dc->row - 1, (int)strlen(dc->lines[dc->row - 1]), dc->row, 0, "");
            dc->goal = -1;
            break;
        case K_DELETE:
            if (dc->has_sel()) { insert_text(""); break; }
            {
                const char* l = dc->lines[dc->row];
                if (e.ctrl) {
                    int t = word_right(l, dc->col);
                    if (t > dc->col) { int sr = dc->row, scc = dc->col; dc->edit(dc->row, dc->col, dc->row, t, ""); dc->row = sr; dc->col = scc; break; }
                }
                if (dc->col < (int)strlen(l)) { int sr = dc->row, scc = dc->col; dc->edit(dc->row, dc->col, dc->row, next_col(l, dc->col), ""); dc->row = sr; dc->col = scc; }
                else if (dc->row < dc->lines.len - 1) { int sr = dc->row, scc = dc->col; dc->edit(dc->row, dc->col, dc->row + 1, 0, ""); dc->row = sr; dc->col = scc; }
            }
            break;
        case K_ESC: dc->arow = -1; if (find_on) find_on = false; break;
        default:
            if (e.ch >= 32 && e.ch < 127 && !e.ctrl && !e.alt) {
                char t[2] = {e.ch, 0};
                insert_text(t, true);
            }
        }
        ensure_visible();
        ui_invalidate();
    }

    // ------------------------------------------------------------ commands
    static void open_cb(App* a, const char* path) { ((SlateApp*)a)->open_path_tab(path); }
    static void save_as_cb(App* a, const char* path) {
        SlateApp* s = (SlateApp*)a;
        s->save_doc(s->d(), path);
    }
    static void goto_cb(App* a, const char* text) {
        SlateApp* s = (SlateApp*)a;
        int n = atoi(text);
        if (n < 1) return;
        s->move_to(n - 1, 0, false);
        s->ensure_visible();
    }
    static void win_close_cb(App* a, int choice) {
        SlateApp* s = (SlateApp*)a;
        if (choice == 0) {
            for (int i = 0; i < s->docs.len; i++) {
                Doc* dc = s->docs[i];
                if (!dc->modified) continue;
                if (!dc->path[0]) { s->cur = i; s->pick_file("Save as", "/home", "", true, "untitled.txt", save_then_close_all_cb); return; }
                s->save_doc(dc, dc->path);
            }
            s->force_close();
        } else if (choice == 1) s->force_close();
    }
    static void save_then_close_all_cb(App* a, const char* p) {
        SlateApp* s = (SlateApp*)a;
        if (s->save_doc(s->d(), p)) win_close_cb(a, 0);
    }
    bool can_close() override {
        int n = 0;
        for (int i = 0; i < docs.len; i++) n += docs[i]->modified;
        if (!n) return true;
        char m[160];
        snprintf(m, sizeof(m), n == 1 ? "save the changes you made before closing?" : "%d documents have unsaved changes. save them before closing?", n);
        ask(m, "Save", "Don't save", "Cancel", win_close_cb);
        return false;
    }

    void on_command(int cmd) override {
        Doc* dc = d();
        if (cmd >= S_LANG && cmd < S_LANG + N_LANGS) { dc->lang = cmd - S_LANG; dc->states_ok = 0; ui_invalidate(); return; }
        switch (cmd) {
        case S_NEW: docs.push(new Doc()); cur = docs.len - 1; set_title(title()); break;
        case S_OPEN: pick_file("Open file", dc->path[0] ? "" : "/home", "", false, "", open_cb); break;
        case S_SAVE: if (dc->path[0]) save_doc(dc, dc->path); else pick_file("Save as", "/home", "", true, "untitled.txt", save_as_cb); break;
        case S_SAVE_AS: pick_file("Save as", "/home", "", true, dc->path[0] ? dc->title : "untitled.txt", save_as_cb); break;
        case S_CLOSE_TAB: close_tab(cur); break;
        case S_CLOSE: close(); break;
        case S_COPY: copy_sel(false); break;
        case S_CUT: copy_sel(true); break;
        case S_PASTE: paste(); break;
        case S_SELALL: dc->arow = 0; dc->acol = 0; dc->row = dc->lines.len - 1; dc->col = (int)strlen(dc->lines[dc->row]); break;
        case S_UNDO: dc->do_undo(); break;
        case S_REDO: dc->do_redo(); break;
        case S_FIND: open_find(false); break;
        case S_REPLACE: open_find(true); break;
        case S_GOTO: prompt("go to line:", "", goto_cb); break;
        case S_DELLINE: {
            int g = ++dc->group;
            int r0, r1;
            selected_rows(r0, r1);
            if (r1 + 1 < dc->lines.len) dc->edit(r0, 0, r1 + 1, 0, "", g);
            else if (r0 > 0) dc->edit(r0 - 1, (int)strlen(dc->lines[r0 - 1]), r1, (int)strlen(dc->lines[r1]), "", g);
            else dc->edit(0, 0, r1, (int)strlen(dc->lines[r1]), "", g);
            dc->arow = -1;
            dc->col = min(dc->col, (int)strlen(dc->lines[dc->row]));
            break;
        }
        case S_DUPLINE: {
            int r0, r1;
            selected_rows(r0, r1);
            char* t = dc->get(r0, 0, r1, (int)strlen(dc->lines[r1]));
            char* ins = (char*)kmalloc(strlen(t) + 2);
            ins[0] = '\n';
            strcpy(ins + 1, t);
            int er = r1, ec = (int)strlen(dc->lines[r1]);
            dc->edit(er, ec, er, ec, ins);
            kfree(t); kfree(ins);
            break;
        }
        case S_MOVE_UP: move_lines(-1); break;
        case S_MOVE_DOWN: move_lines(1); break;
        case S_COMMENT: toggle_comment(); break;
        case S_INDENT: indent_rows(false); break;
        case S_OUTDENT: indent_rows(true); break;
        case S_NUMBERS: numbers = !numbers; break;
        case S_ZOOM: scale = scale == 1 ? 2 : 1; break;
        case S_DARK: dark = !dark; break;
        case S_WS: whitespace = !whitespace; break;
        case S_NEXT_TAB: cur = (cur + 1) % docs.len; break;
        case S_PREV_TAB: cur = (cur + docs.len - 1) % docs.len; break;
        case S_PREVIEW:
            if (dc->path[0]) {
                if (dc->modified) save_doc(dc, dc->path);
                char u[260];
                snprintf(u, sizeof(u), "file://%s", dc->path);
                launch("__new__", "browser");
                Window* w = wm_focused();
                if (w) w->app->open_arg(u);
            } else info("save the file first, then it can be previewed in Beam.");
            break;
        case S_EMOJI: launch("emoji"); break;
        }
        set_title(title());
        ensure_visible();
        ui_invalidate();
    }

    int menus(Menu* m) override {
        Doc* dc = d();
        m[0].clear(); strlcpy(m[0].title, "File", 24);
        m[0].add("New tab", S_NEW, "ctrl+n").add("Open" G_ELLIP, S_OPEN, "ctrl+o").sep()
            .add("Save", S_SAVE, "ctrl+s").add("Save as" G_ELLIP, S_SAVE_AS, "ctrl+shift+s").sep()
            .add("Preview in Beam", S_PREVIEW).sep().add("Close tab", S_CLOSE_TAB, "ctrl+w").add("Close window", S_CLOSE);
        m[1].clear(); strlcpy(m[1].title, "Edit", 24);
        m[1].add("Undo", S_UNDO, "ctrl+z", dc->undo.len > 0).add("Redo", S_REDO, "ctrl+y", dc->redo.len > 0).sep()
            .add("Cut", S_CUT, "ctrl+x").add("Copy", S_COPY, "ctrl+c").add("Paste", S_PASTE, "ctrl+v", clip_has())
            .add("Select all", S_SELALL, "ctrl+a").sep()
            .add("Find" G_ELLIP, S_FIND, "ctrl+f").add("Replace" G_ELLIP, S_REPLACE, "ctrl+h").add("Go to line" G_ELLIP, S_GOTO, "ctrl+g").sep()
            .add("Toggle comment", S_COMMENT, "ctrl+/").add("Indent", S_INDENT, "ctrl+]").add("Outdent", S_OUTDENT, "ctrl+[")
            .add("Move line up", S_MOVE_UP, "alt+up").add("Move line down", S_MOVE_DOWN, "alt+down")
            .add("Duplicate line", S_DUPLINE, "ctrl+d").add("Delete line", S_DELLINE, "ctrl+k").add("Insert emoji" G_ELLIP, S_EMOJI);
        m[2].clear(); strlcpy(m[2].title, "View", 24);
        m[2].add("Line numbers", S_NUMBERS, "", true, numbers).add("Dark theme", S_DARK, "", true, dark)
            .add("Show whitespace", S_WS, "", true, whitespace).add("Big text", S_ZOOM, "ctrl+=", true, scale == 2)
            .add("Next tab", S_NEXT_TAB, "ctrl+tab");
        m[3].clear(); strlcpy(m[3].title, "Language", 24);
        for (int i = 0; i < N_LANGS; i++) m[3].add(LANGS[i].name, S_LANG + i, "", true, dc->lang == i);
        return 4;
    }
};

App* new_slate(const char* arg) { return new SlateApp(arg); }
