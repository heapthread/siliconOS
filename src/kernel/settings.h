// silicon - user settings persisted in /system/settings
#pragma once
#include "core.h"

struct Settings {
    char user[32];
    char hostname[32];
    int  wallpaper;          // index into the wallpaper list
    int  accent;             // index into accent_colors
    int  term_theme;         // console theme index
    int  term_scale;         // full-screen terminal font scale (1 or 2)
    bool clock24;
    bool show_seconds;
    int  dock_magnify;       // 0 = off, 1 = on
    bool sounds;
    bool show_fps;
    char shell[8];           // default shell flavour: sh, bash, zsh, fish
    int  boot_target;        // 0 terminal, 1 desktop
    bool net_enabled;
    bool fast_boot;
};
extern Settings settings;
extern const u32 accent_colors[];
extern const char* accent_names[];
extern const int accent_count;
u32  accent();
void settings_load();
void settings_save();
u32  settings_generation();
