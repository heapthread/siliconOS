// silicon - linear framebuffer with a RAM back buffer
#include "drivers.h"

Framebuffer fb;

void fb_init(BootInfo* bi) {
    fb.lfb = (u32*)bi->fb_addr;
    fb.width = (int)bi->fb_width;
    fb.height = (int)bi->fb_height;
    fb.pitch = (int)(bi->fb_pitch / 4);
    u32* mem = (u32*)kmalloc((size_t)fb.width * fb.height * 4);
    fb.back.init(mem, fb.width, fb.height, fb.width);
    fb.back.clear(0xFF000000);
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
