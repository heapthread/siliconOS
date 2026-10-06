// silicon - settings
#include "settings.h"
#include "fs/fs.h"

Settings settings;

const u32 accent_colors[] = {0xFF9B7BFF, 0xFF4FD1C5, 0xFFFF7A9A, 0xFFFFB347, 0xFF7ED957, 0xFF5AA9FF};
const char* accent_names[] = {"wafer violet", "etch teal", "solder pink", "copper", "trace green", "laser blue"};
const int accent_count = ARRAY_LEN(accent_colors);

u32 accent() { return accent_colors[clamp(settings.accent, 0, accent_count - 1)]; }

static void defaults() {
    strlcpy(settings.user, "user", sizeof(settings.user));
    strlcpy(settings.hostname, "silicon", sizeof(settings.hostname));
    settings.wallpaper = 0;
    settings.accent = 0;
    settings.term_theme = 0;
    settings.term_scale = 1;
    settings.clock24 = true;
    settings.show_seconds = false;
    settings.dock_magnify = 1;
    settings.sounds = true;
    settings.show_fps = false;
    strlcpy(settings.shell, "bash", sizeof(settings.shell));
    settings.boot_target = 0;
    settings.net_enabled = true;
    settings.fast_boot = false;
    settings.fps_limit = 0;
    settings.res_w = settings.res_h = 0;
    settings.greeter = true;
    settings.pass[0] = 0;
    settings.sleep_min = 15;
    settings.saver_min = 5;
    settings.dnd = false;
    settings.kbd_layout = 0;
    settings.muted[0] = 0;
}

static u32 gen = 0;
u32 settings_generation() { return gen; }

void settings_load() {
    defaults();
    FsNode* f = fs_lookup("/system/settings");
    if (!f) return;
    const char* p = fs_read_text(f);
    char key[32], val[128];
    while (*p) {
        int k = 0, v = 0;
        while (*p && *p != '=' && *p != '\n') { if (k < 31) key[k++] = *p; p++; }
        key[k] = 0;
        if (*p == '=') p++;
        while (*p && *p != '\n') { if (v < 127) val[v++] = *p; p++; }
        val[v] = 0;
        if (*p == '\n') p++;
        if (!strcmp(key, "user")) strlcpy(settings.user, val, sizeof(settings.user));
        else if (!strcmp(key, "hostname")) strlcpy(settings.hostname, val, sizeof(settings.hostname));
        else if (!strcmp(key, "wallpaper")) settings.wallpaper = atoi(val);
        else if (!strcmp(key, "accent")) settings.accent = atoi(val);
        else if (!strcmp(key, "term_theme")) settings.term_theme = atoi(val);
        else if (!strcmp(key, "term_scale")) settings.term_scale = clamp(atoi(val), 1, 2);
        else if (!strcmp(key, "clock24")) settings.clock24 = atoi(val);
        else if (!strcmp(key, "show_seconds")) settings.show_seconds = atoi(val);
        else if (!strcmp(key, "dock_magnify")) settings.dock_magnify = atoi(val);
        else if (!strcmp(key, "sounds")) settings.sounds = atoi(val);
        else if (!strcmp(key, "show_fps")) settings.show_fps = atoi(val);
        else if (!strcmp(key, "shell")) strlcpy(settings.shell, val, sizeof(settings.shell));
        else if (!strcmp(key, "boot_target")) settings.boot_target = atoi(val);
        else if (!strcmp(key, "net_enabled")) settings.net_enabled = atoi(val);
        else if (!strcmp(key, "fast_boot")) settings.fast_boot = atoi(val);
        else if (!strcmp(key, "fps_limit")) settings.fps_limit = clamp(atoi(val), 0, 1000);
        else if (!strcmp(key, "res_w")) settings.res_w = atoi(val);
        else if (!strcmp(key, "res_h")) settings.res_h = atoi(val);
        else if (!strcmp(key, "greeter")) settings.greeter = atoi(val);
        else if (!strcmp(key, "pass")) strlcpy(settings.pass, val, sizeof(settings.pass));
        else if (!strcmp(key, "sleep_min")) settings.sleep_min = clamp(atoi(val), 0, 600);
        else if (!strcmp(key, "saver_min")) settings.saver_min = clamp(atoi(val), 0, 600);
        else if (!strcmp(key, "kbd")) settings.kbd_layout = clamp(atoi(val), 0, 15);
        else if (!strcmp(key, "dnd")) settings.dnd = atoi(val) != 0;
        else if (!strcmp(key, "muted")) strlcpy(settings.muted, val, sizeof(settings.muted));
    }
}

void settings_save() {
    gen++;
    char b[1024];
    snprintf(b, sizeof(b),
             "user=%s\nhostname=%s\nwallpaper=%d\naccent=%d\nterm_theme=%d\nterm_scale=%d\n"
             "clock24=%d\nshow_seconds=%d\ndock_magnify=%d\nsounds=%d\nshow_fps=%d\nshell=%s\n"
             "boot_target=%d\nnet_enabled=%d\nfast_boot=%d\nfps_limit=%d\nres_w=%d\nres_h=%d\ngreeter=%d\npass=%s\n"
             "sleep_min=%d\nsaver_min=%d\ndnd=%d\nmuted=%s\nkbd=%d\n",
             settings.user, settings.hostname, settings.wallpaper, settings.accent, settings.term_theme,
             settings.term_scale, settings.clock24, settings.show_seconds, settings.dock_magnify, settings.sounds,
             settings.show_fps, settings.shell, settings.boot_target, settings.net_enabled, settings.fast_boot,
             settings.fps_limit, settings.res_w, settings.res_h, settings.greeter, settings.pass, settings.sleep_min,
             settings.saver_min, settings.dnd, settings.muted, settings.kbd_layout);
    fs_write_text("/system/settings", b);
}
