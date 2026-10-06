// silicon - utilities: Calendar, Tasks, Hex viewer, Storage, Convert, Palette
#include "apps.h"

static const int BAR = 34;

static void toolbar(Canvas& c, int w, const char* icon_id, const char* label) {
    c.fill(0, 0, w, BAR, th::panel);
    c.hline(0, BAR - 1, w, th::border);
    c.blit(icon(icon_id)->small, 10, 9);
    c.text(34, 11, label, th::text, 1, true);
}

// ================================================================ Calendar
struct CalendarApp : App {
    struct Ev { int y, m, d; char text[72]; };
    Ev evs[300];
    int nev = 0;
    int year = 2026, month = 1, sel = 1, today_y = 0, today_m = 0, today_d = 0;
    static const int CELL_W = 56, CELL_H = 52, GX = 12, GY = BAR + 54, SIDE = GX + 7 * CELL_W + 14;

    CalendarApp() {
        DateTime d;
        rtc_read(d);
        today_y = year = d.year; today_m = month = d.month; today_d = sel = d.day;
        load();
    }
    const char* app_id() override { return "calendar"; }
    const char* title() override { return "Calendar"; }
    int pref_w() override { return SIDE + 230; }
    int pref_h() override { return GY + 6 * CELL_H + 14; }
    bool resizable() override { return false; }

    static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
    static int dim(int y, int m) { static const int t[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; return m == 2 && leap(y) ? 29 : t[m - 1]; }
    static int weekday(int y, int m, int d) {          // 0 = Monday
        static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
        if (m < 3) y--;
        int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;   // 0 = Sunday
        return (w + 6) % 7;
    }
    void load() {
        nev = 0;
        FsNode* f = fs_lookup("/home/.config/calendar");
        if (!f) return;
        const char* t = fs_read_text(f);
        while (*t && nev < 300) {
            int y = atoi(t), m = 0, d = 0;
            const char* p = strchr(t, '-');
            if (p) { m = atoi(p + 1); p = strchr(p + 1, '-'); }
            if (p) { d = atoi(p + 1); p = strchr(p, '|'); }
            const char* e = strchr(t, '\n');
            if (p && y > 0) {
                Ev& ev = evs[nev++];
                ev.y = y; ev.m = m; ev.d = d;
                int l = (int)((e ? e : t + strlen(t)) - (p + 1));
                l = min(l, 71);
                memcpy(ev.text, p + 1, l);
                ev.text[l] = 0;
            }
            if (!e) break;
            t = e + 1;
        }
    }
    void save() {
        char* buf = (char*)kmalloc(nev * 90 + 4);
        int o = 0;
        buf[0] = 0;
        for (int i = 0; i < nev; i++) o += snprintf(buf + o, 90, "%04d-%02d-%02d|%s\n", evs[i].y, evs[i].m, evs[i].d, evs[i].text);
        fs_mkdirs("/home/.config");
        fs_write_text("/home/.config/calendar", buf);
        kfree(buf);
    }
    int count_on(int y, int m, int d) { int n = 0; for (int i = 0; i < nev; i++) n += evs[i].y == y && evs[i].m == m && evs[i].d == d; return n; }
    void shift(int dm) {
        month += dm;
        while (month < 1) { month += 12; year--; }
        while (month > 12) { month -= 12; year++; }
        sel = min(sel, dim(year, month));
    }
    Rect cell(int d) {
        int idx = weekday(year, month, 1) + d - 1;
        return {GX + (idx % 7) * CELL_W, GY + (idx / 7) * CELL_H, CELL_W - 2, CELL_H - 2};
    }
    void draw(Canvas& c, int w, int h) override {
        static const char* names[12] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
        static const char* wd[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        toolbar(c, w, "calendar", "calendar");
        draw_button(c, 100, 6, 26, 22, "<", hot(100, 6, 26, 22));
        draw_button(c, 130, 6, 26, 22, ">", hot(130, 6, 26, 22));
        draw_button(c, 160, 6, 60, 22, "Today", hot(160, 6, 60, 22));
        char b[48];
        snprintf(b, sizeof(b), "%s %d", names[month - 1], year);
        c.text(GX, BAR + 8, b, th::text, 2, true);
        for (int i = 0; i < 7; i++) c.text(GX + i * CELL_W + 18, GY - 16, wd[i], i >= 5 ? th::red : th::muted, 1, true);
        for (int d = 1; d <= dim(year, month); d++) {
            Rect r = cell(d);
            bool istoday = year == today_y && month == today_m && d == today_d;
            bool on = hot(r.x, r.y, r.w, r.h);
            u32 bg = d == sel ? blend(th::paper, accent(), 120) : on ? th::panel : th::white;
            c.fill(r.x, r.y, r.w, r.h, bg);
            c.rect(r.x, r.y, r.w, r.h, istoday ? accent() : 0xFFD8D2E8);
            if (istoday) c.rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, accent());
            snprintf(b, sizeof(b), "%d", d);
            c.text(r.x + 6, r.y + 6, b, d == sel ? th::white : th::text, 1, true);
            int n = count_on(year, month, d);
            for (int k = 0; k < min(n, 4); k++) c.fill(r.x + 6 + k * 8, r.y + r.h - 12, 6, 6, th::gold_dk);
        }
        // events of the selected day
        int x0 = SIDE;
        c.vline(x0 - 8, BAR, h - BAR, th::border);
        snprintf(b, sizeof(b), "%d %s", sel, names[month - 1]);
        c.text(x0, BAR + 10, b, th::text, 2, true);
        draw_button(c, x0, BAR + 40, 90, 22, "Add event", hot(x0, BAR + 40, 90, 22), true);
        int y = BAR + 74, shown = 0;
        for (int i = 0; i < nev; i++) {
            if (evs[i].y != year || evs[i].m != month || evs[i].d != sel) continue;
            c.fill(x0, y, 210, 22, th::panel);
            c.text_n(x0 + 6, y + 6, evs[i].text, 22, th::text);
            c.text(x0 + 196, y + 6, "x", hot(x0 + 190, y, 20, 22) ? th::red : th::muted, 1, true);
            y += 26; shown++;
        }
        if (!shown) c.text(x0, y, "nothing planned", th::muted);
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind != M_UP || e.button != 1) return;
        if (e.y < BAR) {
            if (Rect{100, 6, 26, 22}.contains(e.x, e.y)) shift(-1);
            else if (Rect{130, 6, 26, 22}.contains(e.x, e.y)) shift(1);
            else if (Rect{160, 6, 60, 22}.contains(e.x, e.y)) { year = today_y; month = today_m; sel = today_d; }
            ui_invalidate();
            return;
        }
        for (int d = 1; d <= dim(year, month); d++) if (cell(d).contains(e.x, e.y)) sel = d;
        int x0 = SIDE;
        if (Rect{x0, BAR + 40, 90, 22}.contains(e.x, e.y)) add_prompt();
        int y = BAR + 74;
        for (int i = 0; i < nev; i++) {
            if (evs[i].y != year || evs[i].m != month || evs[i].d != sel) continue;
            if (Rect{x0 + 190, y, 20, 22}.contains(e.x, e.y)) { for (int k = i; k + 1 < nev; k++) evs[k] = evs[k + 1]; nev--; save(); break; }
            y += 26;
        }
        ui_invalidate();
    }
    void add_prompt() {
        prompt("What's happening?", "", [](App* a, const char* t) {
            CalendarApp* s = (CalendarApp*)a;
            if (!t[0] || s->nev >= 300) return;
            Ev& ev = s->evs[s->nev++];
            ev.y = s->year; ev.m = s->month; ev.d = s->sel;
            strlcpy(ev.text, t, sizeof(ev.text));
            for (char* p = ev.text; *p; p++) if (*p == '|' || *p == '\n') *p = ' ';
            s->save();
            ui_invalidate();
        });
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        int dm = dim(year, month);
        if (e.key == K_LEFT) sel--;
        else if (e.key == K_RIGHT) sel++;
        else if (e.key == K_UP) sel -= 7;
        else if (e.key == K_DOWN) sel += 7;
        else if (e.key == K_PGUP) shift(-1);
        else if (e.key == K_PGDN) shift(1);
        else if (e.key == 'a' || e.key == K_ENTER) add_prompt();
        if (sel < 1) { shift(-1); sel = dim(year, month) + sel; if (sel < 1) sel = 1; }
        else if (sel > dm) { sel -= dm; shift(1); }
        ui_invalidate();
    }
};

// ================================================================ Tasks
struct TasksApp : App {
    struct Task { char text[80]; bool done; };
    Task tasks[120];
    int n = 0;
    TextField field;
    Smooth sc;
    ScrollBar sb;
    int ch = 0;
    static const int ROW = 28;

    TasksApp() {
        field.focused = true;
        FsNode* f = fs_lookup("/home/.config/tasks");
        if (f) {
            const char* t = fs_read_text(f);
            while (*t && n < 120) {
                const char* e = strchr(t, '\n');
                int l = e ? (int)(e - t) : (int)strlen(t);
                if (l > 2) { tasks[n].done = t[0] == '1'; int k = min(l - 2, 79); memcpy(tasks[n].text, t + 2, k); tasks[n].text[k] = 0; n++; }
                if (!e) break;
                t = e + 1;
            }
        }
    }
    const char* app_id() override { return "tasks"; }
    const char* title() override { return "Tasks"; }
    int pref_w() override { return 420; }
    int pref_h() override { return 460; }
    void save() {
        char* buf = (char*)kmalloc(n * 90 + 4);
        int o = 0;
        buf[0] = 0;
        for (int i = 0; i < n; i++) o += snprintf(buf + o, 90, "%d|%s\n", tasks[i].done ? 1 : 0, tasks[i].text);
        fs_mkdirs("/home/.config");
        fs_write_text("/home/.config/tasks", buf);
        kfree(buf);
    }
    int list_top() { return BAR + 40; }
    int content_h() { return n * ROW + 8; }
    void tick() override { if (sc.step()) ui_invalidate(); }
    void add() {
        if (!field.len || n >= 120) return;
        strlcpy(tasks[n].text, field.text, 80);
        tasks[n++].done = false;
        field.set("");
        save();
    }
    void draw(Canvas& c, int w, int h) override {
        ch = h;
        int left = 0;
        for (int i = 0; i < n; i++) left += !tasks[i].done;
        toolbar(c, w, "tasks", "tasks");
        char b[40];
        snprintf(b, sizeof(b), "%d to do", left);
        c.text(w - text_width(b) - 12, 11, b, th::muted);
        field.focused = true;
        field.draw(c, 12, BAR + 8, w - 24 - 70);
        draw_button(c, w - 76, BAR + 7, 64, 22, "Add", hot(w - 76, BAR + 7, 64, 22), true, field.len > 0);
        int top = list_top();
        c.set_clip({0, top, w - 11, h - top});
        if (!n) c.text_centered(0, top + 60, w, "nothing to do. enjoy it!", th::muted, 1, true);
        for (int i = 0; i < n; i++) {
            int y = top + 4 + i * ROW - sc.pos();
            if (y + ROW < top || y > h) continue;
            bool on = hot(0, y, w - 11, ROW);
            if (on) c.fill(6, y, w - 23, ROW - 2, th::panel);
            draw_checkbox(c, 14, y + 6, "", tasks[i].done);
            c.text_n(42, y + 8, tasks[i].text, (w - 100) / 8, tasks[i].done ? th::muted : th::text);
            if (tasks[i].done) c.hline(42, y + 14, min((int)strlen(tasks[i].text) * 8, w - 100), th::muted);
            if (on) c.text(w - 38, y + 8, "x", th::red, 1, true);
        }
        c.set_clip({0, 0, w, h});
        sb.draw(c, w - 10, top, h - top, content_h(), h - top, sc.pos());
    }
    void on_mouse(const MouseEv& e) override {
        int top = list_top();
        int off = sc.target;
        if (e.kind == M_WHEEL) { sc.set(clamp(sc.target - e.wheel * 56, 0, max(0, content_h() - (ch - top)))); ui_invalidate(); return; }
        int w = win ? win->client().w : 420;
        if (sb.handle(e, w - 10, top, ch - top, content_h(), ch - top, off)) { sc.jump(off); ui_invalidate(); return; }
        if (e.y >= BAR && e.y < top) {
            if (e.kind == M_DOWN) field.click(e.x - 12, w - 24 - 70, kbd_shift_down());
            else if (e.kind == M_MOVE && e.left) field.drag(e.x - 12, w - 24 - 70);
            else if (e.kind == M_UP && Rect{w - 76, BAR + 7, 64, 22}.contains(e.x, e.y)) add();
            ui_invalidate();
            return;
        }
        if (e.kind == M_UP && e.button == 1 && e.y >= top) {
            int i = (e.y - top - 4 + sc.pos()) / ROW;
            if (i >= 0 && i < n) {
                if (e.x > w - 48) { for (int k = i; k + 1 < n; k++) tasks[k] = tasks[k + 1]; n--; }
                else tasks[i].done = !tasks[i].done;
                save();
            }
        }
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.key == K_ENTER) { add(); ui_invalidate(); return; }
        if (field.on_key(e)) ui_invalidate();
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "Tasks", 24);
        m[0].add("Clear completed", 1);
        return 1;
    }
    void on_command(int) override {
        int k = 0;
        for (int i = 0; i < n; i++) if (!tasks[i].done) tasks[k++] = tasks[i];
        n = k;
        save();
        ui_invalidate();
    }
};

// ================================================================ Hex viewer
struct HexApp : App {
    FsNode* file = nullptr;
    Smooth sc;
    ScrollBar sb;
    int ch = 0;
    static const int ROWH = 14;

    HexApp(const char* arg) { if (arg && arg[0]) open_arg(arg); }
    const char* app_id() override { return "hex"; }
    const char* title() override { return "Hex"; }
    int pref_w() override { return 700; }
    int pref_h() override { return 460; }
    u32 background() override { return 0xFF14101F; }
    void open_arg(const char* p) override {
        FsNode* f = fs_lookup(p);
        if (f && !f->dir) { file = f; sc.jump(0); ui_invalidate(); }
    }
    int rows() { return file ? (int)((file->size + 15) / 16) : 0; }
    int content_h() { return rows() * ROWH + 8; }
    void tick() override { if (sc.step()) ui_invalidate(); }
    void draw(Canvas& c, int w, int h) override {
        ch = h;
        toolbar(c, w, "hex", file ? file->name : "hex viewer");
        draw_button(c, w - 80, 6, 70, 22, "Open" G_ELLIP, hot(w - 80, 6, 70, 22));
        if (file) { char b[48]; fmt_bytes(file->size, b, sizeof(b)); c.text(w - 180 - text_width(b) + 90, 11, b, th::muted); }
        if (!file) { c.text_centered(0, h / 2 - 6, w, "open a file to see its bytes", 0xFF8A80B8, 1, true); return; }
        c.set_clip({0, BAR, w - 11, h - BAR});
        int first = max(0, sc.pos() / ROWH), last = min(rows(), (sc.pos() + h - BAR) / ROWH + 2);
        for (int r = first; r < last; r++) {
            int y = BAR + 4 + r * ROWH - sc.pos();
            char line[24];
            snprintf(line, sizeof(line), "%08x", r * 16);
            c.text(10, y, line, 0xFF8A7AD0);
            for (int i = 0; i < 16; i++) {
                u32 off = r * 16 + i;
                if (off >= file->size) break;
                u8 v = file->data[off];
                char hx[3] = {"0123456789abcdef"[v >> 4], "0123456789abcdef"[v & 15], 0};
                c.text(92 + i * 24 + (i >= 8 ? 8 : 0), y, hx, v == 0 ? 0xFF5A5278 : 0xFFE0D8F8);
                char as[2] = {(char)(v >= 32 && v < 127 ? v : '.'), 0};
                c.text(92 + 16 * 24 + 24 + i * 8, y, as, v >= 32 && v < 127 ? 0xFF9BFFB8 : 0xFF5A5278);
            }
        }
        c.set_clip({0, 0, w, h});
        sb.draw(c, w - 10, BAR, h - BAR, content_h(), h - BAR, sc.pos());
    }
    void on_mouse(const MouseEv& e) override {
        int w = win ? win->client().w : 700;
        int off = sc.target;
        if (file && sb.handle(e, w - 10, BAR, ch - BAR, content_h(), ch - BAR, off)) { sc.jump(off); ui_invalidate(); return; }
        if (e.kind == M_WHEEL) { sc.set(clamp(sc.target - e.wheel * 56, 0, max(0, content_h() - (ch - BAR)))); ui_invalidate(); return; }
        if (e.kind == M_UP && Rect{w - 80, 6, 70, 22}.contains(e.x, e.y))
            pick_file("Open file", "/home", "", false, "", [](App* a, const char* p) { a->open_arg(p); });
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down || !file) return;
        int page = ch - BAR - 20, mx = max(0, content_h() - (ch - BAR));
        if (e.key == K_DOWN) sc.set(min(mx, sc.target + ROWH * 3));
        else if (e.key == K_UP) sc.set(max(0, sc.target - ROWH * 3));
        else if (e.key == K_PGDN || e.key == ' ') sc.set(min(mx, sc.target + page));
        else if (e.key == K_PGUP) sc.set(max(0, sc.target - page));
        else if (e.key == K_HOME) sc.set(0);
        else if (e.key == K_END) sc.set(mx);
        ui_invalidate();
    }
};

// ================================================================ Storage
struct StorageApp : App {
    struct Item { FsNode* n; u32 size; };
    Vec<Item> tops, biggest;
    u32 total = 0;
    Smooth sc;
    ScrollBar sb;
    int ch = 0;

    StorageApp() { scan(); }
    ~StorageApp() { tops.free(); biggest.free(); }
    const char* app_id() override { return "storage"; }
    const char* title() override { return "Storage"; }
    int pref_w() override { return 480; }
    int pref_h() override { return 480; }
    void gather(FsNode* n) {
        for (int i = 0; i < n->children.len; i++) {
            FsNode* c = n->children[i];
            if (c->dir) { gather(c); continue; }
            biggest.push({c, c->size});
        }
    }
    void scan() {
        tops.clear(); biggest.clear();
        FsNode* r = fs_root();
        total = fs_tree_size(r);
        for (int i = 0; i < r->children.len; i++) tops.push({r->children[i], fs_tree_size(r->children[i])});
        for (int i = 0; i < tops.len; i++) for (int j = i + 1; j < tops.len; j++) if (tops[j].size > tops[i].size) swap(tops[i], tops[j]);
        gather(r);
        for (int i = 0; i < biggest.len; i++) for (int j = i + 1; j < biggest.len; j++) if (biggest[j].size > biggest[i].size) swap(biggest[i], biggest[j]);
        if (biggest.len > 14) biggest.len = 14;
    }
    int content_h() { return 110 + tops.len * 24 + 40 + biggest.len * 22 + 20; }
    void tick() override { if (sc.step()) ui_invalidate(); }
    void draw(Canvas& c, int w, int h) override {
        ch = h;
        toolbar(c, w, "storage", "storage");
        draw_button(c, w - 90, 6, 80, 22, "Rescan", hot(w - 90, 6, 80, 22));
        c.set_clip({0, BAR, w - 11, h - BAR});
        int y = BAR + 12 - sc.pos();
        u32 used = fs_disk_used(), cap = fs_disk_capacity();
        char a[40], b[40], line[100];
        fmt_bytes(total, a, sizeof(a));
        c.text(14, y, "files", th::muted);
        c.text(14, y + 14, a, th::text, 2, true);
        if (cap) {
            fmt_bytes(used, a, sizeof(a)); fmt_bytes(cap, b, sizeof(b));
            snprintf(line, sizeof(line), "disk region: %s of %s", a, b);
            c.text(w - 20 - text_width(line), y + 4, line, th::muted);
            draw_progress(c, 14, y + 40, w - 40, 12, (int)((u64)used * 100 / cap), accent());
        }
        y += 74;
        c.text(14, y, "by folder", th::text, 1, true);
        y += 22;
        for (int i = 0; i < tops.len; i++, y += 24) {
            fmt_bytes(tops[i].size, b, sizeof(b));
            c.text(14, y + 4, tops[i].n->name, th::text);
            int bw = total ? (int)((u64)tops[i].size * (w - 220) / total) : 0;
            c.fill(130, y + 2, max(2, bw), 14, tops[i].n->dir ? accent() : th::gold_dk);
            c.text(130 + max(2, bw) + 8, y + 4, b, th::muted);
        }
        y += 14;
        c.text(14, y, "largest files", th::text, 1, true);
        y += 22;
        for (int i = 0; i < biggest.len; i++, y += 22) {
            bool on = hot(0, y, w - 11, 22);
            if (on) c.fill(8, y, w - 27, 20, th::panel);
            char p[96];
            fs_path(biggest[i].n, p, sizeof(p));
            fmt_bytes(biggest[i].size, b, sizeof(b));
            c.text_n(14, y + 4, p, (w - 130) / 8, th::text);
            c.text(w - 20 - text_width(b), y + 4, b, th::muted);
        }
        c.set_clip({0, 0, w, h});
        sb.draw(c, w - 10, BAR, h - BAR, content_h(), h - BAR, sc.pos());
    }
    void on_mouse(const MouseEv& e) override {
        int w = win ? win->client().w : 480;
        int off = sc.target;
        if (sb.handle(e, w - 10, BAR, ch - BAR, content_h(), ch - BAR, off)) { sc.jump(off); ui_invalidate(); return; }
        if (e.kind == M_WHEEL) { sc.set(clamp(sc.target - e.wheel * 56, 0, max(0, content_h() - (ch - BAR)))); ui_invalidate(); return; }
        if (e.kind == M_UP && e.button == 1) {
            if (Rect{w - 90, 6, 80, 22}.contains(e.x, e.y)) { scan(); ui_invalidate(); return; }
            int y0 = BAR + 12 - sc.pos() + 74 + 22 + tops.len * 24 + 14 + 22;
            int i = (e.y - y0) / 22;
            if (e.y >= y0 && i >= 0 && i < biggest.len) { char p[200]; fs_path(biggest[i].n, p, sizeof(p)); open_path(p); }
        }
    }
};

// ================================================================ Convert
struct ConvertApp : App {
    struct Unit { const char* name; double f; };
    struct Cat { const char* name; const Unit* units; int n; };
    static const Unit len_u[8], mass_u[6], data_u[6], time_u[7], speed_u[5], temp_u[3], area_u[6];
    int cat = 0, from = 0, to = 1;
    TextField in;
    char out[48] = "";
    char cat_names[8][12] = {"Length", "Mass", "Temp", "Data", "Time", "Speed", "Area"};

    ConvertApp() { in.set("1"); in.focused = true; in.select_all(); }
    const char* app_id() override { return "convert"; }
    const char* title() override { return "Convert"; }
    int pref_w() override { return 460; }
    int pref_h() override { return 360; }
    Cat get(int i) {
        switch (i) {
        case 0: return {"Length", len_u, 8};
        case 1: return {"Mass", mass_u, 6};
        case 2: return {"Temp", temp_u, 3};
        case 3: return {"Data", data_u, 6};
        case 4: return {"Time", time_u, 7};
        case 5: return {"Speed", speed_u, 5};
        default: return {"Area", area_u, 6};
        }
    }
    static double parse(const char* s) {
        double v = 0, f = 0.1;
        bool neg = false, frac = false;
        if (*s == '-') { neg = true; s++; }
        for (; *s; s++) {
            if (*s == '.') frac = true;
            else if (*s >= '0' && *s <= '9') { if (!frac) v = v * 10 + (*s - '0'); else { v += (*s - '0') * f; f /= 10; } }
        }
        return neg ? -v : v;
    }
    void compute() {
        Cat k = get(cat);
        from = min(from, k.n - 1); to = min(to, k.n - 1);
        double v = parse(in.text), r;
        if (cat == 2) {
            double cel = from == 0 ? v : from == 1 ? (v - 32) * 5.0 / 9.0 : v - 273.15;
            r = to == 0 ? cel : to == 1 ? cel * 9.0 / 5.0 + 32 : cel + 273.15;
        } else r = v * k.units[from].f / k.units[to].f;
        fmt_double(r, out, sizeof(out), 6);
    }
    Rect cat_rect(int i) { return {12 + i * 62, 8, 58, 22}; }
    Rect unit_rect(int side, int i) { return {12 + side * 226, 120 + i * 24, 210, 22}; }
    void draw(Canvas& c, int w, int h) override {
        compute();
        c.fill(0, 0, w, 38, th::panel);
        c.hline(0, 37, w, th::border);
        for (int i = 0; i < 7; i++) { Rect r = cat_rect(i); draw_button(c, r.x, r.y, r.w, r.h, cat_names[i], hot(r.x, r.y, r.w, r.h), cat == i); }
        in.focused = true;
        in.draw(c, 12, 50, w - 24);
        c.text(12, 82, "from", th::muted);
        c.text(238, 82, "to", th::muted);
        Cat k = get(cat);
        for (int s = 0; s < 2; s++)
            for (int i = 0; i < k.n; i++) {
                Rect r = unit_rect(s, i);
                bool sel = (s == 0 ? from : to) == i;
                bool on = hot(r.x, r.y, r.w, r.h);
                c.fill(r.x, r.y, r.w, r.h, sel ? accent() : on ? th::panel : th::white);
                c.rect(r.x, r.y, r.w, r.h, th::border);
                c.text(r.x + 8, r.y + 6, k.units[i].name, sel ? th::white : th::text, 1, sel);
            }
        char b[120];
        snprintf(b, sizeof(b), "%s %s = %s %s", in.text, k.units[from].name, out, k.units[to].name);
        c.text_centered(0, 100 - 2 + 8 * 24 + 14, w, b, th::text, 1, true);
        draw_button(c, w / 2 - 50, h - 34, 100, 24, "Copy result", hot(w / 2 - 50, h - 34, 100, 24));
    }
    void on_mouse(const MouseEv& e) override {
        int w = win ? win->client().w : 460, h = win ? win->client().h : 360;
        if (e.y >= 44 && e.y < 76) {
            if (e.kind == M_DOWN) in.click(e.x - 12, w - 24, kbd_shift_down());
            else if (e.kind == M_MOVE && e.left) in.drag(e.x - 12, w - 24);
            ui_invalidate();
            return;
        }
        if (e.kind != M_UP || e.button != 1) return;
        for (int i = 0; i < 7; i++) if (cat_rect(i).contains(e.x, e.y)) { cat = i; from = 0; to = 1; }
        Cat k = get(cat);
        for (int s = 0; s < 2; s++) for (int i = 0; i < k.n; i++) if (unit_rect(s, i).contains(e.x, e.y)) (s == 0 ? from : to) = i;
        if (Rect{w / 2 - 50, h - 34, 100, 24}.contains(e.x, e.y)) clip_set(out);
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (in.on_key(e)) ui_invalidate();
    }
};
const ConvertApp::Unit ConvertApp::len_u[8] = {{"millimetre", 0.001}, {"centimetre", 0.01}, {"metre", 1}, {"kilometre", 1000}, {"inch", 0.0254}, {"foot", 0.3048}, {"yard", 0.9144}, {"mile", 1609.344}};
const ConvertApp::Unit ConvertApp::mass_u[6] = {{"gram", 0.001}, {"kilogram", 1}, {"tonne", 1000}, {"ounce", 0.028349523}, {"pound", 0.45359237}, {"stone", 6.35029318}};
const ConvertApp::Unit ConvertApp::temp_u[3] = {{"celsius", 1}, {"fahrenheit", 1}, {"kelvin", 1}};
const ConvertApp::Unit ConvertApp::data_u[6] = {{"byte", 1}, {"kilobyte", 1024}, {"megabyte", 1048576}, {"gigabyte", 1073741824.0}, {"terabyte", 1099511627776.0}, {"bit", 0.125}};
const ConvertApp::Unit ConvertApp::time_u[7] = {{"millisecond", 0.001}, {"second", 1}, {"minute", 60}, {"hour", 3600}, {"day", 86400}, {"week", 604800}, {"year", 31557600}};
const ConvertApp::Unit ConvertApp::speed_u[5] = {{"metre/s", 1}, {"km/h", 0.277777778}, {"mile/h", 0.44704}, {"knot", 0.514444444}, {"foot/s", 0.3048}};
const ConvertApp::Unit ConvertApp::area_u[6] = {{"square metre", 1}, {"square km", 1e6}, {"hectare", 1e4}, {"acre", 4046.8564}, {"square foot", 0.09290304}, {"square mile", 2589988.11}};

// ================================================================ Palette
struct PaletteApp : App {
    int hue = 260, sat = 180, val = 220;
    bool drag_sv = false, drag_h = false;
    u32 saved[24];
    int nsaved = 0;
    static const int SVX = 14, SVY = BAR + 14, SVS = 200, HX = SVX + SVS + 12, HW = 22;

    PaletteApp() {
        FsNode* f = fs_lookup("/home/.config/palette");
        if (f) {
            const char* t = fs_read_text(f);
            while (*t && nsaved < 24) {
                saved[nsaved++] = 0xFF000000 | (u32)strtoul_hex(t);
                const char* e = strchr(t, '\n');
                if (!e) break;
                t = e + 1;
            }
        }
    }
    static u32 strtoul_hex(const char* s) {
        u32 v = 0;
        for (; *s && *s != '\n'; s++) {
            char ch = *s;
            int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + d;
        }
        return v;
    }
    const char* app_id() override { return "palette"; }
    const char* title() override { return "Palette"; }
    int pref_w() override { return 520; }
    int pref_h() override { return 320; }
    bool resizable() override { return false; }
    static u32 hsv(int h, int s, int v) {
        h = ((h % 360) + 360) % 360;
        int region = h / 60, rem = (h % 60) * 255 / 60;
        int p = v * (255 - s) / 255, q = v * (255 - s * rem / 255) / 255, t = v * (255 - s * (255 - rem) / 255) / 255;
        int r, g, b;
        switch (region) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
                          case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; }
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    }
    void set_from_rgb(u32 c) {
        int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        int mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
        val = mx; sat = mx ? d * 255 / mx : 0;
        if (!d) { return; }
        int h;
        if (mx == r) h = 60 * (g - b) / d; else if (mx == g) h = 120 + 60 * (b - r) / d; else h = 240 + 60 * (r - g) / d;
        hue = (h + 360) % 360;
    }
    u32 cur() { return hsv(hue, sat, val); }
    void save_all() {
        char buf[24 * 10 + 4];
        int o = 0;
        buf[0] = 0;
        for (int i = 0; i < nsaved; i++) o += snprintf(buf + o, 12, "%06x\n", saved[i] & 0xFFFFFF);
        fs_mkdirs("/home/.config");
        fs_write_text("/home/.config/palette", buf);
    }
    Rect swatch(int i) { return {SVX + SVS + HW + 40 + (i % 8) * 28, BAR + 190 + (i / 8) * 28, 24, 24}; }
    void draw(Canvas& c, int w, int h) override {
        toolbar(c, w, "palette", "palette");
        for (int y = 0; y < SVS; y += 4)
            for (int x = 0; x < SVS; x += 4) c.fill(SVX + x, SVY + y, 4, 4, hsv(hue, x * 255 / SVS, 255 - y * 255 / SVS));
        c.rect(SVX - 1, SVY - 1, SVS + 2, SVS + 2, th::border);
        int mx = SVX + sat * SVS / 255, my = SVY + (255 - val) * SVS / 255;
        c.circle(mx, my, 5, th::white, false); c.circle(mx, my, 6, th::ink, false);
        for (int y = 0; y < SVS; y += 2) c.fill(HX, SVY + y, HW, 2, hsv(y * 360 / SVS, 255, 255));
        c.rect(HX - 1, SVY - 1, HW + 2, SVS + 2, th::border);
        int hy = SVY + hue * SVS / 360;
        c.rect(HX - 3, hy - 2, HW + 6, 5, th::ink); c.rect(HX - 2, hy - 1, HW + 4, 3, th::white);
        u32 col = cur();
        int rx = HX + HW + 22;
        c.fill(rx, SVY, w - rx - 14, 70, col);
        c.rect(rx, SVY, w - rx - 14, 70, th::border);
        char b[48];
        snprintf(b, sizeof(b), "#%06x", col & 0xFFFFFF);
        c.text(rx, SVY + 80, b, th::text, 2, true);
        snprintf(b, sizeof(b), "rgb %d, %d, %d", (col >> 16) & 255, (col >> 8) & 255, col & 255);
        c.text(rx, SVY + 104, b, th::muted);
        snprintf(b, sizeof(b), "hsv %d, %d%%, %d%%", hue, sat * 100 / 255, val * 100 / 255);
        c.text(rx, SVY + 118, b, th::muted);
        draw_button(c, rx, SVY + 136, 76, 22, "Copy hex", hot(rx, SVY + 136, 76, 22), true);
        draw_button(c, rx + 82, SVY + 136, 50, 22, "Save", hot(rx + 82, SVY + 136, 50, 22), false, nsaved < 24);
        // harmonies
        for (int i = 0; i < 5; i++) c.fill(rx + i * 28, SVY + 166, 24, 14, hsv(hue + (i - 2) * 30, sat, val));
        for (int i = 0; i < nsaved; i++) {
            Rect r = swatch(i);
            c.fill(r.x, r.y, r.w, r.h, saved[i]);
            c.rect(r.x, r.y, r.w, r.h, hot(r.x, r.y, r.w, r.h) ? th::ink : th::border);
        }
        if (!nsaved) c.text(swatch(0).x, swatch(0).y + 6, "saved colours appear here", th::muted);
    }
    void pick(const MouseEv& e) {
        if (drag_sv) { sat = clamp((e.x - SVX) * 255 / SVS, 0, 255); val = 255 - clamp((e.y - SVY) * 255 / SVS, 0, 255); }
        if (drag_h) hue = clamp((e.y - SVY) * 360 / SVS, 0, 359);
        ui_invalidate();
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_DOWN && e.button == 1) {
            drag_sv = Rect{SVX, SVY, SVS, SVS}.contains(e.x, e.y);
            drag_h = Rect{HX, SVY, HW, SVS}.contains(e.x, e.y);
            if (drag_sv || drag_h) pick(e);
        } else if (e.kind == M_MOVE && (drag_sv || drag_h)) pick(e);
        else if (e.kind == M_UP) {
            drag_sv = drag_h = false;
            int rx = HX + HW + 22;
            if (Rect{rx, SVY + 136, 76, 22}.contains(e.x, e.y)) { char b[12]; snprintf(b, sizeof(b), "#%06x", cur() & 0xFFFFFF); clip_set(b); ui_notify("Copied", b, "palette"); }
            else if (Rect{rx + 82, SVY + 136, 50, 22}.contains(e.x, e.y) && nsaved < 24) { saved[nsaved++] = cur(); save_all(); }
            else for (int i = 0; i < nsaved; i++) if (swatch(i).contains(e.x, e.y)) {
                if (e.button == 2) { for (int k = i; k + 1 < nsaved; k++) saved[k] = saved[k + 1]; nsaved--; save_all(); }
                else set_from_rgb(saved[i]);
                break;
            }
            ui_invalidate();
        }
    }
};

App* new_calendar(const char*) { return new CalendarApp(); }
App* new_tasks(const char*) { return new TasksApp(); }
App* new_hex(const char* a) { return new HexApp(a); }
App* new_storage(const char*) { return new StorageApp(); }
App* new_convert(const char*) { return new ConvertApp(); }
App* new_palette(const char*) { return new PaletteApp(); }
