// silicon - media files: audio (WAV PCM and IMA ADPCM, MP3, FLAC) and video (AVI with Motion JPEG).
// A Media is opened from bytes in memory; audio comes out as 48 kHz stereo through media_fill(),
// video frames are decoded on request.
#pragma once
#include "../core.h"
#include "../gfx/gfx.h"

struct AudioTrack {
    int  rate = 44100;               // native sample rate
    u64  frames = 0;                 // length in frames (0 = unknown)
    virtual ~AudioTrack() {}
    virtual int  read(i16* out, int n) = 0;        // up to n stereo frames at the native rate; 0 at the end
    virtual void seek(u64 frame) = 0;
};

struct VideoTrack {
    int  w = 0, h = 0;
    int  nframes = 0;
    int  fps_x1000 = 25000;
    virtual ~VideoTrack() {}
    virtual bool decode(int index, Image& out) = 0;   // out.px is kmalloc'd; the caller frees it
};

struct Media {
    char kind[8];                    // "wav", "mp3", "flac", "avi"
    char title[64], artist[64], note[96];
    AudioTrack* audio = nullptr;
    VideoTrack* video = nullptr;
    u64  duration_ms = 0;
    const u8* data = nullptr;        // the file (owned by the caller)
    u32  size = 0;
    // 48 kHz resampler state
    u64  pos16 = 0;                  // position in source frames, 16.16
    i16  cache[8192 * 2];
    int  cache_start = 0, cache_len = 0;
    u64  cache_base = 0;
    bool eof = false;
    int  level = 0;                  // peak of the last buffer, 0..100
};

Media* media_open(const u8* data, u32 size, const char* name, char* err, int errlen);
void   media_free(Media* m);
int    media_fill(void* media, i16* out, int frames);     // AudioFill: 48 kHz stereo
void   media_seek(Media* m, u64 ms);
bool   media_is_media_name(const char* name);              // a file name this module can open
