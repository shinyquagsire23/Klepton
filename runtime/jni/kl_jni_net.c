// javax.net.ssl, the WebMessage channel, and the service lookups
// reached in the same batch
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
#include "kl_cacerts.h"
#include "kl_jni_int.h"

// ---- javax.net.ssl ----
// The guest's HTTPS stack (unitytls, under both UnityWebRequest and Mono's
// MobileAuthenticatedStream) builds its CA bundle from the Android *system*
// trust store, over this chain:
//
//   TrustManagerFactory.getInstance(getDefaultAlgorithm()).init(null)
//     .getTrustManagers()[0]        -> javax/net/ssl/X509TrustManager
//     .getAcceptedIssuers()         -> [Ljava/security/cert/X509Certificate;
//       .getEncoded()               -> [B, one DER blob per anchor
//
// All three names are in libunity.so and `checkServerTrusted` is NOT, so the
// guest takes a trust SET from us and reaches its own verdict natively. That is
// what makes answering safe: we say which roots exist, which is precisely what
// the Android call means, and unitytls still validates the whole chain.
//
// This used to answer zero trust managers, on the reasoning that the system
// store is absent and a trust-all manager would silently weaken validation.
// The second half stands — a trust-all manager is refused by the same rule as
// the DRM guard, and stays refused. The first half was wrong about the cost:
// unitytls reads an empty store as UNITYTLS_X509VERIFY_FLAG_NOT_TRUSTED, so
// every HTTPS request failed its chain, and VRChat's login could not complete
// ("Curl error 60 ... UnityTls error code: 7", "Connection to API Failed: SSL
// CA certificate error"). "No roots exist" is not a more conservative answer
// than the truth; it is a different false answer, and it disables TLS rather
// than hardening it.
//
// The anchors are the host's own, baked at build time — see kl_cacerts.c for
// why they cannot be read live on visionOS. KL_CA_ANCHORS=0 restores the empty
// store exactly.
static klj_val klj_TMF_getDefaultAlgorithm(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // Android's documented default since API 24 (we present SDK 29).
    return (klj_val){.l = kl_jni_new_string("PKIX")};
}
static klj_val klj_TMF_getInstance(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("TrustManagerFactory.getInstance(\"%s\")",
            n > 0 ? klj_str(a[0].l) : "");
    return (klj_val){.l = klj_new_object_data("javax/net/ssl/TrustManagerFactory", NULL)};
}
static klj_val klj_TMF_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // init(NULL) means "the system default store", which is the only store we
    // have; a non-NULL KeyStore would be the guest supplying its own, and
    // nothing in any guest here does. Nothing to configure either way.
    return (klj_val){0};
}
static klj_val klj_TMF_getTrustManagers(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    void *arr = klj_new_array('L', "javax/net/ssl/TrustManager", 1);
    ((void **)klj_arr(arr)->data)[0] =
        klj_new_object_data("javax/net/ssl/X509TrustManager", NULL);
    return (klj_val){.l = arr};
}

// The anchors themselves. Each certificate object carries its index into the
// table as its payload, biased by one so it is never the NULL that
// klj_new_object_data uses for "no payload".
static klj_val klj_X509TM_getAcceptedIssuers(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    int   count = kl_cacert_count();
    void *arr   = klj_new_array('L', "java/security/cert/X509Certificate", count);
    void **slot = klj_arr(arr)->data;
    for (int i = 0; i < count; i++)
        slot[i] = klj_new_object_data("java/security/cert/X509Certificate",
                                      (void *)(intptr_t)(i + 1));
    KLJ_LOG("X509TrustManager.getAcceptedIssuers() -> %d anchor(s)", count);
    return (klj_val){.l = arr};
}

// Certificate.getEncoded() -> the DER. Bound against the BASE class, because
// that is where Java declares it and where libunity looks it up (see g_supers).
// Built fresh on every call rather than cached: the guest copies the bytes out
// and drops the array, and a cached one would be retired by the guest's own
// correct DeleteLocalRef.
static klj_val klj_X509Cert_getEncoded(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    klj_object *o = klj_as_object(self);
    int         i = o ? (int)(intptr_t)o->data - 1 : -1;
    size_t      len = 0;
    const unsigned char *der = kl_cacert_at(i, &len);
    if (!der)
        KLJ_LOG("X509Certificate.getEncoded(): no anchor %d — returning empty", i);
    void *out = klj_new_array('B', NULL, (int)len);
    if (der && len) memcpy(klj_arr(out)->data, der, len);
    return (klj_val){.l = out};
}


// Window.getAttributes returns the live LayoutParams — the same object every
// time, because on Android the caller mutates it and hands it back through
// setAttributes. One instance is what makes that round trip mean anything.
static klj_val klj_Window_getAttributes(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *lp;
    return klj_singleton("android/view/WindowManager$LayoutParams", &lp);
}

// Resources and Window are identities: Unity holds them to reach the display
// metrics and the window flags, both of which we answer from KLJ_DISPLAY_*.
static klj_val klj_Context_getResources(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *resources;
    return klj_singleton("android/content/res/Resources", &resources);
}
static klj_val klj_Activity_getWindow(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *window;
    return klj_singleton("android/view/Window", &window);
}

// setRequestedOrientation is recorded, not applied — there is no window manager
// to rotate anything. It is worth naming in the log because it says which way
// round the engine believes the screen is, which is the first thing to check if
// the render target ever comes out transposed.
static klj_val klj_Activity_setRequestedOrientation(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    static const char *names[] = {
        "LANDSCAPE", "PORTRAIT", "USER", "BEHIND", "SENSOR", "NOSENSOR",
        "SENSOR_LANDSCAPE", "SENSOR_PORTRAIT", "REVERSE_LANDSCAPE",
        "REVERSE_PORTRAIT", "FULL_SENSOR", "USER_LANDSCAPE", "USER_PORTRAIT",
        "FULL_USER", "LOCKED",
    };
    int32_t o = n > 0 ? (int32_t)a[0].j : -1;
    const char *name = (o >= 0 && o < (int32_t)(sizeof names / sizeof names[0]))
                       ? names[o] : (o == -1 ? "UNSPECIFIED" : "?");
    KLJ_LOG("Activity.setRequestedOrientation(%d /* %s */) — recorded, not applied", o, name);
    return (klj_val){0};
}

// ---- odds and ends the same batch reached for ----
void *klb_dlopen(const char *path, int flags);   // kl_dl.c — the guest's own dlopen
void *klb_dlsym(void *handle, const char *name); // ...and its dlsym

// System.loadLibrary()/System.load() must call JNI_OnLoad if the library
// exports one. That is Android's contract and not an optimisation: a plugin's
// entire JNI setup lives there, and skipping it leaves the library loaded,
// resolvable, and UNINITIALISED — every cached class, method id and flag still
// at its static initial value.
//
// Nothing needed it until Beat Saber 1.40. libmain and libunity are driven
// explicitly by the driver (m_boot), and 1.28's only runtime plugin was
// libOVRPlugin, which we replace outright. 1.40 loads libOculusXRPlugin.so from
// managed code, and that library caches the answer to
// `OculusUnity.getIsOnOculusHardware()` into a static byte during JNI_OnLoad —
// GetIsOnOculusHardware() is three instructions that read it back. With
// JNI_OnLoad never called the byte stays zero, the Oculus XR Plugin loader
// reads "this is not an Oculus device" and calls Application.Quit(), printing
// a message about how the .apk was built. Nothing in it points at a load step.
//
// Once per image: klb_dlopen returns the existing handle for an already-open
// library (libil2cpp arrives twice over), and JNI_OnLoad is not idempotent.
static void klj_run_jni_onload(void *handle, const char *what) {
    if (!handle) return;
    static void *done[32];
    static unsigned done_n;
    for (unsigned i = 0; i < done_n; i++) if (done[i] == handle) return;
    typedef int (*onload_fn)(void *vm, void *reserved);
    onload_fn onload = (onload_fn)klb_dlsym(handle, "JNI_OnLoad");
    if (!onload) return;             // most libraries have none; that is normal
    if (done_n < sizeof done / sizeof *done) done[done_n++] = handle;
    int v = onload(kl_jni_vm(), NULL);
    KLJ_LOG("%s: JNI_OnLoad -> 0x%08x", what, (unsigned)v);
}

// UnityPlayer.loadLibrary(name) is Unity's System.loadLibrary wrapper. Routing it
// through the guest dlopen rather than kl_load keeps one image registry: the
// library may already be open (libil2cpp arrives this way *and* through
// ClassLoader.findLibrary), and klb_dlopen returns the existing image for that.
klj_val klj_UnityPlayer_loadLibrary(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *name = n > 0 ? klj_str(a[0].l) : NULL;
    if (!name || !*name) return (klj_val){.j = 0};
    char   path[1024];
    size_t len = strlen(name);
    if (strncmp(name, "lib", 3) == 0 && len > 3 && strcmp(name + len - 3, ".so") == 0)
        snprintf(path, sizeof path, "%s/%s", g_native_lib_dir, name);
    else
        snprintf(path, sizeof path, "%s/lib%s.so", g_native_lib_dir, name);
    void *h = klb_dlopen(path, 0x00002 /* RTLD_NOW */);
    KLJ_LOG("UnityPlayer.loadLibrary(\"%s\") -> %s", name, h ? "loaded" : "failed");
    klj_run_jni_onload(h, name);
    return (klj_val){.j = h != NULL};
}

// System.load(path) is the absolute-path form of loadLibrary — OVRPlugin's C# side
// calls it with whatever ClassLoader.findLibrary returned. Routed through the same
// guest dlopen as UnityPlayer.loadLibrary so there is still one image registry;
// the library is usually already open by the time this runs, and klb_dlopen
// returns the existing image rather than loading it twice.
//
// Returns void, so a failure here cannot be reported to the guest as a value. Real
// Android would throw UnsatisfiedLinkError; we log instead, because the guest goes
// on to dlsym the library and that path already names what it could not resolve.
static klj_val klj_System_load(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *path = n > 0 ? klj_str(a[0].l) : NULL;
    if (!path || !*path) return (klj_val){.j = 0};
    void *h = klb_dlopen(path, 0x00002 /* RTLD_NOW */);
    KLJ_LOG("System.load(\"%s\") -> %s", path, h ? "loaded" : "failed");
    klj_run_jni_onload(h, path);
    return (klj_val){.j = 0};
}

// The output devices AudioManager knows about. Answered as an empty array, which
// is the one honest answer available: we do not enumerate host audio hardware, and
// every field of an AudioDeviceInfo — id, type, sample rates, channel masks —
// would have to be invented to fill even one entry.
//
// Empty is not a silent zero of the trap-6d kind. Unity uses this list to notice
// device *changes* (headphones, Bluetooth) and to pick a non-default output; with
// nothing to choose from it stays on the default path, which is the OpenSL device
// that already works. If something later needs a populated list, the trace will
// name the AudioDeviceInfo getter it calls rather than leaving us to guess.
static klj_val klj_AudioManager_getDevices(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("AudioManager.getDevices() -> empty (host devices are not enumerated)");
    return (klj_val){.l = klj_new_array('L', "android/media/AudioDeviceInfo", 0)};
}

// MediaRouter.getSelectedRoute(type). Android always returns a route here — the
// default one if nothing else is selected — so null would be the invented answer,
// not the conservative one. The RouteInfo we hand back has no presentation display
// attached, which is exactly the ordinary case for a device with nothing plugged
// into it, and is what sends Unity down its normal single-display path.
static klj_val klj_MediaRouter_getSelectedRoute(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *route;
    if (!route) route = kl_jni_new_object("android/media/MediaRouter$RouteInfo");
    return (klj_val){.l = route};
}

// Settings.Secure.getString(resolver, "android_id") is what Unity turns into
// SystemInfo.deviceUniqueIdentifier. It has to be a real string: a null here is
// not a harmless blank, it is the guest aborting inside its own string handling —
// which is exactly how this presented under KL_PERMISSIVE, as a SIGABRT with no
// JNI call named because the permissive zero *was* the answer.
//
// The value is synthetic and constant: Android's own android_id is a per-device,
// per-app-signing-key random 64-bit value, so any 16-hex-digit string is a
// well-formed one. Stable across runs because callers cache it and compare.
#define KLJ_ANDROID_ID "4b6c6570746f6e01"      /* "Klepton\1" as hex */

static klj_val klj_Context_getContentResolver(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static void *resolver;
    if (!resolver) resolver = kl_jni_new_object("android/content/ContentResolver");
    return (klj_val){.l = resolver};
}

static klj_val klj_Settings_Secure_getString(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *key = n > 1 ? klj_str(a[1].l) : NULL;
    if (key && strcmp(key, "android_id") == 0)
        return (klj_val){.l = kl_jni_new_string(KLJ_ANDROID_ID)};
    // Android returns null for a setting that is not present, and callers are
    // written for it. Naming it keeps an unexpected key from looking like a value.
    KLJ_LOG("Settings.Secure.getString(\"%s\") -> null (not a setting we serve)",
            key ? key : "(null)");
    return (klj_val){.l = NULL};
}

// Thread.start(). This one is deliberately not a plain no-op, because a no-op
// here is the one shape that lies: if the thread carried a guest Runnable, doing
// nothing means its run() never happens and the guest waits forever for work that
// was never started — a hang with no cause anywhere near it.
//
// What we can serve is the case where the "thread" is one of ours: a HandlerThread
// whose work drains through kl_jni_drain_ui_tasks anyway, so there is genuinely
// nothing to start. So the receiver's class is logged every time, and anything
// that is not a HandlerThread is called out as unhandled rather than swallowed.
static klj_val klj_Thread_start(void *env, void *self, const klj_val *a, int n) {
    klj_object *o = klj_as_object(self);
    // Reached through the superclass walk when the guest looks start() up on
    // java/lang/Thread but the receiver is really a HandlerThread — which does
    // have something to start, and the first version of this wrongly said it did
    // not. That produced a guest blocked forever on a message no thread would
    // ever deliver.
    if (o && strcmp(o->cls, "android/os/HandlerThread") == 0)
        return klj_HandlerThread_start(env, self, a, n);

    (void)a; (void)n;
    KLJ_LOG("Thread.start() on %s — NOT started. If this carried a Runnable, its "
            "run() will never execute; that would show up as a wait with no cause. "
            "Implement it here if so.", o ? o->cls : "(not one of our objects)");
    return (klj_val){.j = 0};
}

// Thread.currentThread() / Thread.setName(String) — UE4 resolves the pair in
// one block and uses them for exactly one thing: naming its own threads. The
// engine spawns its game, render and audio threads with pthread_create and then
// calls up into Java to label them, because on Android that is what shows up in
// a trace.
//
// The object is per-thread and it MUST be, because that is the whole meaning of
// the call: a single shared "the current thread" object would let one thread's
// setName land on another's. It is a plain klj_object with no state of its own
// — the identity is what carries the meaning, and the only thing ever done with
// it here is setName below.
static klj_val klj_Thread_currentThread(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    static _Thread_local void *me;
    if (!me) me = klj_new_object_data("java/lang/Thread", NULL);
    return (klj_val){.l = me};
}

// ...and the name is APPLIED, not recorded. pthread_setname_np on Darwin names
// the CALLING thread, which is the same thread Java's setName would be naming
// here, so the labels reach `sample` and the debugger — and a 172 MB engine
// with a dozen threads is exactly the case where a stack of "Thread 7" is the
// difference between a readable fault report and an unreadable one.
//
// Darwin caps the name at 64 bytes including the terminator and fails the whole
// call if it is longer, where Java simply keeps a long string. Truncating is
// the behaviour that preserves the guest's intent; refusing would drop the
// label entirely for the threads with the most descriptive names.
static klj_val klj_Thread_setName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    const char *name = (n >= 1) ? klj_str(a[0].l) : NULL;
    if (name && *name) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s", name);
        pthread_setname_np(buf);
    }
    return (klj_val){.j = 0};
}

// A void method whose effect is on state we do not model. Shared, but only ever
// bound to methods that genuinely return void — the arguments are ignored, so
// binding it to something with a return value would hand the guest a zero it
// would read as an answer.
static klj_val klj_void_noop(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

// The engine calls the JAVA wrapper UnityPlayer.hidePreservedContent()V; on
// Android it is a one-line forward to the registered native
// nativeHidePreservedContent, which the guest DID register (RegisterNatives
// list, UnityPlayer.nativeHidePreservedContent()V). So the faithful dispatch
// is to call that native, not silence — hiding the preserved frame is the
// guest's decision and it has an implementation for exactly this. If the guest
// never registered it, silence (nothing is preserved to hide anyway).
static klj_val klj_UnityPlayer_hidePreservedContent(void *env, void *self,
                                                    const klj_val *a, int n) {
    (void)a; (void)n;
    void *fn = kl_jni_native("com/unity3d/player/UnityPlayer",
                             "nativeHidePreservedContent", NULL);
    if (fn)
        ((void (*)(void *, void *))fn)(env, self);
    return (klj_val){.j = 0};
}

const klj_binding klj_bind_net[] = {
    {"com/vrchat/android/plugin/Info", "setContext", "(Landroid/content/Context;)V", klj_void_noop},
    {"com/vrchat/android/plugin/Info", "init", "()V", klj_void_noop},
    {"android/media/AudioManager", "setStreamMute", "(IZ)V", klj_void_noop},
    {"android/media/AudioManager", "setMicrophoneMute", "(Z)V", klj_void_noop},

    {"javax/net/ssl/TrustManagerFactory", "getDefaultAlgorithm", "()Ljava/lang/String;", klj_TMF_getDefaultAlgorithm},
    {"javax/net/ssl/TrustManagerFactory", "getInstance", "(Ljava/lang/String;)Ljavax/net/ssl/TrustManagerFactory;", klj_TMF_getInstance},
    {"javax/net/ssl/TrustManagerFactory", "init", "(Ljava/security/KeyStore;)V", klj_TMF_init},
    {"javax/net/ssl/TrustManagerFactory", "getTrustManagers", "()[Ljavax/net/ssl/TrustManager;", klj_TMF_getTrustManagers},
    {"javax/net/ssl/X509TrustManager", "getAcceptedIssuers", "()[Ljava/security/cert/X509Certificate;", klj_X509TM_getAcceptedIssuers},
    {"java/security/cert/Certificate", "getEncoded", "()[B", klj_X509Cert_getEncoded},
    {"android/app/Activity",   "getWindow", "()Landroid/view/Window;",     klj_Activity_getWindow},
    {"android/app/Activity",   "setRequestedOrientation", "(I)V", klj_Activity_setRequestedOrientation},
    {"android/view/Window",  "getAttributes",
     "()Landroid/view/WindowManager$LayoutParams;", klj_Window_getAttributes},
    // getResources is declared on Context and reached through the theme wrapper
    // in between; the guest resolves the id against whichever it named.
    {"android/view/ContextThemeWrapper", "getResources",
     "()Landroid/content/res/Resources;", klj_Context_getResources},
    {"android/content/Context", "getResources",
     "()Landroid/content/res/Resources;", klj_Context_getResources},
    {"java/lang/System", "load", "(Ljava/lang/String;)V", klj_System_load},
    {"com/unity3d/player/UnityPlayer", "hidePreservedContent", "()V", klj_UnityPlayer_hidePreservedContent},
    {"android/content/Context", "getContentResolver", "()Landroid/content/ContentResolver;", klj_Context_getContentResolver},
    {"android/provider/Settings$Secure", "getString", "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;", klj_Settings_Secure_getString},
    {"android/media/AudioManager", "getDevices", "(I)[Landroid/media/AudioDeviceInfo;", klj_AudioManager_getDevices},
    {"android/media/MediaRouter", "getSelectedRoute", "(I)Landroid/media/MediaRouter$RouteInfo;", klj_MediaRouter_getSelectedRoute},
    {"java/lang/Thread", "start", "()V", klj_Thread_start},
    // UE4 names its own threads through Java — see klj_Thread_currentThread.
    {"java/lang/Thread", "currentThread", "()Ljava/lang/Thread;", klj_Thread_currentThread},
    {"java/lang/Thread", "setName", "(Ljava/lang/String;)V", klj_Thread_setName},
    {"android/webkit/WebView", "setBackgroundColor", "(I)V", klj_void_noop},
    {"android/webkit/WebView", "setLayerType", "(ILandroid/graphics/Paint;)V", klj_void_noop},
    {"android/webkit/WebView", "measure", "(II)V", klj_void_noop},
    {"android/webkit/WebView", "setMeasuredDimension", "(II)V", klj_void_noop},
    {"android/webkit/WebView", "layout", "(IIII)V", klj_void_noop},
    {"android/webkit/WebView", "onPause",  "()V", klj_void_noop},
    {"android/webkit/WebView", "onResume", "()V", klj_void_noop},
    {"android/webkit/WebView", "setWebChromeClient", "(Landroid/webkit/WebChromeClient;)V", klj_void_noop},
    // The WebSettings surface, in full: these are every `set*` name the guest
    // binary carries that belongs to this class, and all of them are void
    // setters on a browser that is not here.
    {"android/webkit/WebSettings", "setJavaScriptEnabled", "(Z)V", klj_void_noop},
    {"android/webkit/WebSettings", "setUseWideViewPort", "(Z)V", klj_void_noop},
    {"android/webkit/WebSettings", "setLoadWithOverviewMode", "(Z)V", klj_void_noop},
    {"android/webkit/WebSettings", "setLoadsImagesAutomatically", "(Z)V", klj_void_noop},
    {"android/webkit/WebSettings", "setMediaPlaybackRequiresUserGesture", "(Z)V", klj_void_noop},
    {"android/webkit/WebView", "setVisibility", "(I)V", klj_void_noop},
    {"android/graphics/Bitmap", "copyPixelsToBuffer", "(Ljava/nio/Buffer;)V", klj_void_noop},
    {"android/graphics/Canvas", "drawColor", "(ILandroid/graphics/PorterDuff$Mode;)V", klj_void_noop},
    {"android/graphics/Canvas", "scale", "(FFFF)V", klj_void_noop},
    {"android/widget/RelativeLayout", "setLayerType", "(ILandroid/graphics/Paint;)V", klj_void_noop},
    {"android/widget/RelativeLayout", "measure", "(II)V", klj_void_noop},
    {"android/widget/RelativeLayout", "layout", "(IIII)V", klj_void_noop},
    {"android/widget/RelativeLayout", "draw", "(Landroid/graphics/Canvas;)V", klj_void_noop},
    {"android/widget/RelativeLayout", "addView",
     "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V", klj_void_noop},
    {"android/app/Activity", "setContentView", "(Landroid/view/View;)V", klj_void_noop},
    // Shows a dialog the engine asked to be DEFERRED earlier. The Java's whole
    // body is `if (pendingDialog != NONE) runOnUiThread(show)`, and nothing
    // here ever sets a pending dialog, so the guest's own no-op arm is the
    // honest answer rather than a stub standing in for one.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_ShowHiddenAlertDialog",
     "()V", klj_void_noop},
    // Device quirks and screen policy the host has no equivalent of. Each one is
    // a Java body that either sets a flag nothing here reads or posts a window
    // flag to a window we own, so the guest's own no-op arm is the answer.
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_UseSurfaceViewWorkaround", "()V", klj_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_KeepScreenOn", "(Z)V", klj_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_SetSustainedPerformanceMode", "(Z)V", klj_void_noop},
    {"com/epicgames/ue4/GameActivity", "AndroidThunkJava_DismissSplashScreen", "()V", klj_void_noop},
    {0}
};
