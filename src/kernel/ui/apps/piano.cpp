// silicon - Piano: two octaves on the PC speaker. Tracker-style keys (z s x d c ... and q 2 w 3 e ...),
// notes float up from the keys, and you can record and play back a tune.
#include "apps.h"

static const int NKEYS = 25;                  // C4 .. C6
static const u16 FREQ[NKEYS] = {262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494,
                                523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988, 1047};
static const char* NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char KEYMAP[] = "zsxdcvgbhnjm,q2w3er5t6y7ui";   // ',' and 'q' are both C5 (note 12)
static bool is_black(int n) { int k = n % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }

static const int WK_W = 40, WK_H = 150, BK_W = 26, BK_H = 92, KB_Y = 150, KB_X = 10;
static const int TOP_H = 40;

struct Trail { int note; u64 t0, t1; };      // t1 = release time (0 while held)
struct RecNote { u32 at; u8 note; };

struct PianoApp : App {
    u64 pressed_t[NKEYS];
    int last_note = -1;
    Vec<Trail> trails;
    Vec<RecNote> rec;
    bool recording = false, playing = false;
    u64 rec_t0 = 0, play_t0 = 0;
    int play_i = 0;
    const RecNote* play_src = nullptr;
    int play_n = 0;
    int mouse_note = -1;

    PianoApp() { memset(pressed_t, 0, sizeof(pressed_t)); }
    ~PianoApp() { trails.free(); rec.free(); }
    const char* app_id() override { return "piano"; }
    const char* title() override { return "Piano"; }
    int pref_w() override { return KB_X * 2 + 15 * WK_W; }
    int pref_h() override { return KB_Y + WK_H + 30; }
    bool resizable() override { return false; }
    u32 background() override { return 0xFF1C1630; }

    int white_index(int n) { int w = 0; for (int i = 0; i < n; i++) if (!is_black(i)) w++; return w; }
    Rect key_rect(int n) {
        int wi = white_index(n);
        if (!is_black(n)) return {KB_X + wi * WK_W, KB_Y, WK_W, WK_H};
        return {KB_X + wi * WK_W - BK_W / 2, KB_Y, BK_W, BK_H};
    }
    int note_at(int x, int y) {
        for (int n = 0; n < NKEYS; n++) if (is_black(n) && key_rect(n).contains(x, y)) return n;
        for (int n = 0; n < NKEYS; n++) if (!is_black(n) && key_rect(n).contains(x, y)) return n;
        return -1;
    }
    void play(int n, bool record = true) {
        if (n < 0 || n >= NKEYS) return;
        speaker_play_now(FREQ[n], 220);
        pressed_t[n] = ticks();
        last_note = n;
        trails.push(Trail{n, ticks(), 0});
        if (recording && record) rec.push(RecNote{(u32)(ticks() - rec_t0), (u8)n});
        ui_invalidate();
    }
    void release(int n) {
        for (auto& t : trails) if (t.note == n && !t.t1) t.t1 = ticks();
    }

    void start_play(const RecNote* src, int n) {
        if (!n) return;
        playing = true;
        recording = false;
        play_src = src;
        play_n = n;
        play_i = 0;
        play_t0 = ticks();
    }

    void tick() override {
        u64 t = ticks();
        for (int i = trails.len - 1; i >= 0; i--) {
            Trail& tr = trails[i];
            if (!tr.t1 && t - tr.t0 > 260) tr.t1 = t;     // keyboard notes release on their own
            if (tr.t1 && t - tr.t1 > 1600) trails.remove(i);
        }
        if (playing) {
            while (play_i < play_n && t - play_t0 >= play_src[play_i].at) play(play_src[play_i++].note, false);
            if (play_i >= play_n && t - play_t0 > play_src[play_n - 1].at + 400) playing = false;
        }
        if (trails.len || playing || recording) ui_invalidate();
    }

    void draw(Canvas& c, int w, int h) override {
        u64 t = ticks();
        // header: note name, a square-wave scope and the transport buttons
        c.fill(0, 0, w, TOP_H, 0xFF141024);
        char b[32];
        if (last_note >= 0) snprintf(b, sizeof(b), "%s%d  %u Hz", NAMES[last_note % 12], 4 + last_note / 12, FREQ[last_note]);
        else strlcpy(b, "play with your keyboard", sizeof(b));
        c.text(12, 14, b, 0xFFDCCFFF, 1, true);
        int sx = 220, sw = 160;
        c.fill(sx, 8, sw, 24, 0xFF0C0A16);
        bool sounding = last_note >= 0 && t - pressed_t[last_note] < 220;
        if (sounding) {
            int period = max(4, 4000 / FREQ[last_note]);
            int y_hi = 12, y_lo = 27;
            for (int x = 0; x < sw; x++) {
                bool hi = ((x + (int)(t / 4)) / (period / 2 + 1)) % 2;
                c.put(sx + x, hi ? y_hi : y_lo, 0xFF7FE0A8);
                if (x && (((x + (int)(t / 4)) % (period / 2 + 1)) == 0)) c.vline(sx + x, y_hi, y_lo - y_hi, 0xFF4FBF84);
            }
        } else c.hline(sx, 20, sw, 0xFF2E5A44);
        auto btn = [&](int x, const char* label, bool on) { draw_button(c, x, 9, 64, 22, label, hot(x, 9, 64, 22), on); };
        btn(w - 220, recording ? G_FULL " rec" : "record", recording);
        btn(w - 150, playing ? "stop" : "play", playing);
        btn(w - 80, "demo", false);
        // note trails rising above the keys
        for (auto& tr : trails) {
            Rect k = key_rect(tr.note);
            u64 end = tr.t1 ? tr.t1 : t;
            int top = KB_Y - (int)(t - tr.t0) / 8, bottom = KB_Y - (int)(t - end) / 8;
            top = max(top, TOP_H);
            if (bottom <= TOP_H) continue;
            u32 col = is_black(tr.note) ? 0xFF9B7BFF : 0xFF4FD1C5;
            int fade = tr.t1 ? (int)min<u64>(200, (t - tr.t1) / 8) : 0;
            c.fill(k.x + 6, top, k.w - 12, bottom - top, blend(col, 0xFF1C1630, (u32)fade));
            c.fill(k.x + 6, top, k.w - 12, 2, blend(0xFFFFFFFF, 0xFF1C1630, (u32)fade));
        }
        // keys: whites first, then blacks on top
        for (int pass = 0; pass < 2; pass++)
            for (int n = 0; n < NKEYS; n++) {
                if (is_black(n) != (pass == 1)) continue;
                Rect r = key_rect(n);
                int age = (int)(t - pressed_t[n]);
                bool down = pressed_t[n] && age < 200;
                if (!is_black(n)) {
                    u32 face = down ? blend(0xFFF4F0FA, 0xFF4FD1C5, (u32)(200 - age)) : 0xFFF4F0FA;
                    c.fill(r.x, r.y, r.w - 1, r.h, face);
                    c.fill(r.x, r.y + r.h - (down ? 4 : 8), r.w - 1, down ? 4 : 8, 0xFFB8ACD6);
                    c.vline(r.x + r.w - 1, r.y, r.h, 0xFF4A3E6A);
                    if (n % 12 == 0) { snprintf(b, sizeof(b), "C%d", 4 + n / 12); c.text(r.x + 12, r.y + r.h - 26, b, 0xFF8A7EA8); }
                } else {
                    u32 face = down ? blend(0xFF2A2440, 0xFF9B7BFF, (u32)(220 - min(age, 220))) : 0xFF2A2440;
                    c.fill(r.x, r.y, r.w, r.h + (down ? 0 : 4), 0xFF100C1C);
                    c.fill(r.x + 2, r.y, r.w - 4, r.h - (down ? 2 : 6), face);
                    c.hline(r.x + 4, r.y + r.h - (down ? 4 : 10), r.w - 8, 0xFF443B64);
                }
                // which computer key plays it
                char kc[2] = {KEYMAP[n <= 12 ? n : n + 1], 0};
                if (n == 12) kc[0] = 'q';
                c.text(r.x + r.w / 2 - 4, r.y + (is_black(n) ? r.h - 26 : r.h - 44), kc, is_black(n) ? 0xFF8A7EB8 : 0xFFB8ACD6);
            }
        c.text(12, h - 20, recording ? "recording - press record again to stop" :
                           rec.len ? "press play to hear your tune" : "z s x d c v g b h n j m  /  q 2 w 3 e r 5 t 6 y 7 u i",
               0xFF8A7EB8);
    }

    void on_key(const KeyEvent& e) override {
        if (!e.down || e.ctrl || e.alt) return;
        char ch = e.ch;
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        for (int n = 0; KEYMAP[n]; n++)
            if (KEYMAP[n] == ch && ch) { play(n <= 12 ? n : n - 1); return; }
    }
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_DOWN && e.button == 1) {
            if (e.y < TOP_H) {
                int w = pref_w();
                if (Rect{w - 220, 9, 64, 22}.contains(e.x, e.y)) {
                    recording = !recording;
                    if (recording) { rec.clear(); rec_t0 = ticks(); playing = false; }
                } else if (Rect{w - 150, 9, 64, 22}.contains(e.x, e.y)) {
                    if (playing) playing = false;
                    else start_play(rec.data, rec.len);
                } else if (Rect{w - 80, 9, 64, 22}.contains(e.x, e.y)) demo();
                ui_invalidate();
                return;
            }
            mouse_note = note_at(e.x, e.y);
            play(mouse_note);
        }
        if (e.kind == M_MOVE && e.left) {        // glissando
            int n = note_at(e.x, e.y);
            if (n >= 0 && n != mouse_note) { release(mouse_note); mouse_note = n; play(n); }
        }
        if (e.kind == M_UP) { release(mouse_note); mouse_note = -1; }
    }
    void demo() {
        // Ode to Joy, a beat is 300 ms
        static const u8 tune[] = {4, 4, 5, 7, 7, 5, 4, 2, 0, 0, 2, 4, 4, 2, 2, 255,
                                  4, 4, 5, 7, 7, 5, 4, 2, 0, 0, 2, 4, 2, 0, 0};
        static RecNote notes[sizeof(tune)];
        static int n = 0;
        if (!n) {
            u32 at = 0;
            for (u32 i = 0; i < sizeof(tune); i++) {
                if (tune[i] == 255) { at += 300; continue; }
                notes[n++] = RecNote{at, (u8)(tune[i] + 12)};
                at += (i == 14 || i == 29) ? 600 : 300;
            }
        }
        start_play(notes, n);
    }
};

App* new_piano(const char*) { return new PianoApp(); }
