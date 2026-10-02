// silicon - a small CSS engine for Beam: parses style sheets, matches selectors against the
// element stack and computes the properties silicon's flow layout understands.
#pragma once
#include "../core.h"

static const int CSS_UNSET = -32768;

enum CssDisplay : i8 { CD_UNSET = -1, CD_NONE, CD_BLOCK, CD_INLINE, CD_INLINE_BLOCK, CD_LIST_ITEM, CD_FLEX, CD_TABLE, CD_CONTENTS };

// one element as the selector matcher sees it
struct CssElem {
    u32 tag;                  // hash of the lower-case tag name
    u32 id;                   // hash of the id (0 = none)
    u32 cls[8];
    int ncls;
    u32 attrs[10];            // hashes of attribute names present
    u32 attr_vals[10];        // css_hash(value) | 1
    int nattrs;
    bool has_href;
};

struct CssComputed {
    i8  display;              // CssDisplay
    i8  hidden;               // visibility:hidden, or the usual "visually hidden" tricks
    bool has_color, has_bg;
    u32 color, bg;            // bg alpha 0 = transparent (explicitly)
    i8  bold, italic, mono, underline, strike, upper, pre, list_none;
    i8  align;                // -1 unset, 0 left, 1 center, 2 right
    int font_px;              // absolute size, or
    int font_pct;             // relative to the parent (100 = same)
    int margin[4], padding[4];// top right bottom left in px (CSS_UNSET)
    bool margin_auto_lr;
    int width, max_width, height;   // px, -1 unset
    i8  position;             // 0 static/unset, 1 absolute, 2 fixed, 3 relative, 4 sticky
    int left, top;            // px (CSS_UNSET)
    int border_w[4];          // top right bottom left (0 none)
    u32 border_c[4];
    i8  flex_column;          // flex-direction: column
    i8  float_side;           // 0 none, 1 left, 2 right
};

struct CssSheet;
CssSheet* css_new();
void css_add(CssSheet* s, const char* text, u32 len);     // parse and append rules
int  css_rule_count(CssSheet* s);
void css_free(CssSheet* s);

u32  css_hash(const char* s, int n);                      // case-sensitive
u32  css_hash_lower(const char* s, int n);

// compute the style of stack[depth-1] (ancestors before it); viewport_w for @media
void css_compute(CssSheet* s, const CssElem* stack, int depth, int viewport_w, const char* inline_style, CssComputed& out);
bool css_parse_color(const char* v, u32& out);
