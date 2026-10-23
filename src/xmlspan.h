/* xmlspan.h -- minimal XML reader that keeps byte spans, for preCICE configuration files.
 *
 * Like foamdict, edits replace only the span of an attribute value (or insert/remove whole elements),
 * so comments, ordering and layout survive. The text lives in an FDFile (dirty tracking, atomic save,
 * the text editor); the element tree is re-read from that text after every edit.
 */
#ifndef XMLSPAN_H
#define XMLSPAN_H

#include <stdbool.h>
#include "foamdict.h"

#define XML_MAX_ATTR 24

typedef struct { char name[64]; int vs, ve; } XAttr;          /* value span, without the quotes */

typedef struct XNode {
    char  tag[64];                     /* e.g. "coupling-scheme:serial-implicit"            */
    int   start, end;                  /* whole element: '<' .. after '>' of the end tag    */
    int   openEnd;                     /* position of '>' (or of '/' in '/>') of start tag  */
    int   closeStart;                  /* '<' of "</tag>", or -1 when self-closing          */
    XAttr attr[XML_MAX_ATTR]; int nAttr;
    struct XNode *child; int nChild, capChild;
} XNode;

typedef struct { XNode root; bool ok; char err[160]; } XTree;   /* root: a pseudo element holding the top level */

bool   xml_parse(const char *text, int len, XTree *t);
void   xml_free(XTree *t);

XNode *xml_child(XNode *n, const char *tag, int nth);           /* nth child with this tag (tag "" = any) */
int    xml_count(const XNode *n, const char *tag);
bool   xml_attr(const char *text, const XNode *n, const char *name, char *buf, int cap);
bool   xml_tag_is(const XNode *n, const char *prefix);           /* "coupling-scheme:" matches every scheme */

/* edits: change f's text and re-read *t; node pointers are invalid afterwards */
bool   xml_set_attr(FDFile *f, XTree *t, XNode *n, const char *name, const char *value);
bool   xml_append_child(FDFile *f, XTree *t, XNode *parent, const char *elementText);   /* indented like its siblings */
bool   xml_remove(FDFile *f, XTree *t, XNode *n);
bool   xml_rename(FDFile *f, XTree *t, XNode *n, const char *newTag);

#endif
