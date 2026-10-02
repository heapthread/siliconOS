// silicon - About
#include "apps.h"
#include "../../arch/arch.h"

struct AboutApp : App {
    u64 last = 0;
    const char* app_id() override { return "about"; }
    const char* title() override { return "About silicon"; }
    int pref_w() override { return 460; }
    int pref_h() override { return 300; }
    bool resizable() override { return false; }

    void draw(Canvas& c, int w, int h) override {
        c.gradient_v(0, 0, w, 120, 0xFF2A2140, 0xFF4A3470, 6);
        for (int i = 0; i < 40; i++) c.put((i * 97) % w, (i * 53) % 100 + 4, i % 3 ? 0xFF8070B0 : 0xFFFFFFFF);
        c.blit_scaled(icon("silicon")->big, 24, 12, 96, 96);
        c.text(136, 30, "silicon", th::white, 4, true);
        c.text(140, 84, "version " SILICON_VERSION "  " G_BULLET "  x86_64", 0xFFD8CCF4);
        int y = 136;
        char b[128];
        auto row = [&](const char* k, const char* v) {
            c.text(28, y, k, th::muted, 1, true);
            c.text(130, y, v, th::text);
            y += 18;
        };
        snprintf(b, sizeof(b), "%.40s", cpu_brand());
        row("processor", b);
        snprintf(b, sizeof(b), "%u MB", (u32)(mem_total_bytes >> 20));
        row("memory", b);
        snprintf(b, sizeof(b), "%dx%d, 32-bit color", fb.width, fb.height);
        row("display", b);
        char used[24];
        fmt_bytes(fs_disk_used(), used, sizeof(used));
        snprintf(b, sizeof(b), "SiliconFS, %s used%s", used, fs_persistent() ? "" : " (RAM only)");
        row("storage", b);
        u64 s = ticks() / 1000;
        snprintf(b, sizeof(b), "%uh %um %us", (u32)(s / 3600), (u32)(s / 60 % 60), (u32)(s % 60));
        row("uptime", b);
        c.text(28, h - 26, "made from scratch: bootloader, kernel, pixels.", th::muted);
    }
    void tick() override {
        if (ticks() - last > 1000) { last = ticks(); ui_invalidate(); }
    }
};
App* new_about(const char*) { return new AboutApp(); }
