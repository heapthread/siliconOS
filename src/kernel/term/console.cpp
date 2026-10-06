// silicon - text console
#include "console.h"
#include "../text.h"

// palette order: 0 bg, 1 red, 2 green, 3 yellow, 4 blue, 5 magenta, 6 cyan, 7 text,
//                8 gray, 9..15 bright variants
const ConsoleTheme console_themes[] = {
    {"silicon", {0xFF15111F, 0xFFFF6B8B, 0xFF7FE0A8, 0xFFFFD27F, 0xFF7FA8FF, 0xFFC490FF, 0xFF6FE3E3, 0xFFDCD3F0,
                 0xFF6B6385, 0xFFFF9AB0, 0xFFA8F5C6, 0xFFFFE5A8, 0xFFA8C4FF, 0xFFDDB8FF, 0xFFA0F5F5, 0xFFFFFFFF}},
    {"amber",   {0xFF1A1206, 0xFFFF7040, 0xFFFFB000, 0xFFFFCC33, 0xFFCC8800, 0xFFFF9955, 0xFFFFC266, 0xFFFFB347,
                 0xFF7A5520, 0xFFFF9966, 0xFFFFC94D, 0xFFFFDD77, 0xFFE0A030, 0xFFFFB380, 0xFFFFD699, 0xFFFFE6B3}},
    {"phosphor",{0xFF06140A, 0xFF5CFF5C, 0xFF33FF66, 0xFFB3FF66, 0xFF33CC99, 0xFF66FF99, 0xFF66FFCC, 0xFF4DFF88,
                 0xFF1F6634, 0xFF99FF99, 0xFF80FFA0, 0xFFD9FF99, 0xFF80E6C0, 0xFFA6FFC4, 0xFFA6FFE6, 0xFFD6FFE0}},
    {"paper",   {0xFFF4EFE4, 0xFFC0392B, 0xFF2E8B57, 0xFFB8860B, 0xFF2F5DA8, 0xFF8E44AD, 0xFF16808C, 0xFF2B2833,
                 0xFF8C8577, 0xFFE74C3C, 0xFF27AE60, 0xFFD4A017, 0xFF3D7BD9, 0xFFA569BD, 0xFF1ABC9C, 0xFF000000}},
};
const int console_theme_count = ARRAY_LEN(console_themes);

void Console::init(int c, int r, int scrollback) {
    cols = c; rows = r;
    lines = max(scrollback, r);
    buf = (Cell*)kcalloc(sizeof(Cell) * cols * lines);
    used = 1;
    cx = cy = 0;
    reset_color();
    for (int i = 0; i < cols * lines; i++) buf[i] = Cell{' ', 7, 0, 0};
    dirty = true;
}

void Console::free() { if (buf) kfree(buf); buf = nullptr; }

void Console::resize(int c, int r) {
    if (c == cols && r == rows) return;
    c = max(c, 8); r = max(r, 2);
    Cell* nb = (Cell*)kcalloc(sizeof(Cell) * c * lines);
    for (int i = 0; i < c * lines; i++) nb[i] = Cell{' ', 7, 0, 0};
    for (int y = 0; y < used; y++)
        for (int x = 0; x < min(c, cols); x++) nb[y * c + x] = buf[y * cols + x];
    kfree(buf);
    buf = nb;
    cols = c; rows = r;
    if (cx >= cols) cx = cols - 1;
    dirty = true;
}

void Console::reset_color() { fg = 7; bg = 0; bold = false; }
void Console::set_color(int f, int b) { fg = f; if (b >= 0) bg = b; }

void Console::ensure_line(int i) {
    while (used <= i) {
        Cell* l = line(used);
        for (int x = 0; x < cols; x++) l[x] = Cell{' ', 7, 0, 0};
        used++;
    }
}

void Console::newline() {
    cx = 0;
    cy++;
    if (cy >= lines) {
        // drop the oldest line
        memmove(buf, buf + cols, sizeof(Cell) * cols * (lines - 1));
        if (sel_on) { sel_al--; sel_bl--; if (sel_al < 0 || sel_bl < 0) sel_on = false; }
        cy = lines - 1;
        used = lines;
        dropped++;
        Cell* l = line(cy);
        for (int x = 0; x < cols; x++) l[x] = Cell{' ', 7, 0, 0};
    } else {
        ensure_line(cy);
    }
}

void Console::apply_sgr() {
    if (esc_n == 0) { esc_vals[0] = 0; esc_n = 1; }
    for (int i = 0; i < esc_n; i++) {
        int v = esc_vals[i];
        if (v == 0) reset_color();
        else if (v == 1) bold = true;
        else if (v == 22) bold = false;
        else if (v >= 30 && v <= 37) fg = v - 30 == 0 ? 8 : v - 30;
        else if (v == 39) fg = 7;
        else if (v >= 90 && v <= 97) fg = v - 90 + 8;
        else if (v >= 40 && v <= 47) bg = v - 40;
        else if (v == 49) bg = 0;
        else if (v >= 100 && v <= 107) bg = v - 100 + 8;
    }
}

void Console::putc(char ch) {
    u8 c = (u8)ch;
    if (esc == 1) {
        if (c == '[') { esc = 2; esc_n = 0; esc_val = 0; return; }
        esc = 0;
        return;
    }
    if (esc == 2) {
        if (c >= '0' && c <= '9') { esc_val = esc_val * 10 + (c - '0'); return; }
        if (c == ';') { if (esc_n < 4) esc_vals[esc_n++] = esc_val; esc_val = 0; return; }
        if (esc_n < 4) esc_vals[esc_n++] = esc_val;
        if (c == 'm') apply_sgr();
        else if (c == 'J') clear();
        esc = 0;
        return;
    }
    dirty = true;
    switch (c) {
    case 27: esc = 1; return;
    case '\n': newline(); return;
    case '\r': cx = 0; return;
    case '\b': if (cx > 0) cx--; return;
    case '\t': do { putc(' '); } while (cx % 4); return;
    }
    if (cx >= cols) newline();
    line(cy)[cx] = Cell{c, (u8)(bold && fg < 8 ? fg + 8 : fg), (u8)bg, (u8)(bold ? 1 : 0)};
    cx++;
}

void Console::write(const char* s) { while (*s) putc(*s++); }

void Console::writef(const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    write(b);
}

void Console::clear() {
    for (int i = 0; i < cols * lines; i++) buf[i] = Cell{' ', 7, 0, 0};
    used = 1;
    cx = cy = 0;
    view_off = 0;
    dropped = 0;
    dirty = true;
}

void Console::move_to(int col, int row) {
    ensure_line(top() + rows - 1);
    cx = clamp(col, 0, cols - 1);
    cy = top() + clamp(row, 0, rows - 1);
}

void Console::put_at(int col, int row, u8 ch, int f, int b, bool bd) {
    if (col < 0 || row < 0 || col >= cols || row >= rows) return;
    ensure_line(top() + rows - 1);
    line(top() + row)[col] = Cell{ch, (u8)f, (u8)b, (u8)(bd ? 1 : 0)};
    dirty = true;
}

void Console::text_at(int col, int row, const char* s, int f, int b, bool bd) {
    for (; *s && col < cols; s++, col++) put_at(col, row, (u8)*s, f, b, bd);
}

void Console::fill_row(int row, int f, int b) {
    for (int x = 0; x < cols; x++) put_at(x, row, ' ', f, b);
}

void Console::truncate_abs(i64 l) {
    int rel = (int)clamp<i64>(l - dropped, 0, lines - 1);
    for (int y = rel; y < used; y++) {
        Cell* ln = line(y);
        for (int x = 0; x < cols; x++) ln[x] = Cell{' ', 7, 0, 0};
    }
    used = rel + 1;
    cy = rel;
    cx = 0;
    dirty = true;
}

void Console::set_cursor_abs(i64 l, int col) {
    int rel = (int)clamp<i64>(l - dropped, 0, lines - 1);
    ensure_line(rel);
    cy = rel;
    cx = clamp(col, 0, cols - 1);
    dirty = true;
}

// ---- selection
bool Console::in_sel(int li, int col) const {
    if (!sel_on) return false;
    int l0 = sel_al, c0 = sel_ac, l1 = sel_bl, c1 = sel_bc;
    if (l1 < l0 || (l1 == l0 && c1 < c0)) { swap(l0, l1); swap(c0, c1); }
    if (li < l0 || li > l1) return false;
    if (li == l0 && col < c0) return false;
    if (li == l1 && col > c1) return false;
    return true;
}
void Console::sel_begin(int col, int row) {
    sel_on = true;
    sel_al = sel_bl = clamp(top() - view_off + row, 0, used - 1);
    sel_ac = sel_bc = clamp(col, 0, cols - 1);
    dirty = true;
}
void Console::sel_update(int col, int row) {
    if (!sel_on) return;
    sel_bl = clamp(top() - view_off + row, 0, used - 1);
    sel_bc = clamp(col, 0, cols - 1);
    dirty = true;
}
void Console::sel_word(int col, int row) {
    int li = clamp(top() - view_off + row, 0, used - 1);
    Cell* l = line(li);
    col = clamp(col, 0, cols - 1);
    auto word = [&](int x) { u8 ch = l[x].ch; return ch > ' ' && ch != '|' && ch != ';' && ch != '(' && ch != ')'; };
    int a = col, b = col;
    if (word(col)) {
        while (a > 0 && word(a - 1)) a--;
        while (b < cols - 1 && word(b + 1)) b++;
    }
    sel_on = true; sel_al = sel_bl = li; sel_ac = a; sel_bc = b;
    dirty = true;
}
void Console::sel_line(int row) {
    int li = clamp(top() - view_off + row, 0, used - 1);
    sel_on = true; sel_al = sel_bl = li; sel_ac = 0; sel_bc = cols - 1;
    dirty = true;
}
void Console::sel_all() {
    sel_on = true; sel_al = 0; sel_ac = 0; sel_bl = used - 1; sel_bc = cols - 1;
    dirty = true;
}
int Console::sel_text(char* out, int cap) {
    int n = 0;
    out[0] = 0;
    if (!sel_on) return 0;
    int l0 = sel_al, c0 = sel_ac, l1 = sel_bl, c1 = sel_bc;
    if (l1 < l0 || (l1 == l0 && c1 < c0)) { swap(l0, l1); swap(c0, c1); }
    for (int li = l0; li <= l1 && li < used; li++) {
        int a = li == l0 ? c0 : 0, b = li == l1 ? c1 : cols - 1;
        Cell* l = line(li);
        int last = b;
        while (last >= a && l[last].ch == ' ') last--;      // trim trailing blanks
        for (int x = a; x <= last && n < cap - 2; x++) out[n++] = (char)(l[x].ch ? l[x].ch : ' ');
        if (li < l1 && n < cap - 2) out[n++] = '\n';
    }
    out[n] = 0;
    return n;
}

void Console::scroll_view(int n) {
    view_off = clamp(view_off + n, 0, top());
    dirty = true;
}

void Console::render(Canvas& c, int x0, int y0, bool focused) {
    int cw = cell_w(), ch = cell_h();
    int first = top() - view_off;
    c.fill(x0, y0, cols * cw, rows * ch, color(0));
    for (int r = 0; r < rows; r++) {
        int li = first + r;
        if (li < 0 || li >= used) continue;
        Cell* l = line(li);
        for (int col = 0; col < cols; col++) {
            Cell& cell = l[col];
            int px = x0 + col * cw, py = y0 + r * ch;
            bool sel = sel_on && in_sel(li, col);
            if (sel) c.fill(px, py, cw, ch, blend(color(0), color(4), 150));
            else if (cell.bg) c.fill(px, py, cw, ch, color(cell.bg));
            if (is_emoji_lead(cell.ch) && col + 1 < cols) {
                // an emoji takes two cells: the lead byte and the sprite index
                const Image* sp = emoji_sprite(emoji_from_bytes(cell.ch, l[col + 1].ch), scale > 1 ? 16 : 12);
                if (sp) {
                    c.blit(*sp, px + (2 * cw - sp->w) / 2, py + (ch - sp->h) / 2);
                    if (sel_on && in_sel(li, col + 1)) c.fill(px + cw, py, cw, ch, blend(color(0), color(4), 150));
                    col++;
                    continue;
                }
            }
            if (cell.ch != ' ') c.draw_char(px, py, cell.ch, color(cell.fg), scale, cell.attr & 1);
        }
    }
    if (cursor_visible && view_off == 0) {
        int px = x0 + cx * cw, py = y0 + (cy - first) * ch;
        if (cy - first < rows) {
            bool blink = (ticks() / 530) % 2 == 0;
            if (focused && blink) c.fill(px, py + ch - 2 * scale, cw, 2 * scale, color(15));
            else if (!focused) c.rect(px, py, cw, ch, color(8));
        }
    }
    if (view_off) {
        char s[32];
        snprintf(s, sizeof(s), " " G_UP " %d ", view_off);
        int w = text_width(s);
        c.fill(x0 + cols * cw - w - 4, y0 + 2, w, FONT_H, color(8));
        c.text(x0 + cols * cw - w - 4, y0 + 2, s, color(0));
    }
    dirty = false;
}
