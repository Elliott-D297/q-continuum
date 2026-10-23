/* xmlspan.c -- span-keeping XML reader and editor (see xmlspan.h) */
#include "xmlspan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void node_free(XNode *n)
{
    for (int i = 0; i < n->nChild; i++) node_free(&n->child[i]);
    free(n->child);
    n->child = NULL; n->nChild = n->capChild = 0;
}

void xml_free(XTree *t) { node_free(&t->root); memset(t, 0, sizeof *t); }

static XNode *add_child(XNode *p)
{
    if (p->nChild == p->capChild) {
        p->capChild = p->capChild ? p->capChild * 2 : 8;
        p->child = realloc(p->child, sizeof(XNode) * (size_t)p->capChild);
    }
    XNode *c = &p->child[p->nChild++];
    memset(c, 0, sizeof *c);
    c->closeStart = -1;
    return c;
}

static bool name_ch(char c) { return isalnum((unsigned char)c) || c == ':' || c == '-' || c == '_' || c == '.'; }

/* children are stored by value in a growing array, so the open-element stack holds index paths */
#define MAX_DEPTH 64
static XNode *walk(XNode *root, const int *path, int depth)
{
    XNode *n = root;
    for (int i = 0; i < depth; i++) n = &n->child[path[i]];
    return n;
}

bool xml_parse(const char *s, int len, XTree *t)
{
    xml_free(t);
    t->root.closeStart = -1;
    t->root.start = 0; t->root.end = len;
    int path[MAX_DEPTH], depth = 0;
    int i = 0;
#define FAIL(...) do { snprintf(t->err, sizeof t->err, __VA_ARGS__); t->ok = false; return false; } while (0)
    while (i < len) {
        if (s[i] != '<') { i++; continue; }
        if (!strncmp(s + i, "<!--", 4)) { const char *e = strstr(s + i + 4, "-->"); i = e ? (int)(e - s) + 3 : len; continue; }
        if (!strncmp(s + i, "<?", 2)) { const char *e = strstr(s + i + 2, "?>"); i = e ? (int)(e - s) + 2 : len; continue; }
        if (!strncmp(s + i, "<![CDATA[", 9)) { const char *e = strstr(s + i + 9, "]]>"); i = e ? (int)(e - s) + 3 : len; continue; }
        if (!strncmp(s + i, "<!", 2)) { while (i < len && s[i] != '>') i++; i++; continue; }
        if (s[i + 1] == '/') {                                     /* end tag */
            int a = i + 2, b = a;
            while (b < len && name_ch(s[b])) b++;
            if (depth == 0) FAIL("unexpected </%.*s> at byte %d", b - a, s + a, i);
            XNode *n = walk(&t->root, path, depth);
            if ((int)strlen(n->tag) != b - a || strncmp(n->tag, s + a, (size_t)(b - a)))
                FAIL("</%.*s> closes <%s> (byte %d)", b - a, s + a, n->tag, i);
            n->closeStart = i;
            while (b < len && s[b] != '>') b++;
            n->end = b < len ? b + 1 : len;
            depth--;
            i = n->end;
            continue;
        }
        /* start tag */
        XNode *p = walk(&t->root, path, depth);
        int idx = p->nChild;
        XNode *n = add_child(p);
        n->start = i;
        int a = i + 1, b = a;
        while (b < len && name_ch(s[b])) b++;
        snprintf(n->tag, sizeof n->tag, "%.*s", b - a, s + a);
        int q = b;
        for (;;) {
            while (q < len && isspace((unsigned char)s[q])) q++;
            if (q >= len) FAIL("unterminated <%s>", n->tag);
            if (s[q] == '>') { n->openEnd = q; q++; break; }
            if (s[q] == '/' && q + 1 < len && s[q + 1] == '>') { n->openEnd = q; q += 2; n->end = q; break; }
            int na = q;
            while (q < len && name_ch(s[q])) q++;
            if (q == na) FAIL("bad character '%c' in <%s> (byte %d)", s[q], n->tag, q);
            int ne = q;
            while (q < len && isspace((unsigned char)s[q])) q++;
            if (s[q] != '=') FAIL("attribute %.*s of <%s> has no value", ne - na, s + na, n->tag);
            q++;
            while (q < len && isspace((unsigned char)s[q])) q++;
            char quote = s[q];
            if (quote != '"' && quote != '\'') FAIL("unquoted attribute value in <%s>", n->tag);
            int vs = ++q;
            while (q < len && s[q] != quote) q++;
            if (n->nAttr < XML_MAX_ATTR) {
                XAttr *at = &n->attr[n->nAttr++];
                snprintf(at->name, sizeof at->name, "%.*s", ne - na, s + na);
                at->vs = vs; at->ve = q;
            }
            q++;
        }
        if (n->end == 0) {                                        /* not self-closing: descend */
            if (depth >= MAX_DEPTH) FAIL("nesting too deep");
            path[depth++] = idx;
        }
        i = q;
    }
    if (depth) FAIL("<%s> is never closed", walk(&t->root, path, depth)->tag);
    t->ok = true;
    t->err[0] = 0;
    return true;
#undef FAIL
}

XNode *xml_child(XNode *n, const char *tag, int nth)
{
    if (!n) return NULL;
    for (int i = 0; i < n->nChild; i++)
        if ((!tag[0] || !strcmp(n->child[i].tag, tag)) && nth-- == 0) return &n->child[i];
    return NULL;
}

int xml_count(const XNode *n, const char *tag)
{
    int k = 0;
    for (int i = 0; n && i < n->nChild; i++) if (!tag[0] || !strcmp(n->child[i].tag, tag)) k++;
    return k;
}

bool xml_tag_is(const XNode *n, const char *prefix) { return n && !strncmp(n->tag, prefix, strlen(prefix)); }

bool xml_attr(const char *text, const XNode *n, const char *name, char *buf, int cap)
{
    if (cap) buf[0] = 0;
    for (int i = 0; n && i < n->nAttr; i++)
        if (!strcmp(n->attr[i].name, name)) { snprintf(buf, (size_t)cap, "%.*s", n->attr[i].ve - n->attr[i].vs, text + n->attr[i].vs); return true; }
    return false;
}

/* ============================================================================ editing */
static bool edit(FDFile *f, XTree *t, int a, int b, const char *txt)
{
    if (!fd_replace_span(f, a, b, txt)) return false;
    xml_parse(f->text, f->len, t);
    return true;
}

static void escape(const char *v, char *out, size_t cap)
{
    size_t k = 0;
    for (; *v && k + 7 < cap; v++) {
        const char *r = *v == '"' ? "&quot;" : *v == '<' ? "&lt;" : *v == '&' ? "&amp;" : NULL;
        if (r) { size_t n = strlen(r); memcpy(out + k, r, n); k += n; } else out[k++] = *v;
    }
    out[k] = 0;
}

bool xml_set_attr(FDFile *f, XTree *t, XNode *n, const char *name, const char *value)
{
    char v[512]; escape(value, v, sizeof v);
    for (int i = 0; i < n->nAttr; i++)
        if (!strcmp(n->attr[i].name, name)) return edit(f, t, n->attr[i].vs, n->attr[i].ve, v);
    char ins[640]; snprintf(ins, sizeof ins, " %s=\"%s\"", name, v);
    int at = n->openEnd;                                          /* before '>' or '/>' */
    while (at > n->start && isspace((unsigned char)f->text[at - 1])) at--;
    return edit(f, t, at, at, ins);
}

static int line_start(const FDFile *f, int p) { while (p > 0 && f->text[p - 1] != '\n') p--; return p; }

static void indent_at(const FDFile *f, int p, char *out, int cap)
{
    int a = line_start(f, p), k = 0;
    while (a < f->len && (f->text[a] == ' ' || f->text[a] == '\t') && k < cap - 1) out[k++] = f->text[a++];
    out[k] = 0;
}

bool xml_append_child(FDFile *f, XTree *t, XNode *parent, const char *elementText)
{
    char ind[64], pind[64];
    indent_at(f, parent->start, pind, sizeof pind);
    if (parent->nChild) indent_at(f, parent->child[parent->nChild - 1].start, ind, sizeof ind);
    else pl_fmt_ui(ind, sizeof ind, "%s  ", pind);
    Str b; str_init(&b);
    if (parent == &t->root || parent->closeStart < 0) { str_free(&b); return false; }   /* self-closing parents: not needed for preCICE */
    /* insert at the start of the closing tag's line when it is alone there, else right before it */
    int at = parent->closeStart, ls = line_start(f, at);
    bool alone = true;
    for (int i = ls; i < at; i++) if (!isspace((unsigned char)f->text[i])) alone = false;
    if (alone) at = ls; else str_cat(&b, "\n");
    for (const char *p = elementText; *p;) {                       /* indent every line of the new element */
        const char *e = strchr(p, '\n');
        str_cat(&b, ind);
        str_catn(&b, p, e ? (size_t)(e - p) : strlen(p));
        str_cat(&b, "\n");
        if (!e) break;
        p = e + 1;
    }
    if (!alone) str_cat(&b, pind);
    bool ok = edit(f, t, at, at, b.s);
    str_free(&b);
    return ok;
}

bool xml_remove(FDFile *f, XTree *t, XNode *n)
{
    int a = n->start, b = n->end, ls = line_start(f, a);
    bool wsBefore = true;
    for (int i = ls; i < a; i++) if (!isspace((unsigned char)f->text[i])) wsBefore = false;
    int q = b;
    while (q < f->len && (f->text[q] == ' ' || f->text[q] == '\t')) q++;
    if (wsBefore && (q >= f->len || f->text[q] == '\n')) { a = ls; b = q < f->len ? q + 1 : q; }
    return edit(f, t, a, b, "");
}

bool xml_rename(FDFile *f, XTree *t, XNode *n, const char *newTag)
{
    /* the end tag first: its offset is unaffected by changing the start tag */
    int ct = n->closeStart;
    size_t ol = strlen(n->tag);
    int st = n->start + 1;
    if (ct >= 0 && !fd_replace_span(f, ct + 2, ct + 2 + (int)ol, newTag)) return false;
    return edit(f, t, st, st + (int)ol, newTag);
}
