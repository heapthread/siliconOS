// silicon - linear framebuffer with a RAM back buffer, runtime mode switching through the Bochs
// VBE registers, and the monitor's preferred mode / refresh rate read from the virtual EDID
#include "drivers.h"
#include "pci.h"
#include "../settings.h"

Framebuffer fb;

static const u16 DISPI_INDEX = 0x1CE, DISPI_DATA = 0x1CF;
enum { DI_ID = 0, DI_XRES, DI_YRES, DI_BPP, DI_ENABLE, DI_BANK, DI_VIRT_W, DI_VIRT_H, DI_XOFF, DI_YOFF };

static void dispi_w(u16 idx, u16 v) { outw(DISPI_INDEX, idx); outw(DISPI_DATA, v); }
static u16 dispi_r(u16 idx) { outw(DISPI_INDEX, idx); return inw(DISPI_DATA); }

static bool has_dispi = false;

void fb_init(BootInfo* bi) {
    fb.lfb = (u32*)bi->fb_addr;
    fb.width = (int)bi->fb_width;
    fb.height = (int)bi->fb_height;
    fb.pitch = (int)(bi->fb_pitch / 4);
    u32* mem = (u32*)kmalloc((size_t)fb.width * fb.height * 4);
    fb.back.init(mem, fb.width, fb.height, fb.width);
    fb.back.clear(0xFF000000);
    u16 id = dispi_r(DI_ID);
    has_dispi = id >= 0xB0C0 && id <= 0xB0CF;
}

bool fb_can_resize() { return has_dispi; }

void fb_limit_mode(int* w, int* h) {
    int mw = *w, mh = *h;
    mw = clamp(mw, 800, 3840);
    mh = clamp(mh, 600, 2160);
    // keep the pixel count in check: software rendering cost grows with the area
    const i64 MAXPX = 4200000;
    if ((i64)mw * mh > MAXPX) {
        int f = 1000;
        while (f > 100 && (i64)(mw * f / 1000) * (mh * f / 1000) > MAXPX) f -= 10;
        mw = mw * f / 1000;
        mh = mh * f / 1000;
    }
    mw &= ~7;
    mh &= ~1;
    mw = max(mw, 800);
    mh = max(mh, 600);
    *w = mw;
    *h = mh;
}

bool fb_set_mode(int w, int h) {
    if (!has_dispi) return false;
    fb_limit_mode(&w, &h);
    if (w == fb.width && h == fb.height) return true;
    u32* mem = (u32*)kmalloc_try((size_t)w * h * 4);
    if (!mem) return false;
    int ow = fb.width, oh = fb.height;
    dispi_w(DI_ENABLE, 0);
    dispi_w(DI_XRES, (u16)w);
    dispi_w(DI_YRES, (u16)h);
    dispi_w(DI_BPP, 32);
    dispi_w(DI_VIRT_W, (u16)w);
    dispi_w(DI_ENABLE, 0x41);                      // enabled + linear framebuffer (clears the screen)
    if (dispi_r(DI_XRES) != w || dispi_r(DI_YRES) != h) {      // the device said no: go back
        dispi_w(DI_ENABLE, 0);
        dispi_w(DI_XRES, (u16)ow);
        dispi_w(DI_YRES, (u16)oh);
        dispi_w(DI_VIRT_W, (u16)ow);
        dispi_w(DI_ENABLE, 0x41);
        kfree(mem);
        return false;
    }
    u32* old = fb.back.px;
    fb.width = w;
    fb.height = h;
    fb.pitch = w;
    fb.back.init(mem, w, h, w);
    fb.back.clear(0xFF000000);
    kfree(old);
    klog("display: %dx%d\n", w, h);
    return true;
}

// ---------------------------------------------------------------- EDID (std VGA: BAR2 offset 0)
static volatile u8* edid_base = nullptr;
static bool edid_looked = false;

static bool edid_read(u8 out[128]) {
    if (!edid_looked) {
        edid_looked = true;
        if (!pci_count()) pci_scan();
        PciDevice* d = pci_find(0x1234, 0x1111);
        if (d && (d->bar[2] & 1) == 0 && (d->bar[2] & ~0xFu)) edid_base = (volatile u8*)(u64)(d->bar[2] & ~0xFu);
    }
    if (!edid_base) return false;
    for (int i = 0; i < 128; i++) out[i] = edid_base[i];
    static const u8 hdr[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    return !memcmp(out, hdr, 8);
}

bool fb_edid_mode(int* w, int* h, int* hz) {
    u8 e[128];
    if (!edid_read(e)) return false;
    // first detailed timing descriptor = preferred mode
    const u8* d = e + 54;
    int pclk = d[0] | (d[1] << 8);                        // 10 kHz units
    if (!pclk) return false;
    int ha = d[2] | ((d[4] & 0xF0) << 4), hb = d[3] | ((d[4] & 0x0F) << 8);
    int va = d[5] | ((d[7] & 0xF0) << 4), vb = d[6] | ((d[7] & 0x0F) << 8);
    if (ha < 320 || va < 200) return false;
    *w = ha;
    *h = va;
    long total = (long)(ha + hb) * (va + vb);
    int rate = total ? (int)(((long)pclk * 10000 + total / 2) / total) : 60;
    *hz = (rate >= 24 && rate <= 540) ? rate : 60;
    return true;
}

void fb_present() {
    if (fb.pitch == fb.width) {
        memcpy(fb.lfb, fb.back.px, (size_t)fb.width * fb.height * 4);
        return;
    }
    for (int y = 0; y < fb.height; y++)
        memcpy(fb.lfb + y * fb.pitch, fb.back.px + y * fb.width, fb.width * 4);
}

void fb_present_rect(Rect r) {
    r = r.intersect({0, 0, fb.width, fb.height});
    for (int y = r.y; y < r.y + r.h; y++)
        memcpy(fb.lfb + y * fb.pitch + r.x, fb.back.px + y * fb.width + r.x, r.w * 4);
}
