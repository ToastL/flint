/* cimport.c - import c "stdio.h" as c;  (design.md section 4.15)
 *
 * The header goes through the system C preprocessor (`cc -E`), and a small parser reads the
 * declarations it produces: function prototypes, typedefs, structs, unions, enums and extern
 * variables. `cc -dM -E` lists the macros; the ones that are simple constants (numbers, strings)
 * become constants. Nothing is translated to Flint until it's used (lower.c does that), and a
 * declaration Flint can't use (a va_list parameter, a bit field, a function-like macro) is only
 * an error if the program uses it. */
#include "flint.h"
#include <ctype.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <unistd.h>

struct CHeader {
    const char *name;
    const char *key; /* the header plus the include directories: what makes it the same import */
    CDecl **decls;
    int nd, cd;
    const char **mnames, **mbodies; /* object-like macros (body text), and function-like ones (body NULL) */
    int nm, cm, cmb;
};

static const char *asm_label; /* __asm("_fopen$UNIX2003"): the symbol a declaration really links to */

static CHeader **headers;
static int nheaders, cheaders;

/* one struct/union/enum tag table for every header: C has one `struct tm` per program */
static CStruct **tags;
static int ntags, ctags;

const char *c_header_name(CHeader *h) { return h->name; }

/* ---------------------------------------------------------------- running the preprocessor */

/* run argv; its stdout comes back, its stderr goes to errfile */
static char *run_capture(const char **argv, const char *errfile) {
    int fds[2];
    if (pipe(fds) != 0) return NULL;
    pid_t pid = fork();
    if (pid == 0) {
        FILE *ef = freopen(errfile, "w", stderr);
        (void)ef;
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], (char **)argv);
        _exit(127);
    }
    close(fds[1]);
    size_t n = 0, cap = 1 << 16;
    char *buf = xalloc(cap);
    for (;;) {
        if (n + 4096 >= cap) buf = xrealloc(buf, cap *= 2);
        ssize_t got = read(fds[0], buf + n, cap - n - 1);
        if (got <= 0) break;
        n += (size_t)got;
    }
    buf[n] = 0;
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return NULL;
    return buf;
}

/* ---------------------------------------------------------------- tokens */

enum { CK_ID, CK_NUM, CK_STR, CK_CHAR, CK_P, CK_EOF };
typedef struct {
    int k;
    const char *s; /* identifier, punctuator or number text */
    char *str;     /* string literal bytes */
    int slen;
    int64_t cval;  /* character literal */
} CTok;

static int c_escape(const char **pp) {
    const char *p = *pp;
    int c = *p++;
    switch (c) {
    case 'n': c = '\n'; break;
    case 't': c = '\t'; break;
    case 'r': c = '\r'; break;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        c -= '0';
        for (int i = 0; i < 2 && *p >= '0' && *p <= '7'; i++) c = c * 8 + (*p++ - '0');
        break;
    case 'x':
        c = 0;
        while (isxdigit((unsigned char)*p)) {
            c = c * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
            p++;
        }
        break;
    case 'a': c = 7; break;
    case 'b': c = 8; break;
    case 'f': c = 12; break;
    case 'v': c = 11; break;
    default: break; /* \\ \' \" \? */
    }
    *pp = p;
    return c;
}

static CTok *c_lex(const char *src, int *ntok) {
    CTok *toks = NULL;
    int n = 0, cap = 0;
    const char *p = src;
    bool line_start = true;
    while (*p) {
        if (*p == '\n') {
            line_start = true;
            p++;
            continue;
        }
        if (isspace((unsigned char)*p)) {
            p++;
            continue;
        }
        if (line_start && *p == '#') { /* #pragma and friends */
            while (*p && *p != '\n') p++;
            continue;
        }
        line_start = false;
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }
        CTok t;
        memset(&t, 0, sizeof t);
        const char *s = p;
        if (isalpha((unsigned char)*p) || *p == '_' || *p == '$') {
            while (isalnum((unsigned char)*p) || *p == '_' || *p == '$') p++;
            t.k = CK_ID;
            t.s = xstrndup(s, (size_t)(p - s));
        } else if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
            while (isalnum((unsigned char)*p) || *p == '.' || *p == '_' ||
                   ((*p == '+' || *p == '-') && strchr("eEpP", p[-1]) && !(s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && strchr("eE", p[-1]))))
                p++;
            t.k = CK_NUM;
            t.s = xstrndup(s, (size_t)(p - s));
        } else if (*p == '"' || (*p == 'L' && p[1] == '"')) {
            if (*p == 'L') p++;
            p++;
            char *buf = xalloc(strlen(p) + 1);
            int len = 0;
            while (*p && *p != '"') {
                if (*p == '\\') {
                    p++;
                    buf[len++] = (char)c_escape(&p);
                } else buf[len++] = *p++;
            }
            if (*p) p++;
            if (n > 0 && toks[n - 1].k == CK_STR) { /* "a" "b" is one string */
                CTok *prev = &toks[n - 1];
                char *both = xalloc((size_t)(prev->slen + len + 1));
                memcpy(both, prev->str, (size_t)prev->slen);
                memcpy(both + prev->slen, buf, (size_t)len);
                prev->str = both;
                prev->slen += len;
                continue;
            }
            t.k = CK_STR;
            t.str = buf;
            t.slen = len;
        } else if (*p == '\'') {
            p++;
            int64_t v = 0;
            while (*p && *p != '\'') v = (v << 8) | (*p == '\\' ? (p++, c_escape(&p)) : (unsigned char)*p++);
            if (*p) p++;
            t.k = CK_CHAR;
            t.cval = v;
        } else {
            static const char *ops[] = {"...", "<<=", ">>=", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&",
                                        "||", "##", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", NULL};
            int len = 1;
            for (const char **o = ops; *o; o++)
                if (!strncmp(p, *o, strlen(*o))) {
                    len = (int)strlen(*o);
                    break;
                }
            t.k = CK_P;
            t.s = xstrndup(p, (size_t)len);
            p += len;
        }
        PUSH(toks, n, cap, t);
    }
    CTok eof;
    memset(&eof, 0, sizeof eof);
    eof.k = CK_EOF;
    eof.s = "";
    PUSH(toks, n, cap, eof);
    *ntok = n - 1;
    return toks;
}

/* ---------------------------------------------------------------- the declaration table */

static CDecl *find_decl(CHeader *h, const char *name) {
    for (int i = 0; i < h->nd; i++)
        if (!strcmp(h->decls[i]->name, name)) return h->decls[i];
    return NULL;
}

static CDecl *add_decl(CHeader *h, CDeclKind k, const char *name) {
    CDecl *old = find_decl(h, name);
    if (old) return old->k == CD_BAD && k != CD_BAD ? (old->k = k, old) : NULL; /* the first declaration wins */
    CDecl *d = xalloc(sizeof *d);
    d->k = k;
    d->name = name;
    PUSH(h->decls, h->nd, h->cd, d);
    return d;
}

static void add_bad(CHeader *h, const char *name, const char *why) {
    CDecl *d = add_decl(h, CD_BAD, name);
    if (d) d->why = why;
}

static CStruct *tag_named(const char *kind, const char *tag) {
    const char *display = strf("%s %s", kind, tag);
    for (int i = 0; i < ntags; i++)
        if (!strcmp(tags[i]->display, display)) return tags[i];
    CStruct *s = xalloc(sizeof *s);
    s->tag = tag;
    s->display = display;
    s->is_union = !strcmp(kind, "union");
    PUSH(tags, ntags, ctags, s);
    return s;
}

/* ---------------------------------------------------------------- types */

static CType *ct_new(CTKind k) {
    CType *t = xalloc(sizeof *t);
    t->k = k;
    return t;
}

static CType *ct_int(int size, bool sign) {
    CType *t = ct_new(CT_INT);
    t->size = size;
    t->sign = sign;
    return t;
}

static CType *ct_bad(const char *why) {
    CType *t = ct_new(CT_BAD);
    t->why = why;
    return t;
}

static CType *ct_ptr(CType *to) {
    CType *t = ct_new(CT_PTR);
    t->elem = to;
    return t;
}

static CType *ct_const(CType *t) {
    if (t->is_const) return t;
    CType *c = xalloc(sizeof *c);
    *c = *t;
    c->is_const = true;
    return c;
}

/* ---------------------------------------------------------------- the parser */

typedef struct {
    CTok *t;
    int p, n;
    CHeader *h;
    jmp_buf fail;
    int anon;
} CP;

static CTok *cur(CP *c) { return &c->t[c->p]; }
static bool is_p(CP *c, const char *s) { return cur(c)->k == CK_P && !strcmp(cur(c)->s, s); }
static bool is_id(CP *c, const char *s) { return cur(c)->k == CK_ID && !strcmp(cur(c)->s, s); }
static bool accept_p(CP *c, const char *s) {
    if (!is_p(c, s)) return false;
    c->p++;
    return true;
}
static _Noreturn void cfail(CP *c) { longjmp(c->fail, 1); }
static void expect_p(CP *c, const char *s) {
    if (!accept_p(c, s)) cfail(c);
}

/* skip from an opening bracket to its match */
static void skip_group(CP *c) {
    int depth = 0;
    do {
        if (cur(c)->k == CK_EOF) cfail(c);
        if (is_p(c, "(") || is_p(c, "[") || is_p(c, "{")) depth++;
        else if (is_p(c, ")") || is_p(c, "]") || is_p(c, "}")) depth--;
        c->p++;
    } while (depth > 0);
}

/* __attribute__((...)), __asm("..."), __declspec(...), _Alignas(...): nothing Flint needs */
static bool skip_noise(CP *c) {
    if ((is_id(c, "__asm") || is_id(c, "__asm__") || is_id(c, "asm")) && c->t[c->p + 1].k == CK_P &&
        !strcmp(c->t[c->p + 1].s, "(") && c->t[c->p + 2].k == CK_STR) {
        asm_label = xstrndup(c->t[c->p + 2].str, (size_t)c->t[c->p + 2].slen);
        c->p++;
        skip_group(c);
        return true;
    }
    static const char *with_parens[] = {"__attribute__", "__attribute", "__asm", "__asm__", "asm", "__declspec", "_Alignas",
                                        "__typeof__", "__availability", NULL};
    static const char *words[] = {"__extension__", "__inline", "__inline__", "inline", "_Noreturn", "__restrict", "__restrict__",
                                  "restrict", "volatile", "__volatile__", "_Nullable", "_Nonnull", "_Null_unspecified",
                                  "__nullable", "__nonnull", "__null_unspecified", "register", "auto", "_Thread_local",
                                  "__thread", "__kindof", NULL};
    if (cur(c)->k != CK_ID) return false;
    for (const char **w = with_parens; *w; w++)
        if (is_id(c, *w)) {
            c->p++;
            if (is_p(c, "(")) skip_group(c);
            return true;
        }
    for (const char **w = words; *w; w++)
        if (is_id(c, *w)) {
            c->p++;
            return true;
        }
    return false;
}

static int64_t const_expr(CP *c);
static CType *declarator(CP *c, CType *base, const char **name);
static CType *specifiers(CP *c, int *storage);

enum { ST_NONE = 0, ST_TYPEDEF = 1, ST_EXTERN = 2, ST_STATIC = 4, ST_INLINE = 8 };

static bool is_typedef_name(CP *c, const char *s) {
    CDecl *d = find_decl(c->h, s);
    return d && d->k == CD_TYPE;
}

static bool starts_type(CP *c) {
    static const char *kw[] = {"void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "_Bool",
                               "struct", "union", "enum", "const", "__const", "volatile", "__int128", "__uint128_t",
                               "__int128_t", "_Complex", "__builtin_va_list", "_Atomic", "_Float16", NULL};
    if (cur(c)->k != CK_ID) return false;
    for (const char **k = kw; *k; k++)
        if (is_id(c, *k)) return true;
    return is_typedef_name(c, cur(c)->s);
}

/* struct tag { fields }  /  union ...  /  struct tag */
static CType *struct_spec(CP *c, bool is_union) {
    const char *kind = is_union ? "union" : "struct";
    while (skip_noise(c)) {}
    const char *tag = NULL;
    if (cur(c)->k == CK_ID) tag = cur(c)->s, c->p++;
    while (skip_noise(c)) {}
    CStruct *s = tag ? tag_named(kind, tag) : NULL;
    if (accept_p(c, "{")) {
        if (!s) {
            s = tag_named(kind, strf("anon%d", ++c->anon));
            s->display = strf("an anonymous %s", kind);
        }
        if (s->complete) { /* defined again (another header included it): read past it */
            c->p--;
            skip_group(c);
        } else {
            while (!accept_p(c, "}")) {
                if (accept_p(c, ";")) continue;
                int st = 0;
                CType *base = specifiers(c, &st);
                if (is_p(c, ";")) { /* an anonymous struct/union member */
                    s->why = strf("%s has an anonymous member", s->display);
                    c->p++;
                    continue;
                }
                for (;;) {
                    const char *fname = NULL;
                    CType *ft = declarator(c, base, &fname);
                    if (accept_p(c, ":")) {
                        const_expr(c);
                        s->why = strf("%s has bit fields", s->display);
                    }
                    while (skip_noise(c)) {}
                    CField f = {fname ? fname : "", ft};
                    PUSH(s->fields, s->nf, s->cf, f);
                    if (!accept_p(c, ",")) break;
                }
                expect_p(c, ";");
            }
            s->complete = true;
        }
    }
    if (!s) cfail(c);
    CType *t = ct_new(CT_STRUCT);
    t->st = s;
    return t;
}

/* enum tag : type { A = 1, B, ... }: the constants become declarations; the type is its integer */
static CType *enum_spec(CP *c) {
    while (skip_noise(c)) {}
    if (cur(c)->k == CK_ID) c->p++;
    CType *t = ct_new(CT_ENUM);
    t->size = 4;
    t->sign = true;
    if (accept_p(c, ":")) {
        int st = 0;
        CType *u = specifiers(c, &st);
        while (u->k == CT_NAMED) u = u->elem;
        if (u->k == CT_INT) t->size = u->size, t->sign = u->sign;
    }
    if (accept_p(c, "{")) {
        int64_t next = 0;
        while (!accept_p(c, "}")) {
            if (cur(c)->k != CK_ID) cfail(c);
            const char *name = cur(c)->s;
            c->p++;
            while (skip_noise(c)) {}
            if (accept_p(c, "=")) next = const_expr(c);
            CDecl *d = add_decl(c->h, CD_CONST_INT, name);
            if (d) d->ival = next;
            next++;
            if (!accept_p(c, ",")) {
                expect_p(c, "}");
                break;
            }
        }
    }
    return t;
}

/* the type part of a declaration (`static const unsigned long`), and its storage class */
static CType *specifiers(CP *c, int *storage) {
    int nlong = 0, nshort = 0, nsigned = 0, nunsigned = 0, nint = 0;
    bool is_const = false;
    CType *base = NULL;
    const char *bad = NULL;
    for (;;) {
        if (skip_noise(c)) continue;
        CTok *t = cur(c);
        if (t->k != CK_ID) break;
        const char *s = t->s;
        if (!strcmp(s, "typedef")) *storage |= ST_TYPEDEF;
        else if (!strcmp(s, "extern")) *storage |= ST_EXTERN;
        else if (!strcmp(s, "static")) *storage |= ST_STATIC;
        else if (!strcmp(s, "const") || !strcmp(s, "__const")) is_const = true;
        else if (!strcmp(s, "long")) nlong++;
        else if (!strcmp(s, "short")) nshort++;
        else if (!strcmp(s, "signed") || !strcmp(s, "__signed") || !strcmp(s, "__signed__")) nsigned++;
        else if (!strcmp(s, "unsigned")) nunsigned++;
        else if (!strcmp(s, "int")) nint++;
        else if (!base && !strcmp(s, "void")) base = ct_new(CT_VOID);
        else if (!base && !strcmp(s, "char")) base = ct_int(1, true), base->is_char = true;
        else if (!base && !strcmp(s, "_Bool")) base = ct_new(CT_BOOL), base->size = 1;
        else if (!base && !strcmp(s, "float")) base = ct_new(CT_FLOAT), base->size = 4;
        else if (!base && !strcmp(s, "double")) base = ct_new(CT_FLOAT), base->size = 8;
        else if (!base && (!strcmp(s, "__int128") || !strcmp(s, "__int128_t") || !strcmp(s, "__uint128_t"))) bad = "128-bit integers";
        else if (!base && !strcmp(s, "_Float16")) bad = "_Float16";
        else if (!strcmp(s, "_Complex")) bad = "complex numbers";
        else if (!base && !strcmp(s, "__builtin_va_list")) bad = "va_list";
        else if (!strcmp(s, "_Atomic")) {
            c->p++;
            if (is_p(c, "(")) skip_group(c);
            bad = "atomics";
            continue;
        } else if (!base && (!strcmp(s, "struct") || !strcmp(s, "union"))) {
            c->p++;
            base = struct_spec(c, !strcmp(s, "union"));
            continue;
        } else if (!base && !strcmp(s, "enum")) {
            c->p++;
            base = enum_spec(c);
            continue;
        } else if (!base && !nlong && !nshort && !nsigned && !nunsigned && !nint && is_typedef_name(c, s)) {
            CDecl *d = find_decl(c->h, s);
            base = ct_new(CT_NAMED);
            base->name = s;
            base->elem = d->k == CD_TYPE ? d->t : ct_bad(d->why);
        } else break;
        c->p++;
    }
    if (bad) base = ct_bad(bad);
    else if (base && base->k == CT_FLOAT && base->size == 8 && nlong) base = ct_bad("long double");
    else if (base && base->is_char && (nsigned || nunsigned)) base = ct_int(1, !nunsigned);
    else if (!base) {
        if (!nlong && !nshort && !nsigned && !nunsigned && !nint) cfail(c); /* no type at all */
        int size = nshort ? 2 : nlong ? 8 : 4;
        base = ct_int(size, !nunsigned);
    }
    return is_const ? ct_const(base) : base;
}

static CType *params(CP *c, CType *ret) {
    CType *f = ct_new(CT_FN);
    f->elem = ret;
    CType **ps = NULL;
    int np = 0, cp = 0;
    if (is_id(c, "void") && c->t[c->p + 1].k == CK_P && !strcmp(c->t[c->p + 1].s, ")")) c->p++;
    while (!accept_p(c, ")")) {
        if (accept_p(c, "...")) {
            f->variadic = true;
            expect_p(c, ")");
            break;
        }
        int st = 0;
        CType *base = specifiers(c, &st);
        const char *pname = NULL;
        CType *pt = declarator(c, base, &pname);
        if (pt->k == CT_ARRAY) pt = ct_ptr(pt->elem); /* int a[] as a parameter is int *a */
        if (pt->k == CT_FN) pt = ct_ptr(pt);
        PUSH(ps, np, cp, pt);
        if (!accept_p(c, ",")) {
            expect_p(c, ")");
            break;
        }
    }
    f->params = ps;
    f->np = np;
    return f;
}

/* after the name: [N] and (params), applied innermost first */
static CType *suffixes(CP *c, CType *base) {
    if (accept_p(c, "[")) {
        int64_t n = -1;
        while (is_id(c, "static") || is_id(c, "const") || is_id(c, "restrict") || is_id(c, "__restrict")) c->p++;
        if (!is_p(c, "]")) n = const_expr(c);
        expect_p(c, "]");
        CType *inner = suffixes(c, base);
        CType *a = ct_new(CT_ARRAY);
        a->elem = inner;
        a->n = n;
        return a;
    }
    if (accept_p(c, "(")) {
        CType *ret = base;
        CType *f = params(c, ret);
        return f;
    }
    return base;
}

static CType HOLE; /* where a nested declarator's type goes: int (*fp)(int) */

static CType *fill_hole(CType *t, CType *with) {
    if (t == &HOLE) return with;
    if (t->k == CT_PTR || t->k == CT_ARRAY || t->k == CT_FN) t->elem = fill_hole(t->elem, with);
    return t;
}

static CType *declarator(CP *c, CType *base, const char **name) {
    while (skip_noise(c)) {}
    while (accept_p(c, "*") || accept_p(c, "^")) {
        if (c->t[c->p - 1].s[0] == '^') base = ct_bad("Objective-C blocks");
        else base = ct_ptr(base);
        while (skip_noise(c) || is_id(c, "const") || is_id(c, "__const")) {
            if (is_id(c, "const") || is_id(c, "__const")) c->p++;
        }
    }
    if (is_p(c, "(")) {
        CTok *nx = &c->t[c->p + 1];
        bool nested = nx->k == CK_P && (!strcmp(nx->s, "*") || !strcmp(nx->s, "^") || !strcmp(nx->s, "("));
        if (nx->k == CK_ID && !starts_type(&(CP){.t = c->t, .p = c->p + 1, .n = c->n, .h = c->h}) && strncmp(nx->s, "__attribute", 11))
            nested = true;
        if (nested) {
            c->p++;
            CType *inner = declarator(c, &HOLE, name);
            expect_p(c, ")");
            base = suffixes(c, base);
            return fill_hole(inner, base);
        }
    }
    if (cur(c)->k == CK_ID && !starts_type(c)) {
        *name = cur(c)->s;
        c->p++;
    }
    CType *t = suffixes(c, base);
    while (skip_noise(c)) {}
    return t;
}

/* ---------------------------------------------------------------- constant expressions */

static int macro_depth;
static bool macro_value(CHeader *h, const char *name, int64_t *out);

static int64_t parse_int_text(const char *s, bool *ok) {
    char *end;
    int64_t v = (int64_t)strtoull(s, &end, 0);
    while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L') end++;
    *ok = *end == 0;
    return v;
}

static int64_t cx_cond(CP *c);

static int64_t cx_primary(CP *c) {
    CTok *t = cur(c);
    if (t->k == CK_NUM) {
        bool ok;
        int64_t v = parse_int_text(t->s, &ok);
        if (!ok) cfail(c);
        c->p++;
        return v;
    }
    if (t->k == CK_CHAR) {
        c->p++;
        return t->cval;
    }
    if (t->k == CK_ID) {
        if (!strcmp(t->s, "sizeof") || !strcmp(t->s, "__builtin_offsetof")) cfail(c);
        CDecl *d = find_decl(c->h, t->s);
        c->p++;
        if (d && d->k == CD_CONST_INT) return d->ival;
        int64_t v;
        if (!d && macro_value(c->h, t->s, &v)) return v;
        cfail(c);
    }
    if (accept_p(c, "(")) {
        if (starts_type(c)) { /* a cast: (int)x */
            int st = 0;
            CType *base = specifiers(c, &st);
            const char *nm = NULL;
            declarator(c, base, &nm);
            expect_p(c, ")");
            return cx_primary(c);
        }
        int64_t v = cx_cond(c);
        expect_p(c, ")");
        return v;
    }
    if (accept_p(c, "-")) return -cx_primary(c);
    if (accept_p(c, "+")) return cx_primary(c);
    if (accept_p(c, "~")) return ~cx_primary(c);
    if (accept_p(c, "!")) return !cx_primary(c);
    cfail(c);
}

static int prec_of(const char *op) {
    static const struct { const char *op; int prec; } ops[] = {
        {"*", 10}, {"/", 10}, {"%", 10}, {"+", 9}, {"-", 9}, {"<<", 8}, {">>", 8}, {"<", 7}, {">", 7}, {"<=", 7}, {">=", 7},
        {"==", 6}, {"!=", 6}, {"&", 5}, {"^", 4}, {"|", 3}, {"&&", 2}, {"||", 1},
    };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        if (!strcmp(ops[i].op, op)) return ops[i].prec;
    return 0;
}

static int64_t cx_binary(CP *c, int min) {
    int64_t l = cx_primary(c);
    for (;;) {
        CTok *t = cur(c);
        int pr = t->k == CK_P ? prec_of(t->s) : 0;
        if (!pr || pr < min) return l;
        const char *op = t->s;
        c->p++;
        int64_t r = cx_binary(c, pr + 1);
        switch (op[0]) {
        case '*': l = l * r; break;
        case '/': if (!r) cfail(c); l = l / r; break;
        case '%': if (!r) cfail(c); l = l % r; break;
        case '+': l = l + r; break;
        case '-': l = l - r; break;
        case '<': l = op[1] == '<' ? (int64_t)((uint64_t)l << r) : op[1] == '=' ? l <= r : l < r; break;
        case '>': l = op[1] == '>' ? l >> r : op[1] == '=' ? l >= r : l > r; break;
        case '=': l = l == r; break;
        case '!': l = l != r; break;
        case '&': l = op[1] == '&' ? (l && r) : (l & r); break;
        case '^': l = l ^ r; break;
        case '|': l = op[1] == '|' ? (l || r) : (l | r); break;
        }
    }
}

static int64_t cx_cond(CP *c) {
    int64_t v = cx_binary(c, 1);
    if (accept_p(c, "?")) {
        int64_t a = cx_cond(c);
        expect_p(c, ":");
        int64_t b = cx_cond(c);
        return v ? a : b;
    }
    return v;
}

static int64_t const_expr(CP *c) { return cx_cond(c); }

/* ---------------------------------------------------------------- macros */

static const char *find_macro(CHeader *h, const char *name, bool *fnlike) {
    for (int i = 0; i < h->nm; i++)
        if (!strcmp(h->mnames[i], name)) {
            *fnlike = h->mbodies[i] == NULL;
            return h->mbodies[i];
        }
    return NULL;
}

static bool has_macro(CHeader *h, const char *name) {
    for (int i = 0; i < h->nm; i++)
        if (!strcmp(h->mnames[i], name)) return true;
    return false;
}

/* an object-like macro whose body is an integer constant expression */
static bool macro_value(CHeader *h, const char *name, int64_t *out) {
    bool fnlike = false;
    const char *body = find_macro(h, name, &fnlike);
    if (!body || fnlike || macro_depth > 16) return false;
    int nt;
    CTok *toks = c_lex(body, &nt);
    if (!nt) return false;
    CP c;
    memset(&c, 0, sizeof c);
    c.t = toks;
    c.n = nt;
    c.h = h;
    macro_depth++;
    bool ok = false;
    if (!setjmp(c.fail)) {
        *out = const_expr(&c);
        ok = cur(&c)->k == CK_EOF;
    }
    macro_depth--;
    return ok;
}

/* a macro used as c.NAME: a number, a string, or another name */
static CDecl *macro_decl(CHeader *h, const char *name) {
    bool fnlike = false;
    const char *body = find_macro(h, name, &fnlike);
    if (fnlike) {
        CDecl *d = add_decl(h, CD_BAD, name);
        d->why = "it's a function-like macro";
        return d;
    }
    if (!body) return NULL;
    int nt;
    CTok *toks = c_lex(body, &nt);
    int64_t iv;
    if (macro_value(h, name, &iv)) {
        CDecl *d = add_decl(h, CD_CONST_INT, name);
        d->ival = iv;
        return d;
    }
    /* a float, maybe negated or in parentheses */
    int i = 0, close = 0;
    bool neg = false;
    while (i < nt && toks[i].k == CK_P && !strcmp(toks[i].s, "(")) i++, close++;
    if (i < nt && toks[i].k == CK_P && !strcmp(toks[i].s, "-")) neg = true, i++;
    if (i < nt && toks[i].k == CK_NUM && i + 1 + close == nt) {
        char *end;
        double v = strtod(toks[i].s, &end);
        if (*end == 'f' || *end == 'F' || *end == 'l' || *end == 'L') end++;
        if (!*end) {
            CDecl *d = add_decl(h, CD_CONST_FLOAT, name);
            d->fval = neg ? -v : v;
            return d;
        }
    }
    if (nt == 1 && toks[0].k == CK_STR) {
        CDecl *d = add_decl(h, CD_CONST_STR, name);
        d->sval = toks[0].str;
        d->slen = toks[0].slen;
        return d;
    }
    if (nt == 1 && toks[0].k == CK_ID && strcmp(toks[0].s, name) && macro_depth < 16) { /* #define stdin __stdinp */
        macro_depth++;
        CDecl *d = c_lookup(h, toks[0].s);
        macro_depth--;
        if (d) return d;
    }
    CDecl *d = add_decl(h, CD_BAD, name);
    d->why = strf("it's a macro that isn't a simple constant (#define %s %s)", name, body);
    return d;
}

/* ---------------------------------------------------------------- reading a header */

static void parse_decls(CHeader *h, CTok *toks, int n) {
    CP c;
    memset(&c, 0, sizeof c);
    c.t = toks;
    c.n = n;
    c.h = h;
    while (cur(&c)->k != CK_EOF) {
        int start = c.p;
        const char *last_name = NULL;
        if (setjmp(c.fail)) { /* something this parser doesn't understand: skip the declaration */
            c.p = start;
            int depth = 0;
            while (cur(&c)->k != CK_EOF) {
                if (is_p(&c, "(") || is_p(&c, "[") || is_p(&c, "{")) depth++;
                else if (is_p(&c, ")") || is_p(&c, "]")) depth--;
                else if (is_p(&c, "}")) {
                    if (--depth <= 0) {
                        c.p++;
                        if (is_p(&c, ";")) c.p++;
                        break;
                    }
                } else if (is_p(&c, ";") && depth <= 0) {
                    c.p++;
                    break;
                }
                c.p++;
            }
            if (last_name) add_bad(h, last_name, "Flint couldn't read its declaration");
            continue;
        }
        if (accept_p(&c, ";")) continue;
        int st = 0;
        CType *base = specifiers(&c, &st);
        if (accept_p(&c, ";")) continue; /* just a struct/enum definition */
        for (;;) {
            const char *name = NULL;
            asm_label = NULL;
            CType *t = declarator(&c, base, &name);
            last_name = name;
            if (!name) cfail(&c);
            if (is_p(&c, "{")) { /* a function body (static inline in a header) */
                skip_group(&c);
                add_bad(h, name, "it's a static inline function in the header: there's no symbol to link to");
                break;
            }
            if (accept_p(&c, "=")) { /* an initializer: skip it */
                int depth = 0;
                while (cur(&c)->k != CK_EOF && !(depth == 0 && (is_p(&c, ",") || is_p(&c, ";")))) {
                    if (is_p(&c, "(") || is_p(&c, "{") || is_p(&c, "[")) depth++;
                    if (is_p(&c, ")") || is_p(&c, "}") || is_p(&c, "]")) depth--;
                    c.p++;
                }
            }
            if (st & ST_TYPEDEF) {
                CDecl *d = add_decl(h, CD_TYPE, name);
                if (d) d->t = t;
            } else if (t->k == CT_FN) {
                if (st & ST_STATIC) add_bad(h, name, "it's a static function in the header: there's no symbol to link to");
                else {
                    CDecl *d = add_decl(h, CD_FN, name);
                    if (d) d->t = t, d->sval = asm_label;
                }
            } else if (!(st & ST_STATIC)) {
                CDecl *d = add_decl(h, CD_VAR, name);
                if (d) d->t = t, d->sval = asm_label;
            }
            if (!accept_p(&c, ",")) break;
        }
        if (!is_p(&c, ";") && c.t[c.p - 1].k == CK_P && !strcmp(c.t[c.p - 1].s, "}")) continue; /* after a body */
        expect_p(&c, ";");
    }
}

static void read_macros(CHeader *h, const char *text) {
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        if (!strncmp(p, "#define ", 8)) {
            const char *s = p + 8, *q = s;
            while (q < e && (isalnum((unsigned char)*q) || *q == '_')) q++;
            const char *name = xstrndup(s, (size_t)(q - s));
            const char *body = NULL;
            if (q < e && *q == '(') body = NULL; /* function-like */
            else {
                while (q < e && (*q == ' ' || *q == '\t')) q++;
                body = xstrndup(q, (size_t)(e - q));
            }
            PUSH(h->mnames, h->nm, h->cm, name);
            int nb = h->nm - 1;
            PUSH(h->mbodies, nb, h->cmb, body);
        }
        p = *e ? e + 1 : e;
    }
}

CHeader *c_header(const char *header, Unit *from, Token *tok) {
    const char *dir = ".";
    if (from->file && from->file[0] != '<') {
        char *d = strf("%s", from->file);
        char *slash = strrchr(d, '/');
        if (slash) *slash = 0;
        dir = d;
    }
    const char *key = strf("%s|%s", header, dir);
    if (from->pkg)
        for (int i = 0; i < from->pkg->ncinc; i++) key = strf("%s|%s", key, from->pkg->cinc[i]);
    for (int i = 0; i < nheaders; i++)
        if (!strcmp(headers[i]->key, key)) return headers[i];
    /* a one-line C file that includes the header */
    char tmpl[] = "/tmp/flint-cimport-XXXXXX";
    char *tdir = mkdtemp(tmpl);
    if (!tdir) error_at(tok, "can't create a temp directory to read %s", header);
    const char *cfile = strf("%s/h.c", tdir);
    FILE *f = fopen(cfile, "w");
    fprintf(f, "#include \"%s\"\n", header);
    fclose(f);
    const char **argv = NULL;
    int na = 0, ca = 0;
    PUSH(argv, na, ca, g_cc);
    PUSH(argv, na, ca, "-E");
    PUSH(argv, na, ca, "-P");
    PUSH(argv, na, ca, strf("-I%s", dir));
    if (from->pkg)
        for (int i = 0; i < from->pkg->ncinc; i++) PUSH(argv, na, ca, strf("-I%s", from->pkg->cinc[i]));
    PUSH(argv, na, ca, cfile);
    PUSH(argv, na, ca, NULL);
    const char *efile = strf("%s/err.txt", tdir);
    char *pre = run_capture(argv, efile);
    char *macros = NULL;
    if (pre) {
        argv[1] = "-dM"; /* the same, listing the macros instead */
        argv[2] = "-E";
        macros = run_capture(argv, efile);
    }
    char *errs = read_file(efile);
    unlink(cfile);
    unlink(efile);
    rmdir(tdir);
    if (!pre || !macros) { /* show the C compiler's own complaint, without the temp file's name */
        const char *why = errs ? strstr(errs, "error: ") : NULL;
        if (why) {
            why += 7;
            char *nl = strchr(why, '\n');
            if (nl) *nl = 0;
            error_at(tok, "the C preprocessor couldn't read \"%s\": %s", header, why);
        }
        error_at(tok, "the C preprocessor couldn't read \"%s\"", header);
    }
    CHeader *h = xalloc(sizeof *h);
    h->name = header;
    h->key = key;
    read_macros(h, macros);
    int nt;
    CTok *toks = c_lex(pre, &nt);
    parse_decls(h, toks, nt);
    PUSH(headers, nheaders, cheaders, h);
    return h;
}

CDecl *c_lookup(CHeader *h, const char *name) {
    CDecl *d = find_decl(h, name);
    if (d) return d;
    const char *kinds[] = {"struct", "union", "enum"};
    for (int i = 0; i < 3; i++) { /* c.struct_tm */
        size_t kl = strlen(kinds[i]);
        if (strncmp(name, kinds[i], kl) || name[kl] != '_') continue;
        const char *display = strf("%s %s", kinds[i], name + kl + 1);
        for (int j = 0; j < ntags; j++)
            if (!strcmp(tags[j]->display, display)) {
                CDecl *td = add_decl(h, CD_TYPE, name);
                td->t = ct_new(CT_STRUCT);
                td->t->st = tags[j];
                return td;
            }
        if (i == 2) { /* enum tags only matter as integers */
            CDecl *td = add_decl(h, CD_TYPE, name);
            td->t = ct_int(4, true);
            return td;
        }
    }
    if (has_macro(h, name)) return macro_decl(h, name);
    return NULL;
}
