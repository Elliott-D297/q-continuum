/* fieldinfo.h -- what an OpenFOAM field means (ROADMAP 2.5): units from its dimensions, a description, typical values,
   whether the turbulence model uses it, and the values the shipped tutorials start it at */
#ifndef FIELDINFO_H
#define FIELDINFO_H

#include <stdbool.h>

typedef struct {
    const char *name;            /* field name, or a prefix ending in '.' ("alpha.") */
    const char *meaning;
    const char *typical;         /* typical values, in words */
} FieldInfo;

/* "[0 2 -3 0 0 0 0]" -> "m²/s³" (named units where common: Pa, W/m², kg/m³); false if it isn't a dimension set */
bool dims_unit(const char *dims, char *out, int cap);
const FieldInfo *field_info(const char *name);            /* NULL for fields it doesn't know */
/* the fields a turbulence model reads ("k", "omega", "nut"); returns how many (at most 6) */
int  turb_model_fields(const char *model, const char **out);
/* 1 the case's turbulence model uses the field, 0 it doesn't (omega under kEpsilon, k in a laminar run),
   -1 not a turbulence field or not known. simType "laminar" / "RAS" / "LES", model as in the dictionary. */
int  turb_field_used(const char *field, const char *simType, const char *model);

/* the tutorials' initial values per field (internalField uniform; vectors by magnitude), scanned once in the
   background and cached; false until ready or when the field never appears with a uniform value */
typedef struct { int n; double min, median, max; } TutRange;
void tut_ranges_start(const char *tutorialsDir, const char *cacheFile);
bool tut_range(const char *field, TutRange *out);
/* the scan itself (the background child, and tests): writes "field n min median max" lines; returns fields found */
int  tut_ranges_scan(const char *tutorialsDir, const char *outFile);

/* a value the user typed, before it is written (bug hunt H-01, H-07). NULL when it may go in, else why not:
   - never ';', '{' or '}' outside quotes, and brackets and quotes balanced (one value must not become several entries);
   - a key that held a number takes a number (or a $macro / #directive), finite;
   - time steps, intervals and Courant limits above 0; corrector counts whole numbers >= 0;
   - field values ("value", "inletValue", "internalField", ...) fit the field class: uniform x, uniform (x y z).
   key: the entry's own name; oldv: its value now ("" if new); cls: FoamFile class of the file ("" if none). */
const char *value_problem(const char *key, const char *oldv, const char *newv, const char *cls);
/* "uniform X" that doesn't fit the field class ("vol/surface/point" + Scalar/Vector/Tensor...); NULL when fine */
const char *field_value_problem(const char *cls, const char *raw);

/* the dimensions a field must have whatever the solver ("U" -> "[0 1 -1 0 0 0 0]"); NULL when it depends (p) or isn't known */
const char *field_expected_dims(const char *name);
/* two dimension sets equal, read as numbers ("[0 1 -1 0 0 0 0]" == "[0 1 -1 0 0 0  0]"); 5-entry sets too */
bool dims_equal(const char *a, const char *b);

#endif
