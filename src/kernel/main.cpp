// silicon - kernel main: boot, full-screen terminal, and the hand-off to the desktop
#include "core.h"
#include "bootinfo.h"
#include "settings.h"
#include "arch/arch.h"
#include "drv/drivers.h"
#include "fs/fs.h"
#include "term/shell.h"
#include "ui/ui.h"
#include "net/net.h"
#include "firmware.h"

extern "C" char __kernel_end[];
u64 mem_total_bytes = 0;

static void memory_init(BootInfo* bi) {
    u64 kend = ((u64)__kernel_end + 0xFFFFF) & ~0xFFFFFull;
    u64 best_start = 0, best_end = 0;
    for (u32 i = 0; i < bi->e820_count; i++) {
        E820Entry& e = bi->e820[i];
        if (e.type != 1) continue;
        mem_total_bytes += e.length;
        u64 s = e.base, en = e.base + e.length;
        if (en > 0xFFFFFFFFull) en = 0xFFFFFFFFull;    // identity mapped range only
        if (s < kend) s = kend;
        if (en > s && en - s > best_end - best_start) { best_start = s; best_end = en; }
    }
    if (!best_end) panic("no usable memory above the kernel");
    heap_init(best_start, best_end);
    klog("memory: %u MiB total, heap %lx..%lx\n", (u32)(mem_total_bytes >> 20), best_start, best_end);
}

// ---------------------------------------------------------------- boot splash
static void splash_frame(int pct, const char* what) {
    Canvas& c = fb.back;
    int W = fb.width, H = fb.height;
    c.gradient_v(0, 0, W, H, rgb(18, 12, 30), rgb(46, 26, 70), 12);
    // stars
    srand(7);
    for (int i = 0; i < 90; i++) {
        int x = rand() % W, y = rand() % (H * 2 / 3);
        c.put(x, y, i % 7 == 0 ? rgb(255, 230, 250) : rgb(120, 100, 160));
    }
    srand((u32)ticks() | 1);
    int cx = W / 2, cy = H / 2 - 40;
    // big pixel chip: body, pins, die
    int s = 8;
    int bw = 12 * s, bh = 12 * s;
    int bx = cx - bw / 2, by = cy - bh / 2;
    for (int i = 0; i < 5; i++) {
        int off = (3 + i * 2) * s - s / 2;           // 5 pins centred on each side
        c.fill(bx + off, by - 2 * s, s, 2 * s, rgb(230, 180, 90));
        c.fill(bx + off, by + bh, s, 2 * s, rgb(230, 180, 90));
        c.fill(bx - 2 * s, by + off, 2 * s, s, rgb(230, 180, 90));
        c.fill(bx + bw, by + off, 2 * s, s, rgb(230, 180, 90));
    }
    c.fill(bx, by, bw, bh, rgb(40, 34, 58));
    c.rect(bx, by, bw, bh, rgb(90, 80, 120));
    c.fill(bx + 3 * s, by + 3 * s, 6 * s, 6 * s, rgb(150, 120, 255));
    c.fill(bx + 4 * s, by + 4 * s, 4 * s, 4 * s, rgb(200, 180, 255));
    c.fill(bx + s, by + s, s, s, rgb(255, 120, 150));   // pin-1 marker
    c.text_centered(0, cy + bh / 2 + 40, W, "silicon", rgb(240, 232, 255), 4, true);
    // progress bar made of pixel "dies"
    int segs = 24, sw = 12, gap = 4;
    int total = segs * (sw + gap) - gap;
    int x0 = cx - total / 2, y0 = cy + bh / 2 + 110;
    for (int i = 0; i < segs; i++) {
        bool on = i * 100 / segs < pct;
        c.fill(x0 + i * (sw + gap), y0, sw, 8, on ? rgb(170, 140, 255) : rgb(60, 50, 86));
    }
    c.text_centered(0, y0 + 22, W, what, rgb(140, 125, 175));
    fb_present();
}

static void splash(const char* what, int pct) {
    splash_frame(pct, what);
}

// ---------------------------------------------------------------- full-screen terminal
static Console con;
static Shell shell;
static bool want_ui = false;
static const int HEADER_H = 22;

struct TermHost : ShellHost {
    void shell_request(Shell* sh, ShellRequest r, const char* arg) override {
        if (r == SR_START_UI) want_ui = true;
        else if (r == SR_SETUP) {
            firmware_setup();
            // the firmware may have changed how the terminal looks
            con.theme = clamp(settings.term_theme, 0, console_theme_count - 1);
            if (con.scale != settings.term_scale) {
                con.scale = settings.term_scale;
                int cols = (fb.width - 16) / con.cell_w(), rows = (fb.height - HEADER_H - 8) / con.cell_h();
                con.resize(cols, rows);
                con.clear();
            }
            con.dirty = true;
        }
        else if (r == SR_ZOOM) {
            settings.term_scale = settings.term_scale == 1 ? 2 : 1;
            settings_save();
            con.scale = settings.term_scale;
            int cols = (fb.width - 16) / con.cell_w(), rows = (fb.height - HEADER_H - 8) / con.cell_h();
            con.resize(cols, rows);
            con.clear();
        } else if (r == SR_EXIT) {
            sh->out("this is the main terminal; use 'shutdown' or 'reboot'\n");
        } else if (r == SR_WALLPAPER) {
            ui_wallpaper_command(sh, arg);
        }
    }
    bool host_is_ui() override { return false; }
};
static TermHost term_host;

static void draw_terminal() {
    Canvas& c = fb.back;
    u32 bg = con.color(0);
    c.fill(0, 0, fb.width, fb.height, bg);
    // header strip
    u32 hb = shade(bg, 160);
    c.fill(0, 0, fb.width, HEADER_H, hb);
    c.hline(0, HEADER_H - 1, fb.width, shade(bg, 220));
    c.text(8, 5, G_CHIP " silicon", accent(), 1, true);
    c.text(90, 5, "terminal", con.color(8));
    DateTime d;
    rtc_read(d);
    char t[64];
    snprintf(t, sizeof(t), "%s %d %s  %02d:%02d", weekday_name(d.weekday), d.day, month_name(d.month), d.hour, d.minute);
    c.text(fb.width - text_width(t) - 10, 5, t, con.color(7));
    const char* hint = "type 'ui' for the desktop";
    c.text_centered(0, 5, fb.width, hint, con.color(8));
    int cw = con.cols * con.cell_w(), ch = con.rows * con.cell_h();
    con.render(c, (fb.width - cw) / 2, HEADER_H + 4 + (fb.height - HEADER_H - 4 - ch) / 2);
    fb_present();
}

static void terminal_loop() {
    u64 last_draw = 0;
    int last_min = -1;
    for (;;) {
        KeyEvent e;
        bool any = false;
        while (kbd_poll(e)) { shell.on_key(e); any = true; }
        net_poll();
        shell.tick();
        fs_tick();
        if (want_ui) {
            want_ui = false;
            fs_sync();
            ui_run();
            con.theme = settings.term_theme;
            con.dirty = true;
        }
        DateTime d;
        if (ticks() - last_draw > 250) { rtc_read(d); if (d.minute != last_min) { last_min = d.minute; con.dirty = true; } }
        bool blink = ticks() - last_draw > 265;
        if (any || con.dirty || blink) {
            draw_terminal();
            last_draw = ticks();
        }
        if (shell.busy() || net_busy()) cpu_rest();
        else cpu_idle();
    }
}

extern "C" void kmain(BootInfo* bi) {
    serial_init();
    klog("\nsilicon " SILICON_VERSION " booting\n");
    gdt_init();
    idt_init();
    memory_init(bi);
    fb_init(bi);
    klog("framebuffer %ux%u pitch %u @ %lx\n", bi->fb_width, bi->fb_height, bi->fb_pitch, bi->fb_addr);
    pit_init(1000);
    sti();
    kbd_init();
    mouse_init(fb.width, fb.height);
    fs_init();
    settings_load();
    int boot_target = firmware_boot();          // POST, setup (DEL/F2), boot menu (F12)
    splash("waking up the silicon", 30);
    splash("bringing up the network", 50);
    net_init();
    splash("unpacking pixels", 60);
    ui_preload();
    for (int pct = 60; pct <= 100; pct += settings.fast_boot ? 20 : 4) {     // let the chip finish warming up
        splash(pct < 100 ? "unpacking pixels" : "ready", pct);
        if (!settings.fast_boot) sleep_ms(30);
    }
    srand((u32)rtc_stamp());
    if (!settings.fast_boot) sleep_ms(300);

    con.scale = settings.term_scale;
    con.theme = clamp(settings.term_theme, 0, console_theme_count - 1);
    int cols = (fb.width - 16) / con.cell_w(), rows = (fb.height - HEADER_H - 8) / con.cell_h();
    con.init(cols, rows);
    shell.init(&con, &term_host);
    shell.banner();
    shell.run_line("fetch");
    shell.start_prompt();
    if (boot_target == 1) want_ui = true;
    terminal_loop();
}
