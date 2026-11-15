/* interp.c - runs IR at compile time (comptime code).
 *
 * The interpreter executes the same unoptimized IR that the backend compiles, with the
 * backend's semantics (see arm64.c), so comptime code behaves exactly like runtime code:
 * overflow checks, bounds checks and panics are the same instructions, and a panic becomes
 * a compile error.
 *
 * Memory is a set of regions: one per call frame (its stack slots), one per string literal,
 * global and function. A pointer is (region << 32) | offset, so every load and store is
 * checked against the region it points into: out-of-bounds accesses, uses of a frame after
 * its function returned, writes to runtime globals and calls to C are errors, not UB.
 *
 * It's deterministic (no I/O, no clock, no host addresses leak out) and stops after
 * ct_step_limit instructions. */
#include "flint.h"
#include "fmt_float.h"
#include <math.h>
#include <setjmp.h>

int64_t ct_step_limit = 10000000;

typedef union {
    int64_t i;
    double d;
} RV;

typedef struct {
    int kind;
    const char *sym;
    uint8_t *data;
    int64_t size;
    bool dead;     /* a frame whose function returned */
    bool readonly; /* string literals, `let` globals */
    bool noread;   /* a `var` global: its value at run time isn't known now */
    const char *name;
    IRFunc *fn;    /* CR_FUNC */
    const char *why; /* CR_EXTERN: why it can't be called */
} Region;

static Module *MOD;
static CtLookup LOOKUP;
static Region *regs;
static int nregs, cregs;
static int first_frame; /* regions from here on belong to the current run */
static int64_t steps;
static CtError *ERR;
static jmp_buf *FAIL;
static int depth;

/* the call chain, for error notes */
typedef struct {
    IRFunc *f;
    Token *call; /* the call instruction in the caller */
} Frame;
static Frame stack[1024]; /* calls nest at most 1000 deep */

/* ---------- symbols -> regions (cached across runs: strings, globals and functions don't change) ---------- */

typedef struct {
    const char *sym;
    int region;
} SymEnt;
static SymEnt *symtab;
static int symcap, symn;

static uint64_t hash_str(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

static int new_region(int kind, int64_t size) {
    Region r;
    memset(&r, 0, sizeof r);
    r.kind = kind;
    r.size = size;
    r.data = xalloc((size_t)(size > 0 ? size : 1));
    if (nregs == 0) { /* region 0 is null */
        Region z;
        memset(&z, 0, sizeof z);
        PUSH(regs, nregs, cregs, z);
    }
    PUSH(regs, nregs, cregs, r);
    return nregs - 1;
}

static uint64_t mkptr(int region, int64_t off) { return ((uint64_t)region << 32) + (uint64_t)off; }

static _Noreturn void fail(Token *tok, const char *fmt, ...);

static int sym_lookup(const char *sym) {
    if (symcap) {
        uint64_t h = hash_str(sym) & (uint64_t)(symcap - 1);
        while (symtab[h].sym) {
            if (!strcmp(symtab[h].sym, sym)) return symtab[h].region;
            h = (h + 1) & (uint64_t)(symcap - 1);
        }
    }
    return 0;
}

static void sym_insert(const char *sym, int region) {
    if (2 * (symn + 1) > symcap) {
        int oc = symcap;
        SymEnt *old = symtab;
        symcap = symcap ? symcap * 2 : 256;
        symtab = xalloc(sizeof(SymEnt) * (size_t)symcap);
        symn = 0;
        for (int i = 0; i < oc; i++)
            if (old[i].sym) sym_insert(old[i].sym, old[i].region);
        free(old);
    }
    uint64_t h = hash_str(sym) & (uint64_t)(symcap - 1);
    while (symtab[h].sym) h = (h + 1) & (uint64_t)(symcap - 1);
    symtab[h].sym = sym;
    symtab[h].region = region;
    symn++;
}

static int region_for_sym(const char *sym, bool got, Token *tok);

/* fill a global's region from its initial data */
static void init_global(int r, int gi, Token *tok) { /* (indexes: looking up symbols may move both arrays) */
    GlobalVar *g = &MOD->globals[gi];
    switch (g->kind) {
    case GV_ZERO: break;
    case GV_FILL: memset(regs[r].data, 0xAA, (size_t)g->size); break;
    case GV_INT: memcpy(regs[r].data, &g->ival, (size_t)(g->size < 8 ? g->size : 8)); break;
    case GV_STR: {
        int64_t len = MOD->strs[g->sidx].len;
        uint64_t p = mkptr(region_for_sym(MOD->strs[g->sidx].label, false, tok), 0);
        memcpy(regs[r].data, &p, 8);
        memcpy(regs[r].data + 8, &len, 8);
        break;
    }
    case GV_DATA:
        for (int k = 0; k < MOD->globals[gi].nitems; k++) {
            DataItem d = MOD->globals[gi].items[k];
            int64_t v = d.val;
            if (d.sym) v = (int64_t)mkptr(region_for_sym(d.sym, false, tok), d.val);
            memcpy(regs[r].data + d.off, &v, (size_t)d.size);
        }
        break;
    }
}

static int region_for_sym(const char *sym, bool got, Token *tok) {
    int r = sym_lookup(sym);
    if (r) return r;
    if (!strncmp(sym, "l_fl_str_", 9) && !got) {
        int k = atoi(sym + 9);
        StrLit *s = &MOD->strs[k];
        r = new_region(CR_STR, s->len + 1); /* with its NUL, like the emitted data */
        memcpy(regs[r].data, s->data, (size_t)s->len);
        regs[r].readonly = true;
        regs[r].sym = sym;
        regs[r].name = "a string literal";
        sym_insert(sym, r);
        return r;
    }
    if (!got)
        for (int i = 0; i < MOD->nglobals; i++) {
            GlobalVar *g = &MOD->globals[i];
            if (strcmp(g->sym, sym)) continue;
            r = new_region(CR_GLOBAL, g->size);
            regs[r].sym = g->sym;
            regs[r].name = g->name ? g->name : g->sym;
            regs[r].readonly = true; /* comptime code never changes a runtime global */
            regs[r].noread = !g->is_const;
            sym_insert(sym, r); /* before its data: it may point at itself */
            init_global(r, i, tok);
            return r;
        }
    const char *why = NULL;
    IRFunc *f = got ? NULL : LOOKUP(sym, &why);
    r = new_region(f ? CR_FUNC : CR_EXTERN, 0);
    regs[r].sym = sym;
    regs[r].fn = f;
    regs[r].why = why;
    regs[r].name = f && f->name ? f->name : sym[0] == '_' ? sym + 1 : sym;
    if (f || got) sym_insert(sym, r); /* a function still being compiled may be ready next time */
    return r;
}

/* ---------- errors ---------- */

static Token *cur_tok;

static _Noreturn void fail(Token *tok, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ERR->msg = strf("%s", buf);
    ERR->tok = tok ? tok : cur_tok;
    ERR->ncalls = 0;
    for (int i = depth - 1; i >= 1 && ERR->ncalls < 16; i--) {
        ERR->calls[ERR->ncalls] = stack[i].call;
        ERR->fns[ERR->ncalls] = stack[i].f->name ? stack[i].f->name : stack[i].f->sym;
        ERR->ncalls++;
    }
    longjmp(*FAIL, 1);
}

/* ---------- memory ---------- */

static Region *mem_at(uint64_t p, int64_t n, bool write, int64_t *off) {
    uint64_t ri = p >> 32;
    int64_t o = (int64_t)(p & 0xffffffffu);
    if (p == 0) fail(NULL, "comptime code used a null pointer");
    if (ri == 0 || ri >= (uint64_t)nregs) fail(NULL, "comptime code used a pointer that doesn't point at anything (0x%llx)", (unsigned long long)p);
    Region *r = &regs[ri];
    if (r->dead) fail(NULL, "comptime code used a pointer to a local variable of a function that already returned");
    if (r->kind == CR_FUNC || r->kind == CR_EXTERN)
        fail(NULL, "comptime code read or wrote the memory of '%s', which is a function", r->name);
    if (o + n > r->size) fail(NULL, "comptime code accessed memory out of bounds (%lld bytes at offset %lld of %s, which has %lld)",
                              (long long)n, (long long)o, r->name ? r->name : "a value", (long long)r->size);
    if (write && r->readonly) {
        if (r->kind == CR_GLOBAL) fail(NULL, "comptime code can't change the runtime global '%s'", r->name);
        fail(NULL, "comptime code wrote to read-only memory (%s)", r->name);
    }
    if (!write && r->noread)
        fail(NULL, "comptime code can't read '%s': it's a 'var', so its value is only known at run time", r->name);
    *off = o;
    return r;
}

static void mem_read(uint64_t p, void *out, int64_t n) {
    if (n <= 0) return;
    int64_t o;
    Region *r = mem_at(p, n, false, &o);
    memcpy(out, r->data + o, (size_t)n);
}

static void mem_write(uint64_t p, const void *in, int64_t n) {
    if (n <= 0) return;
    int64_t o;
    Region *r = mem_at(p, n, true, &o);
    memcpy(r->data + o, in, (size_t)n);
}

static uint8_t *mem_span(uint64_t p, int64_t n, bool write) {
    if (n <= 0) return NULL;
    int64_t o;
    Region *r = mem_at(p, n, write, &o);
    return r->data + o;
}

static int64_t load_int(uint64_t p, int mem) {
    int size = mem & 0x7f;
    bool sg = mem & 0x80;
    uint64_t v = 0;
    mem_read(p, &v, size);
    if (size < 8) {
        int sh = 64 - 8 * size;
        if (sg) return (int64_t)(v << sh) >> sh;
        return (int64_t)v;
    }
    return (int64_t)v;
}

/* ---------- frames ---------- */

typedef struct {
    IRFunc *f;
    int64_t *off; /* slot offsets in the frame region */
    int64_t size;
} Layout;
static Layout *layouts;
static int nlayouts, clayouts;

static Layout *layout_of(IRFunc *f) {
    for (int i = nlayouts - 1; i >= 0; i--)
        if (layouts[i].f == f) return &layouts[i];
    Layout l;
    l.f = f;
    l.off = xalloc(sizeof(int64_t) * (size_t)(f->nslots + 1));
    int64_t cur = 0;
    for (int s = 0; s < f->nslots; s++) {
        int al = f->slots[s].align < 1 ? 1 : f->slots[s].align;
        cur = (cur + al - 1) / al * al;
        l.off[s] = cur;
        cur += f->slots[s].size;
    }
    l.size = cur;
    PUSH(layouts, nlayouts, clayouts, l);
    return &layouts[nlayouts - 1];
}

/* ---------- condition codes ---------- */

static bool cc_holds(int cc, bool N, bool Z, bool Cf, bool V) {
    switch (cc) {
    case CC_EQ: return Z;
    case CC_NE: return !Z;
    case CC_LT: return N != V;
    case CC_LE: return Z || N != V;
    case CC_GT: return !Z && N == V;
    case CC_GE: return N == V;
    case CC_LO: return !Cf;
    case CC_LS: return !Cf || Z;
    case CC_HI: return Cf && !Z;
    case CC_HS: return Cf;
    case CC_MI: return N;
    case CC_PL: return !N;
    }
    return false;
}

static bool icmp(int cc, int64_t a, int64_t b) {
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b, r = ua - ub;
    bool N = (int64_t)r < 0, Z = r == 0, Cf = ua >= ub;
    bool V = ((a ^ b) & (a ^ (int64_t)r)) < 0;
    return cc_holds(cc, N, Z, Cf, V);
}

static bool fcmp_cc(int cc, double a, double b) {
    bool N, Z, Cf, V;
    if (isnan(a) || isnan(b)) N = false, Z = false, Cf = true, V = true;
    else if (a < b) N = true, Z = false, Cf = false, V = false;
    else if (a == b) N = false, Z = true, Cf = true, V = false;
    else N = false, Z = false, Cf = true, V = false;
    return cc_holds(cc, N, Z, Cf, V);
}

static double round_cls(double d, int cls) { return cls == C_F32 ? (double)(float)d : d; }

static int64_t fcvt_s(double d) {
    if (isnan(d)) return 0;
    if (d >= 9223372036854775808.0) return INT64_MAX;
    if (d < -9223372036854775808.0) return INT64_MIN;
    return (int64_t)d;
}

static uint64_t fcvt_u(double d) {
    if (isnan(d) || d <= 0) return 0;
    if (d >= 18446744073709551616.0) return UINT64_MAX;
    return (uint64_t)d;
}

/* ---------- SIMD lanes ---------- */

static void vec_op(Inst *in, RV *v) {
    int es = VL_ESIZE(in->imm), total = VL_TOTAL(in->imm), n = total / es;
    bool fl = VL_FLOAT(in->imm);
    uint8_t a[16], b[16], out[16];
#define OPV(o) ((o).k == O_IMM ? (o).v : v[(o).v].i)
    if (in->mem == VOP_SPLAT) {
        for (int l = 0; l < n; l++) {
            if (fl && es == 4) {
                float f = (float)v[in->b.v].d;
                memcpy(out + 4 * l, &f, 4);
            } else if (fl) {
                memcpy(out + 8 * l, &v[in->b.v].d, 8);
            } else {
                int64_t x = OPV(in->b);
                memcpy(out + es * l, &x, (size_t)es);
            }
        }
        mem_write((uint64_t)OPV(in->a), out, total);
        return;
    }
    mem_read((uint64_t)OPV(in->b), a, total);
    if (in->mem != VOP_NEG) mem_read((uint64_t)OPV(in->c), b, total);
    for (int l = 0; l < n; l++) {
        if (fl) {
            double x, y = 0, r = 0;
            if (es == 4) {
                float fx, fy = 0;
                memcpy(&fx, a + 4 * l, 4);
                if (in->mem != VOP_NEG) memcpy(&fy, b + 4 * l, 4);
                x = fx, y = fy;
            } else {
                memcpy(&x, a + 8 * l, 8);
                if (in->mem != VOP_NEG) memcpy(&y, b + 8 * l, 8);
            }
            switch (in->mem) {
            case VOP_ADD: r = x + y; break;
            case VOP_SUB: r = x - y; break;
            case VOP_MUL: r = x * y; break;
            case VOP_DIV: r = x / y; break;
            case VOP_NEG: r = -x; break;
            }
            if (es == 4) {
                float fr = (float)r;
                memcpy(out + 4 * l, &fr, 4);
            } else {
                memcpy(out + 8 * l, &r, 8);
            }
        } else {
            uint64_t x = 0, y = 0, r = 0;
            memcpy(&x, a + es * l, (size_t)es);
            if (in->mem != VOP_NEG) memcpy(&y, b + es * l, (size_t)es);
            switch (in->mem) {
            case VOP_ADD: r = x + y; break;
            case VOP_SUB: r = x - y; break;
            case VOP_MUL: r = x * y; break;
            case VOP_AND: r = x & y; break;
            case VOP_OR: r = x | y; break;
            case VOP_XOR: r = x ^ y; break;
            case VOP_NEG: r = 0 - x; break;
            }
            memcpy(out + es * l, &r, (size_t)es);
        }
    }
    mem_write((uint64_t)OPV(in->a), out, total);
#undef OPV
}

/* the backend's order: pairwise (faddp) for floats */
static RV vec_reduce(Inst *in, RV *v) {
    int es = VL_ESIZE(in->imm), total = VL_TOTAL(in->imm), n = total / es;
    uint8_t a[16];
    mem_read((uint64_t)(in->a.k == O_IMM ? in->a.v : v[in->a.v].i), a, total);
    RV r;
    if (VL_FLOAT(in->imm)) {
        double x[4];
        for (int l = 0; l < n; l++) {
            if (es == 4) {
                float f;
                memcpy(&f, a + 4 * l, 4);
                x[l] = f;
            } else {
                memcpy(&x[l], a + 8 * l, 8);
            }
        }
        if (n == 4) {
            float p = (float)(x[0] + x[1]), q = (float)(x[2] + x[3]);
            r.d = (float)((double)p + (double)q);
        } else {
            r.d = es == 4 ? (float)(x[0] + x[1]) : x[0] + x[1];
        }
        return r;
    }
    uint64_t s = 0;
    for (int l = 0; l < n; l++) {
        uint64_t x = 0;
        memcpy(&x, a + es * l, (size_t)es);
        s += x;
    }
    if (es < 8) {
        int sh = 64 - 8 * es;
        s = VL_SIGNED(in->imm) ? (uint64_t)((int64_t)(s << sh) >> sh) : (s << sh) >> sh;
    }
    r.i = (int64_t)s;
    return r;
}

/* ---------- calls the interpreter does itself: the runtime's pure parts and libc ---------- */

static _Noreturn void panic_with(const char *msg, int64_t len) { fail(NULL, "panic at compile time: %.*s", (int)len, msg); }

static bool builtin_call(const char *sym, RV *a, int n, RV *ret) {
    const char *s = sym[0] == '_' ? sym + 1 : sym;
#define I(k) (a[k].i)
#define P(k) ((uint64_t)a[k].i)
    if (!strcmp(s, "rt_panic")) {
        int64_t len = I(1);
        char *buf = xalloc((size_t)(len > 0 ? len : 0) + 1);
        mem_read(P(0), buf, len);
        panic_with(buf, len);
    }
    if (!strcmp(s, "rt_oob")) fail(NULL, "panic at compile time: index %lld out of bounds (len %lld)", (long long)I(0), (long long)I(1));
    if (!strcmp(s, "rt_range")) {
        if (I(0) < 0 || I(1) < I(0) || I(1) > I(2))
            fail(NULL, "panic at compile time: slice [%lld..%lld] out of bounds (len %lld)", (long long)I(0), (long long)I(1), (long long)I(2));
        return true;
    }
    if (!strcmp(s, "rt_assert_fail")) fail(NULL, "assertion failed at compile time");
    if (!strcmp(s, "rt_exit")) fail(NULL, "comptime code can't exit the compiler (exit(%lld))", (long long)I(0));
    if (!strcmp(s, "rt_error_name")) {
        int64_t code = I(0);
        const char *nm = code >= 0 && code < MOD->nerrnames ? MOD->errnames[code] : "(unknown error)";
        const char *key = strf("l_fl_err_%lld", (long long)code);
        int r = sym_lookup(key);
        if (!r) {
            r = new_region(CR_CONST, (int64_t)strlen(nm) + 1);
            memcpy(regs[r].data, nm, strlen(nm));
            regs[r].readonly = true;
            regs[r].name = "an error name";
            sym_insert(key, r);
        }
        ret->i = (int64_t)mkptr(r, 0);
        return true;
    }
    if (!strcmp(s, "memcpy") || !strcmp(s, "memmove")) {
        int64_t len = I(2);
        if (len > 0) {
            uint8_t *tmp = xalloc((size_t)len);
            mem_read(P(1), tmp, len);
            mem_write(P(0), tmp, len);
            free(tmp);
        }
        ret->i = I(0);
        return true;
    }
    if (!strcmp(s, "memset") || !strcmp(s, "bzero")) {
        bool bz = s[0] == 'b';
        int64_t len = bz ? I(1) : I(2);
        if (len > 0) {
            uint8_t *p = mem_span(P(0), len, true);
            memset(p, bz ? 0 : (int)(I(1) & 0xff), (size_t)len);
        }
        ret->i = I(0);
        return true;
    }
    if (!strcmp(s, "memcmp")) {
        int64_t len = I(2);
        int c = 0;
        if (len > 0) {
            uint8_t *x = mem_span(P(0), len, false), *y = mem_span(P(1), len, false);
            c = memcmp(x, y, (size_t)len);
        }
        ret->i = c < 0 ? -1 : c > 0;
        return true;
    }
    if (!strcmp(s, "strlen")) {
        int64_t len = 0;
        for (;; len++) {
            uint8_t ch;
            mem_read(P(0) + (uint64_t)len, &ch, 1);
            if (!ch) break;
        }
        ret->i = len;
        return true;
    }
    if (!strcmp(s, "rt_fmt_f64") || !strcmp(s, "rt_fmt_f32")) {
        char buf[64];
        uint64_t len = s[8] == '6' ? fl_fmt_f64(a[0].d, buf) : fl_fmt_f32((float)a[0].d, buf);
        mem_write(P(1), buf, (int64_t)len);
        ret->i = (int64_t)len;
        return true;
    }
    if (!strcmp(s, "rt_fmt_f64_prec")) {
        uint64_t cap = (uint64_t)I(4);
        char *buf = xalloc((size_t)cap + 64);
        uint64_t len = fl_fmt_f64_prec(a[0].d, I(1), (uint8_t)I(2), buf, cap);
        mem_write(P(3), buf, (int64_t)(len + 1 <= cap ? len + 1 : len));
        free(buf);
        ret->i = (int64_t)len;
        return true;
    }
    if (!strcmp(s, "rt_parse_f64")) {
        int64_t len = I(1);
        uint8_t *buf = xalloc((size_t)(len > 0 ? len : 0) + 1);
        mem_read(P(0), buf, len);
        uint8_t ok = 0;
        ret->d = fl_parse_f64(buf, (uint64_t)len, &ok);
        mem_write(P(2), &ok, 1);
        free(buf);
        return true;
    }
    static const struct { const char *name; double (*f1)(double); double (*f2)(double, double); } math[] = {
        {"sin", sin, NULL}, {"cos", cos, NULL}, {"tan", tan, NULL}, {"exp", exp, NULL}, {"log", log, NULL},
        {"floor", floor, NULL}, {"ceil", ceil, NULL}, {"round", round, NULL}, {"pow", NULL, pow},
        {"atan2", NULL, atan2}, {"fmod", NULL, fmod},
    };
    for (size_t k = 0; k < sizeof math / sizeof math[0]; k++)
        if (!strcmp(s, math[k].name)) {
            ret->d = math[k].f1 ? math[k].f1(a[0].d) : math[k].f2(a[0].d, a[1].d);
            return true;
        }
    (void)n;
    return false;
#undef I
#undef P
}

static const char *IO_FNS[] = {"rt_write", "rt_flush", "rt_now", "rt_args", "write", "read", "open", "close", "printf",
                               "puts", "fopen", "fwrite", "fread", "mmap", "munmap", "malloc", "free", "posix_memalign",
                               "getpagesize", "rt_debug_init", "rt_debug_quarantine", "malloc_size", NULL};

/* ---------- the interpreter loop ---------- */

static RV exec(IRFunc *f, RV *args, int *argcls, int nargs, int nfixed);

static RV do_call(Inst *in, RV *v) {
    int nargs = in->sym ? in->nargs : in->nargs - 1;
    RV *av = xalloc(sizeof(RV) * (size_t)(nargs + 1));
    for (int j = 0; j < nargs; j++) av[j] = in->args[j].k == O_IMM ? (RV){.i = in->args[j].v} : v[in->args[j].v];
    RV ret = {0};
    Region *target;
    if (in->sym) {
        if (builtin_call(in->sym, av, nargs, &ret)) {
            free(av);
            return ret;
        }
        int ri = region_for_sym(in->sym, false, in->tok); /* (it may move regs) */
        target = &regs[ri];
    } else {
        Opd c = in->args[nargs];
        uint64_t p = (uint64_t)(c.k == O_IMM ? c.v : v[c.v].i);
        uint64_t ri = p >> 32;
        if (p == 0 || ri >= (uint64_t)nregs || (p & 0xffffffffu) || (regs[ri].kind != CR_FUNC && regs[ri].kind != CR_EXTERN))
            fail(NULL, "comptime code called something that isn't a function");
        target = &regs[ri];
        if (builtin_call(target->sym, av, nargs, &ret)) {
            free(av);
            return ret;
        }
    }
    { /* the runtime's I/O functions are Flint code now (std.rt), but still off limits */
        const char *nm = target->name;
        for (const char **io = IO_FNS; *io; io++)
            if (!strcmp(nm, *io)) fail(NULL, "comptime code can't call '%s': there's no I/O, memory from the system or C at compile time", nm);
    }
    if (target->kind != CR_FUNC || !target->fn) {
        const char *nm = target->name;
        if (target->why) fail(NULL, "comptime code can't call '%s': %s", nm, target->why);
        fail(NULL, "comptime code can't call '%s' (a C function): there's no I/O or C at compile time", nm);
    }
    if (depth >= 1000) fail(NULL, "comptime calls nested more than 1000 deep (infinite recursion?)");
    stack[depth].f = target->fn;
    stack[depth].call = in->tok;
    depth++;
    ret = exec(target->fn, av, in->argcls, nargs, in->nfixed);
    depth--;
    free(av);
    return ret;
}

static RV exec(IRFunc *f, RV *args, int *argcls, int nargs, int nfixed) {
    /* where each argument went: x0-x7, d0-d7, or the stack (the backend's rules) */
    RV ireg[8] = {{0}}, freg[8] = {{0}};
    RV *stk = xalloc(sizeof(RV) * (size_t)(nargs + 1));
    int ic = 0, fc = 0, ns = 0;
    for (int j = 0; j < nargs; j++) {
        bool fp = argcls[j] != C_I;
        bool on_stack = j >= nfixed || (fp ? fc : ic) >= 8;
        if (on_stack) {
            stk[ns++] = args[j];
            if (j < nfixed) (fp ? fc++ : ic++);
        } else if (fp) {
            freg[fc++] = args[j];
        } else {
            ireg[ic++] = args[j];
        }
    }
    Layout *lay = layout_of(f);
    int64_t *loff = lay->off; /* (the layouts array may move during calls) */
    int fr = new_region(CR_FRAME, lay->size);
    regs[fr].name = strf("a local variable of '%s'", f->name ? f->name : f->sym);
    RV *v = xalloc(sizeof(RV) * (size_t)(f->nv + 1));
    int b = 0;
    RV result = {0};
    for (;;) {
        Block *B = &f->blocks[b];
        int next = -1;
        for (int i = 0; i < B->n; i++) {
            Inst *in = &B->insts[i];
            if (in->tok) cur_tok = in->tok;
            if (++steps > ct_step_limit)
                fail(NULL, "comptime evaluation took more than %lld steps (an infinite loop?)", (long long)ct_step_limit);
#define O(o) ((o).k == O_IMM ? (o).v : v[(o).v].i)
#define OF(o) (v[(o).v].d)
#define U(o) ((uint64_t)O(o))
            switch (in->op) {
            case I_NOP: break;
            case I_GETARG:
                if (in->imm >= 1000) v[in->dst] = in->imm - 1000 < ns ? stk[in->imm - 1000] : (RV){0};
                else v[in->dst] = in->cls == C_I ? ireg[in->imm & 7] : freg[in->imm & 7];
                if (in->cls == C_F32) v[in->dst].d = (float)v[in->dst].d;
                break;
            case I_ICONST: v[in->dst].i = in->imm; break;
            case I_FCONST: v[in->dst].d = round_cls(in->fimm, in->cls); break;
            case I_MOV:
                if (in->cls == C_I) v[in->dst].i = O(in->a);
                else v[in->dst].d = OF(in->a);
                break;
            case I_ADD: v[in->dst].i = (int64_t)(U(in->a) + U(in->b)); break;
            case I_SUB: v[in->dst].i = (int64_t)(U(in->a) - U(in->b)); break;
            case I_MUL: v[in->dst].i = (int64_t)(U(in->a) * U(in->b)); break;
            case I_SDIV: {
                int64_t x = O(in->a), y = O(in->b);
                v[in->dst].i = y == 0 ? 0 : (x == INT64_MIN && y == -1) ? INT64_MIN : x / y;
                break;
            }
            case I_UDIV: v[in->dst].i = U(in->b) == 0 ? 0 : (int64_t)(U(in->a) / U(in->b)); break;
            case I_SREM: {
                int64_t x = O(in->a), y = O(in->b);
                v[in->dst].i = y == 0 ? x : (y == -1) ? 0 : x % y;
                break;
            }
            case I_UREM: v[in->dst].i = U(in->b) == 0 ? O(in->a) : (int64_t)(U(in->a) % U(in->b)); break;
            case I_AND: v[in->dst].i = O(in->a) & O(in->b); break;
            case I_OR: v[in->dst].i = O(in->a) | O(in->b); break;
            case I_XOR: v[in->dst].i = O(in->a) ^ O(in->b); break;
            case I_SHL: v[in->dst].i = (int64_t)(U(in->a) << (U(in->b) & 63)); break;
            case I_ASR: v[in->dst].i = O(in->a) >> (U(in->b) & 63); break;
            case I_LSR: v[in->dst].i = (int64_t)(U(in->a) >> (U(in->b) & 63)); break;
            case I_NEG: v[in->dst].i = (int64_t)(0 - U(in->a)); break;
            case I_NOT: v[in->dst].i = ~O(in->a); break;
            case I_FADD: v[in->dst].d = round_cls(OF(in->a) + OF(in->b), in->cls); break;
            case I_FSUB: v[in->dst].d = round_cls(OF(in->a) - OF(in->b), in->cls); break;
            case I_FMUL: v[in->dst].d = round_cls(OF(in->a) * OF(in->b), in->cls); break;
            case I_FDIV: v[in->dst].d = round_cls(OF(in->a) / OF(in->b), in->cls); break;
            case I_FNEG: v[in->dst].d = -OF(in->a); break;
            case I_FSQRT: v[in->dst].d = round_cls(sqrt(OF(in->a)), in->cls); break;
            case I_FABS: v[in->dst].d = fabs(OF(in->a)); break;
            case I_CMP: v[in->dst].i = icmp(in->mem, O(in->a), O(in->b)); break;
            case I_FCMP: v[in->dst].i = fcmp_cc(in->mem, OF(in->a), OF(in->b)); break;
            case I_SEL:
                if (in->cls == C_I) v[in->dst].i = O(in->a) ? O(in->b) : O(in->c);
                else v[in->dst].d = O(in->a) ? OF(in->b) : OF(in->c);
                break;
            case I_EXT: {
                int size = in->mem & 0x7f;
                uint64_t x = U(in->a);
                if (size < 8) {
                    int sh = 64 - 8 * size;
                    x = (in->mem & 0x80) ? (uint64_t)((int64_t)(x << sh) >> sh) : (x << sh) >> sh;
                }
                v[in->dst].i = (int64_t)x;
                break;
            }
            case I_SCVTF: v[in->dst].d = in->cls == C_F32 ? (double)(float)O(in->a) : (double)O(in->a); break;
            case I_UCVTF: v[in->dst].d = in->cls == C_F32 ? (double)(float)U(in->a) : (double)U(in->a); break;
            case I_FCVTZS: v[in->dst].i = fcvt_s(OF(in->a)); break;
            case I_FCVTZU: v[in->dst].i = (int64_t)fcvt_u(OF(in->a)); break;
            case I_FCVT: v[in->dst].d = round_cls(OF(in->a), in->cls); break;
            case I_LOAD:
            case I_STORE: {
                uint64_t addr = in->slot >= 0 ? mkptr(fr, loff[in->slot] + in->imm) : U(in->a) + (uint64_t)in->imm;
                int size = in->cls == C_F32 ? 4 : in->cls == C_F64 ? 8 : (in->mem & 0x7f);
                if (in->op == I_LOAD) {
                    if (in->cls == C_F32) {
                        float fv;
                        mem_read(addr, &fv, 4);
                        v[in->dst].d = fv;
                    } else if (in->cls == C_F64) {
                        mem_read(addr, &v[in->dst].d, 8);
                    } else {
                        v[in->dst].i = load_int(addr, in->mem);
                    }
                } else {
                    if (in->cls == C_F32) {
                        float fv = (float)OF(in->b);
                        mem_write(addr, &fv, 4);
                    } else if (in->cls == C_F64) {
                        mem_write(addr, &OF(in->b), 8);
                    } else {
                        int64_t x = O(in->b);
                        mem_write(addr, &x, size);
                    }
                }
                break;
            }
            case I_ADDR_SLOT: v[in->dst].i = (int64_t)mkptr(fr, loff[in->slot]); break;
            case I_ADDR_SYM: v[in->dst].i = (int64_t)mkptr(region_for_sym(in->sym, in->mem, in->tok), 0); break;
            case I_CALL: {
                RV r = do_call(in, v);
                if (in->dst >= 0) {
                    v[in->dst] = r;
                    if (in->cls == C_F32) v[in->dst].d = (float)r.d;
                }
                break;
            }
            case I_MEMCPY: {
                uint8_t *tmp = xalloc((size_t)in->imm + 1);
                mem_read(U(in->b), tmp, in->imm);
                mem_write(U(in->a), tmp, in->imm);
                free(tmp);
                break;
            }
            case I_MEMZERO: {
                uint8_t *p = mem_span(U(in->a), in->imm, true);
                if (p) memset(p, 0, (size_t)in->imm);
                break;
            }
            case I_SMULH: v[in->dst].i = (int64_t)(((__int128)O(in->a) * (__int128)O(in->b)) >> 64); break;
            case I_UMULH:
                v[in->dst].i = (int64_t)(((unsigned __int128)U(in->a) * (unsigned __int128)U(in->b)) >> 64);
                break;
            case I_VEC: vec_op(in, v); break;
            case I_VREDUCE: v[in->dst] = vec_reduce(in, v); break;
            case I_JMP: next = in->t1; break;
            case I_BR: next = O(in->a) ? in->t1 : in->t2; break;
            case I_RET:
                if (in->a.k != O_NONE) {
                    if (in->cls == C_I) result.i = O(in->a);
                    else result.d = OF(in->a);
                }
                regs[fr].dead = depth > 1; /* the outermost frame stays readable: the result may live there */
                if (regs[fr].dead) {
                    free(regs[fr].data);
                    regs[fr].data = NULL;
                }
                free(v);
                free(stk);
                return result;
            case I_TRAP: fail(NULL, "comptime code reached unreachable code");
            default: fail(NULL, "internal error: the comptime interpreter doesn't know IR op %d", in->op);
            }
#undef O
#undef OF
#undef U
            if (next >= 0) break;
        }
        if (next < 0) fail(NULL, "internal error: block b%d of %s has no terminator", b, f->sym);
        b = next;
    }
}

void ct_init(Module *m, CtLookup lookup) {
    MOD = m;
    LOOKUP = lookup;
}

bool ct_run(IRFunc *f, uint64_t *ret, CtError *err) {
    jmp_buf jb;
    jmp_buf *saved_fail = FAIL;
    CtError *saved_err = ERR;
    int saved_depth = depth;
    Token *saved_tok = cur_tok;
    FAIL = &jb;
    ERR = err;
    memset(err, 0, sizeof *err);
    steps = 0; /* the limit is per evaluation */
    first_frame = nregs ? nregs : 1;
    cur_tok = NULL;
    bool ok;
    if (setjmp(jb)) {
        ok = false;
    } else {
        stack[depth].f = f;
        stack[depth].call = NULL;
        depth++;
        int dummy = 0;
        RV r = exec(f, NULL, &dummy, 0, 0);
        depth--;
        memcpy(ret, &r, 8);
        ok = true;
    }
    depth = saved_depth;
    FAIL = saved_fail;
    ERR = saved_err;
    cur_tok = saved_tok;
    return ok;
}

bool ct_region(uint64_t ptr, CtRegion *out, int64_t *off) {
    uint64_t ri = ptr >> 32;
    memset(out, 0, sizeof *out);
    if (ptr == 0 || ri == 0 || ri >= (uint64_t)nregs || regs[ri].dead) return false;
    Region *r = &regs[ri];
    out->kind = r->kind;
    out->sym = r->sym;
    out->data = r->data;
    out->size = r->size;
    out->id = (int)ri;
    *off = (int64_t)(ptr & 0xffffffffu);
    return true;
}

/* frames of finished runs aren't needed once their results were copied out */
void ct_release(void) {
    for (int i = first_frame; i < nregs; i++)
        if (regs[i].kind == CR_FRAME && !regs[i].dead) {
            regs[i].dead = true;
            free(regs[i].data);
            regs[i].data = NULL;
        }
}
