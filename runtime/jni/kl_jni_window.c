// window flags and input: decor view, InputDevice/InputManager,
// AudioManager's device census, window insets
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

// ------------------------------------------------------ window flags / input
//
// The batch nativeRender reaches after audio. Mostly constants, and the methods
// that consume them are recorded rather than applied: there is no window manager
// here to keep a screen on or hide a navigation bar, and pretending otherwise
// would be inventing behaviour rather than reporting it.
static klj_val klj_Window_getDecorView(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *view;
    return klj_singleton("android/view/View", &view);
}

static klj_val klj_Window_setFlags(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("Window.setFlags(0x%llx, mask 0x%llx) — recorded, not applied",
            n > 0 ? (unsigned long long)a[0].j : 0ull,
            n > 1 ? (unsigned long long)a[1].j : 0ull);
    return (klj_val){.j = 0};
}

static klj_val klj_PowerManager_sustainedPerf(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // Answered false because it is true: nothing here honours a sustained
    // performance request, and claiming it would have Unity size its frame
    // budget against a guarantee we do not make.
    return (klj_val){.j = 0};
}

// BatteryManager — the headset's own battery, which Steam Link reads through
// IsHmdBatteryCharging()/GetHmdBatteryLevel() and PUBLISHES TO THE HOST as a
// device property; the Steam client shows it next to the headset. So it is
// telemetry, not a control input: nothing here changes streaming behaviour, and
// the cost of a wrong answer is a wrong number on someone's desktop.
//
// Answered as a headset on its battery and nearly full, which is what a Vision
// Pro on its pack is for most of a session. Both are knobs because neither is
// measured yet: on device the real values are available (UIDevice's battery
// monitoring), and wiring them is the honest fix — until then a fixed answer at
// least does not fluctuate, and a fluctuating invented number would be worse
// than a static one.
static klj_val klj_BatteryManager_isCharging(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // The charging flag lives in the kl_ovrp battery seam — the single source
    // both this Java answer and ovrp_GetSystemBatteryLevel2 read, so a visionOS
    // frontend pushing the real UIDevice state updates every consumer at once.
    // KL_BATTERY_CHARGING still overrides via the seam's own env read.
    return (klj_val){.j = (uint64_t)kl_ovrp_battery_charging()};
}

// getIntProperty(id). BATTERY_PROPERTY_CAPACITY is 4 and is the only one this
// guest asks for; anything else answers Integer.MIN_VALUE, which is what
// Android returns for a property the device does not expose.
static klj_val klj_BatteryManager_getIntProperty(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int id = n > 0 ? (int)(int32_t)a[0].j : 0;
    if (id == 4) return (klj_val){.j = (uint32_t)kl_ovrp_battery_level()};
    return (klj_val){.j = (uint32_t)INT32_MIN};
}

// The two Touch controllers, as Android input device ids. See
// klj_InputDevice_getDeviceIds for why the enumeration is load-bearing.
#define KLJ_INPUT_DEV_LTOUCH 8
#define KLJ_INPUT_DEV_RTOUCH 9

static klj_val klj_InputDevice_getDeviceIds(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // The two Touch controllers. This is not cosmetic: Unity's Android player
    // builds its *joystick list* from this enumeration, and Input.GetAxis with
    // "Get Motion from all Joysticks" reads nothing when the list is empty. An
    // empty answer here is why Beat Saber's TriggerLeftHand/TriggerRightHand
    // axes (InputManager joystick axes 8 and 9) stayed at zero even though
    // libunity was faithfully filling the axis values from our
    // ovrp_GetControllerState — the values had no device to land on, so the
    // menu pointer never activated and no click ever occurred.
    //
    // On a Quest the Touch controllers really are Android input devices, so
    // reporting them is the truthful answer, not an invented one. Ids are
    // arbitrary but must be stable: Unity keys its joystick slots on them.
    static void *ids;
    if (!ids) {
        ids = klj_new_array('I', NULL, 2);
        int32_t *v = klj_arr(ids)->data;
        v[0] = KLJ_INPUT_DEV_LTOUCH;
        v[1] = KLJ_INPUT_DEV_RTOUCH;
    }
    KLJ_LOG("InputDevice.getDeviceIds() -> [%d, %d] (the two Touch controllers)",
            KLJ_INPUT_DEV_LTOUCH, KLJ_INPUT_DEV_RTOUCH);
    return (klj_val){.l = ids};
}

// An InputDevice instance. One per controller, allocated once and reused: the
// object identity is what Unity keys its joystick slot on, and a fresh object
// per getDevice() would look like a hot-plug every frame.
//
// The values describe a Quest Touch controller, which is what the guest is
// told it is holding everywhere else (Build.MODEL, ovrp_GetSystemProductName).
// SOURCE_GAMEPAD|SOURCE_JOYSTICK is what makes Unity treat it as a joystick at
// all — a device with neither source is enumerated and then ignored.
#define KLJ_SOURCE_GAMEPAD   0x00000401
#define KLJ_SOURCE_JOYSTICK  0x01000010

typedef struct {
    int         id;
    const char *name;
    const char *descriptor;
} klj_inputdev;

static klj_inputdev g_input_devs[] = {
    { KLJ_INPUT_DEV_LTOUCH, "Oculus Touch Controller - Left",  "klepton-touch-l" },
    { KLJ_INPUT_DEV_RTOUCH, "Oculus Touch Controller - Right", "klepton-touch-r" },
};

static klj_inputdev *klj_inputdev_of(void *self) {
    klj_object *o = klj_as_object(self);
    return o ? o->data : NULL;
}

static klj_val klj_InputDevice_getDevice(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int id = (n > 0) ? (int)(int32_t)a[0].j : -1;
    static void *cache[sizeof g_input_devs / sizeof g_input_devs[0]];
    for (unsigned i = 0; i < sizeof g_input_devs / sizeof g_input_devs[0]; i++) {
        if (g_input_devs[i].id != id) continue;
        if (!cache[i]) {
            cache[i] = klj_new_object_data("android/view/InputDevice", &g_input_devs[i]);
            klj_as_object(cache[i])->pinned = 1;   // survives frame pops
        }
        KLJ_LOG("InputDevice.getDevice(%d) -> %s", id, g_input_devs[i].name);
        return (klj_val){.l = cache[i]};
    }
    KLJ_LOG("InputDevice.getDevice(%d) -> null (unknown id)", id);
    return (klj_val){.l = NULL};
}

static klj_val klj_InputDevice_getName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_inputdev *d = klj_inputdev_of(self);
    return (klj_val){.l = kl_jni_new_string(d ? d->name : "")};
}

static klj_val klj_InputDevice_getId(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_inputdev *d = klj_inputdev_of(self);
    return (klj_val){.j = (uint64_t)(d ? d->id : -1)};
}

static klj_val klj_InputDevice_getSources(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = KLJ_SOURCE_GAMEPAD | KLJ_SOURCE_JOYSTICK};
}

static klj_val klj_InputDevice_getDescriptor(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_inputdev *d = klj_inputdev_of(self);
    return (klj_val){.l = kl_jni_new_string(d ? d->descriptor : "")};
}

// USB product/vendor ids. libunity calls these from its joystick-descriptor
// builder (guest libunity 0xf4dea8/0xf4dfe0) purely to name the device — the
// controllers are already enumerated via getDeviceIds and the real input never
// flows down this path (it goes through OVRPlugin's GetControllerState4). The
// controllers are not USB devices here, so 0 is the honest answer; anything
// more specific would be invented for a string nobody acts on.
static klj_val klj_InputDevice_getProductId(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

static klj_val klj_InputDevice_getVendorId(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// The controllers are physical devices, not a synthetic mouse/keyboard vdev, so
// isVirtual() is false — same informational-class query as getProductId above.
static klj_val klj_InputDevice_isVirtual(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// Unity reads the joystick axis table from getMotionRanges(). The honest answer
// for THIS device is an EMPTY list: every axis is served through OVRPlugin's
// GetControllerState4, not through Android InputDevice motion ranges — the game
// reads controller input over OVRPlugin, and a device reporting "no axes on the
// InputDevice path" cannot disagree with what OVRPlugin reports. Building a
// fake axis table here would be guessing at conventions nothing reads, and an
// empty set is the configuration Unity already handles for axis-less devices.
static klj_val klj_InputDevice_getMotionRanges(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_list(NULL, 0)};
}

static klj_val klj_InputDevice_getMotionRange(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};   // no InputDevice-backed axis exists
}

static klj_val klj_InputManager_registerListener(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // Same shape as registerDisplayListener: recorded, and nothing is owed a
    // callback because our device set never changes.
    KLJ_LOG("InputManager.registerInputDeviceListener — recorded; the device set never changes");
    return (klj_val){.l = NULL};
}

// ...and the set itself, which Beat Saber 1.40 asks for through InputManager
// where 1.28 asked through InputDevice. Two doors, ONE answer: this delegates
// to klj_InputDevice_getDeviceIds rather than deciding again, so the two can
// never disagree about which controllers exist. An empty answer here is the
// bug that comment describes — Unity builds its joystick list from this
// enumeration, and with no devices in it the trigger axes read zero however
// faithfully ovrp_GetControllerState is filled in.
static klj_val klj_InputDevice_getDeviceIds(void *env, void *self, const klj_val *a, int n);
static klj_val klj_InputDevice_getDevice(void *env, void *self, const klj_val *a, int n);

// ...and the same for the lookup: InputManager.getInputDevice(id) is
// InputDevice.getDevice(id) asked through the instance rather than the static.
static klj_val klj_InputManager_getInputDevice(void *env, void *self, const klj_val *a, int n) {
    return klj_InputDevice_getDevice(env, self, a, n);
}

static klj_val klj_InputManager_getInputDeviceIds(void *env, void *self, const klj_val *a, int n) {
    return klj_InputDevice_getDeviceIds(env, self, a, n);
    return (klj_val){.j = 0};
}

 klj_val klj_AudioManager_getStreamVolume(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    // Android's music stream runs 0..15 and Unity scales its mixer against it.
    // Full volume is the honest answer for a host that has no volume control of
    // its own to report.
    KLJ_LOG("AudioManager.getStreamVolume(%lld) -> 15 (of 15)",
            n > 0 ? (long long)(int32_t)a[0].j : -1);
    return (klj_val){.j = 15};
}

// ...and the rest of the volume model, which has to agree with the line above:
// the music stream runs 0..15 and sits at 15. Reporting a different maximum
// here than getStreamVolume answers against would make the guest compute a
// fraction greater than one, which is the display-panel group-answer rule in
// yet another subsystem.
static klj_val klj_AudioManager_getStreamMinVolume(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}
static klj_val klj_AudioManager_getStreamMaxVolume(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 15};
}
// Writes are accepted and dropped. The host's own volume is the user's, not
// the guest's to set, and kl_audio.c mixes at unity gain — so honouring these
// would mean turning down a device the person is also using for everything
// else. The read-back above is unaffected because nothing here records state.
static klj_val klj_AudioManager_setStreamVolume(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("AudioManager.setStreamVolume(stream %lld, %lld) — accepted and dropped; "
            "the host device's volume is the user's",
            n > 0 ? (long long)(int32_t)a[0].j : -1,
            n > 1 ? (long long)(int32_t)a[1].j : -1);
    return (klj_val){.j = 0};
}

static klj_val klj_UnityPlayer_getLaunchURL(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // null, and that is the answer rather than a placeholder: the app was not
    // launched from a deep link, and Unity checks for null explicitly.
    return (klj_val){.l = NULL};
}


static klj_val klj_AudioManager_getProperty(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *k = n > 0 ? klj_str(a[0].l) : "";
    // The keys are the values of AudioManager.PROPERTY_* below; match on the
    // documented strings so the two cannot drift apart.
    if (strstr(k, "OUTPUT_SAMPLE_RATE")) {
        KLJ_LOG("AudioManager.getProperty(OUTPUT_SAMPLE_RATE) -> %d", KLJ_AUDIO_RATE);
        return (klj_val){.l = kl_jni_new_string("48000")};
    }
    if (strstr(k, "OUTPUT_FRAMES_PER_BUFFER")) {
        KLJ_LOG("AudioManager.getProperty(OUTPUT_FRAMES_PER_BUFFER) -> %d", KLJ_AUDIO_FRAMES);
        return (klj_val){.l = kl_jni_new_string("256")};
    }
    // Android returns null for an unknown property, and FMOD handles that.
    KLJ_LOG("AudioManager.getProperty(\"%s\") -> null (unknown property)", k);
    return (klj_val){.l = NULL};
}

// android.media.AudioSystem is the hidden framework class behind AudioManager,
// and Unity 2018.4's FMOD build reaches for it directly where 2019.4 went
// through getProperty(). Same device, so it MUST answer the same two numbers --
// this is the group-answer rule and the whole reason those live in one macro
// each: FMOD sizes its mixer from the rate and its buffer from the frame count,
// and a run where the two doors disagree is a mixer configured for a device
// that does not exist.
static klj_val klj_AudioSystem_getPrimaryOutputSamplingRate(void *env, void *self,
                                                            const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("AudioSystem.getPrimaryOutputSamplingRate() -> %d", KLJ_AUDIO_RATE);
    return (klj_val){.j = KLJ_AUDIO_RATE};
}
static klj_val klj_AudioSystem_getPrimaryOutputFrameCount(void *env, void *self,
                                                          const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("AudioSystem.getPrimaryOutputFrameCount() -> %d", KLJ_AUDIO_FRAMES);
    return (klj_val){.j = KLJ_AUDIO_FRAMES};
}

// No Bluetooth audio is routed here: kl_audio opens a CoreAudio output unit on
// whatever the host has selected, and nothing in this shim can present an A2DP
// device. False is the truthful answer rather than a convenient one -- Unity
// uses it to pick a higher output latency, which would be wrong for the device
// we actually play through.
static klj_val klj_AudioManager_isBluetoothA2dpOn(void *env, void *self,
                                                  const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("AudioManager.isBluetoothA2dpOn() -> false");
    return (klj_val){.j = 0};
}

// ---------------------------------------------------------- window insets
//
// Unity 2018.4 asks the decor view for its insets to compute a safe area; the
// 2019.4 build in this project never did. Answered as one description of one
// window, like the display and audio groups above: a headset draws into an
// immersive surface with no status bar, no navigation bar and no display
// cutout, so every inset is genuinely zero. The object carries no payload
// because its accessors are constant fields (see g_fields) -- there is one
// window and every WindowInsets describes it.
//
// A real Android view returns null here when it is not attached to a window,
// and answering null would also "work"; it is the wrong answer because our view
// IS attached, and it would send Unity down its no-information path instead of
// telling it the insets are zero.
#define KLJ_CLASS_WINDOWINSETS "android/view/WindowInsets"

static klj_val klj_View_getRootWindowInsets(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("View.getRootWindowInsets() -> WindowInsets (all zero — immersive surface)");
    return (klj_val){.l = kl_jni_new_object(KLJ_CLASS_WINDOWINSETS)};
}

// The listener contract: return the insets the view did NOT consume. We consume
// nothing, so the argument comes straight back. Returning a fresh object would
// be equivalent today and would stop being so the moment insets are non-zero.
static klj_val klj_View_onApplyWindowInsets(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("View.onApplyWindowInsets() -> the insets unchanged (nothing consumed)");
    return (klj_val){.l = n > 0 ? a[0].l : kl_jni_new_object(KLJ_CLASS_WINDOWINSETS)};
}

// Recorded and dropped. The listener fires when the insets CHANGE, and ours are
// constant for the life of the process, so a registration that is never called
// back is the accurate behaviour rather than an unimplemented one.
static klj_val klj_View_setOnApplyWindowInsetsListener(void *env, void *self,
                                                       const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("View.setOnApplyWindowInsetsListener() — recorded; insets never change, "
            "so it is never called back");
    return (klj_val){.l = NULL};
}

// Null is what Android itself returns when the display has no cutout, and this
// display has none -- there is no notch in a headset. It is the same answer a
// Quest gives, so Unity takes the branch it takes on the real device.
static klj_val klj_WindowInsets_getDisplayCutout(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("WindowInsets.getDisplayCutout() -> null (no cutout on this display)");
    return (klj_val){.l = NULL};
}

static klj_val klj_PackageManager_hasSystemFeature(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *f = n > 0 ? klj_str(a[0].l) : "";
    // Truthfully, for the device we present: a Quest 2 reports low-latency audio
    // and it is the branch FMOD wants. Anything else is answered no rather than
    // guessed yes — claiming a feature is a promise, same rule as the EGL and GL
    // extension strings.
    int has = strstr(f, "audio.low_latency") != NULL;
    KLJ_LOG("PackageManager.hasSystemFeature(\"%s\") -> %s", f, has ? "true" : "false");
    return (klj_val){.j = (uint64_t)has};
}

// Context.bindService(Intent, ServiceConnection, int) — "connect me to that
// service". Answered FALSE, which is exactly what Android returns when nothing
// installed can service the Intent, and nothing here can: the caller is Unity
// 2018.4's analytics reaching for
// com.google.android.gms.ads.identifier.service.START, i.e. Play Services'
// advertising ID, and there is no Play Services on this host. Reporting its
// absence is the same story kl_ovrplat tells about the Oculus platform.
//
// False is also the only answer that does not HANG, which is why this is not a
// place to be permissive. bindService is asynchronous and `true` is a PROMISE
// that ServiceConnection.onServiceConnected will be called later; we have no
// service to call it from, so a fabricated true leaves the guest waiting on a
// callback that cannot come — a wait that never ends, rather than an error it
// can handle. False is a state its own code already handles,
// and it leaves the connection with nothing queued against it, so the matching
// unbindService has nothing to undo either.
static klj_val klj_Context_bindService(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_object *intent = n > 0 ? klj_as_object(a[0].l) : NULL;
    const char *action = intent ? (const char *)intent->data : NULL;
    KLJ_LOG("Context.bindService(\"%s\") -> false (no such service on this host; "
            "the connection is never called back)", action ? action : "(no action)");
    return (klj_val){.j = 0};
}

// ...and its partner, which the guest calls on the way out whether or not the
// bind succeeded. Nothing was ever bound, so there is nothing to undo and a
// no-op IS the behaviour rather than a stub standing in for one.
//
// Real Android throws IllegalArgumentException for a ServiceConnection that was
// never bound, and we deliberately do not: this project's exception state is
// always clear, so throwing would mean building machinery for a
// path whose only purpose is to be caught and ignored. Returning quietly is
// what the caller's catch block would have produced anyway.
static klj_val klj_Context_unbindService(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("Context.unbindService() — nothing was bound, so nothing to undo");
    return (klj_val){.l = NULL};
}

static klj_val klj_Context_checkPermission(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    // Answered from the SAME manifest model as checkSelfPermission below. It
    // used to grant everything unconditionally, which is the group-answer
    // mistake in its purest form: two entry points asking one question and
    // disagreeing. Beat Saber 1.40 is where that stopped being theoretical —
    // it asks for android.permission.BLUETOOTH, which this manifest does not
    // declare, took the GRANTED at its word, and then called GetObjectClass on
    // the null adapter that followed. A permission we grant is a capability we
    // have promised to have.
    const char *p = n > 0 ? klj_str(a[0].l) : "", *why = "";
    int granted = klj_permission_state(p, &why);
    KLJ_LOG("Context.checkCallingOrSelfPermission(\"%s\") -> %s%s", p,
            granted ? "GRANTED" : "DENIED", why);
    return (klj_val){.j = granted ? 0 : (uint64_t)(int64_t)-1};
}

// Activity.checkSelfPermission(String) — "may I use this, right now". Two
// separate reasons to say no, and they are not interchangeable, so the log says
// which one applied:
//
//   not declared   Android returns DENIED for a permission the app never put in
//                  its manifest, whatever the user did. Granting one would be
//                  inventing a capability out of nothing.
//   no hardware    declared, but the device we present does not have the part.
//                  We present a Quest 2 (the settled decision), which has hand
//                  tracking and no eye or face tracker — so the eye/face
//                  permissions are denied for the same reason
//                  PackageManager.hasSystemFeature answers no to everything but
//                  low-latency audio. These have to agree: a granted permission
//                  for a sensor that reports nothing is the group-answer mistake
//                  the display panel and the GLES capability set already avoid.
//
// Everything else the app declared is granted. That is what a Quest reports for
// this app by the time someone is pairing: the dangerous ones (RECORD_AUDIO for
// stream microphone input, the ACCESS_*_LOCATION that reading an SSID needs)
// were requested at first run, and there is no user here to ask.
// This is the UNION over the manifests of every guest in the tree, the same
// shape and for the same reason as kl_libc_table.h: one table, and a name that
// belongs to the other target costs nothing here. It is not free in principle —
// granting a permission this guest never declared is the very bug the caller
// above documents — but no guest asks about another's, and the alternative is
// parsing binary AXML at runtime to answer a question two entry points ask.
// The first four are Beat Saber's whole manifest; the rest are Steam Link's.
static const char *const g_manifest_permissions[] = {
    "android.permission.ACCESS_NETWORK_STATE",
    "android.permission.INTERNET",
    "android.permission.READ_EXTERNAL_STORAGE",
    "android.permission.WRITE_EXTERNAL_STORAGE",
    "android.permission.ACCESS_COARSE_LOCATION",
    "android.permission.ACCESS_FINE_LOCATION",
    "android.permission.ACCESS_WIFI_STATE",
    "android.permission.BLUETOOTH_CONNECT",
    "android.permission.CHANGE_NETWORK_STATE",
    "android.permission.MODIFY_AUDIO_SETTINGS",
    "android.permission.RECORD_AUDIO",
    "android.permission.VIBRATE",
    "android.permission.WAKE_LOCK",
    "com.oculus.permission.EYE_TRACKING",
    "com.oculus.permission.FACE_TRACKING",
    "com.oculus.permission.HAND_TRACKING",
    "com.oculus.permission.RENDER_MODEL",
    "com.oculus.permission.WIFI_LOCK",
    "com.picovr.permission.EYE_TRACKING",
    "com.picovr.permission.FACE_TRACKING",
    "org.khronos.openxr.permission.OPENXR",
    "org.khronos.openxr.permission.OPENXR_SYSTEM",
};

// Declared, but not on the headset we claim to be. Vendor-prefixed twice over
// because the manifest carries both the Oculus and the Pico spelling of each.
static const char *const g_absent_hardware_permissions[] = {
    "com.oculus.permission.EYE_TRACKING",
    "com.oculus.permission.FACE_TRACKING",
    "com.picovr.permission.EYE_TRACKING",
    "com.picovr.permission.FACE_TRACKING",
};

// ...and, like the <meta-data> Bundle, the declared set is read from the
// guest's OWN manifest where one is unpacked, falling back to the union above.
// The union is only ever approximately right: it is every guest's permissions
// at once, so it grants this guest things it never declared and — the way it
// actually bit — misses what a guest declares that no other one does. Beat
// Saber 1.28 declares com.oculus.permission.DEVICE_CONFIG_PUSH_TO_CLIENT, which
// appears in no other manifest here and so was absent from the list, and 1.40
// declares READ_/WRITE_EXTERNAL_STORAGE. Android answers this question by
// looking at the manifest; so do we.
// The <meta-data> parser's attribute helper; both readers of AndroidManifest.xml
// share it so they cannot disagree about how an attribute is spelled.

static const char *const *klj_declared_permissions(size_t *count) {
    static const char **list;
    static size_t n;
    static int tried;
    if (!tried) {
        tried = 1;
        char path[1024];
        snprintf(path, sizeof path, "%s/../AndroidManifest.xml", g_assets_dir);
        FILE *f = fopen(path, "rb");
        if (f) {
            long size = (fseek(f, 0, SEEK_END), ftell(f));
            char *xml = size > 0 ? malloc((size_t)size + 1) : NULL;
            if (xml) {
                rewind(f);
                xml[fread(xml, 1, (size_t)size, f)] = '\0';
                size_t cap = 16;
                list = calloc(cap, sizeof *list);
                for (const char *p = xml; (p = strstr(p, "<uses-permission")) != NULL; ) {
                    const char *end = strchr(p, '>');
                    if (!end) break;
                    const char *k = klj_xml_attr(p, end, "android:name=\"");
                    p = end;
                    const char *ke = k ? strchr(k, '"') : NULL;
                    if (!ke) continue;
                    if (n == cap) { cap *= 2; list = realloc(list, cap * sizeof *list); }
                    list[n++] = strndup(k, (size_t)(ke - k));
                }
                free(xml);
            }
            fclose(f);
        }
        if (n) KLJ_LOG("manifest <uses-permission>: %zu declared", n);
        else   KLJ_LOG("no readable <uses-permission> beside %s — using the union "
                       "list, which is every guest's permissions at once", g_assets_dir);
    }
    if (n) { *count = n; return list; }
    *count = sizeof g_manifest_permissions / sizeof *g_manifest_permissions;
    return g_manifest_permissions;
}

 int klj_permission_state(const char *p, const char **why) {
    for (size_t i = 0; i < sizeof g_absent_hardware_permissions /
                           sizeof *g_absent_hardware_permissions; i++)
        if (strcmp(p, g_absent_hardware_permissions[i]) == 0) {
            *why = " (the Quest 2 we present has no such sensor)";
            return 0;
        }
    size_t n = 0;
    const char *const *declared = klj_declared_permissions(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(p, declared[i]) == 0) { *why = ""; return 1; }
    *why = " (not declared in the guest's manifest)";
    return 0;
}

static klj_val klj_Context_checkSelfPermission(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *p = n > 0 ? klj_str(a[0].l) : "", *why = "";
    int granted = klj_permission_state(p, &why);
    KLJ_LOG("Activity.checkSelfPermission(\"%s\") -> %s%s", p,
            granted ? "GRANTED" : "DENIED", why);
    return (klj_val){.j = granted ? 0 : (uint64_t)(int64_t)-1};
}

// SteamLink.startVRLink(String) — the 2D->VR handoff, and by
// the time it is called the hard part has already succeeded: the shell paired,
// the host sent its authorization proof request, and the answer was
// k_ERemoteDeviceStreamingSuccess. Its body (smali, SteamLink.smali:1676) is an
// Intent for `com.valvesoftware.steamlinkvr/android.app.NativeActivity` carrying
// sOriginalPackage / sOriginalActivity / sStartInfo / sArgs, a startActivity,
// and finishAndRemoveTask on this one.
//
// The VR activity is a second FRONT DOOR — a different guest library, chain and
// lifecycle — so whether it can be honoured is the driver's question, not this
// file's. `kl_jni_set_vrlink_handoff` is how a driver answers it; see the
// header. With no handler installed, or with one that returns because it could
// not do the handoff, this stops by name like every other unimplemented call.
//
// What it does FIRST either way is print the payload, because that string is
// the authorized session and it is expensive to get back: reaching this line
// costs a pairing round trip with a real Steam host and a person typing a PIN.
// `~` is the field separator the guest itself splits on, and field 3 is what it
// forwards as sStartInfo.
//
// It must never simply RETURN. This is the only path by which the guest can be
// told its activity started, and the shell calls finishAndRemoveTask() the
// instant it does — so telling it that falsely ends the run with no picture and
// no reason given.
static void (*g_vrlink_handoff)(const char *sargs);

void kl_jni_set_vrlink_handoff(void (*fn)(const char *sargs)) {
    g_vrlink_handoff = fn;
}

static klj_val klj_SL_startVRLink(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *args = n > 0 ? klj_str(a[0].l) : "";
    KLJ_LOG("SteamLink.startVRLink: the shell paired and the host authorized the "
            "session — this is the 2D -> VR handoff");
    KLJ_LOG("  sArgs = \"%s\"", args ? args : "");
    if (args) {
        int  field = 0;
        const char *p = args;
        while (field < 16) {
            const char *sep = strchr(p, '~');
            KLJ_LOG("  sArgs[%d] = \"%.*s\"%s", field,
                    sep ? (int)(sep - p) : (int)strlen(p), p,
                    field == 3 ? "   <- forwarded as sStartInfo" : "");
            if (!sep) break;
            p = sep + 1; field++;
        }
    }
    if (g_vrlink_handoff && args && *args) g_vrlink_handoff(args);
    KLJ_LOG("  the VR front door was not entered (no handoff handler, or it "
            "could not run) — the session above is what KL_SLINK_SARGS wants");
    kl_fatal_prepare(); abort();
}

// SteamLink.isVRLinkRunning() — "is the OTHER half up". On Android the Java
// answers it from its own bookkeeping around the VRLink activity; here the
// only party that knows is the DRIVER (the app runs both front doors in one
// process, `build/m_slink` re-execs into the other one), so the driver states
// it — kl_app.c sets it at the handoff and at the VR door's onCreate — and
// this binding only repeats it. The default, with nothing set, is false:
// truthful for a shell that has not handed off yet, and for the host shell,
// whose VR half lives in a process it cannot see.
//
// The newer Steam Link build polls this from the shell's background threads
// AFTER startVRLink, so an unbound name here was a run that paired, handed
// off, brought the VR chain up — and then stopped by name in the half that
// was supposed to be finished.
static volatile int g_vrlink_running;

void kl_jni_set_vrlink_running(int on) { g_vrlink_running = on; }

static klj_val klj_SL_isVRLinkRunning(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    int on = g_vrlink_running;
    KLJ_LOG("SteamLink.isVRLinkRunning() -> %s", on ? "true" : "false");
    return (klj_val){.j = (uint64_t)(on != 0)};
}

// Activity.requestPermissions(String[], int) — the runtime prompt. On Android
// this RETURNS IMMEDIATELY and the answer arrives later on the main thread, at
// onRequestPermissionsResult; returning is therefore the whole of the
// synchronous contract, and anything more would be this call pretending to be
// the dialog.
//
// There is no user to prompt, so the answer is the one checkSelfPermission just
// gave — a prompt cannot turn "no such sensor" into yes. What must not happen is
// silence: the guest asked a question, and a request that never resolves is a
// wait that never ends. The delivery path is not invented
// either, it is read off the guest's own SDLActivity bytecode:
// onRequestPermissionsResult computes `grantResults[0] == PERMISSION_GRANTED`
// and calls the static native nativePermissionResult(requestCode, granted). We
// call that native with the same two values.
//
// Deliberately synchronous, where Android is not. Deferring it means a queue
// entry kind that does not exist yet (g_ui_tasks holds Runnables and Messages),
// and this caller — Valve's own code, which requested two permissions and
// carried straight on — does not block on the answer. If a future caller does
// re-enter badly, this is the line to move onto the main looper.
static klj_val klj_Activity_requestPermissions(void *env, void *self, const klj_val *a, int n) {
    (void)self;
    klj_array *arr = n > 0 ? klj_arr(a[0].l) : NULL;
    int32_t    code = n > 1 ? (int32_t)a[1].j : 0;
    int        all_granted = 1;

    for (int i = 0; arr && arr->kind == 'L' && i < arr->len; i++) {
        const char *p = klj_str(((void **)arr->data)[i]), *why = "";
        int granted = klj_permission_state(p ? p : "", &why);
        if (!granted) all_granted = 0;
        KLJ_LOG("Activity.requestPermissions[%d] \"%s\" -> %s%s", i, p ? p : "",
                granted ? "GRANTED" : "DENIED", why);
    }

    // grantResults[0] is what SDLActivity actually reads, so a mixed request is
    // reported by its first element — but only all-granted is answered true, to
    // avoid a partial grant reading as a whole one.
    void (*result)(void *, void *, int32_t, uint8_t) =
        (void (*)(void *, void *, int32_t, uint8_t))
            kl_jni_native("org/libsdl/app/SDLActivity", "nativePermissionResult", NULL);
    KLJ_LOG("Activity.requestPermissions(code=%d) -> nativePermissionResult(%d, %s)%s",
            code, code, all_granted ? "true" : "false",
            result ? "" : " — SKIPPED, the guest registered no such native");
    if (result) result(kl_jni_env(), kl_jni_class("org/libsdl/app/SDLActivity"), code,
                       (uint8_t)(all_granted ? 1 : 0));
    (void)env;
    return (klj_val){.j = 0};
}

const klj_binding klj_bind_window[] = {
    // Reached by pressing *Start Pairing*: the shell checks a permission before
    // it opens the socket. Bound on SteamLink rather than on Context because a
    // binding is matched on the full (class, name, signature) triple and this is
    // where the guest resolves it. See klj_Context_checkSelfPermission for why
    // the answer is the manifest's and not a blanket yes.
    {"com/valvesoftware/steamlink/SteamLink", "checkSelfPermission", "(Ljava/lang/String;)I",
     klj_Context_checkSelfPermission},
    // ...and again on Activity itself, because the VR half's activity IS
    // android.app.NativeActivity and resolves the method there. Same
    // implementation: the answer is a property of the manifest and the device we
    // present, not of which class the guest looked it up on.
    {"android/app/Activity", "checkSelfPermission", "(Ljava/lang/String;)I",
     klj_Context_checkSelfPermission},
    {"android/app/Activity", "requestPermissions", "([Ljava/lang/String;I)V",
     klj_Activity_requestPermissions},
    {"com/valvesoftware/steamlink/SteamLink", "requestPermissions", "([Ljava/lang/String;I)V",
     klj_Activity_requestPermissions},
    // The handoff, reached: the shell has paired, the host has answered
    // k_ERemoteDeviceStreamingSuccess, and this is the 2D frontend handing the
    // authorized session to the VR half. See klj_SL_startVRLink — it stops, but
    // it prints the payload first, because that string IS the session.
    {"com/valvesoftware/steamlink/SteamLink", "startVRLink", "(Ljava/lang/String;)V",
     klj_SL_startVRLink},
    // ...and the question about the other half, answered from the driver's
    // state (see klj_SL_isVRLinkRunning above).
    {"com/valvesoftware/steamlink/SteamLink", "isVRLinkRunning", "()Z",
     klj_SL_isVRLinkRunning},
    {"android/view/Window", "getDecorView", "()Landroid/view/View;", klj_Window_getDecorView},
    {"android/view/Window", "setFlags", "(II)V", klj_Window_setFlags},
    {"android/os/PowerManager", "isSustainedPerformanceModeSupported", "()Z", klj_PowerManager_sustainedPerf},
    {"android/os/BatteryManager", "isCharging", "()Z", klj_BatteryManager_isCharging},
    {"android/os/BatteryManager", "getIntProperty", "(I)I", klj_BatteryManager_getIntProperty},
    {"android/view/InputDevice", "getDeviceIds", "()[I", klj_InputDevice_getDeviceIds},
    {"android/view/InputDevice", "getDevice", "(I)Landroid/view/InputDevice;", klj_InputDevice_getDevice},
    {"android/view/InputDevice", "getName", "()Ljava/lang/String;", klj_InputDevice_getName},
    {"android/view/InputDevice", "getId", "()I", klj_InputDevice_getId},
    {"android/view/InputDevice", "getSources", "()I", klj_InputDevice_getSources},
    {"android/view/InputDevice", "getDescriptor", "()Ljava/lang/String;", klj_InputDevice_getDescriptor},
    {"android/view/InputDevice", "getProductId", "()I", klj_InputDevice_getProductId},
    {"android/view/InputDevice", "getVendorId", "()I", klj_InputDevice_getVendorId},
    {"android/view/InputDevice", "isVirtual", "()Z", klj_InputDevice_isVirtual},
    {"android/view/InputDevice", "getMotionRanges", "()Ljava/util/List;", klj_InputDevice_getMotionRanges},
    {"android/view/InputDevice", "getMotionRange", "(I)Landroid/view/InputDevice$MotionRange;", klj_InputDevice_getMotionRange},
    {"android/hardware/input/InputManager", "registerInputDeviceListener",
     "(Landroid/hardware/input/InputManager$InputDeviceListener;Landroid/os/Handler;)V",
     klj_InputManager_registerListener},
    {"android/hardware/input/InputManager", "getInputDeviceIds", "()[I",
     klj_InputManager_getInputDeviceIds},
    {"android/hardware/input/InputManager", "getInputDevice",
     "(I)Landroid/view/InputDevice;", klj_InputManager_getInputDevice},
    {"android/media/AudioManager", "getStreamVolume", "(I)I", klj_AudioManager_getStreamVolume},
    {"android/media/AudioManager", "getStreamMinVolume", "(I)I", klj_AudioManager_getStreamMinVolume},
    {"android/media/AudioManager", "getStreamMaxVolume", "(I)I", klj_AudioManager_getStreamMaxVolume},
    {"android/media/AudioManager", "setStreamVolume", "(III)V", klj_AudioManager_setStreamVolume},
    {"com/unity3d/player/UnityPlayer", "getLaunchURL", "()Ljava/lang/String;", klj_UnityPlayer_getLaunchURL},
    {"android/media/AudioManager", "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", klj_AudioManager_getProperty},
    {"android/media/AudioManager", "isBluetoothA2dpOn", "()Z", klj_AudioManager_isBluetoothA2dpOn},
    // Unity 2018.4 reads the device's audio configuration off the hidden
    // AudioSystem class instead of AudioManager.getProperty(). Same numbers.
    {"android/media/AudioSystem", "getPrimaryOutputSamplingRate", "()I", klj_AudioSystem_getPrimaryOutputSamplingRate},
    {"android/media/AudioSystem", "getPrimaryOutputFrameCount", "()I", klj_AudioSystem_getPrimaryOutputFrameCount},
    // ...and asks the decor view for its insets to compute a safe area.
    {"android/view/View", "getRootWindowInsets", "()Landroid/view/WindowInsets;", klj_View_getRootWindowInsets},
    {"android/view/View", "onApplyWindowInsets", "(Landroid/view/WindowInsets;)Landroid/view/WindowInsets;", klj_View_onApplyWindowInsets},
    {"android/view/View", "setOnApplyWindowInsetsListener", "(Landroid/view/View$OnApplyWindowInsetsListener;)V", klj_View_setOnApplyWindowInsetsListener},
    {"android/view/WindowInsets", "getDisplayCutout", "()Landroid/view/DisplayCutout;", klj_WindowInsets_getDisplayCutout},
    {"android/content/pm/PackageManager", "hasSystemFeature", "(Ljava/lang/String;)Z", klj_PackageManager_hasSystemFeature},
    {"android/content/Context", "checkCallingOrSelfPermission", "(Ljava/lang/String;)I", klj_Context_checkPermission},
    {"android/content/Context", "bindService", "(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z", klj_Context_bindService},
    {"android/content/Context", "unbindService", "(Landroid/content/ServiceConnection;)V", klj_Context_unbindService},
    {0}
};
