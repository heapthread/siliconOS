// Threads and preemption: four worker threads spin as fast as they can while the window keeps
// answering the mouse, because the scheduler takes the cpu back from them on every timer tick.
#include "silicon_user.h"

using namespace si;

static volatile unsigned long long counters[4];
static volatile int stop = 0;

static void worker(void* arg) {
    int id = (int)(unsigned long long)arg;
    while (!stop) counters[id] = counters[id] + 1;
}

static void itoa_(unsigned long long v, char* out) {
    char tmp[24];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int o = 0;
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
}

extern "C" int app_main() {
    const int W = 420, H = 300;
    if (!win_open(W, H, "Threads")) return 1;
    for (int i = 0; i < 4; i++) thread(worker, (void*)(unsigned long long)i);
    int mx = 0, my = 0, frames = 0;
    unsigned long long last[4] = {0, 0, 0, 0};
    unsigned long long rate[4] = {0, 0, 0, 0};
    u64 t_rate = ticks();
    bool running = true;
    while (running) {
        Event e;
        while (event(&e)) {
            if (e.type == EV_CLOSE) running = false;
            if (e.type == EV_MOUSE) { mx = e.a; my = e.b; }
        }
        u64 now = ticks();
        if (now - t_rate >= 1000) {
            for (int i = 0; i < 4; i++) { rate[i] = (counters[i] - last[i]) * 1000 / (now - t_rate); last[i] = counters[i]; }
            t_rate = now;
        }
        clear(0xF5F6F8);
        fill(0, 0, W, 48, 0x7C5CFA);
        text(16, 10, "4 threads, all busy", 0xFFFFFF, TEXT_BOLD, 2);
        for (int i = 0; i < 4; i++) {
            char num[32], line[64] = "thread ";
            line[7] = (char)('1' + i);
            line[8] = 0;
            text(16, 66 + i * 34, line, 0x1C1F26, TEXT_BOLD);
            itoa_(rate[i], num);
            text(100, 66 + i * 34, num, 0x667085);
            text(230, 66 + i * 34, "increments/s", 0x667085);
            unsigned long long w = rate[i] / 400000;
            rrect(16, 82 + i * 34 - 4, 380, 4, 2, 0xDDE1E8);
            rrect(16, 82 + i * 34 - 4, (int)(w > 380 ? 380 : w < 4 ? 4 : w), 4, 2, 0x7C5CFA);
        }
        disc(mx, my < 200 ? 200 : my, 8, 0xE5484D);
        text(16, H - 30, "the dot follows the mouse: the window is still responsive", 0x667085);
        frames++;
        present();
        sleep(16);
    }
    stop = 1;
    return 0;
}
