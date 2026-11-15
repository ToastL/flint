/* manifest.c - flint.toml, the package manifest (design.md section 4.16)
 *
 *   [package]
 *   name = "app"
 *   root = "src/main.fl"            # the default
 *
 *   [dependencies]
 *   geometry = { path = "../geometry" }
 *
 *   [c]                             # C code in the package, built with the C compiler
 *   sources = ["c/fast.c"]
 *   include = ["c"]
 *   flags = ["-O2"]
 *   link = ["m"]                    # -lm
 *
 * The format is a small subset of TOML: [tables], key = value, where a value is a "string",
 * an integer, true/false, an array of strings, or an inline table of strings; # starts a comment. */
#include "flint.h"
#include <ctype.h>
#include <limits.h>
#include <unistd.h>

typedef struct {
    const char *file, *p, *line_start;
    int line;
} TP;

static _Noreturn void bad(TP *t, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s:%d: error: %s\n", t->file, t->line, msg);
    exit(1);
}

static void skip_ws(TP *t) {
    while (*t->p == ' ' || *t->p == '\t' || *t->p == '\r') t->p++;
    if (*t->p == '#')
        while (*t->p && *t->p != '\n') t->p++;
}

/* whitespace, comments and newlines (inside arrays and inline tables) */
static void skip_all(TP *t) {
    for (;;) {
        skip_ws(t);
        if (*t->p != '\n') return;
        t->p++;
        t->line++;
    }
}

static const char *key(TP *t) {
    const char *s = t->p;
    while (*t->p && (isalnum((unsigned char)*t->p) || *t->p == '_' || *t->p == '-')) t->p++;
    if (t->p == s) bad(t, "expected a key");
    return xstrndup(s, (size_t)(t->p - s));
}

static const char *string(TP *t) {
    if (*t->p != '"') bad(t, "expected a \"string\"");
    t->p++;
    char *out = xalloc(strlen(t->p) + 1);
    int n = 0;
    while (*t->p != '"') {
        if (!*t->p || *t->p == '\n') bad(t, "unterminated string");
        if (*t->p == '\\') {
            t->p++;
            char c = *t->p;
            out[n++] = c == 'n' ? '\n' : c == 't' ? '\t' : c;
            t->p++;
            continue;
        }
        out[n++] = *t->p++;
    }
    t->p++;
    return out;
}

typedef enum { V_STR, V_INT, V_BOOL, V_ARR, V_TABLE } VKind;
typedef struct Value Value;
struct Value {
    VKind k;
    const char *s;
    int64_t i;
    const char **arr; /* V_ARR: strings */
    int n, cap;
    const char **keys; /* V_TABLE: key = "string" pairs */
    const char **vals;
    int nk, ck, nv, cv;
    int line;
};

static Value value(TP *t) {
    Value v;
    memset(&v, 0, sizeof v);
    v.line = t->line;
    if (*t->p == '"') {
        v.k = V_STR;
        v.s = string(t);
    } else if (*t->p == '[') {
        v.k = V_ARR;
        t->p++;
        skip_all(t);
        while (*t->p != ']') {
            PUSH(v.arr, v.n, v.cap, string(t));
            skip_all(t);
            if (*t->p == ',') {
                t->p++;
                skip_all(t);
            } else if (*t->p != ']') bad(t, "expected ',' or ']' in the array");
        }
        t->p++;
    } else if (*t->p == '{') {
        v.k = V_TABLE;
        t->p++;
        skip_ws(t);
        while (*t->p != '}') {
            const char *k = key(t);
            skip_ws(t);
            if (*t->p != '=') bad(t, "expected '=' after '%s'", k);
            t->p++;
            skip_ws(t);
            PUSH(v.keys, v.nk, v.ck, k);
            PUSH(v.vals, v.nv, v.cv, string(t));
            skip_ws(t);
            if (*t->p == ',') {
                t->p++;
                skip_ws(t);
            } else if (*t->p != '}') bad(t, "expected ',' or '}' in the inline table");
        }
        t->p++;
    } else if (!strncmp(t->p, "true", 4) || !strncmp(t->p, "false", 5)) {
        v.k = V_BOOL;
        v.i = *t->p == 't';
        t->p += v.i ? 4 : 5;
    } else if (isdigit((unsigned char)*t->p) || *t->p == '-') {
        v.k = V_INT;
        v.i = strtoll(t->p, (char **)&t->p, 10);
    } else {
        bad(t, "expected a value: a \"string\", a number, true/false, [array] or { table }");
    }
    return v;
}

static const char *table_get(Value *v, const char *k) {
    for (int i = 0; i < v->nk; i++)
        if (!strcmp(v->keys[i], k)) return v->vals[i];
    return NULL;
}

static const char *join_path(const char *dir, const char *p) {
    if (p[0] == '/') return strf("%s", p);
    return strf("%s/%s", dir, p);
}

static const char *real(const char *p) {
    char buf[PATH_MAX];
    return realpath(p, buf) ? strf("%s", buf) : strf("%s", p);
}

static Package **loaded;
static int nloaded, cloaded;

static const char **str_list(TP *t, Value *v, const char *what, int *n) {
    if (v->k != V_ARR) bad(t, "'%s' must be an array of strings", what);
    *n = v->n;
    return v->arr;
}

Package *read_manifest(const char *dir0) {
    const char *dir = real(dir0);
    for (int i = 0; i < nloaded; i++)
        if (!strcmp(loaded[i]->dir, dir)) return loaded[i];
    const char *file = strf("%s/flint.toml", dir);
    char *src = read_file(file);
    if (!src) {
        fprintf(stderr, "flint: can't read %s\n", file);
        exit(1);
    }
    Package *pkg = xalloc(sizeof *pkg);
    pkg->dir = dir;
    PUSH(loaded, nloaded, cloaded, pkg);
    TP t = {file, src, src, 1};
    const char *table = "";
    const char *root = NULL;
    for (;;) {
        skip_all(&t);
        if (!*t.p) break;
        if (*t.p == '[') {
            t.p++;
            table = key(&t);
            if (*t.p != ']') bad(&t, "expected ']' after the table name");
            t.p++;
            if (strcmp(table, "package") && strcmp(table, "dependencies") && strcmp(table, "c"))
                bad(&t, "unknown table [%s] (flint.toml has [package], [dependencies] and [c])", table);
            continue;
        }
        const char *k = key(&t);
        skip_ws(&t);
        if (*t.p != '=') bad(&t, "expected '=' after '%s'", k);
        t.p++;
        skip_ws(&t);
        Value v = value(&t);
        skip_ws(&t);
        if (*t.p && *t.p != '\n') bad(&t, "expected the end of the line after the value of '%s'", k);
        if (!strcmp(table, "package")) {
            if (!strcmp(k, "name")) {
                if (v.k != V_STR) bad(&t, "'name' must be a string");
                pkg->name = v.s;
            } else if (!strcmp(k, "root")) {
                if (v.k != V_STR) bad(&t, "'root' must be a string");
                root = v.s;
            } else if (!strcmp(k, "version") || !strcmp(k, "description")) {
                /* informational */
            } else bad(&t, "unknown key '%s' in [package] (name, root, version, description)", k);
        } else if (!strcmp(table, "dependencies")) {
            if (v.k != V_TABLE) bad(&t, "a dependency is an inline table: %s = { path = \"../%s\" }", k, k);
            if (table_get(&v, "url") || table_get(&v, "hash"))
                bad(&t, "dependencies fetched by url and hash aren't supported yet; use a local copy: %s = { path = \"...\" }", k);
            const char *p = table_get(&v, "path");
            if (!p) bad(&t, "dependency '%s' needs a path: %s = { path = \"../%s\" }", k, k, k);
            for (int i = 0; i < pkg->ndeps; i++)
                if (!strcmp(pkg->deps[i].name, k)) bad(&t, "dependency '%s' is listed twice", k);
            if (!strcmp(k, "std")) bad(&t, "'std' is the standard library's name; pick another name for this dependency");
            const char *ddir = join_path(dir, p);
            char probe[PATH_MAX];
            if (!realpath(ddir, probe) || access(strf("%s/flint.toml", probe), R_OK) != 0)
                bad(&t, "dependency '%s': no flint.toml in %s", k, ddir);
            Dep d = {k, read_manifest(ddir)};
            PUSH(pkg->deps, pkg->ndeps, pkg->cdeps, d);
        } else if (!strcmp(table, "c")) {
            int n;
            const char **list = str_list(&t, &v, k, &n);
            const char **abs = xalloc(sizeof(char *) * (size_t)(n + 1));
            bool paths = !strcmp(k, "sources") || !strcmp(k, "include");
            for (int i = 0; i < n; i++) abs[i] = paths ? join_path(dir, list[i]) : list[i];
            if (!strcmp(k, "sources")) pkg->csrc = abs, pkg->ncsrc = n;
            else if (!strcmp(k, "include")) pkg->cinc = abs, pkg->ncinc = n;
            else if (!strcmp(k, "flags")) pkg->cflags = abs, pkg->ncflags = n;
            else if (!strcmp(k, "link")) pkg->clink = abs, pkg->nclink = n;
            else bad(&t, "unknown key '%s' in [c] (sources, include, flags, link)", k);
        } else {
            bad(&t, "'%s' must be inside a table, like [package]", k);
        }
    }
    if (!pkg->name) {
        t.line = 1;
        bad(&t, "the [package] table needs a name: name = \"...\"");
    }
    pkg->root = join_path(dir, root ? root : "src/main.fl");
    if (access(pkg->root, R_OK) != 0) {
        t.line = 1;
        bad(&t, "the package's root file %s doesn't exist (set root = \"...\" in [package])", pkg->root);
    }
    char *rd = strf("%s", pkg->root);
    char *slash = strrchr(rd, '/');
    if (slash) *slash = 0;
    pkg->rootdir = rd;
    return pkg;
}

/* the nearest directory at or above `start` that holds a flint.toml */
const char *find_manifest(const char *start) {
    char *d = strf("%s", start);
    for (;;) {
        if (access(strf("%s/flint.toml", d), R_OK) == 0) return d;
        char *slash = strrchr(d, '/');
        if (!slash || slash == d) return NULL;
        *slash = 0;
    }
}
