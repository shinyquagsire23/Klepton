// SDLActivity, and Steam Link's shell overrides on it
//
// One family of the synthetic JNIEnv's Java classes. The mechanism (registries,
// dispatch, id interning) is kl_jni.c; this file owns implementations and the
// binding table that names them. See runtime/jni/kl_jni_int.h for the seam.
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "klepton.h"
#include "kl_jni.h"
#include "kl_fault.h"
#include "kl_target.h"
#include "kl_env.h"
#include "kl_ovrp.h"
#include "kl_avdec.h"
#include "kl_egl.h"
#include "kl_ndk.h"
#include "kl_va.h"
#include "kl_jni_int.h"

// ------------------------------------------------------------- SDLActivity --
// Steam Link's half of the M4 surface (PLANNING §11). Every one of these is
// called by SDL3 itself during SDL_main's startup, not by the app.

#define KLJ_SDLA "org/libsdl/app/SDLActivity"

static struct { char *k, *v; } g_menv[32];
static unsigned g_menv_n;

void kl_jni_add_manifest_env(const char *key, const char *value) {
    if (!key || !value || g_menv_n >= sizeof g_menv / sizeof *g_menv) return;
    g_menv[g_menv_n].k = strdup(key);
    g_menv[g_menv_n].v = strdup(value);
    if (g_menv[g_menv_n].k && g_menv[g_menv_n].v) g_menv_n++;
}

static klj_val klj_SDLA_getContext(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_activity()};
}

// Android walks the ApplicationInfo metaData Bundle and calls nativeSetenv per
// SDL_ENV.* key. We call the same native with the same pairs, so the guest ends
// up in the same state by the same route rather than by us reaching around it.
// The boolean answers "was there a metaData bundle at all", which is what the
// real implementation returns.
static klj_val klj_SDLA_getManifestEnv(void *env, void *self, const klj_val *a, int n) {
    (void)self; (void)a; (void)n;
    if (!g_menv_n) return (klj_val){.j = 0};
    void *fn = kl_jni_native(KLJ_SDLA, "nativeSetenv", NULL);
    if (!fn) return (klj_val){.j = 0};
    void *cls = kl_jni_class(KLJ_SDLA);
    kl_jni_local_frame_push();
    for (unsigned i = 0; i < g_menv_n; i++)
        ((void (*)(void *, void *, void *, void *))fn)(
            env, cls, kl_jni_new_string(g_menv[i].k), kl_jni_new_string(g_menv[i].v));
    kl_jni_local_frame_pop();
    return (klj_val){.j = 1};
}

// A comma-separated list in SDL's own formatLocale() shape: language, then
// "_COUNTRY" when the locale has one.
static klj_val klj_SDLA_getPreferredLocales(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("en_US")};
}

// These two are part of the display panel's GROUP answer, not independent facts:
// they have to agree with the resolution and density handed to
// nativeSetScreenResolution, because SDL derives isTablet from the diagonal
// those imply. A large flat panel is a tablet and is not a television.
static klj_val klj_SDLA_isTablet(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 1};
}
static klj_val klj_SDLA_isAndroidTV(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// Android's version walks InputDevice.getDeviceIds() and calls nativeAddTouch
// for every non-virtual device with SOURCE_TOUCHSCREEN. We present a flat window
// driven by a mouse, so the honest answer is that there are no touchscreens —
// and SDL's mouse path (onNativeMouse) is a different surface that does not go
// through here. Registering a fictitious touch device would give SDL a second,
// contradictory pointer.
static klj_val klj_SDLA_initTouch(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("initTouch: no touchscreen devices presented (mouse is the pointer)");
    return (klj_val){.j = 0};
}

// sendCommand posts a Message to the UI thread's SDLCommandHandler and reports
// only whether it was QUEUED. So `true` is the accurate answer here: we did
// accept it. What the handler would have done — window title, window flags,
// keep-screen-on, hide the text edit — is host-window policy that has no guest
// state attached, so recording the command is the whole of it. Naming them makes
// the trace readable rather than a column of integers.
static klj_val klj_SDLA_sendMessage(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int cmd = n > 0 ? (int)(int64_t)a[0].j : 0;
    int arg = n > 1 ? (int)(int64_t)a[1].j : 0;
    const char *name = cmd == 1 ? "CHANGE_TITLE"
                     : cmd == 2 ? "CHANGE_WINDOW_STYLE"
                     : cmd == 3 ? "TEXTEDIT_HIDE"
                     : cmd == 5 ? "SET_KEEP_SCREEN_ON"
                     : cmd >= 0x8000 ? "USER" : "?";
    KLJ_LOG("sendMessage(%d=%s, %d) accepted", cmd, name, arg);
    return (klj_val){.j = 1};
}

// The real one maps an SDL cursor id onto an Android PointerIcon and returns
// false for ids it has no icon for. We accept every id: the host window owns its
// own cursor, and a false here makes SDL believe the cursor is unsupported and
// stop asking.
static klj_val klj_SDLA_setSystemCursor(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 1};
}

// The one that decides whether there is a window at all. SDL3 calls this, then
// ANativeWindow_fromSurface() on the result — and kl_ndk.c's synthetic window
// ignores the Surface entirely, so all that is required here is a non-NULL
// object of the right class. Returning NULL is what produced Steam Link's
// "Couldn't create window: Could not fetch native window".
//
// The window's GEOMETRY is not set here: it belongs with the resolution handed
// to nativeSetScreenResolution (kl_ndk_set_window), because SDL cross-checks the
// two and a disagreement is the display-panel group-answer hazard again.
static klj_val klj_SDLA_getNativeSurface(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_object("android/view/Surface")};
}

// Recorded, not applied: there is no window manager here to rotate, and the
// guest's own idea of orientation follows from the resolution it was given.
static klj_val klj_SDLA_setOrientation(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("setOrientation(w=%d h=%d resizable=%d hint=%s) recorded, not applied",
            n > 0 ? (int)(int64_t)a[0].j : 0, n > 1 ? (int)(int64_t)a[1].j : 0,
            n > 2 ? (int)(int64_t)a[2].j : 0,
            (n > 3 ? klj_str(a[3].l) : NULL) ? klj_str(a[3].l) : "");
    return (klj_val){.j = 0};
}

// Android polls the InputDevice list here and calls nativeAddJoystick /
// nativeRemoveJoystick for the deltas. We present no joysticks, so the honest
// poll finds no changes and reports none — the same answer the real one gives on
// a device with nothing plugged in.
static klj_val klj_SDLCM_pollInputDevices(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// SDL derives this from isAndroidTV / isVRHeadset / isTablet, in that order, so
// it is not an independent fact — it is a RESTATEMENT of the display group
// answer, and it has to be derived the same way or the two disagree. (Only the
// older SDL3 in steamlink-android.apk calls it; the VR build's does not.)
static klj_val klj_SDLA_formFactor(void *env, void *self, const klj_val *a, int n) {
    const char *ff = klj_SDLA_isAndroidTV(env, self, a, n).j ? "tv"
                   : klj_SDLA_isTablet(env, self, a, n).j    ? "tablet"
                                                            : "phone";
    return (klj_val){.l = kl_jni_new_string(ff)};
}

static klj_val klj_SL_canDisplay4K(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// "This facility is not available." Distinct from klj_SDLA_false only in that
// the caller wants an object back, not a boolean.
static klj_val klj_SDLA_null(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};
}

// Three "what kind of Android is this" probes. All false: we present an ordinary
// handheld-class Android, and each of these selects a different windowing
// behaviour we do not implement. Same group as isTablet/isAndroidTV.
static klj_val klj_SDLA_false(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

static klj_val klj_SDLA_setActivityTitle(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *t = n > 0 ? klj_str(a[0].l) : NULL;
    KLJ_LOG("setActivityTitle(\"%s\")", t ? t : "");
    return (klj_val){.j = 1};
}

static klj_val klj_SDLA_setWindowStyle(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("setWindowStyle(fullscreen=%d) recorded, not applied",
            n > 0 ? (int)(int64_t)a[0].j : 0);
    return (klj_val){.j = 0};
}

// No haptic devices are presented, so a poll finds no changes — the same answer
// the real one gives with nothing connected. (The guest already logged "no
// rumble capable system haptic device found" off the back of this.)
static klj_val klj_SDLCM_pollHapticDevices(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// There is no user here to press a button, and that is the honest answer: -1 is
// SDL's "dismissed without a selection". What matters is that the TEXT reaches
// the log — a message box is the guest explaining itself, and on this target it
// is usually the streaming client reporting why it could not connect. Losing
// that to a silent default would throw away the best diagnostic in the run.
static klj_val klj_SDLA_messageBox(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *title = n > 1 ? klj_str(a[1].l) : NULL;
    const char *msg   = n > 2 ? klj_str(a[2].l) : NULL;
    KLJ_LOG("MESSAGEBOX [%s] %s", title ? title : "", msg ? msg : "");
    return (klj_val){.j = (uint64_t)(int64_t)-1};
}

static klj_val klj_SL_streamingComplete(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("streamingComplete(%d)", n > 0 ? (int)(int64_t)a[0].j : 0);
    return (klj_val){.j = 0};
}

// The activity's single ShellWifiInfo (`getWifiInfo()` is a plain field read of
// `mWifiInfo`, so the object identity is stable and observable). Its four
// public fields are read straight off the object by libshell — they are in
// g_fields, where the values and the reasoning live.
#define KLJ_WIFIINFO "com/valvesoftware/steamlink/SteamLink$ShellWifiInfo"
static klj_val klj_SL_getWifiInfo(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *obj;
    if (!obj) {
        obj = klj_new_object_data(KLJ_WIFIINFO, NULL);
        klj_as_object(obj)->pinned = 1;   // the activity holds it for its life
    }
    return (klj_val){.l = obj};
}

const klj_binding klj_bind_sdl[] = {
    {KLJ_SDLA, "getContext", "()Landroid/app/Activity;", klj_SDLA_getContext},
    {KLJ_SDLA, "getManifestEnvironmentVariables", "()Z", klj_SDLA_getManifestEnv},
    {KLJ_SDLA, "getPreferredLocales", "()Ljava/lang/String;", klj_SDLA_getPreferredLocales},
    {KLJ_SDLA, "isTablet", "()Z", klj_SDLA_isTablet},
    {KLJ_SDLA, "isAndroidTV", "()Z", klj_SDLA_isAndroidTV},
    {KLJ_SDLA, "initTouch", "()V", klj_SDLA_initTouch},
    {KLJ_SDLA, "sendMessage", "(II)Z", klj_SDLA_sendMessage},
    {KLJ_SDLA, "setSystemCursor", "(I)Z", klj_SDLA_setSystemCursor},
    {KLJ_SDLA, "getNativeSurface", "()Landroid/view/Surface;", klj_SDLA_getNativeSurface},
    {KLJ_SDLA, "setOrientation", "(IIZLjava/lang/String;)V", klj_SDLA_setOrientation},
    {"org/libsdl/app/SDL", "getContext", "()Landroid/app/Activity;", klj_SDLA_getContext},
    {KLJ_SDLA, "setActivityTitle", "(Ljava/lang/String;)Z", klj_SDLA_setActivityTitle},
    {KLJ_SDLA, "setWindowStyle", "(Z)V", klj_SDLA_setWindowStyle},
    {KLJ_SDLA, "getDeviceFormFactor", "()Ljava/lang/String;", klj_SDLA_formFactor},
    {KLJ_SDLA, "isChromebook", "()Z", klj_SDLA_false},
    {KLJ_SDLA, "isDeXMode", "()Z", klj_SDLA_false},
    {KLJ_SDLA, "shouldMinimizeOnFocusLoss", "()Z", klj_SDLA_false},
    // Declared on SDLActivity, so Steam Link's override resolves here through
    // the superclass walk rather than needing its own entry.
    {KLJ_SDLA, "messageboxShowMessageBox",
     "(ILjava/lang/String;Ljava/lang/String;[I[I[Ljava/lang/String;[I)I", klj_SDLA_messageBox},
    {"org/libsdl/app/SDLControllerManager", "pollInputDevices", "()V", klj_SDLCM_pollInputDevices},
    {"org/libsdl/app/SDLControllerManager", "pollHapticDevices", "()V", klj_SDLCM_pollHapticDevices},
    // The older SDL3 in steamlink-android.apk names the same two operations
    // detect*; the VR build's names them poll*. Same answer — we present no
    // joysticks and no haptic devices, so a scan finds nothing to add.
    {"org/libsdl/app/SDLControllerManager", "detectDevices", "()V", klj_SDLCM_pollInputDevices},
    {"org/libsdl/app/SDLControllerManager", "detectHapticDevices", "()V", klj_SDLCM_pollHapticDevices},
    // Steam Link's USB-over-network helper. Acquiring it would claim we can
    // forward USB devices, which we cannot; NULL is the honest "not available"
    // and the app has a path for it.
    {"com/valvesoftware/steamlink/VirtualHere", "acquire",
     "(Landroid/content/Context;)Lcom/valvesoftware/steamlink/VirtualHere;", klj_SDLA_null},
    {"com/valvesoftware/steamlink/SteamLink", "streamingComplete", "(I)V", klj_SL_streamingComplete},
    // The first thing libshell's main() asks, and it is answered TRUTHFULLY
    // rather than conveniently. The method is not a capability probe: its body
    // is `getIntent().getStringExtra("returnFrom").equals("vrlink")`, i.e. "did
    // the VR activity hand control back to me". We launch this activity
    // directly, from nothing, with no extras — so false is what the real
    // Android would compute, not a stub's shrug.
    //
    // It is also the fork that decides what this run can show. True sends the
    // shell down the returning-from-VR path (§11.9's handoff, which we have not
    // synthesized); false is the plain 2D configuration frontend, which is the
    // whole reason for opening this front door.
    {"com/valvesoftware/steamlink/SteamLink", "wasLaunchedFromVRLink", "()Z", klj_SDLA_false},
    {"com/valvesoftware/steamlink/SteamLink", "getWifiInfo",
     "()L" KLJ_WIFIINFO ";", klj_SL_getWifiInfo},
    // Both are one-line reads of an intent extra — getStringExtra("displayTitle")
    // and ("displayMessage") — that another activity sets when it hands this one
    // a message to show on the way in. We launch it directly with no extras, so
    // null is what Android would compute, and getStringExtra returns null for an
    // absent key rather than "". Not a stub: the same answer as the real thing.
    {"com/valvesoftware/steamlink/SteamLink", "getMessageTitle", "()Ljava/lang/String;",
     klj_SDLA_null},
    {"com/valvesoftware/steamlink/SteamLink", "getMessageText", "()Ljava/lang/String;",
     klj_SDLA_null},
    // Steam Link's own. Part of the display panel's group answer: the panel we
    // describe is 1280x800, so 4K playback is not something it can display, and
    // claiming otherwise would have the app negotiate a stream its own window
    // cannot show.
    {"com/valvesoftware/steamlink/SteamLinkUtils", "canDisplay4KVideo", "()Z", klj_SL_canDisplay4K},
    // ...and the same group's HDR half, which asks the same question of the same
    // panel. Its body walks Display.getHdrCapabilities().getSupportedHdrTypes()
    // looking for HDR10, and our synthetic display advertises no HDR types at
    // all — so false is what it would compute, not a refusal. Answering true
    // would have the client negotiate an HDR stream and then tone-map it onto
    // an SDR window, which is the 4K mistake in a different colour space.
    {"com/valvesoftware/steamlink/SteamLinkUtils", "canDisplayHDRVideo", "(ZZ)Z", klj_SDLA_false},
    // The overlay SurfaceView's visibility. On Android these post a Runnable to
    // the UI thread and BLOCK on Object.wait() until it has run — the caller's
    // contract is "the surface is now in that state", not "a request was sent".
    // We present one surface and it is the window, so both states are already
    // true and returning immediately IS that contract. What must not happen is
    // the literal translation: queueing a task and waiting would hang, because
    // the visibility this waits on is a second view we do not have.
    {"com/valvesoftware/steamlink/SteamLink", "showOverlaySurface", "()V", klj_SDLA_null},
    {"com/valvesoftware/steamlink/SteamLink", "hideOverlaySurface", "()V", klj_SDLA_null},
    {0}
};
