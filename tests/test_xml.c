/* test_xml.c -- xmlspan over every preCICE configuration under a directory:
 *   parse; put every attribute value back as itself (bytes unchanged); then on a copy of each file
 *   change max-time, add an exchange, remove it again, rename the coupling scheme and back -- the file
 *   must return to its original bytes and stay valid XML.
 *   test_xml <dir> */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "xmlspan.h"

static int nFiles, nFail;
static void fail(const char *p, const char *w) { printf("FAIL  %s\n      %s\n", p, w); nFail++; }

/* nodes move on every edit (the tree is re-read), so walk by index path and re-resolve after each change */
static XNode *at_path(XTree *t, const int *path, int depth)
{
    XNode *n = &t->root;
    for (int i = 0; i < depth && n; i++) n = i < 64 && path[i] < n->nChild ? &n->child[path[i]] : NULL;
    return n;
}

static void putback(FDFile *f, XTree *t, int *path, int depth, const char *orig, const char *p, bool *bad)
{
    XNode *n = at_path(t, path, depth);
    if (!n) { fail(p, "tree shape changed"); *bad = true; return; }
    int nA = n->nAttr, nC = n->nChild;
    for (int i = 0; i < nA && !*bad; i++) {
        n = at_path(t, path, depth);
        char v[512]; snprintf(v, sizeof v, "%.*s", n->attr[i].ve - n->attr[i].vs, f->text + n->attr[i].vs);
        if (strchr(v, '&') || strchr(v, '"')) continue;            /* escaped values are rewritten escaped */
        char name[64]; snprintf(name, sizeof name, "%s", n->attr[i].name);
        xml_set_attr(f, t, n, name, v);
        if (strcmp(f->text, orig)) { fail(p, "putting an attribute back changed the file"); *bad = true; }
    }
    for (int i = 0; i < nC && !*bad && depth < 63; i++) { path[depth] = i; putback(f, t, path, depth + 1, orig, p, bad); }
}

static void check(const char *path)
{
    FDFile *f = fd_load(path);
    if (!f) return;
    nFiles++;
    char *orig = pl_strdup(f->text);
    XTree t = { 0 };
    if (!xml_parse(f->text, f->len, &t)) { char w[200]; snprintf(w, sizeof w, "parse: %s", t.err); fail(path, w); goto out; }
    bool bad = false;
    int pth[64];
    putback(f, &t, pth, 0, orig, path, &bad);
    if (bad) goto out;
    XNode *cfg = xml_child(&t.root, "precice-configuration", 0), *cs = NULL;
    for (int i = 0; cfg && i < cfg->nChild; i++) if (xml_tag_is(&cfg->child[i], "coupling-scheme:")) cs = &cfg->child[i];
    if (!cs || cs->closeStart < 0) goto out;                     /* multi-coupling etc.: parse + put-back only */
    char tag[64]; snprintf(tag, sizeof tag, "%s", cs->tag);
    XNode *mt = xml_child(cs, "max-time", 0);
    if (mt) {
        char v[64]; xml_attr(f->text, mt, "value", v, sizeof v);
        xml_set_attr(f, &t, mt, "value", "123.5");
        cfg = xml_child(&t.root, "precice-configuration", 0);
        for (int i = 0; i < cfg->nChild; i++) if (xml_tag_is(&cfg->child[i], "coupling-scheme:")) cs = &cfg->child[i];
        char w[64]; xml_attr(f->text, xml_child(cs, "max-time", 0), "value", w, sizeof w);
        if (strcmp(w, "123.5")) { fail(path, "max-time edit not read back"); goto out; }
        xml_set_attr(f, &t, xml_child(cs, "max-time", 0), "value", v);
    }
    cfg = xml_child(&t.root, "precice-configuration", 0);
    for (int i = 0; i < cfg->nChild; i++) if (xml_tag_is(&cfg->child[i], "coupling-scheme:")) cs = &cfg->child[i];
    int ne = xml_count(cs, "exchange");
    xml_append_child(f, &t, cs, "<exchange data=\"X\" mesh=\"M\" from=\"A\" to=\"B\" />");
    cfg = xml_child(&t.root, "precice-configuration", 0);
    for (int i = 0; i < cfg->nChild; i++) if (xml_tag_is(&cfg->child[i], "coupling-scheme:")) cs = &cfg->child[i];
    if (!t.ok || xml_count(cs, "exchange") != ne + 1) { fail(path, "append_child did not add an exchange"); goto out; }
    XNode *last = NULL;
    for (int i = 0; i < cs->nChild; i++) if (!strcmp(cs->child[i].tag, "exchange")) last = &cs->child[i];
    xml_remove(f, &t, last);
    cfg = xml_child(&t.root, "precice-configuration", 0);
    for (int i = 0; i < cfg->nChild; i++) if (xml_tag_is(&cfg->child[i], "coupling-scheme:")) cs = &cfg->child[i];
    xml_rename(f, &t, cs, "coupling-scheme:parallel-implicit");
    if (!t.ok) { fail(path, "rename broke the XML"); goto out; }
    cfg = xml_child(&t.root, "precice-configuration", 0);
    for (int i = 0; i < cfg->nChild; i++) if (xml_tag_is(&cfg->child[i], "coupling-scheme:")) cs = &cfg->child[i];
    xml_rename(f, &t, cs, tag);
    if (strcmp(f->text, orig)) {
        FILE *o = fopen("/tmp/qc-xml-diff.xml", "w"); if (o) { fputs(f->text, o); fclose(o); }
        fail(path, "edit + undo did not return to the original bytes (see /tmp/qc-xml-diff.xml)");
    }
out:
    xml_free(&t);
    free(orig);
    fd_free(f);
}

static void walk(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[PL_PATH]; pl_join(p, sizeof p, dir, de->d_name);
        struct stat st;
        if (lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) walk(p);
        else if (pl_starts_with(de->d_name, "precice-config") && pl_ends_with(de->d_name, ".xml")) check(p);
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <dir>\n", argv[0]); return 2; }
    for (int i = 1; i < argc; i++) walk(argv[i]);
    printf("\n%d preCICE configurations, %d failed\n", nFiles, nFail);
    return nFail ? 1 : 0;
}
