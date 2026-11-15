/* module.c - finding and reading modules: one file is one module (design.md section 4.10)
 *
 *   import std.io;      the standard library, compiled into the flint binary
 *   import geometry;    a dependency's root module (flint.toml [dependencies])
 *   import geometry.x;  a file in that dependency
 *   import net.http;    <package root>/net/http.fl
 *
 * Every file is read once. Imports may form cycles: declaration order doesn't matter anywhere. */
#include "flint.h"
#include "std_fl.h" /* the std directory, embedded by the Makefile */
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

const char *g_cc = "cc";

char *read_file(const char *path) {
    long n;
    return read_file_len(path, &n);
}

/* the file's bytes (plus a terminating 0, not counted in *len), or NULL */
char *read_file_len(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || S_ISDIR(st.st_mode)) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xalloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = n;
    return buf;
}

/* is there a std module with this name? (for "did you forget the import" hints) */
bool std_has(const char *name) {
    char *f = strf("%s.fl", name);
    for (int i = 0; STD_FILES[i].name; i++)
        if (!strcmp(STD_FILES[i].name, f)) return true;
    return false;
}

static Unit **units;
static Package *root_pkg;
static int nunits_, cunits;

static const char *real_path(const char *p) {
    char buf[PATH_MAX];
    return realpath(p, buf) ? strf("%s", buf) : strf("%s", p);
}

static Unit *find_unit(const char *file) {
    for (int i = 0; i < nunits_; i++)
        if (!strcmp(units[i]->file, file)) return units[i];
    return NULL;
}

static const char *prefix_of(const char *path) {
    char *s = strf("%s__", path);
    for (char *c = s; *c; c++)
        if (*c == '.' || *c == '-') *c = '_';
    return s;
}

/* lex a file and read its imports; the rest is parsed once every file is known */
static Unit *new_unit(const char *path, const char *file, const char *src, Package *pkg, bool is_std) {
    Unit *u = xalloc(sizeof *u);
    u->path = path;
    u->file = file;
    u->pkg = pkg;
    u->is_std = is_std;
    u->prefix = prefix_of(path);
    const char *last = strrchr(path, '.');
    u->dname = strf("%s.", last ? last + 1 : path);
    for (int i = 0; i < nunits_; i++)
        if (!strcmp(units[i]->prefix, u->prefix))
            error_at(NULL, "modules '%s' and '%s' would have the same symbol names; rename one of them", units[i]->path, path);
    u->toks = lex(src, is_std ? strf("<std>/%s", strrchr(file, '/') + 1) : file);
    parse_imports(u->toks, u);
    PUSH(units, nunits_, cunits, u);
    return u;
}

static Unit *std_unit(const char *name, Token *tok) {
    const char *file = strf("<std>/%s.fl", name);
    Unit *u = find_unit(file);
    if (u) return u;
    char *fname = strf("%s.fl", name);
    for (char *c = fname; *c; c++)
        if (*c == '.' && strcmp(c, ".fl")) *c = '/';
    for (int i = 0; STD_FILES[i].name; i++)
        if (!strcmp(STD_FILES[i].name, fname))
            return new_unit(strf("std.%s", name), file, xstrndup((const char *)STD_FILES[i].data, STD_FILES[i].len), NULL, true);
    if (tok) error_at(tok, "the standard library has no module 'std.%s'", name);
    error_at(NULL, "internal error: std.%s is missing", name);
}

static Unit *file_unit(const char *path, const char *file, Package *pkg, Token *tok, const char *looked) {
    const char *rp = real_path(file);
    Unit *u = find_unit(rp);
    if (u) return u;
    char *src = read_file(file);
    if (!src) {
        if (tok) error_at(tok, "can't find module '%s' (looked for %s)", path, looked);
        error_at(NULL, "can't read %s", file);
    }
    return new_unit(path, rp, src, pkg, false);
}

static const char *path_to_file(const char *dotted) {
    char *s = strf("%s.fl", dotted);
    for (char *c = s; c < s + strlen(dotted); c++)
        if (*c == '.') *c = '/';
    return s;
}

static Unit *resolve_import(Unit *from, Import *im) {
    const char *p = im->mpath;
    if (!strncmp(p, "std.", 4)) return std_unit(p + 4, im->tok);
    if (!strcmp(p, "std")) error_at(im->tok, "import a module of the standard library, like 'import std.io;'");
    if (from->is_std) error_at(im->tok, "internal error: std imports '%s'", p);
    Package *pkg = from->pkg;
    const char *dot = strchr(p, '.');
    size_t first = dot ? (size_t)(dot - p) : strlen(p);
    for (int i = 0; i < pkg->ndeps; i++) {
        Dep *d = &pkg->deps[i];
        if (strlen(d->name) != first || strncmp(d->name, p, first)) continue;
        if (!dot) return file_unit(d->pkg->name, d->pkg->root, d->pkg, im->tok, d->pkg->root);
        const char *f = strf("%s/%s", d->pkg->rootdir, path_to_file(dot + 1));
        return file_unit(strf("%s.%s", d->pkg->name, dot + 1), f, d->pkg, im->tok, f);
    }
    const char *rel = path_to_file(p);
    const char *f = strf("%s/%s", pkg->rootdir, rel);
    /* a dependency's own modules are named after it: geometry.shapes */
    return file_unit(pkg == root_pkg ? p : strf("%s.%s", pkg->name, p), f, pkg, im->tok, rel);
}

static void add_dir(Package *pkg, const char *dir, const char *rel) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        const char *full = strf("%s/%s", dir, e->d_name);
        const char *r = rel ? strf("%s/%s", rel, e->d_name) : strf("%s", e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (strcmp(e->d_name, "out")) add_dir(pkg, full, r);
            continue;
        }
        size_t n = strlen(r);
        if (n < 4 || strcmp(r + n - 3, ".fl")) continue;
        char *dotted = xstrndup(r, n - 3);
        for (char *c = dotted; *c; c++)
            if (*c == '/') *c = '.';
        file_unit(dotted, full, pkg, NULL, full);
    }
    closedir(d);
}

Unit **load_program(const char *root_file, Package *pkg, bool all_files, int *nunits) {
    root_pkg = pkg;
    char *src = read_file(root_file);
    if (!src) error_at(NULL, "can't read %s", root_file);
    const char *base = strrchr(root_file, '/');
    base = base ? base + 1 : root_file;
    char *stem = strf("%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    Unit *root = new_unit(stem, real_path(root_file), src, pkg, false);
    root->is_root = true;
    root->prefix = "";
    root->dname = "";
    if (all_files) add_dir(pkg, pkg->rootdir, NULL);
    std_unit("fmt", NULL); /* @format, @hash and @eql expand into calls to these */
    std_unit("hash", NULL);
    std_unit("rt", NULL); /* the runtime: program entry, panics, output (every program has it) */
    for (int i = 0; i < nunits_; i++) { /* the list grows as imports are found */
        Unit *u = units[i];
        for (int j = 0; j < u->nimports; j++) {
            Import *im = &u->imports[j];
            if (im->header) im->c = c_header(im->header, u, im->tok);
            else im->u = resolve_import(u, im);
        }
    }
    for (int i = 0; i < nunits_; i++) prescan(units[i]->toks);
    for (int i = 0; i < nunits_; i++) parse(units[i]->toks, units[i]);
    *nunits = nunits_;
    return units;
}
