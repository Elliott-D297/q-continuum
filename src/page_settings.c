/* page_settings.c -- settings page and modal dialogs */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "actions.h"
#include "palette.h"
#include "sweep.h"
#include "precheck.h"
#include "program.h"

static float code_block(float x, float y, float w, const char *text, const char *id)
{
    int lines = 1;
    for (const char *p = text; *p; p++) if (*p == '\n') lines++;
    float h = lines * 21 + 24;
    Rectangle r = { x, y, w, h };
    ui_rect(r, 9, TH.input);
    ui_rect_lines(r, 9, 1, TH.lineSoft);
    float ly = y + 12;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        char line[400]; int n = e ? (int)(e - p) : (int)strlen(p);
        snprintf(line, sizeof line, "%.*s", n, p);
        ui_text(F_MONO, 13, line, x + 16, ly, TH.text);
        ly += 21;
        if (!e) break;
        p = e + 1;
    }
    Rectangle cb = { x + w - 40, y + 8, 30, 30 };
    if (ui_button(ui_id(id), cb, ICON_COPY, NULL, BTN_ICON)) { SetClipboardText(text); ui_toast(TOAST_OK, "Copied to the clipboard"); }
    ui_tooltip(cb, "Copy");
    return h;
}

void page_settings(Rectangle r)
{
    UiScroll *sc = &A.scr[14];
    float top = ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = fminf(r.width - 2 * PAGE_PAD, 920);
    float y = top + 30;
    y += page_header(x, y, w, "Settings", "Where OpenFOAM lives, where your cases go, and how results open in ParaView.");

    /* ---------------- OpenFOAM */
    y += section_head(x, y, w, "OpenFOAM", "Q-Continuum loads this environment (etc/bashrc) before running any tool.");
    const FoamInstall *act = env_active(&A.env, &A.set);
    for (int i = 0; i < A.env.nInst; i++) {
        const FoamInstall *f = &A.env.inst[i];
        bool sel = act && !strcmp(act->bashrc, f->bashrc);
        Rectangle row = { x, y, w, 58 };
        if (ui_row(ui_idn(ui_id("inst"), i), row, sel)) { pl_strlcpy(A.set.bashrc, f->bashrc, sizeof A.set.bashrc); settings_save(&A.set); A.tutScanned = false; }
        DrawCircleV((Vector2){ row.x + 20, row.y + 29 }, 7, sel ? TH.accent : BLANK);
        DrawRing((Vector2){ row.x + 20, row.y + 29 }, 6.5f, 8, 0, 360, 24, sel ? TH.accent : TH.line);
        if (sel) DrawCircleV((Vector2){ row.x + 20, row.y + 29 }, 3.5f, TH.textOnAccent);
        char t[64]; snprintf(t, sizeof t, "OpenFOAM %s", f->version);
        ui_text(F_SEMI, 14.5f, t, row.x + 40, row.y + 10, TH.text);
        ui_text_clip(F_MONO, 12, f->root, row.x + 40, row.y + 32, w - 200, TH.text3);
        if (f->hasTutorials) ui_badge(row.x + w - 120, row.y + 18, ICON_CHECK, "tutorials", TH.ok, TH.okSoft);
        y += 62;
    }
    if (A.env.nInst == 0) {
        const char *msg = A.env.wsl ? "Install the v2606 packages inside this WSL distribution (Ubuntu 24.04), then restart Q-Continuum."
                                    : "Install the v2606 packages, then restart Q-Continuum.";
        float ch = callout_height(w, "OpenFOAM was not found", msg);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "OpenFOAM was not found", msg);
        y += ch + 12;
        y += code_block(x, y, w, "curl -s https://dl.openfoam.com/add-debian-repo.sh | sudo bash\nsudo apt-get install -y openfoam2606-default", "copy-install") + 12;
        if (ui_button(ui_id("detect"), (Rectangle){ x, y, 150, 34 }, ICON_REFRESH_CW, "Detect again", BTN_SECONDARY)) {
            env_detect(&A.env); A.tutScanned = false;
            ui_toast(A.env.nInst ? TOAST_OK : TOAST_WARN, A.env.nInst ? "Found OpenFOAM %s" : "Still not found", A.env.nInst ? A.env.inst[0].version : "");
        }
        y += 48;
    }
    Rectangle cr = prop_row(x, &y, w, "Custom environment", "Path to another etc/bashrc, e.g. a source build in ~/OpenFOAM/OpenFOAM-v2606.", 36);
    char rc[PL_PATH]; snprintf(rc, sizeof rc, "%s", A.set.bashrc);
    if (ui_input(ui_id("bashrc"), (Rectangle){ cr.x, cr.y, cr.width, 36 }, rc, sizeof rc, "Auto-detect", INPUT_MONO)) {
        char e[PL_PATH]; pl_expand_home(e, sizeof e, rc);
        if (!rc[0] || pl_is_file(e)) { pl_strlcpy(A.set.bashrc, rc[0] ? e : "", sizeof A.set.bashrc); settings_save(&A.set); A.tutScanned = false; ui_toast(TOAST_OK, "OpenFOAM environment updated"); }
        else ui_toast(TOAST_ERR, "%s does not exist", e);
    }
    if (A.set.bashrc[0] && !pl_is_file(A.set.bashrc)) {
        /* a saved path that has since gone (a deleted source build): say what is used instead */
        const FoamInstall *fi = env_active(&A.env, &A.set);
        char t[PL_PATH + 160];
        snprintf(t, sizeof t, "%s no longer exists, so %s is used. Clear this field or point it at an existing etc/bashrc.",
                 A.set.bashrc, fi ? fi->version : "no OpenFOAM");
        y += ui_text_wrap(F_REG, 12.5f, t, (Rectangle){ cr.x, y + 4, cr.width, 60 }, 18, TH.warn, true) + 8;
    }
    y += 26;

    /* ---------------- workspace */
    y += section_head(x, y, w, "Workspace", "New cases are copied here, and the Cases page lists everything inside it.");
    cr = prop_row(x, &y, w, "Cases folder", "OpenFOAM's convention is ~/OpenFOAM/<user>-<version>/run.", 36);
    char ws[PL_PATH]; snprintf(ws, sizeof ws, "%s", A.set.workspace);
    if (ui_input(ui_id("ws"), (Rectangle){ cr.x, cr.y, cr.width - 46, 36 }, ws, sizeof ws, NULL, INPUT_MONO)) {
        pl_expand_home(A.set.workspace, sizeof A.set.workspace, ws);
        pl_mkdirs(A.set.workspace);
        settings_save(&A.set);
        app_rescan_cases(true);
    }
    if (ui_button(ui_id("ws-open"), (Rectangle){ cr.x + cr.width - 38, cr.y + 2, 32, 32 }, ICON_FOLDER_OPEN, NULL, BTN_ICON)) {
        char e[PL_PATH]; pl_expand_home(e, sizeof e, A.set.workspace); pl_mkdirs(e); launch_folder(&A.env, e);
    }
    /* each program's own folder: its new projects are made there, beside the software, not in the OpenFOAM workspace */
    for (int g = PROG_DEME; g < PROG_COUNT; g++) {
        char lab[64], cur[PL_PATH], def[PL_PATH];
        pl_fmt_ui(lab, sizeof lab, "%s projects", prog_info(g)->name);
        char keep[PL_PATH]; pl_strlcpy(keep, A.set.progDir[g], sizeof keep);
        A.set.progDir[g][0] = 0; app_prog_dir(g, def, sizeof def); pl_strlcpy(A.set.progDir[g], keep, sizeof A.set.progDir[g]);
        app_prog_dir(g, cur, sizeof cur);
        cr = prop_row(x, &y, w, lab, g == PROG_DEME ? "New projects of each program are made in its own folder. Empty = the default." : NULL, 36);
        char ed[PL_PATH]; pl_strlcpy(ed, A.set.progDir[g], sizeof ed);
        if (ui_input(ui_idn(ui_id("pfolder"), g), (Rectangle){ cr.x, cr.y, cr.width - 46, 36 }, ed, sizeof ed, def, INPUT_MONO)) {
            char e[PL_PATH]; pl_expand_home(e, sizeof e, ed); pl_trim_inplace(e);
            pl_strlcpy(A.set.progDir[g], strcmp(e, def) ? e : "", sizeof A.set.progDir[g]);
            settings_save(&A.set); app_discover_again(); app_rescan_cases(true);
        }
        if (ui_button(ui_idn(ui_id("pfolder-open"), g), (Rectangle){ cr.x + cr.width - 38, cr.y + 2, 32, 32 }, ICON_FOLDER_OPEN, NULL, BTN_ICON)) {
            pl_mkdirs(cur); launch_folder(&A.env, cur);
        }
    }
    /* more places to find projects in (any program; OpenFOAM cases too): two folders deep */
    {
        cr = prop_row(x, &y, w, "More project folders", "Searched two folders deep for cases and projects of every program, besides the "
                      "workspace, the program folders above, ~/OpenFOAM and the folders of your home.", 36);
        static char add[PL_PATH];
        if (ui_input(ui_id("pdir-add"), (Rectangle){ cr.x, cr.y, cr.width - 46, 36 }, add, sizeof add, "e.g. ~/heatBench, then Enter", INPUT_MONO)) {
            char e[PL_PATH]; pl_expand_home(e, sizeof e, add); pl_trim_inplace(e);
            if (e[0] && pl_is_dir(e)) { names_add(&A.set.demeDirs, e); settings_save(&A.set); app_discover_again(); app_rescan_cases(true); add[0] = 0; }
            else if (e[0]) ui_toast(TOAST_ERR, "%s is not a folder", e);
        }
        for (int i = 0; i < A.set.demeDirs.n; i++) {
            Rectangle row = { cr.x, y, cr.width, 30 };
            ui_text_clip(F_MONO, 12.5f, A.set.demeDirs.names[i], row.x + 4, row.y + 7, row.width - 50, TH.text2);
            if (ui_button(ui_idn(ui_id("pdir-del"), i), (Rectangle){ row.x + row.width - 38, row.y, 30, 30 }, ICON_X, NULL, BTN_ICON)) {
                free(A.set.demeDirs.names[i]);
                memmove(&A.set.demeDirs.names[i], &A.set.demeDirs.names[i + 1], sizeof(char *) * (size_t)(A.set.demeDirs.n - i - 1));
                A.set.demeDirs.n--; settings_save(&A.set); app_discover_again(); app_rescan_cases(true);
                break;
            }
            y += 34;
        }
    }
    cr = prop_row(x, &y, w, "Default processes", "Used for new parallel runs. This machine has the number of cores shown.", 36);
    char np[16]; snprintf(np, sizeof np, "%d", A.set.nProcs);
    if (ui_input(ui_id("np"), (Rectangle){ cr.x, cr.y, 100, 36 }, np, sizeof np, NULL, INPUT_NUMERIC)) { int v = atoi(np); if (v >= 1) { A.set.nProcs = v; settings_save(&A.set); } }
    char cores[32]; snprintf(cores, sizeof cores, "%d cores", A.env.nCpu);
    ui_text(F_REG, 13, cores, cr.x + 116, cr.y + 9, TH.text3);
    y += 26;

    /* ---------------- runs */
    y += section_head(x, y, w, "Runs", "What happens around a run: notifications, the queue and the history of each case.");
    static const char *nm[] = { "Never", "When not in front", "Always" };
    cr = prop_row(x, &y, w, "Notify when a run ends", A.env.wsl ? "A Windows notification, with its sound if you like." : "A desktop notification (notify-send).", 34);
    if (ui_segmented(ui_id("notify"), (Rectangle){ cr.x, cr.y, fminf(cr.width, 460), 34 }, nm, 3, &A.set.notify)) settings_save(&A.set);
    cr = prop_row(x, &y, w, "Only runs longer than", "Short runs finish while you watch: no notification for those.", 36);
    char ms[16]; snprintf(ms, sizeof ms, "%d", A.set.notifyMinSec);
    if (ui_input(ui_id("notify-min"), (Rectangle){ cr.x, cr.y, 90, 36 }, ms, sizeof ms, NULL, INPUT_NUMERIC)) { int v = atoi(ms); if (v >= 0) { A.set.notifyMinSec = v; settings_save(&A.set); } }
    ui_text(F_REG, 13, "seconds", cr.x + 102, cr.y + 9, TH.text3);
    cr = prop_row(x, &y, w, "Sound", NULL, 30);
    if (ui_switch(ui_id("notify-sound"), cr.x, cr.y + 2, &A.set.notifySound)) settings_save(&A.set);
    cr = prop_row(x, &y, w, "Test", NULL, 34);
    if (ui_button(ui_id("notify-test"), (Rectangle){ cr.x, cr.y, ui_button_width(ICON_ZAP, "Send a test notification"), 34 }, ICON_ZAP, "Send a test notification", BTN_SECONDARY))
        notify_desktop(&A.env, "Q-Continuum", "Notifications work: you'll get one like this when a long run ends.", A.set.notifySound);
    static const char *dm[] = { "Type its name", "Just confirm", "Don't ask" };
    cr = prop_row(x, &y, w, "Deleting a case", A.set.deleteConfirm == 2 ? "Deleted cases go straight to the Trash (still restorable), with no question asked."
                                                                       : "How sure the Delete dialog makes you be. Handy to relax while trying things out.", 34);
    if (ui_segmented(ui_id("del-confirm"), (Rectangle){ cr.x, cr.y, fminf(cr.width, 460), 34 }, dm, 3, &A.set.deleteConfirm)) settings_save(&A.set);
    {
        char rl[64], rh[200]; pl_fmt_ui(rl, sizeof rl, "Review before saving (%s)", term(T_REVIEW));
        pl_fmt_ui(rh, sizeof rh, "Save, Ctrl+S and Run show each change and the diff first; nothing is written until you press %s.", term(T_REVIEW_GO));
        cr = prop_row(x, &y, w, rl, rh, 30);
        if (ui_switch(ui_id("review-on"), cr.x, cr.y + 2, &A.set.reviewSave)) settings_save(&A.set);
    }
    cr = prop_row(x, &y, w, "Setup history", "Record every case's setup in its .qc-history (git) on each save and run, for History > Changes.", 30);
    if (ui_switch(ui_id("hist-on"), cr.x, cr.y + 2, &A.set.history)) settings_save(&A.set);
    y += 26;

    /* ---------------- ParaView */
    y += section_head(x, y, w, "ParaView", "Results open as <case>.foam, which ParaView's built-in OpenFOAM reader understands.");
    static const char *pvm[] = { "Automatic", "Linux", "Windows", "Custom" };
    cr = prop_row(x, &y, w, "Launch with", A.env.wsl ? "Under WSL, Windows ParaView is usually faster than the Linux build through WSLg." : NULL, 34);
    if (ui_segmented(ui_id("pv-mode"), (Rectangle){ cr.x, cr.y, fminf(cr.width, 420), 34 }, pvm, 4, &A.set.pvMode)) settings_save(&A.set);
    cr = prop_row(x, &y, w, "Linux ParaView", NULL, 22);
    if (A.env.pvLinux[0]) { ui_icon(ICON_CIRCLE_CHECK, cr.x + 8, cr.y + 10, 15, TH.ok); ui_text(F_MONO, 12.5f, A.env.pvLinux, cr.x + 24, cr.y + 2, TH.text2); }
    else { ui_icon(ICON_CIRCLE_X, cr.x + 8, cr.y + 10, 15, TH.text3); ui_text(F_REG, 13, "Not installed:  sudo apt install paraview", cr.x + 24, cr.y + 2, TH.text3); }
    if (A.env.wsl) {
        cr = prop_row(x, &y, w, "Windows ParaView", NULL, 22);
        if (A.env.pvWin[0]) { ui_icon(ICON_CIRCLE_CHECK, cr.x + 8, cr.y + 10, 15, TH.ok); ui_text_clip(F_MONO, 12.5f, A.env.pvWin, cr.x + 24, cr.y + 2, cr.width - 30, TH.text2); }
        else { ui_icon(ICON_CIRCLE_X, cr.x + 8, cr.y + 10, 15, TH.text3); ui_text(F_REG, 13, "Not found in C:\\Program Files (install it from paraview.org)", cr.x + 24, cr.y + 2, TH.text3); }
    }
    if (A.set.pvMode == PV_CUSTOM) {
        cr = prop_row(x, &y, w, "Command", "%f is replaced by the .foam file path.", 36);
        if (ui_input(ui_id("pv-cmd"), (Rectangle){ cr.x, cr.y, cr.width, 36 }, A.set.pvCustom, sizeof A.set.pvCustom, "paraview %f", INPUT_MONO)) settings_save(&A.set);
    }
    cr = prop_row(x, &y, w, "Currently", NULL, 22);
    ui_text_clip(F_REG, 13, paraview_description(&A.env, &A.set), cr.x, cr.y + 2, cr.width, TH.text);
    y += 26;

    /* ---------------- editor */
    y += section_head(x, y, w, "Text editor", "Used for lists and long values that are easier to edit as text.");
    cr = prop_row(x, &y, w, "Command", A.env.code[0] ? "Leave empty to use VS Code." : "Leave empty to use the desktop default (xdg-open).", 36);
    if (ui_input(ui_id("editor"), (Rectangle){ cr.x, cr.y, cr.width, 36 }, A.set.editor, sizeof A.set.editor, A.env.code[0] ? "code %f" : "xdg-open %f", INPUT_MONO)) settings_save(&A.set);
    y += 26;

    /* ---------------- appearance */
    y += section_head(x, y, w, "Appearance", NULL);
    /* one grid of every theme, each tile with its own Q (logo.c); tile ids are "theme#<THEME_*>" */
    static const char *const THEME_NAME[THEME__N] = { "LCARS", "Classic dark", "Classic light", "Stardew", "Pokémon", "JARVIS",
                                                      "Severance", "Alien", "Pip-Boy", "NERV", "Portal", "Dark Souls", "Factorio",
                                                      "Windows 95", "Star Citizen" };
    {
        const float tw = 128, th = 92, gap = 8;
        float cw = w - fminf(300, w * 0.38f);
        int cols = (int)fmaxf(1, floorf((cw + gap) / (tw + gap)));
        int rows = (THEME__N + cols - 1) / cols;
        cr = prop_row(x, &y, w, "Themes", "The classic styles are plain; the rest are homages: a starship console, two game menus, Stark's "
                      "HUD, Lumon's terminal, MU/TH/UR on a real-looking tube, a Pip-Boy, NERV, Aperture, Lordran, a factory floor, "
                      "Windows 95 and a mobiGlas. The window and desktop icon change with it.", rows * (th + gap) - gap);
        UiId tid = ui_id("theme");
        for (int t = 0; t < THEME__N; t++) {
            Rectangle tr = { cr.x + (t % cols) * (tw + gap), cr.y + floorf((float)t / cols) * (th + gap), tw, th };
            bool sel = A.set.theme == t;
            if (ui_row(ui_idn(tid, t), tr, sel) && !sel) { A.set.theme = t; ui_set_theme(A.set.theme); settings_save(&A.set); }
            ui_register(ui_idn(tid, t), tr, THEME_NAME[t]);
            ui_rect_lines(tr, 7, sel ? 2 : 1, sel ? TH.accent : TH.lineSoft);
            logo_draw(t, tr.x + (tw - 44) / 2, tr.y + 12, 44);
            float nw = ui_text_w(F_MED, 12.5f, THEME_NAME[t]);
            ui_text_clip(F_MED, 12.5f, THEME_NAME[t], tr.x + fmaxf(6, (tw - nw) / 2), tr.y + th - 26, tw - 12, sel ? TH.text : TH.text2);
        }
    }
    static const char *const SOUND_HINT[SOUND__N] = {
        "Console beeps on keys, a chirp trill when a run completes and the red-alert klaxon when one fails.",
        "Wooden taps on keys, a marimba sparkle when a run completes and the town bell when one fails.",
        "Menu blips on keys, a fanfare when a run completes and the low-HP beeping when one fails.",
        "Soft holographic ticks on keys, the reactor powering up into a chime when a run completes and a pulsing alarm when one fails.",
        "Polite office chimes: the work is mysterious and important.", "MU/TH/UR's teletype chirps and a two-tone alarm.",
        "Geiger clicks on keys, the level-up chirp when a run completes, crackling rads when one fails.", "Hard beeps and the command centre's klaxon.",
        "Aperture's clean dings, and a polite alarm.", "A stone tap, a bonfire's bell when a run completes, the death toll when one fails.",
        "Mechanical clunks, research completed, and the biters alarm.", "A click, the chord when a run completes, the critical stop when one fails.",
        "Soft MFD blips on keys, the quantum drive spooling up when a run completes, the shields-down warning when one fails." };
    int pk = app_sound_pack();
    char hint[400];
    if (pk >= 0) snprintf(hint, sizeof hint, "%s Your own files in ~/.config/q-continuum/sounds%s replace them.", SOUND_HINT[pk],
                          sound_pack_dir(pk));
    else snprintf(hint, sizeof hint, "Every theme but the classic styles has sounds. The classic styles are always silent.");
    cr = prop_row(x, &y, w, "Sounds", hint, 30);
    if (ui_switch(ui_id("sounds"), cr.x, cr.y + 2, &A.set.sounds)) settings_save(&A.set);
    if (A.set.sounds) {
        static const char *lv[] = { "Quiet", "Medium", "Loud" };
        cr = prop_row(x, &y, w, "Volume", NULL, 34);
        if (ui_segmented(ui_id("sound-level"), (Rectangle){ cr.x, cr.y, 300, 34 }, lv, 3, &A.set.soundLevel)) settings_save(&A.set);
        float bx = cr.x + 316;
        if (ui_button(ui_id("sound-done"), (Rectangle){ bx, cr.y, 120, 34 }, ICON_PLAY, "Complete", BTN_SECONDARY)) app_sound(SND_DONE);
        if (ui_button(ui_id("sound-alert"), (Rectangle){ bx + 128, cr.y, 110, 34 }, ICON_PLAY, "Alert", BTN_SECONDARY)) app_sound(SND_ALERT);
        if (sound_device_state() == 2) {
            const char *m = A.env.wsl ? "No sound device: key beeps need the PulseAudio client (sudo apt install libpulse0, then restart). "
                                        "Until then the run chimes and the klaxon play through Windows."
                                      : "No sound device: install the PulseAudio client (sudo apt install libpulse0) and restart.";
            y += ui_text_wrap(F_REG, 12.5f, m, (Rectangle){ cr.x, y + 2, fminf(cr.width, 620), 60 }, 18, TH.warn, true) + 10;
        } else if (sound_device_state() == 3) {
            const char *m = A.env.wsl ? "The WSL sound server isn't answering (a WSLg fault, usually after sleep or a long uptime). Restarting WSL "
                                        "fixes it: wsl --shutdown in Windows, which also stops anything running in WSL. Until then the run "
                                        "chimes and the klaxon play through Windows."
                                      : "The sound server isn't answering, so the app stays silent. Restart PulseAudio (or PipeWire), then the app.";
            y += ui_text_wrap(F_REG, 12.5f, m, (Rectangle){ cr.x, y + 2, fminf(cr.width, 620), 80 }, 18, TH.warn, true) + 10;
        }
    }
    static const char *scales[] = { "100%", "125%", "150%", "175%", "200%" };
    int si = (int)lroundf((A.set.uiScale - 1.0f) / 0.25f);
    if (si < 0) si = 0;
    if (si > 4) si = 4;
    cr = prop_row(x, &y, w, "Interface size", "Larger sizes suit high-resolution screens.", 34);
    if (ui_segmented(ui_id("scale"), (Rectangle){ cr.x, cr.y, fminf(cr.width, 420), 34 }, scales, 5, &si)) {
        A.set.uiScale = 1.0f + 0.25f * si;
        ui_set_scale(app_fit_scale());
        settings_save(&A.set);
    }
    if (app_fit_scale() < A.set.uiScale - 0.01f) {
        char t[160]; snprintf(t, sizeof t, "Shown at %.0f%% for now: the window is too small for %.0f%%. Enlarge it to get the full size.",
                              app_fit_scale() * 100, A.set.uiScale * 100);
        ui_text_wrap(F_REG, 12.5f, t, (Rectangle){ cr.x, cr.y + 42, fminf(cr.width, 420), 40 }, 18, TH.warn, true);
        y += 30;
    }
    y += 26;

    y += section_head(x, y, w, "About", NULL);
    y += ui_text_wrap(F_REG, 13, "Q-Continuum " QC_VERSION ", a desktop front-end for setting up, running and monitoring OpenFOAM cases. "
                      "Typeset in IBM Plex, Antonio, Pixelify Sans, Silkscreen, Rajdhani, VT323, Share Tech Mono, Cinzel, Titillium Web, Oranienbaum and Electrolize (SIL Open Font License) with Lucide icons (ISC). The Stardew, Pokémon, JARVIS, Severance, Alien, Pip-Boy, NERV, Portal, Dark Souls, Factorio, Windows 95 and Star Citizen themes are fan homages, not affiliated with ConcernedApe, Nintendo / The Pokémon Company, Marvel, Apple TV+, 20th Century Studios, Bethesda, Khara, Valve, FromSoftware, Wube Software, Microsoft or Cloud Imperium Games. "
                      "Not affiliated with or endorsed by OpenCFD Ltd or the OpenFOAM Foundation. OPENFOAM is a registered trademark of OpenCFD Ltd.",
                      (Rectangle){ x, y, fminf(w, 720), 200 }, 20, TH.text3, true) + 30;
    ui_scroll_end(sc, y - top);
}

/* ============================================================================ modals */
static FDFile *modal_file(void)
{
    if (!A.cs) return NULL;
    size_t n = strlen(A.cs->s.path);
    if (strncmp(A.mFile, A.cs->s.path, n) || A.mFile[n] != '/') return NULL;
    return case_dict(A.cs, A.mFile + n + 1);
}

static void finish_unsaved(void)
{
    A.modal = MD_NONE;
    if (A.mFlag) { A.quit = true; return; }
    if (A.pendingOpen[0]) {
        char p[PL_PATH]; pl_strlcpy(p, A.pendingOpen, sizeof p);
        A.pendingOpen[0] = 0;
        app_open_case(p, A.pendingPage);
    }
}

void draw_modals(void)
{
    if (A.modal == MD_NONE) return;
    bool close = false;
    Rectangle b;
    switch (A.modal) {
    case MD_OPEN_FOLDER: {
        b = ui_modal_begin("Open a case folder", "Any folder with system/controlDict. It is added to your case list.", 560, 250, &close);
        ui_input(ui_id("m-open"), (Rectangle){ b.x, b.y, b.width, 38 }, A.mText, sizeof A.mText, "~/OpenFOAM/run/myCase", INPUT_MONO | INPUT_LIVE);
        if (A.mErr[0]) ui_text(F_REG, 13, A.mErr, b.x, b.y + 48, TH.err);
        bool go = ui_button(ui_id("m-open-go"), (Rectangle){ b.x + b.width - 110, b.y + b.height - 38, 110, 38 }, NULL, "Open", BTN_PRIMARY)
                  || (inp_key_pressed(KEY_ENTER) && A.mText[0]);
        if (ui_button(ui_id("m-open-cancel"), (Rectangle){ b.x + b.width - 230, b.y + b.height - 38, 110, 38 }, NULL, "Cancel", BTN_GHOST)) close = true;
        if (go) {
            char p[PL_PATH]; pl_expand_home(p, sizeof p, A.mText);
            if (!case_is_case(p)) snprintf(A.mErr, sizeof A.mErr, "No system/controlDict in that folder.");
            else { settings_add_recent(&A.set, p); settings_save(&A.set); A.modal = MD_NONE; app_rescan_cases(true); app_open_case(p, PG_OVERVIEW); }
        }
        break;
    }
    case MD_CLEAN: {
        char csub[700] = "Moves time directories after the start time, processor folders, log files and postProcessing to the case's "
                         "trash (History > Deleted results). Your setup in system, constant and 0.orig stays.";
        if (A.cs && case_has_results(&A.cs->s)) {
            const CaseSummary *cs = &A.cs->s;
            char where[160];
            if (cs->procTimes) snprintf(where, sizeof where, "%d time directories in the case folder and processor0..%d, which hold %d more",
                                        cs->nTimes, cs->nProcDirs - 1, cs->procTimes);
            else snprintf(where, sizeof where, "%d time director%s", cs->nTimes, cs->nTimes == 1 ? "y" : "ies");
            snprintf(csub, sizeof csub, "Moves the results up to t = %s (%s), the log files and postProcessing to the case's trash, "
                     "where History > Deleted results can restore them. Your setup in system, constant and 0.orig stays.", case_latest_result(cs), where);
        }
        b = ui_modal_begin("Clean results", csub, 600, 320, &close);
        ui_checkbox(ui_id("m-clean-mesh"), (Rectangle){ b.x, b.y + 4, b.width, 24 }, &A.mFlag, "Also remove the generated mesh (blockMesh cases only)");
        ui_checkbox(ui_id("m-clean-perm"), (Rectangle){ b.x, b.y + 38, b.width, 24 }, &A.mFlag2, "Delete permanently (otherwise they go to the case's trash and can be restored)");
        if (ui_button(ui_id("m-clean-cancel"), (Rectangle){ b.x + b.width - 240, b.y + b.height - 38, 110, 38 }, NULL, "Cancel", BTN_GHOST)) close = true;
        if (ui_button(ui_id("m-clean-go"), (Rectangle){ b.x + b.width - 120, b.y + b.height - 38, 120, 38 }, ICON_TRASH_2, "Clean", BTN_DANGER) && A.cs) {
            case_rescan(A.cs);
            if (A.cj->active && !strcmp(A.cj->casePath, A.cs->s.path)) ui_toast(TOAST_WARN, "Stop the run first");
            else if (A.cs->s.runningLog[0]) ui_toast(TOAST_ERR, "%s is still being written: another process is running this case. Stop it first.", A.cs->s.runningLog);
            else {
                char msg[160];
                case_trash_results(A.cs, TR_LOGS | TR_POST | TR_ZERO | (A.mFlag ? TR_MESH : 0), A.mFlag2, "Clean", msg, sizeof msg);
                logset_open(&A.logs, A.cs->s.path);
                A.stepInit = false;
                ui_toast(TOAST_OK, "%s", msg);
                app_rescan_cases(true);
                A.modal = MD_NONE;
            }
        }
        break;
    }
    case MD_PV_CLAUDE: {
        bool have = A.env.claude[0] != 0, view = app_claude_view_exists();
        const char *sub = have
            ? "Claude Code opens in a terminal in this case folder and writes paraview/view.py: a ParaView script with views chosen for "
              "this case. It asks you before it writes a file or runs a command. When it is done, open the view here."
            : "Claude Code isn't installed on this machine. Install it with: curl -fsSL https://claude.ai/install.sh | bash, "
              "run claude once in a terminal to sign in, then come back.";
        b = ui_modal_begin("ParaView with Claude", sub, 640, 300, &close);
        ui_text(F_MED, 13, "What should the view show? (optional)", b.x, b.y + 4, TH.text2);
        ui_disable(!have);
        ui_input(ui_id("m-pvc-req"), (Rectangle){ b.x, b.y + 26, b.width, 38 }, A.mText2, sizeof A.mText2,
                 "e.g. streamlines coloured by velocity, the temperature on the walls", 0);
        ui_disable(false);
        float by = b.y + b.height - 38;
        if (ui_button(ui_id("m-pvc-cancel"), (Rectangle){ b.x, by, 100, 38 }, NULL, "Cancel", BTN_GHOST)) close = true;
        float bx = b.x + b.width, w1 = ui_button_width(ICON_SPARKLES, "Start Claude");
        bx -= w1;
        ui_disable(!have);
        if (ui_button(ui_id("m-pvc-go"), (Rectangle){ bx, by, w1, 38 }, ICON_SPARKLES, "Start Claude", BTN_PRIMARY)) { app_paraview_claude(A.mText2); A.modal = MD_NONE; }
        ui_disable(false);
        if (view) {
            float w2 = ui_button_width(ICON_EXTERNAL_LINK, "Open Claude's view");
            bx -= w2 + 10;
            if (ui_button(ui_id("m-pvc-open"), (Rectangle){ bx, by, w2, 38 }, ICON_EXTERNAL_LINK, "Open Claude's view", BTN_SECONDARY)) { app_paraview_claude_view(); A.modal = MD_NONE; }
        }
        break;
    }
    case MD_UNSAVED: {
        char sub[200]; pl_fmt_ui(sub, sizeof sub, "%d edited file%s in %s %s not been written to disk.", case_dirty_count(A.cs),
                                case_dirty_count(A.cs) == 1 ? "" : "s", A.cs ? A.cs->s.name : "", case_dirty_count(A.cs) == 1 ? "has" : "have");
        b = ui_modal_begin("Save your changes?", sub, 540, 210, &close);
        if (ui_button(ui_id("m-un-save"), (Rectangle){ b.x + b.width - 110, b.y + b.height - 38, 110, 38 }, ICON_SAVE, "Save", BTN_PRIMARY)) {
            if (app_save_all(false)) finish_unsaved();
        }
        if (ui_button(ui_id("m-un-discard"), (Rectangle){ b.x + b.width - 250, b.y + b.height - 38, 130, 38 }, NULL, "Don't save", BTN_SECONDARY)) {
            for (int i = 0; A.cs && i < A.cs->nFiles; i++) if (A.cs->files[i].f->dirty) fd_reload(A.cs->files[i].f);
            finish_unsaved();
        }
        if (ui_button(ui_id("m-un-cancel"), (Rectangle){ b.x, b.y + b.height - 38, 100, 38 }, NULL, "Cancel", BTN_GHOST)) close = true;
        if (close) { A.pendingOpen[0] = 0; A.mFlag = false; }
        break;
    }
    case MD_DELETE: {
        const char *name = pl_basename(A.mPath);
        char sub[PL_PATH + 400];
        snprintf(sub, sizeof sub, "%s\n%s", A.mPath, A.mText2);
        bool typeName = A.set.deleteConfirm == 0;
        b = ui_modal_begin("Delete this case?", sub, 640, typeName ? 330 : 260, &close);
        float y = b.y + 6;
        if (typeName) {
            char lab[200]; snprintf(lab, sizeof lab, "Type %s to confirm", name);
            ui_text(F_MED, 13, lab, b.x, y, TH.text2);
            y += 22;
            ui_input(ui_id("m-del-name"), (Rectangle){ b.x, y, b.width, 38 }, A.mText, sizeof A.mText, "case name", INPUT_MONO | INPUT_LIVE);
            y += 50;
        }
        ui_checkbox(ui_id("m-del-perm"), (Rectangle){ b.x, y, b.width, 24 }, &A.mFlag, "Delete permanently (otherwise it goes to the Trash and can be restored)");
        y += 34;
        if (A.mErr[0]) ui_text_wrap(F_REG, 12.5f, A.mErr, (Rectangle){ b.x, y, b.width, 60 }, 17, TH.err, true);
        if (ui_button(ui_id("m-del-cancel"), (Rectangle){ b.x, b.y + b.height - 38, 100, 38 }, NULL, "Cancel", BTN_GHOST)) close = true;
        bool match = !typeName || !strcmp(A.mText, name);
        const char *go = A.mFlag ? "Delete permanently" : "Move to Trash";
        float bw = ui_button_width(ICON_TRASH_2, go);
        ui_disable(!match);
        if (ui_button(ui_id("m-del-go"), (Rectangle){ b.x + b.width - bw, b.y + b.height - 38, bw, 38 }, ICON_TRASH_2, go, BTN_DANGER)) {
            char err[400];
            char path[PL_PATH]; pl_strlcpy(path, A.mPath, sizeof path);
            if (app_delete_case(path, A.mFlag, err, sizeof err)) {
                ui_toast(TOAST_OK, A.mFlag ? "Deleted %s" : "Moved %s to the Trash", pl_basename(path));
                A.modal = MD_NONE;
            } else pl_fmt_ui(A.mErr, sizeof A.mErr, "%s", err);
        }
        ui_disable(false);
        break;
    }
    case MD_CPL_NEW: case MD_CPL_OPEN: case MD_CPL_RESULTS: { void coupling_modal(bool *close); coupling_modal(&close); break; }
    case MD_HIST_CONFIRM: history_modal(&close); break;
    case MD_MOVIE: app_movie_modal(&close); break;
    case MD_STUDY_NEW: study_new_modal(&close); break;
    case MD_STUDY_OUT: study_out_modal(&close); break;
    case MD_STUDY_EXP: study_exp_modal(&close); break;
    case MD_PROG_CLEAN: prog_clean_modal(&close); break;
    case MD_ENV: env_modal(&close); break;
    case MD_PLUME: plume_modal(&close); break;
    case MD_AWAY: away_modal(&close); break;
    case MD_CPL_BUILD: cpl_build_modal(&close); break;
    case MD_FLAVOUR: ui_modal_layer(); flavour_modal(&close); break;
    case MD_QUEUE_ADD: {
        if (!A.cs) { close = true; break; }
        extern int run_procs_for_check(void), run_mode_for_queue(void);
        const char *lt = case_latest_result(&A.cs->s);
        char sub[300]; pl_fmt_ui(sub, sizeof sub, "%s already has results up to t = %s. When its turn comes, should the run continue from "
                                "there, or start over (the old results go to the case's trash)?", A.cs->s.name, lt);
        b = ui_modal_begin("Add to the queue", sub, 600, 230, &close);
        float by = b.y + b.height - 38;
        if (ui_button(ui_id("m-qa-cancel"), (Rectangle){ b.x, by, 100, 38 }, NULL, "Cancel", BTN_GHOST)) close = true;
        char lab[64]; snprintf(lab, sizeof lab, "Continue from t = %s", lt);
        float w1 = ui_button_width(ICON_PLAY, lab), w2 = ui_button_width(ICON_ROTATE_CCW, "Start over");
        if (ui_button(ui_id("m-qa-cont"), (Rectangle){ b.x + b.width - w1, by, w1, 38 }, ICON_PLAY, lab, BTN_PRIMARY)) {
            app_queue_add(A.cs->s.path, -1, run_procs_for_check(), true, false); A.modal = MD_NONE;
        }
        if (ui_button(ui_id("m-qa-over"), (Rectangle){ b.x + b.width - w1 - 10 - w2, by, w2, 38 }, ICON_ROTATE_CCW, "Start over", BTN_SECONDARY)) {
            app_queue_add(A.cs->s.path, run_mode_for_queue(), run_procs_for_check(), false, true); A.modal = MD_NONE;
        }
        break;
    }
    case MD_PRECHECK: {
        if (!A.cs) { close = true; break; }
        PreCheck pc;
        extern int run_procs_for_check(void);
        precheck_run(&A.cs->s, run_procs_for_check(), app_jobs_procs(), A.env.nCpu, &pc);
        const PreItem *it[3] = { &pc.mem, &pc.co, &pc.cores };
        const Sweep *sw = run_sweep(A.cs, false);
        float need = 0;
        for (int i = 0; i < 3; i++)
            if (it[i]->level == PC_WARN || it[i]->level == PC_BAD) need += 38 + ui_text_wrap(F_REG, 13, it[i]->text, (Rectangle){ 0, 0, 680 - 96, 120 }, 19, BLANK, false);
        int nsw = 0;
        for (int i = 0; i < sw->n && nsw < 5; i++) if (sw->f[i].level == PC_BAD || sw->f[i].ask) {   /* what made Run ask (H-08) */ need += 44 + ui_text_wrap(F_REG, 13, sw->f[i].x.text, (Rectangle){ 0, 0, 680 - 96 - 90, 120 }, 19, BLANK, false); nsw++; }
        int worst = pc.worst > sw->worst ? pc.worst : sw->worst;
        b = ui_modal_begin(worst >= PC_BAD ? "This run will likely fail" : "Before you run", "Q-Continuum found something worth a look:", 680, 190 + fminf(need, 520), &close);
        float y = b.y + 2;
        for (int i = 0; i < 3; i++) {
            const PreItem *p = it[i];
            if (p->level != PC_WARN && p->level != PC_BAD) continue;
            Color col = p->level == PC_BAD ? TH.err : TH.warn;
            ui_icon(p->level == PC_BAD ? ICON_CIRCLE_X : ICON_TRIANGLE_ALERT, b.x + 10, y + 10, 16, col);
            ui_text(F_MED, 14, p->title, b.x + 28, y + 1, col);
            y += 24;
            y += ui_text_wrap(F_REG, 13, p->text, (Rectangle){ b.x + 28, y, b.width - 28, 120 }, 19, TH.text2, true) + 14;
        }
        for (int i = 0, k = 0; i < sw->n && k < 5 && y < b.y + b.height - 90; i++) {
            const Finding *f = &sw->f[i];
            if (f->level != PC_BAD && !f->ask) continue;
            k++;
            ui_icon(ICON_CIRCLE_X, b.x + 10, y + 10, 16, TH.err);
            ui_text_clip(F_MED, 14, f->x.title, b.x + 28, y + 1, b.width - 28 - 90, TH.err);
            if (f->x.fix && ui_button(ui_idn(ui_id("m-pc-fix"), i), (Rectangle){ b.x + b.width - 80, y - 4, 80, 30 }, NULL, "Fix", BTN_SECONDARY)) {
                Explanation ex = f->x; app_apply_fix(&ex); run_sweep(A.cs, true);
            }
            if (f->x.fix) ui_tooltip((Rectangle){ b.x + b.width - 80, y - 4, 80, 30 }, f->x.fixLabel);
            y += 24;
            y += ui_text_wrap(F_REG, 13, f->x.text, (Rectangle){ b.x + 28, y, b.width - 28 - 90, 120 }, 19, TH.text2, true) + 14;
        }
        float by = b.y + b.height - 38;
        if (ui_button(ui_id("m-pc-cancel"), (Rectangle){ b.x, by, 110, 38 }, NULL, "Cancel", BTN_GHOST)) close = true;
        const char *gl = worst >= PC_WARN ? "Run anyway" : "Run";      /* everything fixed from here: just run */
        float bw = ui_button_width(ICON_PLAY, gl);
        if (ui_button(ui_id("m-pc-go"), (Rectangle){ b.x + b.width - bw, by, bw, 38 }, ICON_PLAY, gl, worst >= PC_BAD ? BTN_DANGER : BTN_PRIMARY)) {
            A.precheckOk = true; A.runPending = true; A.modal = MD_NONE;
        }
        if (close) { A.runConfirmed = false; A.precheckOk = false; }
        break;
    }
    case MD_RUN_SCRIPT: {
        char sub[400];
        pl_fmt_ui(sub, sizeof sub, "%s looks like it cleans or rebuilds the case. Scripts like this can delete results or start "
                 "over from t = 0. Run ./%s with no arguments?", A.mText, A.mText);
        b = ui_modal_begin("Run this script?", sub, 560, 260, &close);
        bool res = A.cs && (case_has_results(&A.cs->s) || A.cs->s.lastLog[0]);
        if (res) ui_checkbox(ui_id("m-rs-keep"), (Rectangle){ b.x, b.y + 4, b.width, 24 }, &A.mFlag2, "Move the results and logs to the trash first, so they can be restored");
        if (ui_button(ui_id("m-rs-cancel"), (Rectangle){ b.x, b.y + b.height - 38, 100, 38 }, NULL, "Cancel", BTN_GHOST)) close = true;
        float bw = ui_button_width(ICON_PLAY, "Run it");
        if (ui_button(ui_id("m-rs-go"), (Rectangle){ b.x + b.width - bw, b.y + b.height - 38, bw, 38 }, ICON_PLAY, "Run it", BTN_DANGER)) {
            A.modal = MD_NONE;
            if (res && A.mFlag2) {
                char msg[160], why[300]; pl_fmt_ui(why, sizeof why, "Before ./%s", A.mText);
                case_trash_results(A.cs, TR_LOGS | TR_POST, false, why, msg, sizeof msg);
                logset_open(&A.logs, A.cs->s.path);
                ui_toast(TOAST_INFO, "%s", msg);
            }
            run_case_script(A.mText, A.mText2);
        }
        break;
    }
    case MD_NEW_FILE: {
        b = ui_modal_begin("New file", "Path inside the case. Files under system/, constant/ and 0* get an OpenFOAM header; "
                           "anything else (a script, notes) starts empty.", 560, 250, &close);
        ui_input(ui_id("m-nf-path"), (Rectangle){ b.x, b.y + 4, b.width, 38 }, A.mText, sizeof A.mText, "system/myDict", INPUT_MONO);
        if (A.mErr[0]) ui_text(F_REG, 12.5f, A.mErr, b.x, b.y + 50, TH.err);
        if (ui_button(ui_id("m-nf-cancel"), (Rectangle){ b.x + b.width - 240, b.y + b.height - 38, 110, 38 }, NULL, "Cancel", BTN_GHOST)) close = true;
        if (ui_button(ui_id("m-nf-go"), (Rectangle){ b.x + b.width - 120, b.y + b.height - 38, 120, 38 }, ICON_PLUS, "Create", BTN_PRIMARY) && A.cs) {
            char rel[PL_PATH]; pl_strlcpy(rel, A.mText, sizeof rel); pl_trim_inplace(rel);
            char full[PL_PATH]; pl_join(full, sizeof full, A.cs->s.path, rel);
            if (!rel[0] || rel[strlen(rel) - 1] == '/' || strstr(rel, "..") || rel[0] == '/') snprintf(A.mErr, sizeof A.mErr, "Give a file name inside the case, e.g. system/myDict.");
            else if (pl_exists(full)) pl_fmt_ui(A.mErr, sizeof A.mErr, "%s already exists.", rel);
            else {
                bool dict = pl_starts_with(rel, "system/") || pl_starts_with(rel, "constant/") || rel[0] == '0';
                bool ok;
                if (dict) ok = case_create_dict(A.cs, rel, "dictionary", "\n") != NULL;
                else { char dir[PL_PATH]; pl_dirname(dir, sizeof dir, full); pl_mkdirs(dir); ok = pl_write_file(full, "", 0); }
                if (ok) { snprintf(A.filesSel, sizeof A.filesSel, "%s", rel); A.filesRaw = true; A.modal = MD_NONE; ui_toast(TOAST_OK, "Created %s", rel); }
                else pl_fmt_ui(A.mErr, sizeof A.mErr, "Could not create %s.", rel);
            }
        }
        break;
    }
    case MD_RUN_RESULTS: {
        const CaseSummary *cs = A.cs ? &A.cs->s : NULL;
        if (!cs) { close = true; break; }
        const char *latest = case_latest_result(cs);
        char sub[900]; int k = 0;
        k += snprintf(sub + k, sizeof sub - k, "%s has results up to t = %s", cs->name, latest);
        if (cs->procTimes) k += snprintf(sub + k, sizeof sub - k, " (%d time director%s in processor0..%d)", cs->procTimes,
                                         cs->procTimes == 1 ? "y" : "ies", cs->nProcDirs - 1);
        if (A.runMode == 1)
            snprintf(sub + k, sizeof sub - k, ". Running Allrun from the start DELETES those results (time directories after t = %g "
                     "and processor*) and every log.* file first; the script then meshes and decomposes again. If the case has its own "
                     "resume option, use it from a terminal instead.", cs->startTime);
        else
            snprintf(sub + k, sizeof sub - k, ". Delete and start over moves them to the trash (History > Deleted results brings them back)%s%s "
                     "and runs from t = %g.", cs->hasZeroOrig ? ", restores 0 from 0.orig" : "",
                     cs->procTimes ? ", decomposes again (processor* goes to the trash too)" : "", cs->startTime);
        /* continuing only makes sense below the end time (H-18): the endTime as edited, saved or not */
        double endT = cs->endTime;
        { FDFile *cd = case_dict(A.cs, "system/controlDict"); char v[64]; if (cd && fd_get(cd, "endTime", v, sizeof v) && pl_is_number(v)) endT = atof(v); }
        bool pastEnd = latest[0] && endT > 0 && atof(latest) >= endT * (1 - 1e-9);
        /* a changed mesh: continuing would run on the old one, so the results can't continue */
        char meshWhy[300] = "", n0[32];
        bool meshNew = A.runMode == 0 && mesh_change_pending(A.cs, meshWhy, sizeof meshWhy);
        snprintf(n0, sizeof n0, "%ld", cs->nCells);
        float extra = 0;
        char mtext[500] = "";
        if (meshNew) {
            snprintf(mtext, sizeof mtext, "The mesh changes: %s Continuing keeps the old %s-cell mesh (and its fields), so your change "
                     "would be ignored. Start over to mesh again.", meshWhy, n0);
            extra = ui_text_wrap(F_REG, 13, mtext, (Rectangle){ 0, 0, 620 - 58, 200 }, 19, BLANK, false) + 12;
        }
        b = ui_modal_begin("This case already has results", sub, 620, 230 + extra, &close);
        if (meshNew) ui_text_wrap(F_REG, 13, mtext, (Rectangle){ b.x, b.y + 4, b.width, 200 }, 19, TH.warn, true);
        float by = b.y + b.height - 38;
        if (ui_button(ui_id("m-rr-cancel"), (Rectangle){ b.x, by, 100, 38 }, NULL, "Cancel", BTN_GHOST)) close = true;
        float bx = b.x + b.width;
        if (A.runMode == 0) {
            char lab[64]; snprintf(lab, sizeof lab, "Continue from t = %s", latest);
            float w = ui_button_width(ICON_PLAY, lab);
            bx -= w;
            ui_disable(meshNew || pastEnd);
            if (ui_button(ui_id("m-rr-cont"), (Rectangle){ bx, by, w, 38 }, ICON_PLAY, lab, meshNew || pastEnd ? BTN_SECONDARY : BTN_PRIMARY)) {
                A.continueRun = true; A.runConfirmed = true; A.runPending = true; A.modal = MD_NONE;
            }
            ui_disable(false);
            if (pastEnd) {
                char why[160]; snprintf(why, sizeof why, "The results already reach endTime %g: raise endTime to continue past t = %s.", endT, latest);
                ui_tooltip((Rectangle){ bx, by, w, 38 }, why);
                ui_text_wrap(F_REG, 13, why, (Rectangle){ b.x, by - 30, b.width, 30 }, 19, TH.warn, true);
            }
            bx -= 10;
        }
        const char *over = A.runMode == 1 ? "Delete and run Allrun" : "Delete and start over";
        float w2 = ui_button_width(ICON_TRASH_2, over);
        if (ui_button(ui_id("m-rr-over"), (Rectangle){ bx - w2, by, w2, 38 }, ICON_TRASH_2, over, BTN_DANGER)) {
            A.continueRun = false; A.runConfirmed = true; A.runPending = true; A.modal = MD_NONE;
        }
        break;
    }
    case MD_QUIT_RUNNING: {
        int nrun = app_jobs_active();
        char sub[600], names[300] = ""; int k = 0;
        for (int i = 0; i <= MAX_JOBS; i++)
            if (A.jobs[i].active && k < (int)sizeof names - 2) k += snprintf(names + k, sizeof names - (size_t)k, "%s%s", k ? ", " : "", pl_basename(A.jobs[i].casePath));
        snprintf(sub, sizeof sub, "%s. You can leave %s running in the background: %s writing %s logs, and Q-Continuum picks them up the next "
                 "time you open the case. Steps still to come run too, once the one before succeeds.", names,
                 nrun == 1 ? "it" : "them", nrun == 1 ? "it keeps" : "they keep", nrun == 1 ? "its" : "their");
        b = ui_modal_begin(nrun == 1 ? "A run is still going" : "Runs are still going", sub, 560, 260, &close);
        if (ui_button(ui_id("m-q-keep"), (Rectangle){ b.x + b.width - 170, b.y + b.height - 38, 170, 38 }, NULL, "Keep running", BTN_PRIMARY)) {
            for (int i = 0; i <= MAX_JOBS; i++) if (A.jobs[i].active) { job_detach(&A.jobs[i]); A.jobs[i].active = false; }
            A.quit = true; A.modal = MD_NONE;
        }
        if (ui_button(ui_id("m-q-stop"), (Rectangle){ b.x + b.width - 320, b.y + b.height - 38, 140, 38 }, ICON_SQUARE, "Stop and quit", BTN_DANGER)) {
            for (int i = 0; i <= MAX_JOBS; i++) job_stop(&A.jobs[i], GetTime());
            A.quit = true; A.modal = MD_NONE;
        }
        if (ui_button(ui_id("m-q-cancel"), (Rectangle){ b.x, b.y + b.height - 38, 100, 38 }, NULL, "Cancel", BTN_GHOST)) close = true;
        break;
    }
    case MD_CONFLICT: {
        int fi = -1;
        if (!A.cs || !case_conflicts(A.cs, &fi)) { close = true; break; }
        FDFile *f = A.cs->files[fi].f;
        const char *rel = A.cs->files[fi].rel;
        char sub[600];
        snprintf(sub, sizeof sub, "%s was changed outside Q-Continuum (an editor, a script, a run) while you had unsaved edits to "
                 "the same entries here. Nothing has been written. Choose which version to keep; the other is lost.", rel);
        b = ui_modal_begin("Changed in two places", sub, 760, 520, &close);
        /* the lines that differ: yours, then the disk's */
        Rectangle box = { b.x, b.y + 2, b.width, b.height - 56 };
        ui_rect(box, 9, TH.input);
        ui_rect_lines(box, 9, 1, TH.lineSoft);
        float ly = box.y + 12;
        for (int pass = 0; pass < 2 && ly < box.y + box.height - 24; pass++) {
            const char *a = pass == 0 ? f->text : f->diskText, *other = pass == 0 ? f->diskText : f->text;
            ui_text(F_MED, 13, pass == 0 ? "Yours (in Q-Continuum)" : "On disk", box.x + 14, ly, pass == 0 ? TH.accent : TH.info);
            ly += 22;
            int shown = 0;
            for (const char *p = a; p && *p && ly < box.y + box.height - 22; ) {
                const char *e = strchr(p, '\n');
                int n = e ? (int)(e - p) : (int)strlen(p);
                char line[300]; snprintf(line, sizeof line, "%.*s", n < 299 ? n : 299, p);
                char t[300]; pl_strlcpy(t, line, sizeof t); pl_trim_inplace(t);
                bool inOther = false;                        /* a line the other version has too (as a whole line) */
                for (const char *q = other; q && *q && t[0]; ) {
                    const char *qe = strchr(q, '\n');
                    int m = qe ? (int)(qe - q) : (int)strlen(q);
                    if (m == n && !strncmp(q, p, (size_t)n)) { inOther = true; break; }
                    q = qe ? qe + 1 : NULL;
                }
                if (t[0] && !inOther) { ui_text_clip(F_MONO, 12.5f, line, box.x + 24, ly, box.width - 40, TH.text); ly += 18; shown++; }
                p = e ? e + 1 : NULL;
            }
            if (!shown) { ui_text(F_REG, 12.5f, "(only layout or comments differ)", box.x + 24, ly, TH.text3); ly += 18; }
            ly += 10;
        }
        float by = b.y + b.height - 38;
        if (ui_button(ui_id("m-cf-cancel"), (Rectangle){ b.x, by, 110, 38 }, NULL, "Decide later", BTN_GHOST)) close = true;
        float w1 = ui_button_width(ICON_SAVE, "Keep mine"), w2 = ui_button_width(ICON_REFRESH_CW, "Take the disk version");
        if (ui_button(ui_id("m-cf-mine"), (Rectangle){ b.x + b.width - w1, by, w1, 38 }, ICON_SAVE, "Keep mine", BTN_PRIMARY)) {
            fd_resolve(f, true);
            if (fd_save(f)) ui_toast(TOAST_OK, "Saved your version of %s", rel); else ui_toast(TOAST_ERR, "Could not write %s", rel);
            close = true;
        }
        if (ui_button(ui_id("m-cf-disk"), (Rectangle){ b.x + b.width - w1 - 10 - w2, by, w2, 38 }, ICON_REFRESH_CW, "Take the disk version", BTN_SECONDARY)) {
            fd_resolve(f, false);
            ui_toast(TOAST_INFO, "%s: using the version on disk", rel);
            close = true;
        }
        if (close) A.lastDiskCheck = 0;                      /* another conflicted file comes up next */
        break;
    }
    case MD_ADD_ENTRY: {
        char sub[300]; pl_fmt_ui(sub, sizeof sub, "Inside %s%s", A.mPath[0] ? A.mPath : "the top level", A.mPath[0] ? "" : " of the file");
        b = ui_modal_begin("Add entry", sub, 560, 320, &close);
        ui_text(F_MED, 13, "Keyword", b.x, b.y, TH.text2);
        ui_input(ui_id("m-add-key"), (Rectangle){ b.x, b.y + 22, b.width, 36 }, A.mText2, sizeof A.mText2, "e.g. value", INPUT_MONO | INPUT_LIVE);
        ui_checkbox(ui_id("m-add-dict"), (Rectangle){ b.x, b.y + 72, b.width, 24 }, &A.mFlag, "Sub-dictionary");
        if (!A.mFlag) {
            ui_text(F_MED, 13, "Value", b.x, b.y + 108, TH.text2);
            ui_input(ui_id("m-add-val"), (Rectangle){ b.x, b.y + 130, b.width, 36 }, A.mText, sizeof A.mText, "e.g. uniform 0", INPUT_MONO | INPUT_LIVE);
        }
        if (A.mErr[0]) ui_text(F_REG, 13, A.mErr, b.x, b.y + 176, TH.err);
        if (ui_button(ui_id("m-add-cancel"), (Rectangle){ b.x + b.width - 230, b.y + b.height - 38, 110, 38 }, NULL, "Cancel", BTN_GHOST)) close = true;
        if (ui_button(ui_id("m-add-go"), (Rectangle){ b.x + b.width - 110, b.y + b.height - 38, 110, 38 }, ICON_PLUS, "Add", BTN_PRIMARY)) {
            FDFile *f = modal_file();
            pl_trim_inplace(A.mText2); pl_trim_inplace(A.mText);
            if (!f) snprintf(A.mErr, sizeof A.mErr, "The file is no longer open.");
            else if (!A.mText2[0] || strchr(A.mText2, ' ') || strchr(A.mText2, '/')) snprintf(A.mErr, sizeof A.mErr, "Use a single keyword without spaces or '/'.");
            else if (!A.mFlag && !A.mText[0]) snprintf(A.mErr, sizeof A.mErr, "Enter a value.");
            else {
                char path[1400];
                bool fits = A.mPath[0] ? pl_fmt(path, sizeof path, "%s/%s", A.mPath, A.mText2) : pl_fmt(path, sizeof path, "%s", A.mText2);
                if (!fits) snprintf(A.mErr, sizeof A.mErr, "That keyword path is too long.");
                else if (fd_find(f, path)) pl_fmt_ui(A.mErr, sizeof A.mErr, "%s already exists here.", A.mText2);
                else {
                    bool ok = A.mFlag ? fd_ensure_dict(f, path) : fd_put(f, path, A.mText);
                    if (ok) { ui_toast(TOAST_OK, "Added %s (not saved yet)", A.mText2); A.modal = MD_NONE; A.mFlag = false; }
                    else snprintf(A.mErr, sizeof A.mErr, "Could not add the entry.");
                }
            }
        }
        break;
    }
    case MD_SHORTCUTS:
        actions_sheet(&close);
        break;
    case MD_PALETTE:
        palette_draw(&close);
        break;
    case MD_REVIEW:
        review_modal(&close);
        break;
    case MD_WHATS_NEW: {
        static const char *const NEWS[][2] = {
            { "A Q for every theme", "Each theme has its own Q mark, and the window and desktop icon change with the theme." },
            { "Star Citizen theme", "A mobiGlas look: runs are contracts in Stanton, the Run page a quantum drive, a failed run a 30k, "
              "a finished one pays out. All themes are now in one place: Settings > Themes." },
            { "Eight new themes", "Settings > Themes: Severance, Alien (a real-looking CRT), Pip-Boy, NERV, Portal, Dark Souls, "
              "Factorio and Windows 95, each with its own names, sounds, Run-page instrument and moments (YOU DIED, PATTERN BLUE, cake...)." },
            { "Coupled runs are cases", "A preCICE folder opens like a case: Overview, Coupling setup, Participants, Run. Couple two cases "
              "makes a conjugate-heat-transfer coupling from two of yours, with the nuances checked first." },
            { "Patch inspector", "Click a patch name on Boundaries: every field on it in one place, U and p first. Wall / Inlet / Outlet set "
              "every field at once (one Ctrl+Z undoes it)." },
            { "Captain's log", "Every run is recorded inside the case (History): how it ended and why, the residuals, the setup change since "
              "the run before. Export as Markdown for a lab notebook." },
            { "Plume on a surface", "New case: an under-expanded jet impinging on a plate (rhoCentralFoam), set from the nozzle exit state, "
              "with the numbers worked out. SENSOR SWEEP now checks the Knudsen number of compressible cases." },
            { "Away mission", "Run page: pack a case for aerofly (Slurm, v2412), checked with v2412 here first, with the scp/ssh commands." },
            { "Fixes from a bug hunt", "22 problems found by an independent tester fixed, among them typed values that could break a dictionary, "
              "patch groups and #include on Boundaries, and Run asking before a run that would fail." },
            { "JARVIS", "A new theme: Stark's heads-up display. Arc-reactor cyan on near-black, HUD corner brackets, a reactor "
              "behind the page, holographic sounds, and its own names (the command bar is JARVIS, Run is Initiate)." },
            { "Each program in its own folder", "New DEME, Chrono, CalculiX, code_aster, code_saturne, MOOSE, Elmer and SPARTA projects "
              "are made in ~/deme, ~/chrono, ~/calculix, ~/simvia/aster ... instead of the OpenFOAM workspace. Settings > Workspace "
              "changes each one." },
            { "Every starter runs", "Each program's starters were run with the real program. Chrono has one now: a pendulum, its "
              "period checked against theory. Editing a driver in src/ rebuilds it on the next run." },
            { "Examples that run on their own", "SPARTA and MOOSE examples copied from New case bring the files they borrow from "
              "neighbouring examples and make the ones their README generates. MOOSE examples that need their own compiled app are "
              "no longer offered." },
            { "SPARTA packages", "Before a SPARTA run, every compute, dump and fix style is checked against your build: a style from a "
              "package it lacks (FFT, VTK, Python) says so, with the make line. The Environment panel lists SPARTA and every other program." },
            { "DEME iso view", "The particle viewer has Iso beside Side, Front and Top: the bed from the front corner, above, in its box." },
            { "Mesh > View", "Every OpenFOAM mesh on the Mesh page: the cross section (3-D meshes cut across x, y or z with a slider, "
              "coloured by any field) and the boundary in 3-D, patches you can hide." },
            { "Copy from the logs", "Copy log, or drag over lines and Ctrl+C. Ask Claude and Terminal open one working tab." },
            { "Every program, one workflow", "DEME, Chrono, CalculiX, code_aster, code_saturne, MOOSE, Elmer and SPARTA projects are "
              "cases now, beside OpenFOAM's: on the Cases page (a filter per program), with Overview, Setup, Run, Results, Files and "
              "History. New case has starters and the installed examples for each." },
            { "Setup without the input file", "Setup lists the values in a program's input (end time, time step, mesh size, materials, "
              "loads), Basic or All, and writes a change back in place: every other line and comment stays as it was." },
            { "Runs you can watch", "Run checks first (the program, the files the input names, a build), then shows progress from "
              "the program's own output, its residual-like values in the chart and the log. DEME and Chrono build when needed." },
            { "Results", "ParaView for fields, cgx for CalculiX, SALOME for MED, CSV tables as charts, and the particle viewer for "
              "DEME frames and now SPARTA's particle dumps." },
            { "Review before saving", "Save, Ctrl+S and Run show what changes in each file, entry by entry and as a diff, before "
              "anything is written. Settings > Runs turns it off." },
            { "Glass", "The light and dark themes are frosted glass now, in the style of iOS, with a new Q mark." },
            { "Names that fit the theme", "The pre-run check, the divergence alarm, the command bar and the review are called "
              "Sensor sweep, Red alert, Computer and Make it so on the bridge; Morning field check, Storm warning, Journal and "
              "Ship it on the farm; plain names in the glass themes." },
            { "DEME without the C++", "New DEME projects start from a settle box, rotating drum, hopper or impact crater. Their "
              "settings are a form (gravity and material presets, size ranges, a time-step check against the Rayleigh step with a fix), "
              "and a change needs no rebuild. Runs show progress, results and GPU load, and the particles play back in the viewer." },
            { "Studies, Basic and Advanced", "Basic lists this case's usual settings in words (time step, viscosity, the inlet "
              "velocity, the turbulence model, mesh refinement with cell counts) and fills the values for you: x½ x1 x2 or a range. "
              "Advanced takes any keyword, picked from a list, and can pair two settings value by value." },
            { "Big residuals", "Expand on the residuals chart (or Ctrl+Shift+L) gives it the whole page; Esc puts it back." },
            { "Stardew Valley and Pokémon themes", "Two game-menu looks, each with its own sounds: Settings > Appearance." },
            { "Your own sounds", "Sound files in ~/.config/q-continuum/sounds (or sounds/stardew, sounds/pokemon) replace the built-in "
              "cues: click.wav, done.mp3, alert.ogg and so on." },
            { "SENSOR SWEEP", "Before you run, the case is read for the mistakes that stop runs: a patch without a condition, a missing "
              "scheme or solver, the pressure in the wrong units, a split that doesn't add up, nothing written before the end, output "
              "bigger than the disk. Each says where, with a Fix button; a problem makes Run ask first." },
            { "Errors in plain words", "When OpenFOAM stops, the error card says what went wrong with your numbers, opens the file at "
              "the line, and offers a fix: a misspelt patch renamed, the closest valid type, a usual scheme, the right units." },
            { "RED ALERT", "A run that is diverging (residuals up a thousandfold, Courant blowing up, the time step collapsing) raises "
              "an alert with Stop and write, Halve the time step, or Continue." },
            { "Computer (Ctrl+K)", "One search box for commands, pages, cases, patches, fields and every keyword. Type deltaT = 0.001 "
              "to set it." },
            { "Coupled runs say what's missing", "Before running, each participant is checked: commands on the PATH, Python modules, files run.sh "
              "expects, a mesh. The fix is a button: Download meshes, or Use <folder> for one that has what it needs." },
            { "Log viewer", "Search the log (Enter for the next match), jump to the first error, fold finished time steps. "
              "The Commands tab lists every command the app ran, with copy buttons." },
            { "Undo and redo", "Ctrl+Z and Ctrl+Y for every dictionary edit, with buttons beside Save." },
            { "Units and meaning", "Boundaries shows each field's unit from its dimensions, marks fields the turbulence model doesn't "
              "read as not used, and says what a field is, its typical values and what the tutorials start it at." },
            { "Keyboard shortcuts", "F1 lists every command and its shortcut: Ctrl+R run, Ctrl+M mesh, Ctrl+P ParaView, Ctrl+1..8 "
              "the case pages." },
            { "Status at a glance", "The page buttons say No mesh, N missing, Running, Failed. Empty pages offer the next step." },
            { "Picks up where you left off", "The last case, page and region open at start. A file changed outside the app is "
              "named when it reloads." },
            { "Sensible process counts", "Small meshes run in serial: the default keeps at least 10 000 cells per process." },
            { "LCARS sounds", "Key beeps, a chime when a run completes, a klaxon when it fails: Settings > Appearance." },
        };
        int nn = (int)(sizeof NEWS / sizeof NEWS[0]);
        b = ui_modal_begin("What's new", "Since the last update of Q-Continuum.", 720, 660, &close);
        static UiScroll ws;
        float top = ui_scroll_begin(&ws, (Rectangle){ b.x, b.y, b.width, b.height - 50 });
        float y = top + 2;
        for (int i = 0; i < nn; i++) {
            ui_icon(ICON_SPARKLES, b.x + 9, y + 10, 14, TH.accent);
            /* the features' names in this theme */
            const char *tt = NEWS[i][0]; char tb[96];
            if (!strcmp(tt, "SENSOR SWEEP")) tt = term(T_SWEEP);
            else if (!strcmp(tt, "RED ALERT")) tt = term(T_ALERT);
            else if (!strcmp(tt, "Computer (Ctrl+K)")) { pl_fmt_ui(tb, sizeof tb, "%s (Ctrl+K)", term(T_PALETTE)); tt = tb; }
            ui_text(F_SEMI, 14, tt, b.x + 26, y, TH.text);
            y += 22;
            y += ui_text_wrap(F_REG, 13, NEWS[i][1], (Rectangle){ b.x + 26, y, b.width - 40, 200 }, 19, TH.text2, true) + 12;
        }
        ui_scroll_end(&ws, y - top);
        if (ui_button(ui_id("m-wn-ok"), (Rectangle){ b.x + b.width - 120, b.y + b.height - 38, 120, 38 }, NULL, "Got it", BTN_PRIMARY)) close = true;
        if (close && A.set.whatsNewSeen < WHATS_NEW_REV) { A.set.whatsNewSeen = WHATS_NEW_REV; settings_save(&A.set); }
        break;
    }
    default:
        A.modal = MD_NONE;
        return;
    }
    ui_modal_end();
    if (close) A.modal = MD_NONE;
}
