// silicon - text console: cell grid with scrollback, rendered onto any canvas
#pragma once
#include "../core.h"
#include "../gfx/gfx.h"
#include "../drv/drivers.h"

struct Cell { u8 ch, fg, bg, attr; };   // attr bit0 = bold

struct ConsoleTheme {
    const char* name;
    u32 colors[16];      // 0 = background, 7 = default text
};
extern const ConsoleTheme console_themes[];
extern const int console_theme_count;

// A program that temporarily owns a console (editor, snake, rain...)
struct ConsoleProgram {
    virtual ~ConsoleProgram() {}
    virtual void on_key(const KeyEvent& e) = 0;
    virtual void tick() {}
    virtual bool done() = 0;
};

class Console {
public:
    int cols = 80, rows = 25;           // visible size
    int scale = 1;
    int theme = 0;
    bool cursor_visible = true;
    bool dirty = true;

    void init(int cols, int rows, int scrollback = 400);
    void resize(int cols, int rows);
    void free();

    void write(const char* s);
    void writef(const char* fmt, ...);
    void putc(char c);
    void clear();
    void set_color(int fg, int bg = -1);
    void reset_color();
    void move_to(int col, int row);     // in visible coordinates
    int  cursor_col() const { return cx; }
    int  cursor_row() const { return cy - top(); }

    // direct cell access for full-screen programs (visible coordinates)
    void put_at(int col, int row, u8 ch, int fg, int bg, bool bold = false);
    void text_at(int col, int row, const char* s, int fg, int bg, bool bold = false);
    void fill_row(int row, int fg, int bg);

    // absolute line numbers survive scrollback trimming (used by the line editor)
    i64  abs_line() const { return dropped + cy; }
    void truncate_abs(i64 l);
    void set_cursor_abs(i64 l, int col);

    void scroll_view(int lines);        // +up / -down into scrollback
    void scroll_reset() { view_off = 0; dirty = true; }
    int  scroll_total() const { return used; }
    int  scroll_offset() const { return top() - view_off; }
    void scroll_to(int offset) { view_off = clamp(top() - offset, 0, top()); dirty = true; }

    // text selection (mouse): positions are visible-screen coordinates
    bool has_sel() const { return sel_on && (sel_al != sel_bl || sel_ac != sel_bc); }
    void sel_begin(int col, int row);
    void sel_update(int col, int row);
    void sel_clear() { if (sel_on) { sel_on = false; dirty = true; } }
    void sel_word(int col, int row);
    void sel_line(int row);
    void sel_all();
    int  sel_text(char* out, int cap);       // the selected text (silicon codepage, newline separated)

    // pixel size and rendering
    int  cell_w() const { return FONT_W * scale; }
    int  cell_h() const { return FONT_H * scale; }
    void render(Canvas& c, int x, int y, bool focused = true);
    u32  color(int i) const { return console_themes[theme].colors[i & 15]; }

private:
    Cell* buf = nullptr;
    int lines = 0;          // total buffer lines (ring is not needed: we shift)
    int used = 0;           // lines in use
    int cx = 0, cy = 0;     // cursor in buffer coordinates
    int fg = 7, bg = 0;
    bool bold = false;
    int view_off = 0;
    i64 dropped = 0;
    int esc = 0;            // ANSI parse state
    int esc_val = 0, esc_vals[4], esc_n = 0;
    bool sel_on = false;
    int sel_al = 0, sel_ac = 0, sel_bl = 0, sel_bc = 0;   // anchor and cursor: buffer line + column
    bool in_sel(int li, int col) const;

    int  top() const { return max(0, used - rows); }
    Cell* line(int i) { return buf + i * cols; }
    void newline();
    void ensure_line(int i);
    void apply_sgr();
};

// ANSI helpers for shell output
#define C_RESET  "\x1b[0m"
#define C_BOLD   "\x1b[1m"
#define C_RED    "\x1b[31m"
#define C_GREEN  "\x1b[32m"
#define C_YELLOW "\x1b[33m"
#define C_BLUE   "\x1b[34m"
#define C_MAG    "\x1b[35m"
#define C_CYAN   "\x1b[36m"
#define C_WHITE  "\x1b[37m"
#define C_GRAY   "\x1b[90m"
#define C_BRED   "\x1b[91m"
#define C_BGREEN "\x1b[92m"
#define C_BYEL   "\x1b[93m"
#define C_BBLUE  "\x1b[94m"
#define C_BMAG   "\x1b[95m"
#define C_BCYAN  "\x1b[96m"
#define C_BWHITE "\x1b[97m"
