// silicon - Trash
#include "apps.h"

struct TrashApp : App {
    int sel = -1;
    int cw = 0, ch = 0;
    const char* app_id() override { return "trash"; }
    const char* title() override { return "Trash"; }
    int pref_w() override { return 520; }
    int pref_h() override { return 340; }
    void on_resize(int w, int h) override { cw = w; ch = h; }

    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        FsNode* t = fs_trash_dir();
        if (sel >= t->children.len) sel = -1;
        c.fill(0, 0, w, 44, th::panel);
        c.hline(0, 43, w, th::border);
        c.blit(icon("trash")->mid, 10, 6);
        char b[64];
        snprintf(b, sizeof(b), "%d item%s in the trash", t->children.len, t->children.len == 1 ? "" : "s");
        c.text(50, 9, "Trash", th::text, 1, true);
        c.text(50, 24, b, th::muted);
        draw_button(c, w - 120, 10, 108, 24, "Empty trash", hot(w - 120, 10, 108, 24), true, t->children.len > 0);
        c.fill(10, 52, w - 20, 20, th::panel);
        c.text(40, 56, "name", th::muted, 1, true);
        c.text(w / 2, 56, "came from", th::muted, 1, true);
        for (int i = 0; i < t->children.len; i++) {
            FsNode* n = t->children[i];
            int y = 74 + i * 22;
            if (y > h - 50) break;
            bool s = i == sel;
            if (s) c.fill(10, y, w - 20, 22, accent());
            else if (i % 2) c.fill(10, y, w - 20, 22, 0xFFE6E1F0);
            c.blit(icon(n->dir ? "folder" : (str_icontains(n->name, ".pix") ? "image_file" : "text_file"))->small, 16, y + 3);
            c.text_n(40, y + 5, n->name, (w / 2 - 50) / 8, s ? th::white : th::text);
            c.text_n(w / 2, y + 5, n->origin[0] ? n->origin : "?", (w / 2 - 20) / 8, s ? th::white : th::muted);
        }
        if (!t->children.len) {
            c.text_centered(0, h / 2, w, "nothing here. nice and tidy " G_HEART, th::muted);
        }
        bool can = sel >= 0;
        draw_button(c, w - 230, h - 34, 100, 24, "Restore", hot(w - 230, h - 34, 100, 24), false, can);
        draw_button(c, w - 120, h - 34, 108, 24, "Delete", hot(w - 120, h - 34, 108, 24), false, can);
    }
    static void empty_cb(App*, int choice) {
        if (choice == 0) { fs_empty_trash(); ui_notify("Trash emptied", "gone for good", "trash"); }
    }
    void on_mouse(const MouseEv& e) override {
        FsNode* t = fs_trash_dir();
        if (e.kind == M_WHEEL) { sel = clamp(sel - e.wheel, 0, t->children.len - 1); ui_invalidate(); return; }
        if (e.kind == M_DOWN && e.y >= 74 && e.y < ch - 50) {
            int i = (e.y - 74) / 22;
            sel = i < t->children.len ? i : -1;
        }
        if (e.kind == M_DBL && sel >= 0) { fs_restore(t->children[sel]); sel = -1; }
        if (e.kind == M_UP) {
            if (Rect{cw - 120, 10, 108, 24}.contains(e.x, e.y) && t->children.len)
                ask("permanently delete everything in the trash?", "Empty", "Cancel", nullptr, empty_cb);
            if (sel >= 0 && Rect{cw - 230, ch - 34, 100, 24}.contains(e.x, e.y)) {
                FsNode* r = fs_restore(t->children[sel]);
                if (r) { char p[200]; fs_path(r, p, sizeof(p)); ui_notify("Restored", p, "folder"); }
                sel = -1;
            }
            if (sel >= 0 && Rect{cw - 120, ch - 34, 108, 24}.contains(e.x, e.y)) { fs_remove(t->children[sel]); sel = -1; }
        }
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        FsNode* t = fs_trash_dir();
        if (e.key == K_DOWN) sel = min(sel + 1, t->children.len - 1);
        if (e.key == K_UP) sel = max(sel - 1, 0);
        if (e.key == K_DELETE && sel >= 0) { fs_remove(t->children[sel]); sel = -1; }
        if (e.key == K_ENTER && sel >= 0) { fs_restore(t->children[sel]); sel = -1; }
        ui_invalidate();
    }
};

App* new_trash(const char*) { return new TrashApp(); }
