// silicon - Slate: the text editor
#include "apps.h"

enum {
    S_NEW = 1, S_OPEN, S_SAVE, S_SAVE_AS, S_CLOSE, S_CUT, S_COPY, S_PASTE, S_SELALL, S_DELLINE,
    S_NUMBERS, S_ZOOM, S_DUPLINE,
};

static char* clipboard = nullptr;

struct SlateApp : App {
    Vec<char*> lines;
    int row = 0, col = 0, top = 0, left = 0;
    int arow = -1, acol = 0;                  // selection anchor (-1 = none)
    bool modified = false, numbers = true, mouse_sel = false;
    int scale = 1;
    char path[200] = "";
    char tbuf[80];
    char status[64] = "";
    u64 status_t = 0;
    int cw = 0, ch = 0;
    Smooth sc;                               // vertical scroll in pixels
    ScrollBar sb;

    SlateApp(const char* p) {
        lines.push(strdup(""));
        if (p) load(p);
    }
    ~SlateApp() { clear_lines(); lines.free(); }

    void clear_lines() { for (int i = 0; i < lines.len; i++) kfree(lines[i]); lines.clear(); }

    void load(const char* p) {
        FsNode* f = fs_lookup(p);
        if (!f || f->dir) return;
        clear_lines();
        const char* t = fs_read_text(f);
        for (;;) {
            const char* e = strchr(t, '\n');
            int l = e ? (int)(e - t) : (int)strlen(t);
            char* ln = (char*)kmalloc(l + 1);
            memcpy(ln, t, l);
            ln[l] = 0;
            for (int i = 0; i < l; i++) if (ln[i] == '\t' || (u8)ln[i] < 32) ln[i] = ' ';
            lines.push(ln);
            if (!e) break;
            t = e + 1;
        }
        if (lines.len > 1 && !lines.back()[0]) { kfree(lines.back()); lines.len--; }
        fs_path(f, path, sizeof(path));
        row = col = top = left = 0;
        arow = -1;
        modified = false;
        set_title(title());
    }

    bool save_to(const char* p) {
        FsNode* f = fs_lookup(p);
        if (!f) f = fs_create_path(p, false);
        if (!f || f->dir) { info("can't save there. check the folder exists."); return false; }
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
        fs_write(f, buf, o);
        kfree(buf);
        fs_path(f, path, sizeof(path));
        modified = false;
        snprintf(status, sizeof(status), "saved %u bytes", o);
        status_t = ticks();
        set_title(title());
        return true;
    }

    const char* name() { const char* s = strrchr(path, '/'); return path[0] ? (s ? s + 1 : path) : "untitled"; }
    const char* app_id() override { return "slate"; }
    const char* title() override {
        snprintf(tbuf, sizeof(tbuf), "Slate " G_BULLET " %s%s", name(), modified ? " " G_DIAM : "");
        return tbuf;
    }
    int pref_w() override { return 600; }
    int pref_h() override { return 420; }
    u32 background() override { return 0xFFFBF9FF; }
    void on_resize(int w, int h) override { cw = w; ch = h; }
    bool wants_ibeam(int x, int y) override { return y > 0 && y < ch - 20; }

    // ------------------------------------------------------------ geometry
    int lh() { return 14 * scale; }
    int gutter() { return numbers ? 44 : 8; }
    int vis_rows() { return max(1, (ch - 22 - 6) / lh()); }
    int vis_cols() { return max(1, (cw - gutter() - 18) / (8 * scale)); }
    void ensure_visible() {
        if (row < top) top = row;
        if (row >= top + vis_rows()) top = row - vis_rows() + 1;
        sc.set(top * lh());
        if (col < left) left = col;
        if (col >= left + vis_cols()) left = col - vis_cols() + 1;
    }
    void mark() { modified = true; set_title(title()); }

    // ------------------------------------------------------------ selection helpers
    bool has_sel() { return arow >= 0 && (arow != row || acol != col); }
    void sel_range(int& r0, int& c0, int& r1, int& c1) {
        if (arow < row || (arow == row && acol < col)) { r0 = arow; c0 = acol; r1 = row; c1 = col; }
        else { r0 = row; c0 = col; r1 = arow; c1 = acol; }
    }
    char* sel_text() {
        int r0, c0, r1, c1;
        sel_range(r0, c0, r1, c1);
        u32 total = 1;
        for (int r = r0; r <= r1; r++) total += strlen(lines[r]) + 1;
        char* out = (char*)kmalloc(total);
        u32 o = 0;
        for (int r = r0; r <= r1; r++) {
            const char* l = lines[r];
            int a = r == r0 ? c0 : 0, b = r == r1 ? c1 : (int)strlen(l);
            memcpy(out + o, l + a, b - a);
            o += b - a;
            if (r != r1) out[o++] = '\n';
        }
        out[o] = 0;
        return out;
    }
    void delete_sel() {
        if (!has_sel()) { arow = -1; return; }
        int r0, c0, r1, c1;
        sel_range(r0, c0, r1, c1);
        char* first = lines[r0];
        char* last = lines[r1];
        char* nl = (char*)kmalloc(c0 + strlen(last + c1) + 1);
        memcpy(nl, first, c0);
        strcpy(nl + c0, last + c1);
        for (int r = r0; r <= r1; r++) kfree(lines[r]);
        for (int r = r1; r > r0; r--) lines.remove(r);
        lines[r0] = nl;
        row = r0; col = c0;
        arow = -1;
        mark();
    }
    void insert_text(const char* t) {
        delete_sel();
        for (; *t; t++) {
            if (*t == '\n') newline();
            else if ((u8)*t >= 32) insert_char(*t);
        }
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
        mark();
    }
    void newline() {
        char* l = lines[row];
        // keep the indentation of the current line
        int indent = 0;
        while (l[indent] == ' ' && indent < col) indent++;
        char* rest = (char*)kmalloc(indent + strlen(l + col) + 1);
        memset(rest, ' ', indent);
        strcpy(rest + indent, l + col);
        l[col] = 0;
        lines.insert(row + 1, rest);
        row++;
        col = indent;
        mark();
    }

    // ------------------------------------------------------------ drawing
    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        int r0 = -1, c0 = 0, r1 = -1, c1 = 0;
        if (has_sel()) sel_range(r0, c0, r1, c1);
        int cwid = 8 * scale;
        if (numbers) {
            c.fill(0, 0, gutter() - 6, h - 20, 0xFFEDE9F6);
            c.vline(gutter() - 6, 0, h - 20, 0xFFD8D1E8);
        }
        int first = sc.pos() / lh(), yoff = sc.pos() % lh();
        for (int i = 0; i < vis_rows() + 2; i++) {
            int li = first + i;
            if (li >= lines.len) break;
            int y = 4 + i * lh() - yoff;
            if (y > h - 22) break;
            if (li == row) c.fill(gutter() - 5, y - 1, w - gutter() + 5, lh(), 0xFFF1ECFF);
            if (numbers) {
                char num[12];
                snprintf(num, sizeof(num), "%3d", li + 1);
                c.text(4, y + 1 * scale - 1, num, li == row ? accent() : 0xFFB0A6C8, scale > 1 ? 1 : 1, li == row);
            }
            const char* l = lines[li];
            int len = (int)strlen(l);
            // selection background
            if (li >= r0 && li <= r1 && r0 >= 0) {
                int a = li == r0 ? c0 : 0, b = li == r1 ? c1 : len + 1;
                a = max(a - left, 0); b = max(b - left, 0);
                if (b > a) c.fill(gutter() + a * cwid, y - 1, (b - a) * cwid, lh(), blend(0xFFFFFFFF, accent(), 110));
            }
            if (left < len) c.text_n(gutter(), y, l + left, vis_cols() + 1, th::text, scale);
        }
        // cursor
        int cy = 3 + row * lh() - sc.pos();
        if (win == wm_focused() && (ticks() / 530) % 2 == 0 && cy > -lh() && cy < h - 22)
            c.fill(gutter() + (col - left) * cwid, cy, 2, lh(), accent());
        // scrollbar + status
        sb.draw(c, w - 10, 0, h - 20, (lines.len + vis_rows() - 1) * lh(), vis_rows() * lh(), sc.pos());
        c.fill(0, h - 20, w, 20, th::panel);
        c.hline(0, h - 20, w, th::border);
        char st[96];
        snprintf(st, sizeof(st), "ln %d, col %d  " G_BULLET "  %d lines", row + 1, col + 1, lines.len);
        c.text(8, h - 15, st, th::muted);
        const char* right = status_t && ticks() - status_t < 3000 ? status : (modified ? "unsaved" : (path[0] ? path : "new file"));
        c.text(w - text_width(right) - 10, h - 15, right, modified && right != status ? th::red : th::muted);
    }

    void tick() override {
        if (sc.step()) ui_invalidate();
    }

    // ------------------------------------------------------------ input
    void pos_from_mouse(int x, int y, int& r, int& cc) {
        r = clamp((y - 4 + sc.pos()) / lh(), 0, lines.len - 1);
        cc = clamp(left + (x - gutter() + 4 * scale) / (8 * scale), 0, (int)strlen(lines[r]));
    }
    void on_mouse(const MouseEv& e) override {
        int total = (lines.len + vis_rows() - 1) * lh();
        if (e.kind == M_WHEEL) {
            top = clamp(top - e.wheel * 3, 0, max(0, lines.len - 1));
            sc.set(top * lh());
            ui_invalidate();
            return;
        }
        int off = sc.target;
        if (sb.handle(e, cw - 10, 0, ch - 20, total, vis_rows() * lh(), off)) {
            top = off / lh();
            sc.jump(off);
            return;
        }
        if (e.y >= ch - 20) return;
        if (e.kind == M_DOWN && e.button == 1) {
            pos_from_mouse(e.x, e.y, row, col);
            arow = row; acol = col;
            mouse_sel = true;
        } else if (e.kind == M_DBL && e.button == 1) {
            // select the word under the mouse
            const char* l = lines[row];
            int a = col, b = col;
            while (a > 0 && l[a - 1] != ' ') a--;
            while (l[b] && l[b] != ' ') b++;
            arow = row; acol = a; col = b;
            mouse_sel = false;
        } else if (e.kind == M_MOVE && mouse_sel && e.left) {
            pos_from_mouse(e.x, e.y, row, col);
            ensure_visible();
        } else if (e.kind == M_UP) {
            mouse_sel = false;
            if (e.button == 2) {
                Menu m;
                m.add("Cut", S_CUT, "ctrl+x", has_sel()).add("Copy", S_COPY, "ctrl+c", has_sel())
                 .add("Paste", S_PASTE, "ctrl+v", clipboard != nullptr).sep().add("Select all", S_SELALL, "ctrl+a");
                Rect cl = win->client();
                menu_popup(&m, cl.x + e.x, cl.y + e.y, [](void* ctx, int cmd) { ((App*)ctx)->on_command(cmd); }, this);
            }
        }
        ui_invalidate();
    }

    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.ctrl) {
            switch (e.key) {
            case 's': on_command(e.shift ? S_SAVE_AS : S_SAVE); return;
            case 'o': on_command(S_OPEN); return;
            case 'n': on_command(S_NEW); return;
            case 'x': on_command(S_CUT); return;
            case 'c': on_command(S_COPY); return;
            case 'v': on_command(S_PASTE); return;
            case 'a': on_command(S_SELALL); return;
            case 'k': on_command(S_DELLINE); return;
            case 'd': on_command(S_DUPLINE); return;
            case '=': case '+': scale = 2; ui_invalidate(); return;
            case '-': scale = 1; ui_invalidate(); return;
            }
        }
        bool nav = e.key == K_UP || e.key == K_DOWN || e.key == K_LEFT || e.key == K_RIGHT || e.key == K_HOME ||
                   e.key == K_END || e.key == K_PGUP || e.key == K_PGDN;
        if (nav) {
            if (e.shift) { if (arow < 0) { arow = row; acol = col; } }
            else if (has_sel() && (e.key == K_LEFT || e.key == K_RIGHT)) {
                int r0, c0, r1, c1;
                sel_range(r0, c0, r1, c1);
                if (e.key == K_LEFT) { row = r0; col = c0; } else { row = r1; col = c1; }
                arow = -1;
                ensure_visible();
                ui_invalidate();
                return;
            } else arow = -1;
            switch (e.key) {
            case K_UP: if (row > 0) row--; break;
            case K_DOWN: if (row < lines.len - 1) row++; break;
            case K_LEFT: if (col > 0) col--; else if (row > 0) { row--; col = 1 << 20; } break;
            case K_RIGHT: if (col < (int)strlen(lines[row])) col++; else if (row < lines.len - 1) { row++; col = 0; } break;
            case K_HOME: col = 0; break;
            case K_END: col = 1 << 20; break;
            case K_PGUP: row = max(0, row - vis_rows()); break;
            case K_PGDN: row = min(lines.len - 1, row + vis_rows()); break;
            }
            col = clamp(col, 0, (int)strlen(lines[row]));
            ensure_visible();
            ui_invalidate();
            return;
        }
        switch (e.key) {
        case K_ENTER: delete_sel(); newline(); break;
        case K_TAB: delete_sel(); for (int i = 0; i < 4; i++) insert_char(' '); break;
        case K_BACKSPACE:
            if (has_sel()) { delete_sel(); break; }
            arow = -1;
            if (col > 0) {
                char* l = lines[row];
                memmove(l + col - 1, l + col, strlen(l + col) + 1);
                col--;
                mark();
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
                mark();
            }
            break;
        case K_DELETE:
            if (has_sel()) { delete_sel(); break; }
            arow = -1;
            if (col < (int)strlen(lines[row])) { char* l = lines[row]; memmove(l + col, l + col + 1, strlen(l + col)); mark(); }
            else if (row < lines.len - 1) {
                char* l = lines[row];
                char* nl = (char*)kmalloc(strlen(l) + strlen(lines[row + 1]) + 1);
                strcpy(nl, l);
                strcat_simple(nl, lines[row + 1]);
                kfree(l); kfree(lines[row + 1]);
                lines[row] = nl;
                lines.remove(row + 1);
                mark();
            }
            break;
        case K_ESC: arow = -1; break;
        default:
            if (e.ch >= 32 && e.ch < 127 && !e.ctrl) { delete_sel(); insert_char(e.ch); }
        }
        ensure_visible();
        ui_invalidate();
    }
    static void strcat_simple(char* d, const char* s) { strcpy(d + strlen(d), s); }

    // ------------------------------------------------------------ commands
    static void open_cb(App* a, const char* text) {
        SlateApp* s = (SlateApp*)a;
        FsNode* f = fs_lookup(text, fs_home());
        if (!f || f->dir) { s->info("no such file."); return; }
        char p[200];
        fs_path(f, p, sizeof(p));
        s->load(p);
    }
    static void save_as_cb(App* a, const char* text) {
        SlateApp* s = (SlateApp*)a;
        char p[200];
        if (text[0] == '/') strlcpy(p, text, sizeof(p));
        else snprintf(p, sizeof(p), "/home/%s", text);
        s->save_to(p);
    }
    static void close_cb(App* a, int choice) {
        SlateApp* s = (SlateApp*)a;
        if (choice == 0) {
            if (!s->path[0]) { s->prompt("save as (in /home):", "untitled.txt", save_as_cb); return; }
            if (s->save_to(s->path)) s->force_close();
        } else if (choice == 1) s->force_close();
    }
    bool can_close() override {
        if (!modified) return true;
        char m[160];
        snprintf(m, sizeof(m), "save changes to %s before closing?", name());
        ask(m, "Save", "Don't save", "Cancel", close_cb);
        return false;
    }

    void on_command(int cmd) override {
        switch (cmd) {
        case S_NEW: launch("__new__", "slate"); break;
        case S_OPEN: prompt("open file (path from /home):", "notes/todo.txt", open_cb); break;
        case S_SAVE: if (path[0]) save_to(path); else prompt("save as (in /home):", "untitled.txt", save_as_cb); break;
        case S_SAVE_AS: prompt("save as (in /home, or a full path):", path[0] ? path : "untitled.txt", save_as_cb); break;
        case S_CLOSE: close(); break;
        case S_COPY: case S_CUT:
            if (has_sel()) {
                if (clipboard) kfree(clipboard);
                clipboard = sel_text();
                if (cmd == S_CUT) delete_sel();
            }
            break;
        case S_PASTE: if (clipboard) insert_text(clipboard); break;
        case S_SELALL: arow = 0; acol = 0; row = lines.len - 1; col = (int)strlen(lines[row]); break;
        case S_DELLINE:
            arow = -1;
            if (lines.len > 1) { kfree(lines[row]); lines.remove(row); row = min(row, lines.len - 1); }
            else lines[0][0] = 0;
            col = min(col, (int)strlen(lines[row]));
            mark();
            break;
        case S_DUPLINE: lines.insert(row + 1, strdup(lines[row])); row++; mark(); break;
        case S_NUMBERS: numbers = !numbers; break;
        case S_ZOOM: scale = scale == 1 ? 2 : 1; break;
        }
        ensure_visible();
        ui_invalidate();
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "File", 24);
        m[0].add("New", S_NEW, "ctrl+n").add("Open" G_ELLIP, S_OPEN, "ctrl+o").sep()
            .add("Save", S_SAVE, "ctrl+s").add("Save as" G_ELLIP, S_SAVE_AS, "ctrl+shift+s").sep().add("Close", S_CLOSE, "ctrl+w");
        m[1].clear(); strlcpy(m[1].title, "Edit", 24);
        m[1].add("Cut", S_CUT, "ctrl+x", has_sel()).add("Copy", S_COPY, "ctrl+c", has_sel())
            .add("Paste", S_PASTE, "ctrl+v", clipboard != nullptr).sep().add("Select all", S_SELALL, "ctrl+a")
            .add("Duplicate line", S_DUPLINE, "ctrl+d").add("Delete line", S_DELLINE, "ctrl+k");
        m[2].clear(); strlcpy(m[2].title, "View", 24);
        m[2].add("Line numbers", S_NUMBERS, "", true, numbers).add("Big text", S_ZOOM, "ctrl+=", true, scale == 2);
        return 3;
    }
};

App* new_slate(const char* arg) { return new SlateApp(arg); }
