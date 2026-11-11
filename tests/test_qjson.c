/* test_qjson.c -- the JSON reader the SALOME service's answers go through (src/qjson.c) */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "qjson.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    char err[200];
    QJ *j = qj_parse("{\"ok\": true, \"faces\": [{\"id\": 3, \"area\": 1.5e-3, \"centre\": [0, -2.5, 1e2], \"kind\": \"PLANE\"},"
                     " {\"id\": 4, \"kind\": \"caf\\u00e9 \\\"q\\\"\\n\"}], \"n\": null, \"neg\": -0.25, \"empty\": {}, \"e2\": []}", err, sizeof err);
    CHECK(j, "parse: %s", err);
    if (j) {
        CHECK(qj_true(qj_get(j, "ok")), "ok");
        const QJ *f = qj_get(j, "faces");
        CHECK(qj_len(f) == 2, "faces %d", qj_len(f));
        CHECK(qj_num(qj_get(qj_at(f, 0), "id"), -1) == 3, "id");
        CHECK(fabs(qj_num(qj_get(qj_at(f, 0), "area"), 0) - 1.5e-3) < 1e-15, "area");
        CHECK(qj_num(qj_at(qj_get(qj_at(f, 0), "centre"), 2), 0) == 100, "centre z");
        CHECK(!strcmp(qj_str(qj_get(qj_at(f, 1), "kind"), ""), "caf\xc3\xa9 \"q\"\n"), "escapes: [%s]", qj_str(qj_get(qj_at(f, 1), "kind"), ""));
        CHECK(qj_get(j, "n") && qj_get(j, "n")->type == QJ_NULL, "null");
        CHECK(qj_num(qj_get(j, "neg"), 0) == -0.25, "neg");
        CHECK(qj_len(qj_get(j, "empty")) == 0 && qj_get(j, "empty")->type == QJ_OBJ, "empty object");
        CHECK(qj_len(qj_get(j, "e2")) == 0 && qj_get(j, "e2")->type == QJ_ARR, "empty array");
        CHECK(!qj_get(j, "missing") && !qj_at(f, 5) && qj_num(NULL, 7) == 7 && !strcmp(qj_str(NULL, "d"), "d"), "missing");
        qj_free(j);
    }
    /* Python's json.dumps writes NaN for a failed measure: read, not refused */
    j = qj_parse("[NaN, Infinity, -Infinity, 1]", err, sizeof err);
    CHECK(j && qj_len(j) == 4 && isnan(qj_num(qj_at(j, 0), 0)) && qj_num(qj_at(j, 1), 0) > 1e300, "NaN: %s", err);
    qj_free(j);
    static const char *bad[] = { "{\"a\" 1}", "[1, 2", "{\"a\": tru}", "\"open", "[1,]x", "{} {}", "", NULL };
    for (int i = 0; bad[i]; i++) {
        QJ *b = qj_parse(bad[i], err, sizeof err);
        CHECK(!b && err[0], "accepted bad JSON: %s", bad[i]);
        qj_free(b);
    }
    char deep[300]; memset(deep, '[', 100); deep[100] = 0;
    CHECK(!qj_parse(deep, err, sizeof err), "nesting limit");
    printf(fails ? "test_qjson: %d failures\n" : "test_qjson: all passed\n", fails);
    return fails != 0;
}
