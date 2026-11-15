// win32run - handles, Windows paths, files and the console.
//
// Paths: "C:\home\notes.txt", "c:/home/notes.txt", "\home\notes.txt" and "notes.txt" (relative to the
// current directory) all mean silicon's /home/notes.txt. Every drive letter is silicon's root. Names are
// matched without regard to case, as on Windows.
//
// Files: silicon's file calls read and write whole files, so an open file is a buffer: read in when it
// is opened, changed in memory, written back when the last handle closes (or on FlushFileBuffers and
// when the program exits). Handles to the same file share one buffer.
//
// The console: standard output and error go to the shell that started the program (and to the serial
// log); standard input is what is typed there, a line at a time.
#include "rt.h"

namespace rt {

// ---------------------------------------------------------------- handles
static const int MAX_HANDLES = 4096;
static Obj* table[MAX_HANDLES];
static Lock handle_lock;
HANDLE std_handles[3];
char cur_dir[MAX_PATH] = "/home";

void obj_ref(Obj* o) { __sync_fetch_and_add(&o->refs, 1); }
void obj_unref(Obj* o) {
    if (__sync_sub_and_fetch(&o->refs, 1) == 0 && o->destroy) o->destroy(o);
}

HANDLE h_new(Obj* o) {
    Guard g(handle_lock);
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!table[i]) { table[i] = o; return (HANDLE)(u64)(i * 4); }
    obj_unref(o);
    set_error(ERROR_TOO_MANY_OPEN_FILES);
    return nullptr;
}

static Obj* thread_obj_for(Thr* t);

Obj* h_get(HANDLE h, ObjType type) {
    u64 v = (u64)h;
    if (v == (u64)-2) {                                          // GetCurrentThread()
        Obj* o = thread_obj_for(cur_thr());
        if (o && (type == OBJ_NONE || type == OBJ_THREAD)) return o;
    }
    if (v & 3 || v / 4 >= MAX_HANDLES || !v) { set_error(ERROR_INVALID_HANDLE); return nullptr; }
    Obj* o = table[v / 4];
    if (!o || (type != OBJ_NONE && o->type != type)) { set_error(ERROR_INVALID_HANDLE); return nullptr; }
    return o;
}

bool h_close(HANDLE h) {
    u64 v = (u64)h;
    if (v == (u64)-1 || v == (u64)-2) return true;              // pseudo handles
    if (v & 3 || v / 4 >= MAX_HANDLES || !v) { set_error(ERROR_INVALID_HANDLE); return false; }
    Obj* o;
    {
        Guard g(handle_lock);
        o = table[v / 4];
        if (!o) { set_error(ERROR_INVALID_HANDLE); return false; }
        table[v / 4] = nullptr;
    }
    for (int i = 0; i < 3; i++) if (std_handles[i] == h) std_handles[i] = nullptr;
    obj_unref(o);
    return true;
}

HANDLE h_dup(HANDLE h) {
    Obj* o = h_get(h);
    if (!o) return nullptr;
    obj_ref(o);
    return h_new(o);
}

// thread objects live in sync.cpp; this file only needs to hand one out for GetCurrentThread
Obj* thread_obj(Thr* t);
static Obj* thread_obj_for(Thr* t) { return t ? thread_obj(t) : nullptr; }

// ---------------------------------------------------------------- paths
static bool name_eq_ci(const char* a, const char* b) { return !sicmp(a, b); }

// out holds a silicon path; fix the case of each part to what is on disk (parts that don't exist stay as given)
static void fix_case(char* path) {
    char built[MAX_PATH] = "";
    const char* p = path;
    bool exists = true;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char part[MAX_PATH];
        usize n = 0;
        while (*p && *p != '/' && n + 1 < sizeof(part)) part[n++] = *p++;
        part[n] = 0;
        char dir[MAX_PATH];
        slcpy(dir, built[0] ? built : "/", sizeof(dir));
        if (exists) {
            char cand[MAX_PATH];
            snfmt(cand, sizeof(cand), "%s/%s", built, part);
            si::Stat st;
            if (si::file_stat(cand, &st) != 0) {
                char ent[64];
                bool found = false;
                for (int i = 0;; i++) {
                    int k = si::dir_entry(dir, i, ent, sizeof(ent));
                    if (k <= 0) break;
                    if (name_eq_ci(ent, part)) { slcpy(part, ent, sizeof(part)); found = true; break; }
                }
                if (!found) exists = false;
            }
        }
        slcat(built, "/", sizeof(built));
        slcat(built, part, sizeof(built));
    }
    slcpy(path, built[0] ? built : "/", MAX_PATH);
}

bool path_to_silicon(const char* win, char* out, usize cap) {
    if (!win || !*win) return false;
    char tmp[MAX_PATH * 2];
    const char* s = win;
    if (!sncmp(s, "\\\\?\\", 4) || !sncmp(s, "\\??\\", 4)) s += 4;
    if (!sncmp(s, "\\\\.\\", 4)) return false;                    // devices
    usize n = 0;
    bool absolute = false;
    if (((s[0] | 32) >= 'a' && (s[0] | 32) <= 'z') && s[1] == ':') {
        s += 2;
        absolute = *s == '\\' || *s == '/';
    } else absolute = *s == '\\' || *s == '/';
    if (!absolute) { slcpy(tmp, cur_dir, sizeof(tmp)); n = slen(tmp); tmp[n++] = '/'; }
    for (; *s && n + 1 < sizeof(tmp); s++) tmp[n++] = *s == '\\' ? '/' : *s;
    tmp[n] = 0;
    // normalize: no empty, "." or ".." parts
    char res[MAX_PATH * 2];
    usize r = 0;
    const char* p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* e = p;
        while (*e && *e != '/') e++;
        usize len = (usize)(e - p);
        // Windows ignores trailing dots and spaces in a name
        while (len && (p[len - 1] == ' ' || (p[len - 1] == '.' && !(len == 2 && p[0] == '.') && !(len == 1)))) len--;
        if (len == 1 && p[0] == '.') {}
        else if (len == 2 && p[0] == '.' && p[1] == '.') { while (r && res[r - 1] != '/') r--; if (r) r--; }
        else if (len) {
            if (r + len + 2 >= sizeof(res)) return false;
            res[r++] = '/';
            memcpy(res + r, p, len);
            r += len;
        }
        p = e;
    }
    if (!r) res[r++] = '/';
    res[r] = 0;
    for (usize i = 0; i < r; i++) if (res[i] == ':' || res[i] == '*' || res[i] == '?' || res[i] == '"' || res[i] == '<' || res[i] == '>' || res[i] == '|') return false;
    if (r >= cap) return false;
    slcpy(out, res, cap);
    fix_case(out);
    return true;
}

bool wpath_to_silicon(const WCHAR* win, char* out, usize cap) {
    if (!win) return false;
    char u[MAX_PATH * 3];
    if (!utf8_from_wide(win, -1, u, sizeof(u))) return false;
    return path_to_silicon(u, out, cap);
}

void path_to_windows(const char* s, char* out, usize cap) {
    usize n = 0;
    if (cap < 4) { if (cap) out[0] = 0; return; }
    out[n++] = 'C'; out[n++] = ':';
    if (*s != '/') out[n++] = '\\';
    for (; *s && n + 1 < cap; s++) out[n++] = *s == '/' ? '\\' : *s;
    if (n == 2) out[n++] = '\\';
    out[n] = 0;
}

static bool is_device(const char* win, int* which) {             // NUL, CON, CONIN$, CONOUT$
    const char* b = win;
    for (const char* p = win; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
    if (!sicmp(b, "nul") || !sicmp(win, "\\\\.\\nul")) { *which = 3; return true; }
    if (!sicmp(b, "conin$")) { *which = 0; return true; }
    if (!sicmp(b, "conout$") || !sicmp(b, "con")) { *which = 1; return true; }
    return false;
}

// ---------------------------------------------------------------- file contents
static FileData* open_files = nullptr;
static Lock file_lock;

bool file_flush(FileData* fd) {
    if (!fd->dirty) return true;
    if (si::file_write(fd->path, fd->data, (int)fd->size) != 0) return false;
    fd->dirty = false;
    return true;
}

void files_flush_all() {
    for (FileData* fd = open_files; fd; fd = fd->next) file_flush(fd);
}

static FileData* data_for(const char* path, bool load) {
    for (FileData* fd = open_files; fd; fd = fd->next) if (!scmp(fd->path, path)) { fd->refs++; return fd; }
    FileData* fd = (FileData*)izalloc(sizeof(FileData));
    if (!fd) return nullptr;
    slcpy(fd->path, path, sizeof(fd->path));
    if (load) {
        int n = si::file_size(path);
        if (n > 0) {
            fd->cap = (usize)n + 64;
            fd->data = (u8*)ialloc(fd->cap);
            if (!fd->data || si::file_read(path, fd->data, n) != n) { ifree(fd->data); ifree(fd); return nullptr; }
            fd->size = (usize)n;
        }
    }
    fd->refs = 1;
    fd->next = open_files;
    open_files = fd;
    return fd;
}

static void data_release(FileData* fd) {
    Guard g(file_lock);
    if (--fd->refs > 0) return;
    if (fd->delete_on_close) si::file_delete(fd->path);
    else file_flush(fd);
    for (FileData** p = &open_files; *p; p = &(*p)->next) if (*p == fd) { *p = fd->next; break; }
    ifree(fd->data);
    ifree(fd);
}

static void file_destroy(Obj* o) {
    FileObj* f = (FileObj*)o;
    if (f->fd) data_release(f->fd);
    if (f->dirpath) ifree(f->dirpath);
    ifree(f);
}

static bool reserve(FileData* fd, usize n) {
    if (n <= fd->cap) return true;
    usize nc = fd->cap ? fd->cap : 256;
    while (nc < n) nc *= 2;
    u8* nd = (u8*)ialloc(nc);
    if (!nd) return false;
    if (fd->data) { memcpy(nd, fd->data, fd->size); ifree(fd->data); }
    fd->data = nd;
    fd->cap = nc;
    return true;
}

enum { GENERIC_READ = 0x80000000u, GENERIC_WRITE = 0x40000000u, GENERIC_ALL = 0x10000000u, FILE_APPEND_DATA = 4, FILE_WRITE_DATA = 2 };
enum { CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4, TRUNCATE_EXISTING = 5 };
enum { FILE_FLAG_BACKUP_SEMANTICS = 0x02000000, FILE_FLAG_DELETE_ON_CLOSE = 0x04000000 };

static bool parent_exists(const char* path) {
    char dir[MAX_PATH];
    slcpy(dir, path, sizeof(dir));
    char* sl = (char*)srchr(dir, '/');
    if (!sl) return false;
    if (sl == dir) return true;
    *sl = 0;
    si::Stat st;
    return si::file_stat(dir, &st) == 0 && (st.flags & 1);
}

FileObj* file_open(const char* path, u32 access, u32 disp, u32 flags, u32* error) {
    *error = 0;
    si::Stat st;
    bool exists = si::file_stat(path, &st) == 0;
    bool want_write = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    if (exists && (st.flags & 1)) {
        if (!(flags & FILE_FLAG_BACKUP_SEMANTICS) || disp == CREATE_NEW) { *error = disp == CREATE_NEW ? ERROR_ALREADY_EXISTS : ERROR_ACCESS_DENIED; return nullptr; }
        FileObj* f = (FileObj*)izalloc(sizeof(FileObj));
        f->type = OBJ_FILE; f->refs = 1; f->destroy = file_destroy;
        f->dirpath = sdup(path);
        f->access = access;
        return f;
    }
    if (exists && disp == CREATE_NEW) { *error = ERROR_FILE_EXISTS; return nullptr; }
    if (!exists && (disp == OPEN_EXISTING || disp == TRUNCATE_EXISTING)) { *error = parent_exists(path) ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND; return nullptr; }
    if (!exists && !parent_exists(path)) { *error = ERROR_PATH_NOT_FOUND; return nullptr; }
    if (want_write && exists && (st.flags & 2)) { *error = ERROR_ACCESS_DENIED; return nullptr; }
    bool truncate = disp == CREATE_ALWAYS || disp == TRUNCATE_EXISTING;
    Guard g(file_lock);
    if (!exists) {
        if (si::file_write(path, "", 0) != 0) { *error = ERROR_ACCESS_DENIED; return nullptr; }
    }
    FileData* fd = data_for(path, exists && !truncate);
    if (!fd) { *error = ERROR_NOT_ENOUGH_MEMORY; return nullptr; }
    if (truncate && fd->size) { fd->size = 0; fd->dirty = true; }
    if (truncate && exists) { fd->dirty = true; }
    if (flags & FILE_FLAG_DELETE_ON_CLOSE) fd->delete_on_close = true;
    FileObj* f = (FileObj*)izalloc(sizeof(FileObj));
    f->type = OBJ_FILE; f->refs = 1; f->destroy = file_destroy;
    f->fd = fd;
    f->access = access;
    f->append = (access & FILE_APPEND_DATA) && !(access & (GENERIC_WRITE | FILE_WRITE_DATA));
    if (exists && (disp == OPEN_ALWAYS || disp == CREATE_ALWAYS)) *error = ERROR_ALREADY_EXISTS;    // success, but says so
    return f;
}

i64 file_read(FileObj* f, void* buf, usize n) {
    if (!f->fd) return -1;
    Guard g(file_lock);
    FileData* fd = f->fd;
    if (f->pos >= fd->size) return 0;
    usize k = fd->size - f->pos;
    if (k > n) k = n;
    memcpy(buf, fd->data + f->pos, k);
    f->pos += k;
    return (i64)k;
}

i64 file_write(FileObj* f, const void* buf, usize n) {
    if (!f->fd) return -1;
    Guard g(file_lock);
    FileData* fd = f->fd;
    if (f->append) f->pos = fd->size;
    u64 end = f->pos + n;
    if (end > (8u << 20)) { set_error(ERROR_DISK_FULL); return -1; }        // what one silicon write can hold
    if (!reserve(fd, end)) { set_error(ERROR_NOT_ENOUGH_MEMORY); return -1; }
    if (f->pos > fd->size) memset(fd->data + fd->size, 0, f->pos - fd->size);
    memcpy(fd->data + f->pos, buf, n);
    f->pos = end;
    if (end > fd->size) fd->size = end;
    fd->dirty = true;
    return (i64)n;
}

static bool file_set_size(FileObj* f, u64 n) {
    if (!f->fd) return false;
    Guard g(file_lock);
    if (n > (8u << 20) || !reserve(f->fd, n)) return false;
    if (n > f->fd->size) memset(f->fd->data + f->fd->size, 0, n - f->fd->size);
    f->fd->size = n;
    f->fd->dirty = true;
    return true;
}

// ---------------------------------------------------------------- the console
static char in_buf[2048];
static int in_len = 0, in_pos = 0;
static bool in_eof = false;
static Lock in_lock;

static void con_destroy(Obj* o) { ifree(o); }
static void nul_destroy(Obj* o) { ifree(o); }

i64 con_read_line(char* buf, usize cap) {
    Guard g(in_lock);
    u32 spins = 0;
    while (in_pos >= in_len) {
        if (in_eof) return 0;
        int n = si::con_read(in_buf, sizeof(in_buf));
        if (n < 0) { in_eof = true; return 0; }
        if (n == 0) { nap(spins++ + 16); continue; }
        in_len = n;
        in_pos = 0;
    }
    usize k = 0;
    while (in_pos < in_len && k < cap) {
        char c = in_buf[in_pos++];
        buf[k++] = c;
        if (c == '\n') break;
    }
    return (i64)k;
}

void handles_init() {
    for (int i = 0; i < 3; i++) {
        ConsoleObj* c = (ConsoleObj*)izalloc(sizeof(ConsoleObj));
        c->type = OBJ_CONSOLE; c->refs = 1; c->destroy = con_destroy;
        c->stream = i;
        c->mode = i == 0 ? 0x1F7 : 0x7;                         // input: processed, line, echo...; output: processed, wrap, VT
        std_handles[i] = h_new(c);
    }
}

// ---------------------------------------------------------------- kernel32: files
enum { FILE_ATTRIBUTE_READONLY = 1, FILE_ATTRIBUTE_HIDDEN = 2, FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_ARCHIVE = 0x20,
       FILE_ATTRIBUTE_NORMAL = 0x80 };
static const u32 INVALID_FILE_ATTRIBUTES = 0xFFFFFFFF;

static HANDLE open_common(const char* win, u32 access, u32 disp, u32 flags) {
    int dev;
    if (is_device(win, &dev)) {
        if (dev == 3) {
            Obj* o = (Obj*)izalloc(sizeof(Obj));
            o->type = OBJ_NUL; o->refs = 1; o->destroy = nul_destroy;
            return h_new(o);
        }
        ConsoleObj* c = (ConsoleObj*)izalloc(sizeof(ConsoleObj));
        c->type = OBJ_CONSOLE; c->refs = 1; c->destroy = con_destroy;
        c->stream = dev;
        c->mode = dev == 0 ? 0x1F7 : 0x7;
        return h_new(c);
    }
    char path[MAX_PATH];
    if (!path_to_silicon(win, path, sizeof(path))) { set_error(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    u32 err;
    FileObj* f = file_open(path, access, disp, flags, &err);
    if (!f) { trace("CreateFile %s: error %u", path, err); set_error(err); return INVALID_HANDLE_VALUE; }
    HANDLE h = h_new(f);
    if (!h) return INVALID_HANDLE_VALUE;
    set_error(err);
    return h;
}

static HANDLE WINAPI CreateFileA(const char* name, u32 access, u32 share, SECURITY_ATTRIBUTES* sa, u32 disp, u32 flags, HANDLE tmpl) {
    (void)share; (void)sa; (void)tmpl;
    if (!name) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    return open_common(name, access, disp, flags);
}
static HANDLE WINAPI CreateFileW(const WCHAR* name, u32 access, u32 share, SECURITY_ATTRIBUTES* sa, u32 disp, u32 flags, HANDLE tmpl) {
    (void)share; (void)sa; (void)tmpl;
    if (!name) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    char u[MAX_PATH * 3];
    if (!utf8_from_wide(name, -1, u, sizeof(u))) { set_error(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    return open_common(u, access, disp, flags);
}
struct CREATEFILE2_EXTENDED_PARAMETERS { u32 dwSize, dwFileAttributes, dwFileFlags, dwSecurityQosFlags; SECURITY_ATTRIBUTES* lpSecurityAttributes; HANDLE hTemplateFile; };
static HANDLE WINAPI CreateFile2(const WCHAR* name, u32 access, u32 share, u32 disp, CREATEFILE2_EXTENDED_PARAMETERS* ex) {
    return CreateFileW(name, access, share, nullptr, disp, ex ? ex->dwFileFlags : 0, nullptr);
}

static BOOL WINAPI ReadFile(HANDLE h, void* buf, u32 n, u32* got, OVERLAPPED* ov) {
    if (got) *got = 0;
    Obj* o = h_get(h);
    if (!o) return FALSE;
    if (o->type == OBJ_CONSOLE) {
        i64 k = con_read_line((char*)buf, n);
        if (got) *got = (u32)k;
        return TRUE;
    }
    if (o->type == OBJ_NUL) return TRUE;
    if (o->type == OBJ_PIPE) {
        extern i64 pipe_read(Obj* o, void* buf, u32 n);
        i64 k = pipe_read(o, buf, n);
        if (k < 0) { set_error(ERROR_BROKEN_PIPE); return FALSE; }
        if (got) *got = (u32)k;
        return TRUE;
    }
    if (o->type != OBJ_FILE) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    FileObj* f = (FileObj*)o;
    if (!f->fd) { set_error(ERROR_INVALID_FUNCTION); return FALSE; }
    if (ov) f->pos = (u64)ov->Offset | ((u64)ov->OffsetHigh << 32);
    i64 k = file_read(f, buf, n);
    if (got) *got = (u32)k;
    if (ov) { ov->Internal = 0; ov->InternalHigh = (u64)k; if (!k && n) { set_error(ERROR_HANDLE_EOF); return FALSE; } }
    return TRUE;
}

static BOOL WINAPI WriteFile(HANDLE h, const void* buf, u32 n, u32* put, OVERLAPPED* ov) {
    if (put) *put = 0;
    Obj* o = h_get(h);
    if (!o) return FALSE;
    if (o->type == OBJ_CONSOLE) {
        con_out((const char*)buf, n);
        if (put) *put = n;
        return TRUE;
    }
    if (o->type == OBJ_NUL) { if (put) *put = n; return TRUE; }
    if (o->type == OBJ_PIPE) {
        extern i64 pipe_write(Obj* o, const void* buf, u32 n);
        i64 k = pipe_write(o, buf, n);
        if (k < 0) { set_error(ERROR_BROKEN_PIPE); return FALSE; }
        if (put) *put = (u32)k;
        return TRUE;
    }
    if (o->type != OBJ_FILE) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    FileObj* f = (FileObj*)o;
    if (!f->fd || !(f->access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA))) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
    if (ov) f->pos = (u64)ov->Offset | ((u64)ov->OffsetHigh << 32);
    i64 k = file_write(f, buf, n);
    if (k < 0) return FALSE;
    if (put) *put = (u32)k;
    if (ov) { ov->Internal = 0; ov->InternalHigh = (u64)k; }
    return TRUE;
}

static BOOL WINAPI SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER* newpos, u32 method) {
    FileObj* f = (FileObj*)h_get(h, OBJ_FILE);
    if (!f) {
        Obj* o = h_get(h);
        if (o && (o->type == OBJ_CONSOLE || o->type == OBJ_NUL)) { if (newpos) newpos->QuadPart = 0; return TRUE; }
        return FALSE;
    }
    if (!f->fd) { set_error(ERROR_INVALID_FUNCTION); return FALSE; }
    i64 base = method == 0 ? 0 : method == 1 ? (i64)f->pos : (i64)f->fd->size;
    i64 np = base + dist.QuadPart;
    if (np < 0) { set_error(ERROR_NEGATIVE_SEEK); return FALSE; }
    f->pos = (u64)np;
    if (newpos) newpos->QuadPart = np;
    return TRUE;
}

static u32 WINAPI SetFilePointer(HANDLE h, i32 lo, i32* hi, u32 method) {
    LARGE_INTEGER d, np;
    d.QuadPart = hi ? (i64)(((u64)(u32)*hi << 32) | (u32)lo) : (i64)lo;
    if (!SetFilePointerEx(h, d, &np, method)) return 0xFFFFFFFF;
    if (hi) *hi = np.HighPart;
    set_error(0);
    return np.LowPart;
}

static BOOL WINAPI GetFileSizeEx(HANDLE h, LARGE_INTEGER* sz) {
    FileObj* f = (FileObj*)h_get(h, OBJ_FILE);
    if (!f) return FALSE;
    sz->QuadPart = f->fd ? (i64)f->fd->size : 0;
    return TRUE;
}
static u32 WINAPI GetFileSize(HANDLE h, u32* hi) {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(h, &s)) return 0xFFFFFFFF;
    if (hi) *hi = (u32)s.HighPart;
    return s.LowPart;
}

static BOOL WINAPI SetEndOfFile(HANDLE h) {
    FileObj* f = (FileObj*)h_get(h, OBJ_FILE);
    if (!f) return FALSE;
    if (!file_set_size(f, f->pos)) { set_error(ERROR_DISK_FULL); return FALSE; }
    return TRUE;
}

static BOOL WINAPI FlushFileBuffers(HANDLE h) {
    Obj* o = h_get(h);
    if (!o) return FALSE;
    if (o->type != OBJ_FILE || !((FileObj*)o)->fd) return TRUE;
    Guard g(file_lock);
    if (!file_flush(((FileObj*)o)->fd)) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
    return TRUE;
}

static BOOL WINAPI CloseHandle(HANDLE h) { return h_close(h); }

enum { FILE_TYPE_UNKNOWN = 0, FILE_TYPE_DISK = 1, FILE_TYPE_CHAR = 2, FILE_TYPE_PIPE = 3 };
static u32 WINAPI GetFileType(HANDLE h) {
    Obj* o = h_get(h);
    if (!o) return FILE_TYPE_UNKNOWN;
    if (o->type == OBJ_FILE) return FILE_TYPE_DISK;
    if (o->type == OBJ_CONSOLE || o->type == OBJ_NUL) return FILE_TYPE_CHAR;
    if (o->type == OBJ_PIPE) return FILE_TYPE_PIPE;
    set_error(0);
    return FILE_TYPE_UNKNOWN;
}

static u32 attrs_of(const si::Stat& st, const char* name) {
    u32 a = (st.flags & 1) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    if (st.flags & 2) a |= FILE_ATTRIBUTE_READONLY;
    const char* b = srchr(name, '/');
    b = b ? b + 1 : name;
    if (b[0] == '.' && b[1] && scmp(b, "..")) a |= FILE_ATTRIBUTE_HIDDEN;
    return a;
}

static u32 attrs_path(const char* win) {
    char path[MAX_PATH];
    if (!path_to_silicon(win, path, sizeof(path))) { set_error(ERROR_INVALID_NAME); return INVALID_FILE_ATTRIBUTES; }
    si::Stat st;
    if (si::file_stat(path, &st) != 0) { set_error(parent_exists(path) ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND); return INVALID_FILE_ATTRIBUTES; }
    return attrs_of(st, path);
}
static u32 WINAPI GetFileAttributesA(const char* n) { return n ? attrs_path(n) : INVALID_FILE_ATTRIBUTES; }
static u32 WINAPI GetFileAttributesW(const WCHAR* n) {
    if (!n) return INVALID_FILE_ATTRIBUTES;
    char u[MAX_PATH * 3];
    utf8_from_wide(n, -1, u, sizeof(u));
    return attrs_path(u);
}

struct WIN32_FILE_ATTRIBUTE_DATA { u32 dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; u32 nFileSizeHigh, nFileSizeLow; };
static void set_ft(FILETIME& f, u32 stamp) { u64 v = stamp_to_filetime(stamp); f.dwLowDateTime = (u32)v; f.dwHighDateTime = (u32)(v >> 32); }

static BOOL attr_ex(const char* win, WIN32_FILE_ATTRIBUTE_DATA* d) {
    char path[MAX_PATH];
    if (!path_to_silicon(win, path, sizeof(path))) { set_error(ERROR_INVALID_NAME); return FALSE; }
    si::Stat st;
    if (si::file_stat(path, &st) != 0) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    memset(d, 0, sizeof(*d));
    d->dwFileAttributes = attrs_of(st, path);
    set_ft(d->ftCreationTime, st.ctime);
    set_ft(d->ftLastAccessTime, st.mtime);
    set_ft(d->ftLastWriteTime, st.mtime);
    d->nFileSizeLow = st.size;
    // a file open here may have grown past what is on disk
    for (FileData* fd = open_files; fd; fd = fd->next) if (!scmp(fd->path, path)) d->nFileSizeLow = (u32)fd->size;
    return TRUE;
}
static BOOL WINAPI GetFileAttributesExA(const char* n, int lvl, WIN32_FILE_ATTRIBUTE_DATA* d) { (void)lvl; return attr_ex(n, d); }
static BOOL WINAPI GetFileAttributesExW(const WCHAR* n, int lvl, WIN32_FILE_ATTRIBUTE_DATA* d) {
    (void)lvl;
    char u[MAX_PATH * 3];
    utf8_from_wide(n, -1, u, sizeof(u));
    return attr_ex(u, d);
}
static BOOL WINAPI SetFileAttributesA(const char* n, u32 a) { (void)a; return attrs_path(n) != INVALID_FILE_ATTRIBUTES; }
static BOOL WINAPI SetFileAttributesW(const WCHAR* n, u32 a) { (void)a; return GetFileAttributesW(n) != INVALID_FILE_ATTRIBUTES; }

struct BY_HANDLE_FILE_INFORMATION {
    u32 dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    u32 dwVolumeSerialNumber, nFileSizeHigh, nFileSizeLow, nNumberOfLinks, nFileIndexHigh, nFileIndexLow;
};
static BOOL WINAPI GetFileInformationByHandle(HANDLE h, BY_HANDLE_FILE_INFORMATION* info) {
    FileObj* f = (FileObj*)h_get(h, OBJ_FILE);
    if (!f) return FALSE;
    const char* path = f->fd ? f->fd->path : f->dirpath;
    si::Stat st;
    memset(info, 0, sizeof(*info));
    if (si::file_stat(path, &st) == 0) {
        info->dwFileAttributes = attrs_of(st, path);
        set_ft(info->ftCreationTime, st.ctime);
        set_ft(info->ftLastAccessTime, st.mtime);
        set_ft(info->ftLastWriteTime, st.mtime);
    }
    info->nFileSizeLow = f->fd ? (u32)f->fd->size : 0;
    info->dwVolumeSerialNumber = 0x51C0;
    info->nNumberOfLinks = 1;
    u32 hash = 2166136261u;
    for (const char* p = path; *p; p++) hash = (hash ^ (u8)*p) * 16777619u;
    info->nFileIndexLow = hash;
    return TRUE;
}

static BOOL WINAPI GetFileTime(HANDLE h, FILETIME* c, FILETIME* a, FILETIME* w) {
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle(h, &i)) return FALSE;
    if (c) *c = i.ftCreationTime;
    if (a) *a = i.ftLastAccessTime;
    if (w) *w = i.ftLastWriteTime;
    return TRUE;
}
static BOOL WINAPI SetFileTime(HANDLE h, const FILETIME* c, const FILETIME* a, const FILETIME* w) {
    (void)c; (void)a; (void)w;
    return h_get(h, OBJ_FILE) != nullptr;
}

static BOOL del_path(const char* win) {
    char path[MAX_PATH];
    if (!path_to_silicon(win, path, sizeof(path))) { set_error(ERROR_INVALID_NAME); return FALSE; }
    si::Stat st;
    if (si::file_stat(path, &st) != 0) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    if (st.flags & 1) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
    Guard g(file_lock);
    for (FileData* fd = open_files; fd; fd = fd->next)
        if (!scmp(fd->path, path)) { fd->delete_on_close = true; return TRUE; }      // goes when the last handle closes
    if (si::file_delete(path) != 0) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
    return TRUE;
}
static BOOL WINAPI DeleteFileA(const char* n) { return n ? del_path(n) : FALSE; }
static BOOL WINAPI DeleteFileW(const WCHAR* n) { char u[MAX_PATH * 3]; utf8_from_wide(n, -1, u, sizeof(u)); return del_path(u); }

static BOOL mkdir_path(const char* win) {
    char path[MAX_PATH];
    if (!path_to_silicon(win, path, sizeof(path))) { set_error(ERROR_INVALID_NAME); return FALSE; }
    si::Stat st;
    if (si::file_stat(path, &st) == 0) { set_error(ERROR_ALREADY_EXISTS); return FALSE; }
    if (!parent_exists(path)) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    if (si::make_dir(path) != 0) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
    return TRUE;
}
static BOOL WINAPI CreateDirectoryA(const char* n, SECURITY_ATTRIBUTES* sa) { (void)sa; return mkdir_path(n); }
static BOOL WINAPI CreateDirectoryW(const WCHAR* n, SECURITY_ATTRIBUTES* sa) { (void)sa; char u[MAX_PATH * 3]; utf8_from_wide(n, -1, u, sizeof(u)); return mkdir_path(u); }

static BOOL rmdir_path(const char* win) {
    char path[MAX_PATH];
    if (!path_to_silicon(win, path, sizeof(path))) { set_error(ERROR_INVALID_NAME); return FALSE; }
    si::Stat st;
    if (si::file_stat(path, &st) != 0) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    if (!(st.flags & 1)) { set_error(267); return FALSE; }
    char ent[64];
    if (si::dir_entry(path, 0, ent, sizeof(ent)) > 0) { set_error(ERROR_DIR_NOT_EMPTY); return FALSE; }
    if (si::file_delete(path) != 0) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
    return TRUE;
}
static BOOL WINAPI RemoveDirectoryA(const char* n) { return rmdir_path(n); }
static BOOL WINAPI RemoveDirectoryW(const WCHAR* n) { char u[MAX_PATH * 3]; utf8_from_wide(n, -1, u, sizeof(u)); return rmdir_path(u); }

enum { MOVEFILE_REPLACE_EXISTING = 1, MOVEFILE_COPY_ALLOWED = 2 };
static BOOL move_path(const char* from_w, const char* to_w, u32 flags) {
    char from[MAX_PATH], to[MAX_PATH];
    if (!path_to_silicon(from_w, from, sizeof(from)) || !path_to_silicon(to_w, to, sizeof(to))) { set_error(ERROR_INVALID_NAME); return FALSE; }
    si::Stat st;
    if (si::file_stat(from, &st) != 0) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    files_flush_all();
    if (si::file_stat(to, &st) == 0) {
        if (!(flags & MOVEFILE_REPLACE_EXISTING)) { set_error(ERROR_ALREADY_EXISTS); return FALSE; }
        if (si::file_delete(to) != 0) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
    }
    if (si::file_rename(from, to) != 0) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
    Guard g(file_lock);
    for (FileData* fd = open_files; fd; fd = fd->next) if (!scmp(fd->path, from)) slcpy(fd->path, to, sizeof(fd->path));
    return TRUE;
}
static BOOL WINAPI MoveFileA(const char* a, const char* b) { return move_path(a, b, 0); }
static BOOL WINAPI MoveFileW(const WCHAR* a, const WCHAR* b) {
    char ua[MAX_PATH * 3], ub[MAX_PATH * 3];
    utf8_from_wide(a, -1, ua, sizeof(ua)); utf8_from_wide(b, -1, ub, sizeof(ub));
    return move_path(ua, ub, 0);
}
static BOOL WINAPI MoveFileExA(const char* a, const char* b, u32 f) { return b ? move_path(a, b, f) : del_path(a); }
static BOOL WINAPI MoveFileExW(const WCHAR* a, const WCHAR* b, u32 f) {
    char ua[MAX_PATH * 3], ub[MAX_PATH * 3];
    utf8_from_wide(a, -1, ua, sizeof(ua));
    if (!b) return del_path(ua);
    utf8_from_wide(b, -1, ub, sizeof(ub));
    return move_path(ua, ub, f);
}

static BOOL copy_path(const char* from_w, const char* to_w, BOOL fail_if_exists) {
    char from[MAX_PATH], to[MAX_PATH];
    if (!path_to_silicon(from_w, from, sizeof(from)) || !path_to_silicon(to_w, to, sizeof(to))) { set_error(ERROR_INVALID_NAME); return FALSE; }
    files_flush_all();
    si::Stat st;
    if (si::file_stat(from, &st) != 0 || (st.flags & 1)) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    if (fail_if_exists && si::file_stat(to, &st) == 0) { set_error(ERROR_FILE_EXISTS); return FALSE; }
    int n = si::file_size(from);
    u8* buf = (u8*)ialloc((usize)n + 1);
    if (!buf) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    bool ok = si::file_read(from, buf, n) == n && si::file_write(to, buf, n) == 0;
    ifree(buf);
    if (!ok) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
    return TRUE;
}
static BOOL WINAPI CopyFileA(const char* a, const char* b, BOOL f) { return copy_path(a, b, f); }
static BOOL WINAPI CopyFileW(const WCHAR* a, const WCHAR* b, BOOL f) {
    char ua[MAX_PATH * 3], ub[MAX_PATH * 3];
    utf8_from_wide(a, -1, ua, sizeof(ua)); utf8_from_wide(b, -1, ub, sizeof(ub));
    return copy_path(ua, ub, f);
}

// ---------------------------------------------------------------- finding files
struct WIN32_FIND_DATAA { u32 dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; u32 nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; char cFileName[MAX_PATH]; char cAlternateFileName[14]; };
struct WIN32_FIND_DATAW { u32 dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; u32 nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; WCHAR cFileName[MAX_PATH]; WCHAR cAlternateFileName[14]; };
struct FindObj : Obj { char dir[MAX_PATH]; char pattern[MAX_PATH]; int index; int dots; };

static void find_destroy(Obj* o) { ifree(o); }

static bool wild_match(const char* pat, const char* s) {
    if (!scmp(pat, "*") || !scmp(pat, "*.*")) return true;
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return true;
            for (const char* t = s; *t; t++) if (wild_match(pat, t)) return true;
            return wild_match(pat, "");
        }
        if (!*s) {
            if (*pat == '.' && !pat[1]) return true;               // "name." matches "name"
            return false;
        }
        if (*pat != '?' && lower((u8)*pat) != lower((u8)*s)) return false;
        pat++; s++;
    }
    return !*s;
}

// the next entry that matches, as Windows describes it
static bool find_next(FindObj* f, char* name, usize cap, u32* attrs, u32* size, u32* mtime, u32* ctime) {
    while (f->dots < 2) {
        const char* d = f->dots == 0 ? "." : "..";
        f->dots++;
        if (!scmp(f->dir, "/") || !wild_match(f->pattern, d)) continue;
        slcpy(name, d, cap);
        *attrs = FILE_ATTRIBUTE_DIRECTORY; *size = 0; *mtime = *ctime = 0;
        return true;
    }
    char ent[64];
    for (;;) {
        int k = si::dir_entry(f->dir, f->index++, ent, sizeof(ent));
        if (k <= 0) return false;
        if (!wild_match(f->pattern, ent)) continue;
        char full[MAX_PATH];
        snfmt(full, sizeof(full), "%s%s%s", f->dir, scmp(f->dir, "/") ? "/" : "", ent);
        si::Stat st;
        if (si::file_stat(full, &st) != 0) memset(&st, 0, sizeof(st));
        slcpy(name, ent, cap);
        *attrs = attrs_of(st, full);
        *size = st.size;
        for (FileData* fd = open_files; fd; fd = fd->next) if (!scmp(fd->path, full)) *size = (u32)fd->size;
        *mtime = st.mtime;
        *ctime = st.ctime;
        return true;
    }
}

static FindObj* find_start(const char* win) {
    char path[MAX_PATH];
    // the pattern is the last part; it may hold wildcards, which the path conversion refuses
    char dirpart[MAX_PATH * 3];
    slcpy(dirpart, win, sizeof(dirpart));
    char* sl = nullptr;
    for (char* p = dirpart; *p; p++) if (*p == '\\' || *p == '/') sl = p;
    const char* pat;
    if (sl) { *sl = 0; pat = sl + 1; if (!dirpart[0] || (dirpart[1] == ':' && !dirpart[2])) slcat(dirpart, "\\", sizeof(dirpart)); }
    else if (dirpart[0] && dirpart[1] == ':') { pat = dirpart + 2; dirpart[2] = 0; }
    else { pat = win; scpy(dirpart, "."); }
    char patbuf[MAX_PATH];
    slcpy(patbuf, pat, sizeof(patbuf));
    if (!patbuf[0]) { set_error(ERROR_FILE_NOT_FOUND); return nullptr; }
    if (!path_to_silicon(dirpart, path, sizeof(path))) { set_error(ERROR_PATH_NOT_FOUND); return nullptr; }
    si::Stat st;
    if (si::file_stat(path, &st) != 0 || !(st.flags & 1)) { set_error(ERROR_PATH_NOT_FOUND); return nullptr; }
    FindObj* f = (FindObj*)izalloc(sizeof(FindObj));
    f->type = OBJ_FIND; f->refs = 1; f->destroy = find_destroy;
    slcpy(f->dir, path, sizeof(f->dir));
    slcpy(f->pattern, patbuf, sizeof(f->pattern));
    // a pattern without wildcards that names a folder lists that entry itself, not its contents
    return f;
}

static void fill_a(WIN32_FIND_DATAA* d, const char* name, u32 attrs, u32 size, u32 mt, u32 ct) {
    memset(d, 0, sizeof(*d));
    d->dwFileAttributes = attrs;
    d->nFileSizeLow = size;
    set_ft(d->ftCreationTime, ct); set_ft(d->ftLastAccessTime, mt); set_ft(d->ftLastWriteTime, mt);
    slcpy(d->cFileName, name, sizeof(d->cFileName));
}
static void fill_w(WIN32_FIND_DATAW* d, const char* name, u32 attrs, u32 size, u32 mt, u32 ct) {
    memset(d, 0, sizeof(*d));
    d->dwFileAttributes = attrs;
    d->nFileSizeLow = size;
    set_ft(d->ftCreationTime, ct); set_ft(d->ftLastAccessTime, mt); set_ft(d->ftLastWriteTime, mt);
    wide_from_utf8(name, -1, d->cFileName, MAX_PATH);
}

static HANDLE WINAPI FindFirstFileA(const char* pat, WIN32_FIND_DATAA* d) {
    FindObj* f = find_start(pat);
    if (!f) return INVALID_HANDLE_VALUE;
    char name[MAX_PATH]; u32 a, s, mt, ct;
    if (!find_next(f, name, sizeof(name), &a, &s, &mt, &ct)) { ifree(f); set_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    fill_a(d, name, a, s, mt, ct);
    HANDLE h = h_new(f);
    return h ? h : INVALID_HANDLE_VALUE;
}
static HANDLE WINAPI FindFirstFileW(const WCHAR* pat, WIN32_FIND_DATAW* d) {
    char u[MAX_PATH * 3];
    utf8_from_wide(pat, -1, u, sizeof(u));
    FindObj* f = find_start(u);
    if (!f) return INVALID_HANDLE_VALUE;
    char name[MAX_PATH]; u32 a, s, mt, ct;
    if (!find_next(f, name, sizeof(name), &a, &s, &mt, &ct)) { ifree(f); set_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    fill_w(d, name, a, s, mt, ct);
    HANDLE h = h_new(f);
    return h ? h : INVALID_HANDLE_VALUE;
}
static HANDLE WINAPI FindFirstFileExA(const char* pat, int lvl, WIN32_FIND_DATAA* d, int op, void* filter, u32 flags) {
    (void)lvl; (void)op; (void)filter; (void)flags;
    return FindFirstFileA(pat, d);
}
static HANDLE WINAPI FindFirstFileExW(const WCHAR* pat, int lvl, WIN32_FIND_DATAW* d, int op, void* filter, u32 flags) {
    (void)lvl; (void)op; (void)filter; (void)flags;
    return FindFirstFileW(pat, d);
}
static BOOL WINAPI FindNextFileA(HANDLE h, WIN32_FIND_DATAA* d) {
    FindObj* f = (FindObj*)h_get(h, OBJ_FIND);
    if (!f) return FALSE;
    char name[MAX_PATH]; u32 a, s, mt, ct;
    if (!find_next(f, name, sizeof(name), &a, &s, &mt, &ct)) { set_error(ERROR_NO_MORE_FILES); return FALSE; }
    fill_a(d, name, a, s, mt, ct);
    return TRUE;
}
static BOOL WINAPI FindNextFileW(HANDLE h, WIN32_FIND_DATAW* d) {
    FindObj* f = (FindObj*)h_get(h, OBJ_FIND);
    if (!f) return FALSE;
    char name[MAX_PATH]; u32 a, s, mt, ct;
    if (!find_next(f, name, sizeof(name), &a, &s, &mt, &ct)) { set_error(ERROR_NO_MORE_FILES); return FALSE; }
    fill_w(d, name, a, s, mt, ct);
    return TRUE;
}
static BOOL WINAPI FindClose(HANDLE h) { if (!h_get(h, OBJ_FIND)) return FALSE; return h_close(h); }

// ---------------------------------------------------------------- directories and names
static u32 put_a(const char* s, char* out, u32 cap) {           // Windows' rule: size without NUL when it fits, else size needed with it
    u32 n = (u32)slen(s);
    if (!out || cap <= n) return n + 1;
    memcpy(out, s, n + 1);
    return n;
}
static u32 put_w(const char* s, WCHAR* out, u32 cap) {
    u32 n = (u32)wide_from_utf8(s, -1, nullptr, 0) - 1;
    if (!out || cap <= n) return n + 1;
    wide_from_utf8(s, -1, out, (int)cap);
    return n;
}

static u32 WINAPI GetCurrentDirectoryA(u32 cap, char* out) { char w[MAX_PATH]; path_to_windows(cur_dir, w, sizeof(w)); return put_a(w, out, cap); }
static u32 WINAPI GetCurrentDirectoryW(u32 cap, WCHAR* out) { char w[MAX_PATH]; path_to_windows(cur_dir, w, sizeof(w)); return put_w(w, out, cap); }
static BOOL set_cwd(const char* win) {
    char path[MAX_PATH];
    if (!path_to_silicon(win, path, sizeof(path))) { set_error(ERROR_INVALID_NAME); return FALSE; }
    si::Stat st;
    if (si::file_stat(path, &st) != 0 || !(st.flags & 1)) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    slcpy(cur_dir, path, sizeof(cur_dir));
    return TRUE;
}
static BOOL WINAPI SetCurrentDirectoryA(const char* p) { return set_cwd(p); }
static BOOL WINAPI SetCurrentDirectoryW(const WCHAR* p) { char u[MAX_PATH * 3]; utf8_from_wide(p, -1, u, sizeof(u)); return set_cwd(u); }

static u32 full_path(const char* in, char* out_win, usize cap, usize* file_part) {
    char path[MAX_PATH];
    if (!path_to_silicon(in, path, sizeof(path))) return 0;
    path_to_windows(path, out_win, cap);
    usize n = slen(in);
    if (n && (in[n - 1] == '\\' || in[n - 1] == '/') && slen(out_win) > 3) slcat(out_win, "\\", cap);
    const char* b = srchr(out_win, '\\');
    if (file_part) *file_part = b && b[1] ? (usize)(b + 1 - out_win) : (usize)-1;
    return 1;
}
static u32 WINAPI GetFullPathNameA(const char* in, u32 cap, char* out, char** file) {
    char w[MAX_PATH];
    usize fp;
    if (!in || !full_path(in, w, sizeof(w), &fp)) { set_error(ERROR_INVALID_NAME); return 0; }
    u32 r = put_a(w, out, cap);
    if (file && out && r < cap) *file = fp == (usize)-1 ? nullptr : out + fp;
    return r;
}
static u32 WINAPI GetFullPathNameW(const WCHAR* in, u32 cap, WCHAR* out, WCHAR** file) {
    char u[MAX_PATH * 3], w[MAX_PATH];
    usize fp;
    if (!in) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    utf8_from_wide(in, -1, u, sizeof(u));
    if (!full_path(u, w, sizeof(w), &fp)) { set_error(ERROR_INVALID_NAME); return 0; }
    u32 r = put_w(w, out, cap);
    if (file && out && r < cap) {
        if (fp == (usize)-1) *file = nullptr;
        else { WCHAR* last = nullptr; for (WCHAR* p = out; *p; p++) if (*p == '\\') last = p; *file = last ? last + 1 : out; }
    }
    return r;
}
static u32 WINAPI GetLongPathNameA(const char* in, char* out, u32 cap) { return put_a(in, out, cap); }
static u32 WINAPI GetLongPathNameW(const WCHAR* in, WCHAR* out, u32 cap) {
    u32 n = (u32)wlen(in);
    if (!out || cap <= n) return n + 1;
    memmove(out, in, (n + 1) * 2);
    return n;
}
static u32 WINAPI GetShortPathNameW(const WCHAR* in, WCHAR* out, u32 cap) { return GetLongPathNameW(in, out, cap); }

static void ensure_tmp() { si::make_dir("/home/tmp"); }
static u32 WINAPI GetTempPathA(u32 cap, char* out) { ensure_tmp(); return put_a("C:\\home\\tmp\\", out, cap); }
static u32 WINAPI GetTempPathW(u32 cap, WCHAR* out) { ensure_tmp(); return put_w("C:\\home\\tmp\\", out, cap); }
static u32 WINAPI GetTempPath2W(u32 cap, WCHAR* out) { return GetTempPathW(cap, out); }
static u32 tmp_counter = 0;
static u32 temp_name(const char* dir, const char* prefix, u32 unique, char* out) {
    for (int tries = 0; tries < 1000; tries++) {
        u32 n = unique ? unique : ((u32)si::ticks() + ++tmp_counter) & 0xFFFF;
        char pre[4] = {0};
        for (int i = 0; i < 3 && prefix && prefix[i]; i++) pre[i] = prefix[i];
        usize dl = slen(dir);
        snfmt(out, MAX_PATH, "%s%s%s%X.tmp", dir, dl && dir[dl - 1] != '\\' && dir[dl - 1] != '/' ? "\\" : "", pre, n);
        if (unique) return n;
        char path[MAX_PATH];
        si::Stat st;
        if (path_to_silicon(out, path, sizeof(path)) && si::file_stat(path, &st) != 0) {
            si::file_write(path, "", 0);
            return n;
        }
    }
    return 0;
}
static u32 WINAPI GetTempFileNameA(const char* dir, const char* prefix, u32 unique, char* out) { return temp_name(dir, prefix, unique, out); }
static u32 WINAPI GetTempFileNameW(const WCHAR* dir, const WCHAR* prefix, u32 unique, WCHAR* out) {
    char d[MAX_PATH * 3], p[16], o[MAX_PATH];
    utf8_from_wide(dir, -1, d, sizeof(d));
    if (prefix) utf8_from_wide(prefix, -1, p, sizeof(p)); else p[0] = 0;
    u32 r = temp_name(d, p, unique, o);
    if (r) wide_from_utf8(o, -1, out, MAX_PATH);
    return r;
}

static u32 WINAPI GetLogicalDrives() { return 1u << 2; }    // just C:
static u32 WINAPI GetLogicalDriveStringsA(u32 cap, char* out) { if (cap < 5) return 5; memcpy(out, "C:\\\0", 5); return 4; }
static u32 WINAPI GetDriveTypeA(const char* root) { (void)root; return 3; }      // DRIVE_FIXED
static u32 WINAPI GetDriveTypeW(const WCHAR* root) { (void)root; return 3; }
static BOOL WINAPI GetDiskFreeSpaceExA(const char* d, u64* avail, u64* total, u64* free_) {
    (void)d;
    if (avail) *avail = 32ull << 20;
    if (total) *total = 48ull << 20;
    if (free_) *free_ = 32ull << 20;
    return TRUE;
}
static BOOL WINAPI GetDiskFreeSpaceExW(const WCHAR* d, u64* avail, u64* total, u64* free_) { (void)d; return GetDiskFreeSpaceExA(nullptr, avail, total, free_); }
static BOOL WINAPI GetVolumeInformationW(const WCHAR* root, WCHAR* name, u32 ncap, u32* serial, u32* maxlen, u32* flags, WCHAR* fs, u32 fscap) {
    (void)root;
    if (name && ncap) put_w("silicon", name, ncap);
    if (serial) *serial = 0x51C0;
    if (maxlen) *maxlen = 47;
    if (flags) *flags = 3;
    if (fs && fscap) put_w("SiliconFS", fs, fscap);
    return TRUE;
}

static u32 WINAPI GetFinalPathNameByHandleW(HANDLE h, WCHAR* out, u32 cap, u32 flags) {
    (void)flags;
    FileObj* f = (FileObj*)h_get(h, OBJ_FILE);
    if (!f) return 0;
    char w[MAX_PATH + 8] = "\\\\?\\";
    char p[MAX_PATH];
    path_to_windows(f->fd ? f->fd->path : f->dirpath, p, sizeof(p));
    slcat(w, p, sizeof(w));
    return put_w(w, out, cap);
}

// ---------------------------------------------------------------- console
static HANDLE WINAPI GetStdHandle(u32 which) {
    int i = which == (u32)-10 ? 0 : which == (u32)-11 ? 1 : which == (u32)-12 ? 2 : -1;
    if (i < 0) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    return std_handles[i];
}
static BOOL WINAPI SetStdHandle(u32 which, HANDLE h) {
    int i = which == (u32)-10 ? 0 : which == (u32)-11 ? 1 : which == (u32)-12 ? 2 : -1;
    if (i < 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
    std_handles[i] = h;
    return TRUE;
}

static BOOL WINAPI WriteConsoleA(HANDLE h, const void* buf, u32 n, u32* put, void* r) {
    (void)r;
    if (!h_get(h, OBJ_CONSOLE)) return FALSE;
    con_out((const char*)buf, n);
    if (put) *put = n;
    return TRUE;
}
static BOOL WINAPI WriteConsoleW(HANDLE h, const WCHAR* buf, u32 n, u32* put, void* r) {
    (void)r;
    if (!h_get(h, OBJ_CONSOLE)) return FALSE;
    char tmp[1024];
    u32 done = 0;
    while (done < n) {
        u32 k = n - done > 256 ? 256 : n - done;
        if (k == 256 && (u16)buf[done + k - 1] >= 0xD800 && (u16)buf[done + k - 1] < 0xDC00) k--;     // don't split a surrogate pair
        int b = utf8_from_wide(buf + done, (int)k, tmp, sizeof(tmp));
        con_out(tmp, (usize)b);
        done += k;
    }
    if (put) *put = n;
    return TRUE;
}

static BOOL WINAPI ReadConsoleA(HANDLE h, void* buf, u32 n, u32* got, void* ctl) {
    (void)ctl;
    if (!h_get(h, OBJ_CONSOLE)) return FALSE;
    i64 k = con_read_line((char*)buf, n);
    // the console hands lines over ending in CR LF
    if (k > 0 && ((char*)buf)[k - 1] == '\n' && (u32)k < n) { ((char*)buf)[k - 1] = '\r'; ((char*)buf)[k++] = '\n'; }
    if (got) *got = (u32)k;
    return TRUE;
}
static char wpending[8];
static int wpending_n = 0;
static BOOL WINAPI ReadConsoleW(HANDLE h, WCHAR* buf, u32 n, u32* got, void* ctl) {
    (void)ctl;
    if (!h_get(h, OBJ_CONSOLE)) return FALSE;
    char tmp[1024];
    usize have = (usize)wpending_n;
    memcpy(tmp, wpending, have);
    wpending_n = 0;
    i64 k = con_read_line(tmp + have, (n < 300 ? n : 300));
    if (k <= 0 && !have) { if (got) *got = 0; return TRUE; }
    usize len = have + (usize)(k > 0 ? k : 0);
    if (len && tmp[len - 1] == '\n') { tmp[len - 1] = '\r'; tmp[len++] = '\n'; }
    // keep an incomplete UTF-8 sequence for next time
    usize cut = len;
    for (usize i = len; i > 0 && i + 4 > len; i--) {
        u8 c = (u8)tmp[i - 1];
        if ((c & 0xC0) == 0xC0) { int need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2; if (i - 1 + (usize)need > len) cut = i - 1; break; }
        if (c < 0x80) break;
    }
    wpending_n = (int)(len - cut);
    memcpy(wpending, tmp + cut, (usize)wpending_n);
    int w = wide_from_utf8(tmp, (int)cut, buf, (int)n);
    if (got) *got = (u32)w;
    return TRUE;
}

static BOOL WINAPI GetConsoleMode(HANDLE h, u32* mode) {
    ConsoleObj* c = (ConsoleObj*)h_get(h, OBJ_CONSOLE);
    if (!c) return FALSE;
    *mode = c->mode;
    return TRUE;
}
static BOOL WINAPI SetConsoleMode(HANDLE h, u32 mode) {
    ConsoleObj* c = (ConsoleObj*)h_get(h, OBJ_CONSOLE);
    if (!c) return FALSE;
    c->mode = mode;
    return TRUE;
}
static u32 cp_out = 65001, cp_in = 65001;
static u32 WINAPI GetConsoleCP() { return cp_in; }
static u32 WINAPI GetConsoleOutputCP() { return cp_out; }
static BOOL WINAPI SetConsoleCP(u32 cp) { cp_in = cp; return TRUE; }
static BOOL WINAPI SetConsoleOutputCP(u32 cp) { cp_out = cp; return TRUE; }
static WCHAR con_title[256] = L"win32run";
static BOOL WINAPI SetConsoleTitleA(const char* t) { wide_from_utf8(t, -1, con_title, 256); return TRUE; }
static BOOL WINAPI SetConsoleTitleW(const WCHAR* t) { usize n = wlen(t); if (n > 255) n = 255; memcpy(con_title, t, n * 2); con_title[n] = 0; return TRUE; }
static u32 WINAPI GetConsoleTitleW(WCHAR* out, u32 cap) { u32 n = (u32)wlen(con_title); if (cap) { u32 k = n < cap - 1 ? n : cap - 1; memcpy(out, con_title, k * 2); out[k] = 0; } return n; }
static u32 WINAPI GetConsoleTitleA(char* out, u32 cap) { return (u32)utf8_from_wide(con_title, -1, out, (int)cap); }

struct COORD { i16 X, Y; };
struct SMALL_RECT { i16 Left, Top, Right, Bottom; };
struct CONSOLE_SCREEN_BUFFER_INFO { COORD dwSize, dwCursorPosition; u16 wAttributes; SMALL_RECT srWindow; COORD dwMaximumWindowSize; };
static BOOL WINAPI GetConsoleScreenBufferInfo(HANDLE h, CONSOLE_SCREEN_BUFFER_INFO* i) {
    ConsoleObj* c = (ConsoleObj*)h_get(h, OBJ_CONSOLE);
    if (!c || !c->stream) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    memset(i, 0, sizeof(*i));
    i->dwSize = {120, 9000};
    i->wAttributes = 7;
    i->srWindow = {0, 0, 119, 39};
    i->dwMaximumWindowSize = {120, 40};
    return TRUE;
}
static u16 cur_attr = 7;
static BOOL WINAPI SetConsoleTextAttribute(HANDLE h, u16 attr) {
    if (!h_get(h, OBJ_CONSOLE)) return FALSE;
    if (attr == cur_attr) return TRUE;
    cur_attr = attr;
    // Windows colours (blue = 1, green = 2, red = 4) as ANSI escapes
    static const int ansi[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    char b[32];
    int fg = ansi[attr & 7], bg = ansi[(attr >> 4) & 7];
    int n;
    if (attr == 7) n = snfmt(b, sizeof(b), "[0m");
    else if (bg) n = snfmt(b, sizeof(b), "[0;%d;%dm", (attr & 8) ? 90 + fg : 30 + fg, 40 + bg);
    else n = snfmt(b, sizeof(b), "[0;%dm", (attr & 8) ? 90 + fg : 30 + fg);
    con_out(b, (usize)n);
    return TRUE;
}
static BOOL WINAPI SetConsoleCursorPosition(HANDLE h, COORD c) {
    if (!h_get(h, OBJ_CONSOLE)) return FALSE;
    char b[32];
    int n = snfmt(b, sizeof(b), "\x1b[%d;%dH", c.Y + 1, c.X + 1);
    con_out(b, (usize)n);
    return TRUE;
}
static BOOL WINAPI FillConsoleOutputCharacterA(HANDLE h, char ch, u32 n, COORD c, u32* done) { (void)h; (void)ch; (void)c; if (done) *done = n; return TRUE; }
static BOOL WINAPI FillConsoleOutputCharacterW(HANDLE h, WCHAR ch, u32 n, COORD c, u32* done) { (void)h; (void)ch; (void)c; if (done) *done = n; return TRUE; }
static BOOL WINAPI FillConsoleOutputAttribute(HANDLE h, u16 a, u32 n, COORD c, u32* done) { (void)h; (void)a; (void)c; if (done) *done = n; return TRUE; }
struct CONSOLE_CURSOR_INFO { u32 dwSize; BOOL bVisible; };
static BOOL WINAPI GetConsoleCursorInfo(HANDLE h, CONSOLE_CURSOR_INFO* i) { (void)h; i->dwSize = 25; i->bVisible = TRUE; return TRUE; }
static BOOL WINAPI SetConsoleCursorInfo(HANDLE h, const CONSOLE_CURSOR_INFO* i) { (void)h; (void)i; return TRUE; }
typedef BOOL (WINAPI *CtrlHandler)(u32);
static BOOL WINAPI SetConsoleCtrlHandler(CtrlHandler h, BOOL add) { (void)h; (void)add; return TRUE; }
static HANDLE WINAPI GetConsoleWindow() { return nullptr; }
static BOOL WINAPI AllocConsole() { return TRUE; }
static BOOL WINAPI FreeConsole() { return TRUE; }
static BOOL WINAPI AttachConsole(u32 pid) { (void)pid; return TRUE; }
static BOOL WINAPI GetNumberOfConsoleInputEvents(HANDLE h, u32* n) { (void)h; *n = 0; return TRUE; }
static BOOL WINAPI FlushConsoleInputBuffer(HANDLE h) { (void)h; return TRUE; }
static BOOL WINAPI GetConsoleScreenBufferInfoEx(HANDLE h, void* i) { (void)h; (void)i; set_error(ERROR_NOT_SUPPORTED); return FALSE; }

static const Export file_exports[] = {
    E(CreateFileA), E(CreateFileW), E(CreateFile2), E(ReadFile), E(WriteFile), E(SetFilePointer), E(SetFilePointerEx),
    E(GetFileSize), E(GetFileSizeEx), E(SetEndOfFile), E(FlushFileBuffers), E(CloseHandle), E(GetFileType),
    E(GetFileAttributesA), E(GetFileAttributesW), E(GetFileAttributesExA), E(GetFileAttributesExW), E(SetFileAttributesA),
    E(SetFileAttributesW), E(GetFileInformationByHandle), E(GetFileTime), E(SetFileTime), E(DeleteFileA), E(DeleteFileW),
    E(CreateDirectoryA), E(CreateDirectoryW), E(RemoveDirectoryA), E(RemoveDirectoryW), E(MoveFileA), E(MoveFileW),
    E(MoveFileExA), E(MoveFileExW), E(CopyFileA), E(CopyFileW), E(FindFirstFileA), E(FindFirstFileW), E(FindFirstFileExA),
    E(FindFirstFileExW), E(FindNextFileA), E(FindNextFileW), E(FindClose), E(GetCurrentDirectoryA), E(GetCurrentDirectoryW),
    E(SetCurrentDirectoryA), E(SetCurrentDirectoryW), E(GetFullPathNameA), E(GetFullPathNameW), E(GetLongPathNameA),
    E(GetLongPathNameW), E(GetShortPathNameW), E(GetTempPathA), E(GetTempPathW), E(GetTempPath2W), E(GetTempFileNameA),
    E(GetTempFileNameW), E(GetLogicalDrives), E(GetLogicalDriveStringsA), E(GetDriveTypeA), E(GetDriveTypeW),
    E(GetDiskFreeSpaceExA), E(GetDiskFreeSpaceExW), E(GetVolumeInformationW), E(GetFinalPathNameByHandleW),
    E(GetStdHandle), E(SetStdHandle), E(WriteConsoleA), E(WriteConsoleW), E(ReadConsoleA), E(ReadConsoleW),
    E(GetConsoleMode), E(SetConsoleMode), E(GetConsoleCP), E(GetConsoleOutputCP), E(SetConsoleCP), E(SetConsoleOutputCP),
    E(SetConsoleTitleA), E(SetConsoleTitleW), E(GetConsoleTitleA), E(GetConsoleTitleW), E(GetConsoleScreenBufferInfo),
    E(SetConsoleTextAttribute), E(SetConsoleCursorPosition), E(FillConsoleOutputCharacterA), E(FillConsoleOutputCharacterW),
    E(FillConsoleOutputAttribute), E(GetConsoleCursorInfo), E(SetConsoleCursorInfo), E(SetConsoleCtrlHandler),
    E(GetConsoleWindow), E(AllocConsole), E(FreeConsole), E(AttachConsole), E(GetNumberOfConsoleInputEvents),
    E(FlushConsoleInputBuffer), E(GetConsoleScreenBufferInfoEx),
};
EXPORT_TABLE("kernel32.dll", file_exports);

}  // namespace rt
