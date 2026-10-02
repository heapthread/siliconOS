// silicon - desktop entry points
#pragma once
#include "../core.h"

class Shell;
void ui_preload();                                // decode assets during boot
void ui_run();                                    // runs the desktop until the user exits
void ui_wallpaper_command(Shell* sh, const char* arg);
