#include "clipboard.h"
#include "drv/drivers.h"
#include "text.h"

static char* buf = nullptr;
static u32 blen = 0, bcap = 0, gen = 1;
static const u32 CLIP_MAX = 256 * 1024;

static Image clip_img;
static Vec<char*> clip_paths;
static bool clip_cut = false;
static void clear_extra() {
    if (clip_img.px) kfree(clip_img.px);
    clip_img = Image();
    for (int i = 0; i < clip_paths.len; i++) kfree(clip_paths[i]);
    clip_paths.clear();
}

static void store(const char* t, u32 n) {
    clear_extra();
    if (n > CLIP_MAX) n = CLIP_MAX;
    if (n + 1 > bcap) {
        if (buf) kfree(buf);
        bcap = max(n + 1, 256u);
        buf = (char*)kmalloc(bcap);
    }
    memcpy(buf, t, n);
    buf[n] = 0;
    blen = n;
    gen++;
}

void clip_set(const char* text, u32 len) {
    store(text, len);
    bridge_send_clip(buf, blen);
}
void clip_set(const char* text) { clip_set(text, (u32)strlen(text)); }
void clip_from_host(const char* text, u32 len) { store(text, len); }
const char* clip_get() { return buf ? buf : ""; }
u32 clip_len() { return blen; }
bool clip_has() { return blen > 0; }
u32 clip_generation() { return gen; }

int clip_kind() { return clip_img.px ? CLIP_IMAGE : clip_paths.len ? CLIP_FILES : blen ? CLIP_TEXT : CLIP_EMPTY; }
void clip_set_image(const Image& img) {
    if (!img.px || img.w <= 0 || img.h <= 0) return;
    u32* px = (u32*)kmalloc_try((size_t)img.w * img.h * 4);
    if (!px) return;
    memcpy(px, img.px, (size_t)img.w * img.h * 4);
    store("", 0);                       // also forgets any text or files
    clip_img.w = img.w; clip_img.h = img.h; clip_img.px = px;
    blen = 0;
}
const Image* clip_image() { return clip_img.px ? &clip_img : nullptr; }
void clip_files_begin(bool cut) { clear_extra(); clip_cut = cut; }
void clip_files_add(const char* path) { clip_paths.push(strdup(path)); }
void clip_files_end() {
    u32 total = 1;
    for (int i = 0; i < clip_paths.len; i++) total += (u32)strlen(clip_paths[i]) + 1;
    char* text = (char*)kmalloc(total);
    u32 o = 0;
    for (int i = 0; i < clip_paths.len; i++) { u32 l = (u32)strlen(clip_paths[i]); memcpy(text + o, clip_paths[i], l); o += l; if (i + 1 < clip_paths.len) text[o++] = '\n'; }
    text[o] = 0;
    // store() would forget the list we just built: keep it aside
    Vec<char*> keep;
    for (int i = 0; i < clip_paths.len; i++) keep.push(clip_paths[i]);
    clip_paths.clear();
    bool cut = clip_cut;
    store(text, o);
    bridge_send_clip(buf, blen);
    kfree(text);
    for (int i = 0; i < keep.len; i++) clip_paths.push(keep[i]);
    keep.free();
    clip_cut = cut;
}
int clip_file_count() { return clip_paths.len; }
const char* clip_file(int i) { return i >= 0 && i < clip_paths.len ? clip_paths[i] : ""; }
bool clip_files_cut() { return clip_cut; }
void clip_clear() { store("", 0); blen = 0; }
