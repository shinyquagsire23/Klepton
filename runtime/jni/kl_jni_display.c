// the presented display: DisplayManager, Display, Mode
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

// ---- the presented display ----
// The screen itself is described up at KLJ_DISPLAY_*, next to g_fields, since
// most of it is read as DisplayMetrics fields rather than through a method.

// Display.DEFAULT_DISPLAY is 0. There is exactly one display here, so any other
// id has no Display — which is Android's own answer, not a shortcut.
static klj_val klj_DisplayManager_getDisplay(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int32_t id = n > 0 ? (int32_t)a[0].j : 0;
    if (id != 0) {
        KLJ_LOG("DisplayManager.getDisplay(%d) -> null (only display 0 exists)", id);
        return (klj_val){.l = NULL};
    }
    static void *display;
    return klj_singleton("android/view/Display", &display);
}

// registerDisplayListener is a host->guest callback, and it lands in the same
// gap runOnUiThread does: we can record the listener but nothing will ever fire
// it, because calling into the proxy needs JNIBridge.invoke. Our display never
// changes, so no callback is *owed* — but that is a property of the display we
// chose, not of the mechanism, so it is logged rather than left silent.
static klj_val klj_DisplayManager_registerDisplayListener(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("DisplayManager.registerDisplayListener(%p) — recorded; the presented "
            "display never changes, so nothing is owed a callback",
            n > 0 ? a[0].l : NULL);
    return (klj_val){0};
}
static klj_val klj_DisplayManager_unregisterDisplayListener(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){0};
}

// Everything below reads straight off KLJ_DISPLAY_*. getWidth/getHeight are the
// deprecated pair and getRealMetrics the modern one; they agree here because our
// display has no system decor to subtract, which is the whole reason the two
// exist separately on Android.
static klj_val klj_Display_getDisplayId(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}
static klj_val klj_Display_getWidth(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = KLJ_DISPLAY_W};
}
static klj_val klj_Display_getHeight(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = KLJ_DISPLAY_H};
}
static klj_val klj_Display_getRotation(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = KLJ_DISPLAY_ROTATION};
}
klj_val klj_Display_getRefreshRate(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.d = KLJ_DISPLAY_REFRESH};
}
// The DisplayMetrics the guest passes is already answered from the same
// constants by the field dispatch, so there is nothing to write into it — one
// display means every DisplayMetrics describes it. Logged once so the no-op is
// visible if the metrics ever stop matching what Unity then computes.
static klj_val klj_Display_getMetrics(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static int said;
    if (!said++) KLJ_LOG("Display.get*Metrics: %dx%d @ %ddpi, %.0f Hz (fields are constants)",
                         KLJ_DISPLAY_W, KLJ_DISPLAY_H, KLJ_DISPLAY_DPI, (double)KLJ_DISPLAY_REFRESH);
    return (klj_val){0};
}
// Frame-pacing numbers. Unity's FrameTiming asks for these to decide when to
// submit; they have to be consistent with the refresh rate we claim or it will
// pace against a period that does not exist. The vsync offset is 0 on nearly
// every real device, and the presentation deadline is surfaceflinger's default
// of one refresh period plus a millisecond.
klj_val klj_Display_getAppVsyncOffsetNanos(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}
klj_val klj_Display_getPresentationDeadlineNanos(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = (uint64_t)(1e9 / KLJ_DISPLAY_REFRESH) + 1000000};
}
// sRGB, not wide gamut — which is what the panel description above says, and
// claiming otherwise would have Unity pick a colour space it then renders into.
static klj_val klj_Display_isWideColorGamut(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// Beat Saber 1.40 asks these two; 1.28 did not. Both belong to the DISPLAY
// PANEL — the group of answers Unity cross-checks against each other — so they
// are derived from the same KLJ_DISPLAY_* constants as getWidth/getHeight/
// getRefreshRate rather than answered on their own terms.
static klj_val klj_Display_isHdr(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // False, and consistent with isWideColorGamut above. The eye textures are
    // RGBA16F, which is a precision choice inside our own pipeline and says
    // nothing about the Android display this panel describes; a Quest reports
    // false here too. Answering true asks Unity to pick an HDR swapchain
    // format against a surface that is not one.
    return (klj_val){.j = 0};
}

// getSupportedModes() -> exactly ONE mode, the one the rest of the panel
// already describes. A Quest really does report several (60/72/90/120) and it
// would be easy to list them, but every extra entry is a mode Unity may SELECT,
// and nothing downstream of here would then agree: getRefreshRate() is a
// constant, the frame clock is the Choreographer's, and on device the real
// number is pushed from the compositor's primeDisplay. One mode makes the panel
// self-consistent by construction, which is the rule that got Bloom and the
// GLES 3.2 capability set right.
static klj_val klj_Display_getSupportedModes(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *modes;
    if (!modes) {
        modes = klj_new_array('L', "android/view/Display$Mode", 1);
        void **slot = klj_arr(modes)->data;
        slot[0] = klj_new_object_data("android/view/Display$Mode", NULL);
    }
    return (klj_val){.l = modes};
}
static klj_val klj_Mode_getModeId(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 1};        // Android numbers modes from 1, not 0
}
static klj_val klj_Mode_getPhysicalWidth(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = KLJ_DISPLAY_W};
}
static klj_val klj_Mode_getPhysicalHeight(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = KLJ_DISPLAY_H};
}
static klj_val klj_Mode_getRefreshRate(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.d = KLJ_DISPLAY_REFRESH};
}

// UnityPermissions.skipPermissionsDialog() reads the manifest's
// `unityplayer.SkipPermissionsDialog` metadata off the activity, falling back
// to the application. This manifest declares it **false**, so false is the
// transcription, not a choice — and the branch it opens is already served:
// Activity.checkSelfPermission answers granted, so Unity finds nothing to ask
// for and no dialog is ever constructed.
static klj_val klj_UnityPlayer_skipPermissionsDialog(void *env, void *self,
                                                     const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// UnityPlayer.startOrientationListener(int) — start watching the accelerometer
// for screen rotation, and report whether that succeeded.
//
// Transcribed from this APK's own UnityPlayer.smali rather than decided: the
// body constructs an OrientationEventListener, asks it `canDetectOrientation()`
// and, when that is false, logs "Orientation Listener cannot detect
// orientation." and returns **false**. That is the arm a headset takes — there
// is no rotating screen here and no orientation sensor is presented — so false
// is a transcription of the device we present rather than a refusal.
//
// It also has to be false rather than the more agreeable true: the true arm
// promises the listener is ENABLED, i.e. that orientation callbacks will keep
// arriving, and the guest is entitled to wait for one. A silent yes is worse
// than an honest no.
static klj_val klj_UnityPlayer_startOrientationListener(void *env, void *self,
                                                        const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// UnityPlayer.startActivityIndicator(int) / .stopActivityIndicator() — show and
// hide Android's loading spinner.
//
// A genuine no-op rather than an unimplemented one, and the smali is what says
// so: both bodies do nothing but `postOnUiThread` a Runnable, and each Runnable
// bottoms out in a ProgressBar being added to or removed from the activity's
// view hierarchy, inside a try/catch that only logs. Neither returns a value and
// neither calls back into native, so there is nothing the guest can observe.
//
// There is no Android view hierarchy here at all — the guest's frames reach a
// compositor of ours, not a FrameLayout — so the spinner has nowhere to be
// drawn. This is the "only implement what is forced" rule landing on its easy
// side: the forced behaviour is to return.
static klj_val klj_UnityPlayer_startActivityIndicator(void *env, void *self,
                                                      const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// The manifest permission model; defined with the other permission entry
// points further down, and read by three callers rather than two now.
 int klj_permission_state(const char *p, const char **why);

// UnityPlayer.requestUserAuthorization(String) — ask the user for one runtime
// permission and BLOCK until they answer (its body builds a
// UnityPermissions$ModalWaitForPermissionResponse and calls waitForResponse()).
//
// Returning is the answer, and returning IMMEDIATELY is the point. There is no
// user to ask, and the permission's state is already settled by the manifest
// model that checkSelfPermission reads — so there is nothing to wait for and
// nothing this call could change. The one thing it must not do is model the
// wait: this is the caller's thread, and a wait for a response no one can send
// is a hang rather than an error.
static klj_val klj_UnityPlayer_requestUserAuthorization(void *env, void *self,
                                                        const klj_val *a, int n) {
    (void)env; (void)self;
    const char *p = n > 0 ? klj_str(a[0].l) : "", *why = "";
    int granted = klj_permission_state(p, &why);
    KLJ_LOG("UnityPlayer.requestUserAuthorization(\"%s\") — no user to ask; "
            "it stays %s%s", p, granted ? "GRANTED" : "DENIED", why);
    return (klj_val){.l = NULL};
}

// UnityPermissions.hasUserAuthorizedPermission(Activity, String) — the STATIC
// form of the same question Context.checkCallingOrSelfPermission and
// Activity.checkSelfPermission already answer, so it reads the same
// klj_permission_state rather than deciding again. A permission that is granted
// through one door and denied through another is the group-answer mistake the
// display panel and the GLES capability set exist to avoid, and this door is
// the one Unity's C# Permission.HasUserAuthorizedPermission takes.
//
// Note the signature in g_bindings: Unity builds it with DOTS in the class name
// (`Lcom.unity3d.player.UnityPlayerActivity;`), not slashes. That is the guest's
// string and a binding is matched on it exactly, so it is transcribed as the
// guest spells it rather than corrected.
static klj_val klj_UnityPermissions_hasUserAuthorized(void *env, void *self,
                                                      const klj_val *a, int n) {
    (void)env; (void)self;
    const char *p = n > 1 ? klj_str(a[1].l) : "", *why = "";
    int granted = klj_permission_state(p, &why);
    KLJ_LOG("UnityPermissions.hasUserAuthorizedPermission(\"%s\") -> %s%s", p,
            granted ? "true" : "false", why);
    return (klj_val){.j = granted ? 1u : 0u};
}

// Resources.getIdentifier looks a name up in the APK's resource table. We have
// no resource table, and 0 is Android's own "no such resource" — the caller must
// already handle it, since a name that is not in the APK returns the same thing.
// Logged, because it names what the guest expected to find.
static klj_val klj_Resources_getIdentifier(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("Resources.getIdentifier(\"%s\", \"%s\", \"%s\") -> 0 (no resource table)",
            n > 0 ? klj_str(a[0].l) : "", n > 1 ? klj_str(a[1].l) : "",
            n > 2 ? klj_str(a[2].l) : "");
    return (klj_val){.j = 0};
}

static klj_val klj_Integer_parseInt(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *t = n > 0 ? klj_str(a[0].l) : NULL;
    // Java throws NumberFormatException on garbage; nothing here can throw, and
    // the only caller is FMOD reading back the audio properties we just handed
    // it, so the input is our own string. strtol's 0 is the right answer for
    // anything else and the log says when that happened.
    long v = t ? strtol(t, NULL, 10) : 0;
    if (!t) KLJ_LOG("Integer.parseInt(null) -> 0");
    return (klj_val){.j = (uint64_t)(int64_t)(int32_t)v};
}

const klj_binding klj_bind_display[] = {
    {"java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", klj_Integer_parseInt},
    {"com/unity3d/player/UnityPermissions", "hasUserAuthorizedPermission",
     "(Lcom.unity3d.player.UnityPlayerActivity;Ljava/lang/String;)Z",
                                                 klj_UnityPermissions_hasUserAuthorized},

    // ---- display, window and orientation ----
    {"android/hardware/display/DisplayManager", "getDisplay",
     "(I)Landroid/view/Display;", klj_DisplayManager_getDisplay},
    {"android/hardware/display/DisplayManager", "registerDisplayListener",
     "(Landroid/hardware/display/DisplayManager$DisplayListener;Landroid/os/Handler;)V",
     klj_DisplayManager_registerDisplayListener},
    {"android/hardware/display/DisplayManager", "unregisterDisplayListener",
     "(Landroid/hardware/display/DisplayManager$DisplayListener;)V",
     klj_DisplayManager_unregisterDisplayListener},
    {"android/view/Display", "getDisplayId",   "()I", klj_Display_getDisplayId},
    {"android/view/Display", "getWidth",       "()I", klj_Display_getWidth},
    {"android/view/Display", "getHeight",      "()I", klj_Display_getHeight},
    {"android/view/Display", "getRotation",    "()I", klj_Display_getRotation},
    {"android/view/Display", "getMetrics",     "(Landroid/util/DisplayMetrics;)V", klj_Display_getMetrics},
    {"android/view/Display", "getRealMetrics", "(Landroid/util/DisplayMetrics;)V", klj_Display_getMetrics},
    {"android/view/Display", "isWideColorGamut",              "()Z", klj_Display_isWideColorGamut},
    {"android/view/Display", "isHdr",                         "()Z", klj_Display_isHdr},
    {"android/view/Display", "getSupportedModes",
     "()[Landroid/view/Display$Mode;", klj_Display_getSupportedModes},
    {"android/view/Display$Mode", "getModeId",         "()I", klj_Mode_getModeId},
    {"android/view/Display$Mode", "getPhysicalWidth",  "()I", klj_Mode_getPhysicalWidth},
    {"android/view/Display$Mode", "getPhysicalHeight", "()I", klj_Mode_getPhysicalHeight},
    {"android/view/Display$Mode", "getRefreshRate",    "()F", klj_Mode_getRefreshRate},
    {"com/unity3d/player/UnityPlayer", "skipPermissionsDialog", "()Z",
     klj_UnityPlayer_skipPermissionsDialog},
    {"com/unity3d/player/UnityPlayer", "startOrientationListener", "(I)Z",
     klj_UnityPlayer_startOrientationListener},
    // Both spinner entry points share one handler: each is a return, and giving
    // "show" and "hide" separate empty bodies would only invite one of them to
    // grow a meaning the other does not have.
    {"com/unity3d/player/UnityPlayer", "startActivityIndicator", "(I)V",
     klj_UnityPlayer_startActivityIndicator},
    {"com/unity3d/player/UnityPlayer", "stopActivityIndicator", "()V",
     klj_UnityPlayer_startActivityIndicator},
    {"com/unity3d/player/UnityPlayer", "requestUserAuthorization",
     "(Ljava/lang/String;)V", klj_UnityPlayer_requestUserAuthorization},
    {"android/content/res/Resources", "getIdentifier",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", klj_Resources_getIdentifier},
    {0}
};
