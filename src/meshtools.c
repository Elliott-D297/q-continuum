/* meshtools.c -- Mesh > Tools (see meshtools.h) */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "meshtools.h"
#include "foamdict.h"

const char *const MT_NAME[MT__N] = { "Move, scale, turn", "Mirror", "Refine everywhere", "Renumber", "Cell zone from a box",
                                     "Merge patches", "Baffles from a face zone", "Check a surface" };
const char *const MT_WHAT[MT__N] = {
    "transformPoints: a mesh drawn in millimetres made metres (scale 0.001), turned so the flow runs along x, or moved so the origin sits where "
    "you want it. Scale first, then turn, then move.",
    "mirrorMesh: half of a symmetric geometry made whole. The mirror plane's patch goes; the two halves join.",
    "refineMesh -all: every cell split in two along each direction (eight times the cells in 3-D): the next mesh of a mesh study in one step.",
    "renumberMesh: the cells numbered again so neighbours have close numbers. The mesh is the same; the solver's matrices are narrower and "
    "a run is often 10 to 30 % faster.",
    "topoSet: the cells whose centres are in a box, as a cellZone: where a porous medium, a fan's momentum source or a rotating frame (MRF) acts.",
    "createPatch: several patches as one (the faces of a part split by angle, put back as one wall), with the type you give.",
    "createBaffles: the faces of a faceZone (from snappyHexMesh's faceZone, or topoSet) made into two walls back to back: a thin plate or fin "
    "with flow on both sides.",
    "surfaceCheck: an STL's triangles: closed or not, its size, bad triangles. Before meshing a part, or to find why snappyHexMesh leaks." };

bool mt_word_ok(const char *w)
{
    if (!w || !w[0] || isdigit((unsigned char)w[0]) || strlen(w) >= 64) return false;
    for (const char *q = w; *q; q++) if (!isalnum((unsigned char)*q) && *q != '_' && *q != '-' && *q != '.') return false;
    return true;
}

static bool finite3(const double *v) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); }
static void cat(char *b, int cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void cat(char *b, int cap, const char *fmt, ...)
{
    size_t n = strlen(b);
    if (n + 1 >= (size_t)cap) return;
    va_list ap; va_start(ap, fmt); vsnprintf(b + n, (size_t)cap - n, fmt, ap); va_end(ap);
}

#define HDR(obj) "FoamFile { version 2.0; format ascii; class dictionary; object " obj "; }\n// Q-Continuum, Mesh > Tools\n\n"

bool mt_script(const MtIn *in, char *cmd, int ccap, char *dictRel, int dcap, char *dict, int dictCap, char *err, int ecap)
{
    cmd[0] = dictRel[0] = dict[0] = err[0] = 0;
    switch (in->tool) {
    case MT_TRANSFORM: {
        if (!(in->scale > 0) || !isfinite(in->scale) || !finite3(in->rotAxis) || !isfinite(in->rotDeg) || !finite3(in->move)) {
            snprintf(err, (size_t)ecap, "The scale must be above 0 and every number finite"); return false;
        }
        double al = sqrt(in->rotAxis[0] * in->rotAxis[0] + in->rotAxis[1] * in->rotAxis[1] + in->rotAxis[2] * in->rotAxis[2]);
        bool any = false;
        if (fabs(in->scale - 1) > 1e-12) { cat(cmd, ccap, "transformPoints -scale %.10g && ", in->scale); any = true; }
        if (fabs(in->rotDeg) > 1e-12) {
            if (!(al > 0)) { snprintf(err, (size_t)ecap, "Turning needs an axis (not 0 0 0)"); return false; }
            cat(cmd, ccap, "transformPoints -rotate-angle '((%.10g %.10g %.10g) %.10g)' && ", in->rotAxis[0], in->rotAxis[1], in->rotAxis[2], in->rotDeg);
            any = true;
        }
        if (fabs(in->move[0]) + fabs(in->move[1]) + fabs(in->move[2]) > 0) {
            cat(cmd, ccap, "transformPoints -translate '(%.10g %.10g %.10g)' && ", in->move[0], in->move[1], in->move[2]); any = true;
        }
        if (!any) { snprintf(err, (size_t)ecap, "Nothing to do: scale 1, no turn, no move"); return false; }
        cat(cmd, ccap, "echo 'Q-Continuum: the mesh moved, scaled or turned'");
        return true;
    }
    case MT_MIRROR: {
        double nl = sqrt(in->normal[0] * in->normal[0] + in->normal[1] * in->normal[1] + in->normal[2] * in->normal[2]);
        if (!finite3(in->point) || !finite3(in->normal) || !(nl > 0)) { snprintf(err, (size_t)ecap, "The mirror plane needs a point and a normal (not 0 0 0)"); return false; }
        snprintf(dictRel, (size_t)dcap, "system/mirrorMeshDict.qc-tool");
        snprintf(dict, (size_t)dictCap, HDR("mirrorMeshDict") "planeType           pointAndNormal;\n\npointAndNormalDict\n{\n    point   (%.10g %.10g %.10g);\n"
                 "    normal  (%.10g %.10g %.10g);\n}\n\nplaneTolerance      1e-3;\n", in->point[0], in->point[1], in->point[2], in->normal[0], in->normal[1], in->normal[2]);
        snprintf(cmd, (size_t)ccap, "mirrorMesh -overwrite -dict system/mirrorMeshDict.qc-tool");
        return true;
    }
    case MT_REFINE:
        snprintf(cmd, (size_t)ccap, "refineMesh -all -overwrite");
        return true;
    case MT_RENUMBER:
        snprintf(cmd, (size_t)ccap, "renumberMesh -overwrite");
        return true;
    case MT_ZONE: {
        if (!mt_word_ok(in->name)) { snprintf(err, (size_t)ecap, "The zone's name must be one word (letters, digits, _ - .)"); return false; }
        if (!finite3(in->lo) || !finite3(in->hi) || !(in->hi[0] > in->lo[0] && in->hi[1] > in->lo[1] && in->hi[2] > in->lo[2])) {
            snprintf(err, (size_t)ecap, "The box's \"to\" corner must be above its \"from\" corner in x, y and z"); return false;
        }
        snprintf(dictRel, (size_t)dcap, "system/topoSetDict.qc-tool");
        snprintf(dict, (size_t)dictCap, HDR("topoSetDict") "actions\n(\n    {\n        name    %sCells;\n        type    cellSet;\n        action  new;\n"
                 "        source  boxToCell;\n        box     (%.10g %.10g %.10g) (%.10g %.10g %.10g);\n    }\n    {\n        name    %s;\n        type    cellZoneSet;\n"
                 "        action  new;\n        source  setToCellZone;\n        set     %sCells;\n    }\n);\n",
                 in->name, in->lo[0], in->lo[1], in->lo[2], in->hi[0], in->hi[1], in->hi[2], in->name, in->name);
        snprintf(cmd, (size_t)ccap, "topoSet -dict system/topoSetDict.qc-tool");
        return true;
    }
    case MT_MERGE: {
        if (!mt_word_ok(in->name)) { snprintf(err, (size_t)ecap, "The merged patch's name must be one word"); return false; }
        static const char *const TY[] = { "patch", "wall", "symmetryPlane", "symmetry", "empty", "wedge" };
        bool tok = false; for (size_t i = 0; i < sizeof TY / sizeof TY[0]; i++) tok |= !strcmp(in->type, TY[i]);
        if (!tok) { snprintf(err, (size_t)ecap, "The type must be patch, wall, symmetryPlane, symmetry, empty or wedge"); return false; }
        char list[600] = ""; int n = 0;
        char tmp[512]; snprintf(tmp, sizeof tmp, "%s", in->patches);
        for (char *t = strtok(tmp, " ,\t"); t; t = strtok(NULL, " ,\t")) {
            if (!mt_word_ok(t)) { snprintf(err, (size_t)ecap, "\"%s\" isn't a patch name", t); return false; }
            cat(list, sizeof list, "%s%s", n ? " " : "", t); n++;
        }
        if (n < 1) { snprintf(err, (size_t)ecap, "Name the patches to merge"); return false; }
        snprintf(dictRel, (size_t)dcap, "system/createPatchDict.qc-tool");
        snprintf(dict, (size_t)dictCap, HDR("createPatchDict") "pointSync false;\n\npatches\n(\n    {\n        name            %s;\n        patchInfo\n        {\n"
                 "            type            %s;\n        }\n        constructFrom   patches;\n        patches         (%s);\n    }\n);\n", in->name, in->type, list);
        snprintf(cmd, (size_t)ccap, "createPatch -overwrite -dict system/createPatchDict.qc-tool");
        return true;
    }
    case MT_BAFFLES: {
        if (!mt_word_ok(in->zone)) { snprintf(err, (size_t)ecap, "Choose the face zone"); return false; }
        snprintf(dictRel, (size_t)dcap, "system/createBafflesDict.qc-tool");
        snprintf(dict, (size_t)dictCap, HDR("createBafflesDict") "internalFacesOnly true;\n\nbaffles\n{\n    %s\n    {\n        type        faceZone;\n"
                 "        zoneName    %s;\n        patches\n        {\n            master\n            {\n                name    %s_master;\n                type    wall;\n"
                 "            }\n            slave\n            {\n                name    %s_slave;\n                type    wall;\n            }\n        }\n    }\n}\n",
                 in->zone, in->zone, in->zone, in->zone);
        snprintf(cmd, (size_t)ccap, "createBaffles -overwrite -dict system/createBafflesDict.qc-tool");
        return true;
    }
    case MT_SURFACE: {
        if (!in->file[0]) { snprintf(err, (size_t)ecap, "Choose the STL"); return false; }
        char q[PL_PATH + 8]; pl_shell_quote(q, sizeof q, in->file);
        snprintf(cmd, (size_t)ccap, "surfaceCheck %s", q);
        return true;
    }
    }
    snprintf(err, (size_t)ecap, "No such tool");
    return false;
}

/* ---- the boundary file: "N ( name { ... } ... )", read as text */
static const char *skip_ws(const char *p)
{
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (p[0] == '/' && p[1] == '*') { const char *e = strstr(p + 2, "*/"); p = e ? e + 2 : p + strlen(p); continue; }
        return p;
    }
}

/* each patch: its name's span and its dictionary's span; how many */
typedef struct { int ns, ne, ds, de; } PatchSpan;
static int boundary_spans(const char *t, PatchSpan *out, int max)
{
    const char *p = t;
    const char *ff = strstr(t, "FoamFile");                /* past the header */
    if (ff) { const char *b = strchr(ff, '{'); int d = 0; for (p = b; p && *p; p++) { if (*p == '{') d++; else if (*p == '}' && --d == 0) { p++; break; } } if (!p) return 0; }
    p = skip_ws(p);
    while (*p && isdigit((unsigned char)*p)) p++;
    p = skip_ws(p);
    if (*p != '(') return 0;
    p++;
    int n = 0;
    for (;;) {
        p = skip_ws(p);
        if (!*p || *p == ')') break;
        const char *ns = p;
        while (*p && !isspace((unsigned char)*p) && *p != '{') p++;
        const char *ne = p;
        p = skip_ws(p);
        if (*p != '{') return n;
        const char *ds = p; int d = 0;
        for (; *p; p++) { if (*p == '{') d++; else if (*p == '}' && --d == 0) { p++; break; } }
        if (n < max) out[n] = (PatchSpan){ (int)(ns - t), (int)(ne - t), (int)(ds - t), (int)(p - t) };
        n++;
    }
    return n < max ? n : max;
}

int mt_patches(const char *casePath, char names[][64], char types[][32], int max)
{
    char b[PL_PATH]; pl_join(b, sizeof b, casePath, "constant/polyMesh/boundary");
    char *t = pl_read_file(b, NULL);
    if (!t) return 0;
    PatchSpan sp[512]; int n = boundary_spans(t, sp, 512), k = 0;
    for (int i = 0; i < n && k < max; i++) {
        snprintf(names[k], 64, "%.*s", sp[i].ne - sp[i].ns, t + sp[i].ns);
        types[k][0] = 0;
        char blk[4096]; snprintf(blk, sizeof blk, "%.*s", sp[i].de - sp[i].ds, t + sp[i].ds);
        char *ty = strstr(blk, "type");
        while (ty && ty > blk && !isspace((unsigned char)ty[-1]) && ty[-1] != '{') ty = strstr(ty + 4, "type");
        if (ty) sscanf(ty + 4, " %31[^; \t\n]", types[k]);
        k++;
    }
    free(t);
    return k;
}

int mt_face_zones(const char *casePath, char names[][64], int max)
{
    char b[PL_PATH]; pl_join(b, sizeof b, casePath, "constant/polyMesh/faceZones");
    char *t = pl_read_file(b, NULL);
    if (!t) return 0;
    PatchSpan sp[256]; int n = boundary_spans(t, sp, 256), k = 0;   /* the same layout: N ( name { ... } ) (binary lists sit inside the braces) */
    for (int i = 0; i < n && k < max; i++) snprintf(names[k++], 64, "%.*s", sp[i].ne - sp[i].ns, t + sp[i].ns);
    free(t);
    return k;
}

/* the field files of a folder whose boundaryField has the patch: renamed there */
static int rename_in_fields(const char *dir, const char *from, const char *to)
{
    NameList l = { 0 };
    pl_list_dir(dir, &l, false, false);
    int n = 0;
    for (int i = 0; i < l.n; i++) {
        char p[PL_PATH]; pl_join(p, sizeof p, dir, l.names[i]);
        if (!pl_is_file(p) || l.names[i][0] == '.') continue;
        char *h = pl_read_head(p, 4096);
        bool field = h && strstr(h, "Field") && strstr(h, "FoamFile");
        free(h);
        if (!field) continue;
        FDFile *f = fd_load(p);
        if (!f || !f->ok) { fd_free(f); continue; }
        FDEntry *bf = fd_child(&f->root, "boundaryField");
        FDEntry *e = bf ? fd_child(bf, from) : NULL;
        if (e && e->keyEnd - e->keyStart == (int)strlen(from) && !strncmp(f->text + e->keyStart, from, strlen(from))) {
            fd_replace_span(f, e->keyStart, e->keyEnd, to);
            if (fd_save(f)) n++;
        }
        fd_free(f);
    }
    names_free(&l);
    return n;
}

bool mt_rename_patch(const char *casePath, const char *from, const char *to, int *files, char *err, int ecap)
{
    if (files) *files = 0;
    if (!mt_word_ok(to)) { snprintf(err, (size_t)ecap, "\"%s\" isn't a valid name: one word of letters, digits, _ - .", to); return false; }
    char b[PL_PATH]; pl_join(b, sizeof b, casePath, "constant/polyMesh/boundary");
    char *t = pl_read_file(b, NULL);
    if (!t) { snprintf(err, (size_t)ecap, "No mesh: constant/polyMesh/boundary is missing"); return false; }
    PatchSpan sp[512]; int n = boundary_spans(t, sp, 512), at = -1;
    for (int i = 0; i < n; i++) {
        int len = sp[i].ne - sp[i].ns;
        if (len == (int)strlen(to) && !strncmp(t + sp[i].ns, to, (size_t)len)) { free(t); snprintf(err, (size_t)ecap, "There is a patch called %s already", to); return false; }
        if (len == (int)strlen(from) && !strncmp(t + sp[i].ns, from, (size_t)len)) at = i;
    }
    if (at < 0) { free(t); snprintf(err, (size_t)ecap, "No patch called %s", from); return false; }
    size_t L = strlen(t);
    size_t lt = strlen(to), tail = L - (size_t)sp[at].ne;
    char *o = malloc(L + lt + 1);
    if (!o) { free(t); snprintf(err, (size_t)ecap, "Out of memory"); return false; }
    memcpy(o, t, (size_t)sp[at].ns); memcpy(o + sp[at].ns, to, lt); memcpy(o + sp[at].ns + lt, t + sp[at].ne, tail + 1);  // NOLINT(bugprone-not-null-terminated-result): false positive, the last copy takes the tail with its NUL
    bool ok = pl_write_file(b, o, strlen(o));
    free(o); free(t);
    if (!ok) { snprintf(err, (size_t)ecap, "Could not write constant/polyMesh/boundary"); return false; }
    int nf = 1;
    static const char *const DIRS[] = { "0", "0.orig" };
    for (int i = 0; i < 2; i++) { char d[PL_PATH]; pl_join(d, sizeof d, casePath, DIRS[i]); if (pl_is_dir(d)) nf += rename_in_fields(d, from, to); }
    if (files) *files = nf;
    return true;
}
