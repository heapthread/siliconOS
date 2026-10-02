// silicon - Pixel: a tiny pixel-art painter (64x48, saves .pix, can become the wallpaper)
#include "apps.h"

const u32 pix_palette[32] = {
    0xFFFFFFFF, 0xFF000000, 0xFF1D2B53, 0xFF7E2553, 0xFF008751, 0xFFAB5236, 0xFF5F574F, 0xFFC2C3C7,
    0xFFFFF1E8, 0xFFFF004D, 0xFFFFA300, 0xFFFFEC27, 0xFF00E436, 0xFF29ADFF, 0xFF83769C, 0xFFFF77A8,
    0xFFFFCCAA, 0xFF1A1428, 0xFF2A2140, 0xFF4A3470, 0xFF9B7BFF, 0xFFCCBCFF, 0xFF4FD1C5, 0xFF1E6F68,
    0xFFECBC5C, 0xFFAA7830, 0xFFFF7A9A, 0xFF7ED957, 0xFF5AA9FF, 0xFF3A3352, 0xFFE5486A, 0xFF8C84A4,
};

static const int PW = 64, PH = 48, Z = 7;
static const int CX = 84, CY = 10;

bool pix_load(FsNode* f, Image& out) {
    if (!f || f->dir || f->size < 8 || memcmp(f->data, "PIX1", 4)) return false;
    int w = f->data[4] | (f->data[5] << 8), h = f->data[6] | (f->data[7] << 8);
    if ((u32)(8 + w * h) > f->size) return false;
    out = image_new(w, h, 0);
    for (int i = 0; i < w * h; i++) out.px[i] = pix_palette[f->data[8 + i] & 31];
    return true;
}

enum { T_PENCIL, T_ERASER, T_FILL, T_LINE, T_RECT, T_PICK };
enum { P_NEW = 1, P_OPEN, P_SAVE, P_SAVE_AS, P_WALL, P_UNDO, P_CLEAR, P_GRID, P_MIRROR, P_CLOSE };

struct PixelApp : App {
    u8 px[PW * PH];
    Vec<u8*> undo;
    int tool = T_PENCIL, color = 20;
    bool grid = true, mirror = false, modified = false, drawing = false;
    int sx = -1, sy = -1, hx = -1, hy = -1, lx = -1, ly = -1;
    char path[200] = "";
    char tbuf[80];

    PixelApp(const char* p) {
        memset(px, 0, sizeof(px));
        if (p) load(p);
        else seed();
    }
    ~PixelApp() { for (int i = 0; i < undo.len; i++) kfree(undo[i]); undo.free(); }

    void seed() {
        // a little starter: our chip, so the canvas isn't intimidating
        for (int y = 16; y < 32; y++) for (int x = 24; x < 40; x++) px[y * PW + x] = 29;
        for (int y = 20; y < 28; y++) for (int x = 28; x < 36; x++) px[y * PW + x] = 20;
        for (int i = 0; i < 4; i++) {
            int ox = 26 + i * 4, oy = 18 + i * 4;
            px[14 * PW + ox] = px[15 * PW + ox] = px[32 * PW + ox] = px[33 * PW + ox] = 24;
            px[oy * PW + 22] = px[oy * PW + 23] = px[oy * PW + 40] = px[oy * PW + 41] = 24;
        }
        px[17 * PW + 25] = 26;
    }

    void load(const char* p) {
        FsNode* f = fs_lookup(p);
        if (!f || f->size < 8 || memcmp(f->data, "PIX1", 4)) return;
        int w = f->data[4] | (f->data[5] << 8), h = f->data[6] | (f->data[7] << 8);
        memset(px, 0, sizeof(px));
        for (int y = 0; y < min(h, PH); y++)
            for (int x = 0; x < min(w, PW); x++) px[y * PW + x] = f->data[8 + y * w + x] & 31;
        fs_path(f, path, sizeof(path));
        modified = false;
        set_title(title());
    }
    bool save_to(const char* p) {
        FsNode* f = fs_lookup(p);
        if (!f) f = fs_create_path(p, false);
        if (!f || f->dir) { info("can't save there."); return false; }
        u8* buf = (u8*)kmalloc(8 + PW * PH);
        memcpy(buf, "PIX1", 4);
        buf[4] = PW; buf[5] = 0; buf[6] = PH; buf[7] = 0;
        memcpy(buf + 8, px, PW * PH);
        fs_write(f, buf, 8 + PW * PH);
        kfree(buf);
        fs_path(f, path, sizeof(path));
        modified = false;
        set_title(title());
        ui_notify("Painting saved", path, "pixel");
        return true;
    }

    const char* name() { const char* s = strrchr(path, '/'); return path[0] ? (s ? s + 1 : path) : "untitled.pix"; }
    const char* app_id() override { return "pixel"; }
    const char* title() override {
        snprintf(tbuf, sizeof(tbuf), "Pixel " G_BULLET " %s%s", name(), modified ? " " G_DIAM : "");
        return tbuf;
    }
    int pref_w() override { return CX + PW * Z + 12; }
    int pref_h() override { return CY + PH * Z + 52; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFFE4DEF0; }

    void push_undo() {
        u8* s = (u8*)kmalloc(sizeof(px));
        memcpy(s, px, sizeof(px));
        undo.push(s);
        if (undo.len > 24) { kfree(undo[0]); undo.remove(0); }
    }
    void set(int x, int y, int c) {
        if (x < 0 || y < 0 || x >= PW || y >= PH) return;
        px[y * PW + x] = (u8)c;
        if (mirror) px[y * PW + (PW - 1 - x)] = (u8)c;
        modified = true;
    }
    void line(int x0, int y0, int x1, int y1, int c) {
        int dx = iabs(x1 - x0), sx2 = x0 < x1 ? 1 : -1, dy = -iabs(y1 - y0), sy2 = y0 < y1 ? 1 : -1, err = dx + dy;
        for (;;) {
            set(x0, y0, c);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx2; }
            if (e2 <= dx) { err += dx; y0 += sy2; }
        }
    }
    void flood(int x, int y, int c) {
        int target = px[y * PW + x];
        if (target == c) return;
        Vec<int> stack;
        stack.push(y * PW + x);
        while (stack.len) {
            int p = stack.back();
            stack.len--;
            if (px[p] != target) continue;
            px[p] = (u8)c;
            int xx = p % PW, yy = p / PW;
            if (xx > 0) stack.push(p - 1);
            if (xx < PW - 1) stack.push(p + 1);
            if (yy > 0) stack.push(p - PW);
            if (yy < PH - 1) stack.push(p + PW);
        }
        stack.free();
        modified = true;
    }

    // ------------------------------------------------------------ drawing
    Rect tool_rect(int i) { return {8 + (i % 2) * 34, 10 + (i / 2) * 34, 30, 30}; }
    Rect swatch(int i) { return {CX + i * 14, CY + PH * Z + 8, 14, 14}; }

    void draw_tool_glyph(Canvas& c, int t, int x, int y, u32 col) {
        static const char* glyphs[6][8] = {
            {"......##", ".....#.#", "....#.#.", "...#.#..", "..#.#...", ".##.....", ".##.....", "#......."},
            {"........", "...####.", "..#...#.", ".#...#..", "####.#..", "#..##...", "####....", "........"},
            {"..#.....", ".#.#....", "#...#...", "#....#..", ".#...##.", "..#.#.##", "...#..##", "......#."},
            {"#.......", ".#......", "..#.....", "...#....", "....#...", ".....#..", "......#.", ".......#"},
            {"########", "#......#", "#......#", "#......#", "#......#", "#......#", "#......#", "########"},
            {".....##.", "....####", "...#####", "..#.##..", ".#.#....", "#.#.....", "##......", "#......."},
        };
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 8; xx++)
                if (glyphs[t][yy][xx] == '#') c.fill(x + xx * 2, y + yy * 2, 2, 2, col);
    }

    void draw(Canvas& c, int w, int h) override {
        // tools
        const char* names[6] = {"pencil", "eraser", "fill", "line", "rect", "picker"};
        for (int i = 0; i < 6; i++) {
            Rect r = tool_rect(i);
            bool on = tool == i;
            draw_bevel(c, r.x, r.y, r.w, r.h, on ? accent() : (hot(r.x, r.y, r.w, r.h) ? blend(th::panel, accent(), 50) : th::panel), on);
            draw_tool_glyph(c, i, r.x + 7, r.y + 7, on ? th::white : th::ink);
        }
        c.text(8, 116, names[tool], th::muted, 1, true);
        // current color
        draw_bevel(c, 8, 136, 70, 40, th::white, true);
        c.fill(11, 139, 64, 34, pix_palette[color]);
        // toggles
        draw_checkbox(c, 8, 190, "grid", grid);
        draw_checkbox(c, 8, 212, "mirror", mirror);
        draw_button(c, 8, 240, 70, 22, "undo", hot(8, 240, 70, 22), false, undo.len > 0);
        draw_button(c, 8, 268, 70, 22, "save", hot(8, 268, 70, 22), true);
        draw_button(c, 8, 296, 70, 22, "backdrop", hot(8, 296, 70, 22));
        // canvas with a checker under color 0? no: 0 is paper white
        c.rect(CX - 2, CY - 2, PW * Z + 4, PH * Z + 4, th::ink);
        for (int y = 0; y < PH; y++)
            for (int x = 0; x < PW; x++) c.fill(CX + x * Z, CY + y * Z, Z, Z, pix_palette[px[y * PW + x]]);
        if (grid) {
            for (int x = 1; x < PW; x++) for (int y = 0; y < PH * Z; y += 2) c.put(CX + x * Z, CY + y, blend(c.get(CX + x * Z, CY + y), 0xFF000000, 30));
            for (int y = 1; y < PH; y++) for (int x = 0; x < PW * Z; x += 2) c.put(CX + x, CY + y * Z, blend(c.get(CX + x, CY + y * Z), 0xFF000000, 30));
        }
        if (mirror) for (int y = 0; y < PH * Z; y += 3) c.put(CX + PW * Z / 2, CY + y, th::red);
        // shape preview
        if (drawing && (tool == T_LINE || tool == T_RECT) && hx >= 0) {
            u32 pc = pix_palette[tool == T_ERASER ? 0 : color];
            auto pv = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= PW || y >= PH) return;
                c.fill(CX + x * Z + 1, CY + y * Z + 1, Z - 2, Z - 2, pc);
            };
            if (tool == T_LINE) {
                int x0 = sx, y0 = sy, x1 = hx, y1 = hy;
                int dx = iabs(x1 - x0), s1 = x0 < x1 ? 1 : -1, dy = -iabs(y1 - y0), s2 = y0 < y1 ? 1 : -1, err = dx + dy;
                for (;;) { pv(x0, y0); if (x0 == x1 && y0 == y1) break; int e2 = 2 * err; if (e2 >= dy) { err += dy; x0 += s1; } if (e2 <= dx) { err += dx; y0 += s2; } }
            } else {
                for (int x = min(sx, hx); x <= max(sx, hx); x++) { pv(x, sy); pv(x, hy); }
                for (int y = min(sy, hy); y <= max(sy, hy); y++) { pv(sx, y); pv(hx, y); }
            }
        }
        // hover outline
        if (hx >= 0 && hy >= 0) c.rect(CX + hx * Z - 1, CY + hy * Z - 1, Z + 2, Z + 2, th::ink);
        // palette
        for (int i = 0; i < 32; i++) {
            Rect r = swatch(i);
            c.fill(r.x, r.y, r.w, r.h, pix_palette[i]);
            c.rect(r.x, r.y, r.w, r.h, i == color ? th::ink : 0xFF9C92B8);
            if (i == color) c.rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, th::white);
        }
        char st[64];
        if (hx >= 0) snprintf(st, sizeof(st), "%d, %d", hx, hy);
        else snprintf(st, sizeof(st), "64 x 48");
        c.text(CX, h - 16, st, th::muted);
        c.text(w - 230, h - 16, "p e f l r i  " G_BULLET "  ctrl+z undo", th::muted);
    }

    bool canvas_pos(int x, int y, int& px_, int& py_) {
        px_ = (x - CX) / Z; py_ = (y - CY) / Z;
        return x >= CX && y >= CY && px_ < PW && py_ < PH;
    }

    void apply_at(int x, int y) {
        int c = tool == T_ERASER ? 0 : color;
        if (tool == T_PENCIL || tool == T_ERASER) {
            if (lx >= 0) line(lx, ly, x, y, c);
            else set(x, y, c);
            lx = x; ly = y;
        }
    }

    void on_mouse(const MouseEv& e) override {
        int x, y;
        bool in = canvas_pos(e.x, e.y, x, y);
        if (in) { hx = x; hy = y; } else if (!drawing) { hx = hy = -1; }
        if ((e.kind == M_DOWN || e.kind == M_DBL) && (e.button == 1 || e.button == 2)) {
            for (int i = 0; i < 6; i++) if (tool_rect(i).contains(e.x, e.y)) tool = i;
            for (int i = 0; i < 32; i++) if (swatch(i).contains(e.x, e.y)) color = i;
            if (Rect{8, 190, 70, 16}.contains(e.x, e.y)) grid = !grid;
            if (Rect{8, 212, 70, 16}.contains(e.x, e.y)) mirror = !mirror;
            if (in) {
                if (tool == T_PICK || e.button == 2) { color = px[y * PW + x]; if (tool == T_PICK) tool = T_PENCIL; }
                else {
                    push_undo();
                    drawing = true;
                    sx = x; sy = y;
                    lx = ly = -1;
                    if (tool == T_FILL) { flood(x, y, color); drawing = false; }
                    else apply_at(x, y);
                }
            }
        } else if (e.kind == M_MOVE && drawing && in) {
            apply_at(x, y);
        } else if (e.kind == M_UP) {
            if (Rect{8, 240, 70, 22}.contains(e.x, e.y)) on_command(P_UNDO);
            if (Rect{8, 268, 70, 22}.contains(e.x, e.y)) on_command(P_SAVE);
            if (Rect{8, 296, 70, 22}.contains(e.x, e.y)) on_command(P_WALL);
            if (drawing && hx >= 0) {
                int c = color;
                if (tool == T_LINE) line(sx, sy, hx, hy, c);
                if (tool == T_RECT) {
                    for (int xx = min(sx, hx); xx <= max(sx, hx); xx++) { set(xx, sy, c); set(xx, hy, c); }
                    for (int yy = min(sy, hy); yy <= max(sy, hy); yy++) { set(sx, yy, c); set(hx, yy, c); }
                }
            }
            drawing = false;
            lx = ly = -1;
            set_title(title());
        }
        ui_invalidate();
    }

    void on_key(const KeyEvent& e) override {
        if (e.ctrl && e.key == 'z') on_command(P_UNDO);
        else if (e.ctrl && e.key == 's') on_command(P_SAVE);
        else if (e.ctrl && e.key == 'o') on_command(P_OPEN);
        else if (!e.ctrl) {
            switch (e.key) {
            case 'p': tool = T_PENCIL; break;
            case 'e': tool = T_ERASER; break;
            case 'f': tool = T_FILL; break;
            case 'l': tool = T_LINE; break;
            case 'r': tool = T_RECT; break;
            case 'i': tool = T_PICK; break;
            case 'g': grid = !grid; break;
            case 'm': mirror = !mirror; break;
            case '[': color = (color + 31) % 32; break;
            case ']': color = (color + 1) % 32; break;
            }
        }
        ui_invalidate();
    }

    static void save_as_cb(App* a, const char* text) {
        PixelApp* p = (PixelApp*)a;
        char path[200];
        if (text[0] == '/') strlcpy(path, text, sizeof(path));
        else snprintf(path, sizeof(path), "/home/pictures/%s", text);
        if (!str_icontains(path, ".pix")) strlcat(path, ".pix", sizeof(path));
        p->save_to(path);
    }
    static void open_cb(App* a, const char* text) {
        PixelApp* p = (PixelApp*)a;
        char path[200];
        if (text[0] == '/') strlcpy(path, text, sizeof(path));
        else snprintf(path, sizeof(path), "/home/pictures/%s", text);
        if (!fs_lookup(path)) { p->info("no such painting."); return; }
        p->load(path);
    }
    static void close_cb(App* a, int choice) {
        PixelApp* p = (PixelApp*)a;
        if (choice == 0) {
            if (p->path[0]) { if (p->save_to(p->path)) p->force_close(); }
            else p->prompt("save in /home/pictures as:", "painting.pix", save_as_cb);
        } else if (choice == 1) p->force_close();
    }
    bool can_close() override {
        if (!modified) return true;
        ask("save this painting before closing?", "Save", "Don't save", "Cancel", close_cb);
        return false;
    }

    void on_command(int cmd) override {
        switch (cmd) {
        case P_NEW: launch("__new__", "pixel"); break;
        case P_OPEN: prompt("open painting (in /home/pictures):", "painting.pix", open_cb); break;
        case P_SAVE: if (path[0]) save_to(path); else prompt("save in /home/pictures as:", "painting.pix", save_as_cb); break;
        case P_SAVE_AS: prompt("save in /home/pictures as:", name(), save_as_cb); break;
        case P_WALL: {
            Image img = image_new(fb.width, fb.height, 0);
            for (int y = 0; y < fb.height; y++)
                for (int x = 0; x < fb.width; x++) img.px[y * fb.width + x] = pix_palette[px[(y * PH / fb.height) * PW + x * PW / fb.width]];
            int i = wallpaper_add_custom(name(), img);
            wallpaper_set(i);
            ui_notify("Wallpaper set", "your painting is now the backdrop", "pixel");
            break;
        }
        case P_UNDO:
            if (undo.len) { memcpy(px, undo.back(), sizeof(px)); kfree(undo.back()); undo.len--; modified = true; }
            break;
        case P_CLEAR: push_undo(); memset(px, 0, sizeof(px)); modified = true; break;
        case P_GRID: grid = !grid; break;
        case P_MIRROR: mirror = !mirror; break;
        case P_CLOSE: close(); break;
        }
        ui_invalidate();
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "File", 24);
        m[0].add("New", P_NEW).add("Open" G_ELLIP, P_OPEN, "ctrl+o").sep().add("Save", P_SAVE, "ctrl+s")
            .add("Save as" G_ELLIP, P_SAVE_AS).sep().add("Use as wallpaper", P_WALL).sep().add("Close", P_CLOSE, "ctrl+w");
        m[1].clear(); strlcpy(m[1].title, "Edit", 24);
        m[1].add("Undo", P_UNDO, "ctrl+z", undo.len > 0).add("Clear canvas", P_CLEAR);
        m[2].clear(); strlcpy(m[2].title, "View", 24);
        m[2].add("Grid", P_GRID, "g", true, grid).add("Mirror drawing", P_MIRROR, "m", true, mirror);
        return 3;
    }
};

App* new_pixel(const char* arg) { return new PixelApp(arg); }
