/* meshopts.c -- the keyword table and the user's values (see meshopts.h) */
#include "meshopts.h"
#include "foamdict.h"
#include "stlmesh.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SHM "system/snappyHexMeshDict"
#define MQD "system/meshQualityDict"
#define BMD "system/blockMeshDict"
#define SFE "system/surfaceFeatureExtractDict"
#define EMD "system/extrudeMeshDict"
#define CFD "system/meshDict"
#define FHD "system/foamyHexMeshDict"
#define COL "system/collapseDict"
#define FQD "system/foamyQuadMeshDict"
#define E2D "system/extrude2DMeshDict"

#define TF "true|false"
#define OO "on|off"
#define YN "yes|no"
#define B10 "1|0"

/* group names: the panel lists them in this order */
#define G_STEPS  "snappyHexMesh: steps and general"
#define G_CAST   "snappyHexMesh: castellation (refining the background)"
#define G_SURF   "snappyHexMesh: each boundary"
#define G_REG    "snappyHexMesh: each refinement region"
#define G_SNAP   "snappyHexMesh: snapping to the surface"
#define G_LAY    "snappyHexMesh: prism layers"
#define G_PLAY   "snappyHexMesh: prism layers on each boundary"
#define G_MQC    "snappyHexMesh: quality while meshing"
#define G_MQD    "Mesh quality limits (meshQualityDict)"
#define G_BMD    "Background mesh (blockMeshDict)"
#define G_SFE    "Feature edges (surfaceFeatureExtractDict)"
#define G_EMD    "2-D extrusion (extrudeMeshDict)"
#define G_CF     "cfMesh: sizes and general"
#define G_CFP    "cfMesh: each boundary"
#define G_CFR    "cfMesh: each refinement region"
#define G_CFL    "cfMesh: prism layers"
#define G_FSC    "foamyHexMesh: surface conformation"
#define G_FIP    "foamyHexMesh: initial points"
#define G_FMC    "foamyHexMesh: motion and cell size"
#define G_FPF    "foamyHexMesh: filtering and decomposition"
#define G_COL    "foamyHexMesh: collapsing short edges (collapseDict)"
#define G_FQ     "foamyQuadMesh (2-D)"
#define G_E2D    "2-D extrusion (extrude2DMeshDict)"
#define G_CMD    "Command-line options"

#define SN MW_SNAPPY
const MeshOpt MESH_OPTS[] = {
    /* ---------------------------------------------------------------- snappyHexMeshDict: top level */
    { SN, SHM, "castellatedMesh", G_STEPS, "Castellate", "Refine the background cells round the surface and cut away the ones outside the fluid. The first stage.", MO_BOOL, TF, NULL, 0 },
    { SN, SHM, "snap", G_STEPS, "Snap", "Move the stair-stepped boundary onto the surface. Off leaves the blocky castellated mesh.", MO_BOOL, TF, NULL, 0 },
    { SN, SHM, "addLayers", G_STEPS, "Add layers", "Grow prism layers on the walls (the layers set below and under Prism layers).", MO_BOOL, TF, NULL, 0 },
    { SN, SHM, "type", G_STEPS, "Mesh type", "castellatedBufferLayer adds a buffer layer at the surface before snapping (newer, experimental).", MO_WORD, "castellated|castellatedBufferLayer", "castellated", 0 },
    { SN, SHM, "singleRegionName", G_STEPS, "Single-region patch names", "A surface of one region names its patch after the surface (else surface_region).", MO_BOOL, TF, "true", 0 },
    { SN, SHM, "mergePatchFaces", G_STEPS, "Merge patch faces", "Merge boundary faces of one cell that lie in one plane. Off keeps the mesh usable for later refinement.", MO_BOOL, TF, "true", 0 },
    { SN, SHM, "mergeAcrossPatches", G_STEPS, "Merge across patches", "Merge such faces even across patches (can help the layers cover corners).", MO_BOOL, TF, "false", 0 },
    { SN, SHM, "keepPatches", G_STEPS, "Keep empty patches", "Keep patches that ended up with no faces (else they are removed).", MO_BOOL, TF, "false", 0 },
    { SN, SHM, "mergeTolerance", G_STEPS, "Merge tolerance", "Points closer than this fraction of the mesh's size are one point. The write precision must be finer.", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "debugFlags", G_STEPS, "Debug flags", "Extra output: a list of mesh, intersections, featureSeeds, attraction, layerInfo.", MO_TEXT, NULL, "()", 0 },
    { SN, SHM, "writeFlags", G_STEPS, "Write flags", "Extra fields: a list of scalarLevels (refinement level per cell), layerSets, layerFields (layer coverage).", MO_TEXT, NULL, "()", 0 },
    { SN, SHM, "setFormat", G_STEPS, "Line format", "File format for lines it writes (a leak path).", MO_WORD, "vtk|ensight|raw|csv|gnuplot|xmgr", "vtk", 0 },
    { SN, SHM, "surfaceFormat", G_STEPS, "Surface format", "File format for surfaces it writes (leak-closure faces).", MO_WORD, "vtk|ensight|raw|obj|stl|foam", "vtk", 0 },

    /* ---------------------------------------------------------------- castellatedMeshControls */
    { SN, SHM, "castellatedMeshControls/maxLocalCells", G_CAST, "Max cells per processor", "Above this on one processor, it balances before refining instead of after.", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "castellatedMeshControls/maxGlobalCells", G_CAST, "Max cells in all", "Refinement stops at once on reaching this (before the cells outside are removed). A guard against running out of memory.", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "castellatedMeshControls/minRefinementCells", G_CAST, "Stop below this many cells to refine", "Ends the refinement loop when so few cells are left to split (-1: no refinement).", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "castellatedMeshControls/maxLoadUnbalance", G_CAST, "Allowed imbalance", "Fraction off perfect balance between processors before it rebalances (0: always).", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "castellatedMeshControls/maxCellUnbalance", G_CAST, "Cell-count imbalance trigger", "Skip balancing while the cells added or the excess on a processor are below this (-1: off).", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "castellatedMeshControls/balanceAtEnd", G_CAST, "Balance at the end", "Balance the processors once refinement is done.", MO_BOOL, TF, "false", 0 },
    { SN, SHM, "castellatedMeshControls/nCellsBetweenLevels", G_CAST, "Cells between levels", "Layers of cells of one size before the size halves again. More: smoother growth (lower volume growth), more cells.", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "castellatedMeshControls/features", G_CAST, "Feature-edge refinement", "A list of { file \"x.eMesh\"; level N; } or levels ((distance level)): cells on the edges refined.", MO_TEXT, NULL, NULL, 0 },
    { SN, SHM, "castellatedMeshControls/resolveFeatureAngle", G_CAST, "Resolve feature angle", "Where the surface bends more than this inside one cell, refine to the maximum level. Smaller: more refinement on curves.", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "castellatedMeshControls/planarAngle", G_CAST, "Planar angle", "Normals closer than this are the same plane or opposite (gaps, baffles, snapping).", MO_REAL, NULL, "= resolveFeatureAngle", 0 },
    { SN, SHM, "castellatedMeshControls/gapLevelIncrement", G_CAST, "Extra levels in gaps", "Levels added above the maximum in narrow gaps.", MO_INT, NULL, "0", 0 },
    { SN, SHM, "castellatedMeshControls/locationInMesh", G_CAST, "Point in the fluid", "The mesh kept is the part this point is in. It must not sit on a face.", MO_VEC, NULL, NULL, 0 },
    { SN, SHM, "castellatedMeshControls/locationsInMesh", G_CAST, "Points in the fluid, with zones", "Several regions: ((x y z) zoneName) ...; 'none' is the unzoned part. Use instead of the one point.", MO_TEXT, NULL, NULL, 0 },
    { SN, SHM, "castellatedMeshControls/locationsOutsideMesh", G_CAST, "Points that must be outside", "((x y z) ...): meshing stops (or closes the leak) if the fluid reaches one.", MO_TEXT, NULL, NULL, 0 },
    { SN, SHM, "castellatedMeshControls/allowFreeStandingZoneFaces", G_CAST, "Free-standing zone faces", "Keep faceZone faces that aren't on a cellZone's boundary.", MO_BOOL, TF, NULL, 0 },
    { SN, SHM, "castellatedMeshControls/handleSnapProblems", G_CAST, "Remove problem cells", "Remove cells likely to snap badly before snapping.", MO_BOOL, TF, "true", 0 },
    { SN, SHM, "castellatedMeshControls/useTopologicalSnapDetection", G_CAST, "Topological snap detection", "Off: find cells to be squashed geometrically instead.", MO_BOOL, TF, "true", 0 },
    { SN, SHM, "castellatedMeshControls/interfaceRefine", G_CAST, "Refine across level interfaces", "Refine surface cells whose opposite faces have different levels.", MO_BOOL, TF, "true", 0 },
    { SN, SHM, "castellatedMeshControls/nCellZoneErodeIter", G_CAST, "Cell zone erosion", "Erosion passes giving left-over cells to the background (negative: grow the zones).", MO_INT, NULL, "0", 0 },
    { SN, SHM, "castellatedMeshControls/minCellFraction", G_CAST, "Smallest region kept (fraction)", "Remove regions smaller than this fraction of the mesh.", MO_REAL, NULL, "0", 0 },
    { SN, SHM, "castellatedMeshControls/nMinCells", G_CAST, "Smallest region kept (cells)", "Remove regions with fewer cells than this.", MO_INT, NULL, "0", 0 },
    { SN, SHM, "castellatedMeshControls/useLeakClosure", G_CAST, "Close leaks", "Close a leak between the inside and outside points itself (else stop).", MO_BOOL, TF, "false", 0 },
    { SN, SHM, "castellatedMeshControls/nFilterIter", G_CAST, "Filter passes", "Passes filtering out cells that would snap badly.", MO_INT, NULL, NULL, 0 },

    /* ---------------------------------------------------------------- refinementSurfaces, per boundary */
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/level", G_SURF, "Refinement levels", "(min max): every cell on it refined to min, to max where it bends more than the resolve feature angle. Each level halves the cell.", MO_TEXT, NULL, NULL, MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/patchInfo/type", G_SURF, "Patch type", "What the patch is made: wall or patch (no constraint types here).", MO_WORD, "wall|patch", "wall", MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/patchInfo/inGroups", G_SURF, "Patch groups", "(group1 group2): groups the patch belongs to.", MO_TEXT, NULL, NULL, MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/gapLevel", G_SURF, "Gap refinement", "(cells min max): at least this many cells across a narrow gap, between those levels.", MO_TEXT, NULL, NULL, MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/gapMode", G_SURF, "Gap side", "Which side's gaps count.", MO_WORD, "mixed|inside|outside", "mixed", MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/gapSelf", G_SURF, "Gaps within the surface", "Count gaps between parts of this one surface.", MO_BOOL, TF, "true", MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/gapLevelIncrement", G_SURF, "Extra levels in gaps", "Levels added above the maximum in narrow gaps on this surface.", MO_INT, NULL, NULL, MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/perpendicularAngle", G_SURF, "Perpendicular angle", "Refine where small cells meet large ones across a flat surface (degrees; negative: off).", MO_REAL, NULL, NULL, MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/curvatureLevel", G_SURF, "Curvature refinement", "(cellsPerRadius startLevel maxLevel minRadiusLevel): refine by the radius of curvature.", MO_TEXT, NULL, NULL, MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/blockLevel", G_SURF, "Block thin gaps", "Remove cells in gaps thinner than two cells of this level.", MO_INT, NULL, NULL, MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/leakLevel", G_SURF, "Leak check level", "Check for a leak once refinement reaches this level (not only at the end).", MO_INT, NULL, NULL, MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/faceZone", G_SURF, "Face zone", "Put its faces in a faceZone of this name (an internal surface: a baffle, a porous face, a fan).", MO_TEXT, NULL, NULL, MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/cellZone", G_SURF, "Cell zone", "A closed surface: the cells inside it in a cellZone of this name (MRF, porosity, a solid).", MO_TEXT, NULL, NULL, MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/cellZoneInside", G_SURF, "Which cells are the zone", "inside, outside, or the side of insidePoint.", MO_WORD, "inside|outside|insidePoint|none", NULL, MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/insidePoint", G_SURF, "Inside point", "A point in the zone (cellZoneInside insidePoint).", MO_VEC, NULL, NULL, MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/faceType", G_SURF, "Face zone faces become", "internal faces, baffles (two walls back to back), or free boundary faces.", MO_WORD, "internal|baffle|boundary", "internal", MO_PATCH },
    { SN, SHM, "castellatedMeshControls/refinementSurfaces/%s/addBufferLayers", G_SURF, "Buffer layers here", "With type castellatedBufferLayer: add the buffer layer on this surface.", MO_BOOL, TF, NULL, MO_PATCH },

    /* ---------------------------------------------------------------- refinementRegions, per region */
    { SN, SHM, "castellatedMeshControls/refinementRegions/%s/mode", G_REG, "Mode", "inside / outside the shape, or by distance from it.", MO_WORD, "inside|outside|distance", NULL, MO_REGION },
    { SN, SHM, "castellatedMeshControls/refinementRegions/%s/levels", G_REG, "Levels", "((distance level) ...): inside/outside take one pair, the distance ignored; distance mode, increasing distances.", MO_TEXT, NULL, NULL, MO_REGION },
    { SN, SHM, "castellatedMeshControls/refinementRegions/%s/gapLevel", G_REG, "Gap refinement", "(cells min max): at least this many cells across narrow gaps inside it.", MO_TEXT, NULL, NULL, MO_REGION },
    { SN, SHM, "castellatedMeshControls/refinementRegions/%s/gapMode", G_REG, "Gap side", "Which side's gaps count.", MO_WORD, "mixed|inside|outside", "mixed", MO_REGION },
    { SN, SHM, "castellatedMeshControls/refinementRegions/%s/gapSelf", G_REG, "Gaps within one surface", "Count gaps between parts of one surface.", MO_BOOL, TF, "true", MO_REGION },
    { SN, SHM, "castellatedMeshControls/refinementRegions/%s/levelIncrement", G_REG, "Directional refinement", "(minLevel maxLevel (nx ny nz)): split cells of those levels only along x, y, z, after all else (a wake, a thin gap).", MO_TEXT, NULL, NULL, MO_REGION },
    { SN, SHM, "castellatedMeshControls/refinementRegions/%s/smoothDirection", G_REG, "Smoothing direction", "Smooth the size jumps along this direction after refinement.", MO_VEC, NULL, NULL, MO_REGION },
    { SN, SHM, "castellatedMeshControls/refinementRegions/%s/nSmoothExpansion", G_REG, "Expansion smoothing passes", "Passes smoothing the expansion ratio along that direction.", MO_INT, NULL, NULL, MO_REGION },
    { SN, SHM, "castellatedMeshControls/refinementRegions/%s/nSmoothPosition", G_REG, "Position smoothing passes", "Passes smoothing point positions along that direction.", MO_INT, NULL, NULL, MO_REGION },

    /* ---------------------------------------------------------------- snapControls */
    { SN, SHM, "snapControls/nSmoothPatch", G_SNAP, "Patch smoothing passes", "Smooth the boundary before finding where it goes on the surface.", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "snapControls/nSmoothInternal", G_SNAP, "Interface smoothing passes", "Smooth points on refinement interfaces inside: lowers non-orthogonality there.", MO_INT, NULL, "= nSmoothPatch", 0 },
    { SN, SHM, "snapControls/tolerance", G_SNAP, "Attraction distance", "Points within this many local edge lengths are pulled to the surface.", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "snapControls/nSolveIter", G_SNAP, "Displacement passes", "Relaxation passes spreading the snapping movement into the mesh.", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "snapControls/nRelaxIter", G_SNAP, "Snapping relaxation passes", "Tries at scaling back the movement where quality fails.", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "snapControls/nFeatureSnapIter", G_SNAP, "Feature snapping passes", "Passes snapping onto sharp edges (0 or none: no feature snapping).", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "snapControls/implicitFeatureSnap", G_SNAP, "Find edges itself", "Find sharp edges by sampling the surface.", MO_BOOL, TF, "false", 0 },
    { SN, SHM, "snapControls/explicitFeatureSnap", G_SNAP, "Use the extracted edges", "Snap to the feature edges surfaceFeatureExtract found.", MO_BOOL, TF, "true", 0 },
    { SN, SHM, "snapControls/multiRegionFeatureSnap", G_SNAP, "Edges between boundaries", "Also snap to the edges where two boundaries meet.", MO_BOOL, TF, "false", 0 },
    { SN, SHM, "snapControls/detectNearSurfacesSnap", G_SNAP, "Opposite near surfaces", "Snap to the right one of two close, opposite surfaces.", MO_BOOL, TF, "true", 0 },
    { SN, SHM, "snapControls/nFaceSplitInterval", G_SNAP, "Face splitting interval", "Split faces across corners every this many passes (-1: never).", MO_INT, NULL, "-1", 0 },
    { SN, SHM, "snapControls/detectBaffles", G_SNAP, "Detect baffle edges", "Treat baffle edges as features.", MO_BOOL, TF, "true", 0 },
    { SN, SHM, "snapControls/baffleFeaturePoints", G_SNAP, "Baffle feature points", "Treat the corners of baffles as feature points.", MO_BOOL, TF, NULL, 0 },
    { SN, SHM, "snapControls/releasePoints", G_SNAP, "Release points", "Points on several regions lead; the face's other points follow.", MO_BOOL, TF, "false", 0 },
    { SN, SHM, "snapControls/stringFeatures", G_SNAP, "String features", "Walk along feature edges adding missing ones.", MO_BOOL, TF, "true", 0 },
    { SN, SHM, "snapControls/avoidDiagonal", G_SNAP, "Avoid diagonal attraction", "On diagonal attraction, pull the face's other points too.", MO_BOOL, TF, "false", 0 },
    { SN, SHM, "snapControls/concaveAngle", G_SNAP, "Concave angle", "Concave faces up to this angle are left whole when splitting.", MO_REAL, NULL, "45", 0 },
    { SN, SHM, "snapControls/minAreaRatio", G_SNAP, "Minimum split area ratio", "Don't split a face into parts smaller than this ratio.", MO_REAL, NULL, "0.3", 0 },
    { SN, SHM, "snapControls/strictRegionSnap", G_SNAP, "Strict region snapping", "Points go only to the surface they came from (intersecting surfaces).", MO_BOOL, TF, "false", 0 },
    { SN, SHM, "snapControls/solver", G_SNAP, "Buffer-layer motion solver", "With type castellatedBufferLayer: the mesh motion solver (displacementPointSmoothing ...).", MO_TEXT, NULL, NULL, 0 },

    /* ---------------------------------------------------------------- addLayersControls */
    { SN, SHM, "addLayersControls/relativeSizes", G_LAY, "Relative sizes", "Thicknesses below as fractions of the surface cell (true) or in metres (false).", MO_BOOL, TF, NULL, 0 },
    { SN, SHM, "addLayersControls/thicknessModel", G_LAY, "Thickness given by", "Which two of first, final, overall and expansion set the layers.", MO_WORD, "finalAndExpansion|firstAndExpansion|firstAndOverall|overallAndExpansion|firstAndRelativeFinal|finalAndOverall", NULL, 0 },
    { SN, SHM, "addLayersControls/expansionRatio", G_LAY, "Expansion ratio", "Each layer this much thicker than the one under it.", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/finalLayerThickness", G_LAY, "Outer layer thickness", "The layer next to the bulk cells.", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/firstLayerThickness", G_LAY, "First layer thickness", "The layer on the wall (sets y+). Always metres with firstAndRelativeFinal.", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/thickness", G_LAY, "Overall thickness", "All the layers together.", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/minThickness", G_LAY, "Minimum thickness", "Where the layers can't be this thick, none are added there.", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/nGrow", G_LAY, "Grow non-extruded area", "Also stop layers this many faces round points that couldn't extrude (helps near features).", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/featureAngle", G_LAY, "Feature angle", "No layers across bends sharper than this (degrees: 0 flat, 90 square corner).", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/mergePatchFacesAngle", G_LAY, "Merge faces angle", "Merge patch faces flatter than this.", MO_REAL, NULL, "= featureAngle", 0 },
    { SN, SHM, "addLayersControls/maxFaceThicknessRatio", G_LAY, "Max face thickness ratio", "Stop the layers on warped faces thicker than this ratio.", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/nSmoothSurfaceNormals", G_LAY, "Normal smoothing passes", "Passes smoothing the surface normals the layers grow along.", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/nSmoothThickness", G_LAY, "Thickness smoothing passes", "Smooth the layer thickness over the surface.", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/meshShrinker", G_LAY, "Mesh shrinker", "How the mesh is pushed back to make room: medial axis (default) or a motion solver.", MO_WORD, "displacementMedialAxis|displacementMotionSolver", "displacementMedialAxis", 0 },
    { SN, SHM, "addLayersControls/solver", G_LAY, "Motion solver", "With displacementMotionSolver: displacementLaplacian ... (its Coeffs go under Any other keyword).", MO_TEXT, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/minMedialAxisAngle", G_LAY, "Medial axis angle", "Angle picking the medial axis points.", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/maxThicknessToMedialRatio", G_LAY, "Max thickness to medial distance", "Thin the layers where they fill this much of the gap to the medial axis.", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/nSmoothNormals", G_LAY, "Interior direction smoothing", "Passes smoothing the direction the mesh moves inside.", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/nMedialAxisIter", G_LAY, "Medial axis steps", "Limit the steps walking away from the surface.", MO_INT, NULL, "unlimited", 0 },
    { SN, SHM, "addLayersControls/nSmoothDisplacement", G_LAY, "Displacement smoothing", "Smooth the displacement after the medial axis.", MO_INT, NULL, "0", 0 },
    { SN, SHM, "addLayersControls/detectExtrusionIsland", G_LAY, "Detect extrusion islands", "Don't extrude a point whose neighbours aren't extruded.", MO_BOOL, TF, "false", 0 },
    { SN, SHM, "addLayersControls/layerTerminationAngle", G_LAY, "Layer termination angle", "Don't extrude round sharp edges where a face would become a wedge (-180: always try).", MO_REAL, NULL, "= featureAngle / 2", 0 },
    { SN, SHM, "addLayersControls/disableWallEdges", G_LAY, "Don't shrink wall edges", "Off lets one- or two-cell channels still get layers.", MO_BOOL, TF, "false", 0 },
    { SN, SHM, "addLayersControls/slipFeatureAngle", G_LAY, "Slip feature angle", "At sides without layers, let the mesh slip where the extrusion leans more than this.", MO_REAL, NULL, "= featureAngle / 2", 0 },
    { SN, SHM, "addLayersControls/nRelaxIter", G_LAY, "Relaxation passes", "Tries at scaling back the movement where quality fails.", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/nBufferCellsNoExtrude", G_LAY, "Termination buffer", "Step the layer count down over this many cells where layers end (negative: end at once).", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/nLayerIter", G_LAY, "Layer passes", "The most layer addition passes; it stops there, possibly with bad cells.", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/nRelaxedIter", G_LAY, "Passes before relaxed checks", "After this many passes the relaxed quality limits apply.", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/nOuterIter", G_LAY, "Outer passes", "Add the layers in this many goes (one layer per go helps thin gaps).", MO_INT, NULL, "1", 0 },
    { SN, SHM, "addLayersControls/concaveAngle", G_LAY, "Concave angle", "Concave corners beyond this angle are handled as features.", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "addLayersControls/additionalReporting", G_LAY, "Report bad faces", "Print the centres of the few faces that fail after layering.", MO_BOOL, TF, "false", 0 },
    /* per boundary */
    { SN, SHM, "addLayersControls/layers/%s/nSurfaceLayers", G_PLAY, "Layers", "Prism layers on this boundary (0: frozen, no layers and no shrinking).", MO_INT, NULL, NULL, MO_PATCH },
    { SN, SHM, "addLayersControls/layers/%s/relativeSizes", G_PLAY, "Relative sizes", "This boundary's thicknesses relative (true) or in metres.", MO_BOOL, TF, NULL, MO_PATCH },
    { SN, SHM, "addLayersControls/layers/%s/thicknessModel", G_PLAY, "Thickness given by", "Which two values set this boundary's layers.", MO_WORD, "finalAndExpansion|firstAndExpansion|firstAndOverall|overallAndExpansion|firstAndRelativeFinal|finalAndOverall", NULL, MO_PATCH },
    { SN, SHM, "addLayersControls/layers/%s/expansionRatio", G_PLAY, "Expansion ratio", "Each layer this much thicker than the one under it.", MO_REAL, NULL, NULL, MO_PATCH },
    { SN, SHM, "addLayersControls/layers/%s/finalLayerThickness", G_PLAY, "Outer layer thickness", "The layer next to the bulk cells.", MO_REAL, NULL, NULL, MO_PATCH },
    { SN, SHM, "addLayersControls/layers/%s/firstLayerThickness", G_PLAY, "First layer thickness", "The layer on the wall.", MO_REAL, NULL, NULL, MO_PATCH },
    { SN, SHM, "addLayersControls/layers/%s/thickness", G_PLAY, "Overall thickness", "All this boundary's layers together.", MO_REAL, NULL, NULL, MO_PATCH },
    { SN, SHM, "addLayersControls/layers/%s/minThickness", G_PLAY, "Minimum thickness", "Thinner than this: no layers there.", MO_REAL, NULL, NULL, MO_PATCH },

    /* ---------------------------------------------------------------- meshQualityControls in snappyHexMeshDict */
    { SN, SHM, "meshQualityControls/nSmoothScale", G_MQC, "Error smoothing passes", "Passes spreading the scale-back where the quality fails.", MO_INT, NULL, NULL, 0 },
    { SN, SHM, "meshQualityControls/errorReduction", G_MQC, "Error reduction", "How much of the movement is taken back at failing points.", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "meshQualityControls/relaxed/maxNonOrtho", G_MQC, "Relaxed max non-orthogonality", "Used after the passes before relaxed checks (layers).", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "meshQualityControls/relaxed/maxBoundarySkewness", G_MQC, "Relaxed max boundary skewness", "Relaxed limit (layers).", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "meshQualityControls/relaxed/maxInternalSkewness", G_MQC, "Relaxed max internal skewness", "Relaxed limit (layers).", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "meshQualityControls/relaxed/minDeterminant", G_MQC, "Relaxed min determinant", "Relaxed limit (layers).", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "meshQualityControls/relaxed/minTetQuality", G_MQC, "Relaxed min tet quality", "Relaxed limit (layers).", MO_REAL, NULL, NULL, 0 },
    { SN, SHM, "meshQualityControls/relaxed/minFaceWeight", G_MQC, "Relaxed min face weight", "Relaxed limit (layers).", MO_REAL, NULL, NULL, 0 },

    /* ---------------------------------------------------------------- meshQualityDict */
    { MW_QUALITY, MQD, "maxNonOrtho", G_MQD, "Max non-orthogonality", "Angle between a face's normal and the line joining the cell centres. Above 70 the gradients get inaccurate (180: off).", MO_REAL, NULL, "65", 0 },
    { MW_QUALITY, MQD, "maxBoundarySkewness", G_MQD, "Max boundary skewness", "How far a boundary face's centre is off the line between cell centres (<0: off).", MO_REAL, NULL, "20", 0 },
    { MW_QUALITY, MQD, "maxInternalSkewness", G_MQD, "Max internal skewness", "The same for faces inside (<0: off).", MO_REAL, NULL, "4", 0 },
    { MW_QUALITY, MQD, "maxConcave", G_MQD, "Max concave angle", "Faces may be concave up to this angle (180: off).", MO_REAL, NULL, "80", 0 },
    { MW_QUALITY, MQD, "minVol", G_MQD, "Min pyramid volume", "Smallest cell pyramid, m³ (-1e30: off).", MO_REAL, NULL, "1e-13", 0 },
    { MW_QUALITY, MQD, "minTetQuality", G_MQD, "Min tet quality", "Of the tets a cell decomposes into: 1 regular, 0 flat, <0 inside out (-1e30: off).", MO_REAL, NULL, "1e-15", 0 },
    { MW_QUALITY, MQD, "minArea", G_MQD, "Min face area", "m² (<0: off).", MO_REAL, NULL, "-1", 0 },
    { MW_QUALITY, MQD, "minTwist", G_MQD, "Min face twist", "How flat a face is, 1 flat (<-1: off).", MO_REAL, NULL, "0.02", 0 },
    { MW_QUALITY, MQD, "minDeterminant", G_MQD, "Min cell determinant", "1 for a cube, 0 or less folded.", MO_REAL, NULL, "0.001", 0 },
    { MW_QUALITY, MQD, "minFaceWeight", G_MQD, "Min face weight", "Where the face sits between the cell centres: 0.5 halfway.", MO_REAL, NULL, "0.05", 0 },
    { MW_QUALITY, MQD, "minVolRatio", G_MQD, "Min volume ratio", "Smallest ratio of neighbouring cells' volumes (0 to 1).", MO_REAL, NULL, "0.01", 0 },
    { MW_QUALITY, MQD, "minTriangleTwist", G_MQD, "Min triangle twist", ">0 for Fluent (-1: off).", MO_REAL, NULL, "-1", 0 },
    { MW_QUALITY, MQD, "minVolCollapseRatio", G_MQD, "Min collapse volume ratio", ">0: keep cells all on the surface if they keep this share of their volume; <0 delete.", MO_REAL, NULL, NULL, 0 },
    { MW_QUALITY, MQD, "minEdgeLength", G_MQD, "Min edge length", "m (<0: off).", MO_REAL, NULL, "-1", 0 },

    /* ---------------------------------------------------------------- blockMeshDict (the background) */
    { SN, BMD, "scale", G_BMD, "Scale", "All vertices multiplied by this.", MO_REAL, NULL, "1", 0 },
    { SN, BMD, "blocks", G_BMD, "Blocks", "The background block: hex (vertices) (cells in x y z) simpleGrading (gx gy gz). Its cells set the base size.", MO_TEXT, NULL, NULL, 0 },
    { SN, BMD, "mergeType", G_BMD, "Merge type", "Join the blocks by their topology or by matching points.", MO_WORD, "topology|points", "topology", 0 },
    { SN, BMD, "checkFaceCorrespondence", G_BMD, "Check face correspondence", "Check that the boundary faces belong to the blocks.", MO_BOOL, TF, "true", 0 },
    { SN, BMD, "verbose", G_BMD, "Verbose", "Say more while meshing.", MO_BOOL, TF, "true", 0 },

    /* ---------------------------------------------------------------- surfaceFeatureExtractDict */
    { SN, SFE, "geometry.stl/extractionMethod", G_SFE, "Extraction method", "Find edges from the surface, read them from a file, or none.", MO_WORD, "extractFromSurface|extractFromFile|none", NULL, 0 },
    { SN, SFE, "geometry.stl/includedAngle", G_SFE, "Included angle", "Edges whose faces meet at less than this are features (180: every edge; 0: none). 180 - feature angle.", MO_REAL, NULL, NULL, 0 },
    { SN, SFE, "geometry.stl/geometricTestOnly", G_SFE, "Geometric test only", "Don't count the edges between boundaries as features.", MO_BOOL, YN, "no", 0 },
    { SN, SFE, "geometry.stl/intersectionMethod", G_SFE, "Intersections", "Add the lines where the surface crosses itself or its regions cross.", MO_WORD, "none|self|region", "none", 0 },
    { SN, SFE, "geometry.stl/tolerance", G_SFE, "Intersection tolerance", "For surface intersections.", MO_REAL, NULL, "1e-3", 0 },
    { SN, SFE, "geometry.stl/featureEdgeFile", G_SFE, "Feature edge file", "With extractFromFile: \"<constant>/triSurface/edges.nas\".", MO_TEXT, NULL, NULL, 0 },
    { SN, SFE, "geometry.stl/subsetFeatures/nonManifoldEdges", G_SFE, "Keep non-manifold edges", "Edges with more than two faces.", MO_BOOL, YN, "yes", 0 },
    { SN, SFE, "geometry.stl/subsetFeatures/strictNonManifoldEdges", G_SFE, "Strict non-manifold edges", "The same, topology only.", MO_BOOL, YN, "no", 0 },
    { SN, SFE, "geometry.stl/subsetFeatures/openEdges", G_SFE, "Keep open edges", "Edges with one face (a hole's rim).", MO_BOOL, YN, "yes", 0 },
    { SN, SFE, "geometry.stl/subsetFeatures/plane", G_SFE, "Only edges crossing a plane", "(normal)(point).", MO_TEXT, NULL, NULL, 0 },
    { SN, SFE, "geometry.stl/subsetFeatures/insideBox", G_SFE, "Only edges inside a box", "(min)(max).", MO_TEXT, NULL, NULL, 0 },
    { SN, SFE, "geometry.stl/subsetFeatures/outsideBox", G_SFE, "Only edges outside a box", "(min)(max).", MO_TEXT, NULL, NULL, 0 },
    { SN, SFE, "geometry.stl/trimFeatures/minElem", G_SFE, "Drop features of fewer edges", "Remove features with fewer edges than this.", MO_INT, NULL, "0", 0 },
    { SN, SFE, "geometry.stl/trimFeatures/minLen", G_SFE, "Drop features shorter than", "m, the feature's whole length.", MO_REAL, NULL, "0", 0 },
    { SN, SFE, "geometry.stl/closeness", G_SFE, "Write closeness", "Write how close surface parts are to each other.", MO_BOOL, YN, "no", 0 },
    { SN, SFE, "geometry.stl/curvature", G_SFE, "Write curvature", "Write the surface curvature.", MO_BOOL, YN, "no", 0 },
    { SN, SFE, "geometry.stl/featureProximity", G_SFE, "Write feature proximity", "Write how close features are to each other.", MO_BOOL, YN, "no", 0 },
    { SN, SFE, "geometry.stl/maxFeatureProximity", G_SFE, "Feature proximity range", "m.", MO_REAL, NULL, "1", 0 },
    { SN, SFE, "geometry.stl/writeObj", G_SFE, "Write OBJ", "Write the features as .obj to look at.", MO_BOOL, YN, "no", 0 },
    { SN, SFE, "geometry.stl/writeVTK", G_SFE, "Write VTK", "Write the closeness / curvature / proximity fields as VTK.", MO_BOOL, YN, "no", 0 },
    { MW_FOAMY2D, SFE, "part2d.stl/includedAngle", G_SFE, "Included angle", "Edges whose faces meet at less than this are features (180 - feature angle).", MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, SFE, "part2d.stl/writeObj", G_SFE, "Write OBJ", "Write the features as .obj.", MO_BOOL, YN, "no", 0 },

    /* ---------------------------------------------------------------- extrudeMeshDict (snappy 2-D) */
    { MW_SNAPPY2D, EMD, "nLayers", G_EMD, "Cells through the depth", "2-D needs 1.", MO_INT, NULL, NULL, 0 },
    { MW_SNAPPY2D, EMD, "expansionRatio", G_EMD, "Expansion ratio", "Through the depth.", MO_REAL, NULL, NULL, 0 },
    { MW_SNAPPY2D, EMD, "linearNormalCoeffs/thickness", G_EMD, "Depth", "m: the part's depth.", MO_REAL, NULL, NULL, 0 },
    { MW_SNAPPY2D, EMD, "flipNormals", G_EMD, "Flip normals", "Extrude the other way.", MO_BOOL, TF, NULL, 0 },
    { MW_SNAPPY2D, EMD, "mergeFaces", G_EMD, "Merge front and back", "Only for a 360° wedge.", MO_BOOL, TF, NULL, 0 },
    { MW_SNAPPY2D, EMD, "mergeTol", G_EMD, "Merge tolerance", "Merge small edges, a fraction of the box.", MO_REAL, NULL, NULL, 0 },

    /* ---------------------------------------------------------------- cfMesh meshDict */
    { MW_CF, CFD, "maxCellSize", G_CF, "Max cell size", "m: the largest cells (the base size, far field in a tunnel).", MO_REAL, NULL, NULL, 0 },
    { MW_CF, CFD, "minCellSize", G_CF, "Min cell size", "m: automatic refinement stops here.", MO_REAL, NULL, NULL, 0 },
    { MW_CF, CFD, "boundaryCellSize", G_CF, "Boundary cell size", "m: cells on the boundary.", MO_REAL, NULL, NULL, 0 },
    { MW_CF, CFD, "boundaryCellSizeRefinementThickness", G_CF, "Boundary refinement thickness", "m: how far in from the boundary its size holds.", MO_REAL, NULL, NULL, 0 },
    { MW_CF, CFD, "keepCellsIntersectingBoundary", G_CF, "Keep cells cut by the boundary", "1 keeps them in the template mesh.", MO_BOOL, B10, "0", 0 },
    { MW_CF, CFD, "checkForGluedMesh", G_CF, "Check for glued mesh", "Remove cells that join regions through a single point or edge.", MO_BOOL, B10, "0", 0 },
    { MW_CF, CFD, "enforceGeometryConstraints", G_CF, "Enforce geometry", "Stop if the mesh can't follow the geometry (no cells removed silently).", MO_BOOL, B10, "0", 0 },
    { MW_CF, CFD, "meshQualitySettings/maxNonOrthogonality", G_CF, "Max non-orthogonality", "The optimiser works on cells above this.", MO_REAL, NULL, NULL, 0 },
    { MW_CF, CFD, "meshQualitySettings/minTetQuality", G_CF, "Min tet quality", "The optimiser works on cells below this.", MO_REAL, NULL, NULL, 0 },
    { MW_CF, CFD, "workflowControls/stopAfter", G_CF, "Stop after", "Stop after this stage (to look at the mesh so far).", MO_WORD, "templateGeneration|surfaceTopology|surfaceProjection|patchAssignment|edgeExtraction|boundaryLayerGeneration|meshOptimisation|boundaryLayerRefinement", NULL, 0 },
    { MW_CF, CFD, "workflowControls/restartFromLatestStep", G_CF, "Restart from the last stage", "Carry on from where it stopped.", MO_BOOL, B10, "0", 0 },
    { MW_CF, CFD, "anisotropicSources", G_CF, "Anisotropic sources", "Stretched cells: see cfMesh's guide; add its entries under Any other keyword.", MO_TEXT, NULL, NULL, 0 },
    /* per boundary */
    { MW_CF, CFD, "localRefinement/%s/cellSize", G_CFP, "Cell size", "m on this boundary.", MO_REAL, NULL, NULL, MO_PATCH },
    { MW_CF, CFD, "localRefinement/%s/additionalRefinementLevels", G_CFP, "Extra levels", "Halvings beyond the max cell size (instead of a size).", MO_INT, NULL, NULL, MO_PATCH },
    { MW_CF, CFD, "localRefinement/%s/refinementThickness", G_CFP, "Refinement thickness", "m: how far from it the size holds.", MO_REAL, NULL, NULL, MO_PATCH },
    { MW_CF, CFD, "boundaryLayers/patchBoundaryLayers/%s/nLayers", G_CFP, "Layers", "Prism layers on this boundary.", MO_INT, NULL, NULL, MO_PATCH },
    { MW_CF, CFD, "boundaryLayers/patchBoundaryLayers/%s/thicknessRatio", G_CFP, "Thickness ratio", "Each layer this much thicker than the one under it.", MO_REAL, NULL, NULL, MO_PATCH },
    { MW_CF, CFD, "boundaryLayers/patchBoundaryLayers/%s/maxFirstLayerThickness", G_CFP, "Max first layer", "m.", MO_REAL, NULL, NULL, MO_PATCH },
    { MW_CF, CFD, "boundaryLayers/patchBoundaryLayers/%s/allowDiscontinuity", G_CFP, "Allow discontinuity", "1: the layer count may jump at this boundary's edges.", MO_BOOL, B10, "0", MO_PATCH },
    /* per region */
    { MW_CF, CFD, "objectRefinements/%s/cellSize", G_CFR, "Cell size", "m inside it.", MO_REAL, NULL, NULL, MO_REGION },
    { MW_CF, CFD, "objectRefinements/%s/additionalRefinementLevels", G_CFR, "Extra levels", "Halvings beyond the max cell size (instead of a size).", MO_INT, NULL, NULL, MO_REGION },
    { MW_CF, CFD, "objectRefinements/%s/refinementThickness", G_CFR, "Refinement thickness", "m beyond its edge.", MO_REAL, NULL, NULL, MO_REGION },
    /* layers */
    { MW_CF, CFD, "boundaryLayers/nLayers", G_CFL, "Layers everywhere", "Prism layers on every boundary (the per-boundary ones override).", MO_INT, NULL, NULL, 0 },
    { MW_CF, CFD, "boundaryLayers/thicknessRatio", G_CFL, "Thickness ratio", "Each layer this much thicker.", MO_REAL, NULL, NULL, 0 },
    { MW_CF, CFD, "boundaryLayers/maxFirstLayerThickness", G_CFL, "Max first layer", "m.", MO_REAL, NULL, NULL, 0 },
    { MW_CF, CFD, "boundaryLayers/optimiseLayer", G_CFL, "Optimise the layers", "Smooth the layers' normals and thickness.", MO_BOOL, B10, "1", 0 },
    { MW_CF, CFD, "boundaryLayers/untangleLayers", G_CFL, "Untangle layers", "Shrink the layers where cells would fold.", MO_BOOL, B10, "1", 0 },
    { MW_CF, CFD, "boundaryLayers/optimisationParameters/nSmoothNormals", G_CFL, "Normal smoothing passes", NULL, MO_INT, NULL, "5", 0 },
    { MW_CF, CFD, "boundaryLayers/optimisationParameters/maxNumIterations", G_CFL, "Optimisation passes", NULL, MO_INT, NULL, "5", 0 },
    { MW_CF, CFD, "boundaryLayers/optimisationParameters/featureSizeFactor", G_CFL, "Feature size factor", "Layers at most this share of the local feature size.", MO_REAL, NULL, "0.4", 0 },
    { MW_CF, CFD, "boundaryLayers/optimisationParameters/reCalculateNormals", G_CFL, "Recalculate normals", NULL, MO_BOOL, B10, "1", 0 },
    { MW_CF, CFD, "boundaryLayers/optimisationParameters/relThicknessTol", G_CFL, "Thickness tolerance", "Relative change of thickness between neighbours.", MO_REAL, NULL, "0.1", 0 },

    /* ---------------------------------------------------------------- foamyHexMeshDict */
    { MW_FOAMY, FHD, "surfaceConformation/locationInMesh", G_FSC, "Point in the fluid", NULL, MO_VEC, NULL, NULL, 0 },
    { MW_FOAMY, FHD, "surfaceConformation/pointPairDistanceCoeff", G_FSC, "Point pair distance", "Of the local cell size.", MO_REAL, NULL, "0.1", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/mixedFeaturePointPPDistanceCoeff", G_FSC, "Mixed feature point distance", NULL, MO_REAL, NULL, "5.0", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/featurePointExclusionDistanceCoeff", G_FSC, "Feature point exclusion", NULL, MO_REAL, NULL, "0.65", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/featureEdgeExclusionDistanceCoeff", G_FSC, "Feature edge exclusion", NULL, MO_REAL, NULL, "0.65", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/surfaceSearchDistanceCoeff", G_FSC, "Surface search distance", NULL, MO_REAL, NULL, "5", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/maxSurfaceProtrusionCoeff", G_FSC, "Max surface protrusion", NULL, MO_REAL, NULL, "0.1", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/maxQuadAngle", G_FSC, "Max quad angle", NULL, MO_REAL, NULL, "125", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/surfaceConformationRebuildFrequency", G_FSC, "Rebuild every", "Iterations between rebuilding the conformation.", MO_INT, NULL, "10", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/featurePointControls/specialiseFeaturePoints", G_FSC, "Specialise feature points", NULL, MO_BOOL, OO, "on", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/featurePointControls/edgeAiming", G_FSC, "Edge aiming", NULL, MO_BOOL, OO, "on", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/featurePointControls/guardFeaturePoints", G_FSC, "Guard feature points", NULL, MO_BOOL, OO, "off", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/featurePointControls/snapFeaturePoints", G_FSC, "Snap feature points", NULL, MO_BOOL, OO, "off", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/featurePointControls/circulateEdges", G_FSC, "Circulate edges", NULL, MO_BOOL, OO, "off", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/conformationControls/edgeSearchDistCoeff", G_FSC, "Edge search distance", NULL, MO_REAL, NULL, "5", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/conformationControls/surfacePtReplaceDistCoeff", G_FSC, "Surface point replace distance", NULL, MO_REAL, NULL, "0.5", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/conformationControls/surfacePtExclusionDistanceCoeff", G_FSC, "Surface point exclusion", NULL, MO_REAL, NULL, "0.5", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/conformationControls/maxIterations", G_FSC, "Conformation passes", NULL, MO_INT, NULL, "15", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/conformationControls/iterationToInitialHitRatioLimit", G_FSC, "Stop at hit ratio", NULL, MO_REAL, NULL, "0.001", 0 },
    { MW_FOAMY, FHD, "surfaceConformation/geometryToConformTo/@/includedAngle", G_FSC, "Included angle", "Feature edges of the part: 180 - feature angle.", MO_REAL, NULL, NULL, MO_PART },
    { MW_FOAMY, FHD, "surfaceConformation/geometryToConformTo/@/featureMethod", G_FSC, "Feature method", NULL, MO_WORD, "extractFeatures|extendedFeatureEdgeMesh|none", NULL, MO_PART },
    { MW_FOAMY, FHD, "initialPoints/initialPointsMethod", G_FIP, "Initial points", "How the volume is first filled with points.", MO_WORD, "autoDensity|uniformGrid|bodyCentredCubic|pointFile", "autoDensity", 0 },
    { MW_FOAMY, FHD, "initialPoints/minimumSurfaceDistanceCoeff", G_FIP, "Min distance from the surface", NULL, MO_REAL, NULL, "0.55", 0 },
    { MW_FOAMY, FHD, "initialPoints/fixInitialPoints", G_FIP, "Fix the initial points", NULL, MO_BOOL, TF, "false", 0 },
    { MW_FOAMY, FHD, "initialPoints/autoDensityCoeffs/minLevels", G_FIP, "Min levels", NULL, MO_INT, NULL, "4", 0 },
    { MW_FOAMY, FHD, "initialPoints/autoDensityCoeffs/maxSizeRatio", G_FIP, "Max size ratio", NULL, MO_REAL, NULL, "5.0", 0 },
    { MW_FOAMY, FHD, "initialPoints/autoDensityCoeffs/sampleResolution", G_FIP, "Sample resolution", NULL, MO_INT, NULL, "5", 0 },
    { MW_FOAMY, FHD, "initialPoints/autoDensityCoeffs/surfaceSampleResolution", G_FIP, "Surface sample resolution", NULL, MO_INT, NULL, "5", 0 },
    { MW_FOAMY, FHD, "initialPoints/uniformGridCoeffs/initialCellSize", G_FIP, "Uniform grid cell size", "m.", MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY, FHD, "initialPoints/uniformGridCoeffs/randomiseInitialGrid", G_FIP, "Randomise the grid", NULL, MO_BOOL, YN, NULL, 0 },
    { MW_FOAMY, FHD, "initialPoints/uniformGridCoeffs/randomPerturbationCoeff", G_FIP, "Random perturbation", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY, FHD, "initialPoints/pointFileCoeffs/pointFile", G_FIP, "Point file", "\"<constant>/internalDelaunayVertices\".", MO_TEXT, NULL, NULL, 0 },
    { MW_FOAMY, FHD, "motionControl/defaultCellSize", G_FMC, "Default cell size", "m: the base size.", MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY, FHD, "motionControl/minimumCellSizeCoeff", G_FMC, "Min cell size coefficient", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY, FHD, "motionControl/maxSmoothingIterations", G_FMC, "Smoothing passes", NULL, MO_INT, NULL, "100", 0 },
    { MW_FOAMY, FHD, "motionControl/maxRefinementIterations", G_FMC, "Refinement passes", NULL, MO_INT, NULL, "0", 0 },
    { MW_FOAMY, FHD, "motionControl/cellAspectRatioControl/aspectRatio", G_FMC, "Cell aspect ratio", NULL, MO_REAL, NULL, "1.0", 0 },
    { MW_FOAMY, FHD, "motionControl/cellAspectRatioControl/aspectRatioDirection", G_FMC, "Aspect ratio direction", NULL, MO_VEC, NULL, "(1 0 0)", 0 },
    { MW_FOAMY, FHD, "motionControl/relaxationModel", G_FMC, "Relaxation model", NULL, MO_WORD, "adaptiveLinear|rampHoldFall", "adaptiveLinear", 0 },
    { MW_FOAMY, FHD, "motionControl/adaptiveLinearCoeffs/relaxationStart", G_FMC, "Relaxation start", NULL, MO_REAL, NULL, "1.0", 0 },
    { MW_FOAMY, FHD, "motionControl/adaptiveLinearCoeffs/relaxationEnd", G_FMC, "Relaxation end", NULL, MO_REAL, NULL, "0.0", 0 },
    { MW_FOAMY, FHD, "motionControl/objOutput", G_FMC, "Write OBJ", NULL, MO_BOOL, YN, "no", 0 },
    { MW_FOAMY, FHD, "motionControl/timeChecks", G_FMC, "Time checks", NULL, MO_BOOL, YN, "no", 0 },
    { MW_FOAMY, FHD, "motionControl/printVertexInfo", G_FMC, "Print vertex info", NULL, MO_BOOL, OO, "off", 0 },
    { MW_FOAMY, FHD, "motionControl/maxLoadUnbalance", G_FMC, "Allowed imbalance", NULL, MO_REAL, NULL, "0.2", 0 },
    { MW_FOAMY, FHD, "motionControl/alignmentAcceptanceAngle", G_FMC, "Alignment acceptance angle", NULL, MO_REAL, NULL, "48", 0 },
    { MW_FOAMY, FHD, "motionControl/pointInsertionCriteria/cellCentreDistCoeff", G_FMC, "Insertion: cell centre distance", NULL, MO_REAL, NULL, "1.75", 0 },
    { MW_FOAMY, FHD, "motionControl/pointInsertionCriteria/faceAreaRatioCoeff", G_FMC, "Insertion: face area ratio", NULL, MO_REAL, NULL, "0.0025", 0 },
    { MW_FOAMY, FHD, "motionControl/pointInsertionCriteria/acceptanceAngle", G_FMC, "Insertion: acceptance angle", NULL, MO_REAL, NULL, "21.5", 0 },
    { MW_FOAMY, FHD, "motionControl/pointRemovalCriteria/cellCentreDistCoeff", G_FMC, "Removal: cell centre distance", NULL, MO_REAL, NULL, "0.65", 0 },
    { MW_FOAMY, FHD, "motionControl/faceAreaWeightModel", G_FMC, "Face area weight model", NULL, MO_WORD, "piecewiseLinearRamp", "piecewiseLinearRamp", 0 },
    { MW_FOAMY, FHD, "motionControl/piecewiseLinearRampCoeffs/lowerAreaFraction", G_FMC, "Ramp: lower area fraction", NULL, MO_REAL, NULL, "0.5", 0 },
    { MW_FOAMY, FHD, "motionControl/piecewiseLinearRampCoeffs/upperAreaFraction", G_FMC, "Ramp: upper area fraction", NULL, MO_REAL, NULL, "1.0", 0 },
    { MW_FOAMY, FHD, "motionControl/shapeControlFunctions/@/priority", G_FMC, "Part: priority", "Higher wins where size controls overlap.", MO_INT, NULL, NULL, MO_PART },
    { MW_FOAMY, FHD, "motionControl/shapeControlFunctions/@/mode", G_FMC, "Part: side", NULL, MO_WORD, "inside|outside|bothSides", NULL, MO_PART },
    { MW_FOAMY, FHD, "motionControl/shapeControlFunctions/@/forceInitialPointInsertion", G_FMC, "Part: force initial points", NULL, MO_BOOL, OO, "off", MO_PART },
    { MW_FOAMY, FHD, "motionControl/shapeControlFunctions/@/surfaceCellSizeFunction", G_FMC, "Part: surface size function", NULL, MO_WORD, "uniformValue|nonUniformField", NULL, MO_PART },
    { MW_FOAMY, FHD, "motionControl/shapeControlFunctions/@/uniformValueCoeffs/surfaceCellSizeCoeff", G_FMC, "Part: surface cell size", "Of the default cell size.", MO_REAL, NULL, NULL, MO_PART },
    { MW_FOAMY, FHD, "motionControl/shapeControlFunctions/@/cellSizeFunction", G_FMC, "Part: size away from it", NULL, MO_WORD, "uniform|uniformDistance|linearDistance|surfaceOffsetLinearDistance|linearSpatial", NULL, MO_PART },
    { MW_FOAMY, FHD, "motionControl/shapeControlFunctions/@/surfaceOffsetLinearDistanceCoeffs/distanceCellSizeCoeff", G_FMC, "Part: far cell size", "Of the default cell size.", MO_REAL, NULL, NULL, MO_PART },
    { MW_FOAMY, FHD, "motionControl/shapeControlFunctions/@/surfaceOffsetLinearDistanceCoeffs/surfaceOffsetCoeff", G_FMC, "Part: surface offset", "Of the default cell size.", MO_REAL, NULL, NULL, MO_PART },
    { MW_FOAMY, FHD, "motionControl/shapeControlFunctions/@/surfaceOffsetLinearDistanceCoeffs/linearDistanceCoeff", G_FMC, "Part: growth distance", "Of the default cell size.", MO_REAL, NULL, NULL, MO_PART },
    { MW_FOAMY, FHD, "polyMeshFiltering/filterEdges", G_FPF, "Filter edges", NULL, MO_BOOL, OO, "on", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/filterFaces", G_FPF, "Filter faces", NULL, MO_BOOL, OO, "off", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/writeTetDualMesh", G_FPF, "Write the tet dual mesh", NULL, MO_BOOL, TF, "false", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/writeCellShapeControlMesh", G_FPF, "Write the shape control mesh", NULL, MO_BOOL, TF, "false", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/writeBackgroundMeshDecomposition", G_FPF, "Write the background decomposition", NULL, MO_BOOL, TF, "false", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/filterSizeCoeff", G_FPF, "Filter size", NULL, MO_REAL, NULL, "0.2", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/mergeClosenessCoeff", G_FPF, "Merge closeness", NULL, MO_REAL, NULL, "1e-9", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/continueFilteringOnBadInitialPolyMesh", G_FPF, "Filter a bad first mesh", NULL, MO_BOOL, TF, "true", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/filterErrorReductionCoeff", G_FPF, "Filter error reduction", NULL, MO_REAL, NULL, "0.5", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/filterCountSkipThreshold", G_FPF, "Filter skip threshold", NULL, MO_INT, NULL, "4", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/maxCollapseIterations", G_FPF, "Collapse passes", NULL, MO_INT, NULL, "25", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/maxConsecutiveEqualFaceSets", G_FPF, "Stop after equal face sets", NULL, MO_INT, NULL, "5", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/surfaceStepFaceAngle", G_FPF, "Surface step face angle", NULL, MO_REAL, NULL, "80", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/edgeCollapseGuardFraction", G_FPF, "Edge collapse guard", NULL, MO_REAL, NULL, "0.3", 0 },
    { MW_FOAMY, FHD, "polyMeshFiltering/maxCollapseFaceToPointSideLengthCoeff", G_FPF, "Face-to-point collapse length", NULL, MO_REAL, NULL, "0.35", 0 },
    { MW_FOAMY, FHD, "backgroundMeshDecomposition/minLevels", G_FPF, "Decomposition: min levels", NULL, MO_INT, NULL, "1", 0 },
    { MW_FOAMY, FHD, "backgroundMeshDecomposition/sampleResolution", G_FPF, "Decomposition: sample resolution", NULL, MO_INT, NULL, "4", 0 },
    { MW_FOAMY, FHD, "backgroundMeshDecomposition/spanScale", G_FPF, "Decomposition: span scale", NULL, MO_REAL, NULL, "20", 0 },
    { MW_FOAMY, FHD, "backgroundMeshDecomposition/maxCellWeightCoeff", G_FPF, "Decomposition: max cell weight", NULL, MO_REAL, NULL, "20", 0 },
    /* collapseDict */
    { MW_FOAMY, COL, "controlMeshQuality", G_COL, "Check quality after collapsing", "Redo with stricter filtering where bad faces appear.", MO_BOOL, OO, NULL, 0 },
    { MW_FOAMY, COL, "collapseEdgesCoeffs/minimumEdgeLength", G_COL, "Min edge length", "m: shorter edges are merged.", MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY, COL, "collapseEdgesCoeffs/maximumMergeAngle", G_COL, "Max merge angle", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY, COL, "collapseFacesCoeffs/initialFaceLengthFactor", G_COL, "Initial face length factor", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY, COL, "collapseFacesCoeffs/maxCollapseFaceToPointSideLengthCoeff", G_COL, "Face-to-point length", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY, COL, "collapseFacesCoeffs/allowEarlyCollapseToPoint", G_COL, "Early collapse to a point", NULL, MO_BOOL, OO, NULL, 0 },
    { MW_FOAMY, COL, "collapseFacesCoeffs/allowEarlyCollapseCoeff", G_COL, "Early collapse factor", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY, COL, "collapseFacesCoeffs/guardFraction", G_COL, "Guard fraction", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY, COL, "controlMeshQualityCoeffs/edgeReductionFactor", G_COL, "Edge reduction factor", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY, COL, "controlMeshQualityCoeffs/faceReductionFactor", G_COL, "Face reduction factor", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY, COL, "controlMeshQualityCoeffs/maximumSmoothingIterations", G_COL, "Smoothing passes", NULL, MO_INT, NULL, NULL, 0 },
    { MW_FOAMY, COL, "controlMeshQualityCoeffs/maximumIterations", G_COL, "Outer passes", NULL, MO_INT, NULL, NULL, 0 },
    { MW_FOAMY, COL, "controlMeshQualityCoeffs/maxPointErrorCount", G_COL, "Max point error count", NULL, MO_INT, NULL, NULL, 0 },

    /* ---------------------------------------------------------------- foamyQuadMeshDict + extrude2DMeshDict */
    { MW_FOAMY2D, FQD, "surfaceConformation/locationInMesh", G_FQ, "Point in the fluid", NULL, MO_VEC, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "surfaceConformation/pointPairDistanceCoeff", G_FQ, "Point pair distance", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "surfaceConformation/minEdgeLenCoeff", G_FQ, "Min edge length", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "surfaceConformation/maxNotchLenCoeff", G_FQ, "Max notch length", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "surfaceConformation/minNearPointDistCoeff", G_FQ, "Min near point distance", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "surfaceConformation/maxQuadAngle", G_FQ, "Max quad angle", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "surfaceConformation/insertSurfaceNearestPointPairs", G_FQ, "Near-boundary point pairs", NULL, MO_BOOL, YN, NULL, 0 },
    { MW_FOAMY2D, FQD, "surfaceConformation/mirrorPoints", G_FQ, "Mirror points", "Mirror near-boundary points instead of point pairs.", MO_BOOL, YN, NULL, 0 },
    { MW_FOAMY2D, FQD, "surfaceConformation/insertSurfaceNearPointPairs", G_FQ, "Point pairs very near the surface", NULL, MO_BOOL, YN, NULL, 0 },
    { MW_FOAMY2D, FQD, "surfaceConformation/maxBoundaryConformingIter", G_FQ, "Conforming passes", NULL, MO_INT, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "surfaceConformation/randomiseInitialGrid", G_FQ, "Randomise the grid", NULL, MO_BOOL, YN, NULL, 0 },
    { MW_FOAMY2D, FQD, "surfaceConformation/randomPerturbation", G_FQ, "Random perturbation", "1 = a cell size.", MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/minCellSize", G_FQ, "Cell size", "m.", MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/defaultPriority", G_FQ, "Default priority", NULL, MO_INT, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/nearWallAlignedDist", G_FQ, "Wall-aligned cells", "Cell layers aligned with the wall.", MO_INT, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/relaxationModel", G_FQ, "Relaxation model", NULL, MO_WORD, "adaptiveLinear|rampHoldFall", NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/adaptiveLinearCoeffs/relaxationStart", G_FQ, "Relaxation start", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/adaptiveLinearCoeffs/relaxationEnd", G_FQ, "Relaxation end", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/objOutput", G_FQ, "Write OBJ", NULL, MO_BOOL, YN, NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/meshedSurfaceOutput", G_FQ, "Write the meshed surface", NULL, MO_BOOL, YN, NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/shapeControlFunctions/part/mode", G_FQ, "Part: side", NULL, MO_WORD, "inside|outside|bothSides", NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/shapeControlFunctions/part/priority", G_FQ, "Part: priority", NULL, MO_INT, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/shapeControlFunctions/part/cellSizeFunction", G_FQ, "Part: size away from it", NULL, MO_WORD, "uniform|linearDistance|surfaceOffsetLinearDistance", NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/shapeControlFunctions/part/linearDistanceCoeffs/distanceCellSizeCoeff", G_FQ, "Part: far cell size", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/shapeControlFunctions/part/linearDistanceCoeffs/distanceCoeff", G_FQ, "Part: growth distance", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "motionControl/shapeControlFunctions/part/uniformValueCoeffs/surfaceCellSizeCoeff", G_FQ, "Part: surface cell size", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "shortEdgeFilter/shortEdgeFilterFactor", G_FQ, "Short edge filter", "Edges below this share of the face's mean edge are deleted.", MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, FQD, "shortEdgeFilter/edgeAttachedToBoundaryFactor", G_FQ, "Boundary edge weight", NULL, MO_REAL, NULL, "2.0", 0 },
    { MW_FOAMY2D, E2D, "nLayers", G_E2D, "Cells through the depth", "2-D needs 1.", MO_INT, NULL, NULL, 0 },
    { MW_FOAMY2D, E2D, "expansionRatio", G_E2D, "Expansion ratio", NULL, MO_REAL, NULL, NULL, 0 },
    { MW_FOAMY2D, E2D, "linearDirectionCoeffs/thickness", G_E2D, "Depth", "m.", MO_REAL, NULL, NULL, 0 },

    /* ---------------------------------------------------------------- command lines */
    { MW_POLY, "cmd:polyDualMesh", "featureAngle", G_CMD, "polyDualMesh: feature angle", "Edges sharper than this stay edges of the polyhedra.", MO_REAL, NULL, NULL, 0 },
    { MW_POLY, "cmd:polyDualMesh", "-concaveMultiCells", G_CMD, "polyDualMesh: split cells at concave edges", NULL, MO_BOOL, TF, "true", 0 },
    { MW_POLY, "cmd:polyDualMesh", "-splitAllFaces", G_CMD, "polyDualMesh: split every face", "Keep no face whole across a feature.", MO_BOOL, TF, "false", 0 },
    { MW_POLY, "cmd:polyDualMesh", "-doNotPreserveFaceZones", G_CMD, "polyDualMesh: don't keep face zones", NULL, MO_BOOL, TF, "false", 0 },
    { MW_CF, "cmd:surfaceFeatureEdges", "-angle", G_CMD, "surfaceFeatureEdges: angle", "cfMesh's feature edges: bends sharper than this.", MO_REAL, NULL, NULL, 0 },
    { MW_ANY, "cmd:checkMesh", "-allGeometry", G_CMD, "checkMesh: every geometry check", "Also face planarity, cell determinant and more.", MO_BOOL, TF, "false", 0 },
    { MW_ANY, "cmd:checkMesh", "-allTopology", G_CMD, "checkMesh: every topology check", NULL, MO_BOOL, TF, "false", 0 },
    { MW_ANY, "cmd:checkMesh", "-meshQuality", G_CMD, "checkMesh: the meshQualityDict limits", "Also check against the limits above.", MO_BOOL, TF, "false", 0 },
    { MW_ANY, "cmd:checkMesh", "extra", G_CMD, "checkMesh: other options", "e.g. -writeSets vtk (the failing cells as VTK files).", MO_TEXT, NULL, NULL, 0 },
    { SN, "cmd:snappyHexMesh", "extra", G_CMD, "snappyHexMesh: other options", "Added after -overwrite (e.g. -profiling).", MO_TEXT, NULL, NULL, 0 },
    { SN, "cmd:blockMesh", "extra", G_CMD, "blockMesh: other options", "e.g. -merge-points, -verbose.", MO_TEXT, NULL, NULL, 0 },
    { SN, "cmd:surfaceFeatureExtract", "extra", G_CMD, "surfaceFeatureExtract: other options", NULL, MO_TEXT, NULL, NULL, 0 },
    { MW_CF, "cmd:cfMesh", "extra", G_CMD, "cfMesh mesher: other options", NULL, MO_TEXT, NULL, NULL, 0 },
};
const int MESH_OPTS_N = (int)(sizeof MESH_OPTS / sizeof *MESH_OPTS);

/* ============================================================================ the user's values */
void mo_init(MeshOverrides *o) { memset(o, 0, sizeof *o); }
void mo_free(MeshOverrides *o) { free(o->s); memset(o, 0, sizeof *o); }

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

bool mo_parse(const char *text, MeshOverrides *o)
{
    o->n = 0;
    if (!text) return false;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[1024];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len); line[len] = 0;
        p += len + (nl ? 1 : 0);
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        char *eq = strstr(l, " = ");
        if (!eq) continue;
        *eq = 0;
        char *val = trim(eq + 3), *key = trim(l);
        char *sp = strchr(key, ' ');
        if (!sp) continue;
        *sp = 0;
        mo_set(o, key, trim(sp + 1), val);
    }
    return true;
}

bool mo_load(const char *casePath, MeshOverrides *o)
{
    o->n = 0;
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, MO_INI);
    char *t = pl_read_file(p, NULL);
    if (!t) return false;
    mo_parse(t, o);
    free(t);
    return true;
}

void mo_text(const MeshOverrides *o, Str *out)
{
    str_cat(out, "# Q-Continuum: OpenFOAM settings set by hand (Mesh > STL mesher > Mesh settings > Advanced). Generate applies\n"
                 "# them to the dictionaries it writes. <file> <path> = <value>; !remove leaves the entry out; cmd:<program> are\n"
                 "# its command-line options.\n");
    for (int i = 0; i < o->n; i++) str_catf(out, "%s %s = %s\n", o->s[i].file, o->s[i].path, o->s[i].value);
}

bool mo_save(const char *casePath, const MeshOverrides *o)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, MO_INI);
    if (o->n == 0) { if (pl_exists(p)) unlink(p); return true; }
    char d[PL_PATH]; pl_join(d, sizeof d, casePath, "mesher"); pl_mkdirs(d);
    Str b; str_init(&b);
    mo_text(o, &b);
    char *old = pl_read_file(p, NULL);
    bool ok = (old && !strcmp(old, b.s)) || pl_write_file(p, b.s, b.len);
    free(old); str_free(&b);
    return ok;
}

const char *mo_get(const MeshOverrides *o, const char *file, const char *path)
{
    for (int i = 0; i < o->n; i++) if (!strcmp(o->s[i].file, file) && !strcmp(o->s[i].path, path)) return o->s[i].value;
    return NULL;
}

void mo_set(MeshOverrides *o, const char *file, const char *path, const char *value)
{
    int at = -1;
    for (int i = 0; i < o->n; i++) if (!strcmp(o->s[i].file, file) && !strcmp(o->s[i].path, path)) at = i;
    if (!value || !*value) {
        if (at >= 0) { memmove(&o->s[at], &o->s[at + 1], sizeof *o->s * (size_t)(o->n - at - 1)); o->n--; }
        return;
    }
    if (at < 0) {
        if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 32; o->s = realloc(o->s, sizeof *o->s * (size_t)o->cap); }
        at = o->n++;
        pl_strlcpy(o->s[at].file, file, sizeof o->s[at].file);
        pl_strlcpy(o->s[at].path, path, sizeof o->s[at].path);
    }
    pl_strlcpy(o->s[at].value, value, sizeof o->s[at].value);
}

/* the table entry a stored path belongs to (a %s / @ path matches any name there) */
static bool path_match(const char *pat, const char *path)
{
    for (; *pat; pat++) {
        if ((pat[0] == '%' && pat[1] == 's') || pat[0] == '@') {
            while (*path && *path != '/') path++;
            if (pat[0] == '%') pat++;
            continue;
        }
        if (*pat != *path) return false;
        path++;
    }
    return *path == 0;
}

static const MeshOpt *opt_for(const char *file, const char *path)
{
    for (int i = 0; i < MESH_OPTS_N; i++)
        if (!strcmp(MESH_OPTS[i].file, file) && path_match(MESH_OPTS[i].path, path)) return &MESH_OPTS[i];
    return NULL;
}

/* which mask a file belongs to, for values outside the table */
static unsigned file_mask(const char *file)
{
    for (int i = 0; i < MESH_OPTS_N; i++) if (!strcmp(MESH_OPTS[i].file, file)) {
        unsigned m = 0;
        for (int k = i; k < MESH_OPTS_N; k++) if (!strcmp(MESH_OPTS[k].file, file)) m |= MESH_OPTS[k].when;
        return m;
    }
    return MW_ANY;
}

int mo_count(const MeshOverrides *o, unsigned active)
{
    int n = 0;
    for (int i = 0; i < o->n; i++) {
        const MeshOpt *op = opt_for(o->s[i].file, o->s[i].path);
        unsigned w = op ? op->when : file_mask(o->s[i].file);
        if (w & active) n++;
    }
    return n;
}

unsigned mo_active_mask(int mesher, bool twoD, bool polyConvert)
{
    unsigned m = MW_ANY;
    if (mesher == MESHER_SNAPPY) m |= MW_SNAPPY | MW_QUALITY | MW_FEAT | (twoD ? MW_SNAPPY2D : 0);
    else if (mesher == MESHER_FOAMY) m |= twoD ? MW_FOAMY2D | MW_FEAT : MW_FOAMY | MW_QUALITY;
    else if (mesher_is_cfmesh(mesher)) m |= MW_CF;
    if (polyConvert && !twoD) m |= MW_POLY;
    return m;
}

bool mo_check_text(const char *v, char *err, int cap)
{
    int par = 0, brace = 0, sq = 0;
    bool quote = false;
    for (const char *c = v; *c; c++) {
        if (*c == '"') { quote = !quote; continue; }
        if (quote) continue;
        if (*c == '\n' || *c == '\r') { snprintf(err, (size_t)cap, "One line, please"); return false; }
        if (*c == '(') par++; else if (*c == ')') par--;
        else if (*c == '{') brace++; else if (*c == '}') brace--;
        else if (*c == '[') sq++; else if (*c == ']') sq--;
        else if (*c == ';' && brace == 0) { snprintf(err, (size_t)cap, "No ';' (it ends the entry): only inside { }"); return false; }
        if (par < 0 || brace < 0 || sq < 0) { snprintf(err, (size_t)cap, "A closing bracket with no opening one"); return false; }
    }
    if (quote) { snprintf(err, (size_t)cap, "A quote is not closed"); return false; }
    if (par || brace || sq) { snprintf(err, (size_t)cap, "The brackets don't balance"); return false; }
    return true;
}

static bool is_number(const char *s, bool integer)
{
    char *e;
    if (integer) { (void)strtol(s, &e, 10); } else (void)strtod(s, &e);
    while (*e == ' ') e++;
    return e != s && !*e;
}

bool mo_check(const MeshOpt *op, const char *v, char *err, int cap)
{
    if (!strcmp(v, MO_REMOVE)) return true;
    if (op && !strncmp(op->file, "cmd:", 4)) {
        if (!mo_safe_args(v)) { snprintf(err, (size_t)cap, "No ; & | < > ` $ or quotes in command-line options"); return false; }
        if (op->type == MO_TEXT) return true;
    }
    if (!op) return mo_check_text(v, err, cap);
    switch (op->type) {
    case MO_INT:
        if (!is_number(v, true)) { snprintf(err, (size_t)cap, "A whole number, please"); return false; }
        return true;
    case MO_REAL:
        if (!is_number(v, false)) { snprintf(err, (size_t)cap, "A number, please (1e-6 style is fine)"); return false; }
        return true;
    case MO_BOOL: case MO_WORD: {
        if (!op->choices) return mo_check_text(v, err, cap);
        const char *c = op->choices;
        size_t n = strlen(v);
        while (*c) {
            const char *bar = strchr(c, '|');
            size_t l = bar ? (size_t)(bar - c) : strlen(c);
            if (l == n && !strncmp(c, v, n)) return true;
            c += l + (bar ? 1 : 0);
        }
        snprintf(err, (size_t)cap, "One of: %s", op->choices);
        return false;
    }
    case MO_VEC: {
        double a, b, c; char t;
        if (sscanf(v, " ( %lf %lf %lf ) %c", &a, &b, &c, &t) != 3) { snprintf(err, (size_t)cap, "Three numbers in brackets: (x y z)"); return false; }
        return true;
    }
    case MO_TEXT: default: return mo_check_text(v, err, cap);
    }
}

bool mo_apply(const MeshOverrides *o, const char *file, Str *text, char *err, int cap)
{
    bool any = false;
    for (int i = 0; i < o->n; i++) if (!strcmp(o->s[i].file, file)) any = true;
    if (!any) return true;
    FDFile *f = fd_from_text(file, text->s);
    if (!f) { snprintf(err, (size_t)cap, "Could not read %s back to apply the Advanced settings", file); return false; }
    bool ok = true;
    for (int i = 0; i < o->n && ok; i++) {
        const MOSet *s = &o->s[i];
        if (strcmp(s->file, file)) continue;
        if (!strcmp(s->value, MO_REMOVE)) { fd_remove(f, s->path); continue; }
        FDEntry *e = fd_find(f, s->path);
        if (e && e->kind == FD_DICT) {               /* a whole sub-dictionary given as { ... } */
            if (s->value[0] == '{') { ok = fd_replace_span(f, e->valStart - 1, e->end, s->value); }
            else { snprintf(err, (size_t)cap, "%s: %s is a dictionary: give its value as { ... }", file, s->path); ok = false; }
            continue;
        }
        if (s->value[0] == '{') {                    /* a new sub-dictionary */
            char tmp[600]; snprintf(tmp, sizeof tmp, "%s", s->path);
            if (e) fd_remove(f, s->path);
            if (!fd_ensure_dict(f, tmp)) { snprintf(err, (size_t)cap, "%s: could not add %s", file, s->path); ok = false; continue; }
            e = fd_find(f, s->path);
            if (e) ok = fd_replace_span(f, e->valStart - 1, e->end, s->value);
            continue;
        }
        if (!fd_put(f, s->path, s->value)) { snprintf(err, (size_t)cap, "%s: could not set %s", file, s->path); ok = false; }
    }
    if (ok) { text->len = 0; text->s[0] = 0; str_cat(text, f->text); }
    fd_free(f);
    return ok;
}

bool mo_safe_args(const char *s)
{
    for (; *s; s++) if (strchr(";&|<>`$\"'\\\n\r", *s)) return false;
    return true;
}

bool mo_cmd_flag(const MeshOverrides *o, const char *prog, const char *flag, bool dflt)
{
    char f[64]; snprintf(f, sizeof f, "cmd:%s", prog);
    const char *v = mo_get(o, f, flag);
    if (!v) return dflt;
    return !strcmp(v, "true");
}

const char *mo_cmd_value(const MeshOverrides *o, const char *prog, const char *key, const char *dflt)
{
    char f[64]; snprintf(f, sizeof f, "cmd:%s", prog);
    const char *v = mo_get(o, f, key);
    return v && mo_safe_args(v) ? v : dflt;
}

const char *mo_cmd_extra(const MeshOverrides *o, const char *prog)
{
    const char *v = mo_cmd_value(o, prog, "extra", "");
    return v;
}

void mo_sig_text(const char *casePath, Str *out)
{
    MeshOverrides o; mo_init(&o);
    mo_load(casePath, &o);
    bool head = false;
    for (int i = 0; i < o.n; i++) {
        if (!strcmp(o.s[i].file, "cmd:checkMesh")) continue;   /* doesn't change the mesh */
        if (!head) { str_cat(out, "[advanced]\n"); head = true; }
        str_catf(out, "%s %s = %s\n", o.s[i].file, o.s[i].path, o.s[i].value);
    }
    mo_free(&o);
}
