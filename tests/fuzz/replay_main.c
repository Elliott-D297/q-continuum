/* replay_main.c -- run a fuzz harness over files (the crash fixtures, a seed corpus) without libFuzzer, so every
 * crash the fuzzer ever found is checked by ctest:  fuzzreplay_<target> <file or dir>... */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int run_file(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    size_t r = fread(b, 1, (size_t)n, f);
    fclose(f);
    LLVMFuzzerTestOneInput(b, r);
    free(b);
    return 1;
}

int main(int argc, char **argv)
{
    int n = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) { n += run_file(argv[i]); continue; }
        DIR *d = opendir(argv[i]);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[4096]; snprintf(p, sizeof p, "%s/%s", argv[i], e->d_name);
            n += run_file(p);
        }
        if (d) closedir(d);
    }
    printf("replayed %d input%s without a crash\n", n, n == 1 ? "" : "s");
    return 0;
}
