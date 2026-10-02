// silicon - shell commands
#include "shell.h"
#include "../settings.h"
#include "../arch/arch.h"
#include "../drv/drivers.h"

// ---------------------------------------------------------------- commands
struct ShellCommands {
static FsNode* need(Shell& sh, const char* path, bool want_dir = false, bool want_file = false) {
    FsNode* n = fs_lookup(path, sh.cwd);
    if (!n) { sh.outf(C_BRED "%s: no such file or directory\n" C_RESET, path); return nullptr; }
    if (want_dir && !n->dir) { sh.outf(C_BRED "%s: not a directory\n" C_RESET, path); return nullptr; }
    if (want_file && n->dir) { sh.outf(C_BRED "%s: is a directory\n" C_RESET, path); return nullptr; }
    return n;
}

static void fmt_size(u32 b, char* out, int n) {
    if (b < 1024) snprintf(out, n, "%u B", b);
    else if (b < 1024 * 1024) snprintf(out, n, "%u.%u KB", b / 1024, (b % 1024) * 10 / 1024);
    else snprintf(out, n, "%u.%u MB", b >> 20, ((b >> 10) % 1024) * 10 / 1024);
}

static void help(Shell& sh, int argc, char** argv) {
    if (argc > 1) {
        const Command* c = find_command(argv[1]);
        if (!c) { sh.outf("no help for '%s'\n", argv[1]); return; }
        sh.outf(C_BWHITE "%s" C_RESET " %s\n  %s\n", c->name, c->args, c->help);
        return;
    }
    sh.outf(C_BMAG G_CHIP " silicon commands" C_GRAY "  (shell: %s, switch with bash / zsh / fish)\n" C_RESET, sh.flavor_name());
    for (int i = 0; i < command_count; i++) {
        char left[40];
        snprintf(left, sizeof(left), "%s %s", commands[i].name, commands[i].args);
        sh.outf("  " C_BCYAN "%-26s" C_RESET C_GRAY "%s\n" C_RESET, left, commands[i].help);
    }
    sh.out(C_GRAY "  keys: tab completes, up/down history, ctrl+r searches history, pgup/pgdn scroll, ctrl+l clears\n");
    sh.out("  syntax: a | b, a && b, a || b, a; b, > file, >> file, < file, $VAR, *.txt, !!, ~\n" C_RESET);
}

static void clear(Shell& sh, int, char**) { sh.con->clear(); }

static void echo(Shell& sh, int argc, char** argv) {
    int i = 1;
    bool nl = true;
    if (i < argc && !strcmp(argv[i], "-n")) { nl = false; i++; }
    for (; i < argc; i++) { sh.out(argv[i]); if (i < argc - 1) sh.out(" "); }
    if (nl) sh.out("\n");
}

static void ls(Shell& sh, int argc, char** argv) {
    bool lng = false;
    const char* paths[64];
    int np = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) { if (strchr(argv[i], 'l')) lng = true; }
        else if (np < 64) paths[np++] = argv[i];
    }
    if (!np) { ls_dir(sh, sh.cwd, lng); return; }
    // files first (like ls does), then each directory under its own heading
    int files = 0;
    for (int i = 0; i < np; i++) {
        FsNode* n = need(sh, paths[i]);
        if (!n || n->dir) continue;
        sh.out(C_BGREEN);
        sh.out(paths[i]);
        sh.out(C_RESET "  ");
        files++;
    }
    if (files) sh.out("\n");
    bool first = !files;
    for (int i = 0; i < np; i++) {
        FsNode* n = fs_lookup(paths[i], sh.cwd);
        if (!n || !n->dir) continue;
        if (np > 1) sh.outf("%s" C_BOLD "%s:\n" C_RESET, first ? "" : "\n", paths[i]);
        first = false;
        ls_dir(sh, n, lng);
    }
}

static void ls_dir(Shell& sh, FsNode* d, bool lng) {
    fs_sort(d);
    if (!d->children.len) { sh.out(C_GRAY "(empty)\n" C_RESET); return; }
    if (lng) {
        for (int i = 0; i < d->children.len; i++) {
            FsNode* n = d->children[i];
            char sz[24];
            DateTime dt;
            stamp_to_datetime(n->mtime, dt);
            if (n->dir) snprintf(sz, sizeof(sz), "%d items", n->children.len);
            else fmt_size(n->size, sz, sizeof(sz));
            sh.outf(C_GRAY "%s  %10s  %s %02d %02d:%02d  " C_RESET "%s%s%s\n" C_RESET, n->dir ? "dir " : "file", sz,
                    month_name(dt.month), dt.day, dt.hour, dt.minute, n->dir ? C_BBLUE C_BOLD : "", n->name,
                    n->dir ? "/" : "");
        }
        return;
    }
    int width = 0;
    for (int i = 0; i < d->children.len; i++) width = max(width, (int)strlen(d->children[i]->name) + 3);
    int per = max(1, sh.con->cols / width);
    int col = 0;
    for (int i = 0; i < d->children.len; i++) {
        FsNode* n = d->children[i];
        char nm[64];
        snprintf(nm, sizeof(nm), "%s%s", n->name, n->dir ? "/" : "");
        sh.out(n->dir ? C_BBLUE C_BOLD : (str_icontains(n->name, ".txt") ? C_RESET : C_BGREEN));
        sh.out(nm);
        sh.out(C_RESET);
        for (int k = (int)strlen(nm); k < width; k++) sh.out(" ");
        if (++col == per) { sh.out("\n"); col = 0; }
    }
    if (col) sh.out("\n");
}

static void cd(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.cwd = fs_home(); return; }
    FsNode* d = need(sh, argv[1], true);
    if (d) sh.cwd = d;
}

static void pwd(Shell& sh, int, char**) {
    char p[256];
    fs_path(sh.cwd, p, sizeof(p));
    sh.outf("%s\n", p);
}

static void mkdir(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: mkdir <dir>...\n"); return; }
    for (int i = 1; i < argc; i++) {
        if (fs_lookup(argv[i], sh.cwd)) { sh.outf(C_BYEL "%s already exists\n" C_RESET, argv[i]); continue; }
        if (!fs_create_path(argv[i], true, sh.cwd)) sh.outf(C_BRED "cannot create %s\n" C_RESET, argv[i]);
    }
}

static void touch(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: touch <file>...\n"); return; }
    for (int i = 1; i < argc; i++) {
        FsNode* f = fs_lookup(argv[i], sh.cwd);
        if (f) { f->mtime = rtc_stamp(); fs_mark_dirty(); continue; }
        if (!fs_create_path(argv[i], false, sh.cwd)) sh.outf(C_BRED "cannot create %s\n" C_RESET, argv[i]);
    }
}

static void cat(Shell& sh, int argc, char** argv) {
    if (argc < 2) {
        if (sh.stdin_text) sh.out(sh.stdin_text);
        else sh.out("usage: cat <file>...\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        FsNode* f = need(sh, argv[i], false, true);
        if (!f) continue;
        const char* t = fs_read_text(f);
        sh.out(t);
        if (f->size && t[f->size - 1] != '\n') sh.out("\n");
    }
}

static void write_impl(Shell& sh, int argc, char** argv, bool append) {
    if (argc < 2) { sh.outf("usage: %s <file> <text...>\n", argv[0]); return; }
    FsNode* f = fs_lookup(argv[1], sh.cwd);
    if (!f) f = fs_create_path(argv[1], false, sh.cwd);
    if (!f || f->dir) { sh.outf(C_BRED "cannot write %s\n" C_RESET, argv[1]); return; }
    char buf[1024] = "";
    for (int i = 2; i < argc; i++) { strlcat(buf, argv[i], sizeof(buf)); if (i < argc - 1) strlcat(buf, " ", sizeof(buf)); }
    strlcat(buf, "\n", sizeof(buf));
    if (append) fs_append(f, buf, strlen(buf));
    else fs_write(f, buf, strlen(buf));
}
static void write(Shell& sh, int argc, char** argv) { write_impl(sh, argc, argv, false); }
static void append(Shell& sh, int argc, char** argv) { write_impl(sh, argc, argv, true); }

static void edit(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: edit <file>\n"); return; }
    FsNode* f = fs_lookup(argv[1], sh.cwd);
    if (f && f->dir) { sh.outf(C_BRED "%s is a directory\n" C_RESET, argv[1]); return; }
    sh.start_program(make_editor(sh.con, f, argv[1]));
    // editor creates the file on save relative to the shell cwd
    extern FsNode* editor_cwd;
    editor_cwd = sh.cwd;
}

static void rm(Shell& sh, int argc, char** argv) {
    bool rec = false;
    int n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-rf")) { rec = true; continue; }
        n++;
        FsNode* f = need(sh, argv[i]);
        if (!f) continue;
        if (f == fs_root() || f == fs_home() || f == fs_trash_dir()) { sh.outf(C_BRED "refusing to remove %s\n" C_RESET, argv[i]); continue; }
        if (f->dir && !rec && f->children.len) { sh.outf(C_BRED "%s is a non-empty directory (use rm -r)\n" C_RESET, argv[i]); continue; }
        if (fs_is_ancestor(f, sh.cwd)) sh.cwd = f->parent;
        fs_remove(f);
    }
    if (!n) sh.out("usage: rm [-r] <path>...\n");
}

static void mv(Shell& sh, int argc, char** argv) {
    if (argc != 3) { sh.out("usage: mv <from> <to>\n"); return; }
    FsNode* f = need(sh, argv[1]);
    if (!f) return;
    FsNode* dst = fs_lookup(argv[2], sh.cwd);
    bool ok;
    if (dst && dst->dir) ok = fs_move(f, dst, nullptr);
    else {
        const char* slash = strrchr(argv[2], '/');
        FsNode* parent = sh.cwd;
        const char* leaf = argv[2];
        if (slash) {
            char dp[256];
            int l = (int)(slash - argv[2]);
            memcpy(dp, argv[2], l); dp[l] = 0;
            parent = l ? fs_lookup(dp, sh.cwd) : fs_root();
            leaf = slash + 1;
        }
        ok = parent && fs_move(f, parent, leaf);
    }
    if (!ok) sh.out(C_BRED "mv: failed\n" C_RESET);
}

static void cp(Shell& sh, int argc, char** argv) {
    if (argc != 3) { sh.out("usage: cp <from> <to>\n"); return; }
    FsNode* f = need(sh, argv[1]);
    if (!f) return;
    FsNode* dst = fs_lookup(argv[2], sh.cwd);
    FsNode* r = nullptr;
    if (dst && dst->dir) r = fs_copy(f, dst, nullptr);
    else if (!dst) {
        char leaf[FS_NAME_MAX];
        const char* slash = strrchr(argv[2], '/');
        FsNode* parent = sh.cwd;
        if (slash) {
            char dp[256];
            int l = (int)(slash - argv[2]);
            memcpy(dp, argv[2], l); dp[l] = 0;
            parent = l ? fs_lookup(dp, sh.cwd) : fs_root();
            strlcpy(leaf, slash + 1, sizeof(leaf));
        } else strlcpy(leaf, argv[2], sizeof(leaf));
        if (parent) r = fs_copy(f, parent, leaf);
    }
    if (!r) sh.out(C_BRED "cp: failed\n" C_RESET);
}

static void tree_rec(Shell& sh, FsNode* n, char* prefix, int depth) {
    fs_sort(n);
    for (int i = 0; i < n->children.len; i++) {
        FsNode* c = n->children[i];
        bool last = i == n->children.len - 1;
        sh.outf(C_GRAY "%s%s" G_H " " C_RESET "%s%s%s\n" C_RESET, prefix, last ? G_BL : G_LT, c->dir ? C_BBLUE C_BOLD : "",
                c->name, c->dir ? "/" : "");
        if (c->dir && depth < 12) {
            int pl = (int)strlen(prefix);
            strlcat(prefix, last ? "  " : G_V " ", 128);
            tree_rec(sh, c, prefix, depth + 1);
            prefix[pl] = 0;
        }
    }
}

static void tree(Shell& sh, int argc, char** argv) {
    FsNode* d = argc > 1 ? need(sh, argv[1], true) : sh.cwd;
    if (!d) return;
    char p[256];
    fs_path(d, p, sizeof(p));
    sh.outf(C_BBLUE C_BOLD "%s\n" C_RESET, p);
    char prefix[128] = "";
    tree_rec(sh, d, prefix, 0);
}

static void find_rec(Shell& sh, FsNode* n, const char* pat, int& hits) {
    for (int i = 0; i < n->children.len; i++) {
        FsNode* c = n->children[i];
        if (str_icontains(c->name, pat)) {
            char p[256];
            fs_path(c, p, sizeof(p));
            sh.outf("%s%s\n" C_RESET, c->dir ? C_BBLUE : "", p);
            hits++;
        }
        if (c->dir) find_rec(sh, c, pat, hits);
    }
}

static void find(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: find <name>\n"); return; }
    int hits = 0;
    find_rec(sh, fs_root(), argv[1], hits);
    if (!hits) sh.out(C_GRAY "nothing found\n" C_RESET);
}

static bool contains(const char* hay, const char* needle, bool icase) {
    if (icase) return str_icontains(hay, needle);
    int n = (int)strlen(needle);
    for (; *hay; hay++) if (!strncmp(hay, needle, n)) return true;
    return !n;
}

// run fn on every line of text
template <typename F> static void each_line(const char* t, F fn) {
    int ln = 1;
    while (*t) {
        const char* e = strchr(t, '\n');
        int l = e ? (int)(e - t) : (int)strlen(t);
        char lb[512];
        int cl = min(l, 511);
        memcpy(lb, t, cl);
        lb[cl] = 0;
        fn(lb, ln);
        t += l + (e ? 1 : 0);
        ln++;
    }
}

static void grep(Shell& sh, int argc, char** argv) {
    bool icase = false, num = false, inv = false, count = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char* f = argv[i] + 1; *f; f++) {
            if (*f == 'i') icase = true; else if (*f == 'n') num = true;
            else if (*f == 'v') inv = true; else if (*f == 'c') count = true;
        }
    }
    if (i >= argc) { sh.out("usage: grep [-invc] <text> [file...]\n"); return; }
    const char* pat = argv[i++];
    int hits = 0;
    auto scan = [&](const char* text, const char* fname) {
        each_line(text, [&](const char* l, int ln) {
            if (contains(l, pat, icase) == inv) return;
            hits++;
            if (count) return;
            if (fname) sh.outf(C_BMAG "%s" C_GRAY ":" C_RESET, fname);
            if (num) sh.outf(C_GRAY "%d:" C_RESET, ln);
            sh.outf("%s\n", l);
        });
    };
    if (i >= argc) {
        if (!sh.stdin_text) { sh.out("usage: grep [-invc] <text> [file...]\n"); return; }
        scan(sh.stdin_text, nullptr);
    }
    for (int k = i; k < argc; k++) {
        FsNode* f = need(sh, argv[k], false, true);
        if (f) scan(fs_read_text(f), argc - i > 1 ? argv[k] : nullptr);
    }
    if (count) sh.outf("%d\n", hits);
    if (!hits) sh.status = 1;
}

static void wc(Shell& sh, int argc, char** argv) {
    const char* t = sh.stdin_text;
    if (argc >= 2 && argv[argc - 1][0] != '-') { FsNode* f = need(sh, argv[argc - 1], false, true); if (!f) return; t = fs_read_text(f); }
    if (!t) { sh.out("usage: wc [-l] <file>\n"); return; }
    int lines = 0, words = 0, bytes = 0;
    bool inw = false;
    for (const char* p = t; *p; p++) {
        bytes++;
        if (*p == '\n') lines++;
        bool sp = *p == ' ' || *p == '\n' || *p == '\t';
        if (!sp && !inw) words++;
        inw = !sp;
    }
    if (argc >= 2 && !strcmp(argv[1], "-l")) sh.outf("%d\n", lines);
    else sh.outf("%7d %7d %7d\n", lines, words, bytes);
}

static const char* input_of(Shell& sh, int argc, char** argv, int first_file) {
    for (int i = first_file; i < argc; i++)
        if (argv[i][0] != '-') { FsNode* f = need(sh, argv[i], false, true); return f ? fs_read_text(f) : nullptr; }
    if (!sh.stdin_text) sh.err("%s: no input (give a file or pipe something in)\n", argv[0]);
    return sh.stdin_text;
}

static int count_arg(int argc, char** argv, int def) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) return atoi(argv[i + 1]);
        if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') return atoi(argv[i] + 1);
    }
    return def;
}

static void head(Shell& sh, int argc, char** argv) {
    int n = count_arg(argc, argv, 10);
    const char* t = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) { i++; continue; }
        if (argv[i][0] == '-') continue;
        if (argv[i][0] >= '0' && argv[i][0] <= '9' && !fs_lookup(argv[i], sh.cwd)) { n = atoi(argv[i]); continue; }
        FsNode* f = need(sh, argv[i], false, true);
        if (!f) return;
        t = fs_read_text(f);
    }
    if (!t) t = sh.stdin_text;
    if (!t) { sh.out("usage: head [-n N] <file>\n"); return; }
    each_line(t, [&](const char* l, int ln) { if (ln <= n) sh.outf("%s\n", l); });
}

static void tail(Shell& sh, int argc, char** argv) {
    int n = count_arg(argc, argv, 10);
    const char* t = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) { i++; continue; }
        if (argv[i][0] == '-') continue;
        FsNode* f = need(sh, argv[i], false, true);
        if (!f) return;
        t = fs_read_text(f);
    }
    if (!t) t = sh.stdin_text;
    if (!t) { sh.out("usage: tail [-n N] <file>\n"); return; }
    int total = 0;
    each_line(t, [&](const char*, int) { total++; });
    each_line(t, [&](const char* l, int ln) { if (ln > total - n) sh.outf("%s\n", l); });
}

static void sort(Shell& sh, int argc, char** argv) {
    bool rev = false, numeric = false;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-r")) rev = true; if (!strcmp(argv[i], "-n")) numeric = true; }
    const char* t = input_of(sh, argc, argv, 1);
    if (!t) return;
    Vec<char*> v;
    each_line(t, [&](const char* l, int) { v.push(strdup(l)); });
    for (int i = 1; i < v.len; i++) {
        char* k = v[i];
        int j = i - 1;
        auto less = [&](const char* a, const char* b) {
            int c = numeric ? atoi(a) - atoi(b) : strcmp(a, b);
            return rev ? c > 0 : c < 0;
        };
        while (j >= 0 && less(k, v[j])) { v[j + 1] = v[j]; j--; }
        v[j + 1] = k;
    }
    for (int i = 0; i < v.len; i++) { sh.outf("%s\n", v[i]); kfree(v[i]); }
    v.free();
}

static void uniq(Shell& sh, int argc, char** argv) {
    bool cnt = argc > 1 && !strcmp(argv[1], "-c");
    const char* t = input_of(sh, argc, argv, 1);
    if (!t) return;
    char prev[512] = "";
    int run = 0;
    bool first = true;
    auto flush = [&]() { if (run) { if (cnt) sh.outf("%7d %s\n", run, prev); else sh.outf("%s\n", prev); } };
    each_line(t, [&](const char* l, int) {
        if (!first && !strcmp(l, prev)) { run++; return; }
        flush();
        strlcpy(prev, l, sizeof(prev));
        run = 1;
        first = false;
    });
    flush();
}

static void rev(Shell& sh, int argc, char** argv) {
    const char* t = input_of(sh, argc, argv, 1);
    if (!t) return;
    each_line(t, [&](const char* l, int) {
        char r[512];
        int n = (int)strlen(l);
        for (int i = 0; i < n; i++) r[i] = l[n - 1 - i];
        r[n] = 0;
        sh.outf("%s\n", r);
    });
}

static void tee(Shell& sh, int argc, char** argv) {
    const char* t = sh.stdin_text ? sh.stdin_text : "";
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        FsNode* f = fs_lookup(argv[i], sh.cwd);
        if (!f) f = fs_create_path(argv[i], false, sh.cwd);
        if (f && !f->dir) fs_write(f, t, strlen(t));
    }
    sh.out(t);
}

static void seq(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: seq [first] last\n"); return; }
    int a = argc > 2 ? atoi(argv[1]) : 1, b = atoi(argv[argc - 1]);
    int step = a <= b ? 1 : -1;
    for (int i = a, n = 0; n < 10000; i += step, n++) { sh.outf("%d\n", i); if (i == b) break; }
}

static void trash(Shell& sh, int argc, char** argv) {
    if (argc < 2) {
        FsNode* t = fs_trash_dir();
        if (!t->children.len) { sh.out(C_GRAY "trash is empty\n" C_RESET); return; }
        for (int i = 0; i < t->children.len; i++)
            sh.outf("  %s  " C_GRAY "(from %s)\n" C_RESET, t->children[i]->name, t->children[i]->origin);
        return;
    }
    for (int i = 1; i < argc; i++) {
        FsNode* f = need(sh, argv[i]);
        if (!f) continue;
        if (f == fs_home() || f == fs_root()) { sh.out(C_BRED "refusing\n" C_RESET); continue; }
        if (fs_is_ancestor(f, sh.cwd)) sh.cwd = f->parent;
        if (fs_trash(f)) sh.outf(C_GRAY "moved %s to the trash\n" C_RESET, argv[i]);
    }
}

static void restore(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: restore <name in trash>\n"); return; }
    FsNode* f = fs_child(fs_trash_dir(), argv[1]);
    if (!f) { sh.outf(C_BRED "%s is not in the trash\n" C_RESET, argv[1]); return; }
    FsNode* r = fs_restore(f);
    char p[256];
    if (r) { fs_path(r, p, sizeof(p)); sh.outf("restored to %s\n", p); }
}

static void emptytrash(Shell& sh, int, char**) {
    int n = fs_trash_dir()->children.len;
    fs_empty_trash();
    sh.outf("removed %d item%s\n", n, n == 1 ? "" : "s");
}

static void df(Shell& sh, int, char**) {
    fs_sync();
    char used[24], cap[24];
    fmt_size(fs_disk_used(), used, sizeof(used));
    fmt_size(fs_disk_capacity(), cap, sizeof(cap));
    if (!fs_persistent()) { sh.out(C_BYEL "filesystem is RAM-only (no disk found)\n" C_RESET); return; }
    sh.outf("SiliconFS on ata0  " C_BWHITE "%s" C_RESET " used of %s  " C_GRAY "(%d nodes)\n" C_RESET, used, cap,
            fs_count(fs_root()));
}

static void bar(Shell& sh, u64 used, u64 total, int width) {
    int fill = total ? (int)(used * width / total) : 0;
    sh.out(C_BMAG);
    for (int i = 0; i < width; i++) sh.out(i < fill ? G_FULL : G_LIGHT);
    sh.out(C_RESET);
}

static void mem(Shell& sh, int, char**) {
    u64 used = heap_used(), total = heap_total();
    sh.out("heap  ");
    bar(sh, used, total, 30);
    sh.outf("  %u KB / %u MB\n", (u32)(used >> 10), (u32)(total >> 20));
    sh.outf("ram   %u MB installed\n", (u32)(mem_total_bytes >> 20));
}

static void uptime(Shell& sh, int, char**) {
    u64 s = ticks() / 1000;
    u64 idle = idle_ticks(), t = ticks();
    sh.outf("up %uh %02um %02us, cpu idle %u%%\n", (u32)(s / 3600), (u32)(s / 60 % 60), (u32)(s % 60),
            t ? (u32)(idle * 100 / t) : 0);
}

static void date(Shell& sh, int, char**) {
    DateTime d;
    rtc_read(d);
    sh.outf("%s %s %02d %d  %02d:%02d:%02d\n", weekday_name(d.weekday), month_name(d.month), d.day, d.year,
            d.hour, d.minute, d.second);
}

static void cal(Shell& sh, int, char**) {
    DateTime d;
    rtc_read(d);
    static const int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days = md[d.month - 1] + (d.month == 2 && d.year % 4 == 0);
    int first = (d.weekday - (d.day - 1) % 7 + 7) % 7;
    sh.outf("     " C_BWHITE "%s %d\n" C_RESET, month_name(d.month), d.year);
    sh.out(C_GRAY " Su Mo Tu We Th Fr Sa\n" C_RESET);
    for (int i = 0; i < first; i++) sh.out("   ");
    for (int day = 1; day <= days; day++) {
        if (day == d.day) sh.outf(" " C_BMAG C_BOLD "%2d" C_RESET, day);
        else sh.outf(" %2d", day);
        if ((first + day) % 7 == 0) sh.out("\n");
    }
    sh.out("\n");
}

static void fetch(Shell& sh, int, char**) {
    static const char* art[] = {
        "   " G_V " " G_V " " G_V " " G_V " " G_V "   ",
        "  " G_TL G_H G_H G_H G_H G_H G_H G_H G_H G_H G_TR "  ",
        G_H G_H G_RT " " G_FULL G_FULL G_FULL G_FULL G_FULL G_FULL G_FULL " " G_LT G_H G_H,
        G_H G_H G_RT " " G_FULL G_LIGHT G_LIGHT G_LIGHT G_LIGHT G_LIGHT G_FULL " " G_LT G_H G_H,
        G_H G_H G_RT " " G_FULL G_LIGHT G_MED G_MED G_MED G_LIGHT G_FULL " " G_LT G_H G_H,
        G_H G_H G_RT " " G_FULL G_LIGHT G_LIGHT G_LIGHT G_LIGHT G_LIGHT G_FULL " " G_LT G_H G_H,
        G_H G_H G_RT " " G_FULL G_FULL G_FULL G_FULL G_FULL G_FULL G_FULL " " G_LT G_H G_H,
        "  " G_BL G_H G_H G_H G_H G_H G_H G_H G_H G_H G_BR "  ",
        "   " G_V " " G_V " " G_V " " G_V " " G_V "   ",
    };
    char info[10][96];
    u64 s = ticks() / 1000;
    snprintf(info[0], 96, C_BMAG C_BOLD "%s" C_RESET "@" C_BCYAN C_BOLD "%s" C_RESET, settings.user, settings.hostname);
    snprintf(info[1], 96, C_GRAY "-----------------" C_RESET);
    snprintf(info[2], 96, C_BMAG "os     " C_RESET "silicon " SILICON_VERSION " x86_64");
    snprintf(info[3], 96, C_BMAG "kernel " C_RESET "silicon/ring0, own bootloader");
    snprintf(info[4], 96, C_BMAG "uptime " C_RESET "%uh %um %us", (u32)(s / 3600), (u32)(s / 60 % 60), (u32)(s % 60));
    snprintf(info[5], 96, C_BMAG "shell  " C_RESET "%s (silicon)", sh.flavor_name());
    snprintf(info[6], 96, C_BMAG "cpu    " C_RESET "%.36s", cpu_brand());
    snprintf(info[7], 96, C_BMAG "memory " C_RESET "%u KB / %u MB", (u32)(heap_used() >> 10), (u32)(mem_total_bytes >> 20));
    snprintf(info[8], 96, C_BMAG "screen " C_RESET "%dx%d", fb.width, fb.height);
    char sw[96] = "";
    for (int i = 1; i < 7; i++) {
        char b[16];
        snprintf(b, sizeof(b), "\x1b[%dm" G_FULL G_FULL G_FULL, 90 + i);
        strlcat(sw, b, sizeof(sw));
    }
    strlcat(sw, C_RESET, sizeof(sw));
    snprintf(info[9], 96, "%s", sw);
    sh.out("\n");
    for (int i = 0; i < 10; i++) {
        sh.out(C_BMAG);
        sh.out(i < 9 ? art[i] : "                 ");
        sh.out(C_RESET "   ");
        sh.out(info[i]);
        sh.out("\n");
    }
    sh.out("\n");
}

static void theme(Shell& sh, int argc, char** argv) {
    if (argc < 2) {
        sh.out("themes:");
        for (int i = 0; i < console_theme_count; i++)
            sh.outf(" %s%s" C_RESET, i == sh.con->theme ? C_BMAG C_BOLD : "", console_themes[i].name);
        sh.out("\nusage: theme <name>\n");
        return;
    }
    for (int i = 0; i < console_theme_count; i++)
        if (!strcmp(argv[1], console_themes[i].name)) {
            sh.con->theme = i;
            sh.con->dirty = true;
            if (!sh.host->host_is_ui()) { settings.term_theme = i; settings_save(); }
            return;
        }
    sh.outf(C_BRED "unknown theme %s\n" C_RESET, argv[1]);
}

static void zoom(Shell& sh, int, char**) {
    if (sh.host->host_is_ui()) { sh.out("zoom works in the full-screen terminal\n"); return; }
    sh.host->shell_request(&sh, SR_ZOOM, nullptr);
}

static void colors(Shell& sh, int, char**) {
    for (int i = 0; i < 16; i++) sh.outf("\x1b[%dm" G_FULL G_FULL G_FULL, i < 8 ? 30 + i : 90 + i - 8);
    sh.out(C_RESET "\n");
}

static void history(Shell& sh, int, char**) {
    for (int i = 0; i < sh.history.len; i++) sh.outf(C_GRAY "%4d" C_RESET "  %s\n", i + 1, sh.history[i]);
}

static void whoami(Shell& sh, int, char**) { sh.outf("%s\n", settings.user); }

static void user(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.outf("%s\n", settings.user); return; }
    strlcpy(settings.user, argv[1], sizeof(settings.user));
    settings_save();
}

static void hostname(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.outf("%s\n", settings.hostname); return; }
    strlcpy(settings.hostname, argv[1], sizeof(settings.hostname));
    settings_save();
}

static void uname(Shell& sh, int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "-a")) sh.outf("silicon %s " SILICON_VERSION " x86_64 %s\n", settings.hostname, cpu_vendor());
    else sh.out("silicon\n");
}

static void about(Shell& sh, int, char**) {
    sh.out(C_BMAG C_BOLD G_CHIP " silicon " SILICON_VERSION C_RESET "\n");
    sh.out("a tiny pixel operating system, written from scratch in C++ and assembly.\n");
    sh.out(C_GRAY "own MBR + stage 2 bootloader " G_RIGHT " long mode " G_RIGHT " silicon kernel\n");
    sh.out("SiliconFS on disk, pixel desktop with a silicon-die dock.\n" C_RESET);
}

static void setup(Shell& sh, int, char**) { sh.host->shell_request(&sh, SR_SETUP, nullptr); }
static void reboot(Shell& sh, int, char**) { sh.out("rebooting...\n"); fs_sync(); sleep_ms(200); power_reboot(); }
static void shutdown(Shell& sh, int, char**) { sh.out("saving and powering off...\n"); fs_sync(); sleep_ms(200); power_off(); }
static void sync(Shell& sh, int, char**) { sh.out(fs_sync() ? "synced\n" : "nothing to sync (RAM only)\n"); }

static void ui(Shell& sh, int, char**) {
    if (sh.host->host_is_ui()) { sh.out("you're already in the desktop " G_HEART "\n"); return; }
    sh.host->shell_request(&sh, SR_START_UI, nullptr);
}

static void exit(Shell& sh, int, char**) {
    if (sh.flavor_stack.len) {
        ShellFlavor f = sh.flavor_stack.back();
        sh.flavor_stack.len--;
        sh.flavor = f;
        return;
    }
    sh.host->shell_request(&sh, SR_EXIT, nullptr);
}

static void enter_shell(Shell& sh, ShellFlavor f) {
    sh.flavor_stack.push(sh.flavor);
    sh.set_flavor(f, true);
}
static void bash(Shell& sh, int argc, char** argv) {
    if (argc > 1) { FsNode* f = need(sh, argv[1], false, true); if (f) sh.source_file(f); return; }
    enter_shell(sh, FL_BASH);
}
static void zsh(Shell& sh, int argc, char** argv) {
    if (argc > 1) { FsNode* f = need(sh, argv[1], false, true); if (f) sh.source_file(f); return; }
    enter_shell(sh, FL_ZSH);
}
static void fish(Shell& sh, int argc, char** argv) {
    if (argc > 1) { FsNode* f = need(sh, argv[1], false, true); if (f) sh.source_file(f); return; }
    enter_shell(sh, FL_FISH);
}

static void chsh(Shell& sh, int argc, char** argv) {
    const char* want = argc > 2 && !strcmp(argv[1], "-s") ? argv[2] : argc > 1 ? argv[1] : nullptr;
    if (!want) { sh.outf("default shell: %s\nusage: chsh [-s] bash|zsh|fish\n", settings.shell); return; }
    const char* name = strrchr(want, '/') ? strrchr(want, '/') + 1 : want;
    if (strcmp(name, "bash") && strcmp(name, "zsh") && strcmp(name, "fish")) { sh.err("chsh: %s: unknown shell\n", want); return; }
    strlcpy(settings.shell, name, sizeof(settings.shell));
    settings_save();
    sh.outf("default shell is now %s (new terminals use it)\n", name);
}

static void export_(Shell& sh, int argc, char** argv) {
    if (argc < 2) {
        for (int i = 0; i < sh.vars.len; i++)
            if (sh.vars[i].exported) sh.outf("declare -x %s=\"%s\"\n", sh.vars[i].name, sh.vars[i].value);
        return;
    }
    for (int i = 1; i < argc; i++) {
        char* eq = strchr(argv[i], '=');
        if (eq) { *eq = 0; sh.set_var(argv[i], eq + 1, true); }
        else sh.set_var(argv[i], sh.get_var(argv[i]), true);
    }
}

static void unset(Shell& sh, int argc, char** argv) { for (int i = 1; i < argc; i++) sh.unset_var(argv[i]); }

static void env(Shell& sh, int, char**) {
    const char* names[] = {"USER", "HOSTNAME", "SHELL", "PWD"};
    for (const char* n : names) sh.outf("%s=%s\n", n, sh.get_var(n));
    for (int i = 0; i < sh.vars.len; i++) sh.outf("%s=%s\n", sh.vars[i].name, sh.vars[i].value);
}

// fish-style: set NAME value..., set -x NAME value, set -e NAME, set (lists)
static void set(Shell& sh, int argc, char** argv) {
    if (argc < 2) { env(sh, argc, argv); return; }
    int i = 1;
    bool exp = false, erase = false;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strchr(argv[i], 'x')) exp = true;
        if (strchr(argv[i], 'e')) erase = true;
    }
    if (i >= argc) return;
    if (erase) { sh.unset_var(argv[i]); return; }
    char val[256] = "";
    for (int k = i + 1; k < argc; k++) { strlcat(val, argv[k], sizeof(val)); if (k < argc - 1) strlcat(val, " ", sizeof(val)); }
    sh.set_var(argv[i], val, exp);
}

static void alias(Shell& sh, int argc, char** argv) {
    if (argc < 2) {
        for (int i = 0; i < sh.aliases.len; i++)
            sh.outf(sh.flavor == FL_FISH ? "alias %s '%s'\n" : "alias %s='%s'\n", sh.aliases[i].name, sh.aliases[i].value);
        return;
    }
    char* eq = strchr(argv[1], '=');
    if (eq) { *eq = 0; sh.set_alias(argv[1], eq + 1); return; }
    if (argc >= 3) {
        // fish style: alias name 'command'
        char val[256] = "";
        for (int k = 2; k < argc; k++) { strlcat(val, argv[k], sizeof(val)); if (k < argc - 1) strlcat(val, " ", sizeof(val)); }
        sh.set_alias(argv[1], val);
        return;
    }
    const char* a = sh.get_alias(argv[1]);
    if (a) sh.outf("alias %s='%s'\n", argv[1], a);
    else sh.err("alias: %s: not found\n", argv[1]);
}

static void unalias(Shell& sh, int argc, char** argv) { for (int i = 1; i < argc; i++) sh.unset_alias(argv[i]); }

static void source(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: source <file>\n"); return; }
    FsNode* f = need(sh, argv[1], false, true);
    if (f) sh.source_file(f);
}

static void which(Shell& sh, int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const char* a = sh.get_alias(argv[i]);
        if (a) sh.outf("%s: aliased to %s\n", argv[i], a);
        else if (find_command(argv[i])) sh.outf("%s: shell built-in command\n", argv[i]);
        else sh.err("%s not found\n", argv[i]);
    }
}

static void true_(Shell& sh, int, char**) { sh.status = 0; }
static void false_(Shell& sh, int, char**) { sh.status = 1; }

static void sleep_(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: sleep <seconds>\n"); return; }
    sh.prog_keep_screen = true;
    sh.start_program(make_sleep(sh.con, (u32)atoi(argv[1]) * 1000));
}

static void take(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: take <dir>\n"); return; }
    FsNode* d = fs_lookup(argv[1], sh.cwd);
    if (!d) d = fs_create_path(argv[1], true, sh.cwd);
    if (d && d->dir) sh.cwd = d;
    else sh.err("take: cannot create %s\n", argv[1]);
}

static void open(Shell& sh, int argc, char** argv) {
    if (!sh.host->host_is_ui()) { sh.out("open needs the desktop (type 'ui')\n"); return; }
    if (argc < 2) { sh.out("usage: open <file|dir|app>\n"); return; }
    FsNode* f = fs_lookup(argv[1], sh.cwd);
    if (f) { char p[256]; fs_path(f, p, sizeof(p)); sh.host->shell_request(&sh, SR_OPEN, p); }
    else sh.host->shell_request(&sh, SR_OPEN, argv[1]);
}

static void wallpaper(Shell& sh, int argc, char** argv) {
    sh.host->shell_request(&sh, SR_WALLPAPER, argc > 1 ? argv[1] : nullptr);
}

static void chipsay(Shell& sh, int argc, char** argv) {
    char msg[200] = "";
    for (int i = 1; i < argc; i++) { strlcat(msg, argv[i], sizeof(msg)); if (i < argc - 1) strlcat(msg, " ", sizeof(msg)); }
    if (!msg[0]) strlcpy(msg, "beep boop. i am made of sand.", sizeof(msg));
    int l = (int)strlen(msg);
    sh.out(" " G_TL);
    for (int i = 0; i < l + 2; i++) sh.out(G_H);
    sh.outf(G_TR "\n " G_V " %s " G_V "\n " G_BL G_H G_TT, msg);
    for (int i = 0; i < l; i++) sh.out(G_H);
    sh.out(G_BR "\n");
    sh.out("   " G_V "\n");
    sh.out(C_BMAG "  " G_V G_V G_V G_V "\n");
    sh.out(" " G_H G_FULL G_FULL G_FULL G_FULL G_H "\n");
    sh.out(" " G_H G_FULL C_BWHITE G_DIAM G_DIAM C_BMAG G_FULL G_H "\n");
    sh.out(" " G_H G_FULL G_FULL G_FULL G_FULL G_H "\n");
    sh.out("  " G_V G_V G_V G_V C_RESET "\n");
}

// tiny integer expression evaluator: + - * / % ^ ( )
struct Expr {
    const char* p; bool err = false;
    void ws() { while (*p == ' ') p++; }
    i64 atom() {
        ws();
        if (*p == '(') { p++; i64 v = sum(); ws(); if (*p == ')') p++; else err = true; return v; }
        if (*p == '-') { p++; return -atom(); }
        if (*p < '0' || *p > '9') { err = true; return 0; }
        i64 v = 0;
        if (p[0] == '0' && p[1] == 'x') { p += 2; while (true) { char c = to_lower(*p); if (c >= '0' && c <= '9') v = v * 16 + c - '0'; else if (c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10; else break; p++; } return v; }
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        return v;
    }
    i64 power() { i64 b = atom(); ws(); if (*p == '^') { p++; i64 e = power(); i64 r = 1; while (e-- > 0) r *= b; return r; } return b; }
    i64 prod() {
        i64 v = power();
        for (;;) {
            ws();
            if (*p == '*') { p++; v *= power(); }
            else if (*p == '/' || *p == '%') { char op = *p++; i64 d = power(); if (!d) { err = true; return 0; } v = op == '/' ? v / d : v % d; }
            else return v;
        }
    }
    i64 sum() {
        i64 v = prod();
        for (;;) { ws(); if (*p == '+') { p++; v += prod(); } else if (*p == '-') { p++; v -= prod(); } else return v; }
    }
};

static void calc(Shell& sh, int argc, char** argv) {
    char e[200] = "";
    for (int i = 1; i < argc; i++) strlcat(e, argv[i], sizeof(e));
    if (!e[0]) { sh.out("usage: calc <expression>   e.g. calc (2+3)*7\n"); return; }
    Expr x{e};
    i64 v = x.sum();
    x.ws();
    if (x.err || *x.p) { sh.out(C_BRED "calc: bad expression\n" C_RESET); return; }
    sh.outf("%ld  " C_GRAY "(0x%lx)\n" C_RESET, v, v);
}

static void beep(Shell& sh, int argc, char** argv) {
    speaker_beep(argc > 1 ? atoi(argv[1]) : 880, argc > 2 ? atoi(argv[2]) : 120);
}

static void rain(Shell& sh, int, char**) { sh.start_program(make_rain(sh.con)); }
static void snake(Shell& sh, int, char**) { sh.start_program(make_snake(sh.con)); }
static void life(Shell& sh, int, char**) { sh.start_program(make_life(sh.con)); }

static void fsreset(Shell& sh, int argc, char** argv) {
    if (argc < 2 || strcmp(argv[1], "--yes")) {
        sh.out(C_BYEL "this erases every file. run 'fsreset --yes' to confirm.\n" C_RESET);
        return;
    }
    fs_format();
    settings_load();
    sh.cwd = fs_home();
    sh.out("filesystem reset to factory state\n");
}
};

void cmd_ifconfig(Shell&, int, char**);
void cmd_dhcp(Shell&, int, char**);
void cmd_lspci(Shell&, int, char**);
void cmd_netstat(Shell&, int, char**);
void cmd_ping(Shell&, int, char**);
void cmd_nslookup(Shell&, int, char**);
void cmd_curl(Shell&, int, char**);
void cmd_wget(Shell&, int, char**);

// ---------------------------------------------------------------- the table
const Command commands[] = {
    {"help", "[cmd]", "show commands or help for one", ShellCommands::help},
    {"ls", "[-l] [dir]", "list a directory", ShellCommands::ls},
    {"cd", "[dir]", "change directory (~ = home)", ShellCommands::cd},
    {"pwd", "", "print working directory", ShellCommands::pwd},
    {"tree", "[dir]", "show a directory tree", ShellCommands::tree},
    {"cat", "<file>", "print a file", ShellCommands::cat},
    {"head", "<file> [n]", "first lines of a file", ShellCommands::head},
    {"edit", "<file>", "full-screen text editor", ShellCommands::edit},
    {"write", "<file> <text>", "overwrite a file with text", ShellCommands::write},
    {"append", "<file> <text>", "append a line to a file", ShellCommands::append},
    {"touch", "<file>", "create an empty file", ShellCommands::touch},
    {"mkdir", "<dir>", "create a directory", ShellCommands::mkdir},
    {"cp", "<from> <to>", "copy a file or directory", ShellCommands::cp},
    {"mv", "<from> <to>", "move or rename", ShellCommands::mv},
    {"rm", "[-r] <path>", "delete permanently", ShellCommands::rm},
    {"trash", "[path]", "move to trash / list trash", ShellCommands::trash},
    {"restore", "<name>", "restore from trash", ShellCommands::restore},
    {"emptytrash", "", "empty the trash", ShellCommands::emptytrash},
    {"find", "<name>", "search files by name", ShellCommands::find},
    {"grep", "[-invc] <text> [file]", "search inside files or a pipe", ShellCommands::grep},
    {"sort", "[-rn] [file]", "sort lines", ShellCommands::sort},
    {"uniq", "[-c] [file]", "collapse repeated lines", ShellCommands::uniq},
    {"tail", "[-n N] [file]", "last lines", ShellCommands::tail},
    {"rev", "[file]", "reverse each line", ShellCommands::rev},
    {"tee", "<file>", "copy a pipe into a file", ShellCommands::tee},
    {"seq", "[a] <b>", "print a number sequence", ShellCommands::seq},
    {"wc", "<file>", "count lines and words", ShellCommands::wc},
    {"echo", "<text>", "print text (try: echo hi > f)", ShellCommands::echo},
    {"open", "<file|app>", "open in the desktop", ShellCommands::open},
    {"clear", "", "clear the screen", ShellCommands::clear},
    {"history", "", "show command history", ShellCommands::history},
    {"date", "", "show date and time", ShellCommands::date},
    {"cal", "", "show this month", ShellCommands::cal},
    {"uptime", "", "time since boot", ShellCommands::uptime},
    {"mem", "", "memory usage", ShellCommands::mem},
    {"df", "", "disk usage", ShellCommands::df},
    {"sync", "", "flush files to disk now", ShellCommands::sync},
    {"fetch", "", "system info with style", ShellCommands::fetch},
    {"uname", "[-a]", "system name", ShellCommands::uname},
    {"about", "", "about silicon", ShellCommands::about},
    {"whoami", "", "print user name", ShellCommands::whoami},
    {"user", "[name]", "show or set user name", ShellCommands::user},
    {"hostname", "[name]", "show or set host name", ShellCommands::hostname},
    {"theme", "[name]", "terminal color theme", ShellCommands::theme},
    {"zoom", "", "toggle big terminal font", ShellCommands::zoom},
    {"colors", "", "show the palette", ShellCommands::colors},
    {"wallpaper", "[name]", "list or set the wallpaper", ShellCommands::wallpaper},
    {"calc", "<expr>", "integer calculator", ShellCommands::calc},
    {"chipsay", "<text>", "a chip says something", ShellCommands::chipsay},
    {"beep", "[hz] [ms]", "pc speaker", ShellCommands::beep},
    {"rain", "", "silicon rain (any key quits)", ShellCommands::rain},
    {"snake", "", "play snake", ShellCommands::snake},
    {"life", "", "game of life", ShellCommands::life},
    {"ifconfig", "", "network address and stats", cmd_ifconfig},
    {"ping", "[-c n] <host>", "check a host answers", cmd_ping},
    {"nslookup", "<host>", "look up an address (DNS)", cmd_nslookup},
    {"curl", "[-I] [-o f] <url>", "fetch a web page", cmd_curl},
    {"wget", "[-O f] <url>", "download a file", cmd_wget},
    {"netstat", "", "open connections", cmd_netstat},
    {"dhcp", "", "ask for a new address", cmd_dhcp},
    {"lspci", "", "list PCI devices", cmd_lspci},
    {"ui", "", "start the desktop", ShellCommands::ui},
    {"exit", "", "leave a subshell / close terminal", ShellCommands::exit},
    {"bash", "[script]", "switch to bash", ShellCommands::bash},
    {"zsh", "[script]", "switch to zsh", ShellCommands::zsh},
    {"fish", "[script]", "switch to fish", ShellCommands::fish},
    {"chsh", "[-s] <shell>", "set the default shell", ShellCommands::chsh},
    {"export", "[NAME=value]", "export a variable", ShellCommands::export_},
    {"unset", "<NAME>", "remove a variable", ShellCommands::unset},
    {"set", "[-x|-e] NAME value", "fish-style variables", ShellCommands::set},
    {"env", "", "list variables", ShellCommands::env},
    {"alias", "[name=cmd]", "list or define aliases", ShellCommands::alias},
    {"unalias", "<name>", "remove an alias", ShellCommands::unalias},
    {"source", "<file>", "run commands from a file", ShellCommands::source},
    {"which", "<name>", "what a command is", ShellCommands::which},
    {"true", "", "succeed", ShellCommands::true_},
    {"false", "", "fail", ShellCommands::false_},
    {"sleep", "<sec>", "wait", ShellCommands::sleep_},
    {"take", "<dir>", "mkdir + cd (zsh)", ShellCommands::take},
    {"reboot", "", "restart the machine", ShellCommands::reboot},
    {"setup", "", "open the firmware setup utility", ShellCommands::setup},
    {"shutdown", "", "power off", ShellCommands::shutdown},
    {"fsreset", "--yes", "erase all files", ShellCommands::fsreset},
};
const int command_count = ARRAY_LEN(commands);

const Command* find_command(const char* name) {
    for (int i = 0; i < command_count; i++)
        if (!strcmp(commands[i].name, name)) return &commands[i];
    if (!strcmp(name, "neofetch")) return find_command("fetch");
    if (!strcmp(name, "dir")) return find_command("ls");
    if (!strcmp(name, "cls")) return find_command("clear");
    if (!strcmp(name, "poweroff")) return find_command("shutdown");
    if (!strcmp(name, "nano") || !strcmp(name, "vi") || !strcmp(name, "vim")) return find_command("edit");
    if (!strcmp(name, ".")) return find_command("source");
    if (!strcmp(name, "type")) return find_command("which");
    if (!strcmp(name, "printenv")) return find_command("env");
    if (!strcmp(name, "abbr")) return find_command("alias");
    if (!strcmp(name, "host") || !strcmp(name, "dig")) return find_command("nslookup");
    if (!strcmp(name, "ip")) return find_command("ifconfig");
    return nullptr;
}
