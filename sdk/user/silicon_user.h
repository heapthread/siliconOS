// silicon SDK for user programs (.uapp): programs that run in ring 3 with their own address space.
//
// A program is plain freestanding C++ with an entry point
//
//     int app_main();
//
// Everything the system offers is a system call (int 0x80), wrapped by the functions below.
// Windows are a pixel buffer you draw into (or use the si::fill / si::text helpers, which draw
// with the system's fonts) plus a stream of events. A crash only ends your own process.
//
// Build:  python sdk/build_uapp.py main.cpp -o my.uapp      Run:  open my.uapp (or `run my.uapp` in the shell)
#pragma once

extern "C" {
void* memcpy(void* d, const void* s, unsigned long long n);
void* memmove(void* d, const void* s, unsigned long long n);
void* memset(void* d, int c, unsigned long long n);
int   memcmp(const void* a, const void* b, unsigned long long n);
unsigned long long strlen(const char* s);
}

namespace si {

typedef unsigned char  u8;
typedef unsigned int   u32;
typedef unsigned long long u64;
typedef long long word;                  // a machine word (long is 32 bits with this toolchain)

enum EventType { EV_NONE, EV_KEY, EV_MOUSE, EV_CLOSE, EV_RESIZE };
enum MouseKind { MOUSE_MOVE, MOUSE_DOWN, MOUSE_UP, MOUSE_DOUBLE, MOUSE_WHEEL, MOUSE_LEAVE };
enum Key { KEY_BACKSPACE = 8, KEY_TAB = 9, KEY_ENTER = 10, KEY_ESC = 27,
           KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_INSERT, KEY_DELETE };
enum { TEXT_BOLD = 1 };

// key:    a = key code, b = character (Unicode), c = 1 down / 0 up, d = shift 1 | ctrl 2 | alt 4
// mouse:  a = x, b = y, c = button (1 left, 2 right), d = MouseKind | (left button held ? 0x100 : 0)    wheel: c = delta
// resize: a = new width, b = new height (the pixel buffer now has this size)
struct Event { u32 type; int a, b, c, d; };

static inline word sys(word n, word a1 = 0, word a2 = 0, word a3 = 0, word a4 = 0, word a5 = 0) {
    word ret;
    register word r10 __asm__("r10") = a4;
    register word r8 __asm__("r8") = a5;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8) : "memory", "rcx", "r11");
    return ret;
}

enum { S_EXIT = 0, S_YIELD = 1, S_SLEEP = 2, S_TICKS = 3, S_LOG = 4, S_GETPID = 5, S_RAND = 6,
       S_WIN_OPEN = 10, S_WIN_PRESENT = 11, S_WIN_TITLE = 12, S_WIN_EVENT = 13, S_WIN_SIZE = 14, S_GFX = 20,
       S_FILE_READ = 30, S_FILE_WRITE = 31, S_FILE_SIZE = 32, S_DIR_ENTRY = 33,
       S_MEM_GROW = 40, S_THREAD = 41, S_THREAD_EXIT = 42, S_CLIP_SET = 50, S_CLIP_GET = 51, S_NOTIFY = 60 };
enum { G_FILL = 1, G_RRECT = 2, G_TEXT = 3, G_TEXT_W = 4, G_ICON = 5, G_LINE = 6, G_DISC = 7, G_CLEAR = 8, G_RECT = 9 };

// ---- process
[[noreturn]] static inline void exit(int code) { sys(S_EXIT, code); for (;;) {} }
static inline void yield() { sys(S_YIELD); }
static inline void sleep(unsigned ms) { sys(S_SLEEP, ms); }
static inline u64 ticks() { return (u64)sys(S_TICKS); }              // milliseconds since boot
static inline int pid() { return (int)sys(S_GETPID); }
static inline u32 rand() { return (u32)sys(S_RAND); }
static inline void log(const char* s) { sys(S_LOG, (word)s); }

// ---- window: win_open returns the pixel buffer (0xAARRGGBB, rows are `width` pixels apart)
static inline u32* win_open(int w, int h, const char* title, int flags = 0 /* 1 = resizable */) { return (u32*)sys(S_WIN_OPEN, w, h, (word)title, flags); }
static inline void win_size(int* w, int* h) { word v = sys(S_WIN_SIZE); *w = (int)(v & 0xFFFF); *h = (int)((v >> 16) & 0xFFFF); }
static inline void present() { sys(S_WIN_PRESENT); }                  // tell the desktop the pixels changed
static inline void set_title(const char* t) { sys(S_WIN_TITLE, (word)t); }
static inline bool event(Event* e, bool block = false) { return sys(S_WIN_EVENT, (word)e, block ? 1 : 0) == 1; }

// ---- drawing helpers (system fonts, anti-aliased shapes); colours 0xRRGGBB
static inline word gfx(word op, int a0 = 0, int a1 = 0, int a2 = 0, int a3 = 0, int a4 = 0, int a5 = 0, const char* s = nullptr) {
    int a[8] = {a0, a1, a2, a3, a4, a5, 0, 0};
    return sys(S_GFX, op, (word)a, (word)s);
}
static inline void clear(u32 rgb) { gfx(G_CLEAR, (int)rgb); }
static inline void fill(int x, int y, int w, int h, u32 rgb) { gfx(G_FILL, x, y, w, h, (int)rgb); }
static inline void frame(int x, int y, int w, int h, u32 rgb) { gfx(G_RECT, x, y, w, h, (int)rgb); }
static inline void rrect(int x, int y, int w, int h, int r, u32 rgb) { gfx(G_RRECT, x, y, w, h, r, (int)rgb); }
static inline void line(int x0, int y0, int x1, int y1, u32 rgb) { gfx(G_LINE, x0, y0, x1, y1, (int)rgb); }
static inline void disc(int cx, int cy, int r, u32 rgb) { gfx(G_DISC, cx, cy, r, (int)rgb); }
static inline int text(int x, int y, const char* s, u32 rgb, int flags = 0, int scale = 1) { return (int)gfx(G_TEXT, x, y, (int)rgb, flags | (scale << 8), 0, 0, s); }
static inline int text_width(const char* s, int flags = 0, int scale = 1) { return (int)gfx(G_TEXT_W, 0, 0, 0, flags | (scale << 8), 0, 0, s); }
static inline void icon(const char* name, int x, int y, int size = 32) { gfx(G_ICON, x, y, size, 0, 0, 0, name); }

// ---- files (SiliconFS). Reads work everywhere except system secrets; writes only below /home
static inline int file_read(const char* path, void* buf, int cap) { return (int)sys(S_FILE_READ, (word)path, (word)buf, cap); }
static inline int file_write(const char* path, const void* buf, int len) { return (int)sys(S_FILE_WRITE, (word)path, (word)buf, len); }
static inline int file_size(const char* path) { return (int)sys(S_FILE_SIZE, (word)path); }           // -1 missing, -2 folder
static inline int dir_entry(const char* path, int i, char* name, int cap) { return (int)sys(S_DIR_ENTRY, (word)path, i, (word)name, cap); }   // 1 file, 2 folder, 0 end

// ---- clipboard and notifications
static inline void clip_set(const char* s) { sys(S_CLIP_SET, (word)s); }
static inline int clip_get(char* buf, int cap) { return (int)sys(S_CLIP_GET, (word)buf, cap); }
static inline void notify(const char* title, const char* body) { sys(S_NOTIFY, (word)title, (word)body); }

// ---- memory and threads
static inline void* mem_grow(unsigned long long bytes) { return (void*)sys(S_MEM_GROW, (word)bytes); }
void* malloc(unsigned long long n);
void  free(void* p);
int   thread(void (*fn)(void*), void* arg);      // another thread in this process, preemptively scheduled; returns its id or -1

}  // namespace si

extern "C" int app_main();
