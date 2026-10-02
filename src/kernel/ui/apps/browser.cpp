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

enum { B_BACK = 1, B_FWD, B_RELOAD, B_HOME, B_STOP, B_SOURCE, B_SAVE, B_BOOKMARK, B_BOOKMARKS, B_HISTORY, B_NEWWIN, B_CLOSE, B_IMAGES, B_CSS };

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

struct BrowserApp : App {
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

    BrowserApp(const char* start) {
        memset(&doc, 0, sizeof(doc));
        navigate(start && start[0] ? start : "silicon://start", false);
    }
    ~BrowserApp() {
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
        doc.img_size = &BrowserApp::img_size_cb;
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
        sync_images();
        sync_sheets();
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
        BrowserApp* b = (BrowserApp*)ctx;
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
                c.text_n(x, y, doc.str(it), it.tlen, it.color, it.scale, it.bold);
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
                draw_button(c, x, y, it.w, it.h, doc.str(it), hot(x, y, it.w, it.h), true);
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

        // toolbar
        c.fill(0, 0, w, TB_H, th::panel);
        c.hline(0, TB_H - 1, w, th::border);
        draw_button(c, 8, 7, 26, 24, G_LEFT, hot(8, 7, 26, 24), false, back.len > 0);
        draw_button(c, 38, 7, 26, 24, G_RIGHT, hot(38, 7, 26, 24), false, fwd.len > 0);
        draw_button(c, 68, 7, 26, 24, req ? G_TIMES : "@", hot(68, 7, 26, 24));
        draw_button(c, 98, 7, 28, 24, "~", hot(98, 7, 28, 24));
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
        draw_button(c, w - 38, 7, 30, 24, G_DIAM, hot(w - 38, 7, 30, 24));
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
        if (e.kind == M_MOVE) {
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
        if (e.kind == M_DOWN || e.kind == M_DBL) {
            addr_focus = false;
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
            }
            ui_invalidate();
            return;
        }
        if (e.kind == M_UP && e.button == 1) {
            int i = hit_item(e.x, e.y);
            if (i >= 0 && doc.items[i].link >= 0) navigate(doc.links[doc.items[i].link], true);
        }
    }

    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
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
            if (e.key == K_BACKSPACE && l) f.value[l - 1] = 0;
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
        case K_PGDN: case ' ': scroll_by(view_h() - 40); break;
        case K_PGUP: scroll_by(-(view_h() - 40)); break;
        case K_HOME: sc.set(0); break;
        case K_END: sc.set(max_scroll()); break;
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
        case B_CLOSE: close(); break;
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
        m[0].add("New window", B_NEWWIN).add("Save page", B_SAVE, "", src != nullptr).sep().add("Close", B_CLOSE, "ctrl+w");
        m[1].clear(); strlcpy(m[1].title, "View", 24);
        m[1].add("Reload", B_RELOAD, "f5").add("Stop", B_STOP, "esc", req != nullptr).sep().add("Page source", B_SOURCE, "", true, view_source)
            .add("Show pictures", B_IMAGES, "", true, show_images)
            .add("Page styles (css)", B_CSS, "", true, show_css);
        m[2].clear(); strlcpy(m[2].title, "Go", 24);
        m[2].add("Back", B_BACK, "alt+left", back.len > 0).add("Forward", B_FWD, "alt+right", fwd.len > 0)
            .add("Start page", B_HOME).sep().add("History", B_HISTORY);
        m[3].clear(); strlcpy(m[3].title, "Bookmarks", 24);
        m[3].add("Bookmark this page", B_BOOKMARK, "ctrl+d").add("Show bookmarks", B_BOOKMARKS);
        return 4;
    }
};

App* new_browser(const char* arg) { return new BrowserApp(arg); }
