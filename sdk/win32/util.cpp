// win32run - strings, UTF-8/UTF-16 and the runtime's own messages
#include "rt.h"

namespace rt {

bool opt_strict = false;
bool opt_trace = false;

usize slen(const char* s) { usize n = 0; while (s[n]) n++; return n; }
usize wlen(const WCHAR* s) { usize n = 0; while (s[n]) n++; return n; }
int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

int scmp(const char* a, const char* b) {
    while (*a && *a == *b) a++, b++;
    return (u8)*a - (u8)*b;
}
int sicmp(const char* a, const char* b) {
    while (*a && lower((u8)*a) == lower((u8)*b)) a++, b++;
    return lower((u8)*a) - lower((u8)*b);
}
int sncmp(const char* a, const char* b, usize n) {
    for (; n; n--, a++, b++) { if (*a != *b) return (u8)*a - (u8)*b; if (!*a) break; }
    return 0;
}
int snicmp(const char* a, const char* b, usize n) {
    for (; n; n--, a++, b++) { int x = lower((u8)*a), y = lower((u8)*b); if (x != y) return x - y; if (!x) break; }
    return 0;
}
int wcmp(const WCHAR* a, const WCHAR* b) {
    while (*a && *a == *b) a++, b++;
    return (int)*a - (int)*b;
}
int wicmp(const WCHAR* a, const WCHAR* b) {
    while (*a && lower(*a) == lower(*b)) a++, b++;
    return lower(*a) - lower(*b);
}
char* scpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }
void slcpy(char* d, const char* s, usize cap) {
    if (!cap) return;
    usize i = 0;
    for (; i + 1 < cap && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
void slcat(char* d, const char* s, usize cap) { usize n = slen(d); if (n < cap) slcpy(d + n, s, cap - n); }
char* sdup(const char* s) { usize n = slen(s) + 1; char* d = (char*)ialloc(n); if (d) memcpy(d, s, n); return d; }
WCHAR* wdup(const WCHAR* s) { usize n = (wlen(s) + 1) * 2; WCHAR* d = (WCHAR*)ialloc(n); if (d) memcpy(d, s, n); return d; }
const char* schr(const char* s, int c) { for (;; s++) { if (*s == (char)c) return s; if (!*s) return nullptr; } }
const char* srchr(const char* s, int c) { const char* r = nullptr; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return r; } }
const char* sfind(const char* hay, const char* needle) {
    usize n = slen(needle);
    if (!n) return hay;
    for (; *hay; hay++) if (*hay == *needle && !sncmp(hay, needle, n)) return hay;
    return nullptr;
}
void* izalloc(usize n) { void* p = ialloc(n); if (p) memset(p, 0, n); return p; }

// ---------------------------------------------------------------- UTF-8 <-> UTF-16
int utf8_from_wide(const WCHAR* w, int n, char* out, int cap) {
    if (n < 0) n = (int)wlen(w) + 1;
    int o = 0;
    for (int i = 0; i < n; i++) {
        u32 c = (u16)w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && (u16)w[i + 1] >= 0xDC00 && (u16)w[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + ((u16)w[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
        char b[4];
        int k;
        if (c < 0x80) { b[0] = (char)c; k = 1; }
        else if (c < 0x800) { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 63)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 63)); b[2] = (char)(0x80 | (c & 63)); k = 3; }
        else { b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 63)); b[2] = (char)(0x80 | ((c >> 6) & 63)); b[3] = (char)(0x80 | (c & 63)); k = 4; }
        if (cap) { if (o + k > cap) return 0; for (int j = 0; j < k; j++) out[o + j] = b[j]; }
        o += k;
    }
    return o;
}

int wide_from_utf8(const char* s, int n, WCHAR* out, int cap) {
    if (n < 0) n = (int)slen(s) + 1;
    int o = 0;
    for (int i = 0; i < n;) {
        u32 c = (u8)s[i++];
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (c >= 0x80) {
            if (!more || i + more > n) c = c >= 0x80 && more == 0 ? c : 0xFFFD;      // a stray byte is taken as Latin-1
            else {
                u32 v = c & (0x3F >> more);
                bool ok = true;
                for (int k = 0; k < more; k++) { u8 b = (u8)s[i + k]; if ((b & 0xC0) != 0x80) { ok = false; break; } v = (v << 6) | (b & 63); }
                if (ok) { c = v; i += more; }
            }
        }
        if (c >= 0x10000) {
            if (cap) { if (o + 2 > cap) return 0; c -= 0x10000; out[o] = (WCHAR)(0xD800 + (c >> 10)); out[o + 1] = (WCHAR)(0xDC00 + (c & 0x3FF)); }
            o += 2;
        } else {
            if (cap) { if (o + 1 > cap) return 0; out[o] = (WCHAR)c; }
            o++;
        }
    }
    return o;
}

char* utf8_dup(const WCHAR* w) {
    int n = utf8_from_wide(w, -1, nullptr, 0);
    char* s = (char*)ialloc((usize)n + 1);
    if (!s) return nullptr;
    utf8_from_wide(w, -1, s, n);
    return s;
}
WCHAR* wide_dup(const char* s) {
    int n = wide_from_utf8(s, -1, nullptr, 0);
    WCHAR* w = (WCHAR*)ialloc(((usize)n + 1) * 2);
    if (!w) return nullptr;
    wide_from_utf8(s, -1, w, n);
    return w;
}

// ---------------------------------------------------------------- messages
void con_out(const char* s, usize n) {
    u32 spins = 0;
    while (n) {
        int k = si::con_write(s, (int)(n > 4096 ? 4096 : n));
        if (k < 0) return;
        if (k == 0) { nap(spins++); continue; }           // the shell is behind: let it catch up
        s += k;
        n -= (usize)k;
    }
}

void msg(const char* fmt, ...) {
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    int n = snfmt(b, sizeof(b), "[win32run] ");
    n += vsnfmt(b + n, sizeof(b) - (usize)n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(b) - 2) n = (int)sizeof(b) - 2;
    b[n++] = '\n';
    con_out(b, (usize)n);
}

void trace(const char* fmt, ...) {
    if (!opt_trace) return;
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    int n = snfmt(b, sizeof(b), "[win32run] ");
    n += vsnfmt(b + n, sizeof(b) - (usize)n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(b) - 2) n = (int)sizeof(b) - 2;
    b[n++] = '\n';
    con_out(b, (usize)n);
}

void quit(int code) {
    files_flush_all();
    si::exit(code);
}

u64 now_ms() { return si::ticks(); }

void nap(u32 spins) {
    if (spins < 16) si::yield();
    else si::sleep(spins < 200 ? 1 : 4);
}

}  // namespace rt
