/* qjson.h -- a small JSON reader, for the answers of assets/salome/salome_tool.py (the SALOME service): objects, arrays,
 * strings (UTF-8, \uXXXX escapes), numbers, true/false/null. A parsed document is one block of nodes; free it with qj_free. */
#ifndef QJSON_H
#define QJSON_H

#include <stdbool.h>

typedef enum { QJ_NULL = 0, QJ_BOOL, QJ_NUM, QJ_STR, QJ_ARR, QJ_OBJ } QjType;

typedef struct QJ QJ;
struct QJ {
    QjType type;
    double num;                 /* QJ_NUM; QJ_BOOL: 0 or 1      */
    char  *str;                 /* QJ_STR; an object member: its key in key */
    char  *key;                 /* the member's key (in an object) */
    QJ    *child, *next;        /* QJ_ARR / QJ_OBJ: the first element; the next sibling */
    int    n;                   /* QJ_ARR / QJ_OBJ: how many    */
};

QJ         *qj_parse(const char *text, char *err, int cap);   /* NULL on a syntax error (err says where) */
void        qj_free(QJ *j);
const QJ   *qj_get(const QJ *obj, const char *key);           /* a member, NULL if missing or not an object */
const QJ   *qj_at(const QJ *arr, int i);                       /* an element, NULL out of range */
double      qj_num(const QJ *j, double def);                   /* a number (true 1, false 0), else def */
const char *qj_str(const QJ *j, const char *def);              /* a string, else def */
bool        qj_true(const QJ *j);                              /* true, or a non-zero number */
int         qj_len(const QJ *j);                               /* elements of an array or object, else 0 */

#endif
