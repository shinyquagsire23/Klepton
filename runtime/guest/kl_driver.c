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
#include "kl_obbmap.h"
#include "kl_egl.h"
#include "kl_env.h"
#include "kl_guestpoke.h"
#include "kl_jkxr.h"
#include "kl_native.h"
#include "kl_sdl2.h"
#include "kl_gles3jni.h"
#include "kl_jni.h"
#include "kl_mediandk.h"
#include "kl_opensl.h"
#include "kl_openxr.h"
#include "kl_ovrp.h"
#include "kl_ovrplat.h"
#include "kl_mprobe.h"
#include "kl_sample.h"
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

// Per-target env defaults: bake in a title's validated flag set so it need not be
// typed on every launch. setenv with overwrite=0 means an explicit KL_* from the
// launch environment (run.sh forwards every KL_* to the device) still wins, so the
// documented per-flag A/B overrides (e.g. KL_HL2_TWOPASS_OPAQUE=0) keep working.
// Applied here, at the very top of driver init, because it must precede every reader:
// the hl2 cfg synth (kl_libc.c), ANGLE's own getenv("KL_ANGLE_MV_RELAX"), and the
// cached-static flag defaults in kl_glfb.c. The THREADED_MIRROR / CINEMA_CTX fixes
// already default-on for hl2 via their own target check, so they are not repeated.
static void kl_driver_env_defaults(const kl_target *t) {
    if (!t || !t->name) return;
    if (!strcmp(t->name, "hl2")) {
        // Validated Half-Life 2 VR configuration (2026-08-31): direct-array multiview
        // + forced eye-RT allocation, single-threaded material queue (threaded wedges
        // the guest's GL context), ANGLE multiview relaxation, two-pass brush+opaque
        // (recovers the geometry the emulated single-pass multiview silently drops),
        // force-clear, visfix, no-cull, and sv_cheats (unblocks the skybox fog cvars).
        // See the hl2-* memories for why each is needed.
        setenv("KL_HL2_MULTIVIEW",     "1", 0);
        setenv("KL_HL2_FORCERT",       "1", 0);
        setenv("KL_HL2_MAT_QUEUE",     "0", 0);
        setenv("KL_ANGLE_MV_RELAX",    "1", 0);
        setenv("KL_HL2_FORCE_CLEAR",   "1", 0);
        setenv("KL_HL2_VISFIX",        "1", 0);
        setenv("KL_HL2_TWOPASS_BRUSH", "1", 0);
        setenv("KL_HL2_TWOPASS_OPAQUE","1", 0);
        setenv("KL_HL2_NO_CULL",       "1", 0);
        setenv("KL_HL2_SV_CHEATS",     "1", 0);
        // LOWMEM confirmed on device (2026-08-31): long in-game sessions were
        // signal-9 jettisoned by the OS; mat_picmip 2 + mat_reducefillrate 1 cut
        // the footprint and the session ran meaningfully longer. KL_HL2_LOWMEM=0
        // restores full-res textures.
        setenv("KL_HL2_LOWMEM",        "1", 0);
    }
    else if (!strcmp(t->name, "hl1")) {
        // Half-Life on Xash3D / Lambda1VR. Written into userconfig.cfg on device by
        // kl_xash_write_userconfig (kl_native.c), which execs AFTER config.cfg so
        // these win over the saved/archived values:
        //   vr_smoothturn/vr_turn_angle — force smooth turning (the in-menu toggle's
        //     hitbox is offset/unclickable); vr_turn_angle < 10 selects smooth mode
        //     and is also its speed (0 would be smooth-but-motionless).
        //   vr_height_adjust — the guest reads the head in LOCAL space, which Klepton
        //     places ~1.675 m below the floor (STAGE bounds query fails, so the game
        //     never gets a floor-grounded space), so the head reads ~1.68 m too high
        //     and clips low ceilings (the intro tram roof). -1.5 m cancels most of it.
        //     The real fix is the LOCAL/STAGE reference-space math in kl_openxr.c.
        // (con_logfile was tried here to capture the engine console, but this build
        // of Xash writes no logfile on this platform — verified inert — so it's
        // dropped. The engine console is only reachable on-screen.)
        setenv("KL_XASH_CVARS",
               "vr_smoothturn 1; vr_turn_angle 5; vr_height_adjust -1.5", 0);
    }
    else if (!strcmp(t->name, "wrath2")) {
        // wrath2's world never appears because the GAME thread grinds through FMOD
        // Studio bank loading (UEngine::LoadMap -> LoadBanksForMap -> loadBankFile),
        // which polls with short usleeps. At DEFAULT QoS on visionOS those sleeps
        // are coalesced to 30 Hz (33 ms), so a ~few-second bank load stretches to
        // minutes and the level never finishes loading in a session. Cap the poll
        // sleep and lift the guest threads off default QoS so their timer wakeups
        // are not coalesced — same fix class as olar's audio-thread QoS starvation.
        setenv("KL_USLEEP_CAP", "250", 0);   // microseconds; the FMOD poll slept 5-33 ms/call
        setenv("KL_GUEST_QOS",  "1",   0);   // USER_INITIATED instead of default
    }
}

void kl_driver_init(const kl_target *t, const char *libdir, kl_slink_door door) {
    g_target = t;
    g_door   = door;
    g_error[0] = 0;
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir ? libdir : "");
    kl_driver_env_defaults(t);
}

kl_guest_kind kl_driver_kind(void) {
    return g_target ? g_target->kind : KL_GUEST_UNITY;
}

const char      *kl_driver_error(void)  { return g_error; }
const char      *kl_driver_target_name(void) { return g_target ? g_target->name : NULL; }
int              kl_driver_gap_only(void) { return g_gap_only; }
unsigned         kl_driver_frames(void) { return g_frames; }
void             kl_driver_note_frame(void) { g_frames++; }
void             kl_driver_set_phase_hook(void (*fn)(const char *)) { g_phase_hook = fn; }
void             kl_driver_set_alarm(unsigned s) { g_alarm = s; }

int kl_driver_owns_frame_loop(void) {
    switch (kl_driver_kind()) {
    case KL_GUEST_STEAMLINK:
    case KL_GUEST_UE4:
    case KL_GUEST_JKXR:
    case KL_GUEST_NATIVE:
    case KL_GUEST_SDL2:
    case KL_GUEST_GLES3JNI: return 1;
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
    // Before load, not before initJni: newer libunity asks ALooper_forThread()
    // for the UI thread inside its own JNI_OnLoad, which runs during this call.
    // Marking afterwards left "Couldn't retrieve native ALooper for UI thread."
    // in every affected Unity boot.
    kl_jni_mark_ui_thread();
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
    // Two initJni generations. Unity 2021's is (Context) alone; the 2022+/Meta
    // template (newer Meta Unity guests) registers
    // (Landroid/content/Context;ILjava/lang/String;)V - context, CONTEXT TYPE,
    // command line. Calling the new one with the old arity left the int and
    // String registers holding garbage: libunity then logged the junk forever
    // ("Unknown context type: 31 / <stale pointer>", 4069 times) and trapped
    // with SIGILL before the first frame. The int is com.unity3d.player.a.o's
    // enum value - ActivityOrService = 0, which UnityPlayerActivity uses - and
    // the string is the "unity" intent extra, empty on a plain launch.
    // Arity is decided from the signature RegisterNatives actually stored, not
    // from an exact-string lookup: the exact match came back NULL on a run whose
    // log showed the 3-arg registration, and printing the stored bytes verbatim
    // is the only way the next log can say why. strstr on the context-type int
    // plus the command-line String is the discriminator between generations.
    const char *ijsig = kl_jni_native_sig("com/unity3d/player/UnityPlayer", "initJni");
    void *initJni = kl_jni_native("com/unity3d/player/UnityPlayer", "initJni", NULL);
    if (!initJni) return fail("UnityPlayer.initJni was never registered");
    // THREE initJni generations, decided from the signature RegisterNatives
    // actually stored:
    //   1-arg  (Landroid/content/Context;)V                         Unity 2021
    //   2-arg  (Landroid/content/Context;I)V                        context + type
    //   3-arg  (Landroid/content/Context;ILjava/lang/String;)V      + command line
    // The int is the context-type enum (0 = ActivityOrService, what
    // UnityPlayerActivity uses); the String is the "unity" intent extra, empty
    // on a plain launch. Calling a newer arity with an older one leaves the int
    // (and String) registers holding garbage, and libunity logs "Unknown context
    // type: <junk>" forever and SIGILLs before the first frame — which is exactly
    // what Mission ISS's (Context;I) form did when it fell to the 1-arg branch.
    int initJni3 = ijsig && strstr(ijsig, "ILjava/lang/String;") != NULL;
    int initJni2 = !initJni3 && ijsig && strstr(ijsig, ";I)") != NULL;
    phase("initJni");
    P(out, "\n=== UnityPlayer.initJni%s -> %s-arg call ===\n",
      ijsig ? ijsig : "<sig unrecorded>", initJni3 ? "3" : initJni2 ? "2" : "1");
    // The shared singleton for the same reason the Context is one: this is the
    // object UnityPlayerActivity keeps in mUnityPlayer, and the guest reads it
    // back through that field. See kl_jni.h.
    void *thiz = kl_jni_unity_player();
    kl_jni_local_frame_push();
    if (initJni3)
        ((void (*)(void *, void *, void *, int, void *))initJni)(
            kl_jni_env(), thiz, kl_jni_activity(), 0, kl_jni_new_string(""));
    else if (initJni2)
        ((void (*)(void *, void *, void *, int))initJni)(
            kl_jni_env(), thiz, kl_jni_activity(), 0);
    else
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
    if (kl_ue4_configure(g_libdir, g_target->entry_lib, out) != 0) return fail(kl_ue4_error());
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

static int boot_native(FILE *out) {
    phase("native configure");
    if (kl_native_configure(g_libdir, g_target->entry_lib, out) != 0)
        return fail(kl_native_error());
    phase("native load");
    if (kl_native_load(out) != 0) return fail(kl_native_error());
    P(out, "\n=== the shim gap ===\n");
    kl_native_gap(out);
    if (g_gap_only) return 0;
    P(out, "\n=== EXIT CRITERION MET: the NativeActivity library is mapped and "
           "initialised ===\n");
    return 0;
}

static int boot_sdl2(FILE *out) {
    phase("sdl2 configure");
    if (kl_sdl2_configure(g_libdir, g_target->entry_lib, out) != 0) return fail(kl_sdl2_error());
    phase("sdl2 load");
    if (kl_sdl2_load(out) != 0) return fail(kl_sdl2_error());
    P(out, "\n=== the shim gap ===\n");
    kl_sdl2_gap(out);
    if (g_gap_only) return 0;
    P(out, "\n=== EXIT CRITERION MET: the SDL2 graph is mapped and JNI_OnLoad ran ===\n");
    return 0;
}

static int boot_gles3jni(FILE *out) {
    phase("gles3jni configure");
    if (kl_gles3jni_configure(g_libdir, g_target->entry_lib, out) != 0) return fail(kl_gles3jni_error());
    phase("gles3jni load");
    if (kl_gles3jni_load(out) != 0) return fail(kl_gles3jni_error());
    P(out, "\n=== the shim gap ===\n");
    kl_gles3jni_gap(out);
    if (g_gap_only) return 0;
    P(out, "\n=== EXIT CRITERION MET: the GLES3JNILib engine is mapped and JNI_OnLoad ran ===\n");
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
    case KL_GUEST_NATIVE:    rc = boot_native(out);    break;
    case KL_GUEST_SDL2:      rc = boot_sdl2(out);      break;
    case KL_GUEST_GLES3JNI:  rc = boot_gles3jni(out);  break;
    default:                 rc = boot_unity(out);     break;
    }
    if (rc == 0 && g_gap_only)
        P(out, "\n(KL_GAP_ONLY: stopping at the shim gap — nothing was run)\n");
    return rc;
}

// ---- the lifecycle ---------------------------------------------------------

// Unity 2022+/Meta template split the player: initJni stays on UnityPlayer, but
// the lifecycle natives (nativeRecreateGfxState/Resume/Render) moved onto the
// UnityPlayerForActivityOrService subclass. Some guests registered them only
// there, so a UnityPlayer-only lookup came back "not registered" and the frame
// loop ended before the first frame. Prefer the subclass, fall back to the base
// so 2019/2021 titles (which register on UnityPlayer) are unchanged. Identity of
// thiz does not matter here: initJni and these calls already use different
// synthetic objects, so libunity keys its state off native globals, not thiz.
static void *unity_lifecycle_native(const char *name) {
    void *fn = kl_jni_native("com/unity3d/player/UnityPlayerForActivityOrService",
                             name, NULL);
    return fn ? fn : kl_jni_native("com/unity3d/player/UnityPlayer", name, NULL);
}

// KL_SAMPLE_MS: the guest-thread sampler, wired here so it runs on DEVICE
// too - m_boot starts it around its own pump, but the visionOS app pumps
// through this driver and never saw the knob. Metadata for managed-name
// resolution comes from KL_IL2CPP_METADATA or the libdir-derived path; on
// device the tree usually is not beside the libs, and the sampler then
// reports module+offset frames, which is the useful part on v29 metadata.
//
// A helper because every DOOR needs it, not just Unity's: the first wrath2
// freeze-hunt ran with KL_SAMPLE_MS set and got nothing back — the knob was
// honoured only in begin_unity, and a UE4 guest booted through begin_ue4,
// which never looked. (A UE4 guest has no il2cpp metadata; the sampler then
// reports module+offset frames, which is the half that matters there anyway.)
static void sample_start_if_asked(void) {
    const char *senv = getenv("KL_SAMPLE_MS");
    if (!senv || !*senv) return;
    char meta[1024];
    const char *mp = getenv("KL_IL2CPP_METADATA");
    if (!mp) {
        snprintf(meta, sizeof meta, "%s", g_libdir);
        char *tail = strstr(meta, "/lib/");
        if (tail) *tail = 0;
        snprintf(meta + strlen(meta), sizeof meta - strlen(meta),
                 "/assets/bin/Data/Managed/Metadata/global-metadata.dat");
        mp = meta;
    }
    kl_sample_start((unsigned)strtoul(senv, NULL, 10), mp);
}

static int begin_unity(FILE *out) {
    // The SAME player initJni ran on, not a second instance — see kl_jni.h.
    g_thiz = kl_jni_unity_player();
    if (!g_thiz) return fail("kl_driver_boot must run first");

    // Index the OBB's audio banks by basename (NULL base = no UE4 loose-path
    // serving, banks only). A Unity sound engine — Wwise (ZIX) or FMOD — reads
    // its .bnk/.wem/.bank by a native path unrelated to the OBB layout, and the
    // banks live only inside the OBB; without this the engine gets AK_FileNotFound
    // and renders silence.
    kl_obbmap_init(kl_jni_obb_dir(), NULL);

    // The order UnityPlayerActivity drives: attach a surface, resume, then one
    // frame. nativeRecreateGfxState is what reaches for EGL, and it is also what
    // pulls in libil2cpp — the image the boot gate never loads.
    void *surface = kl_jni_new_object("android/view/Surface");
    struct { const char *name; int kind; } seq[] = {
        { "nativeRecreateGfxState", 2 }, { "nativeResume", 0 }, { "nativeRender", 1 },
    };
    for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        void *fn = unity_lifecycle_native(seq[i].name);
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

    g_render = unity_lifecycle_native("nativeRender");
    // The graphics device exists by now and no frame has been drawn yet. The
    // raise lets libunity bind past its un-queried cap of 32 texture units
    // instead of refusing every bind and reading stale unit-0 textures; it is
    // declined per Unity build, and names the build when it declines.
    kl_guest_poke_texture_unit_cap();
    sample_start_if_asked();
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
    sample_start_if_asked();
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
    sample_start_if_asked();
    phase("looper pump");
    return 0;
}

static int begin_sdl2(FILE *out) {
    phase("SDLActivity begin");
    P(out, "\n=== SDLActivity: setupJNI + nativeRunMain ===\n");
    if (g_alarm) alarm(g_alarm);
    int rc = kl_sdl2_begin(out);
    if (g_alarm) alarm(0);
    if (rc != 0) return fail(kl_sdl2_error());
    sample_start_if_asked();
    phase("looper pump");
    return 0;
}

static int begin_gles3jni(FILE *out) {
    phase("GLES3JNILib onCreate");
    P(out, "\n=== GLES3JNILib: onCreate + lifecycle ===\n");
    if (g_alarm) alarm(g_alarm);
    if (kl_gles3jni_create(out) != 0) { if (g_alarm) alarm(0); return fail(kl_gles3jni_error()); }
    if (g_alarm) alarm(0);
    // onStart/onResume/onSurfaceCreated/Changed — the surface is where the engine
    // stops waiting, so a begin that stopped at onCreate would look like a hang.
    kl_gles3jni_start(out);
    sample_start_if_asked();
    phase("looper pump");
    return 0;
}

static int begin_native(FILE *out) {
    phase("ANativeActivity_onCreate");
    P(out, "\n=== ANativeActivity_onCreate ===\n");
    if (g_alarm) alarm(g_alarm);
    if (kl_native_create(out) != 0) { if (g_alarm) alarm(0); return fail(kl_native_error()); }
    if (g_alarm) alarm(0);
    // onStart / onResume / onNativeWindowCreated: the game's render thread has
    // been spinning since onCreate and blocks until the window arrives, so a begin
    // that stopped at onCreate would look like a hang.
    kl_native_start(out);
    sample_start_if_asked();
    phase("looper pump");
    return 0;
}

int kl_driver_lifecycle_begin(FILE *out) {
    if (!g_target) return fail("kl_driver_init was not called");
    switch (kl_driver_kind()) {
    case KL_GUEST_STEAMLINK: return begin_steamlink(out);
    case KL_GUEST_UE4:       return begin_ue4(out);
    case KL_GUEST_JKXR:      return begin_jkxr(out);
    case KL_GUEST_NATIVE:    return begin_native(out);
    case KL_GUEST_SDL2:      return begin_sdl2(out);
    case KL_GUEST_GLES3JNI:  return begin_gles3jni(out);
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
    // KL_PROBE_INPUT: the managed-side probe, self-gated on its env knob and
    // wired here so it runs on DEVICE too (m_boot ticks it around its own
    // pump). Between frames on the thread that just ran one, which is where
    // managed calls are safe.
    kl_mprobe_tick(g_frames);
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
    case KL_GUEST_NATIVE: return kl_native_pump(seconds, quit);
    case KL_GUEST_SDL2: return kl_sdl2_pump(seconds, quit);
    case KL_GUEST_GLES3JNI: return kl_gles3jni_pump(seconds, quit);
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
    case KL_GUEST_NATIVE:
    case KL_GUEST_SDL2:
    case KL_GUEST_GLES3JNI:  return kl_env_str("KL_NATIVE_WAIT", NULL)
                                  ? strtod(getenv("KL_NATIVE_WAIT"), NULL) : 5.0;
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
    case KL_GUEST_NATIVE:
        fprintf(out, "\n=== the NativeActivity run ===\n");
        kl_native_report(out);
        fflush(out);
        return;
    case KL_GUEST_SDL2:
        fprintf(out, "\n=== the SDL2 run ===\n");
        kl_sdl2_report(out);
        return;
    case KL_GUEST_GLES3JNI:
        fprintf(out, "\n=== the GLES3JNILib run ===\n");
        kl_gles3jni_report(out);
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
