/* fmt_float.h - float formatting and parsing shared by the runtime (rt.c) and the compiler's
 * comptime interpreter (src/interp.c), so text made at compile time matches run time exactly. */
#ifndef FL_FMT_FLOAT_H
#define FL_FMT_FLOAT_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Format a float the way JavaScript does: the shortest form that reads back
 * exactly. Writes at most 31 bytes to buf, returns the length. */
static uint64_t fl_fmt_f64(double v, char *buf) {
    if (isnan(v)) { memcpy(buf, "NaN", 3); return 3; }
    if (isinf(v)) {
        const char *s = v > 0 ? "Infinity" : "-Infinity";
        size_t n = strlen(s);
        memcpy(buf, s, n);
        return (int64_t)n;
    }
    if (v == (double)(int64_t)v && v > -1e15 && v < 1e15 && !(v == 0 && signbit(v))) {
        /* whole numbers: plain integer digits (same output as %.15g, much faster) */
        char t[24];
        int n = 0;
        int64_t x = (int64_t)v;
        uint64_t m = x < 0 ? (uint64_t)0 - (uint64_t)x : (uint64_t)x;
        do { t[n++] = (char)('0' + m % 10); m /= 10; } while (m);
        int k = 0;
        if (x < 0) buf[k++] = '-';
        while (n) buf[k++] = t[--n];
        return k;
    }
    char b[40];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(b, sizeof b, "%.*g", prec, v);
        if (strtod(b, 0) == v) break;
    }
    size_t n = strlen(b);
    memcpy(buf, b, n);
    return (int64_t)n;
}

/* Same for 32-bit floats: shortest text that reads back as the same f32. */
static uint64_t fl_fmt_f32(float v, char *buf) {
    if (isnan(v) || isinf(v)) return fl_fmt_f64(v, buf);
    char b[40];
    for (int prec = 6; prec <= 9; prec++) {
        snprintf(b, sizeof b, "%.*g", prec, (double)v);
        if (strtof(b, 0) == v) break;
    }
    size_t n = strlen(b);
    memcpy(buf, b, n);
    return (int64_t)n;
}

/* fixed-point (`{d:.2}`) or, with exp, scientific (`{e}`, `{e:.3}`; prec < 0: the shortest that reads back) */
static uint64_t fl_fmt_f64_prec(double v, int64_t prec, uint8_t exp, char *buf, uint64_t cap) {
    if (isnan(v) || isinf(v)) return fl_fmt_f64(v, buf);
    if (prec > 100) prec = 100;
    int n;
    if (exp && prec < 0) {
        char b[64];
        for (int p = 0; p <= 17; p++) {
            snprintf(b, sizeof b, "%.*e", p, v);
            if (strtod(b, 0) == v) break;
        }
        n = snprintf(buf, (size_t)cap, "%s", b);
    } else {
        n = snprintf(buf, (size_t)cap, exp ? "%.*e" : "%.*f", (int)prec, v);
    }
    return n < 0 ? 0 : (uint64_t)n >= cap ? cap - 1 : (uint64_t)n;
}

/* *ok is set to whether the whole text was a number */
static double fl_parse_f64(const uint8_t *p, uint64_t n, uint8_t *ok) {
    char small[64];
    char *s = n < sizeof small ? small : malloc((size_t)n + 1);
    if (!s) { *ok = 0; return 0; }
    memcpy(s, p, (size_t)n);
    s[n] = 0;
    char *end;
    double v = strtod(s, &end);
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    *ok = !(n == 0 || end == s || *end);
    if (s != small) free(s);
    return v;
}

#endif
