/* arm64.c - IR -> ARM64 assembly (Apple / Mach-O flavor)
 *
 * Register allocation is linear scan over live intervals:
 *   - liveness is computed per block with bitsets, then each vreg gets one
 *     interval [first live position, last live position] in layout order
 *   - intervals that span a call only get callee-saved registers
 *     (x19-x28, d8-d15); others prefer caller-saved ones (x9-x15, d16-d28)
 *   - when registers run out, the interval ending furthest away is spilled
 * Scratch registers never allocated: x16, x17, x8 and d29-d31.
 * x0-x7 / d0-d7 are only used to pass arguments and results. */
#include "flint.h"

static FILE *O;
static IRFunc *F;
static int FN;
static int *order, norder;   /* blocks in layout order */
static int *layout_idx;      /* block -> index in order (-1 = not emitted) */
static int *vreg_reg;        /* vreg -> physical register, or -1 if spilled */
static int *vreg_off;        /* vreg -> spill offset below x29 */
static int *slot_off;        /* slot -> offset below x29 */
static int *usecnt;
static int saved_int[16], nsaved_int, saved_fp[16], nsaved_fp;
static int frame_size;

static const char *CCN[] = {"eq", "ne", "lt", "le", "gt", "ge", "lo", "ls", "hi", "hs", "mi", "pl"};
static const int CCINV[] = {CC_NE, CC_EQ, CC_GE, CC_GT, CC_LE, CC_LT, CC_HS, CC_HI, CC_LS, CC_LO, CC_PL, CC_MI};

static char XN[32][6], WN[32][6], DN[32][6], SN[32][6];

static void init_names(void) {
    for (int i = 0; i < 32; i++) {
        snprintf(XN[i], 6, "x%d", i);
        snprintf(WN[i], 6, "w%d", i);
        snprintf(DN[i], 6, "d%d", i);
        snprintf(SN[i], 6, "s%d", i);
    }
    strcpy(XN[31], "xzr");
    strcpy(WN[31], "wzr");
}

static void E(const char *fmt, ...) {
    va_list ap;
    fputc('\t', O);
    va_start(ap, fmt);
    vfprintf(O, fmt, ap);
    va_end(ap);
    fputc('\n', O);
}

static const char *FR(int r, int cls) { return cls == C_F32 ? SN[r] : DN[r]; }

/* ====================================================================== */
/* register allocation                                                    */
/* ====================================================================== */

static const int INT_CALLER[] = {9, 10, 11, 12, 13, 14, 15};
static const int INT_CALLEE[] = {19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
static const int FP_CALLER[] = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
static const int FP_CALLEE[] = {8, 9, 10, 11, 12, 13, 14, 15};
#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

typedef struct {
    int v, start, end;
    bool cross;
} Iv;

static int cmp_iv(const void *a, const void *b) {
    const Iv *x = a, *y = b;
    if (x->start != y->start) return x->start - y->start;
    return x->v - y->v;
}

static bool is_callee_saved(int r, int file) {
    return file == 0 ? (r >= 19 && r <= 28) : (r >= 8 && r <= 15);
}

static void mark_saved(int r, int file) {
    if (file == 0) {
        for (int i = 0; i < nsaved_int; i++)
            if (saved_int[i] == r) return;
        saved_int[nsaved_int++] = r;
    } else {
        for (int i = 0; i < nsaved_fp; i++)
            if (saved_fp[i] == r) return;
        saved_fp[nsaved_fp++] = r;
    }
}

static void regalloc(void) {
    int nv = F->nv, nb = F->nblocks;
    int W = (nv + 63) / 64;
    if (W == 0) W = 1;
    int *bstart = xalloc(sizeof(int) * (size_t)nb), *bend = xalloc(sizeof(int) * (size_t)nb);
    int pos = 0;
    for (int k = 0; k < norder; k++) {
        int b = order[k];
        bstart[b] = pos;
        pos += F->blocks[b].n;
        bend[b] = pos - 1;
    }
    uint64_t *use = xalloc(sizeof(uint64_t) * (size_t)(nb * W)), *def = xalloc(sizeof(uint64_t) * (size_t)(nb * W));
    uint64_t *in = xalloc(sizeof(uint64_t) * (size_t)(nb * W)), *out = xalloc(sizeof(uint64_t) * (size_t)(nb * W));
#define BIT(set, b, v) ((set)[(size_t)(b) * W + (v) / 64] & ((uint64_t)1 << ((v) % 64)))
#define SET(set, b, v) ((set)[(size_t)(b) * W + (v) / 64] |= ((uint64_t)1 << ((v) % 64)))
    int u[64];
    for (int k = 0; k < norder; k++) {
        int b = order[k];
        Block *B = &F->blocks[b];
        for (int i = 0; i < B->n; i++) {
            int n = inst_uses(&B->insts[i], u);
            for (int j = 0; j < n; j++)
                if (!BIT(def, b, u[j])) SET(use, b, u[j]);
            if (B->insts[i].dst >= 0) SET(def, b, B->insts[i].dst);
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (int k = norder - 1; k >= 0; k--) {
            int b = order[k], s[2];
            int ns = block_succs(&F->blocks[b], s);
            for (int w = 0; w < W; w++) {
                uint64_t o = 0;
                for (int j = 0; j < ns; j++) o |= in[(size_t)s[j] * W + w];
                uint64_t i2 = use[(size_t)b * W + w] | (o & ~def[(size_t)b * W + w]);
                if (o != out[(size_t)b * W + w] || i2 != in[(size_t)b * W + w]) changed = true;
                out[(size_t)b * W + w] = o;
                in[(size_t)b * W + w] = i2;
            }
        }
    }
    int *start = xalloc(sizeof(int) * (size_t)(nv + 1)), *end = xalloc(sizeof(int) * (size_t)(nv + 1));
    for (int v = 0; v < nv; v++) start[v] = INT32_MAX, end[v] = -1;
#define EXT(v, p) do { if ((p) < start[v]) start[v] = (p); if ((p) > end[v]) end[v] = (p); } while (0)
    int *calls = NULL, ncalls = 0, ccalls = 0;
    for (int k = 0; k < norder; k++) {
        int b = order[k];
        Block *B = &F->blocks[b];
        for (int v = 0; v < nv; v++) {
            if (BIT(in, b, v)) EXT(v, bstart[b]);
            if (BIT(out, b, v)) EXT(v, bend[b]);
        }
        for (int i = 0; i < B->n; i++) {
            int p = bstart[b] + i;
            Inst *ins = &B->insts[i];
            int n = inst_uses(ins, u);
            for (int j = 0; j < n; j++) EXT(u[j], p);
            if (ins->dst >= 0) EXT(ins->dst, p);
            if (ins->op == I_CALL) PUSH(calls, ncalls, ccalls, p);
        }
    }
    Iv *ivs = xalloc(sizeof(Iv) * (size_t)(nv + 1));
    int niv = 0;
    for (int v = 0; v < nv; v++) {
        if (end[v] < 0) continue;
        Iv iv = {v, start[v], end[v], false};
        for (int c = 0; c < ncalls; c++)
            if (calls[c] > iv.start && calls[c] < iv.end) {
                iv.cross = true;
                break;
            }
        ivs[niv++] = iv;
    }
    qsort(ivs, (size_t)niv, sizeof(Iv), cmp_iv);

    for (int v = 0; v < nv; v++) vreg_reg[v] = -1, vreg_off[v] = 0;
    int holder[2][32]; /* register -> interval index holding it */
    for (int f = 0; f < 2; f++)
        for (int r = 0; r < 32; r++) holder[f][r] = -1;
    int *active = xalloc(sizeof(int) * (size_t)(niv + 1)), nact = 0;
    int nspill = 0;
    for (int i = 0; i < niv; i++) {
        Iv *cur = &ivs[i];
        int file = F->vcls[cur->v] == C_I ? 0 : 1;
        for (int a = 0; a < nact; a++) {
            Iv *o = &ivs[active[a]];
            if (o->end <= cur->start) {
                int of = F->vcls[o->v] == C_I ? 0 : 1;
                if (vreg_reg[o->v] >= 0) holder[of][vreg_reg[o->v]] = -1;
                active[a--] = active[--nact];
            }
        }
        const int *caller = file ? FP_CALLER : INT_CALLER, *callee = file ? FP_CALLEE : INT_CALLEE;
        int ncaller = file ? NELEM(FP_CALLER) : NELEM(INT_CALLER);
        int ncallee = file ? NELEM(FP_CALLEE) : NELEM(INT_CALLEE);
        int reg = -1;
        if (!cur->cross)
            for (int k = 0; k < ncaller && reg < 0; k++)
                if (holder[file][caller[k]] < 0) reg = caller[k];
        for (int k = 0; k < ncallee && reg < 0; k++)
            if (holder[file][callee[k]] < 0) reg = callee[k];
        if (reg < 0) {
            /* spill the active interval that ends furthest away, if that beats spilling cur */
            int best = -1;
            for (int a = 0; a < nact; a++) {
                Iv *o = &ivs[active[a]];
                if ((F->vcls[o->v] == C_I ? 0 : 1) != file || vreg_reg[o->v] < 0) continue;
                if (cur->cross && !is_callee_saved(vreg_reg[o->v], file)) continue;
                if (best < 0 || o->end > ivs[active[best]].end) best = a;
            }
            if (best >= 0 && ivs[active[best]].end > cur->end) {
                Iv *o = &ivs[active[best]];
                reg = vreg_reg[o->v];
                vreg_reg[o->v] = -1;
                vreg_off[o->v] = ++nspill; /* resolved to a frame offset later */
                active[best] = active[--nact];
            } else {
                vreg_off[cur->v] = ++nspill;
                continue;
            }
        }
        vreg_reg[cur->v] = reg;
        holder[file][reg] = i;
        active[nact++] = i;
        if (is_callee_saved(reg, file)) mark_saved(reg, file);
    }
#undef BIT
#undef SET
#undef EXT
    free(bstart); free(bend); free(use); free(def); free(in); free(out);
    free(start); free(end); free(calls); free(ivs); free(active);
}

/* ====================================================================== */
/* instruction helpers                                                    */
/* ====================================================================== */

static void mov_imm(int r, int64_t val) {
    uint64_t u = (uint64_t)val;
    if (u <= 0xffff) {
        E("mov %s, #%llu", XN[r], (unsigned long long)u);
        return;
    }
    if (~u <= 0xffff) {
        E("movn %s, #%llu", XN[r], (unsigned long long)(~u));
        return;
    }
    bool first = true;
    for (int sh = 0; sh < 64; sh += 16) {
        unsigned h = (unsigned)((u >> sh) & 0xffff);
        if (!h) continue;
        E("%s %s, #%u, lsl #%d", first ? "movz" : "movk", XN[r], h, sh);
        first = false;
    }
}

/* load/store `reg` from/to [base + off] with the right width */
static void mem_op(bool ld, int mem, int cls, int reg, int base, int64_t off) {
    int size = cls == C_F32 ? 4 : cls == C_F64 ? 8 : (mem & 0x7f);
    bool sg = (mem & 0x80) && cls == C_I;
    const char *sc, *un, *rn;
    if (cls != C_I) {
        sc = ld ? "ldr" : "str";
        un = ld ? "ldur" : "stur";
        rn = FR(reg, cls);
    } else if (ld) {
        switch (size) {
        case 1: sc = sg ? "ldrsb" : "ldrb"; un = sg ? "ldursb" : "ldurb"; rn = sg ? XN[reg] : WN[reg]; break;
        case 2: sc = sg ? "ldrsh" : "ldrh"; un = sg ? "ldursh" : "ldurh"; rn = sg ? XN[reg] : WN[reg]; break;
        case 4: sc = sg ? "ldrsw" : "ldr"; un = sg ? "ldursw" : "ldur"; rn = sg ? XN[reg] : WN[reg]; break;
        default: sc = "ldr"; un = "ldur"; rn = XN[reg]; break;
        }
    } else {
        switch (size) {
        case 1: sc = "strb"; un = "sturb"; rn = WN[reg]; break;
        case 2: sc = "strh"; un = "sturh"; rn = WN[reg]; break;
        case 4: sc = "str"; un = "stur"; rn = WN[reg]; break;
        default: sc = "str"; un = "stur"; rn = XN[reg]; break;
        }
    }
    const char *bn = base == 31 ? "sp" : XN[base];
    if (off == 0) E("%s %s, [%s]", sc, rn, bn);
    else if (off > 0 && off % size == 0 && off / size < 4096) E("%s %s, [%s, #%lld]", sc, rn, bn, (long long)off);
    else if (off >= -256 && off <= 255) E("%s %s, [%s, #%lld]", un, rn, bn, (long long)off);
    else {
        int t = base == 17 ? 8 : 17; /* the base may itself live in x17 */
        mov_imm(t, off);
        E("add %s, %s, %s", XN[t], bn, XN[t]);
        E("%s %s, [%s]", sc, rn, XN[t]);
    }
}

static int spill_size(int v) { return F->vcls[v] == C_F32 ? 4 : 8; }

/* integer operand -> register holding it */
static int gi(Opd o, int scratch) {
    if (o.k == O_IMM) {
        mov_imm(scratch, o.v);
        return scratch;
    }
    int v = (int)o.v;
    if (vreg_reg[v] >= 0) return vreg_reg[v];
    mem_op(true, 8, C_I, scratch, 29, -vreg_off[v]);
    return scratch;
}

/* float operand -> register holding it */
static int gf(Opd o, int scratch) {
    int v = (int)o.v;
    if (vreg_reg[v] >= 0) return vreg_reg[v];
    mem_op(true, spill_size(v), F->vcls[v], scratch, 29, -vreg_off[v]);
    return scratch;
}

static int di(int v) { return vreg_reg[v] >= 0 ? vreg_reg[v] : 16; }
static int df(int v) { return vreg_reg[v] >= 0 ? vreg_reg[v] : 31; }

static void fin(int v, int r) {
    if (vreg_reg[v] < 0) mem_op(false, spill_size(v), F->vcls[v], r, 29, -vreg_off[v]);
}

/* move an integer operand into a specific register */
static void into_int(int target, Opd o) {
    if (o.k == O_IMM) {
        mov_imm(target, o.v);
        return;
    }
    int v = (int)o.v;
    if (vreg_reg[v] >= 0) {
        if (vreg_reg[v] != target) E("mov %s, %s", XN[target], XN[vreg_reg[v]]);
    } else {
        mem_op(true, 8, C_I, target, 29, -vreg_off[v]);
    }
}

static void into_fp(int target, Opd o, int cls) {
    int v = (int)o.v;
    if (vreg_reg[v] >= 0) {
        if (vreg_reg[v] != target) E("fmov %s, %s", FR(target, cls), FR(vreg_reg[v], cls));
    } else {
        mem_op(true, spill_size(v), cls, target, 29, -vreg_off[v]);
    }
}

static void label(int b) { fprintf(O, "Lb%d_%d:\n", FN, b); }

static int next_block(int b) {
    int k = layout_idx[b];
    return (k >= 0 && k + 1 < norder) ? order[k + 1] : -1;
}

static void branch(int cc, int t1, int t2, int b) {
    int nx = next_block(b);
    if (t1 == nx) E("b.%s Lb%d_%d", CCN[CCINV[cc]], FN, t2);
    else if (t2 == nx) E("b.%s Lb%d_%d", CCN[cc], FN, t1);
    else {
        E("b.%s Lb%d_%d", CCN[cc], FN, t1);
        E("b Lb%d_%d", FN, t2);
    }
}

static void cmp_operands(Opd a, Opd b) {
    int ra = gi(a, 16);
    if (b.k == O_IMM && b.v >= 0 && b.v <= 4095) E("cmp %s, #%lld", XN[ra], (long long)b.v);
    else if (b.k == O_IMM && b.v < 0 && b.v >= -4095) E("cmn %s, #%lld", XN[ra], (long long)-b.v);
    else E("cmp %s, %s", XN[ra], XN[gi(b, 8)]);
}

static void addsub_imm(bool add, int d, int ra, int64_t v) {
    if (!add) v = (int64_t)(0 - (uint64_t)v); /* (wraps for INT64_MIN, which is what sub means) */
    if (v >= 0 && v <= 4095) E("add %s, %s, #%lld", XN[d], XN[ra], (long long)v);
    else if (v < 0 && v >= -4095) E("sub %s, %s, #%lld", XN[d], XN[ra], (long long)-v);
    else if (v > 0 && (v & 0xfff) == 0 && (v >> 12) <= 4095) E("add %s, %s, #%lld, lsl #12", XN[d], XN[ra], (long long)(v >> 12));
    else {
        mov_imm(8, v);
        E("add %s, %s, x8", XN[d], XN[ra]);
    }
}

/* SIMD arrangement of a vector op: 16b 8h 4s 2d (128-bit) or 8b 4h 2s (64-bit) */
static const char *varr(int64_t imm) {
    int es = VL_ESIZE(imm), total = VL_TOTAL(imm);
    static char buf[8];
    snprintf(buf, sizeof buf, "%d%c", total / es, es == 1 ? 'b' : es == 2 ? 'h' : es == 4 ? 's' : 'd');
    return buf;
}

/* vectors live in memory between operations: load into v29/v30, one instruction, store */
static void emit_vec(Inst *in) {
    int total = VL_TOTAL(in->imm), es = VL_ESIZE(in->imm);
    bool fl = VL_FLOAT(in->imm);
    char arr[8];
    snprintf(arr, sizeof arr, "%s", varr(in->imm));
    const char *bytes = total == 16 ? "16b" : "8b"; /* the logical ops work on bytes */
    char q = total == 16 ? 'q' : 'd';
    if (in->mem == VOP_SPLAT) {
        if (fl) {
            int r = gf(in->b, 30);
            E("dup v29.%s, v%d.%c[0]", arr, r, es == 4 ? 's' : 'd');
        } else {
            int r = gi(in->b, 16);
            E("dup v29.%s, %s", arr, es == 8 ? XN[r] : WN[r]);
        }
    } else {
        E("ldr %c29, [%s]", q, XN[gi(in->b, 16)]);
        if (in->mem != VOP_NEG) E("ldr %c30, [%s]", q, XN[gi(in->c, 16)]);
        switch (in->mem) {
        case VOP_ADD: E("%s v29.%s, v29.%s, v30.%s", fl ? "fadd" : "add", arr, arr, arr); break;
        case VOP_SUB: E("%s v29.%s, v29.%s, v30.%s", fl ? "fsub" : "sub", arr, arr, arr); break;
        case VOP_MUL: E("%s v29.%s, v29.%s, v30.%s", fl ? "fmul" : "mul", arr, arr, arr); break;
        case VOP_DIV: E("fdiv v29.%s, v29.%s, v30.%s", arr, arr, arr); break;
        case VOP_AND: E("and v29.%s, v29.%s, v30.%s", bytes, bytes, bytes); break;
        case VOP_OR: E("orr v29.%s, v29.%s, v30.%s", bytes, bytes, bytes); break;
        case VOP_XOR: E("eor v29.%s, v29.%s, v30.%s", bytes, bytes, bytes); break;
        case VOP_NEG: E("%s v29.%s, v29.%s", fl ? "fneg" : "neg", arr, arr); break;
        }
    }
    E("str %c29, [%s]", q, XN[gi(in->a, 17)]);
}

/* the sum of the lanes, into an integer or float register (pairwise for floats, like the interpreter) */
static void emit_vreduce(Inst *in) {
    int total = VL_TOTAL(in->imm), es = VL_ESIZE(in->imm), lanes = total / es;
    bool fl = VL_FLOAT(in->imm), sg = VL_SIGNED(in->imm);
    E("ldr %c29, [%s]", total == 16 ? 'q' : 'd', XN[gi(in->a, 16)]);
    if (fl) {
        int d = df(in->dst);
        if (es == 4 && lanes == 4) {
            E("faddp v29.4s, v29.4s, v29.4s");
            E("faddp s29, v29.2s");
        } else if (es == 4) {
            E("faddp s29, v29.2s");
        } else {
            E("faddp d29, v29.2d");
        }
        E("fmov %s, %s", FR(d, in->cls), es == 4 ? "s29" : "d29");
        fin(in->dst, d);
        return;
    }
    int d = di(in->dst);
    if (es == 8) {
        E("addp d29, v29.2d");
        E("fmov %s, d29", XN[d]);
    } else {
        if (es == 4 && lanes == 2) E("addp v29.2s, v29.2s, v29.2s");
        else E("addv %c29, v29.%s", es == 1 ? 'b' : es == 2 ? 'h' : 's', varr(in->imm));
        char lane = es == 1 ? 'b' : es == 2 ? 'h' : 's';
        if (sg) E("smov %s, v29.%c[0]", XN[d], lane);
        else E("umov %s, v29.%c[0]", WN[d], lane);
    }
    fin(in->dst, d);
}

/* returns true if it also consumed the following instruction */
static bool emit_inst(Inst *in, Inst *nx, int b, bool last_in_func) {
    int d;
    switch (in->op) {
    case I_NOP: return false;
    case I_GETARG:
        if (in->imm >= 1000) { /* past the 8 argument registers: on the caller's stack, just above our frame record */
            int off = 16 + 8 * (int)(in->imm - 1000);
            if (in->cls == C_I) {
                d = di(in->dst);
                E("ldr %s, [x29, #%d]", XN[d], off);
            } else {
                d = df(in->dst);
                E("ldr %s, [x29, #%d]", FR(d, in->cls), off);
            }
            fin(in->dst, d);
            return false;
        }
        if (in->cls == C_I) {
            d = di(in->dst);
            if (d != in->imm) E("mov %s, %s", XN[d], XN[in->imm]);
            fin(in->dst, d);
        } else {
            d = df(in->dst);
            if (d != in->imm) E("fmov %s, %s", FR(d, in->cls), FR((int)in->imm, in->cls));
            fin(in->dst, d);
        }
        return false;
    case I_ICONST:
        d = di(in->dst);
        mov_imm(d, in->imm);
        fin(in->dst, d);
        return false;
    case I_FCONST: {
        d = df(in->dst);
        if (in->cls == C_F32) {
            float fv = (float)in->fimm;
            uint32_t bits;
            memcpy(&bits, &fv, 4);
            if (bits == 0) E("fmov %s, wzr", SN[d]);
            else {
                mov_imm(16, bits);
                E("fmov %s, w16", SN[d]);
            }
        } else {
            uint64_t bits;
            memcpy(&bits, &in->fimm, 8);
            if (bits == 0) E("fmov %s, xzr", DN[d]);
            else {
                mov_imm(16, (int64_t)bits);
                E("fmov %s, x16", DN[d]);
            }
        }
        fin(in->dst, d);
        return false;
    }
    case I_MOV:
        if (in->cls == C_I) {
            d = di(in->dst);
            into_int(d, in->a);
            fin(in->dst, d);
        } else {
            d = df(in->dst);
            into_fp(d, in->a, in->cls);
            fin(in->dst, d);
        }
        return false;
    case I_ADD:
    case I_SUB: {
        int ra = gi(in->a, 16);
        d = di(in->dst);
        if (in->b.k == O_IMM) addsub_imm(in->op == I_ADD, d, ra, in->b.v);
        else E("%s %s, %s, %s", in->op == I_ADD ? "add" : "sub", XN[d], XN[ra], XN[gi(in->b, 8)]);
        fin(in->dst, d);
        return false;
    }
    case I_MUL: {
        int ra = gi(in->a, 16);
        d = di(in->dst);
        if (in->b.k == O_IMM && in->b.v > 0 && (in->b.v & (in->b.v - 1)) == 0) {
            int sh = 0;
            while (((int64_t)1 << sh) != in->b.v) sh++;
            if (sh == 0) E("mov %s, %s", XN[d], XN[ra]);
            else E("lsl %s, %s, #%d", XN[d], XN[ra], sh);
        } else {
            E("mul %s, %s, %s", XN[d], XN[ra], XN[gi(in->b, 8)]);
        }
        fin(in->dst, d);
        return false;
    }
    case I_SDIV: case I_UDIV: case I_AND: case I_OR: case I_XOR: case I_SMULH: case I_UMULH: {
        static const char *nm[] = {[I_SDIV] = "sdiv", [I_UDIV] = "udiv", [I_AND] = "and", [I_OR] = "orr", [I_XOR] = "eor",
                                   [I_SMULH] = "smulh", [I_UMULH] = "umulh"};
        int ra = gi(in->a, 16), rb = gi(in->b, 8);
        d = di(in->dst);
        E("%s %s, %s, %s", nm[in->op], XN[d], XN[ra], XN[rb]);
        fin(in->dst, d);
        return false;
    }
    case I_SREM: case I_UREM: {
        int ra = gi(in->a, 16), rb = gi(in->b, 8);
        d = di(in->dst);
        E("%s x17, %s, %s", in->op == I_SREM ? "sdiv" : "udiv", XN[ra], XN[rb]);
        E("msub %s, x17, %s, %s", XN[d], XN[rb], XN[ra]);
        fin(in->dst, d);
        return false;
    }
    case I_SHL: case I_ASR: case I_LSR: {
        const char *nm = in->op == I_SHL ? "lsl" : in->op == I_ASR ? "asr" : "lsr";
        int ra = gi(in->a, 16);
        d = di(in->dst);
        if (in->b.k == O_IMM) E("%s %s, %s, #%lld", nm, XN[d], XN[ra], (long long)(in->b.v & 63));
        else E("%s %s, %s, %s", nm, XN[d], XN[ra], XN[gi(in->b, 8)]);
        fin(in->dst, d);
        return false;
    }
    case I_NEG: case I_NOT: {
        int ra = gi(in->a, 16);
        d = di(in->dst);
        E("%s %s, %s", in->op == I_NEG ? "neg" : "mvn", XN[d], XN[ra]);
        fin(in->dst, d);
        return false;
    }
    case I_FADD: case I_FSUB: case I_FMUL: case I_FDIV: {
        static const char *nm[] = {[I_FADD] = "fadd", [I_FSUB] = "fsub", [I_FMUL] = "fmul", [I_FDIV] = "fdiv"};
        int ra = gf(in->a, 29), rb = gf(in->b, 30);
        d = df(in->dst);
        E("%s %s, %s, %s", nm[in->op], FR(d, in->cls), FR(ra, in->cls), FR(rb, in->cls));
        fin(in->dst, d);
        return false;
    }
    case I_FNEG: case I_FSQRT: case I_FABS: {
        const char *nm = in->op == I_FNEG ? "fneg" : in->op == I_FSQRT ? "fsqrt" : "fabs";
        int ra = gf(in->a, 29);
        d = df(in->dst);
        E("%s %s, %s", nm, FR(d, in->cls), FR(ra, in->cls));
        fin(in->dst, d);
        return false;
    }
    case I_CMP:
    case I_FCMP: {
        if (in->op == I_CMP) cmp_operands(in->a, in->b);
        else {
            int c = F->vcls[in->a.v];
            int ra = gf(in->a, 29), rb = gf(in->b, 30);
            E("fcmp %s, %s", FR(ra, c), FR(rb, c));
        }
        if (nx && nx->op == I_BR && nx->a.k == O_REG && nx->a.v == in->dst && usecnt[in->dst] == 1) {
            branch(in->mem, nx->t1, nx->t2, b);
            return true;
        }
        d = di(in->dst);
        E("cset %s, %s", XN[d], CCN[in->mem]);
        fin(in->dst, d);
        return false;
    }
    case I_SEL: {
        int rc = gi(in->a, 16);
        E("cmp %s, #0", XN[rc]);
        if (in->cls == C_I) {
            int rb = gi(in->b, 16), rcc = gi(in->c, 8);
            d = di(in->dst);
            E("csel %s, %s, %s, ne", XN[d], XN[rb], XN[rcc]);
        } else {
            int rb = gf(in->b, 29), rcc = gf(in->c, 30);
            d = df(in->dst);
            E("fcsel %s, %s, %s, ne", FR(d, in->cls), FR(rb, in->cls), FR(rcc, in->cls));
        }
        fin(in->dst, d);
        return false;
    }
    case I_EXT: {
        int ra = gi(in->a, 16);
        d = di(in->dst);
        int size = in->mem & 0x7f;
        bool sg = in->mem & 0x80;
        if (size == 1) sg ? E("sxtb %s, %s", XN[d], WN[ra]) : E("and %s, %s, #0xff", XN[d], XN[ra]);
        else if (size == 2) sg ? E("sxth %s, %s", XN[d], WN[ra]) : E("and %s, %s, #0xffff", XN[d], XN[ra]);
        else if (size == 4) sg ? E("sxtw %s, %s", XN[d], WN[ra]) : E("mov %s, %s", WN[d], WN[ra]);
        else if (d != ra) E("mov %s, %s", XN[d], XN[ra]);
        fin(in->dst, d);
        return false;
    }
    case I_SCVTF: case I_UCVTF: {
        int ra = gi(in->a, 16);
        d = df(in->dst);
        E("%s %s, %s", in->op == I_SCVTF ? "scvtf" : "ucvtf", FR(d, in->cls), XN[ra]);
        fin(in->dst, d);
        return false;
    }
    case I_FCVTZS: case I_FCVTZU: {
        int c = F->vcls[in->a.v];
        int ra = gf(in->a, 29);
        d = di(in->dst);
        E("%s %s, %s", in->op == I_FCVTZS ? "fcvtzs" : "fcvtzu", XN[d], FR(ra, c));
        fin(in->dst, d);
        return false;
    }
    case I_FCVT: {
        int c = F->vcls[in->a.v];
        int ra = gf(in->a, 29);
        d = df(in->dst);
        if (c == in->cls) {
            if (d != ra) E("fmov %s, %s", FR(d, c), FR(ra, c));
        } else {
            E("fcvt %s, %s", FR(d, in->cls), FR(ra, c));
        }
        fin(in->dst, d);
        return false;
    }
    case I_LOAD: {
        int base, off = (int)in->imm;
        if (in->slot >= 0) {
            base = 29;
            off = -slot_off[in->slot] + (int)in->imm;
        } else {
            base = gi(in->a, 17);
        }
        if (in->cls == C_I) {
            d = di(in->dst);
            mem_op(true, in->mem, C_I, d, base, off);
        } else {
            d = df(in->dst);
            mem_op(true, in->mem, in->cls, d, base, off);
        }
        fin(in->dst, d);
        return false;
    }
    case I_STORE: {
        int base, off = (int)in->imm;
        int rv;
        if (in->cls == C_I) rv = (in->b.k == O_IMM && in->b.v == 0) ? 31 : gi(in->b, 16);
        else rv = gf(in->b, 29);
        if (in->slot >= 0) {
            base = 29;
            off = -slot_off[in->slot] + (int)in->imm;
        } else {
            base = gi(in->a, 17);
        }
        mem_op(false, in->mem, in->cls, rv, base, off);
        return false;
    }
    case I_ADDR_SLOT: {
        d = di(in->dst);
        int off = slot_off[in->slot];
        if (off <= 4095) E("sub %s, x29, #%d", XN[d], off);
        else {
            mov_imm(17, off);
            E("sub %s, x29, x17", XN[d]);
        }
        fin(in->dst, d);
        return false;
    }
    case I_ADDR_SYM:
        d = di(in->dst);
        if (in->mem) { /* defined in another image (a dylib): go through the GOT */
            E("adrp %s, %s@GOTPAGE", XN[d], in->sym);
            E("ldr %s, [%s, %s@GOTPAGEOFF]", XN[d], XN[d], in->sym);
        } else {
            E("adrp %s, %s@PAGE", XN[d], in->sym);
            E("add %s, %s, %s@PAGEOFF", XN[d], XN[d], in->sym);
        }
        fin(in->dst, d);
        return false;
    case I_CALL: {
        /* an indirect call (sym == NULL) carries the callee as its last operand */
        int nargs = in->sym ? in->nargs : in->nargs - 1;
        /* on the stack, 8 bytes each and in order: C varargs (Apple's ABI), or (between Flint functions)
         * fixed arguments that don't fit in x0-x7 / d0-d7 */
        bool *on_stack = xalloc(sizeof(bool) * (size_t)(nargs + 1));
        int nstack = 0, ic = 0, fc = 0;
        for (int j = 0; j < nargs; j++) {
            bool fp = in->argcls[j] != C_I;
            on_stack[j] = j >= in->nfixed || (fp ? fc++ : ic++) >= 8;
            nstack += on_stack[j];
        }
        int space = (nstack * 8 + 15) & ~15;
        if (space) E("sub sp, sp, #%d", space);
        for (int j = 0, k = 0; j < nargs; j++) {
            if (!on_stack[j]) continue;
            if (in->argcls[j] == C_I) E("str %s, [sp, #%d]", XN[gi(in->args[j], 16)], 8 * k);
            else E("str %s, [sp, #%d]", DN[gf(in->args[j], 31)], 8 * k);
            k++;
        }
        ic = fc = 0;
        for (int j = 0; j < nargs; j++) {
            if (on_stack[j]) continue;
            if (in->argcls[j] == C_I) into_int(ic++, in->args[j]);
            else into_fp(fc++, in->args[j], in->argcls[j]);
        }
        free(on_stack);
        if (in->sym) E("bl %s", in->sym);
        else { /* loaded last: argument setup only writes x0-x7, d0-d7 and scratch registers */
            int rc = gi(in->args[nargs], 17);
            E("blr %s", XN[rc]);
        }
        if (space) E("add sp, sp, #%d", space);
        if (in->dst >= 0) {
            if (in->cls == C_I) {
                d = di(in->dst);
                if (d != 0) E("mov %s, x0", XN[d]);
            } else {
                d = df(in->dst);
                if (d != 0) E("fmov %s, %s", FR(d, in->cls), FR(0, in->cls));
            }
            fin(in->dst, d);
        }
        return false;
    }
    case I_MEMCPY:
    case I_MEMZERO: {
        int rd = gi(in->a, 16);
        int rs = in->op == I_MEMCPY ? gi(in->b, 17) : 31;
        int64_t n = in->imm, i = 0;
        for (; i + 8 <= n; i += 8) {
            if (in->op == I_MEMCPY) {
                E("ldr x8, [%s, #%lld]", XN[rs], (long long)i);
                E("str x8, [%s, #%lld]", XN[rd], (long long)i);
            } else E("str xzr, [%s, #%lld]", XN[rd], (long long)i);
        }
        for (; i + 4 <= n; i += 4) {
            if (in->op == I_MEMCPY) {
                E("ldr w8, [%s, #%lld]", XN[rs], (long long)i);
                E("str w8, [%s, #%lld]", XN[rd], (long long)i);
            } else E("str wzr, [%s, #%lld]", XN[rd], (long long)i);
        }
        for (; i < n; i++) {
            if (in->op == I_MEMCPY) {
                E("ldrb w8, [%s, #%lld]", XN[rs], (long long)i);
                E("strb w8, [%s, #%lld]", XN[rd], (long long)i);
            } else E("strb wzr, [%s, #%lld]", XN[rd], (long long)i);
        }
        return false;
    }
    case I_JMP:
        if (in->t1 != next_block(b)) E("b Lb%d_%d", FN, in->t1);
        return false;
    case I_BR: {
        int rc = gi(in->a, 16);
        int nb = next_block(b);
        if (in->t1 == nb) E("cbz %s, Lb%d_%d", XN[rc], FN, in->t2);
        else if (in->t2 == nb) E("cbnz %s, Lb%d_%d", XN[rc], FN, in->t1);
        else {
            E("cbnz %s, Lb%d_%d", XN[rc], FN, in->t1);
            E("b Lb%d_%d", FN, in->t2);
        }
        return false;
    }
    case I_RET:
        if (in->a.k != O_NONE) {
            if (in->cls == C_I) into_int(0, in->a);
            else into_fp(0, in->a, in->cls);
        }
        if (!last_in_func) E("b Lret%d", FN);
        return false;
    case I_TRAP:
        E("brk #1");
        return false;
    case I_VEC:
        emit_vec(in);
        return false;
    case I_VREDUCE:
        emit_vreduce(in);
        return false;
    }
    fprintf(stderr, "flint: internal error: unknown IR op %d\n", in->op);
    exit(1);
}

/* ====================================================================== */
/* functions                                                              */
/* ====================================================================== */

static int cmp_seq(const void *a, const void *b) {
    const Block *x = &F->blocks[*(const int *)a], *y = &F->blocks[*(const int *)b];
    if (x->cold != y->cold) return x->cold - y->cold;
    return x->seq - y->seq;
}

static void emit_func(IRFunc *f, int idx) {
    F = f;
    FN = idx;
    int nb = f->nblocks, nv = f->nv;
    order = xalloc(sizeof(int) * (size_t)(nb + 1));
    layout_idx = xalloc(sizeof(int) * (size_t)(nb + 1));
    norder = 0;
    for (int b = 0; b < nb; b++) {
        layout_idx[b] = -1;
        if (!f->blocks[b].dead && (b == 0 || f->blocks[b].n)) order[norder++] = b;
    }
    /* entry block first, then by start order, cold blocks last */
    if (norder > 1) qsort(order + 1, (size_t)(norder - 1), sizeof(int), cmp_seq);
    for (int k = 0; k < norder; k++) {
        layout_idx[order[k]] = k;
        Block *B = &f->blocks[order[k]];
        if (!B->n || !inst_is_term(&B->insts[B->n - 1])) {
            fprintf(stderr, "flint: internal error: block b%d of %s has no terminator\n", order[k], f->sym);
            exit(1);
        }
    }
    vreg_reg = xalloc(sizeof(int) * (size_t)(nv + 1));
    vreg_off = xalloc(sizeof(int) * (size_t)(nv + 1));
    usecnt = xalloc(sizeof(int) * (size_t)(nv + 1));
    nsaved_int = nsaved_fp = 0;
    int u[64];
    for (int k = 0; k < norder; k++) {
        Block *B = &f->blocks[order[k]];
        for (int i = 0; i < B->n; i++) {
            int n = inst_uses(&B->insts[i], u);
            for (int j = 0; j < n; j++) usecnt[u[j]]++;
        }
    }
    regalloc();

    /* frame: [x29-8 ...] callee-saved registers, then slots, then spills */
    int cursor = 8 * (nsaved_int + nsaved_fp);
    cursor = (cursor + 15) & ~15;
    slot_off = xalloc(sizeof(int) * (size_t)(f->nslots + 1));
    bool *used = xalloc((size_t)f->nslots + 1);
    for (int k = 0; k < norder; k++) {
        Block *B = &f->blocks[order[k]];
        for (int i = 0; i < B->n; i++)
            if (B->insts[i].slot >= 0 && (B->insts[i].op == I_LOAD || B->insts[i].op == I_STORE || B->insts[i].op == I_ADDR_SLOT))
                used[B->insts[i].slot] = true;
    }
    for (int s = 0; s < f->nslots; s++) {
        if (!used[s]) continue;
        int al = f->slots[s].align < 1 ? 1 : f->slots[s].align;
        cursor += f->slots[s].size;
        cursor = (cursor + al - 1) / al * al;
        slot_off[s] = cursor;
    }
    for (int v = 0; v < nv; v++) {
        if (vreg_reg[v] < 0 && vreg_off[v] > 0) {
            cursor = (cursor + 8 + 7) & ~7;
            vreg_off[v] = cursor;
        }
    }
    frame_size = (cursor + 15) & ~15;
    free(used);

    fprintf(O, "\n\t.p2align 2\n");
    if (f->is_main || f->exported) fprintf(O, "\t.globl %s\n", f->sym);
    fprintf(O, "%s:\n", f->sym);
    E("stp x29, x30, [sp, #-16]!");
    E("mov x29, sp");
    if (frame_size) {
        if (frame_size <= 4095) E("sub sp, sp, #%d", frame_size);
        else {
            mov_imm(16, frame_size);
            E("sub sp, sp, x16");
        }
    }
    int off = 0;
    for (int i = 0; i < nsaved_int; i++) E("stur %s, [x29, #%d]", XN[saved_int[i]], -(off += 8));
    for (int i = 0; i < nsaved_fp; i++) E("stur %s, [x29, #%d]", DN[saved_fp[i]], -(off += 8));

    for (int k = 0; k < norder; k++) {
        int b = order[k];
        Block *B = &f->blocks[b];
        if (k > 0) label(b);
        for (int i = 0; i < B->n; i++) {
            Inst *nx = i + 1 < B->n ? &B->insts[i + 1] : NULL;
            bool last = (k == norder - 1) && (i == B->n - 1);
            if (emit_inst(&B->insts[i], nx, b, last)) i++;
        }
    }
    fprintf(O, "Lret%d:\n", FN);
    if (f->is_main && f->retcls < 0) E("mov x0, #0");
    off = 0;
    for (int i = 0; i < nsaved_int; i++) E("ldur %s, [x29, #%d]", XN[saved_int[i]], -(off += 8));
    for (int i = 0; i < nsaved_fp; i++) E("ldur %s, [x29, #%d]", DN[saved_fp[i]], -(off += 8));
    E("mov sp, x29");
    E("ldp x29, x30, [sp], #16");
    E("ret");
    free(order); free(layout_idx); free(vreg_reg); free(vreg_off); free(usecnt); free(slot_off);
}

void emit_arm64(Module *m, FILE *out) {
    O = out;
    init_names();
    fprintf(O, "\t.build_version macos, 13, 0\n");
    fprintf(O, "\t.section __TEXT,__text,regular,pure_instructions\n");
    for (int i = 0; i < m->nfuncs; i++) emit_func(m->funcs[i], i);
    if (m->nstrs) {
        fprintf(O, "\n\t.section __TEXT,__const\n");
        for (int i = 0; i < m->nstrs; i++) {
            fprintf(O, "%s:\n\t.byte ", m->strs[i].label);
            for (int j = 0; j < m->strs[i].len; j++) fprintf(O, "%d,", (unsigned char)m->strs[i].data[j]);
            fprintf(O, "0\n");
        }
    }
    if (m->nglobals) {
        fprintf(O, "\n\t.section __DATA,__data\n");
        for (int i = 0; i < m->nglobals; i++) {
            GlobalVar *g = &m->globals[i];
            if (g->kind == GV_ZERO) continue; /* in __bss below: no bytes in the executable */
            int al = 0;
            while ((1 << al) < g->align) al++;
            if (g->exported) fprintf(O, "\t.globl %s\n", g->sym);
            fprintf(O, "\t.p2align %d\n%s:\n", al, g->sym);
            if (g->kind == GV_STR) {
                fprintf(O, "\t.quad %s\n\t.quad %d\n", m->strs[g->sidx].label, m->strs[g->sidx].len);
            } else if (g->kind == GV_INT) {
                const char *dir = g->size == 1 ? ".byte" : g->size == 2 ? ".short" : g->size == 4 ? ".long" : ".quad";
                uint64_t v = (uint64_t)g->ival;
                if (g->size < 8) v &= ((uint64_t)1 << (8 * g->size)) - 1;
                fprintf(O, "\t%s %llu\n", dir, (unsigned long long)v);
            } else if (g->kind == GV_DATA) {
                int at = 0;
                for (int k = 0; k < g->nitems; k++) {
                    DataItem *d = &g->items[k];
                    if (d->off > at) fprintf(O, "\t.space %d\n", d->off - at);
                    if (d->sym) {
                        if (d->val) fprintf(O, "\t.quad %s+%lld\n", d->sym, (long long)d->val);
                        else fprintf(O, "\t.quad %s\n", d->sym);
                    } else {
                        const char *dir = d->size == 1 ? ".byte" : d->size == 2 ? ".short" : d->size == 4 ? ".long" : ".quad";
                        uint64_t v = (uint64_t)d->val;
                        if (d->size < 8) v &= ((uint64_t)1 << (8 * d->size)) - 1;
                        fprintf(O, "\t%s %llu\n", dir, (unsigned long long)v);
                    }
                    at = d->off + d->size;
                }
                if (g->size > at) fprintf(O, "\t.space %d\n", g->size - at);
            } else if (g->kind == GV_FILL) {
                fprintf(O, "\t.fill %d, 1, 0xaa\n", g->size);
            }
        }
        for (int i = 0; i < m->nglobals; i++) {
            GlobalVar *g = &m->globals[i];
            if (g->kind != GV_ZERO) continue;
            int al = 0;
            while ((1 << al) < g->align) al++;
            if (g->exported) fprintf(O, "\t.globl %s\n", g->sym);
            fprintf(O, "\t.zerofill __DATA,__bss,%s,%d,%d\n", g->sym, g->size ? g->size : 1, al);
        }
    }
    /* error code -> name, for the runtime (main returning an error, printing errors) */
    fprintf(O, "\n\t.section __TEXT,__const\n");
    for (int i = 0; i < m->nerrnames; i++) {
        fprintf(O, "l_fl_err_%d:\n\t.byte ", i);
        for (const char *c = m->errnames[i]; *c; c++) fprintf(O, "%d,", (unsigned char)*c);
        fprintf(O, "0\n");
    }
    fprintf(O, "\n\t.section __DATA,__const\n\t.p2align 3\n\t.globl _fl_error_names\n_fl_error_names:\n");
    for (int i = 0; i < m->nerrnames; i++) fprintf(O, "\t.quad l_fl_err_%d\n", i);
    fprintf(O, "\t.globl _fl_error_count\n_fl_error_count:\n\t.quad %d\n", m->nerrnames);
    fprintf(O, "\t.globl _fl_main_errors\n_fl_main_errors:\n\t.quad %d\n", m->main_errors ? 1 : 0);
    fprintf(O, "\n.subsections_via_symbols\n");
}
