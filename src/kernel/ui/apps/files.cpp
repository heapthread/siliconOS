// silicon - Files: browse SiliconFS
#include "apps.h"

enum {
    F_OPEN = 1, F_RENAME, F_TRASH, F_DUP, F_NEW_FOLDER, F_NEW_FILE, F_CLOSE, F_VIEW_ICONS, F_VIEW_LIST,
    F_GO_HOME, F_GO_NOTES, F_GO_PICS, F_GO_ROOT, F_GO_TRASH, F_GO_UP, F_TERMINAL_HERE, F_WALLPAPER,
    F_COPY, F_CUT, F_PASTE, F_SELALL,
};

// the file clipboard: paths copied or cut in any Files window
static Vec<char*> g_fclip;
static bool g_fclip_cut = false;
static void fclip_clear() { for (int i = 0; i < g_fclip.len; i++) kfree(g_fclip[i]); g_fclip.clear(); }

static const int TOOLBAR_H = 34, SIDEBAR_W = 132, STATUS_H = 20;

struct Place { const char* label; const char* path; const char* icon; };
static const Place places[] = {
    {"Home", "/home", "folder_full"}, {"Notes", "/home/notes", "folder"}, {"Pictures", "/home/pictures", "folder"},
    {"Games", "/home/games", "folder"}, {"System", "/system", "folder"}, {"Disk", "/", "disk"}, {"Trash", "/trash", "trash"},
};

static const char* node_icon(FsNode* n) {
    if (n->dir) return n->children.len ? "folder_full" : "folder";
    if (str_icontains(n->name, ".pix") || is_image_name(n->name)) return "image_file";
    if (str_icontains(n->name, ".htm")) return "browser";
    if (str_icontains(n->name, ".txt") || str_icontains(n->name, ".md")) return "text_file";
    return "file";
}

struct FilesApp : App {
    char path[256] = "/home";
    Vec<char*> back;
    int sel = -1;
    Vec<FsNode*> multi;          // the whole selection when more than one item is picked (empty = just `sel`)
    int anchor = 0;
    bool marquee = false, press_multi = false;
    int mq_x0 = 0, mq_y0 = 0, mq_x1 = 0, mq_y1 = 0;
    Smooth sc;
    ScrollBar sb;
    bool list_view = false;
    int cw = 0, ch = 0;
    // drag and drop
    bool pressing = false, dragging = false;
    int press_x = 0, press_y = 0, press_item = -1;

    FilesApp(const char* arg) { if (arg) strlcpy(path, arg, sizeof(path)); }
    ~FilesApp() { for (int i = 0; i < back.len; i++) kfree(back[i]); back.free(); multi.free(); }

    const char* app_id() override { return "files"; }
    char tbuf[64];
    const char* title() override {
        FsNode* d = fs_lookup(path);
        snprintf(tbuf, sizeof(tbuf), "Files " G_BULLET " %s", !d || d == fs_root() ? "Disk" : d->name);
        return tbuf;
    }
    int pref_w() override { return 640; }
    int pref_h() override { return 410; }
    void on_resize(int w, int h) override { cw = w; ch = h; }
    void tick() override { if (sc.step()) ui_invalidate(); }

    FsNode* dir() {
        FsNode* d = fs_lookup(path);
        if (!d || !d->dir) { strlcpy(path, "/home", sizeof(path)); d = fs_home(); sel = -1; }
        return d;
    }
    void retitle() { set_title(title()); }
    void go(const char* p, bool remember = true) {
        FsNode* n = fs_lookup(p);
        if (!n || !n->dir) return;
        if (remember && strcmp(p, path)) back.push(strdup(path));
        fs_path(n, path, sizeof(path));
        sel = -1;
        multi.clear();
        sc.jump(0);
        retitle();
        ui_invalidate();
    }
    void open_arg(const char* arg) override { go(arg); }
    // something dragged in from another window or the desktop
    bool on_drop(const char* p, int x, int y) override {
        FsNode* src = fs_lookup(p);
        if (!src || src == fs_root()) return false;
        FsNode* d = dir();
        int t = hit(x, y);
        if (t >= 0 && d->children[t]->dir) d = d->children[t];
        else if (!content().contains(x, y)) {
            int pl = place_hit(x, y);
            if (pl < 0) return false;
            d = fs_lookup(places[pl].path);
            if (!d) return false;
        }
        if (src->parent == d || src == d || (src->dir && fs_is_ancestor(src, d))) return false;
        if (d == fs_trash_dir()) { fs_trash(src); return true; }
        char nm[FS_NAME_MAX];
        fs_unique_name(d, src->name, nm, sizeof(nm));
        if (!fs_move(src, d, nm)) return false;
        multi.clear();
        sel = -1;
        return true;
    }
    void go_back() {
        if (!back.len) return;
        char* p = back.back();
        back.len--;
        go(p, false);
        kfree(p);
    }
    void go_up() {
        FsNode* d = dir();
        if (d->parent) { char p[256]; fs_path(d->parent, p, sizeof(p)); go(p); }
    }

    // ------------------------------------------------------------ selection
    bool picked_has(FsNode* n) {
        if (multi.len) return multi.find(n) >= 0;
        FsNode* d = dir();
        return sel >= 0 && sel < d->children.len && d->children[sel] == n;
    }
    int sel_count() { return multi.len ? multi.len : (selected() ? 1 : 0); }
    void picked(Vec<FsNode*>& out) {
        if (multi.len) { for (int i = 0; i < multi.len; i++) out.push(multi[i]); }
        else if (selected()) out.push(selected());
    }
    void select_range(int a, int b) {
        FsNode* d = dir();
        multi.clear();
        for (int i = min(a, b); i <= max(a, b) && i < d->children.len; i++) multi.push(d->children[i]);
        sel = b;
    }
    void update_marquee() {
        FsNode* d = dir();
        Rect mq = {min(mq_x0, mq_x1), min(mq_y0, mq_y1), iabs(mq_x1 - mq_x0), iabs(mq_y1 - mq_y0)};
        multi.clear();
        for (int i = 0; i < d->children.len; i++) {
            Rect r = item_rect(i);
            if (r.x < mq.x + mq.w && r.x + r.w > mq.x && r.y < mq.y + mq.h && r.y + r.h > mq.y) { multi.push(d->children[i]); sel = i; }
        }
        if (!multi.len) sel = -1;
        else anchor = sel;
    }
    void do_copy(bool cut) {
        Vec<FsNode*> items;
        picked(items);
        if (!items.len) { items.free(); return; }
        fclip_clear();
        g_fclip_cut = cut;
        char* text = (char*)kmalloc(items.len * 260 + 4);
        int o = 0;
        text[0] = 0;
        for (int i = 0; i < items.len; i++) {
            char p[256];
            fs_path(items[i], p, sizeof(p));
            g_fclip.push(strdup(p));
            o += snprintf(text + o, 260, "%s%s", i ? "\n" : "", p);
        }
        clip_set(text);
        kfree(text);
        char msg[48];
        snprintf(msg, sizeof(msg), "%d item%s", items.len, items.len == 1 ? "" : "s");
        ui_notify(cut ? "Cut" : "Copied", msg, "folder");
        items.free();
    }
    void do_paste() {
        FsNode* d = dir();
        int done = 0, skipped = 0;
        for (int i = 0; i < g_fclip.len; i++) {
            FsNode* src = fs_lookup(g_fclip[i]);
            if (!src || src == fs_root()) continue;
            if (src->dir && (src == d || fs_is_ancestor(src, d))) { skipped++; continue; }
            char nm[FS_NAME_MAX];
            if (g_fclip_cut) {
                if (src->parent == d) continue;
                fs_unique_name(d, src->name, nm, sizeof(nm));
                if (fs_move(src, d, nm)) done++; else skipped++;
            } else {
                fs_unique_name(d, src->name, nm, sizeof(nm));
                if (fs_copy(src, d, nm)) done++; else skipped++;
            }
        }
        if (g_fclip_cut) fclip_clear();
        multi.clear();
        sel = -1;
        char msg[64];
        snprintf(msg, sizeof(msg), "%d item%s%s", done, done == 1 ? "" : "s", skipped ? ", some skipped" : "");
        if (done || skipped) ui_notify("Pasted", msg, "folder");
    }
    void select_all() {
        FsNode* d = dir();
        multi.clear();
        for (int i = 0; i < d->children.len; i++) multi.push(d->children[i]);
        sel = d->children.len ? 0 : -1;
        anchor = 0;
    }

    // ------------------------------------------------------------ geometry
    Rect content() { return {SIDEBAR_W, TOOLBAR_H, cw - SIDEBAR_W, ch - TOOLBAR_H - STATUS_H}; }
    int cols() { return max(1, (content().w - 16) / 96); }
    Rect item_rect(int i) {
        Rect c = content();
        if (list_view) return {c.x + 4, c.y + 22 + i * 20 - sc.pos(), c.w - 18, 20};
        int col = i % cols(), row = i / cols();
        return {c.x + 8 + col * 96, c.y + 8 + row * 96 - sc.pos(), 92, 92};
    }
    int content_height(int n) {
        if (list_view) return 22 + n * 20 + 8;
        return 8 + ((n + cols() - 1) / cols()) * 96;
    }
    int hit(int x, int y) {
        FsNode* d = dir();
        if (!content().contains(x, y)) return -1;
        for (int i = 0; i < d->children.len; i++) if (item_rect(i).contains(x, y)) return i;
        return -1;
    }
    int place_hit(int x, int y) {
        for (int i = 0; i < ARRAY_LEN(places); i++)
            if (Rect{6, TOOLBAR_H + 26 + i * 24, SIDEBAR_W - 12, 22}.contains(x, y)) return i;
        return -1;
    }

    // ------------------------------------------------------------ drawing
    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        FsNode* d = dir();
        fs_sort(d);
        if (sel >= d->children.len) sel = -1;
        for (int i = multi.len - 1; i >= 0; i--) if (d->children.find(multi[i]) < 0) multi.remove(i);
        // toolbar
        c.fill(0, 0, w, TOOLBAR_H, th::panel);
        c.hline(0, TOOLBAR_H - 1, w, th::border);
        draw_button(c, 8, 6, 26, 22, G_LEFT, hot(8, 6, 26, 22), false, back.len > 0);
        draw_button(c, 38, 6, 26, 22, G_UP, hot(38, 6, 26, 22), false, d->parent != nullptr);
        draw_bevel(c, 72, 6, w - 72 - 116, 22, th::white, true);
        c.blit(icon(node_icon(d))->small, 77, 9);
        c.text_n(98, 11, path, (w - 72 - 116 - 34) / 8, th::text);
        draw_button(c, w - 108, 6, 66, 22, "+ folder", hot(w - 108, 6, 66, 22));
        draw_button(c, w - 36, 6, 28, 22, list_view ? G_FULL : "=", hot(w - 36, 6, 28, 22));
        // sidebar
        c.fill(0, TOOLBAR_H, SIDEBAR_W, h - TOOLBAR_H, th::panel_dk);
        c.vline(SIDEBAR_W - 1, TOOLBAR_H, h - TOOLBAR_H, th::border);
        c.text(12, TOOLBAR_H + 9, "PLACES", th::muted, 1, true);
        for (int i = 0; i < ARRAY_LEN(places); i++) {
            int y = TOOLBAR_H + 26 + i * 24;
            bool cur = !strcmp(path, places[i].path);
            bool drop = dragging && place_hit(ui_mouse_x - win->client().x, ui_mouse_y - win->client().y) == i;
            if (cur || drop) c.fill(6, y, SIDEBAR_W - 12, 22, drop ? blend(accent(), th::white, 80) : accent());
            else if (hot(6, y, SIDEBAR_W - 12, 22)) c.fill(6, y, SIDEBAR_W - 12, 22, blend(th::panel_dk, accent(), 60));
            c.blit(icon(places[i].icon)->small, 12, y + 3);
            c.text(34, y + 5, places[i].label, cur ? th::white : th::text, 1, cur);
        }
        // disk usage gauge
        if (fs_persistent()) {
            int y = h - STATUS_H - 40;
            c.text(12, y, "disk", th::muted);
            int pct = (int)((u64)fs_disk_used() * 100 / max(1u, fs_disk_capacity()));
            draw_progress(c, 12, y + 14, SIDEBAR_W - 24, 10, max(pct, 2), accent());
        }
        // content
        Rect cr = content();
        c.set_clip({cr.x, cr.y, cr.w, cr.h});
        if (list_view) {
            c.fill(cr.x, cr.y, cr.w, 20, th::panel);
            c.hline(cr.x, cr.y + 20, cr.w, th::border);
            c.text(cr.x + 30, cr.y + 4, "name", th::muted, 1, true);
            c.text(cr.x + cr.w - 210, cr.y + 4, "size", th::muted, 1, true);
            c.text(cr.x + cr.w - 130, cr.y + 4, "modified", th::muted, 1, true);
        }
        for (int i = 0; i < d->children.len; i++) {
            FsNode* n = d->children[i];
            Rect r = item_rect(i);
            if (r.y + r.h < cr.y || r.y > cr.y + cr.h) continue;
            bool is_sel = picked_has(n);
            bool drop = dragging && n->dir && !picked_has(n) && r.contains(ui_mouse_x - win->client().x, ui_mouse_y - win->client().y);
            if (list_view) {
                if (is_sel || drop) c.fill(r.x, r.y, r.w, r.h, drop ? blend(accent(), th::white, 80) : accent());
                else if (i % 2) c.fill(r.x, r.y, r.w, r.h, 0xFFE6E1F0);
                u32 col = is_sel ? th::white : th::text;
                c.blit(icon(node_icon(n))->small, r.x + 6, r.y + 2);
                c.text_n(r.x + 26, r.y + 4, n->name, (r.w - 240) / 8, col);
                char sz[24];
                if (n->dir) snprintf(sz, sizeof(sz), "%d items", n->children.len);
                else fmt_bytes(n->size, sz, sizeof(sz));
                c.text(r.x + r.w - 206, r.y + 4, sz, is_sel ? th::white : th::muted);
                DateTime dt;
                stamp_to_datetime(n->mtime, dt);
                char ds[32];
                snprintf(ds, sizeof(ds), "%s %d %02d:%02d", month_name(dt.month), dt.day, dt.hour, dt.minute);
                c.text(r.x + r.w - 126, r.y + 4, ds, is_sel ? th::white : th::muted);
            } else {
                if (drop) c.fill(r.x + 10, r.y, 72, 70, blend(accent(), th::white, 80));
                draw_icon_label(c, icon(node_icon(n)), r.x, r.y + 4, r.w, n->name, is_sel, false);
            }
        }
        if (!d->children.len) c.text_centered(cr.x, cr.y + cr.h / 2 - 6, cr.w, "this folder is empty", th::muted);
        if (marquee) draw_marquee(c, {min(mq_x0, mq_x1), min(mq_y0, mq_y1), iabs(mq_x1 - mq_x0), iabs(mq_y1 - mq_y0)});
        c.reset_clip();
        c.set_clip({0, 0, w, h});
        int total = content_height(d->children.len);
        if (total > cr.h) sb.draw(c, w - 10, cr.y, cr.h, total, cr.h, sc.pos());
        // status bar
        c.fill(0, h - STATUS_H, w, STATUS_H, th::panel);
        c.hline(0, h - STATUS_H, w, th::border);
        char st[96];
        if (sel_count() > 1) snprintf(st, sizeof(st), "%d selected", sel_count());
        else if (sel >= 0) {
            FsNode* n = d->children[sel];
            char sz[24];
            if (n->dir) snprintf(sz, sizeof(sz), "%d items", n->children.len);
            else fmt_bytes(n->size, sz, sizeof(sz));
            snprintf(st, sizeof(st), "%s  " G_BULLET "  %s", n->name, sz);
        } else snprintf(st, sizeof(st), "%d item%s", d->children.len, d->children.len == 1 ? "" : "s");
        c.text(SIDEBAR_W + 8, h - STATUS_H + 4, st, th::muted);
        // drag ghost
        if (dragging && press_item >= 0 && press_item < d->children.len) {
            Rect cl = win->client();
            c.blit_tinted(icon(node_icon(d->children[press_item]))->mid, ui_mouse_x - cl.x - 16, ui_mouse_y - cl.y - 16, accent(), 60);
        }
    }

    // ------------------------------------------------------------ actions
    FsNode* selected() {
        FsNode* d = dir();
        return sel >= 0 && sel < d->children.len ? d->children[sel] : nullptr;
    }
    void open_item(FsNode* n) {
        if (!n) return;
        char p[256];
        fs_path(n, p, sizeof(p));
        if (n->dir) go(p);
        else open_path(p);
    }
    void trash_selected() {
        Vec<FsNode*> items;
        picked(items);
        if (!items.len) { items.free(); return; }
        bool in_trash = dir() == fs_trash_dir();
        char msg[48];
        if (items.len == 1) strlcpy(msg, items[0]->name, sizeof(msg));
        else snprintf(msg, sizeof(msg), "%d items", items.len);
        for (int i = 0; i < items.len; i++) { if (in_trash) fs_remove(items[i]); else fs_trash(items[i]); }
        ui_notify(in_trash ? "Deleted" : "Moved to Trash", msg, "trash");
        items.free();
        multi.clear();
        sel = -1;
        ui_invalidate();
    }
    static void rename_cb(App* a, const char* text) {
        FilesApp* f = (FilesApp*)a;
        FsNode* n = f->selected();
        if (n && !fs_move(n, n->parent, text)) f->info("that name is taken or not allowed.");
    }
    static void new_folder_cb(App* a, const char* text) {
        FilesApp* f = (FilesApp*)a;
        if (fs_child(f->dir(), text) || !fs_create(f->dir(), text, true)) f->info("cannot create a folder with that name.");
    }
    static void new_file_cb(App* a, const char* text) {
        FilesApp* f = (FilesApp*)a;
        if (fs_child(f->dir(), text) || !fs_create(f->dir(), text, false)) f->info("cannot create a file with that name.");
    }

    void on_command(int cmd) override {
        FsNode* n = selected();
        switch (cmd) {
        case F_OPEN: open_item(n); break;
        case F_RENAME: if (n) prompt("rename to:", n->name, rename_cb); break;
        case F_TRASH: trash_selected(); break;
        case F_DUP: {
            Vec<FsNode*> items;
            picked(items);
            for (int i = 0; i < items.len; i++) { char nm[FS_NAME_MAX]; fs_unique_name(dir(), items[i]->name, nm, sizeof(nm)); fs_copy(items[i], dir(), nm); }
            items.free();
            multi.clear();
            break;
        }
        case F_COPY: do_copy(false); break;
        case F_CUT: do_copy(true); break;
        case F_PASTE: do_paste(); break;
        case F_SELALL: select_all(); break;
        case F_NEW_FOLDER: { char nm[FS_NAME_MAX]; fs_unique_name(dir(), "new folder", nm, sizeof(nm)); prompt("new folder name:", nm, new_folder_cb); break; }
        case F_NEW_FILE: { char nm[FS_NAME_MAX]; fs_unique_name(dir(), "untitled.txt", nm, sizeof(nm)); prompt("new file name:", nm, new_file_cb); break; }
        case F_CLOSE: close(); break;
        case F_VIEW_ICONS: list_view = false; sc.jump(0); break;
        case F_VIEW_LIST: list_view = true; sc.jump(0); break;
        case F_GO_HOME: go("/home"); break;
        case F_GO_NOTES: go("/home/notes"); break;
        case F_GO_PICS: go("/home/pictures"); break;
        case F_GO_ROOT: go("/"); break;
        case F_GO_TRASH: go("/trash"); break;
        case F_GO_UP: go_up(); break;
        case F_WALLPAPER: launch("wallpapers"); break;
        }
        ui_invalidate();
    }

    int menus(Menu* m) override {
        bool has = selected() != nullptr;
        m[0].clear(); strlcpy(m[0].title, "File", 24);
        m[0].add("New folder", F_NEW_FOLDER, "ctrl+n").add("New text file", F_NEW_FILE).sep()
            .add("Open", F_OPEN, "enter", has).add("Rename" G_ELLIP, F_RENAME, "f2", has).add("Duplicate", F_DUP, "ctrl+d", has)
            .add("Move to Trash", F_TRASH, "del", has).sep().add("Close", F_CLOSE, "ctrl+w");
        m[1].clear(); strlcpy(m[1].title, "Edit", 24);
        m[1].add("Copy", F_COPY, "ctrl+c", has).add("Cut", F_CUT, "ctrl+x", has).add("Paste", F_PASTE, "ctrl+v", g_fclip.len > 0)
            .sep().add("Select all", F_SELALL, "ctrl+a");
        m[2].clear(); strlcpy(m[2].title, "View", 24);
        m[2].add("As icons", F_VIEW_ICONS, "", true, !list_view).add("As list", F_VIEW_LIST, "", true, list_view);
        m[3].clear(); strlcpy(m[3].title, "Go", 24);
        m[3].add("Up", F_GO_UP, "backspace").sep().add("Home", F_GO_HOME).add("Notes", F_GO_NOTES)
            .add("Pictures", F_GO_PICS).add("Disk", F_GO_ROOT).add("Trash", F_GO_TRASH);
        return 4;
    }

    static void ctx_fn(void* ctx, int cmd) { ((App*)ctx)->on_command(cmd); }

    void on_mouse(const MouseEv& e) override {
        FsNode* d = dir();
        int total = content_height(d->children.len);
        int maxs = max(0, total - content().h);
        if (e.kind == M_WHEEL) {
            sc.set(clamp(sc.target - e.wheel * 72, 0, maxs));
            ui_invalidate();
            return;
        }
        int off = sc.target;
        if (sb.handle(e, cw - 10, content().y, content().h, total, content().h, off)) { sc.jump(off); return; }
        if (e.kind == M_MOVE) {
            if (marquee) { mq_x1 = e.x; mq_y1 = e.y; update_marquee(); ui_invalidate(); return; }
            if (pressing && !dragging && press_item >= 0 && (iabs(e.x - press_x) > 6 || iabs(e.y - press_y) > 6)) dragging = true;
            ui_invalidate();
            return;
        }
        if (e.kind == M_UP) {
            if (marquee) { marquee = false; pressing = false; ui_invalidate(); return; }
            if (dragging) {
                int target = hit(e.x, e.y);
                FsNode* src = press_item >= 0 && press_item < d->children.len ? d->children[press_item] : nullptr;
                FsNode* dst = nullptr;
                if (target >= 0 && d->children[target]->dir && !picked_has(d->children[target])) dst = d->children[target];
                int p = place_hit(e.x, e.y);
                if (p >= 0) dst = fs_lookup(places[p].path);
                if (src && dst) {
                    Vec<FsNode*> items;
                    picked(items);
                    if (!picked_has(src)) { items.clear(); items.push(src); }
                    for (int i = 0; i < items.len; i++) {
                        if (dst == fs_trash_dir()) fs_trash(items[i]);
                        else if (items[i] != dst && !fs_move(items[i], dst, nullptr)) ui_notify("Can't move", "a file with that name exists there", "file");
                    }
                    items.free();
                    multi.clear();
                    sel = -1;
                } else if (src && !dst) {
                    // dropped outside this window: another app, the desktop or the trash
                    Rect cl = win->client();
                    int sx = cl.x + e.x, sy = cl.y + e.y;
                    if (!win->r.contains(sx, sy)) {
                        Vec<FsNode*> items;
                        picked(items);
                        if (!picked_has(src)) { items.clear(); items.push(src); }
                        for (int i = 0; i < items.len; i++) {
                            char path_i[256];
                            fs_path(items[i], path_i, sizeof(path_i));
                            int res = ui_drop(sx, sy, path_i, win);
                            if (res == DROP_TRASH) fs_trash(items[i]);
                            else if (res == DROP_DESKTOP && items[i]->parent != fs_home()) {
                                char nm[FS_NAME_MAX];
                                fs_unique_name(fs_home(), items[i]->name, nm, sizeof(nm));
                                fs_move(items[i], fs_home(), nm);
                            }
                        }
                        items.free();
                        multi.clear();
                        sel = -1;
                    }
                }
            } else if (e.button == 1) {
                if (press_multi && press_item >= 0 && press_item < d->children.len && !kbd_ctrl_down() && !kbd_shift_down()) { multi.clear(); sel = anchor = press_item; }
                press_multi = false;
                // toolbar buttons
                if (Rect{8, 6, 26, 22}.contains(e.x, e.y)) go_back();
                else if (Rect{38, 6, 26, 22}.contains(e.x, e.y)) go_up();
                else if (Rect{cw - 108, 6, 66, 22}.contains(e.x, e.y)) on_command(F_NEW_FOLDER);
                else if (Rect{cw - 36, 6, 28, 22}.contains(e.x, e.y)) on_command(list_view ? F_VIEW_ICONS : F_VIEW_LIST);
                else { int p = place_hit(e.x, e.y); if (p >= 0) go(places[p].path); }
            } else if (e.button == 2) {
                int i = hit(e.x, e.y);
                Menu m;
                Rect cl = win->client();
                if (i >= 0) {
                    if (!picked_has(d->children[i])) { multi.clear(); sel = anchor = i; }
                    m.add("Open", F_OPEN).add("Rename" G_ELLIP, F_RENAME, "", sel_count() == 1).add("Duplicate", F_DUP).sep()
                     .add("Copy", F_COPY).add("Cut", F_CUT).sep()
                     .add(dir() == fs_trash_dir() ? "Delete forever" : "Move to Trash", F_TRASH);
                } else if (content().contains(e.x, e.y)) {
                    m.add("New folder", F_NEW_FOLDER).add("New text file", F_NEW_FILE).sep()
                     .add("Paste", F_PASTE, "", g_fclip.len > 0).add("Select all", F_SELALL).sep()
                     .add("As icons", F_VIEW_ICONS, "", true, !list_view).add("As list", F_VIEW_LIST, "", true, list_view);
                }
                if (m.count) menu_popup(&m, cl.x + e.x, cl.y + e.y, ctx_fn, this);
            }
            pressing = dragging = false;
            press_item = -1;
            ui_invalidate();
            return;
        }
        if (e.kind == M_DOWN || e.kind == M_DBL) {
            if (e.button != 1) return;
            int i = hit(e.x, e.y);
            bool ctrl = kbd_ctrl_down(), shift = kbd_shift_down();
            press_multi = false;
            if (e.kind == M_DBL && i >= 0) { multi.clear(); sel = anchor = i; open_item(d->children[i]); return; }
            if (content().contains(e.x, e.y)) {
                if (i < 0) {
                    if (!ctrl && !shift) { sel = -1; multi.clear(); }
                    marquee = true; mq_x0 = mq_x1 = e.x; mq_y0 = mq_y1 = e.y;
                } else if (ctrl) {
                    if (!multi.len && selected()) multi.push(selected());
                    int at = multi.find(d->children[i]);
                    if (at >= 0) multi.remove(at); else multi.push(d->children[i]);
                    sel = anchor = i;
                } else if (shift && sel >= 0) select_range(anchor, i);
                else if (!picked_has(d->children[i])) { multi.clear(); sel = anchor = i; }
                else { sel = i; press_multi = multi.len > 1; }
            }
            pressing = true;
            press_x = e.x; press_y = e.y;
            press_item = i;
            ui_invalidate();
        }
    }

    void on_key(const KeyEvent& e) override {
        FsNode* d = dir();
        int n = d->children.len;
        int step = list_view ? 1 : cols();
        if (e.ctrl && !e.alt && (e.key == 'c' || e.key == 'x' || e.key == 'v' || e.key == 'a')) {
            on_command(e.key == 'c' ? F_COPY : e.key == 'x' ? F_CUT : e.key == 'v' ? F_PASTE : F_SELALL);
            return;
        }
        int old_sel = sel;
        switch (e.key) {
        case K_RIGHT: if (!list_view) sel = min(sel + 1, n - 1); break;
        case K_LEFT: if (!list_view) sel = max(sel - 1, 0); break;
        case K_DOWN: sel = min(sel + step, n - 1); break;
        case K_UP: sel = sel < 0 ? 0 : max(sel - step, 0); break;
        case K_ENTER: open_item(selected()); break;
        case K_BACKSPACE: go_up(); break;
        case K_DELETE: trash_selected(); break;
        case K_F2: on_command(F_RENAME); break;
        default:
            if (e.ctrl && e.key == 'n') on_command(F_NEW_FOLDER);
            else if (e.ctrl && e.key == 'd') on_command(F_DUP);
            else if (e.ch && !e.ctrl) {
                // type-to-select
                for (int i = 0; i < n; i++)
                    if (to_lower(d->children[(sel + 1 + i) % n]->name[0]) == to_lower(e.ch)) { sel = (sel + 1 + i) % n; break; }
            }
        }
        if (sel != old_sel && (e.key == K_RIGHT || e.key == K_LEFT || e.key == K_UP || e.key == K_DOWN)) {
            if (e.shift && old_sel >= 0) select_range(anchor, sel);
            else { multi.clear(); anchor = sel; }
        }
        // keep the selection visible
        if (sel >= 0) {
            Rect r = item_rect(sel), c = content();
            int t = sc.target;
            if (r.y < c.y) t -= c.y - r.y + 4;
            if (r.y + r.h > c.y + c.h) t += r.y + r.h - (c.y + c.h) + 4;
            sc.set(max(t, 0));
        }
        ui_invalidate();
    }
};

App* new_files(const char* arg) { return new FilesApp(arg); }
