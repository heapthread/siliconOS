// silicon - display policy: follow the QEMU window size, honour a chosen resolution, frame rate
#pragma once
#include "core.h"

void display_init();                       // early in boot: pick the first mode
void display_poll();                       // call from every main loop; applies window resizes
bool display_apply(int w, int h);          // switch mode and tell the UI layers
void display_on_change(void (*fn)());      // register a "screen size changed" callback
int  display_monitor_hz();                 // refresh rate of the host monitor (from the EDID)
int  display_target_fps();                 // frame cap: the setting, or the monitor's rate
void display_refresh_from_settings();
void display_bridge_size(int w, int h);    // the host helper reports the QEMU window size
void display_bridge_hz(int hz);      // after settings changed res/fps
