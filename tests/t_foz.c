// t_foz — replay a Fossilize pipeline database through the Vulkan layer, with
// no guest: the launcher's prewarm (kl_foz_prewarm, runtime/gfx/kl_fossilize.cpp)
// on this Mac.
//
//   build/t_foz <device-info.json> <file.foz|dir>[:...]
//
// What it proves here is that the databases parse, that a device can be made
// from the device info, and that every pipeline goes through the layer's
// fix-ups and MoltenVK without being refused. How long a compile takes on the
// headset it cannot say.
#include <stdio.h>
#include <stdlib.h>

int kl_foz_prewarm(const char *info_path);
int kl_foz_progress(unsigned *done, unsigned *total, double *elapsed);

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: t_foz <device-info.json> <file.foz|dir>[:...]\n"); return 2; }
    setenv("KL_VK_FOZ_REPLAY", argv[2], 1);
    int rc = kl_foz_prewarm(argv[1]);
    unsigned done = 0, total = 0; double s = 0;
    int st = kl_foz_progress(&done, &total, &s);
    printf("t_foz: prewarm returned %d; state %d, %u of %u pipelines, %.1f s\n", rc, st, done, total, s);
    return rc ? 1 : 0;
}
