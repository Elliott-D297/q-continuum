/* qjson.c -- a small JSON reader (see qjson.h). Recursive descent; nesting limited to 64 levels. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qjson.h"

typedef struct { const char *s, *p; char *err; int cap; int depth; } P;

static void fail(P *q, const char *what)
{
    if (q->err && q->cap > 0 && !q->err[0]) snprintf(q->err, (size_t)q->cap, "%s at character %ld", what, (long)(q->p - q->s));
}

static void ws(P *q) { while (*q->p == ' ' || *q->p == '\t' || *q->p == '\n' || *q->p == '\r') q->p++; }

static QJ *node(QjType t) { QJ *j = calloc(1, sizeof *j); if (j) j->type = t; return j; }

static void put_utf8(char **o, unsigned c)
{
    char *d = *o;
    if (c < 0x80) *d++ = (char)c;
    else if (c < 0x800) { *d++ = (char)(0xC0 | (c >> 6)); *d++ = (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { *d++ = (char)(0xE0 | (c >> 12)); *d++ = (char)(0x80 | ((c >> 6) & 0x3F)); *d++ = (char)(0x80 | (c & 0x3F)); }
    else { *d++ = (char)(0xF0 | (c >> 18)); *d++ = (char)(0x80 | ((c >> 12) & 0x3F)); *d++ = (char)(0x80 | ((c >> 6) & 0x3F)); *d++ = (char)(0x80 | (c & 0x3F)); }
    *o = d;
}

static int hex4(const char *h)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = h[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return -1;
    }
    return v;
}

static char *string(P *q)
{
    if (*q->p != '"') { fail(q, "a string expected"); return NULL; }
    const char *b = ++q->p;
    size_t n = 0;
    while (b[n] && b[n] != '"') { if (b[n] == '\\' && b[n + 1]) n++; n++; }
    if (b[n] != '"') { fail(q, "an unterminated string"); return NULL; }
    char *out = malloc(n * 2 + 4), *o = out;          /* (an escape never grows past 4 bytes from its 6) */
    if (!out) return NULL;
    while (*q->p != '"') {
        char c = *q->p++;
        if (c != '\\') { *o++ = c; continue; }
        char e = *q->p++;
        switch (e) {
        case 'n': *o++ = '\n'; break;  case 't': *o++ = '\t'; break;  case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;  case 'f': *o++ = '\f'; break;
        case 'u': {
            int v = hex4(q->p);
            if (v < 0) { fail(q, "a bad \\u escape"); free(out); return NULL; }
            q->p += 4;
            unsigned cp = (unsigned)v;
            if (cp >= 0xD800 && cp < 0xDC00 && q->p[0] == '\\' && q->p[1] == 'u') {   /* a surrogate pair */
                int lo = hex4(q->p + 2);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (unsigned)(lo - 0xDC00); q->p += 6; }
            }
            put_utf8(&o, cp);
            break;
        }
        default: *o++ = e;                             /* \" \\ \/ */
        }
    }
    q->p++;
    *o = 0;
    return out;
}

static QJ *value(P *q);

static QJ *container(P *q, bool obj)
{
    QJ *j = node(obj ? QJ_OBJ : QJ_ARR), *last = NULL;
    if (!j) return NULL;
    q->p++;
    ws(q);
    if (*q->p == (obj ? '}' : ']')) { q->p++; return j; }
    for (;;) {
        char *key = NULL;
        if (obj) {
            ws(q);
            key = string(q);
            if (!key) { qj_free(j); return NULL; }
            ws(q);
            if (*q->p != ':') { fail(q, "':' expected"); free(key); qj_free(j); return NULL; }
            q->p++;
        }
        QJ *v = value(q);
        if (!v) { free(key); qj_free(j); return NULL; }
        v->key = key;
        if (last) last->next = v; else j->child = v;
        last = v;
        j->n++;
        ws(q);
        if (*q->p == ',') { q->p++; continue; }
        if (*q->p == (obj ? '}' : ']')) { q->p++; return j; }
        fail(q, obj ? "',' or '}' expected" : "',' or ']' expected");
        qj_free(j);
        return NULL;
    }
}

static QJ *value(P *q)
{
    ws(q);
    if (++q->depth > 64) { fail(q, "nested too deep"); return NULL; }
    QJ *j = NULL;
    char c = *q->p;
    if (c == '{' || c == '[') j = container(q, c == '{');
    else if (c == '"') { char *s = string(q); if (s) { j = node(QJ_STR); if (j) j->str = s; else free(s); } }
    else if (!strncmp(q->p, "true", 4)) { q->p += 4; j = node(QJ_BOOL); if (j) j->num = 1; }
    else if (!strncmp(q->p, "false", 5)) { q->p += 5; j = node(QJ_BOOL); }
    else if (!strncmp(q->p, "null", 4)) { q->p += 4; j = node(QJ_NULL); }
    else if (c == '-' || (c >= '0' && c <= '9')) {
        char *e; double d = strtod(q->p, &e);
        if (e == q->p) fail(q, "a bad number");
        else { q->p = e; j = node(QJ_NUM); if (j) j->num = d; }
    } else if (!strncmp(q->p, "NaN", 3) || !strncmp(q->p, "Infinity", 8) || !strncmp(q->p, "-Infinity", 9)) {
        /* Python's json.dumps writes these: read them as numbers rather than fail the whole answer */
        int k = q->p[0] == 'N' ? 3 : q->p[0] == 'I' ? 8 : 9;
        double d = q->p[0] == 'N' ? 0.0 / 0.0 : q->p[0] == 'I' ? 1e308 * 10 : -1e308 * 10;
        q->p += k; j = node(QJ_NUM); if (j) j->num = d;
    } else fail(q, "a value expected");
    q->depth--;
    return j;
}

QJ *qj_parse(const char *text, char *err, int cap)
{
    if (err && cap > 0) err[0] = 0;
    if (!text) { if (err) snprintf(err, (size_t)cap, "no text"); return NULL; }
    P q = { text, text, err, cap, 0 };
    QJ *j = value(&q);
    if (!j) return NULL;
    ws(&q);
    if (*q.p) { fail(&q, "text after the value"); qj_free(j); return NULL; }
    return j;
}

void qj_free(QJ *j)
{
    while (j) {
        QJ *n = j->next;
        qj_free(j->child);
        free(j->str); free(j->key);
        free(j);
        j = n;
    }
}

const QJ *qj_get(const QJ *obj, const char *key)
{
    if (!obj || obj->type != QJ_OBJ || !key) return NULL;
    for (const QJ *c = obj->child; c; c = c->next) if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

const QJ *qj_at(const QJ *arr, int i)
{
    if (!arr || (arr->type != QJ_ARR && arr->type != QJ_OBJ) || i < 0) return NULL;
    const QJ *c = arr->child;
    while (c && i-- > 0) c = c->next;
    return c;
}

double qj_num(const QJ *j, double def) { return j && (j->type == QJ_NUM || j->type == QJ_BOOL) ? j->num : def; }
const char *qj_str(const QJ *j, const char *def) { return j && j->type == QJ_STR ? j->str : def; }
bool qj_true(const QJ *j) { return j && (j->type == QJ_BOOL || j->type == QJ_NUM) && j->num != 0; }
int qj_len(const QJ *j) { return j && (j->type == QJ_ARR || j->type == QJ_OBJ) ? j->n : 0; }
