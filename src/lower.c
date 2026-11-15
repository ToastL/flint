/* lower.c - type checking + lowering the AST to IR.
 *
 * Every local variable starts life in a stack slot; the optimizer later promotes
 * slots whose address never escapes into virtual registers. Aggregates (str,
 * slices, structs) are always handled by address. */
#include "flint.h"
#include <math.h>

/* ====================================================================== */
/* types                                                                  */
/* ====================================================================== */

static Type *ty_void, *ty_bool, *ty_i8, *ty_i16, *ty_i32, *ty_i64, *ty_u8, *ty_u16, *ty_u32,
    *ty_u64, *ty_f32, *ty_f64, *ty_str, *ty_null, *ty_litint, *ty_litfloat, *ty_noreturn,
    *ty_anyerror, *ty_errlit, *ty_usize, *ty_isize, *ty_cstr, *ty_type;

static Type *mkprim(TyKind k, int size, int align) {
    Type *t = xalloc(sizeof *t);
    t->k = k;
    t->size = size;
    t->align = align;
    t->state = 2;
    return t;
}

static Type *slice_of_q(Type *t, bool c);

static void init_types(void) {
    ty_void = mkprim(TY_VOID, 0, 1);
    ty_bool = mkprim(TY_BOOL, 1, 1);
    ty_i8 = mkprim(TY_I8, 1, 1);
    ty_i16 = mkprim(TY_I16, 2, 2);
    ty_i32 = mkprim(TY_I32, 4, 4);
    ty_i64 = mkprim(TY_I64, 8, 8);
    ty_u8 = mkprim(TY_U8, 1, 1);
    ty_u16 = mkprim(TY_U16, 2, 2);
    ty_u32 = mkprim(TY_U32, 4, 4);
    ty_u64 = mkprim(TY_U64, 8, 8);
    ty_f32 = mkprim(TY_F32, 4, 4);
    ty_f64 = mkprim(TY_F64, 8, 8);
    ty_null = mkprim(TY_NULL, 8, 8);
    ty_litint = mkprim(TY_LITINT, 8, 8);
    ty_litfloat = mkprim(TY_LITFLOAT, 8, 8);
    ty_noreturn = mkprim(TY_NORETURN, 0, 1);
    ty_usize = mkprim(TY_U64, 8, 8); /* distinct from u64: pointer-sized, 32 to 64 bits */
    ty_usize->name = "usize";
    ty_isize = mkprim(TY_I64, 8, 8);
    ty_isize->name = "isize";
    ty_cstr = mkprim(TY_PTR, 8, 8); /* [*]const u8 that promises a NUL terminator: for C */
    ty_cstr->elem = ty_u8;
    ty_cstr->many = true;
    ty_cstr->is_const = true;
    ty_cstr->name = "cstr";
    ty_str = slice_of_q(ty_u8, true); /* a string is just []const u8 */
    ty_anyerror = mkprim(TY_ERRSET, 2, 2);
    ty_anyerror->name = "anyerror";
    ty_anyerror->all_errors = true;
    ty_errlit = mkprim(TY_ERRSET, 2, 2); /* error.X before it meets a set */
    ty_errlit->name = "error";
    ty_errlit->all_errors = true;
    ty_type = mkprim(TY_TYPE, 8, 8);
}

static Type **structs;
static int nstructs, cstructs;

/* Compile-time bindings while lowering a generic instance: comptime T: type binds a type, a
 * comptime value parameter binds a constant. Instances of `-> type` functions also bind Self. */
typedef struct {
    const char *name;
    Type *type;     /* a type binding, or NULL for a value */
    Type *vt;       /* a value binding: its type */
    int64_t ival;
    double fval;
    const char *sdata; /* a string value */
    int slen;
    void *var; /* a value binding: the Var that names it */
} Binding;

typedef struct TEnv {
    struct TEnv *parent;
    Binding *b;
    int n, cap;
} TEnv;

static TEnv *TE; /* the bindings in effect (NULL outside generic code) */

static Binding *env_find(const char *name) {
    for (TEnv *e = TE; e; e = e->parent)
        for (int i = e->n - 1; i >= 0; i--)
            if (!strcmp(e->b[i].name, name)) return &e->b[i];
    return NULL;
}

/* ---------- modules: every top-level name belongs to the file (Unit) that declares it ---------- */

static Unit *CU;       /* the module whose code is being looked at: names resolve in it */
static Unit **UNITS;   /* every module of the program */
static int NUNITS;
static bool G_ONLY;    /* looking up a module's member: skip the caller's local variables */

/* the module an import alias names in the current file (`io` for `import std.io;`) */
static Unit *alias_unit(const char *name) {
    if (!CU) return NULL;
    for (int i = 0; i < CU->nimports; i++)
        if (!strcmp(CU->imports[i].alias, name)) return CU->imports[i].u;
    return NULL;
}

static CHeader *alias_header(const char *name) {
    if (!CU) return NULL;
    for (int i = 0; i < CU->nimports; i++)
        if (!strcmp(CU->imports[i].alias, name)) return CU->imports[i].c;
    return NULL;
}

static Type *type_in(Unit *u, const char *name) {
    for (int i = 0; i < nstructs; i++)
        if (structs[i]->unit == u && !strcmp(structs[i]->name, name)) return structs[i];
    return NULL;
}

static Type *c_type_named(CHeader *h, const char *name, Token *tok);

/* a type by name: a comptime binding, a type of this module, or alias.Name from an imported one */
static Type *local_type(const char *name);
static Type *global_type(Unit *u, const char *name);
static Type *find_struct(const char *name) {
    Binding *b = env_find(name);
    if (b && b->type) return b->type;
    Type *t = local_type(name);
    if (t) return t;
    t = type_in(CU, name);
    if (t) return t;
    t = global_type(CU, name);
    if (t) return t;
    const char *dot = strchr(name, '.');
    if (dot) {
        char *first = xstrndup(name, (size_t)(dot - name));
        Unit *u = alias_unit(first);
        if (u) {
            Type *ut = type_in(u, dot + 1);
            return ut ? ut : global_type(u, dot + 1);
        }
        CHeader *h = alias_header(first);
        if (h) return c_type_named(h, dot + 1, NULL);
    }
    return NULL;
}

static const char *unit_name(Unit *u) { return u->is_root ? "the main module" : u->path; }

/* using another module's declaration needs 'pub' */
static void check_pub(Unit *owner, bool is_pub, const char *what, Token *tok) {
    if (is_pub || !owner || owner == CU) return;
    error_at(tok, "'%s' is private to %s; mark it 'pub' to use it from another module", what, unit_name(owner));
}

static Binding *env_find(const char *name);
static void check_type_pub(Type *t, Token *tok) {
    if (t && tok && tok->k == TK_ID) { /* a comptime binding was named by the caller */
        Binding *b = env_find(tok->s);
        if (b && b->type == t) return;
    }
    if (t && (t->k == TY_STRUCT || t->k == TY_ENUM || t->k == TY_ERRSET) && t->unit) check_pub(t->unit, t->is_pub, t->name, tok);
}

static Type *ptr_to(Type *t) {
    if (!t->ptr) {
        t->ptr = mkprim(TY_PTR, 8, 8);
        t->ptr->elem = t;
    }
    return t->ptr;
}

static Type *slice_of(Type *t) {
    if (!t->slice) {
        t->slice = mkprim(TY_SLICE, 16, 8);
        t->slice->elem = t;
    }
    return t->slice;
}

static Type *ptr_to_q(Type *t, bool c) {
    if (!c) return ptr_to(t);
    if (!t->cptr) {
        t->cptr = mkprim(TY_PTR, 8, 8);
        t->cptr->elem = t;
        t->cptr->is_const = true;
    }
    return t->cptr;
}

/* [*]T / [*]const T */
static Type *many_ptr_q(Type *t, bool c) {
    Type **slot = c ? &t->cmptr : &t->mptr;
    if (!*slot) {
        *slot = mkprim(TY_PTR, 8, 8);
        (*slot)->elem = t;
        (*slot)->is_const = c;
        (*slot)->many = true;
    }
    return *slot;
}

static Type *slice_of_q(Type *t, bool c) {
    if (!c) return slice_of(t);
    if (!t->cslice) {
        t->cslice = mkprim(TY_SLICE, 16, 8);
        t->cslice->elem = t;
        t->cslice->is_const = true;
    }
    return t->cslice;
}

/* ?T: a pointer-sized value (null = 0) for ?*T, otherwise { value, present } */
static Type *opt_of(Type *t) {
    if (!t->opt) {
        Type *o = mkprim(TY_OPT, 8, 8);
        o->elem = t;
        if (t->k != TY_PTR && t->k != TY_FN) o->state = 0; /* laid out on demand */
        t->opt = o;
    }
    return t->opt;
}
/* ?*T, ?[*]T and ?fn(...): null is address 0, so the optional is just the pointer */
static bool is_optptr(Type *t) { return t->k == TY_OPT && (t->elem->k == TY_PTR || t->elem->k == TY_FN); }

/* E!T: just the u16 error code for !void, otherwise { value, err: u16 } */
static Type *err_union(Type *set, Type *t) {
    for (Type *u = t->eus; u; u = u->eu_next)
        if (u->eset == set) return u;
    Type *u = mkprim(TY_ERRUNION, 2, 2);
    u->elem = t;
    u->eset = set;
    if (t->k != TY_VOID) u->state = 0; /* laid out on demand */
    u->eu_next = t->eus;
    t->eus = u;
    return u;
}
static int err_off(Type *eu) { return (eu->elem->size + 1) & ~1; }

static bool is_int(Type *t) { return (t->k >= TY_I8 && t->k <= TY_U64) || t->k == TY_LITINT; }
static bool is_float(Type *t) { return t->k == TY_F32 || t->k == TY_F64 || t->k == TY_LITFLOAT; }
static bool is_num(Type *t) { return is_int(t) || is_float(t); }
static bool is_signed(Type *t) { return (t->k >= TY_I8 && t->k <= TY_I64) || t->k == TY_LITINT; }
static bool is_lit(Type *t) { return t->k == TY_LITINT || t->k == TY_LITFLOAT; }
static bool is_ptr(Type *t) { return t->k == TY_PTR; }
static bool is_slice(Type *t) { return t->k == TY_SLICE; }
static bool is_many(Type *t) { return t->k == TY_PTR && t->many; }
static bool is_fn(Type *t) { return t->k == TY_FN; }
static bool is_array(Type *t) { return t->k == TY_ARRAY || t->k == TY_VECTOR; } /* a vector indexes like an array */
static bool is_vector(Type *t) { return t->k == TY_VECTOR; }
static bool is_array_ptr(Type *t) { return t->k == TY_PTR && !t->many && t->elem->k == TY_ARRAY; }
static bool is_agg(Type *t) {
    return t->k == TY_SLICE || t->k == TY_STRUCT || t->k == TY_ARRAY || t->k == TY_VECTOR || (t->k == TY_ENUM && t->has_payload) ||
           (t->k == TY_OPT && t->elem->k != TY_PTR && t->elem->k != TY_FN) || (t->k == TY_ERRUNION && t->elem->k != TY_VOID);
}
static bool is_bytes(Type *t) { return t->k == TY_SLICE && t->elem->k == TY_U8; } /* []u8 or []const u8: a string */
static bool is_plain_enum(Type *t) { return t->k == TY_ENUM && !t->has_payload; }

static const char *tname(Type *t) {
    if (t == ty_usize || t == ty_isize || t == ty_cstr) return t->name;
    switch (t->k) {
    case TY_VOID: return "void";
    case TY_BOOL: return "bool";
    case TY_I8: return "i8";
    case TY_I16: return "i16";
    case TY_I32: return "i32";
    case TY_I64: return "i64";
    case TY_U8: return "u8";
    case TY_U16: return "u16";
    case TY_U32: return "u32";
    case TY_U64: return "u64";
    case TY_F32: return "f32";
    case TY_F64: return "f64";
    case TY_PTR: return strf("%s%s%s", t->many ? "[*]" : "*", t->is_const ? "const " : "", tname(t->elem));
    case TY_SLICE: return strf("[]%s%s", t->is_const ? "const " : "", tname(t->elem));
    case TY_STRUCT: return t->dname ? t->dname : t->name;
    case TY_ENUM: return t->dname ? t->dname : t->name;
    case TY_OPT: return strf("?%s", tname(t->elem));
    case TY_NORETURN: return "noreturn";
    case TY_ERRSET: return t->dname ? t->dname : t->name;
    case TY_ERRUNION:
        return t->eset->all_errors ? strf("!%s", tname(t->elem)) : strf("%s!%s", tname(t->eset), tname(t->elem));
    case TY_ARRAY: return strf("[%lld]%s", (long long)t->alen, tname(t->elem));
    case TY_FN: {
        const char *s = "fn(";
        for (int i = 0; i < t->nparams; i++) s = strf("%s%s%s", s, i ? ", " : "", tname(t->params[i]));
        if (t->variadic) s = strf("%s%s...", s, t->nparams ? ", " : "");
        s = strf("%s)", s);
        return t->elem == ty_void ? s : strf("%s -> %s", s, tname(t->elem));
    }
    case TY_NULL: return "null";
    case TY_LITINT: return "integer literal";
    case TY_LITFLOAT: return "float literal";
    case TY_TYPE: return "type";
    case TY_VECTOR: return strf("@Vector(%lld, %s)", (long long)t->alen, tname(t->elem));
    }
    return "?";
}

/* types as comptime values: a type's id indexes this table */
static Type **type_tab;
static int ntype_tab, ctype_tab;
static int64_t type_id(Type *t) {
    if (!t->tid) {
        PUSH(type_tab, ntype_tab, ctype_tab, t);
        t->tid = ntype_tab;
    }
    return t->tid;
}
static Type *type_from_id(int64_t id) { return id >= 1 && id <= ntype_tab ? type_tab[id - 1] : NULL; }

static Type *default_t(Type *t) {
    if (t->k == TY_LITINT) return ty_i64;
    if (t->k == TY_LITFLOAT) return ty_f64;
    return t;
}

static int cls_of(Type *t) {
    if (t->k == TY_F32) return C_F32;
    if (t->k == TY_F64 || t->k == TY_LITFLOAT) return C_F64;
    return C_I;
}

static int memcode(Type *t) {
    if (t->k == TY_ENUM) return memcode(t->tagt);
    if (is_int(t)) return default_t(t)->size | (is_signed(t) ? 0x80 : 0);
    return t->size;
}

static void layout(Type *t, Token *tok);

/* tagged union: { tag, payload } with the payload at the largest payload alignment */
static void layout_enum(Type *t, Token *tok) {
    if (t->state == 2) return;
    if (t->state == 1) error_at(tok, "enum '%s' contains itself by value (use a pointer)", t->name);
    t->state = 1;
    int al = t->tagt->align, pal = 1, maxsz = 0;
    for (int i = 0; i < t->nvariants; i++) {
        Type *p = t->variants[i].payload;
        if (!p) continue;
        layout(p, t->variants[i].tok);
        if (p->align > pal) pal = p->align;
        if (p->size > maxsz) maxsz = p->size;
    }
    if (!t->has_payload) {
        t->size = t->tagt->size;
        t->align = t->tagt->align;
    } else {
        if (pal > al) al = pal;
        t->payload_off = (t->tagt->size + pal - 1) / pal * pal;
        t->size = (t->payload_off + maxsz + al - 1) / al * al;
        t->align = al;
    }
    t->state = 2;
}

static void layout(Type *t, Token *tok) {
    if (t->opaque) error_at(tok, "%s can only be used behind a pointer (%s)", tname(t), t->opaque_why);
    if (t->k == TY_ENUM) {
        layout_enum(t, tok);
        return;
    }
    if (t->k == TY_ARRAY) {
        if (t->state == 2) return;
        layout(t->elem, tok);
        if (t->elem->size && t->alen > (int64_t)(0x7fffffff / t->elem->size)) error_at(tok, "%s is too big", tname(t));
        t->size = (int)(t->alen * t->elem->size);
        t->align = t->elem->align;
        t->state = 2;
        return;
    }
    if (t->k == TY_ERRUNION) { /* { value, err } */
        if (t->state == 2) return;
        layout(t->elem, tok);
        int al = t->elem->align > 2 ? t->elem->align : 2;
        t->size = (err_off(t) + 2 + al - 1) / al * al;
        t->align = al;
        t->state = 2;
        return;
    }
    if (t->k == TY_OPT) { /* { value, present } */
        if (t->state == 2) return;
        layout(t->elem, tok);
        int al = t->elem->align ? t->elem->align : 1;
        t->size = (t->elem->size + 1 + al - 1) / al * al;
        t->align = al;
        t->state = 2;
        return;
    }
    if (t->k != TY_STRUCT || t->state == 2) return;
    if (t->state == 1)
        error_at(tok, "struct '%s' contains itself by value (use a pointer: *%s)", t->name, t->name);
    t->state = 1;
    if (t->packed) { /* fields at bit offsets, first field in the lowest bits (like C bitfields) */
        int off = 0;
        for (int i = 0; i < t->nfields; i++) {
            Field *f = &t->fields[i];
            layout(f->t, f->tok);
            if (!f->bits) {
                Type *ft = f->t;
                if (ft == ty_bool) f->bits = 1;
                else if (is_int(ft)) f->bits = ft->size * 8, f->sbits = is_signed(ft);
                else if (ft->k == TY_ENUM && !ft->has_payload) f->bits = ft->tagt->size * 8, f->sbits = is_signed(ft->tagt);
                else if (ft->k == TY_STRUCT && ft->packed) f->bits = ft->pbits;
                else
                    error_at(f->tok, "a packed struct field must be an integer, bool, plain enum or packed struct, not %s",
                             tname(ft));
            }
            f->off = 0;
            f->bitoff = off;
            off += f->bits;
            if (off > 64) error_at(f->tok, "packed struct '%s' needs more than 64 bits (it can't be bigger than u64)", t->name);
        }
        t->pbits = off;
        t->tagt = off <= 8 ? ty_u8 : off <= 16 ? ty_u16 : off <= 32 ? ty_u32 : ty_u64;
        t->size = t->align = t->tagt->size;
        t->state = 2;
        return;
    }
    if (t->is_union) { /* every field at offset 0 */
        int sz = 0, al = 1;
        for (int i = 0; i < t->nfields; i++) {
            Field *f = &t->fields[i];
            layout(f->t, f->tok);
            f->off = 0;
            if (f->t->size > sz) sz = f->t->size;
            if (f->t->align > al) al = f->t->align;
        }
        t->size = sz ? (sz + al - 1) / al * al : 1;
        t->align = al;
        t->state = 2;
        return;
    }
    int off = 0, al = 1;
    for (int i = 0; i < t->nfields; i++) {
        Field *f = &t->fields[i];
        layout(f->t, f->tok);
        int a = f->t->align;
        off = (off + a - 1) / a * a;
        f->off = off;
        off += f->t->size;
        if (a > al) al = a;
    }
    if (off == 0) off = 1;
    t->size = (off + al - 1) / al * al;
    t->align = al;
    t->state = 2;
}

/* function types are interned, so two equal signatures are the same Type */
static Type **fntys;
static int nfntys, cfntys;

static Type *fn_type(Type **pt, int np, Type *ret, bool variadic) {
    for (int i = 0; i < nfntys; i++) {
        Type *t = fntys[i];
        if (t->nparams != np || t->elem != ret || t->variadic != variadic) continue;
        int k = 0;
        while (k < np && t->params[k] == pt[k]) k++;
        if (k == np) return t;
    }
    Type *t = mkprim(TY_FN, 8, 8);
    t->params = xalloc(sizeof(Type *) * (size_t)(np + 1));
    if (np) memcpy(t->params, pt, sizeof(Type *) * (size_t)np);
    t->nparams = np;
    t->elem = ret;
    t->variadic = variadic;
    PUSH(fntys, nfntys, cfntys, t);
    return t;
}

static Type **arrtys; /* interned [N]T types */
static int narrtys, carrtys;

static Type *array_of(Type *e, int64_t n) {
    for (int i = 0; i < narrtys; i++)
        if (arrtys[i]->elem == e && arrtys[i]->alen == n) return arrtys[i];
    Type *t = mkprim(TY_ARRAY, 0, 1);
    t->elem = e;
    t->alen = n;
    t->state = 0; /* laid out on demand: the element may not be laid out yet */
    PUSH(arrtys, narrtys, carrtys, t);
    return t;
}

static Type **vectys; /* interned @Vector(N, T) types */
static int nvectys, cvectys;

static Type *vector_of(Type *e, int64_t n) {
    for (int i = 0; i < nvectys; i++)
        if (vectys[i]->elem == e && vectys[i]->alen == n) return vectys[i];
    int total = (int)(n * e->size);
    Type *t = mkprim(TY_VECTOR, total, total);
    t->elem = e;
    t->alen = n;
    PUSH(vectys, nvectys, cvectys, t);
    return t;
}

static int64_t eval_const_int(Node *e, const char *what);

static Type **tuptys; /* interned tuple types */
static int ntuptys, ctuptys;

static Type *tuple_of(Type **ts, int n) {
    for (int i = 0; i < ntuptys; i++) {
        Type *t = tuptys[i];
        if (t->nfields != n) continue;
        int k = 0;
        while (k < n && t->fields[k].t == ts[k]) k++;
        if (k == n) return t;
    }
    Type *t = xalloc(sizeof *t);
    t->k = TY_STRUCT;
    t->tuple = true;
    t->fields = xalloc(sizeof(Field) * (size_t)n);
    const char *name = "(";
    for (int i = 0; i < n; i++) {
        t->fields[i].name = strf("%d", i);
        t->fields[i].t = ts[i];
        name = strf("%s%s%s", name, i ? ", " : "", tname(ts[i]));
    }
    t->nfields = n;
    t->name = strf("%s)", name);
    PUSH(tuptys, ntuptys, ctuptys, t);
    return t;
}

/* u3, i12, ...: a width that isn't 8/16/32/64, allowed only in packed structs. Returns the width or 0. */
static int bitwidth_name(const char *s, bool *sg) {
    if ((s[0] != 'u' && s[0] != 'i') || !s[1]) return 0;
    int w = 0;
    for (const char *c = s + 1; *c; c++) {
        if (*c < '0' || *c > '9' || w > 64) return 0;
        w = w * 10 + (*c - '0');
    }
    if (s[1] == '0' || w < 1 || w > 64 || w == 8 || w == 16 || w == 32 || w == 64) return 0;
    *sg = s[0] == 'i';
    return w;
}

static Type *type_of_expr(Node *e);
static Type *generic_type(Node *n);

static Type *prim_type(const char *name);
static Type *resolve_in(Node *n);
static Type *resolve(Node *n) { /* a type node names things in the module it was written in */
    Unit *su = CU;
    if (n->unit) CU = n->unit;
    Type *t = resolve_in(n);
    CU = su;
    return t;
}

static Type *prim_type(const char *name) {
    static const struct { const char *name; Type **t; } prims[] = {
        {"void", &ty_void}, {"bool", &ty_bool}, {"i8", &ty_i8}, {"i16", &ty_i16},
        {"i32", &ty_i32}, {"i64", &ty_i64}, {"u8", &ty_u8}, {"u16", &ty_u16},
        {"u32", &ty_u32}, {"u64", &ty_u64}, {"f32", &ty_f32}, {"f64", &ty_f64},
        {"str", &ty_str}, {"anyerror", &ty_anyerror}, {"usize", &ty_usize}, {"isize", &ty_isize}, {"cstr", &ty_cstr},
        {"noreturn", &ty_noreturn},
    };
    for (size_t i = 0; i < sizeof prims / sizeof prims[0]; i++)
        if (!strcmp(name, prims[i].name)) return *prims[i].t;
    return NULL;
}

static Type *resolve_in(Node *n) {
    if (n->k == N_TYPEEXPR) return resolve(n->a);
    if (n->k == N_TTYPEOF) return type_of_expr(n->a);
    if (n->k == N_TGENERIC) return generic_type(n);
    if (n->k == N_TVECTOR) { /* @Vector(4, f32): one SIMD register */
        int64_t len = eval_const_int(n->c, "a vector length");
        Type *e = resolve(n->a);
        bool ok = (is_int(e) && !is_lit(e)) || e->k == TY_F32 || e->k == TY_F64;
        if (!ok) error_at(n->a->tok, "a vector's elements must be integers or floats, not %s", tname(e));
        if (len < 2 || (len * e->size != 8 && len * e->size != 16))
            error_at(n->tok, "@Vector(%lld, %s) would be %lld bytes; a vector is one SIMD register: 8 or 16 bytes with at least "
                             "2 elements (split it, or use an array)", (long long)len, tname(e), (long long)(len * e->size));
        return vector_of(e, len);
    }
    if (n->k != N_TNAME && n->k != N_TPTR && n->k != N_TERRU && n->k != N_TARRAY && n->k != N_TTUPLE && n->k != N_TFN &&
        n->k != N_TOPT && n->k != N_TSLICE)
        error_at(n->tok, "expected a type here");
    if (n->k == N_TPTR) {
        Type *e = resolve(n->a);
        if (n->flag2) {
            if (e == ty_void) error_at(n->tok, "[*]void has no item size; use *void");
            return many_ptr_q(e, n->flag);
        }
        return ptr_to_q(e, n->flag);
    }
    if (n->k == N_TERRU) {
        Type *set = n->a ? resolve(n->a) : ty_anyerror;
        if (set->k != TY_ERRSET) error_at(n->a->tok, "'%s' isn't an error set, so it can't go before '!'", tname(set));
        Type *e = resolve(n->b);
        if (e->k == TY_ERRUNION) error_at(n->tok, "nested error unions aren't supported");
        return err_union(set, e);
    }
    if (n->k == N_TARRAY) {
        if (!n->c) error_at(n->tok, "'[_]' infers the length from an array literal, so it can't be used here; write the length");
        int64_t len = eval_const_int(n->c, "an array length");
        if (len < 0) error_at(n->c->tok, "an array length can't be negative (%lld)", (long long)len);
        Type *e = resolve(n->a);
        if (e == ty_void || e == ty_noreturn) error_at(n->a->tok, "can't have an array of %s", tname(e));
        return array_of(e, len);
    }
    if (n->k == N_TTUPLE) {
        Type **ts = xalloc(sizeof(Type *) * (size_t)n->n);
        for (int i = 0; i < n->n; i++) {
            ts[i] = resolve(n->list[i]);
            if (ts[i] == ty_void || ts[i] == ty_noreturn) error_at(n->list[i]->tok, "a tuple can't hold %s", tname(ts[i]));
        }
        return tuple_of(ts, n->n);
    }
    if (n->k == N_TFN) {
        Type **pt = xalloc(sizeof(Type *) * (size_t)(n->n + 1));
        for (int i = 0; i < n->n; i++) {
            pt[i] = resolve(n->list[i]);
            if (pt[i] == ty_void) error_at(n->list[i]->tok, "a parameter can't be void");
        }
        return fn_type(pt, n->n, n->b ? resolve(n->b) : ty_void, n->flag == 1);
    }
    if (n->k == N_TOPT) {
        Type *e = resolve(n->a);
        if (e->k == TY_OPT) error_at(n->tok, "nested optionals (??T) aren't supported");
        if (e == ty_void) error_at(n->tok, "?void isn't a useful type; use bool");
        return opt_of(e);
    }
    if (n->k == N_TSLICE) {
        Type *e = resolve(n->a);
        if (e == ty_void) error_at(n->tok, "can't have a slice of void");
        return slice_of_q(e, n->flag);
    }
    Type *pt = prim_type(n->name);
    if (pt) return pt;
    const char *cdot = strchr(n->name, '.');
    if (cdot) { /* c.FILE: with a location for errors */
        CHeader *h = alias_header(xstrndup(n->name, (size_t)(cdot - n->name)));
        if (h) return c_type_named(h, cdot + 1, n->tok);
    }
    Type *s = find_struct(n->name);
    if (s) {
        check_type_pub(s, n->tok);
        return s;
    }
    if (strchr(n->name, '.')) {
        char *first = xstrndup(n->name, (size_t)(strchr(n->name, '.') - n->name));
        Unit *u = alias_unit(first);
        if (u) error_at(n->tok, "%s has no type '%s'", u->path, strchr(n->name, '.') + 1);
        if (!alias_header(first) && std_has(first))
            error_at(n->tok, "unknown type '%s' ('%s' is a module of the standard library: add 'import std.%s;')", n->name, first, first);
    }
    if (!strcmp(n->name, "type") || !strcmp(n->name, "anytype"))
        error_at(n->tok, "'%s' can only be the type of a function parameter (comptime T: type, x: anytype)", n->name);
    Binding *vb = env_find(n->name);
    if (vb && !vb->type) error_at(n->tok, "'%s' is a compile-time value, not a type", n->name);
    bool sg;
    if (bitwidth_name(n->name, &sg))
        error_at(n->tok, "'%s' can only be the type of a packed struct field (bit fields); use a full-width integer here", n->name);
    if (!strcmp(n->name, "int") || !strcmp(n->name, "float") || !strcmp(n->name, "byte"))
        error_at(n->tok, "unknown type '%s': Flint only has fixed-width types (i32, i64, u8, f64, ...)", n->name);
    error_at(n->tok, "unknown type '%s'", n->name);
}

typedef struct {
    int off;
    Type *t;
} Leaf;

/* Flatten an aggregate into its scalar pieces. Returns count, or -1 if more than max. */
static int leaves(Type *t, int base, Leaf *out, int n, int max) {
    if (n < 0) return -1;
    switch (t->k) {
    case TY_SLICE:
        if (n + 2 > max) return -1;
        out[n++] = (Leaf){base, ptr_to(t->elem)};
        out[n++] = (Leaf){base + 8, ty_usize};
        return n;
    case TY_VECTOR: /* its bytes, as 64-bit pieces */
        for (int off = 0; off < t->size; off += 8) {
            if (n + 1 > max) return -1;
            out[n++] = (Leaf){base + off, ty_u64};
        }
        return n;
    case TY_ARRAY:
        layout(t, NULL);
        if (t->alen > max) return -1;
        for (int64_t i = 0; i < t->alen && n >= 0; i++) n = leaves(t->elem, base + (int)i * t->elem->size, out, n, max);
        return n;
    case TY_STRUCT:
        layout(t, NULL);
        if (t->packed) { /* one integer */
            if (n + 1 > max) return -1;
            out[n++] = (Leaf){base, t->tagt};
            return n;
        }
        if (t->is_union) { /* overlapping fields: copy the bytes */
            for (int off = 0; off < t->size;) {
                int rem = t->size - off;
                Type *ct = rem >= 8 ? ty_u64 : rem >= 4 ? ty_u32 : rem >= 2 ? ty_u16 : ty_u8;
                if (n + 1 > max) return -1;
                out[n++] = (Leaf){base + off, ct};
                off += ct->size;
            }
            return n;
        }
        for (int i = 0; i < t->nfields && n >= 0; i++)
            n = leaves(t->fields[i].t, base + t->fields[i].off, out, n, max);
        return n;
    case TY_ERRUNION:
        if (t->elem == ty_void) break;
        layout(t, NULL);
        n = leaves(t->elem, base, out, n, max);
        if (n < 0 || n + 1 > max) return -1;
        out[n++] = (Leaf){base + err_off(t), ty_u16};
        return n;
    case TY_OPT:
        if (is_optptr(t)) break;
        layout(t, NULL);
        n = leaves(t->elem, base, out, n, max);
        if (n < 0 || n + 1 > max) return -1;
        out[n++] = (Leaf){base + t->elem->size, ty_bool};
        return n;
    case TY_ENUM:
        if (!t->has_payload) break;
        layout(t, NULL);
        for (int off = 0; off < t->size;) { /* a union: copy its bytes, not its fields */
            int rem = t->size - off;
            Type *ct = rem >= 8 ? ty_u64 : rem >= 4 ? ty_u32 : rem >= 2 ? ty_u16 : ty_u8;
            if (n + 1 > max) return -1;
            out[n++] = (Leaf){base + off, ct};
            off += ct->size;
        }
        return n;
    default:
        break;
    }
    if (n + 1 > max) return -1;
    out[n++] = (Leaf){base, t};
    return n;
}

/* ====================================================================== */
/* functions, variables, scopes                                           */
/* ====================================================================== */

typedef struct GenInst GenInst;

typedef struct FnInfo {
    const char *name;
    Node *decl;
    Type *recv;
    Type **pt;
    const char **pn;
    Token **ptok;
    int np;
    Type *ret; /* NULL while being inferred */
    bool ext, variadic;
    int self_kind;
    const char *sym;
    int state; /* 0 = todo, 1 = lowering, 2 = done */
    bool lib;  /* from the standard library: lowered only if used */
    bool lazy; /* a method of a generic type's instance: lowered only if used */
    bool generic; /* has comptime or anytype parameters: each call site picks an instance */
    bool type_fn; /* returns `type`: called at compile time to make a type */
    bool ct_fn;   /* an instance of a type function: it only ever runs at compile time */
    TEnv *tenv;   /* the bindings its body sees (an instance, or a method of a generic type) */
    Unit *unit;   /* the module that declares it */
    bool is_pub;
    GenInst *insts; /* instances made so far (generic functions) */
    int ninsts, cinsts;
} FnInfo;

/* one instance of a generic function: the comptime arguments (and anytype argument types) it was made for */
typedef struct {
    Type *t;   /* a type argument, or an anytype argument's type */
    Type *vt;  /* a value argument's type */
    int64_t ival;
    double fval;
    const char *sdata;
    int slen;
} KeyPart;

struct GenInst {
    KeyPart *key;
    int nkey;
    FnInfo *fn;   /* the instance (generic functions) */
    Type *type;   /* the type it made (`-> type` functions) */
};

enum { VK_SLOT, VK_REF, VK_GLOBAL, VK_CONST, VK_CSTR, VK_LAZY, VK_TYPE }; /* VK_TYPE: let T = u8; (t is the type) */

typedef struct {
    const char *name;
    Type *t;
    int kind;
    int slot, reg;
    const char *sym;
    bool is_const;
    int64_t cival;
    double cfval;
    bool ext; /* VK_GLOBAL defined in C: reached through the GOT */
    bool param_copy; /* VK_REF: an aggregate parameter (the callee's own copy) */
    const char *sdata; /* VK_CSTR: a compile-time string (a comptime []const u8 parameter) */
    int slen;
    Token *origin; /* VK_CSTR: where the string was written (errors about a format string point there) */
    Node *lazy;      /* VK_LAZY: a global, set up the first time it's used (or at the end) */
    Type *lazy_owner;
    void *lazy_te;   /* VK_LAZY: the comptime bindings its initializer sees (a generic type's let) */
    bool busy;       /* VK_LAZY: being set up now (so a use means it depends on itself) */
    Unit *unit;      /* globals: the module that declares it */
    bool is_pub;
} Var;

typedef struct Scope {
    struct Scope *parent;
    Var *vars;
    int n, cap;
    Node **defers;
    int nd, dcap;
    bool loop;
    int brk, cont;
    const char *label;
    int unsafe_depth; /* defers run with the unsafe-ness of the scope they were declared in */
} Scope;

typedef struct Ctx {
    IRFunc *f;
    FnInfo *fn;
    int blk;
    Scope *cur;
    int retptr;
    int seq;
    int unsafe_depth; /* inside `unsafe { }` or an `unsafe fn` body */
    int defer_depth;  /* lowering a defer's body: it can't leave the function */
    bool ct_scratch;  /* a comptime expression or block: it runs once, now, and can't return */
    struct Ctx *outer; /* ... and the function it was written in (its constants are visible) */
} Ctx;

static Ctx *C;
static Module *M;
static int MODE;
static bool in_unsafe(void) { return C && C->unsafe_depth > 0; }
/* overflow checks: Debug and ReleaseSafe, outside unsafe blocks */
static bool safe_arith(void) { return MODE != MODE_FAST && !in_unsafe(); }
/* bounds checks: every mode, outside unsafe blocks */
static bool bounds_checked(void) { return !in_unsafe(); }
static void require_unsafe(Token *t, const char *what) {
    if (!in_unsafe()) error_at(t, "%s is only allowed inside an 'unsafe' block", what);
}
static FnInfo **fns;
static int nfns, cfns;
static Var *gvars;
static int ngvars, cgvars;

static const char *BUILTINS[] = {"exit", "assert", "panic", "now", "args",
                                 "min", "max", "abs", "sqrt", "sin", "cos", "tan", "exp",
                                 "log", "floor", "ceil", "round", "pow", "atan2", NULL};

static bool is_builtin(const char *s) {
    for (const char **b = BUILTINS; *b; b++)
        if (!strcmp(*b, s)) return true;
    return false;
}


static FnInfo *fn_in(Unit *u, const char *name) {
    for (int i = 0; i < nfns; i++)
        if (!fns[i]->recv && fns[i]->unit == u && !strcmp(fns[i]->name, name)) return fns[i];
    return NULL;
}

static FnInfo *find_fn(const char *name) { return fn_in(CU, name); }

static FnInfo *find_method(Type *recv, const char *name) {
    for (int i = 0; i < nfns; i++)
        if (fns[i]->recv == recv && !strcmp(fns[i]->name, name)) return fns[i];
    return NULL;
}

static Var *materialize(int i);

static Var *global_in(Unit *u, const char *name) {
    for (int i = 0; i < ngvars; i++)
        if (gvars[i].unit == u && !strcmp(gvars[i].name, name)) return gvars[i].kind == VK_LAZY ? materialize(i) : &gvars[i];
    return NULL;
}

static Token *CUR_TOK;

static Var *find_var(const char *name) {
    if (C && !G_ONLY)
        for (Scope *s = C->cur; s; s = s->parent)
            for (int i = s->n - 1; i >= 0; i--)
                if (!strcmp(s->vars[i].name, name)) return &s->vars[i];
    if (C && !G_ONLY) /* comptime code sees the constants of the function around it, not its variables */
        for (Ctx *o = C->ct_scratch ? C->outer : NULL; o; o = o->ct_scratch ? o->outer : NULL)
            for (Scope *s = o->cur; s; s = s->parent)
                for (int i = s->n - 1; i >= 0; i--) {
                    if (strcmp(s->vars[i].name, name)) continue;
                    if (s->vars[i].kind == VK_CONST || s->vars[i].kind == VK_CSTR || s->vars[i].kind == VK_TYPE) return &s->vars[i];
                    error_at(CUR_TOK, "'%s' is a runtime variable, so comptime code can't use it", name);
                }
    Binding *b = G_ONLY ? NULL : env_find(name);
    if (b && !b->type) return (Var *)b->var;
    return global_in(CU, name);
}

/* let T = List(i32); inside a function (or a comptime value around comptime code) */
static Type *local_type(const char *name) {
    for (Ctx *o = C; o && !G_ONLY; o = o->ct_scratch ? o->outer : NULL)
        for (Scope *s = o->cur; s; s = s->parent)
            for (int i = s->n - 1; i >= 0; i--)
                if (!strcmp(s->vars[i].name, name)) return s->vars[i].kind == VK_TYPE ? s->vars[i].t : NULL;
    return NULL;
}

/* let IntList = List(i32); at the top level of module u (set up on first use) */
static Type *global_type(Unit *u, const char *name) {
    if (u != CU) { /* another module's: only if it's pub */
        for (int i = 0; i < ngvars; i++)
            if (gvars[i].unit == u && !strcmp(gvars[i].name, name) && !gvars[i].is_pub) return NULL;
    }
    for (int i = 0; i < ngvars; i++) {
        if (gvars[i].unit != u || strcmp(gvars[i].name, name)) continue;
        if (gvars[i].kind == VK_LAZY && !gvars[i].busy) {
            Var *v = materialize(i);
            return v && v->kind == VK_TYPE ? v->t : NULL;
        }
        if (gvars[i].kind == VK_TYPE) return gvars[i].t;
    }
    return NULL;
}

/* is this name a local variable (or a comptime value) right here? Those hide imports. */
static bool is_local(const char *name) {
    for (Ctx *o = C; o; o = o->ct_scratch ? o->outer : NULL)
        for (Scope *s = o->cur; s; s = s->parent)
            for (int i = s->n - 1; i >= 0; i--)
                if (!strcmp(s->vars[i].name, name)) return true;
    Binding *b = env_find(name);
    return b != NULL;
}

/* `io` in `io.print`: the module an import alias names (unless a local hides it) */
static Unit *unit_ref(Node *n) {
    if (n->k != N_NAME || is_local(n->name)) return NULL;
    Unit *su = CU;
    if (n->unit) CU = n->unit;
    Unit *u = alias_unit(n->name);
    CU = su;
    return u;
}

static CHeader *header_ref(Node *n) {
    if (n->k != N_NAME || is_local(n->name)) return NULL;
    Unit *su = CU;
    if (n->unit) CU = n->unit;
    CHeader *h = alias_header(n->name);
    CU = su;
    return h;
}

static Var *lookup(const char *name, Token *tok) {
    Var *v = find_var(name);
    if (v) return v;
    if (find_fn(name) || is_builtin(name)) error_at(tok, "'%s' is a function; call it with ()", name);
    if (find_struct(name)) error_at(tok, "'%s' is a type, not a value", name);
    if (alias_unit(name) || alias_header(name)) error_at(tok, "'%s' is a module; use one of its members: %s.name", name, name);
    if (std_has(name)) error_at(tok, "'%s' is not defined (it's a module of the standard library: add 'import std.%s;')", name, name);
    error_at(tok, "'%s' is not defined", name);
}

static void push_scope(bool loop, int brk, int cont) {
    Scope *s = xalloc(sizeof *s);
    s->parent = C->cur;
    s->loop = loop;
    s->brk = brk;
    s->cont = cont;
    s->unsafe_depth = C->unsafe_depth;
    C->cur = s;
}

static void pop_scope(void) { C->cur = C->cur->parent; }

static Var *declare(const char *name, Type *t, Token *tok, bool is_const) {
    Scope *s = C->cur;
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->vars[i].name, name)) error_at(tok, "'%s' is already declared in this scope", name);
    if (t == ty_void || t == ty_null || is_lit(t))
        error_at(tok, "can't declare '%s' with type %s", name, tname(t));
    if (t == ty_type) error_at(tok, "'%s' would hold a type, which only exists at compile time: use 'let %s = ...;'", name, name);
    Var v;
    memset(&v, 0, sizeof v);
    v.name = name;
    v.t = t;
    v.is_const = is_const;
    v.slot = v.reg = -1;
    PUSH(s->vars, s->n, s->cap, v);
    return &s->vars[s->n - 1];
}

/* ====================================================================== */
/* IR building                                                            */
/* ====================================================================== */

int new_vreg_in(IRFunc *f, int cls) {
    PUSH(f->vcls, f->nv, f->vcap, cls);
    return f->nv - 1;
}

static int new_vreg(int cls) { return new_vreg_in(C->f, cls); }

static int new_block(void) {
    Block b;
    memset(&b, 0, sizeof b);
    b.seq = -1;
    PUSH(C->f->blocks, C->f->nblocks, C->f->bcap, b);
    return C->f->nblocks - 1;
}

static int new_cold_block(void) {
    int b = new_block();
    C->f->blocks[b].cold = true;
    return b;
}

static void set_blk(int b) {
    C->blk = b;
    if (C->f->blocks[b].seq < 0) C->f->blocks[b].seq = C->seq++;
}

static int new_slot(Type *t) {
    Slot s = {t->size ? t->size : 1, t->align ? t->align : 1};
    PUSH(C->f->slots, C->f->nslots, C->f->scap, s);
    return C->f->nslots - 1;
}

static bool terminated(void) {
    Block *b = &C->f->blocks[C->blk];
    return b->n && inst_is_term(&b->insts[b->n - 1]);
}

/* CUR_TOK: the node being lowered; instructions remember it (comptime errors point there) */

static Inst *emit(int op) {
    if (terminated()) set_blk(new_block()); /* unreachable code gets its own block */
    Inst in;
    memset(&in, 0, sizeof in);
    in.op = op;
    in.tok = CUR_TOK;
    in.dst = -1;
    in.slot = -1;
    in.t1 = in.t2 = -1;
    Block *b = &C->f->blocks[C->blk];
    PUSH(b->insts, b->n, b->cap, in);
    return &b->insts[b->n - 1];
}

static void jmp(int b) {
    if (terminated()) return;
    emit(I_JMP)->t1 = b;
}

static void br(Opd c, int t, int f) {
    Inst *i = emit(I_BR);
    i->a = c;
    i->t1 = t;
    i->t2 = f;
}

static Opd R(int v) { return (Opd){O_REG, v}; }
static Opd IMM(int64_t v) { return (Opd){O_IMM, v}; }

static int op1(int op, int cls, Opd a) {
    int r = new_vreg(cls);
    Inst *i = emit(op);
    i->dst = r;
    i->cls = cls;
    i->a = a;
    return r;
}

static int op2(int op, int cls, Opd a, Opd b) {
    int r = new_vreg(cls);
    Inst *i = emit(op);
    i->dst = r;
    i->cls = cls;
    i->a = a;
    i->b = b;
    return r;
}

static int iconst(int64_t v) {
    int r = new_vreg(C_I);
    Inst *i = emit(I_ICONST);
    i->dst = r;
    i->imm = v;
    return r;
}

static int fconst(int cls, double v) {
    int r = new_vreg(cls);
    Inst *i = emit(I_FCONST);
    i->dst = r;
    i->cls = cls;
    i->fimm = v;
    return r;
}

static void mov(int dst, int cls, Opd a) {
    Inst *i = emit(I_MOV);
    i->dst = dst;
    i->cls = cls;
    i->a = a;
}

static int cmp(int cc, Opd a, Opd b) {
    static const int mirror[] = {CC_EQ, CC_NE, CC_GT, CC_GE, CC_LT, CC_LE, CC_HI, CC_HS, CC_LO, CC_LS, CC_MI, CC_PL};
    if (a.k == O_IMM) {
        if (b.k == O_IMM) a = R(iconst(a.v));
        else {
            Opd t = a;
            a = b;
            b = t;
            cc = mirror[cc];
        }
    }
    int r = new_vreg(C_I);
    Inst *i = emit(I_CMP);
    i->dst = r;
    i->a = a;
    i->b = b;
    i->mem = cc;
    return r;
}

static int fcmp(int cc, int a, int b) {
    int r = new_vreg(C_I);
    Inst *i = emit(I_FCMP);
    i->dst = r;
    i->a = R(a);
    i->b = R(b);
    i->mem = cc;
    return r;
}

static int emit_call(const char *sym, Opd *args, int *cls, int n, int nfixed, int retcls) {
    int r = retcls >= 0 ? new_vreg(retcls) : -1;
    Inst *i = emit(I_CALL);
    i->sym = sym;
    i->args = args;
    i->argcls = cls;
    i->nargs = n;
    i->nfixed = nfixed;
    i->dst = r;
    i->cls = retcls < 0 ? C_I : retcls;
    return r;
}

/* callv(sym, retcls, n, opd1, cls1, opd2, cls2, ...) */
static int callv(const char *sym, int retcls, int n, ...) {
    Opd *args = xalloc(sizeof(Opd) * (size_t)(n ? n : 1));
    int *cls = xalloc(sizeof(int) * (size_t)(n ? n : 1));
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        args[i] = va_arg(ap, Opd);
        cls[i] = va_arg(ap, int);
    }
    va_end(ap);
    return emit_call(sym, args, cls, n, n, retcls);
}

static void trap(void) { emit(I_TRAP); }

/* ---------- addresses and values ---------- */

typedef struct {
    int slot, reg;
    int64_t off;
} Addr;

typedef struct {
    Type *t;
    int reg;
    bool isconst;
    int64_t ival;
    double fval;
    Addr a;     /* aggregates: where the value lives */
    bool fresh; /* aggregates: a is a private temporary slot */
    int strlit; /* a string literal: its index + 1 (it can become a cstr) */
} Val;

static Addr A_slot(int s, int64_t off) { return (Addr){s, -1, off}; }
static Addr A_reg(int r, int64_t off) { return (Addr){-1, r, off}; }
static Addr A_add(Addr a, int64_t off) {
    a.off += off;
    return a;
}

static Val V_reg(Type *t, int r) {
    Val v;
    memset(&v, 0, sizeof v);
    v.t = t;
    v.reg = r;
    v.a = A_reg(-1, 0);
    return v;
}
static Val V_int(Type *t, int64_t x) {
    Val v = V_reg(t, -1);
    v.isconst = true;
    v.ival = x;
    return v;
}
static Val V_flt(Type *t, double x) {
    Val v = V_reg(t, -1);
    v.isconst = true;
    v.fval = x;
    return v;
}
static Val V_agg(Type *t, Addr a, bool fresh) {
    Val v = V_reg(t, -1);
    v.a = a;
    v.fresh = fresh;
    return v;
}
static Val V_void(void) { return V_reg(ty_void, -1); }

static int reg_of(Val v) {
    if (!v.isconst) return v.reg;
    if (is_float(v.t)) return fconst(cls_of(v.t), v.fval);
    return iconst(v.ival);
}

static Opd opd_of(Val v) {
    if (v.isconst && !is_float(v.t)) return IMM(v.ival);
    return R(reg_of(v));
}

static int addr_reg(Addr a) {
    int r;
    if (a.slot >= 0) {
        r = new_vreg(C_I);
        Inst *i = emit(I_ADDR_SLOT);
        i->dst = r;
        i->slot = a.slot;
    } else {
        r = a.reg;
    }
    if (a.off) r = op2(I_ADD, C_I, R(r), IMM(a.off));
    return r;
}

static int load(Type *t, Addr a) {
    int r = new_vreg(cls_of(t));
    Inst *i = emit(I_LOAD);
    i->dst = r;
    i->cls = cls_of(t);
    i->mem = memcode(t);
    i->slot = a.slot;
    if (a.slot < 0) i->a = R(a.reg);
    i->imm = a.off;
    return r;
}

static void store(Type *t, Addr a, Opd v) {
    Inst *i = emit(I_STORE);
    i->cls = cls_of(t);
    i->mem = memcode(t);
    i->slot = a.slot;
    if (a.slot < 0) i->a = R(a.reg);
    i->imm = a.off;
    i->b = v;
}

static Addr temp(Type *t) { return A_slot(new_slot(t), 0); }

static void copy_agg(Type *t, Addr dst, Addr src) {
    Leaf L[8];
    int n = leaves(t, 0, L, 0, 8);
    if (n >= 0) {
        int regs[8];
        for (int i = 0; i < n; i++) regs[i] = load(L[i].t, A_add(src, L[i].off));
        for (int i = 0; i < n; i++) store(L[i].t, A_add(dst, L[i].off), R(regs[i]));
        return;
    }
    int d = addr_reg(dst), s = addr_reg(src);
    if (t->size <= 64) {
        Inst *i = emit(I_MEMCPY);
        i->a = R(d);
        i->b = R(s);
        i->imm = t->size;
    } else {
        callv("_memcpy", -1, 3, R(d), C_I, R(s), C_I, IMM(t->size), C_I);
    }
}

static void zero_at(Type *t, Addr dst) {
    if (!is_agg(t)) {
        if (is_float(t)) store(t, dst, R(fconst(cls_of(t), 0)));
        else store(t, dst, IMM(0));
        return;
    }
    Leaf L[8];
    int n = leaves(t, 0, L, 0, 8);
    if (n >= 0) {
        for (int i = 0; i < n; i++) zero_at(L[i].t, A_add(dst, L[i].off));
        return;
    }
    int d = addr_reg(dst);
    if (t->size <= 64) {
        Inst *i = emit(I_MEMZERO);
        i->a = R(d);
        i->imm = t->size;
    } else {
        callv("_bzero", -1, 2, R(d), C_I, IMM(t->size), C_I);
    }
}

/* the memory of an `undefined` value. Reading it must give an unspecified but *stable* value
 * (section 4.1), never UB. Debug fills it with 0xAA so such reads stand out. Release modes zero
 * scalars and small aggregates (a few stores, which the optimizer turns into register values),
 * and keep large ones in memory, whose bytes are stable without a memset: the optimizer would
 * otherwise promote never-written fields to registers that hold anything at all. */
static void undefined_at(Type *t, Addr dst) {
    if (MODE != MODE_DEBUG) {
        if (!is_agg(t) || t->size <= 64) zero_at(t, dst);
        else (void)addr_reg(dst); /* taking the address pins a slot in memory */
        return;
    }
    const int64_t pat = (int64_t)0xAAAAAAAAAAAAAAAAULL;
    if (t->size > 256) {
        callv("_memset", -1, 3, R(addr_reg(dst)), C_I, IMM(0xAA), C_I, IMM(t->size), C_I);
        return;
    }
    int off = 0;
    for (; off + 8 <= t->size; off += 8) store(ty_u64, A_add(dst, off), IMM(pat));
    for (int k = 4; k >= 1; k /= 2)
        if (t->size - off >= k) {
            store(k == 4 ? ty_u32 : k == 2 ? ty_u16 : ty_u8, A_add(dst, off), IMM(pat));
            off += k;
        }
}

static Val load_val(Type *t, Addr a) {
    if (is_agg(t)) return V_agg(t, a, false);
    return V_reg(t, load(t, a));
}

static void store_val(Type *t, Addr a, Val v) {
    if (is_agg(t)) copy_agg(t, a, v.a);
    else store(t, a, opd_of(v));
}

/* string literal -> fresh str value */
static int add_str(const char *data, int len) {
    for (int i = 0; i < M->nstrs; i++)
        if (M->strs[i].len == len && !memcmp(M->strs[i].data, data, (size_t)len)) return i;
    StrLit s = {strf("l_fl_str_%d", M->nstrs), xalloc((size_t)len + 1), len};
    if (len) memcpy(s.data, data, (size_t)len);
    PUSH(M->strs, M->nstrs, M->scap, s);
    return M->nstrs - 1;
}

/* the address of a symbol; `got`: it's defined in another image (C), so go through the GOT */
static int addr_sym_q(const char *sym, bool got) {
    int r = new_vreg(C_I);
    Inst *i = emit(I_ADDR_SYM);
    i->dst = r;
    i->sym = sym;
    i->mem = got;
    return r;
}

static int addr_sym(const char *sym) { return addr_sym_q(sym, false); }

static Val str_lit(const char *data, int len) {
    int k = add_str(data, len);
    Addr t = temp(ty_str);
    store(ptr_to(ty_u8), t, R(addr_sym(M->strs[k].label)));
    store(ty_i64, A_add(t, 8), IMM(len));
    Val v = V_agg(ty_str, t, true);
    v.strlit = k + 1;
    return v;
}

/* ====================================================================== */
/* conversions                                                            */
/* ====================================================================== */

static int64_t trunc_ext(int64_t x, Type *t) {
    int bits = t->size * 8;
    if (bits >= 64) return x;
    uint64_t m = ((uint64_t)1 << bits) - 1;
    uint64_t u = (uint64_t)x & m;
    if (is_signed(t) && (u >> (bits - 1))) u |= ~m;
    return (int64_t)u;
}

/* keep narrow integers sign/zero-extended to 64 bits in registers */
static int norm(int r, Type *t) {
    if (!is_int(t) || is_lit(t) || t->size >= 8) return r;
    int d = new_vreg(C_I);
    Inst *i = emit(I_EXT);
    i->dst = d;
    i->a = R(r);
    i->mem = memcode(t);
    return d;
}

static bool widens(Type *a, Type *b) {
    if (is_int(a) && !is_lit(a) && is_int(b) && !is_lit(b)) {
        bool sa = is_signed(a), sb = is_signed(b);
        /* usize/isize are pointer-sized: at least 32 and at most 64 bits on any target */
        if (a == b) return true;
        if (b == ty_usize) return !sa && a->size <= 4;
        if (b == ty_isize) return sa ? a->size <= 4 : a->size < 4;
        if (a == ty_usize) return !sb && b->size == 8;
        if (a == ty_isize) return sb && b->size == 8;
        if (sa == sb) return a->size <= b->size;
        return !sa && sb && a->size < b->size;
    }
    if (is_int(a) && !is_lit(a) && b->k == TY_F64) return true;
    if (is_int(a) && !is_lit(a) && b->k == TY_F32) return a->size <= 2;
    return a->k == TY_F32 && b->k == TY_F64;
}

/* explicit numeric / pointer conversion (used by `as` and implicit widening) */
static Val convert(Val v, Type *to) {
    Type *from = v.t;
    if (from == to) return v;
    if (v.isconst && (is_num(from) || from->k == TY_BOOL || from == ty_null)) {
        if (is_float(to)) {
            double d = is_float(from) ? v.fval : (is_signed(from) ? (double)v.ival : (double)(uint64_t)v.ival);
            if (to->k == TY_F32) d = (float)d;
            return V_flt(to, d);
        }
        int64_t x = is_float(from) ? (is_signed(to) ? (int64_t)v.fval : (int64_t)(uint64_t)v.fval) : v.ival;
        if (is_int(to)) x = trunc_ext(x, to);
        return V_int(to, x);
    }
    if ((is_int(from) || from->k == TY_BOOL) && is_int(to)) {
        bool same = from->size == to->size && is_signed(from) == is_signed(to);
        bool noop = same || to->size == 8 || (from->size < to->size && (!is_signed(from) || is_signed(to)));
        int r = noop ? v.reg : norm(v.reg, to);
        return V_reg(to, r);
    }
    if (is_int(from) && is_float(to))
        return V_reg(to, op1(is_signed(from) ? I_SCVTF : I_UCVTF, cls_of(to), R(v.reg)));
    if (is_float(from) && is_int(to)) {
        int r = op1(is_signed(to) ? I_FCVTZS : I_FCVTZU, C_I, R(reg_of(v)));
        return V_reg(to, norm(r, to));
    }
    if (is_float(from) && is_float(to)) return V_reg(to, op1(I_FCVT, cls_of(to), R(reg_of(v))));
    Val w = v;
    w.t = to;
    return w;
}

/* every error name has one program-wide code; 0 means "no error" */
static int err_code(const char *name) {
    if (!M->nerrnames) PUSH(M->errnames, M->nerrnames, M->errcap, "(no error)");
    for (int i = 1; i < M->nerrnames; i++)
        if (!strcmp(M->errnames[i], name)) return i;
    PUSH(M->errnames, M->nerrnames, M->errcap, name);
    return M->nerrnames - 1;
}

static bool set_has(Type *set, int64_t code) {
    if (set->all_errors) return true;
    for (int i = 0; i < set->nerrs; i++)
        if (set->errcodes[i] == code) return true;
    return false;
}

static bool set_subset(Type *a, Type *b) {
    if (b->all_errors) return true;
    if (a->all_errors) return false;
    for (int i = 0; i < a->nerrs; i++)
        if (!set_has(b, a->errcodes[i])) return false;
    return true;
}

static Val eu_ok(Val inner, Type *eu) {
    if (eu->elem == ty_void) return V_int(eu, 0);
    layout(eu, NULL);
    Addr a = temp(eu);
    store_val(eu->elem, a, inner);
    store(ty_u16, A_add(a, err_off(eu)), IMM(0));
    return V_agg(eu, a, true);
}

static Val eu_fail(Val e, Type *eu) {
    if (eu->elem == ty_void) return e.isconst ? V_int(eu, e.ival) : V_reg(eu, e.reg);
    layout(eu, NULL);
    Addr a = temp(eu);
    store(ty_u16, A_add(a, err_off(eu)), opd_of(e));
    return V_agg(eu, a, true);
}

/* the error code (0 = success) */
static Opd eu_err(Val v) {
    if (v.t->elem == ty_void) return v.isconst ? IMM(v.ival) : R(reg_of(v));
    return R(load(ty_u16, A_add(v.a, err_off(v.t))));
}

static Val eu_value(Val v) {
    if (v.t->elem == ty_void) return V_void();
    return load_val(v.t->elem, v.a);
}

static Val opt_null(Type *ot) {
    if (is_optptr(ot)) return V_int(ot, 0);
    layout(ot, NULL);
    Addr a = temp(ot);
    zero_at(ot, a);
    return V_agg(ot, a, true);
}

static Val opt_wrap(Val inner, Type *ot) {
    if (is_optptr(ot)) {
        inner.t = ot;
        return inner;
    }
    layout(ot, NULL);
    Addr a = temp(ot);
    store_val(ot->elem, a, inner);
    store(ty_bool, A_add(a, ot->elem->size), IMM(1));
    return V_agg(ot, a, true);
}

static int opt_has(Val v) {
    if (is_optptr(v.t)) return cmp(CC_NE, opd_of(v), IMM(0));
    return load(ty_bool, A_add(v.a, v.t->elem->size));
}

static Val opt_value(Val v) {
    if (is_optptr(v.t)) return V_reg(v.t->elem, reg_of(v));
    return load_val(v.t->elem, v.a);
}

/* the value of an expression that never finishes (return, break, panic): never used */
static Val dead_value(Type *want) {
    if (want == ty_void) return V_void();
    if (is_agg(want)) return V_agg(want, temp(want), true);
    return is_float(want) ? V_flt(want, 0) : V_int(want, 0);
}

static Val coerce(Val v, Type *want, Token *tok, const char *what) {
    Type *t = v.t;
    if (t == want) return v;
    if (t == ty_noreturn) return dead_value(want);
    if (want->k == TY_ERRUNION) {
        if (t->k == TY_ERRSET) return eu_fail(coerce(v, want->eset, tok, what), want); /* return error.X */
        if (t->k == TY_ERRUNION) {
            if (t->elem == want->elem) {
                if (!set_subset(t->eset, want->eset))
                    error_at(tok, "%s can fail with errors that %s doesn't include", tname(t), tname(want));
                v.t = want;
                return v;
            }
        } else {
            return eu_ok(coerce(v, want->elem, tok, what), want);
        }
    }
    if (want->k == TY_ERRSET && t->k == TY_ERRSET) {
        if (t == ty_errlit) {
            if (!set_has(want, v.ival)) error_at(tok, "%s has no error %s", tname(want), M->errnames[v.ival]);
        } else if (!set_subset(t, want)) {
            error_at(tok, "%s includes errors that %s doesn't", tname(t), tname(want));
        }
        v.t = want;
        return v;
    }
    if (want->k == TY_OPT && t->k != TY_OPT) { /* T -> ?T */
        if (t == ty_null) return opt_null(want);
        return opt_wrap(coerce(v, want->elem, tok, what), want);
    }
    if (is_optptr(t) && is_optptr(want)) { /* ?*T -> ?*const T, ?*void */
        Type *a = t->elem, *b = want->elem;
        if ((a->elem == b->elem && a->many == b->many && (b->is_const || !a->is_const) && (b != ty_cstr || a == ty_cstr)) ||
            b->elem == ty_void) {
            v.t = want;
            return v;
        }
    }
    if (t->k == TY_LITINT && is_int(want)) {
        if (want->size < 8) {
            if (trunc_ext(v.ival, want) != v.ival)
                error_at(tok, "integer literal %lld does not fit in %s", (long long)v.ival, tname(want));
        } else if (want->k == TY_U64 && v.ival < 0) {
            error_at(tok, "integer literal %lld does not fit in %s", (long long)v.ival, tname(want));
        }
        v.t = want;
        return v;
    }
    if (t->k == TY_LITINT && is_float(want)) return V_flt(want, (double)v.ival);
    if (t->k == TY_LITFLOAT && is_float(want)) {
        if (want->k == TY_F32) v.fval = (float)v.fval;
        v.t = want;
        return v;
    }
    if (is_array_ptr(t) && (is_slice(want) || is_many(want)) && t->elem->elem == want->elem &&
        (want->is_const || !t->is_const)) { /* &arr as a slice or a [*]T */
        if (is_many(want)) {
            v.t = want;
            return v;
        }
        Addr a = temp(want);
        store(ptr_to(want->elem), a, opd_of(v));
        store(ty_usize, A_add(a, 8), IMM(t->elem->alen));
        return V_agg(want, a, true);
    }
    if (want == ty_cstr && is_bytes(t)) {
        if (!v.strlit)
            error_at(tok, "only a string literal converts to cstr for %s: other strings have no NUL terminator (use mem.dupeZ)", what);
        return V_reg(ty_cstr, addr_sym(M->strs[v.strlit - 1].label));
    }
    if (t == ty_null && is_fn(want))
        error_at(tok, "%s can't be null; use ?%s for a function that might be missing", tname(want), tname(want));
    if (t == ty_null && is_ptr(want))
        error_at(tok, "%s can't be null; use ?%s for a pointer that might be null", tname(want), tname(want));
    if (((is_ptr(t) && is_ptr(want) && t->many == want->many && (want != ty_cstr || t == ty_cstr)) ||
         (is_slice(t) && is_slice(want))) &&
        t->elem == want->elem && want->is_const) {
        v.t = want; /* adding const is always fine */
        return v;
    }
    if (widens(t, want)) return convert(v, want);
    if (is_ptr(t) && is_ptr(want) && want->elem == ty_void) { /* *T -> *void: forgetting the type is safe */
        v.t = want;
        return v;
    }
    if ((is_ptr(t) || is_optptr(t)) && (is_ptr(want) || is_optptr(want)) &&
        (t->k == TY_OPT ? t->elem : t)->elem == ty_void)
        error_at(tok, "type mismatch: expected %s, found %s for %s (converting from *void is unchecked: use "
                      "'@ptrCast(%s, p)' inside an 'unsafe' block)", tname(want), tname(t), what, tname(want));
    const char *hint = (is_num(t) && is_num(want)) ? strf(" (convert explicitly with 'as %s')", tname(want)) : "";
    if (t->k == TY_OPT) hint = " (it might be null: unwrap it with 'x.?' or 'if (x) |v| { ... }')";
    if (t->k == TY_ERRUNION) hint = " (it can fail: handle the error with 'try' or 'catch')";
    error_at(tok, "type mismatch: expected %s, found %s for %s%s", tname(want), tname(t), what, hint);
}

static Type *unify(Type *a, Type *b, Token *tok, const char *op) {
    if (a == b) return a;
    if (is_lit(a) && is_lit(b)) return ty_litfloat;
    if ((a->k == TY_LITINT && is_num(b)) || (a->k == TY_LITFLOAT && is_float(b))) return b;
    if ((b->k == TY_LITINT && is_num(a)) || (b->k == TY_LITFLOAT && is_float(a))) return a;
    if (widens(a, b)) return b;
    if (widens(b, a)) return a;
    error_at(tok, "operator '%s' can't mix %s and %s%s", op, tname(a), tname(b),
             is_num(a) && is_num(b) ? " (use 'as' to convert)" : "");
}

static Val defaulted(Val v) {
    if (v.t == ty_errlit) v.t = ty_anyerror;
    if (v.t->k == TY_LITINT) v.t = ty_i64;
    else if (v.t->k == TY_LITFLOAT) v.t = ty_f64;
    return v;
}

/* ====================================================================== */
/* expressions                                                            */
/* ====================================================================== */

static Val expr(Node *n, Type *want);
static void panic_if(int bad, const char *msg);
static void cond(Node *n, int t, int f);
static void stmt(Node *n);
static void lower_fn(FnInfo *f);
static Val lib_call(Type *recv, const char *name, Val *args, int n, Token *tok);
static Val ex_builtin(Node *n, Type *want);
static Val tag_name(Val v);
static Val err_name_str(Opd code);
static Val enum_value(Type *et, const char *vname, int kind, Node **list, int n, Token *tok);
static Val lower_switch(Node *n, Type *want);
static const char *mangle(Type *t);

static Type *static_type(Node *n);
static bool is_static_ref(Node *n) { return static_type(n) != NULL; }
static FnInfo *callee_fn(Node *fn);
static Var *module_var(Node *n);
static _Noreturn void no_member(Unit *u, Node *n);
static Val c_value(CHeader *h, Node *n);
static Val c_call(CHeader *h, Node *fn, Node **args, int nargs, Token *tok);
static CDecl *c_need(CHeader *h, const char *name, Token *tok);
static Var *static_var(Type *t, const char *name);

static bool is_lvalue_node(Node *n) {
    if (n->k == N_NAME) {
        Var *v = find_var(n->name);
        return v && v->kind != VK_CONST;
    }
    if (n->k == N_FIELDACC && unit_ref(n->a)) {
        Var *v = module_var(n);
        return v && v->kind != VK_CONST;
    }
    if (n->k == N_FIELDACC && header_ref(n->a)) {
        CDecl *d = c_lookup(header_ref(n->a), n->name);
        return d && d->k == CD_VAR;
    }
    if (n->k == N_FIELDACC) return !is_static_ref(n->a) || static_var(static_type(n->a), n->name);
    if (n->k == N_INDEX) return true;
    if (n->k == N_DEREF) return true;
    return false;
}

typedef struct {
    Addr a;
    Type *t;
    bool ro;   /* read-only location */
    bool temp; /* field of a temporary value (not addressable) */
    int bits, bitoff; /* a packed struct field: these bits of the integer at `a` */
    bool sbits;
    Type *backing;
    Token *union_tok; /* a field of an extern union: reading it needs unsafe */
} LV;

static LV lval(Node *n);
static LV c_var(CHeader *h, CDecl *d, Token *tok);

static _Noreturn void opt_misuse(Token *tok, Type *t) {
    error_at(tok, "this is %s, which might be null; unwrap it first with 'if (x) |v| { ... }' or 'x.?'", tname(t));
}

static _Noreturn void eu_misuse(Token *tok, Type *t) {
    error_at(tok, "this is %s, which might be an error; handle it first with 'try' or 'catch'", tname(t));
}

static Addr elem_addr(int p, Val idx, int es) {
    if (idx.isconst) return A_reg(p, idx.ival * es);
    Opd off = es == 1 ? R(idx.reg) : R(op2(I_MUL, C_I, R(idx.reg), IMM(es)));
    return A_reg(op2(I_ADD, C_I, R(p), off), 0);
}

static void bounds_check(Opd idx, Opd len) {
    int c = cmp(CC_LO, idx, len);
    int ok = new_block(), fail = new_cold_block();
    br(R(c), ok, fail);
    set_blk(fail);
    callv("_rt_oob", -1, 2, idx, C_I, len, C_I);
    trap();
    set_blk(ok);
}

/* an index, slice bound or count: a usize (literals and narrower unsigned integers widen to it) */
static Val usize_val(Node *e, const char *what) {
    Val v = expr(e, ty_usize);
    if (v.t == ty_usize) return v;
    if (v.t->k == TY_LITINT) {
        if (v.ival < 0) error_at(e->tok, "%s can't be negative (%lld)", what, (long long)v.ival);
        return V_int(ty_usize, v.ival);
    }
    if (!is_int(v.t)) error_at(e->tok, "%s must be an integer (usize), found %s", what, tname(v.t));
    if (is_signed(v.t)) error_at(e->tok, "%s must be usize, found %s; convert it with 'as usize'", what, tname(v.t));
    return coerce(v, ty_usize, e->tok, what);
}

static Val index_val(Node *n) { return usize_val(n->b, "an index"); }

/* element i of the array [N]T at `ba`: constant indexes are checked at compile time */
static LV array_elem(Type *at, Addr ba, bool ro, bool tmp, Node *n) {
    LV r;
    memset(&r, 0, sizeof r);
    r.t = at->elem;
    r.ro = ro;
    r.temp = tmp;
    Val iv = index_val(n);
    int es = at->elem->size;
    if (iv.isconst) {
        if ((uint64_t)iv.ival >= (uint64_t)at->alen)
            error_at(n->b->tok, "index %llu is out of bounds for %s", (unsigned long long)iv.ival, tname(at));
        r.a = A_add(ba, iv.ival * es);
        return r;
    }
    if (bounds_checked()) bounds_check(opd_of(iv), IMM(at->alen));
    r.a = elem_addr(addr_reg(ba), iv, es);
    return r;
}

static LV index_lv_at(Node *n, Token **union_tok);

/* a[i]; indexing into a field of an extern union reads the union */
static LV index_lv(Node *n) {
    Token *ut = NULL;
    LV r = index_lv_at(n, &ut);
    if (!r.union_tok) r.union_tok = ut;
    return r;
}

static LV index_lv_at(Node *n, Token **union_tok) {
    Type *bt;
    Addr ba = A_reg(-1, 0);
    int p = -1;
    bool bro = false, btmp = false;
    if (is_lvalue_node(n->a)) {
        LV b = lval(n->a);
        bt = b.t;
        bro = b.ro;
        btmp = b.temp;
        *union_tok = b.union_tok;
        if (is_ptr(bt)) p = load(bt, b.a);
        else ba = b.a;
    } else {
        Val v = expr(n->a, NULL);
        bt = v.t;
        btmp = true;
        if (is_ptr(bt)) p = reg_of(v);
        else ba = v.a;
    }
    LV r;
    memset(&r, 0, sizeof r);
    if (bt->k == TY_OPT) opt_misuse(n->tok, bt);
    if (bt->k == TY_ERRUNION) eu_misuse(n->tok, bt);
    if (is_array(bt)) {
        layout(bt, n->tok);
        return array_elem(bt, ba, bro, btmp, n);
    }
    if (is_array_ptr(bt)) { /* *[N]T indexes like the array it points at */
        layout(bt->elem, n->tok);
        return array_elem(bt->elem, A_reg(p, 0), bt->is_const, false, n);
    }
    if (is_ptr(bt)) {
        if (bt->elem == ty_void) error_at(n->tok, "can't index *void; cast it first");
        if (!bt->many)
            error_at(n->tok, "can't index %s: it points at a single item (a pointer to many items is [*]%s%s)", tname(bt),
                     bt->is_const ? "const " : "", tname(bt->elem));
        require_unsafe(n->tok, strf("indexing %s (it has no length to check against)", tname(bt)));
        Val iv = index_val(n);
        r.t = bt->elem;
        r.ro = bt->is_const;
        r.a = elem_addr(p, iv, r.t->size);
        return r;
    }
    if (is_slice(bt)) {
        r.t = bt->elem;
        r.ro = bt->is_const;
        Val iv = index_val(n);
        int ptr = load(ptr_to(r.t), ba);
        if (bounds_checked()) bounds_check(opd_of(iv), R(load(ty_i64, A_add(ba, 8))));
        r.a = elem_addr(ptr, iv, r.t->size);
        return r;
    }
    error_at(n->tok, "can't index into %s", tname(bt));
}

/* member access on a struct / str / slice located at `base` */
static LV member(Type *bt, Addr base, bool ro, Node *n) {
    if (bt->k == TY_OPT) opt_misuse(n->tok, bt);
    if (bt->k == TY_ERRUNION) eu_misuse(n->tok, bt);
    LV r;
    memset(&r, 0, sizeof r);
    r.ro = ro;
    if (is_array(bt)) { /* .len is a constant; it lives in a scratch slot so it can be read like a field */
        if (strcmp(n->name, "len")) error_at(n->tok, "%s has no field '%s' (arrays have .len)", tname(bt), n->name);
        r.t = ty_usize;
        r.a = temp(ty_usize);
        store(ty_usize, r.a, IMM(bt->alen));
        r.ro = true;
        return r;
    }
    if (is_slice(bt)) {
        const char *f = n->name;
        if (!strcmp(f, "len")) r.t = ty_usize, r.a = A_add(base, 8);
        else if (!strcmp(f, "ptr")) r.t = many_ptr_q(bt->elem, bt->is_const), r.a = base;
        else error_at(n->tok, "%s has no field '%s' (a slice has .ptr and .len)", tname(bt), f);
        return r;
    }
    if (bt->k != TY_STRUCT) error_at(n->tok, "%s has no fields", tname(bt));
    for (int i = 0; i < bt->nfields; i++) {
        if (!strcmp(bt->fields[i].name, n->name)) {
            Field *fd = &bt->fields[i];
            r.t = fd->t;
            r.a = A_add(base, fd->off);
            if (bt->is_union) r.union_tok = n->tok;
            if (bt->packed) {
                layout(bt, n->tok);
                r.bits = fd->bits;
                r.bitoff = fd->bitoff;
                r.sbits = fd->sbits;
                r.backing = bt->tagt;
            }
            return r;
        }
    }
    if (find_method(bt, n->name)) error_at(n->tok, "'%s' is a method; call it with ()", n->name);
    if (bt->tuple) error_at(n->tok, "%s has no element .%s (its elements are .0 to .%d)", tname(bt), n->name, bt->nfields - 1);
    error_at(n->tok, "struct '%s' has no field '%s'", bt->name, n->name);
}

static LV lval_in(Node *n);
static LV lval(Node *n) {
    Unit *su = CU;
    if (n->unit) CU = n->unit;
    LV r = lval_in(n);
    CU = su;
    return r;
}

static LV lval_in(Node *n) {
    LV r;
    memset(&r, 0, sizeof r);
    if (n->k == N_NAME) {
        Var *v = lookup(n->name, n->tok);
        r.t = v->t;
        r.ro = v->is_const;
        switch (v->kind) {
        case VK_SLOT: r.a = A_slot(v->slot, 0); break;
        case VK_REF: r.a = A_reg(v->reg, 0); break;
        case VK_GLOBAL: r.a = A_reg(addr_sym_q(v->sym, v->ext), 0); break;
        case VK_TYPE: error_at(n->tok, "'%s' is a type, not a variable", n->name);
        default: error_at(n->tok, "can't assign to '%s': it's immutable (use 'var' to declare a mutable variable)", n->name);
        }
        return r;
    }
    if (n->k == N_FIELDACC && header_ref(n->a)) { /* c.some_global */
        CHeader *h = header_ref(n->a);
        CDecl *d = c_need(h, n->name, n->tok);
        if (d->k != CD_VAR) error_at(n->tok, "'%s' from %s isn't a variable", n->name, c_header_name(h));
        return c_var(h, d, n->tok);
    }
    if (n->k == N_FIELDACC && unit_ref(n->a)) { /* mod.counter */
        Var *v = module_var(n);
        if (!v) no_member(unit_ref(n->a), n);
        if (v->kind != VK_GLOBAL) error_at(n->tok, "can't assign to '%s': it's a constant", n->name);
        r.t = v->t;
        r.ro = v->is_const;
        r.a = A_reg(addr_sym_q(v->sym, v->ext), 0);
        return r;
    }
    if (n->k == N_FIELDACC) {
        if (is_static_ref(n->a) && static_var(static_type(n->a), n->name)) {
            Var *v = static_var(static_type(n->a), n->name);
            if (v->kind != VK_GLOBAL) error_at(n->tok, "can't assign to '%s': it's a constant", v->name);
            r.t = v->t;
            r.ro = v->is_const;
            r.a = A_reg(addr_sym_q(v->sym, v->ext), 0);
            return r;
        }
        if (is_static_ref(n->a))
            error_at(n->tok, "'%s.%s' must be called: %s.%s(...)", tname(static_type(n->a)), n->name, tname(static_type(n->a)), n->name);
        Type *bt;
        Addr ba;
        bool ro = false, tmp = false;
        LV outer; /* a packed struct inside a packed struct: its bits sit inside the outer integer */
        memset(&outer, 0, sizeof outer);
        if (is_lvalue_node(n->a)) {
            LV b = lval(n->a);
            outer = b;
            bt = b.t;
            ba = b.a;
            ro = b.ro;
            tmp = b.temp;
            if (is_ptr(bt)) {
                if (bt->many) error_at(n->tok, "%s points at many items; index it first: p[i].%s", tname(bt), n->name);
                ro = bt->is_const;
                ba = A_reg(load(bt, b.a), 0);
                bt = bt->elem;
                tmp = false;
            }
        } else {
            Val v = expr(n->a, NULL);
            bt = v.t;
            tmp = true;
            if (is_ptr(bt)) {
                if (bt->many) error_at(n->tok, "%s points at many items; index it first: p[i].%s", tname(bt), n->name);
                ro = bt->is_const;
                ba = A_reg(reg_of(v), 0);
                bt = bt->elem;
                tmp = false;
            } else if (is_agg(bt)) {
                ba = v.a;
            } else {
                error_at(n->tok, "%s has no fields", tname(bt));
            }
        }
        r = member(bt, ba, ro, n);
        r.temp = tmp;
        if (!r.union_tok) r.union_tok = outer.union_tok; /* u.s.x reads the union */
        if (outer.bits && r.bits) {
            r.bitoff += outer.bitoff;
            r.backing = outer.backing;
        }
        return r;
    }
    if (n->k == N_INDEX) return index_lv(n);
    if (n->k == N_DEREF) {
        Val p = expr(n->a, NULL);
        if (p.t->k == TY_OPT) opt_misuse(n->tok, p.t);
        if (!is_ptr(p.t)) error_at(n->tok, "can't dereference %s: not a pointer", tname(p.t));
        if (p.t->elem == ty_void) error_at(n->tok, "can't dereference *void; cast it first");
        if (p.t->many) error_at(n->tok, "%s points at many items, so '.*' is ambiguous; index it: p[0]", tname(p.t));
        r.t = p.t->elem;
        r.ro = p.t->is_const;
        r.a = A_reg(reg_of(p), 0);
        return r;
    }
    error_at(n->tok, "can't assign to this expression");
}

/* ---------- packed struct fields ---------- */

static Val bits_load(LV l) {
    int r = load(l.backing, l.a); /* zero-extended */
    if (l.bits < 64) {
        if (l.sbits) {
            if (64 - l.bitoff - l.bits) r = op2(I_SHL, C_I, R(r), IMM(64 - l.bitoff - l.bits));
            r = op2(I_ASR, C_I, R(r), IMM(64 - l.bits));
        } else {
            if (l.bitoff) r = op2(I_LSR, C_I, R(r), IMM(l.bitoff));
            r = op2(I_AND, C_I, R(r), IMM((int64_t)((1ULL << l.bits) - 1)));
        }
    }
    if (l.t->k == TY_STRUCT) { /* a nested packed struct */
        Addr a = temp(l.t);
        store(l.t->tagt, a, R(r));
        return V_agg(l.t, a, true);
    }
    return V_reg(l.t, r);
}

static void bits_store(LV l, Val v, Token *tok) {
    uint64_t m = l.bits >= 64 ? ~0ULL : (1ULL << l.bits) - 1;
    Opd nv;
    if (v.isconst && !is_agg(v.t)) {
        bool fits = l.sbits ? (l.bits >= 64 || (v.ival >= -(int64_t)(1ULL << (l.bits - 1)) && v.ival < (int64_t)(1ULL << (l.bits - 1))))
                            : (l.bits >= 64 || (uint64_t)v.ival <= m);
        if (!fits) error_at(tok, "%lld doesn't fit in this %d-bit field", (long long)v.ival, l.bits);
        nv = IMM((int64_t)(((uint64_t)v.ival & m) << l.bitoff));
    } else {
        int r = is_agg(v.t) ? load(v.t->tagt, v.a) : reg_of(v);
        if (l.bits < 64 && is_int(v.t)) { /* like 'as': a value that doesn't fit traps */
            const char *msg = strf("value doesn't fit in this %d-bit field", l.bits);
            if (l.sbits) {
                int s = op2(I_ASR, C_I, R(op2(I_SHL, C_I, R(r), IMM(64 - l.bits))), IMM(64 - l.bits));
                panic_if(cmp(CC_NE, R(s), R(r)), msg);
            } else {
                panic_if(cmp(CC_HI, R(r), IMM((int64_t)m)), msg);
            }
        }
        if (l.bits < 64) r = op2(I_AND, C_I, R(r), IMM((int64_t)m));
        if (l.bitoff) r = op2(I_SHL, C_I, R(r), IMM(l.bitoff));
        nv = R(r);
    }
    if (l.bits >= 64) {
        store(l.backing, l.a, nv);
        return;
    }
    int raw = load(l.backing, l.a);
    raw = op2(I_AND, C_I, R(raw), IMM((int64_t)~(m << l.bitoff)));
    raw = op2(I_OR, C_I, R(raw), nv);
    store(l.backing, l.a, R(raw));
}

static void union_read(LV l) {
    if (l.union_tok) require_unsafe(l.union_tok, "reading a field of an extern union");
}

static Val lv_load(LV l) {
    union_read(l);
    return l.bits ? bits_load(l) : load_val(l.t, l.a);
}

static void lv_store(LV l, Val v, Token *tok) {
    if (l.bits) bits_store(l, v, tok);
    else store_val(l.t, l.a, v);
}

/* ---------- operators ---------- */

static bool is_cmp_op(const char *op) {
    return !strcmp(op, "==") || !strcmp(op, "!=") || !strcmp(op, "<") || !strcmp(op, "<=") ||
           !strcmp(op, ">") || !strcmp(op, ">=");
}

static int cc_for(const char *op, bool is_signed_, bool is_fp) {
    if (!strcmp(op, "==")) return CC_EQ;
    if (!strcmp(op, "!=")) return CC_NE;
    if (is_fp) {
        if (!strcmp(op, "<")) return CC_MI;
        if (!strcmp(op, "<=")) return CC_LS;
        if (!strcmp(op, ">")) return CC_GT;
        return CC_GE;
    }
    if (!strcmp(op, "<")) return is_signed_ ? CC_LT : CC_LO;
    if (!strcmp(op, "<=")) return is_signed_ ? CC_LE : CC_LS;
    if (!strcmp(op, ">")) return is_signed_ ? CC_GT : CC_HI;
    return is_signed_ ? CC_GE : CC_HS;
}

static Val fold(const char *op, Val a, Val b, Token *tok) {
    bool fp = is_float(a.t) || is_float(b.t);
    if (fp) {
        double x = is_float(a.t) ? a.fval : (double)a.ival, y = is_float(b.t) ? b.fval : (double)b.ival;
        if (!strcmp(op, "+")) return V_flt(ty_litfloat, x + y);
        if (!strcmp(op, "-")) return V_flt(ty_litfloat, x - y);
        if (!strcmp(op, "*")) return V_flt(ty_litfloat, x * y);
        if (!strcmp(op, "/")) return V_flt(ty_litfloat, x / y);
        if (!strcmp(op, "%")) return V_flt(ty_litfloat, x - y * (double)(int64_t)(x / y));
        if (!strcmp(op, "==")) return V_int(ty_bool, x == y);
        if (!strcmp(op, "!=")) return V_int(ty_bool, x != y);
        if (!strcmp(op, "<")) return V_int(ty_bool, x < y);
        if (!strcmp(op, "<=")) return V_int(ty_bool, x <= y);
        if (!strcmp(op, ">")) return V_int(ty_bool, x > y);
        if (!strcmp(op, ">=")) return V_int(ty_bool, x >= y);
        error_at(tok, "operator '%s' needs integers", op);
    }
    int64_t x = a.ival, y = b.ival;
    uint64_t ux = (uint64_t)x, uy = (uint64_t)y;
    if ((!strcmp(op, "/") || !strcmp(op, "%")) && y == 0) error_at(tok, "division by zero");
    if (!strcmp(op, "+")) return V_int(ty_litint, (int64_t)(ux + uy));
    if (!strcmp(op, "-")) return V_int(ty_litint, (int64_t)(ux - uy));
    if (!strcmp(op, "*")) return V_int(ty_litint, (int64_t)(ux * uy));
    if (!strcmp(op, "/")) return V_int(ty_litint, (x == INT64_MIN && y == -1) ? x : x / y);
    if (!strcmp(op, "%")) return V_int(ty_litint, (x == INT64_MIN && y == -1) ? 0 : x % y);
    if (!strcmp(op, "&")) return V_int(ty_litint, x & y);
    if (!strcmp(op, "|")) return V_int(ty_litint, x | y);
    if (!strcmp(op, "^")) return V_int(ty_litint, x ^ y);
    if (!strcmp(op, "<<")) return V_int(ty_litint, (int64_t)(ux << (uy & 63)));
    if (!strcmp(op, ">>")) return V_int(ty_litint, x >> (uy & 63));
    if (!strcmp(op, "==")) return V_int(ty_bool, x == y);
    if (!strcmp(op, "!=")) return V_int(ty_bool, x != y);
    if (!strcmp(op, "<")) return V_int(ty_bool, x < y);
    if (!strcmp(op, "<=")) return V_int(ty_bool, x <= y);
    if (!strcmp(op, ">")) return V_int(ty_bool, x > y);
    return V_int(ty_bool, x >= y);
}

static void panic_msg(const char *msg);

/* panic with `msg` when the bool in vreg `bad` is true (the panic path is a cold block) */
static void panic_if(int bad, const char *msg) {
    int ok = new_block(), fail = new_cold_block();
    br(R(bad), fail, ok);
    set_blk(fail);
    panic_msg(msg);
    set_blk(ok);
}

static int64_t int_min(Type *t) {
    if (!is_signed(t)) return 0;
    return t->size >= 8 ? INT64_MIN : -((int64_t)1 << (t->size * 8 - 1));
}

static int64_t int_max(Type *t) { /* as raw 64-bit pattern: u64's max is -1 */
    int bits = t->size * 8 - is_signed(t);
    return bits >= 64 ? -1 : bits == 63 ? INT64_MAX : ((int64_t)1 << bits) - 1;
}

/* r = a op b (I_ADD, I_SUB or I_MUL) on registers holding normalized values of type t.
 * *ovf gets a vreg that is 1 when the true result doesn't fit in t. */
static int ovf_op(int o, Type *t, Opd a, Opd b, int *ovf) {
    if (a.k == O_IMM) a = R(iconst(a.v));
    int r = op2(o, C_I, a, b);
    if (t->size < 8) { /* the exact result fits in 64 bits: it overflowed iff narrowing changes it */
        *ovf = cmp(CC_NE, R(norm(r, t)), R(r));
        return r;
    }
    bool sg = is_signed(t);
    if (o == I_MUL) {
        int hi = op2(sg ? I_SMULH : I_UMULH, C_I, a, b.k == O_IMM ? R(iconst(b.v)) : b);
        *ovf = sg ? cmp(CC_NE, R(hi), R(op2(I_ASR, C_I, R(r), IMM(63)))) : cmp(CC_NE, R(hi), IMM(0));
        return r;
    }
    if (!sg) {
        *ovf = o == I_ADD ? cmp(CC_LO, R(r), a) : cmp(CC_LO, a, b);
        return r;
    }
    if (b.k == O_IMM) { /* a ± constant: it overflowed iff the result moved the wrong way */
        bool up = (o == I_ADD) == (b.v >= 0);
        *ovf = cmp(up ? CC_LT : CC_GT, R(r), a);
        return r;
    }
    /* signed: add overflows iff both operands' signs differ from the result's; sub iff a's sign
     * differs from b's and from the result's */
    int x = op2(I_XOR, C_I, a, R(r));
    int y = o == I_ADD ? op2(I_XOR, C_I, b, R(r)) : op2(I_XOR, C_I, a, b);
    *ovf = cmp(CC_LT, R(op2(I_AND, C_I, R(x), R(y))), IMM(0));
    return r;
}

/* wrap: +% -% *% (never checked) */
static Val arith(const char *op, Type *t, Val a, Val b, bool wrap) {
    int cls = cls_of(t);
    if (is_float(t)) {
        int ra = reg_of(a), rb = reg_of(b);
        if (!strcmp(op, "%")) return V_reg(t, callv("_fmod", C_F64, 2, R(ra), C_F64, R(rb), C_F64));
        int o = !strcmp(op, "+") ? I_FADD : !strcmp(op, "-") ? I_FSUB : !strcmp(op, "*") ? I_FMUL : I_FDIV;
        return V_reg(t, op2(o, cls, R(ra), R(rb)));
    }
    bool sg = is_signed(t);
    int o;
    bool need_norm = false, commut = false;
    if (!strcmp(op, "+")) o = I_ADD, need_norm = commut = true;
    else if (!strcmp(op, "-")) o = I_SUB, need_norm = true;
    else if (!strcmp(op, "*")) o = I_MUL, need_norm = commut = true;
    else if (!strcmp(op, "/")) o = sg ? I_SDIV : I_UDIV;
    else if (!strcmp(op, "%")) o = sg ? I_SREM : I_UREM;
    else if (!strcmp(op, "&")) o = I_AND, commut = true;
    else if (!strcmp(op, "|")) o = I_OR, commut = true;
    else if (!strcmp(op, "^")) o = I_XOR, commut = true;
    else if (!strcmp(op, "<<")) o = I_SHL, need_norm = true;
    else o = sg ? I_ASR : I_LSR;
    Opd oa = opd_of(a), ob = opd_of(b);
    if (oa.k == O_IMM && commut) {
        Opd tmp = oa;
        oa = ob;
        ob = tmp;
    }
    if (oa.k == O_IMM) oa = R(iconst(oa.v));
    if ((o == I_ADD || o == I_SUB || o == I_MUL) && !wrap && safe_arith()) {
        int ovf;
        int r = ovf_op(o, t, oa, ob, &ovf);
        panic_if(ovf, "integer overflow");
        return V_reg(t, r); /* it fit, so it's already normalized */
    }
    if (o == I_SDIV || o == I_UDIV || o == I_SREM || o == I_UREM) {
        if (ob.k != O_IMM) panic_if(cmp(CC_EQ, ob, IMM(0)), "division by zero"); /* in every mode */
        if (sg) {
            /* MIN / -1 overflows: a trap in safe modes; it wraps (to MIN) in ReleaseFast */
            if ((o == I_SDIV) && safe_arith() && (ob.k != O_IMM || ob.v == -1)) {
                int bad = cmp(CC_EQ, oa, IMM(int_min(t)));
                if (ob.k != O_IMM) bad = op2(I_AND, C_I, R(bad), R(cmp(CC_EQ, ob, IMM(-1))));
                panic_if(bad, "integer overflow");
            }
            need_norm = o == I_SDIV; /* a narrow MIN / -1 must wrap back into range */
        }
    }
    if (o == I_SHL || o == I_ASR || o == I_LSR) {
        int bits = t->size * 8;
        if (ob.k == O_IMM) {
            /* constant amounts are checked by binop */
        } else if (safe_arith()) {
            panic_if(cmp(CC_HS, ob, IMM(bits)), "shift amount too large");
        } else if (bits < 64) {
            ob = R(op2(I_AND, C_I, ob, IMM(bits - 1))); /* ReleaseFast: the amount is taken mod the width */
        }
    }
    int r = op2(o, C_I, oa, ob);
    return V_reg(t, need_norm ? norm(r, t) : r);
}

static int sel(int c, Opd x, Opd y) { /* c ? x : y */
    if (x.k == O_IMM) x = R(iconst(x.v));
    if (y.k == O_IMM) y = R(iconst(y.v));
    int r = new_vreg(C_I);
    Inst *i = emit(I_SEL);
    i->dst = r;
    i->cls = C_I;
    i->a = R(c);
    i->b = x;
    i->c = y;
    return r;
}

static int arith_opcode(char op) { return op == '+' ? I_ADD : op == '-' ? I_SUB : I_MUL; }

/* a +| b, a -| b, a *| b: clamp to the type's range instead of overflowing */
static Val saturate(char op, Type *t, Val a, Val b) {
    int ovf, o = arith_opcode(op);
    Opd oa = opd_of(a), ob = opd_of(b);
    if (oa.k == O_IMM) oa = R(iconst(oa.v));
    int r = ovf_op(o, t, oa, ob, &ovf);
    Opd lim;
    if (!is_signed(t)) {
        lim = IMM(o == I_SUB ? 0 : int_max(t));
    } else {
        /* the true result's sign: a's for + and -, a^b's for * */
        int neg = cmp(CC_LT, o == I_MUL ? R(op2(I_XOR, C_I, oa, ob)) : oa, IMM(0));
        lim = R(sel(neg, IMM(int_min(t)), IMM(int_max(t))));
    }
    return V_reg(t, sel(ovf, lim, R(r)));
}

/* SIMD: one instruction for the whole vector (the operands live in memory, like other aggregates) */
static int64_t vlanes(Type *vt) {
    Type *e = vt->elem;
    return VLANES(e->size, is_float(e), is_signed(e) && !is_float(e), vt->size);
}

static Val vec_op(int vop, Type *vt, Opd b, Opd c, int cls) {
    Addr r = temp(vt);
    int ra = addr_reg(r);
    Inst *in = emit(I_VEC);
    in->mem = vop;
    in->a = R(ra);
    in->b = b;
    in->c = c;
    in->cls = cls;
    in->imm = vlanes(vt);
    return V_agg(vt, r, true);
}

static Val vec_binop(const char *op, Val a, Val b, Token *tok) {
    Type *vt = is_vector(a.t) ? a.t : b.t;
    if (a.t != b.t)
        error_at(tok, "'%s' needs two %s, found %s and %s (to use one number for every element, write @splat(x))", op, tname(vt),
                 tname(a.t), tname(b.t));
    bool fl = is_float(vt->elem);
    int vop;
    if (!strcmp(op, "+") || !strcmp(op, "+%")) vop = VOP_ADD;
    else if (!strcmp(op, "-") || !strcmp(op, "-%")) vop = VOP_SUB;
    else if (!strcmp(op, "*") || !strcmp(op, "*%")) vop = VOP_MUL;
    else if (!strcmp(op, "/")) vop = VOP_DIV;
    else if (!strcmp(op, "&")) vop = VOP_AND;
    else if (!strcmp(op, "|")) vop = VOP_OR;
    else if (!strcmp(op, "^")) vop = VOP_XOR;
    else error_at(tok, "'%s' doesn't work on vectors (they have + - * / & | ^, element by element)", op);
    if (op[1] == '%' && fl) error_at(tok, "'%s' needs integers", op);
    if (vop == VOP_DIV && !fl) error_at(tok, "integer vectors have no '/': ARM64 has no SIMD integer division (divide the elements one by one)");
    if (vop >= VOP_AND && fl) error_at(tok, "'%s' needs integer vectors, not %s", op, tname(vt));
    if (vop == VOP_MUL && !fl && vt->elem->size == 8) error_at(tok, "ARM64 has no SIMD multiply for 64-bit integers (%s)", tname(vt));
    int pa = addr_reg(a.a), pb = addr_reg(b.a);
    return vec_op(vop, vt, R(pa), R(pb), C_I);
}

static Val binop(const char *op, Val a, Val b, Token *tok) {
    char wop[2] = {0, 0};
    if (is_vector(a.t) || is_vector(b.t)) return vec_binop(op, a, b, tok);
    if (a.t == ty_type || b.t == ty_type) { /* types compare by identity, at compile time */
        if (a.t != b.t || (strcmp(op, "==") && strcmp(op, "!=")))
            error_at(tok, "'%s' can't be used on a type%s", op, a.t != b.t ? " and a value" : "; types only compare with == and !=");
        if (!a.isconst || !b.isconst) error_at(tok, "internal error: a type that isn't known at compile time");
        return V_int(ty_bool, (a.ival == b.ival) == (op[0] == '='));
    }
    if (op[0] && strchr("+-*", op[0]) && op[1] == '%') { /* +% -% *%: wrapping (what the hardware does) */
        if (!is_int(a.t) || !is_int(b.t)) error_at(tok, "'%s' needs integers", op);
        wop[0] = op[0];
        op = wop;
    } else if (op[0] && strchr("+-*", op[0]) && op[1] == '|') { /* +| -| *|: saturating, in every mode */
        if (!is_int(a.t) || !is_int(b.t)) error_at(tok, "'%s' needs integers", op);
        char sop[2] = {op[0], 0};
        if (is_lit(a.t) && is_lit(b.t)) return fold(sop, a, b, tok);
        Type *t = unify(a.t, b.t, tok, op);
        if (is_signed(a.t) != is_signed(b.t) && !is_lit(a.t) && !is_lit(b.t))
            error_at(tok, "operator '%s' can't mix %s and %s", op, tname(a.t), tname(b.t));
        a = coerce(a, t, tok, "operand");
        b = coerce(b, t, tok, "operand");
        return saturate(op[0], t, a, b);
    }
    if (is_lit(a.t) && is_lit(b.t)) return fold(op, a, b, tok);
    if (is_cmp_op(op)) {
        if (is_slice(a.t) || is_slice(b.t)) {
            if (is_bytes(a.t) && is_bytes(b.t))
                error_at(tok, "'%s' doesn't compare strings (that's a loop over the bytes); use %s", op,
                         (!strcmp(op, "==") || !strcmp(op, "!=")) ? "mem.eql(u8, a, b)" : "mem.order(u8, a, b)");
            error_at(tok, "can't compare slices with '%s'; compare their elements (mem.eql compares contents)", op);
        }
        if ((is_optptr(a.t) || is_ptr(a.t)) && (is_optptr(b.t) || is_ptr(b.t)) && (a.t->k == TY_OPT || b.t->k == TY_OPT) &&
            (!strcmp(op, "==") || !strcmp(op, "!="))) { /* ?*T == ?*T (or *T): null is address 0, so compare the addresses */
            Type *ea = a.t->k == TY_OPT ? a.t->elem : a.t, *eb = b.t->k == TY_OPT ? b.t->elem : b.t;
            if (ea->elem != eb->elem && ea->elem != ty_void && eb->elem != ty_void)
                error_at(tok, "can't compare %s with %s", tname(a.t), tname(b.t));
            return V_reg(ty_bool, cmp(cc_for(op, false, false), opd_of(a), opd_of(b)));
        }
        if (a.t->k == TY_OPT || b.t->k == TY_OPT) {
            bool an = a.t == ty_null, bn = b.t == ty_null;
            if (!(an || bn) || (strcmp(op, "==") && strcmp(op, "!=")))
                error_at(tok, "an optional can only be compared with null (== or !=), or with another optional pointer; unwrap it to compare its value");
            int h = opt_has(an ? b : a);
            if (!strcmp(op, "==")) h = op2(I_XOR, C_I, R(h), IMM(1));
            return V_reg(ty_bool, h);
        }
        if (is_fn(a.t) || is_fn(b.t)) {
            if (a.t == ty_null || b.t == ty_null)
                error_at(tok, "a %s is never null; only optional functions (?fn(...)) can be", tname(is_fn(a.t) ? a.t : b.t));
            if (a.t != b.t) error_at(tok, "can't compare %s with %s", tname(a.t), tname(b.t));
            if (strcmp(op, "==") && strcmp(op, "!=")) error_at(tok, "functions only support == and !=");
            return V_reg(ty_bool, cmp(cc_for(op, false, false), opd_of(a), opd_of(b)));
        }
        if ((is_ptr(a.t) && b.t == ty_null) || (is_ptr(b.t) && a.t == ty_null))
            error_at(tok, "a %s is never null; only optional pointers (?*T) can be", tname(is_ptr(a.t) ? a.t : b.t));
        if ((is_ptr(a.t) || a.t == ty_null) && (is_ptr(b.t) || b.t == ty_null)) {
            if (a.t->elem != b.t->elem && a.t != ty_null && b.t != ty_null && a.t->elem != ty_void && b.t->elem != ty_void)
                error_at(tok, "can't compare %s with %s", tname(a.t), tname(b.t));
            return V_reg(ty_bool, cmp(cc_for(op, false, false), opd_of(a), opd_of(b)));
        }
        if (a.t->k == TY_ERRSET && b.t->k == TY_ERRSET) { /* e == error.NotFound */
            if (strcmp(op, "==") && strcmp(op, "!=")) error_at(tok, "errors only support == and !=");
            if (a.t == ty_errlit && !set_has(b.t, a.ival)) error_at(tok, "%s has no error %s", tname(b.t), M->errnames[a.ival]);
            if (b.t == ty_errlit && !set_has(a.t, b.ival)) error_at(tok, "%s has no error %s", tname(a.t), M->errnames[b.ival]);
            return V_reg(ty_bool, cmp(cc_for(op, false, false), opd_of(a), opd_of(b)));
        }
        if (a.t->k == TY_ENUM || b.t->k == TY_ENUM) {
            if (a.t != b.t) error_at(tok, "can't compare %s with %s", tname(a.t), tname(b.t));
            if (a.t->has_payload) error_at(tok, "can't compare %s values with '%s'; use switch to look at them", tname(a.t), op);
            if (strcmp(op, "==") && strcmp(op, "!=")) error_at(tok, "enums only support == and !=");
            return V_reg(ty_bool, cmp(cc_for(op, false, false), opd_of(a), opd_of(b)));
        }
        if (a.t == ty_bool && b.t == ty_bool && (!strcmp(op, "==") || !strcmp(op, "!=")))
            return V_reg(ty_bool, cmp(cc_for(op, false, false), opd_of(a), opd_of(b)));
        if (!is_num(a.t) || !is_num(b.t)) error_at(tok, "can't compare %s with %s", tname(a.t), tname(b.t));
        if (is_int(a.t) && is_int(b.t) && !is_lit(a.t) && !is_lit(b.t) && is_signed(a.t) != is_signed(b.t))
            error_at(tok, "comparing signed and unsigned integers (%s, %s); convert one with 'as'", tname(a.t), tname(b.t));
        Type *t = unify(a.t, b.t, tok, op);
        a = coerce(a, t, tok, "operand");
        b = coerce(b, t, tok, "operand");
        if (is_float(t)) return V_reg(ty_bool, fcmp(cc_for(op, true, true), reg_of(a), reg_of(b)));
        return V_reg(ty_bool, cmp(cc_for(op, is_signed(t), false), opd_of(a), opd_of(b)));
    }
    bool add_sub = !strcmp(op, "+") || !strcmp(op, "-");
    if (add_sub && is_ptr(a.t) && is_int(b.t)) {
        if (a.t->elem == ty_void) error_at(tok, "can't do arithmetic on *void");
        if (!a.t->many) error_at(tok, "pointer arithmetic needs a pointer to many items ([*]%s), not %s", tname(a.t->elem), tname(a.t));
        if (is_signed(b.t) && !is_lit(b.t)) error_at(tok, "a pointer offset must be usize, found %s; convert it with 'as usize'", tname(b.t));
        require_unsafe(tok, "pointer arithmetic");
        Val idx = convert(defaulted(b), ty_i64);
        if (!strcmp(op, "-")) {
            if (idx.isconst) idx.ival = -idx.ival;
            else idx = V_reg(ty_i64, op1(I_NEG, C_I, R(idx.reg)));
        }
        Addr ad = elem_addr(reg_of(a), idx, a.t->elem->size);
        return V_reg(a.t, addr_reg(ad));
    }
    if (!strcmp(op, "-") && is_ptr(a.t) && is_ptr(b.t) && a.t->elem == b.t->elem) {
        if (!a.t->many || !b.t->many) error_at(tok, "subtracting pointers needs pointers to many items ([*]T)");
        require_unsafe(tok, "pointer arithmetic");
        int d = op2(I_SUB, C_I, R(reg_of(a)), R(reg_of(b)));
        int es = a.t->elem->size ? a.t->elem->size : 1;
        if (es > 1) d = op2(I_SDIV, C_I, R(d), R(iconst(es)));
        return V_reg(ty_isize, d);
    }
    if (!is_num(a.t) || !is_num(b.t)) {
        Type *bad = is_num(a.t) ? b.t : a.t;
        error_at(tok, "operator '%s' needs numbers, found %s%s", op, tname(bad),
                 (is_bytes(bad) && !strcmp(op, "+"))
                     ? " ('+' doesn't join strings, since that would allocate: use fmt.allocPrint(alloc, \"{s}{s}\", .{ a, b }) or fmt.bufPrint)"
                     : "");
    }
    if (!strcmp(op, "<<") || !strcmp(op, ">>")) {
        if (!is_int(a.t) || !is_int(b.t)) error_at(tok, "'%s' needs integers", op);
        if (is_signed(b.t) && !is_lit(b.t))
            error_at(tok, "a shift amount must be unsigned, found %s; convert it with 'as u32'", tname(b.t));
        Type *t = default_t(a.t);
        a = coerce(a, t, tok, "operand");
        if (b.isconst && (b.ival < 0 || b.ival >= t->size * 8))
            error_at(tok, "shift amount %lld is out of range for %s (0 to %d)", (long long)b.ival, tname(t), t->size * 8 - 1);
        b = convert(defaulted(b), ty_i64);
        return arith(op, t, a, b, false);
    }
    Type *t = unify(a.t, b.t, tok, op);
    if ((!strcmp(op, "&") || !strcmp(op, "|") || !strcmp(op, "^")) && !is_int(t))
        error_at(tok, "'%s' needs integers", op);
    a = coerce(a, t, tok, "operand");
    b = coerce(b, t, tok, "operand");
    if (is_lit(t)) return fold(op, a, b, tok);
    return arith(op, t, a, b, wop[0] != 0);
}

static Val bool_from_cond(Node *n) {
    int r = new_vreg(C_I);
    int tb = new_block(), fb = new_block(), join = new_block();
    cond(n, tb, fb);
    set_blk(tb);
    mov(r, C_I, IMM(1));
    jmp(join);
    set_blk(fb);
    mov(r, C_I, IMM(0));
    jmp(join);
    set_blk(join);
    return V_reg(ty_bool, r);
}

static Val ex_binary(Node *n, Type *want) {
    const char *op = n->name;
    if (!strcmp(op, "&&") || !strcmp(op, "||")) return bool_from_cond(n);
    if (!strcmp(op, "+")) {
        Node *ops[256];
        int k = 0;
        Node *x = n;
        while (x->k == N_BINARY && !strcmp(x->name, "+") && k < 256) {
            ops[k++] = x;
            x = x->a;
        }
        Val acc = expr(x, want);
        for (int i = k - 1; i >= 0; i--) {
            Val b = expr(ops[i]->b, is_lit(acc.t) ? NULL : acc.t);
            acc = binop("+", acc, b, ops[i]->tok);
        }
        return acc;
    }
    bool c = is_cmp_op(op);
    Val a = expr(n->a, c ? NULL : want);
    Val b = expr(n->b, is_lit(a.t) ? NULL : a.t);
    return binop(op, a, b, n->tok);
}

static Val ex_unary(Node *n, Type *want) {
    const char *op = n->name;
    if (!strcmp(op, "&")) {
        if (n->a->k == N_NAME && find_var(n->a->name) && (find_var(n->a->name)->kind == VK_CONST || find_var(n->a->name)->kind == VK_CSTR))
            error_at(n->a->tok, "'%s' is a compile-time constant and has no address; give it a type to make it a variable", n->a->name);
        if (n->a->k == N_ARRAYLIT || n->a->k == N_STRUCTLIT) { /* &[_]i32 { 1, 2 }: a read-only stack value */
            Val v = expr(n->a, NULL);
            return V_reg(ptr_to_q(v.t, true), addr_reg(v.a));
        }
        if (!is_lvalue_node(n->a)) error_at(n->tok, "can't take the address of a temporary value");
        LV l = lval(n->a);
        if (l.temp) error_at(n->tok, "can't take the address of a temporary value");
        if (l.bits) error_at(n->tok, "can't take the address of a packed struct field: it isn't at a byte address");
        union_read(l);
        return V_reg(ptr_to_q(l.t, l.ro), addr_reg(l.a)); /* &let_binding is *const T */
    }
    Val v = expr(n->a, want);
    if (!strcmp(op, "-") && is_vector(v.t)) { /* each element negated (wrapping, like all vector integer math) */
        if (!is_float(v.t->elem) && !is_signed(v.t->elem)) error_at(n->tok, "can't negate %s: its elements are unsigned", tname(v.t));
        int pa = addr_reg(v.a);
        return vec_op(VOP_NEG, v.t, R(pa), (Opd){O_NONE, 0}, C_I);
    }
    if (!strcmp(op, "-")) {
        if (!is_num(v.t)) error_at(n->tok, "unary '-' needs a number, found %s", tname(v.t));
        if (v.isconst && is_float(v.t)) return V_flt(v.t, -v.fval);
        if (is_int(v.t) && !is_signed(v.t))
            error_at(n->tok, "can't negate an unsigned %s; write '0 -%% x' to wrap on purpose", tname(v.t));
        if (v.isconst && is_lit(v.t)) return V_int(v.t, -v.ival);
        if (is_float(v.t)) return V_reg(v.t, op1(I_FNEG, cls_of(v.t), R(reg_of(v))));
        if (safe_arith()) { /* only MIN overflows */
            int r = reg_of(v);
            panic_if(cmp(CC_EQ, R(r), IMM(int_min(v.t))), "integer overflow");
            return V_reg(v.t, op1(I_NEG, C_I, R(r)));
        }
        return V_reg(v.t, norm(op1(I_NEG, C_I, R(reg_of(v))), v.t));
    }
    if (!strcmp(op, "!")) {
        if (v.t != ty_bool) error_at(n->tok, "'!' needs a bool, found %s", tname(v.t));
        if (v.isconst) return V_int(ty_bool, !v.ival);
        return V_reg(ty_bool, op2(I_XOR, C_I, R(v.reg), IMM(1)));
    }
    /* ~ */
    if (!is_int(v.t)) error_at(n->tok, "'~' needs an integer, found %s", tname(v.t));
    if (v.isconst) return V_int(v.t, is_lit(v.t) ? ~v.ival : trunc_ext(~v.ival, v.t));
    return V_reg(v.t, norm(op1(I_NOT, C_I, R(v.reg)), v.t));
}

static void panic_msg(const char *msg) {
    Val m = str_lit(msg, (int)strlen(msg));
    callv("_rt_panic", -1, 2, R(load(ptr_to(ty_u8), m.a)), C_I, IMM((int64_t)strlen(msg)), C_I);
    trap();
}

/* x as Color: checked, so an enum variable always holds one of its variants */
static Val int_to_enum(Val v, Type *et, Token *tok) {
    if (et->has_payload) error_at(tok, "can't convert an integer to %s: its variants carry payloads", tname(et));
    layout(et, tok);
    if (v.isconst) {
        for (int i = 0; i < et->nvariants; i++)
            if (et->variants[i].value == v.ival) return V_int(et, v.ival);
        error_at(tok, "%lld isn't a value of %s", (long long)v.ival, tname(et));
    }
    int64_t lo = et->variants[0].value, hi = lo;
    for (int i = 1; i < et->nvariants; i++) {
        if (et->variants[i].value < lo) lo = et->variants[i].value;
        if (et->variants[i].value > hi) hi = et->variants[i].value;
    }
    int r = reg_of(v);
    int ok = new_block(), fail = new_cold_block();
    if ((uint64_t)(hi - lo) + 1 == (uint64_t)et->nvariants) { /* dense: one unsigned compare */
        int d = lo ? op2(I_SUB, C_I, R(r), IMM(lo)) : r;
        br(R(cmp(CC_LS, R(d), IMM(hi - lo))), ok, fail);
    } else {
        for (int i = 0; i < et->nvariants; i++) {
            int nx = new_block();
            br(R(cmp(CC_EQ, R(r), IMM(et->variants[i].value))), ok, nx);
            set_blk(nx);
        }
        jmp(fail);
    }
    set_blk(fail);
    panic_msg("integer doesn't match any enum value in 'as'");
    set_blk(ok);
    return V_reg(et, r);
}

/* `x as T` for numbers: a value that doesn't fit traps, in every mode and even inside unsafe (section 6.2) */
static Val checked_num(Val v, Type *to, Token *tok) {
    Type *from = v.t;
    if (from == to || !is_int(to) || from->k == TY_BOOL) return convert(v, to);
    const char *msg = strf("value doesn't fit in %s in 'as'", tname(to));
    if (v.isconst) {
        if (is_float(from)) {
            double d = v.fval, lo = is_signed(to) ? (double)int_min(to) : 0, hi = is_signed(to) ? (double)int_max(to) : (double)(uint64_t)int_max(to);
            if (!(d > lo - 1 && d < hi + 1) || (to->size == 8 && !(d >= lo && d < hi)))
                error_at(tok, "%g doesn't fit in %s", d, tname(to));
        } else {
            __int128 x = is_lit(from) ? (__int128)v.ival : (is_signed(from) ? (__int128)v.ival : (__int128)(uint64_t)v.ival);
            __int128 lo = int_min(to), hi = is_signed(to) ? (__int128)int_max(to) : (__int128)(uint64_t)int_max(to);
            if (x < lo || x > hi) {
                char buf[48];
                snprintf(buf, sizeof buf, is_signed(from) || is_lit(from) ? "%lld" : "%llu", (long long)v.ival);
                error_at(tok, "%s doesn't fit in %s", buf, tname(to));
            }
        }
        return convert(v, to);
    }
    if (is_float(from)) { /* in range and not NaN: both compares are false for NaN */
        int r = reg_of(v);
        if (from->k == TY_F32) r = op1(I_FCVT, C_F64, R(r));
        int bits = to->size * 8;
        double lo = is_signed(to) ? -ldexp(1, bits - 1) : -1, hi = ldexp(1, bits - is_signed(to));
        int above = is_signed(to) && bits == 64 ? fcmp(CC_GE, r, fconst(C_F64, lo)) : fcmp(CC_GT, r, fconst(C_F64, lo));
        int below = fcmp(CC_MI, r, fconst(C_F64, hi));
        panic_if(cmp(CC_EQ, R(op2(I_AND, C_I, R(above), R(below))), IMM(0)), msg);
        return convert(v, to);
    }
    if (widens(from, to) || (from->size == to->size && is_signed(from) == is_signed(to))) return convert(v, to);
    int r = reg_of(v);
    if (is_signed(from) && !is_signed(to) && to->size >= from->size) {
        panic_if(cmp(CC_LT, R(r), IMM(0)), msg); /* only negatives don't fit */
    } else if (is_signed(from) && is_signed(to)) {
        panic_if(cmp(CC_NE, R(norm(r, to)), R(r)), msg); /* narrowing: it fits iff sign-extending changes nothing */
    } else { /* as an unsigned number (negatives become huge) it must be <= the max */
        panic_if(cmp(CC_HI, R(r), IMM(int_max(to))), msg);
    }
    return convert(v, to);
}

static Val ex_cast(Node *n) {
    Type *to = resolve(n->b);
    Val v = expr(n->a, is_num(to) ? to : NULL);
    Type *t = v.t;
    if (is_slice(t) && is_slice(to) && t->elem == to->elem) {
        if (t->is_const && !to->is_const) error_at(n->tok, "'as' can't remove const: %s to %s", tname(t), tname(to));
        return coerce(v, to, n->tok, "'as'");
    }
    if (is_plain_enum(t) && is_int(to)) {
        Val w = v;
        w.t = t->tagt;
        return checked_num(w, to, n->tok);
    }
    if (is_int(t) && to->k == TY_ENUM) return int_to_enum(v, to, n->tok);
    if ((is_ptr(t) || is_optptr(t) || is_fn(t)) && is_int(to))
        error_at(n->tok, "'as' converts numbers; use '@ptrToInt(p)' for a pointer's address");
    if (is_int(t) && (is_ptr(to) || is_optptr(to)))
        error_at(n->tok, "'as' converts numbers; turning an integer into a pointer is '@intToPtr(%s, n)' inside an 'unsafe' block", tname(to));
    if ((is_ptr(t) || is_optptr(t)) && (is_ptr(to) || is_optptr(to))) {
        Val c = v;
        Type *want = to;
        if (t->k != TY_OPT && to->k == TY_OPT) want = to->elem;
        if (!(is_ptr(want) && is_ptr(t) && want->elem == ty_void) && !(t->k == TY_OPT && want->k == TY_OPT && want->elem->elem == ty_void) &&
            !(t->elem == want->elem || (t->k == TY_OPT && t->elem->elem == want->elem->elem)))
            error_at(n->tok, "'as' doesn't reinterpret pointers; use '@ptrCast(%s, p)' inside an 'unsafe' block", tname(to));
        return coerce(c, to, n->tok, "'as'");
    }
    bool ok = (is_num(t) && is_num(to)) || (t == ty_bool && is_int(to)) || t == to;
    if (!ok) error_at(n->tok, "can't convert %s to %s", tname(t), tname(to));
    return checked_num(v, to, n->tok);
}

/* the subject of `if (x) |v| ... else |err| ...`: an optional or an error union.
 * With |*v| (loc != NULL) the subject must be a variable: loc gets where it lives. */
static Val cap_subject_at(Node *n, const char *capture, const char *ecapture, LV *loc) {
    Val v;
    if (loc) {
        if (!is_lvalue_node(n)) error_at(n->tok, "|*%s| points into the value, so it needs a variable, not a temporary", capture);
        *loc = lval(n);
        if (loc->temp) error_at(n->tok, "|*%s| points into the value, so it needs a variable, not a temporary", capture);
        v = load_val(loc->t, loc->a);
    } else {
        v = expr(n, NULL);
    }
    if (v.t->k == TY_OPT) {
        if (ecapture) error_at(n->tok, "'else |%s|' captures an error, but this is an optional (%s)", ecapture, tname(v.t));
        return v;
    }
    if (v.t->k == TY_ERRUNION) {
        if (capture && v.t->elem == ty_void && strcmp(capture, "_"))
            error_at(n->tok, "there's no value to capture in |%s|: this is %s (write 'if (x) { ... } else |err| { ... }')", capture, tname(v.t));
        return v;
    }
    if (capture) error_at(n->tok, "'|%s|' unwraps an optional (?T) or error union (!T), but this is %s", capture, tname(v.t));
    error_at(n->tok, "'else |%s|' captures an error, but this is %s", ecapture, tname(v.t));
}

static Val cap_subject(Node *n, const char *capture, const char *ecapture) {
    return cap_subject_at(n, capture, ecapture, NULL);
}

static Var *new_local(const char *name, Type *t, Token *tok, bool is_const);

/* |*v|: v points at the value inside the optional / error union (which sits at offset 0) */
static void bind_ptr(const char *name, Token *tok, LV loc) {
    if (!strcmp(name, "_")) return;
    Type *et = loc.t->elem;
    if (et == ty_void) error_at(tok, "there's no value to point at in |*%s|: this is %s", name, tname(loc.t));
    Type *pt = ptr_to_q(et, loc.ro);
    Var *v = new_local(name, pt, tok, true);
    store(pt, A_slot(v->slot, 0), R(addr_reg(loc.a)));
}

/* 1 when the subject holds a value */
static int cap_ok(Val v) {
    if (v.t->k == TY_OPT) return opt_has(v);
    return cmp(CC_EQ, eu_err(v), IMM(0));
}

static void bind_err(const char *name, Token *tok, Val v);

static void bind_opt(const char *name, Token *tok, Val ov);

static bool comptime_cond(Node *c);
static void check_names(Node *n);
static Val ex_ternary(Node *n, Type *want) {
    if (n->a->k == N_COMPTIME && !n->name && !n->name2) { /* if (comptime c) a else b */
        bool take = comptime_cond(n->a);
        check_names(take ? n->c : n->b);
        return expr(take ? n->b : n->c, want);
    }
    int ta = new_block(), tb = new_block(), join = new_block();
    Val ov = V_void();
    bool caps = n->name || n->name2;
    LV loc;
    if (caps) {
        ov = cap_subject_at(n->a, n->name, n->name2, n->flag2 ? &loc : NULL);
        br(R(cap_ok(ov)), ta, tb);
    } else {
        cond(n->a, ta, tb);
    }
    set_blk(ta);
    push_scope(false, -1, -1);
    if (n->name && n->flag2) bind_ptr(n->name, n->tok, loc);
    else if (n->name) bind_opt(n->name, n->tok, ov);
    Val a = expr(n->b, want);
    pop_scope();
    int ea = C->blk;
    set_blk(tb);
    push_scope(false, -1, -1);
    if (n->name2) bind_err(n->name2, n->tok, ov);
    Val b = expr(n->c, want ? want : (is_lit(a.t) ? NULL : a.t));
    pop_scope();
    int eb = C->blk;
    Type *t;
    if (a.t == ty_noreturn) t = b.t;
    else if (b.t == ty_noreturn) t = a.t;
    else if (a.t == b.t) t = a.t;
    else if (want && want->k == TY_OPT) t = want;
    else if (is_num(a.t) && is_num(b.t)) t = unify(a.t, b.t, n->tok, "?:");
    else if (a.t == ty_null && (is_ptr(b.t) || is_fn(b.t))) t = opt_of(b.t); /* null or a pointer: ?*T */
    else if (b.t == ty_null && (is_ptr(a.t) || is_fn(a.t))) t = opt_of(a.t);
    else if (b.t->k == TY_OPT && (b.t->elem == a.t || (is_lit(a.t) && is_num(b.t->elem)))) t = b.t; /* T or ?T */
    else if (a.t->k == TY_OPT && (a.t->elem == b.t || (is_lit(b.t) && is_num(a.t->elem)))) t = a.t;
    else if (a.t == ty_null && b.t->k == TY_OPT) t = b.t; /* null or ?T */
    else if (b.t == ty_null && a.t->k == TY_OPT) t = a.t;
    else if (is_slice(a.t) && is_slice(b.t) && a.t->elem == b.t->elem) t = slice_of_q(a.t->elem, true); /* []T and []const T */
    else if (is_ptr(a.t) && is_ptr(b.t) && !a.t->many && !b.t->many && a.t->elem == b.t->elem) t = ptr_to_q(a.t->elem, true);
    else error_at(n->tok, "both branches of ?: must have the same type (%s vs %s)", tname(a.t), tname(b.t));
    if (is_lit(t) && want && is_num(want) && !(t->k == TY_LITFLOAT && is_int(want))) t = want; /* let a: u8 = if (c) 'A' else 'a'; */
    t = default_t(t);
    if (t == ty_void || t == ty_noreturn) error_at(n->tok, "an if-expression's branches must produce values");
    if (is_agg(t)) {
        Addr res = temp(t);
        set_blk(ea);
        copy_agg(t, res, coerce(a, t, n->b->tok, "if-expression branch").a);
        jmp(join);
        set_blk(eb);
        copy_agg(t, res, coerce(b, t, n->c->tok, "if-expression branch").a);
        jmp(join);
        set_blk(join);
        return V_agg(t, res, true);
    }
    int r = new_vreg(cls_of(t));
    set_blk(ea);
    mov(r, cls_of(t), opd_of(coerce(a, t, n->b->tok, "?: branch")));
    jmp(join);
    set_blk(eb);
    mov(r, cls_of(t), opd_of(coerce(b, t, n->c->tok, "?: branch")));
    jmp(join);
    set_blk(join);
    return V_reg(t, r);
}

/* arr[lo..hi]: a slice pointing into the array at `ba` (nothing is copied) */
static Val slice_array(Node *n, Type *at, Addr ba, bool ro) {
    layout(at, n->tok);
    Val lo = n->b ? usize_val(n->b, "a slice bound") : V_int(ty_usize, 0);
    Val hi = n->c ? usize_val(n->c, "a slice bound") : V_int(ty_usize, at->alen);
    if (lo.isconst && hi.isconst) {
        if ((uint64_t)hi.ival > (uint64_t)at->alen)
            error_at(n->c->tok, "slice end %llu is out of bounds for %s", (unsigned long long)hi.ival, tname(at));
        if ((uint64_t)lo.ival > (uint64_t)hi.ival)
            error_at(n->tok, "slice start %llu is after its end %llu", (unsigned long long)lo.ival, (unsigned long long)hi.ival);
    } else if (bounds_checked()) {
        callv("_rt_range", -1, 3, opd_of(lo), C_I, opd_of(hi), C_I, IMM(at->alen), C_I);
    }
    Type *st = slice_of_q(at->elem, ro);
    Addr t = temp(st);
    store(ptr_to(at->elem), t, R(addr_reg(elem_addr(addr_reg(ba), lo, at->elem->size))));
    if (lo.isconst && hi.isconst) store(ty_usize, A_add(t, 8), IMM(hi.ival - lo.ival));
    else store(ty_usize, A_add(t, 8), R(op2(I_SUB, C_I, R(reg_of(hi)), opd_of(lo))));
    return V_agg(st, t, true);
}

static Val ex_slice(Node *n) {
    Val base;
    if (is_lvalue_node(n->a)) { /* an array is sliced where it lives */
        LV l = lval(n->a);
        if (is_array(l.t)) {
            if (l.temp) error_at(n->tok, "can't slice a temporary array; store it in a variable first");
            return slice_array(n, l.t, l.a, l.ro);
        }
        union_read(l);
        base = load_val(l.t, l.a);
    } else {
        base = expr(n->a, NULL);
    }
    Type *bt = base.t;
    if (is_array(bt)) error_at(n->tok, "can't slice a temporary array; store it in a variable first");
    if (is_array_ptr(bt)) return slice_array(n, bt->elem, A_reg(reg_of(base), 0), bt->is_const);
    Val lo = n->b ? usize_val(n->b, "a slice bound") : V_int(ty_usize, 0);
    if (is_ptr(bt)) {
        if (!bt->many) error_at(n->tok, "can't slice %s: it points at a single item (use [*]T)", tname(bt));
        if (!n->c) error_at(n->tok, "slicing a pointer needs an end: p[a..b]");
        require_unsafe(n->tok, strf("slicing %s (it has no length to check against)", tname(bt)));
        Val hi = usize_val(n->c, "a slice bound");
        Type *st = slice_of_q(bt->elem, bt->is_const);
        Addr t = temp(st);
        store(ptr_to(bt->elem), t, R(addr_reg(elem_addr(reg_of(base), lo, bt->elem->size))));
        store(ty_i64, A_add(t, 8), R(op2(I_SUB, C_I, opd_of(hi).k == O_IMM ? R(reg_of(hi)) : R(hi.reg), opd_of(lo))));
        return V_agg(st, t, true);
    }
    if (!is_slice(bt)) error_at(n->tok, "can't slice %s", tname(bt));
    Type *et = bt->elem;
    int ptr = load(ptr_to(et), base.a);
    int len = load(ty_i64, A_add(base.a, 8));
    Val hi = n->c ? usize_val(n->c, "a slice bound") : V_reg(ty_usize, len);
    if (bounds_checked()) callv("_rt_range", -1, 3, opd_of(lo), C_I, opd_of(hi), C_I, R(len), C_I);
    Addr t = temp(bt);
    store(ptr_to(et), t, R(addr_reg(elem_addr(ptr, lo, et->size))));
    store(ty_i64, A_add(t, 8), R(op2(I_SUB, C_I, R(reg_of(hi)), opd_of(lo))));
    return V_agg(bt, t, true);
}

static Val struct_init(Type *st, Node *n);

static Val ex_structlit(Node *n) {
    if (n->a) { /* List(i32) { ... } */
        Type *t = static_type(n->a);
        if (!t || t->k != TY_STRUCT) error_at(n->tok, "this isn't a struct type");
        return struct_init(t, n);
    }
    Type *t = find_struct(n->name);
    check_type_pub(t, n->tok);
    return struct_init(t, n);
}

/* the fields of n (N_FIELD nodes) fill in a value of struct type st */
static void field_init(Type *st, Field *fd, Addr base, Val v, Token *tok) {
    if (!st->packed) {
        store_val(fd->t, A_add(base, fd->off), v);
        return;
    }
    LV l;
    memset(&l, 0, sizeof l);
    l.t = fd->t;
    l.a = base;
    l.bits = fd->bits;
    l.bitoff = fd->bitoff;
    l.sbits = fd->sbits;
    l.backing = st->tagt;
    bits_store(l, v, tok);
}

static Val struct_init(Type *st, Node *n) {
    layout(st, n->tok);
    Addr t = temp(st);
    if (st->packed) zero_at(st, t);
    if (st->is_union && n->n != 1)
        error_at(n->tok, "a %s literal sets exactly one field (its fields share memory), but %d are given", st->name, n->n);
    bool *given = xalloc((size_t)st->nfields + 1);
    for (int i = 0; i < n->n; i++) {
        Node *f = n->list[i];
        int k = -1;
        for (int j = 0; j < st->nfields; j++)
            if (!strcmp(st->fields[j].name, f->name)) k = j;
        if (k < 0) error_at(f->tok, "struct '%s' has no field '%s'", st->name, f->name);
        if (given[k]) error_at(f->tok, "field '%s' given twice", f->name);
        given[k] = true;
        Field *fd = &st->fields[k];
        if (f->a->k == N_UNDEFINED) { /* filled in later */
            if (!st->packed) undefined_at(fd->t, A_add(t, fd->off));
            continue;
        }
        Val v = coerce(expr(f->a, fd->t), fd->t, f->a->tok, strf("field '%s'", f->name));
        field_init(st, fd, t, v, f->a->tok);
    }
    for (int j = 0; j < st->nfields && !st->is_union; j++) {
        if (given[j]) continue;
        Field *fd = &st->fields[j];
        if (!fd->def)
            error_at(n->tok, "missing field '%s' in %s literal (every field must be set, or have a default)", fd->name, st->name);
        if (fd->def->k == N_UNDEFINED) {
            if (!st->packed) undefined_at(fd->t, A_add(t, fd->off));
            continue;
        }
        TEnv *saved_te = TE;
        if (st->genv) TE = st->genv; /* a generic type's defaults see its own bindings */
        Val v = coerce(expr(fd->def, fd->t), fd->t, fd->def->tok, strf("default of field '%s'", fd->name));
        TE = saved_te;
        field_init(st, fd, t, v, fd->def->tok);
    }
    free(given);
    return V_agg(st, t, true);
}

/* [N]T { a, b, ... } (also .{ a, b } where an array is expected) */
static Val array_init(Type *at, Node **els, int n, Token *tok) {
    layout(at, tok);
    if (n != at->alen) error_at(tok, "%s needs %lld element(s), but %d %s given", tname(at), (long long)at->alen, n, n == 1 ? "is" : "are");
    Type *et = at->elem;
    Addr a = temp(at);
    for (int i = 0; i < n; i++)
        store_val(et, A_add(a, (int64_t)i * et->size), coerce(expr(els[i], et), et, els[i]->tok, "array element"));
    return V_agg(at, a, true);
}

/* the type of [N]T { ... } / [_]T { ... } */
static Type *arraylit_type(Node *n) {
    Node *ty = n->a;
    Type *et = resolve(ty->a);
    if (et == ty_void || et == ty_noreturn) error_at(ty->a->tok, "can't have an array of %s", tname(et));
    int64_t len = n->n;
    if (ty->c) {
        len = eval_const_int(ty->c, "an array length");
        if (len < 0) error_at(ty->c->tok, "an array length can't be negative (%lld)", (long long)len);
    }
    return array_of(et, len);
}

static Val ex_arraylit(Node *n) { return array_init(arraylit_type(n), n->list, n->n, n->tok); }

/* @splat(v): an array with every element set to v (its type comes from context) */
static Val ex_splat(Node *n, Type *want) {
    if (want && is_vector(want)) { /* one dup instruction */
        Type *et = want->elem;
        Val v = coerce(expr(n->list[0], et), et, n->list[0]->tok, "@splat value");
        Opd o = is_float(et) ? R(reg_of(v)) : opd_of(v);
        return vec_op(VOP_SPLAT, want, o, (Opd){O_NONE, 0}, cls_of(et));
    }
    if (!want || !is_array(want))
        error_at(n->tok, "can't tell which array @splat makes here; give it a type: 'let a: [N]T = @splat(v);'");
    Type *at = want, *et = at->elem;
    layout(at, n->tok);
    Val v = coerce(expr(n->list[0], et), et, n->list[0]->tok, "@splat value");
    Addr a = temp(at);
    bool zero = v.isconst && (is_float(et) ? v.fval == 0 && !signbit(v.fval) : v.ival == 0);
    if (zero) {
        zero_at(at, a);
    } else if (at->alen <= 8) {
        for (int64_t i = 0; i < at->alen; i++) store_val(et, A_add(a, i * et->size), v);
    } else if (at->alen > 0) { /* a counted loop */
        if (!is_agg(et)) v = V_reg(et, reg_of(v));
        int base = addr_reg(a);
        int islot = new_slot(ty_usize);
        store(ty_usize, A_slot(islot, 0), IMM(0));
        int body = new_block(), exit = new_block();
        jmp(body);
        set_blk(body);
        int i = load(ty_usize, A_slot(islot, 0));
        store_val(et, elem_addr(base, V_reg(ty_usize, i), et->size), v);
        int i2 = op2(I_ADD, C_I, R(i), IMM(1));
        store(ty_usize, A_slot(islot, 0), R(i2));
        br(R(cmp(CC_LO, R(i2), IMM(at->alen))), body, exit);
        set_blk(exit);
    }
    return V_agg(at, a, true);
}

/* .{ ... }: a struct, tuple or array literal whose type comes from context */
static Val ex_anon(Node *n, Type *want) {
    Type *t = want;
    while (t && (t->k == TY_OPT || t->k == TY_ERRUNION)) t = t->elem; /* ?T and E!T wrap the value */
    if (!t && !n->flag) { /* .{ a, b } with nothing to go by: a tuple of the elements' own types */
        Type **ts = xalloc(sizeof(Type *) * (size_t)(n->n + 1));
        Val *vs = xalloc(sizeof(Val) * (size_t)(n->n + 1));
        for (int i = 0; i < n->n; i++) {
            vs[i] = defaulted(expr(n->list[i], NULL));
            if (vs[i].t == ty_null || vs[i].t == ty_void || vs[i].t == ty_noreturn)
                error_at(n->list[i]->tok, "can't put %s in a tuple without a type", tname(vs[i].t));
            if (!is_agg(vs[i].t)) vs[i] = V_reg(vs[i].t, reg_of(vs[i]));
            ts[i] = vs[i].t;
        }
        Type *tt = tuple_of(ts, n->n);
        layout(tt, n->tok);
        Addr a = temp(tt);
        for (int i = 0; i < n->n; i++) store_val(ts[i], A_add(a, tt->fields[i].off), vs[i]);
        return V_agg(tt, a, true);
    }
    if (!t) error_at(n->tok, "can't tell the type of '.{ ... }' here; write the type: T { ... }");
    if (is_array(t)) {
        if (n->flag) error_at(n->tok, "an array literal lists its elements without names: .{ a, b }");
        return array_init(t, n->list, n->n, n->tok);
    }
    if (t->k == TY_STRUCT && t->tuple) {
        if (n->flag) error_at(n->tok, "a tuple literal lists its elements without names: .{ a, b }");
        layout(t, n->tok);
        if (n->n != t->nfields) error_at(n->tok, "%s has %d elements, but %d %s given", tname(t), t->nfields, n->n, n->n == 1 ? "is" : "are");
        Addr a = temp(t);
        for (int i = 0; i < n->n; i++) {
            Field *fd = &t->fields[i];
            store_val(fd->t, A_add(a, fd->off), coerce(expr(n->list[i], fd->t), fd->t, n->list[i]->tok, strf("element %d", i)));
        }
        return V_agg(t, a, true);
    }
    if (t->k == TY_STRUCT) {
        if (!n->flag && n->n) error_at(n->tok, "%s's fields need names: .{ field: value, ... }", tname(t));
        return struct_init(t, n);
    }
    error_at(n->tok, "a '.{ ... }' literal can't make %s", tname(t));
}

/* ---------- calls ---------- */

#define MAXARGS 40

/* vals: already-evaluated arguments (including self); toks: where each came from.
 * f->sym == NULL is a call through a function value: `callee` holds its address. */
static Val call_core_to(FnInfo *f, Val *vals, Token **toks, int given, Token *tok, Opd callee) {
    if (f->state == 0 && !f->ext) lower_fn(f);
    if (!f->ret) error_at(tok, "'%s' calls itself before its return type is known; add '-> type' to it", f->name);
    Opd A[MAXARGS + 8];
    int K[MAXARGS + 8];
    int n = 0, nfixed = 0, ic = 0, fc = 0;
    bool aggret = is_agg(f->ret);
    Addr ret = A_slot(-1, 0);
    if (aggret) {
        ret = temp(f->ret);
        A[n] = R(addr_reg(ret));
        K[n++] = C_I;
        ic++;
        nfixed = n; /* the hidden result pointer is a fixed argument, even with no parameters */
    }
    for (int i = 0; i < given; i++) {
        if (n >= MAXARGS) error_at(tok, "too many arguments");
        Val v = vals[i];
        Token *at = toks[i];
        if (i < f->np) {
            Type *pt = f->pt[i];
            v = coerce(v, pt, at, f->pn ? strf("argument '%s'", f->pn[i]) : strf("argument %d", i + 1));
            if (is_agg(pt)) {
                if (f->ext) error_at(at, "can't pass %s to an extern C function (pass .ptr and .len instead)", tname(pt));
                Leaf L[4];
                int k = leaves(pt, 0, L, 0, 4);
                if (k >= 0) {
                    for (int j = 0; j < k; j++) {
                        A[n] = R(load(L[j].t, A_add(v.a, L[j].off)));
                        K[n] = cls_of(L[j].t);
                        if (K[n] == C_I) ic++;
                        else fc++;
                        n++;
                    }
                } else {
                    Addr t = v.a;
                    if (!v.fresh) {
                        t = temp(pt);
                        copy_agg(pt, t, v.a);
                    }
                    A[n] = R(addr_reg(t));
                    K[n++] = C_I;
                    ic++;
                }
            } else {
                A[n] = opd_of(v);
                K[n] = cls_of(pt);
                if (K[n] == C_I) ic++;
                else fc++;
                n++;
            }
            nfixed = n;
        } else {
            v = defaulted(v);
            if (v.strlit) v = coerce(v, ty_cstr, at, "argument");
            if (is_agg(v.t)) error_at(at, "pass %s to C varargs as .ptr (and make sure it's NUL-terminated)", tname(v.t));
            if (v.t == ty_void) error_at(at, "this expression has no value");
            if (v.t->k == TY_F32) v = convert(v, ty_f64);
            A[n] = opd_of(v);
            K[n++] = cls_of(v.t);
        }
    }
    if ((ic > 8 || fc > 8) && f->ext)
        error_at(tok, "too many arguments for a C function (up to 8 integer and 8 float registers are supported)");
    if (!f->sym) { /* the callee goes last; it's not an argument */
        A[n] = callee;
        K[n++] = C_I;
    }
    Opd *args2 = xalloc(sizeof(Opd) * (size_t)(n ? n : 1));
    int *cls2 = xalloc(sizeof(int) * (size_t)(n ? n : 1));
    memcpy(args2, A, sizeof(Opd) * (size_t)n);
    memcpy(cls2, K, sizeof(int) * (size_t)n);
    int retcls = (f->ret == ty_void || f->ret == ty_noreturn || aggret) ? -1 : cls_of(f->ret);
    int r = emit_call(f->sym, args2, cls2, n, nfixed, retcls);
    if (f->ret == ty_noreturn) { /* it never comes back, like exit() */
        trap();
        return V_reg(ty_noreturn, -1);
    }
    if (aggret) return V_agg(f->ret, ret, true);
    if (f->ret == ty_void) return V_void();
    if (f->ext || !f->sym) r = norm(r, f->ret); /* C leaves the upper bits of narrow results unspecified */
    return V_reg(f->ret, r);
}

static Val call_core(FnInfo *f, Val *vals, Token **toks, int given, Token *tok) {
    return call_core_to(f, vals, toks, given, tok, IMM(0));
}

/* the function's address, as a value of its fn(...) type */
static Val fn_value(FnInfo *f, Token *tok) {
    if (f->recv) check_pub(f->unit, f->is_pub, strf("%s.%s", tname(f->recv), f->name), tok);
    if (f->type_fn) error_at(tok, "'%s' returns a type; call it with its arguments to name the type", f->name);
    if (f->generic) error_at(tok, "'%s' is generic (it has comptime or anytype parameters), so it has no single address", f->name);
    if (f->decl->is_unsafe) require_unsafe(tok, strf("using the unsafe fn '%s' as a value", f->name));
    if (f->state == 0 && !f->ext) lower_fn(f);
    if (!f->ret) error_at(tok, "'%s' is used before its return type is known; add '-> type' to it", f->name);
    int r = addr_sym_q(f->sym, f->ext); /* C functions live in another image: through the GOT */
    return V_reg(fn_type(f->pt, f->np, f->ret, f->variadic), r);
}

/* call through a value of type fn(...) */
static Val call_value(Val fv, Node **args, int nargs, Token *tok) {
    Type *ft = fv.t;
    if (ft->k == TY_OPT && is_fn(ft->elem)) opt_misuse(tok, ft);
    if (!is_fn(ft)) error_at(tok, "can't call %s: it isn't a function", tname(ft));
    int callee = reg_of(fv);
    FnInfo f;
    memset(&f, 0, sizeof f);
    f.name = "this function";
    f.pt = ft->params;
    f.np = ft->nparams;
    f.ret = ft->elem;
    f.variadic = ft->variadic;
    f.state = 2;
    if (nargs < f.np || (nargs > f.np && !f.variadic))
        error_at(tok, "this function takes %d argument(s), got %d", f.np, nargs);
    if (nargs > MAXARGS) error_at(tok, "too many arguments");
    Val vals[MAXARGS + 1];
    Token *toks[MAXARGS + 1];
    for (int i = 0; i < nargs; i++) {
        vals[i] = expr(args[i], i < f.np ? f.pt[i] : NULL);
        toks[i] = args[i]->tok;
    }
    return call_core_to(&f, vals, toks, nargs, tok, R(callee));
}


/* ---------- comptime: code that runs while compiling (design section 4.11) ----------
 * A comptime expression is lowered into a scratch function, which the interpreter (interp.c)
 * runs right away. A scalar result becomes a constant; anything else becomes read-only data:
 * a global holding the bytes, with the pointers in it turned into relocations (and the comptime
 * memory they point at turned into globals of their own). */

static void data_item(GlobalVar *g, int off, int size, int64_t val, const char *sym);
static int CT_DEPTH; /* > 0 while lowering code for the comptime interpreter */

static IRFunc *ct_lookup(const char *sym, const char **why) {
    for (int i = M->nfuncs - 1; i >= 0; i--)
        if (!strcmp(M->funcs[i]->sym, sym)) return M->funcs[i];
    for (int i = 0; i < nfns; i++) {
        FnInfo *f = fns[i];
        if (!f->sym || strcmp(f->sym, sym)) continue;
        if (f->ext) *why = "it's a C function, and there's no I/O or C at compile time";
        else if (f->state == 1) *why = strf("'%s' is still being compiled (a function can't run itself at compile time)", f->name);
        return NULL;
    }
    return NULL;
}

/* a comptime evaluation failed: point at what failed, then at the calls that led there */
static _Noreturn void ct_fail(CtError *e, Token *site) {
    Token *nt[8];
    const char *nm[8];
    int nn = 0;
    for (int i = 0; i < e->ncalls && nn < 4; i++) { /* recursion: each call site once */
        if (!e->calls[i] || e->calls[i] == e->tok) continue;
        bool dup = false;
        for (int k = 0; k < nn; k++) dup |= nt[k] == e->calls[i];
        if (dup) continue;
        nt[nn] = e->calls[i];
        nm[nn++] = strf("'%s' was called here", e->fns[i]);
    }
    Token *at = e->tok ? e->tok : site;
    if (at != site) {
        nt[nn] = site;
        nm[nn++] = "while running this at compile time";
    }
    error_notes(at, nn, nt, nm, "%s", e->msg);
}

/* ----- turning comptime memory into data ----- */

typedef struct {
    int64_t off;
    const char *sym;
    int64_t add;
} CtRel;

typedef struct {
    int g;          /* its global (index in M->globals) */
    uint8_t *bytes; /* a copy of the comptime memory */
    int64_t size;
    CtRel *rel;
    int nrel, crel;
    int region;     /* the comptime region it came from (-1: the result itself) */
} CtBuild;

typedef struct {
    int b;
    int64_t off;
    Type *t;
} CtSeen;

static CtBuild *ctb;
static int nctb, cctb;
static CtSeen *ctseen;
static int nctseen, cctseen;
static int ct_counter;

static bool has_ptrs(Type *t) {
    switch (t->k) {
    case TY_PTR: case TY_FN: case TY_SLICE: return true;
    case TY_OPT: case TY_ERRUNION: case TY_ARRAY: return has_ptrs(t->elem);
    case TY_STRUCT:
        if (t->packed || t->is_union) return false;
        for (int i = 0; i < t->nfields; i++)
            if (has_ptrs(t->fields[i].t)) return true;
        return false;
    case TY_ENUM:
        for (int i = 0; i < t->nvariants; i++)
            if (t->variants[i].payload && has_ptrs(t->variants[i].payload)) return true;
        return false;
    default: return false;
    }
}

static int ct_new_build(int region, const uint8_t *data, int64_t size, int align) {
    if (size > 0x7fffffff) error_at(CUR_TOK, "a value made at compile time is too big (%lld bytes)", (long long)size);
    GlobalVar g;
    memset(&g, 0, sizeof g);
    g.sym = strf("_fl_ct_%d", ++ct_counter);
    g.kind = GV_DATA;
    g.size = (int)(size ? size : 1);
    g.align = align;
    g.is_const = true;
    g.name = "a value made at compile time";
    PUSH(M->globals, M->nglobals, M->gcap, g);
    CtBuild b;
    memset(&b, 0, sizeof b);
    b.g = M->nglobals - 1;
    b.bytes = xalloc((size_t)(size ? size : 1));
    if (size && data) memcpy(b.bytes, data, (size_t)size);
    b.size = size;
    b.region = region;
    PUSH(ctb, nctb, cctb, b);
    return nctb - 1;
}

static void ct_walk(int bi, Type *t, int64_t off, Token *site);

/* the pointer stored at [build bi + off], which points at `count` items of type elem (NULL: don't look inside) */
static void ct_ptr(int bi, int64_t off, Type *elem, int64_t count, Token *site) {
    uint64_t p;
    memcpy(&p, ctb[bi].bytes + off, 8);
    if (!p) return;
    CtRegion r;
    int64_t roff;
    if (!ct_region(p, &r, &roff))
        error_at(site, "the value made at compile time points at memory that's gone (a local variable of a function that returned)");
    const char *sym = r.sym;
    if (r.kind == CR_FRAME || r.kind == CR_CONST) { /* comptime memory: it becomes data of its own */
        int tb = -1;
        for (int k = 0; k < nctb; k++)
            if (ctb[k].region == r.id) tb = k;
        if (tb < 0) tb = ct_new_build(r.id, r.data, r.size, 16);
        sym = M->globals[ctb[tb].g].sym;
        if (elem && elem->k != TY_VOID && count > 0 && has_ptrs(elem)) {
            layout(elem, site);
            for (int64_t i = 0; i < count; i++) {
                int64_t at = roff + i * elem->size;
                if (at + elem->size > ctb[tb].size) break;
                bool seen = false;
                for (int k = 0; k < nctseen && !seen; k++)
                    seen = ctseen[k].b == tb && ctseen[k].off == at && ctseen[k].t == elem;
                if (seen) continue;
                CtSeen s = {tb, at, elem};
                PUSH(ctseen, nctseen, cctseen, s);
                ct_walk(tb, elem, at, site);
            }
        }
    }
    CtRel rel = {off, sym, roff};
    PUSH(ctb[bi].rel, ctb[bi].nrel, ctb[bi].crel, rel);
}

/* find the pointers in a value of type t at [build bi + off] */
static void ct_walk(int bi, Type *t, int64_t off, Token *site) {
    uint8_t *b = ctb[bi].bytes;
    switch (t->k) {
    case TY_PTR: ct_ptr(bi, off, t->many ? NULL : t->elem, 1, site); return;
    case TY_FN: ct_ptr(bi, off, NULL, 0, site); return;
    case TY_SLICE: {
        int64_t len;
        memcpy(&len, b + off + 8, 8);
        ct_ptr(bi, off, t->elem, len, site);
        return;
    }
    case TY_OPT:
        if (is_optptr(t)) ct_walk(bi, t->elem, off, site);
        else if (b[off + t->elem->size]) ct_walk(bi, t->elem, off, site);
        return;
    case TY_ERRUNION: {
        if (t->elem == ty_void) return;
        uint16_t code;
        memcpy(&code, b + off + err_off(t), 2);
        if (!code) ct_walk(bi, t->elem, off, site);
        return;
    }
    case TY_ARRAY:
        if (!has_ptrs(t->elem)) return;
        for (int64_t i = 0; i < t->alen; i++) ct_walk(bi, t->elem, off + i * t->elem->size, site);
        return;
    case TY_STRUCT:
        if (t->packed || t->is_union) return;
        for (int i = 0; i < t->nfields; i++) ct_walk(bi, t->fields[i].t, off + t->fields[i].off, site);
        return;
    case TY_ENUM: {
        if (!t->has_payload) return;
        uint64_t tag = 0;
        memcpy(&tag, b + off, (size_t)t->tagt->size);
        for (int i = 0; i < t->nvariants; i++)
            if ((uint64_t)trunc_ext(t->variants[i].value, t->tagt) == (uint64_t)trunc_ext((int64_t)tag, t->tagt) && t->variants[i].payload)
                ct_walk(bi, t->variants[i].payload, off + t->payload_off, site);
        return;
    }
    default: return;
    }
}

static int cmp_rel(const void *a, const void *b) {
    const CtRel *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

/* the builds become the globals' data: relocations, and the other bytes in aligned pieces (zeros are gaps) */
static void ct_emit_builds(void) {
    for (int k = 0; k < nctb; k++) {
        CtBuild *bd = &ctb[k];
        qsort(bd->rel, (size_t)bd->nrel, sizeof(CtRel), cmp_rel);
        int ri = 0;
        int64_t o = 0;
        while (o < bd->size) {
            if (ri < bd->nrel && bd->rel[ri].off == o) {
                data_item(&M->globals[bd->g], (int)o, 8, bd->rel[ri].add, bd->rel[ri].sym);
                o += 8;
                ri++;
                continue;
            }
            int64_t lim = ri < bd->nrel ? bd->rel[ri].off : bd->size;
            int sz = 1;
            for (int c = 8; c > 1; c /= 2)
                if (o % c == 0 && o + c <= lim) {
                    sz = c;
                    break;
                }
            uint64_t v = 0;
            memcpy(&v, bd->bytes + o, (size_t)sz);
            if (v) data_item(&M->globals[bd->g], (int)o, sz, (int64_t)v, NULL);
            o += sz;
        }
        free(bd->bytes);
        free(bd->rel);
    }
    nctb = 0;
    nctseen = 0;
}

typedef struct {
    bool isconst;
    int64_t ival;
    double fval;
    int g; /* otherwise: the global holding it */
} CtOut;

static bool ptr_like(Type *t) { return t->k == TY_PTR || t->k == TY_FN || is_optptr(t); }

/* the value comptime code returned (its register's bits; for an aggregate, its address) */
static void ct_result(Type *t, uint64_t bits, Token *site, CtOut *out) {
    memset(out, 0, sizeof *out);
    if (!is_agg(t) && !ptr_like(t)) {
        out->isconst = true;
        if (is_float(t)) memcpy(&out->fval, &bits, 8);
        else if (t->k == TY_ENUM) out->ival = trunc_ext((int64_t)bits, t->tagt);
        else if (is_int(t)) out->ival = trunc_ext((int64_t)bits, t);
        else if (t == ty_bool) out->ival = bits & 1;
        else out->ival = (int64_t)bits;
        return;
    }
    int bi;
    if (ptr_like(t)) {
        bi = ct_new_build(-1, (uint8_t *)&bits, 8, 8);
        ct_walk(bi, t, 0, site);
    } else {
        layout(t, site);
        CtRegion r;
        int64_t off;
        if (!ct_region(bits, &r, &off) || off + t->size > r.size) error_at(site, "internal error: lost the comptime result");
        bi = ct_new_build(-1, r.data + off, t->size, t->align);
        ct_walk(bi, t, 0, site);
    }
    out->g = ctb[bi].g;
    ct_emit_builds();
}

static void block_body(Node *b);
static Var *new_local(const char *name, Type *t, Token *tok, bool is_const);

/* lower `e` (or, with blk, the block e) into a scratch function, run it, and collect its value */
static const char **CT_PRE_NAMES; /* locals ct_compute declares first (a `where` clause's run-time parameters) */
static Type **CT_PRE_TYPES;
static int CT_NPRE;

static Type *ct_compute(Node *e, bool blk, Type *want, const char *what, CtOut *out) {
    FnInfo dummy;
    memset(&dummy, 0, sizeof dummy);
    dummy.name = "comptime code";
    dummy.ret = ty_void;
    dummy.decl = e;
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    IRFunc *f = xalloc(sizeof *f);
    f->sym = "_fl_comptime";
    f->name = "comptime code";
    ctx.f = f;
    ctx.fn = &dummy;
    ctx.retptr = -1;
    ctx.ct_scratch = true;
    ctx.outer = C;
    ctx.unsafe_depth = C ? C->unsafe_depth : 0;
    Ctx *saved = C;
    C = &ctx;
    CT_DEPTH++;
    set_blk(new_block());
    push_scope(false, -1, -1);
    for (int i = 0; i < CT_NPRE; i++) new_local(CT_PRE_NAMES[i], CT_PRE_TYPES[i], e->tok, true);
    CT_NPRE = 0;
    Type *t = ty_void;
    if (blk) {
        block_body(e);
        if (!terminated()) emit(I_RET);
    } else {
        Val v = expr(e, want);
        if (want) v = coerce(v, want, e->tok, what);
        v = defaulted(v);
        t = v.t;
        if (t == ty_noreturn) error_at(e->tok, "this comptime expression never produces a value");
        if (t == ty_null) error_at(e->tok, "can't tell the type of a comptime null; give the declaration a type");
        if (t == ty_void) error_at(e->tok, "this comptime expression doesn't produce a value");
        if (v.isconst && !is_agg(t)) { /* already known: nothing to run */
            CT_DEPTH--;
            C = saved;
            memset(out, 0, sizeof *out);
            out->isconst = true;
            out->ival = v.ival;
            out->fval = v.fval;
            return t;
        }
        Inst *r;
        if (is_agg(t)) {
            int a = addr_reg(v.a);
            r = emit(I_RET);
            r->a = R(a);
            r->cls = C_I;
        } else {
            Opd o = opd_of(v);
            r = emit(I_RET);
            r->a = o;
            r->cls = cls_of(t);
        }
    }
    CT_DEPTH--;
    C = saved;
    uint64_t bits = 0;
    CtError err;
    if (!ct_run(f, &bits, &err)) ct_fail(&err, e->tok);
    if (!blk) ct_result(t, bits, e->tok, out);
    ct_release();
    return t;
}

/* a comptime expression inside a function: its value, as a constant or a read of its data */
static Val ct_eval(Node *e, Type *want, const char *what) {
    CtOut o;
    Type *t = ct_compute(e, false, want, what, &o);
    if (o.isconst) return is_float(t) ? V_flt(t, o.fval) : V_int(t, o.ival);
    int p = addr_sym(M->globals[o.g].sym);
    if (is_agg(t)) return V_agg(t, A_reg(p, 0), false);
    return V_reg(t, load(t, A_reg(p, 0)));
}

/* comptime { ... }: run it for its effects (asserts, @compileError) */
static void ct_block(Node *b) {
    CtOut o;
    ct_compute(b, true, NULL, NULL, &o);
}

/* functions lowered only for comptime code are dropped unless run-time code reaches them too */
static void prune_ct_only(void) {
    int n = M->nfuncs;
    bool any = false;
    for (int i = 0; i < n; i++) any |= M->funcs[i]->ct_only;
    if (!any) return;
    int cap = 16;
    while (cap < 2 * n) cap *= 2;
    int *tab = xalloc(sizeof(int) * (size_t)cap); /* symbol -> function index + 1 */
    for (int i = 0; i < n; i++) {
        uint64_t h = 1469598103934665603ULL;
        for (const char *c = M->funcs[i]->sym; *c; c++) h = (h ^ (unsigned char)*c) * 1099511628211ULL;
        int k = (int)(h & (uint64_t)(cap - 1));
        while (tab[k]) k = (k + 1) & (cap - 1);
        tab[k] = i + 1;
    }
    bool *live = xalloc((size_t)n + 1);
    int *work = xalloc(sizeof(int) * (size_t)(n + 1)), nw = 0;
#define MARK(sym_)                                                                                    \
    do {                                                                                              \
        const char *ms_ = (sym_);                                                                     \
        uint64_t mh_ = 1469598103934665603ULL;                                                        \
        for (const char *mc_ = ms_; *mc_; mc_++) mh_ = (mh_ ^ (unsigned char)*mc_) * 1099511628211ULL; \
        for (int mk_ = (int)(mh_ & (uint64_t)(cap - 1)); tab[mk_]; mk_ = (mk_ + 1) & (cap - 1))       \
            if (!strcmp(M->funcs[tab[mk_] - 1]->sym, ms_)) {                                          \
                if (!live[tab[mk_] - 1]) live[tab[mk_] - 1] = true, work[nw++] = tab[mk_] - 1;        \
                break;                                                                                \
            }                                                                                         \
    } while (0)
    for (int i = 0; i < n; i++)
        if (!M->funcs[i]->ct_only) live[i] = true, work[nw++] = i;
    for (int i = 0; i < M->nglobals; i++)
        for (int k = 0; k < M->globals[i].nitems; k++)
            if (M->globals[i].items[k].sym) MARK(M->globals[i].items[k].sym);
    while (nw) {
        IRFunc *f = M->funcs[work[--nw]];
        for (int b = 0; b < f->nblocks; b++)
            for (int i = 0; i < f->blocks[b].n; i++) {
                Inst *in = &f->blocks[b].insts[i];
                if ((in->op == I_CALL || in->op == I_ADDR_SYM) && in->sym) MARK(in->sym);
            }
    }
#undef MARK
    int m = 0;
    for (int i = 0; i < n; i++)
        if (live[i]) M->funcs[m++] = M->funcs[i];
    M->nfuncs = m;
    free(tab);
    free(live);
    free(work);
}

/* ---------- the untaken branch of `if (comptime c)`: it must refer only to names that exist ----------
 * It isn't type-checked (so it can call what only exists on another target's terms), but a typo
 * in it is still an error everywhere. Names bound inside the branch are collected first. */

typedef struct {
    const char **names;
    int n, cap;
} NameSet;

static void collect_bound(Node *n, NameSet *s) {
    if (!n) return;
    switch (n->k) {
    case N_LET: case N_IF: case N_WHILE: case N_FORRANGE: case N_FOREACH: case N_TERNARY: case N_CATCH:
    case N_DEFER: case N_ORELSE:
        if (n->name) PUSH(s->names, s->n, s->cap, n->name);
        if (n->name2) PUSH(s->names, s->n, s->cap, n->name2);
        break;
    default: break;
    }
    if ((n->k == N_LET) || (n->k == N_PATTERN && n->flag == 2))
        for (int i = 0; i < n->n; i++)
            if (n->list[i] && n->list[i]->name) PUSH(s->names, s->n, s->cap, n->list[i]->name);
    collect_bound(n->a, s);
    collect_bound(n->b, s);
    collect_bound(n->c, s);
    if (n->k != N_LET && !(n->k == N_PATTERN && n->flag == 2))
        for (int i = 0; i < n->n; i++) collect_bound(n->list[i], s);
}

static bool name_bound(NameSet *s, const char *name) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->names[i], name)) return true;
    return false;
}

static bool prim_name(const char *n) {
    static const char *P[] = {"void", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "str",
                              "anyerror", "usize", "isize", "cstr", "type", "anytype", "noreturn", NULL};
    for (const char **p = P; *p; p++)
        if (!strcmp(*p, n)) return true;
    return false;
}

static bool unit_has(Unit *u, const char *name) {
    if (fn_in(u, name) || type_in(u, name)) return true;
    for (int i = 0; i < ngvars; i++)
        if (gvars[i].unit == u && !strcmp(gvars[i].name, name)) return true;
    return false;
}

/* `a.b.c` written as one name (types: heap.Arena, Outer.Inner) */
static void check_dotted(const char *name, Token *tok, Token *mtok, NameSet *s) {
    const char *dot = strchr(name, '.');
    char *first = dot ? xstrndup(name, (size_t)(dot - name)) : (char *)name;
    if (name_bound(s, first) || prim_name(first) || find_struct(first) || env_find(first) || find_var(first) ||
        find_fn(first) || is_builtin(first) || !strcmp(first, "_") || !strcmp(first, "Self"))
        return;
    Unit *u = alias_unit(first);
    if (u) {
        if (dot) {
            const char *rest = dot + 1, *d2 = strchr(rest, '.');
            const char *member = d2 ? xstrndup(rest, (size_t)(d2 - rest)) : rest;
            if (!unit_has(u, member)) error_at(mtok, "%s has no '%s' (in the branch this target doesn't take)", unit_name(u), member);
        }
        return;
    }
    CHeader *h = alias_header(first);
    if (h) {
        if (dot && !c_lookup(h, dot + 1)) error_at(mtok, "%s doesn't declare '%s' (in the branch this target doesn't take)", c_header_name(h), dot + 1);
        return;
    }
    error_at(tok, "'%s' is not defined (in the branch this target doesn't take)", first);
}

static void check_names_in(Node *n, NameSet *s) {
    if (!n) return;
    Unit *su = CU;
    if (n->unit) CU = n->unit;
    switch (n->k) {
    case N_NAME:
    case N_TNAME:
        check_dotted(n->name, n->tok, n->tok, s);
        break;
    case N_TGENERIC:
        check_dotted(n->name, n->tok, n->tok, s);
        break;
    case N_STRUCTLIT:
        if (!n->a && n->name) check_dotted(n->name, n->tok, n->tok, s);
        break;
    case N_FIELDACC:
        if (n->a->k == N_NAME && !name_bound(s, n->a->name) && !is_local(n->a->name)) {
            check_dotted(strf("%s.%s", n->a->name, n->name), n->a->tok, n->tok, s);
            CU = su;
            return;
        }
        break;
    case N_PATTERN:
        if (n->flag == 2) { /* .Tag(captures) */
            CU = su;
            return;
        }
        break;
    default: break;
    }
    check_names_in(n->a, s);
    check_names_in(n->b, s);
    check_names_in(n->c, s);
    if (n->k != N_LET)
        for (int i = 0; i < n->n; i++) check_names_in(n->list[i], s);
    CU = su;
}

static void check_names(Node *n) {
    NameSet s;
    memset(&s, 0, sizeof s);
    collect_bound(n, &s);
    check_names_in(n, &s);
}

/* if (comptime c): the condition, known now */
static bool comptime_cond(Node *c) {
    Val v = ct_eval(c->a, ty_bool, "the comptime condition");
    if (v.t != ty_bool || !v.isconst) error_at(c->tok, "a comptime condition must be a bool");
    return v.ival != 0;
}

/* ---------- generics: comptime parameters, anytype, `-> type` functions ---------- */

static void data_item(GlobalVar *g, int off, int size, int64_t val, const char *sym);
static void declare_enum(Node *e);
static void declare_lazy(Node *ln, Type *owner);
static Type *ct_struct_expr(Node *n);
static void check_where(FnInfo *f, TEnv *env, const char *what, Token *tok);
static void declare_fn(Node *d, bool ext);
static void struct_fields(Type *t, Node *s);
static bool is_generic_param(Node *p);
static Type *instantiate_type(FnInfo *f, Node **args, int nargs, Token *tok);
static int geninst_counter;

static void bind_type(TEnv *e, const char *name, Type *t) {
    Binding b;
    memset(&b, 0, sizeof b);
    b.name = name;
    b.type = t;
    PUSH(e->b, e->n, e->cap, b);
}

/* a comptime value parameter: inside the instance its name is a constant */
static void bind_value(TEnv *e, const char *name, Val v, Type *vt, KeyPart *kp, Token *origin) {
    Binding b;
    memset(&b, 0, sizeof b);
    b.name = name;
    b.vt = vt;
    Var *var = xalloc(sizeof *var);
    var->name = name;
    var->t = vt;
    var->is_const = true;
    var->slot = var->reg = -1;
    kp->vt = vt;
    if (v.strlit) {
        StrLit *s = &M->strs[v.strlit - 1];
        var->kind = VK_CSTR;
        var->sdata = kp->sdata = s->data;
        var->slen = kp->slen = s->len;
        var->origin = origin;
    } else {
        var->kind = VK_CONST;
        var->cival = kp->ival = v.ival;
        var->cfval = kp->fval = v.fval;
    }
    b.var = var;
    PUSH(e->b, e->n, e->cap, b);
}

static bool key_eq(KeyPart *a, KeyPart *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i].t != b[i].t || a[i].vt != b[i].vt) return false;
        if (!a[i].vt) continue;
        if (is_bytes(a[i].vt)) {
            if (a[i].slen != b[i].slen || memcmp(a[i].sdata, b[i].sdata, (size_t)a[i].slen)) return false;
        } else if (is_float(a[i].vt) ? a[i].fval != b[i].fval : a[i].ival != b[i].ival) {
            return false;
        }
    }
    return true;
}

/* the type an argument names: u8, *Node, []const u8, List(i32), @TypeOf(x) */
static Type *type_arg(Node *a) {
    switch (a->k) {
    case N_NAME: {
        if (find_var(a->name) && !find_struct(a->name))
            error_at(a->tok, "'%s' is a value, but this parameter needs a type", a->name);
        Node tmp = *a;
        tmp.k = N_TNAME;
        return resolve(&tmp);
    }
    case N_TYPEEXPR: return resolve(a->a);
    case N_CALL: {
        FnInfo *f = callee_fn(a->a);
        if (f && f->type_fn) return instantiate_type(f, a->list, a->n, a->tok);
        break;
    }
    case N_FIELDACC: {
        Type *t = static_type(a);
        if (t) return t;
        break;
    }
    case N_TNAME: case N_TPTR: case N_TSLICE: case N_TOPT: case N_TARRAY: case N_TFN: case N_TTUPLE: case N_TERRU:
    case N_TGENERIC: case N_TTYPEOF:
        return resolve(a);
    default: break;
    }
    error_at(a->tok, "expected a type here (this parameter is 'comptime ...: type')");
}

/* where a comptime argument was written: a forwarded comptime string keeps its first origin */
static Token *origin_of(Node *a) {
    Var *v = a->k == N_NAME ? find_var(a->name) : NULL;
    return v && v->kind == VK_CSTR && v->origin ? v->origin : a->tok;
}

/* an argument for a comptime value parameter: it must be known at compile time */
static Val comptime_value(Node *a, Type *vt, const char *pname) {
    Node tmp;
    if (a->k == N_TNAME) { /* List(N) in a type position: N is a constant's name */
        tmp = *a;
        tmp.k = N_NAME;
        a = &tmp;
    }
    Val v = coerce(expr(a, vt), vt, a->tok, strf("comptime parameter '%s'", pname));
    if (is_bytes(vt) ? !v.strlit : (!v.isconst || is_agg(vt)))
        error_at(a->tok, "'%s' is a comptime parameter, so its argument must be known at compile time", pname);
    return v;
}

static const char *value_text(Val v, Type *vt) {
    if (v.strlit) return strf("\"%.*s\"", M->strs[v.strlit - 1].len, M->strs[v.strlit - 1].data);
    if (is_float(vt)) return strf("%g", v.fval);
    if (vt == ty_bool) return v.ival ? "true" : "false";
    return strf("%lld", (long long)v.ival);
}

/* ----- struct types made by type functions ----- */

typedef struct {
    Type *t;
    Node *s;
} PendStruct;
static PendStruct *pend; /* structs whose methods are declared once their type function has finished */
static int npend, cpend;
static const char *TF_NAME; /* the instance being made: List(i32) */
static int TF_COUNT;        /* structs made for it so far */

static Type *new_inst_struct(const char *name, Unit *u, const char *base) {
    Type *t = xalloc(sizeof *t);
    t->k = TY_STRUCT;
    t->name = name;
    t->mname = strf("%s%s__T%d", u ? u->prefix : "", base, ++geninst_counter);
    t->unit = u;
    t->is_pub = true;
    if (u && *u->dname) t->dname = strf("%s%s", u->dname, t->name);
    TEnv *env = xalloc(sizeof *env); /* Self, and the types declared inside it */
    env->parent = TE;
    t->genv = env;
    bind_type(env, "Self", t);
    return t;
}

/* the types declared inside an instance's struct (any depth): Self.Node becomes List(i32).Node */
static void collect_nested(Node *s, Node ***out, int *n, int *cap) {
    for (int j = 0; j < s->n; j++) {
        Node *m = s->list[j];
        if (m->k != N_STRUCT && m->k != N_ENUM) continue;
        PUSH(*out, *n, *cap, m);
        if (m->k == N_STRUCT) collect_nested(m, out, n, cap);
    }
}

static void nested_types(Type *t, Node *s, Node ***nodes, Type ***types, int *count) {
    Node **ns = NULL;
    int n = 0, cap = 0;
    collect_nested(s, &ns, &n, &cap);
    Type **ts = xalloc(sizeof(Type *) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) {
        Node *m = ns[i];
        const char *shortn = !strncmp(m->name, "Self.", 5) ? m->name + 5 : m->name;
        Type *nt = xalloc(sizeof *nt);
        nt->k = m->k == N_ENUM ? TY_ENUM : TY_STRUCT;
        nt->name = strf("%s.%s", t->name, shortn);
        char *mn = strf("%s_%s", t->mname, shortn);
        for (char *c = mn; *c; c++)
            if (*c == '.') *c = '_';
        nt->mname = mn;
        nt->unit = t->unit;
        nt->is_pub = true;
        if (t->unit && *t->unit->dname) nt->dname = strf("%s%s", t->unit->dname, nt->name);
        nt->genv = t->genv;
        if (m->k == N_STRUCT) {
            nt->packed = m->flag == 1;
            nt->is_union = m->flag == 2;
        }
        bind_type(t->genv, m->name, nt);
        if (strcmp(shortn, m->name)) bind_type(t->genv, shortn, nt);
        PUSH(structs, nstructs, cstructs, nt); /* so List(i32).Node finds it */
        ts[i] = nt;
    }
    *nodes = ns;
    *types = ts;
    *count = n;
}

/* an instance's fields, layout, nested types and lets (its methods come later: declare_inst_methods) */
static void fill_inst_struct(Type *t, Node *s, Token *tok) {
    TEnv *saved = TE;
    Unit *su = CU;
    TE = t->genv;
    if (s->unit) CU = s->unit;
    Node **nn;
    Type **nt;
    int k;
    nested_types(t, s, &nn, &nt, &k);
    for (int i = 0; i < k; i++)
        if (nn[i]->k == N_ENUM) declare_enum(nn[i]);
    for (int i = 0; i < k; i++)
        if (nn[i]->k == N_STRUCT) struct_fields(nt[i], nn[i]);
    struct_fields(t, s);
    if (g_ncalls < 64) g_calls[g_ncalls] = tok;
    g_ncalls++;
    layout(t, tok);
    for (int i = 0; i < k; i++) layout(nt[i], tok);
    g_ncalls--;
    for (int j = 0; j < s->n; j++)
        if (s->list[j]->k == N_LET) declare_lazy(s->list[j], t);
    for (int i = 0; i < k; i++)
        for (int j = 0; j < nn[i]->n; j++)
            if (nn[i]->list[j]->k == N_LET) declare_lazy(nn[i]->list[j], nt[i]);
    TE = saved;
    CU = su;
}

static void declare_inst_methods(Type *t, Node *s) {
    TEnv *saved = TE;
    TE = t->genv;
    Node **ns = NULL;
    int n = 0, cap = 0;
    PUSH(ns, n, cap, s);
    collect_nested(s, &ns, &n, &cap);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < ns[i]->n; j++) {
            Node *m = ns[i]->list[j];
            if (m->k != N_FN) continue;
            declare_fn(m, false);
            fns[nfns - 1]->lazy = true; /* lowered only when called */
        }
    TE = saved;
}

/* `struct { ... }` inside a type function: a new type, made while the function is compiled */
static Type *ct_struct_expr(Node *n) {
    const char *nm = TF_NAME ? (TF_COUNT++ ? strf("%s#%d", TF_NAME, TF_COUNT) : TF_NAME) : "struct";
    Type *t = new_inst_struct(nm, C->fn->decl->unit, C->fn->name);
    /* its declarations see the function's constants and type names (let n = ...; let T = ...;) */
    Scope *chain[256];
    int depth = 0;
    for (Scope *sc = C->cur; sc && depth < 256; sc = sc->parent) chain[depth++] = sc;
    for (int d = depth - 1; d >= 0; d--)
        for (int i = 0; i < chain[d]->n; i++) {
            Var *v = &chain[d]->vars[i];
            if (v->kind == VK_TYPE) {
                bind_type(t->genv, v->name, v->t);
            } else if (v->kind == VK_CONST || v->kind == VK_CSTR) {
                Binding b;
                memset(&b, 0, sizeof b);
                b.name = v->name;
                b.vt = v->t;
                Var *copy = xalloc(sizeof *copy);
                *copy = *v;
                b.var = copy;
                PUSH(((TEnv *)t->genv)->b, ((TEnv *)t->genv)->n, ((TEnv *)t->genv)->cap, b);
            }
        }
    bind_type(t->genv, "Self", t); /* (again: it wins over a captured name) */
    fill_inst_struct(t, n, n->tok);
    PendStruct ps = {t, n};
    PUSH(pend, npend, cpend, ps);
    return t;
}

/* the source text of a `where` clause, from its tokens */
static const char *where_text(Node *d) {
    const char *s = "";
    Token *prev = NULL;
    for (Token *k = d->wbeg; k < d->wend; k++) {
        const char *txt = k->k == TK_INT ? strf("%llu", (unsigned long long)k->ival)
                        : k->k == TK_FLOAT ? strf("%g", k->fval)
                        : k->k == TK_STR ? strf("\"%.*s\"", k->slen, k->str) : k->s;
        bool tight = !prev || !strcmp(txt, "(") || !strcmp(txt, ")") || !strcmp(txt, ",") || !strcmp(txt, ".") ||
                     !strcmp(prev->s ? prev->s : "", "(") || !strcmp(prev->s ? prev->s : "", ".") ||
                     ((!strcmp(prev->s ? prev->s : "", "!") || !strcmp(prev->s ? prev->s : "", "@")) && prev->k == TK_OP);
        if (!strcmp(txt, "(") && prev && prev->k == TK_KW) tight = false;
        s = strf("%s%s%s", s, tight ? "" : " ", txt);
        prev = k;
    }
    return s;
}

/* fn f(comptime T: type, ...) where cond: checked for each new instance, at the call */
/* a `where` clause may ask about a run-time parameter's type (@TypeOf(x)), not its value */
static void where_params_ok(Node *e, const char **pn, int np, bool in_typeof) {
    if (!e) return;
    if (e->k == N_TTYPEOF || (e->k == N_BUILTIN && !strcmp(e->name, "@TypeOf"))) in_typeof = true;
    if (e->k == N_NAME && !in_typeof)
        for (int i = 0; i < np; i++)
            if (!strcmp(pn[i], e->name))
                error_at(e->tok, "'%s' is only known at run time; a 'where' condition can ask about its type: @TypeOf(%s)", e->name, e->name);
    where_params_ok(e->a, pn, np, in_typeof);
    where_params_ok(e->b, pn, np, in_typeof);
    where_params_ok(e->c, pn, np, in_typeof);
    for (int i = 0; i < e->n; i++) where_params_ok(e->list[i], pn, np, in_typeof);
}

static void check_where_p(FnInfo *f, TEnv *env, const char *what, Token *tok, const char **pn, Type **pt, int np) {
    Node *d = f->decl;
    if (!d->wherec) return;
    where_params_ok(d->wherec, pn, np, false);
    CT_PRE_NAMES = pn;
    CT_PRE_TYPES = pt;
    CT_NPRE = np;
    TEnv *saved = TE;
    Unit *su = CU;
    Ctx *sc = C;
    Token *st = CUR_TOK;
    TE = env;
    CU = d->unit;
    C = NULL; /* it sees the comptime parameters and the module, not the caller's variables */
    CtOut o;
    Type *t = ct_compute(d->wherec, false, NULL, "the 'where' condition", &o);
    TE = saved;
    CU = su;
    C = sc;
    CUR_TOK = st;
    if (t != ty_bool) error_at(d->wherec->tok, "a 'where' condition must be a bool, not %s", tname(t));
    if (!o.ival) {
        Token *nt[1] = {d->wbeg};
        const char *nm[1] = {"the constraint is here"};
        error_notes(tok, 1, nt, nm, "%s doesn't satisfy 'where %s'", what, where_text(d));
    }
}

static void check_where(FnInfo *f, TEnv *env, const char *what, Token *tok) { check_where_p(f, env, what, tok, NULL, NULL, 0); }

/* type function call List(i32): runs at compile time and gives a type (one per distinct argument list) */
static Type *instantiate_type(FnInfo *f, Node **args, int nargs, Token *tok) {
    Node *d = f->decl;
    if (nargs != f->np) error_at(tok, "'%s' takes %d argument(s), got %d", f->name, f->np, nargs);
    TEnv *env = xalloc(sizeof *env);
    env->parent = f->tenv;
    KeyPart *key = xalloc(sizeof(KeyPart) * (size_t)(f->np + 1));
    const char *name = strf("%s(", f->name);
    TEnv *saved = TE;
    for (int i = 0; i < f->np; i++) {
        Node *p = d->list[i];
        if (!p->flag)
            error_at(p->tok, "'%s' returns a type, so it runs at compile time: its parameters must be comptime", f->name);
        if (p->a->k == N_TNAME && !strcmp(p->a->name, "type")) {
            Type *t = type_arg(args[i]);
            bind_type(env, p->name, t);
            key[i].t = t;
            name = strf("%s%s%s", name, i ? ", " : "", tname(t));
        } else {
            TE = env;
            Type *vt = resolve(p->a);
            TE = saved;
            Val v = comptime_value(args[i], vt, p->name);
            bind_value(env, p->name, v, vt, &key[i], origin_of(args[i]));
            name = strf("%s%s%s", name, i ? ", " : "", value_text(v, vt));
        }
    }
    for (int k = 0; k < f->ninsts; k++)
        if (key_eq(f->insts[k].key, key, f->np)) {
            if (!f->insts[k].type)
                error_at(tok, "'%s)' needs itself while it's being made (inside its own struct, use Self)", name);
            return f->insts[k].type;
        }
    check_where(f, env, strf("%s)", name), tok);
    Node *body = d->c;
    if (g_ncalls < 64) g_calls[g_ncalls] = tok;
    g_ncalls++;
    if (body->n == 1 && body->list[0]->k == N_RETURN && body->list[0]->a && body->list[0]->a->k == N_STRUCT) {
        /* just `return struct { ... };`: made directly, and registered first, so it can name itself */
        Node *s = body->list[0]->a;
        TE = env;
        Type *t = new_inst_struct(strf("%s)", name), d->unit, f->name);
        GenInst gi = {key, f->np, NULL, t};
        PUSH(f->insts, f->ninsts, f->cinsts, gi);
        fill_inst_struct(t, s, tok);
        declare_inst_methods(t, s);
        TE = saved;
        g_ncalls--;
        return t;
    }
    /* a body with logic in it: compile it (comptime parameters are constants there) and run it */
    GenInst gi = {key, f->np, NULL, NULL}; /* NULL: being made */
    PUSH(f->insts, f->ninsts, f->cinsts, gi);
    int gidx = f->ninsts - 1;
    FnInfo *g = xalloc(sizeof *g);
    *g = *f;
    g->generic = false;
    g->type_fn = false;
    g->ct_fn = true;
    g->insts = NULL;
    g->ninsts = g->cinsts = 0;
    g->tenv = env;
    g->np = 0;
    g->ret = ty_type;
    g->state = 0;
    g->lazy = false;
    g->sym = strf("%s__ct%d", f->sym, ++geninst_counter);
    const char *sn = TF_NAME;
    int sc = TF_COUNT, pstart = npend;
    TF_NAME = strf("%s)", name);
    TF_COUNT = 0;
    CT_DEPTH++;
    lower_fn(g);
    CT_DEPTH--;
    IRFunc *irf = M->funcs[M->nfuncs - 1];
    uint64_t bits = 0;
    CtError err;
    if (!ct_run(irf, &bits, &err)) ct_fail(&err, tok);
    ct_release();
    Type *res = type_from_id((int64_t)bits);
    if (!res) error_at(tok, "internal error: '%s)' didn't produce a type", name);
    f->insts[gidx].type = res;
    for (int k = pstart; k < npend; k++) declare_inst_methods(pend[k].t, pend[k].s);
    npend = pstart;
    TF_NAME = sn;
    TF_COUNT = sc;
    g_ncalls--;
    return res;
}

/* List(i32) written in a type position */
static Type *generic_type(Node *n) {
    Unit *su = CU;
    if (n->unit) CU = n->unit;
    FnInfo *f = find_fn(n->name);
    const char *dot = strchr(n->name, '.');
    if (!f && dot) { /* list.ArrayList(i32) */
        Unit *u = alias_unit(xstrndup(n->name, (size_t)(dot - n->name)));
        if (u) {
            f = fn_in(u, dot + 1);
            if (f) check_pub(u, f->is_pub, dot + 1, n->tok);
        }
    }
    CU = su;
    if (!f || !f->type_fn) error_at(n->tok, "'%s' isn't a function that returns a type, so '%s(...)' isn't a type", n->name, n->name);
    Type *t = instantiate_type(f, n->list, n->n, n->tok);
    if (n->name2) { /* Tree(i32).Node */
        Type *in = t->unit ? type_in(t->unit, strf("%s.%s", t->name, n->name2)) : NULL;
        if (!in) error_at(n->tok, "%s has no type '%s'", tname(t), n->name2);
        return in;
    }
    return t;
}

/* @TypeOf(e): e's type. e is lowered into an unreachable block, so it never runs */
static Type *type_of_expr(Node *e) {
    if (!C) error_at(e->tok, "@TypeOf can only be used inside a function");
    int saved = C->blk;
    set_blk(new_block());
    Val v = defaulted(expr(e, NULL));
    if (!terminated()) trap();
    C->blk = saved;
    if (v.t == ty_null || v.t == ty_noreturn) error_at(e->tok, "@TypeOf can't name the type of %s", tname(v.t));
    return v.t;
}

/* a call of a generic function: bind its comptime parameters, find or make the instance, call it */
static Val call_generic(FnInfo *f, Node **args, int nargs, Val *self, Token *tok) {
    Node *d = f->decl;
    int first = self ? 1 : 0, given = nargs + first;
    if (f->type_fn) {
        if (self) error_at(tok, "'%s' makes a type, so it can't be called as a method", f->name);
        return V_int(ty_type, type_id(instantiate_type(f, args, nargs, tok)));
    }
    if (given != f->np) error_at(tok, "'%s' takes %d argument(s), got %d", f->name, f->np - first, nargs);
    if (given > MAXARGS) error_at(tok, "too many arguments");
    TEnv *env = xalloc(sizeof *env);
    env->parent = f->tenv;
    KeyPart *key = xalloc(sizeof(KeyPart) * (size_t)(f->np + 1));
    Val vals[MAXARGS + 1];
    Token *toks[MAXARGS + 1];
    Type **pt = xalloc(sizeof(Type *) * (size_t)(f->np + 1));
    const char **pn = xalloc(sizeof(char *) * (size_t)(f->np + 1));
    Token **ptok = xalloc(sizeof(Token *) * (size_t)(f->np + 1));
    int nv = 0;
    TEnv *saved = TE;
    const char *desc = strf("%s(", f->name); /* for a failed `where`: max(str, ...) */
    for (int i = 0; i < f->np; i++) {
        Node *p = d->list[i];
        Node *a = i < first ? NULL : args[i - first];
        Token *at = a ? a->tok : tok;
        const char *sep = i ? ", " : "";
        if (p->flag) {
            if (!a) error_at(p->tok, "the receiver can't be a comptime parameter");
            if (p->a->k == N_TNAME && !strcmp(p->a->name, "type")) {
                Type *t = type_arg(a);
                bind_type(env, p->name, t);
                key[i].t = t;
                desc = strf("%s%s%s", desc, sep, tname(t));
            } else {
                TE = env;
                Type *vt = resolve(p->a);
                TE = saved;
                Val cv = comptime_value(a, vt, p->name);
                bind_value(env, p->name, cv, vt, &key[i], origin_of(a));
                desc = strf("%s%s%s", desc, sep, value_text(cv, vt));
            }
            continue;
        }
        desc = strf("%s%s%s", desc, sep, p->a->k == N_TNAME && !strcmp(p->a->name, "anytype") ? "anytype" : "...");
        Val v;
        Type *t;
        if (p->a->k == N_TNAME && !strcmp(p->a->name, "anytype")) { /* the argument's type becomes the parameter's */
            v = a ? defaulted(expr(a, NULL)) : *self;
            if (v.t == ty_null || v.t == ty_void || v.t == ty_noreturn)
                error_at(at, "can't pass %s as an anytype argument (it needs a type)", tname(v.t));
            t = v.t;
            key[i].t = t;
            desc = strf("%.*s%s", (int)(strlen(desc) - 7), desc, tname(t)); /* anytype -> the argument's type */
        } else {
            TE = env;
            t = resolve(p->a);
            TE = saved;
            v = a ? expr(a, t) : *self;
        }
        layout(t, at);
        vals[nv] = v;
        toks[nv] = at;
        pt[nv] = t;
        pn[nv] = p->name;
        ptok[nv] = p->tok;
        nv++;
    }
    FnInfo *g = NULL;
    for (int k = 0; k < f->ninsts; k++)
        if (key_eq(f->insts[k].key, key, f->np)) g = f->insts[k].fn;
    if (!g) {
        check_where_p(f, env, strf("%s)", desc), tok, pn, pt, nv);
        TE = env;
        Type *ret = d->b ? resolve(d->b) : ty_void;
        TE = saved;
        g = xalloc(sizeof *g);
        *g = *f;
        g->generic = false;
        g->insts = NULL;
        g->ninsts = g->cinsts = 0;
        g->tenv = env;
        g->np = nv;
        g->pt = pt;
        g->pn = pn;
        g->ptok = ptok;
        g->ret = ret;
        g->state = 0;
        g->sym = strf("%s__%d", f->sym, ++geninst_counter);
        GenInst gi = {key, f->np, g, NULL};
        PUSH(f->insts, f->ninsts, f->cinsts, gi);
    }
    if (g_ncalls < 64) g_calls[g_ncalls] = tok;
    g_ncalls++;
    Val res = call_core(g, vals, toks, nv, tok); /* lowers the instance the first time */
    g_ncalls--;
    return res;
}

/* the type a node names when it's used as `Type.member`: a type name, List(i32), or a type expression */
static FnInfo *callee_fn(Node *fn);

static Type *static_type_in(Node *n) {
    if (n->k == N_NAME) {
        Var *v = find_var(n->name);
        if (v) return v->kind == VK_TYPE ? v->t : NULL;
        Type *t = find_struct(n->name);
        check_type_pub(t, n->tok);
        return t;
    }
    if (n->k == N_FIELDACC) {
        Unit *u = unit_ref(n->a);
        if (u) { /* geo.Point, or geo.Points (let Points = List(Point);) */
            Type *t = type_in(u, n->name);
            if (t) {
                check_type_pub(t, n->tok);
                return t;
            }
            Unit *su = CU;
            CU = u;
            t = global_type(u, n->name);
            CU = su;
            if (t) {
                for (int i = 0; i < ngvars; i++)
                    if (gvars[i].unit == u && !strcmp(gvars[i].name, n->name)) check_pub(u, gvars[i].is_pub, n->name, n->tok);
            }
            return t;
        }
        CHeader *h = header_ref(n->a);
        if (h) return c_type_named(h, n->name, n->tok); /* c.FILE */
        Type *o = static_type(n->a); /* Outer.Inner */
        return o && o->unit ? type_in(o->unit, strf("%s.%s", o->name, n->name)) : NULL;
    }
    if (n->k == N_TYPEEXPR) return resolve(n->a);
    if (n->k == N_CALL) {
        FnInfo *f = callee_fn(n->a);
        if (f && f->type_fn) return instantiate_type(f, n->list, n->n, n->tok);
    }
    return NULL;
}

static Type *static_type(Node *n) {
    Unit *su = CU;
    if (n->unit) CU = n->unit;
    Type *t = static_type_in(n);
    CU = su;
    return t;
}

/* the function a callee expression names without a value: f, or mod.f (NULL otherwise) */
static FnInfo *callee_fn(Node *fn) {
    if (fn->k == N_NAME && !find_var(fn->name)) return find_fn(fn->name);
    if (fn->k == N_FIELDACC) {
        Unit *u = unit_ref(fn->a);
        if (!u) return NULL;
        FnInfo *f = fn_in(u, fn->name);
        if (f) check_pub(u, f->is_pub, fn->name, fn->tok);
        return f;
    }
    return NULL;
}

/* mod.name used as a value or a place: the global (or constant) `name` of that module */
static Var *module_var(Node *n) {
    if (n->k != N_FIELDACC) return NULL;
    Unit *u = unit_ref(n->a);
    if (!u) return NULL;
    Var *v = global_in(u, n->name);
    if (v) check_pub(u, v->is_pub, n->name, n->tok);
    return v;
}

/* a module member that isn't a function, global or type: say so */
static _Noreturn void no_member(Unit *u, Node *n) {
    error_at(n->tok, "%s has no '%s'", unit_name(u), n->name);
}

static Val call_fn(FnInfo *f, Node **args, int nargs, Val *self, Token *tok) {
    if (f->recv) check_pub(f->unit, f->is_pub, strf("%s.%s", tname(f->recv), f->name), tok);
    if (f->decl->is_unsafe) require_unsafe(tok, strf("calling the unsafe fn '%s'", f->name));
    if (f->generic) return call_generic(f, args, nargs, self, tok);
    int first = self ? 1 : 0;
    int given = nargs + first;
    if (given < f->np || (given > f->np && !f->variadic))
        error_at(tok, "'%s' takes %d argument(s), got %d", f->name, f->np - first, nargs);
    if (given > MAXARGS) error_at(tok, "too many arguments");
    Val vals[MAXARGS + 1];
    Token *toks[MAXARGS + 1];
    for (int i = 0; i < given; i++) {
        if (i < first) {
            vals[i] = *self;
            toks[i] = tok;
        } else {
            Node *an = args[i - first];
            vals[i] = expr(an, i < f->np ? f->pt[i] : NULL);
            toks[i] = an->tok;
        }
    }
    return call_core(f, vals, toks, given, tok);
}

/* ---------- strings: the prelude's StrBuf does the building ---------- */

static Val lib_call(Type *recv, const char *name, Val *args, int n, Token *tok) {
    FnInfo *m = find_method(recv, name);
    if (!m) error_at(tok, "internal error: prelude method %s.%s is missing", tname(recv), name);
    Token *toks[8];
    for (int i = 0; i < n; i++) toks[i] = tok;
    return call_core(m, args, toks, n, tok);
}

static void argn(Node *n, int k) {
    if (n->n != k) error_at(n->tok, "'%s' takes %d argument(s), got %d", n->a->name, k, n->n);
}

static Val builtin(Node *n, Type *want) {
    const char *name = n->a->name;
    if (!strcmp(name, "exit")) {
        argn(n, 1);
        Val v = coerce(expr(n->list[0], ty_i32), ty_i32, n->list[0]->tok, "exit code");
        callv("_rt_exit", -1, 1, opd_of(v), C_I);
        trap();
        return V_reg(ty_noreturn, -1);
    }
    if (!strcmp(name, "panic")) {
        argn(n, 1);
        Val v = expr(n->list[0], NULL);
        if (!is_bytes(v.t)) error_at(n->list[0]->tok, "panic takes a string, found %s", tname(v.t));
        int p = load(ptr_to(ty_u8), v.a), l = load(ty_i64, A_add(v.a, 8));
        callv("_rt_panic", -1, 2, R(p), C_I, R(l), C_I);
        trap();
        return V_reg(ty_noreturn, -1);
    }
    if (!strcmp(name, "assert")) {
        argn(n, 1);
        int ok = new_block(), fail = new_cold_block();
        cond(n->list[0], ok, fail);
        set_blk(fail);
        callv("_rt_assert_fail", -1, 1, IMM(n->tok->line), C_I);
        trap();
        set_blk(ok);
        return V_void();
    }
    if (!strcmp(name, "now")) {
        argn(n, 0);
        return V_reg(ty_f64, callv("_rt_now", C_F64, 0));
    }
    if (!strcmp(name, "args")) {
        argn(n, 0);
        Type *st = slice_of_q(ty_str, true); /* the runtime's array, made once at startup: read-only */
        Addr t = temp(st);
        callv("_rt_args", -1, 1, R(addr_reg(t)), C_I);
        return V_agg(st, t, true);
    }
    if (!strcmp(name, "min") || !strcmp(name, "max")) {
        argn(n, 2);
        Val a = expr(n->list[0], want);
        Val b = expr(n->list[1], is_lit(a.t) ? want : a.t);
        if (!is_num(a.t) || !is_num(b.t)) error_at(n->tok, "%s needs numbers", name);
        bool mn = !strcmp(name, "min");
        if (is_lit(a.t) && is_lit(b.t)) {
            Val c = fold("<", a, b, n->tok);
            return (c.ival == mn) ? a : b;
        }
        Type *t = default_t(unify(a.t, b.t, n->tok, name));
        a = coerce(a, t, n->tok, "argument");
        b = coerce(b, t, n->tok, "argument");
        int c;
        if (is_float(t)) c = fcmp(mn ? CC_MI : CC_GT, reg_of(a), reg_of(b));
        else c = cmp(mn ? (is_signed(t) ? CC_LT : CC_LO) : (is_signed(t) ? CC_GT : CC_HI), R(reg_of(a)), opd_of(b));
        int r = new_vreg(cls_of(t));
        Inst *i = emit(I_SEL);
        i->dst = r;
        i->cls = cls_of(t);
        i->a = R(c);
        i->b = R(reg_of(a));
        i->c = R(reg_of(b));
        return V_reg(t, r);
    }
    if (!strcmp(name, "abs")) {
        argn(n, 1);
        Val a = defaulted(expr(n->list[0], want));
        if (!is_num(a.t)) error_at(n->tok, "abs needs a number");
        if (a.isconst) return is_float(a.t) ? V_flt(a.t, a.fval < 0 ? -a.fval : a.fval) : V_int(a.t, a.ival < 0 ? -a.ival : a.ival);
        if (is_float(a.t)) return V_reg(a.t, op1(I_FABS, cls_of(a.t), R(a.reg)));
        if (!is_signed(a.t)) return a;
        int c = cmp(CC_LT, R(a.reg), IMM(0));
        int ng = norm(op1(I_NEG, C_I, R(a.reg)), a.t);
        int r = new_vreg(C_I);
        Inst *i = emit(I_SEL);
        i->dst = r;
        i->a = R(c);
        i->b = R(ng);
        i->c = R(a.reg);
        return V_reg(a.t, r);
    }
    /* math on f64 */
    int nargs = (!strcmp(name, "pow") || !strcmp(name, "atan2")) ? 2 : 1;
    argn(n, nargs);
    int regs[2];
    for (int i = 0; i < nargs; i++)
        regs[i] = reg_of(coerce(expr(n->list[i], ty_f64), ty_f64, n->list[i]->tok, "argument"));
    if (!strcmp(name, "sqrt")) return V_reg(ty_f64, op1(I_FSQRT, C_F64, R(regs[0])));
    const char *sym = strf("_%s", name);
    if (nargs == 1) return V_reg(ty_f64, callv(sym, C_F64, 1, R(regs[0]), C_F64));
    return V_reg(ty_f64, callv(sym, C_F64, 2, R(regs[0]), C_F64, R(regs[1]), C_F64));
}

static Val ex_call(Node *n, Type *want) {
    Node *fn = n->a;
    if (fn->k == N_NAME && !find_var(fn->name)) {
        FnInfo *f = find_fn(fn->name);
        if (f) return call_fn(f, n->list, n->n, NULL, n->tok);
        if (is_builtin(fn->name)) return builtin(n, want);
        if (!strcmp(fn->name, "alloc") || !strcmp(fn->name, "free"))
            error_at(fn->tok, "there's no global %s(): memory comes from an Allocator you pass around, like "
                              "'let xs = try alloc.alloc(T, n); defer alloc.free(xs);' (see heap.c_allocator, heap.DebugAllocator)", fn->name);
        if (!strcmp(fn->name, "print"))
            error_at(fn->tok, "print(...) was replaced by formatted output: try io.print(\"x = {}\\n\", .{x}); "
                              "(the format string is checked at compile time)");
        if (find_struct(fn->name) && find_struct(fn->name)->k == TY_STRUCT)
            error_at(n->tok, "to create a %s, write %s { field: value, ... }", fn->name, fn->name);
        error_at(fn->tok, "function '%s' is not defined", fn->name);
    }
    if (fn->k != N_FIELDACC) return call_value(expr(fn, NULL), n->list, n->n, n->tok);
    Unit *mu = unit_ref(fn->a);
    if (mu) { /* io.print(...) */
        FnInfo *f = callee_fn(fn);
        if (f) return call_fn(f, n->list, n->n, NULL, n->tok);
        if (module_var(fn)) return call_value(expr(fn, NULL), n->list, n->n, n->tok);
        Type *t = type_in(mu, fn->name);
        if (t && t->k == TY_STRUCT) error_at(n->tok, "to create a %s, write %s.%s { field: value, ... }", tname(t), fn->a->name, fn->name);
        no_member(mu, fn);
    }
    if (header_ref(fn->a)) return c_call(header_ref(fn->a), fn, n->list, n->n, n->tok);
    if (is_static_ref(fn->a)) {
        Type *st = static_type(fn->a);
        if (st->k == TY_ENUM) {
            for (int i = 0; i < st->nvariants; i++)
                if (!strcmp(st->variants[i].name, fn->name)) return enum_value(st, fn->name, 1, n->list, n->n, n->tok);
        }
        FnInfo *m = find_method(st, fn->name);
        if (!m) error_at(fn->tok, "'%s' has no method '%s'", tname(st), fn->name);
        return call_fn(m, n->list, n->n, NULL, n->tok);
    }
    LV l;
    Val v = V_void();
    bool have_lv = is_lvalue_node(fn->a);
    if (have_lv) l = lval(fn->a);
    else {
        v = defaulted(expr(fn->a, NULL)); /* (0.25).toString() calls f64's method */
        memset(&l, 0, sizeof l);
        l.t = v.t;
        l.a = v.a;
        l.temp = true;
    }
    Type *rt = l.t;
    FnInfo *m = find_method(rt, fn->name);
    if (!m && is_slice(rt) && !rt->is_const) m = find_method(slice_of_q(rt->elem, true), fn->name); /* []u8 has str's methods */
    if (!m && is_slice(rt) && (!strcmp(fn->name, "push") || !strcmp(fn->name, "pop")))
        error_at(fn->tok, "slices can't grow: '%s' would allocate behind your back; use an ArrayList(%s), which takes an allocator",
                 fn->name, tname(rt->elem));
    bool via_ptr = false; /* receiver is a pointer to the method's type: auto-deref */
    if (!m && is_ptr(rt) && !rt->many) {
        m = find_method(rt->elem, fn->name);
        via_ptr = m != NULL;
    }
    if (!m) { /* a field holding a function: s.callback(x) */
        Type *st = rt;
        Addr base = l.a;
        if (is_ptr(rt) && !rt->many) {
            base = A_reg(have_lv ? load(rt, l.a) : reg_of(v), 0);
            st = rt->elem;
        }
        if (st->k == TY_STRUCT)
            for (int i = 0; i < st->nfields; i++) {
                Field *fd = &st->fields[i];
                if (strcmp(fd->name, fn->name)) continue;
                if (!is_fn(fd->t) && !(fd->t->k == TY_OPT && is_fn(fd->t->elem)))
                    error_at(fn->tok, "field '%s' is %s, not a function", fd->name, tname(fd->t));
                return call_value(load_val(fd->t, A_add(base, fd->off)), n->list, n->n, fn->tok);
            }
    }
    if (!m && rt->k == TY_OPT) opt_misuse(fn->tok, rt);
    if (!m) error_at(fn->tok, "%s has no method '%s'", tname(rt), fn->name);
    Type *base = via_ptr ? rt->elem : rt;
    if (!m->self_kind)
        error_at(fn->tok, "'%s.%s' doesn't take self; call it as %s.%s(...)", tname(base), fn->name, tname(base), fn->name);
    Val self;
    if (m->self_kind == 2) {
        if (via_ptr) self = have_lv ? V_reg(rt, load(rt, l.a)) : v;
        else if (have_lv && !l.temp) {
            if (l.bits) error_at(fn->tok, "'%s' takes a pointer, but a packed struct field has no address", fn->name);
            if (l.ro && !m->pt[0]->is_const)
                error_at(fn->tok, "'%s' modifies its receiver, which is immutable here (declare it with 'var')", fn->name);
            self = V_reg(ptr_to_q(rt, l.ro), addr_reg(l.a));
        } else error_at(fn->tok, "'%s' modifies its receiver, so it needs a variable, not a temporary", fn->name);
    } else {
        if (via_ptr) self = load_val(base, A_reg(have_lv ? load(rt, l.a) : reg_of(v), 0));
        else self = have_lv ? lv_load(l) : v;
    }
    return call_fn(m, n->list, n->n, &self, n->tok);
}

/* @embedFile("path"): a file's bytes, read while compiling. The path is relative to the
 * directory of the file that uses it. Used for a value, a global and a field of a global. */
static bool is_embed(Node *e) { return e && e->k == N_BUILTIN && !strcmp(e->name, "@embedFile"); }
static const char *embed_file(Node *n, int *len) {
    if (n->n != 1 || n->list[0]->k != N_STR) error_at(n->tok, "@embedFile takes one path, a string literal");
    Node *p = n->list[0];
    if (p->slen == 0 || memchr(p->str, 0, (size_t)p->slen)) error_at(p->tok, "@embedFile: invalid path");
    const char *from = n->tok->path ? n->tok->path : g_path;
    const char *slash = strrchr(from, '/');
    char *full = p->str[0] == '/' || !slash ? strf("%.*s", p->slen, p->str)
                                            : strf("%.*s/%.*s", (int)(slash - from), from, p->slen, p->str);
    long n_ = 0;
    char *data = read_file_len(full, &n_);
    if (!data) error_at(p->tok, "@embedFile: can't read '%.*s' (relative to this file's directory)", p->slen, p->str);
    if (n_ > INT32_MAX) error_at(p->tok, "@embedFile: '%.*s' is too large", p->slen, p->str);
    *len = (int)n_;
    return data;
}

/* @sizeOf(T) @alignOf(T) @zeroes(T) @truncate(T, x) @bitcast(T, x) */
static Val ex_format(Node *n);
static Val ex_hash(Node *n);
static Val ex_eql(Node *n);

static Val ex_builtin(Node *n, Type *want) {
    if (!strcmp(n->name, "@format")) return ex_format(n);
    if (!strcmp(n->name, "@hash")) return ex_hash(n);
    if (!strcmp(n->name, "@eql")) return ex_eql(n);
    if (!strcmp(n->name, "@splat")) {
        if (n->n != 1) error_at(n->tok, "@splat takes one value");
        return ex_splat(n, want);
    }
    if (!strcmp(n->name, "@errorName")) {
        Val v = defaulted(expr(n->list[0], NULL));
        if (v.t->k != TY_ERRSET) error_at(n->list[0]->tok, "@errorName needs an error, found %s", tname(v.t));
        return err_name_str(opd_of(v));
    }
    if (!strcmp(n->name, "@addWithOverflow") || !strcmp(n->name, "@subWithOverflow") || !strcmp(n->name, "@mulWithOverflow")) {
        /* (wrapped result, overflowed) */
        if (n->n != 2) error_at(n->tok, "%s takes two integers", n->name);
        Val a = expr(n->list[0], want && want->tuple ? want->fields[0].t : NULL);
        Val b = expr(n->list[1], is_lit(a.t) ? NULL : a.t);
        if (!is_int(a.t) || !is_int(b.t)) error_at(n->tok, "%s needs integers", n->name);
        if (is_signed(a.t) != is_signed(b.t) && !is_lit(a.t) && !is_lit(b.t))
            error_at(n->tok, "%s can't mix %s and %s", n->name, tname(a.t), tname(b.t));
        Type *t = default_t(unify(a.t, b.t, n->tok, n->name));
        a = coerce(a, t, n->tok, "operand");
        b = coerce(b, t, n->tok, "operand");
        int ovf;
        Opd oa = opd_of(a);
        if (oa.k == O_IMM) oa = R(iconst(oa.v));
        int r = ovf_op(arith_opcode(n->name[1] == 'a' ? '+' : n->name[1] == 's' ? '-' : '*'), t, oa, opd_of(b), &ovf);
        Type *parts[2] = {t, ty_bool};
        Type *tt = tuple_of(parts, 2);
        layout(tt, n->tok);
        Addr ta = temp(tt);
        store(t, A_add(ta, tt->fields[0].off), R(norm(r, t)));
        store(ty_bool, A_add(ta, tt->fields[1].off), R(ovf));
        return V_agg(tt, ta, true);
    }
    if (!strcmp(n->name, "@ptrToInt")) {
        if (n->n != 1) error_at(n->tok, "@ptrToInt takes one pointer");
        Val v = expr(n->list[0], NULL);
        if (!is_ptr(v.t) && !is_optptr(v.t)) error_at(n->list[0]->tok, "@ptrToInt needs a pointer, found %s", tname(v.t));
        return V_reg(ty_usize, reg_of(v));
    }
    if (!strcmp(n->name, "@assume")) { /* an optimizer hint: checked in Debug, trusted otherwise */
        require_unsafe(n->tok, "@assume");
        if (n->n != 1) error_at(n->tok, "@assume takes one condition");
        if (MODE == MODE_DEBUG) {
            Val c = expr(n->list[0], ty_bool);
            if (c.t != ty_bool) error_at(n->list[0]->tok, "@assume needs a bool, found %s", tname(c.t));
            if (!c.isconst || !c.ival) panic_if(cmp(CC_EQ, opd_of(c).k == O_IMM ? R(iconst(c.ival)) : opd_of(c), IMM(0)), "@assume'd condition is false");
        }
        return V_void();
    }
    if (!strcmp(n->name, "@tagName")) {
        Val v = expr(n->list[0], NULL);
        if (v.t->k != TY_ENUM) error_at(n->list[0]->tok, "@tagName needs an enum value, found %s", tname(v.t));
        return tag_name(v);
    }
    if (!strcmp(n->name, "@reduceAdd")) { /* the sum of a vector's elements */
        if (n->n != 1) error_at(n->tok, "@reduceAdd takes one vector");
        Val v = expr(n->list[0], NULL);
        if (!is_vector(v.t)) error_at(n->list[0]->tok, "@reduceAdd needs a vector, found %s", tname(v.t));
        Type *et = v.t->elem;
        int pa = addr_reg(v.a);
        int r = new_vreg(cls_of(et));
        Inst *in = emit(I_VREDUCE);
        in->dst = r;
        in->cls = cls_of(et);
        in->a = R(pa);
        in->imm = vlanes(v.t);
        return V_reg(et, r);
    }
    if (!strcmp(n->name, "@compileError")) { /* an error now, if this code is compiled at all */
        if (n->n != 1) error_at(n->tok, "@compileError takes one string");
        Val m = expr(n->list[0], NULL);
        if (!m.strlit) error_at(n->list[0]->tok, "@compileError needs a string known at compile time");
        error_at(n->tok, "%.*s", M->strs[m.strlit - 1].len, M->strs[m.strlit - 1].data);
    }
    if (!strcmp(n->name, "@embedFile")) {
        int len;
        const char *data = embed_file(n, &len);
        return str_lit(data, len);
    }
    if (!strcmp(n->name, "@typeName") || !strcmp(n->name, "@isInt") || !strcmp(n->name, "@isFloat") ||
        !strcmp(n->name, "@isSigned") || !strcmp(n->name, "@hasField") || !strcmp(n->name, "@hasMethod")) {
        /* questions about a type, answered at compile time (comptime code and `where` clauses use them) */
        Type *qt = resolve(n->b);
        bool named = n->name[1] == 'h';
        if (n->n != (named ? 1 : 0)) error_at(n->tok, named ? "%s takes a type and a name" : "%s takes just a type", n->name);
        if (!strcmp(n->name, "@typeName")) {
            const char *tn = tname(qt);
            return str_lit(tn, (int)strlen(tn));
        }
        if (!strcmp(n->name, "@isInt")) return V_int(ty_bool, is_int(qt) && !is_lit(qt));
        if (!strcmp(n->name, "@isFloat")) return V_int(ty_bool, is_float(qt) && !is_lit(qt));
        if (!strcmp(n->name, "@isSigned")) return V_int(ty_bool, is_signed(qt) && !is_lit(qt));
        Val nm = expr(n->list[0], NULL);
        if (!nm.strlit) error_at(n->list[0]->tok, "%s needs a name known at compile time (a string)", n->name);
        const char *want_name = xstrndup(M->strs[nm.strlit - 1].data, (size_t)M->strs[nm.strlit - 1].len);
        bool has = false;
        if (!strcmp(n->name, "@hasField")) {
            if (qt->k == TY_STRUCT)
                for (int i = 0; i < qt->nfields; i++) has |= !strcmp(qt->fields[i].name, want_name);
            if (is_slice(qt)) has = !strcmp(want_name, "ptr") || !strcmp(want_name, "len");
        } else {
            Type *rt = is_ptr(qt) && !qt->many ? qt->elem : qt;
            has = find_method(rt, want_name) != NULL;
        }
        return V_int(ty_bool, has);
    }
    Type *t = resolve(n->b);
    layout(t, n->tok);
    const char *b = n->name;
    bool needs_value = !strcmp(b, "@truncate") || !strcmp(b, "@bitcast") || !strcmp(b, "@ptrCast") || !strcmp(b, "@intToPtr");
    if (n->n != (needs_value ? 1 : 0))
        error_at(n->tok, needs_value ? "%s takes a type and a value" : "%s takes just a type", b);
    if (!strcmp(b, "@sizeOf")) return V_int(ty_litint, t->size);
    if (!strcmp(b, "@minInt") || !strcmp(b, "@maxInt")) {
        if (!is_int(t)) error_at(n->b->tok, "%s needs an integer type, not %s", b, tname(t));
        return V_int(t, b[2] == 'i' ? int_min(t) : int_max(t));
    }
    if (!strcmp(b, "@alignOf")) return V_int(ty_litint, t->align);
    if (!strcmp(b, "@zeroes")) {
        if (t == ty_void) error_at(n->tok, "void has no value");
        if (is_agg(t)) {
            Addr a = temp(t);
            zero_at(t, a);
            return V_agg(t, a, true);
        }
        return is_float(t) ? V_flt(t, 0) : V_int(t, 0);
    }
    if (!strcmp(b, "@ptrCast") || !strcmp(b, "@intToPtr")) {
        require_unsafe(n->tok, b);
        if (!is_ptr(t) && !is_optptr(t)) error_at(n->b->tok, "%s makes a pointer, not %s", b, tname(t));
        Val v = expr(n->list[0], NULL);
        if (b[1] == 'p') {
            if (!is_ptr(v.t) && !is_optptr(v.t)) error_at(n->list[0]->tok, "@ptrCast needs a pointer, found %s", tname(v.t));
            if (v.t->k == TY_OPT && t->k != TY_OPT)
                error_at(n->list[0]->tok, "%s might be null; unwrap it first ('p.?') or cast to ?%s", tname(v.t), tname(t));
        } else {
            if (!is_int(v.t)) error_at(n->list[0]->tok, "@intToPtr needs an integer, found %s", tname(v.t));
            if (v.isconst && (is_lit(v.t) || !is_signed(v.t))) v = V_int(ty_usize, v.ival); /* a constant address */
            else v = coerce(v, ty_usize, n->list[0]->tok, "@intToPtr's address");
            if (t->k != TY_OPT && v.isconst && v.ival == 0)
                error_at(n->list[0]->tok, "%s can't be null; use ?%s", tname(t), tname(t));
        }
        return V_reg(t, reg_of(v));
    }
    Val v = expr(n->list[0], NULL);
    bool pk_to = t->k == TY_STRUCT && t->packed, pk_from = v.t->k == TY_STRUCT && v.t->packed;
    if (!strcmp(b, "@bitcast") && (pk_to || pk_from)) { /* a packed struct <-> its integer */
        if (pk_from && is_int(t)) {
            if (t->size != v.t->size)
                error_at(n->tok, "@bitcast needs types of the same size (%s is %d bytes, %s is %d)", tname(v.t), v.t->size, tname(t), t->size);
            return V_reg(t, norm(load(v.t->tagt, v.a), t));
        }
        if (pk_to && is_int(v.t)) {
            Val w = v.t->k == TY_LITINT ? coerce(v, t->tagt, n->list[0]->tok, "@bitcast value") : v;
            if (w.t->size != t->size)
                error_at(n->tok, "@bitcast needs types of the same size (%s is %d bytes, %s is %d)", tname(w.t), w.t->size, tname(t), t->size);
            Addr a = temp(t);
            store(t->tagt, a, opd_of(w));
            return V_agg(t, a, true);
        }
        error_at(n->tok, "@bitcast converts a packed struct to or from an integer of the same size");
    }
    if (!strcmp(b, "@bitcast") && (is_ptr(t) || is_optptr(t) || is_ptr(v.t) || is_optptr(v.t)))
        error_at(n->tok, "@bitcast doesn't convert pointers; use @ptrCast, @intToPtr or @ptrToInt");
    if (!strcmp(b, "@bitcast") && ((is_float(t) && !is_lit(t) && is_int(v.t)) || (is_float(v.t) && is_int(t) && !is_lit(t)))) {
        /* a float's bits as an integer of the same size, or the other way */
        if (v.t->k == TY_LITFLOAT) v = coerce(v, t->size == 4 ? ty_f32 : ty_f64, n->list[0]->tok, "@bitcast value");
        if (v.t->k == TY_LITINT) v = coerce(v, t->size == 4 ? ty_u32 : ty_u64, n->list[0]->tok, "@bitcast value");
        if (t->size != v.t->size)
            error_at(n->tok, "@bitcast needs types of the same size (%s is %d bytes, %s is %d)", tname(v.t), v.t->size, tname(t), t->size);
        if (v.isconst) {
            if (is_float(v.t)) { /* float -> int */
                uint64_t bits = 0;
                if (v.t->size == 4) {
                    float f = (float)v.fval;
                    memcpy(&bits, &f, 4);
                } else {
                    memcpy(&bits, &v.fval, 8);
                }
                return V_int(t, trunc_ext((int64_t)bits, t));
            }
            uint64_t bits = (uint64_t)v.ival; /* int -> float */
            if (t->size == 4) {
                uint32_t b32 = (uint32_t)bits;
                float f;
                memcpy(&f, &b32, 4);
                return V_flt(t, f);
            }
            double d;
            memcpy(&d, &bits, 8);
            return V_flt(t, d);
        }
        Addr a = temp(v.t); /* through memory: store one way, load the other */
        store(v.t, a, opd_of(v));
        return V_reg(t, load(t, a));
    }
    if (!is_int(t) || !is_int(v.t)) error_at(n->tok, "%s works on integers and floats of the same size (found %s -> %s)", b, tname(v.t), tname(t));
    Type *from = default_t(v.t);
    if (!strcmp(b, "@truncate") && t->size > from->size) error_at(n->tok, "@truncate can't widen %s to %s; use 'as'", tname(from), tname(t));
    if (!strcmp(b, "@bitcast") && t->size != from->size)
        error_at(n->tok, "@bitcast needs types of the same size (%s is %d bytes, %s is %d)", tname(from), from->size, tname(t), t->size);
    if (v.isconst) return V_int(t, trunc_ext(v.ival, t));
    return V_reg(t, norm(v.reg, t)); /* keep the low bits, re-extended for the new type */
}

/* ---------- enums ---------- */

static void block_body(Node *b);
static void emit_defers(Scope *stop);
static Var *new_local(const char *name, Type *t, Token *tok, bool is_const);

static Variant *find_variant(Type *et, const char *name) {
    for (int i = 0; i < et->nvariants; i++)
        if (!strcmp(et->variants[i].name, name)) return &et->variants[i];
    return NULL;
}

/* kind: 0 = bare (Red), 1 = tuple payload (Circle(2.0)), 2 = struct payload (Rect { w: 1 }) */
static Val enum_value(Type *et, const char *vname, int kind, Node **list, int n, Token *tok) {
    Variant *vr = find_variant(et, vname);
    if (!vr) error_at(tok, "enum %s has no variant '%s'", et->name, vname);
    layout(et, tok);
    static const char *shape[] = {"", "(...)", " { ... }"};
    if (kind != vr->kind) {
        if (vr->kind == 0) error_at(tok, "%s.%s has no payload; write just %s.%s", et->name, vname, et->name, vname);
        error_at(tok, "%s.%s needs its payload: %s.%s%s", et->name, vname, et->name, vname, shape[vr->kind]);
    }
    if (!et->has_payload) return V_int(et, vr->value);
    Addr a = temp(et);
    store(et->tagt, a, IMM(vr->value));
    Type *pt = vr->payload;
    Addr pa = A_add(a, et->payload_off);
    if (vr->kind == 1) {
        if (n != pt->nfields) error_at(tok, "%s.%s takes %d value(s), got %d", et->name, vname, pt->nfields, n);
        for (int i = 0; i < n; i++) {
            Field *fd = &pt->fields[i];
            store_val(fd->t, A_add(pa, fd->off), coerce(expr(list[i], fd->t), fd->t, list[i]->tok, "payload"));
        }
    } else if (vr->kind == 2) {
        bool *given = xalloc((size_t)pt->nfields + 1);
        for (int i = 0; i < n; i++) {
            Node *f = list[i];
            int k = -1;
            for (int j = 0; j < pt->nfields; j++)
                if (!strcmp(pt->fields[j].name, f->name)) k = j;
            if (k < 0) error_at(f->tok, "%s.%s has no field '%s'", et->name, vname, f->name);
            if (given[k]) error_at(f->tok, "field '%s' given twice", f->name);
            given[k] = true;
            Field *fd = &pt->fields[k];
            store_val(fd->t, A_add(pa, fd->off), coerce(expr(f->a, fd->t), fd->t, f->a->tok, strf("field '%s'", f->name)));
        }
        for (int j = 0; j < pt->nfields; j++)
            if (!given[j]) error_at(tok, "missing field '%s' in %s.%s", pt->fields[j].name, et->name, vname);
        free(given);
    }
    return V_agg(et, a, true);
}

static Opd enum_tag(Val v) {
    if (v.t->has_payload) return R(load(v.t->tagt, v.a));
    return opd_of(v);
}

/* the variant's name as a str */
static Val tag_name(Val v) {
    Type *et = v.t;
    Opd tag = enum_tag(v);
    if (tag.k == O_IMM) {
        for (int i = 0; i < et->nvariants; i++)
            if (et->variants[i].value == tag.v) return str_lit(et->variants[i].name, (int)strlen(et->variants[i].name));
        tag = R(iconst(tag.v));
    }
    Addr res = temp(ty_str);
    int join = new_block();
    for (int i = 0; i < et->nvariants; i++) {
        int yes = new_block(), no = new_block();
        br(R(cmp(CC_EQ, tag, IMM(et->variants[i].value))), yes, no);
        set_blk(yes);
        copy_agg(ty_str, res, str_lit(et->variants[i].name, (int)strlen(et->variants[i].name)).a);
        jmp(join);
        set_blk(no);
    }
    copy_agg(ty_str, res, str_lit("?", 1).a);
    jmp(join);
    set_blk(join);
    return V_agg(ty_str, res, true);
}

/* .Circle(r) binds r to the payload; .Rect(r) binds r to the payload struct */
static void bind_captures(Node *pat, Variant *vr, Type *et, Val subj) {
    if (vr->kind == 0) error_at(pat->tok, "%s.%s has no payload to capture", et->name, vr->name);
    Type *pt = vr->payload;
    Addr pa = A_add(subj.a, et->payload_off);
    if (vr->kind == 2) {
        if (pat->n != 1)
            error_at(pat->tok, "%s.%s has a struct payload: capture it with one name, like .%s(r)", et->name, vr->name, vr->name);
        Node *c = pat->list[0];
        if (strcmp(c->name, "_")) {
            Var *v = declare(c->name, pt, c->tok, true);
            v->kind = VK_REF;
            v->reg = addr_reg(pa);
        }
        return;
    }
    if (pat->n != pt->nfields)
        error_at(pat->tok, "%s.%s holds %d value(s), but the pattern captures %d", et->name, vr->name, pt->nfields, pat->n);
    for (int i = 0; i < pat->n; i++) {
        Node *c = pat->list[i];
        if (!strcmp(c->name, "_")) continue;
        Field *fd = &pt->fields[i];
        Addr fa = A_add(pa, fd->off);
        if (is_agg(fd->t)) {
            Var *v = declare(c->name, fd->t, c->tok, true);
            v->kind = VK_REF;
            v->reg = addr_reg(fa);
        } else {
            Var *v = new_local(c->name, fd->t, c->tok, true);
            store(fd->t, A_slot(v->slot, 0), R(load(fd->t, fa)));
        }
    }
}

typedef struct {
    __int128 lo, hi;
} Interval;

static __int128 as_wide(int64_t v, Type *t) { return is_signed(t) ? (__int128)v : (__int128)(uint64_t)v; }

static const char *wide_str(__int128 v) {
    return v < 0 ? strf("%lld", (long long)v) : strf("%llu", (unsigned long long)v);
}

static bool arm_ends_with_fallthrough(Node *arm) {
    return arm->a->k == N_BLOCK && arm->a->n && arm->a->list[arm->a->n - 1]->k == N_FALLTHROUGH;
}

/* switch: a chain of compares (a range is one unsigned compare), then the case bodies.
 * Exhaustiveness is checked here: a missing enum variant or an uncovered integer is an error. */
static Val lower_switch(Node *n, Type *want) {
    bool is_expr = n->flag;
    Val subj = defaulted(expr(n->a, NULL));
    Type *st = subj.t;
    bool is_enum = st->k == TY_ENUM, is_err = st->k == TY_ERRSET;
    if (!is_enum && !is_err && !is_int(st) && st != ty_bool)
        error_at(n->a->tok, "can't switch on %s (switch works on enums, errors, integers and bools)", tname(st));
    if (is_enum) layout(st, n->tok);
    Type *ct = is_enum ? st->tagt : is_err ? ty_u16 : st; /* what the tag compares as */
    Opd tag = is_enum ? enum_tag(subj) : opd_of(subj);
    if (tag.k == O_IMM) tag = R(iconst(tag.v));
    int narms = n->n;
    int *body = xalloc(sizeof(int) * (size_t)narms);
    bool *covered = is_enum ? xalloc((size_t)st->nvariants + 1) : is_err ? xalloc((size_t)st->nerrs + 1) : NULL;
    Interval *ivs = NULL;
    int niv = 0, civ = 0, else_arm = -1;
    for (int i = 0; i < narms; i++) body[i] = new_block();
    for (int i = 0; i < narms; i++) {
        Node *arm = n->list[i];
        for (int j = 0; j < arm->n; j++) {
            Node *p = arm->list[j];
            if (p->flag == 3) {
                if (else_arm >= 0) error_at(p->tok, "a switch can only have one 'else' case");
                if (arm->n > 1) error_at(p->tok, "'else' must be a case of its own");
                else_arm = i;
                continue;
            }
            if (p->n && arm->n > 1) error_at(p->tok, "a case that captures a payload must have a single pattern");
            int nt = new_block();
            if (p->flag == 2 && is_err) { /* .NotFound on an error */
                int code = err_code(p->name);
                if (!set_has(st, code)) error_at(p->tok, "%s has no error %s", tname(st), p->name);
                for (int k = 0; k < st->nerrs; k++)
                    if (st->errcodes[k] == code) {
                        if (covered[k]) error_at(p->tok, "error.%s is already handled by an earlier case", p->name);
                        covered[k] = true;
                    }
                br(R(cmp(CC_EQ, tag, IMM(code))), body[i], nt);
            } else if (p->flag == 2) { /* .Tag */
                if (!is_enum) error_at(p->tok, "'.%s' patterns need an enum, but this switch is on %s", p->name, tname(st));
                Variant *vr = find_variant(st, p->name);
                if (!vr) error_at(p->tok, "enum %s has no variant '%s'", st->name, p->name);
                int k = (int)(vr - st->variants);
                if (covered[k]) error_at(p->tok, "%s.%s is already handled by an earlier case", st->name, vr->name);
                covered[k] = true;
                br(R(cmp(CC_EQ, tag, IMM(vr->value))), body[i], nt);
            } else { /* value, or lo..=hi */
                Val lo = expr(p->a, st), hi;
                if (!lo.isconst) error_at(p->a->tok, "case values must be constants");
                lo = coerce(lo, st, p->a->tok, "case value");
                hi = lo;
                if (p->flag == 1) {
                    if (is_enum || is_err || st == ty_bool) error_at(p->tok, "ranges only work on integers");
                    hi = expr(p->b, st);
                    if (!hi.isconst) error_at(p->b->tok, "case values must be constants");
                    hi = coerce(hi, st, p->b->tok, "case value");
                }
                __int128 wl = as_wide(lo.ival, ct), wh = as_wide(hi.ival, ct);
                if (wl > wh) error_at(p->tok, "empty range: %s is greater than %s", wide_str(wl), wide_str(wh));
                if (is_err) {
                    for (int k = 0; k < st->nerrs; k++)
                        if (st->errcodes[k] == lo.ival) {
                            if (covered[k]) error_at(p->tok, "error.%s is already handled by an earlier case", M->errnames[lo.ival]);
                            covered[k] = true;
                        }
                } else if (is_enum) {
                    int k = 0;
                    while (st->variants[k].value != lo.ival) k++;
                    if (covered[k]) error_at(p->tok, "%s.%s is already handled by an earlier case", st->name, st->variants[k].name);
                    covered[k] = true;
                } else {
                    for (int v = 0; v < niv; v++)
                        if (wl <= ivs[v].hi && ivs[v].lo <= wh)
                            error_at(p->tok, "case %s is already handled by an earlier case", wide_str(wl > ivs[v].lo ? wl : ivs[v].lo));
                    Interval iv = {wl, wh};
                    PUSH(ivs, niv, civ, iv);
                }
                if (wl == wh) {
                    br(R(cmp(CC_EQ, tag, IMM(lo.ival))), body[i], nt);
                } else { /* lo <= x <= hi  as one unsigned compare: (x - lo) <= (hi - lo) */
                    int d = op2(I_SUB, C_I, tag, IMM(lo.ival));
                    br(R(cmp(CC_LS, R(d), IMM((int64_t)(uint64_t)(wh - wl)))), body[i], nt);
                }
            }
            set_blk(nt);
        }
    }
    /* nothing matched */
    if (else_arm >= 0) {
        jmp(body[else_arm]);
    } else if (is_err) {
        if (st->all_errors) error_at(n->tok, "a switch on %s needs an 'else' case: any error can show up", tname(st));
        const char *missing = NULL;
        for (int k = 0; k < st->nerrs; k++)
            if (!covered[k])
                missing = missing ? strf("%s, error.%s", missing, M->errnames[st->errcodes[k]]) : strf("error.%s", M->errnames[st->errcodes[k]]);
        if (missing) error_at(n->tok, "switch doesn't handle %s (add it or an 'else' case)", missing);
        trap();
    } else if (is_enum) {
        const char *missing = NULL;
        int nmissing = 0;
        for (int v = 0; v < st->nvariants; v++) {
            if (covered[v]) continue;
            missing = missing ? strf("%s, %s.%s", missing, st->name, st->variants[v].name)
                              : strf("%s.%s", st->name, st->variants[v].name);
            nmissing++;
        }
        if (nmissing)
            error_at(n->tok, "switch doesn't handle %s (add %s or an 'else' case)", missing, nmissing > 1 ? "them" : "it");
        Val msg = str_lit("invalid enum value in switch", 28); /* e.g. garbage handed over by C */
        callv("_rt_panic", -1, 2, R(load(ptr_to(ty_u8), msg.a)), C_I, IMM(28), C_I);
        trap();
    } else {
        __int128 tmin = 0, tmax = 1;
        if (st != ty_bool) {
            int bits = st->size * 8;
            tmin = is_signed(st) ? -((__int128)1 << (bits - 1)) : 0;
            tmax = is_signed(st) ? ((__int128)1 << (bits - 1)) - 1 : ((__int128)1 << bits) - 1;
        }
        for (int a = 0; a < niv; a++) /* sort the intervals, then look for a gap */
            for (int b = a + 1; b < niv; b++)
                if (ivs[b].lo < ivs[a].lo) {
                    Interval t = ivs[a];
                    ivs[a] = ivs[b];
                    ivs[b] = t;
                }
        __int128 next = tmin;
        for (int a = 0; a < niv && next <= tmax; a++) {
            if (ivs[a].lo > next) break;
            if (ivs[a].hi + 1 > next) next = ivs[a].hi + 1;
        }
        if (next <= tmax)
            error_at(n->tok, "switch on %s doesn't handle every value (%s is missing); add an 'else' case", tname(st),
                     st == ty_bool ? (next ? "true" : "false") : wide_str(next));
        trap();
    }
    /* case bodies */
    int join = new_block();
    Val *vals = is_expr ? xalloc(sizeof(Val) * (size_t)narms) : NULL;
    int *ends = is_expr ? xalloc(sizeof(int) * (size_t)narms) : NULL;
    for (int i = 0; i < narms; i++) {
        Node *arm = n->list[i];
        set_blk(body[i]);
        push_scope(false, -1, -1);
        if (arm->n == 1 && arm->list[0]->flag == 2 && arm->list[0]->n)
            bind_captures(arm->list[0], find_variant(st, arm->list[0]->name), st, subj);
        if (is_expr) {
            vals[i] = expr(arm->a, want ? want : (i > 0 && !is_lit(vals[0].t) ? vals[0].t : NULL));
            ends[i] = C->blk;
            pop_scope();
            continue;
        }
        if (arm_ends_with_fallthrough(arm)) {
            Token *ft = arm->a->list[arm->a->n - 1]->tok;
            if (i + 1 >= narms) error_at(ft, "fallthrough in the last case has nowhere to go");
            Node *nx = n->list[i + 1];
            if (nx->n == 1 && nx->list[0]->n) error_at(ft, "can't fall through into a case that captures a payload");
            for (int j = 0; j + 1 < arm->a->n; j++) stmt(arm->a->list[j]);
            emit_defers(C->cur);
            pop_scope();
            jmp(body[i + 1]);
            continue;
        }
        if (arm->a->k == N_BLOCK) {
            block_body(arm->a);
        } else {
            stmt(arm->a);
            emit_defers(C->cur);
        }
        pop_scope();
        jmp(join);
    }
    if (!is_expr) {
        set_blk(join);
        return V_void();
    }
    Type *t = ty_noreturn;
    for (int i = 0; i < narms; i++) {
        Type *b = vals[i].t;
        if (b == ty_noreturn || b == t) continue;
        if (t == ty_noreturn) {
            t = b;
            continue;
        }
        if (is_num(t) && is_num(b)) t = unify(t, b, n->list[i]->a->tok, "switch");
        else if (t == ty_null && is_ptr(b)) t = b;
        else if (!(b == ty_null && is_ptr(t)))
            error_at(n->list[i]->a->tok, "every case of a switch expression must have the same type (%s vs %s)", tname(t), tname(b));
    }
    t = want ? want : default_t(t);
    if (t == ty_void) error_at(n->tok, "switch cases must produce values");
    Val res;
    if (is_agg(t)) {
        Addr ra = temp(t);
        for (int i = 0; i < narms; i++) {
            set_blk(ends[i]);
            copy_agg(t, ra, coerce(vals[i], t, n->list[i]->a->tok, "switch case").a);
            jmp(join);
        }
        res = V_agg(t, ra, true);
    } else {
        int r = new_vreg(cls_of(t));
        for (int i = 0; i < narms; i++) {
            set_blk(ends[i]);
            mov(r, cls_of(t), opd_of(coerce(vals[i], t, n->list[i]->a->tok, "switch case")));
            jmp(join);
        }
        res = V_reg(t, r);
    }
    set_blk(join);
    return res;
}

/* ---------- optionals ---------- */

static void bind_err(const char *name, Token *tok, Val v) {
    if (!strcmp(name, "_")) return;
    Var *ev = new_local(name, v.t->eset, tok, true);
    store(v.t->eset, A_slot(ev->slot, 0), eu_err(v));
}

/* binds the value inside an optional or (successful) error union */
static void bind_opt(const char *name, Token *tok, Val ov) {
    if (!strcmp(name, "_")) return;
    Type *et = ov.t->elem;
    Val inner = ov.t->k == TY_OPT ? opt_value(ov) : eu_value(ov);
    Var *v = new_local(name, et, tok, true);
    store_val(et, A_slot(v->slot, 0), inner);
}

static Val ex_unwrap(Node *n) {
    Val v = expr(n->a, NULL);
    if (v.t->k != TY_OPT) error_at(n->tok, "'.?' needs an optional value (?T), found %s", tname(v.t));
    int ok = new_block(), fail = new_cold_block();
    br(R(opt_has(v)), ok, fail);
    set_blk(fail);
    Val msg = str_lit("unwrapped a null value with .?", 30);
    callv("_rt_panic", -1, 2, R(load(ptr_to(ty_u8), msg.a)), C_I, IMM(30), C_I);
    trap();
    set_blk(ok);
    return opt_value(v);
}

static void emit_defers_err(Scope *stop, Val *err);

static void ret_inst(Opd o) {
    FnInfo *f = C->fn;
    Inst *r = emit(I_RET);
    if (!is_agg(f->ret) && f->ret != ty_void) {
        r->a = o;
        r->cls = cls_of(f->ret);
    }
}

/* leave the function with error `code`: errdefers and defers run first */
static void err_return(Opd code) {
    Type *rt = C->fn->ret;
    Val e = code.k == O_IMM ? V_int(rt->eset, code.v) : V_reg(rt->eset, (int)code.v);
    emit_defers_err(NULL, &e);
    if (is_agg(rt)) store(ty_u16, A_reg(C->retptr, err_off(rt)), code);
    ret_inst(code);
}

static Val ex_try(Node *n) {
    if (C->ct_scratch) error_at(n->tok, "'try' would leave the comptime expression; handle the error with 'catch'");
    if (C->defer_depth)
        error_at(n->tok, "'try' can't be used in a defer: the function is already leaving (handle the error with 'catch')");
    Val v = expr(n->a, NULL);
    if (v.t->k != TY_ERRUNION) error_at(n->tok, "'try' needs something that can fail (!T), but this is %s", tname(v.t));
    FnInfo *f = C->fn;
    if (!f->ret || f->ret->k != TY_ERRUNION)
        error_at(n->tok, "'try' passes the error up, so '%s' must return an error union (like -> !%s)", f->name,
                 f->ret ? tname(f->ret) : "T");
    if (!set_subset(v.t->eset, f->ret->eset))
        error_at(n->tok, "this can fail with %s errors, but '%s' returns %s", tname(v.t->eset), f->name, tname(f->ret));
    Opd code = eu_err(v);
    int fail = new_block(), ok = new_block();
    br(R(cmp(CC_NE, code, IMM(0))), fail, ok);
    set_blk(fail);
    err_return(code);
    set_blk(ok);
    return eu_value(v);
}

/* x catch value / x catch |e| expr / x catch |e| { ... } */
static Val ex_catch(Node *n) {
    Val v = expr(n->a, NULL);
    if (v.t->k != TY_ERRUNION) error_at(n->tok, "'catch' needs something that can fail (!T), but this is %s", tname(v.t));
    Type *et = v.t->elem;
    Opd code = eu_err(v);
    int ok = new_block(), fail = new_block(), join = new_block();
    br(R(cmp(CC_NE, code, IMM(0))), fail, ok);
    set_blk(fail);
    push_scope(false, -1, -1);
    if (n->name) bind_err(n->name, n->tok, v);
    Val d = V_void();
    bool dead;
    if (n->b->k == N_BLOCK) {
        block_body(n->b);
        dead = terminated();
        if (!dead && et != ty_void)
            error_at(n->b->tok, "this catch block must produce a value of type %s or leave with return, break or continue", tname(et));
    } else {
        d = expr(n->b, et);
        dead = d.t == ty_noreturn;
        if (!dead && et == ty_void && d.t != ty_void)
            error_at(n->b->tok, "the call gives no value, so the catch has nothing to replace (write 'catch {}' to ignore the error)");
        if (!dead && et != ty_void) d = coerce(d, et, n->b->tok, "the 'catch' value");
    }
    pop_scope();
    int dend = C->blk;
    if (et == ty_void) {
        if (!dead) jmp(join);
        set_blk(ok);
        jmp(join);
        set_blk(join);
        return V_void();
    }
    if (is_agg(et)) {
        Addr ra = temp(et);
        set_blk(ok);
        copy_agg(et, ra, eu_value(v).a);
        jmp(join);
        if (!dead) {
            set_blk(dend);
            copy_agg(et, ra, d.a);
            jmp(join);
        }
        set_blk(join);
        return V_agg(et, ra, true);
    }
    int r = new_vreg(cls_of(et));
    set_blk(ok);
    mov(r, cls_of(et), opd_of(eu_value(v)));
    jmp(join);
    if (!dead) {
        set_blk(dend);
        mov(r, cls_of(et), opd_of(d));
        jmp(join);
    }
    set_blk(join);
    return V_reg(et, r);
}

/* an error's name, as a str */
static Val err_name_str(Opd code) {
    int p = callv("_rt_error_name", C_I, 1, code, C_I);
    int l = callv("_strlen", C_I, 1, R(p), C_I);
    Addr t = temp(ty_str);
    store(ptr_to(ty_u8), t, R(p));
    store(ty_i64, A_add(t, 8), R(l));
    return V_agg(ty_str, t, true);
}

/* ---------- @format: compile-time checked formatting (design section 4.14) ---------- */

/* like `try v`: a failed v returns its error from the current function */
static Val try_val(Val v, Token *tok) {
    FnInfo *f = C->fn;
    if (C->ct_scratch) error_at(tok, "this passes errors up, which would leave the comptime expression; use it inside a function");
    if (C->defer_depth) error_at(tok, "@format passes write errors up, so it can't be used directly in a defer");
    if (!f->ret || f->ret->k != TY_ERRUNION)
        error_at(tok, "this passes write errors up, so '%s' must return an error union (like -> !void)", f->name);
    if (!set_subset(v.t->eset, f->ret->eset))
        error_at(tok, "this can fail with %s errors, but '%s' returns %s", tname(v.t->eset), f->name, tname(f->ret));
    Opd code = eu_err(v);
    int fail = new_block(), ok = new_block();
    br(R(cmp(CC_NE, code, IMM(0))), fail, ok);
    set_blk(fail);
    err_return(code);
    set_blk(ok);
    return eu_value(v);
}

/* what the compiler itself calls in std (whatever the program imports, and ignoring pub) */
static Unit *std_unit_named(const char *mod) {
    const char *path = strf("std.%s", mod);
    for (int i = 0; i < NUNITS; i++)
        if (!strcmp(UNITS[i]->path, path)) return UNITS[i];
    error_at(NULL, "internal error: std.%s isn't loaded", mod);
}

static Type *std_type(const char *mod, const char *name) {
    Type *t = type_in(std_unit_named(mod), name);
    if (!t) error_at(NULL, "internal error: std.%s.%s is missing", mod, name);
    return t;
}

static FnInfo *std_fn(const char *mod, const char *name) {
    FnInfo *f = fn_in(std_unit_named(mod), name);
    if (!f) error_at(NULL, "internal error: std.%s.%s is missing", mod, name);
    return f;
}

/* fmt.<name>(args...) from std.fmt, as `try` */
static void fmt_call(const char *name, Val *args, int n, Token *tok) {
    FnInfo *m = std_fn("fmt", name);
    Token *toks[8];
    for (int i = 0; i < n; i++) toks[i] = tok;
    try_val(call_core(m, args, toks, n, tok), tok);
}

static void fmt_write(Val w, Val s, Token *tok) {
    FnInfo *m = find_method(std_type("fmt", "Writer"), "writeAll");
    Val args[2] = {w, s};
    Token *toks[2] = {tok, tok};
    try_val(call_core(m, args, toks, 2, tok), tok);
}

static void fmt_lit(Val w, const char *s, int len, Token *tok) {
    if (len) fmt_write(w, str_lit(s, len), tok);
}

typedef struct {
    const char *spec; /* "", "d", "x", "X", "b", "o", "s", "c", "e", "*", "any" */
    int64_t width, prec;
    bool has_prec;
    char fill, align;
} FmtSpec;

static Val fmt_opts(FmtSpec *sp, int base, bool upper, Token *tok) {
    Type *ot = std_type("fmt", "FmtOpts");
    layout(ot, tok);
    Addr a = temp(ot);
    zero_at(ot, a);
    struct { const char *f; int64_t v; } set[] = {
        {"width", sp->width}, {"prec", sp->prec}, {"has_prec", sp->has_prec}, {"fill", (unsigned char)sp->fill},
        {"align", (unsigned char)sp->align}, {"base", base}, {"upper", upper},
    };
    for (size_t i = 0; i < sizeof set / sizeof set[0]; i++)
        for (int j = 0; j < ot->nfields; j++)
            if (!strcmp(ot->fields[j].name, set[i].f)) store(ot->fields[j].t, A_add(a, ot->fields[j].off), IMM(set[i].v));
    return V_agg(ot, a, true);
}

static void fmt_value(Val w, Val v, FmtSpec *sp, Token *tok, int depth);

/* { a, b, c }: the elements of an array or slice, through a loop */
static void fmt_elems(Val w, Type *et, int p, Opd len, Token *tok, int depth) {
    if (len.k == O_IMM && len.v == 0) {
        fmt_lit(w, "{}", 2, tok);
        return;
    }
    fmt_lit(w, "{", 1, tok);
    int islot = new_slot(ty_usize);
    store(ty_usize, A_slot(islot, 0), IMM(0));
    int body = new_block(), exit = new_block();
    Opd l = len.k == O_IMM ? R(iconst(len.v)) : len;
    br(R(cmp(CC_NE, l, IMM(0))), body, exit);
    set_blk(body);
    int i = load(ty_usize, A_slot(islot, 0));
    int first = new_block(), rest = new_block(), elem = new_block();
    br(R(cmp(CC_EQ, R(i), IMM(0))), first, rest);
    set_blk(rest);
    fmt_lit(w, ", ", 2, tok);
    jmp(elem);
    set_blk(first);
    fmt_lit(w, " ", 1, tok);
    jmp(elem);
    set_blk(elem);
    FmtSpec inner = {"", 0, 0, false, ' ', 0};
    fmt_value(w, load_val(et, elem_addr(p, V_reg(ty_usize, i), et->size)), &inner, tok, depth + 1);
    int i2 = op2(I_ADD, C_I, R(load(ty_usize, A_slot(islot, 0))), IMM(1));
    store(ty_usize, A_slot(islot, 0), R(i2));
    br(R(cmp(CC_LO, R(i2), l)), body, exit);
    set_blk(exit);
    int some = new_block(), none = new_block(), done = new_block();
    br(R(cmp(CC_NE, l, IMM(0))), some, none);
    set_blk(some);
    fmt_lit(w, " }", 2, tok);
    jmp(done);
    set_blk(none);
    fmt_lit(w, "}", 1, tok);
    jmp(done);
    set_blk(done);
}

static int fmt_argno; /* the argument being formatted (1-based), for error messages */

static const char *fmt_arg(void) { return fmt_argno ? strf(" (argument %d)", fmt_argno) : ""; }

static _Noreturn void fmt_bad(Token *tok, FmtSpec *sp, Type *t) {
    error_at(tok, "format string: '{%s}' can't format %s%s", sp->spec, tname(t), fmt_arg());
}

static void fmt_value(Val w, Val v, FmtSpec *sp, Token *tok, int depth) {
    Type *t = v.t;
    const char *s = sp->spec;
    bool dflt = !*s || !strcmp(s, "any");
    if (t->k == TY_OPT) {
        int yes = new_block(), no = new_block(), join = new_block();
        br(R(opt_has(v)), yes, no);
        set_blk(yes);
        fmt_value(w, opt_value(v), sp, tok, depth);
        jmp(join);
        set_blk(no);
        Val args[3] = {w, str_lit("null", 4), fmt_opts(sp, 10, false, tok)};
        fmt_call("formatStr", args, 3, tok);
        jmp(join);
        set_blk(join);
        return;
    }
    if (t == ty_errlit) v = defaulted(v), t = v.t;
    if (is_int(t)) {
        v = defaulted(v);
        t = v.t;
        if (!strcmp(s, "c")) {
            if (t->k != TY_U8) error_at(tok, "format string: '{c}' prints a byte (u8), not %s%s", tname(t), fmt_arg());
            Val args[3] = {w, v, fmt_opts(sp, 10, false, tok)};
            fmt_call("formatByte", args, 3, tok);
            return;
        }
        int base = !strcmp(s, "x") || !strcmp(s, "X") ? 16 : !strcmp(s, "b") ? 2 : !strcmp(s, "o") ? 8 : 10;
        if (!dflt && strcmp(s, "d") && base == 10) fmt_bad(tok, sp, t);
        bool sg = is_signed(t);
        Val args[3] = {w, convert(v, sg ? ty_i64 : ty_u64), fmt_opts(sp, base, !strcmp(s, "X"), tok)};
        fmt_call(sg ? "formatInt" : "formatUint", args, 3, tok);
        return;
    }
    if (is_float(t)) {
        if (!dflt && strcmp(s, "d") && strcmp(s, "e")) fmt_bad(tok, sp, t);
        bool f32 = t->k == TY_F32;
        Val args[3] = {w, f32 ? v : convert(defaulted(v), ty_f64), fmt_opts(sp, 10, !strcmp(s, "e"), tok)};
        fmt_call(f32 ? "formatFloat32" : "formatFloat", args, 3, tok);
        return;
    }
    if (t == ty_bool) {
        if (!dflt) fmt_bad(tok, sp, t);
        Val args[3] = {w, v, fmt_opts(sp, 10, false, tok)};
        fmt_call("formatBool", args, 3, tok);
        return;
    }
    if (is_bytes(t) && (!strcmp(s, "s") || (!*s && depth > 0))) {
        if (depth > 0 && !*s) fmt_lit(w, "\"", 1, tok);
        Val args[3] = {w, coerce(v, ty_str, tok, "a string"), fmt_opts(sp, 10, false, tok)};
        fmt_call("formatStr", args, 3, tok);
        if (depth > 0 && !*s) fmt_lit(w, "\"", 1, tok);
        return;
    }
    if (is_bytes(t) && (!strcmp(s, "x") || !strcmp(s, "X"))) {
        Val args[3] = {w, coerce(v, ty_str, tok, "bytes"), fmt_opts(sp, 16, !strcmp(s, "X"), tok)};
        fmt_call("formatHex", args, 3, tok);
        return;
    }
    if (is_bytes(t) && !*s)
        error_at(tok, "format string: use '{s}' to print %s as text, or '{any}' for its bytes as numbers%s", tname(t), fmt_arg());
    if (t == ty_cstr && !strcmp(s, "s")) {
        Val args[3] = {w, v, fmt_opts(sp, 10, false, tok)};
        fmt_call("formatCStr", args, 3, tok);
        return;
    }
    if (t->k == TY_ENUM) {
        if (!dflt && strcmp(s, "s")) fmt_bad(tok, sp, t);
        Val args[3] = {w, tag_name(v), fmt_opts(sp, 10, false, tok)};
        fmt_call("formatStr", args, 3, tok);
        return;
    }
    if (t->k == TY_ERRSET) {
        if (!dflt && strcmp(s, "s")) fmt_bad(tok, sp, t);
        fmt_lit(w, "error.", 6, tok);
        Val args[3] = {w, err_name_str(opd_of(v)), fmt_opts(sp, 10, false, tok)};
        fmt_call("formatStr", args, 3, tok);
        return;
    }
    if (is_ptr(t) || is_fn(t) || is_optptr(t)) {
        if (!dflt && strcmp(s, "*")) fmt_bad(tok, sp, t);
        Val args[3] = {w, V_reg(ty_u64, reg_of(v)), fmt_opts(sp, 16, false, tok)};
        fmt_call("formatPtr", args, 3, tok);
        return;
    }
    if (t->k == TY_ERRUNION) {
        if (s[0] != '!')
            error_at(tok, "format string: %s might be an error; handle it first with 'try' or 'catch', or print either with '{!}'%s",
                     tname(t), fmt_arg());
        FmtSpec inner = *sp;
        inner.spec = s + 1;
        Opd code = eu_err(v);
        int ok = new_block(), bad = new_block(), join = new_block();
        br(R(cmp(CC_EQ, code, IMM(0))), ok, bad);
        set_blk(ok);
        if (t->elem == ty_void) fmt_lit(w, "void", 4, tok);
        else fmt_value(w, eu_value(v), &inner, tok, depth);
        jmp(join);
        set_blk(bad);
        Val e = code.k == O_IMM ? V_int(t->eset, code.v) : V_reg(t->eset, (int)code.v);
        inner.spec = "";
        fmt_value(w, e, &inner, tok, depth);
        jmp(join);
        set_blk(join);
        return;
    }
    if (s[0] == '!') error_at(tok, "format string: '{%s}' is for error unions, but this is %s%s", s, tname(t), fmt_arg());
    if (!dflt) fmt_bad(tok, sp, t);
    if (depth > 16) error_at(tok, "format string: %s is nested too deeply to print", tname(t));
    if (is_array(t)) {
        layout(t, tok);
        fmt_elems(w, t->elem, addr_reg(v.a), IMM(t->alen), tok, depth);
        return;
    }
    if (is_slice(t)) {
        fmt_elems(w, t->elem, load(ptr_to(t->elem), v.a), R(load(ty_usize, A_add(v.a, 8))), tok, depth);
        return;
    }
    if (t->k == TY_STRUCT && !t->packed && !t->is_union) {
        layout(t, tok);
        if (t->tuple) fmt_lit(w, t->nfields ? ".{ " : ".{", t->nfields ? 3 : 2, tok);
        else {
            fmt_lit(w, t->name, (int)strlen(t->name), tok);
            fmt_lit(w, t->nfields ? "{ " : "{", t->nfields ? 2 : 1, tok);
        }
        for (int i = 0; i < t->nfields; i++) {
            Field *fd = &t->fields[i];
            if (i) fmt_lit(w, ", ", 2, tok);
            if (!t->tuple) {
                fmt_lit(w, fd->name, (int)strlen(fd->name), tok);
                fmt_lit(w, ": ", 2, tok);
            }
            FmtSpec inner = {"", 0, 0, false, ' ', 0};
            fmt_value(w, load_val(fd->t, A_add(v.a, fd->off)), &inner, tok, depth + 1);
        }
        fmt_lit(w, t->nfields ? " }" : "}", t->nfields ? 2 : 1, tok);
        return;
    }
    error_at(tok, "format string: can't format %s yet (format its fields)%s", tname(t), fmt_arg());
}

/* @format(writer, "x = {}\n", .{x}) */
static Val ex_format(Node *n) {
    if (n->n != 3) error_at(n->tok, "@format takes a Writer, a format string and a tuple of arguments");
    Val w = expr(n->list[0], NULL);
    if (w.t != std_type("fmt", "Writer")) error_at(n->list[0]->tok, "@format writes to a Writer, not %s", tname(w.t));
    Node *fn = n->list[1];
    Val fv = expr(fn, ty_str);
    if (!fv.strlit) error_at(fn->tok, "the format string must be known at compile time (a string literal)");
    Token *ftok = origin_of(fn);
    const char *f = M->strs[fv.strlit - 1].data;
    int flen = M->strs[fv.strlit - 1].len;
    Val args = expr(n->list[2], NULL);
    if (args.t->k != TY_STRUCT || !args.t->tuple)
        error_at(n->list[2]->tok, "the arguments go in a tuple: .{ x, y } (or .{} for none), not %s", tname(args.t));
    layout(args.t, n->tok);
    if (!args.fresh) { /* the writes below may change what the arguments point into: copy them first */
        Addr c = temp(args.t);
        copy_agg(args.t, c, args.a);
        args = V_agg(args.t, c, true);
    }
    Addr wa = temp(w.t); /* the writer is used once per piece */
    copy_agg(w.t, wa, w.a);
    w = V_agg(w.t, wa, false);
    int next = 0;
    char *lit = xalloc((size_t)flen + 1);
    int nl = 0;
    for (int i = 0; i < flen; i++) {
        char c = f[i];
        if (c == '}') {
            if (i + 1 < flen && f[i + 1] == '}') {
                lit[nl++] = '}';
                i++;
                continue;
            }
            error_at(ftok, "format string: unmatched '}' (write '}}' for a literal brace)");
        }
        if (c != '{') {
            lit[nl++] = c;
            continue;
        }
        if (i + 1 < flen && f[i + 1] == '{') {
            lit[nl++] = '{';
            i++;
            continue;
        }
        int j = i + 1;
        while (j < flen && f[j] != '}') j++;
        if (j >= flen) error_at(ftok, "format string: unclosed '{' (write '{{' for a literal brace)");
        const char *ph = xstrndup(f + i + 1, (size_t)(j - i - 1));
        i = j;
        FmtSpec sp = {"", 0, 0, false, ' ', 0};
        const char *colon = strchr(ph, ':');
        sp.spec = colon ? xstrndup(ph, (size_t)(colon - ph)) : ph;
        static const char *specs[] = {"", "d", "x", "X", "b", "o", "s", "c", "e", "*", "any", NULL};
        bool known = false;
        for (const char **k = specs; *k; k++) known |= !strcmp(*k, sp.spec + (sp.spec[0] == '!'));
        if (!known)
            error_at(ftok, "format string: unknown '{%s}' (use {}, {d}, {x}, {X}, {b}, {o}, {s}, {c}, {e}, {*} or {any}, "
                           "after '!' for an error union, and optionally followed by :[fill][<>^][width][.precision])", ph);
        if (colon) {
            const char *o = colon + 1;
            if (o[0] && o[1] && strchr("<>^", o[1])) sp.fill = o[0], sp.align = o[1], o += 2;
            else if (o[0] && strchr("<>^", o[0])) sp.align = o[0], o++;
            while (*o >= '0' && *o <= '9') sp.width = sp.width * 10 + (*o++ - '0');
            if (*o == '.') {
                o++;
                sp.has_prec = true;
                if (!(*o >= '0' && *o <= '9')) error_at(ftok, "format string: '.' must be followed by a precision in '{%s}'", ph);
                while (*o >= '0' && *o <= '9') sp.prec = sp.prec * 10 + (*o++ - '0');
            }
            if (*o) error_at(ftok, "format string: can't read the options in '{%s}' (they're :[fill][<>^][width][.precision])", ph);
        }
        if (next >= args.t->nfields)
            error_at(ftok, "format string has more placeholders than arguments (%d given)", args.t->nfields);
        fmt_lit(w, lit, nl, ftok);
        nl = 0;
        Field *fd = &args.t->fields[next++];
        fmt_argno = next;
        fmt_value(w, load_val(fd->t, A_add(args.a, fd->off)), &sp, ftok, 0);
        fmt_argno = 0;
    }
    fmt_lit(w, lit, nl, ftok);
    if (next < args.t->nfields)
        error_at(ftok, "format string uses %d argument(s), but %d are given", next, args.t->nfields);
    return V_void();
}

/* @hash(x): a 64-bit hash of an integer, bool, enum, pointer or string (HashMap's keys) */
static Val ex_hash(Node *n) {
    if (n->n != 1) error_at(n->tok, "@hash takes one value");
    Val v = defaulted(expr(n->list[0], NULL));
    Token *toks[1] = {n->tok};
    if (is_bytes(v.t)) {
        Val a[1] = {coerce(v, ty_str, n->tok, "@hash")};
        return call_core(std_fn("hash", "bytes"), a, toks, 1, n->tok);
    }
    if (!(is_int(v.t) || v.t == ty_bool || is_plain_enum(v.t) || is_ptr(v.t) || is_optptr(v.t)))
        error_at(n->list[0]->tok, "@hash works on integers, bools, plain enums, pointers and strings, not %s", tname(v.t));
    Val a[1] = {V_reg(ty_u64, reg_of(v))};
    return call_core(std_fn("hash", "int"), a, toks, 1, n->tok);
}

/* @eql(a, b): == for integers, bools, enums and pointers; the bytes for strings */
static Val ex_eql(Node *n) {
    if (n->n != 2) error_at(n->tok, "@eql takes two values");
    Val a = expr(n->list[0], NULL);
    Val b = expr(n->list[1], is_lit(a.t) ? NULL : a.t);
    if (is_bytes(a.t) && is_bytes(b.t)) {
        Token *toks[2] = {n->tok, n->tok};
        Val args[2] = {coerce(a, ty_str, n->tok, "@eql"), coerce(b, ty_str, n->tok, "@eql")};
        return call_core(std_fn("hash", "eqlBytes"), args, toks, 2, n->tok);
    }
    if (is_agg(a.t) || is_agg(b.t)) error_at(n->tok, "@eql compares integers, bools, plain enums, pointers and strings, not %s", tname(a.t));
    return binop("==", a, b, n->tok);
}

/* a orelse b: a's value if it's there, else b (which may be another ?T, or return/break/...) */
static Val ex_orelse(Node *n) {
    Val v = expr(n->a, NULL);
    if (v.t->k != TY_OPT) error_at(n->tok, "'orelse' needs an optional (?T) on its left, found %s", tname(v.t));
    Type *et = v.t->elem;
    int has = new_block(), none = new_block(), join = new_block();
    br(R(opt_has(v)), has, none);
    set_blk(none);
    Val d;
    if (n->b->k == N_BLOCK) { /* x orelse { ...; return; }: the block has no value, so it must leave */
        push_scope(false, -1, -1);
        block_body(n->b);
        pop_scope();
        if (!terminated())
            error_at(n->b->tok, "an orelse block has no value to give, so it must leave with return, break or continue");
        d = V_reg(ty_noreturn, -1);
    } else {
        d = expr(n->b, et);
    }
    int dend = C->blk;
    bool chain = d.t == v.t;
    bool dead = d.t == ty_noreturn;
    Type *rt = chain ? v.t : et;
    if (!dead && !chain) d = coerce(d, et, n->b->tok, "the 'orelse' default");
    if (is_agg(rt)) {
        Addr ra = temp(rt);
        set_blk(has);
        copy_agg(rt, ra, chain ? v.a : opt_value(v).a);
        jmp(join);
        if (!dead) {
            set_blk(dend);
            copy_agg(rt, ra, d.a);
            jmp(join);
        }
        set_blk(join);
        return V_agg(rt, ra, true);
    }
    int r = new_vreg(cls_of(rt));
    set_blk(has);
    mov(r, cls_of(rt), opd_of(chain ? v : opt_value(v)));
    jmp(join);
    if (!dead) {
        set_blk(dend);
        mov(r, cls_of(rt), opd_of(d));
        jmp(join);
    }
    set_blk(join);
    return V_reg(rt, r);
}

/* ---------- dispatch ---------- */

static Val expr_in(Node *n, Type *want);
static Val expr(Node *n, Type *want) { /* names in n resolve in the module n was written in */
    Unit *su = CU;
    Token *st = CUR_TOK;
    if (n->unit) CU = n->unit;
    CUR_TOK = n->tok;
    Val v = expr_in(n, want);
    CU = su;
    CUR_TOK = st;
    return v;
}

static Val expr_in(Node *n, Type *want) {
    switch (n->k) {
    case N_INT:
        if (n->ival > (uint64_t)INT64_MAX) return V_int(ty_u64, (int64_t)n->ival);
        return V_int(ty_litint, (int64_t)n->ival);
    case N_FLOAT: return V_flt(ty_litfloat, n->fval);
    case N_BOOL: return V_int(ty_bool, (int64_t)n->ival);
    case N_NULL: return V_int(ty_null, 0);
    case N_STR: return str_lit(n->str, n->slen);
    case N_BUILTIN: return ex_builtin(n, want);
    case N_UNDEFINED: error_at(n->tok, "'undefined' can only be the initial value of a 'var' or a struct field");
    case N_TYPEEXPR: /* a type as a comptime value: let P = *Node; */
        if (n->a->k == N_TPTR && !n->a->flag2 && n->a->a->k == N_TNAME && !prim_type(n->a->a->name) && !find_struct(n->a->a->name))
            error_at(n->tok, "dereference is written after the pointer in Flint: 'p.*'");
        return V_int(ty_type, type_id(resolve(n->a)));
    case N_STRUCT:
        if (!C || !C->fn || !C->fn->ct_fn) error_at(n->tok, "'struct { ... }' can only be made in a function that returns 'type'");
        return V_int(ty_type, type_id(ct_struct_expr(n)));
    case N_COMPTIME: return ct_eval(n->a, want, "the comptime value");
    case N_NAME: {
        if (!find_var(n->name)) {
            FnInfo *f = find_fn(n->name);
            if (f) return fn_value(f, n->tok);
            if (is_builtin(n->name)) error_at(n->tok, "'%s' is a builtin, not a function value; call it with ()", n->name);
            Type *tt = prim_type(n->name);
            if (!tt) {
                tt = find_struct(n->name);
                if (tt) check_type_pub(tt, n->tok);
            }
            if (tt) return V_int(ty_type, type_id(tt)); /* a type as a comptime value */
        }
        Var *v = lookup(n->name, n->tok);
        if (v->kind == VK_TYPE) return V_int(ty_type, type_id(v->t));
        if (v->kind == VK_CONST) return is_float(v->t) ? V_flt(v->t, v->cfval) : V_int(v->t, v->cival);
        if (v->kind == VK_CSTR) return str_lit(v->sdata, v->slen);
        LV l = lval(n);
        return lv_load(l);
    }
    case N_FIELDACC: {
        Unit *mu = unit_ref(n->a);
        if (mu) { /* io.stdout, fmt.format as a value */
            Var *gv = module_var(n);
            if (gv) {
                if (gv->kind == VK_TYPE) return V_int(ty_type, type_id(gv->t));
                if (gv->kind == VK_CONST) return is_float(gv->t) ? V_flt(gv->t, gv->cfval) : V_int(gv->t, gv->cival);
                return lv_load(lval(n));
            }
            FnInfo *mf = callee_fn(n);
            if (mf) return fn_value(mf, n->tok);
            if (type_in(mu, n->name)) {
                check_type_pub(type_in(mu, n->name), n->tok);
                return V_int(ty_type, type_id(type_in(mu, n->name)));
            }
            no_member(mu, n);
        }
        if (header_ref(n->a)) return c_value(header_ref(n->a), n);
    }
        if (is_static_ref(n->a) && static_var(static_type(n->a), n->name)) { /* heap.page_allocator */
            Var *sv = static_var(static_type(n->a), n->name);
            if (sv->kind == VK_CONST) return is_float(sv->t) ? V_flt(sv->t, sv->cfval) : V_int(sv->t, sv->cival);
            return lv_load(lval(n));
        }
        if (is_static_ref(n->a) && find_method(static_type(n->a), n->name)) /* Vec2.len as a value */
            return fn_value(find_method(static_type(n->a), n->name), n->tok);
        if (is_static_ref(n->a) && static_type(n->a)->k == TY_ENUM)
            return enum_value(static_type(n->a), n->name, 0, NULL, 0, n->tok);
        if (is_static_ref(n->a) && static_type(n->a)->k == TY_ERRSET) { /* FileError.NotFound */
            Type *set = static_type(n->a);
            int code = err_code(n->name);
            if (!set_has(set, code)) error_at(n->tok, "%s has no error %s", set->name, n->name);
            return V_int(set, code);
        }
        if (is_static_ref(n->a) && static_type(n)) return V_int(ty_type, type_id(static_type(n))); /* Outer.Inner */
        /* fall through */
    case N_DEREF:
    case N_INDEX: {
        LV l = lval(n);
        return lv_load(l);
    }
    case N_UNARY: return ex_unary(n, want);
    case N_BINARY: return ex_binary(n, want);
    case N_TERNARY: return ex_ternary(n, want);
    case N_CAST: return ex_cast(n);
    case N_CALL: return ex_call(n, want);
    case N_SLICE: return ex_slice(n);
    case N_STRUCTLIT: return ex_structlit(n);
    case N_ARRAY:
        error_at(n->tok, "'[a, b, ...]' allocated on the heap, so it was removed: write a stack array [_]T { a, b, ... } "
                         "(or .{ a, b } where the type is known), or build an ArrayList with an allocator");
    case N_ARRAYLIT: return ex_arraylit(n);
    case N_ANONLIT: return ex_anon(n, want);
    case N_DOTTAG: {
        Type *et = n->name2 ? find_struct(n->name2) : want;
        if (n->name2) check_type_pub(et, n->tok);
        if (et && et->k == TY_ERRSET && !n->name2 && !n->flag) { /* .NotFound where an error is expected */
            int code = err_code(n->name);
            if (!set_has(et, code)) error_at(n->tok, "%s has no error %s", tname(et), n->name);
            return V_int(et, code);
        }
        if (!et || et->k != TY_ENUM)
            error_at(n->tok, "can't tell which enum '.%s' belongs to here; write the type: E.%s", n->name, n->name);
        return enum_value(et, n->name, n->flag, n->list, n->n, n->tok);
    }
    case N_SWITCH: return lower_switch(n, want);
    case N_UNWRAP: return ex_unwrap(n);
    case N_TRY: return ex_try(n);
    case N_CATCH: return ex_catch(n);
    case N_ERRLIT: return V_int(ty_errlit, err_code(n->name));
    case N_ORELSE: return ex_orelse(n);
    case N_UNREACHABLE: {
        Val msg = str_lit("reached unreachable code", 24);
        callv("_rt_panic", -1, 2, R(load(ptr_to(ty_u8), msg.a)), C_I, IMM(24), C_I);
        trap();
        return V_reg(ty_noreturn, -1);
    }
    case N_RETURN:
    case N_BREAK:
    case N_CONTINUE:
        stmt(n);
        return V_reg(ty_noreturn, -1);
    default: error_at(n->tok, "expected an expression");
    }
}

/* branch to t if n is true, else f */
static void cond_in(Node *n, int t, int f);
static void cond(Node *n, int t, int f) {
    Unit *su = CU;
    if (n->unit) CU = n->unit;
    cond_in(n, t, f);
    CU = su;
}

static void cond_in(Node *n, int t, int f) {
    if (n->k == N_BINARY && !strcmp(n->name, "&&")) {
        int m = new_block();
        cond(n->a, m, f);
        set_blk(m);
        cond(n->b, t, f);
        return;
    }
    if (n->k == N_BINARY && !strcmp(n->name, "||")) {
        int m = new_block();
        cond(n->a, t, m);
        set_blk(m);
        cond(n->b, t, f);
        return;
    }
    if (n->k == N_UNARY && !strcmp(n->name, "!")) {
        cond(n->a, f, t);
        return;
    }
    Val v = expr(n, NULL);
    if (v.t->k == TY_OPT)
        error_at(n->tok, "this condition is %s, not bool; unwrap it with 'if (x) |v| { ... }' or compare it: 'x != null'", tname(v.t));
    if (v.t->k == TY_ERRUNION)
        error_at(n->tok, "this condition is %s, not bool; use 'if (x) |v| { ... } else |err| { ... }'", tname(v.t));
    if (v.t != ty_bool) error_at(n->tok, "condition must be bool, found %s", tname(v.t));
    if (v.isconst) {
        jmp(v.ival ? t : f);
        return;
    }
    br(R(v.reg), t, f);
}

/* ====================================================================== */
/* statements                                                             */
/* ====================================================================== */

static void block_body(Node *b);

/* run the defers of every scope from the innermost up to `stop` (NULL: all).
 * errdefers run only when `err` is given, i.e. when leaving with an error. */
static void emit_defers_err(Scope *stop, Val *err) {
    for (Scope *s = C->cur; s; s = s->parent) {
        for (int i = s->nd - 1; i >= 0; i--) {
            Node *dn = s->defers[i];
            if (dn->flag && !err) continue;
            Scope *saved = C->cur;
            int saved_unsafe = C->unsafe_depth;
            C->cur = s;
            C->unsafe_depth = s->unsafe_depth;
            push_scope(false, -1, -1);
            if (dn->flag && dn->name && strcmp(dn->name, "_")) {
                Var *ev = new_local(dn->name, err->t, dn->tok, true);
                store(err->t, A_slot(ev->slot, 0), opd_of(*err));
            }
            Node *d = dn->a;
            C->defer_depth++;
            if (d->k == N_BLOCK)
                for (int j = 0; j < d->n; j++) stmt(d->list[j]);
            else
                stmt(d);
            C->defer_depth--;
            C->cur = saved;
            C->unsafe_depth = saved_unsafe;
        }
        if (s == stop) break;
    }
}

static void emit_defers(Scope *stop) { emit_defers_err(stop, NULL); }

static bool ends_in_jump(Node *b) {
    if (!b->n) return false;
    NodeKind k = b->list[b->n - 1]->k;
    return k == N_RETURN || k == N_BREAK || k == N_CONTINUE;
}

static void block_body(Node *b) {
    for (int i = 0; i < b->n; i++) stmt(b->list[i]);
    if (!ends_in_jump(b)) emit_defers(C->cur);
}

static void sub_block(Node *b, bool loop, int brk, int cont) {
    push_scope(loop, brk, cont);
    block_body(b);
    pop_scope();
}

/* let n = 42;  ->  a compile-time constant, inlined at every use */
static void declare_const(const char *name, Val v, Token *tok) {
    Scope *s = C->cur;
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->vars[i].name, name)) error_at(tok, "'%s' is already declared in this scope", name);
    Var var;
    memset(&var, 0, sizeof var);
    var.name = name;
    var.t = v.t;
    var.kind = VK_CONST;
    var.is_const = true;
    var.cival = v.ival;
    var.cfval = v.fval;
    var.slot = var.reg = -1;
    PUSH(s->vars, s->n, s->cap, var);
}

static void declare_type_alias(const char *name, Type *t, Token *tok) {
    Scope *s = C->cur;
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->vars[i].name, name)) error_at(tok, "'%s' is already declared in this scope", name);
    Var var;
    memset(&var, 0, sizeof var);
    var.name = name;
    var.t = t;
    var.kind = VK_TYPE;
    var.is_const = true;
    var.slot = var.reg = -1;
    PUSH(s->vars, s->n, s->cap, var);
}

/* does e use only names known at compile time (no runtime variables)? */
static bool refs_only_comptime(Node *e) {
    if (!e) return true;
    if (e->k == N_NAME) {
        Var *v = find_var(e->name);
        return !v || v->kind == VK_CONST || v->kind == VK_CSTR || v->kind == VK_TYPE || v->kind == VK_GLOBAL || v->kind == VK_LAZY;
    }
    if (e->k == N_STRUCT) return false; /* a type made here is made by this function's own lowering */
    if (!refs_only_comptime(e->a) || !refs_only_comptime(e->b) || !refs_only_comptime(e->c)) return false;
    for (int i = 0; i < e->n; i++)
        if (!refs_only_comptime(e->list[i])) return false;
    return true;
}

static Var *new_local(const char *name, Type *t, Token *tok, bool is_const) {
    Var *v = declare(name, t, tok, is_const);
    v->kind = VK_SLOT;
    v->slot = new_slot(t);
    return v;
}

static void add_retptr(void);

/* the local variable (or by-value parameter) whose memory `x` is part of, or NULL: x, x.f, x[i]
 * where every step stays inside the variable (no pointer or slice in between) */
static Var *local_root(Node *x, Type **t) {
    if (x->k == N_NAME) {
        Var *v = find_var(x->name);
        if (!v || !(v->kind == VK_SLOT || (v->kind == VK_REF && v->param_copy))) return NULL;
        *t = v->t;
        return v;
    }
    Type *bt;
    if (x->k == N_FIELDACC) {
        Var *v = local_root(x->a, &bt);
        if (!v || bt->k != TY_STRUCT) return NULL;
        for (int i = 0; i < bt->nfields; i++)
            if (!strcmp(bt->fields[i].name, x->name)) {
                *t = bt->fields[i].t;
                return v;
            }
        return NULL;
    }
    if (x->k == N_INDEX) {
        Var *v = local_root(x->a, &bt);
        if (!v || !is_array(bt)) return NULL;
        *t = bt->elem;
        return v;
    }
    return NULL;
}

static bool always_returns(Node *b);
static bool arm_ends_with_fallthrough(Node *arm);

static bool has_break(Node *b) {
    for (int i = 0; i < b->n; i++) {
        Node *s = b->list[i];
        if (s->k == N_BREAK) return true;
        if (s->k == N_SWITCH)
            for (int j = 0; j < s->n; j++) {
                Node *body = s->list[j]->a;
                if (body->k == N_BREAK || (body->k == N_BLOCK && has_break(body))) return true;
            }
        if (s->k == N_IF && (has_break(s->b) || (s->c && has_break(s->c)))) return true;
        if (s->k == N_BLOCK && has_break(s)) return true;
    }
    return false;
}

static bool always_returns(Node *b) {
    for (int i = 0; i < b->n; i++) {
        Node *s = b->list[i];
        if (s->k == N_RETURN) return true;
        if (s->k == N_IF && s->c && always_returns(s->b) && always_returns(s->c)) return true;
        if (s->k == N_BLOCK && always_returns(s)) return true;
        if (s->k == N_SWITCH && !s->flag) { /* switches are exhaustive, so: every case returns */
            bool all = true;
            for (int j = 0; j < s->n && all; j++) {
                Node *arm = s->list[j];
                if (arm_ends_with_fallthrough(arm)) continue;
                all = arm->a->k == N_RETURN || (arm->a->k == N_BLOCK && always_returns(arm->a));
            }
            if (all) return true;
        }
        if (s->k == N_WHILE && s->a->k == N_BOOL && s->a->ival && !has_break(s->b)) return true;
        if (s->k == N_EXPRSTMT && s->a->k == N_CALL && s->a->a->k == N_NAME &&
            (!strcmp(s->a->a->name, "exit") || !strcmp(s->a->a->name, "panic")))
            return true;
        if (s->k == N_EXPRSTMT && s->a->k == N_UNREACHABLE) return true;
        if (s->k == N_EXPRSTMT && s->a->k == N_CALL) { /* a call of a function that returns noreturn */
            FnInfo *f = s->a->a->k == N_NAME && !find_var(s->a->a->name) ? find_fn(s->a->a->name) : callee_fn(s->a->a);
            if (f && f->ret == ty_noreturn) return true;
        }
    }
    return false;
}

static void stmt_in(Node *n);
static void stmt(Node *n) {
    Unit *su = CU;
    Token *st = CUR_TOK;
    if (n->unit) CU = n->unit;
    CUR_TOK = n->tok;
    stmt_in(n);
    CU = su;
    CUR_TOK = st;
}

static void stmt_in(Node *n) {
    switch (n->k) {
    case N_LET: {
        if (n->n) { /* let (a, b) = tuple; */
            Type *tt = n->a ? resolve(n->a) : NULL;
            Val tv = expr(n->b, tt);
            if (tt) tv = coerce(tv, tt, n->b->tok, "the destructured value");
            if (tv.t->k != TY_STRUCT || !tv.t->tuple)
                error_at(n->b->tok, "only tuples can be destructured, but this is %s", tname(tv.t));
            layout(tv.t, n->tok);
            if (tv.t->nfields != n->n)
                error_at(n->tok, "%s has %d elements, but %d names are given", tname(tv.t), tv.t->nfields, n->n);
            if (!tv.fresh) { /* copy first: the names may shadow what the value came from */
                Addr c = temp(tv.t);
                copy_agg(tv.t, c, tv.a);
                tv = V_agg(tv.t, c, true);
            }
            for (int i = 0; i < n->n; i++) {
                Node *nm = n->list[i];
                if (!strcmp(nm->name, "_")) continue;
                Field *fd = &tv.t->fields[i];
                Var *var = new_local(nm->name, fd->t, nm->tok, n->flag);
                store_val(fd->t, A_slot(var->slot, 0), load_val(fd->t, A_add(tv.a, fd->off)));
            }
            return;
        }
        Type *t = NULL;
        Val v = V_void();
        bool type_ann = n->a && n->a->k == N_TNAME && !strcmp(n->a->name, "type"); /* let T: type = ... */
        if (type_ann) t = ty_type;
        else if (n->a) {
            t = resolve(n->a);
            layout(t, n->a->tok);
        }
        if (n->b && n->b->k == N_UNDEFINED) {
            if (n->flag) error_at(n->b->tok, "a 'let' needs a real value; 'undefined' is only for 'var'");
            if (!t) error_at(n->tok, "'%s' needs a type to be undefined: 'var %s: T = undefined;'", n->name, n->name);
            Var *lv = new_local(n->name, t, n->tok, false);
            undefined_at(t, A_slot(lv->slot, 0));
            return;
        }
        if (n->b) {
            /* in a function that only runs at compile time, a let made of compile-time-known names is known now */
            if (C->fn && C->fn->ct_fn && n->flag && refs_only_comptime(n->b)) v = ct_eval(n->b, t, strf("'%s'", n->name));
            else v = expr(n->b, t);
            if (t) v = coerce(v, t, n->b->tok, strf("'%s'", n->name));
            if (v.t == ty_type) { /* let T = List(i32); a name for a type */
                if (!n->flag) error_at(n->tok, "'%s' would hold a type, which only exists at compile time: use 'let %s = ...;'", n->name, n->name);
                if (!v.isconst) error_at(n->b->tok, "a type must be known at compile time");
                declare_type_alias(n->name, type_from_id(v.ival), n->tok);
                return;
            }
            bool ct_init = (C->fn && C->fn->ct_fn) || n->b->k == N_COMPTIME; /* let n = comptime f(); is a constant */
            if (ct_init && n->flag && v.isconst && !is_agg(v.t) && v.t != ty_null) {
                declare_const(n->name, v, n->tok); /* usable in types: [n]u8 */
                return;
            }
            else {
                if (v.t == ty_null)
                    error_at(n->tok, "can't infer the type of '%s' from null; write 'let %s: *T = null'", n->name, n->name);
                if (v.t == ty_void) error_at(n->b->tok, "that expression doesn't produce a value");
                if (is_lit(v.t)) { /* an untyped number */
                    if (!n->flag)
                        error_at(n->tok, "give '%s' a type: 'var %s: %s = ...;' (an untyped number is a compile-time constant)",
                                 n->name, n->name, v.t->k == TY_LITINT ? "i64" : "f64");
                    declare_const(n->name, v, n->tok);
                    return;
                }
                t = v.t;
            }
        }
        if (is_agg(t) && n->b && v.fresh && v.a.slot >= 0 && v.a.off == 0) {
            Var *var = declare(n->name, t, n->tok, n->flag); /* adopt the temporary */
            var->kind = VK_SLOT;
            var->slot = v.a.slot;
            return;
        }
        if (!n->b) {
            Var *var = new_local(n->name, t, n->tok, n->flag);
            zero_at(t, A_slot(var->slot, 0));
            return;
        }
        if (!is_agg(t)) v = V_reg(t, reg_of(v)); /* evaluate before the name comes into scope */
        Var *var = new_local(n->name, t, n->tok, n->flag);
        store_val(t, A_slot(var->slot, 0), v);
        return;
    }
    case N_ASSIGN: {
        bool simple = !strcmp(n->name, "=");
        LV l = lval(n->a);
        if (l.ro) {
            if (n->a->k == N_NAME) error_at(n->a->tok, "can't assign to '%s': it's immutable (use 'var' to declare a mutable variable)", n->a->name);
            error_at(n->tok, "can't assign here: the location is read-only");
        }
        if (l.temp) error_at(n->tok, "can't assign to a field of a temporary value");
        if (simple) {
            Val v = coerce(expr(n->b, l.t), l.t, n->b->tok, "assignment");
            lv_store(l, v, n->b->tok);
            return;
        }
        char op[4] = {0};
        memcpy(op, n->name, strlen(n->name) - 1);
        if (is_agg(l.t) && !is_vector(l.t))
            error_at(n->tok, "'%s' needs a number%s", n->name,
                     is_bytes(l.t) && !strcmp(op, "+") ? " ('+=' doesn't append to strings, since that would allocate: use an ArrayList(u8) or fmt.bufPrint)" : "");
        Val cur = lv_load(l);
        Val r = expr(n->b, l.t);
        Val res = binop(op, cur, r, n->tok);
        res = coerce(res, l.t, n->b->tok, "assignment");
        lv_store(l, res, n->b->tok);
        return;
    }
    case N_INCR: {
        LV l = lval(n->a);
        if (l.ro) error_at(n->a->tok, "can't modify an immutable value");
        if (l.temp) error_at(n->tok, "can't modify a field of a temporary value");
        if (!is_int(l.t) && !is_ptr(l.t)) error_at(n->tok, "'%s' needs an integer or pointer, found %s", n->name, tname(l.t));
        Val cur = lv_load(l);
        Val res = binop(n->name[0] == '+' ? "+" : "-", cur, V_int(ty_litint, 1), n->tok);
        lv_store(l, res, n->tok);
        return;
    }
    case N_EXPRSTMT: {
        Val v = expr(n->a, NULL);
        if (v.t->k == TY_ERRUNION)
            error_at(n->tok, "this can fail (%s) and the error is ignored; handle it with 'try' or 'catch'", tname(v.t));
        if (v.t != ty_void && v.t != ty_noreturn)
            error_at(n->tok, "unused value of type %s; write '_ = ...;' to discard it", tname(v.t));
        return;
    }
    case N_DISCARD: {
        Val v = expr(n->a, NULL);
        if (v.t->k == TY_ERRUNION) error_at(n->tok, "'_ =' can't throw away an error; write 'catch {}' to ignore it on purpose");
        return;
    }
    case N_SWITCH: lower_switch(n, NULL); return;
    case N_COMPTIME: ct_block(n->a); return;
    case N_FALLTHROUGH: error_at(n->tok, "fallthrough must be the last statement of a switch case");
    case N_BLOCK:
        C->unsafe_depth += n->is_unsafe;
        sub_block(n, false, -1, -1);
        C->unsafe_depth -= n->is_unsafe;
        return;
    case N_IF: {
        if (n->a->k == N_COMPTIME && !n->name && !n->name2) { /* if (comptime c): only the taken branch is compiled */
            bool take = comptime_cond(n->a);
            Node *yes = take ? n->b : n->c, *no = take ? n->c : n->b;
            if (no) check_names(no);
            if (yes) sub_block(yes, false, -1, -1); /* (an `else if` is a block holding the next if) */
            return;
        }
        int tb = new_block(), eb = n->c ? new_block() : -1, join = new_block();
        Val ov = V_void();
        bool caps = n->name || n->name2;
        LV loc;
        if (caps) { /* if (maybe) |v| { ... } else |err| { ... } */
            ov = cap_subject_at(n->a, n->name, n->name2, n->flag2 ? &loc : NULL);
            br(R(cap_ok(ov)), tb, n->c ? eb : join);
        } else {
            cond(n->a, tb, n->c ? eb : join);
        }
        set_blk(tb);
        push_scope(false, -1, -1);
        if (n->name && n->flag2) bind_ptr(n->name, n->tok, loc);
        else if (n->name) bind_opt(n->name, n->tok, ov);
        sub_block(n->b, false, -1, -1);
        pop_scope();
        jmp(join);
        if (n->c) {
            set_blk(eb);
            push_scope(false, -1, -1);
            if (n->name2) bind_err(n->name2, n->tok, ov);
            sub_block(n->c, false, -1, -1);
            pop_scope();
            jmp(join);
        }
        set_blk(join);
        return;
    }
    case N_WHILE:
        if (n->name) { /* while (next()) |v| { ... }: test at the top, every time */
            int head = new_block(), body = new_block(), exit = new_block();
            jmp(head);
            set_blk(head);
            LV loc;
            Val ov = cap_subject_at(n->a, n->name, NULL, n->flag2 ? &loc : NULL);
            br(R(cap_ok(ov)), body, exit);
            set_blk(body);
            push_scope(true, exit, head);
            C->cur->label = n->label;
            if (n->flag2) bind_ptr(n->name, n->tok, loc);
            else bind_opt(n->name, n->tok, ov);
            block_body(n->b);
            pop_scope();
            jmp(head);
            set_blk(exit);
            return;
        }
        /* fall through */
    {
        /* rotated loop: test once up front, then at the bottom of each iteration */
        int body = new_block(), cont = new_block(), exit = new_block();
        cond(n->a, body, exit);
        set_blk(body);
        push_scope(true, exit, cont);
        C->cur->label = n->label;
        block_body(n->b);
        pop_scope();
        jmp(cont);
        set_blk(cont);
        cond(n->a, body, exit);
        set_blk(exit);
        return;
    }
    case N_FORRANGE: {
        Val a = expr(n->a, NULL);
        Val b = expr(n->b, is_lit(a.t) ? NULL : a.t);
        Type *t = unify(a.t, b.t, n->tok, "..");
        if (t->k == TY_LITINT) /* for (0..n) counts in usize, like lengths; negative bounds count in i64 */
            t = (a.isconst && a.ival < 0) || (b.isconst && b.ival < 0) ? ty_i64 : ty_usize;
        t = default_t(t);
        if (!is_int(t)) error_at(n->a->tok, "range bounds must be integers, found %s", tname(t));
        a = coerce(a, t, n->a->tok, "range start");
        b = coerce(b, t, n->b->tok, "range end");
        Opd end = b.isconst ? IMM(b.ival) : R(reg_of(b));
        Opd start = opd_of(a);
        int lt = n->flag ? (is_signed(t) ? CC_LE : CC_LS) : (is_signed(t) ? CC_LT : CC_LO);
        int body = new_block(), step = new_block(), exit = new_block();
        push_scope(true, exit, step);
        C->cur->label = n->label;
        Var *iv = new_local(n->name, t, n->tok, true);
        int islot = iv->slot;
        store(t, A_slot(islot, 0), start);
        br(R(cmp(lt, start.k == O_IMM ? R(iconst(start.v)) : start, end)), body, exit);
        set_blk(body);
        block_body(n->c);
        jmp(step);
        set_blk(step);
        int i2 = op2(I_ADD, C_I, R(load(t, A_slot(islot, 0))), IMM(1));
        i2 = norm(i2, t);
        store(t, A_slot(islot, 0), R(i2));
        br(R(cmp(lt, R(i2), end)), body, exit);
        pop_scope();
        set_blk(exit);
        return;
    }
    case N_FOREACH: {
        Type *et;
        int p, len;
        bool ro;
        Val s;
        if (is_lvalue_node(n->a)) { /* arrays are looped over in place, so |*x| changes them */
            LV l = lval(n->a);
            union_read(l);
            s = is_array(l.t) ? V_agg(l.t, l.a, false) : load_val(l.t, l.a);
            ro = l.ro || l.temp;
        } else {
            s = expr(n->a, NULL);
            ro = true;
        }
        if (is_array_ptr(s.t)) {
            ro = s.t->is_const;
            s = V_agg(s.t->elem, A_reg(reg_of(s), 0), false);
        }
        if (is_array(s.t)) {
            layout(s.t, n->a->tok);
            et = s.t->elem;
            p = addr_reg(s.a);
            len = iconst(s.t->alen);
        } else {
            if (is_slice(s.t)) et = s.t->elem, ro = s.t->is_const;
            else error_at(n->a->tok, "can't loop over %s (use an array, a slice, or a range a..b)", tname(s.t));
            p = load(ptr_to(et), s.a);
            len = load(ty_i64, A_add(s.a, 8));
        }
        int body = new_block(), step = new_block(), exit = new_block();
        push_scope(true, exit, step);
        C->cur->label = n->label;
        int islot = n->name2 ? new_local(n->name2, ty_usize, n->tok, true)->slot : new_slot(ty_usize);
        store(ty_i64, A_slot(islot, 0), IMM(0));
        br(R(cmp(CC_GT, R(len), IMM(0))), body, exit);
        set_blk(body);
        int i = load(ty_i64, A_slot(islot, 0));
        Addr ea = elem_addr(p, V_reg(ty_i64, i), et->size);
        if (n->flag) { /* |*x|: x points at the element */
            Type *pt = ptr_to_q(et, ro);
            Var *v = new_local(n->name, pt, n->tok, true);
            store(pt, A_slot(v->slot, 0), R(addr_reg(ea)));
        } else if (is_agg(et)) { /* alias the element in place (read-only) */
            Var *v = declare(n->name, et, n->tok, true);
            v->kind = VK_REF;
            v->reg = addr_reg(ea);
        } else {
            Var *v = new_local(n->name, et, n->tok, true);
            store(et, A_slot(v->slot, 0), R(load(et, ea)));
        }
        block_body(n->c);
        jmp(step);
        set_blk(step);
        int i2 = op2(I_ADD, C_I, R(load(ty_i64, A_slot(islot, 0))), IMM(1));
        store(ty_i64, A_slot(islot, 0), R(i2));
        br(R(cmp(CC_LT, R(i2), R(len))), body, exit);
        pop_scope();
        set_blk(exit);
        return;
    }
    case N_RETURN: {
        FnInfo *f = C->fn;
        if (C->ct_scratch) error_at(n->tok, "'return' can't leave a comptime expression or block");
        if (n->a && ((n->a->k == N_UNARY && !strcmp(n->a->name, "&")) || n->a->k == N_SLICE)) {
            Type *rt;
            Var *lv = local_root(n->a->a, &rt);
            if (lv && (n->a->k == N_UNARY || is_array(rt)))
                error_at(n->a->tok, "this returns a pointer to the local '%s', which is gone once the function returns",
                         lv->name);
        }
        if (!n->a) {
            if (!f->ret) f->ret = ty_void;
            bool ok_void = f->ret->k == TY_ERRUNION && f->ret->elem == ty_void; /* `return;` in !void: success */
            if (f->ret != ty_void && !ok_void) error_at(n->tok, "function '%s' must return a %s", f->name, tname(f->ret));
            emit_defers(NULL);
            ret_inst(IMM(0));
            return;
        }
        Val v = expr(n->a, f->ret);
        if (!f->ret && v.t->k == TY_ERRSET)
            error_at(n->a->tok, "a function that returns errors needs a declared return type, like -> !T");
        if (!f->ret) {
            if (v.t == ty_null || v.t == ty_void)
                error_at(n->a->tok, "can't infer the return type here; add '-> type' to the function");
            f->ret = default_t(v.t);
            if (is_agg(f->ret)) add_retptr();
        }
        if (f->ret == ty_void) error_at(n->a->tok, "function '%s' returns nothing, but a value is returned here", f->name);
        /* 0: success, 1: an error, 2: an error union that may hold either */
        int kind = f->ret->k != TY_ERRUNION ? 0 : v.t->k == TY_ERRSET ? 1 : v.t->k == TY_ERRUNION ? 2 : 0;
        v = coerce(v, f->ret, n->a->tok, "return value");
        Opd code = kind ? eu_err(v) : IMM(0);
        Opd o = IMM(0);
        if (is_agg(f->ret)) copy_agg(f->ret, A_reg(C->retptr, 0), v.a);
        else o = opd_of(v);
        if (kind == 0) {
            emit_defers(NULL);
        } else {
            Val e = code.k == O_IMM ? V_int(f->ret->eset, code.v) : V_reg(f->ret->eset, (int)code.v);
            if (kind == 2) {
                int eb = new_block(), nb = new_block();
                br(R(cmp(CC_NE, code, IMM(0))), eb, nb);
                set_blk(eb);
                emit_defers_err(NULL, &e);
                ret_inst(o);
                set_blk(nb);
                emit_defers(NULL);
            } else {
                emit_defers_err(NULL, &e);
            }
        }
        ret_inst(o);
        return;
    }
    case N_BREAK:
    case N_CONTINUE: {
        Scope *s = C->cur;
        while (s && !(s->loop && (!n->label || (s->label && !strcmp(s->label, n->label))))) s = s->parent;
        if (!s && n->label) error_at(n->tok, "no enclosing loop is labeled '%s'", n->label);
        if (!s) error_at(n->tok, "'%s' outside of a loop", n->k == N_BREAK ? "break" : "continue");
        emit_defers(s);
        jmp(n->k == N_BREAK ? s->brk : s->cont);
        return;
    }
    case N_DEFER: {
        if (n->flag && (!C->fn->ret || C->fn->ret->k != TY_ERRUNION))
            error_at(n->tok, "errdefer only runs when an error is returned, but '%s' doesn't return an error union", C->fn->name);
        Node *d = n->a;
        Node *body = d->k == N_BLOCK ? d : NULL;
        if (body) {
            for (int i = 0; i < body->n; i++) {
                NodeKind k = body->list[i]->k;
                if (k == N_RETURN || k == N_BREAK || k == N_CONTINUE)
                    error_at(body->list[i]->tok, "return/break/continue are not allowed inside defer");
            }
        }
        PUSH(C->cur->defers, C->cur->nd, C->cur->dcap, n);
        return;
    }
    default: error_at(n->tok, "unexpected statement");
    }
}

/* ====================================================================== */
/* functions and program                                                  */
/* ====================================================================== */

static int getarg(int cls, int idx) {
    int r = new_vreg(cls);
    Inst *i = emit(I_GETARG);
    i->dst = r;
    i->cls = cls;
    i->imm = idx;
    return r;
}

/* The return type turned out to be an aggregate: insert the hidden result pointer as x0. */
static void add_retptr(void) {
    Block *e = &C->f->blocks[0];
    for (int i = 0; i < e->n; i++) {
        if (e->insts[i].op == I_GETARG && e->insts[i].cls == C_I) {
            if (e->insts[i].imm >= 1000 || ++e->insts[i].imm >= 8)
                error_at(C->fn->decl->tok, "'%s' has many parameters, so give it a return type ('-> T') instead of inferring one", C->fn->name);
        }
    }
    Inst in;
    memset(&in, 0, sizeof in);
    in.op = I_GETARG;
    in.cls = C_I;
    in.slot = -1;
    in.dst = new_vreg(C_I);
    in.t1 = in.t2 = -1;
    PUSH(e->insts, e->n, e->cap, in);
    memmove(&e->insts[1], &e->insts[0], sizeof(Inst) * (size_t)(e->n - 1));
    e->insts[0] = in;
    C->retptr = in.dst;
}

static void lower_fn(FnInfo *f) {
    if (f->state != 0) return;
    f->state = 1;
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    Ctx *saved = C;
    TEnv *saved_te = TE;
    Unit *saved_unit = CU;
    TE = f->tenv;
    CU = f->decl->unit;
    C = &ctx;
    IRFunc *irf = xalloc(sizeof *irf);
    irf->sym = f->sym;
    irf->name = f->recv ? strf("%s.%s", tname(f->recv), f->name) : f->name;
    irf->ct_only = CT_DEPTH > 0;
    irf->is_main = !f->recv && !strcmp(f->name, "main") && f->unit && f->unit->is_root;
    irf->exported = f->decl->exported;
    C->f = irf;
    C->fn = f;
    C->retptr = -1;
    C->unsafe_depth = f->decl->is_unsafe; /* an unsafe fn's body is an unsafe context */
    set_blk(new_block());
    push_scope(false, -1, -1);
    int ic = 0, fc = 0, sk = 0; /* sk: arguments past the 8 registers of their class come on the stack */
#define NEXT_ARG(c) ((c) == C_I ? (ic < 8 ? ic++ : (ic++, 1000 + sk++)) : (fc < 8 ? fc++ : (fc++, 1000 + sk++)))
    if (f->ret && is_agg(f->ret)) C->retptr = getarg(C_I, ic++);
    for (int i = 0; i < f->np; i++) {
        Type *pt = f->pt[i];
        layout(pt, f->ptok[i]);
        if (is_agg(pt)) {
            Leaf L[4];
            int k = leaves(pt, 0, L, 0, 4);
            if (k >= 0) {
                Var *v = new_local(f->pn[i], pt, f->ptok[i], false);
                int slot = v->slot;
                for (int j = 0; j < k; j++) {
                    int c = cls_of(L[j].t);
                    int r = getarg(c, NEXT_ARG(c));
                    store(L[j].t, A_slot(slot, L[j].off), R(r));
                }
            } else {
                Var *v = declare(f->pn[i], pt, f->ptok[i], false);
                v->kind = VK_REF;
                v->param_copy = true;
                v->reg = getarg(C_I, NEXT_ARG(C_I));
            }
        } else {
            int c = cls_of(pt);
            int r = getarg(c, NEXT_ARG(c));
            Var *v = new_local(f->pn[i], pt, f->ptok[i], false);
            store(pt, A_slot(v->slot, 0), R(r));
        }
        if ((ic > 8 || fc > 8) && f->decl->exported)
            error_at(f->ptok[i], "too many parameters for a function C calls (up to 8 integer and 8 float registers are supported)");
    }
#undef NEXT_ARG
    Node *body = f->decl->c;
    block_body(body);
    if (!f->ret) f->ret = ty_void;
    bool ok_void = f->ret->k == TY_ERRUNION && f->ret->elem == ty_void;
    if (f->ret != ty_void && !ok_void && !always_returns(body))
        error_at(f->decl->tok, "function '%s' doesn't return a value on every path", f->name);
    if (!terminated()) {
        if (f->ret == ty_void) emit(I_RET);
        else if (ok_void) ret_inst(IMM(0));
        else trap();
    }
    irf->retcls = (f->ret == ty_void || is_agg(f->ret)) ? -1 : cls_of(f->ret);
    PUSH(M->funcs, M->nfuncs, M->fcap, irf);
    C = saved;
    TE = saved_te;
    CU = saved_unit;
    f->state = 2;
}

static const char *mangle(Type *t) {
    if (t == ty_cstr) return "cstr";
    if (t->mname) return t->mname;
    if (t->k == TY_PTR) return strf("%s%s_%s", t->many ? "m" : "p", t->is_const ? "c" : "", mangle(t->elem));
    if (t->k == TY_SLICE) return strf("s%s_%s", t->is_const ? "c" : "", mangle(t->elem));
    if (t->k == TY_OPT) return strf("o_%s", mangle(t->elem));
    if (t->k == TY_ARRAY) return strf("a%lld_%s", (long long)t->alen, mangle(t->elem));
    if (t->k == TY_VECTOR) return strf("v%lld_%s", (long long)t->alen, mangle(t->elem));
    if (t->k == TY_ERRUNION) return strf("e_%s", mangle(t->elem));
    return tname(t);
}

static bool is_generic_param(Node *p) {
    return p->flag || (p->a->k == N_TNAME && !strcmp(p->a->name, "anytype"));
}

static void declare_fn_in(Node *d, bool ext);
static void declare_fn(Node *d, bool ext) {
    Unit *su = CU;
    if (d->unit) CU = d->unit;
    declare_fn_in(d, ext);
    CU = su;
}

static void declare_fn_in(Node *d, bool ext) {
    FnInfo *f = xalloc(sizeof *f);
    f->name = d->name;
    f->decl = d;
    f->ext = ext;
    f->variadic = d->flag == 1;
    f->unit = d->unit;
    f->is_pub = d->is_pub;
    f->lib = d->unit && d->unit->is_std; /* the standard library: lowered only if used */
    if (d->a) {
        f->recv = resolve(d->a);
        const char *rn = tname(f->recv);
        if (f->recv == ty_void) error_at(d->tok, "can't add methods to void");
        if (find_method(f->recv, d->name)) error_at(d->tok, "method '%s.%s' is already defined", rn, d->name);
        for (int i = 0; i < f->recv->nfields; i++)
            if (!strcmp(f->recv->fields[i].name, d->name))
                error_at(d->tok, "'%s' already has a field named '%s'", rn, d->name);
        bool builtin_field = !strcmp(d->name, "len") || !strcmp(d->name, "ptr");
        if (is_slice(f->recv) && builtin_field) error_at(d->tok, "'%s' is a built-in field of %s", d->name, rn);
        f->sym = strf("_fl_%s_%s", mangle(f->recv), d->name);
    } else {
        FnInfo *prev = fn_in(CU, d->name);
        if (ext && prev && prev->ext) return; /* repeated extern */
        if (prev) error_at(d->tok, "function '%s' is already defined", d->name);
        if (is_builtin(d->name)) error_at(d->tok, "'%s' is a builtin and can't be redefined", d->name);
        if (alias_unit(d->name) || alias_header(d->name)) error_at(d->tok, "'%s' is already the name of an import", d->name);
        f->sym = (ext || d->exported) ? strf("_%s", d->name) : strf("_fl_%s%s", CU ? CU->prefix : "", d->name);
    }
    f->np = d->n;
    f->pt = xalloc(sizeof(Type *) * (size_t)(d->n + 1));
    f->pn = xalloc(sizeof(char *) * (size_t)(d->n + 1));
    f->ptok = xalloc(sizeof(Token *) * (size_t)(d->n + 1));
    f->tenv = TE;
    f->type_fn = d->b && d->b->k == N_TNAME && !strcmp(d->b->name, "type");
    f->generic = f->type_fn;
    for (int i = 0; i < d->n; i++)
        if (is_generic_param(d->list[i])) f->generic = true;
    if (f->generic && ext) error_at(d->tok, "an extern function can't have comptime or anytype parameters");
    if (f->generic && d->exported) error_at(d->tok, "an exported function can't have comptime or anytype parameters");
    for (int i = 0; i < d->n; i++) {
        Node *p = d->list[i];
        f->pn[i] = p->name;
        f->ptok[i] = p->tok;
        /* a generic function's parameter types may name its comptime parameters: they're resolved per call */
        if (!f->generic || (i == 0 && f->recv && !is_generic_param(p))) f->pt[i] = resolve(p->a);
        for (int j = 0; j < i; j++)
            if (!strcmp(f->pn[j], p->name)) error_at(p->tok, "duplicate parameter '%s'", p->name);
    }
    if (f->recv && f->np > 0 && f->pt[0]) { /* a first parameter of type T or *T makes it callable as x.f() */
        if (f->pt[0] == f->recv) f->self_kind = 1;
        else if (is_ptr(f->pt[0]) && f->pt[0]->elem == f->recv) f->self_kind = 2;
    }
    if (f->generic) f->ret = NULL; /* resolved per instance */
    else if (d->b) f->ret = resolve(d->b);
    else f->ret = ty_void; /* no '-> T' means void */
    if (ext && is_agg(f->ret))
        error_at(d->b->tok, "an extern function can't return %s yet (C returns small structs in registers, which Flint's calls "
                            "to C don't handle): have the C side write it through a pointer parameter", tname(f->ret));
    if (ext) f->state = 2;
    PUSH(fns, nfns, cfns, f);
}

static void global_let_in(Node *n, Type *owner);

static Var *static_var(Type *t, const char *name) {
    if (!t->unit) return NULL;
    return global_in(t->unit, strf("%s.%s", t->name, name));
}

/* a global's first use (or the end of the program): make its data now */
static Var *materialize(int i) {
    if (gvars[i].busy) error_at(gvars[i].lazy->tok, "'%s' depends on itself", gvars[i].name);
    gvars[i].busy = true;
    Node *ln = gvars[i].lazy;
    Type *lo = gvars[i].lazy_owner;
    Unit *u = gvars[i].unit;
    const char *name = gvars[i].name;
    TEnv *ste = TE;
    int snc = g_ncalls;
    TE = gvars[i].lazy_te;
    g_ncalls = 0;
    Ctx *sc = C; /* a global's initializer sees no function's locals */
    C = NULL;
    global_let_in(ln, lo);
    C = sc;
    TE = ste;
    g_ncalls = snc;
    gvars[i].name = ""; /* the real one was added at the end */
    return global_in(u, name);
}

/* evaluate a global initializer in a scratch function; it must fold to a constant */
static Val const_eval(Node *e, Type *t, const char *what) {
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    IRFunc *scratch = xalloc(sizeof *scratch);
    ctx.f = scratch;
    Ctx *saved = C;
    C = &ctx;
    set_blk(new_block());
    Val val = expr(e, t);
    if (t) val = coerce(val, t, e->tok, what);
    C = saved;
    return val;
}

static void data_item(GlobalVar *g, int off, int size, int64_t val, const char *sym) {
    DataItem d = {off, size, val, sym};
    PUSH(g->items, g->nitems, g->citems, d);
}

static int64_t raw_bits(Val v, Type *t) {
    if (t->k == TY_F64) {
        int64_t b;
        memcpy(&b, &v.fval, 8);
        return b;
    }
    if (t->k == TY_F32) {
        float fl = (float)v.fval;
        int32_t b;
        memcpy(&b, &fl, 4);
        return b;
    }
    return v.ival;
}

/* a constant aggregate for a global's initial data: struct literals, string literals, function names,
 * &other_global, null, undefined (zero) and constant numbers */
static void static_init(GlobalVar *g, Type *t, int off, Node *e) {
    if (e->k == N_UNDEFINED) return; /* globals start zeroed: a stable value */
    if (t->k == TY_STRUCT && !t->packed && !t->is_union && (e->k == N_STRUCTLIT || e->k == N_ANONLIT)) {
        if (e->k == N_STRUCTLIT && (e->a ? static_type(e->a) : find_struct(e->name)) != t)
            error_at(e->tok, "type mismatch: expected %s, found another struct", tname(t));
        layout(t, e->tok);
        if (t->tuple) {
            if (e->n != t->nfields) error_at(e->tok, "%s has %d elements, but %d are given", tname(t), t->nfields, e->n);
            for (int i = 0; i < e->n; i++) static_init(g, t->fields[i].t, off + t->fields[i].off, e->list[i]);
            return;
        }
        if (e->k == N_ANONLIT && !e->flag && e->n) error_at(e->tok, "%s's fields need names: .{ field: value, ... }", tname(t));
        for (int j = 0; j < t->nfields; j++) {
            Field *fd = &t->fields[j];
            Node *v = fd->def;
            for (int i = 0; i < e->n; i++)
                if (!strcmp(e->list[i]->name, fd->name)) v = e->list[i]->a;
            if (!v) error_at(e->tok, "missing field '%s' in %s literal", fd->name, tname(t));
            static_init(g, fd->t, off + fd->off, v);
        }
        for (int i = 0; i < e->n; i++) {
            bool found = false;
            for (int j = 0; j < t->nfields; j++) found |= !strcmp(t->fields[j].name, e->list[i]->name);
            if (!found) error_at(e->list[i]->tok, "struct '%s' has no field '%s'", tname(t), e->list[i]->name);
        }
        return;
    }
    if (is_bytes(t) && t->is_const && (e->k == N_STR || is_embed(e))) {
        int len = e->slen;
        const char *data = e->k == N_STR ? e->str : embed_file(e, &len);
        int k = add_str(data, len);
        data_item(g, off, 8, 0, M->strs[k].label);
        data_item(g, off + 8, 8, len, NULL);
        return;
    }
    Type *bt = t->k == TY_OPT ? t->elem : t;
    if (t->k == TY_OPT && e->k == N_NULL && (is_fn(bt) || is_ptr(bt))) return;
    if (is_fn(bt)) {
        FnInfo *f = NULL;
        if (e->k == N_NAME && !find_var(e->name)) f = find_fn(e->name);
        else if (e->k == N_FIELDACC && unit_ref(e->a)) f = callee_fn(e);
        else if (e->k == N_FIELDACC && is_static_ref(e->a)) f = find_method(static_type(e->a), e->name);
        if (!f) error_at(e->tok, "a function pointer in a global's initial value must be a function's name");
        Ctx ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.f = xalloc(sizeof(IRFunc));
        Ctx *saved = C;
        C = &ctx;
        set_blk(new_block());
        Val fv = fn_value(f, e->tok); /* lowers it; the instructions land in a scratch function */
        C = saved;
        if (fv.t != bt) error_at(e->tok, "type mismatch: expected %s, found %s", tname(bt), tname(fv.t));
        data_item(g, off, 8, 0, f->sym);
        return;
    }
    if (is_ptr(bt)) {
        if (e->k == N_UNARY && !strcmp(e->name, "&")) {
            Var *v = NULL;
            if (e->a->k == N_NAME) v = find_var(e->a->name);
            else if (e->a->k == N_FIELDACC && unit_ref(e->a->a)) v = module_var(e->a);
            else if (e->a->k == N_FIELDACC && is_static_ref(e->a->a)) v = static_var(static_type(e->a->a), e->a->name);
            if (v && v->kind == VK_GLOBAL && !v->ext) {
                Type *pt = ptr_to_q(v->t, v->is_const);
                bool ok = pt == bt || (pt->elem == bt->elem && bt->is_const && !bt->many) || (bt->elem == ty_void && !pt->is_const);
                if (!ok) error_at(e->tok, "type mismatch: expected %s, found %s", tname(bt), tname(pt));
                data_item(g, off, 8, 0, v->sym);
                return;
            }
        }
        error_at(e->tok, "a pointer in a global's initial value must be null or the address of another global (&name)");
    }
    if (is_array(t) && (e->k == N_ARRAYLIT || (e->k == N_ANONLIT && !e->flag))) { /* [_]T { a, b } */
        if (e->k == N_ARRAYLIT && arraylit_type(e) != t)
            error_at(e->tok, "type mismatch: expected %s, found %s", tname(t), tname(arraylit_type(e)));
        layout(t, e->tok);
        if (e->n != t->alen)
            error_at(e->tok, "%s needs %lld element(s), but %d %s given", tname(t), (long long)t->alen, e->n, e->n == 1 ? "is" : "are");
        for (int i = 0; i < e->n; i++) static_init(g, t->elem, off + i * t->elem->size, e->list[i]);
        return;
    }
    if (is_agg(t)) error_at(e->tok, "a global's initial value can't contain %s (yet)", tname(t));
    Val v = const_eval(e, t, "initial value");
    if (!v.isconst) error_at(e->tok, "global initializers must be constants (compute it at compile time with 'comptime')");
    data_item(g, off, t->size, raw_bits(v, t), NULL);
}

/* a global, or (owner != NULL) a `let`/`var` declared inside a type, named Owner.name */
static void global_let_at(Node *n, Type *owner);
static void global_let_in(Node *n, Type *owner) {
    Unit *su = CU;
    if (n->unit) CU = n->unit;
    global_let_at(n, owner);
    CU = su;
}

static bool dup_global(const char *name) {
    for (int i = 0; i < ngvars; i++)
        if (gvars[i].unit == CU && gvars[i].kind != VK_LAZY && !strcmp(gvars[i].name, name)) return true;
    return false;
}

static void global_let_at(Node *n, Type *owner) {
    if (n->n) error_at(n->tok, "destructuring only works inside functions");
    const char *name = owner ? strf("%s.%s", owner->name, n->name) : n->name;
    if (n->is_extern) { /* extern var name: T;  storage lives in C */
        if (dup_global(name)) error_at(n->tok, "'%s' is already defined", name);
        Var v;
        memset(&v, 0, sizeof v);
        v.unit = CU;
        v.is_pub = n->is_pub;
        v.name = n->name;
        v.is_const = n->flag;
        v.slot = v.reg = -1;
        v.kind = VK_GLOBAL;
        v.t = resolve(n->a);
        layout(v.t, n->a->tok);
        if (v.t == ty_void) error_at(n->a->tok, "a variable can't be void");
        v.sym = strf("_%s", n->name);
        v.ext = true;
        PUSH(gvars, ngvars, cgvars, v);
        return;
    }
    if (dup_global(name)) error_at(n->tok, "'%s' is already defined", name);
    bool type_ann = n->a && n->a->k == N_TNAME && !strcmp(n->a->name, "type");
    Type *t = type_ann ? ty_type : n->a ? resolve(n->a) : NULL;
    if (t && !type_ann) layout(t, n->a->tok);
    Var v;
    memset(&v, 0, sizeof v);
    v.unit = CU;
    v.is_pub = n->is_pub;
    v.name = name;
    v.is_const = n->flag;
    v.slot = v.reg = -1;
    GlobalVar g;
    memset(&g, 0, sizeof g);
    g.sym = n->exported ? strf("_%s", n->name) : owner ? strf("_g_%s_%s", mangle(owner), n->name) : strf("_g_%s%s", CU->prefix, n->name);
    g.exported = n->exported;
    g.is_const = n->flag;
    g.name = name;
    if (n->b && (n->b->k == N_STR || is_embed(n->b))) {
        if (t && t != ty_str) error_at(n->b->tok, "type mismatch: expected %s, found []const u8", tname(t));
        t = ty_str;
        g.kind = GV_STR;
        int len = n->b->slen;
        const char *data = n->b->k == N_STR ? n->b->str : embed_file(n->b, &len);
        g.sidx = add_str(data, len);
    } else if (n->b && t && n->b->k == N_BUILTIN && is_agg(t) &&
               ((!strcmp(n->b->name, "@splat") && n->b->n == 1 && n->b->list[0]->k == N_INT && n->b->list[0]->ival == 0) ||
                !strcmp(n->b->name, "@zeroes"))) {
        if (!strcmp(n->b->name, "@zeroes") && resolve(n->b->b) != t) error_at(n->b->tok, "type mismatch: expected %s", tname(t));
        if (!strcmp(n->b->name, "@splat") && !is_array(t)) error_at(n->b->tok, "@splat makes an array, but '%s' is %s", n->name, tname(t));
        g.kind = GV_ZERO; /* all-zero aggregate: .space */
    } else if (n->b && n->b->k == N_UNDEFINED) { /* globals start zeroed */
        if (n->flag) error_at(n->b->tok, "a 'let' needs a real value; 'undefined' is only for 'var'");
        if (!t) error_at(n->tok, "'%s' needs a type to be undefined: 'var %s: T = undefined;'", n->name, n->name);
        g.kind = MODE == MODE_DEBUG ? GV_FILL : GV_ZERO;
    } else if (n->b && n->b->k == N_COMPTIME) { /* let TABLE = comptime crcTable(); */
        CtOut o;
        Type *vt = ct_compute(n->b->a, false, t, strf("'%s'", name), &o);
        if (vt == ty_type) {
            if (!n->flag) error_at(n->tok, "'%s' would hold a type, which only exists at compile time: use 'let'", name);
            v.kind = VK_TYPE;
            v.t = type_from_id(o.ival);
            PUSH(gvars, ngvars, cgvars, v);
            return;
        }
        if (o.isconst) {
            Val val = is_float(vt) ? V_flt(vt, o.fval) : V_int(vt, o.ival);
            t = vt;
            if (n->flag && !n->exported) {
                v.kind = VK_CONST;
                v.t = t;
                v.cival = val.ival;
                v.cfval = val.fval;
                PUSH(gvars, ngvars, cgvars, v);
                return;
            }
            g.kind = GV_INT;
            g.ival = raw_bits(val, t);
        } else { /* the data comptime made becomes this global */
            GlobalVar *cg = &M->globals[o.g];
            cg->sym = g.sym;
            cg->exported = g.exported;
            cg->is_const = g.is_const;
            cg->name = g.name;
            v.kind = VK_GLOBAL;
            v.t = vt;
            v.sym = g.sym;
            PUSH(gvars, ngvars, cgvars, v);
            return;
        }
    } else if (n->b && (n->b->k == N_ARRAYLIT || (n->b->k == N_ANONLIT && t && is_array(t)))) {
        if (!t) t = arraylit_type(n->b);
        layout(t, n->b->tok);
        g.kind = GV_DATA;
        static_init(&g, t, 0, n->b);
    } else if (n->b && (n->b->k == N_STRUCTLIT || (n->b->k == N_ANONLIT && t && t->k == TY_STRUCT))) {
        if (!t) t = n->b->a ? static_type(n->b->a) : find_struct(n->b->name);
        layout(t, n->b->tok);
        g.kind = GV_DATA;
        static_init(&g, t, 0, n->b);
    } else if (n->b) {
        Val val = const_eval(n->b, t, strf("'%s'", name));
        if (val.t == ty_type) { /* let IntList = List(i32); */
            if (!n->flag) error_at(n->tok, "'%s' would hold a type, which only exists at compile time: use 'let'", name);
            if (n->exported) error_at(n->tok, "a type can't be exported");
            v.kind = VK_TYPE;
            v.t = type_from_id(val.ival);
            PUSH(gvars, ngvars, cgvars, v);
            return;
        }
        if (!val.isconst || is_agg(val.t))
            error_at(n->b->tok, "global initializers must be constants (compute it at compile time with 'comptime', or assign it at the start of main)");
        val = defaulted(val);
        if (val.t == ty_null) error_at(n->tok, "can't infer a type for '%s' from null; add ': type'", name);
        t = val.t;
        if (n->flag && !n->exported) { /* const scalar: inline it everywhere (an exported one needs storage) */
            v.kind = VK_CONST;
            v.t = t;
            v.cival = val.ival;
            v.cfval = val.fval;
            PUSH(gvars, ngvars, cgvars, v);
            return;
        }
        g.kind = GV_INT;
        g.ival = raw_bits(val, t);
    } else {
        g.kind = GV_ZERO;
    }
    g.size = t->size;
    g.align = t->align;
    PUSH(M->globals, M->nglobals, M->gcap, g);
    v.kind = VK_GLOBAL;
    v.t = t;
    v.sym = g.sym;
    PUSH(gvars, ngvars, cgvars, v);
}

static int64_t eval_const_int(Node *e, const char *what) {
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    IRFunc *scratch = xalloc(sizeof *scratch);
    ctx.f = scratch;
    ctx.ct_scratch = true; /* it sees the constants around it, not the variables */
    ctx.outer = C;
    Ctx *saved = C;
    C = &ctx;
    set_blk(new_block());
    Val v = expr(e, NULL);
    C = saved;
    if (!v.isconst || !is_int(v.t)) error_at(e->tok, "%s must be an integer constant", what);
    return v.ival;
}

/* variants, tag values, tag type and payload structs of an enum */
static void declare_enum(Node *e) {
    Type *t = find_struct(e->name);
    int nv = 0;
    for (int i = 0; i < e->n; i++) nv += e->list[i]->k == N_VARIANT;
    if (!nv) error_at(e->tok, "enum '%s' needs at least one variant", e->name);
    t->variants = xalloc(sizeof(Variant) * (size_t)nv);
    int64_t next = 0, lo = 0, hi = 0;
    for (int i = 0; i < e->n; i++) {
        Node *vn = e->list[i];
        if (vn->k != N_VARIANT) continue;
        for (int k = 0; k < t->nvariants; k++)
            if (!strcmp(t->variants[k].name, vn->name)) error_at(vn->tok, "duplicate variant '%s'", vn->name);
        Variant *v = &t->variants[t->nvariants++];
        v->name = vn->name;
        v->tok = vn->tok;
        v->kind = vn->flag;
        v->value = vn->c ? eval_const_int(vn->c, "an enum value") : next;
        for (int k = 0; k + 1 < t->nvariants; k++)
            if (t->variants[k].value == v->value)
                error_at(vn->tok, "%s.%s has the same value (%lld) as %s.%s", e->name, v->name, (long long)v->value,
                         e->name, t->variants[k].name);
        next = v->value + 1;
        if (t->nvariants == 1 || v->value < lo) lo = v->value;
        if (t->nvariants == 1 || v->value > hi) hi = v->value;
        if (v->kind) {
            t->has_payload = true;
            Type *p = xalloc(sizeof *p);
            p->k = TY_STRUCT;
            p->name = strf("%s.%s", e->name, v->name);
            p->fields = xalloc(sizeof(Field) * (size_t)(vn->n + 1));
            for (int j = 0; j < vn->n; j++) {
                Node *f = vn->list[j];
                Field *fd = &p->fields[p->nfields++];
                fd->name = v->kind == 1 ? strf("_%d", j) : f->name;
                fd->t = resolve(v->kind == 1 ? f : f->a);
                fd->tok = f->tok;
                for (int k = 0; k + 1 < p->nfields; k++)
                    if (!strcmp(p->fields[k].name, fd->name)) error_at(f->tok, "duplicate field '%s'", fd->name);
            }
            v->payload = p;
        }
    }
    if (e->b) {
        t->tagt = resolve(e->b);
        if (!is_int(t->tagt)) error_at(e->b->tok, "an enum's tag type must be an integer type, not %s", tname(t->tagt));
        for (int i = 0; i < t->nvariants; i++)
            if (trunc_ext(t->variants[i].value, t->tagt) != t->variants[i].value)
                error_at(t->variants[i].tok, "%s.%s = %lld doesn't fit in %s", e->name, t->variants[i].name,
                         (long long)t->variants[i].value, tname(t->tagt));
    } else if (lo >= 0) {
        t->tagt = hi < 256 ? ty_u8 : hi < 65536 ? ty_u16 : hi <= 0xffffffffLL ? ty_u32 : ty_u64;
    } else {
        t->tagt = (lo >= -128 && hi < 128) ? ty_i8 : (lo >= -32768 && hi < 32768) ? ty_i16
                : (lo >= INT32_MIN && hi <= INT32_MAX) ? ty_i32 : ty_i64;
    }
}

/* an initializer made only of literals, names and operators: safe to fold before any type is laid out */
static bool simple_const(Node *e) {
    if (!e) return true;
    switch (e->k) {
    case N_INT: case N_FLOAT: case N_BOOL: case N_NAME: return true;
    case N_UNARY: return strcmp(e->name, "&") && simple_const(e->a);
    case N_BINARY: return simple_const(e->a) && simple_const(e->b);
    default: return false;
    }
}

/* a struct's fields, from its declaration (methods are declared separately) */
static void struct_fields(Type *t, Node *s) {
    t->fields = xalloc(sizeof(Field) * (size_t)(s->n + 1));
    for (int j = 0; j < s->n; j++) {
        Node *f = s->list[j];
        if (f->k != N_FIELD) continue; /* methods are declared below */
        for (int k = 0; k < t->nfields; k++)
            if (!strcmp(t->fields[k].name, f->name)) error_at(f->tok, "duplicate field '%s'", f->name);
        Field *fd = &t->fields[t->nfields++];
        fd->name = f->name;
        bool sg;
        int bw = f->a->k == N_TNAME ? bitwidth_name(f->a->name, &sg) : 0;
        if (bw && t->packed) { /* u3: held in the next full-width integer, stored in 3 bits */
            fd->t = bw <= 8 ? (sg ? ty_i8 : ty_u8) : bw <= 16 ? (sg ? ty_i16 : ty_u16) : bw <= 32 ? (sg ? ty_i32 : ty_u32) : (sg ? ty_i64 : ty_u64);
            fd->bits = bw;
            fd->sbits = sg;
        } else {
            fd->t = resolve(f->a);
        }
        if (t->is_union && f->b) error_at(f->b->tok, "union fields can't have defaults (a literal sets exactly one)");
        fd->tok = f->tok;
        fd->def = f->b;
    }
}

/* types declared inside types (Outer.Inner) become top-level items, right after their parent */
static void flatten(Node *it, Node ***out, int *n, int *cap) {
    PUSH(*out, *n, *cap, it);
    if (it->k != N_STRUCT) return;
    for (int j = 0; j < it->n; j++) {
        Node *m = it->list[j];
        if (m->k == N_STRUCT || m->k == N_ENUM) flatten(m, out, n, cap);
    }
}

/* a global (or a type's let) waiting for its first use */
static void declare_lazy(Node *ln, Type *owner) {
    const char *name = owner ? strf("%s.%s", owner->name, ln->name) : ln->name;
    for (int i = 0; i < ngvars; i++)
        if (gvars[i].unit == CU && !strcmp(gvars[i].name, name)) error_at(ln->tok, "'%s' is already defined", name);
    if (!owner && (alias_unit(name) || alias_header(name))) error_at(ln->tok, "'%s' is already the name of an import", name);
    Var v;
    memset(&v, 0, sizeof v);
    v.name = name;
    v.kind = VK_LAZY;
    v.lazy = ln;
    v.lazy_owner = owner;
    v.lazy_te = TE;
    v.slot = v.reg = -1;
    v.unit = CU;
    v.is_pub = ln->is_pub;
    PUSH(gvars, ngvars, cgvars, v);
}

static void lower_tests(Unit **units, int nunits);

Module *lower_program(Unit **units, int nunits, int mode, bool test_mode) {
    init_types();
    M = xalloc(sizeof *M);
    ct_init(M, ct_lookup);
    MODE = mode;
    UNITS = units;
    NUNITS = nunits;
    Node **items = NULL;
    int n = 0, ncap = 0;
    for (int u = 0; u < nunits; u++)
        for (int i = 0; i < units[u]->nitems; i++) flatten(units[u]->items[i], &items, &n, &ncap);
#define IN_UNIT(it) (CU = (it)->unit)
    for (int i = 0; i < n; i++) {
        if (items[i]->k != N_STRUCT && items[i]->k != N_ENUM && items[i]->k != N_ERRSET) continue;
        Node *s = items[i];
        IN_UNIT(s);
        if (type_in(CU, s->name) || !strcmp(s->name, "anyerror")) error_at(s->tok, "type '%s' is already defined", s->name);
        if (alias_unit(s->name) || alias_header(s->name)) error_at(s->tok, "'%s' is already the name of an import", s->name);
        Type *t = xalloc(sizeof *t);
        t->k = s->k == N_ENUM ? TY_ENUM : s->k == N_ERRSET ? TY_ERRSET : TY_STRUCT;
        t->name = s->name;
        t->unit = CU;
        t->is_pub = s->is_pub;
        if (*CU->dname) t->dname = strf("%s%s", CU->dname, s->name);
        if (strchr(s->name, '.') || *CU->prefix) { /* Outer.Inner, or a module's type: no '.' in symbols */
            char *m = strf("%s%s", CU->prefix, s->name);
            for (char *c = m; *c; c++)
                if (*c == '.') *c = '_';
            t->mname = m;
        }
        if (s->k == N_ERRSET) { /* error FileError { NotFound, ... } */
            t->size = t->align = 2;
            t->state = 2;
            t->errcodes = xalloc(sizeof(int) * (size_t)(s->n + 1));
            for (int j = 0; j < s->n; j++) {
                int code = err_code(s->list[j]->name);
                for (int k = 0; k < t->nerrs; k++)
                    if (t->errcodes[k] == code) error_at(s->list[j]->tok, "duplicate error '%s'", s->list[j]->name);
                t->errcodes[t->nerrs++] = code;
            }
        }
        if (s->k == N_STRUCT) {
            t->packed = s->flag == 1;
            t->is_union = s->flag == 2;
        }
        PUSH(structs, nstructs, cstructs, t);
    }
    for (int i = 0; i < n; i++)
        if (items[i]->k == N_ENUM) IN_UNIT(items[i]), declare_enum(items[i]);
    for (int i = 0; i < n; i++) { /* constants first: types like [N]u8 may use them */
        Node *it = items[i];
        if (it->k == N_LET && it->flag && !it->a && it->b && simple_const(it->b)) {
            IN_UNIT(it);
            if (alias_unit(it->name) || alias_header(it->name)) error_at(it->tok, "'%s' is already the name of an import", it->name);
            global_let_in(it, NULL);
            it->flag2 = 1;
        }
    }
#define IS_TYPE_FN(d) ((d)->k == N_FN && (d)->b && (d)->b->k == N_TNAME && !strcmp((d)->b->name, "type"))
    for (int i = 0; i < n; i++) /* functions that make types first: fields and signatures may use them */
        if (IS_TYPE_FN(items[i])) IN_UNIT(items[i]), declare_fn(items[i], false);
    for (int i = 0; i < n; i++)
        if (items[i]->k == N_STRUCT) IN_UNIT(items[i]), struct_fields(type_in(CU, items[i]->name), items[i]);
    for (int i = 0; i < nstructs; i++) CU = structs[i]->unit, layout(structs[i], NULL);
    for (int i = 0; i < n; i++) {
        IN_UNIT(items[i]);
        if (items[i]->k == N_FN) {
            if (!IS_TYPE_FN(items[i])) declare_fn(items[i], false);
        } else if (items[i]->k == N_STRUCT || items[i]->k == N_ENUM) {
            for (int j = 0; j < items[i]->n; j++)
                if (items[i]->list[j]->k == N_FN) declare_fn(items[i]->list[j], false);
        } else if (items[i]->k == N_EXTERN)
            for (int j = 0; j < items[i]->n; j++) declare_fn(items[i]->list[j], true);
    }
    for (int i = 0; i < n; i++) { /* globals are made when first used, so their order doesn't matter */
        IN_UNIT(items[i]);
        if (items[i]->k == N_LET && !items[i]->flag2) {
            if (items[i]->is_extern) global_let_in(items[i], NULL);
            else declare_lazy(items[i], NULL);
        }
        if (items[i]->k == N_STRUCT)
            for (int j = 0; j < items[i]->n; j++)
                if (items[i]->list[j]->k == N_LET) declare_lazy(items[i]->list[j], type_in(CU, items[i]->name));
    }
    Unit *root = units[0];
    CU = root;
    FnInfo *mainf = fn_in(root, "main");
    if (!test_mode) {
        if (!mainf || mainf->ext) error_at(NULL, "no 'fn main()' found");
        if (mainf->np) error_at(mainf->decl->tok, "main takes no parameters (use args() to read command-line arguments)");
    }
    for (int i = 0; i < nfns; i++) /* the standard library, generic instances and their methods: only when used */
        if (!fns[i]->ext && (!fns[i]->lib || fns[i]->decl->exported) && !fns[i]->generic && !fns[i]->lazy &&
            !(test_mode && fns[i] == mainf))
            lower_fn(fns[i]);
    for (int i = 0; i < ngvars; i++) /* the program's own globals, even unused ones */
        if (gvars[i].kind == VK_LAZY && gvars[i].name[0] && gvars[i].unit && !gvars[i].unit->is_std) materialize(i);
    for (int i = 0; i < n; i++) /* top-level comptime { ... } blocks */
        if (items[i]->k == N_COMPTIME) {
            IN_UNIT(items[i]);
            TE = NULL;
            ct_block(items[i]->a);
        }
    if (test_mode) {
        lower_tests(units, nunits);
    } else {
        M->main_errors = mainf->ret->k == TY_ERRUNION && mainf->ret->elem == ty_void;
        if (mainf->ret != ty_void && !is_int(mainf->ret) && !M->main_errors)
            error_at(mainf->decl->tok, "main must return nothing, an integer exit code, or !void");
    }
    err_code("(no error)"); /* make sure the error-name table exists */
    prune_ct_only();
    return M;
}

/* flint test: each `test "name" { ... }` becomes a function returning !void (its error code comes
 * back in x0), and main hands them to the runtime one at a time: it runs each in its own process,
 * so a panic fails only that test. */
static void lower_tests(Unit **units, int nunits) {
    FnInfo **tests = NULL;
    const char **names = NULL;
    int nt = 0, ct = 0, cn = 0, nn = 0;
    for (int u = 0; u < nunits; u++) {
        if (units[u]->is_std) continue;
        for (int i = 0; i < units[u]->nitems; i++) {
            Node *t = units[u]->items[i];
            if (t->k != N_TEST) continue;
            Node *d = xalloc(sizeof *d);
            *d = *t;
            d->k = N_FN;
            d->name = "test";
            FnInfo *f = xalloc(sizeof *f);
            f->name = strf("test \"%s\"", t->str);
            f->decl = d;
            f->unit = units[u];
            f->ret = err_union(ty_anyerror, ty_void);
            f->sym = strf("_fl_test_%d", nt);
            CU = units[u];
            lower_fn(f);
            PUSH(tests, nt, ct, f);
            const char *shown = units[u]->is_root ? t->str : strf("%s: %s", units[u]->path, t->str);
            PUSH(names, nn, cn, shown);
        }
    }
    FnInfo mainf;
    memset(&mainf, 0, sizeof mainf);
    mainf.name = "main";
    mainf.ret = ty_i64;
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    C = &ctx;
    IRFunc *irf = xalloc(sizeof *irf);
    irf->sym = "_fl_main";
    irf->is_main = true;
    C->f = irf;
    C->fn = &mainf;
    C->retptr = -1;
    set_blk(new_block());
    push_scope(false, -1, -1);
    for (int i = 0; i < nt; i++) {
        Val name = str_lit(names[i], (int)strlen(names[i]));
        int p = load(ptr_to(ty_u8), name.a);
        callv("_rt_test_run", -1, 3, R(p), C_I, IMM((int64_t)strlen(names[i])), C_I, R(addr_sym(tests[i]->sym)), C_I);
    }
    int r = callv("_rt_test_end", C_I, 0);
    Inst *ret = emit(I_RET);
    ret->a = R(r);
    ret->cls = C_I;
    irf->retcls = C_I;
    PUSH(M->funcs, M->nfuncs, M->fcap, irf);
    C = NULL;
}

/* ---------- import c "header.h" as c: C declarations, translated when they're used ---------- */

static _Noreturn void c_bad(CHeader *h, const char *name, const char *why, Token *tok) {
    error_at(tok, "'%s' from %s can't be used from Flint: %s", name, c_header_name(h), why);
}

static CDecl *c_need(CHeader *h, const char *name, Token *tok) {
    CDecl *d = c_lookup(h, name);
    if (!d) error_at(tok, "'%s' isn't declared in %s", name, c_header_name(h));
    if (d->k == CD_BAD) c_bad(h, name, d->why, tok);
    return d;
}

static Type *ctype(CHeader *h, const char *name, CType *ct, Token *tok);

static Type *c_struct_type(CHeader *h, const char *name, CStruct *s, Token *tok) {
    if (s->flint) return s->flint;
    Type *t = xalloc(sizeof *t);
    t->k = TY_STRUCT;
    t->name = s->tag;
    t->dname = s->display;
    char *m = strf("c_%s_%s", s->is_union ? "union" : "struct", s->tag);
    t->mname = m;
    t->is_pub = true;
    t->is_union = s->is_union;
    s->flint = t;
    if (!s->complete || s->why) { /* only usable behind a pointer */
        t->opaque = true;
        t->opaque_why = s->why ? s->why : strf("C declares %s without its fields", s->display);
        t->state = 2;
        t->size = 0;
        t->align = 1;
        return t;
    }
    t->fields = xalloc(sizeof(Field) * (size_t)(s->nf + 1));
    for (int i = 0; i < s->nf; i++) {
        Field *fd = &t->fields[t->nfields++];
        fd->name = s->fields[i].name;
        fd->t = ctype(h, name, s->fields[i].t, tok);
        fd->tok = tok;
    }
    layout(t, tok);
    return t;
}

static Type *c_int(int size, bool sign) {
    switch (size) {
    case 1: return sign ? ty_i8 : ty_u8;
    case 2: return sign ? ty_i16 : ty_u16;
    case 4: return sign ? ty_i32 : ty_u32;
    default: return sign ? ty_i64 : ty_u64;
    }
}

/* the Flint type for a C type (design.md section 4.15): C pointers can be null, so they're optional */
static Type *ctype(CHeader *h, const char *name, CType *ct, Token *tok) {
    switch (ct->k) {
    case CT_VOID: return ty_void;
    case CT_INT: return ct->is_char ? ty_u8 : c_int(ct->size, ct->sign);
    case CT_ENUM: return c_int(ct->size, ct->sign);
    case CT_BOOL: return ty_bool;
    case CT_FLOAT: return ct->size == 4 ? ty_f32 : ty_f64;
    case CT_NAMED:
        if (!strcmp(ct->name, "size_t") || !strcmp(ct->name, "uintptr_t")) return ty_usize;
        if (!strcmp(ct->name, "ssize_t") || !strcmp(ct->name, "intptr_t") || !strcmp(ct->name, "ptrdiff_t")) return ty_isize;
        return ctype(h, name, ct->elem, tok);
    case CT_PTR: {
        CType *e = ct->elem, *u = e;
        bool is_const = e->is_const;
        while (u->k == CT_NAMED && strcmp(u->name, "size_t")) {
            u = u->elem;
            is_const |= u->is_const;
        }
        if (u->k == CT_INT && u->is_char) return opt_of(is_const ? ty_cstr : many_ptr_q(ty_u8, false));
        if (u->k == CT_VOID) return opt_of(ptr_to_q(ty_void, is_const));
        if (u->k == CT_FN) return opt_of(ctype(h, name, u, tok));
        Type *pt = ctype(h, name, e, tok);
        return opt_of(ptr_to_q(pt, is_const));
    }
    case CT_ARRAY: return array_of(ctype(h, name, ct->elem, tok), ct->n < 0 ? 0 : ct->n);
    case CT_FN: {
        Type **pt = xalloc(sizeof(Type *) * (size_t)(ct->np + 1));
        for (int i = 0; i < ct->np; i++) pt[i] = ctype(h, name, ct->params[i], tok);
        return fn_type(pt, ct->np, ctype(h, name, ct->elem, tok), ct->variadic);
    }
    case CT_STRUCT: return c_struct_type(h, name, ct->st, tok);
    case CT_BAD: c_bad(h, name, strf("it uses %s, which Flint doesn't have", ct->why), tok);
    }
    return ty_void;
}

/* a C function as an extern FnInfo */
static FnInfo *c_fn(CHeader *h, CDecl *d, Token *tok) {
    if (d->cache) return d->cache;
    CType *ft = d->t;
    FnInfo *f = xalloc(sizeof *f);
    f->name = d->name;
    Node *decl = xalloc(sizeof *decl);
    decl->k = N_FN;
    decl->tok = tok;
    decl->name = d->name;
    f->decl = decl;
    f->ext = true;
    f->is_pub = true;
    f->variadic = ft->variadic;
    f->sym = d->sval ? d->sval : strf("_%s", d->name);
    f->np = ft->np;
    f->pt = xalloc(sizeof(Type *) * (size_t)(ft->np + 1));
    f->pn = xalloc(sizeof(char *) * (size_t)(ft->np + 1));
    f->ptok = xalloc(sizeof(Token *) * (size_t)(ft->np + 1));
    for (int i = 0; i < ft->np; i++) {
        f->pt[i] = ctype(h, d->name, ft->params[i], tok);
        layout(f->pt[i], tok);
        f->pn[i] = strf("p%d", i);
        f->ptok[i] = tok;
    }
    f->ret = ctype(h, d->name, ft->elem, tok);
    bool by_value = is_agg(f->ret);
    for (int i = 0; i < f->np; i++) by_value |= is_agg(f->pt[i]);
    if (by_value)
        c_bad(h, d->name, "it passes or returns a struct by value, and Flint's calls to C only handle numbers and pointers so far", tok);
    f->state = 2;
    d->cache = f;
    return f;
}

static LV c_var(CHeader *h, CDecl *d, Token *tok) {
    LV r;
    memset(&r, 0, sizeof r);
    r.t = ctype(h, d->name, d->t, tok);
    layout(r.t, tok);
    r.ro = d->t->is_const;
    r.a = A_reg(addr_sym_q(d->sval ? d->sval : strf("_%s", d->name), true), 0); /* C's data: through the GOT */
    return r;
}

static Type *c_type_named(CHeader *h, const char *name, Token *tok) {
    CDecl *d = tok ? c_need(h, name, tok) : c_lookup(h, name);
    if (!d || d->k != CD_TYPE) {
        if (tok) error_at(tok, "'%s' from %s isn't a type", name, c_header_name(h));
        return NULL;
    }
    return ctype(h, name, d->t, tok);
}

/* c.NAME as a value: a constant, a C global, or a function's address */
static Val c_value(CHeader *h, Node *n) {
    CDecl *d = c_need(h, n->name, n->tok);
    switch (d->k) {
    case CD_CONST_INT: return V_int(ty_litint, d->ival);
    case CD_CONST_FLOAT: return V_flt(ty_litfloat, d->fval);
    case CD_CONST_STR: return str_lit(d->sval, d->slen);
    case CD_FN: return fn_value(c_fn(h, d, n->tok), n->tok);
    case CD_VAR: return lv_load(c_var(h, d, n->tok));
    case CD_TYPE: error_at(n->tok, "'%s' from %s is a type, not a value", n->name, c_header_name(h));
    default: break;
    }
    c_bad(h, n->name, d->why, n->tok);
}

static Val c_call(CHeader *h, Node *fn, Node **args, int nargs, Token *tok) {
    CDecl *d = c_need(h, fn->name, fn->tok);
    if (d->k == CD_FN) return call_fn(c_fn(h, d, fn->tok), args, nargs, NULL, tok);
    if (d->k == CD_VAR) return call_value(c_value(h, fn), args, nargs, tok);
    error_at(fn->tok, "'%s' from %s isn't a function", fn->name, c_header_name(h));
}
