// M3 gate: enter the guest through its real entry point.
//
// The plan said this milestone ends at ANativeActivity_onCreate. It does not —
// there is no NativeActivity in this APK. AndroidManifest.xml declares
// com.unity3d.player.UnityPlayerActivity, and libmain.so exports exactly one
// thing: JNI_OnLoad. So the true entry is a JNI call, and the gate is that
// libmain's JNI_OnLoad runs against our synthetic JavaVM and registers the two
// natives the Java side expects.
//
// Phase 1 is the assertion. Phase 2 is reconnaissance: it drives the registered
// NativeLoader.load() to find out what the *next* milestone has to answer for,
// and runs in a forked child because an unimplemented JNI slot aborts by design.
#include <ctype.h>
#include <dlfcn.h>
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
#include "../runtime/kl_glfb.h"
#include "../runtime/kl_jni.h"
#include "../runtime/kl_egl.h"
#include "../runtime/kl_opensl.h"
#include "../runtime/kl_mediandk.h"
#include "../runtime/kl_ovrp.h"
#include "../runtime/kl_ovrplat.h"
#include "../runtime/kl_openxr.h"
#include "../runtime/kl_view.h"
#include "../runtime/kl_sample.h"
#include "../runtime/kl_mprobe.h"
#include "../runtime/kl_metadump.h"
#include "../runtime/kl_il2cpp.h"
#include "../runtime/kl_guestpoke.h"
#include "../runtime/kl_fault.h"
#include "../runtime/kl_target.h"
#include "../runtime/kl_ue4.h"
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
//
// The path form is what every command in CLAUDE.md and every Makefile gate
// passes, and it resolves to the same target as the name.
static const kl_target *TARGET;
static const char *LIBDIR;

typedef int  (*jni_onload_fn)(void *vm, void *reserved);
typedef int8_t (*nativeloader_load_fn)(void *env, void *clazz, void *path);

static int fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

// The fatal-signal reporter moved to runtime/kl_fault.c when the visionOS app
// needed the same thing inside a bundle (PLANNING §12.7) — it is a diagnostic
// that ships, not a harness detail. The sampling profiler stays host-only, so
// t_boot registers it rather than kl_fault.c referencing RUNTIME_DIAG.
static void install_fault_reporter(void) {
    kl_fault_add_reporter(kl_sample_stop_report);
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

// The guest's frame, read back through Metal — the P5 gate, and it belongs to
// the RUN rather than to the Unity lifecycle it used to be nested inside.
// Every non-Unity door reaches the eye textures the same way (kl_glfb's eye
// table), so a driver that stops before this reports a working pipeline as
// nothing at all — the same shape as m_boot never calling kl_mediandk_report.
static void report_eye_interop(void) {
    if (!kl_glfb_has_mtl_provider() && !kl_glfb_eye_mtl_texture(0, 0, NULL)) return;
    printf("\n=== P5 interop: the guest's frame, read back through Metal ===\n");
    // P5.3's gate. Not "did the interop bind" — that is reported when it
    // happens — but "did the guest's rendering arrive in the MTLTexture".
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
    // something nobody is looking at. That is trap 43's family: an
    // instrument answering confidently about the wrong subject.
    // Open Brush is where it bit (3 OpenXR swapchain images).
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
}

// The Unreal Engine 4 door. A different ENTRY, not a different lifecycle: the
// guest is started through ANativeActivity_onCreate and then runs its own game
// thread, so this drives it by delivering lifecycle and pumping the looper
// rather than by calling a render native the way the Unity path below does.
//
// Everything before the door is shared — the fault reporter, the permissive
// flag, the texture dump — and everything under it is shared too, which is the
// point of adding a non-Unity engine at all.
static int ue4_run(void) {
    // The guest's threads need x18 and the TLS veneer's slot before any guest
    // code runs (trap 1). The Unity path gets this from kl_app_boot; nothing
    // was calling it here, and libUE4 has stack protectors.
    kl_thread_init();

    if (kl_ue4_configure(LIBDIR, stdout) != 0) return fail(kl_ue4_error());
    if (kl_ue4_load(stdout) != 0) return fail(kl_ue4_error());

    // The work list, before anything is driven. For a brand-new target this is
    // the most valuable thing a run produces, and it is worth having even when
    // the boot below dies immediately: KL_GAP_ONLY stops here.
    printf("\n=== the shim gap ===\n");
    kl_ue4_gap(stdout);
    if (getenv("KL_GAP_ONLY")) { fflush(NULL); return 0; }

    printf("\n=== ANativeActivity_onCreate ===\n");
    fflush(NULL);
    const char *aenv = getenv("KL_ALARM");
    alarm(aenv ? (unsigned)strtoul(aenv, NULL, 10) : 20);
    if (kl_ue4_create(stdout) != 0) { alarm(0); return fail(kl_ue4_error()); }
    kl_ue4_start(stdout);
    alarm(0);

    // Seconds, not frames: a NativeActivity guest owns its own frame loop, so
    // there is no render call here to count. KL_UE4_WAIT is the budget.
    const char *wenv = getenv("KL_UE4_WAIT");
    double want = wenv ? strtod(wenv, NULL) : 5.0;
    printf("\n=== pumping the looper for %.1f s ===\n", want);
    fflush(NULL);
    alarm(aenv ? (unsigned)strtoul(aenv, NULL, 10) : (unsigned)(want + 30));
    double spent = kl_ue4_pump(want, NULL);
    alarm(0);
    printf("  pumped %.2f s\n", spent);

    printf("\n=== reports ===\n");
    kl_ue4_report(stdout);
    // ...and the P5 gate, which is the only thing here that says whether the
    // guest's rendering ARRIVED. This door reaches the eye textures exactly the
    // way BONELAB's does — kl_vulkan publishes the MTLTexture behind each eye
    // VkImage into kl_glfb's eye table — so leaving it to the Unity tail meant
    // a UE4 run producing a picture and a report that never mentioned one.
    report_eye_interop();
    fflush(NULL);
    return 0;
}

static int recon_run(int view_pump) {
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
    // at all on a NativeActivity target — the M4 loop's batch-scouting mode,
    // absent exactly where a brand-new target most needs it.
    kl_jni_set_permissive(getenv("KL_PERMISSIVE") != NULL);

    // Armed before anything runs: texture uploads happen all through init and the
    // lifecycle, not just inside the frame pump.
    kl_egl_dump_textures(getenv("KL_DUMP_TEXTURES"));

    // Which door this target takes. The Unity sequence below is not a default
    // that happens to fit every guest — it names libmain, NativeLoader and
    // UnityPlayer.initJni — so a UE4 target has to branch before any of it.
    if (TARGET && TARGET->kind == KL_GUEST_UE4) return ue4_run();

    char path[1024];
    snprintf(path, sizeof path, "%s/libmain.so", LIBDIR);

    printf("=== libmain.so entry ===\n");
    kl_image *main_img = kl_load_auto(path);
    if (!main_img) return fail(kl_error());
    kl_register_image("libmain.so", main_img);
    kl_run_init(main_img);

    jni_onload_fn onload = (jni_onload_fn)kl_sym(main_img, "JNI_OnLoad");
    if (!onload) return fail("libmain.so exports no JNI_OnLoad");

    int version;
    kl_jni_local_frame_push();      // the JVM would pop each native's local
    version = onload(kl_jni_vm(), NULL);   // frame on return; the host plays
    kl_jni_local_frame_pop();       // that half (see kl_jni.h)
    printf("  JNI_OnLoad returned 0x%08x\n", version);
    if (version != KL_JNI_VERSION_1_6)
        return fail("JNI_OnLoad did not return JNI_VERSION_1_6");

    // libmain registers com.unity3d.player.NativeLoader.{load,unload} — the
    // shim Unity's Java side calls to dlopen libunity.so.
    const char *CLS = "com/unity3d/player/NativeLoader";
    void *load   = kl_jni_native(CLS, "load", NULL);
    void *unload = kl_jni_native(CLS, "unload", NULL);
    if (!load || !unload) return fail("NativeLoader natives were not registered");
    printf("  registered %s.load=%p unload=%p\n", CLS, load, unload);

    printf("\n=== M3 EXIT CRITERION MET: guest JNI_OnLoad ran, natives registered ===\n");

    // ---- phase 2: reconnaissance, non-fatal ----
    printf("\n=== recon: driving NativeLoader.load(\"libunity.so\") ===\n");
    fflush(NULL);
    // load() takes the *directory* — it appends "/libunity.so" itself.
    kl_jni_local_frame_push();
    int8_t ok = ((nativeloader_load_fn)load)(kl_jni_env(), NULL,
                                             kl_jni_new_string(LIBDIR));
    kl_jni_local_frame_pop();
    printf("  NativeLoader.load returned %d\n", ok);

    // UnityPlayer's constructor calls initJni(Context) first (UnityPlayer.smali
    // line 372). It is `private final native`, so an instance method: the guest
    // sees (JNIEnv*, jobject thiz, jobject context). Both objects are opaque to
    // us — what matters is what libunity asks them for.
    void *initJni = kl_jni_native("com/unity3d/player/UnityPlayer", "initJni", NULL);
    if (initJni) {
        printf("\n=== recon: UnityPlayer.initJni(Context) ===\n");
        fflush(NULL);
        void *thiz = kl_jni_new_object("com/unity3d/player/UnityPlayer");
        // On device the Context is the Activity — AndroidManifest.xml declares
        // UnityPlayerActivity — and Unity checks that with IsInstanceOf. Handing
        // it a bare Context would send it down the no-Activity path.
        // The shared singleton, not a fresh instance: Unity also reads this
        // back through the static UnityPlayer.currentActivity and compares.
        void *context = kl_jni_activity();
        kl_jni_local_frame_push();
        ((void (*)(void *, void *, void *))initJni)(kl_jni_env(), thiz, context);
        kl_jni_local_frame_pop();
        printf("  initJni returned\n");
        // ...and the rest of that same constructor. See kl_jni.h: the helper
        // objects hand THEMSELVES to libunity, so a driver that stops at
        // initJni leaves handles libunity does not null-check.
        kl_jni_unity_construct_helpers();
    }

    // Lifecycle, in the order UnityPlayerActivity drives it: attach a
    // surface, resume, then pump one frame. This is where M4 runs into M5 —
    // nativeRecreateGfxState is what reaches for EGL.
    if (getenv("KL_LIFECYCLE")) {
        // P5.3: with KL_GLFB_MTL=1, the eye textures Unity is about to ask for
        // become MTLTextures we allocated — the host stand-in for what
        // Compositor Services will hand over. Registered here because
        // ovrp_SetupEyeTexture2 arrives inside nativeRecreateGfxState, below.
        kl_mtl_provider_install();
        void *surface = kl_jni_new_object("android/view/Surface");
        void *thiz2   = kl_jni_new_object("com/unity3d/player/UnityPlayer");
        struct { const char *name; int kind; } seq[] = {
            {"nativeRecreateGfxState", 2}, {"nativeResume", 0}, {"nativeRender", 1},
        };
        for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
            void *fn = kl_jni_native("com/unity3d/player/UnityPlayer", seq[i].name, NULL);
            if (!fn) { printf("  %s: not registered\n", seq[i].name); continue; }
            printf("\n=== recon: UnityPlayer.%s ===\n", seq[i].name);
            fflush(NULL);
            // The render loop may block; do not hang the sweep. KL_ALARM
            // widens the window when the question is what it is waiting on.
            const char *aenv = getenv("KL_ALARM");
            alarm(aenv ? (unsigned)strtoul(aenv, NULL, 10) : 20);
            kl_jni_local_frame_push();
            if (seq[i].kind == 2)
                ((void (*)(void *, void *, int, void *))fn)(kl_jni_env(), thiz2, 0, surface);
            else if (seq[i].kind == 1)
                printf("  -> %d\n", ((int8_t (*)(void *, void *))fn)(kl_jni_env(), thiz2));
            else
                ((void (*)(void *, void *))fn)(kl_jni_env(), thiz2);
            kl_jni_local_frame_pop();
            alarm(0);
            printf("  %s returned\n", seq[i].name);
            // Android's UI thread runs its looper between callbacks; here
            // the host has to pump it, or the queue only ever grows.
            alarm(10);
            unsigned ran = kl_jni_drain_ui_tasks();
            alarm(0);
            if (ran) printf("  drained %u posted task%s\n", ran, ran == 1 ? "" : "s");
        }

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
            void *fn = kl_jni_native("com/unity3d/player/UnityPlayer", "nativeRender", NULL);
            kl_guest_poke_texture_unit_cap();
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
            for (i = 0; fn && (view_pump ? !g_view_quit : i < frames); i++) {
                struct timespec t0;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                // Pin this frame's poses first, so every ovrp_GetNodePoseState
                // inside the frame answers the same thing and the pose recorded
                // for timewarp is the one the picture was drawn from. On the
                // host the frontend and the guest are usually the same thread,
                // so this changes nothing — but the viewer is what proves the
                // reprojection math before a device is available, and it can
                // only prove it if it runs the same shape.
                kl_ovrp_frame_latch();
                // The frame clock next: on Android the Choreographer's
                // doFrame is what wakes the engine for a frame, and
                // nativeRender then draws what it decided. Ticking after
                // rendering would hand every frame the previous one's time.
                // The frame clock is now a free-running host thread started at
                // the first Choreographer postFrameCallback — it is NOT driven
                // from here. Doing it inline is how the pump blocked forever: a
                // doFrame could only be delivered before nativeRender, and
                // nativeRender waits on the refresh counter a doFrame advances.
                // One local frame per pumped frame, as the JVM gives each
                // nativeRender on Android — the guest's per-frame locals are
                // meant to die here (see kl_jni.h).
                kl_jni_local_frame_push();
                ((int8_t (*)(void *, void *))fn)(kl_jni_env(), thiz2);
                kl_jni_local_frame_pop();
                kl_jni_drain_ui_tasks();
                // Android's low-memory notification, which nothing delivered
                // before this: one task_info call, and the guest drops its
                // caches when the footprint crosses the reported budget.
                kl_mem_pressure_poll();
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
            printf("  pumped %u frames\n", i);
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
    kl_jni_report(stdout);
    kl_egl_report(stdout);
    kl_opensl_report(stdout);
    // A Unity guest can reach the video path too — Open Brush seeds a video
    // into its own media library at startup — and this driver had never asked
    // it anything. The report was wired into the Steam Link driver and the
    // fault path only, so on a CLEAN Unity run a refused demux said nothing.
    kl_mediandk_report(stdout);
    kl_ovrp_report(stdout);
    kl_ovrplat_report(stdout);
    kl_openxr_report(stdout);
    kl_metadump_watch_report(stdout);
    kl_madv_report();
    kl_mem_report();
    fflush(NULL);   // _exit does not flush stdio, and the report is the point
    _exit(0);
}

// The KL_VIEW guest thread: the same recon sequence the re-exec'd child runs,
// but in-process and pumping until the window closes. kl_thread_init() first —
// this thread runs guest code, and guest code needs its TLS slot (trap 1).
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
    if (getenv("KL_VIEW_CPU")) {
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
    pthread_t guest;
    if (pthread_create(&guest, NULL, view_guest_thread, NULL)) {
        fprintf(stderr, "view: pthread_create failed\n");
        return 1;
    }
    int rc = kl_view_main(LIBDIR, hw);   // returns when the window closes
    g_view_quit = 1;
    pthread_join(guest, NULL);
    kl_jni_report(stdout);
    kl_egl_report(stdout);
    kl_opensl_report(stdout);
    // A Unity guest can reach the video path too — Open Brush seeds a video
    // into its own media library at startup — and this driver had never asked
    // it anything. The report was wired into the Steam Link driver and the
    // fault path only, so on a CLEAN Unity run a refused demux said nothing.
    kl_mediandk_report(stdout);
    kl_ovrp_report(stdout);
    kl_ovrplat_report(stdout);
    kl_openxr_report(stdout);
    kl_metadump_watch_report(stdout);
    kl_madv_report();
    kl_mem_report();
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
    // Everything the guest is told about itself, from one row: the library
    // path, the assets, the APK it opens as a zip, and the userdata directory
    // its saves land in.
    kl_target_apply_host(TARGET, NULL);
    printf("=== target: %s (%s, %s) ===\n", TARGET->name, LIBDIR, TARGET->apk);

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
    // child. Whatever it prints before dying is the M4 work list.
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
    // means nothing on a NativeActivity guest — printing it after a UE4 run is
    // a diagnostic asserting something it never established, which is the habit
    // this tree keeps having to unlearn (the getrandom stop that reported
    // itself as "an unimplemented JNI call" is the same shape).
    printf("\n=== M4 (partial): %s ===\n",
           TARGET && TARGET->kind == KL_GUEST_UE4
               ? "the NativeActivity lifecycle ran with no unimplemented JNI calls"
               : "initJni completed with no unimplemented JNI calls");
    return 0;
}
