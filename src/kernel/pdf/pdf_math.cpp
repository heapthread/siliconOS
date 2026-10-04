// silicon - PDF: small math library (the kernel has no libm)
#include "pdf_int.h"

static const double PI = 3.14159265358979323846;

double m_floor(double x) {
    if (x != x || x > 4e15 || x < -4e15) return x;
    long long i = (long long)x;
    double f = (double)i;
    return f > x ? f - 1 : f;
}
double m_fabs(double x) { return x < 0 ? -x : x; }

double m_sqrt(double x) {
    if (x <= 0) return 0;
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

// sin / cos by reducing to [-pi/4, pi/4] and a Taylor series
static double sin_core(double x) {
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156))))));
}
static double cos_core(double x) {
    double x2 = x * x;
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132 * (1 - x2 / 182))))));
}
static void reduce(double x, int* q, double* r) {
    double k = m_floor(x / (PI / 2) + 0.5);
    *r = x - k * (PI / 2);
    long long kk = (long long)k;
    *q = (int)(kk & 3);
}
double m_sin(double x) {
    int q; double r;
    reduce(x, &q, &r);
    switch (q) { case 0: return sin_core(r); case 1: return cos_core(r); case 2: return -sin_core(r); default: return -cos_core(r); }
}
double m_cos(double x) {
    int q; double r;
    reduce(x, &q, &r);
    switch (q) { case 0: return cos_core(r); case 1: return -sin_core(r); case 2: return -cos_core(r); default: return sin_core(r); }
}

static double atan_core(double x) {    // |x| <= 1
    // reduce further: atan(x) = 2 atan(x / (1 + sqrt(1 + x^2)))
    x = x / (1 + m_sqrt(1 + x * x));
    x = x / (1 + m_sqrt(1 + x * x));
    double x2 = x * x;
    double s = x * (1 - x2 / 3 * (1 - x2 * 3 / 5 * (1 - x2 * 5 / 7 * (1 - x2 * 7 / 9 * (1 - x2 * 9 / 11)))));
    return 4 * s;
}
double m_atan2(double y, double x) {
    if (x == 0 && y == 0) return 0;
    double ax = m_fabs(x), ay = m_fabs(y);
    double a;
    if (ay <= ax) a = atan_core(ay / ax);
    else a = PI / 2 - atan_core(ax / ay);
    if (x < 0) a = PI - a;
    return y < 0 ? -a : a;
}

double m_exp(double x) {
    if (x > 700) x = 700;
    if (x < -700) return 0;
    double k = m_floor(x / 0.6931471805599453 + 0.5);
    double r = x - k * 0.6931471805599453;
    double t = 1, s = 1;
    for (int i = 1; i < 18; i++) { t *= r / i; s += t; }
    long long kk = (long long)k;
    union { u64 u; double d; } b;
    b.u = (u64)(kk + 1023) << 52;
    return s * b.d;
}
double m_log(double x) {
    if (x <= 0) return -700;
    union { u64 u; double d; } b;
    b.d = x;
    int e = (int)((b.u >> 52) & 0x7FF) - 1023;
    b.u = (b.u & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;
    double m = b.d;                       // [1, 2)
    if (m > 1.4142135623730951) { m *= 0.5; e++; }
    double t = (m - 1) / (m + 1), t2 = t * t, s = 0, p = t;
    for (int i = 1; i < 30; i += 2) { s += p / i; p *= t2; }
    return 2 * s + e * 0.6931471805599453;
}
double m_pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 0) return 0;
    if (x < 0) {
        double yi = m_floor(y);
        if (yi != y) return 0;
        double r = m_exp(y * m_log(-x));
        return ((long long)yi & 1) ? -r : r;
    }
    return m_exp(y * m_log(x));
}

Mat mat_inv(const Mat& m, bool* ok) {
    Mat r;
    double det = m.a * m.d - m.b * m.c;
    if (det > -1e-14 && det < 1e-14) { if (ok) *ok = false; return r; }
    if (ok) *ok = true;
    double id = 1.0 / det;
    r.a = m.d * id;  r.b = -m.b * id;
    r.c = -m.c * id; r.d = m.a * id;
    r.e = -(m.e * r.a + m.f * r.c);
    r.f = -(m.e * r.b + m.f * r.d);
    return r;
}

void Path::append(const Path& o, const Mat& m) {
    for (int i = 0; i < o.s.len; i++) {
        PSeg g = o.s[i];
        int np = g.op == P_CURVE ? 3 : g.op == P_CLOSE ? 0 : 1;
        for (int k = 0; k < np; k++) {
            double ox, oy;
            mat_apply(m, g.x[2 * k], g.x[2 * k + 1], &ox, &oy);
            g.x[2 * k] = (float)ox;
            g.x[2 * k + 1] = (float)oy;
        }
        s.push(g);
    }
}
