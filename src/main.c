/* main.c - the `flint` command line driver */
#include "flint.h"
#include "fmt_float.h"
#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *USAGE =
    "usage:\n"
    "  flint run   file.fl [args...]   compile and run\n"
    "  flint build file.fl [-o out]    compile to a native executable\n"
    "  flint test  file.fl             run the file's `test` blocks (and those of the modules it imports)\n"
    "  flint asm   file.fl             print the generated ARM64 assembly\n"
    "  flint ir    file.fl             print the optimized IR\n"
    "  flint tokens file.fl            print the tokens (for comparing compilers)\n"
    "in a package (a directory with flint.toml), leave out the file:\n"
    "  flint build [-o out]            builds out/<name>\n"
    "  flint run [-- args...]\n"
    "  flint test                      runs the tests in every .fl file of the package\n"
    "options:\n"
    "  -O Debug        all safety checks, undefined memory filled with 0xAA, no optimization (default)\n"
    "  -O ReleaseSafe  optimized, all safety checks\n"
    "  -O ReleaseFast  optimized; integer overflow wraps instead of trapping\n"
    "                  (bounds, null, cast and division checks stay: only 'unsafe' removes them)\n";

/* run argv, return exit status */
static int run(char **argv) {
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "flint: can't run %s\n", argv[0]);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 1;
}

/* flint ast: the tree, for comparing compilers */
static void dump_node(Node *n, int d) {
    if (!n) {
        printf("%*s-\n", d * 2, "");
        return;
    }
    printf("%*s%d @%d:%d", d * 2, "", n->k, n->tok ? n->tok->line : 0, n->tok ? n->tok->col : 0);
    if (n->name) printf(" n=%s", n->name);
    if (n->name2) printf(" n2=%s", n->name2);
    if (n->label) printf(" l=%s", n->label);
    if (n->ival) printf(" i=%llu", (unsigned long long)n->ival);
    if (n->fval != 0) {
        char b[40];
        printf(" f=%.*s", (int)fl_fmt_f64(n->fval, b), b);
    }
    if (n->str) {
        printf(" s=");
        for (int i = 0; i < n->slen; i++) printf("%02x", (unsigned char)n->str[i]);
    }
    if (n->flag) printf(" f1=%d", n->flag);
    if (n->flag2) printf(" f2=%d", n->flag2);
    if (n->exported || n->is_extern || n->is_unsafe || n->is_pub)
        printf(" b=%d%d%d%d", n->exported, n->is_extern, n->is_unsafe, n->is_pub);
    printf("\n");
    if (n->a || n->b || n->c) {
        dump_node(n->a, d + 1);
        dump_node(n->b, d + 1);
        dump_node(n->c, d + 1);
    }
    if (n->wherec) dump_node(n->wherec, d + 1);
    for (int i = 0; i < n->n; i++) dump_node(n->list[i], d + 1);
}

static bool ends_with(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && !strcmp(s + n - m, suf);
}

/* every package the build uses: the root package and its dependencies, each once */
static Package **all_pkgs;
static int npkgs, cpkgs;
static void collect_pkgs(Package *p) {
    for (int i = 0; i < npkgs; i++)
        if (all_pkgs[i] == p) return;
    PUSH(all_pkgs, npkgs, cpkgs, p);
    for (int i = 0; i < p->ndeps; i++) collect_pkgs(p->deps[i].pkg);
}

int main(int argc, char **argv) {
    int mode = MODE_DEBUG;
    const char *cmd = argc > 1 ? argv[1] : NULL, *path = NULL, *out = NULL;
    char **prog_args = NULL;
    int nprog = 0;
    bool known = cmd && (!strcmp(cmd, "run") || !strcmp(cmd, "build") || !strcmp(cmd, "asm") || !strcmp(cmd, "ir") ||
                         !strcmp(cmd, "test") || !strcmp(cmd, "tokens") || !strcmp(cmd, "ast"));
    if (!known) {
        fputs(USAGE, stderr);
        return 2;
    }
    for (int i = 2; i < argc; i++) {
        if (path && !strcmp(cmd, "run")) { /* everything after the file goes to the program */
            prog_args = &argv[i];
            nprog = argc - i;
            break;
        }
        if (!strcmp(argv[i], "--") && !strcmp(cmd, "run")) {
            prog_args = &argv[i + 1];
            nprog = argc - i - 1;
            break;
        }
        if (!strcmp(argv[i], "-O") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "Debug")) mode = MODE_DEBUG;
            else if (!strcmp(m, "ReleaseSafe")) mode = MODE_SAFE;
            else if (!strcmp(m, "ReleaseFast")) mode = MODE_FAST;
            else {
                fprintf(stderr, "flint: unknown build mode '%s' (Debug, ReleaseSafe or ReleaseFast)\n", m);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!path && ends_with(argv[i], ".fl")) path = argv[i];
        else {
            fprintf(stderr, "flint: unexpected argument '%s'\n%s", argv[i], USAGE);
            return 2;
        }
    }
    bool test_mode = !strcmp(cmd, "test");
    if (!strcmp(cmd, "ast")) { /* just this file, with its own type names */
        if (!path) {
            fputs(USAGE, stderr);
            return 2;
        }
        char *src = read_file(path);
        if (!src) {
            fprintf(stderr, "flint: can't read %s\n", path);
            return 1;
        }
        g_path = path;
        g_src = src;
        Unit *u = xalloc(sizeof *u);
        u->path = "main";
        u->toks = lex(src, path);
        parse_imports(u->toks, u);
        prescan(u->toks);
        parse(u->toks, u);
        for (int i = 0; i < u->nimports; i++)
            printf("import %s %s %s\n", u->imports[i].alias, u->imports[i].mpath ? u->imports[i].mpath : "",
                   u->imports[i].header ? u->imports[i].header : "");
        for (int i = 0; i < u->nitems; i++) dump_node(u->items[i], 0);
        return 0;
    }
    if (!strcmp(cmd, "tokens")) {
        if (!path) {
            fputs(USAGE, stderr);
            return 2;
        }
        char *src = read_file(path);
        if (!src) {
            fprintf(stderr, "flint: can't read %s\n", path);
            return 1;
        }
        g_path = path;
        g_src = src;
        static const char *kinds[] = {"id", "kw", "int", "float", "str", "op", "eof"};
        for (Token *t = lex(src, path);; t++) {
            printf("%d:%d %s", t->line, t->col, kinds[t->k]);
            if (t->k == TK_INT) printf(" %llu", (unsigned long long)t->ival);
            else if (t->k == TK_FLOAT) {
                char b[40];
                printf(" %.*s", (int)fl_fmt_f64(t->fval, b), b);
            } else if (t->k == TK_STR) {
                printf(" %d ", t->slen);
                for (int i = 0; i < t->slen; i++) printf("%02x", (unsigned char)t->str[i]);
            } else printf(" %s", t->s);
            printf("\n");
            if (t->k == TK_EOF) break;
        }
        return 0;
    }

    Package *pkg;
    bool package_mode = false;
    if (path) { /* a single file: its directory is the root that module paths start from */
        pkg = xalloc(sizeof *pkg);
        char *d = strf("%s", path);
        pkg->dir = pkg->rootdir = strf("%s", dirname(d));
        pkg->root = path;
        char *b = strf("%s", path);
        b = basename(b);
        char *dot = strrchr(b, '.');
        if (dot) *dot = 0;
        pkg->name = strf("%s", b);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) return 1;
        const char *dir = find_manifest(cwd);
        if (!dir) {
            fprintf(stderr, "flint: no file given, and no flint.toml in this directory or above it\n%s", USAGE);
            return 2;
        }
        pkg = read_manifest(dir);
        package_mode = true;
    }
    g_path = pkg->root;

    int nunits;
    Unit **units = load_program(pkg->root, pkg, test_mode && package_mode, &nunits);
    Module *m = lower_program(units, nunits, mode, test_mode);
    if (mode != MODE_DEBUG) optimize(m);

    if (!strcmp(cmd, "ir")) {
        dump_ir(m, stdout);
        return 0;
    }
    if (!strcmp(cmd, "asm")) {
        emit_arm64(m, stdout);
        return 0;
    }

    char tmpl[] = "/tmp/flint-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        fprintf(stderr, "flint: can't create a temp directory\n");
        return 1;
    }
    char *spath = strf("%s/prog.s", dir), *opath = strf("%s/prog.o", dir);
    FILE *f = fopen(spath, "w");
    emit_arm64(m, f);
    fclose(f);

    char *exe;
    if (!strcmp(cmd, "build")) {
        if (out) exe = (char *)out;
        else if (package_mode) {
            char *od = strf("%s/out", pkg->dir);
            mkdir(od, 0755);
            exe = strf("%s/%s", od, pkg->name);
        } else exe = strf("%s", pkg->name);
    } else {
        exe = strf("%s/prog", dir);
    }

    /* assemble with the system assembler, link with the C toolchain (for libc) */
    char *as_argv[] = {"as", "-arch", "arm64", "-o", opath, spath, NULL};
    if (run(as_argv) != 0) {
        fprintf(stderr, "flint: internal error: the generated assembly didn't assemble (%s)\n", spath);
        return 1;
    }
    const char **ld = NULL;
    int nld = 0, cld = 0;
    const char **objs = NULL;
    int nobj = 0, cobj = 0;
    collect_pkgs(pkg);
    for (int p = 0; p < npkgs; p++) { /* the packages' C sources, compiled by the C compiler */
        Package *pk = all_pkgs[p];
        for (int i = 0; i < pk->ncsrc; i++) {
            const char **cc = NULL;
            int ncc = 0, ccc = 0;
            const char *obj = strf("%s/c%d_%d.o", dir, p, i);
            PUSH(cc, ncc, ccc, g_cc);
            PUSH(cc, ncc, ccc, "-c");
            PUSH(cc, ncc, ccc, mode == MODE_DEBUG ? "-O0" : "-O2");
            for (int j = 0; j < pk->ncinc; j++) PUSH(cc, ncc, ccc, strf("-I%s", pk->cinc[j]));
            for (int j = 0; j < pk->ncflags; j++) PUSH(cc, ncc, ccc, pk->cflags[j]);
            PUSH(cc, ncc, ccc, pk->csrc[i]);
            PUSH(cc, ncc, ccc, "-o");
            PUSH(cc, ncc, ccc, obj);
            PUSH(cc, ncc, ccc, NULL);
            if (run((char **)cc) != 0) {
                fprintf(stderr, "flint: compiling %s failed\n", pk->csrc[i]);
                return 1;
            }
            PUSH(objs, nobj, cobj, obj);
        }
    }
    PUSH(ld, nld, cld, "cc");
    PUSH(ld, nld, cld, "-o");
    PUSH(ld, nld, cld, exe);
    PUSH(ld, nld, cld, opath);
    for (int i = 0; i < nobj; i++) PUSH(ld, nld, cld, objs[i]);
    for (int p = 0; p < npkgs; p++)
        for (int i = 0; i < all_pkgs[p]->nclink; i++) PUSH(ld, nld, cld, strf("-l%s", all_pkgs[p]->clink[i]));
    PUSH(ld, nld, cld, NULL);
    if (run((char **)ld) != 0) {
        fprintf(stderr, "flint: linking failed\n");
        return 1;
    }
    int status = 0;
    if (!strcmp(cmd, "run") || test_mode) {
        char **pargv = xalloc(sizeof(char *) * (size_t)(nprog + 2));
        pargv[0] = exe;
        for (int i = 0; i < nprog; i++) pargv[i + 1] = prog_args[i];
        status = run(pargv);
    }
    if (!getenv("FLINT_KEEP")) {
        unlink(spath);
        unlink(opath);
        for (int i = 0; i < nobj; i++) unlink(objs[i]);
        if (strcmp(cmd, "build")) unlink(exe);
        rmdir(dir);
    }
    return status;
}
