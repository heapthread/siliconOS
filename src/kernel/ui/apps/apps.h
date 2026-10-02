// silicon - app factories
#pragma once
#include "../wm.h"
#include "../desk.h"

App* new_terminal(const char* arg);
App* new_files(const char* arg);
App* new_slate(const char* arg);
App* new_pixel(const char* arg);
App* new_calculator(const char* arg);
App* new_monitor(const char* arg);
App* new_settings(const char* arg);
App* new_games(const char* arg);
App* new_clock(const char* arg);
App* new_wallpapers(const char* arg);
App* new_about(const char* arg);
App* new_minesweeper(const char* arg);
App* new_snake(const char* arg);
App* new_reversi(const char* arg);
App* new_trash(const char* arg);
App* new_power(const char* arg);
App* new_browser(const char* arg);
App* new_stacker(const char* arg);
App* new_g2048(const char* arg);
App* new_breakout(const char* arg);
App* new_flappy(const char* arg);
App* new_photos(const char* arg);
App* new_piano(const char* arg);

bool is_image_name(const char* name);   // .png .jpg .gif .bmp (photos.cpp)

// high scores live in /home/games/<name>.best (games.cpp)
int  game_best(const char* name);
void game_save_best(const char* name, int score);

// Pixel paintings (.pix): "PIX1" u16 w u16 h, then w*h palette indices (pixel.cpp)
extern const u32 pix_palette[32];
bool pix_load(FsNode* f, Image& out);
