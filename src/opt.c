/* opt.c - IR optimizations
 *
 *   promote      slots whose address never escapes become vregs (mem2reg + scalar
 *                replacement of aggregates: each field gets its own vreg)
 *   const_prop   fold constants, turn constant operands into immediates
 *   copy_prop    replace copies with their source
 *   simplify_cfg thread jumps through empty blocks, drop unreachable blocks
 *   dce          remove instructions whose results are unused
 *   coalesce     "t = a op b; x = t"  ->  "x = a op b"
 */
#include "flint.h"

static int *defcnt, *usecnt;
static int cntcap;

static void count(IRFunc *f) {
    if (cntcap < f->nv + 1) {
        cntcap = f->nv * 2 + 16;
        defcnt = xrealloc(defcnt, sizeof(int) * (size_t)cntcap);
        usecnt = xrealloc(usecnt, sizeof(int) * (size_t)cntcap);
    }
    memset(defcnt, 0, sizeof(int) * (size_t)(f->nv + 1));
    memset(usecnt, 0, sizeof(int) * (size_t)(f->nv + 1));
    int u[64];
    for (int b = 0; b < f->nblocks; b++) {
        Block *B = &f->blocks[b];
        if (B->dead) continue;
        for (int i = 0; i < B->n; i++) {
            Inst *in = &B->insts[i];
            if (in->dst >= 0) defcnt[in->dst]++;
            if (in->op == I_CALL && in->nargs > 64) continue;
            int n = inst_uses(in, u);
            for (int k = 0; k < n; k++) usecnt[u[k]]++;
        }
    }
}

static void compact(IRFunc *f) {
    for (int b = 0; b < f->nblocks; b++) {
        Block *B = &f->blocks[b];
        int j = 0;
        for (int i = 0; i < B->n; i++)
            if (B->insts[i].op != I_NOP) B->insts[j++] = B->insts[i];
        B->n = j;
    }
}

static void nop(Inst *in) {
    in->op = I_NOP;
    in->dst = -1;
    in->a.k = in->b.k = in->c.k = O_NONE;
}

/* ---------------- promotion ---------------- */

typedef struct {
    int64_t off;
    int mem, cls, vreg;
} Acc;

static void promote(IRFunc *f) {
    int ns = f->nslots;
    if (!ns) return;
    bool *esc = xalloc((size_t)ns);
    Acc **acc = xalloc(sizeof(Acc *) * (size_t)ns);
    int *na = xalloc(sizeof(int) * (size_t)ns), *ca = xalloc(sizeof(int) * (size_t)ns);
    for (int b = 0; b < f->nblocks; b++) {
        Block *B = &f->blocks[b];
        for (int i = 0; i < B->n; i++) {
            Inst *in = &B->insts[i];
            if (in->op == I_ADDR_SLOT) esc[in->slot] = true;
            if ((in->op == I_LOAD || in->op == I_STORE) && in->slot >= 0) {
                int s = in->slot;
                int k;
                for (k = 0; k < na[s]; k++)
                    if (acc[s][k].off == in->imm) break;
                if (k < na[s]) {
                    if (acc[s][k].mem != in->mem || acc[s][k].cls != in->cls) esc[s] = true;
                } else {
                    Acc a = {in->imm, in->mem, in->cls, -1};
                    PUSH(acc[s], na[s], ca[s], a);
                }
            }
        }
    }
    for (int s = 0; s < ns; s++) {
        if (esc[s]) continue;
        for (int i = 0; i < na[s] && !esc[s]; i++)
            for (int j = i + 1; j < na[s]; j++) {
                int64_t a0 = acc[s][i].off, a1 = a0 + (acc[s][i].mem & 0x7f);
                int64_t b0 = acc[s][j].off, b1 = b0 + (acc[s][j].mem & 0x7f);
                if (a0 < b1 && b0 < a1) {
                    esc[s] = true;
                    break;
                }
            }
        if (!esc[s])
            for (int i = 0; i < na[s]; i++) acc[s][i].vreg = new_vreg_in(f, acc[s][i].cls);
    }
    for (int b = 0; b < f->nblocks; b++) {
        Block *B = &f->blocks[b];
        for (int i = 0; i < B->n; i++) {
            Inst *in = &B->insts[i];
            if ((in->op != I_LOAD && in->op != I_STORE) || in->slot < 0 || esc[in->slot]) continue;
            int s = in->slot, v = -1;
            for (int k = 0; k < na[s]; k++)
                if (acc[s][k].off == in->imm) v = acc[s][k].vreg;
            if (in->op == I_LOAD) {
                in->op = I_MOV;
                in->a = (Opd){O_REG, v};
            } else {
                in->op = I_MOV;
                in->dst = v;
                in->a = in->b;
                in->b.k = O_NONE;
            }
            in->slot = -1;
            in->imm = 0;
        }
    }
    for (int s = 0; s < ns; s++) free(acc[s]);
    free(acc);
    free(na);
    free(ca);
    free(esc);
}

/* ---------------- constant propagation ---------------- */

static int64_t ext_val(int64_t x, int mem) {
    int bits = (mem & 0x7f) * 8;
    if (bits >= 64) return x;
    uint64_t m = ((uint64_t)1 << bits) - 1, u = (uint64_t)x & m;
    if ((mem & 0x80) && (u >> (bits - 1))) u |= ~m;
    return (int64_t)u;
}

static bool cc_eval(int cc, int64_t a, int64_t b) {
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    switch (cc) {
    case CC_EQ: return a == b;
    case CC_NE: return a != b;
    case CC_LT: return a < b;
    case CC_LE: return a <= b;
    case CC_GT: return a > b;
    case CC_GE: return a >= b;
    case CC_LO: return ua < ub;
    case CC_LS: return ua <= ub;
    case CC_HI: return ua > ub;
    case CC_HS: return ua >= ub;
    }
    return false;
}

static bool fold_bin(int op, int64_t a, int64_t b, int64_t *out) {
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    switch (op) {
    case I_ADD: *out = (int64_t)(ua + ub); return true;
    case I_SUB: *out = (int64_t)(ua - ub); return true;
    case I_MUL: *out = (int64_t)(ua * ub); return true;
    case I_AND: *out = a & b; return true;
    case I_OR: *out = a | b; return true;
    case I_XOR: *out = a ^ b; return true;
    case I_SHL: *out = (int64_t)(ua << (ub & 63)); return true;
    case I_ASR: *out = a >> (ub & 63); return true;
    case I_LSR: *out = (int64_t)(ua >> (ub & 63)); return true;
    case I_SDIV: if (b == 0 || (a == INT64_MIN && b == -1)) return false; *out = a / b; return true;
    case I_UDIV: if (b == 0) return false; *out = (int64_t)(ua / ub); return true;
    case I_SREM: if (b == 0 || (a == INT64_MIN && b == -1)) return false; *out = a % b; return true;
    case I_UREM: if (b == 0) return false; *out = (int64_t)(ua % ub); return true;
    case I_SMULH: *out = (int64_t)(((__int128)a * (__int128)b) >> 64); return true;
    case I_UMULH: *out = (int64_t)(uint64_t)(((unsigned __int128)ua * ub) >> 64); return true;
    }
    return false;
}

static void make_const(Inst *in, int64_t v) {
    int d = in->dst;
    memset(in, 0, sizeof *in);
    in->op = I_ICONST;
    in->dst = d;
    in->imm = v;
    in->slot = -1;
    in->t1 = in->t2 = -1;
}

static bool const_prop(IRFunc *f) {
    count(f);
    bool ch = false;
    bool *isc = xalloc((size_t)f->nv + 1);
    int64_t *cv = xalloc(sizeof(int64_t) * (size_t)(f->nv + 1));
    for (int b = 0; b < f->nblocks; b++) {
        Block *B = &f->blocks[b];
        if (B->dead) continue;
        for (int i = 0; i < B->n; i++) {
            Inst *in = &B->insts[i];
            if (in->op == I_MOV && in->cls == C_I && in->a.k == O_IMM) {
                make_const(in, in->a.v);
                ch = true;
            }
            if (in->op == I_ICONST && defcnt[in->dst] == 1) {
                isc[in->dst] = true;
                cv[in->dst] = in->imm;
            }
        }
    }
#define SUB(o) (((o).k == O_REG && isc[(o).v]) ? ((o) = (Opd){O_IMM, cv[(o).v]}, ch = true, true) : false)
    static const int mirror[] = {CC_EQ, CC_NE, CC_GT, CC_GE, CC_LT, CC_LE, CC_HI, CC_HS, CC_LO, CC_LS, CC_MI, CC_PL};
    for (int b = 0; b < f->nblocks; b++) {
        Block *B = &f->blocks[b];
        if (B->dead) continue;
        for (int i = 0; i < B->n; i++) {
            Inst *in = &B->insts[i];
            int64_t r;
            switch (in->op) {
            case I_ADD: case I_MUL: case I_AND: case I_OR: case I_XOR:
                SUB(in->a);
                SUB(in->b);
                if (in->a.k == O_IMM && in->b.k == O_REG) {
                    Opd t = in->a;
                    in->a = in->b;
                    in->b = t;
                }
                if (in->a.k == O_IMM && in->b.k == O_IMM && fold_bin(in->op, in->a.v, in->b.v, &r)) {
                    make_const(in, r);
                    ch = true;
                } else if (in->b.k == O_IMM && ((in->op == I_ADD || in->op == I_OR || in->op == I_XOR) && in->b.v == 0)) {
                    in->op = I_MOV; /* x + 0 */
                    in->b.k = O_NONE;
                    ch = true;
                } else if (in->b.k == O_IMM && in->op == I_MUL && in->b.v == 1) {
                    in->op = I_MOV;
                    in->b.k = O_NONE;
                    ch = true;
                }
                break;
            case I_SUB: case I_SHL: case I_ASR: case I_LSR:
            case I_SDIV: case I_UDIV: case I_SREM: case I_UREM: case I_SMULH: case I_UMULH:
                SUB(in->a);
                SUB(in->b);
                if (in->a.k == O_IMM && in->b.k == O_IMM && fold_bin(in->op, in->a.v, in->b.v, &r)) {
                    make_const(in, r);
                    ch = true;
                } else if (in->b.k == O_IMM && in->b.v == 0 && in->op <= I_LSR && in->op != I_SDIV &&
                           in->op != I_UDIV && in->op != I_SREM && in->op != I_UREM && in->a.k == O_REG) {
                    in->op = I_MOV;
                    in->b.k = O_NONE;
                    ch = true;
                }
                break;
            case I_NEG: case I_NOT: case I_EXT:
                if (SUB(in->a)) {
                    int64_t x = in->a.v;
                    make_const(in, in->op == I_NEG ? (int64_t)(0 - (uint64_t)x) : in->op == I_NOT ? ~x : ext_val(x, in->mem));
                }
                break;
            case I_CMP:
                SUB(in->a);
                SUB(in->b);
                if (in->a.k == O_IMM && in->b.k == O_IMM) make_const(in, cc_eval(in->mem, in->a.v, in->b.v));
                else if (in->a.k == O_IMM) {
                    Opd t = in->a;
                    in->a = in->b;
                    in->b = t;
                    in->mem = mirror[in->mem];
                }
                break;
            case I_MOV:
                if (in->cls == C_I && SUB(in->a)) make_const(in, in->a.v);
                break;
            case I_STORE:
                if (in->cls == C_I) SUB(in->b);
                break;
            case I_CALL:
                for (int k = 0; k < in->nargs; k++)
                    if (in->argcls[k] == C_I) SUB(in->args[k]);
                break;
            case I_RET:
                if (in->cls == C_I) SUB(in->a);
                break;
            case I_BR:
                if (SUB(in->a)) {
                    int t = in->a.v ? in->t1 : in->t2;
                    in->op = I_JMP;
                    in->t1 = t;
                    in->a.k = O_NONE;
                }
                break;
            case I_SEL:
                if (SUB(in->a)) {
                    in->op = I_MOV;
                    in->a = in->a.v ? in->b : in->c;
                    in->b.k = in->c.k = O_NONE;
                }
                break;
            }
        }
    }
#undef SUB
    free(isc);
    free(cv);
    return ch;
}

/* ---------------- copy propagation ---------------- */

static bool replace_uses(Inst *in, const int *rep) {
    bool ch = false;
    if (in->op == I_CALL) {
        for (int k = 0; k < in->nargs; k++)
            if (in->args[k].k == O_REG && rep[in->args[k].v] >= 0) in->args[k].v = rep[in->args[k].v], ch = true;
        return ch;
    }
    Opd *ops[3] = {&in->a, &in->b, &in->c};
    for (int k = 0; k < 3; k++)
        if (ops[k]->k == O_REG && rep[ops[k]->v] >= 0) ops[k]->v = rep[ops[k]->v], ch = true;
    return ch;
}

static bool copy_prop(IRFunc *f) {
    count(f);
    bool ch = false;
    int nv = f->nv;
    int *rep = xalloc(sizeof(int) * (size_t)(nv + 1));
    for (int i = 0; i <= nv; i++) rep[i] = -1;
    /* global: t = mov s, both defined exactly once -> t is always s */
    for (int b = 0; b < f->nblocks; b++) {
        Block *B = &f->blocks[b];
        if (B->dead) continue;
        for (int i = 0; i < B->n; i++) {
            Inst *in = &B->insts[i];
            if (in->op == I_MOV && in->a.k == O_REG && defcnt[in->dst] == 1 && defcnt[in->a.v] == 1 &&
                in->dst != in->a.v)
                rep[in->dst] = (int)in->a.v;
        }
    }
    for (int v = 0; v < nv; v++) {
        int r = rep[v], guard = 0;
        while (r >= 0 && rep[r] >= 0 && guard++ < 64) r = rep[r];
        rep[v] = r;
    }
    for (int b = 0; b < f->nblocks; b++) {
        Block *B = &f->blocks[b];
        if (B->dead) continue;
        for (int i = 0; i < B->n; i++) ch |= replace_uses(&B->insts[i], rep);
    }
    /* block-local: t = mov s (t defined once, s may change later) */
    for (int i = 0; i <= nv; i++) rep[i] = -1;
    int *live = xalloc(sizeof(int) * (size_t)(nv + 1));
    for (int b = 0; b < f->nblocks; b++) {
        Block *B = &f->blocks[b];
        if (B->dead) continue;
        int nl = 0;
        for (int i = 0; i < B->n; i++) {
            Inst *in = &B->insts[i];
            ch |= replace_uses(in, rep);
            if (in->dst >= 0) {
                for (int k = 0; k < nl; k++) {
                    if (rep[live[k]] == in->dst || live[k] == in->dst) {
                        rep[live[k]] = -1;
                        live[k--] = live[--nl];
                    }
                }
                if (in->op == I_MOV && in->a.k == O_REG && defcnt[in->dst] == 1 && in->a.v != in->dst) {
                    rep[in->dst] = (int)in->a.v;
                    live[nl++] = in->dst;
                }
            }
        }
        for (int k = 0; k < nl; k++) rep[live[k]] = -1;
    }
    free(live);
    free(rep);
    return ch;
}

/* ---------------- extended-basic-block optimizations ----------------
 * Walk chains of blocks where every block after the first has exactly one
 * predecessor. Facts established earlier in the chain stay valid, so we can:
 *   - propagate copies (t = mov v) across block boundaries
 *   - reuse identical pure computations (common subexpression elimination)
 *   - resolve branches whose condition is already known (repeated bounds checks)
 */

typedef struct {
    int op, cls, mem, slot;
    Opd a, b, c;
    int64_t imm;
    const char *sym;
    int res;
} VN;

typedef struct {
    VN *vn;
    int nvn, cvn;
    int *cpt, *cps; /* copy pairs: t is a copy of s */
    int ncp, ccp, ccp2;
    int *kc, *kv; /* known condition values */
    int nk, ck, ck2;
} Env;

static Env env_clone(Env *e) {
    Env n;
    memset(&n, 0, sizeof n);
    for (int i = 0; i < e->nvn; i++) PUSH(n.vn, n.nvn, n.cvn, e->vn[i]);
    for (int i = 0; i < e->ncp; i++) {
        PUSH(n.cpt, n.ncp, n.ccp, e->cpt[i]);
        n.ncp--;
        PUSH(n.cps, n.ncp, n.ccp2, e->cps[i]);
    }
    for (int i = 0; i < e->nk; i++) {
        PUSH(n.kc, n.nk, n.ck, e->kc[i]);
        n.nk--;
        PUSH(n.kv, n.nk, n.ck2, e->kv[i]);
    }
    return n;
}

static void env_free(Env *e) {
    free(e->vn);
    free(e->cpt);
    free(e->cps);
    free(e->kc);
    free(e->kv);
}

static bool cse_ok(Inst *in) {
    switch (in->op) {
    case I_ADD: case I_SUB: case I_MUL: case I_AND: case I_OR: case I_XOR: case I_SMULH: case I_UMULH:
    case I_SHL: case I_ASR: case I_LSR: case I_NEG: case I_NOT:
    case I_FADD: case I_FSUB: case I_FMUL: case I_FDIV: case I_FNEG: case I_FSQRT: case I_FABS:
    case I_CMP: case I_FCMP: case I_EXT: case I_SCVTF: case I_UCVTF: case I_FCVTZS: case I_FCVTZU: case I_FCVT:
    case I_ADDR_SYM: case I_ADDR_SLOT: case I_ICONST:
        return in->dst >= 0;
    default:
        return false;
    }
}

static bool opd_eq(Opd x, Opd y) { return x.k == y.k && (x.k == O_NONE || x.v == y.v); }
static bool opd_is(Opd x, int v) { return x.k == O_REG && x.v == v; }

static void subst_copies(Inst *in, Env *e, bool *ch) {
    Opd *ops[3] = {&in->a, &in->b, &in->c};
    int nops = in->op == I_CALL ? in->nargs : 3;
    for (int k = 0; k < nops; k++) {
        Opd *o = in->op == I_CALL ? &in->args[k] : ops[k];
        if (o->k != O_REG) continue;
        for (int i = e->ncp - 1; i >= 0; i--)
            if (e->cpt[i] == o->v) {
                o->v = e->cps[i];
                *ch = true;
                break;
            }
    }
}

static void invalidate(Env *e, int v) {
    for (int i = 0; i < e->nvn; i++) {
        VN *x = &e->vn[i];
        if (x->res == v || opd_is(x->a, v) || opd_is(x->b, v) || opd_is(x->c, v)) e->vn[i--] = e->vn[--e->nvn];
    }
    for (int i = 0; i < e->ncp; i++)
        if (e->cpt[i] == v || e->cps[i] == v) {
            e->ncp--;
            e->cpt[i] = e->cpt[e->ncp];
            e->cps[i] = e->cps[e->ncp];
            i--;
        }
    for (int i = 0; i < e->nk; i++)
        if (e->kc[i] == v) {
            e->nk--;
            e->kc[i] = e->kc[e->nk];
            e->kv[i] = e->kv[e->nk];
            i--;
        }
}

static void ebb_walk(IRFunc *f, int b, Env *e, const int *preds, bool *ch, int depth) {
    Block *B = &f->blocks[b];
    for (int i = 0; i < B->n; i++) {
        Inst *in = &B->insts[i];
        subst_copies(in, e, ch);
        if (in->op == I_BR && in->a.k == O_REG) {
            for (int k = 0; k < e->nk; k++)
                if (e->kc[k] == in->a.v) {
                    int t = e->kv[k] ? in->t1 : in->t2;
                    in->op = I_JMP;
                    in->t1 = t;
                    in->a.k = O_NONE;
                    *ch = true;
                    break;
                }
        }
        bool matched = false;
        if (cse_ok(in)) {
            for (int k = 0; k < e->nvn; k++) {
                VN *x = &e->vn[k];
                if (x->op == in->op && x->cls == in->cls && x->mem == in->mem && x->slot == in->slot &&
                    x->imm == in->imm && x->sym == in->sym && opd_eq(x->a, in->a) && opd_eq(x->b, in->b) &&
                    opd_eq(x->c, in->c) && x->res != in->dst) {
                    int res = x->res;
                    int dst = in->dst, cls = f->vcls[dst];
                    memset(in, 0, sizeof *in);
                    in->op = I_MOV;
                    in->dst = dst;
                    in->cls = cls;
                    in->a = (Opd){O_REG, res};
                    in->slot = -1;
                    in->t1 = in->t2 = -1;
                    matched = true;
                    *ch = true;
                    break;
                }
            }
        }
        if (in->dst >= 0) {
            invalidate(e, in->dst);
            if (in->op == I_MOV && in->a.k == O_REG && in->a.v != in->dst) {
                PUSH(e->cpt, e->ncp, e->ccp, in->dst);
                e->ncp--;
                PUSH(e->cps, e->ncp, e->ccp2, (int)in->a.v);
            } else if (!matched && cse_ok(in) && !opd_is(in->a, in->dst) && !opd_is(in->b, in->dst) && !opd_is(in->c, in->dst)) {
                VN x = {in->op, in->cls, in->mem, in->slot, in->a, in->b, in->c, in->imm, in->sym, in->dst};
                PUSH(e->vn, e->nvn, e->cvn, x);
            }
        }
    }
    if (depth > 200) return;
    int s[2];
    int ns = block_succs(B, s);
    Inst *t = B->n ? &B->insts[B->n - 1] : NULL;
    for (int k = 0; k < ns; k++) {
        if (preds[s[k]] != 1 || s[k] == b || (k == 1 && s[1] == s[0])) continue;
        Env child = env_clone(e);
        if (t && t->op == I_BR && t->a.k == O_REG && t->t1 != t->t2) {
            PUSH(child.kc, child.nk, child.ck, (int)t->a.v);
            child.nk--;
            PUSH(child.kv, child.nk, child.ck2, s[k] == t->t1);
        }
        ebb_walk(f, s[k], &child, preds, ch, depth + 1);
        env_free(&child);
    }
}

static bool ebb_opt(IRFunc *f) {
    bool ch = false;
    int *preds = xalloc(sizeof(int) * (size_t)(f->nblocks + 1));
    for (int b = 0; b < f->nblocks; b++) {
        if (f->blocks[b].dead) continue;
        int s[2];
        int ns = block_succs(&f->blocks[b], s);
        for (int k = 0; k < ns; k++)
            if (!(k == 1 && s[1] == s[0])) preds[s[k]]++;
    }
    for (int b = 0; b < f->nblocks; b++) {
        if (f->blocks[b].dead || (b != 0 && preds[b] == 1)) continue;
        Env e;
        memset(&e, 0, sizeof e);
        ebb_walk(f, b, &e, preds, &ch, 0);
        env_free(&e);
    }
    free(preds);
    return ch;
}

/* ---------------- dead code ---------------- */

static bool dce(IRFunc *f) {
    bool ch = false, again = true;
    while (again) {
        again = false;
        count(f);
        for (int b = 0; b < f->nblocks; b++) {
            Block *B = &f->blocks[b];
            if (B->dead) continue;
            for (int i = 0; i < B->n; i++) {
                Inst *in = &B->insts[i];
                bool self_mov = in->op == I_MOV && in->a.k == O_REG && in->a.v == in->dst;
                if ((in->dst >= 0 && usecnt[in->dst] == 0 && !inst_has_effects(in)) || self_mov) {
                    if (in->op == I_CALL) continue;
                    nop(in);
                    again = ch = true;
                }
            }
        }
        compact(f);
    }
    return ch;
}

/* ---------------- control flow ---------------- */

static int thread(IRFunc *f, int b) {
    for (int guard = 0; guard < 32; guard++) {
        Block *B = &f->blocks[b];
        if (B->n == 1 && B->insts[0].op == I_JMP && B->insts[0].t1 != b) b = B->insts[0].t1;
        else break;
    }
    return b;
}

static bool simplify_cfg(IRFunc *f) {
    bool ch = false;
    for (int b = 0; b < f->nblocks; b++) {
        Block *B = &f->blocks[b];
        if (B->dead || !B->n) continue;
        Inst *t = &B->insts[B->n - 1];
        if (t->op == I_JMP || t->op == I_BR) {
            int n1 = thread(f, t->t1);
            if (n1 != t->t1) t->t1 = n1, ch = true;
        }
        if (t->op == I_BR) {
            int n2 = thread(f, t->t2);
            if (n2 != t->t2) t->t2 = n2, ch = true;
            if (t->t1 == t->t2) {
                t->op = I_JMP;
                t->a.k = O_NONE;
                ch = true;
            }
        }
    }
    bool *seen = xalloc((size_t)f->nblocks);
    int *stack = xalloc(sizeof(int) * (size_t)(f->nblocks + 1));
    int sp = 0;
    stack[sp++] = 0;
    seen[0] = true;
    while (sp) {
        int b = stack[--sp], s[2];
        int k = block_succs(&f->blocks[b], s);
        for (int i = 0; i < k; i++)
            if (!seen[s[i]]) seen[s[i]] = true, stack[sp++] = s[i];
    }
    for (int b = 0; b < f->nblocks; b++)
        if (!seen[b] && !f->blocks[b].dead) {
            f->blocks[b].dead = true;
            f->blocks[b].n = 0;
            ch = true;
        }
    free(seen);
    free(stack);
    return ch;
}

/* ---------------- coalescing ---------------- */

static void coalesce(IRFunc *f) {
    count(f);
    for (int b = 0; b < f->nblocks; b++) {
        Block *B = &f->blocks[b];
        for (int i = 1; i < B->n; i++) {
            Inst *m = &B->insts[i], *p = &B->insts[i - 1];
            if (m->op != I_MOV || m->a.k != O_REG) continue;
            int s = (int)m->a.v;
            if (p->dst == s && defcnt[s] == 1 && usecnt[s] == 1 && f->vcls[s] == f->vcls[m->dst]) {
                p->dst = m->dst;
                nop(m);
            }
        }
    }
    compact(f);
}

void optimize(Module *m) {
    for (int i = 0; i < m->nfuncs; i++) {
        IRFunc *f = m->funcs[i];
        simplify_cfg(f);
        promote(f);
        for (int iter = 0; iter < 8; iter++) {
            bool ch = false;
            ch |= const_prop(f);
            ch |= simplify_cfg(f);
            ch |= copy_prop(f);
            ch |= ebb_opt(f);
            ch |= dce(f);
            if (!ch) break;
        }
        coalesce(f);
        dce(f);
        simplify_cfg(f);
    }
}
