// silicon - PDF: functions and colour spaces
#include "pdf_gfx.h"

struct RenderCache {
    struct CsEnt { Obj* o; ColorSpace* cs; };
    Vec<CsEnt> cs;
};
static RenderCache* rcache(PdfDoc* d) {
    if (!d->rc) d->rc = new RenderCache();
    return d->rc;
}
void render_cache_free(PdfDoc* d) {
    if (!d->rc) return;
    for (int i = 0; i < d->rc->cs.len; i++) delete d->rc->cs[i].cs;
    d->rc->cs.free();
    delete d->rc;
    d->rc = nullptr;
}

// ================================================================ functions
enum {
    F_PUSH, F_ABS, F_ADD, F_ATAN, F_CEIL, F_COS, F_CVI, F_CVR, F_DIV, F_EXP, F_FLOOR, F_IDIV, F_LN, F_LOG, F_MOD, F_MUL, F_NEG,
    F_ROUND, F_SIN, F_SQRT, F_SUB, F_TRUNC, F_AND, F_BITSHIFT, F_EQ, F_FALSE, F_GE, F_GT, F_LE, F_LT, F_NE, F_NOT, F_OR, F_TRUE,
    F_XOR, F_COPY, F_DUP, F_EXCH, F_INDEX, F_POP, F_ROLL, F_IF, F_IFELSE
};
static const char* f_names[] = {"", "abs", "add", "atan", "ceiling", "cos", "cvi", "cvr", "div", "exp", "floor", "idiv", "ln", "log", "mod", "mul", "neg",
                                "round", "sin", "sqrt", "sub", "truncate", "and", "bitshift", "eq", "false", "ge", "gt", "le", "lt", "ne", "not", "or", "true",
                                "xor", "copy", "dup", "exch", "index", "pop", "roll", "if", "ifelse"};

PdfFunc::~PdfFunc() {
    if (samples) kfree(samples);
    for (int i = 0; i < subs.len; i++) delete subs[i];
    subs.free();
    prog.free();
    consts.free();
}

static bool ps_parse_block(const u8* d, u32 n, u32& p, Vec<u32>& code, Vec<double>& consts, int depth) {
    Vec<Vec<u32>> pending;
    bool ok = true;
    while (p < n) {
        u8 c = d[p];
        if (c <= 32) { p++; continue; }
        if (c == '%') { while (p < n && d[p] != '\n' && d[p] != '\r') p++; continue; }
        if (c == '{') {
            p++;
            if (depth > 16) { ok = false; break; }
            Vec<u32> sub;
            if (!ps_parse_block(d, n, p, sub, consts, depth + 1)) { sub.free(); ok = false; break; }
            pending.push(sub);
            continue;
        }
        if (c == '}') { p++; break; }
        u32 st = p;
        while (p < n && d[p] > 32 && d[p] != '{' && d[p] != '}' && d[p] != '%') p++;
        char tok[24];
        u32 l = min(p - st, 23u);
        memcpy(tok, d + st, l);
        tok[l] = 0;
        if ((tok[0] >= '0' && tok[0] <= '9') || tok[0] == '-' || tok[0] == '.' || tok[0] == '+') {
            double v = 0, f = 0.1;
            bool neg = false, frac = false;
            for (int i = 0; tok[i]; i++) {
                if (tok[i] == '-') neg = true;
                else if (tok[i] == '.') frac = true;
                else if (tok[i] >= '0' && tok[i] <= '9') { if (frac) { v += (tok[i] - '0') * f; f *= 0.1; } else v = v * 10 + (tok[i] - '0'); }
            }
            consts.push(neg ? -v : v);
            code.push(F_PUSH);
            code.push((u32)(consts.len - 1));
            continue;
        }
        if (!strcmp(tok, "if")) {
            if (pending.len < 1) { ok = false; break; }
            Vec<u32> b = pending[pending.len - 1];
            pending.len--;
            code.push(F_IF); code.push((u32)b.len);
            for (int i = 0; i < b.len; i++) code.push(b[i]);
            b.free();
            continue;
        }
        if (!strcmp(tok, "ifelse")) {
            if (pending.len < 2) { ok = false; break; }
            Vec<u32> b2 = pending[pending.len - 1], b1 = pending[pending.len - 2];
            pending.len -= 2;
            code.push(F_IFELSE); code.push((u32)b1.len); code.push((u32)b2.len);
            for (int i = 0; i < b1.len; i++) code.push(b1[i]);
            for (int i = 0; i < b2.len; i++) code.push(b2[i]);
            b1.free(); b2.free();
            continue;
        }
        int op = -1;
        for (int i = 1; i < ARRAY_LEN(f_names); i++) if (!strcmp(tok, f_names[i])) { op = i; break; }
        if (op < 0) { ok = false; break; }
        code.push((u32)op);
    }
    for (int i = 0; i < pending.len; i++) pending[i].free();
    pending.free();
    return ok;
}

static bool ps_run(const u32* code, int n, double* st, int& sp, const double* consts) {
    int pc = 0;
    while (pc < n) {
        u32 op = code[pc++];
        switch (op) {
        case F_PUSH: if (sp >= 99) return false; st[sp++] = consts[code[pc++]]; break;
        case F_IF: {
            u32 len = code[pc++];
            if (sp < 1) return false;
            bool c = st[--sp] != 0;
            if (c) { if (!ps_run(code + pc, (int)len, st, sp, consts)) return false; }
            pc += len;
            break;
        }
        case F_IFELSE: {
            u32 l1 = code[pc++], l2 = code[pc++];
            if (sp < 1) return false;
            bool c = st[--sp] != 0;
            if (c) { if (!ps_run(code + pc, (int)l1, st, sp, consts)) return false; }
            else if (!ps_run(code + pc + l1, (int)l2, st, sp, consts)) return false;
            pc += l1 + l2;
            break;
        }
        default: {
            auto need = [&](int k) { return sp >= k; };
#define BIN(expr) { if (!need(2)) return false; double b = st[--sp], a = st[sp - 1]; (void)a; (void)b; st[sp - 1] = (expr); }
#define UN(expr) { if (!need(1)) return false; double a = st[sp - 1]; st[sp - 1] = (expr); }
            switch (op) {
            case F_ABS: UN(m_fabs(a)); break;
            case F_ADD: BIN(a + b); break;
            case F_SUB: BIN(a - b); break;
            case F_MUL: BIN(a * b); break;
            case F_DIV: BIN(b != 0 ? a / b : 0); break;
            case F_IDIV: BIN(b != 0 ? (double)((long long)a / (long long)b) : 0); break;
            case F_MOD: BIN(b != 0 ? (double)((long long)a % (long long)b) : 0); break;
            case F_ATAN: { if (!need(2)) return false; double den = st[--sp], num = st[sp - 1]; double r = m_atan2(num, den) * 180 / 3.141592653589793; if (r < 0) r += 360; st[sp - 1] = r; break; }
            case F_CEIL: UN(m_ceil(a)); break;
            case F_FLOOR: UN(m_floor(a)); break;
            case F_COS: UN(m_cos(a * 3.141592653589793 / 180)); break;
            case F_SIN: UN(m_sin(a * 3.141592653589793 / 180)); break;
            case F_CVI: case F_TRUNC: UN((double)(long long)a); break;
            case F_CVR: break;
            case F_EXP: BIN(m_pow(a, b)); break;
            case F_LN: UN(m_log(a)); break;
            case F_LOG: UN(m_log(a) / 2.302585092994046); break;
            case F_NEG: UN(-a); break;
            case F_ROUND: UN(m_floor(a + 0.5)); break;
            case F_SQRT: UN(m_sqrt(a)); break;
            case F_AND: BIN((double)((long long)a & (long long)b)); break;
            case F_OR: BIN((double)((long long)a | (long long)b)); break;
            case F_XOR: BIN((double)((long long)a ^ (long long)b)); break;
            case F_NOT: UN(a == 0 ? 1 : (a == 1 ? 0 : (double)(~(long long)a))); break;
            case F_BITSHIFT: BIN(b >= 0 ? (double)((long long)a << (int)b) : (double)((long long)a >> (int)-b)); break;
            case F_EQ: BIN(a == b); break;
            case F_NE: BIN(a != b); break;
            case F_GE: BIN(a >= b); break;
            case F_GT: BIN(a > b); break;
            case F_LE: BIN(a <= b); break;
            case F_LT: BIN(a < b); break;
            case F_TRUE: if (sp >= 99) return false; st[sp++] = 1; break;
            case F_FALSE: if (sp >= 99) return false; st[sp++] = 0; break;
            case F_DUP: if (!need(1) || sp >= 99) return false; st[sp] = st[sp - 1]; sp++; break;
            case F_POP: if (!need(1)) return false; sp--; break;
            case F_EXCH: { if (!need(2)) return false; double t = st[sp - 1]; st[sp - 1] = st[sp - 2]; st[sp - 2] = t; break; }
            case F_COPY: {
                if (!need(1)) return false;
                int k = (int)st[--sp];
                if (k < 0 || k > sp || sp + k > 99) return false;
                for (int i = 0; i < k; i++) st[sp + i] = st[sp - k + i];
                sp += k;
                break;
            }
            case F_INDEX: {
                if (!need(1)) return false;
                int k = (int)st[--sp];
                if (k < 0 || k >= sp) return false;
                st[sp] = st[sp - 1 - k];
                sp++;
                break;
            }
            case F_ROLL: {
                if (!need(2)) return false;
                int j = (int)st[--sp], k = (int)st[--sp];
                if (k < 0 || k > sp) return false;
                if (k == 0) break;
                j %= k; if (j < 0) j += k;
                double tmp[100];
                for (int i = 0; i < k; i++) tmp[(i + j) % k] = st[sp - k + i];
                for (int i = 0; i < k; i++) st[sp - k + i] = tmp[i];
                break;
            }
            default: return false;
            }
#undef BIN
#undef UN
        }
        }
    }
    return true;
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

void PdfFunc::eval(const double* in, double* out) const {
    if (type == 100) {
        for (int i = 0; i < subs.len; i++) subs[i]->eval(in, out + i);
        return;
    }
    double x[8];
    for (int i = 0; i < m && i < 8; i++) x[i] = clampd(in[i], domain[2 * i], domain[2 * i + 1]);
    if (type == 2) {
        double t = N == 1 ? x[0] : m_pow(x[0], N);
        for (int j = 0; j < n; j++) out[j] = c0[j] + t * (c1[j] - c0[j]);
    } else if (type == 3) {
        int k = 0;
        while (k < nsub - 1 && x[0] >= bounds[k]) k++;
        double lo = k == 0 ? domain[0] : bounds[k - 1];
        double hi = k == nsub - 1 ? domain[1] : bounds[k];
        double e0 = enc3[2 * k], e1 = enc3[2 * k + 1];
        double v = hi == lo ? e0 : e0 + (x[0] - lo) * (e1 - e0) / (hi - lo);
        subs[k]->eval(&v, out);
    } else if (type == 0) {
        // multilinear interpolation of the sample table
        double e[8];
        int i0[8];
        double fr[8];
        for (int i = 0; i < m; i++) {
            double lo = domain[2 * i], hi = domain[2 * i + 1];
            double v = hi == lo ? encode[2 * i] : encode[2 * i] + (x[i] - lo) * (encode[2 * i + 1] - encode[2 * i]) / (hi - lo);
            v = clampd(v, 0, size[i] - 1);
            e[i] = v;
            i0[i] = (int)v;
            if (i0[i] >= size[i] - 1 && size[i] > 1) i0[i] = size[i] - 2;
            fr[i] = size[i] > 1 ? v - i0[i] : 0;
        }
        double maxv = bps >= 32 ? 4294967295.0 : (double)((1u << bps) - 1);
        auto sample = [&](const int* idx, int j) -> double {
            u64 pos = 0, mul = 1;
            for (int i = 0; i < m; i++) { pos += (u64)idx[i] * mul; mul *= size[i]; }
            u64 bit = (pos * n + j) * bps;
            u64 byte = bit >> 3;
            if (byte + (bps + 7) / 8 > nsamples) return 0;
            u32 v = 0;
            if (bps == 8) v = samples[byte];
            else if (bps == 16) v = (samples[byte] << 8) | samples[byte + 1];
            else if (bps == 32) v = ((u32)samples[byte] << 24) | (samples[byte + 1] << 16) | (samples[byte + 2] << 8) | samples[byte + 3];
            else if (bps == 24) v = (samples[byte] << 16) | (samples[byte + 1] << 8) | samples[byte + 2];
            else { int sh = 8 - bps - (int)(bit & 7); v = (samples[byte] >> sh) & ((1u << bps) - 1); }
            return v / maxv;
        };
        for (int j = 0; j < n; j++) {
            double acc = 0;
            int corners = 1 << m;
            for (int c = 0; c < corners; c++) {
                int idx[8];
                double w = 1;
                for (int i = 0; i < m; i++) {
                    bool hi = (c >> i) & 1;
                    idx[i] = i0[i] + (hi && size[i] > 1 ? 1 : 0);
                    w *= hi ? fr[i] : 1 - fr[i];
                }
                if (w > 0) acc += w * sample(idx, j);
            }
            out[j] = decode[2 * j] + acc * (decode[2 * j + 1] - decode[2 * j]);
        }
    } else if (type == 4) {
        double st[100];
        int sp = 0;
        for (int i = 0; i < m; i++) st[sp++] = x[i];
        if (!ps_run(prog.data, prog.len, st, sp, consts.data)) { for (int j = 0; j < n; j++) out[j] = 0; return; }
        for (int j = 0; j < n; j++) {
            int idx = sp - n + j;
            out[j] = idx >= 0 ? st[idx] : 0;
        }
    }
    if (has_range && type != 100)
        for (int j = 0; j < n; j++) out[j] = clampd(out[j], range[2 * j], range[2 * j + 1]);
}

PdfFunc* fn_parse(PdfDoc* d, Obj* o, int depth) {
    o = pdf_res(d, o);
    if (!o || depth > 8) return nullptr;
    if (o->type == O_ARR) {
        PdfFunc* f = new PdfFunc();
        f->type = 100;
        f->m = 1;
        for (u32 i = 0; i < o->arr.n; i++) {
            PdfFunc* s = fn_parse(d, aget(d, o, i), depth + 1);
            if (!s) { delete f; return nullptr; }
            f->subs.push(s);
            f->m = s->m;
        }
        f->n = f->subs.len;
        return f;
    }
    if (!is_dict(o)) return nullptr;
    int type = (int)onum(dget(d, o, "FunctionType"), -1);
    PdfFunc* f = new PdfFunc();
    f->type = type;
    Obj* dom = dget(d, o, "Domain");
    f->m = max(1, (int)alen(dom) / 2);
    if (f->m > 8) { delete f; return nullptr; }
    for (int i = 0; i < f->m * 2; i++) f->domain[i] = onum(aget(d, dom, i), i & 1);
    Obj* rng = dget(d, o, "Range");
    if (rng && rng->type == O_ARR) {
        f->has_range = true;
        f->n = (int)rng->arr.n / 2;
        if (f->n > 8) f->n = 8;
        for (int i = 0; i < f->n * 2; i++) f->range[i] = onum(aget(d, rng, i), i & 1);
    }
    if (type == 2) {
        Obj* c0 = dget(d, o, "C0");
        Obj* c1 = dget(d, o, "C1");
        int n = max(1, (int)max(alen(c0), alen(c1)));
        if (!c0 && !c1) n = 1;
        if (n > 8) n = 8;
        f->n = n;
        for (int i = 0; i < n; i++) { f->c0[i] = c0 ? onum(aget(d, c0, i), 0) : 0; f->c1[i] = c1 ? onum(aget(d, c1, i), 1) : 1; }
        f->N = onum(dget(d, o, "N"), 1);
    } else if (type == 3) {
        Obj* fns = dget(d, o, "Functions");
        Obj* bnd = dget(d, o, "Bounds");
        Obj* enc = dget(d, o, "Encode");
        int k = (int)alen(fns);
        if (k < 1 || k > 60) { delete f; return nullptr; }
        for (int i = 0; i < k; i++) {
            PdfFunc* s = fn_parse(d, aget(d, fns, i), depth + 1);
            if (!s) { delete f; return nullptr; }
            f->subs.push(s);
        }
        f->nsub = k;
        f->n = f->subs[0]->n;
        for (int i = 0; i < k - 1; i++) f->bounds[i] = onum(aget(d, bnd, i), 0);
        for (int i = 0; i < k * 2; i++) f->enc3[i] = onum(aget(d, enc, i), i & 1);
    } else if (type == 0) {
        if (o->type != O_STREAM) { delete f; return nullptr; }
        Obj* sz = dget(d, o, "Size");
        for (int i = 0; i < f->m; i++) f->size[i] = max(1, (int)onum(aget(d, sz, i), 1));
        f->bps = (int)onum(dget(d, o, "BitsPerSample"), 8);
        if (f->bps < 1 || f->bps > 32) { delete f; return nullptr; }
        Obj* enc = dget(d, o, "Encode");
        for (int i = 0; i < f->m; i++) {
            f->encode[2 * i] = enc ? onum(aget(d, enc, 2 * i), 0) : 0;
            f->encode[2 * i + 1] = enc ? onum(aget(d, enc, 2 * i + 1), f->size[i] - 1) : f->size[i] - 1;
        }
        Obj* dec = dget(d, o, "Decode");
        for (int i = 0; i < f->n; i++) {
            f->decode[2 * i] = dec ? onum(aget(d, dec, 2 * i), f->range[2 * i]) : f->range[2 * i];
            f->decode[2 * i + 1] = dec ? onum(aget(d, dec, 2 * i + 1), f->range[2 * i + 1]) : f->range[2 * i + 1];
        }
        f->samples = pdf_stream_copy(d, o, &f->nsamples);
    } else if (type == 4) {
        if (o->type != O_STREAM) { delete f; return nullptr; }
        u32 len;
        u8* code = pdf_stream_copy(d, o, &len);
        u32 p = 0;
        while (p < len && code[p] != '{') p++;
        p++;
        bool ok = ps_parse_block(code, len, p, f->prog, f->consts, 0);
        kfree(code);
        if (!ok) { delete f; return nullptr; }
    } else { delete f; return nullptr; }
    return f;
}

// ================================================================ colour
u32 rgb_from_cmyk(double c, double m, double y, double k) {
    c = clampd(c, 0, 1); m = clampd(m, 0, 1); y = clampd(y, 0, 1); k = clampd(k, 0, 1);
    double r = 255 + c * (-4.387332384609988 * c + 54.48615194189176 * m + 18.82290502165302 * y + 212.25662451639585 * k - 285.2331026137004) +
               m * (1.7149763477362134 * m - 5.6096736904047315 * y - 17.873870861415444 * k - 5.497006427196366) +
               y * (-2.5217340131683033 * y - 21.248923337353073 * k + 17.5119270841813) + k * (-21.86122147463605 * k - 189.48180835922747);
    double g = 255 + c * (8.841041422036149 * c + 60.118027045597366 * m + 6.871425592049007 * y + 31.159100130055922 * k - 79.2970844816548) +
               m * (-15.310361306967817 * m + 17.575251261109482 * y + 131.35250912493976 * k - 190.9453302588951) +
               y * (4.444339102852739 * y + 9.8632861493405 * k - 24.86741582555878) + k * (-20.737325471181034 * k - 187.80453709719578);
    double b = 255 + c * (0.8842522430003296 * c + 8.078677503112928 * m + 30.89978309703729 * y - 0.23883238689178934 * k - 14.183576799673286) +
               m * (10.49593273432072 * m + 63.02378494754052 * y + 50.606957656360734 * k - 112.23884253719248) +
               y * (0.03296041114873217 * y + 115.60384449646641 * k - 193.58209356861505) + k * (-22.33816807309886 * k - 180.12613974708367);
    return rgb((u32)clampd(r + 0.5, 0, 255), (u32)clampd(g + 0.5, 0, 255), (u32)clampd(b + 0.5, 0, 255));
}

ColorSpace::~ColorSpace() {
    if (lut) kfree(lut);
    if (fn) delete fn;
    if (owned_base && base) delete base;
}

ColorSpace* cs_device(int n) {
    static ColorSpace g, c, k;
    static bool init = false;
    if (!init) {
        init = true;
        g.kind = CS_GRAY; g.n = 1;
        c.kind = CS_RGB; c.n = 3;
        k.kind = CS_CMYK; k.n = 4;
    }
    return n == 1 ? &g : n == 4 ? &k : &c;
}

static u32 gray_rgb(double g) { u32 v = (u32)(clampd(g, 0, 1) * 255 + 0.5); return rgb(v, v, v); }
static u32 rgb3(double r, double g, double b) { return rgb((u32)(clampd(r, 0, 1) * 255 + 0.5), (u32)(clampd(g, 0, 1) * 255 + 0.5), (u32)(clampd(b, 0, 1) * 255 + 0.5)); }

static double lab_g(double x) { return x >= 6.0 / 29 ? x * x * x : 108.0 / 841 * (x - 4.0 / 29); }
static double srgb_enc(double v) {
    v = clampd(v, 0, 1);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * m_pow(v, 1 / 2.4) - 0.055;
}

u32 cs_rgb(const ColorSpace* cs, const double* c) {
    switch (cs->kind) {
    case CS_GRAY: case CS_CAL_GRAY: return gray_rgb(c[0]);
    case CS_RGB: case CS_CAL_RGB: return rgb3(c[0], c[1], c[2]);
    case CS_CMYK: return rgb_from_cmyk(c[0], c[1], c[2], c[3]);
    case CS_LAB: {
        double L = clampd(c[0], 0, 100), a = clampd(c[1], cs->rng[0], cs->rng[1]), b = clampd(c[2], cs->rng[2], cs->rng[3]);
        double M = (L + 16) / 116, Ll = M + a / 500, N = M - b / 200;
        double X = cs->white[0] * lab_g(Ll), Y = cs->white[1] * lab_g(M), Z = cs->white[2] * lab_g(N);
        // XYZ (D65-ish) to linear sRGB
        double r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
        double g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
        double bb = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
        return rgb3(srgb_enc(r), srgb_enc(g), srgb_enc(bb));
    }
    case CS_INDEXED: {
        int i = (int)clampd(c[0] + 0.5, 0, cs->hival);
        double comps[8];
        int bn = cs->base->n;
        for (int k = 0; k < bn && k < 8; k++) comps[k] = cs->lut[i * bn + k] / 255.0;
        if (cs->base->kind == CS_LAB) { comps[0] *= 100; comps[1] = cs->base->rng[0] + comps[1] * (cs->base->rng[1] - cs->base->rng[0]); comps[2] = cs->base->rng[2] + comps[2] * (cs->base->rng[3] - cs->base->rng[2]); }
        return cs_rgb(cs->base, comps);
    }
    case CS_SEP: {
        if (!cs->fn) return gray_rgb(1 - c[0]);
        double out[16];
        for (int i = 0; i < 16; i++) out[i] = 0;
        cs->fn->eval(c, out);
        return cs_rgb(cs->base, out);
    }
    default: return 0xFF000000;
    }
}

void cs_initial(const ColorSpace* cs, double* c) {
    for (int i = 0; i < 8; i++) c[i] = 0;
    if (cs->kind == CS_CMYK) c[3] = 1;
    else if (cs->kind == CS_SEP) for (int i = 0; i < cs->n; i++) c[i] = 1;
    else if (cs->kind == CS_LAB) { c[1] = clampd(0, cs->rng[0], cs->rng[1]); c[2] = clampd(0, cs->rng[2], cs->rng[3]); }
}

static ColorSpace* cs_from_name(const char* n) {
    if (!strcmp(n, "DeviceGray") || !strcmp(n, "G") || !strcmp(n, "CalGray")) return cs_device(1);
    if (!strcmp(n, "DeviceRGB") || !strcmp(n, "RGB") || !strcmp(n, "CalRGB")) return cs_device(3);
    if (!strcmp(n, "DeviceCMYK") || !strcmp(n, "CMYK")) return cs_device(4);
    return nullptr;
}

ColorSpace* cs_parse(PdfDoc* d, Obj* o, Obj* res, int depth) {
    o = pdf_res(d, o);
    if (!o || depth > 6) return cs_device(1);
    if (o->type == O_NAME) {
        const char* nm = (const char*)o->str.s;
        ColorSpace* c = cs_from_name(nm);
        if (c) return c;
        if (!strcmp(nm, "Pattern")) {
            RenderCache* rc = rcache(d);
            for (int i = 0; i < rc->cs.len; i++) if (rc->cs[i].o == nullptr && rc->cs[i].cs->kind == CS_PATTERN && !rc->cs[i].cs->base) return rc->cs[i].cs;
            ColorSpace* p = new ColorSpace();
            p->kind = CS_PATTERN; p->n = 0;
            rc->cs.push({nullptr, p});
            return p;
        }
        Obj* tab = dget(d, res, "ColorSpace");
        Obj* e = dget(d, tab, nm);
        if (e && !(e->type == O_NAME && !strcmp((const char*)e->str.s, nm))) return cs_parse(d, e, res, depth + 1);
        return cs_device(1);
    }
    if (o->type != O_ARR || !o->arr.n) return cs_device(1);
    RenderCache* rc = rcache(d);
    for (int i = 0; i < rc->cs.len; i++) if (rc->cs[i].o == o) return rc->cs[i].cs;
    Obj* f = aget(d, o, 0);
    const char* kind = oname(f);
    ColorSpace* cs = nullptr;
    if (!strcmp(kind, "ICCBased")) {
        Obj* s = aget(d, o, 1);
        int n = (int)onum(dget(d, s, "N"), 3);
        ColorSpace* alt = nullptr;
        Obj* a = dget(d, s, "Alternate");
        if (a && n != 3 && n != 1 && n != 4) alt = cs_parse(d, a, res, depth + 1);
        return alt ? alt : cs_device(n == 1 ? 1 : n == 4 ? 4 : 3);
    }
    if (!strcmp(kind, "CalRGB") || !strcmp(kind, "CalGray") || !strcmp(kind, "DeviceRGB") || !strcmp(kind, "DeviceGray") || !strcmp(kind, "DeviceCMYK") ||
        !strcmp(kind, "G") || !strcmp(kind, "RGB") || !strcmp(kind, "CMYK")) {
        ColorSpace* c = cs_from_name(kind);
        return c ? c : cs_device(1);
    }
    cs = new ColorSpace();
    if (!strcmp(kind, "Lab")) {
        cs->kind = CS_LAB; cs->n = 3;
        Obj* p = aget(d, o, 1);
        Obj* wp = dget(d, p, "WhitePoint");
        for (int i = 0; i < 3; i++) cs->white[i] = onum(aget(d, wp, i), cs->white[i]);
        Obj* r = dget(d, p, "Range");
        for (int i = 0; i < 4; i++) cs->rng[i] = onum(aget(d, r, i), i & 1 ? 100 : -100);
    } else if (!strcmp(kind, "Indexed") || !strcmp(kind, "I")) {
        cs->kind = CS_INDEXED; cs->n = 1;
        ColorSpace* b = cs_parse(d, aget(d, o, 1), res, depth + 1);
        cs->base = b;
        cs->hival = clamp((int)onum(aget(d, o, 2), 0), 0, 255);
        Obj* lk = aget(d, o, 3);
        size_t need = (size_t)(cs->hival + 1) * b->n;
        cs->lut = (u8*)kcalloc(need + 4);
        if (lk && lk->type == O_STR) memcpy(cs->lut, lk->str.s, min((size_t)lk->str.len, need));
        else if (lk && lk->type == O_STREAM) {
            u32 l;
            u8* dd = pdf_stream_copy(d, lk, &l);
            memcpy(cs->lut, dd, min((size_t)l, need));
            kfree(dd);
        }
    } else if (!strcmp(kind, "Separation") || !strcmp(kind, "DeviceN")) {
        cs->kind = CS_SEP;
        Obj* names = aget(d, o, 1);
        cs->n = !strcmp(kind, "Separation") ? 1 : max(1, (int)alen(names));
        if (cs->n > 8) cs->n = 8;
        if (names && names->type == O_NAME && !strcmp((const char*)names->str.s, "None")) cs->none = true;
        cs->base = cs_parse(d, aget(d, o, 2), res, depth + 1);
        cs->fn = fn_parse(d, aget(d, o, 3));
    } else if (!strcmp(kind, "Pattern")) {
        cs->kind = CS_PATTERN; cs->n = 0;
        if (o->arr.n > 1) cs->base = cs_parse(d, aget(d, o, 1), res, depth + 1);
    } else {
        delete cs;
        return cs_device(1);
    }
    rc->cs.push({o, cs});
    return cs;
}
