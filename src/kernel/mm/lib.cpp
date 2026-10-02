// silicon - freestanding C library bits
#include "../core.h"

extern "C" {

// plain word loops: QEMU's TCG turns these into fast host code (rep movsb is slow there)
typedef u64 __attribute__((may_alias, aligned(1))) u64u;
void* memcpy(void* d, const void* s, size_t n) {
    u8* dp = (u8*)d;
    const u8* sp = (const u8*)s;
    while (n >= 32) {
        u64 a = ((const u64u*)sp)[0], b = ((const u64u*)sp)[1], c = ((const u64u*)sp)[2], e = ((const u64u*)sp)[3];
        ((u64u*)dp)[0] = a; ((u64u*)dp)[1] = b; ((u64u*)dp)[2] = c; ((u64u*)dp)[3] = e;
        dp += 32; sp += 32; n -= 32;
    }
    while (n >= 8) { *(u64u*)dp = *(const u64u*)sp; dp += 8; sp += 8; n -= 8; }
    while (n--) *dp++ = *sp++;
    return d;
}

void* memmove(void* d, const void* s, size_t n) {
    u8* dp = (u8*)d;
    const u8* sp = (const u8*)s;
    if (dp == sp || n == 0) return d;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    dp += n - 1;
    sp += n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(dp), "+S"(sp), "+c"(n) :: "memory");
    return d;
}

void* memset(void* d, int c, size_t n) {
    void* r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

int memcmp(const void* a, const void* b, size_t n) {
    const u8* x = (const u8*)a;
    const u8* y = (const u8*)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

// gcc/mingw runtime hooks we never need
int atexit(void (*)(void)) { return 0; }
void __cxa_pure_virtual() { panic("pure virtual call"); }
void* __dso_handle = nullptr;

}

void memset32(u32* d, u32 v, size_t count) {
    if (((u64)d & 4) && count) { *d++ = v; count--; }
    u64 vv = ((u64)v << 32) | v;
    u64* q = (u64*)d;
    while (count >= 8) { q[0] = vv; q[1] = vv; q[2] = vv; q[3] = vv; q += 4; count -= 8; }
    while (count >= 2) { *q++ = vv; count -= 2; }
    d = (u32*)q;
    if (count) *d = v;
}

size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (u8)a[i] - (u8)b[i];
    }
    return 0;
}

char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }

void strlcpy(char* d, const char* s, size_t n) {
    if (!n) return;
    size_t i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

void strlcat(char* d, const char* s, size_t n) {
    size_t l = strlen(d);
    if (l + 1 >= n) return;
    strlcpy(d + l, s, n - l);
}

char* strchr(const char* s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char*)s;
    return c == 0 ? (char*)s : nullptr;
}

char* strrchr(const char* s, int c) {
    const char* r = nullptr;
    for (; *s; s++) if (*s == (char)c) r = s;
    return (char*)r;
}

char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
char to_upper(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

bool str_starts(const char* s, const char* p) {
    while (*p) if (*s++ != *p++) return false;
    return true;
}

bool str_ieq_prefix(const char* s, const char* p) {
    while (*p) if (to_lower(*s++) != to_lower(*p++)) return false;
    return true;
}

bool str_icontains(const char* hay, const char* needle) {
    if (!*needle) return true;
    for (; *hay; hay++) if (str_ieq_prefix(hay, needle)) return true;
    return false;
}

char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = (char*)kmalloc(n);
    memcpy(d, s, n);
    return d;
}

int atoi(const char* s) {
    int sign = 1, v = 0;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v * sign;
}

static u32 rng_state = 0x5111C0DE;
void srand(u32 seed) { rng_state = seed ? seed : 1; }
u32 rand() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// ---------------------------------------------------------------- printf
// supports: %d %i %u %x %X %p %s %c %% with optional '-', '0', width, and 'l' (64-bit)
struct Out { char* buf; size_t n; size_t pos; };
static void out_c(Out& o, char c) { if (o.pos + 1 < o.n) o.buf[o.pos] = c; o.pos++; }

static void out_num(Out& o, u64 v, int base, bool neg, int width, bool zero, bool left, bool upper) {
    char tmp[32];
    int len = 0;
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[len++] = digits[v % base]; v /= base; } while (v);
    int total = len + (neg ? 1 : 0);
    if (!left && !zero) for (int i = total; i < width; i++) out_c(o, ' ');
    if (neg) out_c(o, '-');
    if (!left && zero) for (int i = total; i < width; i++) out_c(o, '0');
    while (len) out_c(o, tmp[--len]);
    if (left) for (int i = total; i < width; i++) out_c(o, ' ');
}

int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap) {
    Out o{buf, n, 0};
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_c(o, *fmt); continue; }
        fmt++;
        bool left = false, zero = false, lng = false;
        int width = 0, prec = -1;
        if (*fmt == '-') { left = true; fmt++; }
        if (*fmt == '0') { zero = true; fmt++; }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') { fmt++; prec = 0; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
        while (*fmt == 'l') { lng = true; fmt++; }
        switch (*fmt) {
        case 'd': case 'i': {
            i64 v = lng ? va_arg(ap, i64) : va_arg(ap, int);
            out_num(o, v < 0 ? -(u64)v : (u64)v, 10, v < 0, width, zero, left, false);
            break;
        }
        case 'u': out_num(o, lng ? va_arg(ap, u64) : va_arg(ap, u32), 10, false, width, zero, left, false); break;
        case 'x': out_num(o, lng ? va_arg(ap, u64) : va_arg(ap, u32), 16, false, width, zero, left, false); break;
        case 'X': out_num(o, lng ? va_arg(ap, u64) : va_arg(ap, u32), 16, false, width, zero, left, true); break;
        case 'p': out_c(o, '0'); out_c(o, 'x'); out_num(o, (u64)va_arg(ap, void*), 16, false, 16, true, false, false); break;
        case 'c': out_c(o, (char)va_arg(ap, int)); break;
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int l = (int)strlen(s);
            if (prec >= 0 && l > prec) l = prec;
            if (!left) for (int i = l; i < width; i++) out_c(o, ' ');
            for (int i = 0; i < l; i++) out_c(o, s[i]);
            if (left) for (int i = l; i < width; i++) out_c(o, ' ');
            break;
        }
        case '%': out_c(o, '%'); break;
        case 0: fmt--; break;
        default: out_c(o, '%'); out_c(o, *fmt); break;
        }
    }
    if (n) buf[o.pos < n ? o.pos : n - 1] = 0;
    return (int)o.pos;
}

int snprintf(char* buf, size_t n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
