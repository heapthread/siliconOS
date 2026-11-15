// win32run - shell32 (command lines, special folders) and the text helpers of user32 (CharUpper,
// wsprintf, LoadString...) that console programs use too.
#include "crt.h"

namespace rt {

// ---------------------------------------------------------------- shell32
static WCHAR** WINAPI CommandLineToArgvW(const WCHAR* line, int* argc) {
    if (!argc) { set_error(ERROR_INVALID_PARAMETER); return nullptr; }
    if (!line || !*line) {                                       // the program's own path
        WCHAR* p = (WCHAR*)h_alloc(sizeof(WCHAR*) * 2 + (wlen(exe_module->wpath) + 1) * 2);
        WCHAR** a = (WCHAR**)p;
        WCHAR* s = (WCHAR*)(a + 2);
        memcpy(s, exe_module->wpath, (wlen(exe_module->wpath) + 1) * 2);
        a[0] = s; a[1] = nullptr;
        *argc = 1;
        return a;
    }
    int chars = 0;
    int n = split_cmdline(line, nullptr, nullptr, &chars);
    // one block (LocalFree frees it all): the pointers, then the strings
    u8* mem = (u8*)h_alloc(sizeof(WCHAR*) * (usize)(n + 1) + sizeof(WCHAR) * (usize)(chars + 1));
    WCHAR** argv = (WCHAR**)mem;
    WCHAR* buf = (WCHAR*)(mem + sizeof(WCHAR*) * (usize)(n + 1));
    split_cmdline(line, argv, buf, nullptr);
    argv[n] = nullptr;
    *argc = n;
    return argv;
}

// special folders, all inside silicon's /home
static const char* folder_path(int csidl) {
    switch (csidl & 0xFF) {
    case 0x00: case 0x10: return "C:\\home\\Desktop";            // desktop
    case 0x05: case 0x0C: return "C:\\home\\Documents";          // personal, my documents
    case 0x1A: return "C:\\home\\AppData\\Roaming";              // appdata
    case 0x1C: return "C:\\home\\AppData\\Local";                // local appdata
    case 0x23: return "C:\\ProgramData";                         // common appdata
    case 0x28: return "C:\\home";                                // profile
    case 0x24: return "C:\\Windows";
    case 0x25: case 0x29: return "C:\\Windows\\System32";
    case 0x26: case 0x2A: return "C:\\Program Files";
    case 0x27: return "C:\\home\\Pictures";
    case 0x0D: return "C:\\home\\Music";
    case 0x0E: return "C:\\home\\Videos";
    case 0x14: return "C:\\Windows\\Fonts";
    case 0x2B: return "C:\\Program Files\\Common Files";
    case 0x07: return "C:\\home\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
    case 0x08: return "C:\\home\\AppData\\Roaming\\Microsoft\\Windows\\Recent";
    case 0x15: return "C:\\home\\AppData\\Roaming\\Microsoft\\Windows\\Templates";
    case 0x20: return "C:\\home\\AppData\\Local\\Microsoft\\Windows\\INetCache";
    }
    return "C:\\home";
}
static void make_dirs(const char* win) {
    char path[MAX_PATH];
    if (!path_to_silicon(win, path, sizeof(path))) return;
    for (char* p = path + 1; *p; p++) if (*p == '/') { *p = 0; si::make_dir(path); *p = '/'; }
    si::make_dir(path);
}
static HRESULT WINAPI SHGetFolderPathW(HANDLE w, int csidl, HANDLE tok, u32 flags, WCHAR* out) {
    (void)w; (void)tok; (void)flags;
    const char* p = folder_path(csidl);
    if (csidl & 0x8000) make_dirs(p);                            // CSIDL_FLAG_CREATE
    wide_from_utf8(p, -1, out, MAX_PATH);
    return 0;
}
static HRESULT WINAPI SHGetFolderPathA(HANDLE w, int csidl, HANDLE tok, u32 flags, char* out) {
    (void)w; (void)tok; (void)flags;
    const char* p = folder_path(csidl);
    if (csidl & 0x8000) make_dirs(p);
    slcpy(out, p, MAX_PATH);
    return 0;
}
static BOOL WINAPI SHGetSpecialFolderPathW(HANDLE w, WCHAR* out, int csidl, BOOL create) { return SHGetFolderPathW(w, csidl | (create ? 0x8000 : 0), nullptr, 0, out) == 0; }
static BOOL WINAPI SHGetSpecialFolderPathA(HANDLE w, char* out, int csidl, BOOL create) { return SHGetFolderPathA(w, csidl | (create ? 0x8000 : 0), nullptr, 0, out) == 0; }
struct GUID { u32 a; u16 b, c; u8 d[8]; };
static HRESULT WINAPI SHGetKnownFolderPath(const GUID* id, u32 flags, HANDLE tok, WCHAR** out) {
    (void)tok;
    // the first word of the well-known folder ids
    int csidl = 0x28;
    switch (id->a) {
    case 0xB4BFCC3A: csidl = 0x00; break;   // Desktop
    case 0xFDD39AD0: case 0xF42EE2D3: csidl = 0x05; break;   // Documents
    case 0x3EB685DB: csidl = 0x1A; break;   // RoamingAppData
    case 0xF1B32785: csidl = 0x1C; break;   // LocalAppData
    case 0x62AB5D82: csidl = 0x23; break;   // ProgramData
    case 0x5E6C858F: csidl = 0x28; break;   // Profile
    case 0x1AC14E77: csidl = 0x25; break;   // System
    case 0x905E63B6: csidl = 0x26; break;   // ProgramFiles
    case 0x33E28130: csidl = 0x27; break;   // Pictures
    case 0x374DE290: csidl = 0x28; break;   // Downloads -> the profile
    case 0xF38BF404: csidl = 0x24; break;   // Windows
    case 0xFD228CB7: csidl = 0x14; break;   // Fonts
    }
    WCHAR* buf = (WCHAR*)h_alloc(MAX_PATH * 2);
    SHGetFolderPathW(nullptr, csidl | ((flags & 0x8000) ? 0x8000 : 0), nullptr, 0, buf);
    *out = buf;
    return 0;
}
static void WINAPI CoTaskMemFree(void* p) { h_free(p); }
static void* WINAPI CoTaskMemAlloc(SIZE_T n) { return h_alloc(n); }
static void* WINAPI CoTaskMemRealloc(void* p, SIZE_T n) { return h_realloc(p, n); }
static int WINAPI SHCreateDirectoryExW(HANDLE w, const WCHAR* p, void* sa) {
    (void)w; (void)sa;
    char u[MAX_PATH * 3];
    utf8_from_wide(p, -1, u, sizeof(u));
    make_dirs(u);
    return 0;
}
static int WINAPI SHCreateDirectoryExA(HANDLE w, const char* p, void* sa) { (void)w; (void)sa; make_dirs(p); return 0; }
static HANDLE WINAPI ShellExecuteW(HANDLE w, const WCHAR* op, const WCHAR* file, const WCHAR* params, const WCHAR* dir, int show) {
    (void)w; (void)op; (void)params; (void)dir; (void)show;
    char u[300];
    utf8_from_wide(file ? file : L"?", -1, u, sizeof(u));
    msg("ShellExecute(%s) is not supported", u);
    return (HANDLE)(i64)2;                                       // "file not found": failures are values <= 32
}
static HANDLE WINAPI ShellExecuteA(HANDLE w, const char* op, const char* file, const char* params, const char* dir, int show) {
    (void)w; (void)op; (void)params; (void)dir; (void)show;
    msg("ShellExecute(%s) is not supported", file ? file : "?");
    return (HANDLE)(i64)2;
}
static BOOL WINAPI ShellExecuteExW(void* info) { (void)info; set_error(ERROR_NOT_SUPPORTED); return FALSE; }
static u32 WINAPI DragQueryFileW(HANDLE d, u32 i, WCHAR* f, u32 n) { (void)d; (void)i; (void)f; (void)n; return 0; }
static void WINAPI DragAcceptFiles(HANDLE w, BOOL a) { (void)w; (void)a; }
static void WINAPI DragFinish(HANDLE d) { (void)d; }
static BOOL WINAPI Shell_NotifyIconW(u32 m, void* d) { (void)m; (void)d; return TRUE; }
static BOOL WINAPI PathFileExistsW(const WCHAR* p) { return k32<u32 (WINAPI*)(const WCHAR*)>("GetFileAttributesW")(p) != 0xFFFFFFFF; }
static BOOL WINAPI PathFileExistsA(const char* p) { return k32<u32 (WINAPI*)(const char*)>("GetFileAttributesA")(p) != 0xFFFFFFFF; }
static BOOL WINAPI PathIsDirectoryW(const WCHAR* p) { u32 a = k32<u32 (WINAPI*)(const WCHAR*)>("GetFileAttributesW")(p); return a != 0xFFFFFFFF && (a & 0x10); }
static WCHAR* WINAPI PathFindFileNameW(const WCHAR* p) { const WCHAR* r = p; for (; *p; p++) if ((*p == '\\' || *p == '/') && p[1]) r = p + 1; return (WCHAR*)r; }
static char* WINAPI PathFindFileNameA(const char* p) { const char* r = p; for (; *p; p++) if ((*p == '\\' || *p == '/') && p[1]) r = p + 1; return (char*)r; }
static WCHAR* WINAPI PathFindExtensionW(const WCHAR* p) {
    const WCHAR* dot = nullptr;
    for (; *p; p++) { if (*p == '\\' || *p == '/' || *p == ' ') dot = nullptr; else if (*p == '.') dot = p; }
    return (WCHAR*)(dot ? dot : p);
}
static BOOL WINAPI PathRemoveFileSpecW(WCHAR* p) {
    WCHAR* last = nullptr;
    for (WCHAR* q = p; *q; q++) if (*q == '\\' || *q == '/') last = q;
    if (!last) { if (*p) { *p = 0; return TRUE; } return FALSE; }
    if (last == p + 2 && p[1] == ':') last[1] = 0; else *last = 0;
    return TRUE;
}
static BOOL WINAPI PathRemoveFileSpecA(char* p) {
    char* last = nullptr;
    for (char* q = p; *q; q++) if (*q == '\\' || *q == '/') last = q;
    if (!last) { if (*p) { *p = 0; return TRUE; } return FALSE; }
    if (last == p + 2 && p[1] == ':') last[1] = 0; else *last = 0;
    return TRUE;
}
static WCHAR* WINAPI PathCombineW(WCHAR* out, const WCHAR* a, const WCHAR* b) {
    WCHAR tmp[MAX_PATH];
    usize n = 0;
    if (b && (b[0] == '\\' || (b[0] && b[1] == ':'))) { usize l = wlen(b); memcpy(tmp, b, (l + 1) * 2); }
    else {
        if (a) { n = wlen(a); memcpy(tmp, a, n * 2); }
        if (n && tmp[n - 1] != '\\') tmp[n++] = '\\';
        usize l = b ? wlen(b) : 0;
        memcpy(tmp + n, b, l * 2);
        tmp[n + l] = 0;
    }
    memcpy(out, tmp, (wlen(tmp) + 1) * 2);
    return out;
}
static BOOL WINAPI PathAppendW(WCHAR* p, const WCHAR* more) { PathCombineW(p, p, more); return TRUE; }
static BOOL WINAPI PathIsRelativeW(const WCHAR* p) { return !(p[0] == '\\' || (p[0] && p[1] == ':')); }
static HRESULT WINAPI SHGetMalloc(void** m) { *m = nullptr; return (HRESULT)0x80004001; }
static int WINAPI SHFileOperationW(void* op) { (void)op; return 0x78; }
static HRESULT WINAPI SetCurrentProcessExplicitAppUserModelID(const WCHAR* id) { (void)id; return 0; }

static const Export shell_exports[] = {
    E(CommandLineToArgvW), E(SHGetFolderPathW), E(SHGetFolderPathA), E(SHGetSpecialFolderPathW), E(SHGetSpecialFolderPathA),
    E(SHGetKnownFolderPath), E(SHCreateDirectoryExW), E(SHCreateDirectoryExA), E(ShellExecuteW), E(ShellExecuteA), E(ShellExecuteExW),
    E(DragQueryFileW), E(DragAcceptFiles), E(DragFinish), E(Shell_NotifyIconW), E(SHGetMalloc), E(SHFileOperationW),
    E(SetCurrentProcessExplicitAppUserModelID),
};
EXPORT_TABLE("shell32.dll", shell_exports);
static const Export shlwapi_exports[] = {
    E(PathFileExistsW), E(PathFileExistsA), E(PathIsDirectoryW), E(PathFindFileNameW), E(PathFindFileNameA), E(PathFindExtensionW),
    E(PathRemoveFileSpecW), E(PathRemoveFileSpecA), E(PathCombineW), E(PathAppendW), E(PathIsRelativeW),
};
EXPORT_TABLE("shlwapi.dll", shlwapi_exports);
static const Export ole_exports[] = { E(CoTaskMemFree), E(CoTaskMemAlloc), E(CoTaskMemRealloc) };
EXPORT_TABLE("ole32.dll", ole_exports);
static const Export combase_exports[] = { E(CoTaskMemFree), E(CoTaskMemAlloc), E(CoTaskMemRealloc) };
EXPORT_TABLE("combase.dll", combase_exports);

// ---------------------------------------------------------------- user32: text helpers
static WCHAR* WINAPI CharUpperW(WCHAR* s) {
    if ((u64)s < 0x10000) return (WCHAR*)(u64)wupper((u32)(u64)s);
    for (WCHAR* p = s; *p; p++) *p = (WCHAR)wupper((u16)*p);
    return s;
}
static WCHAR* WINAPI CharLowerW(WCHAR* s) {
    if ((u64)s < 0x10000) return (WCHAR*)(u64)wlower((u32)(u64)s);
    for (WCHAR* p = s; *p; p++) *p = (WCHAR)wlower((u16)*p);
    return s;
}
static char* WINAPI CharUpperA(char* s) {
    if ((u64)s < 0x100) return (char*)(u64)upper((int)(u64)s);
    for (char* p = s; *p; p++) *p = (char)upper((u8)*p);
    return s;
}
static char* WINAPI CharLowerA(char* s) {
    if ((u64)s < 0x100) return (char*)(u64)lower((int)(u64)s);
    for (char* p = s; *p; p++) *p = (char)lower((u8)*p);
    return s;
}
static u32 WINAPI CharUpperBuffW(WCHAR* s, u32 n) { for (u32 i = 0; i < n; i++) s[i] = (WCHAR)wupper((u16)s[i]); return n; }
static u32 WINAPI CharLowerBuffW(WCHAR* s, u32 n) { for (u32 i = 0; i < n; i++) s[i] = (WCHAR)wlower((u16)s[i]); return n; }
static u32 WINAPI CharUpperBuffA(char* s, u32 n) { for (u32 i = 0; i < n; i++) s[i] = (char)upper((u8)s[i]); return n; }
static u32 WINAPI CharLowerBuffA(char* s, u32 n) { for (u32 i = 0; i < n; i++) s[i] = (char)lower((u8)s[i]); return n; }
static char* WINAPI CharNextA(const char* s) {
    if (!*s) return (char*)s;
    u8 c = (u8)*s++;
    if (c >= 0xC0) while (((u8)*s & 0xC0) == 0x80) s++;          // the ANSI code page is UTF-8
    return (char*)s;
}
static WCHAR* WINAPI CharNextW(const WCHAR* s) { return (WCHAR*)(*s ? s + 1 : s); }
static char* WINAPI CharPrevA(const char* start, const char* s) {
    if (s <= start) return (char*)start;
    s--;
    while (s > start && ((u8)*s & 0xC0) == 0x80) s--;
    return (char*)s;
}
static WCHAR* WINAPI CharPrevW(const WCHAR* start, const WCHAR* s) { return (WCHAR*)(s > start ? s - 1 : start); }
static BOOL WINAPI IsCharAlphaW(WCHAR c) { return (ctype1((u16)c) & 0x100) != 0; }
static BOOL WINAPI IsCharAlphaNumericW(WCHAR c) { return (ctype1((u16)c) & 0x104) != 0; }
static BOOL WINAPI IsCharUpperW(WCHAR c) { return (ctype1((u16)c) & 1) != 0; }
static BOOL WINAPI IsCharLowerW(WCHAR c) { return (ctype1((u16)c) & 2) != 0; }
static BOOL WINAPI IsCharAlphaA(char c) { return IsCharAlphaW((u8)c); }
static BOOL WINAPI IsCharAlphaNumericA(char c) { return IsCharAlphaNumericW((u8)c); }
static BOOL WINAPI IsCharUpperA(char c) { return IsCharUpperW((u8)c); }
static BOOL WINAPI IsCharLowerA(char c) { return IsCharLowerW((u8)c); }
static BOOL WINAPI CharToOemA(const char* s, char* d) { if (s != d) scpy(d, s); return TRUE; }
static BOOL WINAPI OemToCharA(const char* s, char* d) { if (s != d) scpy(d, s); return TRUE; }
static BOOL WINAPI CharToOemBuffA(const char* s, char* d, u32 n) { memmove(d, s, n); return TRUE; }
static BOOL WINAPI OemToCharBuffA(const char* s, char* d, u32 n) { memmove(d, s, n); return TRUE; }

// wsprintf: printf with at most 1024 characters and no floating point
static int WINAPI wvsprintfA(char* out, const char* fmt, va_list ap) { return vsnfmt(out, 1025, fmt, ap); }
static int CDECL wsprintfA(char* out, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int n = vsnfmt(out, 1025, fmt, ap); va_end(ap); return n; }
static int WINAPI wvsprintfW(WCHAR* out, const WCHAR* fmt, va_list ap) {
    char* f = utf8_dup(fmt);
    struct S : Sink { char buf[4100]; usize n; } s;
    s.put = [](Sink* k, const char* d, usize n) { S* x = (S*)k; for (usize i = 0; i < n && x->n < sizeof(x->buf) - 1; i++) x->buf[x->n++] = d[i]; };
    s.total = 0; s.n = 0;
    vformat(&s, f, ap, FMT_LEGACY_WIDE);
    s.buf[s.n] = 0;
    ifree(f);
    int w = wide_from_utf8(s.buf, -1, out, 1025);
    if (!w) { out[1024] = 0; w = 1025; }
    return w - 1;
}
static int CDECL wsprintfW(WCHAR* out, const WCHAR* fmt, ...) { va_list ap; va_start(ap, fmt); int n = wvsprintfW(out, fmt, ap); va_end(ap); return n; }

// strings from the program's resources (RT_STRING: blocks of 16 counted UTF-16 strings)
static int load_string(HMODULE m, u32 id, const WCHAR** str) {
    u32 size;
    WCHAR* block = (WCHAR*)res_lookup(m, (const WCHAR*)6, (const WCHAR*)(u64)((id >> 4) + 1), &size);
    if (!block) return 0;
    for (u32 i = 0; i < (id & 15); i++) block += 1 + (u16)*block;
    *str = block + 1;
    return (u16)*block;
}
static int WINAPI LoadStringW(HMODULE m, u32 id, WCHAR* out, int cap) {
    const WCHAR* s = nullptr;
    int n = load_string(m, id, &s);
    if (!cap) { *(const WCHAR**)out = s; return n; }             // a pointer into the resource itself
    if (!n) { if (cap) out[0] = 0; set_error(ERROR_RESOURCE_NAME_NOT_FOUND); return 0; }
    if (n >= cap) n = cap - 1;
    memcpy(out, s, (usize)n * 2);
    out[n] = 0;
    return n;
}
static int WINAPI LoadStringA(HMODULE m, u32 id, char* out, int cap) {
    const WCHAR* s = nullptr;
    int n = load_string(m, id, &s);
    if (!n || cap <= 0) { if (cap > 0) out[0] = 0; return 0; }
    int k = utf8_from_wide(s, n, nullptr, 0);
    char* tmp = (char*)ialloc((usize)k + 1);
    utf8_from_wide(s, n, tmp, k);
    tmp[k] = 0;
    slcpy(out, tmp, (usize)cap);
    ifree(tmp);
    return (int)slen(out);
}

static const Export user_text[] = {
    E(CharUpperW), E(CharLowerW), E(CharUpperA), E(CharLowerA), E(CharUpperBuffW), E(CharLowerBuffW), E(CharUpperBuffA), E(CharLowerBuffA),
    E(CharNextA), E(CharNextW), E(CharPrevA), E(CharPrevW), E(IsCharAlphaW), E(IsCharAlphaNumericW), E(IsCharUpperW), E(IsCharLowerW),
    E(IsCharAlphaA), E(IsCharAlphaNumericA), E(IsCharUpperA), E(IsCharLowerA), E(CharToOemA), E(OemToCharA), E(CharToOemBuffA),
    E(OemToCharBuffA), E(wvsprintfA), E(wsprintfA), E(wvsprintfW), E(wsprintfW), E(LoadStringW), E(LoadStringA),
};
EXPORT_TABLE("user32.dll", user_text);

}  // namespace rt
