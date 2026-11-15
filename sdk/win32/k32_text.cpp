// win32run - code pages, UTF-16, string comparison and locale information (kernel32), the bits of ntdll
// and advapi32 programs call directly, and random numbers.
//
// The ANSI code page is UTF-8 (65001), as on Windows 10 with the "use UTF-8" option: silicon's files and
// console are UTF-8, so narrow strings pass through unchanged. Code pages 1252, 437 and 28591 are
// converted as Windows-1252 / Latin-1 for programs that ask for them by number.
#include "rt.h"

namespace rt {

enum { CP_ACP = 0, CP_OEMCP = 1, CP_MACCP = 2, CP_THREAD_ACP = 3, CP_UTF7 = 65000, CP_UTF8 = 65001 };
enum { MB_ERR_INVALID_CHARS = 8, WC_ERR_INVALID_CHARS = 0x80, WC_NO_BEST_FIT_CHARS = 0x400 };

static const u16 cp1252_hi[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

static bool is_utf8_cp(u32 cp) { return cp == CP_ACP || cp == CP_OEMCP || cp == CP_THREAD_ACP || cp == CP_UTF8 || cp == CP_MACCP; }

static bool valid_utf8(const char* s, int n) {
    for (int i = 0; i < n;) {
        u8 c = (u8)s[i];
        int more = c < 0x80 ? 0 : c >= 0xF0 && c < 0xF5 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 && c < 0xE0 ? 1 : -1;
        if (more < 0 || i + more >= n + (more ? 0 : 1)) { if (more) return false; }
        if (more < 0) return false;
        for (int k = 1; k <= more; k++) if (i + k >= n || ((u8)s[i + k] & 0xC0) != 0x80) return false;
        i += more + 1;
    }
    return true;
}

static int WINAPI MultiByteToWideChar(u32 cp, u32 flags, const char* src, int n, WCHAR* dst, int cap) {
    if (!src || n == 0 || cap < 0 || (cap && !dst)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (n < 0) n = (int)slen(src) + 1;
    if (is_utf8_cp(cp)) {
        if ((flags & MB_ERR_INVALID_CHARS) && !valid_utf8(src, n)) { set_error(ERROR_NO_UNICODE_TRANSLATION); return 0; }
        int need = wide_from_utf8(src, n, nullptr, 0);
        if (!cap) return need;
        if (need > cap) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        return wide_from_utf8(src, n, dst, cap);
    }
    // single-byte code pages
    if (!cap) return n;
    if (n > cap) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < n; i++) {
        u8 c = (u8)src[i];
        dst[i] = (WCHAR)(cp == 1252 && c >= 0x80 && c < 0xA0 ? cp1252_hi[c - 0x80] : c);
    }
    return n;
}

static int WINAPI WideCharToMultiByte(u32 cp, u32 flags, const WCHAR* src, int n, char* dst, int cap, const char* defchar, BOOL* used) {
    if (!src || n == 0 || cap < 0 || (cap && !dst)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (n < 0) n = (int)wlen(src) + 1;
    if (used) *used = FALSE;
    if (is_utf8_cp(cp)) {
        if (flags & WC_ERR_INVALID_CHARS)
            for (int i = 0; i < n; i++) {
                u16 c = (u16)src[i];
                if (c >= 0xD800 && c < 0xDC00) { if (i + 1 >= n || (u16)src[i + 1] < 0xDC00 || (u16)src[i + 1] >= 0xE000) { set_error(ERROR_NO_UNICODE_TRANSLATION); return 0; } i++; }
                else if (c >= 0xDC00 && c < 0xE000) { set_error(ERROR_NO_UNICODE_TRANSLATION); return 0; }
            }
        int need = utf8_from_wide(src, n, nullptr, 0);
        if (!cap) return need;
        if (need > cap) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        return utf8_from_wide(src, n, dst, cap);
    }
    if (!cap) return n;
    if (n > cap) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    char dc = defchar ? *defchar : '?';
    for (int i = 0; i < n; i++) {
        u16 c = (u16)src[i];
        char o = 0;
        if (c < 0x80 || (c >= 0xA0 && c < 0x100)) o = (char)c;
        else if (cp == 1252) { for (int k = 0; k < 32; k++) if (cp1252_hi[k] == c) { o = (char)(0x80 + k); break; } }
        if (!o && c) { o = dc; if (used) *used = TRUE; }
        dst[i] = o;
    }
    return n;
}

static u32 WINAPI GetACP() { return CP_UTF8; }
static u32 WINAPI GetOEMCP() { return CP_UTF8; }
struct CPINFO { u32 MaxCharSize; u8 DefaultChar[2]; u8 LeadByte[12]; };
struct CPINFOEXW { u32 MaxCharSize; u8 DefaultChar[2]; u8 LeadByte[12]; WCHAR UnicodeDefaultChar; u32 CodePage; WCHAR CodePageName[260]; };
static BOOL WINAPI GetCPInfo(u32 cp, CPINFO* i) {
    memset(i, 0, sizeof(*i));
    i->MaxCharSize = is_utf8_cp(cp) ? 4 : 1;
    i->DefaultChar[0] = '?';
    return TRUE;
}
static BOOL WINAPI GetCPInfoExW(u32 cp, u32 f, CPINFOEXW* i) {
    (void)f;
    memset(i, 0, sizeof(*i));
    i->MaxCharSize = is_utf8_cp(cp) ? 4 : 1;
    i->DefaultChar[0] = '?';
    i->UnicodeDefaultChar = '?';
    i->CodePage = is_utf8_cp(cp) ? CP_UTF8 : cp;
    wide_from_utf8(is_utf8_cp(cp) ? "65001 (UTF-8)" : "1252 (ANSI - Latin I)", -1, i->CodePageName, 260);
    return TRUE;
}
static BOOL WINAPI IsValidCodePage(u32 cp) { return is_utf8_cp(cp) || cp == 1252 || cp == 437 || cp == 28591 || cp == 1200 || cp == 20127; }
static BOOL WINAPI IsDBCSLeadByte(u8 c) { (void)c; return FALSE; }
static BOOL WINAPI IsDBCSLeadByteEx(u32 cp, u8 c) { (void)cp; (void)c; return FALSE; }

// ---------------------------------------------------------------- lstr*
static int WINAPI lstrlenA(const char* s) { return s ? (int)slen(s) : 0; }
static int WINAPI lstrlenW(const WCHAR* s) { return s ? (int)wlen(s) : 0; }
static char* WINAPI lstrcpyA(char* d, const char* s) { return scpy(d, s); }
static WCHAR* WINAPI lstrcpyW(WCHAR* d, const WCHAR* s) { WCHAR* r = d; while ((*d++ = *s++)) {} return r; }
static char* WINAPI lstrcpynA(char* d, const char* s, int n) { if (n > 0) slcpy(d, s, (usize)n); return d; }
static WCHAR* WINAPI lstrcpynW(WCHAR* d, const WCHAR* s, int n) {
    if (n <= 0) return d;
    int i = 0;
    for (; i < n - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
    return d;
}
static char* WINAPI lstrcatA(char* d, const char* s) { scpy(d + slen(d), s); return d; }
static WCHAR* WINAPI lstrcatW(WCHAR* d, const WCHAR* s) { lstrcpyW(d + wlen(d), s); return d; }

// a rough "word sort": case and accents differ last, as CompareString does for Latin text
static int fold(u32 c) { return lower((int)c); }
static int cmp_text(const WCHAR* a, int na, const WCHAR* b, int nb, bool nocase) {
    if (na < 0) na = (int)wlen(a);
    if (nb < 0) nb = (int)wlen(b);
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) {
        int x = fold(a[i]), y = fold(b[i]);
        if (x != y) return x < y ? 1 : 3;
    }
    if (na != nb) return na < nb ? 1 : 3;
    if (!nocase)
        for (int i = 0; i < n; i++) if (a[i] != b[i]) return (a[i] >= 'a' && a[i] <= 'z') ? 1 : 3;     // lower case sorts first
    return 2;
}
enum { NORM_IGNORECASE = 1, LINGUISTIC_IGNORECASE = 0x10, NORM_IGNORENONSPACE = 2 };
static int WINAPI CompareStringW(u32 lcid, u32 flags, const WCHAR* a, int na, const WCHAR* b, int nb) {
    (void)lcid;
    if (!a || !b) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    return cmp_text(a, na, b, nb, flags & (NORM_IGNORECASE | LINGUISTIC_IGNORECASE));
}
static int WINAPI CompareStringEx(const WCHAR* loc, u32 flags, const WCHAR* a, int na, const WCHAR* b, int nb, void* v, void* r, i64 p) {
    (void)loc; (void)v; (void)r; (void)p;
    return CompareStringW(0, flags, a, na, b, nb);
}
static int WINAPI CompareStringA(u32 lcid, u32 flags, const char* a, int na, const char* b, int nb) {
    WCHAR wa[1024], wb[1024];
    int la = MultiByteToWideChar(CP_UTF8, 0, a, na, wa, 1024), lb = MultiByteToWideChar(CP_UTF8, 0, b, nb, wb, 1024);
    if (na < 0 && la) la--;
    if (nb < 0 && lb) lb--;
    return CompareStringW(lcid, flags, wa, la, wb, lb);
}
static int WINAPI CompareStringOrdinal(const WCHAR* a, int na, const WCHAR* b, int nb, BOOL nocase) {
    if (na < 0) na = (int)wlen(a);
    if (nb < 0) nb = (int)wlen(b);
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) {
        int x = nocase ? upper(a[i]) : a[i], y = nocase ? upper(b[i]) : b[i];
        if (x != y) return x < y ? 1 : 3;
    }
    return na == nb ? 2 : na < nb ? 1 : 3;
}
static int WINAPI lstrcmpW(const WCHAR* a, const WCHAR* b) { return CompareStringW(0, 0, a ? a : L"", -1, b ? b : L"", -1) - 2; }
static int WINAPI lstrcmpiW(const WCHAR* a, const WCHAR* b) { return CompareStringW(0, NORM_IGNORECASE, a ? a : L"", -1, b ? b : L"", -1) - 2; }
static int WINAPI lstrcmpA(const char* a, const char* b) { return CompareStringA(0, 0, a ? a : "", -1, b ? b : "", -1) - 2; }
static int WINAPI lstrcmpiA(const char* a, const char* b) { return CompareStringA(0, NORM_IGNORECASE, a ? a : "", -1, b ? b : "", -1) - 2; }

// case mapping for Latin-1, Latin Extended-A, Greek and Cyrillic
u32 wupper(u32 c) {
    if (c < 0x80) return (u32)upper((int)c);
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c < 0x180 && (c & 1) && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) return c - 1;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}
u32 wlower(u32 c) {
    if (c < 0x80) return (u32)lower((int)c);
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c == 0x178) return 0xFF;
    if (c >= 0x100 && c < 0x180 && !(c & 1) && c != 0x130 && c != 0x138) return c + 1;
    if (c >= 0x391 && c <= 0x3A9) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}
enum { LCMAP_LOWERCASE = 0x100, LCMAP_UPPERCASE = 0x200, LCMAP_SORTKEY = 0x400 };
static int WINAPI LCMapStringEx(const WCHAR* loc, u32 flags, const WCHAR* src, int n, WCHAR* dst, int cap, void* v, void* r, i64 p) {
    (void)loc; (void)v; (void)r; (void)p;
    if (n < 0) n = (int)wlen(src) + 1;
    if (flags & LCMAP_SORTKEY) {                                // a sort key: the folded text as bytes
        int need = n * 2 + 1;
        if (!cap) return need;
        if (cap < need) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        u8* o = (u8*)dst;
        for (int i = 0; i < n; i++) { u32 c = wupper((u16)src[i]); o[i * 2] = (u8)(c >> 8); o[i * 2 + 1] = (u8)c; }
        o[n * 2] = 0;
        return need;
    }
    if (!cap) return n;
    if (cap < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < n; i++) {
        u32 c = (u16)src[i];
        dst[i] = (WCHAR)((flags & LCMAP_UPPERCASE) ? wupper(c) : (flags & LCMAP_LOWERCASE) ? wlower(c) : c);
    }
    return n;
}
static int WINAPI LCMapStringW(u32 lcid, u32 flags, const WCHAR* src, int n, WCHAR* dst, int cap) { (void)lcid; return LCMapStringEx(nullptr, flags, src, n, dst, cap, nullptr, nullptr, 0); }
static int WINAPI LCMapStringA(u32 lcid, u32 flags, const char* src, int n, char* dst, int cap) {
    (void)lcid;
    if (n < 0) n = (int)slen(src) + 1;
    if (!cap) return n;
    if (cap < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < n; i++) dst[i] = (char)((flags & LCMAP_UPPERCASE) ? upper((u8)src[i]) : (flags & LCMAP_LOWERCASE) ? lower((u8)src[i]) : src[i]);
    return n;
}

enum { C1_UPPER = 1, C1_LOWER = 2, C1_DIGIT = 4, C1_SPACE = 8, C1_PUNCT = 0x10, C1_CNTRL = 0x20, C1_BLANK = 0x40, C1_XDIGIT = 0x80, C1_ALPHA = 0x100, C1_DEFINED = 0x200 };
u16 ctype1(u32 c) {
    u16 t = C1_DEFINED;
    if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0)) t |= C1_CNTRL;
    if (c == ' ' || c == '\t' || c == 0xA0 || c == 0x3000) t |= C1_BLANK;
    if ((c >= 9 && c <= 13) || c == ' ' || c == 0xA0 || c == 0x2028 || c == 0x2029 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A)) t |= C1_SPACE;
    if (c >= '0' && c <= '9') t |= C1_DIGIT | C1_XDIGIT;
    if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) t |= C1_XDIGIT;
    bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c < 0x250 && c != 0xD7 && c != 0xF7) ||
                 (c >= 0x370 && c < 0x530) || (c >= 0x5D0 && c < 0x5EB) || (c >= 0x620 && c < 0x64B) || (c >= 0x3040 && c < 0x9FFF) || (c >= 0xAC00 && c < 0xD7A4);
    if (alpha) {
        t |= C1_ALPHA;
        if (wupper(c) != c) t |= C1_LOWER;
        else if (wlower(c) != c) t |= C1_UPPER;
    }
    if (!alpha && !(t & (C1_DIGIT | C1_SPACE | C1_CNTRL)) && c > 0x20 && c != 0xFFFF) t |= C1_PUNCT;
    return t;
}
static BOOL WINAPI GetStringTypeW(u32 type, const WCHAR* src, int n, u16* out) {
    if (n < 0) n = (int)wlen(src) + 1;
    for (int i = 0; i < n; i++) {
        u32 c = (u16)src[i];
        if (type == 1) out[i] = ctype1(c);
        else if (type == 2) out[i] = (c >= 0x590 && c < 0x900) ? 2 : (c < 0x80 && (ctype1(c) & (C1_ALPHA | C1_DIGIT))) ? 1 : 0;   // C2: L, R, other
        else out[i] = 0;
    }
    return TRUE;
}
static BOOL WINAPI GetStringTypeExW(u32 lcid, u32 type, const WCHAR* src, int n, u16* out) { (void)lcid; return GetStringTypeW(type, src, n, out); }
static BOOL WINAPI GetStringTypeA(u32 lcid, u32 type, const char* src, int n, u16* out) {
    (void)lcid;
    if (n < 0) n = (int)slen(src) + 1;
    for (int i = 0; i < n; i++) out[i] = type == 1 ? ctype1((u8)src[i]) : 0;
    return TRUE;
}
static BOOL WINAPI GetStringTypeExA(u32 lcid, u32 type, const char* src, int n, u16* out) { return GetStringTypeA(lcid, type, src, n, out); }
static int WINAPI FoldStringW(u32 f, const WCHAR* s, int n, WCHAR* d, int cap) {
    (void)f;
    if (n < 0) n = (int)wlen(s) + 1;
    if (!cap) return n;
    if (cap < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(d, s, (usize)n * 2);
    return n;
}
static int WINAPI NormalizeString(int form, const WCHAR* s, int n, WCHAR* d, int cap) { (void)form; return FoldStringW(0, s, n, d, cap); }
static BOOL WINAPI IsNormalizedString(int form, const WCHAR* s, int n) { (void)form; (void)s; (void)n; return TRUE; }

// ---------------------------------------------------------------- locales: everything is en-US
static const u32 LCID_EN_US = 0x409;
static u32 WINAPI GetUserDefaultLCID() { return LCID_EN_US; }
static u32 WINAPI GetSystemDefaultLCID() { return LCID_EN_US; }
static u32 WINAPI GetThreadLocale() { return LCID_EN_US; }
static BOOL WINAPI SetThreadLocale(u32 l) { (void)l; return TRUE; }
static u16 WINAPI GetUserDefaultLangID() { return 0x409; }
static u16 WINAPI GetSystemDefaultLangID() { return 0x409; }
static u16 WINAPI GetUserDefaultUILanguage() { return 0x409; }
static u16 WINAPI GetSystemDefaultUILanguage() { return 0x409; }
static u16 WINAPI GetThreadUILanguage() { return 0x409; }
static u16 WINAPI SetThreadUILanguage(u16 l) { (void)l; return 0x409; }
static int put_loc(const char* s, WCHAR* out, int cap) {
    int n = (int)slen(s) + 1;
    if (!cap) return n;
    if (cap < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    wide_from_utf8(s, -1, out, cap);
    return n;
}
static int WINAPI GetUserDefaultLocaleName(WCHAR* out, int cap) { return put_loc("en-US", out, cap); }
static int WINAPI GetSystemDefaultLocaleName(WCHAR* out, int cap) { return put_loc("en-US", out, cap); }
static u32 WINAPI LocaleNameToLCID(const WCHAR* name, u32 f) { (void)name; (void)f; return LCID_EN_US; }
static int WINAPI LCIDToLocaleName(u32 l, WCHAR* out, int cap, u32 f) { (void)l; (void)f; return put_loc("en-US", out, cap); }
static BOOL WINAPI IsValidLocale(u32 l, u32 f) { (void)f; return (l & 0x3FF) == 9 || l == 0x7F || l == 0x400 || l == 0x800; }
static BOOL WINAPI IsValidLocaleName(const WCHAR* n) { (void)n; return TRUE; }
static int WINAPI ResolveLocaleName(const WCHAR* n, WCHAR* out, int cap) { (void)n; return put_loc("en-US", out, cap); }

static const char* locale_value(u32 type, char* num) {
    switch (type & 0xFFFF) {
    case 0x01: return "0409";                   // LOCALE_ILANGUAGE
    case 0x02: case 0x6F: return "English (United States)";   // LOCALE_SLANGUAGE, LOCALE_SLOCALIZEDDISPLAYNAME
    case 0x1001: return "English";              // LOCALE_SENGLANGUAGE
    case 0x03: return "ENU";                    // LOCALE_SABBREVLANGNAME
    case 0x04: return "English";                // LOCALE_SNATIVELANGNAME
    case 0x05: return "1";                      // LOCALE_ICOUNTRY
    case 0x06: case 0x1002: return "United States";
    case 0x07: return "USA";                    // LOCALE_SABBREVCTRYNAME
    case 0x08: return "United States";
    case 0x0B: return "437";                    // LOCALE_IDEFAULTCODEPAGE (OEM)
    case 0x1004: snfmt(num, 16, "%u", CP_UTF8); return num;   // LOCALE_IDEFAULTANSICODEPAGE
    case 0x0C: return ",";                      // LOCALE_SLIST
    case 0x0D: return "1";                      // LOCALE_IMEASURE (US)
    case 0x0E: return ".";                      // LOCALE_SDECIMAL
    case 0x0F: return ",";                      // LOCALE_STHOUSAND
    case 0x10: return "3;0";                    // LOCALE_SGROUPING
    case 0x11: return "2";                      // LOCALE_IDIGITS
    case 0x12: return "1";                      // LOCALE_ILZERO
    case 0x14: return "$";                      // LOCALE_SCURRENCY
    case 0x15: return "USD";                    // LOCALE_SINTLSYMBOL
    case 0x16: return ".";                      // LOCALE_SMONDECIMALSEP
    case 0x17: return ",";                      // LOCALE_SMONTHOUSANDSEP
    case 0x18: return "3;0";                    // LOCALE_SMONGROUPING
    case 0x19: return "2";                      // LOCALE_ICURRDIGITS
    case 0x1B: return "0";                      // LOCALE_ICURRENCY
    case 0x1C: return "0";                      // LOCALE_INEGCURR
    case 0x1D: return "/";                      // LOCALE_SDATE
    case 0x1E: return ":";                      // LOCALE_STIME
    case 0x1F: return "M/d/yyyy";               // LOCALE_SSHORTDATE
    case 0x20: return "dddd, MMMM d, yyyy";     // LOCALE_SLONGDATE
    case 0x1003: return "h:mm:ss tt";           // LOCALE_STIMEFORMAT
    case 0x21: return "0";                      // LOCALE_IDATE
    case 0x23: return "0";                      // LOCALE_ITIME (12 h)
    case 0x28: return "AM";                     // LOCALE_S1159
    case 0x29: return "PM";                     // LOCALE_S2359
    case 0x50: return "";                       // LOCALE_SPOSITIVESIGN
    case 0x51: return "-";                      // LOCALE_SNEGATIVESIGN
    case 0x59: return "en";                     // LOCALE_SISO639LANGNAME
    case 0x5A: return "US";                     // LOCALE_SISO3166CTRYNAME
    case 0x5C: case 0x67: return "en-US";       // LOCALE_SNAME, LOCALE_SPARENT ... close enough
    case 0x1009: return "0";                    // LOCALE_IFIRSTDAYOFWEEK (Monday = 0... Sunday is 6)
    case 0x100C: return "0";                    // LOCALE_IFIRSTWEEKOFYEAR
    case 0x1010: return "1";                    // LOCALE_IPAPERSIZE
    case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: {
        static const char* d[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
        return d[(type & 0xFFFF) - 0x2A];
    }
    case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: {
        static const char* d[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        return d[(type & 0xFFFF) - 0x31];
    }
    }
    if ((type & 0xFFFF) >= 0x38 && (type & 0xFFFF) <= 0x43) {
        static const char* m[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
        return m[(type & 0xFFFF) - 0x38];
    }
    if ((type & 0xFFFF) >= 0x44 && (type & 0xFFFF) <= 0x4F) {
        static const char* m[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        return m[(type & 0xFFFF) - 0x44];
    }
    return nullptr;
}
static const u32 LOCALE_RETURN_NUMBER = 0x20000000;
static int WINAPI GetLocaleInfoEx(const WCHAR* loc, u32 type, WCHAR* out, int cap) {
    (void)loc;
    char num[16];
    const char* v = locale_value(type, num);
    if (!v) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (type & LOCALE_RETURN_NUMBER) {
        u32 x = 0;
        for (const char* p = v; *p >= '0' && *p <= '9'; p++) x = x * 10 + (u32)(*p - '0');
        if ((type & 0xFFFF) == 1) x = 0x409;
        if (!cap) return 2;
        if (cap < 2) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        memcpy(out, &x, 4);
        return 2;
    }
    return put_loc(v, out, cap);
}
static int WINAPI GetLocaleInfoW(u32 lcid, u32 type, WCHAR* out, int cap) { (void)lcid; return GetLocaleInfoEx(nullptr, type, out, cap); }
static int WINAPI GetLocaleInfoA(u32 lcid, u32 type, char* out, int cap) {
    (void)lcid;
    char num[16];
    const char* v = locale_value(type, num);
    if (!v) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    int n = (int)slen(v) + 1;
    if (!cap) return n;
    if (cap < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(out, v, (usize)n);
    return n;
}
static BOOL WINAPI SetLocaleInfoW(u32 l, u32 t, const WCHAR* v) { (void)l; (void)t; (void)v; return TRUE; }
typedef BOOL (WINAPI *LocEnumW)(WCHAR*);
typedef BOOL (WINAPI *LocEnumEx)(WCHAR*, u32, LPARAM);
static BOOL WINAPI EnumSystemLocalesW(LocEnumW f, u32 fl) { (void)fl; WCHAR s[] = L"00000409"; f(s); return TRUE; }
static BOOL WINAPI EnumSystemLocalesEx(LocEnumEx f, u32 fl, LPARAM p, void* r) { (void)fl; (void)r; WCHAR s[] = L"en-US"; f(s, 1, p); return TRUE; }
static u32 WINAPI GetUserGeoID(u32 c) { (void)c; return 244; }
static BOOL WINAPI GetUserPreferredUILanguages(u32 f, u32* n, WCHAR* buf, u32* sz) {
    (void)f;
    *n = 1;
    if (!buf || *sz < 7) { *sz = 7; if (buf) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; } return TRUE; }
    wide_from_utf8("en-US", -1, buf, 6);
    buf[6] = 0;
    *sz = 7;
    return TRUE;
}
static BOOL WINAPI GetThreadPreferredUILanguages(u32 f, u32* n, WCHAR* buf, u32* sz) { return GetUserPreferredUILanguages(f, n, buf, sz); }
static BOOL WINAPI GetProcessPreferredUILanguages(u32 f, u32* n, WCHAR* buf, u32* sz) { return GetUserPreferredUILanguages(f, n, buf, sz); }
static BOOL WINAPI SetProcessPreferredUILanguages(u32 f, const WCHAR* b, u32* n) { (void)f; (void)b; if (n) *n = 1; return TRUE; }

// GetDateFormat / GetTimeFormat with Windows' picture strings
static int fmt_date(const SYSTEMTIME* t, const char* pic, bool time, char* out, int cap) {
    static const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char* months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    char b[256];
    int o = 0;
    auto add = [&](const char* s, int n) { for (int i = 0; i < n && o < 250; i++) b[o++] = s[i]; };
    for (const char* p = pic; *p;) {
        char c = *p;
        int run = 1;
        while (p[run] == c) run++;
        char tmp[32];
        int k = 0;
        if (c == '\'') {
            const char* e = schr(p + 1, '\'');
            if (!e) e = p + slen(p);
            add(p + 1, (int)(e - p - 1));
            p = *e ? e + 1 : e;
            continue;
        }
        if (!time && c == 'd') {
            if (run == 1) k = snfmt(tmp, sizeof(tmp), "%d", t->wDay);
            else if (run == 2) k = snfmt(tmp, sizeof(tmp), "%02d", t->wDay);
            else if (run == 3) k = snfmt(tmp, sizeof(tmp), "%.3s", days[t->wDayOfWeek % 7]);
            else k = snfmt(tmp, sizeof(tmp), "%s", days[t->wDayOfWeek % 7]);
        } else if (!time && c == 'M') {
            int m = (t->wMonth + 11) % 12;
            if (run == 1) k = snfmt(tmp, sizeof(tmp), "%d", t->wMonth);
            else if (run == 2) k = snfmt(tmp, sizeof(tmp), "%02d", t->wMonth);
            else if (run == 3) k = snfmt(tmp, sizeof(tmp), "%.3s", months[m]);
            else k = snfmt(tmp, sizeof(tmp), "%s", months[m]);
        } else if (!time && c == 'y') {
            if (run <= 2) k = snfmt(tmp, sizeof(tmp), run == 1 ? "%d" : "%02d", t->wYear % 100);
            else k = snfmt(tmp, sizeof(tmp), "%d", t->wYear);
        } else if (time && (c == 'h' || c == 'H')) {
            int h = c == 'h' ? (t->wHour % 12 ? t->wHour % 12 : 12) : t->wHour;
            k = snfmt(tmp, sizeof(tmp), run == 1 ? "%d" : "%02d", h);
        } else if (time && c == 'm') k = snfmt(tmp, sizeof(tmp), run == 1 ? "%d" : "%02d", t->wMinute);
        else if (time && c == 's') k = snfmt(tmp, sizeof(tmp), run == 1 ? "%d" : "%02d", t->wSecond);
        else if (time && c == 't') k = snfmt(tmp, sizeof(tmp), "%.*s", run == 1 ? 1 : 2, t->wHour < 12 ? "AM" : "PM");
        else { for (int i = 0; i < run; i++) tmp[k++] = c; }
        add(tmp, k);
        p += run;
    }
    b[o++] = 0;
    if (!cap) return o;
    if (cap < o) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(out, b, (usize)o);
    return o;
}
static void now_st(SYSTEMTIME* s) { rt_GetLocalTime(s); }
static int WINAPI GetDateFormatA(u32 l, u32 f, const SYSTEMTIME* t, const char* pic, char* out, int cap) {
    (void)l;
    SYSTEMTIME s;
    if (!t) { now_st(&s); t = &s; }
    return fmt_date(t, pic ? pic : (f & 2) ? "dddd, MMMM d, yyyy" : "M/d/yyyy", false, out, cap);
}
static int WINAPI GetTimeFormatA(u32 l, u32 f, const SYSTEMTIME* t, const char* pic, char* out, int cap) {
    (void)l; (void)f;
    SYSTEMTIME s;
    if (!t) { now_st(&s); t = &s; }
    return fmt_date(t, pic ? pic : "h:mm:ss tt", true, out, cap);
}
static int wrap_w(int (*fn)(const SYSTEMTIME*, const char*, bool, char*, int), const SYSTEMTIME* t, const WCHAR* pic, const char* def, bool time, WCHAR* out, int cap) {
    char p[128], o[256];
    if (pic) utf8_from_wide(pic, -1, p, sizeof(p)); else slcpy(p, def, sizeof(p));
    SYSTEMTIME s;
    if (!t) { now_st(&s); t = &s; }
    int n = fn(t, p, time, o, sizeof(o));
    if (!n) return 0;
    int need = wide_from_utf8(o, -1, nullptr, 0);
    if (!cap) return need;
    if (cap < need) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    return wide_from_utf8(o, -1, out, cap);
}
static int WINAPI GetDateFormatW(u32 l, u32 f, const SYSTEMTIME* t, const WCHAR* pic, WCHAR* out, int cap) { (void)l; return wrap_w(fmt_date, t, pic, (f & 2) ? "dddd, MMMM d, yyyy" : "M/d/yyyy", false, out, cap); }
static int WINAPI GetTimeFormatW(u32 l, u32 f, const SYSTEMTIME* t, const WCHAR* pic, WCHAR* out, int cap) { (void)l; (void)f; return wrap_w(fmt_date, t, pic, "h:mm:ss tt", true, out, cap); }
static int WINAPI GetDateFormatEx(const WCHAR* n, u32 f, const SYSTEMTIME* t, const WCHAR* pic, WCHAR* out, int cap, const WCHAR* cal) { (void)n; (void)cal; return GetDateFormatW(0, f, t, pic, out, cap); }
static int WINAPI GetTimeFormatEx(const WCHAR* n, u32 f, const SYSTEMTIME* t, const WCHAR* pic, WCHAR* out, int cap) { (void)n; return GetTimeFormatW(0, f, t, pic, out, cap); }
static int WINAPI GetNumberFormatW(u32 l, u32 f, const WCHAR* v, const void* fmt, WCHAR* out, int cap) {
    (void)l; (void)f; (void)fmt;
    int n = (int)wlen(v) + 1;
    if (!cap) return n;
    if (cap < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(out, v, (usize)n * 2);
    return n;
}
static int WINAPI GetNumberFormatEx(const WCHAR* loc, u32 f, const WCHAR* v, const void* fmt, WCHAR* out, int cap) { (void)loc; return GetNumberFormatW(0, f, v, fmt, out, cap); }
static int WINAPI GetCalendarInfoW(u32 l, u32 cal, u32 t, WCHAR* out, int cap, u32* val) { (void)l; (void)cal; (void)t; if (val) *val = 1; return put_loc("1", out, cap); }
static int WINAPI GetCalendarInfoEx(const WCHAR* n, u32 cal, const WCHAR* r, u32 t, WCHAR* out, int cap, u32* val) { (void)n; (void)r; return GetCalendarInfoW(0, cal, t, out, cap, val); }

// ---------------------------------------------------------------- random numbers
static u64 rng_state = 0;
static u64 rng() {
    if (!rng_state) {
        rng_state = si::ticks_us() * 0x9E3779B97F4A7C15ull ^ ((u64)si::rand() << 32 | si::rand());
        u32 a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        if (c & (1u << 30)) { u64 r; u8 ok; __asm__ volatile("rdrand %0; setc %1" : "=r"(r), "=qm"(ok)); if (ok) rng_state ^= r; }
    }
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state ^ ((u64)si::rand() << 17) ^ si::ticks_us();
}
void random_bytes(void* buf, usize n) {
    u8* p = (u8*)buf;
    while (n) { u64 r = rng(); usize k = n < 8 ? n : 8; memcpy(p, &r, k); p += k; n -= k; }
}
static u8 WINAPI SystemFunction036(void* buf, u32 n) { random_bytes(buf, n); return 1; }      // RtlGenRandom
static NTSTATUS WINAPI BCryptGenRandom(void* alg, u8* buf, u32 n, u32 flags) { (void)alg; (void)flags; random_bytes(buf, n); return 0; }
static BOOL WINAPI ProcessPrng(u8* buf, SIZE_T n) { random_bytes(buf, n); return TRUE; }
static BOOL WINAPI CryptAcquireContextW(u64* h, const WCHAR* c, const WCHAR* p, u32 t, u32 f) { (void)c; (void)p; (void)t; (void)f; *h = 0x5ec; return TRUE; }
static BOOL WINAPI CryptAcquireContextA(u64* h, const char* c, const char* p, u32 t, u32 f) { (void)c; (void)p; (void)t; (void)f; *h = 0x5ec; return TRUE; }
static BOOL WINAPI CryptGenRandom(u64 h, u32 n, u8* buf) { (void)h; random_bytes(buf, n); return TRUE; }
static BOOL WINAPI CryptReleaseContext(u64 h, u32 f) { (void)h; (void)f; return TRUE; }

// ---------------------------------------------------------------- the registry: empty
static i32 WINAPI RegOpenKeyExW(HANDLE k, const WCHAR* s, u32 o, u32 a, HANDLE* out) { (void)k; (void)s; (void)o; (void)a; *out = nullptr; return ERROR_FILE_NOT_FOUND; }
static i32 WINAPI RegOpenKeyExA(HANDLE k, const char* s, u32 o, u32 a, HANDLE* out) { (void)k; (void)s; (void)o; (void)a; *out = nullptr; return ERROR_FILE_NOT_FOUND; }
static i32 WINAPI RegOpenKeyW(HANDLE k, const WCHAR* s, HANDLE* out) { return RegOpenKeyExW(k, s, 0, 0, out); }
static i32 WINAPI RegOpenKeyA(HANDLE k, const char* s, HANDLE* out) { return RegOpenKeyExA(k, s, 0, 0, out); }
static i32 WINAPI RegCreateKeyExW(HANDLE k, const WCHAR* s, u32 r, WCHAR* c, u32 o, u32 a, void* sa, HANDLE* out, u32* d) { (void)k; (void)s; (void)r; (void)c; (void)o; (void)a; (void)sa; (void)d; *out = nullptr; return ERROR_ACCESS_DENIED; }
static i32 WINAPI RegCreateKeyExA(HANDLE k, const char* s, u32 r, char* c, u32 o, u32 a, void* sa, HANDLE* out, u32* d) { (void)k; (void)s; (void)r; (void)c; (void)o; (void)a; (void)sa; (void)d; *out = nullptr; return ERROR_ACCESS_DENIED; }
static i32 WINAPI RegQueryValueExW(HANDLE k, const WCHAR* n, u32* r, u32* t, u8* d, u32* sz) { (void)k; (void)n; (void)r; (void)t; (void)d; (void)sz; return ERROR_FILE_NOT_FOUND; }
static i32 WINAPI RegQueryValueExA(HANDLE k, const char* n, u32* r, u32* t, u8* d, u32* sz) { (void)k; (void)n; (void)r; (void)t; (void)d; (void)sz; return ERROR_FILE_NOT_FOUND; }
static i32 WINAPI RegGetValueW(HANDLE k, const WCHAR* s, const WCHAR* v, u32 f, u32* t, void* d, u32* sz) { (void)k; (void)s; (void)v; (void)f; (void)t; (void)d; (void)sz; return ERROR_FILE_NOT_FOUND; }
static i32 WINAPI RegSetValueExW(HANDLE k, const WCHAR* n, u32 r, u32 t, const u8* d, u32 sz) { (void)k; (void)n; (void)r; (void)t; (void)d; (void)sz; return ERROR_ACCESS_DENIED; }
static i32 WINAPI RegSetValueExA(HANDLE k, const char* n, u32 r, u32 t, const u8* d, u32 sz) { (void)k; (void)n; (void)r; (void)t; (void)d; (void)sz; return ERROR_ACCESS_DENIED; }
static i32 WINAPI RegDeleteValueW(HANDLE k, const WCHAR* n) { (void)k; (void)n; return ERROR_FILE_NOT_FOUND; }
static i32 WINAPI RegDeleteKeyW(HANDLE k, const WCHAR* n) { (void)k; (void)n; return ERROR_FILE_NOT_FOUND; }
static i32 WINAPI RegEnumKeyExW(HANDLE k, u32 i, WCHAR* n, u32* nl, u32* r, WCHAR* c, u32* cl, FILETIME* t) { (void)k; (void)i; (void)n; (void)nl; (void)r; (void)c; (void)cl; (void)t; return ERROR_NO_MORE_ITEMS; }
static i32 WINAPI RegEnumValueW(HANDLE k, u32 i, WCHAR* n, u32* nl, u32* r, u32* t, u8* d, u32* dl) { (void)k; (void)i; (void)n; (void)nl; (void)r; (void)t; (void)d; (void)dl; return ERROR_NO_MORE_ITEMS; }
static i32 WINAPI RegQueryInfoKeyW(HANDLE k, WCHAR* c, u32* cl, u32* r, u32* nk, u32* mk, u32* mc, u32* nv, u32* mvn, u32* mvl, u32* sd, FILETIME* t) {
    (void)k; (void)c; (void)cl; (void)r; (void)mk; (void)mc; (void)mvn; (void)mvl; (void)sd; (void)t;
    if (nk) *nk = 0;
    if (nv) *nv = 0;
    return ERROR_FILE_NOT_FOUND;
}
static i32 WINAPI RegCloseKey(HANDLE k) { (void)k; return 0; }
static i32 WINAPI RegFlushKey(HANDLE k) { (void)k; return 0; }
static BOOL WINAPI OpenProcessToken(HANDLE p, u32 a, HANDLE* t) { (void)p; (void)a; *t = (HANDLE)0x7070; return TRUE; }
static BOOL WINAPI OpenThreadToken(HANDLE th, u32 a, BOOL s, HANDLE* t) { (void)th; (void)a; (void)s; *t = nullptr; set_error(1008); return FALSE; }
static BOOL WINAPI GetTokenInformation(HANDLE t, int c, void* b, u32 n, u32* r) {
    (void)t;
    if (c == 20) { if (r) *r = 4; if (n >= 4) { *(u32*)b = 0; return TRUE; } }   // TokenElevation: not elevated
    if (r) *r = 0;
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}
static BOOL WINAPI AllocateAndInitializeSid(void* a, u8 n, u32 s0, u32 s1, u32 s2, u32 s3, u32 s4, u32 s5, u32 s6, u32 s7, void** sid) {
    (void)a; (void)n; (void)s0; (void)s1; (void)s2; (void)s3; (void)s4; (void)s5; (void)s6; (void)s7;
    *sid = h_alloc(64, true);
    return TRUE;
}
static void* WINAPI FreeSid(void* s) { h_free(s); return nullptr; }
static BOOL WINAPI CheckTokenMembership(HANDLE t, void* sid, BOOL* m) { (void)t; (void)sid; *m = FALSE; return TRUE; }
static BOOL WINAPI LookupPrivilegeValueW(const WCHAR* s, const WCHAR* n, void* l) { (void)s; (void)n; memset(l, 0, 8); return TRUE; }
static BOOL WINAPI AdjustTokenPrivileges(HANDLE t, BOOL d, void* n, u32 l, void* p, u32* r) { (void)t; (void)d; (void)n; (void)l; (void)p; (void)r; return TRUE; }
static HANDLE WINAPI RegisterEventSourceW(const WCHAR* s, const WCHAR* n) { (void)s; (void)n; return (HANDLE)0x7071; }
static BOOL WINAPI DeregisterEventSource(HANDLE h) { (void)h; return TRUE; }
static BOOL WINAPI ReportEventW(HANDLE h, u16 t, u16 c, u32 id, void* sid, u16 n, u32 d, const WCHAR** s, void* raw) { (void)h; (void)t; (void)c; (void)id; (void)sid; (void)n; (void)d; (void)s; (void)raw; return TRUE; }
static u32 WINAPI EventRegister(const void* g, void* cb, void* ctx, u64* h) { (void)g; (void)cb; (void)ctx; *h = 1; return 0; }
static u32 WINAPI EventUnregister(u64 h) { (void)h; return 0; }
static u32 WINAPI EventWriteTransfer(u64 h, const void* d, const void* a, const void* r, u32 n, void* data) { (void)h; (void)d; (void)a; (void)r; (void)n; (void)data; return 0; }
static u32 WINAPI EventSetInformation(u64 h, int c, void* i, u32 n) { (void)h; (void)c; (void)i; (void)n; return 0; }

// ---------------------------------------------------------------- ntdll odds and ends
struct RTL_OSVERSIONINFOEXW { u32 dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId; WCHAR szCSDVersion[128]; u16 a, b, c; u8 d, e; };
static NTSTATUS WINAPI RtlGetVersion(RTL_OSVERSIONINFOEXW* v) {
    u32 sz = v->dwOSVersionInfoSize;
    memset((u8*)v + 4, 0, sz > 4 ? sz - 4 : 0);
    v->dwMajorVersion = 10; v->dwMinorVersion = 0; v->dwBuildNumber = 19045; v->dwPlatformId = 2;
    return 0;
}
static void WINAPI RtlGetNtVersionNumbers(u32* ma, u32* mi, u32* b) { if (ma) *ma = 10; if (mi) *mi = 0; if (b) *b = 0xF0000000 | 19045; }
static void WINAPI RtlInitUnicodeString(UNICODE_STRING* u, const WCHAR* s) {
    u->Buffer = (WCHAR*)s;
    u->Length = s ? (u16)(wlen(s) * 2) : 0;
    u->MaximumLength = s ? (u16)(u->Length + 2) : 0;
}
static void WINAPI RtlInitAnsiString(ANSI_STRING* a, const char* s) {
    a->Buffer = (char*)s;
    a->Length = s ? (u16)slen(s) : 0;
    a->MaximumLength = s ? (u16)(a->Length + 1) : 0;
}
static void WINAPI RtlFreeUnicodeString(UNICODE_STRING* u) { if (u->Buffer) h_free(u->Buffer); u->Buffer = nullptr; u->Length = u->MaximumLength = 0; }
static NTSTATUS WINAPI RtlAnsiStringToUnicodeString(UNICODE_STRING* u, const ANSI_STRING* a, u8 alloc) {
    int n = wide_from_utf8(a->Buffer, a->Length, nullptr, 0);
    if (alloc) { u->Buffer = (WCHAR*)h_alloc(((usize)n + 1) * 2); u->MaximumLength = (u16)((n + 1) * 2); }
    else if (u->MaximumLength < (n + 1) * 2) return (NTSTATUS)0x80000005;
    wide_from_utf8(a->Buffer, a->Length, u->Buffer, n);
    u->Buffer[n] = 0;
    u->Length = (u16)(n * 2);
    return 0;
}
static u32 WINAPI RtlNtStatusToDosError(NTSTATUS s) {
    switch ((u32)s) {
    case 0: return 0;
    case 0xC0000005: return ERROR_NOACCESS;
    case 0xC0000008: return ERROR_INVALID_HANDLE;
    case 0xC000000D: return ERROR_INVALID_PARAMETER;
    case 0xC0000017: return ERROR_NOT_ENOUGH_MEMORY;
    case 0xC0000022: return ERROR_ACCESS_DENIED;
    case 0xC0000034: return ERROR_FILE_NOT_FOUND;
    case 0xC000003A: return ERROR_PATH_NOT_FOUND;
    case 0xC00000BB: return ERROR_NOT_SUPPORTED;
    case 0xC0000002: return ERROR_CALL_NOT_IMPLEMENTED;
    case 0x00000102: return WAIT_TIMEOUT;
    }
    return 317;
}
static u32 WINAPI RtlGetLastWin32Error() { return get_error(); }
static void WINAPI RtlSetLastWin32Error(u32 e) { set_error(e); }
static PEB* WINAPI RtlGetCurrentPeb() { return peb; }
static void* WINAPI RtlAllocateHeap(HANDLE h, u32 f, SIZE_T n) { (void)h; return h_alloc(n, f & 8); }
static u8 WINAPI RtlFreeHeap(HANDLE h, u32 f, void* p) { (void)h; (void)f; h_free(p); return 1; }
static void* WINAPI RtlReAllocateHeap(HANDLE h, u32 f, void* p, SIZE_T n) { (void)h; return h_realloc(p, n, f & 8); }
static SIZE_T WINAPI RtlSizeHeap(HANDLE h, u32 f, void* p) { (void)h; (void)f; return h_size(p); }
static u64 WINAPI RtlCompareMemory(const void* a, const void* b, u64 n) { u64 i = 0; while (i < n && ((const u8*)a)[i] == ((const u8*)b)[i]) i++; return i; }
static void WINAPI RtlZeroMemory(void* d, u64 n) { memset(d, 0, n); }
static void WINAPI RtlFillMemory(void* d, u64 n, u8 c) { memset(d, c, n); }
static void WINAPI RtlMoveMemory(void* d, const void* s, u64 n) { memmove(d, s, n); }
static void WINAPI RtlCopyMemory(void* d, const void* s, u64 n) { memmove(d, s, n); }
static u16 WINAPI RtlCaptureStackBackTrace(u32 skip, u32 n, void** out, u32* hash) {
    CONTEXT c;
    rt_RtlCaptureContext(&c);
    u16 k = 0;
    u32 h = 0;
    for (u32 i = 0; i < skip + n + 1 && k < n; i++) {
        u64 base = 0;
        RUNTIME_FUNCTION* fn = lookup_function(c.Rip, &base);
        void* data; u64 frame;
        if (fn) rt_RtlVirtualUnwind(0, base, c.Rip, fn, &c, &data, &frame, nullptr);
        else { if (!vm_readable((void*)c.Rsp, 8)) break; c.Rip = *(u64*)c.Rsp; c.Rsp += 8; }
        if (!c.Rip) break;
        if (i >= skip) { out[k++] = (void*)c.Rip; h += (u32)c.Rip; }
    }
    if (hash) *hash = h;
    return k;
}
static NTSTATUS WINAPI NtQueryInformationProcess(HANDLE p, int c, void* b, u32 n, u32* r) {
    (void)p;
    if (c == 0 && n >= 48) {                                    // ProcessBasicInformation
        memset(b, 0, 48);
        ((u64*)b)[1] = (u64)peb;
        ((u64*)b)[4] = 0x40;
        if (r) *r = 48;
        return 0;
    }
    if (c == 7 && n >= 8) { *(u64*)b = 0; if (r) *r = 8; return 0; }        // ProcessDebugPort: none
    return (NTSTATUS)0xC0000003;                                // STATUS_INVALID_INFO_CLASS
}
static NTSTATUS WINAPI NtQuerySystemInformation(int c, void* b, u32 n, u32* r) { (void)c; (void)b; (void)n; if (r) *r = 0; return (NTSTATUS)0xC0000003; }
static NTSTATUS WINAPI NtSetInformationThread(HANDLE h, int c, void* b, u32 n) { (void)h; (void)c; (void)b; (void)n; return 0; }
static NTSTATUS WINAPI NtQueryInformationThread(HANDLE h, int c, void* b, u32 n, u32* r) {
    (void)h;
    if (c == 0 && n >= 48) {                                    // ThreadBasicInformation
        memset(b, 0, 48);
        ((u64*)b)[1] = (u64)teb();
        ((u64*)b)[2] = 0x40;
        ((u64*)b)[3] = cur_thr()->id;
        if (r) *r = 48;
        return 0;
    }
    return (NTSTATUS)0xC0000003;
}
static NTSTATUS WINAPI NtDelayExecution(u8 alert, LARGE_INTEGER* t) {
    (void)alert;
    i64 v = t->QuadPart;
    if (v < 0) si::sleep((unsigned)((-v) / 10000));
    else si::yield();
    return 0;
}
static NTSTATUS WINAPI NtYieldExecution() { si::yield(); return 0; }
static NTSTATUS WINAPI NtClose(HANDLE h) { return h_close(h) ? 0 : (NTSTATUS)0xC0000008; }
static NTSTATUS WINAPI NtQueryPerformanceCounter(LARGE_INTEGER* c, LARGE_INTEGER* f) { c->QuadPart = (i64)si::ticks_us(); if (f) f->QuadPart = 1000000; return 0; }
static NTSTATUS WINAPI RtlQueryPerformanceCounter(LARGE_INTEGER* c) { c->QuadPart = (i64)si::ticks_us(); return 1; }
static NTSTATUS WINAPI RtlQueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000; return 1; }
static u32 WINAPI RtlRandomEx(u32* seed) { *seed = *seed * 1103515245u + 12345u; return *seed & 0x7FFFFFFF; }
static void* WINAPI RtlPcToFileHeader(void* pc, void** base) { Module* m = mod_by_addr(pc); *base = m ? m->base : nullptr; return *base; }
static NTSTATUS WINAPI RtlGetSystemTimePrecise(LARGE_INTEGER* t) { (void)t; return 0; }
static NTSTATUS WINAPI NtTerminateProcess(HANDLE h, NTSTATUS s) { if (!h || (u64)h == (u64)-1) { files_flush_all(); si::exit((int)s); } return (NTSTATUS)0xC0000022; }
static void WINAPI RtlExitUserProcess(u32 code) { process_exit(code); }
static void WINAPI RtlExitUserThread(u32 code) { thr_exit(code); }
static u8 WINAPI RtlDllShutdownInProgress() { return 0; }
static NTSTATUS WINAPI LdrGetDllHandle(const WCHAR* p, u32* f, UNICODE_STRING* n, HMODULE* out) {
    (void)p; (void)f;
    char u[MAX_PATH * 3];
    int k = utf8_from_wide(n->Buffer, n->Length / 2, u, sizeof(u) - 1);
    u[k] = 0;
    Module* m = mod_find(u);
    if (!m) return (NTSTATUS)0xC0000135;
    *out = m->base;
    return 0;
}

static const Export text_exports[] = {
    E(MultiByteToWideChar), E(WideCharToMultiByte), E(GetACP), E(GetOEMCP), E(GetCPInfo), E(GetCPInfoExW), E(IsValidCodePage),
    E(IsDBCSLeadByte), E(IsDBCSLeadByteEx), E(lstrlenA), E(lstrlenW), E(lstrcpyA), E(lstrcpyW), E(lstrcpynA), E(lstrcpynW), E(lstrcatA),
    E(lstrcatW), E(lstrcmpA), E(lstrcmpW), E(lstrcmpiA), E(lstrcmpiW), E(CompareStringA), E(CompareStringW), E(CompareStringEx),
    E(CompareStringOrdinal), E(LCMapStringA), E(LCMapStringW), E(LCMapStringEx), E(GetStringTypeA), E(GetStringTypeW),
    E(GetStringTypeExA), E(GetStringTypeExW), E(FoldStringW), E(NormalizeString), E(IsNormalizedString),
    E(GetUserDefaultLCID), E(GetSystemDefaultLCID), E(GetThreadLocale), E(SetThreadLocale), E(GetUserDefaultLangID),
    E(GetSystemDefaultLangID), E(GetUserDefaultUILanguage), E(GetSystemDefaultUILanguage), E(GetThreadUILanguage), E(SetThreadUILanguage),
    E(GetUserDefaultLocaleName), E(GetSystemDefaultLocaleName), E(LocaleNameToLCID), E(LCIDToLocaleName), E(IsValidLocale),
    E(IsValidLocaleName), E(ResolveLocaleName), E(GetLocaleInfoA), E(GetLocaleInfoW), E(GetLocaleInfoEx), E(SetLocaleInfoW),
    E(EnumSystemLocalesW), E(EnumSystemLocalesEx), E(GetUserGeoID), E(GetUserPreferredUILanguages), E(GetThreadPreferredUILanguages),
    E(GetProcessPreferredUILanguages), E(SetProcessPreferredUILanguages), E(GetDateFormatA), E(GetDateFormatW), E(GetDateFormatEx),
    E(GetTimeFormatA), E(GetTimeFormatW), E(GetTimeFormatEx), E(GetNumberFormatW), E(GetNumberFormatEx), E(GetCalendarInfoW),
    E(GetCalendarInfoEx), E(RtlCaptureStackBackTrace), EA("RtlPcToFileHeader", RtlPcToFileHeader),
};
EXPORT_TABLE("kernel32.dll", text_exports);

static const Export adv_text[] = {
    E(SystemFunction036), E(CryptAcquireContextW), E(CryptAcquireContextA), E(CryptGenRandom), E(CryptReleaseContext),
    E(RegOpenKeyExW), E(RegOpenKeyExA), E(RegOpenKeyW), E(RegOpenKeyA), E(RegCreateKeyExW), E(RegCreateKeyExA), E(RegQueryValueExW),
    E(RegQueryValueExA), E(RegGetValueW), E(RegSetValueExW), E(RegSetValueExA), E(RegDeleteValueW), E(RegDeleteKeyW), E(RegEnumKeyExW),
    E(RegEnumValueW), E(RegQueryInfoKeyW), E(RegCloseKey), E(RegFlushKey), E(OpenProcessToken), E(OpenThreadToken), E(GetTokenInformation),
    E(AllocateAndInitializeSid), E(FreeSid), E(CheckTokenMembership), E(LookupPrivilegeValueW), E(AdjustTokenPrivileges),
    E(RegisterEventSourceW), E(DeregisterEventSource), E(ReportEventW), E(EventRegister), E(EventUnregister), E(EventWriteTransfer),
    E(EventSetInformation),
};
EXPORT_TABLE("advapi32.dll", adv_text);

static const Export bcrypt_exports[] = { E(BCryptGenRandom) };
EXPORT_TABLE("bcrypt.dll", bcrypt_exports);
static const Export bcp_exports[] = { E(ProcessPrng) };
EXPORT_TABLE("bcryptprimitives.dll", bcp_exports);

static const Export nt_text[] = {
    E(RtlGetVersion), E(RtlGetNtVersionNumbers), E(RtlInitUnicodeString), E(RtlInitAnsiString), E(RtlFreeUnicodeString),
    E(RtlAnsiStringToUnicodeString), E(RtlNtStatusToDosError), E(RtlGetLastWin32Error), E(RtlSetLastWin32Error), E(RtlGetCurrentPeb),
    E(RtlAllocateHeap), E(RtlFreeHeap), E(RtlReAllocateHeap), E(RtlSizeHeap), E(RtlCompareMemory), E(RtlZeroMemory), E(RtlFillMemory),
    E(RtlMoveMemory), E(RtlCopyMemory), E(RtlCaptureStackBackTrace), E(NtQueryInformationProcess), E(NtQuerySystemInformation),
    E(NtSetInformationThread), E(NtQueryInformationThread), E(NtDelayExecution), E(NtYieldExecution), E(NtClose),
    E(NtQueryPerformanceCounter), E(RtlQueryPerformanceCounter), E(RtlQueryPerformanceFrequency), E(RtlRandomEx), E(RtlPcToFileHeader),
    E(RtlGetSystemTimePrecise), E(NtTerminateProcess), E(RtlExitUserProcess), E(RtlExitUserThread), E(RtlDllShutdownInProgress),
    E(LdrGetDllHandle), EA("RtlGenRandom", SystemFunction036),
    EA("ZwQueryInformationProcess", NtQueryInformationProcess), EA("ZwQuerySystemInformation", NtQuerySystemInformation),
    EA("ZwSetInformationThread", NtSetInformationThread), EA("ZwDelayExecution", NtDelayExecution), EA("ZwClose", NtClose),
};
EXPORT_TABLE("ntdll.dll", nt_text);

}  // namespace rt
