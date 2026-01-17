#include "cfgscript.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CFG_MAX_LINE
#define CFG_MAX_LINE 4096
#endif

#ifndef CFG_INCLUDE_DEPTH
#define CFG_INCLUDE_DEPTH 16
#endif

/* =========================================================
 * Last error message
 * ========================================================= */
static char g_last_err[512];

const char *cfg_last_error(void)
{
    return g_last_err;
}

static void set_errf(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (file && line > 0) {
        int n = snprintf(g_last_err, sizeof(g_last_err), "%s:%d: ", file, line);
        if (n < 0) {
            n = 0;
        }
        vsnprintf(g_last_err + (size_t)n, sizeof(g_last_err) - (size_t)n, fmt, ap);
    } else {
        vsnprintf(g_last_err, sizeof(g_last_err), fmt, ap);
    }
    va_end(ap);
}

/* =========================================================
 * small utils
 * ========================================================= */
static void *xmalloc(size_t n)
{
    return malloc(n ? n : 1);
}

static void *xrealloc(void *p, size_t n)
{
    return realloc(p, n ? n : 1);
}

static char *xstrdup(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    char *p = (char *)xmalloc(n + 1);

    if (!p) {
        return NULL;
    }
    if (n) {
        memcpy(p, s, n);
    }
    p[n] = 0;

    return p;
}

static char *ltrim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;

    return s;
}
static void rtrim_inplace(char *s)
{
    size_t n = strlen(s);

    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || isspace((unsigned char)s[n - 1]))) {
        s[n - 1] = 0;
        n--;
    }
}
static int startswith(const char *s, const char *pfx)
{
    while (*pfx) {
        if (*s++ != *pfx++) {
            return 0;
        }
    }

    return 1;
}

/* portable-ish strcasecmp */
static int strcasecmp_local(const char *a, const char *b)
{
    unsigned char ca, cb;

    while (*a && *b) {
        ca = (unsigned char)tolower((unsigned char)*a);
        cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        a++;
        b++;
    }

    return (int)(unsigned char)tolower((unsigned char)*a) -
           (int)(unsigned char)tolower((unsigned char)*b);
}

/* dirname("a/b/c.cfg") => "a/b" ; dirname("c.cfg") => "." */
static char *path_dirname(const char *path)
{
    const char *last = NULL;

    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') {
            last = p;
        }

    if (!last) {
        return xstrdup(".");
    }

    size_t n = (size_t)(last - path);

    if (n == 0) {
        return xstrdup(".");
    }

    char *out = (char *)xmalloc(n + 1);

    if (!out) {
        return NULL;
    }
    memcpy(out, path, n);
    out[n] = 0;

    return out;
}

static char *path_join(const char *dir, const char *file)
{
#ifdef _WIN32
    if ((isalpha((unsigned char)file[0]) && file[1] == ':' &&
         (file[2] == '\\' || file[2] == '/')) ||
        (file[0] == '\\' && file[1] == '\\')) {
        return xstrdup(file);
    }
#endif
    if (file[0] == '/' || file[0] == '\\') {
        return xstrdup(file);
    }

    size_t nd = strlen(dir), nf = strlen(file);
    int need_sep = (nd > 0 && dir[nd - 1] != '/' && dir[nd - 1] != '\\');
    char *out = (char *)xmalloc(nd + (need_sep ? 1 : 0) + nf + 1);

    if (!out) {
        return NULL;
    }

    memcpy(out, dir, nd);
    size_t pos = nd;

    if (need_sep) {
        out[pos++] = '/';
    }

    memcpy(out + pos, file, nf);
    out[pos + nf] = 0;

    return out;
}

/* =========================================================
 * string builder
 * ========================================================= */
typedef struct {
    char *buf;
    size_t len, cap;
} sbuf_t;

static void sbuf_init(sbuf_t *b)
{
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
}

static void sbuf_free(sbuf_t *b)
{
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

static int sbuf_reserve(sbuf_t *b, size_t add)
{
    size_t need = b->len + add + 1;
    if (need <= b->cap) {
        return 1;
    }

    size_t nc = b->cap ? b->cap * 2 : 256;

    while (nc < need)
        nc *= 2;

    char *p = (char *)xrealloc(b->buf, nc);

    if (!p) {
        return 0;
    }

    b->buf = p;
    b->cap = nc;

    return 1;
}

static int sbuf_append(sbuf_t *b, const char *s)
{
    size_t n = strlen(s);

    if (!sbuf_reserve(b, n))
        return 0;

    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;

    return 1;
}

static int sbuf_append_char(sbuf_t *b, char c)
{
    if (!sbuf_reserve(b, 1))
        return 0;

    b->buf[b->len++] = c;
    b->buf[b->len] = 0;

    return 1;
}

/* =========================================================
 * map: vars (macros)
 * ========================================================= */
typedef struct {
    char *k;
    char *v;
} kv_t;
typedef struct {
    kv_t *a;
    size_t n, cap;
} map_t;

static void map_init(map_t *m)
{
    memset(m, 0, sizeof(*m));
}

static void map_free(map_t *m)
{
    for (size_t i = 0; i < m->n; i++) {
        free(m->a[i].k);
        free(m->a[i].v);
    }

    free(m->a);
    memset(m, 0, sizeof(*m));
}

static int map_find(const map_t *m, const char *k)
{
    for (size_t i = 0; i < m->n; i++)
        if (strcmp(m->a[i].k, k) == 0)
            return (int)i;

    return -1;
}

static const char *map_get(const map_t *m, const char *k)
{
    int idx = map_find(m, k);

    return (idx >= 0) ? m->a[idx].v : NULL;
}

static int map_set(map_t *m, const char *k, const char *v)
{
    int idx = map_find(m, k);

    if (idx >= 0) {
        char *nv = xstrdup(v ? v : "");
        if (!nv) {
            return 0;
        }
        free(m->a[idx].v);
        m->a[idx].v = nv;
        return 1;
    }

    if (m->n == m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 32;
        kv_t *na = (kv_t *)xrealloc(m->a, nc * sizeof(kv_t));
        if (!na) {
            return 0;
        }
        m->a = na;
        m->cap = nc;
    }

    m->a[m->n].k = xstrdup(k);
    m->a[m->n].v = xstrdup(v ? v : "");

    if (!m->a[m->n].k || !m->a[m->n].v) {
        return 0;
    }

    m->n++;
    return 1;
}

static void map_unset(map_t *m, const char *k)
{
    int idx = map_find(m, k);

    if (idx < 0) {
        return;
    }

    free(m->a[idx].k);
    free(m->a[idx].v);
    m->a[idx] = m->a[m->n - 1];
    m->n--;
}

/* =========================================================
 * outbuf: preprocessed lines + origins
 * ========================================================= */
typedef struct {
    const char *file;
    int line;
} origin_t;

typedef struct {
    char **lines;
    origin_t *orig;
    size_t n, cap;

    char **files;
    size_t nfiles, capfiles;
} outbuf_t;

static cfg_status_t outbuf_to_text(const outbuf_t *pp, char **out_text, size_t *out_len,
                                   int with_origin)
{
    if (!out_text) {
        return CFG_ERR_PARSE;
    }

    *out_text = NULL;

    if (out_len) {
        *out_len = 0;
    }

    sbuf_t b;
    sbuf_init(&b);

    for (size_t i = 0; i < pp->n; i++) {
        const char *line = pp->lines[i] ? pp->lines[i] : "";

        if (with_origin) {
            char hdr[512];
            const char *f = pp->orig[i].file ? pp->orig[i].file : "<unknown>";
            int ln = pp->orig[i].line;
            /* 以注释形式输出来源，不影响 KV parser（因为 '#' 行会被忽略） */
            snprintf(hdr, sizeof(hdr), "# %s:%d\n", f, ln);
            if (!sbuf_append(&b, hdr)) {
                sbuf_free(&b);
                return CFG_ERR_OOM;
            }
        }

        if (!sbuf_append(&b, line)) {
            sbuf_free(&b);
            return CFG_ERR_OOM;
        }

        if (!sbuf_append_char(&b, '\n')) {
            sbuf_free(&b);
            return CFG_ERR_OOM;
        }
    }

    if (!b.buf) {
        b.buf = xstrdup("");
        if (!b.buf) {
            return CFG_ERR_OOM;
        }
        b.len = 0;
        b.cap = 1;
    }

    *out_text = b.buf;
    if (out_len) {
        *out_len = b.len;
    }
    /* 注意：这里不能 sbuf_free，否则会 free 掉 buf。我们把 buf 交给调用者。 */
    return CFG_OK;
}

static void outbuf_init(outbuf_t *o)
{
    memset(o, 0, sizeof(*o));
}

static void outbuf_free(outbuf_t *o)
{
    for (size_t i = 0; i < o->n; i++)
        free(o->lines[i]);
    free(o->lines);
    free(o->orig);

    for (size_t i = 0; i < o->nfiles; i++)
        free(o->files[i]);
    free(o->files);
    memset(o, 0, sizeof(*o));
}

static const char *outbuf_intern_file(outbuf_t *o, const char *file)
{
    for (size_t i = 0; i < o->nfiles; i++)
        if (strcmp(o->files[i], file) == 0)
            return o->files[i];

    if (o->nfiles == o->capfiles) {
        size_t nc = o->capfiles ? o->capfiles * 2 : 16;
        char **nf = (char **)xrealloc(o->files, nc * sizeof(char *));
        if (!nf) {
            return NULL;
        }
        o->files = nf;
        o->capfiles = nc;
    }

    o->files[o->nfiles] = xstrdup(file);

    if (!o->files[o->nfiles]) {
        return NULL;
    }

    return o->files[o->nfiles++];
}
static int outbuf_add_line(outbuf_t *o, const char *line, const char *file, int lineno)
{
    if (o->n == o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 128;
        char **nl = (char **)xrealloc(o->lines, nc * sizeof(char *));
        origin_t *no = (origin_t *)xrealloc(o->orig, nc * sizeof(origin_t));
        if (!nl || !no) {
            return 0;
        }
        o->lines = nl;
        o->orig = no;
        o->cap = nc;
    }

    o->lines[o->n] = xstrdup(line);

    if (!o->lines[o->n]) {
        return 0;
    }

    const char *f = outbuf_intern_file(o, file ? file : "<unknown>");

    if (!f) {
        return 0;
    }
    o->orig[o->n].file = f;
    o->orig[o->n].line = lineno;
    o->n++;

    return 1;
}

/* =========================================================
 * macro expansion ${NAME}
 * ========================================================= */
static int expand_macros_into(sbuf_t *out, const map_t *vars, const char *line)
{
    for (size_t i = 0; line && line[i];) {
        if (line[i] == '$' && line[i + 1] == '{') {
            size_t j = i + 2;
            while (line[j] && line[j] != '}')
                j++;

            if (line[j] == '}') {
                char name[256];
                size_t nlen = j - (i + 2);

                if (nlen >= sizeof(name))
                    nlen = sizeof(name) - 1;

                memcpy(name, line + i + 2, nlen);
                name[nlen] = 0;
                const char *v = map_get(vars, name);

                if (v && !sbuf_append(out, v))
                    return 0;
                i = j + 1;
                continue;
            }
        }

        if (!sbuf_append_char(out, line[i]))
            return 0;
        i++;
    }
    return 1;
}
static char *expand_macros_dup(const map_t *vars, const char *line)
{
    sbuf_t b;
    sbuf_init(&b);

    if (!expand_macros_into(&b, vars, line ? line : "")) {
        sbuf_free(&b);
        return NULL;
    }

    return b.buf ? b.buf : xstrdup("");
}

/* =========================================================
 * expression evaluator (strict error)
 * ========================================================= */
static int is_num_str(const char *s)
{
    if (!s || !*s) {
        return 0;
    }

    const char *p = s;

    if (*p == '+' || *p == '-') {
        p++;
    }

    if (!isdigit((unsigned char)*p))
        return 0;

    while (*p) {
        if (!isdigit((unsigned char)*p))
            return 0;
        p++;
    }

    return 1;
}

typedef enum {
    TK_EOF,
    TK_LP,
    TK_RP,
    TK_NOT,
    TK_AND,
    TK_OR,
    TK_EQ,
    TK_NE,
    TK_LT,
    TK_LE,
    TK_GT,
    TK_GE,
    TK_PLUS,
    TK_MINUS,
    TK_MUL,
    TK_DIV,
    TK_MOD,
    TK_INT,
    TK_ID,
    TK_STR
} tok_t;

typedef struct {
    const char *p;
    tok_t tk;
    long long ival;
    char id[256];
    char *sval; /* owned */
} lexer_t;

static void lex_init(lexer_t *lx, const char *s)
{
    lx->p = s ? s : "";
    lx->tk = TK_EOF;
    lx->ival = 0;
    lx->id[0] = 0;
    lx->sval = NULL;
}

static void lex_free(lexer_t *lx)
{
    free(lx->sval);
    lx->sval = NULL;
}

static void lex_skip_ws(lexer_t *lx)
{
    while (*lx->p && isspace((unsigned char)*lx->p))
        lx->p++;
}

static void lex_next(lexer_t *lx)
{
    free(lx->sval);
    lx->sval = NULL;
    lex_skip_ws(lx);
    const char *p = lx->p;
    if (!*p) {
        lx->tk = TK_EOF;
        return;
    }

    if (startswith(p, "&&")) {
        lx->tk = TK_AND;
        lx->p += 2;
        return;
    }
    if (startswith(p, "||")) {
        lx->tk = TK_OR;
        lx->p += 2;
        return;
    }
    if (startswith(p, "==")) {
        lx->tk = TK_EQ;
        lx->p += 2;
        return;
    }
    if (startswith(p, "!=")) {
        lx->tk = TK_NE;
        lx->p += 2;
        return;
    }
    if (startswith(p, "<=")) {
        lx->tk = TK_LE;
        lx->p += 2;
        return;
    }
    if (startswith(p, ">=")) {
        lx->tk = TK_GE;
        lx->p += 2;
        return;
    }

    if (*p == '!') {
        lx->tk = TK_NOT;
        lx->p++;
        return;
    }
    if (*p == '(') {
        lx->tk = TK_LP;
        lx->p++;
        return;
    }
    if (*p == ')') {
        lx->tk = TK_RP;
        lx->p++;
        return;
    }
    if (*p == '<') {
        lx->tk = TK_LT;
        lx->p++;
        return;
    }
    if (*p == '>') {
        lx->tk = TK_GT;
        lx->p++;
        return;
    }

    if (*p == '+') {
        lx->tk = TK_PLUS;
        lx->p++;
        return;
    }
    if (*p == '-') {
        lx->tk = TK_MINUS;
        lx->p++;
        return;
    }
    if (*p == '*') {
        lx->tk = TK_MUL;
        lx->p++;
        return;
    }
    if (*p == '/') {
        lx->tk = TK_DIV;
        lx->p++;
        return;
    }
    if (*p == '%') {
        lx->tk = TK_MOD;
        lx->p++;
        return;
    }

    if (*p == '"' || *p == '\'') {
        char q = *p++;
        char tmp[CFG_MAX_LINE];
        size_t n = 0;
        while (*p && *p != q) {
            if (*p == '\\' && p[1]) {
                p++;
                char c = *p++;
                if (c == 'n') {
                    c = '\n';
                }
                else if (c == 'r')
                    c = '\r';
                else if (c == 't')
                    c = '\t';
                tmp[n++] = c;
            } else {
                tmp[n++] = *p++;
            }
            if (n + 1 >= sizeof(tmp))
                break;
        }
        if (*p == q) {
            p++;
        }
        tmp[n] = 0;
        lx->tk = TK_STR;
        lx->sval = xstrdup(tmp);
        lx->p = p;
        return;
    }

    if (isdigit((unsigned char)*p) || ((*p == '+' || *p == '-') && isdigit((unsigned char)p[1]))) {
        char *end = NULL;
        lx->ival = strtoll(p, &end, 10);
        lx->tk = TK_INT;
        lx->p = end;
        return;
    }

    if (isalpha((unsigned char)*p) || *p == '_') {
        size_t n = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
            if (n + 1 < sizeof(lx->id))
                lx->id[n++] = *p;
            p++;
        }
        lx->id[n] = 0;
        lx->tk = TK_ID;
        lx->p = p;
        return;
    }

    /* unknown char => EOF (parser will flag unexpected token) */
    lx->tk = TK_EOF;
}

typedef enum { VT_INT, VT_STR } vtype_t;
typedef struct {
    vtype_t t;
    long long i;
    char *s;
} val_t;

static val_t v_int(long long x)
{
    val_t v;
    v.t = VT_INT;
    v.i = x;
    v.s = NULL;
    return v;
}

static val_t v_str(const char *s)
{
    val_t v;
    v.t = VT_STR;
    v.i = 0;
    v.s = xstrdup(s ? s : "");
    return v;
}

static void v_free(val_t *v)
{
    if (v->t == VT_STR) {
        free(v->s);
    }
    v->s = NULL;
}

static int v_truthy(const val_t *v)
{
    return (v->t == VT_INT) ? (v->i != 0) : (v->s && v->s[0] != 0);
}

static long long v_to_int(const val_t *v)
{
    if (v->t == VT_INT) {
        return v->i;
    }
    if (v->s && is_num_str(v->s))
        return strtoll(v->s, NULL, 10);
    return 0;
}

typedef struct {
    lexer_t lx;
    const map_t *vars;
    int err;
    char emsg[256];
} expr_ctx_t;

static void expr_fail(expr_ctx_t *c, const char *fmt, ...)
{
    if (c->err) {
        return;
    }
    c->err = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->emsg, sizeof(c->emsg), fmt, ap);
    va_end(ap);
}

static val_t parse_expr(expr_ctx_t *c); /* fwd */

static val_t resolve_id(expr_ctx_t *c, const char *id)
{
    if (strcmp(id, "true") == 0)
        return v_int(1);

    if (strcmp(id, "false") == 0)
        return v_int(0);

    const char *v = map_get(c->vars, id);
    if (!v) {
        return v_str("");
    }
    if (is_num_str(v))
        return v_int(strtoll(v, NULL, 10));
    return v_str(v);
}

static val_t parse_primary(expr_ctx_t *c)
{
    lexer_t *lx = &c->lx;
    if (c->err) {
        return v_int(0);
    }

    if (lx->tk == TK_INT) {
        val_t v = v_int(lx->ival);
        lex_next(lx);
        return v;
    }

    if (lx->tk == TK_STR) {
        val_t v = v_str(lx->sval ? lx->sval : "");
        lex_next(lx);
        return v;
    }

    if (lx->tk == TK_ID) {
        char id[256];
        strncpy(id, lx->id, sizeof(id));
        id[sizeof(id) - 1] = 0;
        lex_next(lx);

        if (strcmp(id, "defined") == 0) {
            if (lx->tk != TK_LP) {
                expr_fail(c, "expected '(' after defined");
                return v_int(0);
            }
            lex_next(lx);
            if (lx->tk != TK_ID) {
                expr_fail(c, "expected identifier inside defined(...)");
                return v_int(0);
            }
            char name[256];
            strncpy(name, lx->id, sizeof(name));
            name[sizeof(name) - 1] = 0;
            lex_next(lx);
            if (lx->tk != TK_RP) {
                expr_fail(c, "expected ')' after defined(...)");
                return v_int(0);
            }
            lex_next(lx);
            return v_int(map_get(c->vars, name) ? 1 : 0);
        }

        return resolve_id(c, id);
    }

    if (lx->tk == TK_LP) {
        lex_next(lx);
        val_t v = parse_expr(c);
        if (lx->tk != TK_RP) {
            v_free(&v);
            expr_fail(c, "missing ')'");
            return v_int(0);
        }
        lex_next(lx);
        return v;
    }

    expr_fail(c, "unexpected token in expression");
    return v_int(0);
}

static val_t parse_unary(expr_ctx_t *c)
{
    lexer_t *lx = &c->lx;
    if (c->err) {
        return v_int(0);
    }

    if (lx->tk == TK_NOT) {
        lex_next(lx);
        val_t v = parse_unary(c);
        int r = !v_truthy(&v);
        v_free(&v);
        return v_int(r);
    }

    if (lx->tk == TK_PLUS) {
        lex_next(lx);
        return parse_unary(c);
    }

    if (lx->tk == TK_MINUS) {
        lex_next(lx);
        val_t v = parse_unary(c);
        long long a = v_to_int(&v);
        v_free(&v);
        return v_int(-a);
    }

    return parse_primary(c);
}

static val_t parse_mul(expr_ctx_t *c)
{
    lexer_t *lx = &c->lx;
    val_t v = parse_unary(c);

    while (!c->err && (lx->tk == TK_MUL || lx->tk == TK_DIV || lx->tk == TK_MOD)) {
        tok_t op = lx->tk;
        lex_next(lx);
        val_t rhs = parse_unary(c);

        long long a = v_to_int(&v);
        long long b = v_to_int(&rhs);
        long long r = 0;

        if (op == TK_MUL) {
            r = a * b;
        }
        else if (op == TK_DIV) {
            if (b == 0) {
                v_free(&v);
                v_free(&rhs);
                expr_fail(c, "division by zero");
                return v_int(0);
            }
            r = a / b;
        } else if (op == TK_MOD) {
            if (b == 0) {
                v_free(&v);
                v_free(&rhs);
                expr_fail(c, "modulo by zero");
                return v_int(0);
            }
            r = a % b;
        }

        v_free(&v);
        v_free(&rhs);
        v = v_int(r);
    }
    return v;
}

static val_t parse_add(expr_ctx_t *c)
{
    lexer_t *lx = &c->lx;
    val_t v = parse_mul(c);

    while (!c->err && (lx->tk == TK_PLUS || lx->tk == TK_MINUS)) {
        tok_t op = lx->tk;
        lex_next(lx);
        val_t rhs = parse_mul(c);

        long long a = v_to_int(&v);
        long long b = v_to_int(&rhs);
        long long r = (op == TK_PLUS) ? (a + b) : (a - b);

        v_free(&v);
        v_free(&rhs);
        v = v_int(r);
    }
    return v;
}

static int cmp_vals(const val_t *a, const val_t *b, tok_t op)
{
    if (a->t == VT_INT && b->t == VT_INT) {
        long long x = a->i, y = b->i;
        switch (op) {
        case TK_EQ:
            return x == y;
        case TK_NE:
            return x != y;
        case TK_LT:
            return x < y;
        case TK_LE:
            return x <= y;
        case TK_GT:
            return x > y;
        case TK_GE:
            return x >= y;
        default:
            return 0;
        }
    }

    char ax[64], bx[64];
    const char *as =
        (a->t == VT_STR) ? (a->s ? a->s : "") : (snprintf(ax, sizeof(ax), "%" PRId64, a->i), ax);
    const char *bs =
        (b->t == VT_STR) ? (b->s ? b->s : "") : (snprintf(bx, sizeof(bx), "%" PRId64, b->i), bx);
    int c = strcmp(as, bs);

    switch (op) {
    case TK_EQ:
        return c == 0;
    case TK_NE:
        return c != 0;
    case TK_LT:
        return c < 0;
    case TK_LE:
        return c <= 0;
    case TK_GT:
        return c > 0;
    case TK_GE:
        return c >= 0;
    default:
        return 0;
    }
}

static val_t parse_rel(expr_ctx_t *c)
{
    lexer_t *lx = &c->lx;
    val_t left = parse_add(c);

    while (!c->err && (lx->tk == TK_EQ || lx->tk == TK_NE || lx->tk == TK_LT || lx->tk == TK_LE ||
                       lx->tk == TK_GT || lx->tk == TK_GE)) {
        tok_t op = lx->tk;
        lex_next(lx);
        val_t right = parse_add(c);

        int r = cmp_vals(&left, &right, op);
        v_free(&left);
        v_free(&right);
        left = v_int(r);
    }

    return left;
}

static val_t parse_and(expr_ctx_t *c)
{
    lexer_t *lx = &c->lx;
    val_t v = parse_rel(c);

    while (!c->err && lx->tk == TK_AND) {
        lex_next(lx);
        val_t rhs = parse_rel(c);
        int r = v_truthy(&v) && v_truthy(&rhs);
        v_free(&v);
        v_free(&rhs);
        v = v_int(r);
    }

    return v;
}

static val_t parse_expr(expr_ctx_t *c)
{
    lexer_t *lx = &c->lx;
    val_t v = parse_and(c);

    while (!c->err && lx->tk == TK_OR) {
        lex_next(lx);
        val_t rhs = parse_and(c);
        int r = v_truthy(&v) || v_truthy(&rhs);
        v_free(&v);
        v_free(&rhs);
        v = v_int(r);
    }

    return v;
}

static int eval_bool_expr_strict(const map_t *vars, const char *expr, char *out_emsg,
                                 size_t emsg_sz)
{
    expr_ctx_t c;
    c.vars = vars;
    c.err = 0;
    c.emsg[0] = 0;
    lex_init(&c.lx, expr);
    lex_next(&c.lx);
    val_t v = parse_expr(&c);

    /* must consume all tokens */
    if (!c.err && c.lx.tk != TK_EOF) {
        expr_fail(&c, "unexpected trailing tokens");
    }

    int ok = !c.err;
    int truth = v_truthy(&v);

    v_free(&v);
    lex_free(&c.lx);

    if (!ok && out_emsg && emsg_sz) {
        snprintf(out_emsg, emsg_sz, "%s", c.emsg[0] ? c.emsg : "expression error");
    }

    return ok ? truth : 0;
}

static int eval_int_expr_strict(const map_t *vars, const char *expr, long long *out_val,
                                char *out_emsg, size_t emsg_sz)
{
    expr_ctx_t c;
    c.vars = vars;
    c.err = 0;
    c.emsg[0] = 0;
    lex_init(&c.lx, expr);
    lex_next(&c.lx);
    val_t v = parse_expr(&c);

    if (!c.err && c.lx.tk != TK_EOF) {
        expr_fail(&c, "unexpected trailing tokens");
    }

    int ok = !c.err;
    long long r = v_to_int(&v);

    v_free(&v);
    lex_free(&c.lx);

    if (ok) {
        if (out_val) {
            *out_val = r;
        }
    } else {
        if (out_emsg && emsg_sz) {
            snprintf(out_emsg, emsg_sz, "%s", c.emsg[0] ? c.emsg : "expression error");
        }
    }

    return ok;
}

/* =========================================================
 * directives
 * ========================================================= */
static int is_directive_line(const char *line)
{
    const char *p = line;

    while (*p && isspace((unsigned char)*p))
        p++;

    return *p == '%';
}

static int directive_name(const char *line, char *name, size_t nname, const char **out_args)
{
    const char *p = line;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '%') {
        return 0;
    }

    p++;

    while (*p && isspace((unsigned char)*p))
        p++;

    size_t i = 0;

    while (*p && (isalpha((unsigned char)*p) || *p == '_')) {

        if (i + 1 < nname) {
            name[i++] = *p;
        }
        p++;
    }

    name[i] = 0;

    while (*p && isspace((unsigned char)*p))
        p++;
    *out_args = p;

    return 1;
}

/* %define NAME value... (only ${} expansion) */
static int handle_define(map_t *vars, const char *args)
{
    char *tmp = xstrdup(args ? args : "");

    if (!tmp) {
        return 0;
    }

    char *p = ltrim(tmp);

    char name[256] = {0};
    size_t i = 0;

    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i + 1 < sizeof(name))
            name[i++] = *p;
        p++;
    }

    name[i] = 0;
    p = ltrim(p);

    char *val = expand_macros_dup(vars, p);

    if (!val) {
        free(tmp);
        return 0;
    }
    rtrim_inplace(val);

    int ok = 1;

    if (name[0]) {
        ok = map_set(vars, name, val);
    }

    free(val);
    free(tmp);

    return ok;
}

static void handle_undef(map_t *vars, const char *args)
{
    char *tmp = xstrdup(args ? args : "");

    if (!tmp) {
        return;
    }
    char *p = ltrim(tmp);
    char name[256] = {0};
    size_t i = 0;

    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i + 1 < sizeof(name))
            name[i++] = *p;
        p++;
    }

    name[i] = 0;

    if (name[0]) {
        map_unset(vars, name);
    }
    free(tmp);
}

/* NEW: %set NAME <expr>  (strict eval, int result stored as string) */
static int handle_set(map_t *vars, const char *args, char *errbuf, size_t errsz)
{
    char *tmp = xstrdup(args ? args : "");

    if (!tmp) {
        return 0;
    }
    char *p = ltrim(tmp);

    char name[256] = {0};
    size_t i = 0;

    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i + 1 < sizeof(name))
            name[i++] = *p;
        p++;
    }

    name[i] = 0;
    p = ltrim(p);

    if (!name[0]) {
        free(tmp);
        return 0;
    }

    if (*p == '=') {
        p++;
        p = ltrim(p);
    }

    char *expr = expand_macros_dup(vars, p);

    if (!expr) {
        free(tmp);
        return 0;
    }
    rtrim_inplace(expr);

    long long v = 0;
    int ok = eval_int_expr_strict(vars, expr, &v, errbuf, errsz);

    free(expr);

    if (!ok) {
        free(tmp);
        return 0;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%" PRId64, v);
    ok = map_set(vars, name, buf);

    free(tmp);
    return ok;
}

/* parse %include "file" or %include <file> */
static char *parse_include_target(const char *args)
{
    const char *p = args;

    while (*p && isspace((unsigned char)*p))
        p++;

    if (*p == '"') {
        p++;
        const char *q = p;
        while (*q && *q != '"')
            q++;
        size_t n = (size_t)(q - p);
        char *s = (char *)xmalloc(n + 1);
        if (!s) {
            return NULL;
        }
        memcpy(s, p, n);
        s[n] = 0;
        return s;
    }

    if (*p == '<') {
        p++;
        const char *q = p;
        while (*q && *q != '>')
            q++;
        size_t n = (size_t)(q - p);
        char *s = (char *)xmalloc(n + 1);
        if (!s) {
            return NULL;
        }
        memcpy(s, p, n);
        s[n] = 0;
        return s;
    }

    const char *q = p;

    while (*q && !isspace((unsigned char)*q))
        q++;
    size_t n = (size_t)(q - p);

    if (!n) {
        return NULL;
    }
    char *s = (char *)xmalloc(n + 1);

    if (!s) {
        return NULL;
    }
    memcpy(s, p, n);
    s[n] = 0;

    return s;
}

/* =========================================================
 * %for parsing
 * ========================================================= */
static void split_list_items(const char *s, char ***out_items, size_t *out_n)
{
    *out_items = NULL;
    *out_n = 0;
    char *tmp = xstrdup(s ? s : "");

    if (!tmp) {
        return;
    }
    char *p = tmp;

    while (*p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p) {
            break;
        }

        char *start = p;

        while (*p && *p != ',')
            p++;
        if (*p == ',') {
            *p = 0;
            p++;
        }

        char *end = start + strlen(start);

        while (end > start && isspace((unsigned char)end[-1]))
            end--;
        *end = 0;

        char **ni = (char **)xrealloc(*out_items, ((*out_n) + 1) * sizeof(char *));

        if (!ni) {
            break;
        }
        *out_items = ni;
        (*out_items)[(*out_n)++] = xstrdup(start);
    }

    free(tmp);
}

static void make_range_items(long long a, long long b, char ***out_items, size_t *out_n)
{
    *out_items = NULL;
    *out_n = 0;
    long long step = (a <= b) ? 1 : -1;
    size_t cap = 0;

    for (long long x = a;; x += step) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%" PRId64, x);

        if (*out_n == cap) {
            size_t nc = cap ? cap * 2 : 16;
            char **ni = (char **)xrealloc(*out_items, nc * sizeof(char *));
            if (!ni) {
                break;
            }
            *out_items = ni;
            cap = nc;
        }

        (*out_items)[(*out_n)++] = xstrdup(buf);

        if (x == b) {
            break;
        }
    }
}

/* parse "%for VAR in RHS"; RHS supports ${} expansion. range uses "A..B" */
static int parse_for(const map_t *vars, const char *args, char **out_var, char ***out_items,
                     size_t *out_n)
{
    *out_var = NULL;
    *out_items = NULL;
    *out_n = 0;

    char *tmp = xstrdup(args ? args : "");

    if (!tmp) {
        return 0;
    }

    char *p = ltrim(tmp);

    char var[256] = {0};
    size_t vi = 0;

    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (vi + 1 < sizeof(var))
            var[vi++] = *p;
        p++;
    }

    var[vi] = 0;
    p = ltrim(p);

    if (!var[0]) {
        free(tmp);
        return 0;
    }

    if (!(p[0] == 'i' && p[1] == 'n' && (p[2] == 0 || isspace((unsigned char)p[2])))) {
        free(tmp);
        return 0;
    }

    p += 2;
    p = ltrim(p);

    char *rhs = expand_macros_dup(vars, p);

    if (!rhs) {
        free(tmp);
        return 0;
    }
    rtrim_inplace(rhs);

    char *dots = strstr(rhs, "..");

    if (dots) {
        *dots = 0;
        char *lhs = ltrim(rhs);
        char *rr = ltrim(dots + 2);
        rtrim_inplace(lhs);
        rtrim_inplace(rr);

        long long a = 0, b = 0;

        if (is_num_str(lhs))
            a = strtoll(lhs, NULL, 10);
        else {
            const char *v = map_get(vars, lhs);
            if (!v || !is_num_str(v)) {
                free(rhs);
                free(tmp);
                return 0;
            }
            a = strtoll(v, NULL, 10);
        }

        if (is_num_str(rr))
            b = strtoll(rr, NULL, 10);
        else {
            const char *v = map_get(vars, rr);
            if (!v || !is_num_str(v)) {
                free(rhs);
                free(tmp);
                return 0;
            }
            b = strtoll(v, NULL, 10);
        }

        make_range_items(a, b, out_items, out_n);
    } else {
        split_list_items(rhs, out_items, out_n);
    }

    free(rhs);
    free(tmp);

    *out_var = xstrdup(var);
    return (*out_var && *out_n > 0);
}

static void free_items(char **items, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(items[i]);
    free(items);
}

/* =========================================================
 * Preprocess engine (supports nested for by recursion)
 * ========================================================= */
typedef struct {
    int parent_active;
    int this_active;
    int any_taken;
} if_frame_t;

typedef struct {
    if_frame_t *a;
    size_t n, cap;
} if_stack_t;

static void ifs_init(if_stack_t *s)
{
    memset(s, 0, sizeof(*s));
}

static void ifs_free(if_stack_t *s)
{
    free(s->a);
    memset(s, 0, sizeof(*s));
}

static int ifs_push(if_stack_t *s, if_frame_t f)
{
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 16;
        if_frame_t *na = (if_frame_t *)xrealloc(s->a, nc * sizeof(if_frame_t));
        if (!na) {
            return 0;
        }
        s->a = na;
        s->cap = nc;
    }

    s->a[s->n++] = f;
    return 1;
}

static if_frame_t *ifs_top(if_stack_t *s)
{
    return s->n ? &s->a[s->n - 1] : NULL;
}

static void ifs_pop(if_stack_t *s)
{
    if (s->n) {
        s->n--;
    }
}

static int current_active(const if_stack_t *s)
{
    for (size_t i = 0; i < s->n; i++)
        if (!s->a[i].this_active) {
            return 0;
        }
    return 1;
}

/* forward */
static cfg_status_t preprocess_file(const char *path, int depth, map_t *vars, outbuf_t *out);

typedef struct {
    char **lines;
    int *linenos;
    size_t n, cap;
} file_lines_t;

static void file_lines_free(file_lines_t *fl)
{
    for (size_t i = 0; i < fl->n; i++)
        free(fl->lines[i]);

    free(fl->lines);
    free(fl->linenos);
    memset(fl, 0, sizeof(*fl));
}

static cfg_status_t load_file_lines(const char *path, file_lines_t *fl)
{
    memset(fl, 0, sizeof(*fl));
    FILE *fp = fopen(path, "rb");

    if (!fp) {
        return CFG_ERR_IO;
    }

    char buf[CFG_MAX_LINE];
    int lineno = 0;

    while (fgets(buf, sizeof(buf), fp)) {
        lineno++;
        rtrim_inplace(buf);

        if (fl->n == fl->cap) {
            size_t nc = fl->cap ? fl->cap * 2 : 256;
            char **nl = (char **)xrealloc(fl->lines, nc * sizeof(char *));
            int *ni = (int *)xrealloc(fl->linenos, nc * sizeof(int));
            if (!nl || !ni) {
                fclose(fp);
                return CFG_ERR_OOM;
            }
            fl->lines = nl;
            fl->linenos = ni;
            fl->cap = nc;
        }
        fl->lines[fl->n] = xstrdup(buf);
        if (!fl->lines[fl->n]) {
            fclose(fp);
            return CFG_ERR_OOM;
        }
        fl->linenos[fl->n] = lineno;
        fl->n++;
    }

    fclose(fp);
    return CFG_OK;
}

/* process a block: lines[idx..end) */
static cfg_status_t preprocess_block(const char *cur_path, int depth, map_t *vars, outbuf_t *out,
                                     char **lines, int *linenos, size_t nlines,
                                     size_t *io_idx /* in/out */
)
{
    if_stack_t ifs;
    ifs_init(&ifs);

    while (*io_idx < nlines) {
        size_t i = *io_idx;
        const char *line = lines[i] ? lines[i] : "";
        int src_line = linenos[i];

        *io_idx = i + 1;

        /* -------- directive? -------- */
        if (is_directive_line(line)) {
            char name[64];
            const char *args = NULL;
            directive_name(line, name, sizeof(name), &args);

            if (strcmp(name, "endif") == 0) {
                if (ifs.n == 0) {
                    set_errf(cur_path, src_line, "unexpected %%endif");
                    ifs_free(&ifs);
                    return CFG_ERR_PARSE;
                }
                ifs_pop(&ifs);
                continue;
            }

            if (strcmp(name, "elif") == 0 || strcmp(name, "else") == 0) {
                if_frame_t *f = ifs_top(&ifs);
                if (!f) {
                    set_errf(cur_path, src_line, "unexpected %%%s", name);
                    ifs_free(&ifs);
                    return CFG_ERR_PARSE;
                }

                if (strcmp(name, "else") == 0) {
                    if (!f->parent_active) {
                        f->this_active = 0;
                    }
                    else {
                        f->this_active = f->any_taken ? 0 : 1;
                        f->any_taken = 1;
                    }
                } else {
                    if (!f->parent_active) {
                        f->this_active = 0;
                    }
                    else if (f->any_taken)
                        f->this_active = 0;
                    else {
                        char *e = expand_macros_dup(vars, args);
                        if (!e) {
                            ifs_free(&ifs);
                            return CFG_ERR_OOM;
                        }
                        char emsg[256] = {0};
                        int cond = eval_bool_expr_strict(vars, e, emsg, sizeof(emsg));
                        free(e);
                        /* strict: expression error should be fatal */
                        if (emsg[0]) {
                            set_errf(cur_path, src_line, "in %%elif: %s", emsg);
                            ifs_free(&ifs);
                            return CFG_ERR_PARSE;
                        }
                        f->this_active = cond ? 1 : 0;
                        if (cond) {
                            f->any_taken = 1;
                        }
                    }
                }
                continue;
            }

            if (strcmp(name, "if") == 0) {
                int parent = current_active(&ifs);
                int cond = 0;
                if (parent) {
                    char *e = expand_macros_dup(vars, args);
                    if (!e) {
                        ifs_free(&ifs);
                        return CFG_ERR_OOM;
                    }
                    char emsg[256] = {0};
                    cond = eval_bool_expr_strict(vars, e, emsg, sizeof(emsg));
                    free(e);
                    if (emsg[0]) {
                        set_errf(cur_path, src_line, "in %%if: %s", emsg);
                        ifs_free(&ifs);
                        return CFG_ERR_PARSE;
                    }
                }
                if_frame_t fr = {parent, (parent && cond) ? 1 : 0, (parent && cond) ? 1 : 0};
                if (!ifs_push(&ifs, fr)) {
                    ifs_free(&ifs);
                    return CFG_ERR_OOM;
                }
                continue;
            }

            if (strcmp(name, "define") == 0) {
                if (current_active(&ifs)) {
                    if (!handle_define(vars, args)) {
                        ifs_free(&ifs);
                        return CFG_ERR_OOM;
                    }
                }
                continue;
            }

            if (strcmp(name, "undef") == 0) {
                if (current_active(&ifs))
                    handle_undef(vars, args);
                continue;
            }

            if (strcmp(name, "set") == 0) {
                if (current_active(&ifs)) {
                    char emsg[256] = {0};
                    if (!handle_set(vars, args, emsg, sizeof(emsg))) {
                        if (emsg[0]) {
                            set_errf(cur_path, src_line, "in %%set: %s", emsg);
                        }
                        else
                            set_errf(cur_path, src_line, "in %%set: failed");
                        ifs_free(&ifs);
                        return CFG_ERR_PARSE;
                    }
                }
                continue;
            }

            if (strcmp(name, "include") == 0) {
                if (current_active(&ifs)) {
                    if (depth >= CFG_INCLUDE_DEPTH) {
                        set_errf(cur_path, src_line, "include recursion too deep");
                        ifs_free(&ifs);
                        return CFG_ERR_RECURSION;
                    }

                    char *inc = parse_include_target(args);

                    if (!inc) {
                        set_errf(cur_path, src_line, "bad %%include syntax");
                        ifs_free(&ifs);
                        return CFG_ERR_PARSE;
                    }

                    char *dir = path_dirname(cur_path);
                    char *full = path_join(dir ? dir : ".", inc);
                    free(dir);
                    free(inc);

                    if (!full) {
                        ifs_free(&ifs);
                        return CFG_ERR_OOM;
                    }

                    cfg_status_t st = preprocess_file(full, depth + 1, vars, out);
                    free(full);
                    if (st != CFG_OK) {
                        ifs_free(&ifs);
                        return st;
                    }
                }
                continue;
            }

            if (strcmp(name, "for") == 0) {
                /* If inactive: skip block to matching endfor, respecting nesting */
                if (!current_active(&ifs)) {
                    int nest = 1;

                    while (*io_idx < nlines) {
                        const char *l2 = lines[*io_idx] ? lines[*io_idx] : "";
                        int ln2 = linenos[*io_idx];
                        (void)ln2;
                        (*io_idx)++;
                        if (is_directive_line(l2)) {
                            char n2[64];
                            const char *a2 = NULL;
                            directive_name(l2, n2, sizeof(n2), &a2);
                            if (strcmp(n2, "for") == 0)
                                nest++;
                            else if (strcmp(n2, "endfor") == 0) {
                                nest--;
                                if (nest == 0) {
                                    break;
                                }
                            }
                        }
                    }

                    if (nest != 0) {
                        set_errf(cur_path, src_line, "unterminated %%for block");
                        ifs_free(&ifs);
                        return CFG_ERR_PARSE;
                    }
                    continue;
                }

                /* Active for: capture until matching endfor (nested ok), then replay recursively
                 * for each item */
                char *var = NULL;
                char **items = NULL;
                size_t nitems = 0;

                if (!parse_for(vars, args, &var, &items, &nitems)) {
                    set_errf(cur_path, src_line, "bad %%for syntax");
                    ifs_free(&ifs);
                    return CFG_ERR_PARSE;
                }

                size_t block_start = *io_idx;
                int nest = 1;

                while (*io_idx < nlines) {
                    const char *l2 = lines[*io_idx] ? lines[*io_idx] : "";
                    int ln2 = linenos[*io_idx];
                    (void)ln2;
                    if (is_directive_line(l2)) {
                        char n2[64];
                        const char *a2 = NULL;
                        directive_name(l2, n2, sizeof(n2), &a2);
                        if (strcmp(n2, "for") == 0)
                            nest++;
                        else if (strcmp(n2, "endfor") == 0) {
                            nest--;
                            if (nest == 0) {
                                break;
                            }
                        }
                    }
                    (*io_idx)++;
                }

                if (*io_idx >= nlines) {
                    set_errf(cur_path, src_line, "unterminated %%for block");
                    free(var);
                    free_items(items, nitems);
                    ifs_free(&ifs);
                    return CFG_ERR_PARSE;
                }
                size_t block_end = *io_idx; /* points to line of %endfor */
                (*io_idx)++;                /* consume %endfor */

                const char *old = map_get(vars, var);
                char *old_copy = old ? xstrdup(old) : NULL;

                for (size_t it = 0; it < nitems; it++) {
                    if (!map_set(vars, var, items[it])) {
                        free(old_copy);
                        free(var);
                        free_items(items, nitems);
                        ifs_free(&ifs);
                        return CFG_ERR_OOM;
                    }
                    size_t sub_idx = block_start;
                    cfg_status_t st = preprocess_block(cur_path, depth, vars, out, lines, linenos,
                                                       block_end, &sub_idx);
                    if (st != CFG_OK) {
                        if (old_copy) {
                            map_set(vars, var, old_copy);
                        }
                        else
                            map_unset(vars, var);
                        free(old_copy);
                        free(var);
                        free_items(items, nitems);
                        ifs_free(&ifs);
                        return st;
                    }
                }

                if (old_copy) {
                    map_set(vars, var, old_copy);
                    free(old_copy);
                } else
                    map_unset(vars, var);

                free(var);
                free_items(items, nitems);
                continue;
            }

            if (strcmp(name, "endfor") == 0) {
                set_errf(cur_path, src_line, "unexpected %%endfor");
                ifs_free(&ifs);
                return CFG_ERR_PARSE;
            }

            /* unknown directive: strict? 这里我选择“忽略未知指令”，如果你要严格报错可以改成
             * CFG_ERR_PARSE */
            continue;
        }

        /* -------- normal line -------- */
        if (current_active(&ifs)) {
            sbuf_t exp;
            sbuf_init(&exp);

            if (!expand_macros_into(&exp, vars, line)) {
                sbuf_free(&exp);
                ifs_free(&ifs);
                return CFG_ERR_OOM;
            }
            if (!outbuf_add_line(out, exp.buf ? exp.buf : "", cur_path, src_line)) {
                sbuf_free(&exp);
                ifs_free(&ifs);
                return CFG_ERR_OOM;
            }
            sbuf_free(&exp);
        }
    }

    if (ifs.n != 0) {
        /* missing endif */
        set_errf(cur_path, linenos[nlines ? (nlines - 1) : 0], "missing %%endif");
        ifs_free(&ifs);
        return CFG_ERR_PARSE;
    }

    ifs_free(&ifs);

    return CFG_OK;
}

static cfg_status_t preprocess_file(const char *path, int depth, map_t *vars, outbuf_t *out)
{
    file_lines_t fl;
    cfg_status_t st = load_file_lines(path, &fl);

    if (st != CFG_OK) {
        if (st == CFG_ERR_IO) {
            set_errf(path, 0, "cannot open file");
        }
        file_lines_free(&fl);
        return st;
    }

    /* intern file path early */
    if (!outbuf_intern_file(out, path)) {
        file_lines_free(&fl);
        return CFG_ERR_OOM;
    }

    size_t idx = 0;
    st = preprocess_block(path, depth, vars, out, fl.lines, fl.linenos, fl.n, &idx);
    file_lines_free(&fl);

    return st;
}

/* =========================================================
 * KV config storage & parser
 * ========================================================= */
typedef enum { CVT_STR, CVT_INT, CVT_BOOL } cval_type_t;

typedef struct {
    char *key;
    char *sval;
    cval_type_t type;
    long long ival;
    int bval;
    char *file;
    int line;
} cfg_entry_t;

struct cfg {
    cfg_entry_t *a;
    size_t n, cap;
};

static void entry_free(cfg_entry_t *e)
{
    free(e->key);
    free(e->sval);
    free(e->file);
    memset(e, 0, sizeof(*e));
}

static int cfg_find(const cfg_t *c, const char *key)
{
    for (size_t i = 0; i < c->n; i++)
        if (strcmp(c->a[i].key, key) == 0)
            return (int)i;
    return -1;
}

static int cfg_upsert(cfg_t *c, const char *key, const char *sval, const char *file, int line)
{
    int idx = cfg_find(c, key);

    if (idx >= 0) {
        char *nv = xstrdup(sval);
        if (!nv) {
            return 0;
        }

        free(c->a[idx].sval);
        c->a[idx].sval = nv;
        free(c->a[idx].file);
        c->a[idx].file = xstrdup(file ? file : "<unknown>");

        if (!c->a[idx].file) {
            return 0;
        }

        c->a[idx].line = line;

        if (strcasecmp_local(nv, "true") == 0 || strcmp(nv, "1") == 0 ||
            strcasecmp_local(nv, "yes") == 0 || strcasecmp_local(nv, "on") == 0) {
            c->a[idx].type = CVT_BOOL;
            c->a[idx].bval = 1;
        } else if (strcasecmp_local(nv, "false") == 0 || strcmp(nv, "0") == 0 ||
                   strcasecmp_local(nv, "no") == 0 || strcasecmp_local(nv, "off") == 0) {
            c->a[idx].type = CVT_BOOL;
            c->a[idx].bval = 0;
        } else if (is_num_str(nv)) {
            c->a[idx].type = CVT_INT;
            c->a[idx].ival = strtoll(nv, NULL, 10);
        } else {
            c->a[idx].type = CVT_STR;
        }

        return 1;
    }

    if (c->n == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 128;
        cfg_entry_t *na = (cfg_entry_t *)xrealloc(c->a, nc * sizeof(cfg_entry_t));
        if (!na) {
            return 0;
        }
        c->a = na;
        c->cap = nc;
    }

    cfg_entry_t *e = &c->a[c->n];
    memset(e, 0, sizeof(*e));
    e->key = xstrdup(key);
    e->sval = xstrdup(sval);

    if (!e->key || !e->sval) {
        return 0;
    }

    e->file = xstrdup(file ? file : "<unknown>");

    if (!e->file) {
        return 0;
    }

    e->line = line;

    if (strcasecmp_local(e->sval, "true") == 0 || strcmp(e->sval, "1") == 0 ||
        strcasecmp_local(e->sval, "yes") == 0 || strcasecmp_local(e->sval, "on") == 0) {
        e->type = CVT_BOOL;
        e->bval = 1;
    } else if (strcasecmp_local(e->sval, "false") == 0 || strcmp(e->sval, "0") == 0 ||
               strcasecmp_local(e->sval, "no") == 0 || strcasecmp_local(e->sval, "off") == 0) {
        e->type = CVT_BOOL;
        e->bval = 0;
    } else if (is_num_str(e->sval)) {
        e->type = CVT_INT;
        e->ival = strtoll(e->sval, NULL, 10);
    } else {
        e->type = CVT_STR;
    }

    c->n++;

    return 1;
}

/* parse "key = value" (or ':') ; supports inline comments # ; */
static int parse_kv_line(const char *in, char *out_key, size_t ksz, char *out_val, size_t vsz)
{
    const char *p = in;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (!*p) {
        return 0;
    }
    if (*p == '#' || *p == ';') {
        return 0;
    }

    const char *eq = strchr(p, '=');
    const char *co = strchr(p, ':');
    const char *sep = NULL;

    if (eq && co) {
        sep = (eq < co) ? eq : co;
    }
    else
        sep = eq ? eq : co;
    if (!sep) {
        return 0;
    }

    const char *k0 = p;
    const char *k1 = sep;

    while (k1 > k0 && isspace((unsigned char)k1[-1]))
        k1--;
    size_t kn = (size_t)(k1 - k0);
    if (!kn) {
        return 0;
    }
    if (kn >= ksz) {
        kn = ksz - 1;
    }

    memcpy(out_key, k0, kn);
    out_key[kn] = 0;

    const char *v0 = sep + 1;

    while (*v0 && isspace((unsigned char)*v0))
        v0++;

    const char *v1 = v0 + strlen(v0);

    while (v1 > v0 && isspace((unsigned char)v1[-1]))
        v1--;

    if (v0 < v1 && (*v0 == '"' || *v0 == '\'')) {
        char q = *v0;
        v0++;
        const char *qend = v0;
        while (*qend && *qend != q) {
            if (*qend == '\\' && qend[1]) {
                qend++;
            }
            qend++;
        }
        v1 = qend;
    } else {
        const char *cmt = NULL;
        for (const char *t = v0; *t; t++) {
            if (*t == '#' || *t == ';') {
                cmt = t;
                break;
            }
        }
        if (cmt) {
            v1 = cmt;
            while (v1 > v0 && isspace((unsigned char)v1[-1]))
                v1--;
        }
    }

    size_t vn = (size_t)(v1 - v0);
    if (vn >= vsz) {
        vn = vsz - 1;
    }
    memcpy(out_val, v0, vn);
    out_val[vn] = 0;

    return 1;
}

static cfg_status_t parse_kv(outbuf_t *pp, cfg_t *cfg)
{
    char key[512];
    char val[CFG_MAX_LINE];

    for (size_t i = 0; i < pp->n; i++) {
        const char *line = pp->lines[i];
        if (!line) {
            continue;
        }

        char tmp[CFG_MAX_LINE];
        strncpy(tmp, line, sizeof(tmp));
        tmp[sizeof(tmp) - 1] = 0;

        if (!parse_kv_line(tmp, key, sizeof(key), val, sizeof(val)))
            continue;

        /* origin is interned file in outbuf; keep pointer */
        if (!cfg_upsert(cfg, key, val, pp->orig[i].file, pp->orig[i].line))
            return CFG_ERR_OOM;
    }

    return CFG_OK;
}

/* =========================================================
 * public API
 * ========================================================= */
cfg_t *cfg_load(const char *path, cfg_status_t *out_status)
{
    g_last_err[0] = 0;

    if (out_status) {
        *out_status = CFG_OK;
    }

    if (!path) {
        if (out_status) {
            *out_status = CFG_ERR_IO;
        }
        set_errf(NULL, 0, "path is NULL");
        return NULL;
    }

    map_t vars;
    map_init(&vars);
    outbuf_t pp;
    outbuf_init(&pp);

    cfg_status_t st = preprocess_file(path, 0, &vars, &pp);
    map_free(&vars);

    if (st != CFG_OK) {
        outbuf_free(&pp);
        if (out_status) {
            *out_status = st;
        }
        if (!g_last_err[0]) {
            set_errf(path, 0, "preprocess failed (%d)", (int)st);
        }
        return NULL;
    }

    cfg_t *c = (cfg_t *)xmalloc(sizeof(cfg_t));

    if (!c) {
        outbuf_free(&pp);
        if (out_status) {
            *out_status = CFG_ERR_OOM;
        }
        set_errf(path, 0, "out of memory");
        return NULL;
    }
    memset(c, 0, sizeof(*c));

    st = parse_kv(&pp, c);
    outbuf_free(&pp);

    if (st != CFG_OK) {
        cfg_free(c);
        if (out_status) {
            *out_status = st;
        }
        if (!g_last_err[0]) {
            set_errf(path, 0, "parse failed (%d)", (int)st);
        }
        return NULL;
    }

    return c;
}

void cfg_free(cfg_t *c)
{
    if (!c) {
        return;
    }

    for (size_t i = 0; i < c->n; i++)
        entry_free(&c->a[i]);

    free(c->a);
    free(c);
}

int cfg_has(const cfg_t *c, const char *key)
{
    if (!c || !key) {
        return 0;
    }

    return cfg_find(c, key) >= 0;
}

const char *cfg_get_str(const cfg_t *c, const char *key, const char *defval)
{
    if (!c || !key) {
        return defval;
    }

    int idx = cfg_find(c, key);

    if (idx < 0) {
        return defval;
    }

    return c->a[idx].sval ? c->a[idx].sval : defval;
}

long long cfg_get_int(const cfg_t *c, const char *key, long long defval)
{
    if (!c || !key) {
        return defval;
    }

    int idx = cfg_find(c, key);

    if (idx < 0) {
        return defval;
    }

    const cfg_entry_t *e = &c->a[idx];

    if (e->type == CVT_INT) {
        return e->ival;
    }

    if (e->type == CVT_BOOL) {
        return e->bval ? 1 : 0;
    }

    if (e->sval && is_num_str(e->sval))
        return strtoll(e->sval, NULL, 10);

    return defval;
}

int cfg_get_bool(const cfg_t *c, const char *key, int defval)
{
    if (!c || !key) {
        return defval;
    }

    int idx = cfg_find(c, key);

    if (idx < 0) {
        return defval;
    }

    const cfg_entry_t *e = &c->a[idx];

    if (e->type == CVT_BOOL) {
        return e->bval;
    }

    if (e->type == CVT_INT) {
        return (e->ival != 0);
    }

    if (!e->sval) {
        return defval;
    }

    if (strcasecmp_local(e->sval, "true") == 0 || strcmp(e->sval, "1") == 0 ||
        strcasecmp_local(e->sval, "yes") == 0 || strcasecmp_local(e->sval, "on") == 0)
        return 1;

    if (strcasecmp_local(e->sval, "false") == 0 || strcmp(e->sval, "0") == 0 ||
        strcasecmp_local(e->sval, "no") == 0 || strcasecmp_local(e->sval, "off") == 0)
        return 0;

    return defval;
}

void cfg_dump(const cfg_t *c)
{
    if (!c) {
        return;
    }

    for (size_t i = 0; i < c->n; i++) {
        const cfg_entry_t *e = &c->a[i];
        printf("%s = %s    (from %s:%d)\n", e->key, e->sval ? e->sval : "",
               e->file ? e->file : "<unknown>", e->line);
    }
}

int cfg_get_origin(const cfg_t *c, const char *key, const char **out_file, int *out_line)
{
    if (out_file) {
        *out_file = NULL;
    }

    if (out_line) {
        *out_line = 0;
    }

    if (!c || !key) {
        return 0;
    }
    int idx = cfg_find(c, key);

    if (idx < 0) {
        return 0;
    }

    if (out_file) {
        *out_file = c->a[idx].file;
    }

    if (out_line) {
        *out_line = c->a[idx].line;
    }
    return 1;
}

cfg_status_t cfg_dump_preprocessed_text(const char *in_path, char **out_text, size_t *out_len,
                                        int with_origin)
{
    if (!in_path || !out_text) {
        return CFG_ERR_PARSE;
    }

    g_last_err[0] = 0;

    map_t vars;
    map_init(&vars);
    outbuf_t pp;
    outbuf_init(&pp);

    cfg_status_t st = preprocess_file(in_path, 0, &vars, &pp);
    map_free(&vars);

    if (st != CFG_OK) {
        outbuf_free(&pp);
        if (!g_last_err[0]) {
            set_errf(in_path, 0, "preprocess failed (%d)", (int)st);
        }
        return st;
    }

    st = outbuf_to_text(&pp, out_text, out_len, with_origin);
    outbuf_free(&pp);

    if (st != CFG_OK) {
        if (!g_last_err[0]) {
            set_errf(in_path, 0, "dump preprocess text failed (%d)", (int)st);
        }
        return st;
    }
    return CFG_OK;
}

cfg_status_t cfg_dump_preprocessed_file(const char *in_path, const char *out_path, int with_origin)
{
    if (!in_path || !out_path) {
        return CFG_ERR_PARSE;
    }

    char *text = NULL;
    size_t len = 0;

    cfg_status_t st = cfg_dump_preprocessed_text(in_path, &text, &len, with_origin);
    if (st != CFG_OK) {
        return st;
    }

    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        free(text);
        set_errf(out_path, 0, "cannot open output file");
        return CFG_ERR_IO;
    }

    if (len && fwrite(text, 1, len, fp) != len) {
        fclose(fp);
        free(text);
        set_errf(out_path, 0, "write failed");
        return CFG_ERR_IO;
    }

    fclose(fp);
    free(text);
    return CFG_OK;
}
