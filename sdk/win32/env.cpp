// win32run - the process and thread blocks (PEB, TEB), threads, and the environment.
//
// Windows code finds its thread block at gs:[0x30] and the process block at gs:[0x60]; compilers and C
// runtimes read both directly (the stack limits, the last error, thread-local storage, the process heap,
// the command line). Each thread here gets a real TEB in its own memory and silicon points the thread's
// gs base at it (the set_gs system call), so those reads work without any help.
#include "rt.h"

namespace rt {

PEB* peb = nullptr;
Thr* threads = nullptr;
Lock thread_lock;
WCHAR* env_block = nullptr;
Lock env_lock;
WCHAR* cmdline_w = nullptr;
char* cmdline_a = nullptr;
static u32 next_tid = 0x104;
static RTL_USER_PROCESS_PARAMETERS* params = nullptr;
static RTL_CRITICAL_SECTION peb_lock;

static void* sysalloc(usize n) { return vm_alloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }

static void ustr(UNICODE_STRING& u, const WCHAR* s) {
    usize n = wlen(s);
    u.Buffer = (WCHAR*)h_alloc((n + 1) * 2);
    memcpy(u.Buffer, s, (n + 1) * 2);
    u.Length = (u16)(n * 2);
    u.MaximumLength = (u16)(n * 2 + 2);
}

// ---------------------------------------------------------------- the environment
static usize block_len(const WCHAR* b) {                 // in WCHARs, the final double NUL included
    usize n = 0;
    while (b[n]) n += wlen(b + n) + 1;
    return n + 1;
}

static const WCHAR* find_var(const WCHAR* name, usize* len_out) {
    usize nl = wlen(name);
    for (const WCHAR* p = env_block; *p; p += wlen(p) + 1) {
        bool eq = true;
        for (usize i = 0; i < nl; i++) if (lower(p[i]) != lower(name[i])) { eq = false; break; }
        if (eq && p[nl] == '=') { if (len_out) *len_out = wlen(p); return p; }
    }
    return nullptr;
}

const WCHAR* env_get(const WCHAR* name) {
    const WCHAR* p = find_var(name, nullptr);
    return p ? p + wlen(name) + 1 : nullptr;
}

bool env_set(const WCHAR* name, const WCHAR* value) {
    if (!name[0]) return false;
    for (const WCHAR* p = name + 1; *p; p++) if (*p == '=') return false;
    usize old_len = block_len(env_block);
    usize vlen = value ? wlen(value) : 0, nlen = wlen(name);
    WCHAR* nb = (WCHAR*)h_alloc((old_len + nlen + vlen + 4) * 2);
    if (!nb) return false;
    usize o = 0;
    for (const WCHAR* p = env_block; *p; p += wlen(p) + 1) {
        usize l = wlen(p);
        bool same = l > nlen && p[nlen] == '=';
        for (usize i = 0; same && i < nlen; i++) if (lower(p[i]) != lower(name[i])) same = false;
        if (same) continue;
        memcpy(nb + o, p, (l + 1) * 2);
        o += l + 1;
    }
    if (value) {
        memcpy(nb + o, name, nlen * 2); o += nlen;
        nb[o++] = '=';
        memcpy(nb + o, value, vlen * 2); o += vlen;
        nb[o++] = 0;
    }
    nb[o] = 0;
    WCHAR* old = env_block;
    env_block = nb;
    if (params) params->Environment = nb;
    (void)old;                                           // a program may still be reading it: keep it
    return true;
}

static void env_init_block() {
    static const char* defaults[] = {
        "ALLUSERSPROFILE=C:\\ProgramData", "APPDATA=C:\\home\\AppData\\Roaming", "CommonProgramFiles=C:\\Program Files\\Common Files",
        "COMPUTERNAME=SILICON", "ComSpec=C:\\Windows\\System32\\cmd.exe", "HOMEDRIVE=C:", "HOMEPATH=\\home",
        "LOCALAPPDATA=C:\\home\\AppData\\Local", "NUMBER_OF_PROCESSORS=1", "OS=Windows_NT",
        "Path=C:\\Windows\\System32;C:\\Windows;C:\\system\\win32", "PATHEXT=.COM;.EXE;.BAT;.CMD",
        "PROCESSOR_ARCHITECTURE=AMD64", "PROCESSOR_IDENTIFIER=Intel64 Family 6 Model 85 Stepping 7, GenuineIntel",
        "PROCESSOR_LEVEL=6", "ProgramData=C:\\ProgramData", "ProgramFiles=C:\\Program Files", "ProgramFiles(x86)=C:\\Program Files (x86)",
        "PROMPT=$P$G", "PUBLIC=C:\\Users\\Public", "SystemDrive=C:", "SystemRoot=C:\\Windows", "TEMP=C:\\home\\tmp", "TMP=C:\\home\\tmp",
        "USERDOMAIN=SILICON", "USERNAME=user", "USERPROFILE=C:\\home", "windir=C:\\Windows", "WIN32RUN=1",
    };
    usize total = 1;
    for (const char* d : defaults) total += slen(d) + 1;
    env_block = (WCHAR*)h_alloc(total * 2);
    usize o = 0;
    for (const char* d : defaults) { o += (usize)wide_from_utf8(d, -1, env_block + o, (int)(total - o)); }
    env_block[o] = 0;
}

// ---------------------------------------------------------------- the module lists
void ldr_add(Module* m) {
    LDR_DATA_TABLE_ENTRY* e = (LDR_DATA_TABLE_ENTRY*)h_alloc(sizeof(LDR_DATA_TABLE_ENTRY) + 16, true);
    e->DllBase = m->base;
    e->EntryPoint = m->entry;
    e->SizeOfImage = m->size;
    ustr(e->FullDllName, m->wpath);
    const WCHAR* b = m->wpath;
    for (const WCHAR* p = m->wpath; *p; p++) if (*p == '\\') b = p + 1;
    ustr(e->BaseDllName, b);
    e->LoadCount = 0xFFFF;
    e->Flags = 0x4;
    e->TlsIndex = (u16)(m->tls_index < 0 ? 0 : m->tls_index);
    e->HashLinks.Flink = e->HashLinks.Blink = &e->HashLinks;
    PEB_LDR_DATA* l = peb->Ldr;
    LIST_ENTRY* lists[3] = {&l->InLoadOrderModuleList, &l->InMemoryOrderModuleList, &l->InInitializationOrderModuleList};
    LIST_ENTRY* links[3] = {&e->InLoadOrderLinks, &e->InMemoryOrderLinks, &e->InInitializationOrderLinks};
    for (int i = 0; i < 3; i++) {
        LIST_ENTRY* head = lists[i];
        links[i]->Flink = head;
        links[i]->Blink = head->Blink;
        head->Blink->Flink = links[i];
        head->Blink = links[i];
    }
    m->ldr = e;
}

// ---------------------------------------------------------------- threads
static TEB* new_teb(Thr* t) {
    TEB* teb_ = (TEB*)sysalloc(0x3000);
    if (!teb_) return nullptr;
    teb_->Self = teb_;
    teb_->ProcessEnvironmentBlock = peb;
    teb_->UniqueProcess = 0x40;
    teb_->UniqueThread = t->id;
    teb_->thr = t;
    teb_->TlsLinks.Flink = teb_->TlsLinks.Blink = &teb_->TlsLinks;
    teb_->ExceptionList = (void*)~0ull;
    if (t->stack) {
        teb_->StackBase = t->stack + t->stack_size;
        teb_->StackLimit = t->stack;
        teb_->DeallocationStack = t->stack;
    }
    return teb_;
}

static void link_thread(Thr* t) {
    Guard g(thread_lock);
    t->next = threads;
    threads = t;
}

// the first thread: the one silicon started win32run on (its stack is set later, see main.cpp)
static Thr* main_thr() {
    Thr* t = (Thr*)izalloc(sizeof(Thr));
    t->id = next_tid;
    next_tid += 4;
    t->refs = 1;
    t->started = 1;
    t->teb = new_teb(t);
    link_thread(t);
    si::set_gs(t->teb);
    return t;
}

// threads that have ended but whose stacks are still mapped (they can't free their own)
static void reap_stacks() {
    Guard g(thread_lock);
    for (Thr* t = threads; t; t = t->next) {
        if (t->done == 2 && t->stack) {
            vm_free(t->stack, 0, MEM_RELEASE);
            t->stack = nullptr;
        }
        if (t->done == 1) t->done = 2;                       // next time round: it has certainly left its stack
    }
}

static void WINAPI thread_start(Thr* t) {
    si::set_gs(t->teb);
    while (t->suspend > 0) si::sleep(2);
    t->started = 1;
    mod_notify_thread(DLL_THREAD_ATTACH);
    u32 code = t->fn(t->arg);
    thr_exit(code);
}

Thr* thr_create(ThreadFn fn, void* arg, usize stack_size, bool suspended) {
    reap_stacks();
    if (!stack_size) stack_size = exe_module ? exe_module->nt->OptionalHeader.SizeOfStackReserve : (1u << 20);
    if (stack_size < (256u << 10)) stack_size = 256u << 10;
    if (stack_size > (16u << 20)) stack_size = 16u << 20;
    stack_size = (stack_size + 0xFFFF) & ~0xFFFFull;
    Thr* t = (Thr*)izalloc(sizeof(Thr));
    if (!t) { set_error(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    t->stack = (u8*)vm_alloc(nullptr, stack_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!t->stack) { ifree(t); set_error(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    t->stack_size = stack_size;
    thread_lock.lock();
    t->id = next_tid;
    next_tid += 4;
    thread_lock.unlock();
    t->fn = fn;
    t->arg = arg;
    t->refs = 2;                                             // the thread itself, and the handle
    t->suspend = suspended ? 1 : 0;
    t->teb = new_teb(t);
    if (!t->teb) { vm_free(t->stack, 0, MEM_RELEASE); ifree(t); set_error(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    tls_setup_thread(t);
    link_thread(t);
    t->ktid = si::thread_at((void*)&thread_start, t, t->stack + stack_size);
    if (t->ktid < 0) {
        t->done = 1;
        t->refs = 1;
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        msg("can't start another thread (silicon allows 64 per program)");
        return nullptr;
    }
    return t;
}

void thr_exit(u32 code) {
    Thr* t = cur_thr();
    mod_notify_thread(DLL_THREAD_DETACH);
    t->exit_code = code;
    __sync_synchronize();
    t->done = 1;
    // the last thread leaving ends the process, as on Windows
    bool any = false;
    thread_lock.lock();
    for (Thr* o = threads; o; o = o->next) if (!o->done) { any = true; break; }
    thread_lock.unlock();
    if (!any) process_exit(code);
    si::sys(si::S_THREAD_EXIT);
    for (;;) {}
}

void thr_release(Thr* t) { __sync_fetch_and_sub(&t->refs, 1); }

static volatile int exiting = 0;
void process_exit(u32 code) {
    if (__sync_lock_test_and_set(&exiting, 1)) { si::sleep(1000000); }
    // DLLs get to clean up (in reverse load order, as Windows does)
    Module* list[128];
    int n = 0;
    for (Module* m = modules; m && n < 128; m = m->next) list[n++] = m;
    for (int i = n - 1; i >= 0; i--) {
        Module* m = list[i];
        if (m->builtin || !m->is_dll || !m->inited || !m->entry) continue;
        ((BOOL (WINAPI*)(void*, u32, void*))m->entry)(m->base, DLL_PROCESS_DETACH, (void*)1);
    }
    files_flush_all();
    trace("exit code %u", code);
    si::exit((int)code);
}

// ---------------------------------------------------------------- start-up
void env_init(const char* exe_path, const WCHAR* cmdline) {
    peb = (PEB*)sysalloc(0x1000);
    peb->OSMajorVersion = 10;
    peb->OSMinorVersion = 0;
    peb->OSBuildNumber = 19045;
    peb->OSPlatformId = 2;
    peb->NumberOfProcessors = 1;
    peb->SessionId = 1;
    peb->ImageSubsystem = 3;
    peb->FastPebLock = &peb_lock;
    peb_lock.LockCount = -1;
    PEB_LDR_DATA* ldr = (PEB_LDR_DATA*)sysalloc(0x1000);
    ldr->Length = sizeof(PEB_LDR_DATA);
    ldr->Initialized = 1;
    ldr->InLoadOrderModuleList.Flink = ldr->InLoadOrderModuleList.Blink = &ldr->InLoadOrderModuleList;
    ldr->InMemoryOrderModuleList.Flink = ldr->InMemoryOrderModuleList.Blink = &ldr->InMemoryOrderModuleList;
    ldr->InInitializationOrderModuleList.Flink = ldr->InInitializationOrderModuleList.Blink = &ldr->InInitializationOrderModuleList;
    peb->Ldr = ldr;
    peb->ProcessHeap = (HANDLE)0x10000;                      // the process heap's handle (see kernel32 heap functions)

    main_thr();
    env_init_block();

    cmdline_w = wdup(cmdline);
    cmdline_a = utf8_dup(cmdline);
    params = (RTL_USER_PROCESS_PARAMETERS*)sysalloc(0x2000);
    params->MaximumLength = params->Length = sizeof(RTL_USER_PROCESS_PARAMETERS);
    params->Flags = 1;                                       // normalized
    ustr(params->CommandLine, cmdline_w);
    char win[MAX_PATH];
    WCHAR w[MAX_PATH];
    path_to_windows(exe_path, win, sizeof(win));
    wide_from_utf8(win, -1, w, MAX_PATH);
    ustr(params->ImagePathName, w);
    path_to_windows(cur_dir, win, sizeof(win));
    slcat(win, "\\", sizeof(win));
    wide_from_utf8(win, -1, w, MAX_PATH);
    ustr(params->CurrentDirectory, w);
    ustr(params->DllPath, L"C:\\Windows\\System32");
    ustr(params->WindowTitle, params->ImagePathName.Buffer);
    params->Environment = env_block;
    params->StandardInput = std_handles[0];
    params->StandardOutput = std_handles[1];
    params->StandardError = std_handles[2];
    params->ShowWindowFlags = 1;                             // SW_SHOWNORMAL
    peb->ProcessParameters = params;
}

}  // namespace rt
