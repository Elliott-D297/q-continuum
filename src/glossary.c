/* glossary.c -- the CFD and meshing glossary and its [[term]] markup (see glossary.h).
 *
 * Numbers used in more than one entry, worked once here so they agree with the meshing tutorial (tour.c):
 *   air, ν = 1.5e-5 m²/s, ρ = 1.2 kg/m³, U = 10 m/s along a flat plate, x = 1 m:
 *   Re_x = U x / ν = 6.7e5;  turbulent Cf = 0.026 Re_x^(-1/7) = 0.0038 (White);  τw = ½ ρ U² Cf = 0.23 Pa;
 *   u_τ = sqrt(τw / ρ) = 0.44 m/s;  y at y+ = 1: ν / u_τ = 34 µm;  y+ = 50: 1.7 mm;
 *   turbulent δ = 0.37 x Re_x^(-1/5) = 25 mm;  laminar (Blasius) δ = 5.0 x / sqrt(Re_x) = 6.1 mm.
 * OpenFOAM v2606 checks (primitiveMeshCheck.C): non-orthogonality 70°, skewness 4, aspect ratio 1000;
 * meshQualityDict: maxNonOrtho 65, maxInternalSkewness 4, maxBoundarySkewness 20. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "glossary.h"

static const GlossTerm T[] = {
    /* ---------------------------------------------------------------- the mesh itself */
    { "mesh", "Mesh", "grid",
      "The flow region cut into many small [[cell|cells]]. The solver finds one value of velocity, pressure and the rest "
      "for each cell, so the mesh decides what detail the answer can hold.",
      "Like the pixels of a photograph: a feature smaller than a few pixels is lost, however good the camera.",
      "A 2-D teaching case: 10 to 50 thousand cells. A 3-D intake or car: 1 to 10 million. Each million cells needs "
      "roughly 1 to 2 GB of memory for a steady incompressible run.",
      "constant/polyMesh: points, faces, owner, neighbour and boundary.",
      "cell|face|cell-count|checkmesh" },
    { "cell", "Cell (control volume)", "cells|control volume|element",
      "One of the small volumes the flow region is cut into. Mass, momentum and energy are balanced over each one: the "
      "integral conservation laws of fluid mechanics, applied to a tiny control volume.",
      "The control volume you drew round a pipe bend, made very small and repeated millions of times, each one passing "
      "flow to its neighbours through its [[face|faces]].",
      "Values are stored at the cell centre. Across one cell the solver assumes the flow changes in a straight line, so a "
      "velocity profile needs several cells across it to come out curved.",
      "polyMesh/owner and polyMesh/neighbour say which cells share each face; checkMesh prints the count as \"cells:\".",
      "face|hex|tet|poly|prism" },
    { "face", "Face", "faces",
      "A flat side shared by two [[cell|cells]]. The mass flow (and momentum and heat flow) through it is worked out "
      "here: what leaves one cell enters its neighbour. A face on the edge of the region belongs to a [[patch]].",
      "The surfaces of your control volume, where you wrote ρ U·A for the flow in and out.",
      "A hexahedral cell has 6 faces, a tetrahedron 4, a polyhedron typically 10 to 16.",
      "polyMesh/faces (the points of each face), owner and neighbour (the cells either side).",
      "cell|patch|non-orthogonality|skewness" },
    { "hex", "Hexahedron (hex)", "hexahedra|hexahedron|hexes|brick",
      "A six-sided brick-shaped [[cell]]. Lined up with the flow it is the most accurate and cheapest shape: its "
      "[[face|faces]] sit square to the line between neighbouring cell centres, and it fills space with the fewest cells.",
      "A wall of bricks: each one touches six neighbours face to face.",
      "About five tetrahedra fill the space of one hex of the same edge length, so a [[tet]] mesh needs several times "
      "the cells for the same size.",
      "blockMesh makes only hexes; snappyHexMesh makes mostly hexes. checkMesh counts them as \"hexahedra:\".",
      "tet|poly|prism|blockmesh|snappyhexmesh" },
    { "tet", "Tetrahedron (tet)", "tetrahedra|tetrahedron|tets",
      "A [[cell]] with four triangular faces. Tetrahedra fill any shape automatically, which is why CAD-based meshers "
      "(SALOME's NETGEN) use them, but their faces are not lined up with the flow, so they smear gradients more than "
      "[[hex|hexahedra]] and need more cells.",
      "A pyramid with a triangular base. Any shape can be filled with them, as any surface can be covered in triangles.",
      "Near walls a tet mesh needs [[prism-layers]]: tetrahedra in a boundary layer would have to be tiny in every "
      "direction.",
      "checkMesh counts them as \"tetrahedra:\". SALOME's tetrahedral mesher writes them; ideasUnvToFoam reads them in.",
      "hex|poly|prism|salome" },
    { "poly", "Polyhedron (poly)", "polyhedra|polyhedron|polyhedral",
      "A [[cell]] with many faces, often 10 to 16. Made by merging tetrahedra (SALOME's polyhedral mesher, "
      "polyDualMesh) or where snappyHexMesh splits a [[hex]] at a [[refinement-transition|change of refinement level]]. "
      "More neighbours than a [[tet]] means better gradients from fewer cells.",
      "A football's panels: each cell touches many neighbours.",
      "A polyhedral mesh has 3 to 5 times fewer cells than the tet mesh it was made from.",
      "checkMesh counts them as \"polyhedra:\".",
      "tet|hex|refinement-transition" },
    { "prism", "Prism (wedge cell)", "prisms|wedge cell",
      "A [[cell]] that is a triangle swept along a line: two triangular ends and three four-sided faces. [[prism-layers|Prism "
      "layers]] on a tetrahedral mesh are made of them.",
      "A Toblerone bar, cut into slices.",
      "On a tet mesh's wall every surface triangle grows a stack of prisms, one per layer.",
      "checkMesh counts them as \"prisms:\".",
      "prism-layers|tet" },
    { "cell-count", "Cell count", "cells in all|number of cells",
      "How many [[cell|cells]] the mesh has. Memory and run time grow with it; accuracy grows with it too, until "
      "[[mesh-independence|the answer stops changing]].",
      "Halve every cell's size in 3-D and there are 8 times as many cells. A transient run also needs time steps half "
      "as long (the [[courant|Courant number]]), so it takes about 16 times as long.",
      "Roughly 1 to 2 GB of memory per million cells; a laptop with 16 GB copes with about 5 million.",
      "checkMesh prints \"cells:\"; Mesh settings estimates it before meshing.",
      "mesh|mesh-independence|courant" },

    /* ---------------------------------------------------------------- sizes */
    { "base-size", "Base size", "base cell|background cell size",
      "The largest [[cell]], away from the part. Every other size is given as a share of it (as STAR-CCM+ does), so "
      "changing it scales the whole mesh.",
      "The coarsest grid the mesh starts from; near the part the cells are split finer.",
      "Start with about 20 to 50 base cells along the part's length, then refine near it.",
      "snappyHexMesh: the cell size of the [[background-mesh|background blockMesh]] (system/blockMeshDict).",
      "surface-size|background-mesh|refinement-level" },
    { "surface-size", "Surface size (target surface size)", "target surface size|surface cell|surface cells",
      "The size of the [[cell|cells]] on the part's surface, as a share of the [[base-size]]. It sets how well curves "
      "and edges come out and how fine the flow is resolved next to the part.",
      "A circle drawn with 8 straight segments looks like an octagon; with 36 it looks round.",
      "About 36 cells round a full circle shows a cylinder well. snappyHexMesh rounds the size to a whole "
      "[[refinement-level]]: base / 2, base / 4, base / 8 ...",
      "snappyHexMeshDict castellatedMeshControls refinementSurfaces { <surface> { level (min max); } }.",
      "base-size|refinement-level|curvature" },
    { "refinement-level", "Refinement level", "level|levels",
      "Each level halves the [[cell]] edge, splitting every [[hex]] into 8. Level 0 is the [[base-size|base cell]], "
      "level 3 an eighth of its size.",
      "Cutting a sugar cube into 8 smaller cubes, and each of those into 8 again.",
      "Level 3 puts 8 × 8 × 8 = 512 cells where one base cell was.",
      "snappyHexMeshDict: refinementSurfaces level (min max); refinementRegions levels ((distance level)); "
      "nCellsBetweenLevels.",
      "base-size|surface-size|refinement-transition|refinement-region" },
    { "refinement-transition", "Refinement transition", "transition|size step|nCellsBetweenLevels",
      "Where the [[cell]] size halves from one [[refinement-level]] to the next. The hexes split there, leaving "
      "[[poly|polyhedra]] with a face shared with four smaller neighbours, and the sudden size change adds a small error.",
      "A road narrowing from two lanes to one: traffic copes, but not well if it happens at a junction.",
      "Keep transitions out of jets, shear layers and wakes; at least 3 cells of one size before the next "
      "(nCellsBetweenLevels 3).",
      "snappyHexMeshDict castellatedMeshControls nCellsBetweenLevels.",
      "refinement-level|poly|expansion-ratio" },
    { "refinement-region", "Refinement region (volumetric control)", "volumetric control|refinement box",
      "A box, sphere or cylinder inside which the [[cell|cells]] are finer: round a wake, a gap, a jet, where the flow "
      "changes fast away from any wall.",
      "Drawing a box round the wake behind a car and asking for finer cells there.",
      "A wake box about 2 to 3 part lengths long behind the part, one level finer than the far field.",
      "snappyHexMeshDict: geometry { <name> { type box; min (...); max (...); } } and castellatedMeshControls "
      "refinementRegions { <name> { mode inside; levels ((1e15 <level>)); } }.",
      "refinement-level|domain" },
    { "curvature", "Curvature refinement", "points per circle|pts/circle|resolveFeatureAngle",
      "Finer [[surface-size|surface cells]] where the surface is curved, so a circle has enough cells round it to stay "
      "round.",
      "How many straight segments it takes to draw a circle smoothly.",
      "36 points per circle is a 10° turn per cell.",
      "snappyHexMeshDict resolveFeatureAngle: cells whose surface normals differ more than this are refined to the "
      "surface's maximum level.",
      "surface-size|feature-edge" },
    { "grading", "Grading", "simpleGrading|edgeGrading",
      "Cells changing size smoothly across a block, given as the size of the last [[cell]] divided by the first. Used to "
      "pack cells against a wall without making the whole block fine.",
      "Fence posts closer together near the gate and further apart away from it.",
      "Grading 10 over 20 cells is a ratio of 10^(1/19) = 1.13 from one cell to the next.",
      "blockMeshDict: hex (...) (nx ny nz) simpleGrading (gx gy gz); edgeGrading for one per edge.",
      "expansion-ratio|blockmesh" },

    /* ---------------------------------------------------------------- the boundary layer */
    { "boundary-layer", "Boundary layer", "",
      "The thin region next to a wall where viscosity matters: the velocity goes from zero at the wall (no-slip) to the "
      "free stream. The wall's shear stress, the drag and the heat transfer are all decided in it.",
      "The velocity profile u(y) from fluid mechanics: steepest at the wall, flattening out at its edge δ.",
      "Air at 10 m/s, 1 m along a flat plate (Re_x = 6.7e5): turbulent δ = 0.37 x Re_x^(-1/5) ≈ 25 mm; laminar "
      "(Blasius) δ = 5.0 x / sqrt(Re_x) ≈ 6 mm.",
      "",
      "prism-layers|y+|viscous-sublayer|wall-function" },
    { "prism-layers", "Prism layers (inflation layers)", "inflation layers|boundary-layer cells|layers|addLayers|layer addition",
      "Thin [[cell|cells]] stacked on the walls, thin across the wall and long along it, to follow the "
      "[[boundary-layer]], where the velocity changes from zero to the free stream over a short distance.",
      "The velocity profile is steepest at the wall, so the cells are thinnest there and grow outward, like the "
      "rings of a tree.",
      "5 to 20 layers, each 1.1 to 1.3 times the one below; the stack about as thick as the boundary layer.",
      "snappyHexMeshDict addLayersControls: layers { <patch> { nSurfaceLayers N; } }, expansionRatio, "
      "finalLayerThickness, relativeSizes, minThickness.",
      "first-layer|expansion-ratio|layer-thickness|y+" },
    { "first-layer", "First layer thickness", "first cell|first cell height|first layer|y1",
      "The height of the [[cell]] touching the wall. It sets the [[y+]] of the run: OpenFOAM measures y+ at that cell's "
      "centre, half this height from the wall.",
      "The bottom rung of the ladder of [[prism-layers|prism layers]].",
      "Air at 10 m/s, 1 m along a plate: y+ = 1 needs the cell centre 34 µm from the wall (a first layer about 70 µm "
      "thick); y+ = 50 needs 1.7 mm.",
      "snappyHexMeshDict addLayersControls firstLayerThickness (with thicknessModel firstAndExpansion or "
      "firstAndOverall).",
      "y+|prism-layers|expansion-ratio" },
    { "expansion-ratio", "Expansion ratio (stretching)", "stretching|growth rate|growth ratio|expansionRatio",
      "Each [[prism-layers|layer]] (or cell) this many times as thick as the one before it.",
      "A geometric series: layers t, rt, r²t ...; N of them add up to t (r^N - 1) / (r - 1).",
      "1.1 to 1.3 is smooth. Above about 1.5 the jump from one cell to the next adds an error to the equations, like a "
      "coarse step in a numerical integration.",
      "snappyHexMeshDict addLayersControls expansionRatio; blockMesh's per-cell ratio is the [[grading]]^(1/(n-1)).",
      "first-layer|layer-thickness|grading" },
    { "layer-thickness", "Layer stack thickness", "total thickness|total layer thickness|finalLayerThickness|final layer",
      "The [[prism-layers]] added up. Ideally they cover most of the [[boundary-layer]], and the outer one is close in "
      "size to the [[surface-size|surface cells]] beyond it so the step between them is small.",
      "The top of the ladder should reach the next floor: a thin outer layer under a big cell is a sudden jump.",
      "Q-Continuum keeps the outer layer 30 % to 100 % of the surface cell next to it (snappyHexMesh's "
      "finalLayerThickness 0.3 to 1 with relativeSizes true).",
      "snappyHexMeshDict addLayersControls finalLayerThickness, thickness, relativeSizes.",
      "prism-layers|first-layer|expansion-ratio" },
    { "y+", "y+ (wall distance in viscous units)", "y plus|yplus|y-plus",
      "How far the first [[cell]] centre is from the wall, measured in viscous lengths: y+ = y u_τ / ν, with the "
      "[[friction-velocity]] u_τ. It says which part of the turbulent [[boundary-layer]] that cell sits in.",
      "The law of the wall: under y+ 5 the [[viscous-sublayer]] (u+ = y+), 5 to 30 the buffer layer, 30 to about 300 "
      "the log layer.",
      "Resolving the wall (k-omega SST with no [[wall-function]]): y+ about 1. Wall functions: 30 to 300. Avoid 5 to 30. "
      "Air at 10 m/s, 1 m along a plate: y+ = 1 is 34 µm from the wall.",
      "postProcess -func yPlus (the run's yPlus function object); nutUSpaldingWallFunction copes with any y+.",
      "first-layer|friction-velocity|wall-function|viscous-sublayer" },
    { "friction-velocity", "Friction velocity", "u_tau|u tau|shear velocity",
      "u_τ = sqrt(τw / ρ): the wall's shear stress written as a velocity. It is the velocity scale of the flow next to "
      "the wall, and with ν it sets the viscous length ν / u_τ that [[y+]] counts in.",
      "Faster flow scrubs the wall harder (more τw), so the viscous layer gets thinner.",
      "Flat plate: τw = ½ ρ U² Cf with Cf = 0.026 Re_x^(-1/7). Air at 10 m/s, x = 1 m: Cf = 0.0038, τw = 0.23 Pa, "
      "u_τ = 0.44 m/s.",
      "",
      "y+|boundary-layer" },
    { "viscous-sublayer", "Viscous sublayer", "laminar sublayer",
      "The layer right against the wall, under [[y+]] 5, where viscosity dominates turbulence and the velocity rises in a "
      "straight line: u+ = y+.",
      "A film of slow fluid stuck to the wall that the eddies cannot reach.",
      "Air at 10 m/s, 1 m along a plate: about 0.17 mm thick.",
      "",
      "y+|wall-function|boundary-layer" },
    { "wall-function", "Wall function", "wall functions|nutkWallFunction",
      "A formula (the law of the wall) that links the first [[cell]], placed in the log layer, to the wall, instead of "
      "resolving the [[viscous-sublayer]] with many thin cells. Far fewer cells; less accurate where the flow separates "
      "or heat transfer matters.",
      "Using the Moody chart for the pressure drop instead of working out the velocity profile.",
      "Needs a first cell centre at [[y+]] 30 to 300.",
      "0/nut: nutkWallFunction (or nutUSpaldingWallFunction for any y+); 0/k: kqRWallFunction; 0/omega: "
      "omegaWallFunction; 0/epsilon: epsilonWallFunction.",
      "y+|viscous-sublayer|first-layer" },

    /* ---------------------------------------------------------------- how a mesh is made */
    { "background-mesh", "Background mesh", "background blockMesh",
      "The block of [[hex|hexahedra]] (from blockMesh) that [[snappyhexmesh]] starts from and refines near the part. "
      "Its cells are the [[base-size]].",
      "A sheet of squared paper laid over the part, the squares split finer where the outline crosses them.",
      "Its cells should be close to cubes (aspect ratio 1), so every refinement level is cubes too.",
      "system/blockMeshDict, a single block round the part (or the whole domain).",
      "base-size|blockmesh|snappyhexmesh" },
    { "castellation", "Castellation", "castellate|castellated|castellatedMesh",
      "snappyHexMesh's first stage: the [[background-mesh]] is refined near the surface, then the cells on the wrong "
      "side of it are thrown away, leaving a stair-stepped outline.",
      "A castle wall's battlements: the curved part drawn in square blocks.",
      "After it the mesh is all hexes and the surface looks like Lego.",
      "snappyHexMeshDict castellatedMesh true; castellatedMeshControls { maxGlobalCells, refinementSurfaces, "
      "refinementRegions, locationInMesh ... }.",
      "snapping|location-in-mesh|refinement-level" },
    { "snapping", "Snapping", "snap",
      "snappyHexMesh's second stage: the stair-stepped points are moved onto the real surface and the cells behind them "
      "smoothed, so the mesh follows the part.",
      "Pressing cling film onto a shape: the grid deforms to sit on the surface.",
      "Sharp corners only stay sharp when they are [[feature-edge|feature edges]] the points can snap to.",
      "snappyHexMeshDict snap true; snapControls { nSmoothPatch, tolerance, nSolveIter, nRelaxIter, nFeatureSnapIter }.",
      "castellation|feature-edge|prism-layers" },
    { "feature-edge", "Feature edge (feature angle)", "feature angle|features|sharp edge|featureAngle|includedAngle",
      "A sharp edge of the part, where two faces meet at more than the feature angle. The mesher snaps points to it so "
      "the corner stays crisp instead of being rounded off.",
      "The edge of a table top: smoothing over it would change where the flow separates.",
      "A feature angle of 30° keeps every bend sharper than 30° (surfaceFeatureExtract's includedAngle 150).",
      "system/surfaceFeatureExtractDict includedAngle; snappyHexMeshDict castellatedMeshControls "
      "features ({ file \"<part>.eMesh\"; level N; }).",
      "snapping|curvature" },
    { "location-in-mesh", "Point in the fluid (locationInMesh)", "locationInMesh|location in mesh",
      "A point inside the fluid that tells snappyHexMesh which side of the surface to keep: inside a pipe for "
      "internal flow, out in the tunnel for flow round a part.",
      "Pointing at the room you want to keep when a floor plan has many rooms.",
      "It must not sit exactly on a [[face]] of the [[background-mesh]], or inside the part.",
      "snappyHexMeshDict castellatedMeshControls locationInMesh (x y z);",
      "castellation|domain" },
    { "domain", "Domain (wind tunnel)", "computational domain|wind tunnel|tunnel",
      "The whole region that is meshed. For flow round a part it is a box round it, a wind tunnel, whose walls must be "
      "far enough away not to change the flow at the part.",
      "A real wind tunnel's walls squeeze the flow round a large model; the same happens in a too-small domain.",
      "About 3 to 5 part lengths ahead, above, below and to the sides, 8 to 10 behind; the part blocking no more than a "
      "few percent of the cross section.",
      "",
      "refinement-region|location-in-mesh|patch" },
    { "stl", "STL surface", "stl file",
      "A part's surface as a list of triangles, with no units and no named faces unless the file has several solids. "
      "Most CAD programs export it.",
      "A paper model folded from flat triangles: a curve is only as round as its triangles are small.",
      "It must be [[watertight]] to mesh the inside or outside of it. Units are a guess: CAD often writes millimetres.",
      "constant/triSurface/<part>.stl for snappyHexMesh.",
      "watertight|cad" },
    { "cad", "CAD file (STEP, IGES)", "step|step file|iges|brep",
      "A part as exact geometry: true curves and named faces, not triangles. SALOME meshes it directly, and the faces "
      "become boundaries you can name.",
      "The drawing itself, rather than a photograph of it.",
      "SALOME reads STEP and IGES in metres whatever unit the file was drawn in.",
      "",
      "stl|salome" },
    { "watertight", "Closed (watertight) surface", "closed|closed surface|open edges|open edge",
      "A surface with no gaps: every triangle edge is shared by exactly two triangles. Only then does inside and outside "
      "mean something, so the mesher can fill one side.",
      "A balloon holds air; a balloon with a hole does not.",
      "One open edge is enough to stop a mesher telling inside from outside.",
      "surfaceCheck <part>.stl reports open edges.",
      "stl" },

    /* ---------------------------------------------------------------- meshers */
    { "blockmesh", "blockMesh", "block mesh",
      "OpenFOAM's structured mesher: the region is built from six-sided blocks, each with its 8 corners, its cell "
      "counts and its [[grading]]. Every cell is a [[hex]], lined up as you chose.",
      "Building a shape from deformable bricks, then cutting each brick into a grid.",
      "The lid-driven cavity is one block of 20 × 20 cells.",
      "system/blockMeshDict: vertices, blocks, edges (arc, spline, polyLine), boundary.",
      "hex|grading|background-mesh" },
    { "snappyhexmesh", "snappyHexMesh", "snappy",
      "OpenFOAM's mesher for any shape from an [[stl]]: [[castellation]], [[snapping]], then [[prism-layers]]. The result "
      "is mostly [[hex|hexahedra]], with polyhedra where sizes change and near the surface.",
      "Squared paper laid over the part, refined at its outline, pressed onto it, then lined with thin cells.",
      "The most robust choice for a part from an STL, with layers on any part.",
      "system/snappyHexMeshDict (castellatedMeshControls, snapControls, addLayersControls, meshQualityControls).",
      "castellation|snapping|prism-layers|background-mesh" },
    { "salome", "SALOME (NETGEN)", "netgen",
      "An open-source CAD and meshing program. Its NETGEN mesher fills a [[cad]] part with [[tet|tetrahedra]] or "
      "[[poly|polyhedra]], with [[prism-layers]] on flat CAD faces.",
      "A mesher that works from the drawing's true faces, not from triangles.",
      "Best from a STEP file; from an STL it has to rebuild faces first.",
      "Q-Continuum runs it without its window; the mesh comes in through ideasUnvToFoam.",
      "tet|poly|cad" },

    /* ---------------------------------------------------------------- boundaries */
    { "patch", "Patch (boundary)", "patches|boundary|boundaries",
      "A named group of boundary [[face|faces]] where one boundary condition applies: an inlet, an outlet, the walls. "
      "Its type says what kind of boundary it is: patch, wall, symmetry, empty, wedge.",
      "The labelled edges of your control volume: flow in here, out there, no-slip on these.",
      "A duct has three: inlet, outlet, walls.",
      "constant/polyMesh/boundary (type, nFaces, startFace); the values in each 0/ field file.",
      "wall|empty|symmetry|face" },
    { "wall", "Wall (patch type)", "wall patch|walls",
      "A [[patch]] of type wall: no flow through it, no-slip on it. Turbulence models treat it as a wall ([[y+]], "
      "[[wall-function|wall functions]]) and [[prism-layers]] grow on it.",
      "The solid surface the boundary layer grows on.",
      "After a run, check each wall's y+ against the turbulence model's range.",
      "constant/polyMesh/boundary: type wall;",
      "patch|y+|prism-layers" },
    { "empty", "Empty (2-D front and back)", "empty patch|2-d|2d|two-d|one cell thick",
      "OpenFOAM is always 3-D. A 2-D case is one [[cell]] thick, and its front and back are a [[patch]] of type empty: "
      "nothing is solved in that direction.",
      "A slice of the flow between two glass plates that the fluid ignores.",
      "Exactly one cell across the thin direction, or the solver stops with an error.",
      "constant/polyMesh/boundary: type empty; the 0/ fields: type empty.",
      "patch|wedge" },
    { "wedge", "Wedge (axisymmetric)", "axisymmetric|axi-symmetric",
      "A flow that is the same at every angle round an axis (a pipe, a nozzle) is meshed as a thin slice, one cell thick, "
      "with a [[patch]] of type wedge either side.",
      "A slice of cake that stands for the whole cake.",
      "The slice angle under 5°, and the axis along the wedge's edge.",
      "constant/polyMesh/boundary: type wedge; made with extrudeMesh (wedge) or blockMesh.",
      "empty|patch" },
    { "symmetry", "Symmetry plane", "symmetryPlane|symmetry plane",
      "A mirror: no flow through it and no shear along it. Halves the cells when the part and the flow are symmetric.",
      "A mirror down the middle of a car: model one half, see both.",
      "Only when the flow really is symmetric: a wake that sheds vortices from side to side is not.",
      "constant/polyMesh/boundary: type symmetryPlane (a flat plane) or symmetry.",
      "patch" },

    /* ---------------------------------------------------------------- quality */
    { "checkmesh", "checkMesh", "check mesh",
      "OpenFOAM's check of the mesh on disk: the number of cells of each shape, and the quality numbers "
      "([[non-orthogonality|non-orthogonality]], [[skewness]], [[aspect-ratio]], volumes). It ends \"Mesh OK.\" or flags what fails with ***.",
      "A building inspector going round every cell.",
      "Fails at non-orthogonality over 70°, skewness over 4, aspect ratio over 1000, or any [[negative-volume]].",
      "checkMesh [-allGeometry -allTopology]; the log is log.checkMesh.",
      "non-orthogonality|skewness|aspect-ratio|negative-volume" },
    { "non-orthogonality", "Non-orthogonality", "non-orthogonal|nonOrtho|maxNonOrtho",
      "The angle between the line joining two [[cell]] centres and the [[face]] between them, against its normal. 0° is "
      "ideal (a grid of cubes). The larger it is, the less accurate the diffusion and pressure terms worked out across "
      "the face.",
      "Measuring the slope of a hill by walking at an angle across it instead of straight up it.",
      "Under 65° fine; checkMesh warns over 70°; over about 85° most solvers fail.",
      "fvSolution nNonOrthogonalCorrectors; fvSchemes laplacianSchemes Gauss linear limited corrected 0.5; "
      "meshQualityDict maxNonOrtho 65.",
      "skewness|checkmesh" },
    { "skewness", "Skewness", "skew|maxInternalSkewness",
      "How far the line joining two [[cell]] centres misses the middle of the [[face]] between them. Values passed to the "
      "face are interpolated at the wrong point, which loses accuracy.",
      "Reading a thermometer that hangs off to the side of where you want the temperature.",
      "checkMesh flags faces over 4; snappyHexMesh allows 4 inside the mesh and 20 on the boundary.",
      "meshQualityDict maxInternalSkewness 4, maxBoundarySkewness 20.",
      "non-orthogonality|checkmesh" },
    { "aspect-ratio", "Aspect ratio", "aspect",
      "A cell's longest side over its shortest. Long thin cells are fine where the flow changes slowly along them (the "
      "[[prism-layers]] in a boundary layer), bad where it changes in every direction.",
      "A plank laid along the flow is fine; turned across it, it averages over too much.",
      "Hundreds in the boundary layer are normal; checkMesh flags over 1000.",
      "checkMesh \"Max aspect ratio\".",
      "checkmesh|prism-layers" },
    { "negative-volume", "Negative volume (inverted cell)", "negative volumes|inverted cell|min volume",
      "A [[cell]] turned inside out, so its volume works out below zero. No solver can run on it.",
      "A box folded so its lid passes through its bottom.",
      "Must be zero cells.",
      "checkMesh \"Min volume\" and \"***Zero or negative cell volume\"; meshQualityDict minVol, minDeterminant.",
      "checkmesh" },
    { "mesh-independence", "Mesh independence (grid convergence)", "grid convergence|mesh convergence|grid independence",
      "Refining the mesh until the answer you care about (drag, pressure drop) stops changing. Until then the answer "
      "depends on the mesh, not only on the physics.",
      "Measuring a coastline with a shorter and shorter ruler until the length settles.",
      "Three meshes, each about twice the cells of the one before; when the two finest agree within a percent or two, "
      "the middle one is enough.",
      "Duplicate case or a Study runs the meshes side by side.",
      "cell-count|mesh" },
    { "courant", "Courant number", "co|cfl|courant number",
      "Co = |U| Δt / Δx: how many [[cell|cells]] the flow crosses in one time step. A transient run's time step is tied "
      "to the cell size by it.",
      "A camera's shutter: if the subject moves more than a pixel in one frame, the picture smears.",
      "Under 1 for most transient runs; halving the cells halves the time step.",
      "system/controlDict deltaT, maxCo, adjustTimeStep.",
      "cell-count" },
};
#define NT ((int)(sizeof T / sizeof T[0]))

int gloss_count(void) { return NT; }
const GlossTerm *gloss_at(int i) { return i >= 0 && i < NT ? &T[i] : NULL; }
int gloss_index(const GlossTerm *t) { return t ? (int)(t - T) : -1; }

/* w (len n) equals one of the '|' separated words in list, any case */
static bool in_list(const char *list, const char *w, size_t n)
{
    for (const char *p = list; p && *p;) {
        const char *e = strchr(p, '|');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && !strncasecmp(p, w, n)) return true;
        p = e ? e + 1 : NULL;
    }
    return false;
}

const GlossTerm *gloss_find(const char *word)
{
    if (!word || !*word) return NULL;
    size_t n = strlen(word);
    for (int i = 0; i < NT; i++) if (!strcasecmp(T[i].key, word)) return &T[i];
    for (int i = 0; i < NT; i++) if (in_list(T[i].aliases, word, n)) return &T[i];
    for (int i = 0; i < NT; i++) if (!strcasecmp(T[i].name, word)) return &T[i];
    return NULL;
}

bool gloss_see(const GlossTerm *t, int n, char *out, size_t cap)
{
    const char *p = t ? t->see : NULL;
    for (int k = 0; p && *p; k++) {
        const char *e = strchr(p, '|');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (k == n) { snprintf(out, cap, "%.*s", (int)l, p); return true; }
        p = e ? e + 1 : NULL;
    }
    return false;
}

static const char *ci_str(const char *h, const char *n) { return h && *n ? strcasestr(h, n) : NULL; }

int gloss_search(const char *q, int *out, int max)
{
    while (q && *q == ' ') q++;
    int n = 0;
    if (!q || !*q) { for (int i = 0; i < NT && n < max; i++) out[n++] = i; return n; }
    /* tiers: name/key starts with it, name/key/alias contains it, the text contains it */
    for (int tier = 0; tier < 3; tier++)
        for (int i = 0; i < NT && n < max; i++) {
            bool hit;
            if (tier == 0) hit = !strncasecmp(T[i].name, q, strlen(q)) || !strncasecmp(T[i].key, q, strlen(q));
            else if (tier == 1) hit = ci_str(T[i].name, q) || ci_str(T[i].key, q) || ci_str(T[i].aliases, q);
            else hit = ci_str(T[i].what, q) || ci_str(T[i].picture, q) || ci_str(T[i].number, q) || ci_str(T[i].foam, q);
            if (!hit) continue;
            bool seen = false;
            for (int k = 0; k < n; k++) if (out[k] == i) { seen = true; break; }
            if (!seen) out[n++] = i;
        }
    return n;
}

int gloss_strip(const char *s, char *out, size_t cap, GlossSpan *spans, int max)
{
    size_t o = 0;
    int ns = 0;
    if (!cap) return 0;
    for (const char *p = s ? s : ""; *p;) {
        const char *close = p[0] == '[' && p[1] == '[' ? strstr(p + 2, "]]") : NULL;
        if (close) {
            const char *in = p + 2, *bar = memchr(in, '|', (size_t)(close - in));
            const char *show = bar ? bar + 1 : in;
            size_t kl = (size_t)((bar ? bar : close) - in), sl = (size_t)(close - show);
            int st = (int)o;
            for (size_t k = 0; k < sl && o + 1 < cap; k++) out[o++] = !bar && show[k] == '-' ? ' ' : show[k];   /* [[prism-layers]]: "prism layers" */
            if (ns < max && spans) {
                spans[ns].start = st; spans[ns].end = (int)o;
                snprintf(spans[ns].key, sizeof spans[ns].key, "%.*s", (int)kl, in);
                ns++;
            }
            p = close + 2;
            continue;
        }
        if (o + 1 < cap) out[o++] = *p;
        p++;
    }
    out[o] = 0;
    return ns;
}
