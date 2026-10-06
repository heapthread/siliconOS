// silicon - the system clipboard (UTF-8 text). Copies made in silicon are also sent to the host
// clipboard, and host copies arrive through the same bridge (drv/bridge.cpp).
#pragma once
#include "core.h"
#include "gfx/gfx.h"

void        clip_set(const char* text, u32 len);         // copy
void        clip_set(const char* text);
const char* clip_get();                                  // never null ("" when empty)
u32         clip_len();
bool        clip_has();
u32         clip_generation();                           // changes on every copy
void        clip_from_host(const char* text, u32 len);   // host -> silicon (doesn't echo back)

// ---- richer contents: a picture or a list of files (the text view of a file list is its paths)
enum { CLIP_EMPTY, CLIP_TEXT, CLIP_IMAGE, CLIP_FILES };
int         clip_kind();
void        clip_set_image(const Image& img);            // copies the pixels
const Image* clip_image();                                // nullptr unless the clipboard holds a picture
void        clip_files_begin(bool cut);
void        clip_files_add(const char* path);
void        clip_files_end();                             // publishes the list (and its paths as text)
int         clip_file_count();
const char* clip_file(int i);
bool        clip_files_cut();
void        clip_clear();

#include "text.h"       // utf8_to_silicon / silicon_to_utf8 for pasting and copying
