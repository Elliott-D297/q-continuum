/* params.c -- scanners for the programs' input files (see params.h). Each scanner walks the text once and records
 * the byte span of every value it shows; an edit splices the new text into that span and scans again. */
#define _GNU_SOURCE
#include "params.h"
#include "program.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ============================================================================ well-known keys */
typedef struct { int prog; const char *key; const char *unit; const char *hint; } Known;

static const Known KNOWN[] = {
    { PROG_MOOSE, "end_time", "s", "Simulated time the run ends at" },
    { PROG_MOOSE, "start_time", "s", "Simulated time the run starts from" },
    { PROG_MOOSE, "dt", "s", "Time step (a TimeStepper block may change it)" },
    { PROG_MOOSE, "num_steps", "steps", "Number of time steps (instead of end_time)" },
    { PROG_MOOSE, "nx", "cells", "Elements along x" }, { PROG_MOOSE, "ny", "cells", "Elements along y" }, { PROG_MOOSE, "nz", "cells", "Elements along z" },
    { PROG_MOOSE, "xmax", "m", "Domain length in x" }, { PROG_MOOSE, "ymax", "m", "Domain length in y" }, { PROG_MOOSE, "zmax", "m", "Domain length in z" },
    { PROG_MOOSE, "value", NULL, "The boundary value (Dirichlet: the variable's units)" },
    { PROG_MOOSE, "initial_condition", NULL, "Starting value of the variable" },
    { PROG_MOOSE, "prop_values", NULL, "Material values, in the order of prop_names" },
    { PROG_MOOSE, "thermal_conductivity", "W/(m·K)", NULL }, { PROG_MOOSE, "specific_heat", "J/(kg·K)", NULL },
    { PROG_MOOSE, "density", "kg/m³", NULL }, { PROG_MOOSE, "youngs_modulus", "Pa", NULL }, { PROG_MOOSE, "poissons_ratio", NULL, NULL },
    { PROG_MOOSE, "solve_type", NULL, "NEWTON, PJFNK or JFNK" }, { PROG_MOOSE, "nl_rel_tol", NULL, "Nonlinear relative tolerance" },
    { PROG_MOOSE, "nl_abs_tol", NULL, "Nonlinear absolute tolerance" }, { PROG_MOOSE, "file", NULL, "Mesh or data file" },
    { PROG_MOOSE, "steady_state_detection", NULL, "Stop when the solution stops changing" },

    { PROG_ELMER, "Timestep Sizes", "s", "Time step" }, { PROG_ELMER, "Timestep Intervals", "steps", "Number of time steps" },
    { PROG_ELMER, "Simulation Type", NULL, "Steady state, Transient or Scanning" }, { PROG_ELMER, "Output Intervals", "steps", "Write results every N steps" },
    { PROG_ELMER, "Steady State Max Iterations", NULL, "Coupling iterations between solvers" },
    { PROG_ELMER, "Heat Conductivity", "W/(m·K)", NULL }, { PROG_ELMER, "Density", "kg/m³", NULL }, { PROG_ELMER, "Heat Capacity", "J/(kg·K)", NULL },
    { PROG_ELMER, "Heat Source", "W/kg", "Heat per unit mass" }, { PROG_ELMER, "Temperature", "K", NULL }, { PROG_ELMER, "Heat Flux", "W/m²", NULL },
    { PROG_ELMER, "Heat Transfer Coefficient", "W/(m²·K)", NULL }, { PROG_ELMER, "External Temperature", "K", NULL },
    { PROG_ELMER, "Emissivity", NULL, "0 to 1" }, { PROG_ELMER, "Youngs Modulus", "Pa", NULL }, { PROG_ELMER, "Poisson Ratio", NULL, NULL },
    { PROG_ELMER, "Linear System Convergence Tolerance", NULL, NULL }, { PROG_ELMER, "Nonlinear System Max Iterations", NULL, NULL },

    { PROG_CALCULIX, "E", "Pa (SI model)", "Young's modulus (units follow the model: Pa with m, MPa with mm)" }, { PROG_CALCULIX, "nu", NULL, "Poisson's ratio" },
    { PROG_CALCULIX, "k", "W/(m·K)", "Conductivity" }, { PROG_CALCULIX, "cp", "J/(kg·K)", "Specific heat" },
    { PROG_CALCULIX, "rho", "kg/m³", "Density" }, { PROG_CALCULIX, "alpha", "1/K", "Thermal expansion" },
    { PROG_CALCULIX, "initial increment", "s", "First time increment" }, { PROG_CALCULIX, "step period", "s", "Length of the step" },
    { PROG_CALCULIX, "min increment", "s", NULL }, { PROG_CALCULIX, "max increment", "s", NULL },
    { PROG_CALCULIX, "INC", "increments", "Most increments the step may take" },
    { PROG_CALCULIX, "load", NULL, "node or set, degree of freedom, value" },
    { PROG_CALCULIX, "boundary", NULL, "node or set, first dof, last dof, value (dof 11 = temperature)" },

    { PROG_ASTER, "JUSQU_A", "s", "End of the time list" }, { PROG_ASTER, "PAS", "s", "Time step of the list" },
    { PROG_ASTER, "NOMBRE", "steps", "Steps in the interval" }, { PROG_ASTER, "INST_FIN", "s", "End time" },
    { PROG_ASTER, "LAMBDA", "W/(m·K)", "Conductivity" }, { PROG_ASTER, "RHO_CP", "J/(m³·K)", "Volumetric heat capacity" },
    { PROG_ASTER, "E", "Pa", "Young's modulus" }, { PROG_ASTER, "NU", NULL, "Poisson's ratio" }, { PROG_ASTER, "RHO", "kg/m³", "Density" },
    { PROG_ASTER, "ALPHA", "1/K", "Thermal expansion" }, { PROG_ASTER, "TEMP", "K or °C", "Imposed temperature" },
    { PROG_ASTER, "VALE", NULL, "Value" }, { PROG_ASTER, "FX", "N", NULL }, { PROG_ASTER, "FY", "N", NULL }, { PROG_ASTER, "FZ", "N", NULL },
    { PROG_ASTER, "PRES", "Pa", "Pressure" }, { PROG_ASTER, "FLUN", "W/m²", "Normal heat flux" }, { PROG_ASTER, "COEF_H", "W/(m²·K)", NULL },
    { PROG_ASTER, "TEMP_EXT", "K or °C", NULL }, { PROG_ASTER, "PARM_THETA", NULL, "1 = implicit Euler, 0.57 = Crank-Nicolson-ish" },
    { PROG_ASTER, "memory_limit", "MB", "Memory for the job" }, { PROG_ASTER, "time_limit", "s", "Wall time limit" },
    { PROG_ASTER, "mpi_nbcpu", "processes", NULL }, { PROG_ASTER, "ncpus", "threads", NULL },

    { PROG_SPARTA, "timestep", "s", "Time step" }, { PROG_SPARTA, "run", "steps", "Steps to run" },
    { PROG_SPARTA, "global", NULL, "nrho [1/m³] = number density, fnum = real molecules per simulated particle" },
    { PROG_SPARTA, "create_grid", "cells", "Grid cells in x y z" }, { PROG_SPARTA, "create_box", "m", "xlo xhi ylo yhi zlo zhi" },
    { PROG_SPARTA, "mixture", NULL, "vstream [m/s], temp [K] of the incoming gas" }, { PROG_SPARTA, "species", NULL, "Species file and species" },
    { PROG_SPARTA, "surf_collide", NULL, "diffuse <wall T [K]> <accommodation 0-1>" }, { PROG_SPARTA, "collide", NULL, "Collision model and its file" },
    { PROG_SPARTA, "fix", NULL, NULL }, { PROG_SPARTA, "stats", "steps", "Print every N steps" }, { PROG_SPARTA, "dump", NULL, "What is written and how often" },
    { PROG_SPARTA, "boundary", NULL, "o open, r reflect, p periodic, s surface (x y z)" }, { PROG_SPARTA, "dimension", NULL, "2 or 3" },

    { PROG_SATURNE, "time_step_ref", "s", "Reference time step" }, { PROG_SATURNE, "iterations", "steps", "Time steps to run" },
    { PROG_SATURNE, "property_value", NULL, NULL }, { PROG_SATURNE, "initial_value", NULL, NULL },
    { PROG_SATURNE, "max_courant_num", NULL, NULL }, { PROG_SATURNE, "max_fourier_num", NULL, NULL },
};

/* Basic view: the keys that set time, size, materials and loads */
static const char *const BASIC[] = {
    "end_time", "dt", "num_steps", "nx", "ny", "nz", "xmax", "ymax", "zmax", "value", "initial_condition", "prop_values",
    "Timestep Sizes", "Timestep Intervals", "Simulation Type", "Heat Conductivity", "Density", "Heat Capacity", "Heat Source",
    "Temperature", "Heat Flux", "Youngs Modulus", "Poisson Ratio",
    "E", "nu", "k", "cp", "rho", "alpha", "step period", "initial increment", "load", "boundary",
    "JUSQU_A", "PAS", "INST_FIN", "LAMBDA", "RHO_CP", "NU", "RHO", "TEMP", "PRES", "FLUN",
    "timestep", "run", "global", "create_grid", "create_box", "mixture", "surf_collide",
    "time_step_ref", "iterations", NULL };

static void annotate(int prog, Param *p)
{
    const char *k = p->key;
    const char *last = strrchr(k, '>');                          /* "INCREMENT > PAS" -> PAS */
    if (last) { last++; while (*last == ' ') last++; } else last = k;
    for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++)
        if (KNOWN[i].prog == prog && !strcasecmp(KNOWN[i].key, last)) { p->unit = KNOWN[i].unit; p->hint = KNOWN[i].hint; break; }
    p->importance = 1;
    for (int i = 0; BASIC[i]; i++) if (!strcasecmp(BASIC[i], last)) p->importance = 2;
}

/* ============================================================================ adding */
static void add(ParamFile *f, const char *group, const char *key, int start, int len)
{
    while (len > 0 && (f->text[start + len - 1] == ' ' || f->text[start + len - 1] == '\t' || f->text[start + len - 1] == '\r')) len--;
    while (len > 0 && (f->text[start] == ' ' || f->text[start] == '\t')) { start++; len--; }
    if (len < 0) return;
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 64; f->p = realloc(f->p, sizeof(Param) * (size_t)f->cap); }
    Param *p = &f->p[f->n++];
    memset(p, 0, sizeof *p);
    pl_strlcpy(p->group, group, sizeof p->group);
    pl_strlcpy(p->key, key, sizeof p->key);
    p->start = start; p->len = len;
    int l = 1; for (int i = 0; i < start; i++) if (f->text[i] == '\n') l++;
    p->line = l;
    int c = len < (int)sizeof p->value - 1 ? len : (int)sizeof p->value - 1;
    memcpy(p->value, f->text + start, (size_t)c); p->value[c] = 0;
    p->editable = len < (int)sizeof p->value - 1 && !memchr(f->text + start, '\n', (size_t)len);
    char *e; double v = strtod(p->value, &e); (void)v;
    while (*e == ' ') e++;
    p->numeric = e != p->value && !*e;
    annotate(f->prog, p);
}

/* append to a fixed buffer at *at, never past its end (snprintf returns what it would have written) */
static void cat_at(char *b, int cap, int *at, const char *x, const char *y)
{
    if (*at >= cap - 1) return;
    int n = snprintf(b + *at, (size_t)(cap - *at), "%s%s", x, y);
    *at = n < 0 ? *at : (*at + n < cap - 1 ? *at + n : cap - 1);
}

static int line_end(const char *t, int i) { while (t[i] && t[i] != '\n') i++; return i; }
static int skip_blank(const char *t, int i) { while (t[i] == ' ' || t[i] == '\t') i++; return i; }

/* ============================================================================ MOOSE */
static void scan_moose(ParamFile *f)
{
    const char *t = f->text;
    char stack[8][64]; int depth = 0;
    for (int i = 0; t[i]; ) {
        int e = line_end(t, i), s = skip_blank(t, i);
        int cmt = s; while (cmt < e && t[cmt] != '#') cmt++;          /* a # inside quotes is rare in MOOSE inputs */
        if (t[s] == '[') {
            int q = s + 1; while (q < cmt && t[q] != ']') q++;
            char name[64]; int n = q - s - 1; if (n > 63) n = 63; memcpy(name, t + s + 1, (size_t)(n > 0 ? n : 0)); name[n > 0 ? n : 0] = 0;
            if (!strcmp(name, "") || !strcmp(name, "../")) { if (depth > 0) depth--; }
            else if (depth < 8) { const char *nm = name; if (!strncmp(nm, "./", 2)) nm += 2; pl_strlcpy(stack[depth++], nm, sizeof stack[0]); }
        } else if (depth > 0) {
            int eq = s; while (eq < cmt && t[eq] != '=') eq++;
            if (eq < cmt && eq > s) {
                char key[96]; int kl = eq - s; while (kl > 0 && (t[s + kl - 1] == ' ' || t[s + kl - 1] == '\t')) kl--;
                if (kl > 0 && kl < 95) {
                    memcpy(key, t + s, (size_t)kl); key[kl] = 0;
                    char group[128] = ""; int gl = 0;
                    for (int d = 0; d < depth; d++) cat_at(group, (int)sizeof group, &gl, d ? " / " : "", stack[d]);
                    int vs = skip_blank(t, eq + 1), ve = cmt;
                    if (t[vs] == '\'' || t[vs] == '"') {                /* quoted: edit inside the quotes */
                        char qc = t[vs]; int qe = vs + 1; while (qe < e && t[qe] != qc) qe++;
                        if (qe < e) { vs++; ve = qe; }
                    }
                    add(f, group, key, vs, ve - vs);
                }
            }
        }
        i = t[e] ? e + 1 : e;
    }
}

/* ============================================================================ Elmer */
static void scan_elmer(ParamFile *f)
{
    const char *t = f->text;
    char sect[128] = ""; bool in = false;
    for (int i = 0; t[i]; ) {
        int e = line_end(t, i), s = skip_blank(t, i);
        int cmt = s; bool q = false;
        while (cmt < e && (q || (t[cmt] != '!' && t[cmt] != '#'))) { if (t[cmt] == '"') q = !q; cmt++; }
        if (s < cmt) {
            int eq = s; while (eq < cmt && t[eq] != '=') eq++;
            if (!in) {
                int n = cmt - s; while (n > 0 && (t[s + n - 1] == ' ' || t[s + n - 1] == '\r')) n--;
                if (eq >= cmt && n > 0 && n < 127 && strncasecmp(t + s, "End", 3)) { memcpy(sect, t + s, (size_t)n); sect[n] = 0; in = true; }
            } else if (!strncasecmp(t + s, "End", 3) && (cmt - s == 3 || isspace((unsigned char)t[s + 3]))) in = false;
            else if (eq < cmt) {
                char key[96]; int kl = eq - s; while (kl > 0 && t[s + kl - 1] == ' ') kl--;
                if (kl > 0 && kl < 95) {
                    memcpy(key, t + s, (size_t)kl); key[kl] = 0;
                    char *par = strchr(key, '('); if (par) { *par = 0; int k2 = (int)strlen(key); while (k2 > 0 && key[k2 - 1] == ' ') key[--k2] = 0; }
                    int vs = skip_blank(t, eq + 1);
                    /* "Real 300", "Integer 5", "Logical True", "String x", "File x": the value is after the type word */
                    static const char *const TYPES[] = { "Real ", "Integer ", "Logical ", "String ", "File ", NULL };
                    for (int k = 0; TYPES[k]; k++) if (!strncasecmp(t + vs, TYPES[k], strlen(TYPES[k])) &&
                                                         strncasecmp(t + vs, "Real MATC", 9)) { vs = skip_blank(t, vs + (int)strlen(TYPES[k])); break; }
                    int ve = cmt;
                    if (t[vs] == '"') { int qe = vs + 1; while (qe < cmt && t[qe] != '"') qe++; if (qe < cmt) { vs++; ve = qe; } }
                    if (ve > vs) add(f, sect, key, vs, ve - vs);
                }
            }
        }
        i = t[e] ? e + 1 : e;
    }
}

/* ============================================================================ CalculiX */
static const struct { const char *card; const char *fields[5]; } CCX_FIELDS[] = {
    { "*ELASTIC", { "E", "nu", "temperature", NULL } },
    { "*CONDUCTIVITY", { "k", "temperature", NULL } },
    { "*SPECIFIC HEAT", { "cp", "temperature", NULL } },
    { "*DENSITY", { "rho", "temperature", NULL } },
    { "*EXPANSION", { "alpha", "temperature", NULL } },
    { "*HEAT TRANSFER", { "initial increment", "step period", "min increment", "max increment", NULL } },
    { "*STATIC", { "initial increment", "step period", "min increment", "max increment", NULL } },
    { "*DYNAMIC", { "initial increment", "step period", "min increment", "max increment", NULL } },
    { "*COUPLED TEMPERATURE-DISPLACEMENT", { "initial increment", "step period", "min increment", "max increment", NULL } },
    { "*UNCOUPLED TEMPERATURE-DISPLACEMENT", { "initial increment", "step period", "min increment", "max increment", NULL } },
    { "*FREQUENCY", { "modes", NULL } },
};
static const char *const CCX_LINES[] = { "*CLOAD", "*DLOAD", "*BOUNDARY", "*CFLUX", "*DFLUX", "*FILM", "*RADIATE", "*INITIAL CONDITIONS",
                                         "*AMPLITUDE", "*PHYSICAL CONSTANTS", NULL };
static const char *const CCX_SKIP[] = { "*NODE", "*ELEMENT", "*NSET", "*ELSET", "*SURFACE", "*INCLUDE", "*HEADING", NULL };

static bool card_is(const char *t, int s, const char *card)
{
    size_t n = strlen(card);
    if (strncasecmp(t + s, card, n)) return false;
    char c = t[s + n];
    return c == ',' || c == '\n' || c == '\r' || c == ' ' || c == 0;
}

static void scan_calculix(ParamFile *f)
{
    const char *t = f->text;
    char card[96] = "", material[64] = "";
    int mode = 0, fieldsAt = -1;          /* mode 0 skip data, 1 named fields, 2 whole lines */
    int lineNo = 0;
    for (int i = 0; t[i]; ) {
        int e = line_end(t, i), s = skip_blank(t, i);
        if (t[s] == '*' && t[s + 1] == '*') { i = t[e] ? e + 1 : e; continue; }   /* comment */
        if (t[s] == '*') {
            int ce = s; while (ce < e && t[ce] != ',' && t[ce] != '\r') ce++;
            int n = ce - s; if (n > 95) n = 95;
            memcpy(card, t + s, (size_t)n); card[n] = 0;
            for (char *c = card; *c; c++) *c = (char)toupper((unsigned char)*c);
            while (n > 0 && card[n - 1] == ' ') card[--n] = 0;
            mode = 0; fieldsAt = -1; lineNo = 0;
            if (card_is(t, s, "*MATERIAL")) {
                const char *nm = strcasestr(t + s, "NAME=");
                if (nm && nm < t + e) { int k = 0; nm += 5; while (nm < t + e && *nm != ',' && *nm != '\r' && *nm != '\n' && k < 63) material[k++] = *nm++; material[k] = 0; }
            }
            if (card_is(t, s, "*STEP")) material[0] = 0;
            bool skip = false; for (int k = 0; CCX_SKIP[k]; k++) if (card_is(t, s, CCX_SKIP[k])) skip = true;
            if (!skip) {
                for (size_t k = 0; k < sizeof CCX_FIELDS / sizeof CCX_FIELDS[0]; k++) if (card_is(t, s, CCX_FIELDS[k].card)) { mode = 1; fieldsAt = (int)k; }
                for (int k = 0; CCX_LINES[k]; k++) if (card_is(t, s, CCX_LINES[k])) mode = 2;
                /* the card's own parameters: *STEP, INC=1000 */
                for (int p = ce; p < e; ) {
                    if (t[p] == ',') {
                        int ks = skip_blank(t, p + 1), eq = ks; while (eq < e && t[eq] != '=' && t[eq] != ',') eq++;
                        if (eq < e && t[eq] == '=') {
                            char key[96]; int kl = eq - ks; if (kl > 95) kl = 95; memcpy(key, t + ks, (size_t)kl); key[kl] = 0;
                            int vs = eq + 1, ve = vs; while (ve < e && t[ve] != ',' && t[ve] != '\r') ve++;
                            if (strcasecmp(key, "NAME") && strcasecmp(key, "ELSET") && strcasecmp(key, "NSET") && strcasecmp(key, "INPUT") &&
                                strcasecmp(key, "MATERIAL") && strcasecmp(key, "TYPE"))
                                add(f, card, key, vs, ve - vs);
                            p = ve; continue;
                        }
                    }
                    p++;
                }
            }
        } else if (s < e && mode) {
            char group[128];
            if (material[0] && (mode == 1 && fieldsAt >= 0 && fieldsAt <= 4)) pl_fmt_ui(group, sizeof group, "*MATERIAL %s > %s", material, card);
            else pl_strlcpy(group, card, sizeof group);
            if (mode == 1 && fieldsAt >= 0) {
                int fi = 0;
                for (int p = s; p <= e && fi < 5; ) {
                    int q = p; while (q < e && t[q] != ',' && t[q] != '\r') q++;
                    const char *name = CCX_FIELDS[fieldsAt].fields[fi];
                    if (!name) break;
                    if (q > p || t[q] == ',') { char key[96]; if (lineNo) snprintf(key, sizeof key, "%s (row %d)", name, lineNo + 1); else pl_strlcpy(key, name, sizeof key);
                                                if (q > p) add(f, group, key, p, q - p); }
                    fi++;
                    if (q >= e || t[q] != ',') break;
                    p = q + 1;
                }
                lineNo++;
            } else if (mode == 2) {
                const char *k = !strcmp(card, "*CLOAD") || !strcmp(card, "*DLOAD") || !strcmp(card, "*CFLUX") || !strcmp(card, "*DFLUX") ? "load"
                              : !strcmp(card, "*BOUNDARY") ? "boundary" : "data";
                add(f, group, k, s, e - s);
            }
        }
        i = t[e] ? e + 1 : e;
    }
}

/* ============================================================================ code_aster */
static void scan_aster_export(ParamFile *f)
{
    const char *t = f->text;
    for (int i = 0; t[i]; ) {
        int e = line_end(t, i), s = skip_blank(t, i);
        if (t[s] == 'P' && (t[s + 1] == ' ' || t[s + 1] == '\t')) {
            int ks = skip_blank(t, s + 1), ke = ks; while (ke < e && !isspace((unsigned char)t[ke])) ke++;
            char key[96]; int kl = ke - ks; if (kl > 95) kl = 95; memcpy(key, t + ks, (size_t)kl); key[kl] = 0;
            int vs = skip_blank(t, ke);
            if (vs < e) add(f, "Job", key, vs, e - vs);
        }
        i = t[e] ? e + 1 : e;
    }
}

static void scan_aster_comm(ParamFile *f)
{
    const char *t = f->text;
    char stack[12][64]; int depth = 0;
    char lhs[64] = "";                         /* res = THER_LINEAIRE( ...: "res" */
    char lastIdent[64] = ""; int lastIdentEnd = -1;
    char pendingKw[64] = "";                   /* KEY=  before _F( */
    for (int i = 0; t[i]; i++) {
        char c = t[i];
        if (c == '#') { while (t[i] && t[i] != '\n') i++; if (!t[i]) break; continue; }
        if (c == '\'' || c == '"') {
            int s = i + 1, q = s; while (t[q] && t[q] != c && t[q] != '\n') q++;
            i = t[q] == c ? q : q - 1;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            int s = i; while (isalnum((unsigned char)t[i]) || t[i] == '_') i++;
            int n = i - s; if (n > 63) n = 63;
            memcpy(lastIdent, t + s, (size_t)n); lastIdent[n] = 0; lastIdentEnd = i;
            int p = skip_blank(t, i);
            if (t[p] == '=' && t[p + 1] != '=') {
                bool upper = true; for (int k = 0; lastIdent[k]; k++) if (islower((unsigned char)lastIdent[k])) upper = false;
                int vs = skip_blank(t, p + 1);
                if (depth == 0) pl_strlcpy(lhs, lastIdent, sizeof lhs);
                else if (upper) {
                    /* KEY=_F( ... ) opens a keyword group; KEY=<number> or KEY='text' is a value */
                    if (!strncmp(t + vs, "_F", 2) && t[skip_blank(t, vs + 2)] == '(') pl_strlcpy(pendingKw, lastIdent, sizeof pendingKw);
                    else if (isdigit((unsigned char)t[vs]) || ((t[vs] == '-' || t[vs] == '+' || t[vs] == '.') && (isdigit((unsigned char)t[vs + 1]) || t[vs + 1] == '.')) ||
                             t[vs] == '\'' || t[vs] == '"') {
                        int vstart = vs, vend, after;
                        if (t[vs] == '\'' || t[vs] == '"') { char qc = t[vs]; vend = vs + 1; while (t[vend] && t[vend] != qc && t[vend] != '\n') vend++;
                                                             if (t[vend] == qc) { vstart = vs + 1; after = vend + 1; } else { vend = vs; after = vs; } }
                        else { char *e; strtod(t + vs, &e); vend = (int)(e - t); after = vend; }
                        if (vend > vstart) {
                            char group[128], key[96];
                            snprintf(group, sizeof group, "%s%s%s", lhs[0] && depth > 0 ? lhs : "", lhs[0] && depth > 0 ? " = " : "", depth > 0 ? stack[0] : "");
                            key[0] = 0; int kl = 0;
                            for (int d = 1; d < depth && d < 12; d++) cat_at(key, (int)sizeof key, &kl, stack[d], " > ");
                            cat_at(key, (int)sizeof key, &kl, lastIdent, "");
                            add(f, group, key, vstart, vend - vstart);
                        }
                        i = after > vs ? after : vs;
                    }
                }
            }
            i--;
            continue;
        }
        if (c == '(') {
            int p = i - 1; while (p >= 0 && (t[p] == ' ' || t[p] == '\t')) p--;
            const char *name = (lastIdentEnd >= 0 && p == lastIdentEnd - 1) ? lastIdent : "(";
            if (!strcmp(name, "_F") && pendingKw[0]) name = pendingKw;
            if (depth < 12) pl_strlcpy(stack[depth], name, sizeof stack[0]);
            depth++;
            pendingKw[0] = 0;
            continue;
        }
        if (c == ')') { if (depth > 0) depth--; if (depth == 0) lhs[0] = 0; continue; }
        if (c == '\n' && depth == 0) lhs[0] = 0;
    }
}

/* ============================================================================ code_saturne XML */
static void scan_xml(ParamFile *f)
{
    const char *t = f->text;
    char stack[16][64]; int depth = 0;
    for (int i = 0; t[i]; i++) {
        if (t[i] != '<') continue;
        if (t[i + 1] == '?' || t[i + 1] == '!') { const char *e = strchr(t + i, '>'); if (!e) break; i = (int)(e - t); continue; }
        if (t[i + 1] == '/') { if (depth > 0) depth--; const char *e = strchr(t + i, '>'); if (!e) break; i = (int)(e - t); continue; }
        int ns = i + 1, ne = ns; while (t[ne] && !isspace((unsigned char)t[ne]) && t[ne] != '>' && t[ne] != '/') ne++;
        char name[64]; int n = ne - ns; if (n > 63) n = 63; memcpy(name, t + ns, (size_t)n); name[n] = 0;
        const char *gt = strchr(t + ne, '>'); if (!gt) break;
        bool selfClose = gt[-1] == '/';
        /* name="..." / label="..." identify repeated elements: <variable name="temperature"> */
        char ident[64] = "";
        const char *na = strstr(t + ne, "name=\""); if (!na || na > gt) na = strstr(t + ne, "label=\"");
        if (na && na < gt) { const char *q = strchr(na, '"') + 1; int k = 0; while (*q && *q != '"' && k < 63) ident[k++] = *q++; ident[k] = 0; }
        int body = (int)(gt - t) + 1;
        if (!selfClose) {
            const char *lt = strchr(t + body, '<');
            if (lt && lt[1] == '/' && !strncmp(lt + 2, name, (size_t)n)) {            /* a leaf: <name>text</name> */
                int vs = body, ve = (int)(lt - t);
                char group[128] = ""; int gl = 0;
                int from = depth > 2 ? depth - 2 : 0;
                for (int d = from; d < depth && d < 16; d++) cat_at(group, (int)sizeof group, &gl, d > from ? " / " : "", stack[d]);
                char key[96]; if (ident[0]) pl_fmt_ui(key, sizeof key, "%s (%s)", name, ident); else pl_strlcpy(key, name, sizeof key);
                bool blank = true; for (int k = vs; k < ve; k++) if (!isspace((unsigned char)t[k])) blank = false;
                if (!blank) add(f, group, key, vs, ve - vs);
                i = ve - 1;
                continue;
            }
            if (depth < 16) { if (ident[0]) pl_fmt_ui(stack[depth], sizeof stack[0], "%s %s", name, ident); else pl_strlcpy(stack[depth], name, sizeof stack[0]); }
            depth++;
        }
        i = (int)(gt - t);
    }
}

/* ============================================================================ SPARTA */
static void scan_sparta(ParamFile *f)
{
    const char *t = f->text;
    for (int i = 0; t[i]; ) {
        int e = line_end(t, i), s = skip_blank(t, i);
        int cmt = s; while (cmt < e && t[cmt] != '#') cmt++;
        if (s < cmt && isalpha((unsigned char)t[s])) {
            int ke = s; while (ke < cmt && !isspace((unsigned char)t[ke])) ke++;
            char key[96]; int kl = ke - s; if (kl > 95) kl = 95; memcpy(key, t + s, (size_t)kl); key[kl] = 0;
            int vs = skip_blank(t, ke);
            if (vs < cmt && t[cmt - 1] != '&') add(f, "Commands", key, vs, cmt - vs);
        }
        i = t[e] ? e + 1 : e;
    }
}

/* ============================================================================ .ini */
static void scan_ini(ParamFile *f)
{
    const char *t = f->text;
    char sect[128] = "";
    for (int i = 0; t[i]; ) {
        int e = line_end(t, i), s = skip_blank(t, i);
        if (t[s] == '[') { int q = s + 1; while (q < e && t[q] != ']') q++; int n = q - s - 1; if (n > 127) n = 127; if (n > 0) { memcpy(sect, t + s + 1, (size_t)n); sect[n] = 0; } }
        else if (t[s] != '#' && t[s] != ';') {
            int eq = s; while (eq < e && t[eq] != '=') eq++;
            if (eq < e && eq > s) {
                char key[96]; int kl = eq - s; while (kl > 0 && t[s + kl - 1] == ' ') kl--; if (kl > 95) kl = 95;
                memcpy(key, t + s, (size_t)kl); key[kl] = 0;
                int cmt = eq + 1; while (cmt < e && t[cmt] != '#' && t[cmt] != ';') cmt++;
                add(f, sect, key, eq + 1, cmt - eq - 1);
            }
        }
        i = t[e] ? e + 1 : e;
    }
}

/* ============================================================================ file */
long long params_stamp(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec + st.st_size;
}

void params_rescan(ParamFile *f)
{
    f->n = 0;
    if (!f->text) return;
    const char *b = pl_basename(f->path);
    switch (f->prog) {
    case PROG_MOOSE: scan_moose(f); break;
    case PROG_ELMER: scan_elmer(f); break;
    case PROG_CALCULIX: scan_calculix(f); break;
    case PROG_ASTER: if (pl_ends_with(b, ".export")) scan_aster_export(f); else scan_aster_comm(f); break;
    case PROG_SATURNE: if (pl_ends_with(b, ".xml")) scan_xml(f); else scan_ini(f); break;
    case PROG_SPARTA: scan_sparta(f); break;
    default: scan_ini(f); break;
    }
}

bool params_from_text(ParamFile *f, int prog, const char *path, const char *text)
{
    params_free(f);
    pl_strlcpy(f->path, path, sizeof f->path);
    f->prog = prog;
    f->text = pl_strdup(text);
    f->len = (int)strlen(text);
    f->orig = pl_strdup(text);
    params_rescan(f);
    return true;
}

bool params_load(ParamFile *f, int prog, const char *path)
{
    size_t len = 0;
    char *t = pl_read_file(path, &len);
    if (!t) { params_free(f); return false; }
    params_from_text(f, prog, path, t);
    free(t);
    f->stamp = params_stamp(path);
    return true;
}

void params_free(ParamFile *f)
{
    free(f->text); free(f->p); free(f->orig);
    memset(f, 0, sizeof *f);
}

bool params_set(ParamFile *f, int i, const char *value)
{
    if (i < 0 || i >= f->n || !f->p[i].editable || strchr(value, '\n')) return false;
    Param *p = &f->p[i];
    if (!strcmp(p->value, value)) return true;
    int vl = (int)strlen(value);
    char *nt = malloc((size_t)f->len - (size_t)p->len + (size_t)vl + 1);
    if (!nt) return false;
    memcpy(nt, f->text, (size_t)p->start);
    memcpy(nt + p->start, value, (size_t)vl);
    int tail = f->len - p->start - p->len + 1;
    memcpy(nt + p->start + vl, f->text + p->start + p->len, (size_t)tail);
    free(f->text);
    f->text = nt;
    f->len = f->len - p->len + vl;
    f->dirty = strcmp(f->text, f->orig ? f->orig : "") != 0;
    params_rescan(f);
    return true;
}

bool params_save(ParamFile *f)
{
    if (!f->text || !pl_write_file(f->path, f->text, (size_t)f->len)) return false;
    free(f->orig); f->orig = pl_strdup(f->text);
    f->dirty = false;
    f->stamp = params_stamp(f->path);
    return true;
}

bool params_changed_on_disk(const ParamFile *f) { return f->path[0] && params_stamp(f->path) != f->stamp; }

int params_find(const ParamFile *f, const char *group, const char *key)
{
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->p[i].key, key) && (!group || !strcmp(f->p[i].group, group))) return i;
    return -1;
}
