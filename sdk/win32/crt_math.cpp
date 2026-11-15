// win32run - the C runtime's math library. The transcendental functions use the x87 unit, which works
// in 80-bit precision internally, so double results are correctly rounded or within an ulp.
#include "crt.h"

namespace rt {

typedef long double ld;
static inline u64 bits(double x) { u64 b; memcpy(&b, &x, 8); return b; }
static inline double from_bits(u64 b) { double x; memcpy(&x, &b, 8); return x; }
static inline bool is_nan(double x) { return (bits(x) & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
static inline bool is_inf(double x) { return (bits(x) & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull; }
static inline bool sign_of(double x) { return bits(x) >> 63; }
static const double INF = __builtin_inf();
static const double NAN_ = __builtin_nan("");

static inline ld x87_sin(ld x) { ld r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static inline ld x87_cos(ld x) { ld r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
static inline ld x87_atan2(ld y, ld x) { ld r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
static inline ld x87_yl2x(ld x, ld y) { ld r; __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }        // y*log2(x)
static inline ld x87_yl2xp1(ld x, ld y) { ld r; __asm__("fyl2xp1" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }    // y*log2(x+1)
static inline ld x87_2xm1(ld x) { ld r; __asm__("f2xm1" : "=t"(r) : "0"(x)); return r; }
static inline ld x87_scale(ld x, ld e) { ld r; __asm__("fscale" : "=t"(r) : "0"(x), "u"(e)); return r; }
static inline ld x87_rnd(ld x) { ld r; __asm__("frndint" : "=t"(r) : "0"(x)); return r; }
static inline ld x87_sqrt(ld x) { ld r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
static inline ld x87_ln2() { ld r; __asm__("fldln2" : "=t"(r)); return r; }
static inline ld x87_lg2() { ld r; __asm__("fldlg2" : "=t"(r)); return r; }
static inline ld x87_l2e() { ld r; __asm__("fldl2e" : "=t"(r)); return r; }
static inline ld x87_pi() { ld r; __asm__("fldpi" : "=t"(r)); return r; }
static ld x87_prem(ld x, ld y, bool ieee) {
    ld r;
    if (ieee) __asm__("1: fprem1\n fnstsw %%ax\n testw $0x400, %%ax\n jnz 1b" : "=t"(r) : "0"(x), "u"(y) : "ax", "cc");
    else __asm__("1: fprem\n fnstsw %%ax\n testw $0x400, %%ax\n jnz 1b" : "=t"(r) : "0"(x), "u"(y) : "ax", "cc");
    return r;
}
// 2^x for any x (f2xm1 takes |x| <= 1)
static ld exp2l_(ld x) {
    if (x != x) return x;
    if (x > 16400) return (ld)INF;
    if (x < -16500) return 0;
    ld i = x87_rnd(x);
    return x87_scale(x87_2xm1(x - i) + 1, i);
}
static ld big_reduce(ld x) { return x87_prem(x, 2 * x87_pi(), true); }        // for huge arguments to sin/cos

static double CDECL c_sqrt(double x) { if (x < 0) { set_errno(EDOM); return NAN_; } double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static double CDECL c_fabs(double x) { return from_bits(bits(x) & 0x7FFFFFFFFFFFFFFFull); }
static double CDECL c_sin(double x) { if (is_inf(x)) { set_errno(EDOM); return NAN_; } ld a = x; if (c_fabs(x) > 9.2e18) a = big_reduce(a); return (double)x87_sin(a); }
static double CDECL c_cos(double x) { if (is_inf(x)) { set_errno(EDOM); return NAN_; } ld a = x; if (c_fabs(x) > 9.2e18) a = big_reduce(a); return (double)x87_cos(a); }
static double CDECL c_tan(double x) {
    if (is_inf(x)) { set_errno(EDOM); return NAN_; }
    ld a = x;
    if (c_fabs(x) > 9.2e18) a = big_reduce(a);
    return (double)(x87_sin(a) / x87_cos(a));
}
static double CDECL c_atan2(double y, double x) { return (double)x87_atan2(y, x); }
static double CDECL c_atan(double x) { return (double)x87_atan2(x, 1); }
static double CDECL c_asin(double x) { if (x < -1 || x > 1) { set_errno(EDOM); return NAN_; } ld a = x; return (double)x87_atan2(a, x87_sqrt((1 - a) * (1 + a))); }
static double CDECL c_acos(double x) { if (x < -1 || x > 1) { set_errno(EDOM); return NAN_; } ld a = x; return (double)x87_atan2(x87_sqrt((1 - a) * (1 + a)), a); }
static double CDECL c_exp(double x) {
    if (is_nan(x)) return x;
    if (x > 709.78) { set_errno(ERANGE); return INF; }
    if (x < -745.2) return 0;
    return (double)exp2l_((ld)x * x87_l2e());
}
static double CDECL c_exp2(double x) { if (x > 1024) { set_errno(ERANGE); return INF; } return (double)exp2l_(x); }
static double CDECL c_expm1(double x) {
    if (c_fabs(x) < 0.5) { ld t = (ld)x * x87_l2e(); return (double)x87_2xm1(t); }
    return c_exp(x) - 1;
}
static double CDECL c_log(double x) {
    if (is_nan(x)) return x;
    if (x < 0) { set_errno(EDOM); return NAN_; }
    if (x == 0) { set_errno(ERANGE); return -INF; }
    if (is_inf(x)) return x;
    return (double)x87_yl2x(x, x87_ln2());
}
static double CDECL c_log2(double x) { if (x < 0) { set_errno(EDOM); return NAN_; } if (x == 0) { set_errno(ERANGE); return -INF; } if (is_inf(x) || is_nan(x)) return x; return (double)x87_yl2x(x, 1); }
static double CDECL c_log10(double x) { if (x < 0) { set_errno(EDOM); return NAN_; } if (x == 0) { set_errno(ERANGE); return -INF; } if (is_inf(x) || is_nan(x)) return x; return (double)x87_yl2x(x, x87_lg2()); }
static double CDECL c_log1p(double x) {
    if (x < -1) { set_errno(EDOM); return NAN_; }
    if (x == -1) { set_errno(ERANGE); return -INF; }
    if (c_fabs(x) < 0.29) return (double)x87_yl2xp1(x, x87_ln2());
    return c_log(1 + x);
}
static double CDECL c_pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 1) return 1;
    if (is_nan(x) || is_nan(y)) return NAN_;
    bool yint = y == (double)(i64)y || c_fabs(y) >= 9007199254740992.0;
    bool yodd = yint && c_fabs(y) < 9007199254740992.0 && ((i64)y & 1);
    if (is_inf(y)) {
        double ax = c_fabs(x);
        if (ax == 1) return 1;
        return (ax > 1) == (y > 0) ? INF : 0;
    }
    if (x == 0) {
        if (y < 0) { set_errno(ERANGE); return yodd ? from_bits(bits(x) | 0x7FF0000000000000ull) : INF; }
        return yodd ? x : 0;
    }
    if (is_inf(x)) {
        if (x > 0) return y > 0 ? INF : 0;
        if (y > 0) return yodd ? -INF : INF;
        return yodd ? -0.0 : 0;
    }
    bool neg = false;
    if (x < 0) {
        if (!yint) { set_errno(EDOM); return NAN_; }
        neg = yodd;
        x = -x;
    }
    // small integer powers exactly by squaring, the rest through 2^(y*log2(x)) in extended precision
    if (yint && c_fabs(y) <= 64) {
        ld r = 1, b = x;
        u64 n = (u64)c_fabs(y);
        while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        if (y < 0) r = 1 / r;
        double d = (double)r;
        if (is_inf(d)) set_errno(ERANGE);
        return neg ? -d : d;
    }
    ld l = x87_yl2x(x, 1) * (ld)y;
    double d = (double)exp2l_(l);
    if (is_inf(d) || d == 0) set_errno(ERANGE);
    return neg ? -d : d;
}
static double CDECL c_sinh(double x) { if (c_fabs(x) < 1e-5) return x; double e = c_expm1(c_fabs(x)); double r = 0.5 * (e + e / (e + 1)); return x < 0 ? -r : r; }
static double CDECL c_cosh(double x) { double e = c_exp(c_fabs(x)); return 0.5 * (e + 1 / e); }
static double CDECL c_tanh(double x) {
    double a = c_fabs(x);
    if (a > 22) return x < 0 ? -1.0 : 1.0;
    double e = c_expm1(2 * a);
    double r = e / (e + 2);
    return x < 0 ? -r : r;
}
static double CDECL c_asinh(double x) { double a = c_fabs(x); double r = a > 1e8 ? c_log(a) + 0.6931471805599453 : c_log1p(a + a * a / (1 + c_sqrt(1 + a * a))); return x < 0 ? -r : r; }
static double CDECL c_acosh(double x) { if (x < 1) { set_errno(EDOM); return NAN_; } return x > 1e8 ? c_log(x) + 0.6931471805599453 : c_log(x + c_sqrt(x * x - 1)); }
static double CDECL c_atanh(double x) { if (x < -1 || x > 1) { set_errno(EDOM); return NAN_; } double a = c_fabs(x); double r = 0.5 * c_log1p(2 * a / (1 - a)); return x < 0 ? -r : r; }
static double CDECL c_cbrt(double x) {
    if (x == 0 || is_nan(x) || is_inf(x)) return x;
    double a = c_fabs(x);
    double r = (double)exp2l_(x87_yl2x(a, 1) / 3);
    r = r - (r * r * r - a) / (3 * r * r);                       // one Newton step polishes the last bit
    return x < 0 ? -r : r;
}
static double CDECL c_hypot(double x, double y) {
    if (is_inf(x) || is_inf(y)) return INF;
    ld a = x, b = y;
    return (double)x87_sqrt(a * a + b * b);
}

static double CDECL c_floor(double x) {
    if (c_fabs(x) >= 4503599627370496.0 || x == 0 || is_nan(x)) return x;
    double t = (double)(i64)x;
    return t > x ? t - 1 : t;
}
static double CDECL c_ceil(double x) {
    if (c_fabs(x) >= 4503599627370496.0 || x == 0 || is_nan(x)) return x;
    double t = (double)(i64)x;
    double r = t < x ? t + 1 : t;
    return r == 0 && x < 0 ? -0.0 : r;
}
static double CDECL c_trunc(double x) { if (c_fabs(x) >= 4503599627370496.0 || is_nan(x)) return x; double t = (double)(i64)x; return t == 0 && x < 0 ? -0.0 : t; }
static double CDECL c_round(double x) {
    if (c_fabs(x) >= 4503599627370496.0 || is_nan(x)) return x;
    double t = c_trunc(x);
    if (c_fabs(x - t) >= 0.5) t += x < 0 ? -1 : 1;
    return t;
}
static double CDECL c_rint(double x) { if (c_fabs(x) >= 4503599627370496.0 || is_nan(x)) return x; return (double)x87_rnd(x); }
static double CDECL c_nearbyint(double x) { return c_rint(x); }
static i32 CDECL c_lround(double x) { return (i32)c_round(x); }
static i64 CDECL c_llround(double x) { return (i64)c_round(x); }
static i32 CDECL c_lrint(double x) { return (i32)c_rint(x); }
static i64 CDECL c_llrint(double x) { return (i64)c_rint(x); }
static double CDECL c_fmod(double x, double y) { if (y == 0 || is_inf(x)) { set_errno(EDOM); return NAN_; } if (is_inf(y)) return x; return (double)x87_prem(x, y, false); }
static double CDECL c_remainder(double x, double y) { if (y == 0 || is_inf(x)) { set_errno(EDOM); return NAN_; } return (double)x87_prem(x, y, true); }
static double CDECL c_remquo(double x, double y, int* q) {
    double r = c_remainder(x, y);
    double n = c_round((x - r) / y);
    *q = (int)((i64)c_fabs(n) & 7) * (n < 0 ? -1 : 1);
    return r;
}
static double CDECL c_modf(double x, double* ip) {
    if (is_inf(x)) { *ip = x; return from_bits(bits(x) & 0x8000000000000000ull); }
    double t = c_trunc(x);
    *ip = t;
    return from_bits((bits(x - t) & 0x7FFFFFFFFFFFFFFFull) | (bits(x) & 0x8000000000000000ull));
}
static double CDECL c_frexp(double x, int* e) {
    *e = 0;
    if (x == 0 || is_nan(x) || is_inf(x)) return x;
    u64 b = bits(x);
    int ex = (int)((b >> 52) & 0x7FF);
    if (!ex) { x *= 18014398509481984.0; b = bits(x); ex = (int)((b >> 52) & 0x7FF) - 54; }
    *e = ex - 1022;
    return from_bits((b & 0x800FFFFFFFFFFFFFull) | (1022ull << 52));
}
static double CDECL c_ldexp(double x, int e) {
    if (x == 0 || is_nan(x) || is_inf(x)) return x;
    double r = (double)x87_scale(x, e);
    if (is_inf(r) || r == 0) set_errno(ERANGE);
    return r;
}
static double CDECL c_scalbn(double x, int e) { return c_ldexp(x, e); }
static double CDECL c_scalbln(double x, i32 e) { return c_ldexp(x, e); }
static int CDECL c_ilogb(double x) {
    if (x == 0) return (int)0x80000000;
    if (is_nan(x)) return (int)0x80000000;
    if (is_inf(x)) return 0x7FFFFFFF;
    int e;
    c_frexp(x, &e);
    return e - 1;
}
static double CDECL c_logb(double x) { if (x == 0) { set_errno(ERANGE); return -INF; } if (is_inf(x)) return INF; if (is_nan(x)) return x; return (double)c_ilogb(x); }
static double CDECL c_copysign(double x, double y) { return from_bits((bits(x) & 0x7FFFFFFFFFFFFFFFull) | (bits(y) & 0x8000000000000000ull)); }
static double CDECL c__chgsign(double x) { return from_bits(bits(x) ^ 0x8000000000000000ull); }
static double CDECL c_nextafter(double x, double y) {
    if (is_nan(x) || is_nan(y)) return NAN_;
    if (x == y) return y;
    if (x == 0) return from_bits(1 | (bits(y) & 0x8000000000000000ull));
    u64 b = bits(x);
    if ((x < y) == (x > 0)) b++; else b--;
    return from_bits(b);
}
static double CDECL c_fmin(double a, double b) { if (is_nan(a)) return b; if (is_nan(b)) return a; return a < b ? a : b; }
static double CDECL c_fmax(double a, double b) { if (is_nan(a)) return b; if (is_nan(b)) return a; return a > b ? a : b; }
static double CDECL c_fdim(double a, double b) { return a > b ? a - b : 0; }
static double CDECL c_fma(double a, double b, double c) { return (double)((ld)a * b + c); }
static double CDECL c_nan(const char* s) { (void)s; return NAN_; }

// error function and gamma
static double CDECL c_erfc(double x) {
    // W. J. Cody's rational approximations, good to about 1e-16
    double a = c_fabs(x);
    double r;
    if (a < 0.5) {
        static const double p[] = {3.20937758913846947e03, 3.77485237685302021e02, 1.13864154151050156e02, 3.16112374387056560e00, 1.85777706184603153e-1};
        static const double q[] = {2.84423683343917062e03, 1.28261652607737228e03, 2.44024637934444173e02, 2.36012909523441209e01, 1.0};
        double z = x * x;
        double n = (((p[4] * z + p[3]) * z + p[2]) * z + p[1]) * z + p[0];
        double d = (((q[4] * z + q[3]) * z + q[2]) * z + q[1]) * z + q[0];
        return 1 - x * n / d;
    } else if (a < 4) {
        static const double p[] = {1.23033935479799725e03, 2.05107837782607147e03, 1.71204761263407058e03, 8.81952221241769090e02, 2.98635138197400131e02,
                                   6.61191906371416295e01, 8.88314979438837594e00, 5.64188496988670089e-1, 2.15311535474403846e-8};
        static const double q[] = {1.23033935480374942e03, 3.43936767414372164e03, 4.36261909014324716e03, 3.29079923573345963e03, 1.62138957456669019e03,
                                   5.37181101862009858e02, 1.17693950891312499e02, 1.57449261107098347e01, 1.0};
        double n = p[8], d = q[8];
        for (int i = 7; i >= 0; i--) { n = n * a + p[i]; d = d * a + q[i]; }
        r = c_exp(-a * a) * n / d;
    } else {
        static const double p[] = {-6.58749161529837803e-4, -1.60837851487422766e-2, -1.25781726111229246e-1, -3.60344899949804439e-1, -3.05326634961232344e-1, -1.63153871373020978e-2};
        static const double q[] = {2.33520497626869185e-3, 6.05183413124413191e-2, 5.27905102951428412e-1, 1.87295284992346725e00, 2.56852019228982242e00, 1.0};
        double z = 1 / (a * a);
        double n = p[5], d = q[5];
        for (int i = 4; i >= 0; i--) { n = n * z + p[i]; d = d * z + q[i]; }
        r = c_exp(-a * a) / a * (0.564189583547756287 + z * n / d);
    }
    return x < 0 ? 2 - r : r;
}
static double CDECL c_erf(double x) { if (c_fabs(x) < 0.5) return 1 - c_erfc(x); return x < 0 ? c_erfc(-x) - 1 : 1 - c_erfc(x); }
static double lanczos_lgamma(double x) {
    static const double g = 7, c[9] = {0.99999999999980993, 676.5203681218851, -1259.1392167224028, 771.32342877765313, -176.61502916214059,
                                       12.507343278686905, -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7};
    x -= 1;
    double a = c[0];
    double t = x + g + 0.5;
    for (int i = 1; i < 9; i++) a += c[i] / (x + i);
    return 0.9189385332046727 + (x + 0.5) * c_log(t) - t + c_log(a);
}
static int signgam_v = 1;
static double CDECL c_lgamma(double x) {
    signgam_v = 1;
    if (is_inf(x)) return INF;
    if (x <= 0 && x == c_floor(x)) { set_errno(ERANGE); return INF; }
    if (x < 0.5) {
        double s = c_sin(3.141592653589793 * x);
        if (s < 0) signgam_v = -1;
        return 1.1447298858494002 - c_log(c_fabs(s)) - lanczos_lgamma(1 - x);
    }
    return lanczos_lgamma(x);
}
static double CDECL c_tgamma(double x) {
    if (x == 0) { set_errno(ERANGE); return sign_of(x) ? -INF : INF; }
    if (x < 0 && x == c_floor(x)) { set_errno(EDOM); return NAN_; }
    if (x == c_floor(x) && x <= 171) { double r = 1; for (int i = 2; i < (int)x; i++) r *= i; return r; }
    double l = c_lgamma(x);
    return signgam_v * c_exp(l);
}

// float and long double versions (long double is double on Windows)
#define F1(n) static float CDECL c_##n##f(float x) { return (float)c_##n(x); }
#define F2(n) static float CDECL c_##n##f(float x, float y) { return (float)c_##n(x, y); }
F1(sqrt) F1(fabs) F1(sin) F1(cos) F1(tan) F1(atan) F1(asin) F1(acos) F1(exp) F1(exp2) F1(expm1) F1(log) F1(log2) F1(log10) F1(log1p)
F1(sinh) F1(cosh) F1(tanh) F1(asinh) F1(acosh) F1(atanh) F1(cbrt) F1(floor) F1(ceil) F1(trunc) F1(round) F1(rint) F1(nearbyint) F1(logb)
F1(erf) F1(erfc) F1(lgamma) F1(tgamma)
F2(atan2) F2(pow) F2(hypot) F2(fmod) F2(remainder) F2(copysign) F2(nextafter) F2(fmin) F2(fmax) F2(fdim)
static float CDECL c_fmaf(float a, float b, float c) { return (float)((double)a * b + c); }
static float CDECL c_modff(float x, float* ip) { double i; float r = (float)c_modf(x, &i); *ip = (float)i; return r; }
static float CDECL c_frexpf(float x, int* e) { return (float)c_frexp(x, e); }
static float CDECL c_ldexpf(float x, int e) { return (float)c_ldexp(x, e); }
static float CDECL c_scalbnf(float x, int e) { return (float)c_ldexp(x, e); }
static int CDECL c_ilogbf(float x) { return c_ilogb(x); }
static i32 CDECL c_lroundf(float x) { return c_lround(x); }
static i64 CDECL c_llroundf(float x) { return c_llround(x); }
static i32 CDECL c_lrintf(float x) { return c_lrint(x); }
static i64 CDECL c_llrintf(float x) { return c_llrint(x); }
static float CDECL c_remquof(float x, float y, int* q) { return (float)c_remquo(x, y, q); }
static float CDECL c_nanf(const char* s) { (void)s; return __builtin_nanf(""); }
static float CDECL c_nexttowardf(float x, double y) { return (float)c_nextafter(x, y); }
static double CDECL c_nexttoward(double x, double y) { return c_nextafter(x, y); }
static float CDECL c__chgsignf(float x) { return -x; }

// Microsoft's classification helpers
static int CDECL c__finite(double x) { return !is_nan(x) && !is_inf(x); }
static int CDECL c__isnan(double x) { return is_nan(x); }
static int CDECL c__finitef(float x) { return c__finite(x); }
static int CDECL c__isnanf(float x) { return is_nan(x); }
static int CDECL c__fpclass(double x) {
    u64 b = bits(x);
    bool neg = b >> 63;
    u64 e = (b >> 52) & 0x7FF, m = b & 0xFFFFFFFFFFFFFull;
    if (e == 0x7FF) return m ? ((m >> 51) ? 2 : 1) : (neg ? 4 : 0x200);     // QNAN, SNAN, NINF, PINF
    if (!e) return m ? (neg ? 0x10 : 0x80) : (neg ? 0x20 : 0x40);            // ND, PD, NZ, PZ
    return neg ? 8 : 0x100;                                                // NN, PN
}
static short CDECL c__dclass(double x) { u64 e = (bits(x) >> 52) & 0x7FF, m = bits(x) & 0xFFFFFFFFFFFFFull; return e == 0x7FF ? (m ? 2 : 1) : !e ? (m ? -2 : 0) : -1; }
static short CDECL c__fdclass(float x) { return c__dclass(x); }
static short CDECL c__ldclass(double x) { return c__dclass(x); }
static int CDECL c__dsign(double x) { return sign_of(x) ? 0x8000 : 0; }
static int CDECL c__fdsign(float x) { return sign_of(x) ? 0x8000 : 0; }
static int CDECL c__ldsign(double x) { return c__dsign(x); }
static short CDECL c__dtest(double* x) { return c__dclass(*x); }
static short CDECL c__fdtest(float* x) { return c__dclass(*x); }
static int CDECL c__dpcomp(double a, double b) { return is_nan(a) || is_nan(b) ? 0 : a < b ? 1 : a == b ? 2 : 4; }
static int CDECL c__fdpcomp(float a, float b) { return c__dpcomp(a, b); }
static double CDECL c__hypot(double x, double y) { return c_hypot(x, y); }
static float CDECL c__hypotf(float x, float y) { return (float)c_hypot(x, y); }
static double CDECL c__copysign(double x, double y) { return c_copysign(x, y); }
static double CDECL c__logb(double x) { return c_logb(x); }
static double CDECL c__nextafter(double x, double y) { return c_nextafter(x, y); }
static double CDECL c__scalb(double x, i32 e) { return c_ldexp(x, e); }
static double CDECL c__cabs(double re, double im) { return c_hypot(re, im); }
static int* CDECL c___signgam() { return &signgam_v; }

static const Export math_exports[] = {
    X(sqrt), X(fabs), X(sin), X(cos), X(tan), X(atan2), X(atan), X(asin), X(acos), X(exp), X(exp2), X(expm1), X(log), X(log2), X(log10),
    X(log1p), X(pow), X(sinh), X(cosh), X(tanh), X(asinh), X(acosh), X(atanh), X(cbrt), X(hypot), X(floor), X(ceil), X(trunc), X(round),
    X(rint), X(nearbyint), X(lround), X(llround), X(lrint), X(llrint), X(fmod), X(remainder), X(remquo), X(modf), X(frexp), X(ldexp),
    X(scalbn), X(scalbln), X(ilogb), X(logb), X(copysign), X(nextafter), X(nexttoward), X(fmin), X(fmax), X(fdim), X(fma), X(nan), X(erf),
    X(erfc), X(lgamma), X(tgamma),
    X(sqrtf), X(fabsf), X(sinf), X(cosf), X(tanf), X(atan2f), X(atanf), X(asinf), X(acosf), X(expf), X(exp2f), X(expm1f), X(logf), X(log2f),
    X(log10f), X(log1pf), X(powf), X(sinhf), X(coshf), X(tanhf), X(asinhf), X(acoshf), X(atanhf), X(cbrtf), X(hypotf), X(floorf), X(ceilf),
    X(truncf), X(roundf), X(rintf), X(nearbyintf), X(lroundf), X(llroundf), X(lrintf), X(llrintf), X(fmodf), X(remainderf), X(remquof),
    X(modff), X(frexpf), X(ldexpf), X(scalbnf), X(ilogbf), X(logbf), X(copysignf), X(nextafterf), X(nexttowardf), X(fminf), X(fmaxf),
    X(fdimf), X(fmaf), X(nanf), X(erff), X(erfcf), X(lgammaf), X(tgammaf),
    EA("sqrtl", c_sqrt), EA("fabsl", c_fabs), EA("sinl", c_sin), EA("cosl", c_cos), EA("tanl", c_tan), EA("atan2l", c_atan2), EA("atanl", c_atan),
    EA("expl", c_exp), EA("logl", c_log), EA("log10l", c_log10), EA("powl", c_pow), EA("floorl", c_floor), EA("ceill", c_ceil), EA("fmodl", c_fmod),
    EA("frexpl", c_frexp), EA("ldexpl", c_ldexp), EA("modfl", c_modf), EA("copysignl", c_copysign), EA("truncl", c_trunc), EA("roundl", c_round),
    EA("hypotl", c_hypot), EA("cbrtl", c_cbrt), EA("log2l", c_log2), EA("exp2l", c_exp2), EA("nanl", c_nan),
    X(_finite), X(_isnan), X(_finitef), X(_isnanf), X(_fpclass), X(_dclass), X(_fdclass), X(_ldclass), X(_dsign), X(_fdsign), X(_ldsign),
    X(_dtest), X(_fdtest), X(_dpcomp), X(_fdpcomp), X(_hypot), X(_hypotf), X(_copysign), X(_chgsign), X(_chgsignf), X(_logb), X(_nextafter),
    X(_scalb), X(_cabs), X(__signgam),
};
EXPORT_TABLE("ucrtbase.dll", math_exports);

}  // namespace rt
