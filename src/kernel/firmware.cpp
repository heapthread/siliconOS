// silicon - firmware: a BIOS-style power-on self test screen, the setup utility (DEL / F2) and a
// boot menu (F12). The options live in the normal settings file, so the OS and the firmware agree.
#include "core.h"
#include "settings.h"
#include "firmware.h"
#include "arch/arch.h"
#include "drv/drivers.h"
#include "drv/pci.h"
#include "term/console.h"
#include "fs/fs.h"

extern u64 mem_total_bytes;

// classic firmware palette
static const u32 FW_BLUE = 0xFF0000A8, FW_BLUE_DK = 0xFF000070, FW_GRAY = 0xFFAAAAAA, FW_WHITE = 0xFFFFFFFF;
static const u32 FW_YELLOW = 0xFFFFFF55, FW_CYAN = 0xFF55FFFF, FW_BLACK = 0xFF000000, FW_DIM = 0xFF5555AA;

static Canvas& scr() { return fb.back; }

static bool poll_key(KeyEvent& e) {
    while (kbd_poll(e)) if (e.down) return true;
    return false;
}

// ================================================================ POST
static int post_y = 0;
static void post_line(const char* fmt, ...) {
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    scr().fill(0, post_y, fb.width - 230, 16, FW_BLACK);
    scr().text(24, post_y, b, FW_GRAY);
    fb_present_rect({0, post_y, fb.width, 16});
    post_y += 16;
}

static void draw_post_logo(Canvas& c) {
    // "energy chip" logo in the top right, like the stickers on old boards
    int x = fb.width - 200, y = 24;
    c.fill(x, y, 172, 96, FW_BLACK);
    for (int i = 0; i < 6; i++) { c.fill(x + 30 + i * 14, y + 4, 6, 10, 0xFFE6B450); c.fill(x + 30 + i * 14, y + 82, 6, 10, 0xFFE6B450); }
    c.fill(x + 20, y + 14, 112, 68, 0xFF2A2440);
    c.rect(x + 20, y + 14, 112, 68, 0xFF6A5E9A);
    c.fill(x + 50, y + 30, 52, 36, 0xFF9B7BFF);
    c.fill(x + 60, y + 38, 32, 20, 0xFFDCCFFF);
    c.text(x + 140, y + 30, "s", 0xFF7FE0A8, 3, true);
    c.text(x + 18, y + 98, "silicon ready", 0xFF7FE0A8, 1, true);
}

static int post_screen(bool* enter_setup, bool* boot_menu) {
    Canvas& c = scr();
    c.clear(FW_BLACK);
    draw_post_logo(c);
    c.text(24, 24, "silicon firmware  v1.0", FW_WHITE, 2, true);
    c.text(24, 52, "Copyright (C) 2026, silicon project. Hand-built in assembly and C++.", FW_GRAY);
    fb_present();
    post_y = 90;
    post_line("%s", cpu_brand());
    post_line("Speed: %u MHz", cpu_mhz());
    post_line("");
    // memory test: count up like the real thing
    u32 total_k = (u32)(mem_total_bytes >> 10);
    int steps = settings.fast_boot ? 4 : 24;
    for (int i = 1; i <= steps; i++) {
        char b[80];
        snprintf(b, sizeof(b), "Memory Test : %8uK%s", (u32)((u64)total_k * i / steps), i == steps ? " OK" : "");
        c.fill(0, post_y, fb.width - 230, 16, FW_BLACK);
        c.text(24, post_y, b, i == steps ? FW_WHITE : FW_GRAY);
        fb_present_rect({0, post_y, fb.width, 16});
        sleep_ms(settings.fast_boot ? 5 : 22);
    }
    post_y += 32;
    post_line("Detecting IDE drives ...");
    if (ata_present()) post_line("  Primary Master   : QEMU HARDDISK  %u MB", ata_sectors() / 2048);
    else post_line("  Primary Master   : None");
    post_line("  Primary Slave    : None");
    post_line("");
    if (!pci_count()) pci_scan();
    post_line("PCI device listing ...");
    post_line("  Bus Dev Fun  Vendor Device  Class  Name");
    for (int i = 0; i < pci_count(); i++) {
        PciDevice* d = pci_get(i);
        post_line("  %3u %3u %3u   %04x   %04x    %02x%02x  %s", d->bus, d->dev, d->fn, d->vendor, d->device, d->cls, d->subcls, d->name);
    }
    post_line("");
    bool nic = pci_find(0x10EC, 0x8139) != nullptr;
    post_line("Onboard LAN      : %s", nic ? (settings.net_enabled ? "Realtek RTL8139, enabled" : "Realtek RTL8139, disabled in setup") : "not present");
    post_line("Boot target      : %s%s", settings.boot_target ? "desktop" : "terminal", settings.fast_boot ? " (fast boot)" : "");
    // the famous prompt
    int py = fb.height - 40;
    c.fill(0, py - 4, fb.width, 24, FW_BLACK);
    c.text(24, py, "Press DEL or F2 to enter SETUP, F12 for Boot Menu", FW_WHITE, 1, true);
    fb_present_rect({0, py - 4, fb.width, 24});
    u64 until = ticks() + (settings.fast_boot ? 250 : 1600);
    *enter_setup = *boot_menu = false;
    while (ticks() < until) {
        KeyEvent e;
        if (poll_key(e)) {
            if (e.key == K_DELETE || e.key == K_F2) { *enter_setup = true; break; }
            if (e.key == K_F12) { *boot_menu = true; break; }
            if (e.key == K_ENTER || e.key == K_ESC) break;
        }
        // a little countdown bar
        int left = (int)(until - ticks());
        int w = left * 300 / (settings.fast_boot ? 250 : 1600);
        c.fill(24, py + 16, 300, 2, 0xFF222222);
        c.fill(24, py + 16, w, 2, FW_GRAY);
        fb_present_rect({24, py + 16, 300, 2});
        cpu_rest();
    }
    if (*enter_setup) { c.fill(0, py - 4, fb.width, 24, FW_BLACK); c.text(24, py, "Entering SETUP ...", FW_YELLOW, 1, true); fb_present(); sleep_ms(250); }
    return 0;
}

// ================================================================ setup utility
enum ItemKind { IT_INFO, IT_CHOICE, IT_ACTION, IT_SPACER };
struct Item {
    const char* label;
    ItemKind kind;
    const char* const* choices;
    int nchoices;
    int* value;
    const char* help;
    int action;
};

static const char* const ENABLED[] = {"Disabled", "Enabled"};
static const char* const SHELLS[] = {"bash", "zsh", "fish"};
static const char* const SIZES[] = {"Normal (1x)", "Large (2x)"};
static const char* const TARGETS[] = {"Terminal", "Desktop"};
static const char* const SHOWN[] = {"Hidden", "Shown"};
static const char* const CLOCKS[] = {"12-hour", "24-hour"};
static const char* theme_names[16];

// working copies of the settings while the user edits
static int v_net, v_shell, v_scale, v_theme, v_sounds, v_fps, v_clock, v_target, v_fast;
static bool changed = false;

static void load_values() {
    v_net = settings.net_enabled;
    v_shell = !strcmp(settings.shell, "zsh") ? 1 : !strcmp(settings.shell, "fish") ? 2 : 0;
    v_scale = settings.term_scale >= 2 ? 1 : 0;
    v_theme = clamp(settings.term_theme, 0, console_theme_count - 1);
    v_sounds = settings.sounds;
    v_fps = settings.show_fps;
    v_clock = settings.clock24;
    v_target = settings.boot_target ? 1 : 0;
    v_fast = settings.fast_boot;
    changed = false;
}
static void store_values() {
    settings.net_enabled = v_net;
    strlcpy(settings.shell, SHELLS[v_shell], sizeof(settings.shell));
    settings.term_scale = v_scale ? 2 : 1;
    settings.term_theme = v_theme;
    settings.sounds = v_sounds;
    settings.show_fps = v_fps;
    settings.clock24 = v_clock;
    settings.boot_target = v_target;
    settings.fast_boot = v_fast;
    settings_save();
}
static void load_defaults() {
    v_net = 1; v_shell = 0; v_scale = 0; v_theme = 0; v_sounds = 1; v_fps = 0; v_clock = 1; v_target = 0; v_fast = 0;
    changed = true;
}

enum { A_SAVE_EXIT = 1, A_DISCARD_EXIT, A_DEFAULTS, A_SAVE_REBOOT, A_SHUTDOWN };

static char info_buf[8][96];
static Item tab_main[] = {
    {"System Time", IT_INFO, nullptr, 0, nullptr, "The time from the real-time clock. silicon reads it in local time.", 0},
    {"System Date", IT_INFO, nullptr, 0, nullptr, "The date from the real-time clock.", 0},
    {"", IT_SPACER, nullptr, 0, nullptr, "", 0},
    {"Processor", IT_INFO, nullptr, 0, nullptr, "The processor silicon is running on.", 0},
    {"Processor Speed", IT_INFO, nullptr, 0, nullptr, "Measured against the PIT at boot (TSC).", 0},
    {"Total Memory", IT_INFO, nullptr, 0, nullptr, "Usable RAM reported by the E820 memory map.", 0},
    {"Boot Disk", IT_INFO, nullptr, 0, nullptr, "The ATA disk that holds the bootloader, the kernel and SiliconFS.", 0},
    {"Firmware", IT_INFO, nullptr, 0, nullptr, "silicon firmware, the same binary as the kernel.", 0},
};
static Item tab_adv[] = {
    {"Onboard LAN (RTL8139)", IT_CHOICE, ENABLED, 2, &v_net, "Turn the network card off to boot without networking.", 0},
    {"Default Shell", IT_CHOICE, SHELLS, 3, &v_shell, "Which shell the terminal starts: bash, zsh or fish.", 0},
    {"Terminal Font Size", IT_CHOICE, SIZES, 2, &v_scale, "Font size of the full-screen terminal.", 0},
    {"Terminal Theme", IT_CHOICE, theme_names, 0, &v_theme, "Colour theme of the terminal.", 0},
    {"System Sounds", IT_CHOICE, ENABLED, 2, &v_sounds, "PC speaker beeps for clicks, errors and games.", 0},
    {"FPS Counter", IT_CHOICE, SHOWN, 2, &v_fps, "Show frames per second in the desktop's top bar.", 0},
    {"Clock Format", IT_CHOICE, CLOCKS, 2, &v_clock, "How the desktop clock shows the time.", 0},
};
static Item tab_boot[] = {
    {"Boot Into", IT_CHOICE, TARGETS, 2, &v_target, "Start in the terminal (type 'ui' for the desktop) or go straight to the desktop.", 0},
    {"Fast Boot", IT_CHOICE, ENABLED, 2, &v_fast, "Skip most of this screen's delay and the long boot animation.", 0},
    {"", IT_SPACER, nullptr, 0, nullptr, "", 0},
    {"1st Boot Device", IT_INFO, nullptr, 0, nullptr, "silicon boots from its own MBR bootloader on the ATA disk.", 0},
    {"2nd Boot Device", IT_INFO, nullptr, 0, nullptr, "Network boot isn't supported (yet).", 0},
};
static Item tab_exit[] = {
    {"Save Changes and Exit", IT_ACTION, nullptr, 0, nullptr, "Write the settings to SiliconFS and continue booting.", A_SAVE_EXIT},
    {"Discard Changes and Exit", IT_ACTION, nullptr, 0, nullptr, "Forget what you changed and continue booting.", A_DISCARD_EXIT},
    {"Load Setup Defaults", IT_ACTION, nullptr, 0, nullptr, "Put every firmware option back to its default.", A_DEFAULTS},
    {"", IT_SPACER, nullptr, 0, nullptr, "", 0},
    {"Save Changes and Reboot", IT_ACTION, nullptr, 0, nullptr, "Save, then restart the machine.", A_SAVE_REBOOT},
    {"Shut Down", IT_ACTION, nullptr, 0, nullptr, "Turn the machine off without saving.", A_SHUTDOWN},
};
struct Tab { const char* name; Item* items; int n; };
static Tab tabs[] = {
    {"Main", tab_main, ARRAY_LEN(tab_main)},
    {"Advanced", tab_adv, ARRAY_LEN(tab_adv)},
    {"Boot", tab_boot, ARRAY_LEN(tab_boot)},
    {"Exit", tab_exit, ARRAY_LEN(tab_exit)},
};

static const char* info_value(int tab, int i) {
    char* b = info_buf[i % 8];
    if (tab == 0) {
        DateTime d;
        rtc_read(d);
        static const char* wd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        switch (i) {
        case 0: snprintf(b, 96, "[%02d:%02d:%02d]", d.hour, d.minute, d.second); break;
        case 1: snprintf(b, 96, "[%s %02d/%02d/%04d]", wd[clamp(d.weekday, 0, 6)], d.month, d.day, d.year); break;
        case 3: strlcpy(b, cpu_brand(), 96); break;
        case 4: snprintf(b, 96, "%u MHz", cpu_mhz()); break;
        case 5: snprintf(b, 96, "%u MB", (u32)(mem_total_bytes >> 20)); break;
        case 6: snprintf(b, 96, ata_present() ? "ATA primary master, %u MB" : "none", ata_sectors() / 2048); break;
        case 7: strlcpy(b, "silicon fw 1.0 (" SILICON_VERSION ")", 96); break;
        default: b[0] = 0;
        }
    } else if (tab == 2) strlcpy(b, i == 3 ? "[SiliconFS on ATA]" : "[Network - disabled]", 96);
    else b[0] = 0;
    return b;
}

static void frame(Canvas& c, int x, int y, int w, int h, u32 bg, u32 line) {
    c.fill(x, y, w, h, bg);
    c.rect(x + 3, y + 3, w - 6, h - 6, line);
    c.rect(x + 5, y + 5, w - 10, h - 10, line);
}

static void draw_setup(int tab, int sel, int popup, int popup_sel) {
    Canvas& c = scr();
    int W = fb.width, H = fb.height;
    c.clear(FW_BLUE);
    // title + tabs
    c.fill(0, 0, W, 28, FW_GRAY);
    c.text_centered(0, 7, W, "silicon firmware setup utility  -  Copyright (C) 2026 silicon", FW_BLUE, 1, true);
    c.fill(0, 28, W, 26, FW_BLUE_DK);
    int tx = 24;
    for (int i = 0; i < ARRAY_LEN(tabs); i++) {
        int tw = text_width(tabs[i].name, 1, true) + 28;
        if (i == tab) c.fill(tx, 30, tw, 24, FW_GRAY);
        c.text(tx + 14, 36, tabs[i].name, i == tab ? FW_BLUE : FW_WHITE, 1, true);
        tx += tw + 6;
    }
    // items panel + help panel
    int top = 64, ph = H - top - 64;
    int lw = W * 64 / 100;
    frame(c, 12, top, lw, ph, FW_GRAY, FW_BLUE);
    frame(c, 12 + lw + 6, top, W - lw - 30, ph, FW_GRAY, FW_BLUE);
    c.text_centered(12 + lw + 6, top + 14, W - lw - 30, "Item Specific Help", FW_BLUE, 1, true);
    c.hline(12 + lw + 18, top + 32, W - lw - 54, FW_BLUE);
    Tab& t = tabs[tab];
    int y = top + 22;
    for (int i = 0; i < t.n; i++, y += 26) {
        Item& it = t.items[i];
        if (it.kind == IT_SPACER) continue;
        bool on = i == sel;
        int lx = 34;
        if (it.kind == IT_ACTION) {
            if (on) c.fill(lx - 6, y - 4, lw - 46, 22, FW_BLUE);
            c.text(lx, y, it.label, on ? FW_WHITE : FW_BLUE, 1, true);
            continue;
        }
        c.text(lx, y, it.label, it.kind == IT_INFO ? FW_DIM : FW_BLUE, 1, it.kind != IT_INFO);
        int vx = lx + 250;
        const char* v = it.kind == IT_INFO ? info_value(tab, i) : it.choices[*it.value];
        char b[120];
        if (it.kind == IT_CHOICE) snprintf(b, sizeof(b), "[%s]", v);
        else strlcpy(b, v, sizeof(b));
        if (on) c.fill(vx - 4, y - 4, text_width(b, 1, true) + 8, 22, FW_BLUE);
        c.text(vx, y, b, on ? FW_WHITE : (it.kind == IT_INFO ? FW_BLACK : FW_BLUE), 1, it.kind == IT_CHOICE);
    }
    // help text, wrapped
    const char* help = t.items[sel].help;
    int hx = 12 + lw + 22, hy = top + 44, hw = (W - lw - 64) / 8;
    while (*help) {
        int n = (int)strlen(help);
        if (n > hw) { n = hw; while (n > 0 && help[n] != ' ') n--; if (!n) n = hw; }
        c.text_n(hx, hy, help, n, FW_BLUE);
        hy += 16;
        help += n;
        while (*help == ' ') help++;
    }
    if (changed) c.text(hx, top + ph - 30, "* unsaved changes", 0xFFAA0000, 1, true);
    // key legend
    c.fill(0, H - 52, W, 52, FW_BLUE_DK);
    c.text(24, H - 44, G_LEFT G_RIGHT " Select Screen   " G_UP G_DOWN " Select Item   Enter Change   +/- Cycle", FW_WHITE);
    c.text(24, H - 26, "F9 Setup Defaults   F10 Save and Exit   Esc Exit", FW_WHITE);
    c.text(W - 230, H - 35, "silicon fw 1.0", FW_CYAN, 1, true);
    // choice popup
    if (popup >= 0) {
        Item& it = t.items[popup];
        int pw = 260, phh = 40 + it.nchoices * 20;
        int px = (W - pw) / 2, py = (H - phh) / 2;
        c.fill(px + 8, py + 8, pw, phh, FW_BLACK);
        frame(c, px, py, pw, phh, FW_GRAY, FW_BLUE);
        c.text_centered(px, py + 12, pw, it.label, FW_BLUE, 1, true);
        for (int k = 0; k < it.nchoices; k++) {
            int yy = py + 32 + k * 20;
            if (k == popup_sel) c.fill(px + 14, yy - 3, pw - 28, 18, FW_BLUE);
            c.text(px + 24, yy, it.choices[k], k == popup_sel ? FW_WHITE : FW_BLUE, 1, true);
        }
    }
    fb_present();
}

static int first_selectable(Tab& t, int from, int dir) {
    for (int k = 0; k < t.n; k++) {
        int i = ((from + dir * k) % t.n + t.n) % t.n;
        if (t.items[i].kind == IT_CHOICE || t.items[i].kind == IT_ACTION || (t.items[i].kind == IT_INFO && dir)) return i;
    }
    return 0;
}

static bool confirm(const char* q) {
    Canvas& c = scr();
    int W = fb.width, H = fb.height;
    int pw = 360, ph = 90, px = (W - pw) / 2, py = (H - ph) / 2;
    c.fill(px + 8, py + 8, pw, ph, FW_BLACK);
    frame(c, px, py, pw, ph, 0xFFAA0000, FW_WHITE);
    c.text_centered(px, py + 22, pw, q, FW_WHITE, 1, true);
    c.text_centered(px, py + 52, pw, "[Y]es    [N]o", FW_YELLOW, 1, true);
    fb_present();
    for (;;) {
        KeyEvent e;
        if (poll_key(e)) {
            if (e.key == 'y' || e.key == K_ENTER) return true;
            if (e.key == 'n' || e.key == K_ESC) return false;
        }
        cpu_idle();
    }
}

void firmware_setup() {
    for (int i = 0; i < console_theme_count && i < 16; i++) theme_names[i] = console_themes[i].name;
    tab_adv[3].nchoices = min(console_theme_count, 16);
    load_values();
    int tab = 0, sel = 0, popup = -1, psel = 0;
    u64 last_draw = 0;
    bool dirty = true;
    for (;;) {
        if (dirty || ticks() - last_draw > 500) {     // the clock ticks on the Main page
            draw_setup(tab, sel, popup, psel);
            last_draw = ticks();
            dirty = false;
        }
        KeyEvent e;
        if (!poll_key(e)) { cpu_idle(); continue; }
        dirty = true;
        Tab& t = tabs[tab];
        if (popup >= 0) {
            Item& it = t.items[popup];
            if (e.key == K_UP) psel = (psel + it.nchoices - 1) % it.nchoices;
            else if (e.key == K_DOWN) psel = (psel + 1) % it.nchoices;
            else if (e.key == K_ENTER) { if (*it.value != psel) changed = true; *it.value = psel; popup = -1; }
            else if (e.key == K_ESC) popup = -1;
            continue;
        }
        switch (e.key) {
        case K_LEFT: tab = (tab + ARRAY_LEN(tabs) - 1) % ARRAY_LEN(tabs); sel = first_selectable(tabs[tab], 0, 1); break;
        case K_RIGHT: tab = (tab + 1) % ARRAY_LEN(tabs); sel = first_selectable(tabs[tab], 0, 1); break;
        case K_UP: sel = first_selectable(t, sel - 1, -1); break;
        case K_DOWN: sel = first_selectable(t, sel + 1, 1); break;
        case '+': case '=': case '-': case ' ': {
            Item& it = t.items[sel];
            if (it.kind == IT_CHOICE) { *it.value = (*it.value + (e.key == '-' ? it.nchoices - 1 : 1)) % it.nchoices; changed = true; }
            break;
        }
        case K_F9: if (confirm("Load setup defaults?")) load_defaults(); break;
        case K_F10: if (confirm("Save configuration and exit?")) { store_values(); return; } break;
        case K_ESC: if (!changed || confirm("Quit without saving?")) return; break;
        case K_ENTER: {
            Item& it = t.items[sel];
            if (it.kind == IT_CHOICE) { popup = sel; psel = *it.value; }
            else if (it.kind == IT_ACTION) {
                switch (it.action) {
                case A_SAVE_EXIT: if (confirm("Save configuration and exit?")) { store_values(); return; } break;
                case A_DISCARD_EXIT: if (confirm("Discard changes and exit?")) return; break;
                case A_DEFAULTS: if (confirm("Load setup defaults?")) load_defaults(); break;
                case A_SAVE_REBOOT: if (confirm("Save and reboot?")) { store_values(); fs_sync(); power_reboot(); } break;
                case A_SHUTDOWN: if (confirm("Shut down now?")) power_off(); break;
                }
            }
            break;
        }
        }
    }
}

// ================================================================ boot menu
static int boot_menu() {
    static const char* const opts[] = {"silicon  -  terminal", "silicon  -  desktop", "Enter Setup", "Reboot"};
    int sel = settings.boot_target ? 1 : 0;
    for (;;) {
        Canvas& c = scr();
        int W = fb.width, H = fb.height;
        int pw = 380, ph = 150, px = (W - pw) / 2, py = (H - ph) / 2;
        c.fill(px + 8, py + 8, pw, ph, 0xFF111111);
        frame(c, px, py, pw, ph, FW_BLUE, FW_GRAY);
        c.text_centered(px, py + 14, pw, "Please select boot device:", FW_WHITE, 1, true);
        for (int i = 0; i < 4; i++) {
            int yy = py + 42 + i * 22;
            if (i == sel) c.fill(px + 20, yy - 4, pw - 40, 20, FW_GRAY);
            c.text(px + 34, yy, opts[i], i == sel ? FW_BLUE : FW_WHITE, 1, true);
        }
        c.text_centered(px, py + ph - 20, pw, G_UP " and " G_DOWN " to move selection, Enter to boot", FW_GRAY);
        fb_present();
        KeyEvent e;
        while (!poll_key(e)) cpu_idle();
        if (e.key == K_UP) sel = (sel + 3) % 4;
        if (e.key == K_DOWN) sel = (sel + 1) % 4;
        if (e.key == K_ESC) return settings.boot_target ? 1 : 0;
        if (e.key == K_ENTER) {
            if (sel == 3) power_reboot();
            return sel;
        }
    }
}

int firmware_boot() {
    bool setup = false, menu = false;
    post_screen(&setup, &menu);
    int target = settings.boot_target ? 1 : 0;
    for (;;) {
        if (setup) { firmware_setup(); target = settings.boot_target ? 1 : 0; break; }
        if (menu) {
            int r = boot_menu();
            if (r == 2) { setup = true; menu = false; continue; }
            target = r;
        }
        break;
    }
    return target;
}
