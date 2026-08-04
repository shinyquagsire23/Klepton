// Generic guest-image load harness: klepton's reconnaissance tool.
//   ./build/t_load <path.so> [--init]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../runtime/klepton.h"

// Rough grouping so a large missing-import list is readable.
static const char *category(const char *s) {
    if (!strncmp(s, "__android_log", 13))                      return "log";
    if (!strncmp(s, "A", 1) && strchr(s, '_') &&
        (!strncmp(s, "AAsset", 6) || !strncmp(s, "ALooper", 7) ||
         !strncmp(s, "ANative", 7) || !strncmp(s, "ASensor", 7) ||
         !strncmp(s, "AConfig", 7) || !strncmp(s, "AInput", 6)))return "NDK/android";
    if (!strncmp(s, "pthread_", 8) || !strncmp(s, "sem_", 4))   return "pthread";
    if (!strncmp(s, "egl", 3) || (s[0]=='g' && s[1]=='l'))      return "EGL/GLES";
    if (!strncmp(s, "vk", 2))                                   return "Vulkan";
    if (!strncmp(s, "_Z", 2) || !strncmp(s, "__cxa", 5) ||
        !strncmp(s, "_Unwind", 7) || !strncmp(s, "__gxx", 5))   return "C++/unwind";
    if (!strncmp(s, "JNI_", 4))                                 return "JNI";
    return "libc/libm/other";
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <lib.so> [--init]\n", argv[0]); return 2; }
    int do_init = (argc > 2 && !strcmp(argv[2], "--init"));

    kl_thread_init();
    printf("=== loading %s ===\n", argv[1]);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    kl_image *img = kl_load(argv[1]);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (!img) { printf("FAILED: %s\n", kl_error()); return 1; }
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    const kl_stats *st = kl_get_stats(img);
    unsigned total = st->relative + st->abs64 + st->glob_dat + st->jump_slot;
    printf("  mapped at %p, span %.2f MB, load+relocate in %.1f ms\n",
           kl_base(img), kl_span(img) / 1048576.0, ms);
    printf("  relocations: %u  (RELATIVE=%u ABS64=%u GLOB_DAT=%u JUMP_SLOT=%u)\n",
           total, st->relative, st->abs64, st->glob_dat, st->jump_slot);
    printf("  TLS rewrites: %u    inline svc #0: %u\n", st->tls_rewrites, st->svc_sites);
    printf("  x18 sites: %u    veneered: %u    refused: %u\n",
           st->x18_sites, st->x18_patched, st->x18_refused);
    printf("  imports: %u bound, %u unresolved sites\n", st->imports_bound, st->imports_missing);

    unsigned nm = 0;
    const char *const *miss = kl_missing_imports(img, &nm);
    if (nm) {
        printf("\n  %u UNIQUE unresolved imports:\n", nm);
        const char *cats[] = { "libc/libm/other", "pthread", "NDK/android", "C++/unwind",
                               "EGL/GLES", "Vulkan", "JNI", "log" };
        for (size_t c = 0; c < sizeof cats / sizeof *cats; c++) {
            unsigned n = 0;
            for (unsigned i = 0; i < nm; i++) if (!strcmp(category(miss[i]), cats[c])) n++;
            if (!n) continue;
            printf("    [%s] %u\n      ", cats[c], n);
            unsigned shown = 0;
            for (unsigned i = 0; i < nm; i++) {
                if (strcmp(category(miss[i]), cats[c])) continue;
                if (shown && shown % 4 == 0) printf("\n      ");
                printf("%-26s", miss[i]);
                if (++shown >= 24) { printf("... (+%u more)", n - shown); break; }
            }
            printf("\n");
        }
    }

    if (do_init) {
        printf("\n  running DT_INIT_ARRAY (%s)…\n",
               nm ? "WARNING: unresolved imports present" : "all imports bound");
        fflush(stdout);
        kl_run_init(img);
        printf("  init OK\n");
    }
    printf("\n=== loaded successfully ===\n");
    return 0;
}
