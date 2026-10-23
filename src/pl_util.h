/* pl_util.h -- small POSIX helpers: strings, paths, files, directories, processes */
#ifndef PL_UTIL_H
#define PL_UTIL_H

#include <stddef.h>
#include <stdbool.h>
#include <time.h>
#include <sys/types.h>

#define PL_PATH 4096   /* PATH_MAX: realpath() needs this much */

/* ---------- strings ---------- */
typedef struct { char *s; size_t len, cap; } Str;
void  str_init(Str *b);
void  str_free(Str *b);
void  str_cat(Str *b, const char *s);
void  str_catn(Str *b, const char *s, size_t n);
void  str_catf(Str *b, const char *fmt, ...);

/* snprintf that says whether everything fitted: false on truncation. For commands and paths, where a cut-off string
   would run the wrong thing or name the wrong file, the caller refuses instead. */
bool  pl_fmt(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* snprintf for text shown on screen, where clipping is harmless (the text is clipped when drawn anyway) */
void  pl_fmt_ui(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

char *pl_strdup(const char *s);
char *pl_strndup(const char *s, size_t n);
void  pl_strlcpy(char *dst, const char *src, size_t cap);
void  pl_trim_inplace(char *s);
bool  pl_starts_with(const char *s, const char *prefix);
bool  pl_foam_key_matches(const char *key, const char *name);   /* exact keyword, or a "quoted" regex matching all of name */
bool  pl_ends_with(const char *s, const char *suffix);
bool  pl_contains_ci(const char *hay, const char *needle);      /* case-insensitive substring */
bool  pl_is_number(const char *s);                               /* whole string is a number   */
void  pl_shell_quote(char *dst, size_t cap, const char *s);      /* 'single quoted' for bash   */
void  pl_human_size(char *dst, size_t cap, long long bytes);
void  pl_time_ago(char *dst, size_t cap, time_t t);
void  pl_duration(char *dst, size_t cap, double seconds);

/* ---------- paths and files ---------- */
void  pl_join(char *dst, size_t cap, const char *a, const char *b);
const char *pl_basename(const char *p);
void  pl_dirname(char *dst, size_t cap, const char *p);
void  pl_expand_home(char *dst, size_t cap, const char *p);    /* ~/x -> /home/u/x */
const char *pl_home(void);
bool  pl_exists(const char *p);
bool  pl_is_dir(const char *p);
bool  pl_is_file(const char *p);
time_t pl_mtime(const char *p);
/* these four keep answers for paths under /mnt/<drive>/ for 0.5 s (9p stats are slow); this forgets them */
void  pl_stat_cache_clear(void);
bool  pl_is_exec(const char *p);      /* a regular file we may run (cached like the four above on /mnt/<drive>) */
long long pl_file_size(const char *p);
long long pl_dir_size(const char *p, int maxDepth);
char *pl_read_file(const char *p, size_t *len);               /* malloc'd, NUL-terminated */
char *pl_read_head(const char *p, size_t maxBytes);           /* first bytes of a file    */
bool  pl_write_file(const char *p, const char *data, size_t len);  /* atomic (tmp + rename) */
bool  pl_touch(const char *p);
bool  pl_mkdirs(const char *p);
int   pl_remove_tree(const char *p);                          /* rm -rf, refuses "/" and $HOME */

typedef struct { char **names; int n, cap; } NameList;
void  names_free(NameList *l);
void  names_add(NameList *l, const char *s);
void  names_sort(NameList *l);
int   pl_list_dir(const char *dir, NameList *out, bool dirsOnly, bool filesOnly);
/* for per-frame callers: cached until the folder's mtime changes (or 2 s); owned by the cache, valid this frame only */
const NameList *pl_list_dir_cached(const char *dir, bool dirsOnly, bool filesOnly);

/* ---------- processes ---------- */
pid_t pl_spawn_shell(const char *script, const char *logPath);   /* bash -c, own session  */
pid_t pl_spawn_shell_log(const char *script, const char *logPath, bool append);   /* ... appending to the log */
void  pl_spawn_detached(const char *script);                    /* fire and forget        */
int   pl_run_wait(const char *script, char *out, size_t cap);   /* run, capture stdout    */
char *pl_run_capture(const char *script, size_t maxBytes, int *status);   /* malloc'd stdout, at most maxBytes */
bool  pl_which(const char *exe, char *out, size_t cap);
bool  pl_is_wsl(void);

#endif
