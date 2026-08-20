// See kl_driver.h.
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "klepton.h"
#include "kl_driver.h"
#include "kl_egl.h"
#include "kl_env.h"
#include "kl_guestpoke.h"
#include "kl_jkxr.h"
#include "kl_jni.h"
#include "kl_mediandk.h"
#include "kl_opensl.h"
#include "kl_openxr.h"
#include "kl_ovrp.h"
#include "kl_ovrplat.h"
#include "kl_slink.h"
#include "kl_ue4.h"

typedef int    (*jni_onload_fn)(void *vm, void *reserved);
typedef int8_t (*nativeloader_load_fn)(void *env, void *clazz, void *path);

static const kl_target *g_target;
static char             g_libdir[1024];
static kl_slink_door    g_door;
static char             g_error[512];
static void           (*g_phase_hook)(const char *);
static unsigned         g_alarm = 20;
static unsigned         g_frames;
static int              g_gap_only;

// The Unity guest's handle and its resolved nativeRender, kept between _begin
// and each _frame. A NULL g_render is how _frame knows _begin has not run.
static void *g_thiz, *g_render;

static void phase(const char *p) { if (g_phase_hook) g_phase_hook(p); }

static int fail(const char *msg) {
    snprintf(g_error, sizeof g_error, "%s", msg ? msg : "(no reason given)");
    return 1;
}

// Every print goes through here so `out` may be NULL at any call site.
static void P(FILE *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void P(FILE *out, const char *fmt, ...) {
    if (!out) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

void kl_driver_init(const kl_target *t, const char *libdir, kl_slink_door door) {
    g_target = t;
    g_door   = door;
    g_error[0] = 0;
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir ? libdir : "");
}

static kl_guest_kind kl_driver_kind(void) {
    return g_target ? g_target->kind : KL_GUEST_UNITY;
}

const char      *kl_driver_error(void)  { return g_error; }
int              kl_driver_gap_only(void) { return g_gap_only; }
unsigned         kl_driver_frames(void) { return g_frames; }
void             kl_driver_note_frame(void) { g_frames++; }
void             kl_driver_set_phase_hook(void (*fn)(const char *)) { g_phase_hook = fn; }
void             kl_driver_set_alarm(unsigned s) { g_alarm = s; }

int kl_driver_owns_frame_loop(void) {
    switch (kl_driver_kind()) {
    case KL_GUEST_STEAMLINK:
    case KL_GUEST_UE4:
    case KL_GUEST_JKXR: return 1;
    default:            return 0;
    }
}

// ---- boot ------------------------------------------------------------------

static int boot_unity(FILE *out) {
    char path[1200];
    snprintf(path, sizeof path, "%s/libmain.so", g_libdir);

    phase("libmain");
    P(out, "=== libmain.so entry ===\n");
    kl_image *img = kl_load_auto(path);
    if (!img) return fail(kl_error());
    kl_register_image("libmain.so", img);
    kl_run_init(img);

    jni_onload_fn onload = (jni_onload_fn)kl_sym(img, "JNI_OnLoad");
    if (!onload) return fail("libmain.so exports no JNI_OnLoad");

    kl_jni_local_frame_push();          // the JVM would pop each native's local
    int version = onload(kl_jni_vm(), NULL);   // frame on return; the host plays
    kl_jni_local_frame_pop();           // that half (see kl_jni.h)
    P(out, "  JNI_OnLoad returned 0x%08x\n", version);
    if (version != KL_JNI_VERSION_1_6)
        return fail("JNI_OnLoad did not return JNI_VERSION_1_6");

    // libmain registers com.unity3d.player.NativeLoader.{load,unload} — the shim
    // Unity's Java side calls to dlopen libunity.so.
    const char *CLS = "com/unity3d/player/NativeLoader";
    void *load = kl_jni_native(CLS, "load", NULL);
    if (!load || !kl_jni_native(CLS, "unload", NULL))
        return fail("NativeLoader natives were not registered");
    P(out, "  registered %s.load=%p\n", CLS, load);
    P(out, "\n=== EXIT CRITERION MET: guest JNI_OnLoad ran, natives registered ===\n");

    // load() takes the *directory* — it appends "/libunity.so" itself.
    phase("NativeLoader.load");
    P(out, "\n=== NativeLoader.load(\"%s\") ===\n", g_libdir);
    kl_jni_local_frame_push();
    int8_t ok = ((nativeloader_load_fn)load)(kl_jni_env(), NULL,
                                             kl_jni_new_string(g_libdir));
    kl_jni_local_frame_pop();
    P(out, "  NativeLoader.load returned %d\n", ok);
    if (!ok) return fail("NativeLoader.load could not bring up libunity.so");

    // UnityPlayer's constructor calls initJni(Context) first. It is
    // `private final native`, so the guest sees (JNIEnv*, jobject thiz, jobject
    // context). The Context must be the Activity — the manifest declares
    // UnityPlayerActivity and Unity checks with IsInstanceOf — and the shared
    // singleton, because Unity reads it back through the static
    // UnityPlayer.currentActivity and compares.
    void *initJni = kl_jni_native("com/unity3d/player/UnityPlayer", "initJni", NULL);
    if (!initJni) return fail("UnityPlayer.initJni was never registered");
    phase("initJni");
    P(out, "\n=== UnityPlayer.initJni(Context) ===\n");
    void *thiz = kl_jni_new_object("com/unity3d/player/UnityPlayer");
    kl_jni_local_frame_push();
    ((void (*)(void *, void *, void *))initJni)(kl_jni_env(), thiz, kl_jni_activity());
    kl_jni_local_frame_pop();
    P(out, "  initJni returned\n");
    // Here rather than at libmain: the chain dlopens libunity.so and libil2cpp.so
    // on its way through initJni, so this is the first moment the map is complete.
    kl_dl_report_images(out);
    // ...and the rest of that same constructor: the helper objects hand
    // THEMSELVES to libunity, and it does not null-check the handles.
    kl_jni_unity_construct_helpers();
    P(out, "\n=== EXIT CRITERION MET: initJni completed, no unimplemented "
           "JNI calls ===\n");
    return 0;
}

// Steam Link's seven natives are STATIC exports, resolved by name off libmain
// rather than through RegisterNatives — the mix is the measurement, and it is
// the streaming client's alone: the shell has no video surface to hand anyone.
static const char *const SL_STATIC_NATIVES[] = {
    "Java_com_valvesoftware_steamlink_SteamLink_useVideoSurface",
    "Java_com_valvesoftware_steamlink_SteamLink_videoSurfaceCreated",
    "Java_com_valvesoftware_steamlink_SteamLink_videoSurfaceDestroyed",
    "Java_com_valvesoftware_steamlink_SteamLink_overlaySurfaceCreated",
    "Java_com_valvesoftware_steamlink_SteamLink_overlaySurfaceDestroyed",
    "Java_com_valvesoftware_steamlink_SteamLink_freezeRendering",
    "Java_com_valvesoftware_steamlink_SteamLink_thawRendering",
};

static int boot_steamlink(FILE *out) {
    P(out, "=== Steam Link: %s front door (%s -> %s), %s ===\n",
      kl_slink_door_name(), kl_slink_main_lib(), kl_slink_main_fn(), g_libdir);

    // Mapping and relocation are separated from DT_INIT_ARRAY on purpose: an
    // unresolved import aborts by name when it is CALLED and the first init
    // array calls one, so running inits as we go would stop the run before the
    // gap could be printed and the shim work list would arrive one symbol per
    // rebuild.
    phase("steamlink chain");
    P(out, "=== mapping the working set ===\n");
    if (kl_slink_load_chain(out) != 0) return fail(kl_slink_error());
    kl_slink_report_gap(out);
    if (g_gap_only) return 0;

    phase("steamlink inits");
    P(out, "\n=== DT_INIT_ARRAY, dependencies first ===\n");
    kl_slink_run_inits(out);

    // The VR front door diverges here and never rejoins. Everything below is
    // SDL3's, and libvrlink_scene has none of it: no libSDL3 in its DT_NEEDED,
    // no JNI_OnLoad export, no natives to register. Its whole entry is the one
    // function Android's NativeActivity dlsyms.
    if (g_door == KL_SLINK_VR) {
        P(out, "\n=== EXIT CRITERION MET: the VR chain is bound and "
               "initialised ===\n");
        return 0;
    }

    phase("SDL3 JNI_OnLoad");
    P(out, "\n=== libSDL3.so JNI_OnLoad ===\n");
    if (kl_slink_sdl_onload(out) != 0) return fail(kl_slink_error());

    if (g_door == KL_SLINK_CLIENT) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", g_libdir, kl_slink_main_lib());
        kl_image *entry = kl_find_image(path);
        unsigned n = 0;
        for (size_t i = 0; entry && i < sizeof SL_STATIC_NATIVES / sizeof *SL_STATIC_NATIVES; i++)
            if (kl_sym(entry, SL_STATIC_NATIVES[i])) n++;
        P(out, "  libmain static Java_* natives resolved: %u/%zu\n",
          n, sizeof SL_STATIC_NATIVES / sizeof *SL_STATIC_NATIVES);
    }

    // SDL.setupJNI() caches the activity class and every method id SDL3 will
    // call back through — the densest single JNI call in the app, and therefore
    // where the surface starts failing by name.
    phase("SDL.setupJNI");
    P(out, "\n=== SDL.setupJNI() ===\n");
    kl_slink_sdl_setup(out);
    P(out, "\n=== EXIT CRITERION MET: the chain is bound, SDL3 JNI_OnLoad "
           "ran ===\n");
    return 0;
}

static int boot_ue4(FILE *out) {
    phase("ue4 configure");
    if (kl_ue4_configure(g_libdir, out) != 0) return fail(kl_ue4_error());
    phase("ue4 chain");
    if (kl_ue4_load(out) != 0) return fail(kl_ue4_error());
    P(out, "\n=== the shim gap ===\n");
    kl_ue4_gap(out);
    if (g_gap_only) return 0;
    P(out, "\n=== EXIT CRITERION MET: the Unreal chain is bound and "
           "initialised ===\n");
    return 0;
}

static int boot_jkxr(FILE *out) {
    phase("jkxr configure");
    // The libdir must be ABSOLUTE for this door: the engine chdirs into its own
    // data directory inside onCreate, and every relative path handed to it stops
    // resolving at that moment.
    if (kl_jkxr_configure(g_libdir, g_target->entry_lib, out) != 0)
        return fail(kl_jkxr_error());
    phase("jkxr chain");
    if (kl_jkxr_load(out) != 0) return fail(kl_jkxr_error());
    P(out, "\n=== the shim gap ===\n");
    kl_jkxr_gap(out);
    if (g_gap_only) return 0;
    P(out, "\n=== EXIT CRITERION MET: the OpenJK chain is bound and "
           "initialised ===\n");
    return 0;
}

int kl_driver_boot(FILE *out) {
    if (!g_target) return fail("kl_driver_init was not called");
    g_gap_only = kl_env_on("KL_GAP_ONLY", 0);
    int rc;
    switch (kl_driver_kind()) {
    case KL_GUEST_STEAMLINK: rc = boot_steamlink(out); break;
    case KL_GUEST_UE4:       rc = boot_ue4(out);       break;
    case KL_GUEST_JKXR:      rc = boot_jkxr(out);      break;
    default:                 rc = boot_unity(out);     break;
    }
    if (rc == 0 && g_gap_only)
        P(out, "\n(KL_GAP_ONLY: stopping at the shim gap — nothing was run)\n");
    return rc;
}

// ---- the lifecycle ---------------------------------------------------------

static int begin_unity(FILE *out) {
    g_thiz = kl_jni_new_object("com/unity3d/player/UnityPlayer");
    if (!g_thiz) return fail("kl_driver_boot must run first");

    // The order UnityPlayerActivity drives: attach a surface, resume, then one
    // frame. nativeRecreateGfxState is what reaches for EGL, and it is also what
    // pulls in libil2cpp — the image the boot gate never loads.
    void *surface = kl_jni_new_object("android/view/Surface");
    struct { const char *name; int kind; } seq[] = {
        { "nativeRecreateGfxState", 2 }, { "nativeResume", 0 }, { "nativeRender", 1 },
    };
    for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        void *fn = kl_jni_native("com/unity3d/player/UnityPlayer", seq[i].name, NULL);
        if (!fn) { P(out, "  %s: not registered\n", seq[i].name); continue; }
        phase(seq[i].name);
        P(out, "\n=== UnityPlayer.%s ===\n", seq[i].name);
        // The render loop may block; the watchdog is what names WHERE, since a
        // SIGALRM is reported as "still alive and blocked" rather than a crash.
        if (g_alarm) alarm(g_alarm);
        kl_jni_local_frame_push();
        if (seq[i].kind == 2)
            ((void (*)(void *, void *, int, void *))fn)(kl_jni_env(), g_thiz, 0, surface);
        else if (seq[i].kind == 1)
            P(out, "  -> %d\n", ((int8_t (*)(void *, void *))fn)(kl_jni_env(), g_thiz));
        else
            ((void (*)(void *, void *))fn)(kl_jni_env(), g_thiz);
        kl_jni_local_frame_pop();
        if (g_alarm) alarm(0);
        P(out, "  %s returned\n", seq[i].name);
        // Android's UI thread runs its looper between callbacks; here nothing
        // else will, so the queue would only grow.
        if (g_alarm) alarm(g_alarm);
        unsigned ran = kl_jni_drain_ui_tasks();
        if (g_alarm) alarm(0);
        if (ran) P(out, "  drained %u posted task%s\n", ran, ran == 1 ? "" : "s");
    }

    g_render = kl_jni_native("com/unity3d/player/UnityPlayer", "nativeRender", NULL);
    // The graphics device exists by now and no frame has been drawn yet. The
    // raise lets libunity bind past its un-queried cap of 32 texture units
    // instead of refusing every bind and reading stale unit-0 textures; it is
    // declined per Unity build, and names the build when it declines.
    kl_guest_poke_texture_unit_cap();
    phase("frame pump");
    return 0;
}

static int begin_steamlink(FILE *out) {
    if (g_door != KL_SLINK_VR) {
        phase("SDL onCreate -> main");
        P(out, "\n=== onCreate -> %s ===\n", kl_slink_main_fn());
        if (kl_slink_sdl_start_main(out) != 0) return fail(kl_slink_error());
        phase("guest running");
        return 0;
    }
    phase("ANativeActivity_onCreate");
    P(out, "\n=== ANativeActivity_onCreate (the VR front door) ===\n");
    if (kl_slink_vr_create(out) != 0) return fail(kl_slink_error());
    phase("activity lifecycle");
    P(out, "\n=== the activity lifecycle ===\n");
    kl_slink_vr_start(out);
    phase("looper pump");
    return 0;
}

static int begin_ue4(FILE *out) {
    phase("ANativeActivity_onCreate");
    P(out, "\n=== ANativeActivity_onCreate ===\n");
    if (g_alarm) alarm(g_alarm);
    if (kl_ue4_create(out) != 0) { if (g_alarm) alarm(0); return fail(kl_ue4_error()); }
    kl_ue4_start(out);
    if (g_alarm) alarm(0);
    phase("looper pump");
    return 0;
}

static int begin_jkxr(FILE *out) {
    phase("GLES3JNILib.onCreate");
    P(out, "\n=== GLES3JNILib.onCreate ===\n");
    if (g_alarm) alarm(g_alarm);
    if (kl_jkxr_create(out) != 0) { if (g_alarm) alarm(0); return fail(kl_jkxr_error()); }
    // Where the engine will look for its data, which nothing else can say. id
    // Tech 3 takes fs_basepath from the cwd the guest chdir'd to inside onCreate
    // and fs_homepath from $HOME; a chdir that failed leaves the process where it
    // started and every pk3 is then looked for in `<cwd>/base`. This port drops
    // its own console output, so the engine's report of that reaches the log as
    // an exit and nothing else.
    char cwd[1200];
    P(out, "  [jkxr] cwd after onCreate (the engine's fs_basepath): %s\n",
      getcwd(cwd, sizeof cwd) ? cwd : strerror(errno));
    P(out, "  [jkxr] HOME (the engine's fs_homepath root): %s\n",
      getenv("HOME") ? getenv("HOME") : "(unset)");
    // onStart / onResume / surfaceCreated / surfaceChanged. The surface is where
    // the engine stops waiting — its render thread blocks until one arrives — so
    // a begin that stopped at onCreate would look like a hang.
    kl_jkxr_start(out);
    if (g_alarm) alarm(0);
    phase("looper pump");
    return 0;
}

int kl_driver_lifecycle_begin(FILE *out) {
    if (!g_target) return fail("kl_driver_init was not called");
    switch (kl_driver_kind()) {
    case KL_GUEST_STEAMLINK: return begin_steamlink(out);
    case KL_GUEST_UE4:       return begin_ue4(out);
    case KL_GUEST_JKXR:      return begin_jkxr(out);
    default:                 return begin_unity(out);
    }
}

int kl_driver_frame(void) {
    if (kl_driver_owns_frame_loop()) return -1;
    if (!g_render || !g_thiz) return -1;
    if (g_alarm) alarm(g_alarm);
    // Pin this frame's poses before anything in the frame can ask, so every
    // ovrp_GetNodePoseState inside it answers the same thing and the pose
    // recorded for timewarp is the one the picture was drawn from. A guest frame
    // is longer than a display frame whenever performance is short, and without
    // this the head moves INSIDE the frame.
    kl_ovrp_frame_latch();
    // No Choreographer tick here: doFrame comes from a free-running host thread
    // started at the guest's first postFrameCallback (kl_jni_looper.c), because
    // the engine waits inside nativeRender on a refresh counter each doFrame
    // advances. A tick from this loop is a second source handing the engine two
    // doFrames per rendered frame, which is its frame delta halved.
    kl_jni_local_frame_push();
    int r = ((int8_t (*)(void *, void *))g_render)(kl_jni_env(), g_thiz);
    kl_jni_local_frame_pop();
    kl_jni_drain_ui_tasks();
    // Android's low-memory notification: one task_info call, and the guest drops
    // its caches when the footprint crosses the reported budget.
    kl_mem_pressure_poll();
    if (g_alarm) alarm(0);
    g_frames++;
    return r;
}

double kl_driver_pump(double seconds, const volatile int *quit) {
    switch (kl_driver_kind()) {
    case KL_GUEST_STEAMLINK:
        return g_door == KL_SLINK_VR ? kl_slink_vr_pump(seconds, quit)
                                     : kl_slink_sdl_pump(seconds, quit);
    case KL_GUEST_UE4:  return kl_ue4_pump(seconds, quit);
    case KL_GUEST_JKXR: return kl_jkxr_pump(seconds, quit);
    default:            return 0.0;
    }
}

double kl_driver_pump_default(void) {
    switch (kl_driver_kind()) {
    // 10 s measures how far the guest gets by itself; the pairing loop wants
    // longer and says so with the knob.
    case KL_GUEST_STEAMLINK: return (double)kl_env_uint("KL_SLINK_WAIT", 10);
    // RE4 needs 300: the first minute is the engine's one-time shader
    // optimization.
    case KL_GUEST_UE4:       return kl_env_str("KL_UE4_WAIT", NULL)
                                  ? strtod(getenv("KL_UE4_WAIT"), NULL) : 5.0;
    case KL_GUEST_JKXR:      return kl_env_str("KL_JKXR_WAIT", NULL)
                                  ? strtod(getenv("KL_JKXR_WAIT"), NULL) : 5.0;
    default:                 return 0.0;
    }
}

void kl_driver_report(FILE *out) {
    if (!out) return;
    switch (kl_driver_kind()) {
    case KL_GUEST_STEAMLINK:
        fprintf(out, "\n=== the Steam Link %s run ===\n", kl_slink_door_name());
        kl_slink_report(out);
        return;
    case KL_GUEST_UE4:
        fprintf(out, "\n=== the Unreal run ===\n");
        kl_ue4_report(out);
        fflush(out);
        return;
    case KL_GUEST_JKXR:
        fprintf(out, "\n=== the OpenJK run ===\n");
        kl_jkxr_report(out);
        fflush(out);
        return;
    default: break;
    }
    fprintf(out, "  pumped %u frames\n", g_frames);
    kl_jni_report(out);
    kl_egl_report(out);
    kl_opensl_report(out);
    // A Unity guest can reach the video path too — Open Brush seeds a video into
    // its own media library at startup.
    kl_mediandk_report(out);
    kl_ovrp_report(out);
    kl_ovrplat_report(out);
    kl_openxr_report(out);
    kl_madv_report();
    kl_mem_report();
    fflush(out);
}
