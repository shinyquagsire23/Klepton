// "Max resolution per eye" (kl_ovrp_set_eye_max_choice, KleptonEyeMax.swift):
// which cap the display's request is held to. The launch line's KL_OVRP_EYE_MAX
// first, then the player's choice for the game, then the title's own cap (4000
// for Gun Club VR), then the general 3072; Native leaves the request as it is.
// The render scale still applies after the cap, so "Resolution" stays a
// fraction of the chosen size. kl_ovrp_title_eye_cap answers by name before a
// guest runs. Each case is a child of its own, since a target is set once per
// process.
//
// The swapchain itself (kl_ovrp_eye_desc_size, what ovrp_CalculateEyeLayerDesc
// hands the guest): a Unity guest multiplies the offer by a scale of its own,
// and Gun Club VR's 1.4 turned its 4000 cap into 5600 an eye on the headset.
// For Gun Club and for any chosen value that product is held to the cap too;
// another title on its default keeps its own scale on top, Native stays
// uncapped, and KL_OVRP_EYE_DESC_CAP=0 restores the old behaviour.
// `make maxeye`, in `make check`. No game content.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "klepton.h"
#include "guest/kl_driver.h"
#include "xr/kl_ovrp.h"

#define GUNCLUB "x2285803881494677"
#define OTHER "maxeye-test-other"
#define ONIRI "x7611090302277055"
#define FLOORPLAN2 "x2401648309868679"

// What the guest is told for a 4633x3717 request (the display's own size).
static int eye(const char *target, int choice, const char *env, const char *rs, int want_w, int want_h) {
    static kl_target t;
    t.name = target;
    kl_driver_init(&t, NULL, 0);
    kl_driver_set_alarm(0);
    if (env) setenv("KL_OVRP_EYE_MAX", env, 1); else unsetenv("KL_OVRP_EYE_MAX");
    if (rs) setenv("KL_OVRP_RENDER_SCALE", rs, 1); else unsetenv("KL_OVRP_RENDER_SCALE");
    kl_ovrp_set_eye_max_choice(choice);
    kl_ovrp_set_eye_texture_size(4633, 3717);
    int w = 0, h = 0, fw = 0, fh = 0;
    float s = 0;
    kl_ovrp_eye_texture_size(&w, &h);
    kl_ovrp_eye_texture_scale(&s, &fw, &fh);
    if (w != want_w || h != want_h) { fprintf(stderr, "  eye %dx%d, want %dx%d\n", w, h, want_w, want_h); return 1; }
    // MetalFX restores to the capped size, before the render scale.
    if (!rs && (fw != w || fh != h)) { fprintf(stderr, "  full %dx%d, want %dx%d\n", fw, fh, w, h); return 1; }
    return 0;
}

// The final layer for a guest scale `gs`; desc_env sets KL_OVRP_EYE_DESC_CAP.
static const char *desc_rs;
static int desc(const char *target, int choice, const char *desc_env, float gs, int want_w, int want_h) {
    static kl_target t;
    t.name = target;
    kl_driver_init(&t, NULL, 0);
    kl_driver_set_alarm(0);
    unsetenv("KL_OVRP_EYE_MAX");
    if (desc_rs) setenv("KL_OVRP_RENDER_SCALE", desc_rs, 1); else unsetenv("KL_OVRP_RENDER_SCALE");
    if (desc_env) setenv("KL_OVRP_EYE_DESC_CAP", desc_env, 1); else unsetenv("KL_OVRP_EYE_DESC_CAP");
    kl_ovrp_set_eye_max_choice(choice);
    kl_ovrp_set_eye_texture_size(4633, 3717);
    int w = 0, h = 0;
    kl_ovrp_eye_desc_size(gs, &w, &h);
    if (w != want_w || h != want_h) { fprintf(stderr, "  layer %dx%d, want %dx%d\n", w, h, want_w, want_h); return 1; }
    return 0;
}
static int run_desc(const char *what, const char *target, int choice, const char *desc_env, float gs, int w, int h) {
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) _exit(desc(target, choice, desc_env, gs, w, h));
    int st = 0;
    waitpid(p, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

static int run(const char *what, const char *target, int choice, const char *env, const char *rs, int w, int h) {
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) _exit(eye(target, choice, env, rs, w, h));
    int st = 0;
    waitpid(p, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    return ok;
}

int main(void) {
    printf("=== Max resolution per eye: launch line > choice > title > 3072 ===\n");
    int ok = 1;
    ok &= kl_ovrp_title_eye_cap(GUNCLUB) == 4000;
    ok &= kl_ovrp_title_eye_cap(OTHER) == 3072;
    ok &= kl_ovrp_title_eye_cap("cs1") == 2560;
    ok &= kl_ovrp_title_eye_cap(ONIRI) == 0;
    ok &= kl_ovrp_title_eye_cap("x3713270985434472") == 4000;
    ok &= kl_ovrp_title_eye_cap(FLOORPLAN2) == 4096;
    ok &= kl_ovrp_title_eye_cap("x2108775495884888") == 4608;
    ok &= kl_ovrp_title_eye_cap("x2223615721048141") == 4608;
    ok &= kl_ovrp_title_eye_cap("x2426206484098337") == 4608;
    printf("%s title caps by name: Gun Club 4000, cs1 2560, Forest Oniri none, PowerWash 4000, Floor Plan 2 4096, Vader Immortal I-III 4608, other 3072\n", ok ? "ok  " : "FAIL");
    ok &= run("other title, default: 3072 (3072x2465)", OTHER, 0, NULL, NULL, 3072, 2465);
    ok &= run("Gun Club, default: its own 4000 (4000x3209)", GUNCLUB, 0, NULL, NULL, 4000, 3209);
    ok &= run("Floor Plan 2, default: its own 4096 (4096x3286)", FLOORPLAN2, 0, NULL, NULL, 4096, 3286);
    ok &= run("Gun Club, chose 2560: 2560x2054", GUNCLUB, 2560, NULL, NULL, 2560, 2054);
    ok &= run("other title, chose 4608: 4608x3697", OTHER, 4608, NULL, NULL, 4608, 3697);
    ok &= run("other title, Native: the whole request (4633x3717)", OTHER, KL_OVRP_EYE_NATIVE, NULL, NULL, 4633, 3717);
    ok &= run("Forest Oniri, default: no cap, the whole request (4633x3717)", ONIRI, 0, NULL, NULL, 4633, 3717);
    ok &= run("Forest Oniri, chose 3072: 3072x2465", ONIRI, 3072, NULL, NULL, 3072, 2465);
    ok &= run("Gun Club, chose 4608, KL_OVRP_EYE_MAX=3072: the launch line (3072x2465)", GUNCLUB, 4608, "3072", NULL, 3072, 2465);
    ok &= run("other title, Native, KL_OVRP_EYE_MAX=2048: the launch line (2048x1643)", OTHER, KL_OVRP_EYE_NATIVE, "2048", NULL, 2048, 1643);
    ok &= run("other title, chose 4000 at 80 %: 3200x2568", OTHER, 4000, NULL, "0.8", 3200, 2568);
    ok &= run("other title, a nonsense choice (-7): the default 3072", OTHER, -7, NULL, NULL, 3072, 2465);
    ok &= run_desc("Gun Club, default, Unity scale 1.4: layer held to 4000 (4000x3210)", GUNCLUB, 0, NULL, 1.4f, 4000, 3210);
    ok &= run_desc("Gun Club, chose 3072, scale 1.4: 3072x2466", GUNCLUB, 3072, NULL, 1.4f, 3072, 2466);
    ok &= run_desc("other title, chose 4608, scale 1.2: 4608x3698", OTHER, 4608, NULL, 1.2f, 4608, 3698);
    ok &= run_desc("other title, default, scale 1.2: its own scale on top (3686x2958)", OTHER, 0, NULL, 1.2f, 3686, 2958);
    ok &= run_desc("other title, Native, scale 1.2: uncapped (5559x4460)", OTHER, KL_OVRP_EYE_NATIVE, NULL, 1.2f, 5559, 4460);
    ok &= run_desc("Forest Oniri, default, scale 1.2: uncapped (5559x4460)", ONIRI, 0, NULL, 1.2f, 5559, 4460);
    ok &= run_desc("Gun Club, KL_OVRP_EYE_DESC_CAP=0, scale 1.4: as before (5600x4492)", GUNCLUB, 0, "0", 1.4f, 5600, 4492);
    desc_rs = "0.8";
    ok &= run_desc("Gun Club at 80 %, scale 1.4: the cap follows Resolution (3200x2568)", GUNCLUB, 0, NULL, 1.4f, 3200, 2568);
    desc_rs = NULL;
    puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
