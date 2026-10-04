// silicon - Emoji: a picker. Click one and it's on the clipboard, ready to paste anywhere.
#include "apps.h"
#include "../../text.h"

struct EmojiApp : App {
    TextField q;
    Vec<int> shown;
    Smooth sc;
    int sel = 0;
    int cw = 0, ch = 0;
    char status[96] = "pick an emoji: it is copied to the clipboard";
    u64 status_t = 0;
    int recent[16];
    int nrecent = 0;
    ScrollBar sb;

    EmojiApp() { q.focused = true; refresh(); }
    ~EmojiApp() { shown.free(); }
    const char* app_id() override { return "emoji"; }
    const char* title() override { return "Emoji"; }
    int pref_w() override { return 452; }
    int pref_h() override { return 430; }
    void on_resize(int w, int h) override { cw = w; ch = h; }
    bool wants_ibeam(int x, int y) override { return y > 8 && y < 34 && x > 40; }

    static const int HEAD_H = 44, FOOT_H = 26, CELL = 40;
    int cols() { return max(1, (cw - 16) / CELL); }
    Rect cell(int i, bool with_recent) {
        int rows_recent = with_recent && nrecent ? 1 : 0;
        (void)rows_recent;
        int c = cols();
        return {8 + (i % c) * CELL, HEAD_H + 6 + (i / c) * CELL - sc.pos() + (nrecent ? 34 : 0), CELL, CELL};
    }
    int content_h() { return (((shown.len + cols() - 1) / cols()) * CELL) + 12 + (nrecent ? 34 : 0); }

    void refresh() {
        shown.clear();
        // all words of the query must appear in the name
        for (int i = 1; i <= emoji_count(); i++) {
            bool ok = true;
            const char* p = q.text;
            while (*p && ok) {
                while (*p == ' ') p++;
                char w[40];
                int n = 0;
                while (*p && *p != ' ' && n < 39) w[n++] = *p++;
                w[n] = 0;
                if (n && !str_icontains(emoji_name(i), w)) ok = false;
            }
            if (ok) shown.push(i);
        }
        sel = clamp(sel, 0, max(0, shown.len - 1));
        sc.jump(0);
    }

    void pick(int idx) {
        u32 cp = emoji_codepoint(idx);
        char u[8];
        int n = utf8_encode(cp, u);
        u[n] = 0;
        clip_set(u);
        snprintf(status, sizeof(status), "copied %s " G_BULLET " U+%X " G_BULLET " ctrl+v pastes it", emoji_name(idx), cp);
        status_t = ticks();
        // most recent first
        int pos = -1;
        for (int i = 0; i < nrecent; i++) if (recent[i] == idx) pos = i;
        if (pos < 0) { pos = nrecent < 16 ? nrecent++ : 15; }
        for (int i = pos; i > 0; i--) recent[i] = recent[i - 1];
        recent[0] = idx;
        ui_beep(0);
    }

    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        c.fill(0, 0, w, HEAD_H, th::panel);
        c.hline(0, HEAD_H - 1, w, th::border);
        c.text(10, 14, "search", th::muted, 1, true);
        q.focused = true;
        q.draw(c, 66, 10, w - 76);
        c.set_clip({0, HEAD_H, w - 11, h - HEAD_H - FOOT_H});
        int y = HEAD_H + 8 - sc.pos();
        if (nrecent) {
            c.text(10, y, "recent", th::muted, 1, true);
            for (int i = 0; i < nrecent; i++) {
                if (const Image* s = emoji_sprite(recent[i], 16)) {
                    Rect r{8 + i * 30 + 52, y - 6, 28, 26};
                    bool hv = hot(r.x, r.y, r.w, r.h);
                    if (hv) c.fill(r.x, r.y, r.w, r.h, blend(th::paper, accent(), 80));
                    c.blit(*s, r.x + 6, r.y + 5);
                }
            }
        }
        for (int i = 0; i < shown.len; i++) {
            Rect r = cell(i, true);
            if (r.y + r.h < HEAD_H || r.y > h - FOOT_H) continue;
            bool hv = hot(r.x, r.y, r.w, r.h);
            if (i == sel) c.fill(r.x + 1, r.y + 1, r.w - 2, r.h - 2, blend(th::paper, accent(), 120));
            else if (hv) c.fill(r.x + 1, r.y + 1, r.w - 2, r.h - 2, blend(th::paper, accent(), 60));
            if (const Image* s = emoji_sprite(shown[i], 16)) c.blit_scaled(*s, r.x + 4, r.y + 4, 32, 32);
        }
        c.set_clip({0, 0, w, h});
        sb.draw(c, w - 10, HEAD_H, h - HEAD_H - FOOT_H, content_h(), h - HEAD_H - FOOT_H, sc.pos());
        c.fill(0, h - FOOT_H, w, FOOT_H, th::panel);
        c.hline(0, h - FOOT_H, w, th::border);
        // hovered emoji name, or the last status
        int hov = -1;
        for (int i = 0; i < shown.len; i++) { Rect r = cell(i, true); if (r.y + r.h > HEAD_H && r.y < h - FOOT_H && hot(r.x, r.y, r.w, r.h)) { hov = i; break; } }
        char b[120];
        if (status_t && ticks() - status_t < 4000) strlcpy(b, status, sizeof(b));
        else if (hov >= 0) snprintf(b, sizeof(b), "%s  " G_BULLET "  U+%X", emoji_name(shown[hov]), emoji_codepoint(shown[hov]));
        else snprintf(b, sizeof(b), "%d emoji", shown.len);
        c.text(10, h - FOOT_H + 8, b, th::text);
    }

    void tick() override {
        if (sc.step()) ui_invalidate();
        static u64 last = 0;
        if (ticks() - last > 400) { last = ticks(); ui_invalidate(); }
    }

    void on_mouse(const MouseEv& e) override {
        int off = sc.target;
        int vh = ch - HEAD_H - FOOT_H;
        if (sb.handle(e, cw - 10, HEAD_H, vh, content_h(), vh, off)) { sc.jump(off); ui_invalidate(); return; }
        if (e.kind == M_WHEEL) { sc.set(clamp(sc.target - e.wheel * 48, 0, max(0, content_h() - vh))); ui_invalidate(); return; }
        if (e.y < HEAD_H) {
            if (e.kind == M_DOWN) q.click(e.x - 66, cw - 76, kbd_shift_down());
            else if (e.kind == M_MOVE && e.left) q.drag(e.x - 66, cw - 76);
            ui_invalidate();
            return;
        }
        if (e.kind == M_UP && e.button == 1) {
            if (nrecent) {
                int y = HEAD_H + 8 - sc.pos() - 6;
                for (int i = 0; i < nrecent; i++)
                    if (Rect{8 + i * 30 + 52, y, 28, 26}.contains(e.x, e.y)) { pick(recent[i]); ui_invalidate(); return; }
            }
            for (int i = 0; i < shown.len; i++) {
                Rect r = cell(i, true);
                if (r.contains(e.x, e.y) && e.y > HEAD_H && e.y < ch - FOOT_H) { sel = i; pick(shown[i]); break; }
            }
        }
        ui_invalidate();
    }

    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        int c = cols();
        if (e.key == K_ENTER) { if (shown.len) pick(shown[sel]); }
        else if (e.key == K_LEFT && !e.ctrl && sel > 0 && q.cur == q.len) sel--;
        else if (e.key == K_RIGHT && !e.ctrl && sel + 1 < shown.len && q.cur == q.len) sel++;
        else if (e.key == K_UP) sel = max(0, sel - c);
        else if (e.key == K_DOWN) sel = min(shown.len - 1, sel + c);
        else if (q.on_key(e)) { refresh(); }
        // keep the selection in view
        Rect r = cell(sel, true);
        int vh = ch - HEAD_H - FOOT_H;
        if (r.y < HEAD_H) sc.set(max(0, sc.target + r.y - HEAD_H - 6));
        else if (r.y + r.h > HEAD_H + vh) sc.set(sc.target + r.y + r.h - HEAD_H - vh + 6);
        ui_invalidate();
    }
};

App* new_emoji(const char*) { return new EmojiApp(); }
