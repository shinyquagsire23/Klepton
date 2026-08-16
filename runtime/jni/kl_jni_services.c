// FMOD's device census, Sentry, the Bluetooth HFP profile,
// AlertDialog, manifest meta-data, package identity, external
// storage, Oculus manifest flags, ActivityThread, AssetManager
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
#include <zlib.h>
#include "kl_jni_int.h"

// ---- org.fmod.FmodAndroidAudioManager — FMOD's Java device census ----
//
// FMOD keeps a Java singleton that answers questions about audio DEVICES:
// which ones exist, whether a Bluetooth headset is among them, whether a
// sample rate is available for capture. VRChat's FMOD build reaches it as soon
// as the output stream is open.
//
// Almost none of this is a decision. Every device question in the class is
// written as a loop over `AudioManager.getDevices(...)`, and this shim already
// answers that with an EMPTY array — so each one falls to its own not-found
// branch, and the values below are that branch read out of the smali rather
// than picked:
//
//   getBluetoothInputDeviceId      -1     (the `const/4 p0, -0x1` arm)
//   isBluetoothInputDeviceAvailable false
//   isBuiltinInputDeviceAvailable   false
//   isBuiltinSpeakerDevice(id)      false
//   isInputSampleRateAvailable(hz)  false
//
// So this is not a second opinion about the audio hardware, it is the same
// opinion arriving through another door — which is the point. The one that
// would be a decision if it disagreed is isBuiltinInputDeviceAvailable, and it
// must be false: kl_aaudio.c refuses capture streams by design (kl_aaudio.h),
// and a census that advertises a microphone the open call then denies is the
// worst of both.
//
// isBluetoothScoConnected reads a field that only startBluetoothSco sets, and
// that call cannot succeed here, so it is false by the same chain. The two
// setters record and do nothing, exactly as HFPStatus's do.
static klj_val klj_fmod_audio_manager(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *singleton;
    if (!singleton) {
        singleton = kl_jni_new_object("org/fmod/FmodAndroidAudioManager");
        ((klj_object *)singleton)->pinned = 1;   // a static getInstance(); it never dies
    }
    return (klj_val){.l = singleton};
}
static klj_val klj_fmod_bt_input_id(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = (uint64_t)-1};    // no device of TYPE_BLUETOOTH_SCO to find
}
static klj_val klj_fmod_false(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}
static klj_val klj_fmod_void(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){0};
}

// UnityPlayer.getNetworkConnectivity() — the native side of Unity's
// Application.internetReachability, and the THIRD door onto a question this
// file already answers twice (ConnectivityManager.getActiveNetworkInfo ->
// isConnected/getType). It reads the same conclusion for the same reason: a
// network that exists is connected, and the device we present is a Quest 2,
// which has no cellular radio — so the only transport it can have is the local
// one.
//
// The values are Unity's NetworkReachability enum, which is not Android's
// ConnectivityManager.TYPE_*: 0 NotReachable, 1 ReachableViaCarrierDataNetwork,
// 2 ReachableViaLocalAreaNetwork. Two tables that happen to overlap at 1 with
// DIFFERENT meanings, which is exactly the trap-16b shape — so the constant is
// spelled out here rather than borrowed from the ConnectivityManager block.
enum { KLJ_UNITY_REACHABLE_VIA_LAN = 2 };
static klj_val klj_UnityPlayer_getNetworkConnectivity(void *env, void *self,
                                                      const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static int said;
    if (!said++)
        KLJ_LOG("UnityPlayer.getNetworkConnectivity() -> ReachableViaLocalAreaNetwork "
                "(the same link NetworkInfo.getType() reports as WIFI)");
    return (klj_val){.j = KLJ_UNITY_REACHABLE_VIA_LAN};
}

// ---- io.sentry.Sentry — an optional SDK this APK does not ship ----
//
// VRChat's managed code reaches for `io.sentry.Sentry` through Unity's
// ReflectionHelper and calls close(). There is NO io/sentry class anywhere in
// this APK: on a device the AndroidJavaClass lookup throws
// ClassNotFoundException and the C# catches it, which is how "close Sentry if
// the Android SDK is present" is written.
//
// We cannot reproduce that, and the reason is a settled design decision rather
// than an oversight: FindClass here never fails (it interns any name) and the
// exception state is always clear, so a guest cannot discover that a class is
// absent. That is right for the Android framework, every class of which we
// implement — and it is exactly wrong for an application's OPTIONAL dependency,
// where the absence IS the answer. The failure it produces is an abort on a
// method call the guest was fully prepared to have fail.
//
// close() is answered rather than the design changed, because a no-op is the
// truth twice over: nothing was ever initialised, so there is nothing to close,
// and Sentry is crash TELEMETRY — "closed, sending nothing" is the state we
// want to be in and the one we are actually in.
//
// Deliberately only close(). If this guest ever calls Sentry.init or
// captureException it should stop by name and be decided on then, because
// those are a translated guest phoning home about a machine it is not running
// on, which is not ours to answer for.
static klj_val klj_Sentry_close(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static int said;
    if (!said++)
        KLJ_LOG("io/sentry/Sentry.close() — this APK ships no Sentry SDK, so "
                "there is nothing initialised to close");
    return (klj_val){0};
}

// ---- JavaBroadcastReceiver: a subscription to a bus with no publisher ----
//
// Unity's helper builds an IntentFilter out of the action names it is given,
// calls Context.registerReceiver with itself, and remembers the interface to
// forward to. Its own onReceive is only ever called BY THE SYSTEM — nothing in
// the guest or in this shim can deliver a broadcast, because there is no
// Android framework here sending them.
//
// So this accepts and does nothing observable, and that is the honest answer
// rather than a stub: a registration whose delivery path does not exist has no
// effect to model. What it must not do is fail, because the caller treats
// registration as unconditional. The actions are NAMED once, because that list
// is the only statement of what the guest expected to hear about — if a title
// ever depends on one arriving, this line is where the work starts.
static klj_val klj_JavaBroadcastReceiver_setReceiver(void *env, void *self,
                                                     const klj_val *a, int n) {
    (void)env; (void)self;
    if (n >= 3) {
        klj_array *acts = klj_arr(a[2].l);
        char buf[256]; buf[0] = 0;
        if (acts && acts->kind == 'L')
            for (int i = 0; i < acts->len; i++) {
                const char *s = klj_str(((void **)acts->data)[i]);
                if (!s) continue;
                if (buf[0]) strlcat(buf, " ", sizeof buf);
                strlcat(buf, s, sizeof buf);
            }
        KLJ_LOG("JavaBroadcastReceiver.setReceiver: registered for [%s] — nothing "
                "here broadcasts, so no onReceive can ever arrive", buf);
    }
    return (klj_val){0};
}

// updateDigest(context, key, value) — the send side of that same dead bus, and
// transcribed rather than guessed (JavaBroadcastReceiver.smali:72). It builds
// `new Intent("UpdateMBDMap")`, puts the pair in a Bundle, aims it at the app's
// OWN package, and sends it; the only receiver that could ever act on it is the
// registration above, which by construction never fires. So a no-op here is the
// whole of the method's observable effect, not a stub of it — the pair is
// logged once so the digest map is at least visible if it ever matters.
//
// Worth knowing where the guest's own version goes next: the sendBroadcast it
// calls is `aGGhd3/aVmTv8/vt1Tu8.sendBroadcast`, i.e. the APK's Appdome layer
// has hooked it. That layer never runs here (we execute no Java), which is why
// this stops at Unity's method rather than inside it.
static klj_val klj_JavaBroadcastReceiver_updateDigest(void *env, void *self,
                                                      const klj_val *a, int n) {
    (void)env; (void)self;
    static int said;
    if (n >= 3 && !said++) {
        const char *k = klj_str(a[1].l), *v = klj_str(a[2].l);
        KLJ_LOG("JavaBroadcastReceiver.updateDigest(\"%s\", \"%s\") — an Intent "
                "broadcast to our own package, which nothing here delivers",
                k ? k : "(null)", v ? v : "(null)");
    }
    return (klj_val){0};
}

// ---- HFPStatus: the Bluetooth hands-free profile, which is not here ----
//
// com/unity3d/player/HFPStatus manages a Bluetooth SCO link so a headset's
// microphone can be recorded from. Nothing on this host presents one, so the
// four methods libunity calls are answered from that single fact rather than
// invented one at a time. They come as a group for the usual reason: a
// requestHFPStat that appears to start something, paired with a getHFPStat that
// says nothing started, is worse than a consistent "no such device".
//
// The honest position is that there is no hands-free device, and the guest's
// own Java already has that path — HFPStatus.requestHFPStat logs
// "startBluetoothSco() failed. no bluetooth device connected." and carries on.
static klj_val klj_HFP_clear(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // Tear-down of a link that was never up: unregisterReceiver + stopBluetoothSco
    // over nothing. Genuinely a no-op, not a stub.
    return (klj_val){0};
}
static klj_val klj_HFP_get(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};      // no SCO link is up, and none can be
}
static klj_val klj_HFP_request(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static int said;
    if (!said++)
        KLJ_LOG("HFPStatus.requestHFPStat() — no Bluetooth hands-free device is "
                "presented, so this is the guest's own \"no bluetooth device "
                "connected\" path");
    return (klj_val){0};
}
static klj_val klj_HFP_set_recording(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    // AudioManager.setMode(MODE_NORMAL) on the far side. Recorded, not applied —
    // the same position as Process.setThreadPriority: the host audio route is
    // not the guest's to pick.
    KLJ_LOG("HFPStatus.setHFPRecordingStat(%d) — recorded, not applied",
            n > 0 ? (int)a[0].j : 0);
    return (klj_val){0};
}

// See kl_jni.h. UnityPlayer's constructor runs initJni and then builds its
// helper objects; this is that second half, for the helpers whose constructors
// hand themselves to libunity through a private native.
//
// Only HFPStatus so far, because only HFPStatus is FORCED — libunity keeps its
// handle and calls clearHFPStat on it unconditionally. Camera2Wrapper (with
// initCamera2Jni) has exactly the same shape two lines earlier in the same
// constructor and is deliberately not done here: nothing has asked for it, and
// a camera we announce is a camera something may try to open.
void kl_jni_unity_construct_helpers(void) {
    static int done;
    if (done) return;
    done = 1;

    // HFPStatus.<init>(Context) stores the context, gets the AudioManager, and
    // calls initHFPStatusJni() — which is how libunity comes to hold this
    // object at all. The two field reads have no observable effect here; the
    // native call is the whole point.
    void *init = kl_jni_native("com/unity3d/player/HFPStatus", "initHFPStatusJni", NULL);
    if (!init) return;             // a guest that never registered it wants none of this
    void *thiz = kl_jni_new_object("com/unity3d/player/HFPStatus");
    ((klj_object *)thiz)->pinned = 1;   // libunity keeps it for the process's life
    KLJ_LOG("HFPStatus: constructed and handed to initHFPStatusJni()");
    kl_jni_local_frame_push();
    ((void (*)(void *, void *))init)(kl_jni_env(), thiz);
    kl_jni_local_frame_pop();
}

// PorterDuff.Mode.CLEAR — the blend mode the guest hands Canvas.drawColor to
// wipe its WebView bitmap. Our Canvas has no pixel store, so the mode is never
// read; it has to be a non-null object of the right class and nothing more.
 klj_val klj_porterduff_clear(void) {
    static void *mode;
    return klj_singleton("android/graphics/PorterDuff$Mode", &mode);
}

 klj_val klj_currentActivity_field(void) {
    return (klj_val){.l = kl_jni_activity()};
}

// Oculus's device-config client. The native half of this
// (libdeviceconfigclient-full-aar.so) is not in the APK — the dlopen for it fails
// early in every run — so there is no configuration service behind this on a real
// Quest either unless the system provides one. Doing nothing is what an absent
// service does; it is a void method, so there is no answer to invent.
static klj_val klj_OculusDeviceConfig_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("OculusDeviceConfig.init() — no device-config service here");
    return (klj_val){.j = 0};
}

// getCurrentState returns a state enum whose numbering is not recoverable from
// the APK (the class lives in an absent AAR) — but the guest's
// OculusDeviceConfigExperimentModel::<Initialize>d__6::MoveNext disassembly
// (2026-08-07) pins the contract: 2 = success (Initialize completes), 3 =
// failure (GetError() -> System.Exception), anything else = poll
// Task.Delay(11) until a 5 s Stopwatch deadline throws TimeoutException and
// il2cpp aborts. In practice BOTH non-success outcomes abort the boot: 0 polls
// to the watchdog, and the state-3 exception propagates out of
// OculusDeviceConfigExperimentModel::.ctor uncaught (constructed on a
// class-init path — il2cpp aborts on a throwing cctor). That leaves 2: the
// service "succeeded" and the experiment flags it gates read as defaults
// (absent param -> false/0), which is the baseline the game ships for a
// device with no config.
static klj_val klj_OculusDeviceConfig_getCurrentState(void *env, void *self,
                                                      const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static int warned;
    if (!warned) {
        warned = 1;
        KLJ_LOG("OculusDeviceConfig.getCurrentState() -> 2 (success; 0 polls to "
                "the 5 s watchdog and 3's exception is uncatchable where .ctor "
                "runs — enum semantics from the MoveNext disasm)");
    }
    return (klj_val){.j = 2};
}

// The state==2 path queries experiment params one by one. The managed flow
// (GetBooleanAsync disasm, 2026-08-07) is: DidPrefetchParamName(name) -> if
// FALSE the game throws KeyNotFoundException itself and the boot aborts; if
// true it calls GetBoolean(name) and takes our word for the value. There is
// no config backend, so every param is "present" with the value false —
// experiment off, the shipping baseline for a device with no config.
static klj_val klj_OculusDeviceConfig_didPrefetchParamName(void *env, void *self,
                                                           const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("OculusDeviceConfig.didPrefetchParamName(\"%s\") -> true (false is "
            "fatal: the game KeyNotFound-aborts on unknown params)",
            n > 0 ? klj_str(a[0].l) : "?");
    return (klj_val){.j = 1};
}
static klj_val klj_OculusDeviceConfig_getBoolean(void *env, void *self,
                                                 const klj_val *a, int n) {
    (void)env; (void)self;
    // (Activity, String) — the activity is unused; the name is a[1].
    KLJ_LOG("OculusDeviceConfig.getBoolean(\"%s\") -> false (no config backend)",
            n > 1 ? klj_str(a[1].l) : "?");
    return (klj_val){.j = 0};
}
// The state==3 path reads one error string out of the service before raising
// its managed "Failed to initialize OculusDeviceConfig." exception. There is
// no service, so the honest string is the reason there is none.
static klj_val klj_OculusDeviceConfig_getError(void *env, void *self,
                                               const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("OculusDeviceConfig.getError() -> \"no device-config service\"");
    return (klj_val){.l = kl_jni_new_string("no device-config service on this device")};
}

// Telephony call-state notifications. There is no telephony here, so registering
// a listener that can never fire is exactly right — Unity only wants to be told
// to duck audio during a call, and no call will ever arrive.
static klj_val klj_UnityPlayer_addPhoneCallListener(void *env, void *self,
                                                    const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// System.nanoTime() — and it must be the *same clock* the Choreographer stamps
// its frameTimeNanos from. The engine subtracts one from the other to get its
// frame delta, so two different monotonic clocks would produce a delta made of
// the offset between them: a garbage frame time that looks like a stall or a
// negative step, with nothing pointing back here.
static klj_val klj_System_nanoTime(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (klj_val){.j = (uint64_t)((int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec)};
}

// Wall clock, unlike nanoTime — this one is allowed to be the real date.
static klj_val klj_System_currentTimeMillis(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (klj_val){.j = (uint64_t)((int64_t)ts.tv_sec * 1000ll + ts.tv_nsec / 1000000)};
}

// Who installed this package. null is Android's answer for "not installed by a
// package installer that recorded itself" — a sideloaded build — which is exactly
// this situation, so it is the true answer rather than a stand-in for one.
static klj_val klj_PackageManager_getInstallerPackageName(void *env, void *self,
                                                          const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};
}

// There is no vibrator. False is the honest answer and the useful one: it is the
// documented way to say so, and it keeps Unity from offering haptics it cannot
// deliver. (Quest controllers have haptics, but those arrive through the XR
// runtime's own API, not android.os.Vibrator.)
static klj_val klj_Vibrator_hasVibrator(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// No proxy is configured, and null is how Android says so — the caller is asking
// "what proxy should I use for this URL", and "none" is a real answer rather than
// a missing one.
static klj_val klj_UnityPlayer_getNetworkProxySettings(void *env, void *self,
                                                       const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};
}

// Class.getName() returns the *dotted* binary name, not the slashed internal one
// we store — "java.lang.String", not "java/lang/String". Unity round-trips this
// through its reflection helpers and back into FindClass, so getting the
// separator wrong would produce a class name that never matches anything, with
// the failure landing wherever the lookup finally happened.
static klj_val klj_Class_getName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    const char *internal = klj_class_name(self);
    if (!internal) return (klj_val){.l = NULL};
    char dotted[256];
    size_t i = 0;
    for (; internal[i] && i < sizeof dotted - 1; i++)
        dotted[i] = internal[i] == '/' ? '.' : internal[i];
    dotted[i] = 0;
    return (klj_val){.l = kl_jni_new_string(dotted)};
}

// Locale.getDefault() and its accessors. Answered as a group for the usual
// reason: the language and country have to describe one locale, and Unity reads
// them separately to build Application.systemLanguage.
//
// The value is taken from the host's LANG rather than hardcoded, because unlike
// Build.MODEL this is a user preference and not device identity — the same
// reasoning that makes the synthetic /proc report the host's real core count.
// The fallback is en/US, which is what an unset LANG means in practice.
static void klj_locale_parts(char *lang, size_t lang_sz, char *country, size_t country_sz) {
    snprintf(lang, lang_sz, "en");
    snprintf(country, country_sz, "US");
    const char *env = getenv("LANG");
    if (!env || !*env || strncmp(env, "C", 2) == 0) return;
    // "en_US.UTF-8" -> "en" + "US"
    char buf[64];
    snprintf(buf, sizeof buf, "%s", env);
    char *dot = strchr(buf, '.');
    if (dot) *dot = 0;
    char *us = strchr(buf, '_');
    if (us) {
        *us = 0;
        snprintf(country, country_sz, "%s", us + 1);
    }
    if (*buf) snprintf(lang, lang_sz, "%s", buf);
}

// ...and the same two strings for the NDK's AConfiguration, which is the OTHER
// door onto this one fact. Exposed rather than duplicated: a guest that reads
// its locale through java.util.Locale and its resource configuration through
// AConfiguration must not be told two different things, and Unreal Engine 4
// reads BOTH — java.util.Locale for FInternationalization and
// AConfiguration_getLanguage out of native_app_glue.
void kl_jni_locale_parts(char *lang, size_t lang_sz, char *country, size_t country_sz) {
    klj_locale_parts(lang, lang_sz, country, country_sz);
}

static klj_val klj_Locale_getDefault(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *locale;
    if (!locale) {
        char lang[16], country[16];
        klj_locale_parts(lang, sizeof lang, country, sizeof country);
        locale = kl_jni_new_object("java/util/Locale");
        // PINNED, because the cache outlives the read. The object is handed
        // out again on every later
        // call, but the guest is entitled to retire the reference it was given:
        // Open Brush calls this from inside a local frame, PopLocalFrame retires
        // it correctly, and every subsequent getDefault() then returns a handle
        // to a slot that has been reused. It surfaced as `GetObjectClass on an
        // untagged pointer` four frames deep in IL2CPP's reflection path —
        // nowhere near a locale, and hundreds of frames after the call that
        // cached it.
        if (locale) klj_as_object(locale)->pinned = 1;
        KLJ_LOG("Locale.getDefault() -> %s_%s (from the host LANG)", lang, country);
    }
    return (klj_val){.l = locale};
}

static klj_val klj_Locale_getLanguage(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    char lang[16], country[16];
    klj_locale_parts(lang, sizeof lang, country, sizeof country);
    return (klj_val){.l = kl_jni_new_string(lang)};
}

static klj_val klj_Locale_getCountry(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    char lang[16], country[16];
    klj_locale_parts(lang, sizeof lang, country, sizeof country);
    return (klj_val){.l = kl_jni_new_string(country)};
}

// BCP 47, which is what Beat Saber 1.40 asks for where 1.28 read getLanguage()
// and getCountry() separately. Built from the same two parts so the three
// cannot disagree — a language tag is a rendering of this Locale, not a second
// opinion about it. Java omits the region subtag entirely when there is none,
// rather than leaving a trailing hyphen.
static klj_val klj_Locale_toLanguageTag(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    char lang[16], country[16], tag[40];
    klj_locale_parts(lang, sizeof lang, country, sizeof country);
    if (*country) snprintf(tag, sizeof tag, "%s-%s", lang, country);
    else          snprintf(tag, sizeof tag, "%s", lang);
    KLJ_LOG("Locale.toLanguageTag() -> %s", tag);
    return (klj_val){.l = kl_jni_new_string(tag)};
}

// null means "this route is not presenting to a secondary display", which is the
// ordinary case and the one Unity is checking for. Documented as nullable, so this
// is the API's own answer rather than a stub standing in for one.
static klj_val klj_RouteInfo_getPresentationDisplay(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};
}

// new String(bytes, charsetName). Our jstrings are NUL-terminated byte buffers
// because we define the representation, so for a UTF-8 charset this is a copy
// with a terminator. Any other charset would need a real transcode, so it is
// reported rather than silently mangled.
static klj_val klj_String_init_bytes(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_array  *arr     = n > 0 && a[0].l ? klj_arr(a[0].l) : NULL;
    const char *charset = n > 1 ? klj_str(a[1].l) : NULL;
    if (charset && strcasecmp(charset, "UTF-8") && strcasecmp(charset, "UTF8") &&
        strcasecmp(charset, "US-ASCII") && strcasecmp(charset, "ISO-8859-1"))
        KLJ_LOG("new String(byte[], \"%s\") — treated as UTF-8, not transcoded", charset);
    if (!arr || arr->kind != 'B') return (klj_val){.l = kl_jni_new_string("")};
    char *buf = malloc((size_t)arr->len + 1);
    memcpy(buf, arr->data, (size_t)arr->len);
    buf[arr->len] = '\0';
    void *s = kl_jni_new_string(buf);
    free(buf);
    return (klj_val){.l = s};
}

// s.getBytes(charsetName) — the inverse of the constructor above, and the same
// deal: our jstrings ARE NUL-terminated UTF-8 byte buffers, so this is a copy
// WITHOUT the terminator (a Java byte[] carries no NUL, and one that did would
// show up as a trailing garbage character in whatever the guest builds from it).
// A non-UTF-8 charset is reported for the same reason as there — silently
// handing back the wrong encoding is a silent zero in string form.
static klj_val klj_String_getBytes(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    const char *s       = klj_str(self);
    const char *charset = n > 0 ? klj_str(a[0].l) : NULL;
    if (charset && strcasecmp(charset, "UTF-8") && strcasecmp(charset, "UTF8") &&
        strcasecmp(charset, "US-ASCII") && strcasecmp(charset, "ISO-8859-1"))
        KLJ_LOG("String.getBytes(\"%s\") — returning UTF-8, not transcoded", charset);
    size_t len = s ? strlen(s) : 0;
    void  *obj = klj_new_array('B', NULL, (int)len);
    if (len) memcpy(klj_arr(obj)->data, s, len);
    return (klj_val){.l = obj};
}

// Uri.encode(s): percent-encode everything outside the unreserved set. The set
// is the Android API's, not a guess — it keeps the RFC 3986 unreserved
// characters plus the sub-delims Uri leaves alone, and a space becomes %20
// rather than '+', which is the difference from form encoding.
static klj_val klj_Uri_encode(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *s = n > 0 ? klj_str(a[0].l) : NULL;
    if (!s) return (klj_val){.l = NULL};
    static const char *keep = "_-!.~'()*";
    size_t len = strlen(s);
    char  *out = malloc(len * 3 + 1);
    size_t w   = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || strchr(keep, c)) out[w++] = (char)c;
        else w += (size_t)sprintf(out + w, "%%%02X", c);
    }
    out[w] = '\0';
    void *r = kl_jni_new_string(out);
    free(out);
    return (klj_val){.l = r};
}

// ---- AlertDialog ----
// This is a measurement instrument before it is a UI shim. Unity only reaches
// for a dialog when it has something to say, and the title and message it sets
// are the engine's own diagnosis — far more useful logged than drawn. Nothing
// here renders anything; the builder records and reports.
// ...and then BONELAB raised one that has to be ANSWERED. Unity's
// `gles-api-check` warning ("Your device does not match the hardware
// requirements of this application.") offers Continue and Abort, and blocks the
// engine until one of them is pressed — so a builder that only records leaves
// the guest waiting forever on a dialog nobody can see.
//
// We press CONTINUE, and that is a decision rather than a default: the question
// is "run this application on hardware it does not recognise?", which is the
// question the person launching Klepton has already answered by launching it.
// Nothing is fabricated about the hardware — the warning is logged in full,
// including which button was pressed, so a run that then fails downstream says
// plainly that it was allowed to.
//
// It is the POSITIVE button specifically, because that is where Android puts
// "proceed" and where Unity put Continue. A dialog with no positive listener is
// left alone: pressing something we were not offered would be inventing an
// answer, which is the thing this file does not do.
typedef struct {
    char *title, *message;
    char *positive, *negative;
    void *on_positive;          // the guest's OnClickListener proxy, pinned
} klj_dialog;

static klj_dialog *klj_dlg(void *self) {
    klj_object *o = klj_as_object(self);
    return o ? o->data : NULL;
}
static klj_val klj_AlertBuilder_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    void *obj = kl_jni_new_object(klj_class_name(clazz));
    klj_as_object(obj)->data = calloc(1, sizeof(klj_dialog));
    KLJ_LOG("AlertDialog.Builder: the guest is raising a dialog");
    return (klj_val){.l = obj};
}
// Every Builder setter returns `this`; that is what the guest's chained calls
// depend on.
#define KLJ_ALERT_SET(Sfx, field)                                                 \
    static klj_val klj_AlertBuilder_set##Sfx(void *env, void *self,               \
                                             const klj_val *a, int n) {           \
        (void)env;                                                                \
        klj_dialog *d = klj_dlg(self);                                            \
        const char *s = n > 0 ? klj_str(a[0].l) : NULL;                           \
        if (d) { free(d->field); d->field = s ? strdup(s) : NULL; }               \
        KLJ_LOG("AlertDialog." #field ": %s", s ? s : "(null)");                  \
        return (klj_val){.l = self};                                              \
    }
KLJ_ALERT_SET(Title,   title)
KLJ_ALERT_SET(Message, message)
#undef KLJ_ALERT_SET

// setPositiveButton(label, listener) / setNegativeButton(...). The listener is
// a JNIBridge proxy and is PINNED: the guest builds it inside a local frame,
// and the press happens at show(), which is later.
static klj_val klj_AlertBuilder_button(void *self, const klj_val *a, int n,
                                       int positive) {
    klj_dialog *d = klj_dlg(self);
    const char *label = n > 0 ? klj_str(a[0].l) : NULL;
    void       *l     = n > 1 ? a[1].l : NULL;
    if (d) {
        char **slot = positive ? &d->positive : &d->negative;
        free(*slot);
        *slot = label ? strdup(label) : NULL;
        if (positive) {
            d->on_positive = l;
            klj_object *o = klj_as_object(l);
            if (o) o->pinned++;
        }
    }
    KLJ_LOG("AlertDialog.%s button: \"%s\"%s", positive ? "positive" : "negative",
            label ? label : "(null)", l ? "" : " (no listener)");
    return (klj_val){.l = self};
}
static klj_val klj_AlertBuilder_setPositive(void *env, void *self, const klj_val *a, int n) {
    (void)env; return klj_AlertBuilder_button(self, a, n, 1);
}
static klj_val klj_AlertBuilder_setNegative(void *env, void *self, const klj_val *a, int n) {
    (void)env; return klj_AlertBuilder_button(self, a, n, 0);
}
// The chainable no-ops: they configure how a dialog we do not draw would look.
static klj_val klj_AlertBuilder_self(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    return (klj_val){.l = self};
}
// create() hands back the dialog, which for us is the same object: the Builder
// already holds everything show() needs, and two objects would be two states.
static klj_val klj_AlertBuilder_create(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    return (klj_val){.l = self};
}

// The dialog's furniture: Unity puts a "do not show this again" CheckBox in the
// warning through Builder.setView(). Nothing draws it and nobody can tick it, so
// isChecked() is FALSE — the suppression is a choice the user never made, and
// answering true would silently hide the next run's warning too.
static klj_val klj_View_new(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_object(klj_class_name(clazz))};
}
static klj_val klj_View_void(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};
}
static klj_val klj_CheckBox_isChecked(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// DialogInterface.BUTTON_POSITIVE. Android's button ids are negative and this
// one is -1, which the listener switches on.
#define KLJ_BUTTON_POSITIVE (-1)
static klj_val klj_AlertBuilder_show(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_dialog *d = klj_dlg(self);
    if (!d) return (klj_val){.l = self};
    KLJ_LOG("AlertDialog.show: \"%s\" — %s", d->title ? d->title : "(no title)",
            d->message ? d->message : "(no message)");
    if (!d->on_positive) {
        KLJ_LOG("AlertDialog: nothing to press (no positive listener) — the "
                "guest may be waiting on this");
        return (klj_val){.l = self};
    }
    KLJ_LOG("AlertDialog: pressing \"%s\" — running this guest on this hardware "
            "is the decision already made by launching it",
            d->positive ? d->positive : "the positive button");
    void *args = klj_new_array('L', "java/lang/Object", 2);
    ((void **)klj_arr(args)->data)[0] = self;            // the DialogInterface
    ((void **)klj_arr(args)->data)[1] = klj_box_int(KLJ_BUTTON_POSITIVE);
    klj_proxy_invoke(d->on_positive, "android/content/DialogInterface$OnClickListener",
                     "onClick", "(Landroid/content/DialogInterface;I)V", args);
    return (klj_val){.l = self};
}

// ---- manifest meta-data ----
// PackageItemInfo.metaData is the <meta-data> block from AndroidManifest.xml,
// and it is real configuration rather than boilerplate: Unity reads its splash
// settings from here and the Oculus layer reads focus-awareness and the
// supported-device list. Returning a null Bundle would silently drop all of it,
// so the values are transcribed from the APK's own manifest.
static const klj_kv g_metadata[] = {
    {"unity.splash-mode",                   "0"},
    {"unity.splash-enable",                 "false"},
    {"unity.build-id",                      "7a1e012b-c456-4b79-b3d2-b878d039f91e"},
    {"unityplayer.SkipPermissionsDialog",   "false"},
    {"com.oculus.ossplash",                 "true"},
    {"com.oculus.vr.focusaware",            "true"},
    {"com.oculus.supportedDevices",         "quest|delmar"},
    {"com.samsung.android.vr.application.mode", "vr_only"},
    {NULL, NULL},
};

// A Bundle is a key/value table, and there is now more than one of them: the
// manifest's <meta-data> above, and an activity's launch extras below. So the
// accessors dispatch on `self` — they used to read the manifest table whatever
// object they were called on, which was fine while that was the only Bundle in
// existence and would have silently answered manifest values to a guest asking
// about its launch arguments.
//
// The payload is the table itself, NULL-terminated, borrowed and never freed:
// every one of them is static storage that outlives the process.
static const char *klj_bundle_get(void *self, const char *key) {
    klj_object *o = klj_as_object(self);
    const klj_kv *kv = o ? o->data : NULL;
    if (!kv || !key) return NULL;
    for (int i = 0; kv[i].key; i++)
        if (strcmp(kv[i].key, key) == 0) return kv[i].val;
    return NULL;
}

// ...and the table above is a TRANSCRIPTION, which goes stale on a guest swap
// exactly as the version code did. Beat Saber 1.40 is where that stopped being
// cosmetic: the Oculus XR Plugin reads its own configuration from here through
// OculusUnity.getManifestSetting(), and 1.40 declares three keys
// (com.unity.xr.oculus.LowOverheadMode / LateLatching / LateLatchingDebug) that
// no earlier manifest had, so a stale table answers false to all three — a
// setting silently disabled rather than an error.
//
// apktool has already decoded AndroidManifest.xml to text beside the assets and
// libraries, so read the guest's own <meta-data> instead of describing it here.
// The parse is deliberately narrow: this is apktool's output, not arbitrary XML,
// and every element in it has the shape
// `<meta-data android:name="K" android:value="V"/>`. Anything it cannot read
// falls back to the transcription, and says which it used.
const char *klj_xml_attr(const char *el, const char *end, const char *attr) {
    const char *p = strstr(el, attr);
    return (p && p < end) ? p + strlen(attr) : NULL;
}

static const klj_kv *klj_manifest_metadata(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s/../AndroidManifest.xml", g_assets_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    long size = (fseek(f, 0, SEEK_END), ftell(f));
    char *xml = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (!xml) { fclose(f); return NULL; }
    rewind(f);
    size_t got = fread(xml, 1, (size_t)size, f);
    fclose(f);
    xml[got] = '\0';

    size_t cap = 32, n = 0;
    klj_kv *kv = calloc(cap, sizeof *kv);
    for (const char *p = xml; (p = strstr(p, "<meta-data")) != NULL; ) {
        const char *end = strchr(p, '>');
        if (!end) break;
        const char *k = klj_xml_attr(p, end, "android:name=\"");
        const char *v = klj_xml_attr(p, end, "android:value=\"");
        p = end;
        if (!k || !v) continue;
        const char *ke = strchr(k, '"'), *ve = strchr(v, '"');
        if (!ke || !ve) continue;
        if (n + 1 >= cap) { cap *= 2; kv = realloc(kv, cap * sizeof *kv);
                            memset(kv + n, 0, (cap - n) * sizeof *kv); }
        kv[n].key = strndup(k, (size_t)(ke - k));
        kv[n].val = strndup(v, (size_t)(ve - v));
        n++;
    }
    free(xml);
    if (!n) { free(kv); return NULL; }
    kv[n].key = kv[n].val = NULL;
    KLJ_LOG("manifest <meta-data>: %zu entries from %s", n, path);
    return kv;
}

 klj_val klj_metaData_field(void) {
    static void *bundle;
    if (!bundle) {
        const klj_kv *kv = klj_manifest_metadata();
        if (!kv) {
            kv = g_metadata;
            KLJ_LOG("no readable AndroidManifest.xml beside %s — using the "
                    "transcribed <meta-data>, which may not be this guest's",
                    g_assets_dir);
        }
        bundle = klj_new_object_data("android/os/Bundle", (void *)kv);
    }
    return (klj_val){.l = bundle};
}

// ...and the same table read from C, for a driver that has to stand in for the
// guest's own Java. A UE4 guest never asks for these over JNI: GameActivity.java
// reads the <meta-data> block itself and hands the answers to native setters, so
// whoever acts out that Java needs the values. Deliberately the same lookup the
// guest's own Bundle.getString goes through, so the two cannot disagree.
const char *kl_jni_manifest_meta(const char *key) {
    return klj_bundle_get(klj_metaData_field().l, key);
}

// Bundle accessors. The two-argument forms take a default, which is what Android
// returns for a missing key; the one-argument forms return the type's zero.
static klj_val klj_Bundle_getString(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    const char *v = klj_bundle_get(self, n > 0 ? klj_str(a[0].l) : NULL);
    if (!v && n > 1) return (klj_val){.l = a[1].l};
    return (klj_val){.l = v ? kl_jni_new_string(v) : NULL};
}
static klj_val klj_Bundle_getBoolean(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    const char *v = klj_bundle_get(self, n > 0 ? klj_str(a[0].l) : NULL);
    if (!v) return (klj_val){.j = n > 1 ? a[1].j : 0};
    return (klj_val){.j = strcmp(v, "true") == 0};
}
static klj_val klj_Bundle_getInt(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    const char *v = klj_bundle_get(self, n > 0 ? klj_str(a[0].l) : NULL);
    if (!v) return (klj_val){.j = n > 1 ? a[1].j : 0};
    return (klj_val){.j = (uint64_t)(int64_t)strtol(v, NULL, 10)};
}
static klj_val klj_Bundle_containsKey(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    return (klj_val){.j = klj_bundle_get(self, n > 0 ? klj_str(a[0].l) : NULL) != NULL};
}

// ---------- the guest's version, which NAMES A FILE ----------
//
// versionCode and versionName used to be transcribed constants (545 /
// "1.28.0_4124311467"). That is stale the moment a guest is swapped, and it is
// not a cosmetic staleness: for a SPLIT APPLICATION BINARY build — which Beat
// Saber 1.40 is, marked by assets/unity_obb_guid — Unity builds the asset pack
// path out of the version code, so a wrong number sends it looking for
// `main.545.com.beatgames.beatsaber.obb` when the file it was shipped with is
// `main.1716...`. A missing OBB reads as missing game data, several layers
// from the constant that caused it.
//
// Read from the unpacked tree instead, so a swap describes itself. apktool.yml
// is where the unpacker records both, in plain text, beside the assets and
// libraries this run is already pointed at. Falling back to the constants keeps
// a tree without one working, and says so rather than answering quietly.
void klj_guest_version(long *code, const char **name) {
    static long  cached_code = -1;
    static char  cached_name[64];
    if (cached_code < 0) {
        cached_code = 545;                       // 1.28's, the historical default
        snprintf(cached_name, sizeof cached_name, "1.28.0_4124311467");
        char path[1024];
        // apktool.yml sits beside assets/, one level up from the assets dir.
        snprintf(path, sizeof path, "%s/../apktool.yml", g_assets_dir);
        FILE *f = fopen(path, "r");
        if (f) {
            char line[256];
            long  got_code = -1;
            char  got_name[64] = {0};
            while (fgets(line, sizeof line, f)) {
                char buf[64];
                if (sscanf(line, " versionCode: '%ld'", &got_code) == 1) continue;
                if (sscanf(line, " versionCode: %ld", &got_code) == 1) continue;
                if (sscanf(line, " versionName: %63s", buf) == 1)
                    snprintf(got_name, sizeof got_name, "%s", buf);
            }
            fclose(f);
            if (got_code > 0) cached_code = got_code;
            if (*got_name) snprintf(cached_name, sizeof cached_name, "%s", got_name);
            KLJ_LOG("guest version %ld / %s (from %s)", cached_code, cached_name, path);
        } else {
            KLJ_LOG("no apktool.yml beside %s — falling back to versionCode %ld / %s. "
                    "A split-binary guest builds its OBB NAME from this number.",
                    g_assets_dir, cached_code, cached_name);
        }
    }
    if (code) *code = cached_code;
    if (name) *name = cached_name;
}

// ...and the same pair, for anything outside this file that has to describe the
// application to the guest. kl_ovrplat answers ovr_Application_GetVersion out of
// it, so the version the platform reports and the version PackageManager reports
// are one number rather than two that can disagree.
void kl_jni_guest_version(long *code, const char **name) {
    klj_guest_version(code, name);
}

// ---------- ...and the package name, which NAMES THE SAME FILE ----------
//
// `com.beatgames.beatsaber` was a constant here for the whole project, and it is
// the version code's problem wearing a different hat: an OBB is
// `main.<versionCode>.<package>.obb`, so the package name is half that filename,
// and it is also how the guest looks ITSELF up through the PackageManager and
// derives its storage paths. A second Unity title inherits the wrong one
// silently — SUPERHOT VR is `unity.SUPERHOT_Team.SUPERHOT_VR_QA`, and a guest
// told it is Beat Saber is a guest asking the platform about a different
// application.
//
// So it is read from the guest's own AndroidManifest.xml, which apktool has
// already decoded to text beside the assets and which is staged on every device
// run for exactly this reason. The parse is the same deliberately-narrow shape
// as klj_manifest_metadata's: the `package="..."` attribute of the root
// <manifest> element.
 const char *klj_guest_package(void) {
    static char cached[192];
    if (!*cached) {
        snprintf(cached, sizeof cached, "com.beatgames.beatsaber");  // historical
        char path[1024];
        snprintf(path, sizeof path, "%s/../AndroidManifest.xml", g_assets_dir);
        FILE *f = fopen(path, "rb");
        if (f) {
            char head[4096];
            size_t got = fread(head, 1, sizeof head - 1, f);
            fclose(f);
            head[got] = '\0';
            const char *m = strstr(head, "<manifest");
            const char *end = m ? strchr(m, '>') : NULL;
            const char *p = m ? klj_xml_attr(m, end ? end : head + got, "package=\"") : NULL;
            const char *pe = p ? strchr(p, '"') : NULL;
            if (p && pe && pe > p && (size_t)(pe - p) < sizeof cached) {
                snprintf(cached, sizeof cached, "%.*s", (int)(pe - p), p);
                KLJ_LOG("guest package %s (from %s)", cached, path);
            } else {
                KLJ_LOG("no package= in %s — falling back to %s", path, cached);
            }
        } else {
            KLJ_LOG("no AndroidManifest.xml beside %s — falling back to package %s. "
                    "A split-binary guest builds its OBB NAME from this.",
                    g_assets_dir, cached);
        }
    }
    return cached;
}

const char *kl_jni_guest_package(void) { return klj_guest_package(); }

 klj_val klj_PackageInfo_packageName(void) {
    return (klj_val){.l = kl_jni_new_string(klj_guest_package())};
}

 klj_val klj_PackageInfo_versionCode(void) {
    long code; klj_guest_version(&code, NULL);
    return (klj_val){.j = (uint64_t)(int64_t)code};
}
// API 28's replacement for the int field, and the same number: getLongVersionCode
// is defined as (versionCodeMajor << 32) | versionCode, and nothing here has a
// major — apktool.yml carries one versionCode and the manifest declares no
// android:versionCodeMajor. Answering anything else would give a guest two
// different versions of itself depending on which accessor it used.
static klj_val klj_PackageInfo_getLongVersionCode(void *env, void *self,
                                                  const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return klj_PackageInfo_versionCode();
}

 klj_val klj_PackageInfo_versionName(void) {
    const char *name; klj_guest_version(NULL, &name);
    return (klj_val){.l = kl_jni_new_string(name)};
}

// PackageInfo, like ApplicationInfo, is read field-by-field.
static klj_val klj_PM_getPackageInfo(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *pi;
    return klj_singleton("android/content/pm/PackageInfo", &pi);
}

static klj_val klj_Context_getApplicationInfo(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *ai;
    return klj_singleton("android/content/pm/ApplicationInfo", &ai);
}

void klj_mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

// java/io/File carries its path as the payload; that is all anything reads.
 void *klj_new_file(const char *path) {
    klj_mkdir_p(path);
    void *obj = kl_jni_new_object("java/io/File");
    klj_as_object(obj)->data = strdup(path);
    return obj;
}
static klj_val klj_File_getPath(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    return (klj_val){.l = kl_jni_new_string(o && o->data ? o->data : "")};
}
// java.io.File.getParent() returns null, not "", when there is no parent.
static klj_val klj_File_getParent(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o    = klj_as_object(self);
    const char *path = o ? o->data : NULL;
    const char *cut  = path ? strrchr(path, '/') : NULL;
    if (!cut) return (klj_val){.l = NULL};
    char parent[1024];
    size_t len = (size_t)(cut - path);
    if (len == 0) len = 1;                       // "/x" -> "/"
    if (len >= sizeof parent) len = sizeof parent - 1;
    memcpy(parent, path, len);
    parent[len] = '\0';
    return (klj_val){.l = kl_jni_new_string(parent)};
}

// getExternalFilesDir(type): type selects a subdirectory ("music", "pictures"),
// and null means the root — which is what Unity passes.
static klj_val klj_Context_getExternalFilesDir(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *type = n > 0 && a[0].l ? klj_str(a[0].l) : NULL;
    char path[1024];
    if (type && *type) snprintf(path, sizeof path, "%s/files/%s", kl_jni_files_dir(), type);
    else               snprintf(path, sizeof path, "%s/files", kl_jni_files_dir());
    KLJ_LOG("getExternalFilesDir(%s) -> %s", type ? type : "null", path);
    return (klj_val){.l = klj_new_file(path)};
}
static klj_val klj_Context_getFilesDir(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    char path[1024];
    snprintf(path, sizeof path, "%s/files", kl_jni_files_dir());
    return (klj_val){.l = klj_new_file(path)};
}
static klj_val klj_Context_getCacheDir(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    char path[1024];
    snprintf(path, sizeof path, "%s/cache", kl_jni_files_dir());
    klj_mkdir_p(path);
    return (klj_val){.l = klj_new_file(path)};
}
// Android's two cache directories differ in which volume they are on and in
// nothing else that matters here: one storage, one answer. It is CREATED for
// the same reason the OBB directory is — the caller is asking where to put a
// file, and a path that does not exist is a write that fails much later.
// BONELAB's Vulkan plugin keeps its pipeline-state cache here.
static klj_val klj_Context_getExternalCacheDir(void *env, void *self, const klj_val *a, int n) {
    return klj_Context_getCacheDir(env, self, a, n);
}

// ---- android.app.ActivityThread ----
// The back door to a Context, for native code that was never handed one:
// ActivityThread.currentActivityThread().getApplication(). It is @hide, and
// every Unity-era native plugin that wants a Context without an Activity uses
// it anyway. Both are singletons here, and the Application IS the application
// Context object — the same one getApplicationContext() answers with, so a
// plugin and the engine cannot end up with two different ideas of it.
static klj_val klj_ActivityThread_current(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)clazz; (void)a; (void)n;
    static void *at;
    return klj_singleton("android/app/ActivityThread", &at);
}
static klj_val klj_ActivityThread_getApplication(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *app;
    return klj_singleton("android/app/Application", &app);
}

// Our storage is a plain writable directory, so "mounted" is the honest state.
// Any other value sends Unity down a read-only or unavailable-storage path.
// ---- Unreal Engine 4's GameActivity ----
//
// UE4's Java side is one big class of `AndroidThunkJava_*` methods that the
// engine resolves in a block during JNI_OnLoad and then calls as it needs them
// (78 method ids on RE4 before the first call). They are transcribed from the
// APK's own GameActivity.smali, like every other binding here.
//
// The font directory is the first one any UE4 title reaches, because Slate
// wants a typeface before it has a window. The guest's own implementation is a
// three-entry search — /system/fonts, /system/font, /data/fonts — returning the
// first that File.exists(), and then appending "/" to whatever it found. Note
// what it does when it finds NOTHING: `v3` stays null, StringBuilder.append
// spells that "null", and the method returns the literal string "null/". That
// is not a case worth reproducing; it is what a device without fonts would say,
// and we are presenting a Quest, which has /system/fonts like any other
// Android device.
//
// So the answer is the Quest's, and the path is MAPPED rather than invented —
// kl_guest_path already redirects the guest's absolute paths, and a font
// directory that answers a name and then has no files in it is the shape of
// a read that silently finds nothing, where the guest is entitled to assume the
// directory it was handed exists. Whether the engine actually loads
// from here is the next run's question; until it does, this is a name.
static klj_val klj_UE4_GetFontDirectory(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("/system/fonts/")};
}

static klj_val klj_Environment_getExternalStorageState(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("mounted")};
}
static klj_val klj_Environment_getExternalStorageDirectory(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_file(kl_jni_files_dir())};
}

// MANAGE_EXTERNAL_STORAGE — "does this app hold unrestricted access to shared
// storage?" — and the answer here is yes, because there is nothing to restrict.
// The permission exists on Android to separate one app's files in shared
// storage from every OTHER app's. There are no other apps in this process, and
// the whole of the guest's external storage is the one directory we hand it
// (kl_guest_extstorage_map, from getExternalStorageDirectory above), which it
// may read and write in full. So the permission's purpose is satisfied
// vacuously rather than granted generously.
//
// It is NOT a free answer, and Open Brush is why. LoadingScene.cs Start():
//
//     if (!UserHasManageExternalStoragePermission()) {
//         m_WaitingForPermission = true;
//         AskForManageStoragePermission();
//         while (!UserHasManageExternalStoragePermission())
//             yield return new WaitForEndOfFrame();
//     }
//
// That loop is BEFORE `SceneManager.LoadSceneAsync("Main")`, so answering false
// parks the app forever on its GvrOverlay — a uniform grey field, every counter
// healthy, stereo layers submitted every frame, nothing in any log. It is the
// exact shape of a graphics failure and it is a permission.
//
// False would also cost more than it saved: the only way OUT of that loop
// without this call is for AskForManageStoragePermission() to THROW, which is
// how the guest recognises a device with no all-files-access settings screen
// (`m_FolderPermissionOverride = true`). Serving that arm means an
// ActivityNotFoundException we cannot raise — our JNI has no exception state
// (klj_ExceptionCheck is 0 forever) — plus Uri.fromParts, an
// Intent(String, Uri) constructor, addCategory and startActivity, i.e. four
// fabricated answers about a Settings activity that does not exist here.
// Answering the question we CAN answer truthfully forces none of them.
//
// The honest tension, recorded rather than hidden: we present SDK_INT = 29 and
// this method arrived in API 30, so a real device answering our own Build
// fields would refuse the call rather than answer it. A guest that gates on
// `SDK_INT >= 30` therefore never reaches this handler at all and gets legacy
// external storage — which the /sdcard mapping now serves — so the two stay
// consistent for the common shape. Open Brush is the other shape: it simply
// tries the call and catches the failure, and for it there is no version to be
// consistent with, only a directory that is or is not writable.
static klj_val klj_Environment_isExternalStorageManager(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("Environment.isExternalStorageManager() -> true "
            "(external storage here is one directory the guest owns outright)");
    return (klj_val){.j = 1};
}

// There is no ARCore here and there never will be — Vision Pro's world sensing
// arrives through ARKit under our own ovrp_* layer, not through Google AR.
// False is the truthful answer, and Unity has a supported no-AR path.
// libOculusXRPlugin's GetIsSupportedDevice() upcall, and the gate Beat Saber
// 1.40 fails without: the Oculus XR Plugin loader refuses to initialize on
// what it decides is "a non-Oculus device" and calls Application.Quit().
//
// Not invented — this is the guest's own OculusUnity.getIsOnOculusHardware(),
// transcribed: `Build.MANUFACTURER.toLowerCase(Locale.ENGLISH).contains("oculus")`.
// Build.MANUFACTURER is answered "Oculus" (a settled decision — reporting Apple
// hardware fails every Oculus branch in the guest), so running the guest's test
// against that answer gives true, and answering
// true here is the same statement made directly rather than through three
// String methods. Read back through kl_jni_build_string for exactly that
// reason: that is the single source Build.MANUFACTURER and
// ro.product.manufacturer already answer from, so this cannot drift away from
// the field it is supposed to be reading — and if the presented manufacturer is
// ever changed, this answers false, which is then correct rather than stale.
static klj_val klj_OculusUnity_isOnOculusHardware(void *env, void *self,
                                                  const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    const char *mfr = kl_jni_build_string("MANUFACTURER");
    int oculus = mfr && strcasestr(mfr, "oculus") != NULL;
    KLJ_LOG("OculusUnity.getIsOnOculusHardware() -> %s (Build.MANUFACTURER = \"%s\")",
            oculus ? "true" : "false", mfr ? mfr : "(unset)");
    return (klj_val){.j = (uint64_t)oculus};
}

// OculusUnity.loadLibrary(name) — a logged wrapper around System.loadLibrary
// (OculusUnity.smali:69: Log.d then System.loadLibrary). Delegated to
// UnityPlayer.loadLibrary, which is the same wrapper for the other plugin, so
// every library the guest loads through Java goes through one path and gets its
// JNI_OnLoad called. Its own return is void; the boolean is discarded here
// exactly as the guest's `invoke-static` discards it.

static klj_val klj_OculusUnity_loadLibrary(void *env, void *self, const klj_val *a, int n) {
    klj_UnityPlayer_loadLibrary(env, self, a, n);
    return (klj_val){.l = NULL};
}

// OculusUnity.getManifestSetting(key) and the three named wrappers over it.
// Transcribed from OculusUnity.smali:78-85 — it reads
// ApplicationInfo.metaData.getBoolean(key), which is the same Bundle
// klj_metaData_field serves, now parsed from the guest's own manifest. So these
// answer whatever the APK declares rather than a policy of ours, and the four
// entry points cannot disagree because there is one lookup behind them.
//
// Android's Bundle.getBoolean returns false for a missing key, which is also
// what the guest's own catch block returns, so an absent key needs no special
// case.
static const char *klj_bundle_get(void *self, const char *key);

static int klj_oculus_manifest_flag(const char *key) {
    const char *v = klj_bundle_get(klj_metaData_field().l, key);
    int on = v && (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
    KLJ_LOG("OculusUnity.getManifestSetting(\"%s\") -> %s%s", key,
            on ? "true" : "false", v ? "" : " (not declared)");
    return on;
}

static klj_val klj_OculusUnity_manifestSetting(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    return (klj_val){.j = (uint64_t)klj_oculus_manifest_flag(n > 0 ? klj_str(a[0].l) : "")};
}

#define KLJ_OCULUS_FLAG(fn, key)                                                  \
    static klj_val fn(void *env, void *self, const klj_val *a, int n) {           \
        (void)env; (void)self; (void)a; (void)n;                                  \
        return (klj_val){.j = (uint64_t)klj_oculus_manifest_flag(key)};           \
    }
KLJ_OCULUS_FLAG(klj_OculusUnity_lowOverhead,      "com.unity.xr.oculus.LowOverheadMode")
KLJ_OCULUS_FLAG(klj_OculusUnity_lateLatching,     "com.unity.xr.oculus.LateLatching")
KLJ_OCULUS_FLAG(klj_OculusUnity_lateLatchingDebug,"com.unity.xr.oculus.LateLatchingDebug")
#undef KLJ_OCULUS_FLAG

// The private native OculusUnity.surfaceCreated(Surface). OculusUnity.smali
// declares it `private native` but libOculusXRPlugin's JNI_OnLoad never calls
// RegisterNatives for it — it is reached by Java_ symbol resolution on real
// Android (the library exports Java_com_unity_oculus_OculusUnity_surfaceCreated,
// 0x10b7c). We run the REAL libOculusXRPlugin, so the implementation exists and
// is the honest thing to call: it takes a global ref to the Surface and hands it
// to the plugin's internal surface-notify, which is the one effect the whole
// UnitySurfaceView dance exists to produce. Resolved by symbol rather than bound
// in jni land, because no guest ever registered it and the JVM's Java_ name
// resolution is exactly the mechanism Android would use.
static void *klj_oculus_surface_created_native(void) {
    static void *fn;
    if (!fn) {
        kl_image *img = kl_find_image("libOculusXRPlugin.so");
        if (img)
            fn = kl_sym(img, "Java_com_unity_oculus_OculusUnity_surfaceCreated");
        if (!fn)
            fprintf(stderr, "  [jni] no Java_com_unity_oculus_OculusUnity_surfaceCreated "
                            "export in libOculusXRPlugin.so — surface setup skipped\n");
    }
    return fn;
}

// OculusUnity.initOculus() — transcribed from OculusUnity.smali:22-39. The real
// body logs, stashes the Activity, then posts a lambda to the UI thread that
// finds the unitySurfaceView (res id 0x7f020000, present in this APK's
// res/values/ids.xml) and calls the private native surfaceCreated(Surface).
//
// Executed INLINE rather than through the posted Runnable: nothing downstream
// reads glView/activity back, and the only observable effect is the native
// surfaceCreated running, so deferring it to a UI-thread drain would add a
// dependency on the host pump draining at the right moment for zero observable
// difference. If a later abort names SurfaceView/SurfaceHolder methods, THAT is
// the signal the singular surface fidelity matters and the object model exists
// then. The Surface handed over is a valid jobject; the native only NewGlobalRefs
// it and never dereferences it.
static klj_val klj_OculusUnity_initOculus(void *env, void *self, const klj_val *a, int n) {
    (void)a; (void)n;
    KLJ_LOG("OculusUnity.initOculus() — activity %s, surface dance inline",
            (klj_as_object(kl_jni_activity())->cls));
    void *surface = kl_jni_new_object("android/view/Surface");
    void *fn = klj_oculus_surface_created_native();
    if (fn)
        ((void (*)(void *, void *, void *))fn)(env, self, surface);
    return (klj_val){.j = 0};
}

// pauseOculus/resumeOculus are empty bodies and destroyOculus only removes a
// SurfaceHolder callback that our inline dance never installed (smali:62-66) —
// the recorded-not-fitted shape, exactly like every other lifecycle no-op here.
static klj_val klj_OculusUnity_lifecycle(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

static klj_val klj_UnityPlayer_initializeGoogleAr(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// Google Play Asset Delivery — Beat Saber 1.40's first stop after initJni, and
// the reason its assets load differently from 1.28's. Unity 2021+ always
// compiles the wrapper in and asks for it at startup.
//
// The absence is reported through playCoreApiMissing(), NOT by returning NULL
// from init(), and this is read off the guest's own smali rather than guessed:
// init() constructs the singleton unconditionally and its only other exit is a
// RuntimeException for being called twice, so NULL is not in its range at all.
// Answering NULL got exactly the failure that implies — Unity does not test it,
// and called GetObjectClass on 0 one line later.
//
// playCoreApiMissing() is the real question, and it is `this.a == null`, i.e.
// "the Play Core AssetPackManager did not construct". True is the truthful
// answer: Play Asset Delivery is the Play Store's delivery channel, this is a
// Meta Store build, and there is no Play Store on a Quest OR on a Vision Pro.
// The same "the platform is genuinely absent" story kl_ovrplat.c tells for the
// Oculus Platform and bindService tells for Play Services — and every Quest
// takes this branch on real hardware, so it is a path Unity supports.
//
// What it falls through to is the split application binary: assets/unity_obb_guid
// in the APK marks the build as one, so Unity looks for the OBB instead.
static klj_val klj_PlayAssetDelivery_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *singleton;      // init() is a singleton factory; getInstance()
    if (!singleton)              // hands back the same object, so identity matters
        singleton = klj_new_object_data("com/unity3d/player/PlayAssetDeliveryUnityWrapper", NULL);
    KLJ_LOG("PlayAssetDeliveryUnityWrapper.init() -> wrapper "
            "(playCoreApiMissing will answer true: no Play Store here)");
    return (klj_val){.l = singleton};
}

static klj_val klj_PlayAssetDelivery_getInstance(void *env, void *self, const klj_val *a, int n) {
    return klj_PlayAssetDelivery_init(env, self, a, n);
}

static klj_val klj_PlayAssetDelivery_missing(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 1};        // Play Core is absent — see above
}

static klj_val klj_Context_getAssets(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *am;
    return klj_singleton("android/content/res/AssetManager", &am);
}

// InputStream carries the whole asset in memory: assets read this way are small
// config blobs, and it makes Scanner a pure string walk.
//
// ...which stopped being the whole story when a guest opened its OBB through
// java.util.zip. That file is 3.4 GB, so a stream over an entry in it is a
// WINDOW into a file rather than a buffer, and the two kinds are distinguished
// by which of `data` and `f` is set. Nothing chooses between them at the read
// site: klj_stream_read serves both.

 size_t klj_stream_read(klj_stream *st, void *buf, size_t want) {
    if (!st || st->pos >= st->len) return 0;
    size_t take = st->len - st->pos;
    if (take > want) take = want;
    if (st->data) {
        memcpy(buf, st->data + st->pos, take);
    } else if (st->f) {
        // Seek every time rather than trusting the handle's position: a stream
        // is the only thing that knows where it is, and two of them over one
        // file would otherwise read each other's bytes.
        if (fseeko(st->f, (off_t)(st->base + st->pos), SEEK_SET) != 0) return 0;
        take = fread(buf, 1, take, st->f);
    } else {
        return 0;
    }
    st->pos += take;
    return take;
}

// Scanner walks the bytes directly, so a window into a file has to become bytes
// first. Nothing takes this path today — Scanner is the assets path and those
// are slurped — and it is here so that a stream from a zip cannot silently scan
// as empty, which is a wrong answer with no error anywhere.
static const char *klj_stream_bytes(klj_stream *st) {
    if (!st) return NULL;
    if (!st->data && st->f) {
        char *d = malloc(st->len + 1);
        if (!d) return NULL;
        size_t got = 0;
        if (fseeko(st->f, (off_t)st->base, SEEK_SET) == 0)
            got = fread(d, 1, st->len, st->f);
        d[got] = '\0';
        st->data = d;
        st->len  = got;
    }
    return st->data;
}

 void klj_stream_close(klj_stream *st) {
    if (!st) return;
    if (st->f) { fclose(st->f); st->f = NULL; }
}

// A stream owns everything in it: `data` is its own slurp or its own inflate,
// and `f` is its own handle — a window stream reopens the archive precisely so
// two streams over one file cannot disturb each other's position. So the whole
// record dies with the object, and a leaked one was a whole file's contents.
 void klj_stream_free(void *p) {
    klj_stream *st = p;
    if (!st) return;
    klj_stream_close(st);
    free(st->data);
    free(st);
}

static klj_val klj_AssetManager_open(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *rel = n > 0 ? klj_str(a[0].l) : NULL;
    if (!rel) return (klj_val){.l = NULL};

    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_assets_dir, rel);
    FILE *f = fopen(path, "rb");
    KLJ_LOG("AssetManager.open(\"%s\") -> %s", rel, f ? path : "MISSING");
    if (!f) return (klj_val){.l = NULL};   // guest catches IOException

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    klj_stream *st = calloc(1, sizeof *st);
    st->data = malloc((size_t)sz + 1);
    st->len  = fread(st->data, 1, (size_t)sz, f);
    st->data[st->len] = '\0';
    fclose(f);

    void *obj = kl_jni_new_object("java/io/InputStream");
    klj_as_object(obj)->data = st;
    klj_own(obj, klj_stream_free);
    return (klj_val){.l = obj};
}

// Scanner, only as far as Unity uses it: wrap a stream, set a delimiter, pull
// tokens. Java's delimiter is a *regex*, and we do not have one — so we handle
// the input-anchor idioms exactly and treat anything else as a literal, warning
// when that is a guess. Unity slurps boot.config with "\z"; if we simply
// searched for that as a literal it would not be found and we would return the
// whole input anyway — the right answer for the wrong reason. Naming the case
// keeps the next delimiter from splitting silently in the wrong place.
static int klj_delim_whole_input(const char *d) {
    return strcmp(d, "\\A") == 0 || strcmp(d, "\\z") == 0 || strcmp(d, "\\Z") == 0;
}
typedef struct { klj_stream *st; char delim[32]; } klj_scanner;

static klj_val klj_Scanner_init(void *env, void *clazz, const klj_val *a, int n) {
    (void)env; (void)clazz;
    klj_object *in = n > 0 ? klj_as_object(a[0].l) : NULL;
    klj_scanner *sc = calloc(1, sizeof *sc);
    sc->st = in ? in->data : NULL;
    // A Scanner holds a RAW pointer into the stream's payload, so the stream
    // must outlive it — and streams release their payload on recycle now. Pin
    // the input rather than copy or reference-count it: the alternative is a
    // use-after-free that only appears once the pool has turned over 8192
    // slots. The cost is one unreclaimable slot per Scanner, and Unity makes
    // one, for boot.config.
    if (in) in->pinned = 1;
    snprintf(sc->delim, sizeof sc->delim, "%s", "\n");
    void *obj = kl_jni_new_object("java/util/Scanner");
    klj_as_object(obj)->data = sc;
    return (klj_val){.l = klj_own(obj, free)};
}
static klj_val klj_Scanner_useDelimiter(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_object  *o  = klj_as_object(self);
    klj_scanner *sc = o ? o->data : NULL;
    const char  *d  = n > 0 ? klj_str(a[0].l) : NULL;
    if (sc && d) snprintf(sc->delim, sizeof sc->delim, "%s", d);
    KLJ_LOG("Scanner.useDelimiter(\"%s\")%s", d ? d : "(null)",
            d && !klj_delim_whole_input(d) && strpbrk(d, "\\[](){}*+?|^$.")
                ? "  WARNING: regex delimiter treated as a literal" : "");
    return (klj_val){.l = self};
}
static klj_val klj_Scanner_next(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object  *o  = klj_as_object(self);
    klj_scanner *sc = o ? o->data : NULL;
    if (!sc || !sc->st || sc->st->pos >= sc->st->len)
        return (klj_val){.l = NULL};   // guest catches NoSuchElementException

    klj_stream *st   = sc->st;
    if (!klj_stream_bytes(st)) return (klj_val){.l = NULL};
    const char *rest = st->data + st->pos;
    const char *end  = klj_delim_whole_input(sc->delim) ? NULL : strstr(rest, sc->delim);
    size_t take = end ? (size_t)(end - rest) : st->len - st->pos;

    char *tok = malloc(take + 1);
    memcpy(tok, rest, take);
    tok[take] = '\0';
    st->pos += take + (end ? strlen(sc->delim) : 0);

    KLJ_LOG("Scanner.next() -> %zu bytes", take);
    void *s = kl_jni_new_string(tok);
    free(tok);
    return (klj_val){.l = s};
}

const klj_binding klj_bind_services[] = {
    {"org/fmod/FmodAndroidAudioManager", "getInstance",
     "()Lorg/fmod/FmodAndroidAudioManager;",        klj_fmod_audio_manager},
    {"org/fmod/FmodAndroidAudioManager", "getBluetoothInputDeviceId", "()I",
                                                    klj_fmod_bt_input_id},
    {"org/fmod/FmodAndroidAudioManager", "isBluetoothInputDeviceAvailable", "()Z",
                                                    klj_fmod_false},
    {"org/fmod/FmodAndroidAudioManager", "isBluetoothScoConnected",   "()Z", klj_fmod_false},
    {"org/fmod/FmodAndroidAudioManager", "isBuiltinInputDeviceAvailable", "()Z",
                                                    klj_fmod_false},
    {"org/fmod/FmodAndroidAudioManager", "isBuiltinSpeakerDevice",    "(I)Z", klj_fmod_false},
    {"org/fmod/FmodAndroidAudioManager", "isInputSampleRateAvailable","(I)Z", klj_fmod_false},
    {"org/fmod/FmodAndroidAudioManager", "setActivity",
     "(Landroid/app/Activity;)V",                   klj_fmod_void},
    {"org/fmod/FmodAndroidAudioManager", "setUseBluetooth",   "(Z)V", klj_fmod_void},
    {"org/fmod/FmodAndroidAudioManager", "startBluetoothSco", "()V",  klj_fmod_void},
    {"org/fmod/FmodAndroidAudioManager", "stopBluetoothSco",  "()V",  klj_fmod_void},
    {"android/content/pm/PackageInfo", "getLongVersionCode", "()J",
     klj_PackageInfo_getLongVersionCode},
    {"com/unity3d/player/UnityPlayer", "getNetworkConnectivity", "()I",
     klj_UnityPlayer_getNetworkConnectivity},
    {"io/sentry/Sentry", "close", "()V", klj_Sentry_close},
    {"com/unity3d/player/JavaBroadcastReceiver", "setReceiver",
     "(Landroid/content/Context;Lcom/unity3d/player/JavaBroadcastReceiverInterface;"
     "[Ljava/lang/String;)V", klj_JavaBroadcastReceiver_setReceiver},
    {"com/unity3d/player/JavaBroadcastReceiver", "updateDigest",
     "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)V",
     klj_JavaBroadcastReceiver_updateDigest},
    {"com/unity3d/player/HFPStatus", "clearHFPStat",   "()V",  klj_HFP_clear},
    {"com/unity3d/player/HFPStatus", "getHFPStat",     "()Z",  klj_HFP_get},
    {"com/unity3d/player/HFPStatus", "requestHFPStat", "()V",  klj_HFP_request},
    {"com/unity3d/player/HFPStatus", "setHFPRecordingStat", "(Z)V", klj_HFP_set_recording},

    {"android/net/Uri", "encode", "(Ljava/lang/String;)Ljava/lang/String;", klj_Uri_encode},
    {"java/lang/String", "<init>", "([BLjava/lang/String;)V", klj_String_init_bytes},
    {"java/lang/String", "getBytes", "(Ljava/lang/String;)[B", klj_String_getBytes},
    {"com/unity3d/player/UnityPlayer", "loadLibrary", "(Ljava/lang/String;)Z", klj_UnityPlayer_loadLibrary},
    {"java/lang/System", "nanoTime", "()J", klj_System_nanoTime},
    {"java/lang/System", "currentTimeMillis", "()J", klj_System_currentTimeMillis},
    {"com/unity3d/player/UnityPlayer", "getNetworkProxySettings", "(Ljava/lang/String;)Ljava/lang/String;", klj_UnityPlayer_getNetworkProxySettings},
    {"com/unity3d/player/UnityPlayer", "addPhoneCallListener", "()V", klj_UnityPlayer_addPhoneCallListener},
    {"java/lang/Class",  "getName",  "()Ljava/lang/String;", klj_Class_getName},
    // Unity builds this signature from Class.getName(), so it is spelled with
    // dots where a real JNI signature would have slashes. Matched as the string
    // the guest actually asks for.
    {"com/oculus/oculusdeviceconfig/OculusDeviceConfig", "init", "(Lcom.unity3d.player.UnityPlayerActivity;)V", klj_OculusDeviceConfig_init},
    {"com/oculus/oculusdeviceconfig/OculusDeviceConfig", "getCurrentState", "()I", klj_OculusDeviceConfig_getCurrentState},
    {"com/oculus/oculusdeviceconfig/OculusDeviceConfig", "getError", "()Ljava/lang/String;", klj_OculusDeviceConfig_getError},
    {"com/oculus/oculusdeviceconfig/OculusDeviceConfig", "didPrefetchParamName", "(Ljava/lang/String;)Z", klj_OculusDeviceConfig_didPrefetchParamName},
    {"com/oculus/oculusdeviceconfig/OculusDeviceConfig", "getBoolean", "(Lcom.unity3d.player.UnityPlayerActivity;Ljava/lang/String;)Z", klj_OculusDeviceConfig_getBoolean},
    {"android/os/Vibrator", "hasVibrator", "()Z", klj_Vibrator_hasVibrator},
    {"android/content/pm/PackageManager", "getInstallerPackageName", "(Ljava/lang/String;)Ljava/lang/String;", klj_PackageManager_getInstallerPackageName},
    {"java/util/Locale", "getDefault",  "()Ljava/util/Locale;",   klj_Locale_getDefault},
    {"java/util/Locale", "getLanguage", "()Ljava/lang/String;",   klj_Locale_getLanguage},
    {"java/util/Locale", "getCountry",  "()Ljava/lang/String;",   klj_Locale_getCountry},
    {"java/util/Locale", "toLanguageTag", "()Ljava/lang/String;", klj_Locale_toLanguageTag},
    {"android/media/MediaRouter$RouteInfo", "getPresentationDisplay", "()Landroid/view/Display;", klj_RouteInfo_getPresentationDisplay},

    {"android/app/AlertDialog$Builder", "<init>", "(Landroid/content/Context;)V", klj_AlertBuilder_init},
    {"android/app/AlertDialog$Builder", "setTitle",
     "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", klj_AlertBuilder_setTitle},
    {"android/app/AlertDialog$Builder", "setMessage",
     "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", klj_AlertBuilder_setMessage},
    {"android/app/AlertDialog$Builder", "setPositiveButton",
     "(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)"
     "Landroid/app/AlertDialog$Builder;", klj_AlertBuilder_setPositive},
    {"android/app/AlertDialog$Builder", "setNegativeButton",
     "(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)"
     "Landroid/app/AlertDialog$Builder;", klj_AlertBuilder_setNegative},
    {"android/app/AlertDialog$Builder", "setCancelable", "(Z)Landroid/app/AlertDialog$Builder;",
     klj_AlertBuilder_self},
    {"android/app/AlertDialog$Builder", "setOnCancelListener",
     "(Landroid/content/DialogInterface$OnCancelListener;)Landroid/app/AlertDialog$Builder;",
     klj_AlertBuilder_self},
    {"android/app/AlertDialog$Builder", "setOnKeyListener",
     "(Landroid/content/DialogInterface$OnKeyListener;)Landroid/app/AlertDialog$Builder;",
     klj_AlertBuilder_self},
    {"android/app/AlertDialog$Builder", "setOnDismissListener",
     "(Landroid/content/DialogInterface$OnDismissListener;)Landroid/app/AlertDialog$Builder;",
     klj_AlertBuilder_self},
    {"android/app/AlertDialog$Builder", "create", "()Landroid/app/AlertDialog;",
     klj_AlertBuilder_create},
    {"android/app/AlertDialog$Builder", "show", "()Landroid/app/AlertDialog;",
     klj_AlertBuilder_show},
    {"android/app/AlertDialog", "show", "()V", klj_AlertBuilder_show},
    {"android/app/AlertDialog$Builder", "setView", "(Landroid/view/View;)Landroid/app/AlertDialog$Builder;",
     klj_AlertBuilder_self},
    {"android/widget/CheckBox", "<init>", "(Landroid/content/Context;)V", klj_View_new},
    {"android/widget/TextView", "setText", "(Ljava/lang/CharSequence;)V", klj_View_void},
    {"android/widget/CompoundButton", "setChecked", "(Z)V", klj_View_void},
    {"android/widget/CompoundButton", "isChecked", "()Z", klj_CheckBox_isChecked},
    {"android/widget/CompoundButton", "setOnCheckedChangeListener",
     "(Landroid/widget/CompoundButton$OnCheckedChangeListener;)V", klj_View_void},
    {"android/content/Context", "getAssets", "()Landroid/content/res/AssetManager;", klj_Context_getAssets},
    {"android/content/Context", "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", klj_Context_getExternalFilesDir},
    {"android/content/Context", "getFilesDir", "()Ljava/io/File;", klj_Context_getFilesDir},
    {"android/content/Context", "getCacheDir", "()Ljava/io/File;", klj_Context_getCacheDir},
    {"android/content/Context", "getExternalCacheDir", "()Ljava/io/File;",
     klj_Context_getExternalCacheDir},
    {"android/app/ActivityThread", "currentActivityThread", "()Landroid/app/ActivityThread;",
     klj_ActivityThread_current},
    {"android/app/ActivityThread", "getApplication", "()Landroid/app/Application;",
     klj_ActivityThread_getApplication},
    {"android/content/Context", "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;", klj_Context_getApplicationInfo},
    {"android/content/pm/PackageManager", "getPackageInfo",
     "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", klj_PM_getPackageInfo},
    // Same singleton the Context hands out — there is one application here.
    {"android/content/pm/PackageManager", "getApplicationInfo",
     "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;", klj_Context_getApplicationInfo},
    // Unreal Engine 4's GameActivity — see klj_UE4_GetFontDirectory.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetFontDirectory",
     "()Ljava/lang/String;", klj_UE4_GetFontDirectory},
    {"android/os/Environment", "getExternalStorageState", "()Ljava/lang/String;", klj_Environment_getExternalStorageState},
    {"android/os/Environment", "getExternalStorageDirectory", "()Ljava/io/File;", klj_Environment_getExternalStorageDirectory},
    {"android/os/Environment", "isExternalStorageManager", "()Z", klj_Environment_isExternalStorageManager},
    {"java/io/File", "getAbsolutePath", "()Ljava/lang/String;", klj_File_getPath},
    {"java/io/File", "getPath",         "()Ljava/lang/String;", klj_File_getPath},
    {"java/io/File", "getCanonicalPath","()Ljava/lang/String;", klj_File_getPath},
    {"java/io/File", "getParent",       "()Ljava/lang/String;", klj_File_getParent},
    {"android/content/res/AssetManager", "open", "(Ljava/lang/String;)Ljava/io/InputStream;", klj_AssetManager_open},
    {"com/unity3d/player/UnityPlayer", "initializeGoogleAr", "()Z", klj_UnityPlayer_initializeGoogleAr},
    {"com/unity/oculus/OculusUnity", "getIsOnOculusHardware", "()Z",
     klj_OculusUnity_isOnOculusHardware},
    {"com/unity/oculus/OculusUnity", "loadLibrary", "(Ljava/lang/String;)V",
     klj_OculusUnity_loadLibrary},
    {"com/unity/oculus/OculusUnity", "getManifestSetting", "(Ljava/lang/String;)Z",
     klj_OculusUnity_manifestSetting},
    {"com/unity/oculus/OculusUnity", "getLowOverheadMode", "()Z",
     klj_OculusUnity_lowOverhead},
    {"com/unity/oculus/OculusUnity", "getLateLatching", "()Z",
     klj_OculusUnity_lateLatching},
    {"com/unity/oculus/OculusUnity", "getLateLatchingDebug", "()Z",
     klj_OculusUnity_lateLatchingDebug},
    {"com/unity/oculus/OculusUnity", "initOculus", "()V", klj_OculusUnity_initOculus},
    {"com/unity/oculus/OculusUnity", "pauseOculus",   "()V", klj_OculusUnity_lifecycle},
    {"com/unity/oculus/OculusUnity", "resumeOculus",  "()V", klj_OculusUnity_lifecycle},
    {"com/unity/oculus/OculusUnity", "destroyOculus", "()V", klj_OculusUnity_lifecycle},
    {"com/unity3d/player/PlayAssetDeliveryUnityWrapper", "init",
     "(Landroid/content/Context;)Lcom/unity3d/player/PlayAssetDeliveryUnityWrapper;",
     klj_PlayAssetDelivery_init},
    {"com/unity3d/player/PlayAssetDeliveryUnityWrapper", "getInstance",
     "()Lcom/unity3d/player/PlayAssetDeliveryUnityWrapper;",
     klj_PlayAssetDelivery_getInstance},
    {"com/unity3d/player/PlayAssetDeliveryUnityWrapper", "playCoreApiMissing", "()Z",
     klj_PlayAssetDelivery_missing},

    {"android/os/Bundle", "getString",  "(Ljava/lang/String;)Ljava/lang/String;", klj_Bundle_getString},
    {"android/os/Bundle", "getString",  "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", klj_Bundle_getString},
    {"android/os/Bundle", "getBoolean", "(Ljava/lang/String;)Z",  klj_Bundle_getBoolean},
    {"android/os/Bundle", "getBoolean", "(Ljava/lang/String;Z)Z", klj_Bundle_getBoolean},
    {"android/os/Bundle", "getInt",     "(Ljava/lang/String;)I",  klj_Bundle_getInt},
    {"android/os/Bundle", "getInt",     "(Ljava/lang/String;I)I", klj_Bundle_getInt},
    {"android/os/Bundle", "containsKey","(Ljava/lang/String;)Z",  klj_Bundle_containsKey},

    {"java/util/Scanner", "<init>",        "(Ljava/io/InputStream;Ljava/lang/String;)V", klj_Scanner_init},
    {"java/util/Scanner", "useDelimiter",  "(Ljava/lang/String;)Ljava/util/Scanner;",    klj_Scanner_useDelimiter},
    {"java/util/Scanner", "next",          "()Ljava/lang/String;",                       klj_Scanner_next},

    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetAssetManager",
     "()Landroid/content/res/AssetManager;", klj_Context_getAssets},
    {0}
};
