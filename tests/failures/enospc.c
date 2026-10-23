/* enospc.c -- LD_PRELOAD shim for the disk-full test (1.10): once QC_FULL_BYTES bytes have been written to files under
 * QC_FULL_DIR (counted per process), write()/writev()/pwrite() to such files fail with ENOSPC, as on a full disk.
 *   cc -shared -fPIC -O2 -o enospc.so enospc.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

static long used;

static int full_target(int fd)
{
    const char *dir = getenv("QC_FULL_DIR");
    if (!dir || fd <= 2) return 0;
    char l[64], p[4096];
    snprintf(l, sizeof l, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(l, p, sizeof p - 1);
    if (n <= 0) return 0;
    p[n] = 0;
    return strncmp(p, dir, strlen(dir)) == 0;
}

static int over(int fd, size_t n)
{
    if (!full_target(fd)) return 0;
    const char *b = getenv("QC_FULL_BYTES");
    long limit = b ? atol(b) : 0;
    if (used + (long)n > limit) { errno = ENOSPC; return 1; }
    used += (long)n;
    return 0;
}

ssize_t write(int fd, const void *buf, size_t n)
{
    static ssize_t (*real)(int, const void *, size_t);
    if (!real) real = (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
    if (over(fd, n)) return -1;
    return real(fd, buf, n);
}

ssize_t writev(int fd, const struct iovec *iov, int cnt)
{
    static ssize_t (*real)(int, const struct iovec *, int);
    if (!real) real = (ssize_t (*)(int, const struct iovec *, int))dlsym(RTLD_NEXT, "writev");
    size_t n = 0; for (int i = 0; i < cnt; i++) n += iov[i].iov_len;
    if (over(fd, n)) return -1;
    return real(fd, iov, cnt);
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    static ssize_t (*real)(int, const void *, size_t, off_t);
    if (!real) real = (ssize_t (*)(int, const void *, size_t, off_t))dlsym(RTLD_NEXT, "pwrite");
    if (over(fd, n)) return -1;
    return real(fd, buf, n, off);
}
