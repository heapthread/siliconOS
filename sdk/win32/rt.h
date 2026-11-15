// win32run - a user-space Win32 runtime for silicon.
//
// win32run is an ordinary ring 3 silicon program. It loads a 64-bit Windows program (PE32+) into its
// own address space, gives it the thread and process blocks Windows code expects (TEB at gs, PEB),
// and stands in for the Windows system DLLs: every function a program imports from kernel32, ntdll,
// the C runtime, user32 or gdi32 is one of the functions in this folder, written against silicon's
// system calls. Nothing here runs in the kernel, so a misbehaving Windows program can only end itself.
//
// This header is the runtime's own interface: the types, and what the parts offer each other.
#pragma once
#include <stdarg.h>
#include "../user/silicon_user.h"

// every function a Windows program can call uses the Microsoft x64 calling convention
#define WINAPI __attribute__((ms_abi))

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char i8;
typedef short i16;
typedef int i32;
typedef long long i64;
typedef u64 usize;

// ---------------------------------------------------------------- Windows types
typedef int BOOL;
typedef u8 BYTE;
typedef u16 WORD;
typedef u32 DWORD;
typedef u32 UINT;
typedef i32 LONG;
typedef u32 ULONG;
typedef i64 LONGLONG;
typedef u64 ULONGLONG;
typedef u64 SIZE_T;
typedef u64 ULONG_PTR;
typedef u64 DWORD_PTR;
typedef u64 UINT_PTR;
typedef i64 LONG_PTR;
typedef i64 INT_PTR;
typedef u64 WPARAM;
typedef i64 LPARAM;
typedef i64 LRESULT;
typedef wchar_t WCHAR;                   // 16 bits with this toolchain, as on Windows
typedef void* HANDLE;
typedef HANDLE HMODULE;
typedef HANDLE HINSTANCE;
typedef i32 NTSTATUS;
typedef i32 HRESULT;
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(i64)-1)
#define MAX_PATH 260

struct UNICODE_STRING { u16 Length, MaximumLength; u32 pad; WCHAR* Buffer; };
struct ANSI_STRING { u16 Length, MaximumLength; u32 pad; char* Buffer; };
struct LIST_ENTRY { LIST_ENTRY* Flink; LIST_ENTRY* Blink; };
struct FILETIME { u32 dwLowDateTime, dwHighDateTime; };
struct SYSTEMTIME { u16 wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; };
union LARGE_INTEGER { struct { u32 LowPart; i32 HighPart; }; i64 QuadPart; };
struct SECURITY_ATTRIBUTES { u32 nLength; void* lpSecurityDescriptor; BOOL bInheritHandle; };
struct OVERLAPPED { u64 Internal, InternalHigh; u32 Offset, OffsetHigh; HANDLE hEvent; };
struct MEMORY_BASIC_INFORMATION {
    void* BaseAddress; void* AllocationBase; u32 AllocationProtect; u32 pad1;
    SIZE_T RegionSize; u32 State, Protect, Type; u32 pad2;
};
struct RTL_CRITICAL_SECTION {
    void* DebugInfo; LONG LockCount; LONG RecursionCount; HANDLE OwningThread; HANDLE LockSemaphore; ULONG_PTR SpinCount;
};

// error codes the runtime sets
enum {
    ERROR_SUCCESS = 0, ERROR_INVALID_FUNCTION = 1, ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3,
    ERROR_TOO_MANY_OPEN_FILES = 4, ERROR_ACCESS_DENIED = 5, ERROR_INVALID_HANDLE = 6, ERROR_NOT_ENOUGH_MEMORY = 8,
    ERROR_INVALID_DATA = 13, ERROR_OUTOFMEMORY = 14, ERROR_NO_MORE_FILES = 18, ERROR_WRITE_PROTECT = 19,
    ERROR_HANDLE_EOF = 38, ERROR_NOT_SUPPORTED = 50, ERROR_FILE_EXISTS = 80, ERROR_INVALID_PARAMETER = 87,
    ERROR_BROKEN_PIPE = 109, ERROR_CALL_NOT_IMPLEMENTED = 120, ERROR_INSUFFICIENT_BUFFER = 122,
    ERROR_INVALID_NAME = 123, ERROR_MOD_NOT_FOUND = 126, ERROR_PROC_NOT_FOUND = 127, ERROR_DIR_NOT_EMPTY = 145,
    ERROR_ALREADY_EXISTS = 183, ERROR_ENVVAR_NOT_FOUND = 203, ERROR_MORE_DATA = 234, ERROR_NO_MORE_ITEMS = 259,
    ERROR_INVALID_ADDRESS = 487, ERROR_BAD_EXE_FORMAT = 193, ERROR_NOT_FOUND = 1168, ERROR_NO_UNICODE_TRANSLATION = 1113,
    ERROR_TIMEOUT = 1460, WAIT_TIMEOUT = 258, ERROR_CLASS_ALREADY_EXISTS = 1410, ERROR_CLASS_DOES_NOT_EXIST = 1411,
    ERROR_INVALID_WINDOW_HANDLE = 1400, ERROR_RESOURCE_DATA_NOT_FOUND = 1812, ERROR_RESOURCE_TYPE_NOT_FOUND = 1813,
    ERROR_RESOURCE_NAME_NOT_FOUND = 1814, ERROR_NOACCESS = 998, ERROR_NOT_OWNER = 288, ERROR_BAD_PATHNAME = 161,
    ERROR_SHARING_VIOLATION = 32, ERROR_DISK_FULL = 112, ERROR_NEGATIVE_SEEK = 131, ERROR_BAD_LENGTH = 24,
    ERROR_FILENAME_EXCED_RANGE = 206, ERROR_INVALID_FLAGS = 1004, ERROR_SEM_NOT_FOUND = 187,
};

// page protection and allocation flags (VirtualAlloc and friends)
enum {
    PAGE_NOACCESS = 0x01, PAGE_READONLY = 0x02, PAGE_READWRITE = 0x04, PAGE_WRITECOPY = 0x08,
    PAGE_EXECUTE = 0x10, PAGE_EXECUTE_READ = 0x20, PAGE_EXECUTE_READWRITE = 0x40, PAGE_EXECUTE_WRITECOPY = 0x80,
    PAGE_GUARD = 0x100,
    MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_DECOMMIT = 0x4000, MEM_RELEASE = 0x8000, MEM_FREE = 0x10000,
    MEM_PRIVATE = 0x20000, MEM_MAPPED = 0x40000, MEM_IMAGE = 0x1000000, MEM_TOP_DOWN = 0x100000,
};

// ---------------------------------------------------------------- the thread and process blocks
// Only the fields programs (and their C runtimes) really read are named; the offsets are Windows'.
struct PEB;
namespace rt { struct Thr; }
struct TEB {
    void* ExceptionList;                  // 0x00  NT_TIB
    void* StackBase;                      // 0x08
    void* StackLimit;                     // 0x10
    void* SubSystemTib;                   // 0x18
    void* FiberData;                      // 0x20
    void* ArbitraryUserPointer;           // 0x28
    TEB*  Self;                           // 0x30  gs:[0x30]
    void* EnvironmentPointer;             // 0x38
    u64   UniqueProcess;                  // 0x40  ClientId
    u64   UniqueThread;                   // 0x48
    void* ActiveRpcHandle;                // 0x50
    void** ThreadLocalStoragePointer;     // 0x58  one block per module with a .tls section
    PEB*  ProcessEnvironmentBlock;        // 0x60  gs:[0x60]
    u32   LastErrorValue;                 // 0x68
    u8    pad0[0x1478 - 0x6C];
    void* DeallocationStack;              // 0x1478
    void* TlsSlots[64];                   // 0x1480
    LIST_ENTRY TlsLinks;                  // 0x1680
    u8    pad1[0x1780 - 0x1690];
    void** TlsExpansionSlots;             // 0x1780
    u8    pad2[0x2000 - 0x1788];
    rt::Thr* thr;                         // 0x2000  the runtime's own record of this thread (not Windows')
};

struct RTL_USER_PROCESS_PARAMETERS {
    u32 MaximumLength, Length, Flags, DebugFlags;      // 0x00
    HANDLE ConsoleHandle;                               // 0x10
    u32 ConsoleFlags, pad0;                             // 0x18
    HANDLE StandardInput, StandardOutput, StandardError;   // 0x20 0x28 0x30
    UNICODE_STRING CurrentDirectory;                    // 0x38
    HANDLE CurrentDirectoryHandle;                      // 0x48
    UNICODE_STRING DllPath;                             // 0x50
    UNICODE_STRING ImagePathName;                       // 0x60
    UNICODE_STRING CommandLine;                         // 0x70
    WCHAR* Environment;                                 // 0x80
    u32 StartingX, StartingY, CountX, CountY, CountCharsX, CountCharsY, FillAttribute, WindowFlags, ShowWindowFlags, pad1;
    UNICODE_STRING WindowTitle, DesktopInfo, ShellInfo, RuntimeData;
};

struct PEB_LDR_DATA {
    u32 Length; u8 Initialized; u8 pad[3];              // 0x00
    void* SsHandle;                                     // 0x08
    LIST_ENTRY InLoadOrderModuleList;                   // 0x10
    LIST_ENTRY InMemoryOrderModuleList;                 // 0x20
    LIST_ENTRY InInitializationOrderModuleList;         // 0x30
    void* EntryInProgress;
};

struct LDR_DATA_TABLE_ENTRY {
    LIST_ENTRY InLoadOrderLinks;                        // 0x00
    LIST_ENTRY InMemoryOrderLinks;                      // 0x10
    LIST_ENTRY InInitializationOrderLinks;              // 0x20
    void* DllBase;                                      // 0x30
    void* EntryPoint;                                   // 0x38
    u32 SizeOfImage, pad0;                              // 0x40
    UNICODE_STRING FullDllName;                         // 0x48
    UNICODE_STRING BaseDllName;                         // 0x58
    u32 Flags; u16 LoadCount, TlsIndex;                 // 0x68
    LIST_ENTRY HashLinks;
    u32 TimeDateStamp;
};

struct PEB {
    u8 InheritedAddressSpace, ReadImageFileExecOptions, BeingDebugged, BitField;   // 0x00
    u32 pad0;
    HANDLE Mutant;                                      // 0x08
    void* ImageBaseAddress;                             // 0x10
    PEB_LDR_DATA* Ldr;                                  // 0x18
    RTL_USER_PROCESS_PARAMETERS* ProcessParameters;     // 0x20
    void* SubSystemData;                                // 0x28
    HANDLE ProcessHeap;                                 // 0x30
    RTL_CRITICAL_SECTION* FastPebLock;                  // 0x38
    u8 pad1[0xB8 - 0x40];
    u32 NumberOfProcessors;                             // 0xB8
    u32 NtGlobalFlag;                                   // 0xBC
    u8 pad2[0x118 - 0xC0];
    u32 OSMajorVersion, OSMinorVersion;                 // 0x118 0x11C
    u16 OSBuildNumber, OSCSDVersion;                    // 0x120
    u32 OSPlatformId;                                   // 0x124
    u32 ImageSubsystem, ImageSubsystemMajorVersion, ImageSubsystemMinorVersion;   // 0x128
    u8 pad3[0x2C0 - 0x134];
    u32 SessionId;                                      // 0x2C0
    u8 pad4[0x400 - 0x2C4];
};

static_assert(__builtin_offsetof(TEB, ThreadLocalStoragePointer) == 0x58, "TEB");
static_assert(__builtin_offsetof(TEB, LastErrorValue) == 0x68, "TEB");
static_assert(__builtin_offsetof(TEB, TlsSlots) == 0x1480, "TEB");
static_assert(__builtin_offsetof(TEB, TlsExpansionSlots) == 0x1780, "TEB");
static_assert(__builtin_offsetof(PEB, ProcessHeap) == 0x30, "PEB");
static_assert(__builtin_offsetof(PEB, OSMajorVersion) == 0x118, "PEB");
static_assert(__builtin_offsetof(RTL_USER_PROCESS_PARAMETERS, CommandLine) == 0x70, "params");
static_assert(__builtin_offsetof(LDR_DATA_TABLE_ENTRY, BaseDllName) == 0x58, "ldr");

// ---------------------------------------------------------------- cpu context and exceptions (winnt.h shapes)
struct alignas(16) M128A { u64 Low; i64 High; };
struct alignas(16) CONTEXT {
    u64 P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
    u32 ContextFlags, MxCsr;
    u16 SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    u32 EFlags;
    u64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    u64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15;
    u64 Rip;
    union {
        u8 FltSave[512];
        struct { M128A Header[2]; M128A Legacy[8]; M128A Xmm[16]; };
    };
    M128A VectorRegister[26];
    u64 VectorControl, DebugControl, LastBranchToRip, LastBranchFromRip, LastExceptionToRip, LastExceptionFromRip;
};
static_assert(__builtin_offsetof(CONTEXT, Rax) == 0x78, "CONTEXT");
static_assert(__builtin_offsetof(CONTEXT, Rip) == 0xF8, "CONTEXT");
static_assert(__builtin_offsetof(CONTEXT, FltSave) == 0x100, "CONTEXT");
static_assert(sizeof(CONTEXT) == 0x4D0, "CONTEXT");

struct EXCEPTION_RECORD {
    u32 ExceptionCode, ExceptionFlags;
    EXCEPTION_RECORD* ExceptionRecord;
    void* ExceptionAddress;
    u32 NumberParameters, pad;
    u64 ExceptionInformation[15];
};
struct EXCEPTION_POINTERS { EXCEPTION_RECORD* ExceptionRecord; CONTEXT* ContextRecord; };
struct RUNTIME_FUNCTION { u32 BeginAddress, EndAddress, UnwindData; };

// ---------------------------------------------------------------- PE file format
struct IMAGE_DOS_HEADER { u16 e_magic; u16 pad[29]; i32 e_lfanew; };
struct IMAGE_FILE_HEADER { u16 Machine, NumberOfSections; u32 TimeDateStamp, PointerToSymbolTable, NumberOfSymbols; u16 SizeOfOptionalHeader, Characteristics; };
struct IMAGE_DATA_DIRECTORY { u32 VirtualAddress, Size; };
struct IMAGE_OPTIONAL_HEADER64 {
    u16 Magic; u8 MajorLinkerVersion, MinorLinkerVersion;
    u32 SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData, AddressOfEntryPoint, BaseOfCode;
    u64 ImageBase;
    u32 SectionAlignment, FileAlignment;
    u16 MajorOperatingSystemVersion, MinorOperatingSystemVersion, MajorImageVersion, MinorImageVersion, MajorSubsystemVersion, MinorSubsystemVersion;
    u32 Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum;
    u16 Subsystem, DllCharacteristics;
    u64 SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit;
    u32 LoaderFlags, NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[16];
};
struct IMAGE_NT_HEADERS64 { u32 Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER64 OptionalHeader; };
struct IMAGE_SECTION_HEADER {
    char Name[8]; u32 VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData, PointerToRelocations, PointerToLinenumbers;
    u16 NumberOfRelocations, NumberOfLinenumbers; u32 Characteristics;
};
struct IMAGE_IMPORT_DESCRIPTOR { u32 OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk; };
struct IMAGE_EXPORT_DIRECTORY {
    u32 Characteristics, TimeDateStamp; u16 MajorVersion, MinorVersion;
    u32 Name, Base, NumberOfFunctions, NumberOfNames, AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals;
};
struct IMAGE_BASE_RELOCATION { u32 VirtualAddress, SizeOfBlock; };
struct IMAGE_TLS_DIRECTORY64 { u64 StartAddressOfRawData, EndAddressOfRawData, AddressOfIndex, AddressOfCallBacks; u32 SizeOfZeroFill, Characteristics; };
enum { DIR_EXPORT = 0, DIR_IMPORT = 1, DIR_RESOURCE = 2, DIR_EXCEPTION = 3, DIR_BASERELOC = 5, DIR_TLS = 9, DIR_IAT = 12, DIR_DELAY_IMPORT = 13 };
enum { DLL_PROCESS_DETACH = 0, DLL_PROCESS_ATTACH = 1, DLL_THREAD_ATTACH = 2, DLL_THREAD_DETACH = 3 };

// ================================================================ the runtime's own parts
namespace rt {

// ---- options (main.cpp)
extern bool opt_strict;                 // end the program at the first call to a missing function
extern bool opt_trace;                  // narrate loading

// ---- a lock a thread takes for a moment
struct Lock {
    volatile int v = 0;
    void lock() { while (__sync_lock_test_and_set(&v, 1)) si::yield(); }
    void unlock() { __sync_lock_release(&v); }
};
struct Guard { Lock& l; Guard(Lock& l_) : l(l_) { l.lock(); } ~Guard() { l.unlock(); } };

// ---- strings and text (util.cpp)
usize slen(const char* s);
usize wlen(const WCHAR* s);
int   scmp(const char* a, const char* b);
int   sicmp(const char* a, const char* b);                 // ASCII case-insensitive
int   sncmp(const char* a, const char* b, usize n);
int   snicmp(const char* a, const char* b, usize n);
int   wcmp(const WCHAR* a, const WCHAR* b);
int   wicmp(const WCHAR* a, const WCHAR* b);
char* scpy(char* d, const char* s);
void  slcpy(char* d, const char* s, usize cap);            // always terminates
void  slcat(char* d, const char* s, usize cap);
char* sdup(const char* s);
WCHAR* wdup(const WCHAR* s);
const char* sfind(const char* hay, const char* needle);
const char* srchr(const char* s, int c);
const char* schr(const char* s, int c);
int   lower(int c);
int   upper(int c);
// UTF-8 <-> UTF-16. n < 0: the source is NUL-terminated and the NUL is converted too. cap == 0: just count.
// Return the number of units written (or needed), 0 when the output is too small.
int   utf8_from_wide(const WCHAR* w, int n, char* out, int cap);
int   wide_from_utf8(const char* s, int n, WCHAR* out, int cap);
char* utf8_dup(const WCHAR* w);                            // malloc'd copies
WCHAR* wide_dup(const char* s);

// ---- formatted output (fmt.cpp): the engine behind the runtime's own messages and the C runtime's printf
struct Sink { void (*put)(Sink* self, const char* s, usize n); usize total; };
enum { FMT_LEGACY_WIDE = 1, FMT_THREE_DIGIT_EXP = 2 };   // %s/%c take wide arguments (Microsoft wprintf); 1.0e+000 (msvcrt)
int vformat(Sink* out, const char* fmt, va_list ap, int flags = 0);
int vsnfmt(char* buf, usize cap, const char* fmt, va_list ap);       // like vsnprintf
int snfmt(char* buf, usize cap, const char* fmt, ...);
int fmt_double(char* out, double v, char conv, int prec, int flags);   // one %e %f %g %a conversion (no padding); flags: 1 '#', 2 three-digit exponent
double parse_double(const char* s, const char** end);
double wparse_double(const WCHAR* s, const WCHAR** end);

// ---- messages (util.cpp)
void con_out(const char* s, usize n);   // to the program's console (and the serial log), all of it
void msg(const char* fmt, ...);         // a line from the runtime itself: "[win32run] ..."
void trace(const char* fmt, ...);       // only with --trace
[[noreturn]] void quit(int code);       // end the process now

// ---- the runtime's own memory (not the Windows heaps)
static inline void* ialloc(usize n) { return si::malloc(n); }
static inline void  ifree(void* p) { si::free(p); }
void* izalloc(usize n);                 // zeroed

// ---- virtual memory (vm.cpp): the address space above 4 GiB, with VirtualAlloc's rules
void* vm_alloc(void* addr, usize size, u32 type, u32 protect, u32 kind = MEM_PRIVATE);
bool  vm_free(void* addr, usize size, u32 type);
bool  vm_protect(void* addr, usize size, u32 protect, u32* old);
usize vm_query(const void* addr, MEMORY_BASIC_INFORMATION* mbi);
bool  vm_readable(const void* addr, usize size);
bool  vm_writable(const void* addr, usize size);
u64   vm_committed();                   // bytes

// ---- the heap behind HeapAlloc, malloc and operator new (heap.cpp)
void* h_alloc(usize n, bool zero = false);
void  h_free(void* p);
void* h_realloc(void* p, usize n, bool zero = false);
usize h_size(void* p);                  // (usize)-1 when p isn't a block
void* h_aligned(usize n, usize align);
void  h_aligned_free(void* p);

// ---- modules (pe.cpp)
struct Export { const char* name; void* fn; };
// a table of functions the runtime implements, registered under the DLL it belongs to
struct ExportTable { const char* dll; const Export* list; int count; ExportTable* next; };
void register_exports(ExportTable* t);
#define EXPORT_TABLE(dllname, array) \
    static rt::ExportTable _tbl_##array = {dllname, array, (int)(sizeof(array) / sizeof(array[0])), nullptr}; \
    static struct _Reg_##array { _Reg_##array() { rt::register_exports(&_tbl_##array); } } _reg_##array
#define E(n) {#n, (void*)&n}
#define EA(name, fn) {name, (void*)&fn}

struct Module {
    char   name[64];                    // "kernel32.dll": lower case, with extension
    WCHAR  wpath[MAX_PATH];             // C:\... as Windows shows it
    char   path[MAX_PATH];              // where the file is in silicon ("" for built-in modules)
    u8*    base;
    u32    size;
    void*  entry;
    bool   builtin;                     // one of the runtime's stand-ins, not a file
    bool   is_dll;
    bool   inited;                      // DllMain(PROCESS_ATTACH) ran
    int    refs;
    IMAGE_NT_HEADERS64* nt;
    LDR_DATA_TABLE_ENTRY* ldr;
    int    tls_index;                   // -1: no .tls
    RUNTIME_FUNCTION* pdata;
    u32    pdata_count;
    Module* next;
};
extern Module* modules;                 // in load order; the first is the program
extern Module* exe_module;
Module* mod_load(const char* name, int* error);             // LoadLibrary: loads imports, runs DllMain
Module* mod_load_exe(const char* silicon_path, int* error);
Module* mod_find(const char* name);                          // by name ("user32", "USER32.DLL", a full path)
Module* mod_by_base(void* base);
Module* mod_by_addr(const void* addr);
void*   mod_export(Module* m, const char* name, u32 ordinal, bool for_getproc);
void*   find_builtin(const char* name, const char** dll);   // one of the runtime's functions by name
void    mod_run_tls_callbacks(u32 reason);
void    mod_notify_thread(u32 reason);                       // DllMain THREAD_ATTACH / DETACH for every DLL
void    mod_init_all();                                      // run the DllMains that haven't run yet
const char* describe_addr(const void* addr, char* buf, usize cap);   // "prog.exe+0x1234"
void*   image_dir(Module* m, int index, u32* size);

// ---- missing functions (trap.cpp)
void* make_trap(const char* dll, const char* func);         // a stub that reports "[WIN32 UNIMPLEMENTED] dll!func" when called
void  install_fault_handler();
int   traps_hit();
extern "C" u64 rt_call_on_stack(void* fn, void* arg, void* stack_top);
// Windows exceptions (seh.cpp)
struct DISPATCHER_CONTEXT {
    u64 ControlPc, ImageBase; RUNTIME_FUNCTION* FunctionEntry; u64 EstablisherFrame, TargetIp;
    CONTEXT* ContextRecord; void* LanguageHandler; void* HandlerData; void* HistoryTable; u32 ScopeIndex, Fill0;
};
bool  dispatch_exception(EXCEPTION_RECORD* rec, CONTEXT* ctx);     // true: a handler said continue with ctx
extern "C" void WINAPI rt_RaiseException(u32 code, u32 flags, u32 nargs, const u64* args);
extern "C" void WINAPI rt_RtlCaptureContext(CONTEXT* c);
extern "C" void* WINAPI rt_RtlVirtualUnwind(u32 type, u64 base, u64 pc, RUNTIME_FUNCTION* fn, CONTEXT* c, void** data, u64* frame, void* ptrs);
void  random_bytes(void* buf, usize n);
u32   wupper(u32 c);
u32   wlower(u32 c);
u16   ctype1(u32 c);                    // GetStringTypeW's C1_ flags for one character
void* res_lookup(HMODULE m, const WCHAR* type, const WCHAR* name, u32* size);   // a resource's data
void WINAPI rt_GetLocalTime(SYSTEMTIME* s);
u64   filetime_now();
[[noreturn]] void restore_context(CONTEXT* ctx);
[[noreturn]] void unhandled_exception(EXCEPTION_RECORD* rec, CONTEXT* ctx);
RUNTIME_FUNCTION* lookup_function(u64 pc, u64* base);
void  backtrace(CONTEXT* ctx, int max);

// ---- threads and the process blocks (env.cpp)
typedef DWORD (WINAPI *ThreadFn)(void* arg);
struct Thr {
    TEB*  teb;
    u32   id;                           // the Windows thread id
    int   ktid;                         // silicon's
    u8*   stack;                        // lowest address
    usize stack_size;
    ThreadFn fn;
    void* arg;
    volatile int started;
    volatile int done;
    u32   exit_code;
    volatile int suspend;               // CREATE_SUSPENDED: waits for ResumeThread
    void** tls_blocks;                  // the array TEB.ThreadLocalStoragePointer points at
    int   tls_cap;
    void* fls[128];                     // fiber-local storage (one fiber per thread here)
    Thr*  next;
    volatile int refs;
    void* crt;                          // the C runtime's per-thread state (errno, strtok...), see crt.h
    struct Obj* obj;                    // its handle object (sync.cpp), made when first asked for
    bool  in_fault;                     // running the fault handler (silicon wants fault_return to leave it)
    int   dispatch_depth;               // exceptions being dispatched on this thread (seh.cpp)
    CONTEXT* dispatch_ctx[8];           // where each of them happened
};
extern PEB* peb;
extern Thr* threads;
extern Lock thread_lock;
static inline TEB* teb() { TEB* t; __asm__("movq %%gs:0x30, %0" : "=r"(t)); return t; }
static inline Thr* cur_thr() { return teb()->thr; }
static inline void set_error(u32 e) { teb()->LastErrorValue = e; }
static inline u32 get_error() { return teb()->LastErrorValue; }
void  env_init(const char* exe_path, const WCHAR* cmdline);  // PEB, process parameters, the first thread's TEB
Thr*  thr_create(ThreadFn fn, void* arg, usize stack_size, bool suspended);
[[noreturn]] void thr_exit(u32 code);
void  thr_release(Thr* t);
void  tls_setup_thread(Thr* t);         // the .tls blocks of every loaded module, for this thread
void  ldr_add(Module* m);               // into the PEB's module lists
[[noreturn]] void process_exit(u32 code);
extern WCHAR* env_block;                // the environment: NAME=value\0...\0\0
extern Lock env_lock;
const WCHAR* env_get(const WCHAR* name);
bool  env_set(const WCHAR* name, const WCHAR* value);        // value == nullptr removes it
extern WCHAR* cmdline_w;
extern char*  cmdline_a;

// ---- handles, files and the console (handle.cpp, file.cpp)
enum ObjType { OBJ_NONE, OBJ_FILE, OBJ_CONSOLE, OBJ_THREAD, OBJ_EVENT, OBJ_MUTEX, OBJ_SEMAPHORE, OBJ_FIND, OBJ_NUL,
               OBJ_PROCESS, OBJ_MAPPING, OBJ_TIMER, OBJ_PIPE };
struct Obj {
    ObjType type;
    volatile int refs;
    void (*destroy)(Obj*);
};
HANDLE h_new(Obj* o);                   // takes over the caller's reference
Obj*   h_get(HANDLE h, ObjType type = OBJ_NONE);   // nullptr (and ERROR_INVALID_HANDLE) when it isn't one
bool   h_close(HANDLE h);
void   obj_ref(Obj* o);
void   obj_unref(Obj* o);
HANDLE h_dup(HANDLE h);

// a file's contents. Several handles to the same file share one of these
struct FileData {
    char  path[MAX_PATH];
    u8*   data;
    usize size, cap;
    bool  dirty;
    bool  delete_on_close;
    int   refs;
    FileData* next;
};
struct FileObj : Obj {
    FileData* fd;
    u64   pos;
    u32   access;                       // GENERIC_READ / GENERIC_WRITE
    bool  append;
    char* dirpath;                      // a folder opened with FILE_FLAG_BACKUP_SEMANTICS (fd is null)
};
// silicon file stamps are seconds since 2000-01-01; FILETIME counts 100 ns since 1601-01-01
static inline u64 stamp_to_filetime(u32 st) { return ((u64)st + 12591158400ull) * 10000000ull; }
static inline u32 filetime_to_stamp(u64 ft) { u64 s = ft / 10000000ull; return s > 12591158400ull ? (u32)(s - 12591158400ull) : 0; }
struct ConsoleObj : Obj { int stream; u32 mode; };          // 0 input, 1 output, 2 error
extern HANDLE std_handles[3];
// Windows path -> silicon path ("C:\home\a.txt", "a.txt", "..\x" -> "/home/a.txt"...). false: not a usable path
bool  path_to_silicon(const char* win, char* out, usize cap);
bool  wpath_to_silicon(const WCHAR* win, char* out, usize cap);
void  path_to_windows(const char* silicon, char* out, usize cap);    // "/home/a" -> "C:\home\a"
extern char cur_dir[MAX_PATH];          // the current directory, as a silicon path
FileObj* file_open(const char* silicon_path, u32 access, u32 disposition, u32 flags, u32* error);
bool  file_flush(FileData* fd);
i64   file_read(FileObj* f, void* buf, usize n);
i64   file_write(FileObj* f, const void* buf, usize n);
i64   con_read_line(char* buf, usize cap);                 // blocks; 0 at the end of input
void  handles_init();
void  files_flush_all();

// ---- waiting (sync.cpp): the one loop behind WaitForSingleObject, Sleep, critical sections
u64   now_ms();
void  nap(u32 spins);                   // yield, then sleep a little once it has gone on for a while

}  // namespace rt
