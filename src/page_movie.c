/* page_movie.c -- Make a movie: pvbatch renders every written time to PNG frames (assets/paraview/movie_template.py,
 * or Claude's paraview/view.py for the view), ffmpeg encodes an H.264 mp4, and a copy goes to the Videos folder */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "assets.h"

static struct {
    char path[PL_PATH];               /* the case the dialog was set up for */
    char fields[64][64]; int nFields;
    char field[64];
    int  style;                       /* 0 surface, 1 middle slice */
    int  fps;                         /* 0: 24, 1: 30, 2: 60 */
    int  size;                        /* 0: 1280x720, 1: 1920x1080, 2: 3840x2160 */
    bool useClaude;
    bool openWhenDone;
    char dest[PL_PATH];
    char lastMovie[PL_PATH], lastDest[PL_PATH];   /* for the toast when the job ends */
} M = { .fps = 1, .size = 1, .openWhenDone = true };

static bool has_empty_patch(const Case *c)
{
    for (int i = 0; i < c->nPatches; i++) if (!strcmp(c->patches[i].type, "empty")) return true;
    return false;
}

/* the fields written at the latest time (in processor0 for a decomposed case, in the region's folder for a multi-region one) */
static void read_fields(Case *c)
{
    M.nFields = 0;
    const char *lt = case_latest_result(&c->s);
    if (!lt[0]) return;
    char rel[PL_PATH];
    bool proc = c->s.procTimes && (!c->s.nTimes || atof(c->s.procLatest) > atof(c->s.latest));
    snprintf(rel, sizeof rel, "%s%s%s%s", proc ? "processor0/" : "", lt, c->s.nRegions ? "/" : "", c->s.nRegions ? c->s.regions[0] : "");
    char d[PL_PATH]; pl_join(d, sizeof d, c->s.path, rel);
    NameList l = { 0 };
    pl_list_dir(d, &l, false, true);
    names_sort(&l);
    for (int i = 0; i < l.n && M.nFields < 64; i++) {
        const char *n = l.names[i];
        if (n[0] == '.' || pl_ends_with(n, ".gz") || pl_ends_with(n, "~")) continue;
        pl_strlcpy(M.fields[M.nFields++], n, sizeof M.fields[0]);
    }
    names_free(&l);
    M.field[0] = 0;
    static const char *pref[] = { "U", "T", "alpha.water", "p_rgh", "p", NULL };
    for (int k = 0; pref[k] && !M.field[0]; k++)
        for (int i = 0; i < M.nFields; i++) if (!strcmp(M.fields[i], pref[k])) { pl_strlcpy(M.field, pref[k], sizeof M.field); break; }
    if (!M.field[0] && M.nFields) pl_strlcpy(M.field, M.fields[0], sizeof M.field);
}

static void start_movie(Case *c)
{
    static const int fpsv[] = { 24, 30, 60 }, wv[] = { 1280, 1920, 3840 }, hv[] = { 720, 1080, 2160 };
    int fps = fpsv[M.fps], W = wv[M.size], H = hv[M.size];
    char mdir[PL_PATH]; pl_join(mdir, sizeof mdir, c->s.path, "movie");
    pl_mkdirs(mdir);
    char sp[PL_PATH]; pl_join(sp, sizeof sp, mdir, ".qc-movie.py");
    if (!pl_write_file(sp, (const char *)asset_movie_template_py, asset_movie_template_py_len)) { ui_toast(TOAST_ERR, "Could not write %s", sp); return; }
    app_write_foam_file();
    char foam[PL_PATH], n[300]; snprintf(n, sizeof n, "%s.foam", c->s.name); pl_join(foam, sizeof foam, c->s.path, n);
    char view[PL_PATH] = ""; if (M.useClaude) pl_join(view, sizeof view, c->s.path, "paraview/view.py");
    bool proc = c->s.procTimes && !c->s.nTimes;
    char qf[PL_PATH + 8], qv[PL_PATH + 8], qd[PL_PATH + 8], qfield[80], qreg[80], qpv[PL_PATH + 8], qff[PL_PATH + 8];
    pl_shell_quote(qf, sizeof qf, foam); pl_shell_quote(qv, sizeof qv, view); pl_shell_quote(qd, sizeof qd, M.dest);
    pl_shell_quote(qfield, sizeof qfield, M.field); pl_shell_quote(qreg, sizeof qreg, c->s.nRegions ? c->s.regions[0] : "");
    pl_shell_quote(qpv, sizeof qpv, A.env.pvbatch); pl_shell_quote(qff, sizeof qff, A.env.ffmpeg);
    char out[300]; snprintf(out, sizeof out, "%s_%dfps.mp4", c->s.name, fps);
    char cmd1[8192], cmd2[4096], cmd3[PL_PATH * 3];
    /* pvbatch brings its own libraries: OpenFOAM's (and its MPI) must not leak into it */
    bool fits = pl_fmt(cmd1, sizeof cmd1, "env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME QC_FOAM=%s QC_OUT=movie QC_FIELD=%s QC_STYLE=%s "
             "QC_W=%d QC_H=%d QC_VIEW=%s QC_DECOMPOSED=%d QC_TWOD=%d QC_REGION=%s %s movie/.qc-movie.py",
             qf, qfield, M.style ? "slice" : "surface", W, H, qv, proc ? 1 : 0, has_empty_patch(c) ? 1 : 0, qreg, qpv);
    fits = fits && pl_fmt(cmd2, sizeof cmd2, "ls movie/frame.*.png > /dev/null 2>&1 || { echo 'Q-Continuum: could not render any frames (see log.movie-frames)'; exit 1; }\n"
             "env -u LD_LIBRARY_PATH %s -hide_banner -y -framerate %d -i movie/frame.%%04d.png -c:v libx264 -preset medium -crf 18 "
             "-pix_fmt yuv420p -vf 'pad=ceil(iw/2)*2:ceil(ih/2)*2' -movflags +faststart movie/%s", qff, fps, out);
    fits = fits && pl_fmt(cmd3, sizeof cmd3, "mkdir -p %s && cp movie/%s %s/ && echo Saved %s/%s", qd, out, qd, qd, out);
    if (!fits) { ui_toast(TOAST_ERR, "The paths are too long to make the movie"); return; }
    if (!app_can_start()) return;
    job_reset(A.cj, "Movie");
    job_add(A.cj, "frames", "Render frames (pvbatch)", cmd1, "log.movie-frames", false);
    job_add(A.cj, "encode", "Encode mp4 (ffmpeg)", cmd2, "log.movie-encode", false);
    job_add(A.cj, "copy", "Copy to the Videos folder", cmd3, "log.movie-encode", false);
    A.cj->steps[2].append = true;
    if (job_start(A.cj, c->s.path, app_bashrc(), GetTime())) {
        snprintf(M.lastMovie, sizeof M.lastMovie, "%s", out);
        pl_strlcpy(M.lastDest, M.dest, sizeof M.lastDest);
        ui_toast(TOAST_INFO, "Making the movie: the Run page shows the frames as they render");
    } else ui_toast(TOAST_ERR, "Could not start the movie");
}

void app_movie_finished(Job *j)
{
    if (strcmp(j->title, "Movie") || j->failed || j->stopped) return;
    ui_toast(TOAST_OK, "Movie saved: %s/%s", M.lastDest, M.lastMovie);
    if (M.openWhenDone && !getenv("QC_VIDEOS")) launch_folder(&A.env, M.lastDest);
}

void app_movie_modal(bool *close)
{
    Case *c = A.cs;
    if (!c) { *close = true; return; }
    if (strcmp(M.path, c->s.path)) {                     /* set up for this case */
        pl_strlcpy(M.path, c->s.path, sizeof M.path);
        read_fields(c);
        pl_join(M.dest, sizeof M.dest, env_videos_dir(&A.env), c->s.name);
        M.useClaude = app_claude_view_exists();
    }
    bool missing = !A.env.pvbatch[0] || !A.env.ffmpeg[0];
    char sub[400];
    if (missing) snprintf(sub, sizeof sub, "Needs %s%s%s. %s", !A.env.pvbatch[0] ? "ParaView's pvbatch (sudo apt install paraview)" : "",
                          !A.env.pvbatch[0] && !A.env.ffmpeg[0] ? " and " : "", !A.env.ffmpeg[0] ? "ffmpeg (sudo apt install ffmpeg, or conda install ffmpeg)" : "",
                          "Restart Q-Continuum after installing.");
    else snprintf(sub, sizeof sub, "Renders every written time (%d so far) with ParaView, one frame each, and encodes an mp4.",
                  c->s.nTimes ? c->s.nTimes + 1 : c->s.procTimes + 1);
    Rectangle b = ui_modal_begin("Make a movie", sub, 680, app_claude_view_exists() ? 500 : 464, close);
    float x = b.x, y = b.y + 4, w = b.width;
    ui_disable(missing || !case_has_results(&c->s));
    bool claude = app_claude_view_exists();
    if (claude) {
        ui_checkbox(ui_id("mv-claude"), (Rectangle){ x, y, w, 24 }, &M.useClaude, "Use Claude's view (paraview/view.py) for the picture");
        y += 36;
    } else M.useClaude = false;
    ui_disable(missing || M.useClaude);
    ui_text(F_MED, 13, "Colour by", x, y, TH.text2);
    ui_text(F_MED, 13, "Show", x + w / 2 + 8, y, TH.text2);
    y += 22;
    const char *fl[64]; for (int i = 0; i < M.nFields; i++) fl[i] = M.fields[i];
    char pick[64] = "";
    if (ui_dropdown(ui_id("mv-field"), (Rectangle){ x, y, w / 2 - 8, 36 }, M.field, fl, M.nFields, DD_MONO, pick, sizeof pick, "field")) pl_strlcpy(M.field, pick, sizeof M.field);
    static const char *st[] = { "Whole domain", "Middle slice" };
    ui_segmented(ui_id("mv-style"), (Rectangle){ x + w / 2 + 8, y, w / 2 - 8, 36 }, st, 2, &M.style);
    y += 52;
    ui_disable(missing);
    ui_text(F_MED, 13, "Frames per second", x, y, TH.text2);
    ui_text(F_MED, 13, "Size", x + w / 2 + 8, y, TH.text2);
    y += 22;
    static const char *fp[] = { "24", "30", "60" }, *sz[] = { "720p", "1080p", "4K" };
    ui_segmented(ui_id("mv-fps"), (Rectangle){ x, y, w / 2 - 8, 34 }, fp, 3, &M.fps);
    ui_segmented(ui_id("mv-size"), (Rectangle){ x + w / 2 + 8, y, w / 2 - 8, 34 }, sz, 3, &M.size);
    y += 50;
    ui_text(F_MED, 13, "Save a copy in", x, y, TH.text2);
    y += 22;
    ui_input(ui_id("mv-dest"), (Rectangle){ x, y, w, 36 }, M.dest, sizeof M.dest, "folder", INPUT_MONO);
    y += 46;
    ui_checkbox(ui_id("mv-open"), (Rectangle){ x, y, w, 24 }, &M.openWhenDone, "Open the folder when it's done");
    y += 34;
    static const int fpsv[] = { 24, 30, 60 };
    int frames = (c->s.nTimes ? c->s.nTimes : c->s.procTimes) + 1;
    char t[200]; snprintf(t, sizeof t, "%d frames at %d fps: a %.1f s movie. The frames stay in the case's movie/ folder.", frames, fpsv[M.fps], frames / (double)fpsv[M.fps]);
    ui_text_wrap(F_REG, 12.5f, t, (Rectangle){ x, y, w, 40 }, 18, TH.text3, true);
    ui_disable(false);
    float by = b.y + b.height - 38;
    if (ui_button(ui_id("mv-cancel"), (Rectangle){ x, by, 100, 38 }, NULL, "Cancel", BTN_GHOST)) *close = true;
    float bw = ui_button_width(ICON_PLAY, "Make the movie");
    ui_disable(missing || !case_has_results(&c->s) || A.cj->active || (!M.useClaude && !M.field[0]));
    if (ui_button(ui_id("mv-go"), (Rectangle){ x + w - bw, by, bw, 38 }, ICON_PLAY, "Make the movie", BTN_PRIMARY)) { A.modal = MD_NONE; start_movie(c); app_go(PG_RUN); }
    ui_disable(false);
}
