# Q-Continuum feature inventory

Every user-visible feature, from reading the source on 2026-10-19 (branch `usability-overhaul`, baseline 686a204).
This table drives the test plan: a feature is only *verified* when an automated test exercises it.

**Status** values:

- **verified-real**: an automated test runs it against real OpenFOAM v2606 (or real tutorial files and logs).
- **verified-fakes**: an automated test runs it against fixtures or fakes only.
- **untested**: no automated test. "Manual (date)" in the test column means an earlier session drove it by hand; that is not counted as verification.
- **broken**: observed not to work.

Test IDs refer to `ctest` names (`ctest --test-dir build -N` lists them).

## 1. Shell, navigation and global keys

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| LCARS frame | `app.c` `lcars_header`, `lcars_column` | Header elbow with OpenFOAM version, breadcrumb, column of case pages, general-page buttons in the top bar | Manual screenshots (2026-10-18) | untested |
| Classic shell | `app.c` `draw_sidebar`, `nav_bar`, `draw_topbar` | Sidebar with case pages, top nav bar, breadcrumb bar | Manual screenshots | untested |
| Breadcrumb | `app.c` | Cases / case / page; earlier crumbs are links | none | untested |
| LCARS sounds | `sound.c`, `app.c` `app_sound`, `ui.c` `ui_sound_hook` | Synthesised key chirps (6, a different one each press), denied two-note, error triple beep, run-complete trill, power-down on stop, red-alert klaxon; your own files in ~/.config/q-continuum/sounds (<cue>.wav|ogg|mp3, click-N) replace them, levelled; LCARS theme only (Stardew and Pokémon have their own packs, below); on WSL played by two persistent Windows PowerShell SoundPlayers (WSLg audio stutters/echoes), `--play-sounds <pack>` + QC_SOUND_DEBUG to check; Settings > Appearance > Sounds + Quiet/Medium/Loud; WSL without libpulse plays the run cues through Windows | `t_lcars_sounds` (cue log), `--export-sounds` levels, silent end-to-end Windows fallback | verified-fakes |
| Stardew / Pokémon themes | `ui.c` `make_theme`, `DisplayFace`, `game_radius`, framed `ui_rect_lines`, game `ui_button` bevel; `sound.c` `synth_stardew`, `synth_pokemon` | Game-menu looks on the classic layout: Stardew parchment + wood/orange-bevel frames, Pixelify Sans labels, green OK; Pokémon white boxes + slate/blue double border, Silkscreen caps, blue/yellow cursor, type-colour charts; hard drop shadows; each theme its own synthesised cue pack, overridable in sounds/stardew, sounds/pokemon | Manual screenshots (all g_pages, 2026-10-20); goldens of the other themes unchanged | untested |
| Header actions | `app.c` | Run case / Resume, Stop run, Pause, ParaView, ParaView + Claude, Save N, Clean (case pages only) | Manual | untested |
| Running-elsewhere blocks | `app.c` | Other cases' running jobs listed in the column/sidebar; click opens that case's Run page | Manual (2026-10-17) | untested |
| Environment block | `app.c` | "Environment ready / Set up OpenFOAM"; click opens Settings | none | untested |
| Action registry and shortcuts | `actions.c` | Every app command once (label, shortcut, button, shell equivalent, help, when it can't run); Ctrl+S save, Ctrl+Z/Ctrl+Y undo/redo, Ctrl+R run, Ctrl+M mesh, Ctrl+P ParaView, Ctrl+L expand log, Ctrl+0..8 pages, Ctrl+N new case, Ctrl+, settings, F5 rescan; a blocked shortcut says why | `t_actions`; `actions_covered` fails for any action no journey exercises | verified-real |
| Computer (Ctrl+K) | `palette.c` | One search over actions, cases, patches, fields and every keyword of the case's dictionaries; Enter runs or jumps; `key = value` (or `dict/path = value`) previews old → new and sets it, undoable | `t_computer` | verified-real |
| Shortcut sheet (F1) | `actions.c` `actions_sheet` | Every command with its shortcut and help; greyed with the reason when it can't run; click runs it | `t_actions` | verified-real |
| Undo / redo | `undo.c`, `foamdict.c` `fd_edit_hook` | Every dictionary edit in the open case, 100 steps; header buttons with what the step changes; back to the saved text leaves nothing to save; refuses over a file changed outside since | `t_undo`, `t_actions` | verified-real |
| Outside changes | `foamcase.c` `case_reload_report`, `app.c` | A dictionary changed outside the app reloads (or merges under unsaved edits) with a notice naming it; the case summary follows | `t_external_edits`, `t_undo` | verified-real |
| Esc | `ui.c`, `page_run.c` | Close modal; leave the expanded log | none | untested |
| Window close | `app.c` `main` | Asks when runs are going (Keep running / Stop and quit) or edits are unsaved | Manual (2026-10-17), `--close-at` | untested |
| Interface scale clamp | `app.c` `app_fit_scale` | Uses the chosen scale unless the window is too small | none | untested |
| GPU driver probe | `app.c` `gl_window_works` | On WSL picks d3d12 Mesa driver after probing in a child; falls back to software GL | Manual (2026-10-17) | untested |
| Idle throttling | `app.c` `main` | 60 fps while animating, 12 fps while a job runs, 5 fps idle | Manual CPU measurement (2026-10-17) | untested |
| Toasts | `ui.c` `ui_toast` | Transient info/ok/warn/error messages; clicks pass through, and they fade while the mouse is over them | Asserted by most journeys (`assert toast`) | verified-real |
| Tooltips | `ui.c` `ui_tooltip` | Hover help on buttons | none | untested |

## 2. Command-line flags

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| `<case>` / `--case <dir>` | `app.c` `main` | Open a case at start | Used by tests | verified-fakes |
| `--page <prefix>` | `app.c` | Start on a page (title prefix) | Used by manual tests | untested |
| `--export-sounds <dir>` | `app.c` | Write the LCARS cues as WAV files to listen to | Level check (no clipping) | verified |
| `--screenshot <png> --frames N` | `app.c` | Render N frames, save PNG, quit; never writes settings | Used by manual tests | untested |
| `--hidden` | `app.c` | No window on the desktop | Used by manual tests | untested |
| JARVIS theme | `ui.c` `make_theme` (THEME_JARVIS, `hud`), `hud_backdrop`, HUD `ui_rect_lines` brackets, glow `ui_shadow`, `FACE_JARVIS` (Rajdhani); `app.c` `draw_logo` arc-reactor Q; `sound.c` `synth_jarvis`; `terms.c` JARVIS column | Near-black navy, arc-reactor cyan lines/accent, gold secondary; translucent panels over a 48 px grid + reactor rings; corner brackets on panels; Rajdhani labels/headings (Plex body); glassy sine cue pack, overridable in sounds/jarvis | `t_jarvis` (theme, names, sound hint, Environment, palette); manual screenshots 2026-10-20 | verified-fakes |
| `--theme 0..5`, `--scale f` | `app.c` | Override theme / UI scale | Used by manual tests | untested |
| `--bc F P` | `app.c` | Preselect a BC matrix cell | Manual | untested |
| `--autorun`, `--wait-job` | `app.c`, `page_run.c` | Start the run at once; screenshot when it ends | Manual batch harness (2026-10-17) | untested |
| `--procs N`, `--builtin`, `--continue` | `app.c` | Run settings for tests | Manual | untested |
| `--click f:x:y`, `--drag f:x0:y0:x1:y1`, `--type f:text` | `app.c`, `ui.c` | Injected input at pixel coordinates | Manual | untested |
| `--sweep N`, `--close-at N`, `--scroll px` | `app.c` | Visit every page/tab; simulate window close; scroll all pages | Manual ASan sweep of 557 tutorials (2026-10-17) | untested |
| `--file rel`, `--raw`, `--coupling dir`, `--tab n` | `app.c` | Files page selection, text mode, Coupling page | Manual | untested |
| `--notify-test`, `--version` | `app.c` | One test notification; print version | none | untested |
| `--list-actions` | `app.c` | Print the action registry (id, button, shortcut) | `actions_covered` | verified |

## 3. Cases page

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| Mesh an STL | `page_cases.c`, `page_stlmesh.c` `new_case_from` | Picks an STL, makes `<workspace>/<name>` with the minimal dictionaries and the STL, opens Mesh > STL mesher | Manual | untested |
| Case cards | `page_cases.c` `case_card` | Solver, cells, t/endTime, status chip, path, age; newest activity first | none | untested |
| Workspace scan | `app.c` `app_rescan_cases` | Workspace + one level of grouping folders + recent list; every 3 s | none | untested |
| Search | `page_cases.c` | Filter by name, solver or path | none | untested |
| Category filter | `page_cases.c` | All / Incompressible / Heat transfer / Multiphase / Other / DEME | none | untested |
| Delete (hover icon) | `page_cases.c`, `app.c` `app_ask_delete_case` | Delete dialog: type name / confirm / don't ask; Trash (gio) or permanent; refuses running cases | Manual | untested |
| Open folder dialog | `page_settings.c` `MD_OPEN_FOLDER` | Add any case folder to the list | none | untested |
| Empty state | `page_cases.c` | "No cases here yet" with Start from a tutorial / Open a case folder | none | **broken** (hidden when DEME projects exist, see friction log F-01) |
| OpenFOAM missing callout | `page_cases.c` | Points to Settings | none | untested |
| DEME project cards | `page_deme.c` `deme_card` | DEME projects shown among cases | Manual | untested |
| Adopt external runs | `app.c` `adopt_running` | A run started in a terminal shows as running, with Stop/Pause | Manual (2026-10-18) | untested |

## 4. New case page

| Feature | Where | What it does | How it's tested | Status |
| Plume on a surface (4.4) | `plume.c`, `page_cases.c` `plume_modal` | New case starter: gas, exit Mach/p/T/radius, stand-off, domain, ambient, plate T, resolution, Courant; shows a, Ue, ρe, mdot, p0, NPR, Mach-disk estimate, mesh, Δt, steps and core-minutes live; writes a 5-degree wedge rhoCentralFoam case (Kurganov/vanLeer, laminar, Sutherland) with plate pressure and wall-shear function objects | `plume` (hand-worked numbers), `t_plume` (GUI + real rhoCentralFoam), full default run 107 s on 4 cores | verified-real |
|---|---|---|---|---|
| Starter cards | `page_cases.c` `STARTERS` | cavity, pitzDaily, buoyantCavity, hotRadiationRoom (P1, fvDOM), damBreak | none | untested |
| Tutorial browser | `page_cases.c` `page_new` | Search, group filter, list of every tutorial with solver | none | untested |
| Clone | `page_cases.c` `do_clone` | `cp -a` into the workspace, unique name, path checks, opens the copy | Manual | untested |
| Group clone | `page_cases.c` `tut_needs_group` | Copies the whole tutorial group when the case refers to `../` siblings | Manual tutorial sweep (2026-10-17) | untested |
| Name validation | `page_cases.c`, `foamcase.c` `foam_path_problem` | Refuses '/', leading '.', spaces/quotes OpenFOAM can't run in | none | untested |

## 5. Overview page

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| Title, badges, path copy | `page_case.c` `page_overview` | Name, category, solver; copy path | none | untested |
| Terminal / Open folder / Delete | `page_case.c` | Terminal with OpenFOAM sourced; file manager; delete dialog | none | untested |
| Stat tiles | `page_case.c` | Mesh, simulated time, last run, initial conditions | none | untested |
| Case check list | `page_case.c` `check_row` | Mesh, solver, 0.orig, unsaved, results, ParaView, Claude: each with an action button | none | untested |
| Patch list | `page_case.c` | Patches with type and face count | none | untested |
| Cross-section preview | `page_xsection.c` | Small 2-D section for 2-D cases | Manual (2026-10-18) | untested |

## 6. Mesh page

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| Generate mesh | `page_case.c` `generate_mesh` | blockMesh / surfaceFeatureExtract + snappyHexMesh / checkMesh; multi-region topoSet, splitMeshRegions, 0.orig copy, changeDictionary | Manual | untested |
| Check mesh | `page_case.c` | Runs checkMesh; "Mesh OK" / failed checks callout from log.checkMesh | none | untested |
| Mesh out-of-date warning | `page_case.c`, `foamcase.c` | Dict newer than mesh, or block cell count ≠ mesh | Manual (2026-10-16) | untested |
| Stat tiles | `page_case.c` | Cells, faces, points, patches | none | untested |
| Scale + domain size | `page_case.c` | `scale`/`convertToMeters` editor; bounding box of vertices | none | untested |
| Base size | `page_case.c` `base_size` | One reference size scales all block counts by old/new; refuses >50 M cells | Manual (2026-10-17) | untested |
| Per-block counts | `page_case.c` `parse_blocks` | nx/ny/nz inputs per hex block, cells per block and total | none | untested |
| Patch types (blockMeshDict) | `page_case.c` | Type dropdown per boundary entry | none | untested |
| Patch types (polyMesh) | `page_case.c` `region_mesh` | Type dropdown per patch of an imported or region mesh | Manual | untested |
| snappyHexMesh card | `page_case.c` | castellate/snap/layers switches, maxGlobalCells, locationInMesh | none | untested |
| SALOME tab | `page_salome.c` | Template mesh.py, run headless + ideasUnvToFoam, edit, open GUI, convert only | Manual (2026-10-17) | untested |
| STL mesher tab | `page_stlmesh.c`, `stlmesh.c` | Any STL (ASCII/binary, named solids) chosen in Windows' Open dialog (zenity/kdialog on Linux) or typed; units; watertight check; split by angle into named boundaries with types; internal or wind-tunnel domain; STAR-CCM+-style reference values (base size, target/min surface %, curvature, growth, feature angle), prism layers, box/sphere/cylinder volumetric controls; cell estimate; snappyHexMesh (dictionaries written, user's kept as .orig) or SALOME NETGEN tet / polyhedral (polyDualMesh) / Body Fitting via `salome/stl_mesh.py` reading `mesher/mesher.ini` | `t_stl_mesher.qcs` (flange, snappyHexMesh, Mesh OK, prism layers) | snappy tested; SALOME untested (not installed) |
| Cross section tab | `page_xsection.c`, `xsection.c` | 2-D cells/patches/field view; zoom, pan, fit; field dropdown, edges | Manual (2026-10-18) | untested |
| Region pills | `app_widgets.c` `region_bar` | Choose the region for Mesh/BC/Physics/Numerics | Manual | untested |

## 7. Boundary conditions page

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| Patch × field matrix | `page_bc.c` | Type, value, colour class per cell; internalField row | `j3_patch_all_fields` | verified-real |
| Units and field information | `page_bc.c` `field_desc`, `fieldinfo.c` | Each column's unit from its dimensions (m/s, m²/s³, Pa); fields the turbulence model doesn't read marked "not used"; tooltip and drawer: meaning, typical values, the tutorials' initial values (scanned once, cached) | `t_field_info`, `units` (dims, model fields, tutorial scan) | verified-real |
| Group/regex matching | `page_bc.c` `find_bc` | Exact name, patch group, then "regex" (last wins) as OpenFOAM does | none | untested |
| Missing BC count | `page_bc.c` | Cells marked missing; count includes fields paged out of view | Manual (2026-10-17) | untested |
| Constraint mismatch | `page_bc.c` | Warns when a constraint patch type differs in the field | none | untested |
| Value validation | `page_bc.c` `bc_value_problem` | uniform scalar/vector/tensor component counts | none | untested |
| Field paging | `page_bc.c` | Columns paged when there are many fields | none | untested |
| Drawer: type dropdown | `page_bc.c` `drawer` | Suggested types per field and class; default entries filled in | none | untested |
| Drawer: Add condition | `page_bc.c` | Default type for the field and patch type (wall functions on walls) | Manual | untested |
| Drawer: Override for this patch | `page_bc.c` | Copies a group/regex entry into an exact entry | none | untested |
| Drawer: Add setting / remove | `page_bc.c` | Add or delete keys in the patch entry | none | untested |
| Drawer: Copy to similar patches | `page_bc.c` | Copies the entry to every patch of the same mesh type | none | untested |
| Patch inspector (3.6) | `page_bc.c` `inspector`, `cond_editor` | Click a patch name (or pick a patch in Ctrl+K): every field's condition on it in one drawer, U, p, p_rgh, T, turbulence first, the model's unused fields folded; type, values, Add condition edited in place | `j3_patch_all_fields`, `t_computer` | verified-real |
| Patch roles: Wall / Inlet / Outlet | `page_bc.c` `apply_role` | One click sets every field for the role (noSlip + wall functions; fixedValue at the starting value; p fixedValue + inletOutlet backflow), unsaved and undoable | `j3_patch_all_fields` (then 50 real simpleFoam iterations) | verified-real |
| Run guard | `page_bc.c` `bc_missing_report`, `page_run.c` | Run refuses when a mesh patch has no BC | Manual (2026-10-17) | untested |

## 8. Physics page

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| Turbulence card | `page_dicts.c` `physics_models` | simulationType, RAS/LES model, Turbulence on | none | untested |
| Required fields + Create | `page_dicts.c` `required_fields`, `create_turb_field` | Badges for k/epsilon/omega/nut/nuTilda; Create writes a field with wall functions and derived values | Manual (2026-10-17) | untested |
| Transport card | `page_dicts.c` | nu, transportModel, sigma, phase nu/rho; raw tree fallback | none | untested |
| Thermophysical card | `page_dicts.c` | thermoType parts; mixture tree | none | untested |
| Gravity card | `page_dicts.c` | constant/g value | none | untested |
| Radiation card | `page_dicts.c` | radiation switch, model, solverFreq, fvDOM nPhi/nTheta/maxIter/tolerance, absorption | Manual (2026-10-17) | untested |
| Radiation model defaults | `page_dicts.c` `radiation_model_defaults` | Switching to fvDOM/P1 adds coeffs, div(Ji,Ii_h), Ii/G solver, absorption/scatter/soot | Manual (2026-10-17) | untested |
| Create G / IDefault | `page_dicts.c` `create_radiation_field` | Writes the radiation field with Marshak / greyDiffusive BCs | none | untested |
| All files tab | `page_dicts.c` `files_in` | Tree editor for every dictionary in constant/ | none | untested |

## 9. Numerics page

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| Schemes / Solvers tabs | `page_dicts.c` `dict_sections` | fvSchemes / fvSolution sections with plain titles and hints; tree editor; Add entry | none | untested |
| Parallel tab | `page_dicts.c` | numberOfSubdomains, method, simple/hierarchical split auto-fixed to the process count | none | untested |
| Create decomposeParDict | `page_dicts.c` `create_decompose` | Writes scotch with the default process count | none | untested |
| `decomp_split` | `page_dicts.c` | Prime-factor split; 2-D keeps z = 1 | none | untested |

## 10. Run page

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| Solver, endTime, deltaT, writeInterval, writeControl, adjustTimeStep, maxCo | `page_run.c` | Bound to controlDict | none | untested |
| Processes spinner | `page_run.c` `procs` | From decomposeParDict / Allrun's dict / Settings; serial-only apps forced to 1 | Manual | untested |
| Before you run panel | `page_run.c`, `precheck.c` | Memory, Courant, cores: one line each, full text on hover | Manual (2026-10-18) | untested |
| Pre-run dialog | `page_settings.c` `MD_PRECHECK` | Run anyway / Cancel when a check warns | Manual | untested |
| Built-in steps | `page_run.c` `start_run` | restore0, blockMesh, features, snappy, checkMesh, topoSet, split, region restore, changeDictionary, setFields, decomposePar, user pre steps, solver, reconstructPar, user post steps | Manual batch over tutorials (2026-10-17) | untested |
| Allrun / group Allrun mode | `page_run.c` | Runs the case's (or group's) Allrun after clearing logs; process count reaches its decomposeParDict | Manual | untested |
| Allrun-needed detection | `page_run.c` `allrun_needs_script` | Defaults to Allrun when it runs tools the built-in steps don't | Manual | untested |
| Stale mesh re-mesh | `page_run.c` | Turns mesh steps on when a mesh dict is newer than the mesh | Manual (2026-10-16) | untested |
| Results-overwrite guard | `page_run.c`, `MD_RUN_RESULTS` | Continue / Delete and start over / Cancel | Manual | untested |
| Continue from latest time | `page_run.c` | startFrom latestTime, keeps decomposition, appends the log, refuses past endTime | Manual (2026-10-17) | untested |
| Turbulence-field and BC guards | `page_run.c` | Refuses to start when required fields or patch BCs are missing | Manual | untested |
| Your steps | `page_run.c` `user_steps` | Shell commands before/after the solver, in `.q-continuum.ini` | none | untested |
| Case scripts | `page_run.c` `case_scripts` | Executables in the case (and group Allrun/Allclean) with remembered arguments; risky ones confirm | Manual | untested |
| Pause / Resume | `app.c`, `foamcase.c`, `page_run.c` | SIGUSR1 + stopAtWriteNowSignal, or controlDict stopAt writeNow; resume continues in place | Manual (2026-10-18) | untested |
| Stop | `runner.c` `job_stop` | SIGTERM to the process group, SIGKILL after 4 s | Manual | untested |
| Add to queue | `page_run.c`, `MD_QUEUE_ADD` | Queue the case; asks continue/start over when it has results | Manual | untested |
| Status line + time ruler | `page_run.c` | Running/Finished/Failed/Paused…; t of endTime with ticks | none | untested |
| KPIs | `page_run.c` | Steps, Courant max, Δt, solver time, remaining (ETA), sim time per second | none | untested |
| Resume card | `page_run.c` | Paused or interrupted run explained, Resume button | Manual | untested |
| Fatal error card / error translator | `page_run.c`, `explain.c` | FOAM FATAL in plain words with the case's numbers; Open at line; one-click Fix (saved, undoable) for a missing or misspelt patch condition, unknown BC or model type (closest valid; v2606-only names under v2412), missing scheme or keyword, wrong dimensions, decomposition that doesn't multiply out, no pressure reference, no outflow, unsplit region mesh, no times, a failed write | `explain_fixes` (each mistake run with the real solver, fixed, run again: 12/12), `t_error_fix`, log fixtures `fatal_*` | verified-real |
| RED ALERT | `redalert.c`, `app.c` `red_alert_tick`, `page_run.c` | Divergence watch twice a second: NaN, residual up 1000× above 0.1, Courant over 50 / 5× maxCo / 4× in 10 steps, Δt down 1000× (red); smaller signs and repeated bounding (yellow). Stop and write, Halve Δt (saved, read next step), Continue; klaxon; notice off the Run page | `units` (9 real logs quiet, the diverging cavity alerts, synthetic cases), `t_red_alert` | verified-real |
| SENSOR SWEEP | `sweep.c`, `page_run.c` `precheck_panel`, `MD_PRECHECK` | Patch coverage (exact, regex, inGroups), constraint types, schemes and solvers of the solved fields, pressure units for the solver, decomposition, region files, output volume vs free disk, first write before the end, leftover processor*, solid Fourier number; file:line, Open, Fix; a problem makes Run ask first | `sweep_tutorials` (0 problems in 557 tutorials), `t_sensor_sweep` | verified-real |
| Continuum check (Knudsen, 4.3) | `sweep.c` `knudsen`, `physics.c` | Compressible cases (p in Pa): λ = k_B T/(√2 π d² p) with the gas's VHS diameter from molWeight, Kn = λ/Δx with the mean cell size, at the initial field, every pressure patch and the lowest-pressure cell of the latest time; slip above 0.01 (maxwellSlipU, smoluchowskiJumpT), DSMC above 0.1 | `units` (hand-worked λ, regimes, gases), `sweep_tutorials` (0 false problems), biconic25-55Run35 by hand | verified-real |
| Ask Claude (errors) | `app.c` `app_claude_error` | Writes a prompt with log tail and setup diff; opens Claude Code in a terminal | Manual (2026-10-18), `QC_TEST_TERMINAL` | untested |
| Residual chart | `page_run.c` `run_chart` | Log-scale initial residuals, legend toggle, region pills, hover read-out | none (parser has fixtures) | untested |
| Monitors chart | `page_run.c` `mon_chart`, `monitor.c` | postProcessing time series, file picker, column toggles, hover | Manual (2026-10-18) | untested |
| Stop when converged | `page_run.c`, `app.c` `converge_watch` | Stops (write-and-stop) when a monitored value changes less than tol over N samples | Manual (2026-10-18, NACA case) | untested |
| Log console | `logview.c` `run_console` | Tabs per log in run order, +N overflow, follow, colour by line type (routine lines dimmed), expand to full page; search with match count and Enter for the next; First error; Fold time steps (steps with a warning or error stay open) | `t_log_viewer` | verified-real |
| Commands tab | `logview.c` `commands_view`, `runner.c` cmdlog | Every step run for the case: state, time, exit code, log; copy as terminal commands, or the last run as a script | `t_log_viewer` (clipboard asserted) | verified-real |
| Empty chart: Run case | `page_run.c` `chart_area` | With nothing plotted and nothing running, the chart offers Run case | `t_empty_states` | verified-real |
| Chart/log splitter | `page_run.c` | Drag, fold, double-click resets; saved as run_split | Manual (2026-10-17) | untested |
| Open in ParaView / Clean results / ParaView with Claude / Make a movie | `page_run.c` | Buttons | Manual | untested |
| Oversubscription toast | `page_run.c` | Warns when runs use more processes than cores | Manual | untested |

## 11. Files page

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| File tree | `page_dicts.c` `file_list` | Case-root text files, system/, constant/ (+ region folders), field folder; dirty dots | none | untested |
| Entries view | `app_widgets.c` `dict_tree` | Tree editor with suggestions, add/remove entries | none | untested |
| Text view | `texteditor.c` | Syntax colours, selection, clipboard, undo/redo, find/replace, comment toggle, duplicate line, indent | none | untested |
| Save / Reload / Open in editor | `page_dicts.c` | Per-file save, reload from disk, external editor | none | untested |
| New file | `MD_NEW_FILE` | Creates a dictionary with a FoamFile header, or an empty text file | none | untested |
| Add entry dialog | `MD_ADD_ENTRY` | Keyword + value or sub-dictionary | none | untested |
| Binary field guard | `foamdict.c` | Binary files load read-only and refuse to save | test_foamdict | verified-real |

## 12. History page

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| Changes tab | `page_history.c`, `safety.c` | git history of the setup in `.qc-history`; diff per revision; compare with now; run outcome badges; go back | Manual (2026-10-18) | untested |
| Record the setup now | `page_history.c` | Manual commit | Manual | untested |
| Snapshots tab | `page_history.c`, `safety.c` | Full copies with label; restore (current state to trash first); delete | Manual | untested |
| Deleted results tab | `page_history.c`, `safety.c` | Trash entries from Clean / start over; restore, delete, empty; pruned to 5 entries / 14 days | Manual | untested |
| Captain's log (3.7) | `runlog.c`, `page_history.c` `tab_runlog` | Every run recorded in `<case>/.qcontinuum/log` (solver, version, processes, cells, checkMesh quality, reached/end time, wall and ExecutionTime, last Courant, final residuals, steps, outcome and the reason in words, setup diff since the run before); History tab per theme name; Export as Markdown; go back to a run's setup | `t_captains_log` (3 real icoFoam runs, one failing) | verified-real |

## 13. Queue page

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| Queue list | `page_queue.c` | Waiting runs, running now; persisted in XDG config `queue.ini` | Manual (2026-10-18) | untested |
| Core budget | `page_queue.c` `queue_tick` | Starts queued runs FIFO while cores allow | Manual | untested |
| Pause / resume queue, remove, retry, open | `page_queue.c` | Queue controls | Manual | untested |
| Desktop notification | `env.c` `notify_desktop` | Windows toast via PowerShell on WSL, notify-send elsewhere; never/when not in front/always; min duration; sound | Manual `--notify-test` | untested |

## 14. Studies page

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| New study dialog | `page_studies.c` | One or two parameters (file, keyword, values, template); variants copied with tar; optionally queued | Manual (2026-10-18) | untested |
| Environment panel | `page_env.c` `env_modal`; sidebar "Environment ready" (both styles) | What is installed and what each part is for (OpenFOAM, MPI, ParaView, movies, SALOME, preCICE, CalculiX, DEME/CUDA/GPU, Claude Code, editor, cores/memory/disk, sound), Check again, Terminal with OpenFOAM, Settings button; the sidebar button no longer just opens Settings | `t_env` | verified |
| Residuals Expand | `page_run.c` `chart_max_button`, `A.chartMax`; `page_coupling.c` | The residuals chart (Run page, coupled participant) takes the whole page: Expand button, Ctrl+Shift+L (chart.expand), Esc or Restore back; log and chart expansions exclude each other | `t_chart_expand` | verified |
| Study Basic / Advanced | `page_studies.c` `build_knobs`, `values_editor`, `use_knob` | Basic: this case's usual settings in words (time step, end time, max Co, viscosity, velocity/temperature on fixed-value patches, turbulence model limited to models whose fields exist, relaxation, mesh refinement via base size with cell estimates, write interval), quick picks (×½ ×1 ×2, ×0.1 ×1 ×10, ±20 %) and an even/log range; Advanced: any file + keyword with a keyword list and auto template, pair two settings or every combination; preview of the cases made | `t_study_modes` | verified |
| Study table + chart | `page_studies.c` | Outputs per variant (final residual, latest time, wall time, monitor value); measurements CSV overlay | Manual | untested |
| Queue the variants not run | `page_studies.c` | Adds them to the queue | Manual | untested |

## 15. Coupling page (preCICE)

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| Coupled-run list, open folder, from tutorial | `page_coupling.c` | Finds folders with precice-config.xml | Manual (2026-10-17) | untested |
| Overview diagram | `page_coupling.c` `diagram` | Participants, meshes, exchanges | none | untested |
| Configuration forms | `page_coupling.c` `tab_config` | Scheme, time window, iterations, acceleration, exchanges, data; span-keeping XML edits | test_xml (span round trip over preCICE tutorials) | verified-real (XML layer only) |
| XML tab | `page_coupling.c` | Raw XML editor | none | untested |
| Validate | `page_coupling.c` | precice-config-validate | none | untested |
| Run coupled / Stop all | `page_coupling.c`, `coupling.c` | Starts every participant; per-participant logs and iteration charts | Manual (2026-10-17) | untested |
| Add the adapter | `page_coupling.c` | Adds preCICE_Adapter to a participant's controlDict | none | untested |

## 16. Other programs (DEME, Chrono, CalculiX, code_aster, code_saturne, MOOSE, Elmer, SPARTA)

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| Detection, inputs | `program.c` `prog_detect`, `prog_inputs` | A folder is a project of a program by its files (CMake with find_package(DEME/Chrono), *.inp with *STEP, .export/.comm, DATA+SRC, .i with [Executioner], .sif, in.* with create_box) | `programs` (fixtures from real runs), `test_program` starters | verified-real |
| Cases list | `app.c` `scan_projects`, `page_cases.c` | Workspace, ~/deme, ~/OpenFOAM, home two deep, Settings folders; filter pill per program; cards with program, input, results | `t_programs`, `t_deme` | verified |
| New case starters | `program.c` `PROG_TEMPLATES`, `prog_template_write`, `page_cases.c` `new_program_section`, `page_deme.c` `deme_write_project`; `app.c` `app_prog_dir` | Chrono pendulum (assets/chrono, period vs theory 0.04 %), CalculiX beam/slab, MOOSE heat, Elmer heat, SPARTA wedge, code_aster bar, 4 DEME drivers, empty code_saturne case; made in the program's own folder (Settings > Workspace per program) | `starters` (ctest), `t_programs`, `t_deme`; every starter run for real with `test_program run`, the 4 DEME on the GPU (2026-10-20) | verified-real |
| Installed examples | `program.c` `prog_examples`, `prog_example_copy` | SPARTA examples, MOOSE examples that run on a stock MOOSE app (those defining their own objects are left out); copy resolves symlinks, brings `../x` files in and rewrites the path, makes README-generated inputs (`% python ../../tools/...`, run Python 3/numpy 2-safe) | `test_program examples` + `copy-example`, all 46 run with the real programs 2026-10-20: 43 finish or run cleanly, fft/vtk/python need SPARTA packages the build lacks | verified-real |
| SPARTA style check | `program.c` `sparta_help_ex` (cached spa -h, background), `prog_sparta_has`, `prog_sparta_missing`, `prog_checks` | compute/dump/fix styles and python lines checked against the build before a run, with the package and make line; Environment panel lists SPARTA with missing optional packages | fft/vtk/python examples; `t_jarvis` | verified-real |
| Setup values | `params.c`, `page_prog.c` `page_prog_setup` | The input's values with units and hints, Basic / All, search, edit in place (other bytes kept), line link to Files | `params` (byte-exact edits on real inputs), `t_programs`, `t_review` | verified-real |
| Run | `program.c` `prog_run_steps`, `page_prog.c` `prog_start_run` | Build if needed (DEME/Chrono), ccx/run_aster/code_saturne/MOOSE app/ElmerSolver(+ElmerGrid)/spa_mpi with threads or MPI; exit line in the log | `t_programs` (ccx, spa_mpi on 2 ranks); `test_program run` for all | verified-real |
| Progress, series, errors | `program.c` `prog_parse_line`, `prog_progress` | Time/steps and end, residual-like series per program, fatal messages from each program's error format | `programs` (real logs + provoked error texts) | verified-real |
| Checks and fixes | `program.c` `prog_checks`, `prog_apply_fix` | Missing program, files an input names, foreign .export paths (fix), .comm without .export (fix), not built (build) | `programs` (aster copy fixed) | verified-fakes |
| Results | `program.c` `prog_results`, `page_prog.c` `page_prog_results` | ParaView / cgx / SALOME / table chart / particle viewer; Clean to .qc-trash | `t_programs`, `t_deme` | verified |
| Particle viewer | `page_deme.c` `deme_particles_panel`, `demeparams.c` `deme_frame_load` | DEME CSV frames and SPARTA dumps; side/front/top/iso (isometric from (1,-1,1), z up, depth-sorted, domain box drawn), colour by speed/height/size, play, follow | `t_programs` (SPARTA), `t_deme`; iso checked on real settle frames 2026-10-20 | verified-real |
| DEME Setup form | `page_deme.c` `deme_setup_panel`, `demeparams.c` | params.ini form, presets, Rayleigh check + fix | `t_deme`, `test_deme` | verified |
| DEME driver results, GPU | `page_deme.c` `deme_run_panel` | QC result table, GPU load, crash explanations | `test_deme`; a real settle run (2026-10-20) | verified-fakes |
| DEME + OpenFOAM pair | `page_deme.c` `deme_pair_panel`, `deme_pair_tick` | Start both; one ending stops the other | Manual | untested |
| Program GUIs | `page_prog.c` `tool_row` | code_saturne GUI, cgx, salome_meca, ElmerGUI | Manual | untested |

## 17. ParaView, movies, Claude

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| Open in ParaView | `app.c`, `env.c` `launch_paraview` | Touches `<case>.foam`; Windows ParaView via wslpath, Linux, or custom command | Manual | untested |
| ParaView + Claude | `app.c` `app_paraview_claude` | Claude Code in a terminal writes paraview/view.py; "Open Claude's view" | Manual (2026-10-17) | untested |
| Make a movie | `page_movie.c` | pvbatch frames + ffmpeg mp4, copy to Windows Videos | Manual (2026-10-18) | untested |

## 18. Settings page

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| OpenFOAM installs | `page_settings.c`, `env.c` | Detected installs (v2606 source, /usr/lib/openfoam/openfoam2412); select one; custom etc/bashrc; missing-path warning | Manual | untested |
| Install commands | `page_settings.c` | Copyable apt commands when none found; Detect again | none | untested |
| Workspace, default processes | `page_settings.c` | Cases folder (with folder button), default MPI ranks | none | untested |
| Runs: notifications, delete confirmation, setup history | `page_settings.c` | Notify mode, minimum duration, sound, test; delete confirm mode; history switch | Manual | untested |
| ParaView launch mode | `page_settings.c` | Automatic / Linux / Windows / Custom command | none | untested |
| Text editor command | `page_settings.c` | External editor (`code %f` default) | none | untested |
| Appearance | `page_settings.c` | LCARS / Classic dark / Classic light; 100–200 % scale | Manual screenshots | untested |
| Settings file | `env.c` | `~/.config/q-continuum/settings.ini`, migrated from `~/.config/plenum` | none | untested |

## 19. Core libraries (no UI)

| Feature | Where | What it does | How it's tested | Status |
|---|---|---|---|---|
| Format-preserving parser | `foamdict.c` | Spans for every entry; edits splice one span | `foamdict_tutorials`: 9001 tutorial files round-trip, spans, self-edit, re-parse | verified-real |
| Log parser | `runner.c` | Residual series, Courant, Δt, times, fatal message, nProcs, checkMesh result | `logparse`: 13 real v2606 logs vs reviewed expectations | verified-real |
| Job runner | `runner.c` | bash + bashrc per step, process groups, stop/detach/adopt | none | untested |
| Case model | `foamcase.c` | Summary, patches (polyMesh/blockMeshDict/fields), fields, regions, staleness, results | none | untested |
| Trash, snapshots, git history | `safety.c` | See History | none | untested |
| Pre-run checks | `precheck.c` | Memory model, Courant from bbox and U, free cores | none | untested |
| Monitor reader | `monitor.c` | postProcessing .dat/.csv time series | none | untested |
| preCICE XML | `xmlspan.c`, `coupling.c` | Span-keeping XML parser; participants and checks | `xml_precice_tutorials`; `test_coupling` built but **not registered in ctest** | verified-real (XML) |
| Cross-section reader | `xsection.c` | polyMesh ascii/binary, 2-D detection, field values | none | untested |

| AWAY MISSION (5.1) | `page_away.c`, Run page and action case.away | Packs the case for aerofly: setup only, `aerofly.slurm` (ntasks, nodes, time, partition, account, modules from Settings; the app's steps, srun for the solver, decomposition from $SLURM_NTASKS), checked with the local OpenFOAM v2412 (mesh + solver -dry-run), tar.gz; scp/ssh/rsync commands to copy; never connects | `t_away` (v2412 check, package contents, the script run here under v2412 on 2 ranks through an srun stand-in) | verified-real |

| Coupled run as a case | `program.c` PROG_COUPLED, `foamcase.c` coupled_status, `page_coupling.c` page_cpl_case, `app.c` CPL_PAGES | A folder with precice-config.xml is a project on Cases (participants, results, state from their logs) and opens with its own pages: Overview (diagram, set-up checks, validate), Coupling setup (data, meshes, scheme; XML), Participants (each one's preciceDict interfaces, Open as a case with the way back in the breadcrumb), Run, Files (config, run.sh, preciceDict), History; header Stop stops all participants; a participant's Run page points to its coupled run and refuses to start alone | `t_coupled_case` (real flow-over-heated-plate coupled run, buoyantPimpleFoam + laplacianFoam, 5 windows) | verified-real |
| Couple two cases | `cplbuild.c`, `page_coupling.c` cpl_build_modal (Coupling > Couple two cases) | CHT through preCICE from two plain OpenFOAM cases: copies them (originals untouched), writes precice-config.xml (serial-implicit, Aitken, nearest-neighbor), each preciceDict (CHT module, solver type compressible/basic/incompressible, k for laplacianFoam), the adapter in controlDict, the interface T condition (fixedValue where Temperature is read, fixedGradient where the heat flux is), a self-contained run.sh; checks first: solver supported, patch exists, k given, deltaT divides the time window, end times, 2-D/3-D agree, adjustTimeStep | `cplbuild` (refusals, the tutorial pair), `t_cpl_build` (GUI, then a real 10-window coupled run) | verified-real |
| Screen and game themes | `ui.c` make_theme / retro_face / retro_backdrop / CRT pass, `flavour.c`, `terms.c`, `sound.c` PACKS | Severance, Alien (CRT shader: curvature, scanlines, glow, vignette, flicker; mouse unwarped), Pip-Boy (STAT/INV/DATA, Geiger on the continuity error), NERV (Unit-0N, sync ratio, PATTERN BLUE), Portal (test chambers, the facility AI on checkMesh, cake), Dark Souls (HP/stamina, bonfires per write, YOU DIED + respawn from latestTime), Factorio (assembler, belt of time folders, sim s per wall h, biters), Windows 95 (progress window, paperclip at Co > 1, blue screen); own names, sounds, fonts (SIL OFL) | `t_themes`, `t_theme_moments` | verified-real |
| Star Citizen theme | `ui.c` (RETRO_STARCITIZEN palette, star backdrop, cut-corner MFD panels, Electrolize), `flavour.c`, `terms.c`, `sound.c` | mobiGlas look: runs are contracts in Stanton, quantum-drive bar to endTime on Run, 30k disconnect on failure, aUEC payout on completion; own names and sounds | `t_themes` | verified-real |
| Theme logos + icon | `logo.c`, Settings > Themes grid (`theme#<id>`) | A Q mark per theme; window icon and desktop icon (~/.local/share/icons, WSLg's Start-menu .ico) follow the theme; `--export-logos <dir>` writes all 15 + sheet.png | `t_themes`, `t_actions` | verified-real |
