// silicon - app registry and launching
#include "apps/apps.h"
#include "../media/media.h"

const AppInfo app_infos[] = {
    {"terminal", "Terminal", "the silicon shell", false, true},
    {"apps", "Apps", "every app, utility and game", true, true},
    {"notifications", "Notifications", "history and do not disturb", true, false},
    {"firewall", "Firewall", "packet filter and proxy", true, false},
    {"docs", "Docs", "word, spreadsheet and csv reader", false, false},
    {"write", "Write", "unicode text with scalable fonts", false, false},
    {"mail", "Mail", "pop3 and smtp email", true, false},
    {"media", "Media", "audio and video player", false, true},
    {"files", "Files", "browse SiliconFS", false, true},
    {"browser", "Beam", "web browser", false, true},
    {"slate", "Slate", "text editor", false, true},
    {"pixel", "Pixel", "pixel painter", false, true},
    {"photos", "Photos", "picture viewer", false, true},
    {"calculator", "Calculator", "numbers", true, true},
    {"monitor", "Monitor", "system monitor", true, true},
    {"games", "Games", "game library", true, true},
    {"settings", "Settings", "preferences", true, true},
    {"clock", "Clock", "clock and stopwatch", true, false},
    {"piano", "Piano", "pc speaker keys", true, false},
    {"emoji", "Emoji", "emoji picker", true, false},
    {"pdfview", "PDF", "document viewer", false, false},
    {"pong", "Pong", "bounce the ball", true, false},
    {"invaders", "Invaders", "defend the board", true, false},
    {"asteroids", "Asteroids", "blast the rocks", true, false},
    {"runner", "Runner", "jump the pins", true, false},
    {"connect4", "Connect Four", "drop discs, line up four", true, false},
    {"memory", "Memory", "find the matching pairs", true, false},
    {"wordy", "Wordy", "guess the five letter word", true, false},
    {"sudoku", "Sudoku", "fill the grid, no repeats", true, false},
    {"solitaire", "Solitaire", "classic Klondike", true, false},
    {"chess", "Chess", "play white against silicon", true, false},
    {"calendar", "Calendar", "month view and events", true, false},
    {"tasks", "Tasks", "to-do list", true, false},
    {"hex", "Hex", "hex viewer", false, false},
    {"storage", "Storage", "what fills the disk", true, false},
    {"convert", "Convert", "unit converter", true, false},
    {"palette", "Palette", "colour picker", true, false},
    {"capture", "Capture", "screenshots", true, false},
    {"weather", "Weather", "forecast from wttr.in", true, false},
    {"typing", "Typing", "typing speed test", true, false},
    {"network", "Network", "interface and ping", true, false},
    {"archive", "Archive", "zip files", false, false},
    {"wallpapers", "Wallpapers", "pick a backdrop", true, false},
    {"about", "About silicon", "this machine", true, false},
    {"minesweeper", "Minesweeper", "classic", true, false},
    {"snake", "Snake", "classic", true, false},
    {"reversi", "Reversi", "classic", true, false},
    {"stacker", "Stacker", "falling blocks", true, false},
    {"g2048", "2048", "slide the chips", true, false},
    {"breakout", "Breakout", "break the die", true, false},
    {"flappy", "Flappy Chip", "flap through traces", true, false},
    {"trash", "Trash", "deleted files", true, false},
    {"module", "Program", "a program from the SDK", false, false},
    {"power", "Power", "restart or shut down", true, false},
};
const int app_info_count = ARRAY_LEN(app_infos);

const char* app_display_name(const char* id) {
    for (int i = 0; i < app_info_count; i++) if (!strcmp(app_infos[i].id, id)) return app_infos[i].name;
    return id;
}

static App* create(const char* id, const char* arg) {
    struct { const char* id; App* (*fn)(const char*); } table[] = {
        {"terminal", new_terminal}, {"files", new_files}, {"slate", new_slate}, {"pixel", new_pixel},
        {"calculator", new_calculator}, {"monitor", new_monitor}, {"settings", new_settings},
        {"games", new_games}, {"clock", new_clock}, {"wallpapers", new_wallpapers}, {"about", new_about},
        {"minesweeper", new_minesweeper}, {"snake", new_snake}, {"reversi", new_reversi},
        {"trash", new_trash}, {"power", new_power}, {"browser", new_browser},
        {"stacker", new_stacker}, {"g2048", new_g2048}, {"breakout", new_breakout}, {"flappy", new_flappy},
        {"photos", new_photos}, {"piano", new_piano}, {"emoji", new_emoji}, {"pdfview", new_pdfview}, {"apps", new_apps}, {"notifications", new_notifications}, {"firewall", new_firewall}, {"docs", new_docs}, {"write", new_write}, {"mail", new_mail}, {"media", new_media},
        {"pong", new_pong}, {"invaders", new_invaders}, {"asteroids", new_asteroids}, {"runner", new_runner}, {"connect4", new_connect4}, {"memory", new_memory}, {"wordy", new_wordy}, {"sudoku", new_sudoku}, {"solitaire", new_solitaire}, {"chess", new_chess}, {"calendar", new_calendar}, {"tasks", new_tasks}, {"hex", new_hex}, {"storage", new_storage}, {"convert", new_convert}, {"palette", new_palette}, {"capture", new_capture}, {"weather", new_weather}, {"typing", new_typing}, {"network", new_network}, {"archive", new_archive},
    };
    for (auto& t : table) if (!strcmp(t.id, id)) return t.fn(arg);
    return nullptr;
}

App* launch(const char* id, const char* arg) {
    bool force_new = false;
    if (!strcmp(id, "__new__")) { id = arg; arg = nullptr; force_new = true; }
    bool single = false;
    for (int i = 0; i < app_info_count; i++) if (!strcmp(app_infos[i].id, id)) single = app_infos[i].single;
    Window* existing = wm_find(id);
    if (existing && (single || (!arg && !force_new))) {
        wm_focus(existing);
        if (arg) existing->app->open_arg(arg);
        return existing->app;
    }
    App* a = create(id, arg);
    if (!a) return nullptr;
    Window* w = wm_open(a, a->pref_w(), a->pref_h());
    w->resizable = a->resizable();
    return a;
}

bool is_pdf_name(const char* name);
bool is_image_name(const char* name);
bool is_office_name(const char* name);
const char* file_icon_name(const char* name, bool dir, bool has_children) {
    if (dir) return has_children ? "folder_full" : "folder";
    if (str_icontains(name, ".pix") || is_image_name(name)) return "image_file";
    if (str_icontains(name, ".htm")) return "browser";
    if (str_icontains(name, ".sapp") || str_icontains(name, ".uapp")) return "module";
    if (str_icontains(name, ".zip")) return "archive";
    if (is_pdf_name(name)) return "pdf";
    if (is_office_name(name)) return "docs";
    if (str_icontains(name, ".mp3") || str_icontains(name, ".wav") || str_icontains(name, ".avi") || str_icontains(name, ".ogg")) return "media";
    if (str_icontains(name, ".ics")) return "calendar";
    if (str_icontains(name, ".txt") || str_icontains(name, ".md") || str_icontains(name, ".log")) return "text_file";
    return "file";
}

void uapp_run(const char* path);
void open_path(const char* path) {
    FsNode* n = fs_lookup(path);
    if (!n) { ui_notify("Not found", path, "file"); return; }
    if (n == fs_trash_dir()) { launch("trash"); return; }
    if (n->dir) { launch("__new__", "files"); Window* w = wm_focused(); if (w) w->app->open_arg(path); return; }
    if (str_icontains(n->name, ".htm")) {
        char u[300];
        snprintf(u, sizeof(u), "file://%s", path);
        launch("__new__", "browser");
        Window* w = wm_focused();
        if (w) w->app->open_arg(u);
        return;
    }
    if (str_icontains(n->name, ".zip")) { launch("__new__", "archive"); Window* w = wm_focused(); if (w) w->app->open_arg(path); return; }
    if (is_office_name(n->name)) { launch("__new__", "docs"); Window* w = wm_focused(); if (w) w->app->open_arg(path); return; }
    if (media_is_media_name(n->name)) { launch("__new__", "media"); Window* w = wm_focused(); if (w) w->app->open_arg(path); return; }
    if (str_icontains(n->name, ".uapp")) { uapp_run(path); return; }
    if (str_icontains(n->name, ".sapp")) { App* a = new_module(path); Window* w = wm_open(a, a->pref_w(), a->pref_h()); w->resizable = a->resizable(); return; }
    if (is_pdf_name(n->name)) { launch("__new__", "pdfview"); Window* w = wm_focused(); if (w) w->app->open_arg(path); return; }
    if (is_image_name(n->name)) { launch("__new__", "photos"); Window* w = wm_focused(); if (w) w->app->open_arg(path); return; }
    if (str_icontains(n->name, ".pix")) { App* a = create("pixel", path); Window* w = wm_open(a, a->pref_w(), a->pref_h()); w->resizable = a->resizable(); return; }
    App* a = create("slate", path);
    Window* w = wm_open(a, a->pref_w(), a->pref_h());
    w->resizable = a->resizable();
}
