// silicon - app registry and launching
#include "apps/apps.h"

const AppInfo app_infos[] = {
    {"terminal", "Terminal", "the silicon shell", false, true},
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
        {"photos", new_photos}, {"piano", new_piano},
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
    if (is_image_name(n->name)) { launch("__new__", "photos"); Window* w = wm_focused(); if (w) w->app->open_arg(path); return; }
    if (str_icontains(n->name, ".pix")) { App* a = create("pixel", path); Window* w = wm_open(a, a->pref_w(), a->pref_h()); w->resizable = a->resizable(); return; }
    App* a = create("slate", path);
    Window* w = wm_open(a, a->pref_w(), a->pref_h());
    w->resizable = a->resizable();
}
