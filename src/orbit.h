/* orbit.h -- the camera of the 3-D views (Mesh > View, the STL mesher): it circles a target, either freely with one world
 * axis kept upright (a turntable), or about one chosen world axis only, so a part can be spun about x, y or z alone. */
#ifndef QC_ORBIT_H
#define QC_ORBIT_H
#include <stdbool.h>
#include "raylib.h"

typedef struct {
    Vector3 target;
    float   dist;
    Vector3 back, up;                     /* unit: from the target toward the camera, and the camera's up */
    int     upAxis;                       /* the world axis kept upright when turning freely (0 x, 1 y, 2 z) */
    int     spin;                         /* -1: turn freely; 0, 1, 2: only about that world axis */
} Orbit;

/* yaw and pitch in the up axis' frame (x, y, z of it are the world's (up+2)%3, up, (up+1)%3), level */
void     orbit_set(Orbit *o, float yaw, float pitch);
/* the mouse moved from a to b (screen points) with c the view's centre: the part turns as if grabbed */
void     orbit_drag(Orbit *o, Vector2 a, Vector2 b, Vector2 c);
/* the part turned by ang radians (counter-clockwise looking down the axis) about the turn axis (turning freely: the up axis) */
void     orbit_spin(Orbit *o, float ang);
void     orbit_pan(Orbit *o, float dx, float dy);
void     orbit_level(Orbit *o);           /* the up axis upright again, looking from where the camera is */
Vector3  orbit_right(const Orbit *o);
Camera3D orbit_camera(const Orbit *o);
/* the world axes drawn at (x, y), length l, scale s, the turn axis drawn bolder with a ring round it */
void     orbit_draw_axes(const Orbit *o, float x, float y, float l, float s);
/* the arrow keys, the mouse over the view: 15 degrees about the turn axis (turning freely: left and right about the up
   axis, up and down over it); true when it turned */
bool     orbit_keys(Orbit *o);
/* how to turn it, for the views' hints: "drag to turn", or "drag across the x axis to spin it" */
const char *orbit_hint(const Orbit *o);
/* the toolbar: "Turn about" Free / X / Y / Z, two 90 degree turns and (fit) a Fit button, width orbit_tools_w(fit).
   1: it turned the view, 2: Fit was pressed (the caller fits), 0: nothing */
float    orbit_tools_w(bool fit);
int      orbit_tools(Orbit *o, const char *id, float x, float y, bool fit);

#endif
