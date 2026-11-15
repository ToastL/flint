/* lex.c - source text -> tokens. Statements end with an explicit ';'. */
#include "flint.h"
#include <ctype.h>

static const char *KEYWORDS[] = {
    "fn", "let", "var", "const", "if", "else", "while", "for", "return", "break", "continue",
    "struct", "enum", "switch", "fallthrough", "orelse", "unreachable", "true", "false", "null", "undefined",
    "error", "try", "catch", "errdefer",
    "as", "extern", "export",
    "pub", "defer", "union", "packed", "unsafe", "comptime", "import", "test", "where", NULL,
};

/* longest first, so "..=" wins over ".." and "." */
static const char *PUNCT[] = {
    "...", "..=", "<<=", ">>=", "+%=", "-%=", "*%=", "+|=", "-|=", "*|=",
    "+%", "-%", "*%", "+|", "-|", "*|", ".*", ".?",
    "->", "=>", "..", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "<<", ">>", "++", "--",
    "+", "-", "*", "/", "%", "<", ">", "=", "!", "&", "|", "^", "~", ".", ",", ":", ";",
    "(", ")", "{", "}", "[", "]", "?", NULL,
};

typedef struct {
    Token *toks;
    int n, cap;
    char depth[1024]; /* open brackets; newlines inside ( ) and [ ] don't end statements */
    int nd;
    const char *src, *path;
} Lexer;

static Token *lex_range(const char *src, const char *path, int start, int end, int line, int ls, bool sub);

static Token mk(Lexer *L, TokKind k, int line, int col) {
    Token t;
    memset(&t, 0, sizeof t);
    t.k = k;
    t.line = line;
    t.col = col;
    t.path = L->path;
    t.src = L->src;
    return t;
}

static bool is_one_of(const char *s, const char **set) {
    for (; *set; set++)
        if (!strcmp(s, *set)) return true;
    return false;
}


/* Decode the escape at src[j] == '\\'. Returns the byte; *adv gets the length consumed. */
static int escape(Lexer *L, const char *src, int j, int line, int ls, int *adv) {
    char e = src[j + 1];
    *adv = 2;
    switch (e) {
    case 'n': return 10;
    case 't': return 9;
    case 'r': return 13;
    case '0': return 0;
    case '\\': return '\\';
    case '"': return '"';
    case '\'': return '\'';
    case '`': return '`';
    case '$': return '$';
    case 'x':
        if (isxdigit((unsigned char)src[j + 2]) && isxdigit((unsigned char)src[j + 3])) {
            char hx[3] = {src[j + 2], src[j + 3], 0};
            *adv = 4;
            return (int)strtol(hx, NULL, 16);
        }
        break;
    }
    Token et = mk(L, TK_OP, line, j - ls + 1);
    error_at(&et, "unknown escape '\\%c'", e);
}

static Token *lex_range(const char *src, const char *path, int start, int end, int line, int ls, bool sub) {
    Lexer L;
    memset(&L, 0, sizeof L);
    L.src = src;
    L.path = path;
    if (sub) L.depth[L.nd++] = '('; /* newlines inside ${...} never end a statement */
    int i = start;
    while (i < end) {
        char c = src[i];
        int col = i - ls + 1;
        Token here = mk(&L, TK_OP, line, col);
        if (c == '\n') {
            i++;
            line++;
            ls = i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            i++;
            continue;
        }
        if (c == '/' && i + 1 < end && src[i + 1] == '/') {
            while (i < end && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < end && src[i + 1] == '*') {
            int j = i + 2;
            while (j + 1 < end && !(src[j] == '*' && src[j + 1] == '/')) {
                if (src[j] == '\n') {
                    line++;
                    ls = j + 1;
                }
                j++;
            }
            if (j + 1 >= end) error_at(&here, "unterminated comment");
            i = j + 2;
            continue;
        }
        if (isdigit((unsigned char)c)) {
            char buf[128];
            int bl = 0, j = i, base = 10;
            bool is_float = false;
            if (c == '0' && (src[i + 1] == 'x' || src[i + 1] == 'X')) base = 16;
            if (c == '0' && (src[i + 1] == 'b' || src[i + 1] == 'B')) base = 2;
            if (c == '0' && (src[i + 1] == 'o' || src[i + 1] == 'O')) base = 8;
            if (base != 10) {
                j += 2;
                while (j < end && (isalnum((unsigned char)src[j]) || src[j] == '_')) {
                    if (src[j] != '_' && bl < 120) buf[bl++] = src[j];
                    j++;
                }
            } else {
                while (j < end && (isdigit((unsigned char)src[j]) || src[j] == '_')) {
                    if (src[j] != '_' && bl < 120) buf[bl++] = src[j];
                    j++;
                }
                /* after a '.', a number is a tuple field (t.0.1), never a float */
                bool field = L.n > 0 && L.toks[L.n - 1].k == TK_OP && !strcmp(L.toks[L.n - 1].s, ".");
                if (!field && j + 1 < end && src[j] == '.' && isdigit((unsigned char)src[j + 1])) {
                    is_float = true;
                    buf[bl++] = src[j++];
                    while (j < end && (isdigit((unsigned char)src[j]) || src[j] == '_')) {
                        if (src[j] != '_' && bl < 120) buf[bl++] = src[j];
                        j++;
                    }
                }
                if (!field && j + 1 < end && (src[j] == 'e' || src[j] == 'E') &&
                    (isdigit((unsigned char)src[j + 1]) ||
                     ((src[j + 1] == '+' || src[j + 1] == '-') && j + 2 < end && isdigit((unsigned char)src[j + 2])))) {
                    is_float = true;
                    buf[bl++] = src[j++];
                    buf[bl++] = src[j++];
                    while (j < end && isdigit((unsigned char)src[j])) {
                        if (bl < 120) buf[bl++] = src[j];
                        j++;
                    }
                }
            }
            buf[bl] = 0;
            Token t = mk(&L, is_float ? TK_FLOAT : TK_INT, line, col);
            if (is_float) {
                t.fval = strtod(buf, NULL);
            } else {
                if (bl == 0) error_at(&here, "bad number literal");
                uint64_t v = 0;
                for (int k = 0; k < bl; k++) {
                    int d = isdigit((unsigned char)buf[k]) ? buf[k] - '0' : (tolower((unsigned char)buf[k]) - 'a' + 10);
                    if (d < 0 || d >= base) error_at(&here, "bad digit '%c' in number literal", buf[k]);
                    if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) error_at(&here, "integer literal too large");
                    v = v * (uint64_t)base + (uint64_t)d;
                }
                t.ival = v;
            }
            PUSH(L.toks, L.n, L.cap, t);
            i = j;
            continue;
        }
        if ((isalpha((unsigned char)c) || c == '_') || (c == '@' && isalpha((unsigned char)src[i + 1]))) {
            int j = i + 1; /* identifiers; @name is a builtin */
            while (j < end && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
            char *w = xstrndup(src + i, (size_t)(j - i));
            Token t = mk(&L, is_one_of(w, KEYWORDS) ? TK_KW : TK_ID, line, col);
            t.s = w;
            PUSH(L.toks, L.n, L.cap, t);
            i = j;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            int j = i + 1;
            char *buf = NULL;
            int len = 0, cap = 0;
            for (;;) {
                if (j >= end || src[j] == '\n') error_at(&here, "unterminated literal");
                char ch = src[j];
                if (ch == q) {
                    j++;
                    break;
                }
                if (ch == '\\') {
                    int adv;
                    int v = escape(&L, src, j, line, ls, &adv);
                    PUSH(buf, len, cap, (char)v);
                    j += adv;
                    continue;
                }
                PUSH(buf, len, cap, ch);
                j++;
            }
            if (q == '"') {
                Token t = mk(&L, TK_STR, line, col);
                t.str = xalloc((size_t)len + 1);
                if (len) memcpy(t.str, buf, (size_t)len);
                t.slen = len;
                PUSH(L.toks, L.n, L.cap, t);
            } else {
                if (len != 1) error_at(&here, "character literal must be exactly one byte");
                Token t = mk(&L, TK_INT, line, col);
                t.ival = (unsigned char)buf[0];
                PUSH(L.toks, L.n, L.cap, t);
            }
            free(buf);
            i = j;
            continue;
        }
        if (c == '`')
            error_at(&here, "template literals were removed because they allocate behind your back; use "
                            "fmt.allocPrint(alloc, \"...{}...\", .{x}), fmt.bufPrint(&buf, ...) or io.print(...)");
        bool matched = false;
        for (const char **p = PUNCT; *p; p++) {
            size_t pl = strlen(*p);
            if (i + (int)pl <= end && !strncmp(src + i, *p, pl)) {
                if (pl == 1 && strchr("([{", c)) {
                    if (L.nd < (int)sizeof L.depth) L.depth[L.nd++] = c;
                } else if (pl == 1 && strchr(")]}", c)) {
                    if (L.nd > (sub ? 1 : 0)) L.nd--;
                }
                Token t = mk(&L, TK_OP, line, col);
                t.s = *p;
                PUSH(L.toks, L.n, L.cap, t);
                i += (int)pl;
                matched = true;
                break;
            }
        }
        if (!matched) error_at(&here, "unexpected character '%c'", c);
    }
    Token eof = mk(&L, TK_EOF, line, i - ls + 1);
    eof.s = "";
    PUSH(L.toks, L.n, L.cap, eof);
    return L.toks;
}

Token *lex(const char *src, const char *path) {
    return lex_range(src, path, 0, (int)strlen(src), 1, 0, false);
}
