#include "clipboard.h"
#include "drv/drivers.h"
#include "text.h"

static char* buf = nullptr;
static u32 blen = 0, bcap = 0, gen = 1;
static const u32 CLIP_MAX = 256 * 1024;

static void store(const char* t, u32 n) {
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
