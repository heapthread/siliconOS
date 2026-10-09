// silicon - Media: plays audio (WAV, MP3, FLAC) and video (AVI, Motion JPEG) through the AC97 sound card
#include "apps.h"
#include "../../media/media.h"
#include "../../drv/audio.h"

struct MediaApp : App {
    Media* m = nullptr;
    u8* buf = nullptr;                // our own copy of the file
    char path[256] = "";
    char err[96] = "";
    bool playing = false;
    u64 base_ms = 0;                  // where in the file the audio stream (re)started
    u64 wall0 = 0;                    // wall clock when playback of a video without sound started
    u64 paused_at = 0;
    Image frame;
    int frame_idx = -1;
    int level_smooth = 0;
    bool dragging_seek = false, dragging_vol = false;
    int cw = 0, ch = 0;
    u8 bars[48];
    u64 last_bar = 0;

    MediaApp(const char* arg) {
        memset(bars, 0, sizeof(bars));
        if (arg && arg[0]) open_file(arg);
    }
    ~MediaApp() { close_file(); }

    const char* app_id() override { return "media"; }
    const char* title() override { return m && m->title[0] ? m->title : "Media"; }
    int pref_w() override { return 640; }
    int pref_h() override { return 460; }

    void close_file() {
        if (m && audio_available()) audio_stop();
        if (m) media_free(m);
        m = nullptr;
        if (buf) kfree(buf);
        buf = nullptr;
        image_free(frame);
        frame_idx = -1;
        playing = false;
    }

    bool open_file(const char* p) {
        FsNode* n = fs_lookup(p);
        if (!n || n->dir) { snprintf(err, sizeof(err), "can't find %s", p); return false; }
        close_file();
        err[0] = 0;
        buf = (u8*)kmalloc_try(n->size + 16);
        if (!buf) { snprintf(err, sizeof(err), "not enough memory for this file"); return false; }
        memcpy(buf, n->data, n->size);
        m = media_open(buf, n->size, n->name, err, sizeof(err));
        if (!m) { kfree(buf); buf = nullptr; return false; }
        strlcpy(path, p, sizeof(path));
        if (!m->title[0]) { strlcpy(m->title, n->name, sizeof(m->title)); }
        set_title(m->title);
        base_ms = 0;
        frame_idx = -1;
        play();
        return true;
    }

    u64 duration() { return m ? m->duration_ms : 0; }
    u64 now_ms() {
        if (!m) return 0;
        u64 t;
        if (m->audio && audio_available()) t = base_ms + audio_position() * 1000 / AUDIO_RATE;
        else t = playing ? base_ms + (ticks() - wall0) : base_ms;
        return min(t, duration());
    }

    void start_audio(u64 at_ms) {
        base_ms = at_ms;
        wall0 = ticks();
        if (m->audio && audio_available()) {
            media_seek(m, at_ms);
            audio_start(media_fill, m);
        }
    }
    void play() {
        if (!m) return;
        if (now_ms() >= duration() && duration()) base_ms = 0;
        if (m->audio && audio_available()) {
            if (audio_active() && !audio_playing()) audio_pause(false);
            else start_audio(base_ms);
        } else { wall0 = ticks(); }
        playing = true;
    }
    void pause() {
        if (!m) return;
        if (m->audio && audio_available()) audio_pause(true);
        else base_ms = now_ms();
        playing = false;
    }
    void toggle() { if (playing) pause(); else play(); }
    void seek_to(u64 ms) {
        if (!m) return;
        ms = min(ms, duration() ? duration() - 1 : 0);
        bool was = playing;
        base_ms = ms;
        if (m->audio && audio_available()) { start_audio(ms); if (!was) audio_pause(true); }
        else wall0 = ticks();
        frame_idx = -1;
        ui_invalidate();
    }

    void tick() override {
        if (!m) return;
        u64 t = ticks();
        if (playing) {
            if (m->video) {
                int idx = (int)(now_ms() * m->video->fps_x1000 / 1000000);
                idx = clamp(idx, 0, m->video->nframes - 1);
                if (idx != frame_idx) {
                    Image img;
                    if (m->video->decode(idx, img)) { image_free(frame); frame = img; }
                    frame_idx = idx;
                    ui_invalidate();
                }
            }
            bool done = m->audio && audio_available() ? (audio_finished() || now_ms() >= duration()) : now_ms() >= duration();
            if (done) { pause(); base_ms = 0; if (m->audio && audio_available()) audio_stop(); frame_idx = -1; }
            if (t - last_bar > 50) {
                last_bar = t;
                level_smooth = max(m->level, level_smooth * 7 / 10);
                memmove(bars, bars + 1, sizeof(bars) - 1);
                bars[sizeof(bars) - 1] = (u8)clamp(level_smooth, 0, 100);
                ui_invalidate();
            }
        }
    }

    // ------------------------------------------------------------ layout
    static const int BAR_H = 96;
    Rect view_rect() { return {0, 0, cw, ch - BAR_H}; }
    Rect seek_rect() { return {70, ch - BAR_H + 18, cw - 140, 8}; }
    Rect play_rect() { return {cw / 2 - 22, ch - 60, 44, 44}; }
    Rect vol_rect() { return {cw - 130, ch - 40, 100, 6}; }
    Rect open_rect() { return {16, ch - 52, 76, 28}; }

    static void fmt_time(u64 ms, char* out, int n) {
        u32 s = (u32)(ms / 1000);
        if (s >= 3600) snprintf(out, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
        else snprintf(out, n, "%u:%02u", s / 60, s % 60);
    }

    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        Rect v = view_rect();
        c.fill(0, 0, w, v.h, 0xFF0E1016);
        if (!m) {
            c.blit(icon("media")->big, w / 2 - 32, v.h / 2 - 54);
            c.text_centered(0, v.h / 2 + 20, w, err[0] ? err : "Drop a song or video here", err[0] ? 0xFFFF8A8A : 0xFFB4BAC8, 1, false);
            c.text_centered(0, v.h / 2 + 42, w, "WAV, MP3, FLAC, Ogg Vorbis and Motion JPEG AVI", 0xFF6A7080);
        } else if (m->video && frame.px) {
            int fw = frame.w, fh = frame.h;
            int dw = v.w, dh = v.w * fh / max(1, fw);
            if (dh > v.h) { dh = v.h; dw = v.h * fw / max(1, fh); }
            c.blit_scaled(frame, (v.w - dw) / 2, (v.h - dh) / 2, dw, dh);
        } else if (m->video) {
            c.text_centered(0, v.h / 2 - 6, w, "decoding...", 0xFF8A90A0);
        } else {
            // audio: cover art stand-in and a level history
            int cx = w / 2;
            c.rrect(cx - 56, 46, 112, 112, 24, accent(), 60);
            c.blit(icon("media")->big, cx - 32, 70);
            c.text_centered(0, 176, w, m->title, th::white, 2, true);
            if (m->artist[0]) c.text_centered(0, 208, w, m->artist, 0xFFB4BAC8);
            int n = (int)sizeof(bars), bw = max(3, (w - 80) / n - 2), x0 = (w - n * (bw + 2)) / 2;
            for (int i = 0; i < n; i++) {
                int bh = 4 + bars[i] * (v.h - 270) / 100;
                c.rrect(x0 + i * (bw + 2), v.h - 24 - bh, bw, bh, 2, accent(), 90 + i * 3);
            }
        }
        // controls
        c.fill(0, v.h, w, BAR_H, th::paper);
        c.hline(0, v.h, w, th::border);
        u64 now = now_ms(), dur = duration();
        char t0[16], t1[16];
        fmt_time(now, t0, sizeof(t0));
        fmt_time(dur, t1, sizeof(t1));
        Rect sk = seek_rect();
        c.text(14, sk.y - 2, t0, th::muted);
        c.text(w - 14 - text_width(t1), sk.y - 2, t1, th::muted);
        c.rrect(sk.x, sk.y, sk.w, sk.h, 4, th::panel_dk);
        int fillw = dur ? (int)(sk.w * now / dur) : 0;
        if (fillw > 0) c.rrect(sk.x, sk.y, max(8, fillw), sk.h, 4, accent());
        if (m) c.disc(sk.x + fillw, sk.y + 4, 7, th::white), c.disc(sk.x + fillw, sk.y + 4, 5, accent());
        // play / pause
        Rect pr = play_rect();
        bool hp = hot(pr.x, pr.y, pr.w, pr.h);
        c.disc(pr.x + 22, pr.y + 22, 22, hp ? shade(accent(), 90) : accent());
        if (playing) { c.rrect(pr.x + 14, pr.y + 12, 6, 20, 2, th::white); c.rrect(pr.x + 25, pr.y + 12, 6, 20, 2, th::white); }
        else c.aa_line(pr.x + 17, pr.y + 12, pr.x + 17, pr.y + 32, 4, th::white), c.aa_line(pr.x + 17, pr.y + 12, pr.x + 33, pr.y + 22, 4, th::white), c.aa_line(pr.x + 17, pr.y + 32, pr.x + 33, pr.y + 22, 4, th::white);
        Rect o = open_rect();
        draw_button(c, o.x, o.y, o.w, o.h, "Open", hot(o.x, o.y, o.w, o.h));
        // volume
        Rect vr = vol_rect();
        c.text(vr.x - 54, vr.y - 4, "volume", th::muted);
        c.rrect(vr.x, vr.y, vr.w, vr.h, 3, th::panel_dk);
        int vw = vr.w * audio_volume() / 100;
        if (vw > 0) c.rrect(vr.x, vr.y, max(6, vw), vr.h, 3, accent());
        c.disc(vr.x + vw, vr.y + 3, 6, th::white);
        c.disc(vr.x + vw, vr.y + 3, 4, shade(accent(), 90));
        if (!audio_available()) c.text(14, h - 18, "no sound card found (start QEMU with -device AC97)", th::red);
        else if (m) c.text(14, h - 18, m->note[0] ? m->note : m->audio ? (m->video ? "video with sound" : m->kind) : "video, no sound", m->note[0] ? th::gold_dk : th::muted);
    }

    void on_mouse(const MouseEv& e) override {
        Rect sk = seek_rect(), vr = vol_rect();
        if (e.kind == M_UP) { dragging_seek = dragging_vol = false; }
        if (e.kind == M_DOWN || e.kind == M_DBL) {
            if (Rect{sk.x - 8, sk.y - 10, sk.w + 16, sk.h + 20}.contains(e.x, e.y) && m) dragging_seek = true;
            else if (Rect{vr.x - 8, vr.y - 10, vr.w + 16, vr.h + 20}.contains(e.x, e.y)) dragging_vol = true;
            else if (play_rect().contains(e.x, e.y)) { if (m) toggle(); else open_dialog(); }
            else if (open_rect().contains(e.x, e.y)) open_dialog();
            else if (view_rect().contains(e.x, e.y) && m) toggle();
        }
        if (dragging_seek && (e.kind == M_DOWN || e.kind == M_MOVE || e.kind == M_UP)) {
            int x = clamp(e.x - sk.x, 0, sk.w);
            if (e.kind == M_UP) seek_to(duration() * x / max(1, sk.w));
            else if (e.kind == M_DOWN) { base_ms = duration() * x / max(1, sk.w); }
        }
        if (dragging_vol) audio_set_volume(clamp((e.x - vr.x) * 100 / max(1, vr.w), 0, 100));
        ui_invalidate();
    }
    void open_dialog() {
        pick_file("Open media", "/home", "wav,mp3,flac,ogg,aac,m4a,mp4,avi", false, nullptr, [](App* a, const char* p) { ((MediaApp*)a)->open_file(p); ui_invalidate(); });
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (e.key == ' ') toggle();
        else if (e.key == K_LEFT) seek_to(now_ms() > 5000 ? now_ms() - 5000 : 0);
        else if (e.key == K_RIGHT) seek_to(now_ms() + 5000);
        else if (e.key == K_UP) audio_set_volume(audio_volume() + 5);
        else if (e.key == K_DOWN) audio_set_volume(audio_volume() - 5);
        else if (e.key == 'o' && e.ctrl) open_dialog();
        ui_invalidate();
    }
    bool on_drop(const char* p, int, int) override { return open_file(p); }
    void open_arg(const char* a) override { open_file(a); }
    bool can_close() override { close_file(); return true; }
    int menus(Menu* mm) override {
        mm[0].clear(); strlcpy(mm[0].title, "File", 24);
        mm[0].add("Open" G_ELLIP, 1, "ctrl+o");
        mm[1].clear(); strlcpy(mm[1].title, "Playback", 24);
        mm[1].add(playing ? "Pause" : "Play", 2, "space", m != nullptr).add("Back 5 s", 3, "left", m != nullptr).add("Forward 5 s", 4, "right", m != nullptr)
            .sep().add("Volume up", 5, "up").add("Volume down", 6, "down");
        return 2;
    }
    void on_command(int cmd) override {
        if (cmd == 1) open_dialog();
        else if (cmd == 2) toggle();
        else if (cmd == 3) seek_to(now_ms() > 5000 ? now_ms() - 5000 : 0);
        else if (cmd == 4) seek_to(now_ms() + 5000);
        else if (cmd == 5) audio_set_volume(audio_volume() + 10);
        else if (cmd == 6) audio_set_volume(audio_volume() - 10);
        ui_invalidate();
    }
};

App* new_media(const char* arg) { return new MediaApp(arg); }
