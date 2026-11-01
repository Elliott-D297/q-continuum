#!/bin/bash
# run_visual_matrix.sh <journey.qcs> [outdir]: the journey at several window sizes and UI scales (the user's is 1.5), each
# run's screenshots tiled into <outdir>/sheet_<size>_<scale>.png to look over. Layout bugs (overlaps, cut-off text, a view
# too small) show at some sizes only: look at every sheet after changing a page.
here=$(cd "$(dirname "$0")" && pwd)
j=$1; out=${2:-$(mktemp -d)}; mkdir -p "$out"
fails=0
for cfg in ${QC_MATRIX:-1440x900:1.0 1440x900:1.5 2560x1440:1.5 1920x1080:1.25 1280x800:1.0}; do
    w=${cfg%%:*}; sc=${cfg##*:}; o=$out/${w}_$sc
    QC_WINDOW=$w QC_UI_SCALE=$sc bash "$here/run_journey.sh" "$j" "$o" > /dev/null 2>&1; rc=$?
    echo "$cfg rc=$rc $(grep -m1 FAIL "$o/journey"*.log)"; [ $rc = 0 ] || fails=$((fails + 1))
    python3 - "$o" "$out/sheet_${w}_$sc.png" <<'PY' 2>/dev/null || echo "  (no sheet: python3 with PIL makes them)"
import sys, glob
from PIL import Image
shots = sorted(glob.glob(sys.argv[1] + "/*.png"))
if shots:
    W = 900
    ims = [Image.open(p) for p in shots]
    ims = [im.resize((W, int(im.height * W / im.width))) for im in ims]
    H = max(im.height for im in ims); rows = (len(ims) + 1) // 2
    sh = Image.new("RGB", (2 * W, H * rows), "white")
    for i, im in enumerate(ims): sh.paste(im, ((i % 2) * W, (i // 2) * H))
    sh.save(sys.argv[2])
PY
done
echo "sheets in $out"
exit $fails
