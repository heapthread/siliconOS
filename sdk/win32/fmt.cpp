// win32run - formatted output: one printf engine for the runtime's messages and for the C runtime
// (printf, sprintf, fprintf, the wide variants and UCRT's __stdio_common_* all end up here), and the
// number conversions that go with it. Floating point is printed exactly (the digits come from
// arbitrary-precision arithmetic in base 10^9, rounded half to even), so %.17g round-trips.
#include "rt.h"

namespace rt {

// ---------------------------------------------------------------- floating point -> text
static char* put_u(u64 x, char* end) {            // digits of x, written backwards from end
    do { *--end = (char)('0' + x % 10); x /= 10; } while (x);
    return end;
}

struct Buf {
    char* p;
    void add(const char* s, usize n) { memcpy(p, s, n); p += n; }
    void ch(char c) { *p++ = c; }
    void zeros(int n) { for (; n > 0; n--) *p++ = '0'; }
};

static int fmt_hex(char* out, double v, char conv, int prec, bool alt) {
    Buf o = {out};
    u64 bits;
    memcpy(&bits, &v, 8);
    int e = (int)((bits >> 52) & 0x7FF);
    u64 m = bits & 0xFFFFFFFFFFFFFull;
    bool up = conv == 'A';
    int lead = 1;
    if (e == 0) { lead = 0; e = m ? -1022 : 0; } else e -= 1023;
    if (prec >= 0 && prec < 13) {                 // round the 52-bit fraction to prec hex digits (half to even)
        int drop = (13 - prec) * 4;
        u64 full = ((u64)lead << 52) | m;
        u64 rem = full & ((1ull << drop) - 1), half = 1ull << (drop - 1);
        full >>= drop;
        if (rem > half || (rem == half && (full & 1))) full++;
        full <<= drop;
        lead = (int)(full >> 52);
        m = full & 0xFFFFFFFFFFFFFull;
    }
    o.ch('0');
    o.ch(up ? 'X' : 'x');
    o.ch((char)('0' + lead));
    char hex[13];
    int nd = 13;
    for (int i = 0; i < 13; i++) { int d = (int)((m >> (48 - i * 4)) & 15); hex[i] = (char)(d < 10 ? '0' + d : (up ? 'A' : 'a') + d - 10); }
    if (prec < 0) { while (nd > 0 && hex[nd - 1] == '0') nd--; } else nd = prec < 13 ? prec : 13;
    if (nd || alt || prec > 0) o.ch('.');
    o.add(hex, (usize)nd);
    if (prec > 13) o.zeros(prec - 13);
    o.ch(up ? 'P' : 'p');
    o.ch(e < 0 ? '-' : '+');
    char eb[8];
    char* es = put_u((u64)(e < 0 ? -e : e), eb + 8);
    o.add(es, (usize)(eb + 8 - es));
    return (int)(o.p - out);
}

// v >= 0 and finite. Writes the conversion without sign or padding; returns its length. The buffer must
// hold 350 + prec characters.
int fmt_double(char* out, double v, char conv, int prec, int flags) {
    bool alt = (flags & 1) != 0;
    char t = conv;
    if ((t | 32) == 'a') return fmt_hex(out, v, conv, prec, alt);
    int p = prec < 0 ? 6 : prec;
    Buf o = {out};

    static const int N = 420;
    u32 big[N];
    u32 *a, *d, *r, *z;
    int e2 = 0, e, i, j;
    // v = y * 2^e2 with y in [1, 2); long double keeps every bit of a double with room to spare
    long double y = v;
    if (v != 0) {
        u64 bits;
        memcpy(&bits, &v, 8);
        int be = (int)((bits >> 52) & 0x7FF);
        u64 m = bits & 0xFFFFFFFFFFFFFull;
        if (be == 0) {                             // subnormal: normalize by hand
            int sh = 0;
            while (!(m & (1ull << 52))) { m <<= 1; sh++; }
            e2 = -1022 - sh;
            y = (long double)m / (long double)(1ull << 52);
        } else {
            e2 = be - 1023;
            y = (long double)(m | (1ull << 52)) / (long double)(1ull << 52);
        }
        y *= 268435456.0L;                         // 2^28: the first base-10^9 digit holds 29 bits
        e2 -= 28;
    }
    if (e2 < 0) a = r = z = big;
    else a = r = z = big + N - 80;
    do {
        *z = (u32)y;
        y = 1000000000.0L * (y - (long double)*z++);
    } while (y != 0);

    while (e2 > 0) {                               // multiply by 2^e2
        u32 carry = 0;
        int sh = e2 < 29 ? e2 : 29;
        for (d = z - 1; d >= a; d--) {
            u64 x = ((u64)*d << sh) + carry;
            *d = (u32)(x % 1000000000);
            carry = (u32)(x / 1000000000);
        }
        if (carry) *--a = carry;
        while (z > a && !z[-1]) z--;
        e2 -= sh;
    }
    while (e2 < 0) {                               // divide by 2^-e2
        u32 carry = 0, *b;
        int sh = -e2 < 9 ? -e2 : 9, need = 1 + (p + 53 / 3 + 8) / 9;
        for (d = a; d < z; d++) {
            u32 rm = *d & ((1u << sh) - 1);
            *d = (*d >> sh) + carry;
            carry = (1000000000u >> sh) * rm;
        }
        if (!*a) a++;
        if (carry) *z++ = carry;
        b = (t | 32) == 'f' ? r : a;               // digits past the requested precision aren't needed...
        if (z - b > need) { z = b + need; z[-1] |= 1; }    // ...except to know that they weren't all zero
        e2 += sh;
    }

    if (a < z) for (i = 10, e = 9 * (int)(r - a); *a >= (u32)i; i *= 10, e++) {}
    else e = 0;

    // round to the requested number of digits
    j = p - ((t | 32) != 'f' ? e : 0) - ((t | 32) == 'g' && p ? 1 : 0);
    if (j < 9 * (int)(z - r - 1)) {
        u32 x;
        d = r + 1 + ((j + 9 * 1100) / 9 - 1100);
        j += 9 * 1100;
        j %= 9;
        for (i = 10, j++; j < 9; i *= 10, j++) {}
        x = *d % (u32)i;
        if (x || d + 1 != z) {
            bool odd = ((*d / (u32)i) & 1) || (i == 1000000000 && d > a && (d[-1] & 1));
            bool round_up = x > (u32)i / 2 || (x == (u32)i / 2 && (d + 1 != z || odd));
            *d -= x;
            if (round_up) {
                *d += (u32)i;
                while (*d > 999999999) {
                    *d-- = 0;
                    if (d < a) *--a = 0;
                    (*d)++;
                }
                for (i = 10, e = 9 * (int)(r - a); *a >= (u32)i; i *= 10, e++) {}
            }
        }
        if (z > d + 1) z = d + 1;
    }
    for (; z > a && !z[-1]; z--) {}

    if ((t | 32) == 'g') {
        if (!p) p++;
        if (p > e && e >= -4) { t--; p -= e + 1; }
        else { t -= 2; p--; }
        if (!alt) {                                // %g drops trailing zeros
            if (z > a && z[-1]) for (i = 10, j = 0; z[-1] % (u32)i == 0; i *= 10, j++) {}
            else j = 9;
            int lim = (t | 32) == 'f' ? 9 * (int)(z - r - 1) - j : 9 * (int)(z - r - 1) + e - j;
            if (lim < 0) lim = 0;
            if (p > lim) p = lim;
        }
    }

    char buf[12];
    char* s;
    if ((t | 32) == 'f') {
        if (a > r) a = r;
        for (d = a; d <= r; d++) {
            s = put_u(*d, buf + 9);
            if (d != a) while (s > buf) *--s = '0';
            else if (s == buf + 9) *--s = '0';
            o.add(s, (usize)(buf + 9 - s));
        }
        if (p || alt) o.ch('.');
        for (; d < z && p > 0; d++, p -= 9) {
            s = put_u(*d, buf + 9);
            while (s > buf) *--s = '0';
            o.add(s, (usize)(p < 9 ? p : 9));
        }
        o.zeros(p);
    } else {
        if (z <= a) z = a + 1;
        for (d = a; d < z && p >= 0; d++) {
            s = put_u(*d, buf + 9);
            if (s == buf + 9) *--s = '0';
            if (d != a) while (s > buf) *--s = '0';
            else {
                o.ch(*s++);
                if (p > 0 || alt) o.ch('.');
            }
            int n = (int)(buf + 9 - s);
            o.add(s, (usize)(n < p ? n : p));
            p -= n;
        }
        o.zeros(p);
        o.ch(t);                                   // 'e' or 'E'
        o.ch(e < 0 ? '-' : '+');
        char eb[8];
        char* es = put_u((u64)(e < 0 ? -e : e), eb + 8);
        while (eb + 8 - es < ((flags & 2) ? 3 : 2)) *--es = '0';
        o.add(es, (usize)(eb + 8 - es));
    }
    return (int)(o.p - out);
}

// ---------------------------------------------------------------- text -> floating point
static long double pow10l(int e) {
    long double r = 1, b = 10;
    bool neg = e < 0;
    if (neg) e = -e;
    for (; e; e >>= 1, b *= b) if (e & 1) r *= b;
    return neg ? 1 / r : r;
}

template <typename C>
static double parse_double_t(const C* s, const C** end) {
    const C* p = s;
    while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    auto is = [&](const char* w) { for (int i = 0; w[i]; i++) if (lower((int)p[i]) != w[i]) return false; return true; };
    if (is("inf")) {
        p += is("infinity") ? 8 : 3;
        if (end) *end = p;
        double inf = __builtin_inf();
        return neg ? -inf : inf;
    }
    if (is("nan")) {
        p += 3;
        if (*p == '(') { const C* q = p + 1; while (*q && *q != ')') q++; if (*q == ')') p = q + 1; }
        if (end) *end = p;
        return __builtin_nan("");
    }
    long double v = 0;
    bool any = false;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {                  // hexadecimal: 0x1.8p3
        const C* q = p + 2;
        int exp = 0;
        bool dot = false;
        for (;; q++) {
            int c = lower((int)*q), dg;
            if (c == '.' && !dot) { dot = true; continue; }
            if (c >= '0' && c <= '9') dg = c - '0';
            else if (c >= 'a' && c <= 'f') dg = c - 'a' + 10;
            else break;
            v = v * 16 + dg;
            if (dot) exp -= 4;
            any = true;
        }
        if (!any) { if (end) *end = p + 1; return neg ? -0.0 : 0.0; }
        if (*q == 'p' || *q == 'P') {
            const C* x = q + 1;
            bool en = false;
            if (*x == '+' || *x == '-') en = *x++ == '-';
            if (*x >= '0' && *x <= '9') {
                int ev = 0;
                while (*x >= '0' && *x <= '9') { if (ev < 100000) ev = ev * 10 + (*x - '0'); x++; }
                exp += en ? -ev : ev;
                q = x;
            }
        }
        while (exp > 0) { v *= 2; exp--; if (v > 1e4000L) break; }
        while (exp < 0) { v /= 2; exp++; if (v == 0) break; }
        if (end) *end = q;
        return (double)(neg ? -v : v);
    }
    u64 mant = 0;
    int digits = 0, exp10 = 0;
    bool dot = false;
    const C* q = p;
    for (;; q++) {
        if (*q == '.' && !dot) { dot = true; continue; }
        if (*q < '0' || *q > '9') break;
        any = true;
        if (digits < 19) {
            mant = mant * 10 + (u64)(*q - '0');
            if (mant) digits++;
            if (dot) exp10--;
        } else if (!dot) exp10++;
    }
    if (!any) { if (end) *end = s; return 0; }
    if (*q == 'e' || *q == 'E') {
        const C* x = q + 1;
        bool en = false;
        if (*x == '+' || *x == '-') en = *x++ == '-';
        if (*x >= '0' && *x <= '9') {
            int ev = 0;
            while (*x >= '0' && *x <= '9') { if (ev < 100000) ev = ev * 10 + (*x - '0'); x++; }
            exp10 += en ? -ev : ev;
            q = x;
        }
    }
    if (end) *end = q;
    if (!mant) return neg ? -0.0 : 0.0;
    if (exp10 > 400) v = __builtin_infl();
    else if (exp10 < -400) v = 0;
    else if (exp10 >= 0) v = (long double)mant * pow10l(exp10);
    else v = (long double)mant / pow10l(-exp10);
    return (double)(neg ? -v : v);
}

double parse_double(const char* s, const char** end) { return parse_double_t<char>(s, end); }
double wparse_double(const WCHAR* s, const WCHAR** end) { return parse_double_t<WCHAR>(s, end); }

// ---------------------------------------------------------------- the printf engine
static void pad(Sink* out, char c, int n) {
    char b[32];
    memset(b, c, sizeof(b));
    while (n > 0) { int k = n > 32 ? 32 : n; out->put(out, b, (usize)k); out->total += (usize)k; n -= k; }
}
static void emit(Sink* out, const char* s, usize n) { if (n) { out->put(out, s, n); out->total += n; } }

int vformat(Sink* out, const char* fmt, va_list ap, int fl) {
    out->total = 0;
    for (const char* f = fmt; *f;) {
        if (*f != '%') {
            const char* s = f;
            while (*f && *f != '%') f++;
            emit(out, s, (usize)(f - s));
            continue;
        }
        f++;
        if (*f == '%') { emit(out, "%", 1); f++; continue; }
        bool left = false, plus = false, space = false, alt = false, zero = false;
        for (;; f++) {
            if (*f == '-') left = true;
            else if (*f == '+') plus = true;
            else if (*f == ' ') space = true;
            else if (*f == '#') alt = true;
            else if (*f == '0') zero = true;
            else break;
        }
        int width = 0, prec = -1;
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { left = true; width = -width; } f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        // length: on Windows a long is 32 bits
        int size = 4;
        bool wide = false, narrow = false;
        for (;; f++) {
            if (*f == 'h') { narrow = true; size = f[1] == 'h' ? 1 : 2; if (f[1] == 'h') f++; }
            else if (*f == 'l') { wide = true; if (f[1] == 'l') { size = 8; f++; } else size = 4; }
            else if (*f == 'L') size = 4;
            else if (*f == 'j' || *f == 'z' || *f == 't') size = 8;
            else if (*f == 'w') wide = true;
            else if (*f == 'I') {
                if (f[1] == '6' && f[2] == '4') { size = 8; f += 2; }
                else if (f[1] == '3' && f[2] == '2') { size = 4; f += 2; }
                else size = 8;
            } else break;
        }
        char c = *f;
        if (!c) break;
        f++;
        char num[40];
        char tmp[8];
        const char* body = nullptr;
        usize blen = 0;
        const char* prefix = "";
        usize plen = 0;
        int zeros = 0;
        char* heap = nullptr;
        bool numeric = false;

        switch (c) {
        case 'd': case 'i': {
            i64 v = size == 8 ? va_arg(ap, i64) : (i64)va_arg(ap, int);
            if (size == 2) v = (i16)v; else if (size == 1) v = (i8)v;
            u64 u = v < 0 ? (u64)0 - (u64)v : (u64)v;
            char* s = put_u(u, num + sizeof(num));
            if (!u && prec == 0) s = num + sizeof(num);
            body = s; blen = (usize)(num + sizeof(num) - s);
            prefix = v < 0 ? "-" : plus ? "+" : space ? " " : "";
            plen = slen(prefix);
            numeric = true;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'p': {
            u64 u;
            if (c == 'p') { u = (u64)va_arg(ap, void*); if (prec < 0) prec = 16; }
            else {
                u = size == 8 ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned);
                if (size == 2) u = (u16)u; else if (size == 1) u = (u8)u;
            }
            char* s = num + sizeof(num);
            if (c == 'u') s = put_u(u, s);
            else if (c == 'o') { u64 x = u; do { *--s = (char)('0' + (x & 7)); x >>= 3; } while (x); if (alt && *s != '0') *--s = '0'; }
            else {
                const char* dg = c == 'x' ? "0123456789abcdef" : "0123456789ABCDEF";
                u64 x = u;
                do { *--s = dg[x & 15]; x >>= 4; } while (x);
                if (alt && u && c != 'p') { prefix = c == 'x' ? "0x" : "0X"; plen = 2; }
            }
            if (!u && prec == 0) s = num + sizeof(num);
            body = s; blen = (usize)(num + sizeof(num) - s);
            numeric = true;
            break;
        }
        case 'c': case 'C': {
            bool w = c == 'C' ? !(fl & FMT_LEGACY_WIDE) : ((fl & FMT_LEGACY_WIDE) != 0);
            if (wide) w = true;
            if (narrow) w = false;
            int v = va_arg(ap, int);
            if (w) { WCHAR wc[2] = {(WCHAR)v, 0}; blen = (usize)utf8_from_wide(wc, 1, tmp, sizeof(tmp)); }
            else { tmp[0] = (char)v; blen = 1; }
            body = tmp;
            break;
        }
        case 's': case 'S': {
            bool w = c == 'S' ? !(fl & FMT_LEGACY_WIDE) : ((fl & FMT_LEGACY_WIDE) != 0);
            if (wide) w = true;
            if (narrow) w = false;
            if (w) {
                const WCHAR* ws = va_arg(ap, const WCHAR*);
                if (!ws) ws = L"(null)";
                int n = 0;
                while (ws[n] && (prec < 0 || n < prec)) n++;
                int bytes = n ? utf8_from_wide(ws, n, nullptr, 0) : 0;
                heap = (char*)ialloc((usize)bytes + 1);
                if (heap && n) utf8_from_wide(ws, n, heap, bytes);
                body = heap ? heap : "";
                blen = heap ? (usize)bytes : 0;
            } else {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                usize n = 0;
                while (s[n] && (prec < 0 || n < (usize)prec)) n++;
                body = s; blen = n;
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            double v = va_arg(ap, double);
            u64 bits;
            memcpy(&bits, &v, 8);
            bool neg = (bits >> 63) != 0;
            prefix = neg ? "-" : plus ? "+" : space ? " " : "";
            plen = slen(prefix);
            bool up = c >= 'A' && c <= 'Z';
            if (((bits >> 52) & 0x7FF) == 0x7FF) {
                body = (bits & 0xFFFFFFFFFFFFFull) ? (up ? "NAN" : "nan") : (up ? "INF" : "inf");
                blen = 3;
                zero = false;
            } else {
                int want = prec < 0 ? 6 : prec;
                heap = (char*)ialloc((usize)want + 400);
                if (!heap) break;
                if (neg) v = -v;
                blen = (usize)fmt_double(heap, v, c == 'F' ? 'f' : c, prec, (alt ? 1 : 0) | ((fl & FMT_THREE_DIGIT_EXP) ? 2 : 0));
                body = heap;
                if (zero && !left) { int total = (int)(plen + blen); if (width > total) zeros = width - total; }
            }
            break;
        }
        case 'n': {
            void* dst = va_arg(ap, void*);
            if (dst) { if (size == 8) *(i64*)dst = (i64)out->total; else if (size == 2) *(i16*)dst = (i16)out->total; else *(int*)dst = (int)out->total; }
            continue;
        }
        default:                                   // not a conversion: print it as it stands
            emit(out, "%", 1);
            emit(out, &c, 1);
            continue;
        }
        if (numeric) {
            if (prec >= 0) { if ((usize)prec > blen) zeros = prec - (int)blen; }
            else if (zero && !left) { int total = (int)(plen + blen); if (width > total) zeros = width - total; }
        }
        int total = (int)(plen + blen) + zeros;
        if (!left && width > total) pad(out, ' ', width - total);
        emit(out, prefix, plen);
        pad(out, '0', zeros);
        emit(out, body, blen);
        if (left && width > total) pad(out, ' ', width - total);
        if (heap) ifree(heap);
    }
    return (int)out->total;
}

struct MemSink : Sink { char* buf; usize cap, len; };
static void mem_put(Sink* s, const char* p, usize n) {
    MemSink* m = (MemSink*)s;
    for (usize i = 0; i < n; i++) { if (m->len + 1 < m->cap) m->buf[m->len] = p[i]; m->len++; }
}

int vsnfmt(char* buf, usize cap, const char* fmt, va_list ap) {
    MemSink m;
    m.put = mem_put; m.total = 0; m.buf = buf; m.cap = cap; m.len = 0;
    int n = vformat(&m, fmt, ap);
    if (cap) buf[m.len < cap ? m.len : cap - 1] = 0;
    return n;
}

int snfmt(char* buf, usize cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnfmt(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

}  // namespace rt
