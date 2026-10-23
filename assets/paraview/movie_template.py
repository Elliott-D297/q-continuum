# movie.py -- written by Q-Continuum: renders every written time of an OpenFOAM case to PNG frames with pvbatch.
# Settings come from the environment (Q-Continuum sets them):
#   QC_FOAM   the case's .foam file        QC_OUT    folder for frame.NNNN.png
#   QC_FIELD  field to colour by           QC_STYLE  surface | slice
#   QC_W/QC_H frame size in pixels         QC_VIEW   a ParaView script that builds the view instead (Claude's view.py)
#   QC_DECOMPOSED=1  the results are only in processor* folders     QC_TWOD=1  a 2-D case (it has "empty" patches)
#   QC_REGION  multi-region cases: the region to show
import glob
import os
from paraview.simple import *

FOAM = os.environ["QC_FOAM"]
OUT = os.environ["QC_OUT"]
FIELD = os.environ.get("QC_FIELD", "")
STYLE = os.environ.get("QC_STYLE", "surface")
W, H = int(os.environ.get("QC_W", "1920")), int(os.environ.get("QC_H", "1080"))
VIEW = os.environ.get("QC_VIEW", "")
os.makedirs(OUT, exist_ok=True)
for f in glob.glob(os.path.join(OUT, "frame.*.png")):
    os.remove(f)


def style(v):
    v.OrientationAxesVisibility = 0
    try:
        v.UseColorPaletteForBackground = 0
    except AttributeError:
        pass
    v.Background = [0.20, 0.23, 0.28]          # slate: still (black-coloured) regions stay visible


layout = None
if VIEW:
    # the view script builds the pipeline and the views; only the frame loop is ours
    __file__ = VIEW
    exec(compile(open(VIEW).read(), VIEW, "exec"))
    try:
        layout = GetLayout()
    except Exception:
        layout = None
    if layout is None:
        layout = CreateLayout("movie")
        layout.AssignView(0, GetActiveViewOrCreate("RenderView"))
else:
    r = OpenFOAMReader(registrationName="case", FileName=FOAM)
    if os.environ.get("QC_DECOMPOSED") == "1":
        r.CaseType = "Decomposed Case"
    r.UpdatePipelineInformation()
    if os.environ.get("QC_REGION"):
        r.MeshRegions = [os.environ["QC_REGION"] + "/internalMesh"]
    times = list(r.TimestepValues) if r.TimestepValues else [0.0]
    r.UpdatePipeline(times[-1])
    b = r.GetDataInformation().GetBounds()
    ext = [b[1] - b[0], b[3] - b[2], b[5] - b[4]]
    centre = [(b[0] + b[1]) / 2, (b[2] + b[3]) / 2, (b[4] + b[5]) / 2]
    thin = ext.index(min(ext))
    twoD = os.environ.get("QC_TWOD") == "1" or min(ext) < 0.05 * max(ext)
    view = CreateView("RenderView")
    style(view)
    layout = CreateLayout("movie")
    layout.AssignView(0, view)
    src = r
    if STYLE == "slice" and not twoD:
        # a plane through the middle, along the longest direction (the flow, usually): normal y, or z when y is longest
        s = Slice(registrationName="middle", Input=r)
        s.SliceType = "Plane"
        s.SliceType.Origin = centre
        n = 2 if ext[1] >= max(ext[0], ext[2]) else 1
        s.SliceType.Normal = [1.0 if k == n else 0.0 for k in range(3)]
        src = s
    d = Show(src, view)
    if not FIELD:
        names = list(r.PointData.keys())
        for pref in ("U", "T", "alpha.water", "p_rgh", "p"):
            if pref in names:
                FIELD = pref
                break
        if not FIELD and names:
            FIELD = names[0]
    if FIELD:
        arr = r.PointData[FIELD] if FIELD in r.PointData.keys() else None
        vec = arr is not None and arr.GetNumberOfComponents() > 1
        ColorBy(d, ("POINTS", FIELD, "Magnitude") if vec else ("POINTS", FIELD))
        lut = GetColorTransferFunction(FIELD)
        try:
            lut.ApplyPreset("Turbo", True)
        except Exception:
            lut.ApplyPreset("Jet", True)
        if vec:
            lut.VectorMode = "Magnitude"
        # one colour scale for the whole movie: the range over a few times, so colours mean the same in every frame
        lo, hi = None, None
        for t in sorted(set([times[len(times) // 4], times[len(times) // 2], times[-1]])):
            src.UpdatePipeline(t)
            a = src.PointData[FIELD] if FIELD in src.PointData.keys() else None
            if a is None:
                continue
            rg = a.GetRange(-1 if vec else 0)
            lo = rg[0] if lo is None else min(lo, rg[0])
            hi = rg[1] if hi is None else max(hi, rg[1])
        if lo is not None and hi > lo:
            lut.RescaleTransferFunction(lo, hi)
            try:
                lut.AutomaticRescaleRangeMode = "Never"
            except Exception:
                pass
        d.SetScalarBarVisibility(view, True)
        bar = GetScalarBar(lut, view)
        bar.Title = FIELD + (" magnitude" if vec else "")
        bar.ComponentTitle = ""
        bar.TitleColor = bar.LabelColor = [0.92, 0.92, 0.92]
    t = AnnotateTime(registrationName="time")
    t.Format = "t = {time:.5g} s"
    td = Show(t, view)
    td.WindowLocation = "Upper Left Corner"
    td.FontSize = 22
    td.Color = [0.92, 0.92, 0.92]
    # the camera: head-on for a 2-D case (along its thin direction), from a three-quarter angle otherwise
    view.CameraFocalPoint = centre
    if twoD:
        pos = list(centre)
        pos[thin] += max(ext) * 3
        view.CameraPosition = pos
        view.CameraViewUp = [0, 1, 0] if thin != 1 else [0, 0, 1]
    else:
        diag = (ext[0] ** 2 + ext[1] ** 2 + ext[2] ** 2) ** 0.5
        view.CameraPosition = [centre[0] - diag, centre[1] - diag, centre[2] + diag * 0.8]
        view.CameraViewUp = [0, 0, 1]
    view.ResetCamera(False)
    if twoD:                                    # flat on: fill the frame (a little margin, room for the colour bar)
        up = 1 if thin != 1 else 2
        across = [k for k in range(3) if k not in (thin, up)][0]
        view.CameraParallelProjection = 1
        view.CameraParallelScale = 0.5 * max(ext[up] * 1.15, ext[across] * 1.3 / (W / H))

layout.SetSize(W, H)
scene = GetAnimationScene()
scene.UpdateAnimationUsingDataTimeSteps()
times = list(GetTimeKeeper().TimestepValues) or [0.0]
for i, t in enumerate(times):
    scene.AnimationTime = t
    SaveScreenshot(os.path.join(OUT, "frame.%04d.png" % i), layout, ImageResolution=[W, H])
    print("frame %d of %d  (t = %g)" % (i + 1, len(times), t), flush=True)
print("Q-Continuum: rendered %d frames into %s" % (len(times), OUT))
