// silicon - scalable fonts: TrueType outlines (the PDF engine's parser) rasterized at any size and
// cached as coverage bitmaps. The fonts (DejaVu Sans, regular and bold) are embedded in the kernel.
#pragma once
#include "gfx.h"

struct VFont;
VFont* vfont_get(bool bold, bool mono = false);                         // nullptr when the font data is missing
int    vfont_gid(VFont* f, u32 cp);                  // glyph index, 0 = .notdef
float  vfont_advance(VFont* f, int gid, float px);   // horizontal advance in pixels
float  vfont_ascent(float px);
float  vfont_line_height(float px);
// draws one glyph with its origin at (x, baseline); returns false when there is nothing to draw
bool   vfont_draw_gid(Canvas& c, VFont* f, int gid, float px, float x, float baseline, u32 argb);
float  vfont_text_width_utf8(VFont* f, const char* s, int n, float px);
const u8* asset_find(const char* name, u32* len);    // an embedded blob by name (ui/assets.cpp)
