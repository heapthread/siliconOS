// silicon - text helpers: UTF-8, silicon's 8-bit codepage and emoji.
//
// Text inside silicon is a byte string in the silicon codepage (latin-1 letters collapsed to ASCII
// plus some drawing glyphs, see tools/font.txt). An emoji is two bytes: a lead byte (1 or 2) and an
// index byte (1..255), together index = (lead - 1) * 255 + byte. Every byte is 8 pixels wide, so an
// emoji takes 16 pixels (two cells) and word wrapping / column math keep working unchanged.
#pragma once
#include "core.h"
#include "gfx/gfx.h"

static inline bool is_emoji_lead(u8 c) { return c == 1 || c == 2; }
static inline int  emoji_from_bytes(u8 lead, u8 b) { return (lead - 1) * 255 + b; }          // 1-based index
static inline void emoji_to_bytes(int idx, char out[2]) { out[0] = (char)(1 + (idx - 1) / 255); out[1] = (char)(1 + (idx - 1) % 255); }

int         emoji_count();
int         emoji_index(u32 cp);                 // 1-based, 0 = no sprite
u32         emoji_codepoint(int idx);
const char* emoji_name(int idx);
const Image* emoji_sprite(int idx, int px);      // px: 16 (big) or 12 (inline with text)
int         emoji_find(const char* name_part, int start);   // next index at or after start whose name contains name_part, or 0

u32  utf8_decode(const char*& p, const char* end);           // advances p; invalid bytes come out as themselves
int  utf8_encode(u32 cp, char* out);                         // returns the byte count (1..4)
int  utf8_len_at(const char* p, const char* end);            // bytes of the sequence starting at p
int  glyph_for(u32 cp, char out[4]);                         // bytes in the silicon codepage for one code point (0 = drop)

// whole-string conversions (the clipboard helpers in clipboard.h wrap these)
void utf8_to_silicon(const char* in, u32 len, char* out, u32 out_cap);
void silicon_to_utf8(const char* in, u32 len, char* out, u32 out_cap);
