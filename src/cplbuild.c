/* cplbuild.c -- a CHT coupled case from two OpenFOAM cases (see cplbuild.h) */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cplbuild.h"
#include "foamcase.h"
#include "foamdict.h"
#include "physics.h"

/* the OpenFOAM adapter's CHT solver types */
static const char *solver_type(const char *s)
{
    static const char *const comp[] = { "buoyantPimpleFoam", "buoyantSimpleFoam", "rhoPimpleFoam", "rhoSimpleFoam", "chtMultiRegionFoam",
                                        "buoyantFoam", NULL };
    static const char *const basic[] = { "laplacianFoam", "solidFoam", NULL };
    static const char *const incomp[] = { "buoyantBoussinesqPimpleFoam", "buoyantBoussinesqSimpleFoam", "pimpleFoam", "simpleFoam",
                                          "scalarTransportFoam", NULL };
    for (int i = 0; comp[i]; i++) if (!strcmp(s, comp[i])) return "compressible";
    for (int i = 0; basic[i]; i++) if (!strcmp(s, basic[i])) return "basic";
    for (int i = 0; incomp[i]; i++) if (!strcmp(s, incomp[i])) return "incompressible";
    return "";
}

void cplb_inspect(CplPart *p)
{
    p->ok = false; p->solver[0] = p->solverType[0] = p->patches[0] = 0;
    p->deltaT = p->endTime = 0; p->adjustable = false; p->dims = 3;
    if (!case_is_case(p->src)) return;
    Case *c = case_open(p->src);
    if (!c) return;
    p->ok = true;
    pl_strlcpy(p->solver, c->s.app, sizeof p->solver);
    pl_strlcpy(p->solverType, solver_type(c->s.app), sizeof p->solverType);
    p->deltaT = c->s.deltaT; p->endTime = c->s.endTime;
    FDFile *cd = case_dict(c, "system/controlDict");
    char v[32] = "";
    if (cd && fd_get(cd, "adjustTimeStep", v, sizeof v)) p->adjustable = !strcmp(v, "yes") || !strcmp(v, "on") || !strcmp(v, "true");
    int k = 0;
    for (int i = 0; i < c->nPatches; i++) {
        if (!strcmp(c->patches[i].type, "empty")) p->dims = 2;
        if (c->patches[i].pattern) continue;
        k += snprintf(p->patches + k, sizeof p->patches - (size_t)k, "%s%s", k ? " " : "", c->patches[i].name);
        if (k >= (int)sizeof p->patches - 1) break;
    }
    if (p->dims == 3) {                                       /* before meshing: unlisted faces go to an empty defaultFaces */
        FDFile *bm = case_dict(c, "system/blockMeshDict");
        BlockMeshInfo bi; memset(&bi, 0, sizeof bi);
        if (bm) blockmesh_info(bm, &bi);
        if (bi.ok && bi.oneCellDir >= 0) p->dims = 2;
    }
    case_close(c);
}

static bool has_word(const char *list, const char *w)
{
    size_t n = strlen(w);
    for (const char *q = list; (q = strstr(q, w)); q += n)
        if ((q == list || q[-1] == ' ') && (q[n] == 0 || q[n] == ' ')) return true;
    return false;
}

int cplb_check(CplBuild *b, CplCheck *out, int max)
{
    int n = 0;
    #define ADD(s, ...) do { if (n < max) { out[n].sev = (s); snprintf(out[n].text, sizeof out[n].text, __VA_ARGS__); n++; } } while (0)
    for (int i = 0; i < 2; i++) {
        CplPart *p = &b->p[i];
        const char *role = i == 0 ? "writes Temperature, reads Heat-Flux" : "writes Heat-Flux, reads Temperature";
        if (!p->name[0]) ADD(2, "Participant %d needs a name.", i + 1);
        if (!p->ok) { ADD(2, "%s: %s isn't an OpenFOAM case.", p->name, p->src[0] ? p->src : "no case chosen"); continue; }
        if (!p->solverType[0]) ADD(2, "%s: the preCICE OpenFOAM adapter's CHT module doesn't support %s (it does buoyant*/rho* solvers, "
                                   "laplacianFoam, and the incompressible ones with T).", p->name, p->solver);
        else ADD(0, "%s (%s, %s): adapter solver type %s; %s on patch %s.", p->name, p->solver, pl_basename(p->src), p->solverType, role,
                 p->patch[0] ? p->patch : "(none)");
        if (!p->patch[0]) ADD(2, "%s: pick the interface patch.", p->name);
        else if (!has_word(p->patches, p->patch)) ADD(2, "%s: %s has no patch %s (it has %s).", p->name, pl_basename(p->src), p->patch, p->patches);
        if (!strcmp(p->solverType, "basic") && !(p->k > 0))
            ADD(2, "%s: %s knows only the diffusivity DT; the adapter needs the conductivity k in W/(m·K) to turn the temperature "
                "gradient into a heat flux (q = -k ∂T/∂n).", p->name, p->solver);
        if (!strcmp(p->solverType, "incompressible"))
            ADD(1, "%s: an incompressible solver: the adapter reads rho, Cp, Pr and Prt from preciceDict's CHT dictionary; check them there "
                "after the build.", p->name);
        if (p->adjustable) ADD(1, "%s has adjustTimeStep yes: the adapter shortens steps to hit each time window, but a step longer than the "
                               "window is cut every time.", p->name);
    }
    if (!strcmp(b->p[0].name, b->p[1].name) && b->p[0].name[0]) ADD(2, "The two participants need different names.");
    if (b->p[0].ok && b->p[1].ok) {
        if (b->p[0].dims != b->p[1].dims)
            ADD(2, "%s is %d-D and %s is %d-D: preCICE meshes of one coupling have one dimension.", b->p[0].name, b->p[0].dims, b->p[1].name, b->p[1].dims);
        double d0 = b->p[0].deltaT, d1 = b->p[1].deltaT;
        if (!(b->window > 0)) b->window = fmax(d0, d1);
        for (int i = 0; i < 2; i++) {
            double dt = b->p[i].deltaT, r = b->window / dt;
            if (dt > 0 && fabs(r - round(r)) > 1e-6)
                ADD(2, "%s's deltaT %g doesn't divide the time window %g: preCICE needs whole steps per window (window / deltaT = %.4g).",
                    b->p[i].name, dt, b->window, r);
            else if (dt > b->window * (1 + 1e-9)) ADD(2, "%s's deltaT %g is longer than the time window %g.", b->p[i].name, dt, b->window);
        }
        double e = fmin(b->p[0].endTime, b->p[1].endTime);
        if (!(b->maxTime > 0)) b->maxTime = e;
        if (fabs(b->p[0].endTime - b->p[1].endTime) > 1e-12)
            ADD(1, "The end times differ (%g and %g): the coupling stops at max-time %g, and the adapter ends both there.", b->p[0].endTime,
                b->p[1].endTime, b->maxTime);
        if (b->window > 0 && b->maxTime > 0)
            ADD(0, "Coupling: serial-implicit, %s first; time windows of %g s up to %g s (%.0f windows), at most %d iterations each, "
                "Aitken on the heat flux; %d-D meshes.", b->p[0].name, b->window, b->maxTime, b->maxTime / b->window,
                b->maxIter > 0 ? b->maxIter : 30, b->p[0].dims);
    }
    return n;
    #undef ADD
}

static bool write_text(const char *dir, const char *rel, const char *text)
{
    char p[PL_PATH];
    return pl_fmt(p, sizeof p, "%s/%s", dir, rel) && pl_write_file(p, text, strlen(text));
}

bool cplb_write(const CplBuild *b0, const char *outDir, char *err, int cap)
{
    CplBuild b = *b0;
    CplCheck ck[24];
    int n = cplb_check(&b, ck, 24);
    for (int i = 0; i < n; i++) if (ck[i].sev == 2) { snprintf(err, (size_t)cap, "%s", ck[i].text); return false; }
    if (pl_exists(outDir)) { snprintf(err, (size_t)cap, "%s already exists", outDir); return false; }
    if (!pl_mkdirs(outDir)) { snprintf(err, (size_t)cap, "Can't create %s", outDir); return false; }
    const char *data[2] = { "Temperature", "Heat-Flux" };     /* p[i] writes data[i] */
    for (int i = 0; i < 2; i++) {
        const CplPart *p = &b.p[i];
        char low[64]; int k = 0;
        for (const char *q = p->name; *q && k < 50; q++) low[k++] = (char)(*q >= 'A' && *q <= 'Z' ? *q + 32 : *q);
        low[k] = 0;
        char dir[PL_PATH], qs[PL_PATH + 8], qd[PL_PATH + 8], cmd[PL_PATH * 3], absOut[PL_PATH], absSrc[PL_PATH];
        if (!realpath(outDir, absOut)) pl_strlcpy(absOut, outDir, sizeof absOut);   /* the copy changes folder: absolute paths */
        if (!realpath(p->src, absSrc)) pl_strlcpy(absSrc, p->src, sizeof absSrc);
        if (!pl_fmt(dir, sizeof dir, "%s/%s-openfoam", absOut, low)) { snprintf(err, (size_t)cap, "Path too long"); return false; }
        pl_shell_quote(qs, sizeof qs, absSrc); pl_shell_quote(qd, sizeof qd, dir);
        /* the setup only: no results, processor folders, logs or the app's own records */
        Str ex; str_init(&ex);                                /* result times are left out, not copied and removed */
        NameList l = { 0 };
        pl_list_dir(p->src, &l, true, false);
        for (int t = 0; t < l.n; t++) if (pl_is_number(l.names[t]) && atof(l.names[t]) > 0) str_catf(&ex, "--exclude='./%s' ", l.names[t]);
        names_free(&l);
        pl_fmt(cmd, sizeof cmd, "mkdir -p %s && cd %s && tar --exclude='./processor*' --exclude='./log.*' --exclude=./postProcessing "
               "--exclude='./.qc*' --exclude=./.qcontinuum --exclude='./*.foam' %s -cf - . | (cd %s && tar xf -)", qd, qs, ex.s ? ex.s : "", qd);
        str_free(&ex);
        if (pl_run_wait(cmd, NULL, 0) != 0) { snprintf(err, (size_t)cap, "Copying %s failed", p->src); return false; }
        /* preciceDict */
        char pd[2048];
        int m = snprintf(pd, sizeof pd,
            "FoamFile\n{\n    version     2.0;\n    format      ascii;\n    class       dictionary;\n    location    \"system\";\n"
            "    object      preciceDict;\n}\n\n// Written by Q-Continuum's coupled-case builder: %s %s %s across patch %s\n\n"
            "preciceConfig \"../precice-config.xml\";\n\nparticipant %s;\n\nmodules (CHT);\n\ninterfaces\n{\n  Interface1\n  {\n"
            "    mesh              %s-Mesh;\n    patches           (%s);\n    readData\n    (\n      %s\n    );\n    writeData\n    (\n      %s\n    );\n  };\n};\n",
            p->name, i == 0 ? "writes" : "writes", data[i], p->patch, p->name, p->name, p->patch, data[1 - i], data[i]);
        if (!strcmp(p->solverType, "basic"))
            snprintf(pd + m, sizeof pd - (size_t)m, "\nCHT\n{\n   k   [ 1  1 -3 -1 0 0 0 ] %g;\n   solverType \"basic\";\n};\n", p->k);
        else if (!strcmp(p->solverType, "incompressible"))
            snprintf(pd + m, sizeof pd - (size_t)m, "\nCHT\n{\n   rho [ 1 -3 0 0 0 0 0 ] 1.2;\n   Cp  [ 0 2 -2 -1 0 0 0 ] 1005;\n"
                          "   Pr  [ 0 0 0 0 0 0 0 ] 0.71;\n   Prt [ 0 0 0 0 0 0 0 ] 0.85;\n   solverType \"incompressible\";\n};\n");
        else snprintf(pd + m, sizeof pd - (size_t)m, "\nCHT\n{\n   solverType \"compressible\";\n};\n");
        if (!write_text(dir, "system/preciceDict", pd)) { snprintf(err, (size_t)cap, "Can't write %s/system/preciceDict", dir); return false; }
        /* the adapter, and the interface's temperature: fixedValue where Temperature is read, fixedGradient where the heat flux is */
        char cdp[PL_PATH]; pl_fmt(cdp, sizeof cdp, "%s/system/controlDict", dir);
        FDFile *cd = fd_load(cdp);
        bool ok = cd && fd_ensure_dict(cd, "functions") && fd_put(cd, "functions/preCICE_Adapter/type", "preciceAdapterFunctionObject") &&
                  fd_put(cd, "functions/preCICE_Adapter/libs", "(\"libpreciceAdapterFunctionObject.so\")") && fd_save(cd);
        if (cd) fd_free(cd);
        if (!ok) { snprintf(err, (size_t)cap, "Can't add the adapter to %s", cdp); return false; }
        for (int z = 0; z < 2; z++) {
            char tp[PL_PATH]; pl_fmt(tp, sizeof tp, "%s/%s/T", dir, z ? "0.orig" : "0");
            if (!pl_is_file(tp)) continue;
            FDFile *t = fd_load(tp);
            if (!t) continue;
            char path[300], iv[160] = "uniform 300";
            FDEntry *ie = fd_find(t, "internalField");
            if (ie) { char raw[160]; fd_entry_value(t, ie, raw, sizeof raw); if (!strncmp(raw, "uniform", 7)) pl_strlcpy(iv, raw, sizeof iv); }
            snprintf(path, sizeof path, "boundaryField/%s", p->patch);
            FDEntry *e = fd_find(t, path);
            if (e) fd_replace_span(t, e->valStart, e->valEnd, "\n    ");
            else fd_ensure_dict(t, path);
            snprintf(path, sizeof path, "boundaryField/%s/type", p->patch);
            fd_put(t, path, i == 1 ? "fixedValue" : "fixedGradient");
            snprintf(path, sizeof path, "boundaryField/%s/%s", p->patch, i == 1 ? "value" : "gradient");
            fd_put(t, path, i == 1 ? iv : "uniform 0");
            fd_save(t); fd_free(t);
        }
        static const char *RUN =
            "#!/bin/bash\n# Written by Q-Continuum's coupled-case builder: meshes when needed, then the solver (serial, or -parallel)\nset -e\n"
            "if [ ! -f constant/polyMesh/owner ] && [ -f system/blockMeshDict ]; then blockMesh > log.blockMesh 2>&1; fi\n"
            "if [ -d 0.orig ] && [ ! -d 0 ]; then cp -r 0.orig 0; fi\n"
            "solver=$(foamDictionary -entry application -value system/controlDict)\n"
            "if [ \"${1:-}\" = -parallel ]; then\n"
            "    np=$(foamDictionary -entry numberOfSubdomains -value system/decomposeParDict)\n"
            "    decomposePar -force > log.decomposePar 2>&1\n"
            "    OVS=; mpirun --version 2>&1 | grep -q 'Open MPI' && OVS=--oversubscribe\n"
            "    mpirun $OVS -np \"$np\" \"$solver\" -parallel\n    reconstructPar > log.reconstructPar 2>&1\n"
            "else\n    \"$solver\"\nfi\n";
        char rp[PL_PATH]; pl_fmt(rp, sizeof rp, "%s/run.sh", dir);
        if (!pl_write_file(rp, RUN, strlen(RUN)) || chmod(rp, 0755) != 0) { snprintf(err, (size_t)cap, "Can't write %s", rp); return false; }
    }
    /* precice-config.xml, the tutorials' layout */
    const CplPart *a = &b.p[0], *c = &b.p[1];
    char x[6000];
    snprintf(x, sizeof x,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<!-- Written by Q-Continuum's coupled-case builder: conjugate heat transfer between %s (%s) and %s (%s) -->\n"
        "<precice-configuration>\n  <log>\n    <sink filter=\"%%Severity%% > debug and %%Rank%% = 0\" format=\"---[precice] %%ColorizedSeverity%% %%Message%%\" enabled=\"true\" />\n  </log>\n\n"
        "  <data:scalar name=\"Temperature\" />\n  <data:scalar name=\"Heat-Flux\" />\n\n"
        "  <mesh name=\"%s-Mesh\" dimensions=\"%d\">\n    <use-data name=\"Temperature\" />\n    <use-data name=\"Heat-Flux\" />\n  </mesh>\n\n"
        "  <mesh name=\"%s-Mesh\" dimensions=\"%d\">\n    <use-data name=\"Temperature\" />\n    <use-data name=\"Heat-Flux\" />\n  </mesh>\n\n"
        "  <participant name=\"%s\">\n    <provide-mesh name=\"%s-Mesh\" />\n    <receive-mesh name=\"%s-Mesh\" from=\"%s\" />\n"
        "    <read-data name=\"Heat-Flux\" mesh=\"%s-Mesh\" />\n    <write-data name=\"Temperature\" mesh=\"%s-Mesh\" />\n"
        "    <mapping:nearest-neighbor direction=\"read\" from=\"%s-Mesh\" to=\"%s-Mesh\" constraint=\"consistent\" />\n  </participant>\n\n"
        "  <participant name=\"%s\">\n    <receive-mesh name=\"%s-Mesh\" from=\"%s\" />\n    <provide-mesh name=\"%s-Mesh\" />\n"
        "    <mapping:nearest-neighbor direction=\"read\" from=\"%s-Mesh\" to=\"%s-Mesh\" constraint=\"consistent\" />\n"
        "    <read-data name=\"Temperature\" mesh=\"%s-Mesh\" />\n    <write-data name=\"Heat-Flux\" mesh=\"%s-Mesh\" />\n  </participant>\n\n"
        "  <m2n:sockets acceptor=\"%s\" connector=\"%s\" exchange-directory=\"..\" />\n\n"
        "  <coupling-scheme:serial-implicit>\n    <time-window-size value=\"%g\" />\n    <max-time value=\"%g\" />\n    <max-iterations value=\"%d\" />\n"
        "    <participants first=\"%s\" second=\"%s\" />\n"
        "    <exchange data=\"Temperature\" mesh=\"%s-Mesh\" from=\"%s\" to=\"%s\" />\n"
        "    <exchange data=\"Heat-Flux\" mesh=\"%s-Mesh\" from=\"%s\" to=\"%s\" />\n"
        "    <relative-convergence-measure limit=\"1.0e-5\" data=\"Temperature\" mesh=\"%s-Mesh\" />\n"
        "    <acceleration:aitken>\n      <data mesh=\"%s-Mesh\" name=\"Heat-Flux\" />\n      <initial-relaxation value=\"0.5\" />\n    </acceleration:aitken>\n"
        "  </coupling-scheme:serial-implicit>\n</precice-configuration>\n",
        a->name, a->solver, c->name, c->solver,
        a->name, a->dims, c->name, c->dims,
        a->name, a->name, c->name, c->name, a->name, a->name, c->name, a->name,
        c->name, a->name, a->name, c->name, a->name, c->name, c->name, c->name,
        a->name, c->name,
        b.window, b.maxTime, b.maxIter > 0 ? b.maxIter : 30, a->name, c->name,
        a->name, a->name, c->name, c->name, c->name, a->name, a->name, c->name);
    if (!write_text(outDir, "precice-config.xml", x)) { snprintf(err, (size_t)cap, "Can't write precice-config.xml"); return false; }
    return true;
}
