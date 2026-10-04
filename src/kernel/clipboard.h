// silicon - the system clipboard (UTF-8 text). Copies made in silicon are also sent to the host
// clipboard, and host copies arrive through the same bridge (drv/bridge.cpp).
#pragma once
#include "core.h"

void        clip_set(const char* text, u32 len);         // copy
void        clip_set(const char* text);
const char* clip_get();                                  // never null ("" when empty)
u32         clip_len();
bool        clip_has();
u32         clip_generation();                           // changes on every copy
void        clip_from_host(const char* text, u32 len);   // host -> silicon (doesn't echo back)

#include "text.h"       // utf8_to_silicon / silicon_to_utf8 for pasting and copying
