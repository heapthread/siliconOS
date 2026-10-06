// A small silicon app: a bouncing ball, a click counter that survives restarts, and a look into /home.
#include "silicon.h"

static const char* COUNT_FILE = "/home/apps/hello.count";

static int clicks = 0;
static float bx = 60, by = 20, vx = 2.2f, vy = 1.6f;
static int width = 420, height = 300;
static char names[10][40];
static int kinds[10], nnames = 0;

static void itoa_(int v, char* out) {
    char tmp[16];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    bool neg = v < 0;
    if (neg) v = -v;
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int o = 0;
    if (neg) out[o++] = '-';
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
}

static void load() {
    char buf[16] = {0};
    int n = si->file_read(COUNT_FILE, buf, 15);
    clicks = 0;
    for (int i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) clicks = clicks * 10 + (buf[i] - '0');
    nnames = 0;
    while (nnames < 10) {
        int k = si->dir_entry("/home", nnames, names[nnames], 40);
        if (!k) break;
        kinds[nnames++] = k;
    }
}

static void draw(SiCanvas* c, int w, int h) {
    width = w; height = h;
    for (int y = 0; y < 34; y++) si->fill(c, 0, y, w, 1, 0xFF000000 | ((60 + y * 3) << 16) | ((40 + y) << 8) | 200);
    si->text(c, 12, 9, "Hello from the silicon SDK", 0xFFFFFFFF, 1, 1);
    char b[48] = "clicks: ";
    itoa_(clicks, b + 8);
    si->text(c, 12, 48, b, 0xFF222244, 2, 1);
    si->text(c, 12, 74, "click below to count (saved in /home/apps/hello.count)", 0xFF666688, 1, 0);
    si->circle(c, (int)bx, (int)by + 100, 14, 0xFFE5486A, 1);
    si->circle(c, (int)bx - 4, (int)by + 96, 4, 0xFFFFB0C0, 1);
    si->text(c, 12, h - 16 - 12 * nnames, "in /home:", 0xFF444466, 1, 1);
    for (int i = 0; i < nnames; i++) {
        char line[48] = "";
        const char* s = names[i];
        int o = 0;
        line[o++] = kinds[i] == 2 ? '/' : ' ';
        for (int k = 0; s[k] && o < 40; k++) line[o++] = s[k];
        line[o] = 0;
        si->text(c, 24, h - 14 - 12 * (nnames - 1 - i), line, kinds[i] == 2 ? 0xFF2255AA : 0xFF333333, 1, 0);
    }
}

static void mouse(const SiMouse* m) {
    if (m->kind == SI_MOUSE_UP && m->button == 1 && m->y > 90) {
        clicks++;
        char b[16];
        itoa_(clicks, b);
        int n = 0;
        while (b[n]) n++;
        si->file_write(COUNT_FILE, b, n);
        si->beep(0);
        si->invalidate();
    }
}

static void tick() {
    bx += vx; by += vy;
    if (bx < 20 || bx > width - 20) vx = -vx;
    if (by < 0 || by > height - 190) vy = -vy;
    si->invalidate();
}

extern "C" int silicon_app(SiApp* app) {
    const char* t = "Hello (SDK)";
    for (int i = 0; t[i]; i++) { app->title[i] = t[i]; app->title[i + 1] = 0; }
    app->width = width; app->height = height;
    app->background = 0xFFF4F1FA;
    app->draw = draw;
    app->mouse = mouse;
    app->tick = tick;
    load();
    si->log("hello module started");
    return 0;
}
