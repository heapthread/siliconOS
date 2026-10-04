// silicon - greeter (login / lock screen), sleep screen and password handling
#pragma once
#include "../core.h"

void greeter_run(bool at_boot);          // blocks until the user signs in
void sleep_run();                        // dim, resting screen; returns when woken by a key or click
void lock_after_wake();                  // the greeter, when a password protects this machine

void password_hash(const char* pw, char out[72]);
bool password_is_set();
bool password_check(const char* pw);
void password_set(const char* pw);       // "" removes the password

// provided by desktop.cpp
struct Image;
const Image& ui_wallpaper_fit();         // the current wallpaper at the current screen size
