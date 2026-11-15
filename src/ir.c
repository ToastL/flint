/* ir.c - IR utilities and a human-readable dump (`flint ir file.fl`) */
#include "flint.h"

static const char *OPNAME[I_COUNT] = {
    "nop", "getarg", "iconst", "fconst", "mov",
    "add", "sub", "mul", "sdiv", "udiv", "srem", "urem", "and", "or", "xor",
    "shl", "asr", "lsr", "neg", "not",
    "fadd", "fsub", "fmul", "fdiv", "fneg", "fsqrt", "fabs",
    "cmp", "fcmp", "sel", "ext", "scvtf", "ucvtf", "fcvtzs", "fcvtzu", "fcvt",
    "load", "store", "addrslot", "addrsym", "call", "memcpy", "memzero",
    "jmp", "br", "ret", "trap",
    "smulh", "umulh", "vec", "vreduce",
};
static const char *VOPNAME[] = {"add", "sub", "mul", "div", "and", "or", "xor", "neg", "splat"};
static const char *CCNAME[] = {"eq", "ne", "lt", "le", "gt", "ge", "lo", "ls", "hi", "hs", "mi", "pl"};

int inst_uses(Inst *in, int *out) {
    int n = 0;
    if (in->op == I_CALL) {
        for (int i = 0; i < in->nargs; i++)
            if (in->args[i].k == O_REG) out[n++] = (int)in->args[i].v;
        return n;
    }
    if (in->a.k == O_REG) out[n++] = (int)in->a.v;
    if (in->b.k == O_REG) out[n++] = (int)in->b.v;
    if (in->c.k == O_REG) out[n++] = (int)in->c.v;
    return n;
}

bool inst_is_term(Inst *in) {
    return in->op == I_JMP || in->op == I_BR || in->op == I_RET || in->op == I_TRAP;
}

bool inst_has_effects(Inst *in) {
    switch (in->op) {
    case I_STORE: case I_CALL: case I_MEMCPY: case I_MEMZERO: case I_VEC:
    case I_JMP: case I_BR: case I_RET: case I_TRAP:
        return true;
    default:
        return false;
    }
}

int block_succs(Block *b, int *out) {
    if (!b->n) return 0;
    Inst *t = &b->insts[b->n - 1];
    if (t->op == I_JMP) {
        out[0] = t->t1;
        return 1;
    }
    if (t->op == I_BR) {
        out[0] = t->t1;
        out[1] = t->t2;
        return 2;
    }
    return 0;
}

static void opd(FILE *o, Opd x) {
    if (x.k == O_REG) fprintf(o, "v%lld", (long long)x.v);
    else if (x.k == O_IMM) fprintf(o, "#%lld", (long long)x.v);
}

void dump_ir(Module *m, FILE *o) {
    for (int fi = 0; fi < m->nfuncs; fi++) {
        IRFunc *f = m->funcs[fi];
        fprintf(o, "func %s (vregs %d, slots %d)\n", f->sym, f->nv, f->nslots);
        for (int b = 0; b < f->nblocks; b++) {
            Block *B = &f->blocks[b];
            if (B->dead) continue;
            fprintf(o, " b%d:%s\n", b, B->cold ? " (cold)" : "");
            for (int i = 0; i < B->n; i++) {
                Inst *in = &B->insts[i];
                fprintf(o, "    ");
                if (in->dst >= 0) fprintf(o, "v%d = ", in->dst);
                fprintf(o, "%s", OPNAME[in->op]);
                if (in->cls == C_F32) fprintf(o, ".s");
                if (in->cls == C_F64) fprintf(o, ".d");
                switch (in->op) {
                case I_ICONST: fprintf(o, " %lld", (long long)in->imm); break;
                case I_FCONST: fprintf(o, " %g", in->fimm); break;
                case I_GETARG: fprintf(o, " %lld", (long long)in->imm); break;
                case I_LOAD:
                case I_STORE:
                    fprintf(o, "%d%s [", in->mem & 0x7f, (in->mem & 0x80) ? "s" : "");
                    if (in->slot >= 0) fprintf(o, "slot%d", in->slot);
                    else opd(o, in->a);
                    fprintf(o, " + %lld]", (long long)in->imm);
                    if (in->op == I_STORE) {
                        fprintf(o, ", ");
                        opd(o, in->b);
                    }
                    break;
                case I_ADDR_SLOT: fprintf(o, " slot%d", in->slot); break;
                case I_ADDR_SYM: fprintf(o, " %s%s", in->sym, in->mem ? "@got" : ""); break;
                case I_CALL:
                    if (in->sym) fprintf(o, " %s(", in->sym);
                    else {
                        fprintf(o, " *");
                        opd(o, in->args[in->nargs - 1]);
                        fprintf(o, "(");
                    }
                    for (int k = 0; k < in->nargs - (in->sym ? 0 : 1); k++) {
                        if (k) fprintf(o, ", ");
                        if (k == in->nfixed) fprintf(o, "... ");
                        opd(o, in->args[k]);
                    }
                    fprintf(o, ")");
                    break;
                case I_VEC:
                case I_VREDUCE:
                    if (in->op == I_VEC) fprintf(o, ".%s", VOPNAME[in->mem]);
                    fprintf(o, ".%dx%d%s ", VL_TOTAL(in->imm) / VL_ESIZE(in->imm), VL_ESIZE(in->imm) * 8, VL_FLOAT(in->imm) ? "f" : "");
                    opd(o, in->a);
                    if (in->b.k) { fprintf(o, ", "); opd(o, in->b); }
                    if (in->c.k) { fprintf(o, ", "); opd(o, in->c); }
                    break;
                case I_JMP: fprintf(o, " b%d", in->t1); break;
                case I_BR:
                    fprintf(o, " ");
                    opd(o, in->a);
                    fprintf(o, ", b%d, b%d", in->t1, in->t2);
                    break;
                case I_CMP:
                case I_FCMP:
                    fprintf(o, ".%s ", CCNAME[in->mem]);
                    opd(o, in->a);
                    fprintf(o, ", ");
                    opd(o, in->b);
                    break;
                case I_EXT: fprintf(o, "%d%s ", in->mem & 0x7f, (in->mem & 0x80) ? "s" : "u"); opd(o, in->a); break;
                case I_MEMCPY:
                case I_MEMZERO:
                    fprintf(o, " ");
                    opd(o, in->a);
                    if (in->op == I_MEMCPY) {
                        fprintf(o, ", ");
                        opd(o, in->b);
                    }
                    fprintf(o, ", %lld", (long long)in->imm);
                    break;
                default:
                    if (in->a.k) { fprintf(o, " "); opd(o, in->a); }
                    if (in->b.k) { fprintf(o, ", "); opd(o, in->b); }
                    if (in->c.k) { fprintf(o, ", "); opd(o, in->c); }
                }
                fprintf(o, "\n");
            }
        }
        fprintf(o, "\n");
    }
}
