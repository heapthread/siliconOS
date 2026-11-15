// win32run - the C runtime: low-level I/O (file descriptors), stdio streams, the printf and scanf
// families in all their forms (msvcrt's, UCRT's __stdio_common_*, narrow and wide), directories and stat.
//
// Text mode works as on Windows: a stream opened without "b" turns "\n" into "\r\n" when writing to a
// file and "\r\n" back into "\n" when reading. Console output is passed through as is.
#include "crt.h"

namespace rt {

// ---------------------------------------------------------------- file descriptors
enum { FD_TEXT = 1, FD_APPEND = 2, FD_CONSOLE = 4, FD_EOF = 8, FD_PIPE = 16 };
enum { O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_APPEND = 8, O_CREAT = 0x100, O_TRUNC = 0x200, O_EXCL = 0x400,
       O_TEXT = 0x4000, O_BINARY = 0x8000, O_WTEXT = 0x10000, O_U16TEXT = 0x20000, O_U8TEXT = 0x40000, O_TEMPORARY = 0x40 };
struct Fd { HANDLE h; u32 flags; bool used; int pend; };
static const int MAX_FD = 512;
static Fd fds[MAX_FD];
static Lock fd_lock;
static bool fds_ready = false;

typedef BOOL (WINAPI *ReadFileFn)(HANDLE, void*, u32, u32*, OVERLAPPED*);
typedef BOOL (WINAPI *WriteFileFn)(HANDLE, const void*, u32, u32*, OVERLAPPED*);
static ReadFileFn pReadFile;
static WriteFileFn pWriteFile;

static void fds_init() {
    if (fds_ready) return;
    pReadFile = k32<ReadFileFn>("ReadFile");
    pWriteFile = k32<WriteFileFn>("WriteFile");
    auto gft = k32<u32 (WINAPI*)(HANDLE)>("GetFileType");
    for (int i = 0; i < 3; i++) {
        fds[i].h = std_handles[i];
        fds[i].used = true;
        fds[i].pend = -1;
        u32 t = gft(std_handles[i]);
        fds[i].flags = FD_TEXT | (t == 2 ? FD_CONSOLE : t == 3 ? FD_PIPE : 0);
    }
    fds_ready = true;
}

int fd_new(HANDLE h, int flags) {
    fds_init();
    Guard g(fd_lock);
    for (int i = 3; i < MAX_FD; i++)
        if (!fds[i].used) { fds[i] = {h, (u32)flags, true, -1}; return i; }
    set_errno(EMFILE);
    return -1;
}
static Fd* fd_get(int fd) {
    fds_init();
    if (fd < 0 || fd >= MAX_FD || !fds[fd].used) { set_errno(EBADF); return nullptr; }
    return &fds[fd];
}
HANDLE fd_handle(int fd) { Fd* f = fd_get(fd); return f ? f->h : INVALID_HANDLE_VALUE; }

// raw bytes, no translation
static int fd_raw_read(int fd, void* buf, u32 n) {
    Fd* f = fd_get(fd);
    if (!f) return -1;
    if (!n) return 0;
    u8* b = (u8*)buf;
    int got = 0;
    if (f->pend >= 0) { b[got++] = (u8)f->pend; f->pend = -1; if (!--n) return got; }
    u32 k = 0;
    if (!pReadFile(f->h, b + got, n, &k, nullptr)) {
        if (get_error() == ERROR_BROKEN_PIPE) return got;
        set_errno_from_win32();
        return got ? got : -1;
    }
    return got + (int)k;
}
static int fd_raw_write(int fd, const void* buf, u32 n) {
    Fd* f = fd_get(fd);
    if (!f) return -1;
    if (f->flags & FD_APPEND) {
        auto sfp = k32<BOOL (WINAPI*)(HANDLE, LARGE_INTEGER, LARGE_INTEGER*, u32)>("SetFilePointerEx");
        LARGE_INTEGER z; z.QuadPart = 0;
        sfp(f->h, z, nullptr, 2);
    }
    u32 k = 0;
    if (!pWriteFile(f->h, buf, n, &k, nullptr)) { set_errno_from_win32(); return -1; }
    return (int)k;
}

static int CDECL c__read(int fd, void* buf, u32 n) {
    Fd* f = fd_get(fd);
    if (!f) return -1;
    if (!(f->flags & FD_TEXT) || (f->flags & FD_CONSOLE)) return fd_raw_read(fd, buf, n);
    if (f->flags & FD_EOF) return 0;
    int k = fd_raw_read(fd, buf, n);
    if (k <= 0) return k;
    u8* b = (u8*)buf;
    int o = 0;
    for (int i = 0; i < k; i++) {
        u8 c = b[i];
        if (c == 0x1A) { f->flags |= FD_EOF; break; }           // Ctrl-Z ends a text file
        if (c == '\r') {
            if (i + 1 < k) { if (b[i + 1] == '\n') continue; }
            else {
                u8 nx;
                int m = fd_raw_read(fd, &nx, 1);
                if (m == 1) { if (nx == '\n') { b[o++] = '\n'; continue; } f->pend = nx; }
            }
        }
        b[o++] = c;
    }
    return o;
}
static int CDECL c__write(int fd, const void* buf, u32 n) {
    Fd* f = fd_get(fd);
    if (!f) return -1;
    if (!(f->flags & FD_TEXT) || (f->flags & FD_CONSOLE)) return fd_raw_write(fd, buf, n);
    const u8* s = (const u8*)buf;
    u8 tmp[1024];
    u32 i = 0;
    while (i < n) {
        u32 o = 0;
        while (i < n && o < sizeof(tmp) - 2) { if (s[i] == '\n') tmp[o++] = '\r'; tmp[o++] = s[i++]; }
        if (fd_raw_write(fd, tmp, o) < 0) return -1;
    }
    return (int)n;
}

static HANDLE open_handle(const WCHAR* path, int oflag, int* fdflags) {
    u32 access = (oflag & 3) == O_RDONLY ? 0x80000000u : (oflag & 3) == O_WRONLY ? 0x40000000u : 0xC0000000u;
    u32 disp;
    if ((oflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) disp = 1;
    else if ((oflag & (O_CREAT | O_TRUNC)) == (O_CREAT | O_TRUNC)) disp = 2;
    else if (oflag & O_CREAT) disp = 4;
    else if (oflag & O_TRUNC) disp = 5;
    else disp = 3;
    u32 flags = (oflag & O_TEMPORARY) ? 0x04000000 : 0;
    auto cf = k32<HANDLE (WINAPI*)(const WCHAR*, u32, u32, void*, u32, u32, HANDLE)>("CreateFileW");
    HANDLE h = cf(path, access, 3, nullptr, disp, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) { set_errno_from_win32(); return h; }
    extern int c_fmode_value();
    bool text = (oflag & O_TEXT) || (!(oflag & O_BINARY) && !(oflag & (O_WTEXT | O_U8TEXT | O_U16TEXT)) && c_fmode_value() != O_BINARY);
    *fdflags = (text ? FD_TEXT : 0) | ((oflag & O_APPEND) ? FD_APPEND : 0);
    return h;
}
int c_fmode_value() { int* p = nullptr; extern int* fmode_ptr(); p = fmode_ptr(); return p ? *p : O_TEXT; }

static int CDECL c__wopen(const WCHAR* path, int oflag, int pmode) {
    (void)pmode;
    int fl = 0;
    HANDLE h = open_handle(path, oflag, &fl);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int fd = fd_new(h, fl);
    if (fd < 0) k32<BOOL (WINAPI*)(HANDLE)>("CloseHandle")(h);
    return fd;
}
static int CDECL c__open(const char* path, int oflag, int pmode) { WCHAR* w = wide_dup(path); int r = c__wopen(w, oflag, pmode); ifree(w); return r; }
static int CDECL c__sopen(const char* path, int oflag, int sh, int pmode) { (void)sh; return c__open(path, oflag, pmode); }
static int CDECL c__wsopen(const WCHAR* path, int oflag, int sh, int pmode) { (void)sh; return c__wopen(path, oflag, pmode); }
static int CDECL c__sopen_s(int* fd, const char* path, int oflag, int sh, int pmode) { (void)sh; *fd = c__open(path, oflag, pmode); return *fd < 0 ? *errno_ptr() : 0; }
static int CDECL c__wsopen_s(int* fd, const WCHAR* path, int oflag, int sh, int pmode) { (void)sh; *fd = c__wopen(path, oflag, pmode); return *fd < 0 ? *errno_ptr() : 0; }
static int CDECL c__sopen_dispatch(const char* path, int oflag, int sh, int pmode, int* fd, int secure) { (void)secure; return c__sopen_s(fd, path, oflag, sh, pmode); }
static int CDECL c__wsopen_dispatch(const WCHAR* path, int oflag, int sh, int pmode, int* fd, int secure) { (void)secure; return c__wsopen_s(fd, path, oflag, sh, pmode); }
static int CDECL c__creat(const char* path, int pmode) { return c__open(path, O_CREAT | O_TRUNC | O_RDWR, pmode); }
static int CDECL c__wcreat(const WCHAR* path, int pmode) { return c__wopen(path, O_CREAT | O_TRUNC | O_RDWR, pmode); }
static int CDECL c__close(int fd) {
    Fd* f = fd_get(fd);
    if (!f) return -1;
    if (fd > 2) k32<BOOL (WINAPI*)(HANDLE)>("CloseHandle")(f->h);
    Guard g(fd_lock);
    f->used = false;
    return 0;
}
static i64 CDECL c__lseeki64(int fd, i64 off, int whence) {
    Fd* f = fd_get(fd);
    if (!f) return -1;
    if (f->flags & (FD_CONSOLE | FD_PIPE)) { set_errno(ESPIPE); return -1; }
    auto sfp = k32<BOOL (WINAPI*)(HANDLE, LARGE_INTEGER, LARGE_INTEGER*, u32)>("SetFilePointerEx");
    LARGE_INTEGER d, np;
    d.QuadPart = off;
    if (whence == 1 && f->pend >= 0) d.QuadPart--;
    if (!sfp(f->h, d, &np, (u32)whence)) { set_errno_from_win32(); return -1; }
    f->pend = -1;
    f->flags &= ~FD_EOF;
    return np.QuadPart;
}
static i32 CDECL c__lseek(int fd, i32 off, int whence) { i64 r = c__lseeki64(fd, off, whence); return r > 0x7FFFFFFF ? (set_errno(EINVAL), -1) : (i32)r; }
static i64 CDECL c__telli64(int fd) { return c__lseeki64(fd, 0, 1); }
static i32 CDECL c__tell(int fd) { return c__lseek(fd, 0, 1); }
static int CDECL c__commit(int fd) { Fd* f = fd_get(fd); if (!f) return -1; k32<BOOL (WINAPI*)(HANDLE)>("FlushFileBuffers")(f->h); return 0; }
static int CDECL c__dup(int fd) {
    Fd* f = fd_get(fd);
    if (!f) return -1;
    HANDLE h = h_dup(f->h);
    if (!h) { set_errno(EBADF); return -1; }
    return fd_new(h, (int)(f->flags & ~FD_EOF));
}
static int CDECL c__dup2(int a, int b) {
    Fd* f = fd_get(a);
    if (!f || b < 0 || b >= MAX_FD) { set_errno(EBADF); return -1; }
    if (fds[b].used && b > 2) k32<BOOL (WINAPI*)(HANDLE)>("CloseHandle")(fds[b].h);
    HANDLE h = b <= 2 ? f->h : h_dup(f->h);
    fds[b] = {h, f->flags, true, -1};
    if (b <= 2) std_handles[b] = h;
    return 0;
}
static i64 CDECL c__filelengthi64(int fd) {
    Fd* f = fd_get(fd);
    if (!f) return -1;
    LARGE_INTEGER s;
    if (!k32<BOOL (WINAPI*)(HANDLE, LARGE_INTEGER*)>("GetFileSizeEx")(f->h, &s)) { set_errno_from_win32(); return -1; }
    return s.QuadPart;
}
static i32 CDECL c__filelength(int fd) { return (i32)c__filelengthi64(fd); }
static int CDECL c__eof(int fd) {
    i64 pos = c__telli64(fd), len = c__filelengthi64(fd);
    if (pos < 0 || len < 0) return -1;
    return pos >= len;
}
static int CDECL c__chsize_s(int fd, i64 size) {
    i64 old = c__telli64(fd);
    if (c__lseeki64(fd, size, 0) < 0) return *errno_ptr();
    if (!k32<BOOL (WINAPI*)(HANDLE)>("SetEndOfFile")(fd_handle(fd))) { set_errno_from_win32(); return *errno_ptr(); }
    c__lseeki64(fd, old, 0);
    return 0;
}
static int CDECL c__chsize(int fd, i32 size) { return c__chsize_s(fd, size) ? -1 : 0; }
static i64 CDECL c__get_osfhandle(int fd) { Fd* f = fd_get(fd); return f ? (i64)f->h : -1; }
static int CDECL c__open_osfhandle(i64 h, int flags) {
    auto gft = k32<u32 (WINAPI*)(HANDLE)>("GetFileType");
    u32 t = gft((HANDLE)h);
    int fl = ((flags & O_TEXT) || !(flags & O_BINARY) ? FD_TEXT : 0) | ((flags & O_APPEND) ? FD_APPEND : 0) | (t == 2 ? FD_CONSOLE : t == 3 ? FD_PIPE : 0);
    return fd_new((HANDLE)h, fl);
}
static int CDECL c__isatty(int fd) { Fd* f = fd_get(fd); return f && (f->flags & FD_CONSOLE) ? 1 : 0; }
static int CDECL c__setmode(int fd, int mode) {
    Fd* f = fd_get(fd);
    if (!f) return -1;
    int old = (f->flags & FD_TEXT) ? O_TEXT : O_BINARY;
    if (mode & O_BINARY) f->flags &= ~FD_TEXT;
    else if (mode & (O_TEXT | O_WTEXT | O_U8TEXT | O_U16TEXT)) f->flags |= FD_TEXT;
    else { set_errno(EINVAL); return -1; }
    extern void stream_mode_changed(int fd);
    stream_mode_changed(fd);
    return old;
}
static int CDECL c__locking(int fd, int mode, i32 n) { (void)fd; (void)mode; (void)n; return 0; }
static int umask_v = 0;
static int CDECL c__umask(int m) { int o = umask_v; umask_v = m; return o; }
static int CDECL c__umask_s(int m, int* old) { *old = c__umask(m); return 0; }
static int CDECL c__pipe(int* fds_out, u32 size, int mode) {
    HANDLE r, w;
    if (!k32<BOOL (WINAPI*)(HANDLE*, HANDLE*, void*, u32)>("CreatePipe")(&r, &w, nullptr, size)) { set_errno(EMFILE); return -1; }
    int fl = ((mode & O_BINARY) ? 0 : FD_TEXT) | FD_PIPE;
    fds_out[0] = fd_new(r, fl);
    fds_out[1] = fd_new(w, fl);
    return 0;
}

// ---------------------------------------------------------------- streams
struct Priv;
struct CFILE { char* _ptr; int _cnt; char* _base; int _flag; int _file; int _charbuf; int _bufsiz; Priv* p; };
static_assert(sizeof(CFILE) == 48, "msvcrt's FILE is 48 bytes");
enum { M_NONE, M_READ, M_WRITE };
enum { IOFBF = 0, IOLBF = 0x40, IONBF = 4 };
struct Priv {
    int  fd;
    u8*  buf;
    int  cap, pos, len;
    int  mode;
    int  bufmode;
    bool can_read, can_write, text, eof, err, own_buf, used, console;
    int  unget[8];
    int  nunget;
    i64  buf_off;                       // file offset of buf[0] while reading
    volatile u32 owner;
    int  depth;
    char* tmp_path;
    Priv* next;
};
static CFILE iob[20];
static Priv iob_priv[3];
static u8 iob_buf[3][1024];
static Priv* streams = nullptr;
static Lock streams_lock;
static bool streams_ready = false;

static void streams_init() {
    if (streams_ready) return;
    fds_init();
    for (int i = 0; i < 3; i++) {
        Priv* p = &iob_priv[i];
        memset(p, 0, sizeof(*p));
        p->fd = i;
        p->buf = iob_buf[i];
        p->cap = sizeof(iob_buf[i]);
        p->can_read = i == 0;
        p->can_write = i != 0;
        p->text = true;
        p->console = (fds[i].flags & FD_CONSOLE) != 0;
        p->bufmode = i == 2 ? IONBF : p->console ? IOLBF : IOFBF;
        p->used = true;
        iob[i].p = p;
        iob[i]._file = i;
        iob[i]._flag = i == 0 ? 1 : 2;
    }
    iob_priv[2].next = nullptr;
    iob_priv[1].next = &iob_priv[2];
    iob_priv[0].next = &iob_priv[1];
    streams = &iob_priv[0];
    streams_ready = true;
}
static Priv* P(CFILE* f) {
    streams_init();
    if (!f || !f->p) { invalid_parameter(); return nullptr; }
    return f->p;
}
void stream_mode_changed(int fd) {
    streams_init();
    Guard g(streams_lock);
    for (Priv* p = streams; p; p = p->next) if (p->used && p->fd == fd) p->text = (fds[fd].flags & FD_TEXT) != 0;
}

static void lock(Priv* p) {
    u32 me = cur_thr()->id;
    if (p->owner == me) { p->depth++; return; }
    for (u32 s = 0; !__sync_bool_compare_and_swap(&p->owner, 0u, me); s++) nap(s);
    p->depth = 1;
}
static void unlock(Priv* p) { if (--p->depth == 0) { __sync_synchronize(); p->owner = 0; } }
struct FLock { Priv* p; FLock(Priv* p_) : p(p_) { lock(p); } ~FLock() { unlock(p); } };

static bool flush_w(Priv* p) {
    if (p->mode != M_WRITE || !p->pos) return true;
    int k = fd_raw_write(p->fd, p->buf, (u32)p->pos);
    bool ok = k == p->pos;
    p->pos = 0;
    if (!ok) p->err = true;
    return ok;
}
// leave read mode: put the file position back to where the reader is
static void drop_r(Priv* p) {
    if (p->mode != M_READ) return;
    if (!p->console && (p->len - p->pos + p->nunget) > 0) c__lseeki64(p->fd, p->buf_off + p->pos - p->nunget, 0);
    p->pos = p->len = 0;
    p->nunget = 0;
}
static int flush(Priv* p) {
    if (p->mode == M_WRITE) { bool ok = flush_w(p); p->mode = M_NONE; return ok ? 0 : -1; }
    if (p->mode == M_READ) { drop_r(p); p->mode = M_NONE; }
    return 0;
}
void flush_all_streams() {
    if (!streams_ready) return;
    for (Priv* p = streams; p; p = p->next) if (p->used && p->mode == M_WRITE) { lock(p); flush_w(p); unlock(p); }
}

static int raw_get(Priv* p) {
    if (p->nunget) return p->unget[--p->nunget];
    if (p->mode != M_READ) {
        if (!p->can_read) { p->err = true; set_errno(EBADF); return -1; }
        if (p->mode == M_WRITE) flush_w(p);
        p->mode = M_READ;
        p->pos = p->len = 0;
    }
    if (p->pos >= p->len) {
        if (p->eof) return -1;
        if (p->fd == 0) flush_all_streams();                    // the prompt shows before we wait for input
        p->buf_off = p->console ? 0 : c__telli64(p->fd);
        int k = fd_raw_read(p->fd, p->buf, (u32)(p->bufmode == IONBF && !p->console ? 1 : p->cap));
        if (k <= 0) { if (k < 0) p->err = true; else p->eof = true; return -1; }
        p->len = k;
        p->pos = 0;
    }
    return p->buf[p->pos++];
}
static void raw_unget(Priv* p, int c) { if (c >= 0 && p->nunget < 8) p->unget[p->nunget++] = c; }

static int s_getc(Priv* p) {
    int c = raw_get(p);
    if (p->text && !p->console) {
        if (c == '\r') {
            int d = raw_get(p);
            if (d == '\n') return '\n';
            raw_unget(p, d);
        } else if (c == 0x1A) { p->eof = true; p->nunget = 0; return -1; }
    }
    return c;
}

static bool raw_put(Priv* p, u8 c) {
    if (p->mode != M_WRITE) {
        if (!p->can_write) { p->err = true; set_errno(EBADF); return false; }
        drop_r(p);
        p->mode = M_WRITE;
        p->pos = 0;
    }
    p->buf[p->pos++] = c;
    if (p->pos >= p->cap) return flush_w(p);
    return true;
}
static int s_putc(Priv* p, int c) {
    u8 b = (u8)c;
    if (b == '\n' && p->text && !p->console) { if (!raw_put(p, '\r')) return -1; }
    if (!raw_put(p, b)) return -1;
    if (p->bufmode == IONBF || (p->bufmode == IOLBF && b == '\n')) if (!flush_w(p)) return -1;
    return b;
}
static usize s_write(Priv* p, const void* data, usize n) {
    const u8* s = (const u8*)data;
    // big binary writes go straight through
    if (n >= (usize)p->cap && !(p->text && !p->console)) {
        if (p->mode != M_WRITE) { if (!p->can_write) { p->err = true; set_errno(EBADF); return 0; } drop_r(p); p->mode = M_WRITE; p->pos = 0; }
        if (!flush_w(p)) return 0;
        usize done = 0;
        while (done < n) {
            u32 k = (u32)(n - done > 0x40000000 ? 0x40000000 : n - done);
            int w = fd_raw_write(p->fd, s + done, k);
            if (w <= 0) { p->err = true; break; }
            done += (usize)w;
        }
        return done;
    }
    bool line = false;
    for (usize i = 0; i < n; i++) {
        u8 b = s[i];
        if (b == '\n' && p->text && !p->console) { if (!raw_put(p, '\r')) return i; }
        if (!raw_put(p, b)) return i;
        if (b == '\n') line = true;
    }
    if (p->bufmode == IONBF || (p->bufmode == IOLBF && line)) flush_w(p);
    return n;
}
static usize s_read(Priv* p, void* data, usize n) {
    u8* d = (u8*)data;
    usize got = 0;
    while (got < n) {
        if (!p->text || p->console) {
            // take what is buffered in one go
            if (!p->nunget && p->mode == M_READ && p->pos < p->len) {
                usize k = (usize)(p->len - p->pos);
                if (k > n - got) k = n - got;
                memcpy(d + got, p->buf + p->pos, k);
                p->pos += (int)k;
                got += k;
                continue;
            }
        }
        int c = s_getc(p);
        if (c < 0) break;
        d[got++] = (u8)c;
        if (p->console && c == '\n' && got < n && p->pos >= p->len) break;    // a typed line is all there is for now
    }
    return got;
}

static CFILE* new_stream(int fd, bool r, bool w, bool text) {
    streams_init();
    CFILE* f = (CFILE*)izalloc(sizeof(CFILE) + sizeof(Priv));
    if (!f) { set_errno(ENOMEM); return nullptr; }
    Priv* p = (Priv*)(f + 1);
    p->fd = fd;
    p->cap = 4096;
    p->buf = (u8*)h_alloc((usize)p->cap);
    p->own_buf = true;
    p->can_read = r;
    p->can_write = w;
    p->text = text;
    p->console = (fds[fd].flags & FD_CONSOLE) != 0;
    p->bufmode = p->console ? IOLBF : IOFBF;
    p->used = true;
    f->p = p;
    f->_file = fd;
    f->_flag = (r ? 1 : 0) | (w ? 2 : 0);
    Guard g(streams_lock);
    p->next = streams;
    streams = p;
    return f;
}

static bool parse_mode(const char* m, int* oflag, bool* r, bool* w) {
    int fl = 0;
    switch (*m) {
    case 'r': fl = O_RDONLY; break;
    case 'w': fl = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': fl = O_WRONLY | O_CREAT | O_APPEND; break;
    default: return false;
    }
    bool plus = false;
    for (const char* p = m + 1; *p && *p != ','; p++) {
        if (*p == '+') plus = true;
        else if (*p == 'b') fl |= O_BINARY;
        else if (*p == 't') fl |= O_TEXT;
        else if (*p == 'x') fl |= O_EXCL;
        else if (*p == 'D') fl |= O_TEMPORARY;
    }
    if (sfind(m, "ccs=")) fl |= O_BINARY;                       // encoded text: no CR/LF translation (bytes are UTF-8 here)
    if (plus) fl = (fl & ~3) | O_RDWR;
    *r = (fl & 3) != O_WRONLY;
    *w = (fl & 3) != O_RDONLY;
    *oflag = fl;
    return true;
}

static CFILE* open_stream(const WCHAR* path, const char* mode) {
    int fl;
    bool r, w;
    if (!path || !mode || !parse_mode(mode, &fl, &r, &w)) { set_errno(EINVAL); return nullptr; }
    int fd = c__wopen(path, fl, 0666);
    if (fd < 0) return nullptr;
    CFILE* f = new_stream(fd, r, w, (fds[fd].flags & FD_TEXT) != 0);
    if (!f) c__close(fd);
    return f;
}
static CFILE* CDECL c__wfopen(const WCHAR* path, const WCHAR* mode) { char m[16]; utf8_from_wide(mode, -1, m, sizeof(m)); return open_stream(path, m); }
static CFILE* CDECL c_fopen(const char* path, const char* mode) {
    if (!path) { set_errno(EINVAL); return nullptr; }
    WCHAR* w = wide_dup(path);
    CFILE* f = open_stream(w, mode);
    ifree(w);
    return f;
}
static int CDECL c_fopen_s(CFILE** out, const char* path, const char* mode) { *out = c_fopen(path, mode); return *out ? 0 : *errno_ptr(); }
static int CDECL c__wfopen_s(CFILE** out, const WCHAR* path, const WCHAR* mode) { *out = c__wfopen(path, mode); return *out ? 0 : *errno_ptr(); }
static CFILE* CDECL c__fsopen(const char* path, const char* mode, int sh) { (void)sh; return c_fopen(path, mode); }
static CFILE* CDECL c__wfsopen(const WCHAR* path, const WCHAR* mode, int sh) { (void)sh; return c__wfopen(path, mode); }
static CFILE* CDECL c__fdopen(int fd, const char* mode) {
    int fl; bool r, w;
    if (!fd_get(fd) || !parse_mode(mode, &fl, &r, &w)) return nullptr;
    bool text = (fl & O_BINARY) ? false : (fl & O_TEXT) ? true : (fds[fd].flags & FD_TEXT) != 0;
    return new_stream(fd, r, w, text);
}
static CFILE* CDECL c__wfdopen(int fd, const WCHAR* mode) { char m[16]; utf8_from_wide(mode, -1, m, sizeof(m)); return c__fdopen(fd, m); }

static int close_stream(CFILE* f, bool keep_struct) {
    Priv* p = P(f);
    if (!p || !p->used) return -1;
    lock(p);
    int r = flush(p);
    if (p->fd > 2 || f < iob || f >= iob + 20) { if (c__close(p->fd) < 0) r = -1; }
    if (p->tmp_path) { auto df = k32<BOOL (WINAPI*)(const char*)>("DeleteFileA"); df(p->tmp_path); ifree(p->tmp_path); p->tmp_path = nullptr; }
    p->used = false;
    unlock(p);
    if (!keep_struct && (f < iob || f >= iob + 20)) {
        Guard g(streams_lock);
        for (Priv** q = &streams; *q; q = &(*q)->next) if (*q == p) { *q = p->next; break; }
        if (p->own_buf) h_free(p->buf);
        ifree(f);
    }
    return r;
}
static int CDECL c_fclose(CFILE* f) { return close_stream(f, false); }
static int CDECL c__fcloseall() {
    int n = 0;
    for (;;) {
        Priv* p = nullptr;
        { Guard g(streams_lock); for (Priv* q = streams; q; q = q->next) if (q->used && q->fd > 2) { p = q; break; } }
        if (!p) break;
        c_fclose((CFILE*)p - 1);
        n++;
    }
    return n;
}
static CFILE* CDECL c_freopen(const char* path, const char* mode, CFILE* f) {
    Priv* p = P(f);
    if (!p) return nullptr;
    lock(p);
    flush(p);
    if (p->fd > 2) c__close(p->fd);
    int fl; bool r, w;
    if (!path || !parse_mode(mode, &fl, &r, &w)) { p->used = false; unlock(p); set_errno(EINVAL); return nullptr; }
    WCHAR* wp = wide_dup(path);
    int fd = c__wopen(wp, fl, 0666);
    ifree(wp);
    if (fd < 0) { p->used = false; unlock(p); return nullptr; }
    if (f >= iob && f < iob + 3) { c__dup2(fd, (int)(f - iob)); c__close(fd); fd = (int)(f - iob); }
    p->fd = fd; p->can_read = r; p->can_write = w; p->text = (fds[fd].flags & FD_TEXT) != 0;
    p->console = (fds[fd].flags & FD_CONSOLE) != 0;
    p->eof = p->err = false; p->nunget = 0; p->pos = p->len = 0; p->mode = M_NONE; p->used = true;
    f->_file = fd;
    unlock(p);
    return f;
}
static CFILE* CDECL c__wfreopen(const WCHAR* path, const WCHAR* mode, CFILE* f) { char a[MAX_PATH * 3], m[16]; utf8_from_wide(path, -1, a, sizeof(a)); utf8_from_wide(mode, -1, m, sizeof(m)); return c_freopen(a, m, f); }
static int CDECL c_freopen_s(CFILE** out, const char* path, const char* mode, CFILE* f) { *out = c_freopen(path, mode, f); return *out ? 0 : *errno_ptr(); }
static int CDECL c__wfreopen_s(CFILE** out, const WCHAR* path, const WCHAR* mode, CFILE* f) { *out = c__wfreopen(path, mode, f); return *out ? 0 : *errno_ptr(); }

static int CDECL c_fflush(CFILE* f) {
    if (!f) { flush_all_streams(); return 0; }
    Priv* p = P(f);
    if (!p) return -1;
    FLock l(p);
    if (p->mode == M_WRITE) return flush_w(p) ? 0 : -1;
    return 0;
}
static int CDECL c__flushall() { flush_all_streams(); return 0; }
static int CDECL c_fgetc(CFILE* f) { Priv* p = P(f); if (!p) return -1; FLock l(p); return s_getc(p); }
static int CDECL c_fputc(int c, CFILE* f) { Priv* p = P(f); if (!p) return -1; FLock l(p); return s_putc(p, c); }
static int CDECL c_getchar() { return c_fgetc(&iob[0]); }
static int CDECL c_putchar(int c) { return c_fputc(c, &iob[1]); }
static int CDECL c_ungetc(int c, CFILE* f) {
    Priv* p = P(f);
    if (!p || c < 0) return -1;
    FLock l(p);
    if (p->mode == M_WRITE) flush(p);
    if (p->mode != M_READ) { p->mode = M_READ; p->pos = p->len = 0; p->buf_off = p->console ? 0 : c__telli64(p->fd); }
    if (p->nunget >= 8) return -1;
    p->unget[p->nunget++] = (u8)c;
    p->eof = false;
    return (u8)c;
}
static char* CDECL c_fgets(char* s, int n, CFILE* f) {
    Priv* p = P(f);
    if (!p || n <= 0) return nullptr;
    FLock l(p);
    int i = 0;
    while (i < n - 1) {
        int c = s_getc(p);
        if (c < 0) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i) return nullptr;
    s[i] = 0;
    return s;
}
static int CDECL c_fputs(const char* s, CFILE* f) { Priv* p = P(f); if (!p) return -1; FLock l(p); usize n = slen(s); return s_write(p, s, n) == n ? 0 : -1; }
static int CDECL c_puts(const char* s) {
    Priv* p = P(&iob[1]);
    FLock l(p);
    usize n = slen(s);
    if (s_write(p, s, n) != n || s_putc(p, '\n') < 0) return -1;
    return 0;
}
static char* CDECL c_gets_s(char* s, usize n) {
    Priv* p = P(&iob[0]);
    FLock l(p);
    usize i = 0;
    int c;
    while ((c = s_getc(p)) >= 0 && c != '\n') if (i + 1 < n) s[i++] = (char)c;
    if (c < 0 && !i) return nullptr;
    s[i] = 0;
    return s;
}
static char* CDECL c_gets(char* s) { return c_gets_s(s, 0x7FFFFFFF); }
static usize CDECL c_fread(void* buf, usize size, usize n, CFILE* f) {
    Priv* p = P(f);
    if (!p || !size || !n) return 0;
    FLock l(p);
    return s_read(p, buf, size * n) / size;
}
static usize CDECL c_fread_s(void* buf, usize cap, usize size, usize n, CFILE* f) { if (size && n > cap / size) n = cap / size; return c_fread(buf, size, n, f); }
static usize CDECL c_fwrite(const void* buf, usize size, usize n, CFILE* f) {
    Priv* p = P(f);
    if (!p || !size || !n) return 0;
    FLock l(p);
    return s_write(p, buf, size * n) / size;
}
static int CDECL c__fseeki64(CFILE* f, i64 off, int whence) {
    Priv* p = P(f);
    if (!p) return -1;
    FLock l(p);
    if (p->console) { set_errno(ESPIPE); return -1; }
    if (whence == 1 && p->mode == M_READ) { off -= (p->len - p->pos) + p->nunget; }
    if (p->mode == M_WRITE) flush_w(p);
    p->mode = M_NONE;
    p->pos = p->len = 0;
    p->nunget = 0;
    if (c__lseeki64(p->fd, off, whence) < 0) return -1;
    p->eof = false;
    return 0;
}
static int CDECL c_fseek(CFILE* f, i32 off, int whence) { return c__fseeki64(f, off, whence); }
static i64 CDECL c__ftelli64(CFILE* f) {
    Priv* p = P(f);
    if (!p) return -1;
    FLock l(p);
    if (p->console) { set_errno(ESPIPE); return -1; }
    if (p->mode == M_READ) return p->buf_off + p->pos - p->nunget;
    i64 pos = c__telli64(p->fd);
    if (pos < 0) return -1;
    if (p->mode == M_WRITE) {
        if (fds[p->fd].flags & FD_APPEND) { flush_w(p); return c__telli64(p->fd); }
        pos += p->pos;
    }
    return pos;
}
static i32 CDECL c_ftell(CFILE* f) { i64 r = c__ftelli64(f); return r > 0x7FFFFFFF ? -1 : (i32)r; }
static void CDECL c_rewind(CFILE* f) { c__fseeki64(f, 0, 0); Priv* p = P(f); if (p) p->err = false; }
static int CDECL c_fgetpos(CFILE* f, i64* pos) { *pos = c__ftelli64(f); return *pos < 0 ? -1 : 0; }
static int CDECL c_fsetpos(CFILE* f, const i64* pos) { return c__fseeki64(f, *pos, 0); }
static int CDECL c_feof(CFILE* f) { Priv* p = P(f); return p && p->eof && !p->nunget ? 0x10 : 0; }
static int CDECL c_ferror(CFILE* f) { Priv* p = P(f); return p && p->err ? 0x20 : 0; }
static void CDECL c_clearerr(CFILE* f) { Priv* p = P(f); if (p) { p->eof = false; p->err = false; } }
static int CDECL c_clearerr_s(CFILE* f) { c_clearerr(f); return 0; }
static int CDECL c__fileno(CFILE* f) { Priv* p = P(f); return p ? p->fd : -1; }
static int CDECL c_setvbuf(CFILE* f, char* buf, int mode, usize size) {
    Priv* p = P(f);
    if (!p || (mode != IOFBF && mode != IOLBF && mode != IONBF)) { set_errno(EINVAL); return -1; }
    FLock l(p);
    flush(p);
    p->bufmode = mode;
    if (mode != IONBF && size >= 2) {
        if (p->own_buf && p->buf && (p < iob_priv || p >= iob_priv + 3)) h_free(p->buf);
        if (buf) { p->buf = (u8*)buf; p->own_buf = false; }
        else { p->buf = (u8*)h_alloc(size); p->own_buf = true; }
        p->cap = (int)(size > 0x7FFFFFFF ? 0x7FFFFFFF : size);
    }
    return 0;
}
static void CDECL c_setbuf(CFILE* f, char* buf) { c_setvbuf(f, buf, buf ? IOFBF : IONBF, 512); }
static CFILE* CDECL c___acrt_iob_func(u32 i) { streams_init(); return &iob[i < 20 ? i : 0]; }
static CFILE* CDECL c___iob_func() { streams_init(); return iob; }
static void CDECL c__lock_file(CFILE* f) { Priv* p = P(f); if (p) lock(p); }
static void CDECL c__unlock_file(CFILE* f) { Priv* p = P(f); if (p) unlock(p); }
static int CDECL c__getw(CFILE* f) { int v; return c_fread(&v, 4, 1, f) == 1 ? v : -1; }
static int CDECL c__putw(int v, CFILE* f) { return c_fwrite(&v, 4, 1, f) == 1 ? v : -1; }
static int CDECL c__fgetchar() { return c_getchar(); }
static int CDECL c__fputchar(int c) { return c_putchar(c); }
static int CDECL c__get_stream_buffer_pointers(CFILE* f, char*** base, char*** ptr, int** cnt) {
    (void)f;
    static char* dummy_p = nullptr;
    static int dummy_c = 0;
    if (base) *base = &dummy_p;
    if (ptr) *ptr = &dummy_p;
    if (cnt) *cnt = &dummy_c;
    return 0;
}

static CFILE* CDECL c_tmpfile() {
    char path[64];
    static u32 n = 0;
    si::make_dir("/home/tmp");
    snfmt(path, sizeof(path), "C:\\home\\tmp\\t%x%x.tmp", (u32)si::ticks_us(), ++n);
    CFILE* f = c_fopen(path, "w+bD");
    return f;
}
static int CDECL c_tmpfile_s(CFILE** out) { *out = c_tmpfile(); return *out ? 0 : *errno_ptr(); }
static char* CDECL c_tmpnam(char* buf) {
    static u32 n = 0;
    char* out = buf ? buf : crt_thread()->tmpnam_buf;
    si::make_dir("/home/tmp");
    snfmt(out, MAX_PATH, "C:\\home\\tmp\\s%x%x.", (u32)si::ticks_us(), ++n);
    return out;
}
static int CDECL c_tmpnam_s(char* buf, usize n) { char b[MAX_PATH]; c_tmpnam(b); if (slen(b) >= n) return ERANGE; scpy(buf, b); return 0; }
static WCHAR* CDECL c__wtmpnam(WCHAR* buf) { static WCHAR wb[MAX_PATH]; char b[MAX_PATH]; c_tmpnam(b); WCHAR* o = buf ? buf : wb; wide_from_utf8(b, -1, o, MAX_PATH); return o; }
static char* CDECL c__tempnam(const char* dir, const char* prefix) {
    static u32 n = 0;
    char* out = (char*)h_alloc(MAX_PATH);
    si::make_dir("/home/tmp");
    snfmt(out, MAX_PATH, "%s\\%s%x%x", dir && *dir ? dir : "C:\\home\\tmp", prefix ? prefix : "", (u32)si::ticks_us(), ++n);
    return out;
}
static char* CDECL c__mktemp(char* t) {
    usize n = slen(t);
    static u32 seq = 0;
    if (n < 6 || scmp(t + n - 6, "XXXXXX")) { set_errno(EINVAL); return nullptr; }
    u32 v = (u32)si::ticks_us() + ++seq;
    for (int i = 0; i < 6; i++) { t[n - 6 + i] = (char)('a' + v % 26); v /= 26; }
    return t;
}
static int CDECL c__mktemp_s(char* t, usize n) { (void)n; return c__mktemp(t) ? 0 : EINVAL; }

// ---------------------------------------------------------------- wide streams (UTF-8 underneath)
static int put_wide(Priv* p, u32 c) {
    char b[4];
    int k;
    if (c < 0x80) { b[0] = (char)c; k = 1; }
    else if (c < 0x800) { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 63)); k = 2; }
    else if (c < 0x10000) { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 63)); b[2] = (char)(0x80 | (c & 63)); k = 3; }
    else { b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 63)); b[2] = (char)(0x80 | ((c >> 6) & 63)); b[3] = (char)(0x80 | (c & 63)); k = 4; }
    for (int i = 0; i < k; i++) if (s_putc(p, (u8)b[i]) < 0) return -1;
    return 0;
}
static u32 pending_low[1];                                       // the second half of a surrogate pair waiting to be read
static int get_wide(Priv* p) {
    int c = s_getc(p);
    if (c < 0x80) return c;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    u32 v = (u32)c & (0x3F >> more);
    for (int i = 0; i < more; i++) {
        int d = s_getc(p);
        if (d < 0 || (d & 0xC0) != 0x80) { if (d >= 0) raw_unget(p, d); return 0xFFFD; }
        v = (v << 6) | (u32)(d & 63);
    }
    if (v >= 0x10000) { v -= 0x10000; pending_low[0] = 0xDC00 + (v & 0x3FF); return (int)(0xD800 + (v >> 10)); }
    return (int)v;
}
static WCHAR high_pending = 0;
static u16 CDECL c_fputwc(WCHAR c, CFILE* f) {
    Priv* p = P(f);
    if (!p) return 0xFFFF;
    FLock l(p);
    u32 v = (u16)c;
    if (v >= 0xD800 && v < 0xDC00) { high_pending = c; return (u16)c; }
    if (v >= 0xDC00 && v < 0xE000 && high_pending) { v = 0x10000 + (((u32)(u16)high_pending - 0xD800) << 10) + (v - 0xDC00); high_pending = 0; }
    return put_wide(p, v) < 0 ? 0xFFFF : (u16)c;
}
static u16 CDECL c_fgetwc(CFILE* f) {
    Priv* p = P(f);
    if (!p) return 0xFFFF;
    FLock l(p);
    if (pending_low[0]) { u16 v = (u16)pending_low[0]; pending_low[0] = 0; return v; }
    int c = get_wide(p);
    return c < 0 ? 0xFFFF : (u16)c;
}
static u16 CDECL c_ungetwc(u16 c, CFILE* f) {
    if (c == 0xFFFF) return c;
    char b[4];
    int k = 0;
    if (c < 0x80) b[k++] = (char)c;
    else if (c < 0x800) { b[k++] = (char)(0xC0 | (c >> 6)); b[k++] = (char)(0x80 | (c & 63)); }
    else { b[k++] = (char)(0xE0 | (c >> 12)); b[k++] = (char)(0x80 | ((c >> 6) & 63)); b[k++] = (char)(0x80 | (c & 63)); }
    for (int i = k - 1; i >= 0; i--) c_ungetc((u8)b[i], f);
    return c;
}
static u16 CDECL c_getwchar() { return c_fgetwc(&iob[0]); }
static u16 CDECL c_putwchar(WCHAR c) { return c_fputwc(c, &iob[1]); }
static int CDECL c_fputws(const WCHAR* s, CFILE* f) { for (; *s; s++) if (c_fputwc(*s, f) == 0xFFFF) return -1; return 0; }
static int CDECL c__putws(const WCHAR* s) { if (c_fputws(s, &iob[1]) < 0) return -1; return c_fputwc('\n', &iob[1]) == 0xFFFF ? -1 : 0; }
static WCHAR* CDECL c_fgetws(WCHAR* s, int n, CFILE* f) {
    int i = 0;
    while (i < n - 1) {
        u16 c = c_fgetwc(f);
        if (c == 0xFFFF) break;
        s[i++] = (WCHAR)c;
        if (c == '\n') break;
    }
    if (!i) return nullptr;
    s[i] = 0;
    return s;
}
static WCHAR* CDECL c__getws_s(WCHAR* s, usize n) {
    usize i = 0;
    u16 c;
    while ((c = c_fgetwc(&iob[0])) != 0xFFFF && c != '\n') if (i + 1 < n) s[i++] = (WCHAR)c;
    if (c == 0xFFFF && !i) return nullptr;
    s[i] = 0;
    return s;
}
static int CDECL c_fwide(CFILE* f, int mode) { (void)f; return mode; }
static u16 CDECL c__fgetwchar() { return c_getwchar(); }
static u16 CDECL c__fputwchar(WCHAR c) { return c_putwchar(c); }

// ---------------------------------------------------------------- printf: the sinks and every entry point
struct FileSink : Sink { Priv* p; };
static void file_put(Sink* s, const char* d, usize n) { s_write(((FileSink*)s)->p, d, n); }
struct BufSink : Sink { char* buf; usize cap, len; };
static void buf_put(Sink* s, const char* d, usize n) {
    BufSink* b = (BufSink*)s;
    for (usize i = 0; i < n; i++) { if (b->len < b->cap) b->buf[b->len] = d[i]; b->len++; }
}

static int print_file(CFILE* f, const char* fmt, va_list ap, int fl) {
    Priv* p = P(f);
    if (!p || !fmt) { set_errno(EINVAL); return -1; }
    FLock l(p);
    FileSink s;
    s.put = file_put; s.total = 0; s.p = p;
    bool err0 = p->err;
    int n = vformat(&s, fmt, ap, fl);
    return p->err && !err0 ? -1 : n;
}
// into a buffer: n chars of output, the whole length returned; the caller decides about truncation
static int print_buf(char* buf, usize cap, const char* fmt, va_list ap, int fl, usize* written) {
    BufSink s;
    s.put = buf_put; s.total = 0; s.buf = buf; s.cap = cap; s.len = 0;
    int n = vformat(&s, fmt, ap, fl);
    *written = s.len < cap ? s.len : cap;
    return n;
}

// wide formats are turned into UTF-8, formatted, and the result turned back
static char* fmt_from_wide(const WCHAR* f) { return utf8_dup(f); }
static int print_wbuf(WCHAR* buf, usize cap, const WCHAR* wfmt, va_list ap, int fl) {     // returns the length in WCHARs
    char* fmt = fmt_from_wide(wfmt);
    BufSink s;
    s.put = buf_put; s.total = 0; s.buf = nullptr; s.cap = 0; s.len = 0;
    va_list c1;
    va_copy(c1, ap);
    int n = vformat(&s, fmt, c1, fl);
    va_end(c1);
    char* tmp = (char*)h_alloc((usize)n + 1);
    usize w;
    print_buf(tmp, (usize)n + 1, fmt, ap, fl, &w);
    tmp[n] = 0;
    int wl = wide_from_utf8(tmp, n, nullptr, 0);
    if (buf && cap) {
        WCHAR* wt = (WCHAR*)h_alloc(((usize)wl + 1) * 2);
        wide_from_utf8(tmp, n, wt, wl);
        usize k = (usize)wl < cap ? (usize)wl : cap;
        memcpy(buf, wt, k * 2);
        h_free(wt);
    }
    h_free(tmp);
    ifree(fmt);
    return wl;
}

// UCRT's option bits
enum { UO_LEGACY_VSPRINTF_NULL = 1, UO_STANDARD_SNPRINTF = 2, UO_LEGACY_WIDE = 4, UO_LEGACY_MSVCRT = 8, UO_THREE_DIGIT = 16 };
static int uflags(u64 opt, bool wide) { return ((opt & UO_LEGACY_WIDE) && wide ? FMT_LEGACY_WIDE : 0) | ((opt & UO_THREE_DIGIT) ? FMT_THREE_DIGIT_EXP : 0); }

static int CDECL c___stdio_common_vfprintf(u64 opt, CFILE* f, const char* fmt, void* loc, va_list ap) { (void)loc; return print_file(f, fmt, ap, uflags(opt, false)); }
static int CDECL c___stdio_common_vfprintf_s(u64 opt, CFILE* f, const char* fmt, void* loc, va_list ap) { (void)loc; return print_file(f, fmt, ap, uflags(opt, false)); }
static int CDECL c___stdio_common_vfprintf_p(u64 opt, CFILE* f, const char* fmt, void* loc, va_list ap) { (void)loc; return print_file(f, fmt, ap, uflags(opt, false)); }
static int CDECL c___stdio_common_vsprintf(u64 opt, char* buf, usize count, const char* fmt, void* loc, va_list ap) {
    (void)loc;
    if (!fmt) { set_errno(EINVAL); return -1; }
    usize w;
    int n = print_buf(buf, buf ? count : 0, fmt, ap, uflags(opt, false), &w);
    if (!buf || !count) return n;
    if ((usize)n < count) { buf[n] = 0; return n; }
    if (opt & UO_STANDARD_SNPRINTF) { buf[count - 1] = 0; return n; }
    if ((opt & UO_LEGACY_VSPRINTF_NULL) && (usize)n == count) return n;    // _vsnprintf: exactly full, no terminator
    buf[count - 1] = 0;
    return (usize)n == count ? n : -1;
}
static int CDECL c___stdio_common_vsprintf_s(u64 opt, char* buf, usize count, const char* fmt, void* loc, va_list ap) {
    (void)loc;
    if (!buf || !count || !fmt) { invalid_parameter(); set_errno(EINVAL); return -1; }
    usize w;
    int n = print_buf(buf, count, fmt, ap, uflags(opt, false), &w);
    if ((usize)n >= count) { buf[0] = 0; invalid_parameter(); set_errno(ERANGE); return -1; }
    buf[n] = 0;
    return n;
}
static int CDECL c___stdio_common_vsnprintf_s(u64 opt, char* buf, usize size, usize maxcount, const char* fmt, void* loc, va_list ap) {
    (void)loc;
    if (!buf || !size || !fmt) { invalid_parameter(); set_errno(EINVAL); return -1; }
    usize limit = maxcount == (usize)-1 ? size - 1 : (maxcount < size - 1 ? maxcount : size - 1);
    usize w;
    int n = print_buf(buf, limit, fmt, ap, uflags(opt, false), &w);
    if ((usize)n <= limit) { buf[n] = 0; return n; }
    buf[limit] = 0;
    if (maxcount == (usize)-1 || maxcount < size) return -1;    // truncated as asked
    buf[0] = 0;
    invalid_parameter();
    set_errno(ERANGE);
    return -1;
}
static int CDECL c___stdio_common_vsprintf_p(u64 opt, char* buf, usize count, const char* fmt, void* loc, va_list ap) { return c___stdio_common_vsprintf(opt | UO_STANDARD_SNPRINTF, buf, count, fmt, loc, ap); }

static int print_wfile(CFILE* f, const WCHAR* wfmt, va_list ap, int fl) {
    char* fmt = fmt_from_wide(wfmt);
    int r = print_file(f, fmt, ap, fl);
    ifree(fmt);
    if (r <= 0) return r;
    return r;                                                    // characters written (bytes for non-ASCII; close enough)
}
static int CDECL c___stdio_common_vfwprintf(u64 opt, CFILE* f, const WCHAR* fmt, void* loc, va_list ap) { (void)loc; return print_wfile(f, fmt, ap, uflags(opt, true)); }
static int CDECL c___stdio_common_vfwprintf_s(u64 opt, CFILE* f, const WCHAR* fmt, void* loc, va_list ap) { (void)loc; return print_wfile(f, fmt, ap, uflags(opt, true)); }
static int CDECL c___stdio_common_vfwprintf_p(u64 opt, CFILE* f, const WCHAR* fmt, void* loc, va_list ap) { (void)loc; return print_wfile(f, fmt, ap, uflags(opt, true)); }
static int CDECL c___stdio_common_vswprintf(u64 opt, WCHAR* buf, usize count, const WCHAR* fmt, void* loc, va_list ap) {
    (void)loc;
    if (!fmt) { set_errno(EINVAL); return -1; }
    int n = print_wbuf(buf, buf ? count : 0, fmt, ap, uflags(opt, true));
    if (!buf || !count) return n;
    if ((usize)n < count) { buf[n] = 0; return n; }
    if (opt & UO_STANDARD_SNPRINTF) { buf[count - 1] = 0; return -1; }      // swprintf returns -1 when it doesn't fit
    if ((opt & UO_LEGACY_VSPRINTF_NULL) && (usize)n == count) return n;
    buf[count - 1] = 0;
    return -1;
}
static int CDECL c___stdio_common_vswprintf_s(u64 opt, WCHAR* buf, usize count, const WCHAR* fmt, void* loc, va_list ap) {
    (void)loc;
    if (!buf || !count || !fmt) { invalid_parameter(); set_errno(EINVAL); return -1; }
    int n = print_wbuf(buf, count, fmt, ap, uflags(opt, true));
    if ((usize)n >= count) { buf[0] = 0; invalid_parameter(); set_errno(ERANGE); return -1; }
    buf[n] = 0;
    return n;
}
static int CDECL c___stdio_common_vsnwprintf_s(u64 opt, WCHAR* buf, usize size, usize maxcount, const WCHAR* fmt, void* loc, va_list ap) {
    (void)loc;
    if (!buf || !size || !fmt) { invalid_parameter(); set_errno(EINVAL); return -1; }
    usize limit = maxcount == (usize)-1 ? size - 1 : (maxcount < size - 1 ? maxcount : size - 1);
    int n = print_wbuf(buf, limit, fmt, ap, uflags(opt, true));
    if ((usize)n <= limit) { buf[n] = 0; return n; }
    buf[limit] = 0;
    return -1;
}
static int CDECL c___stdio_common_vswprintf_p(u64 opt, WCHAR* buf, usize count, const WCHAR* fmt, void* loc, va_list ap) { return c___stdio_common_vswprintf(opt, buf, count, fmt, loc, ap); }

// the classic entry points (msvcrt.dll exports these directly; mingw's UCRT headers inline most of them)
static int MS = FMT_THREE_DIGIT_EXP;                             // msvcrt prints exponents with three digits
static int CDECL c_vfprintf(CFILE* f, const char* fmt, va_list ap) { return print_file(f, fmt, ap, 0); }
static int CDECL c_vprintf(const char* fmt, va_list ap) { return print_file(&iob[1], fmt, ap, 0); }
static int CDECL c_fprintf(CFILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = print_file(f, fmt, ap, 0); va_end(ap); return r; }
static int CDECL c_printf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = print_file(&iob[1], fmt, ap, 0); va_end(ap); return r; }
static int CDECL c_vsnprintf(char* buf, usize n, const char* fmt, va_list ap) { return c___stdio_common_vsprintf(UO_STANDARD_SNPRINTF, buf, n, fmt, nullptr, ap); }
static int CDECL c__vsnprintf(char* buf, usize n, const char* fmt, va_list ap) { return c___stdio_common_vsprintf(UO_LEGACY_VSPRINTF_NULL, buf, n, fmt, nullptr, ap); }
static int CDECL c_vsprintf(char* buf, const char* fmt, va_list ap) { usize w; int n = print_buf(buf, 0x7FFFFFFF, fmt, ap, 0, &w); buf[n] = 0; return n; }
static int CDECL c_snprintf(char* buf, usize n, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c_vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
static int CDECL c__snprintf(char* buf, usize n, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c__vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
static int CDECL c_sprintf(char* buf, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c_vsprintf(buf, fmt, ap); va_end(ap); return r; }
static int CDECL c__vscprintf(const char* fmt, va_list ap) { usize w; return print_buf(nullptr, 0, fmt, ap, 0, &w); }
static int CDECL c__scprintf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c__vscprintf(fmt, ap); va_end(ap); return r; }
static int CDECL c_vsprintf_s(char* buf, usize n, const char* fmt, va_list ap) { return c___stdio_common_vsprintf_s(0, buf, n, fmt, nullptr, ap); }
static int CDECL c_sprintf_s(char* buf, usize n, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c_vsprintf_s(buf, n, fmt, ap); va_end(ap); return r; }
static int CDECL c__vsnprintf_s(char* buf, usize n, usize max, const char* fmt, va_list ap) { return c___stdio_common_vsnprintf_s(0, buf, n, max, fmt, nullptr, ap); }
static int CDECL c__snprintf_s(char* buf, usize n, usize max, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c__vsnprintf_s(buf, n, max, fmt, ap); va_end(ap); return r; }
static int CDECL c_vfprintf_s(CFILE* f, const char* fmt, va_list ap) { return print_file(f, fmt, ap, 0); }
static int CDECL c_vprintf_s(const char* fmt, va_list ap) { return print_file(&iob[1], fmt, ap, 0); }
static int CDECL c_fprintf_s(CFILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = print_file(f, fmt, ap, 0); va_end(ap); return r; }
static int CDECL c_printf_s(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = print_file(&iob[1], fmt, ap, 0); va_end(ap); return r; }
static int CDECL c_vfwprintf(CFILE* f, const WCHAR* fmt, va_list ap) { return print_wfile(f, fmt, ap, FMT_LEGACY_WIDE); }
static int CDECL c_vwprintf(const WCHAR* fmt, va_list ap) { return print_wfile(&iob[1], fmt, ap, FMT_LEGACY_WIDE); }
static int CDECL c_fwprintf(CFILE* f, const WCHAR* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c_vfwprintf(f, fmt, ap); va_end(ap); return r; }
static int CDECL c_wprintf(const WCHAR* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c_vwprintf(fmt, ap); va_end(ap); return r; }
static int CDECL c__vsnwprintf(WCHAR* buf, usize n, const WCHAR* fmt, va_list ap) { return c___stdio_common_vswprintf(UO_LEGACY_WIDE | UO_LEGACY_VSPRINTF_NULL, buf, n, fmt, nullptr, ap); }
static int CDECL c__snwprintf(WCHAR* buf, usize n, const WCHAR* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c__vsnwprintf(buf, n, fmt, ap); va_end(ap); return r; }
static int CDECL c_vswprintf(WCHAR* buf, usize n, const WCHAR* fmt, va_list ap) { return c___stdio_common_vswprintf(UO_LEGACY_WIDE | UO_STANDARD_SNPRINTF, buf, n, fmt, nullptr, ap); }
static int CDECL c_swprintf(WCHAR* buf, usize n, const WCHAR* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c_vswprintf(buf, n, fmt, ap); va_end(ap); return r; }
static int CDECL c__vswprintf(WCHAR* buf, const WCHAR* fmt, va_list ap) { int n = print_wbuf(buf, 0x7FFFFFF, fmt, ap, FMT_LEGACY_WIDE); buf[n] = 0; return n; }
static int CDECL c__swprintf(WCHAR* buf, const WCHAR* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c__vswprintf(buf, fmt, ap); va_end(ap); return r; }
static int CDECL c_vswprintf_s(WCHAR* buf, usize n, const WCHAR* fmt, va_list ap) { return c___stdio_common_vswprintf_s(UO_LEGACY_WIDE, buf, n, fmt, nullptr, ap); }
static int CDECL c_swprintf_s(WCHAR* buf, usize n, const WCHAR* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c_vswprintf_s(buf, n, fmt, ap); va_end(ap); return r; }
static int CDECL c__vsnwprintf_s(WCHAR* buf, usize n, usize max, const WCHAR* fmt, va_list ap) { return c___stdio_common_vsnwprintf_s(UO_LEGACY_WIDE, buf, n, max, fmt, nullptr, ap); }
static int CDECL c__snwprintf_s(WCHAR* buf, usize n, usize max, const WCHAR* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c__vsnwprintf_s(buf, n, max, fmt, ap); va_end(ap); return r; }
static int CDECL c__vscwprintf(const WCHAR* fmt, va_list ap) { return print_wbuf(nullptr, 0, fmt, ap, FMT_LEGACY_WIDE); }
static int CDECL c__scwprintf(const WCHAR* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c__vscwprintf(fmt, ap); va_end(ap); return r; }
// msvcrt.dll's own versions: three-digit exponents
static int CDECL m_vfprintf(CFILE* f, const char* fmt, va_list ap) { return print_file(f, fmt, ap, MS); }
static int CDECL m_vprintf(const char* fmt, va_list ap) { return print_file(&iob[1], fmt, ap, MS); }
static int CDECL m_fprintf(CFILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = print_file(f, fmt, ap, MS); va_end(ap); return r; }
static int CDECL m_printf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = print_file(&iob[1], fmt, ap, MS); va_end(ap); return r; }
static int CDECL m__vsnprintf(char* buf, usize n, const char* fmt, va_list ap) { return c___stdio_common_vsprintf(UO_LEGACY_VSPRINTF_NULL | UO_THREE_DIGIT, buf, n, fmt, nullptr, ap); }
static int CDECL m__snprintf(char* buf, usize n, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = m__vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
static int CDECL m_vsprintf(char* buf, const char* fmt, va_list ap) { usize w; int n = print_buf(buf, 0x7FFFFFFF, fmt, ap, MS, &w); buf[n] = 0; return n; }
static int CDECL m_sprintf(char* buf, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = m_vsprintf(buf, fmt, ap); va_end(ap); return r; }

// ---------------------------------------------------------------- scanf
template <typename C>
static int scan_impl(ScanSrc* in, const C* f, va_list ap, int flags) {
    int assigned = 0, conversions = 0;
    bool wide_fmt = sizeof(C) == 2;
    auto skip_ws = [&]() { int c; while ((c = in->get(in)) >= 0 && (c == ' ' || (c >= 9 && c <= 13))) {} if (c >= 0) in->unget(in, c); return c; };
    while (*f) {
        u32 fc = (u32)(sizeof(C) == 1 ? (u8)*f : (u16)*f);
        if (fc == ' ' || (fc >= 9 && fc <= 13)) { skip_ws(); f++; continue; }
        if (fc != '%') {
            int c = in->get(in);
            if (c < 0) return conversions || assigned ? assigned : -1;
            if ((u32)c != fc) { in->unget(in, c); return assigned; }
            f++;
            continue;
        }
        f++;
        if (*f == '%') {
            skip_ws();
            int c = in->get(in);
            if (c != '%') { if (c >= 0) in->unget(in, c); return assigned; }
            f++;
            continue;
        }
        bool suppress = false;
        if (*f == '*') { suppress = true; f++; }
        int width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10 + (int)(*f++ - '0');
        int size = 4;                                            // 1 hh, 2 h, 4 default, 8 ll/I64/z, 16 L
        bool lng = false, shrt = false;
        for (;;) {
            if (*f == 'h') { shrt = true; size = f[1] == 'h' ? 1 : 2; if (f[1] == 'h') f++; f++; }
            else if (*f == 'l') { lng = true; if (f[1] == 'l') { size = 8; f++; } else size = 4; f++; }
            else if (*f == 'L') { size = 16; f++; }
            else if (*f == 'j' || *f == 'z' || *f == 't') { size = 8; f++; }
            else if (*f == 'w') { lng = true; f++; }
            else if (*f == 'I') {
                if (f[1] == '6' && f[2] == '4') { size = 8; f += 3; }
                else if (f[1] == '3' && f[2] == '2') { size = 4; f += 3; }
                else { size = 8; f++; }
            } else break;
        }
        u32 conv = (u32)(sizeof(C) == 1 ? (u8)*f : (u16)*f);
        if (!conv) break;
        f++;
        if (conv == 'n') { if (!suppress) { void* p = va_arg(ap, void*); if (size == 8) *(i64*)p = in->count; else if (size == 2) *(i16*)p = (i16)in->count; else if (size == 1) *(i8*)p = (i8)in->count; else *(int*)p = in->count; } continue; }
        if (conv != 'c' && conv != 'C' && conv != '[') { if (skip_ws() < 0) return conversions || assigned ? assigned : -1; }
        // strings, characters and scan sets
        if (conv == 's' || conv == 'S' || conv == 'c' || conv == 'C' || conv == '[') {
            bool wide_dst = lng || conv == 'S' || conv == 'C' ? !(shrt) : ((flags & SCAN_LEGACY_WIDE) && wide_fmt && !shrt);
            if ((conv == 'S' || conv == 'C') && (flags & SCAN_LEGACY_WIDE) && wide_fmt) wide_dst = false;
            bool set[256] = {false};
            bool invert = false;
            u32 wide_ranges[32][2];
            int nranges = 0;
            if (conv == '[') {
                if (*f == '^') { invert = true; f++; }
                bool first = true;
                while (*f && (first || *f != ']')) {
                    u32 a = (u32)(sizeof(C) == 1 ? (u8)*f : (u16)*f);
                    u32 b = a;
                    if (f[1] == '-' && f[2] && f[2] != ']') { b = (u32)(sizeof(C) == 1 ? (u8)f[2] : (u16)f[2]); f += 2; }
                    if (a > b) { u32 t = a; a = b; b = t; }
                    for (u32 k = a; k <= b && k < 256; k++) set[k] = true;
                    if (b >= 256 && nranges < 32) { wide_ranges[nranges][0] = a; wide_ranges[nranges][1] = b; nranges++; }
                    f++;
                    first = false;
                }
                if (*f == ']') f++;
            }
            auto in_set = [&](int c) {
                bool m = c < 256 ? set[c] : false;
                for (int k = 0; !m && k < nranges; k++) if ((u32)c >= wide_ranges[k][0] && (u32)c <= wide_ranges[k][1]) m = true;
                return invert ? !m : m;
            };
            usize cap = (usize)-1;
            void* dst = nullptr;
            if (!suppress) {
                dst = va_arg(ap, void*);
                if (flags & SCAN_SECURE) cap = (usize)va_arg(ap, u32);
            }
            int maxw = width ? width : (conv == 'c' || conv == 'C' ? 1 : 0x7FFFFFFF);
            usize o = 0;
            int got = 0;
            while (got < maxw) {
                int c = in->get(in);
                if (c < 0) break;
                bool ok = conv == '[' ? in_set(c) : (conv == 'c' || conv == 'C') ? true : !(c == ' ' || (c >= 9 && c <= 13));
                if (!ok) { in->unget(in, c); break; }
                got++;
                if (suppress) continue;
                u32 cp = (u32)c;
                if (!wide_fmt && wide_dst && cp >= 0x80) {               // a UTF-8 sequence from narrow input into a wide string
                    int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
                    u32 v = cp & (0x3F >> more);
                    for (int k = 0; k < more; k++) { int d = in->get(in); if (d < 0 || (d & 0xC0) != 0x80) { if (d >= 0) in->unget(in, d); break; } v = (v << 6) | (u32)(d & 63); }
                    cp = v;
                }
                if (wide_dst) {
                    if (o + 1 >= cap && conv != 'c' && conv != 'C') { ((WCHAR*)dst)[0] = 0; return assigned; }
                    if (o < cap) ((WCHAR*)dst)[o++] = (WCHAR)cp;
                } else {
                    char b[4];
                    int k = 1;
                    if (wide_fmt && cp >= 0x80) k = cp < 0x800 ? 2 : 3, b[0] = (char)(k == 2 ? 0xC0 | (cp >> 6) : 0xE0 | (cp >> 12)), b[k - 1] = (char)(0x80 | (cp & 63)), (k == 3 ? (b[1] = (char)(0x80 | ((cp >> 6) & 63))) : 0);
                    else b[0] = (char)cp;
                    for (int j = 0; j < k; j++) {
                        if (o + 1 >= cap && conv != 'c' && conv != 'C') { ((char*)dst)[0] = 0; return assigned; }
                        if (o < cap) ((char*)dst)[o++] = b[j];
                    }
                }
            }
            if (!got) return conversions || assigned ? assigned : (in->get(in) < 0 ? -1 : assigned);
            conversions++;
            if (!suppress) {
                if (conv != 'c' && conv != 'C') { if (wide_dst) ((WCHAR*)dst)[o] = 0; else ((char*)dst)[o] = 0; }
                assigned++;
            }
            continue;
        }
        // numbers: collect the text first
        char num[400];
        int n = 0, maxw = width ? width : (int)sizeof(num) - 1;
        if (maxw > (int)sizeof(num) - 1) maxw = (int)sizeof(num) - 1;
        auto take = [&](int c) { num[n++] = (char)c; };
        int c = in->get(in);
        if (conv == 'e' || conv == 'E' || conv == 'f' || conv == 'F' || conv == 'g' || conv == 'G' || conv == 'a' || conv == 'A') {
            if ((c == '+' || c == '-') && n < maxw) { take(c); c = in->get(in); }
            if ((c == 'i' || c == 'I' || c == 'n' || c == 'N') && n < maxw) {           // inf, infinity, nan
                const char* word = (c | 32) == 'i' ? "infinity" : "nan";
                int k = 0;
                while (word[k] && n < maxw && c >= 0 && (c | 32) == word[k]) { take(c); k++; c = in->get(in); }
                if (c >= 0) in->unget(in, c);
                if (k < 3) return assigned;
            } else {
                bool hex = false, digits = false;
                if (c == '0' && n < maxw) { take(c); digits = true; c = in->get(in); if ((c == 'x' || c == 'X') && n < maxw) { hex = true; take(c); c = in->get(in); } }
                auto isd = [&](int d) { return (d >= '0' && d <= '9') || (hex && ((d | 32) >= 'a' && (d | 32) <= 'f')); };
                while (c >= 0 && isd(c) && n < maxw) { take(c); digits = true; c = in->get(in); }
                if (c == '.' && n < maxw) { take(c); c = in->get(in); while (c >= 0 && isd(c) && n < maxw) { take(c); digits = true; c = in->get(in); } }
                if (digits && (c == (hex ? 'p' : 'e') || c == (hex ? 'P' : 'E')) && n < maxw) {
                    take(c); c = in->get(in);
                    if ((c == '+' || c == '-') && n < maxw) { take(c); c = in->get(in); }
                    while (c >= '0' && c <= '9' && n < maxw) { take(c); c = in->get(in); }
                }
                if (c >= 0) in->unget(in, c);
                if (!digits) return assigned;
            }
            num[n] = 0;
            double v = parse_double(num, nullptr);
            conversions++;
            if (!suppress) {
                void* p = va_arg(ap, void*);
                if (lng || size == 16) *(double*)p = v; else *(float*)p = (float)v;
                assigned++;
            }
            continue;
        }
        int base = conv == 'x' || conv == 'X' || conv == 'p' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
        if (conv != 'd' && conv != 'i' && conv != 'u' && conv != 'x' && conv != 'X' && conv != 'o' && conv != 'p') return assigned;
        if ((c == '+' || c == '-') && n < maxw) { take(c); c = in->get(in); }
        if (c == '0' && n < maxw && (base == 0 || base == 16)) {
            take(c); c = in->get(in);
            if ((c == 'x' || c == 'X') && n < maxw) { take(c); c = in->get(in); base = 16; }
            else if (base == 0) base = 8;
        }
        if (!base) base = 10;
        bool any = n && num[n - 1] == '0';
        auto ok = [&](int d) { int v = d >= '0' && d <= '9' ? d - '0' : (d | 32) >= 'a' && (d | 32) <= 'z' ? (d | 32) - 'a' + 10 : 99; return v < base; };
        while (c >= 0 && ok(c) && n < maxw) { take(c); any = true; c = in->get(in); }
        if (c >= 0) in->unget(in, c);
        if (!any) return conversions || assigned ? assigned : (c < 0 ? -1 : assigned);
        num[n] = 0;
        bool neg = num[0] == '-';
        const char* q = num + (num[0] == '-' || num[0] == '+');
        if (base == 16 && q[0] == '0' && (q[1] | 32) == 'x') q += 2;
        u64 v = 0;
        for (; *q; q++) { int d = *q >= '0' && *q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10; v = v * (u64)base + (u64)d; }
        if (neg) v = 0 - v;
        conversions++;
        if (!suppress) {
            void* p = va_arg(ap, void*);
            if (conv == 'p') *(u64*)p = v;
            else if (size == 1) *(u8*)p = (u8)v;
            else if (size == 2) *(u16*)p = (u16)v;
            else if (size == 8 || size == 16) *(u64*)p = v;
            else *(u32*)p = (u32)v;
            assigned++;
        }
    }
    return assigned;
}
int vscan(ScanSrc* src, const void* fmt, va_list ap, int flags) {
    return flags & SCAN_WIDE_FMT ? scan_impl(src, (const WCHAR*)fmt, ap, flags) : scan_impl(src, (const char*)fmt, ap, flags);
}

struct StrSrc : ScanSrc { const u8* s; usize len, pos; };
static int str_get(ScanSrc* b) { StrSrc* s = (StrSrc*)b; if (s->pos >= s->len || !s->s[s->pos]) { s->pos++; return -1; } s->count++; return s->s[s->pos++]; }
static void str_unget(ScanSrc* b, int c) { (void)c; StrSrc* s = (StrSrc*)b; s->pos--; s->count--; }
struct WStrSrc : ScanSrc { const WCHAR* s; usize len, pos; };
static int wstr_get(ScanSrc* b) { WStrSrc* s = (WStrSrc*)b; if (s->pos >= s->len || !s->s[s->pos]) { s->pos++; return -1; } s->count++; return (u16)s->s[s->pos++]; }
static void wstr_unget(ScanSrc* b, int c) { (void)c; WStrSrc* s = (WStrSrc*)b; s->pos--; s->count--; }
struct FileSrc : ScanSrc { Priv* p; bool wide; };
static int file_get(ScanSrc* b) { FileSrc* s = (FileSrc*)b; int c = s->wide ? get_wide(s->p) : s_getc(s->p); if (c >= 0) s->count++; return c; }
static void file_unget(ScanSrc* b, int c) {
    FileSrc* s = (FileSrc*)b;
    if (c < 0) return;
    s->count--;
    if (s->wide && c >= 0x80) {
        char t[4]; int k = c < 0x800 ? 2 : 3;
        t[0] = (char)(k == 2 ? 0xC0 | (c >> 6) : 0xE0 | (c >> 12)); if (k == 3) t[1] = (char)(0x80 | ((c >> 6) & 63)); t[k - 1] = (char)(0x80 | (c & 63));
        for (int i = k - 1; i >= 0; i--) raw_unget(s->p, (u8)t[i]);
    } else raw_unget(s->p, c);
}

static int scan_str(const char* s, usize len, const void* fmt, va_list ap, int flags) {
    StrSrc src; src.get = str_get; src.unget = str_unget; src.count = 0; src.s = (const u8*)s; src.len = len; src.pos = 0;
    return vscan(&src, fmt, ap, flags);
}
static int scan_wstr(const WCHAR* s, usize len, const void* fmt, va_list ap, int flags) {
    WStrSrc src; src.get = wstr_get; src.unget = wstr_unget; src.count = 0; src.s = s; src.len = len; src.pos = 0;
    return vscan(&src, fmt, ap, flags);
}
static int scan_file(CFILE* f, const void* fmt, va_list ap, int flags, bool wide) {
    Priv* p = P(f);
    if (!p) return -1;
    FLock l(p);
    FileSrc src; src.get = file_get; src.unget = file_unget; src.count = 0; src.p = p; src.wide = wide;
    return vscan(&src, fmt, ap, flags);
}
static int sflags(u64 opt) { return ((opt & 1) ? SCAN_SECURE : 0) | ((opt & 2) ? SCAN_LEGACY_WIDE : 0); }
static int CDECL c___stdio_common_vsscanf(u64 opt, const char* s, usize len, const char* fmt, void* loc, va_list ap) { (void)loc; return scan_str(s, len, fmt, ap, sflags(opt)); }
static int CDECL c___stdio_common_vswscanf(u64 opt, const WCHAR* s, usize len, const WCHAR* fmt, void* loc, va_list ap) { (void)loc; return scan_wstr(s, len, fmt, ap, sflags(opt) | SCAN_WIDE_FMT); }
static int CDECL c___stdio_common_vfscanf(u64 opt, CFILE* f, const char* fmt, void* loc, va_list ap) { (void)loc; return scan_file(f, fmt, ap, sflags(opt), false); }
static int CDECL c___stdio_common_vfwscanf(u64 opt, CFILE* f, const WCHAR* fmt, void* loc, va_list ap) { (void)loc; return scan_file(f, fmt, ap, sflags(opt) | SCAN_WIDE_FMT, true); }
static int CDECL c_vsscanf(const char* s, const char* fmt, va_list ap) { return scan_str(s, (usize)-1, fmt, ap, 0); }
static int CDECL c_sscanf(const char* s, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c_vsscanf(s, fmt, ap); va_end(ap); return r; }
static int CDECL c_sscanf_s(const char* s, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = scan_str(s, (usize)-1, fmt, ap, SCAN_SECURE); va_end(ap); return r; }
static int CDECL c_vfscanf(CFILE* f, const char* fmt, va_list ap) { return scan_file(f, fmt, ap, 0, false); }
static int CDECL c_fscanf(CFILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c_vfscanf(f, fmt, ap); va_end(ap); return r; }
static int CDECL c_fscanf_s(CFILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = scan_file(f, fmt, ap, SCAN_SECURE, false); va_end(ap); return r; }
static int CDECL c_vscanf(const char* fmt, va_list ap) { return scan_file(&iob[0], fmt, ap, 0, false); }
static int CDECL c_scanf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = c_vscanf(fmt, ap); va_end(ap); return r; }
static int CDECL c_scanf_s(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = scan_file(&iob[0], fmt, ap, SCAN_SECURE, false); va_end(ap); return r; }
static int CDECL c_swscanf(const WCHAR* s, const WCHAR* fmt, ...) { va_list ap; va_start(ap, fmt); int r = scan_wstr(s, (usize)-1, fmt, ap, SCAN_WIDE_FMT | SCAN_LEGACY_WIDE); va_end(ap); return r; }
static int CDECL c_vswscanf(const WCHAR* s, const WCHAR* fmt, va_list ap) { return scan_wstr(s, (usize)-1, fmt, ap, SCAN_WIDE_FMT | SCAN_LEGACY_WIDE); }
static int CDECL c_fwscanf(CFILE* f, const WCHAR* fmt, ...) { va_list ap; va_start(ap, fmt); int r = scan_file(f, fmt, ap, SCAN_WIDE_FMT | SCAN_LEGACY_WIDE, true); va_end(ap); return r; }
static int CDECL c_wscanf(const WCHAR* fmt, ...) { va_list ap; va_start(ap, fmt); int r = scan_file(&iob[0], fmt, ap, SCAN_WIDE_FMT | SCAN_LEGACY_WIDE, true); va_end(ap); return r; }

// ---------------------------------------------------------------- errors
static const char* err_text(int e) {
    static const char* t[] = {"No error", "Operation not permitted", "No such file or directory", "No such process", "Interrupted function call",
        "Input/output error", "No such device or address", "Arg list too long", "Exec format error", "Bad file descriptor", "No child processes",
        "Resource temporarily unavailable", "Not enough space", "Permission denied", "Bad address", "Unknown error", "Resource device",
        "File exists", "Improper link", "No such device", "Not a directory", "Is a directory", "Invalid argument", "Too many open files in system",
        "Too many open files", "Inappropriate I/O control operation", "Unknown error", "File too large", "No space left on device", "Invalid seek",
        "Read-only file system", "Too many links", "Broken pipe", "Domain error", "Result too large", "Unknown error", "Resource deadlock avoided",
        "Unknown error", "Filename too long", "No locks available", "Function not implemented", "Directory not empty", "Illegal byte sequence"};
    if (e >= 0 && e < (int)(sizeof(t) / sizeof(t[0]))) return t[e];
    if (e == STRUNCATE) return "Truncated";
    return "Unknown error";
}
static char* CDECL c_strerror(int e) { CrtThread* t = crt_thread(); slcpy(t->strerror_buf, err_text(e), sizeof(t->strerror_buf)); return t->strerror_buf; }
static int CDECL c_strerror_s(char* buf, usize n, int e) { slcpy(buf, err_text(e), n); return 0; }
static char* CDECL c__strerror(const char* s) {
    CrtThread* t = crt_thread();
    if (s && *s) snfmt(t->strerror_buf, sizeof(t->strerror_buf), "%s: %s\n", s, err_text(*errno_ptr()));
    else snfmt(t->strerror_buf, sizeof(t->strerror_buf), "%s\n", err_text(*errno_ptr()));
    return t->strerror_buf;
}
static WCHAR* CDECL c__wcserror(int e) { static WCHAR b[96]; wide_from_utf8(err_text(e), -1, b, 96); return b; }
static int CDECL c__wcserror_s(WCHAR* buf, usize n, int e) { wide_from_utf8(err_text(e), -1, buf, (int)n); return 0; }
static void CDECL c_perror(const char* s) {
    int e = *errno_ptr();
    if (s && *s) c_fprintf(&iob[2], "%s: %s\n", s, err_text(e));
    else c_fprintf(&iob[2], "%s\n", err_text(e));
}
static void CDECL c__wperror(const WCHAR* s) { char u[300]; if (s) utf8_from_wide(s, -1, u, sizeof(u)); else u[0] = 0; c_perror(u); }
static const char* sys_errlist_arr[43];
static int sys_nerr_v = 43;
static const char** CDECL c___sys_errlist() { if (!sys_errlist_arr[0]) for (int i = 0; i < 43; i++) sys_errlist_arr[i] = err_text(i); return sys_errlist_arr; }
static int* CDECL c___sys_nerr() { return &sys_nerr_v; }

// ---------------------------------------------------------------- directories and files by name
static WCHAR* wpath_arg(const char* s) { return wide_dup(s); }
static int CDECL c__wremove(const WCHAR* p) {
    if (!k32<BOOL (WINAPI*)(const WCHAR*)>("DeleteFileW")(p)) { set_errno_from_win32(); return -1; }
    return 0;
}
static int CDECL c_remove(const char* p) { WCHAR* w = wpath_arg(p); int r = c__wremove(w); ifree(w); return r; }
static int CDECL c__wrename(const WCHAR* a, const WCHAR* b) {
    if (!k32<BOOL (WINAPI*)(const WCHAR*, const WCHAR*, u32)>("MoveFileExW")(a, b, 0)) { set_errno_from_win32(); return -1; }
    return 0;
}
static int CDECL c_rename(const char* a, const char* b) { WCHAR* x = wpath_arg(a); WCHAR* y = wpath_arg(b); int r = c__wrename(x, y); ifree(x); ifree(y); return r; }
static int CDECL c__wmkdir(const WCHAR* p) {
    if (!k32<BOOL (WINAPI*)(const WCHAR*, void*)>("CreateDirectoryW")(p, nullptr)) { set_errno_from_win32(); return -1; }
    return 0;
}
static int CDECL c__mkdir(const char* p) { WCHAR* w = wpath_arg(p); int r = c__wmkdir(w); ifree(w); return r; }
static int CDECL c__wrmdir(const WCHAR* p) {
    if (!k32<BOOL (WINAPI*)(const WCHAR*)>("RemoveDirectoryW")(p)) { set_errno_from_win32(); return -1; }
    return 0;
}
static int CDECL c__rmdir(const char* p) { WCHAR* w = wpath_arg(p); int r = c__wrmdir(w); ifree(w); return r; }
static int CDECL c__wchdir(const WCHAR* p) {
    if (!k32<BOOL (WINAPI*)(const WCHAR*)>("SetCurrentDirectoryW")(p)) { set_errno_from_win32(); return -1; }
    return 0;
}
static int CDECL c__chdir(const char* p) { WCHAR* w = wpath_arg(p); int r = c__wchdir(w); ifree(w); return r; }
static char* CDECL c__getcwd(char* buf, int n) {
    char tmp[MAX_PATH];
    u32 k = k32<u32 (WINAPI*)(u32, char*)>("GetCurrentDirectoryA")(sizeof(tmp), tmp);
    if (!buf) { usize cap = (usize)(n > (int)k ? n : (int)k + 1); buf = (char*)h_alloc(cap); }
    else if ((int)k >= n) { set_errno(ERANGE); return nullptr; }
    memcpy(buf, tmp, k + 1);
    return buf;
}
static WCHAR* CDECL c__wgetcwd(WCHAR* buf, int n) {
    WCHAR tmp[MAX_PATH];
    u32 k = k32<u32 (WINAPI*)(u32, WCHAR*)>("GetCurrentDirectoryW")(MAX_PATH, tmp);
    if (!buf) { usize cap = (usize)(n > (int)k ? n : (int)k + 1); buf = (WCHAR*)h_alloc(cap * 2); }
    else if ((int)k >= n) { set_errno(ERANGE); return nullptr; }
    memcpy(buf, tmp, (k + 1) * 2);
    return buf;
}
static char* CDECL c__getdcwd(int d, char* buf, int n) { (void)d; return c__getcwd(buf, n); }
static WCHAR* CDECL c__wgetdcwd(int d, WCHAR* buf, int n) { (void)d; return c__wgetcwd(buf, n); }
static int CDECL c__getdrive() { return 3; }
static int CDECL c__chdrive(int d) { return d == 3 ? 0 : -1; }
static int CDECL c__waccess(const WCHAR* p, int mode) {
    u32 a = k32<u32 (WINAPI*)(const WCHAR*)>("GetFileAttributesW")(p);
    if (a == 0xFFFFFFFF) { set_errno(ENOENT); return -1; }
    if ((mode & 2) && (a & 1)) { set_errno(EACCES); return -1; }
    return 0;
}
static int CDECL c__access(const char* p, int mode) { WCHAR* w = wpath_arg(p); int r = c__waccess(w, mode); ifree(w); return r; }
static int CDECL c__access_s(const char* p, int mode) { return c__access(p, mode) ? *errno_ptr() : 0; }
static int CDECL c__waccess_s(const WCHAR* p, int mode) { return c__waccess(p, mode) ? *errno_ptr() : 0; }
static int CDECL c__wchmod(const WCHAR* p, int mode) { (void)mode; return c__waccess(p, 0); }
static int CDECL c__chmod(const char* p, int mode) { (void)mode; return c__access(p, 0); }
static int CDECL c__wunlink(const WCHAR* p) { return c__wremove(p); }
static int CDECL c__unlink(const char* p) { return c_remove(p); }
static int CDECL c__utime64(const char* p, void* t) { (void)t; return c__access(p, 0); }
static int CDECL c__wutime64(const WCHAR* p, void* t) { (void)t; return c__waccess(p, 0); }
static int CDECL c__futime64(int fd, void* t) { (void)t; return fd_get(fd) ? 0 : -1; }

// stat: one filler for every layout of struct stat
struct StatInfo { u32 mode; u64 size; i64 atime, mtime, ctime; u32 dev; };
static bool stat_of(const WCHAR* path, StatInfo* si_) {
    struct WFAD { u32 attr; FILETIME c, a, w; u32 hi, lo; } d;
    if (!k32<BOOL (WINAPI*)(const WCHAR*, int, WFAD*)>("GetFileAttributesExW")(path, 0, &d)) { set_errno(ENOENT); return false; }
    bool dir = d.attr & 0x10;
    u32 m = dir ? 0x4000 | 0x40 : 0x8000;
    m |= 0x100 | ((d.attr & 1) ? 0 : 0x80);
    usize n = wlen(path);
    if (!dir && n > 4 && (!wicmp(path + n - 4, L".exe") || !wicmp(path + n - 4, L".bat") || !wicmp(path + n - 4, L".cmd") || !wicmp(path + n - 4, L".com"))) m |= 0x40;
    m |= (m & 0x1C0) >> 3 | (m & 0x1C0) >> 6;
    si_->mode = m;
    si_->size = ((u64)d.hi << 32) | d.lo;
    auto ft = [](FILETIME f) { u64 v = ((u64)f.dwHighDateTime << 32) | f.dwLowDateTime; return (i64)(v / 10000000ull) - 11644473600ll; };
    si_->mtime = ft(d.w); si_->atime = ft(d.a); si_->ctime = ft(d.c);
    si_->dev = 2;
    return true;
}
static void fill_stat(void* out, const StatInfo& s, int layout) {
    u8* b = (u8*)out;
    // dev, ino, mode, nlink, uid, gid, rdev: the same in every layout
    memset(b, 0, layout == 0 ? 36 : layout == 3 ? 56 : 48);
    *(u32*)(b + 0) = s.dev;
    *(u16*)(b + 6) = (u16)s.mode;
    *(i16*)(b + 8) = 1;
    *(u32*)(b + 16) = s.dev;
    switch (layout) {
    case 0: *(u32*)(b + 20) = (u32)s.size; *(i32*)(b + 24) = (i32)s.atime; *(i32*)(b + 28) = (i32)s.mtime; *(i32*)(b + 32) = (i32)s.ctime; break;   // _stat32
    case 1: *(u64*)(b + 24) = s.size; *(i32*)(b + 32) = (i32)s.atime; *(i32*)(b + 36) = (i32)s.mtime; *(i32*)(b + 40) = (i32)s.ctime; break;       // _stat32i64
    case 2: *(u32*)(b + 20) = (u32)s.size; *(i64*)(b + 24) = s.atime; *(i64*)(b + 32) = s.mtime; *(i64*)(b + 40) = s.ctime; break;                 // _stat64i32
    case 3: *(u64*)(b + 24) = s.size; *(i64*)(b + 32) = s.atime; *(i64*)(b + 40) = s.mtime; *(i64*)(b + 48) = s.ctime; break;                      // _stat64
    }
}
static int wstat_any(const WCHAR* p, void* out, int layout) {
    WCHAR tmp[MAX_PATH];
    usize n = wlen(p);
    if (n >= MAX_PATH) { set_errno(ENAMETOOLONG); return -1; }
    memcpy(tmp, p, (n + 1) * 2);
    while (n > 1 && (tmp[n - 1] == '\\' || tmp[n - 1] == '/') && !(n == 3 && tmp[1] == ':')) tmp[--n] = 0;     // "dir\" is "dir"
    StatInfo s;
    if (!stat_of(tmp, &s)) return -1;
    fill_stat(out, s, layout);
    return 0;
}
static int stat_any(const char* p, void* out, int layout) { WCHAR* w = wide_dup(p); int r = wstat_any(w, out, layout); ifree(w); return r; }
static int fstat_any(int fd, void* out, int layout) {
    Fd* f = fd_get(fd);
    if (!f) return -1;
    StatInfo s;
    memset(&s, 0, sizeof(s));
    if (f->flags & FD_CONSOLE) s.mode = 0x2000 | 0x1B6;
    else if (f->flags & FD_PIPE) s.mode = 0x1000 | 0x1B6;
    else {
        struct BHFI { u32 attr; FILETIME c, a, w; u32 vol, hi, lo, links, ih, il; } i;
        if (!k32<BOOL (WINAPI*)(HANDLE, BHFI*)>("GetFileInformationByHandle")(f->h, &i)) { set_errno(EBADF); return -1; }
        s.mode = (i.attr & 0x10) ? 0x4000 | 0x1FF : 0x8000 | 0x1B6;
        s.size = ((u64)i.hi << 32) | i.lo;
        auto ft = [](FILETIME t) { u64 v = ((u64)t.dwHighDateTime << 32) | t.dwLowDateTime; return (i64)(v / 10000000ull) - 11644473600ll; };
        s.mtime = ft(i.w); s.atime = ft(i.a); s.ctime = ft(i.c);
        s.dev = 2;
    }
    fill_stat(out, s, layout);
    return 0;
}
static int CDECL c__stat32(const char* p, void* o) { return stat_any(p, o, 0); }
static int CDECL c__stat32i64(const char* p, void* o) { return stat_any(p, o, 1); }
static int CDECL c__stat64i32(const char* p, void* o) { return stat_any(p, o, 2); }
static int CDECL c__stat64(const char* p, void* o) { return stat_any(p, o, 3); }
static int CDECL c__wstat32(const WCHAR* p, void* o) { return wstat_any(p, o, 0); }
static int CDECL c__wstat32i64(const WCHAR* p, void* o) { return wstat_any(p, o, 1); }
static int CDECL c__wstat64i32(const WCHAR* p, void* o) { return wstat_any(p, o, 2); }
static int CDECL c__wstat64(const WCHAR* p, void* o) { return wstat_any(p, o, 3); }
static int CDECL c__fstat32(int fd, void* o) { return fstat_any(fd, o, 0); }
static int CDECL c__fstat32i64(int fd, void* o) { return fstat_any(fd, o, 1); }
static int CDECL c__fstat64i32(int fd, void* o) { return fstat_any(fd, o, 2); }
static int CDECL c__fstat64(int fd, void* o) { return fstat_any(fd, o, 3); }

// _findfirst and friends, in each layout of struct _finddata_t
struct FD { u32 attr; FILETIME c, a, w; u32 hi, lo, r0, r1; WCHAR name[MAX_PATH]; WCHAR alt[14]; };
static i64 ft_unix(FILETIME f) { u64 v = ((u64)f.dwHighDateTime << 32) | f.dwLowDateTime; return (i64)(v / 10000000ull) - 11644473600ll; }
static void fill_find(void* out, const FD& d, int layout, bool wide) {
    u8* b = (u8*)out;
    usize name_off;
    switch (layout) {
    case 0: memset(b, 0, 20); *(u32*)b = d.attr; *(i32*)(b + 4) = (i32)ft_unix(d.c); *(i32*)(b + 8) = (i32)ft_unix(d.a); *(i32*)(b + 12) = (i32)ft_unix(d.w); *(u32*)(b + 16) = d.lo; name_off = 20; break;   // 32
    case 1: memset(b, 0, 32); *(u32*)b = d.attr; *(i32*)(b + 4) = (i32)ft_unix(d.c); *(i32*)(b + 8) = (i32)ft_unix(d.a); *(i32*)(b + 12) = (i32)ft_unix(d.w); *(u64*)(b + 16) = d.lo; name_off = 24; break;   // 32i64
    case 2: memset(b, 0, 36); *(u32*)b = d.attr; *(i64*)(b + 8) = ft_unix(d.c); *(i64*)(b + 16) = ft_unix(d.a); *(i64*)(b + 24) = ft_unix(d.w); *(u32*)(b + 32) = d.lo; name_off = 36; break;       // 64i32
    default: memset(b, 0, 40); *(u32*)b = d.attr; *(i64*)(b + 8) = ft_unix(d.c); *(i64*)(b + 16) = ft_unix(d.a); *(i64*)(b + 24) = ft_unix(d.w); *(u64*)(b + 32) = d.lo; name_off = 40; break;      // 64
    }
    if (wide) memcpy(b + name_off, d.name, sizeof(d.name));
    else utf8_from_wide(d.name, -1, (char*)(b + name_off), MAX_PATH);
}
static i64 findfirst_any(const WCHAR* spec, void* out, int layout, bool wide) {
    FD d;
    HANDLE h = k32<HANDLE (WINAPI*)(const WCHAR*, FD*)>("FindFirstFileW")(spec, &d);
    if (h == INVALID_HANDLE_VALUE) { set_errno(ENOENT); return -1; }
    fill_find(out, d, layout, wide);
    return (i64)h;
}
static int findnext_any(i64 h, void* out, int layout, bool wide) {
    FD d;
    if (!k32<BOOL (WINAPI*)(HANDLE, FD*)>("FindNextFileW")((HANDLE)h, &d)) { set_errno(ENOENT); return -1; }
    fill_find(out, d, layout, wide);
    return 0;
}
static i64 nf(const char* s, void* o, int l) { WCHAR* w = wide_dup(s); i64 r = findfirst_any(w, o, l, false); ifree(w); return r; }
static i64 CDECL c__findfirst32(const char* s, void* o) { return nf(s, o, 0); }
static i64 CDECL c__findfirst32i64(const char* s, void* o) { return nf(s, o, 1); }
static i64 CDECL c__findfirst64i32(const char* s, void* o) { return nf(s, o, 2); }
static i64 CDECL c__findfirst64(const char* s, void* o) { return nf(s, o, 3); }
static int CDECL c__findnext32(i64 h, void* o) { return findnext_any(h, o, 0, false); }
static int CDECL c__findnext32i64(i64 h, void* o) { return findnext_any(h, o, 1, false); }
static int CDECL c__findnext64i32(i64 h, void* o) { return findnext_any(h, o, 2, false); }
static int CDECL c__findnext64(i64 h, void* o) { return findnext_any(h, o, 3, false); }
static i64 CDECL c__wfindfirst32(const WCHAR* s, void* o) { return findfirst_any(s, o, 0, true); }
static i64 CDECL c__wfindfirst64i32(const WCHAR* s, void* o) { return findfirst_any(s, o, 2, true); }
static i64 CDECL c__wfindfirst64(const WCHAR* s, void* o) { return findfirst_any(s, o, 3, true); }
static int CDECL c__wfindnext32(i64 h, void* o) { return findnext_any(h, o, 0, true); }
static int CDECL c__wfindnext64i32(i64 h, void* o) { return findnext_any(h, o, 2, true); }
static int CDECL c__wfindnext64(i64 h, void* o) { return findnext_any(h, o, 3, true); }
static int CDECL c__findclose(i64 h) { return k32<BOOL (WINAPI*)(HANDLE)>("FindClose")((HANDLE)h) ? 0 : -1; }

// ---------------------------------------------------------------- the console (conio.h)
static int CDECL c__getch() { flush_all_streams(); u8 c; int k = fd_raw_read(0, &c, 1); return k == 1 ? (c == '\n' ? '\r' : c) : -1; }
static int CDECL c__getche() { return c__getch(); }
static int CDECL c__kbhit() { return 0; }
static int CDECL c__putch(int c) { char b = (char)c; con_out(&b, 1); return c; }
static int CDECL c__cputs(const char* s) { con_out(s, slen(s)); return 0; }
static u16 CDECL c__getwch() { return (u16)c__getch(); }
static u16 CDECL c__putwch(WCHAR c) { char b[4]; int k = utf8_from_wide(&c, 1, b, 4); con_out(b, (usize)k); return (u16)c; }
static int CDECL c__ungetch(int c) { return c; }
static int CDECL c__cprintf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char b[1024];
    int n = vsnfmt(b, sizeof(b), fmt, ap);
    va_end(ap);
    con_out(b, (usize)(n < (int)sizeof(b) ? n : (int)sizeof(b) - 1));
    return n;
}

static const Export stdio_exports[] = {
    X(_read), X(_write), X(_open), X(_wopen), X(_sopen), X(_wsopen), X(_sopen_s), X(_wsopen_s), X(_sopen_dispatch), X(_wsopen_dispatch),
    X(_creat), X(_wcreat), X(_close), X(_lseek), X(_lseeki64), X(_tell), X(_telli64), X(_commit), X(_dup), X(_dup2), X(_filelength),
    X(_filelengthi64), X(_eof), X(_chsize), X(_chsize_s), X(_get_osfhandle), X(_open_osfhandle), X(_isatty), X(_setmode), X(_locking),
    X(_umask), X(_umask_s), X(_pipe),
    EA("read", c__read), EA("write", c__write), EA("open", c__open), EA("close", c__close), EA("lseek", c__lseek), EA("dup", c__dup),
    EA("dup2", c__dup2), EA("isatty", c__isatty), EA("fileno", c__fileno), EA("unlink", c__unlink), EA("access", c__access),
    EA("chdir", c__chdir), EA("getcwd", c__getcwd), EA("mkdir", c__mkdir), EA("rmdir", c__rmdir), EA("setmode", c__setmode),
    X(fopen), X(_wfopen), X(fopen_s), X(_wfopen_s), X(_fsopen), X(_wfsopen), X(_fdopen), X(_wfdopen), X(fclose), X(_fcloseall), X(freopen),
    X(_wfreopen), X(freopen_s), X(_wfreopen_s), X(fflush), X(_flushall), X(fgetc), X(fputc), EA("getc", c_fgetc), EA("putc", c_fputc),
    X(getchar), X(putchar), X(ungetc), X(fgets), X(fputs), X(puts), X(gets), X(gets_s), X(fread), X(fread_s), X(fwrite), X(fseek),
    X(_fseeki64), X(ftell), X(_ftelli64), X(rewind), X(fgetpos), X(fsetpos), X(feof), X(ferror), X(clearerr), X(clearerr_s), X(_fileno),
    X(setvbuf), X(setbuf), X(__acrt_iob_func), X(__iob_func), X(_lock_file), X(_unlock_file), X(_getw), X(_putw), X(_fgetchar),
    X(_fputchar), X(_get_stream_buffer_pointers), X(tmpfile), X(tmpfile_s), X(tmpnam), X(tmpnam_s), X(_wtmpnam), X(_tempnam), X(_mktemp),
    X(_mktemp_s),
    EA("_fread_nolock", c_fread), EA("_fwrite_nolock", c_fwrite), EA("_fseek_nolock", c_fseek), EA("_ftell_nolock", c_ftell),
    EA("_fseeki64_nolock", c__fseeki64), EA("_ftelli64_nolock", c__ftelli64), EA("_fgetc_nolock", c_fgetc), EA("_fputc_nolock", c_fputc),
    EA("_getc_nolock", c_fgetc), EA("_putc_nolock", c_fputc), EA("_ungetc_nolock", c_ungetc), EA("_fflush_nolock", c_fflush),
    EA("_fclose_nolock", c_fclose), EA("_getchar_nolock", c_getchar), EA("_putchar_nolock", c_putchar),
    X(fputwc), X(fgetwc), X(ungetwc), EA("putwc", c_fputwc), EA("getwc", c_fgetwc), X(getwchar), X(putwchar), X(fputws), X(_putws), X(fgetws),
    X(_getws_s), X(fwide), X(_fgetwchar), X(_fputwchar), EA("_fputwc_nolock", c_fputwc), EA("_fgetwc_nolock", c_fgetwc),
    X(__stdio_common_vfprintf), X(__stdio_common_vfprintf_s), X(__stdio_common_vfprintf_p), X(__stdio_common_vsprintf),
    X(__stdio_common_vsprintf_s), X(__stdio_common_vsnprintf_s), X(__stdio_common_vsprintf_p), X(__stdio_common_vfwprintf),
    X(__stdio_common_vfwprintf_s), X(__stdio_common_vfwprintf_p), X(__stdio_common_vswprintf), X(__stdio_common_vswprintf_s),
    X(__stdio_common_vsnwprintf_s), X(__stdio_common_vswprintf_p),
    X(vfprintf), X(vprintf), X(fprintf), X(printf), X(vsnprintf), X(_vsnprintf), X(vsprintf), X(snprintf), X(_snprintf), X(sprintf),
    X(_vscprintf), X(_scprintf), X(vsprintf_s), X(sprintf_s), X(_vsnprintf_s), X(_snprintf_s), X(vfprintf_s), X(vprintf_s), X(fprintf_s),
    X(printf_s), X(vfwprintf), X(vwprintf), X(fwprintf), X(wprintf), X(_vsnwprintf), X(_snwprintf), X(vswprintf), X(swprintf),
    X(_vswprintf), X(_swprintf), X(vswprintf_s), X(swprintf_s), X(_vsnwprintf_s), X(_snwprintf_s), X(_vscwprintf), X(_scwprintf),
    X(__stdio_common_vsscanf), X(__stdio_common_vswscanf), X(__stdio_common_vfscanf), X(__stdio_common_vfwscanf), X(vsscanf), X(sscanf),
    X(sscanf_s), X(vfscanf), X(fscanf), X(fscanf_s), X(vscanf), X(scanf), X(scanf_s), X(swscanf), X(vswscanf), X(fwscanf), X(wscanf),
    X(strerror), X(strerror_s), X(_strerror), X(_wcserror), X(_wcserror_s), X(perror), X(_wperror), X(__sys_errlist), X(__sys_nerr),
    X(remove), X(_wremove), X(rename), X(_wrename), X(_mkdir), X(_wmkdir), X(_rmdir), X(_wrmdir), X(_chdir), X(_wchdir), X(_getcwd),
    X(_wgetcwd), X(_getdcwd), X(_wgetdcwd), X(_getdrive), X(_chdrive), X(_access), X(_waccess), X(_access_s), X(_waccess_s), X(_chmod),
    X(_wchmod), X(_unlink), X(_wunlink), X(_utime64), X(_wutime64), X(_futime64), EA("_utime", c__utime64), EA("_utime32", c__utime64),
    X(_stat32), X(_stat32i64), X(_stat64i32), X(_stat64), X(_wstat32), X(_wstat32i64), X(_wstat64i32), X(_wstat64), X(_fstat32),
    X(_fstat32i64), X(_fstat64i32), X(_fstat64), EA("_stat", c__stat64i32), EA("_wstat", c__wstat64i32), EA("_fstat", c__fstat64i32),
    EA("_stati64", c__stat64), EA("_wstati64", c__wstat64), EA("_fstati64", c__fstat64), EA("stat", c__stat64i32), EA("fstat", c__fstat64i32),
    X(_findfirst32), X(_findfirst32i64), X(_findfirst64i32), X(_findfirst64), X(_findnext32), X(_findnext32i64), X(_findnext64i32),
    X(_findnext64), X(_wfindfirst32), X(_wfindfirst64i32), X(_wfindfirst64), X(_wfindnext32), X(_wfindnext64i32), X(_wfindnext64),
    X(_findclose), EA("_findfirst", c__findfirst64i32), EA("_findnext", c__findnext64i32), EA("_findfirsti64", c__findfirst64),
    EA("_findnexti64", c__findnext64), EA("_wfindfirst", c__wfindfirst64i32), EA("_wfindnext", c__wfindnext64i32),
    X(_getch), X(_getche), X(_kbhit), X(_putch), X(_cputs), X(_getwch), X(_putwch), X(_ungetch), X(_cprintf),
    EA("_getch_nolock", c__getch), EA("_putch_nolock", c__putch),
};
EXPORT_TABLE("ucrtbase.dll", stdio_exports);

static const Export stdio_msvcrt[] = {
    {"_iob", (void*)iob}, EA("vfprintf", m_vfprintf), EA("vprintf", m_vprintf), EA("fprintf", m_fprintf), EA("printf", m_printf),
    EA("_vsnprintf", m__vsnprintf), EA("_snprintf", m__snprintf), EA("vsprintf", m_vsprintf), EA("sprintf", m_sprintf),
};
EXPORT_TABLE("msvcrt.dll", stdio_msvcrt);

}  // namespace rt
