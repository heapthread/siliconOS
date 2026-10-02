// silicon - Files: browse SiliconFS
#include "apps.h"

enum {
    F_OPEN = 1, F_RENAME, F_TRASH, F_DUP, F_NEW_FOLDER, F_NEW_FILE, F_CLOSE, F_VIEW_ICONS, F_VIEW_LIST,
    F_GO_HOME, F_GO_NOTES, F_GO_PICS, F_GO_ROOT, F_GO_TRASH, F_GO_UP, F_TERMINAL_HERE, F_WALLPAPER,
};

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
    Smooth sc;
    ScrollBar sb;
    bool list_view = false;
    int cw = 0, ch = 0;
    // drag and drop
    bool pressing = false, dragging = false;
    int press_x = 0, press_y = 0, press_item = -1;

    FilesApp(const char* arg) { if (arg) strlcpy(path, arg, sizeof(path)); }
    ~FilesApp() { for (int i = 0; i < back.len; i++) kfree(back[i]); back.free(); }

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
        sc.jump(0);
        retitle();
        ui_invalidate();
    }
    void open_arg(const char* arg) override { go(arg); }
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
            bool is_sel = i == sel;
            bool drop = dragging && n->dir && i != press_item && r.contains(ui_mouse_x - win->client().x, ui_mouse_y - win->client().y);
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
        c.reset_clip();
        c.set_clip({0, 0, w, h});
        int total = content_height(d->children.len);
        if (total > cr.h) sb.draw(c, w - 10, cr.y, cr.h, total, cr.h, sc.pos());
        // status bar
        c.fill(0, h - STATUS_H, w, STATUS_H, th::panel);
        c.hline(0, h - STATUS_H, w, th::border);
        char st[96];
        if (sel >= 0) {
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
        FsNode* n = selected();
        if (!n) return;
        if (dir() == fs_trash_dir()) { fs_remove(n); ui_notify("Deleted", "removed permanently", "trash"); }
        else { fs_trash(n); ui_notify("Moved to Trash", n->name, "trash"); }
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
        case F_DUP:
            if (n) { char nm[FS_NAME_MAX]; fs_unique_name(dir(), n->name, nm, sizeof(nm)); fs_copy(n, dir(), nm); }
            break;
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
        m[1].clear(); strlcpy(m[1].title, "View", 24);
        m[1].add("As icons", F_VIEW_ICONS, "", true, !list_view).add("As list", F_VIEW_LIST, "", true, list_view);
        m[2].clear(); strlcpy(m[2].title, "Go", 24);
        m[2].add("Up", F_GO_UP, "backspace").sep().add("Home", F_GO_HOME).add("Notes", F_GO_NOTES)
            .add("Pictures", F_GO_PICS).add("Disk", F_GO_ROOT).add("Trash", F_GO_TRASH);
        return 3;
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
            if (pressing && !dragging && press_item >= 0 && (iabs(e.x - press_x) > 6 || iabs(e.y - press_y) > 6)) dragging = true;
            ui_invalidate();
            return;
        }
        if (e.kind == M_UP) {
            if (dragging) {
                int target = hit(e.x, e.y);
                FsNode* src = press_item < d->children.len ? d->children[press_item] : nullptr;
                FsNode* dst = nullptr;
                if (target >= 0 && target != press_item && d->children[target]->dir) dst = d->children[target];
                int p = place_hit(e.x, e.y);
                if (p >= 0) dst = fs_lookup(places[p].path);
                if (src && dst) {
                    if (dst == fs_trash_dir()) fs_trash(src);
                    else if (!fs_move(src, dst, nullptr)) ui_notify("Can't move", "a file with that name exists there", "file");
                    sel = -1;
                }
            } else if (e.button == 1) {
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
                    sel = i;
                    m.add("Open", F_OPEN).add("Rename" G_ELLIP, F_RENAME).add("Duplicate", F_DUP).sep()
                     .add(dir() == fs_trash_dir() ? "Delete forever" : "Move to Trash", F_TRASH);
                } else if (content().contains(e.x, e.y)) {
                    m.add("New folder", F_NEW_FOLDER).add("New text file", F_NEW_FILE).sep()
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
            if (content().contains(e.x, e.y)) sel = i;
            if (e.kind == M_DBL && i >= 0) { open_item(d->children[i]); return; }
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
