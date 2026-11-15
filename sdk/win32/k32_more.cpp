// win32run - file mappings, the thread pool's work objects, and smaller kernel32/advapi32 pieces that real
// programs turned out to need (found by running nasm, binutils, make and glib-based tools).
//
// silicon has no shared memory between a file and an address space, so a file mapping is a private copy:
// the file's bytes are copied in when the mapping is made, and a writable mapping copies them back when a
// view is flushed or unmapped and when the mapping goes away. Every view of one mapping shares the copy,
// so two views of the same mapping see each other's writes, as on Windows.
#include "rt.h"

namespace rt {

// ---------------------------------------------------------------- file mappings
struct MapObj : Obj {
    u8*      mem;                       // the mapped bytes (whole pages)
    usize    size;                      // the mapping's size in bytes
    usize    alloc;                     // bytes of pages allocated
    FileObj* file;                      // null for memory backed by the page file
    bool     writable;
};
struct View { u8* addr; usize size; MapObj* map; View* next; };
static View* views;
static Lock map_lock;

static void map_write_back(MapObj* m) {
    if (!m->file || !m->writable) return;
    u64 pos = m->file->pos;
    bool app = m->file->append;
    m->file->pos = 0;
    m->file->append = false;
    file_write(m->file, m->mem, m->size);
    m->file->pos = pos;
    m->file->append = app;
}
static void map_destroy(Obj* o) {
    MapObj* m = (MapObj*)o;
    map_write_back(m);
    if (m->file) { file_flush(m->file->fd); obj_unref(m->file); }
    vm_free(m->mem, 0, MEM_RELEASE);
    ifree(m);
}

static HANDLE create_mapping(HANDLE file, u32 protect, u64 size) {
    u32 p = protect & 0xFF;
    bool writable = p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE;
    bool exec = p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
    FileObj* f = nullptr;
    if (file != INVALID_HANDLE_VALUE && file) {
        f = (FileObj*)h_get(file, OBJ_FILE);
        if (!f || !f->fd) { set_error(ERROR_INVALID_HANDLE); return nullptr; }
        if (!size) size = f->fd->size;
        if (!size) { set_error(1006); return nullptr; }          // ERROR_FILE_INVALID: an empty file can't be mapped
        if (size > f->fd->size && !writable) { set_error(ERROR_ACCESS_DENIED); return nullptr; }
    } else if (!size) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (size > (1ull << 32)) { set_error(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    usize alloc = (size + 0xFFFF) & ~0xFFFFull;
    u8* mem = (u8*)vm_alloc(nullptr, alloc, MEM_RESERVE | MEM_COMMIT, exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, MEM_MAPPED);
    if (!mem) { set_error(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    MapObj* m = (MapObj*)izalloc(sizeof(MapObj));
    m->type = OBJ_MAPPING; m->refs = 1; m->destroy = map_destroy;
    m->mem = mem; m->size = size; m->alloc = alloc; m->writable = writable;
    if (f) {
        obj_ref(f);
        m->file = f;
        usize have = f->fd->size < size ? f->fd->size : size;
        memcpy(mem, f->fd->data, have);
        if (size > f->fd->size) map_write_back(m);             // a mapping larger than its file makes the file that large
    }
    return h_new(m);
}
static HANDLE WINAPI CreateFileMappingW(HANDLE file, SECURITY_ATTRIBUTES* sa, u32 protect, u32 hi, u32 lo, const WCHAR* name) {
    (void)sa; (void)name;
    return create_mapping(file, protect, ((u64)hi << 32) | lo);
}
static HANDLE WINAPI CreateFileMappingA(HANDLE file, SECURITY_ATTRIBUTES* sa, u32 protect, u32 hi, u32 lo, const char* name) {
    (void)sa; (void)name;
    return create_mapping(file, protect, ((u64)hi << 32) | lo);
}
static HANDLE WINAPI CreateFileMappingFromApp(HANDLE file, SECURITY_ATTRIBUTES* sa, u32 protect, u64 size, const WCHAR* name) {
    (void)sa; (void)name;
    return create_mapping(file, protect, size);
}
// mappings have no names that another process could open
static HANDLE WINAPI OpenFileMappingW(u32 a, BOOL i, const WCHAR* n) { (void)a; (void)i; (void)n; set_error(ERROR_FILE_NOT_FOUND); return nullptr; }
static HANDLE WINAPI OpenFileMappingA(u32 a, BOOL i, const char* n) { (void)a; (void)i; (void)n; set_error(ERROR_FILE_NOT_FOUND); return nullptr; }

static void* WINAPI MapViewOfFileEx(HANDLE h, u32 access, u32 hi, u32 lo, SIZE_T n, void* base) {
    (void)access;
    MapObj* m = (MapObj*)h_get(h, OBJ_MAPPING);
    if (!m) return nullptr;
    if (base) { set_error(ERROR_INVALID_ADDRESS); return nullptr; }     // views live inside the mapping's copy
    u64 off = ((u64)hi << 32) | lo;
    if (off & 0xFFFF) { set_error(ERROR_INVALID_PARAMETER); return nullptr; }   // the allocation granularity, as on Windows
    if (off > m->size || (n && off + n > m->size)) { set_error(ERROR_ACCESS_DENIED); return nullptr; }
    View* v = (View*)izalloc(sizeof(View));
    v->addr = m->mem + off;
    v->size = n ? n : m->size - off;
    v->map = m;
    obj_ref(m);
    Guard g(map_lock);
    v->next = views;
    views = v;
    return v->addr;
}
static void* WINAPI MapViewOfFile(HANDLE h, u32 access, u32 hi, u32 lo, SIZE_T n) { return MapViewOfFileEx(h, access, hi, lo, n, nullptr); }
static void* WINAPI MapViewOfFileFromApp(HANDLE h, u32 access, u64 off, SIZE_T n) { return MapViewOfFileEx(h, access, (u32)(off >> 32), (u32)off, n, nullptr); }

static View* view_at(const void* addr, bool unlink) {
    Guard g(map_lock);
    for (View** pv = &views; *pv; pv = &(*pv)->next) {
        View* v = *pv;
        if ((const u8*)addr >= v->addr && (const u8*)addr < v->addr + v->size) {
            if (unlink) *pv = v->next;
            return v;
        }
    }
    return nullptr;
}
static BOOL WINAPI UnmapViewOfFile(const void* addr) {
    View* v = view_at(addr, true);
    if (!v) { set_error(ERROR_INVALID_ADDRESS); return FALSE; }
    map_write_back(v->map);
    obj_unref(v->map);
    ifree(v);
    return TRUE;
}
static BOOL WINAPI UnmapViewOfFileEx(const void* addr, u32 flags) { (void)flags; return UnmapViewOfFile(addr); }
static BOOL WINAPI FlushViewOfFile(const void* addr, SIZE_T n) {
    (void)n;
    View* v = view_at(addr, false);
    if (!v) { set_error(ERROR_INVALID_ADDRESS); return FALSE; }
    map_write_back(v->map);
    if (v->map->file) file_flush(v->map->file->fd);
    return TRUE;
}

// ---------------------------------------------------------------- thread pool work objects
// each submitted callback runs on a thread of its own; Wait... waits until none are left running
struct TpWork {
    void (WINAPI *cb)(void* instance, void* ctx, TpWork* work);
    void* ctx;
    volatile int pending;
};
struct TpRun { TpWork* w; void (WINAPI *simple)(void*, void*); void* ctx; };
static DWORD WINAPI tp_thread(void* p) {
    TpRun r = *(TpRun*)p;
    ifree(p);
    if (r.w) { r.w->cb(nullptr, r.w->ctx, r.w); __sync_fetch_and_sub(&r.w->pending, 1); }
    else r.simple(nullptr, r.ctx);
    return 0;
}
static bool tp_start(TpWork* w, void (WINAPI *simple)(void*, void*), void* ctx) {
    TpRun* r = (TpRun*)izalloc(sizeof(TpRun));
    r->w = w; r->simple = simple; r->ctx = ctx;
    Thr* t = thr_create(tp_thread, r, 256u << 10, false);
    if (!t) { ifree(r); return false; }
    thr_release(t);
    return true;
}
static TpWork* WINAPI CreateThreadpoolWork(void (WINAPI *cb)(void*, void*, TpWork*), void* ctx, void* env) {
    (void)env;
    TpWork* w = (TpWork*)izalloc(sizeof(TpWork));
    w->cb = cb;
    w->ctx = ctx;
    return w;
}
static void WINAPI SubmitThreadpoolWork(TpWork* w) {
    __sync_fetch_and_add(&w->pending, 1);
    if (!tp_start(w, nullptr, nullptr)) { __sync_fetch_and_sub(&w->pending, 1); msg("SubmitThreadpoolWork: could not start a thread"); }
}
static void WINAPI WaitForThreadpoolWorkCallbacks(TpWork* w, BOOL cancel) {
    (void)cancel;                                              // callbacks already started run to the end either way
    for (int i = 0; w->pending; i++) { if (i < 64) si::yield(); else si::sleep(1); }
}
static void WINAPI CloseThreadpoolWork(TpWork* w) { WaitForThreadpoolWorkCallbacks(w, FALSE); ifree(w); }
static BOOL WINAPI TrySubmitThreadpoolCallback(void (WINAPI *cb)(void*, void*), void* ctx, void* env) { (void)env; return tp_start(nullptr, cb, ctx); }
// QueueUserWorkItem's callback is a thread function: it gets the context as its one argument
static BOOL WINAPI QueueUserWorkItem(ThreadFn fn, void* ctx, u32 flags) {
    (void)flags;
    Thr* t = thr_create(fn, ctx, 256u << 10, false);
    if (!t) return FALSE;
    thr_release(t);
    return TRUE;
}

// ---------------------------------------------------------------- threads
// a thread handle from an id (the handle shares the thread's object, as CreateThread's does)
Obj* thread_obj(Thr* t);
static HANDLE WINAPI OpenThread(u32 access, BOOL inherit, u32 id) {
    (void)access; (void)inherit;
    Guard g(thread_lock);
    for (Thr* t = threads; t; t = t->next) {
        if (t->id != id || t->done) continue;
        Obj* o = thread_obj(t);
        obj_ref(o);
        return h_new(o);
    }
    set_error(ERROR_INVALID_PARAMETER);
    return nullptr;
}
// another thread's registers can't be read while it runs; the calling thread's can
static BOOL WINAPI GetThreadContext(HANDLE h, CONTEXT* c) {
    if ((i64)h == -2) { u32 flags = c->ContextFlags; rt_RtlCaptureContext(c); c->ContextFlags = flags; return TRUE; }
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}
static BOOL WINAPI SetThreadContext(HANDLE h, const CONTEXT* c) { (void)h; (void)c; set_error(ERROR_NOT_SUPPORTED); return FALSE; }

// waitable objects have no names that could be opened again
static HANDLE WINAPI OpenSemaphoreW(u32 a, BOOL i, const WCHAR* n) { (void)a; (void)i; (void)n; set_error(ERROR_FILE_NOT_FOUND); return nullptr; }
static HANDLE WINAPI OpenSemaphoreA(u32 a, BOOL i, const char* n) { (void)a; (void)i; (void)n; set_error(ERROR_FILE_NOT_FOUND); return nullptr; }

// ---------------------------------------------------------------- the rest
static BOOL WINAPI SetSystemTime(const SYSTEMTIME* t) { (void)t; set_error(1314); return FALSE; }     // ERROR_PRIVILEGE_NOT_HELD

typedef BOOL (WINAPI *LocEnumA)(char*);
static BOOL WINAPI EnumSystemLocalesA(LocEnumA f, u32 fl) { (void)fl; char s[] = "00000409"; f(s); return TRUE; }

// there are no devices to talk to
static BOOL WINAPI DeviceIoControl(HANDLE h, u32 code, void* in, u32 insz, void* out, u32 outsz, u32* got, void* ov) {
    (void)h; (void)code; (void)in; (void)insz; (void)out; (void)outsz; (void)ov;
    if (got) *got = 0;
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

// PathCchSkipRoot: past "C:\", "\\server\share\" or "\"
static i32 WINAPI PathCchSkipRoot(const WCHAR* p, const WCHAR** rest) {
    if (!p || !rest) return (i32)0x80070057;                    // E_INVALIDARG
    const WCHAR* s = p;
    if (s[0] == '\\' && s[1] == '\\') {
        s += 2;
        if (s[0] == '?' && s[1] == '\\') s += 2;
        if (((s[0] | 0x20) >= 'a' && (s[0] | 0x20) <= 'z') && s[1] == ':') { s += 2; if (*s == '\\') s++; *rest = s; return 0; }
        for (int part = 0; part < 2 && *s; part++) { while (*s && *s != '\\') s++; if (*s == '\\') s++; }
        *rest = s;
        return 0;
    }
    if (((s[0] | 0x20) >= 'a' && (s[0] | 0x20) <= 'z') && s[1] == ':') { s += 2; if (*s == '\\') s++; *rest = s; return 0; }
    if (s[0] == '\\') { *rest = s + 1; return 0; }
    return (i32)0x80070057;
}

// security descriptors: kept as plain memory, never checked
static BOOL WINAPI InitializeSecurityDescriptor(void* sd, u32 rev) { if (!sd || rev != 1) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } memset(sd, 0, 40); ((u8*)sd)[0] = 1; return TRUE; }
static BOOL WINAPI SetSecurityDescriptorDacl(void* sd, BOOL present, void* dacl, BOOL defaulted) {
    (void)defaulted;
    if (!sd) return FALSE;
    *(u16*)((u8*)sd + 2) |= present ? 4 : 0;                     // SE_DACL_PRESENT
    *(void**)((u8*)sd + 32) = dacl;
    return TRUE;
}
static BOOL WINAPI IsValidSecurityDescriptor(void* sd) { return sd && ((u8*)sd)[0] == 1; }
static i32 WINAPI RegLoadMUIStringW(HANDLE k, const WCHAR* v, WCHAR* out, u32 sz, u32* need, u32 f, const WCHAR* dir) {
    (void)k; (void)v; (void)out; (void)sz; (void)need; (void)f; (void)dir;
    return ERROR_FILE_NOT_FOUND;
}

static const Export more_exports[] = {
    E(CreateFileMappingW), E(CreateFileMappingA), E(CreateFileMappingFromApp), E(OpenFileMappingW), E(OpenFileMappingA),
    E(MapViewOfFile), E(MapViewOfFileEx), E(MapViewOfFileFromApp), E(UnmapViewOfFile), E(UnmapViewOfFileEx), E(FlushViewOfFile),
    E(CreateThreadpoolWork), E(SubmitThreadpoolWork), E(WaitForThreadpoolWorkCallbacks), E(CloseThreadpoolWork),
    E(TrySubmitThreadpoolCallback), E(QueueUserWorkItem), E(OpenThread), E(GetThreadContext), E(SetThreadContext),
    E(OpenSemaphoreW), E(OpenSemaphoreA), E(SetSystemTime), E(EnumSystemLocalesA), E(DeviceIoControl),
    E(PathCchSkipRoot),
};
EXPORT_TABLE("kernel32.dll", more_exports);

static const Export more_adv[] = {
    E(InitializeSecurityDescriptor), E(SetSecurityDescriptorDacl), E(IsValidSecurityDescriptor), E(RegLoadMUIStringW),
};
EXPORT_TABLE("advapi32.dll", more_adv);

}  // namespace rt
