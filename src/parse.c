/* parse.c - tokens -> AST. Recursive descent, with precedence climbing for
 * binary operators. The grammar is in docs/design.md, section 2. */
#include "flint.h"

static Token *T;
static int P;
static const char **snames; /* struct names (pre-scanned), so `Name {` starts a struct literal */
static int nsnames, csnames;
static const char **enames; /* enum names, so `Shape.Rect {` starts a variant literal */
static int nenames, cenames;
static const char **tfnames; /* functions returning `type`, so `List(i32) {` starts a struct literal */
static int ntfnames, ctfnames;
static const char *cur_struct; /* struct whose body we're in: `Self` means this type */
static bool in_receiver;       /* parsing `fn []T.name`: the '.' ends the type */
static Unit *cur_unit;         /* the file being parsed */

static Node *expr(void);
static Node *stmt(void);
static Node *block(void);
static Node *type_(void);
static int no_lit; /* parsing a `where` clause: `Name {` starts the body, not a literal */
static Node *let_stmt(void);
static Node *struct_decl(void);

/* ---------- helpers ---------- */

static Node *mk(NodeKind k, Token *t) {
    Node *n = xalloc(sizeof *n);
    n->k = k;
    n->tok = t;
    n->unit = cur_unit;
    return n;
}

static void add(Node *n, Node *c) { PUSH(n->list, n->n, n->cap, c); }

static Token *peek(int o) {
    int i = P;
    while (o-- > 0 && T[i].k != TK_EOF) i++;
    return &T[i];
}

static bool is_op(Token *t, const char *v) { return (t->k == TK_OP || t->k == TK_KW) && !strcmp(t->s, v); }
static bool at(const char *v) { return is_op(&T[P], v); }
static bool at_n(int o, const char *v) { return is_op(peek(o), v); }

static Token *next(void) {
    Token *t = &T[P];
    if (t->k != TK_EOF) P++;
    return t;
}

static Token *accept(const char *v) { return at(v) ? next() : NULL; }

static const char *desc(Token *t) {
    switch (t->k) {
    case TK_EOF: return "end of file";
    case TK_INT: return strf("'%llu'", (unsigned long long)t->ival);
    case TK_FLOAT: return "a number";
    case TK_STR: return "a string";
    default: return strf("'%s'", t->s);
    }
}

static Token *expect(const char *v, const char *what) {
    if (at(v)) return next();
    if (what) error_at(&T[P], "expected %s, found %s", what, desc(&T[P]));
    error_at(&T[P], "expected '%s', found %s", v, desc(&T[P]));
}

static Token *ident(const char *what) {
    if (T[P].k != TK_ID || T[P].s[0] == '@') error_at(&T[P], "expected %s, found %s", what, desc(&T[P]));
    return next();
}

/* Require ';'. On failure, point just past the previous token: that's where it's missing. */
static void semi(const char *after) {
    if (accept(";")) return;
    Token *prev = P > 0 ? &T[P - 1] : &T[P];
    Token at_end = *prev;
    if (prev->k == TK_ID || prev->k == TK_KW || prev->k == TK_OP) at_end.col += (int)strlen(prev->s);
    error_at(&at_end, "expected ';' after %s", after);
}

static bool is_enum_name(const char *s) {
    for (int i = 0; i < nenames; i++)
        if (!strcmp(enames[i], s)) return true;
    return false;
}

static bool is_type_fn_name(const char *s) {
    for (int i = 0; i < ntfnames; i++)
        if (!strcmp(tfnames[i], s)) return true;
    return false;
}

/* `io` in `import std.io;`: so `geo.Point { ... }` starts a struct literal */
static bool is_import_alias(const char *s) {
    if (!cur_unit) return false;
    for (int i = 0; i < cur_unit->nimports; i++)
        if (!strcmp(cur_unit->imports[i].alias, s)) return true;
    return false;
}

static bool is_alias_node(Node *e) { return e->k == N_NAME && is_import_alias(e->name); }

/* `c` in `import c "stdio.h" as c;`: its types aren't known until the header is read, so c.Name { ... }
 * is always a struct literal */
static bool is_c_alias(const char *s) {
    if (!cur_unit) return false;
    for (int i = 0; i < cur_unit->nimports; i++)
        if (cur_unit->imports[i].header && !strcmp(cur_unit->imports[i].alias, s)) return true;
    return false;
}

static bool is_struct_name(const char *s) {
    if (cur_struct && !strcmp(s, "Self")) return true;
    for (int i = 0; i < nsnames; i++)
        if (!strcmp(snames[i], s)) return true;
    return false;
}

static const char *self_name(Token *t) {
    if (!cur_struct) error_at(t, "'Self' can only be used inside a struct");
    return cur_struct;
}

/* ---------- types ---------- */

static Node *type_(void) {
    Token *t = &T[P];
    if (accept("*")) {
        Node *n = mk(N_TPTR, t);
        n->flag = accept("const") != NULL;
        n->a = type_();
        return n;
    }
    if (accept("[")) {
        if (at("*") && at_n(1, "]")) { /* [*]T */
            next();
            next();
            Node *n = mk(N_TPTR, t);
            n->flag2 = 1;
            n->flag = accept("const") != NULL;
            n->a = type_();
            return n;
        }
        if (!at("]")) { /* [N]T, or [_]T in an array literal */
            Node *n = mk(N_TARRAY, t);
            if (T[P].k == TK_ID && !strcmp(T[P].s, "_") && at_n(1, "]")) next();
            else n->c = expr();
            expect("]", "']' after the array length");
            n->a = type_();
            return n;
        }
        next();
        Node *n = mk(N_TSLICE, t);
        n->flag = accept("const") != NULL;
        n->a = type_();
        return n;
    }
    if (accept("(")) { /* (A, B): a tuple */
        Node *n = mk(N_TTUPLE, t);
        add(n, type_());
        if (!at(",")) error_at(&T[P], "a tuple type needs at least two elements: (A, B)");
        while (accept(",")) {
            if (at(")")) break;
            add(n, type_());
        }
        expect(")", "')' to close the tuple type");
        return n;
    }
    if (accept("?")) {
        Node *n = mk(N_TOPT, t);
        n->a = type_();
        return n;
    }
    if (at("!")) { /* !T: can fail with any error */
        next();
        Node *n = mk(N_TERRU, t);
        n->b = type_();
        return n;
    }
    if (accept("fn")) { /* fn(i32, i32) -> i32 */
        Node *n = mk(N_TFN, t);
        expect("(", "'(' and parameter types after 'fn'");
        while (!at(")")) {
            if (accept("...")) {
                n->flag = 1;
                break;
            }
            if (T[P].k == TK_ID && at_n(1, ":"))
                error_at(&T[P], "function types list just the parameter types: write fn(%s) without names", "T");
            add(n, type_());
            if (!accept(",")) break;
        }
        expect(")", "')' or ',' in the parameter types");
        if (accept("->")) n->b = type_();
        return n;
    }
    if (T[P].k == TK_ID && !strcmp(T[P].s, "@Vector")) { /* @Vector(4, f32): a SIMD vector */
        Node *n = mk(N_TVECTOR, next());
        expect("(", "'(' after @Vector");
        n->c = expr();
        expect(",", "',' and the element type: @Vector(4, f32)");
        n->a = type_();
        expect(")", "')' to close @Vector");
        return n;
    }
    if (T[P].k == TK_ID && !strcmp(T[P].s, "@TypeOf")) { /* @TypeOf(expr) */
        Node *n = mk(N_TTYPEOF, next());
        expect("(", "'(' after @TypeOf");
        n->a = expr();
        expect(")", "')' to close @TypeOf");
        return n;
    }
    Token *id = ident("a type");
    Node *n = mk(N_TNAME, id);
    n->name = !strcmp(id->s, "Self") ? self_name(id) : id->s;
    while (!in_receiver && at(".") && peek(1)->k == TK_ID && peek(1)->s[0] != '@') { /* heap.Arena: a type inside a type */
        next();
        n->name = strf("%s.%s", n->name, next()->s);
    }
    if (!in_receiver && at("(")) { /* List(i32): a type made by a function that returns `type` */
        next();
        n->k = N_TGENERIC;
        while (!at(")")) {
            Token *a = &T[P];
            if (a->k == TK_INT || a->k == TK_STR || a->k == TK_FLOAT || is_op(a, "-") || is_op(a, "true") || is_op(a, "false"))
                add(n, expr());
            else
                add(n, type_());
            if (!accept(",")) break;
        }
        expect(")", "')' or ',' after the type arguments");
        while (at(".") && peek(1)->k == TK_ID) { /* Tree(i32).Node: a type inside an instance */
            next();
            const char *m = next()->s;
            n->name2 = n->name2 ? strf("%s.%s", n->name2, m) : m;
        }
    }
    if (at("!") && T[P].k == TK_OP) { /* E!T */
        Token *bang = next();
        Node *u = mk(N_TERRU, bang);
        u->a = n;
        u->b = type_();
        return u;
    }
    return n;
}

/* ---------- declarations ---------- */

/* fn name(params) -> T   (also `fn T.name(...)` for methods on built-in types) */
static bool at_fn(void) { return at("fn") || (at("unsafe") && at_n(1, "fn")); }

static Node *fn_sig(void) {
    bool uns = accept("unsafe");
    expect("fn", NULL);
    Node *f;
    if ((at("[") || at("*") || (T[P].k == TK_ID && at_n(1, "."))) && !(cur_unit && cur_unit->is_std))
        error_at(&T[P], "methods are declared inside their type's body; for a built-in type (str, u8, slices) write a plain "
                        "function in a module instead, like 'pub fn trim(s: []const u8) -> []const u8'");
    if (at("[") || at("*")) {
        in_receiver = true;
        Node *rt = type_();
        in_receiver = false;
        expect(".", "'.' and a method name after the receiver type");
        Token *name = ident("method name");
        f = mk(N_FN, name);
        f->name = name->s;
        f->a = rt;
        f->name2 = "receiver";
    } else {
        Token *name = ident("function name");
        f = mk(N_FN, name);
        f->name = name->s;
        if (accept(".")) {
            Node *rt = mk(N_TNAME, name);
            rt->name = name->s;
            f->a = rt;
            f->name2 = rt->name;
            name = ident("method name");
            f->name = name->s;
            f->tok = name;
        }
    }
    f->is_unsafe = uns;
    expect("(", NULL);
    while (!at(")")) {
        if (accept("...")) {
            f->flag = 1;
            break;
        }
        bool ct = accept("comptime") != NULL;
        Token *pn = ident("parameter name");
        expect(":", "':' and a type after the parameter name");
        Node *p = mk(N_PARAM, pn);
        p->name = pn->s;
        p->flag = ct; /* comptime: known at compile time; each distinct value makes its own copy of the function */
        p->a = type_();
        if (p->a->k == N_TNAME && !strcmp(p->a->name, "type") && !ct)
            error_at(pn, "a type parameter must be known at compile time: write 'comptime %s: type'", pn->s);
        if (p->a->k == N_TNAME && !strcmp(p->a->name, "anytype") && ct)
            error_at(pn, "'anytype' already takes its type from the argument; drop 'comptime'");
        add(f, p);
        if (!accept(",")) break;
    }
    expect(")", "')' or ',' in the parameter list");
    if (accept("->")) f->b = type_();
    if (accept("where")) { /* fn sum(comptime T: type, ...) -> T where isNumber(T) */
        no_lit++;
        f->wbeg = &T[P];
        f->wherec = expr();
        no_lit--;
        f->wend = &T[P];
    }
    return f;
}

static Node *fn_body(Node *f) {
    if (!at("{")) error_at(&T[P], "expected '{' to start the body of '%s', found %s", f->name, desc(&T[P]));
    f->c = block();
    return f;
}

/* field: value, ...  up to and including the closing '}' */
static void field_inits(Node *into, const char *what) {
    while (!at("}")) {
        Token *fname = ident("field name");
        expect(":", strf("':' and a value after field '%s'", fname->s));
        Node *f = mk(N_FIELD, fname);
        f->name = fname->s;
        f->a = expr();
        add(into, f);
        if (!accept(",")) break;
    }
    expect("}", strf("'}' to close the %s", what));
}

static Node *method_in(Token *owner) {
    Node *m = fn_sig();
    if (m->a) error_at(m->tok, "methods inside a type don't name a receiver: write 'fn %s(...)'", m->name);
    Node *rt = mk(N_TNAME, owner);
    rt->name = owner->s;
    m->a = rt;
    m->name2 = owner->s;
    return fn_body(m);
}

/* enum Color: u8 { Red = 1, Green }    enum Shape { Circle(f32), Rect { w: f32, h: f32 }, Empty } */
static Node *enum_decl(void) {
    expect("enum", NULL);
    Token *name = ident("enum name");
    Node *e = mk(N_ENUM, name);
    e->name = name->s;
    if (accept(":")) e->b = type_();
    const char *saved = cur_struct;
    cur_struct = name->s;
    expect("{", NULL);
    while (!at("}")) {
        if (T[P].k == TK_EOF) error_at(name, "unclosed body of enum '%s'", name->s);
        bool pub = accept("pub") != NULL;
        if (at_fn()) {
            add(e, method_in(name));
            e->list[e->n - 1]->is_pub = pub;
            continue;
        }
        if (pub) error_at(&T[P], "variants are always visible; 'pub' goes on the enum");
        Token *vn = ident("a variant name or 'fn'");
        Node *v = mk(N_VARIANT, vn);
        v->name = vn->s;
        if (accept("(")) {
            v->flag = 1;
            while (!at(")")) {
                add(v, type_());
                if (!accept(",")) break;
            }
            expect(")", "')' to close the payload types");
            if (!v->n) error_at(vn, "empty payload; write just '%s'", vn->s);
        } else if (accept("{")) {
            v->flag = 2;
            while (!at("}")) {
                Token *fname = ident("field name");
                expect(":", "':' and a type after the field name");
                Node *f = mk(N_FIELD, fname);
                f->name = fname->s;
                f->a = type_();
                add(v, f);
                if (!accept(",")) break;
            }
            expect("}", "'}' to close the payload fields");
        }
        if (accept("=")) v->c = expr();
        add(e, v);
        if (!accept(",") && !at("}") && !at_fn() && !at("pub"))
            error_at(&T[P], "expected ',' after variant '%s', found %s", v->name, desc(&T[P]));
    }
    expect("}", NULL);
    cur_struct = saved;
    return e;
}

/* the fields and methods of a struct, from '{' to '}'. `owner` names the type inside (Self for an anonymous struct) */
static void struct_members(Node *s, Token *name, const char *kw) {
    const char *saved = cur_struct;
    cur_struct = name->s;
    expect("{", NULL);
    while (!at("}")) {
        if (T[P].k == TK_EOF) error_at(name, "unclosed body of %s '%s'", kw, name->s);
        bool pub = accept("pub") != NULL;
        if (at_fn()) {
            add(s, method_in(name));
            s->list[s->n - 1]->is_pub = pub;
            continue;
        }
        if (at("struct") || at("enum") || (at("packed") && at_n(1, "struct"))) { /* a type inside a type: Outer.Inner */
            bool pk = accept("packed") != NULL;
            bool en = at("enum");
            next();
            Token *nm = ident(en ? "enum name" : "struct name");
            Token *q = xalloc(sizeof *q);
            *q = *nm;
            q->s = strf("%s.%s", name->s, nm->s);
            P--; /* let the declaration read the name again, then rename it */
            T[P].s = q->s;
            P--;
            Node *inner = en ? enum_decl() : struct_decl();
            inner->flag = pk;
            inner->is_pub = pub;
            add(s, inner);
            continue;
        }
        if (at("let") || at("var")) { /* a constant or variable that belongs to the type: Type.name */
            Node *l = let_stmt();
            semi("the declaration");
            l->is_pub = pub;
            add(s, l);
            continue;
        }
        if (pub) error_at(&T[P], "fields are always visible; 'pub' is for functions, constants and types");
        Token *fname = ident("a field name, 'fn', 'let' or 'var'");
        expect(":", "':' and a type after the field name");
        Node *f = mk(N_FIELD, fname);
        f->name = fname->s;
        f->a = type_();
        if (accept("=")) f->b = expr();
        add(s, f);
        if (!accept(",") && !at("}") && !at_fn() && !at("pub"))
            error_at(&T[P], "expected ',' after field '%s', found %s", f->name, desc(&T[P]));
    }
    expect("}", NULL);
    cur_struct = saved;
}

/* struct Name { ... }, or (kw = "union") the body of an extern union */
static Node *struct_decl_kw(const char *kw) {
    expect(kw, NULL);
    Token *name = ident(strf("%s name", kw));
    Node *s = mk(N_STRUCT, name);
    s->name = name->s;
    struct_members(s, name, kw);
    return s;
}

static Node *struct_decl(void) { return struct_decl_kw("struct"); }

/* ---------- statements ---------- */

static Node *let_stmt(void) {
    Token *kw = next(); /* let | var */
    if (at("(")) { /* let (q, r) = divmod(a, b); */
        Token *lp = next();
        Node *n = mk(N_LET, lp);
        while (!at(")")) {
            Token *v = ident("a variable name");
            Node *vn = mk(N_NAME, v);
            vn->name = v->s;
            add(n, vn);
            if (!accept(",")) break;
        }
        expect(")", "')' to close the names");
        if (n->n < 2) error_at(lp, "destructuring needs at least two names: let (a, b) = ...;");
        n->name = n->list[0]->name;
        n->flag = !strcmp(kw->s, "let");
        if (accept(":")) n->a = type_();
        if (!accept("=")) error_at(&T[P], "expected '=' and a tuple value");
        n->b = expr();
        return n;
    }
    Token *name = ident("variable name");
    Node *n = mk(N_LET, name);
    n->name = name->s;
    n->flag = !strcmp(kw->s, "let");
    if (accept(":")) n->a = type_();
    if (!accept("="))
        error_at(&T[P], "'%s' needs a value: every variable must be initialized%s", name->s,
                 n->flag ? "" : " (write '= undefined' to fill it in later)");
    n->b = expr();
    return n;
}

static Node *paren_cond(const char *kw) {
    if (!at("(")) error_at(&T[P], "expected '(' after '%s': conditions are written %s (cond) { ... }", kw, kw);
    next();
    Node *e = expr();
    expect(")", "')' to close the condition");
    return e;
}

/* optional |name| after a condition: unwraps an optional. |*name| points at the value
 * inside it instead (only where by_ptr is given). */
static const char *capture_p(int *by_ptr) {
    if (!accept("|")) return NULL;
    if (at("*")) {
        if (!by_ptr) error_at(&T[P], "an error can't be captured by pointer; write |err|");
        next();
        *by_ptr = 1;
    }
    const char *name = ident("a capture name")->s;
    expect("|", "'|' to close the capture");
    return name;
}

static const char *capture(void) { return capture_p(NULL); }

static Node *body_block(const char *kw) {
    if (!at("{")) error_at(&T[P], "expected '{': the body of '%s' must be a block in braces", kw);
    return block();
}

static Node *if_rest(Token *t) {
    Node *n = mk(N_IF, t);
    n->a = paren_cond("if");
    n->name = capture_p(&n->flag2);
    n->b = body_block("if");
    if (accept("else")) {
        n->name2 = capture();
        if (n->name2 && at("if")) error_at(&T[P], "'else |%s|' must be followed by a block", n->name2);
        if (at("if")) {
            Token *it = next();
            Node *b = mk(N_BLOCK, it);
            add(b, if_rest(it));
            n->c = b;
        } else {
            n->c = body_block("else");
        }
    }
    return n;
}

/* for (items) |item, i| { }   for (items) |*item| { }   for (a..b) |i| { } */
static Node *for_rest(Token *t, const char *label) {
    if (!at("(")) error_at(&T[P], "expected '(' after 'for': write for (items) |item| { ... } or for (0..n) |i| { ... }");
    next();
    Node *start = expr();
    if (at("..") || at("..=")) {
        bool incl = !strcmp(next()->s, "..=");
        Node *end = expr();
        expect(")", "')' after the range");
        expect("|", "a capture like |i| after the range");
        if (at("*")) error_at(&T[P], "a range loop variable can't be captured by pointer");
        Token *v = ident("loop variable");
        if (at(",")) error_at(&T[P], "a range loop takes a single variable");
        expect("|", "'|' to close the capture");
        Node *n = mk(N_FORRANGE, t);
        n->name = v->s;
        n->a = start;
        n->b = end;
        n->flag = incl;
        n->label = label;
        n->c = body_block("for");
        return n;
    }
    expect(")", "')' after the sequence");
    expect("|", "a capture like |item| or |item, i| after the sequence");
    bool by_ptr = accept("*") != NULL;
    Token *v = ident("loop variable");
    Token *i = accept(",") ? ident("index variable") : NULL;
    expect("|", "'|' to close the capture");
    Node *n = mk(N_FOREACH, t);
    n->name = v->s;
    n->name2 = i ? i->s : NULL;
    n->flag = by_ptr;
    n->a = start;
    n->label = label;
    n->c = body_block("for");
    return n;
}

/* .Tag  .Tag(a, b)  value  lo..=hi  else */
static Node *pattern(void) {
    Token *t = &T[P];
    Node *p = mk(N_PATTERN, t);
    if (accept("else")) {
        p->flag = 3;
        return p;
    }
    if (at(".") && peek(1)->k == TK_ID) {
        next();
        p->flag = 2;
        p->name = ident("variant name")->s;
        if (accept("(")) {
            while (!at(")")) {
                Token *c = ident("a capture name");
                Node *cn = mk(N_NAME, c);
                cn->name = c->s;
                add(p, cn);
                if (!accept(",")) break;
            }
            expect(")", "')' to close the captures");
        }
        return p;
    }
    p->a = expr();
    if (accept("..=")) {
        p->flag = 1;
        p->b = expr();
    } else if (at("..")) {
        error_at(&T[P], "case ranges are inclusive: write 'lo..=hi'");
    }
    return p;
}

static Node *simple_stmt(void);

/* a switch arm body without braces: an assignment, a call, or return/break/continue */
static Node *arm_stmt(void) {
    Token *t = &T[P];
    if (accept("return")) {
        Node *s = mk(N_RETURN, t);
        if (!at(",") && !at("}")) s->a = expr();
        return s;
    }
    if (at("break") || at("continue")) {
        bool brk = at("break");
        next();
        Node *s = mk(brk ? N_BREAK : N_CONTINUE, t);
        if (accept(":")) s->label = ident("a loop label")->s;
        return s;
    }
    return simple_stmt();
}

static Node *switch_rest(Token *t, bool is_expr) {
    Node *n = mk(N_SWITCH, t);
    n->flag = is_expr;
    if (!at("(")) error_at(&T[P], "expected '(' after 'switch': write switch (value) { ... }");
    next();
    n->a = expr();
    expect(")", "')' after the switch value");
    expect("{", "'{' to start the switch cases");
    while (!at("}")) {
        if (T[P].k == TK_EOF) error_at(t, "unclosed switch");
        Node *arm = mk(N_ARM, &T[P]);
        add(arm, pattern());
        while (accept(",")) add(arm, pattern());
        expect("=>", "'=>' after the case pattern");
        if (is_expr) {
            arm->a = expr();
        } else if (at("{")) {
            arm->a = block();
            accept(",");
            add(n, arm);
            continue;
        } else {
            arm->a = arm_stmt();
        }
        if (!accept(",") && !at("}")) error_at(&T[P], "expected ',' after the case, found %s", desc(&T[P]));
        add(n, arm);
    }
    expect("}", NULL);
    if (!n->n) error_at(t, "a switch needs at least one case");
    return n;
}

static Node *block(void) {
    Token *t = expect("{", NULL);
    Node *b = mk(N_BLOCK, t);
    while (!at("}")) {
        if (T[P].k == TK_EOF) error_at(t, "unclosed '{'");
        add(b, stmt());
    }
    expect("}", NULL);
    return b;
}

static const char *ASSIGN_OPS[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
                                   "+%=", "-%=", "*%=", "+|=", "-|=", "*|=", NULL};

static Node *simple_stmt(void) {
    Token *t = &T[P];
    if (t->k == TK_ID && !strcmp(t->s, "_") && at_n(1, "=")) {
        next();
        next();
        Node *d = mk(N_DISCARD, t);
        d->a = expr();
        return d;
    }
    if (at("++") || at("--")) error_at(t, "Flint has no '%s'; write 'x %s= 1;'", t->s, t->s[0] == '+' ? "+" : "-");
    Node *e = expr();
    for (const char **op = ASSIGN_OPS; *op; op++) {
        if (at(*op)) {
            Token *ot = next();
            Node *n = mk(N_ASSIGN, ot);
            n->name = ot->s;
            n->a = e;
            n->b = expr();
            return n;
        }
    }
    if (at("++") || at("--"))
        error_at(&T[P], "Flint has no '%s'; write 'x %s= 1;'", T[P].s, T[P].s[0] == '+' ? "+" : "-");
    Node *n = mk(N_EXPRSTMT, t);
    n->a = e;
    return n;
}

static Node *stmt(void) {
    Token *t = &T[P];
    const char *label = NULL;
    if (t->k == TK_ID && at_n(1, ":") && (at_n(2, "for") || at_n(2, "while"))) {
        label = t->s;
        next();
        next();
        t = &T[P];
    }
    if (accept("while")) {
        Node *n = mk(N_WHILE, t);
        n->a = paren_cond("while");
        n->name = capture_p(&n->flag2);
        n->label = label;
        n->b = body_block("while");
        return n;
    }
    if (accept("for")) return for_rest(t, label);
    if (accept("switch")) return switch_rest(t, false);
    if (accept("fallthrough")) {
        Node *s = mk(N_FALLTHROUGH, t);
        semi("fallthrough");
        return s;
    }
    if (at("let") || at("var")) {
        Node *s = let_stmt();
        semi("the declaration");
        return s;
    }
    if (at("{")) return block();
    if (at("unsafe")) { /* unsafe { ... }: the only place unchecked operations are allowed */
        next();
        if (!at("{")) error_at(&T[P], "expected '{' after 'unsafe', found %s", desc(&T[P]));
        Node *b = block();
        b->is_unsafe = true;
        return b;
    }
    if (at("comptime") && at_n(1, "{")) { /* comptime { ... }: runs while compiling */
        next();
        Node *n = mk(N_COMPTIME, t);
        n->flag = 1;
        n->a = block();
        return n;
    }
    if (accept("if")) return if_rest(t);
    if (accept("return")) {
        Node *s = mk(N_RETURN, t);
        if (!at(";")) s->a = expr();
        semi("return");
        return s;
    }
    if (at("break") || at("continue")) {
        bool brk = at("break");
        next();
        Node *s = mk(brk ? N_BREAK : N_CONTINUE, t);
        if (accept(":")) s->label = ident("a loop label")->s;
        semi(brk ? "break" : "continue");
        return s;
    }
    if (at("errdefer")) { /* runs only when the function returns an error */
        next();
        Node *s = mk(N_DEFER, t);
        s->flag = 1;
        s->name = capture();
        if (at("{")) {
            s->a = block();
            return s;
        }
        s->a = simple_stmt();
        semi("the deferred statement");
        return s;
    }
    if (accept("defer")) {
        Node *s = mk(N_DEFER, t);
        if (at("{")) {
            s->a = block();
            return s;
        }
        s->a = simple_stmt();
        semi("the deferred statement");
        return s;
    }
    Node *s = simple_stmt();
    semi(s->k == N_ASSIGN ? "the assignment" : "the statement");
    return s;
}

/* ---------- expressions ---------- */

/* lowest to highest: || && comparison | ^ & shifts additive multiplicative */
static int binprec(Token *t) {
    if (t->k != TK_OP) return -1;
    static const struct { const char *op; int p; } tab[] = {
        {"||", 1}, {"&&", 2},
        {"==", 3}, {"!=", 3}, {"<", 3}, {"<=", 3}, {">", 3}, {">=", 3},
        {"|", 4}, {"^", 5}, {"&", 6}, {"<<", 7}, {">>", 7},
        {"+", 8}, {"-", 8}, {"+%", 8}, {"-%", 8}, {"+|", 8}, {"-|", 8},
        {"*", 9}, {"/", 9}, {"%", 9}, {"*%", 9}, {"*|", 9},
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (!strcmp(t->s, tab[i].op)) return tab[i].p;
    return -1;
}

static Node *unary(void);

static Node *cast_expr(void) {
    Node *e = unary();
    while (at("as")) {
        Token *t = next();
        Node *c = mk(N_CAST, t);
        c->a = e;
        c->b = type_();
        e = c;
    }
    return e;
}

static Node *binary(int minp) {
    Node *lhs = cast_expr();
    for (;;) {
        Token *t = &T[P];
        int p = binprec(t);
        if (p < 0 || p < minp) return lhs;
        next();
        Node *n = mk(N_BINARY, t);
        n->name = t->s;
        n->a = lhs;
        n->b = binary(p + 1);
        if (p == 3 && binprec(&T[P]) == 3)
            error_at(&T[P], "comparisons can't be chained; write 'a < b && b < c'");
        lhs = n;
    }
}

static Node *expr(void) {
    if (at("if")) { /* if (cond) a else b */
        Token *t = next();
        Node *n = mk(N_TERNARY, t);
        n->a = paren_cond("if");
        n->name = capture_p(&n->flag2);
        n->b = expr();
        if (!accept("else")) error_at(&T[P], "an 'if' expression needs an 'else' branch");
        n->name2 = capture();
        n->c = expr();
        return n;
    }
    Node *e = binary(1);
    while (at("orelse") || at("catch")) { /* lowest precedence: a orelse b orelse c */
        Token *t = next();
        Node *o = mk(!strcmp(t->s, "catch") ? N_CATCH : N_ORELSE, t);
        o->a = e;
        if (o->k == N_CATCH) o->name = capture();
        if (at("{")) { /* f() catch |e| { ... }   x orelse { ... } */
            o->b = block();
            e = o;
            continue;
        }
        o->b = binary(1);
        e = o;
    }
    if (at("?")) error_at(&T[P], "Flint has no '?:' operator; write 'if (cond) a else b'");
    return e;
}

static Node *primary(void);
static Node *struct_lit(Token *t, const char *name);

/* can a type start at t? (used to tell [N]T { ... } from the old [a, b] literal) */
static bool starts_type(Token *t) {
    if (t->k == TK_ID) return t->s[0] != '@';
    return is_op(t, "[") || is_op(t, "*") || is_op(t, "?") || is_op(t, "fn") || is_op(t, "!");
}

static Node *postfix(void) {
    Node *e = primary();
    for (;;) {
        Token *t = &T[P];
        if (accept("(")) {
            Node *c = mk(N_CALL, t);
            c->a = e;
            while (!at(")")) {
                add(c, expr());
                if (!accept(",")) break;
            }
            expect(")", "')' or ',' in the argument list");
            e = c;
            if (!no_lit && at("{") && ((e->a->k == N_NAME && is_type_fn_name(e->a->name)) ||
                            (e->a->k == N_FIELDACC && is_alias_node(e->a->a) && is_type_fn_name(e->a->name)))) { /* List(i32) { ... } */
                Node *s = mk(N_STRUCTLIT, t);
                s->a = e;
                next();
                field_inits(s, "struct literal");
                e = s;
            }
        } else if (accept("[")) {
            Node *lo = at("..") ? NULL : expr();
            if (accept("..")) {
                Node *s = mk(N_SLICE, t);
                s->a = e;
                s->b = lo;
                s->c = at("]") ? NULL : expr();
                e = s;
            } else {
                Node *ix = mk(N_INDEX, t);
                ix->a = e;
                ix->b = lo;
                e = ix;
            }
            expect("]", NULL);
        } else if (accept(".?")) {
            Node *u = mk(N_UNWRAP, t);
            u->a = e;
            e = u;
        } else if (accept(".*")) {
            Node *d = mk(N_DEREF, t);
            d->a = e;
            e = d;
        } else if (accept(".")) {
            if (T[P].k == TK_INT) { /* t.0: a tuple element */
                Token *it = next();
                Node *f = mk(N_FIELDACC, it);
                f->a = e;
                f->name = strf("%llu", (unsigned long long)it->ival);
                e = f;
                continue;
            }
            Token *name = ident("a field or method name");
            if (!no_lit && at("{") && e->k == N_CALL && ((e->a->k == N_NAME && is_type_fn_name(e->a->name)) ||
                                             (e->a->k == N_FIELDACC && is_alias_node(e->a->a) && is_type_fn_name(e->a->name)))) {
                Node *f = mk(N_FIELDACC, name); /* Tree(i32).Node { ... } */
                f->a = e;
                f->name = name->s;
                Node *sl = mk(N_STRUCTLIT, name);
                sl->a = f;
                next();
                field_inits(sl, "struct literal");
                e = sl;
                continue;
            }
            bool qual_enum = e->k == N_FIELDACC && is_alias_node(e->a) && is_enum_name(e->name); /* geo.Shape.Rect { } */
            if (!no_lit && ((e->k == N_NAME && is_enum_name(e->name)) || qual_enum) && at("{")) { /* Shape.Rect { w: 1, h: 2 } */
                next();
                Node *d = mk(N_DOTTAG, name);
                d->name = name->s;
                d->name2 = qual_enum ? strf("%s.%s", e->a->name, e->name) : e->name;
                d->flag = 2;
                field_inits(d, "variant literal");
                e = d;
                continue;
            }
            if (!no_lit && e->k == N_NAME && at("{") &&
                (((is_struct_name(e->name) || is_import_alias(e->name)) && is_struct_name(name->s)) || is_c_alias(e->name))) {
                /* Outer.Inner { ... }, geo.Point { ... }, c.struct_tm { ... } */
                e = struct_lit(name, strf("%s.%s", e->name, name->s));
                continue;
            }
            Node *f = mk(N_FIELDACC, name);
            f->a = e;
            f->name = name->s;
            e = f;
        } else {
            return e;
        }
    }
}

static Node *unary(void) {
    Token *t = &T[P];
    if (t->k == TK_OP && (!strcmp(t->s, "-") || !strcmp(t->s, "!") || !strcmp(t->s, "~") || !strcmp(t->s, "&"))) {
        next();
        Node *n = mk(N_UNARY, t);
        n->name = t->s;
        n->a = unary();
        return n;
    }
    if (accept("try")) {
        Node *n = mk(N_TRY, t);
        n->a = unary();
        return n;
    }
    if (accept("comptime")) { /* comptime f(x) + 1: the whole expression after it runs at compile time */
        Node *n = mk(N_COMPTIME, t);
        if (at("{")) error_at(t, "a comptime block is a statement: 'comptime { ... }' on its own line");
        n->a = expr();
        return n;
    }
    if (at("++") || at("--")) error_at(t, "Flint has no '%s'; write 'x %s= 1;'", t->s, t->s[0] == '+' ? "+" : "-");
    return postfix();
}

static Node *struct_lit(Token *t, const char *name) {
    expect("{", NULL);
    Node *s = mk(N_STRUCTLIT, t);
    s->name = name;
    field_inits(s, "struct literal");
    return s;
}

static Node *builtin_call(Token *t) {
    static const char *with_values[] = {"@tagName", "@errorName", "@splat", "@addWithOverflow", "@subWithOverflow",
                                        "@mulWithOverflow", "@ptrToInt", "@assume", "@format", "@hash", "@eql",
                                        "@reduceAdd", "@compileError", "@embedFile", NULL};
    for (const char **b = with_values; *b; b++) { /* these take only values */
        if (strcmp(*b, t->s)) continue;
        Node *n = mk(N_BUILTIN, t);
        n->name = t->s;
        expect("(", strf("'(' after %s", t->s));
        add(n, expr());
        while (accept(",")) add(n, expr());
        expect(")", NULL);
        return n;
    }
    static const char *with_type[] = {"@sizeOf", "@alignOf", "@zeroes", "@truncate", "@bitcast", "@ptrCast", "@intToPtr",
                                      "@minInt", "@maxInt", "@typeName", "@isInt", "@isFloat", "@isSigned",
                                      "@hasField", "@hasMethod", NULL};
    bool known = false;
    for (const char **b = with_type; *b; b++)
        if (!strcmp(*b, t->s)) known = true;
    if (!known)
        error_at(t, "unknown builtin '%s' (available: @sizeOf, @alignOf, @minInt, @maxInt, @zeroes, @truncate, @bitcast, @ptrCast, @intToPtr, "
                    "@ptrToInt, @splat, @tagName, @errorName, @addWithOverflow, @subWithOverflow, @mulWithOverflow, @assume, "
                    "@TypeOf, @format, @hash, @eql, @typeName, @isInt, @isFloat, @isSigned, @hasField, @hasMethod, "
                    "@compileError, @Vector, @reduceAdd, @embedFile)", t->s);
    Node *n = mk(N_BUILTIN, t);
    n->name = t->s;
    expect("(", strf("'(' after %s", t->s));
    n->b = type_();
    while (accept(",")) add(n, expr());
    expect(")", NULL);
    return n;
}

static bool starts_type(Token *t);

static Node *primary(void) {
    Token *t = &T[P];
    switch (t->k) {
    case TK_INT: {
        next();
        Node *n = mk(N_INT, t);
        n->ival = t->ival;
        return n;
    }
    case TK_FLOAT: {
        next();
        Node *n = mk(N_FLOAT, t);
        n->fval = t->fval;
        return n;
    }
    case TK_STR: {
        next();
        Node *n = mk(N_STR, t);
        n->str = t->str;
        n->slen = t->slen;
        return n;
    }
    default: break;
    }
    if (accept("true") || accept("false")) {
        Node *n = mk(N_BOOL, t);
        n->ival = !strcmp(t->s, "true");
        return n;
    }
    if (accept("null")) return mk(N_NULL, t);
    if (accept("unreachable")) return mk(N_UNREACHABLE, t);
    if (accept("error")) {
        expect(".", "'.' and an error name after 'error' (like error.NotFound)");
        Node *e = mk(N_ERRLIT, &T[P]);
        e->name = ident("an error name")->s;
        return e;
    }
    /* control flow as an expression (type noreturn), as in `x orelse return 0` */
    if (accept("return")) {
        Node *r = mk(N_RETURN, t);
        if (!at(";") && !at(",") && !at(")") && !at("}") && !at("]")) r->a = expr();
        return r;
    }
    if (at("break") || at("continue")) {
        bool brk = at("break");
        next();
        Node *r = mk(brk ? N_BREAK : N_CONTINUE, t);
        if (accept(":")) r->label = ident("a loop label")->s;
        return r;
    }
    if (accept("switch")) return switch_rest(t, true);
    if (at(".") && peek(1)->k == TK_ID) { /* .Red  .Circle(2.0)  .Rect { w: 1, h: 2 } */
        next();
        Token *name = ident("variant name");
        Node *d = mk(N_DOTTAG, name);
        d->name = name->s;
        if (accept("(")) {
            d->flag = 1;
            while (!at(")")) {
                add(d, expr());
                if (!accept(",")) break;
            }
            expect(")", "')' to close the payload");
        } else if (accept("{")) {
            d->flag = 2;
            field_inits(d, "variant literal");
        }
        return d;
    }
    if (at(".") && at_n(1, "{")) { /* .{ x: 1, y: 2 } or .{ 1, 2 }: the type comes from context */
        next();
        next();
        Node *a = mk(N_ANONLIT, t);
        a->flag = T[P].k == TK_ID && at_n(1, ":"); /* named fields */
        if (a->flag) {
            field_inits(a, "literal");
            return a;
        }
        while (!at("}")) {
            add(a, expr());
            if (!accept(",")) break;
        }
        expect("}", "'}' or ',' in the literal");
        return a;
    }
    if (accept("undefined")) return mk(N_UNDEFINED, t);
    if (at("struct") && at_n(1, "{")) { /* struct { ... }: the type a `-> type` function returns */
        next();
        Token *owner = xalloc(sizeof *owner);
        *owner = *t;
        owner->s = "Self";
        Node *s = mk(N_STRUCT, t);
        s->name = NULL;
        struct_members(s, owner, "struct");
        return s;
    }
    if (at("*") || at("?") || at("fn") || (T[P].k == TK_ID && (!strcmp(T[P].s, "@TypeOf") || !strcmp(T[P].s, "@Vector"))) ||
        (at("[") && ((at_n(1, "]") && (starts_type(peek(2)) || is_op(peek(2), "const"))) || (at_n(1, "*") && at_n(2, "]"))))) {
        /* a type as a value: alloc.create(*Node) */
        Node *n = mk(N_TYPEEXPR, t);
        n->a = type_();
        return n;
    }
    if (accept("(")) {
        Node *e = expr();
        expect(")", NULL);
        return e;
    }
    if (at("[")) { /* [N]T { a, b }  or  [_]T { a, b }: an array value on the stack */
        int save = P;
        next();
        Node *ty = mk(N_TARRAY, t);
        bool lit = false;
        if (T[P].k == TK_ID && !strcmp(T[P].s, "_") && at_n(1, "]")) {
            next();
            next();
            lit = true;
        } else if (!at("]")) {
            ty->c = expr();
            lit = at("]") && starts_type(peek(1));
            if (lit) next();
        }
        if (lit) {
            ty->a = type_();
            if (!at("{") && ty->c) { /* [N]T used as a type argument */
                Node *te = mk(N_TYPEEXPR, t);
                te->a = ty;
                return te;
            }
            Node *a = mk(N_ARRAYLIT, t);
            a->a = ty;
            expect("{", strf("'{' and the elements after the array type"));
            while (!at("}")) {
                add(a, expr());
                if (!accept(",")) break;
            }
            expect("}", "'}' or ',' in the array literal");
            return a;
        }
        P = save;
    }
    if (accept("[")) {
        Node *a = mk(N_ARRAY, t);
        while (!at("]")) {
            add(a, expr());
            if (!accept(",")) break;
        }
        expect("]", "']' or ',' in array literal");
        return a;
    }
    if (t->k == TK_ID) {
        next();
        if (t->s[0] == '@') return builtin_call(t);
        const char *name = !strcmp(t->s, "Self") ? self_name(t) : t->s;
        if (!no_lit && is_struct_name(t->s) && at("{")) return struct_lit(t, name);
        Node *n = mk(N_NAME, t);
        n->name = name;
        return n;
    }
    error_at(t, "expected an expression, found %s", desc(t));
}

/* ---------- top level ---------- */

/* import std.io;  import net.http as web;  import c "stdio.h" as c; */
static void import_decl(Unit *u) {
    Token *kw = next();
    Import im;
    memset(&im, 0, sizeof im);
    im.tok = kw;
    if (T[P].k == TK_ID && !strcmp(T[P].s, "c") && peek(1)->k == TK_STR) { /* a C header */
        next();
        Token *h = next();
        im.header = xstrndup(h->str, (size_t)h->slen);
        if (!accept("as")) error_at(&T[P], "a C header needs a name to reach it by: import c \"%s\" as c;", im.header);
        im.alias = ident("a name for the header")->s;
    } else {
        Token *first = ident("a module path, like std.io");
        im.tok = first;
        const char *path = first->s, *last = first->s;
        while (accept(".")) {
            last = ident("the next part of the module path")->s;
            path = strf("%s.%s", path, last);
        }
        im.mpath = path;
        im.alias = accept("as") ? ident("a name for the module")->s : last;
    }
    semi("the import");
    for (int i = 0; i < u->nimports; i++)
        if (!strcmp(u->imports[i].alias, im.alias))
            error_at(im.tok, "'%s' is already the name of another import; use 'as' to pick a different name", im.alias);
    PUSH(u->imports, u->nimports, u->cimports, im);
}

void parse_imports(Token *toks, Unit *u) {
    T = toks;
    P = 0;
    cur_unit = u;
    while (at("import")) import_decl(u);
}

void prescan(Token *toks) {
    T = toks;
    for (int i = 0; T[i].k != TK_EOF; i++) /* type names from every file, so `Name {` parses before Name's file is read */
        if (T[i].k == TK_KW && T[i + 1].k == TK_ID) {
            if (!strcmp(T[i].s, "struct") || !strcmp(T[i].s, "union")) PUSH(snames, nsnames, csnames, T[i + 1].s);
            if (!strcmp(T[i].s, "enum")) PUSH(enames, nenames, cenames, T[i + 1].s);
            if (!strcmp(T[i].s, "fn") && is_op(&T[i + 2], "(")) { /* fn Name(...) -> type */
                int d = 0, j = i + 2;
                for (; T[j].k != TK_EOF; j++) {
                    if (is_op(&T[j], "(")) d++;
                    else if (is_op(&T[j], ")") && --d == 0) break;
                }
                if (T[j].k != TK_EOF && is_op(&T[j + 1], "->") && T[j + 2].k == TK_ID && !strcmp(T[j + 2].s, "type"))
                    PUSH(tfnames, ntfnames, ctfnames, T[i + 1].s);
            }
        }
}

void parse(Token *toks, Unit *u) {
    T = toks;
    P = 0;
    cur_struct = NULL;
    cur_unit = u;
    while (at("import")) { /* already read by parse_imports */
        while (!at(";")) next();
        next();
    }
    Node **items = NULL;
    int n = 0, cap = 0;
    while (T[P].k != TK_EOF) {
        if (at("import")) error_at(&T[P], "imports go at the top of the file, before any declaration");
        Token *pub = accept("pub");
        Node *it;
        if (at("test")) {
            Token *t = next();
            if (pub) error_at(pub, "a test can't be 'pub'");
            if (T[P].k != TK_STR) error_at(&T[P], "expected the test's name as a string: test \"what it checks\" { ... }");
            Token *name = next();
            it = mk(N_TEST, t);
            it->str = xstrndup(name->str, (size_t)name->slen);
            it->slen = name->slen;
            if (!at("{")) error_at(&T[P], "expected '{' to start the test's body, found %s", desc(&T[P]));
            it->c = block();
            PUSH(items, n, cap, it);
            continue;
        }
        if (at("comptime")) { /* comptime { assert(@sizeOf(Header) == 16); } */
            Token *t = next();
            if (pub) error_at(pub, "a comptime block can't be 'pub'");
            if (!at("{")) error_at(&T[P], "expected '{' after 'comptime' at top level, found %s", desc(&T[P]));
            it = mk(N_COMPTIME, t);
            it->flag = 1;
            it->a = block();
            PUSH(items, n, cap, it);
            continue;
        }
        if (at_fn()) {
            it = fn_body(fn_sig());
        } else if (at("export")) {
            next();
            if (at("let") || at("var")) { /* export var counter: u64 = 0; */
                it = let_stmt();
                semi("the declaration");
                it->exported = true;
                it->is_pub = pub != NULL;
                PUSH(items, n, cap, it);
                continue;
            }
            it = fn_sig();
            if (it->a) error_at(it->tok, "methods can't be exported");
            it->exported = true;
            fn_body(it);
        } else if (at("extern")) {
            Token *t = next();
            if (T[P].k == TK_STR) {
                if (strcmp(T[P].str, "C")) error_at(&T[P], "only extern \"C\" is supported");
                next();
            }
            if (at("let") || at("var")) { /* extern var optind: i32;  a global defined in C */
                Token *kw = next();
                Token *name = ident("variable name");
                it = mk(N_LET, name);
                it->name = name->s;
                it->flag = !strcmp(kw->s, "let");
                it->is_extern = true;
                expect(":", "':' and a type: an extern variable needs its type");
                it->a = type_();
                if (at("=")) error_at(&T[P], "an extern variable is defined in C, so it has no value here");
                semi("the extern declaration");
                it->is_pub = pub != NULL;
                PUSH(items, n, cap, it);
                continue;
            }
            if (at("union")) { /* extern union Value { i: i64, f: f64 }: a C union */
                it = struct_decl_kw("union");
                it->flag = 2;
                it->is_pub = pub != NULL;
                PUSH(items, n, cap, it);
                continue;
            }
            if (!at_fn()) error_at(&T[P], "expected 'fn' or 'union' after 'extern', found %s", desc(&T[P]));
            Node *f = fn_sig();
            if (at("{")) error_at(&T[P], "an extern function has no body; end the declaration with ';'");
            semi("the extern declaration");
            f->is_pub = pub != NULL;
            it = mk(N_EXTERN, t);
            add(it, f);
        } else if (at("packed")) { /* packed struct Flags { ... }: bit fields in one integer */
            next();
            if (!at("struct")) error_at(&T[P], "expected 'struct' after 'packed', found %s", desc(&T[P]));
            it = struct_decl();
            it->flag = 1;
        } else if (at("union")) {
            error_at(&T[P], "unions exist only for C interop: write 'extern union'; use an enum with payloads for a tagged union");
        } else if (at("struct")) {
            it = struct_decl();
        } else if (at("enum")) {
            it = enum_decl();
        } else if (at("error") && peek(1)->k == TK_ID) {
            next();
            Token *name = ident("error set name");
            it = mk(N_ERRSET, name);
            it->name = name->s;
            expect("{", NULL);
            while (!at("}")) {
                Token *en = ident("an error name");
                Node *e = mk(N_NAME, en);
                e->name = en->s;
                add(it, e);
                if (!accept(",")) break;
            }
            expect("}", "'}' to close the error set");
        } else if (at("let") || at("var")) {
            it = let_stmt();
            semi("the declaration");
        } else {
            error_at(&T[P], "expected 'fn', 'struct', 'enum', 'error', 'extern', 'export', 'let', 'var' or 'test' at top level, found %s",
                     desc(&T[P]));
        }
        it->is_pub = pub != NULL;
        PUSH(items, n, cap, it);
    }
    u->items = items;
    u->nitems = n;
}
