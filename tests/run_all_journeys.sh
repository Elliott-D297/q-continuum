#!/bin/bash
# run_all_journeys.sh <outdir>: every journey and GUI test script with the seed case it needs; prints a table
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$(mktemp -d)}; mkdir -p "$out"
seed=$out/seed; mkdir -p "$seed"
# a private copy of the binary: rebuilding build/ while a long suite runs must not change it under the later rows
src_bin=${QC_BIN:-$here/../build/q-continuum}
case "$src_bin" in *.sh) ;; *) cp "$src_bin" "$out/.q-continuum.bin" && export QC_BIN=$out/.q-continuum.bin;; esac
T=${FOAM_TUTORIALS:-$HOME/OpenFOAM/OpenFOAM-v2606/tutorials}
# coupled: two preCICE tutorials and their shared tools, as laid out in the tutorials repository
PT=${PRECICE_TUTORIALS:-$HOME/src/precice-tutorials}
[ -d "$seed/coupled" ] || { mkdir -p "$seed/coupled" && for t in elastic-tube-3d heat-exchanger tools; do cp -a "$PT/$t" "$seed/coupled/"; done; }
[ -d "$seed/cavity" ] || cp -a "$T/incompressible/icoFoam/cavity/cavity" "$seed/cavity"
[ -d "$seed/pitzDaily" ] || cp -a "$T/incompressible/simpleFoam/pitzDaily" "$seed/pitzDaily"
# a 3-D mesh with fvDOM's ray fields beside T (Mesh > View): blockMesh only, no run
[ -d "$seed/hotRoom" ] || { cp -a "$T/heatTransfer/buoyantSimpleFoam/hotRadiationRoomFvDOM" "$seed/hotRoom" && cp -a "$seed/hotRoom/0.orig" "$seed/hotRoom/0" &&
    (. "${WM_PROJECT_DIR:-$HOME/OpenFOAM/OpenFOAM-v2606}/etc/bashrc" && blockMesh -case "$seed/hotRoom") > "$seed/hotRoom/log.blockMesh" 2>&1; }
[ -d "$seed/fohp" ] || { mkdir -p "$seed/fohp" && for t in flow-over-heated-plate tools; do cp -a "$PT/$t" "$seed/fohp/"; done; }
# two plain cases to couple (Couple two cases): the heated-plate tutorial's participants without their coupling, 0.1 s
[ -d "$seed/cplpair" ] || { mkdir -p "$seed/cplpair" && for p in fluid solid; do cp -a "$PT/flow-over-heated-plate/$p-openfoam" "$seed/cplpair/${p}Case" &&
    rm -f "$seed/cplpair/${p}Case/system/preciceDict" "$seed/cplpair/${p}Case/run.sh" "$seed/cplpair/${p}Case/clean.sh" &&
    (. "${WM_PROJECT_DIR:-$HOME/OpenFOAM/OpenFOAM-v2606}/etc/bashrc"; foamDictionary "$seed/cplpair/${p}Case/system/controlDict" -entry functions -remove
     foamDictionary "$seed/cplpair/${p}Case/system/controlDict" -entry endTime -set 0.1) > /dev/null 2>&1; done; }
[ -d "$seed/damBreak" ] || cp -a "$T/multiphase/interFoam/laminar/damBreak/damBreak" "$seed/damBreak"
# the bug hunt's own cases (tests/bughunt_seeds): #include, patch groups, regex keys; broken dictionaries
for b in "$here"/bughunt_seeds/*; do [ -d "$seed/$(basename "$b")" ] || cp -a "$b" "$seed/"; done
[ -d "$seed/multiRegionHeater" ] || cp -a "$T/heatTransfer/chtMultiRegionFoam/multiRegionHeater" "$seed/multiRegionHeater"
declare -A SEEDS=( [j1_first_case]="" [j2_double_mesh]=cavity [j3_patch_all_fields]=pitzDaily [j4_parallel]=cavity
                   [j5_pause_resume]=cavity [j6_cht]=multiRegionHeater [j7_diverge]=cavity [t_external_edits]=cavity
                   [t_no_write]=multiRegionHeater [t_early_end]=cavity [t_courant_text]=cavity [t_study_handedit]=cavity [t_files_binary]=cavity [t_procs_default]=cavity [j8_next_day]=cavity [t_lcars_sounds]=cavity [t_session_restore]=cavity [t_nav_badges]=cavity [t_coupled_checks]=coupled [t_log_viewer]=cavity [t_empty_states]=cavity [t_field_info]=pitzDaily [t_undo]=cavity [t_actions]=cavity [t_computer]=cavity [t_red_alert]=cavity [t_error_fix]=cavity [t_sensor_sweep]=cavity [t_deme]=cavity [t_study_modes]=pitzDaily [t_chart_expand]=cavity [t_env]=cavity [t_programs]="" [t_review]=cavity [t_log_copy]=cavity [t_mesh_view]=hotRoom [t_jarvis]=cavity [t_captains_log]=cavity [t_bh_values]=cavity [t_bh_bc]=cavityRx [t_bh_roles]=damBreak [t_bh_ask]=brMissing [t_bh_study]=cavity [t_bh_cht_mesh]=multiRegionHeater [t_bh_startfrom]=cavity [t_bh_deleted]=cavity [t_bh_results]=cavity [t_bh_halve]=cavVar [t_plume]="" [t_away]=cavity [t_coupled_case]=fohp [t_cpl_build]=cplpair [t_themes]=cavity [t_theme_moments]=cavity [t_bh_writes]="multiRegionHeater cavity" )
fails=0
for name in ${QC_ONLY:-j1_first_case j2_double_mesh j3_patch_all_fields j4_parallel j5_pause_resume j6_cht j7_diverge j8_next_day t_external_edits t_no_write t_early_end t_courant_text t_study_handedit t_files_binary t_procs_default t_lcars_sounds t_session_restore t_nav_badges t_coupled_checks t_log_viewer t_empty_states t_field_info t_undo t_actions t_computer t_red_alert t_error_fix t_sensor_sweep t_deme t_study_modes t_chart_expand t_env t_programs t_review t_log_copy t_mesh_view t_jarvis t_captains_log t_bh_values t_bh_bc t_bh_roles t_bh_ask t_bh_writes t_bh_study t_bh_cht_mesh t_bh_startfrom t_bh_deleted t_bh_results t_bh_halve t_plume t_away t_coupled_case t_cpl_build t_themes t_theme_moments}; do
    s=${SEEDS[$name]}
    sd=""; for w in $s; do sd="$sd $seed/$w"; done      # one or more seed cases
    rm -rf "$out/$name"; mkdir -p "$out/$name"
    QC_APP_ENV="${QC_APP_ENV:-} QC_TEST_SOUND_LOG=$out/$name/sounds.txt" QC_SEED_CASES="$sd" "$here/run_journey.sh" "$here/journeys/$name.qcs" "$out/$name" > "$out/$name.txt" 2>&1
    rc=$?
    printf "%-22s %s  %s\n" "$name" "$([ $rc = 0 ] && echo PASS || echo FAIL)" "$(grep -o 'clicks=.*' "$out/$name/journey.log" | tail -1)"
    [ $rc = 0 ] || fails=$((fails + 1))
done
exit $fails
