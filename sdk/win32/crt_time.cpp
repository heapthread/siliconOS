// win32run - the C runtime's time functions. silicon's clock is local time and the runtime reports a
// time zone of UTC, so local time and UTC are the same here.
#include "crt.h"

namespace rt {

struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
static u64 start_ms = 0;
static struct ClockStart { ClockStart() { start_ms = si::ticks(); } } clock_start;

static i64 now_unix() { return (i64)(filetime_now() / 10000000ull) - 11644473600ll; }
static bool leap(i64 y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static i64 CDECL c__time64(i64* t) { i64 v = now_unix(); if (t) *t = v; return v; }
static i32 CDECL c__time32(i32* t) { i32 v = (i32)now_unix(); if (t) *t = v; return v; }
static i32 CDECL c_clock() { return (i32)(si::ticks() - start_ms); }
static double CDECL c__difftime64(i64 a, i64 b) { return (double)(a - b); }
static double CDECL c__difftime32(i32 a, i32 b) { return (double)(a - b); }

static bool to_tm(i64 t, tm* out) {
    if (t < -62135596800ll || t > 32535215999ll) return false;
    i64 days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    out->tm_hour = (int)(rem / 3600); out->tm_min = (int)(rem / 60 % 60); out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((days + 4) % 7);
    if (out->tm_wday < 0) out->tm_wday += 7;
    i64 y = 1970;
    if (days >= 0) { for (;;) { i64 yd = leap(y) ? 366 : 365; if (days < yd) break; days -= yd; y++; } }
    else { while (days < 0) { y--; days += leap(y) ? 366 : 365; } }
    out->tm_year = (int)(y - 1900);
    out->tm_yday = (int)days;
    int m = 0;
    for (;; m++) { int md = mdays[m] + (m == 1 && leap(y)); if (days < md) break; days -= md; }
    out->tm_mon = m;
    out->tm_mday = (int)days + 1;
    out->tm_isdst = 0;
    return true;
}
static i64 from_tm(tm* t) {
    // normalize the fields first, as mktime must
    i64 y = t->tm_year + 1900, mon = t->tm_mon;
    y += mon / 12; mon %= 12;
    if (mon < 0) { mon += 12; y--; }
    i64 days = 0;
    if (y >= 1970) for (i64 i = 1970; i < y; i++) days += leap(i) ? 366 : 365;
    else for (i64 i = y; i < 1970; i++) days -= leap(i) ? 366 : 365;
    for (int i = 0; i < mon; i++) days += mdays[i] + (i == 1 && leap(y));
    days += t->tm_mday - 1;
    i64 v = days * 86400 + (i64)t->tm_hour * 3600 + (i64)t->tm_min * 60 + t->tm_sec;
    to_tm(v, t);
    return v;
}
static tm* tls_tm() { return (tm*)crt_thread()->tm_buf; }
static tm* CDECL c__gmtime64(const i64* t) { tm* r = tls_tm(); if (!to_tm(*t, r)) { set_errno(EINVAL); return nullptr; } return r; }
static tm* CDECL c__gmtime32(const i32* t) { i64 v = *t; return c__gmtime64(&v); }
static int CDECL c__gmtime64_s(tm* out, const i64* t) { if (!out || !t || !to_tm(*t, out)) { if (out) memset(out, 0xFF, sizeof(tm)); return EINVAL; } return 0; }
static int CDECL c__gmtime32_s(tm* out, const i32* t) { i64 v = *t; return c__gmtime64_s(out, &v); }
static i64 CDECL c__mktime64(tm* t) { return from_tm(t); }
static i32 CDECL c__mktime32(tm* t) { return (i32)from_tm(t); }
static i64 CDECL c__mkgmtime64(tm* t) { return from_tm(t); }
static i32 CDECL c__mkgmtime32(tm* t) { return (i32)from_tm(t); }

static const char* wday_names[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char* mon_names[12] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

static usize do_strftime(char* out, usize cap, const char* f, const tm* t) {
    usize o = 0;
    auto put = [&](const char* s) { while (*s) { if (o + 1 >= cap) return false; out[o++] = *s++; } return true; };
    char b[64];
    for (; *f; f++) {
        if (*f != '%') { b[0] = *f; b[1] = 0; if (!put(b)) return 0; continue; }
        f++;
        bool alt = false;
        if (*f == '#') { alt = true; f++; }
        if (*f == 'E' || *f == 'O') f++;
        int h12 = t->tm_hour % 12 ? t->tm_hour % 12 : 12;
        switch (*f) {
        case 'a': snfmt(b, sizeof(b), "%.3s", wday_names[t->tm_wday % 7]); break;
        case 'A': snfmt(b, sizeof(b), "%s", wday_names[t->tm_wday % 7]); break;
        case 'b': case 'h': snfmt(b, sizeof(b), "%.3s", mon_names[t->tm_mon % 12]); break;
        case 'B': snfmt(b, sizeof(b), "%s", mon_names[t->tm_mon % 12]); break;
        case 'c': snfmt(b, sizeof(b), alt ? "%s, %s %02d, %d %02d:%02d:%02d" : "%02d/%02d/%02d %02d:%02d:%02d", t->tm_mon + 1, t->tm_mday, t->tm_year % 100, t->tm_hour, t->tm_min, t->tm_sec); break;
        case 'C': snfmt(b, sizeof(b), "%02d", (t->tm_year + 1900) / 100); break;
        case 'd': snfmt(b, sizeof(b), alt ? "%d" : "%02d", t->tm_mday); break;
        case 'D': snfmt(b, sizeof(b), "%02d/%02d/%02d", t->tm_mon + 1, t->tm_mday, t->tm_year % 100); break;
        case 'e': snfmt(b, sizeof(b), "%2d", t->tm_mday); break;
        case 'F': snfmt(b, sizeof(b), "%d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday); break;
        case 'g': snfmt(b, sizeof(b), "%02d", t->tm_year % 100); break;
        case 'G': snfmt(b, sizeof(b), "%d", t->tm_year + 1900); break;
        case 'H': snfmt(b, sizeof(b), alt ? "%d" : "%02d", t->tm_hour); break;
        case 'I': snfmt(b, sizeof(b), alt ? "%d" : "%02d", h12); break;
        case 'j': snfmt(b, sizeof(b), alt ? "%d" : "%03d", t->tm_yday + 1); break;
        case 'm': snfmt(b, sizeof(b), alt ? "%d" : "%02d", t->tm_mon + 1); break;
        case 'M': snfmt(b, sizeof(b), alt ? "%d" : "%02d", t->tm_min); break;
        case 'n': scpy(b, "\n"); break;
        case 'p': scpy(b, t->tm_hour < 12 ? "AM" : "PM"); break;
        case 'r': snfmt(b, sizeof(b), "%02d:%02d:%02d %s", h12, t->tm_min, t->tm_sec, t->tm_hour < 12 ? "AM" : "PM"); break;
        case 'R': snfmt(b, sizeof(b), "%02d:%02d", t->tm_hour, t->tm_min); break;
        case 'S': snfmt(b, sizeof(b), alt ? "%d" : "%02d", t->tm_sec); break;
        case 't': scpy(b, "\t"); break;
        case 'T': case 'X': snfmt(b, sizeof(b), "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec); break;
        case 'u': snfmt(b, sizeof(b), "%d", t->tm_wday ? t->tm_wday : 7); break;
        case 'U': snfmt(b, sizeof(b), "%02d", (t->tm_yday + 7 - t->tm_wday) / 7); break;
        case 'V': {
            int wd = (t->tm_wday + 6) % 7;
            int w = (t->tm_yday - wd + 10) / 7;
            snfmt(b, sizeof(b), "%02d", w < 1 ? 52 : w > 52 ? 1 : w);
            break;
        }
        case 'w': snfmt(b, sizeof(b), "%d", t->tm_wday); break;
        case 'W': snfmt(b, sizeof(b), "%02d", (t->tm_yday + 7 - (t->tm_wday + 6) % 7) / 7); break;
        case 'x': snfmt(b, sizeof(b), alt ? "%s, %s %02d, %d" : "%02d/%02d/%02d", t->tm_mon + 1, t->tm_mday, t->tm_year % 100); break;
        case 'y': snfmt(b, sizeof(b), alt ? "%d" : "%02d", t->tm_year % 100); break;
        case 'Y': snfmt(b, sizeof(b), "%d", t->tm_year + 1900); break;
        case 'z': scpy(b, "+0000"); break;
        case 'Z': scpy(b, "Coordinated Universal Time"); break;
        case '%': scpy(b, "%"); break;
        case 0: f--; b[0] = 0; break;
        default: b[0] = '%'; b[1] = *f; b[2] = 0; break;
        }
        if (!put(b)) return 0;
    }
    if (o >= cap) return 0;
    out[o] = 0;
    return o;
}
static usize CDECL c_strftime(char* out, usize cap, const char* f, const tm* t) { if (!out || !f || !t || !cap) { set_errno(EINVAL); return 0; } return do_strftime(out, cap, f, t); }
static usize CDECL c__strftime_l(char* out, usize cap, const char* f, const tm* t, void* l) { (void)l; return c_strftime(out, cap, f, t); }
static usize CDECL c__Strftime(char* out, usize cap, const char* f, const tm* t, void* d) { (void)d; return c_strftime(out, cap, f, t); }
static usize CDECL c_wcsftime(WCHAR* out, usize cap, const WCHAR* f, const tm* t) {
    char* fu = utf8_dup(f);
    char tmp[1024];
    usize n = do_strftime(tmp, sizeof(tmp), fu, t);
    ifree(fu);
    if (!n && !tmp[0]) { if (cap) out[0] = 0; return 0; }
    int w = wide_from_utf8(tmp, -1, nullptr, 0);
    if ((usize)w > cap) { if (cap) out[0] = 0; return 0; }
    wide_from_utf8(tmp, -1, out, (int)cap);
    return (usize)w - 1;
}
static usize CDECL c__wcsftime_l(WCHAR* out, usize cap, const WCHAR* f, const tm* t, void* l) { (void)l; return c_wcsftime(out, cap, f, t); }

static void fmt_asctime(char* out, const tm* t) {
    snfmt(out, 26, "%.3s %.3s %2d %02d:%02d:%02d %d\n", wday_names[t->tm_wday % 7], mon_names[t->tm_mon % 12], t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, t->tm_year + 1900);
}
static char* CDECL c_asctime(const tm* t) { char* b = crt_thread()->asctime_buf; fmt_asctime(b, t); return b; }
static int CDECL c_asctime_s(char* out, usize n, const tm* t) { if (n < 26) return EINVAL; fmt_asctime(out, t); return 0; }
static WCHAR* CDECL c__wasctime(const tm* t) { char b[32]; fmt_asctime(b, t); WCHAR* w = crt_thread()->wasctime_buf; wide_from_utf8(b, -1, w, 32); return w; }
static int CDECL c__wasctime_s(WCHAR* out, usize n, const tm* t) { char b[32]; fmt_asctime(b, t); wide_from_utf8(b, -1, out, (int)n); return 0; }
static char* CDECL c__ctime64(const i64* t) { tm* r = c__gmtime64(t); return r ? c_asctime(r) : nullptr; }
static char* CDECL c__ctime32(const i32* t) { i64 v = *t; return c__ctime64(&v); }
static int CDECL c__ctime64_s(char* out, usize n, const i64* t) { tm r; if (c__gmtime64_s(&r, t)) return EINVAL; return c_asctime_s(out, n, &r); }
static int CDECL c__ctime32_s(char* out, usize n, const i32* t) { i64 v = *t; return c__ctime64_s(out, n, &v); }
static WCHAR* CDECL c__wctime64(const i64* t) { tm* r = c__gmtime64(t); return r ? c__wasctime(r) : nullptr; }
static WCHAR* CDECL c__wctime32(const i32* t) { i64 v = *t; return c__wctime64(&v); }
static int CDECL c__wctime64_s(WCHAR* out, usize n, const i64* t) { tm r; if (c__gmtime64_s(&r, t)) return EINVAL; return c__wasctime_s(out, n, &r); }

struct timeb64 { i64 time; u16 millitm; i16 timezone, dstflag; };
struct timeb32 { i32 time; u16 millitm; i16 timezone, dstflag; };
static void CDECL c__ftime64(timeb64* t) { u64 f = filetime_now(); t->time = (i64)(f / 10000000ull) - 11644473600ll; t->millitm = (u16)(f / 10000 % 1000); t->timezone = 0; t->dstflag = 0; }
static int CDECL c__ftime64_s(timeb64* t) { c__ftime64(t); return 0; }
static void CDECL c__ftime32(timeb32* t) { timeb64 x; c__ftime64(&x); t->time = (i32)x.time; t->millitm = x.millitm; t->timezone = 0; t->dstflag = 0; }
static int CDECL c__ftime32_s(timeb32* t) { c__ftime32(t); return 0; }
struct timespec64 { i64 tv_sec; i32 tv_nsec; };
static int CDECL c__timespec64_get(timespec64* ts, int base) {
    if (base != 1) return 0;
    u64 f = filetime_now();
    ts->tv_sec = (i64)(f / 10000000ull) - 11644473600ll;
    ts->tv_nsec = (i32)(f % 10000000ull * 100);
    return base;
}
struct timespec32 { i32 tv_sec; i32 tv_nsec; };
static int CDECL c__timespec32_get(timespec32* ts, int base) { timespec64 t; int r = c__timespec64_get(&t, base); ts->tv_sec = (i32)t.tv_sec; ts->tv_nsec = t.tv_nsec; return r; }

static i32 tz_v = 0;
static int dl_v = 0;
static i32 dstbias_v = -3600;
static char tzn0[] = "UTC", tzn1[] = "UTC";
static char* tzname_v[2] = {tzn0, tzn1};
static void CDECL c__tzset() {}
static int CDECL c__get_timezone(i32* s) { *s = tz_v; return 0; }
static int CDECL c__get_daylight(int* d) { *d = dl_v; return 0; }
static int CDECL c__get_dstbias(i32* d) { *d = dstbias_v; return 0; }
static int CDECL c__get_tzname(usize* r, char* buf, usize n, int i) { const char* s = tzname_v[i & 1]; if (r) *r = slen(s) + 1; if (buf) slcpy(buf, s, n); return 0; }
static i32* CDECL c___p__timezone() { return &tz_v; }
static int* CDECL c___p__daylight() { return &dl_v; }
static i32* CDECL c___p__dstbias() { return &dstbias_v; }
static char** CDECL c___p__tzname() { return tzname_v; }
static char* CDECL c__strdate(char* b) { tm t; i64 n = now_unix(); to_tm(n, &t); snfmt(b, 9, "%02d/%02d/%02d", t.tm_mon + 1, t.tm_mday, t.tm_year % 100); return b; }
static char* CDECL c__strtime(char* b) { tm t; i64 n = now_unix(); to_tm(n, &t); snfmt(b, 9, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec); return b; }
static int CDECL c__strdate_s(char* b, usize n) { if (n < 9) return ERANGE; c__strdate(b); return 0; }
static int CDECL c__strtime_s(char* b, usize n) { if (n < 9) return ERANGE; c__strtime(b); return 0; }
static WCHAR* CDECL c__wstrdate(WCHAR* b) { char t[16]; c__strdate(t); wide_from_utf8(t, -1, b, 9); return b; }
static WCHAR* CDECL c__wstrtime(WCHAR* b) { char t[16]; c__strtime(t); wide_from_utf8(t, -1, b, 9); return b; }
static u32 CDECL c__getsystime(tm* t) { u64 f = filetime_now(); to_tm((i64)(f / 10000000ull) - 11644473600ll, t); return (u32)(f / 10000 % 1000); }

static const Export time_exports[] = {
    X(_time64), X(_time32), EA("time", c__time64), X(clock), X(_difftime64), X(_difftime32), EA("difftime", c__difftime64),
    X(_gmtime64), X(_gmtime32), EA("gmtime", c__gmtime64), X(_gmtime64_s), X(_gmtime32_s), EA("gmtime_s", c__gmtime64_s),
    EA("_localtime64", c__gmtime64), EA("_localtime32", c__gmtime32), EA("localtime", c__gmtime64), EA("_localtime64_s", c__gmtime64_s),
    EA("_localtime32_s", c__gmtime32_s), EA("localtime_s", c__gmtime64_s), X(_mktime64), X(_mktime32), EA("mktime", c__mktime64),
    X(_mkgmtime64), X(_mkgmtime32), EA("_mkgmtime", c__mkgmtime64), X(strftime), X(_strftime_l), X(_Strftime), X(wcsftime), X(_wcsftime_l),
    X(asctime), X(asctime_s), X(_wasctime), X(_wasctime_s), X(_ctime64), X(_ctime32), EA("ctime", c__ctime64), X(_ctime64_s), X(_ctime32_s),
    X(_wctime64), X(_wctime32), X(_wctime64_s), EA("_wctime", c__wctime64), X(_ftime64), X(_ftime64_s), X(_ftime32), X(_ftime32_s),
    EA("_ftime", c__ftime64), X(_timespec64_get), X(_timespec32_get), EA("timespec_get", c__timespec64_get), X(_tzset),
    EA("tzset", c__tzset), X(_get_timezone), X(_get_daylight), X(_get_dstbias), X(_get_tzname), X(__p__timezone), X(__p__daylight),
    X(__p__dstbias), X(__p__tzname), EA("__timezone", c___p__timezone), EA("__daylight", c___p__daylight), EA("__tzname", c___p__tzname),
    EA("__dstbias", c___p__dstbias), X(_strdate), X(_strtime), X(_strdate_s), X(_strtime_s), X(_wstrdate), X(_wstrtime), X(_getsystime),
};
EXPORT_TABLE("ucrtbase.dll", time_exports);

static const Export time_msvcrt[] = {
    {"_timezone", (void*)&tz_v}, {"_daylight", (void*)&dl_v}, {"_tzname", (void*)tzname_v}, {"_dstbias", (void*)&dstbias_v},
};
EXPORT_TABLE("msvcrt.dll", time_msvcrt);

}  // namespace rt
