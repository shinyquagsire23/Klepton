// The host driver: enter the guest through its real entry point, on macOS.
//
// The guest's own sequences are runtime/guest/kl_driver.c, shared verbatim with
// the visionOS app so that one guest cannot be described two ways. What is here
// is everything a COMMAND LINE adds and an app bundle has no use for: the DRM
// policy self-test, the re-exec'd recon child (an unimplemented JNI slot aborts
// by design, so the run that measures the surface must be disposable), the SDL
// viewer, the host-only instruments (sampler, managed probe, metadata dump), the
// Metal stand-in for Compositor Services, and how far to go — KL_LIFECYCLE,
// KL_FRAMES, KL_SLINK_MAIN, KL_GAP_ONLY.
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../runtime/klepton.h"
#include "../runtime/gfx/kl_glfb.h"
#include "../runtime/kl_jni.h"
#include "../runtime/gfx/kl_egl.h"
#include "../runtime/xr/kl_openxr.h"
#include "../runtime/xr/kl_ovrp.h"
#include "../runtime/xr/kl_ovrplat.h"
#include "../runtime/gfx/kl_view.h"
#include "../runtime/diag/kl_sample.h"
#include "../runtime/diag/kl_mprobe.h"
#include "../runtime/diag/kl_metadump.h"
#include "../runtime/kl_fault.h"
#include "../runtime/kl_target.h"
#include "../runtime/guest/kl_driver.h"
#include "../runtime/guest/kl_slink.h"
#include "../tests/t_mtl_provider.h"

// Which guest, and where its libraries are. Both come from the target table
// (runtime/kl_target.h) rather than from four literals here and in kl_jni.c's
// defaults — the APK, the asset tree and the userdata directory have to agree
// with the libraries, and when they did not it was silent: SUPERHOT's libraries
// opening Beat Saber's APK is a guest reading someone else's game data.
//
//   ./build/m_boot                      the default target (beatsaber)
//   ./build/m_boot superhot             ...by name
//   ./build/m_boot superhot/lib/arm64-v8a   ...the same, by path
//   ./build/m_boot steamlink-vr         ...including Steam Link, whose front
//                                       door is KL_SLINK_VR / KL_SLINK_SHELL
//
// The path form is what every documented run command and every Makefile gate
// passes, and it resolves to the same target as the name.
static const kl_target *TARGET;
static const char *LIBDIR;
static kl_slink_door DOOR;          // read only by a Steam Link target
#define DOOR_IS_VR (TARGET->kind == KL_GUEST_STEAMLINK && DOOR == KL_SLINK_VR)

static int fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

// The fatal-signal reporter moved to runtime/kl_fault.c when the visionOS app
// needed the same thing inside a bundle — it is a diagnostic
// that ships, not a harness detail. The sampling profiler stays host-only, so
// t_boot registers it rather than kl_fault.c referencing RUNTIME_DIAG.
static void install_fault_reporter(void) {
    kl_fault_add_reporter(kl_sample_stop_report);
    // A crash report that does not depend on stderr surviving. On the host that
    // is usually the terminal and fine, but a viewer run redirects, a guest can
    // take the fd, and stdio can hold the last writes behind a lock the dying
    // thread never releases — and a signal death is exactly when the output
    // matters most. KL_CRASH_LOG moves it; the default sits beside the run.
    kl_fault_set_crash_path(kl_env_str("KL_CRASH_LOG", "/tmp/klepton-crash.log"));
    kl_fault_install();
    // After kl_fault_install, deliberately: the watch chains to whatever it
    // finds on SIGSEGV/SIGBUS, so a real crash still reaches the reporter.
    kl_fault_add_reporter(kl_metadump_watch_report);
    kl_metadump_watch_install();
}

// The entitlement refusal in kl_ovrplat.c is a policy guard, and Beat Saber does
// not currently reach it — the platform fails to initialise first, so the guard
// has never fired in a real run. A safety mechanism that has never been exercised
// is an assumption, not a mechanism, so it is checked here directly.
//
// Two properties matter: that an ownership query does not resolve to something
// callable-and-harmless, and that calling it dies rather than returning a value.
// The call is made in a child because passing the test means aborting.
// The resolve happens in the child too, not just the call. kl_ovrplat_sym records
// what was looked up, and the surface report separates entitlement lookups out
// specifically so a real one is visible — probing from the parent would leave our
// own test sitting in that list, indistinguishable from the guest asking.
// What does calling `name` actually do? Run it in a child, because for most of
// this surface the PASSING outcome is death. The value matters as well as the
// refusal now — the line below distinguishes the application's own entitlement
// (answered, and answered yes) from DLC (refused), and "it returned something"
// cannot tell those apart.
enum { KLDRM_UNRESOLVED = -1, KLDRM_ABORTED, KLDRM_ZERO, KLDRM_NONZERO };

static int drm_call(const char *name) {
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        // The refusal prints a paragraph and a surface report; not wanted here.
        freopen("/dev/null", "w", stderr);
        void *fn = kl_ovrplat_sym(name);
        if (!fn) _exit(2);              // resolved to nothing at all
        uint64_t v = ((uint64_t (*)(void))fn)();
        _exit(v ? 4 : 3);
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT) return KLDRM_ABORTED;
    if (!WIFEXITED(st)) return KLDRM_UNRESOLVED;
    switch (WEXITSTATUS(st)) {
    case 3: return KLDRM_ZERO;
    case 4: return KLDRM_NONZERO;
    default: return KLDRM_UNRESOLVED;
    }
}

// The line this asserts, in both directions. It moved once, deliberately, and
// the move is the reason the value is now checked rather than just the refusal:
//
//   The APPLICATION's own entitlement is answered, and answered yes. Klepton
//   runs an APK unpacked from a device the user owns, so ownership of the
//   application itself is the one thing in this family we can actually assert
//   — and it is asserted about a title the user already bought. It is not a
//   fabricated answer in the sense the refusals below are about; it is the
//   true state of affairs for the only way this project is used.
//
//   DLC is a different question and stays refused, because it is one we
//   genuinely cannot answer: paid content here is out-of-band asset data plus
//   a licence held by a platform that is absent, and neither half is on this
//   host to check. Claiming ownership of content we cannot see IS the
//   circumvention, so it keeps aborting by name.
//
// The two are separable because the API separates them — the viewer-entitled
// call is about the app, the AssetFile and IAP families are about content.
static int check_drm_guard(void) {
    // 1. The application's own entitlement: answered, and non-zero. Checked for
    //    its VALUE, since a 0 here reads to the guest as "not entitled" and
    //    sends a legitimately-owned title down its licence-failure path.
    int r = drm_call("ovr_Entitlement_GetIsViewerEntitled");
    if (r == KLDRM_UNRESOLVED)
        return fail("entitlement guard: the name resolved to nothing");
    if (r == KLDRM_ABORTED)
        return fail("entitlement guard ABORTED — the application's own entitlement "
                    "is the one we can assert, and refusing it stops a title the "
                    "user owns");
    if (r != KLDRM_NONZERO)
        return fail("entitlement guard answered 0 — that reads as 'not entitled' "
                    "and is the licence-failure path, not a neutral answer");

    // 1b. ...and the COMPLETION says so, because the value alone stopped being
    //     the answer. It is a request, not a predicate: the SDK wraps the return
    //     in a `Request` and awaits a message, so a non-zero id with nothing
    //     arriving for it is a guest that waits forever — which is exactly what
    //     stopped 1.40 at the epilepsy screen, one request over. Assert the pair.
    {
        void *pop = kl_ovrplat_sym("ovr_PopMessage");
        void *ent = kl_ovrplat_sym("ovr_Entitlement_GetIsViewerEntitled");
        void *rid = kl_ovrplat_sym("ovr_Message_GetRequestID");
        void *err = kl_ovrplat_sym("ovr_Message_IsError");
        if (!pop || !ent || !rid || !err)
            return fail("entitlement guard: the message pump resolved to nothing");
        while (((void *(*)(void))pop)()) { }             // drain anything queued
        uint64_t req = ((uint64_t (*)(void))ent)();
        void    *msg = ((void *(*)(void))pop)();
        if (!msg)
            return fail("the entitlement request queued NO completion — the guest "
                        "awaits one and would wait forever");
        if (((uint64_t (*)(void *))rid)(msg) != req)
            return fail("the entitlement completion carries the wrong request id");
        if (((uint64_t (*)(void *))err)(msg))
            return fail("the entitlement completion is an ERROR — that is the "
                        "licence-failure path, not a neutral answer");
    }

    // 2. Anything that would DELIVER paid content must still die rather than
    //    answer. This is also what proves the ovr_AssetFile_GetList carve-out
    //    below is an exact name and not a widening of the "AssetFile" marker to
    //    the whole family.
    r = drm_call("ovr_AssetFile_DownloadById");
    if (r == KLDRM_UNRESOLVED)
        return fail("DRM guard: ovr_AssetFile_DownloadById resolved to nothing");
    if (r != KLDRM_ABORTED)
        return fail("DRM guard did NOT abort on ovr_AssetFile_DownloadById — a "
                    "content-delivery call must never be answered");

    // 3. The carve-out, in the other direction: enumerating installed DLC is
    //    answered with "none", because that grants nothing (kl_ovrplat.c,
    //    g_plat_absent). If this ever starts aborting the game stops at the
    //    language select again.
    r = drm_call("ovr_AssetFile_GetList");
    if (r == KLDRM_UNRESOLVED) return fail("ovr_AssetFile_GetList resolved to nothing");
    if (r == KLDRM_ABORTED)
        return fail("ovr_AssetFile_GetList aborted — enumerating DLC we do not have "
                    "is a fact about this host, not a licence decision");

    printf("  the app's own entitlement answers yes AND completes, DLC delivery\n"
           "  refuses and aborts (guard verified); DLC enumeration answers\n"
           "  \"none\" without granting anything\n");
    return 0;
}

// ---- the guest run, in one function ----
// This is everything from libmain.so entry through the lifecycle recon. It
// runs either in the re-exec'd child (normal), or in-process (KL_NOFORK=1,
// for debugging). Returns only on a test failure; success is _exit(0), as
// before.
//
// view_pump is the KL_VIEW shape: the same sequence, but the frame pump runs
// until the viewer window closes instead of KL_FRAMES times, and the function
// RETURNS instead of _exit(0) — the guest is a spawned thread in that mode and
// the reports belong to the main thread after the join.
static volatile int g_view_quit;
// Where this target's `global-metadata.dat` is on disk. Two instruments want it
// — the sampler, to name managed frames, and the metadata dump, to compare what
// is in memory against what was loaded — and they must not derive it
// differently: a sampler resolving against one file while the dump measures
// another is two answers about one guest.
static const char *metadata_path(char *buf, size_t n) {
    const char *menv = getenv("KL_IL2CPP_METADATA");
    if (menv) { snprintf(buf, n, "%s", menv); return buf; }
    // <apk>/lib/arm64-v8a -> <apk>/assets/bin/Data/Managed/...
    snprintf(buf, n, "%s", LIBDIR);
    char *tail = strstr(buf, "/lib/");
    if (tail) *tail = 0;
    snprintf(buf + strlen(buf), n - strlen(buf),
             "/assets/bin/Data/Managed/Metadata/global-metadata.dat");
    return buf;
}

// The guest's frame, read back through Metal. It belongs to the RUN rather than
// to the Unity lifecycle: every non-Unity door reaches the eye textures the same
// way (kl_glfb's eye table), so a driver that stops before this reports a
// working pipeline as nothing at all.
static void report_eye_interop(void) {
    if (!kl_glfb_has_mtl_provider() && !kl_glfb_eye_mtl_texture(0, 0, NULL)) return;
    printf("\n=== Metal interop: the guest's frame, read back ===\n");
    // Not "did the interop bind" — that is reported when it happens — but
    // "did the guest's rendering arrive in the MTLTexture".
    // The lit count uses the same luma threshold as kl_glfb's capture, so
    // it is directly comparable with the reference path's number: the two
    // should agree, and a lit reference frame beside a black interop one
    // means the binding took and the rendering went elsewhere.
    // ...and it fires for an eye MTLTexture that arrived any way at
    // all, not just through a registered provider. On the VULKAN path
    // there IS no provider: MoltenVK backs the guest's eye VkImage with
    // a texture of its own and kl_vulkan.c publishes that one
    // (kl_glfb_note_eye_mtl_texture), so a provider test would have
    // skipped the only guest whose compositor wiring is new.
    // Under the viewer's hardware compositor there IS no reference:
    // registering a GPU fence replaces the readback, so kl_glfb has
    // counted nothing and a bare 0 here would read as a black frame.
    if (!kl_glfb_has_mtl_provider())
        printf("  reference: KL_VK_OUT — on the Vulkan path the eye "
               "is read back from the VkImage, and this is the SAME "
               "storage seen as Metal\n");
    else if (kl_glfb_has_gpu_fence())
        printf("  reference: none — the GPU compositor is driving, "
               "so nothing was read back\n");
    else
        printf("  reference (glReadPixels, kl_glfb): %lu lit\n",
               kl_glfb_last_frame_lit());
    // EVERY stage, not stage 0. This used to read and dump stage 0
    // alone while the compositors show
    // kl_ovrp_last_complete_stage() — so on a guest whose swapchain
    // has more than one image the PNG and the screen are DIFFERENT
    // PICTURES, and the readback can report a perfect frame for
    // something nobody is looking at — an instrument answering
    // confidently about the wrong subject. Open Brush has 3 OpenXR
    // swapchain images and shows it.
    int shown = kl_ovrp_last_complete_stage();
    int stages = kl_ovrp_stage_count();
    if (stages < 1) stages = 1;
    printf("  the compositors show stage %d (kl_ovrp_last_complete_stage)"
           "%s\n", shown,
           shown < 0 ? " — nothing filed, so they fall back to stage 0" : "");
    for (int eye = 0; eye < 2; eye++) {
        for (int st = 0; st < stages; st++) {
            int w = 0, h = 0;
            if (!kl_glfb_eye_mtl_texture(eye, st, NULL)) continue;
            unsigned long lit = kl_mtl_count_lit(eye, st, &w, &h);
            unsigned long n = w && h ? ((unsigned long)((w + 7) / 8)
                                      * (unsigned long)((h + 7) / 8)) : 0;
            printf("  eye %d stage %d MTLTexture %dx%d: %lu/%lu lit, "
                   "mean luma %u%s%s\n",
                   eye, st, w, h, lit, n, kl_mtl_mean_luma(),
                   lit ? "" : "  <<< BLACK",
                   st == shown ? "   <- ON SCREEN" : "");
            // A picture, not just a count: KL_GLFB_OUT already holds the
            // reference frames, so writing the interop eyes beside them
            // is what makes "does it render the same" answerable.
            const char *out = getenv("KL_GLFB_OUT");
            if (out) {
                char p[1200];
                snprintf(p, sizeof p, "%s/mtl_eye%d_s%d.png", out, eye, st);
                printf("    -> %s%s\n", p,
                       kl_mtl_dump_png(eye, st, p) ? "" : "  (write FAILED)");
            }
        }
    }
    // ...and the layers that are NOT an eye. A guest can present its whole
    // frame as an OpenXR quad — JKXR submits nothing else — and for that guest
    // every number above is 0 and says nothing: the eye table is empty because
    // there is no eye. This is the same measurement asked of the storage the
    // overlay pass samples, and it is the only evidence on host that the
    // picture reached the compositor at all.
    for (int i = 0; i < kl_ovrp_overlay_count(); i++) {
        kl_ovrp_overlay ov;
        if (!kl_ovrp_overlay_get(i, &ov)) continue;
        // EVERY image of the layer's swapchain, not just the one the last frame
        // named. The guest rotates through them and the compositor follows, so
        // a single image that stopped receiving the guest's rendering is a
        // FROZEN picture alternating with live ones — which is what a stale
        // frame flickering over a correct one looks like, and what no
        // measurement of one image can see. `stages` here is the record ring's
        // count, which is the same 3 an OpenXR swapchain has.
        for (int st = 0; st < stages; st++) {
            int w = 0, h = 0;
            if (!kl_glfb_layer_mtl_texture(ov.layer_id, st, NULL, NULL)) {
                if (st == ov.stage)
                    printf("  layer %d stage %d (%.2fx%.2f m): no MTLTexture — "
                           "NOT composited\n", ov.layer_id, st,
                           (double)ov.size[0], (double)ov.size[1]);
                continue;
            }
            unsigned long lit = kl_mtl_count_lit_layer(ov.layer_id, st, &w, &h);
            unsigned long n = w && h ? ((unsigned long)((w + 7) / 8)
                                      * (unsigned long)((h + 7) / 8)) : 0;
            // Alpha, beside the luma, because a quad layer is BLENDED: a guest
            // that leaves it at 0 (trap 33) submits a perfect picture that
            // composites to nothing, and that is the same display as black with
            // every other number here healthy.
            unsigned alpha = kl_mtl_mean_alpha();
            printf("  layer %d stage %d MTLTexture %dx%d (%.2fx%.2f m at "
                   "%.2f %.2f %.2f): %lu/%lu lit, mean luma %u, mean alpha %u%s%s%s\n",
                   ov.layer_id, st, w, h, (double)ov.size[0], (double)ov.size[1],
                   (double)ov.pose[4], (double)ov.pose[5], (double)ov.pose[6],
                   lit, n, kl_mtl_mean_luma(), alpha,
                   st == ov.stage ? "   <- LAST SUBMITTED" : "",
                   lit ? "" : "  <<< BLACK",
                   lit && alpha < 8 ? "  <<< TRANSPARENT: a blended layer with "
                                      "this alpha composites to nothing" : "");
            const char *out = getenv("KL_GLFB_OUT");
            if (out) {
                char p[1200];
                snprintf(p, sizeof p, "%s/mtl_layer%d_s%d.png", out, ov.layer_id, st);
                printf("    -> %s%s\n", p,
                       kl_mtl_dump_png_layer(ov.layer_id, st, p) ? ""
                                                                : "  (write FAILED)");
            }
        }
    }
}

// ---- the 2D -> VR handoff ----
//
// SteamLink.startVRLink(String) is the end of the shell's job: it has paired, the
// host has authorized, and it now starts the VR activity with the session as
// `sArgs` and calls finishAndRemoveTask() on itself. Both halves exist in this
// binary, but they are two front doors with different chains, different libraries
// and different lifecycles, and the shell's `main` is on the stack of the thread
// making this call.
//
// So: re-EXEC, which is what Android's own model already is — a new activity in a
// fresh task, the old one finishing. It replaces the process image, so every
// question about tearing the shell down (Qt, SDL, ANGLE's contexts, the audio
// unit, the shell's threads) does not arise. The exec'd process reads the session
// out of the environment through the same KL_SLINK_SARGS path a pasted one uses,
// so nothing downstream can tell the difference.
//
// KL_SLINK_HANDOFF=0 leaves the session printed and the run stopped by name,
// which is what a gate measuring the SHELL wants: a run that re-execs into a
// different front door measures something else.
static const char *g_argv0 = "./build/m_boot";

static void slink_setenv_default(const char *k, const char *v, const char *why) {
    if (getenv(k)) return;
    setenv(k, v, 1);
    printf("    %s=%s   (%s)\n", k, v, why);
}

static void slink_vrlink_handoff(const char *sargs) {
    if (!kl_env_on("KL_SLINK_HANDOFF", 1)) {
        printf("  (KL_SLINK_HANDOFF=0 — not entering the VR front door)\n");
        return;                      // the caller aborts by name, as before
    }

    printf("\n=== 2D -> VR handoff: re-exec into the OpenXR front door ===\n");
    printf("    %s %s\n", g_argv0, TARGET->name);
    setenv("KL_SLINK_SARGS", sargs, 1);
    setenv("KL_SLINK_VR", "1", 1);
    unsetenv("KL_SLINK_SHELL");      // the door resolver would otherwise tie
    unsetenv("KL_VIEW_POKE");        // a click script for the SHELL's UI, and
                                     // replaying it into the VR view would be
                                     // synthetic input nobody asked for
    slink_setenv_default("KL_SLINK_MAIN", "1",
                         "drive the lifecycle on into the OpenXR frame loop");
    slink_setenv_default("KL_GLFB", "1",
                         "the null GL driver cannot present a stream");
    slink_setenv_default("KL_OVRP_IPD", "0.063",
                         "HOST STOPGAP: an IPD of 0 stops the host sending video");
    if (!getenv("KL_VIEW"))
        slink_setenv_default("KL_SLINK_WAIT", "45",
                             "the VR deadline; 10 s would waste the pairing");
    fflush(NULL);

    execl(g_argv0, g_argv0, TARGET->name, (char *)NULL);

    // Only reachable if exec failed. Returning is the contract for "could not do
    // the handoff": the caller then aborts by name with the session printed
    // above, so the run is no worse off than before.
    printf("  !! exec(\"%s\") failed: %s\n", g_argv0, strerror(errno));
    fflush(NULL);
}

// The doors whose guest owns its own frame loop and takes a looper rather than a
// render call: Unreal's NativeActivity, OpenJK's static exports, and Steam Link's
// three. What differs between them is entirely inside kl_driver; what this adds
// is the command line's part — the budget in SECONDS (there is no render call
// here to count), the watchdog around it, and the interop readback, which is the
// only thing that says whether the guest's rendering ARRIVED.
//
// Under the viewer the deadline is the window: the pump runs until it closes,
// and the reports belong to the main thread after the join.
static int pump_and_report(double want, int view_pump) {
    const char *aenv = getenv("KL_ALARM");
    if (view_pump) printf("\n=== pumping the looper until the viewer closes ===\n");
    else           printf("\n=== pumping the looper for %.1f s ===\n", want);
    fflush(NULL);
    // The driver's per-call watchdog off, one alarm around the whole pump: this
    // is a single call that runs for the budget.
    kl_driver_set_alarm(0);
    if (!view_pump) alarm(aenv ? (unsigned)strtoul(aenv, NULL, 10) : (unsigned)(want + 30));
    double spent = kl_driver_pump(view_pump ? -1.0 : want,
                                  view_pump ? &g_view_quit : NULL);
    alarm(0);
    printf("  pumped %.2f s\n", spent);
    if (view_pump) return 0;

    printf("\n=== reports ===\n");
    kl_driver_report(stdout);
    report_eye_interop();
    fflush(NULL);
    return 0;
}

static int begin_and_pump(int view_pump) {
    // Before the engine builds its OpenXR swapchains, which is where the images
    // that need MTLTexture storage are created. The Unity path registers this
    // inside its lifecycle (ovrp_SetupEyeTexture2 arrives there); an Unreal
    // guest needs no provider at all, its eye images come from MoltenVK. Self-
    // guarding on KL_GLFB_MTL / KL_VIEW, so a plain run is unchanged.
    if (TARGET->kind == KL_GUEST_JKXR || DOOR_IS_VR) kl_mtl_provider_install();

    // The same layer policy the device app applies, and for the same reasons —
    // Steam Link's VR door stacks projection layers and streams FOVEATED, so the
    // eye is the widest layer and the narrow one is laid over its centre, into
    // an eye texture allocated larger so the inset keeps its detail.
    //
    // It lived only in the visionOS frontend, which made the host silently a
    // DIFFERENT pipeline: no inset composite at all. That is why this arc's
    // foveal defects "did not reproduce in the macOS viewer" — not a stable
    // layer shape, as was first written down, but a path that was never on.
    // A host run is the cheap half of every A/B here, and it can only be that
    // if it runs what the device runs.
    if (DOOR_IS_VR) {
        kl_openxr_set_capture_topmost_layer(1);
        int es = kl_env_int("KL_XR_EYE_SCALE", 2);
        if (es < 1) es = 1;
        if (es > 4) es = 4;
        kl_glfb_set_eye_mirror_scale(es, 1);
        kl_glfb_set_eye_mirror_cap(kl_env_int("KL_XR_EYE_MAX", 3456));
    }

    if (kl_driver_lifecycle_begin(stdout) != 0) return fail(kl_driver_error());
    return pump_and_report(kl_driver_pump_default(), view_pump);
}

static int native_door_run(int view_pump) {
    if (kl_driver_boot(stdout) != 0) return fail(kl_driver_error());
    if (kl_driver_gap_only()) return 0;
    return begin_and_pump(view_pump);
}

// Steam Link, which is the one target whose front door is a choice rather than a
// property of the tree. All three are the same guest and the same profile — the
// pairing credential the shell stores is exactly what the VR door is handed at
// the handoff — so what a run selects is only which chain it maps and which
// entry it calls.
static int steamlink_run(int view_pump) {
    if (kl_driver_boot(stdout) != 0) return fail(kl_driver_error());
    if (kl_driver_gap_only()) return 0;

    // The VR door without KL_SLINK_MAIN is a measurement of its own: the chain,
    // the init arrays and ANativeActivity_onCreate, which is where a glue that
    // returned early shows up as an empty callback table. Nothing after it runs,
    // so the guest never reaches OpenXR.
    if (DOOR_IS_VR && !getenv("KL_SLINK_MAIN")) {
        printf("\n=== ANativeActivity_onCreate (the VR front door) ===\n");
        fflush(NULL);
        if (kl_slink_vr_create(stdout) != 0) return fail(kl_slink_error());
        printf("  (KL_SLINK_MAIN unset: stopping after onCreate)\n");
        kl_driver_report(stdout);
        return 0;
    }
    // ...and the SDL doors without it stop one step earlier still, at the bound
    // chain: `main` is where everything unimplemented starts failing by name, and
    // the chain gate has to stay green independently of that.
    if (!DOOR_IS_VR && !getenv("KL_SLINK_MAIN") && !view_pump) {
        kl_driver_report(stdout);
        return 0;
    }
    return begin_and_pump(view_pump);
}

static int recon_run(int view_pump) {
    // Every thread that runs guest code seeds bionic's stack-guard canary into
    // TSD slot 5 first, and this is the thread the whole run is on. Without it
    // the slot holds whatever Darwin last left there, libSDL3's JNI_OnLoad copies
    // that at entry, something writes the slot during its 66 RegisterNatives
    // calls, and the epilogue reports "stack smashing detected" — a real canary
    // mismatch with no buffer overflow anywhere near it. libUE4 and the OpenJK
    // engine have stack protectors too; Beat Saber's path to initJni has none,
    // which is the only reason it never needed this.
    kl_thread_init();
    install_fault_reporter();
    kl_mem_pressure_init();
    // Strict: an unimplemented *call* is fatal. Lookups are not, so this
    // stops only where the surface genuinely ends. KL_PERMISSIVE=1 flips it
    // to a zero return, which collects a whole batch in one run when pushing
    // into new territory — scouting only, since the guest then carries on
    // with answers we made up.
    //
    // Set BEFORE the door is chosen, with the texture dump: both are properties
    // of the run rather than of the engine, and they used to sit below the UE4
    // branch, which silently made KL_PERMISSIVE and KL_DUMP_TEXTURES do nothing
    // at all on a NativeActivity target — the batch-scouting mode, absent
    // exactly where a brand-new target most needs it.
    kl_jni_set_permissive(getenv("KL_PERMISSIVE") != NULL);

    // Armed before anything runs: texture uploads happen all through init and the
    // lifecycle, not just inside the frame pump.
    kl_egl_dump_textures(getenv("KL_DUMP_TEXTURES"));

    // Which door this target takes. The Unity sequence is not a default that
    // happens to fit every guest — it names libmain, NativeLoader and
    // UnityPlayer.initJni — so the others branch before any of it. All four live
    // in kl_driver, shared with the visionOS app.
    kl_driver_set_alarm(getenv("KL_ALARM")
                        ? (unsigned)strtoul(getenv("KL_ALARM"), NULL, 10) : 20);
    if (TARGET->kind == KL_GUEST_STEAMLINK) return steamlink_run(view_pump);
    if (TARGET->kind == KL_GUEST_UE4 || TARGET->kind == KL_GUEST_JKXR)
        return native_door_run(view_pump);

    if (kl_driver_boot(stdout) != 0) return fail(kl_driver_error());

    // The lifecycle, behind a knob: everything above is a gate that must stay
    // green, and everything below stops by name until the shim catches up.
    if (getenv("KL_LIFECYCLE")) {
        // With KL_GLFB_MTL=1, the eye textures Unity is about to ask for
        // become MTLTextures we allocated — the host stand-in for what
        // Compositor Services will hand over. Registered here because
        // ovrp_SetupEyeTexture2 arrives inside nativeRecreateGfxState.
        kl_mtl_provider_install();
        if (kl_driver_lifecycle_begin(stdout) != 0) return fail(kl_driver_error());

        // One nativeRender is the engine's first frame and it is almost all
        // setup — no scene is loaded and nothing is drawn yet, which is why
        // the GL surface looked so small. KL_FRAMES pumps the render loop
        // the way UnityPlayer's own thread would, draining the posted-task
        // queue between frames as the UI thread's looper does.
        // In view_pump mode (KL_VIEW) the pump instead runs until the viewer
        // window closes, paced to 72 Hz — the Quest 2 display frequency we
        // report through ovrp_GetSystemDisplayFrequency, so the Choreographer
        // ticks at the rate the engine believes the hardware runs at. The
        // watchdog alarm is not armed there: the window may stay open for
        // minutes, and a human at the keyboard IS the watchdog.
        const char *fenv = getenv("KL_FRAMES");
        unsigned frames = fenv ? (unsigned)strtoul(fenv, NULL, 10) : 0;
        if (frames || view_pump) {
            // Here rather than earlier because it wants il2cpp_init to have
            // decrypted the tables, and it is a scan of live memory — the guest
            // is between frames at this point, which is the quietest the heap
            // gets. It does nothing unless KL_DUMP_METADATA names a file.
            //
            // KL_DUMP_METADATA_AT=end moves it to after the pump instead, for a
            // loader that decrypts LAZILY: at init only whatever
            // MetadataCache::Initialize touched is plaintext, and the tables the
            // game reaches for while it runs are not. `init` stays the default
            // because a guest that dies in the pump still gets a dump.
            const char *mdat = getenv("KL_DUMP_METADATA_AT");
            int md_late = mdat && !strcmp(mdat, "end");
            if (!md_late) {
                char meta[1024];
                kl_metadump_run(metadata_path(meta, sizeof meta));
            }
            if (view_pump)
                printf("\n=== recon: pumping frames until the viewer closes ===\n");
            else
                printf("\n=== recon: pumping %u frames ===\n", frames);
            fflush(NULL);
            const char *aenv2 = getenv("KL_ALARM");
            unsigned budget = aenv2 ? (unsigned)strtoul(aenv2, NULL, 10) : 60;
            // One watchdog around the whole pump rather than the driver's
            // per-frame one, and none at all under the viewer: the window may
            // stay open for minutes, and a human at the keyboard IS the watchdog.
            kl_driver_set_alarm(0);
            if (!view_pump) alarm(budget);
            // KL_SAMPLE_MS: while the pump runs, sample every guest thread's
            // pc/backtrace and resolve it (kl_sample.c) — built to name the
            // loop the loading-pace arc kept measuring. The metadata sits next
            // to the libs in the unpacked APK tree.
            const char *senv = getenv("KL_SAMPLE_MS");
            int sampling = 0;
            if (senv) {
                char meta[1024];
                sampling = kl_sample_start((unsigned)strtoul(senv, NULL, 10),
                                           metadata_path(meta, sizeof meta));
            }
            const long frame_ns = 1000000000L / 72;
            unsigned haptic_pulses = 0;
            unsigned i;
            for (i = 0; view_pump ? !g_view_quit : i < frames; i++) {
                struct timespec t0;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                // The guest's frame: pose latch, nativeRender, the posted-task
                // drain and the memory-pressure poll (kl_driver_frame). It
                // returns -1 when nativeRender was never registered, which is
                // the only way this loop ends early.
                if (kl_driver_frame() < 0) break;
                // The haptics seam's host end. Nothing on macOS can vibrate, so
                // this only drains and counts — but draining is the point: it
                // is what makes the host a working A/B for the whole path down
                // to the actuator, and KL_HAPTICS_TRACE=1 then prints the same
                // pulses a headset would have played. (The queue does not need
                // it to stay healthy; it retires samples on the wall clock
                // whether anyone is listening or not.)
                for (int hand = 0; hand < 2; hand++) {
                    float amp, secs;
                    if (kl_ovrp_haptics_pull(hand, &amp, &secs)) haptic_pulses++;
                }
                // KL_PROBE_INPUT: ask Unity's own managed API what it sees —
                // joystick count, the bound axes, the XR node poses. Between
                // frames on the thread that just ran one, which is where
                // managed calls are safe.
                kl_mprobe_tick(i);
                if (view_pump) {
                    struct timespec t1;
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    long used = (t1.tv_sec - t0.tv_sec) * 1000000000L +
                                (t1.tv_nsec - t0.tv_nsec);
                    if (used < frame_ns) {
                        struct timespec rem = { 0, frame_ns - used };
                        nanosleep(&rem, NULL);
                    }
                }
            }
            alarm(0);
            if (sampling) kl_sample_stop_report(stdout);
            if (md_late) {
                char meta[1024];
                kl_metadump_run(metadata_path(meta, sizeof meta));
            }
            if (haptic_pulses)
                printf("  haptic pulses drained: %u (nothing to play them on "
                       "here — see kl_ovrp.h)\n", haptic_pulses);
            report_eye_interop();
            const char *sd = getenv("KL_DUMP_SHADERS");
            if (sd) kl_egl_dump_shaders(sd);
            if (getenv("KL_DUMP_TEXTURES"))
                printf("  wrote %u texture upload%s as PNG\n",
                       kl_egl_texture_count(),
                       kl_egl_texture_count() == 1 ? "" : "s");
        }
    }

    if (view_pump)
        return 0;   // the main thread prints the reports after the join
    kl_driver_report(stdout);
    kl_metadump_watch_report(stdout);   // host-only, so not the driver's
    fflush(NULL);   // _exit does not flush stdio, and the report is the point
    _exit(0);
}

// The KL_VIEW guest thread: the same recon sequence the re-exec'd child runs,
// but in-process and pumping until the window closes. kl_thread_init() first —
// this thread runs guest code, and guest code needs its TLS slot.
static void *view_guest_thread(void *arg) {
    (void)arg;
    kl_thread_init();
    recon_run(1);
    return NULL;
}

// KL_VIEW=1: the interactive frontend (kl_view.c). This deliberately skips
// BOTH process games main() normally plays:
//  - the DRM-guard fork test, because the guard itself lives in kl_ovrplat.c
//    and is unaffected by who forks, and
//  - the re-exec, because Metal's XPC shader compiler refuses forked children
//    (the AGX abort story above) and a windowed app never forks in the first
//    place — in-process is the whole point.
// The guest runs on a spawned thread; the main thread runs SDL, because macOS
// requires windowing on the main thread.
static int view_run(void) {
    // kl_env_on, not getenv: this has to agree with kl_glfb_enabled() about what
    // KL_GLFB=0 means, or the viewer starts on the strength of a knob the
    // renderer read as off and then displays nothing.
    //
    // ...unless the guest's API is Vulkan, and that is not knowable here: it is
    // measured by kl_vulkan_guest_active() long after this point, from what the
    // guest actually did. A Vulkan guest never brings ANGLE up at all, and its
    // eye textures reach the compositor from MoltenVK instead
    // (kl_glfb_note_eye_mtl_texture), so KL_GLFB has nothing to do with whether
    // there is a picture. Refusing here would refuse the one target whose
    // compositing is new. So this is a NOTE now, not a refusal — the viewer
    // waits for an eye texture from whichever source produces one, and says so
    // in its HUD if none ever arrives.
    if (!kl_env_on("KL_GLFB", 0))
        fprintf(stderr, "view: KL_GLFB is not set — nothing will be displayed "
                        "unless the guest turns out to render through VULKAN, "
                        "whose eye textures do not come from ANGLE\n");
    if (!kl_view_available()) {
        fprintf(stderr, "KL_VIEW=1 but t_boot was built without SDL3\n");
        return 1;
    }
    // The frame-out seam, and which of its two implementations to use. Decided
    // HERE, on the main thread, before the guest thread exists: kl_glfb_mtl_
    // device() brings ANGLE up to answer, and kl_glfb_init() is not something
    // two threads may race into.
    //
    // Hardware is the default — the guest's eye textures become MTLTextures we
    // allocated and the viewer composites one straight into the window's
    // CAMetalLayer, so no frame is ever read back or copied. KL_VIEW_CPU=1
    // keeps the old glReadPixels path, which is the A/B when the compositor
    // shows the wrong picture.
    int hw = 0;
    if (TARGET->kind == KL_GUEST_STEAMLINK && DOOR != KL_SLINK_VR) {
        // A FLAT guest (the shell and client doors): its picture is the default
        // framebuffer of an EGL window surface, not an eye texture keyed by
        // (eye, stage). Requested HERE, before ANGLE is up, that framebuffer's
        // storage is an IOSurface kl_glfb allocates — the compositor samples
        // the guest's own pixels and nothing reads back. The panel size is
        // already published (kl_slink_configure ran in main), so bringing
        // ANGLE up now creates the surface at the size the guest will draw.
        // KL_VIEW_CPU=1 keeps the glReadPixels path as the A/B.
        //
        // The VR door is NOT flat: its eyes are mirror-blitted into provider
        // textures keyed (eye, stage) — the same seam the device compositor
        // samples — so it takes the hardware chain below like every XR door.
        // Keying this on the TARGET rather than the DOOR once made a VR viewer
        // run pay the mirror blits AND a full-frame glReadPixels per frame.
        if (!getenv("KL_VIEW_CPU") && kl_env_on("KL_GLFB", 0)) {
            kl_glfb_request_flat_surface();
            if (kl_glfb_mtl_device() && kl_glfb_flat_surface(NULL, NULL)) {
                hw = 1;
                fprintf(stderr, "view: flat guest — IOSurface composite, "
                                "no readback\n");
            }
        }
        if (!hw) fprintf(stderr, "view: flat guest — readback path\n");
    } else if (getenv("KL_VIEW_CPU")) {
        fprintf(stderr, "view: KL_VIEW_CPU=1 — readback path\n");
    } else if (!kl_env_on("KL_GLFB", 0)) {
        // No GL renderer was asked for, so there is no readback to fall back
        // to and nothing to bring ANGLE up for. The compositor path is the only
        // one that can show anything, and it finds the guest's eye texture by
        // itself whichever API produced it — which for a Vulkan guest is the
        // only way a picture exists at all.
        hw = 1;
        fprintf(stderr, "view: no GL renderer — the compositor will wait for an "
                        "eye texture (Vulkan)\n");
    } else if (!kl_glfb_mtl_device()) {
        fprintf(stderr, "view: no MTLDevice from ANGLE — readback path\n");
    } else {
        kl_mtl_provider_install();          // installs unconditionally under KL_VIEW
        hw = kl_glfb_has_mtl_provider();
    }
    if (!hw) kl_glfb_set_frame_sink(kl_view_frame_sink, NULL);

    // What the display actually runs at, before the guest can ask. Without this
    // every host run describes the Quest 2's 72 Hz panel no matter what is on
    // the desk, and a guest that paces a video stream against the number is
    // asking its far end for frames at a rate nothing here chose.
    //
    // It is a ceiling, not a promise: a heavy target can deliver a third of
    // this. The viewer's HUD prints achieved against advertised every second,
    // and KL_DISPLAY_HZ (read inside kl_ovrp_display_frequency, hence after
    // this push) is how a run pins the number to what it can really sustain.
    // For Steam Link the number is also the far end's encode rate, and the
    // pixel count is the host's choice, so every hertz here divides a bitrate
    // it chose into thinner frames.
    float panel_hz = kl_view_display_hz();
    if (panel_hz > 0.0f) kl_ovrp_set_display_frequency(panel_hz);
    fprintf(stderr, "view: display %.1f Hz%s; guest will be told %.1f Hz\n",
            (double)panel_hz, panel_hz > 0.0f ? "" : " (SDL could not say)",
            (double)kl_ovrp_display_frequency());

    // The guest's frame clock, for the guests that own their own frame loop.
    // Nothing else on the host paces one: the compositor takes the newest frame
    // and never blocks its producer, so an OpenXR guest spins as fast as it can
    // render — Steam Link's VR client ran at ~1000 Hz against a 120 Hz window,
    // paying every mirror blit eight times over and reporting frame pacing to
    // its streaming host against a clock nobody drove.
    //
    // Which pacer a guest wants is a property of the XR RUNTIME it drives, not
    // of the door it came through, and this is the same pairing the device app
    // makes. A Unity guest is NOT in this set: kl_driver_frame already calls its
    // frame from this side, so a pacer there would be the compositor waiting on
    // itself. KL_XR_PACE=0 restores the free-running loop as the A/B.
    int owns_loop = (TARGET->kind == KL_GUEST_STEAMLINK && DOOR == KL_SLINK_VR) ||
                    TARGET->kind == KL_GUEST_JKXR;
    if (owns_loop && kl_env_on("KL_XR_PACE", 1)) {
        kl_openxr_set_frame_pacer(kl_view_pace_wait);
        fprintf(stderr, "view: the guest owns its frame loop — paced by the "
                        "composite at %.1f Hz\n",
                (double)kl_ovrp_display_frequency());
    }

    pthread_t guest;
    if (pthread_create(&guest, NULL, view_guest_thread, NULL)) {
        fprintf(stderr, "view: pthread_create failed\n");
        return 1;
    }
    int rc = kl_view_main(LIBDIR, hw);   // returns when the window closes
    g_view_quit = 1;
    // The flat guest is inside its own main() and does not return on its own;
    // the process exiting is what ends it. Detaching rather than joining makes
    // closing the window close the app, which is what a window close means.
    if (TARGET->kind == KL_GUEST_STEAMLINK) pthread_detach(guest);
    else                                    pthread_join(guest, NULL);
    kl_driver_report(stdout);
    kl_metadump_watch_report(stdout);   // host-only, so not the driver's
    return rc;
}

int main(int argc, char **argv) {
    TARGET = kl_target_resolve(argc > 1 ? argv[1] : NULL);
    if (!TARGET) {
        fprintf(stderr, "FAIL: unknown target '%s' — one of: %s "
                        "(or a path to a guest lib directory)\n",
                argv[1], kl_target_names());
        return 1;
    }
    LIBDIR = TARGET->libdir;
    if (argc > 0 && argv[0] && *argv[0]) g_argv0 = argv[0];
    printf("=== target: %s (%s, %s) ===\n", TARGET->name, LIBDIR, TARGET->apk);

    // Everything the guest is told about itself, from one row: the library path,
    // the assets, the APK it opens as a zip, and the userdata directory its saves
    // land in. Steam Link is told the same four things plus what only it has —
    // which front door, the activity class that goes with it, Qt's plugin path
    // and the panel size — so its description comes from kl_slink instead.
    if (TARGET->kind == KL_GUEST_STEAMLINK) {
        DOOR = kl_slink_door_from_env();
        if (kl_env_on("KL_SLINK_VR", 0) && kl_env_on("KL_SLINK_SHELL", 0))
            printf("(KL_SLINK_VR and KL_SLINK_SHELL are different front doors; "
                   "taking VR)\n");
        // One profile for all three doors: the pairing credential the shell
        // stores is exactly what the VR door is handed at the handoff, so
        // splitting them would make the app re-pair against itself.
        if (kl_slink_configure(DOOR, LIBDIR, TARGET->assets, TARGET->apk,
                               kl_userdata_dir(TARGET->userdata), stdout) != 0)
            return fail(kl_slink_error());
        // Only the shell can reach startVRLink, and only it should be able to
        // hand off — installing this unconditionally would let a client or VR
        // run take a path neither of them has any business on.
        if (DOOR == KL_SLINK_SHELL) kl_jni_set_vrlink_handoff(slink_vrlink_handoff);
    } else {
        kl_target_apply_host(TARGET, NULL);
    }
    kl_driver_init(TARGET, LIBDIR, DOOR);

    // Re-entry of the re-exec'd recon child (see below).
    if (getenv("KL_RECON_CHILD"))
        return recon_run(0);

    if (getenv("KL_VIEW"))
        return view_run();

    printf("=== DRM policy guard ===\n");
    // Under a debugger the forked child inherits the parent's Mach exception
    // ports, so its abort is intercepted and never reads as a SIGABRT to
    // waitpid. KL_SKIP_GUARD_TEST skips this self-test when running under
    // lldb — the guard itself in kl_ovrplat.c is unaffected.
    if (!getenv("KL_SKIP_GUARD_TEST") && check_drm_guard()) return 1;

    // ---- phase 2: reconnaissance, in a child ----
    // Unimplemented JNI slots abort the process on purpose, so this runs in a
    // child. Whatever it prints before dying is the work list.
    //
    // The child is re-EXEC'd, not a bare fork. Metal's shader compiler is an
    // XPC service that refuses forked children, and AGX treats the resulting
    // cold-compile failure of one of its OWN internal blit shaders as fatal
    // (the long-standing "AGX abort"): the failure only showed once the driver
    // needed a compile — a blit in texture setup — and only ever in the child,
    // which is why every standalone recipe was clean. exec resets the XPC
    // state, so the re-exec'd child compiles like any other process.
    // KL_NOFORK=1 skips the child entirely: macOS lldb does not follow exec
    // any better than fork, so debugging the guest needs the guest threads in
    // the traced process, and the debugger becomes the reporter.
    printf("\n=== recon: spawning child ===\n");
    fflush(NULL);
    if (getenv("KL_NOFORK"))
        return recon_run(0);

    pid_t pid = fork();
    if (pid == 0) {
        setenv("KL_RECON_CHILD", "1", 1);
        execl(argv[0], argv[0], LIBDIR, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st) || WEXITSTATUS(st) != 0) {
        // How it stopped is the first question every time: an unimplemented JNI
        // slot aborts (SIGABRT after the report), SIGALRM means the guest
        // blocked, and anything else is a real fault in guest code.
        if (WIFSIGNALED(st))
            printf("\n  (recon stopped on signal %d — %s)\n", WTERMSIG(st),
                   strsignal(WTERMSIG(st)));
        else
            printf("\n  (recon child exited %d)\n", WEXITSTATUS(st));
        printf("  (see the JNI surface report above)\n");
        // Say what was OBSERVED, not what is usually true. This used to report
        // every non-zero child as "an unimplemented JNI call was reached", which
        // is right only for the abort-by-name path — and it named a JNI stop for
        // a run whose actual cause was CPython dying on a refused getrandom,
        // sending the search to the JNI surface report, where there was nothing
        // to find. A diagnostic that asserts a cause it has not established is
        // worse than one that only reports the exit.
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT)
            return fail("guest init did not complete: aborted — an unimplemented "
                        "JNI call or unresolved import (the report names it)");
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM)
            return fail("guest init did not complete: the watchdog fired — the "
                        "guest blocked (KL_ALARM widens it)");
        if (WIFSIGNALED(st))
            return fail("guest init did not complete: a fault in guest code — "
                        "read the fault line above, not the JNI report");
        return fail("guest init did not complete: the guest EXITED on its own "
                    "(no signal) — something in it called exit(); the reason is "
                    "in the log above, not in the JNI report");
    }
    // Named for the door this run actually took. `initJni` is UnityPlayer's and
    // means nothing on a NativeActivity guest — a diagnostic asserting something
    // it never established sends the next reader to the wrong report.
    printf("\n=== EXIT CRITERION MET: %s ===\n",
           TARGET->kind == KL_GUEST_UE4       ? "the NativeActivity lifecycle ran with no unimplemented JNI calls"
           : TARGET->kind == KL_GUEST_JKXR    ? "the GLES3JNILib lifecycle ran with no unimplemented JNI calls"
           : TARGET->kind == KL_GUEST_STEAMLINK ? "the Steam Link chain ran with no unimplemented JNI calls"
                                               : "initJni completed with no unimplemented JNI calls");
    return 0;
}
