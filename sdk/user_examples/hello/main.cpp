// A user program: its own process, a window, events and drawing through the system call interface.
#include "silicon_user.h"

using namespace si;

static void itoa_(long v, char* out) {
    char tmp[24];
    int n = 0;
    bool neg = v < 0;
    if (neg) v = -v;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int o = 0;
    if (neg) out[o++] = '-';
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
}

static void cat(char* d, const char* s) { while (*d) d++; while ((*d++ = *s++)) {} }

extern "C" int app_main() {
    const int W = 380, H = 260;
    u32* px = win_open(W, H, "Hello from ring 3");
    if (!px) return 1;
    int clicks = 0;
    float bx = 60, by = 90, vx = 2.4f, vy = 1.7f;
    char count_file[] = "/home/apps/hello-user.count";
    char buf[16] = {0};
    int n = file_read(count_file, buf, 15);
    for (int i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) clicks = clicks * 10 + (buf[i] - '0');
    bool running = true;
    u64 t0 = ticks();
    while (running) {
        Event e;
        while (event(&e)) {
            if (e.type == EV_CLOSE) running = false;
            if (e.type == EV_MOUSE && (e.d & 0xFF) == MOUSE_DOWN && e.c == 1) {
                clicks++;
                char num[16];
                itoa_(clicks, num);
                file_write(count_file, num, (int)strlen(num));
                notify("Hello from ring 3", "clicks are saved to /home/apps");
            }
            if (e.type == EV_KEY && e.c && e.a == KEY_ESC) running = false;
        }
        bx += vx; by += vy;
        if (bx < 12 || bx > W - 12) vx = -vx;
        if (by < 70 || by > H - 40) vy = -vy;
        clear(0xF5F6F8);
        fill(0, 0, W, 56, 0x3478F6);
        text(16, 12, "Hello from ring 3", 0xFFFFFF, TEXT_BOLD, 2);
        char line[64] = "process ";
        char num[24];
        itoa_(pid(), num);
        cat(line, num);
        cat(line, "   uptime ");
        itoa_((long)((ticks() - t0) / 1000), num);
        cat(line, num);
        cat(line, " s");
        text(16, 64, line, 0x667085);
        disc((int)bx, (int)by, 11, 0xE0508A);
        char c[40] = "clicks: ";
        itoa_(clicks, num);
        cat(c, num);
        rrect(16, H - 34, 130, 24, 8, 0xE6E9EF);
        text(26, H - 30, c, 0x1C1F26, TEXT_BOLD);
        present();
        sleep(16);
    }
    return 0;
}
