/* test_params <tests/programs> <scratch>: the Setup page's scanners (params.c) over real inputs and the starters.
 * Each file shows the values one would change (end time, time step, materials), every span holds exactly its value,
 * and an edit changes only those bytes: the rest of the file comes back byte-identical. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "params.h"
#include "program.h"
#include "pl_util.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { int prog; const char *file; const char *group; const char *key; const char *value; const char *newValue; } Want;

static void one(const Want *w, const char *root)
{
    char p[1024]; pl_join(p, sizeof p, root, w->file);
    ParamFile f = { 0 };
    CHECK(params_load(&f, w->prog, p), "load %s", p);
    int basic = 0;
    for (int i = 0; i < f.n; i++) {
        CHECK(f.p[i].start >= 0 && f.p[i].start + f.p[i].len <= f.len, "%s: span out of the text", w->file);
        if (f.p[i].editable) CHECK(!strncmp(f.text + f.p[i].start, f.p[i].value, (size_t)f.p[i].len) && (int)strlen(f.p[i].value) == f.p[i].len,
                                   "%s: %s/%s span '%.*s' != value '%s'", w->file, f.p[i].group, f.p[i].key, f.p[i].len, f.text + f.p[i].start, f.p[i].value);
        if (f.p[i].importance == 2) basic++;
    }
    printf("%-28s %3d values, %2d basic\n", w->file, f.n, basic);
    int k = params_find(&f, w->group, w->key);
    if (k < 0) {
        for (int i = 0; i < f.n && i < 400; i++) printf("    [%s] %s = %s\n", f.p[i].group, f.p[i].key, f.p[i].value);
    }
    CHECK(k >= 0, "%s: no [%s] %s", w->file, w->group ? w->group : "*", w->key);
    if (k >= 0) {
        CHECK(!strcmp(f.p[k].value, w->value), "%s: %s = '%s', want '%s'", w->file, w->key, f.p[k].value, w->value);
        char *before = pl_strdup(f.text);
        int start = f.p[k].start, len = f.p[k].len;
        CHECK(params_set(&f, k, w->newValue), "%s: set %s", w->file, w->key);
        int nl = (int)strlen(w->newValue);
        CHECK(!memcmp(f.text, before, (size_t)start) && !strcmp(f.text + start + nl, before + start + len) && !strncmp(f.text + start, w->newValue, (size_t)nl),
              "%s: an edit of %s touched other bytes", w->file, w->key);
        int k2 = params_find(&f, w->group, w->key);
        CHECK(k2 >= 0 && !strcmp(f.p[k2].value, w->newValue), "%s: after the edit %s = '%s'", w->file, w->key, k2 >= 0 ? f.p[k2].value : "-");
        CHECK(f.dirty, "%s: not dirty", w->file);
        CHECK(params_set(&f, k2, w->value) && !strcmp(f.text, before) && !f.dirty, "%s: setting it back does not restore the file", w->file);
        free(before);
    }
    params_free(&f);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const char *root = argv[1], *scratch = argv[2];
    static const Want W[] = {
        { PROG_MOOSE,    "moose/A.i",                   "Executioner", "end_time", "250", "500" },
        { PROG_MOOSE,    "moose/A.i",                   "Mesh / gen", "nx", "50", "100" },
        { PROG_ELMER,    "elmer/case.sif",              "Simulation", "Timestep Sizes", "0.5", "0.25" },
        { PROG_ELMER,    "elmer/case.sif",              "Material 1", "Heat Conductivity", "16.2", "20" },
        { PROG_CALCULIX, "calculix/A.inp",              NULL, "step period", "250.0", "500.0" },
        { PROG_CALCULIX, "calculix/A.inp",              NULL, "k", "16.2", "15" },
        { PROG_ASTER,    "aster/A.comm",                NULL, "INTERVALLE > JUSQU_A", "250.", "100." },
        { PROG_ASTER,    "aster/A.export",              "Job", "memory_limit", "2048", "4096" },
        { PROG_SPARTA,   "sparta/in.circle",            "Commands", "timestep", "0.0001", "0.0002" },
        { PROG_SPARTA,   "sparta/in.circle",            "Commands", "run", "1000", "5000" },
    };
    for (size_t i = 0; i < sizeof W / sizeof W[0]; i++) one(&W[i], root);

    /* the starters (written by test_program into <scratch>/tpl-*) */
    static const Want T[] = {
        { PROG_MOOSE,    "tpl-moose-heat/heat.i",        "Materials / steel", "prop_values", "16.2 500 8000", "15 500 8000" },
        { PROG_CALCULIX, "tpl-calculix-beam/beam.inp",   "*MATERIAL STEEL > *ELASTIC", "E", "210000e6", "70000e6" },
        { PROG_CALCULIX, "tpl-calculix-beam/beam.inp",   "*CLOAD", "load", "TIP, 3, -40.", "TIP, 3, -80." },
        { PROG_CALCULIX, "tpl-calculix-slab/slab.inp",   "*STEP", "INC", "1000", "2000" },
        { PROG_ELMER,    "tpl-elmer-heat/case.sif",      "Body Force 1", "Heat Source", "1.0", "2.0" },
        { PROG_SPARTA,   "tpl-sparta-plume/in.wedge",    "Commands", "mixture", "gas N2 vstream 1500.0 0 0 temp 300.0", "gas N2 vstream 2500.0 0 0 temp 300.0" },
        { PROG_ASTER,    "tpl-aster-slab/slab.comm",     "steel = DEFI_MATERIAU", "THER > LAMBDA", "16.2", "20.0" },
        { PROG_ASTER,    "tpl-aster-slab/slab.comm",     NULL, "THER_IMPO > TEMP", "400.0", "500.0" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) one(&T[i], scratch);

    /* code_saturne XML leaves */
    {
        const char *x = "<?xml version=\"1.0\"?>\n<Code_Saturne_GUI>\n <analysis_control>\n  <time_parameters>\n"
                        "   <iterations>10</iterations>\n   <time_step_ref>0.1</time_step_ref>\n  </time_parameters>\n </analysis_control>\n"
                        " <physical_properties><fluid_properties><property label=\"Density\" name=\"density\"><initial_value>1.17862</initial_value>"
                        "</property></fluid_properties></physical_properties>\n</Code_Saturne_GUI>\n";
        ParamFile f = { 0 };
        params_from_text(&f, PROG_SATURNE, "setup.xml", x);
        int a = params_find(&f, NULL, "iterations"), b = params_find(&f, NULL, "initial_value");
        CHECK(a >= 0 && !strcmp(f.p[a].value, "10") && f.p[a].importance == 2, "saturne iterations");
        CHECK(b >= 0 && strstr(f.p[b].group, "density"), "saturne density group '%s'", b >= 0 ? f.p[b].group : "-");
        params_free(&f);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
