// host test driver: pdftest file.pdf [page] [scale] [out.ppm] | pdftest file.pdf text [page]
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
extern "C" { void* malloc(size_t); void* calloc(size_t, size_t); void* realloc(void*, size_t); void free(void*); void exit(int); double strtod(const char*, char**); }
#include "../../src/kernel/pdf/pdf_int.h"
#include "../../src/kernel/text.h"


static void sil2ascii(const char* s, char* out, int n) {
    int k = 0;
    for (; *s && k < n - 1; s++) out[k++] = (unsigned char)*s >= 32 && (unsigned char)*s < 127 ? *s : '?';
    out[k] = 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: pdftest file.pdf [page scale out.ppm | text [page] | info]\n"); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* data = (u8*)malloc(n);
    fread(data, 1, n, f);
    fclose(f);
    char err[128] = "";
    PdfDoc* d = pdf_open(data, (u32)n, err, sizeof(err));
    if (!d) { printf("open failed: %s\n", err); return 1; }
    int np = pdf_page_count(d);
    const char* mode = argc > 2 ? argv[2] : "info";
    if (!strcmp(mode, "info")) {
        char t[200];
        printf("pages: %d\n", np);
        sil2ascii(pdf_info(d, "Title"), t, sizeof(t));
        printf("title: %s\n", t);
        sil2ascii(pdf_info(d, "Producer"), t, sizeof(t));
        printf("producer: %s\n", t);
        for (int i = 0; i < min(np, 3); i++) { float w, h; pdf_page_size(d, i, &w, &h); printf("page %d: %.1f x %.1f\n", i + 1, w, h); }
        int oc = pdf_outline_count(d);
        printf("outline: %d\n", oc);
        for (int i = 0; i < min(oc, 12); i++) { const PdfOutline* o = pdf_outline(d, i); sil2ascii(o->title, t, sizeof(t)); printf("  %*s%s -> %d\n", o->level * 2, "", t, o->page + 1); }
    } else if (!strcmp(mode, "text")) {
        int pg = argc > 3 ? atoi(argv[3]) - 1 : 0;
        const PdfChar* ch;
        int cnt = pdf_page_text(d, pg, &ch);
        for (int i = 0; i < cnt; i++) {
            u32 cp = ch[i].cp;
            if (cp < 128) putchar(cp == 10 ? '\n' : (int)cp); else { char b[8]; int m = utf8_encode(cp, b); fwrite(b, 1, m, stdout); }
        }
        putchar('\n');
    } else {
        int pg = atoi(mode) - 1;
        float scale = argc > 3 ? (float)strtod(argv[3], nullptr) : 1.0f;
        const char* out = argc > 4 ? argv[4] : "out.ppm";
        Image img;
        u64 t0 = ticks();
        if (!pdf_render(d, pg, scale, &img)) { printf("render failed\n"); return 1; }
        printf("rendered %dx%d in %d ms\n", img.w, img.h, (int)(ticks() - t0));
        FILE* o = fopen(out, "wb");
        fprintf(o, "P6\n%d %d\n255\n", img.w, img.h);
        for (int i = 0; i < img.w * img.h; i++) { u32 c = img.px[i]; fputc((c >> 16) & 255, o); fputc((c >> 8) & 255, o); fputc(c & 255, o); }
        fclose(o);
    }
    pdf_close(d);
    return 0;
}
