/* flint.h - shared declarations for the Flint compiler */
#ifndef FLINT_H
#define FLINT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================= util ================= */

void *xalloc(size_t n); /* zeroed */
void *xrealloc(void *p, size_t n);
char *xstrndup(const char *s, size_t n);
char *strf(const char *fmt, ...);

/* Append to a growable array described by (arr, len, cap). */
#define PUSH(arr, len, cap, val)                                              \
    do {                                                                      \
        if ((len) >= (cap)) {                                                 \
            (cap) = (cap) ? (cap) * 2 : 8;                                    \
            (arr) = xrealloc((arr), sizeof(*(arr)) * (size_t)(cap));          \
        }                                                                     \
        (arr)[(len)++] = (val);                                               \
    } while (0)

/* ================= tokens ================= */

typedef enum { TK_ID, TK_KW, TK_INT, TK_FLOAT, TK_STR, TK_OP, TK_EOF } TokKind;

typedef struct Token {
    TokKind k;
    const char *s; /* text of identifier / keyword / operator */
    uint64_t ival;
    double fval;
    char *str; /* string literal bytes */
    int slen;
    int line, col;
    bool autosemi; /* a ';' inserted at a newline */
    const char *path, *src; /* where this token came from (for error messages) */
} Token;

extern const char *g_path;
extern const char *g_src;
_Noreturn void error_at(Token *t, const char *fmt, ...);
extern Token *g_calls[64]; /* calls of generic functions being instantiated (for error notes) */
extern int g_ncalls;

Token *lex(const char *src, const char *path);

/* ================= modules ================= */

typedef struct Unit Unit;
typedef struct Package Package;
typedef struct CHeader CHeader;
typedef struct Node Node;

typedef struct {
    const char *alias;  /* the name the file uses: `io` for `import std.io;` */
    const char *mpath;  /* "std.io", or NULL for a C header */
    const char *header; /* import c "stdio.h" as c: the header */
    Unit *u;            /* the module it names (resolved by the loader) */
    CHeader *c;         /* ... or the C header */
    Token *tok;
} Import;

struct Unit {
    const char *path;   /* dotted module path: "std.io", "net.http" (the root file: its stem) */
    const char *file;   /* where it was read from */
    const char *prefix; /* symbol prefix: "" for the root module, "net_http__" otherwise */
    const char *dname;  /* display prefix for its types: "http." ("" for the root module) */
    Package *pkg;
    bool is_std, is_root;
    Token *toks;
    Node **items;
    int nitems;
    Import *imports;
    int nimports, cimports;
};

/* a package: a directory with a flint.toml (or, without one, the root file's directory) */
typedef struct {
    const char *name;
    Package *pkg;
} Dep;

struct Package {
    const char *name;
    const char *dir;  /* the manifest's directory */
    const char *root; /* the root source file */
    const char *rootdir; /* module paths are resolved from here: the root file's directory */
    Dep *deps;
    int ndeps, cdeps;
    const char **csrc, **cinc, **cflags, **clink;
    int ncsrc, ncinc, ncflags, nclink;
};

Package *read_manifest(const char *dir); /* manifest.c */
const char *find_manifest(const char *start); /* the directory holding flint.toml, or NULL */

/* module.c: read the root file and every module it imports (all_files: also every .fl under the root) */
Unit **load_program(const char *root_file, Package *pkg, bool all_files, int *nunits);
char *read_file(const char *path);
char *read_file_len(const char *path, long *len);
bool std_has(const char *name);
extern const char *g_cc; /* the C compiler used for C headers and C sources */

/* cimport.c: C headers, read with the C preprocessor and a small parser for declarations.
 * lower.c turns what's used into Flint types and extern functions. */
typedef enum { CT_VOID, CT_INT, CT_FLOAT, CT_BOOL, CT_PTR, CT_ARRAY, CT_FN, CT_STRUCT, CT_ENUM, CT_NAMED, CT_BAD } CTKind;
typedef struct CType CType;
typedef struct CStruct CStruct;
struct CType {
    CTKind k;
    int size;        /* CT_INT, CT_FLOAT, CT_ENUM */
    bool sign;       /* CT_INT, CT_ENUM */
    bool is_char;    /* plain `char` */
    bool is_const;   /* const-qualified (matters for what a pointer points at) */
    CType *elem;     /* CT_PTR / CT_ARRAY: the item; CT_FN: the result; CT_NAMED: what the typedef names */
    int64_t n;       /* CT_ARRAY: the length (-1 if unknown) */
    CType **params;  /* CT_FN */
    int np;
    bool variadic;
    CStruct *st;     /* CT_STRUCT */
    const char *name; /* CT_NAMED: the typedef's name */
    const char *why;  /* CT_BAD: why Flint can't use it */
};
typedef struct {
    const char *name;
    CType *t;
} CField;
struct CStruct {
    const char *tag;  /* "tm" for struct tm (or a made-up name for an anonymous one) */
    const char *display; /* "struct tm" */
    bool is_union, complete;
    CField *fields;
    int nf, cf;
    const char *why;  /* set if Flint can't lay it out (bit fields, ...) */
    void *flint;      /* the Flint Type made for it */
};
typedef enum { CD_FN, CD_VAR, CD_TYPE, CD_CONST_INT, CD_CONST_FLOAT, CD_CONST_STR, CD_BAD } CDeclKind;
typedef struct {
    CDeclKind k;
    const char *name;
    const char *why; /* CD_BAD: why it can't be used from Flint */
    CType *t;        /* CD_FN / CD_VAR / CD_TYPE */
    int64_t ival;
    double fval;
    const char *sval;
    int slen;
    void *cache;     /* the Flint declaration made for it */
} CDecl;
CHeader *c_header(const char *header, Unit *from, Token *tok);
CDecl *c_lookup(CHeader *h, const char *name); /* NULL if the header doesn't declare it */
const char *c_header_name(CHeader *h);

/* ================= AST ================= */

typedef enum {
    N_FN, N_STRUCT, N_EXTERN, N_PARAM, N_FIELD,
    N_TNAME, N_TPTR, N_TSLICE,
    N_BLOCK, N_LET, N_ASSIGN, N_INCR, N_EXPRSTMT, N_IF, N_WHILE, N_FORRANGE, N_FOREACH,
    N_RETURN, N_BREAK, N_CONTINUE, N_DEFER,
    N_INT, N_FLOAT, N_STR, N_BOOL, N_NULL, N_NAME, N_UNARY, N_BINARY, N_TERNARY, N_CAST,
    N_CALL, N_INDEX, N_SLICE, N_FIELDACC, N_STRUCTLIT, N_ARRAY,
    N_DEREF, N_DISCARD, N_UNDEFINED, N_BUILTIN,
    N_ENUM, N_VARIANT, N_DOTTAG, N_SWITCH, N_ARM, N_PATTERN, N_FALLTHROUGH,
    N_TOPT, N_UNWRAP, N_ORELSE, N_UNREACHABLE,
    N_ERRSET, N_ERRLIT, N_TERRU, N_TRY, N_CATCH,
    N_TFN, N_TARRAY, N_ARRAYLIT, N_TTUPLE, N_ANONLIT,
    N_TGENERIC, /* List(i32) in a type position: name, args in list */
    N_TTYPEOF,  /* @TypeOf(expr): a = the expression */
    N_TYPEEXPR, /* a type written where an expression goes (a type argument): a = the type node */
    N_TEST,     /* test "name" { ... }: str = the name, c = the body */
    N_COMPTIME, /* comptime expr / comptime { ... } (flag = 1: a block, run for its effects): a = it */
    N_TVECTOR,  /* @Vector(N, T): c = N, a = T */
} NodeKind;

struct Node {
    NodeKind k;
    Token *tok;
    const char *name;  /* identifier / operator / declared name */
    const char *name2; /* method receiver (display name) / loop index variable */
    /* if / while / if-expression: `name` is the |capture| of the value, `name2` the else |err| capture */
    Node *a, *b, *c;
    Node **list; /* statements, args, params, fields, items */
    int n, cap;
    uint64_t ival;
    double fval;
    char *str; /* string literal bytes, or extern header */
    int slen;
    int flag; /* let (immutable) / inclusive range / variadic / const pointee / pointer capture */
    int flag2; /* type nodes: [*]T (many-item pointer). if/while: the capture is |*v| */
    bool exported; /* export fn/let/var: C ABI, unmangled, visible to the linker */
    bool is_extern; /* extern let/var: a global defined in C */
    bool is_unsafe; /* unsafe fn, or an unsafe { } block */
    const char *label; /* loop label, or the label named by break/continue */
    Unit *unit;  /* the module (file) it was written in */
    bool is_pub; /* pub: visible to other modules */
    Node *wherec; /* fn ... where cond: the constraint on its comptime parameters */
    Token *wbeg, *wend; /* ... its first token and the one after it (to quote the clause in errors) */
};

void parse_imports(Token *toks, Unit *u); /* just the `import` lines at the top */
void prescan(Token *toks);                /* type names, before any file is parsed */
void parse(Token *toks, Unit *u);         /* fills u->items */

/* ================= types ================= */

typedef enum {
    TY_VOID, TY_BOOL, TY_I8, TY_I16, TY_I32, TY_I64, TY_U8, TY_U16, TY_U32, TY_U64,
    TY_F32, TY_F64, TY_PTR, TY_SLICE, TY_STRUCT, TY_NULL, TY_LITINT, TY_LITFLOAT, TY_ENUM,
    TY_OPT, TY_NORETURN, TY_ERRSET, TY_ERRUNION, TY_FN, TY_ARRAY,
    TY_TYPE,   /* a type, as a compile-time value (its id) */
    TY_VECTOR, /* @Vector(N, T): elem, alen; 8 or 16 bytes */
} TyKind;

typedef struct Type Type;
typedef struct {
    const char *name;
    Type *t;
    int off;
    Token *tok;
    Node *def; /* default value, or NULL */
    int bits, bitoff; /* packed struct: width and position in the backing integer */
    bool sbits;       /* packed struct: the field is signed (sign-extended when read) */
} Field;

typedef struct {
    const char *name;
    int64_t value; /* the tag */
    int kind;      /* 0 = no payload, 1 = tuple payload Circle(f32), 2 = struct payload Rect { w: f32 } */
    Type *payload; /* anonymous struct holding the payload (fields _0, _1, ... for tuples) */
    Token *tok;
} Variant;

struct Type {
    TyKind k;
    Type *elem;        /* pointee / slice element */
    Type *ptr, *slice;   /* cached derived types */
    Type *cptr, *cslice; /* ... and their const-pointee versions */
    Type *opt;           /* ?T */
    Type *eus, *eu_next; /* error unions E!T with this T (a list), and the next in that list */
    Type *eset;          /* E!T: the error set E */
    int *errcodes;       /* error set: its errors' global codes */
    int nerrs;
    bool all_errors;     /* anyerror */
    bool is_const;       /* pointer/slice: the pointee is read-only */
    bool many;           /* [*]T: a pointer to an unknown number of items (indexable) */
    Type *mptr, *cmptr;  /* cached [*]T and [*]const T */
    Type **params;       /* fn(...) -> T: parameter types (elem is the result) */
    int nparams;
    bool variadic;
    int64_t alen;        /* [N]T: N */
    bool tuple;          /* (A, B): a struct with fields named 0, 1, ... */
    bool packed;         /* packed struct: bit fields in one integer (tagt is that integer) */
    bool is_union;       /* extern union: every field at offset 0 */
    int pbits;           /* packed struct: total bits */
    Variant *variants;   /* enums */
    int nvariants;
    Type *tagt;          /* enum tag type (u8, u16, ...) */
    bool has_payload;    /* tagged union: { tag, payload }; otherwise a plain integer */
    int payload_off;
    const char *name;  /* struct name */
    const char *mname; /* its name in symbols (generic instances, nested and module types) */
    const char *dname; /* its name in messages, when that differs from `name` (io.Writer) */
    Unit *unit;        /* the module that declares it */
    bool is_pub;
    bool opaque;       /* a C struct Flint can't lay out (no body, bit fields): only usable behind a pointer */
    const char *opaque_why;
    void *genv;        /* ... and the compile-time bindings its declarations see */
    Field *fields;
    int nfields;
    int size, align;
    int state; /* struct layout: 0 = todo, 1 = in progress, 2 = done */
    int tid;   /* its id as a comptime value (0 = none yet) */
};

/* ================= IR =================
 * Functions are lists of basic blocks. Values live in an unlimited supply of
 * virtual registers (vregs), each with a register class. Memory locals live in
 * stack "slots"; the optimizer promotes slots whose address never escapes into
 * vregs, and the backend maps vregs onto real ARM64 registers. */

enum { C_I, C_F32, C_F64 }; /* register classes: 64-bit integer, single, double */
enum { O_NONE, O_REG, O_IMM };

typedef struct {
    int k;
    int64_t v; /* vreg number or immediate */
} Opd;

enum {
    I_NOP, I_GETARG, I_ICONST, I_FCONST, I_MOV,
    I_ADD, I_SUB, I_MUL, I_SDIV, I_UDIV, I_SREM, I_UREM, I_AND, I_OR, I_XOR,
    I_SHL, I_ASR, I_LSR, I_NEG, I_NOT,
    I_FADD, I_FSUB, I_FMUL, I_FDIV, I_FNEG, I_FSQRT, I_FABS,
    I_CMP, I_FCMP, I_SEL, I_EXT, I_SCVTF, I_UCVTF, I_FCVTZS, I_FCVTZU, I_FCVT,
    I_LOAD, I_STORE, I_ADDR_SLOT, I_ADDR_SYM, I_CALL, I_MEMCPY, I_MEMZERO,
    I_JMP, I_BR, I_RET, I_TRAP,
    I_SMULH, I_UMULH, /* high 64 bits of a 128-bit product (overflow checks) */
    I_VEC,     /* SIMD on memory: [a] = [b] op [c] (a, b, c hold addresses); mem = VOP_*, imm = VLANES() */
    I_VREDUCE, /* dst = the sum of the lanes at [a]; imm = VLANES() */
    I_COUNT
};

/* I_VEC operations. VOP_SPLAT: every lane of [a] = b (a scalar of class cls); VOP_NEG: [a] = -[b] */
enum { VOP_ADD, VOP_SUB, VOP_MUL, VOP_DIV, VOP_AND, VOP_OR, VOP_XOR, VOP_NEG, VOP_SPLAT };
/* lane layout of a vector op: element size in bytes, float lanes, signed lanes (for the reduce result), total bytes */
#define VLANES(esize, isfloat, issigned, total) ((esize) | ((isfloat) ? 0x10 : 0) | ((issigned) ? 0x20 : 0) | ((total) << 8))
#define VL_ESIZE(imm) ((int)((imm) & 0xf))
#define VL_FLOAT(imm) (((imm) & 0x10) != 0)
#define VL_SIGNED(imm) (((imm) & 0x20) != 0)
#define VL_TOTAL(imm) ((int)((imm) >> 8))

/* condition codes (ARM64 names); float compares use MI/LS/GT/GE/EQ/NE */
enum { CC_EQ, CC_NE, CC_LT, CC_LE, CC_GT, CC_GE, CC_LO, CC_LS, CC_HI, CC_HS, CC_MI, CC_PL };

typedef struct {
    int op;
    int cls;  /* register class of the result (or of the value for STORE/RET) */
    int mem;  /* LOAD/STORE/EXT: byte size | 0x80 if signed. CMP: condition code */
    int dst;  /* defined vreg or -1 */
    Opd a, b, c;
    int slot;    /* LOAD/STORE: base slot (-1 = base address is vreg a). ADDR_SLOT: slot */
    int64_t imm; /* LOAD/STORE offset, ICONST value, GETARG index, MEMCPY size */
    double fimm; /* FCONST value */
    int t1, t2;  /* JMP/BR targets */
    const char *sym; /* CALL target, ADDR_SYM symbol */
    Opd *args;
    int *argcls;
    int nargs, nfixed; /* args past nfixed are C varargs */
    Token *tok; /* the source it came from (for errors found when it runs at compile time) */
} Inst;

typedef struct {
    Inst *insts;
    int n, cap;
    int seq;   /* layout order: the order in which blocks were started */
    bool cold; /* unlikely path (panics): laid out at the end */
    bool dead;
} Block;

typedef struct {
    int size, align;
} Slot;

typedef struct {
    const char *sym;
    const char *name; /* for messages: "fib", "List(i32).push" */
    bool ct_only; /* lowered for comptime code: emitted only if run-time code reaches it too */
    bool is_main;
    bool exported;
    Block *blocks;
    int nblocks, bcap;
    int *vcls; /* class of each vreg */
    int nv, vcap;
    Slot *slots;
    int nslots, scap;
    int retcls; /* -1 = returns nothing in a register */
} IRFunc;

typedef struct {
    const char *label;
    char *data;
    int len;
} StrLit;

enum { GV_ZERO, GV_INT, GV_STR, GV_FILL, GV_DATA }; /* GV_FILL: 0xAA bytes (undefined, Debug) */
/* GV_DATA: a constant aggregate, as pieces at byte offsets (the gaps are zero) */
typedef struct {
    int off, size;
    int64_t val;     /* raw bits */
    const char *sym; /* or the address of this symbol (+ val) */
} DataItem;

typedef struct {
    const char *sym;
    int size, align, kind;
    int64_t ival; /* raw bits for GV_INT */
    int sidx;     /* string index for GV_STR */
    bool exported; /* visible to the linker (.globl) */
    bool is_const; /* a `let`: comptime code may read it */
    const char *name; /* for messages */
    DataItem *items; /* GV_DATA */
    int nitems, citems;
} GlobalVar;

typedef struct {
    IRFunc **funcs;
    int nfuncs, fcap;
    const char **errnames; /* error code -> name (code 0 = success) */
    int nerrnames, errcap;
    bool main_errors;      /* main returns !void */
    StrLit *strs;
    int nstrs, scap;
    GlobalVar *globals;
    int nglobals, gcap;
} Module;

/* build modes (design.md section 6.2) */
enum { MODE_DEBUG, MODE_SAFE, MODE_FAST };
Module *lower_program(Unit **units, int nunits, int mode, bool test_mode);
void optimize(Module *m);
void emit_arm64(Module *m, FILE *out);
void dump_ir(Module *m, FILE *out);

/* interp.c: runs IR at compile time (comptime). Deterministic, no I/O, with a step limit.
 * A pointer is (region << 32) | offset; regions are frames, string literals, globals and functions. */
typedef IRFunc *(*CtLookup)(const char *sym, const char **why); /* the IR of a function, or NULL (why: the reason) */
typedef struct {
    const char *msg;
    Token *tok;        /* the instruction that failed (may be NULL) */
    Token *calls[16];  /* the calls that led there, innermost first */
    const char *fns[16];
    int ncalls;
} CtError;
enum { CR_NONE, CR_FRAME, CR_STR, CR_GLOBAL, CR_FUNC, CR_EXTERN, CR_CONST };
typedef struct {
    int kind;
    const char *sym; /* CR_STR / CR_GLOBAL / CR_FUNC: its symbol */
    uint8_t *data;
    int64_t size;
    int id;
} CtRegion;
void ct_init(Module *m, CtLookup lookup);
bool ct_run(IRFunc *f, uint64_t *ret, CtError *err); /* f takes no arguments; ret: its x0 / d0 bits */
bool ct_region(uint64_t ptr, CtRegion *out, int64_t *off); /* what a comptime pointer points into */
void ct_release(void);                                    /* free the frames of the last run */
extern int64_t ct_step_limit;
_Noreturn void error_notes(Token *t, int nn, Token **nt, const char **nm, const char *fmt, ...);

int inst_uses(Inst *in, int *out); /* vregs read by an instruction; returns count */
bool inst_is_term(Inst *in);
bool inst_has_effects(Inst *in);
int new_vreg_in(IRFunc *f, int cls);
int block_succs(Block *b, int *out);

#endif
