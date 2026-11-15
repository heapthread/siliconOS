// win32run - start-up: read the command line, load the program, give it a stack, and call its entry point.
//
//     win32run [--strict] [--trace] program.exe [arguments...]
#include "rt.h"

namespace rt {
extern "C" RUNTIME_FUNCTION __rt_pdata_start[], __rt_pdata_end[];
void mod_exe_tls_callbacks(u32 reason);

// the runtime's own unwind table has to be sorted for the binary search; the linker mostly does that
static void sort_pdata() {
    RUNTIME_FUNCTION* a = __rt_pdata_start;
    int n = (int)(__rt_pdata_end - __rt_pdata_start);
    for (int i = 1; i < n; i++) {
        RUNTIME_FUNCTION k = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].BeginAddress > k.BeginAddress) { a[j + 1] = a[j]; j--; }
        a[j + 1] = k;
    }
}

typedef u32 (WINAPI *EntryFn)(PEB* peb);

static void start_program(void*) {
    mod_init_all();                                         // DllMain of the DLLs it brought along
    mod_exe_tls_callbacks(DLL_PROCESS_ATTACH);
    trace("calling the entry point of %s", exe_module->name);
    u32 code = ((EntryFn)exe_module->entry)(peb);
    process_exit(code);
}

// the next word of a command line, quotes removed; returns where the word started
static const char* next_word(const char*& p, char* out, usize cap) {
    while (*p == ' ' || *p == '\t') p++;
    const char* start = p;
    usize n = 0;
    bool q = false;
    while (*p && (q || (*p != ' ' && *p != '\t'))) {
        if (*p == '"') { q = !q; p++; continue; }
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return start;
}

}  // namespace rt

using namespace rt;

extern "C" int app_main() {
    sort_pdata();
    char cmd[2048];
    si::proc_info(0, cmd, sizeof(cmd));
    si::proc_info(1, cur_dir, sizeof(cur_dir));
    if (!cur_dir[0]) scpy(cur_dir, "/home");

    // options, then the program, then its arguments (passed on exactly as typed)
    const char* p = cmd;
    char word[MAX_PATH];
    char exe_arg[MAX_PATH] = "";
    const char* args = "";
    while (*p) {
        next_word(p, word, sizeof(word));
        if (!word[0]) break;
        if (!scmp(word, "--strict")) opt_strict = true;
        else if (!scmp(word, "--trace")) opt_trace = true;
        else if (!scmp(word, "--help") || !scmp(word, "-h")) {
            msg("usage: win32run [--strict] [--trace] <program.exe> [arguments]");
            return 0;
        } else {
            slcpy(exe_arg, word, sizeof(exe_arg));
            while (*p == ' ' || *p == '\t') p++;
            args = p;
            break;
        }
    }
    if (!exe_arg[0]) { msg("usage: win32run [--strict] [--trace] <program.exe> [arguments]"); return 1; }

    char path[MAX_PATH];
    si::Stat st;
    if (!path_to_silicon(exe_arg, path, sizeof(path)) || si::file_stat(path, &st) != 0) {
        char with_ext[MAX_PATH];
        snfmt(with_ext, sizeof(with_ext), "%s.exe", exe_arg);
        if (!path_to_silicon(with_ext, path, sizeof(path)) || si::file_stat(path, &st) != 0) {
            msg("%s: no such file", exe_arg);
            return 1;
        }
    }
    if (st.flags & 1) { msg("%s is a folder", exe_arg); return 1; }

    handles_init();
    // the command line Windows programs see: the program's full path in quotes, then the arguments
    char winpath[MAX_PATH], line[2400];
    path_to_windows(path, winpath, sizeof(winpath));
    snfmt(line, sizeof(line), "\"%s\"%s%s", winpath, args[0] ? " " : "", args);
    WCHAR* wline = wide_dup(line);
    env_init(path, wline);
    install_fault_handler();
    trace("loading %s", path);

    int err = 0;
    Module* m = mod_load_exe(path, &err);
    if (!m) return 1;
    if (m->is_dll) { msg("%s is a DLL, not a program", exe_arg); return 1; }
    if (!m->entry) { msg("%s has no entry point", exe_arg); return 1; }

    // the main thread's stack: what the program asked for, within reason
    usize ssize = m->nt->OptionalHeader.SizeOfStackReserve;
    if (ssize < (1u << 20)) ssize = 1u << 20;
    if (ssize > (16u << 20)) ssize = 16u << 20;
    u8* stack = (u8*)vm_alloc(nullptr, ssize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!stack) { msg("no memory for the program's stack"); return 1; }
    Thr* t = cur_thr();
    t->stack = stack;
    t->stack_size = ssize;
    t->teb->StackBase = stack + ssize;
    t->teb->StackLimit = stack;
    t->teb->DeallocationStack = stack;
    tls_setup_thread(t);

    rt_call_on_stack((void*)&start_program, nullptr, stack + ssize - 64);
    return 0;
}
