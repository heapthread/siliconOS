// silicon - process shell commands: ps, kill, run
#include "shell.h"
#include "../proc/proc.h"
#include "../text.h"

void uapp_run(const char* path);

void cmd_ps(Shell& sh, int, char**) {
    sh.out(C_BWHITE "  pid  threads  memory    cpu      name\n" C_RESET);
    sh.outf("   0        1  kernel    %3d.%d%%   silicon (the kernel thread)\n", 100 - proc_cpu_permille() / 10, (1000 - proc_cpu_permille()) % 10 > 0 ? 0 : 0);
    for (int i = 0; i < proc_count(); i++) {
        Proc* p = proc_at(i);
        u64 up = ticks() - p->start_ms + 1;
        u64 permille = p->cpu_tsc / max<u64>(1, (u64)cpu_mhz() * up);
        sh.outf("  %3d  %7d  %5u KB  %3u.%u%%   %s%s\n", p->pid, p->threads.len, p->mem_pages * 4, (u32)(permille / 10), (u32)(permille % 10), p->name,
                p->dead ? C_GRAY "  (ended)" C_RESET : "");
    }
}

void cmd_kill(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: kill <pid>\n"); return; }
    Proc* p = proc_by_pid(atoi(argv[1]));
    if (!p || p->dead) { sh.err("no such process\n"); return; }
    proc_kill(p, "killed");
    sh.outf("process %d (%s) ended\n", p->pid, p->name);
}

// "a b" "c" -> one command line, with quotes around what needs them
static void join_args(int argc, char** argv, char* out, int cap) {
    out[0] = 0;
    for (int i = 0; i < argc; i++) {
        bool q = !argv[i][0] || strchr(argv[i], ' ') || strchr(argv[i], '\t');
        if (i) strlcat(out, " ", cap);
        if (q) strlcat(out, "\"", cap);
        strlcat(out, argv[i], cap);
        if (q) strlcat(out, "\"", cap);
    }
}

void cmd_run(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: run <program.uapp> [arguments]      (see sdk/build_uapp.py; samples are in /home/apps)\n"); return; }
    char path[200], cwd[160], cmd[512];
    fs_path(sh.cwd, cwd, sizeof(cwd));
    if (argv[1][0] == '/') strlcpy(path, argv[1], sizeof(path));
    else snprintf(path, sizeof(path), "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", argv[1]);
    join_args(argc - 1, argv + 1, cmd, sizeof(cmd));
    char err[96];
    Proc* p = proc_spawn(path, err, sizeof(err), cmd, cwd);
    if (!p) { sh.err("%s\n", err); return; }
    sh.outf("started %s as process %d (it opens its own window in the desktop)\n", p->name, p->pid);
}

// ---------------------------------------------------------------- win32run: Windows programs
// The Win32 runtime is an ordinary ring 3 program (sdk/win32, built into the kernel image as an asset).
// It loads the .exe into its own address space and stands in for the Windows system DLLs.
const u8* asset_find(const char* name, u32* len);

Proc* win32_spawn(const char* cmdline, const char* cwd, bool with_console, char* err, int errlen) {
    u32 len = 0;
    const u8* img = asset_find("uapp/win32run", &len);
    if (!img) { snprintf(err, errlen, "the Win32 runtime is not part of this build"); return nullptr; }
    // the process is named after the program it runs: the first word that isn't an option
    char name[48] = "win32run";
    const char* s = cmdline;
    while (*s) {
        while (*s == ' ') s++;
        bool q = *s == '"';
        const char* a = q ? s + 1 : s;
        const char* e = a;
        while (*e && (q ? *e != '"' : *e != ' ')) e++;
        if (a[0] != '-' || q) {
            const char* b = a;
            for (const char* c = a; c < e; c++) if (*c == '/' || *c == '\\') b = c + 1;
            if (e > b) strlcpy(name, b, min<size_t>(sizeof(name), (size_t)(e - b) + 1));
            break;
        }
        s = *e ? e + 1 : e;
    }
    return proc_spawn_image(img, len, name, "/system/win32run", cmdline, cwd, with_console, err, errlen);
}

// the Windows test programs built into the kernel image go to /home/win32 (only when changed)
struct AssetEntry { const char* name; const u8* data; u64 len; };
extern "C" AssetEntry asset_table[];
extern "C" u64 asset_count;
void win32_install_samples() {
    for (u64 i = 0; i < asset_count; i++) {
        AssetEntry& e = asset_table[i];
        if (!str_starts(e.name, "exe/")) continue;
        char path[96];
        snprintf(path, sizeof(path), "/home/win32/%s", e.name + 4);
        FsNode* have = fs_lookup(path);
        if (have && have->size == (u32)e.len && !memcmp(have->data, e.data, (u32)e.len)) continue;
        fs_mkdirs("/home/win32");
        FsNode* f = have ? have : fs_create_path(path, false);
        if (f) fs_write(f, e.data, (u32)e.len);
    }
}

// the shell side of a program's console: shows what it prints, sends it the lines typed, ctrl+c ends it
struct ProcConsole : ConsoleProgram {
    Shell& sh;
    int pid;
    char line[256];
    int len = 0;
    ProcConsole(Shell& s, int pid_) : sh(s), pid(pid_) {}
    ~ProcConsole() { Proc* p = proc_by_pid(pid); if (p) p->con_hold = false; }
    void on_key(const KeyEvent& e) override {
        Proc* p = proc_by_pid(pid);
        if (!p || p->dead) return;
        if (e.ctrl && (e.key == 'c' || e.key == 'C')) { sh.out("^C\n"); proc_kill(p, "interrupted"); return; }
        if (e.ctrl && (e.key == 'd' || e.key == 'D' || e.key == 'z' || e.key == 'Z')) { proc_con_input(p, nullptr, 0); return; }
        if (e.key == K_ENTER) {
            line[len++] = '\n';
            proc_con_input(p, line, len);
            len = 0;
            sh.out("\n");
        } else if (e.key == K_BACKSPACE) {
            if (len) { len--; sh.out("\b \b"); }
        } else if (e.ch && (u8)e.ch >= 32 && len < (int)sizeof(line) - 2) {
            line[len++] = e.ch;
            char two[2] = {e.ch, 0};
            sh.out(two);
        }
    }
    void pump(Proc* p) {
        char raw[1024], sil[1100];
        for (int rounds = 0; rounds < 8; rounds++) {
            int n = proc_con_take(p, raw, sizeof(raw) - 1);
            if (!n) break;
            raw[n] = 0;
            utf8_to_silicon(raw, (u32)n, sil, sizeof(sil));
            sh.out(sil);
        }
    }
    void tick() override { Proc* p = proc_by_pid(pid); if (p) pump(p); }
    bool done() override {
        Proc* p = proc_by_pid(pid);
        if (!p) return true;
        if (!p->dead || p->co_head != p->co_tail) return false;
        if (p->why[0] && strcmp(p->why, "finished") && strcmp(p->why, "exited with an error") && strcmp(p->why, "interrupted"))
            sh.outf(C_BRED "%s ended: %s" C_RESET "\n", p->name, p->why);
        sh.status = p->exit_code;
        p->con_hold = false;
        return true;
    }
};

void cmd_win32run(Shell& sh, int argc, char** argv) {
    if (argc < 2) {
        sh.out("usage: win32run [--strict] [--trace] <program.exe> [arguments]\n"
               "runs a 64-bit Windows program. --strict stops at the first Windows function that is missing,\n"
               "--trace lists every module and import while loading.\n");
        return;
    }
    char cwd[160], cmd[512], err[96];
    fs_path(sh.cwd, cwd, sizeof(cwd));
    join_args(argc - 1, argv + 1, cmd, sizeof(cmd));
    Proc* p = win32_spawn(cmd, cwd, true, err, sizeof(err));
    if (!p) { sh.err("win32run: %s\n", err); return; }
    sh.prog_keep_screen = true;
    sh.start_program(new ProcConsole(sh, p->pid));
}
