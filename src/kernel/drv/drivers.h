// silicon - device drivers
#pragma once
#include "../core.h"
#include "../bootinfo.h"
#include "../gfx/gfx.h"

// ---------------------------------------------------------------- framebuffer
struct Framebuffer {
    u32* lfb;
    int  width, height, pitch;   // pitch in pixels
    Canvas back;                 // full-screen back buffer
};
extern Framebuffer fb;
void fb_init(BootInfo* bi);
void fb_present();
void fb_present_rect(Rect r);

// ---------------------------------------------------------------- keyboard
enum Key : u16 {
    K_NONE = 0,
    K_BACKSPACE = 8, K_TAB = 9, K_ENTER = 10, K_ESC = 27,
    K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_INSERT, K_DELETE,
    K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12,
    K_LSHIFT, K_RSHIFT, K_LCTRL, K_RCTRL, K_LALT, K_RALT, K_CAPS, K_SUPER,
};
struct KeyEvent {
    u16  key;        // printable ASCII or Key
    char ch;         // translated character (0 if none)
    bool down;
    bool shift, ctrl, alt;
};
void kbd_init();
bool kbd_poll(KeyEvent& ev);

// ---------------------------------------------------------------- mouse
struct MouseState {
    int  x, y;
    bool left, right, middle;
    int  wheel;              // accumulated wheel delta since last read
};
void mouse_init(int screen_w, int screen_h);
bool mouse_update(MouseState& st);   // true if anything changed
bool mouse_absolute();               // vmmouse active (no grab needed)
void mouse_warp(int x, int y);

// ---------------------------------------------------------------- rtc
struct DateTime { int year, month, day, hour, minute, second, weekday; };
void rtc_read(DateTime& dt);
u32  rtc_stamp();                    // seconds-ish timestamp for file times
void stamp_to_datetime(u32 stamp, DateTime& dt);
const char* month_name(int m);
const char* weekday_name(int d);

// ---------------------------------------------------------------- ata
bool ata_init();
bool ata_present();
bool ata_read(u32 lba, u32 count, void* buf);
bool ata_write(u32 lba, u32 count, const void* buf);
u32  ata_sectors();

// ---------------------------------------------------------------- power / misc
[[noreturn]] void power_off();
[[noreturn]] void power_reboot();
void speaker_beep(u32 hz, u32 ms);
void speaker_play_now(u32 hz, u32 ms);   // interrupts the queue

// ---------------------------------------------------------------- memory facts
extern u64 mem_total_bytes;
