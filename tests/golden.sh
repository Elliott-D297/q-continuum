#!/bin/bash
# golden.sh [--bless]: screenshot every case page of a fresh cavity copy in each theme and compare with
# tests/golden/<theme>/<page>.png (ImageMagick compare -metric AE -fuzz 2%). A page fails when more than
# QC_GOLDEN_PX pixels (default 0.5 % of the image) differ; the diff image is left next to the shot for inspection.
# The host's free memory, load and core count are fixed (QC_TEST_HOST, QC_TEST_CPUS): the Run page shows them.
# --bless copies the new shots over the goldens: look at the diff images first.
here=$(cd "$(dirname "$0")" && pwd)
bless=0; [ "${1:-}" = --bless ] && bless=1
out=${QC_GOLDEN_DIR:-/tmp/qc-golden}  # one fixed path: the workspace path is drawn on screen, so it must not vary
magick=${QC_MAGICK:-$HOME/miniforge/envs/qc-dev/bin/magick}
T=${FOAM_TUTORIALS:-$HOME/OpenFOAM/OpenFOAM-v2606/tutorials}
names=(lcars classic-dark classic-light stardew pokemon jarvis)
fails=0
for th in 0 1 2 5; do                                        # the game themes: manual screenshots
    name=${names[$th]}
    d=$out/$name
    rm -rf "$d"; mkdir -p "$d/shots" "$d/seed"
    cp -a "$T/incompressible/icoFoam/cavity/cavity" "$d/seed/cavity"
    find "$d/seed/cavity" -exec touch -d '3 hours ago' {} +                 # the Cases page draws "3 h ago"
    QC_TEST_HOST="6000 0" QC_TEST_CPUS=16 QC_THEME=$th QC_SEED_CASES=$d/seed/cavity "$here/run_journey.sh" "$here/journeys/g_pages.qcs" "$d" > "$d/run.txt" 2>&1 \
        || { echo "$name: journey failed (see $d/journey.log)"; fails=$((fails + 1)); continue; }
    for shot in "$d"/shots/*.png; do
        page=$(basename "$shot" .png); gold=$here/golden/$name/$page.png
        if [ $bless = 1 ]; then mkdir -p "$here/golden/$name"; cp "$shot" "$gold"; echo "$name/$page: blessed"; continue; fi
        [ -f "$gold" ] || { echo "$name/$page: NO GOLDEN"; fails=$((fails + 1)); continue; }
        px=$("$magick" compare -metric AE -fuzz 2% "$gold" "$shot" "$d/shots/$page.diff.png" 2>&1 >/dev/null | awk '{print int($1)}')
        total=$("$magick" identify -format '%w %h' "$gold" | awk '{print $1*$2}')
        lim=${QC_GOLDEN_PX:-$((total / 200))}
        if [ "${px:-999999999}" -le "$lim" ]; then echo "$name/$page: ok ($px px)"
        else echo "$name/$page: DIFFERS ($px px > $lim; see $d/shots/$page.diff.png)"; fails=$((fails + 1)); fi
    done
done
exit $fails
