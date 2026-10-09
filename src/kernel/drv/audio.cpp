// silicon - audio: the Intel AC97 codec (QEMU's "AC97" device) playing one stereo 16-bit 48 kHz stream.
// A source (the media player, the system) supplies samples through a callback; audio_poll(), called
// from the main loops, keeps a ring of 32 DMA buffers topped up.
#include "audio.h"
#include "pci.h"

static const u16 AC97_VENDOR = 0x8086, AC97_DEVICE = 0x2415;
static const int NBUF = 32;
static const int BUF_FRAMES = 2048;                       // 42.7 ms each
static const int RING_FILL = 24;                          // keep this many buffers queued

struct __attribute__((packed)) Bd { u32 addr; u32 ctl; };

static bool present = false;
static u16 nam = 0, nabm = 0;
static Bd* bdl = nullptr;
static i16* bufs = nullptr;                               // NBUF * BUF_FRAMES stereo frames
static AudioFill source = nullptr;
static void* source_ctx = nullptr;
static bool playing = false, paused = false, ended = false;
static int next_buf = 0;                                  // the next descriptor to fill
static u64 queued_frames = 0;                             // frames handed to the DMA ring so far
static int volume_pct = 80;
static u64 played_base = 0;                               // frames the codec had played when the stream (re)started
static int last_civ = 0;
static u64 finished_frames = 0;                           // frames in buffers the codec has completely played
static int filled_valid = 0;                              // how many descriptors currently hold data
static bool starved = false;

static inline u8 r8(u16 off) { return inb(nabm + off); }
static inline u16 r16(u16 off) { return inw(nabm + off); }
static inline void w8(u16 off, u8 v) { outb(nabm + off, v); }
static inline void w32(u16 off, u32 v) { outl(nabm + off, v); }

void audio_set_volume(int pct) {
    volume_pct = clamp(pct, 0, 100);
    if (!present) return;
    int att = (100 - volume_pct) * 31 / 100;               // 0 = loudest, 31 = quietest (1.5 dB steps)
    u16 v = volume_pct == 0 ? 0x8000 : (u16)((att << 8) | att);
    outw(nam + 0x02, v);                                   // master
    outw(nam + 0x18, 0x0808);                              // pcm out a little below full scale
}
int audio_volume() { return volume_pct; }

bool audio_available() { return present; }

void audio_init() {
    if (!pci_count()) pci_scan();
    PciDevice* d = pci_find(AC97_VENDOR, AC97_DEVICE);
    if (!d) { klog("audio: no AC97 codec\n"); return; }
    if (!(d->bar[0] & 1) || !(d->bar[1] & 1)) { klog("audio: unexpected AC97 BARs\n"); return; }
    nam = (u16)(d->bar[0] & ~3u);
    nabm = (u16)(d->bar[1] & ~3u);
    pci_enable_bus_master(d);
    w32(0x2C, 0x02);                                       // cold reset released
    for (int i = 0; i < 100; i++) io_wait();
    outw(nam + 0x00, 0xFFFF);                              // codec reset
    for (int i = 0; i < 100; i++) io_wait();
    bdl = (Bd*)kmalloc(NBUF * sizeof(Bd) + 16);
    bdl = (Bd*)(((u64)bdl + 7) & ~7ull);
    bufs = (i16*)kcalloc((size_t)NBUF * BUF_FRAMES * 4 + 16);
    if ((u64)bufs > 0xFFFFFFF0ull || (u64)bdl > 0xFFFFFFF0ull) { klog("audio: buffers above 4 GiB\n"); return; }
    present = true;
    audio_set_volume(volume_pct);
    klog("audio: AC97 nam %x nabm %x, %d buffers of %d frames\n", nam, nabm, NBUF, BUF_FRAMES);
}

static void fill_buffer(int i) {
    i16* out = bufs + (size_t)i * BUF_FRAMES * 2;
    int got = 0;
    if (source && !ended) got = source(source_ctx, out, BUF_FRAMES);
    if (got < BUF_FRAMES) {
        memset(out + got * 2, 0, (size_t)(BUF_FRAMES - got) * 4);
        if (got <= 0) ended = true;
        else ended = true;                                  // the last, partial buffer
    }
    bdl[i].addr = (u32)(u64)out;
    bdl[i].ctl = (u32)(BUF_FRAMES * 2);                     // length counts 16-bit samples
    queued_frames += BUF_FRAMES;
}

static void hw_reset_and_start() {
    w8(0x1B, 0x02);                                        // reset the PCM out box
    for (int i = 0; i < 1000 && (r8(0x1B) & 0x02); i++) io_wait();
    next_buf = 0;
    queued_frames = 0;
    finished_frames = 0;
    last_civ = 0;
    ended = false;
    int n = 8;
    for (int i = 0; i < n; i++) fill_buffer(i);
    next_buf = n;
    filled_valid = n;
    w32(0x10, (u32)(u64)bdl);
    w8(0x15, (u8)(n - 1));
    if (!paused) w8(0x1B, 0x01);
}

bool audio_start(AudioFill fn, void* ctx) {
    if (!present) return false;
    audio_stop();
    source = fn;
    source_ctx = ctx;
    playing = true;
    paused = false;
    hw_reset_and_start();
    return true;
}

void audio_stop() {
    if (!present) return;
    if (playing) w8(0x1B, 0x00);
    playing = false;
    paused = false;
    source = nullptr;
    ended = true;
}

void audio_pause(bool p) {
    if (!present || !playing || p == paused) return;
    paused = p;
    w8(0x1B, p ? 0x00 : 0x01);
}

bool audio_playing() { return present && playing && !paused; }
bool audio_active() { return present && playing; }
bool audio_finished() { return present && playing && ended && filled_valid == 0; }

// frames the codec has actually played since the stream started
u64 audio_position() {
    if (!present || !playing) return 0;
    u8 civ = r8(0x14) & 31;
    u16 picb = r16(0x18);                                  // samples left in the current buffer
    u64 played = finished_frames;
    u64 in_cur = (u64)(BUF_FRAMES * 2 - min<int>(picb, BUF_FRAMES * 2)) / 2;
    (void)civ;
    return played + in_cur;
}

void audio_poll() {
    if (!present || !playing) return;
    u8 civ = r8(0x14) & 31;
    // buffers the codec moved past are consumed
    while (last_civ != civ) {
        last_civ = (last_civ + 1) % NBUF;
        finished_frames += BUF_FRAMES;
        if (filled_valid > 0) filled_valid--;
    }
    u16 sr = r16(0x16);
    if (paused) return;
    if (ended) {
        if (filled_valid <= 1 && (sr & 1)) { w8(0x1B, 0); }      // everything played: the codec halted
        return;
    }
    // top the ring up
    int guard = 0;
    while (filled_valid < RING_FILL && !ended && guard++ < NBUF) {
        fill_buffer(next_buf);
        w8(0x15, (u8)next_buf);                             // last valid index
        next_buf = (next_buf + 1) % NBUF;
        filled_valid++;
    }
    // under-run: the codec ran out of buffers and halted; start again from the current position
    if ((sr & 1) && filled_valid > 1 && !ended) {
        w8(0x1B, 0x01);
        starved = true;
    }
}
