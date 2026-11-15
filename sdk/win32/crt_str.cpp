// win32run - the C runtime: strings (narrow and wide), character classes, number conversions,
// multibyte <-> wide conversions (UTF-8) and the locale functions ("C" locale, UTF-8 code page).
#include "crt.h"

namespace rt {

// ---------------------------------------------------------------- memory and narrow strings
static void* CDECL c_memcpy(void* d, const void* s, usize n) {
    u8* a = (u8*)d; const u8* b = (const u8*)s;
    if (n >= 16 && !(((u64)a | (u64)b) & 7)) { for (; n >= 8; n -= 8, a += 8, b += 8) *(u64*)a = *(const u64*)b; }
    while (n--) *a++ = *b++;
    return d;
}
static void* CDECL c_memmove(void* d, const void* s, usize n) {
    u8* a = (u8*)d; const u8* b = (const u8*)s;
    if (a == b || !n) return d;
    if (a < b || a >= b + n) return c_memcpy(d, s, n);
    a += n; b += n;
    while (n--) *--a = *--b;
    return d;
}
static void* CDECL c_memset(void* d, int c, usize n) {
    u8* a = (u8*)d;
    u64 v = (u8)c * 0x0101010101010101ull;
    while (n && ((u64)a & 7)) { *a++ = (u8)c; n--; }
    for (; n >= 8; n -= 8, a += 8) *(u64*)a = v;
    while (n--) *a++ = (u8)c;
    return d;
}
static int CDECL c_memcmp(const void* x, const void* y, usize n) {
    const u8 *a = (const u8*)x, *b = (const u8*)y;
    for (; n; n--, a++, b++) if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}
static void* CDECL c_memchr(const void* s, int c, usize n) { const u8* p = (const u8*)s; for (; n; n--, p++) if (*p == (u8)c) return (void*)p; return nullptr; }
static int CDECL c__memicmp(const void* x, const void* y, usize n) {
    const u8 *a = (const u8*)x, *b = (const u8*)y;
    for (; n; n--, a++, b++) { int p = lower(*a), q = lower(*b); if (p != q) return p - q; }
    return 0;
}
static void* CDECL c__memccpy(void* d, const void* s, int c, usize n) {
    u8* a = (u8*)d; const u8* b = (const u8*)s;
    for (; n; n--) { u8 v = *b++; *a++ = v; if (v == (u8)c) return a; }
    return nullptr;
}
static int CDECL c_memcpy_s(void* d, usize dn, const void* s, usize n) {
    if (!n) return 0;
    if (!d || !s || dn < n) { if (d && dn) c_memset(d, 0, dn); invalid_parameter(); return !d || !s ? EINVAL : ERANGE; }
    c_memcpy(d, s, n);
    return 0;
}
static int CDECL c_memmove_s(void* d, usize dn, const void* s, usize n) {
    if (!n) return 0;
    if (!d || !s || dn < n) { invalid_parameter(); return !d || !s ? EINVAL : ERANGE; }
    c_memmove(d, s, n);
    return 0;
}

static usize CDECL c_strlen(const char* s) { return slen(s); }
static usize CDECL c_strnlen(const char* s, usize n) { usize i = 0; while (i < n && s[i]) i++; return i; }
static char* CDECL c_strcpy(char* d, const char* s) { return scpy(d, s); }
static char* CDECL c_strncpy(char* d, const char* s, usize n) {
    usize i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
static char* CDECL c_strcat(char* d, const char* s) { scpy(d + slen(d), s); return d; }
static char* CDECL c_strncat(char* d, const char* s, usize n) {
    char* e = d + slen(d);
    usize i = 0;
    for (; i < n && s[i]; i++) e[i] = s[i];
    e[i] = 0;
    return d;
}
static int CDECL c_strcmp(const char* a, const char* b) { int r = scmp(a, b); return r < 0 ? -1 : r > 0; }
static int CDECL c_strncmp(const char* a, const char* b, usize n) { int r = sncmp(a, b, n); return r < 0 ? -1 : r > 0; }
static int CDECL c__stricmp(const char* a, const char* b) { return sicmp(a, b); }
static int CDECL c__strnicmp(const char* a, const char* b, usize n) { return snicmp(a, b, n); }
static int CDECL c__stricmp_l(const char* a, const char* b, void* l) { (void)l; return sicmp(a, b); }
static int CDECL c__strnicmp_l(const char* a, const char* b, usize n, void* l) { (void)l; return snicmp(a, b, n); }
static int CDECL c_strcoll(const char* a, const char* b) { return c_strcmp(a, b); }
static int CDECL c__stricoll(const char* a, const char* b) { return sicmp(a, b); }
static int CDECL c__strncoll(const char* a, const char* b, usize n) { return c_strncmp(a, b, n); }
static int CDECL c__strnicoll(const char* a, const char* b, usize n) { return snicmp(a, b, n); }
static usize CDECL c_strxfrm(char* d, const char* s, usize n) { usize l = slen(s); if (n > l) scpy(d, s); else if (n) { c_memcpy(d, s, n - 1); d[n - 1] = 0; } return l; }
static char* CDECL c_strchr(const char* s, int c) { return (char*)schr(s, c); }
static char* CDECL c_strrchr(const char* s, int c) { return (char*)srchr(s, c); }
static char* CDECL c_strstr(const char* h, const char* n) { return (char*)sfind(h, n); }
static usize CDECL c_strspn(const char* s, const char* set) { usize i = 0; while (s[i] && schr(set, s[i])) i++; return i; }
static usize CDECL c_strcspn(const char* s, const char* set) { usize i = 0; while (s[i] && !schr(set, s[i])) i++; return i; }
static char* CDECL c_strpbrk(const char* s, const char* set) { for (; *s; s++) if (schr(set, *s)) return (char*)s; return nullptr; }
static char* CDECL c__strdup(const char* s) { if (!s) return nullptr; usize n = slen(s) + 1; char* d = (char*)h_alloc(n); if (d) c_memcpy(d, s, n); return d; }
static char* CDECL c_strtok_s(char* s, const char* delim, char** ctx) {
    if (!s) s = *ctx;
    if (!s) return nullptr;
    s += c_strspn(s, delim);
    if (!*s) { *ctx = s; return nullptr; }
    char* e = s + c_strcspn(s, delim);
    if (*e) *e++ = 0;
    *ctx = e;
    return s;
}
static char* CDECL c_strtok(char* s, const char* delim) { return c_strtok_s(s, delim, &crt_thread()->strtok_next); }
static char* CDECL c__strlwr(char* s) { for (char* p = s; *p; p++) *p = (char)lower((u8)*p); return s; }
static char* CDECL c__strupr(char* s) { for (char* p = s; *p; p++) *p = (char)upper((u8)*p); return s; }
static int CDECL c__strlwr_s(char* s, usize n) { (void)n; c__strlwr(s); return 0; }
static int CDECL c__strupr_s(char* s, usize n) { (void)n; c__strupr(s); return 0; }
static char* CDECL c__strrev(char* s) { usize n = slen(s); for (usize i = 0; i < n / 2; i++) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; } return s; }
static char* CDECL c__strset(char* s, int c) { for (char* p = s; *p; p++) *p = (char)c; return s; }
static char* CDECL c__strnset(char* s, int c, usize n) { for (char* p = s; *p && n; p++, n--) *p = (char)c; return s; }

static int CDECL c_strcpy_s(char* d, usize n, const char* s) {
    if (!d || !n) { invalid_parameter(); return EINVAL; }
    if (!s) { d[0] = 0; invalid_parameter(); return EINVAL; }
    usize l = slen(s);
    if (l >= n) { d[0] = 0; invalid_parameter(); return ERANGE; }
    c_memcpy(d, s, l + 1);
    return 0;
}
static int CDECL c_strcat_s(char* d, usize n, const char* s) {
    if (!d || !n || !s) { invalid_parameter(); return EINVAL; }
    usize a = c_strnlen(d, n);
    if (a == n) { invalid_parameter(); return EINVAL; }
    usize b = slen(s);
    if (a + b >= n) { d[0] = 0; invalid_parameter(); return ERANGE; }
    c_memcpy(d + a, s, b + 1);
    return 0;
}
static const usize TRUNCATE = (usize)-1;
static int CDECL c_strncpy_s(char* d, usize n, const char* s, usize count) {
    if (!d || !n) { invalid_parameter(); return EINVAL; }
    if (!s) { d[0] = 0; if (!count) return 0; invalid_parameter(); return EINVAL; }
    usize l = c_strnlen(s, count == TRUNCATE ? n : count);
    if (l >= n) {
        if (count == TRUNCATE) { c_memcpy(d, s, n - 1); d[n - 1] = 0; return STRUNCATE; }
        d[0] = 0; invalid_parameter(); return ERANGE;
    }
    c_memcpy(d, s, l);
    d[l] = 0;
    return 0;
}
static int CDECL c_strncat_s(char* d, usize n, const char* s, usize count) {
    if (!d || !n) { invalid_parameter(); return EINVAL; }
    usize a = c_strnlen(d, n);
    if (a == n) { invalid_parameter(); return EINVAL; }
    usize l = c_strnlen(s, count == TRUNCATE ? n : count);
    if (a + l >= n) {
        if (count == TRUNCATE) { c_memcpy(d + a, s, n - a - 1); d[n - 1] = 0; return STRUNCATE; }
        d[0] = 0; invalid_parameter(); return ERANGE;
    }
    c_memcpy(d + a, s, l);
    d[a + l] = 0;
    return 0;
}
static usize CDECL c_strnlen_s(const char* s, usize n) { return s ? c_strnlen(s, n) : 0; }

// ---------------------------------------------------------------- wide strings
static usize CDECL c_wcslen(const WCHAR* s) { return wlen(s); }
static usize CDECL c_wcsnlen(const WCHAR* s, usize n) { usize i = 0; while (i < n && s[i]) i++; return i; }
static WCHAR* CDECL c_wcscpy(WCHAR* d, const WCHAR* s) { WCHAR* r = d; while ((*d++ = *s++)) {} return r; }
static WCHAR* CDECL c_wcsncpy(WCHAR* d, const WCHAR* s, usize n) { usize i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
static WCHAR* CDECL c_wcscat(WCHAR* d, const WCHAR* s) { c_wcscpy(d + wlen(d), s); return d; }
static WCHAR* CDECL c_wcsncat(WCHAR* d, const WCHAR* s, usize n) { WCHAR* e = d + wlen(d); usize i = 0; for (; i < n && s[i]; i++) e[i] = s[i]; e[i] = 0; return d; }
static int CDECL c_wcscmp(const WCHAR* a, const WCHAR* b) { int r = wcmp(a, b); return r < 0 ? -1 : r > 0; }
static int CDECL c_wcsncmp(const WCHAR* a, const WCHAR* b, usize n) {
    for (; n; n--, a++, b++) { if (*a != *b) return (u16)*a < (u16)*b ? -1 : 1; if (!*a) break; }
    return 0;
}
static int CDECL c__wcsicmp(const WCHAR* a, const WCHAR* b) {
    for (;; a++, b++) { int x = (int)wlower((u16)*a), y = (int)wlower((u16)*b); if (x != y || !x) return x - y; }
}
static int CDECL c__wcsnicmp(const WCHAR* a, const WCHAR* b, usize n) {
    for (; n; n--, a++, b++) { int x = (int)wlower((u16)*a), y = (int)wlower((u16)*b); if (x != y) return x - y; if (!x) break; }
    return 0;
}
static int CDECL c_wcscoll(const WCHAR* a, const WCHAR* b) { return c_wcscmp(a, b); }
static int CDECL c__wcsicoll(const WCHAR* a, const WCHAR* b) { return c__wcsicmp(a, b); }
static usize CDECL c_wcsxfrm(WCHAR* d, const WCHAR* s, usize n) { usize l = wlen(s); if (n > l) c_wcscpy(d, s); return l; }
static WCHAR* CDECL c_wcschr(const WCHAR* s, WCHAR c) { for (;; s++) { if (*s == c) return (WCHAR*)s; if (!*s) return nullptr; } }
static WCHAR* CDECL c_wcsrchr(const WCHAR* s, WCHAR c) { const WCHAR* r = nullptr; for (;; s++) { if (*s == c) r = s; if (!*s) return (WCHAR*)r; } }
static WCHAR* CDECL c_wcsstr(const WCHAR* h, const WCHAR* n) {
    usize l = wlen(n);
    if (!l) return (WCHAR*)h;
    for (; *h; h++) if (!c_wcsncmp(h, n, l)) return (WCHAR*)h;
    return nullptr;
}
static usize CDECL c_wcsspn(const WCHAR* s, const WCHAR* set) { usize i = 0; while (s[i] && c_wcschr(set, s[i])) i++; return i; }
static usize CDECL c_wcscspn(const WCHAR* s, const WCHAR* set) { usize i = 0; while (s[i] && !c_wcschr(set, s[i])) i++; return i; }
static WCHAR* CDECL c_wcspbrk(const WCHAR* s, const WCHAR* set) { for (; *s; s++) if (c_wcschr(set, *s)) return (WCHAR*)s; return nullptr; }
static WCHAR* CDECL c__wcsdup(const WCHAR* s) { if (!s) return nullptr; usize n = (wlen(s) + 1) * 2; WCHAR* d = (WCHAR*)h_alloc(n); if (d) c_memcpy(d, s, n); return d; }
static WCHAR* CDECL c_wcstok_s(WCHAR* s, const WCHAR* delim, WCHAR** ctx) {
    if (!s) s = *ctx;
    if (!s) return nullptr;
    s += c_wcsspn(s, delim);
    if (!*s) { *ctx = s; return nullptr; }
    WCHAR* e = s + c_wcscspn(s, delim);
    if (*e) *e++ = 0;
    *ctx = e;
    return s;
}
static WCHAR* CDECL c_wcstok(WCHAR* s, const WCHAR* delim, WCHAR** ctx) { return c_wcstok_s(s, delim, ctx ? ctx : &crt_thread()->wcstok_next); }
static WCHAR* CDECL c__wcstok(WCHAR* s, const WCHAR* delim) { return c_wcstok_s(s, delim, &crt_thread()->wcstok_next); }
static WCHAR* CDECL c__wcslwr(WCHAR* s) { for (WCHAR* p = s; *p; p++) *p = (WCHAR)wlower((u16)*p); return s; }
static WCHAR* CDECL c__wcsupr(WCHAR* s) { for (WCHAR* p = s; *p; p++) *p = (WCHAR)wupper((u16)*p); return s; }
static int CDECL c__wcslwr_s(WCHAR* s, usize n) { (void)n; c__wcslwr(s); return 0; }
static int CDECL c__wcsupr_s(WCHAR* s, usize n) { (void)n; c__wcsupr(s); return 0; }
static WCHAR* CDECL c__wcsrev(WCHAR* s) { usize n = wlen(s); for (usize i = 0; i < n / 2; i++) { WCHAR t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; } return s; }
static WCHAR* CDECL c__wcsset(WCHAR* s, WCHAR c) { for (WCHAR* p = s; *p; p++) *p = c; return s; }
static WCHAR* CDECL c_wmemcpy(WCHAR* d, const WCHAR* s, usize n) { c_memcpy(d, s, n * 2); return d; }
static WCHAR* CDECL c_wmemmove(WCHAR* d, const WCHAR* s, usize n) { c_memmove(d, s, n * 2); return d; }
static WCHAR* CDECL c_wmemset(WCHAR* d, WCHAR c, usize n) { for (usize i = 0; i < n; i++) d[i] = c; return d; }
static int CDECL c_wmemcmp(const WCHAR* a, const WCHAR* b, usize n) { for (; n; n--, a++, b++) if (*a != *b) return (u16)*a < (u16)*b ? -1 : 1; return 0; }
static WCHAR* CDECL c_wmemchr(const WCHAR* s, WCHAR c, usize n) { for (; n; n--, s++) if (*s == c) return (WCHAR*)s; return nullptr; }
static int CDECL c_wcscpy_s(WCHAR* d, usize n, const WCHAR* s) {
    if (!d || !n) { invalid_parameter(); return EINVAL; }
    if (!s) { d[0] = 0; invalid_parameter(); return EINVAL; }
    usize l = wlen(s);
    if (l >= n) { d[0] = 0; invalid_parameter(); return ERANGE; }
    c_memcpy(d, s, (l + 1) * 2);
    return 0;
}
static int CDECL c_wcscat_s(WCHAR* d, usize n, const WCHAR* s) {
    if (!d || !n || !s) { invalid_parameter(); return EINVAL; }
    usize a = c_wcsnlen(d, n), b = wlen(s);
    if (a == n) { invalid_parameter(); return EINVAL; }
    if (a + b >= n) { d[0] = 0; invalid_parameter(); return ERANGE; }
    c_memcpy(d + a, s, (b + 1) * 2);
    return 0;
}
static int CDECL c_wcsncpy_s(WCHAR* d, usize n, const WCHAR* s, usize count) {
    if (!d || !n) { invalid_parameter(); return EINVAL; }
    if (!s) { d[0] = 0; return count ? EINVAL : 0; }
    usize l = c_wcsnlen(s, count == TRUNCATE ? n : count);
    if (l >= n) {
        if (count == TRUNCATE) { c_memcpy(d, s, (n - 1) * 2); d[n - 1] = 0; return STRUNCATE; }
        d[0] = 0; invalid_parameter(); return ERANGE;
    }
    c_memcpy(d, s, l * 2);
    d[l] = 0;
    return 0;
}
static int CDECL c_wcsncat_s(WCHAR* d, usize n, const WCHAR* s, usize count) {
    if (!d || !n) { invalid_parameter(); return EINVAL; }
    usize a = c_wcsnlen(d, n);
    usize l = c_wcsnlen(s, count == TRUNCATE ? n : count);
    if (a + l >= n) {
        if (count == TRUNCATE) { c_memcpy(d + a, s, (n - a - 1) * 2); d[n - 1] = 0; return STRUNCATE; }
        d[0] = 0; invalid_parameter(); return ERANGE;
    }
    c_memcpy(d + a, s, l * 2);
    d[a + l] = 0;
    return 0;
}
static int CDECL c_wmemcpy_s(WCHAR* d, usize dn, const WCHAR* s, usize n) { return c_memcpy_s(d, dn * 2, s, n * 2); }
static int CDECL c_wmemmove_s(WCHAR* d, usize dn, const WCHAR* s, usize n) { return c_memmove_s(d, dn * 2, s, n * 2); }

// ---------------------------------------------------------------- character classes
enum { CT_UPPER = 1, CT_LOWER = 2, CT_DIGIT = 4, CT_SPACE = 8, CT_PUNCT = 0x10, CT_CONTROL = 0x20, CT_BLANK = 0x40, CT_HEX = 0x80, CT_ALPHA = 0x103 };
static u16 ctype_table[257];                                     // [0] is EOF; Microsoft's _pctype points at [1]
static bool ctype_ready = false;
static void ctype_init() {
    if (ctype_ready) return;
    for (int c = 0; c < 256; c++) {
        u16 t = 0;
        if (c < 128) {
            if (c >= 'A' && c <= 'Z') t |= CT_UPPER | 0x100;
            if (c >= 'a' && c <= 'z') t |= CT_LOWER | 0x100;
            if (c >= '0' && c <= '9') t |= CT_DIGIT | CT_HEX;
            if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) t |= CT_HEX;
            if (c == ' ' || (c >= 9 && c <= 13)) t |= CT_SPACE;
            if (c == ' ' || c == '\t') t |= CT_BLANK;
            if (c < 32 || c == 127) t |= CT_CONTROL;
            if (c > 32 && c < 127 && !(t & (0x100 | CT_DIGIT))) t |= CT_PUNCT;
        }
        ctype_table[c + 1] = t;
    }
    ctype_ready = true;
}
static inline u16 ct(int c) { ctype_init(); return c >= -1 && c < 256 ? ctype_table[c + 1] : 0; }
static const u16* CDECL c___pctype_func() { ctype_init(); return ctype_table + 1; }
static const u16** CDECL c___p__pctype() { static const u16* p; ctype_init(); p = ctype_table + 1; return &p; }
static const u16* CDECL c___pwctype_func() { ctype_init(); return ctype_table + 1; }
static int CDECL c__isctype(int c, int mask) { return ct(c) & mask; }
static int CDECL c__isctype_l(int c, int mask, void* l) { (void)l; return ct(c) & mask; }
static int CDECL c_isalpha(int c) { return ct(c) & 0x103; }
static int CDECL c_isupper(int c) { return ct(c) & CT_UPPER; }
static int CDECL c_islower(int c) { return ct(c) & CT_LOWER; }
static int CDECL c_isdigit(int c) { return ct(c) & CT_DIGIT; }
static int CDECL c_isxdigit(int c) { return ct(c) & CT_HEX; }
static int CDECL c_isspace(int c) { return ct(c) & CT_SPACE; }
static int CDECL c_ispunct(int c) { return ct(c) & CT_PUNCT; }
static int CDECL c_isalnum(int c) { return ct(c) & (0x103 | CT_DIGIT); }
static int CDECL c_isprint(int c) { return (ct(c) & (0x103 | CT_DIGIT | CT_PUNCT)) || c == ' '; }
static int CDECL c_isgraph(int c) { return ct(c) & (0x103 | CT_DIGIT | CT_PUNCT); }
static int CDECL c_iscntrl(int c) { return ct(c) & CT_CONTROL; }
static int CDECL c_isblank(int c) { return c == ' ' || c == '\t'; }
static int CDECL c___isascii(int c) { return (unsigned)c < 128; }
static int CDECL c___iscsym(int c) { return c_isalnum(c) || c == '_'; }
static int CDECL c___iscsymf(int c) { return c_isalpha(c) || c == '_'; }
static int CDECL c_toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int CDECL c_tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int CDECL c__toupper(int c) { return c - 32; }
static int CDECL c__tolower(int c) { return c + 32; }
static int CDECL c___toascii(int c) { return c & 0x7F; }
static int CDECL c__toupper_l(int c, void* l) { (void)l; return c_toupper(c); }
static int CDECL c__tolower_l(int c, void* l) { (void)l; return c_tolower(c); }
static int CDECL c_iswctype(WCHAR c, u16 mask) {
    u16 t = ctype1((u16)c);
    u16 r = 0;
    if (t & 1) r |= CT_UPPER;
    if (t & 2) r |= CT_LOWER;
    if (t & 4) r |= CT_DIGIT;
    if (t & 8) r |= CT_SPACE;
    if (t & 0x10) r |= CT_PUNCT;
    if (t & 0x20) r |= CT_CONTROL;
    if (t & 0x40) r |= CT_BLANK;
    if (t & 0x80) r |= CT_HEX;
    if (t & 0x100) r |= 0x100;
    return r & mask;
}
static int CDECL c_iswalpha(WCHAR c) { return c_iswctype(c, 0x103); }
static int CDECL c_iswupper(WCHAR c) { return c_iswctype(c, CT_UPPER); }
static int CDECL c_iswlower(WCHAR c) { return c_iswctype(c, CT_LOWER); }
static int CDECL c_iswdigit(WCHAR c) { return c >= '0' && c <= '9'; }
static int CDECL c_iswxdigit(WCHAR c) { return c_iswctype(c, CT_HEX); }
static int CDECL c_iswspace(WCHAR c) { return c_iswctype(c, CT_SPACE); }
static int CDECL c_iswpunct(WCHAR c) { return c_iswctype(c, CT_PUNCT); }
static int CDECL c_iswalnum(WCHAR c) { return c_iswctype(c, 0x103 | CT_DIGIT); }
static int CDECL c_iswprint(WCHAR c) { return c_iswctype(c, 0x103 | CT_DIGIT | CT_PUNCT) || c == ' '; }
static int CDECL c_iswgraph(WCHAR c) { return c_iswctype(c, 0x103 | CT_DIGIT | CT_PUNCT); }
static int CDECL c_iswcntrl(WCHAR c) { return c_iswctype(c, CT_CONTROL); }
static int CDECL c_iswblank(WCHAR c) { return c == ' ' || c == '\t'; }
static int CDECL c_iswascii(WCHAR c) { return (u16)c < 128; }
static int CDECL c_isleadbyte(int c) { (void)c; return 0; }
static WCHAR CDECL c_towupper(WCHAR c) { return (WCHAR)wupper((u16)c); }
static WCHAR CDECL c_towlower(WCHAR c) { return (WCHAR)wlower((u16)c); }
static u32 CDECL c_wctype(const char* n) {
    static const struct { const char* n; u32 m; } t[] = {{"alnum", 0x107}, {"alpha", 0x103}, {"blank", CT_BLANK}, {"cntrl", CT_CONTROL},
        {"digit", CT_DIGIT}, {"graph", 0x117}, {"lower", CT_LOWER}, {"print", 0x157}, {"punct", CT_PUNCT}, {"space", CT_SPACE},
        {"upper", CT_UPPER}, {"xdigit", CT_HEX}};
    for (auto& e : t) if (!scmp(e.n, n)) return e.m;
    return 0;
}

// ---------------------------------------------------------------- numbers from text
template <typename C>
static u64 parse_int(const C* s, const C** end, int base, bool* neg, bool* overflow, bool unsign, u64 limit) {
    const C* p = s;
    while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
    *neg = false;
    *overflow = false;
    if (*p == '+' || *p == '-') *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        ((p[2] >= '0' && p[2] <= '9') || ((p[2] | 32) >= 'a' && (p[2] | 32) <= 'f'))) { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') base = 8;
    else if (base == 0) base = 10;
    if (base < 2 || base > 36) { if (end) *end = s; return 0; }
    u64 v = 0;
    const C* start = p;
    for (;; p++) {
        int c = (int)*p, d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if ((c | 32) >= 'a' && (c | 32) <= 'z') d = (c | 32) - 'a' + 10;
        else break;
        if (d >= base) break;
        if (v > (limit - (u64)d) / (u64)base) *overflow = true;
        else v = v * (u64)base + (u64)d;
    }
    if (p == start) { if (end) *end = s; return 0; }
    if (end) *end = p;
    (void)unsign;
    return v;
}
template <typename C, typename T, bool S>
static T to_int(const C* s, C** end, int base) {
    bool neg, ovf;
    const C* e;
    u64 limit = S ? (u64)(T)(((u64)1 << (sizeof(T) * 8 - 1)) - 1) + 1 : (u64)(T)-1;
    u64 v = parse_int(s, &e, base, &neg, &ovf, !S, limit);
    if (end) *end = (C*)e;
    if (S) {
        u64 maxpos = limit - 1;
        if (ovf || (!neg && v > maxpos)) { set_errno(ERANGE); return neg ? (T)(-(i64)maxpos - 1) : (T)maxpos; }
        return neg ? (T)(0 - v) : (T)v;
    }
    if (ovf) { set_errno(ERANGE); return (T)-1; }
    return neg ? (T)(0 - v) : (T)v;
}
static i32 CDECL c_strtol(const char* s, char** e, int b) { return to_int<char, i32, true>(s, e, b); }
static u32 CDECL c_strtoul(const char* s, char** e, int b) { return to_int<char, u32, false>(s, e, b); }
static i64 CDECL c_strtoll(const char* s, char** e, int b) { return to_int<char, i64, true>(s, e, b); }
static u64 CDECL c_strtoull(const char* s, char** e, int b) { return to_int<char, u64, false>(s, e, b); }
static i32 CDECL c_wcstol(const WCHAR* s, WCHAR** e, int b) { return to_int<WCHAR, i32, true>(s, e, b); }
static u32 CDECL c_wcstoul(const WCHAR* s, WCHAR** e, int b) { return to_int<WCHAR, u32, false>(s, e, b); }
static i64 CDECL c_wcstoll(const WCHAR* s, WCHAR** e, int b) { return to_int<WCHAR, i64, true>(s, e, b); }
static u64 CDECL c_wcstoull(const WCHAR* s, WCHAR** e, int b) { return to_int<WCHAR, u64, false>(s, e, b); }
static i32 CDECL c_strtol_l(const char* s, char** e, int b, void* l) { (void)l; return c_strtol(s, e, b); }
static int CDECL c_atoi(const char* s) { return (int)to_int<char, i32, true>(s, nullptr, 10); }
static i32 CDECL c_atol(const char* s) { return to_int<char, i32, true>(s, nullptr, 10); }
static i64 CDECL c_atoll(const char* s) { return to_int<char, i64, true>(s, nullptr, 10); }
static int CDECL c__wtoi(const WCHAR* s) { return (int)to_int<WCHAR, i32, true>(s, nullptr, 10); }
static i32 CDECL c__wtol(const WCHAR* s) { return to_int<WCHAR, i32, true>(s, nullptr, 10); }
static i64 CDECL c__wtoll(const WCHAR* s) { return to_int<WCHAR, i64, true>(s, nullptr, 10); }

static double check_range(double v) {
    u64 b;
    c_memcpy(&b, &v, 8);
    if (((b >> 52) & 0x7FF) == 0x7FF && !(b & 0xFFFFFFFFFFFFFull)) set_errno(ERANGE);
    return v;
}
static double CDECL c_strtod(const char* s, char** e) {
    const char* end;
    double v = parse_double(s, &end);
    if (e) *e = (char*)end;
    if (end != s) { const char* p = s; while (*p == ' ' || (*p >= 9 && *p <= 13)) p++; if (*p != 'i' && *p != 'I' && !(p[0] == '-' && (p[1] | 32) == 'i') && !(p[0] == '+' && (p[1] | 32) == 'i')) check_range(v); }
    return v;
}
static float CDECL c_strtof(const char* s, char** e) { return (float)c_strtod(s, e); }
static double CDECL c_wcstod(const WCHAR* s, WCHAR** e) { const WCHAR* end; double v = wparse_double(s, &end); if (e) *e = (WCHAR*)end; return v; }
static float CDECL c_wcstof(const WCHAR* s, WCHAR** e) { return (float)c_wcstod(s, e); }
static double CDECL c_atof(const char* s) { return parse_double(s, nullptr); }
static double CDECL c__wtof(const WCHAR* s) { return wparse_double(s, nullptr); }
static double CDECL c__strtod_l(const char* s, char** e, void* l) { (void)l; return c_strtod(s, e); }
static int CDECL c__atoflt(float* out, const char* s) { *out = (float)parse_double(s, nullptr); return 0; }
static int CDECL c__atodbl(double* out, char* s) { *out = parse_double(s, nullptr); return 0; }

// ---------------------------------------------------------------- numbers to text
template <typename C>
static C* utoa_any(u64 v, C* out, int radix, bool neg) {
    char tmp[72];
    int n = 0;
    do { int d = (int)(v % (u64)radix); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (u64)radix; } while (v);
    int o = 0;
    if (neg) out[o++] = '-';
    while (n) out[o++] = (C)tmp[--n];
    out[o] = 0;
    return out;
}
static char* CDECL c__itoa(int v, char* o, int r) { return utoa_any<char>(r == 10 && v < 0 ? (u64)(-(i64)v) : (u64)(u32)v, o, r, r == 10 && v < 0); }
static char* CDECL c__ltoa(i32 v, char* o, int r) { return c__itoa(v, o, r); }
static char* CDECL c__ultoa(u32 v, char* o, int r) { return utoa_any<char>(v, o, r, false); }
static char* CDECL c__i64toa(i64 v, char* o, int r) { return utoa_any<char>(r == 10 && v < 0 ? (u64)0 - (u64)v : (u64)v, o, r, r == 10 && v < 0); }
static char* CDECL c__ui64toa(u64 v, char* o, int r) { return utoa_any<char>(v, o, r, false); }
static WCHAR* CDECL c__itow(int v, WCHAR* o, int r) { return utoa_any<WCHAR>(r == 10 && v < 0 ? (u64)(-(i64)v) : (u64)(u32)v, o, r, r == 10 && v < 0); }
static WCHAR* CDECL c__ltow(i32 v, WCHAR* o, int r) { return c__itow(v, o, r); }
static WCHAR* CDECL c__ultow(u32 v, WCHAR* o, int r) { return utoa_any<WCHAR>(v, o, r, false); }
static WCHAR* CDECL c__i64tow(i64 v, WCHAR* o, int r) { return utoa_any<WCHAR>(r == 10 && v < 0 ? (u64)0 - (u64)v : (u64)v, o, r, r == 10 && v < 0); }
static WCHAR* CDECL c__ui64tow(u64 v, WCHAR* o, int r) { return utoa_any<WCHAR>(v, o, r, false); }
static int CDECL c__itoa_s(int v, char* o, usize n, int r) { char b[72]; c__itoa(v, b, r); if (slen(b) >= n) { if (n) o[0] = 0; return ERANGE; } scpy(o, b); return 0; }
static int CDECL c__ltoa_s(i32 v, char* o, usize n, int r) { return c__itoa_s(v, o, n, r); }
static int CDECL c__ultoa_s(u32 v, char* o, usize n, int r) { char b[72]; c__ultoa(v, b, r); if (slen(b) >= n) { if (n) o[0] = 0; return ERANGE; } scpy(o, b); return 0; }
static int CDECL c__i64toa_s(i64 v, char* o, usize n, int r) { char b[72]; c__i64toa(v, b, r); if (slen(b) >= n) { if (n) o[0] = 0; return ERANGE; } scpy(o, b); return 0; }
static int CDECL c__ui64toa_s(u64 v, char* o, usize n, int r) { char b[72]; c__ui64toa(v, b, r); if (slen(b) >= n) { if (n) o[0] = 0; return ERANGE; } scpy(o, b); return 0; }
static int CDECL c__itow_s(int v, WCHAR* o, usize n, int r) { WCHAR b[72]; c__itow(v, b, r); if (wlen(b) >= n) { if (n) o[0] = 0; return ERANGE; } c_wcscpy(o, b); return 0; }
static char* CDECL c__gcvt(double v, int digits, char* buf) { snfmt(buf, 64, "%.*g", digits, v); return buf; }
static int CDECL c__gcvt_s(char* buf, usize n, double v, int digits) { snfmt(buf, n, "%.*g", digits, v); return 0; }
static char ecvt_buf[400];
static char* CDECL c__ecvt(double v, int nd, int* dec, int* sign) {
    char tmp[400];
    *sign = v < 0;
    if (v < 0) v = -v;
    if (nd < 1) nd = 1;
    if (nd > 340) nd = 340;
    snfmt(tmp, sizeof(tmp), "%.*e", nd - 1, v);
    int o = 0;
    for (const char* p = tmp; *p && *p != 'e'; p++) if (*p != '.') ecvt_buf[o++] = *p;
    ecvt_buf[o] = 0;
    const char* e = schr(tmp, 'e');
    *dec = v == 0 ? 0 : (e ? c_atoi(e + 1) + 1 : 1);
    return ecvt_buf;
}
static char* CDECL c__fcvt(double v, int nd, int* dec, int* sign) {
    char tmp[400];
    *sign = v < 0;
    if (v < 0) v = -v;
    if (nd > 300) nd = 300;
    snfmt(tmp, sizeof(tmp), "%.*f", nd < 0 ? 0 : nd, v);
    int o = 0, d = -1;
    const char* p = tmp;
    while (*p == '0' && p[1] && p[1] != '.') p++;
    for (; *p; p++) { if (*p == '.') { d = o; continue; } ecvt_buf[o++] = *p; }
    ecvt_buf[o] = 0;
    *dec = d < 0 ? o : d;
    if (ecvt_buf[0] == '0' && v < 1) {                           // leading zeros of a small number don't count as digits
        int z = 0;
        while (ecvt_buf[z] == '0') z++;
        c_memmove(ecvt_buf, ecvt_buf + z, slen(ecvt_buf + z) + 1);
        *dec -= z;
    }
    return ecvt_buf;
}

// ---------------------------------------------------------------- multibyte (UTF-8) <-> wide
struct mbstate { u32 v; };
static int decode_utf8(const char* s, usize n, u32* out) {          // bytes used; 0 for NUL; -1 bad; -2 incomplete
    if (!n) return -2;
    u8 c = (u8)s[0];
    if (!c) { *out = 0; return 0; }
    if (c < 0x80) { *out = c; return 1; }
    int more = c >= 0xF0 && c < 0xF5 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 && c < 0xE0 ? 1 : -1;
    if (more < 0) return -1;
    u32 v = c & (0x3F >> more);
    for (int i = 1; i <= more; i++) {
        if ((usize)i >= n) return -2;
        u8 b = (u8)s[i];
        if ((b & 0xC0) != 0x80) return -1;
        v = (v << 6) | (b & 0x3F);
    }
    *out = v;
    return more + 1;
}
static int encode_utf8(u32 c, char* out) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 63)); out[2] = (char)(0x80 | (c & 63)); return 3; }
    out[0] = (char)(0xF0 | (c >> 18)); out[1] = (char)(0x80 | ((c >> 12) & 63)); out[2] = (char)(0x80 | ((c >> 6) & 63)); out[3] = (char)(0x80 | (c & 63));
    return 4;
}
static int CDECL c_mbtowc(WCHAR* out, const char* s, usize n) {
    if (!s) return 0;
    u32 c;
    int k = decode_utf8(s, n, &c);
    if (k < 0) { set_errno(EILSEQ); return -1; }
    if (out) *out = (WCHAR)(c > 0xFFFF ? 0xFFFD : c);
    return k;
}
static int CDECL c_mblen(const char* s, usize n) { return c_mbtowc(nullptr, s, n); }
static int CDECL c_wctomb(char* out, WCHAR c) { if (!out) return 0; return encode_utf8((u16)c, out); }
static int CDECL c_wctomb_s(int* r, char* out, usize n, WCHAR c) { char b[4]; int k = encode_utf8((u16)c, b); if (out) { if ((usize)k > n) return ERANGE; c_memcpy(out, b, (usize)k); } if (r) *r = k; return 0; }
static usize CDECL c_mbrtowc(WCHAR* out, const char* s, usize n, mbstate* st) {
    (void)st;
    if (!s) return 0;
    u32 c;
    int k = decode_utf8(s, n, &c);
    if (k == -2) return (usize)-2;
    if (k < 0) { set_errno(EILSEQ); return (usize)-1; }
    if (out) *out = (WCHAR)(c > 0xFFFF ? 0xFFFD : c);
    return (usize)k;
}
static usize CDECL c_mbrlen(const char* s, usize n, mbstate* st) { return c_mbrtowc(nullptr, s, n, st); }
static usize CDECL c_wcrtomb(char* out, WCHAR c, mbstate* st) { (void)st; if (!out) return 1; return (usize)encode_utf8((u16)c, out); }
static int CDECL c_wcrtomb_s(usize* r, char* out, usize n, WCHAR c, mbstate* st) { (void)st; char b[4]; int k = encode_utf8((u16)c, b); if (out) { if ((usize)k > n) return ERANGE; c_memcpy(out, b, (usize)k); } if (r) *r = (usize)k; return 0; }
static int CDECL c_mbsinit(const mbstate* st) { return !st || !st->v; }
static WCHAR CDECL c_btowc(int c) { return c >= 0 && c < 0x80 ? (WCHAR)c : (WCHAR)0xFFFF; }
static int CDECL c_wctob(WCHAR c) { return (u16)c < 0x80 ? (int)c : -1; }
static usize CDECL c_mbstowcs(WCHAR* out, const char* s, usize n) {
    if (!out) { int k = wide_from_utf8(s, -1, nullptr, 0); return (usize)(k - 1); }
    usize o = 0;
    while (o < n) {
        u32 c;
        int k = decode_utf8(s, 4, &c);
        if (k < 0) { set_errno(EILSEQ); return (usize)-1; }
        if (!k) { out[o] = 0; return o; }
        if (c >= 0x10000) { if (o + 2 > n) break; c -= 0x10000; out[o++] = (WCHAR)(0xD800 + (c >> 10)); out[o++] = (WCHAR)(0xDC00 + (c & 0x3FF)); }
        else out[o++] = (WCHAR)c;
        s += k;
    }
    return o;
}
static usize CDECL c_wcstombs(char* out, const WCHAR* s, usize n) {
    if (!out) { int k = utf8_from_wide(s, -1, nullptr, 0); return (usize)(k - 1); }
    usize o = 0;
    for (usize i = 0; o < n; i++) {
        u32 c = (u16)s[i];
        if (!c) { out[o] = 0; return o; }
        if (c >= 0xD800 && c < 0xDC00 && (u16)s[i + 1] >= 0xDC00 && (u16)s[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + ((u16)s[i + 1] - 0xDC00); i++; }
        char b[4];
        int k = encode_utf8(c, b);
        if (o + (usize)k > n) break;
        c_memcpy(out + o, b, (usize)k);
        o += (usize)k;
    }
    return o;
}
static int CDECL c_mbstowcs_s(usize* r, WCHAR* out, usize cap, const char* s, usize count) {
    usize need = c_mbstowcs(nullptr, s, 0);
    if (count != TRUNCATE && count < need) need = count;
    if (!out) { if (r) *r = need + 1; return 0; }
    if (need >= cap && count != TRUNCATE) { out[0] = 0; return ERANGE; }
    usize k = c_mbstowcs(out, s, need < cap - 1 ? need : cap - 1);
    out[k] = 0;
    if (r) *r = k + 1;
    return need >= cap ? STRUNCATE : 0;
}
static int CDECL c_wcstombs_s(usize* r, char* out, usize cap, const WCHAR* s, usize count) {
    usize need = c_wcstombs(nullptr, s, 0);
    if (count != TRUNCATE && count < need) need = count;
    if (!out) { if (r) *r = need + 1; return 0; }
    if (need >= cap && count != TRUNCATE) { out[0] = 0; return ERANGE; }
    usize k = c_wcstombs(out, s, need < cap - 1 ? need : cap - 1);
    out[k] = 0;
    if (r) *r = k + 1;
    return need >= cap ? STRUNCATE : 0;
}
static usize CDECL c_mbsrtowcs(WCHAR* out, const char** s, usize n, mbstate* st) {
    (void)st;
    usize k = c_mbstowcs(out, *s, n);
    if (out && k != (usize)-1) { if (k < n) *s = nullptr; else { const char* p = *s; for (usize i = 0; i < k; i++) { u32 c; int m = decode_utf8(p, 4, &c); p += m > 0 ? m : 1; } *s = p; } }
    return k;
}
static usize CDECL c_wcsrtombs(char* out, const WCHAR** s, usize n, mbstate* st) {
    (void)st;
    usize k = c_wcstombs(out, *s, n);
    if (out && k != (usize)-1 && k < n) *s = nullptr;
    return k;
}
static usize CDECL c__mbstrlen(const char* s) { usize n = 0; while (*s) { u32 c; int k = decode_utf8(s, 4, &c); s += k > 0 ? k : 1; n++; } return n; }
static usize CDECL c_c16rtomb(char* out, u16 c, mbstate* st) { return c_wcrtomb(out, (WCHAR)c, st); }
static usize CDECL c_mbrtoc16(u16* out, const char* s, usize n, mbstate* st) { return c_mbrtowc((WCHAR*)out, s, n, st); }
static usize CDECL c_c32rtomb(char* out, u32 c, mbstate* st) { (void)st; if (!out) return 1; return (usize)encode_utf8(c, out); }
static usize CDECL c_mbrtoc32(u32* out, const char* s, usize n, mbstate* st) {
    (void)st;
    if (!s) return 0;
    u32 c;
    int k = decode_utf8(s, n, &c);
    if (k == -2) return (usize)-2;
    if (k < 0) { set_errno(EILSEQ); return (usize)-1; }
    if (out) *out = c;
    return (usize)k;
}

// ---------------------------------------------------------------- locale: the "C" locale with UTF-8 text
struct lconv {
    char* decimal_point; char* thousands_sep; char* grouping; char* int_curr_symbol; char* currency_symbol; char* mon_decimal_point;
    char* mon_thousands_sep; char* mon_grouping; char* positive_sign; char* negative_sign; char int_frac_digits, frac_digits, p_cs_precedes,
    p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn;
    WCHAR* _W_decimal_point; WCHAR* _W_thousands_sep; WCHAR* _W_int_curr_symbol; WCHAR* _W_currency_symbol; WCHAR* _W_mon_decimal_point;
    WCHAR* _W_mon_thousands_sep; WCHAR* _W_positive_sign; WCHAR* _W_negative_sign;
};
static char s_dot[] = ".", s_empty[] = "";
static WCHAR w_dot[] = L".", w_empty[] = L"";
static lconv c_lconv = {s_dot, s_empty, s_empty, s_empty, s_empty, s_empty, s_empty, s_empty, s_empty, s_empty,
                        127, 127, 127, 127, 127, 127, 127, 127, w_dot, w_empty, w_empty, w_empty, w_empty, w_empty, w_empty, w_empty};
static lconv* CDECL c_localeconv() { return &c_lconv; }
static char cur_locale[64] = "C";
static char* CDECL c_setlocale(int cat, const char* loc) {
    (void)cat;
    if (!loc) return cur_locale;
    if (!loc[0]) slcpy(cur_locale, "English_United States.utf8", sizeof(cur_locale));
    else slcpy(cur_locale, loc, sizeof(cur_locale));
    return cur_locale;
}
static WCHAR cur_wlocale[64] = L"C";
static WCHAR* CDECL c__wsetlocale(int cat, const WCHAR* loc) {
    (void)cat;
    if (loc) { usize n = wlen(loc); if (n > 63) n = 63; c_memcpy(cur_wlocale, loc, n * 2); cur_wlocale[n] = 0; }
    return cur_wlocale;
}
struct LocaleInfo { void* locinfo; void* mbcinfo; };
static LocaleInfo the_locale;
static void* CDECL c__create_locale(int cat, const char* loc) { (void)cat; (void)loc; return &the_locale; }
static void* CDECL c__wcreate_locale(int cat, const WCHAR* loc) { (void)cat; (void)loc; return &the_locale; }
static void CDECL c__free_locale(void* l) { (void)l; }
static void* CDECL c__get_current_locale() { return &the_locale; }
static u32 CDECL c____lc_codepage_func() { return 65001; }
static u32 CDECL c____lc_collate_cp_func() { return 65001; }
static WCHAR** CDECL c____lc_locale_name_func() { static WCHAR* names[6] = {nullptr}; return names; }
static int CDECL c____mb_cur_max_func() { return 1; }
static int CDECL c____mb_cur_max_l_func(void* l) { (void)l; return 1; }
static int* CDECL c___p___mb_cur_max() { static int n = 1; return &n; }
static int c___mb_cur_max = 1;
static int CDECL c__getmbcp() { return 65001; }
static int CDECL c__setmbcp(int cp) { (void)cp; return 0; }
static char* CDECL c__Getdays() { return c__strdup(":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:Thu:Thursday:Fri:Friday:Sat:Saturday"); }
static char* CDECL c__Getmonths() { return c__strdup(":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:Jul:July:Aug:August:Sep:September:Oct:October:Nov:November:Dec:December"); }
static void* CDECL c__Gettnames() { return h_alloc(256, true); }
static int CDECL c___iswcsymf(WCHAR c) { return c_iswalpha(c) || c == '_'; }
static int CDECL c___iswcsym(WCHAR c) { return c_iswalnum(c) || c == '_'; }

// the multibyte-character functions: the code page is UTF-8, which has no lead bytes in the DBCS sense
static int CDECL c__ismbblead(u32 c) { (void)c; return 0; }
static int CDECL c__ismbbtrail(u32 c) { (void)c; return 0; }
static int CDECL c__ismbblead_l(u32 c, void* l) { (void)c; (void)l; return 0; }
static int CDECL c__ismbcspace(u32 c) { return c == ' ' || (c >= 9 && c <= 13); }
static int CDECL c__ismbcdigit(u32 c) { return c >= '0' && c <= '9'; }
static int CDECL c__ismbcalpha(u32 c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static usize CDECL c__mbclen(const u8* s) { u8 c = *s; return c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }
static u8* CDECL c__mbsinc(const u8* s) { return (u8*)s + c__mbclen(s); }
static u8* CDECL c__mbsdec(const u8* start, const u8* s) { if (s <= start) return nullptr; s--; while (s > start && (*s & 0xC0) == 0x80) s--; return (u8*)s; }
static usize CDECL c__mbslen(const u8* s) { usize n = 0; while (*s) { s += c__mbclen(s); n++; } return n; }
static u8* CDECL c__mbschr(const u8* s, u32 c) { return (u8*)schr((const char*)s, (int)c); }
static u8* CDECL c__mbsrchr(const u8* s, u32 c) { return (u8*)srchr((const char*)s, (int)c); }
static u8* CDECL c__mbsstr(const u8* a, const u8* b) { return (u8*)sfind((const char*)a, (const char*)b); }
static int CDECL c__mbscmp(const u8* a, const u8* b) { return scmp((const char*)a, (const char*)b); }
static int CDECL c__mbsicmp(const u8* a, const u8* b) { return sicmp((const char*)a, (const char*)b); }
static int CDECL c__mbsnbcmp(const u8* a, const u8* b, usize n) { return sncmp((const char*)a, (const char*)b, n); }
static int CDECL c__mbsnbicmp(const u8* a, const u8* b, usize n) { return snicmp((const char*)a, (const char*)b, n); }
static u8* CDECL c__mbscpy(u8* d, const u8* s) { return (u8*)scpy((char*)d, (const char*)s); }
static u8* CDECL c__mbsnbcpy(u8* d, const u8* s, usize n) { return (u8*)c_strncpy((char*)d, (const char*)s, n); }
static u8* CDECL c__mbscat(u8* d, const u8* s) { return (u8*)c_strcat((char*)d, (const char*)s); }
static u8* CDECL c__mbsupr(u8* s) { return (u8*)c__strupr((char*)s); }
static u8* CDECL c__mbslwr(u8* s) { return (u8*)c__strlwr((char*)s); }
static u8* CDECL c__mbstok(u8* s, const u8* d) { return (u8*)c_strtok((char*)s, (const char*)d); }
static usize CDECL c__mbsspn(const u8* s, const u8* set) { return c_strspn((const char*)s, (const char*)set); }
static usize CDECL c__mbscspn(const u8* s, const u8* set) { return c_strcspn((const char*)s, (const char*)set); }
static u8* CDECL c__mbspbrk(const u8* s, const u8* set) { return (u8*)c_strpbrk((const char*)s, (const char*)set); }
static u32 CDECL c__mbctoupper(u32 c) { return (u32)c_toupper((int)c); }
static u32 CDECL c__mbctolower(u32 c) { return (u32)c_tolower((int)c); }
static u8 mbctype_table[257];
static u8* CDECL c___p__mbctype() { return mbctype_table; }
static const Export str_exports[] = {
    X(_ismbblead), X(_ismbbtrail), X(_ismbblead_l), X(_ismbcspace), X(_ismbcdigit), X(_ismbcalpha), X(_mbclen), X(_mbsinc), X(_mbsdec),
    X(_mbslen), X(_mbschr), X(_mbsrchr), X(_mbsstr), X(_mbscmp), X(_mbsicmp), X(_mbsnbcmp), X(_mbsnbicmp), X(_mbscpy), X(_mbsnbcpy),
    X(_mbscat), X(_mbsupr), X(_mbslwr), X(_mbstok), X(_mbsspn), X(_mbscspn), X(_mbspbrk), X(_mbctoupper), X(_mbctolower), X(__p__mbctype),
    X(memcpy), X(memmove), X(memset), X(memcmp), X(memchr), X(_memicmp), X(_memccpy), X(memcpy_s), X(memmove_s),
    X(strlen), X(strnlen), X(strcpy), X(strncpy), X(strcat), X(strncat), X(strcmp), X(strncmp), X(_stricmp), X(_strnicmp),
    X(_stricmp_l), X(_strnicmp_l), EA("_strcmpi", c__stricmp), EA("stricmp", c__stricmp), EA("strcmpi", c__stricmp), EA("strnicmp", c__strnicmp),
    X(strcoll), X(_stricoll), X(_strncoll), X(_strnicoll), X(strxfrm), X(strchr), X(strrchr), X(strstr), X(strspn), X(strcspn),
    X(strpbrk), X(_strdup), EA("strdup", c__strdup), X(strtok), X(strtok_s), X(_strlwr), X(_strupr), EA("strlwr", c__strlwr),
    EA("strupr", c__strupr), X(_strlwr_s), X(_strupr_s), X(_strrev), X(_strset), X(_strnset), X(strcpy_s), X(strcat_s),
    X(strncpy_s), X(strncat_s), X(strnlen_s),
    X(wcslen), X(wcsnlen), X(wcscpy), X(wcsncpy), X(wcscat), X(wcsncat), X(wcscmp), X(wcsncmp), X(_wcsicmp), X(_wcsnicmp),
    EA("wcsicmp", c__wcsicmp), X(wcscoll), X(_wcsicoll), X(wcsxfrm), X(wcschr), X(wcsrchr), X(wcsstr), X(wcsspn), X(wcscspn),
    X(wcspbrk), X(_wcsdup), EA("wcsdup", c__wcsdup), X(wcstok), X(_wcstok), X(wcstok_s), X(_wcslwr), X(_wcsupr), X(_wcslwr_s),
    X(_wcsupr_s), X(_wcsrev), X(_wcsset), X(wmemcpy), X(wmemmove), X(wmemset), X(wmemcmp), X(wmemchr), X(wcscpy_s), X(wcscat_s),
    X(wcsncpy_s), X(wcsncat_s), X(wmemcpy_s), X(wmemmove_s),
    X(__pctype_func), X(__p__pctype), X(__pwctype_func), X(_isctype), X(_isctype_l), X(isalpha), X(isupper), X(islower), X(isdigit),
    X(isxdigit), X(isspace), X(ispunct), X(isalnum), X(isprint), X(isgraph), X(iscntrl), X(isblank), X(__isascii), EA("isascii", c___isascii),
    X(__iscsym), X(__iscsymf), X(toupper), X(tolower), X(_toupper), X(_tolower), X(__toascii), X(_toupper_l), X(_tolower_l),
    X(iswctype), EA("is_wctype", c_iswctype), X(iswalpha), X(iswupper), X(iswlower), X(iswdigit), X(iswxdigit), X(iswspace), X(iswpunct),
    X(iswalnum), X(iswprint), X(iswgraph), X(iswcntrl), X(iswblank), X(iswascii), X(isleadbyte), X(towupper), X(towlower), X(wctype),
    X(__iswcsymf), X(__iswcsym),
    X(strtol), X(strtoul), X(strtoll), X(strtoull), EA("_strtoi64", c_strtoll), EA("_strtoui64", c_strtoull), EA("strtoimax", c_strtoll),
    EA("strtoumax", c_strtoull), X(wcstol), X(wcstoul), X(wcstoll), X(wcstoull), EA("_wcstoi64", c_wcstoll), EA("_wcstoui64", c_wcstoull),
    EA("_strtol_l", c_strtol_l), X(atoi), X(atol), X(atoll), EA("_atoi64", c_atoll), X(_wtoi), X(_wtol), X(_wtoll), EA("_wtoi64", c__wtoll),
    X(strtod), X(strtof), EA("strtold", c_strtod), X(wcstod), X(wcstof), EA("wcstold", c_wcstod), X(atof), X(_wtof), X(_strtod_l),
    X(_atoflt), X(_atodbl),
    X(_itoa), X(_ltoa), X(_ultoa), X(_i64toa), X(_ui64toa), X(_itow), X(_ltow), X(_ultow), X(_i64tow), X(_ui64tow), EA("itoa", c__itoa),
    EA("ltoa", c__ltoa), X(_itoa_s), X(_ltoa_s), X(_ultoa_s), X(_i64toa_s), X(_ui64toa_s), X(_itow_s), X(_gcvt), X(_gcvt_s), X(_ecvt), X(_fcvt),
    X(mbtowc), X(mblen), X(wctomb), X(wctomb_s), X(mbrtowc), X(mbrlen), X(wcrtomb), X(wcrtomb_s), X(mbsinit), X(btowc), X(wctob),
    X(mbstowcs), X(wcstombs), X(mbstowcs_s), X(wcstombs_s), X(mbsrtowcs), X(wcsrtombs), X(_mbstrlen), X(c16rtomb), X(mbrtoc16),
    X(c32rtomb), X(mbrtoc32),
    X(localeconv), X(setlocale), X(_wsetlocale), X(_create_locale), X(_wcreate_locale), X(_free_locale), X(_get_current_locale),
    X(___lc_codepage_func), X(___lc_collate_cp_func), X(___lc_locale_name_func), X(___mb_cur_max_func), X(___mb_cur_max_l_func),
    X(__p___mb_cur_max), X(_getmbcp), X(_setmbcp), X(_Getdays), X(_Getmonths), X(_Gettnames),
};
EXPORT_TABLE("ucrtbase.dll", str_exports);

// _pctype is a variable holding a pointer to the table
static const u16* pctype_var = nullptr;
static struct PctypeInit { PctypeInit() { ctype_init(); pctype_var = ctype_table + 1; } } pctype_init;
static const Export msvcrt_str[] = {
    {"__mb_cur_max", (void*)&c___mb_cur_max}, {"_pctype", (void*)&pctype_var}, {"_mbctype", (void*)mbctype_table},
};
EXPORT_TABLE("msvcrt.dll", msvcrt_str);

}  // namespace rt
