// silicon - Beam, the web browser: http + https (TLS 1.3), HTML layout, links, forms,
// history, bookmarks, view-source and saving pages.
#include "apps.h"
#include "../html.h"
#include "../../net/net.h"

static const int TB_H = 38, ST_H = 18;

static const char* START_PAGE =
    "<html><head><title>Beam start page</title><style>"
    "body{background:#1c1630;color:#dcd2f5}"
    ".wrap{max-width:620px;margin:0 auto}"
    ".hero{background:#2a2244;border-bottom:3px solid #9b7bff;padding:16px;text-align:center;margin-bottom:12px}"
    ".hero h1{color:#ffffff}"
    ".tag{color:#9b8cc8}"
    ".card{background:#f4f0fa;color:#2a2238;border-left:4px solid #9b7bff;padding:10px 14px;margin:12px 0}"
    ".card h3{color:#5a3fc0;margin:0}"
    ".card ul{list-style:none}"
    ".card a{color:#4a2fa8;text-decoration:none;font-weight:bold}"
    ".dim{color:#7a6e98}"
    ".tip{color:#8a7eb8;text-align:center}"
    "</style></head><body><div class=\"wrap\">"
    "<div class=\"hero\"><h1>Beam</h1><p class=\"tag\">a tiny web browser for silicon - http, https, css and pictures</p>"
    "<form action=\"https://lite.duckduckgo.com/lite/\" method=\"get\"><input type=\"text\" name=\"q\" size=\"40\"> <input type=\"submit\" value=\"Search\"></form>"
    "<p class=\"tag\">search is powered by DuckDuckGo Lite</p></div>"
    "<div class=\"card\"><h3>places that look great in Beam</h3><ul>"
    "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a> <span class=\"dim\">- tech news</span></li>"
    "<li><a href=\"https://xkcd.com/\">xkcd</a> <span class=\"dim\">- a webcomic, with pictures</span></li>"
    "<li><a href=\"https://text.npr.org/\">text.npr.org</a> <span class=\"dim\">- NPR news, text only</span></li>"
    "<li><a href=\"https://lite.cnn.com/\">lite.cnn.com</a> <span class=\"dim\">- CNN lite</span></li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Silicon\">Wikipedia: Silicon</a></li>"
    "<li><a href=\"https://wiby.me/\">wiby.me</a> <span class=\"dim\">- a search engine for the classic web</span></li>"
    "<li><a href=\"http://68k.news/\">68k.news</a> <span class=\"dim\">- headlines for old computers</span></li>"
    "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">the first website</a> <span class=\"dim\">(CERN, 1991)</span></li>"
    "<li><a href=\"https://wttr.in/?format=4\">wttr.in</a> <span class=\"dim\">- the weather, in one line</span></li>"
    "</ul></div>"
    "<div class=\"card\"><h3>silicon pages</h3><ul>"
    "<li><a href=\"silicon://bookmarks\">bookmarks</a></li><li><a href=\"silicon://history\">history</a></li>"
    "<li><a href=\"silicon://about\">about Beam</a></li></ul></div>"
    "<p class=\"tip\">tip: type words in the address bar to search, ctrl+l jumps there, alt+left goes back</p>"
    "</div></body></html>";

static const char* ABOUT_PAGE =
    "<html><head><title>about Beam</title></head><body><h1>Beam</h1>"
    "<p>Beam is silicon's web browser. Everything under it is written from scratch for silicon:</p><ul>"
    "<li>the <b>RTL8139</b> network driver</li><li>ethernet, ARP, IPv4, ICMP, UDP, <b>DHCP</b> and <b>DNS</b></li>"
    "<li>a <b>TCP</b> stack with retransmission and flow control</li>"
    "<li><b>TLS 1.2 and 1.3</b>: X25519, AES-128-GCM, SHA-256, HKDF</li><li>an HTTP/1.1 client (chunked, redirects)</li>"
    "<li>an HTML layout engine with a CSS engine (selectors, cascade, @media, variables) and PNG / JPEG / GIF decoders</li><li>no JavaScript</li></ul>"
    "<h3>please note</h3><p>HTTPS traffic is encrypted, but certificates are <b>not verified</b> yet, "
    "so don't type anything secret into a web page.</p>"
    "<p><a href=\"silicon://start\">back to the start page</a></p></body></html>";

enum { B_BACK = 1, B_FWD, B_RELOAD, B_HOME, B_STOP, B_SOURCE, B_SAVE, B_BOOKMARK, B_BOOKMARKS, B_HISTORY, B_NEWWIN, B_CLOSE, B_IMAGES, B_CSS,
       B_NEWTAB, B_COPY, B_SELALL, B_FIND, B_OPENLINK, B_LINKTAB, B_COPYLINK, B_NEXTTAB, B_PREVTAB };

static Vec<char*> global_history;

// a picture on the current page, fetched in the background after the html arrives
struct WebImage {
    char* url;
    Image img;               // decoded (shrunk to a sane size)
    Image disp;              // cached copy scaled to how big the page shows it
    int nw, nh;              // natural size
    int dw, dh;              // size disp was made for
    enum { QUEUED, LOADING, OK, FAILED } state;
    HttpRequest* req;
};
static const int MAX_IMG_LOADS = 3;

// a linked style sheet
struct WebSheet {
    char* url;
    char* text;
    u32 len;
    enum { QUEUED, LOADING, DONE, FAILED } state;
    HttpRequest* req;
};

struct BeamTab : App {
    App* owner = nullptr;                    // the Beam window this tab lives in
    int yoff = 0;                            // where the tab's area starts inside the window (below the tab strip)
    bool hot2(int x, int y, int w, int h) { return hot(x, y + yoff, w, h); }
    void set_title(const char* t);           // only the front tab titles the window
    void open_in_new_tab(const char* u);
    Vec<WebImage*> images;
    Vec<WebSheet*> sheets;
    char* ext_css = nullptr;
    bool show_css = true;
    Vec<int> img_map;                 // doc.img_urls index -> images index
    Vec<int> vis_bottom, vis_top;     // prefix max of item bottoms / suffix min of item tops
    bool show_images = true;
    bool img_relayout = false;
    u64 img_relayout_t = 0;
    char url[600] = "";
    char tbuf[96];
    TextField addr;
    bool addr_focus = false;
    bool addr_fresh = false;          // focused but untouched: the first key replaces the url
    Vec<char*> back, fwd;
    HttpRequest* req = nullptr;
    u64 load_t0 = 0;
    char* src = nullptr;              // the page source (owned)
    u32 src_len = 0;
    bool plain = false, view_source = false, secure = false;
    char tlsv[12] = "";
    HtmlDoc doc;
    Smooth sc;
    ScrollBar sb;
    int cw = 0, ch = 0;
    int hover_link = -1;
    int focus_field = -1;
    char status[160] = "";
    int layout_w = 0;
    int phase = -1;
    // text selection (positions are a text item plus a caret between characters)
    bool sel_on = false, sel_drag = false, sel_moved = false;
    int sa_i = 0, sa_c = 0, sb_i = 0, sb_c = 0;
    int down_x = 0, down_y = 0;
    // find in page
    bool find_on = false, find_focus = false;
    TextField fq;
    Vec<int> fm_item, fm_pos;
    int fm_len = 0, fcur = -1;
    char fq_last[128] = "";
    // context menu
    Menu ctxm;
    char ctx_link[600] = "";

    BeamTab(const char* start) {
        memset(&doc, 0, sizeof(doc));
        navigate(start && start[0] ? start : "silicon://start", false);
    }
    ~BeamTab() {
        clear_images();
        clear_sheets();
        sheets.free();
        html_free_css(doc);
        images.free();
        img_map.free();
        vis_bottom.free();
        vis_top.free();
        if (req) http_free(req);
        if (src) kfree(src);
        doc.clear();
        doc.items.free(); doc.links.free(); doc.fields.free(); doc.forms.free();
        if (doc.pool) kfree(doc.pool);
        for (int i = 0; i < back.len; i++) kfree(back[i]);
        for (int i = 0; i < fwd.len; i++) kfree(fwd[i]);
        back.free(); fwd.free();
    }

    const char* app_id() override { return "browser"; }
    const char* title() override {
        const char* t = doc.title[0] ? doc.title : url;
        snprintf(tbuf, sizeof(tbuf), "%s%s", req ? "loading " G_ELLIP " " : "", t);
        return tbuf;
    }
    int pref_w() override { return 780; }
    int pref_h() override { return 540; }
    u32 background() override { return doc.bg ? doc.bg : 0xFFFBF9FF; }
    int cursor_at(int x, int y) override {
        if (y > 6 && y < 32 && x > 132 && x < cw - 44) return CUR_IBEAM;
        if (y > TB_H && y < ch - ST_H) {
            int i = hit_item(x, y);
            if (i >= 0) return doc.items[i].link >= 0 || doc.items[i].kind == HK_BUTTON || doc.items[i].kind == HK_CHECK ? CUR_HAND : CUR_IBEAM;
        }
        return CUR_ARROW;
    }
    void open_arg(const char* arg) override { if (arg) navigate(arg, true); }

    // ------------------------------------------------------------ page content
    void set_source(const char* data, u32 len, bool is_plain) {
        if (src) kfree(src);
        src = (char*)kmalloc(len + 1);
        memcpy(src, data, len);
        src[len] = 0;
        src_len = len;
        plain = is_plain;
        view_source = false;
        doc.bg = doc.fg = 0;
        focus_field = -1;
        hover_link = -1;
        relayout(false);
        sc.jump(0);
        set_title(title());
    }
    void relayout(bool keep_fields) {
        if (!src) return;
        layout_w = cw - 12;
        HtmlDoc old;
        memset(&old, 0, sizeof(old));
        if (keep_fields) { for (int i = 0; i < doc.fields.len; i++) old.fields.push(doc.fields[i]); }
        u32 bg = doc.bg, fg = doc.fg;
        doc.bg = view_source ? 0 : bg;
        doc.fg = view_source ? 0 : fg;
        doc.img_size = &BeamTab::img_size_cb;
        doc.ext_css = show_css ? ext_css : nullptr;
        doc.ext_css_len = show_css && ext_css ? (u32)strlen(ext_css) : 0;
        doc.img_ctx = this;
        html_layout(src, src_len, url, layout_w, doc, plain || view_source, keep_fields ? &old : nullptr);
        old.fields.free();
        // items come out of the layout almost sorted by y: these let draw() skip to the visible ones
        int n = doc.items.len;
        vis_bottom.clear();
        vis_top.clear();
        for (int i = 0; i < n; i++) { vis_bottom.push(0); vis_top.push(0); }
        for (int i = 0, m = -(1 << 30); i < n; i++) { m = max(m, doc.items[i].y + doc.items[i].h); vis_bottom[i] = m; }
        for (int i = n - 1, m = 1 << 30; i >= 0; i--) { m = min(m, doc.items[i].y); vis_top[i] = m; }
        sel_on = sel_drag = false;
        sync_images();
        sync_sheets();
        if (find_on) rebuild_matches(false);
        ui_invalidate();
    }

    // ------------------------------------------------------------ style sheets
    void clear_sheets() {
        for (int i = 0; i < sheets.len; i++) {
            WebSheet* w = sheets[i];
            if (w->req) http_free(w->req);
            if (w->text) kfree(w->text);
            kfree(w->url);
            delete w;
        }
        sheets.clear();
        if (ext_css) kfree(ext_css);
        ext_css = nullptr;
        doc.ext_css = nullptr;
        doc.ext_css_len = 0;
    }
    void add_sheet(const char* u, int at) {
        for (int k = 0; k < sheets.len; k++) if (!strcmp(sheets[k]->url, u)) return;
        if (sheets.len >= 16) return;
        WebSheet* w = new WebSheet();
        memset(w, 0, sizeof(*w));
        w->url = strdup(u);
        w->state = WebSheet::QUEUED;
        if (at < 0 || at > sheets.len) sheets.push(w); else sheets.insert(at, w);
    }
    void sync_sheets() {
        bool web = str_ieq_prefix(url, "http://") || str_ieq_prefix(url, "https://");
        if (!web || !show_css || view_source) return;
        for (int i = 0; i < doc.css_urls.len; i++)
            if (str_ieq_prefix(doc.css_urls[i], "http://") || str_ieq_prefix(doc.css_urls[i], "https://")) add_sheet(doc.css_urls[i], -1);
    }
    int sheets_pending() {
        int n = 0;
        for (int i = 0; i < sheets.len; i++) n += sheets[i]->state <= WebSheet::LOADING;
        return n;
    }
    // @import "x" / @import url(x): fetched too (one level)
    void find_imports(WebSheet* w, int idx) {
        const char* t = w->text;
        int at = idx;
        for (const char* p = t; *p; p++) {
            if (*p != '@' || !str_ieq_prefix(p, "@import")) continue;
            const char* q = p + 7;
            while (*q == ' ') q++;
            if (str_ieq_prefix(q, "url(")) q += 4;
            while (*q == ' ' || *q == '"' || *q == '\'') q++;
            char rel[300];
            int o = 0;
            while (*q && *q != '"' && *q != '\'' && *q != ')' && *q != ';' && *q != ' ' && o < 299) rel[o++] = *q++;
            rel[o] = 0;
            if (!o) continue;
            char full[600];
            url_resolve(w->url, rel, full, sizeof(full));
            add_sheet(full, at++);
        }
    }
    void rebuild_css() {
        u32 total = 0;
        for (int i = 0; i < sheets.len; i++) if (sheets[i]->state == WebSheet::DONE) total += sheets[i]->len + 1;
        if (ext_css) kfree(ext_css);
        ext_css = nullptr;
        if (!total) return;
        ext_css = (char*)kmalloc(total + 1);
        u32 o = 0;
        for (int i = 0; i < sheets.len; i++) {
            WebSheet* w = sheets[i];
            if (w->state != WebSheet::DONE) continue;
            memcpy(ext_css + o, w->text, w->len);
            o += w->len;
            ext_css[o++] = '\n';
        }
        ext_css[o] = 0;
    }
    // returns how many sheet downloads are running
    int pump_sheets() {
        int active = 0;
        bool finished = false;
        for (int i = 0; i < sheets.len; i++) {
            WebSheet* w = sheets[i];
            if (w->state != WebSheet::LOADING) continue;
            int ph = http_step(w->req);
            if (ph == HTTP_DONE) {
                u32 len;
                const u8* body = http_body(w->req, &len);
                if (http_status(w->req) < 400 && len < 2 * 1024 * 1024) {
                    w->text = (char*)kmalloc(len + 1);
                    memcpy(w->text, body, len);
                    w->text[len] = 0;
                    w->len = len;
                    w->state = WebSheet::DONE;
                    find_imports(w, i);
                } else w->state = WebSheet::FAILED;
                http_free(w->req);
                w->req = nullptr;
                finished = true;
            } else if (ph == HTTP_ERROR) {
                w->state = WebSheet::FAILED;
                http_free(w->req);
                w->req = nullptr;
                finished = true;
            } else active++;
        }
        for (int i = 0; i < sheets.len && active < 4 && !req; i++) {
            WebSheet* w = sheets[i];
            if (w->state != WebSheet::QUEUED) continue;
            w->req = http_get(w->url);
            http_set_accept(w->req, "text/css,*/*;q=0.1");
            w->state = WebSheet::LOADING;
            active++;
        }
        // restyle once every sheet is in (one re-layout instead of one per sheet)
        if (finished && !sheets_pending()) {
            rebuild_css();
            relayout(true);
        }
        if (active) ui_invalidate();
        return active;
    }

    // ------------------------------------------------------------ pictures
    static bool img_size_cb(void* ctx, const char* u, int* w, int* h) {
        BeamTab* b = (BeamTab*)ctx;
        for (int i = 0; i < b->images.len; i++) {
            WebImage* wi = b->images[i];
            if (wi->state == WebImage::OK && !strcmp(wi->url, u)) { *w = wi->nw; *h = wi->nh; return true; }
        }
        return false;
    }
    void free_image(WebImage* wi) {
        if (wi->req) http_free(wi->req);
        if (wi->img.px) kfree(wi->img.px);
        if (wi->disp.px) kfree(wi->disp.px);
        kfree(wi->url);
        delete wi;
    }
    void clear_images() {
        for (int i = 0; i < images.len; i++) free_image(images[i]);
        images.clear();
        img_map.clear();
    }
    // match the layout's image list against what's loaded / queued
    void sync_images() {
        img_map.clear();
        bool web = str_ieq_prefix(url, "http://") || str_ieq_prefix(url, "https://");
        for (int i = 0; i < doc.img_urls.len; i++) {
            const char* u = doc.img_urls[i];
            int found = -1;
            for (int k = 0; k < images.len && found < 0; k++) if (!strcmp(images[k]->url, u)) found = k;
            if (found < 0 && web && show_images && !view_source &&
                (str_ieq_prefix(u, "http://") || str_ieq_prefix(u, "https://"))) {
                WebImage* wi = new WebImage();
                memset(wi, 0, sizeof(*wi));
                wi->url = strdup(u);
                wi->state = WebImage::QUEUED;
                images.push(wi);
                found = images.len - 1;
            }
            img_map.push(found);
        }
    }
    // decoded pictures on this page stay under this many bytes; later ones show their placeholder
    static const u32 IMG_BUDGET = 48u * 1024 * 1024;
    u32 image_bytes() {
        u32 n = 0;
        for (int i = 0; i < images.len; i++) {
            n += (u32)images[i]->img.w * images[i]->img.h * 4;
            n += (u32)images[i]->disp.w * images[i]->disp.h * 4;
        }
        return n;
    }
    WebImage* image_for(const HtmlItem& it) {
        if (it.img < 0 || it.img >= img_map.len || img_map[it.img] < 0) return nullptr;
        WebImage* wi = images[img_map[it.img]];
        return wi->state == WebImage::OK ? wi : nullptr;
    }
    int images_pending() {
        int n = 0;
        for (int i = 0; i < images.len; i++) n += images[i]->state <= WebImage::LOADING;
        return n;
    }
    void pump_images() {
        int active = 0;
        for (int i = 0; i < images.len; i++) {
            WebImage* wi = images[i];
            if (wi->state != WebImage::LOADING) continue;
            int ph = http_step(wi->req);
            if (ph == HTTP_DONE) {
                u32 len;
                const u8* body = http_body(wi->req, &len);
                if (http_status(wi->req) < 400 && len <= 6 * 1024 * 1024 && image_bytes() < IMG_BUDGET &&
                    image_load(body, len, wi->img)) {
                    wi->nw = wi->img.w;
                    wi->nh = wi->img.h;
                    image_shrink(wi->img, 760, 760);
                    wi->state = WebImage::OK;
                    // only re-flow when the page didn't already reserve the right space
                    for (int k = 0; k < img_map.len; k++)
                        if (img_map[k] == i && k < doc.img_sized.len && !doc.img_sized[k]) img_relayout = true;
                } else wi->state = WebImage::FAILED;
                http_free(wi->req);
                wi->req = nullptr;
                ui_invalidate();
            } else if (ph == HTTP_ERROR) {
                wi->state = WebImage::FAILED;
                http_free(wi->req);
                wi->req = nullptr;
            } else active++;
        }
        for (int i = 0; i < images.len && active < MAX_IMG_LOADS && !req; i++) {
            WebImage* wi = images[i];
            if (wi->state != WebImage::QUEUED) continue;
            wi->req = http_get(wi->url);
            http_set_accept(wi->req, "image/png,image/gif,image/jpeg,image/bmp;q=0.9,*/*;q=0.1");
            wi->state = WebImage::LOADING;
            active++;
        }
        // re-flow the page as pictures arrive, but not on every single one
        if (img_relayout && (ticks() - img_relayout_t > 400 || !active)) {
            img_relayout = false;
            img_relayout_t = ticks();
            relayout(true);
        }
        if (active) ui_invalidate();
    }
    // the decoded picture scaled to the size the page wants (cached)
    const Image& display_image(WebImage* wi, int w, int h) {
        if (w >= wi->img.w || h >= wi->img.h) return wi->img;      // nearest-neighbour upscale is fine
        if (wi->disp.px && wi->dw == w && wi->dh == h) return wi->disp;
        wi->dw = w;
        wi->dh = h;
        if (wi->disp.px) kfree(wi->disp.px);
        wi->disp.w = wi->img.w;
        wi->disp.h = wi->img.h;
        wi->disp.px = (u32*)kmalloc_try((size_t)wi->img.w * wi->img.h * 4);
        if (!wi->disp.px) { wi->disp.w = wi->disp.h = 0; return wi->img; }
        memcpy(wi->disp.px, wi->img.px, (size_t)wi->img.w * wi->img.h * 4);
        image_shrink(wi->disp, w, h);
        return wi->disp;
    }
    void error_page(const char* what, const char* detail) {
        char html[1400];
        snprintf(html, sizeof(html),
                 "<html><head><title>can't load page</title></head><body><h2>%s</h2><p>%s</p>"
                 "<p><code>%s</code></p><ul><li>check the address</li><li>is the network up? (try <code>ifconfig</code> in a terminal)</li>"
                 "<li>some sites need javascript, which Beam doesn't have</li></ul>"
                 "<p><a href=\"silicon://start\">start page</a></p></body></html>", what, detail, url);
        set_source(html, strlen(html), false);
    }

    // ------------------------------------------------------------ navigation
    void navigate(const char* target, bool push_history) {
        char u[600];
        while (*target == ' ') target++;
        if (!*target) return;
        bool has_scheme = strstr_ci(target, "://") || str_ieq_prefix(target, "silicon:") || str_ieq_prefix(target, "about:");
        bool looks_like_host = strchr(target, '.') && !strchr(target, ' ');
        if (target[0] == '/' ) snprintf(u, sizeof(u), "file://%s", target);
        else if (has_scheme) strlcpy(u, target, sizeof(u));
        else if (looks_like_host) snprintf(u, sizeof(u), "http://%s", target);
        else {
            char q[400];
            url_encode(target, q, sizeof(q));
            snprintf(u, sizeof(u), "https://lite.duckduckgo.com/lite/?q=%s", q);
        }
        if (str_ieq_prefix(u, "about:")) snprintf(u, sizeof(u), "silicon://%s", target + 6);
        // search engines wrap results in javascript redirects: jump straight to the real page
        if (const char* w = strstr_ci(u, "duckduckgo.com/l/?uddg=")) {
            const char* s = w + 23;
            char d[600];
            int o = 0;
            auto hex = [](char c) { return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'; };
            while (*s && *s != '&' && o < 599) {
                if (*s == '%' && s[1] && s[2]) { d[o++] = (char)(hex(s[1]) * 16 + hex(s[2])); s += 3; }
                else if (*s == '+') { d[o++] = ' '; s++; }
                else d[o++] = *s++;
            }
            d[o] = 0;
            strlcpy(u, d, sizeof(u));
        }
        if (str_ieq_prefix(u, "javascript:")) { strlcpy(status, "javascript links don't work in Beam", sizeof(status)); ui_invalidate(); return; }
        if (str_ieq_prefix(u, "mailto:")) { ui_notify("Beam", "there's no mail app yet", "browser"); return; }
        if (push_history && url[0]) {
            back.push(strdup(url));
            for (int i = 0; i < fwd.len; i++) kfree(fwd[i]);
            fwd.clear();
        }
        strlcpy(url, u, sizeof(url));
        addr.set(url);
        clear_images();
        clear_sheets();
        if (req) { http_free(req); req = nullptr; }
        secure = false;
        status[0] = 0;
        global_history.push(strdup(url));
        if (global_history.len > 200) { kfree(global_history[0]); global_history.remove(0); }

        if (str_ieq_prefix(url, "silicon://")) {
            const char* page = url + 10;
            if (str_starts(page, "about")) set_source(ABOUT_PAGE, strlen(ABOUT_PAGE), false);
            else if (str_starts(page, "bookmarks")) bookmarks_page();
            else if (str_starts(page, "history")) history_page();
            else set_source(START_PAGE, strlen(START_PAGE), false);
            return;
        }
        if (str_ieq_prefix(url, "file://")) {
            FsNode* f = fs_lookup(url + 7);
            if (!f || f->dir) { error_page("file not found", "there's no such file in SiliconFS"); return; }
            bool html = str_icontains(f->name, ".htm");
            set_source((const char*)fs_read_text(f), f->size, !html);
            return;
        }
        if (!net_link_up()) { error_page("no network", "silicon has no network connection right now"); return; }
        req = http_get(url);
        load_t0 = ticks();
        set_title(title());
        ui_invalidate();
    }

    void go_back() {
        if (!back.len) return;
        fwd.push(strdup(url));
        char* p = back.back();
        back.len--;
        navigate(p, false);
        kfree(p);
    }
    void go_fwd() {
        if (!fwd.len) return;
        back.push(strdup(url));
        char* p = fwd.back();
        fwd.len--;
        navigate(p, false);
        kfree(p);
    }

    static const char* strstr_ci(const char* h, const char* n) {
        for (; *h; h++) if (str_ieq_prefix(h, n)) return h;
        return nullptr;
    }

    void bookmarks_page() {
        FsNode* f = fs_lookup("/home/.config/beam/bookmarks");
        char* html = (char*)kmalloc(16384);
        int o = snprintf(html, 16384, "<html><head><title>bookmarks</title></head><body><h1>bookmarks</h1><ul>");
        if (f) {
            const char* t = fs_read_text(f);
            while (*t && o < 15000) {
                const char* e = strchr(t, '\n');
                int l = e ? (int)(e - t) : (int)strlen(t);
                char ln[700];
                memcpy(ln, t, min(l, 699));
                ln[min(l, 699)] = 0;
                char* bar = strchr(ln, '|');
                if (bar) { *bar = 0; o += snprintf(html + o, 16384 - o, "<li><a href=\"%s\">%s</a></li>", bar + 1, ln); }
                t += l + (e ? 1 : 0);
            }
        } else o += snprintf(html + o, 16384 - o, "<li>no bookmarks yet - press the star to add one</li>");
        snprintf(html + o, 16384 - o, "</ul></body></html>");
        set_source(html, strlen(html), false);
        kfree(html);
    }
    void history_page() {
        char* html = (char*)kmalloc(32768);
        int o = snprintf(html, 32768, "<html><head><title>history</title></head><body><h1>history</h1><ol>");
        for (int i = global_history.len - 1; i >= 0 && o < 31000; i--)
            o += snprintf(html + o, 32768 - o, "<li><a href=\"%s\">%s</a></li>", global_history[i], global_history[i]);
        snprintf(html + o, 32768 - o, "</ol></body></html>");
        set_source(html, strlen(html), false);
        kfree(html);
    }
    void add_bookmark() {
        fs_mkdirs("/home/.config/beam");
        FsNode* f = fs_create_path("/home/.config/beam/bookmarks", false);
        char line[800];
        snprintf(line, sizeof(line), "%s|%s\n", doc.title[0] ? doc.title : url, url);
        fs_append(f, line, strlen(line));
        ui_notify("Bookmarked", doc.title[0] ? doc.title : url, "browser");
    }
    void save_page() {
        if (!src) return;
        fs_mkdirs("/home/downloads");
        char name[FS_NAME_MAX];
        fs_unique_name(fs_lookup("/home/downloads"), plain ? "page.txt" : "page.html", name, sizeof(name));
        char path[128];
        snprintf(path, sizeof(path), "/home/downloads/%s", name);
        fs_write_text(path, src);
        ui_notify("Saved", path, "browser");
    }

    // ------------------------------------------------------------ forms
    void submit(int form) {
        if (form < 0 || form >= doc.forms.len) return;
        char q[1500] = "";
        for (int i = 0; i < doc.fields.len; i++) {
            HtmlField& f = doc.fields[i];
            if (f.form != form || !f.name[0]) continue;
            if ((!strcmp(f.type, "checkbox") || !strcmp(f.type, "radio")) && !f.checked) continue;
            if (!strcmp(f.type, "submit") || !strcmp(f.type, "button") || !strcmp(f.type, "reset")) continue;
            char en[600], ev[600];
            url_encode(f.name, en, sizeof(en));
            url_encode(f.value, ev, sizeof(ev));
            if (q[0]) strlcat(q, "&", sizeof(q));
            strlcat(q, en, sizeof(q));
            strlcat(q, "=", sizeof(q));
            strlcat(q, ev, sizeof(q));
        }
        HtmlForm& fm = doc.forms[form];
        if (fm.post) {
            back.push(strdup(url));
            strlcpy(url, fm.action, sizeof(url));
            addr.set(url);
            if (req) http_free(req);
            req = http_get(url, "POST", q);
            load_t0 = ticks();
        } else {
            char target[1200];
            char act[512];
            strlcpy(act, fm.action, sizeof(act));
            char* qm = strchr(act, '?');
            if (qm) *qm = 0;
            snprintf(target, sizeof(target), "%s?%s", act, q);
            navigate(target, true);
        }
    }

    // ------------------------------------------------------------ loading
    void tick() override {
        if (sc.step()) ui_invalidate();
        if (!req) {
            int css_busy = sheets.len ? pump_sheets() : 0;
            if (images.len && !css_busy && !sheets_pending()) pump_images();
            return;
        }
        int ph = http_step(req);
        if (ph != phase) { phase = ph; set_title(title()); }
        ui_invalidate();
        if (ph == HTTP_ERROR) {
            char msg[160];
            strlcpy(msg, http_error(req), sizeof(msg));
            http_free(req);
            req = nullptr;
            error_page("can't reach this page", msg);
            return;
        }
        if (ph != HTTP_DONE) return;
        strlcpy(url, http_final_url(req), sizeof(url));
        addr.set(url);
        secure = str_ieq_prefix(url, "https://");
        strlcpy(tlsv, http_tls_version(req), sizeof(tlsv));
        u32 len;
        const u8* body = http_body(req, &len);
        const char* ct = http_content_type(req);
        int status = http_status(req);
        bool html = !ct[0] || str_icontains(ct, "html");
        bool text = html || str_icontains(ct, "text/") || str_icontains(ct, "json") || str_icontains(ct, "xml") || str_icontains(ct, "javascript");
        bool is_pdf = str_icontains(ct, "pdf") || (len > 5 && !memcmp(body, "%PDF-", 5));
        if (is_pdf && status < 400) {
            // save the document and hand it to the PDF viewer
            char base[FS_NAME_MAX] = "document.pdf";
            const char* u = url;
            const char* q = strchr(u, '?');
            int ulen = q ? (int)(q - u) : (int)strlen(u);
            int st = ulen;
            while (st > 0 && u[st - 1] != '/') st--;
            if (ulen - st > 0 && ulen - st < (int)sizeof(base) - 5) {
                memcpy(base, u + st, ulen - st);
                base[ulen - st] = 0;
                if (!str_icontains(base, ".pdf")) strlcat(base, ".pdf", sizeof(base));
            }
            fs_mkdirs("/home/downloads");
            char name[FS_NAME_MAX];
            fs_unique_name(fs_lookup("/home/downloads"), base, name, sizeof(name));
            char path[200];
            snprintf(path, sizeof(path), "/home/downloads/%s", name);
            FsNode* f = fs_create_path(path, false);
            char page[600];
            if (f && fs_write(f, body, len)) {
                snprintf(page, sizeof(page), "<html><head><title>%s</title></head><body><h2>%s</h2>"
                         "<p>This is a PDF. It was saved to <code>%s</code> and opened in the PDF viewer.</p></body></html>", name, name, path);
                set_source(page, strlen(page), false);
                http_free(req);
                req = nullptr;
                set_title(title());
                open_path(path);
                return;
            }
            snprintf(page, sizeof(page), "<html><head><title>download</title></head><body><h2>couldn't save the PDF</h2>"
                     "<p>there is no room for it in SiliconFS.</p></body></html>");
            set_source(page, strlen(page), false);
            http_free(req);
            req = nullptr;
            set_title(title());
            return;
        }
        if (!text) {
            char page[800];
            snprintf(page, sizeof(page), "<html><head><title>download</title></head><body><h2>this isn't a web page</h2>"
                     "<p>the server sent <code>%s</code> (%u bytes). Beam can't show it, but you can save it with File > Save page.</p></body></html>",
                     ct, len);
            set_source(page, strlen(page), false);
            if (src) kfree(src);
            src = (char*)kmalloc(len + 1);
            memcpy(src, body, len);
            src[len] = 0;
            src_len = len;
        } else set_source((const char*)body, len, !html);
        if (status >= 400) snprintf(status_msg(), 160, "the server answered %d", status);
        http_free(req);
        req = nullptr;
        set_title(title());
    }
    char* status_msg() { return status; }

    // ------------------------------------------------------------ selection and find
    bool has_selection() { return sel_on && (sa_i != sb_i || sa_c != sb_c); }
    void sel_order(int* li, int* lc, int* hi, int* hc) {
        if (sb_i < sa_i || (sb_i == sa_i && sb_c < sa_c)) { *li = sb_i; *lc = sb_c; *hi = sa_i; *hc = sa_c; }
        else { *li = sa_i; *lc = sa_c; *hi = sb_i; *hc = sb_c; }
    }
    // the text item and caret nearest to a point in the view
    bool text_at(int mx, int my, int* item, int* caret) {
        int cy = my - TB_H + sc.pos();
        int best = -1, bd = 1 << 30;
        for (int i = 0; i < doc.items.len; i++) {
            HtmlItem& it = doc.items[i];
            if (it.kind != HK_TEXT || !it.tlen) continue;
            int dy = cy < it.y ? it.y - cy : cy >= it.y + it.h ? cy - (it.y + it.h) + 1 : 0;
            if (dy * 8 > bd) continue;
            int dx = mx < it.x ? it.x - mx : mx >= it.x + it.w ? mx - (it.x + it.w) + 1 : 0;
            int d = dy * 8 + dx;
            if (d < bd) { bd = d; best = i; }
        }
        if (best < 0) return false;
        HtmlItem& it = doc.items[best];
        *item = best;
        *caret = clamp((mx - it.x + 4 * it.scale) / (8 * it.scale), 0, (int)it.tlen);
        return true;
    }
    void select_word_at(int mx, int my) {
        int i, c;
        if (!text_at(mx, my, &i, &c)) return;
        const char* t = doc.str(doc.items[i]);
        int n = doc.items[i].tlen;
        int a = clamp(c, 0, n - 1), b = a;
        if (t[a] == ' ') { sel_on = false; return; }
        while (a > 0 && t[a - 1] != ' ') a--;
        while (b < n && t[b] != ' ') b++;
        sel_on = true; sa_i = sb_i = i; sa_c = a; sb_c = b;
    }
    void select_all_text() {
        int first = -1, last = -1;
        for (int i = 0; i < doc.items.len; i++) if (doc.items[i].kind == HK_TEXT && doc.items[i].tlen) { if (first < 0) first = i; last = i; }
        if (first < 0) return;
        sel_on = true; sa_i = first; sa_c = 0; sb_i = last; sb_c = doc.items[last].tlen;
    }
    void copy_selection() {
        if (!has_selection()) return;
        int li, lc, hi, hc;
        sel_order(&li, &lc, &hi, &hc);
        u32 cap = 4096, len = 0;
        char* buf = (char*)kmalloc(cap);
        const HtmlItem* prev = nullptr;
        for (int i = li; i <= hi && i < doc.items.len; i++) {
            HtmlItem& it = doc.items[i];
            if (it.kind != HK_TEXT || !it.tlen) continue;
            int a = i == li ? lc : 0, b = i == hi ? hc : it.tlen;
            if (b <= a) continue;
            if (len + (u32)(b - a) + 4 > cap) {
                while (len + (u32)(b - a) + 4 > cap) cap *= 2;
                buf = (char*)krealloc(buf, cap);
            }
            if (prev) {
                if (it.y >= prev->y + prev->h - 2) buf[len++] = '\n';
                else if (it.x > prev->x + prev->w + 2) buf[len++] = ' ';
            }
            memcpy(buf + len, doc.str(it) + a, b - a);
            len += b - a;
            prev = &it;
        }
        buf[len] = 0;
        char* u = (char*)kmalloc(len * 4 + 8);
        silicon_to_utf8(buf, len, u, len * 4 + 8);
        clip_set(u, (u32)strlen(u));
        kfree(u);
        kfree(buf);
    }
    bool match_here(const char* t, int n, int p, const char* q, int ql) {
        if (p + ql > n) return false;
        for (int k = 0; k < ql; k++) if (to_lower(t[p + k]) != to_lower(q[k])) return false;
        return true;
    }
    void rebuild_matches(bool jump) {
        fm_item.clear(); fm_pos.clear();
        fcur = -1;
        fm_len = (int)strlen(fq.text);
        if (!fm_len || !src) return;
        for (int i = 0; i < doc.items.len && fm_item.len < 800; i++) {
            HtmlItem& it = doc.items[i];
            if (it.kind != HK_TEXT || !it.tlen) continue;
            const char* t = doc.str(it);
            for (int p = 0; p + fm_len <= (int)it.tlen; p++)
                if (match_here(t, it.tlen, p, fq.text, fm_len)) { fm_item.push(i); fm_pos.push(p); p += fm_len - 1; }
        }
        if (fm_item.len) {
            // start at the first match at or below the top of the view
            int top = sc.pos();
            fcur = 0;
            for (int k = 0; k < fm_item.len; k++) if (doc.items[fm_item[k]].y >= top) { fcur = k; break; }
            if (jump) goto_match();
        }
    }
    void goto_match() {
        if (fcur < 0 || fcur >= fm_item.len) return;
        HtmlItem& it = doc.items[fm_item[fcur]];
        if (it.y < sc.target + 20 || it.y > sc.target + view_h() - 60) sc.set(clamp(it.y - view_h() / 3, 0, max_scroll()));
    }
    void find_step(int dir) {
        if (fq.text[0] && strcmp(fq.text, fq_last)) { strlcpy(fq_last, fq.text, sizeof(fq_last)); rebuild_matches(true); ui_invalidate(); return; }
        if (!fm_item.len) return;
        fcur = (fcur + dir + fm_item.len) % fm_item.len;
        goto_match();
        ui_invalidate();
    }
    void toggle_find() {
        find_on = !find_on || !find_focus;
        find_focus = find_on;
        fq.focused = find_on;
        if (find_on) { fq.select_all(); if (fq.text[0]) rebuild_matches(false); }
        else { fm_item.clear(); fm_pos.clear(); fcur = -1; }
        ui_invalidate();
    }
    Rect find_rect() { return {cw - 380, ch - ST_H - 32, 372, 28}; }
    void draw_find(Canvas& c) {
        Rect r = find_rect();
        draw_bevel(c, r.x, r.y, r.w, r.h, th::panel);
        fq.focused = find_focus;
        fq.draw(c, r.x + 6, r.y + 3, 190);
        char b[32];
        if (fm_item.len) snprintf(b, sizeof(b), "%d/%d", fcur + 1, fm_item.len);
        else snprintf(b, sizeof(b), fq.text[0] ? "none" : "");
        c.text(r.x + 204, r.y + 8, b, fq.text[0] && !fm_item.len ? th::red : th::muted);
        draw_button(c, r.x + 282, r.y + 3, 24, 22, "<", hot2(r.x + 282, r.y + 3, 24, 22));
        draw_button(c, r.x + 310, r.y + 3, 24, 22, ">", hot2(r.x + 310, r.y + 3, 24, 22));
        draw_button(c, r.x + 340, r.y + 3, 24, 22, "x", hot2(r.x + 340, r.y + 3, 24, 22));
    }
    void find_mouse(const MouseEv& e) {
        Rect r = find_rect();
        if (e.kind == M_DOWN && Rect{r.x + 6, r.y + 3, 190, 22}.contains(e.x, e.y)) { find_focus = true; addr_focus = false; focus_field = -1; fq.click(e.x - (r.x + 6), 190, kbd_shift_down()); }
        else if (e.kind == M_MOVE && e.left && find_focus) fq.drag(e.x - (r.x + 6), 190);
        else if (e.kind == M_UP && e.button == 1) {
            if (Rect{r.x + 282, r.y + 3, 24, 22}.contains(e.x, e.y)) find_step(-1);
            else if (Rect{r.x + 310, r.y + 3, 24, 22}.contains(e.x, e.y)) find_step(1);
            else if (Rect{r.x + 340, r.y + 3, 24, 22}.contains(e.x, e.y)) { find_on = find_focus = false; fm_item.clear(); fm_pos.clear(); fcur = -1; }
        }
        ui_invalidate();
    }
    void draw_text_marks(Canvas& c, int i, HtmlItem& it, int x, int y) {
        int cw8 = 8 * it.scale;
        if (find_on && fm_len) {
            for (int k = 0; k < fm_item.len; k++) {
                if (fm_item[k] < i) continue;
                if (fm_item[k] > i) break;
                bool cur = k == fcur;
                c.fill_alpha(x + fm_pos[k] * cw8, y - 1, fm_len * cw8, it.h + 2, cur ? 0xFFFF8C1A : 0xFFFFD21E, cur ? 160 : 120);
            }
        }
        if (has_selection()) {
            int li, lc, hi, hc;
            sel_order(&li, &lc, &hi, &hc);
            if (i >= li && i <= hi) {
                int a = i == li ? lc : 0, b = i == hi ? hc : it.tlen;
                if (b > a) c.fill_alpha(x + a * cw8, y - 1, (b - a) * cw8, it.h + 2, 0xFF2D62E0, 110);
            }
        }
    }
    void context_menu(const MouseEv& e) {
        int i = hit_item(e.x, e.y);
        ctxm.clear();
        ctx_link[0] = 0;
        if (i >= 0 && doc.items[i].link >= 0) {
            strlcpy(ctx_link, doc.links[doc.items[i].link], sizeof(ctx_link));
            ctxm.add("Open link", B_OPENLINK).add("Open in new tab", B_LINKTAB).add("Copy link address", B_COPYLINK).sep();
        }
        ctxm.add("Copy", B_COPY, "ctrl+c", has_selection()).add("Select all", B_SELALL, "ctrl+a").sep()
            .add("Back", B_BACK, "alt+left", back.len > 0).add("Forward", B_FWD, "alt+right", fwd.len > 0).add("Reload", B_RELOAD, "f5").sep()
            .add("Find in page" G_ELLIP, B_FIND, "ctrl+f").add("Page source", B_SOURCE, "", true, view_source).add("Save page", B_SAVE, "", src != nullptr);
        Rect cl = win->client();
        menu_popup(&ctxm, cl.x + e.x, cl.y + e.y + yoff, ctx_fn, owner);
    }
    static void ctx_fn(void* ctx, int cmd) { ((App*)ctx)->on_command(cmd); }

    // ------------------------------------------------------------ drawing
    int view_h() { return ch - TB_H - ST_H; }
    int max_scroll() { return max(0, doc.height - view_h()); }

    void draw(Canvas& c, int w, int h) override {
        if (w != cw || h != ch) { cw = w; ch = h; }
        if (src && layout_w != cw - 12) relayout(true);
        // content
        int top = TB_H, vh = view_h();
        c.fill(0, top, w, vh, doc.bg ? doc.bg : 0xFFFBF9FF);
        c.set_clip({0, top, w - 11, vh});
        int sy = sc.pos();
        int first = 0, lo = 0, hi = vis_bottom.len;
        while (lo < hi) { int mid = (lo + hi) / 2; if (vis_bottom[mid] < sy) lo = mid + 1; else hi = mid; }
        first = lo;
        for (int i = first; i < doc.items.len; i++) {
            if (i < vis_top.len && vis_top[i] > sy + vh) break;
            HtmlItem& it = doc.items[i];
            int y = top + it.y - sy;
            if (y + it.h < top || y > top + vh) continue;
            int x = it.x;
            bool hov = it.link >= 0 && it.link == hover_link;
            switch (it.kind) {
            case HK_TEXT: {
                if (it.bg) c.fill(x - 1, y - 1, it.w + 2, it.h + 2, it.bg);
                if (hov) c.fill(x - 1, y - 1, it.w + 2, it.h + 2, blend(0xFFFFFFFF, accent(), 70));
                draw_text_marks(c, i, it, x, y);
                c.emoji_px = it.scale >= 2 ? 16 : 12;       // headings get the big emoji sprites
                c.text_n(x, y, doc.str(it), it.tlen, it.color, it.scale, it.bold);
                c.emoji_px = 12;
                if (it.underline) c.hline(x, y + 11 * it.scale, it.w, hov ? accent() : it.color);
                if (it.strike) c.hline(x, y + 6 * it.scale, it.w, it.color);
                break;
            }
            case HK_RECT: c.fill(x, y, it.w, it.h, it.color); break;
            case HK_BULLET: c.fill(x + 2, y + 4, 4, 4, it.color); break;
            case HK_IMAGE: {
                if (WebImage* wi = image_for(it)) {
                    c.blit_scaled(display_image(wi, it.w, it.h), x, y, it.w, it.h);
                    if (hov) c.rect(x - 1, y - 1, it.w + 2, it.h + 2, accent());
                    break;
                }
                c.fill(x, y, it.w, it.h, 0xFFE4DEF0);
                c.rect(x, y, it.w, it.h, 0xFFB0A6C8);
                if (it.h >= 20) c.blit(icon("image_file")->small, x + 3, y + 3);
                if (it.tlen) c.text_n(x + 22, y + 5, doc.str(it), max(0, (it.w - 26) / 8), 0xFF746A8C);
                if (hov) c.rect(x - 1, y - 1, it.w + 2, it.h + 2, accent());
                break;
            }
            case HK_FIELD: {
                HtmlField& f = doc.fields[it.field];
                bool foc = it.field == focus_field;
                draw_bevel(c, x, y, it.w, it.h, th::white, true);
                if (foc) c.rect(x - 1, y - 1, it.w + 2, it.h + 2, accent());
                const char* v = !strcmp(f.type, "select") ? doc.str(it) : f.value;
                char shown[256];
                strlcpy(shown, v, sizeof(shown));
                if (!strcmp(f.type, "password")) for (char* p = shown; *p; p++) *p = '*';
                int maxc = (it.w - 10) / 8;
                int sl = (int)strlen(shown);
                int start = max(0, sl - maxc + 1);
                c.text_n(x + 5, y + 4, shown + start, maxc, th::text);
                if (!strcmp(f.type, "select")) c.text(x + it.w - 14, y + 4, G_TRIDN, th::muted);
                if (foc && (ticks() / 500) % 2 == 0) c.vline(x + 5 + (sl - start) * 8, y + 3, 14, accent());
                break;
            }
            case HK_BUTTON:
                draw_button(c, x, y, it.w, it.h, doc.str(it), hot2(x, y, it.w, it.h), true);
                break;
            case HK_CHECK: {
                HtmlField& f = doc.fields[it.field];
                draw_bevel(c, x, y, 14, 14, th::white, true);
                if (f.checked) c.fill(x + 3, y + 3, 8, 8, accent());
                break;
            }
            }
        }
        c.set_clip({0, 0, w, h});
        sb.draw(c, w - 10, top, vh, doc.height, vh, sc.pos());
        if (find_on) draw_find(c);

        // toolbar
        c.fill(0, 0, w, TB_H, th::panel);
        c.hline(0, TB_H - 1, w, th::border);
        draw_button(c, 8, 7, 26, 24, G_LEFT, hot2(8, 7, 26, 24), false, back.len > 0);
        draw_button(c, 38, 7, 26, 24, G_RIGHT, hot2(38, 7, 26, 24), false, fwd.len > 0);
        draw_button(c, 68, 7, 26, 24, req ? G_TIMES : "@", hot2(68, 7, 26, 24));
        draw_button(c, 98, 7, 28, 24, "~", hot2(98, 7, 28, 24));
        // address bar with a lock
        int ax = 132, aw = w - ax - 46;
        draw_bevel(c, ax, 7, aw, 24, th::white, true);
        if (addr_focus) c.rect(ax - 1, 6, aw + 2, 26, accent());
        u32 lockc = secure ? 0xFF2E8B57 : 0xFF9C92B8;
        c.fill(ax + 7, 15, 9, 7, lockc);
        c.rect(ax + 9, 11, 5, 5, lockc);
        int maxc = (aw - 32) / 8;
        const char* shown = addr.text;
        int start = addr_focus ? max(0, addr.cur - maxc + 1) : 0;
        if (addr_focus && addr_fresh) c.fill(ax + 22, 11, min((int)strlen(shown), maxc) * 8 + 4, 16, blend(0xFFFFFFFF, accent(), 110));
        c.text_n(ax + 24, 13, shown + start, maxc, addr_focus ? th::text : 0xFF4A4262);
        if (addr_focus && (ticks() / 500) % 2 == 0) c.vline(ax + 24 + (addr.cur - start) * 8, 11, 16, accent());
        draw_button(c, w - 38, 7, 30, 24, G_DIAM, hot2(w - 38, 7, 30, 24));
        // progress: a travelling "packet" while loading
        if (req) {
            int ph = phase;
            int pct = ph == HTTP_RESOLVING ? 10 : ph == HTTP_CONNECTING ? 25 : ph == HTTP_HANDSHAKE ? 40 : ph == HTTP_SENDING ? 50 : 60;
            u32 exp = http_expected(req);
            if (ph == HTTP_RECEIVING && exp) pct = 60 + (int)(min<u32>(http_received(req), exp) * 40 / exp);
            c.fill(0, TB_H - 3, w * pct / 100, 3, accent());
            int px = (int)((ticks() - load_t0) / 3 % (u64)(w + 60)) - 30;
            c.fill(px, TB_H - 3, 24, 3, blend(accent(), th::white, 160));
        }

        // status bar
        c.fill(0, h - ST_H, w, ST_H, th::panel);
        c.hline(0, h - ST_H, w, th::border);
        char st[200];
        if (hover_link >= 0 && hover_link < doc.links.len) snprintf(st, sizeof(st), G_RIGHT " %s", doc.links[hover_link]);
        else if (req) {
            static const char* names[] = {"looking up", "connecting to", "securing connection to", "asking", "receiving from", "", ""};
            Url u;
            url_parse(url, u);
            u32 got = http_received(req);
            snprintf(st, sizeof(st), "%s %s" G_ELLIP "  %s", names[clamp(phase, 0, 6)], u.host, "");
            if (got) { char b[24]; fmt_bytes(got, b, sizeof(b)); strlcat(st, b, sizeof(st)); }
        } else if (status[0]) strlcpy(st, status, sizeof(st));
        else if (int sp_ = sheets_pending()) snprintf(st, sizeof(st), "loading styles" G_ELLIP " %d to go", sp_);
        else if (int pend = images_pending()) snprintf(st, sizeof(st), "loading pictures" G_ELLIP " %d to go", pend);
        else if (secure) snprintf(st, sizeof(st), "encrypted (%s, certificate not verified)  " G_BULLET "  %d links", tlsv[0] ? tlsv : "tls", doc.links.len);
        else snprintf(st, sizeof(st), "%s  " G_BULLET "  %d links", str_ieq_prefix(url, "silicon:") ? "built-in page" : "not encrypted", doc.links.len);
        c.text_n(8, h - ST_H + 3, st, (w - 16) / 8, th::muted);
    }

    // ------------------------------------------------------------ input
    int hit_item(int mx, int my) {
        int cy = my - TB_H + sc.pos();
        for (int i = doc.items.len - 1; i >= 0; i--) {
            HtmlItem& it = doc.items[i];
            if ((it.link >= 0 || it.field >= 0) && mx >= it.x - 1 && mx < it.x + it.w + 1 && cy >= it.y - 2 && cy < it.y + it.h + 2) return i;
        }
        return -1;
    }

    void scroll_by(int d) { sc.set(clamp(sc.target + d, 0, max_scroll())); ui_invalidate(); }

    void on_mouse(const MouseEv& e) override {
        int off = sc.target;
        if (sb.handle(e, cw - 10, TB_H, view_h(), doc.height, view_h(), off)) { sc.jump(off); return; }
        if (e.kind == M_WHEEL) { scroll_by(-e.wheel * 60); return; }
        if (find_on && find_rect().contains(e.x, e.y)) { find_mouse(e); return; }
        if (e.kind == M_MOVE) {
            if (sel_drag && e.left) {
                if (iabs(e.x - down_x) > 3 || iabs(e.y - down_y) > 3) sel_moved = true;
                if (sel_moved) {
                    int ii, cc;
                    if (text_at(e.x, e.y, &ii, &cc)) { sb_i = ii; sb_c = cc; }
                    if (e.y < TB_H + 4) scroll_by(-24);
                    else if (e.y > ch - ST_H - 4) scroll_by(24);
                    ui_invalidate();
                }
            }
            int hl = -1;
            if (e.y > TB_H && e.y < ch - ST_H) {
                int i = hit_item(e.x, e.y);
                if (i >= 0) hl = doc.items[i].link;
            }
            if (hl != hover_link) { hover_link = hl; ui_invalidate(); }
            return;
        }
        if (e.kind != M_DOWN && e.kind != M_DBL && e.kind != M_UP) return;
        if (e.y < TB_H) {
            sel_drag = false;
            if (e.kind != M_UP) {
                if (Rect{132, 7, cw - 178, 24}.contains(e.x, e.y)) {
                    if (!addr_focus) { addr_focus = true; addr_fresh = true; addr.cur = addr.len; }
                    else addr.cur = clamp((e.x - 156) / 8, 0, addr.len);
                    focus_field = -1;
                }
                ui_invalidate();
                return;
            }
            if (Rect{8, 7, 26, 24}.contains(e.x, e.y)) go_back();
            else if (Rect{38, 7, 26, 24}.contains(e.x, e.y)) go_fwd();
            else if (Rect{68, 7, 26, 24}.contains(e.x, e.y)) on_command(req ? B_STOP : B_RELOAD);
            else if (Rect{98, 7, 28, 24}.contains(e.x, e.y)) navigate("silicon://start", true);
            else if (Rect{cw - 38, 7, 30, 24}.contains(e.x, e.y)) add_bookmark();
            ui_invalidate();
            return;
        }
        if (e.y >= ch - ST_H) return;
        if (e.kind == M_UP && e.button == 2) { context_menu(e); return; }
        if (e.kind == M_DOWN || e.kind == M_DBL) {
            addr_focus = false;
            find_focus = false;
            int i = hit_item(e.x, e.y);
            focus_field = -1;
            if (i >= 0 && doc.items[i].field >= 0) {
                HtmlItem& it = doc.items[i];
                HtmlField& f = doc.fields[it.field];
                if (it.kind == HK_CHECK) {
                    if (!strcmp(f.type, "radio"))
                        for (int k = 0; k < doc.fields.len; k++)
                            if (!strcmp(doc.fields[k].name, f.name) && doc.fields[k].form == f.form) doc.fields[k].checked = false;
                    f.checked = !f.checked || !strcmp(f.type, "radio");
                } else if (it.kind == HK_BUTTON) submit(f.form);
                else focus_field = it.field;
            } else if (e.button == 1) {
                int ii, cc;
                if (e.kind == M_DBL) { select_word_at(e.x, e.y); sel_drag = false; }
                else if (text_at(e.x, e.y, &ii, &cc)) { sel_on = true; sa_i = sb_i = ii; sa_c = sb_c = cc; sel_drag = true; sel_moved = false; down_x = e.x; down_y = e.y; }
                else sel_on = false;
            }
            ui_invalidate();
            return;
        }
        if (e.kind == M_UP && e.button == 1) {
            bool dragged = sel_moved && sel_drag;
            sel_drag = false;
            if (!dragged) {
                if (!has_selection()) sel_on = false;
                int i = hit_item(e.x, e.y);
                if (i >= 0 && doc.items[i].link >= 0) navigate(doc.links[doc.items[i].link], true);
            }
            ui_invalidate();
        }
    }

    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.ctrl && e.key == 'f') { toggle_find(); return; }
        if (find_on && find_focus) {
            if (e.key == K_ESC) { find_on = find_focus = false; fm_item.clear(); fm_pos.clear(); fcur = -1; ui_invalidate(); return; }
            if (e.key == K_ENTER) { find_step(e.shift ? -1 : 1); return; }
            if (e.ctrl && (e.key == 'l' || e.key == 'r')) {}
            else {
                char before[128];
                strlcpy(before, fq.text, sizeof(before));
                if (fq.on_key(e)) {
                    if (strcmp(before, fq.text)) { strlcpy(fq_last, fq.text, sizeof(fq_last)); rebuild_matches(true); }
                    ui_invalidate();
                    return;
                }
            }
        }
        if (e.ctrl && e.key == 'c' && !addr_focus && has_selection()) { copy_selection(); return; }
        if (e.ctrl && e.key == 'a' && !addr_focus && focus_field < 0) { select_all_text(); ui_invalidate(); return; }
        if ((e.ctrl && e.key == 'l') || e.key == K_F6) { addr_focus = true; addr_fresh = true; focus_field = -1; addr.cur = addr.len; ui_invalidate(); return; }
        if (e.key == K_F5 || (e.ctrl && e.key == 'r')) { on_command(B_RELOAD); return; }
        if (e.alt && e.key == K_LEFT) { go_back(); return; }
        if (e.alt && e.key == K_RIGHT) { go_fwd(); return; }
        if (e.ctrl && e.key == 'd') { add_bookmark(); return; }
        if (addr_focus) {
            if (e.key == K_ENTER) { addr_focus = false; navigate(addr.text, true); return; }
            if (e.key == K_ESC) { addr_focus = false; addr.set(url); ui_invalidate(); return; }
            if (addr_fresh && ((e.ch >= 32 && e.ch < 127 && !e.ctrl) || e.key == K_BACKSPACE || e.key == K_DELETE)) addr.set("");
            addr_fresh = false;
            addr.on_key(e);
            ui_invalidate();
            return;
        }
        if (focus_field >= 0 && focus_field < doc.fields.len) {
            HtmlField& f = doc.fields[focus_field];
            int l = (int)strlen(f.value);
            if (e.key == K_ENTER && strcmp(f.type, "textarea")) { submit(f.form); return; }
            if (e.ctrl && e.key == 'v') {
                u32 n = clip_len();
                char* t = (char*)kmalloc(n + 8);
                utf8_to_silicon(clip_get(), n, t, n + 8);
                for (char* p = t; *p && l < 250; p++) if ((u8)*p >= 32 && *p != 127) { f.value[l++] = *p; f.value[l] = 0; }
                kfree(t);
            }
            else if (e.key == K_BACKSPACE && l) f.value[l - 1] = 0;
            else if (e.key == K_ESC) focus_field = -1;
            else if (e.key == K_TAB) {
                for (int k = 1; k <= doc.fields.len; k++) {
                    int n = (focus_field + k) % doc.fields.len;
                    if (strcmp(doc.fields[n].type, "hidden") && strcmp(doc.fields[n].type, "submit")) { focus_field = n; break; }
                }
            } else if (e.ch >= 32 && e.ch < 127 && !e.ctrl && l < 250) { f.value[l] = e.ch; f.value[l + 1] = 0; }
            else if (e.key == K_ENTER) { f.value[l] = ' '; f.value[l + 1] = 0; }
            ui_invalidate();
            return;
        }
        switch (e.key) {
        case K_DOWN: scroll_by(48); break;
        case K_UP: scroll_by(-48); break;
        case K_PGDN: scroll_by(view_h() - 40); break;
        case ' ': scroll_by(e.shift ? -(view_h() - 40) : view_h() - 40); break;
        case K_PGUP: scroll_by(-(view_h() - 40)); break;
        case K_HOME: sc.set(0); break;
        case K_END: sc.set(max_scroll()); break;
        case 'j': scroll_by(48); break;
        case 'k': scroll_by(-48); break;
        case K_BACKSPACE: go_back(); break;
        case K_ESC: on_command(B_STOP); break;
        }
        ui_invalidate();
    }

    void on_command(int cmd) override {
        switch (cmd) {
        case B_BACK: go_back(); break;
        case B_FWD: go_fwd(); break;
        case B_RELOAD: { char u[600]; strlcpy(u, url, sizeof(u)); navigate(u, false); break; }
        case B_STOP: if (req) { http_free(req); req = nullptr; strlcpy(status, "stopped", sizeof(status)); } break;
        case B_HOME: navigate("silicon://start", true); break;
        case B_SOURCE: view_source = !view_source; relayout(false); sc.jump(0); break;
        case B_SAVE: save_page(); break;
        case B_BOOKMARK: add_bookmark(); break;
        case B_BOOKMARKS: navigate("silicon://bookmarks", true); break;
        case B_HISTORY: navigate("silicon://history", true); break;
        case B_NEWWIN: launch("__new__", "browser"); break;
        case B_COPY: copy_selection(); break;
        case B_SELALL: select_all_text(); break;
        case B_FIND: toggle_find(); break;
        case B_OPENLINK: if (ctx_link[0]) navigate(ctx_link, true); break;
        case B_LINKTAB: if (ctx_link[0]) open_in_new_tab(ctx_link); break;
        case B_COPYLINK: if (ctx_link[0]) clip_set(ctx_link); break;
        case B_CSS:
            show_css = !show_css;
            if (!show_css) clear_sheets();
            html_free_css(doc);
            relayout(true);
            break;
        case B_IMAGES:
            show_images = !show_images;
            if (!show_images) clear_images();
            relayout(true);
            break;
        }
        ui_invalidate();
    }
    int menus(Menu* m) override {
        m[0].clear(); strlcpy(m[0].title, "File", 24);
        m[0].add("New tab", B_NEWTAB, "ctrl+t").add("New window", B_NEWWIN).add("Save page", B_SAVE, "", src != nullptr).sep().add("Close tab", B_CLOSE, "ctrl+w");
        m[1].clear(); strlcpy(m[1].title, "Edit", 24);
        m[1].add("Copy", B_COPY, "ctrl+c", has_selection()).add("Select all", B_SELALL, "ctrl+a").sep().add("Find in page" G_ELLIP, B_FIND, "ctrl+f");
        m[2].clear(); strlcpy(m[2].title, "View", 24);
        m[2].add("Reload", B_RELOAD, "f5").add("Stop", B_STOP, "esc", req != nullptr).sep().add("Page source", B_SOURCE, "", true, view_source)
            .add("Show pictures", B_IMAGES, "", true, show_images)
            .add("Page styles (css)", B_CSS, "", true, show_css);
        m[3].clear(); strlcpy(m[3].title, "Go", 24);
        m[3].add("Back", B_BACK, "alt+left", back.len > 0).add("Forward", B_FWD, "alt+right", fwd.len > 0)
            .add("Start page", B_HOME).add("History", B_HISTORY).sep()
            .add("Bookmark this page", B_BOOKMARK, "ctrl+d").add("Show bookmarks", B_BOOKMARKS).sep()
            .add("Next tab", B_NEXTTAB, "ctrl+tab").add("Previous tab", B_PREVTAB, "ctrl+shift+tab");
        return 4;
    }
};

// ================================================================ the window: a strip of tabs
static const int TAB_H = 26;

struct BrowserApp : App {
    Vec<BeamTab*> tabs;
    int active = 0;
    char tbuf[96];

    BrowserApp(const char* start) { new_tab(start && start[0] ? start : "silicon://start"); }
    ~BrowserApp() { for (int i = 0; i < tabs.len; i++) delete tabs[i]; tabs.free(); }

    BeamTab* cur() { return tabs[active]; }
    BeamTab* new_tab(const char* url) {
        BeamTab* t = new BeamTab(url);
        t->owner = this;
        t->yoff = TAB_H;
        t->win = win;
        tabs.push(t);
        active = tabs.len - 1;
        set_title(title());
        ui_invalidate();
        return t;
    }
    void close_tab(int i) {
        if (tabs.len <= 1) { close(); return; }
        delete tabs[i];
        tabs.remove(i);
        if (active >= tabs.len) active = tabs.len - 1;
        else if (i < active) active--;
        set_title(title());
        ui_invalidate();
    }
    void switch_to(int i) { active = (i + tabs.len) % tabs.len; set_title(title()); ui_invalidate(); }

    const char* app_id() override { return "browser"; }
    const char* title() override { return cur()->title(); }
    int pref_w() override { return 780; }
    int pref_h() override { return 540 + TAB_H; }
    u32 background() override { return cur()->background(); }
    void open_arg(const char* arg) override { if (arg) cur()->open_arg(arg); }
    int cursor_at(int x, int y) override { return y < TAB_H ? CUR_ARROW : cur()->cursor_at(x, y - TAB_H); }
    void tick() override {
        for (int i = 0; i < tabs.len; i++) { tabs[i]->win = win; tabs[i]->tick(); }
    }
    int tab_w(int w) { return clamp((w - 36) / max(1, tabs.len), 60, 190); }

    void draw(Canvas& c, int w, int h) override {
        c.fill(0, 0, w, TAB_H, th::panel_dk);
        c.hline(0, TAB_H - 1, w, th::border);
        int tw = tab_w(w);
        for (int i = 0; i < tabs.len; i++) {
            int x = 4 + i * tw;
            bool on = i == active, hov = hot(x, 0, tw - 2, TAB_H);
            c.fill(x, on ? 2 : 4, tw - 2, TAB_H - (on ? 2 : 4), on ? th::panel : hov ? blend(th::panel_dk, th::white, 60) : th::panel_dk);
            if (on) c.fill(x, 2, tw - 2, 2, accent());
            c.vline(x + tw - 2, 5, TAB_H - 8, th::border);
            const char* t = tabs[i]->doc.title[0] ? tabs[i]->doc.title : tabs[i]->url;
            char lab[96];
            snprintf(lab, sizeof(lab), "%s%s", tabs[i]->req ? G_ELLIP " " : "", t);
            c.text_n(x + 8, 9, lab, (tw - 34) / 8, on ? th::text : th::muted, 1, on);
            if (tabs.len > 1 && (on || hov)) c.text(x + tw - 18, 9, "x", hot(x + tw - 22, 4, 18, 18) ? th::red : th::muted, 1, true);
        }
        int px = 4 + tabs.len * tw + 2;
        draw_button(c, px, 3, 22, 20, "+", hot(px, 3, 22, 20));
        // the front tab gets the rest of the window
        int ox = c.ox, oy = c.oy;
        Rect oc = c.clip;
        c.origin(ox, oy + TAB_H);
        c.set_clip({0, 0, w, h - TAB_H});
        cur()->win = win;
        cur()->draw(c, w, h - TAB_H);
        c.origin(ox, oy);
        c.clip = oc;
    }
    void on_mouse(const MouseEv& e) override {
        if (e.y >= TAB_H) { MouseEv m = e; m.y -= TAB_H; cur()->on_mouse(m); return; }
        if (e.kind != M_UP) return;
        int w = win ? win->client().w : 780;
        int tw = tab_w(w);
        if (e.button == 3) {
            int i = (e.x - 4) / tw;
            if (i >= 0 && i < tabs.len) close_tab(i);
            return;
        }
        if (e.button != 1) return;
        int px = 4 + tabs.len * tw + 2;
        if (Rect{px, 3, 22, 20}.contains(e.x, e.y)) { new_tab("silicon://start"); return; }
        int i = (e.x - 4) / tw;
        if (i < 0 || i >= tabs.len) return;
        if (tabs.len > 1 && e.x >= 4 + i * tw + tw - 22 && (i == active || true)) { close_tab(i); return; }
        switch_to(i);
    }
    void on_key(const KeyEvent& e) override {
        if (e.down && e.ctrl && !e.alt) {
            if (e.key == 't') { new_tab("silicon://start"); cur()->addr_focus = true; cur()->addr_fresh = true; cur()->addr.cur = cur()->addr.len; return; }
            if (e.key == 'w') { close_tab(active); return; }
            if (e.key == K_TAB) { switch_to(active + (e.shift ? -1 : 1)); return; }
            if (e.key == K_PGDN) { switch_to(active + 1); return; }
            if (e.key == K_PGUP) { switch_to(active - 1); return; }
        }
        if (e.down && e.alt && e.key >= '1' && e.key <= '9') { if (e.key - '1' < tabs.len) switch_to(e.key - '1'); return; }
        cur()->on_key(e);
    }
    int menus(Menu* m) override { return cur()->menus(m); }
    void on_command(int cmd) override {
        switch (cmd) {
        case B_NEWTAB: new_tab("silicon://start"); cur()->addr_focus = true; cur()->addr_fresh = true; cur()->addr.cur = cur()->addr.len; break;
        case B_CLOSE: close_tab(active); break;
        case B_NEXTTAB: switch_to(active + 1); break;
        case B_PREVTAB: switch_to(active - 1); break;
        default: cur()->on_command(cmd); break;
        }
        ui_invalidate();
    }
};

void BeamTab::set_title(const char* t) {
    BrowserApp* b = (BrowserApp*)owner;
    if (b && b->tabs.len && b->cur() == this) b->set_title(t);
}
void BeamTab::open_in_new_tab(const char* u) { ((BrowserApp*)owner)->new_tab(u); }

App* new_browser(const char* arg) { return new BrowserApp(arg); }
