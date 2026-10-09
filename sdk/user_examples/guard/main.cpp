// Memory protection test: each button does something a ring 3 program must not be able to do.
// The kernel stops only this process; the desktop and every other program carry on.
#include "silicon_user.h"

using namespace si;

struct Btn { int x, y, w, h; const char* label; };
static const Btn btns[] = {
    {16, 70, 388, 30, "write to kernel memory (0x100000)"},
    {16, 108, 388, 30, "read the kernel's heap (0x20000000)"},
    {16, 146, 388, 30, "jump to address zero"},
    {16, 184, 388, 30, "execute a privileged instruction (cli)"},
    {16, 222, 388, 30, "spin forever (preemption keeps the desktop alive)"},
};

static void boom(int which) {
    switch (which) {
    case 0: *(volatile int*)0x100000 = 0xDEAD; break;
    case 1: { volatile int v = *(volatile int*)0x20000000; (void)v; break; }
    case 2: ((void (*)())0)(); break;
    case 3: __asm__ volatile("cli"); break;
    case 4: for (;;) {} break;
    }
}

extern "C" int app_main() {
    const int W = 420, H = 270;
    if (!win_open(W, H, "Protection test")) return 1;
    int hot = -1;
    bool running = true;
    while (running) {
        Event e;
        while (event(&e)) {
            if (e.type == EV_CLOSE) running = false;
            if (e.type == EV_KEY && e.c && e.a >= '1' && e.a <= '5') boom(e.a - '1');      // keys 1-5 do the same as the buttons
            if (e.type == EV_MOUSE) {
                hot = -1;
                for (int i = 0; i < 5; i++)
                    if (e.a >= btns[i].x && e.a < btns[i].x + btns[i].w && e.b >= btns[i].y && e.b < btns[i].y + btns[i].h) hot = i;
                if ((e.d & 0xFF) == MOUSE_DOWN && e.c == 1 && hot >= 0) boom(hot);
            }
        }
        clear(0xF5F6F8);
        fill(0, 0, W, 48, 0xE5484D);
        text(16, 10, "Try to break out", 0xFFFFFF, TEXT_BOLD, 2);
        text(16, 52, "every button is a bug the kernel has to survive", 0x667085);
        for (int i = 0; i < 5; i++) {
            rrect(btns[i].x, btns[i].y, btns[i].w, btns[i].h, 6, i == hot ? 0xFFD9DB : 0xFFFFFF);
            frame(btns[i].x, btns[i].y, btns[i].w, btns[i].h, 0xC8CCD5);
            text(btns[i].x + 12, btns[i].y + 8, btns[i].label, 0x1C1F26);
        }
        present();
        sleep(16);
    }
    return 0;
}
