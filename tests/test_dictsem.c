/* test_dictsem.c -- dictionary lookups follow OpenFOAM's rules, so what the app shows and edits is what OpenFOAM uses:
 * a later entry overrides an earlier one with the same keyword, and two dictionaries with the same keyword merge
 * (the later one's entries win). Found by the parser oracle: injectorPipe's controlDict sets endTime 0.4 and later
 * endTime 1200, and the app showed (and edited) the 0.4 OpenFOAM never uses.
 */
#include <stdio.h>
#include <string.h>
#include "foamdict.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    {   /* bug hunt H-04: a counted word list "2(sideWalls wall)" inside polyMesh/boundary closed the patch list early */
        FDFile *b = fd_from_text("boundary", "2\n(\n    a\n    {\n        type wall;\n        inGroups 2(sideWalls wall);\n        nFaces 60;\n    }\n"
                                              "    b\n    {\n        type empty;\n        inGroups 1(empty);\n        nFaces 800;\n    }\n)\n");
        char v[64] = "";
        CHECK(b && b->root.nChild == 2, "boundary has %d patches, want 2", b ? b->root.nChild : -1);
        fd_get(b, "a/nFaces", v, sizeof v); CHECK(!strcmp(v, "60"), "a/nFaces reads '%s'", v);
        fd_get(b, "a/inGroups", v, sizeof v); CHECK(!strcmp(v, "2(sideWalls wall)"), "a/inGroups reads '%s'", v);
        fd_get(b, "b/nFaces", v, sizeof v); CHECK(!strcmp(v, "800"), "b/nFaces reads '%s'", v);
        fd_free(b);
    }

    const char *t =
        "endTime 0.4;\n"
        "deltaT 1e-4;\n"
        "a { x 1; y 2; }\n"
        "endTime 1200;\n"
        "a { y 3; z 4; }\n"
        "b { c { k 1; } }\n"
        "b { c { k 2; } }\n";
    FDFile *f = fd_from_text("", t);
    char v[64];
    fd_get(f, "endTime", v, sizeof v);  CHECK(!strcmp(v, "1200"), "endTime reads %s, OpenFOAM uses the later 1200", v);
    fd_get(f, "a/x", v, sizeof v);      CHECK(!strcmp(v, "1"), "a/x reads %s (only in the first a)", v);
    fd_get(f, "a/y", v, sizeof v);      CHECK(!strcmp(v, "3"), "a/y reads %s, the later a overrides it with 3", v);
    fd_get(f, "a/z", v, sizeof v);      CHECK(!strcmp(v, "4"), "a/z reads %s", v);
    fd_get(f, "b/c/k", v, sizeof v);    CHECK(!strcmp(v, "2"), "b/c/k reads %s, nested merge gives 2", v);
    /* editing changes the entry OpenFOAM uses, and only that one */
    fd_put(f, "endTime", "5");
    CHECK(strstr(f->text, "endTime 0.4;") && strstr(f->text, "endTime 5;"), "the edit went to the wrong endTime:\n%s", f->text);
    fd_put(f, "a/x", "9");
    CHECK(strstr(f->text, "a { x 9; y 2; }"), "a/x should be edited where it is:\n%s", f->text);
    fd_free(f);
    printf("%s: %d failure%s\n", fails ? "FAILED" : "passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
