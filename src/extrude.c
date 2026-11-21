/* extrude.c -- extrudeMesh dictionaries (see extrude.h) */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extrude.h"
#include "pl_util.h"

const char *const EX_MODEL[EX__N] = { "linearNormal", "linearDirection", "plane", "wedge", "sector" };
const char *const EX_MODEL_NAME[EX__N] = { "Straight out (along the faces' normals)", "Straight, one direction", "One cell, 2-D (plane)",
                                           "Wedge (axisymmetric)", "Round an axis (sector)" };

static const char *HEAD = "FoamFile\n{\n    version     2.0;\n    format      ascii;\n    class       dictionary;\n    object      %s;\n}\n"
                          "// Written by Q-Continuum (Mesh > Block builder, Extrude)\n\n";

static void vec(Str *o, const double *v) { str_catf(o, "(%.15g %.15g %.15g)", v[0] + 0.0, v[1] + 0.0, v[2] + 0.0); }

char *ex_patch_dict(const ExtrudeCfg *c, char *why, int cap)
{
    if (!c->patch[0]) { snprintf(why, (size_t)cap, "Choose the patch to extrude"); return NULL; }
    if (c->model < 0 || c->model >= EX__N) { snprintf(why, (size_t)cap, "No such extrusion"); return NULL; }
    if (c->nLayers < 1 || c->nLayers > 10000) { snprintf(why, (size_t)cap, "Layers: 1 to 10000"); return NULL; }
    if (!(c->expansion > 0)) { snprintf(why, (size_t)cap, "The expansion ratio must be above 0"); return NULL; }
    bool lin = c->model == EX_LINEAR_NORMAL || c->model == EX_LINEAR_DIRECTION;
    if (lin && !(c->thickness > 0)) { snprintf(why, (size_t)cap, "The thickness must be above 0"); return NULL; }
    if (c->model == EX_LINEAR_DIRECTION && !(fabs(c->dir[0]) + fabs(c->dir[1]) + fabs(c->dir[2]) > 0)) { snprintf(why, (size_t)cap, "Give a direction"); return NULL; }
    if ((c->model == EX_WEDGE || c->model == EX_SECTOR) && !(fabs(c->axis[0]) + fabs(c->axis[1]) + fabs(c->axis[2]) > 0)) { snprintf(why, (size_t)cap, "Give the axis' direction"); return NULL; }
    if ((c->model == EX_WEDGE || c->model == EX_SECTOR) && !(c->angle > 0 && c->angle <= 360)) { snprintf(why, (size_t)cap, "The angle: above 0, at most 360 degrees"); return NULL; }
    Str o; str_init(&o);
    str_catf(&o, HEAD, "extrudeMeshDict");
    str_catf(&o, "constructFrom   mesh;\nsourceCase      \"<case>\";\nsourcePatches   (%s);\nexposedPatchName %s;\n\n", c->patch, c->patch);
    str_catf(&o, "extrudeModel    %s;\nnLayers         %d;\nexpansionRatio  %.15g;\n\n", EX_MODEL[c->model],
             c->model == EX_PLANE || c->model == EX_WEDGE ? 1 : c->nLayers, c->expansion);
    switch (c->model) {
    case EX_LINEAR_NORMAL: str_catf(&o, "linearNormalCoeffs\n{\n    thickness       %.15g;\n}\n", c->thickness); break;
    case EX_LINEAR_DIRECTION: str_cat(&o, "linearDirectionCoeffs\n{\n    direction       "); vec(&o, c->dir); str_catf(&o, ";\n    thickness       %.15g;\n}\n", c->thickness); break;
    case EX_PLANE: str_catf(&o, "planeCoeffs\n{\n    thickness       %.15g;\n}\n", c->thickness > 0 ? c->thickness : 0.01); break;
    default:
        str_cat(&o, "sectorCoeffs\n{\n    point           "); vec(&o, c->point);
        str_cat(&o, ";\n    axis            "); vec(&o, c->axis);
        str_catf(&o, ";\n    angle           %.15g;\n}\n", c->angle);
    }
    str_catf(&o, "\nflipNormals     %s;\nmergeFaces      false;\n", c->flip ? "true" : "false");
    return o.s;
}

bool ex_axi_dicts(const AxiCfg *c, char **topoSet, char **createPatch, char **extrude, char *why, int cap)
{
    *topoSet = *createPatch = *extrude = NULL;
    int k = c->thin, a = c->axisDir;
    if (k < 0 || k > 2 || a < 0 || a > 2 || a == k) { snprintf(why, (size_t)cap, "The axis must lie in the mesh's plane"); return false; }
    int j = 3 - k - a;                                       /* the radial direction */
    if (!c->empties[0]) { snprintf(why, (size_t)cap, "The mesh has no empty patches: it isn't a 2-D mesh"); return false; }
    if (!(c->angle > 0 && c->angle <= 10)) { snprintf(why, (size_t)cap, "The wedge angle: above 0, at most 10 degrees (5 or less is usual)"); return false; }
    double tol = 1e-9 * (fabs(c->hi[j] - c->lo[j]) + 1);
    if (c->lo[j] < c->axisAt - tol && c->hi[j] > c->axisAt + tol) {
        snprintf(why, (size_t)cap, "The mesh runs from %g to %g in %c: it crosses the axis at %g. An axisymmetric mesh lies on one side of its axis (the "
                 "radius from 0 up)", c->lo[j], c->hi[j], "xyz"[j], c->axisAt);
        return false;
    }
    double side = (c->lo[j] + c->hi[j]) / 2 >= c->axisAt ? 1 : -1;
    /* the front: the faces facing -thin; extrudeMesh turns the patch along its outward normal n, so the axis is r x n */
    double r[3] = { 0, 0, 0 }, n[3] = { 0, 0, 0 }, ax[3];
    r[j] = side; n[k] = -1;
    ax[0] = r[1] * n[2] - r[2] * n[1]; ax[1] = r[2] * n[0] - r[0] * n[2]; ax[2] = r[0] * n[1] - r[1] * n[0];
    double pt[3] = { 0, 0, 0 };
    pt[j] = c->axisAt; pt[k] = c->lo[k];
    const char *fr = c->front[0] ? c->front : "front", *bk = c->back[0] ? c->back : "back";
    Str t; str_init(&t);
    str_catf(&t, HEAD, "topoSetDict");
    str_cat(&t, "actions\n(\n");
    for (int pass = 0; pass < 2; pass++) {
        const char *set = pass ? "axiBack" : "axiFront";
        double nn[3] = { 0, 0, 0 }; nn[k] = pass ? 1 : -1;
        char words[256]; pl_strlcpy(words, c->empties, sizeof words);
        bool first = true;
        for (char *w = strtok(words, " "); w; w = strtok(NULL, " ")) {
            str_catf(&t, "    { name %s; type faceSet; action %s; source patchToFace; patch %s; }\n", set, first ? "new" : "add", w);
            first = false;
        }
        str_catf(&t, "    { name %s; type faceSet; action subset; source normalToFace; normal ", set); vec(&t, nn); str_cat(&t, "; cos 0.9; }\n");
    }
    str_cat(&t, ");\n");
    Str p; str_init(&p);
    str_catf(&p, HEAD, "createPatchDict");
    str_catf(&p, "pointSync false;\npatches\n(\n    { name %s; patchInfo { type patch; } constructFrom set; set axiFront; }\n"
                 "    { name %s; patchInfo { type patch; } constructFrom set; set axiBack; }\n);\n", fr, bk);
    Str e; str_init(&e);
    str_catf(&e, HEAD, "extrudeMeshDict");
    str_catf(&e, "// the 2-D mesh turned about the %c axis through %c = %g, %g degrees in all (half either side of its plane)\n", "xyz"[a], "xyz"[j], c->axisAt, c->angle);
    str_catf(&e, "constructFrom   patch;\nsourceCase      \"<case>\";\nsourcePatches   (%s);\nexposedPatchName %s;\n\n", fr, bk);
    str_cat(&e, "extrudeModel    wedge;\nnLayers         1;\nexpansionRatio  1;\n\nsectorCoeffs\n{\n    point           "); vec(&e, pt);
    str_cat(&e, ";\n    axis            "); vec(&e, ax);
    str_catf(&e, ";\n    angle           %.15g;\n}\n\nflipNormals     false;\nmergeFaces      false;\n", c->angle);
    *topoSet = t.s; *createPatch = p.s; *extrude = e.s;
    return true;
}
