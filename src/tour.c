/* tour.c -- the guided tour (see tour.h). The steps are a table: the page each is about (and the Mesh or Physics tab), the
 * widget it lights up (an id as the journeys name them: "nav:mesh", "stl-view-turn#1"), whether it needs an open case, and
 * the words. While the card is up the page behind it is seen but not used (the card is the input layer, as a modal is). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "tour.h"

typedef struct {
    int         page;          /* PG_*, or -1: stay where it is */
    int         meshTab;       /* Mesh page: 0 dictionaries, 1 SALOME, 2 view, 3 STL mesher; -1: as it is */
    int         physicsTab;    /* Physics page: 2 Setup, 0 Models, 1 All files; -1: as it is */
    bool        needCase;
    const char *target;        /* the widget lit up, or NULL */
    const char *title;
    const char *body;
} Step;

enum { NO = -1 };

static const Step STEPS[] = {
    { NO, NO, NO, false, NULL, "Welcome to Q-Continuum",
      "Q-Continuum sets up, runs and watches OpenFOAM simulations without you having to write OpenFOAM's text files by hand "
      "(though you still can, and every file is there to look at).\n\n"
      "This tour takes about ten minutes. Each step opens the page it talks about and lights up the control it describes. "
      "Next and Back (or the arrow keys) move through it; Try it hides this card so you can use the page, and the pill at the "
      "bottom left brings the tour back where you left it. Esc or Skip ends it.\n\n"
      "You can take it again whenever you like: Settings > Help > Take the tour." },
    { PG_CASES, NO, NO, false, "nav:cases", "Getting around",
      "The bar along the top (or the side, in some themes) takes you anywhere: Cases (your simulations), New case, Queue "
      "(runs waiting for free cores), Studies (variants of a case run side by side), Coupling (preCICE multi-physics) and Settings.\n\n"
      "Once a case is open, its own pages appear down the side, in the order you usually work: Overview, Mesh, Boundary conditions, "
      "Physics, Numerics, Run, Files and History. Ctrl+0 is Cases, Ctrl+1 to Ctrl+8 are the case pages, Ctrl+, is Settings." },
    { PG_CASES, NO, NO, false, NULL, "Cases",
      "Every case in your workspace folder (set in Settings) is a card here, with what it is (solver, mesh, last run) and how its "
      "last run ended. Click a card to open it.\n\n"
      "Hover a card for its buttons: the copy icon duplicates the case (setup and mesh, no results: handy for trying a variant), "
      "the bin deletes it (it goes to the Windows Recycle Bin first). Open folder adds a case from anywhere on disk; F5 looks for "
      "new or changed cases again." },
    { PG_NEW, NO, NO, false, "starter-blank", "Starting a new case",
      "There are three ways to start:\n\n"
      "1. A blank case (this card, or Ctrl+Shift+N): you give it a name, steady or transient, and where its mesh comes from: an "
      "STL file from any CAD program, a mesh made elsewhere (SALOME, Gmsh, Fluent UNV), a simple box, or later. This is the way "
      "for your own parts.\n"
      "2. A starter below: worked examples (a cavity, a pipe, flow past a cylinder, a dam break ...) copied from OpenFOAM's "
      "tutorials, ready to run.\n"
      "3. Any of OpenFOAM's several hundred tutorials, searched by name further down." },
    { NO, NO, NO, true, NULL, "A case to look at",
      "The next steps are about a case's own pages, so a case has to be open. If none is, Make a practice case copies OpenFOAM's "
      "lid-driven cavity (a box whose lid slides, icoFoam, a few seconds to run) into your workspace as tour-cavity and opens it. "
      "You can delete it afterwards like any other case.\n\n"
      "If a case is open already, the tour uses that one." },
    { PG_OVERVIEW, NO, NO, true, NULL, "Overview",
      "The case at a glance: where it lives (copy its path, open a terminal or the folder there), its solver, its mesh, and a "
      "Case check: what it still needs before it can run (a mesh, boundary conditions for every field) and what it has produced.\n\n"
      "For a case made from an STL, a step bar runs along the top of the Mesh, Physics and Run pages: STL > Boundaries > Mesh > "
      "Physics > Run. Each step says whether it is done, and the button at its right does the next thing to do." },
    { PG_MESH, 0, NO, true, "mesh-gen", "Mesh: the dictionaries",
      "Mesh has four tabs. Dictionaries edits the mesh files the case already has (blockMeshDict's blocks, cell counts and grading, "
      "snappyHexMeshDict) as forms, with a cross-section preview.\n\n"
      "Generate mesh (Ctrl+M) runs blockMesh (and snappyHexMesh, if the case has one) and then checkMesh; the log shows on the Run "
      "page and the result in the cross section. The practice cavity is a 20 x 20 block: try Generate mesh now with Try it." },
    { PG_MESH, 3, NO, true, NULL, "Mesh: the STL mesher",
      "STL mesher turns a part from CAD into a mesh, in five steps. One is open at a time; finished ones fold up to a one-line "
      "summary you can click to change, and Next moves on.\n\n"
      "1. Geometry: choose the STL and check its size (CAD often saves millimetres: the units picker fixes it).\n"
      "2. Flow: through the part (a pipe, a duct) or around it (a wing, a car, in a wind tunnel), which way the flow goes and "
      "which way is up. A flat profile can be meshed 2-D.\n"
      "3. Boundaries: click a face of the part in the 3-D view to pick it, then name it and say what it is (wall, inlet, outlet ...).\n"
      "4. Mesh settings: how fine (a density preset, or the base and surface cell sizes, prism layers near walls); the view draws "
      "the cells to scale beside the part.\n"
      "5. Generate: snappyHexMesh, or SALOME for tetrahedra, polyhedra and body-fitted cells, with a preview and plain advice if it fails." },
    { PG_MESH, 3, NO, true, "stl-view-turn#0", "Turning the 3-D views",
      "The STL mesher and Mesh > View show the part in 3-D. Drag to turn it, right-drag (or Shift+drag) to move it, and click it "
      "once, then use the wheel to zoom (so the page still scrolls over it). Double-click or Fit puts it back.\n\n"
      "Turn about picks how dragging turns it. Free turns it any way while keeping the Up axis upright. X, Y or Z spins it about "
      "that axis only: drag across the axis (shown thicker, with a ring, in the corner gizmo) to spin it. The two round arrows "
      "turn it 90 degrees each way about that axis, and the arrow keys turn it 15 degrees while the mouse is over the view.\n\n"
      "Up says which axis points up (CAD programs disagree: Z or Y)." },
    { PG_MESH, 2, NO, true, NULL, "Mesh: View",
      "View shows the mesh that was made: a cross section through it, or Surface (3-D), the boundary faces with a colour per patch. "
      "Click a patch in the list to hide it and see inside. The counts (cells, faces, points, patches, size) are along the top; "
      "Look x / y / z points the camera along an axis.\n\n"
      "Mesh quality is checked by checkMesh after every generate: its verdict is on the Mesh page and in the case check." },
    { PG_BC, NO, NO, true, NULL, "Boundary conditions",
      "A grid of every field (U velocity, p pressure, k, omega ... ) against every patch (the named faces of the mesh). Each cell "
      "is that field's condition on that patch; click it to change the type and value. Problems (a patch with no condition, a "
      "value with the wrong units) are marked.\n\n"
      "Click a patch's name for the patch inspector: every field on it in one place, and Wall / Inlet / Outlet buttons that set "
      "them all at once in the usual way. Ctrl+Z undoes any change, Ctrl+S saves." },
    { PG_PHYSICS, NO, NO, true, NULL, "Physics",
      "For a case started from an STL or a blank case, Physics opens on Setup: the whole simulation as choices instead of files.\n\n"
      "It starts with What are you simulating?: External aero, Internal flow, Vortex shedding, Resolved turbulence, Heat transfer, "
      "Natural convection or Blank. "
      "A card fills in the models, the run's length and the time step from your part's size and the inflow speed, and the one "
      "that suits your part (flow through it or around it) is marked. Change brings the cards back later.\n\n"
      "Then a list of steps: Models (with Energy for heat and buoyancy), Materials, Boundaries, Starting values, Reports, Solver "
      "and Check. Each is marked done, "
      "worth a look or wrong; Next walks through them, and More settings in a step shows the expert ones. Check says what is left "
      "to fix and runs it.\n\n"
      "Write case files puts it all into 0/, constant/ and system/. Models and All files show the files themselves (a tutorial "
      "like the cavity has only these)." },
    { PG_PHYSICS, NO, 2, true, NULL, "Lift and drag",
      "Setup's Reports step has Lift and drag: it measures the force of the flow on the part while it runs. Switch on Monitor forces, tick the "
      "walls that make up the part, and the reference values are worked out from the STL: drag along the flow, lift up, the "
      "reference length (a wing's chord), the frontal area, the free-stream speed.\n\n"
      "The coefficients are Cd = drag / (1/2 rho U^2 A) and Cl the same with the lift, plus the pitching moment Cm. In 2-D the "
      "area is the chord times the mesh's depth, so they are the usual per-span airfoil values. The Run page shows them live and "
      "plots how they settle (a steady run has converged when they stop changing).\n\n"
      "Type the values in instead (Typed in) when you want a wing's planform area, or another moment centre." },
    { PG_NUMERICS, NO, NO, true, NULL, "Numerics",
      "How the equations are discretised and solved: Schemes (fvSchemes: time stepping, gradients, the convection terms), "
      "Solvers (fvSolution: the linear solvers, tolerances, relaxation, SIMPLE or PIMPLE correctors) and Parallel (decomposeParDict: "
      "how the mesh is split between processors).\n\n"
      "The setup and the tutorials come with sensible choices; hover any keyword for what it means and which values it takes." },
    { PG_RUN, NO, NO, true, "top-run", "Run",
      "Run case (Ctrl+R, here or on the left) runs every step the case needs (meshing if the mesh is out of date, decomposing for a parallel run, the "
      "solver), checked first (memory, time step, a missing field). Processes sets how many cores it uses.\n\n"
      "While it runs: the progress bar and time remaining, the residuals chart (how far each equation is from solved: falling lines "
      "are good), Monitors (forces, probes and anything else the case writes, with the Cd / Cl readout above them) and the log "
      "(search it; First error jumps to what went wrong, explained in plain words with a fix where there is one).\n\n"
      "Stop when converged ends a run once a monitored value stops changing. A run can be paused and continued, or sent to the Queue." },
    { NO, NO, NO, true, "top-pv", "Results in ParaView",
      "Open in ParaView (Ctrl+P, top right) opens the results in ParaView for contours, streamlines and slices. ParaView + Claude "
      "writes a ParaView view for you from a plain request (\"pressure on the wing with streamlines\").\n\n"
      "Run's log and History keep the numbers; the Make a movie command turns a transient run into a video." },
    { PG_FILES, NO, NO, true, NULL, "Files",
      "Every file of the case, in folders, with an editor that knows OpenFOAM's dictionaries (colours, folding, a check as you "
      "type). Changes anywhere in Q-Continuum are only written when you save (Ctrl+S, or Save at the top); the top bar counts what "
      "is waiting. Undo and Redo (Ctrl+Z, Ctrl+Y) work across pages.\n\n"
      "Edit a file in another program and Q-Continuum notices and reloads it." },
    { PG_HISTORY, NO, NO, true, NULL, "History",
      "Every run is recorded inside the case: how it ended and why, its residuals, and what in the setup changed since the run "
      "before. Snapshots keep a whole case to go back to; Clean moves old results to the trash here instead of deleting them.\n\n"
      "Export the log as Markdown for a lab notebook or a report." },
    { NO, NO, NO, false, "nav:queue", "Queue and Studies",
      "Queue: runs wait here and start on their own, oldest first, when there are cores free. It survives closing the app.\n\n"
      "Studies: variants of a case with one or two settings changed (a mesh refinement study, a range of inlet speeds, several "
      "turbulence models), queued and then compared side by side in a table and a chart, against your measurements if you have them." },
    { NO, NO, NO, false, NULL, "Finding anything",
      "Ctrl+K opens the command palette: type a few letters of any command, page, case, patch, field or OpenFOAM keyword and press "
      "Enter. Typing \"deltaT = 0.001\" there sets that value in the open case.\n\n"
      "F1 lists every keyboard shortcut, F11 is full screen, F5 reads the cases from disk again. Hover almost anything for a tooltip "
      "saying what it does." },
    { PG_SETTINGS, NO, NO, false, "set-tour", "Settings",
      "Settings holds the OpenFOAM installation used (found on its own; change it here if you have several), the workspace folder "
      "new cases go in, ParaView, the text editor and how runs behave.\n\n"
      "Appearance has the themes (a classic dark or light look, LCARS and many fan themes with their own sounds) and the interface "
      "size, if text is too small or large on your screen.\n\n"
      "Help > Take the tour brings this tour back." },
    { NO, NO, NO, false, NULL, "Where to go from here",
      "A good first hour:\n\n"
      "1. On the practice cavity: Run case (Ctrl+R) and watch the residuals fall; then Open in ParaView to see the vortex.\n"
      "2. Your own part: New case > Blank case > Mesh an STL. Follow the step bar: STL, Boundaries, Mesh, Physics (switch on Lift "
      "and drag for a wing or a car), Run.\n"
      "3. Change one thing (the inlet speed, the mesh density) with Duplicate case or a Study, and compare.\n\n"
      "When something fails, the Run page's First error says why in plain words. Good luck!" },
};
#define N_STEPS ((int)(sizeof STEPS / sizeof STEPS[0]))

static struct {
    bool on, paused;
    int  at;                   /* the step shown */
    int  went;                 /* the step whose page was last gone to (-1: none) */
} T = { .went = -1 };

bool tour_active(void) { return T.on; }

void tour_start(void)
{
    T.on = true; T.paused = false; T.at = 0; T.went = -1;
}

static void tour_end(void)
{
    T.on = false;
    if (!A.set.tourSeen) { A.set.tourSeen = true; settings_save(&A.set); }
}

/* "name", "name#3": as the journeys name widgets */
static UiId target_id(const char *s)
{
    char base[96]; int k = 0;
    while (s[k] && s[k] != '#' && k < 95) { base[k] = s[k]; k++; }
    base[k] = 0;
    UiId id = ui_id(base);
    if (s[k] == '#') id = ui_idn(id, atoi(s + k + 1));
    return id;
}

/* the step's page (once, on arriving at it: the user may move on with Try it) */
static void go_to(int k)
{
    if (T.went == k) return;
    T.went = k;
    const Step *s = &STEPS[k];
    if (s->needCase && !A.cs) return;                     /* the practice case step asks first */
    if (s->page >= 0) app_go((Page)s->page);
    if (s->meshTab >= 0) A.meshTab = s->meshTab;
    if (s->physicsTab >= 0) A.physicsTab = s->physicsTab;
}

static void move(int d)
{
    int k = T.at + d;
    if (k < 0) k = 0;
    if (k >= N_STEPS) { tour_end(); return; }
    T.at = k;
}

static void draw_paused(void)
{
    char t[64]; snprintf(t, sizeof t, "Tour paused (%d of %d)  ·  Resume", T.at + 1, N_STEPS);
    float bw = ui_button_width(ICON_BOOK_OPEN, t) + 8;
    Rectangle r = { 16, ui_height() - 56, bw, 40 };
    ui_occlude_begin((Rectangle){ r.x, r.y, r.width + 38, r.height });
    ui_shadow(r, 10, 0.6f);
    if (ui_button(ui_id("tour-resume"), r, ICON_BOOK_OPEN, t, BTN_PRIMARY)) { T.paused = false; T.went = -1; }
    Rectangle x = { r.x + r.width + 6, r.y + 4, 32, 32 };
    if (ui_button(ui_id("tour-end"), x, ICON_X, NULL, BTN_SECONDARY)) tour_end();
    ui_tooltip(x, "End the tour (Settings > Help > Take the tour brings it back)");
    ui_occlude_end();
}

void tour_draw(void)
{
    if (!T.on || A.modal != MD_NONE) return;
    if (T.paused) { draw_paused(); return; }
    go_to(T.at);
    const Step *s = &STEPS[T.at];
    bool waiting = s->needCase && !A.cs;
    float sw = ui_width(), sh = ui_height(), sc = ui_scale();
    ui_modal_layer();                                     /* the card takes the input; the page shows through */
    /* the lit-up widget: everything else dimmed round it */
    Rectangle hole = { 0 }; bool lit = false;
    if (s->target && !waiting) {
        const UiWidget *w = ui_widget_find(target_id(s->target));
        if (w && w->vis.width > 0 && w->layer == 0) {
            hole = (Rectangle){ w->vis.x / sc - 6, w->vis.y / sc - 6, w->vis.width / sc + 12, w->vis.height / sc + 12 };
            lit = true;
        }
    }
    Color dim = ui_alpha(TH.dark ? (Color){ 3, 7, 13, 255 } : (Color){ 26, 36, 54, 255 }, 0.5f);
    if (lit) {
        DrawRectangleRec((Rectangle){ 0, 0, sw, hole.y }, dim);
        DrawRectangleRec((Rectangle){ 0, hole.y + hole.height, sw, sh - hole.y - hole.height }, dim);
        DrawRectangleRec((Rectangle){ 0, hole.y, hole.x, hole.height }, dim);
        DrawRectangleRec((Rectangle){ hole.x + hole.width, hole.y, sw - hole.x - hole.width, hole.height }, dim);
        float p = 0.55f + 0.45f * sinf((float)ui_time() * 4);
        ui_rect_lines(hole, 10, 3, ui_alpha(TH.accent, p));
        ui_request_frames(2);
    } else DrawRectangleRec((Rectangle){ 0, 0, sw, sh }, dim);
    /* the card: beside the lit widget where there is room, else at the bottom right */
    float cw = fminf(600, sw - 32), pad = 22;
    char body[3000]; pl_strlcpy(body, s->body, sizeof body);
    float th = ui_text_wrap(F_REG, 14, body, (Rectangle){ 0, 0, cw - 2 * pad, 2000 }, 21, TH.text2, false);
    float ch = pad + 22 + 36 + th + 16 + 14 + 14 + 36 + pad;          /* heading, title, words, dots, buttons */
    if (ch > sh - 32) ch = sh - 32;
    Rectangle card = { sw - cw - 24, fmaxf(16, fminf((sh - ch) / 2 + 40, sh - ch - 90)), cw, ch };   /* clear of the toasts below */
    if (lit) {
        if (hole.x + hole.width + 20 + cw < sw - 16) card.x = hole.x + hole.width + 20;
        else if (hole.x - 20 - cw > 16) card.x = hole.x - 20 - cw;
        card.y = fmaxf(16, fminf(hole.y, sh - ch - 16));
        if (CheckCollisionRecs(card, hole)) {            /* no room either side: above or below it */
            card.x = fmaxf(16, fminf(hole.x, sw - cw - 16));
            card.y = hole.y + hole.height + 20 + ch < sh - 16 ? hole.y + hole.height + 20 : fmaxf(16, hole.y - 20 - ch);
        }
    } else if (T.at == 0 || T.at == N_STEPS - 1) card = (Rectangle){ (sw - cw) / 2, (sh - ch) / 2, cw, ch };
    ui_shadow(card, 14, 1);
    Color solid = TH.bg; solid.a = 255;                   /* glass themes' surfaces are see-through: the words over the page stay legible */
    ui_rect(card, 14, solid);
    ui_rect(card, 14, TH.surface);
    ui_rect_lines(card, 14, 1.5f, ui_alpha(TH.accent, 0.7f));
    ui_register(ui_id("tour-card"), card, s->title);
    float x = card.x + pad, y = card.y + pad;
    char cnt[32]; snprintf(cnt, sizeof cnt, "%d / %d", T.at + 1, N_STEPS);
    ui_text(F_MED, 12.5f, ui_cap("Tour"), x, y + 1, TH.accent);
    ui_text(F_MONO, 12.5f, cnt, x + ui_text_w(F_MED, 12.5f, ui_cap("Tour")) + 10, y + 1, TH.text3);
    Rectangle xr = { card.x + card.width - pad - 30, y - 4, 30, 30 };
    bool skip = ui_button(ui_id("tour-skip"), xr, ICON_X, NULL, BTN_GHOST);
    ui_tooltip(xr, "Skip the tour (Settings > Help > Take the tour brings it back)");
    y += 22;
    ui_text_clip(F_SEMI, 20, ui_cap_words(s->title), x, y, card.width - 2 * pad, TH.text);
    y += 36;
    Rectangle br = { x, y, card.width - 2 * pad, card.y + card.height - pad - 52 - y };
    ui_clip_push(br);
    ui_text_wrap(F_REG, 14, body, (Rectangle){ br.x, br.y, br.width, 2000 }, 21, TH.text2, true);
    ui_clip_pop();
    /* progress dots */
    float dy = card.y + card.height - pad - 40 - 14, dx = x;
    for (int k = 0; k < N_STEPS; k++) {
        DrawCircleV((Vector2){ dx + 4, dy }, k == T.at ? 4 : 3, k == T.at ? TH.accent : k < T.at ? ui_alpha(TH.accent, 0.45f) : TH.lineSoft);
        dx += 13;
    }
    /* the buttons */
    float by = card.y + card.height - pad - 36, bx = card.x + card.width - pad;
    bool last = T.at == N_STEPS - 1;
    const char *nl = last ? "Finish" : T.at == 0 ? "Start the tour" : "Next";
    float nw = ui_button_width(last ? ICON_CHECK : ICON_ARROW_RIGHT, nl);
    bx -= nw;
    bool next = false, back = false, tryit = false;
    if (waiting) {
        const char *pl = "Make a practice case";
        float pw = ui_button_width(ICON_FOLDER_PLUS, pl);
        bx += nw - pw;
        if (ui_button(ui_id("tour-practice"), (Rectangle){ bx, by, pw, 36 }, ICON_FOLDER_PLUS, pl, BTN_PRIMARY) && app_practice_case()) next = true;
        nw = pw;
    } else next = ui_button(ui_id("tour-next"), (Rectangle){ bx, by, nw, 36 }, last ? ICON_CHECK : ICON_ARROW_RIGHT, nl, BTN_PRIMARY);
    if (T.at > 0 && !last) {
        float tw = ui_button_width(ICON_EYE, "Try it");
        bx -= tw + 10;
        tryit = ui_button(ui_id("tour-try"), (Rectangle){ bx, by, tw, 36 }, ICON_EYE, "Try it", BTN_SECONDARY);
        ui_tooltip((Rectangle){ bx, by, tw, 36 }, "Hide this card and use the page; the pill at the bottom left brings the tour back");
    }
    if (T.at > 0) {
        float bw2 = ui_button_width(ICON_ARROW_LEFT, "Back");
        bx -= bw2 + 10;
        back = ui_button(ui_id("tour-back"), (Rectangle){ bx, by, bw2, 36 }, ICON_ARROW_LEFT, "Back", BTN_GHOST);
    }
    if (T.at == 0) {
        float bw2 = ui_button_width(NULL, "Skip the tour");
        bx -= bw2 + 10;
        skip |= ui_button(ui_id("tour-skip2"), (Rectangle){ bx, by, bw2, 36 }, NULL, "Skip the tour", BTN_GHOST);
    }
    if (!ui_any_input_focused()) {
        if (inp_key_pressed(KEY_RIGHT) && !waiting) next = true;
        if (inp_key_pressed(KEY_LEFT)) back = true;
        if (inp_key_pressed(KEY_ESCAPE)) skip = true;
    }
    ui_modal_end();
    if (skip) { tour_end(); return; }
    if (tryit) { T.paused = true; return; }
    if (back) move(-1);
    else if (next) move(+1);
}
