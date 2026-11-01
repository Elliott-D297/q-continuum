/* simsetup.c -- the simulation set up from choices (see simsetup.h).
 *
 * Incompressible flow: simpleFoam (steady, SIMPLE) or pimpleFoam (transient, PIMPLE), laminar or RANS (k-omega SST,
 * k-epsilon, realizable k-epsilon, Spalart-Allmaras). Pressures are typed in pascals and written kinematic (p / rho),
 * as the incompressible solvers want them. Each boundary's conditions follow from its role, in the way of the tutorials:
 * inlets fix the velocity (or the flow rate, or the total pressure) and the turbulence from an intensity and a mixing
 * length; outlets fix the pressure and let the rest flow out (inletOutlet); walls take wall functions.
 *
 * A compressible gas (plan step 4): rhoSimpleFoam or rhoPimpleFoam, hePsiThermo (perfectGas, Sutherland's viscosity, a
 * constant cp from gamma), the pressure absolute (the operating pressure plus the gauge ones typed in), the energy as the
 * internal energy e. Inlets by speed, Mach number, flow rate or total pressure and temperature (the isentropic forms);
 * outlets by static pressure, or non-reflecting (waveTransmissive) on a transient run.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "simsetup.h"
#include "foamcase.h"
#include "foamdict.h"

const char *const FLOW_NAME[FLOW__N] = { "Steady", "Transient" };
const char *const FLUID_NAME[FLUID__N] = { "Air (20 °C)", "Water (20 °C)", "Custom" };
const char *const ROLE_NAME[ROLE__N] = { "Wall", "Opening", "Inlet", "Outlet", "Symmetry" };
const char *const INLET_NAME[IN__N] = { "Speed (normal)", "Velocity vector", "Volume flow rate", "Mass flow rate", "Total pressure", "Mach number" };
const char *const WALL_NAME[WL__N] = { "No slip", "Slip", "Moving" };
const char *const ENERGY_NAME[EN__N] = { "Off", "Temperature", "With buoyancy" };
const char *const HEAT_NAME[HT__N] = { "Adiabatic", "Fixed temperature", "Heat flux", "Heat rate", "Convection" };

static const char *const FLOW_WORD[FLOW__N] = { "steady", "transient" };
static const char *const ALG_WORD[ALG__N] = { "PISO", "PIMPLE", "largeStepPIMPLE" };
static const char *const TS_WORD[TS__N] = { "auto", "Euler", "backward", "CrankNicolson" };
static const char *const FLUID_WORD[FLUID__N] = { "air", "water", "custom" };
static const char *const ROLE_WORD[ROLE__N] = { "wall", "opening", "inlet", "outlet", "symmetry" };
static const char *const INLET_WORD[IN__N] = { "speed", "vector", "flowRate", "massFlowRate", "totalPressure", "mach" };
static const char *const WALLT_WORD[WT__N] = { "auto", "wallFunctions", "resolved" };
static const char *const TI_WORD[TI__N] = { "length", "viscosityRatio", "hydraulicDiameter" };
static const char *const COMP_WORD[COMP__N] = { "incompressible", "idealGas" };
static const char *const GAS_WORD[GAS__N] = { "air", "nitrogen", "CO2", "custom" };
static const char *const WALL_WORD[WL__N] = { "noSlip", "slip", "moving" };
static const char *const ENERGY_WORD[EN__N] = { "off", "temperature", "buoyancy" };
static const char *const HEAT_WORD[HT__N] = { "adiabatic", "fixedTemperature", "heatFlux", "heatRate", "convection" };
static const char *const TMPL_WORD[TMPL__N] = { "externalAero", "internalFlow", "vortexShedding", "resolvedTurbulence", "heatTransfer",
                                                "naturalConvection", "highSpeedGas", "freeSurface", "rotatingMachinery", "blank" };

const char *const SIM_FILES[] = { "0/U", "0/p", "0/k", "0/omega", "0/epsilon", "0/nut", "0/nuTilda", "0/ReThetat", "0/gammaInt", "0/T", "0/p_rgh", "0/alphat",
                                  "constant/transportProperties", "constant/turbulenceProperties", "constant/thermophysicalProperties", "constant/g", "system/controlDict", "system/fvSchemes", "system/fvSolution",
                                  "system/fvOptions", NULL };

#define MARK "// Written by Q-Continuum's simulation setup (Physics > Setup) from simulation.ini. Hand edits are kept unless the setup\n" \
             "// changes the same setting (it says so when it does); its own last version is in .qc-setup/.\n"
#define MARK_START "// Written by Q-Continuum's simulation setup"   /* this and older versions of the line */

/* ============================================================================ defaults */
void sim_fluid(SimSetup *s, int fluid)
{
    s->fluid = fluid;
    /* at 20 °C; air's expansion is an ideal gas's, 1/T */
    if (fluid == FLUID_AIR) { s->rho = 1.204; s->nu = 1.516e-5; s->cp = 1005; s->kappa = 0.0257; s->beta = 1 / 293.15; }
    else if (fluid == FLUID_WATER) { s->rho = 998.2; s->nu = 1.004e-6; s->cp = 4182; s->kappa = 0.598; s->beta = 2.07e-4; }
    else {
        if (!(s->rho > 0) || !(s->nu > 0)) { s->rho = 1000; s->nu = 1e-6; }
        if (!(s->cp > 0) || !(s->kappa > 0)) { s->cp = 4182; s->kappa = 0.598; s->beta = 2.07e-4; }
    }
}

void sim_defaults(SimSetup *s, double size)
{
    memset(s, 0, sizeof *s);
    s->flow = FLOW_STEADY;
    s->turb = TURB_KOMEGASST;
    sim_fluid(s, FLUID_AIR);
    s->intensity = 5;
    s->length = size > 0 ? 0.07 * size : 0.01;          /* 7 % of the size: the usual guess for a duct's mixing length */
    s->iterations = 1000; s->writeEvery = 100;
    s->resStop = true; s->resP = 1e-4; s->resRest = 1e-5;
    s->endTime = 1; s->deltaT = 1e-3; s->maxCo = 1; s->writeTime = 0.1; s->adjustDt = true;
    s->algo = ALG_PIMPLE; s->nOuter = 1; s->nCorr = 2; s->timeScheme = TS_AUTO;
    s->forceAuto = true;
    s->tmpl = -1;
    s->Tref = s->T0 = 293.15;
    s->g[2] = -9.81; s->gAuto = true;
    s->nuAuto = true; s->nuLength = size > 0 ? size : 1;
    s->comp = COMP_INCOMP; s->pOp = SIM_POP;
    s->wallTreat = WT_AUTO; s->avgStart = -1;
    sim_gas_preset(s, GAS_AIR);
    s->fr = (SimForceRef){ { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0, 0 }, 0, size > 0 ? size : 1, size > 0 ? size * size : 1 };
}

void sim_boundary_defaults(const SimSetup *s, SimBoundary *b)
{
    b->inlet = IN_SPEED; b->speed = 1; b->vec[0] = 1; b->vec[1] = b->vec[2] = 0;
    b->flow = 0.001; b->mdot = 0.001 * s->rho; b->p = 0;
    b->intensity = s->intensity; b->length = s->length;
    b->wall = WL_NOSLIP; b->wallU[0] = b->wallU[1] = b->wallU[2] = 0;
    b->force = -1;
    b->heat = HT_ADIABATIC; b->T = b->Ta = s->T0 > 0 ? s->T0 : 293.15;
    b->q = 1000; b->Q = 100; b->h = 10;
    b->mach = 0.5; b->nonRefl = false;
    b->turbSpec = TI_LENGTH; b->ratio = 10; b->dh = 0;
}

void sim_ini_path(const char *casePath, char *out, size_t cap) { pl_join(out, cap, casePath, "simulation.ini"); }

/* ============================================================================ simulation.ini */
static int word_index(const char *const *w, int n, const char *v, int def)
{
    for (int i = 0; i < n; i++) if (!strcasecmp(w[i], v)) return i;
    return def;
}

static void vec_parse(const char *v, double out[3]) { sscanf(v, "%lf %lf %lf", &out[0], &out[1], &out[2]); }

bool sim_load(const char *casePath, SimSetup *s, double size)
{
    char p[PL_PATH]; sim_ini_path(casePath, p, sizeof p);
    char *ini = pl_read_file(p, NULL);
    sim_defaults(s, size);
    if (!ini) return false;
    SimBoundary *b = NULL;
    for (char *line = strtok(ini, "\n"); line; line = strtok(NULL, "\n")) {
        pl_trim_inplace(line);
        if (!line[0] || line[0] == '#') continue;
        if (line[0] == '[') {
            b = NULL;
            char name[64] = "";
            if (sscanf(line, "[boundary %63[^]]]", name) == 1 && s->nB < SIM_MAX_B) {
                pl_trim_inplace(name);
                b = &s->b[s->nB++];
                memset(b, 0, sizeof *b);
                pl_strlcpy(b->name, name, sizeof b->name);
                pl_strlcpy(b->meshType, "patch", sizeof b->meshType);
                sim_boundary_defaults(s, b);
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        pl_trim_inplace(k); pl_trim_inplace(v);
        if (!b) {
            if (!strcmp(k, "template")) s->tmpl = !strcmp(v, "none") ? -2 : word_index(TMPL_WORD, TMPL__N, v, TMPL_BLANK);
            else if (!strcmp(k, "flow")) s->flow = word_index(FLOW_WORD, FLOW__N, v, FLOW_STEADY);
            else if (!strcmp(k, "turbulence")) {
                s->turb = TURB_KOMEGASST;
                for (int t = 0; t < TURB__N; t++) if (!strcasecmp(sim_model_word(t), v)) s->turb = t;
            }
            else if (!strcmp(k, "algorithm")) s->algo = word_index(ALG_WORD, ALG__N, v, ALG_PIMPLE);
            else if (!strcmp(k, "outer_correctors")) s->nOuter = atoi(v);
            else if (!strcmp(k, "correctors")) s->nCorr = atoi(v);
            else if (!strcmp(k, "time_scheme")) s->timeScheme = word_index(TS_WORD, TS__N, v, TS_AUTO);
            else if (!strcmp(k, "fluid")) s->fluid = word_index(FLUID_WORD, FLUID__N, v, FLUID_CUSTOM);
            else if (!strcmp(k, "density")) s->rho = atof(v);
            else if (!strcmp(k, "viscosity")) s->nu = atof(v);
            else if (!strcmp(k, "initial_velocity")) vec_parse(v, s->U0);
            else if (!strcmp(k, "initial_pressure")) s->p0 = atof(v);
            else if (!strcmp(k, "turbulence_intensity")) s->intensity = atof(v);
            else if (!strcmp(k, "turbulence_length")) s->length = atof(v);
            else if (!strcmp(k, "iterations")) s->iterations = atof(v);
            else if (!strcmp(k, "write_every")) s->writeEvery = atof(v);
            else if (!strcmp(k, "stop_on_residuals")) s->resStop = atoi(v) != 0;
            else if (!strcmp(k, "residual_p")) s->resP = atof(v);
            else if (!strcmp(k, "residual_rest")) s->resRest = atof(v);
            else if (!strcmp(k, "end_time")) s->endTime = atof(v);
            else if (!strcmp(k, "time_step")) s->deltaT = atof(v);
            else if (!strcmp(k, "max_courant")) s->maxCo = atof(v);
            else if (!strcmp(k, "write_time")) s->writeTime = atof(v);
            else if (!strcmp(k, "adjust_time_step")) s->adjustDt = atoi(v) != 0;
            else if (!strcmp(k, "forces")) s->forces = atoi(v) != 0;
            else if (!strcmp(k, "force_reference_auto")) s->forceAuto = atoi(v) != 0;
            else if (!strcmp(k, "drag_direction")) vec_parse(v, s->fr.drag);
            else if (!strcmp(k, "lift_direction")) vec_parse(v, s->fr.lift);
            else if (!strcmp(k, "moment_centre")) vec_parse(v, s->fr.CofR);
            else if (!strcmp(k, "reference_speed")) s->fr.Uinf = atof(v);
            else if (!strcmp(k, "reference_length")) s->fr.lRef = atof(v);
            else if (!strcmp(k, "reference_area")) s->fr.aRef = atof(v);
            else if (!strcmp(k, "energy")) s->energy = word_index(ENERGY_WORD, EN__N, v, EN_OFF);
            else if (!strcmp(k, "specific_heat")) s->cp = atof(v);
            else if (!strcmp(k, "conductivity")) s->kappa = atof(v);
            else if (!strcmp(k, "expansion")) s->beta = atof(v);
            else if (!strcmp(k, "reference_temperature")) s->Tref = atof(v);
            else if (!strcmp(k, "initial_temperature")) s->T0 = atof(v);
            else if (!strcmp(k, "gravity")) vec_parse(v, s->g);
            else if (!strcmp(k, "gravity_auto")) s->gAuto = atoi(v) != 0;
            else if (!strcmp(k, "heat_report")) s->heatReport = atoi(v) != 0;
            else if (!strcmp(k, "nusselt_length_auto")) s->nuAuto = atoi(v) != 0;
            else if (!strcmp(k, "nusselt_length")) s->nuLength = atof(v);
            else if (!strcmp(k, "nusselt_delta_T")) s->nuDT = atof(v);
            else if (!strcmp(k, "compressibility")) s->comp = word_index(COMP_WORD, COMP__N, v, COMP_INCOMP);
            else if (!strcmp(k, "gas")) s->gas = word_index(GAS_WORD, GAS__N, v, GAS_CUSTOM);
            else if (!strcmp(k, "molar_mass")) s->molW = atof(v);
            else if (!strcmp(k, "heat_capacity_ratio")) s->gamma = atof(v);
            else if (!strcmp(k, "sutherland_As")) s->muAs = atof(v);
            else if (!strcmp(k, "sutherland_Ts")) s->muTs = atof(v);
            else if (!strcmp(k, "operating_pressure")) s->pOp = atof(v);
            else if (!strcmp(k, "wall_treatment")) s->wallTreat = word_index(WALLT_WORD, WT__N, v, WT_AUTO);
            else if (!strcmp(k, "averaging_start")) s->avgStart = !strcmp(v, "auto") ? -1 : atof(v);
            continue;
        }
        if (!strcmp(k, "role")) b->role = word_index(ROLE_WORD, ROLE__N, v, ROLE_WALL);
        else if (!strcmp(k, "mesh_type")) pl_strlcpy(b->meshType, v, sizeof b->meshType);
        else if (!strcmp(k, "inlet")) b->inlet = word_index(INLET_WORD, IN__N, v, IN_SPEED);
        else if (!strcmp(k, "speed")) b->speed = atof(v);
        else if (!strcmp(k, "velocity")) vec_parse(v, b->vec);
        else if (!strcmp(k, "flow_rate")) b->flow = atof(v);
        else if (!strcmp(k, "mass_flow_rate")) b->mdot = atof(v);
        else if (!strcmp(k, "pressure")) b->p = atof(v);
        else if (!strcmp(k, "turbulence_intensity")) b->intensity = atof(v);
        else if (!strcmp(k, "turbulence_length")) b->length = atof(v);
        else if (!strcmp(k, "wall")) b->wall = word_index(WALL_WORD, WL__N, v, WL_NOSLIP);
        else if (!strcmp(k, "wall_velocity")) vec_parse(v, b->wallU);
        else if (!strcmp(k, "forces")) b->force = !strcmp(v, "default") ? -1 : atoi(v) != 0;
        else if (!strcmp(k, "heat")) b->heat = word_index(HEAT_WORD, HT__N, v, HT_ADIABATIC);
        else if (!strcmp(k, "temperature")) b->T = atof(v);
        else if (!strcmp(k, "heat_flux")) b->q = atof(v);
        else if (!strcmp(k, "heat_rate")) b->Q = atof(v);
        else if (!strcmp(k, "heat_transfer_coefficient")) b->h = atof(v);
        else if (!strcmp(k, "ambient_temperature")) b->Ta = atof(v);
        else if (!strcmp(k, "mach")) b->mach = atof(v);
        else if (!strcmp(k, "non_reflecting")) b->nonRefl = atoi(v) != 0;
        else if (!strcmp(k, "far_field")) b->farField = atoi(v) != 0;
        else if (!strcmp(k, "turbulence_by")) b->turbSpec = word_index(TI_WORD, TI__N, v, TI_LENGTH);
        else if (!strcmp(k, "viscosity_ratio")) b->ratio = atof(v);
        else if (!strcmp(k, "hydraulic_diameter")) b->dh = atof(v);
    }
    free(ini);
    /* no template line: a setup from before the templates, its own choices. "none": saved before one was chosen (a
       boundary set on the mesher tab), so Setup still asks */
    s->tmpl = s->tmpl == -2 ? -1 : s->tmpl < 0 ? TMPL_BLANK : s->tmpl;
    if (s->fluid != FLUID_CUSTOM) sim_fluid(s, s->fluid);
    if (s->gas != GAS_CUSTOM) sim_gas_preset(s, s->gas);
    sim_settle(s);
    return true;
}

bool sim_save(const char *casePath, const SimSetup *s)
{
    Str o; str_init(&o);
    str_cat(&o, "# Q-Continuum simulation setup (Physics > Setup). Write setup turns it into 0/, constant/ and system/.\n"
                "# SI units; pressures in Pa (gauge), written to OpenFOAM divided by the density.\n[simulation]\n");
    str_catf(&o, "template = %s\n", s->tmpl >= 0 && s->tmpl < TMPL__N ? TMPL_WORD[s->tmpl] : "none");
    str_catf(&o, "flow = %s\nturbulence = %s\nfluid = %s\ndensity = %.8g\nviscosity = %.8g\n", FLOW_WORD[s->flow], sim_model_word(s->turb),
             FLUID_WORD[s->fluid], s->rho, s->nu);
    str_catf(&o, "initial_velocity = %.8g %.8g %.8g\ninitial_pressure = %.8g\nturbulence_intensity = %.8g\nturbulence_length = %.8g\n",
             s->U0[0], s->U0[1], s->U0[2], s->p0, s->intensity, s->length);
    str_catf(&o, "iterations = %.8g\nwrite_every = %.8g\nend_time = %.8g\ntime_step = %.8g\nmax_courant = %.8g\nwrite_time = %.8g\nadjust_time_step = %d\n",
             s->iterations, s->writeEvery, s->endTime, s->deltaT, s->maxCo, s->writeTime, s->adjustDt);
    str_catf(&o, "algorithm = %s\nouter_correctors = %d\ncorrectors = %d\ntime_scheme = %s\n", ALG_WORD[s->algo], s->nOuter, s->nCorr, TS_WORD[s->timeScheme]);
    str_catf(&o, "stop_on_residuals = %d\nresidual_p = %.8g\nresidual_rest = %.8g\n", s->resStop, s->resP, s->resRest);
    str_catf(&o, "forces = %d\nforce_reference_auto = %d\ndrag_direction = %.8g %.8g %.8g\nlift_direction = %.8g %.8g %.8g\nmoment_centre = %.8g %.8g %.8g\n"
                 "reference_speed = %.8g\nreference_length = %.8g\nreference_area = %.8g\n", s->forces, s->forceAuto, s->fr.drag[0], s->fr.drag[1], s->fr.drag[2],
             s->fr.lift[0], s->fr.lift[1], s->fr.lift[2], s->fr.CofR[0], s->fr.CofR[1], s->fr.CofR[2], s->fr.Uinf, s->fr.lRef, s->fr.aRef);
    str_catf(&o, "energy = %s\nspecific_heat = %.8g\nconductivity = %.8g\nexpansion = %.8g\nreference_temperature = %.8g\ninitial_temperature = %.8g\n"
                 "gravity = %.8g %.8g %.8g\ngravity_auto = %d\nheat_report = %d\nnusselt_length_auto = %d\nnusselt_length = %.8g\nnusselt_delta_T = %.8g\n",
             ENERGY_WORD[s->energy], s->cp, s->kappa, s->beta, s->Tref, s->T0, s->g[0], s->g[1], s->g[2], s->gAuto, s->heatReport, s->nuAuto, s->nuLength, s->nuDT);
    str_catf(&o, "compressibility = %s\ngas = %s\nmolar_mass = %.8g\nheat_capacity_ratio = %.8g\nsutherland_As = %.8g\nsutherland_Ts = %.8g\noperating_pressure = %.8g\n",
             COMP_WORD[s->comp], GAS_WORD[s->gas], s->molW, s->gamma, s->muAs, s->muTs, s->pOp);
    str_catf(&o, "wall_treatment = %s\n", WALLT_WORD[s->wallTreat]);
    if (s->avgStart < 0) str_cat(&o, "averaging_start = auto\n");
    else str_catf(&o, "averaging_start = %.8g\n", s->avgStart);
    for (int i = 0; i < s->nB; i++) {
        const SimBoundary *b = &s->b[i];
        str_catf(&o, "\n[boundary %s]\nrole = %s\nmesh_type = %s\n", b->name, ROLE_WORD[b->role], b->meshType[0] ? b->meshType : "patch");
        str_catf(&o, "inlet = %s\nspeed = %.8g\nvelocity = %.8g %.8g %.8g\nflow_rate = %.8g\nmass_flow_rate = %.8g\npressure = %.8g\n",
                 INLET_WORD[b->inlet], b->speed, b->vec[0], b->vec[1], b->vec[2], b->flow, b->mdot, b->p);
        str_catf(&o, "turbulence_intensity = %.8g\nturbulence_length = %.8g\nwall = %s\nwall_velocity = %.8g %.8g %.8g\n",
                 b->intensity, b->length, WALL_WORD[b->wall], b->wallU[0], b->wallU[1], b->wallU[2]);
        if (b->force < 0) str_cat(&o, "forces = default\n");
        else str_catf(&o, "forces = %d\n", b->force);
        str_catf(&o, "heat = %s\ntemperature = %.8g\nheat_flux = %.8g\nheat_rate = %.8g\nheat_transfer_coefficient = %.8g\nambient_temperature = %.8g\n",
                 HEAT_WORD[b->heat], b->T, b->q, b->Q, b->h, b->Ta);
        str_catf(&o, "mach = %.8g\nnon_reflecting = %d\nfar_field = %d\n", b->mach, b->nonRefl, b->farField);
        str_catf(&o, "turbulence_by = %s\nviscosity_ratio = %.8g\nhydraulic_diameter = %.8g\n", TI_WORD[b->turbSpec], b->ratio, b->dh);
    }
    char p[PL_PATH]; sim_ini_path(casePath, p, sizeof p);
    char *old = pl_read_file(p, NULL);                  /* unchanged: left alone, so its time says when the setup changed */
    bool same = old && !strcmp(old, o.s);
    free(old);
    bool ok = same || pl_write_file(p, o.s, o.len);
    str_free(&o);
    return ok;
}

/* ============================================================================ the boundaries */
SimBoundary *sim_find(SimSetup *s, const char *name)
{
    for (int i = 0; i < s->nB; i++) if (!strcmp(s->b[i].name, name)) return &s->b[i];
    return NULL;
}

void sim_set_boundaries(SimSetup *s, const SimPatch *p, int n)
{
    SimBoundary *old = malloc(sizeof(SimBoundary) * SIM_MAX_B);
    if (!old) return;
    int nOld = s->nB;
    memcpy(old, s->b, sizeof(SimBoundary) * (size_t)nOld);
    /* the names and types may point into s->b itself (the boundaries kept as they were): read from the copy */
    if (n > SIM_MAX_B) n = SIM_MAX_B;
    char (*nm)[64] = malloc(64 * (size_t)(n ? n : 1)), (*mt)[24] = malloc(24 * (size_t)(n ? n : 1));
    if (!nm || !mt) { free(old); free(nm); free(mt); return; }
    for (int i = 0; i < n; i++) {
        pl_strlcpy(nm[i], p[i].name ? p[i].name : "", 64);
        pl_strlcpy(mt[i], p[i].meshType && p[i].meshType[0] ? p[i].meshType : "patch", 24);
    }
    s->nB = 0;
    for (int i = 0; i < n && s->nB < SIM_MAX_B; i++) {
        bool dup = false;
        for (int k = 0; k < s->nB; k++) if (!strcmp(s->b[k].name, nm[i])) dup = true;
        if (dup) continue;
        SimBoundary *b = &s->b[s->nB++];
        const SimBoundary *was = NULL;
        for (int k = 0; k < nOld; k++) if (!strcmp(old[k].name, nm[i])) was = &old[k];
        if (was) *b = *was;
        else {
            memset(b, 0, sizeof *b);
            pl_strlcpy(b->name, nm[i], sizeof b->name);
            b->role = p[i].role;
            sim_boundary_defaults(s, b);
        }
        if (p[i].fromStl || !was) b->role = p[i].role;
        b->fromStl = p[i].fromStl;
        pl_strlcpy(b->meshType, mt[i], sizeof b->meshType);
        b->area = p[i].area;
    }
    free(old); free(nm); free(mt);
}

void sim_rename(SimSetup *s, const char *from, const char *to)
{
    SimBoundary *b = sim_find(s, from);
    if (b && !sim_find(s, to)) pl_strlcpy(b->name, to, sizeof b->name);
}

bool sim_far_field(const SimSetup *s, const SimBoundary *b)
{
    return b->role == ROLE_OPENING && b->farField && !sim_constraint(b->meshType) && (sim_is_gas(s) || s->energy == EN_OFF);
}

bool sim_constraint(const char *t)
{
    static const char *const C[] = { "empty", "wedge", "cyclic", "cyclicAMI", "cyclicACMI", "cyclicSlip", "symmetry", "symmetryPlane",
                                     "processor", "processorCyclic", NULL };
    for (int i = 0; C[i]; i++) if (!strcmp(t, C[i])) return true;
    return false;
}

static double vlen(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

/* a gas's: the lowest pressure the flow leaves to (outlets, openings), Pa absolute; the operating pressure with none */
static double exit_pressure(const SimSetup *s)
{
    double p = 0;
    for (int i = 0; i < s->nB; i++) {
        const SimBoundary *b = &s->b[i];
        if (sim_constraint(b->meshType) || (b->role != ROLE_OUTLET && b->role != ROLE_OPENING)) continue;
        double a = sim_pabs(s, b->p);
        if (!(p > 0) || a < p) p = a;
    }
    return p > 0 ? p : sim_pabs(s, 0);
}

/* the isentropic Mach number a total pressure p0 expands to at p (both absolute): (p0/p)^((g-1)/g) = 1 + (g-1)/2 M² */
static double isentropic_mach(double g, double p0, double p)
{
    if (!(g > 1) || !(p > 0) || !(p0 > p)) return 0;
    return sqrt(2 / (g - 1) * (pow(p0 / p, (g - 1) / g) - 1));
}

/* an inlet's speed, m/s, and its Mach number (0: the fluid's speed of sound is unknown) */
static double inlet_speed(const SimSetup *s, const SimBoundary *b, double *mach)
{
    bool gas = sim_is_gas(s);
    double u = 0, T = gas || s->energy != EN_OFF ? b->T : s->T0;
    double rho = gas && b->T > 0 ? sim_pabs(s, 0) / (SIM_R / s->molW * b->T) : s->rho;   /* the gas's density coming in */
    if (sim_far_field(s, b)) u = vlen(b->vec);
    else if (b->inlet == IN_SPEED) u = fabs(b->speed);
    else if (b->inlet == IN_VECTOR) u = vlen(b->vec);
    else if (b->inlet == IN_FLOW) u = b->area > 0 ? b->flow / b->area : 0;
    else if (b->inlet == IN_MASS) u = b->area > 0 && rho > 0 ? b->mdot / (rho * b->area) : 0;
    else if (b->inlet == IN_MACH && gas) { if (mach) *mach = b->mach; return b->mach * sim_sound_speed(s, b->T); }
    else if (b->inlet == IN_PRESSURE && gas) {
        /* its T is the total temperature: the static one is lower by 1 + (g-1)/2 M² */
        double M = isentropic_mach(s->gamma, sim_pabs(s, b->p), exit_pressure(s));
        if (mach) *mach = M;
        return M * sim_sound_speed(s, b->T / (1 + 0.5 * (s->gamma - 1) * M * M));
    }
    else if (b->inlet == IN_PRESSURE) u = s->rho > 0 ? sqrt(2 * fabs(b->p) / s->rho) : 0;
    if (mach) { double a = sim_sound_speed(s, T); *mach = a > 0 ? u / a : 0; }
    return u;
}

double sim_ref_speed(const SimSetup *s)
{
    double U = vlen(s->U0);
    for (int i = 0; i < s->nB; i++) {
        const SimBoundary *b = &s->b[i];
        if ((b->role != ROLE_INLET && !sim_far_field(s, b)) || sim_constraint(b->meshType)) continue;
        double u = inlet_speed(s, b, NULL);
        if (u > U) U = u;
    }
    return U > 0 ? U : 1;
}

double sim_mach(const SimSetup *s)
{
    double a0 = sim_sound_speed(s, s->T0), M = a0 > 0 ? vlen(s->U0) / a0 : 0;
    for (int i = 0; i < s->nB; i++) {
        const SimBoundary *b = &s->b[i];
        if ((b->role != ROLE_INLET && !sim_far_field(s, b)) || sim_constraint(b->meshType)) continue;
        double m = 0;
        inlet_speed(s, b, &m);
        if (m > M) M = m;
    }
    return M;
}

/* by default the forces are the part's: its walls from the STL (with no STL, every wall but a wind tunnel's ground) */
bool sim_force_on(const SimSetup *s, const SimBoundary *b)
{
    if (sim_constraint(b->meshType) || b->role != ROLE_WALL) return false;
    if (b->force >= 0) return b->force == 1;
    bool stl = false;
    for (int i = 0; i < s->nB; i++) if (s->b[i].fromStl) stl = true;
    return stl ? b->fromStl : strcmp(b->name, "ground") != 0;
}

double sim_force_speed(const SimSetup *s) { return s->fr.Uinf > 0 ? s->fr.Uinf : sim_ref_speed(s); }

/* k = 3/2 (U I)², epsilon = Cmu^3/4 k^3/2 / L, omega = k^1/2 / (Cmu^1/4 L), nuTilda = (3/2)^1/2 U I L */
void sim_turbulence(const SimSetup *s, double U, double I, double L, double *k, double *omega, double *eps, double *nuTilda)
{
    const double Cmu = 0.09;
    double i = I / 100, l = L > 0 ? L : 1e-3;
    double kk = 1.5 * (U * i) * (U * i);
    if (kk < 1e-10) kk = 1e-10;
    if (k) *k = kk;
    if (omega) *omega = sqrt(kk) / (pow(Cmu, 0.25) * l);
    if (eps) *eps = pow(Cmu, 0.75) * pow(kk, 1.5) / l;
    if (nuTilda) { double nt = sqrt(1.5) * U * i * l; *nuTilda = nt > 3 * s->nu ? nt : 3 * s->nu; }
}

/* sim_fields and sim_app: simmodels.c */

static int area[64];

int sim_check_area(int i) { return i >= 0 && i < 64 ? area[i] : SA_BOUNDARIES; }

int sim_check(const SimSetup *s, char *out, int cap)
{
#define AREA(a) do { if (n < 64) area[n] = (a); n++; } while (0)
    int n = 0, nIn = 0, nOut = 0, nOpen = 0, nPin = 0;
    out[0] = 0;
    for (int i = 0; i < s->nB; i++) {
        const SimBoundary *b = &s->b[i];
        if (sim_constraint(b->meshType)) continue;
        if (b->role == ROLE_INLET) { nIn++; if (b->inlet == IN_PRESSURE) nPin++; }
        if (b->role == ROLE_OUTLET) nOut++;
        if (b->role == ROLE_OPENING) nOpen++;
        if (b->role == ROLE_INLET && b->inlet == IN_SPEED && b->speed <= 0) {
            AREA(SA_BOUNDARIES); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "%s: an inlet speed above 0 m/s brings flow in.\n", b->name);
        }
        if (b->role == ROLE_INLET && (b->inlet == IN_FLOW || b->inlet == IN_MASS) && (b->inlet == IN_FLOW ? b->flow : b->mdot) <= 0) {
            AREA(SA_BOUNDARIES); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "%s: a flow rate above 0 brings flow in.\n", b->name);
        }
        /* the STL's: only meshing again makes it a wall (with layers); another mesh patch is made one by sim_write's caller */
        if (b->fromStl && b->role == ROLE_WALL && b->wall == WL_NOSLIP && strcmp(b->meshType, "wall") && s->turb != TURB_LAMINAR) {
            AREA(SA_BOUNDARIES); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "%s is not a wall in the mesh (its type is %s), so it gets no wall functions "
                           "(make it a Wall in the STL mesher and mesh again).\n", b->name, b->meshType);
        }
    }
    if (s->forces) {
        int nf = 0;
        for (int i = 0; i < s->nB; i++) if (sim_force_on(s, &s->b[i])) nf++;
        if (!nf) { AREA(SA_REPORTS); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "Forces: no wall is picked to measure the lift and drag on.\n"); }
        if (!(s->fr.lRef > 0) || !(s->fr.aRef > 0)) { AREA(SA_REPORTS); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "Forces: the reference length and area must be above 0.\n"); }
    }
    if (nIn && !nOut && !nOpen && nPin < nIn) {
        AREA(SA_BOUNDARIES); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "Flow comes in but has no way out: make a boundary an Outlet or an Opening.\n");
    }
    if (!nIn && !nOpen && vlen(s->U0) == 0 && !(s->nB && nOut)) {
        bool moving = false;
        for (int i = 0; i < s->nB; i++) if (s->b[i].role == ROLE_WALL && s->b[i].wall == WL_MOVING) moving = true;
        /* with buoyancy a heated or cooled wall drives the flow (natural convection) */
        if (s->energy == EN_BUOYANT)
            for (int i = 0; i < s->nB; i++) if (s->b[i].role == ROLE_WALL && s->b[i].heat != HT_ADIABATIC) moving = true;
        if (!moving) { AREA(SA_BOUNDARIES); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "Nothing moves the fluid: no inlet, opening, moving wall, heated wall or starting velocity.\n"); }
    }
    if (!(s->rho > 0) || !(s->nu > 0)) { AREA(SA_MATERIALS); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "The density and viscosity must be above 0.\n"); }
    /* heat */
    if (s->energy != EN_OFF) {
        if (!(s->cp > 0) || !(s->kappa > 0)) { AREA(SA_MATERIALS); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "The specific heat and conductivity must be above 0.\n"); }
        if (s->energy == EN_BUOYANT && !(sim_beta(s) > 0)) { AREA(SA_MATERIALS); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "Buoyancy needs a thermal expansion above 0.\n"); }
        if (s->energy == EN_BUOYANT && !(sim_g_mag(s) > 0)) { AREA(SA_MODELS); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "Buoyancy with no gravity: give gravity a direction, or pick Temperature only.\n"); }
        if (!(s->T0 > 0) || !(s->Tref > 0)) { AREA(SA_START); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "Temperatures are in kelvin and must be above 0 K.\n"); }
        bool heats = false;
        for (int i = 0; i < s->nB; i++) {
            const SimBoundary *b = &s->b[i];
            if (sim_constraint(b->meshType)) continue;
            bool wall = b->role == ROLE_WALL;
            double t = b->role == ROLE_INLET || (wall && b->heat == HT_FIXED) ? b->T : wall && b->heat == HT_CONVECT ? b->Ta : 1;
            if (!(t > 0)) { AREA(SA_BOUNDARIES); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "%s: temperatures are in kelvin and must be above 0 K.\n", b->name); }
            if (wall && b->heat == HT_CONVECT && !(b->h > 0)) { AREA(SA_BOUNDARIES); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "%s: the heat transfer coefficient must be above 0.\n", b->name); }
            if ((b->role == ROLE_INLET && b->T != s->T0) || (wall && ((b->heat == HT_FIXED && b->T != s->T0) || (b->heat == HT_FLUX && b->q != 0)
                || (b->heat == HT_POWER && b->Q != 0) || (b->heat == HT_CONVECT && b->Ta != s->T0)))) heats = true;
        }
        if (!heats && s->nB && !sim_is_gas(s)) { AREA(SA_BOUNDARIES); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "Nothing heats or cools the fluid: give a wall a temperature or a heat flux, or an inlet a temperature other than the start's.\n"); }
    }
    /* compressibility: how fast against the speed of sound */
    bool gas = sim_is_gas(s);
    double Ma = sim_mach(s);
    if (!gas && Ma > 0.3 && s->fluid != FLUID_WATER) {
        AREA(SA_MODELS); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "Ma is about %.2g: the density changes by %.0f %% along the flow, so treat the gas "
                       "as compressible (Compressibility: Ideal gas).\n", Ma, 100 * (1 - pow(1 + 0.2 * Ma * Ma, -2.5)));
    }
    if (gas) {
        if (!(s->pOp > 0)) { AREA(SA_MATERIALS); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "The operating pressure is absolute and must be above 0 Pa.\n"); }
        if (!(s->molW > 0) || !(s->gamma > 1) || !(s->muAs > 0)) {
            AREA(SA_MATERIALS); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "The gas needs a molar mass above 0, a ratio of specific heats above 1 and a viscosity.\n");
        }
        double pin = 0;
        bool fixedIn = false;
        for (int i = 0; i < s->nB; i++) {
            const SimBoundary *b = &s->b[i];
            if (sim_constraint(b->meshType)) continue;
            if (b->role != ROLE_WALL && b->role != ROLE_SYMMETRY && !(sim_pabs(s, b->p) > 0)) {
                AREA(SA_BOUNDARIES); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "%s: %.6g Pa gauge is below vacuum (the operating pressure is %.6g Pa).\n", b->name, b->p, s->pOp);
            }
            if (b->role == ROLE_INLET && b->inlet == IN_MACH && !(b->mach > 0)) {
                AREA(SA_BOUNDARIES); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "%s: a Mach number above 0 brings flow in.\n", b->name);
            }
            if (b->role == ROLE_INLET && b->inlet == IN_PRESSURE && sim_pabs(s, b->p) > pin) pin = sim_pabs(s, b->p);
            if (b->role == ROLE_INLET && b->inlet != IN_PRESSURE) fixedIn = true;
        }
        if (pin > 0 && !fixedIn && pin <= exit_pressure(s)) {
            AREA(SA_BOUNDARIES); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "The inlet's total pressure is not above the outlet's (%.6g Pa absolute): nothing pushes the gas through.\n",
                           exit_pressure(s));
        }
        if (Ma > 1 && s->flow == FLOW_STEADY) {
            AREA(SA_MODELS); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "Ma is about %.2g: supersonic. A steady run can stall on the shocks; a Transient "
                           "run with a Courant limit of 0.5 gets through them more surely.\n", Ma);
        } else if (Ma > 1 && s->flow == FLOW_TRANSIENT && s->adjustDt && s->maxCo > 0.5) {
            AREA(SA_SOLVER); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "Ma is about %.2g: supersonic. Keep the Courant limit at 0.5 or under (it is %.3g) "
                           "so the shocks stay steady.\n", Ma, s->maxCo);
        }
    }
    /* the turbulence approach against the run and the mesh */
    int appr = sim_approach(s->turb);
    const char *why = sim_approach_why_not(s, appr);
    if (why) { AREA(SA_MODELS); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "%s.\n", why); }
    if ((appr == APPR_LES || appr == APPR_DES) && sim_two_d(s)) {
        AREA(SA_MODELS); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "%s follows eddies in 3-D: on a 2-D mesh the answer is not physical. "
                       "Use URANS here, or mesh in 3-D.\n", APPR_NAME[appr]);
    }
    if ((appr == APPR_LES || appr == APPR_DES) && s->flow == FLOW_TRANSIENT && s->algo != ALG_PISO && s->adjustDt && s->maxCo > 0.5) {
        AREA(SA_SOLVER); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "%s wants a Courant number of 0.5 or under, where the small cells at the walls are, "
                       "to follow its eddies; the limit is %.3g.\n", APPR_NAME[appr], s->maxCo);
    }
    /* the averages start after the start-up has washed out and before the end */
    if ((appr == APPR_LES || appr == APPR_DES) && s->flow == FLOW_TRANSIENT && sim_avg_start(s) >= s->endTime) {
        AREA(SA_SOLVER); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "The averaging starts at %.4g s, after the run ends (%.4g s): there will be no UMean.\n",
                       sim_avg_start(s), s->endTime);
    }
    /* a wall treatment the model can't do (kept in simulation.ini from another model): what it runs with instead */
    if (s->wallTreat != WT_AUTO && sim_wall_treatment(s) != s->wallTreat && s->turb != TURB_LAMINAR) {
        AREA(SA_MODELS); pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "%s: it runs with %s.\n", sim_wall_why_not(s, s->wallTreat),
                       sim_wall_treatment(s) == WT_RESOLVED ? "the wall resolved" : "wall functions");
    }
    size_t l = strlen(out);
    if (l && out[l - 1] == '\n') out[l - 1] = 0;
    return n;
#undef AREA
}

/* ============================================================================ writing */
static void num(char *o, size_t cap, double v) { snprintf(o, cap, "%.6g", v); }

/* the transition onset momentum-thickness Reynolds number for a free-stream turbulence intensity (%), Langtry and
   Menter's correlation: kOmegaSSTLM's ReThetat at an inlet */
static double re_theta(double Tu)
{
    if (Tu < 0.027) Tu = 0.027;
    return Tu <= 1.3 ? 1173.51 - 589.428 * Tu + 0.2196 / (Tu * Tu) : 331.5 * pow(Tu - 0.5658, -0.671);
}

/* the wall function for nut: low-Re for transition; Spalding's (right at any y+, so for resolved walls too) for the
   one-equation, LES and DES models and resolved walls; the log law's (nutkWallFunction) with wall functions */
static const char *nut_wall(const SimSetup *s)
{
    int a = sim_approach(s->turb);
    if (s->turb == TURB_SSTLM) return "nutLowReWallFunction";
    if (s->turb == TURB_SPALART || s->turb == TURB_SADDES || s->turb == TURB_SAIDDES || a == APPR_LES) return "nutUSpaldingWallFunction";
    if (sim_wall_treatment(s) == WT_RESOLVED) return "nutUSpaldingWallFunction";
    return "nutkWallFunction";
}

/* with heat on or as a gas, absolute pressures in Pa (sim_pabs: the gauge ones typed in, above SIM_PATM or pOp) */
#define pabs(g) sim_pabs(s, (g))

/* a gas's p and T (rhoSimpleFoam, rhoPimpleFoam): p is solved, absolute. A total-pressure inlet or an opening takes the
   isentropic forms (totalPressure and totalTemperature with psi and gamma: p0 = p (1 + (g-1)/2 M²)^(g/(g-1))); an
   outlet fixes the static pressure, or on a transient run lets waves out (waveTransmissive). Walls: T as with heat. */
static bool gas_entry(Str *o, const SimSetup *s, const SimBoundary *b, const char *f)
{
    char v[64];
    bool tr = s->flow == FLOW_TRANSIENT;
    if (!strcmp(f, "p")) {
        num(v, sizeof v, pabs(b->p));
        if (b->role == ROLE_WALL || b->role == ROLE_SYMMETRY) str_cat(o, "        type            zeroGradient;\n");
        else if (b->role == ROLE_OUTLET && b->nonRefl && tr)
            str_catf(o, "        type            waveTransmissive;\n        field           p;\n        psi             thermo:psi;\n        gamma           %.6g;\n"
                        "        fieldInf        %s;\n        lInf            %.6g;\n        value           uniform %s;\n", s->gamma, v, s->length > 0 ? s->length / 0.07 : 1, v);
        else if (b->role == ROLE_OUTLET) str_catf(o, "        type            fixedValue;\n        value           uniform %s;\n", v);
        else if (sim_far_field(s, b))
            str_catf(o, "        type            freestreamPressure;\n        freestreamValue uniform %s;\n        value           uniform %s;\n", v, v);
        else if (b->role == ROLE_OPENING || b->inlet == IN_PRESSURE)
            str_catf(o, "        type            totalPressure;\n        p0              uniform %s;\n        psi             thermo:psi;\n        gamma           %.6g;\n"
                        "        value           uniform %s;\n", v, s->gamma, v);
        else str_cat(o, "        type            zeroGradient;\n");
        return true;
    }
    if (!strcmp(f, "T") && ((b->role == ROLE_OPENING && !sim_far_field(s, b)) || (b->role == ROLE_INLET && b->inlet == IN_PRESSURE))) {
        /* the total temperature: the static one falls as the gas speeds up */
        num(v, sizeof v, b->T);
        str_catf(o, "        type            totalTemperature;\n        psi             thermo:psi;\n        gamma           %.6g;\n        T0              uniform %s;\n"
                    "        value           uniform %s;\n", s->gamma, v, v);
        return true;
    }
    return false;                                       /* the rest as with heat */
}

/* with heat on: p (worked out from p_rgh), p_rgh (what the buoyant solvers solve), T and alphat, the way of the
   heatTransfer tutorials: p_rgh is fixed where the pressure is (prghPressure, prghTotalPressure) and fixedFluxPressure
   elsewhere, so the hydrostatic part is right on walls and inlets */
static void heat_entry(Str *o, const SimSetup *s, const SimBoundary *b, const char *f)
{
    char v[64], w[64];
    bool wall = b->role == ROLE_WALL || b->role == ROLE_SYMMETRY;
    if (sim_is_gas(s) && gas_entry(o, s, b, f)) return;
    if (!strcmp(f, "p")) { num(v, sizeof v, pabs(s->p0)); str_catf(o, "        type            calculated;\n        value           uniform %s;\n", v); return; }
    if (!strcmp(f, "p_rgh")) {
        num(v, sizeof v, pabs(b->p)); num(w, sizeof w, pabs(s->p0));
        if (b->role == ROLE_OUTLET) str_catf(o, "        type            prghPressure;\n        p               uniform %s;\n        value           uniform %s;\n", v, v);
        else if (b->role == ROLE_OPENING || (b->role == ROLE_INLET && b->inlet == IN_PRESSURE))
            str_catf(o, "        type            prghTotalPressure;\n        p0              uniform %s;\n        value           uniform %s;\n", v, v);
        else str_catf(o, "        type            fixedFluxPressure;\n        value           uniform %s;\n", w);
        return;
    }
    if (!strcmp(f, "alphat")) {
        if (b->role == ROLE_WALL && b->wall != WL_SLIP && !strcmp(b->meshType, "wall"))
            str_cat(o, "        type            compressible::alphatWallFunction;\n        Prt             0.85;\n        value           uniform 0;\n");
        else str_cat(o, "        type            calculated;\n        value           uniform 0;\n");
        return;
    }
    /* T */
    if (b->role == ROLE_INLET) { num(v, sizeof v, b->T); str_catf(o, "        type            fixedValue;\n        value           uniform %s;\n", v); return; }
    if (!wall) { num(v, sizeof v, b->T); str_catf(o, "        type            inletOutlet;\n        inletValue      uniform %s;\n        value           uniform %s;\n", v, v); return; }
    if (b->role == ROLE_SYMMETRY || b->heat == HT_ADIABATIC) { str_cat(o, "        type            zeroGradient;\n"); return; }
    if (b->heat == HT_FIXED) { num(v, sizeof v, b->T); str_catf(o, "        type            fixedValue;\n        value           uniform %s;\n", v); return; }
    num(w, sizeof w, s->T0);
    str_cat(o, "        type            externalWallHeatFluxTemperature;\n");
    if (b->heat == HT_FLUX) str_catf(o, "        mode            flux;\n        q               uniform %.6g;\n", b->q);
    else if (b->heat == HT_POWER) str_catf(o, "        mode            power;\n        Q               %.6g;\n", b->Q);
    else str_catf(o, "        mode            coefficient;\n        h               uniform %.6g;\n        Ta              constant %.6g;\n", b->h, b->Ta);
    str_catf(o, "        kappaMethod     fluidThermo;\n        value           uniform %s;\n", w);
}

/* one field's entry for one boundary */
static void bc_entry(Str *o, const SimSetup *s, const SimBoundary *b, const char *f, double k0, double om0, double ep0, double nt0)
{
    char v[64];
    bool U = !strcmp(f, "U"), P = !strcmp(f, "p"), K = !strcmp(f, "k"), OM = !strcmp(f, "omega"), EP = !strcmp(f, "epsilon");
    bool NT = !strcmp(f, "nuTilda"), NUT = !strcmp(f, "nut"), RT = !strcmp(f, "ReThetat"), GI = !strcmp(f, "gammaInt");
    double amb = K ? k0 : OM ? om0 : EP ? ep0 : RT ? re_theta(s->intensity) : GI ? 1 : nt0;   /* the turbulence that comes back in */
    str_catf(o, "    %s\n    {\n", b->name);
    if (sim_constraint(b->meshType)) { str_catf(o, "        type            %s;\n    }\n", b->meshType); return; }
    if (s->energy != EN_OFF && (P || !strcmp(f, "p_rgh") || !strcmp(f, "T") || !strcmp(f, "alphat"))) {
        heat_entry(o, s, b, f);
        str_cat(o, "    }\n");
        return;
    }
    switch (b->role) {
    case ROLE_WALL:
        if (b->wall == WL_SLIP) {
            if (U) str_cat(o, "        type            slip;\n");
            else if (NUT) str_cat(o, "        type            calculated;\n        value           uniform 0;\n");
            else str_cat(o, "        type            zeroGradient;\n");
            break;
        }
        if (strcmp(b->meshType, "wall") && !U && !P) {                 /* wall functions only work on a wall patch */
            if (NUT) str_cat(o, "        type            calculated;\n        value           uniform 0;\n");
            else if (NT) str_cat(o, "        type            fixedValue;\n        value           uniform 0;\n");
            else str_cat(o, "        type            zeroGradient;\n");
            break;
        }
        if (U && b->wall == WL_MOVING) str_catf(o, "        type            movingWallVelocity;\n        value           uniform (%.6g %.6g %.6g);\n", b->wallU[0], b->wallU[1], b->wallU[2]);
        else if (U) str_cat(o, "        type            noSlip;\n");
        else if (P) str_cat(o, "        type            zeroGradient;\n");
        else if (K) { num(v, sizeof v, k0); str_catf(o, "        type            kqRWallFunction;\n        value           uniform %s;\n", v); }
        else if (OM) {
            /* resolved: the viscous and log values blended smoothly (Menter and Esch), right at y+ 1 and in between */
            num(v, sizeof v, om0);
            str_catf(o, "        type            omegaWallFunction;\n%s        value           uniform %s;\n",
                     sim_wall_treatment(s) == WT_RESOLVED ? "        blending        binomial;\n        n               2;\n" : "", v);
        }
        else if (EP) { num(v, sizeof v, ep0); str_catf(o, "        type            epsilonWallFunction;\n        value           uniform %s;\n", v); }
        else if (NT) str_cat(o, "        type            fixedValue;\n        value           uniform 0;\n");
        else if (NUT) str_catf(o, "        type            %s;\n        value           uniform 0;\n", nut_wall(s));
        else str_cat(o, "        type            zeroGradient;\n");      /* ReThetat, gammaInt */
        break;
    case ROLE_SYMMETRY:                                                 /* a symmetry plane on a plain patch: slip */
        if (U) str_cat(o, "        type            slip;\n");
        else if (NUT) str_cat(o, "        type            calculated;\n        value           uniform 0;\n");
        else str_cat(o, "        type            zeroGradient;\n");
        break;
    case ROLE_INLET: {
        double pb = b->p / s->rho;
        if (U) {
            if (b->inlet == IN_SPEED) str_catf(o, "        type            surfaceNormalFixedValue;\n        refValue        uniform %.6g;\n"
                                                  "        value           uniform (0 0 0);\n", -fabs(b->speed));
            else if (b->inlet == IN_VECTOR) str_catf(o, "        type            fixedValue;\n        value           uniform (%.6g %.6g %.6g);\n", b->vec[0], b->vec[1], b->vec[2]);
            else if (b->inlet == IN_MACH && sim_is_gas(s))
                str_catf(o, "        type            surfaceNormalFixedValue;\n        refValue        uniform %.6g;\n        value           uniform (0 0 0);\n",
                         -inlet_speed(s, b, NULL));
            else if (b->inlet == IN_FLOW) str_catf(o, "        type            flowRateInletVelocity;\n        volumetricFlowRate %.6g;\n"
                                                      "        extrapolateProfile no;\n        value           uniform (0 0 0);\n", b->flow);
            else if (b->inlet == IN_MASS) str_catf(o, "        type            flowRateInletVelocity;\n        massFlowRate    %.6g;\n        rhoInlet        %.6g;\n"
                                                      "        extrapolateProfile no;\n        value           uniform (0 0 0);\n", b->mdot,
                                                      sim_is_gas(s) && b->T > 0 ? pabs(0) / (SIM_R / s->molW * b->T) : s->rho);
            else str_cat(o, "        type            pressureInletOutletVelocity;\n        value           uniform (0 0 0);\n");
        } else if (P) {
            if (b->inlet == IN_PRESSURE) str_catf(o, "        type            totalPressure;\n        p0              uniform %.6g;\n        value           uniform %.6g;\n", pb, pb);
            else str_cat(o, "        type            zeroGradient;\n");
        } else if (NUT) str_cat(o, "        type            calculated;\n        value           uniform 0;\n");
        else {
            double Ub = sim_ref_speed(s);
            if (b->inlet == IN_SPEED) Ub = fabs(b->speed);
            else if (b->inlet == IN_VECTOR) Ub = vlen(b->vec);
            else if (b->inlet == IN_MACH && sim_is_gas(s)) Ub = inlet_speed(s, b, NULL);
            double kb, ob, eb, nb, I, Lm;
            sim_inlet_turbulence(s, b, Ub, &I, &Lm);              /* by intensity and length, viscosity ratio or diameter */
            sim_turbulence(s, Ub, I, Lm, &kb, &ob, &eb, &nb);
            if (K) { num(v, sizeof v, kb); str_catf(o, "        type            turbulentIntensityKineticEnergyInlet;\n        intensity       %.6g;\n        value           uniform %s;\n", I / 100, v); }
            else if (OM) { num(v, sizeof v, ob); str_catf(o, "        type            turbulentMixingLengthFrequencyInlet;\n        mixingLength    %.6g;\n        value           uniform %s;\n", Lm, v); }
            else if (EP) { num(v, sizeof v, eb); str_catf(o, "        type            turbulentMixingLengthDissipationRateInlet;\n        mixingLength    %.6g;\n        value           uniform %s;\n", Lm, v); }
            else if (NT) { num(v, sizeof v, nb); str_catf(o, "        type            fixedValue;\n        value           uniform %s;\n", v); }
            else { num(v, sizeof v, RT ? re_theta(I) : 1); str_catf(o, "        type            fixedValue;\n        value           uniform %s;\n", v); }
        }
        break;
    }
    case ROLE_OUTLET:
    case ROLE_OPENING: {
        double pb = b->p / s->rho;
        if (sim_far_field(s, b)) {
            /* the free stream: in where it blows in, out where it leaves, both by its direction at each face */
            if (U) str_catf(o, "        type            freestreamVelocity;\n        freestreamValue uniform (%.6g %.6g %.6g);\n        value           uniform (%.6g %.6g %.6g);\n",
                            b->vec[0], b->vec[1], b->vec[2], b->vec[0], b->vec[1], b->vec[2]);
            else if (P) str_catf(o, "        type            freestreamPressure;\n        freestreamValue uniform %.6g;\n        value           uniform %.6g;\n", pb, pb);
            else if (NUT) str_cat(o, "        type            calculated;\n        value           uniform 0;\n");
            else {
                double kb, ob, eb, nb, I, Lm;
                sim_inlet_turbulence(s, b, vlen(b->vec), &I, &Lm);
                sim_turbulence(s, vlen(b->vec), I, Lm, &kb, &ob, &eb, &nb);
                double x = K ? kb : OM ? ob : EP ? eb : RT ? re_theta(I) : GI ? 1 : nb;
                num(v, sizeof v, x);
                str_catf(o, "        type            inletOutlet;\n        inletValue      uniform %s;\n        value           uniform %s;\n", v, v);
            }
            break;
        }
        if (U) str_cat(o, b->role == ROLE_OUTLET ? "        type            inletOutlet;\n        inletValue      uniform (0 0 0);\n        value           uniform (0 0 0);\n"
                                                 : "        type            pressureInletOutletVelocity;\n        value           uniform (0 0 0);\n");
        else if (P) {
            if (b->role == ROLE_OUTLET) str_catf(o, "        type            fixedValue;\n        value           uniform %.6g;\n", pb);
            else str_catf(o, "        type            totalPressure;\n        p0              uniform %.6g;\n        value           uniform %.6g;\n", pb, pb);
        } else if (NUT) str_cat(o, "        type            calculated;\n        value           uniform 0;\n");
        else { num(v, sizeof v, amb); str_catf(o, "        type            inletOutlet;\n        inletValue      uniform %s;\n        value           uniform %s;\n", v, v); }
        break;
    }
    }
    str_cat(o, "    }\n");
}

/* ---- hand edits: a three-way merge of the file on disk (theirs), what the setup wrote last time (base, kept in
   .qc-setup/) and what it writes now (mine). An entry the user added stays; one they changed or removed stays that way
   unless the setup now writes something else for it; then the setup's wins and is listed (sim_write_replaced). */
static char gKept[2048], gReplaced[2048];

const char *sim_write_kept(void) { return gKept; }
const char *sim_write_replaced(void) { return gReplaced; }

#define KP_MAX 8
typedef struct { const char *key[KP_MAX]; int n; } KPath;
enum { OP_KEEP, OP_INSERT, OP_DELETE };
typedef struct { int kind; char *path[KP_MAX]; int n; char *text; } MergeOp;
typedef struct { MergeOp *op; int n, cap; } MergeOps;

/* entries equal but for spacing (and comments' spacing) */
static bool same_text(const char *a, int al, const char *b, int bl)
{
    int i = 0, j = 0;
    for (;;) {
        bool sa = false, sb = false;
        while (i < al && (a[i] == ' ' || a[i] == '\t' || a[i] == '\n' || a[i] == '\r')) { i++; sa = true; }
        while (j < bl && (b[j] == ' ' || b[j] == '\t' || b[j] == '\n' || b[j] == '\r')) { j++; sb = true; }
        if (i >= al || j >= bl) return i >= al && j >= bl;
        if (sa != sb && i > 0 && j > 0) return false;
        if (a[i] != b[j]) return false;
        i++; j++;
    }
}

static bool same_entry(const FDFile *fa, const FDEntry *a, const FDFile *fb, const FDEntry *b)
{
    return same_text(fa->text + a->start, a->end - a->start, fb->text + b->start, b->end - b->start);
}

static FDEntry *at_path(FDFile *f, const KPath *k)
{
    FDEntry *e = &f->root;
    for (int i = 0; i < k->n && e; i++) e = fd_child(e, k->key[i]);
    return e;
}

static void path_text(char *out, size_t cap, const char *rel, const KPath *k, const char *last)
{
    snprintf(out, cap, "%s:", rel);
    for (int i = 0; i < k->n; i++) snprintf(out + strlen(out), cap - strlen(out), " %s", k->key[i]);
    if (last) snprintf(out + strlen(out), cap - strlen(out), " %s", last);
}

static void note(char *buf, const char *line)
{
    if (strlen(buf) + strlen(line) + 2 < sizeof gKept) { strcat(buf, line); strcat(buf, "\n"); }
}

static void add_op(MergeOps *m, int kind, const KPath *k, const char *last, const char *text, int tl)
{
    if (m->n == m->cap) { m->cap = m->cap ? 2 * m->cap : 16; m->op = realloc(m->op, sizeof(MergeOp) * (size_t)m->cap); }
    MergeOp *o = &m->op[m->n++];
    o->kind = kind; o->n = 0;
    for (int i = 0; i < k->n && o->n < KP_MAX; i++) o->path[o->n++] = strdup(k->key[i]);
    if (last && o->n < KP_MAX) o->path[o->n++] = strdup(last);
    o->text = text ? strndup(text, (size_t)tl) : NULL;
}

static bool has_directive(const FDFile *f, const FDEntry *d, const char *text, int tl)
{
    if (!d) return false;
    for (int i = 0; i < d->nChild; i++)
        if (d->child[i].kind == FD_DIRECTIVE && same_text(f->text + d->child[i].start, d->child[i].end - d->child[i].start, text, tl)) return true;
    return false;
}

static void merge_dict(FDFile *T, FDEntry *td, FDFile *B, FDEntry *bd, FDFile *R, KPath *k, const char *rel, MergeOps *m)
{
    char line[400];
    FDEntry *rd = at_path(R, k);
    for (int i = 0; i < td->nChild; i++) {
        FDEntry *t = &td->child[i];
        const char *txt = T->text + t->start; int tl = t->end - t->start;
        if (t->kind == FD_DIRECTIVE) {                  /* #include and the like: the user's own stay */
            if (!has_directive(B, bd, txt, tl) && !has_directive(R, rd, txt, tl)) { add_op(m, OP_INSERT, k, NULL, txt, tl); path_text(line, sizeof line, rel, k, t->key); note(gKept, line); }
            continue;
        }
        if (!t->key || !t->key[0] || (k->n == 0 && !strcmp(t->key, "FoamFile"))) continue;
        FDEntry *b = bd ? fd_child(bd, t->key) : NULL, *r = rd ? fd_child(rd, t->key) : NULL;
        if (!b) {                                       /* the user's own entry */
            if (!r) { add_op(m, OP_INSERT, k, NULL, txt, tl); path_text(line, sizeof line, rel, k, t->key); note(gKept, line); }
            continue;
        }
        if (same_entry(T, t, B, b)) continue;           /* untouched */
        if (t->kind == FD_DICT && b->kind == FD_DICT && r && r->kind == FD_DICT && k->n < KP_MAX) {
            k->key[k->n++] = t->key;
            merge_dict(T, t, B, b, R, k, rel, m);
            k->n--;
            continue;
        }
        if (!r) continue;                               /* the setup no longer writes it: it goes */
        if (same_entry(T, t, R, r)) continue;           /* the setup now writes what they wrote (the Run page's end time, taken in) */
        path_text(line, sizeof line, rel, k, t->key);
        if (same_entry(R, r, B, b)) { add_op(m, OP_KEEP, k, t->key, txt, tl); note(gKept, line); }
        else note(gReplaced, line);
    }
    /* entries the user deleted: they stay deleted while the setup writes them as before */
    for (int i = 0; bd && i < bd->nChild; i++) {
        FDEntry *b = &bd->child[i];
        if (b->kind == FD_DIRECTIVE || !b->key || fd_child(td, b->key)) continue;
        FDEntry *r = rd ? fd_child(rd, b->key) : NULL;
        if (!r) continue;
        path_text(line, sizeof line, rel, k, b->key);
        if (same_entry(R, r, B, b)) { add_op(m, OP_DELETE, k, b->key, NULL, 0); note(gKept, line); }
        else note(gReplaced, line);
    }
}

/* mine with the user's edits since base applied; NULL when it cannot tell (a file it cannot parse) */
static char *merge3(const char *rel, const char *theirs, const char *base, const char *mine)
{
    FDFile *T = fd_from_text(rel, theirs), *B = fd_from_text(rel, base), *R = fd_from_text(rel, mine);
    char *out = NULL;
    if (T && B && R && T->ok && B->ok && R->ok) {
        MergeOps m = { 0 };
        KPath k = { .n = 0 };
        merge_dict(T, &T->root, B, &B->root, R, &k, rel, &m);
        for (int i = 0; i < m.n; i++) {
            MergeOp *o = &m.op[i];
            KPath p = { .n = 0 };
            for (int j = 0; j < o->n; j++) p.key[p.n++] = o->path[j];
            if (o->kind == OP_INSERT) {
                FDEntry *d = at_path(R, &p);
                if (!d) continue;
                char *ins = malloc(strlen(o->text) + 64);
                if (!ins) continue;
                if (p.n == 0) { sprintf(ins, "\n%s\n", o->text); fd_replace_span(R, R->len, R->len, ins); }
                else { sprintf(ins, "%*s%s\n", 4 * p.n, "", o->text); fd_replace_span(R, d->valEnd, d->valEnd, ins); }
                free(ins);
            } else {
                FDEntry *e = at_path(R, &p);
                if (!e) continue;
                fd_replace_span(R, e->start, e->end, o->kind == OP_KEEP ? o->text : "");
            }
        }
        for (int i = 0; i < m.n; i++) { for (int j = 0; j < m.op[i].n; j++) free(m.op[i].path[j]); free(m.op[i].text); }
        free(m.op);
        if (R->ok) out = strdup(R->text);
    }
    fd_free(T); fd_free(B); fd_free(R);
    return out;
}

/* a file the setup wrote last time, without its comment line (foamDictionary -set drops comments): it still has at
   least half the setup's entries. A file pasted over it (a tutorial's controlDict) does not, and is the user's. */
static bool still_ours(const char *rel, const char *text, const char *base)
{
    FDFile *T = fd_from_text(rel, text), *B = fd_from_text(rel, base);
    int n = 0, have = 0;
    if (T && B && T->ok && B->ok)
        for (int i = 0; i < B->root.nChild; i++) {
            const FDEntry *b = &B->root.child[i];
            if (b->kind == FD_DIRECTIVE || !b->key || !strcmp(b->key, "FoamFile")) continue;
            n++;
            if (fd_child(&T->root, b->key)) have++;
        }
    fd_free(T); fd_free(B);
    return n > 0 && 2 * have >= n;
}

/* a file of ours (or none) is rewritten with the user's edits merged in; a file of the user's keeps its text as
   <file>.orig, once. What the setup wrote is kept in .qc-setup/<file>: the next write's base. */
static bool put_file(const char *casePath, const char *rel, const char *text, char *err, int cap)
{
    char p[PL_PATH], bp[PL_PATH], sub[PL_PATH];
    pl_join(p, sizeof p, casePath, rel);
    snprintf(sub, sizeof sub, ".qc-setup/%s", rel);
    pl_join(bp, sizeof bp, casePath, sub);
    char *old = pl_read_file(p, NULL), *merged = NULL, *base = old ? pl_read_file(bp, NULL) : NULL;
    /* ours: it says so, or it is still what the setup wrote last, edited (foamDictionary -set drops the comment line) */
    bool ours = old && (strstr(old, MARK_START) || (base && still_ours(rel, old, base)));
    if (old && !ours) {
        char bak[PL_PATH + 8]; snprintf(bak, sizeof bak, "%s.orig", p);
        if (!pl_exists(bak)) pl_write_file(bak, old, strlen(old));
    } else if (old && base && strcmp(base, old)) merged = merge3(rel, old, base, text);
    free(base);
    free(old);
    char d[PL_PATH]; pl_dirname(d, sizeof d, p); pl_mkdirs(d);
    const char *out = merged ? merged : text;
    bool ok = pl_write_file(p, out, strlen(out));
    free(merged);
    if (!ok) { snprintf(err, (size_t)cap, "Could not write %s", rel); return false; }
    pl_dirname(d, sizeof d, bp); pl_mkdirs(d);
    pl_write_file(bp, text, strlen(text));
    return true;
}

static void header(Str *o, const char *cls, const char *loc, const char *obj)
{
    foam_header(o, cls, loc, obj);
    str_cat(o, MARK "\n");
}

static bool write_field(const char *casePath, const SimSetup *s, const char *f, double k0, double om0, double ep0, double nt0, char *err, int cap)
{
    Str o; str_init(&o);
    bool vec = !strcmp(f, "U");
    header(&o, vec ? "volVectorField" : "volScalarField", "0", f);
    bool E = s->energy != EN_OFF, PR = !strcmp(f, "p") || !strcmp(f, "p_rgh");
    const char *dims = vec ? "[0 1 -1 0 0 0 0]" : PR && E ? "[1 -1 -2 0 0 0 0]" : !strcmp(f, "T") ? "[0 0 0 1 0 0 0]"
                     : !strcmp(f, "alphat") ? "[1 -1 -1 0 0 0 0]" : !strcmp(f, "p") ? "[0 2 -2 0 0 0 0]" : !strcmp(f, "k") ? "[0 2 -2 0 0 0 0]"
                     : !strcmp(f, "omega") ? "[0 0 -1 0 0 0 0]" : !strcmp(f, "epsilon") ? "[0 2 -3 0 0 0 0]"
                     : !strcmp(f, "ReThetat") || !strcmp(f, "gammaInt") ? "[0 0 0 0 0 0 0]" : "[0 2 -1 0 0 0 0]";
    str_catf(&o, "dimensions      %s;\n\n", dims);
    char v[64];
    if (vec) str_catf(&o, "internalField   uniform (%.6g %.6g %.6g);\n\n", s->U0[0], s->U0[1], s->U0[2]);
    else {
        double x = PR && E ? pabs(s->p0) : !strcmp(f, "T") ? s->T0 : !strcmp(f, "alphat") ? 0 : !strcmp(f, "p") ? s->p0 / s->rho : !strcmp(f, "k") ? k0 : !strcmp(f, "omega") ? om0 : !strcmp(f, "epsilon") ? ep0
                 : !strcmp(f, "nuTilda") ? nt0 : !strcmp(f, "ReThetat") ? re_theta(s->intensity) : !strcmp(f, "gammaInt") ? 1 : 0;
        num(v, sizeof v, x);
        str_catf(&o, "internalField   uniform %s;\n\n", v);
    }
    str_cat(&o, "boundaryField\n{\n");
    for (int i = 0; i < s->nB; i++) bc_entry(&o, s, &s->b[i], f, k0, om0, ep0, nt0);
    str_cat(&o, "}\n");
    char rel[64]; snprintf(rel, sizeof rel, "0/%s", f);
    bool ok = put_file(casePath, rel, o.s, err, cap);
    str_free(&o);
    return ok;
}

/* the equations solved besides U and the pressure, by their names in fvSchemes and fvSolution: the turbulence's, and h
   (the enthalpy, what the buoyant solvers solve for T) */
static int equations(const SimSetup *s, const char *const *fields, int nf, const char *eq[SIM_MAX_F])
{
    int n = 0;
    for (int i = 0; i < nf; i++) {
        const char *f = fields[i];
        if (!strcmp(f, "U") || !strcmp(f, "p") || !strcmp(f, "p_rgh") || !strcmp(f, "nut") || !strcmp(f, "alphat")) continue;
        eq[n++] = !strcmp(f, "T") ? (sim_is_gas(s) ? "e" : "h") : f;
    }
    return n;
}

/* a file of ours the setup no longer writes goes (the user's own stays) */
static void drop_ours(const char *casePath, const char *rel)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, rel);
    char *t = pl_read_file(p, NULL);
    if (t && strstr(t, MARK_START)) unlink(p);
    free(t);
}

/* the mesh's greatest non-orthogonality, degrees, from log.checkMesh ("Mesh non-orthogonality Max: 63.1 average: 9.2");
   0: no log, or one older than the mesh */
static double mesh_non_ortho(const char *casePath)
{
    char p[PL_PATH], f[PL_PATH];
    pl_join(p, sizeof p, casePath, "log.checkMesh");
    pl_join(f, sizeof f, casePath, "constant/polyMesh/faces");
    struct stat a, b;
    if (stat(p, &a) || (!stat(f, &b) && b.st_mtime > a.st_mtime)) return 0;
    char *t = pl_read_file(p, NULL);
    double v = 0;
    const char *q = t ? strstr(t, "Mesh non-orthogonality Max:") : NULL;
    if (q) v = atof(q + strlen("Mesh non-orthogonality Max:"));
    free(t);
    return v;
}

bool sim_write(const char *casePath, const SimSetup *s, char *err, int cap)
{
    err[0] = 0;
    gKept[0] = gReplaced[0] = 0;
    if (!(s->rho > 0) || !(s->nu > 0)) { snprintf(err, (size_t)cap, "The density and viscosity must be above 0"); return false; }
    if (!s->nB) { snprintf(err, (size_t)cap, "There are no boundaries: choose an STL or generate the mesh first"); return false; }
    bool E = s->energy != EN_OFF, buoy = s->energy == EN_BUOYANT;
    if (sim_is_gas(s) && (!(s->molW > 0) || !(s->gamma > 1) || !(s->muAs > 0) || !(s->pOp > 0))) {
        snprintf(err, (size_t)cap, "The gas's molar mass, ratio of specific heats, viscosity and operating pressure must be above 0 (gamma above 1)");
        return false;
    }
    if (E && (!(s->cp > 0) || !(s->kappa > 0) || !(s->T0 > 0) || !(s->Tref > 0))) {
        snprintf(err, (size_t)cap, "The specific heat, conductivity and temperatures must be above 0");
        return false;
    }
    bool tr = s->flow == FLOW_TRANSIENT, turb = s->turb != TURB_LAMINAR;
    int appr0 = sim_approach(s->turb);
    double Uref = sim_ref_speed(s), k0, om0, ep0, nt0;
    sim_turbulence(s, Uref, s->intensity, s->length, &k0, &om0, &ep0, &nt0);
    const char *fields[SIM_MAX_F], *eq[SIM_MAX_F];
    int nf = sim_fields(s, fields), ne = equations(s, fields, nf, eq);
    bool gas = sim_is_gas(s);
    const char *pn = E && !gas ? "p_rgh" : "p";                         /* the pressure solved for */
    bool ok = true;
    for (int i = 0; i < nf && ok; i++) ok = write_field(casePath, s, fields[i], k0, om0, ep0, nt0, err, cap);
    /* our old fields the model doesn't solve (nuTilda after k-omega ...) go; the user's own stay */
    static const char *const turbF[] = { "k", "omega", "epsilon", "nut", "nuTilda", "ReThetat", "gammaInt", "T", "p_rgh", "alphat", NULL };
    for (int i = 0; turbF[i]; i++) {
        bool used = false;
        for (int j = 0; j < nf; j++) if (!strcmp(fields[j], turbF[i])) used = true;
        if (used) continue;
        char rel[32]; snprintf(rel, sizeof rel, "0/%s", turbF[i]);
        drop_ours(casePath, rel);
    }
    if (!E) { drop_ours(casePath, "constant/thermophysicalProperties"); drop_ours(casePath, "constant/g"); }
    if (gas) { drop_ours(casePath, "constant/g"); drop_ours(casePath, "constant/transportProperties"); }   /* rho*Foam read neither */
    Str o; str_init(&o);
    /* ---- constant */
    if (!gas) {
        header(&o, "dictionary", "constant", "transportProperties");
        str_catf(&o, "// %s: density %.6g kg/m³ (pressures are divided by it)\ntransportModel  Newtonian;\n\nnu              %.6g;\n\nrho             %.6g;\n",
                 FLUID_NAME[s->fluid], s->rho, s->nu, s->rho);
        ok = ok && put_file(casePath, "constant/transportProperties", o.s, err, cap);
    }
    if (gas) {
        /* an ideal gas whose density follows p and T (psi = 1 / (R T)), cp from gamma, Sutherland's viscosity; the energy as
           the internal energy, the tutorials' way (rhoSimpleFoam/aerofoilNACA0012, squareBend). The first file written:
           the buffer may not be allocated yet */
        if (o.s) { o.len = 0; o.s[0] = 0; }
        header(&o, "dictionary", "constant", "thermophysicalProperties");
        double R = SIM_R / s->molW;
        str_catf(&o, "// %s: gamma %.4g, R %.6g J/(kg K); at %.6g Pa and %.6g K the density is %.4g kg/m³, the speed of sound %.4g m/s\n"
                     "thermoType\n{\n    type            hePsiThermo;\n    mixture         pureMixture;\n    transport       sutherland;\n"
                     "    thermo          hConst;\n    equationOfState perfectGas;\n    specie          specie;\n    energy          sensibleInternalEnergy;\n}\n\n"
                     "mixture\n{\n    specie\n    {\n        molWeight       %.6g;\n    }\n    thermodynamics\n    {\n        Cp              %.6g;\n        Hf              0;\n    }\n"
                     "    transport\n    {\n        As              %.6g;\n        Ts              %.6g;\n    }\n}\n",
                 GAS_NAME[s->gas], s->gamma, R, pabs(s->p0), s->T0, s->rho, sim_sound_speed(s, s->T0), s->molW, s->gamma * R / (s->gamma - 1), s->muAs, s->muTs);
        ok = ok && put_file(casePath, "constant/thermophysicalProperties", o.s, err, cap);
    } else if (E) {
        /* air: an ideal gas (perfectGas), its molecular weight set so the density is the one in Materials at Tref and
           1 bar; its expansion 1/T. A liquid: Boussinesq about rho at Tref. (Boussinesq for air crashes buoyantPimpleFoam:
           its pressure equation has no compressibility to take up a few per cent of density change.) Without buoyancy
           gravity is zero, so the density changes only move the flow as much as the heat does */
        o.len = 0; o.s[0] = 0;
        header(&o, "dictionary", "constant", "thermophysicalProperties");
        bool gas = s->fluid == FLUID_AIR;
        double mu = s->nu * s->rho, Pr = mu * s->cp / s->kappa;
        str_catf(&o, "// %s: Pr = mu cp / k = %.4g\nthermoType\n{\n    type            heRhoThermo;\n    mixture         pureMixture;\n    transport       const;\n"
                     "    thermo          hConst;\n    equationOfState %s;\n    specie          specie;\n    energy          sensibleEnthalpy;\n}\n\n"
                     "mixture\n{\n    specie\n    {\n        molWeight       %.6g;\n    }\n", FLUID_NAME[s->fluid], Pr, gas ? "perfectGas" : "Boussinesq",
                 gas ? s->rho * 8314.462618 * s->Tref / SIM_PATM : s->fluid == FLUID_WATER ? 18.02 : 28.96);
        if (!gas) str_catf(&o, "    equationOfState\n    {\n        rho0            %.6g;\n        T0              %.6g;\n        beta            %.6g;\n    }\n", s->rho, s->Tref, s->beta);
        str_catf(&o, "    thermodynamics\n    {\n        Cp              %.6g;\n        Hf              0;\n    }\n    transport\n    {\n        mu              %.6g;\n        Pr              %.6g;\n    }\n}\n",
                 s->cp, mu, Pr);
        ok = ok && put_file(casePath, "constant/thermophysicalProperties", o.s, err, cap);
        o.len = 0; o.s[0] = 0;
        header(&o, "uniformDimensionedVectorField", "constant", "g");
        double g[3] = { 0, 0, 0 };
        if (buoy) for (int k = 0; k < 3; k++) g[k] = s->g[k];
        str_catf(&o, "dimensions      [0 1 -2 0 0 0 0];\nvalue           (%.6g %.6g %.6g);\n", g[0], g[1], g[2]);
        ok = ok && put_file(casePath, "constant/g", o.s, err, cap);
    }
    o.len = 0; o.s[0] = 0;
    header(&o, "dictionary", "constant", "turbulenceProperties");
    int appr = sim_approach(s->turb);
    if (!turb) str_cat(&o, "simulationType  laminar;\n");
    else if (appr == APPR_RANS)
        str_catf(&o, "simulationType  RAS;\n\nRAS\n{\n    RASModel        %s;\n    turbulence      on;\n    printCoeffs     on;\n}\n", sim_model_word(s->turb));
    else {
        /* the filter width: the cell's size for LES; IDDES needs its own; DDES the cube root of the largest cell side */
        str_catf(&o, "simulationType  LES;\n\nLES\n{\n    LESModel        %s;\n    turbulence      on;\n    printCoeffs     on;\n\n", sim_model_word(s->turb));
        if (s->turb == TURB_SAIDDES)
            str_cat(&o, "    delta           IDDESDelta;\n\n    IDDESDeltaCoeffs\n    {\n        hmax            maxDeltaxyzCubeRoot;\n"
                        "        maxDeltaxyzCubeRootCoeffs\n        {\n        }\n    }\n");
        else if (appr == APPR_DES)
            str_cat(&o, "    delta           maxDeltaxyzCubeRoot;\n\n    maxDeltaxyzCubeRootCoeffs\n    {\n    }\n");
        else
            str_cat(&o, "    delta           cubeRootVol;\n\n    cubeRootVolCoeffs\n    {\n        deltaCoeff      1;\n    }\n");
        if (s->turb == TURB_DYNKEQN)                     /* the test filter its coefficients come from */
            str_cat(&o, "\n    dynamicKEqnCoeffs\n    {\n        filter          simple;\n    }\n");
        str_cat(&o, "}\n");
    }
    ok = ok && put_file(casePath, "constant/turbulenceProperties", o.s, err, cap);
    /* ---- controlDict: a write interval past the end would leave no results, so it is cut to the run's length */
    o.len = 0; o.s[0] = 0;
    header(&o, "dictionary", "system", "controlDict");
    if (!tr)
        str_catf(&o, "application     %s;\n\nstartFrom       latestTime;\nstartTime       0;\nstopAt          endTime;\nendTime         %.0f;\ndeltaT          1;\n\n"
                     "writeControl    timeStep;\nwriteInterval   %.0f;\n", sim_app(s), fmax(1, s->iterations), fmax(1, fmin(s->writeEvery, s->iterations)));
    else
        str_catf(&o, "application     %s;\n\nstartFrom       latestTime;\nstartTime       0;\nstopAt          endTime;\nendTime         %.6g;\ndeltaT          %.6g;\n\n"
                     "writeControl    %s;\nwriteInterval   %.6g;\n\nadjustTimeStep  %s;\nmaxCo           %.6g;\nmaxDeltaT       %.6g;\n",
                 sim_app(s), s->endTime, s->deltaT, s->adjustDt && s->algo != ALG_PISO ? "adjustable" : "runTime", fmin(s->writeTime, s->endTime), s->adjustDt && s->algo != ALG_PISO ? "yes" : "no", s->maxCo, fmax(s->writeTime, s->deltaT));
    str_cat(&o, "\npurgeWrite      0;\nwriteFormat     binary;\nwritePrecision  8;\nwriteCompression off;\ntimeFormat      general;\ntimePrecision   6;\n"
                "runTimeModifiable true;\n\nfunctions\n{\n    residuals\n    {\n        type            solverInfo;\n        libs            (utilityFunctionObjects);\n"
                "        fields          (U ");
    str_cat(&o, pn);
    for (int i = 0; i < ne; i++) str_catf(&o, " %s", eq[i]);
    str_cat(&o, ");\n        writeControl    timeStep;\n        writeInterval   1;\n    }\n");
    if (turb) {
        /* every wall's y+ at each write (postProcessing/yPlus: min, max, average per wall): the Run page holds it against
           the wall treatment (plan step 5) */
        str_cat(&o, "\n    // the walls' y+ (wall functions want 30 to 300 at the first cell, resolved walls about 1)\n    yPlus\n    {\n        type            yPlus;\n"
                    "        libs            (fieldFunctionObjects);\n        writeControl    writeTime;\n    }\n");
    }
    if (tr && (appr0 == APPR_LES || appr0 == APPR_DES)) {
        /* the time averages LES and DES are run for, from when the start-up has washed out */
        str_catf(&o, "\n    // the mean flow and its fluctuations from %.6g s (Physics > Setup, Solver: averaging start)\n    fieldAverage\n    {\n"
                     "        type            fieldAverage;\n        libs            (fieldFunctionObjects);\n        timeStart       %.6g;\n        writeControl    writeTime;\n"
                     "        fields\n        (\n            U { mean on; prime2Mean on; base time; }\n            %s { mean on; prime2Mean off; base time; }\n        );\n    }\n",
                 sim_avg_start(s), sim_avg_start(s), pn);
    }
    if (gas)                                            /* the Mach number as a field, with the results */
        str_cat(&o, "\n    // the Mach number, |U| / sqrt(gamma R T), written with the results (Ma)\n    MachNo\n    {\n        type            MachNo;\n"
                    "        libs            (fieldFunctionObjects);\n        executeControl  writeTime;\n        writeControl    writeTime;\n    }\n");
    if (s->forces) {
        /* the force on the part's walls in newtons (forces), and as coefficients (forceCoeffs): the Run page plots both */
        Str pt; str_init(&pt);
        for (int i = 0; i < s->nB; i++) if (sim_force_on(s, &s->b[i])) str_catf(&pt, "%s%s", pt.len ? " " : "", s->b[i].name);
        const SimForceRef *r = &s->fr;
        double d[3], l[3], pa[3], dl = vlen(r->drag);
        for (int k = 0; k < 3; k++) d[k] = dl > 0 ? r->drag[k] / dl : k == 0;
        double dot = 0; for (int k = 0; k < 3; k++) dot += r->lift[k] * d[k];
        for (int k = 0; k < 3; k++) l[k] = r->lift[k] - dot * d[k];          /* lift square to the drag */
        double ll = vlen(l);
        if (ll > 0) for (int k = 0; k < 3; k++) l[k] /= ll;
        else { l[0] = l[1] = l[2] = 0; l[fabs(d[2]) < 0.9 ? 2 : 1] = 1; }
        pa[0] = l[1] * d[2] - l[2] * d[1]; pa[1] = l[2] * d[0] - l[0] * d[2]; pa[2] = l[0] * d[1] - l[1] * d[0];   /* lift x drag: + is nose up */
        const char *common = "        libs            (forces);\n        writeControl    timeStep;\n        writeInterval   1;\n        log             yes;\n";
        /* with heat on, p is absolute and the density a field: the forces take rho and the pressure above SIM_PATM */
        char rho[96];
        if (E) snprintf(rho, sizeof rho, "rho;\n        pRef            %.8g", pabs(0));
        else snprintf(rho, sizeof rho, "rhoInf");
        str_catf(&o, "\n    // lift and drag on the part (Physics > Setup, Forces)\n    forces\n    {\n        type            forces;\n%s"
                     "        patches         (%s);\n        rho             %s;\n        rhoInf          %.8g;\n        CofR            (%.8g %.8g %.8g);\n    }\n",
                 common, pt.s, rho, s->rho, r->CofR[0], r->CofR[1], r->CofR[2]);
        str_catf(&o, "\n    forceCoeffs\n    {\n        type            forceCoeffs;\n%s        patches         (%s);\n        rho             %s;\n"
                     "        rhoInf          %.8g;\n        CofR            (%.8g %.8g %.8g);\n        dragDir         (%.8g %.8g %.8g);\n"
                     "        liftDir         (%.8g %.8g %.8g);\n        pitchAxis       (%.8g %.8g %.8g);\n        magUInf         %.8g;\n"
                     "        lRef            %.8g;\n        Aref            %.8g;\n    }\n",
                 common, pt.s, rho, s->rho, r->CofR[0], r->CofR[1], r->CofR[2], d[0], d[1], d[2], l[0], l[1], l[2], pa[0], pa[1], pa[2],
                 sim_force_speed(s), r->lRef > 0 ? r->lRef : 1, r->aRef > 0 ? r->aRef : 1);
        str_free(&pt);
    }
    if (E && s->heatReport) {
        /* the heat through each wall, W (integral) and W/m² (min, max), at each write: postProcessing/wallHeatFlux */
        Str pt; str_init(&pt);
        for (int i = 0; i < s->nB; i++)
            if (s->b[i].role == ROLE_WALL && !sim_constraint(s->b[i].meshType)) str_catf(&pt, "%s%s", pt.len ? " " : "", s->b[i].name);
        if (pt.len)
            str_catf(&o, "\n    // the heat into the fluid through each wall (Physics > Setup, Reports)\n    wallHeatFlux\n    {\n        type            wallHeatFlux;\n"
                         "        libs            (fieldFunctionObjects);\n        patches         (%s);\n        writeControl    writeTime;\n    }\n", pt.s);
        str_free(&pt);
        /* each heated or cooled wall's mean flux (W/m²) and temperature (K) every iteration, and its Nusselt number: the
           mean flux scaled by L / (kappa dT). After wallHeatFlux, whose field they average. */
        double dT = sim_nu_dT(s), L = s->nuLength;
        for (int i = 0; i < s->nB; i++) {
            const SimBoundary *b = &s->b[i];
            if (!sim_wall_heated(b)) continue;
            const char *sfv = "        type            surfaceFieldValue;\n        libs            (fieldFunctionObjects);\n        regionType      patch;\n"
                              "        writeFields     false;\n        operation       areaAverage;\n        writeControl    timeStep;\n        writeInterval   1;\n";
            str_catf(&o, "\n    // %s's mean heat flux into the fluid (W/m²) and temperature (K)\n    wallHeat_%s\n    {\n%s        name            %s;\n"
                         "        fields          (wallHeatFlux T);\n    }\n", b->name, b->name, sfv, b->name);
            if (L > 0 && dT > 0 && s->kappa > 0)
                str_catf(&o, "\n    // %s's Nusselt number: its mean heat flux x L / (kappa dT), L %.6g m, kappa %.6g W/(m K), dT %.6g K\n    Nu_%s\n    {\n%s"
                             "        name            %s;\n        fields          (wallHeatFlux);\n        scaleFactor     %.10g;\n    }\n",
                         b->name, L, s->kappa, dT, b->name, sfv, b->name, L / (s->kappa * dT));
        }
    }
    str_cat(&o, "}\n");
    ok = ok && put_file(casePath, "system/controlDict", o.s, err, cap);
    /* ---- fvSchemes: second order for U, first order (bounded) for the turbulence; limited corrections suit snappy meshes */
    o.len = 0; o.s[0] = 0;
    header(&o, "dictionary", "system", "fvSchemes");
    /* LES and DES resolve eddies: LUST for the velocity (little numerical damping), limitedLinear for their turbulence */
    const char *bd = tr ? "" : "bounded ";
    bool eddies = appr == APPR_LES || appr == APPR_DES;
    int ts = sim_time_scheme(s);
    const char *ddt = !tr ? "steadyState" : ts == TS_BACKWARD ? "backward" : ts == TS_CN ? "CrankNicolson 0.9" : "Euler";
    str_catf(&o, "ddtSchemes\n{\n    default         %s;\n}\n\ngradSchemes\n{\n    default         Gauss linear;\n    grad(U)         cellLimited Gauss linear 1;\n%s}\n\n"
                 "divSchemes\n{\n    default         none;\n    div(phi,U)      %sGauss %s grad(U);\n", ddt, gas ? "    limited         cellLimited Gauss linear 1;\n" : "",
             bd, eddies ? "LUST" : "linearUpwind");
    for (int i = 0; i < ne; i++) {
        /* the enthalpy: limitedLinear, as the heatTransfer tutorials (upwind smears a thermal boundary layer); a gas's
           internal energy: linearUpwind on a limited gradient, as the compressible tutorials */
        const char *sch = !strcmp(eq[i], "e") ? "linearUpwind limited" : !strcmp(eq[i], "h") ? "limitedLinear 1" : eddies ? "limitedLinear 1" : "upwind";
        str_catf(&o, "    div(phi,%s)%*s%sGauss %s;\n", eq[i], (int)(strlen(eq[i]) < 8 ? 8 - strlen(eq[i]) : 1), "", bd, sch);
    }
    /* a gas: the kinetic energy's and the pressure work's terms (div(phi,Ekp) steady, div(phiv,p) transient), and the
       transonic pressure equation's convection (div(phid,p) transient, div((phi|interpolate(rho)),p) steady) */
    if (gas) str_catf(&o, "    div(phi,K)       %sGauss linearUpwind limited;\n    div(phi,Ekp)     %sGauss linearUpwind limited;\n    div(phiv,p)      Gauss linear;\n"
                          "    div(phid,p)      Gauss upwind;\n    div((phi|interpolate(rho)),p) %sGauss upwind;\n    div(((rho*nuEff)*dev2(T(grad(U))))) Gauss linear;\n}\n\n", bd, bd, bd);
    else if (E) str_catf(&o, "    div(phi,K)       %sGauss linear;\n    div(((rho*nuEff)*dev2(T(grad(U))))) Gauss linear;\n}\n\n", bd);
    else str_cat(&o, "    div((nuEff*dev2(T(grad(U))))) Gauss linear;\n}\n\n");
    /* the non-orthogonal correction from the mesh's own checkMesh: full on a clean mesh (blockMesh's, under 60°), limited
       to half on a snappy one (up to 80°), to a third past that; limited to half with no log. On the aerofoilNACA0012
       tutorial's mesh the half limit put the drag 6.8 % off the tutorial's, full correction 1.4 % */
    double no = mesh_non_ortho(casePath);
    const char *corr = no > 0 && no < 60 ? "corrected" : no >= 80 ? "limited corrected 0.33" : "limited corrected 0.5";
    str_catf(&o, "laplacianSchemes\n{\n    default         Gauss linear %s;\n}\n\n"
                 "interpolationSchemes\n{\n    default         linear;\n}\n\nsnGradSchemes\n{\n    default         %s;\n}\n\nwallDist\n{\n    method          meshWave;\n}\n",
             corr, corr);
    ok = ok && put_file(casePath, "system/fvSchemes", o.s, err, cap);
    /* ---- fvSolution */
    o.len = 0; o.s[0] = 0;
    header(&o, "dictionary", "system", "fvSolution");
    Str rx; str_init(&rx);                                              /* "(U|k|omega)" */
    str_cat(&rx, "U");
    for (int i = 0; i < ne; i++) str_catf(&rx, "|%s", eq[i]);
    double pRef = E ? pabs(s->p0) : s->p0 / s->rho;
    str_catf(&o, "solvers\n{\n    %s\n    {\n        solver          GAMG;\n        smoother        GaussSeidel;\n        tolerance       1e-7;\n        relTol          ", pn);
    if (tr) str_catf(&o, "0.01;\n    }\n\n    %sFinal\n    {\n        $%s;\n        relTol          0;\n    }\n\n", pn, pn);
    else str_cat(&o, "0.1;\n    }\n\n");
    if (tr && E)                                                        /* buoyantPimpleFoam's density: worked out, not solved */
        str_cat(&o, "    \"rho.*\"\n    {\n        solver          PCG;\n        preconditioner  DIC;\n        tolerance       0;\n        relTol          0;\n    }\n\n");
    if (tr && s->algo == ALG_LARGE)                                      /* outer correctors short of the last: loosely */
        str_catf(&o, "    \"(%s)\"\n    {\n        solver          smoothSolver;\n        smoother        symGaussSeidel;\n        tolerance       1e-8;\n        relTol          0.1;\n    }\n\n"
                     "    \"(%s)Final\"\n    {\n        solver          smoothSolver;\n        smoother        symGaussSeidel;\n        tolerance       1e-8;\n        relTol          0;\n    }\n}\n\n", rx.s, rx.s);
    else
        str_catf(&o, "    \"(%s)%s\"\n    {\n        solver          smoothSolver;\n        smoother        symGaussSeidel;\n        tolerance       1e-8;\n        relTol          %s;\n    }\n}\n\n",
                 rx.s, tr ? "(Final)?" : "", tr ? "0" : "0.1");
    /* a flow driven by pressure (a total-pressure inlet, an opening) needs SIMPLE's firmer relaxation; else SIMPLEC */
    bool pDriven = false;
    for (int i = 0; i < s->nB; i++)
        if (!sim_constraint(s->b[i].meshType) && ((s->b[i].role == ROLE_INLET && s->b[i].inlet == IN_PRESSURE) || s->b[i].role == ROLE_OPENING)) pDriven = true;
    /* a gas: the pressure kept between a tenth of the lowest and twice the highest the boundaries fix (pressureControl),
       the transonic form above Mach 0.8, and on a transient run the density within bounds wide of anything the
       boundaries' pressures and temperatures make */
    char gx[400] = "";
    if (gas) {
        double pLo = pabs(s->p0), pHi = pLo, tLo = s->T0, tHi = s->T0, R = SIM_R / s->molW;
        for (int i = 0; i < s->nB; i++) {
            const SimBoundary *b = &s->b[i];
            if (sim_constraint(b->meshType)) continue;
            if (b->role != ROLE_WALL && b->role != ROLE_SYMMETRY) { pLo = fmin(pLo, pabs(b->p)); pHi = fmax(pHi, pabs(b->p)); }
            if (b->T > 0) { tLo = fmin(tLo, b->T); tHi = fmax(tHi, b->T); }
        }
        snprintf(gx, sizeof gx, "    transonic       %s;\n    pMinFactor      0.1;\n    pMaxFactor      2;\n", sim_transonic(s) ? "yes" : "no");
        if (tr) snprintf(gx + strlen(gx), sizeof gx - strlen(gx), "    rhoMin          %.4g;\n    rhoMax          %.4g;\n",
                         0.1 * pLo / (R * 2 * tHi), 2 * 2 * pHi / (R * 0.5 * tLo));
    }
    if (!tr && gas) {
        /* rhoSimpleFoam. Subsonic: plain SIMPLE relaxed as the aerofoilNACA0012 tutorial (p 0.7, rho 0.01, U 0.3); SIMPLEC
           diverged round that Mach 0.72 aerofoil. Transonic: SIMPLEC with the squareBend tutorial's light relaxation, the
           pressure equation relaxed as an equation (no longer symmetric); plain SIMPLE stopped a supersonic nozzle on a
           negative temperature */
        bool ts = sim_transonic(s);
        str_catf(&o, "SIMPLE\n{\n    nNonOrthogonalCorrectors 0;\n    consistent      %s;\n%s", ts ? "yes" : "no", gx);
        if (s->resStop)
            str_catf(&o, "\n    residualControl\n    {\n        %-15s %.6g;\n        \"(%s)\"  %.6g;\n    }\n", pn, s->resP, rx.s, s->resRest);
        if (ts) str_cat(&o, "}\n\nrelaxationFactors\n{\n    fields\n    {\n        p               1;\n    }\n    equations\n    {\n        p               1;\n"
                            "        U               0.9;\n        e               0.8;\n        \".*\"            0.9;\n    }\n}\n");
        else str_cat(&o, "}\n\nrelaxationFactors\n{\n    fields\n    {\n        p               0.7;\n        rho             0.01;\n    }\n    equations\n    {\n"
                         "        U               0.3;\n        e               0.7;\n        \".*\"            0.7;\n    }\n}\n");
    } else if (!tr) {
        /* buoyantSimpleFoam has no SIMPLEC; with buoyancy, no momentum predictor (the buoyantCavity tutorial's way) */
        if (E) str_catf(&o, "SIMPLE\n{\n    momentumPredictor %s;\n    nNonOrthogonalCorrectors 0;\n    pRefCell        0;\n    pRefValue       %.8g;\n", buoy ? "no" : "yes", pRef);
        else str_catf(&o, "SIMPLE\n{\n    nNonOrthogonalCorrectors 0;\n    consistent      %s;\n    pRefCell        0;\n    pRefValue       %.6g;\n", pDriven ? "no" : "yes", pRef);
        /* the run ends early once every initial residual is under its target ("SIMPLE solution converged in N iterations") */
        if (s->resStop)
            str_catf(&o, "\n    residualControl\n    {\n        %-15s %.6g;\n        \"(%s)\"  %.6g;\n    }\n", pn, s->resP, rx.s, s->resRest);
        str_cat(&o, "}\n\n");
        /* buoyancy: the tutorials' 0.7 on p_rgh and 0.3 on U, forced convection SIMPLE's usual 0.3 and 0.7; h 0.9 (the
           tutorials' 0.3 took de Vahl Davis's cavity 4000 iterations to settle, 0.9 about 1000) */
        if (E) str_catf(&o, "relaxationFactors\n{\n    fields\n    {\n        rho             1;\n        p_rgh           %s;\n    }\n    equations\n    {\n"
                            "        U               %s;\n        h               0.9;\n        \".*\"            0.7;\n    }\n}\n", buoy ? "0.7" : "0.3", buoy ? "0.3" : "0.7");
        else if (pDriven) str_cat(&o, "relaxationFactors\n{\n    fields\n    {\n        p               0.3;\n    }\n    equations\n    {\n        U               0.7;\n        \".*\"            0.7;\n    }\n}\n");
        else str_cat(&o, "relaxationFactors\n{\n    equations\n    {\n        U               0.9;\n        \".*\"            0.7;\n    }\n}\n");
    } else if (s->algo == ALG_PISO)
        str_catf(&o, "PISO\n{\n    nCorrectors     %d;\n    nNonOrthogonalCorrectors 1;\n    pRefCell        0;\n    pRefValue       %.8g;\n}\n",
                 s->nCorr, pRef);
    else if (s->algo == ALG_PIMPLE)
        str_catf(&o, "PIMPLE\n{\n    momentumPredictor yes;\n    nOuterCorrectors %d;\n    nCorrectors     %d;\n    nNonOrthogonalCorrectors 1;\n"
                     "    pRefCell        0;\n    pRefValue       %.8g;\n%s}\n", s->nOuter, s->nCorr, pRef, gx);
    else {
        /* large steps: many outer correctors, each time step ending once its residuals are small, relaxed like SIMPLE
           (the last outer corrector unrelaxed) */
        str_catf(&o, "PIMPLE\n{\n    momentumPredictor yes;\n    nOuterCorrectors %d;\n    nCorrectors     %d;\n    nNonOrthogonalCorrectors 1;\n"
                     "    turbOnFinalIterOnly false;\n    pRefCell        0;\n    pRefValue       %.8g;\n%s\n"
                     "    residualControl\n    {\n        %s\n        {\n            tolerance       %.6g;\n            relTol          0;\n        }\n"
                     "        \"(%s)\"\n        {\n            tolerance       %.6g;\n            relTol          0;\n        }\n    }\n}\n\n",
                 s->nOuter, s->nCorr, pRef, gx, pn, s->resP, rx.s, s->resRest);
        str_catf(&o, "relaxationFactors\n{\n    fields\n    {\n        %-15s 0.3;\n        %sFinal%*s1;\n    }\n"
                     "    equations\n    {\n        \".*\"            0.7;\n        \".*Final\"       1;\n    }\n}\n", pn, pn, (int)(10 - strlen(pn)), "");
    }
    str_free(&rx);
    ok = ok && put_file(casePath, "system/fvSolution", o.s, err, cap);
    /* ---- fvOptions, a gas's: the temperature and the speed kept in bounds while it starts. A total pressure on fluid
       at rest makes a wild first iteration (the speed overshoots, the kinetic energy takes the internal energy below zero
       and the thermo stops on a negative temperature); once it settles neither limit acts. A file of the user's own
       (porous zones, sources) is left as it is, and so is one in constant/ */
    char fo[PL_PATH], fc[PL_PATH];
    pl_join(fo, sizeof fo, casePath, "system/fvOptions");
    pl_join(fc, sizeof fc, casePath, "constant/fvOptions");
    char *had = pl_read_file(fo, NULL);
    bool theirs = (had && !strstr(had, MARK_START)) || pl_exists(fc);
    free(had);
    if (gas && !theirs) {
        double tLo = s->T0, tHi = s->T0, Ma = sim_mach(s);
        for (int i = 0; i < s->nB; i++) if (!sim_constraint(s->b[i].meshType) && s->b[i].T > 0) { tLo = fmin(tLo, s->b[i].T); tHi = fmax(tHi, s->b[i].T); }
        double uMax = 3 * sim_sound_speed(s, tHi) * fmax(1, 1.5 * Ma);
        o.len = 0; o.s[0] = 0;
        header(&o, "dictionary", "system", "fvOptions");
        str_catf(&o, "// start-up guards: inactive once the run settles (each says how many cells it limited in the log)\n"
                     "limitT\n{\n    type            limitTemperature;\n    active          yes;\n    selectionMode   all;\n    min             %.4g;\n    max             %.4g;\n}\n\n"
                     "limitU\n{\n    type            limitVelocity;\n    active          yes;\n    selectionMode   all;\n    max             %.4g;\n}\n",
                 0.2 * tLo, 5 * tHi, uMax);
        ok = ok && put_file(casePath, "system/fvOptions", o.s, err, cap);
    } else if (!gas) drop_ours(casePath, "system/fvOptions");
    str_free(&o);
    return ok;
}

static bool take(FDFile *f, const char *key, double *v)
{
    char b[64];
    if (!fd_get_resolved(f, key, b, sizeof b)) return false;
    char *e; double d = strtod(b, &e);
    if (e == b || !(d > 0) || d == *v) return false;
    *v = d;
    return true;
}

bool sim_take_controls(const char *casePath, SimSetup *s)
{
    /* ours: the case has a setup, and its controlDict runs the solver the setup writes. Not our mark: foamDictionary -set
       and some editors rewrite the file without its comments */
    char p[PL_PATH]; sim_ini_path(casePath, p, sizeof p);
    if (!pl_is_file(p)) return false;
    pl_join(p, sizeof p, casePath, "system/controlDict");
    FDFile *f = fd_load(p);
    if (!f) return false;
    char app[64] = "";
    if (!fd_get(f, "application", app, sizeof app) || strcmp(app, sim_app(s))) { fd_free(f); return false; }
    bool ch = false;
    if (s->flow == FLOW_STEADY) {
        ch |= take(f, "endTime", &s->iterations);
        ch |= take(f, "writeInterval", &s->writeEvery);
    } else {
        ch |= take(f, "endTime", &s->endTime);
        ch |= take(f, "deltaT", &s->deltaT);
        ch |= take(f, "writeInterval", &s->writeTime);
        ch |= take(f, "maxCo", &s->maxCo);
        char b[32];
        if (fd_get(f, "adjustTimeStep", b, sizeof b)) {
            bool adj = !strcmp(b, "yes") || !strcmp(b, "on") || !strcmp(b, "true");
            if (adj != s->adjustDt) { s->adjustDt = adj; ch = true; }
        }
    }
    fd_free(f);
    return ch;
}
