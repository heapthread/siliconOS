// win32run - the PE32+ loader: maps .exe and .dll files, applies base relocations, binds imports to the
// runtime's own functions (or to DLLs shipped next to the program), sets up .tls and runs DllMain.
//
// The Windows system DLLs are not files here. A "built-in" module is a small fake image (headers only,
// so GetModuleHandle returns something that looks like a module) whose exports are the functions
// registered with EXPORT_TABLE in the other files of the runtime.
#include "rt.h"

namespace rt {

Module* modules = nullptr;
Module* exe_module = nullptr;
static ExportTable* tables = nullptr;
static Lock loader_lock;
static int tls_slots_used = 0;

void register_exports(ExportTable* t) { t->next = tables; tables = t; }

// ---------------------------------------------------------------- names
static void norm_name(const char* in, char* out, usize cap) {          // "C:\x\USER32" -> "user32.dll"
    const char* b = in;
    for (const char* p = in; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    usize i = 0;
    for (; b[i] && i + 1 < cap; i++) out[i] = (char)lower((u8)b[i]);
    out[i] = 0;
    while (i && out[i - 1] == ' ') out[--i] = 0;
    if (i && out[i - 1] == '.') out[--i] = 0;                         // "name." means "no extension"
    else if (!schr(out, '.')) slcat(out, ".dll", cap);
}

// which group of built-in tables answers for a DLL name
enum Group { G_NONE, G_KERNEL, G_NTDLL, G_CRT, G_MSVCRT, G_OTHER };
static Group group_of(const char* n, const char** table_dll) {
    *table_dll = n;
    if (!sncmp(n, "api-ms-win-crt-", 15) || !scmp(n, "ucrtbase.dll") || !scmp(n, "ucrtbased.dll") ||
        !sncmp(n, "vcruntime", 9) || (!sncmp(n, "msvcr", 5) && scmp(n, "msvcrt.dll"))) { *table_dll = "ucrtbase.dll"; return G_CRT; }
    if (!scmp(n, "msvcrt.dll")) return G_MSVCRT;
    if (!sncmp(n, "api-ms-win-core-", 16) || !sncmp(n, "api-ms-win-eventing", 19) || !sncmp(n, "api-ms-win-security", 19) ||
        !scmp(n, "kernelbase.dll") || !scmp(n, "kernel32.dll")) { *table_dll = "kernel32.dll"; return G_KERNEL; }
    if (!scmp(n, "ntdll.dll")) return G_NTDLL;
    return G_OTHER;
}

static void* lookup_table(const char* dll, const char* name) {
    for (ExportTable* t = tables; t; t = t->next) {
        if (scmp(t->dll, dll)) continue;
        for (int i = 0; i < t->count; i++) if (!scmp(t->list[i].name, name)) return t->list[i].fn;
    }
    return nullptr;
}

static bool has_tables(const char* dll) {
    for (ExportTable* t = tables; t; t = t->next) if (!scmp(t->dll, dll)) return true;
    return false;
}

// a built-in function of the module named n (already normalized)
static void* builtin_export(const char* n, const char* func) {
    const char* td;
    Group g = group_of(n, &td);
    void* f = lookup_table(td, func);
    if (f) return f;
    if (g == G_MSVCRT) return lookup_table("ucrtbase.dll", func);
    if (g == G_KERNEL) return lookup_table("ntdll.dll", func);
    if (g == G_CRT) return lookup_table("msvcrt.dll", func);
    return nullptr;
}

void* find_builtin(const char* name, const char** dll) {
    for (ExportTable* t = tables; t; t = t->next)
        for (int i = 0; i < t->count; i++) if (!scmp(t->list[i].name, name)) { if (dll) *dll = t->dll; return t->list[i].fn; }
    return nullptr;
}

// system DLLs that are always built in, implemented or not (a call into a missing function is trapped)
static const char* system_dlls[] = {
    "kernel32.dll", "ntdll.dll", "kernelbase.dll", "msvcrt.dll", "ucrtbase.dll", "user32.dll", "gdi32.dll", "advapi32.dll",
    "shell32.dll", "ole32.dll", "oleaut32.dll", "comctl32.dll", "comdlg32.dll", "ws2_32.dll", "wsock32.dll", "winmm.dll",
    "shlwapi.dll", "version.dll", "imm32.dll", "uxtheme.dll", "dwmapi.dll", "bcrypt.dll", "bcryptprimitives.dll", "crypt32.dll",
    "secur32.dll", "rpcrt4.dll", "setupapi.dll", "psapi.dll", "dbghelp.dll", "iphlpapi.dll", "winspool.drv", "opengl32.dll",
    "glu32.dll", "dinput8.dll", "dsound.dll", "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll", "d3d9.dll", "d3d11.dll",
    "dxgi.dll", "msimg32.dll", "userenv.dll", "netapi32.dll", "mpr.dll", "wininet.dll", "winhttp.dll", "powrprof.dll",
    "shcore.dll", "dbgeng.dll", "ntoskrnl.exe", "mswsock.dll", "normaliz.dll", "usp10.dll", "gdiplus.dll", "avrt.dll",
};
static bool is_system(const char* n) {
    const char* td;
    Group g = group_of(n, &td);
    if (g != G_OTHER) return true;
    for (usize i = 0; i < sizeof(system_dlls) / sizeof(system_dlls[0]); i++) if (!scmp(n, system_dlls[i])) return true;
    return !sncmp(n, "api-ms-win-", 11) || !sncmp(n, "ext-ms-win-", 11) || has_tables(n);
}

// ---------------------------------------------------------------- finding modules
Module* mod_find(const char* name) {
    char n[64];
    norm_name(name, n, sizeof(n));
    const char* td;
    Group g = group_of(n, &td);
    for (Module* m = modules; m; m = m->next) {
        if (!scmp(m->name, n)) return m;
        // every name of the C runtime (or of kernel32) is the same module
        const char* mtd;
        if (g != G_OTHER && g != G_MSVCRT && m->builtin && group_of(m->name, &mtd) == g) return m;
    }
    return nullptr;
}

Module* mod_by_base(void* base) { for (Module* m = modules; m; m = m->next) if (m->base == base) return m; return nullptr; }
Module* mod_by_addr(const void* a) {
    for (Module* m = modules; m; m = m->next) if ((u8*)a >= m->base && (u8*)a < m->base + m->size) return m;
    return nullptr;
}

const char* describe_addr(const void* a, char* buf, usize cap) {
    Module* m = mod_by_addr(a);
    if (m) snfmt(buf, cap, "%s+0x%llx", m->name, (u64)((u8*)a - m->base));
    else if ((u64)a >= 0x40000000ull && (u64)a < 0x48000000ull) snfmt(buf, cap, "win32run+0x%llx", (u64)a - 0x40000000ull);
    else snfmt(buf, cap, "0x%llx", (u64)a);
    return buf;
}

void* image_dir(Module* m, int index, u32* size) {
    if (!m->nt || index >= (int)m->nt->OptionalHeader.NumberOfRvaAndSizes) return nullptr;
    IMAGE_DATA_DIRECTORY& d = m->nt->OptionalHeader.DataDirectory[index];
    if (!d.VirtualAddress || !d.Size || d.VirtualAddress >= m->size) return nullptr;
    if (size) *size = d.Size;
    return m->base + d.VirtualAddress;
}

// ---------------------------------------------------------------- built-in modules
static void set_ustr(UNICODE_STRING& u, const WCHAR* s) {
    u.Buffer = wdup(s);
    u.Length = (u16)(wlen(s) * 2);
    u.MaximumLength = (u16)(u.Length + 2);
}

static void link_module(Module* m) {
    Module** t = &modules;
    while (*t) t = &(*t)->next;
    *t = m;
    ldr_add(m);
}

static Module* make_builtin(const char* n) {
    Module* m = (Module*)izalloc(sizeof(Module));
    slcpy(m->name, n, sizeof(m->name));
    m->builtin = true;
    m->is_dll = true;
    m->inited = true;
    m->refs = 1;
    m->tls_index = -1;
    // a fake image: just headers, so code that looks at a module's headers finds a valid one
    m->size = 0x10000;
    m->base = (u8*)vm_alloc(nullptr, m->size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, MEM_IMAGE);
    if (m->base) {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)m->base;
        dos->e_magic = 0x5A4D;
        dos->e_lfanew = 0x80;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(m->base + 0x80);
        nt->Signature = 0x4550;
        nt->FileHeader.Machine = 0x8664;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->FileHeader.Characteristics = 0x2022;            // executable, large address aware, DLL
        nt->OptionalHeader.Magic = 0x20B;
        nt->OptionalHeader.ImageBase = (u64)m->base;
        nt->OptionalHeader.SectionAlignment = 0x1000;
        nt->OptionalHeader.FileAlignment = 0x200;
        nt->OptionalHeader.MajorOperatingSystemVersion = 10;
        nt->OptionalHeader.MajorSubsystemVersion = 10;
        nt->OptionalHeader.SizeOfImage = m->size;
        nt->OptionalHeader.SizeOfHeaders = 0x400;
        nt->OptionalHeader.Subsystem = 3;
        nt->OptionalHeader.NumberOfRvaAndSizes = 16;
        m->nt = nt;
        vm_protect(m->base, m->size, PAGE_READONLY, nullptr);
    }
    char wp[MAX_PATH];
    snfmt(wp, sizeof(wp), "C:\\Windows\\System32\\%s", n);
    wide_from_utf8(wp, -1, m->wpath, MAX_PATH);
    link_module(m);
    trace("built-in module %s", n);
    return m;
}

// ---------------------------------------------------------------- reading a file into memory
static u8* read_file(const char* path, u32* size) {
    int n = si::file_size(path);
    if (n < 0) return nullptr;
    u8* buf = (u8*)ialloc((usize)n + 16);
    if (!buf) return nullptr;
    int got = si::file_read(path, buf, n);
    if (got != n) { ifree(buf); return nullptr; }
    *size = (u32)n;
    return buf;
}

// a file in dir whose name matches name without regard to case
static bool find_in_dir(const char* dir, const char* name, char* out, usize cap) {
    char ent[64];
    for (int i = 0;; i++) {
        int k = si::dir_entry(dir, i, ent, sizeof(ent));
        if (k <= 0) return false;
        if (k == 1 && !sicmp(ent, name)) {
            snfmt(out, cap, "%s%s%s", dir, scmp(dir, "/") ? "/" : "", ent);
            return true;
        }
    }
}

// ---------------------------------------------------------------- mapping an image
static bool resolve_imports(Module* m, int* error);
static void setup_tls(Module* m);

static const char* load_error = nullptr;

static Module* map_image(const char* path, u8* file, u32 fsize, bool is_exe, int* error) {
    *error = ERROR_BAD_EXE_FORMAT;
    if (fsize < 0x200) { load_error = "file too small"; return nullptr; }
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)file;
    if (dos->e_magic != 0x5A4D) { load_error = "no MZ header: not a Windows program"; return nullptr; }
    if (dos->e_lfanew < 0x40 || (u32)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > fsize) { load_error = "bad PE header offset"; return nullptr; }
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(file + dos->e_lfanew);
    if (nt->Signature != 0x4550) { load_error = "no PE signature (a 16-bit DOS program?)"; return nullptr; }
    if (nt->FileHeader.Machine != 0x8664) {
        load_error = nt->FileHeader.Machine == 0x14C ? "a 32-bit (x86) program: only 64-bit programs are supported" :
                     nt->FileHeader.Machine == 0xAA64 ? "an ARM64 program: only x86_64 is supported" : "not an x86_64 program";
        return nullptr;
    }
    IMAGE_OPTIONAL_HEADER64& oh = nt->OptionalHeader;
    if (oh.Magic != 0x20B) { load_error = "not PE32+ (optional header magic is not 0x20B)"; return nullptr; }
    if (oh.Win32VersionValue || oh.SizeOfImage == 0 || oh.SizeOfImage > (1u << 30) || oh.SectionAlignment < 0x1000 && oh.SectionAlignment != oh.FileAlignment) {
        load_error = "unusual image layout"; return nullptr;
    }
    if (oh.DataDirectory[14].VirtualAddress && oh.NumberOfRvaAndSizes > 14) { load_error = "a .NET program (managed code is not supported)"; return nullptr; }
    u32 nsec = nt->FileHeader.NumberOfSections;
    IMAGE_SECTION_HEADER* sec = (IMAGE_SECTION_HEADER*)((u8*)&nt->OptionalHeader + nt->FileHeader.SizeOfOptionalHeader);
    if ((u8*)(sec + nsec) > file + fsize) { load_error = "section table past the end of the file"; return nullptr; }

    // where it goes: the address it was linked for when that is free, else anywhere (needs relocations)
    usize size = (oh.SizeOfImage + 0xFFF) & ~0xFFFull;
    bool relocatable = !(nt->FileHeader.Characteristics & 1) && oh.NumberOfRvaAndSizes > DIR_BASERELOC &&
                       oh.DataDirectory[DIR_BASERELOC].VirtualAddress;
    u8* base = nullptr;
    if (oh.ImageBase >= 0x100000000ull && oh.ImageBase + size < 0x8000000000ull && !(oh.ImageBase & 0xFFFF))
        base = (u8*)vm_alloc((void*)oh.ImageBase, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, MEM_IMAGE);
    if (!base) {
        if (!relocatable) {
            load_error = oh.ImageBase < 0x100000000ull ? "it must load at a fixed address below 4 GiB and has no relocations"
                                                       : "its fixed address is taken and it has no relocations";
            return nullptr;
        }
        base = (u8*)vm_alloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, MEM_IMAGE);
        if (!base) { *error = ERROR_NOT_ENOUGH_MEMORY; load_error = "out of memory"; return nullptr; }
    }
    trace("mapping %s at %p (linked for %p, %u KiB)", path, base, (void*)oh.ImageBase, (u32)(size >> 10));

    memcpy(base, file, oh.SizeOfHeaders < fsize ? oh.SizeOfHeaders : fsize);
    for (u32 i = 0; i < nsec; i++) {
        IMAGE_SECTION_HEADER& s = sec[i];
        if (s.VirtualAddress >= size) continue;
        u32 raw = s.SizeOfRawData;
        u32 vsz = s.VirtualSize ? s.VirtualSize : s.SizeOfRawData;
        if (raw > vsz) raw = vsz;
        if (s.PointerToRawData >= fsize) raw = 0;
        else if (s.PointerToRawData + raw > fsize) raw = fsize - s.PointerToRawData;
        if (s.VirtualAddress + raw > size) raw = (u32)size - s.VirtualAddress;
        if (raw) memcpy(base + s.VirtualAddress, file + s.PointerToRawData, raw);
        char nm[9] = {0};
        memcpy(nm, s.Name, 8);
        trace("  section %-8s rva %06x size %06x %c%c%c", nm, s.VirtualAddress, vsz,
              (s.Characteristics & 0x40000000) ? 'r' : '-', (s.Characteristics & 0x80000000) ? 'w' : '-', (s.Characteristics & 0x20000000) ? 'x' : '-');
    }

    // base relocations
    i64 delta = (i64)((u64)base - oh.ImageBase);
    if (delta && relocatable) {
        IMAGE_DATA_DIRECTORY& d = oh.DataDirectory[DIR_BASERELOC];
        u8* p = base + d.VirtualAddress, *end = p + d.Size;
        int count = 0;
        while (p + sizeof(IMAGE_BASE_RELOCATION) <= end) {
            IMAGE_BASE_RELOCATION* blk = (IMAGE_BASE_RELOCATION*)p;
            if (blk->SizeOfBlock < 8 || p + blk->SizeOfBlock > end) break;
            u16* e = (u16*)(blk + 1);
            u32 n = (blk->SizeOfBlock - 8) / 2;
            for (u32 i = 0; i < n; i++) {
                u32 type = e[i] >> 12, off = blk->VirtualAddress + (e[i] & 0xFFF);
                if (off + 8 > size) continue;
                if (type == 10) { *(u64*)(base + off) += (u64)delta; count++; }               // IMAGE_REL_BASED_DIR64
                else if (type == 3) { *(u32*)(base + off) += (u32)delta; count++; }           // HIGHLOW
                else if (type == 2) { *(u16*)(base + off) += (u16)delta; count++; }           // LOW
                else if (type == 1) { *(u16*)(base + off) += (u16)((u64)delta >> 16); count++; }   // HIGH
            }
            p += blk->SizeOfBlock;
        }
        trace("  %d relocations applied (moved by %llx)", count, (u64)delta);
        nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        nt->OptionalHeader.ImageBase = (u64)base;
    }
    nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);

    Module* m = (Module*)izalloc(sizeof(Module));
    m->base = base;
    m->size = (u32)size;
    m->nt = nt;
    m->is_dll = (nt->FileHeader.Characteristics & 0x2000) != 0;
    m->entry = nt->OptionalHeader.AddressOfEntryPoint ? base + nt->OptionalHeader.AddressOfEntryPoint : nullptr;
    m->refs = 1;
    m->tls_index = -1;
    norm_name(path, m->name, sizeof(m->name));
    slcpy(m->path, path, sizeof(m->path));
    char wp[MAX_PATH];
    path_to_windows(path, wp, sizeof(wp));
    wide_from_utf8(wp, -1, m->wpath, MAX_PATH);
    u32 psz = 0;
    m->pdata = (RUNTIME_FUNCTION*)image_dir(m, DIR_EXCEPTION, &psz);
    m->pdata_count = psz / sizeof(RUNTIME_FUNCTION);
    (void)is_exe;
    link_module(m);
    return m;
}

// page protection from the section flags, once everything has been written
static void protect_sections(Module* m) {
    IMAGE_NT_HEADERS64* nt = m->nt;
    vm_protect(m->base, nt->OptionalHeader.SizeOfHeaders ? nt->OptionalHeader.SizeOfHeaders : 0x1000, PAGE_READONLY, nullptr);
    IMAGE_SECTION_HEADER* sec = (IMAGE_SECTION_HEADER*)((u8*)&nt->OptionalHeader + nt->FileHeader.SizeOfOptionalHeader);
    for (u32 i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER& s = sec[i];
        u32 vsz = s.VirtualSize ? s.VirtualSize : s.SizeOfRawData;
        if (!vsz || s.VirtualAddress >= m->size) continue;
        u32 c = s.Characteristics;
        bool x = c & 0x20000000, w = c & 0x80000000, r = (c & 0x40000000) || x || w;
        u32 prot = x ? (w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE)
                     : (w ? PAGE_READWRITE : r ? PAGE_READONLY : PAGE_NOACCESS);
        u32 len = vsz;
        if (s.VirtualAddress + len > m->size) len = m->size - s.VirtualAddress;
        vm_protect(m->base + s.VirtualAddress, len, prot, nullptr);
    }
}

// ---------------------------------------------------------------- exports
void* mod_export(Module* m, const char* name, u32 ordinal, bool for_getproc) {
    if (m->builtin) {
        if (!name) return nullptr;
        return builtin_export(m->name, name);
    }
    u32 dsize = 0;
    IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)image_dir(m, DIR_EXPORT, &dsize);
    if (!ed) return nullptr;
    u32* funcs = (u32*)(m->base + ed->AddressOfFunctions);
    u32 idx = (u32)-1;
    if (name) {
        u32* names = (u32*)(m->base + ed->AddressOfNames);
        u16* ords = (u16*)(m->base + ed->AddressOfNameOrdinals);
        int lo = 0, hi = (int)ed->NumberOfNames - 1;                       // names are sorted
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            int c = scmp((const char*)(m->base + names[mid]), name);
            if (!c) { idx = ords[mid]; break; }
            if (c < 0) lo = mid + 1; else hi = mid - 1;
        }
        if (idx == (u32)-1)                                                // in case they aren't
            for (u32 i = 0; i < ed->NumberOfNames; i++) if (!scmp((const char*)(m->base + names[i]), name)) { idx = ords[i]; break; }
    } else idx = ordinal - ed->Base;
    if (idx >= ed->NumberOfFunctions || !funcs[idx]) return nullptr;
    u8* addr = m->base + funcs[idx];
    u8* dir = (u8*)ed;
    if (addr >= dir && addr < dir + dsize) {                               // forwarded: "OTHERDLL.Function" or "OTHERDLL.#12"
        char fw[256];
        slcpy(fw, (const char*)addr, sizeof(fw));
        char* dot = (char*)srchr(fw, '.');
        if (!dot) return nullptr;
        *dot = 0;
        int err;
        Module* t = mod_load(fw, &err);
        if (!t) return nullptr;
        if (dot[1] == '#') { u32 o = 0; for (const char* p = dot + 2; *p >= '0' && *p <= '9'; p++) o = o * 10 + (u32)(*p - '0'); return mod_export(t, nullptr, o, for_getproc); }
        return mod_export(t, dot + 1, 0, for_getproc);
    }
    return addr;
}

// ---------------------------------------------------------------- imports
static Module* load_dll(const char* name, int* error, const char* for_whom);

static void* resolve_one(Module* dep, const char* dll, const char* func, u32 ordinal) {
    void* f = mod_export(dep, func, ordinal, false);
    if (f) return f;
    char nm[80];
    if (!func) { snfmt(nm, sizeof(nm), "#%u", ordinal); func = nm; }
    trace("  missing: %s!%s", dll, func);
    return make_trap(dll, func);
}

static bool bind_thunks(Module* m, Module* dep, const char* dll, u64* names, u64* iat) {
    int n = 0;
    for (; *names; names++, iat++, n++) {
        u64 t = *names;
        if (t >> 63) *iat = (u64)resolve_one(dep, dll, nullptr, (u32)(t & 0xFFFF));
        else {
            if ((t & 0x7FFFFFFF) + 2 >= m->size) return false;
            const char* fn = (const char*)(m->base + (t & 0x7FFFFFFF) + 2);    // past the hint
            *iat = (u64)resolve_one(dep, dll, fn, 0);
        }
    }
    trace("  %d imports from %s", n, dll);
    return true;
}

static bool resolve_imports(Module* m, int* error) {
    IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)image_dir(m, DIR_IMPORT, nullptr);
    for (; d && d->Name; d++) {
        const char* dll = (const char*)(m->base + d->Name);
        Module* dep = load_dll(dll, error, m->name);
        if (!dep) return false;
        u64* iat = (u64*)(m->base + d->FirstThunk);
        u64* names = (u64*)(m->base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        char n[64];
        norm_name(dll, n, sizeof(n));
        if (!bind_thunks(m, dep, n, names, iat)) { load_error = "damaged import table"; *error = ERROR_BAD_EXE_FORMAT; return false; }
    }
    // delay-loaded imports are simply bound now
    struct DelayDesc { u32 Attributes, DllNameRVA, ModuleHandleRVA, ImportAddressTableRVA, ImportNameTableRVA, BoundIAT, UnloadIAT, TimeDateStamp; };
    DelayDesc* dd = (DelayDesc*)image_dir(m, DIR_DELAY_IMPORT, nullptr);
    for (; dd && dd->DllNameRVA; dd++) {
        const char* dll = (const char*)(m->base + dd->DllNameRVA);
        Module* dep = load_dll(dll, error, m->name);
        if (!dep) return false;
        if (dd->ModuleHandleRVA) *(void**)(m->base + dd->ModuleHandleRVA) = dep->base;
        char n[64];
        norm_name(dll, n, sizeof(n));
        if (!bind_thunks(m, dep, n, (u64*)(m->base + dd->ImportNameTableRVA), (u64*)(m->base + dd->ImportAddressTableRVA))) {
            load_error = "damaged delay import table"; *error = ERROR_BAD_EXE_FORMAT; return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- .tls
struct TlsMod { Module* m; IMAGE_TLS_DIRECTORY64* dir; };
static TlsMod tls_mods[64];

static void setup_tls(Module* m) {
    IMAGE_TLS_DIRECTORY64* t = (IMAGE_TLS_DIRECTORY64*)image_dir(m, DIR_TLS, nullptr);
    if (!t || tls_slots_used >= 64) return;
    m->tls_index = tls_slots_used++;
    tls_mods[m->tls_index] = {m, t};
    if (t->AddressOfIndex) *(u32*)t->AddressOfIndex = (u32)m->tls_index;
    trace("  .tls slot %d (%llu bytes of data)", m->tls_index, t->EndAddressOfRawData - t->StartAddressOfRawData + t->SizeOfZeroFill);
}

void tls_setup_thread(Thr* th) {
    if (!tls_slots_used) return;
    if (th->tls_cap < tls_slots_used) {
        int cap = 64;
        void** blocks = (void**)h_alloc(sizeof(void*) * (usize)cap, true);
        if (!blocks) return;
        if (th->tls_blocks) { memcpy(blocks, th->tls_blocks, sizeof(void*) * (usize)th->tls_cap); h_free(th->tls_blocks); }
        th->tls_blocks = blocks;
        th->tls_cap = cap;
        th->teb->ThreadLocalStoragePointer = blocks;
    }
    for (int i = 0; i < tls_slots_used; i++) {
        if (th->tls_blocks[i]) continue;
        IMAGE_TLS_DIRECTORY64* t = tls_mods[i].dir;
        usize data = (usize)(t->EndAddressOfRawData - t->StartAddressOfRawData);
        u8* b = (u8*)h_alloc(data + t->SizeOfZeroFill + 16, true);
        if (!b) continue;
        if (data) memcpy(b, (void*)t->StartAddressOfRawData, data);
        th->tls_blocks[i] = b;
    }
}

typedef void (WINAPI *TlsCallback)(void* base, u32 reason, void* reserved);
void mod_run_tls_callbacks(u32 reason) {
    for (int i = 0; i < tls_slots_used; i++) {
        IMAGE_TLS_DIRECTORY64* t = tls_mods[i].dir;
        if (!t->AddressOfCallBacks) continue;
        for (TlsCallback* cb = (TlsCallback*)t->AddressOfCallBacks; *cb; cb++) (*cb)(tls_mods[i].m->base, reason, nullptr);
    }
}

void mod_exe_tls_callbacks(u32 reason) {
    if (!exe_module || exe_module->tls_index < 0) return;
    IMAGE_TLS_DIRECTORY64* t = tls_mods[exe_module->tls_index].dir;
    if (t->AddressOfCallBacks)
        for (TlsCallback* cb = (TlsCallback*)t->AddressOfCallBacks; *cb; cb++) (*cb)(exe_module->base, reason, nullptr);
}

// ---------------------------------------------------------------- DllMain
typedef BOOL (WINAPI *DllMainFn)(void* base, u32 reason, void* reserved);

static bool run_dllmain(Module* m) {
    if (m->inited || !m->is_dll) return true;
    m->inited = true;
    if (m->tls_index >= 0) {
        IMAGE_TLS_DIRECTORY64* t = tls_mods[m->tls_index].dir;
        if (t->AddressOfCallBacks)
            for (TlsCallback* cb = (TlsCallback*)t->AddressOfCallBacks; *cb; cb++) (*cb)(m->base, DLL_PROCESS_ATTACH, nullptr);
    }
    if (!m->entry) return true;
    trace("DllMain(%s, PROCESS_ATTACH)", m->name);
    BOOL ok = ((DllMainFn)m->entry)(m->base, DLL_PROCESS_ATTACH, (void*)1);
    if (!ok) msg("%s: DllMain failed", m->name);
    return ok != 0;
}

void mod_init_all() {
    // dependencies were linked before the modules that need them, so load order is a good init order
    for (Module* m = modules; m; m = m->next) if (!m->builtin && m != exe_module) run_dllmain(m);
}

void mod_notify_thread(u32 reason) {
    for (Module* m = modules; m; m = m->next) {
        if (m->builtin || !m->is_dll || !m->inited) continue;
        if (m->tls_index >= 0) {
            IMAGE_TLS_DIRECTORY64* t = tls_mods[m->tls_index].dir;
            if (t->AddressOfCallBacks)
                for (TlsCallback* cb = (TlsCallback*)t->AddressOfCallBacks; *cb; cb++) (*cb)(m->base, reason, nullptr);
        }
        if (m->entry) ((DllMainFn)m->entry)(m->base, reason, nullptr);
    }
    if (exe_module && exe_module->tls_index >= 0) {
        IMAGE_TLS_DIRECTORY64* t = tls_mods[exe_module->tls_index].dir;
        if (t->AddressOfCallBacks)
            for (TlsCallback* cb = (TlsCallback*)t->AddressOfCallBacks; *cb; cb++) (*cb)(exe_module->base, reason, nullptr);
    }
}

// ---------------------------------------------------------------- loading
static Module* load_file_module(const char* path, int* error) {
    u32 fsize = 0;
    u8* file = read_file(path, &fsize);
    if (!file) { *error = ERROR_FILE_NOT_FOUND; load_error = "can't read the file"; return nullptr; }
    Module* m = map_image(path, file, fsize, false, error);
    ifree(file);
    if (!m) return nullptr;
    if (!resolve_imports(m, error)) return nullptr;
    setup_tls(m);
    protect_sections(m);
    // threads that exist already get this module's .tls block
    for (Thr* t = threads; t; t = t->next) tls_setup_thread(t);
    return m;
}

static Module* load_dll(const char* name, int* error, const char* for_whom) {
    char n[64];
    norm_name(name, n, sizeof(n));
    Module* m = mod_find(n);
    if (m) { m->refs++; return m; }
    const char* td;
    Group g = group_of(n, &td);
    // a DLL that comes with the program wins, except for the core system ones
    bool core = g == G_KERNEL || g == G_NTDLL || g == G_CRT || g == G_MSVCRT || !scmp(n, "user32.dll") || !scmp(n, "gdi32.dll");
    if (!core) {
        char dirs[3][MAX_PATH];
        int nd = 0;
        if (exe_module) {
            slcpy(dirs[nd], exe_module->path, MAX_PATH);
            char* sl = (char*)srchr(dirs[nd], '/');
            if (sl) { *sl = 0; if (!dirs[nd][0]) scpy(dirs[nd], "/"); nd++; }
        }
        slcpy(dirs[nd++], cur_dir, MAX_PATH);
        scpy(dirs[nd++], "/system/win32");
        const char* b = name;
        for (const char* p = name; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
        char fname[80];
        slcpy(fname, b, sizeof(fname));
        if (!schr(fname, '.')) slcat(fname, ".dll", sizeof(fname));
        for (int i = 0; i < nd; i++) {
            char path[MAX_PATH];
            if (find_in_dir(dirs[i], fname, path, sizeof(path))) {
                trace("loading %s from %s", n, path);
                return load_file_module(path, error);
            }
        }
    }
    if (is_system(n) || for_whom) {
        if (!is_system(n)) msg("[WIN32 UNIMPLEMENTED] %s (needed by %s, not found next to the program): every call into it will be trapped", n, for_whom);
        return make_builtin(n);
    }
    *error = ERROR_MOD_NOT_FOUND;
    return nullptr;
}

Module* mod_load(const char* name, int* error) {
    Guard g(loader_lock);
    // a path to a file: load that file
    char path[MAX_PATH];
    bool has_dir = schr(name, '/') || schr(name, '\\');
    if (has_dir && path_to_silicon(name, path, sizeof(path)) && si::file_size(path) >= 0) {
        char n[64];
        norm_name(name, n, sizeof(n));
        Module* m = mod_find(n);
        if (m) { m->refs++; return m; }
        m = load_file_module(path, error);
        if (m) run_dllmain(m);
        return m;
    }
    Module* m = load_dll(name, error, nullptr);
    if (m) run_dllmain(m);
    return m;
}

Module* mod_load_exe(const char* path, int* error) {
    u32 fsize = 0;
    u8* file = read_file(path, &fsize);
    if (!file) { *error = ERROR_FILE_NOT_FOUND; msg("can't read %s", path); return nullptr; }
    load_error = nullptr;
    Module* m = map_image(path, file, fsize, true, error);
    ifree(file);
    if (!m) { msg("%s can't be loaded: %s", path, load_error ? load_error : "unknown error"); return nullptr; }
    exe_module = m;
    peb->ImageBaseAddress = m->base;
    peb->ImageSubsystem = m->nt->OptionalHeader.Subsystem;
    peb->ImageSubsystemMajorVersion = m->nt->OptionalHeader.MajorSubsystemVersion;
    peb->ImageSubsystemMinorVersion = m->nt->OptionalHeader.MinorSubsystemVersion;
    if (!resolve_imports(m, error)) { msg("%s can't be loaded: %s", path, load_error ? load_error : "a DLL is missing"); return nullptr; }
    setup_tls(m);
    protect_sections(m);
    return m;
}

}  // namespace rt
