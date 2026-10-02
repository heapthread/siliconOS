// silicon - a small HTML layout engine for the Beam browser
#pragma once
#include "../core.h"

enum HtmlKind { HK_TEXT, HK_RECT, HK_IMAGE, HK_FIELD, HK_BUTTON, HK_BULLET, HK_CHECK };

struct HtmlItem {
    u8   kind;
    u8   scale;
    bool bold, underline, strike;
    int  x, y, w, h;
    u32  color, bg;            // bg 0 = none
    int  link;                 // index into links or -1
    int  field;                // index into fields or -1
    int  text;                 // offset into the text pool
    int  tlen;
    int  img;                  // HK_IMAGE: index into img_urls or -1
};

struct HtmlField {
    int  form;
    char name[64];
    char type[12];             // text, password, submit, hidden, checkbox, textarea, search
    char value[256];
    bool checked;
};

struct HtmlForm {
    char action[512];
    bool post;
};

struct HtmlDoc {
    Vec<HtmlItem> items;
    Vec<char*> links;
    Vec<HtmlField> fields;
    Vec<HtmlForm> forms;
    char  title[128];
    int   height;
    u32   bg, fg;
    char* pool;
    int   pool_len, pool_cap;
    int   images;
    Vec<char*> img_urls;       // resolved image addresses (unique)
    Vec<u8> img_sized;         // per url: 1 = the page gave both width and height (loading it can't move things)
    // the browser tells the layout how big already-loaded images are
    bool (*img_size)(void* ctx, const char* url, int* w, int* h);
    void* img_ctx;
    Vec<char*> css_urls;       // linked style sheets the page wants
    const char* ext_css;       // set by the browser: those sheets, fetched and concatenated
    u32   ext_css_len;
    void* css;                 // parsed sheet cache (html.cpp)
    u32   css_key;
    void  clear();
    const char* str(const HtmlItem& it) const { return pool + it.text; }
};

// keep: field values typed by the user survive a re-layout (same order of fields)
void html_free_css(HtmlDoc& doc);
void html_layout(const char* html, u32 len, const char* base_url, int width, HtmlDoc& doc, bool plain_text,
                 const HtmlDoc* keep = nullptr);
