// silicon - the shell engine: line editing, prompts for bash / zsh / fish, history
// expansion, reverse search, autosuggestions, syntax highlighting, variables, aliases,
// globbing, pipes, redirection and ; && ||
#include "shell.h"
#include "../settings.h"
#include "../arch/arch.h"
#include "../drv/drivers.h"

// ================================================================ output
void Shell::out(const char* s) {
    if (!strncmp(s, C_BRED, 5)) status = 1;      // red output means something failed
    if (capture) {
        int n = (int)strlen(s);
        if (cap_len + n + 1 > cap_cap) {
            int nc = max(cap_cap * 2, cap_len + n + 256);
            capture = (char*)krealloc(capture, nc);
            cap_cap = nc;
        }
        for (int i = 0; i < n; i++) {
            if (s[i] == 27) { while (i < n && s[i] != 'm') i++; continue; }   // strip colour codes
            capture[cap_len++] = s[i];
        }
        capture[cap_len] = 0;
        return;
    }
    con->write(s);
}

void Shell::outf(const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    out(b);
}

void Shell::err(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    status = 1;
    // errors go to the screen even inside a pipe (like stderr)
    char* saved = capture;
    capture = nullptr;
    con->write(C_BRED);
    con->write(b);
    con->write(C_RESET);
    capture = saved;
}

void Shell::begin_capture() {
    capture = (char*)kmalloc(256);
    capture[0] = 0;
    cap_len = 0;
    cap_cap = 256;
}

char* Shell::end_capture(int* n) {
    char* r = capture;
    if (n) *n = cap_len;
    capture = nullptr;
    return r;
}

// ================================================================ variables and aliases
static ShellVar* find_in(Vec<ShellVar>& v, const char* name) {
    for (int i = 0; i < v.len; i++) if (!strcmp(v[i].name, name)) return &v[i];
    return nullptr;
}

const char* Shell::get_var(const char* name) {
    static char tmp[64];
    if (!strcmp(name, "?")) { snprintf(tmp, sizeof(tmp), "%d", status); return tmp; }
    if (!strcmp(name, "PWD")) { fs_path(cwd, tmp, sizeof(tmp)); return tmp; }
    if (!strcmp(name, "RANDOM")) { snprintf(tmp, sizeof(tmp), "%u", rand() % 32768); return tmp; }
    if (!strcmp(name, "USER")) return settings.user;
    if (!strcmp(name, "HOSTNAME")) return settings.hostname;
    if (!strcmp(name, "SHELL")) { snprintf(tmp, sizeof(tmp), "/bin/%s", flavor_name()); return tmp; }
    ShellVar* v = find_in(vars, name);
    return v ? v->value : "";
}

void Shell::set_var(const char* name, const char* value, bool exported) {
    ShellVar* v = find_in(vars, name);
    if (v) { kfree(v->value); v->value = strdup(value); v->exported |= exported; return; }
    vars.push(ShellVar{strdup(name), strdup(value), exported});
}

void Shell::unset_var(const char* name) {
    for (int i = 0; i < vars.len; i++)
        if (!strcmp(vars[i].name, name)) { kfree(vars[i].name); kfree(vars[i].value); vars.remove(i); return; }
}

const char* Shell::get_alias(const char* name) {
    ShellVar* a = find_in(aliases, name);
    return a ? a->value : nullptr;
}

void Shell::set_alias(const char* name, const char* value) {
    ShellVar* a = find_in(aliases, name);
    if (a) { kfree(a->value); a->value = strdup(value); return; }
    aliases.push(ShellVar{strdup(name), strdup(value), false});
}

void Shell::unset_alias(const char* name) {
    for (int i = 0; i < aliases.len; i++)
        if (!strcmp(aliases[i].name, name)) { kfree(aliases[i].name); kfree(aliases[i].value); aliases.remove(i); return; }
}

void Shell::source_file(FsNode* f) {
    if (!f || f->dir) return;
    const char* t = fs_read_text(f);
    char ln[256];
    while (*t) {
        int n = 0;
        while (*t && *t != '\n') { if (n < 255) ln[n++] = *t; t++; }
        if (*t) t++;
        ln[n] = 0;
        char* p = ln;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        run_line(p);
        if (prog) break;
    }
}

// ================================================================ flavours
const char* Shell::flavor_name() const { return flavor == FL_ZSH ? "zsh" : flavor == FL_FISH ? "fish" : "bash"; }

static const char* rc_path(ShellFlavor f) {
    return f == FL_ZSH ? "/home/.zshrc" : f == FL_FISH ? "/home/.config/fish/config.fish" : "/home/.bashrc";
}

static void ensure_rc_files() {
    if (!fs_lookup("/home/.bashrc"))
        fs_write_text("/home/.bashrc",
                      "# ~/.bashrc - runs when bash starts\n"
                      "alias ll='ls -l'\n"
                      "alias la='ls -a'\n"
                      "export EDITOR=edit\n");
    if (!fs_lookup("/home/.zshrc"))
        fs_write_text("/home/.zshrc",
                      "# ~/.zshrc - runs when zsh starts (oh-my-silicon theme: robbyrussell)\n"
                      "alias ll='ls -l'\n"
                      "alias gs='echo nothing to commit, working tree clean'\n"
                      "export EDITOR=edit\n");
    if (!fs_lookup("/home/.config/fish/config.fish")) {
        fs_mkdirs("/home/.config/fish");
        fs_write_text("/home/.config/fish/config.fish",
                      "# config.fish - runs when fish starts\n"
                      "alias ll 'ls -l'\n"
                      "set -x EDITOR edit\n");
    }
}

void Shell::set_flavor(ShellFlavor f, bool greet) {
    flavor = f;
    if (greet) {
        if (f == FL_FISH) {
            out("Welcome to fish, the friendly interactive shell\n");
            out("Type " C_BCYAN "help" C_RESET " for instructions on how to use fish\n");
        } else if (f == FL_ZSH) {
            out(C_GRAY "zsh " SILICON_VERSION " (x86_64-silicon) " G_BULLET " oh-my-silicon loaded\n" C_RESET);
        } else {
            out(C_GRAY "GNU bash, version 5.2-silicon (x86_64-silicon)\n" C_RESET);
        }
    }
    FsNode* rc = fs_lookup(rc_path(f));
    if (rc) source_file(rc);
}

void Shell::init(Console* c, ShellHost* h) {
    con = c;
    host = h;
    cwd = fs_home() ? fs_home() : fs_root();
    len = cur = 0;
    line[0] = 0;
    set_var("HOME", "/home", true);
    set_var("PATH", "/bin:/system", true);
    set_var("TERM", "silicon-256color", true);
    set_var("LANG", "en_US.UTF-8", true);
    set_alias("..", "cd ..");
    set_alias("...", "cd ../..");
    set_alias("l", "ls");
    set_alias("cls", "clear");
    ensure_rc_files();
    flavor = !strcmp(settings.shell, "zsh") ? FL_ZSH : !strcmp(settings.shell, "fish") ? FL_FISH : FL_BASH;
}

void Shell::banner() {
    con->write(C_BMAG "  " G_CHIP " silicon " C_RESET C_GRAY SILICON_VERSION " " G_BULLET " x86_64 " G_BULLET " "
               "type " C_BWHITE "help" C_GRAY " for commands, " C_BWHITE "ui" C_GRAY " for the desktop\n" C_RESET);
    set_flavor(flavor, flavor == FL_FISH);
    con->write("\n");
}

// ================================================================ prompts
static int visible_len(const char* s) {
    int n = 0;
    for (; *s; s++) {
        if (*s == 27) { while (*s && *s != 'm') s++; if (!*s) break; continue; }
        n++;
    }
    return n;
}

static void home_path(FsNode* cwd, char* out, int n) {
    char path[256];
    fs_path(cwd, path, sizeof(path));
    FsNode* home = fs_home();
    if (home && fs_is_ancestor(home, cwd)) {
        char hp[256];
        fs_path(home, hp, sizeof(hp));
        snprintf(out, n, "~%s", path + strlen(hp));
    } else strlcpy(out, path, n);
}

// fish abbreviates every directory but the last to its first letter: ~/n/todo
static void fish_path(const char* p, char* out, int n) {
    int o = 0;
    const char* last = strrchr(p, '/');
    for (const char* s = p; *s && o < n - 1; ) {
        if (s < last && *s != '/' && *s != '~') {
            out[o++] = *s;
            while (*s && *s != '/') s++;
            continue;
        }
        out[o++] = *s++;
    }
    out[o] = 0;
}

static void emit_prompt(Shell& sh, Console* con, int& prompt_len, bool rprompt) {
    char p[480], hp[256];
    home_path(sh.cwd, hp, sizeof(hp));
    switch (sh.flavor) {
    case FL_ZSH: {
        const char* base = strrchr(hp, '/');
        base = base && base[1] ? base + 1 : hp;
        snprintf(p, sizeof(p), "%s" G_RIGHT "  " C_BCYAN C_BOLD "%s" C_RESET " ", sh.status ? C_BRED C_BOLD : C_BGREEN C_BOLD, base);
        if (rprompt) {
            // zsh right prompt: the time, pushed to the right edge
            DateTime d;
            rtc_read(d);
            char rp[16];
            snprintf(rp, sizeof(rp), "%02d:%02d:%02d", d.hour, d.minute, d.second);
            int pad = con->cols - visible_len(p) - (int)strlen(rp) - 1;
            if (pad > 8) {
                con->write(p);
                for (int i = 0; i < pad; i++) con->write(" ");
                con->write(C_GRAY);
                con->write(rp);
                con->write(C_RESET "\r");
            }
        }
        break;
    }
    case FL_FISH: {
        char fp[256];
        fish_path(hp, fp, sizeof(fp));
        if (sh.status) snprintf(p, sizeof(p), C_BGREEN "%s" C_RESET "@%s " C_GREEN "%s" C_RED " [%d]" C_RESET "> ", settings.user, settings.hostname, fp, sh.status);
        else snprintf(p, sizeof(p), C_BGREEN "%s" C_RESET "@%s " C_GREEN "%s" C_RESET "> ", settings.user, settings.hostname, fp);
        break;
    }
    default:
        snprintf(p, sizeof(p), C_BGREEN C_BOLD "%s@%s" C_RESET ":" C_BBLUE C_BOLD "%s" C_RESET "$ ", settings.user, settings.hostname, hp);
    }
    prompt_len = visible_len(p);
    con->write(p);
}

void Shell::prompt() {
    if (con->cursor_col() != 0) con->write("\n");
    input_line = con->abs_line();
    emit_prompt(*this, con, prompt_len, true);
    len = cur = 0;
    line[0] = 0;
    hist_idx = history.len;
    searching = false;
}

// ================================================================ autosuggestions + highlighting
const char* Shell::suggestion() {
    if (flavor == FL_BASH || !len || cur != len || searching) return nullptr;
    for (int i = history.len - 1; i >= 0; i--)
        if (str_starts(history[i], line) && (int)strlen(history[i]) > len) return history[i] + len;
    return nullptr;
}

bool is_builtin_name(const char* name) { return find_command(name) != nullptr; }

void Shell::highlight(char* outb, int n) {
    int o = 0;
    auto put = [&](const char* s) { while (*s && o < n - 1) outb[o++] = *s++; };
    auto putc = [&](char ch) { if (o < n - 1) outb[o++] = ch; };
    bool cmd_pos = true;
    int i = 0;
    while (i < len) {
        char ch = line[i];
        if (ch == ' ') { putc(ch); i++; continue; }
        if (ch == '|' || ch == ';' || ch == '&' || ch == '>' || ch == '<') {
            put(C_BMAG);
            while (i < len && (line[i] == '|' || line[i] == ';' || line[i] == '&' || line[i] == '>' || line[i] == '<')) putc(line[i++]);
            put(C_RESET);
            if (ch != '>' && ch != '<') cmd_pos = true;
            continue;
        }
        // one word
        int s = i;
        bool q = false;
        while (i < len && (q || (line[i] != ' ' && line[i] != '|' && line[i] != ';' && line[i] != '&' && line[i] != '>'))) {
            if (line[i] == '"' || line[i] == '\'') q = !q;
            i++;
        }
        char word[128];
        int wl = min(i - s, 127);
        memcpy(word, line + s, wl);
        word[wl] = 0;
        const char* col = C_RESET;
        if (cmd_pos) {
            bool ok = find_command(word) || get_alias(word) || strchr(word, '=') ||
                      (fs_lookup(word, cwd) && fs_lookup(word, cwd)->dir);
            // still typing the first word: don't shout red until it can't be a prefix of anything
            bool partial = false;
            if (!ok && i == len)
                for (int k = 0; k < command_count && !partial; k++) partial = str_starts(commands[k].name, word);
            col = ok ? (flavor == FL_FISH ? C_BBLUE : C_BGREEN) : partial ? C_RESET : C_BRED;
            cmd_pos = false;
        } else if (word[0] == '-') col = C_BCYAN;
        else if (word[0] == '"' || word[0] == '\'') col = C_BYEL;
        else if (word[0] == '$') col = C_BMAG;
        else if (fs_lookup(word, cwd)) col = C_BWHITE;
        put(col);
        for (int k = s; k < i; k++) putc(line[k]);
        put(C_RESET);
    }
    outb[o] = 0;
}

void Shell::redraw() {
    con->truncate_abs(input_line);
    if (searching) {
        char p[160];
        const char* hit = search_hit >= 0 ? history[search_hit] : "";
        snprintf(p, sizeof(p), C_GRAY "(reverse-i-search)" C_RESET "`" C_BYEL "%s" C_RESET "': ", search);
        con->write(p);
        con->write(hit);
        con->scroll_reset();
        return;
    }
    emit_prompt(*this, con, prompt_len, true);
    if (flavor == FL_BASH) con->write(line);
    else {
        char hl[1024];
        highlight(hl, sizeof(hl));
        con->write(hl);
    }
    const char* sug = suggestion();
    if (sug) { con->write(C_GRAY); con->write(sug); con->write(C_RESET); }
    int pos = prompt_len + cur;
    con->set_cursor_abs(input_line + pos / con->cols, pos % con->cols);
    con->scroll_reset();
}

// ================================================================ completion
static int common_prefix(const char* a, const char* b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return i;
}

void Shell::complete() {
    int start = cur;
    while (start > 0 && line[start - 1] != ' ') start--;
    char word[128];
    int wl = min(cur - start, 127);
    memcpy(word, line + start, wl);
    word[wl] = 0;
    bool first_word = true;
    for (int i = 0; i < start; i++) if (line[i] != ' ') first_word = false;

    const char* matches[64];
    bool is_dir[64];
    int nm = 0;
    const char* leaf = word;

    if (first_word) {
        for (int i = 0; i < command_count && nm < 64; i++)
            if (str_starts(commands[i].name, word)) { is_dir[nm] = false; matches[nm++] = commands[i].name; }
        for (int i = 0; i < aliases.len && nm < 64; i++)
            if (str_starts(aliases[i].name, word)) { is_dir[nm] = false; matches[nm++] = aliases[i].name; }
    }
    if (!first_word || !nm) {
        const char* slash = strrchr(word, '/');
        FsNode* dir = cwd;
        if (slash) {
            char prefix_dir[128];
            int dl = (int)(slash - word) + 1;
            memcpy(prefix_dir, word, dl);
            prefix_dir[dl] = 0;
            dir = fs_lookup(dl == 1 ? "/" : prefix_dir, cwd);
            leaf = slash + 1;
        }
        if (dir && dir->dir)
            for (int i = 0; i < dir->children.len && nm < 64; i++)
                if (str_starts(dir->children[i]->name, leaf) && (leaf[0] == '.' || dir->children[i]->name[0] != '.')) {
                    is_dir[nm] = dir->children[i]->dir;
                    matches[nm++] = dir->children[i]->name;
                }
    }
    if (nm == 0) { if (settings.sounds) speaker_beep(220, 20); return; }

    int cp = (int)strlen(matches[0]);
    for (int i = 1; i < nm; i++) cp = min(cp, common_prefix(matches[0], matches[i]));
    int ll = (int)strlen(leaf);
    char ins[128];
    int n = 0;
    for (int i = ll; i < cp && n < 120; i++) ins[n++] = matches[0][i];
    if (nm == 1) ins[n++] = is_dir[0] ? '/' : ' ';
    ins[n] = 0;
    if (n && len + n < 255) {
        memmove(line + cur + n, line + cur, len - cur + 1);
        memcpy(line + cur, ins, n);
        len += n; cur += n;
    } else if (nm > 1) {
        // list the candidates in columns (fish style pager-ish)
        con->set_cursor_abs(input_line + (prompt_len + len) / con->cols, (prompt_len + len) % con->cols);
        con->write("\n");
        int width = 0;
        for (int i = 0; i < nm; i++) width = max(width, (int)strlen(matches[i]) + 3);
        int per = max(1, con->cols / width), col = 0;
        for (int i = 0; i < nm; i++) {
            con->write(is_dir[i] ? C_BBLUE : (flavor == FL_FISH ? C_BCYAN : C_RESET));
            con->write(matches[i]);
            con->write(is_dir[i] ? "/" C_RESET : C_RESET);
            for (int k = (int)strlen(matches[i]) + (is_dir[i] ? 1 : 0); k < width; k++) con->write(" ");
            if (++col == per) { con->write("\n"); col = 0; }
        }
        if (col) con->write("\n");
        input_line = con->abs_line();
    }
    redraw();
}

// ================================================================ keys
void Shell::on_key(const KeyEvent& e) {
    if (!e.down) return;
    if (prog) {
        prog->on_key(e);
        tick();
        return;
    }
    if (searching) {
        auto find_from = [&](int from) {
            for (int i = from; i >= 0; i--) if (search[0] && str_icontains(history[i], search)) return i;
            return -1;
        };
        if (e.ctrl && e.key == 'r') { int h = find_from(search_hit - 1); if (h >= 0) search_hit = h; redraw(); return; }
        if (e.key == K_ESC || (e.ctrl && e.key == 'g') || (e.ctrl && e.key == 'c')) { searching = false; redraw(); return; }
        if (e.key == K_ENTER || e.key == K_LEFT || e.key == K_RIGHT || e.key == K_UP || e.key == K_DOWN) {
            if (search_hit >= 0) strlcpy(line, history[search_hit], sizeof(line));
            len = cur = (int)strlen(line);
            searching = false;
            if (e.key != K_ENTER) { redraw(); return; }
        } else {
            int sl = (int)strlen(search);
            if (e.key == K_BACKSPACE && sl) search[sl - 1] = 0;
            else if (e.ch >= 32 && e.ch < 127 && sl < 62) { search[sl] = e.ch; search[sl + 1] = 0; }
            search_hit = find_from(history.len - 1);
            redraw();
            return;
        }
    }
    if (e.ctrl && e.key == 'r') {
        searching = true;
        search[0] = 0;
        search_hit = -1;
        redraw();
        return;
    }
    if (e.ctrl && e.key == 'l') {
        con->clear();
        char s[256];
        strlcpy(s, line, 256);
        int c = cur;
        prompt();
        strlcpy(line, s, 256);
        len = (int)strlen(line);
        cur = c;
        redraw();
        return;
    }
    if (e.ctrl && e.key == 'c') {
        con->set_cursor_abs(input_line + (prompt_len + len) / con->cols, (prompt_len + len) % con->cols);
        con->write(C_GRAY "^C" C_RESET "\n");
        status = 130;
        prompt();
        return;
    }
    if (e.ctrl && e.key == 'a') { cur = 0; redraw(); return; }
    if (e.ctrl && e.key == 'e') { const char* s = suggestion(); if (s && cur == len) goto accept; cur = len; redraw(); return; }
    if (e.ctrl && e.key == 'u') { memmove(line, line + cur, len - cur + 1); len -= cur; cur = 0; redraw(); return; }
    if (e.ctrl && e.key == 'w') {
        int s = cur;
        while (s > 0 && line[s - 1] == ' ') s--;
        while (s > 0 && line[s - 1] != ' ') s--;
        memmove(line + s, line + cur, len - cur + 1);
        len -= cur - s;
        cur = s;
        redraw();
        return;
    }
    if (e.ctrl && e.key == 'f') goto accept;
    switch (e.key) {
    case K_ENTER: {
        con->set_cursor_abs(input_line + (prompt_len + len) / con->cols, (prompt_len + len) % con->cols);
        // wipe a ghost suggestion that was drawn after the cursor
        {
            con->truncate_abs(input_line);
            emit_prompt(*this, con, prompt_len, true);
            if (flavor == FL_BASH) con->write(line);
            else { char hl[1024]; highlight(hl, sizeof(hl)); con->write(hl); }
        }
        con->write("\n");
        con->scroll_reset();
        char cmd[256];
        strlcpy(cmd, line, sizeof(cmd));
        bool blank = true;
        for (char* p = cmd; *p; p++) if (*p != ' ') blank = false;
        if (!blank) {
            char expanded[256];
            if (flavor != FL_FISH && expand_history(cmd, expanded, sizeof(expanded))) {
                out(expanded);
                out("\n");
                strlcpy(cmd, expanded, sizeof(cmd));
            }
            if (!history.len || strcmp(history.back(), cmd)) history.push(strdup(cmd));
            if (history.len > 200) { kfree(history[0]); history.remove(0); }
            run_line(cmd);
        }
        if (!prog) prompt();
        return;
    }
    case K_BACKSPACE:
        if (cur > 0) { memmove(line + cur - 1, line + cur, len - cur + 1); cur--; len--; }
        redraw();
        return;
    case K_DELETE:
        if (cur < len) { memmove(line + cur, line + cur + 1, len - cur); len--; }
        redraw();
        return;
    case K_LEFT: if (cur > 0) cur--; redraw(); return;
    case K_RIGHT:
        if (cur < len) { cur++; redraw(); return; }
        goto accept;
    case K_HOME: cur = 0; redraw(); return;
    case K_END: cur = len; redraw(); return;
    case K_UP: {
        // fish/zsh: up searches history for lines starting with what's typed
        int from = hist_idx - 1;
        for (int i = from; i >= 0; i--)
            if (flavor == FL_BASH || !len || cur != len || str_starts(history[i], line)) {
                hist_idx = i;
                strlcpy(line, history[i], sizeof(line));
                len = cur = (int)strlen(line);
                break;
            }
        redraw();
        return;
    }
    case K_DOWN:
        if (hist_idx < history.len - 1) { hist_idx++; strlcpy(line, history[hist_idx], sizeof(line)); }
        else { hist_idx = history.len; line[0] = 0; }
        len = cur = (int)strlen(line);
        redraw();
        return;
    case K_PGUP: con->scroll_view(con->rows / 2); return;
    case K_PGDN: con->scroll_view(-con->rows / 2); return;
    case K_TAB: {
        const char* s = suggestion();
        if (s && flavor == FL_FISH) goto accept;
        complete();
        return;
    }
    }
    if (e.ch >= 32 && e.ch < 127 && !e.ctrl && len < 250) {
        memmove(line + cur + 1, line + cur, len - cur + 1);
        line[cur++] = e.ch;
        len++;
        if (cur == len && flavor == FL_BASH) {
            con->scroll_reset();
            char s[2] = {e.ch, 0};
            con->write(s);
        } else redraw();
    }
    return;
accept: {
        const char* s = suggestion();
        if (s) {
            strlcat(line, s, sizeof(line));
            len = cur = (int)strlen(line);
        }
        redraw();
    }
}

void Shell::tick() {
    if (prog) {
        prog->tick();
        if (prog->done()) {
            delete prog;
            prog = nullptr;
            con->reset_color();
            if (!prog_keep_screen) con->clear();
            prog_keep_screen = false;
            con->cursor_visible = true;
            prompt();
        }
    }
}

void Shell::start_program(ConsoleProgram* p) { prog = p; }

// ================================================================ history expansion (bash / zsh)
bool Shell::expand_history(const char* in, char* outb, int n) {
    bool changed = false;
    int o = 0;
    for (int i = 0; in[i] && o < n - 1; i++) {
        if (in[i] == '!' && in[i + 1] && in[i + 1] != ' ' && in[i + 1] != '=' && history.len) {
            const char* rep = nullptr;
            int skip = 0;
            char lastarg[128];
            if (in[i + 1] == '!') { rep = history.back(); skip = 1; }
            else if (in[i + 1] == '$') {
                const char* h = history.back();
                const char* sp = strrchr(h, ' ');
                strlcpy(lastarg, sp ? sp + 1 : h, sizeof(lastarg));
                rep = lastarg;
                skip = 1;
            } else {
                char pre[64];
                int k = 0;
                while (in[i + 1 + k] && in[i + 1 + k] != ' ' && k < 63) { pre[k] = in[i + 1 + k]; k++; }
                pre[k] = 0;
                for (int h = history.len - 1; h >= 0; h--) if (str_starts(history[h], pre)) { rep = history[h]; break; }
                skip = k;
            }
            if (rep) {
                for (const char* r = rep; *r && o < n - 1; r++) outb[o++] = *r;
                i += skip;
                changed = true;
                continue;
            }
        }
        outb[o++] = in[i];
    }
    outb[o] = 0;
    return changed;
}

// ================================================================ parsing and execution
static bool wild_match(const char* pat, const char* s) {
    if (!*pat) return !*s;
    if (*pat == '*') {
        for (; ; s++) {
            if (wild_match(pat + 1, s)) return true;
            if (!*s) return false;
        }
    }
    if (!*s) return false;
    if (*pat == '?' || *pat == *s) return wild_match(pat + 1, s + 1);
    return false;
}

// split the command line on top-level ; && || and run each part
void Shell::run_line(const char* text) {
    char buf[512];
    strlcpy(buf, text, sizeof(buf));
    int op = 0;                     // 0 always, 1 &&, 2 ||
    char* seg = buf;
    char* p = buf;
    bool sq = false, dq = false;
    for (;; p++) {
        char ch = *p;
        if (ch == '\'' && !dq) sq = !sq;
        else if (ch == '"' && !sq) dq = !dq;
        bool end = !ch;
        int next_op = -1;
        if (!sq && !dq && !end) {
            if (ch == ';') next_op = 0;
            else if (ch == '&' && p[1] == '&') next_op = 1;
            else if (ch == '|' && p[1] == '|') next_op = 2;
        }
        if (!end && next_op < 0) continue;
        *p = 0;
        bool run = op == 0 || (op == 1 && status == 0) || (op == 2 && status != 0);
        if (run) {
            // pipeline: split this segment on single | characters
            char* stages[8];
            int ns = 0;
            char* s = seg;
            bool q1 = false, q2 = false;
            stages[ns++] = s;
            for (char* k = s; *k; k++) {
                if (*k == '\'' && !q2) q1 = !q1;
                else if (*k == '"' && !q1) q2 = !q2;
                else if (*k == '|' && !q1 && !q2 && ns < 8) { *k = 0; stages[ns++] = k + 1; }
            }
            char* data = nullptr;
            for (int i = 0; i < ns; i++) {
                char* outp = nullptr;
                run_simple(stages[i], data, i < ns - 1 ? &outp : nullptr);
                if (data) kfree(data);
                data = outp;
                if (prog) break;
            }
            if (data) kfree(data);
        }
        if (end || prog) break;
        if (next_op > 0) p++;          // skip the second char of && / ||
        op = next_op;
        seg = p + 1;
    }
}

// expand one word: quotes, $VARS, ~ ; returns whether it may glob
static bool expand_word(Shell& sh, const char*& p, char* out, int n) {
    int o = 0;
    bool globbable = false, sq = false, dq = false;
    if (*p == '~' && (p[1] == '/' || p[1] == ' ' || !p[1])) {
        const char* h = sh.get_var("HOME");
        while (*h && o < n - 1) out[o++] = *h++;
        p++;
    }
    while (*p && (sq || dq || *p != ' ')) {
        char ch = *p;
        if (ch == '\'' && !dq) { sq = !sq; p++; continue; }
        if (ch == '"' && !sq) { dq = !dq; p++; continue; }
        if (ch == '$' && !sq && (p[1] == '{' || p[1] == '?' || (p[1] >= 'A' && p[1] <= 'Z') || (p[1] >= 'a' && p[1] <= 'z') || p[1] == '_')) {
            p++;
            char name[48];
            int k = 0;
            if (*p == '{') { p++; while (*p && *p != '}' && k < 47) name[k++] = *p++; if (*p == '}') p++; }
            else if (*p == '?') { name[k++] = '?'; p++; }
            else while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_') && k < 47) name[k++] = *p++;
            name[k] = 0;
            const char* v = sh.get_var(name);
            while (*v && o < n - 1) out[o++] = *v++;
            continue;
        }
        if ((ch == '*' || ch == '?') && !sq && !dq) globbable = true;
        if (o < n - 1) out[o++] = ch;
        p++;
    }
    out[o] = 0;
    return globbable;
}

int Shell::run_simple(char* text, const char* in, char** out_text) {
    // redirections: > file, >> file, < file (outside quotes)
    char* redir = nullptr;
    char* infile = nullptr;
    bool append = false;
    {
        bool sq = false, dq = false;
        for (char* p = text; *p; p++) {
            if (*p == '\'' && !dq) sq = !sq;
            else if (*p == '"' && !sq) dq = !dq;
            else if (!sq && !dq && (*p == '>' || *p == '<')) {
                bool is_in = *p == '<';
                *p++ = 0;
                if (!is_in && *p == '>') { append = true; p++; }
                while (*p == ' ') p++;
                char* name = p;
                while (*p && *p != ' ' && *p != '>' && *p != '<') p++;
                char save = *p;
                *p = 0;
                if (is_in) infile = name; else redir = name;
                if (save) { p++; while (*p == ' ') p++; }
                // anything after the file name is ignored (keeps the parser simple)
                break;
            }
        }
    }

    // tokenize + expand
    static char pool[4096];
    char* argv[64];
    int argc = 0, used = 0;
    const char* p = text;
    bool alias_done = false;
    char alias_buf[512];
retokenize:
    while (*p && argc < 63) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char word[256];
        bool glob = expand_word(*this, p, word, sizeof(word));
        // alias expansion of the first word
        if (argc == 0 && !alias_done) {
            const char* a = get_alias(word);
            if (a) {
                alias_done = true;
                snprintf(alias_buf, sizeof(alias_buf), "%s%s", a, p);
                p = alias_buf;
                goto retokenize;
            }
        }
        if (glob) {
            // expand against one directory level
            char dir[256] = "", pat[128];
            const char* slash = strrchr(word, '/');
            if (slash) { int l = (int)(slash - word); memcpy(dir, word, l); dir[l] = 0; strlcpy(pat, slash + 1, sizeof(pat)); }
            else strlcpy(pat, word, sizeof(pat));
            FsNode* d = slash ? fs_lookup(dir[0] ? dir : "/", cwd) : cwd;
            int before = argc;
            if (d && d->dir) {
                fs_sort(d);
                for (int i = 0; i < d->children.len && argc < 63; i++) {
                    const char* nm = d->children[i]->name;
                    if (nm[0] == '.' && pat[0] != '.') continue;
                    if (!wild_match(pat, nm)) continue;
                    char full[256];
                    if (slash) snprintf(full, sizeof(full), "%s/%s", dir, nm); else strlcpy(full, nm, sizeof(full));
                    int l = (int)strlen(full) + 1;
                    if (used + l > (int)sizeof(pool)) break;
                    memcpy(pool + used, full, l);
                    argv[argc++] = pool + used;
                    used += l;
                }
            }
            if (argc == before) {
                if (flavor == FL_ZSH) { err("zsh: no matches found: %s\n", word); return status; }
                if (flavor == FL_FISH) { err("fish: No matches for wildcard '%s'.\n", word); return status; }
            } else continue;
        }
        int l = (int)strlen(word) + 1;
        if (used + l > (int)sizeof(pool)) break;
        memcpy(pool + used, word, l);
        argv[argc++] = pool + used;
        used += l;
    }
    argv[argc] = nullptr;
    if (!argc) return status;

    // NAME=value assignments (bash / zsh)
    if (strchr(argv[0], '=') && argv[0][0] != '=') {
        int i = 0;
        for (; i < argc && strchr(argv[i], '=') && argv[i][0] != '='; i++) {
            char* eq = strchr(argv[i], '=');
            *eq = 0;
            if (flavor == FL_FISH) {
                err("fish: Unsupported use of '='. In fish, please use 'set %s %s'.\n", argv[i], eq + 1);
                return status;
            }
            set_var(argv[i], eq + 1);
        }
        if (i == argc) { status = 0; return 0; }
    }

    // input: pipe or < file
    char* file_in = nullptr;
    if (infile) {
        FsNode* f = fs_lookup(infile, cwd);
        if (!f || f->dir) { err("%s: no such file\n", infile); return status; }
        file_in = strdup(fs_read_text(f));
        in = file_in;
    }
    stdin_text = in;
    bool cap = out_text || redir;
    if (cap) begin_capture();
    status = 0;
    exec(argc, argv);
    stdin_text = nullptr;
    if (file_in) kfree(file_in);
    if (cap) {
        int n = 0;
        char* data = end_capture(&n);
        if (redir) {
            FsNode* f = fs_lookup(redir, cwd);
            if (!f) f = fs_create_path(redir, false, cwd);
            if (!f || f->dir) err("cannot write to %s\n", redir);
            else if (append) fs_append(f, data, n);
            else fs_write(f, data, n);
            if (out_text) { *out_text = (char*)kmalloc(1); (*out_text)[0] = 0; }
            kfree(data);
        } else *out_text = data;
    }
    return status;
}

void Shell::exec(int argc, char** argv) {
    const Command* c = find_command(argv[0]);
    if (!c) {
        FsNode* n = fs_lookup(argv[0], cwd);
        if (n && n->dir && argc == 1 && flavor != FL_BASH) { cwd = n; return; }     // zsh/fish auto-cd
        if (n && !n->dir && (strchr(argv[0], '/') || str_icontains(argv[0], ".sh"))) { source_file(n); return; }
        status = 127;
        if (flavor == FL_FISH) err("fish: Unknown command: %s\n", argv[0]);
        else if (flavor == FL_ZSH) err("zsh: command not found: %s\n", argv[0]);
        else err("bash: %s: command not found\n", argv[0]);
        status = 127;
        return;
    }
    c->fn(*this, argc, argv);
}
