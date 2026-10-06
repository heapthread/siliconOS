// silicon - app modules (.sapp): third-party apps built with the silicon SDK (see sdk/).
// A module is a relocatable flat image plus a table of addresses to fix up. It gets a pointer to
// the SiApi function table (the "syscalls") and registers a window through the SiApp structure.
// Modules run in the kernel's address space (there is no memory protection yet), so only run ones you trust.
#include "apps.h"

// ---- the ABI: keep in sync with sdk/silicon.h
struct SiCanvas;
struct SiMouse { int kind, x, y, button, wheel, left; };
struct SiKey { int key, ch, down, shift, ctrl, alt; };
struct SiApi {
    u32 version;
    u32 size;
    // drawing: the canvas pointer is handed to the draw callback
    void (*fill)(SiCanvas*, int x, int y, int w, int h, u32 argb);
    void (*rect)(SiCanvas*, int x, int y, int w, int h, u32 argb);
    void (*line)(SiCanvas*, int x0, int y0, int x1, int y1, u32 argb);
    void (*circle)(SiCanvas*, int cx, int cy, int r, u32 argb, int filled);
    int  (*text)(SiCanvas*, int x, int y, const char* s, u32 argb, int scale, int bold);
    int  (*text_width)(const char* s, int scale, int bold);
    void (*blit)(SiCanvas*, const u32* argb, int w, int h, int x, int y);
    // the app
    void (*invalidate)(void);
    void (*set_title)(const char*);
    void (*notify)(const char* title, const char* body);
    void (*beep)(int kind);
    u32  (*ticks)(void);
    u32  (*rand)(void);
    void* (*alloc)(u64 n);
    void (*free)(void* p);
    void (*log)(const char* msg);
    // files (SiliconFS paths)
    int  (*file_read)(const char* path, void* buf, int cap);        // bytes read, -1 when missing
    int  (*file_write)(const char* path, const void* buf, int len); // 0 on success
    int  (*file_size)(const char* path);                            // -1 when missing, -2 for a folder
    int  (*dir_entry)(const char* path, int index, char* name, int cap);   // 1 = file, 2 = folder, 0 = no more
    // clipboard and the rest of the desktop
    void (*clip_set)(const char*);
    const char* (*clip_get)(void);
    void (*open_path)(const char* path);
    int  (*screen_w)(void);
    int  (*screen_h)(void);
};
struct SiApp {
    char title[48];
    int  width, height;
    u32  background;
    int  resizable;
    void (*draw)(SiCanvas*, int w, int h);
    void (*mouse)(const SiMouse*);
    void (*key)(const SiKey*);
    void (*tick)(void);
    void (*closed)(void);
    void* reserved[5];
};
static const u32 SI_VERSION = 1;

struct ModApp;
static ModApp* cur_mod = nullptr;

// ---- the function table
static Canvas* C(SiCanvas* c) { return (Canvas*)c; }
static void a_fill(SiCanvas* c, int x, int y, int w, int h, u32 col) { if (col >> 24 == 255) C(c)->fill(x, y, w, h, col); else C(c)->fill_alpha(x, y, w, h, col | 0xFF000000, col >> 24); }
static void a_rect(SiCanvas* c, int x, int y, int w, int h, u32 col) { C(c)->rect(x, y, w, h, col); }
static void a_line(SiCanvas* c, int x0, int y0, int x1, int y1, u32 col) { C(c)->line(x0, y0, x1, y1, col); }
static void a_circle(SiCanvas* c, int cx, int cy, int r, u32 col, int filled) { C(c)->circle(cx, cy, r, col, filled != 0); }
static int  a_text(SiCanvas* c, int x, int y, const char* s, u32 col, int scale, int bold) { return C(c)->text(x, y, s, col, max(1, scale), bold != 0); }
static int  a_text_width(const char* s, int scale, int bold) { return text_width(s, max(1, scale), bold != 0); }
static void a_blit(SiCanvas* c, const u32* px, int w, int h, int x, int y) { Image im; im.w = w; im.h = h; im.px = (u32*)px; C(c)->blit(im, x, y); }
static void a_invalidate();
static void a_set_title(const char* t);
static void a_notify(const char* t, const char* b) { ui_notify(t, b, "module"); }
static void a_beep(int k) { ui_beep(k); }
static u32  a_ticks() { return (u32)ticks(); }
static u32  a_rand() { return rand(); }
static void* a_alloc(u64 n) { return kmalloc_try((size_t)n); }
static void a_free(void* p) { if (p) kfree(p); }
static void a_log(const char* m) { klog("module: %s\n", m); }
static int  a_file_read(const char* path, void* buf, int cap) {
    FsNode* f = fs_lookup(path);
    if (!f || f->dir) return -1;
    int n = (int)min<u32>(f->size, (u32)cap);
    memcpy(buf, f->data, n);
    return n;
}
static int  a_file_write(const char* path, const void* buf, int len) {
    FsNode* f = fs_lookup(path);
    if (!f) {
        char dir[256];
        strlcpy(dir, path, sizeof(dir));
        char* sl = strrchr(dir, '/');
        if (sl && sl != dir) { *sl = 0; fs_mkdirs(dir); }
        f = fs_create_path(path, false);
    }
    if (!f || f->dir) return -1;
    return fs_write(f, buf, (u32)len) ? 0 : -1;
}
static int  a_file_size(const char* path) { FsNode* f = fs_lookup(path); return !f ? -1 : f->dir ? -2 : (int)f->size; }
static int  a_dir_entry(const char* path, int index, char* name, int cap) {
    FsNode* d = fs_lookup(path);
    if (!d || !d->dir || index < 0 || index >= d->children.len) return 0;
    strlcpy(name, d->children[index]->name, cap);
    return d->children[index]->dir ? 2 : 1;
}
static void a_clip_set(const char* t) { clip_set(t); }
static const char* a_clip_get() { return clip_get(); }
static void a_open_path(const char* p) { open_path(p); }
static int  a_screen_w() { return fb.width; }
static int  a_screen_h() { return fb.height; }

static SiApi api = {
    SI_VERSION, (u32)sizeof(SiApi),
    a_fill, a_rect, a_line, a_circle, a_text, a_text_width, a_blit,
    a_invalidate, a_set_title, a_notify, a_beep, a_ticks, a_rand, a_alloc, a_free, a_log,
    a_file_read, a_file_write, a_file_size, a_dir_entry,
    a_clip_set, a_clip_get, a_open_path, a_screen_w, a_screen_h,
};

// ---- the app wrapper
struct ModApp : App {
    u8* raw = nullptr;           // allocation (the image is aligned inside it)
    u8* image = nullptr;
    SiApp sa;
    char name[FS_NAME_MAX] = "";
    char err[96] = "";
    char tbuf[64] = "";
    bool loaded = false;

    ModApp(const char* path) {
        memset(&sa, 0, sizeof(sa));
        FsNode* f = fs_lookup(path);
        if (!f || f->dir) { snprintf(err, sizeof(err), "can't find %s", path); return; }
        strlcpy(name, f->name, sizeof(name));
        load(f);
    }
    ~ModApp() {
        if (loaded && sa.closed) { ModApp* prev = cur_mod; cur_mod = this; sa.closed(); cur_mod = prev; }
        if (raw) kfree(raw);
    }
    static u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
    void load(FsNode* f) {
        const u8* d = f->data;
        u32 n = f->size;
        if (n < 28 || memcmp(d, "SAPP", 4)) { snprintf(err, sizeof(err), "this isn't a silicon app module"); return; }
        u32 ver = rd32(d + 4), isize = rd32(d + 8), entry = rd32(d + 12), base0 = rd32(d + 16), n64 = rd32(d + 20), n32 = rd32(d + 24);
        if (ver != 1 || isize == 0 || isize > 8 * 1024 * 1024 || entry >= isize) { snprintf(err, sizeof(err), "unsupported module version"); return; }
        u32 table = 28, data_off = table + (n64 + n32) * 4;
        if ((u64)data_off + isize > n) { snprintf(err, sizeof(err), "the module file is truncated"); return; }
        raw = (u8*)kmalloc_try(isize + 4096 + 64);
        if (!raw) { snprintf(err, sizeof(err), "out of memory"); return; }
        image = (u8*)(((u64)raw + 4095) & ~4095ull);
        memcpy(image, d + data_off, isize);
        u64 delta = (u64)image - base0;
        for (u32 i = 0; i < n64; i++) {
            u32 off = rd32(d + table + i * 4);
            if (off + 8 > isize) continue;
            u64 v;
            memcpy(&v, image + off, 8);
            v += delta;
            memcpy(image + off, &v, 8);
        }
        for (u32 i = 0; i < n32; i++) {
            u32 off = rd32(d + table + (n64 + i) * 4);
            if (off + 4 > isize) continue;
            u32 v = rd32(image + off) + (u32)delta;
            memcpy(image + off, &v, 4);
        }
        // the entry point fills in the SiApp structure
        typedef int (*EntryFn)(const SiApi*, SiApp*);
        EntryFn fn = (EntryFn)(image + entry);
        sa.width = 480; sa.height = 320; sa.background = th::paper; sa.resizable = 1;
        strlcpy(sa.title, name, sizeof(sa.title));
        ModApp* prev = cur_mod;
        cur_mod = this;
        int rc = fn(&api, &sa);
        cur_mod = prev;
        if (rc != 0) { snprintf(err, sizeof(err), "the module failed to start (code %d)", rc); return; }
        loaded = true;
    }
    const char* app_id() override { return "sapp"; }
    const char* icon_name() override { return "module"; }
    const char* title() override { return loaded ? sa.title : name; }
    int pref_w() override { return loaded ? sa.width : 420; }
    int pref_h() override { return loaded ? sa.height : 140; }
    bool resizable() override { return loaded ? sa.resizable != 0 : false; }
    u32 background() override { return loaded ? sa.background : th::paper; }
    bool wants_key_up() override { return loaded; }

    void draw(Canvas& c, int w, int h) override {
        if (!loaded) {
            c.text_centered(0, h / 2 - 18, w, "can't run this module", th::red, 1, true);
            c.text_centered(0, h / 2 + 2, w, err, th::muted);
            return;
        }
        if (!sa.draw) return;
        ModApp* prev = cur_mod; cur_mod = this;
        sa.draw((SiCanvas*)&c, w, h);
        cur_mod = prev;
    }
    void on_mouse(const MouseEv& e) override {
        if (!loaded || !sa.mouse) return;
        SiMouse m = {(int)e.kind, e.x, e.y, e.button, e.wheel, e.left ? 1 : 0};
        ModApp* prev = cur_mod; cur_mod = this;
        sa.mouse(&m);
        cur_mod = prev;
    }
    void on_key(const KeyEvent& e) override {
        if (!loaded || !sa.key) return;
        SiKey k = {(int)e.key, (int)(u8)e.ch, e.down ? 1 : 0, e.shift ? 1 : 0, e.ctrl ? 1 : 0, e.alt ? 1 : 0};
        ModApp* prev = cur_mod; cur_mod = this;
        sa.key(&k);
        cur_mod = prev;
    }
    void tick() override {
        if (!loaded || !sa.tick) return;
        ModApp* prev = cur_mod; cur_mod = this;
        sa.tick();
        cur_mod = prev;
    }
};

static void a_invalidate() { ui_invalidate(); }
static void a_set_title(const char* t) {
    if (!cur_mod) return;
    strlcpy(cur_mod->sa.title, t, sizeof(cur_mod->sa.title));
    cur_mod->set_title(cur_mod->sa.title);
}

App* new_module(const char* path) { return new ModApp(path); }

// ---- sample modules built from sdk/examples are embedded in the kernel and copied to /home/apps
struct AssetEntry { const char* name; const u8* data; u64 len; };
extern "C" AssetEntry asset_table[];
extern "C" u64 asset_count;

void modules_install_samples() {
    for (u64 i = 0; i < asset_count; i++) {
        AssetEntry& e = asset_table[i];
        if (!str_starts(e.name, "sapp/")) continue;
        char path[96];
        snprintf(path, sizeof(path), "/home/apps/%s.sapp", e.name + 5);
        if (fs_lookup(path)) continue;
        fs_mkdirs("/home/apps");
        FsNode* f = fs_create_path(path, false);
        if (f) fs_write(f, e.data, (u32)e.len);
    }
}
