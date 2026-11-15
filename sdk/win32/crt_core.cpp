// win32run - the C runtime: start-up and exit, errno, the heap functions, sorting, the environment,
// paths, threads, signals, floating-point control and setjmp/longjmp.
#include "crt.h"

namespace rt {

// ---------------------------------------------------------------- per-thread state
CrtThread* crt_thread() {
    Thr* t = cur_thr();
    if (!t->crt) {
        CrtThread* c = (CrtThread*)izalloc(sizeof(CrtThread));
        c->rand_seed = 1;
        t->crt = c;
    }
    return (CrtThread*)t->crt;
}

int errno_from_win32(u32 e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME: case ERROR_MOD_NOT_FOUND: return ENOENT;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_ACCESS_DENIED: case ERROR_WRITE_PROTECT: case ERROR_SHARING_VIOLATION: return EACCES;
    case ERROR_INVALID_HANDLE: return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return ENOMEM;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return EEXIST;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_DISK_FULL: return ENOSPC;
    case ERROR_BROKEN_PIPE: return EPIPE;
    case ERROR_NEGATIVE_SEEK: return EINVAL;
    case 267: return ENOTDIR;
    }
    return EINVAL;
}
void set_errno_from_win32() { u32 e = get_error(); crt_thread()->doserr = e; set_errno(errno_from_win32(e)); }

static int* CDECL c__errno() { return errno_ptr(); }
static u32* CDECL c___doserrno() { return &crt_thread()->doserr; }
static int CDECL c__get_errno(int* v) { if (!v) return EINVAL; *v = *errno_ptr(); return 0; }
static int CDECL c__set_errno(int v) { set_errno(v); return 0; }
static int CDECL c__get_doserrno(u32* v) { if (!v) return EINVAL; *v = crt_thread()->doserr; return 0; }
static int CDECL c__set_doserrno(u32 v) { crt_thread()->doserr = v; return 0; }

// ---------------------------------------------------------------- invalid parameters, assertions, abort
typedef void (WINAPI *InvalidParamHandler)(const WCHAR*, const WCHAR*, const WCHAR*, u32, u64);
static InvalidParamHandler ip_handler = nullptr;
static InvalidParamHandler CDECL c__set_invalid_parameter_handler(InvalidParamHandler h) { InvalidParamHandler o = ip_handler; ip_handler = h; return o; }
static InvalidParamHandler CDECL c__get_invalid_parameter_handler() { return ip_handler; }
static InvalidParamHandler CDECL c__set_thread_local_invalid_parameter_handler(InvalidParamHandler h) { return c__set_invalid_parameter_handler(h); }
static InvalidParamHandler CDECL c__get_thread_local_invalid_parameter_handler() { return ip_handler; }
void invalid_parameter() {
    if (ip_handler) { ip_handler(nullptr, nullptr, nullptr, 0, 0); return; }
    // the C runtime's default: report and end the program (Windows would show a dialog)
    char b[96];
    msg("the C runtime was given an invalid parameter (called from %s)", describe_addr(__builtin_return_address(0), b, sizeof(b)));
    process_exit(0xC0000417);
}
static void CDECL c__invalid_parameter(const WCHAR* e, const WCHAR* f, const WCHAR* file, u32 line, u64 r) {
    if (ip_handler) { ip_handler(e, f, file, line, r); return; }
    invalid_parameter();
}
static void CDECL c__invalid_parameter_noinfo() { invalid_parameter(); }
static void CDECL c__invalid_parameter_noinfo_noreturn() { invalid_parameter(); process_exit(0xC0000417); }
static void CDECL c__invoke_watson(const WCHAR* e, const WCHAR* f, const WCHAR* file, u32 line, u64 r) { (void)e; (void)f; (void)file; (void)line; (void)r; process_exit(0xC0000417); }

typedef void (CDECL *SigHandler)(int);
static SigHandler sig_handlers[32];
static int abort_flags = 1;

static int CDECL c_raise(int sig);
[[noreturn]] static void CDECL c_abort() {
    if (sig_handlers[22] && sig_handlers[22] != (SigHandler)1) c_raise(22);     // SIGABRT
    if (abort_flags & 1) msg("abort() was called: the program ended itself");
    extern void flush_all_streams();
    flush_all_streams();
    files_flush_all();
    si::exit(3);
    for (;;) {}
}
static u32 CDECL c__set_abort_behavior(u32 flags, u32 mask) { u32 o = (u32)abort_flags; abort_flags = (int)((abort_flags & ~mask) | (flags & mask)); return o; }

static void CDECL c__wassert(const WCHAR* m, const WCHAR* file, u32 line) {
    char a[400], b[300];
    utf8_from_wide(m, -1, a, sizeof(a));
    utf8_from_wide(file, -1, b, sizeof(b));
    char line_s[800];
    int n = snfmt(line_s, sizeof(line_s), "Assertion failed: %s, file %s, line %u\n", a, b, line);
    con_out(line_s, (usize)n);
    c_abort();
}
static void CDECL c__assert(const char* m, const char* file, u32 line) {
    char line_s[800];
    int n = snfmt(line_s, sizeof(line_s), "Assertion failed: %s, file %s, line %u\n", m, file, line);
    con_out(line_s, (usize)n);
    c_abort();
}
static void CDECL c__purecall() { msg("pure virtual function call"); c_abort(); }
typedef void (CDECL *PurecallHandler)();
static PurecallHandler CDECL c__set_purecall_handler(PurecallHandler h) { (void)h; return nullptr; }
static PurecallHandler CDECL c__get_purecall_handler() { return nullptr; }
static void CDECL c_terminate() { msg("terminate() was called"); c_abort(); }
typedef void (CDECL *TermHandler)();
static TermHandler term_handler = nullptr;
static TermHandler CDECL c_set_terminate(TermHandler h) { TermHandler o = term_handler; term_handler = h; return o; }
static TermHandler CDECL c__get_terminate() { return term_handler; }
static void CDECL c__amsg_exit(int code) { msg("runtime error %d", code); c_abort(); }
static void CDECL c__crt_debugger_hook(int x) { (void)x; }

// ---------------------------------------------------------------- signals
static SigHandler CDECL c_signal(int sig, SigHandler h) {
    if (sig < 0 || sig >= 32) { set_errno(EINVAL); return (SigHandler)(i64)-1; }
    SigHandler o = sig_handlers[sig];
    sig_handlers[sig] = h;
    return o;
}
static int CDECL c_raise(int sig) {
    if (sig < 0 || sig >= 32) { set_errno(EINVAL); return -1; }
    SigHandler h = sig_handlers[sig];
    if (h == (SigHandler)1) return 0;                           // SIG_IGN
    if (!h) {                                                   // SIG_DFL
        if (sig == 22) { if (abort_flags & 1) msg("abort() was called"); }
        else msg("signal %d raised", sig);
        files_flush_all();
        si::exit(3);
    }
    sig_handlers[sig] = nullptr;
    h(sig);
    return 0;
}
static void* CDECL c___pxcptinfoptrs() { static void* p; return &p; }
static int CDECL c___fpecode() { static int c; return c; }
static int* CDECL c___fpecode_ptr() { static int c; return &c; }

// ---------------------------------------------------------------- arguments
int c_argc = 0;
char** c_argv = nullptr;
WCHAR** c_wargv = nullptr;
static char* c_acmdln = nullptr;
static WCHAR* c_wcmdln = nullptr;
static char* c_pgmptr = nullptr;
static WCHAR* c_wpgmptr = nullptr;

// how Windows splits a command line (CommandLineToArgvW): spaces separate, quotes group, 2n backslashes
// before a quote are n backslashes, 2n+1 are n and a literal quote; the first word ends at a quote or space
int split_cmdline(const WCHAR* p, WCHAR** out, WCHAR* buf, int* nchars) {
    int argc = 0, o = 0;
    // the program name: no escapes
    while (*p == ' ' || *p == '\t') p++;
    if (*p) {
        if (out) out[argc] = buf + o;
        if (*p == '"') { p++; while (*p && *p != '"') { if (buf) buf[o] = *p; o++; p++; } if (*p) p++; }
        else while (*p && *p != ' ' && *p != '\t') { if (buf) buf[o] = *p; o++; p++; }
        if (buf) buf[o] = 0;
        o++;
        argc++;
    }
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (out) out[argc] = buf + o;
        bool q = false;
        while (*p && (q || (*p != ' ' && *p != '\t'))) {
            if (*p == '\\') {
                int bs = 0;
                while (*p == '\\') { bs++; p++; }
                if (*p == '"') {
                    for (int i = 0; i < bs / 2; i++) { if (buf) buf[o] = '\\'; o++; }
                    if (bs & 1) { if (buf) buf[o] = '"'; o++; p++; }
                } else for (int i = 0; i < bs; i++) { if (buf) buf[o] = '\\'; o++; }
            } else if (*p == '"') {
                if (q && p[1] == '"') { if (buf) buf[o] = '"'; o++; p += 2; continue; }    // "" inside quotes
                q = !q;
                p++;
            } else { if (buf) buf[o] = *p; o++; p++; }
        }
        if (buf) buf[o] = 0;
        o++;
        argc++;
    }
    if (nchars) *nchars = o;
    return argc;
}

void crt_args_init() {
    if (c_argv) return;
    int chars = 0;
    int n = split_cmdline(cmdline_w, nullptr, nullptr, &chars);
    c_wargv = (WCHAR**)h_alloc(sizeof(WCHAR*) * (usize)(n + 1));
    WCHAR* wbuf = (WCHAR*)h_alloc(sizeof(WCHAR) * (usize)(chars + 1));
    split_cmdline(cmdline_w, c_wargv, wbuf, nullptr);
    c_wargv[n] = nullptr;
    c_argc = n;
    c_argv = (char**)h_alloc(sizeof(char*) * (usize)(n + 1));
    for (int i = 0; i < n; i++) {
        int b = utf8_from_wide(c_wargv[i], -1, nullptr, 0);
        c_argv[i] = (char*)h_alloc((usize)b);
        utf8_from_wide(c_wargv[i], -1, c_argv[i], b);
    }
    c_argv[n] = nullptr;
    c_acmdln = cmdline_a;
    c_wcmdln = cmdline_w;
    c_pgmptr = utf8_dup(exe_module->wpath);
    c_wpgmptr = wdup(exe_module->wpath);
}

// ---------------------------------------------------------------- the environment
char** c_environ_arr = nullptr;
WCHAR** c_wenviron_arr = nullptr;
static char** initial_env = nullptr;
static WCHAR** initial_wenv = nullptr;
void crt_env_refresh() {
    Guard g(env_lock);
    int n = 0;
    for (const WCHAR* p = env_block; *p; p += wlen(p) + 1) if (*p != '=') n++;
    char** a = (char**)h_alloc(sizeof(char*) * (usize)(n + 1));
    WCHAR** w = (WCHAR**)h_alloc(sizeof(WCHAR*) * (usize)(n + 1));
    int i = 0;
    for (const WCHAR* p = env_block; *p; p += wlen(p) + 1) {
        if (*p == '=') continue;
        w[i] = wdup(p);
        a[i] = utf8_dup(p);
        i++;
    }
    a[n] = nullptr;
    w[n] = nullptr;
    c_environ_arr = a;
    c_wenviron_arr = w;
    if (!initial_env) { initial_env = a; initial_wenv = w; }
}

static char* CDECL c_getenv(const char* name) {
    if (!c_environ_arr) crt_env_refresh();
    usize n = slen(name);
    for (char** e = c_environ_arr; *e; e++) if (!snicmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
    return nullptr;
}
static WCHAR* CDECL c__wgetenv(const WCHAR* name) {
    if (!c_wenviron_arr) crt_env_refresh();
    usize n = wlen(name);
    for (WCHAR** e = c_wenviron_arr; *e; e++) {
        bool eq = true;
        for (usize i = 0; i < n; i++) if (lower((*e)[i]) != lower(name[i])) { eq = false; break; }
        if (eq && (*e)[n] == '=') return *e + n + 1;
    }
    return nullptr;
}
static int CDECL c_getenv_s(usize* req, char* buf, usize cap, const char* name) {
    const char* v = c_getenv(name);
    if (!v) { *req = 0; if (buf && cap) buf[0] = 0; return 0; }
    *req = slen(v) + 1;
    if (!buf) return 0;
    if (cap < *req) return ERANGE;
    memcpy(buf, v, *req);
    return 0;
}
static int CDECL c__wgetenv_s(usize* req, WCHAR* buf, usize cap, const WCHAR* name) {
    const WCHAR* v = c__wgetenv(name);
    if (!v) { *req = 0; if (buf && cap) buf[0] = 0; return 0; }
    *req = wlen(v) + 1;
    if (!buf) return 0;
    if (cap < *req) return ERANGE;
    memcpy(buf, v, *req * 2);
    return 0;
}
static int CDECL c__dupenv_s(char** out, usize* len, const char* name) {
    const char* v = c_getenv(name);
    if (!v) { *out = nullptr; if (len) *len = 0; return 0; }
    usize n = slen(v) + 1;
    *out = (char*)h_alloc(n);
    memcpy(*out, v, n);
    if (len) *len = n;
    return 0;
}
static int CDECL c__wdupenv_s(WCHAR** out, usize* len, const WCHAR* name) {
    const WCHAR* v = c__wgetenv(name);
    if (!v) { *out = nullptr; if (len) *len = 0; return 0; }
    usize n = wlen(v) + 1;
    *out = (WCHAR*)h_alloc(n * 2);
    memcpy(*out, v, n * 2);
    if (len) *len = n;
    return 0;
}
static int CDECL c__wputenv(const WCHAR* s) {
    const WCHAR* eq = s;
    while (*eq && *eq != '=') eq++;
    if (!*eq || eq == s) { set_errno(EINVAL); return -1; }
    WCHAR name[256];
    usize n = (usize)(eq - s);
    if (n > 255) n = 255;
    memcpy(name, s, n * 2);
    name[n] = 0;
    { Guard g(env_lock); env_set(name, eq[1] ? eq + 1 : nullptr); }
    crt_env_refresh();
    return 0;
}
static int CDECL c__putenv(const char* s) { WCHAR* w = wide_dup(s); int r = c__wputenv(w); ifree(w); return r; }
static int CDECL c__putenv_s(const char* name, const char* value) {
    WCHAR* n = wide_dup(name);
    WCHAR* v = wide_dup(value);
    { Guard g(env_lock); env_set(n, v[0] ? v : nullptr); }
    ifree(n); ifree(v);
    crt_env_refresh();
    return 0;
}
static int CDECL c__wputenv_s(const WCHAR* name, const WCHAR* value) {
    { Guard g(env_lock); env_set(name, value[0] ? value : nullptr); }
    crt_env_refresh();
    return 0;
}
static char*** CDECL c___p__environ() { if (!c_environ_arr) crt_env_refresh(); return &c_environ_arr; }
static WCHAR*** CDECL c___p__wenviron() { if (!c_wenviron_arr) crt_env_refresh(); return &c_wenviron_arr; }
static char** CDECL c__get_initial_narrow_environment() { if (!c_environ_arr) crt_env_refresh(); return initial_env; }
static WCHAR** CDECL c__get_initial_wide_environment() { if (!c_wenviron_arr) crt_env_refresh(); return initial_wenv; }
static int CDECL c__initialize_narrow_environment() { crt_env_refresh(); return 0; }
static int CDECL c__initialize_wide_environment() { crt_env_refresh(); return 0; }

// ---------------------------------------------------------------- start-up (both C runtimes' entry protocols)
static int CDECL c__configure_narrow_argv(int mode) { (void)mode; crt_args_init(); return 0; }
static int CDECL c__configure_wide_argv(int mode) { (void)mode; crt_args_init(); return 0; }
static int* CDECL c___p___argc() { crt_args_init(); return &c_argc; }
static char*** CDECL c___p___argv() { crt_args_init(); return &c_argv; }
static WCHAR*** CDECL c___p___wargv() { crt_args_init(); return &c_wargv; }
static char** CDECL c___p__acmdln() { crt_args_init(); return &c_acmdln; }
static WCHAR** CDECL c___p__wcmdln() { crt_args_init(); return &c_wcmdln; }
static char** CDECL c___p__pgmptr() { crt_args_init(); return &c_pgmptr; }
static WCHAR** CDECL c___p__wpgmptr() { crt_args_init(); return &c_wpgmptr; }
static int CDECL c__get_pgmptr(char** p) { crt_args_init(); *p = c_pgmptr; return 0; }
static int CDECL c__get_wpgmptr(WCHAR** p) { crt_args_init(); *p = c_wpgmptr; return 0; }
struct StartupInfo { int newmode; };
static int CDECL c___getmainargs(int* argc, char*** argv, char*** env, int glob, StartupInfo* si_) {
    (void)glob; (void)si_;
    crt_args_init();
    if (!c_environ_arr) crt_env_refresh();
    *argc = c_argc; *argv = c_argv; *env = c_environ_arr;
    return 0;
}
static int CDECL c___wgetmainargs(int* argc, WCHAR*** argv, WCHAR*** env, int glob, StartupInfo* si_) {
    (void)glob; (void)si_;
    crt_args_init();
    if (!c_wenviron_arr) crt_env_refresh();
    *argc = c_argc; *argv = c_wargv; *env = c_wenviron_arr;
    return 0;
}
static int app_type = 1;
static void CDECL c__set_app_type(int t) { app_type = t; }
static int CDECL c__get_app_type() { return app_type; }
static int c_fmode = 0x4000, c_commode = 0;                    // _O_TEXT
static int* CDECL c___p__fmode() { return &c_fmode; }
int* fmode_ptr() { return &c_fmode; }
static int* CDECL c___p__commode() { return &c_commode; }
static int CDECL c__set_fmode(int m) { c_fmode = m; return 0; }
static int CDECL c__get_fmode(int* m) { *m = c_fmode; return 0; }
static int new_mode = 0;
static int CDECL c__set_new_mode(int m) { int o = new_mode; new_mode = m; return o; }
static int CDECL c__query_new_mode() { return new_mode; }
typedef int (CDECL *NewHandler)(usize);
static NewHandler CDECL c__set_new_handler(NewHandler h) { (void)h; return nullptr; }
static NewHandler CDECL c__query_new_handler() { return nullptr; }
static int CDECL c__configthreadlocale(int m) { (void)m; return 2; }   // _DISABLE_PER_THREAD_LOCALE
static void CDECL c___setusermatherr(void* f) { (void)f; }
static void CDECL c__setusermatherr(void* f) { (void)f; }
static int CDECL c__matherr(void* e) { (void)e; return 0; }
static int CDECL c__seh_filter_exe(u32 code, EXCEPTION_POINTERS* ep) { (void)code; (void)ep; return 0; }   // EXCEPTION_CONTINUE_SEARCH
static int CDECL c__seh_filter_dll(u32 code, EXCEPTION_POINTERS* ep) { (void)code; (void)ep; return 0; }
static int CDECL c__XcptFilter(u32 code, EXCEPTION_POINTERS* ep) { (void)code; (void)ep; return 0; }
static void** CDECL c___current_exception() { static void* p; return &p; }
static void** CDECL c___current_exception_context() { static void* p; return &p; }
static int* CDECL c___processing_throw() { static int n; return &n; }
static int CDECL c__is_c_termination_complete() { return 0; }
static int CDECL c__register_thread_local_exe_atexit_callback(void* cb) { (void)cb; return 0; }
static void* CDECL c__get_heap_handle() { return (void*)0x10000; }
static int CDECL c___lconv_init() { return 0; }
static void CDECL c__fpreset();

typedef void (CDECL *PVFV)();
typedef int (CDECL *PIFV)();
static void CDECL c__initterm(PVFV* a, PVFV* b) { for (; a < b; a++) if (*a) (*a)(); }
static int CDECL c__initterm_e(PIFV* a, PIFV* b) { for (; a < b; a++) if (*a) { int r = (*a)(); if (r) return r; } return 0; }

// atexit tables (UCRT keeps one for the program and lets DLLs keep their own)
struct OnexitTable { PVFV* first; PVFV* last; PVFV* end; };
static OnexitTable exit_table;
static Lock exit_lock;
static int CDECL c__initialize_onexit_table(OnexitTable* t) { if (!t) return -1; t->first = t->last = t->end = nullptr; return 0; }
static int CDECL c__register_onexit_function(OnexitTable* t, PVFV f) {
    Guard g(exit_lock);
    if (t->last == t->end) {
        usize n = (usize)(t->end - t->first), nn = n ? n * 2 : 32;
        PVFV* a = (PVFV*)h_alloc(nn * sizeof(PVFV));
        if (!a) return -1;
        if (n) memcpy(a, t->first, n * sizeof(PVFV));
        t->first = a; t->last = a + n; t->end = a + nn;
    }
    *t->last++ = f;
    return 0;
}
static int CDECL c__execute_onexit_table(OnexitTable* t) {
    for (;;) {
        PVFV f;
        {
            Guard g(exit_lock);
            if (!t->first || t->last == t->first) break;
            f = *--t->last;
        }
        if (f) f();
    }
    t->first = t->last = t->end = nullptr;
    return 0;
}
static int CDECL c__crt_atexit(PVFV f) { return c__register_onexit_function(&exit_table, f); }
static int CDECL c_atexit(PVFV f) { return c__register_onexit_function(&exit_table, f); }
static int CDECL c__crt_at_quick_exit(PVFV f) { (void)f; return 0; }
static int CDECL c_at_quick_exit(PVFV f) { (void)f; return 0; }
typedef int (CDECL *OnexitFn)();
static OnexitFn CDECL c__onexit(OnexitFn f) { return c__register_onexit_function(&exit_table, (PVFV)f) ? nullptr : f; }
static OnexitFn CDECL c___dllonexit(OnexitFn f, PVFV** begin, PVFV** end) {
    // the table belongs to a DLL; grow it as the caller describes
    usize n = (usize)(*end - *begin);
    PVFV* a = (PVFV*)h_alloc((n + 1) * sizeof(PVFV));
    if (!a) return nullptr;
    if (n) memcpy(a, *begin, n * sizeof(PVFV));
    a[n] = (PVFV)f;
    *begin = a;
    *end = a + n + 1;
    return f;
}

void flush_all_streams();
static volatile int exiting = 0;
[[noreturn]] static void do_exit(int code, bool run_handlers) {
    if (run_handlers && !__sync_lock_test_and_set(&exiting, 1)) c__execute_onexit_table(&exit_table);
    flush_all_streams();
    process_exit((u32)code);
}
[[noreturn]] static void CDECL c_exit(int code) { do_exit(code, true); }
[[noreturn]] static void CDECL c__exit(int code) { files_flush_all(); process_exit((u32)code); }
[[noreturn]] static void CDECL c__Exit(int code) { c__exit(code); }
[[noreturn]] static void CDECL c_quick_exit(int code) { c__exit(code); }
static void CDECL c__cexit() { if (!__sync_lock_test_and_set(&exiting, 1)) c__execute_onexit_table(&exit_table); flush_all_streams(); }
static void CDECL c__c_exit() { flush_all_streams(); }

// ---------------------------------------------------------------- memory
static void* CDECL c_malloc(usize n) { void* p = h_alloc(n); if (!p) set_errno(ENOMEM); return p; }
static void* CDECL c_calloc(usize n, usize s) {
    if (s && n > ~0ull / s) { set_errno(ENOMEM); return nullptr; }
    void* p = h_alloc(n * s, true);
    if (!p) set_errno(ENOMEM);
    return p;
}
static void* CDECL c_realloc(void* p, usize n) {
    if (p && !n) { h_free(p); return nullptr; }
    void* q = h_realloc(p, n);
    if (!q) set_errno(ENOMEM);
    return q;
}
static void CDECL c_free(void* p) { h_free(p); }
static void* CDECL c__recalloc(void* p, usize n, usize s) { return h_realloc(p, n * s, true); }
static usize CDECL c__msize(void* p) { usize s = h_size(p); if (s == (usize)-1) { invalid_parameter(); return (usize)-1; } return s; }
static void* CDECL c__expand(void* p, usize n) { usize s = h_size(p); return s != (usize)-1 && n <= s ? p : nullptr; }
static void* CDECL c__malloc_base(usize n) { return c_malloc(n); }
static void* CDECL c__calloc_base(usize n, usize s) { return c_calloc(n, s); }
static void* CDECL c__realloc_base(void* p, usize n) { return c_realloc(p, n); }
static void CDECL c__free_base(void* p) { h_free(p); }
static usize CDECL c__msize_base(void* p) { return c__msize(p); }
static void* CDECL c__aligned_malloc(usize n, usize a) { void* p = h_aligned(n, a); if (!p) set_errno(a & (a - 1) ? EINVAL : ENOMEM); return p; }
static void CDECL c__aligned_free(void* p) { h_aligned_free(p); }
static usize CDECL c__aligned_msize(void* p, usize a, usize off) { (void)a; (void)off; if (!p) return 0; void* raw = ((void**)p)[-1]; return h_size(raw) - (usize)((u8*)p - (u8*)raw); }
static void* CDECL c__aligned_realloc(void* p, usize n, usize a) {
    if (!p) return c__aligned_malloc(n, a);
    if (!n) { h_aligned_free(p); return nullptr; }
    void* q = h_aligned(n, a);
    if (!q) return nullptr;
    usize old = c__aligned_msize(p, a, 0);
    memcpy(q, p, old < n ? old : n);
    h_aligned_free(p);
    return q;
}
static void* CDECL c__aligned_offset_malloc(usize n, usize a, usize off) { (void)off; return c__aligned_malloc(n, a); }
static int CDECL c__heapchk() { return -2; }                     // _HEAPOK
static int CDECL c__heapmin() { return 0; }
static int CDECL c__set_sbh_threshold(usize n) { (void)n; return 1; }
static usize CDECL c__get_sbh_threshold() { return 0; }
static int CDECL c__callnewh(usize n) { (void)n; return 0; }

// ---------------------------------------------------------------- sorting and searching
typedef int (CDECL *Cmp)(const void*, const void*);
typedef int (CDECL *CmpS)(void*, const void*, const void*);
static void swap_bytes(u8* a, u8* b, usize n) { while (n--) { u8 t = *a; *a++ = *b; *b++ = t; } }
// introsort-ish: quicksort with median of three, insertion sort for small parts, heapsort when too deep
template <typename C>
static void sort_any(u8* base, usize n, usize w, C cmp, int depth) {
    while (n > 12) {
        if (depth-- <= 0) {
            // heapsort
            auto sift = [&](usize root, usize end) {
                for (;;) {
                    usize child = root * 2 + 1;
                    if (child >= end) break;
                    if (child + 1 < end && cmp(base + child * w, base + (child + 1) * w) < 0) child++;
                    if (cmp(base + root * w, base + child * w) >= 0) break;
                    swap_bytes(base + root * w, base + child * w, w);
                    root = child;
                }
            };
            for (usize i = n / 2; i-- > 0;) sift(i, n);
            for (usize e = n - 1; e > 0; e--) { swap_bytes(base, base + e * w, w); sift(0, e); }
            return;
        }
        u8 *lo = base, *mid = base + (n / 2) * w, *hi = base + (n - 1) * w;
        if (cmp(mid, lo) < 0) swap_bytes(mid, lo, w);
        if (cmp(hi, mid) < 0) { swap_bytes(hi, mid, w); if (cmp(mid, lo) < 0) swap_bytes(mid, lo, w); }
        swap_bytes(mid, base + w, w);                           // the pivot sits at index 1
        u8* pivot = base + w;
        usize i = 1, j = n - 1;
        for (;;) {
            do i++; while (i < n && cmp(base + i * w, pivot) < 0);
            do j--; while (cmp(base + j * w, pivot) > 0);
            if (i >= j) break;
            swap_bytes(base + i * w, base + j * w, w);
        }
        swap_bytes(pivot, base + j * w, w);
        // recurse into the smaller part, loop on the bigger one
        usize left = j, right = n - j - 1;
        if (left < right) { sort_any(base, left, w, cmp, depth); base += (j + 1) * w; n = right; }
        else { sort_any(base + (j + 1) * w, right, w, cmp, depth); n = left; }
    }
    for (usize i = 1; i < n; i++)
        for (usize j = i; j > 0 && cmp(base + (j - 1) * w, base + j * w) > 0; j--) swap_bytes(base + (j - 1) * w, base + j * w, w);
}
static void CDECL c_qsort(void* base, usize n, usize w, Cmp cmp) {
    if (n < 2 || !w) return;
    sort_any((u8*)base, n, w, [&](const void* a, const void* b) { return cmp(a, b); }, 64);
}
static void CDECL c_qsort_s(void* base, usize n, usize w, CmpS cmp, void* ctx) {
    if (n < 2 || !w) return;
    sort_any((u8*)base, n, w, [&](const void* a, const void* b) { return cmp(ctx, a, b); }, 64);
}
static void* CDECL c_bsearch(const void* key, const void* base, usize n, usize w, Cmp cmp) {
    usize lo = 0, hi = n;
    while (lo < hi) {
        usize mid = (lo + hi) / 2;
        const u8* e = (const u8*)base + mid * w;
        int c = cmp(key, e);
        if (!c) return (void*)e;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return nullptr;
}
static void* CDECL c_bsearch_s(const void* key, const void* base, usize n, usize w, CmpS cmp, void* ctx) {
    usize lo = 0, hi = n;
    while (lo < hi) {
        usize mid = (lo + hi) / 2;
        const u8* e = (const u8*)base + mid * w;
        int c = cmp(ctx, key, e);
        if (!c) return (void*)e;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return nullptr;
}
static void* CDECL c__lfind(const void* key, const void* base, u32* n, u32 w, Cmp cmp) {
    for (u32 i = 0; i < *n; i++) if (!cmp(key, (const u8*)base + (usize)i * w)) return (u8*)base + (usize)i * w;
    return nullptr;
}
static void* CDECL c__lsearch(const void* key, void* base, u32* n, u32 w, Cmp cmp) {
    void* r = c__lfind(key, base, n, w, cmp);
    if (r) return r;
    u8* d = (u8*)base + (usize)*n * w;
    memcpy(d, key, w);
    (*n)++;
    return d;
}

// ---------------------------------------------------------------- integers and random numbers
static int CDECL c_abs(int x) { return x < 0 ? -x : x; }
static i32 CDECL c_labs(i32 x) { return x < 0 ? -x : x; }
static i64 CDECL c_llabs(i64 x) { return x < 0 ? -x : x; }
static i64 CDECL c__abs64(i64 x) { return x < 0 ? -x : x; }
struct div_t { int quot, rem; };
struct ldiv_t { i32 quot, rem; };
struct lldiv_t { i64 quot, rem; };
static div_t CDECL c_div(int a, int b) { return {a / b, a % b}; }
static ldiv_t CDECL c_ldiv(i32 a, i32 b) { return {a / b, a % b}; }
static lldiv_t CDECL c_lldiv(i64 a, i64 b) { return {a / b, a % b}; }
static lldiv_t CDECL c_imaxdiv(i64 a, i64 b) { return {a / b, a % b}; }
static i64 CDECL c_imaxabs(i64 x) { return x < 0 ? -x : x; }
static int CDECL c_rand() { CrtThread* t = crt_thread(); t->rand_seed = t->rand_seed * 214013u + 2531011u; return (int)((t->rand_seed >> 16) & 0x7FFF); }
static void CDECL c_srand(u32 s) { crt_thread()->rand_seed = s; }
static int CDECL c_rand_s(u32* v) { if (!v) return EINVAL; random_bytes(v, 4); return 0; }
static u32 CDECL c__rotl(u32 v, int s) { s &= 31; return (v << s) | (v >> ((32 - s) & 31)); }
static u32 CDECL c__rotr(u32 v, int s) { s &= 31; return (v >> s) | (v << ((32 - s) & 31)); }
static u32 CDECL c__lrotl(u32 v, int s) { return c__rotl(v, s); }
static u64 CDECL c__rotl64(u64 v, int s) { s &= 63; return (v << s) | (v >> ((64 - s) & 63)); }
static u64 CDECL c__rotr64(u64 v, int s) { s &= 63; return (v >> s) | (v << ((64 - s) & 63)); }
static u16 CDECL c__byteswap_ushort(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static u32 CDECL c__byteswap_ulong(u32 v) { return __builtin_bswap32(v); }
static u64 CDECL c__byteswap_uint64(u64 v) { return __builtin_bswap64(v); }

// ---------------------------------------------------------------- threads
typedef u32 (WINAPI *BeginFn)(void*);
typedef void (CDECL *BeginFn0)(void*);
struct Begin0 { BeginFn0 fn; void* arg; };
static u32 WINAPI begin0_tramp(void* p) { Begin0 b = *(Begin0*)p; h_free(p); b.fn(b.arg); return 0; }
static u64 CDECL c__beginthreadex(void* sec, u32 stack, BeginFn fn, void* arg, u32 flags, u32* tid) {
    (void)sec;
    auto ct = k32<HANDLE (WINAPI*)(void*, SIZE_T, ThreadFn, void*, u32, u32*)>("CreateThread");
    HANDLE h = ct(nullptr, stack, (ThreadFn)fn, arg, flags, tid);
    if (!h) { set_errno_from_win32(); return 0; }
    return (u64)h;
}
static u64 CDECL c__beginthread(BeginFn0 fn, u32 stack, void* arg) {
    Begin0* b = (Begin0*)h_alloc(sizeof(Begin0));
    b->fn = fn; b->arg = arg;
    u32 tid;
    u64 h = c__beginthreadex(nullptr, stack, begin0_tramp, b, 0, &tid);
    if (!h) return (u64)-1;
    return h;
}
static void CDECL c__endthreadex(u32 code) { thr_exit(code); }
static void CDECL c__endthread() { thr_exit(0); }
static u32 CDECL c__getpid() { return 0x40; }
static int CDECL c__getmaxstdio() { return 512; }
static int CDECL c__setmaxstdio(int n) { return n <= 512 ? n : -1; }
static void CDECL c__sleep(u32 ms) { si::sleep(ms ? ms : 1); }
static void CDECL c__beep(u32 f, u32 d) { (void)f; (void)d; }

// the old msvcrt locks
static RTL_CRITICAL_SECTION crt_locks[64];
static bool crt_locks_init = false;
static void CDECL c__lock(int n) {
    auto init = k32<void (WINAPI*)(RTL_CRITICAL_SECTION*)>("InitializeCriticalSection");
    auto enter = k32<void (WINAPI*)(RTL_CRITICAL_SECTION*)>("EnterCriticalSection");
    if (!crt_locks_init) { for (auto& l : crt_locks) init(&l); crt_locks_init = true; }
    enter(&crt_locks[n & 63]);
}
static void CDECL c__unlock(int n) { k32<void (WINAPI*)(RTL_CRITICAL_SECTION*)>("LeaveCriticalSection")(&crt_locks[n & 63]); }
static void CDECL c__lock_locales() {}
static void CDECL c__unlock_locales() {}

// ---------------------------------------------------------------- floating-point control
static u32 ms_from_mxcsr(u32 mx) {
    u32 cw = 0;
    if (mx & (1 << 7)) cw |= 0x10;          // _EM_INVALID
    if (mx & (1 << 8)) cw |= 0x80000;       // _EM_DENORMAL
    if (mx & (1 << 9)) cw |= 0x8;           // _EM_ZERODIVIDE
    if (mx & (1 << 10)) cw |= 0x4;          // _EM_OVERFLOW
    if (mx & (1 << 11)) cw |= 0x2;          // _EM_UNDERFLOW
    if (mx & (1 << 12)) cw |= 0x1;          // _EM_INEXACT
    u32 rc = (mx >> 13) & 3;
    cw |= rc == 1 ? 0x100 : rc == 2 ? 0x200 : rc == 3 ? 0x300 : 0;
    if (mx & (1 << 15)) cw |= 0x1000000;    // _DN_FLUSH
    return cw;
}
static u32 mxcsr_from_ms(u32 cw, u32 mx) {
    mx &= ~0xFF80u;
    if (cw & 0x10) mx |= 1 << 7;
    if (cw & 0x80000) mx |= 1 << 8;
    if (cw & 0x8) mx |= 1 << 9;
    if (cw & 0x4) mx |= 1 << 10;
    if (cw & 0x2) mx |= 1 << 11;
    if (cw & 0x1) mx |= 1 << 12;
    u32 rc = cw & 0x300;
    mx |= (rc == 0x100 ? 1u : rc == 0x200 ? 2u : rc == 0x300 ? 3u : 0u) << 13;
    if (cw & 0x1000000) mx |= 1 << 15;
    return mx;
}
static u32 get_mxcsr() { u32 v; __asm__ volatile("stmxcsr %0" : "=m"(v)); return v; }
static void set_mxcsr(u32 v) { __asm__ volatile("ldmxcsr %0" :: "m"(v)); }
static u32 CDECL c__controlfp(u32 nv, u32 mask) {
    mask &= ~0x80000u;                                          // _MCW_DN's denormal mask can't be changed
    u32 mx = get_mxcsr();
    u32 cw = ms_from_mxcsr(mx);
    if (mask) { cw = (cw & ~mask) | (nv & mask); set_mxcsr(mxcsr_from_ms(cw, mx)); }
    return ms_from_mxcsr(get_mxcsr());
}
static u32 CDECL c__control87(u32 nv, u32 mask) { return c__controlfp(nv, mask); }
static int CDECL c__controlfp_s(u32* cur, u32 nv, u32 mask) { u32 r = c__controlfp(nv, mask); if (cur) *cur = r; return 0; }
static int CDECL c___control87_2(u32 nv, u32 mask, u32* x87, u32* sse) { u32 r = c__controlfp(nv, mask); if (x87) *x87 = r; if (sse) *sse = r; return 1; }
static u32 status_from_mxcsr(u32 mx) {
    u32 s = 0;
    if (mx & 1) s |= 0x10; if (mx & 2) s |= 0x80000; if (mx & 4) s |= 0x8; if (mx & 8) s |= 0x4; if (mx & 16) s |= 0x2; if (mx & 32) s |= 0x1;
    return s;
}
static u32 CDECL c__statusfp() { return status_from_mxcsr(get_mxcsr()); }
static void CDECL c__statusfp2(u32* x87, u32* sse) { u32 s = c__statusfp(); if (x87) *x87 = 0; if (sse) *sse = s; }
static u32 CDECL c__clearfp() { u32 mx = get_mxcsr(); set_mxcsr(mx & ~0x3Fu); __asm__ volatile("fnclex"); return status_from_mxcsr(mx); }
static void CDECL c__fpreset() { set_mxcsr(0x1F80); __asm__ volatile("fninit"); }
static int CDECL c___fpe_flt_rounds() { u32 rc = (get_mxcsr() >> 13) & 3; return rc == 0 ? 1 : rc == 1 ? 3 : rc == 2 ? 2 : 0; }
static int CDECL c__set_FMA3_enable(int f) { (void)f; return 0; }
static int CDECL c__get_FMA3_enable() { return 0; }
static u32 CDECL c__set_output_format(u32 f) { (void)f; return 0; }
static u32 CDECL c__get_output_format() { return 0; }
static int CDECL c_fegetround() { u32 rc = (get_mxcsr() >> 13) & 3; return (int)(rc == 0 ? 0 : rc == 1 ? 0x100 : rc == 2 ? 0x200 : 0x300); }
static int CDECL c_fesetround(int r) {
    u32 rc = r == 0 ? 0 : r == 0x100 ? 1 : r == 0x200 ? 2 : r == 0x300 ? 3 : 4;
    if (rc > 3) return 1;
    set_mxcsr((get_mxcsr() & ~(3u << 13)) | (rc << 13));
    return 0;
}
static int CDECL c_feclearexcept(int e) { set_mxcsr(get_mxcsr() & ~(u32)(e & 0x3F)); return 0; }
static int CDECL c_fetestexcept(int e) { return (int)(get_mxcsr() & (u32)e & 0x3F); }
static int CDECL c_feraiseexcept(int e) { set_mxcsr(get_mxcsr() | (u32)(e & 0x3F)); return 0; }
struct fenv_t { u32 ctl, stat; };
static int CDECL c_fegetenv(fenv_t* e) { e->ctl = get_mxcsr(); e->stat = 0; return 0; }
static int CDECL c_fesetenv(const fenv_t* e) { set_mxcsr(e->ctl); return 0; }
static int CDECL c_feholdexcept(fenv_t* e) { c_fegetenv(e); set_mxcsr((get_mxcsr() & ~0x3Fu) | 0x1F80); return 0; }
static int CDECL c_feupdateenv(const fenv_t* e) { u32 ex = get_mxcsr() & 0x3F; set_mxcsr(e->ctl | ex); return 0; }
static int CDECL c_fegetexceptflag(u32* f, int e) { *f = get_mxcsr() & (u32)e & 0x3F; return 0; }
static int CDECL c_fesetexceptflag(const u32* f, int e) { set_mxcsr((get_mxcsr() & ~(u32)(e & 0x3F)) | (*f & (u32)e & 0x3F)); return 0; }

// ---------------------------------------------------------------- setjmp / longjmp
// Windows' jmp_buf: Frame, Rbx, Rsp, Rbp, Rsi, Rdi, R12-R15, Rip, MxCsr, FpCsr, Xmm6-Xmm15. A longjmp here
// restores the registers directly; it doesn't run C++ destructors or __finally blocks on the way.
__asm__(".intel_syntax noprefix\n"
        ".text\n"
        ".globl rt_setjmp\n"
        "rt_setjmp:\n"
        "  mov [rcx], rdx\n  mov [rcx+0x08], rbx\n  lea rax, [rsp+8]\n  mov [rcx+0x10], rax\n  mov [rcx+0x18], rbp\n"
        "  mov [rcx+0x20], rsi\n  mov [rcx+0x28], rdi\n  mov [rcx+0x30], r12\n  mov [rcx+0x38], r13\n  mov [rcx+0x40], r14\n"
        "  mov [rcx+0x48], r15\n  mov rax, [rsp]\n  mov [rcx+0x50], rax\n  stmxcsr [rcx+0x58]\n  fnstcw [rcx+0x5c]\n"
        "  movdqu [rcx+0x60], xmm6\n  movdqu [rcx+0x70], xmm7\n  movdqu [rcx+0x80], xmm8\n  movdqu [rcx+0x90], xmm9\n"
        "  movdqu [rcx+0xa0], xmm10\n  movdqu [rcx+0xb0], xmm11\n  movdqu [rcx+0xc0], xmm12\n  movdqu [rcx+0xd0], xmm13\n"
        "  movdqu [rcx+0xe0], xmm14\n  movdqu [rcx+0xf0], xmm15\n"
        "  xor eax, eax\n  ret\n"
        ".globl rt_longjmp\n"
        "rt_longjmp:\n"
        "  mov eax, edx\n  test eax, eax\n  jnz 1f\n  inc eax\n1:\n"
        "  mov rbx, [rcx+0x08]\n  mov rbp, [rcx+0x18]\n  mov rsi, [rcx+0x20]\n  mov rdi, [rcx+0x28]\n  mov r12, [rcx+0x30]\n"
        "  mov r13, [rcx+0x38]\n  mov r14, [rcx+0x40]\n  mov r15, [rcx+0x48]\n  ldmxcsr [rcx+0x58]\n  fldcw [rcx+0x5c]\n"
        "  movdqu xmm6, [rcx+0x60]\n  movdqu xmm7, [rcx+0x70]\n  movdqu xmm8, [rcx+0x80]\n  movdqu xmm9, [rcx+0x90]\n"
        "  movdqu xmm10, [rcx+0xa0]\n  movdqu xmm11, [rcx+0xb0]\n  movdqu xmm12, [rcx+0xc0]\n  movdqu xmm13, [rcx+0xd0]\n"
        "  movdqu xmm14, [rcx+0xe0]\n  movdqu xmm15, [rcx+0xf0]\n"
        "  mov rsp, [rcx+0x10]\n  jmp qword ptr [rcx+0x50]\n"
        ".att_syntax\n");
extern "C" int rt_setjmp(void* buf, void* frame);
extern "C" void rt_longjmp(void* buf, int v);

// ---------------------------------------------------------------- paths
static void CDECL c__splitpath(const char* p, char* drive, char* dir, char* fname, char* ext) {
    const char* s = p;
    if (s[0] && s[1] == ':') { if (drive) { drive[0] = s[0]; drive[1] = ':'; drive[2] = 0; } s += 2; }
    else if (drive) drive[0] = 0;
    const char* last = nullptr;
    for (const char* q = s; *q; q++) if (*q == '\\' || *q == '/') last = q;
    const char* name = last ? last + 1 : s;
    if (dir) { usize n = (usize)(name - s); memcpy(dir, s, n); dir[n] = 0; }
    const char* dot = nullptr;
    for (const char* q = name; *q; q++) if (*q == '.') dot = q;
    if (!dot) dot = name + slen(name);
    if (fname) { usize n = (usize)(dot - name); memcpy(fname, name, n); fname[n] = 0; }
    if (ext) scpy(ext, dot);
}
static int CDECL c__splitpath_s(const char* p, char* d, usize dn, char* dir, usize dirn, char* f, usize fn, char* e, usize en) {
    char a[4], b[MAX_PATH], c[MAX_PATH], x[MAX_PATH];
    c__splitpath(p, a, b, c, x);
    if (d) slcpy(d, a, dn);
    if (dir) slcpy(dir, b, dirn);
    if (f) slcpy(f, c, fn);
    if (e) slcpy(e, x, en);
    return 0;
}
static void CDECL c__wsplitpath(const WCHAR* p, WCHAR* drive, WCHAR* dir, WCHAR* fname, WCHAR* ext) {
    char u[MAX_PATH * 3], a[4], b[MAX_PATH * 3], c[MAX_PATH * 3], x[MAX_PATH * 3];
    utf8_from_wide(p, -1, u, sizeof(u));
    c__splitpath(u, a, b, c, x);
    if (drive) wide_from_utf8(a, -1, drive, 3);
    if (dir) wide_from_utf8(b, -1, dir, MAX_PATH);
    if (fname) wide_from_utf8(c, -1, fname, MAX_PATH);
    if (ext) wide_from_utf8(x, -1, ext, MAX_PATH);
}
static int CDECL c__wsplitpath_s(const WCHAR* p, WCHAR* d, usize dn, WCHAR* dir, usize dirn, WCHAR* f, usize fn, WCHAR* e, usize en) {
    (void)dn; (void)dirn; (void)fn; (void)en;
    c__wsplitpath(p, d, dir, f, e);
    return 0;
}
static void CDECL c__makepath(char* out, const char* drive, const char* dir, const char* fname, const char* ext) {
    out[0] = 0;
    if (drive && *drive) { out[0] = drive[0]; out[1] = ':'; out[2] = 0; }
    if (dir && *dir) { slcat(out, dir, MAX_PATH); usize n = slen(out); if (out[n - 1] != '\\' && out[n - 1] != '/') slcat(out, "\\", MAX_PATH); }
    if (fname) slcat(out, fname, MAX_PATH);
    if (ext && *ext) { if (*ext != '.') slcat(out, ".", MAX_PATH); slcat(out, ext, MAX_PATH); }
}
static int CDECL c__makepath_s(char* out, usize n, const char* drive, const char* dir, const char* fname, const char* ext) {
    char b[MAX_PATH];
    c__makepath(b, drive, dir, fname, ext);
    if (slen(b) >= n) return ERANGE;
    scpy(out, b);
    return 0;
}
static char* CDECL c__fullpath(char* out, const char* in, usize cap) {
    auto f = k32<u32 (WINAPI*)(const char*, u32, char*, char**)>("GetFullPathNameA");
    char tmp[MAX_PATH];
    u32 n = f(in && *in ? in : ".", sizeof(tmp), tmp, nullptr);
    if (!n || n >= sizeof(tmp)) { set_errno(ENOENT); return nullptr; }
    if (!out) { out = (char*)h_alloc(n + 1); cap = n + 1; }
    if (n + 1 > cap) { set_errno(ERANGE); return nullptr; }
    memcpy(out, tmp, n + 1);
    return out;
}
static WCHAR* CDECL c__wfullpath(WCHAR* out, const WCHAR* in, usize cap) {
    auto f = k32<u32 (WINAPI*)(const WCHAR*, u32, WCHAR*, WCHAR**)>("GetFullPathNameW");
    WCHAR tmp[MAX_PATH];
    u32 n = f(in && *in ? in : L".", MAX_PATH, tmp, nullptr);
    if (!n || n >= MAX_PATH) { set_errno(ENOENT); return nullptr; }
    if (!out) { out = (WCHAR*)h_alloc((n + 1) * 2); cap = n + 1; }
    if (n + 1 > cap) { set_errno(ERANGE); return nullptr; }
    memcpy(out, tmp, (n + 1) * 2);
    return out;
}
static int CDECL c_system(const char* cmd) {
    if (!cmd) return 0;                                         // no command interpreter
    msg("system(\"%s\") is not supported", cmd);
    set_errno(ENOENT);
    return -1;
}
static int CDECL c__wsystem(const WCHAR* cmd) { if (!cmd) return 0; set_errno(ENOENT); return -1; }
static i64 CDECL c__spawnv(int m, const char* p, const char* const* a) { (void)m; (void)a; msg("_spawn(%s) is not supported", p); set_errno(ENOENT); return -1; }
static i64 CDECL c__spawnvp(int m, const char* p, const char* const* a) { return c__spawnv(m, p, a); }
static i64 CDECL c__execv(const char* p, const char* const* a) { return c__spawnv(0, p, a); }
static i64 CDECL c__execvp(const char* p, const char* const* a) { return c__spawnv(0, p, a); }
static i64 CDECL c__cwait(int* s, i64 h, int a) { (void)s; (void)h; (void)a; set_errno(ECHILD); return -1; }
static void* CDECL c__popen(const char* c, const char* m) { (void)m; msg("_popen(\"%s\") is not supported", c); set_errno(ENOSYS); return nullptr; }
static void* CDECL c__wpopen(const WCHAR* c, const WCHAR* m) { (void)c; (void)m; set_errno(ENOSYS); return nullptr; }
static int CDECL c__pclose(void* f) { (void)f; return -1; }

static const Export core_exports[] = {
    X(_errno), X(__doserrno), X(_get_errno), X(_set_errno), X(_get_doserrno), X(_set_doserrno),
    X(_set_invalid_parameter_handler), X(_get_invalid_parameter_handler), X(_set_thread_local_invalid_parameter_handler),
    X(_get_thread_local_invalid_parameter_handler), X(_invalid_parameter), X(_invalid_parameter_noinfo),
    X(_invalid_parameter_noinfo_noreturn), X(_invoke_watson), X(abort), X(_set_abort_behavior), X(_wassert), X(_assert),
    X(_purecall), X(_set_purecall_handler), X(_get_purecall_handler), X(terminate), X(set_terminate), X(_get_terminate),
    X(_amsg_exit), X(_crt_debugger_hook), X(signal), X(raise), X(__pxcptinfoptrs), X(__fpecode),
    X(_configure_narrow_argv), X(_configure_wide_argv), X(__p___argc), X(__p___argv), X(__p___wargv), X(__p__acmdln),
    X(__p__wcmdln), X(__p__pgmptr), X(__p__wpgmptr), X(_get_pgmptr), X(_get_wpgmptr), X(__getmainargs), X(__wgetmainargs),
    X(_set_app_type), EA("__set_app_type", c__set_app_type), X(_get_app_type), X(__p__fmode), X(__p__commode), X(_set_fmode), X(_get_fmode),
    X(_set_new_mode), X(_query_new_mode), X(_set_new_handler), X(_query_new_handler), X(_configthreadlocale),
    X(__setusermatherr), X(_setusermatherr), X(_matherr), X(_seh_filter_exe), X(_seh_filter_dll), X(_XcptFilter),
    X(__current_exception), X(__current_exception_context), X(__processing_throw), X(_is_c_termination_complete),
    X(_register_thread_local_exe_atexit_callback), X(_get_heap_handle), X(__lconv_init), X(_initterm), X(_initterm_e),
    X(_initialize_onexit_table), X(_register_onexit_function), X(_execute_onexit_table), X(_crt_atexit), X(atexit),
    X(_crt_at_quick_exit), X(at_quick_exit), X(_onexit), X(__dllonexit), X(exit), X(_exit), X(_Exit), X(quick_exit), X(_cexit),
    X(_c_exit), X(getenv), X(_wgetenv), X(getenv_s), X(_wgetenv_s), X(_dupenv_s), X(_wdupenv_s), X(_putenv), X(_wputenv),
    X(_putenv_s), X(_wputenv_s), EA("putenv", c__putenv), X(__p__environ), X(__p__wenviron), X(_get_initial_narrow_environment),
    X(_get_initial_wide_environment), X(_initialize_narrow_environment), X(_initialize_wide_environment),
    X(malloc), X(calloc), X(realloc), X(free), X(_recalloc), X(_msize), X(_expand), X(_malloc_base), X(_calloc_base),
    X(_realloc_base), X(_free_base), X(_msize_base), X(_aligned_malloc), X(_aligned_free), X(_aligned_msize), X(_aligned_realloc),
    X(_aligned_offset_malloc), X(_heapchk), X(_heapmin), X(_set_sbh_threshold), X(_get_sbh_threshold), X(_callnewh),
    X(qsort), X(qsort_s), X(bsearch), X(bsearch_s), X(_lfind), X(_lsearch), X(abs), X(labs), X(llabs), X(_abs64), X(div),
    X(ldiv), X(lldiv), X(imaxdiv), X(imaxabs), X(rand), X(srand), X(rand_s), X(_rotl), X(_rotr), X(_rotl64), X(_rotr64), X(_lrotl),
    X(_byteswap_ushort), X(_byteswap_ulong), X(_byteswap_uint64), X(_beginthreadex), X(_beginthread), X(_endthreadex),
    X(_endthread), X(_getpid), X(_getmaxstdio), X(_setmaxstdio), X(_sleep), X(_beep), X(_lock), X(_unlock), X(_lock_locales),
    X(_unlock_locales), X(_controlfp), X(_control87), X(_controlfp_s), X(__control87_2), X(_statusfp), X(_statusfp2), X(_clearfp),
    X(_fpreset), X(__fpe_flt_rounds), X(_set_FMA3_enable), X(_get_FMA3_enable), X(_set_output_format), X(_get_output_format),
    X(fegetround), X(fesetround), X(feclearexcept), X(fetestexcept), X(feraiseexcept), X(fegetenv), X(fesetenv), X(feholdexcept),
    X(feupdateenv), X(fegetexceptflag), X(fesetexceptflag),
    EA("_setjmp", rt_setjmp), EA("_setjmpex", rt_setjmp), EA("setjmp", rt_setjmp), EA("longjmp", rt_longjmp),
    X(_splitpath), X(_splitpath_s), X(_wsplitpath), X(_wsplitpath_s), X(_makepath), X(_makepath_s), X(_fullpath), X(_wfullpath),
    X(system), X(_wsystem), X(_spawnv), X(_spawnvp), X(_execv), X(_execvp), X(_cwait), X(_popen), X(_wpopen), X(_pclose),
};
EXPORT_TABLE("ucrtbase.dll", core_exports);

// data the old msvcrt.dll exports as variables
static const Export msvcrt_core[] = {
    {"__argc", (void*)&c_argc}, {"__argv", (void*)&c_argv}, {"__wargv", (void*)&c_wargv}, {"_acmdln", (void*)&c_acmdln},
    {"_wcmdln", (void*)&c_wcmdln}, {"_environ", (void*)&c_environ_arr}, {"_wenviron", (void*)&c_wenviron_arr},
    {"__initenv", (void*)&initial_env}, {"__winitenv", (void*)&initial_wenv}, {"_fmode", (void*)&c_fmode}, {"_commode", (void*)&c_commode},
    {"_pgmptr", (void*)&c_pgmptr}, {"_wpgmptr", (void*)&c_wpgmptr},
};
EXPORT_TABLE("msvcrt.dll", msvcrt_core);

}  // namespace rt
