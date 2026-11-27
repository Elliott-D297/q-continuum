/* page_stlmesh_int.h -- what page_stlmesh.c and page_stlview.c share (improvement plan A5, 2026-11-26): the STL mesher
 * page's state, its steps, the faces picked in the 3-D view and the helpers both use. Not for other pages. */
#ifndef PAGE_STLMESH_INT_H
#define PAGE_STLMESH_INT_H
#include <sys/types.h>
#include "app.h"
#include "stlmesh.h"

typedef struct {
    char   casePath[PL_PATH];   /* the case S and M belong to          */
    Stl    stl;
    MeshCfg m;
    bool   loaded, dirty;
    pid_t  picker;              /* the file dialog, while it is open   */
    char   pickOut[PL_PATH];
    bool   pickNewCase;         /* the pick makes a new case (Cases page) */
    bool   pickLoc;             /* the next click on the part in the view sets the point in the fluid (Flow) */
    char   pickCase[PL_PATH];   /* else the case it was opened for     */
    int    sel;                 /* the boundary picked (-1 none)       */
    int    reached, open;       /* the flow: the furthest step got to, the one unfolded (ST_*) */
    bool   adv;                 /* Mesh settings: the advanced ones shown */
    char   typed[PL_PATH];      /* the path field, for casePath only   */
    unsigned gen;               /* bumped on each case switch          */
    double removeArm;           /* Remove STL: the first click's time  */
    int    scrollTo;            /* a step was opened: bring its head up, a frame on (once the one before has folded) */
} StlPage;
extern StlPage STLP;
enum { ST_GEOM, ST_FLOW, ST_BOUND, ST_MESH, ST_GEN, ST__N };
#include "orbit.h"
#define VIEW_EDGES 60000                                  /* triangle outlines drawn up to this many triangles */
typedef struct {
    Model    model[STL_MAX_REGIONS]; bool has[STL_MAX_REGIONS];
    double   sig;                                         /* the geometry the models were built from */
    RenderTexture2D rt; int rtW, rtH; bool rtOk;
    bool     dirty, dragging, panning, moved;
    Orbit    O;                                           /* the camera (orbit.h): turned freely or about one axis */
    Vector2  last, down;
    double   lastClick;
    int      hover, drawnSel, drawnHover;
    bool     engaged;                                     /* clicked in: the wheel zooms until the mouse leaves */
    bool     touched;                                     /* turned, moved or zoomed by hand: the fit is left alone */
    int      up;                                          /* the axis the view turns about and keeps up (0 x, 1 y, 2 z; -1: the case's) */
    int      fitW, fitH;                                  /* the view's size the last fit was for */
} StlView;
extern StlView STLV;
typedef struct { bool *on; int cap, n; unsigned gen; } StlPick;
extern StlPick STLPK;

void sm_fmt_len(char *b, size_t cap, double v);
double sm_geometry_sig(void);
bool sm_in_pick(const Stl *st, int t);
void sm_loc_now(double out[3], bool *inFluid);
void sm_pick_clear(void);
void sm_pick_reset(void);
Color sm_region_color(int i);
void sm_save_cfg(void);
void sm_select_region(int r);
void sm_view_forget(void);
void sm_view_panel(Rectangle view);
#endif
