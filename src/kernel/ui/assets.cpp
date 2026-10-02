// silicon - decoded icons and wallpapers
#include "wm.h"

struct AssetEntry { const char* name; const u8* data; u64 len; };
extern "C" AssetEntry asset_table[];
extern "C" u64 asset_count;

static Vec<Icon*> icons;
static Vec<Wallpaper*> walls;

// box-filter downscale by an integer factor, alpha weighted
static Image downscale(const Image& src, int f) {
    Image out = image_new(src.w / f, src.h / f, 0);
    for (int y = 0; y < out.h; y++)
        for (int x = 0; x < out.w; x++) {
            u32 a = 0, r = 0, g = 0, b = 0;
            for (int yy = 0; yy < f; yy++)
                for (int xx = 0; xx < f; xx++) {
                    u32 p = src.px[(y * f + yy) * src.w + x * f + xx];
                    u32 pa = p >> 24;
                    a += pa;
                    r += ((p >> 16) & 255) * pa;
                    g += ((p >> 8) & 255) * pa;
                    b += (p & 255) * pa;
                }
            if (!a) continue;
            u32 n = f * f;
            out.px[y * out.w + x] = ((a / n) << 24) | ((r / a) << 16) | ((g / a) << 8) | (b / a);
        }
    return out;
}

void ui_preload() {
    if (icons.len) return;
    for (u64 i = 0; i < asset_count; i++) {
        AssetEntry& e = asset_table[i];
        Image img;
        if (!image_decode(e.data, (u32)e.len, img)) { klog("asset %s: bad data\n", e.name); continue; }
        if (str_starts(e.name, "icon/")) {
            Icon* ic = new Icon();
            ic->name = e.name + 5;
            ic->big = img;
            ic->mid = downscale(img, 2);
            ic->small = downscale(img, 4);
            icons.push(ic);
        } else if (str_starts(e.name, "wall/")) {
            Wallpaper* w = new Wallpaper();
            w->name = e.name + 5;
            w->full = img;
            w->thumb = downscale(img, 8);
            walls.push(w);
        }
    }
    klog("assets: %d icons, %d wallpapers\n", icons.len, walls.len);
}

Icon* icon(const char* name) {
    for (int i = 0; i < icons.len; i++) if (!strcmp(icons[i]->name, name)) return icons[i];
    for (int i = 0; i < icons.len; i++) if (!strcmp(icons[i]->name, "file")) return icons[i];
    return icons[0];
}

int wallpaper_count() { return walls.len; }
Wallpaper* wallpaper_get(int i) { return walls[clamp(i, 0, walls.len - 1)]; }

int wallpaper_add_custom(const char* name, Image full) {
    Wallpaper* w = new Wallpaper();
    w->name = strdup(name);
    w->full = full;
    w->thumb = downscale(full, 8);
    w->custom = true;
    walls.push(w);
    return walls.len - 1;
}
