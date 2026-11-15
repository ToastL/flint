/* util.c - memory helpers and error reporting */
#include "flint.h"

const char *g_path = "<input>";
const char *g_src = "";

void *xalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fprintf(stderr, "flint: out of memory\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) {
        fprintf(stderr, "flint: out of memory\n");
        exit(1);
    }
    return p;
}

char *xstrndup(const char *s, size_t n) {
    char *p = xalloc(n + 1);
    memcpy(p, s, n);
    return p;
}

char *strf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *p = xalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}

Token *g_calls[64]; /* calls of generic functions being instantiated, outermost first */
int g_ncalls;

static void show_line(Token *t) {
    const char *p = t->src ? t->src : g_src;
    for (int l = 1; l < t->line && *p; p++)
        if (*p == '\n') l++;
    const char *e = p;
    while (*e && *e != '\n') e++;
    fprintf(stderr, "  %4d | %.*s\n", t->line, (int)(e - p), p);
    fprintf(stderr, "       | ");
    for (int i = 1; i < t->col; i++) fputc(p[i - 1] == '\t' ? '\t' : ' ', stderr);
    fprintf(stderr, "^\n");
}

/* Print "file:line:col: error: msg", the source line, and a caret; then exit. Inside a generic
 * instance, also show the calls that made it, so the error leads back to the caller's code. */
_Noreturn void error_at(Token *t, const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!t) {
        fprintf(stderr, "%s: error: %s\n", g_path, msg);
        exit(1);
    }
    fprintf(stderr, "%s:%d:%d: error: %s\n", t->path ? t->path : g_path, t->line, t->col, msg);
    show_line(t);
    for (int i = g_ncalls - 1, shown = 0; i >= 0 && shown < 4; i--) {
        Token *c = g_calls[i];
        if (c == t || (c->line == t->line && c->path == t->path)) continue;
        fprintf(stderr, "%s:%d:%d: note: in the instance made by this call\n", c->path ? c->path : g_path, c->line, c->col);
        show_line(c);
        shown++;
    }
    exit(1);
}

/* an error followed by notes, each pointing at its own source line (comptime call chains) */
_Noreturn void error_notes(Token *t, int nn, Token **nt, const char **nm, const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!t) {
        fprintf(stderr, "%s: error: %s\n", g_path, msg);
    } else {
        fprintf(stderr, "%s:%d:%d: error: %s\n", t->path ? t->path : g_path, t->line, t->col, msg);
        show_line(t);
    }
    for (int i = 0; i < nn; i++) {
        if (!nt[i]) continue;
        fprintf(stderr, "%s:%d:%d: note: %s\n", nt[i]->path ? nt[i]->path : g_path, nt[i]->line, nt[i]->col, nm[i]);
        show_line(nt[i]);
    }
    for (int i = g_ncalls - 1, shown = 0; i >= 0 && shown < 4; i--) {
        Token *c = g_calls[i];
        if (t && (c == t || (c->line == t->line && c->path == t->path))) continue;
        fprintf(stderr, "%s:%d:%d: note: in the instance made by this call\n", c->path ? c->path : g_path, c->line, c->col);
        show_line(c);
        shown++;
    }
    exit(1);
}
