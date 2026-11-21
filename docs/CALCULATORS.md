# Calculators, pre-flight checks and the quality explainer

Mesh overhaul plan steps 2, 3 and 4 (gaps L2, L4, L5, L7, L9, L10, L11). Each calculator has a hand-worked example below
(Given / Find / Solution); `tests/test_calc.c` checks `src/calc.c` against these numbers, worked a second time in a
separate script. Open the dialog with **Calculators** in the palette (Ctrl+K, "calc"), from the STL mesher's prism layers
(**First layer from y+**), from Physics > Setup's walls (**Work it through**) or from Mesh > Generate (**Calculators**). It
fills itself in from the open case each time it opens: the fluid, the inflow speed, the part's size or the inlet's
diameter, the y+ the turbulence model wants, the mesher's surface cells, the cell count, the time step.

Code: `src/calc.c` (the numbers, no drawing), `src/page_calc.c` (the dialog), `src/quality.c` (checkMesh's measures and
its log), `src/page_meshquality.c` (Mesh > Quality), the sweep's `mesh_shape()` in `src/sweep.c` (pre-flight).

## 1. Wall spacing from y+

**Given.** Air at 20 °C (ρ = 1.204 kg/m³, μ = 1.825 × 10⁻⁵ Pa s) flowing at U = 10 m/s along a plate L = 1 m long. Target
y+ = 1 (the wall resolved, as k-omega SST wants for separation or heat transfer). Prism layers growing by r = 1.2.

**Find.** The first cell height, and how many layers cover the boundary layer.

**Solution.**

| Step | Working | Result |
|---|---|---|
| Kinematic viscosity | ν = μ / ρ = 1.825 × 10⁻⁵ / 1.204 | 1.516 × 10⁻⁵ m²/s |
| Reynolds number | Re = U L / ν = 10 × 1 / 1.516 × 10⁻⁵ | 6.597 × 10⁵ (turbulent) |
| Skin friction (White, turbulent plate, at x = L) | Cf = 0.026 Re^(-1/7) | 3.834 × 10⁻³ |
| Wall shear stress | τw = ½ ρ U² Cf = 0.5 × 1.204 × 100 × 3.834 × 10⁻³ | 0.2308 Pa |
| Friction velocity | u_τ = √(τw / ρ) = √(0.2308 / 1.204) | 0.4378 m/s |
| Distance of the first cell centre | y = y+ ν / u_τ = 1 × 1.516 × 10⁻⁵ / 0.4378 | 34.62 µm |
| First cell height (centre half way up) | h₁ = 2 y | **69.24 µm** |
| Boundary layer thickness | δ = 0.37 L Re^(-1/5) | 25.37 mm |
| Layers to cover it | N = ⌈ln(1 + δ (r − 1) / h₁) / ln r⌉ = ⌈ln(1 + 0.02537 × 0.2 / 6.924 × 10⁻⁵) / ln 1.2⌉ | **24** (27.18 mm in all) |

The same correlation is behind Physics > Setup's y+ helper (`sim_wall_spacing`); the test checks both give the same numbers.
To blend into 2 mm surface cells, the layers stop once the outer one reaches 0.3 of the cell (the STL mesher's
LAYER_OUTER_MIN): 69.24 µm × 1.2¹² = 0.617 mm, so 13 layers.

**A cryogen in a pipe.** Liquid nitrogen at its boiling point (NIST: ρ = 806.08 kg/m³, μ = 1.6067 × 10⁻⁴ Pa s) at 2 m/s in a
smooth 25 mm pipe, y+ = 1: Re = 2 × 0.025 / 1.993 × 10⁻⁷ = 2.508 × 10⁵; Haaland, 1/√f = −1.8 log₁₀(6.9 / Re), f = 0.01484
(within 2 % of Colebrook); τw = f ρ U² / 8 = 5.981 Pa; u_τ = 0.08614 m/s; y = 2.314 µm; **h₁ = 4.628 µm**. Liquid nitrogen
has about a fifth of water's kinematic viscosity (1.99 × 10⁻⁷ against 1.00 × 10⁻⁶ m²/s): the first cell is several times
thinner than for water at the same speed.

### Fluid properties

| Fluid | State | ρ kg/m³ | μ Pa s | cp J/(kg K) | k W/(m K) | σ N/m | c m/s |
|---|---|---|---|---|---|---|---|
| Air | 20 °C, 1 atm | 1.204 | 1.825e-5 | 1007 | 0.02514 | – | 343.2 |
| Water | 20 °C, 1 atm | 998.0 | 1.002e-3 | 4182 | 0.598 | 0.0728 | 1482 |
| Liquid nitrogen | boiling, 1 atm, 77.36 K | 806.08 | 1.6067e-4 | 2041.5 | 0.14485 | 8.880e-3 | 851.4 |
| Liquid oxygen | boiling, 1 atm, 90.19 K | 1141.16 | 1.9467e-4 | 1699.3 | 0.15085 | 1.3145e-2 | 904.4 |
| Liquid hydrogen (normal) | boiling, 1 atm, 20.37 K | 70.848 | 1.3490e-5 | 9772 | 0.10372 | 1.9117e-3 | 1118.9 |

Air and water: Cengel & Cimbala, *Fluid Mechanics*, tables A-9 and A-15. Cryogens: NIST Chemistry WebBook, saturated
liquid, linear between the two printed rows on either side of 101.325 kPa (fetched 2026-11-21). The liquids' thermal
expansion (5.7e-3, 4.4e-3, 1.6e-2 1/K) is not in NIST's table: approximate, used only for the Rayleigh number.

## 2. Grading

blockMesh's grading is **R = last cell / first cell** over an edge; snappyHexMesh's layer `expansionRatio` is **r**, one
cell to the next. R = r^(N−1).

**Given.** An edge L = 0.1 m with N = 20 cells and simpleGrading R = 10.
**Find.** The first and last cells.
**Solution.** r = 10^(1/19) = 1.1288; Δ₁ = L (r − 1) / (r^N − 1) = 0.1 × 0.1288 / (1.1288²⁰ − 1) = **1.252 mm**;
Δ_N = R Δ₁ = **12.52 mm**. (The 20 cells add up to 0.1 m: the test sums them.)

**Given.** The same edge, the first cell 1 mm and the last 10 mm. **Find.** N and R.
**Solution.** L = (r Δ_N − Δ₁) / (r − 1), so r = (L − Δ₁) / (L − Δ_N) = 0.099 / 0.09 = 1.1;
N = 1 + ln(Δ_N / Δ₁) / ln r = 1 + ln 10 / ln 1.1 = 25.16 → **25**; keeping R = 10: r = 1.1007, Δ₁ = 1.006 mm.

**Given.** 0.05 m from a wall, the first cell 0.1 mm (from y+), growing by 1.2. **Find.** N.
**Solution.** N = ln(1 + L (r − 1) / Δ₁) / ln r = ln(1 + 0.05 × 0.2 / 10⁻⁴) / ln 1.2 = 25.31 → **25**; the first cell kept,
r becomes 1.2025 (found by bisection) so the 25 cells still fill 0.05 m.

## 3. Cells, memory and run time

    memory to mesh  M_mesh  = 180 MB + N × 2.0 kB                       (snappyHexMesh's peak, one process)
    memory to solve M_solve = 150 MB × P + N × m_cell                   (m_cell 1.3 kB simpleFoam ... 2.5 kB interFoam)
    time a step     t_step  = N × c × k_solver × nOuter / S(P)
                    c       = 2.25 µs × (N / 10⁶)^0.2,   S(P) = P × e^(log₂ P)

**Given.** 1 million cells, steady (simpleFoam), 1000 iterations, 8 processes, on a machine with 29 GB free.
**Solution.** M_mesh = 180 + 10⁶ × 2.0 / 1024 = 2133 MB = **2.1 GB**; M_solve = 150 × 8 + 10⁶ × 1.3 / 1024 = 2470 MB =
**2.4 GB**; c = 2.25 µs × 1^0.2 = 2.25 µs; S(8) = 8 × 0.64³ = 2.10; t_step = 10⁶ × 2.25 µs / 2.10 = 1.07 s; 1000
iterations: **18 minutes**.

The Generate step shows the same estimate, and holds Generate back behind **Generate anyway** when meshing would need more
than 90 % of the free memory, from its own button and from the step bar's (journey `t_mesh_memstop`, with
`QC_TEST_HOST=250,0` pretending 250 MB are free). Ctrl+M is not the STL mesher: it runs whatever blockMesh /
snappyHexMesh dictionaries are in system/, as before.

### Where the numbers come from

Measured on this machine (32 threads, 31 GB, WSL2) with OpenFOAM v2606, 2026-11-21:

| Run | Cells | Time an iteration | Peak memory |
|---|---|---|---|
| simpleFoam k-epsilon, 3-D pitzDaily, serial | 391 200 | 0.730 s | 644 MB |
| simpleFoam k-epsilon, 3-D pitzDaily, serial | 4 694 400 | 14.44 s | 6.24 GB |
| snappyHexMesh, motorBike tutorial | 353 578 | 1 min 42 s in all | 869 MB |
| snappyHexMesh, motorBike, background doubled | 1 892 342 | 14 min 44 s in all | 3.86 GB |
| simpleFoam, the 4.69 M case on 8 processes (scotch) | 4 694 400 | 6.97 s | 1.06 GB the largest process |

Per cell and iteration that is 1.87 µs and 3.07 µs: the linear solvers need more sweeps on a bigger mesh, fitted as
(N / 10⁶)^0.2. Eight processes ran 2.07 times as fast as one: this machine runs out of memory bandwidth long before cores,
as desktop CFD usually does, so e = 0.64 per doubling (8 × 0.64³ = 2.1). The solve memory is the Run page's own figure (precheck.c, 1.3 kB a cell for simpleFoam; the 4.7 M run used
1.33). The other solvers' costs relative to simpleFoam (1.25 pimpleFoam a corrector, 1.8 rhoSimpleFoam, 2.2 interFoam,
1.5 buoyantSimpleFoam) are typical ratios, not measured here. The test holds the model to the measurements within 10 %.
A 1.54 M-cell run that overlapped the large snappyHexMesh run was left out.

## 4. Dimensionless numbers

**Given.** Water at 20 °C at U = 2 m/s in a D = 50 mm pipe, heated 10 K above the fluid, a time step of 0.1 ms on 1 mm cells.

| Number | Working | Value | Verdict |
|---|---|---|---|
| Re = ρ U D / μ | 998 × 2 × 0.05 / 1.002 × 10⁻³ | 9.96 × 10⁴ | turbulent (pipe: over 4000) |
| Ma = U / c | 2 / 1482 | 1.35 × 10⁻³ | incompressible (under 0.3) |
| Pr = μ cp / k | 1.002 × 10⁻³ × 4182 / 0.598 | 7.01 | liquid: thermal layer 0.52 × the velocity layer |
| Ra = g β ΔT D³ / (ν α) | 9.81 × 2.07 × 10⁻⁴ × 10 × 0.05³ / (1.004 × 10⁻⁶ × 1.433 × 10⁻⁷) | 1.76 × 10⁷ | laminar convection |
| Ri = (Ra / Pr) / Re² | (1.76 × 10⁷ / 7.01) / (9.96 × 10⁴)² | 2.5 × 10⁻⁴ | forced convection: leave gravity out |
| Fr = U / √(g D) | 2 / √(9.81 × 0.05) | 2.86 | supercritical (free surface only) |
| We = ρ U² D / σ | 998 × 4 × 0.05 / 0.0728 | 2742 | inertia wins (free surface only) |
| Co = U Δt / Δx | 2 × 10⁻⁴ / 10⁻³ | 0.20 | stable |

Thresholds: Re 2300 / 4000 in a pipe, 5 × 10⁵ / 3 × 10⁶ on a plate; Ma 0.3, 0.8, 1.2; Ra 10⁹; Ri 0.1 and 10; Fr 1;
We 1 and 1000; Co 1 and 5.

## 5. Pre-flight checks (step 3)

In the sweep (`sensor_sweep`, the Run page's "Before you run" list and the check before each run):

- **A 2-D or axisymmetric mesh thicker than one cell.** OpenFOAM v2606 runs a 2-D cavity two cells thick without a word
  (checkMesh: "Mesh OK"; icoFoam runs) at twice the cost. A one-cell mesh has exactly two faces a cell on the empty (or
  wedge) patches, so the boundary file and the cell count tell: `2 × cells / faces` is the thickness.
- **A wedge over 5 degrees.** checkMesh prints each wedge patch's angle to the centre plane ("Wedge front with angle 5
  degrees"); the two add up to the wedge.
- **checkMesh's 2-D and wedge errors** ("Number of edges not aligned with or perpendicular to non-empty directions",
  a wedge patch not planar, no opposite wedge, different axes): problems, with what to do.
- **A case on a Windows drive** (/mnt/c ...): a warning, as file I/O through WSL is 5 to 20 times slower.

Test: `ctest -R preflight_shape` (real blockMesh meshes: a cavity 1 and 2 cells thick, wedges of 5 and 10 degrees).

**Layers that didn't happen.** After a SALOME mesh with prism layers on, Mesh > Generate says when the mesh has none and
why: SALOME's own words from log.salome (it grows viscous layers on CAD faces, so on an STL's bare triangles only for an
all-wall body off the ground), or checkMesh counting no prisms. Journey `t_layers_notice`.

## 6. The quality explainer (step 4)

Mesh > Quality, **What the numbers mean**: a row per checkMesh measure (non-orthogonality, skewness, aspect ratio, cell
volumes, face pyramids, concave faces, face flatness, cell determinant, face weight, volume ratio, closed cells) with its
value, its limit and how many faces or cells are past it; it opens at the worst. Each opens to a picture, what it is in
fluid-mechanics words, what it does to the solution, the limits and the fix.

Limits are checkMesh's own (v2606 source: primitiveMeshCheck.C nonOrthThreshold_ 70, skewThreshold_ 4, aspectThreshold_
1000, closedThreshold_ 1e-6; polyMeshCheck.C / checkGeometry.C determinant 0.001, face weight 0.05, volume ratio 0.01) and
snappyHexMesh's meshQualityDict defaults ($FOAM_ETC/caseDicts/meshQualityDict: maxNonOrtho 65, maxInternalSkewness 4,
maxBoundarySkewness 20, maxConcave 80, minVol 1e-13, minDeterminant 0.001, minFaceWeight 0.05, minVolRatio 0.01);
`test_quality` checks the table against both files.

**Show the flagged cells in ParaView** runs `checkMesh -allGeometry -allTopology -writeSets vtk` (its sets cleared first,
so a fixed mesh shows none), gathers every flagged face and cell into one cellSet, `qualityCells` (`topoSet -dict
system/topoSetDict.quality`; only checkMesh's own sets, never the case's zone sets), writes it with `foamToVTK -cellSet
qualityCells -name postProcessing/quality`, and opens ParaView with `paraview/quality.py`: the flagged cells red inside the
faint mesh, the flagged faces orange. The cellSet stays for `refineMesh -cellSet qualityCells` or `subsetMesh`.
