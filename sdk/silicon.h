/* silicon SDK: the ABI between app modules (.sapp) and the silicon kernel.
 *
 * Write your app as plain C or C++ (freestanding: no libc, no exceptions, no RTTI), implement
 *
 *     extern "C" int silicon_app(SiApp* app);
 *
 * fill in `app` (title, size and the callbacks you need) and return 0. Everything the kernel offers
 * is reached through the `si` function table. Build with  python sdk/build_app.py main.cpp -o my.sapp
 * and open the .sapp from Files, the desktop or the shell (`open my.sapp`).
 *
 * Modules share the kernel's address space for now, so only run modules you trust.
 */
#ifndef SILICON_H
#define SILICON_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int u32;
typedef unsigned long long u64;
typedef struct SiCanvas SiCanvas;           /* opaque: pass it back to the drawing calls */

enum { SI_MOUSE_MOVE, SI_MOUSE_DOWN, SI_MOUSE_UP, SI_MOUSE_DOUBLE, SI_MOUSE_WHEEL, SI_MOUSE_LEAVE };
typedef struct { int kind, x, y, button, wheel, left; } SiMouse;       /* x, y relative to the window's client area */

/* key codes: printable ASCII is the character itself (lowercase letters) */
enum { SI_KEY_BACKSPACE = 8, SI_KEY_TAB = 9, SI_KEY_ENTER = 10, SI_KEY_ESC = 27,
       SI_KEY_UP = 0x100, SI_KEY_DOWN, SI_KEY_LEFT, SI_KEY_RIGHT, SI_KEY_HOME, SI_KEY_END, SI_KEY_PGUP, SI_KEY_PGDN,
       SI_KEY_INSERT, SI_KEY_DELETE };
typedef struct { int key, ch, down, shift, ctrl, alt; } SiKey;

typedef struct SiApi {
    u32 version;
    u32 size;
    /* drawing: colours are 0xAARRGGBB (alpha 255 = opaque), the canvas comes from the draw callback */
    void (*fill)(SiCanvas*, int x, int y, int w, int h, u32 argb);
    void (*rect)(SiCanvas*, int x, int y, int w, int h, u32 argb);
    void (*line)(SiCanvas*, int x0, int y0, int x1, int y1, u32 argb);
    void (*circle)(SiCanvas*, int cx, int cy, int r, u32 argb, int filled);
    int  (*text)(SiCanvas*, int x, int y, const char* s, u32 argb, int scale, int bold);   /* 8x12 font, returns the x after the text */
    int  (*text_width)(const char* s, int scale, int bold);
    void (*blit)(SiCanvas*, const u32* argb, int w, int h, int x, int y);
    /* the app and the system */
    void (*invalidate)(void);                              /* ask for a redraw */
    void (*set_title)(const char*);
    void (*notify)(const char* title, const char* body);
    void (*beep)(int kind);                                /* 0 click, 1 error, 2 success */
    u32  (*ticks)(void);                                   /* milliseconds since boot */
    u32  (*rand)(void);
    void* (*alloc)(u64 n);
    void (*free)(void* p);
    void (*log)(const char* msg);
    /* files (SiliconFS paths such as /home/notes/a.txt) */
    int  (*file_read)(const char* path, void* buf, int cap);          /* bytes read, -1 when missing */
    int  (*file_write)(const char* path, const void* buf, int len);   /* 0 on success; creates folders */
    int  (*file_size)(const char* path);                              /* -1 missing, -2 folder */
    int  (*dir_entry)(const char* path, int index, char* name, int cap);   /* 1 file, 2 folder, 0 no more */
    /* clipboard and desktop */
    void (*clip_set)(const char*);
    const char* (*clip_get)(void);
    void (*open_path)(const char* path);                              /* open a file with the right app */
    int  (*screen_w)(void);
    int  (*screen_h)(void);
} SiApi;

typedef struct SiApp {
    char title[48];
    int  width, height;                                    /* preferred client size */
    u32  background;                                       /* window background, 0xAARRGGBB */
    int  resizable;
    void (*draw)(SiCanvas*, int w, int h);
    void (*mouse)(const SiMouse*);
    void (*key)(const SiKey*);                             /* key down and key up events */
    void (*tick)(void);                                    /* about every frame */
    void (*closed)(void);
    void* reserved[5];
} SiApp;

extern const SiApi* si;                                    /* set by the startup code before silicon_app runs */
int silicon_app(SiApp* app);                               /* you implement this */

#ifdef __cplusplus
}
#endif
#endif
