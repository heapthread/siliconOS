// silicon - audio output (AC97): one stereo 16-bit 48 kHz stream pulled from a callback
#pragma once
#include "../core.h"

// Fill up to `frames` stereo frames (interleaved L, R) into `out` and return how many you wrote;
// a short count (or 0) ends the stream.
typedef int (*AudioFill)(void* ctx, i16* out, int frames);

void audio_init();
bool audio_available();                    // there is a sound card
bool audio_start(AudioFill fn, void* ctx); // replaces whatever was playing
void audio_stop();
void audio_pause(bool pause);
bool audio_playing();                      // running (not paused)
bool audio_active();                       // a stream is open (running or paused)
bool audio_finished();                     // the stream played to its end
u64  audio_position();                     // frames played so far
void audio_poll();                         // call from the main loops
void audio_set_volume(int percent);
int  audio_volume();
static const int AUDIO_RATE = 48000;
