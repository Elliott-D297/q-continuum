/* page_meshadv.h -- the STL mesher's "Every OpenFOAM setting" panel (page_meshadv.c) */
#ifndef PAGE_MESHADV_H
#define PAGE_MESHADV_H
#include "stlmesh.h"
float meshadv_panel(const char *casePath, const MeshCfg *m, Stl *s, float x, float y, float w);   /* its height */
int   meshadv_count(const char *casePath, const MeshCfg *m);   /* the user's values that apply to this mesher */
#endif
