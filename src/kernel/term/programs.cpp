// silicon - full-console programs: edit, snake, rain, life
#include "shell.h"
#include "../settings.h"

FsNode* editor_cwd = nullptr;

// ================================================================ editor
struct Editor : ConsoleProgram {
    Console* c;
    FsNode* file;
    char path[160];
    Vec<char*> lines;
    int row = 0, col = 0, top = 0;
    bool modified = false, finished = false, confirm_quit = false;
    char status[96] = "";
    u64 status_until = 0;

    Editor(Console* con, FsNode* f, const char* p) : c(con), file(f) {
        strlcpy(path, p, sizeof(path));
        const char* t = f ? fs_read_text(f) : "";
        const char* s = t;
        for (;;) {
            const char* e = strchr(s, '\n');
            int l = e ? (int)(e - s) : (int)strlen(s);
            char* ln = (char*)kmalloc(l + 1);
            memcpy(ln, s, l);
            ln[l] = 0;
            for (int i = 0; i < l; i++) if (ln[i] == '\t') ln[i] = ' ';
            lines.push(ln);
            if (!e) break;
            s = e + 1;
        }
        if (lines.len > 1 && !lines.back()[0]) { kfree(lines.back()); lines.len--; }
        set_status(f ? "^S save   ^Q quit   ^K delete line" : "new file   ^S save   ^Q quit");
        c->cursor_visible = true;
        render();
    }
    ~Editor() { for (int i = 0; i < lines.len; i++) kfree(lines[i]); lines.free(); }

    void set_status(const char* s) { strlcpy(status, s, sizeof(status)); status_until = ticks() + 4000; }
    int  view_rows() { return c->rows - 2; }
    int  gutter() { return 5; }

    void render() {
        int cols = c->cols;
        c->fill_row(0, 0, 5);
        char title[160];
        snprintf(title, sizeof(title), " " G_CHIP " edit  %s%s", path, modified ? "  " G_BULLET " modified" : "");
        c->text_at(0, 0, title, 15, 5, true);
        char pos[32];
        snprintf(pos, sizeof(pos), "ln %d, col %d ", row + 1, col + 1);
        c->text_at(cols - (int)strlen(pos), 0, pos, 15, 5);
        for (int r = 0; r < view_rows(); r++) {
            int li = top + r;
            c->fill_row(r + 1, 7, 0);
            if (li >= lines.len) { c->put_at(0, r + 1, '~', 8, 0); continue; }
            char num[8];
            snprintf(num, sizeof(num), "%4d", li + 1);
            c->text_at(0, r + 1, num, li == row ? 13 : 8, 0);
            const char* l = lines[li];
            int x = gutter();
            int start = max(0, col - (cols - gutter() - 2));
            for (int i = start; l[i] && x < cols; i++, x++) c->put_at(x, r + 1, (u8)l[i], 7, 0);
        }
        int last = c->rows - 1;
        c->fill_row(last, 7, 8);
        const char* st = ticks() < status_until ? status : "^S save   ^Q quit   ^K delete line   arrows/pgup/pgdn move";
        c->text_at(1, last, st, 15, 8);
        int start = max(0, col - (cols - gutter() - 2));
        c->move_to(gutter() + col - start, row - top + 1);
    }

    void clamp_cursor() {
        row = clamp(row, 0, lines.len - 1);
        col = clamp(col, 0, (int)strlen(lines[row]));
        if (row < top) top = row;
        if (row >= top + view_rows()) top = row - view_rows() + 1;
    }

    void insert_char(char ch) {
        char* l = lines[row];
        int n = (int)strlen(l);
        char* nl = (char*)kmalloc(n + 2);
        memcpy(nl, l, col);
        nl[col] = ch;
        memcpy(nl + col + 1, l + col, n - col + 1);
        kfree(l);
        lines[row] = nl;
        col++;
        modified = true;
    }

    void save() {
        if (!file) {
            file = fs_lookup(path, editor_cwd);
            if (!file) file = fs_create_path(path, false, editor_cwd);
            if (!file) { set_status("cannot create file here"); return; }
        }
        u32 total = 0;
        for (int i = 0; i < lines.len; i++) total += strlen(lines[i]) + 1;
        char* buf = (char*)kmalloc(total + 1);
        u32 o = 0;
        for (int i = 0; i < lines.len; i++) {
            u32 l = strlen(lines[i]);
            memcpy(buf + o, lines[i], l);
            o += l;
            buf[o++] = '\n';
        }
        fs_write(file, buf, o);
        kfree(buf);
        modified = false;
        char s[96];
        snprintf(s, sizeof(s), "saved %u bytes", o);
        set_status(s);
    }

    void on_key(const KeyEvent& e) override {
        if (e.ctrl && e.key == 's') { save(); render(); return; }
        if ((e.ctrl && e.key == 'q') || e.key == K_ESC) {
            if (modified && !confirm_quit) { confirm_quit = true; set_status("unsaved changes! press ^Q again to discard"); render(); return; }
            finished = true;
            return;
        }
        confirm_quit = false;
        if (e.ctrl && e.key == 'k') {
            if (lines.len > 1) { kfree(lines[row]); lines.remove(row); }
            else { lines[0][0] = 0; }
            modified = true;
            clamp_cursor();
            render();
            return;
        }
        switch (e.key) {
        case K_UP: row--; break;
        case K_DOWN: row++; break;
        case K_LEFT: if (col > 0) col--; else if (row > 0) { row--; col = 1 << 20; } break;
        case K_RIGHT: if (col < (int)strlen(lines[row])) col++; else if (row < lines.len - 1) { row++; col = 0; } break;
        case K_HOME: col = 0; break;
        case K_END: col = 1 << 20; break;
        case K_PGUP: row -= view_rows(); break;
        case K_PGDN: row += view_rows(); break;
        case K_ENTER: {
            char* l = lines[row];
            char* rest = strdup(l + col);
            l[col] = 0;
            lines.insert(row + 1, rest);
            row++; col = 0;
            modified = true;
            break;
        }
        case K_BACKSPACE:
            if (col > 0) {
                char* l = lines[row];
                memmove(l + col - 1, l + col, strlen(l + col) + 1);
                col--;
                modified = true;
            } else if (row > 0) {
                char* prev = lines[row - 1];
                int pl = (int)strlen(prev);
                char* nl = (char*)kmalloc(pl + strlen(lines[row]) + 1);
                strcpy(nl, prev);
                strcpy(nl + pl, lines[row]);
                kfree(prev); kfree(lines[row]);
                lines[row - 1] = nl;
                lines.remove(row);
                row--; col = pl;
                modified = true;
            }
            break;
        case K_DELETE: {
            char* l = lines[row];
            if (col < (int)strlen(l)) { memmove(l + col, l + col + 1, strlen(l + col)); modified = true; }
            else if (row < lines.len - 1) {
                char* nl = (char*)kmalloc(strlen(l) + strlen(lines[row + 1]) + 1);
                strcpy(nl, l);
                strcpy(nl + strlen(l), lines[row + 1]);
                kfree(l); kfree(lines[row + 1]);
                lines[row] = nl;
                lines.remove(row + 1);
                modified = true;
            }
            break;
        }
        case K_TAB: for (int i = 0; i < 4; i++) insert_char(' '); break;
        default:
            if (e.ch >= 32 && e.ch < 127 && !e.ctrl) insert_char(e.ch);
        }
        clamp_cursor();
        render();
    }

    void tick() override {
        if (status_until && ticks() > status_until) { status_until = 0; render(); }
    }
    bool done() override { return finished; }
};

ConsoleProgram* make_editor(Console* c, FsNode* f, const char* path) { return new Editor(c, f, path); }

// ================================================================ snake
struct Snake : ConsoleProgram {
    Console* c;
    int w, h;                  // board in 2-char cells
    Vec<int> body;             // packed y*w+x, head at index 0
    int dir = 1, next_dir = 1; // 0 up 1 right 2 down 3 left
    int food = 0, score = 0, best = 0;
    bool over = false, finished = false;
    u64 last = 0;

    Snake(Console* con) : c(con) {
        w = c->cols / 2;
        h = c->rows - 1;
        c->cursor_visible = false;
        reset();
    }
    ~Snake() { body.free(); }

    void reset() {
        body.clear();
        for (int i = 0; i < 4; i++) body.push((h / 2) * w + w / 2 - i);
        dir = next_dir = 1;
        score = 0;
        over = false;
        place_food();
        draw_all();
    }
    void place_food() {
        for (;;) {
            food = rand() % (w * h);
            if (body.find(food) < 0) return;
        }
    }
    void cell(int p, u8 ch, int fg) {
        int x = p % w, y = p / w;
        c->put_at(x * 2, y + 1, ch, fg, 0);
        c->put_at(x * 2 + 1, y + 1, ch, fg, 0);
    }
    void status() {
        c->fill_row(0, 15, 5);
        char s[96];
        snprintf(s, sizeof(s), " " G_CHIP " snake   score %d   best %d   arrows steer, q quits", score, best);
        c->text_at(0, 0, s, 15, 5, true);
    }
    void draw_all() {
        for (int r = 1; r < c->rows; r++) c->fill_row(r, 7, 0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if ((x + y) % 2 == 0) c->put_at(x * 2, y + 1, '.', 8, 0);
        for (int i = 0; i < body.len; i++) cell(body[i], 0x80, i == 0 ? 10 : 2);
        cell(food, 0x9F, 9);
        status();
    }
    void on_key(const KeyEvent& e) override {
        if (e.key == 'q' || e.key == K_ESC) { finished = true; return; }
        if (over) { if (e.key == 'r' || e.key == ' ' || e.key == K_ENTER) reset(); return; }
        int nd = -1;
        if (e.key == K_UP || e.key == 'w') nd = 0;
        if (e.key == K_RIGHT || e.key == 'd') nd = 1;
        if (e.key == K_DOWN || e.key == 's') nd = 2;
        if (e.key == K_LEFT || e.key == 'a') nd = 3;
        if (nd >= 0 && (nd + 2) % 4 != dir) next_dir = nd;
    }
    void tick() override {
        if (over || ticks() - last < (u64)max(55, 110 - score * 2)) return;
        last = ticks();
        dir = next_dir;
        int hx = body[0] % w, hy = body[0] / w;
        if (dir == 0) hy--; else if (dir == 1) hx++; else if (dir == 2) hy++; else hx--;
        int np = hy * w + hx;
        if (hx < 0 || hy < 0 || hx >= w || hy >= h || body.find(np) >= 0) {
            over = true;
            best = max(best, score);
            const char* m1 = "  GAME OVER  ";
            const char* m2 = " r = again, q = quit ";
            c->text_at((c->cols - (int)strlen(m1)) / 2, c->rows / 2, m1, 15, 1, true);
            c->text_at((c->cols - (int)strlen(m2)) / 2, c->rows / 2 + 1, m2, 7, 8);
            if (settings.sounds) speaker_beep(110, 150);
            return;
        }
        cell(body[0], 0x80, 2);
        body.insert(0, np);
        if (np == food) {
            score++;
            place_food();
            cell(food, 0x9F, 9);
            status();
        } else {
            int tail = body.back();
            body.len--;
            int x = tail % w, y = tail / w;
            c->put_at(x * 2, y + 1, (x + y) % 2 == 0 ? '.' : ' ', 8, 0);
            c->put_at(x * 2 + 1, y + 1, ' ', 8, 0);
        }
        cell(np, 0x80, 10);
    }
    bool done() override { return finished; }
};

ConsoleProgram* make_snake(Console* c) { return new Snake(c); }

// ================================================================ rain
struct Rain : ConsoleProgram {
    Console* c;
    int* head;
    int* speed;
    int* len;
    u64 last = 0;
    int frame = 0;
    bool finished = false;

    Rain(Console* con) : c(con) {
        c->cursor_visible = false;
        head = (int*)kmalloc(sizeof(int) * c->cols);
        speed = (int*)kmalloc(sizeof(int) * c->cols);
        len = (int*)kmalloc(sizeof(int) * c->cols);
        for (int x = 0; x < c->cols; x++) respawn(x, true);
        for (int r = 0; r < c->rows; r++) c->fill_row(r, 7, 0);
    }
    ~Rain() { kfree(head); kfree(speed); kfree(len); }
    void respawn(int x, bool initial) {
        head[x] = initial ? -(int)(rand() % (c->rows * 2)) : -(int)(rand() % 12);
        speed[x] = 1 + rand() % 3;
        len[x] = 4 + rand() % (c->rows / 2);
    }
    u8 glyph() {
        static const char set[] = "01SiLICONsilicon<>/\\{}[]=+*#";
        int r = rand() % 40;
        if (r < (int)sizeof(set) - 1) return (u8)set[r];
        return (u8)(0x80 + rand() % 4);
    }
    void on_key(const KeyEvent&) override { finished = true; }
    void tick() override {
        if (ticks() - last < 45) return;
        last = ticks();
        frame++;
        for (int x = 0; x < c->cols; x++) {
            if (frame % speed[x]) continue;
            int hy = head[x];
            if (hy >= 0 && hy < c->rows) c->put_at(x, hy, glyph(), 15, 0, true);
            if (hy - 1 >= 0 && hy - 1 < c->rows) c->put_at(x, hy - 1, glyph(), 13, 0);
            for (int k = 2; k < 5; k++)
                if (hy - k >= 0 && hy - k < c->rows && rand() % 6 == 0) c->put_at(x, hy - k, glyph(), 5, 0);
            int ty = hy - len[x];
            if (ty >= 0 && ty < c->rows) c->put_at(x, ty, ' ', 7, 0);
            if (ty - 1 >= 0 && ty - 1 < c->rows) c->put_at(x, ty - 1, ' ', 7, 0);
            head[x]++;
            if (ty > c->rows) respawn(x, false);
        }
        c->text_at(c->cols - 18, c->rows - 1, " any key to exit ", 0, 13);
    }
    bool done() override { return finished; }
};

ConsoleProgram* make_rain(Console* c) { return new Rain(c); }

// ================================================================ life
struct Life : ConsoleProgram {
    Console* c;
    int w, h;
    u8* a;
    u8* b;
    bool paused = false, finished = false;
    u64 last = 0;
    int gen = 0;

    Life(Console* con) : c(con) {
        w = c->cols / 2; h = c->rows - 1;
        a = (u8*)kcalloc(w * h);
        b = (u8*)kcalloc(w * h);
        c->cursor_visible = false;
        seed();
    }
    ~Life() { kfree(a); kfree(b); }
    void seed() { for (int i = 0; i < w * h; i++) a[i] = rand() % 4 == 0; gen = 0; draw(); }
    void draw() {
        c->fill_row(0, 15, 4);
        char s[96];
        snprintf(s, sizeof(s), " " G_CHIP " life   generation %d   space pause, r reseed, q quit", gen);
        c->text_at(0, 0, s, 15, 4, true);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                u8 ch = a[y * w + x] ? 0x80 : ' ';
                int col = 6 + ((x + y + gen / 8) % 3 == 0 ? 8 : 0);
                c->put_at(x * 2, y + 1, ch, col, 0);
                c->put_at(x * 2 + 1, y + 1, ch, col, 0);
            }
    }
    void step() {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        if (!dx && !dy) continue;
                        int xx = (x + dx + w) % w, yy = (y + dy + h) % h;
                        n += a[yy * w + xx];
                    }
                u8 alive = a[y * w + x];
                b[y * w + x] = alive ? (n == 2 || n == 3) : n == 3;
            }
        swap(a, b);
        gen++;
    }
    void on_key(const KeyEvent& e) override {
        if (e.key == 'q' || e.key == K_ESC) finished = true;
        else if (e.key == ' ') paused = !paused;
        else if (e.key == 'r') seed();
    }
    void tick() override {
        if (paused || ticks() - last < 90) return;
        last = ticks();
        step();
        draw();
    }
    bool done() override { return finished; }
};

ConsoleProgram* make_life(Console* c) { return new Life(c); }

// ================================================================ sleep
struct Sleeper : ConsoleProgram {
    u64 until;
    bool finished = false;
    Sleeper(u32 ms) : until(ticks() + ms) {}
    void on_key(const KeyEvent& e) override { if (e.ctrl && e.key == 'c') finished = true; }
    void tick() override { if (ticks() >= until) finished = true; }
    bool done() override { return finished; }
};

ConsoleProgram* make_sleep(Console* c, u32 ms) { return new Sleeper(ms); }
