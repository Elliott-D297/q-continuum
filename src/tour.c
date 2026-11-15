/* tour.c -- the guided tour (see tour.h). The steps are a table: the page each is about (and the Mesh or Physics tab), the
 * widget it lights up (an id as the journeys name them: "nav:mesh", "stl-view-turn#1"), whether it needs an open case, and
 * the words. While the card is up the page behind it is seen but not used (the card is the input layer, as a modal is). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "tour.h"
#include "meshexamples.h"

typedef struct {
    int         page;          /* PG_*, or -1: stay where it is */
    int         meshTab;       /* Mesh page: 0 dictionaries, 1 SALOME, 2 view, 3 STL mesher; -1: as it is */
    int         physicsTab;    /* Physics page: 2 Setup, 0 Models, 1 All files; -1: as it is */
    bool        needCase;
    const char *target;        /* the widget lit up, or NULL */
    const char *title;
    const char *body;
    int         stl1;          /* STL mesher: its flow step opened (0 geometry ... 4 generate), plus one; 0: as it is */
    int         ex1;           /* needs the meshing tutorial's example case (meshexamples.h), plus one; 0: none */
} Step;

enum { NO = -1 };

static const Step STEPS[] = {
    { NO, NO, NO, false, NULL, "Welcome to Q-Continuum",
      "Q-Continuum sets up, runs and watches OpenFOAM simulations without you having to write OpenFOAM's text files by hand "
      "(though you still can, and every file is there to look at).\n\n"
      "This tour takes about ten minutes. Each step opens the page it talks about and lights up the control it describes. "
      "Next and Back (or the arrow keys) move through it; Try it hides this card so you can use the page, and the pill at the "
      "bottom left brings the tour back where you left it. Esc or Skip ends it.\n\n"
      "You can take it again whenever you like: Settings > Help > Take the tour.", 0, 0 },
    { PG_CASES, NO, NO, false, "nav:cases", "Getting around",
      "The bar along the top (or the side, in some themes) takes you anywhere: Cases (your simulations), New case, Queue "
      "(runs waiting for free cores), Studies (variants of a case run side by side), Coupling (preCICE multi-physics) and Settings.\n\n"
      "Once a case is open, its own pages appear down the side, in the order you usually work: Overview, Mesh, Boundary conditions, "
      "Physics, Numerics, Run, Files and History. Ctrl+0 is Cases, Ctrl+1 to Ctrl+8 are the case pages, Ctrl+, is Settings.", 0, 0 },
    { PG_CASES, NO, NO, false, NULL, "Cases",
      "Every case in your workspace folder (set in Settings) is a card here, with what it is (solver, mesh, last run) and how its "
      "last run ended. Click a card to open it.\n\n"
      "Hover a card for its buttons: the copy icon duplicates the case (setup and mesh, no results: handy for trying a variant), "
      "the bin deletes it (it goes to the Windows Recycle Bin first). Open folder adds a case from anywhere on disk; F5 looks for "
      "new or changed cases again.", 0, 0 },
    { PG_NEW, NO, NO, false, "starter-blank", "Starting a new case",
      "There are three ways to start:\n\n"
      "1. A blank case (this card, or Ctrl+Shift+N): you give it a name, steady or transient, and where its mesh comes from: an "
      "STL file from any CAD program, a mesh made elsewhere (SALOME, Gmsh, Fluent UNV), a simple box, or later. This is the way "
      "for your own parts.\n"
      "2. A starter below: worked examples (a cavity, a pipe, flow past a cylinder, a dam break ...) copied from OpenFOAM's "
      "tutorials, ready to run.\n"
      "3. Any of OpenFOAM's several hundred tutorials, searched by name further down.", 0, 0 },
    { NO, NO, NO, true, NULL, "A case to look at",
      "The next steps are about a case's own pages, so a case has to be open. If none is, Make a practice case copies OpenFOAM's "
      "lid-driven cavity (a box whose lid slides, icoFoam, a few seconds to run) into your workspace as tour-cavity and opens it. "
      "You can delete it afterwards like any other case.\n\n"
      "If a case is open already, the tour uses that one.", 0, 0 },
    { PG_OVERVIEW, NO, NO, true, NULL, "Overview",
      "The case at a glance: where it lives (copy its path, open a terminal or the folder there), its solver, its mesh, and a "
      "Case check: what it still needs before it can run (a mesh, boundary conditions for every field) and what it has produced.\n\n"
      "For a case made from an STL, a step bar runs along the top of the Mesh, Physics and Run pages: STL > Boundaries > Mesh > "
      "Physics > Run. Each step says whether it is done, and the button at its right does the next thing to do.", 0, 0 },
    { PG_MESH, 0, NO, true, "mesh-gen", "Mesh: the dictionaries",
      "Mesh has four tabs. Dictionaries edits the mesh files the case already has (blockMeshDict's blocks, cell counts and grading, "
      "snappyHexMeshDict) as forms, with a cross-section preview.\n\n"
      "Generate mesh (Ctrl+M) runs blockMesh (and snappyHexMesh, if the case has one) and then checkMesh; the log shows on the Run "
      "page and the result in the cross section. The practice cavity is a 20 x 20 block: try Generate mesh now with Try it.", 0, 0 },
    { PG_MESH, 3, NO, true, NULL, "Mesh: the STL mesher",
      "STL mesher turns a part from CAD into a mesh, in five steps. One is open at a time; finished ones fold up to a one-line "
      "summary you can click to change, and Next moves on.\n\n"
      "1. Geometry: choose the STL and check its size (CAD often saves millimetres: the units picker fixes it).\n"
      "2. Flow: through the part (a pipe, a duct) or around it (a wing, a car, in a wind tunnel), which way the flow goes and "
      "which way is up. A flat profile can be meshed 2-D.\n"
      "3. Boundaries: click a face of the part in the 3-D view to pick it, then name it and say what it is (wall, inlet, outlet ...).\n"
      "4. Mesh settings: how fine (a density preset, or the base and surface cell sizes, prism layers near walls); the view draws "
      "the cells to scale beside the part.\n"
      "5. Generate: snappyHexMesh, or SALOME for tetrahedra, polyhedra and body-fitted cells, with a preview and plain advice if it fails.", 0, 0 },
    { PG_MESH, 3, NO, true, "stl-view-turn#0", "Turning the 3-D views",
      "The STL mesher and Mesh > View show the part in 3-D. Drag to turn it, right-drag (or Shift+drag) to move it, and click it "
      "once, then use the wheel to zoom (so the page still scrolls over it). Double-click or Fit puts it back.\n\n"
      "Turn about picks how dragging turns it. Free turns it any way while keeping the Up axis upright. X, Y or Z spins it about "
      "that axis only: drag across the axis (shown thicker, with a ring, in the corner gizmo) to spin it. The two round arrows "
      "turn it 90 degrees each way about that axis, and the arrow keys turn it 15 degrees while the mouse is over the view.\n\n"
      "Up says which axis points up (CAD programs disagree: Z or Y).", 0, 0 },
    { PG_MESH, 2, NO, true, NULL, "Mesh: View",
      "View shows the mesh that was made: a cross section through it, or Surface (3-D), the boundary faces with a colour per patch. "
      "Click a patch in the list to hide it and see inside. The counts (cells, faces, points, patches, size) are along the top; "
      "Look x / y / z points the camera along an axis.\n\n"
      "Mesh quality is checked by checkMesh after every generate: its verdict is on the Mesh page and in the case check.", 0, 0 },
    { PG_BC, NO, NO, true, NULL, "Boundary conditions",
      "A grid of every field (U velocity, p pressure, k, omega ... ) against every patch (the named faces of the mesh). Each cell "
      "is that field's condition on that patch; click it to change the type and value. Problems (a patch with no condition, a "
      "value with the wrong units) are marked.\n\n"
      "Click a patch's name for the patch inspector: every field on it in one place, and Wall / Inlet / Outlet buttons that set "
      "them all at once in the usual way. Ctrl+Z undoes any change, Ctrl+S saves.", 0, 0 },
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
      "like the cavity has only these).", 0, 0 },
    { PG_PHYSICS, NO, 2, true, NULL, "Lift and drag",
      "Setup's Reports step has Lift and drag: it measures the force of the flow on the part while it runs. Switch on Monitor forces, tick the "
      "walls that make up the part, and the reference values are worked out from the STL: drag along the flow, lift up, the "
      "reference length (a wing's chord), the frontal area, the free-stream speed.\n\n"
      "The coefficients are Cd = drag / (1/2 rho U^2 A) and Cl the same with the lift, plus the pitching moment Cm. In 2-D the "
      "area is the chord times the mesh's depth, so they are the usual per-span airfoil values. The Run page shows them live and "
      "plots how they settle (a steady run has converged when they stop changing).\n\n"
      "Type the values in instead (Typed in) when you want a wing's planform area, or another moment centre.", 0, 0 },
    { PG_NUMERICS, NO, NO, true, NULL, "Numerics",
      "How the equations are discretised and solved: Schemes (fvSchemes: time stepping, gradients, the convection terms), "
      "Solvers (fvSolution: the linear solvers, tolerances, relaxation, SIMPLE or PIMPLE correctors) and Parallel (decomposeParDict: "
      "how the mesh is split between processors).\n\n"
      "The setup and the tutorials come with sensible choices; hover any keyword for what it means and which values it takes.", 0, 0 },
    { PG_RUN, NO, NO, true, "top-run", "Run",
      "Run case (Ctrl+R, here or on the left) runs every step the case needs (meshing if the mesh is out of date, decomposing for a parallel run, the "
      "solver), checked first (memory, time step, a missing field). Processes sets how many cores it uses.\n\n"
      "While it runs: the progress bar and time remaining, the residuals chart (how far each equation is from solved: falling lines "
      "are good), Monitors (forces, probes and anything else the case writes, with the Cd / Cl readout above them) and the log "
      "(search it; First error jumps to what went wrong, explained in plain words with a fix where there is one).\n\n"
      "Stop when converged ends a run once a monitored value stops changing. A run can be paused and continued, or sent to the Queue.", 0, 0 },
    { NO, NO, NO, true, "top-pv", "Results in ParaView",
      "Open in ParaView (Ctrl+P, top right) opens the results in ParaView for contours, streamlines and slices. ParaView + Claude "
      "writes a ParaView view for you from a plain request (\"pressure on the wing with streamlines\").\n\n"
      "Run's log and History keep the numbers; the Make a movie command turns a transient run into a video.", 0, 0 },
    { PG_FILES, NO, NO, true, NULL, "Files",
      "Every file of the case, in folders, with an editor that knows OpenFOAM's dictionaries (colours, folding, a check as you "
      "type). Changes anywhere in Q-Continuum are only written when you save (Ctrl+S, or Save at the top); the top bar counts what "
      "is waiting. Undo and Redo (Ctrl+Z, Ctrl+Y) work across pages.\n\n"
      "Edit a file in another program and Q-Continuum notices and reloads it.", 0, 0 },
    { PG_HISTORY, NO, NO, true, NULL, "History",
      "Every run is recorded inside the case: how it ended and why, its residuals, and what in the setup changed since the run "
      "before. Snapshots keep a whole case to go back to; Clean moves old results to the trash here instead of deleting them.\n\n"
      "Export the log as Markdown for a lab notebook or a report.", 0, 0 },
    { NO, NO, NO, false, "nav:queue", "Queue and Studies",
      "Queue: runs wait here and start on their own, oldest first, when there are cores free. It survives closing the app.\n\n"
      "Studies: variants of a case with one or two settings changed (a mesh refinement study, a range of inlet speeds, several "
      "turbulence models), queued and then compared side by side in a table and a chart, against your measurements if you have them.", 0, 0 },
    { NO, NO, NO, false, NULL, "Finding anything",
      "Ctrl+K opens the command palette: type a few letters of any command, page, case, patch, field or OpenFOAM keyword and press "
      "Enter. Typing \"deltaT = 0.001\" there sets that value in the open case.\n\n"
      "F1 lists every keyboard shortcut, F11 is full screen, F5 reads the cases from disk again. Hover almost anything for a tooltip "
      "saying what it does.", 0, 0 },
    { PG_SETTINGS, NO, NO, false, "set-tour", "Settings",
      "Settings holds the OpenFOAM installation used (found on its own; change it here if you have several), the workspace folder "
      "new cases go in, ParaView, the text editor and how runs behave.\n\n"
      "Appearance has the themes (a classic dark or light look, LCARS and many fan themes with their own sounds) and the interface "
      "size, if text is too small or large on your screen.\n\n"
      "Help > Take the tour brings this tour back.", 0, 0 },
    { NO, NO, NO, false, NULL, "Where to go from here",
      "A good first hour:\n\n"
      "1. On the practice cavity: Run case (Ctrl+R) and watch the residuals fall; then Open in ParaView to see the vortex.\n"
      "2. Your own part: New case > Blank case > Mesh an STL. Follow the step bar: STL, Boundaries, Mesh, Physics (switch on Lift "
      "and drag for a wing or a car), Run.\n"
      "3. Change one thing (the inlet speed, the mesh density) with Duplicate case or a Study, and compare.\n\n"
      "When something fails, the Run page's First error says why in plain words. Good luck!", 0, 0 },
};
#define N_MAIN ((int)(sizeof STEPS / sizeof STEPS[0]))

/* ---------------------------------------------------------------------------- the meshing tutorial (user request
   2026-11-13: "an individual tutorial of how to mesh parts, what makes a good mesh, and a couple of examples to go
   through"; on the first start it follows the tour). Ideas first, in fluid-mechanics words with numbers, then three parts
   meshed step by step: a duct (internal), a sphere in a wind tunnel (external), a NACA 0012 section (2-D). */
#define EX(k) (k) + 1
#define ST(k) (k) + 1
static const Step MESH_STEPS[] = {
    { NO, NO, NO, false, NULL, "Meshing: from a part to a good mesh",
      "This tutorial is about the mesh: what it is, what makes one good, and how to make one from a part with Q-Continuum's STL "
      "mesher. It takes about twenty minutes.\n\n"
      "First seven short ideas (no clicking), then three parts meshed step by step, each made for you as a case of its own:\n"
      "1. A square duct: water flowing through a part (internal flow).\n"
      "2. A sphere in a wind tunnel: air flowing round a part (external flow), with its wake.\n"
      "3. A NACA 0012 wing section: a 2-D mesh, one cell thick.\n\n"
      "Next and Back (or the arrow keys) move through it, Try it hides the card so you can work the page, Esc ends it. "
      "Settings > Help > Meshing tutorial brings it back.", 0, 0 },
    { NO, NO, NO, false, NULL, "What a mesh is",
      "OpenFOAM doesn't solve the flow everywhere: it cuts the fluid's space into small cells and solves for one velocity, one "
      "pressure (and so on) per cell, the average over it. The mesh is that set of cells.\n\n"
      "So the mesh is the resolution of your answer. A boundary layer 2 mm thick drawn with one cell has one velocity across it: "
      "the solver cannot see the profile you drew in fluid mechanics, so it cannot get the wall shear stress (and the drag) right.\n\n"
      "Cells cost time and memory: roughly 1 GB of memory and a few minutes per thousand iterations per million cells on one core. "
      "Meshing well means cells where the flow changes, few where it doesn't.", 0, 0 },
    { NO, NO, NO, false, NULL, "A good mesh 1: cells where the flow changes",
      "Put small cells where velocity or pressure change quickly, over a short distance:\n\n"
      "- Next to walls: the boundary layer (no-slip, the velocity goes from zero to the free stream in a few millimetres).\n"
      "- In wakes and shear layers: behind a bluff body, a jet's edge.\n"
      "- Round sharp edges and tight curves: the flow turns quickly there. A circle needs about 36 cells round it.\n\n"
      "Away from those, cells can be large. Between the two the size must grow gently: neighbours no more than about 1.2 to "
      "1.3 times each other's size. A sudden jump (a cell next to one eight times its size) puts a false error into the "
      "equations, as a coarse step in a numerical integration would.", 0, 0 },
    { NO, NO, NO, false, NULL, "A good mesh 2: cell shape (the quality numbers)",
      "A cell's shape matters as much as its size. After every mesh, checkMesh measures each cell; the ones to know:\n\n"
      "- Non-orthogonality: the angle between the line joining two cell centres and the face between them. 0 is ideal; under "
      "65 is fine; over 70 the gradients get inaccurate; over 85 the solver often blows up.\n"
      "- Skewness: how far the face centre is from that line. Under 4 inside the mesh.\n"
      "- Aspect ratio: long, thin cells. Fine along a wall in the boundary layer (the flow changes slowly along it), bad out "
      "in the flow.\n"
      "- Negative volumes: cells turned inside out. Must be zero.\n\n"
      "Mesh > Quality shows these for the mesh on disk, with the worst cells located.", 0, 0 },
    { NO, NO, NO, false, NULL, "A good mesh 3: the boundary layer and y+",
      "Prism layers are thin cells stacked on the walls for the boundary layer. How thin the first one must be is set by y+, the "
      "wall distance in viscous units: y+ = y u_tau / nu, with u_tau = sqrt(tau_wall / rho).\n\n"
      "- y+ about 1: the first cell inside the viscous sublayer; the turbulence model resolves the layer (k-omega SST, about "
      "10 to 20 layers).\n"
      "- y+ 30 to 300: wall functions bridge the inner layer (k-epsilon, fewer layers, far fewer cells).\n\n"
      "A worked example: air at 10 m/s along a 1 m plate. Re = 10 x 1 / 1.5e-5 = 6.7e5; Cf = 0.026 Re^(-1/7) = 0.0038; "
      "tau_wall = 1/2 rho U^2 Cf = 0.23 Pa; u_tau = sqrt(0.23 / 1.2) = 0.44 m/s. For y+ = 1, y = nu / u_tau = 0.034 mm. "
      "The run reports the y+ it got: check it afterwards.", 0, 0 },
    { NO, NO, NO, false, NULL, "A good mesh 4: enough room round the part",
      "Flow round a part (a wing, a car) is meshed inside a wind tunnel: a box round it. Its walls must be far enough away that "
      "they don't change the flow at the part: about 3 to 5 body lengths ahead, above, below and to the sides, 8 to 10 behind "
      "(the wake has to leave cleanly), and the part blocking no more than a few percent of the tunnel's cross section.\n\n"
      "The far field's cells can be very large: the STL mesher makes them up to 8 times the base size, with a box of base-sized "
      "cells round the body and its wake.\n\n"
      "Flow through a part (a pipe, a duct) needs no tunnel: the part's inside is the fluid. A long enough inlet helps (the "
      "profile develops in about 10 to 60 diameters).", 0, 0 },
    { NO, NO, NO, false, NULL, "Which mesher, and is the mesh fine enough?",
      "The STL mesher can use several programs: snappyHexMesh (hexahedra cut to the surface, prism layers on any part: the "
      "robust default), cfMesh, foamyHexMesh, and SALOME's tetrahedra, polyhedra and structured hexahedra. Mesh settings marks "
      "the one it recommends for your part, and why.\n\n"
      "No mesh is right by itself: refine it until the answer stops changing. Mesh the part coarse, medium and fine (each "
      "about twice the cells of the one before), run all three and compare the quantity you care about (drag, pressure drop). "
      "When medium and fine agree within a percent or two, medium is enough. Duplicate case or a Study does this for you.", 0, 0 },
    /* ---- example 1: the duct */
    { NO, NO, NO, false, NULL, "Example 1: a square duct",
      "Water flowing through a duct 0.5 m long and 0.1 m square. The part's inside is the fluid: internal flow.\n\n"
      "Make the example case writes the duct as an STL with three named surfaces (inlet, outlet, walls) into a new case, "
      "example-duct, and opens it in Mesh > STL mesher. The next steps open each of the mesher's five steps in turn.",
      0, EX(MESH_EX_DUCT) },
    { PG_MESH, 3, NO, true, "stl-step#0", "Duct: Geometry",
      "Step 1, Geometry, reads the part and checks it. Look at:\n\n"
      "- The size: 500 mm long. CAD programs often save millimetres; if a part comes in a thousand times too large, the units "
      "picker fixes it.\n"
      "- Closed: every edge shared by two triangles, no holes. A mesher fills the inside of a closed surface; a gap lets the "
      "mesh leak out.\n\n"
      "The 3-D view beside it shows the part: drag to turn it.",
      ST(0), EX(MESH_EX_DUCT) },
    { PG_MESH, 3, NO, true, "stl-domain#0", "Duct: Flow",
      "Step 2, Flow: Through the part is chosen, so the duct's inside is meshed.\n\n"
      "Around the part would put the duct in a wind tunnel and mesh the air outside it: the same STL, a different problem. "
      "This is the most important choice in the whole mesh.",
      ST(1), EX(MESH_EX_DUCT) },
    { PG_MESH, 3, NO, true, "stl-step#2", "Duct: Boundaries",
      "Step 3, Boundaries: each surface of the part becomes a boundary (an OpenFOAM patch) with a name and a kind. The duct "
      "came with three: inlet (kind inlet), outlet (outlet) and walls (wall, where the layers go).\n\n"
      "For your own parts: click a face in the 3-D view to pick it, shift-click to add more, then name it and set its kind. "
      "Name the faces you will give a condition to (an inlet velocity, an outlet pressure); everything else can stay wall.",
      ST(2), EX(MESH_EX_DUCT) },
    { PG_MESH, 3, NO, true, "stl-density#1", "Duct: Mesh settings",
      "Step 4, Mesh settings. Everything is sized from the base size (the largest cells), as in STAR-CCM+:\n\n"
      "- Coarse / Medium / Fine doubles or halves the base size. Medium here is 10 mm: 10 cells across the duct.\n"
      "- The line under it says how many cells that makes along the part and in all: watch it while you change things.\n"
      "- Prism layer mesher: 3 layers on the walls, stretching 1.2 (each 20 % thicker than the one under it). The note under "
      "them gives the first layer's thickness: compare it with the y+ sum a few steps back.\n\n"
      "Try Fine with Try it and see the cell count go up about eight times (twice in each direction).",
      ST(3), EX(MESH_EX_DUCT) },
    { PG_MESH, 3, NO, true, "stl-gen", "Duct: Generate",
      "Step 5, Generate mesh writes OpenFOAM's dictionaries and runs the mesher (here snappyHexMesh: a background block, the "
      "feature edges, castellate, snap to the surface, add the layers) and then checkMesh. It takes under a minute; the log is "
      "on the Run page.\n\n"
      "Press Try it, then Generate mesh, and come back with the pill at the bottom left when it has finished.",
      ST(4), EX(MESH_EX_DUCT) },
    { PG_MESH, 4, NO, true, "mesh-tabs#4", "Duct: is it a good mesh?",
      "Mesh > Quality reads checkMesh's report: the cell count by shape (hexahedra, prisms ...), the worst non-orthogonality, "
      "skewness and aspect ratio, and whether every check passed.\n\n"
      "On the duct expect almost all hexahedra, the prisms of the layers, non-orthogonality well under 65 and Mesh OK.\n\n"
      "Mesh > View shows a cross section: look at the layers along the walls and the even cells in the middle. If a check fails, "
      "Quality says which cells and what usually fixes it.",
      0, EX(MESH_EX_DUCT) },
    /* ---- example 2: the sphere */
    { NO, NO, NO, false, NULL, "Example 2: a sphere in a wind tunnel",
      "Air flowing past a sphere 0.1 m across. Now the fluid is outside the part: external flow, in a wind tunnel round it.\n\n"
      "Make the example case writes the sphere into a new case, example-sphere. Its flow is already set to Around the part, "
      "along +X, with Z up.",
      0, EX(MESH_EX_SPHERE) },
    { PG_MESH, 3, NO, true, "stl-domain#1", "Sphere: the wind tunnel",
      "Around the part puts the sphere in a box. The distances are in body lengths: here 2 ahead, 5 behind, 2 to the sides, "
      "above and below, shorter than you would use for a real result (3 to 5 and 8 to 10) so that the example meshes in a "
      "minute or two.\n\n"
      "The inlet is the face the flow comes from, the outlet the one opposite, the rest far field. The 3-D view draws the tunnel "
      "to scale round the part.",
      ST(1), EX(MESH_EX_SPHERE) },
    { PG_MESH, 3, NO, true, "stl-adv", "Sphere: refine the wake",
      "The sphere's wake (the slow, unsteady flow behind it) needs smaller cells than the far field. In Mesh settings switch on "
      "Advanced, then under Volumetric controls press Add box: a box behind the body with its own cell size (50 % of the base "
      "size to start with). Coordinates are in metres; the 3-D view draws the box.\n\n"
      "The surface of a curved part is refined to the minimum surface size where it bends: Surface curvature (points round a "
      "circle) sets how much.",
      ST(3), EX(MESH_EX_SPHERE) },
    { PG_MESH, 3, NO, true, "stl-gen", "Sphere: generate and look",
      "Generate mesh (Try it, then come back with the pill). Then look at Mesh > View's cross section through the middle: the "
      "cells halve in steps from the far field to the sphere (snappyHexMesh's refinement levels), with the layers on its "
      "surface and the finer box behind it.\n\n"
      "Each step from one size to the next is gradual: a few cells of one size before it halves again (Volume growth rate sets "
      "how many). That is the smooth growth from the start of this tutorial.",
      ST(4), EX(MESH_EX_SPHERE) },
    /* ---- example 3: the airfoil */
    { NO, NO, NO, false, NULL, "Example 3: a wing section in 2-D",
      "Air flowing past a NACA 0012 airfoil with a 1 m chord. A wing section's flow is the same all along its span, so it can "
      "be meshed 2-D: one cell thick, far fewer cells, the same answer per metre of span.\n\n"
      "Make the example case writes the airfoil (extruded 0.1 m along Z) into example-airfoil, set to 2-D across Z, around the "
      "part, flow along +X, Y up.",
      0, EX(MESH_EX_AIRFOIL) },
    { PG_MESH, 3, NO, true, "stl-2d", "Airfoil: one cell thick",
      "Mesh it 2-D across Z is on: the mesh is one cell deep along Z and its front and back become empty patches (OpenFOAM's "
      "way of saying \"nothing changes along here\"). The extrusion's end caps are not walls: the mesher leaves them out.\n\n"
      "A 2-D profile from CAD is often a few dozen straight segments; Smooth the profile (offered here when it is coarse) "
      "fits a smooth curve through them so the layers don't break at every corner.\n\n"
      "With SALOME installed, Mesh settings recommends its triangles for a 2-D profile (the fewest cells); this example stays "
      "with snappyHexMesh, which every installation has.",
      ST(1), EX(MESH_EX_AIRFOIL) },
    { PG_MESH, 3, NO, true, "stl-prism", "Airfoil: layers all round",
      "The boundary layer on a wing is what sets its drag, so the layers matter most here. In 2-D Even surface cells under "
      "the layers is on by itself: the airfoil's surface cells are all one size, so the layers run unbroken from the leading "
      "edge to the trailing edge.\n\n"
      "Try more layers (Number of prism layers) and watch the note: snappyHexMesh holds the outer layer to 30 to 100 % of the "
      "surface cell next to it, so the stack can come out thinner than asked.",
      ST(3), EX(MESH_EX_AIRFOIL) },
    { PG_MESH, 3, NO, true, "stl-gen", "Airfoil: generate",
      "Generate mesh (Try it, then the pill). Afterwards Mesh > View shows it face on: fine cells hugging the profile, the "
      "layers round it, the cells growing out to the far field; and checkMesh says one cell deep.\n\n"
      "Next comes Physics: External aero sets up the run, and Lift and drag measures Cl and Cd per metre of span.",
      ST(4), EX(MESH_EX_AIRFOIL) },
    /* ---- the end */
    { PG_MESH, 3, NO, true, "stl-adv", "Every setting, when you need it",
      "The STL mesher's settings are a few numbers on purpose. When you need more, Advanced under Mesh settings opens Every "
      "OpenFOAM setting: every keyword of the mesher's dictionaries, with the value Q-Continuum writes and OpenFOAM's default, "
      "searchable. Change one and Generate writes yours; the arrow puts it back.",
      ST(3), 0 },
    { NO, NO, NO, false, NULL, "A good mesh, in one list",
      "Before running on a mesh, ask:\n\n"
      "1. Is the right space meshed (through or around), with room round the part?\n"
      "2. Are there small cells where the flow changes: walls, wakes, edges, curves?\n"
      "3. Do the layers give the y+ the turbulence model wants (about 1, or 30 to 300)?\n"
      "4. Does the size grow gently (about 1.2 a cell) from fine to coarse?\n"
      "5. Does checkMesh pass: non-orthogonality under 70, skewness under 4, no negative volumes?\n"
      "6. Does the answer stay the same on a finer mesh?\n\n"
      "The example cases stay in your workspace: delete them like any other case, or carry on to Physics with them.", 0, 0 },
};
#define N_MESH ((int)(sizeof MESH_STEPS / sizeof MESH_STEPS[0]))

typedef struct { const Step *step; int n; const char *label, *again; } Tour;
static const Tour TOURS[2] = {
    { STEPS, N_MAIN, "Tour", "Settings > Help > Take the tour" },
    { MESH_STEPS, N_MESH, "Meshing tutorial", "Settings > Help > Meshing tutorial" },
};
#define CUR (&TOURS[T.tour])
#define N_STEPS (CUR->n)

static struct {
    bool on, paused;
    int  tour;                 /* 0 the tour, 1 the meshing tutorial */
    bool chain;                /* the first start: the meshing tutorial follows the tour */
    int  at;                   /* the step shown */
    int  went;                 /* the step whose page was last gone to (-1: none) */
    int  scrolled;             /* the step whose lit widget was scrolled into view (-1: none) */
    int  scrollTries;
} T = { .went = -1, .scrolled = -1 };

bool tour_active(void) { return T.on; }

static void start(int tour, bool chain) { T.on = true; T.paused = false; T.tour = tour; T.chain = chain; T.at = 0; T.went = -1; T.scrolled = -1; }

static UiId target_id(const char *s);

/* the app's scroll view holding the point (logical units), the largest when nested; NULL: none */
static UiScroll *scroll_at(float px, float py)
{
    UiScroll *best = NULL;
    for (int i = 0; i < (int)(sizeof A.scr / sizeof A.scr[0]); i++) {
        UiScroll *s = &A.scr[i];
        if (s->view.height <= 0 || !CheckCollisionPointRec((Vector2){ px, py }, s->view)) continue;
        if (!best || s->view.width * s->view.height > best->view.width * best->view.height) best = s;
    }
    return best;
}

/* the lit widget below or above the page's view: the page scrolled to put it a third of the way down (the scroll whose
   view the widget was drawn in, from the app's scroll states). Tried for a few frames after arriving: the page has to
   be drawn once before its widgets are known. */
static void scroll_into_view(int k, const char *target)
{
    if (T.scrolled == k) return;
    if (T.scrolled != -2 - k) { T.scrolled = -2 - k; T.scrollTries = 0; }   /* (a step being tried) */
    if (++T.scrollTries > 90) { T.scrolled = k; return; }
    const UiWidget *w = ui_widget_find(target_id(target));
    float sc = ui_scale();
    if ((!w || w->layer != 0) && T.scrollTries > 2 && T.scrollTries % 2 == 0) {
        /* not drawn (off the page, culled): the page's scroll found from a widget that is there, moved down a half view */
        static const char *const ANCHOR[] = { "mesh-tabs#0", "mesh-tabs#3", "stl-step#0", "stl-step#1", "stl-step#2", "stl-step#3", "stl-step#4",
                                              "stl-view", "stl-mesher", "stl-density#0", "stl-base", "stl-target", "stl-next", "stl-adv", "stl-gen" };
        for (size_t a = 0; a < sizeof ANCHOR / sizeof *ANCHOR; a++) {
            const UiWidget *an = ui_widget_find(target_id(ANCHOR[a]));
            if (!an || an->layer != 0 || an->vis.width <= 0) continue;
            UiScroll *sc2 = scroll_at((an->vis.x + an->vis.width / 2) / sc, (an->vis.y + an->vis.height / 2) / sc);
            if (sc2) { ui_scroll_to(sc2, sc2->target + sc2->view.height * 0.6f); sc2->y = sc2->target; return; }   /* a jump: it is looking */
        }
        return;
    }
    if (!w || w->layer != 0) return;
    Rectangle r = { w->r.x / sc, w->r.y / sc, w->r.width / sc, w->r.height / sc }, v = { w->view.x / sc, w->view.y / sc, w->view.width / sc, w->view.height / sc };
    UiScroll *s = v.height > 0 ? scroll_at(v.x + v.width / 2, v.y + v.height / 2) : NULL;
    if (!s) { T.scrolled = k; return; }
    /* in view, clear of the bottom (where the card goes) */
    bool inView = r.y >= s->view.y + 8 && r.y + fminf(r.height, s->view.height * 0.3f) <= s->view.y + s->view.height * 0.72f;
    if (inView && fabsf(s->y - s->target) < 1) { if (T.scrollTries > 12) T.scrolled = k; return; }   /* (settled: a step opening scrolls the page itself) */
    if (inView) return;
    if (T.scrollTries > 60) { T.scrolled = k; return; }  /* the page ends there: as far as it goes */
    ui_scroll_to(s, s->y + (r.y - (s->view.y + s->view.height / 3)));   /* checked again next frame: the page may scroll too */
}

void tour_start(void) { start(0, false); }
void tour_start_mesh(void) { start(1, false); }
void tour_start_first(void) { start(0, true); }

static void tour_end(void)
{
    T.on = false;
    if (!A.set.tourSeen) { A.set.tourSeen = true; settings_save(&A.set); }
}

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
    const Step *s = &CUR->step[k];
    if (s->needCase && !A.cs) return;                     /* the practice case step asks first */
    if (s->ex1 && !stlmesh_example_open(s->ex1 - 1)) return;   /* so does an example's */
    if (s->page >= 0) app_go((Page)s->page);
    if (s->meshTab >= 0) A.meshTab = s->meshTab;
    if (s->physicsTab >= 0) A.physicsTab = s->physicsTab;
    if (s->stl1 && A.cs) stlmesh_focus(A.cs, s->stl1 - 1);
}

static void move(int d)
{
    int k = T.at + d;
    if (k < 0) k = 0;
    if (k >= N_STEPS) {
        if (T.tour == 0 && T.chain) { T.tour = 1; T.chain = false; T.at = 0; T.went = -1; return; }   /* on to the meshing tutorial */
        tour_end();
        return;
    }
    T.at = k;
}

static void draw_paused(void)
{
    char t[96]; snprintf(t, sizeof t, "%s paused (%d of %d)  ·  Resume", CUR->label, T.at + 1, N_STEPS);
    float bw = ui_button_width(ICON_BOOK_OPEN, t) + 8;
    Rectangle r = { 16, ui_height() - 56, bw, 40 };
    ui_occlude_begin((Rectangle){ r.x, r.y, r.width + 38, r.height });
    ui_shadow(r, 10, 0.6f);
    if (ui_button(ui_id("tour-resume"), r, ICON_BOOK_OPEN, t, BTN_PRIMARY)) { T.paused = false; T.went = -1; }
    Rectangle x = { r.x + r.width + 6, r.y + 4, 32, 32 };
    if (ui_button(ui_id("tour-end"), x, ICON_X, NULL, BTN_SECONDARY)) tour_end();
    ui_tooltip(x, T.tour ? "End the meshing tutorial (Settings > Help > Meshing tutorial brings it back)" : "End the tour (Settings > Help > Take the tour brings it back)");
    ui_occlude_end();
}

void tour_draw(void)
{
    if (!T.on || A.modal != MD_NONE) return;
    if (T.paused) { draw_paused(); return; }
    go_to(T.at);
    const Step *s = &CUR->step[T.at];
    if (s->target && !(s->needCase && !A.cs) && !(s->ex1 && !stlmesh_example_open(s->ex1 - 1))) scroll_into_view(T.at, s->target);
    bool needEx = s->ex1 && !stlmesh_example_open(s->ex1 - 1);
    bool waiting = (s->needCase && !A.cs) || needEx;
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
    ui_text(F_MED, 12.5f, ui_cap(CUR->label), x, y + 1, TH.accent);
    ui_text(F_MONO, 12.5f, cnt, x + ui_text_w(F_MED, 12.5f, ui_cap(CUR->label)) + 10, y + 1, TH.text3);
    Rectangle xr = { card.x + card.width - pad - 30, y - 4, 30, 30 };
    bool skip = ui_button(ui_id("tour-skip"), xr, ICON_X, NULL, BTN_GHOST);
    ui_tooltip(xr, TextFormat("Skip the %s (%s brings it back)", T.tour ? "meshing tutorial" : "tour", CUR->again));
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
    bool chainOn = last && T.tour == 0 && T.chain;
    const char *nl = chainOn ? "On to the meshing tutorial" : last ? "Finish" : T.at == 0 ? (T.tour ? "Start the tutorial" : "Start the tour") : "Next";
    float nw = ui_button_width(last && !chainOn ? ICON_CHECK : ICON_ARROW_RIGHT, nl);
    bx -= nw;
    bool next = false, back = false, tryit = false;
    if (waiting && needEx) {
        /* an example step: its case made (the step that introduces it moves on; a later one, after a case switch, stays) */
        const char *pl = "Make the example case";
        float pw = ui_button_width(ICON_FOLDER_PLUS, pl);
        bx += nw - pw;
        if (ui_button(ui_id("tour-example"), (Rectangle){ bx, by, pw, 36 }, ICON_FOLDER_PLUS, pl, BTN_PRIMARY) && stlmesh_example_case(s->ex1 - 1)) {
            if (s->page < 0) next = true; else T.went = -1;
        }
        nw = pw;
    } else if (waiting) {
        const char *pl = "Make a practice case";
        float pw = ui_button_width(ICON_FOLDER_PLUS, pl);
        bx += nw - pw;
        if (ui_button(ui_id("tour-practice"), (Rectangle){ bx, by, pw, 36 }, ICON_FOLDER_PLUS, pl, BTN_PRIMARY) && app_practice_case()) next = true;
        nw = pw;
    } else next = ui_button(ui_id("tour-next"), (Rectangle){ bx, by, nw, 36 }, last && !chainOn ? ICON_CHECK : ICON_ARROW_RIGHT, nl, BTN_PRIMARY);
    if (waiting && needEx && T.at + 1 < N_STEPS && s->page < 0) {   /* an example can be left out */
        float sw2 = ui_button_width(NULL, "Skip this example");
        bx -= sw2 + 10;
        if (ui_button(ui_id("tour-skip-ex"), (Rectangle){ bx, by, sw2, 36 }, NULL, "Skip this example", BTN_GHOST)) {
            int k = T.at + 1;
            while (k < N_STEPS && CUR->step[k].ex1 == s->ex1) k++;
            T.at = k < N_STEPS ? k : N_STEPS - 1; T.went = -1;
            ui_modal_end();
            return;
        }
    }
    if (T.at > 0 && !last && !waiting) {
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
        const char *sl = T.tour ? "Skip the tutorial" : "Skip the tour";
        float bw2 = ui_button_width(NULL, sl);
        bx -= bw2 + 10;
        skip |= ui_button(ui_id("tour-skip2"), (Rectangle){ bx, by, bw2, 36 }, NULL, sl, BTN_GHOST);
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
