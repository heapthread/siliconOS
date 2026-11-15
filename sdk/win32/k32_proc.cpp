// win32run - kernel32: memory (virtual, heaps, global/local), modules and resources, the process,
// thread-local storage, the environment, and system information.
#include "rt.h"

namespace rt {

// ---------------------------------------------------------------- virtual memory
static void* WINAPI VirtualAlloc(void* addr, SIZE_T size, u32 type, u32 protect) {
    if (type & 0x00080000) type |= MEM_COMMIT;                  // MEM_RESET: keep it as it is
    return vm_alloc(addr, size, type, protect);
}
static void* WINAPI VirtualAllocEx(HANDLE p, void* addr, SIZE_T size, u32 type, u32 protect) {
    if ((u64)p != (u64)-1) { set_error(ERROR_ACCESS_DENIED); return nullptr; }
    return VirtualAlloc(addr, size, type, protect);
}
struct MEM_EXTENDED_PARAMETER { u64 type_reserved; u64 value; };
static void* WINAPI VirtualAlloc2(HANDLE p, void* addr, SIZE_T size, u32 type, u32 protect, MEM_EXTENDED_PARAMETER* ex, u32 n) {
    (void)ex; (void)n;
    return VirtualAllocEx(p ? p : (HANDLE)(i64)-1, addr, size, type, protect);
}
static BOOL WINAPI VirtualFree(void* addr, SIZE_T size, u32 type) { return vm_free(addr, size, type); }
static BOOL WINAPI VirtualFreeEx(HANDLE p, void* addr, SIZE_T size, u32 type) { (void)p; return vm_free(addr, size, type); }
static BOOL WINAPI VirtualProtect(void* addr, SIZE_T size, u32 prot, u32* old) {
    u32 o = 0;
    // the runtime's own memory (below 4 GiB) is always read/write/execute; pretend
    if ((u64)addr >= 0x40000000ull && (u64)addr < 0x80000000ull) { if (old) *old = PAGE_EXECUTE_READWRITE; return TRUE; }
    if (!vm_protect(addr, size, prot, &o)) return FALSE;
    if (old) *old = o;
    return TRUE;
}
static BOOL WINAPI VirtualProtectEx(HANDLE p, void* addr, SIZE_T size, u32 prot, u32* old) { (void)p; return VirtualProtect(addr, size, prot, old); }
static SIZE_T WINAPI VirtualQuery(const void* addr, MEMORY_BASIC_INFORMATION* mbi, SIZE_T len) {
    if (len < sizeof(MEMORY_BASIC_INFORMATION)) { set_error(ERROR_BAD_LENGTH); return 0; }
    if ((u64)addr >= 0x40000000ull && (u64)addr < 0x80000000ull) {
        memset(mbi, 0, sizeof(*mbi));
        mbi->BaseAddress = (void*)((u64)addr & ~0xFFFull);
        mbi->AllocationBase = (void*)0x40000000ull;
        mbi->AllocationProtect = mbi->Protect = PAGE_EXECUTE_READWRITE;
        mbi->RegionSize = 0x1000;
        mbi->State = MEM_COMMIT;
        mbi->Type = MEM_PRIVATE;
        return sizeof(*mbi);
    }
    usize r = vm_query(addr, mbi);
    if (!r) set_error(ERROR_INVALID_PARAMETER);
    return r;
}
static SIZE_T WINAPI VirtualQueryEx(HANDLE p, const void* addr, MEMORY_BASIC_INFORMATION* mbi, SIZE_T len) { (void)p; return VirtualQuery(addr, mbi, len); }
static BOOL WINAPI VirtualLock(void* a, SIZE_T s) { (void)a; (void)s; return TRUE; }
static BOOL WINAPI VirtualUnlock(void* a, SIZE_T s) { (void)a; (void)s; return TRUE; }
static BOOL WINAPI FlushInstructionCache(HANDLE p, const void* a, SIZE_T s) { (void)p; (void)a; (void)s; return TRUE; }
static void WINAPI FlushProcessWriteBuffers() { __sync_synchronize(); }
static BOOL WINAPI IsBadReadPtr(const void* p, u64 n) { return !vm_readable(p, n); }
static BOOL WINAPI IsBadWritePtr(void* p, u64 n) { return !vm_writable(p, n); }
static BOOL WINAPI IsBadCodePtr(void* p) { return !vm_readable(p, 1); }
static BOOL WINAPI IsBadStringPtrA(const char* p, u64 n) { (void)n; return !vm_readable(p, 1); }
static BOOL WINAPI ReadProcessMemory(HANDLE p, const void* a, void* buf, SIZE_T n, SIZE_T* got) {
    if ((u64)p != (u64)-1 || !vm_readable(a, n)) { set_error(ERROR_NOACCESS); return FALSE; }
    memcpy(buf, a, n);
    if (got) *got = n;
    return TRUE;
}
static BOOL WINAPI WriteProcessMemory(HANDLE p, void* a, const void* buf, SIZE_T n, SIZE_T* put) {
    if ((u64)p != (u64)-1) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
    u32 old;
    if (!vm_writable(a, n) && !VirtualProtect(a, n, PAGE_EXECUTE_READWRITE, &old)) { set_error(ERROR_NOACCESS); return FALSE; }
    memcpy(a, buf, n);
    if (put) *put = n;
    return TRUE;
}

// ---------------------------------------------------------------- heaps
// Every heap is the runtime's one heap underneath; a heap handle only says which flags to apply.
enum { HEAP_NO_SERIALIZE = 1, HEAP_GENERATE_EXCEPTIONS = 4, HEAP_ZERO_MEMORY = 8, HEAP_REALLOC_IN_PLACE_ONLY = 0x10 };
static const u64 HEAP_MAGIC = 0x10000;
static u64 next_heap = HEAP_MAGIC;
static bool heap_ok(HANDLE h) { return (u64)h >= HEAP_MAGIC && (u64)h <= next_heap && !((u64)h & 0xFFFF); }

static void heap_fail(u32 flags) {
    if (flags & HEAP_GENERATE_EXCEPTIONS) {
        rt_RaiseException(0xC0000017, 0, 0, nullptr);           // STATUS_NO_MEMORY
    }
    set_error(ERROR_NOT_ENOUGH_MEMORY);
}

static HANDLE WINAPI GetProcessHeap() { return (HANDLE)HEAP_MAGIC; }
static HANDLE WINAPI HeapCreate(u32 opts, SIZE_T init, SIZE_T max) { (void)opts; (void)init; (void)max; next_heap += 0x10000; return (HANDLE)next_heap; }
static BOOL WINAPI HeapDestroy(HANDLE h) { if (!heap_ok(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; } return TRUE; }
static void* WINAPI HeapAlloc(HANDLE h, u32 flags, SIZE_T n) {
    (void)h;
    void* p = h_alloc(n, flags & HEAP_ZERO_MEMORY);
    if (!p) heap_fail(flags);
    return p;
}
static void* WINAPI HeapReAlloc(HANDLE h, u32 flags, void* p, SIZE_T n) {
    (void)h;
    if (!p) { set_error(ERROR_INVALID_PARAMETER); return nullptr; }
    usize old = h_size(p);
    if (old == (usize)-1) { set_error(ERROR_INVALID_PARAMETER); return nullptr; }
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
        if (n > old) { set_error(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
        return p;
    }
    void* q = h_realloc(p, n, flags & HEAP_ZERO_MEMORY);
    if (!q) heap_fail(flags);
    return q;
}
static BOOL WINAPI HeapFree(HANDLE h, u32 flags, void* p) { (void)h; (void)flags; h_free(p); return TRUE; }
static SIZE_T WINAPI HeapSize(HANDLE h, u32 flags, const void* p) { (void)h; (void)flags; return h_size((void*)p); }
static BOOL WINAPI HeapValidate(HANDLE h, u32 flags, const void* p) { (void)h; (void)flags; return !p || h_size((void*)p) != (usize)-1; }
static SIZE_T WINAPI HeapCompact(HANDLE h, u32 f) { (void)h; (void)f; return 1u << 20; }
static BOOL WINAPI HeapLock(HANDLE h) { (void)h; return TRUE; }
static BOOL WINAPI HeapUnlock(HANDLE h) { (void)h; return TRUE; }
static BOOL WINAPI HeapWalk(HANDLE h, void* e) { (void)h; (void)e; set_error(ERROR_NO_MORE_ITEMS); return FALSE; }
static BOOL WINAPI HeapSetInformation(HANDLE h, int c, void* i, SIZE_T n) { (void)h; (void)c; (void)i; (void)n; return TRUE; }
static BOOL WINAPI HeapQueryInformation(HANDLE h, int c, void* i, SIZE_T n, SIZE_T* r) { (void)h; (void)c; if (n >= 4) *(u32*)i = 2; if (r) *r = 4; return TRUE; }
static u32 WINAPI GetProcessHeaps(u32 n, HANDLE* out) { if (n) out[0] = GetProcessHeap(); return 1; }

// GlobalAlloc / LocalAlloc: movable memory is just memory here, so a handle and its pointer are the same
enum { GMEM_MOVEABLE = 2, GMEM_ZEROINIT = 0x40 };
static HANDLE WINAPI GlobalAlloc(u32 flags, SIZE_T n) { void* p = h_alloc(n ? n : 1, flags & GMEM_ZEROINIT); if (!p) set_error(ERROR_NOT_ENOUGH_MEMORY); return p; }
static HANDLE WINAPI GlobalReAlloc(HANDLE h, SIZE_T n, u32 flags) { if (flags & 0x80) return h; void* p = h_realloc(h, n ? n : 1, flags & GMEM_ZEROINIT); if (!p) set_error(ERROR_NOT_ENOUGH_MEMORY); return p; }
static HANDLE WINAPI GlobalFree(HANDLE h) { h_free(h); return nullptr; }
static void* WINAPI GlobalLock(HANDLE h) { return h; }
static BOOL WINAPI GlobalUnlock(HANDLE h) { (void)h; set_error(0); return FALSE; }
static SIZE_T WINAPI GlobalSize(HANDLE h) { usize s = h_size(h); return s == (usize)-1 ? 0 : s; }
static HANDLE WINAPI GlobalHandle(const void* p) { return (HANDLE)p; }
static u32 WINAPI GlobalFlags(HANDLE h) { (void)h; return 0; }
struct MEMORYSTATUSEX { u32 dwLength, dwMemoryLoad; u64 ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual; };
static BOOL WINAPI GlobalMemoryStatusEx(MEMORYSTATUSEX* m) {
    u64 total = 160ull << 20, used = vm_committed();
    m->dwMemoryLoad = (u32)(used * 100 / total);
    m->ullTotalPhys = total;
    m->ullAvailPhys = used < total ? total - used : 0;
    m->ullTotalPageFile = total;
    m->ullAvailPageFile = m->ullAvailPhys;
    m->ullTotalVirtual = 0x7E00000000ull;
    m->ullAvailVirtual = 0x7E00000000ull - used;
    m->ullAvailExtendedVirtual = 0;
    return TRUE;
}
struct MEMORYSTATUS { u32 dwLength, dwMemoryLoad; u64 dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual; };
static void WINAPI GlobalMemoryStatus(MEMORYSTATUS* m) {
    MEMORYSTATUSEX e;
    GlobalMemoryStatusEx(&e);
    m->dwMemoryLoad = e.dwMemoryLoad; m->dwTotalPhys = e.ullTotalPhys; m->dwAvailPhys = e.ullAvailPhys;
    m->dwTotalPageFile = e.ullTotalPageFile; m->dwAvailPageFile = e.ullAvailPageFile;
    m->dwTotalVirtual = e.ullTotalVirtual; m->dwAvailVirtual = e.ullAvailVirtual;
}
static BOOL WINAPI GetPhysicallyInstalledSystemMemory(u64* kb) { *kb = 512 * 1024; return TRUE; }

// ---------------------------------------------------------------- modules
static u32 put_str(const char* s, char* out, u32 cap) {          // GetModuleFileName's rules: truncate, say so
    u32 n = (u32)slen(s);
    if (!cap) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    if (n >= cap) { memcpy(out, s, cap - 1); out[cap - 1] = 0; set_error(ERROR_INSUFFICIENT_BUFFER); return cap; }
    memcpy(out, s, n + 1);
    set_error(0);
    return n;
}

static Module* mod_of_handle(HANDLE h) { return h ? mod_by_base(h) : exe_module; }

static HMODULE WINAPI GetModuleHandleA(const char* name) {
    if (!name) return exe_module->base;
    Module* m = mod_find(name);
    if (!m) { set_error(ERROR_MOD_NOT_FOUND); return nullptr; }
    return m->base;
}
static HMODULE WINAPI GetModuleHandleW(const WCHAR* name) {
    if (!name) return exe_module->base;
    char u[MAX_PATH * 3];
    utf8_from_wide(name, -1, u, sizeof(u));
    return GetModuleHandleA(u);
}
enum { GET_MODULE_HANDLE_EX_FLAG_PIN = 1, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 2, GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 4 };
static BOOL WINAPI GetModuleHandleExA(u32 flags, const char* name, HMODULE* out) {
    Module* m;
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        m = mod_by_addr(name);
        if (!m && (u64)name >= 0x40000000ull && (u64)name < 0x48000000ull) m = mod_find("kernel32.dll");   // a runtime function
    } else m = name ? mod_find(name) : exe_module;
    if (!m) { *out = nullptr; set_error(ERROR_MOD_NOT_FOUND); return FALSE; }
    *out = m->base;
    return TRUE;
}
static BOOL WINAPI GetModuleHandleExW(u32 flags, const WCHAR* name, HMODULE* out) {
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) || !name) return GetModuleHandleExA(flags, (const char*)name, out);
    char u[MAX_PATH * 3];
    utf8_from_wide(name, -1, u, sizeof(u));
    return GetModuleHandleExA(flags, u, out);
}
static u32 WINAPI GetModuleFileNameA(HMODULE h, char* out, u32 cap) {
    Module* m = mod_of_handle(h);
    if (!m) { set_error(ERROR_MOD_NOT_FOUND); return 0; }
    char u[MAX_PATH * 3];
    utf8_from_wide(m->wpath, -1, u, sizeof(u));
    return put_str(u, out, cap);
}
static u32 WINAPI GetModuleFileNameW(HMODULE h, WCHAR* out, u32 cap) {
    Module* m = mod_of_handle(h);
    if (!m) { set_error(ERROR_MOD_NOT_FOUND); return 0; }
    u32 n = (u32)wlen(m->wpath);
    if (!cap) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    if (n >= cap) { memcpy(out, m->wpath, (cap - 1) * 2); out[cap - 1] = 0; set_error(ERROR_INSUFFICIENT_BUFFER); return cap; }
    memcpy(out, m->wpath, (n + 1) * 2);
    set_error(0);
    return n;
}
static HMODULE load_lib(const char* name) {
    int err = 0;
    Module* m = mod_load(name, &err);
    if (!m) { trace("LoadLibrary(%s) failed: %d", name, err); set_error((u32)(err ? err : ERROR_MOD_NOT_FOUND)); return nullptr; }
    trace("LoadLibrary(%s) -> %s", name, m->name);
    return m->base;
}
static HMODULE WINAPI LoadLibraryA(const char* n) { if (!n) { set_error(ERROR_INVALID_PARAMETER); return nullptr; } return load_lib(n); }
static HMODULE WINAPI LoadLibraryW(const WCHAR* n) {
    if (!n) { set_error(ERROR_INVALID_PARAMETER); return nullptr; }
    char u[MAX_PATH * 3];
    utf8_from_wide(n, -1, u, sizeof(u));
    return load_lib(u);
}
static HMODULE WINAPI LoadLibraryExA(const char* n, HANDLE f, u32 flags) { (void)f; (void)flags; return LoadLibraryA(n); }
static HMODULE WINAPI LoadLibraryExW(const WCHAR* n, HANDLE f, u32 flags) { (void)f; (void)flags; return LoadLibraryW(n); }
static BOOL WINAPI FreeLibrary(HMODULE h) { Module* m = mod_by_base(h); if (!m) { set_error(ERROR_INVALID_HANDLE); return FALSE; } if (m->refs > 1) m->refs--; return TRUE; }
static void WINAPI FreeLibraryAndExitThread(HMODULE h, u32 code) { FreeLibrary(h); thr_exit(code); }
static BOOL WINAPI DisableThreadLibraryCalls(HMODULE h) { (void)h; return TRUE; }
static BOOL WINAPI SetDllDirectoryA(const char* d) { (void)d; return TRUE; }
static BOOL WINAPI SetDllDirectoryW(const WCHAR* d) { (void)d; return TRUE; }
static void* WINAPI AddDllDirectory(const WCHAR* d) { (void)d; static int cookie; return &cookie; }
static BOOL WINAPI RemoveDllDirectory(void* c) { (void)c; return TRUE; }
static BOOL WINAPI SetDefaultDllDirectories(u32 f) { (void)f; return TRUE; }

static void* WINAPI GetProcAddress(HMODULE h, const char* name) {
    Module* m = mod_of_handle(h);
    if (!m) { set_error(ERROR_MOD_NOT_FOUND); return nullptr; }
    bool by_ord = (u64)name < 0x10000;
    void* f = by_ord ? mod_export(m, nullptr, (u32)(u64)name, true) : mod_export(m, name, 0, true);
    if (f) return f;
    // a function the runtime doesn't have: say so loudly, and hand out a trap so a call is caught too
    char nm[80];
    if (by_ord) snfmt(nm, sizeof(nm), "#%u", (u32)(u64)name);
    else slcpy(nm, name, sizeof(nm));
    if (!m->builtin) { set_error(ERROR_PROC_NOT_FOUND); return nullptr; }     // a real DLL really doesn't have it
    char line[200];
    int n = snfmt(line, sizeof(line), "[WIN32 UNIMPLEMENTED] %s!%s (looked up with GetProcAddress)\n", m->name, nm);
    con_out(line, (usize)n);
    return make_trap(m->name, nm);
}

// ---------------------------------------------------------------- resources
struct ResDir { u32 Characteristics, TimeDateStamp; u16 Major, Minor, NumberOfNamedEntries, NumberOfIdEntries; };
struct ResEntry { u32 Name, OffsetToData; };
struct ResData { u32 OffsetToData, Size, CodePage, Reserved; };

static bool res_name_matches(u8* root, ResEntry* e, const WCHAR* name) {
    if ((u64)name < 0x10000) return !(e->Name >> 31) && (e->Name & 0xFFFF) == (u64)name;
    if (name[0] == '#') {
        u32 id = 0;
        for (const WCHAR* p = name + 1; *p >= '0' && *p <= '9'; p++) id = id * 10 + (u32)(*p - '0');
        return !(e->Name >> 31) && (e->Name & 0xFFFF) == id;
    }
    if (!(e->Name >> 31)) return false;
    u16* s = (u16*)(root + (e->Name & 0x7FFFFFFF));
    u32 len = s[0];
    if (wlen(name) != len) return false;
    for (u32 i = 0; i < len; i++) if (upper(s[1 + i]) != upper(name[i])) return false;
    return true;
}

static ResEntry* res_find(u8* root, ResDir* dir, const WCHAR* name) {
    ResEntry* e = (ResEntry*)(dir + 1);
    u32 n = (u32)dir->NumberOfNamedEntries + dir->NumberOfIdEntries;
    if (!name) return n ? e : nullptr;                          // any (the first)
    for (u32 i = 0; i < n; i++) if (res_name_matches(root, &e[i], name)) return &e[i];
    return nullptr;
}

static ResData* find_res(Module* m, const WCHAR* type, const WCHAR* name, u16 lang) {
    u8* root = (u8*)image_dir(m, DIR_RESOURCE, nullptr);
    if (!root) return nullptr;
    ResEntry* te = res_find(root, (ResDir*)root, type);
    if (!te || !(te->OffsetToData >> 31)) return nullptr;
    ResDir* nd = (ResDir*)(root + (te->OffsetToData & 0x7FFFFFFF));
    ResEntry* ne = res_find(root, nd, name);
    if (!ne || !(ne->OffsetToData >> 31)) return nullptr;
    ResDir* ld = (ResDir*)(root + (ne->OffsetToData & 0x7FFFFFFF));
    ResEntry* le = nullptr;
    if (lang) le = res_find(root, ld, (const WCHAR*)(u64)lang);
    if (!le) le = res_find(root, ld, nullptr);                  // any language
    if (!le || (le->OffsetToData >> 31)) return nullptr;
    return (ResData*)(root + le->OffsetToData);
}

void* res_lookup(HMODULE h, const WCHAR* type, const WCHAR* name, u32* size) {
    Module* m = mod_of_handle(h);
    if (!m || m->builtin) return nullptr;
    ResData* d = find_res(m, type, name, 0);
    if (!d) return nullptr;
    if (size) *size = d->Size;
    return m->base + d->OffsetToData;
}

static HANDLE WINAPI FindResourceExW(HMODULE h, const WCHAR* type, const WCHAR* name, u16 lang) {
    Module* m = mod_of_handle(h);
    if (!m) { set_error(ERROR_INVALID_HANDLE); return nullptr; }
    ResData* d = m->builtin ? nullptr : find_res(m, type, name, lang);
    if (!d) { set_error(ERROR_RESOURCE_NAME_NOT_FOUND); return nullptr; }
    return d;
}
static HANDLE WINAPI FindResourceW(HMODULE h, const WCHAR* name, const WCHAR* type) { return FindResourceExW(h, type, name, 0); }
static WCHAR* res_wname(const char* s, WCHAR* buf) {
    if ((u64)s < 0x10000) return (WCHAR*)s;
    wide_from_utf8(s, -1, buf, 128);
    return buf;
}
static HANDLE WINAPI FindResourceA(HMODULE h, const char* name, const char* type) {
    WCHAR a[128], b[128];
    return FindResourceExW(h, res_wname(type, a), res_wname(name, b), 0);
}
static HANDLE WINAPI FindResourceExA(HMODULE h, const char* type, const char* name, u16 lang) {
    WCHAR a[128], b[128];
    return FindResourceExW(h, res_wname(type, a), res_wname(name, b), lang);
}
// enumerating resources: the callbacks get names as integer ids or as strings (A or W)
typedef BOOL (WINAPI *EnumTypeW)(HMODULE, WCHAR*, i64);
typedef BOOL (WINAPI *EnumNameW)(HMODULE, const WCHAR*, WCHAR*, i64);
typedef BOOL (WINAPI *EnumLangW)(HMODULE, const WCHAR*, const WCHAR*, u16, i64);
static ResDir* res_subdir(u8* root, ResDir* dir, const WCHAR* name) {
    ResEntry* e = res_find(root, dir, name);
    return e && (e->OffsetToData >> 31) ? (ResDir*)(root + (e->OffsetToData & 0x7FFFFFFF)) : nullptr;
}
// an entry's name as the callbacks get it: an id, or a string in buf (UTF-16, or UTF-8 when ansi)
static void* res_entry_name(u8* root, ResEntry* e, void* buf, bool ansi) {
    if (!(e->Name >> 31)) return (void*)(u64)(e->Name & 0xFFFF);
    u16* s = (u16*)(root + (e->Name & 0x7FFFFFFF));
    u32 len = s[0] < 255 ? s[0] : 255;
    WCHAR w[256];
    memcpy(w, s + 1, len * 2);
    w[len] = 0;
    if (ansi) utf8_from_wide(w, -1, (char*)buf, 512);
    else memcpy(buf, w, (len + 1) * 2);
    return buf;
}
static BOOL res_enum(HMODULE h, const WCHAR* type, const WCHAR* name, int level, void* fn, i64 param, bool ansi, const void* atype, const void* aname) {
    Module* m = mod_of_handle(h);
    if (!m) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    u8* root = m->builtin ? nullptr : (u8*)image_dir(m, DIR_RESOURCE, nullptr);
    if (!root) { set_error(ERROR_RESOURCE_DATA_NOT_FOUND); return FALSE; }
    ResDir* dir = (ResDir*)root;
    if (level >= 1 && !(dir = res_subdir(root, dir, type))) { set_error(ERROR_RESOURCE_TYPE_NOT_FOUND); return FALSE; }
    if (level >= 2 && !(dir = res_subdir(root, dir, name))) { set_error(ERROR_RESOURCE_NAME_NOT_FOUND); return FALSE; }
    ResEntry* e = (ResEntry*)(dir + 1);
    u32 n = (u32)dir->NumberOfNamedEntries + dir->NumberOfIdEntries;
    for (u32 i = 0; i < n; i++) {
        char buf[512];
        BOOL more;
        if (level == 0) more = ((EnumTypeW)fn)(h, (WCHAR*)res_entry_name(root, &e[i], buf, ansi), param);
        else if (level == 1) more = ((EnumNameW)fn)(h, (const WCHAR*)atype, (WCHAR*)res_entry_name(root, &e[i], buf, ansi), param);
        else more = ((EnumLangW)fn)(h, (const WCHAR*)atype, (const WCHAR*)aname, (u16)e[i].Name, param);
        if (!more) break;
    }
    return TRUE;
}
static BOOL WINAPI EnumResourceTypesW(HMODULE h, void* fn, i64 p) { return res_enum(h, nullptr, nullptr, 0, fn, p, false, nullptr, nullptr); }
static BOOL WINAPI EnumResourceTypesA(HMODULE h, void* fn, i64 p) { return res_enum(h, nullptr, nullptr, 0, fn, p, true, nullptr, nullptr); }
static BOOL WINAPI EnumResourceNamesW(HMODULE h, const WCHAR* type, void* fn, i64 p) { return res_enum(h, type, nullptr, 1, fn, p, false, type, nullptr); }
static BOOL WINAPI EnumResourceNamesA(HMODULE h, const char* type, void* fn, i64 p) {
    WCHAR a[128];
    return res_enum(h, res_wname(type, a), nullptr, 1, fn, p, true, type, nullptr);
}
static BOOL WINAPI EnumResourceLanguagesW(HMODULE h, const WCHAR* type, const WCHAR* name, void* fn, i64 p) {
    return res_enum(h, type, name, 2, fn, p, false, type, name);
}
static BOOL WINAPI EnumResourceLanguagesA(HMODULE h, const char* type, const char* name, void* fn, i64 p) {
    WCHAR a[128], b[128];
    return res_enum(h, res_wname(type, a), res_wname(name, b), 2, fn, p, true, type, name);
}
static BOOL WINAPI EnumResourceLanguagesExW(HMODULE h, const WCHAR* type, const WCHAR* name, void* fn, i64 p, u32 f, u16 lang) {
    (void)f; (void)lang;
    return EnumResourceLanguagesW(h, type, name, fn, p);
}
static HANDLE WINAPI LoadResource(HMODULE h, HANDLE r) {
    Module* m = mod_of_handle(h);
    if (!m || !r) return nullptr;
    return m->base + ((ResData*)r)->OffsetToData;
}
static void* WINAPI LockResource(HANDLE h) { return h; }
static BOOL WINAPI FreeResource(HANDLE h) { (void)h; return FALSE; }
static u32 WINAPI SizeofResource(HMODULE h, HANDLE r) { (void)h; return r ? ((ResData*)r)->Size : 0; }

// ---------------------------------------------------------------- the process
static void WINAPI ExitProcess(u32 code) { process_exit(code); }
static BOOL WINAPI TerminateProcess(HANDLE p, u32 code) {
    if ((u64)p == (u64)-1) { files_flush_all(); si::exit((int)code); }
    set_error(ERROR_ACCESS_DENIED);
    return FALSE;
}
static void WINAPI FatalExit(int code) { msg("FatalExit(%d)", code); quit(code); }
static void WINAPI FatalAppExitA(u32 a, const char* m) { (void)a; msg("FatalAppExit: %s", m); quit(255); }
static void WINAPI FatalAppExitW(u32 a, const WCHAR* m) { (void)a; char u[400]; utf8_from_wide(m, -1, u, sizeof(u)); msg("FatalAppExit: %s", u); quit(255); }
static HANDLE WINAPI GetCurrentProcess() { return (HANDLE)(i64)-1; }
static u32 WINAPI GetCurrentProcessId() { return 0x40; }
static u32 WINAPI GetProcessId(HANDLE h) { return (u64)h == (u64)-1 ? 0x40 : 0; }
static BOOL WINAPI GetExitCodeProcess(HANDLE h, u32* code) { (void)h; *code = 259; return TRUE; }
static HANDLE WINAPI OpenProcess(u32 a, BOOL i, u32 pid) { (void)a; (void)i; if (pid == 0x40) return (HANDLE)(i64)-1; set_error(ERROR_INVALID_PARAMETER); return nullptr; }
static char* WINAPI GetCommandLineA() { return cmdline_a; }
static WCHAR* WINAPI GetCommandLineW() { return cmdline_w; }

static BOOL WINAPI CreateProcessA(const char* app, char* cmd, void* pa, void* ta, BOOL inh, u32 fl, void* env, const char* dir, void* si_, void* pi) {
    (void)pa; (void)ta; (void)inh; (void)fl; (void)env; (void)dir; (void)si_; (void)pi;
    msg("CreateProcess(%s) is not supported: a program can't start another one yet", app ? app : cmd ? cmd : "?");
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}
static BOOL WINAPI CreateProcessW(const WCHAR* app, WCHAR* cmd, void* pa, void* ta, BOOL inh, u32 fl, void* env, const WCHAR* dir, void* si_, void* pi) {
    char u[300];
    utf8_from_wide(app ? app : cmd ? cmd : L"?", -1, u, sizeof(u));
    return CreateProcessA(u, nullptr, pa, ta, inh, fl, env, nullptr, si_, pi);
}
static u32 WINAPI WinExec(const char* cmd, u32 show) { (void)show; CreateProcessA(cmd, nullptr, nullptr, nullptr, 0, 0, nullptr, nullptr, nullptr, nullptr); return 2; }

struct STARTUPINFOW {
    u32 cb; WCHAR* lpReserved; WCHAR* lpDesktop; WCHAR* lpTitle; u32 dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    u16 wShowWindow, cbReserved2; u8* lpReserved2; HANDLE hStdInput, hStdOutput, hStdError;
};
static void WINAPI GetStartupInfoW(STARTUPINFOW* s) {
    memset(s, 0, sizeof(*s));
    s->cb = sizeof(*s);
    static WCHAR desk[] = L"WinSta0\\Default";
    s->lpDesktop = desk;
    s->lpTitle = peb->ProcessParameters->ImagePathName.Buffer;
    s->dwFlags = 0x100;                                         // STARTF_USESTDHANDLES
    s->wShowWindow = 1;
    s->hStdInput = std_handles[0]; s->hStdOutput = std_handles[1]; s->hStdError = std_handles[2];
}
static void WINAPI GetStartupInfoA(STARTUPINFOW* s) {
    GetStartupInfoW(s);
    static char desk[] = "WinSta0\\Default";
    static char title[MAX_PATH];
    utf8_from_wide(peb->ProcessParameters->ImagePathName.Buffer, -1, title, sizeof(title));
    s->lpDesktop = (WCHAR*)desk;
    s->lpTitle = (WCHAR*)title;
}

// ---------------------------------------------------------------- the environment
static u32 copy_env_a(const WCHAR* v, char* out, u32 cap) {
    int n = utf8_from_wide(v, -1, nullptr, 0);                  // with the NUL
    if (!out || cap < (u32)n) return (u32)n;
    utf8_from_wide(v, -1, out, (int)cap);
    return (u32)n - 1;
}
static u32 WINAPI GetEnvironmentVariableW(const WCHAR* name, WCHAR* out, u32 cap) {
    Guard g(env_lock);
    const WCHAR* v = env_get(name);
    if (!v) { set_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
    u32 n = (u32)wlen(v);
    if (!out || cap <= n) return n + 1;
    memcpy(out, v, (n + 1) * 2);
    set_error(0);
    return n;
}
static u32 WINAPI GetEnvironmentVariableA(const char* name, char* out, u32 cap) {
    WCHAR wn[256];
    wide_from_utf8(name, -1, wn, 256);
    Guard g(env_lock);
    const WCHAR* v = env_get(wn);
    if (!v) { set_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
    set_error(0);
    return copy_env_a(v, out, cap);
}
static BOOL WINAPI SetEnvironmentVariableW(const WCHAR* name, const WCHAR* value) {
    Guard g(env_lock);
    if (!env_set(name, value)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}
static BOOL WINAPI SetEnvironmentVariableA(const char* name, const char* value) {
    WCHAR* wn = wide_dup(name);
    WCHAR* wv = value ? wide_dup(value) : nullptr;
    BOOL r = SetEnvironmentVariableW(wn, wv);
    ifree(wn);
    if (wv) ifree(wv);
    return r;
}
static WCHAR* WINAPI GetEnvironmentStringsW() {
    Guard g(env_lock);
    usize n = 0;
    while (env_block[n]) n += wlen(env_block + n) + 1;
    WCHAR* c = (WCHAR*)h_alloc((n + 1) * 2);
    memcpy(c, env_block, (n + 1) * 2);
    return c;
}
static char* WINAPI GetEnvironmentStringsA() {
    Guard g(env_lock);
    usize n = 0;
    while (env_block[n]) n += wlen(env_block + n) + 1;
    int bytes = utf8_from_wide(env_block, (int)n + 1, nullptr, 0);
    char* c = (char*)h_alloc((usize)bytes + 1);
    utf8_from_wide(env_block, (int)n + 1, c, bytes);
    return c;
}
static BOOL WINAPI FreeEnvironmentStringsW(WCHAR* p) { h_free(p); return TRUE; }
static BOOL WINAPI FreeEnvironmentStringsA(char* p) { h_free(p); return TRUE; }
static BOOL WINAPI SetEnvironmentStringsW(WCHAR* p) { (void)p; return TRUE; }

static u32 expand_env(const WCHAR* src, WCHAR* out, u32 cap) {
    u32 o = 0;
    auto put = [&](WCHAR c) { if (o < cap && out) out[o] = c; o++; };
    for (const WCHAR* p = src; *p;) {
        if (*p == '%') {
            const WCHAR* e = p + 1;
            while (*e && *e != '%') e++;
            if (*e == '%' && e > p + 1) {
                WCHAR name[256];
                usize n = (usize)(e - p - 1);
                if (n > 255) n = 255;
                memcpy(name, p + 1, n * 2);
                name[n] = 0;
                const WCHAR* v = env_get(name);
                if (v) { for (; *v; v++) put(*v); p = e + 1; continue; }
            }
        }
        put(*p++);
    }
    put(0);
    if (out && o > cap && cap) out[cap - 1] = 0;
    return o;
}
static u32 WINAPI ExpandEnvironmentStringsW(const WCHAR* src, WCHAR* out, u32 cap) { Guard g(env_lock); return expand_env(src, out, cap); }
static u32 WINAPI ExpandEnvironmentStringsA(const char* src, char* out, u32 cap) {
    WCHAR* w = wide_dup(src);
    WCHAR buf[4096];
    u32 n;
    { Guard g(env_lock); n = expand_env(w, buf, 4096); }
    ifree(w);
    if (n > 4096) n = 4096, buf[4095] = 0;
    int bytes = utf8_from_wide(buf, -1, nullptr, 0);
    if (out && cap >= (u32)bytes) utf8_from_wide(buf, -1, out, (int)cap);
    return (u32)bytes;
}

// ---------------------------------------------------------------- thread-local and fiber-local storage
static u8 tls_used[64 + 1024];
static Lock tls_lock;
static u32 WINAPI TlsAlloc() {
    Guard g(tls_lock);
    for (u32 i = 0; i < sizeof(tls_used); i++)
        if (!tls_used[i]) {
            tls_used[i] = 1;
            for (Thr* t = threads; t; t = t->next) {            // a fresh slot reads as zero everywhere
                if (i < 64) t->teb->TlsSlots[i] = nullptr;
                else if (t->teb->TlsExpansionSlots) t->teb->TlsExpansionSlots[i - 64] = nullptr;
            }
            return i;
        }
    set_error(ERROR_NO_MORE_ITEMS);
    return 0xFFFFFFFF;
}
static BOOL WINAPI TlsFree(u32 i) {
    if (i >= sizeof(tls_used) || !tls_used[i]) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
    tls_used[i] = 0;
    return TRUE;
}
static void* WINAPI TlsGetValue(u32 i) {
    TEB* t = teb();
    set_error(0);
    if (i < 64) return t->TlsSlots[i];
    if (i < 64 + 1024) return t->TlsExpansionSlots ? t->TlsExpansionSlots[i - 64] : nullptr;
    set_error(ERROR_INVALID_PARAMETER);
    return nullptr;
}
static BOOL WINAPI TlsSetValue(u32 i, void* v) {
    TEB* t = teb();
    if (i < 64) { t->TlsSlots[i] = v; return TRUE; }
    if (i < 64 + 1024) {
        if (!t->TlsExpansionSlots) t->TlsExpansionSlots = (void**)h_alloc(1024 * sizeof(void*), true);
        t->TlsExpansionSlots[i - 64] = v;
        return TRUE;
    }
    set_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}
typedef void (WINAPI *FlsCallback)(void*);
static FlsCallback fls_cb[128];
static u8 fls_used[128];
static u32 WINAPI FlsAlloc(FlsCallback cb) {
    Guard g(tls_lock);
    for (u32 i = 1; i < 128; i++) if (!fls_used[i]) { fls_used[i] = 1; fls_cb[i] = cb; for (Thr* t = threads; t; t = t->next) t->fls[i] = nullptr; return i; }
    set_error(ERROR_NO_MORE_ITEMS);
    return 0xFFFFFFFF;
}
static BOOL WINAPI FlsFree(u32 i) { if (i >= 128 || !fls_used[i]) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } fls_used[i] = 0; return TRUE; }
static void* WINAPI FlsGetValue(u32 i) { if (i >= 128) { set_error(ERROR_INVALID_PARAMETER); return nullptr; } set_error(0); return cur_thr()->fls[i]; }
static BOOL WINAPI FlsSetValue(u32 i, void* v) { if (i >= 128) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } cur_thr()->fls[i] = v; return TRUE; }
static BOOL WINAPI IsThreadAFiber() { return FALSE; }

// ---------------------------------------------------------------- errors and debugging
static u32 WINAPI GetLastError() { return get_error(); }
static void WINAPI SetLastError(u32 e) { set_error(e); }
static void WINAPI RestoreLastError(u32 e) { set_error(e); }
static u32 error_mode = 0;
static u32 WINAPI SetErrorMode(u32 m) { u32 o = error_mode; error_mode = m; return o; }
static u32 WINAPI GetErrorMode() { return error_mode; }
static BOOL WINAPI SetThreadErrorMode(u32 m, u32* old) { if (old) *old = error_mode; error_mode = m; return TRUE; }
static u32 WINAPI GetThreadErrorMode() { return error_mode; }
static BOOL WINAPI IsDebuggerPresent() { return FALSE; }
static BOOL WINAPI CheckRemoteDebuggerPresent(HANDLE h, BOOL* p) { (void)h; *p = FALSE; return TRUE; }
static void WINAPI OutputDebugStringA(const char* s) { if (s) trace("debug: %s", s); }
static void WINAPI OutputDebugStringW(const WCHAR* s) { if (!s) return; char u[600]; utf8_from_wide(s, -1, u, sizeof(u)); trace("debug: %s", u); }
static void WINAPI DebugBreak() { __asm__ volatile("int3"); }
static BOOL WINAPI Beep(u32 f, u32 d) { (void)f; (void)d; return TRUE; }

// FormatMessage: messages for the common system errors, and %1-style inserts for FROM_STRING
static const struct { u32 code; const char* text; } sys_msgs[] = {
    {0, "The operation completed successfully."}, {1, "Incorrect function."}, {2, "The system cannot find the file specified."},
    {3, "The system cannot find the path specified."}, {4, "The system cannot open the file."}, {5, "Access is denied."},
    {6, "The handle is invalid."}, {8, "Not enough memory resources are available to process this command."},
    {13, "The data is invalid."}, {14, "Not enough memory resources are available to complete this operation."},
    {18, "There are no more files."}, {32, "The process cannot access the file because it is being used by another process."},
    {38, "Reached the end of the file."}, {50, "The request is not supported."}, {80, "The file exists."},
    {87, "The parameter is incorrect."}, {109, "The pipe has been ended."}, {112, "There is not enough space on the disk."},
    {120, "This function is not supported on this system."}, {122, "The data area passed to a system call is too small."},
    {123, "The filename, directory name, or volume label syntax is incorrect."}, {126, "The specified module could not be found."},
    {127, "The specified procedure could not be found."}, {145, "The directory is not empty."}, {183, "Cannot create a file when that file already exists."},
    {193, "%1 is not a valid Win32 application."}, {203, "The system could not find the environment option that was entered."},
    {234, "More data is available."}, {258, "The wait operation timed out."}, {259, "No more data is available."},
    {267, "The directory name is invalid."}, {998, "Invalid access to memory location."}, {1400, "Invalid window handle."},
    {1460, "This operation returned because the timeout period expired."},
};
enum { FORMAT_MESSAGE_ALLOCATE_BUFFER = 0x100, FORMAT_MESSAGE_IGNORE_INSERTS = 0x200, FORMAT_MESSAGE_FROM_STRING = 0x400,
       FORMAT_MESSAGE_FROM_HMODULE = 0x800, FORMAT_MESSAGE_FROM_SYSTEM = 0x1000, FORMAT_MESSAGE_ARGUMENT_ARRAY = 0x2000 };

static char* format_msg(u32 flags, const char* src, u32 id, va_list* args) {
    const char* text = nullptr;
    char tmp[64];
    if (flags & FORMAT_MESSAGE_FROM_STRING) text = src;
    else if (flags & FORMAT_MESSAGE_FROM_SYSTEM) {
        for (auto& m : sys_msgs) if (m.code == id) { text = m.text; break; }
        if (!text) { set_error(317); return nullptr; }          // ERROR_MR_MID_NOT_FOUND
    }
    if (!text) { snfmt(tmp, sizeof(tmp), "Message 0x%x", id); text = tmp; }
    usize cap = 256, n = 0;
    char* out = (char*)h_alloc(cap);
    auto put = [&](const char* s, usize k) {
        if (n + k + 3 > cap) { while (n + k + 3 > cap) cap *= 2; out = (char*)h_realloc(out, cap); }
        memcpy(out + n, s, k);
        n += k;
    };
    for (const char* p = text; *p; p++) {
        if (*p == '%' && !(flags & FORMAT_MESSAGE_IGNORE_INSERTS) && p[1] >= '1' && p[1] <= '9') {
            int idx = 0;
            p++;
            while (*p >= '0' && *p <= '9') idx = idx * 10 + (*p++ - '0');
            char spec[16] = "s";
            if (*p == '!') { const char* e = schr(p + 1, '!'); if (e) { usize l = (usize)(e - p - 1); if (l > 14) l = 14; memcpy(spec, p + 1, l); spec[l] = 0; p = e; } else p--; }
            else p--;
            u64 v = 0;
            if (args) {
                if (flags & FORMAT_MESSAGE_ARGUMENT_ARRAY) v = ((u64*)*args)[idx - 1];
                else { va_list c; va_copy(c, *args); for (int i = 0; i < idx; i++) v = va_arg(c, u64); va_end(c); }
            }
            char f[20] = "%";
            slcat(f, spec, sizeof(f));
            char b[512];
            int k = snfmt(b, sizeof(b), f, v);
            put(b, (usize)(k < (int)sizeof(b) ? k : (int)sizeof(b) - 1));
        } else if (*p == '%' && !(flags & FORMAT_MESSAGE_IGNORE_INSERTS) && p[1]) {
            p++;
            if (*p == 'n') put("\r\n", 2);
            else if (*p == '0') break;
            else put(p, 1);
        } else put(p, 1);
    }
    if ((flags & FORMAT_MESSAGE_FROM_SYSTEM) && !(flags & FORMAT_MESSAGE_FROM_STRING)) put("\r\n", 2);
    out[n] = 0;
    return out;
}
static u32 WINAPI FormatMessageA(u32 flags, const void* src, u32 id, u32 lang, char* buf, u32 cap, va_list* args) {
    (void)lang;
    char* s = format_msg(flags, (const char*)src, id, args);
    if (!s) return 0;
    u32 n = (u32)slen(s);
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) { *(char**)buf = s; return n; }
    if (n >= cap) { h_free(s); set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, s, n + 1);
    h_free(s);
    return n;
}
static u32 WINAPI FormatMessageW(u32 flags, const void* src, u32 id, u32 lang, WCHAR* buf, u32 cap, va_list* args) {
    (void)lang;
    char* srcu = (flags & FORMAT_MESSAGE_FROM_STRING) && src ? utf8_dup((const WCHAR*)src) : nullptr;
    char* s = format_msg(flags, srcu, id, args);
    if (srcu) ifree(srcu);
    if (!s) return 0;
    int n = wide_from_utf8(s, -1, nullptr, 0);
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        WCHAR* w = (WCHAR*)h_alloc((usize)n * 2);
        wide_from_utf8(s, -1, w, n);
        h_free(s);
        *(WCHAR**)buf = w;
        return (u32)n - 1;
    }
    if ((u32)n > cap) { h_free(s); set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    wide_from_utf8(s, -1, buf, (int)cap);
    h_free(s);
    return (u32)n - 1;
}
static HANDLE WINAPI LocalAlloc(u32 flags, SIZE_T n) { return GlobalAlloc(flags, n); }
static HANDLE WINAPI LocalReAlloc(HANDLE h, SIZE_T n, u32 flags) { return GlobalReAlloc(h, n, flags); }
static HANDLE WINAPI LocalFree(HANDLE h) { h_free(h); return nullptr; }
static void* WINAPI LocalLock(HANDLE h) { return h; }
static BOOL WINAPI LocalUnlock(HANDLE h) { (void)h; return FALSE; }
static SIZE_T WINAPI LocalSize(HANDLE h) { return GlobalSize(h); }
static HANDLE WINAPI LocalHandle(const void* p) { return (HANDLE)p; }

// ---------------------------------------------------------------- system information
struct SYSTEM_INFO { u16 wProcessorArchitecture, wReserved; u32 dwPageSize; void* lpMinimumApplicationAddress; void* lpMaximumApplicationAddress;
                     u64 dwActiveProcessorMask; u32 dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity; u16 wProcessorLevel, wProcessorRevision; };
static void WINAPI GetSystemInfo(SYSTEM_INFO* s) {
    memset(s, 0, sizeof(*s));
    s->wProcessorArchitecture = 9;                              // AMD64
    s->dwPageSize = 4096;
    s->lpMinimumApplicationAddress = (void*)0x100000000ull;
    s->lpMaximumApplicationAddress = (void*)0x7FFFFFFFFFull;
    s->dwActiveProcessorMask = 1;
    s->dwNumberOfProcessors = 1;
    s->dwProcessorType = 8664;
    s->dwAllocationGranularity = 0x10000;
    s->wProcessorLevel = 6;
    s->wProcessorRevision = 0x5507;
}
static void WINAPI GetNativeSystemInfo(SYSTEM_INFO* s) { GetSystemInfo(s); }
static BOOL WINAPI IsWow64Process(HANDLE h, BOOL* w) { (void)h; *w = FALSE; return TRUE; }
static BOOL WINAPI IsWow64Process2(HANDLE h, u16* p, u16* n) { (void)h; *p = 0; if (n) *n = 0x8664; return TRUE; }
static BOOL WINAPI IsProcessorFeaturePresent(u32 f) {
    switch (f) {
    case 2: case 3: case 6: case 7: case 8: case 10: case 12: case 13: case 17: case 20: case 23: return TRUE;   // cmpxchg, mmx, sse, sse2, sse3, nx, rdtsc, fastfail...
    }
    return FALSE;
}
static u32 WINAPI GetVersion() { return (19045u << 16) | 10; }
struct OSVERSIONINFOEXW { u32 dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId; WCHAR szCSDVersion[128]; u16 wServicePackMajor, wServicePackMinor, wSuiteMask; u8 wProductType, wReserved; };
struct OSVERSIONINFOEXA { u32 dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId; char szCSDVersion[128]; u16 wServicePackMajor, wServicePackMinor, wSuiteMask; u8 wProductType, wReserved; };
static BOOL WINAPI GetVersionExW(OSVERSIONINFOEXW* v) {
    u32 sz = v->dwOSVersionInfoSize;
    if (sz != 276 && sz != sizeof(OSVERSIONINFOEXW)) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memset((u8*)v + 4, 0, sz - 4);
    v->dwMajorVersion = 10; v->dwMinorVersion = 0; v->dwBuildNumber = 19045; v->dwPlatformId = 2;
    if (sz == sizeof(OSVERSIONINFOEXW)) v->wProductType = 1;
    return TRUE;
}
static BOOL WINAPI GetVersionExA(OSVERSIONINFOEXA* v) {
    u32 sz = v->dwOSVersionInfoSize;
    if (sz != 148 && sz != sizeof(OSVERSIONINFOEXA)) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memset((u8*)v + 4, 0, sz - 4);
    v->dwMajorVersion = 10; v->dwMinorVersion = 0; v->dwBuildNumber = 19045; v->dwPlatformId = 2;
    if (sz == sizeof(OSVERSIONINFOEXA)) v->wProductType = 1;
    return TRUE;
}
static BOOL WINAPI VerifyVersionInfoW(OSVERSIONINFOEXW* v, u32 mask, u64 cond) { (void)v; (void)mask; (void)cond; return TRUE; }
static u64 WINAPI VerSetConditionMask(u64 m, u32 t, u8 c) { (void)t; (void)c; return m; }
static u32 put_sys(const char* s, char* out, u32 cap) { u32 n = (u32)slen(s); if (!out || cap <= n) return n + 1; memcpy(out, s, n + 1); return n; }
static u32 put_sysw(const char* s, WCHAR* out, u32 cap) { u32 n = (u32)slen(s); if (!out || cap <= n) return n + 1; wide_from_utf8(s, -1, out, (int)cap); return n; }
static u32 WINAPI GetSystemDirectoryA(char* o, u32 c) { return put_sys("C:\\Windows\\System32", o, c); }
static u32 WINAPI GetSystemDirectoryW(WCHAR* o, u32 c) { return put_sysw("C:\\Windows\\System32", o, c); }
static u32 WINAPI GetWindowsDirectoryA(char* o, u32 c) { return put_sys("C:\\Windows", o, c); }
static u32 WINAPI GetWindowsDirectoryW(WCHAR* o, u32 c) { return put_sysw("C:\\Windows", o, c); }
static u32 WINAPI GetSystemWindowsDirectoryW(WCHAR* o, u32 c) { return put_sysw("C:\\Windows", o, c); }
static u32 WINAPI GetSystemWow64DirectoryW(WCHAR* o, u32 c) { (void)o; (void)c; set_error(ERROR_CALL_NOT_IMPLEMENTED); return 0; }
static BOOL WINAPI GetComputerNameA(char* o, u32* n) { if (*n < 8) { *n = 8; set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; } memcpy(o, "SILICON", 8); *n = 7; return TRUE; }
static BOOL WINAPI GetComputerNameW(WCHAR* o, u32* n) { if (*n < 8) { *n = 8; set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; } wide_from_utf8("SILICON", -1, o, 8); *n = 7; return TRUE; }
static BOOL WINAPI GetComputerNameExW(int t, WCHAR* o, u32* n) { (void)t; return GetComputerNameW(o, n); }
static BOOL WINAPI GetUserNameA(char* o, u32* n) { if (*n < 5) { *n = 5; set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; } memcpy(o, "user", 5); *n = 5; return TRUE; }
static BOOL WINAPI GetUserNameW(WCHAR* o, u32* n) { if (*n < 5) { *n = 5; set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; } wide_from_utf8("user", -1, o, 5); *n = 5; return TRUE; }
static BOOL WINAPI GetProcessTimes(HANDLE h, FILETIME* c, FILETIME* e, FILETIME* k, FILETIME* u) {
    (void)h;
    memset(c, 0, 8); memset(e, 0, 8); memset(k, 0, 8);
    u64 t = now_ms() * 10000;
    u->dwLowDateTime = (u32)t; u->dwHighDateTime = (u32)(t >> 32);
    return TRUE;
}
static BOOL WINAPI GetProcessAffinityMask(HANDLE h, u64* p, u64* s) { (void)h; *p = 1; *s = 1; return TRUE; }
static BOOL WINAPI SetProcessAffinityMask(HANDLE h, u64 m) { (void)h; (void)m; return TRUE; }
static u32 WINAPI GetPriorityClass(HANDLE h) { (void)h; return 0x20; }
static BOOL WINAPI SetPriorityClass(HANDLE h, u32 c) { (void)h; (void)c; return TRUE; }
static BOOL WINAPI SetProcessPriorityBoost(HANDLE h, BOOL d) { (void)h; (void)d; return TRUE; }
static u32 WINAPI GetCurrentProcessorNumber() { return 0; }
static u32 WINAPI GetActiveProcessorCount(u16 g) { (void)g; return 1; }
static u32 WINAPI GetMaximumProcessorCount(u16 g) { (void)g; return 1; }
static u16 WINAPI GetActiveProcessorGroupCount() { return 1; }
static BOOL WINAPI GetLogicalProcessorInformation(void* b, u32* n) {
    // one core, one logical processor
    struct { u64 mask; u32 rel; u32 pad; u8 flags; u8 rest[15]; } info = {1, 0, 0, 0, {0}};
    if (*n < sizeof(info)) { *n = sizeof(info); set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(b, &info, sizeof(info));
    *n = sizeof(info);
    return TRUE;
}
static BOOL WINAPI GetLogicalProcessorInformationEx(int rel, void* b, u32* n) { (void)rel; (void)b; *n = 0; set_error(ERROR_NOT_SUPPORTED); return FALSE; }
static BOOL WINAPI GetProcessHandleCount(HANDLE h, u32* n) { (void)h; *n = 16; return TRUE; }
static BOOL WINAPI GetProcessWorkingSetSize(HANDLE h, SIZE_T* mn, SIZE_T* mx) { (void)h; *mn = 1 << 20; *mx = 64 << 20; return TRUE; }
static BOOL WINAPI SetProcessWorkingSetSize(HANDLE h, SIZE_T mn, SIZE_T mx) { (void)h; (void)mn; (void)mx; return TRUE; }
static BOOL WINAPI SetProcessDEPPolicy(u32 f) { (void)f; return TRUE; }
static BOOL WINAPI GetProcessMitigationPolicy(HANDLE h, int p, void* b, SIZE_T n) { (void)h; (void)p; memset(b, 0, n); return TRUE; }
static BOOL WINAPI SetProcessMitigationPolicy(int p, void* b, SIZE_T n) { (void)p; (void)b; (void)n; return TRUE; }
static BOOL WINAPI ProcessIdToSessionId(u32 pid, u32* s) { (void)pid; *s = 1; return TRUE; }
static u32 WINAPI WTSGetActiveConsoleSessionId() { return 1; }

// pointers "encoded" with a per-process secret
static const u64 PTR_COOKIE = 0x5ead1c0de5117c0dull;
static void* WINAPI EncodePointer(void* p) { return (void*)((u64)p ^ PTR_COOKIE); }
static void* WINAPI DecodePointer(void* p) { return (void*)((u64)p ^ PTR_COOKIE); }
static void* WINAPI EncodeSystemPointer(void* p) { return EncodePointer(p); }
static void* WINAPI DecodeSystemPointer(void* p) { return DecodePointer(p); }

static BOOL WINAPI GetNumaHighestNodeNumber(u32* n) { *n = 0; return TRUE; }
static BOOL WINAPI GetNumaProcessorNode(u8 p, u8* n) { (void)p; *n = 0; return TRUE; }
static BOOL WINAPI QueryThreadCycleTime(HANDLE h, u64* c) { (void)h; *c = si::ticks_us() * 3000; return TRUE; }
static BOOL WINAPI QueryProcessCycleTime(HANDLE h, u64* c) { (void)h; *c = si::ticks_us() * 3000; return TRUE; }
static BOOL WINAPI GetSystemTimes(FILETIME* i, FILETIME* k, FILETIME* u) { memset(i, 0, 8); memset(k, 0, 8); memset(u, 0, 8); return TRUE; }
static BOOL WINAPI GetSystemTimeAdjustment(u32* adj, u32* inc, BOOL* dis) { *adj = 156250; *inc = 156250; *dis = TRUE; return TRUE; }
static u32 WINAPI GetSystemFirmwareTable(u32 p, u32 id, void* b, u32 n) { (void)p; (void)id; (void)b; (void)n; return 0; }
static BOOL WINAPI GetSystemPowerStatus(u8* s) { memset(s, 0, 12); s[0] = 1; s[1] = 128; s[2] = 255; return TRUE; }
static u32 WINAPI SetThreadExecutionState(u32 f) { return f; }

static const Export proc_exports[] = {
    E(VirtualAlloc), E(VirtualAllocEx), E(VirtualAlloc2), E(VirtualFree), E(VirtualFreeEx), E(VirtualProtect), E(VirtualProtectEx),
    E(VirtualQuery), E(VirtualQueryEx), E(VirtualLock), E(VirtualUnlock), E(FlushInstructionCache), E(FlushProcessWriteBuffers),
    E(IsBadReadPtr), E(IsBadWritePtr), E(IsBadCodePtr), E(IsBadStringPtrA), E(ReadProcessMemory), E(WriteProcessMemory),
    E(GetProcessHeap), E(HeapCreate), E(HeapDestroy), E(HeapAlloc), E(HeapReAlloc), E(HeapFree), E(HeapSize), E(HeapValidate),
    E(HeapCompact), E(HeapLock), E(HeapUnlock), E(HeapWalk), E(HeapSetInformation), E(HeapQueryInformation), E(GetProcessHeaps),
    E(GlobalAlloc), E(GlobalReAlloc), E(GlobalFree), E(GlobalLock), E(GlobalUnlock), E(GlobalSize), E(GlobalHandle), E(GlobalFlags),
    E(GlobalMemoryStatusEx), E(GlobalMemoryStatus), E(GetPhysicallyInstalledSystemMemory),
    E(LocalAlloc), E(LocalReAlloc), E(LocalFree), E(LocalLock), E(LocalUnlock), E(LocalSize), E(LocalHandle),
    E(GetModuleHandleA), E(GetModuleHandleW), E(GetModuleHandleExA), E(GetModuleHandleExW), E(GetModuleFileNameA), E(GetModuleFileNameW),
    E(LoadLibraryA), E(LoadLibraryW), E(LoadLibraryExA), E(LoadLibraryExW), E(FreeLibrary), E(FreeLibraryAndExitThread),
    E(DisableThreadLibraryCalls), E(SetDllDirectoryA), E(SetDllDirectoryW), E(AddDllDirectory), E(RemoveDllDirectory),
    E(SetDefaultDllDirectories), E(GetProcAddress), E(FindResourceA), E(FindResourceW), E(FindResourceExA), E(FindResourceExW),
    E(EnumResourceTypesW), E(EnumResourceTypesA), E(EnumResourceNamesW), E(EnumResourceNamesA), E(EnumResourceLanguagesW),
    E(EnumResourceLanguagesA), E(EnumResourceLanguagesExW),
    E(LoadResource), E(LockResource), E(FreeResource), E(SizeofResource),
    E(ExitProcess), E(TerminateProcess), E(FatalExit), E(FatalAppExitA), E(FatalAppExitW), E(GetCurrentProcess), E(GetCurrentProcessId),
    E(GetProcessId), E(GetExitCodeProcess), E(OpenProcess), E(GetCommandLineA), E(GetCommandLineW), E(CreateProcessA), E(CreateProcessW),
    E(WinExec), E(GetStartupInfoA), E(GetStartupInfoW),
    E(GetEnvironmentVariableA), E(GetEnvironmentVariableW), E(SetEnvironmentVariableA), E(SetEnvironmentVariableW),
    E(GetEnvironmentStringsA), E(GetEnvironmentStringsW), EA("GetEnvironmentStrings", GetEnvironmentStringsA), E(FreeEnvironmentStringsA),
    E(FreeEnvironmentStringsW), E(SetEnvironmentStringsW), E(ExpandEnvironmentStringsA), E(ExpandEnvironmentStringsW),
    E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue), E(FlsAlloc), E(FlsFree), E(FlsGetValue), E(FlsSetValue), E(IsThreadAFiber),
    E(GetLastError), E(SetLastError), E(RestoreLastError), E(SetErrorMode), E(GetErrorMode), E(SetThreadErrorMode), E(GetThreadErrorMode),
    E(IsDebuggerPresent), E(CheckRemoteDebuggerPresent), E(OutputDebugStringA), E(OutputDebugStringW), E(DebugBreak), E(Beep),
    E(FormatMessageA), E(FormatMessageW),
    E(GetSystemInfo), E(GetNativeSystemInfo), E(IsWow64Process), E(IsWow64Process2), E(IsProcessorFeaturePresent), E(GetVersion),
    E(GetVersionExA), E(GetVersionExW), E(VerifyVersionInfoW), E(VerSetConditionMask), E(GetSystemDirectoryA), E(GetSystemDirectoryW),
    E(GetWindowsDirectoryA), E(GetWindowsDirectoryW), E(GetSystemWindowsDirectoryW), E(GetSystemWow64DirectoryW), E(GetComputerNameA),
    E(GetComputerNameW), E(GetComputerNameExW), E(GetProcessTimes), E(GetProcessAffinityMask), E(SetProcessAffinityMask),
    E(GetPriorityClass), E(SetPriorityClass), E(SetProcessPriorityBoost), E(GetCurrentProcessorNumber), E(GetActiveProcessorCount),
    E(GetMaximumProcessorCount), E(GetActiveProcessorGroupCount), E(GetLogicalProcessorInformation), E(GetLogicalProcessorInformationEx),
    E(GetProcessHandleCount), E(GetProcessWorkingSetSize), E(SetProcessWorkingSetSize), E(SetProcessDEPPolicy), E(GetProcessMitigationPolicy),
    E(SetProcessMitigationPolicy), E(ProcessIdToSessionId), E(WTSGetActiveConsoleSessionId), E(EncodePointer), E(DecodePointer),
    E(EncodeSystemPointer), E(DecodeSystemPointer), E(GetNumaHighestNodeNumber), E(GetNumaProcessorNode), E(QueryThreadCycleTime),
    E(QueryProcessCycleTime), E(GetSystemTimes), E(GetSystemTimeAdjustment), E(GetSystemFirmwareTable), E(GetSystemPowerStatus),
    E(SetThreadExecutionState),
};
EXPORT_TABLE("kernel32.dll", proc_exports);

static const Export adv_exports[] = { E(GetUserNameA), E(GetUserNameW) };
EXPORT_TABLE("advapi32.dll", adv_exports);

}  // namespace rt
