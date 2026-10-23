/* fstrace.c -- LD_PRELOAD shim: time spent in stat/open/opendir/access per path (digits folded to #), printed at exit
 * to $QC_FSTRACE_OUT, slowest first. For finding per-frame file polling on slow filesystems (/mnt/c over 9p).
 *   cc -shared -fPIC -O2 -o fstrace.so fstrace.c -ldl
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define N 4096
static struct { char key[200]; double s; long n; } T[N];
static int nT;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void add(const char *op, const char *p, double dt)
{
    if (!p) return;
    char k[200]; int j = snprintf(k, sizeof k, "%s ", op);
    for (const char *c = p; *c && j < (int)sizeof k - 1; c++) k[j++] = (*c >= '0' && *c <= '9') ? '#' : *c;
    k[j] = 0;
    for (int i = 0; i < nT; i++) if (!strcmp(T[i].key, k)) { T[i].s += dt; T[i].n++; return; }
    if (nT < N) { strcpy(T[nT].key, k); T[nT].s = dt; T[nT].n = 1; nT++; }
}
static int cmp(const void *a, const void *b) { double x = ((const typeof(T[0]) *)a)->s, y = ((const typeof(T[0]) *)b)->s; return x < y ? 1 : x > y ? -1 : 0; }
__attribute__((destructor)) static void dump(void)
{
    const char *o = getenv("QC_FSTRACE_OUT");
    if (!o || !nT) return;
    char fn[512]; snprintf(fn, sizeof fn, "%s.%d", o, getpid());
    FILE *f = fopen(fn, "w"); if (!f) return;
    qsort(T, (size_t)nT, sizeof T[0], cmp);
    for (int i = 0; i < nT && i < 60; i++) fprintf(f, "%8.3f s %7ld  %s\n", T[i].s, T[i].n, T[i].key);
    fclose(f);
}
#define WRAP(ret, name, sig, args, pathexpr) \
    ret name sig { static ret (*real) sig; if (!real) real = (ret (*) sig)dlsym(RTLD_NEXT, #name); \
                   double t0 = now(); ret r = real args; add(#name, pathexpr, now() - t0); return r; }
WRAP(int, stat, (const char *p, struct stat *b), (p, b), p)
WRAP(int, lstat, (const char *p, struct stat *b), (p, b), p)
WRAP(int, access, (const char *p, int m), (p, m), p)
WRAP(DIR *, opendir, (const char *p), (p), p)
WRAP(int, fstatat, (int d, const char *p, struct stat *b, int fl), (d, p, b, fl), p)
int open(const char *p, int fl, ...)
{
    static int (*real)(const char *, int, ...); if (!real) real = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
    va_list ap; va_start(ap, fl); int mode = va_arg(ap, int); va_end(ap);
    double t0 = now(); int r = real(p, fl, mode); add("open", p, now() - t0); return r;
}
FILE *fopen(const char *p, const char *m)
{
    static FILE *(*real)(const char *, const char *); if (!real) real = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen");
    double t0 = now(); FILE *r = real(p, m); add("fopen", p, now() - t0); return r;
}
struct dirent *readdir(DIR *d)
{
    static struct dirent *(*real)(DIR *); if (!real) real = (struct dirent *(*)(DIR *))dlsym(RTLD_NEXT, "readdir");
    double t0 = now(); struct dirent *r = real(d); double dt = now() - t0;
    char l[64], p[1024]; snprintf(l, sizeof l, "/proc/self/fd/%d", dirfd(d)); ssize_t n = readlink(l, p, sizeof p - 1);
    if (n > 0) { p[n] = 0; add("readdir", p, dt); }
    return r;
}
ssize_t read(int fd, void *b, size_t c)
{
    static ssize_t (*real)(int, void *, size_t); if (!real) real = (ssize_t (*)(int, void *, size_t))dlsym(RTLD_NEXT, "read");
    double t0 = now(); ssize_t r = real(fd, b, c); double dt = now() - t0;
    if (dt > 1e-4) { char l[64], p[1024]; snprintf(l, sizeof l, "/proc/self/fd/%d", fd); ssize_t n = readlink(l, p, sizeof p - 1);
                     if (n > 0) { p[n] = 0; add("read", p, dt); } }
    return r;
}
