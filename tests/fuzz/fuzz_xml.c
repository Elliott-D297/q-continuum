/* fuzz_xml.c -- libFuzzer harness for the span-keeping XML parser of precice-config.xml */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "xmlspan.h"

static void walk(const XNode *n, int len, int depth)
{
    if (depth > 200) return;
    for (int i = 0; i < n->nChild; i++) walk(&n->child[i], len, depth + 1);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *t = malloc(size + 1);
    memcpy(t, data, size); t[size] = 0;
    XTree tree;
    memset(&tree, 0, sizeof tree);
    xml_parse(t, (int)size, &tree);
    walk(&tree.root, (int)size, 0);
    char buf[256];
    XNode *c = xml_child(&tree.root, "", 0);
    if (c) xml_attr(t, c, "name", buf, sizeof buf);
    xml_free(&tree);
    free(t);
    return 0;
}
