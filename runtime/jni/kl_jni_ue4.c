// Unreal Engine 4: MessageBox01, the GetMetaData family, the three
// BroadcastReceivers, MediaPlayer14
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

// ---- UE4's MessageBox01 ----
// `FPlatformMisc::MessageBoxExt` on Android: the engine news up one of these,
// sets a caption and a body, adds one button per choice, calls show() and BLOCKS
// on the answer. It is the only way a UE4 guest can say something to a person,
// and in a Shipping build with logging stripped it is very nearly the only way
// it can say anything at all — so this prints the whole box rather than
// swallowing it. A refusal here would have been an abort naming `setText`, which
// says nothing about what the engine was trying to tell us.
//
// show() answers 0, the FIRST button, which is UE4's own convention for the
// default/affirmative choice (`EAppReturnType::Ok` is the first thing added by
// every caller in the engine). There is no person here to press anything, and a
// modal that never returns is a hang.
static char g_ue4_msg_caption[512];
static char g_ue4_msg_text[4096];
static char g_ue4_msg_buttons[512];

// ---- UE4's GetMetaData* family ----
// `FAndroidMisc::GetConfigRulesVariable`'s neighbours: the engine asks its
// activity for a named value and the activity answers from one of THREE places,
// which is the whole reason these cannot be a plain Bundle read. Transcribed
// from this APK's GameActivity: a handful of `ue4.`-prefixed keys are synthetic
// and answered from the Display or from a system property, and everything else
// falls through to the manifest <meta-data> Bundle.
//
// Routing the synthetic ones at the same Display handlers Unity reads is the
// display-panel group answer again — two engines asking one device the same
// question must not get two numbers.
//
// Unreal 5 renamed the synthetic keys along with everything else: `ue4.display`,
// `ue4.display.getRefreshRate` and `ue4.displaymetrics.dpi` are spelled
// `unreal.*` in 5.3 (Pinball FX VR), and the Java behind them is the same
// method with the same sparse-switch. So they are ALIASED here rather than
// duplicated below, the same choice kl_jni.c's class fold makes.
//
// Only the display family, and that limit is the point: `unreal.GameActivity.*`
// keys are GENUINE manifest entries in a UE5 APK (CookedFlavors,
// bValidateTextureFormats) and must reach the Bundle read untouched. A blanket
// `unreal.` -> `ue4.` prefix swap would send them looking for a key the manifest
// does not have.
//
// What this cost when it was missing: `unreal.displaymetrics.dpi` fell through
// to the manifest, answered "", and FAndroidDeviceProfileSelector split "" on
// "," and indexed element 0 of the empty array — SIGSEGV at address 0x8, on a
// guest worker thread, eight instructions later, naming nothing.
static const char *klj_ue5_meta_alias(const char *k) {
    static const struct { const char *ue5, *ue4; } alias[] = {
        {"unreal.displaymetrics.dpi",     "ue4.displaymetrics.dpi"},
        {"unreal.displaymetrics.metrics", "ue4.displaymetrics.metrics"},
        {"unreal.display",                "ue4.display"},
        {"unreal.display.getRefreshRate", "ue4.display.getRefreshRate"},
        {"unreal.display.PresentationDeadlineNanos",
         "ue4.display.PresentationDeadlineNanos"},
        {"unreal.display.AppVsyncOffsetNanos",
         "ue4.display.AppVsyncOffsetNanos"},
        {0, 0}
    };
    if (!k) return "";
    for (int i = 0; alias[i].ue5; i++)
        if (strcmp(k, alias[i].ue5) == 0) return alias[i].ue4;
    return k;
}

static klj_val klj_GA_hasMetaDataKey(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *k = n > 0 ? klj_str(a[0].l) : NULL;
    return (klj_val){.j = (uint64_t)(k && kl_jni_manifest_meta(k) != NULL)};
}
static klj_val klj_GA_getMetaDataBoolean(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *k = n > 0 ? klj_str(a[0].l) : NULL;
    const char *v = k ? kl_jni_manifest_meta(k) : NULL;
    return (klj_val){.j = (uint64_t)(v && strcmp(v, "true") == 0)};
}
static klj_val klj_GA_getMetaDataInt(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *k = klj_ue5_meta_alias(n > 0 ? klj_str(a[0].l) : "");
    // The graphics API decision, and it is made HERE rather than by the RHI:
    // UE4 offers Vulkan only when the platform reports the vulkan.version
    // feature, and the activity reads that out of
    // PackageManager.getSystemAvailableFeatures().
    //
    // RE4 is packaged VULKAN-ONLY and says so itself — presenting no Vulkan
    // feature gets "This device does not support Vulkan but the app was not
    // packaged with ES 3.1 support" out of its own message box, which is the
    // engine reporting that the alternative does not exist in this build rather
    // than a preference. So the answer is a supported Vulkan feature version and
    // level 1, and the guest reaches kl_vulkan.c — the path BONELAB already
    // drives. `KL_UE4_VULKAN=0` restores the refusal exactly, which is the A/B
    // for anything that suspects the API choice.
    //
    // The version reported here is the GATE, not the instance version: UE reads
    // this feature (out of PackageManager.getSystemAvailableFeatures()) and
    // compares it to its engine minimum BEFORE it ever calls a vk* entry point
    // (the guest instance is still created at VK_API_VERSION_1_0 in kl_vulkan.c).
    // UE4 accepts 1.0, so 1.0.3 ("what a Quest 2 reports") sufficed for BONELAB
    // and RE4. UE5 raised the Android minimum to Vulkan 1.1: OLAR (UE5,
    // Vulkan-only) queried this feature, got 1.0.3, and — with NO vk* call — put
    // up exactly the message above and quit (SIGKILL). Report 1.1.0, which
    // clears the UE5 gate and is still >= the UE4 minimum, so both engines keep
    // taking the Vulkan path. MoltenVK backs 1.4 here, so 1.1 is safely serviced.
    // The two audio keys, and answering them is not optional: GameActivity
    // resolves them through `AudioManager.getProperty`, and
    // `FMixerPlatformAndroid::GetPlatformSettings` rounds its callback size up
    // to a multiple of the frames-per-buffer answer with
    // `while (n < want) n += framesPerBuffer;` — no zero check. A 0 there is an
    // INFINITE LOOP inside the engine, on the game thread, in
    // `UEngine::InitializeAudioDeviceManager`, holding the audio device
    // manager's lock; the process stays alive with every counter healthy and
    // the boot never completes. Routed at kl_jni's own AudioManager handler so
    // the number a UE4 guest is told and the number a Unity guest is told are
    // the same number.
    if (strcmp(k, "audiomanager.framesPerBuffer") == 0)
        return (klj_val){.j = (uint64_t)KLJ_AUDIO_FRAMES};
    if (strcmp(k, "audiomanager.optimalSampleRate") == 0)
        return (klj_val){.j = (uint64_t)KLJ_AUDIO_RATE};
    if (strcmp(k, "android.hardware.vulkan.version") == 0
        || strcmp(k, "android.hardware.vulkan.level") == 0) {
        int level = strcmp(k, "android.hardware.vulkan.level") == 0;
        // KL_UE4_VULKAN: 0 withholds the feature entirely (the A/B that
        // restores the pre-Vulkan refusal); 1 or unset keeps the default,
        // VK_MAKE_VERSION(1,1,0); anything larger is used AS the version word,
        // so a guest that wants a different number can be given one without a
        // rebuild. Unreal decides from the NUMBER, not from a probe: Pinball FX
        // VR (UE5.3) given 1.0.3 dlopens libvulkan.so, calls nothing through
        // it, and puts up "This device does not support Vulkan but the app was
        // not packaged with ES 3.1 support"; given 1.1 it reaches
        // vkCreateInstance and vkCreateDevice.
        const char *env_v = getenv("KL_UE4_VULKAN");
        long want = env_v ? strtol(env_v, NULL, 0) : 1;
        int on = want != 0;
        int v = !on ? 0 : level ? 1
              : want > 1 ? (int)want
              : ((1 << 22) | (1 << 12));   /* VK_MAKE_VERSION(1,1,0) — UE5 minimum */
        KLJ_LOG("GetMetaDataInt(\"%s\") -> 0x%x%s", k, v,
                on ? "" : " (no Vulkan feature presented — KL_UE4_VULKAN=0)");
        return (klj_val){.j = (uint64_t)(int64_t)v};
    }
    const char *v = kl_jni_manifest_meta(k);
    return (klj_val){.j = (uint64_t)(int64_t)(v ? strtol(v, NULL, 10) : 0)};
}

static klj_val klj_GA_getMetaDataLong(void *env, void *self, const klj_val *a, int n) {
    const char *k = klj_ue5_meta_alias(n > 0 ? klj_str(a[0].l) : "");
    if (strcmp(k, "ue4.display.PresentationDeadlineNanos") == 0)
        return klj_Display_getPresentationDeadlineNanos(env, self, a, n);
    if (strcmp(k, "ue4.display.AppVsyncOffsetNanos") == 0)
        return klj_Display_getAppVsyncOffsetNanos(env, self, a, n);
    const char *v = kl_jni_manifest_meta(k);
    return (klj_val){.j = (uint64_t)(v ? strtoll(v, NULL, 10) : 0)};
}
static klj_val klj_GA_getMetaDataFloat(void *env, void *self, const klj_val *a, int n) {
    const char *k = klj_ue5_meta_alias(n > 0 ? klj_str(a[0].l) : "");
    if (strcmp(k, "ue4.display.getRefreshRate") == 0)
        return klj_Display_getRefreshRate(env, self, a, n);
    const char *v = kl_jni_manifest_meta(k);
    return (klj_val){.d = v ? strtod(v, NULL) : 0.0};
}
static klj_val klj_GA_getMetaDataString(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *k = klj_ue5_meta_alias(n > 0 ? klj_str(a[0].l) : "");
    // `getprop:<name>` is a system property read wearing a meta-data key's
    // clothes, and the properties are the ones kl_libc's sysprop table already
    // answers — so this goes through that rather than growing a second table.
    if (strncmp(k, "getprop:", 8) == 0) {
        extern int klb_sysprop_get(const char *name, char *value);   // kl_libc.c
        static char val[512];
        val[0] = '\0';
        klb_sysprop_get(k + 8, val);
        KLJ_LOG("GetMetaDataString(\"%s\") -> \"%s\"", k, val);
        return (klj_val){.l = kl_jni_new_string(val)};
    }
    // `ue4.displaymetrics.dpi` is the DisplayMetrics triple as one comma
    // separated string — xdpi, ydpi, densityDpi — which the engine splits with
    // `FString::ParseIntoArray` and then indexes WITHOUT checking the count.
    // So an empty answer here is not a missing setting, it is a null
    // dereference inside `FAndroidApplicationMisc::ComputePhysicalScreenDensity`
    // eight instructions later, on a guest worker thread, naming nothing.
    // Built from the same three constants DisplayMetrics' own fields are, so
    // the two doors onto the panel cannot disagree.
    //
    // UE5 spells the key `unreal.displaymetrics.dpi` (the package moved from
    // com.epicgames.ue4 to com.epicgames.unreal), so match either prefix — the
    // literal is in libUnreal and Wanderer/OLAR die on exactly this dereference
    // when the unreal spelling falls through to the empty manifest answer.
    if (strcmp(k, "ue4.displaymetrics.dpi") == 0 ||
        strcmp(k, "unreal.displaymetrics.dpi") == 0) {
        static char dpi[64];
        snprintf(dpi, sizeof dpi, "%.2f,%.2f,%d",
                 (double)KLJ_DISPLAY_XDPI, (double)KLJ_DISPLAY_YDPI, KLJ_DISPLAY_DPI);
        KLJ_LOG("GetMetaDataString(\"%s\") -> \"%s\"", k, dpi);
        return (klj_val){.l = kl_jni_new_string(dpi)};
    }
    const char *v = kl_jni_manifest_meta(k);
    return (klj_val){.l = kl_jni_new_string(v ? v : "")};
}

// The launch intent's extras (UE4.27, e.g. Deadpool VR). This host launches the
// activity with no intent extras at all, so both answers are the honest ones:
// the String getter's null is what Java's `getStringExtra` returns for an
// absent key, and false is `getBooleanExtra`'s. Null (not "") matters — UE4
// tests the String answer with `isEmpty` guarded by a null check of its own.
static klj_val klj_GA_getIntentExtrasString(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *k = n > 0 ? klj_str(a[0].l) : "";
    KLJ_LOG("AndroidThunkJava_GetIntentExtrasString(\"%s\") -> null (no launch intent extras)", k);
    return (klj_val){.l = 0};
}
static klj_val klj_GA_getIntentExtrasBoolean(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *k = n > 0 ? klj_str(a[0].l) : "";
    KLJ_LOG("AndroidThunkJava_GetIntentExtrasBoolean(\"%s\") -> false (no launch intent extras)", k);
    return (klj_val){.j = 0};
}

// `AndroidThunkJava_GetAndroidId()` — UE's GameActivity wrapping the one line
// `Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID)`.
// Pinball FX VR (UE5.3) asks for it the instant the player finishes its two
// AGREE-to-terms screens, which is the first thing past the lobby that wants to
// know WHICH device accepted; unbound, it is klj_call_common's abort() — a
// SIGABRT on the GameThread.
//
// It is a device-identity question: it answers "this installation, on this
// device", which is what an analytics or terms-acceptance record keys on, and
// asserts nothing about what the player owns.
//
// The answer is KLJ_ANDROID_ID — the SAME string Settings.Secure.getString
// hands a Unity guest (kl_jni_net.c), shared through kl_jni_int.h so one device
// cannot claim to be two. Non-null and 16 hex digits both matter: UE stores the
// result straight into an FString, and a null is a crash inside the guest's own
// string handling rather than a blank.
static klj_val klj_GA_getAndroidId(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("AndroidThunkJava_GetAndroidId() -> \"%s\" (synthetic and constant, "
            "the same id Settings.Secure.getString answers)", KLJ_ANDROID_ID);
    return (klj_val){.l = kl_jni_new_string(KLJ_ANDROID_ID)};
}

// The display's refresh rate, read and set. One display at one fixed rate —
// the same KLJ_DISPLAY_REFRESH Display.getRefreshRate answers — so the current
// rate is that rate, and a request to set anything else is refused the way a
// panel without that mode refuses it. (The supported-list getter is further
// down, with the orientation answers.)
static klj_val klj_GA_getNativeRefreshRate(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = (int32_t)KLJ_DISPLAY_REFRESH};
}
static klj_val klj_GA_setNativeRefreshRate(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int want = n > 0 ? (int)(int32_t)a[0].j : 0;
    int ok = want == (int)KLJ_DISPLAY_REFRESH;
    KLJ_LOG("AndroidThunkJava_SetNativeDisplayRefreshRate(%d) -> %s (the one rate this display has)",
            want, ok ? "true" : "false");
    return (klj_val){.j = ok};
}

// How the JNI surface reaches back into the guest. See kl_jni.h.
static void *(*g_guest_native)(const char *symbol);
void kl_jni_set_guest_native_resolver(void *(*resolve)(const char *symbol)) {
    g_guest_native = resolve;
}

// `AndroidThunkJava_InitHMDs` — the activity posts a Runnable to the UI thread
// whose entire body is `nativeInitHMDs()`, which is the engine giving every HMD
// module its PreInit. Nothing else starts the XR path, so a no-op here is a
// guest with no head-mounted display and no error anywhere.
//
// Called INLINE rather than queued, and the difference is stated because it is
// real: on Android this crosses from the game thread to the UI thread and comes
// back later, and here it runs on the caller. The engine's own code is what
// runs either way, and running it late is the failure that has no symptom — the
// module list is consumed by the very next thing the game thread does.
static klj_val klj_GA_initHMDs(void *env, void *self, const klj_val *a, int n) {
    (void)a; (void)n;
    // UE5 renamed the package and the exported-native prefix in the same release
    // (com.epicgames.ue4 -> com.epicgames.unreal), and libUnreal exports
    // `Java_com_epicgames_unreal_GameActivity_nativeInitHMDs`. Wanderer is UE5,
    // so the ue4 spelling alone resolved to NULL and the whole XR path — the only
    // thing that starts rendering — never began (0 frames, no ovrp, no swapchain).
    // Try both families; the guest exports exactly one of them.
    void (*fn)(void *, void *) = NULL;
    if (g_guest_native) {
        fn = (void (*)(void *, void *))g_guest_native(
                 "Java_com_epicgames_ue4_GameActivity_nativeInitHMDs");
        if (!fn)
            fn = (void (*)(void *, void *))g_guest_native(
                     "Java_com_epicgames_unreal_GameActivity_nativeInitHMDs");
    }
    if (!fn) {
        KLJ_LOG("AndroidThunkJava_InitHMDs: no guest nativeInitHMDs to call back "
                "into — the HMD modules never get PreInit");
        return (klj_val){.j = 0};
    }
    KLJ_LOG("AndroidThunkJava_InitHMDs -> nativeInitHMDs()");
    fn(env, self);
    return (klj_val){.j = 0};
}

// ---- UE's virtual keyboard -------------------------------------------------
//
// The one UI element a guest cannot draw for itself. Unity hands the job to
// UnityPlayer.showSoftInput (kl_jni_softinput.c); UE hands it to its own
// GameActivity, and the two surfaces share nothing but the shape of the
// problem — four Java methods go out and four natives come back.
//
// **Pinball FX VR died here.** The post-game score screen offers name entry,
// the player pinched Edit, and the log's last live line was
//
//   [jni] no host implementation for com/epicgames/ue4/GameActivity.
//         AndroidThunkJava_ShowVirtualKeyboardInput(ILjava/lang/String;Ljava/lang/String;)V
//
// which is klj_call_common's abort() — a SIGABRT on the GameThread.
//
// Everything below is transcribed from Pinball FX VR's GameActivity.smali rather
// than from the engine's source, because the two paths differ in what they
// report and the difference is the whole of the close sequence:
//
//   AndroidThunkJava_ShowVirtualKeyboardInput(type, label, contents)
//        the NEW keyboard. Stores contents/type, posts GameActivity$t after
//        200 ms, which runs processLastVirtualKeyboardCommand: it sets the
//        y0 EditText's text and inputType, shows the IME, and calls
//        nativeVirtualKeyboardVisible(true) (GameActivity.smali:19297).
//        Live text comes back through the o0 TextWatcher as
//        nativeVirtualKeyboardChanged(s), and **there is no
//        nativeVirtualKeyboardResult on this path at all** — grep the whole
//        smali tree: the only two callers of it are the DIALOG's listeners.
//        The engine commits on the keys instead, and the dismissal is y0's
//        onKeyPreIme: KEYCODE_BACK (4) -> AndroidThunkJava_HideVirtualKeyboardInput()
//        and then nativeVirtualKeyboardSendKey(4).
//   AndroidThunkJava_HideVirtualKeyboardInput()
//        the same handler's other arm: hide the IME, visible(false)
//        (GameActivity.smali:19150).
//   AndroidThunkJava_ShowVirtualKeyboardInputDialog(type, label, contents)
//        the OLD AlertDialog, chosen by the engine's `Android.NewKeyboard`
//        cvar. This one does have a result, and its two listeners fix the
//        shapes exactly:
//          q0 (OK)     -> nativeVirtualKeyboardResult(true, editText.getText())
//          p0 (Cancel) -> nativeVirtualKeyboardChanged(<original contents>)
//                         then nativeVirtualKeyboardResult(false, " ")
//        Note p0 restores the original text BEFORE reporting the refusal, and
//        that the refusal's string is a single SPACE and not empty — both are
//        that method's, not choices made here.
//   AndroidThunkJava_HideVirtualKeyboardInputDialog()
//        dismiss, nothing native.
//
// The natives are static exports of libUnreal (`Java_com_epicgames_ue4_…`,
// folded onto `…_unreal_…` by ue4_native_symbol in kl_ue4.c), not
// RegisterNatives bindings, so they are reached through g_guest_native — and
// resolved per event rather than cached, for kl_jni_softinput.c's reason: a
// lookup that ran before the guest published its symbols would pin a NULL and
// leave the keyboard silently dead for the run.
//
// **What a host without a keyboard does, and why it is not a no-op.** There is
// no IME here. A no-op leaves the engine believing a keyboard is up and the
// player staring at a text field that can never close — worse than the abort,
// because it has no log line. So a show is followed immediately by a close,
// delivered as soon as the report queue drains.
//
// **Which native actually ends the entry, read off libUnreal.** Replaying the
// Java's own BACK dismissal — visible(false) then sendKey(4) — leaves a field
// that stays open with a blinking caret and eats every controller button for
// the rest of the session. On a real device the *IME going away* is what ends
// the edit, and the Java only has to tidy up after it; there is no IME here, so
// nothing would end it. The four natives, disassembled from Pinball FX VR's
// libUnreal:
//
//   nativeVirtualKeyboardVisible(b)   0x4000fc4
//        stores b at the "keyboard is up" global (+0x8ac). On FALSE it also
//        clears the two-word weak pointer at +0x950/+0x958 and releases it —
//        i.e. `VirtualKeyboardWidget.Reset()`. The WIDGET is never told.
//   nativeVirtualKeyboardResult(1, s) 0x4001e24
//        the only one that reaches the widget: it converts s to an FString and
//        dispatches a GameThread task over the live weak pointer — the engine's
//        `SetTextFromVirtualKeyboard(s, TextEntryAccepted)`, which is what
//        broadcasts OnTextCommitted and lets the game's own entry screen close
//        and give input back. Then it Resets.
//   nativeVirtualKeyboardResult(0, s) 0x4001ecc (the b.ne arm)
//        Reset and nothing else — the same do-nothing as visible(false). A
//        "cancel" cannot close a widget that has taken focus.
//   nativeVirtualKeyboardSendKey(k)   0x4002250
//        pushes {0, k} onto the deferred-message queue. A key event. It does
//        not touch the weak pointer and cannot end an entry.
//
// So the close is `result(true, …)` on BOTH paths, and it must come BEFORE any
// visible(false) — the Reset is what makes a later commit a no-op. What differs
// is only the text: a commit reports the edited text, and a cancel reports the
// SEED, which is the truthful "nothing changed" available on a host with no
// IME. The BACK key is not sent any more: it closed nothing, and injecting a
// back-navigation nobody asked for is a real risk once the field really shuts.
//
// `KL_UE_KEYBOARD=1` holds the field open instead and lets a frontend type into
// it through the kl_jni_soft_input_* calls in kl_jni.h — which is what the SDL
// viewer already implements, and what a visionOS text-entry UI would implement
// when there is one. `KL_UE_KEYBOARD_TEXT=<s>` is the version that works with
// no frontend at all: the auto-close commits `<s>` instead of the seed, so the
// initials on a score screen are the player's and not the guest's default.
#define KLJ_VK_TEXT_MAX 1024
#define KLJ_VK_QUEUE    16

enum {
    KLJ_VK_VISIBLE = 1,   // nativeVirtualKeyboardVisible(Z)
    KLJ_VK_CHANGED,       // nativeVirtualKeyboardChanged(Ljava/lang/String;)
    KLJ_VK_RESULT,        // nativeVirtualKeyboardResult(ZLjava/lang/String;)
    KLJ_VK_SENDKEY,       // nativeVirtualKeyboardSendKey(I)
};

typedef struct {
    int  kind, a;
    char s[KLJ_VK_TEXT_MAX];
} klj_vk_ev;

static struct {
    pthread_mutex_t mu;
    void       *activity;        // the GameActivity the guest called us on
    int         open;            // a show was seen, no close reported yet
    int         dialog;          // the old AlertDialog path, not the new one
    int         type;            // Android inputType bits
    char        text[KLJ_VK_TEXT_MAX];
    char        seed[KLJ_VK_TEXT_MAX];   // what the guest opened it with
    char        label[256];
    klj_vk_ev   q[KLJ_VK_QUEUE];
    int         qh, qn;
    unsigned    dropped, opens;
} g_vk = { .mu = PTHREAD_MUTEX_INITIALIZER };

// Caller holds g_vk.mu. A full queue drops the OLDEST, and says so once: these
// are ordered reports and a silent drop is an editor that never closes.
static void klj_vk_push(int kind, int a, const char *s) {
    if (g_vk.qn == KLJ_VK_QUEUE) {
        g_vk.qh = (g_vk.qh + 1) % KLJ_VK_QUEUE;
        g_vk.qn--;
        if (!g_vk.dropped++)
            KLJ_LOG("virtual keyboard: report queue full — dropping the oldest. "
                    "Nothing is draining it; is the pump running?");
    }
    klj_vk_ev *e = &g_vk.q[(g_vk.qh + g_vk.qn) % KLJ_VK_QUEUE];
    e->kind = kind; e->a = a;
    e->s[0] = '\0';
    if (s) { strncpy(e->s, s, sizeof e->s - 1); e->s[sizeof e->s - 1] = '\0'; }
    g_vk.qn++;
}

// The close, in the guest's own two shapes. Caller holds g_vk.mu and has
// already decided the field is open.
static void klj_vk_push_close_locked(int canceled) {
    // What the widget is told, on either path. A cancel reports the SEED: the
    // engine has exactly one end-of-entry native and it is an ACCEPT, so the
    // honest refusal is accepting the text the field already had.
    const char *final = canceled ? g_vk.seed : g_vk.text;
    if (g_vk.dialog) {
        // The AlertDialog listeners, transcribed. Cancel restores the original
        // text first and its refusal string is a single space (p0); OK reports
        // the edited text (q0). Neither touches visibility — the dialog's own
        // dismiss does that, and there is no nativeVirtualKeyboardVisible on
        // either listener.
        //
        // The dialog's cancel is left exactly as its Java writes it, refusal
        // string and all, because on THIS path the engine really does get a
        // result either way and the Reset in the `update == false` arm is the
        // dialog's own choice, not a hole we are patching.
        if (canceled) {
            klj_vk_push(KLJ_VK_CHANGED, 0, g_vk.seed);
            klj_vk_push(KLJ_VK_RESULT, 0, " ");
        } else {
            klj_vk_push(KLJ_VK_RESULT, 1, g_vk.text);
        }
    } else {
        // The new keyboard. `result(true, …)` is the only native that reaches
        // the entry widget (0x4001e24 — see the block above), and it must run
        // before anything Resets the weak pointer it dispatches over, so
        // visible(false) comes LAST and only clears the engine's "a keyboard is
        // up" flag. The live-text report goes first, the way the o0 TextWatcher
        // would have delivered it.
        klj_vk_push(KLJ_VK_CHANGED, 0, final);
        klj_vk_push(KLJ_VK_RESULT, 1, final);
        klj_vk_push(KLJ_VK_VISIBLE, 0, NULL);
    }
    g_vk.open = 0;
}

// Run the queue. Called from klj_drain_soft_input, i.e. on the thread that
// pumps the looper — which is the UI thread by the only definition that
// matters, and the thread GameActivity's own handlers run these natives on.
// The lock is DROPPED before the guest runs: nativeVirtualKeyboardChanged
// reaches engine code that is allowed to call straight back at us.
unsigned klj_ue_vk_drain(void) {
    unsigned ran = 0;
    for (;;) {
        klj_vk_ev e;
        pthread_mutex_lock(&g_vk.mu);
        if (!g_vk.qn) { pthread_mutex_unlock(&g_vk.mu); break; }
        e = g_vk.q[g_vk.qh];
        g_vk.qh = (g_vk.qh + 1) % KLJ_VK_QUEUE;
        g_vk.qn--;
        void *self = g_vk.activity;
        pthread_mutex_unlock(&g_vk.mu);

        static const char *const syms[] = {
            [KLJ_VK_VISIBLE] = "Java_com_epicgames_ue4_GameActivity_nativeVirtualKeyboardVisible",
            [KLJ_VK_CHANGED] = "Java_com_epicgames_ue4_GameActivity_nativeVirtualKeyboardChanged",
            [KLJ_VK_RESULT]  = "Java_com_epicgames_ue4_GameActivity_nativeVirtualKeyboardResult",
            [KLJ_VK_SENDKEY] = "Java_com_epicgames_ue4_GameActivity_nativeVirtualKeyboardSendKey",
        };
        void *f = (e.kind >= KLJ_VK_VISIBLE && e.kind <= KLJ_VK_SENDKEY && g_guest_native)
                ? g_guest_native(syms[e.kind]) : NULL;
        if (!f) {
            KLJ_LOG("virtual keyboard: report %d has no guest native — dropped", e.kind);
            continue;
        }
        void *env = kl_jni_env();
        kl_jni_local_frame_push();
        switch (e.kind) {
        case KLJ_VK_VISIBLE:
            // jboolean is a BYTE. An int would leave the high bits to chance.
            ((void (*)(void *, void *, uint8_t))f)(env, self, (uint8_t)(e.a != 0));
            break;
        case KLJ_VK_CHANGED:
            ((void (*)(void *, void *, void *))f)(env, self, kl_jni_new_string(e.s));
            break;
        case KLJ_VK_RESULT:
            ((void (*)(void *, void *, uint8_t, void *))f)
                (env, self, (uint8_t)(e.a != 0), kl_jni_new_string(e.s));
            break;
        default:
            ((void (*)(void *, void *, int32_t))f)(env, self, (int32_t)e.a);
            break;
        }
        kl_jni_local_frame_pop();
        ran++;
    }
    return ran;
}

static klj_val klj_GA_showVirtualKeyboard(void *env, void *self,
                                          const klj_val *a, int n, int dialog) {
    (void)env;
    int   type     = n > 0 ? (int)(int32_t)a[0].j : 0;
    const char *lbl = n > 1 ? klj_str(a[1].l) : NULL;
    const char *con = n > 2 ? klj_str(a[2].l) : NULL;
    // The guest's own inputType bits, named rather than reinterpreted: the
    // only one with a consequence outside the guest is the password variation,
    // because a frontend must not echo or log a secure field.
    int secure = (type & 0x80) != 0;
    int hold   = kl_env_int("KL_UE_KEYBOARD", 0);
    // The text an auto-close commits. Unset means "commit the seed", i.e. leave
    // the guest's own contents alone; set means a person told us their initials
    // once and wants them every time the game asks. Never for a secure field —
    // a baked password is not a thing we will type on someone's behalf.
    const char *baked = secure ? NULL : kl_env_str("KL_UE_KEYBOARD_TEXT", NULL);
    if (baked && !*baked) baked = NULL;

    pthread_mutex_lock(&g_vk.mu);
    if (self) g_vk.activity = self;
    g_vk.open   = 1;
    g_vk.dialog = dialog;
    g_vk.type   = type;
    g_vk.text[0] = g_vk.seed[0] = g_vk.label[0] = '\0';
    if (con) {
        strncpy(g_vk.text, con, sizeof g_vk.text - 1);
        g_vk.text[sizeof g_vk.text - 1] = '\0';
        memcpy(g_vk.seed, g_vk.text, sizeof g_vk.seed);
    }
    if (lbl) { strncpy(g_vk.label, lbl, sizeof g_vk.label - 1);
               g_vk.label[sizeof g_vk.label - 1] = '\0'; }
    g_vk.opens++;
    // processLastVirtualKeyboardCommand's show arm reports exactly one native,
    // and only on the new path — the dialog's own show() reports nothing.
    if (!dialog) klj_vk_push(KLJ_VK_VISIBLE, 1, NULL);
    if (!hold) {
        if (baked) {
            strncpy(g_vk.text, baked, sizeof g_vk.text - 1);
            g_vk.text[sizeof g_vk.text - 1] = '\0';
        }
        klj_vk_push_close_locked(baked ? 0 : 1);
    }
    pthread_mutex_unlock(&g_vk.mu);

    KLJ_LOG("AndroidThunkJava_ShowVirtualKeyboardInput%s(type=0x%x%s label=\"%s\") "
            "— %s", dialog ? "Dialog" : "", type, secure ? " secure" : "",
            lbl ? lbl : "",
            hold ? "held open; a frontend can type into it now"
                 : baked ? "there is no keyboard on this host, so it is committed "
                           "immediately with KL_UE_KEYBOARD_TEXT and the entry ends"
                 : "there is no keyboard on this host, so the entry is ended "
                   "immediately with the text the guest opened it with "
                   "(KL_UE_KEYBOARD_TEXT=<s> commits <s> instead; "
                   "KL_UE_KEYBOARD=1 holds it open for a frontend)");
    return (klj_val){.j = 0};
}

static klj_val klj_GA_showVirtualKeyboardInput(void *env, void *self,
                                               const klj_val *a, int n) {
    return klj_GA_showVirtualKeyboard(env, self, a, n, 0);
}
static klj_val klj_GA_showVirtualKeyboardInputDialog(void *env, void *self,
                                                     const klj_val *a, int n) {
    return klj_GA_showVirtualKeyboard(env, self, a, n, 1);
}

static klj_val klj_GA_hideVirtualKeyboard(void *env, void *self,
                                          const klj_val *a, int n, int dialog) {
    (void)env; (void)a; (void)n;
    pthread_mutex_lock(&g_vk.mu);
    if (self) g_vk.activity = self;
    int was = g_vk.open;
    g_vk.open = 0;
    // The hide arm of processLastVirtualKeyboardCommand is one native and only
    // on the new path (GameActivity.smali:19150); the dialog's hide is a
    // dismiss() and reports nothing at all.
    if (!dialog) klj_vk_push(KLJ_VK_VISIBLE, 0, NULL);
    pthread_mutex_unlock(&g_vk.mu);
    KLJ_LOG("AndroidThunkJava_HideVirtualKeyboardInput%s()%s",
            dialog ? "Dialog" : "", was ? "" : " — none was open");
    return (klj_val){.j = 0};
}

static klj_val klj_GA_hideVirtualKeyboardInput(void *env, void *self,
                                               const klj_val *a, int n) {
    return klj_GA_hideVirtualKeyboard(env, self, a, n, 0);
}
static klj_val klj_GA_hideVirtualKeyboardInputDialog(void *env, void *self,
                                                     const klj_val *a, int n) {
    return klj_GA_hideVirtualKeyboard(env, self, a, n, 1);
}

// ---- the frontend side, shared with Unity's ---------------------------------
//
// kl_jni.h's four kl_jni_soft_input_* calls are the whole frontend contract and
// there is exactly one of them, so a frontend does not have to know which
// engine the guest is: kl_jni_softinput.c asks these first and answers for
// Unity only when no UE field is open. A guest cannot have both — it has one
// engine.
int klj_ue_vk_active(void) {
    pthread_mutex_lock(&g_vk.mu);
    int open = g_vk.open;
    pthread_mutex_unlock(&g_vk.mu);
    return open;
}

int klj_ue_vk_get(char *buf, size_t cap, int *secure) {
    pthread_mutex_lock(&g_vk.mu);
    int open = g_vk.open;
    if (buf && cap) {
        strncpy(buf, open ? g_vk.text : "", cap - 1);
        buf[cap - 1] = '\0';
    }
    if (secure) *secure = open ? ((g_vk.type & 0x80) != 0) : 0;
    pthread_mutex_unlock(&g_vk.mu);
    return open;
}

void klj_ue_vk_set(const char *utf8) {
    if (!utf8) return;
    pthread_mutex_lock(&g_vk.mu);
    if (!g_vk.open) { pthread_mutex_unlock(&g_vk.mu); return; }
    strncpy(g_vk.text, utf8, sizeof g_vk.text - 1);
    g_vk.text[sizeof g_vk.text - 1] = '\0';
    // The o0 TextWatcher's afterTextChanged, which is what a keystroke IS on
    // this path: the engine's entry widget follows the field live and there is
    // no other channel for it.
    klj_vk_push(KLJ_VK_CHANGED, 0, g_vk.text);
    pthread_mutex_unlock(&g_vk.mu);
}

void klj_ue_vk_close(int canceled) {
    pthread_mutex_lock(&g_vk.mu);
    if (!g_vk.open) { pthread_mutex_unlock(&g_vk.mu); return; }
    int secure = (g_vk.type & 0x80) != 0;
    klj_vk_push_close_locked(canceled);
    pthread_mutex_unlock(&g_vk.mu);
    KLJ_LOG("virtual keyboard %s", canceled ? "canceled"
                                 : secure ? "committed (secure — text not logged)"
                                          : "committed");
}

// ---- ...and the gate for it -------------------------------------------------
//
// `make uevk`, in `make check`. It exists for kl_jni_softinput.c's reason and
// one more of its own: **the failure this seam had was an abort, and the
// failure it can have NEXT is silence.** A show that reports nothing leaves the
// engine holding an editor open forever, which produces no log line, no crash
// and no .ips — the only other instrument would be a person pinching Edit in a
// headset, which costs a build, an install and a launch.
//
// It drives the REAL bindings (they are static in this file, which is why the
// check is here rather than in tests/) against natives it publishes through the
// REAL guest-native resolver seam, so what it measures is the path the guest
// takes.
static struct {
    int  n;
    char log[32][256];
} g_vk_seen;

static void klj_vk_seen(const char *fmt, ...) {
    if (g_vk_seen.n >= (int)(sizeof g_vk_seen.log / sizeof g_vk_seen.log[0])) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_vk_seen.log[g_vk_seen.n++], sizeof g_vk_seen.log[0], fmt, ap);
    va_end(ap);
}

static void klj_vk_t_visible(void *env, void *self, uint8_t v) {
    (void)env; (void)self; klj_vk_seen("visible %d", v);
}
static void klj_vk_t_changed(void *env, void *self, void *s) {
    (void)env; (void)self;
    klj_vk_seen("changed \"%s\"", klj_str(s) ? klj_str(s) : "(null)");
}
static void klj_vk_t_result(void *env, void *self, uint8_t ok, void *s) {
    (void)env; (void)self;
    klj_vk_seen("result %d \"%s\"", ok, klj_str(s) ? klj_str(s) : "(null)");
}
static void klj_vk_t_sendkey(void *env, void *self, int32_t k) {
    (void)env; (void)self; klj_vk_seen("key %d", k);
}

static void *klj_vk_t_resolve(const char *sym) {
    if (!sym) return NULL;
    static const struct { const char *tail; void *fn; } t[] = {
        {"nativeVirtualKeyboardVisible", (void *)klj_vk_t_visible},
        {"nativeVirtualKeyboardChanged", (void *)klj_vk_t_changed},
        {"nativeVirtualKeyboardResult",  (void *)klj_vk_t_result},
        {"nativeVirtualKeyboardSendKey", (void *)klj_vk_t_sendkey},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (strstr(sym, t[i].tail)) return t[i].fn;
    return NULL;
}

// The whole report queue as one string, so a failure prints what actually
// happened rather than "expected 3, got 2".
static const char *klj_vk_trace(char *buf, size_t cap) {
    buf[0] = '\0';
    for (int i = 0; i < g_vk_seen.n; i++) {
        strncat(buf, i ? " | " : "", cap - strlen(buf) - 1);
        strncat(buf, g_vk_seen.log[i], cap - strlen(buf) - 1);
    }
    return buf;
}

int kl_jni_ue_keyboard_selftest(FILE *out) {
    int fails = 0, checks = 0;
    char tr[1024];
#define VK_CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fprintf(out, "  FAIL: " __VA_ARGS__); fprintf(out, "\n"); fails++; } \
    } while (0)
#define VK_TRACE(want) do { \
        checks++; \
        if (strcmp(klj_vk_trace(tr, sizeof tr), (want)) != 0) { \
            fprintf(out, "  FAIL: reported [%s]\n         expected [%s]\n", tr, (want)); \
            fails++; \
        } \
    } while (0)

    void *(*saved)(const char *) = g_guest_native;
    kl_jni_set_guest_native_resolver(klj_vk_t_resolve);
    void *act = (void *)&g_vk;          // the GameActivity handle, passed through
    klj_val a[3];

    // ---- 1. the host with no keyboard, which is every host we have ----
    // The default. A show must not simply return: the engine has already put
    // its editor up, and a report that never comes is a field that never
    // closes — a close that reports only visible(false) and a key leaves the
    // field open with a caret in it for the rest of the session, because
    // neither native reaches the widget. `result 1` is the one
    // that does, and it must land before the visible(false) that Resets the
    // pointer it dispatches over. That ORDER is the whole assertion here.
    unsetenv("KL_UE_KEYBOARD");
    unsetenv("KL_UE_KEYBOARD_TEXT");
    g_vk_seen.n = 0;
    a[0].j = 0x21;                       // TYPE_CLASS_TEXT | CAP_SENTENCES
    a[1].l = kl_jni_new_string("Enter name");
    a[2].l = kl_jni_new_string("Player");
    klj_GA_showVirtualKeyboardInput(kl_jni_env(), act, a, 3);
    VK_CHECK(g_vk_seen.n == 0,
             "the show dispatched %d native(s) before the drain — the queue is "
             "what defers them onto the pump thread", g_vk_seen.n);
    klj_drain_soft_input();
    VK_TRACE("visible 1 | changed \"Player\" | result 1 \"Player\" | visible 0");
    VK_CHECK(!klj_ue_vk_active(),
             "the field is still open after the dismissal — the engine is "
             "holding an editor nothing can close");

    // ---- 1b. KL_UE_KEYBOARD_TEXT: the same auto-close, someone's initials ----
    // The only text-entry a headset with no frontend can perform. It rides the
    // commit arm, so what it proves is that the arm carries an arbitrary string
    // to `result 1` and not just the seed.
    setenv("KL_UE_KEYBOARD_TEXT", "ACE", 1);
    g_vk_seen.n = 0;
    a[2].l = kl_jni_new_string("Player");
    klj_GA_showVirtualKeyboardInput(kl_jni_env(), act, a, 3);
    klj_drain_soft_input();
    VK_TRACE("visible 1 | changed \"ACE\" | result 1 \"ACE\" | visible 0");

    // ...and never into a password field, whatever the env says.
    g_vk_seen.n = 0;
    a[0].j = 0x21 | 0x80;                // TYPE_TEXT_VARIATION_PASSWORD
    a[2].l = kl_jni_new_string("hunter2");
    klj_GA_showVirtualKeyboardInput(kl_jni_env(), act, a, 3);
    klj_drain_soft_input();
    VK_TRACE("visible 1 | changed \"hunter2\" | result 1 \"hunter2\" | visible 0");
    a[0].j = 0x21;
    unsetenv("KL_UE_KEYBOARD_TEXT");

    // ---- 2. held open for a frontend ----
    setenv("KL_UE_KEYBOARD", "1", 1);
    g_vk_seen.n = 0;
    a[2].l = kl_jni_new_string("Player");
    klj_GA_showVirtualKeyboardInput(kl_jni_env(), act, a, 3);
    klj_drain_soft_input();
    VK_TRACE("visible 1");
    VK_CHECK(klj_ue_vk_active(), "KL_UE_KEYBOARD=1 did not hold the field open");

    // The frontend contract is kl_jni.h's, NOT this file's: a frontend has no
    // way to know which engine it is typing into, so the shared four calls must
    // reach here. This is the check that fails if the routing in
    // kl_jni_softinput.c is dropped.
    char text[256]; int secure = 1;
    VK_CHECK(kl_jni_soft_input_active() == 1,
             "kl_jni_soft_input_active() does not see a UE field — a frontend "
             "would never offer a keyboard for it");
    VK_CHECK(kl_jni_soft_input_get(text, sizeof text, &secure) == 1
             && strcmp(text, "Player") == 0,
             "the field's initial text reads \"%s\", not the guest's \"Player\"", text);
    VK_CHECK(secure == 0, "a plain text field reported itself secure");

    g_vk_seen.n = 0;
    kl_jni_soft_input_set("ACE");
    klj_drain_soft_input();
    VK_TRACE("changed \"ACE\"");

    // Commit. The Java on this path never calls the result native (grep the
    // smali: p0 and q0 are the only callers and both are the dialog's) — but
    // the NATIVE is the engine's only end-of-entry, so the commit is it.
    g_vk_seen.n = 0;
    kl_jni_soft_input_close(0);
    klj_drain_soft_input();
    VK_TRACE("changed \"ACE\" | result 1 \"ACE\" | visible 0");
    VK_CHECK(!klj_ue_vk_active(), "the field stayed open after a commit");

    // A cancellation must NOT report the edited text — committing a refused
    // field is worse than dropping a committed one. It reports the SEED,
    // because an accept of the original contents is the only refusal the
    // engine's natives can express and still let go of the widget.
    g_vk_seen.n = 0;
    klj_GA_showVirtualKeyboardInput(kl_jni_env(), act, a, 3);
    kl_jni_soft_input_set("ZZZ");
    klj_drain_soft_input();
    g_vk_seen.n = 0;
    kl_jni_soft_input_close(1);
    klj_drain_soft_input();
    VK_TRACE("changed \"Player\" | result 1 \"Player\" | visible 0");

    // ---- 3. the old AlertDialog path ----
    // Different natives, different order, and the two constants that are the
    // dialog's own rather than ours: cancel restores the ORIGINAL contents
    // before refusing, and its refusal string is a single space.
    g_vk_seen.n = 0;
    a[2].l = kl_jni_new_string("Player");
    klj_GA_showVirtualKeyboardInputDialog(kl_jni_env(), act, a, 3);
    klj_drain_soft_input();
    VK_TRACE("");
    kl_jni_soft_input_set("ACE");
    g_vk_seen.n = 0;
    kl_jni_soft_input_close(0);
    klj_drain_soft_input();
    VK_TRACE("changed \"ACE\" | result 1 \"ACE\"");

    g_vk_seen.n = 0;
    a[2].l = kl_jni_new_string("Player");
    klj_GA_showVirtualKeyboardInputDialog(kl_jni_env(), act, a, 3);
    kl_jni_soft_input_set("ZZZ");
    klj_drain_soft_input();
    g_vk_seen.n = 0;
    kl_jni_soft_input_close(1);
    klj_drain_soft_input();
    VK_TRACE("changed \"Player\" | result 0 \" \"");

    // ---- 4. the guest closing it itself ----
    g_vk_seen.n = 0;
    klj_GA_showVirtualKeyboardInput(kl_jni_env(), act, a, 3);
    klj_drain_soft_input();
    g_vk_seen.n = 0;
    klj_GA_hideVirtualKeyboardInput(kl_jni_env(), act, NULL, 0);
    klj_drain_soft_input();
    VK_TRACE("visible 0");
    VK_CHECK(!klj_ue_vk_active(), "the guest hid the keyboard and the frontend "
                                  "still thinks a field is open");

    unsetenv("KL_UE_KEYBOARD");
    kl_jni_set_guest_native_resolver(saved);
    fprintf(out, "  %d check(s), %d failure(s)\n", checks, fails);
    return fails == 0;
#undef VK_CHECK
#undef VK_TRACE
}

// ---- UE4's three BroadcastReceivers ----
//
// `VolumeReceiver`, `BatteryReceiver` and `HeadsetReceiver`: three tiny Java
// classes UE4 ships, each one a BroadcastReceiver plus a `startReceiver` /
// `stopReceiver` pair the engine calls statically, and each one carrying a
// private native the engine registers on it. GameActivity calls all three
// during onResume.
//
// Their shape is the same and it is worth naming, because it is not the shape
// of a subscription: `startReceiver` registers the filter AND THEN READS THE
// CURRENT STATE AND DISPATCHES IT IMMEDIATELY, by calling the class's own
// native. That initial dispatch is the only one that will ever happen here —
// nothing on this host broadcasts a volume change, a battery change or a
// headset plug — so refusing these calls is not "no events", it is the engine
// never learning the state at all, and each one has a consequence
// (UE4's `FAndroidMisc::GetBatteryState` and `AreHeadPhonesPluggedIn` answer
// out of exactly these).
//
// Transcribed from this APK's own smali, which is what fixes the values that
// are not ours to choose: the volume stream is STREAM_MUSIC (3) and the level
// is `AudioManager.getStreamVolume(3)`, the battery triple is
// `(status, level*100/scale, temperature)` and is dispatched only when one of
// the three CHANGED, and the headset state is the `state` extra of a
// HEADSET_PLUG Intent, which for an Intent that is not one is 0.
//
// Every value comes from the seam that already answers the same question
// elsewhere — kl_ovrp's battery, kl_jni's own stream volume — rather than a
// constant restated here, for the display-panel reason: two numbers describing
// one battery is a level that disagrees with its own charging flag.
//
// The natives are `RegisterNatives` bindings (unlike GameActivity's, which are
// static exports), so they are looked up through kl_jni_native and NOT cached:
// a cache filled by a lookup that beat RegisterNatives would pin a NULL and
// leave the seam silently dead for the run.
static void klj_ue4_receiver_dispatch(void *env, const char *cls, const char *name,
                                      const char *sig, const klj_val *args, int nargs) {
    void *fn = kl_jni_native(cls, name, sig);
    if (!fn) {
        KLJ_LOG("%s.%s: no registered native — the engine never learns this state",
                cls, name);
        return;
    }
    // Static natives take (JNIEnv *, jclass, ...). The class argument is unread
    // by all three of these bodies (they are one-line forwards into the engine),
    // and NULL is what our own jclass handles amount to anyway.
    if (nargs == 1)
        ((void (*)(void *, void *, int32_t))fn)(env, NULL, (int32_t)args[0].j);
    else
        ((void (*)(void *, void *, int32_t, int32_t, int32_t))fn)(
            env, NULL, (int32_t)args[0].j, (int32_t)args[1].j, (int32_t)args[2].j);
}

static klj_val klj_VolumeReceiver_start(void *env, void *self, const klj_val *a, int n) {
    (void)self; (void)a; (void)n;
    klj_val v = klj_AudioManager_getStreamVolume(env, NULL,
                                                 &(klj_val){.j = 3}, 1);
    KLJ_LOG("VolumeReceiver.startReceiver -> volumeChanged(%d)", (int)v.j);
    klj_ue4_receiver_dispatch(env, "com/epicgames/ue4/VolumeReceiver",
                              "volumeChanged", "(I)V", &v, 1);
    return (klj_val){.j = 0};
}

static klj_val klj_BatteryReceiver_start(void *env, void *self, const klj_val *a, int n) {
    (void)self; (void)a; (void)n;
    // BatteryManager.BATTERY_STATUS_*: CHARGING 2, DISCHARGING 3, FULL 5 — the
    // same three-way split klj_Intent_getIntExtra already makes on the sticky
    // battery Intent, from the same seam.
    int charging = kl_ovrp_battery_charging();
    int level    = kl_ovrp_battery_level();
    klj_val args[3] = {
        {.j = (uint64_t)(charging ? (level >= 100 ? 5 : 2) : 3)},
        {.j = (uint64_t)level},
        // Tenths of a degree Celsius, which is what the `temperature` extra
        // carries. kl_ovrp answers the same number to
        // ovrp_GetSystemBatteryTemperature2 in whole degrees.
        {.j = (uint64_t)(int)(kl_ovrp_battery_temperature() * 10.0f)},
    };
    KLJ_LOG("BatteryReceiver.startReceiver -> dispatchEvent(%d, %d, %d)",
            (int)args[0].j, (int)args[1].j, (int)args[2].j);
    klj_ue4_receiver_dispatch(env, "com/epicgames/ue4/BatteryReceiver",
                              "dispatchEvent", "(III)V", args, 3);
    return (klj_val){.j = 0};
}

static klj_val klj_HeadsetReceiver_start(void *env, void *self, const klj_val *a, int n) {
    (void)self; (void)a; (void)n;
    // `getIntExtra("state", 0)` on the launch Intent, which is not a
    // HEADSET_PLUG Intent — so 0, "unplugged". That is also true of this host:
    // the audio output is whatever CoreAudio has, and nothing here presents a
    // headphone jack whose plug state could change.
    klj_val v = {.j = 0};
    KLJ_LOG("HeadsetReceiver.startReceiver -> stateChanged(0) (no headphone jack)");
    klj_ue4_receiver_dispatch(env, "com/epicgames/ue4/HeadsetReceiver",
                              "stateChanged", "(I)V", &v, 1);
    return (klj_val){.j = 0};
}

// `AndroidThunkJava_PushSensorEvents` — the engine draining the activity's
// accumulated accelerometer / gyro / magnetometer / gravity readings into
// `nativeHandleSensorEvents([F[F[F[F)`, once per game tick.
//
// A no-op, and it is the guest's OWN no-op arm rather than a refusal: the body
// is `if (!sensorsRegistered) return;` and then `if (!havePending) return;`,
// and nothing here registers a sensor. A VR guest's head pose comes from
// OVRPlugin, not from these — this is the phone-orientation path — so there is
// no reading to deliver and delivering a fabricated one would be four arrays of
// invented physics arriving every frame.
static klj_val klj_GA_pushSensorEvents(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// ...and the other end of all three. Unregistering a receiver that will never
// fire is a no-op with nothing to record, but it is answered rather than
// refused because the engine calls it on every pause.
static klj_val klj_ue4_receiver_stop(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// ---- io.sentry.unreal.SentryBridgeJava ----
//
// Sentry's UE4 plugin, met first on Into the Radius (`io.sentry.sdk.name =
// sentry.java.android.unreal` in its manifest). The engine calls
// `SentryBridgeJava.init(activity, dsn, release, environment, dist)` during
// startup and then talks to it in one direction only: breadcrumbs, tags,
// contexts and a level. It is CRASH TELEMETRY — a network client aimed at the
// publisher's servers — so declining it is the answer on the merits and not
// merely a shortcut: there is no Sentry service here to be a client of, this
// runtime has no business reporting a translated guest's stack to CM Games,
// and every one of these calls is something the guest tells Sentry rather than
// something it reads back.
//
// So they are ANSWERED as no-ops rather than refused, because the engine calls
// them on paths it does not guard (init is unconditional in the plugin's
// module startup) and an abort there is an abort in the middle of a healthy
// boot — which is exactly how Into the Radius's first long host run ended.
//
// Deliberately NOT here: `captureEventWithScope` and `captureMessageWithScope`.
// Both RETURN a SentryId, both are reached only when the guest is actually
// reporting a crash or a message, and both would need an object invented to
// answer at all. Leaving them out keeps the runtime's habit — a family whose
// answer would have to be fabricated stays unimplemented and aborts by name —
// and if one ever fires, that abort is the news, not the bug.
static klj_val klj_Sentry_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("SentryBridgeJava.init() — declined: crash telemetry, and there is "
            "no Sentry service here to be a client of. The bridge answers as a "
            "no-op so the engine's unconditional init does not abort the boot.");
    return (klj_val){.j = 0};
}

// The write-only half of the same bridge: addBreadcrumb, setTag, removeTag,
// setContext, setLevel, configureScope. One handler, because they differ only
// in what they would have sent.
static klj_val klj_Sentry_sink(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// The engine's request for a render surface size. On Android the activity only
// acts on it when the SurfaceView sizing workaround is on (a device quirk we do
// not have); everything else about it is a record. Worth printing rather than
// dropping — it is the engine stating the resolution it intends to render at,
// which is the first number a graphics arc wants.
static klj_val klj_GA_setDesiredViewSize(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("AndroidThunkJava_SetDesiredViewSize(%d, %d) — recorded; the window "
            "size is kl_ndk's",
            n > 0 ? (int)(int64_t)a[0].j : 0, n > 1 ? (int)(int64_t)a[1].j : 0);
    return (klj_val){.j = 0};
}

// The system font directory, as the Java finds it: the first of /system/fonts,
// /system/font, /data/fonts that exists, with a trailing slash. A real Android
// device answers the first. Nothing of the guest's is under it here, so a font
// the engine then fails to open is a miss it reports itself — which is a better
// place to find out than a directory name we invented.
static klj_val klj_GA_getFontDirectory(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("/system/fonts/")};
}

// `AndroidThunkJava_GetSupportedNativeDisplayRefreshRates()[I` — the engine
// asking which refresh rates the panel offers, so FAndroidMisc can offer them
// to the game (Tetris Effect reads the list during HMD init, before OVRPlugin).
//
// Transcribed from Tetris Effect's own GameActivity.smali rather than
// invented: the Java walks `Display.getSupportedModes()`, keeps every mode whose
// physical width AND height match the CURRENT mode, truncates each one's
// `getRefreshRate()` to an int, and — if that leaves the list empty — returns
// `[60]`. Below API 24 it returns a one-element array unconditionally.
//
// Our display panel reports exactly one mode (klj_Display_getSupportedModes, and
// the reason it reports one is written there), so running that same filter over
// it yields exactly one rate: KLJ_DISPLAY_REFRESH. Answering the array directly
// is the same answer the smali would compute, and it stays consistent with
// getRefreshRate / getSupportedModes / the frame clock by construction — which
// is the rule that whole panel is built on. An extra entry here is a rate the
// engine may SELECT and nothing downstream would honour.
static klj_val klj_GA_getSupportedRefreshRates(void *env, void *self,
                                               const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *rates;
    if (!rates) {
        rates = klj_new_array('I', NULL, 1);
        int32_t *v = klj_arr(rates)->data;
        v[0] = (int32_t)KLJ_DISPLAY_REFRESH;
    }
    static int said;
    if (!said++)
        KLJ_LOG("AndroidThunkJava_GetSupportedNativeDisplayRefreshRates() -> [%d] "
                "(the one mode this panel has)", (int)KLJ_DISPLAY_REFRESH);
    return (klj_val){.l = rates};
}

// `AndroidThunkJava_GetDeviceOrientation()I` — one line in this APK's
// GameActivity.smali (+0x9009): `getWindowManager().getDefaultDisplay()
// .getRotation()`, returned unchanged. So it is not a separate answer at all; it
// is the display panel's rotation under another name, and it is routed at the
// same handler Unity reads so the two cannot disagree.
static klj_val klj_GA_getDeviceOrientation(void *env, void *self,
                                           const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = KLJ_DISPLAY_ROTATION};
}

// Pinball FX (UE5.3) asks the same question under 5.x's names, and both are
// one-liners in that APK's own GameActivity.smali:
//
//   GetDeviceRotation()           -> getWindowManager().getDefaultDisplay().getRotation()
//   GetConfigurationOrientation() -> getResources().getConfiguration().orientation
//
// Answered from the same constants the Display family answers from, so a guest
// that reads the rotation both ways sees one screen. Orientation is
// ORIENTATION_LANDSCAPE (2), NOT derived from KLJ_DISPLAY_W/H (taller than
// wide, which would compute PORTRAIT): the activity declares
// `android:screenOrientation="landscape"`, as every UE Android VR build does,
// and a guest told PORTRAIT transposes its viewport.
#define KLJ_ORIENTATION_LANDSCAPE 2

static klj_val klj_GA_getDeviceRotation(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = KLJ_DISPLAY_ROTATION};
}

static klj_val klj_GA_getConfigurationOrientation(void *env, void *self,
                                                  const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = KLJ_ORIENTATION_LANDSCAPE};
}

// `AndroidThunkJava_ForceQuit()V` — the engine asking the activity to end the
// process. In a UE4.27 GameActivity.smali the whole body is
// `System.exit(0)`: not finish(), not a lifecycle transition — an immediate,
// unconditional process exit, called from two sites in the same class.
//
// It is OBSERVED rather than obeyed, and that is a deliberate default for a
// bring-up: exiting here takes the host process down mid-run, which loses the
// boot log's tail and every end-of-run report (the JNI surface, the OVRPlugin
// surface, the platform surface) — i.e. exactly the evidence that would say WHY
// the guest wanted to quit. A quit has to be VISIBLE, not hidden: a silent
// one reads as a stall.
//
// So it is logged every single time — never once-only — and the fact is
// counted. `KL_UE4_FORCE_QUIT=1` obeys it and really exits, which is what a
// device build wants once a port is past bring-up.
static unsigned g_ue4_quit_requests;

// DIAGNOSTIC: name the guest code that asked. In libUE4 the C++ thunk
// `AndroidThunkCpp_ForceQuit` is reached from exactly two places —
// `FAndroidMisc::RequestExit(true)` (a tail branch) and
// `FAndroidErrorOutputDevice::Serialize` — and knowing WHICH, plus the frame
// above it, is the whole difference between "the engine finished" and "a gate
// failed". UE4's arm64 build keeps frame pointers (every prologue in the
// disassembly is `stp x29,x30; mov x29,sp`), so walking the x29 chain is
// reliable here. Off unless `KL_UE4_QUIT_TRACE=1`; the offsets are symbolised
// offline against the APK's own libUE4 dynsym, which this title ships in full.
static void ue4_quit_trace(unsigned req) {
    if (!kl_env_on("KL_UE4_QUIT_TRACE", 0)) return;
    if (req > 4) return;   // the first few are the interesting ones
    void **fp = (void **)__builtin_frame_address(0);
    for (int i = 0; i < 24 && fp; i++) {
        void *ra = fp[1];
        if (!ra) break;
        size_t off = 0;
        const char *img = kl_addr_image(ra, &off);
        KLJ_LOG("  quit-trace #%02d  %s+0x%zx", i, img ? img : "?", off);
        void **next = (void **)fp[0];
        if (next <= fp || ((uintptr_t)next & 7)) break;
        fp = next;
    }
}

static klj_val klj_GA_forceQuit(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    g_ue4_quit_requests++;
    ue4_quit_trace(g_ue4_quit_requests);
    int obey = kl_env_on("KL_UE4_FORCE_QUIT", 0);
    KLJ_LOG("AndroidThunkJava_ForceQuit() — the guest asked for System.exit(0) "
            "(request #%u). %s", g_ue4_quit_requests,
            obey ? "KL_UE4_FORCE_QUIT=1: obeying."
                 : "OBSERVED, NOT OBEYED: the run continues so the end-of-run "
                   "reports still print. KL_UE4_FORCE_QUIT=1 obeys it.");
    if (obey) { fflush(NULL); _exit(0); }

    // ...but "not obeyed" cannot mean "run on forever". A guest that has
    // decided to quit asks again and again (Into the Radius: 611 times in 19
    // seconds), and its game thread — already torn down as far as UE4 is
    // concerned — segfaults before the run reaches its own end, so the reports
    // are lost anyway, to a crash report instead of to the log.
    //
    // So the observation window is BOUNDED. The first requests are logged and
    // ignored, which is what makes the cause readable; past the limit the run
    // prints every surface it has and exits cleanly, which is strictly more
    // evidence than the segfault produced. KL_UE4_QUIT_LIMIT=0 restores the
    // old unbounded behaviour.
    unsigned limit = (unsigned)kl_env_int("KL_UE4_QUIT_LIMIT", 16);
    if (limit && g_ue4_quit_requests == limit) {
        KLJ_LOG("AndroidThunkJava_ForceQuit(): %u requests is the limit "
                "(KL_UE4_QUIT_LIMIT) — the guest is not coming back, so the "
                "run ends HERE with its reports intact rather than segfaulting "
                "inside a torn-down engine.", limit);
        kl_jni_report(stderr);
        kl_fatal_prepare();
        fflush(NULL);
        _exit(0);
    }
    return (klj_val){.j = 0};
}

// UE4's own VR gate, and the same question Unity's is: GameActivity.onCreate
// sets `PackagedForOculusMobile` from `queryIntentActivities()` for its own
// package under `com.oculus.intent.category.VR`, OR the presence of the Samsung
// VR mode <meta-data> key. Answered by asking those two, not by a constant —
// the manifest declares the category, so a match is the truth (see "Settled
// decisions"), and a guest told otherwise disables the whole path under test.
static klj_val klj_GA_isOculusMobile(void *env, void *self, const klj_val *a, int n) {
    void *list = klj_PM_queryIntentActivities(env, self, a, n).l;
    int vr = list && klj_List_size(env, list, NULL, 0).j > 0;
    if (!vr) vr = kl_jni_manifest_meta("com.samsung.android.vr.application.mode") != NULL;
    KLJ_LOG("AndroidThunkJava_IsOculusMobileApplication() -> %s", vr ? "true" : "false");
    return (klj_val){.j = (uint64_t)(vr != 0)};
}

static klj_val klj_MsgBox_new(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_object("com/epicgames/ue4/MessageBox01")};
}
static klj_val klj_MsgBox_setCaption(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    snprintf(g_ue4_msg_caption, sizeof g_ue4_msg_caption, "%s",
             n > 0 ? klj_str(a[0].l) : "");
    return (klj_val){.j = 0};
}
static klj_val klj_MsgBox_setText(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    snprintf(g_ue4_msg_text, sizeof g_ue4_msg_text, "%s",
             n > 0 ? klj_str(a[0].l) : "");
    return (klj_val){.j = 0};
}
static klj_val klj_MsgBox_addButton(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    size_t at = strlen(g_ue4_msg_buttons);
    snprintf(g_ue4_msg_buttons + at, sizeof g_ue4_msg_buttons - at, "%s[%s]",
             at ? " " : "", n > 0 ? klj_str(a[0].l) : "");
    return (klj_val){.j = 0};
}
static klj_val klj_MsgBox_clear(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    g_ue4_msg_caption[0] = g_ue4_msg_text[0] = g_ue4_msg_buttons[0] = '\0';
    return (klj_val){.j = 0};
}
static klj_val klj_MsgBox_show(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("MESSAGE BOX from the guest — \"%s\": %s   buttons: %s "
            "(answering 0, the first)",
            g_ue4_msg_caption, g_ue4_msg_text,
            g_ue4_msg_buttons[0] ? g_ue4_msg_buttons : "(none)");
    return (klj_val){.j = 0};
}

// ---- Unreal Engine 4: com.epicgames.ue4.MediaPlayer14 ----------------------
//
// UE4's Java media player, and the thing standing between RE4 and its opening
// cutscene: `AVR4CutscenePlayerPawn::StreamLoadTheatreBox` streams a level
// whose actor's BeginPlay runs `UMediaPlayer::OpenSourceLatent`, which reaches
// `FAndroidMediaPlayer` -> `FJavaAndroidMediaPlayer(bool,bool,bool)` -> `new
// MediaPlayer14(ZZZ)`. The whole surface is 33 methods, read out of that
// constructor's own GetClassMethod calls rather than from a version of UE4's
// sources — it resolves every id up front, so the ctor IS the contract.
//
// The media is real and it is ours to decode: `main.203.com.Armature.VR4.obb`
// carries four H.265 MP4s, `Stored` (so a plain byte range), the big one being
// `VR4/Content/Movies/Andy/bio4_opening_h265.mp4`. Everything else in Movies/
// is `.bk2` and goes through the guest's own Bink libraries, which is why only
// this handful ever arrives here. kl_avdec is the decoder; this file is the
// contract and knows nothing about how a frame is produced.
//
// The three constructor booleans are `swizzlePixels`, `vulkanRenderer` and
// `needTrackInfo`, and the second one decides the whole output path: a Vulkan
// guest reads frames through `getVideoLastFrameData()Ljava/nio/Buffer;` — a
// direct ByteBuffer of packed pixels — rather than through the GLES external
// texture `getExternalTextureId` names. RE4 is Vulkan, so that is the path
// implemented; `getExternalTextureId` answers 0, which is "no external texture"
// rather than a name that would be sampled and would be someone else's.
typedef struct {
    int        swizzle, vulkan, need_track_info;
    char       source[1024];     // what the guest asked for, verbatim
    kl_avdec  *dec;
    int        video_enabled, audio_enabled;
    float      volume;
    int        looping;
    int        started;          // start() seen; distinct from the decoder running
    void      *buffer;           // the java.nio.Buffer handed out, cached
    unsigned long long buf_serial;
    int        last_w, last_h;   // for didResolutionChange
    int        res_changed;
} klj_media;

static klj_media *klj_media_of(void *self) {
    klj_object *o = klj_as_object(self);
    return (o && strcmp(o->cls, "com/epicgames/ue4/MediaPlayer14") == 0) ? o->data : NULL;
}

// Resolve what the guest named to a host file plus a byte range.
//
// Three of the four setDataSource forms carry an explicit (offset, length) into
// a container, because on Android the movie lives inside the OBB; the URL form
// carries a path on its own. Both end up here so there is one place that
// decides what file is opened — two would be two chances to disagree about
// which container an offset is into.
static void klj_media_open(klj_media *m, const char *what, long long off,
                           long long len) {
    if (!m) return;
    if (m->dec) { kl_avdec_close(m->dec); m->dec = NULL; }
    snprintf(m->source, sizeof m->source, "%s", what ? what : "");

    // `file://` and a bare path are the same thing to us; UE4 hands over both
    // depending on which MediaSource built the URL.
    const char *p = m->source;
    if (strncmp(p, "file://", 7) == 0) p += 7;
    m->dec = kl_avdec_open(p, off, len);
    if (!m->dec)
        KLJ_LOG("MediaPlayer14: could not open %s (offset %lld, %lld bytes) — "
                "setDataSource answers false and the guest's own no-media path "
                "runs", p, off, len);
}

static klj_val klj_MP14_ctor(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    klj_media *m = calloc(1, sizeof *m);
    if (!m) return (klj_val){.l = NULL};
    m->swizzle         = n > 0 && a[0].j != 0;
    m->vulkan          = n > 1 && a[1].j != 0;
    m->need_track_info = n > 2 && a[2].j != 0;
    m->video_enabled = m->audio_enabled = 1;
    m->volume = 1.0f;
    // Printed once per player and worth it: `vulkan` selects between two
    // completely different ways of handing frames back, and getting it wrong
    // is a black movie with every call succeeding.
    KLJ_LOG("MediaPlayer14(swizzlePixels=%d, vulkanRenderer=%d, needTrackInfo=%d)",
            m->swizzle, m->vulkan, m->need_track_info);
    return (klj_val){.l = klj_new_object_data("com/epicgames/ue4/MediaPlayer14", m)};
}

// The four data sources. Each answers Z — true only if a decoder really opened.
static klj_val klj_MP14_setDataSourceURL(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    klj_media_open(m, n > 0 ? klj_str(a[0].l) : NULL, 0, 0);
    return (klj_val){.j = m && m->dec ? 1 : 0};
}
// (String path, long offset, long size) — the OBB form with the container named.
static klj_val klj_MP14_setDataSourceRange(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    klj_media_open(m, n > 0 ? klj_str(a[0].l) : NULL,
                   n > 1 ? (long long)a[1].j : 0, n > 2 ? (long long)a[2].j : 0);
    return (klj_val){.j = m && m->dec ? 1 : 0};
}
// (AssetManager, String name, long offset, long size) — the same, addressed
// through the asset manager. The AssetManager argument is dropped deliberately:
// kl_jni resolves an asset name against the unpacked tree already, and a second
// resolution here would be a second answer to "where do assets live".
static klj_val klj_MP14_setDataSourceAsset(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    const char *name = n > 1 ? klj_str(a[1].l) : NULL;
    char path[1024];
    if (name && name[0] != '/') {
        snprintf(path, sizeof path, "%s/%s", g_assets_dir, name);
        name = path;
    }
    klj_media_open(m, name, n > 2 ? (long long)a[2].j : 0,
                   n > 3 ? (long long)a[3].j : 0);
    return (klj_val){.j = m && m->dec ? 1 : 0};
}
// (long, long) — a range in an archive the guest already has open, handed over
// as raw numbers with no container named at all. There is nothing here to
// resolve them against, so it is refused BY NAME rather than guessed at: an
// offset into an unknown file is the one input that cannot be honoured.
static klj_val klj_MP14_setDataSourceArchive(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("MediaPlayer14.setDataSourceArchive(%lld, %lld) — refused: the guest "
            "names an offset and a length with no container, and this shim has "
            "no handle on the archive it means",
            n > 0 ? (long long)a[0].j : 0, n > 1 ? (long long)a[1].j : 0);
    return (klj_val){.j = 0};
}

static klj_val klj_MP14_prepare(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n; (void)self;
    return (klj_val){.j = 0};      // the open already did the work
}
static klj_val klj_MP14_start(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (m) { m->started = 1; kl_avdec_play(m->dec, 1); }
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_pause(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (m) kl_avdec_play(m->dec, 0);
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_stop(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (m) { m->started = 0; kl_avdec_play(m->dec, 0); kl_avdec_seek(m->dec, 0); }
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_reset(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (m && m->dec) { kl_avdec_close(m->dec); m->dec = NULL; m->started = 0; }
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_release(void *env, void *self, const klj_val *a, int n) {
    return klj_MP14_reset(env, self, a, n);
}
static klj_val klj_MP14_seekTo(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    if (m) kl_avdec_seek(m->dec, n > 0 ? (int32_t)a[0].j : 0);
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_setLooping(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    if (m) { m->looping = n > 0 && a[0].j != 0; kl_avdec_set_looping(m->dec, m->looping); }
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_isLooping(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = m && m->looping};
}
static klj_val klj_MP14_isPlaying(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = m && kl_avdec_playing(m->dec)};
}
// "Prepared" is "a decoder is open on real media", which is the same condition
// setDataSource answered — there is no asynchronous preparation here because
// kl_avdec_open has already read the asset by the time it returns.
static klj_val klj_MP14_isPrepared(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = m && m->dec != NULL};
}
static klj_val klj_MP14_didComplete(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = m && kl_avdec_complete(m->dec)};
}
static klj_val klj_MP14_getDuration(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = (uint64_t)(uint32_t)(m ? kl_avdec_duration_ms(m->dec) : 0)};
}
static klj_val klj_MP14_getCurrentPosition(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = (uint64_t)(uint32_t)(m ? kl_avdec_position_ms(m->dec) : 0)};
}
static klj_val klj_MP14_getVideoWidth(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = (uint64_t)(uint32_t)(m ? kl_avdec_width(m->dec) : 0)};
}
static klj_val klj_MP14_getVideoHeight(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    return (klj_val){.j = (uint64_t)(uint32_t)(m ? kl_avdec_height(m->dec) : 0)};
}
static klj_val klj_MP14_setVideoEnabled(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    if (m) m->video_enabled = n > 0 && a[0].j != 0;
    return (klj_val){.j = 0};
}
// Recorded rather than applied: nothing here decodes the audio track yet, so
// honouring the volume would be describing a mix that does not exist. Said
// once, so a silent cutscene is a known gap rather than a mystery.
static klj_val klj_MP14_setAudioEnabled(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    if (m) m->audio_enabled = n > 0 && a[0].j != 0;
    static int said;
    if (m && m->audio_enabled && !said) {
        said = 1;
        KLJ_LOG("MediaPlayer14: audio is enabled by the guest and this player "
                "decodes VIDEO only — the cutscene will be silent");
    }
    return (klj_val){.j = 0};
}
static klj_val klj_MP14_setAudioVolume(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    klj_media *m = klj_media_of(self);
    if (m && n > 0) memcpy(&m->volume, &a[0].j, sizeof m->volume);
    return (klj_val){.j = 0};
}

// The frame, as a direct ByteBuffer over the decoder's own storage.
//
// The buffer object is cached per player: UE4 calls this every frame it wants a
// picture, and a fresh jobject per call is garbage the guest's local frame has
// to retire. The ADDRESS behind it is stable for the life of the decoder — the
// frame buffer is reallocated only when the dimensions change, which is why the
// cache is dropped when they do.
static klj_val klj_MP14_getVideoLastFrameData(void *env, void *self,
                                              const klj_val *a, int n) {
    (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (!m || !m->dec) return (klj_val){.l = NULL};
    int w = 0, h = 0;
    size_t bytes = 0;
    unsigned long long serial = 0;
    const void *px = kl_avdec_frame(m->dec, &w, &h, &bytes, &serial);
    if (!px || !bytes) return (klj_val){.l = NULL};
    if (w != m->last_w || h != m->last_h) {
        m->res_changed = 1;
        m->last_w = w; m->last_h = h;
        m->buffer = NULL;
    }
    if (!m->buffer)
        m->buffer = klj_own(klj_NewDirectByteBuffer(env, (void *)px, (int64_t)bytes),
                            NULL);
    m->buf_serial = serial;
    return (klj_val){.l = m->buffer};
}

// (int) -> Z: "is there a frame at all". The int is the external texture the
// GLES path would have rendered into and is not one here.
static klj_val klj_MP14_getVideoLastFrame(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (!m || !m->dec) return (klj_val){.j = 0};
    unsigned long long serial = 0;
    const void *px = kl_avdec_frame(m->dec, NULL, NULL, NULL, &serial);
    return (klj_val){.j = px != NULL && serial != 0};
}

// Latched and cleared by the read, which is what "did it change" means — a
// flag that stayed set would make UE4 rebuild its texture every frame.
static klj_val klj_MP14_didResolutionChange(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_media *m = klj_media_of(self);
    if (!m) return (klj_val){.j = 0};
    int c = m->res_changed;
    m->res_changed = 0;
    return (klj_val){.j = c != 0};
}

// 0 = there is no external texture. The GLES path binds this name and samples
// it; answering anything else would name a texture belonging to the guest's own
// renderer, which is worse than having none.
static klj_val klj_MP14_getExternalTextureId(void *env, void *self,
                                             const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// NULL = "nothing new this tick", which is the answer the GLES path is built to
// handle. It is reached only by a guest that took the external-texture route,
// and RE4 does not.
static klj_val klj_MP14_updateVideoFrame(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static int said;
    if (!said) {
        said = 1;
        KLJ_LOG("MediaPlayer14.updateVideoFrame — the GLES external-texture path, "
                "which this player does not implement (frames come out of "
                "getVideoLastFrameData). Answering null.");
    }
    return (klj_val){.l = NULL};
}

// Empty track lists. `needTrackInfo` is the ctor's third bool and UE4 only asks
// when it is set; an empty array is "this player reports no selectable tracks",
// which is true — nothing here exposes alternates — and is distinguishable from
// the refusal an absent binding would be.
static klj_val klj_MP14_tracks_audio(void *env, void *self, const klj_val *a, int n) {
    (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_array('L',
        "com/epicgames/ue4/MediaPlayer14$AudioTrackInfo", 0)};
}
static klj_val klj_MP14_tracks_caption(void *env, void *self, const klj_val *a, int n) {
    (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_array('L',
        "com/epicgames/ue4/MediaPlayer14$CaptionTrackInfo", 0)};
}
static klj_val klj_MP14_tracks_video(void *env, void *self, const klj_val *a, int n) {
    (void)self; (void)a; (void)n;
    return (klj_val){.l = klj_new_array('L',
        "com/epicgames/ue4/MediaPlayer14$VideoTrackInfo", 0)};
}
static klj_val klj_MP14_selectTrack(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// AndroidThunkJava_GetSharedPreferenceString(name, key, default) — UE's own
// convenience over Android SharedPreferences. On a fresh Klepton install there
// is nothing stored, so the honest answer is the caller's default (the last
// argument); returning it is what Android returns for an absent key anyway, and
// it is enough to get a UE5 title (OLAR) past the settings read it does at boot
// instead of aborting on an unimplemented method.
static klj_val klj_GA_getSharedPreferenceString(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *def = (n > 0 && a[n - 1].l) ? klj_str(a[n - 1].l) : "";
    return (klj_val){.l = kl_jni_new_string(def ? def : "")};
}


// A GameActivity void method we deliberately do nothing for. OLAR registers a
// network-reachability listener; there is no Android connectivity service here
// and the engine treats "never notified" as "no change," which is correct for a
// device that is simply online.
static klj_val klj_GA_void_noop(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = NULL};
}

// The Electra H264 decoder (com/epicgames/unreal/ElectraVideoDecoderH264) that
// wanderer drives for its intro video is a whole family of its own — see
// runtime/jni/kl_jni_electra.c, which wires it to kl_vtdec. It used to be a
// single GetDecoderInformation stub returning NULL here to skip the video.

// ---------------------------------------------------------------------------
// UE4.27's post-init GameActivity surface
// ---------------------------------------------------------------------------
// The 28 GameActivity thunks above are the ones an Unreal guest reaches on the
// way to `FEngineLoop::Init()`. A title that gets past it (Five Nights at
// Freddy's: Help Wanted 2 logs `Engine is initialized` and loads its splash
// map) opens JNI doors nothing here had bound.
//
// Rather than bind one name per failed run, the whole reachable set was
// enumerated statically: every `AndroidThunkJava_*` that APK's GameActivity.smali declares,
// intersected with the name strings actually present in libUE4.so (a thunk the
// engine never looks up cannot abort), minus what is already bound above. What
// is left is implemented here, by family.
//
// **The Ads family is bound too, and the reason is a property of THIS runtime
// rather than of the guest.** Those eight thunks (ShowAdBanner,
// LoadInterstitialAd, GetAdvertisingId and siblings) are in libUE4 but have NO
// Java counterpart in this APK's GameActivity — the smali has zero occurrences.
// On a real device UE4's FJavaWrapper::FindMethod would get NULL back and the
// engine's `if (Method)` guard would skip the whole path. Here, method lookups
// NEVER fail by design (kl_jni.h: only a CALL proves something is needed), so
// the guest is handed eight valid-looking jmethodIDs and the guard cannot fire. What is a dead branch on Android
// is a live abort here. Binding them is not gold-plating; it is repairing a
// hole this runtime's own lookup policy opens.
//
// The smali-only names (GetCommandLine, CookieManager_*, ResizeKeyboard) are
// genuinely unreachable in the other direction — no native code looks them up —
// and are left alone.

// --- Force feedback ------------------------------------------------------
// FAndroidInputInterface::SetForceFeedbackChannelValues calls this the first
// time any force-feedback effect plays, which for a VR title is the first menu
// interaction. There is no device vibrator on this host — Vision Pro has none,
// and CONTROLLER haptics are a different seam entirely (ovrp_SetControllerHaptics,
// which is implemented and drives the real thing). So this is the Android
// phone-body vibrator being asked for, and doing nothing is the truthful answer.
static klj_val klj_GA_vibrate(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    static int said;
    if (!said++)
        KLJ_LOG("AndroidThunkJava_Vibrate(%d ms) -> no device vibrator on this "
                "host (controller haptics go through ovrp_SetControllerHaptics)",
                n > 0 ? (int)(int64_t)a[0].j : 0);
    return (klj_val){.j = 0};
}

// --- Input device enumeration --------------------------------------------
// FAndroidInputInterface asks about any device id it has not seen before, and
// uses the name/descriptor to look the device up in the engine's controller
// mapping. The returned object's FIELDS are read immediately afterwards, so the
// entries in g_fields (kl_jni.c) are half of this answer — returning the object
// without them just moves the abort one line down to "no host value for field".
//
// The values describe what this runtime actually presents: Touch controllers,
// which is what ovrp_GetConnectedControllers2 reports and what
// fill_controller_state populates. A vendor/product of 0 is what the real
// GameActivity returns for a device whose InputDevice lookup fails, and it is
// what we are — there is no Android InputDevice behind this.
static klj_val klj_GA_getInputDeviceInfo(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    static int said;
    if (!said++)
        KLJ_LOG("AndroidThunkJava_GetInputDeviceInfo(%d) -> the synthetic Touch "
                "pair (see g_fields for the field values)",
                n > 0 ? (int)(int64_t)a[0].j : 0);
    return (klj_val){.l = kl_jni_new_object("com/epicgames/ue4/GameActivity$InputDeviceInfo")};
}

// --- Local notifications --------------------------------------------------
// libUE4 carries UPlatformGameInstance's PlatformReceivedLocalNotification
// delegate, so the game instance polls the launch notification right after
// engine init — exactly the window this launch enters.
//
// The object is returned rather than NULL because UE4.27's
// AndroidThunkCpp_GetLaunchNotification does NOT null-check before reading the
// three fields off it. `used` is false: this app was launched from the Home
// View, not from a notification, which is both true and the answer that keeps
// the game out of a notification-handling path with nothing behind it.
static klj_val klj_GA_getLaunchNotification(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static int said;
    if (!said++)
        KLJ_LOG("AndroidThunkJava_LocalNotificationGetLaunchNotification() -> "
                "used=false (this app was launched from the Home View)");
    return (klj_val){.l = kl_jni_new_object("com/epicgames/ue4/GameActivity$LaunchNotification")};
}
// Scheduling one returns the notification's id; 0 is "not scheduled". There is
// no Android AlarmManager here and no notification shade to post into, so the
// whole family agrees that nothing is ever scheduled and nothing exists.
static klj_val klj_GA_notifySchedule(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a;
    static int said;
    if (!said++)
        KLJ_LOG("AndroidThunkJava_LocalNotificationScheduleAtTime(%d args) -> 0 "
                "(no notification service on this host)", n);
    return (klj_val){.j = 0};
}

// --- The virtual keyboard -------------------------------------------------
// Answered further up (klj_GA_showVirtualKeyboardInput & co.: the host's
// soft-input seam, the result delivered through nativeVirtualKeyboardResult).

// Whether a click at (x, y) should be swallowed by the keyboard's own view.
// There is no keyboard view, so nothing is ever swallowed — a "yes" here eats
// the guest's pointer input.
static klj_val klj_GA_virtualInputIgnoreClick(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// --- Device identity and connectivity -------------------------------------
// FPlatformMisc::GetDeviceId / GetLoginId: AndroidThunkJava_GetAndroidId is
// answered further up (klj_GA_getAndroidId, KLJ_ANDROID_ID — one id shared with
// Settings.Secure.getString, so one device cannot claim to be two): stable,
// non-empty and synthetic, which is what the engine needs.
// UE4's EAndroidNetworkConnectionType: Unknown 0, None 1, AirplaneMode 2,
// Cell 3, WiFi 4, WiMAX 5, Bluetooth 6, Ethernet 7. WiFi, because this device
// genuinely is on one and "None" sends the engine down an offline path.
static klj_val klj_GA_getNetworkConnectionType(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 4};
}

// --- Orientation ----------------------------------------------------------
// A head-mounted display has no rotatable panel. SetOrientation is a request we
// cannot honour and must not pretend to. GetDeviceOrientation is answered
// further up (the panel rotation, routed at the handler Unity reads).
static klj_val klj_GA_setOrientation(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    static int said;
    if (!said++)
        KLJ_LOG("AndroidThunkJava_SetOrientation(%d) -> recorded, not applied "
                "(an HMD has no rotatable panel)", n > 0 ? (int)(int64_t)a[0].j : 0);
    return (klj_val){.j = 0};
}

// --- The clipboard --------------------------------------------------------
// A real host clipboard exists on visionOS but is the USER's, shared with every
// other app; a guest that copies into it and a guest that reads it are two
// different risks, and neither is on any path this port needs. Copy is dropped
// and Paste answers the empty string — never NULL, because UE4 wraps the result
// in an FString without checking.
static klj_val klj_GA_clipboardCopy(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static int said;
    if (!said++)
        KLJ_LOG("AndroidThunkJava_ClipboardCopy -> dropped (the host clipboard is "
                "the user's, not the guest's)");
    return (klj_val){.j = 0};
}
static klj_val klj_GA_clipboardPaste(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("")};
}

// The empty string, for the String-returning thunks whose answer is "there is
// nothing here". Never NULL: UE4 wraps these results in an FString without
// checking, and a null jstring reaches FJavaHelper as a dereference.
static klj_val klj_GA_empty_string(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_new_string("")};
}

// --- Things this process will not do to itself ----------------------------
// Minimize, restart, launch another package, open a URL, share, broadcast. On
// visionOS the app does not control its own window lifecycle and there is no
// package manager to ask, so every one of these is accepted and dropped, and the
// predicates answer no. Notably Minimize: KL_EXIT_ON_BACKGROUND=0 is baked for
// UE4 targets precisely so that backgrounding does NOT kill a nine-gigabyte
// load, and a guest minimising itself would defeat that from the inside.
// (klj_GA_void_noop, defined above, is the accept-and-drop answer.)
static klj_val klj_GA_false(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}
static klj_val klj_GA_zero_int(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}
// Screen capture: nothing here records the display, so "not disabled" is true
// and the setter has nothing to set. The pair must agree — a guest that disables
// capture and then reads back "enabled" concludes the call failed.
static int g_ue4_screen_capture_disabled;
static klj_val klj_GA_disableScreenCapture(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    g_ue4_screen_capture_disabled = (n > 0 && a[0].j) ? 1 : 0;
    return (klj_val){.j = 0};
}
static klj_val klj_GA_isScreenCaptureDisabled(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = (uint64_t)g_ue4_screen_capture_disabled};
}

const klj_binding klj_bind_ue4[] = {
    {"android/view/Display", "getRefreshRate", "()F", klj_Display_getRefreshRate},
    {"android/view/Display", "getAppVsyncOffsetNanos",        "()J", klj_Display_getAppVsyncOffsetNanos},
    {"android/view/Display", "getPresentationDeadlineNanos",  "()J", klj_Display_getPresentationDeadlineNanos},

    // ---- Unreal Engine 4: com.epicgames.ue4.GameActivity ----
    //
    // UE4's engine-to-Java surface. Every one of these is a method the ENGINE
    // calls on its own activity — `AndroidThunkJava_*` is Epic's naming for
    // exactly that direction — so unlike the Android framework above, the
    // authority for what each one does is this APK's GameActivity.smali rather
    // than the platform.
    //
    // They are on `com/epicgames/ue4/GameActivity` and not on a superclass
    // because the guest looks the class up by that name (kl_ue4.c sets it as
    // the activity class for the same reason).
    // com.epicgames.ue4.MediaPlayer14 — UE4's Java media player. The names and
    // signatures are FJavaAndroidMediaPlayer's own GetClassMethod calls, in the
    // order that constructor resolves them.
    {"com/epicgames/ue4/MediaPlayer14", "<init>", "(ZZZ)V", klj_MP14_ctor},
    {"com/epicgames/ue4/MediaPlayer14", "getDuration", "()I", klj_MP14_getDuration},
    {"com/epicgames/ue4/MediaPlayer14", "reset", "()V", klj_MP14_reset},
    {"com/epicgames/ue4/MediaPlayer14", "stop", "()V", klj_MP14_stop},
    {"com/epicgames/ue4/MediaPlayer14", "getCurrentPosition", "()I",
     klj_MP14_getCurrentPosition},
    {"com/epicgames/ue4/MediaPlayer14", "isLooping", "()Z", klj_MP14_isLooping},
    {"com/epicgames/ue4/MediaPlayer14", "isPlaying", "()Z", klj_MP14_isPlaying},
    {"com/epicgames/ue4/MediaPlayer14", "isPrepared", "()Z", klj_MP14_isPrepared},
    {"com/epicgames/ue4/MediaPlayer14", "didComplete", "()Z", klj_MP14_didComplete},
    {"com/epicgames/ue4/MediaPlayer14", "setDataSourceURL",
     "(Ljava/lang/String;)Z", klj_MP14_setDataSourceURL},
    {"com/epicgames/ue4/MediaPlayer14", "setDataSourceArchive", "(JJ)Z",
     klj_MP14_setDataSourceArchive},
    {"com/epicgames/ue4/MediaPlayer14", "setDataSource", "(Ljava/lang/String;JJ)Z",
     klj_MP14_setDataSourceRange},
    {"com/epicgames/ue4/MediaPlayer14", "setDataSource",
     "(Landroid/content/res/AssetManager;Ljava/lang/String;JJ)Z",
     klj_MP14_setDataSourceAsset},
    {"com/epicgames/ue4/MediaPlayer14", "prepare", "()V", klj_MP14_prepare},
    {"com/epicgames/ue4/MediaPlayer14", "prepareAsync", "()V", klj_MP14_prepare},
    {"com/epicgames/ue4/MediaPlayer14", "seekTo", "(I)V", klj_MP14_seekTo},
    {"com/epicgames/ue4/MediaPlayer14", "setLooping", "(Z)V", klj_MP14_setLooping},
    {"com/epicgames/ue4/MediaPlayer14", "release", "()V", klj_MP14_release},
    {"com/epicgames/ue4/MediaPlayer14", "getVideoHeight", "()I", klj_MP14_getVideoHeight},
    {"com/epicgames/ue4/MediaPlayer14", "getVideoWidth", "()I", klj_MP14_getVideoWidth},
    {"com/epicgames/ue4/MediaPlayer14", "setVideoEnabled", "(Z)V", klj_MP14_setVideoEnabled},
    {"com/epicgames/ue4/MediaPlayer14", "setAudioEnabled", "(Z)V", klj_MP14_setAudioEnabled},
    {"com/epicgames/ue4/MediaPlayer14", "setAudioVolume", "(F)V", klj_MP14_setAudioVolume},
    {"com/epicgames/ue4/MediaPlayer14", "getVideoLastFrameData", "()Ljava/nio/Buffer;",
     klj_MP14_getVideoLastFrameData},
    {"com/epicgames/ue4/MediaPlayer14", "start", "()V", klj_MP14_start},
    {"com/epicgames/ue4/MediaPlayer14", "pause", "()V", klj_MP14_pause},
    {"com/epicgames/ue4/MediaPlayer14", "getVideoLastFrame", "(I)Z",
     klj_MP14_getVideoLastFrame},
    {"com/epicgames/ue4/MediaPlayer14", "GetAudioTracks",
     "()[Lcom/epicgames/ue4/MediaPlayer14$AudioTrackInfo;", klj_MP14_tracks_audio},
    {"com/epicgames/ue4/MediaPlayer14", "GetCaptionTracks",
     "()[Lcom/epicgames/ue4/MediaPlayer14$CaptionTrackInfo;", klj_MP14_tracks_caption},
    {"com/epicgames/ue4/MediaPlayer14", "GetVideoTracks",
     "()[Lcom/epicgames/ue4/MediaPlayer14$VideoTrackInfo;", klj_MP14_tracks_video},
    {"com/epicgames/ue4/MediaPlayer14", "didResolutionChange", "()Z",
     klj_MP14_didResolutionChange},
    {"com/epicgames/ue4/MediaPlayer14", "getExternalTextureId", "()I",
     klj_MP14_getExternalTextureId},
    {"com/epicgames/ue4/MediaPlayer14", "updateVideoFrame",
     "(I)Lcom/epicgames/ue4/MediaPlayer14$FrameUpdateInfo;", klj_MP14_updateVideoFrame},
    {"com/epicgames/ue4/MediaPlayer14", "selectTrack", "(I)V", klj_MP14_selectTrack},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IsOculusMobileApplication",
     "()Z", klj_GA_isOculusMobile},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_HasMetaDataKey",
     "(Ljava/lang/String;)Z", klj_GA_hasMetaDataKey},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetMetaDataBoolean",
     "(Ljava/lang/String;)Z", klj_GA_getMetaDataBoolean},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetMetaDataInt",
     "(Ljava/lang/String;)I", klj_GA_getMetaDataInt},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetMetaDataLong",
     "(Ljava/lang/String;)J", klj_GA_getMetaDataLong},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetMetaDataFloat",
     "(Ljava/lang/String;)F", klj_GA_getMetaDataFloat},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetMetaDataString",
     "(Ljava/lang/String;)Ljava/lang/String;", klj_GA_getMetaDataString},
    // UE4.27's additions — see the implementations above.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetIntentExtrasString",
     "(Ljava/lang/String;)Ljava/lang/String;", klj_GA_getIntentExtrasString},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetIntentExtrasBoolean",
     "(Ljava/lang/String;)Z", klj_GA_getIntentExtrasBoolean},
    // UE5.3's addition (Pinball FX VR) — asked once the terms screens are accepted.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetAndroidId",
     "()Ljava/lang/String;", klj_GA_getAndroidId},
    // The virtual keyboard, both paths. See the section above: the engine picks
    // between them with `Android.NewKeyboard`, so a guest that has one has the
    // other one frame later, and implementing only the one this title happened
    // to call is a second abort waiting for the cvar to change.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_ShowVirtualKeyboardInput",
     "(ILjava/lang/String;Ljava/lang/String;)V", klj_GA_showVirtualKeyboardInput},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_HideVirtualKeyboardInput",
     "()V", klj_GA_hideVirtualKeyboardInput},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_ShowVirtualKeyboardInputDialog",
     "(ILjava/lang/String;Ljava/lang/String;)V", klj_GA_showVirtualKeyboardInputDialog},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_HideVirtualKeyboardInputDialog",
     "()V", klj_GA_hideVirtualKeyboardInputDialog},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetNativeDisplayRefreshRate",
     "()I", klj_GA_getNativeRefreshRate},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_SetNativeDisplayRefreshRate",
     "(I)Z", klj_GA_setNativeRefreshRate},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_InitHMDs", "()V", klj_GA_initHMDs},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_ForceQuit", "()V",
     klj_GA_forceQuit},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetDeviceOrientation",
     "()I", klj_GA_getDeviceOrientation},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetDeviceRotation",
     "()I", klj_GA_getDeviceRotation},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetConfigurationOrientation",
     "()I", klj_GA_getConfigurationOrientation},
    {"com/epicgames/ue4/GameActivity",
     "AndroidThunkJava_GetSupportedNativeDisplayRefreshRates", "()[I",
     klj_GA_getSupportedRefreshRates},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_SetDesiredViewSize", "(II)V",
     klj_GA_setDesiredViewSize},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetFontDirectory",
     "()Ljava/lang/String;", klj_GA_getFontDirectory},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_AddNetworkListener",
     "()V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_InitializeWithPermission",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IsBhapticsAvailable",
     "()Z", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_EnableMotion",
     "(Z)V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetSharedPreferenceString",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
     klj_GA_getSharedPreferenceString},
    {"io/sentry/unreal/SentryBridgeJava", "init",
     "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
     klj_Sentry_init},
    {"io/sentry/unreal/SentryBridgeJava", "setContext",
     "(Ljava/lang/String;Ljava/util/HashMap;)V", klj_Sentry_sink},
    {"io/sentry/unreal/SentryBridgeJava", "setTag",
     "(Ljava/lang/String;Ljava/lang/String;)V", klj_Sentry_sink},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_PushSensorEvents", "()V",
     klj_GA_pushSensorEvents},
    // UE4's three BroadcastReceivers. `startReceiver` is where the state is
    // actually delivered — see klj_ue4_receiver_dispatch.
    {"com/epicgames/ue4/VolumeReceiver", "startReceiver", "(Landroid/app/Activity;)V",
     klj_VolumeReceiver_start},
    {"com/epicgames/ue4/VolumeReceiver", "stopReceiver", "(Landroid/app/Activity;)V",
     klj_ue4_receiver_stop},
    {"com/epicgames/ue4/BatteryReceiver", "startReceiver", "(Landroid/app/Activity;)V",
     klj_BatteryReceiver_start},
    {"com/epicgames/ue4/BatteryReceiver", "stopReceiver", "(Landroid/app/Activity;)V",
     klj_ue4_receiver_stop},
    {"com/epicgames/ue4/HeadsetReceiver", "startReceiver", "(Landroid/app/Activity;)V",
     klj_HeadsetReceiver_start},
    {"com/epicgames/ue4/HeadsetReceiver", "stopReceiver", "(Landroid/app/Activity;)V",
     klj_ue4_receiver_stop},
    // Sentry's UE4 bridge — see klj_Sentry_init for why these are answered and
    // why the two capture* entry points are not here.
    {"io/sentry/unreal/SentryBridgeJava", "addBreadcrumb",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/util/HashMap;Lio/sentry/SentryLevel;)V",
     klj_Sentry_sink},
    {"io/sentry/unreal/SentryBridgeJava", "configureScope", "(J)V", klj_Sentry_sink},
    {"io/sentry/unreal/SentryBridgeJava", "removeTag",
     "(Ljava/lang/String;)V", klj_Sentry_sink},
    {"io/sentry/unreal/SentryBridgeJava", "setLevel",
     "(Lio/sentry/SentryLevel;)V", klj_Sentry_sink},
    // ---- The post-FEngineLoop::Init surface. See the block above. ----
    // Input and force feedback.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_Vibrate", "(I)V", klj_GA_vibrate},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetInputDeviceInfo",
     "(I)Lcom/epicgames/ue4/GameActivity$InputDeviceInfo;", klj_GA_getInputDeviceInfo},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_VirtualInputIgnoreClick",
     "(II)Z", klj_GA_virtualInputIgnoreClick},
    // Local notifications — the whole family, because GetLaunchNotification is
    // polled right after engine init and the rest follow from whatever it says.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_LocalNotificationGetLaunchNotification",
     "()Lcom/epicgames/ue4/GameActivity$LaunchNotification;", klj_GA_getLaunchNotification},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_LocalNotificationScheduleAtTime",
     "(Ljava/lang/String;ZLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",
     klj_GA_notifySchedule},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_LocalNotificationExists",
     "(I)Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_LocalNotificationDestroyIfExists",
     "(I)Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_LocalNotificationClearAll",
     "()V", klj_GA_void_noop},
    // Remote notifications: there is no push service behind this app.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IsAllowedRemoteNotifications",
     "()Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_RegisterForRemoteNotifications",
     "()V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_UnregisterForRemoteNotifications",
     "()V", klj_GA_void_noop},
    // Identity and connectivity.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetNetworkConnectionType",
     "()I", klj_GA_getNetworkConnectionType},
    // Orientation — an HMD has no rotatable panel.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_SetOrientation",
     "(I)V", klj_GA_setOrientation},
    // Clipboard.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_ClipboardCopy",
     "(Ljava/lang/String;)V", klj_GA_clipboardCopy},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_ClipboardPaste",
     "()Ljava/lang/String;", klj_GA_clipboardPaste},
    // Screen capture — a recorded setter/getter pair, so the two agree.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_DisableScreenCapture",
     "(Z)V", klj_GA_disableScreenCapture},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IsScreenCaptureDisabled",
     "()Z", klj_GA_isScreenCaptureDisabled},
    // The launch intent's remaining accessors. The String form is bound above
    // and answers null; these two must agree that there are no extras at all.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetIntentExtrasInt",
     "(Ljava/lang/String;)I", klj_GA_zero_int},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_HasIntentExtrasKey",
     "(Ljava/lang/String;)Z", klj_GA_false},
    // Things this process will not do to itself or to the system.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_Minimize", "()V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_RestartApplication",
     "(Ljava/lang/String;)V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_LaunchURL",
     "(Ljava/lang/String;)V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_ShareURL",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;II)V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_SendBroadcast",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IsPackageInstalled",
     "(Ljava/lang/String;)Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_LaunchPackage",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_ShowConsoleWindow",
     "(Ljava/lang/String;)V", klj_GA_void_noop},
    // The OBB downloader's progress dialog. The OBBs are staged and mounted, so
    // nothing reaches the downloader — but ShowProgressDialog is also what a
    // GooglePAD asset-pack fetch would raise, and that path exists in this build.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_ShowProgressDialog",
     "(ZLjava/lang/String;ZI)V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_UpdateProgressDialog",
     "(I)V", klj_GA_void_noop},
    // Play Asset Delivery. libplaycore is DT_NEEDED and the GooglePAD module is
    // linked in, but every asset these titles need is in their OBBs — so "not
    // available" is true and keeps the engine on the OBB path it already used.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GooglePAD_Available",
     "()Z", klj_GA_false},
    // Store and accounts. The online subsystem on a Quest build is typically
    // GooglePlay falling back to NULL, so none of this is reachable through the
    // OSS — but a Blueprint can call the IAP nodes directly, and "purchases are
    // not allowed here" is both true and the answer that stops the sequence
    // rather than starting one nothing can finish.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IapSetupService",
     "(Ljava/lang/String;)V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IapIsAllowedToMakePurchases",
     "()Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IapBeginPurchase",
     "(Ljava/lang/String;Ljava/lang/String;)Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IapConsumePurchase",
     "(Ljava/lang/String;)Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IapQueryExistingPurchases",
     "()Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IapQueryInAppPurchases",
     "([Ljava/lang/String;)Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IapRestorePurchases",
     "([Ljava/lang/String;[Z)Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GoogleClientConnect",
     "()V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GoogleClientDisconnect",
     "()V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_ResetAchievements",
     "()V", klj_GA_void_noop},
    // The Ads module. Signatures are the ones the guest actually interns, read
    // off a boot log rather than from the engine source. Every
    // predicate says no, so nothing proceeds to a show; the advertising id is
    // the empty string, never NULL, because UE4 wraps it in an FString unchecked.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_ShowAdBanner",
     "(Ljava/lang/String;Z)V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_HideAdBanner", "()V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_CloseAdBanner", "()V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_LoadInterstitialAd",
     "(Ljava/lang/String;)V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IsInterstitialAdAvailable",
     "()Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_IsInterstitialAdRequested",
     "()Z", klj_GA_false},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_ShowInterstitialAd",
     "()V", klj_GA_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_GetAdvertisingId",
     "()Ljava/lang/String;", klj_GA_empty_string},
    {"com/epicgames/ue4/MessageBox01", "<init>",     "()V", klj_MsgBox_new},
    {"com/epicgames/ue4/MessageBox01", "setCaption", "(Ljava/lang/String;)V", klj_MsgBox_setCaption},
    {"com/epicgames/ue4/MessageBox01", "setText",    "(Ljava/lang/String;)V", klj_MsgBox_setText},
    {"com/epicgames/ue4/MessageBox01", "addButton",  "(Ljava/lang/String;)V", klj_MsgBox_addButton},
    {"com/epicgames/ue4/MessageBox01", "clear",      "()V", klj_MsgBox_clear},
    {"com/epicgames/ue4/MessageBox01", "show",       "()I", klj_MsgBox_show},
    {0}
};
