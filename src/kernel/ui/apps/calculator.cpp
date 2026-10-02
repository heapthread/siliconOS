// silicon - Calculator with a paper tape
#include "apps.h"

struct CalcApp : App {
    double acc = 0;
    char entry[24] = "0";
    char op = 0;
    bool fresh = true, error = false;
    char expr[64] = "";
    char tape[8][48];
    int tape_n = 0;
    int pressed = -1;
    u64 pressed_t = 0;

    const char* app_id() override { return "calculator"; }
    const char* title() override { return "Calculator"; }
    int pref_w() override { return 420; }
    int pref_h() override { return 330; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF2A2340; }

    static constexpr const char* keys[20] = {"C", "+/-", "%", "/", "7", "8", "9", "*", "4", "5", "6", "-",
                                             "1", "2", "3", "+", "0", ".", G_LEFT, "="};
    Rect key_rect(int i) { return {12 + (i % 4) * 62, 98 + (i / 4) * 44, 56, 38}; }

    double value() {
        // parse entry
        double v = 0, frac = 0.1;
        bool neg = false, dot = false;
        for (const char* p = entry; *p; p++) {
            if (*p == '-') neg = true;
            else if (*p == '.') dot = true;
            else if (*p >= '0' && *p <= '9') {
                if (!dot) v = v * 10 + (*p - '0');
                else { v += (*p - '0') * frac; frac /= 10; }
            }
        }
        return neg ? -v : v;
    }
    void show(double v) {
        if (error || v != v || v > 1e15 || v < -1e15) { strlcpy(entry, "error", sizeof(entry)); error = true; return; }
        fmt_double(v, entry, sizeof(entry), 8);
    }
    double apply(double a, char o, double b) {
        switch (o) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': if (b == 0) { error = true; return 0; } return a / b;
        }
        return b;
    }
    void add_tape(const char* s) {
        if (tape_n == 8) { for (int i = 1; i < 8; i++) strlcpy(tape[i - 1], tape[i], 48); tape_n--; }
        strlcpy(tape[tape_n++], s, 48);
    }
    void press(const char* k) {
        if (error && strcmp(k, "C")) { return; }
        if (k[0] >= '0' && k[0] <= '9' && !k[1]) {
            if (fresh || !strcmp(entry, "0")) { entry[0] = 0; fresh = false; }
            if (strlen(entry) < 15) { char s[2] = {k[0], 0}; strlcat(entry, s, sizeof(entry)); }
        } else if (!strcmp(k, ".")) {
            if (fresh) { strlcpy(entry, "0", sizeof(entry)); fresh = false; }
            if (!strchr(entry, '.')) strlcat(entry, ".", sizeof(entry));
        } else if (!strcmp(k, "C")) {
            acc = 0; op = 0; strlcpy(entry, "0", sizeof(entry)); fresh = true; error = false; expr[0] = 0;
        } else if (!strcmp(k, "+/-")) {
            show(-value());
        } else if (!strcmp(k, "%")) {
            show(op ? acc * value() / 100 : value() / 100);
        } else if (!strcmp(k, G_LEFT)) {
            if (!fresh) { int l = (int)strlen(entry); if (l > 1) entry[l - 1] = 0; else strlcpy(entry, "0", sizeof(entry)); }
        } else if (!strcmp(k, "=")) {
            if (op) {
                double b = value();
                double r = apply(acc, op, b);
                char a1[24], b1[24], line[48];
                fmt_double(acc, a1, sizeof(a1));
                fmt_double(b, b1, sizeof(b1));
                show(r);
                snprintf(line, sizeof(line), "%s %c %s = %s", a1, op, b1, entry);
                add_tape(line);
                snprintf(expr, sizeof(expr), "%s %c %s =", a1, op, b1);
                op = 0;
                acc = r;
            }
            fresh = true;
        } else {
            // operator
            if (op && !fresh) { acc = apply(acc, op, value()); show(acc); }
            else if (!op) acc = value();
            op = k[0];
            char a1[24];
            fmt_double(acc, a1, sizeof(a1));
            snprintf(expr, sizeof(expr), "%s %c", a1, op);
            fresh = true;
        }
        ui_invalidate();
    }

    void draw(Canvas& c, int w, int h) override {
        // display
        c.fill(12, 12, 242, 76, 0xFF1A2E2C);
        c.rect(12, 12, 242, 76, th::ink);
        for (int y = 14; y < 86; y += 2) c.hline(13, y, 240, 0xFF1D3331);
        c.text(250 - text_width(expr) - 6, 20, expr, 0xFF6FBFAF);
        int scale = strlen(entry) > 9 ? 2 : 3;
        c.text(250 - text_width(entry, scale, true) - 4, 84 - FONT_H * scale, entry, error ? th::red : 0xFFAAF5E0, scale, true);
        for (int i = 0; i < 20; i++) {
            Rect r = key_rect(i);
            bool is_op = i % 4 == 3, is_fn = i < 3;
            u32 face = i == 19 ? accent() : is_op ? 0xFFFFA850 : is_fn ? 0xFF8C84A4 : 0xFF4A4262;
            bool down = (pressed == i && ticks() - pressed_t < 120) || (hot(r.x, r.y, r.w, r.h) && ui_mouse_left);
            if (hot(r.x, r.y, r.w, r.h)) face = blend(face, th::white, 40);
            draw_bevel(c, r.x, r.y, r.w, r.h, face, down);
            const char* lbl = keys[i];
            const char* shown = !strcmp(lbl, "*") ? G_TIMES : lbl;
            c.text_centered(r.x + (down ? 1 : 0), r.y + 7 + (down ? 1 : 0), r.w, shown, th::white, 2, true);
        }
        // tape
        c.fill(264, 12, w - 276, h - 24, 0xFFF6F1E4);
        c.rect(264, 12, w - 276, h - 24, th::ink);
        for (int y = 30; y < h - 14; y += 16) c.hline(266, y, w - 280, 0xFFE8E0CC);
        c.text(272, 16, "tape", 0xFFAA9C80, 1, true);
        for (int i = 0; i < tape_n; i++) {
            const char* t = tape[i];
            const char* eq = strchr(t, '=');
            c.text_n(270, 36 + i * 32, t, eq ? (int)(eq - t) : 17, 0xFF6A5E48);
            if (eq) c.text(w - 18 - text_width(eq), 36 + i * 32 + 13, eq, 0xFF2A2433, 1, true);
        }
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_UP && e.button == 1)
            for (int i = 0; i < 20; i++) if (key_rect(i).contains(e.x, e.y)) { pressed = i; pressed_t = ticks(); press(keys[i]); }
        ui_invalidate();
    }
    void on_key(const KeyEvent& e) override {
        const char* k = nullptr;
        char d[2] = {e.ch, 0};
        if (e.ch >= '0' && e.ch <= '9') k = d;
        else if (e.ch == '+' || e.ch == '-' || e.ch == '*' || e.ch == '/' || e.ch == '.' || e.ch == '%') k = d;
        else if (e.key == K_ENTER || e.ch == '=') k = "=";
        else if (e.key == K_BACKSPACE) k = G_LEFT;
        else if (e.key == K_ESC || e.ch == 'c') k = "C";
        if (!k) return;
        for (int i = 0; i < 20; i++) if (!strcmp(keys[i], k)) { pressed = i; pressed_t = ticks(); }
        press(k);
    }
    void tick() override { if (pressed >= 0 && ticks() - pressed_t < 200) ui_invalidate(); }
};

App* new_calculator(const char*) { return new CalcApp(); }
