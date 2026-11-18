/* meshexamples.h -- the meshing tutorial's example parts (meshexamples.c) */
#ifndef MESHEXAMPLES_H
#define MESHEXAMPLES_H
#include <stdbool.h>

enum { MESH_EX_DUCT = 0, MESH_EX_SPHERE, MESH_EX_AIRFOIL, MESH_EX__N };
typedef struct {
    const char *word;        /* duct, sphere, airfoil                     */
    const char *caseName;    /* the case folder made in the workspace     */
    const char *file;        /* the STL written into it                   */
    const char *title, *what;
} MeshExample;
extern const MeshExample MESH_EXAMPLES[MESH_EX__N];

bool   mesh_example_write(int k, const char *path);   /* the part as an ASCII STL, metres, a solid per boundary */
bool   mesh_example_setup(int k, const char *dir, char *err, int cap);   /* the example case in dir, as the tutorial makes it */
double mesh_example_naca_y(double x);                 /* NACA 0012 half thickness at x (chord 1), closed trailing edge */
#endif
