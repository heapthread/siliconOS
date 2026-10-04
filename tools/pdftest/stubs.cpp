// temporary stand-ins for modules that do not exist yet
#include "../../src/kernel/pdf/pdf_int.h"
const u8 font_rows[256][12] = {
#include "../../src/kernel/gfx/font_data.inc"
};
Image image_new(int w, int h, u32 fill) { Image i; i.w = w; i.h = h; i.px = (u32*)kmalloc((size_t)w * h * 4); for (int k = 0; k < w * h; k++) i.px[k] = fill; return i; }
void image_free(Image& i) { if (i.px) kfree(i.px); i.px = nullptr; }
