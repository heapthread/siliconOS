// silicon - PDF reader: parses documents, renders pages to bitmaps and extracts their text
#pragma once
#include "../core.h"
#include "../gfx/gfx.h"

struct PdfDoc;

struct PdfChar {
    float x0, y0, x1, y1;        // page space in points, origin top-left
    u32   cp;                    // unicode (0x0A marks a line break the document implied)
};
struct PdfOutline {
    char title[96];
    int  page;                   // 0-based, -1 when the destination can't be resolved
    int  level;
};

PdfDoc*     pdf_open(const u8* data, u32 len, char* err, int errn);   // copies the data
void        pdf_close(PdfDoc* d);
int         pdf_page_count(PdfDoc* d);
void        pdf_page_size(PdfDoc* d, int page, float* w, float* h);   // points, with the page's rotation applied
bool        pdf_render(PdfDoc* d, int page, float scale, Image* out); // white page; out->px is kmalloc'd
int         pdf_page_text(PdfDoc* d, int page, const PdfChar** chars);   // cached inside the document
const char* pdf_info(PdfDoc* d, const char* key);                     // "Title", "Author", ... or ""
int         pdf_outline_count(PdfDoc* d);
const PdfOutline* pdf_outline(PdfDoc* d, int i);
