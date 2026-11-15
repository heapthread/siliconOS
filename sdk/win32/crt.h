// win32run - the C runtime (msvcrt.dll, ucrtbase.dll and the api-ms-win-crt-* sets): what its parts share.
#pragma once
#include "rt.h"

namespace rt {

// every C runtime function is registered under ucrtbase.dll (msvcrt.dll and the api-ms-win-crt names
// find them there); X(name) exports c_name as name
#define X(n) {#n, (void*)&c_##n}
#define CDECL WINAPI                    // on x64 there is only one calling convention

// errno values
enum { EPERM = 1, ENOENT = 2, ESRCH = 3, EINTR = 4, EIO = 5, ENXIO = 6, E2BIG = 7, ENOEXEC = 8, EBADF = 9, ECHILD = 10,
       EAGAIN = 11, ENOMEM = 12, EACCES = 13, EFAULT = 14, EBUSY = 16, EEXIST = 17, EXDEV = 18, ENODEV = 19, ENOTDIR = 20,
       EISDIR = 21, EINVAL = 22, ENFILE = 23, EMFILE = 24, ENOTTY = 25, EFBIG = 27, ENOSPC = 28, ESPIPE = 29, EROFS = 30,
       EMLINK = 31, EPIPE = 32, EDOM = 33, ERANGE = 34, EDEADLK = 36, ENAMETOOLONG = 38, ENOLCK = 39, ENOSYS = 40,
       ENOTEMPTY = 41, EILSEQ = 42, STRUNCATE = 80 };

struct CrtThread {
    int   err;
    u32   doserr;
    char* strtok_next;
    WCHAR* wcstok_next;
    u32   rand_seed;
    u8    tm_buf[64];                   // what gmtime/localtime return
    char  asctime_buf[32];
    WCHAR wasctime_buf[32];
    char  tmpnam_buf[MAX_PATH];
    char  strerror_buf[96];
    u32   mbstate;
};
CrtThread* crt_thread();
static inline int* errno_ptr() { return &crt_thread()->err; }
static inline void set_errno(int e) { crt_thread()->err = e; }
int  errno_from_win32(u32 e);
void set_errno_from_win32();            // from GetLastError

// a kernel32 function of the runtime by name (the CRT is written on top of the Win32 layer, like Microsoft's)
template <typename F> static inline F k32(const char* name) { return (F)find_builtin(name, nullptr); }

// ---- the environment as the C runtime keeps it (UTF-8 and UTF-16 copies)
extern char** c_environ_arr;
extern WCHAR** c_wenviron_arr;
void crt_env_refresh();

// ---- low-level I/O (crt_stdio.cpp)
int  fd_new(HANDLE h, int flags);
HANDLE fd_handle(int fd);

// ---- invalid parameters
void invalid_parameter();

// ---- argv
int  split_cmdline(const WCHAR* line, WCHAR** out, WCHAR* buf, int* nchars);    // CommandLineToArgvW's rules
void crt_args_init();
extern int c_argc;
extern char** c_argv;
extern WCHAR** c_wargv;

// ---- unix time <-> silicon stamps
static inline i64 stamp_to_unix(u32 st) { return (i64)st + 946684800ll; }

// ---- scanf (crt_stdio.cpp)
struct ScanSrc { int (*get)(ScanSrc*); void (*unget)(ScanSrc*, int); int count; };
enum { SCAN_SECURE = 1, SCAN_LEGACY_WIDE = 2, SCAN_WIDE_FMT = 4 };
int vscan(ScanSrc* src, const void* fmt, va_list ap, int flags);

}  // namespace rt
