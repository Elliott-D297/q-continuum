# deme_view.py -- written by Q-Continuum: DEME particle frames (CSV with X,Y,Z,r columns) as spheres, one frame per
# time step. Press Play in ParaView to step through them.
import glob
import os
import re
from paraview.simple import *

OUT = "@OUT@"          # the folder with the frames (Q-Continuum fills this in)

# the largest series of numbered CSV files with X,Y,Z,r columns (DEME's WriteSphereFile)
groups = {}
for f in glob.glob(os.path.join(OUT, "*.csv")):
    with open(f) as fh:
        head = fh.readline().strip().split(",")
    if head[:4] != ["X", "Y", "Z", "r"]:
        continue
    groups.setdefault(re.sub(r"\d+(?=\.csv$)", "#", os.path.basename(f)), []).append(f)
files = sorted(max(groups.values(), key=len)) if groups else []
if not files:
    raise SystemExit("no DEME particle frames (CSV with X,Y,Z,r) in " + OUT)

csv = CSVReader(registrationName="particles", FileName=files)
pts = TableToPoints(registrationName="points", Input=csv)
pts.XColumn, pts.YColumn, pts.ZColumn = "X", "Y", "Z"
glyph = Glyph(registrationName="spheres", Input=pts, GlyphType="Sphere")
glyph.ScaleArray = ["POINTS", "r"]
glyph.ScaleFactor = 2.0                 # the sphere source has radius 0.5: x2 r = the particle
glyph.GlyphMode = "All Points"
glyph.GlyphType.ThetaResolution = 12
glyph.GlyphType.PhiResolution = 8
view = GetActiveViewOrCreate("RenderView")
d = Show(glyph, view)
cols = list(pts.PointData.keys())
colour = "absv" if "absv" in cols else ("v" if "v" in cols else None)
if colour:
    ColorBy(d, ("POINTS", colour))
    lut = GetColorTransferFunction(colour)
    try:
        lut.ApplyPreset("Turbo", True)
    except Exception:
        pass
    d.SetScalarBarVisibility(view, True)
scene = GetAnimationScene()
scene.UpdateAnimationUsingDataTimeSteps()
# colours from the last frame (the first is often at rest), kept for every frame
scene.AnimationTime = scene.EndTime
if colour:
    d.RescaleTransferFunctionToDataRange(False, True)
    try:
        lut.AutomaticRescaleRangeMode = "Never"
    except Exception:
        pass
# from the side, z up (DEME's gravity is usually -z), framing where the particles are at the start and at the end
glyph.UpdatePipeline(scene.EndTime)
be = glyph.GetDataInformation().GetBounds()
scene.AnimationTime = scene.StartTime
glyph.UpdatePipeline(scene.StartTime)
bs = glyph.GetDataInformation().GetBounds()
b = [min(bs[0], be[0]), max(bs[1], be[1]), min(bs[2], be[2]), max(bs[3], be[3]), min(bs[4], be[4]), max(bs[5], be[5])]
c = [(b[0] + b[1]) / 2, (b[2] + b[3]) / 2, (b[4] + b[5]) / 2]
size = max(b[1] - b[0], b[3] - b[2], b[5] - b[4])
Render(view)                            # paraview.simple resets the camera on a view's first render: get that over with
view.CameraFocalPoint = c
view.CameraPosition = [c[0] + 0.5 * size, c[1] - 2.4 * size, c[2] + 0.9 * size]   # no ResetCamera: it would fit one frame only
view.CameraViewUp = [0, 0, 1]
view.CameraViewAngle = 30
Render()
