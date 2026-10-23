/* foamdict.h -- OpenFOAM dictionary parser that keeps the original text.
 *
 * Every entry remembers the byte span of its key and value in the file text. Edits splice
 * new text into exactly that span and re-parse, so comments, alignment and anything the
 * parser does not understand (#include, $macros, #codeStream ...) survive untouched.
 */
#ifndef FOAMDICT_H
#define FOAMDICT_H

#include <stdbool.h>
#include <time.h>
#include "pl_util.h"

typedef enum { FD_VALUE = 0, FD_DICT, FD_DIRECTIVE } FDKind;

typedef struct FDEntry {
    char   *key;                 /* raw keyword text, e.g. div(phi,U) or "(U|k)"         */
    FDKind  kind;
    int     start, end;          /* whole entry [start, end) including ';' or '}'          */
    int     keyStart, keyEnd;
    int     valStart, valEnd;    /* VALUE: text before ';'   DICT: text inside the braces  */
    int     line;
    struct FDEntry *child;       /* DICT children; also dicts found inside a "( ... )"      */
    int     nChild, capChild;    /*   list value (e.g. blockMeshDict boundary)              */
} FDEntry;

/* which version of a file is on disk: whole-second mtimes miss a change made within the same second */
typedef struct { long long sec, nsec, size, ino; } FDSig;

typedef struct {
    char    path[PL_PATH];
    char   *text;
    int     len;
    FDEntry root;
    bool    ok;                  /* parsed without structural errors                       */
    bool    dirty;               /* edited in memory, not yet saved                         */
    bool    binary;              /* holds NUL bytes (writeFormat binary): read-only, the text
                                    stops at the first NUL so saving would truncate the file   */
    char    err[200];
    time_t  mtime;               /* on-disk mtime (seconds) when loaded / saved             */
    FDSig   sig;                 /* the disk version the text is based on                   */
    char   *base;                /* that version's text: the user's edits are text - base   */
    bool    conflict;            /* the disk changed an entry the user also changed: ask     */
    bool    gone;                /* deleted on disk since it was read (reported once; Save writes it again) */
    char   *diskText;            /*   the disk version, for showing both                     */
    FDSig   diskSig;
} FDFile;

enum { FD_SYNC_NONE = 0, FD_SYNC_RELOADED, FD_SYNC_REBASED, FD_SYNC_CONFLICT, FD_SYNC_GONE };

FDFile *fd_load(const char *path);                 /* NULL if the file cannot be read */
FDFile *fd_from_text(const char *path, const char *text);
void    fd_free(FDFile *f);
bool    fd_reload(FDFile *f);
bool    fd_save(FDFile *f);                        /* refuses a conflict; takes in disk changes first */
bool    fd_save_as(const FDFile *f, const char *path);   /* a copy elsewhere; the file itself stays as it is */
bool    fd_changed_on_disk(const FDFile *f);
/* bring in a change on disk: a clean file reloads; an edited one takes the disk version with the user's entry edits
   replayed onto it (rebase); when the same entry changed on both sides, or the edit isn't a set of entry edits (raw
   text), the file is marked as a conflict and keeps the user's text. Saving a conflicted file is refused. */
int     fd_sync(FDFile *f);
void    fd_resolve(FDFile *f, bool keepMine);    /* keep mine: next save overwrites the disk; else take the disk version */
bool    fd_sig(const char *path, FDSig *out);

/* paths use '/' between keys: "boundaryField/inlet/type" */
FDEntry *fd_find(FDFile *f, const char *path);
FDEntry *fd_child(FDEntry *dict, const char *key);
bool     fd_get(FDFile *f, const char *path, char *buf, int cap);      /* value text, trimmed   */
bool     fd_entry_value(const FDFile *f, const FDEntry *e, char *buf, int cap);
bool     fd_put(FDFile *f, const char *path, const char *value);       /* create if missing     */
/* fd_get, following a "$name" value to the top-level entry it names (one level; "deltaT $dtBase;") */
bool     fd_get_resolved(FDFile *f, const char *path, char *out, int cap);
bool     fd_put_entry(FDFile *f, FDEntry *e, const char *value);       /* replaces e's value    */
bool     fd_ensure_dict(FDFile *f, const char *path);                  /* create empty dict     */
bool     fd_remove(FDFile *f, const char *path);
bool     fd_replace_span(FDFile *f, int a, int b, const char *text);  /* low level, re-parses  */
void     fd_set_text(FDFile *f, const char *text);                    /* replace everything    */
/* called after every edit (fd_set_text, fd_replace_span and so everything built on them) with the text before it:
   the app's undo journal. Edits of scratch copies (rebase) call it too; the hook ignores files it doesn't know. */
extern void (*fd_edit_hook)(FDFile *f, const char *before);
extern unsigned long fd_edit_count;      /* bumped by every edit: caches of anything computed from dictionaries */

/* value helpers (operate on value text) */
bool fd_number_span(const char *val, int *a, int *b);      /* last plain number token        */
bool fd_word(const char *val, char *buf, int cap);         /* first word token               */
bool fd_is_multiline(const char *val);

#endif
