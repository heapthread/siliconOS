// silicon - Photos: a gallery of the pictures in SiliconFS and a viewer (PNG, JPEG, GIF, BMP)
// with fit / zoom / pan and next / previous.
#include "apps.h"

bool is_image_name(const char* name) {
    return str_icontains(name, ".png") || str_icontains(name, ".jpg") || str_icontains(name, ".jpeg") ||
           str_icontains(name, ".gif") || str_icontains(name, ".bmp");
}

static const int BAR_H = 34;

struct PhotosApp : App {
    // gallery
    Vec<FsNode*> files;
    Vec<Image> thumbs;                // decoded lazily, a few per tick
    int thumbs_done = 0;
    Smooth sc;
    ScrollBar sb;
    int hover = -1;
    // viewer
    bool viewing = false;
    int cur = -1;
    Image img, fitted;                // original and a shrunk copy for "fit"
    int fit_w = 0, fit_h = 0;
    bool fit = true;
    int zoom = 256;                   // 256 = 1:1
    int pan_x = 0, pan_y = 0;
    bool dragging = false;
    int drag_x = 0, drag_y = 0, drag_px = 0, drag_py = 0;
    u64 open_t0 = 0;
    char err[96] = "";
    int cw = 0, ch = 0;
    char tbuf[96];

    PhotosApp(const char* arg) {
        scan();
        if (arg && arg[0]) open_arg(arg);
    }
    ~PhotosApp() {
        free_image();
        for (int i = 0; i < thumbs.len; i++) if (thumbs[i].px) kfree(thumbs[i].px);
        thumbs.free();
        files.free();
    }
    const char* app_id() override { return "photos"; }
    const char* title() override {
        if (viewing && cur >= 0) { snprintf(tbuf, sizeof(tbuf), "%s - Photos", files[cur]->name); return tbuf; }
        return "Photos";
    }
    int pref_w() override { return 700; }
    int pref_h() override { return 500; }
    u32 background() override { return viewing ? 0xFF16121F : th::paper; }

    void collect(FsNode* n) {
        for (int i = 0; i < n->children.len; i++) {
            FsNode* c = n->children[i];
            if (c == fs_trash_dir()) continue;
            if (c->dir) collect(c);
            else if (is_image_name(c->name)) files.push(c);
        }
    }
    void scan() {
        for (int i = 0; i < thumbs.len; i++) if (thumbs[i].px) kfree(thumbs[i].px);
        thumbs.clear();
        files.clear();
        collect(fs_root());
        for (int i = 0; i < files.len; i++) thumbs.push(Image());
        thumbs_done = 0;
    }
    void free_image() {
        if (img.px) kfree(img.px);
        if (fitted.px) kfree(fitted.px);
        img = Image();
        fitted = Image();
    }
    void open_index(int i) {
        if (i < 0 || i >= files.len) return;
        free_image();
        cur = i;
        viewing = true;
        err[0] = 0;
        fit = true;
        zoom = 256;
        pan_x = pan_y = 0;
        open_t0 = ticks();
        FsNode* f = files[i];
        if (!image_load(f->data, f->size, img)) {
            const char* fmt = image_format(f->data, f->size);
            snprintf(err, sizeof(err), fmt ? "silicon can't decode %s pictures yet" : "this isn't a picture silicon understands", fmt);
        }
        fit_w = fit_h = 0;
        set_title(title());
        ui_invalidate();
    }
    void open_arg(const char* path) override {
        FsNode* f = fs_lookup(path);
        if (!f) return;
        int idx = files.find(f);
        if (idx < 0) { files.push(f); thumbs.push(Image()); idx = files.len - 1; }
        open_index(idx);
    }

    // ------------------------------------------------------------ gallery
    static const int TW = 150, TH = 120;
    int cols() { return max(1, (cw - 20) / (TW + 12)); }
    Rect cell(int i) {
        int c = cols();
        return {14 + (i % c) * (TW + 12), BAR_H + 12 + (i / c) * (TH + 30) - sc.pos(), TW, TH};
    }
    int gallery_h() { return (files.len + cols() - 1) / cols() * (TH + 30) + 20; }

    void tick() override {
        if (sc.step()) ui_invalidate();
        if (viewing && ticks() - open_t0 < 250) ui_invalidate();
        // decode a thumbnail per tick so the gallery fills in without freezing
        if (!viewing && thumbs_done < files.len) {
            FsNode* f = files[thumbs_done];
            Image t;
            if (image_load(f->data, f->size, t)) { image_shrink(t, TW - 8, TH - 8); thumbs[thumbs_done] = t; }
            thumbs_done++;
            ui_invalidate();
        }
    }

    void draw_checker(Canvas& c, int x, int y, int w, int h) {
        for (int yy = 0; yy < h; yy += 8)
            for (int xx = 0; xx < w; xx += 8)
                c.fill(x + xx, y + yy, min(8, w - xx), min(8, h - yy), ((xx + yy) / 8) % 2 ? 0xFF3A3450 : 0xFF2C2840);
    }

    void draw(Canvas& c, int w, int h) override {
        cw = w;
        ch = h;
        // toolbar
        c.fill(0, 0, w, BAR_H, th::panel);
        c.hline(0, BAR_H - 1, w, th::border);
        if (!viewing) {
            c.blit(icon("photos")->small, 10, 9);
            char b[64];
            snprintf(b, sizeof(b), "%d pictures", files.len);
            c.text(36, 11, b, th::text, 1, true);
            draw_button(c, w - 90, 6, 80, 22, "Refresh", hot(w - 90, 6, 80, 22));
            c.set_clip({0, BAR_H, w - 11, h - BAR_H});
            if (!files.len) {
                c.text_centered(0, BAR_H + 80, w, "no pictures yet", th::muted, 2, true);
                c.text_centered(0, BAR_H + 120, w, "save some from the web:  wget -O /home/pictures/cat.jpg <url>", th::muted);
            }
            for (int i = 0; i < files.len; i++) {
                Rect r = cell(i);
                if (r.y + r.h + 30 < BAR_H || r.y > h) continue;
                bool on = i == hover;
                draw_bevel(c, r.x, r.y, r.w, r.h, on ? blend(th::paper, accent(), 70) : th::panel, !on);
                Image& t = thumbs[i];
                if (t.px) {
                    draw_checker(c, r.x + (r.w - t.w) / 2, r.y + (r.h - t.h) / 2, t.w, t.h);
                    c.blit(t, r.x + (r.w - t.w) / 2, r.y + (r.h - t.h) / 2);
                } else if (i >= thumbs_done) c.text_centered(r.x, r.y + r.h / 2 - 6, r.w, G_ELLIP, th::muted);
                else c.blit(icon("image_file")->mid, r.x + r.w / 2 - 16, r.y + r.h / 2 - 16);
                c.text_n(r.x + 2, r.y + r.h + 6, files[i]->name, TW / 8, th::text);
            }
            c.set_clip({0, 0, w, h});
            sb.draw(c, w - 10, BAR_H, h - BAR_H, gallery_h(), h - BAR_H, sc.pos());
            return;
        }
        // viewer toolbar
        draw_button(c, 8, 6, 70, 22, G_LEFT " all", hot(8, 6, 70, 22));
        draw_button(c, 86, 6, 26, 22, "<", hot(86, 6, 26, 22), false, cur > 0);
        draw_button(c, 116, 6, 26, 22, ">", hot(116, 6, 26, 22), false, cur < files.len - 1);
        draw_button(c, 150, 6, 50, 22, "fit", hot(150, 6, 50, 22), fit);
        draw_button(c, 204, 6, 50, 22, "1:1", hot(204, 6, 50, 22), !fit && zoom == 256);
        if (img.px) {
            char b[80];
            snprintf(b, sizeof(b), "%dx%d  " G_BULLET "  %d%%", img.w, img.h, fit ? (fit_w * 100 / max(1, img.w)) : zoom * 100 / 256);
            c.text(w - text_width(b) - 10, 11, b, th::muted);
        }
        int vw = w, vh = h - BAR_H;
        c.set_clip({0, BAR_H, vw, vh});
        if (err[0] || !img.px) {
            c.text_centered(0, BAR_H + vh / 2 - 10, w, err[0] ? err : "can't open this picture", 0xFFB8ACD6, 1, true);
            c.set_clip({0, 0, w, h});
            return;
        }
        int dw, dh;
        const Image* src = &img;
        if (fit) {
            // shrink to fit (never enlarge small pictures by more than 4x)
            int sw = vw - 20, shh = vh - 20;
            dw = img.w; dh = img.h;
            if (dw > sw) { dh = (int)((i64)dh * sw / dw); dw = sw; }
            if (dh > shh) { dw = (int)((i64)dw * shh / dh); dh = shh; }
            int up = 1;
            while (dw * (up + 1) <= sw && dh * (up + 1) <= shh && up < 4 && img.w * (up + 1) <= sw) up++;
            if (dw == img.w && up > 1) { dw *= up; dh *= up; }
            dw = max(dw, 1); dh = max(dh, 1);
            if (dw < img.w) {
                if (fit_w != dw || fit_h != dh || !fitted.px) {
                    if (fitted.px) kfree(fitted.px);
                    fitted.w = img.w; fitted.h = img.h;
                    fitted.px = (u32*)kmalloc_try((size_t)img.w * img.h * 4);
                    if (fitted.px) {
                        memcpy(fitted.px, img.px, (size_t)img.w * img.h * 4);
                        image_shrink(fitted, dw, dh);
                    } else fitted.w = fitted.h = 0;
                }
                if (fitted.px) src = &fitted;
            }
            fit_w = dw;
            fit_h = dh;
        } else {
            dw = img.w * zoom / 256;
            dh = img.h * zoom / 256;
        }
        int x = (vw - dw) / 2 + (fit ? 0 : pan_x), y = BAR_H + (vh - dh) / 2 + (fit ? 0 : pan_y);
        // fade + grow in when a picture opens
        int p = ease_out((int)min<u64>(256, (ticks() - open_t0) * 256 / 220));
        if (p < 256) {
            int gw = dw * (230 + p / 10) / 256, gh = dh * (230 + p / 10) / 256;
            x += (dw - gw) / 2; y += (dh - gh) / 2; dw = gw; dh = gh;
        }
        c.fill(x + 6, y + 6, dw, dh, 0xFF0C0A12);
        draw_checker(c, x, y, dw, dh);
        if (src->w == dw && src->h == dh) c.blit(*src, x, y);
        else c.blit_scaled(*src, x, y, dw, dh);
        if (p < 256) c.fill_alpha(x, y, dw, dh, 0xFF16121F, (u32)(255 - p));
        c.set_clip({0, 0, w, h});
    }

    void set_zoom(int z, int cx, int cy) {
        if (!img.px) return;
        if (fit) { zoom = fit_w * 256 / max(1, img.w); fit = false; pan_x = pan_y = 0; }
        z = clamp(z, 32, 256 * 16);
        // keep the point under the cursor still
        int vx = cx - cw / 2 - pan_x, vy = cy - BAR_H - (ch - BAR_H) / 2 - pan_y;
        pan_x -= vx * z / zoom - vx;
        pan_y -= vy * z / zoom - vy;
        zoom = z;
        ui_invalidate();
    }

    void on_mouse(const MouseEv& e) override {
        if (!viewing) {
            int off = sc.target;
            if (sb.handle(e, cw - 10, BAR_H, ch - BAR_H, gallery_h(), ch - BAR_H, off)) { sc.jump(off); ui_invalidate(); return; }
            if (e.kind == M_WHEEL) { sc.set(clamp(sc.target - e.wheel * 60, 0, max(0, gallery_h() - (ch - BAR_H)))); ui_invalidate(); return; }
            int hv = -1;
            for (int i = 0; i < files.len; i++) if (e.y > BAR_H && cell(i).contains(e.x, e.y)) hv = i;
            if (hv != hover) { hover = hv; ui_invalidate(); }
            if (e.kind == M_UP && e.button == 1) {
                if (Rect{cw - 90, 6, 80, 22}.contains(e.x, e.y)) { scan(); ui_invalidate(); }
                else if (hv >= 0) open_index(hv);
            }
            return;
        }
        if (e.kind == M_WHEEL) { set_zoom(e.wheel > 0 ? zoom * 5 / 4 : zoom * 4 / 5, e.x, e.y); return; }
        if (e.y < BAR_H) {
            if (e.kind != M_UP) return;
            if (Rect{8, 6, 70, 22}.contains(e.x, e.y)) back_to_gallery();
            else if (Rect{86, 6, 26, 22}.contains(e.x, e.y)) open_index(cur - 1);
            else if (Rect{116, 6, 26, 22}.contains(e.x, e.y)) open_index(cur + 1);
            else if (Rect{150, 6, 50, 22}.contains(e.x, e.y)) { fit = true; pan_x = pan_y = 0; }
            else if (Rect{204, 6, 50, 22}.contains(e.x, e.y)) { fit = false; zoom = 256; pan_x = pan_y = 0; }
            ui_invalidate();
            return;
        }
        if (e.kind == M_DOWN && e.button == 1 && !fit) { dragging = true; drag_x = e.x; drag_y = e.y; drag_px = pan_x; drag_py = pan_y; }
        if (e.kind == M_MOVE && dragging) { pan_x = drag_px + e.x - drag_x; pan_y = drag_py + e.y - drag_y; ui_invalidate(); }
        if (e.kind == M_UP) dragging = false;
        if (e.kind == M_DBL) { if (fit) set_zoom(256, e.x, e.y); else { fit = true; pan_x = pan_y = 0; } ui_invalidate(); }
    }
    int cursor_at(int x, int y) override { return viewing && !fit && y > BAR_H ? CUR_MOVE : CUR_ARROW; }
    void back_to_gallery() {
        viewing = false;
        free_image();
        set_title(title());
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (!viewing) {
            if (e.key == K_ENTER && hover >= 0) open_index(hover);
            return;
        }
        switch (e.key) {
        case K_LEFT: open_index(cur - 1); break;
        case K_RIGHT: case ' ': open_index(cur + 1); break;
        case K_ESC: case K_BACKSPACE: back_to_gallery(); break;
        case '+': case '=': set_zoom(zoom * 5 / 4, cw / 2, BAR_H + (ch - BAR_H) / 2); break;
        case '-': set_zoom(zoom * 4 / 5, cw / 2, BAR_H + (ch - BAR_H) / 2); break;
        case '0': case 'f': fit = true; pan_x = pan_y = 0; ui_invalidate(); break;
        case '1': fit = false; zoom = 256; pan_x = pan_y = 0; ui_invalidate(); break;
        }
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "View", 24);
        m[0].add("All pictures", 1, "esc", viewing).add("Previous", 2, "left", viewing && cur > 0)
            .add("Next", 3, "right", viewing && cur < files.len - 1).sep()
            .add("Fit to window", 4, "f", viewing, fit).add("Actual size", 5, "1", viewing).add("Refresh", 6);
        return 1;
    }
    void on_command(int cmd) override {
        if (cmd == 1) back_to_gallery();
        if (cmd == 2) open_index(cur - 1);
        if (cmd == 3) open_index(cur + 1);
        if (cmd == 4) { fit = true; pan_x = pan_y = 0; }
        if (cmd == 5) { fit = false; zoom = 256; pan_x = pan_y = 0; }
        if (cmd == 6) scan();
        ui_invalidate();
    }
};

App* new_photos(const char* arg) { return new PhotosApp(arg); }
